/* ============================================================================
 * test_sched.c -- Scheduler / threading unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/race_barrier.h"
#include "kernel/sched/task.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/dpc.h"
#include "kernel/smp.h"
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
    int i, fail_count = 0;

    g_thread_ran = 0;
    for (i = 0; i < THREAD_MAX + 4; i++) {
        int tid = kthread_create(test_thread_entry, NULL, 0);
        if (tid < 0) {
            fail_count++;
            break;
        }
        thread_join((uint32_t)tid);
    }
    /* One assertion covers all iterations -- avoids 20+ identical
     * [ OK ] lines on the serial log. */
    TEST_ASSERT_EQ(fail_count, 0,
                   "joined kthread slot reused past THREAD_MAX without failure");
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

/* The iteration loops accumulate counts and assert ONCE at the end.
 * Asserting per-iteration emits 100 identical [ OK ] lines on the
 * serial log per test -- noise that obscures real signal. The final
 * assertions still surface any failure via the exact count. */
static void test_sched_race_barrier_a_first(void)
{
    int hits = 0, setup_fail = 0;
    const int iterations = 100;
    int i;
    for (i = 0; i < iterations; i++) {
        int r = race_barrier_run_once(/*a_first=*/1);
        if (r < 0) setup_fail++;
        else if (r == 1) hits++;
    }
    TEST_ASSERT_EQ(setup_fail, 0,
                   "race_barrier_run_once succeeded every iteration (a_first=1)");
    TEST_ASSERT_EQ(hits, iterations,
                   "release(a_first=1) wakes A first on every iteration");
}

static void test_sched_race_barrier_b_first(void)
{
    int hits = 0, setup_fail = 0;
    const int iterations = 100;
    int i;
    for (i = 0; i < iterations; i++) {
        int r = race_barrier_run_once(/*a_first=*/0);
        if (r < 0) setup_fail++;
        else if (r == 1) hits++;
    }
    TEST_ASSERT_EQ(setup_fail, 0,
                   "race_barrier_run_once succeeded every iteration (a_first=0)");
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

/* Test: irql_to_tpr maps every IRQL band to the correct LAPIC TPR byte. */
static void test_irql_to_tpr_mapping(void)
{
    TEST_ASSERT_EQ(irql_to_tpr(PASSIVE_LEVEL), 0x00u, "PASSIVE -> TPR 0x00");
    TEST_ASSERT_EQ(irql_to_tpr(APC_LEVEL), 0x00u, "APC -> TPR 0x00");
    TEST_ASSERT_EQ(irql_to_tpr(DISPATCH_LEVEL), 0x20u, "DISPATCH -> TPR 0x20");
    TEST_ASSERT_EQ(irql_to_tpr(DIRQL_MIN), 0x20u, "DIRQL 3 -> TPR (3-1)<<4");
    TEST_ASSERT_EQ(irql_to_tpr((KIRQL)5), 0x40u, "DIRQL 5 -> TPR 0x40");
    TEST_ASSERT_EQ(irql_to_tpr(DIRQL_MAX), 0xD0u, "DIRQL 14 -> TPR 0xD0");
    TEST_ASSERT_EQ(irql_to_tpr(CLOCK_LEVEL), 0xFFu, "CLOCK -> TPR 0xFF");
    TEST_ASSERT_EQ(irql_to_tpr(HIGH_LEVEL), 0xFFu, "HIGH -> TPR 0xFF");
}

/* Test: the redundant-TPR-write skip (KeRaiseIrql/KeLowerIrql) relies on
 * adjacent IRQLs sharing a TPR class. Pin those collapses so the skip stays
 * correct if the TPR mapping is ever changed. */
static void test_irql_tpr_skip_invariant(void)
{
    TEST_ASSERT_EQ(irql_to_tpr(PASSIVE_LEVEL), irql_to_tpr(APC_LEVEL),
                   "PASSIVE and APC must share a TPR (skip precondition)");
    TEST_ASSERT_EQ(irql_to_tpr(CLOCK_LEVEL), irql_to_tpr(HIGH_LEVEL),
                   "CLOCK and HIGH must share TPR 0xFF");
    TEST_ASSERT_EQ(irql_to_tpr(IPI_LEVEL), irql_to_tpr(HIGH_LEVEL),
                   "IPI and HIGH must share TPR 0xFF");
}

/* Test: vector_to_irql maps hardware vectors to the right IRQL at every band
 * boundary (exception / ISA / device DIRQL / system). */
static void test_vector_to_irql_boundaries(void)
{
    TEST_ASSERT_EQ(vector_to_irql(0x1F), PASSIVE_LEVEL, "exception 0x1F -> PASSIVE");
    TEST_ASSERT_EQ(vector_to_irql(0x20), DISPATCH_LEVEL, "ISA 0x20 -> DISPATCH");
    TEST_ASSERT_EQ(vector_to_irql(0x2F), DISPATCH_LEVEL, "ISA 0x2F -> DISPATCH");
    TEST_ASSERT_EQ(vector_to_irql(0x30), DIRQL_MIN, "device 0x30 -> DIRQL 3");
    TEST_ASSERT_EQ(vector_to_irql(0xEF), DIRQL_MAX, "device 0xEF -> DIRQL 14");
    TEST_ASSERT_EQ(vector_to_irql(0xF0), HIGH_LEVEL, "system 0xF0 -> HIGH");
    TEST_ASSERT_EQ(vector_to_irql(0xFF), HIGH_LEVEL, "spurious 0xFF -> HIGH");
}

/* ---- DPC object + per-CPU queue tests ---- */

static void dpc_noop_routine(struct _KDPC *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
}

/* Test: KeInitializeDpc sets all KDPC fields to documented defaults. */
static void test_dpc_init_fields(void)
{
    KDPC dpc;
    KeInitializeDpc(&dpc, dpc_noop_routine, (void *)0x1234);
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)dpc.routine,
                   (uint64_t)(uintptr_t)dpc_noop_routine, "routine stored");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)dpc.deferred_ctx, 0x1234u, "context stored");
    TEST_ASSERT_EQ((uint64_t)dpc.queued, 0u, "not queued initially");
    TEST_ASSERT_EQ((uint64_t)dpc.cpu_target, (uint64_t)DPC_TARGET_CURRENT,
                   "target defaults to current CPU");
    TEST_ASSERT_EQ((uint64_t)dpc.importance, (uint64_t)MediumImportance,
                   "default importance is Medium");
    TEST_ASSERT_EQ((uint64_t)dpc.threaded, 0u, "not threaded by default");
}

/* Test: insert/remove return codes, depth accounting, and queued_cpu binding.
 * Runs at HIGH_LEVEL to block this CPU's timer-tick DPC drain so the live
 * per-CPU queue state is observed deterministically (per-CPU queue: no other
 * CPU touches it). */
static void test_dpc_insert_remove(void)
{
    KDPC dpc;
    struct dpc_queue *q = dpc_this_cpu_queue();
    KIRQL old;
    uint32_t my_cpu = smp_this_cpu()->cpu_id;
    int r1, r2, rm1, rm2;
    uint32_t qf1, qf2, qcpu, d0, d1, d2, d3;

    KeInitializeDpc(&dpc, dpc_noop_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    d0   = q->depth;
    r1   = KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    qf1  = dpc.queued;
    qcpu = dpc.queued_cpu;
    d1   = q->depth;
    r2   = KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    d2   = q->depth;
    rm1  = KeRemoveQueueDpc(&dpc);
    qf2  = dpc.queued;
    d3   = q->depth;
    rm2  = KeRemoveQueueDpc(&dpc);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)r1, 1u, "first insert returns 1 (newly queued)");
    TEST_ASSERT_EQ((uint64_t)qf1, 1u, "queued flag set");
    TEST_ASSERT_EQ((uint64_t)qcpu, (uint64_t)my_cpu, "queued_cpu bound to inserting CPU");
    TEST_ASSERT_EQ((uint64_t)d1, (uint64_t)(d0 + 1), "depth incremented");
    TEST_ASSERT_EQ((uint64_t)r2, 0u, "re-insert returns 0 (already queued)");
    TEST_ASSERT_EQ((uint64_t)d2, (uint64_t)(d0 + 1), "depth unchanged on re-insert");
    TEST_ASSERT_EQ((uint64_t)rm1, 1u, "remove returns 1");
    TEST_ASSERT_EQ((uint64_t)qf2, 0u, "queued flag cleared");
    TEST_ASSERT_EQ((uint64_t)d3, (uint64_t)d0, "depth restored");
    TEST_ASSERT_EQ((uint64_t)rm2, 0u, "remove un-queued returns 0");
}

/* Test: HighImportance DPC is head-inserted ahead of an earlier queued DPC. */
static void test_dpc_high_importance_head(void)
{
    KDPC dpc_med, dpc_high;
    struct dpc_queue *q = dpc_this_cpu_queue();
    KIRQL old;
    void *head_after;

    KeInitializeDpc(&dpc_med, dpc_noop_routine, (void *)0);
    KeInitializeDpc(&dpc_high, dpc_noop_routine, (void *)0);
    KeSetImportanceDpc(&dpc_high, HighImportance);

    KeRaiseIrql(HIGH_LEVEL, &old);
    KeInsertQueueDpc(&dpc_med, (void *)0, (void *)0);    /* tail */
    KeInsertQueueDpc(&dpc_high, (void *)0, (void *)0);   /* HighImportance -> head */
    head_after = (void *)q->head;
    KeRemoveQueueDpc(&dpc_high);
    KeRemoveQueueDpc(&dpc_med);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)(uintptr_t)head_after, (uint64_t)(uintptr_t)&dpc_high,
                   "HighImportance DPC head-inserted ahead of queued Medium");
}

/* DPC drain callback: records that it ran and at what IRQL. */
static volatile int      g_dpc_drain_ran;
static volatile uint32_t g_dpc_drain_irql;
static void dpc_drain_routine(struct _KDPC *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
    g_dpc_drain_irql = (uint32_t)KeGetCurrentIrql();
    g_dpc_drain_ran  = 1;
}

/* Test: KiDispatchDpc drains the queue and runs the callback at DISPATCH_LEVEL. */
static void test_dpc_drain_executes(void)
{
    KDPC dpc;
    g_dpc_drain_ran  = 0;
    g_dpc_drain_irql = 0xFF;
    KeInitializeDpc(&dpc, dpc_drain_routine, (void *)0);
    KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    /* Drain synchronously. (If a timer tick drained it first the callback has
     * already run -- either way it executes exactly once at DISPATCH_LEVEL.) */
    KiDispatchDpc();
    TEST_ASSERT_EQ((uint64_t)g_dpc_drain_ran, 1u, "DPC callback executed by drain");
    TEST_ASSERT_EQ((uint64_t)g_dpc_drain_irql, (uint64_t)DISPATCH_LEVEL,
                   "DPC callback ran at DISPATCH_LEVEL");
}

/* Test: KeFlushQueuedDpcs (PASSIVE_LEVEL caller) runs the DPC callback at
 * DISPATCH_LEVEL, not the caller's level. */
static void test_dpc_flush_runs_at_dispatch(void)
{
    KDPC dpc;
    g_dpc_drain_ran  = 0;
    g_dpc_drain_irql = 0xFF;
    KeInitializeDpc(&dpc, dpc_drain_routine, (void *)0);
    KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    KeFlushQueuedDpcs();   /* called from PASSIVE_LEVEL */
    TEST_ASSERT_EQ((uint64_t)g_dpc_drain_ran, 1u, "KeFlushQueuedDpcs ran the DPC callback");
    TEST_ASSERT_EQ((uint64_t)g_dpc_drain_irql, (uint64_t)DISPATCH_LEVEL,
                   "flush ran the callback at DISPATCH_LEVEL");
}

/* Registration */
void test_register_sched(void)
{
    test_suite_register_cat("Sched: DPC init fields",
                            test_dpc_init_fields, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC drain executes at DISPATCH_LEVEL",
                            test_dpc_drain_executes, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeFlushQueuedDpcs runs callback at DISPATCH",
                            test_dpc_flush_runs_at_dispatch, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC insert/remove + queued_cpu",
                            test_dpc_insert_remove, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC HighImportance head-insert",
                            test_dpc_high_importance_head, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: irql_to_tpr band mapping",
                            test_irql_to_tpr_mapping, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: irql TPR-skip invariant",
                            test_irql_tpr_skip_invariant, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: vector_to_irql boundaries",
                            test_vector_to_irql_boundaries, TEST_CAT_SCHED);
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
