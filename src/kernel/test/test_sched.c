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
    int tid = thread_create(test_thread_entry, NULL, 0);
    TEST_ASSERT(tid >= 0, "thread_create returns valid TID");
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

/* Registration */
void test_register_sched(void)
{
    test_suite_register_cat("Sched: create thread", test_sched_create_thread, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec_pending state", test_exec_pending_state, TEST_CAT_SCHED);
}

#endif /* KERNEL_TESTS */
