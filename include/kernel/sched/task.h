/* ============================================================================
 * task.h — Kernel task/process and thread management
 *
 * Provides kernel and user-mode threads with cooperative and preemptive
 * scheduling. Supports fork, exec, waitpid, process cleanup, and
 * per-task kernel threads.
 *
 * Design:
 *   - Round-robin scheduler, cooperative + preemptive
 *   - Per-task kernel stack (8 KiB) + optional user stack (16 KiB)
 *   - Per-task thread list (threads share PID/address space)
 *   - Context switch via interrupt frame swapping
 *   - PID 0 = idle/main thread (uses the boot stack)
 *   - Tasks that return from their entry function are marked DEAD
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Task states */
#define TASK_RUNNING    0   /* currently on the CPU */
#define TASK_READY      1   /* runnable, waiting for scheduler */
#define TASK_DEAD       2   /* finished, awaiting cleanup */
#define TASK_BLOCKED    3   /* waiting on I/O or event */
#define TASK_WAITING    4   /* blocked on waitpid */

/* Thread states (same values as TASK_* for simplicity) */
#define THREAD_RUNNING  0
#define THREAD_READY    1
#define THREAD_DEAD     2
#define THREAD_BLOCKED  3   /* blocked on join */

/* Limits */
#define TASK_MAX         32          /* max concurrent tasks */
#define THREAD_MAX       8           /* max threads per task */
#define THREAD_STACK_SIZE 8192       /* 8 KiB per thread stack */
#define TASK_STACK_SIZE  8192        /* 8 KiB per kernel task stack */
#define USER_STACK_SIZE  16384       /* 16 KiB per user task stack */
#define SCHED_QUANTUM    5           /* ticks per time slice (50ms at 100Hz) */

/* Saved CPU context (callee-saved registers only for cooperative switch) */
struct task_context {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;       /* return address (where to resume) */
};

/* Thread entry function type */
typedef void (*thread_entry_t)(void *arg);

/* Thread Control Block — schedulable unit within a task */
struct thread {
    uint32_t    id;             /* thread ID (unique within parent task) */
    uint32_t    state;          /* THREAD_RUNNING, THREAD_READY, etc. */
    uint64_t    rsp;            /* saved stack pointer (interrupt frame) */
    uint8_t    *stack_base;     /* base of thread stack (for kfree) */
    uint32_t    stack_size;     /* allocated stack size in bytes */
    uint32_t    parent_task;    /* index into tasks[] (owning task) */
    int32_t     exit_status;    /* exit status (set on THREAD_DEAD) */
    int32_t     join_tid;       /* thread we're waiting on (-1 = none) */
};

/* Task Control Block */
struct task {
    uint32_t    pid;            /* process ID */
    uint32_t    state;          /* TASK_RUNNING, TASK_READY, etc. */
    uint64_t    rsp;            /* saved stack pointer (interrupt frame) */
    uint8_t    *stack_base;     /* base of kernel stack (for kfree) */
    uint64_t    kernel_rsp;     /* top of kernel stack (for TSS rsp0) */
    uint8_t    *user_stack_base;/* base of user stack (NULL for kernel tasks) */
    const char *name;           /* human-readable name */
    uint32_t    parent_pid;     /* PID of parent (0 for init) */
    int32_t     exit_status;    /* exit code (set on TASK_DEAD) */
    int32_t     wait_pid;       /* PID we're waiting on (-1 = none) */
    uint32_t    exec_pending;   /* 1 = exec'd frame pending, skip save on switch-out */
    /* --- Per-task thread list --- */
    struct thread threads[THREAD_MAX];   /* thread pool for this task */
    uint32_t     num_threads;            /* number of threads (>= 1, thread 0 = main) */
};

/* Task entry function type */
typedef void (*task_entry_t)(void);

/* --- API --- */

/* Initialize the scheduler (makes the current execution context PID 0) */
void task_init(void);

/* Create a new kernel thread. Returns PID or -1 on failure. */
int task_create(task_entry_t entry, const char *name);

/* Create a new user-mode thread. Returns PID or -1 on failure.
 * The entry function runs in ring 3 with its own user stack. */
int task_create_user(task_entry_t entry, const char *name);

/* Voluntarily yield the CPU to the next ready task. */
void yield(void);

/* Get the currently running task. */
struct task *task_current(void);

/* Get total number of tasks (including dead ones). */
uint32_t task_count(void);

/* Get a task by PID. Returns NULL if invalid. */
struct task *task_get_by_pid(uint32_t pid);

/* Enable/disable preemptive scheduling. */
void scheduler_enable(void);
void scheduler_disable(void);

/* Called from PIT IRQ handler — preemptive round-robin.
 * Returns the (possibly new) interrupt frame pointer to restore. */
struct interrupt_frame;
uint64_t schedule(struct interrupt_frame *frame);

/* --- Process lifecycle --- */

/* Fork the current task. Returns child PID to parent, 0 to child, -1 on error.
 * 'frame' is the parent's interrupt frame (from the syscall). */
int task_fork(struct interrupt_frame *frame);

/* Exec: load an ELF binary and replace the current task's code.
 * 'data' is the raw ELF file, 'size' is its length.
 * Returns 0 on success, -1 on failure. */
int task_exec(const uint8_t *data, uint64_t size);

/* Exit the current task with a status code.
 * Wakes any parent waiting via waitpid. */
void task_exit(int32_t status);

/* Wait for a child task to exit. Returns exit status, or -1 on error. */
int32_t task_waitpid(uint32_t child_pid);

/* Clean up a dead task's resources (free stacks). */
void task_cleanup(uint32_t pid);

/* --- Thread API --- */

/* Create a new thread within the current task.
 * Returns thread ID (>= 1) or -1 on failure.
 * Thread shares PID and address space with parent task. */
int thread_create(thread_entry_t entry, void *arg, uint32_t stack_size);

/* Exit the current thread with a status code.
 * Wakes any thread blocked in thread_join(). */
void thread_exit(int32_t status);

/* Block until the specified thread exits. Returns exit status, or -1 on error. */
int32_t thread_join(uint32_t thread_id);

/* Voluntarily yield the CPU to the next ready thread.
 * (This is the same as yield() — threads and tasks share the scheduler.) */
void thread_yield(void);

/* Get the current thread within the current task.
 * Returns NULL if current task has no thread tracking (PID 0 boot). */
struct thread *thread_current(void);

/* --- Assembly (switch_context.asm) --- */

/* Switch from old_rsp to new_rsp. Saves callee-saved regs on old stack,
 * restores them from new stack, then returns into the new task. */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
