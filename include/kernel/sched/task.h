/* ============================================================================
 * task.h — Kernel task/process management
 *
 * Provides kernel and user-mode threads with cooperative and preemptive
 * scheduling. Supports fork, exec, waitpid, and process cleanup.
 *
 * Design:
 *   - Round-robin scheduler, cooperative + preemptive
 *   - Per-task kernel stack (8 KiB) + optional user stack (16 KiB)
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

/* Limits */
#define TASK_MAX         32          /* max concurrent tasks */
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

/* --- Assembly (switch_context.asm) --- */

/* Switch from old_rsp to new_rsp. Saves callee-saved regs on old stack,
 * restores them from new stack, then returns into the new task. */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
