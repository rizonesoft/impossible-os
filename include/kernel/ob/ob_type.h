/* ============================================================================
 * ob_type.h -- Object type descriptor
 *
 * Each kernel object type (File, Process, Thread, Event, ...) is described by
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
     * THESE COUNTERS ARE THE AUTHORITY FOR SYSTEM-WIDE PER-TYPE TOTALS. Quota is
     * not a second source for that fact, and the reason is a difference in BOTH
     * axes, so state it precisely -- an earlier draft of this contract claimed
     * quota "cannot count timers or sections", which is false:
     *
     *   here   -- live objects of THIS type, summed across every principal.
     *             System-wide, per-type, NO owner dimension.
     *   quota  -- what a PRINCIPAL (process / user / job) is charged for.
     *             Per-principal, and keyed by a FIXED 16-row resource taxonomy
     *             chosen by resource semantics, NOT derived from the object-type
     *             registry.
     *
     * CHARGE RULE, pinned here because leaving it implicit invites double
     * counting -- and scoped to the BODY-CLASS rows only, which is a correction a
     * re-adversarial round forced: an earlier draft said "exactly one row per
     * creation", and that was wrong in both directions.
     *
     *   Creating an object body charges EXACTLY ONE BODY-CLASS row -- the
     *   dedicated row when the type has one: Timer -> QUOTA_RES_TIMER,
     *   Thread -> _THREAD, Process -> _PROCESS, Section -> _SECTION,
     *   NotificationState -> _NOTIFICATION_STATE (already charged by KNF at
     *   knf_create_state) -- and QUOTA_RES_OBJECT_BODY for every OTHER registered
     *   type (Token, File, Event, Mutex, Semaphore, Job, AlpcPort, Directory,
     *   SymbolicLink, Peb, Teb, Callback, InfoFile). Never both.
     *
     *   ORTHOGONAL rows are charged INDEPENDENTLY on their own events, and this
     *   rule says nothing about them: QUOTA_RES_NAMESPACE_ENTRY per named
     *   directory entry, QUOTA_RES_HANDLE per handle-table entry,
     *   QUOTA_RES_MAPPED_VIEW at map/unmap of a SECTION_VIEW record (there is no
     *   MappedView object type), QUOTA_RES_ALPC_MESSAGE per queued message,
     *   QUOTA_RES_NOTIFICATION_BYTES for retention budget, plus the pool /
     *   registry / crash-buffer / subscription rows. ONE creation may therefore
     *   legitimately charge a body row AND orthogonal rows: KNF already charges
     *   _NOTIFICATION_STATE together with _NOTIFICATION_BYTES.
     *
     * The Object Manager charge points are bound by this rule.
     *
     * WHAT NEITHER SIDE CAN DO. Quota cannot give a per-principal count for an
     * ARBITRARY registered type: the taxonomy is frozen at 16 rows (appending
     * past it is a hard build failure, see quota.h) and cannot grow one row per
     * registered type. And these counters cannot attribute anything to an owner,
     * because they have no owner column. A per-principal-per-arbitrary-type
     * number would need a type-keyed-per-principal dimension -- up to
     * OB_MAX_TYPES rows for every live principal -- which nothing in this system
     * provides and no caller has asked for. The Object Manager charge-point
     * section of the resource-accounting roadmap records THAT as the settled
     * boundary; the enumerate-and-classify escape hatch both Windows and Linux
     * actually use for "which type is this process leaking" is the deferred
     * system-wide handle snapshot, not a unification of these two counters.
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
