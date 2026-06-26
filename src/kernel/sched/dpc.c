/* ============================================================================
 * dpc.c -- Deferred Procedure Call (DPC) per-CPU queue
 *
 * Implements the NT-style DPC model: ISRs queue lightweight callbacks that
 * execute at DISPATCH_LEVEL after the ISR returns.  The queue is per-CPU,
 * intrusive-linked (zero allocation at insert time), and protected by a
 * raw spinlock for ISR safety.
 *
 * Key invariants:
 *   - KeInsertQueueDpc() never blocks and never allocates memory
 *   - A KDPC can be in at most one CPU's queue at a time
 *   - Re-inserting an already-queued DPC updates args but does not re-queue
 *   - The queue is FIFO: DPCs execute in insertion order
 *   - DPC drain (KiDispatchDpc) is implemented in section 5
 * ============================================================================ */

#include "kernel/sched/dpc.h"
#include "kernel/sched/dpc_config.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/spinlock.h"
#include "kernel/smp.h"
#include "kernel/klog.h"
#include "kernel/barrier.h"
#include "kernel/bugcheck.h"
#include "kernel/boot_timing.h"
#include "kernel/cpuid.h"
#include "kernel/cpu_security.h"

/* Forward declare -- avoid circular include with irql.h */
extern KIRQL KeGetCurrentIrql(void);
extern void  KeRaiseIrql(KIRQL new_irql, KIRQL *old_irql);
extern void  KeLowerIrql(KIRQL old_irql);

/* ---- Per-CPU DPC queues -------------------------------------------------- */

/* Cache-line size for per-CPU storage padding (avoid false sharing between
 * CPUs on the ISR-hot DPC insert/drain path). */
#define DPC_CACHELINE 64

/* One queue per CPU, indexed by cpu_id.  Static allocation avoids any heap
 * dependency during early boot.  struct dpc_queue is cache-line-sized and
 * aligned (dpc.h) so adjacent CPUs' queues never share a line. */
static struct dpc_queue cpu_queues[MAX_CPUS];

/* Per-CPU spinlock protecting the DPC queue, each padded to its own cache line
 * so two CPUs acquiring their own lock do not bounce a shared line on the hot
 * path. The lock flag is the only field; the rest is padding. */
struct dpc_lock_slot {
    spinlock_t lock;
    char       _pad[DPC_CACHELINE - sizeof(spinlock_t)];
} __attribute__((aligned(DPC_CACHELINE)));
static struct dpc_lock_slot queue_lock_slots[MAX_CPUS];
_Static_assert(sizeof(struct dpc_lock_slot) == DPC_CACHELINE,
               "DPC lock slot must occupy exactly one cache line");

/* Accessor: pointer to CPU i's spinlock. */
#define DPC_QLOCK(i) (&queue_lock_slots[(i)].lock)

/* Per-CPU threaded DPC pending list (separate from DISPATCH_LEVEL queue) */
static KDPC *threaded_head[MAX_CPUS];
static volatile uint32_t threaded_pending[MAX_CPUS];

/* ---- DPC fairness budget + watchdog (section 14) ------------------------- *
 *
 * Per-CPU, written only by the owning CPU's drain/tick paths (no lock). The
 * single-DPC runtime watchdog (Bug Check 0x133 param 0x0) measures each DPC's
 * TSC-cycle duration against a precomputed threshold; warn by default, bugcheck
 * when dpc_watchdog_set_strict(1). The per-tick count budget is a token bucket
 * (refilled DPC_BUDGET_PER_TICK/tick, capped) that flags monopolization. The
 * cumulative ">=DISPATCH-time" case (param 0x1) is deferred -- it needs per-CPU
 * time-at-DISPATCH accounting at every current_irql write site (the IRQL-write-
 * surface centralization tracked in section 13). */
struct dpc_watchdog {
    uint32_t budget;             /* remaining per-tick DPC tokens (carry-over)  */
    uint32_t tick_dpcs;          /* DPCs dispatched in the current tick         */
    uint32_t consec_over_depth;  /* consecutive ticks depth > WARN_DEPTH        */
    uint8_t  over_budget_warned; /* monopolization already warned this tick     */
    uint8_t  _pad[3];
};
static struct dpc_watchdog s_wd[MAX_CPUS];

/* 0 = warn-only (default), 1 = escalate a single-DPC overrun to KeBugCheckEx. */
static int s_dpc_wd_strict;
/* Precomputed 100us threshold in TSC cycles; 0 = invariant TSC freq unavailable
 * (the single-DPC timing watchdog then stays OFF -- depth/budget still run). */
static uint64_t s_dpc_single_threshold_cycles;

void dpc_watchdog_set_strict(int on)   { s_dpc_wd_strict = on ? 1 : 0; }
int  dpc_watchdog_strict_enabled(void) { return s_dpc_wd_strict; }

/* Compute the single-DPC cycle threshold + seed budgets. Call after TSC
 * calibration (dpc_init, post-Phase-3). Safe to call again (idempotent). */
void dpc_watchdog_init(void)
{
    uint64_t hz = boot_timing_tsc_freq();   /* 0 if not calibrated */
    uint32_t i;

    /* Arm the single-DPC timing watchdog ONLY when an invariant TSC freq exists
     * AND EVERY online CPU has RDTSCP -- drain_queue times each DPC with
     * rdtscp_read(), which #UDs on a CPU lacking CPU_FEATURE_RDTSCP (it is an
     * AP-probed OPTIONAL feature, not required). cpu_feature_global_mask() is
     * the AND across all online CPUs (finalized before dpc_init, post-SMP), so
     * a BSP-has/AP-lacks skew leaves the timing watchdog off on ALL CPUs rather
     * than faulting the skewed AP. Depth + budget watchdogs still run (no TSC). */
    s_dpc_single_threshold_cycles =
        (hz && (cpu_feature_global_mask() & (1ULL << CPU_FEATURE_RDTSCP)))
            ? (hz / 1000000ULL) * (uint64_t)DPC_WATCHDOG_SINGLE_DPC_US
            : 0;
    for (i = 0; i < MAX_CPUS; i++) {
        s_wd[i].budget = DPC_BUDGET_PER_TICK;
        s_wd[i].tick_dpcs = 0;
        s_wd[i].consec_over_depth = 0;
        s_wd[i].over_budget_warned = 0;
    }
    klog(LOG_INFO, "dpc", "watchdog: single-DPC threshold %u us (%s), budget %u/tick",
         (uint64_t)DPC_WATCHDOG_SINGLE_DPC_US,
         s_dpc_single_threshold_cycles ? "TSC" : "no TSC -- timing off",
         (uint64_t)DPC_BUDGET_PER_TICK);
}

/* A single DPC ran over the 100us threshold: warn (always) + bugcheck (strict).
 * routine_pc identifies the offending DPC. */
static void dpc_watchdog_single_overrun(uint32_t cpu_id, uint64_t cycles,
                                        void *routine_pc)
{
    uint64_t hz = boot_timing_tsc_freq();
    uint64_t us = hz ? (cycles * 1000000ULL) / hz : 0;

    klog(LOG_WARN, "dpc",
         "watchdog: DPC %p on CPU %u ran %u us (> %u us threshold)",
         routine_pc, (uint64_t)cpu_id, us, (uint64_t)DPC_WATCHDOG_SINGLE_DPC_US);
    if (s_dpc_wd_strict)
        /* Win11 DPC_WATCHDOG_VIOLATION param 0x0: p1=0 sub-case, p2=offending
         * routine, p3=elapsed us, p4=threshold us. */
        KeBugCheckEx(BUGCHECK_DPC_WATCHDOG_VIOLATION, 0x0,
                     (uint64_t)(uintptr_t)routine_pc, us,
                     (uint64_t)DPC_WATCHDOG_SINGLE_DPC_US);
}

/* Per-timer-tick watchdog bookkeeping: refill the token budget (carry-over,
 * capped), and fire the sustained-depth warning when the queue stays above
 * DPC_QUEUE_WARN_DEPTH for DPC_DEPTH_WARN_TICKS consecutive ticks. Called from
 * the timer ISR (LAPIC + PIT) every tick, BEFORE the drain.
 *
 * SCOPE: services only the TICKING CPU (smp_this_cpu). AP LAPIC timers are
 * masked today (BSP-only heartbeat), and AP DPC queues likewise only drain from
 * that BSP tick -- so the watchdog covers exactly the CPU that actually drains
 * DPCs. Extending depth/budget bookkeeping to AP queues (a BSP cross-CPU sweep,
 * which would need the per-CPU budget made atomic vs the AP's own drain spend)
 * is deferred to the AP-timer / per-CPU threaded-DPC dispatch work -- a tracked
 * section-14 item. */
void dpc_watchdog_tick(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint32_t id, depth, refilled;

    if (!cpu || cpu->cpu_id >= MAX_CPUS)
        return;
    id = cpu->cpu_id;

    /* Sustained-depth warning (consecutive ticks over the depth threshold). */
    depth = cpu_queues[id].depth;
    if (depth > DPC_QUEUE_WARN_DEPTH) {
        s_wd[id].consec_over_depth++;
        if (s_wd[id].consec_over_depth == DPC_DEPTH_WARN_TICKS)
            klog(LOG_WARN, "dpc",
                 "watchdog: CPU %u DPC queue depth %u sustained > %u for %u ticks",
                 (uint64_t)id, (uint64_t)depth, (uint64_t)DPC_QUEUE_WARN_DEPTH,
                 (uint64_t)DPC_DEPTH_WARN_TICKS);
    } else {
        s_wd[id].consec_over_depth = 0;
    }

    /* Token-bucket refill: add PER_TICK tokens, cap at CARRYOVER_MAX so an idle
     * CPU cannot bank an unbounded burst allowance. */
    refilled = s_wd[id].budget + DPC_BUDGET_PER_TICK;
    if (refilled > DPC_BUDGET_CARRYOVER_MAX)
        refilled = DPC_BUDGET_CARRYOVER_MAX;
    s_wd[id].budget = refilled;
    s_wd[id].tick_dpcs = 0;
    s_wd[id].over_budget_warned = 0;
}

/* ---- Initialization ------------------------------------------------------ */

static int s_queues_ready;

/* Phase 1: initialize per-CPU queues BEFORE sti. No heap, no scheduler needed.
 * After this, ISRs can safely call KeInsertQueueDpc (queued but not drained
 * until dpc_drain_current_cpu runs in a timer ISR after Phase 3). */
void dpc_init_queues(void)
{
    uint32_t i;
    for (i = 0; i < MAX_CPUS; i++) {
        cpu_queues[i].head      = (KDPC *)0;
        cpu_queues[i].tail      = (KDPC *)0;
        cpu_queues[i].depth     = 0;
        cpu_queues[i].executed  = 0;
        cpu_queues[i].max_depth = 0;
        queue_lock_slots[i].lock.flag = 0;
        threaded_head[i]        = (KDPC *)0;
        threaded_pending[i]     = 0;
    }
    s_queues_ready = 1;
    klog(LOG_INFO, "dpc", "DPC queues initialized (%u CPUs)", (uint64_t)MAX_CPUS);
}

/* Phase 3: full init (legacy entry point). If queues already initialized
 * by dpc_init_queues(), this is a no-op for the queue setup. */
void dpc_init(void)
{
    if (!s_queues_ready)
        dpc_init_queues();
    dpc_watchdog_init();   /* TSC is calibrated by now -- seed threshold + budgets */
    klog(LOG_INFO, "dpc", "DPC subsystem ready (scheduler available)");
}

/* ---- Query --------------------------------------------------------------- */

struct dpc_queue *dpc_this_cpu_queue(void)
{
    return &cpu_queues[smp_this_cpu()->cpu_id];
}

struct dpc_queue *dpc_get_cpu_queue(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS)
        return (struct dpc_queue *)0;
    return &cpu_queues[cpu_id];
}

/* ---- KeInitializeDpc ----------------------------------------------------- */

void KeInitializeDpc(KDPC *dpc, KDEFERRED_ROUTINE routine, void *context)
{
    dpc->routine      = routine;
    dpc->deferred_ctx = context;
    dpc->system_arg1  = (void *)0;
    dpc->system_arg2  = (void *)0;
    dpc->next         = (KDPC *)0;
    dpc->queued       = 0;
    dpc->cpu_target   = DPC_TARGET_CURRENT;
    dpc->queued_cpu   = MAX_CPUS;            /* invalid until queued */
    dpc->importance   = MediumImportance;
    dpc->threaded     = 0;
}

/* ---- KeInitializeThreadedDpc --------------------------------------------- */

void KeInitializeThreadedDpc(KDPC *dpc, KDEFERRED_ROUTINE routine, void *context)
{
    KeInitializeDpc(dpc, routine, context);
    dpc->threaded = 1;
}

/* ---- KeSetTargetProcessorDpc --------------------------------------------- */

void KeSetTargetProcessorDpc(KDPC *dpc, uint32_t cpu_number)
{
    if (dpc && cpu_number < MAX_CPUS)
        dpc->cpu_target = cpu_number;
}

/* ---- KeSetImportanceDpc -------------------------------------------------- */

void KeSetImportanceDpc(KDPC *dpc, KDPC_IMPORTANCE importance)
{
    if (dpc)
        dpc->importance = importance;
}

/* ---- KeInsertQueueDpc ---------------------------------------------------- */

/* Core insert. Does NOT call klog: if the queue reaches the warn depth it
 * reports the CPU via *warn_cpu_out (>= 0) so the caller can log AFTER dropping
 * any lock it holds. This lets ktimer_expire_current_cpu queue a DPC while
 * holding the ktimer spinlock without dragging serial I/O under that lock. */
static int dpc_insert_core(KDPC *dpc, void *arg1, void *arg2,
                           uint32_t force_cpu, int *warn_cpu_out)
{
    uint32_t cpu_id;
    struct dpc_queue *q;
    uint64_t irq_flags;
    int warn_depth = 0;

    if (warn_cpu_out)
        *warn_cpu_out = -1;

    for (;;) {
        /* If already queued, the DPC lives on dpc->queued_cpu (resolved from
         * DPC_TARGET_CURRENT at insert time). NT keeps a queued DPC on its
         * original CPU; a re-insert only refreshes the arguments -- under the
         * lock of the queue it actually lives on, not the caller's current CPU.
         * This is the cross-CPU-correct re-insert path. */
        if (dpc->queued) {
            uint32_t qc = dpc->queued_cpu;
            if (qc >= MAX_CPUS) {
                /* Transient: a remove/drain on the owning CPU is mid-clear
                 * (queued_cpu invalidated, queued not yet 0). Spin until it
                 * settles rather than treating it as an insert we can drop --
                 * the clear runs under that CPU's lock with interrupts off and
                 * completes promptly. */
                __asm__ volatile("pause" ::: "memory");
                continue;
            }
            spin_lock_irqsave(DPC_QLOCK(qc), &irq_flags);
            if (dpc->queued && dpc->queued_cpu == qc) {
                dpc->system_arg1 = arg1;
                dpc->system_arg2 = arg2;
                spin_unlock_irqrestore(DPC_QLOCK(qc), irq_flags);
                return 0;
            }
            spin_unlock_irqrestore(DPC_QLOCK(qc), irq_flags);
            continue;   /* state changed under us -- re-evaluate from the top */
        }

        /* Resolve the concrete target CPU for a fresh insert. A caller-supplied
         * force_cpu (< MAX_CPUS) overrides dpc->cpu_target WITHOUT mutating it,
         * so timer DPCs can be pinned to the service CPU while leaving the
         * caller-owned cpu_target field untouched for any later non-timer use. */
        if (force_cpu < MAX_CPUS)
            cpu_id = force_cpu;
        else if (dpc->cpu_target == DPC_TARGET_CURRENT)
            cpu_id = smp_this_cpu()->cpu_id;
        else
            cpu_id = dpc->cpu_target;
        if (cpu_id >= MAX_CPUS)
            return 0;

        q = &cpu_queues[cpu_id];

        /* Lock the target queue -- ISR-safe (saves RFLAGS + cli). */
        spin_lock_irqsave(DPC_QLOCK(cpu_id), &irq_flags);

        /* Recheck under the lock. If the DPC became queued concurrently:
         *  - on THIS CPU (the common ISR-vs-thread race): refresh args, return 0;
         *  - on ANOTHER CPU, or transiently clearing (queued_cpu==MAX_CPUS):
         *    release and re-evaluate from the top. We never double-link and never
         *    drop the insert -- a transient clear settles on retry, and a genuine
         *    concurrent same-DPC insert from a different CPU (caller misuse, see
         *    the header precondition) resolves to the already-queued path. */
        if (dpc->queued) {
            if (dpc->queued_cpu == cpu_id) {
                dpc->system_arg1 = arg1;
                dpc->system_arg2 = arg2;
                spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);
                return 0;
            }
            spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);
            continue;
        }

        /* Confirmed not-queued under the lock -- link it and record its CPU. */
        dpc->system_arg1 = arg1;
        dpc->system_arg2 = arg2;
        dpc->next        = (KDPC *)0;
        dpc->queued_cpu  = cpu_id;
        dpc->queued      = 1;

        if (dpc->importance == HighImportance) {
            /* Head insert -- runs before existing DPCs */
            dpc->next = q->head;
            q->head   = dpc;
            if (!q->tail)
                q->tail = dpc;
        } else {
            /* Tail insert (default FIFO) */
            if (q->tail) {
                q->tail->next = dpc;
                q->tail       = dpc;
            } else {
                q->head = dpc;
                q->tail = dpc;
            }
        }

        q->depth++;
        if (q->depth > q->max_depth)
            q->max_depth = q->depth;
        if (q->depth == DPC_QUEUE_WARN_DEPTH)
            warn_depth = 1;

        spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);

        /* Report (do NOT log) the depth warning so the caller can klog AFTER
         * dropping any spinlock it holds. klog does serial I/O (busy-waits on
         * UART under its own lock) which must never run while a queue/timer
         * spinlock is held with interrupts disabled. */
        if (warn_depth && warn_cpu_out)
            *warn_cpu_out = (int)cpu_id;
        return 1;
    }
}

/* Queue a DPC, logging a depth warning inline (the common path). */
int KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2)
{
    int warn_cpu = -1;
    int r = dpc_insert_core(dpc, arg1, arg2, MAX_CPUS /* honor cpu_target */, &warn_cpu);
    if (warn_cpu >= 0)
        klog(LOG_WARN, "dpc",
             "CPU %u DPC queue depth reached %u (possible starvation)",
             (uint64_t)(uint32_t)warn_cpu, (uint64_t)DPC_QUEUE_WARN_DEPTH);
    return r;
}

/* Queue a DPC WITHOUT logging: if the warn depth is hit, *warn_cpu_out is set
 * to the CPU id (else -1) so a caller holding a spinlock can defer the klog
 * until after it unlocks. Honors dpc->cpu_target. Same return value as
 * KeInsertQueueDpc (1 newly queued, 0 already). */
int KeInsertQueueDpcEx(KDPC *dpc, void *arg1, void *arg2, int *warn_cpu_out)
{
    return dpc_insert_core(dpc, arg1, arg2, MAX_CPUS /* honor cpu_target */, warn_cpu_out);
}

/* Queue a DPC pinned to cpu_id on a FRESH insert, overriding dpc->cpu_target
 * WITHOUT mutating it (the caller-owned field is preserved). No-log; reports the
 * warn depth via *warn_cpu_out like KeInsertQueueDpcEx. NOTE: if the DPC is
 * ALREADY queued, this follows the existing already-queued path (updates args on
 * its current queue, returns 0) -- it does NOT move a queued DPC across CPUs.
 * Used by ktimer to pin timer DPCs to the service CPU, which relies on the
 * timer-DPC ownership precondition (the timer owns the DPC's queueing; the
 * caller must not independently queue it elsewhere). cpu_id >= MAX_CPUS falls
 * back to honoring cpu_target. */
int KeInsertQueueDpcOnCpu(KDPC *dpc, uint32_t cpu_id, void *arg1, void *arg2,
                          int *warn_cpu_out)
{
    return dpc_insert_core(dpc, arg1, arg2, cpu_id, warn_cpu_out);
}

/* ---- KeRemoveQueueDpc ---------------------------------------------------- */

int KeRemoveQueueDpc(KDPC *dpc)
{
    uint32_t cpu_id;
    struct dpc_queue *q;
    KDPC *prev, *cur;
    uint64_t irq_flags;
    int found = 0;

    /* If not queued, nothing to do */
    if (!dpc->queued)
        return 0;

    /* The DPC lives on dpc->queued_cpu (recorded at insert time, resolving
     * DPC_TARGET_CURRENT to the concrete CPU that queued it). Remove from
     * THAT queue regardless of which CPU calls KeRemoveQueueDpc -- the old
     * code re-resolved DPC_TARGET_CURRENT to the remover's CPU and silently
     * failed for cross-CPU cancellation. */
    cpu_id = dpc->queued_cpu;

    if (cpu_id >= MAX_CPUS)
        return 0;

    q = &cpu_queues[cpu_id];

    spin_lock_irqsave(DPC_QLOCK(cpu_id), &irq_flags);

    /* Recheck under lock: it may have drained or moved since the unlocked read. */
    if (!dpc->queued || dpc->queued_cpu != cpu_id) {
        spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);
        return 0;
    }

    /* Walk the queue to find and unlink */
    prev = (KDPC *)0;
    cur  = q->head;

    while (cur) {
        if (cur == dpc) {
            /* Unlink */
            if (prev)
                prev->next = cur->next;
            else
                q->head = cur->next;

            if (q->tail == cur)
                q->tail = prev;

            cur->next       = (KDPC *)0;
            cur->queued_cpu = MAX_CPUS;   /* invalidate before clearing queued */
            cur->queued     = 0;
            q->depth--;
            found = 1;
            break;
        }
        prev = cur;
        cur  = cur->next;
    }

    spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);

    return found;
}

/* ---- Threaded DPC worker ------------------------------------------------- */

static void dpc_thread_fn(void)
{
    extern void yield(void);
    extern uint32_t smp_cpu_count(void);

    klog(LOG_DEBUG, "dpc", "threaded DPC worker started (drains all CPUs)");

    for (;;) {
        /* Check all CPUs for pending threaded DPCs */
        uint32_t any_work = 0;
        uint32_t ncpus = smp_cpu_count();
        uint32_t ci;

        for (ci = 0; ci < ncpus && ci < MAX_CPUS; ci++) {
            if (!__atomic_load_n(&threaded_pending[ci], __ATOMIC_ACQUIRE))
                continue;

            any_work = 1;

            /* Drain threaded DPC list for this CPU at PASSIVE_LEVEL */
            while (threaded_head[ci]) {
                KDPC *dpc = threaded_head[ci];
                threaded_head[ci] = dpc->next;
                dpc->next = (KDPC *)0;

                if (dpc->routine)
                    dpc->routine(dpc, dpc->deferred_ctx,
                                 dpc->system_arg1, dpc->system_arg2);
            }

            __atomic_store_n(&threaded_pending[ci], 0, __ATOMIC_RELEASE);
        }

        if (!any_work)
            yield();
    }
}

void dpc_start_threads(void)
{
    /* Single worker thread drains threaded DPC queues for ALL CPUs.
     * Safe because threaded DPCs run at PASSIVE_LEVEL (no CPU affinity
     * requirement). If per-CPU workers are needed later, use IPI to
     * create tasks on each AP. */
    extern int task_create(void (*entry)(void), const char *name);
    int tid = task_create(dpc_thread_fn, "dpc_thread");
    if (tid < 0) {
        /* No worker means threaded DPCs queued by drain_queue would never run.
         * Surface the degraded mode instead of logging a false success. */
        klog(LOG_ERROR, "dpc",
             "threaded DPC worker creation FAILED -- threaded DPCs will NOT run");
        return;
    }
    klog(LOG_INFO, "dpc", "Threaded DPC worker started (all-CPU drain)");
}

/* ---- Core drain logic (shared by KiDispatchDpc and dpc_drain_current_cpu) */

static uint32_t drain_queue(uint32_t cpu_id)
{
    struct dpc_queue *q = &cpu_queues[cpu_id];
    uint32_t dispatched = 0;

    while (dispatched < DPC_BATCH_LIMIT) {
        KDPC *dpc;
        KDEFERRED_ROUTINE routine;
        void *ctx, *a1, *a2;
        uint8_t threaded;
        uint64_t irq_flags;

        spin_lock_irqsave(DPC_QLOCK(cpu_id), &irq_flags);

        dpc = q->head;
        if (!dpc) {
            spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);
            break;
        }

        q->head = dpc->next;
        if (!q->head)
            q->tail = (KDPC *)0;
        q->depth--;

        routine  = dpc->routine;
        ctx      = dpc->deferred_ctx;
        a1       = dpc->system_arg1;
        a2       = dpc->system_arg2;
        threaded = dpc->threaded;   /* snapshot under the lock */

        dpc->next       = (KDPC *)0;
        dpc->queued_cpu = MAX_CPUS;   /* invalidate before clearing queued */
        dpc->queued     = 0;
        q->executed++;                /* stats under the lock (was outside) */

        spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);

        /* Threaded DPCs: move to the threaded list instead of executing inline.
         * NOTE: this hand-off writes dpc->next after the DPC is marked un-queued,
         * which races a concurrent re-insert -- tracked for the threaded-DPC
         * pending-state fix in the threaded-DPC list-synchronization section. */
        if (threaded) {
            dpc->next = threaded_head[cpu_id];
            threaded_head[cpu_id] = dpc;
            __atomic_store_n(&threaded_pending[cpu_id], 1, __ATOMIC_RELEASE);
            dispatched++;
            continue;
        }

        /* Single-DPC runtime watchdog: time the routine in TSC cycles when an
         * invariant TSC threshold is available (else the timing watchdog is
         * off and only depth/budget run). rdtscp serializes, so the deltas do
         * not straddle the call. */
        if (routine) {
            uint64_t t0 = 0;
            uint32_t aux;
            if (s_dpc_single_threshold_cycles)
                t0 = rdtscp_read(&aux);
            routine(dpc, ctx, a1, a2);
            if (s_dpc_single_threshold_cycles) {
                uint64_t dt = rdtscp_read(&aux) - t0;
                if (dt > s_dpc_single_threshold_cycles)
                    dpc_watchdog_single_overrun(cpu_id, dt, (void *)(uintptr_t)routine);
            }
        }

        /* Per-tick token budget: spend one token; flag monopolization once per
         * tick when the budget is exhausted (the hard per-drain bound stays
         * DPC_BATCH_LIMIT -- this is a fairness diagnostic, not an enforced cap). */
        s_wd[cpu_id].tick_dpcs++;
        if (s_wd[cpu_id].budget) {
            s_wd[cpu_id].budget--;
        } else if (!s_wd[cpu_id].over_budget_warned) {
            s_wd[cpu_id].over_budget_warned = 1;
            klog(LOG_WARN, "dpc",
                 "watchdog: CPU %u exceeded per-tick DPC budget (%u this tick)",
                 (uint64_t)cpu_id, (uint64_t)s_wd[cpu_id].tick_dpcs);
        }

        dispatched++;
    }

    return dispatched;
}

/* ---- KiDispatchDpc -- drain at DISPATCH_LEVEL (for non-ISR callers) ----- */

void KiDispatchDpc(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint32_t cpu_id;
    KIRQL old_irql;
    uint32_t n;

    if (!cpu) return;
    cpu_id = cpu->cpu_id;
    if (cpu_id >= MAX_CPUS) return;
    if (!cpu_queues[cpu_id].head) return;

    /* Restore IRQL via irql_lower_deliver (NOT KeLowerIrql): we already drained
     * one batch here, so the restore must NOT trigger KeLowerIrql's drain-on-
     * lower (that would run a second batch and break the documented bounded
     * single-batch contract KeFlushQueuedDpcs relies on). irql_lower_deliver
     * lowers + delivers pending kernel APCs without any DPC drain and without
     * holding a per-CPU drain guard across the yieldable APC NormalRoutine. */
    KeRaiseIrql(DISPATCH_LEVEL, &old_irql);
    n = drain_queue(cpu_id);
    irql_lower_deliver(old_irql);

    if (n > 0)
        klog(LOG_DEBUG, "dpc", "dispatched %u DPCs on CPU %u",
             (uint64_t)n, (uint64_t)cpu_id);
}

/* ---- Lightweight drain for timer ISR (no IRQL management) --------------- */

uint32_t dpc_drain_current_cpu(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    if (!cpu) return 0;
    if (cpu->cpu_id >= MAX_CPUS) return 0;
    if (!cpu_queues[cpu->cpu_id].head) return 0;

    return drain_queue(cpu->cpu_id);
}

/* Cheap "is any DPC queued on this CPU?" probe -- a single queue-head read, no
 * lock. KeLowerIrql gates its DISPATCH-drain bracket (TPR reprogram + drain
 * call) on this so the common empty path (e.g. every syscall-entry IRQL lower)
 * pays only one byte/pointer read, not two LAPIC TPR MMIO writes. */
int dpc_current_cpu_has_pending(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    if (!cpu || cpu->cpu_id >= MAX_CPUS)
        return 0;
    return cpu_queues[cpu->cpu_id].head != (KDPC *)0;
}

/* ---- KeFlushQueuedDpcs --------------------------------------------------- */

void KeFlushQueuedDpcs(void)
{
    uint32_t cpu;
    uint32_t max_cpus = smp_cpu_count();

    /* Drain our own queue at DISPATCH_LEVEL via KiDispatchDpc (which raises to
     * DISPATCH, drains one batch, lowers). DPC callbacks must run at
     * DISPATCH_LEVEL, not the flush caller's (PASSIVE) level. A bounded single
     * batch deliberately avoids spinning forever on a DPC that re-arms itself
     * during the flush (NT flushes DPCs queued at call time, not "drain until
     * empty"). The full drain-all + in-flight completion barrier is tracked as
     * the DPC-targeting section's KeFlushQueuedDpcs completion-barrier item. */
    {
        struct per_cpu_data *me = smp_this_cpu();
        if (me && me->cpu_id < MAX_CPUS)
            KiDispatchDpc();
    }

    /* Spin-wait for other CPUs to finish their queues
     * (they drain on every timer tick via dpc_drain_current_cpu) */
    for (cpu = 0; cpu < max_cpus && cpu < MAX_CPUS; cpu++) {
        volatile uint32_t spin = 0;
        while (cpu_queues[cpu].head != (KDPC *)0 && spin < 100000) {
            __asm__ volatile ("pause");
            spin++;
        }
    }
}
