/* ============================================================================
 * seqlock.c -- Sequence lock implementation
 *
 * See seqlock.h for the full protocol description and usage examples.
 *
 * Implementation notes:
 *   - Writers use the internal spinlock for mutual exclusion.
 *     spin_lock_irqsave saves RFLAGS so seqlock_write_lock is safe from
 *     any context (thread or IRQ handler).  The saved flags live in
 *     seqlock_t.irq_flags.
 *   - The sequence counter is written with a store-release after each
 *     increment so readers that observe the new seq value also see all
 *     writes that preceded the increment.
 *   - Readers use load-acquire on the sequence counter.  The acquire
 *     barrier prevents the compiler/CPU from speculating data reads before
 *     the seq sample.
 *   - seqlock_read_retry inserts a read memory barrier (rmb) before the
 *     final seq comparison so all data reads are architecturally ordered
 *     before the comparison.  On x86 this is a compiler fence only (TSO
 *     guarantees load-load order); on weaker ISAs it would emit an LFENCE.
 * ============================================================================ */

#include "kernel/sched/seqlock.h"
#include "kernel/barrier.h"

/* ============================================================================
 * Initializer
 * ============================================================================ */

void seqlock_init(seqlock_t *sl)
{
    sl->lock.flag = 0;
    sl->seq = 0;
}

/* ============================================================================
 * Writer side
 * ============================================================================ */

/* Saved IRQ flags for the write-lock path.  Stored per-seqlock so
 * seqlock_write_unlock can restore them without a flag argument. */
static void seqlock_internal_lock(seqlock_t *sl)
{
    uint64_t flags;
    spin_lock_irqsave(&sl->lock, &flags);
    sl->irq_flags = flags;
}

static void seqlock_internal_unlock(seqlock_t *sl)
{
    uint64_t flags = sl->irq_flags;
    spin_unlock_irqrestore(&sl->lock, flags);
}

/* Acquire write-side lock.
 * Increments seq → odd (signals write in progress to readers). */
void seqlock_write_lock(seqlock_t *sl)
{
    seqlock_internal_lock(sl);
    /* Make the odd increment visible to readers before any data writes.
     * __ATOMIC_RELEASE pairs with readers' __ATOMIC_ACQUIRE on seq. */
    __atomic_add_fetch(&sl->seq, 1, __ATOMIC_RELEASE);
}

/* Release write-side lock.
 * Increments seq → even (signals write complete to readers). */
void seqlock_write_unlock(seqlock_t *sl)
{
    /* Make data writes visible before the even increment. */
    __atomic_add_fetch(&sl->seq, 1, __ATOMIC_RELEASE);
    seqlock_internal_unlock(sl);
}

/* ============================================================================
 * Reader side
 * ============================================================================ */

/* Begin a read-side critical section.
 * Spins until the sequence counter is even (no write in progress).
 * Returns the (even) sequence number -- pass this to seqlock_read_retry. */
uint64_t seqlock_read_begin(const seqlock_t *sl)
{
    uint64_t seq;

    for (;;) {
        seq = __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE);
        if ((seq & 1) == 0)   /* even → no write in progress */
            break;
        barrier();            /* prevent compiler optimising the spin away */
    }

    return seq;
}

/* Check for write conflict.
 * Returns 1 (retry) if the sequence counter changed since seqlock_read_begin,
 * meaning a write occurred during the read.
 * Insert rmb() before reading seq so all data reads complete first. */
int seqlock_read_retry(const seqlock_t *sl, uint64_t seq)
{
    rmb();   /* read barrier: complete data reads before final seq sample */
    return __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE) != seq;
}
