/* ============================================================================
 * quota_config.c -- Runtime configurable per-user quota defaults (TODO-25 s6)
 *
 * The kernel-config override layer in the quota precedence rule:
 *
 *     per-block explicit  >  kernel-config override  >  taxonomy default_limit
 *
 * Until this file existed the middle term was documented as "intended" in
 * quota.h and no code implemented it, so the only cap the charge path could
 * ever enforce was whatever a caller happened to pass to quota_set_limit.
 * A default nobody can configure is not a policy.
 *
 * Shape: one TUNABLE_RUNTIME entry per resource type, "quota.user.<name>",
 * defaulting to that type's taxonomy default_limit. The tunables are scoped to
 * USER blocks by NAME and by consumption -- quota_block_alloc consults them
 * only for QUOTA_PRINCIPAL_USER. That scoping is load-bearing rather than
 * cosmetic: the same allocator builds PROCESS and JOB blocks, so a
 * principal-blind override would cap one process at the entire user's budget
 * and cap a job at it a second time.
 *
 * A change reaches blocks that ALREADY EXIST (quota_user_default_relimit). The
 * canonical USER block for a SID is created once and lives as long as the user
 * is logged in, so a create-time-only default would never affect anybody
 * actually running -- an administrative control that changes nothing is worse
 * than none, because it reads as enforcement.
 * ========================================================================== */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/tunables.h"
#include "kernel/quota/quota.h"

/* Tunable names, one per resource type, in enum order.
 *
 * UNSIZED on purpose, exactly as g_quota_desc is: sizing this array
 * [QUOTA_RESOURCE_TYPE_COUNT] would make the assert below tautological. Left
 * unsized it is precisely as long as the rows present, so appending a resource
 * type without a matching name is a COMPILE error rather than a type that
 * silently cannot be configured. */
static const char *const g_quota_tunable_name[] = {
    [QUOTA_RES_HANDLE]             = "quota.user.handles",
    [QUOTA_RES_OBJECT_BODY]        = "quota.user.object_bodies",
    [QUOTA_RES_NAMESPACE_ENTRY]    = "quota.user.namespace_entries",
    [QUOTA_RES_PAGED_POOL]         = "quota.user.paged_pool",
    [QUOTA_RES_NONPAGED_POOL]      = "quota.user.nonpaged_pool",
    [QUOTA_RES_REGISTRY_BYTES]     = "quota.user.registry_bytes",
    [QUOTA_RES_ALPC_MESSAGE]       = "quota.user.alpc_messages",
    [QUOTA_RES_NOTIFICATION_STATE] = "quota.user.notification_states",
    [QUOTA_RES_TIMER]              = "quota.user.timers",
    [QUOTA_RES_THREAD]             = "quota.user.threads",
    [QUOTA_RES_PROCESS]            = "quota.user.processes",
    [QUOTA_RES_SECTION]            = "quota.user.sections",
    [QUOTA_RES_MAPPED_VIEW]        = "quota.user.mapped_views",
    [QUOTA_RES_CRASH_BUFFER]       = "quota.user.crash_buffers",
    /* Appended by section 6 -- rows follow enum order, which is ABI. */
    [QUOTA_RES_NOTIFICATION_SUB]   = "quota.user.notification_subs",
    [QUOTA_RES_NOTIFICATION_BYTES] = "quota.user.notification_bytes",
};

_Static_assert(sizeof(g_quota_tunable_name) / sizeof(g_quota_tunable_name[0])
               == QUOTA_RESOURCE_TYPE_COUNT,
    "quota tunable name table must have one entry per resource type");

const char *quota_config_tunable_name(quota_resource_type_t type)
{
    if ((uint32_t)type >= QUOTA_RESOURCE_TYPE_COUNT)
        return (const char *)0;
    return g_quota_tunable_name[type];
}

uint64_t quota_config_user_default(quota_resource_type_t type)
{
    const quota_resource_desc_t *d = quota_resource_desc(type);
    if (!d)
        return QUOTA_LIMIT_UNLIMITED;

    /* Falls back to the taxonomy when the tunable is not registered yet, which
     * is the normal state for any USER block created before Phase 2 gets here.
     * That keeps registration order a non-constraint instead of a boot-order
     * trap. */
    return kernel_tunable_get_u64(g_quota_tunable_name[type], d->default_limit);
}

/* Change callback. Runs at PASSIVE_LEVEL with the tunable registry lock NOT
 * held (kernel_tunable_set guarantees both, inline or via its deferred
 * trampoline), which is what makes it legal to take the quota registry lock
 * here -- the walk must not run beneath another subsystem's lock.
 *
 * The type is recovered from the name rather than carried in cb_ctx so the
 * single callback serves all types with no per-type context objects to keep
 * alive across an unregister. */
static void cb_quota_user_default(const char *name, int64_t new_value, void *ctx)
{
    (void)ctx;

    if (!name || new_value < 0)
        return;

    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const char *a = g_quota_tunable_name[i];
        const char *b = name;
        while (*a && (*a == *b)) { a++; b++; }
        if (*a != *b)
            continue;

        klog(LOG_INFO, "quota", "user default %s -> %u (live blocks re-limited)",
             name, (uint64_t)new_value);
        quota_user_default_relimit((quota_resource_type_t)i,
                                   (uint64_t)new_value);
        return;
    }
}

void quota_config_register_tunables(void)
{
    uint32_t registered = 0;

    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const quota_resource_desc_t *d =
            quota_resource_desc((quota_resource_type_t)i);
        int64_t def;

        if (!d)
            continue;   /* unreachable: i is bounded by the type count */

        def = (d->default_limit > (uint64_t)QUOTA_AMOUNT_MAX)
            ? QUOTA_AMOUNT_MAX
            : (int64_t)d->default_limit;

        /* min 0 is QUOTA_LIMIT_UNLIMITED, not "cap everything to nothing" --
         * the same 0-means-unlimited convention the taxonomy uses, so an
         * administrator clearing a cap uses the value the header documents. */
        if (kernel_tunable_register(g_quota_tunable_name[i], TUNABLE_UINT,
                                    TUNABLE_RUNTIME, 0, QUOTA_AMOUNT_MAX, def,
                                    cb_quota_user_default, (void *)0,
                                    TUNABLE_OWNER_CORE,
                                    TUNABLE_SRC_BUILTIN) == STATUS_SUCCESS)
            registered++;

        /* Publish the registered value as the effective default. Registration
         * applies boot.conf and any other configured source, so this is the
         * first point the configured value exists -- and no live USER block
         * can disagree yet, which is why a plain publish (no re-limit walk) is
         * the right operation here. */
        quota_user_default_publish((quota_resource_type_t)i,
                                   quota_config_user_default(
                                       (quota_resource_type_t)i));
    }

    klog(LOG_INFO, "quota", "per-user default tunables: %u of %u registered",
         (uint64_t)registered, (uint64_t)QUOTA_RESOURCE_TYPE_COUNT);
}
