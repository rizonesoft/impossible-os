/* ============================================================================
 * task.c — Cooperative kernel thread scheduler
 *
 * Round-robin cooperative scheduling. Tasks call yield() to voluntarily
 * give up the CPU. The scheduler picks the next READY task.
 *
 * Task 0 is the boot/main thread — it uses the existing kernel stack
 * and is created implicitly by task_init().
 * ============================================================================ */

#include "task.h"
#include "heap.h"
#include "printk.h"

/* --- Task table --- */
static struct task tasks[TASK_MAX];
static uint32_t num_tasks = 0;
static uint32_t current_task = 0;

/* --- Task wrapper ---
 * New tasks start execution here. When the entry function returns,
 * we mark the task as dead and yield forever. */
static void task_wrapper(void)
{
    /* The entry function pointer is stored in r12 by task_create.
     * switch_context restores r12, so we can call it here. */
    task_entry_t entry;
    __asm__ volatile("mov %%r12, %0" : "=r"(entry));

    entry();

    /* Task finished — mark as dead and yield */
    tasks[current_task].state = TASK_DEAD;
    printk("[SCHED] Task %u (\"%s\") exited\n",
           (uint64_t)tasks[current_task].pid,
           tasks[current_task].name ? tasks[current_task].name : "?");

    /* Yield forever (we're dead, scheduler will skip us) */
    for (;;)
        yield();
}

/* --- Public API --- */

void task_init(void)
{
    uint32_t i;

    for (i = 0; i < TASK_MAX; i++) {
        tasks[i].pid = 0;
        tasks[i].state = TASK_DEAD;
        tasks[i].rsp = 0;
        tasks[i].stack_base = (uint8_t *)0;
        tasks[i].name = (const char *)0;
    }

    /* Task 0: the current boot/main thread.
     * Its stack is the existing kernel boot stack — we don't allocate one.
     * RSP will be saved by switch_context when it first yields. */
    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack_base = (uint8_t *)0;  /* boot stack, don't free */
    tasks[0].name = "main";
    num_tasks = 1;
    current_task = 0;

    printk("[OK] Scheduler initialized (PID 0 = main thread)\n");
}

int task_create(task_entry_t entry, const char *name)
{
    uint32_t pid;
    uint8_t *stack;
    uint64_t *sp;

    if (num_tasks >= TASK_MAX) {
        printk("[FAIL] task_create: max tasks reached\n");
        return -1;
    }

    pid = num_tasks;

    /* Allocate task stack */
    stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        printk("[FAIL] task_create: cannot allocate stack\n");
        return -1;
    }

    /* Set up initial stack frame so switch_context can "return" into task_wrapper.
     *
     * Stack layout (growing downward):
     *   [top of stack]      ← stack + TASK_STACK_SIZE
     *   ... (unused) ...
     *   r15                 ← sp[0]  = 0
     *   r14                 ← sp[1]  = 0
     *   r13                 ← sp[2]  = 0
     *   r12                 ← sp[3]  = entry (task_wrapper reads this)
     *   rbp                 ← sp[4]  = 0
     *   rbx                 ← sp[5]  = 0
     *   return address      ← sp[6]  = task_wrapper
     *
     * switch_context does: pop r15..rbx, ret → task_wrapper
     */
    sp = (uint64_t *)(stack + TASK_STACK_SIZE);

    /* Align to 16 bytes (required by System V ABI) */
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);

    /* Build the initial stack frame (7 entries: 6 regs + return addr) */
    sp -= 7;
    sp[0] = 0;                           /* r15 */
    sp[1] = 0;                           /* r14 */
    sp[2] = 0;                           /* r13 */
    sp[3] = (uint64_t)entry;             /* r12 = entry function pointer */
    sp[4] = 0;                           /* rbp */
    sp[5] = 0;                           /* rbx */
    sp[6] = (uint64_t)task_wrapper;      /* return address */

    /* Initialize TCB */
    tasks[pid].pid = pid;
    tasks[pid].state = TASK_READY;
    tasks[pid].rsp = (uint64_t)sp;
    tasks[pid].stack_base = stack;
    tasks[pid].name = name;
    num_tasks++;

    printk("[OK] Task %u (\"%s\") created\n",
           (uint64_t)pid, name ? name : "?");

    return (int)pid;
}

void yield(void)
{
    uint32_t prev = current_task;
    uint32_t next;
    uint32_t i;

    /* Find the next ready task (round-robin) */
    next = prev;
    for (i = 0; i < num_tasks; i++) {
        next = (next + 1) % num_tasks;
        if (tasks[next].state == TASK_READY || tasks[next].state == TASK_RUNNING)
            break;
    }

    /* No other task to switch to */
    if (next == prev) return;

    /* Update states */
    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;
    tasks[next].state = TASK_RUNNING;
    current_task = next;

    /* Perform the context switch */
    switch_context(&tasks[prev].rsp, tasks[next].rsp);
}

struct task *task_current(void)
{
    return &tasks[current_task];
}

uint32_t task_count(void)
{
    return num_tasks;
}
