/* ============================================================================
 * test_threads.c — Test thread function bodies
 *
 * Thread entry points used by boot_tests.c for cooperative scheduling,
 * preemptive scheduling, mutex, semaphore, pipe, shared memory, user-mode,
 * exec, shell loader, and fork tests.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/printk.h"
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
    printk("  [ThreadA] Hello from thread A (1/3)\n");
    yield();
    printk("  [ThreadA] Back in thread A (2/3)\n");
    yield();
    printk("  [ThreadA] Thread A finishing (3/3)\n");
}

void thread_b_func(void)
{
    printk("  [ThreadB] Hello from thread B (1/3)\n");
    yield();
    printk("  [ThreadB] Back in thread B (2/3)\n");
    yield();
    printk("  [ThreadB] Thread B finishing (3/3)\n");
}

/* --- Kernel thread test --- */

volatile uint32_t thread_shared_counter = 0;

void thread_inc_func(void *arg)
{
    uint32_t i;
    const char *label = (const char *)arg;
    for (i = 0; i < 5; i++) {
        thread_shared_counter++;
        printk("    [%s] shared_counter = %u\n", label,
               (uint64_t)thread_shared_counter);
        thread_yield();
    }
}

/* --- Mutex test --- */

static mutex_t test_mutex = MUTEX_INIT("test_mutex");
volatile uint32_t mutex_shared_counter = 0;

void mutex_inc_func(void *arg)
{
    uint32_t i;
    const char *label = (const char *)arg;
    for (i = 0; i < 100; i++) {
        mutex_lock(&test_mutex);
        mutex_shared_counter++;
        mutex_unlock(&test_mutex);
    }
    printk("    [%s] done (100 increments)\n", label);
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
        printk("    [Producer] produced item %u\n", (uint64_t)sem_produced);
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
        printk("    [Consumer] consumed item %u\n", (uint64_t)sem_consumed);
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
    printk("    [Reader] got %d bytes: \"%s\"\n", (uint64_t)(uint32_t)n, buf);
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

    printk("    [ShmWriter] done (100 increments, counter=%u)\n",
           (uint64_t)*counter);
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

    printk("    [ShmReader] done (100 increments, counter=%u)\n",
           (uint64_t)*counter);

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
        printk("  [PreemptA] Running (%u/3) — no yield!\n",
               (uint64_t)pa_count);
        /* Busy-wait ~50ms (loop, not yield) to prove preemption */
        sleep_ms(100);
    }
}

void preempt_b_func(void)
{
    uint32_t i;
    for (i = 0; i < 3; i++) {
        pb_count++;
        printk("  [PreemptB] Running (%u/3) — no yield!\n",
               (uint64_t)pb_count);
        sleep_ms(100);
    }
}

/* --- User-mode test function ---
 * This runs in ring 3 — NO kernel function calls allowed!
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
        printk("  [ExecLoader] C:\\ not mounted\n");
        return;
    }

    file = vfs_open("C:\\hello.exe", VFS_O_READ);
    if (!file) {
        printk("  [ExecLoader] hello.exe not found on C:\\\n");
        return;
    }

    buf = (uint8_t *)kmalloc(file->size);
    if (!buf) {
        printk("  [ExecLoader] cannot allocate buffer\n");
        vfs_close(file);
        return;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);
    vfs_close(file);

    if (task_exec(buf, file->size) < 0) {
        printk("  [ExecLoader] exec failed\n");
        kfree(buf);
        return;
    }

    for (;;)
        __asm__ volatile("hlt");
}

/* --- Shell loader: kernel task that execs shell.exe from C:\ --- */
void shell_loader_func(void)
{
    struct vfs_node *file;
    uint8_t *buf;

    if (!vfs_is_mounted('C')) {
        klog(LOG_WARN, "shell", "C:\\ not mounted");
        return;
    }

    file = vfs_open("C:\\shell.exe", VFS_O_READ);
    if (!file) {
        klog(LOG_WARN, "shell", "shell.exe not found on C:\\");
        return;
    }

    buf = (uint8_t *)kmalloc(file->size);
    if (!buf) {
        klog(LOG_ERROR, "shell", "cannot allocate buffer");
        vfs_close(file);
        return;
    }

    vfs_read(file, 0, (uint32_t)file->size, buf);
    vfs_close(file);

    if (task_exec(buf, file->size) < 0) {
        klog(LOG_ERROR, "shell", "exec failed");
        kfree(buf);
        return;
    }

    for (;;)
        __asm__ volatile("hlt");
}

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
