/* ============================================================================
 * test_sched.c -- Scheduler / threading unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/race_barrier.h"
#include "kernel/sched/task.h"
#include "kernel/types.h"

/* Shared flag set by test thread to prove it ran */
static volatile int g_thread_ran = 0;

/* Simple thread entry function */
static void test_thread_entry(void *arg)
{
    (void)arg;
    g_thread_ran = 1;
}

/* Test: create a thread and verify it was registered */
static void test_sched_create_thread(void)
{
    g_thread_ran = 0;
    int tid = kthread_create(test_thread_entry, NULL, 0);
    TEST_ASSERT(tid >= 0, "thread_create returns valid TID");
    if (tid >= 0)
        thread_join((uint32_t)tid);
}

/* Test: exec_pending field exists and is 0 for the current task (not exec'd) */
static void test_exec_pending_state(void)
{
    /* The test runner runs as PID 0 (main) which has never been exec'd.
     * exec_pending should be 0 and exec_pending_tick should be 0. */
    struct task *t = task_get_by_pid(0);
    if (!t) {
        TEST_SKIP("task_get_by_pid(0) returned NULL");
        return;
    }
    TEST_ASSERT_EQ(t->exec_pending, 0, "PID 0 exec_pending is 0 (not exec'd)");
    TEST_ASSERT_EQ(t->exec_pending_tick, 0, "PID 0 exec_pending_tick is 0");
}

/* Test: per-thread kernel_rsp mirrors task-level kernel_rsp for thread 0.
 * PID 0 uses the boot stack (both values are 0).
 * PID > 0 tasks created by task_create have a non-zero kernel_rsp. */
static void test_per_thread_kernel_rsp_mirror(void)
{
    struct task *t0 = task_get_by_pid(0);
    if (!t0) {
        TEST_SKIP("task_get_by_pid(0) returned NULL");
        return;
    }
    TEST_ASSERT_EQ(t0->threads[0].kernel_rsp, t0->kernel_rsp,
                   "PID 0 threads[0].kernel_rsp mirrors task kernel_rsp");

    /* Check a non-PID-0 task if one exists (DPC worker, work queue, etc.) */
    struct task *t1 = task_get_by_pid(1);
    if (t1 && t1->state != TASK_DEAD) {
        TEST_ASSERT_EQ(t1->threads[0].kernel_rsp, t1->kernel_rsp,
                       "PID 1 threads[0].kernel_rsp mirrors task");
        TEST_ASSERT(t1->kernel_rsp != 0,
                    "PID 1 kernel_rsp is non-zero (allocated stack)");
    }
}

/* Regression test for cooperative-yield fairness (fixed 2026-04-15).
 *
 * The bug: find_next_task() iterated tasks starting at from_task+1 and
 * tied same-priority candidates with strict '>'. When the caller
 * cooperatively yielded (RUNNING, not BLOCKED), the scheduler always
 * picked a thread in another task before reaching same-task siblings,
 * so a freshly-created kthread starved indefinitely. Symptom: ALPC
 * handshake tests timed out 5s waiting for a worker that never ran.
 *
 * This test is the canonical positive contract: a `while (!cond) yield()`
 * loop MUST be able to wait for a sibling kthread without thread_join. */
static volatile int g_yield_worker_ran = 0;

static void yield_fairness_worker(void *arg)
{
    (void)arg;
    g_yield_worker_ran = 1;
}

static void test_sched_cooperative_yield_fairness(void)
{
    g_yield_worker_ran = 0;
    int tid = kthread_create(yield_fairness_worker, NULL, 0);
    TEST_ASSERT(tid >= 0, "kthread_create succeeds");
    if (tid < 0)
        return;

    /* Bounded yield loop: 100k iterations is several orders of magnitude
     * more than needed if the scheduler is fair. If the worker never
     * runs, this exits with the assertion below catching it -- never
     * an infinite hang. */
    int i;
    for (i = 0; i < 100000 && !g_yield_worker_ran; i++)
        thread_yield();

    TEST_ASSERT(g_yield_worker_ran,
                "sibling kthread runs while caller cooperatively yields");

    /* Reap the worker so the thread slot is freed cleanly. */
    thread_join((uint32_t)tid);
}

static void test_sched_kthread_slot_reuse(void)
{
    int i;

    g_thread_ran = 0;
    for (i = 0; i < THREAD_MAX + 4; i++) {
        int tid = kthread_create(test_thread_entry, NULL, 0);
        TEST_ASSERT(tid >= 0, "joined kthread slot can be reused past THREAD_MAX");
        if (tid < 0)
            return;
        thread_join((uint32_t)tid);
    }
}

/* ---------------------------------------------------------------------------
 * Deterministic race-barrier tests -- see include/kernel/test/race_barrier.h.
 *
 * Two kthreads (A and B) reach a test_race_barrier_t and park on their
 * release events. The main test thread calls release(a_first) to choose
 * which worker runs its post-barrier branch first, then observes a
 * "winner" integer written by the awakened thread. Repeating the
 * rendezvous 100 times per direction catches scheduler drift that might
 * let the nominally-second thread sneak ahead on a lucky tick.
 * ------------------------------------------------------------------------- */

static test_race_barrier_t g_race_barrier;
/* Order-of-post-checkpoint log: first worker to write becomes winner=1
 * or winner=2. Written with simple volatile assignments; no atomics
 * needed because the release() -> yield -> release() sequence serialises
 * the two writes by construction. */
static volatile int g_race_barrier_winner;
static volatile int g_race_barrier_a_ran;
static volatile int g_race_barrier_b_ran;

static void race_barrier_worker_a(void *arg)
{
    (void)arg;
    test_race_barrier_arrive_a(&g_race_barrier);
    /* First worker to reach this line records the ordering. */
    if (g_race_barrier_winner == 0)
        g_race_barrier_winner = 1;
    g_race_barrier_a_ran = 1;
}

static void race_barrier_worker_b(void *arg)
{
    (void)arg;
    test_race_barrier_arrive_b(&g_race_barrier);
    if (g_race_barrier_winner == 0)
        g_race_barrier_winner = 2;
    g_race_barrier_b_ran = 1;
}

/* Run one rendezvous with the given release order. Returns 1 if the
 * observed winner matches the requested order, 0 if the ordering missed,
 * -1 on a setup failure (kthread_create shortage, worker never ran).
 *
 * Partial-create cleanup: if A creates but B fails, A is already parked
 * inside arrive_a() waiting on a_reached. We MUST signal a_reached
 * before joining A, otherwise thread_join hangs waiting on a worker
 * that has no path forward. Symmetric handling is unnecessary because
 * B is only created after A succeeded -- if B fails, A exists; if A
 * fails, we return immediately without creating B. */
static int race_barrier_run_once(int a_first)
{
    test_race_barrier_init(&g_race_barrier);
    g_race_barrier_winner = 0;
    g_race_barrier_a_ran = 0;
    g_race_barrier_b_ran = 0;

    int ta = kthread_create(race_barrier_worker_a, NULL, 0);
    if (ta < 0)
        return -1;

    int tb = kthread_create(race_barrier_worker_b, NULL, 0);
    if (tb < 0) {
        /* A is already parked inside arrive_a(). Unblock it so
         * thread_join() can reap the slot instead of deadlocking. */
        event_set(&g_race_barrier.a_reached);
        thread_join((uint32_t)ta);
        return -1;
    }

    test_race_barrier_release(&g_race_barrier, a_first);

    /* If release() timed out waiting for arrivals, both workers were
     * unconditionally signalled so join() won't deadlock -- but this
     * rendezvous is a failed setup, not a valid ordering check. */
    if (g_race_barrier.release_timed_out) {
        thread_join((uint32_t)ta);
        thread_join((uint32_t)tb);
        return -1;
    }

    thread_join((uint32_t)ta);
    thread_join((uint32_t)tb);

    if (!g_race_barrier_a_ran || !g_race_barrier_b_ran)
        return -1;

    int expected_winner = a_first ? 1 : 2;
    return g_race_barrier_winner == expected_winner;
}

static void test_sched_race_barrier_a_first(void)
{
    int hits = 0;
    const int iterations = 100;
    int i;
    for (i = 0; i < iterations; i++) {
        int r = race_barrier_run_once(/*a_first=*/1);
        TEST_ASSERT(r >= 0, "race_barrier_run_once did not hit -1 on any iteration");
        if (r == 1) hits++;
    }
    TEST_ASSERT_EQ(hits, iterations,
                   "release(a_first=1) wakes A first on every iteration");
}

static void test_sched_race_barrier_b_first(void)
{
    int hits = 0;
    const int iterations = 100;
    int i;
    for (i = 0; i < iterations; i++) {
        int r = race_barrier_run_once(/*a_first=*/0);
        TEST_ASSERT(r >= 0, "race_barrier_run_once did not hit -1 on any iteration");
        if (r == 1) hits++;
    }
    TEST_ASSERT_EQ(hits, iterations,
                   "release(a_first=0) wakes B first on every iteration");
}

/* Sanity check: barrier init leaves both arrival flags clear and both
 * events unsignalled so a second rendezvous on the same barrier starts
 * from a known-good state. Exercises init() in isolation. */
static void test_sched_race_barrier_init_clears_state(void)
{
    test_race_barrier_init(&g_race_barrier);
    TEST_ASSERT_EQ(g_race_barrier.a_arrived, 0,
                   "init clears a_arrived");
    TEST_ASSERT_EQ(g_race_barrier.b_arrived, 0,
                   "init clears b_arrived");
    TEST_ASSERT_EQ(g_race_barrier.release_timed_out, 0,
                   "init clears release_timed_out");
    TEST_ASSERT_EQ(event_is_set(&g_race_barrier.a_reached), 0,
                   "init leaves a_reached unsignalled");
    TEST_ASSERT_EQ(event_is_set(&g_race_barrier.b_reached), 0,
                   "init leaves b_reached unsignalled");
}

/* Registration */
void test_register_sched(void)
{
    test_suite_register_cat("Sched: create thread",
                            test_sched_create_thread, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec_pending state",
                            test_exec_pending_state, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: per-thread kernel_rsp mirror",
                            test_per_thread_kernel_rsp_mirror, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: cooperative-yield fairness (regression)",
                            test_sched_cooperative_yield_fairness,
                            TEST_CAT_SCHED);
    test_suite_register_cat("Sched: joined kthread slot reuse (regression)",
                            test_sched_kthread_slot_reuse, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: race-barrier release(a_first=1) wins 100x",
                            test_sched_race_barrier_a_first, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: race-barrier release(a_first=0) wins 100x",
                            test_sched_race_barrier_b_first, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: race-barrier init clears state",
                            test_sched_race_barrier_init_clears_state,
                            TEST_CAT_SCHED);
}

#endif /* KERNEL_TESTS */
