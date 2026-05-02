/* ============================================================================
 * registry.c -- Windows-Compatible Registry System: Core Data Structures
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
#include "kernel/fs/vfs.h"
#include "kernel/klog.h"
#include "kernel/boot_info.h"
#include "kernel/smbios.h"
#include "kernel/uefi_runtime.h"

/* ---- String helpers ---- */

static uint32_t reg_strlen(const char *s)
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

/* Forward declaration (defined in.2) */
void registry_mark_dirty(reg_key_t *key);

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

/* Forward declare -- defined in value type helpers section below */
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

    /* HKLM\SOFTWARE\Classes -- the primary HKCR backing store */
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
 * Note: this does NOT use RegOpenKeyEx (not yet implemented) -- it walks
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
                /* Found %VARNAME% -- extract and look up */
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
                        /* Variable not found -- keep original %VARNAME% */
                        const char *orig = p;
                        while (orig <= end && di < dst_size - 1)
                            dst[di++] = *orig++;
                    }
                }
                p = end + 1;
            } else {
                /* No closing % -- copy literal */
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
 * Win32-Compatible Key Operations
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

int RegIsPredefinedKey(HKEY hKey)
{
    return reg_is_predefined(hKey);
}

/* ---- Resolve HKEY to reg_key_t* ---- */

/* Handles predefined sentinels (including HKCU/HKCR redirection)
 * and user-allocated handles. */
static reg_key_t *reg_resolve_key(HKEY hkey)
{
    reg_key_t *k;

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

    /* User-allocated handle: reject tombstoned (deleted/unloaded) keys.
     * RegDeleteKeyDirect and RegUnloadHive set name[0]='\0' to mark the
     * slot freed; returning such a key would allow ghost writes. */
    k = hkey->key;
    if (!k || k->name[0] == '\0')
        return (reg_key_t *)0;
    return k;
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

#include "kernel/timer.h"

static uint64_t reg_get_uptime_ticks(void)
{
    return system_get_ticks();
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
        target->parent->last_write_time = reg_get_uptime_ticks();

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

/* ---- RegDeleteKeyDirect ---- */

long RegDeleteKeyDirect(reg_key_t *key)
{
    if (!key)
        return ERROR_INVALID_PARAMETER;

    /* Cannot delete key with children */
    if (key->child_count > 0)
        return ERROR_ACCESS_DENIED;

    /* Cannot delete a root key (no parent) */
    if (!key->parent)
        return ERROR_ACCESS_DENIED;

    /* Free values and unlink from parent */
    reg_free_values(key);
    reg_remove_child(key->parent, key);

    /* Mark key slot as freed */
    key->name[0] = '\0';
    key->flags = 0;

    return ERROR_SUCCESS;
}

/* ---- RegRenameKey ---- */

long RegRenameKey(reg_key_t *key, const char *new_name)
{
    reg_key_t *parent;

    if (!key || !new_name || new_name[0] == '\0')
        return ERROR_INVALID_PARAMETER;

    parent = key->parent;
    if (!parent)
        return ERROR_ACCESS_DENIED;  /* cannot rename root */

    /* Validate name length */
    {
        uint32_t len = 0;
        while (new_name[len]) len++;
        if (len > REG_MAX_KEY_NAME)
            return ERROR_INVALID_PARAMETER;
    }

    /* Reject if a DIFFERENT sibling with new_name already exists.
     * Renaming to the same name (including case-variant) is a no-op. */
    {
        reg_key_t *existing = reg_find_child(parent, new_name);
        if (existing && existing != key)
            return ERROR_ACCESS_DENIED;  /* collision with different sibling */
        if (existing == key) {
            /* Same key: update name in place for case-only changes, mark
             * dirty, and return without touching hash buckets. */
            reg_strcpy(key->name, new_name, REG_MAX_KEY_NAME + 1);
            parent->last_write_time = reg_get_uptime_ticks();
            key->last_write_time = parent->last_write_time;
            registry_mark_dirty(key);
            return ERROR_SUCCESS;
        }
    }

    /* Unlink from current hash bucket (based on old name) */
    reg_remove_child(parent, key);

    /* Update name */
    reg_strcpy(key->name, new_name, REG_MAX_KEY_NAME + 1);

    /* Re-link into hash bucket (based on new name) */
    reg_add_child(parent, key);

    /* Timestamps + dirty flag */
    parent->last_write_time = reg_get_uptime_ticks();
    key->last_write_time = parent->last_write_time;
    registry_mark_dirty(key);

    return ERROR_SUCCESS;
}

/* ---- RegUnloadHive ----
 *
 * Recursively unload a previously-loaded hive subtree.  Frees all children,
 * their values, and the key itself.  Used by NtUnloadKey. */

/* Forward-declare the subtree deleter (defined below) */
static void reg_delete_subtree(reg_key_t *key);

long RegUnloadHive(reg_key_t *key)
{
    uint32_t b;

    if (!key)
        return ERROR_INVALID_PARAMETER;
    if (!key->parent)
        return ERROR_ACCESS_DENIED;  /* cannot unload a root */

    /* Delete all children recursively */
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

    /* Unlink from parent */
    reg_remove_child(key->parent, key);

    /* Mark slot freed */
    key->name[0] = '\0';
    key->flags = 0;

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

    /* Unlink from parent FIRST while target->name still hashes to the
     * original bucket; reg_delete_subtree() clears name[0]='\0' as
     * part of marking the key freed, after which reg_remove_child
     * would compute the empty-string bucket instead of the original
     * name's bucket and silently fail to unlink (Codex audit on the
     * ESRT registry mirror feature: leaked the ESRT key on every
     * populate-after-init cycle, monotonic key pool exhausts on
     * repeated mirror refreshes). */
    reg_remove_child(target->parent, target);
    reg_delete_subtree(target);

    return ERROR_SUCCESS;
}

/* ============================================================================
 * Win32-Compatible Value Operations
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
    key->last_write_time = reg_get_uptime_ticks();

    /* Mark hive dirty for periodic flush */
    registry_mark_dirty(key);

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
        /* No size pointer but data pointer -- copy what we can */
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
            key->last_write_time = reg_get_uptime_ticks();
            registry_mark_dirty(key);
            return ERROR_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    return ERROR_FILE_NOT_FOUND;
}

/* ============================================================================
 * Win32-Compatible Enumeration
 * ============================================================================ */

/* ---- Helper: get child key by 0-based index ---- */
/* Iterates through all hash buckets linearly. O(n) but children are few. */
static reg_key_t *reg_get_child_by_index(reg_key_t *key, uint32_t index)
{
    uint32_t cur = 0;
    uint32_t b;

    if (!key) return (reg_key_t *)0;

    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = key->children[b];
        while (c) {
            if (cur == index)
                return c;
            cur++;
            c = c->hash_next;
        }
    }
    return (reg_key_t *)0;
}

/* ---- Helper: get value by 0-based index ---- */
static reg_value_t *reg_get_value_by_index(reg_key_t *key, uint32_t index)
{
    uint32_t cur = 0;
    reg_value_t *v;

    if (!key) return (reg_value_t *)0;

    v = key->values;
    while (v) {
        if (cur == index)
            return v;
        cur++;
        v = v->next;
    }
    return (reg_value_t *)0;
}

/* ---- RegEnumKeyEx ---- */

long RegEnumKeyEx(HKEY hKey, uint32_t dwIndex, char *lpName,
                  uint32_t *lpcchName, uint32_t *lpReserved,
                  char *lpClass, uint32_t *lpcchClass,
                  uint64_t *lpftLastWriteTime)
{
    reg_key_t *key, *child;
    uint32_t name_len;

    (void)lpReserved;
    (void)lpClass;
    (void)lpcchClass;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    child = reg_get_child_by_index(key, dwIndex);
    if (!child)
        return ERROR_NO_MORE_ITEMS;

    name_len = reg_strlen(child->name);

    if (lpName && lpcchName) {
        if (*lpcchName <= name_len) {
            *lpcchName = name_len + 1;
            return ERROR_MORE_DATA;
        }
        reg_strcpy(lpName, child->name, *lpcchName);
        *lpcchName = name_len;
    }

    if (lpftLastWriteTime)
        *lpftLastWriteTime = child->last_write_time;

    return ERROR_SUCCESS;
}

/* ---- RegEnumValue ---- */

long RegEnumValue(HKEY hKey, uint32_t dwIndex, char *lpValueName,
                  uint32_t *lpcchValueName, uint32_t *lpReserved,
                  uint32_t *lpType, uint8_t *lpData, uint32_t *lpcbData)
{
    reg_key_t *key;
    reg_value_t *v;
    uint32_t name_len;

    (void)lpReserved;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    v = reg_get_value_by_index(key, dwIndex);
    if (!v)
        return ERROR_NO_MORE_ITEMS;

    /* Value name */
    name_len = reg_strlen(v->name);
    if (lpValueName && lpcchValueName) {
        if (*lpcchValueName <= name_len) {
            *lpcchValueName = name_len + 1;
            return ERROR_MORE_DATA;
        }
        reg_strcpy(lpValueName, v->name, *lpcchValueName);
        *lpcchValueName = name_len;
    }

    /* Type */
    if (lpType)
        *lpType = v->type;

    /* Data */
    if (lpcbData) {
        if (lpData) {
            if (*lpcbData < v->data_size) {
                *lpcbData = v->data_size;
                return ERROR_MORE_DATA;
            }
            reg_memcpy(lpData, v->data, v->data_size);
        }
        *lpcbData = v->data_size;
    }

    return ERROR_SUCCESS;
}

/* ---- RegQueryInfoKey ---- */

long RegQueryInfoKey(HKEY hKey, char *lpClass, uint32_t *lpcchClass,
                     uint32_t *lpReserved, uint32_t *lpcSubKeys,
                     uint32_t *lpcbMaxSubKeyLen,
                     uint32_t *lpcbMaxClassLen,
                     uint32_t *lpcValues, uint32_t *lpcbMaxValueNameLen,
                     uint32_t *lpcbMaxValueLen, uint32_t *lpcbSecurityDescriptor,
                     uint64_t *lpftLastWriteTime)
{
    reg_key_t *key;
    uint32_t b;

    (void)lpClass;
    (void)lpcchClass;
    (void)lpReserved;
    (void)lpcbMaxClassLen;
    (void)lpcbSecurityDescriptor;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    if (lpcSubKeys)
        *lpcSubKeys = key->child_count;

    if (lpcValues)
        *lpcValues = key->value_count;

    if (lpftLastWriteTime)
        *lpftLastWriteTime = key->last_write_time;

    /* Compute max sub-key name length */
    if (lpcbMaxSubKeyLen) {
        uint32_t max_len = 0;
        for (b = 0; b < REG_CHILD_BUCKETS; b++) {
            reg_key_t *c = key->children[b];
            while (c) {
                uint32_t len = reg_strlen(c->name);
                if (len > max_len) max_len = len;
                c = c->hash_next;
            }
        }
        *lpcbMaxSubKeyLen = max_len;
    }

    /* Compute max value name length and max value data size */
    if (lpcbMaxValueNameLen || lpcbMaxValueLen) {
        uint32_t max_name = 0, max_data = 0;
        reg_value_t *v = key->values;
        while (v) {
            if (lpcbMaxValueNameLen) {
                uint32_t len = reg_strlen(v->name);
                if (len > max_name) max_name = len;
            }
            if (lpcbMaxValueLen) {
                if (v->data_size > max_data) max_data = v->data_size;
            }
            v = v->next;
        }
        if (lpcbMaxValueNameLen) *lpcbMaxValueNameLen = max_name;
        if (lpcbMaxValueLen)     *lpcbMaxValueLen = max_data;
    }

    return ERROR_SUCCESS;
}

/* ============================================================================
 * Typed Convenience Helpers
 * ============================================================================ */

/* ---- REG_DWORD ---- */

long RegGetDword(HKEY hKey, const char *lpValueName, uint32_t *pValue)
{
    uint32_t type = 0;
    uint32_t size = sizeof(uint32_t);
    long rc;

    if (!pValue)
        return ERROR_INVALID_PARAMETER;

    rc = RegQueryValueEx(hKey, lpValueName, (uint32_t *)0, &type,
                         (uint8_t *)pValue, &size);
    if (rc != ERROR_SUCCESS)
        return rc;
    if (type != REG_DWORD)
        return ERROR_FILE_NOT_FOUND;
    return ERROR_SUCCESS;
}

long RegSetDword(HKEY hKey, const char *lpValueName, uint32_t dwValue)
{
    return RegSetValueEx(hKey, lpValueName, 0, REG_DWORD,
                         (const uint8_t *)&dwValue, sizeof(uint32_t));
}

/* ---- REG_SZ ---- */

long RegGetString(HKEY hKey, const char *lpValueName,
                  char *lpBuf, uint32_t cbBuf)
{
    uint32_t type = 0;
    uint32_t size = cbBuf;
    long rc;

    if (!lpBuf || cbBuf == 0)
        return ERROR_INVALID_PARAMETER;

    rc = RegQueryValueEx(hKey, lpValueName, (uint32_t *)0, &type,
                         (uint8_t *)lpBuf, &size);
    if (rc != ERROR_SUCCESS)
        return rc;
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return ERROR_FILE_NOT_FOUND;
    return ERROR_SUCCESS;
}

long RegSetString(HKEY hKey, const char *lpValueName, const char *lpString)
{
    uint32_t len;

    if (!lpString)
        return ERROR_INVALID_PARAMETER;

    len = reg_strlen(lpString) + 1;  /* include null terminator */
    return RegSetValueEx(hKey, lpValueName, 0, REG_SZ,
                         (const uint8_t *)lpString, len);
}

/* ---- REG_QWORD ---- */

long RegGetQword(HKEY hKey, const char *lpValueName, uint64_t *pValue)
{
    uint32_t type = 0;
    uint32_t size = sizeof(uint64_t);
    long rc;

    if (!pValue)
        return ERROR_INVALID_PARAMETER;

    rc = RegQueryValueEx(hKey, lpValueName, (uint32_t *)0, &type,
                         (uint8_t *)pValue, &size);
    if (rc != ERROR_SUCCESS)
        return rc;
    if (type != REG_QWORD)
        return ERROR_FILE_NOT_FOUND;
    return ERROR_SUCCESS;
}

long RegSetQword(HKEY hKey, const char *lpValueName, uint64_t qwValue)
{
    return RegSetValueEx(hKey, lpValueName, 0, REG_QWORD,
                         (const uint8_t *)&qwValue, sizeof(uint64_t));
}

/* ---- One-shot read ---- */

long RegReadKeyValue(HKEY hRootKey, const char *lpPath,
                     const char *lpValueName, uint32_t *lpType,
                     uint8_t *lpData, uint32_t *lpcbData)
{
    HKEY hKey;
    long rc;

    rc = RegOpenKeyEx(hRootKey, lpPath, 0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS)
        return rc;

    rc = RegQueryValueEx(hKey, lpValueName, (uint32_t *)0, lpType,
                         lpData, lpcbData);
    RegCloseKey(hKey);
    return rc;
}

/* ============================================================================
 * Populate Factory Defaults (Win32 paths)
 * ============================================================================ */

/* CPUID helper */
static void reg_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                      uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile ("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf));
}

/* External: framebuffer dimensions */
extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);

/* External: PMM stats */
extern uint64_t pmm_get_total_frames(void);
extern uint64_t pmm_get_free_frames(void);

void registry_populate_defaults(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp;
    uint32_t count = 0;

    if (!registry_ready) return;

    /* --- HKLM\SYSTEM\Display --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Display", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "Width",  fb_get_width());
        RegSetDword(hKey, "Height", fb_get_height());
        RegSetDword(hKey, "DPI",    96);
        RegSetDword(hKey, "Scale",  100);
        RegCloseKey(hKey);
        count += 4;
    }

    /* --- HKLM\SYSTEM\Theme --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Theme", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "AccentColor",  "#0078D4");
        RegSetDword(hKey, "DarkMode",       1);
        RegSetString(hKey, "Font",         "Selawik");
        RegSetDword(hKey, "FontSize",      12);
        RegSetDword(hKey, "CornerRadius",  8);
        RegSetString(hKey, "Wallpaper",    "C:\\Impossible\\Web\\Wallpaper\\default.jpg");
        RegSetString(hKey, "WallpaperMode", "stretch");
        RegSetDword(hKey, "EnableAnimations", 1);
        RegCloseKey(hKey);
        count += 8;
    }

    /* --- HKLM\SYSTEM\Shell --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Shell", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "TaskbarHeight",    48);
        RegSetString(hKey, "TaskbarPosition", "bottom");
        RegSetDword(hKey, "ShowClock",         1);
        RegSetDword(hKey, "ShowStartButton",   1);
        RegCloseKey(hKey);
        count += 4;
    }

    /* --- HKLM\SYSTEM\Network --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Network", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "Hostname", "IMPOSSIBLE-PC");
        RegSetDword(hKey, "DHCP",      1);
        RegSetString(hKey, "DNS",      "8.8.8.8");
        RegCloseKey(hKey);
        count += 3;
    }

    /* --- HKLM\SYSTEM\DateTime --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\DateTime", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "Use24Hour",       0);
        RegSetString(hKey, "DateFormat",    "MM/DD/YYYY");
        RegSetDword(hKey, "TimezoneOffset", 0);
        RegSetString(hKey, "TimezoneName",  "UTC");
        RegSetDword(hKey, "NTPEnabled",      1);
        RegCloseKey(hKey);
        count += 5;
    }

    /* --- HKLM\SYSTEM\Recovery --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Recovery", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "AutoRestart", 30);
        RegCloseKey(hKey);
        count += 1;
    }

    /* --- HKLM\SYSTEM\Memory --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Memory", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "SwapSlots", 64);
        RegCloseKey(hKey);
        count += 1;
    }

    /* --- HKLM\HARDWARE\CPU --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\CPU", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        {
            char vendor[13];
            uint32_t eax, ebx, ecx, edx;
            reg_cpuid(0, &eax, &ebx, &ecx, &edx);
            {
                uint32_t *v32 = (uint32_t *)vendor;
                v32[0] = ebx;
                v32[1] = edx;
                v32[2] = ecx;
            }
            vendor[12] = '\0';
            RegSetString(hKey, "Vendor", vendor);
        }
        {
            char brand[49];
            uint32_t leaf, idx = 0;
            uint32_t eax, ebx, ecx, edx;
            for (leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
                reg_cpuid(leaf, &eax, &ebx, &ecx, &edx);
                {
                    uint32_t *b32 = (uint32_t *)&brand[idx];
                    b32[0] = eax; b32[1] = ebx;
                    b32[2] = ecx; b32[3] = edx;
                }
                idx += 16;
            }
            brand[48] = '\0';
            {
                const char *p = brand;
                while (*p == ' ') p++;
                RegSetString(hKey, "Model", p);
            }
        }
        RegCloseKey(hKey);
        count += 2;
    }

    /* --- HKLM\HARDWARE\Memory --- */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\Memory", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        uint64_t total_mb = (pmm_get_total_frames() * 4096) / (1024 * 1024);
        uint64_t free_mb  = (pmm_get_free_frames()  * 4096) / (1024 * 1024);
        RegSetQword(hKey, "TotalMB", total_mb);
        RegSetQword(hKey, "FreeMB",  free_mb);
        RegCloseKey(hKey);
        count += 2;
    }

    /* --- HKU\Default --- */
    if (RegCreateKeyEx(HKEY_USERS, "Default", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "HomeDir", "C:\\Users\\Default");
        RegSetString(hKey, "Shell",   "C:\\cmd.exe");
        RegCloseKey(hKey);
        count += 2;
    }

    if (RegCreateKeyEx(HKEY_USERS, "Default\\Shell", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "Prompt", "C:\\>");
        RegCloseKey(hKey);
        count += 1;
    }

    if (RegCreateKeyEx(HKEY_USERS, "Default\\Desktop", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "Wallpaper", "C:\\Impossible\\Web\\Wallpaper\\default.jpg");
        RegCloseKey(hKey);
        count += 1;
    }

    klog(LOG_DEBUG, "registry", "Registry defaults populated (%u values)",
           (uint64_t)count);

    /* Populate SMBIOS hardware keys now that the registry tree is ready */
    smbios_populate_registry();

    /* Populate Secure Boot state key (HKLM\SYSTEM\SecureBoot\State) */
    uefi_secureboot_populate_registry();

    /* Populate UEFI variable quota (HKLM\SYSTEM\SecureBoot\Vars) --
     * surfaces QueryVariableInfo() results for the Win32
     * GetFirmwareEnvironmentVariable / kernel32 facade. */
    uefi_runtime_populate_vars_registry();

    /*: Populate boot device provenance (HKLM\SYSTEM\Boot\Device\*) */
    boot_device_populate_registry();

    /* Populate firmware platform classification (HKLM\SYSTEM\Boot\Firmware\*) */
    {
        extern void firmware_platform_populate_registry(void);
        firmware_platform_populate_registry();
    }

    /* Mirror ESRT firmware inventory (HKLM\HARDWARE\Firmware\ESRT\*).
     * Idempotent: clears the subtree before writing, so removed firmware
     * components do not leak across reboots. */
    {
        extern void esrt_populate_registry(void);
        esrt_populate_registry();
    }

    /* Mirror the generic firmware-table catalog (HKLM\HARDWARE\Firmware\
     * Tables\*) per the wire format pinned at docs/boot/firmware-
     * tables-schema.md.  Idempotent like the ESRT mirror; sibling
     * subtree (different parent path), so the two mirrors do not
     * stomp each other. */
    {
        extern void firmware_tables_populate_registry(void);
        firmware_tables_populate_registry();
    }

    /* Populate boot decision record (HKLM\SYSTEM\Boot\Decision\*) */
    boot_decision_populate_registry();
}

/* ============================================================================
 * Hive File Format -- Disk Persistence
 *
 * Each root tree (SYSTEM, HARDWARE, etc.) is stored as a separate .hive file.
 * Format: [4096-byte header] [key/value records in depth-first order]
 *
 * Key record:   [name_len:u16][name:N][value_count:u16][child_count:u16]
 * Value record: [name_len:u16][name:N][type:u32][data_size:u32][data:N]
 *
 * Uses PMM for the serialization buffer (can be > 4 KiB).
 * ============================================================================ */

/* External: PMM contiguous allocation */
extern uintptr_t pmm_alloc_contiguous(uint32_t num_pages);
extern void      pmm_free_frame(uintptr_t addr);

/* External: PIT ticks for timestamp */


/* VFS flags shorthand */
#define HIVE_VFS_O_READ  VFS_O_READ
#define HIVE_VFS_O_WRITE VFS_O_WRITE

/* ---- CRC32 (table-less, bit-by-bit) ---- */

static uint32_t hive_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i, j;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

/* ---- Serialization buffer helpers ---- */

typedef struct {
    uint8_t *buf;
    uint32_t pos;
    uint32_t cap;
} hive_buf_t;

static int hive_buf_write_u16(hive_buf_t *b, uint16_t v)
{
    if (b->pos + 2 > b->cap) return -1;
    b->buf[b->pos++] = (uint8_t)(v & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >> 8) & 0xFF);
    return 0;
}

static int hive_buf_write_u32(hive_buf_t *b, uint32_t v)
{
    if (b->pos + 4 > b->cap) return -1;
    b->buf[b->pos++] = (uint8_t)(v & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >>  8) & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >> 16) & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >> 24) & 0xFF);
    return 0;
}

static int hive_buf_write_bytes(hive_buf_t *b, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    if (b->pos + len > b->cap) return -1;
    for (i = 0; i < len; i++)
        b->buf[b->pos++] = data[i];
    return 0;
}

static int hive_buf_read_u16(hive_buf_t *b, uint16_t *v)
{
    if (b->pos + 2 > b->cap) return -1;
    *v = (uint16_t)b->buf[b->pos]
       | ((uint16_t)b->buf[b->pos + 1] << 8);
    b->pos += 2;
    return 0;
}

static int hive_buf_read_u32(hive_buf_t *b, uint32_t *v)
{
    if (b->pos + 4 > b->cap) return -1;
    *v = (uint32_t)b->buf[b->pos]
       | ((uint32_t)b->buf[b->pos + 1] << 8)
       | ((uint32_t)b->buf[b->pos + 2] << 16)
       | ((uint32_t)b->buf[b->pos + 3] << 24);
    b->pos += 4;
    return 0;
}

static int hive_buf_read_bytes(hive_buf_t *b, uint8_t *out, uint32_t len)
{
    uint32_t i;
    if (b->pos + len > b->cap) return -1;
    for (i = 0; i < len; i++)
        out[i] = b->buf[b->pos++];
    return 0;
}

/* ---- Depth-first serialization ---- */

/* Count total keys and values in a subtree */
static void hive_count(reg_key_t *key, uint32_t *nkeys, uint32_t *nvals)
{
    uint32_t b;
    reg_value_t *v;

    (*nkeys)++;

    for (v = key->values; v; v = v->next)
        (*nvals)++;

    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = key->children[b];
        while (c) {
            hive_count(c, nkeys, nvals);
            c = c->hash_next;
        }
    }
}

/* Serialize one key and its values, then recurse into children */
static int hive_serialize_key(hive_buf_t *buf, reg_key_t *key)
{
    uint16_t name_len = (uint16_t)reg_strlen(key->name);
    uint16_t val_count = (uint16_t)key->value_count;
    uint16_t child_count = (uint16_t)key->child_count;
    reg_value_t *v;
    uint32_t b;

    /* Write key record: [name_len][name][value_count][child_count] */
    if (hive_buf_write_u16(buf, name_len) < 0) return -1;
    if (hive_buf_write_bytes(buf, (const uint8_t *)key->name, name_len) < 0) return -1;
    if (hive_buf_write_u16(buf, val_count) < 0) return -1;
    if (hive_buf_write_u16(buf, child_count) < 0) return -1;

    /* Write values: [name_len][name][type][data_size][data] */
    for (v = key->values; v; v = v->next) {
        uint16_t vname_len = (uint16_t)reg_strlen(v->name);
        if (hive_buf_write_u16(buf, vname_len) < 0) return -1;
        if (hive_buf_write_bytes(buf, (const uint8_t *)v->name, vname_len) < 0) return -1;
        if (hive_buf_write_u32(buf, v->type) < 0) return -1;
        if (hive_buf_write_u32(buf, v->data_size) < 0) return -1;
        if (v->data_size > 0) {
            if (hive_buf_write_bytes(buf, v->data, v->data_size) < 0) return -1;
        }
    }

    /* Recurse into children (depth-first across all hash buckets) */
    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = key->children[b];
        while (c) {
            if (hive_serialize_key(buf, c) < 0) return -1;
            c = c->hash_next;
        }
    }

    return 0;
}

/* ---- String helpers for journal paths ---- */

static void hive_str_append(char *dst, uint32_t cap,
                            const char *base, const char *suffix)
{
    uint32_t i = 0, j;
    for (j = 0; base[j] && i < cap - 1; j++)
        dst[i++] = base[j];
    for (j = 0; suffix[j] && i < cap - 1; j++)
        dst[i++] = suffix[j];
    dst[i] = '\0';
}

/* ---- File copy helper (for .hive -> .hive.bak backup) ---- */

static int hive_copy_file(const char *src_path, const char *dst_path)
{
    struct vfs_node *src, *dst;
    uint8_t copy_buf[512];
    uint32_t offset = 0;
    int n;

    src = vfs_open(src_path, HIVE_VFS_O_READ);
    if (!src) return -1;

    vfs_create(dst_path, 1);
    dst = vfs_open(dst_path, HIVE_VFS_O_WRITE);
    if (!dst) { vfs_close(src); return -1; }

    while ((n = vfs_read(src, offset, sizeof(copy_buf), copy_buf)) > 0) {
        vfs_write(dst, offset, (uint32_t)n, copy_buf);
        offset += (uint32_t)n;
    }

    vfs_close(src);
    vfs_close(dst);
    return 0;
}

/* ---- Invalidate a journal file by zeroing its magic ---- */

static void hive_invalidate_log(const char *log_path)
{
    struct vfs_node *f = vfs_open(log_path, HIVE_VFS_O_WRITE);
    if (f) {
        uint32_t zero = 0;
        vfs_write(f, 0, 4, (const uint8_t *)&zero);
        vfs_close(f);
    }
}

/* ---- hive_save (journal-safe) ---- */

int hive_save(reg_key_t *root, const char *filepath)
{
    uint32_t total_keys = 0, total_values = 0;
    uint32_t buf_pages, buf_size, total_write;
    uintptr_t buf_phys;
    uint8_t *buf_ptr;
    hive_header_t *hdr;
    hive_buf_t ser;
    struct vfs_node *f;
    char log_path[160];
    char bak_path[160];

    if (!root || !filepath) return -1;
    if (!vfs_is_mounted(filepath[0])) return -1;

    /* Build journal and backup paths */
    hive_str_append(log_path, sizeof(log_path), filepath, ".log");
    hive_str_append(bak_path, sizeof(bak_path), filepath, ".bak");

    /* Count tree size */
    hive_count(root, &total_keys, &total_values);

    /* Estimate buffer: header + ~256 bytes per key + ~768 bytes per value */
    buf_size = HIVE_HEADER_SIZE + total_keys * 256 + total_values * 768;
    if (buf_size < HIVE_HEADER_SIZE + 4096)
        buf_size = HIVE_HEADER_SIZE + 4096;
    buf_pages = (buf_size + 4095) / 4096;

    /* Allocate buffer from PMM (not kmalloc -- can be large) */
    buf_phys = pmm_alloc_contiguous(buf_pages);
    if (!buf_phys) {
        klog(LOG_ERROR, "hive", "Failed to alloc %u pages for hive save", (uint64_t)buf_pages);
        return -1;
    }
    buf_ptr = (uint8_t *)buf_phys;

    /* Zero the header area */
    {
        uint32_t i;
        for (i = 0; i < HIVE_HEADER_SIZE; i++)
            buf_ptr[i] = 0;
    }

    /* Serialize key/value data after header */
    ser.buf = buf_ptr + HIVE_HEADER_SIZE;
    ser.pos = 0;
    ser.cap = buf_pages * 4096 - HIVE_HEADER_SIZE;

    if (hive_serialize_key(&ser, root) < 0) {
        klog(LOG_ERROR, "hive", "Serialization overflow for '%s'", filepath);
        goto fail_free;
    }

    /* Fill header */
    hdr = (hive_header_t *)buf_ptr;
    hdr->magic        = HIVE_MAGIC;
    hdr->version      = HIVE_VERSION;
    hdr->checksum     = 0;
    hdr->timestamp    = system_get_ticks();
    hdr->total_keys   = total_keys;
    hdr->total_values = total_values;
    hdr->data_offset  = HIVE_HEADER_SIZE;
    hdr->data_size    = ser.pos;

    /* Copy root name */
    {
        uint32_t i;
        const char *n = root->name;
        for (i = 0; i < HIVE_ROOT_NAME_MAX - 1 && n[i]; i++)
            hdr->root_name[i] = n[i];
        hdr->root_name[i] = '\0';
    }

    /* Compute CRC32 */
    hdr->checksum = hive_crc32(buf_ptr, HIVE_HEADER_SIZE);

    total_write = HIVE_HEADER_SIZE + ser.pos;

    /* === STEP 1: Write new data to .hive.log (journal) === */
    vfs_create(log_path, 1);
    f = vfs_open(log_path, HIVE_VFS_O_WRITE);
    if (!f) {
        klog(LOG_ERROR, "hive", "Cannot open journal '%s'", log_path);
        goto fail_free;
    }
    {
        int rc = vfs_write(f, 0, total_write, buf_ptr);
        vfs_close(f);
        if (rc < 0) {
            klog(LOG_ERROR, "hive", "Journal write failed for '%s'", log_path);
            goto fail_free;
        }
    }

    /* === STEP 2: Backup old .hive -> .hive.bak === */
    hive_copy_file(filepath, bak_path);

    /* === STEP 3: Atomic rename .hive.log -> .hive === */
    if (vfs_rename(log_path, filepath) != 0) {
        klog(LOG_ERROR, "hive", "Rename '%s' -> '%s' failed, falling back to copy",
             log_path, filepath);
        /* Fallback: overwrite .hive with new data (pre-vfs_rename behaviour) */
        vfs_create(filepath, 1);
        f = vfs_open(filepath, HIVE_VFS_O_WRITE);
        if (!f) {
            klog(LOG_ERROR, "hive", "Cannot open '%s' for writing", filepath);
            goto fail_free;
        }
        {
            int rc = vfs_write(f, 0, total_write, buf_ptr);
            vfs_close(f);
            if (rc < 0) {
                klog(LOG_ERROR, "hive", "Write failed for '%s'", filepath);
                goto fail_free;
            }
        }
        hive_invalidate_log(log_path);
        vfs_unlink(log_path);   /* clean up stale journal */
    }

    /* Free PMM buffer */
    {
        uint32_t p;
        for (p = 0; p < buf_pages; p++)
            pmm_free_frame(buf_phys + p * 4096);
    }

    klog(LOG_DEBUG, "hive", "Saved '%s': %u keys, %u values (%u bytes)",
         filepath, (uint64_t)total_keys, (uint64_t)total_values,
         (uint64_t)total_write);

    return 0;

fail_free:
    {
        uint32_t p;
        for (p = 0; p < buf_pages; p++)
            pmm_free_frame(buf_phys + p * 4096);
    }
    return -1;
}

/* ---- Journal recovery ---- */

/* Check if a hive file has a valid header (magic + version + CRC32) */
static int hive_validate_file(const char *path)
{
    struct vfs_node *f;
    hive_header_t hdr;
    uint32_t saved_crc, computed_crc;
    int rc;

    f = vfs_open(path, HIVE_VFS_O_READ);
    if (!f) return -1;

    rc = vfs_read(f, 0, HIVE_HEADER_SIZE, (uint8_t *)&hdr);
    vfs_close(f);
    if (rc < (int)HIVE_HEADER_SIZE) return -1;
    if (hdr.magic != HIVE_MAGIC) return -1;
    if (hdr.version != HIVE_VERSION) return -1;

    saved_crc = hdr.checksum;
    hdr.checksum = 0;
    computed_crc = hive_crc32((const uint8_t *)&hdr, HIVE_HEADER_SIZE);
    if (computed_crc != saved_crc) return -1;

    return 0;  /* valid */
}

/* Try to recover from a journal or backup file.
 * Priority: .hive.log (crash during write) -> .hive -> .hive.bak
 * Returns the best path to load from, or NULL if none are valid. */
static const char *hive_best_source(const char *filepath,
                                     char *log_path, char *bak_path)
{
    hive_str_append(log_path, 160, filepath, ".log");
    hive_str_append(bak_path, 160, filepath, ".bak");

    /* 1. Check journal -- if valid, a crash happened mid-write */
    if (hive_validate_file(log_path) == 0) {
        klog(LOG_WARN, "hive", "Recovering from journal: %s", log_path);
        /* Copy journal to main hive to complete the interrupted write */
        hive_copy_file(log_path, filepath);
        hive_invalidate_log(log_path);
        return filepath;
    }

    /* 2. Check main hive */
    if (hive_validate_file(filepath) == 0)
        return filepath;

    /* 3. Fall back to backup */
    if (hive_validate_file(bak_path) == 0) {
        klog(LOG_WARN, "hive", "Main hive corrupt, using backup: %s", bak_path);
        hive_copy_file(bak_path, filepath);
        return filepath;
    }

    /* Nothing usable */
    return (const char *)0;
}

/* ---- Deserialization ---- */

static int hive_deserialize_key(hive_buf_t *buf, reg_key_t *parent,
                                uint32_t *loaded_values)
{
    uint16_t name_len, val_count, child_count;
    char name[REG_MAX_KEY_NAME];
    reg_key_t *key;
    uint16_t vi, ci;

    /* Read key record */
    if (hive_buf_read_u16(buf, &name_len) < 0) return -1;
    if (name_len >= REG_MAX_KEY_NAME) return -1;
    if (hive_buf_read_bytes(buf, (uint8_t *)name, name_len) < 0) return -1;
    name[name_len] = '\0';
    if (hive_buf_read_u16(buf, &val_count) < 0) return -1;
    if (hive_buf_read_u16(buf, &child_count) < 0) return -1;

    /* Create or find the key under parent */
    key = reg_find_child(parent, name);
    if (!key)
        key = reg_create_child(parent, name);
    if (!key) return -1;

    /* Read values */
    for (vi = 0; vi < val_count; vi++) {
        uint16_t vname_len;
        char vname[REG_MAX_VALUE_NAME];
        uint32_t vtype, vdata_size;

        if (hive_buf_read_u16(buf, &vname_len) < 0) return -1;
        if (vname_len >= REG_MAX_VALUE_NAME) return -1;
        if (hive_buf_read_bytes(buf, (uint8_t *)vname, vname_len) < 0) return -1;
        vname[vname_len] = '\0';
        if (hive_buf_read_u32(buf, &vtype) < 0) return -1;
        if (hive_buf_read_u32(buf, &vdata_size) < 0) return -1;

        if (vdata_size > 0 && vdata_size <= REG_MAX_VALUE_SIZE) {
            uint8_t vdata[REG_MAX_VALUE_SIZE];
            if (hive_buf_read_bytes(buf, vdata, vdata_size) < 0) return -1;
            RegSetValueEx((HKEY)(uintptr_t)key, vname, 0, vtype,
                          vdata, vdata_size);
            (*loaded_values)++;
        } else if (vdata_size == 0) {
            RegSetValueEx((HKEY)(uintptr_t)key, vname, 0, vtype,
                          (const uint8_t *)0, 0);
            (*loaded_values)++;
        } else {
            /* Skip oversized values */
            buf->pos += vdata_size;
        }
    }

    /* Recurse into children */
    for (ci = 0; ci < child_count; ci++) {
        if (hive_deserialize_key(buf, key, loaded_values) < 0)
            return -1;
    }

    return 0;
}

/* ---- hive_load ---- */

int hive_load(const char *filepath, reg_key_t *root)
{
    struct vfs_node *f;
    hive_header_t hdr_obj;
    hive_header_t *hdr = &hdr_obj;
    uint32_t saved_crc, computed_crc;
    uint32_t buf_pages;
    uintptr_t data_phys;
    uint8_t *data_ptr;
    hive_buf_t deser;
    uint32_t loaded_values = 0;
    int rc;

    if (!filepath || !root) return -1;
    if (!vfs_is_mounted(filepath[0])) return -1;

    /* Open the hive file */
    f = vfs_open(filepath, HIVE_VFS_O_READ);
    if (!f) return -1;  /* File doesn't exist -- not an error, just no saved data */

    /* Read header */
    rc = vfs_read(f, 0, HIVE_HEADER_SIZE, (uint8_t *)hdr);
    if (rc < (int)HIVE_HEADER_SIZE) {
        klog(LOG_WARN, "hive", "Short read on '%s' header (%d bytes)", filepath, rc);
        vfs_close(f);
        return -1;
    }

    /* Validate magic */
    if (hdr->magic != HIVE_MAGIC) {
        klog(LOG_WARN, "hive", "Bad magic in '%s': 0x%x (expected REGH)", filepath,
             (uint64_t)hdr->magic);
        vfs_close(f);
        return -1;
    }

    /* Validate version */
    if (hdr->version != HIVE_VERSION) {
        klog(LOG_WARN, "hive", "Unsupported hive version %u in '%s'",
             (uint64_t)hdr->version, filepath);
        vfs_close(f);
        return -1;
    }

    /* Validate CRC32 */
    saved_crc = hdr->checksum;
    hdr->checksum = 0;
    computed_crc = hive_crc32((const uint8_t *)hdr, HIVE_HEADER_SIZE);
    if (computed_crc != saved_crc) {
        klog(LOG_WARN, "hive", "CRC32 mismatch in '%s': file=0x%x computed=0x%x",
             filepath, (uint64_t)saved_crc, (uint64_t)computed_crc);
        vfs_close(f);
        return -1;
    }

    /* Sanity check data size */
    if (hdr->data_size == 0 || hdr->data_size > 1024 * 1024) {
        klog(LOG_WARN, "hive", "Invalid data size %u in '%s'",
             (uint64_t)hdr->data_size, filepath);
        vfs_close(f);
        return -1;
    }

    /* Allocate buffer for key/value data from PMM */
    buf_pages = (hdr->data_size + 4095) / 4096;
    data_phys = pmm_alloc_contiguous(buf_pages);
    if (!data_phys) {
        klog(LOG_ERROR, "hive", "Failed to alloc %u pages for hive load", (uint64_t)buf_pages);
        vfs_close(f);
        return -1;
    }
    data_ptr = (uint8_t *)data_phys;

    /* Read key/value data */
    rc = vfs_read(f, HIVE_HEADER_SIZE, hdr->data_size, data_ptr);
    vfs_close(f);
    if (rc < (int)hdr->data_size) {
        klog(LOG_WARN, "hive", "Short data read in '%s': got %d, expected %u",
             filepath, rc, (uint64_t)hdr->data_size);
        {
            uint32_t p;
            for (p = 0; p < buf_pages; p++)
                pmm_free_frame(data_phys + p * 4096);
        }
        return -1;
    }

    /* Deserialize into the tree */
    deser.buf = data_ptr;
    deser.pos = 0;
    deser.cap = hdr->data_size;

    /* The root key itself is the first record -- but we already have the root,
     * so we deserialize as if root's children start from the file. */
    {
        uint16_t root_name_len, root_val_count, root_child_count;
        uint16_t vi, ci;

        /* Read the root key record (skip it, we already have the root) */
        if (hive_buf_read_u16(&deser, &root_name_len) < 0) goto fail;
        deser.pos += root_name_len;  /* skip name (already known) */
        if (hive_buf_read_u16(&deser, &root_val_count) < 0) goto fail;
        if (hive_buf_read_u16(&deser, &root_child_count) < 0) goto fail;

        /* Read root's values */
        for (vi = 0; vi < root_val_count; vi++) {
            uint16_t vname_len;
            char vname[REG_MAX_VALUE_NAME];
            uint32_t vtype, vdata_size;

            if (hive_buf_read_u16(&deser, &vname_len) < 0) goto fail;
            if (vname_len >= REG_MAX_VALUE_NAME) goto fail;
            if (hive_buf_read_bytes(&deser, (uint8_t *)vname, vname_len) < 0) goto fail;
            vname[vname_len] = '\0';
            if (hive_buf_read_u32(&deser, &vtype) < 0) goto fail;
            if (hive_buf_read_u32(&deser, &vdata_size) < 0) goto fail;

            if (vdata_size > 0 && vdata_size <= REG_MAX_VALUE_SIZE) {
                uint8_t vdata[REG_MAX_VALUE_SIZE];
                if (hive_buf_read_bytes(&deser, vdata, vdata_size) < 0) goto fail;
                RegSetValueEx((HKEY)(uintptr_t)root, vname, 0, vtype,
                              vdata, vdata_size);
                loaded_values++;
            } else if (vdata_size == 0) {
                RegSetValueEx((HKEY)(uintptr_t)root, vname, 0, vtype,
                              (const uint8_t *)0, 0);
                loaded_values++;
            } else {
                deser.pos += vdata_size;
            }
        }

        /* Deserialize children */
        for (ci = 0; ci < root_child_count; ci++) {
            if (hive_deserialize_key(&deser, root, &loaded_values) < 0)
                goto fail;
        }
    }

    /* Free PMM buffer */
    {
        uint32_t p;
        for (p = 0; p < buf_pages; p++)
            pmm_free_frame(data_phys + p * 4096);
    }

    klog(LOG_DEBUG, "hive", "Loaded '%s': %u keys, %u values",
         filepath, (uint64_t)hdr->total_keys, (uint64_t)loaded_values);

    return (int)loaded_values;

fail:
    klog(LOG_WARN, "hive", "Corrupt data in '%s' at offset %u",
         filepath, (uint64_t)deser.pos);
    {
        uint32_t p;
        for (p = 0; p < buf_pages; p++)
            pmm_free_frame(data_phys + p * 4096);
    }
    return -1;
}

/* ============================================================================
 * Hive File Disk Layout
 *
 * Each HKLM sub-tree and HKU\Default gets its own .hive file under
 * C:\Impossible\System\Config\Registry\
 *
 * Dirty flags are set by RegSetValueEx / RegDeleteValue and cleared
 * after a successful hive_save.
 * ============================================================================ */

/* Hive descriptor: maps a root sub-key to a file path + dirty flag */
typedef struct {
    const char *path;        /* Hive file path on disk */
    reg_key_t **root_ptr;    /* Pointer to the root sub-key pointer */
    const char *sub_name;    /* Sub-key name under its parent */
    reg_key_t **parent_ptr;  /* Parent root key pointer */
    uint8_t     dirty;       /* 1 = needs flush */
} hive_desc_t;

static hive_desc_t hive_table[REG_HIVE_COUNT] = {
    { REG_HIVE_DIR "\\SYSTEM.hive",   (reg_key_t **)0, "SYSTEM",   (reg_key_t **)0, 0 },
    { REG_HIVE_DIR "\\SOFTWARE.hive", (reg_key_t **)0, "SOFTWARE", (reg_key_t **)0, 0 },
    { REG_HIVE_DIR "\\HARDWARE.hive", (reg_key_t **)0, "HARDWARE", (reg_key_t **)0, 0 },
    { REG_HIVE_DIR "\\DEFAULT.hive",  (reg_key_t **)0, "Default",  (reg_key_t **)0, 0 },
};

/* Late init: wire up hive_table pointers after registry_init() */
static uint8_t hive_table_inited = 0;

static void hive_table_init(void)
{
    if (hive_table_inited) return;

    /* HKLM sub-keys */
    hive_table[0].parent_ptr = &reg_root_hklm;
    hive_table[1].parent_ptr = &reg_root_hklm;
    hive_table[2].parent_ptr = &reg_root_hklm;
    /* HKU sub-key */
    hive_table[3].parent_ptr = &reg_root_hku;

    hive_table_inited = 1;
}

/* Find the sub-key pointer for a hive descriptor */
static reg_key_t *hive_get_subkey(uint32_t idx)
{
    if (!hive_table[idx].parent_ptr || !*hive_table[idx].parent_ptr)
        return (reg_key_t *)0;
    return reg_find_child(*hive_table[idx].parent_ptr, hive_table[idx].sub_name);
}

/* ---- Dirty-flag tracking ---- */

/* Mark the hive containing a key as dirty.
 * Walks up the parent chain to find which root sub-key the key belongs to. */
void registry_mark_dirty(reg_key_t *key)
{
    reg_key_t *cur = key;
    uint32_t i;

    if (!hive_table_inited) return;

    /* Walk to depth-1 child of a root key */
    while (cur && cur->parent && cur->parent->parent)
        cur = cur->parent;

    /* cur is now a direct child of a root key -- match it to a hive */
    for (i = 0; i < REG_HIVE_COUNT; i++) {
        reg_key_t *sub = hive_get_subkey(i);
        if (sub && sub == cur) {
            hive_table[i].dirty = 1;
            return;
        }
    }
}

/* ---- Directory creation helper ---- */

static void hive_ensure_dir(void)
{
    /* Create each path component if needed */
    vfs_create("C:\\Impossible", 0);                          /* directory */
    vfs_create("C:\\Impossible\\System", 0);
    vfs_create("C:\\Impossible\\System\\Config", 0);
    vfs_create("C:\\Impossible\\System\\Config\\Registry", 0);
}

/* ---- registry_flush / registry_flush_checked ---- */

int registry_flush_checked(void)
{
    uint32_t i;
    int failures = 0;

    if (!registry_ready || !hive_table_inited)
        return -1;
    if (!vfs_is_mounted('C'))
        return -1;

    for (i = 0; i < REG_HIVE_COUNT; i++) {
        if (!hive_table[i].dirty) continue;

        reg_key_t *sub = hive_get_subkey(i);
        if (!sub) continue;

        if (hive_save(sub, hive_table[i].path) == 0) {
            hive_table[i].dirty = 0;
            klog(LOG_DEBUG, "registry", "Flushed hive: %s", hive_table[i].path);
        } else {
            failures++;
            klog(LOG_ERROR, "registry", "Flush FAILED for hive: %s",
                 hive_table[i].path);
        }
    }
    return failures;  /* 0 = all clean, >0 = count of failed hives */
}

void registry_flush(void)
{
    uint32_t i;

    if (!registry_ready || !hive_table_inited) return;
    if (!vfs_is_mounted('C')) return;

    for (i = 0; i < REG_HIVE_COUNT; i++) {
        if (!hive_table[i].dirty) continue;

        reg_key_t *sub = hive_get_subkey(i);
        if (!sub) continue;

        if (hive_save(sub, hive_table[i].path) == 0) {
            hive_table[i].dirty = 0;
            klog(LOG_DEBUG, "registry", "Flushed hive: %s", hive_table[i].path);
        }
    }
}

/* ---- registry_save_all ---- */

void registry_save_all(void)
{
    uint32_t i;

    if (!registry_ready) return;
    if (!vfs_is_mounted('C')) return;

    hive_table_init();
    hive_ensure_dir();

    for (i = 0; i < REG_HIVE_COUNT; i++) {
        reg_key_t *sub = hive_get_subkey(i);
        if (!sub) continue;

        if (hive_save(sub, hive_table[i].path) == 0) {
            hive_table[i].dirty = 0;
        }
    }

    klog(LOG_DEBUG, "registry", "All hives saved to disk");
}

/* ---- registry_load_hives ---- */

void registry_load_hives(void)
{
    uint32_t i;
    uint32_t loaded = 0;

    if (!registry_ready) return;
    if (!vfs_is_mounted('C')) return;

    hive_table_init();
    hive_ensure_dir();

    for (i = 0; i < REG_HIVE_COUNT; i++) {
        reg_key_t *sub = hive_get_subkey(i);
        const char *src;
        char log_path[160], bak_path[160];
        int rc;

        if (!sub) continue;

        /* Journal recovery: find best valid source */
        src = hive_best_source(hive_table[i].path, log_path, bak_path);
        if (!src) continue;  /* No valid hive -- use defaults */

        rc = hive_load(src, sub);
        if (rc > 0) {
            loaded++;
            klog(LOG_DEBUG, "registry", "Loaded hive: %s (%d values)",
                 hive_table[i].path, rc);
        }
    }

    klog(LOG_INFO, "registry", "Hive load complete: %u/%u hives loaded",
         (uint64_t)loaded, (uint64_t)REG_HIVE_COUNT);
}
