/* ============================================================================
 * test_ipc.c -- IPC & threading unit tests
 *
 * Tests kernel threads, mutex, semaphore, pipe, and shared memory.
 * Uses test helper functions from test_threads.c.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/sched/task.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/shmem.h"
#include "kernel/mm/pmm.h"   /* pmm_alloc_fail_next -- pipe pool OOM path */
#include "kernel/klog.h"

/* Thread helpers from test_threads.c */
extern volatile uint32_t thread_shared_counter;
extern void thread_inc_func(void *arg);

extern volatile uint32_t mutex_shared_counter;
extern void mutex_inc_func(void *arg);

extern volatile uint32_t sem_produced;
extern volatile uint32_t sem_consumed;
extern void sem_producer_func(void *arg);
extern void sem_consumer_func(void *arg);

extern int pipe_test_id;
extern volatile uint32_t pipe_test_ok;
extern void pipe_writer_func(void *arg);
extern void pipe_reader_func(void *arg);

extern volatile uint32_t shmem_test_ok;
extern void shmem_writer_func(void *arg);
extern void shmem_reader_func(void *arg);

/* Suppress noisy sched/ipc logging during tests */
static void quiet_ipc_logs(void)
{
    klog_set_level("sched", LOG_WARN);
    klog_set_level("ipc", LOG_WARN);
}

static void restore_ipc_logs(void)
{
    klog_set_level("sched", LOG_DEBUG);
    klog_set_level("ipc", LOG_DEBUG);
}

/* ---- Kernel thread test ---- */

static void test_kernel_threads(void)
{
    quiet_ipc_logs();

    thread_shared_counter = 0;
    int tid_a = kthread_create(thread_inc_func, (void *)"ThreadA", 0);
    int tid_b = kthread_create(thread_inc_func, (void *)"ThreadB", 0);

    if (tid_a < 0 || tid_b < 0) {
        TEST_ASSERT(0, "thread_create succeeds");
        restore_ipc_logs();
        return;
    }

    thread_join((uint32_t)tid_a);
    thread_join((uint32_t)tid_b);

    TEST_ASSERT_EQ(thread_shared_counter, 10, "two threads increment counter to 10");
    restore_ipc_logs();
}

/* ---- Mutex test ---- */

static void test_mutex_counter(void)
{
    quiet_ipc_logs();

    mutex_shared_counter = 0;
    int tid_a = kthread_create(mutex_inc_func, (void *)"MutexA", 0);
    int tid_b = kthread_create(mutex_inc_func, (void *)"MutexB", 0);

    if (tid_a < 0 || tid_b < 0) {
        TEST_ASSERT(0, "mutex thread_create succeeds");
        restore_ipc_logs();
        return;
    }

    thread_join((uint32_t)tid_a);
    thread_join((uint32_t)tid_b);

    TEST_ASSERT_EQ(mutex_shared_counter, 200, "mutex protects shared counter (200)");
    restore_ipc_logs();
}

/* ---- Semaphore test ---- */

static void test_semaphore_prodcons(void)
{
    quiet_ipc_logs();

    sem_produced = 0;
    sem_consumed = 0;
    int tid_c = kthread_create(sem_consumer_func, (void *)0, 0);
    int tid_p = kthread_create(sem_producer_func, (void *)0, 0);

    if (tid_p < 0 || tid_c < 0) {
        TEST_ASSERT(0, "semaphore thread_create succeeds");
        restore_ipc_logs();
        return;
    }

    thread_join((uint32_t)tid_p);
    thread_join((uint32_t)tid_c);

    TEST_ASSERT_EQ(sem_consumed, 5, "consumer receives 5 items via semaphore");
    restore_ipc_logs();
}

/* ---- Pipe test ---- */

static void test_pipe_roundtrip(void)
{
    quiet_ipc_logs();
    (void)pipe_init();

    int pipe_fds[2];
    pipe_test_ok = 0;

    if (pipe_create(pipe_fds) != 0) {
        TEST_ASSERT(0, "pipe_create succeeds");
        restore_ipc_logs();
        return;
    }

    pipe_test_id = pipe_fds[0];
    int tid_r = kthread_create(pipe_reader_func, (void *)0, 0);
    int tid_w = kthread_create(pipe_writer_func, (void *)0, 0);

    if (tid_w < 0 || tid_r < 0) {
        TEST_ASSERT(0, "pipe thread_create succeeds");
        /* Deliberately leave the pipe CLAIMED and any started reader parked,
         * and do NOT try to wake and join it (TODO-33 s15, round 2).
         *
         * The tempting cleanup -- close the write end, then join the reader --
         * can hang the whole test boot instead: sem_wait() checks count and
         * registers the waiter non-atomically (`semaphore.c:29-45`), so a
         * reader preempted between those two steps misses the permit this
         * close publishes, then blocks with no writer left to signal it, and
         * the join never returns. That lost-wakeup window is a real defect
         * and it is owned elsewhere; it is not this section's to close.
         * -> XREF: `03-memory-concurrency/TODO-06 s15` (item: "Commit:
         * wait/wake transaction locking -- close the lost-wakeup window in
         * all wait primitives") + `03-memory-concurrency/TODO-08 s11` (item:
         * "**Semaphore + event atomicity**")
         *
         * Leaving in_use SET is what makes stranding safe rather than merely
         * untidy: pipe_test_reset_for_fault_injection() refuses to free a
         * pool with a claimed slot, and the OOM case below returns early on
         * that refusal. So a stranded reader now costs a loudly-failed test,
         * where closing the pipe would hand its frames to pmm_free_contiguous
         * while the reader still holds a pointer into them. */
        restore_ipc_logs();
        return;
    }

    thread_join((uint32_t)tid_w);
    thread_join((uint32_t)tid_r);

    TEST_ASSERT(pipe_test_ok, "pipe data integrity (writer -> reader)");
    restore_ipc_logs();
}

/* ---- Shared memory test ---- */

static void test_shmem_counter(void)
{
    quiet_ipc_logs();
    shmem_test_ok = 0;

    int shm_id = shmem_create("test_counter", sizeof(uint32_t));
    if (shm_id < 0) {
        TEST_ASSERT(0, "shmem_create succeeds");
        restore_ipc_logs();
        return;
    }

    int tid_w = kthread_create(shmem_writer_func, (void *)0, 0);
    int tid_r = kthread_create(shmem_reader_func, (void *)0, 0);

    if (tid_w < 0 || tid_r < 0) {
        TEST_ASSERT(0, "shmem thread_create succeeds");
        shmem_unmap(shm_id);
        restore_ipc_logs();
        return;
    }

    thread_join((uint32_t)tid_w);
    thread_join((uint32_t)tid_r);

    TEST_ASSERT(shmem_test_ok, "shared memory counter reaches 200");
    shmem_unmap(shm_id);
    restore_ipc_logs();
}

/* ---- Registration ---- */

/* ---- Frame-backed pipe pool (TODO-33 s15) ---- */

/* Forces the PIPE_MAX pool allocation to fail (pmm_alloc_pages_hhdm ->
 * pmm_alloc_contiguous, multi-frame, so pmm_alloc_fail_next's single-shot
 * countdown fires on it) and asserts pipe_init() degrades instead of
 * crashing. The four entry points must REFUSE rather than dereference the
 * NULL pool: over the old static array they returned -1 by accident off a
 * zeroed in_use, so this is the test that the accident became a contract.
 * Recovers the pool afterward for every later consumer in this boot. */
static void test_pipe_pool_degrades_on_oom(void)
{
    /* TEST-SIDE-EFFECT-ALLOWED: controlled fault-recovery test of
     * pipe_init()'s own degrade/recover contract. pipe_init() is idempotent
     * by design (the CAS state machine exists for that), degrades rather
     * than halts, and touches no boot-critical or hardware state. */
    char buf[4];
    int fds[2];
    boot_result_t oom_rc;

    quiet_ipc_logs();
    /* RETURN on refusal, do not merely record it. The equality assert macro
     * calls _test_assert_eq() and carries on, so continuing past a refused reset
     * would arm pmm_alloc_fail_next() against a pool that is still READY:
     * pipe_init() would return BOOT_OK without consuming the injection, the
     * probes below would then mutate slot 0 of the LIVE pool, and the unspent
     * injection would go on to fail some unrelated later allocation. */
    if (pipe_test_reset_for_fault_injection() != 1) {
        TEST_ASSERT(0, "pool reset refused -- an earlier test stranded a live pipe");
        restore_ipc_logs();
        return;
    }
    TEST_ASSERT_EQ(pipe_ready(), 0, "reset leaves the pipe pool not-ready");

    pmm_alloc_fail_next();
    oom_rc = pipe_init();
    TEST_ASSERT_EQ((int)oom_rc, (int)BOOT_DEGRADED,
                   "forced OOM makes pipe_init report BOOT_DEGRADED");
    TEST_ASSERT_EQ(pipe_ready(), 0, "forced OOM leaves the pool not-ready");

    /* Observing the degraded state is a PREREQUISITE for the probes below,
     * not merely a result, so branch on it as well as recording it. If the
     * injection did not land, the pool is READY and pipe_create() would
     * SUCCEED here: the probes would then claim slot 0, operate on it, and
     * close only its read end, leaving in_use set (pipe.c clears in_use only
     * when BOTH ends are closed) and the slot leaked into every later suite
     * in this boot. Clear the countdown on the way out, because an injection
     * armed above and never consumed stays armed per-CPU with no expiry and
     * would fire on some unrelated later allocation. */
    if (oom_rc != BOOT_DEGRADED || pipe_ready() != 0) {
        pmm_alloc_fail_countdown_clear();
        restore_ipc_logs();
        return;
    }

    /* All four entry points refuse while degraded, with no NULL deref. */
    TEST_ASSERT(pipe_create(fds) == -1, "pipe_create refuses while degraded");
    TEST_ASSERT(pipe_write(0, "x", 1) == -1, "pipe_write refuses while degraded");
    TEST_ASSERT(pipe_read(0, buf, 1) == -1, "pipe_read refuses while degraded");
    pipe_close(0, PIPE_READ);   /* void: must return, not fault */
    TEST_ASSERT_EQ(pipe_ready(), 0, "pipe_close on a degraded pool is a no-op");

    /* Recover for the next test and any later consumer in this boot. This
     * test FREED the production pool, so a failed recovery leaves IPC pipes
     * dead for the rest of the boot -- the assertion records it but cannot
     * undo it. Retry once before reporting: the 18 frames were free a
     * moment ago, so the realistic failure is a transient race with another
     * allocator rather than genuine exhaustion, and a second attempt costs
     * nothing against a subsystem that would otherwise stay down. */
    if (pipe_init() != BOOT_OK)
        (void)pipe_init();
    TEST_ASSERT_EQ(pipe_ready(), 1,
                   "pool recovered -- a failure here leaves IPC pipes dead for this boot");
    restore_ipc_logs();
}

/* The recovered pool must be USABLE, not merely non-NULL, and a repeat
 * pipe_init() must not disturb a live pipe. The old static-array body
 * re-zeroed in_use for every slot on each call, so this second init silently
 * freed every live pipe while its holders kept their ids -- and
 * test_pipe_roundtrip above makes exactly that second call. */
static void test_pipe_repeat_init_preserves_live_pipe(void)
{
    /* TEST-SIDE-EFFECT-ALLOWED: this case calls pipe_init() on the LIVE
     * subsystem, which is the exact behaviour under test -- a repeat call
     * must NOT disturb a live pipe. Declared here for the same reason the
     * OOM case above declares it: pipe_init() is idempotent by design, it
     * degrades rather than halts, and it touches no boot-critical or
     * hardware state. Without this the file would be inconsistent about
     * declaring the same class of call in two adjacent tests. */
    int fds[2];
    char buf[8];

    quiet_ipc_logs();
    /* Both prerequisites RETURN on failure. They are prerequisites, not
     * results: an equality assert records and carries on, and a failed
     * pipe_create() leaves fds uninitialized, so continuing would feed stack
     * garbage to pipe_write/pipe_read/pipe_close as a pipe id. The bounds
     * check makes that safe from an overrun but not from an in-range hit on
     * a slot this test does not own, where it would mutate or block on
     * someone else's pipe instead of reporting the failure it actually saw. */
    if (!pipe_ready()) {
        TEST_ASSERT(0, "pool not ready -- the OOM case did not recover it");
        restore_ipc_logs();
        return;
    }
    if (pipe_create(fds) != 0) {
        TEST_ASSERT(0, "pipe_create failed on the recovered pool");
        restore_ipc_logs();
        return;
    }

    /* Repeat init while a pipe is live: a no-op that reports BOOT_OK. */
    TEST_ASSERT_EQ((int)pipe_init(), (int)BOOT_OK,
                   "repeat pipe_init reports the state it found");

    /* Round-trip through the still-open pipe. Fits in the 4 KiB ring, so
     * neither end blocks and no second thread is needed. */
    TEST_ASSERT_EQ((int)pipe_write(fds[1], "live", 4), 4,
                   "live pipe still accepts a write after repeat init");
    TEST_ASSERT_EQ((int)pipe_read(fds[0], buf, 4), 4,
                   "live pipe still returns its bytes after repeat init");
    TEST_ASSERT(buf[0] == 'l' && buf[3] == 'e',
                "repeat init did not clobber the live pipe's ring contents");

    pipe_close(fds[0], PIPE_READ);
    pipe_close(fds[1], PIPE_WRITE);
    restore_ipc_logs();
}

void test_register_ipc(void)
{
    test_suite_register_cat("IPC: kernel threads", test_kernel_threads, TEST_CAT_IPC);
    test_suite_register_cat("IPC: mutex", test_mutex_counter, TEST_CAT_SCHED);
    test_suite_register_cat("IPC: semaphore", test_semaphore_prodcons, TEST_CAT_IPC);
    test_suite_register_cat("IPC: pipe", test_pipe_roundtrip, TEST_CAT_IPC);
    test_suite_register_cat("IPC: shared memory", test_shmem_counter, TEST_CAT_IPC);
    test_suite_register_cat("IPC: pipe pool OOM", test_pipe_pool_degrades_on_oom, TEST_CAT_IPC);
    test_suite_register_cat("IPC: pipe reinit", test_pipe_repeat_init_preserves_live_pipe, TEST_CAT_IPC);
}

#endif /* KERNEL_TESTS */
