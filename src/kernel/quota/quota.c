/* ============================================================================
 * quota.c -- Kernel resource-accounting type registry
 *
 * Owns the static const descriptor table that names every chargeable kernel
 * resource type, its accounting unit, default limit, and override privilege.
 * The table is the single source of truth for the taxonomy; the charge API and
 * every integration section consume it through the accessors below.
 * ============================================================================ */

#include "kernel/quota/quota.h"
#include "kernel/security/privileges.h"  /* SE_INCREASE_QUOTA_PRIVILEGE */
#include "kernel/klog.h"

/* --- Descriptor table ---------------------------------------------------- *
 * static const so every field is available at link time (before any charge)
 * and reads are lock-free/SMP-safe. All default limits are UNLIMITED at this
 * layer; concrete numeric caps are policy owned by the kernel configuration
 * layer, applied at charge time per the precedence documented in quota.h. */
/* Local shorthands: every row shares the same override privilege and (at this
 * layer) an unlimited default; concrete caps come from kernel config. */
#define Q_PRIV  SE_INCREASE_QUOTA_PRIVILEGE
#define Q_UNL   QUOTA_LIMIT_UNLIMITED

static const quota_resource_desc_t g_quota_desc[QUOTA_RESOURCE_TYPE_COUNT] = {
    [QUOTA_RES_HANDLE]             = { "handles",             Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_OBJECT_BODY]        = { "object-bodies",       Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_NAMESPACE_ENTRY]    = { "namespace-entries",   Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_PAGED_POOL]         = { "paged-pool",          Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
    [QUOTA_RES_NONPAGED_POOL]      = { "nonpaged-pool",       Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
    [QUOTA_RES_REGISTRY_BYTES]     = { "registry-bytes",      Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
    [QUOTA_RES_ALPC_MESSAGE]       = { "alpc-messages",       Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_NOTIFICATION_STATE] = { "notification-states", Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_TIMER]              = { "timers",              Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_THREAD]             = { "threads",             Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_PROCESS]            = { "processes",           Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_SECTION]            = { "sections",            Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_MAPPED_VIEW]        = { "mapped-views",        Q_PRIV, Q_UNL, QUOTA_UNIT_COUNT },
    [QUOTA_RES_CRASH_BUFFER]       = { "crash-buffers",       Q_PRIV, Q_UNL, QUOTA_UNIT_BYTES },
};

#undef Q_PRIV
#undef Q_UNL

/* Layer 1 defense: the parallel array must have exactly one entry per type, so
 * a new enum member without a matching row is a compile error, not a runtime
 * gap (a designated-initializer table would otherwise leave holes zero-filled
 * with a NULL name). */
_Static_assert(sizeof(g_quota_desc) / sizeof(g_quota_desc[0]) == QUOTA_RESOURCE_TYPE_COUNT,
               "quota descriptor table must have one entry per resource type");

/* Registry-ready flag. Written once by quota_register_types() on the BSP in
 * Phase 2 before any consumer charges; read-only thereafter. Release/acquire
 * pairs the store with the reads so an AP that sees readiness also sees a fully
 * validated table. */
static volatile int g_quota_ready = 0;

/* Content-compare two descriptor names (string literals are not guaranteed to
 * be pooled to one pointer, so a pointer compare is not sufficient). */
static int quota_name_eq(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

boot_result_t quota_register_types(void)
{
    /* Layer 2 defense: validate the static table. A malformed taxonomy (a hole
     * left by a missing initializer, a bad unit, a missing override privilege,
     * or a duplicate name) must halt boot rather than let consumers charge
     * against an inconsistent registry. */
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const quota_resource_desc_t *d = &g_quota_desc[i];
        if (d->name == (const char *)0 || d->name[0] == '\0') {
            klog(LOG_FATAL, "quota", "resource type %u has no name", (uint64_t)i);
            return BOOT_FATAL;
        }
        if (d->unit != QUOTA_UNIT_COUNT && d->unit != QUOTA_UNIT_BYTES) {
            klog(LOG_FATAL, "quota", "resource type %u (%s) has bad unit %d",
                 (uint64_t)i, d->name, (int64_t)d->unit);
            return BOOT_FATAL;
        }
        /* Security-sensitive: every row must name a real override privilege; a
         * zero LUID would mean nothing gates raising this resource's cap. */
        if (RtlIsZeroLuid(&d->override_privilege)) {
            klog(LOG_FATAL, "quota", "resource type %u (%s) has a zero override privilege",
                 (uint64_t)i, d->name);
            return BOOT_FATAL;
        }
        /* Names must be unique so a dump or by-name query never maps two types
         * to one label. */
        for (uint32_t j = 0; j < i; j++) {
            if (quota_name_eq(d->name, g_quota_desc[j].name)) {
                klog(LOG_FATAL, "quota", "resource type %u (%s) duplicates type %u",
                     (uint64_t)i, d->name, (uint64_t)j);
                return BOOT_FATAL;
            }
        }
    }

    __atomic_store_n(&g_quota_ready, 1, __ATOMIC_RELEASE);
    klog(LOG_INFO, "quota", "resource type registry: %u types validated",
         (uint64_t)QUOTA_RESOURCE_TYPE_COUNT);
    return BOOT_OK;
}

const quota_resource_desc_t *quota_resource_desc(quota_resource_type_t type)
{
    if ((uint32_t)type >= QUOTA_RESOURCE_TYPE_COUNT)
        return (const quota_resource_desc_t *)0;
    return &g_quota_desc[type];
}

const char *quota_resource_type_name(quota_resource_type_t type)
{
    const quota_resource_desc_t *d = quota_resource_desc(type);
    return d ? d->name : "?";
}

uint32_t quota_resource_type_count(void)
{
    return QUOTA_RESOURCE_TYPE_COUNT;
}

int quota_registry_ready(void)
{
    return __atomic_load_n(&g_quota_ready, __ATOMIC_ACQUIRE);
}

void quota_types_dump(void)
{
    klog(LOG_INFO, "quota", "resource type registry (%u types):",
         (uint64_t)QUOTA_RESOURCE_TYPE_COUNT);
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const quota_resource_desc_t *d = &g_quota_desc[i];
        const char *unit = (d->unit == QUOTA_UNIT_BYTES) ? "bytes" : "count";
        if (d->default_limit == QUOTA_LIMIT_UNLIMITED)
            klog(LOG_INFO, "quota", "  [%u] %s unit=%s limit=unlimited",
                 (uint64_t)i, d->name, unit);
        else
            klog(LOG_INFO, "quota", "  [%u] %s unit=%s limit=%u",
                 (uint64_t)i, d->name, unit, d->default_limit);
    }
}
