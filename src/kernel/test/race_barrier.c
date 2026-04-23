/* ============================================================================
 * race_barrier.c -- Implementation of test_race_barrier_t.
 *
 * Design note: the 4 functions below implement the protocol documented in
 * include/kernel/test/race_barrier.h. Key invariants:
 *
 *   - arrive_a and arrive_b are symmetric. Each takes the spinlock only for
 *     the one-byte flag update; the wait runs without the lock held.
 *     Holding the spinlock across a yield would deadlock any other taker.
 *
 *   - Workers wait via event_wait_timeout(), NOT event_wait(). event_wait()
 *     in src/kernel/sched/event.c has a non-atomic check-enqueue-block
 *     sequence: between atomic_read(state)==0 and num_waiters++, an
 *     event_set() from another CPU that observes num_waiters==0 sets
 *     state=1 without calling wake_first_waiter. The worker then enters
 *     THREAD_BLOCKED via enqueue_and_block and never runs again -- a
 *     permanent hang. event_wait_timeout() uses a cooperative yield()
 *     (not THREAD_BLOCKED), so the thread stays READY across yields and
 *     observes the state change on the next re-read. The 10-second
 *     timeout is a safety net against a genuinely buggy release; on a
 *     healthy run the worker observes state=1 within one yield round.
 *     Fixing event_wait itself is out of scope here; the broader event
 *     subsystem tightening belongs with the kernel event APIs.
 *
 *   - release() polls both arrived flags under the spinlock. Polling with
 *     cooperative thread_yield() keeps the primitive symmetric and avoids
 *     the chicken-and-egg problem of needing a third event for driver-
 *     side waiting.
 *
 *   - thread_yield() between the two event_set() calls establishes
 *     scheduling order: the first-awakened worker actually RUNS past
 *     its barrier before the second is unblocked. On single-CPU or
 *     yield-aware round-robin schedulers this gives the deterministic
 *     post-checkpoint ordering the tests depend on (header documents
 *     the SMP caveat).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/race_barrier.h"
#include "kernel/sched/task.h"     /* thread_yield() */

/* Large-but-bounded wait timeout. 10 seconds is orders of magnitude
 * longer than any legitimate barrier rendezvous, so a timeout fire
 * indicates a test bug (missing release, never-arriving peer) rather
 * than scheduler pressure. */
#define TEST_RACE_BARRIER_WAIT_MS  10000u

/* Driver-side yield budget for release(). Each iteration of the poll
 * loop costs one thread_yield(); on a healthy 2-CPU system the arrival
 * flags go high within a handful of yields. 1,000,000 is far beyond any
 * legitimate wait (roughly 1 second of CPU time on a modern machine)
 * and exists purely to turn a worker-never-arrived scenario (scheduler
 * regression, kthread never dispatched, fault before arrive_a) into a
 * bounded test failure instead of an indefinite hang. Codex post-commit
 * review flagged the unbounded poll as [H]; this budget closes it. */
#define TEST_RACE_BARRIER_RELEASE_YIELDS  1000000u

void test_race_barrier_init(test_race_barrier_t *b)
{
    b->lock.flag = 0;
    event_init(&b->a_reached, "race_barrier_a", EVENT_AUTO_RESET, 0);
    event_init(&b->b_reached, "race_barrier_b", EVENT_AUTO_RESET, 0);
    b->a_arrived = 0;
    b->b_arrived = 0;
    b->release_timed_out = 0;
}

void test_race_barrier_arrive_a(test_race_barrier_t *b)
{
    uint64_t irq_flags;
    spin_lock_irqsave(&b->lock, &irq_flags);
    b->a_arrived = 1;
    spin_unlock_irqrestore(&b->lock, irq_flags);

    /* Lock is released BEFORE the wait -- event_wait_timeout() yields and
     * holding the lock across a yield deadlocks other waiters. See the
     * file header for why we use event_wait_timeout instead of event_wait
     * (lost-wakeup race in event_wait's check-enqueue-block sequence). */
    (void)event_wait_timeout(&b->a_reached, TEST_RACE_BARRIER_WAIT_MS);
}

void test_race_barrier_arrive_b(test_race_barrier_t *b)
{
    uint64_t irq_flags;
    spin_lock_irqsave(&b->lock, &irq_flags);
    b->b_arrived = 1;
    spin_unlock_irqrestore(&b->lock, irq_flags);

    (void)event_wait_timeout(&b->b_reached, TEST_RACE_BARRIER_WAIT_MS);
}

void test_race_barrier_release(test_race_barrier_t *b, int a_first)
{
    /* Bounded cooperative spin until both workers are parked on their
     * events. The lock is released around thread_yield() so workers can
     * take it for their own arrival updates. Yield budget turns a
     * worker-never-arrives scenario into a test failure rather than a
     * hang (see TEST_RACE_BARRIER_RELEASE_YIELDS rationale above). */
    uint32_t budget = TEST_RACE_BARRIER_RELEASE_YIELDS;
    int both_arrived = 0;

    while (budget > 0) {
        uint64_t irq_flags;
        spin_lock_irqsave(&b->lock, &irq_flags);
        both_arrived = b->a_arrived && b->b_arrived;
        spin_unlock_irqrestore(&b->lock, irq_flags);

        if (both_arrived)
            break;

        thread_yield();
        budget--;
    }

    if (!both_arrived) {
        /* Timeout: mark the barrier timed-out so the test driver can
         * assert on it after thread_join, then unconditionally fire
         * both release events. Workers that actually arrived (but
         * never got their wake because we timed out before event_set)
         * need to be released so the subsequent thread_join doesn't
         * deadlock. Workers that never arrived are unaffected -- they
         * were never parked in arrive_a/arrive_b and the event_set
         * here is a no-op from their point of view (AUTO_RESET state
         * latches for a future event_wait_timeout that never comes). */
        uint64_t irq_flags;
        spin_lock_irqsave(&b->lock, &irq_flags);
        b->release_timed_out = 1;
        spin_unlock_irqrestore(&b->lock, irq_flags);
        event_set(&b->a_reached);
        event_set(&b->b_reached);
        return;
    }

    /* Wake in chosen order. The yield between the two sets lets the first
     * woken worker advance past its barrier before the second worker is
     * unblocked -- that's what makes the ordering deterministic from the
     * test's point of view. */
    if (a_first) {
        event_set(&b->a_reached);
        thread_yield();
        event_set(&b->b_reached);
    } else {
        event_set(&b->b_reached);
        thread_yield();
        event_set(&b->a_reached);
    }
}

#endif /* KERNEL_TESTS */
