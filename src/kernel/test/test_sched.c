/* ============================================================================
 * test_sched.c -- Scheduler / threading unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/race_barrier.h"
#include "kernel/sched/task.h"
#include "kernel/idt.h"           /* struct interrupt_frame (exec frame handoff) */
#include "libc/string.h"          /* memset -- zero a local frame fixture */
#include "kernel/nt/filetime.h"   /* NS_PER_FILETIME_TICK, FILETIME_* (accounting math) */
#include "kernel/time/wall_clock.h" /* wall_clock_time_sourced (CreateTime stamp test) */
#include "kernel/sched/irql.h"
#include "kernel/sched/dpc.h"
#include "kernel/sched/ktimer.h"
#include "kernel/sched/kinterrupt.h"
#include "kernel/sched/apc.h"
#include "kernel/mm/pmm.h"        /* pmm_get_free_frames -- exec stack reclaim */
#include "kernel/mm/heap.h"       /* kmalloc -- exec staging-token release shape */
#include "kernel/mm/vmm.h"        /* vmm_install/uninstall_guard_page */
#include "kernel/irq.h"
#include "kernel/timer.h"
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

/* Test: the exec frame handoff is a pass-through when no exec is pending.
 *
 * Every syscall return on the INT 0x80 path now routes through
 * task_exec_take_pending_frame(). For all syscalls except a successful
 * SYS_EXEC the flag is clear and the helper MUST hand back the caller's own
 * frame byte-for-byte -- returning anything else would iretq the task from a
 * foreign (or NULL) frame. That pass-through is the property the other ~40
 * syscalls depend on, so it is the one worth pinning.
 *
 * No side effects on this path: with exec_pending == 0 the helper does a
 * single atomic load and returns, touching no TSS, MSR, or task state. The
 * pending branch cannot be unit-tested without programming TSS.rsp0 and
 * MSR_KERNEL_GS_BASE for real -- forbidden live-infrastructure mutation under
 * the test policy -- so it is covered by the user-mode fork+exec test
 * (test_process.exe) which regressed deterministically before this fix. */
static void test_exec_take_pending_frame_passthrough(void)
{
    struct task *t = task_current();
    struct interrupt_frame f;
    uint64_t got;

    if (!t) {
        TEST_SKIP("task_current() returned NULL");
        return;
    }
    if (t->exec_pending) {
        TEST_SKIP("current task has an exec pending");
        return;
    }

    /* Distinctive contents: proves the helper returns THIS frame, not merely
     * some non-NULL pointer, and that it leaves the frame unmodified. */
    memset(&f, 0, sizeof(f));
    f.rip = 0xDEAD0000BEEF1234ull;
    f.rsp = 0x0BADC0DE0000F00Dull;

    got = task_exec_take_pending_frame(&f);

    TEST_ASSERT_EQ(got, (uint64_t)(uintptr_t)&f,
                   "no exec pending: returns the caller's own frame");
    TEST_ASSERT_EQ(f.rip, 0xDEAD0000BEEF1234ull,
                   "no exec pending: frame rip left unmodified");
    TEST_ASSERT_EQ(f.rsp, 0x0BADC0DE0000F00Dull,
                   "no exec pending: frame rsp left unmodified");
    TEST_ASSERT_EQ(t->exec_pending, 0,
                   "no exec pending: flag still clear after the call");
}

/* Test: a task that has never exec'd carries no parked exec stack.
 *
 * task_exec parks the kernel stack it supersedes in stack_pending_free and
 * drains it at the next exec or at reap. A recycled task slot that inherited a
 * stale value here would double-free on its first exec, so "clear until an exec
 * puts something there" is the invariant worth pinning. PID 0 is the test
 * runner itself and has never been exec'd. */
static void test_exec_stack_pending_free_clear(void)
{
    struct task *t = task_get_by_pid(0);

    if (!t) {
        TEST_SKIP("task_get_by_pid(0) returned NULL");
        return;
    }
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)t->stack_pending_free, 0,
                   "PID 0 (never exec'd) has no parked exec stack");
}

/* Test: kernel stacks are never shared, and a parked exec stack never aliases
 * a live one.
 *
 * stack_pending_free holds the stack task_exec REPLACED; stack_base holds the
 * one it installed. If those ever named the same allocation, the drain at the
 * next exec would free the stack the task is running on and reap would free it
 * again. The same argument applies ACROSS tasks: two tasks sharing a kernel
 * stack, or one task's parked stack matching another's live stack, is the same
 * double-free with an extra step.
 *
 * Both fields are written together inside one interrupt-disabled publication
 * block, so the property holds at every instant and is checkable live. The
 * cross-task walk always has subjects (the test runner itself is a live task
 * with a stack), so this cannot pass vacuously. */
static void test_exec_stack_pointers_never_alias(void)
{
    uint32_t a, b;
    uint32_t live = 0;

    for (a = 0; a < TASK_MAX; a++) {
        struct task *ta = task_get_by_pid(a);
        if (!ta || !ta->stack_base)
            continue;
        live++;

        /* Within one task: the parked stack is never the live one. */
        if (ta->stack_pending_free)
            TEST_ASSERT(ta->stack_pending_free != ta->stack_base,
                        "parked exec stack never aliases its own live stack");

        for (b = a + 1; b < TASK_MAX; b++) {
            struct task *tb = task_get_by_pid(b);
            if (!tb || !tb->stack_base)
                continue;
            TEST_ASSERT(ta->stack_base != tb->stack_base,
                        "no two tasks share a kernel stack");
            if (ta->stack_pending_free)
                TEST_ASSERT(ta->stack_pending_free != tb->stack_base,
                            "a parked stack is never another task's live stack");
            if (tb->stack_pending_free)
                TEST_ASSERT(tb->stack_pending_free != ta->stack_base,
                            "a parked stack is never another task's live stack");
            if (ta->stack_pending_free && tb->stack_pending_free)
                TEST_ASSERT(ta->stack_pending_free != tb->stack_pending_free,
                            "two tasks never park the same stack");
        }
    }

    /* Proves the live-live comparison had subjects. It does NOT prove the
     * parked-stack comparisons ran: in steady state stack_pending_free is
     * already drained, so those branches are usually skipped. Observing a
     * parked stack requires driving a real exec, which a kernel unit test
     * cannot do -- a post-commit failure terminates the calling task by design
     * and the test runner IS a task. That coverage is owned by the
     * fault-injected exec-lifecycle item in the process-model-extensions
     * roadmap (exec commit point / kernel-stack reclamation). */
    TEST_ASSERT(live > 0, "task table has at least one task with a kernel stack");
}

/* Test: the exec kernel-stack reclamation rule returns every frame.
 *
 * The leak this section closes was arithmetic: task_exec allocates
 * TASK_STACK_SIZE/4096 + 1 frames (the +1 is the guard page BELOW the stack)
 * and reap freed only the latest such allocation, so each re-exec lost that
 * many frames permanently. This exercises the allocate/guard/free rule
 * task_free_kernel_stack() implements -- same page count, same
 * uninstall-guard-before-PMM-free order -- and asserts the free-frame count
 * comes back to exactly where it started. A missing uninstall or an off-by-one
 * page count shows up here as a non-zero delta.
 *
 * TEST-SIDE-EFFECT-ALLOWED: installs and then uninstalls one guard page on a
 * frame this test itself owns for the duration. Paired and self-contained --
 * no task, no boot infrastructure, and no state survives the test. */
static void test_exec_kernel_stack_reclaim_leak_free(void)
{
    uint32_t pages = (TASK_STACK_SIZE / 4096u) + 1u;   /* +1 = guard page */
    uint64_t before, after;
    uintptr_t base;

    before = pmm_get_free_frames();

    base = pmm_alloc_contiguous(pages);
    if (!base) {
        TEST_SKIP("pmm_alloc_contiguous for a task-stack-sized block failed");
        return;
    }
    TEST_ASSERT_EQ(pmm_get_free_frames(), before - pages,
                   "allocating a task kernel stack consumes stack+guard frames");

    vmm_install_guard_page(base, "GUARD: test exec stack reclaim");

    /* Hand it to the PRODUCTION helper, exactly as task_exec and task_cleanup
     * do -- stack_base is guard_base + 4096, and the helper is responsible for
     * uninstalling the guard and returning stack+guard frames. Calling the
     * real function is the point: a test that re-implemented the arithmetic
     * would stay green while the helper drifted. */
    task_test_free_kernel_stack((uint8_t *)(base + 4096u));

    after = pmm_get_free_frames();
    TEST_ASSERT_EQ(after, before,
                   "reclaiming a task kernel stack returns every frame");
}

/* ---- task_exec staging-buffer ownership token ---------------------------
 *
 * The token is what lets task_exec release the caller's staging buffer BEFORE
 * publication (once published, a tick can carry the task into the new image and
 * the caller never runs again). Both task_exec and the caller invoke the
 * release unconditionally, so the whole design rests on it running EXACTLY
 * once. These tests pin that directly rather than inferring it from an exec. */

static uint32_t s_staging_release_calls;

static void test_staging_counting_release(struct task_exec_staging *st)
{
    s_staging_release_calls++;
    if (st)
        st->ptr = (void *)0;
}

/* Test: the release runs exactly once no matter how many times it is invoked.
 * This is the double-free guard -- task_exec releases on the success path and
 * the caller releases again after the call, and only one of them may free. */
static void test_exec_staging_release_is_idempotent(void)
{
    struct task_exec_staging st;
    uint8_t dummy = 0;

    s_staging_release_calls = 0;
    st.release = test_staging_counting_release;
    st.ptr = &dummy;
    st.phys = 0;
    st.pages = 0;

    task_exec_staging_release(&st);
    TEST_ASSERT_EQ((uint64_t)s_staging_release_calls, 1,
                   "first staging release invokes the hook exactly once");

    /* The caller's post-call release, and then a paranoid third: both no-ops. */
    task_exec_staging_release(&st);
    task_exec_staging_release(&st);
    TEST_ASSERT_EQ((uint64_t)s_staging_release_calls, 1,
                   "repeat staging releases are no-ops (no double free)");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)st.release, 0,
                   "a consumed staging token clears its release hook");
}

/* Test: a token with nothing owed, and a NULL token, are both safe no-ops --
 * the shape a caller with a borrowed or static buffer passes. */
static void test_exec_staging_release_empty_is_safe(void)
{
    struct task_exec_staging st;

    s_staging_release_calls = 0;
    st.release = (task_exec_release_fn)0;
    st.ptr = (void *)0;
    st.phys = 0;
    st.pages = 0;

    task_exec_staging_release(&st);
    task_exec_staging_release((struct task_exec_staging *)0);
    TEST_ASSERT_EQ((uint64_t)s_staging_release_calls, 0,
                   "empty and NULL staging tokens release nothing");
}

/* Test: the shared kmalloc release hook actually RETURNS the allocation, not
 * merely clears the bookkeeping. Uses the PRODUCTION hook the three
 * kmalloc-shaped callers pass to task_exec.
 *
 * The heap-accounting assertion is the load-bearing one: asserting only that
 * ptr and release were cleared would stay green if the kfree were deleted and
 * the pointer nulled anyway, which is a leak that looks exactly like success. */
static void test_exec_staging_kfree_shape(void)
{
    struct task_exec_staging st;
    uint64_t before, after;
    void *p;

    before = heap_get_free();
    p = kmalloc(64);
    if (!p) {
        TEST_SKIP("kmalloc(64) failed");
        return;
    }
    TEST_ASSERT(heap_get_free() < before,
                "kmalloc consumed heap capacity (baseline is measurable)");

    st.release = task_exec_staging_kfree;
    st.ptr = p;
    st.phys = 0;
    st.pages = 0;

    task_exec_staging_release(&st);
    after = heap_get_free();

    TEST_ASSERT_EQ(after, before,
                   "kmalloc staging release returns the allocation to the heap");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)st.ptr, 0,
                   "kmalloc staging release clears the buffer pointer");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)st.release, 0,
                   "kmalloc staging release consumes the token");
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

/* Test: KeInitializeThreadedDpc sets threaded=1 and otherwise matches the
 * KeInitializeDpc field defaults. */
static void test_dpc_init_threaded(void)
{
    KDPC dpc;
    KeInitializeThreadedDpc(&dpc, dpc_noop_routine, (void *)0x55);
    TEST_ASSERT_EQ((uint64_t)dpc.threaded, 1u, "KeInitializeThreadedDpc sets threaded=1");
    TEST_ASSERT_EQ((uint64_t)dpc.queued, 0u, "threaded DPC not queued initially");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)dpc.deferred_ctx, 0x55u, "context stored");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)dpc.routine,
                   (uint64_t)(uintptr_t)dpc_noop_routine, "routine stored");
}

/* Test: dpc_start_threads brings up the all-CPU drain worker and is
 * CAS-idempotent (boot already started it; a second call must be a no-op). The
 * worker thread's stack is a permanent one-time allocation, not a leak -- and
 * is zero here when boot started it first, so TEST_LEAK_IGNORE only matters if
 * this test is the very first starter. */
static void test_dpc_worker_started(void)
{
    TEST_LEAK_IGNORE("threaded DPC worker is a permanent one-time global thread");
    dpc_start_threads();   /* idempotent: no-op if boot already started it */
    TEST_ASSERT_EQ((uint64_t)dpc_worker_started(), 1u,
                   "dpc_start_threads brings up the all-CPU drain worker");
}

/* Test: insert / re-insert / remove on the shared per-CPU DPC queue.
 *
 * Every observation goes through dpc_sample_queue(), which reads the depth
 * counter AND walks the linked contents in ONE DPC_QLOCK critical section.
 * That is what makes the assertions interference-proof: any CPU may insert
 * into this queue (dpc_insert_core lets a remote producer target CPU 0, which
 * is where this test runs) and raising local IRQL to HIGH_LEVEL stops only
 * THIS CPU's drain, so exact deltas between separately-read depth samples
 * verify nothing. Two things are checked instead, both consistent by
 * construction: depth == list_len inside each snapshot (the depth-bookkeeping
 * invariant), and the occurrence count of the NAMED KDPC on the list, which no
 * remote producer can disturb. dpc.queued / dpc.queued_cpu are read from the
 * KDPC directly -- the test owns that stack object exclusively, and those
 * fields belong to the owning queue's lock, not necessarily this CPU's. */
static void test_dpc_insert_remove(void)
{
    KDPC dpc;
    KIRQL old;
    uint32_t my_cpu = smp_this_cpu()->cpu_id;
    int r1, r2, rm1, rm2;
    int ok0, ok1, ok2, ok3;
    uint32_t qf1, qf2, qcpu;
    struct dpc_queue_sample s0 = {0, 0, 0, 0, 0}, s1 = {0, 0, 0, 0, 0};
    struct dpc_queue_sample s2 = {0, 0, 0, 0, 0}, s3 = {0, 0, 0, 0, 0};

    KeInitializeDpc(&dpc, dpc_noop_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    ok0  = dpc_sample_queue(my_cpu, &dpc, &s0);
    r1   = KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    qf1  = dpc.queued;
    qcpu = dpc.queued_cpu;
    ok1  = dpc_sample_queue(my_cpu, &dpc, &s1);
    r2   = KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    ok2  = dpc_sample_queue(my_cpu, &dpc, &s2);
    rm1  = KeRemoveQueueDpc(&dpc);
    qf2  = dpc.queued;
    ok3  = dpc_sample_queue(my_cpu, &dpc, &s3);
    rm2  = KeRemoveQueueDpc(&dpc);
    KeLowerIrql(old);

    /* A truncated walk counted fewer entries than the list holds, so its
     * list_len says nothing about depth. Truncation is its own assertion, and
     * because TEST_ASSERT_EQ RECORDS a failure and returns rather than aborting,
     * every depth comparison below is additionally guarded on !truncated -- a
     * legitimately deep ambient queue must not manufacture a depth-accounting
     * failure, which is the exact interference this section exists to remove. */
    TEST_ASSERT_EQ((uint64_t)ok0, 1u, "snapshot before insert taken");
    TEST_ASSERT_EQ((uint64_t)ok1, 1u, "snapshot after insert taken");
    TEST_ASSERT_EQ((uint64_t)ok2, 1u, "snapshot after re-insert taken");
    TEST_ASSERT_EQ((uint64_t)ok3, 1u, "snapshot after remove taken");
    TEST_ASSERT_EQ((uint64_t)s0.truncated, 0u, "snapshot 0 walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)s1.truncated, 0u, "snapshot 1 walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)s2.truncated, 0u, "snapshot 2 walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)s3.truncated, 0u, "snapshot 3 walked the whole queue");

    /* Depth bookkeeping: counter and contents were read under one lock, so
     * this holds at every sample regardless of unrelated queue traffic. */
    TEST_ASSERT_EQ((uint64_t)(s0.truncated || s0.depth == s0.list_len), 1u,
                   "depth matches linked contents before insert");
    TEST_ASSERT_EQ((uint64_t)(s1.truncated || s1.depth == s1.list_len), 1u,
                   "depth matches linked contents after insert");
    TEST_ASSERT_EQ((uint64_t)(s2.truncated || s2.depth == s2.list_len), 1u,
                   "depth matches linked contents after re-insert");
    TEST_ASSERT_EQ((uint64_t)(s3.truncated || s3.depth == s3.list_len), 1u,
                   "depth matches linked contents after remove");

    /* Contract of the NAMED KDPC -- unperturbable by remote producers. */
    TEST_ASSERT_EQ((uint64_t)s0.occurrences, 0u, "not linked before insert");
    TEST_ASSERT_EQ((uint64_t)r1, 1u, "first insert returns 1 (newly queued)");
    TEST_ASSERT_EQ((uint64_t)qf1, 1u, "queued flag set");
    TEST_ASSERT_EQ((uint64_t)qcpu, (uint64_t)my_cpu, "queued_cpu bound to inserting CPU");
    TEST_ASSERT_EQ((uint64_t)s1.occurrences, 1u, "linked exactly once after insert");
    TEST_ASSERT_EQ((uint64_t)r2, 0u, "re-insert returns 0 (already queued)");
    TEST_ASSERT_EQ((uint64_t)s2.occurrences, 1u,
                   "still linked exactly once after re-insert (no double-enqueue)");
    TEST_ASSERT_EQ((uint64_t)rm1, 1u, "remove returns 1");
    TEST_ASSERT_EQ((uint64_t)qf2, 0u, "queued flag cleared");
    TEST_ASSERT_EQ((uint64_t)s3.occurrences, 0u, "unlinked after remove");
    TEST_ASSERT_EQ((uint64_t)rm2, 0u, "remove un-queued returns 0");
}

/* Bounded drain attempts in test_dpc_drain_depth_accounting(). One
 * KiDispatchDpc() runs at most DPC_BATCH_LIMIT callbacks, and unrelated
 * producers may have queued entries ahead of ours. */
#define DPC_DRAIN_TEST_ROUNDS  8

/* Test: the pop-side depth decrement is accounted for.
 *
 * The insert/remove test above never reaches drain_queue(), so on its own it
 * leaves the pop-side decrement unproven -- breaking it would strand q->depth
 * above the real queue length, suppressing deep idle and falsifying the
 * watchdog's depth reporting while every other DPC test still passed. Sampling
 * after a drain closes that: depth and the linked contents are read under one
 * lock, so depth == list_len is a direct check on the decrement at the pop.
 *
 * The PRE-drain snapshot is what stops this being vacuous. Without it, an
 * insert that silently failed would leave the named DPC absent and unqueued,
 * the ambient queue would still satisfy depth == list_len, and every assertion
 * would pass without the drain ever touching our KDPC. That snapshot is taken
 * at HIGH_LEVEL so a timer tick cannot drain the evidence away first. */
static void test_dpc_drain_depth_accounting(void)
{
    KDPC dpc;
    struct dpc_queue_sample sp = {0, 0, 0, 0, 0}, s = {0, 0, 0, 0, 0};
    uint32_t my_cpu = smp_this_cpu()->cpu_id;
    KIRQL old;
    int r, okp, ok = 0, rounds, leftover;
    uint32_t qp;

    KeInitializeDpc(&dpc, dpc_noop_routine, (void *)0);
    /* HighImportance HEAD-inserts (dpc_insert_core), so the fixture is the next
     * node the drain pops whatever the ambient queue holds. Tail-inserting it
     * made the test depend on ambient depth: one drain round runs at most
     * DPC_BATCH_LIMIT callbacks, so a legal queue deep enough would leave the
     * fixture behind the frontier and fail a test about depth accounting for a
     * reason that has nothing to do with depth accounting. */
    KeSetImportanceDpc(&dpc, HighImportance);

    KeRaiseIrql(HIGH_LEVEL, &old);
    r   = KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    okp = dpc_sample_queue(my_cpu, &dpc, &sp);
    qp  = dpc.queued;
    KeLowerIrql(old);

    for (rounds = 0; rounds < DPC_DRAIN_TEST_ROUNDS; rounds++) {
        KiDispatchDpc();
        ok = dpc_sample_queue(my_cpu, &dpc, &s);
        if (ok && !s.truncated && !s.occurrences)
            break;
    }

    /* Clean up BEFORE asserting. A failed TEST_ASSERT_EQ records and continues,
     * so an exhausted retry budget would otherwise return with this stack-owned
     * KDPC still linked in the live queue -- a later drain would then follow a
     * pointer into a dead frame. In the expected case the drain already took
     * it and this is a no-op returning 0. */
    leftover = KeRemoveQueueDpc(&dpc);

    TEST_ASSERT_EQ((uint64_t)r, 1u, "drain: DPC was actually queued");
    TEST_ASSERT_EQ((uint64_t)okp, 1u, "drain: pre-drain snapshot taken");
    TEST_ASSERT_EQ((uint64_t)sp.truncated, 0u,
                   "drain: pre-drain snapshot walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)sp.occurrences, 1u, "drain: linked exactly once before drain");
    TEST_ASSERT_EQ((uint64_t)qp, 1u, "drain: queued flag set before drain");
    TEST_ASSERT_EQ((uint64_t)(sp.truncated || sp.depth == sp.list_len), 1u,
                   "drain: depth matches linked contents before drain");
    TEST_ASSERT_EQ((uint64_t)ok, 1u, "drain: post-drain snapshot taken");
    TEST_ASSERT_EQ((uint64_t)s.truncated, 0u,
                   "drain: post-drain snapshot walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)s.occurrences, 0u, "drain: DPC left the queue");
    TEST_ASSERT_EQ((uint64_t)leftover, 0u, "drain: nothing left to clean up");
    TEST_ASSERT_EQ((uint64_t)dpc.queued, 0u, "drain: queued flag cleared");
    TEST_ASSERT_EQ((uint64_t)(s.truncated || s.depth == s.list_len), 1u,
                   "drain: depth matches linked contents after drain (pop decrement)");
}

/* Records that a threaded callback ran AND the IRQL it ran at. The IRQL is the
 * load-bearing half: a threaded DPC runs at PASSIVE_LEVEL in the worker, so a
 * callback observed at DISPATCH_LEVEL means the drain skipped the hand-off and
 * ran it inline -- a regression that mere absence from the normal queue cannot
 * distinguish from a correct hand-off. */
static volatile int      s_threaded_cancel_ran;
static volatile uint32_t s_threaded_cancel_irql;
static void dpc_threaded_cancel_routine(struct _KDPC *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
    s_threaded_cancel_irql = (uint32_t)KeGetCurrentIrql();
    s_threaded_cancel_ran  = 1;
}

/* Test: the threaded handoff decrements the NORMAL queue's depth as it moves
 * the KDPC onto the threaded list. It is the same pop-side site the plain
 * drain uses, but the handoff deliberately leaves the KDPC OWNED (queued=1) on
 * a second list, so a regression that unlinked without decrementing would
 * strand depth above the normal list length while ownership still looked
 * correct -- and neither the plain drain test nor the existing threaded
 * cancellation test would notice. Raising to DISPATCH_LEVEL keeps THIS CPU's
 * drain out of the way but does NOT exclude the all-CPU PASSIVE worker, so the
 * test tolerates losing the cancel race and flushes instead -- the stack-owned
 * KDPC must never outlive its frame while still reachable. */
static void test_dpc_threaded_handoff_depth(void)
{
    extern uint32_t dpc_drain_current_cpu(void);
    KDPC dpc;
    struct dpc_queue_sample s = {0, 0, 0, 0, 0};
    uint32_t my_cpu = smp_this_cpu()->cpu_id;
    KIRQL old;
    int r, ok = 0, rm, rounds;

    s_threaded_cancel_ran  = 0;
    s_threaded_cancel_irql = 0xFFu;
    KeInitializeThreadedDpc(&dpc, dpc_threaded_cancel_routine, (void *)0);
    /* HEAD-insert: one drain round runs at most DPC_BATCH_LIMIT callbacks, so a
     * tail-inserted fixture behind a legal ambient queue would never be handed
     * off and the occurrences==0 assertion below would fail for a reason that
     * has nothing to do with depth accounting. */
    KeSetImportanceDpc(&dpc, HighImportance);

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    r  = KeInsertQueueDpc(&dpc, (void *)0, (void *)0);  /* -> normal queue */
    for (rounds = 0; rounds < DPC_DRAIN_TEST_ROUNDS; rounds++) {
        dpc_drain_current_cpu();                        /* -> threaded list */
        ok = dpc_sample_queue(my_cpu, &dpc, &s);
        if (ok && !s.truncated && !s.occurrences)
            break;
    }
    rm = KeRemoveQueueDpc(&dpc);      /* may LOSE to the worker on another CPU */
    KeLowerIrql(old);

    /* The threaded worker is ONE all-CPU PASSIVE thread, so raising THIS CPU's
     * IRQL does not exclude it: on a multi-CPU boot it can pop the node off the
     * threaded list before the cancel above, clearing dpc.queued underneath the
     * read. Both outcomes are legal, so neither is asserted -- what must hold is
     * that this stack-owned KDPC is unreachable before the frame dies, which
     * means waiting for any in-flight threaded callback when the cancel lost. */
    if (!rm)
        KeFlushQueuedDpcs();

    TEST_ASSERT_EQ((uint64_t)r, 1u, "threaded: DPC was actually queued");
    TEST_ASSERT_EQ((uint64_t)ok, 1u, "threaded: snapshot taken");
    TEST_ASSERT_EQ((uint64_t)s.truncated, 0u, "threaded: snapshot walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)s.occurrences, 0u,
                   "threaded: off the normal queue after handoff");
    TEST_ASSERT_EQ((uint64_t)(s.threaded_occurrences == 1u || s_threaded_cancel_ran == 1),
                   1u, "threaded: the decremented node went to the threaded list, not nowhere");
    TEST_ASSERT_EQ((uint64_t)(s.truncated || s.depth == s.list_len), 1u,
                   "threaded: normal-queue depth matches contents after handoff");
    TEST_ASSERT_EQ((uint64_t)dpc.queued, 0u,
                   "threaded: owned by nobody once cancelled or run");
    /* dpc_in_flight_threaded() is a single GLOBAL counter, so it is only this
     * test's business on the path where this test just flushed. */
    if (!rm)
        TEST_ASSERT_EQ((uint64_t)dpc_in_flight_threaded(), 0u,
                       "threaded: no threaded callback left in flight after the flush");
}

/* Sentinel written into a dpc_queue_sample before a call that must be refused,
 * so "left untouched" is checkable rather than assumed. */
#define DPC_SAMPLE_SENTINEL  0xEEu

/* Test: dpc_sample_queue() argument and scoping contract. An invalid cpu_id or
 * a NULL out pointer is refused with the caller's buffer untouched; a NULL
 * KDPC still yields a valid queue snapshot with no occurrences; and a KDPC
 * queued on THIS CPU is not counted on another CPU's queue -- occurrences is a
 * membership count for the sampled queue only, never a global "is it queued"
 * answer. */
static void test_dpc_sample_queue_contract(void)
{
    KDPC dpc;
    struct dpc_queue_sample s = {DPC_SAMPLE_SENTINEL, DPC_SAMPLE_SENTINEL,
                                 DPC_SAMPLE_SENTINEL, DPC_SAMPLE_SENTINEL,
                                 DPC_SAMPLE_SENTINEL};
    struct dpc_queue_sample s_null  = {0, 0, 0, 0, 0};
    struct dpc_queue_sample s_mine  = {0, 0, 0, 0, 0};
    struct dpc_queue_sample s_other = {0, 0, 0, 0, 0};
    uint32_t my_cpu = smp_this_cpu()->cpu_id;
    uint32_t other  = MAX_CPUS;
    KIRQL old;
    int bad_cpu, bad_max, bad_out, ins, ok_mine, ok_null, ok_other = 0, cleanup = 0;
    uint32_t i;

    for (i = 0; i < MAX_CPUS; i++) {
        if (i != my_cpu) {
            other = i;      /* any other slot, online or not -- the queue and */
            break;          /* its lock exist for every slot from phase 1 on. */
        }
    }

    KeInitializeDpc(&dpc, dpc_noop_routine, (void *)0);

    bad_cpu = dpc_sample_queue(MAX_CPUS, &dpc, &s);
    bad_max = dpc_sample_queue(0xFFFFFFFFu, &dpc, &s);
    bad_out = dpc_sample_queue(my_cpu, &dpc, (struct dpc_queue_sample *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    ins     = KeInsertQueueDpc(&dpc, (void *)0, (void *)0);
    ok_mine = dpc_sample_queue(my_cpu, &dpc, &s_mine);
    ok_null = dpc_sample_queue(my_cpu, (const KDPC *)0, &s_null);
    if (other < MAX_CPUS)
        ok_other = dpc_sample_queue(other, &dpc, &s_other);
    cleanup = KeRemoveQueueDpc(&dpc);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)bad_cpu, 0u, "sample: cpu_id == MAX_CPUS refused");
    TEST_ASSERT_EQ((uint64_t)bad_max, 0u, "sample: cpu_id == UINT32_MAX refused");
    TEST_ASSERT_EQ((uint64_t)bad_out, 0u, "sample: NULL out pointer refused");
    TEST_ASSERT_EQ((uint64_t)s.depth, (uint64_t)DPC_SAMPLE_SENTINEL,
                   "sample: refused call left depth untouched");
    TEST_ASSERT_EQ((uint64_t)s.list_len, (uint64_t)DPC_SAMPLE_SENTINEL,
                   "sample: refused call left list_len untouched");
    TEST_ASSERT_EQ((uint64_t)s.occurrences, (uint64_t)DPC_SAMPLE_SENTINEL,
                   "sample: refused call left occurrences untouched");
    TEST_ASSERT_EQ((uint64_t)s.truncated, (uint64_t)DPC_SAMPLE_SENTINEL,
                   "sample: refused call left truncated untouched");
    TEST_ASSERT_EQ((uint64_t)s.threaded_occurrences, (uint64_t)DPC_SAMPLE_SENTINEL,
                   "sample: refused call left threaded_occurrences untouched");
    TEST_ASSERT_EQ((uint64_t)ins, 1u, "sample: fixture DPC was actually queued");
    TEST_ASSERT_EQ((uint64_t)ok_mine, 1u, "sample: own-CPU snapshot taken");
    TEST_ASSERT_EQ((uint64_t)s_mine.truncated, 0u,
                   "sample: own-CPU snapshot walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)s_mine.occurrences, 1u,
                   "sample: fixture DPC linked exactly once on its own CPU");
    TEST_ASSERT_EQ((uint64_t)ok_null, 1u, "sample: NULL dpc still snapshots the queue");
    TEST_ASSERT_EQ((uint64_t)s_null.occurrences, 0u, "sample: NULL dpc has no occurrences");
    TEST_ASSERT_EQ((uint64_t)s_null.truncated, 0u,
                   "sample: NULL-dpc snapshot walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)(s_null.truncated || s_null.depth == s_null.list_len), 1u,
                   "sample: NULL-dpc snapshot depth matches contents");
    /* No pre-seeded default: MAX_CPUS is 16, so a slot other than the caller's
     * always exists and the call must actually have been made. Seeding ok_other
     * to 1 would let this assertion pass with nothing sampled. */
    TEST_ASSERT_EQ((uint64_t)(other < MAX_CPUS), 1u, "sample: another CPU slot exists");
    TEST_ASSERT_EQ((uint64_t)ok_other, 1u, "sample: another CPU's slot snapshots");
    TEST_ASSERT_EQ((uint64_t)s_other.truncated, 0u,
                   "sample: another CPU's snapshot walked the whole queue");
    TEST_ASSERT_EQ((uint64_t)s_other.occurrences, 0u,
                   "sample: KDPC on this CPU is not counted on another CPU's queue");
    TEST_ASSERT_EQ((uint64_t)(s_other.truncated || s_other.depth == s_other.list_len), 1u,
                   "sample: another CPU's snapshot depth matches contents");
    /* The fixture is stack-owned: prove it left the queue before the frame does. */
    TEST_ASSERT_EQ((uint64_t)cleanup, 1u, "sample: fixture DPC unlinked before return");
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

/* Test: KDPC_IMPORTANCE numeric values match the WDK wdm.h ABI ordering, so a
 * binary/WDK-facing caller passing the raw Windows value (High == 2) gets the
 * head-insert and the rest tail-queue. Guards against the enum being reordered. */
static void test_dpc_importance_abi_values(void)
{
    TEST_ASSERT_EQ((uint64_t)LowImportance,        0u, "LowImportance == 0 (WDK)");
    TEST_ASSERT_EQ((uint64_t)MediumImportance,     1u, "MediumImportance == 1 (WDK)");
    TEST_ASSERT_EQ((uint64_t)HighImportance,       2u, "HighImportance == 2 (WDK)");
    TEST_ASSERT_EQ((uint64_t)MediumHighImportance, 3u, "MediumHighImportance == 3 (WDK)");
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

/* ---- Section 9: Timer-DPC association (kernel_timer_t) -------------------- */

static void ktimer_dpc_routine(struct _KDPC *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
}

/* Test: KeInitializeTimer field defaults. */
static void test_ktimer_init_fields(void)
{
    kernel_timer_t t;
    KeInitializeTimer(&t);
    TEST_ASSERT_EQ((uint64_t)t.active, 0u, "init: timer not active");
    TEST_ASSERT_EQ((uint64_t)t.cpu, (uint64_t)MAX_CPUS, "init: cpu invalid");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)t.dpc, 0u, "init: no dpc bound");
    TEST_ASSERT_EQ((uint64_t)t.period_ticks, 0u, "init: period 0");
}

/* Test: KeSetTimerEx arms on the BSP service CPU; KeCancelTimer returns prior
 * armed state and clears it; a second cancel returns 0. HIGH_LEVEL-protected
 * so the live timer ISR cannot scan/fire the list mid-test. */
static void test_ktimer_set_cancel(void)
{
    kernel_timer_t t;
    KDPC dpc;
    KIRQL old;
    uint32_t my_cpu = smp_this_cpu()->cpu_id;
    uint32_t armed, armed_cpu;
    int cancel_armed, cancel_idle;

    KeInitializeTimer(&t);
    KeInitializeDpc(&dpc, ktimer_dpc_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    KeSetTimerEx(&t, system_get_ticks() + 1000000ull, 0, &dpc);  /* far future */
    armed       = t.active;
    armed_cpu   = t.cpu;
    cancel_armed = KeCancelTimer(&t);
    cancel_idle  = KeCancelTimer(&t);
    KeLowerIrql(old);

    (void)my_cpu;
    TEST_ASSERT_EQ((uint64_t)armed, 1u, "KeSetTimerEx arms timer (active)");
    TEST_ASSERT_EQ((uint64_t)armed_cpu, 0u, "armed on BSP service CPU (0)");
    TEST_ASSERT_EQ((uint64_t)cancel_armed, 1u, "cancel of armed returns 1");
    TEST_ASSERT_EQ((uint64_t)t.active, 0u, "cancel clears active");
    TEST_ASSERT_EQ((uint64_t)cancel_idle, 0u, "cancel of idle returns 0");
}

/* Test: a due single-shot timer fires once -- queues its DPC and goes inactive.
 * Drives expiry deterministically (due == now) instead of waiting for ticks. */
static void test_ktimer_single_shot_fires(void)
{
    kernel_timer_t t;
    KDPC dpc;
    KIRQL old;
    uint32_t fired, queued_after, active_after;
    void *arg1_after, *arg2_after;

    KeInitializeTimer(&t);
    KeInitializeDpc(&dpc, ktimer_dpc_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    KeSetTimerEx(&t, system_get_ticks(), 0, &dpc);   /* due now */
    fired        = ktimer_expire_current_cpu();
    queued_after = dpc.queued;
    active_after = t.active;
    arg1_after   = dpc.system_arg1;                   /* must be the timer */
    arg2_after   = dpc.system_arg2;                   /* must be NULL */
    KeRemoveQueueDpc(&dpc);                            /* clean up queued DPC */
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)fired, 1u, "single-shot fired once");
    TEST_ASSERT_EQ((uint64_t)queued_after, 1u, "fired timer queued its DPC");
    TEST_ASSERT_EQ((uint64_t)active_after, 0u, "single-shot inactive after fire");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)arg1_after, (uint64_t)(uintptr_t)&t,
                   "DPC arg1 is the timer (KeInsertQueueDpcOnCpu service-CPU handoff)");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)arg2_after, 0u, "DPC arg2 is NULL");
}

/* Test: a periodic timer re-arms (stays active, due advanced past now) on fire. */
static void test_ktimer_periodic_rearm(void)
{
    kernel_timer_t t;
    KDPC dpc;
    KIRQL old;
    uint64_t now, due_after;
    uint32_t fired, active_after;

    KeInitializeTimer(&t);
    KeInitializeDpc(&dpc, ktimer_dpc_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    now          = system_get_ticks();
    KeSetTimerEx(&t, now, 5, &dpc);   /* due now, 5-tick period */
    fired        = ktimer_expire_current_cpu();
    active_after = t.active;
    due_after    = t.due_time_ticks;
    KeRemoveQueueDpc(&dpc);
    KeCancelTimer(&t);                 /* stop the periodic */
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)fired, 1u, "periodic fired once this pass");
    TEST_ASSERT_EQ((uint64_t)active_after, 1u, "periodic stays active (re-armed)");
    TEST_ASSERT(due_after > now, "periodic due advanced past now");
}

/* Test: a NULL-DPC timer fires (unlinks) without queueing a DPC or crashing. */
static void test_ktimer_null_dpc_fires(void)
{
    kernel_timer_t t;
    KIRQL old;
    uint32_t fired, active_after;

    KeInitializeTimer(&t);

    KeRaiseIrql(HIGH_LEVEL, &old);
    KeSetTimerEx(&t, system_get_ticks(), 0, (KDPC *)0);  /* due now, no DPC */
    fired        = ktimer_expire_current_cpu();
    active_after = t.active;
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)fired, 1u, "NULL-dpc single-shot still counts as fired");
    TEST_ASSERT_EQ((uint64_t)active_after, 0u, "NULL-dpc single-shot inactive after fire");
}

/* Test: a not-yet-due timer does NOT fire -- pins the `now >= due` guard so an
 * inverted comparison or unconditional expiry is caught. */
static void test_ktimer_future_no_fire(void)
{
    kernel_timer_t t;
    KDPC dpc;
    KIRQL old;
    uint32_t fired, queued_after, active_after;

    KeInitializeTimer(&t);
    KeInitializeDpc(&dpc, ktimer_dpc_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    KeSetTimerEx(&t, system_get_ticks() + 1000000ull, 0, &dpc);  /* far future */
    fired        = ktimer_expire_current_cpu();
    queued_after = dpc.queued;
    active_after = t.active;
    KeCancelTimer(&t);                                /* clean up */
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)fired, 0u, "future timer does not fire");
    TEST_ASSERT_EQ((uint64_t)queued_after, 0u, "future timer queues no DPC");
    TEST_ASSERT_EQ((uint64_t)active_after, 1u, "future timer stays active");
}

/* Test: re-arming an active timer via KeSetTimerEx replaces its DPC -- only the
 * new DPC is queued on the next expiry, the old one is not (no stale list
 * entry / double-link). */
static void test_ktimer_rearm_replaces_dpc(void)
{
    kernel_timer_t t;
    KDPC dpc_a, dpc_b;
    KIRQL old;
    uint32_t a_queued, b_queued, fired, active_after;

    KeInitializeTimer(&t);
    KeInitializeDpc(&dpc_a, ktimer_dpc_routine, (void *)0);
    KeInitializeDpc(&dpc_b, ktimer_dpc_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    KeSetTimerEx(&t, system_get_ticks() + 1000000ull, 0, &dpc_a);  /* future, DPC A */
    KeSetTimerEx(&t, system_get_ticks(), 0, &dpc_b);               /* re-arm due-now, DPC B */
    fired        = ktimer_expire_current_cpu();
    a_queued     = dpc_a.queued;
    b_queued     = dpc_b.queued;
    active_after = t.active;
    KeRemoveQueueDpc(&dpc_b);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)fired, 1u, "re-armed timer fires once");
    TEST_ASSERT_EQ((uint64_t)b_queued, 1u, "new DPC (B) queued on expiry");
    TEST_ASSERT_EQ((uint64_t)a_queued, 0u, "old DPC (A) NOT queued (replaced)");
    TEST_ASSERT_EQ((uint64_t)active_after, 0u, "single-shot inactive after fire");
}

/* Test: an overdue periodic timer re-arms in BOUNDED time (O(1), not one
 * iteration per missed period) and advances strictly past now -- pins the
 * fix for the unbounded catch-up loop. */
static void test_ktimer_overdue_periodic_bounded(void)
{
    kernel_timer_t t;
    KDPC dpc;
    KIRQL old;
    uint64_t now, due_after;
    uint32_t fired, active_after;

    KeInitializeTimer(&t);
    KeInitializeDpc(&dpc, ktimer_dpc_routine, (void *)0);

    KeRaiseIrql(HIGH_LEVEL, &old);
    now = system_get_ticks();
    /* Far overdue with period 1: the old do/while would spin `now` times under
     * the lock; the O(1) re-arm advances once to now+1. */
    KeSetTimerEx(&t, (now > 100000ull) ? (now - 100000ull) : 0, 1, &dpc);
    fired        = ktimer_expire_current_cpu();
    active_after = t.active;
    due_after    = t.due_time_ticks;
    KeRemoveQueueDpc(&dpc);
    KeCancelTimer(&t);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)fired, 1u, "overdue periodic fires exactly once");
    TEST_ASSERT_EQ((uint64_t)active_after, 1u, "overdue periodic re-armed (active)");
    TEST_ASSERT(due_after > now, "overdue periodic due advanced strictly past now");
}

/* Test: a timer-associated DPC targeted (via KeSetTargetProcessorDpc) to a
 * non-service CPU is normalized to the service CPU at expiry, so it lands on a
 * queue that actually drains -- AP DPC queues have no guaranteed drain trigger
 * yet, so honoring an AP target would strand the DPC (lost-wakeup). */
static void test_ktimer_dpc_target_normalized(void)
{
    kernel_timer_t t;
    KDPC dpc;
    KIRQL old;
    uint32_t fired, queued_cpu_after, target_after;

    KeInitializeTimer(&t);
    KeInitializeDpc(&dpc, ktimer_dpc_routine, (void *)0);
    KeSetTargetProcessorDpc(&dpc, 1);   /* caller targets a non-service CPU */

    KeRaiseIrql(HIGH_LEVEL, &old);
    KeSetTimerEx(&t, system_get_ticks(), 0, &dpc);   /* due now */
    fired            = ktimer_expire_current_cpu();
    queued_cpu_after = dpc.queued_cpu;
    target_after     = dpc.cpu_target;               /* must be UNMUTATED */
    KeRemoveQueueDpc(&dpc);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)fired, 1u, "targeted-DPC timer fired");
    TEST_ASSERT_EQ((uint64_t)queued_cpu_after, 0u,
                   "timer DPC pinned to service CPU (not the AP target)");
    TEST_ASSERT_EQ((uint64_t)target_after, 1u,
                   "caller cpu_target preserved (not mutated by ktimer)");
}

/* ---- Section 10: KINTERRUPT / KeSynchronizeExecution + ISR DPC helper ---- */

static volatile int      g_kisync_ran;
static volatile uint32_t g_kisync_irql;
static int kisync_routine(void *ctx)
{
    g_kisync_irql = (uint32_t)KeGetCurrentIrql();
    g_kisync_ran  = 1;
    return (int)(uintptr_t)ctx;   /* propagate a marker as the BOOLEAN result */
}

/* Test: KeInitializeInterrupt field defaults. */
static void test_kinterrupt_init_fields(void)
{
    KINTERRUPT ki;
    KeInitializeInterrupt(&ki, 0x40);
    TEST_ASSERT_EQ((uint64_t)ki.vector, 0x40u, "init: vector stored");
    TEST_ASSERT_EQ((uint64_t)ki.active_cpu, (uint64_t)MAX_CPUS, "init: no ISR active");
    TEST_ASSERT_EQ((uint64_t)ki.sync_irql, (uint64_t)vector_to_irql(0x40),
                   "init: sync_irql = vector DIRQL");
    TEST_ASSERT_EQ((uint64_t)ki.connected, 0u, "init: not connected");
}

/* Test: KeSynchronizeExecution runs the routine at SynchronizeIrql under the
 * lock, propagates its BOOLEAN result, and restores active_cpu. */
static void test_kinterrupt_synchronize_runs(void)
{
    KINTERRUPT ki;
    int ret;
    KeInitializeInterrupt(&ki, 0x40);
    g_kisync_ran = 0; g_kisync_irql = 0xFF;
    ret = KeSynchronizeExecution(&ki, kisync_routine, (void *)(uintptr_t)7);
    TEST_ASSERT_EQ((uint64_t)g_kisync_ran, 1u, "KeSynchronizeExecution ran the routine");
    TEST_ASSERT_EQ((uint64_t)ret, 7u, "routine BOOLEAN result propagated");
    TEST_ASSERT_EQ((uint64_t)g_kisync_irql, (uint64_t)vector_to_irql(0x40),
                   "routine ran at SynchronizeIrql (DIRQL)");
    TEST_ASSERT_EQ((uint64_t)ki.active_cpu, (uint64_t)MAX_CPUS, "active_cpu restored");
}

/* Test: a call from inside the interrupt's own ISR (active_cpu == this CPU) is
 * rejected (returns FALSE) instead of deadlocking, and does NOT run the routine. */
static void test_kinterrupt_self_isr_rejected(void)
{
    KINTERRUPT ki;
    int ret;
    uint32_t rejects_before, rejects_after;
    KeInitializeInterrupt(&ki, 0x40);
    ki.active_cpu = smp_this_cpu()->cpu_id;   /* simulate "in this vector's ISR" */
    g_kisync_ran = 0;
    rejects_before = KeGetSelfIsrRejectCount();
    ret = KeSynchronizeExecution(&ki, kisync_routine, (void *)(uintptr_t)7);
    rejects_after = KeGetSelfIsrRejectCount();
    TEST_ASSERT_EQ((uint64_t)ret, 0u, "self-ISR call rejected (returns FALSE)");
    TEST_ASSERT_EQ((uint64_t)g_kisync_ran, 0u, "routine NOT run on self-ISR reject");
    TEST_ASSERT_EQ((uint64_t)rejects_after, (uint64_t)(rejects_before + 1),
                   "self-ISR reject bumps the lock-free diagnostic counter (no klog)");
}

/* Test: KeRequestDpcFromIsr enqueues the DPC (thin KeInsertQueueDpc wrapper). */
static void test_kerequestdpc_enqueues(void)
{
    KDPC dpc;
    KIRQL old;
    uint32_t queued;
    KeInitializeDpc(&dpc, dpc_noop_routine, (void *)0);
    KeRaiseIrql(HIGH_LEVEL, &old);
    KeRequestDpcFromIsr(&dpc, (void *)0, (void *)0);
    queued = dpc.queued;
    KeRemoveQueueDpc(&dpc);
    KeLowerIrql(old);
    TEST_ASSERT_EQ((uint64_t)queued, 1u, "KeRequestDpcFromIsr enqueues the DPC");
}

static void kinterrupt_dummy_irq(uint8_t v, void *c) { (void)v; (void)c; }

/* Test: KeConnectInterrupt binds only an EXCLUSIVE OWNED vector under the route
 * lock -- an unowned (no-handler) vector is rejected; a second DIFFERENT
 * KINTERRUPT on the same vector is rejected; re-binding the SAME object is
 * idempotent. Registers a dummy handler on a high unused vector, then cleans up. */
static void test_kinterrupt_bind_exclusive(void)
{
    KINTERRUPT ki_a, ki_b;
    int unowned, c1, c2, c3;
    KeInitializeInterrupt(&ki_a, 0xED);
    KeInitializeInterrupt(&ki_b, 0xED);
    unowned = KeConnectInterrupt(&ki_a);              /* no handler -> rejected */
    irq_register(0xED, kinterrupt_dummy_irq, (void *)0, "ktest");
    c1 = KeConnectInterrupt(&ki_a);                   /* owned vector -> binds */
    c2 = KeConnectInterrupt(&ki_a);                   /* same ki -> idempotent */
    c3 = KeConnectInterrupt(&ki_b);                   /* different ki -> rejected */
    KeDisconnectInterrupt(&ki_a);
    irq_unregister(0xED);
    TEST_ASSERT_EQ((uint64_t)unowned, 0u, "bind on unowned (no-handler) vector rejected");
    TEST_ASSERT_EQ((uint64_t)c1, 1u, "bind on owned exclusive vector succeeds");
    TEST_ASSERT_EQ((uint64_t)c2, 1u, "same KINTERRUPT re-bind is idempotent");
    TEST_ASSERT_EQ((uint64_t)c3, 0u, "different KINTERRUPT on a bound vector rejected");
}

/* ---- Section 11: KAPC objects + per-thread APC queues + regions ---------- */

static void apc_noop_normal(void *c, void *a1, void *a2)
{
    (void)c; (void)a1; (void)a2;
}

/* Test: KeInitializeApc field defaults. */
static void test_apc_init_fields(void)
{
    KAPC apc;
    KeInitializeApc(&apc, (void *)0, OriginalApcEnvironment, 0, 0, 0,
                    (uint8_t)ApcKernelMode, (void *)(uintptr_t)0x55);
    TEST_ASSERT_EQ((uint64_t)apc.type, (uint64_t)APC_OBJECT_TYPE, "init: type tag");
    TEST_ASSERT_EQ((uint64_t)apc.size, (uint64_t)sizeof(KAPC), "init: size = sizeof(KAPC)");
    TEST_ASSERT_EQ((uint64_t)apc.inserted, 0u, "init: not inserted");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)apc.normal_context, 0x55u, "init: context stored");
}

/* Test: insert sets the kernel pending flag + inserted; remove clears them and
 * returns 1; a double-remove returns 0. Targets a LOCAL fake thread (isolated;
 * the queue ops touch only state/apc_lock/apc_state). */
static void test_apc_insert_remove(void)
{
    struct thread fake;
    KAPC apc;
    int r_ins, r_rm, r_rm2;
    uint32_t pend_after, ins_after;
    fake.state = THREAD_READY;
    fake.apc_lock.flag = 0;
    apc_thread_init(&fake.apc_state, (void *)0);
    KeInitializeApc(&apc, &fake, OriginalApcEnvironment, 0, 0,
                    apc_noop_normal, (uint8_t)ApcKernelMode, (void *)0);
    r_ins      = KeInsertQueueApc(&apc, (void *)1, (void *)2, 0);
    pend_after = fake.apc_state.kernel_apc_pending;
    ins_after  = apc.inserted;
    r_rm       = KeRemoveQueueApc(&apc);
    r_rm2      = KeRemoveQueueApc(&apc);
    TEST_ASSERT_EQ((uint64_t)r_ins, 1u, "KeInsertQueueApc returns 1");
    TEST_ASSERT_EQ((uint64_t)pend_after, 1u, "kernel_apc_pending set on insert");
    TEST_ASSERT_EQ((uint64_t)ins_after, 1u, "apc.inserted set");
    TEST_ASSERT_EQ((uint64_t)r_rm, 1u, "KeRemoveQueueApc returns 1");
    TEST_ASSERT_EQ((uint64_t)apc.inserted, 0u, "apc.inserted cleared on remove");
    TEST_ASSERT_EQ((uint64_t)fake.apc_state.kernel_apc_pending, 0u, "pending cleared (queue empty)");
    TEST_ASSERT_EQ((uint64_t)r_rm2, 0u, "double-remove returns 0");
}

/* Test: insert to an exiting (THREAD_DEAD) thread is rejected. */
static void test_apc_insert_to_dead_rejected(void)
{
    struct thread fake;
    KAPC apc;
    int r;
    fake.state = THREAD_DEAD;
    fake.apc_lock.flag = 0;
    apc_thread_init(&fake.apc_state, (void *)0);
    KeInitializeApc(&apc, &fake, OriginalApcEnvironment, 0, 0,
                    apc_noop_normal, (uint8_t)ApcKernelMode, (void *)0);
    r = KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    TEST_ASSERT_EQ((uint64_t)r, 0u, "insert to THREAD_DEAD rejected");
    TEST_ASSERT_EQ((uint64_t)apc.inserted, 0u, "not inserted on DEAD reject");
}

/* Test: critical/guarded region counter semantics (on thread_current()).
 * critical -> APCs disabled but not ALL; guarded -> ALL disabled; balanced
 * enter/leave re-enables. */
static void test_apc_regions(void)
{
    int crit, all_crit, guard, all_guard, after;
    KeEnterCriticalRegion();
    crit     = KeAreApcsDisabled();
    all_crit = KeAreAllApcsDisabled();
    KeLeaveCriticalRegion();
    KeEnterGuardedRegion();
    guard     = KeAreApcsDisabled();
    all_guard = KeAreAllApcsDisabled();
    KeLeaveGuardedRegion();
    after = KeAreApcsDisabled();
    TEST_ASSERT_EQ((uint64_t)crit, 1u, "critical region: APCs disabled");
    TEST_ASSERT_EQ((uint64_t)all_crit, 0u, "critical region: NOT all disabled");
    TEST_ASSERT_EQ((uint64_t)guard, 1u, "guarded region: APCs disabled");
    TEST_ASSERT_EQ((uint64_t)all_guard, 1u, "guarded region: ALL disabled");
    TEST_ASSERT_EQ((uint64_t)after, 0u, "balanced regions: APCs re-enabled");
}

/* ---- Section 12: KiDeliverApc kernel-mode delivery + rundown ------------- */

static volatile int   s_apc_kflag;   /* KernelRoutine ran */
static volatile int   s_apc_nflag;   /* NormalRoutine ran */
static volatile int   s_apc_rflag;   /* RundownRoutine ran */
static volatile KIRQL s_apc_kirql;   /* IRQL observed inside KernelRoutine */
static volatile KIRQL s_apc_nirql;   /* IRQL observed inside NormalRoutine */

static void apc_kroutine(KAPC *a, void **nr, void **nc, void **s1, void **s2)
{
    (void)a; (void)nr; (void)nc; (void)s1; (void)s2;
    s_apc_kirql = KeGetCurrentIrql();
    s_apc_kflag++;
}
static void apc_nroutine(void *c, void *a1, void *a2)
{
    (void)c; (void)a1; (void)a2;
    s_apc_nirql = KeGetCurrentIrql();
    s_apc_nflag++;
}
static void apc_rroutine(KAPC *a) { (void)a; s_apc_rflag++; }

/* Test: a normal kernel APC queued to the current thread is delivered by
 * KiDeliverApc -- KernelRoutine then NormalRoutine run, pending + in-progress
 * clear, the delivery counter advances. Runs on thread_current() (the engine
 * resolves the target itself). */
static void test_apc_deliver_kernel_normal(void)
{
    struct thread *me = thread_current();
    KAPC apc;
    uint64_t before, after;
    s_apc_kflag = s_apc_nflag = 0;
    apc_delivery_stats(&before, 0);
    KeInitializeApc(&apc, me, OriginalApcEnvironment, apc_kroutine, 0,
                    apc_nroutine, (uint8_t)ApcKernelMode, (void *)0);
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    apc_delivery_stats(&after, 0);
    TEST_ASSERT_EQ((uint64_t)s_apc_kflag, 1u, "KernelRoutine ran once");
    TEST_ASSERT_EQ((uint64_t)s_apc_nflag, 1u, "NormalRoutine ran once");
    TEST_ASSERT_EQ((uint64_t)me->apc_state.kernel_apc_pending, 0u, "pending cleared after delivery");
    TEST_ASSERT_EQ((uint64_t)me->apc_state.kernel_apc_in_progress, 0u, "in-progress cleared after delivery");
    TEST_ASSERT_EQ(after - before, 1u, "kernel delivery counter +1");
    TEST_ASSERT_EQ((uint64_t)s_apc_kirql, (uint64_t)APC_LEVEL, "KernelRoutine ran at APC_LEVEL");
    TEST_ASSERT_EQ((uint64_t)s_apc_nirql, (uint64_t)PASSIVE_LEVEL, "NormalRoutine ran at PASSIVE_LEVEL");
}

/* Test: a special kernel APC IS delivered through the real KeLowerIrql path even
 * while inside a critical region (critical blocks only normal APCs). Exercises
 * the KeLowerIrql delivery gate, not just a direct KiDeliverApc call. */
static void test_apc_deliver_special_via_lower_in_critical(void)
{
    struct thread *me = thread_current();
    KAPC apc;
    KIRQL old;
    s_apc_kflag = 0;
    KeInitializeApc(&apc, me, OriginalApcEnvironment, apc_kroutine, 0,
                    (PKNORMAL_ROUTINE)0, (uint8_t)ApcKernelMode, (void *)0);
    KeRaiseIrql(APC_LEVEL, &old);
    KeEnterCriticalRegion();
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    KeLowerIrql(PASSIVE_LEVEL);     /* crosses below APC -> delivers special APC */
    KeLeaveCriticalRegion();
    TEST_ASSERT_EQ((uint64_t)s_apc_kflag, 1u, "special APC delivered via KeLowerIrql in critical region");
    TEST_ASSERT_EQ((uint64_t)me->apc_state.kernel_apc_pending, 0u, "pending cleared after lower-path delivery");
}

/* Test: a special kernel APC (NormalRoutine NULL) runs only its KernelRoutine. */
static void test_apc_deliver_special_kernel(void)
{
    struct thread *me = thread_current();
    KAPC apc;
    s_apc_kflag = s_apc_nflag = 0;
    KeInitializeApc(&apc, me, OriginalApcEnvironment, apc_kroutine, 0,
                    (PKNORMAL_ROUTINE)0, (uint8_t)ApcKernelMode, (void *)0);
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    TEST_ASSERT_EQ((uint64_t)s_apc_kflag, 1u, "special: KernelRoutine ran");
    TEST_ASSERT_EQ((uint64_t)s_apc_nflag, 0u, "special: no NormalRoutine");
    TEST_ASSERT_EQ((uint64_t)me->apc_state.kernel_apc_pending, 0u, "special: pending cleared");
}

/* Test: a critical region suppresses normal kernel APC delivery; leaving the
 * region and re-delivering runs it. */
static void test_apc_deliver_critical_suppressed(void)
{
    struct thread *me = thread_current();
    KAPC apc;
    int delivered_in_region;
    s_apc_nflag = 0;
    KeInitializeApc(&apc, me, OriginalApcEnvironment, 0, 0,
                    apc_nroutine, (uint8_t)ApcKernelMode, (void *)0);
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    KeEnterCriticalRegion();
    KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    delivered_in_region = s_apc_nflag;
    KeLeaveCriticalRegion();
    KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    TEST_ASSERT_EQ((uint64_t)delivered_in_region, 0u, "critical region suppresses normal APC");
    TEST_ASSERT_EQ((uint64_t)s_apc_nflag, 1u, "delivered after leaving critical region");
    TEST_ASSERT_EQ((uint64_t)me->apc_state.kernel_apc_pending, 0u, "pending cleared after delivery");
}

/* Test: a guarded region suppresses even SPECIAL kernel APCs (special_apc_disable
 * blocks all classes), unlike a critical region which blocks only normal APCs. */
static void test_apc_deliver_guarded_suppresses_special(void)
{
    struct thread *me = thread_current();
    KAPC apc;
    int delivered_in_region;
    s_apc_kflag = 0;
    KeInitializeApc(&apc, me, OriginalApcEnvironment, apc_kroutine, 0,
                    (PKNORMAL_ROUTINE)0, (uint8_t)ApcKernelMode, (void *)0);
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    KeEnterGuardedRegion();
    KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    delivered_in_region = s_apc_kflag;
    KeLeaveGuardedRegion();
    KiDeliverApc((uint8_t)ApcKernelMode, (void *)0, (void *)0);
    TEST_ASSERT_EQ((uint64_t)delivered_in_region, 0u, "guarded region suppresses special APC");
    TEST_ASSERT_EQ((uint64_t)s_apc_kflag, 1u, "special APC delivered after leaving guarded region");
}

/* Test: KeLeaveCriticalRegion delivers a deferred normal kernel APC WITHOUT a
 * manual KiDeliverApc call (NT semantics: leaving the region drains it). */
static void test_apc_leave_critical_delivers(void)
{
    struct thread *me = thread_current();
    KAPC apc;
    s_apc_nflag = 0;
    KeInitializeApc(&apc, me, OriginalApcEnvironment, 0, 0,
                    apc_nroutine, (uint8_t)ApcKernelMode, (void *)0);
    KeEnterCriticalRegion();
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    KeLeaveCriticalRegion();            /* must deliver -- no manual KiDeliverApc */
    TEST_ASSERT_EQ((uint64_t)s_apc_nflag, 1u, "KeLeaveCriticalRegion delivers deferred normal APC");
    TEST_ASSERT_EQ((uint64_t)me->apc_state.kernel_apc_pending, 0u, "pending cleared by leave-delivery");
}

/* Test: KeLeaveGuardedRegion delivers a deferred special kernel APC. */
static void test_apc_leave_guarded_delivers_special(void)
{
    struct thread *me = thread_current();
    KAPC apc;
    s_apc_kflag = 0;
    KeInitializeApc(&apc, me, OriginalApcEnvironment, apc_kroutine, 0,
                    (PKNORMAL_ROUTINE)0, (uint8_t)ApcKernelMode, (void *)0);
    KeEnterGuardedRegion();
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    KeLeaveGuardedRegion();             /* must deliver -- no manual KiDeliverApc */
    TEST_ASSERT_EQ((uint64_t)s_apc_kflag, 1u, "KeLeaveGuardedRegion delivers deferred special APC");
    TEST_ASSERT_EQ((uint64_t)me->apc_state.kernel_apc_pending, 0u, "pending cleared by leave-delivery");
}

/* Test: apc_rundown_thread runs RundownRoutine for a still-queued APC on an
 * exiting thread and empties the queue. Uses a local fake thread. */
static void test_apc_rundown_runs(void)
{
    struct thread fake;
    KAPC apc;
    s_apc_rflag = 0;
    fake.state = THREAD_READY;
    fake.apc_lock.flag = 0;
    apc_thread_init(&fake.apc_state, (void *)0);
    KeInitializeApc(&apc, &fake, OriginalApcEnvironment, 0, apc_rroutine,
                    apc_noop_normal, (uint8_t)ApcKernelMode, (void *)0);
    KeInsertQueueApc(&apc, (void *)0, (void *)0, 0);
    fake.state = THREAD_DEAD;            /* caller publishes DEAD before rundown */
    apc_rundown_thread(&fake);
    TEST_ASSERT_EQ((uint64_t)s_apc_rflag, 1u, "RundownRoutine ran for queued APC");
    TEST_ASSERT_EQ((uint64_t)apc.inserted, 0u, "APC inserted flag cleared by rundown");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)fake.apc_state.apc_list_head[ApcKernelMode], 0u,
                   "kernel queue emptied by rundown");
    TEST_ASSERT_EQ((uint64_t)fake.apc_state.kernel_apc_pending, 0u, "pending cleared by rundown");
}

/* ---- Section 13: IRQL violation traps + telemetry ----------------------- */

/* Test: IRQL_REQUIRE_AT_MOST passes at/below the level, counts a violation
 * above it (per-CPU irql_violations). */
static void test_irql_require_at_most(void)
{
    struct per_cpu_data *c = smp_this_cpu();
    uint32_t v0 = c->irql_violations;
    KIRQL old;
    IRQL_REQUIRE_AT_MOST(APC_LEVEL);            /* at PASSIVE -- satisfied */
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v0), 0u, "AT_MOST satisfied: no violation");
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    IRQL_REQUIRE_AT_MOST(APC_LEVEL);            /* at DISPATCH -- violated */
    KeLowerIrql(old);
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v0), 1u, "AT_MOST violated at DISPATCH: +1");
}

/* Test: IRQL_REQUIRE_AT_LEAST counts a violation below the level, passes at it. */
static void test_irql_require_at_least(void)
{
    struct per_cpu_data *c = smp_this_cpu();
    uint32_t v0 = c->irql_violations;
    KIRQL old;
    IRQL_REQUIRE_AT_LEAST(DISPATCH_LEVEL);      /* at PASSIVE -- violated */
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v0), 1u, "AT_LEAST violated at PASSIVE: +1");
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    uint32_t v1 = c->irql_violations;
    IRQL_REQUIRE_AT_LEAST(DISPATCH_LEVEL);      /* at DISPATCH -- satisfied */
    KeLowerIrql(old);
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v1), 0u, "AT_LEAST satisfied at DISPATCH");
}

/* Test: KeLowerIrqlForced lowers to the target and bumps the forced-lower
 * counter (not the violation counter). */
static void test_irql_forced_lower(void)
{
    struct per_cpu_data *c = smp_this_cpu();
    uint32_t f0 = c->irql_forced_lowers, v0 = c->irql_violations;
    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);          /* now at DISPATCH (old = PASSIVE) */
    KeLowerIrqlForced(PASSIVE_LEVEL, "test");   /* unbalanced forced lower */
    TEST_ASSERT_EQ((uint64_t)KeGetCurrentIrql(), (uint64_t)PASSIVE_LEVEL, "forced lower reached PASSIVE");
    TEST_ASSERT_EQ((uint64_t)(c->irql_forced_lowers - f0), 1u, "forced-lower counter +1");
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v0), 0u, "forced lower is not a violation");
}

/* Test: a forced "lower" to a HIGHER level is rejected (counted as a violation,
 * not performed, not counted as a forced lower). */
static void test_irql_forced_lower_rejects_raise(void)
{
    struct per_cpu_data *c = smp_this_cpu();
    uint32_t v0 = c->irql_violations, f0 = c->irql_forced_lowers;
    KeLowerIrqlForced(DISPATCH_LEVEL, "test-bad");   /* at PASSIVE -- a raise */
    TEST_ASSERT_EQ((uint64_t)KeGetCurrentIrql(), (uint64_t)PASSIVE_LEVEL, "rejected raise: still PASSIVE");
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v0), 1u, "rejected raise counted as violation");
    TEST_ASSERT_EQ((uint64_t)(c->irql_forced_lowers - f0), 0u, "rejected raise NOT a forced lower");
}

/* Test: KeRaiseIrql(new<cur) and KeLowerIrql(old>cur) monotonic mismatches are
 * counted as violations (telemetry mode -- they clamp, do not trap). */
static void test_irql_monotonic_violation_counted(void)
{
    struct per_cpu_data *c = smp_this_cpu();
    KIRQL old, dummy;
    KeRaiseIrql(DISPATCH_LEVEL, &old);              /* now DISPATCH (old = PASSIVE) */
    uint32_t v0 = c->irql_violations;
    KeRaiseIrql(PASSIVE_LEVEL, &dummy);             /* new < cur -> violation, clamps */
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v0), 1u, "KeRaiseIrql lower attempt counted");
    KeLowerIrql(old);                               /* back to PASSIVE */
    uint32_t v1 = c->irql_violations;
    KeLowerIrql(DISPATCH_LEVEL);                    /* old > cur -> violation, clamps */
    TEST_ASSERT_EQ((uint64_t)(c->irql_violations - v1), 1u, "KeLowerIrql raise attempt counted");
    TEST_ASSERT_EQ((uint64_t)KeGetCurrentIrql(), (uint64_t)PASSIVE_LEVEL, "stayed at PASSIVE (clamped)");
}

/* ---- Section 14: DPC/APC fairness budget + watchdog --------------------- */

/* Test: kernel_apc_depth tracks insert/remove/rundown (the APC starvation
 * watchdog's lock-free read source). Local fake thread. */
static void test_apc_kernel_depth(void)
{
    struct thread fake;
    KAPC a1, a2;
    fake.state = THREAD_READY;
    fake.apc_lock.flag = 0;
    apc_thread_init(&fake.apc_state, (void *)0);
    KeInitializeApc(&a1, &fake, OriginalApcEnvironment, 0, 0,
                    apc_noop_normal, (uint8_t)ApcKernelMode, (void *)0);
    KeInitializeApc(&a2, &fake, OriginalApcEnvironment, 0, 0,
                    apc_noop_normal, (uint8_t)ApcKernelMode, (void *)0);
    KeInsertQueueApc(&a1, (void *)0, (void *)0, 0);
    KeInsertQueueApc(&a2, (void *)0, (void *)0, 0);
    TEST_ASSERT_EQ((uint64_t)fake.apc_state.kernel_apc_depth, 2u, "depth 2 after 2 kernel inserts");
    KeRemoveQueueApc(&a1);
    TEST_ASSERT_EQ((uint64_t)fake.apc_state.kernel_apc_depth, 1u, "depth 1 after one remove");
    fake.state = THREAD_DEAD;
    apc_rundown_thread(&fake);
    TEST_ASSERT_EQ((uint64_t)fake.apc_state.kernel_apc_depth, 0u, "depth 0 after rundown");
}

/* Test: DPC watchdog strict-mode toggle (default off). */
static void test_dpc_watchdog_strict_toggle(void)
{
    int saved = dpc_watchdog_strict_enabled();
    dpc_watchdog_set_strict(1);
    TEST_ASSERT_EQ((uint64_t)dpc_watchdog_strict_enabled(), 1u, "strict mode enabled");
    dpc_watchdog_set_strict(0);
    TEST_ASSERT_EQ((uint64_t)dpc_watchdog_strict_enabled(), 0u, "strict mode disabled (default)");
    dpc_watchdog_set_strict(saved);   /* restore */
}

/* ---- Section 15: threaded DPC list synchronization ---------------------- */


/* Test: after drain_queue hands a threaded DPC to the threaded list, it is
 * still cancellable -- KeRemoveQueueDpc unlinks it from the threaded list, not
 * just the normal queue -- and it is never LOST either way.
 *
 * Raising to DISPATCH_LEVEL keeps THIS CPU's drain out of the way; it is NOT
 * worker exclusion, because the threaded worker is one all-CPU PASSIVE thread
 * that can pop the node from another CPU. So the outcome of the cancel is a
 * race, and each outcome is asserted on its own terms rather than accepted:
 * if the cancel WON the callback must not have run, and if the worker won the
 * callback must have run. Two earlier shapes of this test were both wrong --
 * the original asserted the cancel always wins (racy, and it read dpc.queued
 * without the owning lock), and the first repair replaced that with
 * `qf_after_handoff == 1 || rm == 0`, which cannot fail at all: KeRemoveQueueDpc
 * returns 0 whenever !dpc->queued (dpc.c:764-765), so the one state that would
 * falsify the disjunction is unreachable by construction. */
static void test_dpc_threaded_remove(void)
{
    extern uint32_t dpc_drain_current_cpu(void);
    KDPC dpc;
    struct dpc_queue_sample s = {0, 0, 0, 0, 0};
    uint32_t my_cpu = smp_this_cpu()->cpu_id;
    KIRQL old;
    int rm, ok = 0, rounds;

    s_threaded_cancel_ran  = 0;
    s_threaded_cancel_irql = 0xFFu;
    KeInitializeThreadedDpc(&dpc, dpc_threaded_cancel_routine, (void *)0);
    /* HEAD-insert so one drain round reaches it: a tail-inserted fixture behind
     * DPC_BATCH_LIMIT unrelated entries never gets handed off at all, and then
     * KeRemoveQueueDpc unlinks it from the NORMAL queue and returns 1 while the
     * callback counter stays 0 -- every assertion below would pass without the
     * threaded-list path being exercised once. */
    KeSetImportanceDpc(&dpc, HighImportance);
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    KeInsertQueueDpc(&dpc, (void *)0, (void *)0);   /* -> normal queue, queued=1 */
    /* Bounded retry rather than one drain: head insertion makes the fixture the
     * next pop, but nothing stops a later head insert displacing it, and one
     * round runs at most DPC_BATCH_LIMIT callbacks. Retrying costs nothing in
     * the normal case (round 0 breaks) and removes the ambient-depth dependency
     * that a single drain would reintroduce. */
    for (rounds = 0; rounds < DPC_DRAIN_TEST_ROUNDS; rounds++) {
        dpc_drain_current_cpu();                     /* -> threaded list, queued stays 1 */
        ok = dpc_sample_queue(my_cpu, &dpc, &s);     /* proof of WHERE it went */
        if (ok && !s.truncated && !s.occurrences)
            break;
    }
    rm = KeRemoveQueueDpc(&dpc);                      /* find + unlink on the threaded list */
    KeLowerIrql(old);

    /* Cancel lost the race: the worker owns it, so wait for the callback to
     * return before this frame (and the KDPC in it) goes away. */
    if (!rm)
        KeFlushQueuedDpcs();

    /* Interpreting rm at all requires knowing the fixture actually left the
     * normal queue -- otherwise a successful "cancel" proves nothing about the
     * threaded list. Head-insertion makes that the expected case; asserting it
     * stops a concurrent head insert from turning the test vacuous. */
    TEST_ASSERT_EQ((uint64_t)ok, 1u, "post-handoff snapshot taken");
    TEST_ASSERT_EQ((uint64_t)s.truncated, 0u, "post-handoff snapshot walked both lists");
    TEST_ASSERT_EQ((uint64_t)s.occurrences, 0u,
                   "threaded DPC left the NORMAL queue at handoff");
    /* Absence from the normal queue alone would also be satisfied by a hand-off
     * that DROPPED the node. One lock covers both lists, so this pins where it
     * actually went: on the threaded list, or already claimed by the worker. */
    TEST_ASSERT_EQ((uint64_t)(s.threaded_occurrences == 1u || s_threaded_cancel_ran == 1),
                   1u, "threaded DPC is on the threaded list, or already run -- never lost");
    /* And it must never have been run INLINE by the drain: a threaded DPC runs
     * at PASSIVE in the worker, so a callback observed at DISPATCH_LEVEL means
     * the hand-off was skipped entirely. */
    TEST_ASSERT_EQ((uint64_t)(!s_threaded_cancel_ran ||
                              s_threaded_cancel_irql == (uint32_t)PASSIVE_LEVEL),
                   1u, "threaded callback ran at PASSIVE_LEVEL, never inline in the drain");

    if (rm) {
        TEST_ASSERT_EQ((uint64_t)s_threaded_cancel_ran, 0u,
                       "cancelling a threaded-pending DPC stops its callback running");
    } else {
        TEST_ASSERT_EQ((uint64_t)s_threaded_cancel_ran, 1u,
                       "a threaded DPC the worker claimed actually ran (never lost)");
        TEST_ASSERT_EQ((uint64_t)dpc_in_flight_threaded(), 0u,
                       "no threaded callback left in flight after the flush");
    }
    TEST_ASSERT_EQ((uint64_t)dpc.queued, 0u, "queued cleared after threaded cancel or run");
}

/* ---- Section 16: KeFlushQueuedDpcs threaded DPC completion -------------- */

static volatile int s_flush_threaded_ran;
static void dpc_flush_test_routine(KDPC *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
    s_flush_threaded_ran = 1;
}

/* Test: KeFlushQueuedDpcs waits for a threaded DPC to complete. Hand a threaded
 * DPC to the worker (drain at DISPATCH), then flush at PASSIVE -- after flush
 * returns the callback must have run and no threaded callback may be in flight,
 * so a teardown caller could now free the KDPC/context with no UAF. The worker
 * is a scheduled PASSIVE thread, so this exercises the yield-based flush wait. */
static void test_dpc_flush_threaded(void)
{
    extern uint32_t dpc_drain_current_cpu(void);
    KDPC dpc;
    KIRQL old;

    s_flush_threaded_ran = 0;
    KeInitializeThreadedDpc(&dpc, dpc_flush_test_routine, (void *)0);
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    KeInsertQueueDpc(&dpc, (void *)0, (void *)0);   /* -> normal queue, queued=1 */
    dpc_drain_current_cpu();                         /* -> threaded_q, worker will run it */
    KeLowerIrql(old);                                /* back to PASSIVE */

    KeFlushQueuedDpcs();                             /* must yield-wait for the worker */

    TEST_ASSERT_EQ((uint64_t)s_flush_threaded_ran, 1u,
                   "KeFlushQueuedDpcs waited for the threaded callback to run");
    TEST_ASSERT_EQ((uint64_t)dpc_in_flight_threaded(), 0u,
                   "no threaded callback left in flight after flush");
}

/* Registration */
/* --- Process accounting (process accounting) ------------------------------- *
 * These cover the deterministic surface: the exact ns->FILETIME and tick-quantum
 * arithmetic the accounting code runs, the ring-classification convention, and
 * that the running task's captured CreateTime is a valid value (placeholder or
 * plausible, never garbage). The behavioral accounting (per-tick charge, I/O
 * counters, slot-reuse zeroing) needs a live scheduler + VFS and is validated by
 * serial-log criteria on QEMU / bare metal, not in the WSL test harness. */

/* ns -> FILETIME 100 ns ticks, exactly as ProcessTimes converts kernel/user
 * time. Verifies the relationship between three independently-defined constants
 * (drift in any one is caught) plus a representative and a sub-tick value. */
static void test_acct_filetime_conversion(void)
{
    TEST_ASSERT_EQ(NSEC_PER_SEC / NS_PER_FILETIME_TICK, FILETIME_TICKS_PER_SECOND,
                   "1e9 ns/s / 100 ns-per-tick == FILETIME_TICKS_PER_SECOND");
    TEST_ASSERT_EQ(12345678900ULL / NS_PER_FILETIME_TICK, 123456789ULL,
                   "12345678900 ns -> 123456789 FILETIME ticks");
    TEST_ASSERT_EQ(50ULL / NS_PER_FILETIME_TICK, 0ULL,
                   "50 ns (< 1 tick) -> 0 FILETIME ticks (statistical: may read 0)");
}

/* Statistical tick quantum = NSEC_PER_SEC / live tick freq, as schedule()
 * charges per tick. Verifies the two nominal rates the timer runs at. */
static void test_acct_tick_quantum(void)
{
    TEST_ASSERT_EQ(NSEC_PER_SEC / 100u, 10000000ULL,
                   "100 Hz tick quantum == 10 ms (10,000,000 ns)");
    TEST_ASSERT_EQ(NSEC_PER_SEC / 1000u, 1000000ULL,
                   "1000 Hz tick quantum == 1 ms (1,000,000 ns)");
}

/* Ring classification the tick charge uses: (CS & 3) == 3 -> ring 3 (user_time),
 * else ring 0 (kernel_time). Real GDT code selectors. */
static void test_acct_ring_classification(void)
{
    TEST_ASSERT_EQ((0x08u & 3u), 0u, "kernel CS (0x08, RPL 0) classifies as ring 0");
    TEST_ASSERT_EQ((0x2Bu & 3u), 3u, "user CS (0x2B, RPL 3) classifies as ring 3");
}

/* The running task's captured CreateTime is never a garbage value: it is either
 * the sentinel (created before the wall clock was sourced) or a plausible
 * absolute FILETIME below the upper bound -- i.e. the stable-capture gate never
 * stores a near-1601 underflow or an implausibly large value. */
static void test_acct_current_task_create_time(void)
{
    struct task *t = task_current();
    TEST_ASSERT(t != NULL, "task_current() is valid during the test");
    /* Never a garbage value: placeholder or a plausible absolute FILETIME. */
    TEST_ASSERT(t->create_time_filetime == FILETIME_NOW_PLACEHOLDER ||
                t->create_time_filetime < FILETIME_MAX_PLAUSIBLE,
                "CreateTime is placeholder or a plausible absolute FILETIME");
    /* When the wall clock is sourced from real hardware, accounting init MUST
     * have stamped a real CreateTime -- catches a task (PID 0, a fork child)
     * that skipped task_init_accounting and kept a 1601 sentinel. */
    if (wall_clock_time_sourced())
        TEST_ASSERT(t->create_time_filetime != FILETIME_NOW_PLACEHOLDER,
                    "sourced clock -> current task CreateTime is stamped (not 1601)");
}

void test_register_sched(void)
{
    test_suite_register_cat("Sched: accounting FILETIME conversion",
                            test_acct_filetime_conversion, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: accounting tick quantum",
                            test_acct_tick_quantum, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: accounting ring classification",
                            test_acct_ring_classification, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: accounting CreateTime validity",
                            test_acct_current_task_create_time, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC init fields",
                            test_dpc_init_fields, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: threaded DPC worker started",
                            test_dpc_worker_started, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeInitializeThreadedDpc sets threaded",
                            test_dpc_init_threaded, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC drain executes at DISPATCH_LEVEL",
                            test_dpc_drain_executes, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeFlushQueuedDpcs runs callback at DISPATCH",
                            test_dpc_flush_runs_at_dispatch, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC insert/remove + queued_cpu",
                            test_dpc_insert_remove, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC drain depth accounting",
                            test_dpc_drain_depth_accounting, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC threaded handoff depth accounting",
                            test_dpc_threaded_handoff_depth, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: dpc_sample_queue argument contract",
                            test_dpc_sample_queue_contract, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC HighImportance head-insert",
                            test_dpc_high_importance_head, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC importance ABI values",
                            test_dpc_importance_abi_values, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer init fields",
                            test_ktimer_init_fields, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer set/cancel + owner CPU",
                            test_ktimer_set_cancel, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer single-shot fires + queues DPC",
                            test_ktimer_single_shot_fires, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer periodic re-arm",
                            test_ktimer_periodic_rearm, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer NULL-dpc fires",
                            test_ktimer_null_dpc_fires, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer future timer does not fire",
                            test_ktimer_future_no_fire, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer re-arm replaces DPC",
                            test_ktimer_rearm_replaces_dpc, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer overdue periodic bounded re-arm",
                            test_ktimer_overdue_periodic_bounded, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: ktimer DPC target normalized to service CPU",
                            test_ktimer_dpc_target_normalized, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KINTERRUPT init fields",
                            test_kinterrupt_init_fields, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeSynchronizeExecution runs at SynchronizeIrql",
                            test_kinterrupt_synchronize_runs, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeSynchronizeExecution self-ISR rejected",
                            test_kinterrupt_self_isr_rejected, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeRequestDpcFromIsr enqueues",
                            test_kerequestdpc_enqueues, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KINTERRUPT bind CAS (exclusive, idempotent)",
                            test_kinterrupt_bind_exclusive, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KAPC init fields",
                            test_apc_init_fields, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeInsertQueueApc / KeRemoveQueueApc",
                            test_apc_insert_remove, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeInsertQueueApc to DEAD thread rejected",
                            test_apc_insert_to_dead_rejected, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: critical/guarded region APC gating",
                            test_apc_regions, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KiDeliverApc normal kernel APC",
                            test_apc_deliver_kernel_normal, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: special APC delivered via KeLowerIrql in critical region",
                            test_apc_deliver_special_via_lower_in_critical, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KiDeliverApc special kernel APC",
                            test_apc_deliver_special_kernel, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: critical region suppresses APC delivery",
                            test_apc_deliver_critical_suppressed, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: guarded region suppresses special APC",
                            test_apc_deliver_guarded_suppresses_special, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeLeaveCriticalRegion delivers deferred APC",
                            test_apc_leave_critical_delivers, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeLeaveGuardedRegion delivers deferred special APC",
                            test_apc_leave_guarded_delivers_special, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: apc_rundown_thread runs RundownRoutine",
                            test_apc_rundown_runs, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: IRQL_REQUIRE_AT_MOST violation counter",
                            test_irql_require_at_most, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: IRQL_REQUIRE_AT_LEAST violation counter",
                            test_irql_require_at_least, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeLowerIrqlForced forced-lower counter",
                            test_irql_forced_lower, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeLowerIrqlForced rejects a raise",
                            test_irql_forced_lower_rejects_raise, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeRaise/KeLowerIrql monotonic mismatch counted",
                            test_irql_monotonic_violation_counted, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: kernel_apc_depth tracks insert/remove/rundown",
                            test_apc_kernel_depth, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: DPC watchdog strict-mode toggle",
                            test_dpc_watchdog_strict_toggle, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeRemoveQueueDpc cancels a threaded DPC",
                            test_dpc_threaded_remove, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: KeFlushQueuedDpcs waits for threaded DPC",
                            test_dpc_flush_threaded, TEST_CAT_SCHED);
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
    test_suite_register_cat("Sched: exec frame handoff pass-through",
                            test_exec_take_pending_frame_passthrough,
                            TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec parked stack clear on fresh task",
                            test_exec_stack_pending_free_clear, TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec stack pointers never alias",
                            test_exec_stack_pointers_never_alias,
                            TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec kernel-stack reclaim is leak-free",
                            test_exec_kernel_stack_reclaim_leak_free,
                            TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec staging release is idempotent",
                            test_exec_staging_release_is_idempotent,
                            TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec staging empty/NULL token is safe",
                            test_exec_staging_release_empty_is_safe,
                            TEST_CAT_SCHED);
    test_suite_register_cat("Sched: exec staging kmalloc release shape",
                            test_exec_staging_kfree_shape, TEST_CAT_SCHED);
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
