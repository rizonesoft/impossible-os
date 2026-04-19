/* ============================================================================
 * race_barrier.h -- Deterministic two-thread rendezvous primitive for tests.
 *
 * A test_race_barrier_t pins two cooperating kthreads at a named checkpoint
 * and lets the test decide which one resumes first. Concurrency tests can
 * then observe behaviour at a controlled interleaving instead of relying on
 * eventual ordering.
 *
 * Protocol (thread A and thread B, plus a third thread driving the test):
 *
 *   Thread A                Thread B                Test driver
 *   --------------------    --------------------    --------------------
 *   // code under test      // code under test      barrier_init(b)
 *   arrive_a(b):            arrive_b(b):            spawn(A); spawn(B)
 *     mark a_arrived          mark b_arrived        release(b, a_first=1)
 *     wait(a_reached)         wait(b_reached)        ...A resumes first...
 *   // post-checkpoint       // post-checkpoint      thread_join(A), join(B)
 *
 * Events (AUTO_RESET) serve as release signals: the test driver's release()
 * call is what unblocks each worker. The `a_arrived` / `b_arrived` flags are
 * the worker-to-driver arrival signal; release() polls them under the
 * spinlock and cooperatively yields until both workers are parked.
 *
 * Release ordering (best-effort, yield-based):
 *   release(b, 1)  -- signals a_reached, thread_yield(), signals b_reached.
 *   release(b, 0)  -- signals b_reached, thread_yield(), signals a_reached.
 *
 * The intermediate thread_yield() drops the driver thread off the current
 * CPU so the just-woken worker can actually RUN (not just sit on the run
 * queue) before the second worker is unblocked. On Impossible OS's
 * flat-cyclic round-robin scheduler, the yield reliably lets the woken
 * worker advance past its checkpoint first; the unit tests in test_sched.c
 * repeat the rendezvous 100x per direction to catch scheduler drift.
 *
 * SMP caveat: on true multi-CPU, the driver's thread_yield() only affects
 * the driver's CPU -- it does NOT synchronise with a worker placed on a
 * different CPU's run queue. The primitive therefore gives "empirically
 * deterministic" ordering under the current 2-CPU WHPX scheduler, not a
 * hard SMP guarantee. Tests that need strict post-checkpoint ordering
 * across CPUs should pair release() with thread_join() on the first
 * worker before signalling the second, or add a completion flag the
 * worker writes before returning. The 100-iteration count in the unit
 * tests is the empirical determinism check; a regression would surface
 * as sporadic order-inversions rather than a hang.
 *
 * Hazard:
 *   Do NOT hold another spinlock while calling arrive_a() or arrive_b().
 *   The internal wait yields; yielding with a caller-held spinlock
 *   deadlocks the system.
 *
 * Implementation note:
 *   arrive_a/arrive_b wait via event_wait_timeout() (cooperative poll),
 *   NOT event_wait(). event_wait() has a check-enqueue-block race in
 *   src/kernel/sched/event.c that can strand a worker as THREAD_BLOCKED
 *   if the driver's event_set() lands between the state check and the
 *   waiter enqueue. The cooperative poll sidesteps that window; the
 *   10-second timeout is a safety net against a never-releasing driver.
 *
 * Compiled only under KERNEL_TESTS -- release builds drop the entire
 * translation unit (see src/kernel/test/race_barrier.c).
 * ============================================================================ */

#pragma once

#ifdef KERNEL_TESTS

#include "kernel/types.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/event.h"

typedef struct test_race_barrier {
    spinlock_t lock;
    event_t    a_reached;          /* AUTO_RESET; release() sets this to wake A */
    event_t    b_reached;          /* AUTO_RESET; release() sets this to wake B */
    uint8_t    a_arrived;          /* 1 after thread A calls arrive_a() */
    uint8_t    b_arrived;          /* 1 after thread B calls arrive_b() */
    uint8_t    release_timed_out;  /* 1 if release() exhausted its yield budget */
} test_race_barrier_t;

/* Initialise both events as AUTO_RESET, clear arrival flags, reset lock. */
void test_race_barrier_init(test_race_barrier_t *b);

/* Thread A: mark arrived, wait for release to signal a_reached. */
void test_race_barrier_arrive_a(test_race_barrier_t *b);

/* Thread B: mark arrived, wait for release to signal b_reached. */
void test_race_barrier_arrive_b(test_race_barrier_t *b);

/* Test driver: spin (cooperative yield) until both flags are set, then wake
 * the workers in the chosen order.
 *   a_first != 0: signal a_reached, yield, signal b_reached.
 *   a_first == 0: signal b_reached, yield, signal a_reached.
 */
void test_race_barrier_release(test_race_barrier_t *b, int a_first);

#endif /* KERNEL_TESTS */
