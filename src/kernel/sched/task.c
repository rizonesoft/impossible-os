/* ============================================================================
 * task.c — Kernel thread scheduler (cooperative + preemptive)
 *
 * Round-robin scheduling with fixed time quantum. The PIT timer IRQ
 * calls schedule() to preempt tasks automatically. Tasks can also
 * call yield() for cooperative switching.
 *
 * Task 0 is the boot/main thread — it uses the existing kernel stack
 * and is created implicitly by task_init().
 * ============================================================================ */

#include "task.h"
#include "idt.h"
#include "gdt.h"
#include "heap.h"
#include "printk.h"
#include "elf.h"

/* Software interrupt vector for yield() */
#define YIELD_INT_VECTOR 0x81

/* --- Task table --- */
static struct task tasks[TASK_MAX];
static uint32_t num_tasks = 0;
static uint32_t current_task = 0;

/* --- Preemptive scheduler state --- */
static volatile uint32_t sched_enabled = 0;
static volatile uint32_t sched_ticks = 0;    /* ticks since last switch */

/* Forward declarations */
static uint64_t yield_irq_handler(struct interrupt_frame *frame);
uint64_t schedule_now(struct interrupt_frame *frame);

/* --- Task wrapper ---
 * New tasks start execution here. When the entry function returns,
 * we mark the task as dead and halt forever (scheduler will skip us). */
static void task_wrapper(void)
{
    /* The entry function pointer is stored in r12 by task_create.
     * switch_context restores r12, so we can call it here. */
    task_entry_t entry;
    __asm__ volatile("mov %%r12, %0" : "=r"(entry));

    entry();

    /* Task finished — mark as dead */
    tasks[current_task].state = TASK_DEAD;
    printk("[SCHED] Task %u (\"%s\") exited\n",
           (uint64_t)tasks[current_task].pid,
           tasks[current_task].name ? tasks[current_task].name : "?");

    /* Yield forever — yield() via INT 0x81 always works,
     * whether preemptive scheduler is enabled or not. */
    for (;;)
        yield();
}

/* --- Find the next runnable task (round-robin) --- */
static uint32_t find_next_task(uint32_t from)
{
    uint32_t i, next;

    next = from;
    for (i = 0; i < num_tasks; i++) {
        next = (next + 1) % num_tasks;
        if (tasks[next].state == TASK_READY || tasks[next].state == TASK_RUNNING)
            return next;
    }
    return from;  /* no other task found */
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
        tasks[i].kernel_rsp = 0;
        tasks[i].user_stack_base = (uint8_t *)0;
        tasks[i].name = (const char *)0;
        tasks[i].parent_pid = 0;
        tasks[i].exit_status = 0;
        tasks[i].wait_pid = -1;
        tasks[i].exec_pending = 0;
    }

    /* Task 0: the current boot/main thread.
     * Its stack is the existing kernel boot stack — we don't allocate one.
     * RSP will be saved by switch_context/schedule when it first yields. */
    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack_base = (uint8_t *)0;  /* boot stack, don't free */
    tasks[0].name = "main";
    num_tasks = 1;
    current_task = 0;

    printk("[OK] Scheduler initialized (PID 0 = main, quantum = %u ticks)\n",
           (uint64_t)SCHED_QUANTUM);

    /* Register the yield software interrupt handler (INT 0x81) */
    idt_register_handler(YIELD_INT_VECTOR, yield_irq_handler);
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

    /* Set up initial stack as a full interrupt frame so the ISR stub can
     * iretq into task_wrapper on the first preemptive switch.
     *
     * The ISR restore sequence is:
     *   pop r15..r8  (8 regs)
     *   pop rbp rdi rsi rdx rcx rbx rax  (7 regs)
     *   add rsp, 16  (skip int_no, err_code)
     *   iretq        (pops rip, cs, rflags, rsp, ss)
     *
     * Total: 22 qwords on the stack.
     */
    sp = (uint64_t *)(stack + TASK_STACK_SIZE);

    /* Align to 16 bytes */
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);

    /* Reserve space for a secondary stack area that iretq will set RSP to.
     * iretq pops RIP, CS, RFLAGS, RSP, SS — the RSP in the frame tells
     * the CPU where to set the stack AFTER returning. */
    {
        uint64_t new_rsp = (uint64_t)sp;  /* stack top after iretq */

        sp -= 22;
        sp[0]  = 0;                           /* r15 */
        sp[1]  = 0;                           /* r14 */
        sp[2]  = 0;                           /* r13 */
        sp[3]  = (uint64_t)entry;             /* r12 = entry function ptr */
        sp[4]  = 0;                           /* r11 */
        sp[5]  = 0;                           /* r10 */
        sp[6]  = 0;                           /* r9 */
        sp[7]  = 0;                           /* r8 */
        sp[8]  = 0;                           /* rbp */
        sp[9]  = 0;                           /* rdi */
        sp[10] = 0;                           /* rsi */
        sp[11] = 0;                           /* rdx */
        sp[12] = 0;                           /* rcx */
        sp[13] = 0;                           /* rbx */
        sp[14] = 0;                           /* rax */
        sp[15] = 0;                           /* int_no (dummy) */
        sp[16] = 0;                           /* err_code (dummy) */
        sp[17] = (uint64_t)task_wrapper;      /* rip */
        sp[18] = GDT_KERNEL_CODE;             /* cs = 0x08 */
        sp[19] = 0x202;                       /* rflags: IF set */
        sp[20] = new_rsp;                     /* rsp after iretq */
        sp[21] = GDT_KERNEL_DATA;             /* ss = 0x10 */
    }

    /* Initialize TCB */
    tasks[pid].pid = pid;
    tasks[pid].state = TASK_READY;
    tasks[pid].rsp = (uint64_t)sp;
    tasks[pid].stack_base = stack;
    tasks[pid].kernel_rsp = (uint64_t)(stack + TASK_STACK_SIZE);
    tasks[pid].user_stack_base = (uint8_t *)0;  /* kernel task */
    tasks[pid].name = name;
    tasks[pid].parent_pid = current_task;
    tasks[pid].exit_status = 0;
    tasks[pid].wait_pid = -1;
    tasks[pid].exec_pending = 0;
    num_tasks++;

    printk("[OK] Task %u (\"%s\") created (kernel)\n",
           (uint64_t)pid, name ? name : "?");

    return (int)pid;
}

int task_create_user(task_entry_t entry, const char *name)
{
    uint32_t pid;
    uint8_t *kstack, *ustack;
    uint64_t *sp;

    if (num_tasks >= TASK_MAX) {
        printk("[FAIL] task_create_user: max tasks reached\n");
        return -1;
    }

    pid = num_tasks;

    /* Allocate kernel stack (for interrupt/syscall handling) */
    kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!kstack) {
        printk("[FAIL] task_create_user: cannot allocate kernel stack\n");
        return -1;
    }

    /* Allocate user stack */
    ustack = (uint8_t *)kmalloc(USER_STACK_SIZE);
    if (!ustack) {
        printk("[FAIL] task_create_user: cannot allocate user stack\n");
        /* TODO: free kstack */
        return -1;
    }

    /* Build initial interrupt frame on the KERNEL stack.
     * The ISR restore does: pop regs, add rsp 16, iretq.
     * iretq pops RIP, CS, RFLAGS, RSP, SS.
     * CS/SS use user-mode selectors (ring 3 = DPL|3).
     * RSP points to the user stack top.
     * RIP points directly to the entry function. */
    sp = (uint64_t *)(kstack + TASK_STACK_SIZE);
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);

    {
        uint64_t user_rsp = (uint64_t)(ustack + USER_STACK_SIZE) & ~0xFULL;

        sp -= 22;
        sp[0]  = 0;                                /* r15 */
        sp[1]  = 0;                                /* r14 */
        sp[2]  = 0;                                /* r13 */
        sp[3]  = 0;                                /* r12 */
        sp[4]  = 0;                                /* r11 */
        sp[5]  = 0;                                /* r10 */
        sp[6]  = 0;                                /* r9 */
        sp[7]  = 0;                                /* r8 */
        sp[8]  = 0;                                /* rbp */
        sp[9]  = 0;                                /* rdi */
        sp[10] = 0;                                /* rsi */
        sp[11] = 0;                                /* rdx */
        sp[12] = 0;                                /* rcx */
        sp[13] = 0;                                /* rbx */
        sp[14] = 0;                                /* rax */
        sp[15] = 0;                                /* int_no */
        sp[16] = 0;                                /* err_code */
        sp[17] = (uint64_t)entry;                  /* rip = user entry */
        sp[18] = GDT_USER_CODE | 3;                /* cs = user code, RPL=3 */
        sp[19] = 0x202;                            /* rflags: IF set */
        sp[20] = user_rsp;                         /* rsp = user stack */
        sp[21] = GDT_USER_DATA | 3;                /* ss = user data, RPL=3 */
    }

    /* Initialize TCB */
    tasks[pid].pid = pid;
    tasks[pid].state = TASK_READY;
    tasks[pid].rsp = (uint64_t)sp;
    tasks[pid].stack_base = kstack;
    tasks[pid].kernel_rsp = (uint64_t)(kstack + TASK_STACK_SIZE);
    tasks[pid].user_stack_base = ustack;
    tasks[pid].name = name;
    tasks[pid].parent_pid = current_task;
    tasks[pid].exit_status = 0;
    tasks[pid].wait_pid = -1;
    tasks[pid].exec_pending = 0;
    num_tasks++;

    printk("[OK] Task %u (\"%s\") created (user mode)\n",
           (uint64_t)pid, name ? name : "?");

    return (int)pid;
}

/* Cooperative yield via software interrupt.
 * Triggers INT 0x81 which goes through the ISR stub (saves full frame),
 * then yield_irq_handler forces a context switch via schedule_now(). */
void yield(void)
{
    __asm__ volatile("int $0x81");
}

/* --- Yield interrupt handler (INT 0x81) ---
 * Forces an immediate context switch (ignores quantum). */
static uint64_t yield_irq_handler(struct interrupt_frame *frame)
{
    return schedule_now(frame);
}

/* Force an immediate context switch (called from yield INT or schedule). */
uint64_t schedule_now(struct interrupt_frame *frame)
{
    uint32_t prev, next;

    if (num_tasks <= 1)
        return (uint64_t)frame;

    prev = current_task;
    next = find_next_task(prev);

    if (next == prev)
        return (uint64_t)frame;

    /* Save current task's interrupt frame pointer
     * (skip if exec_pending — don't overwrite the exec'd frame) */
    if (!tasks[prev].exec_pending)
        tasks[prev].rsp = (uint64_t)frame;
    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;

    /* Switch to next task */
    tasks[next].state = TASK_RUNNING;
    tasks[next].exec_pending = 0;  /* clear on switch-in */
    current_task = next;
    sched_ticks = 0;

    /* Update TSS rsp0 so ring 3→0 transitions use the correct kernel stack */
    if (tasks[next].kernel_rsp)
        tss_set_kernel_stack(tasks[next].kernel_rsp);

    return tasks[next].rsp;
}

/* --- Preemptive scheduler (called from PIT IRQ handler) ---
 *
 * The ISR stub has already saved all registers on the current task's stack.
 * 'frame' points to the saved register state.
 *
 * If it's time to switch:
 *   1. Save the current stack pointer (frame) into current task's TCB
 *   2. Pick the next task
 *   3. Return the next task's saved frame pointer
 *   4. The ISR stub restores from the new frame and iretq's into it
 *
 * If no switch needed, return the same frame pointer.
 */
uint64_t schedule(struct interrupt_frame *frame)
{
    uint32_t prev, next;

    if (!sched_enabled || num_tasks <= 1)
        return (uint64_t)frame;

    sched_ticks++;

    if (sched_ticks < SCHED_QUANTUM)
        return (uint64_t)frame;

    /* Time quantum expired — switch tasks */
    sched_ticks = 0;
    prev = current_task;
    next = find_next_task(prev);

    if (next == prev)
        return (uint64_t)frame;

    /* Save current task's interrupt frame pointer
     * (skip if exec_pending — don't overwrite the exec'd frame) */
    if (!tasks[prev].exec_pending)
        tasks[prev].rsp = (uint64_t)frame;
    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;

    /* Switch to next task */
    tasks[next].state = TASK_RUNNING;
    tasks[next].exec_pending = 0;  /* clear on switch-in */
    current_task = next;

    /* Update TSS rsp0 so ring 3→0 transitions use the correct kernel stack */
    if (tasks[next].kernel_rsp)
        tss_set_kernel_stack(tasks[next].kernel_rsp);

    /* Return the next task's saved frame pointer */
    return tasks[next].rsp;
}

void scheduler_enable(void)
{
    sched_ticks = 0;
    sched_enabled = 1;
}

void scheduler_disable(void)
{
    sched_enabled = 0;
}

struct task *task_current(void)
{
    return &tasks[current_task];
}

uint32_t task_count(void)
{
    return num_tasks;
}

/* ============================================================================
 * Process lifecycle functions
 * ============================================================================ */

/* Simple memcpy for stack duplication */
static void task_memcpy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

int task_fork(struct interrupt_frame *frame)
{
    uint32_t child_pid;
    uint8_t *kstack, *ustack;
    uint64_t *sp;
    uint64_t parent_pid_val = current_task;

    if (num_tasks >= TASK_MAX) {
        printk("[FAIL] task_fork: max tasks reached\n");
        return -1;
    }

    child_pid = num_tasks;

    /* Allocate kernel stack for child */
    kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!kstack) {
        printk("[FAIL] task_fork: cannot allocate kernel stack\n");
        return -1;
    }

    /* Allocate user stack for child */
    ustack = (uint8_t *)kmalloc(USER_STACK_SIZE);
    if (!ustack) {
        printk("[FAIL] task_fork: cannot allocate user stack\n");
        return -1;
    }

    /* Copy parent's user stack to child */
    if (tasks[parent_pid_val].user_stack_base) {
        task_memcpy(ustack, tasks[parent_pid_val].user_stack_base,
                    USER_STACK_SIZE);
    }

    /* Build the child's interrupt frame on its kernel stack.
     * Copy the parent's frame, but adjust: child gets rax=0, parent gets child_pid.
     * The child's RSP (in the frame) needs to point to the child's user stack
     * at the same relative offset as the parent's. */
    sp = (uint64_t *)(kstack + TASK_STACK_SIZE);
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);
    sp -= 22;

    /* Copy the parent's interrupt frame */
    task_memcpy((uint8_t *)sp, (const uint8_t *)frame, 22 * sizeof(uint64_t));

    /* Adjust child's user RSP to equivalent position in child's user stack */
    if (tasks[parent_pid_val].user_stack_base) {
        uint64_t parent_ustack_base = (uint64_t)tasks[parent_pid_val].user_stack_base;
        uint64_t parent_ustack_rsp = sp[20];  /* rsp in iretq frame */
        uint64_t offset = parent_ustack_rsp - parent_ustack_base;
        sp[20] = (uint64_t)ustack + offset;   /* child's equivalent RSP */
    }

    /* Child gets return value 0 */
    sp[14] = 0;  /* rax = 0 for child */

    /* Initialize child TCB */
    tasks[child_pid].pid = child_pid;
    tasks[child_pid].state = TASK_READY;
    tasks[child_pid].rsp = (uint64_t)sp;
    tasks[child_pid].stack_base = kstack;
    tasks[child_pid].kernel_rsp = (uint64_t)(kstack + TASK_STACK_SIZE);
    tasks[child_pid].user_stack_base = ustack;
    tasks[child_pid].name = tasks[parent_pid_val].name;
    tasks[child_pid].parent_pid = parent_pid_val;
    tasks[child_pid].exit_status = 0;
    tasks[child_pid].wait_pid = -1;
    tasks[child_pid].exec_pending = 0;
    num_tasks++;

    printk("[FORK] PID %u forked -> child PID %u\n",
           (uint64_t)parent_pid_val, (uint64_t)child_pid);

    /* Parent gets child_pid as return value */
    return (int)child_pid;
}

int task_exec(const uint8_t *data, uint64_t size)
{
    struct elf_load_result elf;
    uint64_t *sp;
    uint32_t pid = current_task;
    uint8_t *new_kstack;

    /* Load the ELF binary */
    elf = elf_load(data, size);
    if (!elf.success) {
        printk("[EXEC] Failed to load ELF\n");
        return -1;
    }

    /* Allocate a FRESH kernel stack - we cannot reuse the current one
     * because the calling function (exec_loader_func) is still on it. */
    new_kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!new_kstack) {
        printk("[EXEC] Cannot allocate kernel stack\n");
        return -1;
    }

    /* Free old user stack and allocate a fresh one */
    if (tasks[pid].user_stack_base) {
        kfree(tasks[pid].user_stack_base);
    }
    tasks[pid].user_stack_base = (uint8_t *)kmalloc(USER_STACK_SIZE);
    if (!tasks[pid].user_stack_base) {
        printk("[EXEC] Cannot allocate user stack\n");
        kfree(new_kstack);
        return -1;
    }

    /* Build a fresh interrupt frame on the NEW kernel stack */
    sp = (uint64_t *)(new_kstack + TASK_STACK_SIZE);
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);

    {
        uint64_t user_rsp = (uint64_t)(tasks[pid].user_stack_base +
                                        USER_STACK_SIZE) & ~0xFULL;
        sp -= 22;
        sp[0]  = 0;                                /* r15 */
        sp[1]  = 0;                                /* r14 */
        sp[2]  = 0;                                /* r13 */
        sp[3]  = 0;                                /* r12 */
        sp[4]  = 0;                                /* r11 */
        sp[5]  = 0;                                /* r10 */
        sp[6]  = 0;                                /* r9 */
        sp[7]  = 0;                                /* r8 */
        sp[8]  = 0;                                /* rbp */
        sp[9]  = 0;                                /* rdi */
        sp[10] = 0;                                /* rsi */
        sp[11] = 0;                                /* rdx */
        sp[12] = 0;                                /* rcx */
        sp[13] = 0;                                /* rbx */
        sp[14] = 0;                                /* rax */
        sp[15] = 0;                                /* int_no */
        sp[16] = 0;                                /* err_code */
        sp[17] = elf.entry;                        /* rip = ELF entry */
        sp[18] = GDT_USER_CODE | 3;                /* cs = user code, RPL=3 */
        sp[19] = 0x202;                            /* rflags: IF set */
        sp[20] = user_rsp;                         /* rsp = user stack */
        sp[21] = GDT_USER_DATA | 3;                /* ss = user data, RPL=3 */
    }

    /* Switch to new kernel stack and mark exec pending */
    tasks[pid].rsp = (uint64_t)sp;
    tasks[pid].stack_base = new_kstack;
    tasks[pid].kernel_rsp = (uint64_t)(new_kstack + TASK_STACK_SIZE);
    tasks[pid].exec_pending = 1;  /* prevent scheduler from overwriting this frame */

    printk("[EXEC] PID %u -> entry %p\n",
           (uint64_t)pid, elf.entry);

    return 0;
}

void task_exit(int32_t status)
{
    uint32_t pid = current_task;
    uint32_t i;

    tasks[pid].state = TASK_DEAD;
    tasks[pid].exit_status = status;

    printk("[SCHED] Task %u (\"%s\") exited with status %d\n",
           (uint64_t)pid,
           tasks[pid].name ? tasks[pid].name : "?",
           (uint64_t)(uint32_t)status);

    /* Wake parent if it's waiting on us */
    for (i = 0; i < num_tasks; i++) {
        if (tasks[i].state == TASK_WAITING &&
            tasks[i].wait_pid == (int32_t)pid) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_pid = -1;
            break;
        }
    }

    /* Yield away forever */
    for (;;)
        yield();
}

int32_t task_waitpid(uint32_t child_pid)
{
    /* Validate child PID */
    if (child_pid >= num_tasks || child_pid == current_task) {
        printk("[WAIT] Invalid child PID %u\n", (uint64_t)child_pid);
        return -1;
    }

    /* Verify this is actually our child */
    if (tasks[child_pid].parent_pid != current_task) {
        printk("[WAIT] PID %u is not a child of PID %u\n",
               (uint64_t)child_pid, (uint64_t)current_task);
        return -1;
    }

    /* If child is already dead, return immediately */
    if (tasks[child_pid].state == TASK_DEAD) {
        int32_t status = tasks[child_pid].exit_status;
        task_cleanup(child_pid);
        return status;
    }

    /* Block until child exits */
    tasks[current_task].state = TASK_WAITING;
    tasks[current_task].wait_pid = (int32_t)child_pid;

    /* Yield away — scheduler will skip us since we're TASK_WAITING */
    yield();

    /* When we wake up, child has exited */
    {
        int32_t status = tasks[child_pid].exit_status;
        task_cleanup(child_pid);
        return status;
    }
}

void task_cleanup(uint32_t pid)
{
    if (pid >= num_tasks || pid == 0)
        return;

    if (tasks[pid].state != TASK_DEAD)
        return;

    /* Free kernel stack */
    if (tasks[pid].stack_base) {
        kfree(tasks[pid].stack_base);
        tasks[pid].stack_base = (uint8_t *)0;
    }

    /* Free user stack */
    if (tasks[pid].user_stack_base) {
        kfree(tasks[pid].user_stack_base);
        tasks[pid].user_stack_base = (uint8_t *)0;
    }

    tasks[pid].kernel_rsp = 0;
    tasks[pid].rsp = 0;
}

