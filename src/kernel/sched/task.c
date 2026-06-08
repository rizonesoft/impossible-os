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
#include "kernel/cpu_regs.h"      /* CR0_TS */
#include "kernel/cpu_security.h"  /* cr0_write_safe (CR0 pin preservation) */
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
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"
#include "kernel/elf.h"
#include "kernel/random.h"
#include "kernel/boot_init.h"

/* Use VECTOR_YIELD from vectors.h (single source of truth) */
#define YIELD_INT_VECTOR VECTOR_YIELD

/* --- Task table --- */
static struct task tasks[TASK_MAX];
static uint32_t num_tasks = 0;
static uint32_t current_task = 0;
static uint32_t current_thread = 0;  /* thread index within current task */

/* TLS allocation spinlock (definition below in TLS section) */
static spinlock_t tls_lock;

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

    /* Same IRQL-cleanup as task_exit: see the long comment in task_exit
     * below. Forces PASSIVE so the next task scheduled on this CPU
     * starts at the right level even though we never return from this
     * forever-yield (so the IDT's irql_restore block is bypassed). */
    KeLowerIrql(PASSIVE_LEVEL);

    /* Yield forever -- yield() via INT 0x81 always works,
     * whether preemptive scheduler is enabled or not. */
    for (;;)
        yield();
}

/* --- Find the next runnable task+thread (priority-aware) ---
 *
 * Flat cyclic scan: visits every (task, thread) slot exactly once starting
 * at the slot immediately after (from_task, from_thread) in global order,
 * wrapping across task boundaries. This gives every runnable thread the
 * same round-robin chance regardless of which task it lives in, so a
 * thread late in one task's array is not starved by earlier same-priority
 * threads in other tasks.
 *
 * Returns the highest-priority runnable thread; within a tie, the first
 * one reached in the cyclic order (i.e. the one "closest after" the
 * current slot) wins -- that is the fairness rule.
 *
 * Sets *out_thread to the thread index. Returns the task index, or
 * (from_task, from_thread) if nothing else is runnable. */
static uint32_t find_next_task(uint32_t from_task, uint32_t from_thread,
                               uint32_t *out_thread)
{
    uint32_t best_task   = from_task;
    uint32_t best_thread = from_thread;
    uint32_t best_prio   = 0;
    int      found       = 0;
    uint32_t total_slots = 0;
    uint32_t t;

    /* Total slots bounds the cyclic scan. If zero, nothing to do. */
    for (t = 0; t < num_tasks; t++)
        total_slots += tasks[t].num_threads;
    if (total_slots == 0) {
        *out_thread = from_thread;
        return from_task;
    }

    uint32_t task_idx   = from_task;
    uint32_t thread_idx = from_thread;
    uint32_t steps;

    for (steps = 0; steps < total_slots; steps++) {
        /* Advance one slot in global order, hopping tasks on wrap.
         * Bounded by num_tasks to avoid looping on a run of empty tasks. */
        uint32_t hops = 0;
        thread_idx++;
        while (thread_idx >= tasks[task_idx].num_threads) {
            thread_idx = 0;
            task_idx = (task_idx + 1) % num_tasks;
            if (++hops > num_tasks)
                break;  /* every task empty -- total_slots guard catches this */
        }

        /* Skip whole task if not runnable. */
        if (tasks[task_idx].state != TASK_READY &&
            tasks[task_idx].state != TASK_RUNNING)
            continue;

        struct thread *thr = &tasks[task_idx].threads[thread_idx];
        if (thr->state != THREAD_READY && thr->state != THREAD_RUNNING)
            continue;

        /* Strict > keeps the first-reached thread at the current max
         * priority (round-robin tie-break). A strictly higher priority
         * seen later still overrides. */
        if (!found || thr->priority > best_prio) {
            best_task   = task_idx;
            best_thread = thread_idx;
            best_prio   = thr->priority;
            found = 1;
        }
    }

    if (!found) {
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
        /* First FPU use -- allocate and load clean FPU state.
         * Without the load, hardware registers still contain the
         * previous task's SIMD/FP data (cross-task data leakage). */
        task_alloc_xsave(t);
        t->fpu_used = 1;

        if (t->xsave_area) {
            extern struct cpu_features g_cpu;
            if (cpu_has(CPU_FEATURE_XSAVE)) {
                uint64_t xcr0 = g_cpu.xcr0_active;
                __asm__ volatile ("xrstor %0" : :
                    "m"(*(uint8_t *)t->xsave_area),
                    "a"((uint32_t)xcr0),
                    "d"((uint32_t)(xcr0 >> 32)) : "memory");
            } else {
                __asm__ volatile ("fxrstor %0" : :
                    "m"(*(uint8_t *)t->xsave_area) : "memory");
            }
        }
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

        /* Zero the buffer, then set architectural defaults */
        uint8_t *p = (uint8_t *)t->xsave_area;
        uint32_t i;
        for (i = 0; i < pages * 4096; i++)
            p[i] = 0;

        /* FCW at offset 0 in FXSAVE/XSAVE layout (Intel SDM Vol. 1
         * Table 10-2). Default value 0x037F = all x87 exceptions masked,
         * double precision, round-to-nearest (same as FINIT).
         * Without this, FXRSTOR loads FCW=0 which unmasks all x87
         * exceptions and the first imprecise FP op triggers #MF (vec 16).
         * WHPX masks this because XRSTOR uses init optimization; TCG
         * uses FXRSTOR which loads FCW directly from the buffer. */
        *(uint16_t *)(p + 0) = 0x037F;

        /* MXCSR at offset 24 in FXSAVE/XSAVE layout (Intel SDM Vol. 1
         * Table 10-2). Default value 0x1F80 = all SIMD exceptions masked.
         * Without this, XRSTOR loads MXCSR=0 which unmasks all exceptions
         * and the first SSE/AVX instruction triggers #XM (vector 19). */
        *(uint32_t *)(p + 24) = 0x1F80;

        /* Set XSTATE_BV header: bit 0 = x87 initial state present */
        if (size >= FXSAVE_SIZE + 64) {
            /* XSAVE header at offset 512, 64 bytes.
             * XSTATE_BV (offset 512, 8 bytes) = 0x01 (x87 state valid) */
            p[FXSAVE_SIZE] = 0x01;
        }

        /* PKRU initial value: 0x55555554 (keys 1-15 access-disabled,
         * key 0 full access). Only set if PKU is enabled and the XSAVE
         * area includes the PKRU component. The offset comes from CPUID
         * leaf 0x0D subleaf 9 (queried in cpuid_init).
         * Bounds check uses subtraction to avoid uint32 overflow. */
        {
            extern int pku_enabled;
            if (pku_enabled &&
                g_cpu.pkru_xsave_offset > 0 &&
                size >= 4 &&
                g_cpu.pkru_xsave_offset <= size - 4) {
                *(uint32_t *)(p + g_cpu.pkru_xsave_offset) = 0x55555554u;
                /* Mark PKRU as valid in XSTATE_BV (bit 9) */
                if (size >= FXSAVE_SIZE + 64) {
                    uint64_t *xstate_bv = (uint64_t *)(p + FXSAVE_SIZE);
                    *xstate_bv |= (1ULL << 9);
                }
            }
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
        tasks[i].tls_expansion_allocated = 0;
        tasks[i].tls_expansion_phys = 0;
        tasks[i].tls_expansion_virt = 0;
        {
            uint32_t w;
            for (w = 0; w < TLS_EXPANSION_BITMAP_WORDS; w++)
                tasks[i].tls_expansion_bitmap[w] = 0;
        }
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
            tasks[i].threads[j].kernel_rsp = 0;
            tasks[i].threads[j].kernel_stack_base = (uint8_t *)0;
            tasks[i].threads[j].kernel_stack_pages = 0;
            tasks[i].threads[j].user_stack_va = 0;
            tasks[i].threads[j].user_stack_pages = 0;
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
    tasks[0].threads[0].teb = (void *)0;
    tasks[0].threads[0].kernel_gs_base = 0;
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
    tasks[pid].threads[0].kernel_rsp = tasks[pid].kernel_rsp;
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
    tasks[pid].threads[0].teb = (void *)0;
    tasks[pid].threads[0].kernel_gs_base = 0;
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
    tasks[pid].threads[0].kernel_rsp = tasks[pid].kernel_rsp;
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
    tasks[pid].threads[0].teb = (void *)0;
    tasks[pid].threads[0].kernel_gs_base = 0;
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

    /* FPU/SIMD save for cooperative path (mirrors preemptive schedule).
     * CLTS before save: FXSAVE/XSAVE fault #NM when CR0.TS=1
     * (Intel SDM Vol. 3A Section 2.5: TS affects ALL FPU instructions). */
    {
        extern struct cpu_features g_cpu;
        int have_xsave = cpu_has(CPU_FEATURE_XSAVE);
        if (tasks[prev_task].fpu_used && tasks[prev_task].xsave_area) {
            __asm__ volatile ("clts");
            if (have_xsave) {
                uint64_t xcr0 = g_cpu.xcr0_active;
                uint32_t lo = (uint32_t)xcr0, hi = (uint32_t)(xcr0 >> 32);
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
    }

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
    /* Release-store so a future cross-CPU acquirer in the save-gate
     * above sees the zero together with every write that preceded
     * this point (rsp, state, kernel_gs_base). */
    __atomic_store_n(&tasks[next_task].exec_pending, 0u, __ATOMIC_RELEASE);
    tasks[next_task].exec_pending_tick = 0;
    current_task = next_task;
    current_thread = next_thread;
    sched_ticks = 0;

    /* Update TSS rsp0 + per-CPU syscall_rsp0 for the incoming thread's
     * kernel stack.  Per-thread, not per-task, so each thread in a
     * multi-threaded process gets its own ring-0 entry stack. */
    {
        uint64_t next_krsp = tasks[next_task].threads[next_thread].kernel_rsp;
        if (next_krsp) {
            tss_set_kernel_stack(next_krsp);
            smp_this_cpu()->syscall_rsp0 = next_krsp;
        }
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

    /* KERNEL_GS_BASE switch: save prev thread's TEB, load next thread's TEB.
     * swapgs in the ISR stub handles ring transition; this handles
     * switching between threads with different TEBs (same or different task).
     *
     * Two TODO-04 -17 hardening pieces are baked in here:
     *
     * (1) Exec-pending save-gate: when prev_task is still exec_pending
     *     (task_exec set kernel_gs_base = TEB but the first switch-in
     *     has not yet programmed the MSR), skip the save-before-write.
     *     The MSR holds the stale kernel value from before task_exec,
     *     and saving it here would overwrite the TEB pointer. On the
     *     FIRST ring-3 entry swapgs would then put 0 into user GS_BASE
     *     and every `gs:<off>` read page-faults at CR2=<off>. Root cause
     *     of the original "silent gs:0x40 hang on WHPX" -- the probe
     *     binary surfaced it as CR2=0x30 at user RIP, the TEB address
     *     was known good on the task but the thread slot got zeroed.
     *
     * (2) MSR readback invariant: after msr_write, re-read and fatal-log
     *     on mismatch. Converts a hypervisor that silently swallows the
     *     MSR write into a visible named crash at the write site --
     *     better than the silent TEB corruption that previously had no
     *     diagnostic surface. Cost: one RDMSR per context switch (~30
     *     cycles). Always-on, no debug-build gate. */
    if (prev_task != next_task || prev_thread != next_thread) {
        uint64_t new_gs = tasks[next_task].threads[next_thread].kernel_gs_base;
        /* Atomic-load exec_pending: Impossible OS's scheduler today is
         * single-CPU (global `current_task` cursor, no per-CPU run
         * queues), so a cross-CPU race is not reachable at this point.
         * The __atomic_load here is belt-and-suspenders for the future
         * SMP scheduler redesign -- when per-CPU run queues land, the
         * exec_pending flag handoff between CPU-A (switching away from
         * prev) and CPU-B (switching in to prev, clearing the flag)
         * needs acquire-semantics visibility so the save-gate cannot
         * clobber the TEB-primed thread slot. */
        if (!__atomic_load_n(&tasks[prev_task].exec_pending,
                             __ATOMIC_ACQUIRE)) {
            tasks[prev_task].threads[prev_thread].kernel_gs_base =
                msr_read(MSR_IA32_KERNEL_GS_BASE);
        }
        /* NULL guard: kernel threads have kernel_gs_base == 0; writing 0
         * would clobber the MSR for no benefit (no swapgs on ring-0 return). */
        if (new_gs) {
            msr_write(MSR_IA32_KERNEL_GS_BASE, new_gs);
            uint64_t rb = msr_read(MSR_IA32_KERNEL_GS_BASE);
            if (rb != new_gs)
                klog(LOG_FATAL, "sched",
                     "MSR_KERNEL_GS_BASE corrupted on preemptive switch: "
                     "wrote=0x%X read=0x%X (task=%u thread=%u)",
                     new_gs, rb,
                     (uint64_t)next_task, (uint64_t)next_thread);
        } else if (tasks[next_task].teb ||
                   tasks[next_task].threads[next_thread].teb) {
            /* TEB exists on the task OR on the specific thread but its
             * kernel_gs_base slot is 0. This is FAIL-CLOSED: returning
             * to ring 3 with GS_BASE=0 guarantees a user-mode page
             * fault on the first `gs:<off>` read with no useful
             * diagnostic. Panic now so the corruption is named at the
             * scheduler, not later in user code. Two checks because
             * tasks[].teb is the main-thread TEB and threads[tid].teb
             * covers secondary threads; either one non-NULL is enough
             * to prove ring-3 intent. */
            klog(LOG_FATAL, "sched",
                 "ring-3 task %u thread %u has TEB but kernel_gs_base=0 "
                 "(task.teb=%p thread.teb=%p)",
                 (uint64_t)next_task, (uint64_t)next_thread,
                 (uint64_t)(uintptr_t)tasks[next_task].teb,
                 (uint64_t)(uintptr_t)tasks[next_task].threads[next_thread].teb);
        }
    }

    /* FPU/SIMD restore for cooperative path (mirrors preemptive schedule) */
    {
        extern struct cpu_features g_cpu;
        int have_xsave = cpu_has(CPU_FEATURE_XSAVE);
        if (tasks[next_task].fpu_used && tasks[next_task].xsave_area) {
            __asm__ volatile ("clts");
            if (have_xsave) {
                uint64_t xcr0 = g_cpu.xcr0_active;
                uint32_t lo = (uint32_t)xcr0, hi = (uint32_t)(xcr0 >> 32);
                __asm__ volatile ("xrstor %0" : : "m"(*(uint8_t *)tasks[next_task].xsave_area),
                                  "a"(lo), "d"(hi) : "memory");
            } else {
                __asm__ volatile ("fxrstor %0" : : "m"(*(uint8_t *)tasks[next_task].xsave_area) : "memory");
            }
        } else {
            uint64_t cr0;
            __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
            cr0 |= CR0_TS;          /* defer FPU: next FPU use faults #NM */
            cr0_write_safe(cr0);    /* preserve pinned CR0.WP (TODO-09-boot S7) */
        }
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

    /* Match schedule_now: a single-task kernel with multiple runnable
     * kernel threads must still preempt, otherwise any CPU-bound thread
     * in the sole task can monopolize the CPU even when kthread_create
     * added READY siblings. */
    if (!sched_enabled ||
        (num_tasks <= 1 && tasks[0].num_threads <= 1))
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
     * to FXSAVE/FXRSTOR on CPUs without XSAVE (e.g., QEMU TCG).
     * CLTS before save: FXSAVE/XSAVE fault #NM when CR0.TS=1
     * (Intel SDM Vol. 3A Section 2.5: TS affects ALL FPU instructions). */
    {
    extern struct cpu_features g_cpu;
    int have_xsave = cpu_has(CPU_FEATURE_XSAVE);

    if (tasks[prev_task].fpu_used && tasks[prev_task].xsave_area) {
        __asm__ volatile ("clts");
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
        /* CLTS before restore -- XRSTOR/FXRSTOR fault with #NM when CR0.TS=1 */
        __asm__ volatile ("clts");
        if (have_xsave) {
            uint64_t xcr0 = g_cpu.xcr0_active;
            uint32_t lo = (uint32_t)xcr0;
            uint32_t hi = (uint32_t)(xcr0 >> 32);
            __asm__ volatile ("xrstor %0" : : "m"(*(uint8_t *)tasks[next_task].xsave_area),
                              "a"(lo), "d"(hi) : "memory");
        } else {
            __asm__ volatile ("fxrstor %0" : : "m"(*(uint8_t *)tasks[next_task].xsave_area) : "memory");
        }
    } else {
        uint64_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= CR0_TS;          /* defer FPU: next FPU use faults #NM */
        cr0_write_safe(cr0);    /* preserve pinned CR0.WP (TODO-09-boot S7) */
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
    __atomic_store_n(&tasks[next_task].exec_pending, 0u, __ATOMIC_RELEASE);
    tasks[next_task].exec_pending_tick = 0;
    current_task = next_task;
    current_thread = next_thread;

    /* Update TSS rsp0 + per-CPU syscall_rsp0 for the incoming thread's
     * kernel stack.  Per-thread, not per-task, so each thread in a
     * multi-threaded process gets its own ring-0 entry stack. */
    {
        uint64_t next_krsp = tasks[next_task].threads[next_thread].kernel_rsp;
        if (next_krsp) {
            tss_set_kernel_stack(next_krsp);
            smp_this_cpu()->syscall_rsp0 = next_krsp;
        }
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

    /* KERNEL_GS_BASE switch -- same two -17 hardening pieces as
     * schedule_now(): exec_pending save-gate + MSR readback invariant. */
    if (prev_task != next_task || prev_thread != next_thread) {
        uint64_t new_gs = tasks[next_task].threads[next_thread].kernel_gs_base;
        /* Atomic-load exec_pending: Impossible OS's scheduler today is
         * single-CPU (global `current_task` cursor, no per-CPU run
         * queues), so a cross-CPU race is not reachable at this point.
         * The __atomic_load here is belt-and-suspenders for the future
         * SMP scheduler redesign -- when per-CPU run queues land, the
         * exec_pending flag handoff between CPU-A (switching away from
         * prev) and CPU-B (switching in to prev, clearing the flag)
         * needs acquire-semantics visibility so the save-gate cannot
         * clobber the TEB-primed thread slot. */
        if (!__atomic_load_n(&tasks[prev_task].exec_pending,
                             __ATOMIC_ACQUIRE)) {
            tasks[prev_task].threads[prev_thread].kernel_gs_base =
                msr_read(MSR_IA32_KERNEL_GS_BASE);
        }
        if (new_gs) {
            msr_write(MSR_IA32_KERNEL_GS_BASE, new_gs);
            uint64_t rb = msr_read(MSR_IA32_KERNEL_GS_BASE);
            if (rb != new_gs)
                klog(LOG_FATAL, "sched",
                     "MSR_KERNEL_GS_BASE corrupted on cooperative switch: "
                     "wrote=0x%X read=0x%X (task=%u thread=%u)",
                     new_gs, rb,
                     (uint64_t)next_task, (uint64_t)next_thread);
        } else if (tasks[next_task].teb ||
                   tasks[next_task].threads[next_thread].teb) {
            /* Fail-closed matching the preemptive path above.
             * Codex HF 2026-04-22: this site was previously LOG_ERROR +
             * continue, which meant a `GetCurrentProcessId` caller
             * that had migrated to the gs:0x40 native fast path would
             * #PF in user mode at CR2=0x40. Promote to LOG_FATAL so
             * both scheduler paths enforce the same invariant. */
            klog(LOG_FATAL, "sched",
                 "ring-3 task %u thread %u has TEB but kernel_gs_base=0 "
                 "(task.teb=%p thread.teb=%p)",
                 (uint64_t)next_task, (uint64_t)next_thread,
                 (uint64_t)(uintptr_t)tasks[next_task].teb,
                 (uint64_t)(uintptr_t)tasks[next_task].threads[next_thread].teb);
        }
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
    /* Initialize thread 0 (main). Mirrors task_create_user -- without
     * these the child is invisible to find_next_task because
     * num_threads stays at 0 from the zero-init slot and the
     * scheduler's `thread_idx >= num_threads` guard skips the task
     * entirely. Root cause of the 2026-04-21 test_process fork-hang:
     * forked children were created but never dispatched. */
    tasks[child_pid].num_threads = 1;
    tasks[child_pid].threads[0].id = 0;
    tasks[child_pid].threads[0].state = THREAD_READY;
    tasks[child_pid].threads[0].stack_base = (uint8_t *)0;
    tasks[child_pid].threads[0].stack_size = 0;
    tasks[child_pid].threads[0].parent_task = child_pid;
    tasks[child_pid].threads[0].join_tid = -1;
    tasks[child_pid].threads[0].priority = THREAD_PRIO_NORMAL;
    tasks[child_pid].threads[0].kernel_rsp = tasks[child_pid].kernel_rsp;
    tasks[child_pid].threads[0].rsp = (uint64_t)sp;
    tasks[child_pid].user_stack_base = ustack;
    tasks[child_pid].name = tasks[parent_pid_val].name;
    tasks[child_pid].parent_pid = parent_pid_val;
    tasks[child_pid].exit_status = 0;
    tasks[child_pid].wait_pid = -1;
    tasks[child_pid].exec_pending = 0;
    ob_handle_table_init(&tasks[child_pid].handle_table);
    /* Copy parent's KERNEL_GS_BASE (TEB address) -- will allocate
     * a new TEB for the child and update this field. */
    tasks[child_pid].kernel_gs_base = tasks[parent_pid_val].kernel_gs_base;
    /* Mirror into threads[0] for per-thread GS swap */
    tasks[child_pid].threads[0].teb = tasks[parent_pid_val].threads[0].teb;
    tasks[child_pid].threads[0].kernel_gs_base =
        tasks[parent_pid_val].threads[0].kernel_gs_base;
    num_tasks++;

    /* Per-process page table: clone kernel PML4 + mark image + new
     * user stack as User. Without this the child inherits cr3=0 from
     * struct-zero-init, the scheduler falls back to kernel CR3 (which
     * has no User bit on image pages under the per-process PT regime
     * added by KPTI prep), and the child's first ring-3 instruction
     * faults silently before touching any user code. Mirror of the
     * task_exec() PML4 bring-up, minus the destroy-old step (child
     * has no prior cr3). Bug surfaced by the user-mode process-
     * lifecycle test on QEMU WHPX 2026-04-21 (forks succeeded but
     * children never ran). */
    {
        uintptr_t user_cr3 = vmm_create_user_pml4();
        if (user_cr3) {
            uintptr_t addr;
            /* Resolve the parent's current image via the interrupt
             * frame's saved RIP -- parent was executing user code
             * when it INT 0x80'd into fork, so rip is inside the
             * parent's loaded ELF. Mark every image page User in
             * the child's cr3. */
            loaded_module_t img_mod;
            if (exec_find_module_by_pc(frame->rip, &img_mod) == 0) {
                uintptr_t img_base = (uintptr_t)img_mod.base_address;
                uintptr_t img_end  = img_base +
                                     (uintptr_t)img_mod.size_of_image;
                for (addr = img_base; addr < img_end; addr += 4096)
                    vmm_set_user_page(user_cr3, addr);
            } else {
                /* Fallback: mark the whole ELF range. Matches the
                 * task_exec() fallback shape. */
                klog(LOG_WARN, "sched",
                     "task_fork: no module for rip=0x%x, using ELF range",
                     frame->rip);
                for (addr = USER_ELF_BASE; addr < USER_ELF_END; addr += 4096)
                    vmm_set_user_page(user_cr3, addr);
            }
            /* Mark child's user stack as User. The stack is kmalloc'd
             * so it is 16-byte aligned but NOT page-aligned -- the
             * byte `ustack + USER_STACK_SIZE` (the stack TOP, where
             * RSP starts) can easily land in the next 4 KiB page not
             * covered by the literal range. Round DOWN the start and
             * UP the end so every page the stack touches gets its
             * User bit set. Without this, the first ring-3 push lands
             * on a kernel-only page and #PFs silently before any user
             * code runs. Root cause of the 2026-04-21 test_process
             * fork-hang on WHPX + KVM. */
            {
                uintptr_t ustack_lo = (uintptr_t)ustack
                                      & ~(uintptr_t)0xFFFu;
                uintptr_t ustack_hi = ((uintptr_t)ustack + USER_STACK_SIZE
                                       + 0xFFFu) & ~(uintptr_t)0xFFFu;
                for (addr = ustack_lo; addr < ustack_hi; addr += 4096)
                    vmm_set_user_page(user_cr3, addr);
            }
            tasks[child_pid].cr3 = user_cr3;
        } else {
            tasks[child_pid].cr3 = 0;
            klog(LOG_WARN, "sched",
                 "task_fork: per-process PML4 failed for child PID %u",
                 (uint64_t)child_pid);
        }
    }

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

    /* Standard handles: INVALID for now (wires real console handles) */
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

    /* Exec must be called from the main thread (tid 0).  A secondary user
     * thread calling exec would leave stale threads[N].kernel_rsp in the
     * scheduler, causing TSS.rsp0 corruption on switch-in. */
    if (current_thread != 0) {
        klog(LOG_ERROR, "sched",
             "task_exec: rejected from thread %u (must be thread 0)",
             (uint64_t)current_thread);
        return -1;
    }

    /* Fork+exec isolation (2026-04-21): a forked child inherits its
     * parent's per-process cr3, which identity-maps the USER_ELF range
     * to the same physical frames the parent is actively running from.
     * exec_load writes the new binary to those VAs -- corrupting the
     * parent's code in-place. Before loading, replace the child's
     * image-range PTEs with PRIVATE physical frames so the new binary
     * lands in the child's own memory. The replacement is tagged
     * PAGE_OWNED so vmm_destroy_user_pml4 frees the private frames on
     * task exit. Launcher-spawned tasks (cr3=0 at entry) skip this
     * step -- there's no parent sharing physical VAs with them, and
     * the classic identity-mapped load is the right shape. */
    if (tasks[pid].cr3) {
        uintptr_t va;
        for (va = USER_ELF_BASE; va < USER_ELF_END; va += 4096) {
            uintptr_t new_phys = pmm_alloc_frame();
            if (!new_phys) {
                klog(LOG_ERROR, "sched",
                     "task_exec: OOM allocating private frame for "
                     "VA 0x%x -- forked-exec isolation degraded",
                     (uint64_t)va);
                /* Partial isolation still protects the pages we did
                 * remap. Continue without aborting -- exec_load will
                 * fall back to identity writes for unremapped pages
                 * and may corrupt parent, but refusing exec is worse. */
                break;
            }
            /* Zero the frame so the new binary's uninitialized BSS
             * doesn't inherit whatever was in the frame before.
             * pmm_alloc_frame returns an identity-mapped phys, so
             * writing via (uintptr_t)new_phys is safe in kernel mode. */
            {
                uint64_t *p = (uint64_t *)new_phys;
                uint32_t i;
                for (i = 0; i < 512; i++) p[i] = 0;
            }
            vmm_remap_user_page(tasks[pid].cr3, va, new_phys);
            vmm_flush_tlb(va);
        }
    }

    /* Load the binary via multi-format dispatcher (ELF, PE32+, EIF).
     * exec_load_fmt also publishes the matched format name (pointer
     * into the static format-registry table) which we stash on the
     * task so the launcher can log which loader picked this binary.
     * Cleared FIRST so a failing re-exec on an already-exec'd task
     * does not leave the previous image's format name behind --
     * readers of loaded_format must see NULL on failure, not stale
     * state from the last successful exec. */
    tasks[pid].loaded_format = (const char *)0;
    const char *fmt_name = (const char *)0;
    entry = exec_load_fmt(data, size, &exec_err, &fmt_name);
    if (entry == 0) {
        klog(LOG_DEBUG, "sched", "exec_load failed (err=%u)", (uint64_t)exec_err);
        return -1;
    }
    tasks[pid].loaded_format = fmt_name;

    /* Register module if the loader didn't already (PE registers in pe_load).
     * For ELF/EIF, register with the identity-mapped user ELF range. */
    {
        loaded_module_t probe;
        int already_registered = (exec_find_module_by_pc(entry, &probe) == 0);

        if (!already_registered) {
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

    /* Per-process PML4 handling (2026-04-21, revised):
     *   - Forked-exec: the child already has a per-process cr3 from
     *     task_fork. The pre-exec remap loop above replaced its
     *     image-range PTEs with private PAGE_OWNED frames, and
     *     exec_load wrote the new binary into them. REUSE that cr3 --
     *     creating a fresh one would orphan the private frames (new
     *     cr3's PT starts identity-mapped, not pointing at the frames
     *     we just populated), and destroying the old cr3 would free
     *     the private frames the new binary depends on.
     *   - Launcher-exec: the task enters with cr3=0 (kernel_pml4
     *     active). No prior per-process cr3 exists; build a fresh
     *     one and install it. No destroy needed (nothing to free).
     * This split replaced the single "always create + destroy" path
     * that caused both the 2026-04-21 WHPX crash (destroy freed
     * shared kernel PTs; see mm/vmm.c) and the fork+exec parent-
     * image-corruption bug the remap loop above addresses. */
    if (tasks[pid].cr3) {
        /* Forked-exec path: the pre-exec remap loop above already
         * replaced the image-range PTEs with private PAGE_OWNED frames
         * in tasks[pid].cr3, and exec_load wrote the new binary into
         * them. Creating a fresh cr3 here would orphan those frames
         * (the new cr3 starts with identity mappings) -- the binary's
         * code would disappear at the next CR3 switch. Reuse the
         * existing cr3 and just ensure User bits cover the new image's
         * full extent (hello.exe may be larger than test_process.exe). */
        uintptr_t addr;
        loaded_module_t img_mod;
        if (exec_find_module_by_pc(entry, &img_mod) == 0) {
            uintptr_t img_base = (uintptr_t)img_mod.base_address;
            uintptr_t img_end = img_base + (uintptr_t)img_mod.size_of_image;
            for (addr = img_base; addr < img_end; addr += 4096)
                vmm_set_user_page(tasks[pid].cr3, addr);
        }
        /* Same stack-User-bit guarantee as the launcher branch below:
         * fork+exec of a non-ELF image (PE32+/EIF, module size smaller
         * than full USER_ELF range) would otherwise leave the stack
         * kernel-only and fault at the _start prologue's first push. */
        for (addr = USER_ELF_END - USER_STACK_SIZE;
             addr < USER_ELF_END;
             addr += 4096) {
            vmm_set_user_page(tasks[pid].cr3, addr);
        }
    } else {
        /* Launcher-spawned task (cr3=0 at entry): build a fresh
         * per-process cr3 from scratch. No prior per-process cr3 to
         * destroy, no parent sharing identity-mapped VAs -- the new
         * binary's writes at USER_ELF_BASE+ land in kernel-identity
         * frames (shared with any future exec's pre-write state,
         * fine since no other task references them). */
        uintptr_t user_cr3 = vmm_create_user_pml4();
        if (user_cr3) {
            uintptr_t addr;
            loaded_module_t img_mod;
            if (exec_find_module_by_pc(entry, &img_mod) == 0) {
                uintptr_t img_base = (uintptr_t)img_mod.base_address;
                uintptr_t img_end = img_base + (uintptr_t)img_mod.size_of_image;
                for (addr = img_base; addr < img_end; addr += 4096)
                    vmm_set_user_page(user_cr3, addr);
            } else {
                klog(LOG_WARN, "sched",
                     "task_exec: no module found for entry 0x%x, using ELF range",
                     entry);
                for (addr = USER_ELF_BASE; addr < USER_ELF_END; addr += 4096)
                    vmm_set_user_page(user_cr3, addr);
            }

            /* Mark the user stack pages User regardless of image
             * range. task_exec builds the ring-3 iretq frame with
             * RSP at the top of this region (USER_ELF_END -
             * USER_STACK_SIZE..USER_ELF_END) and the task's FIRST
             * user-mode instruction typically pushes a callee-save
             * register there (the Win64 CRT-less _start prologue
             * does exactly this); without the User bit that push
             * faults with ERR=0x7 (present + write + user) at the
             * entry+1 instruction. ELF binaries happened to work
             * because their registered module size covers the
             * whole 1 MiB USER_ELF range including the stack; PE
             * (ImageBase != USER_ELF_BASE, SizeOfImage = 8 KiB)
             * and EIF (sub-KiB code segments) do not. Marking the
             * stack explicitly makes the user-stack mapping
             * independent of each format's image-size convention.
             * Bug surfaced by user-mode binary format loader probe (PE).
             * Stack is always at the top of USER_ELF range per
             * task.h (USER_ELF_END - USER_STACK_SIZE .. USER_ELF_END). */
            for (addr = USER_ELF_END - USER_STACK_SIZE;
                 addr < USER_ELF_END;
                 addr += 4096) {
                vmm_set_user_page(user_cr3, addr);
            }

            tasks[pid].cr3 = user_cr3;
            __asm__ volatile("mov %0, %%cr3" : : "r"(user_cr3) : "memory");
        } else {
            klog(LOG_WARN, "sched", "task_exec: PML4 creation failed for PID %u",
                 (uint64_t)pid);
        }
    }

    /* ---- Build Linux x86-64 initial user stack frame ----
     *
     * Layout (growing downward from user stack top):
     *   [top]      "cmd.exe\0"     program name string data
     *   [top-16]   16 random bytes for AT_RANDOM (stack canary seed)
     *   [...]      auxv pairs (AT_PHDR/PHENT/PHNUM/BASE/FLAGS/UID/EUID/GID/EGID/
     *              SECURE/RANDOM/HWCAP/HWCAP2/PAGESZ/ENTRY/NULL)
     *   [rsp+24]   NULL (envp terminator -- no env on stack)
     *   [rsp+16]   NULL (argv[1] terminator)
     *   [rsp+8]    argv[0] (pointer to program name string)
     *   [rsp+0]    argc (= 1)
     *
     * RSP must be 16-byte aligned BEFORE _start is entered.
     * _start sees argc at [rsp]. Current crt0 ignores argc/argv but this
     * layout is ready for a future crt0 (or glibc/musl) that parses them.
     *
     *: extended auxv carries AT_RANDOM (16-byte stack canary seed),
     * AT_PHDR/PHENT/PHNUM (program headers for dynamic linker),
     * AT_BASE (=0, no interpreter), AT_UID/EUID/GID/EGID (=0), AT_SECURE
     * (=0), AT_HWCAP (raw CPUID 1 EDX), AT_HWCAP2 (=0).
     */
    {
        uint64_t *ustk = (uint64_t *)((uint64_t)(
            tasks[pid].user_stack_base + USER_STACK_SIZE) & ~0xFULL);
        const char *name = tasks[pid].name ? tasks[pid].name : "a.out";
        uint32_t name_len = 0;
        uint64_t user_rsp;
        uint64_t at_random_addr = 0;
        uint64_t phdr_vaddr = 0;
        uint16_t phnum = 0;
        uint16_t phent = 0;
        uint32_t hwcap = 0;
        int is_elf = 0;

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

        /*: push 16 random bytes for AT_RANDOM. glibc/musl read exactly
         * 16 bytes from the address pushed in AT_RANDOM as the seed for
         * __stack_chk_guard. Without this, dynamically linked binaries
         * compiled with -fstack-protector use a zero or constant canary,
         * defeating stack overflow protection. */
        ustk -= 2;  /* 2 qwords = 16 bytes */
        {
            uint8_t *rand_buf = (uint8_t *)ustk;
            if (!rdrand_bytes(rand_buf, 16)) {
                /* Fallback: TSC-mixed bytes. NOT cryptographically strong.
                 *
                 * Why this is acceptable for now:
                 *   1. Impossible OS user binaries are compiled with
                 *      -fno-stack-protector (see Makefile USER_CFLAGS), so
                 *      glibc's __stack_chk_guard is NOT consumed by any
                 *      current user binary. AT_RANDOM is informational only
                 *      until libc with stack canaries lands.
                 *   2. Hardware (RDRAND-capable) takes the fast path above.
                 *      The fallback only fires on TCG and very old VMs.
                 *   3. The LOG_WARN below makes degraded entropy observable.
                 *
                 * Once user binaries link against a libc compiled with
                 * -fstack-protector (planned with the dynamic loader work),
                 * this fallback MUST be replaced with a proper kernel
                 * entropy source. The follow-up item is tracked on the
                 * userland stack-protector and kernel-CSPRNG roadmaps. */
                uint32_t lo1, hi1, lo2, hi2;
                __asm__ volatile ("rdtsc" : "=a"(lo1), "=d"(hi1));
                /* Tiny delay to decorrelate the second sample */
                __asm__ volatile ("pause; pause; pause; pause" ::: "memory");
                __asm__ volatile ("rdtsc" : "=a"(lo2), "=d"(hi2));
                uint64_t mix = ((uint64_t)hi1 << 32 | lo1)
                             ^ (((uint64_t)hi2 << 32 | lo2) * 0x9E3779B97F4A7C15ULL)
                             ^ ((uint64_t)pid * 0xBF58476D1CE4E5B9ULL);
                uint32_t i;
                for (i = 0; i < 8; i++) rand_buf[i] = (uint8_t)(mix >> (i * 8));
                mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
                for (i = 0; i < 8; i++) rand_buf[8 + i] = (uint8_t)(mix >> (i * 8));
                klog(LOG_ERROR, "sched",
                     "task_exec: RDRAND unavailable, using TSC fallback for "
                     "AT_RANDOM (DEGRADED ENTROPY -- not exploitable today "
                     "because user binaries are -fno-stack-protector; "
                     "TODO-20 S5 will replace this with a kernel CSPRNG)");
            }
        }
        at_random_addr = (uint64_t)ustk;

        /*: detect ELF and extract program-header metadata for the auxv.
         * Non-ELF formats (PE/EIF) leave phdr_vaddr/phnum/phent at 0 -- the
         * AT_PHDR/PHENT/PHNUM entries are still emitted but with zero values
         * (the loader ignores zero AT_PHDR per Linux ABI). */
        if (size >= 4 && data[0] == 0x7F && data[1] == 'E' &&
            data[2] == 'L' && data[3] == 'F') {
            is_elf = 1;
            (void)elf_extract_phdr_info(data, size,
                                         &phdr_vaddr, &phnum, &phent);
        }

        /*: build AT_HWCAP from raw CPUID leaf 1 EDX. This matches what
         * Linux x86_64 does -- it passes EDX through directly without
         * inventing bit positions. AT_HWCAP2 is set to 0 because the leaf 7
         * mapping has more divergence risk and glibc/musl on x86_64 read
         * CPUID directly for the SIMD bits used by IFUNC dispatch. */
        {
            uint32_t eax, ebx, ecx, edx;
            cpuid_raw(1, 0, &eax, &ebx, &ecx, &edx);
            hwcap = edx;
        }

        /* Emit auxv pairs into a local array, then bulk-copy onto the
         * stack. Pair count is computed at emission time -- no hard-coded
         * stack subtraction count to drift out of sync. */
        uint64_t auxv[64];  /* 32 pairs max; we use 16 */
        uint32_t naux = 0;
        #define AUXV_EMIT(t, v) do { \
                auxv[naux*2]   = (uint64_t)(t); \
                auxv[naux*2+1] = (uint64_t)(v); \
                naux++; \
            } while (0)

        AUXV_EMIT(AT_PHDR,   phdr_vaddr);
        AUXV_EMIT(AT_PHENT,  phent);
        AUXV_EMIT(AT_PHNUM,  phnum);
        AUXV_EMIT(AT_PAGESZ, 4096);
        AUXV_EMIT(AT_BASE,   0);          /* no dynamic linker (static ELF) */
        AUXV_EMIT(AT_FLAGS,  0);
        AUXV_EMIT(AT_ENTRY,  entry);
        AUXV_EMIT(AT_UID,    0);          /* root until user model lands */
        AUXV_EMIT(AT_EUID,   0);
        AUXV_EMIT(AT_GID,    0);
        AUXV_EMIT(AT_EGID,   0);
        AUXV_EMIT(AT_SECURE, 0);          /* no setuid */
        AUXV_EMIT(AT_RANDOM, at_random_addr);
        AUXV_EMIT(AT_HWCAP,  hwcap);
        AUXV_EMIT(AT_HWCAP2, 0);
        AUXV_EMIT(AT_NULL,   0);          /* terminator (must be last) */

        #undef AUXV_EMIT

        /* Bulk-copy auxv block onto stack. Each pair = 2 qwords. */
        ustk -= naux * 2;
        {
            uint32_t i;
            for (i = 0; i < naux * 2; i++)
                ustk[i] = auxv[i];
        }

        /* Stash auxv pointer in task struct for unit tests to walk */
        tasks[pid].user_auxv = (void *)ustk;
        tasks[pid].user_auxv_pairs = naux;

        /* Reset the section-view bump allocator so the new image's
         * first MapViewOfSection / sys_shmem_map starts at
         * SECTION_VIEW_BASE regardless of any mappings the prior
         * image installed. The prior image's section-view PTEs are
         * in the old cr3, which task_exec replaces further down,
         * so they're reclaimed with it. */
        tasks[pid].next_section_view_va = 0;

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

        klog(LOG_INFO, "sched",
             "ELF auxv: AT_RANDOM=0x%x AT_PHDR=0x%x AT_PHNUM=%u AT_HWCAP=0x%x "
             "(%u pairs, %s)",
             at_random_addr, phdr_vaddr, (uint64_t)phnum,
             (uint64_t)hwcap, (uint64_t)naux,
             is_elf ? "ELF" : "non-ELF");

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
    tasks[pid].threads[0].kernel_rsp = tasks[pid].kernel_rsp;
    /* Release-store pairs with the __atomic_load_n ACQUIRE in the
     * scheduler save-gate: any CPU observing exec_pending=1 also sees
     * the iretq frame writes above, so the save-gate can safely skip
     * the msr_read clobber of threads[0].kernel_gs_base. */
    __atomic_store_n(&tasks[pid].exec_pending, 1u, __ATOMIC_RELEASE);
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
        /* Wire GS: swapgs in ISR exchanges GS_BASE <-> KERNEL_GS_BASE.
         * After swapgs on ring-3 return, user-mode GS points to TEB. */
        tasks[pid].kernel_gs_base = teb ? (uint64_t)(uintptr_t)teb : 0;
        /* Mirror into threads[0] so per-thread GS swap reads from thread */
        tasks[pid].threads[0].teb = (void *)teb;
        tasks[pid].threads[0].kernel_gs_base = tasks[pid].kernel_gs_base;
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
    /* ----: Register PEB/TEB in Ob namespace ----
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

    /* Force IRQL back to PASSIVE_LEVEL before abandoning context.
     *
     * Why: when sys_exit reaches us via INT 0x80, the IDT raises IRQL
     * to vector_to_irql(0x80) = 8 and is supposed to lower it back
     * in idt.c:318 (irql_restore block). The forever-yield below
     * means the IDT handler NEVER returns -- the irql_restore block
     * is bypassed and the next task scheduled on this CPU inherits
     * IRQL=8, which then trips mutex_lock's APC_LEVEL guard. Same
     * problem for any future caller invoking task_exit from inside
     * a raised-IRQL context.
     *
     * Lowering here is safe because: (a) per-CPU current_irql is
     * the only state that needs cleanup; (b) task_exit() is an
     * invariant on "we own no spinlocks and no IRQL-elevated state
     * is load-bearing" -- a task exiting while holding a spinlock
     * is a different bug we'd want surfaced via the IRQL violation
     * NOT silenced; (c) KeLowerIrql clamps when old_irql > cur, so
     * if IRQL is already PASSIVE this is a no-op.
     *
     * Discovered 2026-04-20 via the user-mode test launcher: a
     * spawned test_*.exe binary calling sys_exit triggered
     * `IRQL violation in mutex_lock: CPU 0 at IRQL 8, max=1`
     * immediately after Task N exit, blocking the launcher's
     * subsequent klog calls and breaking the rest of boot. */
    KeLowerIrql(PASSIVE_LEVEL);

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

static void thread_free_stacks(struct thread *thr);  /* defined below thread_join */

void task_cleanup(uint32_t pid)
{
    if (pid >= num_tasks || pid == 0)
        return;

    if (tasks[pid].state != TASK_DEAD)
        return;

    /* Close all handles and free handle table */
    ob_handle_table_destroy(&tasks[pid].handle_table);

    /* Free kernel stack (task-level, thread 0).
     *
     * stack_base may have come from EITHER allocator depending on the
     * task's history:
     *   - task_create_user: kmalloc(TASK_STACK_SIZE) -- in heap
     *   - task_create:      pmm_alloc_contiguous(N+1) with guard page below
     *                       (stack_base = guard_base + 4096)
     *   - task_exec:        replaces with the same PMM+guard pattern
     * Calling kfree on a PMM pointer dereferences ptr - HEADER_SIZE as a
     * struct block_header (garbage), then walks coalesce_free_blocks
     * which can corrupt the heap free-list and hang. heap_owns() checks
     * if stack_base is in the kmalloc range; if not, fall back to PMM
     * free using the known TASK_STACK_SIZE + guard-below convention.
     *
     * Discovered 2026-04-20: the user-mode test launcher's task_cleanup
     * hung in kfree -> coalesce_free_blocks because the test_*.exe task
     * was spawned via task_create + task_exec (both PMM stack paths),
     * so kfree got a PMM pointer. */
    if (tasks[pid].stack_base) {
        if (heap_owns(tasks[pid].stack_base)) {
            kfree(tasks[pid].stack_base);
        } else {
            /* PMM-allocated stack with guard page at stack_base - 4096.
             * MUST uninstall the guard BEFORE freeing the frame: the
             * guard install cleared the identity-map PTE for that VA,
             * so handing the physical frame back to PMM without first
             * restoring the PTE means the next pmm_alloc that returns
             * the same frame faults at zero_page (or wherever the
             * caller writes). Discovered 2026-04-20 via Boot Tests:
             * #PF write at CR2=guard_phys, RIP in zero_page, after
             * task_cleanup freed the guard frame. */
            uintptr_t base = (uintptr_t)tasks[pid].stack_base;
            uint32_t pages = (TASK_STACK_SIZE / 4096) + 1;  /* +1 for guard */
            uint32_t p;
            vmm_uninstall_guard_page(base - 4096);
            for (p = 0; p < pages; p++)
                pmm_free_frame((base - 4096) + (uintptr_t)p * 4096);
        }
        tasks[pid].stack_base = (uint8_t *)0;
    }
    tasks[pid].threads[0].kernel_rsp = 0;

    /* Free per-thread kernel + user stacks + TEBs for secondary threads */
    {
        uint32_t ti;
        for (ti = 1; ti < tasks[pid].num_threads; ti++) {
            thread_free_stacks(&tasks[pid].threads[ti]);
            /* Reclaim per-thread user stack pages (deferred from thread_join
             * because vmm_unmap_page lacks SMP TLB shootdown). Safe here
             * because the task is TASK_DEAD -- no other CPU runs it. */
            if (tasks[pid].threads[ti].user_stack_pages > 0 &&
                tasks[pid].threads[ti].user_stack_va && tasks[pid].cr3) {
                uint32_t p;
                for (p = 0; p < tasks[pid].threads[ti].user_stack_pages; p++)
                    vmm_unmap_user_page(tasks[pid].cr3,
                        tasks[pid].threads[ti].user_stack_va +
                        (uintptr_t)p * 4096);
                tasks[pid].threads[ti].user_stack_va = 0;
                tasks[pid].threads[ti].user_stack_pages = 0;
            }
            /* Reclaim per-thread TEB page (deferred from thread_join for
             * same TLB shootdown reason as user stacks). */
            if (tasks[pid].threads[ti].teb && tasks[pid].cr3) {
                vmm_unmap_user_page(tasks[pid].cr3,
                    (uintptr_t)tasks[pid].threads[ti].teb);
                tasks[pid].threads[ti].teb = (void *)0;
                tasks[pid].threads[ti].kernel_gs_base = 0;
            }
        }
    }

    /* Free user stack.
     *
     * Same heap-vs-user-VA hazard as the kernel stack above: this field
     * is set by task_create_user to a kmalloc'd buffer, but task_exec
     * REPLACES it with `(uint8_t *)(USER_ELF_END - USER_STACK_SIZE)` --
     * a user-mode VA inside the per-process page table, NOT a heap
     * pointer. Passing that VA to kfree dereferences `va - HEADER_SIZE`
     * as a struct block_header (garbage from user space), corrupting
     * the free-list and hanging the next allocator call. heap_owns()
     * gates the kfree to only the kmalloc case; the user-VA case is
     * already torn down with the per-process PML4 below. */
    if (tasks[pid].user_stack_base) {
        if (heap_owns(tasks[pid].user_stack_base))
            kfree(tasks[pid].user_stack_base);
        tasks[pid].user_stack_base = (uint8_t *)0;
    }

    /* Free TLS expansion pages and reset state under tls_lock.
     * Holding the lock during unmap prevents another CPU from re-allocating
     * the same VA between our state-clear and our unmap. The unmap is fast
     * (PTE writes + frame free) so the lock hold time is acceptable.
     * task_cleanup only runs after the task has reached TASK_DEAD, so no
     * legitimate concurrent allocator should exist for this pid -- the lock
     * is defense in depth. */
    {
        uint64_t flags;
        spin_lock_irqsave(&tls_lock, &flags);
        if (tasks[pid].tls_expansion_allocated) {
            uintptr_t free_virt = tasks[pid].tls_expansion_virt;
            if (tasks[pid].teb)
                ((TEB *)tasks[pid].teb)->TlsExpansionSlots = (void *)0;
            tasks[pid].tls_expansion_allocated = 0;
            tasks[pid].tls_expansion_phys = 0;
            tasks[pid].tls_expansion_virt = 0;
            vmm_unmap_page(free_virt, 1);
            vmm_unmap_page(free_virt + 4096, 1);
        }
        tasks[pid].tls_bitmap = 0;
        {
            uint32_t w;
            for (w = 0; w < TLS_EXPANSION_BITMAP_WORDS; w++)
                tasks[pid].tls_expansion_bitmap[w] = 0;
        }
        spin_unlock_irqrestore(&tls_lock, flags);
    }

    POST16(POST16_TLS_EXPAND_CLEAN);

    /* Tear down the per-process page tables LAST -- after every user
     * mapping (secondary-thread stacks/TEBs above, plus this task's own
     * user stack, which lives inside this PML4) has been unmapped.
     * vmm_destroy_user_pml4 frees the PML4/PDPT/PD/PT frames cloned by
     * vmm_create_user_pml4 plus any PAGE_OWNED private image frames the
     * fork+exec isolation path (task_exec -> vmm_remap_user_page)
     * installed. Without this every user process leaks its whole
     * page-table tree and its private image frames on exit.
     *
     * cr3 == 0 means a kernel task, or a launcher-spawned task running
     * on the boot/kernel PML4 -- nothing per-process to free.
     *
     * Safety: never free the PML4 that is the CR3 currently loaded on
     * THIS CPU -- the page walker would then traverse freed (and soon
     * reused) frames. All current callers (task_waitpid, the user-mode
     * launcher) run from a DIFFERENT task whose CR3 differs; switching to
     * the kernel CR3 first is defense in depth for any future
     * self-reaping path.
     *
     * SMP precondition (shared with the per-thread reap loop above):
     * this only frees the dead task's frames because TASK_DEAD implies
     * the task is off-CPU on EVERY CPU. The local-CR3 guard does NOT
     * prove that on its own -- a proven-off-CPU-on-all-CPUs reap barrier
     * lands with the per-CPU run queues work (SMP phase 2). Today's flat
     * single-CPU scheduler (one global current_task cursor) makes the
     * dead task off-CPU before any reaper runs, so the assumption holds. */
    if (tasks[pid].cr3) {
        uintptr_t cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 == tasks[pid].cr3)
            __asm__ volatile("mov %0, %%cr3"
                             : : "r"(vmm_get_kernel_cr3()) : "memory");
        vmm_destroy_user_pml4(tasks[pid].cr3);
        tasks[pid].cr3 = 0;
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
 * kthread_create() adds a new kernel-mode schedulable thread to the current task.
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

/* ---- TLS slot allocation (64 static + 1024 expansion) ------------------- */

/*
 * TLS expansion array VA: 2 pages (8 KiB = 1024 x 8 bytes) per task.
 * Placed below the TEB region: 0x7FFD0000 (well below TEB at 0x7FFDB000).
 * Per-thread expansion arrays will use tid*0x2000 offset when threads get
 * separate TEBs; currently single TEB per process.
 */
#define TLS_EXPANSION_VA_BASE  0x7FFD0000ULL
#define TLS_EXPANSION_PAGES    2  /* 8 KiB = 1024 slots x 8 bytes */

/* tls_lock defined at top of file with other static state */

/* Demand-allocate the expansion array for a task's TEB.
 *
 * Two-phase: slow PMM frame allocation OUTSIDE the spinlock, then under the
 * spinlock either commit (we won the race) or free our frames and return
 * success (someone else won; their array is usable). The mapping + zero-fill
 * runs UNDER the lock so a race-loser can never touch the winner's PTEs.
 *
 * Returns 0 on success, -1 on failure. */
static int tls_expansion_demand_alloc(uint32_t pid)
{
    uintptr_t exp_virt = TLS_EXPANSION_VA_BASE;
    uintptr_t exp_phys;
    uint64_t flags;
    TEB *teb;

    if (!tasks[pid].teb) return -1;

    /* Slow PMM allocation outside the spinlock. */
    exp_phys = pmm_alloc_contiguous(TLS_EXPANSION_PAGES);
    if (!exp_phys) return -1;

    /* Lock-held commit: if we lost the race, free our frames and return.
     * Otherwise install PTEs, zero-fill (~few us), and commit. */
    spin_lock_irqsave(&tls_lock, &flags);
    if (tasks[pid].tls_expansion_allocated) {
        spin_unlock_irqrestore(&tls_lock, flags);
        pmm_free_frame(exp_phys);
        pmm_free_frame(exp_phys + 4096);
        return 0;
    }

    /* We won. Install mappings and zero-fill under the lock. */
    vmm_map_page(exp_virt, exp_phys, VMM_USER_RW);
    vmm_map_page(exp_virt + 4096, exp_phys + 4096, VMM_USER_RW);
    {
        uint8_t *p = (uint8_t *)exp_virt;
        uint32_t i;
        for (i = 0; i < TLS_EXPANSION_PAGES * 4096; i++)
            p[i] = 0;
    }

    teb = (TEB *)tasks[pid].teb;
    if (teb)
        teb->TlsExpansionSlots = (void *)exp_virt;
    tasks[pid].tls_expansion_allocated = 1;
    tasks[pid].tls_expansion_phys = exp_phys;
    tasks[pid].tls_expansion_virt = exp_virt;
    spin_unlock_irqrestore(&tls_lock, flags);

    POST16(POST16_TLS_EXPAND_ALLOC);
    return 0;
}

int tls_alloc(uint32_t pid)
{
    uint64_t bm, flags;
    uint32_t i, w;
    int result = -1;
    int need_expansion_alloc = 0;

    if (pid >= num_tasks) return -1;

    POST16(POST16_TLS_EXPAND);

retry:
    spin_lock_irqsave(&tls_lock, &flags);

    /* Try static slots first (0-63) */
    bm = tasks[pid].tls_bitmap;
    for (i = 0; i < TLS_MINIMUM_AVAILABLE; i++) {
        if (!(bm & (1ULL << i))) {
            tasks[pid].tls_bitmap |= (1ULL << i);
            result = (int)i;
            goto out;
        }
    }

    /* Static exhausted -- try expansion slots (64-1087) */
    for (w = 0; w < TLS_EXPANSION_BITMAP_WORDS; w++) {
        bm = tasks[pid].tls_expansion_bitmap[w];
        for (i = 0; i < 64; i++) {
            if (!(bm & (1ULL << i))) {
                if (!tasks[pid].tls_expansion_allocated) {
                    /* Must do slow allocation outside the lock. Drop lock,
                     * allocate, then retry the whole scan (state may have
                     * changed while we were unlocked). */
                    need_expansion_alloc = 1;
                    goto out;
                }
                tasks[pid].tls_expansion_bitmap[w] |= (1ULL << i);
                result = (int)(TLS_MINIMUM_AVAILABLE + w * 64 + i);
                goto out;
            }
        }
    }

out:
    spin_unlock_irqrestore(&tls_lock, flags);

    if (need_expansion_alloc) {
        need_expansion_alloc = 0;
        if (tls_expansion_demand_alloc(pid) < 0)
            return -1;
        goto retry;
    }
    return result;
}

int tls_free(uint32_t pid, uint32_t index)
{
    uint64_t flags;
    int result = -1;

    if (pid >= num_tasks || index >= TLS_MAXIMUM_AVAILABLE) return -1;

    spin_lock_irqsave(&tls_lock, &flags);

    if (index < TLS_MINIMUM_AVAILABLE) {
        /* Static slot */
        if (!(tasks[pid].tls_bitmap & (1ULL << index)))
            goto out;
        tasks[pid].tls_bitmap &= ~(1ULL << index);
        if (tasks[pid].teb) {
            TEB *teb = (TEB *)tasks[pid].teb;
            teb->TlsSlots[index] = 0;
        }
    } else {
        /* Expansion slot */
        uint32_t exp_index = index - TLS_MINIMUM_AVAILABLE;
        uint32_t word = exp_index / 64;
        uint32_t bit = exp_index % 64;
        if (!(tasks[pid].tls_expansion_bitmap[word] & (1ULL << bit)))
            goto out;
        tasks[pid].tls_expansion_bitmap[word] &= ~(1ULL << bit);
        if (tasks[pid].teb) {
            TEB *teb = (TEB *)tasks[pid].teb;
            if (teb->TlsExpansionSlots) {
                uint64_t *slots = (uint64_t *)teb->TlsExpansionSlots;
                slots[exp_index] = 0;
            }
        }
    }
    result = 0;

out:
    spin_unlock_irqrestore(&tls_lock, flags);
    return result;
}

uint64_t tls_get_value(uint32_t pid, uint32_t index)
{
    uint64_t flags, result = 0;

    if (pid >= num_tasks || index >= TLS_MAXIMUM_AVAILABLE || !tasks[pid].teb)
        return 0;

    spin_lock_irqsave(&tls_lock, &flags);

    if (index < TLS_MINIMUM_AVAILABLE) {
        result = ((TEB *)tasks[pid].teb)->TlsSlots[index];
    } else {
        TEB *teb = (TEB *)tasks[pid].teb;
        if (teb->TlsExpansionSlots)
            result = ((uint64_t *)teb->TlsExpansionSlots)[index - TLS_MINIMUM_AVAILABLE];
    }

    spin_unlock_irqrestore(&tls_lock, flags);
    return result;
}

void tls_set_value(uint32_t pid, uint32_t index, uint64_t value)
{
    uint64_t flags;
    TEB *teb;

    if (pid >= num_tasks || index >= TLS_MAXIMUM_AVAILABLE || !tasks[pid].teb)
        return;

    /* Demand-allocate outside the lock for expansion writes. */
    if (index >= TLS_MINIMUM_AVAILABLE && !tasks[pid].tls_expansion_allocated) {
        if (tls_expansion_demand_alloc(pid) < 0)
            return;
    }

    spin_lock_irqsave(&tls_lock, &flags);

    if (index < TLS_MINIMUM_AVAILABLE) {
        ((TEB *)tasks[pid].teb)->TlsSlots[index] = value;
    } else {
        teb = (TEB *)tasks[pid].teb;
        if (teb->TlsExpansionSlots)
            ((uint64_t *)teb->TlsExpansionSlots)[index - TLS_MINIMUM_AVAILABLE] = value;
    }

    spin_unlock_irqrestore(&tls_lock, flags);
}

int kthread_create(thread_entry_t entry, void *arg, uint32_t stack_size)
{
    uint32_t task_idx = current_task;
    struct task *t = &tasks[task_idx];
    uint32_t tid;
    uint8_t *stack;
    uint64_t *sp;

    for (tid = 1; tid < t->num_threads; tid++) {
        if (t->threads[tid].state == THREAD_FREE)
            break;
    }

    if (tid == t->num_threads && t->num_threads >= THREAD_MAX) {
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

    if (tid == t->num_threads)
        t->num_threads++;

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
    t->threads[tid].kernel_rsp = 0;  /* kernel thread -- no rsp0 switching */
    t->threads[tid].kernel_stack_base = (uint8_t *)0;
    t->threads[tid].kernel_stack_pages = 0;
    t->threads[tid].user_stack_va = 0;
    t->threads[tid].user_stack_pages = 0;
    t->threads[tid].teb = (void *)0;          /* kernel thread -- no TEB */
    t->threads[tid].kernel_gs_base = 0;
    /* Register thread with Object Manager */
    ob_thread_create(&t->threads[tid], t->pid);

    klog(LOG_DEBUG, "sched", "Thread %u created in task %u (\"%s\")",
           (uint64_t)tid, (uint64_t)t->pid,
           t->name ? t->name : "?");

    return (int)tid;
}

/* ============================================================================
 * uthread_create() -- create a user-mode thread in the current task.
 *
 * Builds a ring-3 iret frame with its own kernel stack (PMM, 8 KiB + guard)
 * and user stack (PMM + vmm_map_user_page, 16 KiB per thread).
 * Requires the parent task to have a PEB (rejects kernel tasks).
 * ============================================================================ */

/* Free a contiguous PMM allocation (N pages starting at phys) */
static void pmm_free_pages(uintptr_t phys, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        pmm_free_frame(phys + (uintptr_t)i * 4096);
}

int uthread_create(thread_entry_t entry, void *arg, uint32_t user_stack_size)
{
    uint32_t task_idx = current_task;
    struct task *t = &tasks[task_idx];
    uint32_t tid;
    uint64_t *sp;
    uint32_t kstack_pages = 3;  /* 8 KiB stack + 1 guard page = 3 pages */
    uint32_t ustack_pages;
    uintptr_t kstack_phys, ustack_phys;
    uint8_t *kstack;
    uintptr_t ustack_va;
    uint32_t i;

    /* Reject kernel tasks (no PEB) */
    if (!t->peb) {
        klog(LOG_ERROR, "sched",
             "uthread_create: PID %u has no PEB (kernel task)",
             (uint64_t)t->pid);
        return -1;
    }

    if (t->num_threads >= THREAD_MAX) {
        klog(LOG_ERROR, "sched",
             "uthread_create: max threads reached (PID %u)",
             (uint64_t)t->pid);
        return -1;
    }

    if (user_stack_size < USER_STACK_SIZE)
        user_stack_size = USER_STACK_SIZE;
    /* Round up to page multiple */
    user_stack_size = (user_stack_size + 4095) & ~(uint32_t)4095;
    ustack_pages = user_stack_size / 4096;

    /* Allocate per-thread KERNEL stack (PMM: 8 KiB + guard page) */
    kstack_phys = pmm_alloc_contiguous(kstack_pages);
    if (!kstack_phys) {
        klog(LOG_ERROR, "sched",
             "uthread_create: cannot allocate kernel stack");
        return -1;
    }
    kstack = (uint8_t *)kstack_phys;

    /* Guard page at bottom of kernel stack */
    vmm_install_guard_page(kstack_phys, "uthread kernel stack");

    /* Allocate per-thread USER stack (PMM: 16 KiB) */
    ustack_phys = pmm_alloc_contiguous(ustack_pages);
    if (!ustack_phys) {
        klog(LOG_ERROR, "sched",
             "uthread_create: cannot allocate user stack");
        pmm_free_pages(kstack_phys, kstack_pages);
        return -1;
    }

    tid = t->num_threads;

    /* User stack VA: stride downward by user_stack_size per tid */
    ustack_va = USER_THREAD_STACK_BASE -
                (uintptr_t)tid * (uintptr_t)user_stack_size;

    /* Map user stack pages into the parent's per-process PML4 */
    if (!t->cr3) {
        klog(LOG_ERROR, "sched",
             "uthread_create: PID %u has no per-process PML4",
             (uint64_t)t->pid);
        pmm_free_pages(kstack_phys, kstack_pages);
        pmm_free_pages(ustack_phys, ustack_pages);
        return -1;
    }
    for (i = 0; i < ustack_pages; i++) {
        uintptr_t page_va = ustack_va + (uintptr_t)i * 4096;
        uintptr_t page_phys = ustack_phys + (uintptr_t)i * 4096;
        if (vmm_map_user_page(t->cr3, page_va, page_phys) != 0) {
            klog(LOG_ERROR, "sched",
                 "uthread_create: vmm_map_user_page failed at 0x%lx",
                 (uint64_t)page_va);
            /* Unmap already-mapped pages (vmm_unmap_user_page frees
             * each frame internally).  Only free the UNMAPPED tail
             * via pmm_free_frame to avoid double-free. */
            {
                uint32_t j;
                for (j = 0; j < i; j++)
                    vmm_unmap_user_page(t->cr3,
                        ustack_va + (uintptr_t)j * 4096);
                /* Free the unmapped tail frames (i..ustack_pages-1) */
                for (j = i; j < ustack_pages; j++)
                    pmm_free_frame(ustack_phys + (uintptr_t)j * 4096);
            }
            pmm_free_pages(kstack_phys, kstack_pages);
            return -1;
        }
    }

    /* Allocate per-thread TEB at a distinct user VA */
    {
        TEB *thread_teb = teb_alloc_for_task(task_idx, tid,
                                              ustack_va, user_stack_size,
                                              t->peb);
        if (!thread_teb) {
            klog(LOG_ERROR, "sched",
                 "uthread_create: TEB alloc failed for PID %u TID %u",
                 (uint64_t)t->pid, (uint64_t)tid);
            /* Roll back user stack pages */
            {
                uint32_t j;
                for (j = 0; j < ustack_pages; j++)
                    vmm_unmap_user_page(t->cr3,
                        ustack_va + (uintptr_t)j * 4096);
            }
            pmm_free_pages(kstack_phys, kstack_pages);
            return -1;
        }
        t->threads[tid].teb = (void *)thread_teb;
        t->threads[tid].kernel_gs_base =
            (uint64_t)(uintptr_t)thread_teb;
    }

    /* Build ring-3 iret frame on the KERNEL stack.
     * Same 22-qword layout as task_exec() and task_create_user().
     * CS = GDT_USER_CODE | 3, SS = GDT_USER_DATA | 3, RFLAGS = 0x202. */
    {
        /* Usable kernel stack: skip guard page (page 0), use pages 1-2 */
        uint8_t *kstack_top = kstack + kstack_pages * 4096;
        sp = (uint64_t *)kstack_top;
        sp = (uint64_t *)((uint64_t)sp & ~0xFULL);
    }

    {
        uint64_t user_rsp = (ustack_va + user_stack_size) & ~0xFULL;

        sp -= 22;
        sp[0]  = 0;                                /* r15 */
        sp[1]  = 0;                                /* r14 */
        sp[2]  = (uint64_t)arg;                    /* r13 = argument */
        sp[3]  = (uint64_t)entry;                  /* r12 = entry (unused by iret but preserved) */
        sp[4]  = 0;                                /* r11 */
        sp[5]  = 0;                                /* r10 */
        sp[6]  = 0;                                /* r9 */
        sp[7]  = 0;                                /* r8 */
        sp[8]  = 0;                                /* rbp */
        sp[9]  = (uint64_t)arg;                    /* rdi = first arg (SysV ABI) */
        sp[10] = 0;                                /* rsi */
        sp[11] = 0;                                /* rdx */
        sp[12] = 0;                                /* rcx */
        sp[13] = 0;                                /* rbx */
        sp[14] = 0;                                /* rax */
        sp[15] = 0;                                /* int_no */
        sp[16] = 0;                                /* err_code */
        sp[17] = (uint64_t)entry;                  /* rip = user entry */
        sp[18] = GDT_USER_CODE | 3;                /* cs = ring 3 code */
        sp[19] = 0x202;                            /* rflags: IF set */
        sp[20] = user_rsp;                         /* rsp = user stack top */
        sp[21] = GDT_USER_DATA | 3;                /* ss = ring 3 data */
    }

    /* Initialize thread control block */
    t->threads[tid].id = tid;
    t->threads[tid].state = THREAD_READY;
    t->threads[tid].rsp = (uint64_t)sp;
    t->threads[tid].stack_base = (uint8_t *)0; /* not kmalloc'd */
    t->threads[tid].stack_size = 0;
    t->threads[tid].parent_task = task_idx;
    t->threads[tid].exit_status = 0;
    t->threads[tid].join_tid = -1;
    t->threads[tid].priority      = THREAD_PRIO_NORMAL;
    t->threads[tid].base_priority = THREAD_PRIO_NORMAL;

    /* Per-thread kernel stack ownership (for thread_free_stacks) */
    t->threads[tid].kernel_rsp = (uint64_t)(kstack + kstack_pages * 4096);
    t->threads[tid].kernel_stack_base = kstack;
    t->threads[tid].kernel_stack_pages = kstack_pages;

    /* Per-thread user stack ownership (for task_cleanup reclamation) */
    t->threads[tid].user_stack_va = ustack_va;
    t->threads[tid].user_stack_pages = ustack_pages;

    t->num_threads++;

    /* Register thread with Object Manager */
    ob_thread_create(&t->threads[tid], t->pid);

    klog(LOG_INFO, "sched",
         "uthread %u created in PID %u: kstack=0x%lx ustack=0x%lx-0x%lx teb=%p",
         (uint64_t)tid, (uint64_t)t->pid,
         (uint64_t)(uintptr_t)kstack,
         (uint64_t)ustack_va,
         (uint64_t)(ustack_va + user_stack_size),
         (uint64_t)(uintptr_t)t->threads[tid].teb);

    return (int)tid;
}

/* Legacy wrapper -- dispatches to uthread_create if task has PEB, else kthread_create */
int thread_create(thread_entry_t entry, void *arg, uint32_t stack_size)
{
    struct task *t = &tasks[current_task];
    if (t->peb)
        return uthread_create(entry, arg, stack_size ? stack_size : USER_STACK_SIZE);
    return kthread_create(entry, arg, stack_size);
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

/* Free a dead thread's kernel stack.  If kernel_stack_pages > 0 the stack
 * was PMM-allocated (future uthread path); free each page individually.
 * Otherwise, the thread's stack_base was kmalloc'd -- use kfree. */
static void thread_free_stacks(struct thread *thr)
{
    /* PMM-allocated kernel stack (per-thread, for user threads) */
    if (thr->kernel_stack_pages > 0 && thr->kernel_stack_base) {
        uint32_t i;
        for (i = 0; i < thr->kernel_stack_pages; i++)
            pmm_free_frame((uintptr_t)thr->kernel_stack_base +
                           (uintptr_t)i * 4096);
        thr->kernel_stack_base = (uint8_t *)0;
        thr->kernel_stack_pages = 0;
    }
    /* kmalloc'd kernel stack (kernel threads via thread_create/kthread_create) */
    if (thr->stack_base) {
        kfree(thr->stack_base);
        thr->stack_base = (uint8_t *)0;
    }
    thr->kernel_rsp = 0;
}

static void thread_reap_kernel_slot(struct task *t, uint32_t thread_id)
{
    struct thread *thr = &t->threads[thread_id];

    thread_free_stacks(thr);
    thr->state = THREAD_FREE;
    thr->rsp = 0;
    thr->stack_size = 0;
    thr->exit_status = 0;
    thr->join_tid = -1;
    thr->priority = THREAD_PRIO_NORMAL;
    thr->base_priority = THREAD_PRIO_NORMAL;
    thr->parent_task = t->pid;

    while (t->num_threads > 1 &&
           t->threads[t->num_threads - 1].state == THREAD_FREE)
        t->num_threads--;
}

int32_t thread_join(uint32_t thread_id)
{
    struct task *t = &tasks[current_task];

    /* Validate thread ID */
    if (thread_id >= t->num_threads || thread_id == current_thread ||
        t->threads[thread_id].state == THREAD_FREE) {
        klog(LOG_DEBUG, "sched", "Invalid thread ID %u for join",
               (uint64_t)thread_id);
        return -1;
    }

    /* If target thread is already dead, return immediately */
    if (t->threads[thread_id].state == THREAD_DEAD) {
        int32_t status = t->threads[thread_id].exit_status;
        if (t->threads[thread_id].user_stack_pages == 0 &&
            t->threads[thread_id].teb == (void *)0)
            thread_reap_kernel_slot(t, thread_id);
        else
            thread_free_stacks(&t->threads[thread_id]);
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
        if (t->threads[thread_id].user_stack_pages == 0 &&
            t->threads[thread_id].teb == (void *)0)
            thread_reap_kernel_slot(t, thread_id);
        else
            thread_free_stacks(&t->threads[thread_id]);
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
