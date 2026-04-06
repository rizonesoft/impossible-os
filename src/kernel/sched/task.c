/* ============================================================================
 * task.c -- Kernel thread scheduler (cooperative + preemptive)
 *
 * Round-robin scheduling with fixed time quantum. The PIT timer IRQ
 * calls schedule() to preempt tasks automatically. Tasks can also
 * call yield() for cooperative switching.
 *
 * Task 0 is the boot/main thread -- it uses the existing kernel stack
 * and is created implicitly by task_init().
 * ============================================================================ */

#include "kernel/sched/task.h"
#include "kernel/idt.h"
#include "kernel/gdt.h"
#include "kernel/smp.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/user_range.h"
#include "kernel/cpuid.h"
#include "kernel/klog.h"
#include "kernel/exec.h"
#include "kernel/ipc/signal.h"
#include "kernel/ob/handle_table.h"
#include "kernel/ob/ob_process.h"
#include "kernel/ob/ob_thread.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/msr.h"
#include "kernel/ob/peb.h"
#include "kernel/ob/teb.h"
#include "kernel/acpi.h"
#include "kernel/timer.h"
#include "kernel/vectors.h"

/* Use VECTOR_YIELD from vectors.h (single source of truth) */
#define YIELD_INT_VECTOR VECTOR_YIELD

/* --- Task table --- */
static struct task tasks[TASK_MAX];
static uint32_t num_tasks = 0;
static uint32_t current_task = 0;
static uint32_t current_thread = 0;  /* thread index within current task */

/* --- Preemptive scheduler state --- */
static volatile uint32_t sched_enabled = 0;
static volatile uint32_t sched_ticks = 0;    /* ticks since last switch */

/* Forward declarations */
static uint64_t yield_irq_handler(struct interrupt_frame *frame);
uint64_t schedule_now(struct interrupt_frame *frame);

/* exec_pending stuck detection threshold (ticks).
 * If exec_pending has been set for more than this many ticks without the
 * task being scheduled in, the frame is stuck and will never be consumed.
 * Force-clear and log error so the task doesn't block forever. */
#define EXEC_PENDING_STUCK_TICKS 10

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

    /* Task finished -- mark as dead */
    tasks[current_task].state = TASK_DEAD;
    klog(LOG_DEBUG, "sched", "Task %u (\"%s\") exited",
           (uint64_t)tasks[current_task].pid,
           tasks[current_task].name ? tasks[current_task].name : "?");

    /* Yield forever -- yield() via INT 0x81 always works,
     * whether preemptive scheduler is enabled or not. */
    for (;;)
        yield();
}

/* --- Find the next runnable task+thread (priority-aware) ---
 * Scans ALL tasks × threads and returns the highest effective-priority
 * READY/RUNNING thread.  Round-robin order within equal-priority threads:
 * we still start from the slot *after* the current one so that same-priority
 * threads each get a fair turn.
 * Sets *out_thread to the thread index within the found task.
 * Returns the task index (or from_task/from_thread if nothing else found). */
static uint32_t find_next_task(uint32_t from_task, uint32_t from_thread,
                               uint32_t *out_thread)
{
    uint32_t best_task   = from_task;
    uint32_t best_thread = from_thread;
    uint32_t best_prio   = 0;
    int      found       = 0;

    uint32_t ti;

    /* Scan all tasks, walking threads in round-robin order so equal-priority
     * threads are visited in circular order starting from the current slot. */
    for (ti = 0; ti < num_tasks; ti++) {
        uint32_t task_idx = (from_task + 1 + ti) % num_tasks;
        uint32_t tj;

        if (tasks[task_idx].state != TASK_READY &&
            tasks[task_idx].state != TASK_RUNNING)
            continue;

        /* Walk threads in round-robin order within the task */
        for (tj = 0; tj < tasks[task_idx].num_threads; tj++) {
            uint32_t thread_idx;
            struct thread *thr;

            /* Determine start offset for fairness within same task */
            if (task_idx == from_task)
                thread_idx = (from_thread + 1 + tj) % tasks[task_idx].num_threads;
            else
                thread_idx = tj;

            thr = &tasks[task_idx].threads[thread_idx];

            if (thr->state != THREAD_READY && thr->state != THREAD_RUNNING)
                continue;

            if (!found || thr->priority > best_prio) {
                best_task   = task_idx;
                best_thread = thread_idx;
                best_prio   = thr->priority;
                found = 1;
            }
        }
    }

    if (!found) {
        /* Nothing else runnable -- stay on current */
        *out_thread = from_thread;
        return from_task;
    }

    *out_thread = best_thread;
    return best_task;
}

/* --- #NM handler: lazy FPU allocation on first SIMD/FP use --- */

static uint64_t nm_handler(struct interrupt_frame *frame)
{
    struct task *t;

    /* Clear CR0.TS so the faulting instruction can retry */
    __asm__ volatile ("clts");

    t = &tasks[current_task];
    if (!t->fpu_used) {
        /* First FPU use -- allocate page-aligned XSAVE area */
        task_alloc_xsave(t);
        t->fpu_used = 1;
    }

    return (uint64_t)frame;
}

/* --- XSAVE area allocation (lazy, on first FPU use) ---
 *
 * Alignment requirements (x86-64 SDM):
 *   XSAVE/XRSTOR:   64-byte aligned (else #GP)
 *   FXSAVE/FXRSTOR: 16-byte aligned (else #GP)
 *   XSAVEOPT:       64-byte aligned (else #GP)
 *
 * Current allocation uses pmm_alloc_contiguous() which returns
 * page-aligned (4096-byte) addresses, satisfying all requirements.
 * If allocation ever changes to kmalloc or slab, the 64-byte
 * alignment invariant MUST be preserved -- the runtime assert
 * below catches any future violation immediately.
 */

#define XSAVE_ALIGN     64   /* XSAVE/XRSTOR minimum alignment */
#define FXSAVE_SIZE     512  /* legacy FXSAVE area size */

void task_alloc_xsave(struct task *t)
{
    extern struct cpu_features g_cpu;
    uint32_t size, pages;

    if (!t || t->xsave_area)
        return;  /* already allocated */

    size = g_cpu.xsave_size_max;
    if (size == 0) size = FXSAVE_SIZE;  /* fallback: legacy FXSAVE size */

    /* Round up to page boundary -- PMM returns page-aligned (4096),
     * which satisfies the 64-byte XSAVE alignment requirement. */
    pages = (size + 4095) / 4096;
    t->xsave_area = (void *)pmm_alloc_contiguous(pages);

    if (t->xsave_area) {
        /* Runtime alignment verify: #GP if this invariant is broken.
         * Catches future allocation changes (kmalloc, slab) that
         * might not guarantee 64-byte alignment. */
        if ((uintptr_t)t->xsave_area & (XSAVE_ALIGN - 1)) {
            klog(LOG_FATAL, "sched",
                 "XSAVE area at %p not 64-byte aligned -- #GP on save/restore",
                 (uint64_t)(uintptr_t)t->xsave_area);
            t->xsave_area = (void *)0;
            return;
        }

        /* Zero the buffer */
        uint8_t *p = (uint8_t *)t->xsave_area;
        uint32_t i;
        for (i = 0; i < pages * 4096; i++)
            p[i] = 0;

        /* Set XSTATE_BV header: bit 0 = x87 initial state present */
        if (size >= FXSAVE_SIZE + 64) {
            /* XSAVE header at offset 512, 64 bytes.
             * XSTATE_BV (offset 512, 8 bytes) = 0x01 (x87 state valid) */
            p[FXSAVE_SIZE] = 0x01;
        }
    }
}

/* Compile-time: XSAVE alignment must be power of 2 and >= 64 */
_Static_assert(XSAVE_ALIGN == 64, "XSAVE requires 64-byte alignment (x86-64 SDM)");
_Static_assert((XSAVE_ALIGN & (XSAVE_ALIGN - 1)) == 0, "XSAVE_ALIGN must be power of 2");
_Static_assert(FXSAVE_SIZE == 512, "FXSAVE area is 512 bytes (x86-64 SDM)");

/* --- Public API --- */

boot_result_t task_init(void)
{
    uint32_t i, j;

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
        tasks[i].cr3 = 0;
        tasks[i].kernel_gs_base = 0;
        tasks[i].tls_bitmap = 0;
        tasks[i].xsave_area = (void *)0;
        tasks[i].fpu_used = 0;
        tasks[i].handle_table.entries  = NULL;
        tasks[i].handle_table.capacity = 0;
        tasks[i].handle_table.count    = 0;
        tasks[i].num_threads = 0;
        for (j = 0; j < THREAD_MAX; j++) {
            tasks[i].threads[j].id = 0;
            tasks[i].threads[j].state = THREAD_DEAD;
            tasks[i].threads[j].rsp = 0;
            tasks[i].threads[j].stack_base = (uint8_t *)0;
            tasks[i].threads[j].stack_size = 0;
            tasks[i].threads[j].parent_task = 0;
            tasks[i].threads[j].exit_status = 0;
            tasks[i].threads[j].join_tid = -1;
        }
    }

    /* Task 0: the current boot/main thread.
     * Its stack is the existing kernel boot stack -- we don't allocate one.
     * RSP will be saved by switch_context/schedule when it first yields. */
    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack_base = (uint8_t *)0;  /* boot stack, don't free */
    tasks[0].name = "main";
    num_tasks = 1;
    current_task = 0;

    /* Thread 0 = main thread (implicit, uses task's own stack) */
    tasks[0].threads[0].id = 0;
    tasks[0].threads[0].state = THREAD_RUNNING;
    tasks[0].threads[0].stack_base = (uint8_t *)0;  /* boot stack */
    tasks[0].threads[0].stack_size = 0;
    tasks[0].threads[0].parent_task = 0;
    tasks[0].threads[0].join_tid = -1;
    tasks[0].threads[0].priority      = THREAD_PRIO_NORMAL;
    tasks[0].threads[0].base_priority = THREAD_PRIO_NORMAL;
    tasks[0].num_threads = 1;

    /* Initialize signal state for PID 0 */
    signal_init_task(&tasks[0].signals);

    /* Initialize handle table for PID 0 */
    ob_handle_table_init(&tasks[0].handle_table);

    klog(LOG_DEBUG, "sched", "Scheduler initialized (PID 0 = main, quantum = %u ticks)",
           (uint64_t)SCHED_QUANTUM);

    /* Register the yield software interrupt handler (INT 0x81) */
    idt_register_handler(YIELD_INT_VECTOR, yield_irq_handler);

    /* Register #NM handler for lazy FPU (vector 7 = Device Not Available).
     * Must be unconditional -- schedule() sets CR0.TS on all platforms,
     * including TCG where XSAVE is absent but SSE2 SIMD is used. */
    idt_register_handler(7, nm_handler);

    return BOOT_OK;
}

int task_create(task_entry_t entry, const char *name)
{
    uint32_t pid;
    uint8_t *stack;
    uint64_t *sp;

    if (num_tasks >= TASK_MAX) {
        klog(LOG_ERROR, "sched", "task_create: max tasks reached");
        return -1;
    }

    pid = num_tasks;

    /* Allocate task stack from PMM with guard page at the bottom.
     * Stack grows down, so guard page catches overflow before it
     * corrupts adjacent memory.  PMM gives identity-mapped pages. */
    {
        uint32_t stack_pages = TASK_STACK_SIZE / 4096;
        uintptr_t stack_base = pmm_alloc_contiguous(stack_pages + 1);
        if (!stack_base) {
            klog(LOG_ERROR, "sched", "task_create: cannot allocate stack");
            return -1;
        }
        vmm_install_guard_page(stack_base, "GUARD: kernel task stack overflow");
        stack = (uint8_t *)(stack_base + 4096);  /* usable stack after guard */
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
     * iretq pops RIP, CS, RFLAGS, RSP, SS -- the RSP in the frame tells
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
    tasks[pid].cr3 = 0;  /* kernel task uses boot PML4 */

    /* Thread 0 = main thread (uses task's kernel stack) */
    tasks[pid].threads[0].id = 0;
    tasks[pid].threads[0].state = THREAD_READY;
    tasks[pid].threads[0].stack_base = (uint8_t *)0;  /* shares task stack */
    tasks[pid].threads[0].stack_size = 0;
    tasks[pid].threads[0].parent_task = pid;
    tasks[pid].threads[0].join_tid = -1;
    tasks[pid].threads[0].priority      = THREAD_PRIO_NORMAL;
    tasks[pid].threads[0].base_priority = THREAD_PRIO_NORMAL;
    tasks[pid].num_threads = 1;
    signal_init_task(&tasks[pid].signals);
    ob_handle_table_init(&tasks[pid].handle_table);
    num_tasks++;

    /* Register process and main thread with Object Manager */
    ob_process_create(&tasks[pid]);
    ob_thread_create(&tasks[pid].threads[0], pid);

    klog(LOG_DEBUG, "sched", "Task %u (\"%s\") created (kernel)",
           (uint64_t)pid, name ? name : "?");

    return (int)pid;
}

int task_create_user(task_entry_t entry, const char *name)
{
    uint32_t pid;
    uint8_t *kstack, *ustack;
    uint64_t *sp;

    if (num_tasks >= TASK_MAX) {
        klog(LOG_ERROR, "sched", "task_create_user: max tasks reached");
        return -1;
    }

    pid = num_tasks;

    /* Allocate kernel stack (for interrupt/syscall handling) */
    kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!kstack) {
        klog(LOG_ERROR, "sched", "task_create_user: cannot allocate kernel stack");
        return -1;
    }

    /* User stack: placed at the top of the user region (USER_ELF_BASE..USER_ELF_END)
     * which is identity-mapped in the split PD[USER_PD_INDEX] PT. Physical pages
     * are reserved by pmm_mark_region_used(USER_ELF_BASE, USER_ELF_SIZE).
     * Stack top at USER_ELF_END, grows down. */
    ustack = (uint8_t *)(USER_ELF_END - USER_STACK_SIZE);

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

    /* Per-process page table: clone kernel PML4, mark ELF + user stack as User */
    {
        uintptr_t user_cr3 = vmm_create_user_pml4();
        if (user_cr3) {
            uintptr_t addr;
            /* Mark ELF pages (USER_ELF_BASE range) as User */
            for (addr = USER_ELF_BASE; addr < USER_ELF_END; addr += 4096)
                vmm_set_user_page(user_cr3, addr);
            /* Mark user stack pages as User */
            for (addr = (uintptr_t)ustack;
                 addr < (uintptr_t)ustack + USER_STACK_SIZE;
                 addr += 4096)
                vmm_set_user_page(user_cr3, addr);
            tasks[pid].cr3 = user_cr3;
        } else {
            tasks[pid].cr3 = 0;
            klog(LOG_WARN, "sched", "Task %u: per-process PML4 failed", (uint64_t)pid);
        }
    }

    /* Thread 0 = main thread (uses task's kernel stack) */
    tasks[pid].threads[0].id = 0;
    tasks[pid].threads[0].state = THREAD_READY;
    tasks[pid].threads[0].stack_base = (uint8_t *)0;  /* shares task stack */
    tasks[pid].threads[0].stack_size = 0;
    tasks[pid].threads[0].parent_task = pid;
    tasks[pid].threads[0].join_tid = -1;
    tasks[pid].threads[0].priority      = THREAD_PRIO_NORMAL;
    tasks[pid].threads[0].base_priority = THREAD_PRIO_NORMAL;
    tasks[pid].num_threads = 1;
    signal_init_task(&tasks[pid].signals);
    ob_handle_table_init(&tasks[pid].handle_table);
    num_tasks++;

    /* Register process and main thread with Object Manager */
    ob_process_create(&tasks[pid]);
    ob_thread_create(&tasks[pid].threads[0], pid);
    klog(LOG_DEBUG, "sched", "Task %u (\"%s\") created (user mode)",
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
    uint32_t prev_task, prev_thread;
    uint32_t next_task, next_thread;

    if (num_tasks <= 1 && tasks[0].num_threads <= 1)
        return (uint64_t)frame;

    prev_task = current_task;
    prev_thread = current_thread;
    next_task = find_next_task(prev_task, prev_thread, &next_thread);

    if (next_task == prev_task && next_thread == prev_thread)
        return (uint64_t)frame;

    /* Save current task/thread's interrupt frame pointer
     * (skip if exec_pending -- don't overwrite the exec'd frame) */
    if (!tasks[prev_task].exec_pending) {
        /* Save to thread if multi-threaded, otherwise to task */
        if (prev_thread > 0)
            tasks[prev_task].threads[prev_thread].rsp = (uint64_t)frame;
        else
            tasks[prev_task].rsp = (uint64_t)frame;
    }
    if (tasks[prev_task].state == TASK_RUNNING)
        tasks[prev_task].state = TASK_READY;
    if (tasks[prev_task].threads[prev_thread].state == THREAD_RUNNING)
        tasks[prev_task].threads[prev_thread].state = THREAD_READY;

    /* Switch to next task/thread */
    tasks[next_task].state = TASK_RUNNING;
    tasks[next_task].threads[next_thread].state = THREAD_RUNNING;

    /* exec_pending stuck detection: if set for too long, frame was never consumed */
    if (tasks[next_task].exec_pending &&
        tasks[next_task].exec_pending_tick &&
        (uptime() - tasks[next_task].exec_pending_tick) > EXEC_PENDING_STUCK_TICKS) {
        klog(LOG_WARN, "sched", "PID %u: exec_pending stuck for %u ticks -- force-clearing",
             (uint64_t)next_task,
             (uint64_t)(uptime() - tasks[next_task].exec_pending_tick));
    }
    tasks[next_task].exec_pending = 0;  /* clear on switch-in */
    tasks[next_task].exec_pending_tick = 0;
    current_task = next_task;
    current_thread = next_thread;
    sched_ticks = 0;

    /* Update TSS rsp0 + per-CPU syscall_rsp0 for the correct kernel stack */
    if (tasks[next_task].kernel_rsp) {
        tss_set_kernel_stack(tasks[next_task].kernel_rsp);
        smp_this_cpu()->syscall_rsp0 = tasks[next_task].kernel_rsp;
    }

    /* CR3 switch: load per-process page tables if different from current */
    {
        uintptr_t new_cr3 = tasks[next_task].cr3;
        if (!new_cr3)
            new_cr3 = vmm_get_kernel_cr3();
        uintptr_t cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        if (new_cr3 != cur_cr3)
            __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    }

    /* KERNEL_GS_BASE switch: save prev task's TEB, load next task's TEB.
     * swapgs in the ISR stub handles ring transition; this handles
     * switching between different user-mode tasks with different TEBs. */
    if (prev_task != next_task) {
        tasks[prev_task].kernel_gs_base = msr_read(MSR_IA32_KERNEL_GS_BASE);
        msr_write(MSR_IA32_KERNEL_GS_BASE, tasks[next_task].kernel_gs_base);
    }

    /* Return the correct RSP: thread stack if secondary thread, task stack otherwise */
    if (next_thread > 0)
        return tasks[next_task].threads[next_thread].rsp;
    return tasks[next_task].rsp;
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
    uint32_t prev_task, prev_thread;
    uint32_t next_task, next_thread;

    if (!sched_enabled || num_tasks <= 1)
        return (uint64_t)frame;

    sched_ticks++;

    if (sched_ticks < SCHED_QUANTUM)
        return (uint64_t)frame;

    /* Time quantum expired -- switch */
    sched_ticks = 0;
    prev_task = current_task;
    prev_thread = current_thread;
    next_task = find_next_task(prev_task, prev_thread, &next_thread);

    if (next_task == prev_task && next_thread == prev_thread)
        return (uint64_t)frame;

    /* Save current task/thread's interrupt frame pointer
     * (skip if exec_pending -- don't overwrite the exec'd frame) */
    if (!tasks[prev_task].exec_pending) {
        if (prev_thread > 0)
            tasks[prev_task].threads[prev_thread].rsp = (uint64_t)frame;
        else
            tasks[prev_task].rsp = (uint64_t)frame;
    }
    if (tasks[prev_task].state == TASK_RUNNING)
        tasks[prev_task].state = TASK_READY;
    if (tasks[prev_task].threads[prev_thread].state == THREAD_RUNNING)
        tasks[prev_task].threads[prev_thread].state = THREAD_READY;

    /* --- Lazy FPU: save prev, restore/defer next ---
     * Use XSAVE/XRSTOR when available (AVX/AVX-512 state), fall back
     * to FXSAVE/FXRSTOR on CPUs without XSAVE (e.g., QEMU TCG). */
    {
    extern struct cpu_features g_cpu;
    int have_xsave = cpu_has(CPU_FEATURE_XSAVE);

    if (tasks[prev_task].fpu_used && tasks[prev_task].xsave_area) {
        if (have_xsave) {
            uint64_t xcr0 = g_cpu.xcr0_active;
            uint32_t lo = (uint32_t)xcr0;
            uint32_t hi = (uint32_t)(xcr0 >> 32);
            if (cpu_has(CPU_FEATURE_XSAVEOPT))
                __asm__ volatile ("xsaveopt %0" : "=m"(*(uint8_t *)tasks[prev_task].xsave_area)
                                  : "a"(lo), "d"(hi) : "memory");
            else
                __asm__ volatile ("xsave %0" : "=m"(*(uint8_t *)tasks[prev_task].xsave_area)
                                  : "a"(lo), "d"(hi) : "memory");
        } else {
            __asm__ volatile ("fxsave %0" : "=m"(*(uint8_t *)tasks[prev_task].xsave_area) : : "memory");
        }
    }

    if (tasks[next_task].fpu_used && tasks[next_task].xsave_area) {
        if (have_xsave) {
            uint64_t xcr0 = g_cpu.xcr0_active;
            uint32_t lo = (uint32_t)xcr0;
            uint32_t hi = (uint32_t)(xcr0 >> 32);
            __asm__ volatile ("xrstor %0" : : "m"(*(uint8_t *)tasks[next_task].xsave_area),
                              "a"(lo), "d"(hi) : "memory");
        } else {
            __asm__ volatile ("fxrstor %0" : : "m"(*(uint8_t *)tasks[next_task].xsave_area) : "memory");
        }
        __asm__ volatile ("clts");
    } else {
        uint64_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= (1UL << 3);  /* CR0.TS */
        __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    }
    }

    /* Switch to next task/thread */
    tasks[next_task].state = TASK_RUNNING;
    tasks[next_task].threads[next_thread].state = THREAD_RUNNING;

    /* exec_pending stuck detection (same as yield path) */
    if (tasks[next_task].exec_pending &&
        tasks[next_task].exec_pending_tick &&
        (uptime() - tasks[next_task].exec_pending_tick) > EXEC_PENDING_STUCK_TICKS) {
        klog(LOG_WARN, "sched", "PID %u: exec_pending stuck for %u ticks -- force-clearing",
             (uint64_t)next_task,
             (uint64_t)(uptime() - tasks[next_task].exec_pending_tick));
    }
    tasks[next_task].exec_pending = 0;
    tasks[next_task].exec_pending_tick = 0;
    current_task = next_task;
    current_thread = next_thread;

    /* Update TSS rsp0 + per-CPU syscall_rsp0 for the correct kernel stack */
    if (tasks[next_task].kernel_rsp) {
        tss_set_kernel_stack(tasks[next_task].kernel_rsp);
        smp_this_cpu()->syscall_rsp0 = tasks[next_task].kernel_rsp;
    }

    /* CR3 switch: load per-process page tables if different from current */
    {
        uintptr_t new_cr3 = tasks[next_task].cr3;
        if (!new_cr3)
            new_cr3 = vmm_get_kernel_cr3();
        uintptr_t cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        if (new_cr3 != cur_cr3)
            __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    }

    /* KERNEL_GS_BASE switch: save prev task's TEB, load next task's TEB */
    if (prev_task != next_task) {
        tasks[prev_task].kernel_gs_base = msr_read(MSR_IA32_KERNEL_GS_BASE);
        msr_write(MSR_IA32_KERNEL_GS_BASE, tasks[next_task].kernel_gs_base);
    }

    /* Return the correct RSP */
    if (next_thread > 0)
        return tasks[next_task].threads[next_thread].rsp;
    return tasks[next_task].rsp;
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

struct task *task_get_by_pid(uint32_t pid)
{
    if (pid >= num_tasks)
        return (struct task *)0;
    return &tasks[pid];
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
        klog(LOG_ERROR, "sched", "task_fork: max tasks reached");
        return -1;
    }

    child_pid = num_tasks;

    /* Allocate kernel stack for child */
    kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!kstack) {
        klog(LOG_ERROR, "sched", "task_fork: cannot allocate kernel stack");
        return -1;
    }

    /* Allocate user stack for child */
    ustack = (uint8_t *)kmalloc(USER_STACK_SIZE);
    if (!ustack) {
        klog(LOG_ERROR, "sched", "task_fork: cannot allocate user stack");
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
    ob_handle_table_init(&tasks[child_pid].handle_table);
    /* Copy parent's KERNEL_GS_BASE (TEB address) -- §6 will allocate
     * a new TEB for the child and update this field. */
    tasks[child_pid].kernel_gs_base = tasks[parent_pid_val].kernel_gs_base;
    num_tasks++;

    /* Register forked process and main thread with Object Manager */
    ob_process_create(&tasks[child_pid]);
    ob_thread_create(&tasks[child_pid].threads[0], child_pid);

    klog(LOG_DEBUG, "sched", "PID %u forked -> child PID %u",
           (uint64_t)parent_pid_val, (uint64_t)child_pid);

    /* Parent gets child_pid as return value */
    return (int)child_pid;
}

/* ---- PEB / RTL_USER_PROCESS_PARAMETERS allocation ----------------------- */

/* Fixed user-mode addresses (matches Windows x64 defaults) */
#define PEB_USER_ADDR   0x7FFDE000ULL
#define RTLPP_USER_ADDR 0x7FFDD000ULL  /* RTL_USER_PROCESS_PARAMETERS page */
#define ENV_USER_ADDR   0x7FFDC000ULL  /* Environment block page */

/* Helper: write a UTF-16 UNICODE_STRING from an ASCII source.
 * Writes UTF-16 data at *buf_pos, advances it, and fills us. */
static void peb_build_ustr(UNICODE_STRING *us, uint16_t **buf_pos,
                            const char *ascii)
{
    uint16_t *start = *buf_pos;
    while (*ascii)
        *(*buf_pos)++ = (uint16_t)(uint8_t)*ascii++;
    *(*buf_pos)++ = 0;  /* NUL terminator */
    uint32_t byte_len = (uint32_t)((uintptr_t)*buf_pos - (uintptr_t)start - 2);
    us->Length = (uint16_t)byte_len;
    us->MaximumLength = (uint16_t)(byte_len + 2);
    us->_pad = 0;
    us->Buffer = start;
}

/* Allocate and populate PEB + RTL_USER_PROCESS_PARAMETERS for a user task.
 * Returns PEB pointer (user-space address) or NULL on failure. */
static PEB *peb_alloc_for_task(uint32_t pid, uintptr_t image_base,
                                const char *name)
{
    (void)pid;  /* reserved for future per-process page table */
    uintptr_t peb_phys, rtlpp_phys, env_phys;
    PEB *peb;
    RTL_USER_PROCESS_PARAMETERS *pp;
    uint16_t *env;

    /* Allocate physical pages */
    peb_phys = pmm_alloc_frame();
    rtlpp_phys = pmm_alloc_frame();
    env_phys = pmm_alloc_frame();
    if (!peb_phys || !rtlpp_phys || !env_phys) {
        if (peb_phys) pmm_free_frame(peb_phys);
        if (rtlpp_phys) pmm_free_frame(rtlpp_phys);
        if (env_phys) pmm_free_frame(env_phys);
        return (PEB *)0;
    }

    /* Map into user address space */
    vmm_map_page(PEB_USER_ADDR, peb_phys, VMM_USER_RW);
    vmm_map_page(RTLPP_USER_ADDR, rtlpp_phys, VMM_USER_RW);
    vmm_map_page(ENV_USER_ADDR, env_phys, VMM_USER_RW);

    /* Zero all pages */
    peb = (PEB *)PEB_USER_ADDR;
    pp = (RTL_USER_PROCESS_PARAMETERS *)RTLPP_USER_ADDR;
    env = (uint16_t *)ENV_USER_ADDR;

    {
        uint8_t *p;
        uint32_t i;
        p = (uint8_t *)peb;
        for (i = 0; i < 4096; i++) p[i] = 0;
        p = (uint8_t *)pp;
        for (i = 0; i < 4096; i++) p[i] = 0;
        p = (uint8_t *)env;
        for (i = 0; i < 4096; i++) p[i] = 0;
    }

    /* ---- Populate PEB ---- */
    peb->BeingDebugged = 0;
    peb->ImageBaseAddress = (void *)image_base;
    peb->ProcessParameters = pp;
    peb->OSMajorVersion = 10;
    peb->OSMinorVersion = 0;
    peb->OSBuildNumber = 22621;    /* Windows 11 22H2 */
    peb->OSPlatformId = 2;        /* VER_PLATFORM_WIN32_NT */
    peb->NumberOfProcessors = acpi_get_cpu_count();

    /* ---- Populate RTL_USER_PROCESS_PARAMETERS ---- */
    pp->MaximumLength = sizeof(RTL_USER_PROCESS_PARAMETERS);
    pp->Length = sizeof(RTL_USER_PROCESS_PARAMETERS);

    /* Build UNICODE_STRING fields in the RTLPP page after the struct */
    {
        uint16_t *buf = (uint16_t *)((uint8_t *)pp +
                         sizeof(RTL_USER_PROCESS_PARAMETERS));

        /* ImagePathName */
        if (name && name[0])
            peb_build_ustr(&pp->ImagePathName, &buf, name);

        /* CommandLine (same as image path for now) */
        if (name && name[0])
            peb_build_ustr(&pp->CommandLine, &buf, name);

        /* CurrentDirectory */
        peb_build_ustr(&pp->CurrentDirectoryDosPath, &buf, "C:\\");
    }

    /* Standard handles: INVALID for now (§6 wires real console handles) */
    pp->StandardInput = UHANDLE_INVALID;
    pp->StandardOutput = UHANDLE_INVALID;
    pp->StandardError = UHANDLE_INVALID;

    /* ---- Environment block ---- */
    pp->Environment = (void *)ENV_USER_ADDR;
    {
        uint16_t *ep = env;
        /* PATH=C:\Impossible\System32\ */
        const char *path = "PATH=C:\\Impossible\\System32\\";
        while (*path)
            *ep++ = (uint16_t)(uint8_t)*path++;
        *ep++ = 0;  /* terminate this variable */
        /* SystemRoot=C:\Impossible */
        const char *sysroot = "SystemRoot=C:\\Impossible";
        while (*sysroot)
            *ep++ = (uint16_t)(uint8_t)*sysroot++;
        *ep++ = 0;
        /* Double-NUL terminates the block */
        *ep++ = 0;
    }

    /* ---- PEB Ldr -- minimal module list with main executable ----
     * Place PEB_LDR_DATA + LDR_DATA_TABLE_ENTRY in the PEB page after
     * the PEB struct (offset 0x240+). Plenty of room in the 4 KB page. */
    {
        PEB_LDR_DATA *ldr = (PEB_LDR_DATA *)((uint8_t *)peb + 0x800);
        LDR_DATA_TABLE_ENTRY *mod = (LDR_DATA_TABLE_ENTRY *)(
            (uint8_t *)ldr + sizeof(PEB_LDR_DATA));
        uint16_t *str_buf = (uint16_t *)(
            (uint8_t *)mod + sizeof(LDR_DATA_TABLE_ENTRY));

        /* Zero both structs */
        {
            uint8_t *p = (uint8_t *)ldr;
            uint32_t i;
            uint32_t total = sizeof(PEB_LDR_DATA) +
                             sizeof(LDR_DATA_TABLE_ENTRY);
            for (i = 0; i < total; i++) p[i] = 0;
        }

        /* PEB_LDR_DATA */
        ldr->Length = sizeof(PEB_LDR_DATA);
        ldr->Initialized = 1;

        /* Self-referencing list heads (empty list = Flink/Blink → self) */
        ldr->InLoadOrderModuleList.Flink = &ldr->InLoadOrderModuleList;
        ldr->InLoadOrderModuleList.Blink = &ldr->InLoadOrderModuleList;
        ldr->InMemoryOrderModuleList.Flink = &ldr->InMemoryOrderModuleList;
        ldr->InMemoryOrderModuleList.Blink = &ldr->InMemoryOrderModuleList;
        ldr->InInitializationOrderModuleList.Flink =
            &ldr->InInitializationOrderModuleList;
        ldr->InInitializationOrderModuleList.Blink =
            &ldr->InInitializationOrderModuleList;

        /* LDR_DATA_TABLE_ENTRY for main executable */
        mod->DllBase = (void *)image_base;
        mod->EntryPoint = (void *)image_base;  /* _start */
        mod->SizeOfImage = 0x20000;  /* approximate; refined by PE loader */
        mod->Flags = 0x00004000;     /* LDRP_ENTRY_PROCESSED */
        mod->LoadCount = 1;

        /* Build FullDllName and BaseDllName UNICODE_STRINGs */
        peb_build_ustr(&mod->FullDllName, &str_buf, name ? name : "a.out");
        peb_build_ustr(&mod->BaseDllName, &str_buf, name ? name : "a.out");

        /* Insert module into all three lists (single element: circular) */
        mod->InLoadOrderLinks.Flink = &ldr->InLoadOrderModuleList;
        mod->InLoadOrderLinks.Blink = &ldr->InLoadOrderModuleList;
        ldr->InLoadOrderModuleList.Flink = &mod->InLoadOrderLinks;
        ldr->InLoadOrderModuleList.Blink = &mod->InLoadOrderLinks;

        mod->InMemoryOrderLinks.Flink = &ldr->InMemoryOrderModuleList;
        mod->InMemoryOrderLinks.Blink = &ldr->InMemoryOrderModuleList;
        ldr->InMemoryOrderModuleList.Flink = &mod->InMemoryOrderLinks;
        ldr->InMemoryOrderModuleList.Blink = &mod->InMemoryOrderLinks;

        mod->InInitializationOrderLinks.Flink =
            &ldr->InInitializationOrderModuleList;
        mod->InInitializationOrderLinks.Blink =
            &ldr->InInitializationOrderModuleList;
        ldr->InInitializationOrderModuleList.Flink =
            &mod->InInitializationOrderLinks;
        ldr->InInitializationOrderModuleList.Blink =
            &mod->InInitializationOrderLinks;

        /* Wire PEB->Ldr */
        peb->Ldr = ldr;
    }

    return peb;
}

/* ---- TEB allocation ----------------------------------------------------- */

/* Base address for TEB pages -- one page per thread, growing downward */
#define TEB_USER_BASE   0x7FFDB000ULL  /* below env block at 0x7FFDC000 */

/* Allocate and populate a TEB for a user-mode thread.
 * Returns TEB pointer (user-space address) or NULL on failure. */
static TEB *teb_alloc_for_task(uint32_t pid, uint32_t tid,
                                uintptr_t user_stack_base_addr,
                                uint32_t user_stack_size,
                                void *peb_addr)
{
    uintptr_t teb_virt = TEB_USER_BASE - (uintptr_t)tid * 0x1000;
    uintptr_t teb_phys = pmm_alloc_frame();
    TEB *teb;

    if (!teb_phys) return (TEB *)0;

    vmm_map_page(teb_virt, teb_phys, VMM_USER_RW);

    /* Zero the page */
    teb = (TEB *)teb_virt;
    {
        uint8_t *p = (uint8_t *)teb;
        uint32_t i;
        for (i = 0; i < 4096; i++) p[i] = 0;
    }

    /* NT_TIB */
    teb->NtTib.Self = &teb->NtTib;        /* gs:[0x30] → TEB self-pointer */
    teb->NtTib.StackBase = (void *)(user_stack_base_addr + user_stack_size);
    teb->NtTib.StackLimit = (void *)user_stack_base_addr;
    teb->NtTib.ExceptionList = (void *)0xFFFFFFFFFFFFFFFFULL; /* no SEH */

    /* ClientId */
    teb->ClientId.UniqueProcess = (uint64_t)pid;
    teb->ClientId.UniqueThread = (uint64_t)tid;

    /* PEB pointer -- gs:[0x60] */
    teb->ProcessEnvironmentBlock = (struct peb *)peb_addr;

    /* LastErrorValue -- gs:[0x68] */
    teb->LastErrorValue = 0;

    return teb;
}

int task_exec(const uint8_t *data, uint64_t size)
{
    uint64_t entry;
    int exec_err = 0;
    uint64_t *sp;
    uint32_t pid = current_task;
    uint8_t *new_kstack;

    /* Load the binary via multi-format dispatcher (ELF, PE32+, EIF) */
    entry = exec_load(data, size, &exec_err);
    if (entry == 0) {
        klog(LOG_DEBUG, "sched", "exec_load failed (err=%u)", (uint64_t)exec_err);
        return -1;
    }

    /* Register loaded module in the global crash registry and PEB->Ldr (§6).
     * Uses the identity-mapped user ELF range as the module base/size.
     * The format field is best-effort: detect from magic bytes in data. */
    {
        loaded_module_t mod;
        uint8_t *mp = (uint8_t *)&mod;
        uint32_t mi;
        for (mi = 0; mi < sizeof(mod); mi++) mp[mi] = 0;

        mod.base_address = USER_ELF_BASE;
        mod.size_of_image = USER_ELF_END - USER_ELF_BASE;
        mod.entry_point = entry;

        /* Detect format from magic */
        if (size >= 4 && data[0] == 0x7F && data[1] == 'E' &&
            data[2] == 'L' && data[3] == 'F')
            mod.format = EXEC_FMT_ELF;
        else if (size >= 4 && data[0] == 'E' && data[1] == 'I' &&
                 data[2] == 'F' && data[3] == '!')
            mod.format = EXEC_FMT_EIF;
        else if (size >= 2 && data[0] == 'M' && data[1] == 'Z')
            mod.format = EXEC_FMT_PE;

        /* Name from task name */
        {
            const char *n = tasks[pid].name ? tasks[pid].name : "a.out";
            uint32_t ni = 0;
            while (n[ni] && ni < EXEC_MODULE_NAME_MAX - 1) {
                mod.name[ni] = n[ni];
                ni++;
            }
            mod.name[ni] = 0;
        }

        exec_register_module((process_t *)0, &mod);
    }

    /* Allocate a FRESH kernel stack with guard page - we cannot reuse the
     * current one because the calling function (exec_loader_func) is still on it. */
    {
        uint32_t stack_pages = TASK_STACK_SIZE / 4096;
        uintptr_t stack_base = pmm_alloc_contiguous(stack_pages + 1);
        if (!stack_base) {
            klog(LOG_DEBUG, "sched", "Cannot allocate kernel stack");
            return -1;
        }
        vmm_install_guard_page(stack_base, "GUARD: kernel task stack overflow");
        new_kstack = (uint8_t *)(stack_base + 4096);
    }

    /* User stack: use the fixed user range address (same as task_create_user).
     * The user ELF range (0x800000-0x900000) is split into 4 KiB pages in the
     * per-process PML4 -- vmm_set_user_page only works on split PD entries.
     * kmalloc addresses are in the kernel heap (huge pages) where User bit
     * cannot be set.  The old kmalloc'd stack (if any) is freed. */
    if (tasks[pid].user_stack_base &&
        (uintptr_t)tasks[pid].user_stack_base < USER_ELF_BASE) {
        /* Old stack was kmalloc'd (heap address) -- free it */
        kfree(tasks[pid].user_stack_base);
    }
    tasks[pid].user_stack_base = (uint8_t *)(USER_ELF_END - USER_STACK_SIZE);

    /* Recreate per-process PML4: mark ELF pages + user stack as User.
     * vmm_create_user_pml4 splits PD[USER_PD_INDEX] into 4 KiB pages. */
    {
        uintptr_t user_cr3 = vmm_create_user_pml4();
        if (user_cr3) {
            uintptr_t addr;
            /* Destroy old PML4 if present */
            if (tasks[pid].cr3)
                vmm_destroy_user_pml4(tasks[pid].cr3);
            /* Mark ELF pages as User */
            for (addr = USER_ELF_BASE; addr < USER_ELF_END; addr += 4096)
                vmm_set_user_page(user_cr3, addr);
            tasks[pid].cr3 = user_cr3;
        } else {
            klog(LOG_WARN, "sched", "task_exec: PML4 creation failed for PID %u",
                 (uint64_t)pid);
        }
    }

    /* ---- Build Linux x86-64 initial user stack frame ----
     *
     * Layout (growing downward from user stack top):
     *   [rsp+0]   argc        (uint64_t)
     *   [rsp+8]   argv[0]     (pointer to program name string)
     *   [rsp+16]  NULL        (argv terminator)
     *   [rsp+24]  NULL        (envp terminator -- no env on stack)
     *   [rsp+32]  AT_ENTRY    auxv[0].type
     *   [rsp+40]  entry       auxv[0].value
     *   [rsp+48]  AT_PAGESZ   auxv[1].type
     *   [rsp+56]  4096        auxv[1].value
     *   [rsp+64]  AT_NULL     auxv[2].type (terminator)
     *   [rsp+72]  0           auxv[2].value
     *   [rsp+80+] "cmd.exe\0" (program name string data)
     *
     * RSP must be 16-byte aligned BEFORE _start is entered.
     * _start sees argc at [rsp]. Current crt0 ignores argc/argv
     * but this layout is ready for a future crt0 that parses them.
     */
    {
        uint64_t *ustk = (uint64_t *)((uint64_t)(
            tasks[pid].user_stack_base + USER_STACK_SIZE) & ~0xFULL);
        const char *name = tasks[pid].name ? tasks[pid].name : "a.out";
        uint32_t name_len = 0;
        uint64_t user_rsp;

        /* Count name length */
        { const char *p = name; while (*p++) name_len++; }

        /* String data at very top of stack */
        ustk -= 2;  /* room for string (up to 16 bytes aligned) */
        {
            char *str = (char *)ustk;
            uint32_t i;
            for (i = 0; i <= name_len && i < 15; i++)
                str[i] = name[i];
            str[i > 0 ? i : 0] = '\0';
        }
        uint64_t argv0_addr = (uint64_t)ustk;

        /* auxv (3 entries: AT_ENTRY, AT_PAGESZ, AT_NULL) */
        #define AT_NULL   0
        #define AT_ENTRY  9
        #define AT_PAGESZ 6
        ustk -= 6;  /* 3 pairs × 2 qwords */
        ustk[0] = AT_ENTRY;  ustk[1] = entry;
        ustk[2] = AT_PAGESZ; ustk[3] = 4096;
        ustk[4] = AT_NULL;   ustk[5] = 0;

        /* envp NULL terminator */
        ustk--;
        *ustk = 0;

        /* argv[1] = NULL (terminator) */
        ustk--;
        *ustk = 0;

        /* argv[0] = program name */
        ustk--;
        *ustk = argv0_addr;

        /* argc = 1 */
        ustk--;
        *ustk = 1;

        /* Ensure 16-byte alignment */
        user_rsp = (uint64_t)ustk & ~0xFULL;

        /* Build kernel interrupt frame */
        sp = (uint64_t *)(new_kstack + TASK_STACK_SIZE);
        sp = (uint64_t *)((uint64_t)sp & ~0xFULL);
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
        /* RCX = PEB address (Win64 first arg for ntdll/LdrpInitialize).
         * ELF crt0.asm ignores RCX, so this is safe for both formats.
         * When PE32+ loading lands (TODO-08), ntdll will find PEB in RCX
         * and parse ProcessParameters for argv -- no stack changes needed. */
        sp[12] = tasks[pid].peb
                     ? (uint64_t)(uintptr_t)tasks[pid].peb
                     : 0;                          /* rcx = PEB */
        sp[13] = 0;                                /* rbx */
        sp[14] = 0;                                /* rax */
        sp[15] = 0;                                /* int_no */
        sp[16] = 0;                                /* err_code */
        sp[17] = entry;                        /* rip = ELF entry */
        sp[18] = GDT_USER_CODE | 3;                /* cs = user code */
        sp[19] = 0x202;                            /* rflags: IF set */
        sp[20] = user_rsp;                         /* rsp = user stack */
        sp[21] = GDT_USER_DATA | 3;                /* ss = user data */
    }

    /* Switch to new kernel stack and mark exec pending */
    tasks[pid].rsp = (uint64_t)sp;
    tasks[pid].stack_base = new_kstack;
    tasks[pid].kernel_rsp = (uint64_t)(new_kstack + TASK_STACK_SIZE);
    tasks[pid].exec_pending = 1;  /* prevent scheduler from overwriting this frame */
    tasks[pid].exec_pending_tick = uptime();  /* for stuck detection */

    /* Allocate PEB in user address space */
    tasks[pid].peb = (void *)peb_alloc_for_task(
        pid, entry, tasks[pid].name);

    /* Allocate TEB for the initial thread (TID 0) */
    {
        uintptr_t ustack = (uintptr_t)tasks[pid].user_stack_base;
        TEB *teb = teb_alloc_for_task(pid, 0, ustack, USER_STACK_SIZE,
                                       tasks[pid].peb);
        tasks[pid].teb = (void *)teb;
        /* Wire GS: swapgs in ISR exchanges GS_BASE ↔ KERNEL_GS_BASE.
         * After swapgs on ring-3 return, user-mode GS points to TEB. */
        tasks[pid].kernel_gs_base = teb ? (uint64_t)(uintptr_t)teb : 0;
    }

    if (tasks[pid].peb) {
        PEB *p = (PEB *)tasks[pid].peb;
        klog(LOG_DEBUG, "sched",
             "PID %u: PEB=%p TEB=%p (Win %u.%u.%u, %u CPUs)",
             (uint64_t)pid,
             (uint64_t)(uintptr_t)tasks[pid].peb,
             (uint64_t)(uintptr_t)tasks[pid].teb,
             (uint64_t)p->OSMajorVersion,
             (uint64_t)p->OSMinorVersion,
             (uint64_t)p->OSBuildNumber,
             (uint64_t)p->NumberOfProcessors);
    }
    /* ---- §10: Register PEB/TEB in Ob namespace ----
     * Insert as named objects under \KernelObjects\Process<PID>\ so
     * user-mode tools can enumerate all processes via NtQueryDirectoryObject.
     * This is an Impossible OS exclusive -- neither Windows nor Linux
     * expose PEB/TEB as public named objects. */
    {
        void *ko_dir = (void *)0;
        ObLookupObjectByName("\\KernelObjects", (void *)0, 0, &ko_dir);
        if (ko_dir) {
            /* Build "Process<PID>" directory name */
            char pdir_name[16];
            {
                const char *pfx = "Process";
                uint32_t n = 0;
                while (*pfx) pdir_name[n++] = *pfx++;
                if (pid >= 10) pdir_name[n++] = '0' + (char)(pid / 10);
                pdir_name[n++] = '0' + (char)(pid % 10);
                pdir_name[n] = '\0';
            }

            void *proc_dir = ob_ns_create_directory(ko_dir);
            if (proc_dir) {
                ObInsertObject(proc_dir, pdir_name, ko_dir);

                /* Insert PEB and TEB as named objects.
                 * These are raw pointers -- the OB body IS the PEB/TEB. */
                if (tasks[pid].peb)
                    ObInsertObject(tasks[pid].peb, "Peb", proc_dir);
                if (tasks[pid].teb)
                    ObInsertObject(tasks[pid].teb, "Teb", proc_dir);
            }
            ObDereferenceObject(ko_dir);
        }
    }

    klog(LOG_DEBUG, "sched", "PID %u -> entry %p",
           (uint64_t)pid, entry);

    return 0;
}

void task_exit(int32_t status)
{
    uint32_t pid = current_task;
    uint32_t i;

    tasks[pid].state = TASK_DEAD;
    tasks[pid].exit_status = status;

    /* Mark process object as temporary so it can be freed */
    ob_process_mark_dead(pid);

    klog(LOG_DEBUG, "sched", "Task %u (\"%s\") exited with status %d",
           (uint64_t)pid,
           tasks[pid].name ? tasks[pid].name : "?",
           (uint64_t)(uint32_t)status);

    /* Send SIGCHLD to parent */
    if (pid != 0) {
        signal_send(tasks[pid].parent_pid, SIGCHLD);
    }

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
        klog(LOG_DEBUG, "sched", "Invalid child PID %u", (uint64_t)child_pid);
        return -1;
    }

    /* Verify this is actually our child */
    if (tasks[child_pid].parent_pid != current_task) {
        klog(LOG_DEBUG, "sched", "PID %u is not a child of PID %u",
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

    /* Yield away -- scheduler will skip us since we're TASK_WAITING */
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

    /* Close all handles and free handle table */
    ob_handle_table_destroy(&tasks[pid].handle_table);

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

/* ============================================================================
 * Thread functions -- per-task kernel threads
 *
 * Threads share the same PID and address space as the parent task.
 * Each thread has its own stack. The scheduler treats threads as
 * additional runnable entities within a task.
 *
 * thread_create() adds a new schedulable thread to the current task.
 * The scheduler gives time slices to ALL threads across ALL tasks.
 * ============================================================================ */

/* Thread entry wrapper -- sets up the thread function call and handles exit.
 * Thread entry pointer is in r12, argument pointer is in r13 (set by thread_create). */
static void thread_wrapper(void)
{
    thread_entry_t entry;
    void *arg;

    __asm__ volatile("mov %%r12, %0" : "=r"(entry));
    __asm__ volatile("mov %%r13, %0" : "=r"(arg));

    entry(arg);

    /* Thread returned -- exit cleanly */
    thread_exit(0);
}

/* ---- TLS slot allocation (64 static slots) ------------------------------ */

int tls_alloc(uint32_t pid)
{
    uint64_t bm;
    uint32_t i;

    if (pid >= num_tasks) return -1;
    bm = tasks[pid].tls_bitmap;

    /* Find first free bit (0 = free) */
    for (i = 0; i < 64; i++) {
        if (!(bm & (1ULL << i))) {
            tasks[pid].tls_bitmap |= (1ULL << i);
            return (int)i;
        }
    }
    return -1;  /* all 64 slots occupied */
}

int tls_free(uint32_t pid, uint32_t index)
{
    if (pid >= num_tasks || index >= 64) return -1;
    if (!(tasks[pid].tls_bitmap & (1ULL << index))) return -1; /* not allocated */

    tasks[pid].tls_bitmap &= ~(1ULL << index);

    /* Zero the slot in the main thread's TEB */
    if (tasks[pid].teb) {
        TEB *teb = (TEB *)tasks[pid].teb;
        teb->TlsSlots[index] = 0;
    }
    return 0;
}

uint64_t tls_get_value(uint32_t pid, uint32_t index)
{
    if (pid >= num_tasks || index >= 64 || !tasks[pid].teb)
        return 0;
    return ((TEB *)tasks[pid].teb)->TlsSlots[index];
}

void tls_set_value(uint32_t pid, uint32_t index, uint64_t value)
{
    if (pid >= num_tasks || index >= 64 || !tasks[pid].teb)
        return;
    ((TEB *)tasks[pid].teb)->TlsSlots[index] = value;
}

int thread_create(thread_entry_t entry, void *arg, uint32_t stack_size)
{
    uint32_t task_idx = current_task;
    struct task *t = &tasks[task_idx];
    uint32_t tid;
    uint8_t *stack;
    uint64_t *sp;

    if (t->num_threads >= THREAD_MAX) {
        klog(LOG_ERROR, "sched", "thread_create: max threads reached (PID %u)",
               (uint64_t)t->pid);
        return -1;
    }

    /* Use at least THREAD_STACK_SIZE */
    if (stack_size < THREAD_STACK_SIZE)
        stack_size = THREAD_STACK_SIZE;

    /* Allocate thread stack */
    stack = (uint8_t *)kmalloc(stack_size);
    if (!stack) {
        klog(LOG_ERROR, "sched", "thread_create: cannot allocate stack");
        return -1;
    }

    tid = t->num_threads;

    /* Build initial interrupt frame on thread stack.
     * Same layout as task_create: 22 qwords for ISR stub restore + iretq. */
    sp = (uint64_t *)(stack + stack_size);
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);

    {
        uint64_t new_rsp = (uint64_t)sp;

        sp -= 22;
        sp[0]  = 0;                           /* r15 */
        sp[1]  = 0;                           /* r14 */
        sp[2]  = (uint64_t)arg;               /* r13 = argument */
        sp[3]  = (uint64_t)entry;             /* r12 = entry function */
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
        sp[15] = 0;                           /* int_no */
        sp[16] = 0;                           /* err_code */
        sp[17] = (uint64_t)thread_wrapper;    /* rip */
        sp[18] = GDT_KERNEL_CODE;             /* cs = 0x08 */
        sp[19] = 0x202;                       /* rflags: IF set */
        sp[20] = new_rsp;                     /* rsp after iretq */
        sp[21] = GDT_KERNEL_DATA;             /* ss = 0x10 */
    }

    /* Initialize thread control block */
    t->threads[tid].id = tid;
    t->threads[tid].state = THREAD_READY;
    t->threads[tid].rsp = (uint64_t)sp;
    t->threads[tid].stack_base = stack;
    t->threads[tid].stack_size = stack_size;
    t->threads[tid].parent_task = task_idx;
    t->threads[tid].exit_status = 0;
    t->threads[tid].join_tid = -1;
    t->threads[tid].priority      = THREAD_PRIO_NORMAL;
    t->threads[tid].base_priority = THREAD_PRIO_NORMAL;
    t->num_threads++;

    /* Register thread with Object Manager */
    ob_thread_create(&t->threads[tid], t->pid);

    klog(LOG_DEBUG, "sched", "Thread %u created in task %u (\"%s\")",
           (uint64_t)tid, (uint64_t)t->pid,
           t->name ? t->name : "?");

    return (int)tid;
}

void thread_exit(int32_t status)
{
    struct task *t = &tasks[current_task];
    struct thread *thr = &t->threads[current_thread];
    uint32_t j;

    thr->state = THREAD_DEAD;
    thr->exit_status = status;

    /* Mark thread object as temporary so it can be freed */
    ob_thread_mark_dead(t->pid, thr->id);

    klog(LOG_DEBUG, "sched", "Thread %u (task %u \"%s\") exited with status %d",
           (uint64_t)thr->id, (uint64_t)t->pid,
           t->name ? t->name : "?",
           (uint64_t)(uint32_t)status);

    /* Wake any thread in the same task blocked on thread_join(our tid) */
    for (j = 0; j < t->num_threads; j++) {
        if (t->threads[j].state == THREAD_BLOCKED &&
            t->threads[j].join_tid == (int32_t)thr->id) {
            t->threads[j].state = THREAD_READY;
            t->threads[j].join_tid = -1;
        }
    }

    /* If this was the main thread (tid 0), the entire task dies */
    if (current_thread == 0) {
        task_exit(status);
        /* does not return */
    }

    /* Yield away forever */
    for (;;)
        yield();
}

int32_t thread_join(uint32_t thread_id)
{
    struct task *t = &tasks[current_task];

    /* Validate thread ID */
    if (thread_id >= t->num_threads || thread_id == current_thread) {
        klog(LOG_DEBUG, "sched", "Invalid thread ID %u for join",
               (uint64_t)thread_id);
        return -1;
    }

    /* If target thread is already dead, return immediately */
    if (t->threads[thread_id].state == THREAD_DEAD) {
        int32_t status = t->threads[thread_id].exit_status;

        /* Free the thread's stack */
        if (t->threads[thread_id].stack_base) {
            kfree(t->threads[thread_id].stack_base);
            t->threads[thread_id].stack_base = (uint8_t *)0;
        }

        return status;
    }

    /* Block current thread until target exits */
    t->threads[current_thread].state = THREAD_BLOCKED;
    t->threads[current_thread].join_tid = (int32_t)thread_id;

    /* Yield away -- scheduler will skip us since we're THREAD_BLOCKED */
    yield();

    /* When we wake up, target thread has exited */
    {
        int32_t status = t->threads[thread_id].exit_status;

        /* Free the thread's stack */
        if (t->threads[thread_id].stack_base) {
            kfree(t->threads[thread_id].stack_base);
            t->threads[thread_id].stack_base = (uint8_t *)0;
        }

        return status;
    }
}

void thread_yield(void)
{
    yield();  /* Same mechanism -- INT 0x81 */
}

struct thread *thread_current(void)
{
    if (current_task >= num_tasks)
        return (struct thread *)0;
    if (current_thread >= tasks[current_task].num_threads)
        return (struct thread *)0;
    return &tasks[current_task].threads[current_thread];
}

/* ============================================================================
 * Priority management helpers (used by mutex priority inheritance)
 * ============================================================================ */

/* Permanently change a thread's priority (both effective and base). */
void thread_set_priority(uint32_t task_pid, uint32_t thread_id, uint32_t prio)
{
    struct task *t;
    if (task_pid >= num_tasks) return;
    t = &tasks[task_pid];
    if (thread_id >= t->num_threads) return;
    t->threads[thread_id].priority      = prio;
    t->threads[thread_id].base_priority = prio;
}

/* Temporarily boost effective priority (does NOT change base_priority).
 * Returns 1 if boost was applied, 0 if already at >= new_prio. */
int thread_boost_priority(uint32_t task_pid, uint32_t thread_id, uint32_t new_prio)
{
    struct task *t;
    if (task_pid >= num_tasks) return 0;
    t = &tasks[task_pid];
    if (thread_id >= t->num_threads) return 0;
    if (new_prio > t->threads[thread_id].priority) {
        t->threads[thread_id].priority = new_prio;
        return 1;
    }
    return 0;
}

/* Restore effective priority to base_priority (undoes PI boost). */
void thread_restore_priority(uint32_t task_pid, uint32_t thread_id)
{
    struct task *t;
    if (task_pid >= num_tasks) return;
    t = &tasks[task_pid];
    if (thread_id >= t->num_threads) return;
    t->threads[thread_id].priority = t->threads[thread_id].base_priority;
}
