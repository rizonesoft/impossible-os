/* ============================================================================
 * test_sched.c -- Scheduler / threading unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
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
}

#endif /* KERNEL_TESTS */
