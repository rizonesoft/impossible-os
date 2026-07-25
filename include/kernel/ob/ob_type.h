/* ============================================================================
 * ob_type.h -- Object type descriptor
 *
 * Each kernel object type (File, Process, Thread, Event, …) is described by
 * an OBJECT_TYPE singleton registered at boot.  The callbacks drive lifetime
 * and namespace behaviour for all instances of that type.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/atomic.h"

typedef struct object_type {
    const char *name;           /* type name: "File", "Process", ... */
    size_t      body_size;      /* sizeof the object body (for combined alloc) */

    /* Called when handle_count drops to 0 */
    void (*on_close)(void *body, uint32_t handle_count);

    /* Called when ref_count drops to 0 -- free body resources */
    void (*on_delete)(void *body);

    /* Optional access check on open (return 0 = allow, -1 = deny) */
    int (*on_open)(void *body, uint32_t access);

    /* Namespace parse -- walk remaining path through this object */
    int (*on_parse)(void *body, const char *remaining, void **result);

    /* Per-type statistics -- SMP-safe via atomics.
     *
     * THESE COUNTERS ARE THE AUTHORITY FOR SYSTEM-WIDE PER-TYPE TOTALS, and the
     * quota subsystem is deliberately NOT a second source for the same fact.
     * The two measure different things, and neither is derivable from the other:
     *
     *   here   -- how many Event / Timer / Section objects are live system-wide,
     *             summed across every principal. There is no owner dimension: an
     *             object is counted once, whoever created it.
     *   quota  -- how much a PRINCIPAL (process / user / job) is charged for.
     *             QUOTA_RES_OBJECT_BODY and QUOTA_RES_NAMESPACE_ENTRY are ONE
     *             scalar each per principal, covering bodies of every type,
     *             because the resource-type enum IS the accounting identity and
     *             it carries no OBJECT_TYPE dimension.
     *
     * So a per-type total cannot be read out of quota (it folds all types into
     * one counter per principal), and a per-principal charge cannot be read out
     * of here (there is no owner column). Unifying them would need a global
     * type-keyed-per-principal dimension -- up to OB_MAX_TYPES rows for every
     * live principal -- which nothing in the system provides and no caller has
     * asked for. The Object Manager charge-point section of the resource-
     * accounting roadmap records that as a SETTLED BOUNDARY, not pending work.
     *
     * Read per-type totals through NtQueryObject(ObjectTypesInformation) or
     * ob_get_types(); read per-principal charges through the quota API. */
    atomic_t    total_objects;   /* current live objects of this type */
    atomic_t    total_handles;   /* current open handles to objects of this type */
    uint32_t    peak_objects;    /* high-water mark for total_objects */
    uint32_t    peak_handles;    /* high-water mark for total_handles */

    /* Per-type tracing */
    uint8_t     tracing_enabled; /* 1 = allocate OB_TRACE_INFO for new objects */
    uint8_t     _trace_pad[3];
} OBJECT_TYPE;

/* Lift a uint32 high-water mark to `cur` with a lock-free CAS-max so two CPUs
 * updating the same type's stats concurrently cannot lose a peak update (a plain
 * read-compare-store RMW races and undercounts). `cur` is the SIGNED post-update
 * live count; values <= 0 are ignored so a transient negative -- which the not-
 * yet-serialized handle-table slot claim/free (S3 "ObpReferenceObjectByHandle")
 * can momentarily produce -- never poisons the peak to ~UINT32_MAX (peaks are
 * never decremented, so a single bad lift would stick forever). Shared by the
 * object-alloc (peak_objects) and handle-alloc/inherit (peak_handles) paths. */
static inline void ob_stat_lift_peak(uint32_t *peak, int32_t cur)
{
    if (cur <= 0)
        return;
    uint32_t c = (uint32_t)cur;
    uint32_t p = __atomic_load_n(peak, __ATOMIC_RELAXED);
    while (c > p &&
           !__atomic_compare_exchange_n(peak, &p, c, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;  /* p reloaded by the CAS on failure */
}
