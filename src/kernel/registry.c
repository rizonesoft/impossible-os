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
static uint32_t __attribute__((unused)) reg_bucket(const char *name)
{
    return reg_fnv1a(name) & (REG_CHILD_BUCKETS - 1);
}

/* ---- Pool allocators ---- */

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

static reg_value_t __attribute__((unused)) *reg_alloc_value(const char *name, uint32_t type)
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

/* ---- Initialization ---- */

void registry_init(void)
{
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

    registry_ready = 1;

    klog(LOG_DEBUG, "registry",
         "Registry initialized (pool: %u keys, %u values)",
         (uint64_t)REG_KEY_POOL_SIZE, (uint64_t)REG_VALUE_POOL_SIZE);
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
