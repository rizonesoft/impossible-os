/* ============================================================================
 * task.h — Cooperative kernel threading
 *
 * Provides lightweight kernel threads with cooperative scheduling (yield).
 * Each task has its own stack and TCB (Task Control Block).
 *
 * Design:
 *   - Round-robin scheduler, cooperative (no preemption yet)
 *   - Per-task 8 KiB kernel stack, heap-allocated
 *   - Context switch saves only callee-saved registers (System V ABI)
 *   - PID 0 = idle/main thread (uses the boot stack)
 *   - Tasks that return from their entry function are marked DEAD
 * ============================================================================ */

#pragma once

#include "types.h"

/* Task states */
#define TASK_RUNNING    0   /* currently on the CPU */
#define TASK_READY      1   /* runnable, waiting for scheduler */
#define TASK_DEAD       2   /* finished, awaiting cleanup */
#define TASK_BLOCKED    3   /* future: waiting on I/O or event */

/* Limits */
#define TASK_MAX        32          /* max concurrent tasks */
#define TASK_STACK_SIZE 8192        /* 8 KiB per task */
#define SCHED_QUANTUM   5           /* ticks per time slice (50ms at 100Hz) */

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
    uint64_t    rsp;            /* saved stack pointer */
    uint8_t    *stack_base;     /* base of allocated stack (for kfree) */
    const char *name;           /* human-readable name */
};

/* Task entry function type */
typedef void (*task_entry_t)(void);

/* --- API --- */

/* Initialize the scheduler (makes the current execution context PID 0) */
void task_init(void);

/* Create a new kernel thread. Returns PID or -1 on failure. */
int task_create(task_entry_t entry, const char *name);

/* Voluntarily yield the CPU to the next ready task. */
void yield(void);

/* Get the currently running task. */
struct task *task_current(void);

/* Get total number of tasks (including dead ones). */
uint32_t task_count(void);

/* Enable/disable preemptive scheduling. */
void scheduler_enable(void);
void scheduler_disable(void);

/* Called from PIT IRQ handler — preemptive round-robin.
 * Returns the (possibly new) interrupt frame pointer to restore. */
struct interrupt_frame;
uint64_t schedule(struct interrupt_frame *frame);

/* --- Assembly (switch_context.asm) --- */

/* Switch from old_rsp to new_rsp. Saves callee-saved regs on old stack,
 * restores them from new stack, then returns into the new task. */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
