/* ============================================================================
 * registry.c — Windows-Compatible Registry System: Core Data Structures
 *
 * Static pool allocators, FNV-1a hashing for child key lookup, and
 * predefined root key initialization.
 *
 * Memory: all nodes come from fixed-size static pools (no heap pressure).
 * This allows the registry to be usable very early in boot, before the
 * heap is fully initialized.
 *
 * Pool sizes:
 *   - 512 keys   (~166 KB with 16-bucket hash per key)
 *   - 1024 values (~785 KB with 512-byte data buffers)
 *
 * See rules.md: kmalloc is fine for these structs since they are part
 * of static arrays, not heap-allocated.
 * ============================================================================ */

#include "registry.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- String helpers ---- */

static uint32_t __attribute__((unused)) reg_strlen(const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

static void reg_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void reg_memset(void *dst, uint8_t val, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = val;
}

/* ---- Static pools ---- */

static reg_key_t    reg_key_pool[REG_KEY_POOL_SIZE];
static uint32_t     reg_key_pool_next = 0;

static reg_value_t  reg_value_pool[REG_VALUE_POOL_SIZE];
static uint32_t     reg_value_pool_next = 0;

/* ---- Root key pointers ---- */

static reg_key_t   *reg_root_hklm;    /* HKEY_LOCAL_MACHINE  */
static reg_key_t   *reg_root_hkcu;    /* HKEY_CURRENT_USER   */
static reg_key_t   *reg_root_hkcr;    /* HKEY_CLASSES_ROOT   */
static reg_key_t   *reg_root_hku;     /* HKEY_USERS          */
static reg_key_t   *reg_root_hkcc;    /* HKEY_CURRENT_CONFIG */

static uint8_t      registry_ready = 0;

/* ---- FNV-1a hash (32-bit) ---- */

/* Used for O(1) child key lookup within a parent's hash buckets.
 * Case-insensitive: registry keys are case-preserving but
 * case-insensitive (matching Windows behavior). */

static uint32_t reg_fnv1a(const char *name)
{
    uint32_t hash = 0x811C9DC5;  /* FNV offset basis */
    const uint32_t prime = 0x01000193;  /* FNV prime */
    const char *p = name;

    while (*p) {
        /* Case-insensitive: fold to lowercase */
        uint8_t c = (uint8_t)*p;
        if (c >= 'A' && c <= 'Z') c += 32;
        hash ^= c;
        hash *= prime;
        p++;
    }
    return hash;
}

/* Compute bucket index from hash */
static uint32_t reg_bucket(const char *name)
{
    return reg_fnv1a(name) & (REG_CHILD_BUCKETS - 1);
}

/* ---- Pool allocators ---- */

/* Forward declare — defined in value type helpers section below */
static int reg_stricmp(const char *a, const char *b);

static reg_key_t *reg_alloc_key(const char *name)
{
    reg_key_t *k;
    uint32_t i;

    if (reg_key_pool_next >= REG_KEY_POOL_SIZE)
        return (reg_key_t *)0;

    k = &reg_key_pool[reg_key_pool_next++];

    /* Zero the entire struct first */
    reg_memset(k, 0, sizeof(reg_key_t));

    /* Set the name */
    reg_strcpy(k->name, name, REG_MAX_KEY_NAME + 1);

    /* Initialize hash buckets to NULL */
    for (i = 0; i < REG_CHILD_BUCKETS; i++)
        k->children[i] = (reg_key_t *)0;

    k->parent         = (reg_key_t *)0;
    k->hash_next      = (reg_key_t *)0;
    k->child_count    = 0;
    k->values         = (reg_value_t *)0;
    k->value_count    = 0;
    k->last_write_time = 0;
    k->flags          = REG_FLAG_ALLOCATED;

    return k;
}

static reg_value_t *reg_alloc_value(const char *name, uint32_t type)
{
    reg_value_t *v;

    if (reg_value_pool_next >= REG_VALUE_POOL_SIZE)
        return (reg_value_t *)0;

    v = &reg_value_pool[reg_value_pool_next++];

    /* Zero the entire struct */
    reg_memset(v, 0, sizeof(reg_value_t));

    reg_strcpy(v->name, name, REG_MAX_VALUE_NAME + 1);
    v->type      = type;
    v->data_size = 0;
    v->next      = (reg_value_t *)0;

    return v;
}

/* ---- Predefined handle resolution ---- */

/* Map a predefined HKEY sentinel to its root reg_key_t pointer.
 * Returns NULL for user-allocated handles (caller must dereference). */
reg_key_t *reg_resolve_predefined(HKEY hkey)
{
    if (hkey == HKEY_LOCAL_MACHINE)  return reg_root_hklm;
    if (hkey == HKEY_CURRENT_USER)   return reg_root_hkcu;
    if (hkey == HKEY_CLASSES_ROOT)   return reg_root_hkcr;
    if (hkey == HKEY_USERS)          return reg_root_hku;
    if (hkey == HKEY_CURRENT_CONFIG) return reg_root_hkcc;
    return (reg_key_t *)0;
}

/* ---- Child key management ---- */

int reg_add_child(reg_key_t *parent, reg_key_t *child)
{
    uint32_t b;

    if (!parent || !child)
        return -1;

    b = reg_bucket(child->name);
    child->hash_next = parent->children[b];
    parent->children[b] = child;
    child->parent = parent;
    parent->child_count++;
    return 0;
}

reg_key_t *reg_find_child(reg_key_t *parent, const char *name)
{
    uint32_t b;
    reg_key_t *c;

    if (!parent || !name)
        return (reg_key_t *)0;

    b = reg_bucket(name);
    c = parent->children[b];
    while (c) {
        if (reg_stricmp(c->name, name) == 0)
            return c;
        c = c->hash_next;
    }
    return (reg_key_t *)0;
}

reg_key_t *reg_create_child(reg_key_t *parent, const char *name)
{
    reg_key_t *existing;
    reg_key_t *child;

    if (!parent || !name)
        return (reg_key_t *)0;

    /* Return existing if already present */
    existing = reg_find_child(parent, name);
    if (existing)
        return existing;

    child = reg_alloc_key(name);
    if (!child)
        return (reg_key_t *)0;

    reg_add_child(parent, child);
    return child;
}

/* ---- HKCU / HKCR redirection ---- */

static char reg_current_user[REG_MAX_KEY_NAME + 1] = "Default";

void reg_set_current_user(const char *username)
{
    if (username)
        reg_strcpy(reg_current_user, username, REG_MAX_KEY_NAME + 1);
}

reg_key_t *reg_resolve_hkcu(void)
{
    /* HKCU redirects to HKU\{current_user} */
    reg_key_t *user_key;

    if (!reg_root_hku)
        return (reg_key_t *)0;

    user_key = reg_find_child(reg_root_hku, reg_current_user);
    if (!user_key) {
        /* Auto-create the user profile key */
        user_key = reg_create_child(reg_root_hku, reg_current_user);
    }
    return user_key;
}

reg_key_t *reg_resolve_hkcr(void)
{
    reg_key_t *sw, *classes;

    if (!reg_root_hklm)
        return (reg_key_t *)0;

    /* HKCR is primarily HKLM\SOFTWARE\Classes.
     * Merged view with HKCU is handled transparently at query time. */
    sw = reg_find_child(reg_root_hklm, "SOFTWARE");
    if (!sw)
        return (reg_key_t *)0;

    classes = reg_find_child(sw, "Classes");
    return classes;
}

/* ---- Initialization ---- */

void registry_init(void)
{
    reg_key_t *sw;

    /* Zero pools */
    reg_memset(reg_key_pool, 0, sizeof(reg_key_pool));
    reg_memset(reg_value_pool, 0, sizeof(reg_value_pool));
    reg_key_pool_next = 0;
    reg_value_pool_next = 0;

    /* Create the five predefined root keys */
    reg_root_hklm = reg_alloc_key("HKEY_LOCAL_MACHINE");
    reg_root_hkcu = reg_alloc_key("HKEY_CURRENT_USER");
    reg_root_hkcr = reg_alloc_key("HKEY_CLASSES_ROOT");
    reg_root_hku  = reg_alloc_key("HKEY_USERS");
    reg_root_hkcc = reg_alloc_key("HKEY_CURRENT_CONFIG");

    if (!reg_root_hklm || !reg_root_hkcu || !reg_root_hkcr ||
        !reg_root_hku  || !reg_root_hkcc) {
        klog(LOG_ERROR, "registry", "Registry: failed to allocate root keys");
        return;
    }

    /* Set redirection flags */
    reg_root_hkcu->flags |= REG_FLAG_HKCU_REDIRECT;
    reg_root_hkcr->flags |= REG_FLAG_HKCR_MERGED;

    /* ---- Default sub-keys under HKLM ---- */
    reg_create_child(reg_root_hklm, "SYSTEM");
    sw = reg_create_child(reg_root_hklm, "SOFTWARE");
    reg_create_child(reg_root_hklm, "HARDWARE");

    /* HKLM\SOFTWARE\Classes — the primary HKCR backing store */
    if (sw)
        reg_create_child(sw, "Classes");

    /* ---- Default user profile under HKU ---- */
    reg_create_child(reg_root_hku, "Default");

    /* Initialize HKCU default user */
    reg_strcpy(reg_current_user, "Default", REG_MAX_KEY_NAME + 1);

    registry_ready = 1;

    klog(LOG_DEBUG, "registry",
         "Registry initialized (pool: %u/%u keys, %u/%u values)",
         (uint64_t)reg_key_pool_next, (uint64_t)REG_KEY_POOL_SIZE,
         (uint64_t)reg_value_pool_next, (uint64_t)REG_VALUE_POOL_SIZE);
}

/* ---- Pool statistics ---- */

uint32_t reg_keys_used(void)
{
    return reg_key_pool_next;
}

uint32_t reg_values_used(void)
{
    return reg_value_pool_next;
}

/* ============================================================================
 * Value Type Helpers
 * ============================================================================ */

/* ---- Type name for debug/display ---- */

const char *reg_type_name(uint32_t type)
{
    switch (type) {
        case REG_NONE:             return "REG_NONE";
        case REG_SZ:               return "REG_SZ";
        case REG_EXPAND_SZ:        return "REG_EXPAND_SZ";
        case REG_BINARY:           return "REG_BINARY";
        case REG_DWORD:            return "REG_DWORD";
        case REG_DWORD_BIG_ENDIAN: return "REG_DWORD_BIG_ENDIAN";
        case REG_LINK:             return "REG_LINK";
        case REG_MULTI_SZ:         return "REG_MULTI_SZ";
        case REG_QWORD:            return "REG_QWORD";
        default:                   return "REG_UNKNOWN";
    }
}

/* ---- REG_EXPAND_SZ expansion ---- */

/* Case-insensitive compare for variable names */
static int reg_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        uint8_t ca = (uint8_t)*a, cb = (uint8_t)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    uint8_t ca = (uint8_t)*a, cb = (uint8_t)*b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    return (int)ca - (int)cb;
}

/* Look up an environment variable in the registry.
 * Searches HKLM\System\Environment for a value matching 'var_name'.
 * Returns pointer to string data, or NULL if not found.
 * Note: this does NOT use RegOpenKeyEx (not yet implemented) — it walks
 * the tree directly using the root key pointers. */
static const char *reg_lookup_env_var(const char *var_name)
{
    reg_key_t *sys, *env;
    reg_value_t *v;
    uint32_t b;

    if (!reg_root_hklm || !var_name)
        return (const char *)0;

    /* Walk HKLM -> System -> Environment */
    sys = (reg_key_t *)0;
    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = reg_root_hklm->children[b];
        while (c) {
            if (reg_stricmp(c->name, "System") == 0) { sys = c; break; }
            c = c->hash_next;
        }
        if (sys) break;
    }
    if (!sys) return (const char *)0;

    env = (reg_key_t *)0;
    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = sys->children[b];
        while (c) {
            if (reg_stricmp(c->name, "Environment") == 0) { env = c; break; }
            c = c->hash_next;
        }
        if (env) break;
    }
    if (!env) return (const char *)0;

    /* Search values for matching name */
    v = env->values;
    while (v) {
        if (reg_stricmp(v->name, var_name) == 0 &&
            (v->type == REG_SZ || v->type == REG_EXPAND_SZ))
            return (const char *)v->data;
        v = v->next;
    }
    return (const char *)0;
}

uint32_t reg_expand_sz(const char *src, char *dst, uint32_t dst_size)
{
    uint32_t di = 0;
    const char *p = src;

    if (!src || !dst || dst_size == 0)
        return 0;

    while (*p && di < dst_size - 1) {
        if (*p == '%') {
            /* Extract variable name between %...% */
            const char *start = p + 1;
            const char *end = start;
            while (*end && *end != '%') end++;

            if (*end == '%' && end > start) {
                /* Found %VARNAME% — extract and look up */
                char var_name[REG_MAX_VALUE_NAME + 1];
                uint32_t vlen = (uint32_t)(end - start);
                uint32_t vi;

                if (vlen > REG_MAX_VALUE_NAME) vlen = REG_MAX_VALUE_NAME;
                for (vi = 0; vi < vlen; vi++)
                    var_name[vi] = start[vi];
                var_name[vlen] = '\0';

                {
                    const char *val = reg_lookup_env_var(var_name);
                    if (val) {
                        /* Copy expanded value */
                        while (*val && di < dst_size - 1)
                            dst[di++] = *val++;
                    } else {
                        /* Variable not found — keep original %VARNAME% */
                        const char *orig = p;
                        while (orig <= end && di < dst_size - 1)
                            dst[di++] = *orig++;
                    }
                }
                p = end + 1;
            } else {
                /* No closing % — copy literal */
                dst[di++] = *p++;
            }
        } else {
            dst[di++] = *p++;
        }
    }
    dst[di] = '\0';
    return di + 1;
}

/* ---- REG_MULTI_SZ helpers ---- */

uint32_t reg_multi_sz_count(const uint8_t *data, uint32_t data_size)
{
    uint32_t count = 0;
    uint32_t i = 0;

    if (!data || data_size == 0)
        return 0;

    while (i < data_size) {
        if (data[i] == '\0') {
            /* Check for double-null (end of MULTI_SZ) */
            if (i + 1 >= data_size || data[i + 1] == '\0')
                break;
            count++;
            i++;
        } else {
            i++;
        }
    }
    /* Count the last string (before the double-null) */
    if (i > 0 && data[0] != '\0')
        count++;
    return count;
}

uint32_t reg_multi_sz_get_idx; /* suppress -Wunused warnings via linkage */

const char *reg_multi_sz_get(const uint8_t *data, uint32_t data_size,
                              uint32_t index)
{
    uint32_t cur = 0;
    uint32_t i = 0;

    if (!data || data_size == 0)
        return (const char *)0;

    while (i < data_size) {
        if (cur == index)
            return (const char *)&data[i];

        /* Skip past current string */
        while (i < data_size && data[i] != '\0')
            i++;
        i++;  /* skip the null terminator */

        /* Double-null = end */
        if (i >= data_size || data[i] == '\0')
            break;
        cur++;
    }
    return (const char *)0;
}

uint32_t reg_multi_sz_pack(const char **strings, uint32_t count,
                            uint8_t *out, uint32_t out_size)
{
    uint32_t pos = 0;
    uint32_t i;

    if (!strings || !out || out_size == 0)
        return 0;

    for (i = 0; i < count; i++) {
        const char *s = strings[i];
        if (!s) continue;

        /* Copy string including null terminator */
        while (*s) {
            if (pos >= out_size - 2)  /* need room for final double-null */
                return 0;
            out[pos++] = (uint8_t)*s++;
        }
        if (pos >= out_size - 1)
            return 0;
        out[pos++] = '\0';  /* null terminator for this string */
    }

    /* Final null for double-null termination */
    if (pos >= out_size)
        return 0;
    out[pos++] = '\0';

    return pos;
}

/* ---- REG_LINK helpers ---- */

int reg_key_is_link(const reg_key_t *key)
{
    if (!key) return 0;
    return (key->flags & REG_FLAG_LINK) != 0;
}

const char *reg_key_get_link_target(const reg_key_t *key)
{
    reg_value_t *v;

    if (!key || !(key->flags & REG_FLAG_LINK))
        return (const char *)0;

    /* The link target is stored as the default (unnamed) value
     * with type REG_LINK */
    v = key->values;
    while (v) {
        if (v->name[0] == '\0' && v->type == REG_LINK)
            return (const char *)v->data;
        v = v->next;
    }
    return (const char *)0;
}

/* ============================================================================
 * §2.1  Win32-Compatible Key Operations
 * ============================================================================ */

/* ---- Handle pool ---- */

static reg_handle_t reg_handle_pool[REG_HANDLE_POOL_SIZE];
static uint8_t      reg_handle_used[REG_HANDLE_POOL_SIZE];

static HKEY reg_alloc_handle(reg_key_t *key, uint32_t access)
{
    uint32_t i;
    for (i = 0; i < REG_HANDLE_POOL_SIZE; i++) {
        if (!reg_handle_used[i]) {
            reg_handle_used[i] = 1;
            reg_handle_pool[i].key    = key;
            reg_handle_pool[i].access = access;
            return &reg_handle_pool[i];
        }
    }
    return (HKEY)0;
}

static void reg_free_handle(HKEY hkey)
{
    uint32_t i;
    if (!hkey) return;
    for (i = 0; i < REG_HANDLE_POOL_SIZE; i++) {
        if (&reg_handle_pool[i] == hkey) {
            reg_handle_used[i] = 0;
            reg_handle_pool[i].key    = (reg_key_t *)0;
            reg_handle_pool[i].access = 0;
            return;
        }
    }
}

/* ---- Predefined handle check ---- */

static int reg_is_predefined(HKEY hkey)
{
    uintptr_t v = (uintptr_t)hkey;
    return (v >= 0x80000000UL && v <= 0x80000005UL);
}

/* ---- Resolve HKEY to reg_key_t* ---- */

/* Handles predefined sentinels (including HKCU/HKCR redirection)
 * and user-allocated handles. */
static reg_key_t *reg_resolve_key(HKEY hkey)
{
    if (!hkey) return (reg_key_t *)0;

    if (reg_is_predefined(hkey)) {
        /* HKCU redirects to HKU\{user} */
        if (hkey == HKEY_CURRENT_USER)
            return reg_resolve_hkcu();
        /* HKCR redirects to HKLM\SOFTWARE\Classes */
        if (hkey == HKEY_CLASSES_ROOT)
            return reg_resolve_hkcr();
        return reg_resolve_predefined(hkey);
    }

    /* User-allocated handle */
    return hkey->key;
}

/* ---- Path walker ---- */

/* Walk a backslash-separated path from 'start'.
 * If 'create' is non-zero, create missing keys along the way.
 * Follows REG_LINK keys transparently.
 * Returns the final key, or NULL if not found / alloc failed. */
static reg_key_t *reg_walk_path(reg_key_t *start, const char *path, int create)
{
    reg_key_t *cur = start;
    char component[REG_MAX_KEY_NAME + 1];
    uint32_t ci;
    const char *p;

    if (!start || !path || path[0] == '\0')
        return start;

    p = path;
    if (*p == '\\') p++;  /* skip leading backslash */

    while (cur) {
        ci = 0;
        while (*p && *p != '\\' && ci < REG_MAX_KEY_NAME)
            component[ci++] = *p++;
        component[ci] = '\0';

        if (ci == 0) break;  /* trailing backslash or empty */

        /* Follow REG_LINK if present */
        if (cur->flags & REG_FLAG_LINK) {
            const char *target = reg_key_get_link_target(cur);
            if (target) {
                /* Resolve link: walk from appropriate root.
                 * For simplicity, links are absolute paths from HKLM. */
                cur = reg_walk_path(reg_root_hklm, target, 0);
                if (!cur) return (reg_key_t *)0;
            }
        }

        {
            reg_key_t *child = reg_find_child(cur, component);
            if (child) {
                cur = child;
            } else if (create) {
                child = reg_create_child(cur, component);
                if (!child) return (reg_key_t *)0;
                cur = child;
            } else {
                return (reg_key_t *)0;
            }
        }

        if (*p == '\\') p++;
    }

    /* Final REG_LINK resolution */
    if (cur && (cur->flags & REG_FLAG_LINK)) {
        const char *target = reg_key_get_link_target(cur);
        if (target)
            cur = reg_walk_path(reg_root_hklm, target, 0);
    }

    return cur;
}

/* ---- PIT ticks for timestamps ---- */

extern uint64_t pit_get_ticks(void);

static uint64_t reg_now(void)
{
    return pit_get_ticks();
}

/* ---- RegOpenKeyEx ---- */

long RegOpenKeyEx(HKEY hKey, const char *lpSubKey, uint32_t ulOptions,
                  uint32_t samDesired, HKEY *phkResult)
{
    reg_key_t *base, *target;
    HKEY handle;

    (void)ulOptions;

    if (!phkResult)
        return ERROR_INVALID_PARAMETER;

    *phkResult = (HKEY)0;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    /* NULL or empty subkey = open the base key itself */
    if (!lpSubKey || lpSubKey[0] == '\0')
        target = base;
    else
        target = reg_walk_path(base, lpSubKey, 0);

    if (!target)
        return ERROR_FILE_NOT_FOUND;

    handle = reg_alloc_handle(target, samDesired);
    if (!handle)
        return ERROR_OUTOFMEMORY;

    *phkResult = handle;
    return ERROR_SUCCESS;
}

/* ---- RegCreateKeyEx ---- */

long RegCreateKeyEx(HKEY hKey, const char *lpSubKey, uint32_t dwReserved,
                    const char *lpClass, uint32_t dwOptions,
                    uint32_t samDesired, void *lpSecurityAttributes,
                    HKEY *phkResult, uint32_t *lpdwDisposition)
{
    reg_key_t *base, *target, *pre_existing;
    HKEY handle;

    (void)dwReserved;
    (void)lpClass;
    (void)dwOptions;
    (void)lpSecurityAttributes;

    if (!phkResult)
        return ERROR_INVALID_PARAMETER;

    *phkResult = (HKEY)0;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    /* Check if already exists before creating */
    pre_existing = (!lpSubKey || lpSubKey[0] == '\0')
        ? base : reg_walk_path(base, lpSubKey, 0);

    /* Create (or find) the key */
    if (!lpSubKey || lpSubKey[0] == '\0')
        target = base;
    else
        target = reg_walk_path(base, lpSubKey, 1);

    if (!target)
        return ERROR_OUTOFMEMORY;

    /* Update parent's last-write time */
    if (target->parent)
        target->parent->last_write_time = reg_now();

    if (lpdwDisposition) {
        *lpdwDisposition = pre_existing
            ? REG_OPENED_EXISTING_KEY
            : REG_CREATED_NEW_KEY;
    }

    handle = reg_alloc_handle(target, samDesired);
    if (!handle)
        return ERROR_OUTOFMEMORY;

    *phkResult = handle;
    return ERROR_SUCCESS;
}

/* ---- RegCloseKey ---- */

long RegCloseKey(HKEY hKey)
{
    /* Predefined handles are never closed */
    if (!hKey || reg_is_predefined(hKey))
        return ERROR_SUCCESS;

    reg_free_handle(hKey);
    return ERROR_SUCCESS;
}

/* ---- Key deletion helpers ---- */

/* Remove a child key from its parent's hash buckets. */
static void reg_remove_child(reg_key_t *parent, reg_key_t *child)
{
    reg_key_t **pp;
    uint32_t b;

    if (!parent || !child) return;

    b = reg_bucket(child->name);
    pp = &parent->children[b];
    while (*pp) {
        if (*pp == child) {
            *pp = child->hash_next;
            child->hash_next = (reg_key_t *)0;
            child->parent = (reg_key_t *)0;
            parent->child_count--;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

/* Free all values from a key (marks them as unused). */
static void reg_free_values(reg_key_t *key)
{
    reg_value_t *v, *next;
    if (!key) return;

    v = key->values;
    while (v) {
        next = v->next;
        v->name[0] = '\0';
        v->next = (reg_value_t *)0;
        v = next;
    }
    key->values = (reg_value_t *)0;
    key->value_count = 0;
}

/* ---- RegDeleteKey ---- */

long RegDeleteKey(HKEY hKey, const char *lpSubKey)
{
    reg_key_t *base, *target;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    if (!lpSubKey || lpSubKey[0] == '\0')
        return ERROR_INVALID_PARAMETER;

    target = reg_walk_path(base, lpSubKey, 0);
    if (!target)
        return ERROR_FILE_NOT_FOUND;

    /* Win32 behavior: cannot delete key with child keys */
    if (target->child_count > 0)
        return ERROR_ACCESS_DENIED;

    /* Free values and unlink from parent */
    reg_free_values(target);
    reg_remove_child(target->parent, target);

    /* Mark key slot as freed */
    target->name[0] = '\0';
    target->flags = 0;

    return ERROR_SUCCESS;
}

/* ---- RegDeleteTree ---- */

/* Recursively delete all children of a key. */
static void reg_delete_subtree(reg_key_t *key)
{
    uint32_t b;

    if (!key) return;

    /* Recurse into all children via hash buckets */
    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = key->children[b];
        while (c) {
            reg_key_t *next = c->hash_next;
            reg_delete_subtree(c);
            c = next;
        }
        key->children[b] = (reg_key_t *)0;
    }
    key->child_count = 0;

    /* Free own values */
    reg_free_values(key);

    /* Mark as freed */
    key->name[0] = '\0';
    key->flags = 0;
}

long RegDeleteTree(HKEY hKey, const char *lpSubKey)
{
    reg_key_t *base, *target;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    /* If subKey is NULL, delete all children of hKey (but not hKey itself) */
    if (!lpSubKey || lpSubKey[0] == '\0') {
        uint32_t b;
        for (b = 0; b < REG_CHILD_BUCKETS; b++) {
            reg_key_t *c = base->children[b];
            while (c) {
                reg_key_t *next = c->hash_next;
                reg_delete_subtree(c);
                c = next;
            }
            base->children[b] = (reg_key_t *)0;
        }
        base->child_count = 0;
        reg_free_values(base);
        return ERROR_SUCCESS;
    }

    target = reg_walk_path(base, lpSubKey, 0);
    if (!target)
        return ERROR_FILE_NOT_FOUND;

    /* Delete entire subtree, then unlink from parent */
    reg_delete_subtree(target);
    reg_remove_child(target->parent, target);

    return ERROR_SUCCESS;
}

/* ============================================================================
 * §2.2  Win32-Compatible Value Operations
 * ============================================================================ */

/* ---- Helper: memcpy ---- */

static void reg_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* ---- Helper: find value by name in a key ---- */

static reg_value_t *reg_find_value_in_key(reg_key_t *key, const char *name)
{
    reg_value_t *v;
    if (!key) return (reg_value_t *)0;

    v = key->values;
    while (v) {
        /* Compare: empty name matches empty name (default value) */
        if ((!name || name[0] == '\0') && v->name[0] == '\0')
            return v;
        if (name && reg_stricmp(v->name, name) == 0)
            return v;
        v = v->next;
    }
    return (reg_value_t *)0;
}

/* ---- RegSetValueEx ---- */

long RegSetValueEx(HKEY hKey, const char *lpValueName, uint32_t Reserved,
                   uint32_t dwType, const uint8_t *lpData, uint32_t cbData)
{
    reg_key_t *key;
    reg_value_t *v;
    const char *vname;

    (void)Reserved;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    if (cbData > REG_MAX_VALUE_SIZE)
        return ERROR_INVALID_PARAMETER;

    /* NULL or empty = default value (stored with empty name) */
    vname = (lpValueName && lpValueName[0] != '\0') ? lpValueName : "";

    /* Find existing or allocate new */
    v = reg_find_value_in_key(key, vname);
    if (!v) {
        v = reg_alloc_value(vname, dwType);
        if (!v)
            return ERROR_OUTOFMEMORY;
        /* Add to key's value list */
        v->next = key->values;
        key->values = v;
        key->value_count++;
    }

    /* Update value */
    v->type = dwType;
    v->data_size = cbData;
    if (lpData && cbData > 0)
        reg_memcpy(v->data, lpData, cbData);

    /* Timestamp */
    key->last_write_time = reg_now();

    return ERROR_SUCCESS;
}

/* ---- RegQueryValueEx ---- */

long RegQueryValueEx(HKEY hKey, const char *lpValueName,
                     uint32_t *lpReserved,
                     uint32_t *lpType, uint8_t *lpData,
                     uint32_t *lpcbData)
{
    reg_key_t *key;
    reg_value_t *v;

    (void)lpReserved;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    v = reg_find_value_in_key(key, lpValueName);
    if (!v)
        return ERROR_FILE_NOT_FOUND;

    /* Return type if requested */
    if (lpType)
        *lpType = v->type;

    /* Return size / data */
    if (lpcbData) {
        if (!lpData) {
            /* Caller just wants the required size */
            *lpcbData = v->data_size;
            return ERROR_SUCCESS;
        }
        if (*lpcbData < v->data_size) {
            *lpcbData = v->data_size;
            return ERROR_MORE_DATA;
        }
        reg_memcpy(lpData, v->data, v->data_size);
        *lpcbData = v->data_size;
    } else if (lpData) {
        /* No size pointer but data pointer — copy what we can */
        reg_memcpy(lpData, v->data, v->data_size);
    }

    return ERROR_SUCCESS;
}

/* ---- RegGetValue ---- */

long RegGetValue(HKEY hKey, const char *lpSubKey, const char *lpValue,
                 uint32_t dwFlags, uint32_t *pdwType,
                 void *pvData, uint32_t *pcbData)
{
    reg_key_t *base;
    reg_key_t *target;
    reg_value_t *v;
    uint32_t type;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    /* Walk to sub-key if specified */
    if (lpSubKey && lpSubKey[0] != '\0')
        target = reg_walk_path(base, lpSubKey, 0);
    else
        target = base;

    if (!target)
        return ERROR_FILE_NOT_FOUND;

    v = reg_find_value_in_key(target, lpValue);
    if (!v)
        return ERROR_FILE_NOT_FOUND;

    type = v->type;

    /* Type filtering */
    if ((dwFlags & RRF_RT_ANY) != RRF_RT_ANY && dwFlags != 0) {
        int ok = 0;
        if ((dwFlags & RRF_RT_REG_SZ)        && type == REG_SZ)        ok = 1;
        if ((dwFlags & RRF_RT_REG_EXPAND_SZ) && type == REG_EXPAND_SZ) ok = 1;
        if ((dwFlags & RRF_RT_REG_BINARY)    && type == REG_BINARY)    ok = 1;
        if ((dwFlags & RRF_RT_REG_DWORD)     && type == REG_DWORD)     ok = 1;
        if ((dwFlags & RRF_RT_REG_QWORD)     && type == REG_QWORD)     ok = 1;
        /* REG_SZ flag also accepts REG_EXPAND_SZ (auto-expanded) */
        if ((dwFlags & RRF_RT_REG_SZ) && type == REG_EXPAND_SZ)        ok = 1;
        if (!ok)
            return ERROR_FILE_NOT_FOUND;
    }

    if (pdwType)
        *pdwType = type;

    /* Auto-expand REG_EXPAND_SZ unless RRF_NOEXPAND */
    if (type == REG_EXPAND_SZ && !(dwFlags & RRF_NOEXPAND) && pvData && pcbData) {
        char expanded[REG_MAX_VALUE_SIZE];
        uint32_t exp_len = reg_expand_sz((const char *)v->data, expanded,
                                          REG_MAX_VALUE_SIZE);
        if (exp_len == 0)
            return ERROR_FILE_NOT_FOUND;

        if (*pcbData < exp_len) {
            *pcbData = exp_len;
            return ERROR_MORE_DATA;
        }
        reg_memcpy(pvData, expanded, exp_len);
        *pcbData = exp_len;
        if (pdwType)
            *pdwType = REG_SZ;  /* expanded result is plain string */
        return ERROR_SUCCESS;
    }

    /* Normal copy */
    if (pcbData) {
        if (!pvData) {
            *pcbData = v->data_size;
            return ERROR_SUCCESS;
        }
        if (*pcbData < v->data_size) {
            *pcbData = v->data_size;
            return ERROR_MORE_DATA;
        }
        reg_memcpy(pvData, v->data, v->data_size);
        *pcbData = v->data_size;
    }

    return ERROR_SUCCESS;
}

/* ---- RegDeleteValue ---- */

long RegDeleteValue(HKEY hKey, const char *lpValueName)
{
    reg_key_t *key;
    reg_value_t **pp;
    const char *vname;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    vname = (lpValueName && lpValueName[0] != '\0') ? lpValueName : "";

    pp = &key->values;
    while (*pp) {
        int match;
        if (vname[0] == '\0')
            match = ((*pp)->name[0] == '\0');
        else
            match = (reg_stricmp((*pp)->name, vname) == 0);

        if (match) {
            reg_value_t *doomed = *pp;
            *pp = doomed->next;
            doomed->name[0] = '\0';
            doomed->next = (reg_value_t *)0;
            key->value_count--;
            key->last_write_time = reg_now();
            return ERROR_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    return ERROR_FILE_NOT_FOUND;
}
