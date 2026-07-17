/* ============================================================================
 * test_threads.c -- Test thread function bodies
 *
 * TEST-ONLY: thread entry points used by test_usermode.c / test_ipc.c (both
 * under src/kernel/test/) for cooperative scheduling, preemptive scheduling,
 * mutex, semaphore, pipe, shared memory, user-mode, exec, and fork tests.
 * Pruned from the release flavor's C_SRCS at KERNEL_TESTS=off (TODO-10
 * section 28) -- shell_loader_func was moved OUT of this file to
 * src/kernel/main/shell_loader.c because it is production code (the
 * boot->desktop cmd.exe launcher), not a test thread.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/sched/task.h"
#include "kernel/mm/heap.h"
#include "kernel/fs/vfs.h"
#include "kernel/timer.h"
#include "kernel/sched/mutex.h"
#include "kernel/sched/semaphore.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/shmem.h"

/* --- Cooperative scheduling test threads --- */

void thread_a_func(void)
{
    yield();
    yield();
}

void thread_b_func(void)
{
    yield();
    yield();
}

/* --- Kernel thread test --- */

volatile uint32_t thread_shared_counter = 0;

void thread_inc_func(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < 5; i++) {
        thread_shared_counter++;
        thread_yield();
    }
}

/* --- Mutex test --- */

static mutex_t test_mutex = MUTEX_INIT("test_mutex");
volatile uint32_t mutex_shared_counter = 0;

void mutex_inc_func(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < 100; i++) {
        mutex_lock(&test_mutex);
        mutex_shared_counter++;
        mutex_unlock(&test_mutex);
    }
}

/* --- Semaphore test --- */

static semaphore_t test_sem = SEM_INIT("test_sem", 0);
volatile uint32_t sem_produced = 0;
volatile uint32_t sem_consumed = 0;

void sem_producer_func(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < 5; i++) {
        sem_produced++;
        sem_signal(&test_sem);
        thread_yield();
    }
}

void sem_consumer_func(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < 5; i++) {
        sem_wait(&test_sem);
        sem_consumed++;
    }
}

/* --- Pipe test --- */

int pipe_test_id = -1;
volatile uint32_t pipe_test_ok = 0;

static const char pipe_test_msg[] = "Hello from pipe!";

void pipe_writer_func(void *arg)
{
    (void)arg;
    pipe_write(pipe_test_id, pipe_test_msg, 16);
    pipe_close(pipe_test_id, PIPE_WRITE);
}

void pipe_reader_func(void *arg)
{
    char buf[32];
    int32_t n;
    uint32_t i;
    (void)arg;

    for (i = 0; i < 32; i++) buf[i] = 0;
    n = pipe_read(pipe_test_id, buf, 32);
    if (n == 16) {
        /* Verify the message */
        uint32_t match = 1;
        for (i = 0; i < 16; i++) {
            if (buf[i] != pipe_test_msg[i]) { match = 0; break; }
        }
        if (match) pipe_test_ok = 1;
    }
    pipe_close(pipe_test_id, PIPE_READ);
}

/* --- Shared memory test --- */

volatile uint32_t shmem_test_ok = 0;

void shmem_writer_func(void *arg)
{
    int id;
    volatile uint32_t *counter;
    uint32_t i;
    (void)arg;

    id = shmem_open("test_counter");
    if (id < 0) return;

    counter = (volatile uint32_t *)shmem_map(id);
    if (!counter) return;

    for (i = 0; i < 100; i++)
        (*counter)++;

    shmem_unmap(id);
}

void shmem_reader_func(void *arg)
{
    int id;
    volatile uint32_t *counter;
    uint32_t i;
    (void)arg;

    id = shmem_open("test_counter");
    if (id < 0) return;

    counter = (volatile uint32_t *)shmem_map(id);
    if (!counter) return;

    for (i = 0; i < 100; i++)
        (*counter)++;

    if (*counter == 200)
        shmem_test_ok = 1;

    shmem_unmap(id);
}

/* --- Preemptive scheduling test --- */

static volatile uint32_t pa_count = 0;
static volatile uint32_t pb_count = 0;

void preempt_a_func(void)
{
    uint32_t i;
    for (i = 0; i < 3; i++) {
        pa_count++;
        sleep_ms(100);
    }
}

void preempt_b_func(void)
{
    uint32_t i;
    for (i = 0; i < 3; i++) {
        pb_count++;
        sleep_ms(100);
    }
}

/* --- User-mode test function ---
 * This runs in ring 3 -- NO kernel function calls allowed!
 * All I/O goes through INT 0x80 syscalls. */
void user_test_func(void)
{
    /* sys_write(fd=1, buf, len) via INT 0x80 */
    static const char msg1[] = "  [UserMode] Hello from ring 3!\n";
    __asm__ volatile(
        "mov $1, %%rax\n"   /* SYS_WRITE */
        "mov $1, %%rdi\n"   /* fd = stdout */
        "mov %0, %%rsi\n"   /* buf */
        "mov %1, %%rdx\n"   /* len */
        "int $0x80\n"
        :
        : "r"((uint64_t)msg1), "r"((uint64_t)sizeof(msg1) - 1)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );

    /* sys_write again to confirm we survived */
    static const char msg2[] = "  [UserMode] Syscall returned successfully!\n";
    __asm__ volatile(
        "mov $1, %%rax\n"   /* SYS_WRITE */
        "mov $1, %%rdi\n"   /* fd = stdout */
        "mov %0, %%rsi\n"   /* buf */
        "mov %1, %%rdx\n"   /* len */
        "int $0x80\n"
        :
        : "r"((uint64_t)msg2), "r"((uint64_t)sizeof(msg2) - 1)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );

    /* sys_exit(0) */
    __asm__ volatile(
        "mov $3, %%rax\n"   /* SYS_EXIT */
        "mov $0, %%rdi\n"   /* exit code = 0 */
        "int $0x80\n"
        :
        :
        : "rax", "rdi", "memory"
    );

    /* Should never reach here */
    for (;;)
        __asm__ volatile("hlt");
}

/* --- Exec loader: kernel task that loads an ELF from C:\ --- */
void exec_loader_func(void)
{
    struct vfs_node *file;
    uint8_t *buf;

    if (!vfs_is_mounted('C')) {
        klog(LOG_WARN, "TEST", "[ExecLoader] C:\\ not mounted");
        return;
    }

    file = vfs_open("C:\\hello.exe", VFS_O_READ);
    if (!file) {
        klog(LOG_WARN, "TEST", "[ExecLoader] hello.exe not found on C:\\");
        return;
    }

    buf = (uint8_t *)kmalloc(file->size);
    if (!buf) {
        klog(LOG_WARN, "TEST", "[ExecLoader] cannot allocate buffer");
        vfs_close(file);
        return;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);
    vfs_close(file);

    if (task_exec(buf, file->size) < 0) {
        klog(LOG_WARN, "TEST", "[ExecLoader] exec failed");
        kfree(buf);
        return;
    }

    for (;;)
        __asm__ volatile("hlt");
}

/* shell_loader_func moved to src/kernel/main/shell_loader.c 2026-07-17
 * (release-flavor test-surface exclusion): it is PRODUCTION code (the boot->desktop cmd.exe
 * launcher), not test-only, so it cannot live in a TU pruned entirely
 * under KERNEL_TESTS=off. See shell_loader.h for the full rationale. */

/* --- Fork test function ---
 * Runs in ring 3. Forks, child prints and exits, parent calls waitpid. */
void fork_test_func(void)
{
    long child_pid;

    /* SYS_FORK */
    __asm__ volatile(
        "mov $5, %%rax\n"
        "int $0x80\n"
        : "=a"(child_pid)
        :
        : "rcx", "r11", "memory"
    );

    if (child_pid == 0) {
        /* Child process */
        static const char msg[] = "  [Fork] Child process running!\n";
        __asm__ volatile(
            "mov $1, %%rax\n"
            "mov $1, %%rdi\n"
            "mov %0, %%rsi\n"
            "mov %1, %%rdx\n"
            "int $0x80\n"
            :
            : "r"((uint64_t)msg), "r"((uint64_t)sizeof(msg) - 1)
            : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
        );

        /* Exit with code 42 */
        __asm__ volatile(
            "mov $3, %%rax\n"
            "mov $42, %%rdi\n"
            "int $0x80\n"
            :
            :
            : "rax", "rdi", "memory"
        );
    } else {
        /* Parent process */
        static const char msg[] = "  [Fork] Parent waiting for child...\n";
        __asm__ volatile(
            "mov $1, %%rax\n"
            "mov $1, %%rdi\n"
            "mov %0, %%rsi\n"
            "mov %1, %%rdx\n"
            "int $0x80\n"
            :
            : "r"((uint64_t)msg), "r"((uint64_t)sizeof(msg) - 1)
            : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
        );

        /* SYS_WAITPID(child_pid) */
        long status;
        __asm__ volatile(
            "mov $7, %%rax\n"
            "mov %1, %%rdi\n"
            "int $0x80\n"
            : "=a"(status)
            : "r"(child_pid)
            : "rcx", "r11", "memory"
        );

        /* Report result */
        static const char msg2[] = "  [Fork] Parent: child exited!\n";
        __asm__ volatile(
            "mov $1, %%rax\n"
            "mov $1, %%rdi\n"
            "mov %0, %%rsi\n"
            "mov %1, %%rdx\n"
            "int $0x80\n"
            :
            : "r"((uint64_t)msg2), "r"((uint64_t)sizeof(msg2) - 1)
            : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
        );

        /* Exit parent */
        __asm__ volatile(
            "mov $3, %%rax\n"
            "mov $0, %%rdi\n"
            "int $0x80\n"
            :
            :
            : "rax", "rdi", "memory"
        );
    }

    for (;;)
        __asm__ volatile("hlt");
}
