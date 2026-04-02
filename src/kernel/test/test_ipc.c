/* ============================================================================
 * test_ipc.c — IPC & threading unit tests
 *
 * Tests kernel threads, mutex, semaphore, pipe, and shared memory.
 * Uses test helper functions from test_threads.c.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/sched/task.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/shmem.h"
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
    int tid_a = thread_create(thread_inc_func, (void *)"ThreadA", 0);
    int tid_b = thread_create(thread_inc_func, (void *)"ThreadB", 0);

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
    int tid_a = thread_create(mutex_inc_func, (void *)"MutexA", 0);
    int tid_b = thread_create(mutex_inc_func, (void *)"MutexB", 0);

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
    int tid_c = thread_create(sem_consumer_func, (void *)0, 0);
    int tid_p = thread_create(sem_producer_func, (void *)0, 0);

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
    pipe_init();

    int pipe_fds[2];
    pipe_test_ok = 0;

    if (pipe_create(pipe_fds) != 0) {
        TEST_ASSERT(0, "pipe_create succeeds");
        restore_ipc_logs();
        return;
    }

    pipe_test_id = pipe_fds[0];
    int tid_r = thread_create(pipe_reader_func, (void *)0, 0);
    int tid_w = thread_create(pipe_writer_func, (void *)0, 0);

    if (tid_w < 0 || tid_r < 0) {
        TEST_ASSERT(0, "pipe thread_create succeeds");
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

    int tid_w = thread_create(shmem_writer_func, (void *)0, 0);
    int tid_r = thread_create(shmem_reader_func, (void *)0, 0);

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

void test_register_ipc(void)
{
    test_suite_register_cat("IPC: kernel threads", test_kernel_threads, TEST_CAT_IPC);
    test_suite_register_cat("IPC: mutex", test_mutex_counter, TEST_CAT_SCHED);
    test_suite_register_cat("IPC: semaphore", test_semaphore_prodcons, TEST_CAT_IPC);
    test_suite_register_cat("IPC: pipe", test_pipe_roundtrip, TEST_CAT_IPC);
    test_suite_register_cat("IPC: shared memory", test_shmem_counter, TEST_CAT_IPC);
}

#endif /* KERNEL_TESTS */
