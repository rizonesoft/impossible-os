/* ============================================================================
 * ex_fastref.c -- Fast References: inline-cached Ob references (TODO-06 S6).
 *
 * EX_FAST_REF packs an object pointer and a small cached-reference count into a
 * single pointer-sized word swung atomically with a plain cmpxchg (no DCAS --
 * the whole state fits in one word, unlike the SLIST header). The cached count
 * lets a hot lookup path hand out an Object Manager reference without touching
 * the object's contended refcount on every access; when the cache drains the
 * caller falls back to a real ObReferenceObjectSafe under its own lock.
 *
 * The cached count is 3 bits (max 7) because this kernel's allocator only
 * guarantees 8-byte object alignment -- see the ALIGNMENT note in ex.h for the
 * full reasoning (kmalloc's 24-byte block header yields 8-mod-16 payloads, so
 * only the low 3 bits are reliably zero). This is the x86 Windows model.
 *
 * Arch-neutral: all atomics are __atomic builtins over a uintptr_t; no inline
 * asm, no x86 register names. The packing/ownership contract lives in ex.h.
 * ============================================================================ */

#include "kernel/ex.h"
#include "kernel/klog.h"
#include "kernel/bugcheck.h"

/* --- Pure packing helpers (testable in isolation) ------------------------- */

uintptr_t ExpFastRefPack(void *object, uintptr_t count)
{
    uintptr_t obj = (uintptr_t)object;

    /* A misaligned object would alias the count field; an out-of-range count
     * would corrupt the pointer. Both are caller bugs: bugcheck (LOG_ERROR
     * first so KeBugCheckEx owns the fatal path and keeps the forensics). */
    if (obj & EX_FAST_REF_MASK) {
        klog(LOG_ERROR, "ex",
             "EX_FAST_REF object %p not %u-byte aligned",
             (uint64_t)obj, (unsigned)(EX_FAST_REF_MASK + 1));
        KeBugCheckEx(BUGCHECK_IOS_INVARIANT_VIOLATION, obj,
                     EX_FAST_REF_MASK + 1, 0, 0);
    }
    if (count > EX_FAST_REF_MAX) {
        klog(LOG_ERROR, "ex",
             "EX_FAST_REF count %u exceeds max %u",
             (unsigned)count, (unsigned)EX_FAST_REF_MAX);
        KeBugCheckEx(BUGCHECK_IOS_INVARIANT_VIOLATION, count,
                     EX_FAST_REF_MAX, 1, 0);
    }
    /* Enforce the core invariant at the single packing choke point: a non-zero
     * cached count with no object is impossible (you cannot cache references on
     * nothing). Every packing path -- init, acquire, release, swap, and this
     * public helper -- routes through here, so guarding it here makes the
     * (NULL, count>0) phantom word unconstructible through the API. */
    if (!obj && count != 0) {
        klog(LOG_ERROR, "ex",
             "EX_FAST_REF NULL object with non-zero count %u",
             (unsigned)count);
        KeBugCheckEx(BUGCHECK_IOS_INVARIANT_VIOLATION, 0, count, 2, 0);
    }
    return obj | count;
}

void *ExpFastRefUnpackObject(uintptr_t value)
{
    return (void *)(value & ~EX_FAST_REF_MASK);
}

uintptr_t ExpFastRefUnpackCount(uintptr_t value)
{
    return value & EX_FAST_REF_MASK;
}

/* Unchecked re-pack for the lock-free hot paths: replace only the count field
 * of an ALREADY-VALID packed word. Used in the acquire/release CAS loops where
 * the object bits come straight from a live word (already aligned + non-NULL)
 * and the new count is in range by construction (cnt-1 for cnt>0, cnt+1 for
 * cnt<MAX). This deliberately skips ExpFastRefPack's bugcheck branches so the
 * fast path is a minimal single-word CAS with no out-of-line call -- the
 * boundary helpers (init/swap/public ExpFastRefPack) keep the validation. */
static inline uintptr_t exp_fastref_recount(uintptr_t value, uintptr_t newcount)
{
    return (value & ~EX_FAST_REF_MASK) | newcount;
}

/* --- Lifecycle ------------------------------------------------------------ */

void ExInitializeFastReference(EX_FAST_REF *ref, void *object)
{
    /* Cached count starts at 0: the caller holds exactly the structural
     * reference. ExpFastRefPack runtime-asserts the alignment (NULL is 0, which
     * is trivially aligned and yields an empty ref). The store is the
     * publishing write -- release so a consumer that reads the ref via an
     * acquire sees a fully-formed value (matches the SMP discipline elsewhere
     * in this layer). */
    __atomic_store_n(&ref->Value, ExpFastRefPack(object, 0), __ATOMIC_RELEASE);
}

/* --- Acquire / release (lock-free CAS loops) ------------------------------ */

EX_FAST_REF_RESULT ExAcquireFastReference(EX_FAST_REF *ref)
{
    EX_FAST_REF_RESULT result;
    uintptr_t cur = __atomic_load_n(&ref->Value, __ATOMIC_ACQUIRE);

    for (;;) {
        void     *obj = ExpFastRefUnpackObject(cur);
        uintptr_t cnt = ExpFastRefUnpackCount(cur);

        if (!obj) {
            /* Empty ref -- nothing to hand out. */
            result.object = (void *)0;
            result.cached = false;
            return result;
        }
        if (cnt == 0) {
            /* Cache drained: SLOW PATH. No state change here. Per the LOCK RULE
             * in ex.h, the caller must, under its per-object lock, re-read via
             * ExGetObjectFastReference + confirm the object is still stored
             * before ObReferenceObjectSafe -- the lock excludes the swap/teardown
             * that would otherwise un-store and free the object. */
            result.object = obj;
            result.cached = false;
            return result;
        }
        /* Hand out one cached reference: decrement the count in place. The
         * pointer bits are unchanged, so this also defeats ABA -- a concurrent
         * swap to a different object changes the pointer bits and fails the CAS,
         * forcing a re-read. acquire/relaxed: success acquires (we now own a
         * reference); failure just reloads `cur`. */
        if (__atomic_compare_exchange_n(&ref->Value, &cur,
                                        exp_fastref_recount(cur, cnt - 1), false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            result.object = obj;
            result.cached = true;
            return result;
        }
        /* CAS failed -- `cur` was reloaded with the live value; retry. */
    }
}

bool ExReleaseFastReference(EX_FAST_REF *ref, void *object)
{
    uintptr_t cur;

    /* A NULL object is never a real reference: absorbing it would CAS an empty
     * ref (Value 0) to pack(NULL, 1) -- a phantom count with no object, which a
     * later compare-swap would surface as a non-zero old_count for NULL and a
     * caller would "balance" as references that never existed. Reject up front.
     * (You cannot release a reference you do not hold; the count-0 acquire path
     * returns object==NULL precisely so the caller never tries to.) */
    if (!object)
        return false;

    /* A misaligned object can never match a stored (aligned) pointer and would
     * corrupt the packed value if absorbed -- reject without touching the ref so
     * the caller dereferences it. (Defensive: callers pass the object they
     * acquired, which is aligned by construction.) */
    if ((uintptr_t)object & EX_FAST_REF_MASK)
        return false;

    cur = __atomic_load_n(&ref->Value, __ATOMIC_RELAXED);

    for (;;) {
        void     *obj = ExpFastRefUnpackObject(cur);
        uintptr_t cnt = ExpFastRefUnpackCount(cur);

        /* Can only absorb into the cache when the ref still holds THIS object
         * and the cache is not saturated. Object mismatch (the ref was swapped)
         * or saturation -> caller must ObDereferenceObject the reference. */
        if (obj != object || cnt >= EX_FAST_REF_MAX)
            return false;

        /* release: publish the returned reference so a subsequent acquirer that
         * reads it via acquire observes our store-backed reference. */
        if (__atomic_compare_exchange_n(&ref->Value, &cur,
                                        exp_fastref_recount(cur, cnt + 1), false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return true;
        /* CAS failed -- `cur` reloaded; retry. */
    }
}

/* --- Exchange / query ----------------------------------------------------- */

bool ExCompareSwapFastReference(EX_FAST_REF *ref, void *new_object,
                                void *old_object, uintptr_t *out_old_count)
{
    uintptr_t desired;
    uintptr_t cur;

    /* Swapping OUT a real object hands the caller responsibility for balancing
     * its (cached_count + 1) references -- so the swapped-out count MUST be
     * observable, or those references leak and the object never deletes.
     * Require out_old_count whenever old_object is non-NULL; bugcheck a NULL
     * out-param rather than silently succeed and drop the accounting. (A NULL
     * old_object swap compares against an empty ref -- there is no count to
     * balance, so out_old_count stays optional for that case.) */
    if (old_object && !out_old_count) {
        klog(LOG_ERROR, "ex",
             "ExCompareSwapFastReference: out_old_count is NULL for a "
             "non-NULL old object %p (cached count would leak)",
             (uint64_t)(uintptr_t)old_object);
        KeBugCheckEx(BUGCHECK_IOS_INVARIANT_VIOLATION,
                     (uintptr_t)old_object, 0, 3, 0);
    }

    /* Pack the replacement once (runtime-asserts new_object alignment; NULL ->
     * empty). cached count resets to 0: the caller supplies one structural
     * reference on new_object and replenishes the cache afterwards. */
    desired = ExpFastRefPack(new_object, 0);
    cur = __atomic_load_n(&ref->Value, __ATOMIC_RELAXED);

    for (;;) {
        void *obj = ExpFastRefUnpackObject(cur);

        /* Compare the OBJECT identity, not the whole packed word: the swap must
         * succeed regardless of how many references happen to be cached on the
         * old object at the moment of the swap. */
        if (obj != old_object)
            return false;

        if (__atomic_compare_exchange_n(&ref->Value, &cur, desired, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (out_old_count)
                *out_old_count = ExpFastRefUnpackCount(cur);
            return true;
        }
        /* CAS failed -- `cur` reloaded; the object may have changed, retry the
         * identity check. */
    }
}

void *ExGetObjectFastReference(EX_FAST_REF *ref)
{
    /* Single atomic load -- a coherent snapshot of the pointer bits. The caller
     * is responsible for pinning the object (own reference or lock). */
    return ExpFastRefUnpackObject(__atomic_load_n(&ref->Value, __ATOMIC_ACQUIRE));
}
