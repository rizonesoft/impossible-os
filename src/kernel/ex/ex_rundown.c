/* ============================================================================
 * ex_rundown.c -- Rundown protection (TODO-06 S3).
 *
 * EX_RUNDOWN_REF is a single atomic word: bit 0 is the rundown-active flag and
 * bits 1+ hold the outstanding reference count (each reference is +2). Acquire
 * is a lock-free CAS that fails once the active bit is set; release is a plain
 * atomic subtract; the wait sets the active bit and cooperatively polls until
 * the refcount drains.
 *
 * Why a poll instead of the kernel event_t: event_wait() reads the event state
 * and only THEN enqueues/blocks, so an event_set() landing in that window is
 * lost and the waiter hangs. Rundown re-reads Count every iteration, so no
 * wakeup can be lost. The wait path is a rare teardown operation, not a hot
 * path, so the busy-yield cost is acceptable.
 * ============================================================================ */

#include "kernel/ex.h"
#include "kernel/sched/task.h"   /* yield() */
#include "kernel/klog.h"
#include "kernel/bugcheck.h"

#define EXP_RUNDOWN_ACTIVE  0x1ull   /* bit 0: rundown in progress/complete */
#define EXP_RUNDOWN_REF_INC 0x2ull   /* one outstanding reference */

void ExInitializeRundownProtection(EX_RUNDOWN_REF *r)
{
    __atomic_store_n(&r->Count, 0, __ATOMIC_RELEASE);
}

void ExReInitializeRundownProtection(EX_RUNDOWN_REF *r)
{
    __atomic_store_n(&r->Count, 0, __ATOMIC_RELEASE);
}

bool ExAcquireRundownProtection(EX_RUNDOWN_REF *r)
{
    uint64_t cur = __atomic_load_n(&r->Count, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur & EXP_RUNDOWN_ACTIVE)
            return false;                    /* rundown begun -- refuse */
        if (__atomic_compare_exchange_n(&r->Count, &cur,
                                        cur + EXP_RUNDOWN_REF_INC,
                                        true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
        /* cur reloaded with the current value on failure; retry. */
    }
}

void ExReleaseRundownProtection(EX_RUNDOWN_REF *r)
{
    /* Checked subtract: an unbalanced release (no matching acquire) would wrap
     * Count and silently corrupt every later acquire/wait, so fail fast with a
     * clear invariant bugcheck instead. The -2 never toggles bit 0. A waiter,
     * if any, observes the drop by polling. */
    uint64_t cur = __atomic_load_n(&r->Count, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((cur >> 1) == 0) {
            klog(LOG_ERROR, "ex",
                 "ExReleaseRundownProtection: unbalanced release (Count=0x%lx)",
                 cur);
            KeBugCheckEx(BUGCHECK_IOS_INVARIANT_VIOLATION,
                         (uint64_t)(uintptr_t)r, cur, 0, 0);
        }
        if (__atomic_compare_exchange_n(&r->Count, &cur,
                                        cur - EXP_RUNDOWN_REF_INC,
                                        true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
        /* cur reloaded on failure; retry. */
    }
}

void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r)
{
    /* Set the rundown-active bit (idempotent) so further acquires fail. */
    uint64_t cur = __atomic_load_n(&r->Count, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t want = cur | EXP_RUNDOWN_ACTIVE;
        if (cur == want)
            break;                           /* already active */
        if (__atomic_compare_exchange_n(&r->Count, &cur, want,
                                        true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }

    /* Cooperative poll until every outstanding reference drains. Re-reads Count
     * each iteration, so no release can be missed (unlike a state-then-block
     * event wait). PASSIVE_LEVEL only (yields). */
    while ((__atomic_load_n(&r->Count, __ATOMIC_ACQUIRE) >> 1) != 0)
        yield();
}

void ExRundownCompleted(EX_RUNDOWN_REF *r)
{
    /* Mark rundown finished with zero outstanding references. */
    __atomic_store_n(&r->Count, EXP_RUNDOWN_ACTIVE, __ATOMIC_RELEASE);
}

bool ExIsRundownActive(EX_RUNDOWN_REF *r)
{
    return (__atomic_load_n(&r->Count, __ATOMIC_ACQUIRE) & EXP_RUNDOWN_ACTIVE) != 0;
}
