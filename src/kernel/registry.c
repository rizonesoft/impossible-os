/* ============================================================================
 * registry.c -- Windows-Compatible Registry System: Core Data Structures
 *
 * Fixed-capacity pool allocators, FNV-1a hashing for child key lookup, and
 * predefined root key initialization.
 *
 * Memory: all nodes come from two fixed-capacity pools carved out of
 * contiguous physical frames and reached through their HHDM aliases (they are
 * NOT static arrays -- at ~228 KiB and ~784 KiB they are far past the 4 KiB
 * kmalloc bar, and as BSS they were the kernel image's two largest static
 * consumers). The index-bump allocator on top is unchanged: indexing the pool
 * pointer is identical to indexing the old array.
 *
 * INIT-ORDER PREREQUISITE: because the pools are frame-backed, registry_init()
 * REQUIRES a live PMM + direct map. It is called from phase 2, long after
 * pmm_init in phase 0. This header previously claimed the registry was "usable
 * very early in boot, before the heap is fully initialized" -- that is no
 * longer true, and moving registry_init earlier on the strength of it would
 * fail the boot. Before the pools exist, every entry point resolves through a
 * NULL root key and refuses; reg_alloc_key/reg_alloc_value also NULL-check.
 *
 * Pool sizes:
 *   - 512 keys   (~228 KiB with 16-bucket hash per key)
 *   - 1024 values (~784 KiB with 512-byte data buffers)
 * ============================================================================ */

#include "registry.h"
#include "kernel/mm/pmm.h"          /* frame-backed key/value pools via the HHDM */
#include "kernel/nt/nls_cp.h"
#include "kernel/nt/nls_locale.h"
#include "kernel/nt/nt_rtlstr.h"   /* rtl_upcase_char_inline -- canonical compiled fold */
#include "kernel/security/default_sds.h" /* SeCreateDefaultSD -- registry-key default DACL */
#include "kernel/security/privileges.h"  /* SeSinglePrivilegeCheck, SeBackup/SeRestorePrivilege */
#include "kernel/nt/zw.h"                 /* ssdt_previous_mode */
#include "kernel/time/wall_clock.h"      /* KeQuerySystemTime -- LastWriteTime FILETIME conversion */
#include "kernel/kchecksum.h"
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

/* ---- Key/value pools ----
 *
 * Frame-backed, reached through their HHDM aliases -- NOT static arrays. At
 * REG_KEY_POOL_SIZE keys and REG_VALUE_POOL_SIZE values these are ~228 KiB and
 * ~784 KiB, far past the 4 KiB kmalloc bar, and as BSS they were the kernel
 * image's two largest static consumers (CLAUDE.md: pmm_alloc_contiguous() for
 * anything larger than 4 KiB). Both are NULL until registry_init() allocates
 * them; every pool access is downstream of the NULL-root gate in
 * reg_resolve_predefined(), which rejects work before registry_init() has run.
 * The pool-index allocator on top is unchanged -- reg_key_pool[i] indexes a
 * pointer exactly as it indexed an array. */
static reg_key_t   *reg_key_pool;
static uint32_t     reg_key_pool_next = 0;
static uintptr_t    reg_key_pool_phys;
static uint64_t     reg_key_pool_pages;

static reg_value_t *reg_value_pool;
static uint32_t     reg_value_pool_next = 0;
static uintptr_t    reg_value_pool_phys;
static uint64_t     reg_value_pool_pages;

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
        /* Case-insensitive via the compiled invariant fold (the same authority
         * reg_stricmp + the atom/OB namespace use; ASCII + Latin-1, never the
         * disk NLS table). Insert and lookup both hash through here, so buckets
         * stay consistent; collisions are resolved by reg_stricmp. */
        uint16_t c = rtl_upcase_char_inline((uint8_t)*p);
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

    /* The pool is frame-backed now, so NULL is reachable in a way it was not
     * when this was a BSS array. Callers only get here through an already-
     * resolved non-NULL root key, which cannot exist before registry_init
     * succeeds -- but that is a reachability argument, and under the bring-up
     * identity map a missed NULL does not fault, it memsets physical page 0.
     * One compare closes it. */
    if (!reg_key_pool)
        return (reg_key_t *)0;

    if (reg_key_pool_next >= REG_KEY_POOL_SIZE)
        return (reg_key_t *)0;

    /* Soft warning at 90% pool utilization (item: total key count limit). */
    if (reg_key_pool_next == (REG_KEY_POOL_SIZE * 9) / 10)
        klog(LOG_WARN, "reg", "key pool at 90%% (%u/%u)",
             (uint64_t)reg_key_pool_next, (uint64_t)REG_KEY_POOL_SIZE);

    k = &reg_key_pool[reg_key_pool_next++];

    /* Zero the entire struct first */
    reg_memset(k, 0, sizeof(reg_key_t));

    /* Set the name */
    reg_strcpy(k->name, name, REG_MAX_KEY_NAME + 1);

    /* Default DACL: SY+BA=Full, BU=Read (static self-relative blob, NOT owned,
     * never freed).  Forward-looking storage for SeAccessCheck (TODO-15 s5);
     * tolerates NULL if the security subsystem has not initialized yet. */
    k->security_descriptor = SeCreateDefaultSD(SE_SD_TYPE_REGISTRY_KEY);

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

    /* Same NULL close as reg_alloc_key: the pool is frame-backed, and a missed
     * NULL memsets physical page 0 rather than faulting. */
    if (!reg_value_pool)
        return (reg_value_t *)0;

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
    /* A newly-created key is a NAME change on its parent.  Notifying at this
     * single new-key chokepoint (rather than once at the deepest target) means
     * multi-component creates (reg_walk_path) and copies (reg_copy_subtree)
     * notify the EXACT parent of every inserted key.  reg_dispatch_notify is a
     * no-op until watchers exist, so boot/hive-load creation stays cheap. */
    reg_dispatch_notify(parent, REG_NOTIFY_CHANGE_NAME, NULL);
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

boot_result_t registry_init(void)
{
    reg_key_t *sw;

    /* Back the key/value pools with frames reached through the HHDM. PMM is up
     * long before this (pmm_init runs in phase 0; registry_init is called from
     * phase 2). The PMM bitmap is unlocked, so this relies on the pmm.h caller
     * contract. APs ARE already up here (smp_init runs earlier in phase 2); what
     * makes it safe on the normal path is that the async storage workers joined
     * their boot_async_group barrier before this runs, so no AP is mid-allocation.
     * The one hole is a TIMED-OUT async worker, which keeps running -- that is
     * the unlocked-bitmap defect, owned by the PMM bitmap SMP-locking work, and
     * it only opens on an already-degraded boot. Never allocate a pool lazily.
     * Allocation failure is fatal: without pools there is no tree to serve, so
     * the caller branches to boot recovery on BOOT_FATAL.
     *
     * Each pool is zeroed through a LOCAL and published LAST: pmm_alloc_pages_hhdm
     * does not zero, so assigning the global first would briefly expose a pool of
     * stale frame contents. Unreachable today (nothing reaches a pool before the
     * root keys exist, and x86 is TSO), but this file is arch-neutral and the
     * planned ARM64 port has no such store ordering.
     *
     * NOTE: the byte counts are explicit. These are pointers now, so
     * sizeof(reg_key_pool) would silently collapse to the size of a pointer
     * and zero only the first 8 bytes of a 228 KiB pool. */
    const uint32_t key_bytes =
        (uint32_t)((uint64_t)REG_KEY_POOL_SIZE * sizeof(reg_key_t));
    const uint32_t value_bytes =
        (uint32_t)((uint64_t)REG_VALUE_POOL_SIZE * sizeof(reg_value_t));

    if (!reg_key_pool) {
        uintptr_t phys = 0;
        uint64_t  pages = 0;
        reg_key_t *kp = (reg_key_t *)pmm_alloc_pages_hhdm(key_bytes, &phys, &pages);
        if (!kp) {
            klog(LOG_ERROR, "registry", "Registry: failed to allocate key pool");
            return BOOT_FATAL;
        }
        reg_memset(kp, 0, key_bytes);
        reg_key_pool_phys  = phys;
        reg_key_pool_pages = pages;
        reg_key_pool = kp;
    } else {
        reg_memset(reg_key_pool, 0, key_bytes);
    }

    if (!reg_value_pool) {
        uintptr_t phys = 0;
        uint64_t  pages = 0;
        reg_value_t *vp = (reg_value_t *)pmm_alloc_pages_hhdm(value_bytes, &phys, &pages);
        if (!vp) {
            klog(LOG_ERROR, "registry", "Registry: failed to allocate value pool");
            /* Roll back the key pool completely, and clear its bookkeeping with
             * it -- leaving phys/pages set would park a double-free primitive in
             * a static for whoever adds a registry teardown path. */
            pmm_free_contiguous(reg_key_pool_phys, reg_key_pool_pages);
            reg_key_pool       = (reg_key_t *)0;
            reg_key_pool_phys  = 0;
            reg_key_pool_pages = 0;
            return BOOT_FATAL;
        }
        reg_memset(vp, 0, value_bytes);
        reg_value_pool_phys  = phys;
        reg_value_pool_pages = pages;
        reg_value_pool = vp;
    } else {
        reg_memset(reg_value_pool, 0, value_bytes);
    }

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
        /* No root keys -> registry is unusable; not a reduced-capability
         * state, so BOOT_FATAL (caller branches to boot recovery). */
        return BOOT_FATAL;
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

    return BOOT_OK;
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
    /* Case-insensitive via the compiled invariant fold (ASCII + Latin-1, never
     * the disk NLS table). Original casing is preserved in the stored key; only
     * the compare folds. */
    while (*a && *b) {
        uint16_t ca = rtl_upcase_char_inline((uint8_t)*a);
        uint16_t cb = rtl_upcase_char_inline((uint8_t)*b);
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    uint16_t ca = rtl_upcase_char_inline((uint8_t)*a);
    uint16_t cb = rtl_upcase_char_inline((uint8_t)*b);
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

/* ---- KEY_* access-rights enforcement ----
 *
 * Every public RegXxx entry point calls this before acting.  The granted mask
 * is recorded on the handle at open time (reg_alloc_handle); SeAccessCheck is
 * currently stubbed always-grant (TODO-15 s5 not yet built), so the granted
 * mask equals what the caller requested via samDesired/DesiredAccess.  Once
 * SeAccessCheck lands, the open path will narrow the granted mask against the
 * key's DACL and this per-operation check stays unchanged.
 *
 * Predefined root sentinels (HKLM/HKCU/...) carry implicit full access -- boot
 * code and kernel subsystems address them directly and must not be gated. */
long reg_check_access(HKEY hKey, uint32_t required_mask)
{
    reg_key_t *k;

    if (!hKey) return ERROR_INVALID_HANDLE;
    if (reg_is_predefined(hKey)) return ERROR_SUCCESS;

    /* User handle: reject freed/tombstoned slots (same rule as resolve). */
    k = hKey->key;
    if (!k || k->name[0] == '\0')
        return ERROR_INVALID_HANDLE;

    if ((hKey->access & required_mask) == required_mask)
        return ERROR_SUCCESS;
    return ERROR_ACCESS_DENIED;
}

/* ---- KCB (Key Control Block) LRU cache ----
 *
 * Accelerates repeated resolution of hot keys (e.g. HKLM\SYSTEM\Display opened
 * and closed in a tight loop) by remembering recently-accessed (parent, name)
 * -> child bindings, so reg_walk_path skips the per-component reg_find_child
 * hash-chain walk on a hit.  Keys live in a monotonic pool that never reissues
 * a slot and tombstones deletions via name[0]='\0', so a cached child pointer
 * is stable: a hit is validated live (child->name[0]!=0), still under the same
 * parent, and still bearing the cached name (guards against rename + delete +
 * slot reuse -- all three make the cache miss, never mis-resolve).  Populated
 * on every resolved walk hop and promoted on RegCloseKey (the hot close-then-
 * reopen pattern).  Purged from the delete/rename/unload paths so a freed or
 * renamed slot cannot even briefly serve a hit before the liveness check.
 *
 * Lock-free, consistent with the rest of registry.c; registry-wide SMP
 * synchronization (this cache included) is owned by TODO-14 s14. */
#define REG_KCB_CACHE_SIZE 32

typedef struct {
    reg_key_t *parent;                     /* immediate parent (key part 1)   */
    reg_key_t *child;                      /* cached resolved child           */
    char       name[REG_MAX_KEY_NAME + 1]; /* child name (key part 2)         */
    uint64_t   lru;                        /* last-access stamp (0 = empty)   */
} reg_kcb_entry_t;

static reg_kcb_entry_t reg_kcb_cache[REG_KCB_CACHE_SIZE];
static uint64_t        reg_kcb_clock  = 0;   /* monotonic LRU stamp source    */
static uint64_t        reg_kcb_hits   = 0;
static uint64_t        reg_kcb_misses = 0;

/* Look up (parent, name).  Returns the cached child on a validated-live hit
 * (promoted to MRU) or NULL on miss/stale.  Does NOT touch hit/miss counters --
 * the caller records them at hop granularity. */
static reg_key_t *reg_kcb_lookup(reg_key_t *parent, const char *name)
{
    uint32_t i;

    if (!parent || !name || name[0] == '\0')
        return (reg_key_t *)0;

    for (i = 0; i < REG_KCB_CACHE_SIZE; i++) {
        reg_kcb_entry_t *e = &reg_kcb_cache[i];
        if (e->lru == 0 || e->parent != parent)
            continue;
        if (reg_stricmp(e->name, name) != 0)
            continue;
        /* Validate the cached child is still the exact live key it was cached
         * as -- guards rename (name changed), delete (tombstoned), and the
         * theoretical slot-reuse the monotonic pool never actually performs. */
        if (!e->child || e->child->name[0] == '\0' ||
            e->child->parent != parent ||
            reg_stricmp(e->child->name, name) != 0) {
            e->lru = 0;             /* stale -- drop the entry */
            continue;
        }
        e->lru = ++reg_kcb_clock;   /* promote to MRU */
        return e->child;
    }
    return (reg_key_t *)0;
}

/* Insert or refresh (parent, child->name) -> child, evicting the LRU slot when
 * the cache is full.  Ignores roots (no parent) and tombstoned keys. */
static void reg_kcb_insert(reg_key_t *parent, reg_key_t *child)
{
    uint32_t i, victim;
    uint64_t oldest;

    if (!parent || !child || child->name[0] == '\0')
        return;

    /* Refresh an existing entry for this (parent, name) before allocating. */
    for (i = 0; i < REG_KCB_CACHE_SIZE; i++) {
        reg_kcb_entry_t *e = &reg_kcb_cache[i];
        if (e->lru != 0 && e->parent == parent &&
            reg_stricmp(e->name, child->name) == 0) {
            e->child = child;
            e->lru   = ++reg_kcb_clock;
            return;
        }
    }

    /* Pick an empty slot, else the least-recently-used one. */
    victim = 0;
    oldest = reg_kcb_cache[0].lru;
    for (i = 0; i < REG_KCB_CACHE_SIZE; i++) {
        if (reg_kcb_cache[i].lru == 0) { victim = i; break; }
        if (reg_kcb_cache[i].lru < oldest) { oldest = reg_kcb_cache[i].lru; victim = i; }
    }

    reg_kcb_cache[victim].parent = parent;
    reg_kcb_cache[victim].child  = child;
    reg_strcpy(reg_kcb_cache[victim].name, child->name, REG_MAX_KEY_NAME + 1);
    reg_kcb_cache[victim].lru    = ++reg_kcb_clock;
}

/* Drop every cache entry that references `key` as either parent or child.
 * Called from the delete/rename/unload paths. */
static void reg_kcb_purge_key(reg_key_t *key)
{
    uint32_t i;

    if (!key)
        return;
    for (i = 0; i < REG_KCB_CACHE_SIZE; i++) {
        reg_kcb_entry_t *e = &reg_kcb_cache[i];
        if (e->lru != 0 && (e->child == key || e->parent == key))
            e->lru = 0;
    }
}

/* Test/diagnostic accessor: current cumulative KCB hit / miss counts. */
void reg_kcb_get_stats(uint64_t *hits, uint64_t *misses)
{
    if (hits)   *hits   = reg_kcb_hits;
    if (misses) *misses = reg_kcb_misses;
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

        /* Reject an over-long component rather than truncating it and re-reading
         * the remainder as phantom sub-components (which would resolve to, or
         * delete, an unrelated existing chunked path). */
        if (*p && *p != '\\')
            return (reg_key_t *)0;

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
            /* KCB fast path: a validated cache hit skips the reg_find_child
             * hash-chain walk.  On a miss, resolve normally and populate the
             * cache so the next walk of this hop hits. */
            reg_key_t *child = reg_kcb_lookup(cur, component);
            if (child) {
                reg_kcb_hits++;
            } else {
                reg_kcb_misses++;
                child = reg_find_child(cur, component);
                if (child)
                    reg_kcb_insert(cur, child);
            }
            if (child) {
                cur = child;
            } else if (create) {
                child = reg_create_child(cur, component);
                if (!child) return (reg_key_t *)0;
                reg_kcb_insert(cur, child);
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

/* ---- Rebased monotonic ns for key/hive timestamps ---- */

#include "kernel/timer.h"

/* Nanoseconds since boot from the rebased monotonic source. NOT raw
 * system_get_ticks(), which rewinds across a KeSetTimerResolution rate change
 * (lifetime ticks reinterpreted at the new rate). */
static uint64_t reg_get_uptime_ns(void)
{
    return uptime_ns();
}

/* ---- Change-notification engine (kernel-internal callback watchers) ----
 *
 * A fixed pool of 64 watchers, each linked onto its target key's `watchers`
 * list.  reg_dispatch_notify is called from the mutation paths and walks the
 * changed key up its ancestor chain, firing every watcher whose filter matches
 * and whose subtree scope covers the change (with optional coalescing).  Lock-
 * free, consistent with the rest of registry.c; registry-wide SMP sync (this
 * engine's pool + per-key lists included) is owned by the registry
 * synchronization work (TODO-14 registry SMP locking).
 *
 * The Win32-facing event/semaphore RegNotifyChangeKeyValue + NtNotifyChangeKey
 * path (KEY_NOTIFY enforcement + referenced hEvent objects + synchronous
 * completion status) lands with the registry syscall work; this is the
 * callback engine kernel subsystems register against directly. */
#define REG_WATCHER_POOL_SIZE 64
#define REG_NOTIFY_MAX_DEPTH  4         /* re-entrancy cap: a callback that mutates
                                         * the watched key re-enters dispatch; this
                                         * bounds recursion so it cannot overflow
                                         * the kernel stack. */

static reg_watcher_t reg_watcher_pool[REG_WATCHER_POOL_SIZE];
static uint64_t      reg_watcher_next_id = 1;   /* 64-bit: never wraps/reuses; 0 invalid */
static uint32_t      reg_dispatch_depth  = 0;   /* current dispatch nesting depth */

/* Milliseconds since boot (for watcher coalescing). */
static uint64_t reg_now_ms(void)
{
    return reg_get_uptime_ns() / 1000000ULL;
}

/* True if `ancestor` is `key` or an ancestor of `key` (walk parent chain). */
int reg_is_descendant(reg_key_t *ancestor, reg_key_t *key)
{
    reg_key_t *p = key;
    if (!ancestor || !key)
        return 0;
    while (p) {
        if (p == ancestor)
            return 1;
        p = p->parent;
    }
    return 0;
}

/* Build the root-to-leaf backslash path of `key` into buf (bounded, always
 * NUL-terminated).  Returns the written length (excluding NUL).  ITERATIVE --
 * a recursive walk would consume one kernel-stack frame per level, and the tree
 * may be up to REG_MAX_KEY_DEPTH deep; the callback path must not risk a stack
 * overflow.  Fills from the back of the buffer walking the parent chain, then
 * shifts the result to the front (truncates the root-most prefix if it does not
 * fit, keeping the leaf-most portion). */
static uint32_t reg_build_key_path(reg_key_t *key, char *buf, uint32_t size)
{
    uint32_t pos, i, n;
    reg_key_t *k;

    if (!buf || size == 0)
        return 0;
    if (!key) { buf[0] = '\0'; return 0; }

    pos = size;
    buf[--pos] = '\0';                 /* reserve trailing NUL */
    for (k = key; k; k = k->parent) {
        uint32_t nlen = reg_strlen(k->name);
        if (pos < nlen)
            break;                     /* out of room -- truncate the prefix */
        pos -= nlen;
        for (i = 0; i < nlen; i++)
            buf[pos + i] = k->name[i];
        if (k->parent) {               /* separator precedes every non-root level */
            if (pos == 0)
                break;
            buf[--pos] = '\\';
        }
    }
    /* Shift the built substring (buf[pos..size-1], incl NUL) to the front. */
    n = size - pos;                    /* bytes including NUL */
    for (i = 0; i < n; i++)
        buf[i] = buf[pos + i];
    return n - 1;
}

uint64_t reg_notify_register(HKEY hKey, uint32_t filter, int watch_subtree,
                             reg_notify_fn callback, void *ctx,
                             uint64_t coalesce_ms)
{
    reg_key_t *key;
    uint32_t i;

    if (!callback || filter == 0)
        return 0;
    key = reg_resolve_key(hKey);
    if (!key)
        return 0;

    for (i = 0; i < REG_WATCHER_POOL_SIZE; i++) {
        reg_watcher_t *w = &reg_watcher_pool[i];
        if (w->active)
            continue;
        /* 64-bit monotonic id -- never reused for the practical system lifetime,
         * so the dispatch walk's slot-reuse guard (watcher_id compare) is sound. */
        w->watcher_id    = reg_watcher_next_id++;
        w->key           = key;
        w->filter        = filter;
        w->watch_subtree = watch_subtree;
        w->callback      = callback;
        w->ctx           = ctx;
        w->hit_count     = 0;
        w->coalesce_ms   = coalesce_ms;
        w->last_fired_ms = 0;
        w->active        = 1;
        /* link onto the key's watcher list (head insert) */
        w->next          = key->watchers;
        key->watchers    = w;
        return w->watcher_id;
    }
    return 0;   /* pool exhausted */
}

/* Unlink a watcher slot from its key's list (does not clear `active`). */
static void reg_watcher_unlink(reg_watcher_t *w)
{
    reg_watcher_t **pp;
    if (!w || !w->key)
        return;
    pp = &w->key->watchers;
    while (*pp) {
        if (*pp == w) { *pp = w->next; break; }
        pp = &(*pp)->next;
    }
    w->next = (reg_watcher_t *)0;
}

long RegUnregisterNotify(uint64_t watcher_id)
{
    uint32_t i;
    if (watcher_id == 0)
        return ERROR_INVALID_PARAMETER;
    for (i = 0; i < REG_WATCHER_POOL_SIZE; i++) {
        reg_watcher_t *w = &reg_watcher_pool[i];
        if (w->active && w->watcher_id == watcher_id) {
            reg_watcher_unlink(w);
            w->active = 0;
            w->key    = (reg_key_t *)0;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_FILE_NOT_FOUND;
}

void reg_notify_unregister_all(reg_key_t *key)
{
    reg_watcher_t *w;
    if (!key)
        return;
    w = key->watchers;
    while (w) {
        reg_watcher_t *next = w->next;
        w->active = 0;
        w->key    = (reg_key_t *)0;
        w->next   = (reg_watcher_t *)0;
        w = next;
    }
    key->watchers = (reg_watcher_t *)0;
}

/* Fire one watcher for a matching change (coalescing + telemetry + callback). */
static void reg_notify_fire(reg_watcher_t *w, reg_key_t *changed,
                            uint32_t change_type, const char *value_name)
{
    char path[REG_MAX_PATH];
    uint64_t now;

    if (!w->active || (w->filter & change_type) == 0)
        return;
    now = reg_now_ms();
    if (w->coalesce_ms > 0 && w->last_fired_ms != 0 &&
        (now - w->last_fired_ms) < w->coalesce_ms)
        return;   /* coalesced (deduped) */
    w->last_fired_ms = now;
    w->hit_count++;
    reg_build_key_path(changed, path, sizeof(path));
    if (w->callback)
        w->callback(path, change_type, value_name, w->ctx);
}

void reg_dispatch_notify(reg_key_t *key, uint32_t change_type,
                         const char *value_name)
{
    reg_key_t *level;
    int at_key = 1;

    if (!key || change_type == 0)
        return;

    /* Re-entrancy guard: a callback may mutate the registry and re-enter here.
     * Cap the nesting so a self-modifying callback cannot recurse the kernel
     * stack deeply (each level holds a path buffer + callback frame); depth 4 is
     * well within an 8 KiB kernel task stack.  ATOMIC inc-then-check with an
     * always-paired dec so a lost update (SMP / preemption mid-callback) can
     * never wedge the latch and silently kill all notifications registry-wide.
     * The residual (the depth budget is shared across CPUs, so a concurrent
     * dispatch on another CPU can transiently over-count) rides on the
     * registry-wide lock (TODO-14 registry SMP work), the same lock-free-until-
     * then regime as every other registry structure. */
    if (__atomic_add_fetch(&reg_dispatch_depth, 1, __ATOMIC_SEQ_CST)
            > REG_NOTIFY_MAX_DEPTH) {
        __atomic_sub_fetch(&reg_dispatch_depth, 1, __ATOMIC_SEQ_CST);
        return;
    }

    /* Walk from the changed key up its ancestor chain.  At the changed key
     * itself, fire both direct and subtree watchers; at every ancestor, fire
     * only subtree watchers (a non-subtree watcher on an ancestor does not see
     * a descendant change).  Because a watcher lives on exactly one key, a
     * subtree watcher registered on `key` fires exactly once (at at_key).
     *
     * Fire only a watcher that is still live AND still linked to THIS level:
     * a callback (fired earlier in the walk) may have unregistered a later
     * watcher and re-registered another that reused its 64-slot pool entry.
     * Re-checking w->key == level rejects a reused slot, and if the snapshotted
     * `next` was unlinked/reused its ->next would walk a foreign list, so the
     * walk stops when next is no longer part of `level`. */
    for (level = key; level; level = level->parent, at_key = 0) {
        reg_watcher_t *w = level->watchers;
        while (w) {
            reg_watcher_t *next = w->next;
            uint64_t next_id = next ? next->watcher_id : 0;
            if (w->active && w->key == level && (at_key || w->watch_subtree))
                reg_notify_fire(w, key, change_type, value_name);
            /* Advance only if `next` is STILL the exact same live watcher on this
             * level.  A callback may have unregistered `next` and re-registered a
             * replacement that reused its pool slot -- even on the SAME key (head
             * insert).  The reused slot carries a NEW monotonic watcher_id, so an
             * id mismatch (or inactive / different-key) stops the walk instead of
             * firing a wrong watcher or looping. */
            if (next && (!next->active || next->key != level ||
                         next->watcher_id != next_id))
                break;
            w = next;
        }
    }

    __atomic_sub_fetch(&reg_dispatch_depth, 1, __ATOMIC_SEQ_CST);
}

/* Convert a stored monotonic `last_write_time` (uptime_ns at mutation) to a
 * Windows FILETIME (100-ns intervals since 1601-01-01) at READ time.  Uses the
 * current wall-clock anchor minus the monotonic delta since the event; this
 * avoids the boot-order hazard where registry keys are populated before
 * wall_clock is sourced (mutation-time KeQuerySystemTime would read 0 there).
 * Returns 0 for never-written keys or before the wall clock is sourced. */
uint64_t reg_last_write_filetime(uint64_t stored_uptime_ns)
{
    uint64_t now_ns, delta_100ns;
    uint64_t now_ft;

    if (stored_uptime_ns == 0)
        return 0;
    if (!wall_clock_time_sourced())
        return 0;

    now_ft = (uint64_t)KeQuerySystemTime();
    now_ns = uptime_ns();

    /* Monotonic clock should never go backwards; clamp if it appears to. */
    if (now_ns <= stored_uptime_ns)
        return now_ft;

    delta_100ns = (now_ns - stored_uptime_ns) / 100u;
    if (delta_100ns > now_ft)   /* event predates the FILETIME epoch anchor */
        return 0;
    return now_ft - delta_100ns;
}

/* SeAccessCheck stub: grant what the caller requested.  MAXIMUM_ALLOWED maps
 * to full access.  A samDesired of 0 maps to full access as well: it is the
 * legacy "RegCreateKey without Ex" request that many kernel callers use to mean
 * "give me a working handle", and treating it as no-access would break them.
 * Non-zero restrictive masks (e.g. KEY_READ) are honored as-is, so a read-only
 * handle cannot write.  When TODO-15 s5 SeAccessCheck lands, the open path will
 * narrow the granted mask against the key DACL; the mask returned here is what
 * reg_check_access enforces per operation. */
static uint32_t reg_effective_access(uint32_t sam_desired)
{
    uint32_t out = sam_desired;

    /* Map GENERIC_* rights onto the registry-key GENERIC_MAPPING so a caller
     * requesting GENERIC_READ/WRITE/ALL gets usable specific KEY_* bits rather
     * than a zero-rights handle (KEY_EXECUTE == KEY_READ). */
    if (sam_desired & GENERIC_READ)    out |= KEY_READ;
    if (sam_desired & GENERIC_WRITE)   out |= KEY_WRITE;
    if (sam_desired & GENERIC_EXECUTE) out |= KEY_READ;
    if (sam_desired & GENERIC_ALL)     out |= KEY_ALL_ACCESS;
    out &= ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);

    /* samDesired 0 (legacy "RegCreateKey" full request) and MAXIMUM_ALLOWED
     * both map to full access while SeAccessCheck is stubbed. */
    if (out == 0 || (sam_desired & MAXIMUM_ALLOWED))
        return KEY_ALL_ACCESS;
    return out;
}

/* Anti-escalation cap: until SeAccessCheck evaluates the key DACL at open time,
 * a handle opened FROM another handle cannot gain rights the source handle did
 * not already hold, so a KEY_READ handle cannot reopen the same subtree as
 * KEY_ALL_ACCESS and then write.  Predefined roots grant full access. */
static uint32_t reg_source_access(HKEY source)
{
    if (!source || reg_is_predefined(source))
        return KEY_ALL_ACCESS;
    return source->access;
}

/* Reject a subkey path whose depth exceeds REG_MAX_KEY_DEPTH levels or whose
 * any single component exceeds REG_MAX_KEY_NAME chars.  Returns ERROR_SUCCESS
 * or ERROR_INVALID_PARAMETER (API limits: key name max, path depth max). */
static long reg_validate_path_limits(const char *path)
{
    uint32_t depth = 0, comp = 0;
    const char *p;

    if (!path) return ERROR_SUCCESS;

    for (p = path; *p; p++) {
        if (*p == '\\') {
            if (comp > 0) depth++;
            comp = 0;
            continue;
        }
        comp++;
        if (comp > REG_MAX_KEY_NAME)
            return ERROR_INVALID_PARAMETER;
    }
    if (comp > 0) depth++;
    if (depth > REG_MAX_KEY_DEPTH)
        return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

/* Count the trailing path components under `base` that do not yet exist (and so
 * would be newly created).  RegCreateKeyEx uses this to fail with
 * ERROR_OUTOFMEMORY BEFORE linking anything when the key pool cannot hold the
 * whole path, so a mid-walk allocation failure never leaves a partial path. */
static uint32_t reg_count_missing_components(reg_key_t *base, const char *path)
{
    reg_key_t *cur = base;
    char component[REG_MAX_KEY_NAME + 1];
    uint32_t ci, missing = 0;
    int creating = 0;
    const char *p;

    if (!base || !path)
        return 0;

    p = path;
    if (*p == '\\') p++;

    while (1) {
        ci = 0;
        while (*p && *p != '\\' && ci < REG_MAX_KEY_NAME)
            component[ci++] = *p++;
        component[ci] = '\0';
        if (*p && *p != '\\')     /* over-long component (validated elsewhere) */
            return missing;
        if (ci == 0)
            break;

        if (creating) {
            missing++;            /* everything past the first miss is new */
        } else {
            reg_key_t *child = reg_find_child(cur, component);
            if (child)
                cur = child;
            else { creating = 1; missing++; }
        }

        while (*p == '\\') p++;
        if (!*p) break;
    }
    return missing;
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

    {
        long lim = reg_validate_path_limits(lpSubKey);
        if (lim != ERROR_SUCCESS)
            return lim;
    }

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

    /* SeAccessCheck stub grants the requested mask, capped to the source
     * handle's own grant (anti-escalation); recorded on the handle and
     * enforced per operation by reg_check_access. */
    handle = reg_alloc_handle(target,
                              reg_effective_access(samDesired) & reg_source_access(hKey));
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
    (void)lpSecurityAttributes;

    if (!phkResult)
        return ERROR_INVALID_PARAMETER;

    *phkResult = (HKEY)0;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    /* API limits: reject over-long name components and excessive path depth. */
    {
        long lim = reg_validate_path_limits(lpSubKey);
        if (lim != ERROR_SUCCESS)
            return lim;
    }

    /* Check if already exists before creating */
    pre_existing = (!lpSubKey || lpSubKey[0] == '\0')
        ? base : reg_walk_path(base, lpSubKey, 0);

    /* Creating a NEW subkey requires KEY_CREATE_SUB_KEY on the parent handle
     * (opening an existing key does not). */
    if (!pre_existing && lpSubKey && lpSubKey[0] != '\0') {
        long acc = reg_check_access(hKey, KEY_CREATE_SUB_KEY);
        if (acc != ERROR_SUCCESS)
            return acc;

        /* Fail before linking anything if the key pool cannot hold the whole
         * path -- a mid-walk allocation failure must not leave a partial path. */
        if (reg_count_missing_components(base, lpSubKey) >
            (REG_KEY_POOL_SIZE - reg_key_pool_next))
            return ERROR_OUTOFMEMORY;
    }

    /* Atomic create-or-fail: reserve the handle slot (key = NULL) BEFORE
     * linking anything into the tree, so handle-pool exhaustion cannot leave a
     * freshly-created, handleless key behind (which would later persist via
     * hive flush).  The reserved slot is not exposed until bound below. */
    handle = reg_alloc_handle((reg_key_t *)0,
                              reg_effective_access(samDesired) & reg_source_access(hKey));
    if (!handle)
        return ERROR_OUTOFMEMORY;

    /* Create (or find) the key */
    if (!lpSubKey || lpSubKey[0] == '\0')
        target = base;
    else
        target = reg_walk_path(base, lpSubKey, 1);

    if (!target) {
        reg_free_handle(handle);
        return ERROR_OUTOFMEMORY;
    }

    /* Update parent's last-write time */
    if (target->parent)
        target->parent->last_write_time = reg_get_uptime_ns();

    if (lpdwDisposition) {
        *lpdwDisposition = pre_existing
            ? REG_OPENED_EXISTING_KEY
            : REG_CREATED_NEW_KEY;
    }

    /* REG_OPTION_VOLATILE marks a newly-created key RAM-only (excluded from
     * hive serialization); never flips an existing persisted key. */
    if (!pre_existing && (dwOptions & REG_OPTION_VOLATILE))
        target->flags |= REG_FLAG_VOLATILE;

    /* NAME notification for newly-created sub-keys is emitted by reg_create_child
     * at each inserted level (covers multi-component creates), so no explicit
     * dispatch is needed here. */

    handle->key = target;   /* bind the reserved slot to the resolved key */
    *phkResult = handle;
    return ERROR_SUCCESS;
}

/* ---- RegCloseKey ---- */

long RegCloseKey(HKEY hKey)
{
    uint32_t i;

    /* Predefined handles are never closed */
    if (!hKey || reg_is_predefined(hKey))
        return ERROR_SUCCESS;

    /* Promote the closing key into the KCB cache (close-then-reopen is the hot
     * pattern) -- but ONLY after address-validating hKey against the handle
     * pool.  reg_resolve_key would dereference hKey->key, faulting the kernel on
     * a bogus caller HKEY; the by-address scan matches reg_free_handle's own
     * check and never touches an unvalidated pointer.  reg_kcb_insert rejects a
     * tombstoned or root key. */
    for (i = 0; i < REG_HANDLE_POOL_SIZE; i++) {
        if (&reg_handle_pool[i] == hKey) {
            reg_key_t *k = reg_handle_used[i] ? reg_handle_pool[i].key : (reg_key_t *)0;
            if (k && k->parent && k->name[0] != '\0')
                reg_kcb_insert(k->parent, k);
            break;
        }
    }

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
    long acc;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    acc = reg_check_access(hKey, DELETE);
    if (acc != ERROR_SUCCESS)
        return acc;

    if (!lpSubKey || lpSubKey[0] == '\0')
        return ERROR_INVALID_PARAMETER;

    {
        long lim = reg_validate_path_limits(lpSubKey);
        if (lim != ERROR_SUCCESS)
            return lim;
    }

    target = reg_walk_path(base, lpSubKey, 0);
    if (!target)
        return ERROR_FILE_NOT_FOUND;

    /* Win32 behavior: cannot delete key with child keys */
    if (target->child_count > 0)
        return ERROR_ACCESS_DENIED;

    /* Save the parent before reg_remove_child clears target->parent. */
    {
        reg_key_t *dparent = target->parent;

        /* Free values and unlink from parent */
        reg_free_values(target);
        reg_remove_child(target->parent, target);

        /* Drop KCB + change-notification state before tombstoning the slot (same
         * invariant the other delete/unload paths maintain). */
        reg_kcb_purge_key(target);
        reg_notify_unregister_all(target);

        /* Mark key slot as freed */
        target->name[0] = '\0';
        target->flags = 0;

        /* Notify the parent's NAME watchers AFTER the tombstone so a callback
         * that refreshes state sees the key actually gone. */
        if (dparent)
            reg_dispatch_notify(dparent, REG_NOTIFY_CHANGE_NAME, NULL);
    }

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

    {
        reg_key_t *dparent = key->parent;   /* save before unlink clears it */

        /* Free values and unlink from parent */
        reg_free_values(key);
        reg_remove_child(key->parent, key);

        /* Drop KCB + change-notification state before tombstoning the slot. */
        reg_kcb_purge_key(key);
        reg_notify_unregister_all(key);

        /* Mark key slot as freed */
        key->name[0] = '\0';
        key->flags = 0;

        /* NtDeleteKey routes here -- notify the parent's NAME watchers too,
         * after the tombstone, matching RegDeleteKey/RegDeleteTree. */
        if (dparent)
            reg_dispatch_notify(dparent, REG_NOTIFY_CHANGE_NAME, NULL);
    }

    return ERROR_SUCCESS;
}

/* ---- RegRenameKeyDirect ---- */

/* True for structural keys that other subsystems resolve by fixed name/pointer
 * and therefore must never be renamed: the hive roots (SYSTEM/SOFTWARE/HARDWARE
 * under HKLM, Default under HKU -- the hive table maps them by fixed name), and
 * the HKLM\SOFTWARE\Classes backing key (reg_resolve_hkcr finds it by name, so
 * renaming it would make every HKCR operation return an invalid handle). */
static int reg_is_structural_key(reg_key_t *key)
{
    if (!key || !key->parent)
        return 0;
    if (key->parent == reg_root_hklm || key->parent == reg_root_hku)
        return 1;
    if (key == reg_resolve_hkcr())
        return 1;
    return 0;
}

long RegRenameKeyDirect(reg_key_t *key, const char *new_name)
{
    reg_key_t *parent;

    if (!key || !new_name || new_name[0] == '\0')
        return ERROR_INVALID_PARAMETER;

    parent = key->parent;
    if (!parent)
        return ERROR_ACCESS_DENIED;  /* cannot rename a predefined root */

    /* Cannot rename a structural key (hive root or the HKCR backing key): the
     * hive table + reg_resolve_hkcr resolve them by fixed name, so an in-place
     * rename would orphan the hive from save/load + dirty tracking and/or break
     * HKCR resolution. */
    if (reg_is_structural_key(key))
        return ERROR_ACCESS_DENIED;

    /* The (parent, old-name) KCB binding is about to become wrong; drop it. */
    reg_kcb_purge_key(key);

    /* Validate name: single component only (a '\\' would be stored literally but
     * read back as a path separator, making the key unreachable / undeletable by
     * its enumerated name), non-empty, within length. */
    {
        uint32_t len = 0;
        while (new_name[len]) {
            if (new_name[len] == '\\')
                return ERROR_INVALID_PARAMETER;
            len++;
        }
        if (len > REG_MAX_KEY_NAME)
            return ERROR_INVALID_PARAMETER;
    }

    /* Reject if a DIFFERENT sibling with new_name already exists.
     * Renaming to the same name (including case-variant) is a no-op. */
    {
        reg_key_t *existing = reg_find_child(parent, new_name);
        if (existing && existing != key)
            return ERROR_ALREADY_EXISTS;  /* collision with different sibling */
        if (existing == key) {
            /* Same key: update name in place for case-only changes, mark
             * dirty, and return without touching hash buckets. */
            reg_strcpy(key->name, new_name, REG_MAX_KEY_NAME + 1);
            parent->last_write_time = reg_get_uptime_ns();
            key->last_write_time = parent->last_write_time;
            registry_mark_dirty(key);
            /* A rename is a NAME change on the parent's name-set. */
            reg_dispatch_notify(parent, REG_NOTIFY_CHANGE_NAME, NULL);
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
    parent->last_write_time = reg_get_uptime_ns();
    key->last_write_time = parent->last_write_time;
    registry_mark_dirty(key);

    /* A rename is a NAME change on the parent's name-set. */
    reg_dispatch_notify(parent, REG_NOTIFY_CHANGE_NAME, NULL);

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

    {
        reg_key_t *dparent = key->parent;   /* save before unlink clears it */

        /* Unlink from parent */
        reg_remove_child(key->parent, key);

        /* Drop KCB + change-notification state before tombstoning the slot. */
        reg_kcb_purge_key(key);
        reg_notify_unregister_all(key);

        /* Mark slot freed */
        key->name[0] = '\0';
        key->flags = 0;

        /* NtUnloadKey routes here -- notify the parent's NAME watchers after the
         * tombstone, matching the other delete paths. */
        if (dparent)
            reg_dispatch_notify(dparent, REG_NOTIFY_CHANGE_NAME, NULL);
    }

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

    /* Drop KCB + change-notification state before tombstoning the slot. */
    reg_kcb_purge_key(key);
    reg_notify_unregister_all(key);

    /* Mark as freed */
    key->name[0] = '\0';
    key->flags = 0;
}

long RegDeleteTree(HKEY hKey, const char *lpSubKey)
{
    reg_key_t *base, *target;
    long acc;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;

    /* Destructive: require DELETE on the handle (a read-only handle must not
     * be able to wipe a subtree). */
    acc = reg_check_access(hKey, DELETE);
    if (acc != ERROR_SUCCESS)
        return acc;

    {
        long lim = reg_validate_path_limits(lpSubKey);
        if (lim != ERROR_SUCCESS)
            return lim;
    }

    /* If subKey is NULL, delete all children of hKey (but not hKey itself) */
    if (!lpSubKey || lpSubKey[0] == '\0') {
        uint32_t b;
        int deleted_any = 0;
        for (b = 0; b < REG_CHILD_BUCKETS; b++) {
            reg_key_t *c = base->children[b];
            while (c) {
                reg_key_t *next = c->hash_next;
                reg_delete_subtree(c);
                deleted_any = 1;
                c = next;
            }
            base->children[b] = (reg_key_t *)0;
        }
        base->child_count = 0;
        {
            int had_values = (base->value_count > 0);
            reg_free_values(base);
            /* A recursive clear is a NAME change on base (sub-keys removed) and
             * a LAST_SET change if base also held values -- notify after the
             * clear so callbacks see the emptied key. */
            if (deleted_any)
                reg_dispatch_notify(base, REG_NOTIFY_CHANGE_NAME, NULL);
            if (had_values)
                reg_dispatch_notify(base, REG_NOTIFY_CHANGE_LAST_SET, NULL);
        }
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
    {
        reg_key_t *dparent = target->parent;   /* save before unlink clears it */
        reg_remove_child(target->parent, target);
        reg_delete_subtree(target);
        /* NAME change on the parent -- dispatch AFTER the subtree is gone. */
        if (dparent)
            reg_dispatch_notify(dparent, REG_NOTIFY_CHANGE_NAME, NULL);
    }

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

/* Internal value writer that operates on an already-resolved reg_key_t*, with
 * no HKEY resolution or access check.  The hive restore path holds the real key
 * pointer, not a handle -- casting reg_key_t* to HKEY would misread the key's
 * name bytes as a reg_handle_t and (post-access-enforcement) fail the check. */
static long reg_set_value_direct(reg_key_t *key, const char *lpValueName,
                                 uint32_t dwType, const uint8_t *lpData,
                                 uint32_t cbData)
{
    reg_value_t *v;
    const char *vname;

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
        v->next = key->values;
        key->values = v;
        key->value_count++;
    }

    v->type = dwType;
    v->data_size = cbData;
    if (lpData && cbData > 0)
        reg_memcpy(v->data, lpData, cbData);

    key->last_write_time = reg_get_uptime_ns();
    registry_mark_dirty(key);
    /* Value set/modified -- notify LAST_SET watchers (covers RegSetValueEx and
     * RegCopyTree, which both funnel value writes through this chokepoint). */
    reg_dispatch_notify(key, REG_NOTIFY_CHANGE_LAST_SET, lpValueName);
    return ERROR_SUCCESS;
}

long RegSetValueEx(HKEY hKey, const char *lpValueName, uint32_t Reserved,
                   uint32_t dwType, const uint8_t *lpData, uint32_t cbData)
{
    reg_key_t *key;
    long acc;

    (void)Reserved;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    acc = reg_check_access(hKey, KEY_SET_VALUE);
    if (acc != ERROR_SUCCESS)
        return acc;

    return reg_set_value_direct(key, lpValueName, dwType, lpData, cbData);
}

/* ---- RegCopyTree ----
 *
 * Recursively copy all values + sub-keys of `src` into `dst`.  Follows the same
 * per-bucket depth-first walk as hive_serialize_key / reg_delete_subtree; uses
 * reg_set_value_direct (no per-value handle/access churn) since src/dst were
 * already access-checked at the RegCopyTree entry point. */

/* Count every descendant key + value under `src` (conservatively -- merges into
 * pre-existing dst keys/values are not discounted).  Used to reserve pool
 * capacity BEFORE mutating dst, so a mid-copy allocation failure cannot leave a
 * permanent half-copy in the monotonic (tombstone-only) key/value pools. */
static void reg_count_subtree(reg_key_t *src, uint32_t *nkeys, uint32_t *nvals)
{
    reg_value_t *v;
    uint32_t b;

    if (!src)
        return;
    for (v = src->values; v; v = v->next)
        (*nvals)++;
    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = src->children[b];
        while (c) {
            (*nkeys)++;
            reg_count_subtree(c, nkeys, nvals);
            c = c->hash_next;
        }
    }
}

static long reg_copy_subtree(reg_key_t *src, reg_key_t *dst,
                             uint32_t *nkeys, uint32_t *nvals)
{
    reg_value_t *v;
    uint32_t b;

    if (!src || !dst)
        return ERROR_INVALID_PARAMETER;

    for (v = src->values; v; v = v->next) {
        long rc = reg_set_value_direct(dst, v->name, v->type, v->data, v->data_size);
        if (rc != ERROR_SUCCESS)
            return rc;
        (*nvals)++;
    }

    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = src->children[b];
        while (c) {
            reg_key_t *dc = reg_create_child(dst, c->name);
            long rc;
            if (!dc)
                return ERROR_OUTOFMEMORY;
            dc->last_write_time = reg_get_uptime_ns();
            /* Mark each created child's hive dirty: a structure-only (value-less)
             * copy never calls reg_set_value_direct, and when dst is a bare root
             * (HKLM/HKU) the children can span several hives that a single
             * registry_mark_dirty(dst) would miss. */
            registry_mark_dirty(dc);
            (*nkeys)++;
            rc = reg_copy_subtree(c, dc, nkeys, nvals);
            if (rc != ERROR_SUCCESS)
                return rc;
            c = c->hash_next;
        }
    }
    return ERROR_SUCCESS;
}

long RegCopyTree(HKEY hKeySrc, const char *lpSubKey, HKEY hKeyDest)
{
    reg_key_t *src, *dst, *src_root;
    long acc;

    src = reg_resolve_key(hKeySrc);
    if (!src)
        return ERROR_INVALID_HANDLE;
    dst = reg_resolve_key(hKeyDest);
    if (!dst)
        return ERROR_INVALID_HANDLE;

    /* KEY_READ on source, KEY_WRITE on destination (Win32 contract). */
    acc = reg_check_access(hKeySrc, KEY_READ);
    if (acc != ERROR_SUCCESS)
        return acc;
    acc = reg_check_access(hKeyDest, KEY_WRITE);
    if (acc != ERROR_SUCCESS)
        return acc;

    if (lpSubKey && lpSubKey[0] != '\0') {
        acc = reg_validate_path_limits(lpSubKey);
        if (acc != ERROR_SUCCESS)
            return acc;
        src_root = reg_walk_path(src, lpSubKey, 0);
    } else {
        src_root = src;
    }
    if (!src_root)
        return ERROR_FILE_NOT_FOUND;

    /* Reject any lineage overlap between src_root and dst (either direction):
     *   - dst inside (or equal to) src_root: copying src into its own descendant
     *     self-amplifies (creates B\B\B...) until pool/stack exhaustion.
     *   - dst an ancestor of src_root: a source child whose name collides with an
     *     existing key on the dst->src_root path makes reg_create_child(dst,...)
     *     return a node INSIDE the source subtree, so the copy would write into
     *     the tree it is still walking (and insert not-precounted nodes).
     * With both directions rejected, reg_create_child(dst,name) can only ever
     * return a node disjoint from the source subtree. */
    {
        reg_key_t *p = dst;
        while (p) {                       /* dst descendant-or-equal of src_root */
            if (p == src_root)
                return ERROR_INVALID_PARAMETER;
            p = p->parent;
        }
    }
    {
        reg_key_t *p = src_root;
        while (p) {                       /* dst ancestor-or-equal of src_root */
            if (p == dst)
                return ERROR_INVALID_PARAMETER;
            p = p->parent;
        }
    }

    /* Reserve pool capacity before touching dst: a mid-copy alloc failure would
     * otherwise leave a permanent partial copy (pools are monotonic, deletes only
     * tombstone).  Count is conservative -- merges into pre-existing dst
     * keys/values are not discounted, so this can only over-reserve, never
     * under-reserve. */
    {
        uint32_t need_keys = 0, need_vals = 0;
        reg_count_subtree(src_root, &need_keys, &need_vals);
        if (need_keys > (REG_KEY_POOL_SIZE - reg_key_pool_next) ||
            need_vals > (REG_VALUE_POOL_SIZE - reg_value_pool_next))
            return ERROR_OUTOFMEMORY;
    }

    {
        uint32_t nkeys = 0, nvals = 0;
        long rc = reg_copy_subtree(src_root, dst, &nkeys, &nvals);
        if (rc == ERROR_SUCCESS) {
            /* Structure-only copies never touch reg_set_value_direct, so mark
             * the destination hive dirty here or a value-less copy would not be
             * persisted by the lazy flush path. */
            dst->last_write_time = reg_get_uptime_ns();
            registry_mark_dirty(dst);
            /* Per-inserted-key NAME notifications are emitted by reg_create_child
             * inside reg_copy_subtree; value copies notify LAST_SET via
             * reg_set_value_direct.  No aggregate dispatch needed here. */
            klog(LOG_DEBUG, "registry", "CopyTree: %s -> %s (%u keys, %u values)",
                 src_root->name, dst->name, (uint64_t)nkeys, (uint64_t)nvals);
        }
        return rc;
    }
}

/* ---- RegRenameKey (Win32, in-place) ---- */

long RegRenameKey(HKEY hKey, const char *lpSubKeyName, const char *lpNewKeyName)
{
    reg_key_t *base, *target;
    long acc;

    base = reg_resolve_key(hKey);
    if (!base)
        return ERROR_INVALID_HANDLE;
    if (!lpNewKeyName || lpNewKeyName[0] == '\0' ||
        reg_strlen(lpNewKeyName) > REG_MAX_KEY_NAME)
        return ERROR_INVALID_PARAMETER;

    /* Renaming destroys the old key name -- require DELETE. */
    acc = reg_check_access(hKey, DELETE);
    if (acc != ERROR_SUCCESS)
        return acc;

    if (lpSubKeyName && lpSubKeyName[0] != '\0') {
        acc = reg_validate_path_limits(lpSubKeyName);
        if (acc != ERROR_SUCCESS)
            return acc;
        target = reg_walk_path(base, lpSubKeyName, 0);
    } else {
        target = base;
    }
    if (!target)
        return ERROR_FILE_NOT_FOUND;

    /* Win32 collision: reject only if a DIFFERENT sibling has the new name.
     * A match on target itself (same-name / case-only rename) must fall through
     * to RegRenameKeyDirect, which treats it as a no-op / case update. */
    if (target->parent) {
        reg_key_t *existing = reg_find_child(target->parent, lpNewKeyName);
        if (existing && existing != target)
            return ERROR_ALREADY_EXISTS;
    }

    /* In-place unlink/relink (no copy, no pool churn). */
    return RegRenameKeyDirect(target, lpNewKeyName);
}

/* ---- RegSaveKey / RegRestoreKey (privilege-gated subtree export/import) ----
 *
 * Now that SePrivilegeCheck exists, these are gated on SeBackupPrivilege /
 * SeRestorePrivilege (a KernelMode/Zw caller is trusted; a UserMode caller must
 * hold the enabled privilege). Once past the gate they drive the existing
 * hive_save/hive_load subtree serializers directly (path-based). */
long RegSaveKey(HKEY hKey, const char *lpFile, void *lpSecurityAttributes)
{
    reg_key_t *key;
    (void)lpSecurityAttributes;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;
    if (!lpFile)
        return ERROR_INVALID_PARAMETER;
    if (reg_check_access(hKey, KEY_READ) != ERROR_SUCCESS)
        return ERROR_ACCESS_DENIED;             /* handle lacks read access */
    if (!SeSinglePrivilegeCheck(&SeBackupPrivilege, ssdt_previous_mode()))
        return ERROR_PRIVILEGE_NOT_HELD;        /* SeBackupPrivilege required */

    /* SePrivilegeCheck (the gate this section owns) now guards subtree export.
     * The hive_save BODY is deferred together with RegRestoreKey's: driving
     * hive_save off a caller-owned lpFile needs a probe/copy of the path into a
     * bounded kernel buffer (a UserMode caller could fault, or race the length
     * check so the journal ".log"/".bak" paths differ from the final target),
     * which is marshaling infrastructure owned by the registry save/restore
     * completion work. Fail closed rather than key file I/O off a mutable
     * caller pointer. */
    return ERROR_NOT_SUPPORTED;
}

long RegRestoreKey(HKEY hKey, const char *lpFile, uint32_t dwFlags)
{
    reg_key_t *key;
    (void)dwFlags;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;
    if (!lpFile)
        return ERROR_INVALID_PARAMETER;
    if (reg_check_access(hKey, KEY_WRITE) != ERROR_SUCCESS)
        return ERROR_ACCESS_DENIED;             /* handle lacks write access */
    if (!SeSinglePrivilegeCheck(&SeRestorePrivilege, ssdt_previous_mode()))
        return ERROR_PRIVILEGE_NOT_HELD;        /* SeRestorePrivilege required */

    /* SePrivilegeCheck (the gate this section owns) now guards restore. The
     * restore BODY is deliberately NOT shipped here: a correct RegRestoreKey
     * REPLACES the subtree (Windows semantics -- entries absent from the hive
     * must be removed), which requires staging the hive into a scratch tree,
     * validating it, then atomically swapping so a load/apply failure preserves
     * the original subtree. A plain hive_load merges (stale keys survive, a
     * rollback/recovery integrity failure) and a pre-clear risks irreversible
     * data loss on a bad hive; both are wrong. Fail closed until the scratch-
     * tree atomic-replace infrastructure lands in the registry save/restore
     * completion work rather than report a false success. */
    return ERROR_NOT_SUPPORTED;
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

    {
        long acc = reg_check_access(hKey, KEY_QUERY_VALUE);
        if (acc != ERROR_SUCCESS)
            return acc;
    }

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

    {
        long acc = reg_check_access(hKey, KEY_QUERY_VALUE);
        if (acc != ERROR_SUCCESS)
            return acc;
    }

    {
        long lim = reg_validate_path_limits(lpSubKey);
        if (lim != ERROR_SUCCESS)
            return lim;
    }

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
    long acc;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    acc = reg_check_access(hKey, KEY_SET_VALUE);
    if (acc != ERROR_SUCCESS)
        return acc;

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
            key->last_write_time = reg_get_uptime_ns();
            registry_mark_dirty(key);
            reg_dispatch_notify(key, REG_NOTIFY_CHANGE_LAST_SET, lpValueName);
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

    {
        long acc = reg_check_access(hKey, KEY_ENUMERATE_SUB_KEYS);
        if (acc != ERROR_SUCCESS)
            return acc;
    }

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
        *lpftLastWriteTime = reg_last_write_filetime(child->last_write_time);

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

    {
        long acc = reg_check_access(hKey, KEY_QUERY_VALUE);
        if (acc != ERROR_SUCCESS)
            return acc;
    }

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

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    {
        long acc = reg_check_access(hKey, KEY_QUERY_VALUE);
        if (acc != ERROR_SUCCESS)
            return acc;
    }

    if (lpcSubKeys)
        *lpcSubKeys = key->child_count;

    if (lpcValues)
        *lpcValues = key->value_count;

    if (lpftLastWriteTime)
        *lpftLastWriteTime = reg_last_write_filetime(key->last_write_time);

    if (lpcbSecurityDescriptor)
        *lpcbSecurityDescriptor = key->security_descriptor
            ? SeGetDefaultSDSize(SE_SD_TYPE_REGISTRY_KEY) : 0;

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

/* Read an enumerated child key's full metadata directly under the PARENT's
 * enumeration right (KEY_ENUMERATE_SUB_KEYS), without opening a fresh handle to
 * the child.  NtEnumerateKey(KeyFullInformation) uses this so an enumeration-
 * only handle can report child counts (an access-capped re-open of the child
 * would spuriously fail the KEY_QUERY_VALUE check).  LastWriteTime is returned
 * as a FILETIME.  Any output pointer may be NULL. */
long reg_query_child_full_info(HKEY hKeyParent, const char *child_name,
                               uint32_t *sub_keys, uint32_t *values,
                               uint32_t *max_subkey_len, uint32_t *max_val_name,
                               uint32_t *max_val_data, uint64_t *last_write_ft)
{
    reg_key_t *parent, *child;
    reg_value_t *v;
    uint32_t b;
    long acc;

    parent = reg_resolve_key(hKeyParent);
    if (!parent)
        return ERROR_INVALID_HANDLE;

    acc = reg_check_access(hKeyParent, KEY_ENUMERATE_SUB_KEYS);
    if (acc != ERROR_SUCCESS)
        return acc;

    if (!child_name)
        return ERROR_INVALID_PARAMETER;
    child = reg_find_child(parent, child_name);
    if (!child)
        return ERROR_FILE_NOT_FOUND;

    if (sub_keys)      *sub_keys = child->child_count;
    if (values)        *values = child->value_count;
    if (last_write_ft) *last_write_ft = reg_last_write_filetime(child->last_write_time);

    if (max_subkey_len) {
        uint32_t m = 0;
        for (b = 0; b < REG_CHILD_BUCKETS; b++) {
            reg_key_t *c = child->children[b];
            while (c) {
                uint32_t l = reg_strlen(c->name);
                if (l > m) m = l;
                c = c->hash_next;
            }
        }
        *max_subkey_len = m;
    }
    if (max_val_name || max_val_data) {
        uint32_t mn = 0, md = 0;
        for (v = child->values; v; v = v->next) {
            uint32_t l = reg_strlen(v->name);
            if (l > mn) mn = l;
            if (v->data_size > md) md = v->data_size;
        }
        if (max_val_name) *max_val_name = mn;
        if (max_val_data) *max_val_data = md;
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
    uint32_t local = 0;
    long rc;

    if (!pValue)
        return ERROR_INVALID_PARAMETER;

    /* Read into a zero-initialized local, not *pValue, so a malformed value never
     * modifies the caller's output on failure. A REG_DWORD shorter than 4 bytes
     * (partial copy) is rejected via the exact-size check; an oversized value
     * makes RegQueryValueEx return ERROR_MORE_DATA (rc != SUCCESS). *pValue is
     * assigned only on a clean exact-size REG_DWORD read. */
    rc = RegQueryValueEx(hKey, lpValueName, (uint32_t *)0, &type,
                         (uint8_t *)&local, &size);
    if (rc != ERROR_SUCCESS)
        return rc;
    if (type != REG_DWORD || size != sizeof(uint32_t))
        return ERROR_FILE_NOT_FOUND;
    *pValue = local;
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

    /* --- HKLM\SYSTEM\PowerControl --- */
    /* The button-action policy the ACPI fixed-event dispatcher reads once at
     * init (include/kernel/acpi.h, ACPI_BTN_REG_PATH). Seeded here because a
     * policy surface whose key never exists is not configurable at all: every
     * boot took the "no key" branch and the documented values could not be set
     * by anyone. 3 = shut down, 1 = sleep, matching the Windows encoding. */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\PowerControl", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "PowerButtonAction", 3);
        RegSetDword(hKey, "SleepButtonAction", 1);
        RegCloseKey(hKey);
        count += 2;
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

    /* --- HKLM\SYSTEM\Nls: system ANSI/OEM code page policy (GetACP/GetOEMCP) --- */
    nls_cp_register_defaults();

    /* --- HKLM\SYSTEM\Nls: locale policy (system/user locale + UI language) --- */
    nls_locale_register_defaults();

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
        /* Count feeds NUMBER_OF_PROCESSORS in the default environment block. */
        {
            extern uint32_t smp_cpu_count(void);
            uint32_t ncpu = smp_cpu_count();
            RegSetDword(hKey, "Count", ncpu ? ncpu : 1u);
        }
        RegCloseKey(hKey);
        count += 3;
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

    /* --- HKLM\SYSTEM\ComputerName\ActiveComputerName ---
     * Source for the COMPUTERNAME environment variable (env_init_defaults). */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                       "SYSTEM\\ComputerName\\ActiveComputerName", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "ComputerName", "IMPOSSIBLE-PC");
        RegCloseKey(hKey);
        count += 1;
    }

    /* --- HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment ---
     * Machine-wide system environment variables. env_init_defaults() enumerates
     * this key and overlays each value onto the synthesised base. ComSpec is the
     * canonical example of a system var not synthesised elsewhere. */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                       "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                       0, (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "ComSpec",                "C:\\cmd.exe");
        RegSetString(hKey, "OS",                     "Impossible_OS");
        RegSetString(hKey, "PROCESSOR_ARCHITECTURE", "AMD64");
        RegSetString(hKey, "windir",                 "C:\\Impossible");
        RegCloseKey(hKey);
        count += 4;
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

    /* Populate per-CPU register audit (HKLM\HARDWARE\CPU\%u\Registers) from the
     * snapshots captured during SMP bringup (TODO-09-boot S9). Local extern to
     * keep this arch-neutral file free of cpu_security.h (arch header). */
    {
        extern void cpu_audit_populate_registry(void);
        cpu_audit_populate_registry();
    }

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

/* PMM contiguous allocation comes from kernel/mm/pmm.h (included above). The
 * hand-rolled externs that used to sit here declared pmm_alloc_contiguous()
 * with a uint32_t page count while the real definition takes uint64_t -- a
 * silent prototype mismatch that only surfaced once the real header was
 * included. Never re-add a local extern for it. */

/* External: PIT ticks for timestamp */


/* VFS flags shorthand */
#define HIVE_VFS_O_READ  VFS_O_READ
#define HIVE_VFS_O_WRITE VFS_O_WRITE

/* ---- CRC32 (IEEE) -- delegates to the shared kcrc32 (byte-identical to the
 * former bitwise loop; ~crc == crc ^ 0xFFFFFFFF), so existing hive checksums
 * still validate. ---- */

static uint32_t hive_crc32(const uint8_t *data, uint32_t len)
{
    return kcrc32(data, len);
}

/* ---- Serialization buffer helpers ---- */

typedef struct {
    uint8_t *buf;
    uint32_t pos;
    uint32_t cap;
} hive_buf_t;

static int hive_buf_write_u16(hive_buf_t *b, uint16_t v)
{
    if (!b || b->pos > b->cap || 2 > b->cap - b->pos) return -1;
    b->buf[b->pos++] = (uint8_t)(v & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >> 8) & 0xFF);
    return 0;
}

static int hive_buf_write_u32(hive_buf_t *b, uint32_t v)
{
    if (!b || b->pos > b->cap || 4 > b->cap - b->pos) return -1;
    b->buf[b->pos++] = (uint8_t)(v & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >>  8) & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >> 16) & 0xFF);
    b->buf[b->pos++] = (uint8_t)((v >> 24) & 0xFF);
    return 0;
}

static int hive_buf_write_bytes(hive_buf_t *b, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    if (!b || (!data && len != 0)) return -1;
    if (b->pos > b->cap || len > b->cap - b->pos) return -1;
    for (i = 0; i < len; i++)
        b->buf[b->pos++] = data[i];
    return 0;
}

static int hive_buf_read_u16(hive_buf_t *b, uint16_t *v)
{
    if (!b || !v || b->pos > b->cap || 2 > b->cap - b->pos) return -1;
    *v = (uint16_t)b->buf[b->pos]
       | ((uint16_t)b->buf[b->pos + 1] << 8);
    b->pos += 2;
    return 0;
}

static int hive_buf_read_u32(hive_buf_t *b, uint32_t *v)
{
    if (!b || !v || b->pos > b->cap || 4 > b->cap - b->pos) return -1;
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
    if (!b || (!out && len != 0)) return -1;
    if (b->pos > b->cap || len > b->cap - b->pos) return -1;
    for (i = 0; i < len; i++)
        out[i] = b->buf[b->pos++];
    return 0;
}

static int hive_buf_skip(hive_buf_t *b, uint32_t len)
{
    if (!b || b->pos > b->cap || len > b->cap - b->pos) return -1;
    b->pos += len;
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
            /* Volatile subtrees are RAM-only -- exclude from header totals so
             * they match hive_serialize_key's emitted payload. */
            if (!(c->flags & REG_FLAG_VOLATILE))
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
    uint16_t child_count = 0;
    reg_value_t *v;
    uint32_t b;

    /* Count only NON-volatile children: volatile keys are RAM-only and are not
     * emitted below, so the written child_count must match the records actually
     * serialized or reload walks past the payload. */
    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = key->children[b];
        while (c) {
            if (!(c->flags & REG_FLAG_VOLATILE)) child_count++;
            c = c->hash_next;
        }
    }

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

    /* Recurse into non-volatile children (depth-first across all hash buckets) */
    for (b = 0; b < REG_CHILD_BUCKETS; b++) {
        reg_key_t *c = key->children[b];
        while (c) {
            if (!(c->flags & REG_FLAG_VOLATILE)) {
                if (hive_serialize_key(buf, c) < 0) return -1;
            }
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
    hdr->timestamp    = reg_get_uptime_ns();
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

/* ---- Deserialization ---- */

static int hive_parse_value(hive_buf_t *buf, reg_key_t *key,
                            uint32_t *parsed_values,
                            uint32_t *loaded_values,
                            int apply)
{
    uint16_t vname_len;
    char vname[REG_MAX_VALUE_NAME + 1];
    uint32_t vtype, vdata_size;
    reg_value_t *v = (reg_value_t *)0;
    int new_value = 0;

    if (hive_buf_read_u16(buf, &vname_len) < 0) return -1;
    if (vname_len > REG_MAX_VALUE_NAME) return -1;
    if (hive_buf_read_bytes(buf, (uint8_t *)vname, vname_len) < 0) return -1;
    vname[vname_len] = '\0';
    if (hive_buf_read_u32(buf, &vtype) < 0) return -1;
    if (hive_buf_read_u32(buf, &vdata_size) < 0) return -1;
    if (vdata_size > REG_MAX_VALUE_SIZE) return -1;

    (*parsed_values)++;
    if (apply) {
        if (!key) return -1;
        v = reg_find_value_in_key(key, vname);
        if (!v) {
            v = reg_alloc_value(vname, vtype);
            if (!v) return -1;
            new_value = 1;
        }
        if (hive_buf_read_bytes(buf, v->data, vdata_size) < 0)
            return -1;
        v->type = vtype;
        v->data_size = vdata_size;
        if (new_value) {
            v->next = key->values;
            key->values = v;
            key->value_count++;
        }
        key->last_write_time = reg_get_uptime_ns();
        registry_mark_dirty(key);
        reg_dispatch_notify(key, REG_NOTIFY_CHANGE_LAST_SET, vname);
        (*loaded_values)++;
    } else {
        if (hive_buf_skip(buf, vdata_size) < 0)
            return -1;
    }
    return 0;
}

static int hive_parse_key(hive_buf_t *buf, reg_key_t *parent,
                          uint32_t *parsed_keys,
                          uint32_t *parsed_values,
                          uint32_t *loaded_values,
                          uint32_t depth,
                          int apply)
{
    uint16_t name_len, val_count, child_count;
    char name[REG_MAX_KEY_NAME + 1];
    reg_key_t *key = (reg_key_t *)0;
    uint16_t vi, ci;

    if (depth == 0 || depth > HIVE_MAX_PARSE_DEPTH)
        return -1;

    /* Read key record */
    if (hive_buf_read_u16(buf, &name_len) < 0) return -1;
    if (name_len > REG_MAX_KEY_NAME) return -1;
    if (hive_buf_read_bytes(buf, (uint8_t *)name, name_len) < 0) return -1;
    name[name_len] = '\0';
    if (hive_buf_read_u16(buf, &val_count) < 0) return -1;
    if (hive_buf_read_u16(buf, &child_count) < 0) return -1;
    (*parsed_keys)++;

    /* Create or find the key under parent */
    if (apply) {
        if (!parent) return -1;
        key = reg_find_child(parent, name);
        if (!key)
            key = reg_create_child(parent, name);
        if (!key) return -1;
    }

    /* Read values */
    for (vi = 0; vi < val_count; vi++) {
        if (hive_parse_value(buf, key, parsed_values, loaded_values, apply) < 0)
            return -1;
    }

    /* Recurse into children */
    for (ci = 0; ci < child_count; ci++) {
        if (hive_parse_key(buf, key, parsed_keys, parsed_values,
                           loaded_values, depth + 1, apply) < 0)
            return -1;
    }

    return 0;
}

static int hive_parse_payload(hive_buf_t *deser, reg_key_t *root,
                              uint32_t *parsed_keys,
                              uint32_t *parsed_values,
                              uint32_t *loaded_values,
                              int apply)
{
    uint16_t root_name_len, root_val_count, root_child_count;
    uint16_t vi, ci;

    if (!deser || !parsed_keys || !parsed_values || !loaded_values)
        return -1;

    *parsed_keys = 0;
    *parsed_values = 0;
    *loaded_values = 0;

    if (hive_buf_read_u16(deser, &root_name_len) < 0) return -1;
    /* Payload root record carries the full key name (hive_serialize_key emits
     * strlen(name) up to REG_MAX_KEY_NAME); the header root_name[64] is a
     * separate truncated quick-id field. Bound the payload name accordingly. */
    if (root_name_len > REG_MAX_KEY_NAME) return -1;
    if (hive_buf_skip(deser, root_name_len) < 0) return -1;
    if (hive_buf_read_u16(deser, &root_val_count) < 0) return -1;
    if (hive_buf_read_u16(deser, &root_child_count) < 0) return -1;
    (*parsed_keys)++;

    for (vi = 0; vi < root_val_count; vi++) {
        if (hive_parse_value(deser, root, parsed_values, loaded_values, apply) < 0)
            return -1;
    }

    for (ci = 0; ci < root_child_count; ci++) {
        if (hive_parse_key(deser, root, parsed_keys, parsed_values,
                           loaded_values, 1, apply) < 0)
            return -1;
    }

    return (deser->pos == deser->cap) ? 0 : -1;
}

static int hive_validate_payload(const uint8_t *data, uint32_t data_size,
                                 uint32_t expected_keys,
                                 uint32_t expected_values,
                                 uint32_t *parsed_keys_out,
                                 uint32_t *parsed_values_out)
{
    hive_buf_t deser;
    uint32_t parsed_keys, parsed_values, loaded_values;

    if (!data || data_size == 0)
        return -1;

    deser.buf = (uint8_t *)(uintptr_t)data;
    deser.pos = 0;
    deser.cap = data_size;

    if (hive_parse_payload(&deser, (reg_key_t *)0, &parsed_keys,
                           &parsed_values, &loaded_values, 0) < 0)
        return -1;
    if (parsed_keys != expected_keys || parsed_values != expected_values)
        return -1;

    if (parsed_keys_out) *parsed_keys_out = parsed_keys;
    if (parsed_values_out) *parsed_values_out = parsed_values;
    return 0;
}

#ifdef KERNEL_TESTS
int hive_validate_payload_for_test(const uint8_t *data, uint32_t data_size,
                                   uint32_t expected_keys,
                                   uint32_t expected_values)
{
    return hive_validate_payload(data, data_size, expected_keys,
                                 expected_values, (uint32_t *)0,
                                 (uint32_t *)0);
}
#endif

static void hive_free_pages(uintptr_t phys, uint32_t pages)
{
    uint32_t p;
    if (!phys) return;
    for (p = 0; p < pages; p++)
        pmm_free_frame(phys + p * 4096);
}

static int hive_read_validated_payload(struct vfs_node *f,
                                       const char *path,
                                       const hive_header_t *hdr,
                                       uint8_t **data_out,
                                       uintptr_t *phys_out,
                                       uint32_t *pages_out,
                                       uint32_t *parsed_keys_out,
                                       uint32_t *parsed_values_out)
{
    uint32_t buf_pages;
    uintptr_t data_phys;
    uint8_t *data_ptr;
    int rc;

    if (!f || !path || !hdr || !data_out || !phys_out || !pages_out)
        return -1;
    if (hdr->data_size == 0 || hdr->data_size > 1024 * 1024)
        return -1;

    buf_pages = (hdr->data_size + 4095) / 4096;
    data_phys = pmm_alloc_contiguous(buf_pages);
    if (!data_phys)
        return -1;
    data_ptr = (uint8_t *)data_phys;

    rc = vfs_read(f, HIVE_HEADER_SIZE, hdr->data_size, data_ptr);
    if (rc < (int)hdr->data_size) {
        klog(LOG_WARN, "hive", "Short data read in '%s': got %d, expected %u",
             path, rc, (uint64_t)hdr->data_size);
        hive_free_pages(data_phys, buf_pages);
        return -1;
    }

    if (hive_validate_payload(data_ptr, hdr->data_size, hdr->total_keys,
                              hdr->total_values, parsed_keys_out,
                              parsed_values_out) < 0) {
        klog(LOG_WARN, "hive", "Payload validation failed in '%s'", path);
        hive_free_pages(data_phys, buf_pages);
        return -1;
    }

    *data_out = data_ptr;
    *phys_out = data_phys;
    *pages_out = buf_pages;
    return 0;
}

static int hive_read_valid_header(struct vfs_node *f, const char *path,
                                  hive_header_t *hdr)
{
    uint32_t saved_crc, computed_crc;
    int rc;

    if (!f || !path || !hdr) return -1;

    rc = vfs_read(f, 0, HIVE_HEADER_SIZE, (uint8_t *)hdr);
    if (rc < (int)HIVE_HEADER_SIZE) return -1;
    if (hdr->magic != HIVE_MAGIC) return -1;
    if (hdr->version != HIVE_VERSION) return -1;
    if (hdr->data_offset != HIVE_HEADER_SIZE) return -1;

    saved_crc = hdr->checksum;
    hdr->checksum = 0;
    computed_crc = hive_crc32((const uint8_t *)hdr, HIVE_HEADER_SIZE);
    hdr->checksum = saved_crc;
    if (computed_crc != saved_crc) return -1;

    return 0;
}

/* Check if a hive file has a valid header and a fully parseable payload. */
static int hive_validate_file(const char *path)
{
    struct vfs_node *f;
    hive_header_t *hdr;      /* off-stack: 4 KiB header + recursive parser */
    uint8_t *data_ptr = (uint8_t *)0;
    uintptr_t data_phys = 0;
    uint32_t buf_pages = 0;
    int rc = -1;

    hdr = (hive_header_t *)pmm_alloc_contiguous(1);
    if (!hdr) return -1;

    f = vfs_open(path, HIVE_VFS_O_READ);
    if (!f) {
        hive_free_pages((uintptr_t)hdr, 1);
        return -1;
    }

    if (hive_read_valid_header(f, path, hdr) == 0 &&
        hive_read_validated_payload(f, path, hdr, &data_ptr, &data_phys,
                                    &buf_pages, (uint32_t *)0,
                                    (uint32_t *)0) == 0)
        rc = 0;

    vfs_close(f);
    hive_free_pages(data_phys, buf_pages);
    hive_free_pages((uintptr_t)hdr, 1);
    return rc;
}

/* Try to recover from a journal or backup file.
 * Priority: .hive.log (crash during write) -> .hive -> .hive.bak
 * Returns the best path to load from, or NULL if none are valid. */
static const char *hive_best_source(const char *filepath,
                                     char *log_path, char *bak_path)
{
    hive_str_append(log_path, 160, filepath, ".log");
    hive_str_append(bak_path, 160, filepath, ".bak");

    /* 1. Check journal -- if fully valid, a crash happened mid-write */
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

/* ---- hive_load ---- */

int hive_load(const char *filepath, reg_key_t *root)
{
    struct vfs_node *f = (struct vfs_node *)0;
    hive_header_t *hdr = (hive_header_t *)0;
    uint32_t saved_crc, computed_crc;
    uint32_t buf_pages = 0;
    uintptr_t data_phys = 0;
    uint8_t *data_ptr = (uint8_t *)0;
    hive_buf_t deser;
    uint32_t parsed_keys = 0, parsed_values = 0;
    uint32_t loaded_values = 0;
    int rc, ret = -1;

    deser.pos = 0;

    if (!filepath || !root) return -1;
    if (!vfs_is_mounted(filepath[0])) return -1;

    /* The 4 KiB hive header must NOT live on the 8 KiB task stack: the
     * recursive hive parser (hive_parse_key) needs that stack. One PMM page
     * (identity-mapped, freed via the single `out:` cleanup below). */
    hdr = (hive_header_t *)pmm_alloc_contiguous(1);
    if (!hdr) return -1;

    /* Open the hive file */
    f = vfs_open(filepath, HIVE_VFS_O_READ);
    if (!f) goto out;  /* File doesn't exist -- not an error, just no saved data */

    /* Read header */
    rc = vfs_read(f, 0, HIVE_HEADER_SIZE, (uint8_t *)hdr);
    if (rc < (int)HIVE_HEADER_SIZE) {
        klog(LOG_WARN, "hive", "Short read on '%s' header (%d bytes)", filepath, rc);
        goto out;
    }

    /* Validate magic */
    if (hdr->magic != HIVE_MAGIC) {
        klog(LOG_WARN, "hive", "Bad magic in '%s': 0x%x (expected REGH)", filepath,
             (uint64_t)hdr->magic);
        goto out;
    }

    /* Validate version */
    if (hdr->version != HIVE_VERSION) {
        klog(LOG_WARN, "hive", "Unsupported hive version %u in '%s'",
             (uint64_t)hdr->version, filepath);
        goto out;
    }

    if (hive_read_valid_header(f, filepath, hdr) < 0) {
        saved_crc = hdr->checksum;
        hdr->checksum = 0;
        computed_crc = hive_crc32((const uint8_t *)hdr, HIVE_HEADER_SIZE);
        klog(LOG_WARN, "hive", "CRC32 mismatch in '%s': file=0x%x computed=0x%x",
             filepath, (uint64_t)saved_crc, (uint64_t)computed_crc);
        goto out;
    }

    /* Sanity check data size */
    if (hdr->data_size == 0 || hdr->data_size > 1024 * 1024) {
        klog(LOG_WARN, "hive", "Invalid data size %u in '%s'",
             (uint64_t)hdr->data_size, filepath);
        goto out;
    }

    if (hive_read_validated_payload(f, filepath, hdr, &data_ptr, &data_phys,
                                    &buf_pages, &parsed_keys,
                                    &parsed_values) < 0) {
        klog(LOG_WARN, "hive", "Invalid hive payload in '%s'", filepath);
        goto out;
    }
    vfs_close(f);
    f = (struct vfs_node *)0;

    if (parsed_keys > 0 &&
        parsed_keys - 1 > (REG_KEY_POOL_SIZE - reg_key_pool_next))
        goto out;
    if (parsed_values > (REG_VALUE_POOL_SIZE - reg_value_pool_next))
        goto out;

    /* Deserialize into the tree */
    deser.buf = data_ptr;
    deser.pos = 0;
    deser.cap = hdr->data_size;

    if (hive_parse_payload(&deser, root, &parsed_keys, &parsed_values,
                           &loaded_values, 1) < 0) {
        klog(LOG_WARN, "hive", "Corrupt data in '%s' at offset %u",
             filepath, (uint64_t)deser.pos);
        goto out;
    }

    klog(LOG_DEBUG, "hive", "Loaded '%s': %u keys, %u values",
         filepath, (uint64_t)hdr->total_keys, (uint64_t)loaded_values);
    ret = (int)loaded_values;

out:
    if (f) vfs_close(f);
    hive_free_pages(data_phys, buf_pages);
    hive_free_pages((uintptr_t)hdr, 1);
    return ret;
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

/* ---- RegFlushKey: force an immediate flush of one hive ---- */

long RegFlushKey(HKEY hKey)
{
    reg_key_t *key, *cur;
    uint32_t i;

    key = reg_resolve_key(hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;

    /* Flushing forces persistence I/O; require a read right on the handle so a
     * no-access handle cannot drive disk activity. */
    {
        long acc = reg_check_access(hKey, KEY_QUERY_VALUE);
        if (acc != ERROR_SUCCESS)
            return acc;
    }

    /* Volatile keys have no hive backing: flush is a no-op success (Win32). */
    if (key->flags & REG_FLAG_VOLATILE)
        return ERROR_SUCCESS;

    /* Nothing persisted yet this boot -- nothing to flush. */
    if (!registry_ready || !hive_table_inited)
        return ERROR_SUCCESS;
    if (!vfs_is_mounted('C'))
        return ERROR_INVALID_HANDLE;

    /* Walk to the depth-1 child of a root key (the hive root). */
    cur = key;
    while (cur && cur->parent && cur->parent->parent)
        cur = cur->parent;

    /* A persisted-root handle (\Registry\Machine, \Registry\User) covers
     * several hives; flush all of them rather than returning a no-op that
     * would silently drop dirty SYSTEM/SOFTWARE/HARDWARE/DEFAULT data.  Only
     * these two roots back hives -- HKCC and other parentless predefined roots
     * fall through to the no-matching-hive no-op below. */
    if (cur == reg_root_hklm || cur == reg_root_hku) {
        int failures = registry_flush_checked();
        if (failures > 0)
            return ERROR_REGISTRY_IO_FAILED;
        return ERROR_SUCCESS;
    }

    /* Save only the single hive that contains hKey. */
    for (i = 0; i < REG_HIVE_COUNT; i++) {
        reg_key_t *sub = hive_get_subkey(i);
        if (sub && sub == cur) {
            if (hive_save(sub, hive_table[i].path) == 0) {
                hive_table[i].dirty = 0;
                return ERROR_SUCCESS;
            }
            return ERROR_REGISTRY_IO_FAILED;
        }
    }

    /* Key is not under any persisted hive root (predefined-only subtree):
     * treat as a successful no-op. */
    return ERROR_SUCCESS;
}

int registry_persistence_active(void)
{
    /* Persistence-ORIGIN latch, not flush liveness. hive_table_inited is set
     * once the on-disk hive layer first engages this boot (save/load) and is
     * never cleared, so recoverable hive copies (main/.bak/journal) may exist
     * from here on. It deliberately does NOT test vfs_is_mounted('C'): a later
     * unmount must not make this return 0 and re-open the in-memory absorb
     * path for disk-originated one-shot secrets. Side-effect free (saves
     * nothing) -- unlike registry_flush_checked(), which DOES flush and which
     * returns -1 on unmount. */
    return (registry_ready && hive_table_inited) ? 1 : 0;
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
