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
#include "kernel/test/test_usermode.h"  /* test_usermode_capture_begin -- KERNEL_TESTS
                                          * no-op in release builds */
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
#include "kernel/env.h"
#include "kernel/eif.h"
#include "kernel/ipc/signal.h"
#include "kernel/ipc/pgroup.h"       /* pgroup_note_exec, pgroup_jobctl_lock/unlock */
#include "kernel/ob/handle_table.h"
#include "kernel/ob/ob_process.h"
#include "kernel/ob/ob_thread.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/security/token.h"  /* SeCreateSystemToken, NtDuplicateToken, primary-token API */
#include "kernel/nt/syscall_filter.h"
#include "kernel/nt/mitigation_policy.h"
#include "kernel/nt/pledge.h"       /* pledge_unveil_inherit / _teardown */
#include "kernel/ob/ob_job.h"       /* ob_job_fork_inherit / _detach_task */
#include "kernel/quota/quota.h"     /* quota_task_init / quota_task_teardown */
#include "kernel/quota/quota_ledger.h" /* charge gate seal + ledger release */
#include "kernel/msr.h"
#include "kernel/ob/peb.h"
#include "kernel/ob/teb.h"
#include "kernel/acpi.h"
#include "kernel/timer.h"
#include "kernel/time/timer_resolution.h"  /* timer_resolution_release_process (death-path reap) */
#include "kernel/time/wall_clock.h"  /* KeQuerySystemTime / wall_clock_time_sourced (accounting CreateTime) */
#include "kernel/nt/filetime.h"      /* FILETIME_NOW_PLACEHOLDER (accounting CreateTime) */
#include "kernel/vectors.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"
#include "kernel/elf.h"
#include "kernel/csprng.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"  /* boot_halt: fatal SRM baseline failure */
#include "kernel/fs/vfs.h"     /* VFS_MAX_PATH: cwd cap must match the VFS boundary */

/* Pin the cwd cap to the VFS path cap so a cwd can always round-trip through
 * vfs_resolve_path / vfs_open without truncation (5-layer defense, layer 1). */
_Static_assert(TASK_CWD_MAX == VFS_MAX_PATH,
    "task cwd buffer must match VFS_MAX_PATH");

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
static void task_init_accounting(struct task *t);
static void task_init_rlimits_defaults(struct task *t);
static int task_inherit_primary_token(uint32_t parent_pid, ACCESS_TOKEN **out);

/* The rlimits[] array is indexed by the RLIMIT_* ABI values and its size must
 * stay pinned to the Linux UAPI count so a future getrlimit(resource) never
 * indexes out of bounds. */
_Static_assert(RLIM_NLIMITS == 16, "RLIM_NLIMITS must match Linux UAPI RLIM_NLIMITS");
_Static_assert(RLIMIT_RTTIME == RLIM_NLIMITS - 1, "RTTIME must be the last resource index");
_Static_assert(RLIMIT_AS < RLIM_NLIMITS, "every RLIMIT_* index must fit rlimits[]");

/* exec_pending stuck detection threshold (ticks).
 * If exec_pending has been set for more than this many ticks without the
 * task being scheduled in, the frame is stuck and will never be consumed.
 * Force-clear and log error so the task doesn't block forever. */
#define EXEC_PENDING_STUCK_TICKS 10

#ifdef KERNEL_TESTS
/* Witness the scheduler adopting a task's published exec frame, and check
 * -- once per boot, not once per switch -- that dispatch really is
 * BSP-only.
 *
 * The launcher's per-child loader evidence is bound to the child's slot
 * rather than to file-scope globals precisely so a late store cannot be
 * attributed to the next binary. That binding is sound on its own, but the
 * ORDERING of the inputs armed before publication still rests on the
 * single-dispatch invariant (one global current_task, APs do not run
 * scheduled tasks) -- num_tasks is a plain global that the scheduler reads
 * without an acquire, so placing the arming before the increment orders
 * nothing by the C memory model. That invariant is stated in comments at
 * several load-bearing sites and asserted at none, which is what makes it
 * dangerous: the day an AP dispatches, everything keeps working and the
 * evidence quietly starts lying.
 *
 * Checked HERE rather than on every context switch because this is where
 * the claim is actually made -- once per exec adoption -- so the guard
 * costs the scheduler nothing on the ordinary switch path. Reports rather
 * than halts: an unexpected dispatch CPU invalidates test evidence, which
 * is a loud diagnostic, not a reason to take the machine down inside an
 * interrupt handler. Latched so a broken invariant cannot flood the shared
 * klog budget and suppress the diagnostics around it.
 *
 * It is a DETECTOR, not a barrier, and the difference is deliberate: it
 * cannot make the arming ordering correct on an AP, only make its failure
 * visible instead of silent. Nothing here should be read as licensing the
 * plain-store publication above. */
static uint32_t s_utest_nonbsp_dispatch_reported;

/* Returns non-zero when the caller IS on the BSP, so a caller whose target
 * is only valid there can suppress its write instead of making one that
 * may land in another child's slot. */
int task_utest_report_nonbsp_dispatch(uint32_t pid, const char *where)
{
    struct per_cpu_data *cpu = smp_this_cpu();

    if (!cpu || cpu->cpu_id == 0)
        return 1;
    /* RELAXED is sufficient and honest: the latch publishes no data beyond
     * itself, so the exchange only needs to be indivisible, not ordered
     * against anything. A stronger order here would be exactly the kind of
     * unearned guarantee this section removed from the code it replaced. */
    if (!__atomic_exchange_n(&s_utest_nonbsp_dispatch_reported, 1u,
                             __ATOMIC_RELAXED)) {
        /* TASK_UTEST_PID_UNKNOWN rather than a fabricated 0: the one caller
         * that cannot name a pid is the one whose whole problem is that the
         * cursor it would have to ask is untrustworthy here. */
        if (pid == TASK_UTEST_PID_UNKNOWN)
            klog(LOG_ERROR, "sched",
                 "%s ran on CPU %u, not the BSP -- per-child loader "
                 "evidence ordering is no longer guaranteed",
                 where, (uint64_t)cpu->cpu_id);
        else
            klog(LOG_ERROR, "sched",
                 "PID %u: %s ran on CPU %u, not the BSP -- per-child loader "
                 "evidence ordering is no longer guaranteed",
                 (uint64_t)pid, where, (uint64_t)cpu->cpu_id);
    }
    return 0;
}

static void task_utest_note_frame_adopted(struct task *t)
{
    /* Reported but NOT suppressed, and the difference from the two
     * cursor-resolved sites is the whole point: this one is handed its
     * target explicitly (&tasks[next_task] from the scheduler), so it
     * marks the right slot on any CPU. Only the report is at risk
     * off-BSP, not the attribution. */
    (void)task_utest_report_nonbsp_dispatch(t->pid, "exec frame adoption");
    TASK_UTEST_LOADER_MARK_ADOPTED(t);
}
#define TASK_UTEST_NOTE_FRAME_ADOPTED(tp) task_utest_note_frame_adopted(tp)

/* The syscall-return adoption site, which resolves its task through the
 * GLOBAL cursor (`pid = current_task`) rather than being handed a pointer.
 * That puts it on the same side of the asymmetry as ring-3 syscall entry,
 * NOT with the two scheduler sites it otherwise resembles -- so it
 * suppresses off-BSP instead of marking a slot the cursor may have
 * misidentified. Kept as its own helper precisely so the two shapes cannot
 * be confused again: the distinction is which side resolves the target,
 * never which subsystem the call sits in. */
static void task_utest_note_frame_adopted_cursor(uint32_t pid)
{
    if (!task_utest_report_nonbsp_dispatch(pid,
                                           "exec frame adoption (syscall return)"))
        return;
    TASK_UTEST_LOADER_MARK_ADOPTED(&tasks[pid]);
}
#define TASK_UTEST_NOTE_FRAME_ADOPTED_CURSOR(pid) \
    task_utest_note_frame_adopted_cursor(pid)

/* PROVEN ring-3 execution, as opposed to the frame adoption above: the CPU
 * saved this CS when it took the syscall, so an RPL of 3 is its own record
 * that the interrupted instruction ran at user privilege. That closes the
 * half of the never-ran question adoption cannot: a binary that reached a
 * syscall demonstrably executed, so a later timeout is a hang in ITS code
 * rather than a loader or scheduler fault. */
int task_utest_cs_is_user(uint64_t cs)
{
    return (cs & SEL_RPL_MASK) == SEL_RPL_USER;
}

void task_utest_note_user_entry(uint64_t cs)
{
    struct task *t;

    if (!task_utest_cs_is_user(cs))
        return;
    /* Checked BEFORE resolving the cursor, and the write is SUPPRESSED
     * rather than merely reported when it fails.
     *
     * Unlike frame adoption, this site has no explicit target: it resolves
     * through the global current_task cursor, so a child syscalling on an
     * AP would read the BSP's cursor and set ANOTHER child's entered_user
     * -- reintroducing exactly the cross-invocation contamination this
     * section removes, through a write this section adds. Declining to
     * record leaves the field at its conservative zero (never-ran-leaning,
     * see u_record_verdict), which is a missing observation rather than a
     * false one attributed to an innocent binary. Binding this evidence
     * properly needs a per-CPU current-task identity, owned by the
     * per-CPU run-queue work in 03-memory-concurrency/TODO-07. */
    if (!task_utest_report_nonbsp_dispatch(TASK_UTEST_PID_UNKNOWN,
                                           "ring-3 syscall entry"))
        return;
    t = task_current();
    if (t)
        TASK_UTEST_LOADER_MARK_ENTERED_USER(t);
}
#else
#define TASK_UTEST_NOTE_FRAME_ADOPTED(tp) ((void)(tp))
#define TASK_UTEST_NOTE_FRAME_ADOPTED_CURSOR(pid) ((void)(pid))
#endif /* KERNEL_TESTS */

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

#ifdef KERNEL_TESTS
    /* A task whose entry returns normally dies HERE rather than through
     * task_exit, so this is its own death transition and takes the capture
     * snapshot before anything is published dead. */
    task_utest_cap_note_task_death(&tasks[current_task]);
#endif
    /* Task finished. Main thread DEAD under its APC lock FIRST (before
     * TASK_DEAD is visible) so no cross-thread KeInsertQueueApc can enqueue
     * onto a dead process's thread-0 in the window between the stores. */
    {
        uint64_t af;
        spin_lock_irqsave(&tasks[current_task].threads[0].apc_lock, &af);
        tasks[current_task].threads[0].state = THREAD_DEAD;
        spin_unlock_irqrestore(&tasks[current_task].threads[0].apc_lock, af);
    }
    /* Run RundownRoutine for any APCs still queued on the dead main thread. */
    apc_rundown_thread(&tasks[current_task].threads[0]);
    tasks[current_task].state = TASK_DEAD;
    /* A task whose entry returns normally dies HERE (not via task_exit), so it
     * runs the same shared DEAD-transition teardown: syscall-filter count, OB
     * process object, Job Object membership, and any leaked timer-resolution
     * request. Omitting any of these leaked the resource for a task that simply
     * returned. The memory needing the reap barrier frees later in task_cleanup. */
    task_death_teardown(&tasks[current_task]);
    klog(LOG_DEBUG, "sched", "Task %u (\"%s\") exited",
           (uint64_t)tasks[current_task].pid,
           tasks[current_task].name ? tasks[current_task].name : "?");

    /* Same IRQL-cleanup as task_exit: see the long comment in task_exit
     * below. Forces PASSIVE so the next task scheduled on this CPU
     * starts at the right level even though we never return from this
     * forever-yield (so the IDT's irql_restore block is bypassed).
     * Forced (unbalanced) lower -- counted + classified, see task_exit. */
    KeLowerIrqlForced(PASSIVE_LEVEL, "task_wrapper");

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
        tasks[i].pgid = 0;        /* clear stale group/session on slot reuse; */
        tasks[i].sid = 0;         /* PID 0 (set below) legitimately leads session 0 */
        tasks[i].has_execed = 0;
        tasks[i].exit_status = 0;
        tasks[i].wait_pid = -1;
        tasks[i].exec_pending = 0;
        tasks[i].stack_pending_free = (uint8_t *)0;
        tasks[i].forked_shares_parent_image = 0;
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
        tasks[i].cwd[0] = '\0';
        {
            spinlock_t init = SPINLOCK_INIT;
            tasks[i].cwd_lock = init;
            tasks[i].rlimit_lock = init;
            tasks[i].unveil_lock = init;
        }
        /* Per-process environment + argv: NULL until env_init_defaults /
         * task_set_argv populate them. environ_lock is a mutex (init once here;
         * slots are never reused without a task_cleanup env_free in between). */
        tasks[i].environ = NULL;
        tasks[i].environ_count = 0;
        tasks[i].environ_bytes = 0;
        tasks[i].argv = NULL;
        tasks[i].argc = 0;
        mutex_init(&tasks[i].environ_lock, "environ");
        mutex_init(&tasks[i].chdir_lock, "chdir");   /* SetCurrentDirectory commit txn */
        tasks[i].quota_policy_lock.flag = 0;   /* ProcessQuotaLimits commit txn */
        quota_policy_reset(&tasks[i]);
        tasks[i].pledge_mask = 0;
        tasks[i].unveil_list = (struct unveil_entry *)0;
        tasks[i].unveil_locked = 0;
        tasks[i].unveil_active = 0;
        tasks[i].unveil_gen = 0;
        tasks[i].job = NULL;
        tasks[i].job_lock.flag = 0;
        tasks[i].num_threads = 0;
        for (j = 0; j < THREAD_MAX; j++) {
            tasks[i].threads[j].id = 0;
            tasks[i].threads[j].state = THREAD_DEAD;
            tasks[i].threads[j].pledge_pending = 0;
#ifdef KERNEL_TESTS
            thread_utest_cap_reset(&tasks[i].threads[j]);
#endif
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
            tasks[i].threads[j].kernel_exception_list = (struct ki_exception_registration *)0;
        }
    }

    /* Task 0: the current boot/main thread.
     * Its stack is the existing kernel boot stack -- we don't allocate one.
     * RSP will be saved by switch_context/schedule when it first yields. */
    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack_base = (uint8_t *)0;  /* boot stack, don't free */
    tasks[0].name = "main";
    task_set_cwd(&tasks[0], "C:\\");     /* system process starts at the boot drive root */
    task_init_accounting(&tasks[0]);     /* PID 0 accrues time too; stamp its CreateTime */
    task_init_rlimits_defaults(&tasks[0]); /* PID 0 is the one true source of default limits */
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
    /* APC state for the main thread (tasks[] is BSS-zero, so the apc_lock is a
     * valid unlocked spinlock; explicit reset + the one-time init log). */
    apc_thread_init(&tasks[0].threads[0].apc_state, &tasks[0]);
    tasks[0].threads[0].kernel_apc_disable  = 0;
    tasks[0].threads[0].special_apc_disable = 0;
    klog(LOG_INFO, "apc", "initialized per-thread APC queues");
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

/* Initialize the per-process accounting fields (times, I/O, ctxsw) on a fresh
 * or reused task slot. Called from BOTH task_create() and task_create_user() so
 * the two init paths can never drift.
 * create_time_filetime is a STABLE absolute FILETIME captured ONLY when the wall
 * clock was sourced from real hardware; otherwise FILETIME_NOW_PLACEHOLDER, so a
 * later KeSetSystemTime/NTP step cannot retroactively move an old CreateTime.
 * Runs single-threaded at creation (the slot is not yet schedulable), so plain
 * stores are correct here; the counters become __atomic RELAXED once the task
 * can run and the tick ISR / I/O paths touch them. */
static void task_init_accounting(struct task *t)
{
    t->create_time_ns = uptime_ns();
    t->create_time_filetime = wall_clock_time_sourced()
                                  ? KeQuerySystemTime()
                                  : FILETIME_NOW_PLACEHOLDER;
    t->user_time_ns   = 0;
    t->kernel_time_ns = 0;
    t->io_read_count  = 0;
    t->io_read_bytes  = 0;
    t->io_write_count = 0;
    t->io_write_bytes = 0;
    t->vol_ctxsw      = 0;
    t->invol_ctxsw    = 0;
    /* EVERY cumulative metric resets here, not just the original six: tasks[]
     * slots are REUSED, so a counter left standing is inherited by the next
     * process in that slot and reported as its own. The saturating subtract in
     * task_acct_delta_since() assumes exactly this reset -- a baseline taken
     * against a stale-high counter reports no contribution instead of the
     * member's real usage, silently zeroing a job's aggregate. */
    t->io_other_count = 0;
    t->io_other_bytes = 0;
    t->wakeup_count   = 0;
    t->timer_create_count = 0;
    t->except_telem_rate = 0;   /* s16 telemetry: window=0 => first event opens a fresh window */
}

/* Stamp the sane per-process rlimit defaults. Called for PID 0 ONLY; every other
 * task inherits its creator's limits via task_rlimit_inherit(), so a process that
 * irreversibly lowered a hard limit cannot spawn a child with fresh, higher
 * defaults. Runs single-threaded at PID 0 setup, so no lock is taken here. */
static void task_init_rlimits_defaults(struct task *t)
{
    int i;
    for (i = 0; i < RLIM_NLIMITS; i++) {
        t->rlimits[i].rlim_cur = RLIM_INFINITY;
        t->rlimits[i].rlim_max = RLIM_INFINITY;
    }
    t->rlimits[RLIMIT_STACK].rlim_cur   = RLIMIT_DEFAULT_STACK_CUR;
    t->rlimits[RLIMIT_CORE].rlim_cur    = 0;                       /* no core dumps by default */
    t->rlimits[RLIMIT_NOFILE].rlim_cur  = RLIMIT_DEFAULT_NOFILE_CUR;
    t->rlimits[RLIMIT_NOFILE].rlim_max  = RLIMIT_DEFAULT_NOFILE_MAX;
    /* MEMLOCK carries a FINITE hard ceiling: an unprivileged raise of the soft
     * limit is capped at the hard limit, so leaving rlim_max at RLIM_INFINITY
     * would let any process restore an unbounded pin allowance. Match Linux
     * (soft == hard == 8 MiB for the unprivileged default). */
    t->rlimits[RLIMIT_MEMLOCK].rlim_cur = RLIMIT_DEFAULT_MEMLOCK_CUR;
    t->rlimits[RLIMIT_MEMLOCK].rlim_max = RLIMIT_DEFAULT_MEMLOCK_MAX;
}

/* Copy the creator's full rlimit array into a fresh child slot. Snapshots the
 * parent under its rlimit_lock (an SMP-coherent read; uncontended on the single
 * CPU today). The child is not yet published (num_tasks not yet bumped), so it
 * needs no lock of its own. Copying ALL RLIM_NLIMITS entries also guarantees a
 * reused slot never inherits a prior tenant's limits. */
void task_rlimit_inherit(struct task *child, struct task *parent)
{
    uint64_t flags;
    int i;
    spin_lock_irqsave(&parent->rlimit_lock, &flags);
    for (i = 0; i < RLIM_NLIMITS; i++)
        child->rlimits[i] = parent->rlimits[i];
    spin_unlock_irqrestore(&parent->rlimit_lock, flags);
}

/* --- Per-process mitigation policy (mitigation_flags) -------------------- */

void task_mitigation_apply(struct task *t, uint64_t add_mask)
{
    /* Monotonic OR: acquire-release RMW so a concurrent setter on another CPU
     * cannot lose a bit (a plain t->mitigation_flags |= mask would). */
    __atomic_fetch_or(&t->mitigation_flags, add_mask, __ATOMIC_ACQ_REL);
}

uint64_t task_mitigation_get(struct task *t)
{
    return __atomic_load_n(&t->mitigation_flags, __ATOMIC_ACQUIRE);
}

int task_mitigation_child_set(struct task *t, uint32_t child_flags)
{
    if (child_flags & PROC_MIT_CHILD_NO_CHILD_CREATION) {
        task_mitigation_apply(t, MIT_NO_CHILD_PROCESS);
        return 0;
    }
    /* The request omits NoChildProcessCreation. If the policy is already set,
     * this is an attempt to clear an irreversible restriction -- refuse it. */
    if (task_mitigation_get(t) & MIT_NO_CHILD_PROCESS)
        return -1;
    return 0;  /* not set, not being set: benign no-op */
}

int task_rlimit_get(struct task *t, int resource, rlimit_t *out)
{
    uint64_t flags;
    if (!t || !out || resource < 0 || resource >= RLIM_NLIMITS) {
        if (out) {
            out->rlim_cur = 0;
            out->rlim_max = 0;
        }
        return RLIMIT_ERR_INVAL;
    }
    spin_lock_irqsave(&t->rlimit_lock, &flags);
    *out = t->rlimits[resource];
    spin_unlock_irqrestore(&t->rlimit_lock, flags);
    return RLIMIT_OK;
}

int task_rlimit_set(struct task *t, int resource, const rlimit_t *nl,
                    int caller_privileged)
{
    uint64_t flags;
    rlimit_t want;
    int rc = RLIMIT_OK;

    if (!t || !nl || resource < 0 || resource >= RLIM_NLIMITS)
        return RLIMIT_ERR_INVAL;
    /* Single copy-in: validate, authorize, and commit the SAME snapshot so a
     * concurrent writer (or a future user-copy boundary) cannot present an
     * allowed value at the check and a different one at the commit. A userspace
     * caller must copy_from_user into *nl before calling this. */
    want = *nl;
    if (want.rlim_cur > want.rlim_max)
        return RLIMIT_ERR_INVAL;

    spin_lock_irqsave(&t->rlimit_lock, &flags);
    if (want.rlim_max > t->rlimits[resource].rlim_max && !caller_privileged) {
        /* Raising the hard limit needs SeIncreaseQuotaPrivilege; lowering it (or
         * moving the soft limit within the hard cap) is always self-permitted. */
        rc = RLIMIT_ERR_PERM;
    } else {
        t->rlimits[resource] = want;
    }
    spin_unlock_irqrestore(&t->rlimit_lock, flags);
    return rc;
}

/* Release a stack run whose guard install FAILED.
 *
 * A failed install touches no PTE, so the run is normally still the caller's to
 * free -- EXCEPT for VMM_GUARD_VA_UNSAFE, which says this run's identity VA
 * maps somebody else's frame. Such a frame is unusable by ANY consumer in an
 * identity-mapped kernel: whoever the PMM handed it to next would write through
 * that VA into the other frame. Quarantine it and say so; a bounded leak is
 * recoverable, silently poisoning the free list is not. */
static void stack_run_release_after_guard_failure(uintptr_t base, uint32_t pages,
                                                  int guard_rc)
{
    if (guard_rc == VMM_GUARD_VA_UNSAFE) {
        klog(LOG_ERROR, "sched",
             "quarantining stack run at %p -- its identity VA maps another frame",
             (void *)base);
        return;
    }
    pmm_free_contiguous(base, (uint64_t)pages);
}

/* arm_capture: when non-zero (KERNEL_TESTS builds only -- release callers
 * always pass 0 via task_create()), the new task's source-level output
 * capture is armed as this function's OWN owner (active=1,
 * owner_pid=self) BEFORE the task is published (num_tasks++ below), so a
 * task selected by the scheduler on another CPU the instant it becomes
 * runnable can never observe capture_active still unset. task_create()
 * and task_create_captured() are thin wrappers over this. */
/* test_path / expect_digest are the KERNEL_TESTS loader inputs armed into
 * the new slot before publication; both are NULL for every non-launcher
 * caller and ignored entirely in the release flavor. */
static int task_create_internal(task_entry_t entry, const char *name,
                                int arm_capture,
                                const char *test_path,
                                const uint8_t *expect_digest)
{
    uint32_t pid;
    uint8_t *stack;
    uint64_t *sp;
    ACCESS_TOKEN *inherited_token = (ACCESS_TOKEN *)0;

    if (num_tasks >= TASK_MAX) {
        klog(LOG_ERROR, "sched", "task_create: max tasks reached");
        return -1;
    }

    pid = num_tasks;

    /* Inherit the creating task's primary token before allocating anything, so
     * a fail-closed policy miss costs nothing to unwind. A kernel task created
     * after SRM init runs as a copy of its creator's token (PID 0 = SYSTEM at
     * boot); the slot's token pointer is overwritten below, clearing any stale
     * tenant. */
    if (task_inherit_primary_token(current_task, &inherited_token) < 0) {
        klog(LOG_ERROR, "sched",
             "task_create: primary-token inheritance failed; failing closed");
        return -1;
    }

    /* Job Object inheritance BEFORE any further allocation, so a creator whose
     * job is terminated or at its active-process limit fails process creation
     * CLOSED with only the token to unwind. Without this, a job member could
     * spawn a child outside the job via NtCreateProcess, escaping active-process
     * limits and job-wide termination. Set the fields ob_job_assign reads (pid,
     * state, job) FIRST -- a reused slot still holds a prior tenant's stale
     * state (possibly TASK_DEAD, which would wrongly reject) and job pointer.
     * The TCB init block re-sets pid/state idempotently and no longer clears
     * job (this call owns it). */
    tasks[pid].pid = pid;
    tasks[pid].state = TASK_READY;
    tasks[pid].job = NULL;
    tasks[pid].job_absorb.active = 0;
    tasks[pid].job_lock.flag = 0;    /* unlocked; guards t->job for assign/detach */
    /* Stale-slot reset of the quota fields MUST precede ob_job_fork_inherit:
     * joining a job now reads task->quota under task->quota_lock (the job
     * absorbs the joiner's usage), so a reused slot would hand that read a
     * prior tenant's freed block pointer and a possibly-still-locked lock word.
     * Same reason job/job_absorb are reset just above. */
    tasks[pid].quota = NULL;
    tasks[pid].quota_user = NULL;
    tasks[pid].quota_lock.flag = 0;
    /* Re-open the charge gate and drop the stale ledger POINTER. Both are
     * load-bearing on a reused slot, in opposite directions: the previous tenant
     * left the gate SEALED at its death, so a slot that inherited it would refuse
     * every charge this new process ever makes; and the ledger pointer was
     * cleared at that tenant's reap, but clearing it again here is what keeps the
     * invariant true for any path that reaches a slot without a reap (the pointer
     * is not ours to release -- the reap already dropped the task's reference,
     * and any surviving obligation holds its own). */
    atomic64_set(&tasks[pid].quota_gate, QUOTA_GATE_PACK(QUOTA_GATE_OPEN, 0));
    tasks[pid].quota_ledger = NULL;
    tasks[pid].quota_policy_lock.flag = 0;   /* same reason: never inherit a held lock word */
    /* Windows quota limits are per-process and NOT inherited (Windows seeds a
     * new process from the system defaults). Reset rather than copy, so a
     * recycled slot cannot present the dead tenant's working-set caps. */
    quota_policy_reset(&tasks[pid]);
    if (ob_job_fork_inherit(&tasks[pid], &tasks[current_task]) != 0) {
        klog(LOG_ERROR, "sched",
             "task_create: job inheritance rejected (terminated/at-limit); failing closed");
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        return -1;
    }
    /* Fail closed -- an unaccounted process is an unenforced limit. */
    if (quota_task_init(&tasks[pid], inherited_token) != STATUS_SUCCESS) {
        klog(LOG_ERROR, "sched", "task_create: quota block allocation failed");
        ob_job_detach_task(&tasks[pid]);
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        return -1;
    }

    /* Allocate task stack from PMM with guard page at the bottom.
     * Stack grows down, so guard page catches overflow before it
     * corrupts adjacent memory.  PMM gives identity-mapped pages. */
    {
        uint32_t stack_pages = TASK_STACK_SIZE / 4096;
        uintptr_t stack_base = pmm_alloc_contiguous(stack_pages + 1);
        if (!stack_base) {
            klog(LOG_ERROR, "sched", "task_create: cannot allocate stack");
            quota_task_teardown(&tasks[pid]);  /* roll back the early quota init */
            ob_job_detach_task(&tasks[pid]);   /* roll back the early job inherit */
            if (inherited_token)
                PsDereferencePrimaryToken(inherited_token);
            return -1;
        }
        int guard_rc = vmm_install_guard_page(stack_base,
                                              "GUARD: kernel task stack overflow");
        if (guard_rc != VMM_GUARD_OK) {
            /* Refuse rather than run a task on an unguarded kernel stack: a
             * silent stack overflow corrupts whatever frame sits below it,
             * while a refused task_create is visible and recoverable. */
            klog(LOG_ERROR, "sched",
                 "task_create: no guard page available for kernel stack");
            stack_run_release_after_guard_failure(stack_base, stack_pages + 1, guard_rc);
            quota_task_teardown(&tasks[pid]);  /* roll back the early quota init */
            ob_job_detach_task(&tasks[pid]);   /* roll back the early job inherit */
            if (inherited_token)
                PsDereferencePrimaryToken(inherited_token);
            return -1;
        }
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
    task_set_cwd(&tasks[pid], "C:\\");   /* kernel threads default to the boot drive root */
    tasks[pid].parent_pid = current_task;
    /* pgid/sid/has_execed are inherited + published atomically under the
     * job-control lock at num_tasks++ (see below), so the child can never appear
     * in a process group mid-setsid. */
    tasks[pid].exit_status = 0;
    tasks[pid].wait_pid = -1;
    tasks[pid].exec_pending = 0;
    /* A recycled slot must never inherit the previous occupant's parked exec
     * stack -- that pointer was already freed by its task_cleanup, so keeping
     * it would double-free on this task's first exec. */
    tasks[pid].stack_pending_free = (uint8_t *)0;
    tasks[pid].forked_shares_parent_image = 0;
    /* Harness self-report is per-process and never inherited: a recycled
     * slot presenting the previous tenant's submission would make this
     * task's own UTEST_END read as the forbidden second call. */
    TASK_UTEST_REPORT_RESET(&tasks[pid]);
    task_init_accounting(&tasks[pid]);
    task_rlimit_inherit(&tasks[pid], &tasks[current_task]); /* inherit creator's limits */
    tasks[pid].cr3 = 0;  /* kernel task uses boot PML4 */
    tasks[pid].syscall_filter = (struct syscall_filter *)0;  /* no filter; clear stale tenant ptr on slot reuse */
    tasks[pid].syscall_filter_counted = 0;
    tasks[pid].mitigation_flags = 0;  /* fresh: no mitigation policy; clear stale bits on slot reuse */
    tasks[pid].search_path_mode = 0;  /* unset -> default safe SearchPathW ordering; clear stale bits on slot reuse */
    tasks[pid].pledge_mask = 0;       /* not pledged; clear stale bits on slot reuse */
    fault_site_reset_task(&tasks[pid]);  /* no stale fault-site arm on slot reuse */
    tasks[pid].unveil_list = (struct unveil_entry *)0;  /* full FS visible; clear stale tenant list ptr */
    tasks[pid].unveil_locked = 0;
    tasks[pid].unveil_active = 0;
    tasks[pid].unveil_gen = 0;
    /* tasks[pid].job is set by the early ob_job_fork_inherit above (creator's
     * job or NULL) -- do NOT clear it here or the inheritance would be lost. */
    tasks[pid].threads[0].pledge_pending = 0;

    /* Thread 0 = main thread (uses task's kernel stack) */
    tasks[pid].threads[0].id = 0;
    tasks[pid].threads[0].state = THREAD_READY;
    tasks[pid].threads[0].stack_base = (uint8_t *)0;  /* shares task stack */
    tasks[pid].threads[0].stack_size = 0;
    tasks[pid].threads[0].parent_task = pid;
    tasks[pid].threads[0].join_tid = -1;
    tasks[pid].threads[0].priority      = THREAD_PRIO_NORMAL;
    tasks[pid].threads[0].base_priority = THREAD_PRIO_NORMAL;
    tasks[pid].threads[0].previous_mode = 0;  /* KernelMode on slot reuse */
    fault_site_reset_thread(&tasks[pid].threads[0]);  /* no stale site marker */
    tasks[pid].threads[0].in_system_service = 0;  /* no syscall in flight on a reused slot */
#ifdef KERNEL_TESTS
    thread_utest_cap_reset(&tasks[pid].threads[0]);  /* no open capture write on a reused slot */
#endif
    tasks[pid].threads[0].impersonation_token = (void *)0;  /* no stale impersonation on slot reuse */
    tasks[pid].threads[0].kernel_exception_list = (struct ki_exception_registration *)0;  /* no stale KI_TRY chain */
    tasks[pid].threads[0].teb = (void *)0;
    tasks[pid].threads[0].kernel_gs_base = 0;
    /* Reset the main thread's APC state (sets apc_state.process; matches the
     * secondary-thread create paths). Fresh task slot, but keep the contract
     * uniform: every THREAD_READY thread-0 has initialized APC state. */
    {
        uint64_t af;
        spin_lock_irqsave(&tasks[pid].threads[0].apc_lock, &af);
        apc_thread_init(&tasks[pid].threads[0].apc_state, &tasks[pid]);
        tasks[pid].threads[0].kernel_apc_disable  = 0;
        tasks[pid].threads[0].special_apc_disable = 0;
        spin_unlock_irqrestore(&tasks[pid].threads[0].apc_lock, af);
    }
    tasks[pid].num_threads = 1;
    /* Primary token slot: explicit assignment doubles as the stale-slot reset
     * (a prior tenant's pointer must never survive a reused slot). Set before
     * num_tasks++ so the task is never published token-less; the slot owns the
     * dup's reference (NULL only pre-SRM-init, tolerated for KernelMode). */
    tasks[pid].token = inherited_token;
    signal_init_task(&tasks[pid].signals);
    ob_handle_table_init(&tasks[pid].handle_table);
    /* Every constructor resets the capture fields, matching
     * TASK_UTEST_REPORT_RESET's pattern, then arms them for THIS task
     * (owner=self) when requested -- both BEFORE num_tasks++ publishes
     * the task, for the same reason the token assignment above runs
     * first: nothing may observe a half-initialized slot. task_exec()
     * does NOT reset these (unlike utest_report): capture ownership is
     * per-process-lifetime, not per-loaded-image, so it survives the
     * kernel-task-to-ring-3 self-transition this loader performs. */
    TASK_UTEST_CAPTURE_RESET(&tasks[pid]);
    /* Loader evidence: reset here with the rest, then armed with THIS
     * invocation's inputs below -- both before num_tasks++, for the same
     * reason the capture arming is. */
    TASK_UTEST_LOADER_RESET(&tasks[pid]);
#ifdef KERNEL_TESTS
    if (arm_capture) {
        tasks[pid].utest_capture_active = 1;
        tasks[pid].utest_capture_owner_pid = pid;
        /* The loader reads these once it is scheduled in; publishing them
         * here rather than after the spawn call returns is what binds them
         * to THIS child.
         *
         * Be precise about what that ordering rests on, because the whole
         * point of this record is to stop crediting guarantees nothing
         * provides. It is NOT a C memory-model edge: num_tasks is a plain
         * global the scheduler reads without an acquire, so placement
         * before the increment orders nothing by itself. It is the
         * single-dispatch invariant -- one global current_task, APs park
         * in ap_entry() without ever calling schedule(). That invariant is
         * CHECKED, not asserted: task_utest_note_frame_adopted() reports
         * once if a frame is ever adopted off the BSP, and the loader
         * reports once if it starts on one. Neither is a barrier and
         * neither runs on the ordinary switch path; they turn a silent
         * wrong answer into a loud one. The release/acquire publication
         * protocol that would make this ordering real belongs to the
         * per-CPU run-queue work in 03-memory-concurrency/TODO-07. */
        tasks[pid].utest_loader.test_path     = test_path;
        tasks[pid].utest_loader.expect_digest = expect_digest;
        /* The owner-to-name binding MUST reach the wire before num_tasks++
         * below makes this task schedulable -- an already-published task
         * selected by another CPU could otherwise emit a capture chunk
         * record before a downstream consumer has any binding for its
         * owner pid. */
        test_usermode_capture_begin(pid, name);
    }
#else
    (void)arm_capture;
    /* Same reason as arm_capture above: the loader inputs stay in the
     * signature for BOTH flavors so the wrapper contract does not change,
     * but nothing consumes them when the record does not exist. -Werror
     * -Wunused-parameter turns the omission into a release-build failure
     * that the default KERNEL_TESTS=on build cannot see. */
    (void)test_path;
    (void)expect_digest;
#endif
    /* Inherit the creator's process group + session AND publish the child in one
     * job-control critical section: a concurrent setsid group-reuse scan then
     * either sees this child (and rejects) or does not (child not yet a member) --
     * never a half-published cross-session membership. */
    {
        uint64_t jf = pgroup_jobctl_lock();
        tasks[pid].pgid = tasks[current_task].pgid;
        tasks[pid].sid  = tasks[current_task].sid;
        tasks[pid].has_execed = 0;
        num_tasks++;
        pgroup_jobctl_unlock(jf);
    }

    /* Register process and main thread with Object Manager */
    ob_process_create(&tasks[pid]);
    ob_thread_create(&tasks[pid].threads[0], pid);

    klog(LOG_DEBUG, "sched", "Task %u (\"%s\") created (kernel)",
           (uint64_t)pid, name ? name : "?");

    return (int)pid;
}

int task_create(task_entry_t entry, const char *name)
{
    return task_create_internal(entry, name, 0,
                                (const char *)0, (const uint8_t *)0);
}

#ifdef KERNEL_TESTS
int task_create_captured(task_entry_t entry, const char *name,
                         const char *test_path,
                         const uint8_t *expect_digest)
{
    return task_create_internal(entry, name, 1, test_path, expect_digest);
}
#endif

int task_create_user(task_entry_t entry, const char *name)
{
    uint32_t pid;
    uint8_t *kstack, *ustack;
    uint64_t *sp;
    ACCESS_TOKEN *inherited_token = (ACCESS_TOKEN *)0;

    if (num_tasks >= TASK_MAX) {
        klog(LOG_ERROR, "sched", "task_create_user: max tasks reached");
        return -1;
    }

    pid = num_tasks;

    /* A user task MUST carry a primary token (ring-3 code is subject to token
     * checks): inherit the creator's token before allocating anything and fail
     * closed on a policy miss. Unlike kernel task_create, a NULL result (the
     * helper's pre-SRM-init tolerance) is ALSO fail-closed here -- no ring-3
     * task may be published token-less, even in the pre-init window. */
    if (task_inherit_primary_token(current_task, &inherited_token) < 0 ||
        !inherited_token) {
        klog(LOG_ERROR, "sched",
             "task_create_user: primary-token inheritance failed; failing closed");
        return -1;
    }

    /* Job Object inheritance BEFORE any further allocation (see task_create):
     * a job member spawning a user process inherits the job, fail-closed if the
     * job is terminated or at its active-process limit. Fields ob_job_assign
     * reads (pid, state, job) are set first; the TCB init block re-sets pid/state
     * and no longer clears job (this call owns it). */
    tasks[pid].pid = pid;
    tasks[pid].state = TASK_READY;
    tasks[pid].job = NULL;
    tasks[pid].job_absorb.active = 0;
    tasks[pid].job_lock.flag = 0;    /* unlocked; guards t->job for assign/detach */
    /* Quota stale-slot reset BEFORE the job inherit, same ordering requirement
     * as task_create (see the comment there). */
    tasks[pid].quota = NULL;
    tasks[pid].quota_user = NULL;
    tasks[pid].quota_lock.flag = 0;
    /* Re-open the charge gate and drop the stale ledger POINTER. Both are
     * load-bearing on a reused slot, in opposite directions: the previous tenant
     * left the gate SEALED at its death, so a slot that inherited it would refuse
     * every charge this new process ever makes; and the ledger pointer was
     * cleared at that tenant's reap, but clearing it again here is what keeps the
     * invariant true for any path that reaches a slot without a reap (the pointer
     * is not ours to release -- the reap already dropped the task's reference,
     * and any surviving obligation holds its own). */
    atomic64_set(&tasks[pid].quota_gate, QUOTA_GATE_PACK(QUOTA_GATE_OPEN, 0));
    tasks[pid].quota_ledger = NULL;
    tasks[pid].quota_policy_lock.flag = 0;   /* same reason: never inherit a held lock word */
    quota_policy_reset(&tasks[pid]);   /* see task_create: per-process, never inherited */
    if (ob_job_fork_inherit(&tasks[pid], &tasks[current_task]) != 0) {
        klog(LOG_ERROR, "sched",
             "task_create_user: job inheritance rejected (terminated/at-limit); failing closed");
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        return -1;
    }
    if (quota_task_init(&tasks[pid], inherited_token) != STATUS_SUCCESS) {
        klog(LOG_ERROR, "sched", "task_create_user: quota block allocation failed");
        ob_job_detach_task(&tasks[pid]);
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        return -1;
    }

    /* Allocate kernel stack (for interrupt/syscall handling) */
    kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!kstack) {
        klog(LOG_ERROR, "sched", "task_create_user: cannot allocate kernel stack");
        quota_task_teardown(&tasks[pid]);  /* roll back the early quota init */
        ob_job_detach_task(&tasks[pid]);   /* roll back the early job inherit */
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
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
    /* User process: inherit the creator's cwd (Windows CreateProcess semantics). */
    {
        char parent_cwd[TASK_CWD_MAX];
        task_get_cwd(&tasks[current_task], parent_cwd, sizeof(parent_cwd));
        task_set_cwd(&tasks[pid], parent_cwd[0] ? parent_cwd : "C:\\");
    }
    tasks[pid].parent_pid = current_task;
    /* pgid/sid/has_execed inherited + published atomically under the job-control
     * lock at num_tasks++ (below); CreateProcess starts the child in its parent's
     * group until it setpgid/setsid to detach. */
    tasks[pid].exit_status = 0;
    tasks[pid].wait_pid = -1;
    tasks[pid].exec_pending = 0;
    /* A recycled slot must never inherit the previous occupant's parked exec
     * stack -- that pointer was already freed by its task_cleanup, so keeping
     * it would double-free on this task's first exec. */
    tasks[pid].stack_pending_free = (uint8_t *)0;
    tasks[pid].forked_shares_parent_image = 0;
    /* Harness self-report is per-process and never inherited: a recycled
     * slot presenting the previous tenant's submission would make this
     * task's own UTEST_END read as the forbidden second call. */
    TASK_UTEST_REPORT_RESET(&tasks[pid]);
    /* Same "every constructor resets this" rule as the report above --
     * task_create_internal and task_fork both already call this; a
     * recycled slot's stale owner pid or partially-filled staging buffer
     * must not survive into a new ring-3 process here either. Currently
     * a no-op in practice (slots are never recycled today), but the
     * invariant this function's own precedents document must hold
     * unconditionally, not "except here". */
    TASK_UTEST_CAPTURE_RESET(&tasks[pid]);
    /* Same reasoning one line up, for the loader evidence: a ring-3 task
     * created here is never a launcher child, so it must present no
     * loader verdict at all rather than a recycled slot's. */
    TASK_UTEST_LOADER_RESET(&tasks[pid]);
    task_init_accounting(&tasks[pid]);
    task_rlimit_inherit(&tasks[pid], &tasks[current_task]); /* inherit creator's limits */

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
    tasks[pid].syscall_filter = (struct syscall_filter *)0;  /* no filter; clear stale tenant ptr on slot reuse */
    tasks[pid].syscall_filter_counted = 0;
    tasks[pid].mitigation_flags = 0;  /* fresh: no mitigation policy; clear stale bits on slot reuse */
    tasks[pid].search_path_mode = 0;  /* unset -> default safe SearchPathW ordering; clear stale bits on slot reuse */
    tasks[pid].pledge_mask = 0;       /* not pledged; clear stale bits on slot reuse */
    fault_site_reset_task(&tasks[pid]);  /* no stale fault-site arm on slot reuse */
    tasks[pid].unveil_list = (struct unveil_entry *)0;  /* full FS visible; clear stale tenant list ptr */
    tasks[pid].unveil_locked = 0;
    tasks[pid].unveil_active = 0;
    tasks[pid].unveil_gen = 0;
    /* tasks[pid].job is set by the early ob_job_fork_inherit above (creator's
     * job or NULL) -- do NOT clear it here or the inheritance would be lost. */
    tasks[pid].threads[0].pledge_pending = 0;

    /* Thread 0 = main thread (uses task's kernel stack) */
    tasks[pid].threads[0].id = 0;
    tasks[pid].threads[0].state = THREAD_READY;
    tasks[pid].threads[0].stack_base = (uint8_t *)0;  /* shares task stack */
    tasks[pid].threads[0].stack_size = 0;
    tasks[pid].threads[0].parent_task = pid;
    tasks[pid].threads[0].join_tid = -1;
    tasks[pid].threads[0].priority      = THREAD_PRIO_NORMAL;
    tasks[pid].threads[0].base_priority = THREAD_PRIO_NORMAL;
    tasks[pid].threads[0].previous_mode = 0;  /* KernelMode on slot reuse */
    fault_site_reset_thread(&tasks[pid].threads[0]);  /* no stale site marker */
    tasks[pid].threads[0].in_system_service = 0;  /* no syscall in flight on a reused slot */
#ifdef KERNEL_TESTS
    thread_utest_cap_reset(&tasks[pid].threads[0]);  /* no open capture write on a reused slot */
#endif
    tasks[pid].threads[0].impersonation_token = (void *)0;  /* no stale impersonation on slot reuse */
    tasks[pid].threads[0].kernel_exception_list = (struct ki_exception_registration *)0;  /* no stale KI_TRY chain */
    tasks[pid].threads[0].teb = (void *)0;
    tasks[pid].threads[0].kernel_gs_base = 0;
    /* Reset the main thread's APC state (sets apc_state.process; matches the
     * secondary-thread create paths). Fresh task slot, but keep the contract
     * uniform: every THREAD_READY thread-0 has initialized APC state. */
    {
        uint64_t af;
        spin_lock_irqsave(&tasks[pid].threads[0].apc_lock, &af);
        apc_thread_init(&tasks[pid].threads[0].apc_state, &tasks[pid]);
        tasks[pid].threads[0].kernel_apc_disable  = 0;
        tasks[pid].threads[0].special_apc_disable = 0;
        spin_unlock_irqrestore(&tasks[pid].threads[0].apc_lock, af);
    }
    tasks[pid].num_threads = 1;
    /* Primary token slot: explicit assignment before num_tasks++ (never publish
     * a token-less user task) and stale-slot reset; the slot owns the dup ref. */
    tasks[pid].token = inherited_token;
    signal_init_task(&tasks[pid].signals);
    ob_handle_table_init(&tasks[pid].handle_table);
    /* Inherit the creator's process group + session AND publish the child in one
     * job-control critical section: a concurrent setsid group-reuse scan then
     * either sees this child (and rejects) or does not (child not yet a member) --
     * never a half-published cross-session membership. */
    {
        uint64_t jf = pgroup_jobctl_lock();
        tasks[pid].pgid = tasks[current_task].pgid;
        tasks[pid].sid  = tasks[current_task].sid;
        tasks[pid].has_execed = 0;
        num_tasks++;
        pgroup_jobctl_unlock(jf);
    }

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

/* IBPB on a cross-process switch involving a user-capable task (TODO-10 S8).
 * Flushes the indirect branch predictor so one process cannot leave branches the
 * next mis-speculates. STRUCTURAL user-capable test (own cr3 != kernel PML4 /
 * user stack), NOT "NULL token == user" -- kernel tasks (cr3=0, NULL token) must
 * not IBPB on every kthread switch. Same-task (same process) thread switches and
 * pure kernel-task->kernel-task switches take no IBPB. No-op without CPU_FEATURE_IBPB. */
static void sched_cross_domain_ibpb(uint32_t prev_task, uint32_t next_task)
{
    if (prev_task == next_task)
        return;
    int prev_user = tasks[prev_task].cr3 != 0 ||
                    tasks[prev_task].user_stack_base != NULL;
    int next_user = tasks[next_task].cr3 != 0 ||
                    tasks[next_task].user_stack_base != NULL;
    if (prev_user || next_user)
        cpu_issue_ibpb();
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

    /* Voluntary switch (process accounting): prev_task reached schedule_now() via
     * an explicit yield()/block, not a tick preempt -- so this is a voluntary
     * context switch out. */
    __atomic_fetch_add(&tasks[prev_task].vol_ctxsw, 1ull, __ATOMIC_RELAXED);

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
     * (skip if exec_pending -- don't overwrite the exec'd frame).
     *
     * ACQUIRE, to actually implement the pairing task_exec's publication
     * documents. This is the gate that protects tasks[prev_task].rsp, and
     * observing a stale zero here is the worst outcome in the protocol: the
     * pre-exec syscall frame overwrites the freshly published exec frame and the
     * task resumes at its old RIP over an image that no longer exists -- the
     * exact failure that survived 297 commits. The GS gate further down was
     * already atomic for the same reason; a plain read here left the more
     * damaging half of the same protocol unpaired. */
    if (!__atomic_load_n(&tasks[prev_task].exec_pending, __ATOMIC_ACQUIRE)) {
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

    /* exec_pending stuck detection: if set for too long, frame was never consumed.
     * Read ONCE and reused for the adoption evidence below: a non-zero value
     * here means this switch-in is the moment the frame task_exec published
     * gets consumed, which is the only thing the scheduler can honestly
     * witness about a task's passage into ring 3. */
    {
        /* ACQUIRE, pairing with the RELEASE store in task_exec, and
         * matching the save-gate's load earlier in this function. A plain
         * read here could observe a stale zero, clear exec_pending without
         * marking, and report a genuinely adopted frame as never-ran. */
        uint32_t adopting = __atomic_load_n(&tasks[next_task].exec_pending,
                                            __ATOMIC_ACQUIRE);

        if (adopting &&
            tasks[next_task].exec_pending_tick &&
            (uptime() - tasks[next_task].exec_pending_tick) > EXEC_PENDING_STUCK_TICKS) {
            klog(LOG_WARN, "sched", "PID %u: exec_pending stuck for %u ticks -- force-clearing",
                 (uint64_t)next_task,
                 (uint64_t)(uptime() - tasks[next_task].exec_pending_tick));
        }
        /* Marked whenever a publication is CONSUMED here, deliberately
         * without a next_thread == 0 gate.
         *
         * Such a gate is locally more precise -- the switch-in returns
         * threads[next_thread].rsp for a secondary thread and
         * tasks[next_task].rsp only for thread 0 -- but it is wrong
         * overall, because the clear below is unconditional. Gating only
         * the mark means a sibling switch-in consumes the flag while
         * skipping the mark, and the later thread-0 switch-in that really
         * does run the exec frame finds nothing left to mark: a binary
         * that RAN would then be reported as never-ran, hiding a genuine
         * failure. Erring the other way (marking a publication whose
         * frame a sibling switch-in swallowed) pushes the ambiguous case
         * toward FAIL, which is the direction u_record_verdict is
         * explicitly built to prefer.
         *
         * For the launcher's own children the two readings coincide:
         * task_create_captured makes a single-threaded task and the
         * loader execs itself, so next_thread is always 0 at this point.
         * The divergence is only reachable for a multi-threaded exec,
         * whose clear-versus-consume semantics are a scheduler design
         * question filed rather than settled here -> see
         * the per-CPU run-queue work in 03-memory-concurrency/TODO-07. */
        if (adopting)
            TASK_UTEST_NOTE_FRAME_ADOPTED(&tasks[next_task]);
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

    /* Spectre v2 IBPB (TODO-10 S8): flush the branch predictor before entering a
     * different process domain (gated on user-capable structural check). */
    sched_cross_domain_ibpb(prev_task, next_task);

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
        /* ACQUIRE the TEB publication BEFORE loading the GS slot. task_exec
         * release-stores `teb` AFTER `kernel_gs_base` (see the publication
         * window there), so this order guarantees that a reader observing a
         * non-NULL TEB also observes the matching gs_base.
         *
         * The previous order -- gs_base first, TEB tested later -- could not be
         * fixed by the writer alone: cpu B read gs_base==0, cpu A then stored
         * gs_base and released the TEB, and cpu B went on to observe the fresh
         * TEB and fatal on its STALE zero snapshot. A release store cannot
         * retroactively order a load that already happened, so the ordering
         * must be fixed on BOTH sides. Both switch paths use this order and
         * are kept textually identical for that reason. */
        void *next_thread_teb = __atomic_load_n(
            &tasks[next_task].threads[next_thread].teb, __ATOMIC_ACQUIRE);
        void *next_task_teb = __atomic_load_n(
            &tasks[next_task].teb, __ATOMIC_ACQUIRE);
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
        } else if (next_task_teb || next_thread_teb) {
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
                 (uint64_t)(uintptr_t)next_task_teb,
                 (uint64_t)(uintptr_t)next_thread_teb);
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

    /* Statistical tick accounting (process accounting): charge one tick quantum
     * to the running task's user_time_ns (interrupted in ring 3) or
     * kernel_time_ns (ring 0). Runs on EVERY tick -- before the single-task and
     * quantum early-returns below -- so even a lone CPU-bound task accrues time.
     * The quantum is the nominal tick period (NSEC_PER_SEC / live tick freq),
     * re-read each tick so a KeSetTimerResolution rate change is tracked; the
     * freq==0 guard skips charging before the timer is up. RELAXED: an
     * independent monotonic counter, single-writer on the BSP tick path today.
     *
     * DELIBERATELY NOT GATED ON sched_enabled. That flag means "preemption is
     * allowed", not "no task is running": scheduler_disable() wraps ordinary
     * runtime critical sections -- RCU read-side sections and the compositor's
     * whole compose/swap path -- and time spent inside them is still time the
     * current task consumed. Charging only when preemption happened to be
     * enabled produced a systematic under-count for exactly the kernel-heavy
     * work CPU accounting exists to see, and would let a future rate or health
     * policy be evaded by doing the work inside a preemption-disabled region.
     * The gate below still returns early without switching; only the
     * ACCOUNTING is unconditional. */
    {
        uint32_t hz = system_get_freq();
        if (hz && current_task < TASK_MAX) {
            uint64_t quantum_ns = NSEC_PER_SEC / hz;
            struct task *cur = &tasks[current_task];
            if ((frame->cs & 3) == 3)
                __atomic_fetch_add(&cur->user_time_ns, quantum_ns, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&cur->kernel_time_ns, quantum_ns, __ATOMIC_RELAXED);
        }
    }

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

    /* Involuntary switch (process accounting): prev_task is being preempted by
     * the periodic tick, not yielding -- the reason is the entry path itself
     * (schedule() = tick ISR, schedule_now() = cooperative yield). */
    __atomic_fetch_add(&tasks[prev_task].invol_ctxsw, 1ull, __ATOMIC_RELAXED);

    /* Save current task/thread's interrupt frame pointer
     * (skip if exec_pending -- don't overwrite the exec'd frame).
     * ACQUIRE for the same reason as the schedule_now() twin above: this gate
     * guards tasks[prev_task].rsp, and a stale zero here republishes the
     * pre-exec frame over the exec frame. */
    if (!__atomic_load_n(&tasks[prev_task].exec_pending, __ATOMIC_ACQUIRE)) {
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

    /* exec_pending stuck detection + adoption evidence (same as yield path) */
    {
        /* ACQUIRE for the same reason as the yield path above. */
        uint32_t adopting = __atomic_load_n(&tasks[next_task].exec_pending,
                                            __ATOMIC_ACQUIRE);

        if (adopting &&
            tasks[next_task].exec_pending_tick &&
            (uptime() - tasks[next_task].exec_pending_tick) > EXEC_PENDING_STUCK_TICKS) {
            klog(LOG_WARN, "sched", "PID %u: exec_pending stuck for %u ticks -- force-clearing",
                 (uint64_t)next_task,
                 (uint64_t)(uptime() - tasks[next_task].exec_pending_tick));
        }
        /* Marked whenever a publication is CONSUMED here, deliberately
         * without a next_thread == 0 gate.
         *
         * Such a gate is locally more precise -- the switch-in returns
         * threads[next_thread].rsp for a secondary thread and
         * tasks[next_task].rsp only for thread 0 -- but it is wrong
         * overall, because the clear below is unconditional. Gating only
         * the mark means a sibling switch-in consumes the flag while
         * skipping the mark, and the later thread-0 switch-in that really
         * does run the exec frame finds nothing left to mark: a binary
         * that RAN would then be reported as never-ran, hiding a genuine
         * failure. Erring the other way (marking a publication whose
         * frame a sibling switch-in swallowed) pushes the ambiguous case
         * toward FAIL, which is the direction u_record_verdict is
         * explicitly built to prefer.
         *
         * For the launcher's own children the two readings coincide:
         * task_create_captured makes a single-threaded task and the
         * loader execs itself, so next_thread is always 0 at this point.
         * The divergence is only reachable for a multi-threaded exec,
         * whose clear-versus-consume semantics are a scheduler design
         * question filed rather than settled here -> see
         * the per-CPU run-queue work in 03-memory-concurrency/TODO-07. */
        if (adopting)
            TASK_UTEST_NOTE_FRAME_ADOPTED(&tasks[next_task]);
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

    /* Spectre v2 IBPB (TODO-10 S8): flush the branch predictor before entering a
     * different process domain (gated on user-capable structural check). */
    sched_cross_domain_ibpb(prev_task, next_task);

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
        /* ACQUIRE the TEB publication BEFORE loading the GS slot. task_exec
         * release-stores `teb` AFTER `kernel_gs_base` (see the publication
         * window there), so this order guarantees that a reader observing a
         * non-NULL TEB also observes the matching gs_base.
         *
         * The previous order -- gs_base first, TEB tested later -- could not be
         * fixed by the writer alone: cpu B read gs_base==0, cpu A then stored
         * gs_base and released the TEB, and cpu B went on to observe the fresh
         * TEB and fatal on its STALE zero snapshot. A release store cannot
         * retroactively order a load that already happened, so the ordering
         * must be fixed on BOTH sides. Both switch paths use this order and
         * are kept textually identical for that reason. */
        void *next_thread_teb = __atomic_load_n(
            &tasks[next_task].threads[next_thread].teb, __ATOMIC_ACQUIRE);
        void *next_task_teb = __atomic_load_n(
            &tasks[next_task].teb, __ATOMIC_ACQUIRE);
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
        } else if (next_task_teb || next_thread_teb) {
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
                 (uint64_t)(uintptr_t)next_task_teb,
                 (uint64_t)(uintptr_t)next_thread_teb);
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

/* Set once PID 0 owns the SYSTEM primary token. After this point every task is
 * a descendant of a tokened process, so a NULL parent token at fork/create time
 * is an error (tokenless publication == authorization gap) and fails closed.
 * Before it (early kernel-thread bring-up) a NULL token is expected and
 * tolerated -- those threads run KernelMode, which bypasses token checks. */
static int g_primary_tokens_active = 0;

/* Duplicate the creating task's primary token for a new child slot, enforcing
 * the fail-closed policy. Returns 1 with *out set to a fresh Ob-owned primary
 * token (the child owns the reference); 0 with *out = NULL when there is no
 * token to inherit yet (pre-init only); -1 when inheritance must fail closed
 * (parent tokenless after SRM init, or duplication failed). */
static int task_inherit_primary_token(uint32_t parent_pid, ACCESS_TOKEN **out)
{
    ACCESS_TOKEN *pt = (ACCESS_TOKEN *)tasks[parent_pid].token;

    *out = (ACCESS_TOKEN *)0;
    if (pt) {
        ACCESS_TOKEN *dup = NtDuplicateToken(pt, 0, 0, TokenPrimary);
        if (!dup)
            return -1;      /* dup failure -> fail closed */
        *out = dup;
        return 1;
    }
    if (__atomic_load_n(&g_primary_tokens_active, __ATOMIC_ACQUIRE))
        return -1;          /* tokenless parent after init -> fail closed */
    return 0;               /* pre-init: NULL token tolerated */
}

void task_assign_initial_token(void)
{
    /* PID 0 (the initial system process) must own the SYSTEM primary token
     * before any later kernel task is created: children inherit their parent's
     * token at fork/create time, so a token-less PID 0 would leave every
     * descendant token-less (an authorization hole). Called from boot_phase3
     * right after task_init() -- ObpTokenType is registered by ob_init in the
     * prior phase, and no DPC/kworker task has been spawned yet. Idempotent. */
    if (tasks[0].token)
        return;
    tasks[0].token = SeCreateSystemToken();
    if (!tasks[0].token) {
        /* No SYSTEM token means no security baseline: every descendant would be
         * tokenless. This is unreachable in practice (heap is up, the token is
         * small) -- treat an actual failure as fatal rather than boot into a
         * broken authorization model. */
        boot_halt("SRM: PID 0 SYSTEM token creation failed");
    }
    /* PID 0 is built by task_init, not task_create, so it never passes through
     * the creation-path quota wiring. Attach its blocks here, as soon as it
     * has the token whose SID owns them -- otherwise the initial system
     * process would be the one process in the system charging nothing. */
    if (quota_task_init(&tasks[0], (struct access_token *)tasks[0].token) != STATUS_SUCCESS)
        boot_halt("quota: PID 0 quota block creation failed");
    /* Release-store so a concurrent task_inherit_primary_token on another CPU
     * that acquire-loads the flag also observes tasks[0].token (set above). */
    __atomic_store_n(&g_primary_tokens_active, 1, __ATOMIC_RELEASE);
    klog(LOG_INFO, "security", "Token assigned to pid=0 (SYSTEM)");
}

struct task *task_current(void)
{
    return &tasks[current_task];
}

void task_get_cwd(struct task *t, char *out, uint32_t out_size)
{
    uint64_t flags;
    uint32_t i;

    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!t)
        return;

    spin_lock_irqsave(&t->cwd_lock, &flags);
    for (i = 0; i + 1 < out_size && t->cwd[i]; i++)
        out[i] = t->cwd[i];
    out[i] = '\0';
    spin_unlock_irqrestore(&t->cwd_lock, flags);
}

int task_set_cwd(struct task *t, const char *abs)
{
    uint64_t flags;
    uint32_t len = 0;
    int ret = 0;

    if (!t || !abs)
        return -1;
    while (abs[len])
        len++;
    if (len >= TASK_CWD_MAX)     /* would truncate -- reject, leave cwd unchanged */
        return -1;

    spin_lock_irqsave(&t->cwd_lock, &flags);
    {
        uint32_t i;
        for (i = 0; i < len; i++)
            t->cwd[i] = abs[i];
        t->cwd[len] = '\0';
    }
    spin_unlock_irqrestore(&t->cwd_lock, flags);
    return ret;
}

int task_resolve_path_for(struct task *t, const char *in, char *out,
                          uint32_t out_size)
{
    char cwd[TASK_CWD_MAX];

    /* Drive-qualified input "X:..." -- three shapes, distinguished by the byte
     * after the colon:
     *   "X:\tail" / "X:/tail"  -> ABSOLUTE: resolve from the drive root; cwd is
     *                             irrelevant, so resolve WITHOUT taking cwd_lock
     *                             (concurrent absolute-path opens are a hot
     *                             syscall path and must not serialize on the
     *                             process-wide lock).
     *   "X:tail" / "X:"        -> DRIVE-RELATIVE: resolve `tail` against the
     *                             directory REMEMBERED for drive X. Windows keeps
     *                             that per-drive cwd in the hidden "=X:" env
     *                             variable (TODO-22 s12). When X is the process's
     *                             CURRENT drive, the live task cwd is authoritative
     *                             (and lock-free); otherwise the "=X:" value, or
     *                             the drive root "X:\" when the drive is unset.
     * A non-letter drive falls through to vfs_resolve_path, which rejects it. */
    if (in && in[0] && in[1] == ':') {
        char c0 = in[0];
        int is_letter = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
        char c2 = in[2];
        if (!is_letter || c2 == '\\' || c2 == '/')
            return vfs_resolve_path("C:\\", in, out, out_size);

        /* Drive-relative "X:tail" whose TAIL is ITSELF drive-qualified ("D:C:\..")
         * is malformed: vfs_resolve_path would take the drive from the tail and
         * silently cross to that other volume (a cross-drive delete/overwrite via
         * NtDeleteFile). Fail closed. c2 is a non-separator here; read in[3] only
         * when c2 is a letter (so in[2] is non-NUL and in[3] is in bounds). */
        if (((c2 >= 'A' && c2 <= 'Z') || (c2 >= 'a' && c2 <= 'z')) && in[3] == ':')
            return -1;

        {
            char drive = (c0 >= 'a' && c0 <= 'z') ? (char)(c0 - 32) : c0;
            char base[TASK_CWD_MAX];
            char cdrv;
            int glen;

            task_get_cwd(t, cwd, sizeof(cwd));
            cdrv = (cwd[0] >= 'a' && cwd[0] <= 'z') ? (char)(cwd[0] - 32) : cwd[0];
            if (cwd[0] && cwd[1] == ':' && cdrv == drive)
                return vfs_resolve_path(cwd, &in[2], out, out_size);  /* current drive */

            /* Other drive: the remembered "=X:" directory. env_get_drive_cwd
             * fills the "X:\" root on a genuinely UNSET drive (returns 3, base =
             * "X:\") -- that flows through normally and correctly resolves from the
             * root. But a "=X:" set directly via env_set / a custom CreateProcess
             * block may be up to ENV_VALUE_MAX bytes -- far past TASK_CWD_MAX -- in
             * which case the getter TRUNCATES into `base` and returns the FULL
             * length. Such a value is PRESENT but unrepresentable: falling back to
             * the root would silently RETARGET the operation to "X:\tail" (a wrong,
             * possibly destructive path), so FAIL CLOSED instead (adversarial
             * re-review). glen < 0 (bad arg) is likewise a hard failure. */
            glen = env_get_drive_cwd(t, drive, base, sizeof(base));
            if (glen < 0 || (uint32_t)glen >= sizeof(base))
                return -1;   /* unrepresentable remembered dir -> reject, never retarget */
            return vfs_resolve_path(base, &in[2], out, out_size);
        }
    }

    task_get_cwd(t, cwd, sizeof(cwd));
    if (!cwd[0]) {               /* defensive: a task with no cwd resolves from root */
        cwd[0] = 'C'; cwd[1] = ':'; cwd[2] = '\\'; cwd[3] = '\0';
    }
    return vfs_resolve_path(cwd, in, out, out_size);
}

int task_resolve_path(const char *in, char *out, uint32_t out_size)
{
    return task_resolve_path_for(task_current(), in, out, out_size);
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
 * Process accounting metrics (CPU / I/O / wakeup / timer)
 *
 * The counting seams declared in task.h. Every one is a lock-free RELAXED
 * atomic add on a per-task counter: no lock, no allocation, no logging, so all
 * of them are safe in interrupt context and beneath a caller's spinlock. That
 * is a REQUIREMENT rather than an optimization -- a wait grant is counted while
 * the waking primitive still holds its own lock, and the quota subsystem's
 * chain-charge path may not be entered there at all (quota.h forbids nested
 * entry and its owner-side discovery takes three irqsave sections per charge).
 * Metric recording therefore enters ZERO quota block critical sections and does
 * not participate in the QUOTA_BUDGET_* charge-path contract.
 * ========================================================================== */

void task_acct_note_read_io(struct task *t, uint64_t bytes)
{
    if (!t || !bytes)
        return;
    __atomic_fetch_add(&t->io_read_count, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->io_read_bytes, bytes, __ATOMIC_RELAXED);
}

void task_acct_note_write_io(struct task *t, uint64_t bytes)
{
    if (!t || !bytes)
        return;
    __atomic_fetch_add(&t->io_write_count, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->io_write_bytes, bytes, __ATOMIC_RELAXED);
}

void task_acct_note_control_io(struct task *t, uint64_t bytes)
{
    if (!t)
        return;
    __atomic_fetch_add(&t->io_other_count, 1ull, __ATOMIC_RELAXED);
    if (bytes)
        __atomic_fetch_add(&t->io_other_bytes, bytes, __ATOMIC_RELAXED);
}

void task_acct_note_wakeup(struct task *t, uint32_t prev_state)
{
    /* ONLY a thread that was actually waiting counts. Re-readying an already
     * runnable thread satisfies no wait, and counting it would let a primitive
     * that wakes its whole queue report one wakeup per member per signal even
     * when a single member was blocked. */
    if (!t || prev_state != THREAD_BLOCKED)
        return;
    __atomic_fetch_add(&t->wakeup_count, 1ull, __ATOMIC_RELAXED);
}

void task_acct_note_timer_create(struct task *t)
{
    if (!t)
        return;
    __atomic_fetch_add(&t->timer_create_count, 1ull, __ATOMIC_RELAXED);
}

/* Relaxed load helper: every metric is an independent monotonic counter, so no
 * acquire is needed and none is implied to the caller. */
static uint64_t task_acct_load(const uint64_t *field)
{
    return __atomic_load_n(field, __ATOMIC_RELAXED);
}

void task_acct_sample(const struct task *t, task_acct_sample_t *out)
{
    if (!out)
        return;

    /* Zero FIRST so an unknown task yields an all-zero sample rather than
     * stack residue a caller would divide into a nonsense rate. */
    out->timestamp_ns      = 0;
    out->user_time_ns      = 0;
    out->kernel_time_ns    = 0;
    out->io_read_count     = 0;
    out->io_read_bytes     = 0;
    out->io_write_count    = 0;
    out->io_write_bytes    = 0;
    out->io_other_count    = 0;
    out->io_other_bytes    = 0;
    out->wakeup_count      = 0;
    out->timer_create_count = 0;
    if (!t)
        return;

    /* One timestamp for the whole sample: the consumer divides the difference
     * of two samples by the difference of their timestamps, so the stamp must
     * belong to this read and not to a later one. Taken BEFORE the counters so
     * an interrupted sample under-reports the rate rather than over-reports it
     * (a stamp taken afterwards would pair old counts with a newer clock). */
    out->timestamp_ns       = uptime_ns();
    out->user_time_ns       = task_acct_load(&t->user_time_ns);
    out->kernel_time_ns     = task_acct_load(&t->kernel_time_ns);
    out->io_read_count      = task_acct_load(&t->io_read_count);
    out->io_read_bytes      = task_acct_load(&t->io_read_bytes);
    out->io_write_count     = task_acct_load(&t->io_write_count);
    out->io_write_bytes     = task_acct_load(&t->io_write_bytes);
    out->io_other_count     = task_acct_load(&t->io_other_count);
    out->io_other_bytes     = task_acct_load(&t->io_other_bytes);
    out->wakeup_count       = task_acct_load(&t->wakeup_count);
    out->timer_create_count = task_acct_load(&t->timer_create_count);
}

void task_acct_capture_base(const struct task *t, struct task_acct_base *out)
{
    if (!out)
        return;

    out->user_time_ns   = 0;
    out->kernel_time_ns = 0;
    out->io_read_count  = 0;
    out->io_read_bytes  = 0;
    out->io_write_count = 0;
    out->io_write_bytes = 0;
    out->io_other_count = 0;
    out->io_other_bytes = 0;
    out->wakeup_count   = 0;
    out->timer_create_count = 0;
    if (!t)
        return;

    out->user_time_ns   = task_acct_load(&t->user_time_ns);
    out->kernel_time_ns = task_acct_load(&t->kernel_time_ns);
    out->io_read_count  = task_acct_load(&t->io_read_count);
    out->io_read_bytes  = task_acct_load(&t->io_read_bytes);
    out->io_write_count = task_acct_load(&t->io_write_count);
    out->io_write_bytes = task_acct_load(&t->io_write_bytes);
    out->io_other_count = task_acct_load(&t->io_other_count);
    out->io_other_bytes = task_acct_load(&t->io_other_bytes);
    out->wakeup_count   = task_acct_load(&t->wakeup_count);
    out->timer_create_count = task_acct_load(&t->timer_create_count);
}

/* Saturating subtract: a baseline can never legitimately exceed the live
 * counter (both come from the same monotonic field), so an inversion means the
 * counter was reset under a live baseline -- a slot reused without clearing the
 * membership. Reporting 0 there loses one member's contribution; wrapping would
 * add ~2^64 to a job's aggregate and make every derived rate meaningless. */
static uint64_t task_acct_sub_sat(uint64_t now, uint64_t base)
{
    return (now > base) ? (now - base) : 0ull;
}

void task_acct_delta_fields(const struct task_acct_base *now,
                            const struct task_acct_base *base,
                            struct task_acct_base *out)
{
    if (!out || !now)
        return;
    if (!base) {
        *out = *now;                     /* no baseline: the whole lifetime */
        return;
    }

    out->user_time_ns   = task_acct_sub_sat(now->user_time_ns,   base->user_time_ns);
    out->kernel_time_ns = task_acct_sub_sat(now->kernel_time_ns, base->kernel_time_ns);
    out->io_read_count  = task_acct_sub_sat(now->io_read_count,  base->io_read_count);
    out->io_read_bytes  = task_acct_sub_sat(now->io_read_bytes,  base->io_read_bytes);
    out->io_write_count = task_acct_sub_sat(now->io_write_count, base->io_write_count);
    out->io_write_bytes = task_acct_sub_sat(now->io_write_bytes, base->io_write_bytes);
    out->io_other_count = task_acct_sub_sat(now->io_other_count, base->io_other_count);
    out->io_other_bytes = task_acct_sub_sat(now->io_other_bytes, base->io_other_bytes);
    out->wakeup_count   = task_acct_sub_sat(now->wakeup_count,   base->wakeup_count);
    out->timer_create_count =
        task_acct_sub_sat(now->timer_create_count, base->timer_create_count);
}

void task_acct_delta_since(const struct task *t, const struct task_acct_base *base,
                           struct task_acct_base *out)
{
    struct task_acct_base now;

    if (!out)
        return;
    task_acct_capture_base(t, &now);
    task_acct_delta_fields(&now, base, out);
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
    struct syscall_filter *inherited_filter = (struct syscall_filter *)0;
    ACCESS_TOKEN *inherited_token = (ACCESS_TOKEN *)0;
    uint64_t parent_mit;

    /* Single parent mitigation snapshot used for BOTH the NO_CHILD reject and
     * child inheritance, so an in-flight parent policy change linearizes at
     * this one acquire load (the child either fully predates or fully postdates
     * it). MIT_NO_CHILD_PROCESS: a fork is child creation, so a parent that
     * pledged no children cannot fork. The reject sits BEFORE the capacity
     * check so a policy-blocked fork returns silently in every capacity state
     * (never reaching the TASK_MAX log path) -- and before any allocation. */
    parent_mit = __atomic_load_n(&tasks[parent_pid_val].mitigation_flags,
                                 __ATOMIC_ACQUIRE);
    if (parent_mit & MIT_NO_CHILD_PROCESS)
        return -1;  /* silent: a ring-3 loop retrying a blocked fork must not hammer the global klog lock */

    if (num_tasks >= TASK_MAX) {
        klog(LOG_ERROR, "sched", "task_fork: max tasks reached");
        return -1;
    }

    child_pid = num_tasks;

    /* Syscall filter: a fork is "become a copy of me", so the child MUST
     * inherit the parent's filter unconditionally -- a sandbox a child could
     * escape via fork is no sandbox (seccomp semantics). This differs from
     * NtCreateProcess, which launches a different image and gates inheritance
     * on SYSCALL_FILTER_INHERIT. Clone FIRST, before allocating the child
     * kernel/user stacks, so a clone-alloc failure fails the fork CLOSED
     * without leaking those stacks (and never runs an unsandboxed child).
     * Reading the parent snapshot locklessly is safe: it is immutable and
     * never freed while the parent lives. */
    {
        struct syscall_filter *pf = __atomic_load_n(
            &tasks[parent_pid_val].syscall_filter, __ATOMIC_ACQUIRE);
        if (pf) {
            inherited_filter = syscall_filter_clone(pf);
            if (!inherited_filter) {
                klog(LOG_ERROR, "sched",
                     "task_fork: filter clone failed; failing fork closed");
                return -1;
            }
        }
    }

    /* Primary token: a fork child must start with a deep copy of the parent's
     * primary token, assigned BEFORE the child is schedulable -- otherwise the
     * child could run token-less, an authorization hole. Duplicate FIRST (like
     * the filter clone) so a failure fails the fork CLOSED without leaking the
     * child stacks. The dup is Ob-allocated with refcount 1, owned by the
     * child's token slot once assigned; dereferenced on any later failure. A
     * tokenless parent after SRM init also fails closed (see the helper). */
    if (task_inherit_primary_token((uint32_t)parent_pid_val, &inherited_token) < 0) {
        klog(LOG_ERROR, "sched",
             "task_fork: primary-token inheritance failed; failing fork closed");
        if (inherited_filter)
            kfree(inherited_filter);
        return -1;
    }

    /* Inherit pledge_mask + a deep copy of the unveil set into the child BEFORE
     * any stack allocation, so an OOM clone fails the fork CLOSED with only the
     * token/filter to release (like the stack-alloc paths below) -- a fork child
     * must never escape the parent's restrictions by starting unrestricted.
     * Runs before num_tasks++ publishes the child. */
    if (pledge_unveil_inherit(&tasks[child_pid], &tasks[parent_pid_val]) != 0) {
        klog(LOG_ERROR, "sched",
             "task_fork: pledge/unveil inheritance OOM; failing fork closed");
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        if (inherited_filter)
            kfree(inherited_filter);
        return -1;
    }

    /* The child's PID must be set BEFORE job inheritance: ob_job_assign records
     * child->pid into the job's member array, and a fresh/reused slot still
     * holds a stale (often 0) pid until the TCB init block below. Recording PID
     * 0 would leave the real child unfindable on exit (ref leak) and let
     * NtTerminateJobObject resolve pid 0 and kill the system task. The pid is
     * just the slot index; the TCB init block re-assigns it idempotently.
     * state=TASK_READY is likewise set first so ob_job_assign's under-lock
     * liveness check does not see a reused slot's stale TASK_DEAD. */
    tasks[child_pid].pid = child_pid;
    tasks[child_pid].state = TASK_READY;
    tasks[child_pid].job_absorb.active = 0;
    tasks[child_pid].job_lock.flag = 0;   /* unlocked; guards t->job for assign/detach */
    /* Quota stale-slot reset BEFORE the job inherit: joining a job reads
     * task->quota under task->quota_lock, so a reused slot must not present a
     * prior tenant's freed block or a stale lock word (see task_create). */
    tasks[child_pid].quota = NULL;
    tasks[child_pid].quota_user = NULL;
    tasks[child_pid].quota_lock.flag = 0;
    /* Same reset the other two constructors perform, and this path needs it MOST.
     * The job inherit immediately below reaches ob_job_assign, which quiesces the
     * child's charge gate; a recycled slot that inherited the previous tenant's
     * SEALED gate would fail that quiesce, and ob_job_fork_inherit maps any
     * failure to -1 -- so EVERY fork by a job member into such a slot would be
     * refused outright. The stale ledger pointer is cleared for the same
     * slot-reuse reason, and is not ours to release (the previous tenant's reap
     * already dropped the task's reference). */
    atomic64_set(&tasks[child_pid].quota_gate,
                 QUOTA_GATE_PACK(QUOTA_GATE_OPEN, 0));
    tasks[child_pid].quota_ledger = NULL;
    tasks[child_pid].quota_policy_lock.flag = 0;
    quota_policy_reset(&tasks[child_pid]);  /* per-process, never inherited (see task_create) */
    fault_site_reset_task(&tasks[child_pid]);  /* arm is per-process, never inherited */
    /* Same rule for the harness self-report: the child gets a fresh
     * NONE, so a forked child's own UTEST_END is its FIRST submission
     * rather than a repeat of the parent's. */
    TASK_UTEST_REPORT_RESET(&tasks[child_pid]);
    /* Output capture starts from the same "not captured" baseline as
     * every constructor, then below (once child_pid's slot is otherwise
     * fully built) INHERITS the parent's ownership unchanged -- the
     * OPPOSITE of the self-report reset just above, because a captured
     * binary's descendant output still belongs to the same binary. */
    TASK_UTEST_CAPTURE_RESET(&tasks[child_pid]);
    /* Loader evidence follows the self-report, not the capture ownership:
     * the record describes ONE loader invocation, and a fork descendant
     * did not perform it. Inheriting it would let a child that never
     * loaded anything present its parent's reached_exec to the launcher. */
    TASK_UTEST_LOADER_RESET(&tasks[child_pid]);

    /* Inherit the parent's Job Object membership BEFORE num_tasks++ publishes
     * the child, so a fork can never be used to escape a job's active-process
     * limit or job-wide termination. Fails the fork CLOSED if the parent's job
     * is terminated or at capacity. Rolled back (ob_job_detach_task) on every
     * later fork-failure path, symmetric with pledge/unveil. */
    if (ob_job_fork_inherit(&tasks[child_pid], &tasks[parent_pid_val]) != 0) {
        klog(LOG_ERROR, "sched",
             "task_fork: job inheritance rejected (terminated/at-limit); "
             "failing fork closed");
        pledge_unveil_teardown(&tasks[child_pid]);
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        if (inherited_filter)
            kfree(inherited_filter);
        return -1;
    }

    /* The child gets its OWN process block but SHARES its parent's user block,
     * so forking cannot multiply a user's budget. Rolled back on every later
     * fork-failure path. */
    if (quota_task_init(&tasks[child_pid], inherited_token) != STATUS_SUCCESS) {
        klog(LOG_ERROR, "sched", "task_fork: quota block allocation failed");
        ob_job_detach_task(&tasks[child_pid]);
        pledge_unveil_teardown(&tasks[child_pid]);
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        if (inherited_filter)
            kfree(inherited_filter);
        return -1;
    }

    /* Allocate kernel stack for child */
    kstack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!kstack) {
        klog(LOG_ERROR, "sched", "task_fork: cannot allocate kernel stack");
        quota_task_teardown(&tasks[child_pid]);
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        if (inherited_filter)
            kfree(inherited_filter);
        ob_job_detach_task(&tasks[child_pid]);
        pledge_unveil_teardown(&tasks[child_pid]);
        return -1;
    }

    /* Allocate user stack for child */
    ustack = (uint8_t *)kmalloc(USER_STACK_SIZE);
    if (!ustack) {
        klog(LOG_ERROR, "sched", "task_fork: cannot allocate user stack");
        kfree(kstack);
        quota_task_teardown(&tasks[child_pid]);
        if (inherited_token)
            PsDereferencePrimaryToken(inherited_token);
        if (inherited_filter)
            kfree(inherited_filter);
        ob_job_detach_task(&tasks[child_pid]);
        pledge_unveil_teardown(&tasks[child_pid]);
        return -1;
    }

    /* Per-process page table: clone kernel PML4 + mark image + new user stack
     * as User. Without this the child inherits cr3=0 from struct-zero-init, the
     * scheduler falls back to kernel CR3 (which has no User bit on image pages
     * under the per-process PT regime added by KPTI prep), and the child's
     * first ring-3 instruction faults silently before touching any user code.
     * Mirror of the task_exec() PML4 bring-up, minus the destroy-old step
     * (child has no prior cr3). Bug surfaced by the user-mode process-lifecycle
     * test on QEMU WHPX 2026-04-21 (forks succeeded but children never ran).
     *
     * Placed BEFORE the num_tasks++ publication below, and FAILS THE FORK
     * CLOSED. Both properties are load-bearing. Until 2026-07-28 this ran
     * AFTER publication and, on allocation failure, left cr3 = 0 on a child
     * that had already been published TASK_READY -- a child with no isolated
     * address space, running on the parent's, which task_exec then had to
     * refuse to keep it from overwriting the parent's image. Rolling that back
     * post-publication is not possible safely (another CPU can already observe
     * the pid through task_count() or terminate it through its job
     * membership), so the allocation moved ahead of publication instead, where
     * the same unwind ladder every failure path above uses applies. The
     * inherited filter is NOT attached yet (that happens further down), so it
     * is released with kfree() here exactly as the two stack-failure paths do.
     *
     * The site annotation is what makes this branch addressable from ring 3:
     * the four pmm_alloc_frame() calls inside vmm_create_user_pml4() are not
     * separable from the rest of fork's allocation traffic by an ordinal
     * countdown, which lands on whichever allocation happens to be N-th. */
    {
        uintptr_t user_cr3;
        uint32_t  fs_saved = fault_site_enter(FAULT_SITE_FORK_CHILD_PML4);

        user_cr3 = vmm_create_user_pml4();
        fault_site_restore(fs_saved);

        if (!user_cr3) {
            klog(LOG_ERROR, "sched",
                 "task_fork: per-process PML4 allocation failed for child slot "
                 "%u; failing fork closed", (uint64_t)child_pid);
            kfree(ustack);
            kfree(kstack);
            quota_task_teardown(&tasks[child_pid]);
            if (inherited_token)
                PsDereferencePrimaryToken(inherited_token);
            if (inherited_filter)
                kfree(inherited_filter);
            ob_job_detach_task(&tasks[child_pid]);
            pledge_unveil_teardown(&tasks[child_pid]);
            return -1;
        }

        {
            uintptr_t addr;
            /* Resolve the parent's current image via the interrupt frame's
             * saved RIP -- parent was executing user code when it INT 0x80'd
             * into fork, so rip is inside the parent's loaded ELF. Mark every
             * image page User in the child's cr3. */
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
            /* Mark child's user stack as User. The stack is kmalloc'd so it is
             * 16-byte aligned but NOT page-aligned -- the byte
             * `ustack + USER_STACK_SIZE` (the stack TOP, where RSP starts) can
             * easily land in the next 4 KiB page not covered by the literal
             * range. Round DOWN the start and UP the end so every page the
             * stack touches gets its User bit set. Without this, the first
             * ring-3 push lands on a kernel-only page and #PFs silently before
             * any user code runs. Root cause of the 2026-04-21 test_process
             * fork-hang on WHPX + KVM. */
            {
                uintptr_t ustack_lo = (uintptr_t)ustack
                                      & ~(uintptr_t)0xFFFu;
                uintptr_t ustack_hi = ((uintptr_t)ustack + USER_STACK_SIZE
                                       + 0xFFFu) & ~(uintptr_t)0xFFFu;
                for (addr = ustack_lo; addr < ustack_hi; addr += 4096)
                    vmm_set_user_page(user_cr3, addr);
            }
        }
        tasks[child_pid].cr3 = user_cr3;
    }

    /* NOTE: fork does NOT copy the parent environment into the child here. A
     * naive env_copy in this window blocks on the parent's environ_lock (a
     * sleeping mutex) BETWEEN ob_job_fork_inherit (child joined the job) and
     * num_tasks++ (child published); a concurrent job termination during that
     * yield snapshots child_pid, finds it not-yet-published, skips it, and the
     * fork then publishes a live child in a terminated job. Race-safe env
     * inheritance across EVERY constructor (this fork; the shared task_create that
     * NtCreateProcess and boot/desktop launchers use; and task_create_user) needs
     * the job-membership/publication window made atomic w.r.t. termination -- owned
     * by the child-creation env_copy wiring (02-kernel-core/TODO-12 s7). SYS_EXEC
     * with envp==NULL still inherits whatever
     * environ the task holds (execv semantics); that path is correct
     * independent of this gap. */

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
    /* NOTE: job_absorb is deliberately NOT reset here. Like `job`, it is owned
     * by the earlier ob_job_fork_inherit call above, which may already have
     * recorded what the inherited job absorbed; clearing it here would strand
     * that amount in the job forever. It is zeroed in the pre-inherit block. */
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
    /* base_priority MUST be set alongside priority: it is the value
     * thread_restore_priority() reverts to after any mutex priority-inheritance
     * boost. A forked child left with base_priority=0 (zero-init slot) gets its
     * priority silently reset to 0 -- and thus starved -- by the first
     * mutex_unlock it performs (e.g. task_set_argv on the exec path). Mirror the
     * task_create_* init. */
    tasks[child_pid].threads[0].base_priority = THREAD_PRIO_NORMAL;
    tasks[child_pid].threads[0].previous_mode = 0;  /* KernelMode on slot reuse */
    fault_site_reset_thread(&tasks[child_pid].threads[0]);  /* no stale site marker */
    tasks[child_pid].threads[0].in_system_service = 0;  /* no syscall in flight on a reused slot */
#ifdef KERNEL_TESTS
    thread_utest_cap_reset(&tasks[child_pid].threads[0]);  /* no open capture write on a reused slot */
#endif
    tasks[child_pid].threads[0].kernel_exception_list =
        (struct ki_exception_registration *)0;  /* no stale KI_TRY chain */
    tasks[child_pid].threads[0].kernel_rsp = tasks[child_pid].kernel_rsp;
    tasks[child_pid].threads[0].rsp = (uint64_t)sp;
    /* Reset the child main thread's APC state (sets apc_state.process; the
     * child does NOT inherit the parent's APC queues). */
    {
        uint64_t af;
        spin_lock_irqsave(&tasks[child_pid].threads[0].apc_lock, &af);
        apc_thread_init(&tasks[child_pid].threads[0].apc_state, &tasks[child_pid]);
        tasks[child_pid].threads[0].kernel_apc_disable  = 0;
        tasks[child_pid].threads[0].special_apc_disable = 0;
        spin_unlock_irqrestore(&tasks[child_pid].threads[0].apc_lock, af);
    }
    tasks[child_pid].user_stack_base = ustack;
    tasks[child_pid].name = tasks[parent_pid_val].name;
    /* Inherit output-capture ownership unchanged: a fork()'d descendant's
     * writes belong to the same test binary as its parent, not to a new
     * owner of its own (the opposite of the self-report reset above --
     * see task_utest_capture_inherit in task.h). */
    TASK_UTEST_CAPTURE_INHERIT(&tasks[child_pid], &tasks[parent_pid_val]);
    /* Child inherits the parent's cwd (snapshot the parent under its lock). */
    {
        char parent_cwd[TASK_CWD_MAX];
        task_get_cwd(&tasks[parent_pid_val], parent_cwd, sizeof(parent_cwd));
        task_set_cwd(&tasks[child_pid], parent_cwd[0] ? parent_cwd : "C:\\");
    }
    tasks[child_pid].parent_pid = parent_pid_val;
    tasks[child_pid].exit_status = 0;
    tasks[child_pid].wait_pid = -1;
    tasks[child_pid].exec_pending = 0;
    /* NOT inherited: the parked stack belongs to the PARENT's address space and
     * the parent will reclaim it. A child that copied the pointer would free a
     * stack its parent is still scheduled on. */
    tasks[child_pid].stack_pending_free = (uint8_t *)0;
    /* The child shares the parent's loaded image until it execs. The PML4 is
     * already built above, so this can no longer be reached with cr3 == 0 --
     * that combination is now an invariant violation rather than an OOM
     * outcome, and task_exec still checks for it as a backstop. */
    tasks[child_pid].forked_shares_parent_image = 1;
    ob_handle_table_init(&tasks[child_pid].handle_table);
    /* Copy parent's KERNEL_GS_BASE (TEB address) -- will allocate
     * a new TEB for the child and update this field. */
    tasks[child_pid].kernel_gs_base = tasks[parent_pid_val].kernel_gs_base;
    /* Mirror into threads[0] for per-thread GS swap */
    tasks[child_pid].threads[0].teb = tasks[parent_pid_val].threads[0].teb;
    tasks[child_pid].threads[0].kernel_gs_base =
        tasks[parent_pid_val].threads[0].kernel_gs_base;

    /* Attach the inherited filter (and clear any stale slot pointer) BEFORE
     * num_tasks++ publishes the child to the scheduler. Otherwise a timer
     * preemption between publication and a later attach could run a fork child
     * with syscall_filter == NULL -- an unfiltered escape from a filtered
     * parent. Fail-closed inheritance must hold before the child is
     * schedulable. */
    tasks[child_pid].syscall_filter = (struct syscall_filter *)0;
    tasks[child_pid].syscall_filter_counted = 0;
    if (inherited_filter)
        syscall_filter_attach(&tasks[child_pid], inherited_filter);

    /* Primary token slot: explicit assignment doubles as the stale-slot reset
     * (a prior tenant's token pointer must never survive into a reused slot).
     * Set before num_tasks++ so the child is never published token-less. The
     * child now owns the dup's refcount. Fresh main thread must likewise not
     * inherit a prior tenant's impersonation token. */
    tasks[child_pid].token = inherited_token;
    tasks[child_pid].threads[0].impersonation_token = (void *)0;
    tasks[child_pid].threads[0].pledge_pending = 0;  /* no stale provenance into a fork child */

    /* Inherit the parent's mitigation policy from the single fork snapshot,
     * committed before num_tasks++ publishes the child so it can never run with
     * weaker mitigations than its parent (monotonic-restriction inheritance). */
    tasks[child_pid].mitigation_flags = parent_mit;

    /* SearchPathW ordering policy is NOT inherited: a fork child resets to the
     * safe default (0). Unlike the monotonic mitigation mask above, search_path_mode
     * can express an UNSAFE CWD-first ordering (BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE),
     * and inheriting that would silently downgrade the child's DLL-search security. */
    tasks[child_pid].search_path_mode = 0;

    /* Fresh accounting for the child: its own times/I/O/ctxsw start at zero and
     * CreateTime is stamped at fork (a fork child is a distinct process, not a
     * continuation of the parent's accounting). Before num_tasks++ publishes it. */
    task_init_accounting(&tasks[child_pid]);
    task_rlimit_inherit(&tasks[child_pid], &tasks[parent_pid_val]); /* rlimits inherited across fork */

    /* Inherit the parent's process group + session AND publish the child in ONE
     * job-control critical section: the child reads the parent's CURRENT (pgid,
     * sid) and becomes num_tasks-visible atomically, so a concurrent setsid
     * group-reuse scan can never straddle it (a fork child joins its parent's
     * group and has NOT exec'd -- the shell can still setpgid it before exec). */
    {
        uint64_t jf = pgroup_jobctl_lock();
        tasks[child_pid].pgid = tasks[parent_pid_val].pgid;
        tasks[child_pid].sid  = tasks[parent_pid_val].sid;
        tasks[child_pid].has_execed = 0;
        num_tasks++;
        pgroup_jobctl_unlock(jf);
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
    char cwd_snap[TASK_CWD_MAX];
    uintptr_t peb_phys, rtlpp_phys, env_phys;

    /* Snapshot the task's cwd so the PEB CurrentDirectory matches task->cwd
     * (kept coherent under cwd_lock; the two must never diverge). */
    if (pid < TASK_MAX)
        task_get_cwd(&tasks[pid], cwd_snap, sizeof(cwd_snap));
    else
        cwd_snap[0] = '\0';
    if (!cwd_snap[0]) {
        cwd_snap[0] = 'C'; cwd_snap[1] = ':'; cwd_snap[2] = '\\'; cwd_snap[3] = '\0';
    }
    PEB *peb;
    RTL_USER_PROCESS_PARAMETERS *pp;
    uint16_t *env;

    /* Allocate physical pages. Bracketed as FAULT_SITE_PEB_FRAMES so a
     * ring-3 test can fail THIS post-load allocation by name; an ordinal
     * PMM countdown cannot reach it reliably because the frame count on the
     * exec path ahead of it drifts with unrelated changes. */
    {
        uint32_t fs_saved = fault_site_enter(FAULT_SITE_PEB_FRAMES);
        peb_phys = pmm_alloc_frame();
        rtlpp_phys = pmm_alloc_frame();
        env_phys = pmm_alloc_frame();
        fault_site_restore(fs_saved);
    }
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

        /* CommandLine: encode from task->argv so GetCommandLineW reflects the
         * full argument vector; fall back to the image name when argv is unset.
         * peb_build_ustr does NOT bound-check, so the encode buffer is sized to
         * the RTLPP page's FULL remaining space (reserving room for
         * CurrentDirectory, built next) -- kmalloc'd, not a fixed kernel-stack
         * array (a large stack array risks the task-stack guard during boot).
         * argv_to_cmdline writes 1 ASCII byte per output wchar, so the byte cap
         * equals the wchar budget; the page budget is <= 2 KiB so kmalloc fits.
         * A command line exceeding the whole page still truncates cleanly + stays
         * NUL-terminated (best-effort GetCommandLineW; the stack argv/task->argv
         * are authoritative). */
        {
            const char *cl = name;
            char *cmdline = (char *)0;
            uint16_t *page_end = (uint16_t *)((uint8_t *)pp + 4096);
            uint32_t cwd_len = 0;
            uint32_t avail;                 /* wchars available for CommandLine */
            while (cwd_snap[cwd_len]) cwd_len++;
            avail = (page_end > buf) ? (uint32_t)(page_end - buf) : 0;
            avail = (avail > cwd_len + 2u) ? avail - (cwd_len + 2u) : 0;
            if (avail > 0 && pid < TASK_MAX &&
                tasks[pid].argv && tasks[pid].argc > 0) {
                cmdline = (char *)kmalloc(avail);   /* avail <= ~2 KiB */
                if (cmdline) {
                    argv_to_cmdline(tasks[pid].argc,
                                    (const char *const *)tasks[pid].argv,
                                    cmdline, avail);
                    cl = cmdline;
                }
            }
            if (cl && cl[0])
                peb_build_ustr(&pp->CommandLine, &buf, cl);
            if (cmdline)
                kfree(cmdline);
        }

        /* CurrentDirectory -- synced from task->cwd, not hardcoded */
        peb_build_ustr(&pp->CurrentDirectoryDosPath, &buf, cwd_snap);
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

/* SINGLE SOURCE OF TRUTH for the task_exec initial-stack auxv vector. `X(type,
 * value)` -- the value expressions reference frame-builder locals and are only
 * evaluated where the list is expanded for emission (EXEC_AUXV_ENTRIES(AUXV_EMIT)
 * in task_exec). EXEC_AUXV_PAIRS is derived by counting this SAME list, so adding
 * or removing an entry changes both the emitted frame AND the pair count -- they
 * cannot drift. The _Static_assert then ties ARGV_FRAME_RESERVE (the builder's
 * fixed overhead: AT_RANDOM 16 + auxv EXEC_AUXV_PAIRS*16 + envp NULL 8) to that
 * count, so touching the auxv list without reconciling the argv fit-check reserve
 * fails the BUILD. */
#define EXEC_AUXV_ENTRIES(X) \
    X(AT_PHDR,   phdr_vaddr)     \
    X(AT_PHENT,  phent)          \
    X(AT_PHNUM,  phnum)          \
    X(AT_PAGESZ, 4096)           \
    X(AT_BASE,   0)              \
    X(AT_FLAGS,  0)              \
    X(AT_ENTRY,  entry)          \
    X(AT_UID,    0)              \
    X(AT_EUID,   0)              \
    X(AT_GID,    0)              \
    X(AT_EGID,   0)              \
    X(AT_SECURE, 0)              \
    X(AT_RANDOM, at_random_addr) \
    X(AT_HWCAP,  hwcap)          \
    X(AT_HWCAP2, 0)              \
    X(AT_NULL,   0)
#define EXEC_AUXV_COUNT_ONE(t, v) + 1u
#define EXEC_AUXV_PAIRS (0u EXEC_AUXV_ENTRIES(EXEC_AUXV_COUNT_ONE))
_Static_assert(EXEC_AUXV_PAIRS * 16u + 24u == ARGV_FRAME_RESERVE,
               "ARGV_FRAME_RESERVE must equal the exec auxv fixed overhead "
               "(AT_RANDOM 16 + auxv EXEC_AUXV_PAIRS*16 + envp NULL 8)");

/* Free a task-level kernel stack, whichever allocator produced it.
 *
 * stack_base may have come from EITHER allocator depending on the task's
 * history:
 *   - task_create_user: kmalloc(TASK_STACK_SIZE) -- in heap
 *   - task_create:      pmm_alloc_contiguous(N+1) with guard page below
 *                       (stack_base = guard_base + 4096)
 *   - task_exec:        replaces with the same PMM+guard pattern
 * Calling kfree on a PMM pointer dereferences ptr - HEADER_SIZE as a struct
 * block_header (garbage), then walks coalesce_free_blocks which can corrupt
 * the heap free-list and hang; heap_owns() picks the right one. On the PMM
 * branch the guard MUST be uninstalled BEFORE the frames go back: the install
 * cleared the identity-map PTE for that VA, so handing the frame to PMM
 * without restoring the PTE faults the next allocator that writes it
 * (discovered 2026-04-20 via Boot Tests: #PF write at CR2=guard_phys, RIP in
 * zero_page).
 *
 * Extracted from task_cleanup so the exec stack-reclamation paths free by the
 * same rules -- a second copy of this logic is exactly how the 2026-04-20 bug
 * would come back. */
/* Guard-table capacity vs. what the task table can demand: every live task
 * holds one guard under its kernel stack, a task mid-exec holds a second under
 * the parked stack it has not released yet (task.stack_pending_free), and every
 * user thread holds one under its own per-thread kernel stack (uthread_create).
 * The remainder covers the fixed guards -- heap end, IST stacks, AP stacks,
 * user ELF range. Undersizing this is not a soft failure: installs start
 * refusing, and task_create/uthread_create refuse with them. */
_Static_assert(VMM_MAX_GUARD_PAGES >= (TASK_MAX * (THREAD_MAX + 2)) + 32,
               "guard table must hold every task and thread stack guard plus the fixed guards");

#ifdef KERNEL_TESTS
/* Test seam: the unit test must exercise THIS function, not a copy of its
 * arithmetic, or a divergence between the two is exactly what it fails to
 * catch. Test-build only -- the release flavor keeps the helper static. */
void task_test_free_kernel_stack(uint8_t *stack_base);
#endif

static int task_free_kernel_stack(uint8_t *stack_base)
{
    if (!stack_base)
        return 0;

    if (heap_owns(stack_base)) {
        kfree(stack_base);
        return 0;
    }

    {
        uintptr_t base = (uintptr_t)stack_base;
        uint32_t pages = (TASK_STACK_SIZE / 4096) + 1;  /* +1 for guard */
        uint32_t p;
        if (vmm_uninstall_guard_page(base - 4096) != 0) {
            /* The guard VA is not a confirmed identity mapping, so the frame
             * under it cannot be handed back: the next allocator to write
             * through its identity address would fault in kernel mode --
             * exactly the 2026-04-20 failure this helper exists to prevent.
             * Report failure so the CALLER keeps its pointer: uninstall retains
             * the registration on refusal precisely so a later drain can retry,
             * and a caller that cleared the pointer would strand both the run
             * and its guard slot for the life of the boot. */
            klog(LOG_ERROR, "sched",
                 "task: kernel stack at %p not released -- guard page %p not restored",
                 (void *)base, (void *)(base - 4096));
            return -1;
        }
        for (p = 0; p < pages; p++)
            pmm_free_frame((base - 4096) + (uintptr_t)p * 4096);
    }
    return 0;
}

#ifdef KERNEL_TESTS
void task_test_free_kernel_stack(uint8_t *stack_base)
{
    (void)task_free_kernel_stack(stack_base);
}
#endif

/* Abandon an exec that has already passed its commit point.
 *
 * Past the commit point the calling task's image is gone -- the fork+exec
 * isolation remap replaced its image-range PTEs with zeroed private frames and
 * the loader wrote a new binary over them. Returning -1 to the caller would
 * iretq the task back into an image that no longer exists, which is the defect
 * this exists to remove. So a post-commit failure terminates the task instead.
 *
 * task_exit() is the ONLY correct exit here, not task_death_teardown():
 * task_death_teardown only releases resources, leaving the task marked alive,
 * its waiters unsignalled and its abandoned syscall IRQL raised -- a live task
 * running with a destroyed image. task_exit does the whole transition (thread 0
 * DEAD under apc_lock, APC rundown, TASK_DEAD, teardown, parent signal, IRQL
 * lower) and never returns.
 *
 * The two resources freed first are the ones task_exec owns and has NOT yet
 * published into the task: the replacement kernel stack (nothing is running on
 * it -- the task is still on its old one) and the argv address table. Anything
 * already published is task_exit's to release. */
static int exec_commit_failure(uint32_t pid, const char *why,
                               uint8_t *new_kstack, uint64_t *argv_addrs)
{
    klog(LOG_ERROR, "sched",
         "exec: post-commit failure (%s), terminating PID %u",
         why, (uint64_t)pid);

    if (argv_addrs)
        kfree(argv_addrs);
    task_free_kernel_stack(new_kstack);

    return TASK_EXEC_IMAGE_DESTROYED;
}

/* Private frames the fork+exec isolation remap needs: one per page of the user
 * image range. The address table is 8 bytes per frame; pin that it stays inside
 * kmalloc's 4 KiB ceiling (CLAUDE.md: kmalloc for <= 4 KB only, pmm_alloc_
 * contiguous above that) so growing USER_ELF_* cannot silently start handing
 * kmalloc an over-size request. */
#define EXEC_PRIVATE_FRAMES ((uint32_t)((USER_ELF_END - USER_ELF_BASE) / 4096u))
_Static_assert((USER_ELF_END - USER_ELF_BASE) / 4096u * sizeof(uintptr_t) <= 4096u,
    "exec private-frame address table must fit kmalloc's 4 KiB ceiling");

/* The exec/thread-entry frame builders below write a 22-qword ring-3 iretq
 * frame through a raw uint64_t *sp, and task_exec_take_pending_frame() hands
 * that same memory back to the ISR stub as a struct interrupt_frame. Pin every
 * index those builders assign by name so a field reorder in idt.h cannot
 * silently retarget rip/rsp/cs/ss -- the failure mode is a ring-3 return to a
 * garbage RIP, which is exactly the class of bug this frame handoff caused on
 * 2026-07-27. sizeof + the individual offsets are asserted in idt.h; these tie
 * the INDICES used here to those offsets. */
_Static_assert(__builtin_offsetof(struct interrupt_frame, int_no)   == 15 * 8,
    "task.c frame builders write int_no at sp[15]");
_Static_assert(__builtin_offsetof(struct interrupt_frame, err_code) == 16 * 8,
    "task.c frame builders write err_code at sp[16]");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rip)      == 17 * 8,
    "task.c frame builders write rip at sp[17]");
_Static_assert(__builtin_offsetof(struct interrupt_frame, cs)       == 18 * 8,
    "task.c frame builders write cs at sp[18]");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rflags)   == 19 * 8,
    "task.c frame builders write rflags at sp[19]");
_Static_assert(__builtin_offsetof(struct interrupt_frame, rsp)      == 20 * 8,
    "task.c frame builders write rsp at sp[20]");
_Static_assert(__builtin_offsetof(struct interrupt_frame, ss)       == 21 * 8,
    "task.c frame builders write ss at sp[21]");

/* ---- Staging-buffer ownership token (see task.h for the contract) ------- */

void task_exec_staging_release(struct task_exec_staging *st)
{
    task_exec_release_fn fn;

    if (!st)
        return;
    fn = st->release;
    if (!fn)
        return;

    /* RUNTIME GUARD for the ordering this whole token exists to enforce.
     *
     * A real release must never happen after publication. Once exec_pending is
     * set the task can be carried into the new image at any moment, so a caller
     * that still owed a free might never run -- which is the leak the token was
     * introduced to close. The unit tests around this helper can only prove its
     * own semantics; they stay green if task_exec's call is deleted or moved
     * below the publication window. This check pins the production ordering
     * instead, and it is deterministic where it matters: with the call removed,
     * SYS_EXEC's own post-call release runs with exec_pending still set (INT
     * 0x80 keeps IF=0, so no tick can have consumed it) and this fires on every
     * exec. LOG_ERROR is surfaced by the test harness and by the smoke test's
     * log-cleanliness gate, so the regression cannot land quietly.
     *
     * Only reached when a release is actually owed: an already-consumed token
     * returned above, which is the normal state of the caller's post-call
     * release on the success path. */
    if (current_task < TASK_MAX &&
        __atomic_load_n(&tasks[current_task].exec_pending, __ATOMIC_ACQUIRE))
        klog(LOG_ERROR, "sched",
             "exec: staging release ran AFTER publication (PID %u) -- the "
             "caller may never run to free it", (uint64_t)current_task);

    /* Clear BEFORE invoking: that ordering is what makes this idempotent, so
     * task_exec's pre-publication release and the caller's own post-call
     * release can both run unconditionally and exactly one of them frees. */
    st->release = (task_exec_release_fn)0;
    fn(st);
}

void task_exec_staging_kfree(struct task_exec_staging *st)
{
    if (st && st->ptr) {
        kfree(st->ptr);
        st->ptr = (void *)0;
    }
}

int task_exec(const uint8_t *data, uint64_t size,
              struct task_exec_staging *staging)
{
    uint64_t entry;
    int exec_err = 0;
    uint64_t *sp;
    uint32_t pid = current_task;
    uint8_t *new_kstack;
    /* argv-frame resources are acquired BEFORE the guarded kernel stack (below)
     * so an argv OOM / over-budget rejection returns before any further
     * allocation and never leaks the guard stack. Populated only when the task
     * carries an argv (SYS_EXEC handoff); NULL for kernel-launched binaries. */
    char    **t_argv = (char **)0;
    int       t_argc = 0;
    uint64_t *argv_addrs = (uint64_t *)0;
    int       use_argv = 0;
    /* Private image frames for fork+exec isolation, acquired in full before
     * the remap so the remap itself cannot fail partway. NULL for launcher-
     * spawned tasks (cr3 == 0), which have no parent to isolate from. */
    uintptr_t *priv_frames = (uintptr_t *)0;
    /* New thread-0 TEB. Held here from its (fallible) allocation until the
     * publication window wires it into GS -- see the TEB block for why the
     * wiring cannot happen at allocation time. */
    TEB *new_teb = (TEB *)0;

    /* Exec must be called from the main thread (tid 0).  A secondary user
     * thread calling exec would leave stale threads[N].kernel_rsp in the
     * scheduler, causing TSS.rsp0 corruption on switch-in. */
    if (current_thread != 0) {
        klog(LOG_ERROR, "sched",
             "task_exec: rejected from thread %u (must be thread 0)",
             (uint64_t)current_thread);
        return -1;
    }

    /* ---- Pre-commit phase: every fallible step, before any mutation ----
     *
     * Everything below this comment and above the COMMIT POINT can fail and
     * return -1 safely, because the task's image is still intact at that
     * point. Everything after the commit point cannot: a failure there goes
     * through exec_commit_failure() and terminates the task.
     *
     * Reclaim the stack a PREVIOUS exec superseded. Reaching task_exec again
     * proves the task re-entered the kernel on the stack that exec installed,
     * so it is provably no longer on the parked one. Doing it here (rather
     * than at publication) is what bounds the outstanding stale stacks at one
     * per task instead of one per exec. */
    if (tasks[pid].stack_pending_free) {
        /* Hold the pointer when the release is REFUSED, so a later drain of
         * this same slot can retry (uninstall keeps the guard registration on
         * failure for exactly that). This is a best effort, not a guarantee:
         * a successful exec republishes the slot below and the retained run is
         * then lost. A durable owner across re-exec, rollback and reap is the
         * filed section-19 follow-up. */
        if (task_free_kernel_stack(tasks[pid].stack_pending_free) == 0)
            tasks[pid].stack_pending_free = (uint8_t *)0;
    }

    /* A fork child MUST have its own page table before its image is replaced.
     * Without one, the isolation remap below is skipped (it keys off cr3) and
     * exec_load writes the new binary through identity mappings still shared
     * with the parent, overwriting the parent's code. Since 2026-07-28
     * task_fork fails the fork CLOSED when vmm_create_user_pml4 hits OOM, so
     * FORK no longer produces this state and the check is a backstop there.
     * It is NOT dead: task_create_user still degrades to cr3 = 0 with a
     * LOG_WARN, and while that path clears forked_shares_parent_image (so it
     * does not reach THIS branch), the wider class of published tasks with no
     * isolated address space is not yet closed. Keeping the compare costs one
     * predictable branch on the exec path; what it guards against is one
     * process silently overwriting another's code. */
    if (tasks[pid].forked_shares_parent_image && !tasks[pid].cr3) {
        klog(LOG_ERROR, "sched",
             "task_exec: PID %u is a fork child with no isolated CR3 "
             "(fork-time PML4 OOM) -- refusing exec", (uint64_t)pid);
        return -1;
    }

    /* Acquire the argv-address table and validate the exact argv frame fits
     * the fixed user stack. When the task carries an argv it MUST be used or
     * the exec fails -- a name-path fallback would leave the stack argv
     * disagreeing with the PEB CommandLine (built from the full argv). Name
     * path (argc=1) applies only when argv is unset. argv is read here on
     * thread 0 with no concurrent task_set_argv, so no environ_lock is
     * needed. */
    t_argv = tasks[pid].argv;
    t_argc = tasks[pid].argc;
    if (t_argv && t_argc > 0) {
        uint32_t need = argv_frame_bytes(t_argc, (const char *const *)t_argv);
        if ((uint32_t)t_argc > ARG_ARGC_MAX ||
            (uint64_t)need + ARGV_FRAME_RESERVE > USER_STACK_SIZE) {
            klog(LOG_ERROR, "sched",
                 "task_exec: argv frame (%u B, argc %d) exceeds user stack",
                 need, t_argc);
            return -1;
        }
        {
            uint32_t fs_saved = fault_site_enter(FAULT_SITE_EXEC_ARGV_TABLE);
            argv_addrs = (uint64_t *)kmalloc((uint32_t)t_argc * sizeof(uint64_t));
            fault_site_restore(fs_saved);
        }
        if (!argv_addrs) {
            klog(LOG_ERROR, "sched",
                 "task_exec: OOM allocating argv address table (argc %d)", t_argc);
            return -1;
        }
        use_argv = 1;
    }

    /* Allocate a FRESH kernel stack with guard page -- we cannot reuse the
     * current one because the calling function (exec_loader_func) is still on
     * it. Allocated pre-commit so an OOM here returns into a task whose image
     * is still its own. */
    {
        uint32_t stack_pages = TASK_STACK_SIZE / 4096;
        uintptr_t stack_base = pmm_alloc_contiguous(stack_pages + 1);
        if (!stack_base) {
            klog(LOG_DEBUG, "sched", "Cannot allocate kernel stack");
            if (argv_addrs) kfree(argv_addrs);
            return -1;
        }
        int guard_rc = vmm_install_guard_page(stack_base,
                                              "GUARD: kernel task stack overflow");
        if (guard_rc != VMM_GUARD_OK) {
            /* Pre-commit refusal: the image is still the task's own, so this
             * returns -1 to a caller that can still use it. Failing here is
             * the point -- past the commit point there is no way to decline an
             * unguarded stack, and a task mid-exec is exactly the case that
             * holds two guards at once. */
            klog(LOG_ERROR, "sched",
                 "task_exec: no guard page available for replacement kernel stack");
            stack_run_release_after_guard_failure(stack_base, stack_pages + 1, guard_rc);
            if (argv_addrs) kfree(argv_addrs);
            return -1;
        }
        new_kstack = (uint8_t *)(stack_base + 4096);
    }

    /* Address table for the fork+exec private frames. Allocated pre-commit
     * with everything else that can fail. */
    if (tasks[pid].cr3) {
        {
            uint32_t fs_saved =
                fault_site_enter(FAULT_SITE_EXEC_PRIVATE_FRAMES);
            priv_frames = (uintptr_t *)kmalloc(EXEC_PRIVATE_FRAMES
                                               * sizeof(uintptr_t));
            fault_site_restore(fs_saved);
        }
        if (!priv_frames) {
            klog(LOG_ERROR, "sched",
                 "task_exec: OOM allocating private-frame table (%u frames)",
                 (uint64_t)EXEC_PRIVATE_FRAMES);
            task_free_kernel_stack(new_kstack);
            if (argv_addrs) kfree(argv_addrs);
            return -1;
        }
    }

    /* ================= COMMIT POINT =================
     *
     * The isolation remap below is the first IRREVERSIBLE mutation of the
     * process image: it replaces the child's image-range PTEs with freshly
     * zeroed private frames, and nothing undoes that. Past this line the task
     * cannot be returned to its caller -- every failure goes through
     * exec_commit_failure(). Keep new fallible work ABOVE this line.
     *
     * Fork+exec isolation (2026-04-21): a forked child inherits its
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
        uint32_t  i;

        /* Acquire and zero EVERY private frame before the first PTE moves, so
         * the remap below cannot fail partway.
         *
         * This used to allocate and remap page by page and, on OOM, break out
         * and carry on -- exec_load then wrote the new binary through the
         * identity mappings the child still SHARES with its fork parent,
         * overwriting the parent's code in place. The old comment argued
         * "refusing exec is worse", but corrupting another process's image is
         * not a degradation, it is a memory-safety failure a child can provoke
         * by exhausting memory. Preallocation is what makes failing closed
         * possible: an OOM here is still PRE-COMMIT, so it returns -1 into a
         * task whose own image is untouched. */
        for (i = 0; i < EXEC_PRIVATE_FRAMES; i++) {
            priv_frames[i] = pmm_alloc_frame();
            if (!priv_frames[i]) {
                uint32_t f;
                klog(LOG_ERROR, "sched",
                     "task_exec: OOM on private frame %u of %u -- refusing exec",
                     (uint64_t)i, (uint64_t)EXEC_PRIVATE_FRAMES);
                for (f = 0; f < i; f++)
                    pmm_free_frame(priv_frames[f]);
                kfree(priv_frames);
                task_free_kernel_stack(new_kstack);
                if (argv_addrs) kfree(argv_addrs);
                return -1;
            }
            /* Zero the frame so the new binary's uninitialized BSS doesn't
             * inherit whatever was in the frame before. pmm_alloc_frame
             * returns an identity-mapped phys, so writing via (uintptr_t)
             * phys is safe in kernel mode. */
            {
                uint64_t *z = (uint64_t *)priv_frames[i];
                uint32_t q;
                for (q = 0; q < 512; q++) z[q] = 0;
            }
        }

        /* ===== COMMIT POINT (fork+exec path): infallible from here ===== */
        i = 0;
        for (va = USER_ELF_BASE; va < USER_ELF_END; va += 4096, i++) {
            vmm_remap_user_page(tasks[pid].cr3, va, priv_frames[i]);
            vmm_flush_tlb(va);
        }
        kfree(priv_frames);
        priv_frames = (uintptr_t *)0;
        /* The image is now backed by this task's own frames -- it no longer
         * shares anything with its fork parent. */
        tasks[pid].forked_shares_parent_image = 0;
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
    /* Same reason, same moment: exec replaces the process IMAGE, and a
     * test report is per-image. The three task CONSTRUCTORS reset this,
     * but exec is the constructor-equivalent that was missed -- without
     * it a task that reported and then exec'd would make the new image's
     * UTEST_END read as a forbidden SECOND submission (sticky INVALID,
     * so the launcher fails a binary that did nothing wrong), and in the
     * other direction the pre-exec image's counts would be reconciled
     * against the post-exec image's exit status. */
    TASK_UTEST_REPORT_RESET(&tasks[pid]);
    const char *fmt_name = (const char *)0;
    entry = exec_load_fmt(data, size, &exec_err, &fmt_name);
    if (entry == 0) {
        /* Past the commit point: the isolation remap already replaced this
         * task's image pages with zeroed private frames, so there is nothing
         * left to return to. */
        klog(LOG_DEBUG, "sched", "exec_load failed (err=%u)", (uint64_t)exec_err);
        return exec_commit_failure(pid, "image load failed",
                                   new_kstack, argv_addrs);
    }
    tasks[pid].loaded_format = fmt_name;

    /* Mark the process as having exec'd (monotonic). Done under the job-control
     * lock (pgroup_note_exec) so it is mutually EXCLUSIVE with a concurrent
     * setpgid validation -- a parent can never move a child after its exec
     * completes; such a setpgid returns EACCES (POSIX). */
    pgroup_note_exec(&tasks[pid]);

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

            /* Name: prefer the EIF metadata "name" when the image carries one,
             * else the task name. eif_parse_metadata re-reads the already-
             * validated buffer (pure, no mutation); the image loaded, so the
             * re-parse cannot fail. This gives crash-registry / debugger module
             * lists the binary's declared name on the FIRST load. NOTE: a
             * subsequent re-exec is NOT renamed because exec_find_module_by_pc
             * matches the stale USER_ELF-range entry and skips re-registration
             * (all formats) -- tracked as a separate replace-on-re-exec item. */
            {
                const char *n = tasks[pid].name ? tasks[pid].name : "a.out";
                uint32_t ni = 0;
                if (mod.format == EXEC_FMT_EIF &&
                    size >= sizeof(eif_header_t)) {
                    eif_metadata_t emeta;
                    if (eif_parse_metadata(data, size,
                            (const eif_header_t *)data, &emeta) && emeta.name[0])
                        n = emeta.name;
                }
                /* Bound BEFORE the index: the reversed order read n[ni] at
                 * ni == EXEC_MODULE_NAME_MAX - 1 before deciding to stop. */
                while (ni < EXEC_MODULE_NAME_MAX - 1 && n[ni]) {
                    mod.name[ni] = n[ni];
                    ni++;
                }
                mod.name[ni] = 0;
            }

            exec_register_module((process_t *)0, &mod);
        }
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
            /* No per-process page table means no address-space isolation: the
             * new image would run on the kernel PML4. That is a security
             * failure, not a degradation, so it fails closed. Post-commit --
             * the old image is already gone -- so the task terminates. */
            klog(LOG_ERROR, "sched",
                 "task_exec: PML4 creation failed for PID %u", (uint64_t)pid);
            return exec_commit_failure(pid, "user PML4 creation failed",
                                       new_kstack, argv_addrs);
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

        /* Argument strings at the very top of the user stack. use_argv / argv_addrs
         * were resolved above (before the guard stack) so any argv failure could
         * not leak it. When argv is present it MUST be used (a name-path fallback
         * would disagree with the PEB CommandLine built from the full argv); the
         * name path (argc=1) applies only when argv is unset. argv is read here on
         * thread 0 with no concurrent task_set_argv, so no environ_lock is needed. */
        uint64_t argv0_addr;

        if (use_argv) {
            int ai;
            uint32_t str_qwords = 0;
            /* Push each argv string top-down, recording its user address. */
            for (ai = t_argc - 1; ai >= 0; ai--) {
                const char *s = t_argv[ai] ? t_argv[ai] : "";
                uint32_t slen = 0; while (s[slen]) slen++;
                uint32_t qw = (slen + 1u + 7u) / 8u;
                ustk -= qw;
                { char *dst = (char *)ustk; uint32_t k; for (k = 0; k <= slen; k++) dst[k] = s[k]; }
                argv_addrs[ai] = (uint64_t)ustk;
                str_qwords += qw;
            }
            /* Keep the string area an even qword count so ustk stays 16-aligned
             * (the name path below pushes exactly 2 qwords). */
            if (str_qwords & 1u) { ustk--; *ustk = 0; }
            argv0_addr = argv_addrs[0];
        } else {
            /* String data at very top of stack */
            ustk -= 2;  /* room for string (up to 16 bytes aligned) */
            {
                char *str = (char *)ustk;
                uint32_t i;
                for (i = 0; i <= name_len && i < 15; i++)
                    str[i] = name[i];
                str[i > 0 ? i : 0] = '\0';
            }
            argv0_addr = (uint64_t)ustk;
        }

        /* SysV entry requires &argc 16-byte aligned. Every push below is an even
         * qword count (AT_RANDOM=2, auxv=2*naux) EXCEPT the (argc+3)-qword
         * pointer block, which is odd when argc is even. Pad one qword here
         * (above auxv, an ABI-unspecified region) so argc lands aligned. */
        if (use_argv && !((uint32_t)t_argc & 1u)) { ustk--; *ustk = 0; }

        /*: push 16 random bytes for AT_RANDOM. glibc/musl read exactly
         * 16 bytes from the address pushed in AT_RANDOM as the seed for
         * __stack_chk_guard. Without this, dynamically linked binaries
         * compiled with -fstack-protector use a zero or constant canary,
         * defeating stack overflow protection. */
        ustk -= 2;  /* 2 qwords = 16 bytes */
        csprng_fill((uint8_t *)ustk, 16);
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

        /* Emit auxv pairs from the single-source-of-truth EXEC_AUXV_ENTRIES list
         * into a compile-time-sized local array. naux is derived from the SAME
         * list (EXEC_AUXV_PAIRS), so the count, the emission, and the array size
         * can never drift; the file-scope _Static_assert ties ARGV_FRAME_RESERVE
         * to that same count. Adding/removing an entry cannot compile without
         * reconciling the argv stack budget. */
        uint64_t auxv[EXEC_AUXV_PAIRS * 2];
        uint32_t naux = 0;
        #define AUXV_EMIT(t, v) do { \
                auxv[naux*2]   = (uint64_t)(t); \
                auxv[naux*2+1] = (uint64_t)(v); \
                naux++; \
            } while (0);
        EXEC_AUXV_ENTRIES(AUXV_EMIT)
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

        /* envp NULL terminator (no env on the ELF stack; env is via the PEB). */
        ustk--;
        *ustk = 0;

        if (use_argv) {
            int ai;
            /* argv[] NULL terminator, then argv[argc-1..0] so argc lands lowest
             * ([rsp]=argc, [rsp+8]=argv[0], ...). */
            ustk--; *ustk = 0;
            for (ai = t_argc - 1; ai >= 0; ai--) {
                ustk--; *ustk = argv_addrs[ai];
            }
            ustk--; *ustk = (uint64_t)t_argc;   /* argc */
            kfree(argv_addrs);
            /* NULL it: every post-commit abort below passes argv_addrs to
             * exec_commit_failure(), which frees it. A stale pointer here
             * would be a double free. */
            argv_addrs = (uint64_t *)0;
        } else {
            /* argv[1] = NULL (terminator) */
            ustk--;
            *ustk = 0;

            /* argv[0] = program name */
            ustk--;
            *ustk = argv0_addr;

            /* argc = 1 */
            ustk--;
            *ustk = 1;
        }

        /* &argc is 16-byte aligned by construction (pads emitted above auxv);
         * the mask is a defensive no-op. */
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
         * and parse ProcessParameters for argv -- no stack changes needed.
         *
         * Left 0 here and PATCHED at publication, after peb_alloc_for_task().
         * This slot used to snapshot tasks[pid].peb at frame-build time, which
         * is BEFORE the replacement PEB exists -- so a re-exec handed ring 3
         * the previous image's PEB and a first exec handed it 0. */
        sp[12] = 0;                                /* rcx = PEB (patched below) */
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

    /* Allocate PEB in user address space. A NULL PEB means ring 3 would start
     * with RCX == 0 and no ProcessParameters -- fail closed rather than
     * publish a process that faults on its first PEB access. */
    tasks[pid].peb = (void *)peb_alloc_for_task(
        pid, entry, tasks[pid].name);
    if (!tasks[pid].peb)
        return exec_commit_failure(pid, "PEB allocation failed",
                                   new_kstack, argv_addrs);

    /* Allocate TEB for the initial thread (TID 0).
     *
     * The ALLOCATION lands here because it is fallible; the GS WIRING it feeds
     * is deferred to the publication window below, and that split is
     * load-bearing. Priming kernel_gs_base at this point exposes it for the
     * whole remaining stretch before publication: the scheduler's GS save-gate
     * is keyed on exec_pending, which is still clear until then, so on an
     * interrupts-enabled launcher path a tick landing in the gap runs the gate
     * and overwrites the freshly primed TEB pointer with the CURRENT MSR value
     * -- the OUTGOING image's TEB. task_exec_take_pending_frame would then
     * program that stale value and ring 3 would start with another process's
     * TEB in GS. The gate's fail-closed check cannot catch it either: that only
     * fires when the slot is exactly 0, and a stale non-zero TEB passes. */
    {
        uintptr_t ustack = (uintptr_t)tasks[pid].user_stack_base;
        TEB *teb = teb_alloc_for_task(pid, 0, ustack, USER_STACK_SIZE,
                                       tasks[pid].peb);
        if (!teb)
            return exec_commit_failure(pid, "TEB allocation failed",
                                       new_kstack, argv_addrs);
        /* DO NOT publish the TEB here. Both `tasks[pid].teb` and
         * `threads[0].teb` are published together with `kernel_gs_base`
         * inside the local_irq_save window below, gs_base FIRST.
         *
         * WHY (2026-08-02, intermittent 2-CPU boot halt). Publishing the TEB
         * here left a ~120-line window in which the task had a non-NULL TEB
         * and a ZERO kernel_gs_base. The fail-closed guard in the switch path
         * ("ring-3 task N thread M has TEB but kernel_gs_base=0") tests only
         * `teb != NULL && new_gs == 0`; it has no exec_pending exemption, so
         * it fatals on a state task_exec deliberately created. The old comment
         * here argued the early publish was harmless because "exec_pending is
         * still clear, so the save-gates behave exactly as before" -- true for
         * the SAVE-GATES, which do test exec_pending, and false for that guard.
         *
         * `local_irq_save` hid it at -smp 1 by making the window unobservable
         * locally; at -smp 2 the OTHER cpu's scheduler selects this task and
         * sees the half-published pair. Signature: kvm-2cpu halts while
         * kvm-1cpu / tcg-1cpu / tcg-2cpu pass, ~1 leg per 4-8 matrix runs.
         *
         * Priming gs_base HERE instead would reintroduce the bug the window
         * exists to prevent: a tick before the window would let the GS
         * save-gate store the outgoing image's MSR over the primed TEB. So the
         * pair must be published together, inside, and gs_base must go first.
         * `fork` already orders it that way (kernel_gs_base then teb). */
        new_teb = teb;
    }

    if (tasks[pid].peb) {
        PEB *p = (PEB *)tasks[pid].peb;
        klog(LOG_DEBUG, "sched",
             "PID %u: PEB=%p TEB=%p (Win %u.%u.%u, %u CPUs)",
             (uint64_t)pid,
             (uint64_t)(uintptr_t)tasks[pid].peb,
             (uint64_t)(uintptr_t)new_teb,   /* not yet published; see above */
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
                if (new_teb)   /* local: publication happens in the window below */
                    ObInsertObject((void *)new_teb, "Teb", proc_dir);
            }
            ObDereferenceObject(ko_dir);
        }
    }

    klog(LOG_DEBUG, "sched", "PID %u -> entry %p",
           (uint64_t)pid, entry);

    /* ---- Release the caller's staging buffer, BEFORE publication ----
     *
     * This is the last point at which the caller is provably still running.
     * The moment the publication below stores exec_pending, a tick may switch
     * this task into the new ring-3 image and the caller's own release never
     * executes -- which is exactly how a successful exec from an
     * interrupts-enabled kernel launcher stranded its buffer.
     *
     * Safe here because everything above has already consumed 'data': the last
     * read is the ELF phdr extraction in the frame builder, and nothing between
     * that point and this one reads it or can fail. Deliberately OUTSIDE the
     * local_irq_save window below, because a reschedule landing HERE is
     * harmless -- exec_pending is still clear, so the save-gates behave exactly
     * as they would for any other preemption of this task, and the frame the
     * gate saves is still the live one. (Not because the release itself needs
     * interrupts enabled: kfree takes its lock with spin_lock_irqsave and
     * pmm_free_frame touches no IRQ state, and on the SYS_EXEC path IF is 0
     * across the whole syscall anyway.) Keeping it out of the window also keeps
     * the interrupt-disabled region to the publication stores alone.
     *
     * Idempotent: the caller calls task_exec_staging_release() again after
     * task_exec returns, which is a no-op once this one has consumed it. On
     * every FAILURE return above, this line is never reached and the caller's
     * post-call release is the one that frees. */
    task_exec_staging_release(staging);

    /* ---- PUBLICATION: the last act of task_exec ----
     *
     * Publishing hands the task over to the scheduler, so it must come after
     * EVERY other write this function makes. The moment exec_pending is set,
     * tasks[pid].rsp names a ring-3 frame the scheduler will happily resume:
     * a switch-in clears exec_pending (task.c:1212 / 1489) and restores that
     * frame, abandoning the rest of task_exec wherever it stood. When the
     * publication sat before the PEB/TEB allocation, a tick in that window
     * entered ring 3 with no TEB and kernel_gs_base == 0, and the remainder of
     * task_exec never ran at all.
     *
     * local_irq_save() rather than KeRaiseIrql(DISPATCH_LEVEL): raising IRQL
     * would NOT close this. irql_to_tpr(DISPATCH_LEVEL) is 0x20 (irql.c:73),
     * which masks only priority groups 0-1, and KeRaiseIrql issues CLI only at
     * >= HIGH_LEVEL (irql.c:134) -- the LAPIC timer still delivers and still
     * calls schedule(). IRQL is also per-CPU bookkeeping and serializes
     * nothing across CPUs. What actually has to be atomic here is same-CPU
     * interrupt reentry between the rsp write and the exec_pending store: a
     * tick landing between them sees exec_pending == 0 and the save-gate
     * (task.c:1185 / 1419) overwrites tasks[pid].rsp with the old syscall
     * frame, which the store then publishes as if it were the exec frame.
     * local_irq_save/restore is documented for exactly this shape
     * (spinlock.h:123). On the SYS_EXEC path IF is already 0 -- INT 0x80 is an
     * interrupt gate (idt.c:607, type_attr 0xEE) and isr_common_stub never
     * sti's -- so this is belt-and-braces there and load-bearing on the
     * kernel-launcher path (shell_loader.c), which runs in thread context. */
    {
        uint64_t pub_flags = local_irq_save();

        /* Patch RCX now that the replacement PEB exists (see the frame
         * builder above for why this cannot be done at build time). */
        sp[12] = tasks[pid].peb ? (uint64_t)(uintptr_t)tasks[pid].peb : 0;

        /* Wire GS here, not at TEB allocation: swapgs in the ISR path exchanges
         * GS_BASE <-> KERNEL_GS_BASE, so after swapgs on ring-3 return user-mode
         * GS points at the TEB. Doing it inside this window means no tick can
         * land between the store and the exec_pending publication below, which
         * is what previously let the GS save-gate replace the primed TEB with
         * the outgoing image's. Past publication the gate skips this task
         * entirely, so the value is stable from here on. */
        tasks[pid].kernel_gs_base = (uint64_t)(uintptr_t)new_teb;
        tasks[pid].threads[0].kernel_gs_base = tasks[pid].kernel_gs_base;

        /* TEB published AFTER gs_base, with release ordering: the switch-path
         * guard reads teb first and then gs_base, so any cpu that observes a
         * non-NULL TEB is guaranteed to observe the matching gs_base. Ordered
         * stores, not a lock -- the guard is a read-only observer and needs
         * only that the pair never appears half-written. */
        __atomic_store_n(&tasks[pid].threads[0].teb, (void *)new_teb,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&tasks[pid].teb, (void *)new_teb, __ATOMIC_RELEASE);

        /* Park the superseded kernel stack. task_exec's caller is still
         * running on it, so it cannot be freed here; the next task_exec for
         * this task or task_cleanup reclaims it. */
        tasks[pid].stack_pending_free = tasks[pid].stack_base;

        tasks[pid].rsp = (uint64_t)sp;
        tasks[pid].stack_base = new_kstack;
        tasks[pid].kernel_rsp = (uint64_t)(new_kstack + TASK_STACK_SIZE);
        tasks[pid].threads[0].kernel_rsp = tasks[pid].kernel_rsp;
        tasks[pid].exec_pending_tick = uptime();  /* for stuck detection */
        /* Release-store LAST, and pairs with the __atomic_load_n ACQUIRE in
         * the scheduler save-gate: any CPU observing exec_pending=1 also sees
         * the iretq frame writes and every field written above it. */
        __atomic_store_n(&tasks[pid].exec_pending, 1u, __ATOMIC_RELEASE);

        local_irq_restore(pub_flags);
    }

    return 0;
}

/* Adopt a pending exec frame on the SYSCALL-RETURN path.
 *
 * task_exec() publishes the new ring-3 iretq frame on a fresh kernel stack and
 * sets exec_pending, expecting the next context switch to adopt it -- which is
 * why the scheduler's save-gate deliberately does NOT overwrite tasks[pid].rsp
 * while the flag is set. That contract holds for the kernel-launcher exec path,
 * whose caller hlt-loops afterwards so a tick is guaranteed to land.
 *
 * SYS_EXEC gets no such guarantee: it runs synchronously on the calling task,
 * and with no tick between task_exec() and the stub's iretq the syscall returns
 * to the PRE-exec RIP. By then exec_load has already written the new binary
 * across the image range, so the task resumes on whatever bytes the new image
 * holds at the old RIP -- arbitrary instructions, then a fault at a nonsense
 * address. Diagnosed 2026-07-27 from a transition-ring dump: PID 13 returned to
 * ring 3 at its pre-exec RIP 0x8003A0 and faulted at 0x62226100202020 with RBP
 * holding ASCII "PIPE-OK" popped off the recycled stack. Deterministic under
 * QEMU 8.2.2 TCG, invisible under 10.2.1 -- purely tick-timing luck, and the
 * sole cause of the red CI unit-test job.
 *
 * Adopting the frame here makes the handoff deterministic instead of
 * timing-dependent. It mirrors the scheduler switch-in work that a same-task
 * return would otherwise skip:
 *   - TSS.rsp0 / per-CPU syscall_rsp0: task_exec moved the task to a new
 *     kernel stack, so the next ring-3 -> ring-0 entry must land on it.
 *   - KERNEL_GS_BASE: task_exec allocated a new TEB for thread 0.
 * CR3 needs no reload -- the fork+exec branch keeps the task's existing
 * per-process PML4, and the launcher branch already loaded the new one inside
 * task_exec. The abandoned kernel stack is left exactly as the tick path
 * leaves it (task_exec already re-pointed stack_base at the new allocation).
 *
 * Returns the frame pointer the ISR stub should iretq from: the exec frame when
 * one is pending, otherwise the caller's own frame unchanged. */
uint64_t task_exec_take_pending_frame(struct interrupt_frame *frame)
{
    uint32_t pid = current_task;
    uint64_t exec_frame;

    /* ACQUIRE pairs with the RELEASE store in task_exec: observing the flag
     * also observes every iretq-frame write that preceded it. */
    if (!__atomic_load_n(&tasks[pid].exec_pending, __ATOMIC_ACQUIRE))
        return (uint64_t)frame;

    exec_frame = tasks[pid].rsp;
    if (!exec_frame) {
        /* Flag set with no published frame: refuse to iretq from a NULL RSP.
         * Returning the pre-exec frame is still wrong for the task, but it is
         * a recoverable user-mode fault instead of a ring-0 triple fault. */
        klog(LOG_ERROR, "sched",
             "PID %u: exec_pending set with no frame -- keeping pre-exec frame",
             (uint64_t)pid);
        __atomic_store_n(&tasks[pid].exec_pending, 0u, __ATOMIC_RELEASE);
        tasks[pid].exec_pending_tick = 0;
        return (uint64_t)frame;
    }

    /* ARCH: x86-64 -- will move to arch/ with the TSS/MSR HAL. */
    {
        uint64_t krsp = tasks[pid].threads[0].kernel_rsp;
        if (krsp) {
            tss_set_kernel_stack(krsp);
            smp_this_cpu()->syscall_rsp0 = krsp;
        }
    }

    {
        uint64_t new_gs = tasks[pid].threads[0].kernel_gs_base;
        if (new_gs) {
            msr_write(MSR_IA32_KERNEL_GS_BASE, new_gs);
            /* Same readback invariant the scheduler applies: a hypervisor that
             * swallows the write must surface as a named log, not as a silent
             * gs:<off> fault on the new image's first TEB read. */
            {
                uint64_t rb = msr_read(MSR_IA32_KERNEL_GS_BASE);
                if (rb != new_gs)
                    klog(LOG_FATAL, "sched",
                         "MSR_KERNEL_GS_BASE corrupted on exec return: "
                         "wrote=0x%X read=0x%X (task=%u)",
                         new_gs, rb, (uint64_t)pid);
            }
        }
    }

    /* Adoption evidence, on the syscall-return arm of the handoff. Reaching
     * here means the entry guard observed exec_pending set AND a real frame
     * was found, so the frame is being consumed now. Deliberately not marked
     * on the "set with no frame" bail-out above: that path clears the flag
     * and keeps the PRE-exec frame, which is the opposite of an adoption and
     * would report a lost frame as a binary that ran. */
    TASK_UTEST_NOTE_FRAME_ADOPTED_CURSOR(pid);

    /* RELEASE so the next scheduler save-gate sees the cleared flag together
     * with the TSS/MSR updates above, and resumes saving frames normally. */
    __atomic_store_n(&tasks[pid].exec_pending, 0u, __ATOMIC_RELEASE);
    tasks[pid].exec_pending_tick = 0;

    return exec_frame;
}

/* Release the resources reclaimable at the DEAD-transition point, shared by
 * every process death path (see task.h). The caller owns the TASK_DEAD state
 * store; this only does resource release, and each sub-call is idempotent so a
 * double death path never double-frees. Log-free by contract. */
void task_death_teardown(struct task *t)
{
    if (!t)
        return;
    /* Drop the syscall-filter count so a dead-but-unreaped filtered task stops
     * taxing the global dispatch fast path (snapshot memory frees at reap). */
    syscall_filter_task_dead(t);
    /* Mark the OB process object dead so it becomes reclaimable. */
    ob_process_mark_dead(t->pid);
    /* SEAL the charge gate first: from here no new charge can be admitted
     * against this task, so the detach and the block release below cannot race a
     * charger that would bill blocks being torn down. Sealing does NOT wait for
     * an operation already in flight (one CAS, log-free, safe at elevated IRQL);
     * the reap-time ledger release is what waits. Returns stay legal after the
     * seal -- an obligation for a resource that outlives this task must still be
     * returnable, or everything it charged for would leak. */
    quota_gate_seal(t);
    /* Leave any Job Object cleanly: removes the pid, decrements the active
     * count, drops the membership Ob reference. No-op when unassigned. */
    ob_job_detach_task(t);
    /* Release the process and user quota blocks. Ordered AFTER the job detach
     * so the chain is torn down from the inside out, and idempotent like every
     * other sub-call here (a doubled death path must not double-deref). Any
     * charge still outstanding is released by its owner's own cleanup, which
     * holds its own receipt; dropping these references only ends this task's
     * claim on the blocks, and a block with usage left in it stays alive until
     * that last reference goes. */
    quota_task_teardown(t);
    /* Reap any leaked timer-resolution request so a fast tick this process
     * asked for does not outlive it. No-op when the pid held none. */
    timer_resolution_release_process(t->pid);
}

#ifdef KERNEL_TESTS
void task_utest_cap_note_thread_death(struct task *t, struct thread *thr)
{
    struct task *owner;
    uint8_t      fenced;
    uint8_t      expected;

    if (!t || !thr)
        return;
    /* Only an ARMED write is owed an explanation. A thread that never
     * captured, or whose write already settled through capture_end or a
     * budget CUT, has nothing outstanding. */
    /* ACQUIRE, because this is a REMOTE read of a field the owning thread
     * publishes with release, and because the fields it gates -- the write
     * identity and the previous death reason -- are only meaningful behind
     * that edge. A plain read here would also be a data race against the CAS
     * and the release store, which no argument about the benignness of the
     * values can license. */
    if (__atomic_load_n(&thr->utest_cap_state, __ATOMIC_ACQUIRE) !=
        (uint8_t)UTEST_CAP_THREAD_OPEN)
        return;
    owner = task_get_by_pid(t->utest_capture_owner_pid);
    fenced = owner ? __atomic_load_n(&owner->utest_capture_fenced,
                                     __ATOMIC_ACQUIRE)
                   : (uint8_t)0;

    /* FIRST SNAPSHOT WINS, claimed by exchange rather than observed by a
     * check-then-store. Death paths are deliberately idempotent and can
     * overlap -- a racing self-exit and a remote kill both reach a transition
     * -- so two callers can read NONE and the LATER store would win, which is
     * the wrong one: the earliest reading is the true one, and a later one can
     * see a reap fence latched after this thread had already died. Since the
     * host EXEMPTS a fenced abandon, losing that race certifies genuinely lost
     * output as a deliberate teardown, which is precisely the masking this
     * evidence exists to remove. */
    expected = (uint8_t)UTEST_CAP_DEATH_NONE;
    if (!__atomic_compare_exchange_n(&thr->utest_cap_death, &expected,
                                     fenced ? (uint8_t)UTEST_CAP_DEATH_FENCED
                                            : (uint8_t)UTEST_CAP_DEATH_KILLED,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    /* The owner's stop epoch AS OF THIS DEATH. Recorded here and never
     * re-read at reap, because the reap barrier runs arbitrarily later: a peer
     * sharing this owner can exhaust the budget after this thread has already
     * died, and a reap comparing the epoch it finds THEN would report a killed
     * write as an intentional budget cut -- handing a lost payload the very
     * exemption the per-write narrowing was introduced to withdraw. Only the
     * exchange winner writes it, so the value belongs to the reading that won.
     */
    thr->utest_cap_death_epoch =
        owner ? __atomic_load_n(&owner->utest_capture_stop_epoch,
                                __ATOMIC_ACQUIRE)
              : 0u;
}

void task_utest_cap_note_task_death(struct task *t)
{
    uint32_t tid;

    if (!t)
        return;
    /* Every thread, not just thread 0: a task killed wholesale takes its
     * secondary threads down with it, and each of them may hold its own open
     * write on its own stack. */
    for (tid = 0; tid < t->num_threads && tid < THREAD_MAX; tid++)
        task_utest_cap_note_thread_death(t, &t->threads[tid]);
}
#endif

/* Centralized remote-death transition (see task.h). Idempotent: returns
 * immediately if the target is already dead, so concurrent kill paths and a
 * racing self-exit never double-teardown. Mirrors task_exit's TASK_DEAD-side
 * teardown for a process that will NEVER run its own task_exit. */
void task_terminate_remote(struct task *t, int32_t exit_code)
{
    if (!t || t->state == TASK_DEAD)
        return;
#ifdef KERNEL_TESTS
    /* BEFORE the DEAD publish: the reap fence must be read while this
     * thread's death is still the current event. */
    task_utest_cap_note_task_death(t);
#endif
    t->state = TASK_DEAD;
    t->exit_status = exit_code;
    task_death_teardown(t);
}

void task_exit(int32_t status)
{
    uint32_t pid = current_task;
    uint32_t i;

#ifdef KERNEL_TESTS
    /* Capture evidence first, while no death has been published yet: every
     * thread of this task is about to stop existing, and any of them may be
     * holding an open capture write on its own stack. */
    task_utest_cap_note_task_death(&tasks[pid]);
#endif
    /* Mark the process main thread DEAD under its APC lock FIRST -- before
     * TASK_DEAD is externally visible -- so a cross-thread KeInsertQueueApc
     * cannot enqueue onto a dead process's thread-0 in the window between the
     * two stores (KeInsertQueueApc rejects on the TARGET thread's state). */
    {
        uint64_t af;
        spin_lock_irqsave(&tasks[pid].threads[0].apc_lock, &af);
        tasks[pid].threads[0].state = THREAD_DEAD;
        spin_unlock_irqrestore(&tasks[pid].threads[0].apc_lock, af);
    }
    /* Run RundownRoutine for any APCs still queued on the dead main thread. */
    apc_rundown_thread(&tasks[pid].threads[0]);
    tasks[pid].state = TASK_DEAD;
    tasks[pid].exit_status = status;
    /* Release every resource reclaimable at the DEAD transition (syscall-filter
     * count, OB process object, Job Object membership, leaked timer-resolution
     * request). Shared with the remote-kill, normal-return, and fatal-signal
     * death paths. Safe at this elevated IRQL: each sub-call takes only its own
     * irqsave lock and nests under no caller-held lock. The memory that needs
     * the reap barrier (stacks, CR3, PEB/TEB) frees later in task_cleanup. */
    task_death_teardown(&tasks[pid]);

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
     * subsequent klog calls and breaking the rest of boot.
     *
     * Forced (unbalanced) lower: the IDT restore is bypassed, so use
     * KeLowerIrqlForced to count + classify it as a forced lower rather
     * than tripping the monotonic-lower telemetry. */
    KeLowerIrqlForced(PASSIVE_LEVEL, "task_exit");

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

    /* Free the syscall filter + its whole retire chain. Safe here: the task is
     * DEAD, so no thread of it can be mid-dispatch reading a retired snapshot. */
    syscall_filter_task_teardown(&tasks[pid]);

    /* Free the unveil list at the same reap barrier (no thread mid file-open). */
    pledge_unveil_teardown(&tasks[pid]);

    /* Free the per-process environment + argv arrays at the same barrier (task
     * is DEAD, no thread of it reads environ; env_free takes no lock). */
    env_free(&tasks[pid]);

    /* Close all handles and free handle table. Snapshot the occupied-slot count
     * first for the reap-cleanup log. Closing a file handle drives
     * ObpFreeHandle -> file_on_close -> vfs_close, which also releases that
     * file's share-mode entry and triggers delete-on-close, so those per-file
     * resources are reclaimed here as a side effect of the handle sweep.
     * (Byte-range locks and oplocks are NOT released by vfs_close and are
     * tracked as concrete follow-ups -- see the process exit cleanup notes.) */
    {
        uint32_t handles_closed = tasks[pid].handle_table.count;
        ob_handle_table_destroy(&tasks[pid].handle_table);
        klog(LOG_DEBUG, "task", "PID %u reap cleanup: %u handles closed",
             (uint64_t)pid, (uint64_t)handles_closed);
    }

    /* Free kernel stack (task-level, thread 0), plus any stack a task_exec
     * superseded and parked. Both go through task_free_kernel_stack(), which
     * carries the allocator discrimination and the uninstall-guard-before-PMM
     * -free rule (see its comment for the two 2026-04-20 incidents).
     *
     * The parked stack is safe to free here because the task is OFF-CPU, and
     * that property now rests on the dispatcher rather than on the caller:
     * dispatch uses the single global current_task (APs never run scheduled
     * tasks), so a TASK_DEAD task cannot be executing anywhere while this
     * runs. It used to be stated as "task_cleanup runs only after the parent
     * observed TASK_DEAD in task_waitpid", which stopped being true of every
     * caller when the usermode launcher's run-boundary reap began cleaning
     * capture-owning descendants it had terminated itself
     * (u_capture_reap_tree, src/kernel/test/test_usermode.c) with no waitpid
     * anywhere in that chain. If the task exited before reaching another
     * exec, this is where its last stale stack goes back. */
    if (tasks[pid].stack_base) {
        task_free_kernel_stack(tasks[pid].stack_base);
        tasks[pid].stack_base = (uint8_t *)0;
    }
    if (tasks[pid].stack_pending_free) {
        /* Hold the pointer when the release is REFUSED, so a later drain of
         * this same slot can retry (uninstall keeps the guard registration on
         * failure for exactly that). This is a best effort, not a guarantee:
         * a successful exec republishes the slot below and the retained run is
         * then lost. A durable owner across re-exec, rollback and reap is the
         * filed section-19 follow-up. */
        if (task_free_kernel_stack(tasks[pid].stack_pending_free) == 0)
            tasks[pid].stack_pending_free = (uint8_t *)0;
    }
    tasks[pid].threads[0].kernel_rsp = 0;
    /* Thread 0's task-owned stack is freed here (NOT via thread_free_stacks,
     * which only runs for secondary threads below), so clear its SEH chain head
     * on this path too -- upholds "no KI_TRY chain head outlives its stack" for
     * thread 0. */
    tasks[pid].threads[0].kernel_exception_list = (struct ki_exception_registration *)0;

#ifdef KERNEL_TESTS
    /* Emit any open-write capture evidence for EVERY thread of this task, and
     * thread 0 above all: a task killed wholesale never runs any of its
     * threads again, so none of them reach thread_reap_kernel_slot, and the
     * MAIN thread is where an ordinary test binary does its writing. Scoping
     * this to the secondary-thread loop below would have left the common case
     * -- a binary killed by timeout or by the descendant reap while inside
     * write() -- producing no record at all, which is the exact silence this
     * evidence exists to break.
     *
     * Ordered before TASK_UTEST_CAPTURE_UNLINK clears owner_pid further down,
     * which is what the settlement resolves the owner slot through. It is NOT
     * before every stack free -- thread 0's task-owned stack is released
     * above -- and settlement must therefore never read stack-resident state:
     * the evidence lives in the TCB precisely so it outlives the stack. The death outcome each
     * thread carries was snapshotted at its real death transition, so nothing
     * here re-reads the reap fence. */
    {
        uint32_t ti;
        for (ti = 0; ti < tasks[pid].num_threads && ti < THREAD_MAX; ti++) {
            test_usermode_cap_settle_dead_thread(&tasks[pid],
                                                 &tasks[pid].threads[ti]);
            thread_utest_cap_reset(&tasks[pid].threads[ti]);
        }
    }
#endif

    /* Free per-thread kernel + user stacks + TEBs for secondary threads */
    {
        uint32_t ti;
        for (ti = 1; ti < tasks[pid].num_threads; ti++) {
            /* Publish THREAD_DEAD under apc_lock and run RundownRoutine for any
             * still-queued APCs BEFORE freeing the stack: task_exit only marked
             * thread 0, so a secondary thread can reach here THREAD_READY with
             * queued APCs. The DEAD publish also makes a late cross-task
             * KeInsertQueueApc reject this slot instead of enqueuing onto a
             * thread whose stack is being freed. (Task is TASK_DEAD -- no other
             * CPU runs it -- so this is race-free.) */
            {
                uint64_t af;
                spin_lock_irqsave(&tasks[pid].threads[ti].apc_lock, &af);
                tasks[pid].threads[ti].state = THREAD_DEAD;
                spin_unlock_irqrestore(&tasks[pid].threads[ti].apc_lock, af);
            }
            apc_rundown_thread(&tasks[pid].threads[ti]);
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

    /* Release this process's security tokens before the slot is reclaimable.
     * The primary token is an Ob-owned dup assigned at fork/create; every thread
     * slot may hold an Ob-owned impersonation token if it exited without calling
     * RevertToSelf. Without this teardown a process that forked or impersonated
     * leaks token objects, and a reused slot would carry a stale primary-token
     * pointer (stale security state). Loop ALL threads (thread 0 included; the
     * secondary loop above starts at 1); the exchange-to-NULL is idempotent so a
     * slot already cleared by thread_reap_kernel_slot derefs nothing twice. */
    {
        uint32_t ti;
        for (ti = 0; ti < tasks[pid].num_threads; ti++) {
            ACCESS_TOKEN *imp = (ACCESS_TOKEN *)__atomic_exchange_n(
                &tasks[pid].threads[ti].impersonation_token, (void *)0,
                __ATOMIC_ACQ_REL);
            if (imp)
                PsDereferencePrimaryToken(imp);
        }
        {
            /* Exchange-to-NULL BEFORE the deref, same discipline as the
             * impersonation tokens above: clear the published slot pointer
             * before dropping the last Ob reference so a reader that loads the
             * slot after this point sees NULL, never a freed pointer. (Full
             * reader/teardown mutual exclusion is the deferred per-token-lock
             * protocol -- safe today on the single-cursor scheduler, which
             * reaps only off-CPU DEAD tasks.) */
            ACCESS_TOKEN *pt = (ACCESS_TOKEN *)__atomic_exchange_n(
                &tasks[pid].token, (void *)0, __ATOMIC_ACQ_REL);
            if (pt)
                PsDereferencePrimaryToken(pt);
        }
    }

    /* Release the task's claim on its charge ledger, AFTER every teardown step
     * that can destroy an object body -- and the invariant is exactly that, not a
     * line number. The release decides which outstanding obligations are orphans
     * with no holder left to return them, and reclaims those as leaks. So it must
     * follow EVERY path that still owes a return:
     *
     *   - ob_handle_table_destroy above, which returns the charges the task's own
     *     handles held. Deciding before it would report every open handle a leak.
     *   - apc_rundown_thread in the per-thread loop above. No production code
     *     registers a rundown routine today, but the mechanism runs an arbitrary
     *     caller-supplied function outside apc_lock precisely so one MAY free an
     *     object body, and this ordering is what keeps that future routine from
     *     turning into a false leak.
     *   - the ACCESS_TOKEN derefs immediately above. Token bodies are
     *     ob_alloc_object allocations, so a body obligation released before them
     *     would be reclaimed-as-orphan and counted a leak on EVERY process exit
     *     that had a primary token -- which is every one of them.
     *
     * Anything still outstanding after all of that is genuinely either held by a
     * resource that legitimately outlives the task (left alone) or orphaned. */
    quota_ledger_task_release(&tasks[pid]);

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

    /* A reaped slot is no longer a member of anybody's output-capture tree.
     *
     * Unlinked HERE, at the reap barrier, because these are the fields every
     * capture tree walk selects on and nothing else clears them on the death
     * path -- neither task_exit nor task_terminate_remote touches them, so a
     * cleaned-up child kept advertising itself as live and the usermode
     * launcher's run-boundary reap selected it and called task_cleanup on a
     * slot the ring-3 parent's own waitpid had already cleaned. That is
     * survivable only while every sub-teardown above happens to be
     * idempotent, which none of them declares.
     *
     * UNLINK, not reset: the full reset would also clear the stop latch and
     * the sequence counter, and this runs on the OWNER too. See
     * task_utest_capture_unlink for why that would un-fence a descendant
     * that outlived the reap and restart its record numbering.
     *
     * Safe at this point: every reader of these fields runs while the task is
     * alive (the write path) or before cleanup (the launcher's report and
     * leak snapshots, which are taken from the live TCB). */
    TASK_UTEST_CAPTURE_UNLINK(&tasks[pid]);

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
    /* Stay NON-insertable (FREE) until the APC state is reset below, so a stale
     * cross-thread KeInsertQueueApc cannot enqueue into a half-reset queue
     * during slot reuse. Promoted to THREAD_READY after the APC reset. */
    t->threads[tid].state = THREAD_FREE;
    t->threads[tid].rsp = (uint64_t)sp;
    t->threads[tid].stack_base = stack;
    t->threads[tid].stack_size = stack_size;
    t->threads[tid].parent_task = task_idx;
    t->threads[tid].exit_status = 0;
    t->threads[tid].join_tid = -1;
    t->threads[tid].priority      = THREAD_PRIO_NORMAL;
    t->threads[tid].base_priority = THREAD_PRIO_NORMAL;
    t->threads[tid].previous_mode = 0;  /* KernelMode: reused slot must not inherit a stale NT probe-gating flag */
    t->threads[tid].in_system_service = 0;  /* reused slot must not inherit a stale system-service flag */
#ifdef KERNEL_TESTS
    thread_utest_cap_reset(&t->threads[tid]);  /* nor a stale open capture write */
#endif
    t->threads[tid].impersonation_token = (void *)0;  /* no stale impersonation on slot reuse */
    fault_site_reset_thread(&t->threads[tid]);  /* no stale site marker on slot reuse */
    t->threads[tid].kernel_exception_list = (struct ki_exception_registration *)0;  /* no stale KI_TRY chain */
    t->threads[tid].pledge_pending = 0;
    t->threads[tid].kernel_rsp = 0;  /* kernel thread -- no rsp0 switching */
    t->threads[tid].kernel_stack_base = (uint8_t *)0;
    t->threads[tid].kernel_stack_pages = 0;
    t->threads[tid].user_stack_va = 0;
    t->threads[tid].user_stack_pages = 0;
    t->threads[tid].teb = (void *)0;          /* kernel thread -- no TEB */
    t->threads[tid].kernel_gs_base = 0;
    /* APC state: reset the per-thread queues + region counters under the APC
     * lock. On a reused slot this clears any stale binding before the thread
     * becomes insertable; the lock is persistent (BSS-zero on first use). */
    {
        uint64_t af;
        spin_lock_irqsave(&t->threads[tid].apc_lock, &af);
        apc_thread_init(&t->threads[tid].apc_state, t);
        t->threads[tid].kernel_apc_disable  = 0;
        t->threads[tid].special_apc_disable = 0;
        spin_unlock_irqrestore(&t->threads[tid].apc_lock, af);
    }
    /* APC state is now valid -- make the thread schedulable + insertable. */
    t->threads[tid].state = THREAD_READY;
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

/* Free a per-thread kernel stack run whose FIRST page is the guard.
 *
 * The uthread run is [guard][stack pages...] with kernel_stack_base pointing at
 * the GUARD, unlike a task-level stack (task_free_kernel_stack) whose base is
 * the first usable page. Both obey the same rule: the guard's identity PTE must
 * be restored before any frame in the run goes back to the PMM, or the next
 * allocator that writes through that address faults in kernel mode. Every
 * uthread_create rollback and thread_free_stacks route through here so the rule
 * exists once -- five open-coded copies of the free loop is how the 2026-04-20
 * defect survived on this path after the task-level one was fixed. */
static void uthread_free_kernel_stack(uintptr_t kstack_phys, uint32_t kstack_pages)
{
    if (!kstack_phys || !kstack_pages)
        return;

    if (vmm_uninstall_guard_page(kstack_phys) != 0) {
        klog(LOG_ERROR, "sched",
             "uthread: leaking kernel stack at %p -- guard page not restored",
             (void *)kstack_phys);
        return;
    }
    pmm_free_pages(kstack_phys, kstack_pages);
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

    /* Guard page at bottom of kernel stack. Refuse the thread rather than run
     * ring-3 code on an unguarded kernel stack; the install fails before
     * touching any PTE, so the run is still the caller's to release. */
    {
        int guard_rc = vmm_install_guard_page(kstack_phys, "uthread kernel stack");
        if (guard_rc != VMM_GUARD_OK) {
            klog(LOG_ERROR, "sched",
                 "uthread_create: no guard page available for kernel stack");
            stack_run_release_after_guard_failure(kstack_phys, kstack_pages, guard_rc);
            return -1;
        }
    }

    /* Allocate per-thread USER stack (PMM: 16 KiB) */
    ustack_phys = pmm_alloc_contiguous(ustack_pages);
    if (!ustack_phys) {
        klog(LOG_ERROR, "sched",
             "uthread_create: cannot allocate user stack");
        uthread_free_kernel_stack(kstack_phys, kstack_pages);
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
        uthread_free_kernel_stack(kstack_phys, kstack_pages);
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
            uthread_free_kernel_stack(kstack_phys, kstack_pages);
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
            uthread_free_kernel_stack(kstack_phys, kstack_pages);
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
    /* Non-insertable until the APC state is reset below (slot-reuse safety). */
    t->threads[tid].state = THREAD_FREE;
    t->threads[tid].rsp = (uint64_t)sp;
    t->threads[tid].stack_base = (uint8_t *)0; /* not kmalloc'd */
    t->threads[tid].stack_size = 0;
    t->threads[tid].parent_task = task_idx;
    t->threads[tid].exit_status = 0;
    t->threads[tid].join_tid = -1;
    t->threads[tid].priority      = THREAD_PRIO_NORMAL;
    t->threads[tid].base_priority = THREAD_PRIO_NORMAL;
    t->threads[tid].previous_mode = 0;  /* KernelMode: reused slot must not inherit a stale NT probe-gating flag */
    t->threads[tid].in_system_service = 0;  /* reused slot must not inherit a stale system-service flag */
#ifdef KERNEL_TESTS
    thread_utest_cap_reset(&t->threads[tid]);  /* nor a stale open capture write */
#endif
    t->threads[tid].impersonation_token = (void *)0;  /* no stale impersonation on slot reuse */
    fault_site_reset_thread(&t->threads[tid]);  /* no stale site marker on slot reuse */
    t->threads[tid].kernel_exception_list = (struct ki_exception_registration *)0;  /* no stale KI_TRY chain */
    t->threads[tid].pledge_pending = 0;

    /* Per-thread kernel stack ownership (for thread_free_stacks) */
    t->threads[tid].kernel_rsp = (uint64_t)(kstack + kstack_pages * 4096);
    t->threads[tid].kernel_stack_base = kstack;
    t->threads[tid].kernel_stack_pages = kstack_pages;

    /* Per-thread user stack ownership (for task_cleanup reclamation) */
    t->threads[tid].user_stack_va = ustack_va;
    t->threads[tid].user_stack_pages = ustack_pages;

    /* APC state: reset queues + region counters under the APC lock (same as
     * kthread_create -- clears any stale binding on a reused slot). */
    {
        uint64_t af;
        spin_lock_irqsave(&t->threads[tid].apc_lock, &af);
        apc_thread_init(&t->threads[tid].apc_state, t);
        t->threads[tid].kernel_apc_disable  = 0;
        t->threads[tid].special_apc_disable = 0;
        spin_unlock_irqrestore(&t->threads[tid].apc_lock, af);
    }
    /* APC state valid -- make schedulable + insertable. */
    t->threads[tid].state = THREAD_READY;

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

    /* Store exit_status BEFORE publishing DEAD so a reader that observes DEAD
     * also observes the final status (a joiner reads status after seeing DEAD). */
    thr->exit_status = status;
#ifdef KERNEL_TESTS
    /* Same ordering rule as the status store, for the same reason: the
     * capture evidence is read while this thread's death is the current
     * event, not reconstructed later from a fence that may move. */
    task_utest_cap_note_thread_death(t, thr);
#endif
    /* Mark DEAD under the APC lock so the transition is atomic vs an in-flight
     * cross-thread KeInsertQueueApc (which rejects DEAD/FREE under the same
     * lock) -- no APC can be enqueued onto an exiting thread. (Rundown of any
     * already-queued APCs is owned by the delivery section; a reused slot is
     * re-zeroed under this lock in kthread_create.) */
    {
        uint64_t af;
        spin_lock_irqsave(&thr->apc_lock, &af);
        thr->state = THREAD_DEAD;
        spin_unlock_irqrestore(&thr->apc_lock, af);
    }
    /* Run RundownRoutine for any APCs still queued on the exiting thread. */
    apc_rundown_thread(thr);

    /* Mark thread object as temporary so it can be freed */
    ob_thread_mark_dead(t->pid, thr->id);

    klog(LOG_DEBUG, "sched", "Thread %u (task %u \"%s\") exited with status %d",
           (uint64_t)thr->id, (uint64_t)t->pid,
           t->name ? t->name : "?",
           (uint64_t)(uint32_t)status);

    /* Wake any thread in the same task blocked on thread_join(our tid).
     * Through the canonical wait-grant seam: a completed join IS a wait being
     * satisfied, so it belongs in wakeup_count like every other grant. Counting
     * it anywhere else, or not at all, would make the metric depend on WHICH
     * primitive a thread happened to block on. */
    for (j = 0; j < t->num_threads; j++) {
        if (t->threads[j].state == THREAD_BLOCKED &&
            t->threads[j].join_tid == (int32_t)thr->id) {
            /* CLEAR join_tid BEFORE releasing the joiner. The wake makes it
             * schedulable immediately, so clearing afterwards let it run with
             * its old join_tid still set -- and if this exiting thread were
             * preempted before the clear, that stale tid could later match an
             * unrelated thread reusing the id and steal its wake. */
            t->threads[j].join_tid = -1;
            task_wake_thread(t, j);
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
    /* PMM-allocated kernel stack (per-thread, for user threads). The run's
     * first page is the guard, so it goes back through the shared helper that
     * restores the identity PTE first -- freeing it raw left the guard's entry
     * in the table forever AND handed an unmapped frame to the PMM. */
    if (thr->kernel_stack_pages > 0 && thr->kernel_stack_base) {
        uthread_free_kernel_stack((uintptr_t)thr->kernel_stack_base,
                                  thr->kernel_stack_pages);
        thr->kernel_stack_base = (uint8_t *)0;
        thr->kernel_stack_pages = 0;
    }
    /* kmalloc'd kernel stack (kernel threads via thread_create/kthread_create) */
    if (thr->stack_base) {
        kfree(thr->stack_base);
        thr->stack_base = (uint8_t *)0;
    }
    thr->kernel_rsp = 0;
    /* A kernel-mode SEH (KI_TRY) chain head points at nodes ON this stack. Clear
     * it in the SHARED stack-free helper so EVERY teardown path -- thread_join's
     * direct call, thread_reap_kernel_slot, task_cleanup -- upholds the invariant
     * "no chain head outlives its stack." A thread killed inside KI_TRY runs no
     * cleanup handler (thread_exit/task_exit are noreturn), so without this the
     * dead TCB would retain a pointer into freed stack memory. */
    thr->kernel_exception_list = (struct ki_exception_registration *)0;
}

static void thread_reap_kernel_slot(struct task *t, uint32_t thread_id)
{
    struct thread *thr = &t->threads[thread_id];

#ifdef KERNEL_TESTS
    /* Emit any open-write evidence BEFORE the slot is advertised THREAD_FREE
     * below, for the same reason previous_mode and the KI_TRY chain are
     * cleared here: kthread_create scans locklessly for a FREE slot, so a new
     * tenant can take this one the instant it is published. Evidence read
     * after that point would be the next thread's, or gone. */
    test_usermode_cap_settle_dead_thread(t, thr);
    thread_utest_cap_reset(thr);
#endif
    thread_free_stacks(thr);
    /* Clear the NT probe-gating flag BEFORE publishing THREAD_FREE: a creator
     * (kthread_create) scans locklessly for THREAD_FREE, so any reusable-
     * lifetime field that gates security -- previous_mode gates ProbeFor*IfUser
     * -- must be reset before the slot is advertised, not after. */
    thr->previous_mode = 0;  /* KernelMode */
    thr->in_system_service = 0;  /* clear the system-service flag before THREAD_FREE:
                                  * a self-terminated thread (noreturn thread_exit) skips
                                  * ssdt_syscall_leave, so clear here so the reused slot
                                  * never misclassifies a later kernel fault as 0x3B */
    /* Release any impersonation token before the slot is advertised for reuse:
     * a thread that exited while impersonating (never called RevertToSelf) would
     * otherwise leak the token's reference and leave a stale pointer for the next
     * tenant. Exchange-then-deref mirrors RevertToSelf. */
    {
        ACCESS_TOKEN *imp = (ACCESS_TOKEN *)__atomic_exchange_n(
            &thr->impersonation_token, (void *)0, __ATOMIC_ACQ_REL);
        if (imp)
            PsDereferencePrimaryToken(imp);
    }
    /* Drop any kernel-mode SEH (KI_TRY) chain before advertising THREAD_FREE: a
     * thread killed while inside a KI_TRY body would otherwise leave a chain head
     * pointing at its now-freed kernel stack for the next tenant of this slot. The
     * nodes are stack-local (no ownership to release), so a plain clear suffices. */
    thr->kernel_exception_list = (struct ki_exception_registration *)0;
    /* Publish THREAD_FREE under the APC lock so a concurrent cross-thread
     * KeInsertQueueApc observes a consistent exiting/reaped state and rejects
     * (no APC enqueued onto a slot being reaped for reuse). */
    {
        uint64_t af;
        spin_lock_irqsave(&thr->apc_lock, &af);
        thr->state = THREAD_FREE;
        spin_unlock_irqrestore(&thr->apc_lock, af);
    }
    /* Defensive rundown: an exited thread runs rundown at thread_exit, so the
     * queues are normally empty here; a thread reaped without a prior exit
     * (FREE published directly) still gets its APCs runned-down before reuse. */
    apc_rundown_thread(thr);
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

    /* Block current thread until target exits.
     *
     * PUBLISH join_tid BEFORE THREAD_BLOCKED. thread_exit's scan matches on
     * (state == THREAD_BLOCKED && join_tid == its id), and x86-64 stores are
     * ordered, so any exiting thread that sees BLOCKED necessarily also sees
     * which tid this thread waits for. The reverse order left a window where
     * the target sampled BLOCKED beside a stale or cleared join_tid, skipped
     * this joiner, and left it asleep forever.
     *
     * This does NOT make registration atomic with the target's exit, and must
     * not be read as doing so: the liveness check above and these two stores
     * are still three unsynchronized steps, so a target that exits and scans
     * before THREAD_BLOCKED is published sees a non-blocked joiner, skips it,
     * and the joiner then blocks forever. Store ordering only constrains what
     * an observer that ALREADY sees BLOCKED can see; it cannot force the scan
     * to happen after it. That remaining window is the same pre-existing
     * publish-before-block class as the other wait primitives and is owned by
     * the scheduler's wait/wake transaction-locking work. */
    t->threads[current_thread].join_tid = (int32_t)thread_id;
    t->threads[current_thread].state = THREAD_BLOCKED;

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

#ifdef KERNEL_TESTS
/* ============================================================================
 * Site-targeted fault injection (test flavor only)
 *
 * Ownership split, and why it is this way:
 *   - fault_site_current is PER-THREAD. A task owns many independently
 *     scheduled threads, so a task-wide marker would let a sibling thread's
 *     allocation consume the single shot armed for this thread's site.
 *   - fault_site_arm is PER-TASK, holding the COMPLETE program (allocator +
 *     site) in one word. Nothing about site mode lives in per_cpu_data, so
 *     unlike the ordinal countdowns it does not inherit the documented
 *     "a migrating task does not trigger" caveat in include/kernel/smp.h.
 *
 * The claim is a compare-exchange, so exactly one allocation fires even if
 * threads on several CPUs reach the same site at once.
 * ========================================================================= */

uint32_t fault_site_enter(uint32_t site)
{
    struct thread *th = thread_current();
    uint32_t prev;

    if (!th)
        return FAULT_SITE_NONE;
    prev = th->fault_site_current;
    th->fault_site_current = site;
    return prev;
}

void fault_site_restore(uint32_t saved)
{
    struct thread *th = thread_current();

    if (th)
        th->fault_site_current = saved;
}

/* Valid participating allocator tags are 1..FI_ALLOC_COUNT. */
static int fault_site_tag_valid(uint32_t alloc_tag)
{
    return (alloc_tag >= 1u && alloc_tag <= FI_ALLOC_COUNT);
}

/* Which allocator OWNS each site. Every site is reached through exactly one
 * allocator, so an arm naming the other one could never be claimed -- it
 * would sit there looking armed while the test it belongs to concluded the
 * branch was covered. Validation is centralized here so the dispatcher, the
 * arm path, and any future site all agree on one mapping. Indexed by
 * FAULT_SITE_* id; entry 0 (FAULT_SITE_NONE) is deliberately 0 = no owner,
 * which is what makes "arm site NONE" an error rather than a silent
 * disarm-that-reports-success. */
static const uint32_t s_fault_site_owner[FAULT_SITE_MAX + 1] = {
    [FAULT_SITE_NONE]                = 0u,
    [FAULT_SITE_EXEC_ARGV_TABLE]     = FI_ALLOC_KMALLOC,
    [FAULT_SITE_EXEC_PRIVATE_FRAMES] = FI_ALLOC_KMALLOC,
    [FAULT_SITE_PEB_FRAMES]          = FI_ALLOC_PMM,
    [FAULT_SITE_FORK_CHILD_PML4]     = FI_ALLOC_PMM,
};

/* Layer 1 for the site enum: appending a FAULT_SITE_* id without bumping
 * FAULT_SITE_MAX would size this table one short, and the new id would then
 * read owner 0 -- which fails CLOSED (fault_site_arm_set refuses a tag that
 * matches no allocator) but silently, so the site would look armable in
 * review and never fire. Pin the highest id to the bound instead; a new site
 * must update BOTH lines, and forgetting is a build error rather than a test
 * that quietly proves nothing. */
_Static_assert(FAULT_SITE_FORK_CHILD_PML4 == FAULT_SITE_MAX,
               "append a FAULT_SITE_* id and bump FAULT_SITE_MAX together, "
               "then give it a row in s_fault_site_owner[]");

uint32_t fault_site_owner(uint32_t site)
{
    if (site > FAULT_SITE_MAX)
        return 0u;
    return s_fault_site_owner[site];
}

int fault_site_claim(uint32_t alloc_tag)
{
    struct task   *t  = task_current();
    struct thread *th = thread_current();
    uint32_t *slot, want, armed;

    if (!t || !th || th->fault_site_current == FAULT_SITE_NONE)
        return 0;
    if (!fault_site_tag_valid(alloc_tag))
        return 0;

    slot  = &t->fault_site_arm[FI_ARM_SLOT(alloc_tag)];
    want  = FI_ARM_PACK(alloc_tag, th->fault_site_current);
    armed = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (armed != want)
        return 0;

    /* Single shot: whoever wins the exchange owns the injection. */
    return __atomic_compare_exchange_n(slot, &armed, 0u, false,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_RELAXED) ? 1 : 0;
}

int fault_site_arm_set(uint32_t alloc_tag, uint32_t site)
{
    struct task *t = task_current();

    if (!t || !fault_site_tag_valid(alloc_tag))
        return -1;
    /* Fail CLOSED on anything that could not fire. FAULT_SITE_NONE and a
     * site owned by the other allocator are both rejected rather than
     * stored: accepting them would return success while installing nothing
     * claimable, and the caller's later "arm reads 0" consumed-check would
     * then certify a branch that never executed. */
    if (fault_site_owner(site) != alloc_tag)
        return -1;
    /* Only THIS allocator's slot moves: a live arm on the other allocator
     * is an independent program and must survive. */
    __atomic_store_n(&t->fault_site_arm[FI_ARM_SLOT(alloc_tag)],
                     FI_ARM_PACK(alloc_tag, site), __ATOMIC_RELEASE);
    return 0;
}

uint32_t fault_site_arm_peek(uint32_t alloc_tag)
{
    struct task *t = task_current();

    if (!t || !fault_site_tag_valid(alloc_tag))
        return 0u;
    return __atomic_load_n(&t->fault_site_arm[FI_ARM_SLOT(alloc_tag)],
                           __ATOMIC_ACQUIRE);
}

void fault_site_arm_clear(uint32_t alloc_tag)
{
    struct task *t = task_current();

    if (t && fault_site_tag_valid(alloc_tag))
        __atomic_store_n(&t->fault_site_arm[FI_ARM_SLOT(alloc_tag)], 0u,
                         __ATOMIC_RELEASE);
}
#endif /* KERNEL_TESTS */

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
