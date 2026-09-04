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
#include "kernel/sched/apc.h"
#include "kernel/sched/event.h"

/* Forward declare -- avoid circular include with irql.h */
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

/* Per-CPU threaded DPC pending list (separate from the DISPATCH_LEVEL queue).
 * head + pending are mutated UNDER the per-CPU DPC_QLOCK (producer hand-off and
 * worker pop) -- never lock-free -- so the ISR producer and PASSIVE worker can
 * never corrupt the list or lose a node. Cache-line aligned + padded so a
 * per-DPC write to one CPU's slot cannot false-share with another CPU's. */
struct dpc_threaded_slot {
    KDPC              *head;       /* threaded DPC list head (FIFO pop point)    */
    KDPC              *tail;       /* threaded DPC list tail (FIFO append point) */
    volatile uint32_t  pending;   /* 1 while the list is non-empty (acq/rel)    */
    uint8_t            _pad[DPC_CACHELINE - 2 * sizeof(KDPC *) - sizeof(uint32_t)];
} __attribute__((aligned(DPC_CACHELINE)));
_Static_assert(sizeof(struct dpc_threaded_slot) == DPC_CACHELINE,
               "dpc_threaded_slot must occupy exactly one cache line");
static struct dpc_threaded_slot threaded_q[MAX_CPUS];

/* Count of threaded DPC callbacks that have been popped off threaded_q but whose
 * routine() has not yet returned. Incremented UNDER DPC_QLOCK at the pop (while
 * the node is off all lists), decremented after the routine runs at PASSIVE.
 * KeFlushQueuedDpcs waits for this to reach 0 so a teardown caller cannot free a
 * KDPC/context while its callback is still executing -- the threaded_q pending
 * flags only cover queued-but-not-yet-running work, not the off-list in-flight
 * window. Lock-free atomic so the flush observer reads it without DPC_QLOCK. A
 * single global counter suffices: one worker drains all CPUs today, so it is
 * 0 or 1; it generalizes to a sum if per-CPU workers land (section 17). */
static volatile uint32_t s_in_flight_threaded;

/* The threaded-DPC worker's own task, recorded at worker entry. KeFlushQueuedDpcs
 * uses it to reject a self-deadlocking call from inside a threaded DPC callback
 * (which runs in this task at PASSIVE): the worker already counts the running
 * callback in s_in_flight_threaded, so a flush from it would wait for itself.
 * Pointer compare only -- struct task stays opaque here. */
struct task;
extern struct task *task_current(void);
static struct task *s_worker_task;

/* Worker idle event: the ISR producer event_set()s it (ISR-safe) on hand-off;
 * the worker idles on event_wait_timeout (yield-poll -- it never enqueues onto
 * the event waiter queue, so it is safe against event_set() from multiple-CPU
 * ISR drain paths, unlike permanent event_wait whose waiter-array mutation is
 * not yet SMP-synchronized). dpc_watchdog_tick re-signals every tick while any
 * list is pending so a missed signal self-heals. AUTO_RESET. */
static event_t s_dpc_worker_event;

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
/* Per-routine overrun accounting, so a chronically slow DPC is reported once
 * with a count instead of once per occurrence.
 *
 * WHY: dpc_watchdog_single_overrun() warned unconditionally on every crossing.
 * A single slow routine therefore owned the serial log -- a 2026-07-27 capture
 * carried 1776 DPC-watchdog lines, 65% of all warnings in the boot, and a
 * 2026-07-28 WHPX capture showed the same routine (quota_pressure_tick)
 * overrunning continuously from Phase 3 onward. The overrun itself is a real
 * defect and is owned separately; suppressing the SPAM must not suppress the
 * SIGNAL, so the first crossings still warn and the totals are still reported.
 *
 * SMP: this lives inside s_wd[cpu_id], which is only ever touched by the CPU it
 * indexes -- drain_queue() is reached exclusively via smp_this_cpu()->cpu_id
 * from both of its callers (verified 2026-07-28), so no lock is needed and none
 * is taken. That matters here because the reporting path calls klog(), and
 * holding a spinlock across serial output is forbidden. */
#define DPC_WD_TRACKED_ROUTINES 8   /* distinct offenders remembered per CPU */
#define DPC_WD_WARN_BURST       3   /* per-routine lines before collapsing    */

struct dpc_wd_offender {
    void    *pc;                 /* offending routine, NULL = free slot         */
    uint32_t count;              /* total crossings observed on this CPU        */
    uint32_t worst_us;           /* longest single run seen                     */
    uint32_t warned;             /* lines already emitted for this routine      */
};

struct dpc_watchdog {
    uint32_t budget;             /* remaining per-tick DPC tokens (carry-over)  */
    uint32_t tick_dpcs;          /* DPCs dispatched in the current tick         */
    uint32_t consec_over_depth;  /* consecutive ticks depth > WARN_DEPTH        */
    /* COUNT of NON-threaded DPC routine()s currently executing on this CPU
     * (popped off the queue, lock dropped, callback running). A counter, not a
     * flag: normal DPCs run at DISPATCH_LEVEL, which does not mask the CLOCK-
     * level timer ISR, so a timer-ISR drain can run a nested DPC on top of a
     * running one -- a boolean cleared by the inner callback would falsely
     * report the outer as done. fetch_add UNDER DPC_QLOCK at dequeue (so a
     * remote KeFlushQueuedDpcs sampling the queue head + this count under the
     * same lock gets a consistent snapshot), fetch_sub after the routine. The
     * completion barrier waits for this to reach 0. (Threaded callbacks use the
     * global s_in_flight_threaded.) */
    volatile uint32_t in_flight;
    /* Deferred depth-warn flag: set to 1 when an insert crosses
     * DPC_QUEUE_WARN_DEPTH; consumed by dpc_watchdog_tick and cleared by
     * KeRemoveQueueDpc (cancel-to-empty). The two stores and the tick's clear
     * run UNDER DPC_QLOCK(cpu_id); the tick ALSO does a lock-free __atomic
     * acquire-load fast path first so an empty tick pays no lock, so all
     * accesses use __atomic (RELEASE store / ACQUIRE load). dpc_watchdog_tick
     * emits the klog AFTER dropping the lock, keeping the warning off the
     * ISR/DIRQL insert path (klog busy-waits the UART and must never run at an
     * arbitrary device interrupt's IRQL). */
    uint32_t warn_pending;
    uint8_t  over_budget_warned; /* monopolization already warned this tick     */
    /* Pad + align to one cache line: drain_queue writes budget/tick_dpcs/
     * in_flight per dispatched DPC (and warn_pending is written under the queue
     * lock), so adjacent CPUs' records must not false-share (matches the
     * cpu_queues / queue_lock_slots discipline). */
    uint8_t  _pad[DPC_CACHELINE - 21];
} __attribute__((aligned(DPC_CACHELINE)));
_Static_assert(sizeof(struct dpc_watchdog) == DPC_CACHELINE,
               "dpc_watchdog must occupy exactly one cache line (no false-share)");
static struct dpc_watchdog s_wd[MAX_CPUS];

/* Offender table lives OUTSIDE struct dpc_watchdog on purpose. That struct is
 * pinned to exactly one cache line by a _Static_assert because it is written on
 * every drain and must not false-share between CPUs; this table is touched only
 * when a DPC actually overruns (rare), so putting it on the hot line would cost
 * the fast path to serve the slow one. Same per-CPU ownership rule, its own
 * aligned storage. */
struct dpc_wd_offender_table {
    struct dpc_wd_offender slot[DPC_WD_TRACKED_ROUTINES];
    uint32_t overflow;           /* crossings from routines past the table      */
} __attribute__((aligned(DPC_CACHELINE)));
static struct dpc_wd_offender_table s_wd_off[MAX_CPUS];

/* 0 = warn-only (default), 1 = escalate a single-DPC overrun to KeBugCheckEx. */
static int s_dpc_wd_strict;
/* Precomputed 100us threshold in TSC cycles; 0 = invariant TSC freq unavailable
 * (the single-DPC timing watchdog then stays OFF -- depth/budget still run). */
static uint64_t s_dpc_single_threshold_cycles;

/* Last APC-starvation event count reported by the watchdog tick (BSP-only). */
static uint32_t s_apc_starv_reported;

void dpc_watchdog_set_strict(int on)   { s_dpc_wd_strict = on ? 1 : 0; }
int  dpc_watchdog_strict_enabled(void) { return s_dpc_wd_strict; }

/* Compute the single-DPC cycle threshold + seed budgets. Call after TSC
 * calibration (dpc_init, post-Phase-3). Safe to call again (idempotent). */
void dpc_watchdog_init(void)
{
    uint64_t hz = boot_timing_tsc_freq();   /* 0 if not calibrated */
    uint32_t i;

    /* Arm the single-DPC timing watchdog ONLY when ALL of: a calibrated TSC freq,
     * RDTSCP on EVERY online CPU, and an INVARIANT TSC. RDTSCP is gated on
     * cpu_feature_global_has() -- the AND across all online CPUs -- because it
     * is the instruction that #UDs on a CPU that lacks it (an AP-probed optional
     * feature), so a BSP-has/AP-lacks skew must disable timing everywhere rather
     * than fault the skewed AP. CPU_FEATURE_TSC_INV (invariant/frequency-stable
     * TSC) is a platform-uniform property and is NOT in the AP-probe set, so the
     * BSP cpu_has() read is representative -- without it the cycle threshold
     * would drift and produce false WARN/bugchecks. Depth + budget watchdogs
     * still run when timing is off (no TSC needed). */
    s_dpc_single_threshold_cycles =
        (hz && cpu_feature_global_has(CPU_FEATURE_RDTSCP)
             && cpu_has(CPU_FEATURE_TSC_INV))
            ? (hz / 1000000ULL) * (uint64_t)DPC_WATCHDOG_SINGLE_DPC_US
            : 0;
    for (i = 0; i < MAX_CPUS; i++) {
        s_wd[i].budget = DPC_BUDGET_PER_TICK;
        s_wd[i].tick_dpcs = 0;
        s_wd[i].consec_over_depth = 0;
        s_wd[i].over_budget_warned = 0;
        /* warn_pending is NOT reset here: static storage zero-inits it before
         * any producer runs, and once queues are live the flag is touched only
         * under DPC_QLOCK -- an unlocked clear here could drop a live crossing
         * an ISR already armed (this init is documented idempotent). */
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
    /* Split conversion: whole-second part (cycles/hz)*1e6 plus the sub-second
     * remainder ((cycles%hz)*1e6)/hz. Overflow-safe (cycles%hz < hz, so the
     * remainder product stays well under 2^64 for any real TSC) AND precise for
     * the sub-second 100us-999ms overruns this watchdog targets -- a plain
     * (cycles/hz)*1e6 would report 0 us for any overrun under one second. */
    uint64_t us = hz ? ((cycles / hz) * 1000000ULL
                        + ((cycles % hz) * 1000000ULL) / hz)
                     : 0;

    /* Account first, then decide whether to speak. Per-CPU table, no lock (see
     * struct dpc_wd_offender). A routine that has already had its say is
     * counted silently and surfaces in the end-of-boot roll-up instead. */
    int suppressed = 0;
    if (cpu_id < MAX_CPUS) {
        struct dpc_wd_offender_table *tb = &s_wd_off[cpu_id];
        struct dpc_wd_offender *slot = (struct dpc_wd_offender *)0;
        for (uint32_t i = 0; i < DPC_WD_TRACKED_ROUTINES; i++) {
            if (tb->slot[i].pc == routine_pc) { slot = &tb->slot[i]; break; }
            if (!tb->slot[i].pc && !slot)      slot = &tb->slot[i];
        }
        if (slot) {
            if (!slot->pc) slot->pc = routine_pc;
            slot->count++;
            if ((uint32_t)us > slot->worst_us) slot->worst_us = (uint32_t)us;
            if (slot->warned >= DPC_WD_WARN_BURST)
                suppressed = 1;
            else
                slot->warned++;
        } else {
            /* Table full: count it so the roll-up can say the report is partial
             * rather than silently under-reporting. */
            tb->overflow++;
            suppressed = 1;
        }
    }

    if (!suppressed)
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
 * SCOPE: services only the TICKING CPU (smp_this_cpu), which is the BSP today
 * (AP LAPIC timers are masked, BSP-only heartbeat). The BSP tick drains and
 * bookkeeps ONLY the BSP queue. An AP queue is NOT swept by the BSP tick: it
 * drains only when that AP lowers IRQL on its own, and its budget/warn_pending
 * therefore get no BSP coverage. Extending depth/budget/warn bookkeeping to AP
 * queues (a BSP cross-CPU sweep, needing the per-CPU budget made atomic vs the
 * AP's own drain spend) is deferred -- a tracked AP-watchdog-coverage item. */
/* End-of-boot roll-up of DPC overruns, one line per offending routine.
 *
 * The per-routine burst limit above stops a chronic offender from owning the
 * log, but suppression that loses the signal is worse than the spam: an
 * operator must still be able to see WHICH routine overran, HOW OFTEN, and how
 * BAD the worst case was. This prints exactly that, once, and says so when the
 * per-CPU table overflowed rather than under-reporting silently.
 *
 * Read-only aggregation across the per-CPU tables. Safe to call from the boot
 * path after DPCs have been running; costs one pass over MAX_CPUS x 8 slots. */
void dpc_watchdog_report(void)
{
    uint32_t routines = 0;
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        for (uint32_t i = 0; i < DPC_WD_TRACKED_ROUTINES; i++) {
            struct dpc_wd_offender *o = &s_wd_off[c].slot[i];
            if (!o->pc || !o->count) continue;
            if (!routines)
                klog(LOG_INFO, "dpc", "--- DPC overrun summary (threshold %u us) ---",
                     (uint64_t)DPC_WATCHDOG_SINGLE_DPC_US);
            routines++;
            klog(LOG_WARN, "dpc",
                 "  CPU %u %p: %u overrun(s), worst %u us",
                 (uint64_t)c, o->pc, (uint64_t)o->count, (uint64_t)o->worst_us);
        }
        if (s_wd_off[c].overflow)
            klog(LOG_WARN, "dpc",
                 "  CPU %u: %u further overrun(s) from untracked routines "
                 "(more than %u distinct offenders)",
                 (uint64_t)c, (uint64_t)s_wd_off[c].overflow,
                 (uint64_t)DPC_WD_TRACKED_ROUTINES);
    }
    if (!routines)
        klog(LOG_INFO, "dpc", "DPC overruns: none");
}

void dpc_watchdog_tick(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint32_t id, depth, refilled;

    if (!cpu || cpu->cpu_id >= MAX_CPUS)
        return;
    id = cpu->cpu_id;

    /* Instant depth-crossing warning. An insert that pushed the queue to exactly
     * DPC_QUEUE_WARN_DEPTH armed warn_pending under DPC_QLOCK -- it may run up to
     * DIRQL (any device ISR) where klog must not busy-wait the UART, so it never
     * logs there. Drain it here: read+clear under the lock (the flag's only
     * writers -- insert and cancel-to-empty -- also hold it), klog after the
     * unlock. This is the once-per-tick DPC diagnostic site shared with the
     * sustained-depth + budget warnings below; like them it necessarily runs in
     * the timer-ISR context (the DPC subsystem has no non-interrupt periodic
     * context). The fix's win is removing the PER-INSERT klog at arbitrary
     * device DIRQL, not eliminating the bounded once-per-tick heartbeat log. */
    if (__atomic_load_n(&s_wd[id].warn_pending, __ATOMIC_ACQUIRE)) {
        /* Lock-free acquire-load fast path: the common tick (no crossing armed)
         * pays NO DPC_QLOCK, preserving the empty-tick lock-free path. Only when
         * a crossing IS pending do we take the lock to recheck+clear (a
         * cancel-to-empty may have cleared it since the load) and emit off-lock. */
        uint64_t irq_flags;
        int crossed;
        spin_lock_irqsave(DPC_QLOCK(id), &irq_flags);
        crossed = (int)__atomic_load_n(&s_wd[id].warn_pending, __ATOMIC_RELAXED);
        __atomic_store_n(&s_wd[id].warn_pending, 0, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(DPC_QLOCK(id), irq_flags);
        if (crossed)
            klog(LOG_WARN, "dpc",
                 "CPU %u DPC queue depth reached %u (possible starvation)",
                 (uint64_t)id, (uint64_t)DPC_QUEUE_WARN_DEPTH);
    }

    /* Sustained-depth warning (consecutive ticks over the depth threshold). */
    depth = __atomic_load_n(&cpu_queues[id].depth, __ATOMIC_RELAXED);
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

    /* APC starvation reporting: the ISR-safe KeInsertQueueApc only bumps an
     * atomic event counter; emit the warning here (log-safe timer-ISR context)
     * when it advances. One global last-seen is fine -- only the BSP ticks. */
    {
        uint32_t ev = apc_starvation_events();
        if (ev != s_apc_starv_reported) {
            klog(LOG_WARN, "apc",
                 "starvation watchdog: kernel APC queue reached depth %u (%u events)",
                 (uint64_t)APC_STARVATION_WARN_DEPTH, (uint64_t)ev);
            s_apc_starv_reported = ev;
        }
    }

    /* Threaded-DPC worker prompt-wake self-heal: if ANY CPU still has pending
     * threaded DPCs, re-signal the worker every tick. The worker idles on
     * event_wait_timeout (bounded yield-poll -- it never enqueues onto the
     * event waiter queue, so it stays SMP-safe against multi-CPU-ISR
     * event_set); this per-tick re-signal just shortens the wake latency below
     * the poll interval. A lock-free pending read is fine -- a missed 1 still
     * drains on the next poll/tick. */
    {
        /* Scan ALL slots, not [0, smp_cpu_count()): cpu_id slots can be sparse
         * (abandoned AP), and a dense scan would miss a live high slot's pending
         * threaded work and never re-signal the worker for it. */
        uint32_t c;
        for (c = 0; c < MAX_CPUS; c++) {
            if (__atomic_load_n(&threaded_q[c].pending, __ATOMIC_ACQUIRE)) {
                event_set(&s_dpc_worker_event);
                break;
            }
        }
    }
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
        __atomic_store_n(&cpu_queues[i].depth, 0u, __ATOMIC_RELAXED);
        cpu_queues[i].executed  = 0;
        cpu_queues[i].max_depth = 0;
        queue_lock_slots[i].lock.flag = 0;
        threaded_q[i].head        = (KDPC *)0;
        threaded_q[i].tail        = (KDPC *)0;
        threaded_q[i].pending     = 0;
    }
    event_init(&s_dpc_worker_event, "dpc_worker", EVENT_AUTO_RESET, 0);
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

/* Test/diagnostic: one consistent snapshot of cpu_id's normal DPC queue.
 *
 * The depth counter and the linked contents are read inside the SAME
 * DPC_QLOCK critical section, which is the whole point: any CPU may insert
 * into any CPU's queue (dpc_insert_core below), and raising local IRQL stops
 * only this CPU's drain, so a delta between two separately-locked reads of
 * q->depth is not a property of the depth bookkeeping. depth == list_len
 * within one snapshot is.
 *
 * The walk is bounded by DPC_SAMPLE_MAX_WALK so this helper cannot hold the
 * lock with interrupts off for an unbounded time; a walk that reaches the cap
 * (or one that meets a corrupt cycle) sets truncated, and the caller must
 * reject a truncated sample rather than compare its counts against depth.
 * Reports nothing about dpc->queued / dpc->queued_cpu: those live under the
 * lock of whichever queue owns the KDPC, which need not be this one. */
int dpc_sample_queue(uint32_t cpu_id, const KDPC *dpc,
                     struct dpc_queue_sample *out)
{
    struct dpc_queue *q;
    const KDPC *cur;
    uint64_t irq_flags;
    uint32_t len = 0, occ = 0, truncated = 0, depth;
    uint32_t tocc = 0, visited = 0;

    if (cpu_id >= MAX_CPUS || !out)
        return 0;

    q = &cpu_queues[cpu_id];

    spin_lock_irqsave(DPC_QLOCK(cpu_id), &irq_flags);
    /* ONE budget across BOTH walks, not one each: the cap is a bound on how long
     * this holds DPC_QLOCK with interrupts off, and two independently-capped
     * loops under a single acquire would make the real bound twice the one the
     * header advertises. */
    for (cur = q->head; cur; cur = cur->next) {
        if (visited >= DPC_SAMPLE_MAX_WALK) {
            truncated = 1;
            break;
        }
        visited++;
        len++;
        if (cur == dpc)
            occ++;
    }
    depth = __atomic_load_n(&q->depth, __ATOMIC_RELAXED);
    /* Same critical section, same lock: threaded_q[cpu_id] is covered by
     * DPC_QLOCK(cpu_id) exactly as the normal queue is (drain_queue moves a node
     * between the two while holding it once), so a caller can tell "off the
     * normal queue AND on the threaded list" from "off both" with no window in
     * between. Bounded by the same cap, sharing the truncated flag. */
    for (cur = threaded_q[cpu_id].head; cur; cur = cur->next) {
        if (visited >= DPC_SAMPLE_MAX_WALK) {
            truncated = 1;
            break;
        }
        visited++;
        if (cur == dpc)
            tocc++;
    }
    spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);

    out->depth                = depth;
    out->list_len             = len;
    out->occurrences          = occ;
    out->truncated            = truncated;
    out->threaded_occurrences = tocc;
    return 1;
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

/* Core insert. Does NOT call klog: if the queue reaches the warn depth it arms
 * a per-CPU warn_pending flag (emitted later by dpc_watchdog_tick) and reports
 * the CPU via *warn_cpu_out (>= 0) for INFORMATION only -- callers must never
 * log from it, since this path is callable up to DIRQL. This lets the timer/ISR
 * insert sites queue a DPC under their own spinlock with no serial I/O. */
/* Forward-progress bound for the insert retry loop. Each retry observes a
 * transient (an owner mid-clear, or a concurrent state change) that settles in a
 * bounded critical section, so a correctly-used DPC resolves in a handful of
 * spins. This cap is astronomically beyond any legitimate transient -- reaching
 * it means a corrupted/looping KDPC state, which must fail loud (the insert is
 * callable up to DIRQL, where an unbounded spin would hang the interrupt path)
 * rather than spin forever or silently drop the insert. Mirrors the
 * KeFlushQueuedDpcs completion-barrier cap -> bugcheck precedent. */
#define DPC_INSERT_MAX_SPINS  (1u << 24)

static int dpc_insert_core(KDPC *dpc, void *arg1, void *arg2,
                           uint32_t force_cpu, int *warn_cpu_out)
{
    uint32_t cpu_id;
    struct dpc_queue *q;
    uint64_t irq_flags;
    int warn_depth = 0;
    uint32_t spins = 0;

    if (warn_cpu_out)
        *warn_cpu_out = -1;

    for (;;) {
        if (++spins > DPC_INSERT_MAX_SPINS)
            KeBugCheckEx(BUGCHECK_DPC_WATCHDOG_VIOLATION, 0x5,
                         (uint64_t)(uintptr_t)dpc,
                         (uint64_t)dpc->queued_cpu, (uint64_t)spins);
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

        /* RELAXED atomics, not a plain ++. These writers are already
         * serialized by DPC_QLOCK, but pm_deep_idle_allowed() reads this field
         * LOCK-FREE from the idle path (src/kernel/pm_idle.c), and a plain
         * write racing an atomic read is undefined under the C memory model
         * however benign the emitted code looks. One access discipline for
         * this field, everywhere. */
        uint32_t new_depth = __atomic_add_fetch(&q->depth, 1u,
                                                __ATOMIC_RELAXED);
        if (new_depth > q->max_depth)
            q->max_depth = new_depth;
        /* Depth warning: arm a per-CPU pending flag UNDER this queue's lock --
         * the SAME lock dpc_watchdog_tick holds when it reads+clears the flag,
         * and KeRemoveQueueDpc holds on cancel-to-empty. NEVER klog here -- this
         * path is callable up to DIRQL where klog busy-waits the UART. */
        if (new_depth == DPC_QUEUE_WARN_DEPTH) {
            warn_depth = 1;
            __atomic_store_n(&s_wd[cpu_id].warn_pending, 1, __ATOMIC_RELEASE);
        }

        spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);

        /* warn_cpu_out is reported (informational) for callers that want the CPU
         * id; they must NOT log from it at their IRQL -- dpc_watchdog_tick emits
         * the warning once per tick. */
        if (warn_depth) {
            if (warn_cpu_out)
                *warn_cpu_out = (int)cpu_id;
        }
        return 1;
    }
}

/* Queue a DPC. Callable up to DIRQL, so the depth warning is NOT logged here --
 * dpc_insert_core arms a per-CPU pending flag that dpc_watchdog_tick emits once
 * per tick (klog must never busy-wait on the UART at an interrupt's IRQL). */
int KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2)
{
    return dpc_insert_core(dpc, arg1, arg2, MAX_CPUS /* honor cpu_target */, (int *)0);
}

/* Queue a DPC, reporting the warn-depth CPU id in *warn_cpu_out (else -1) for
 * callers that want it -- informational ONLY: the depth warning is emitted by
 * dpc_watchdog_tick once per tick, never by the caller (it may run up to DIRQL).
 * Honors dpc->cpu_target. Same return value as KeInsertQueueDpc. */
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
            __atomic_sub_fetch(&q->depth, 1u, __ATOMIC_RELAXED);
            /* If cancellation empties the queue, drop any pending depth-warn
             * flag under this lock: a deep queue that was cancelled (not drained)
             * is not a starvation event, and no later drain would service this
             * now-empty queue to consume the flag -- leaving it set would mis-
             * attribute a stale depth-64 warning to the next unrelated DPC. */
            if (!q->head)
                __atomic_store_n(&s_wd[cpu_id].warn_pending, 0, __ATOMIC_RELEASE);
            found = 1;
            break;
        }
        prev = cur;
        cur  = cur->next;
    }

    /* Not on the normal queue -- it may have been handed off to the threaded
     * list (still queued=1, owned, under this same DPC_QLOCK). Unlink it there
     * so cancellation/teardown can stop a pending threaded callback. */
    if (!found) {
        prev = (KDPC *)0;
        cur  = threaded_q[cpu_id].head;
        while (cur) {
            if (cur == dpc) {
                if (prev)
                    prev->next = cur->next;
                else
                    threaded_q[cpu_id].head = cur->next;
                if (threaded_q[cpu_id].tail == cur)
                    threaded_q[cpu_id].tail = prev;   /* maintain FIFO tail */
                cur->next       = (KDPC *)0;
                cur->queued_cpu = MAX_CPUS;
                cur->queued     = 0;
                if (!threaded_q[cpu_id].head)
                    __atomic_store_n(&threaded_q[cpu_id].pending, 0, __ATOMIC_RELEASE);
                found = 1;
                break;
            }
            prev = cur;
            cur  = cur->next;
        }
    }

    spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);

    return found;
}

/* ---- Threaded DPC worker ------------------------------------------------- */

static void dpc_thread_fn(void)
{
    /* Record our own task so KeFlushQueuedDpcs can detect a self-deadlocking
     * call from inside a threaded DPC callback (which runs in this task). */
    __atomic_store_n(&s_worker_task, task_current(), __ATOMIC_RELEASE);

    klog(LOG_DEBUG, "dpc", "threaded DPC worker started (drains all CPUs)");

    for (;;) {
        uint32_t any_work = 0;
        uint32_t ci;

        /* Scan ALL MAX_CPUS slots, NOT [0, smp_cpu_count()). smp_cpu_count() is
         * the COUNT of online CPUs (1 + online), but cpu_id slots can be SPARSE
         * when an AP is abandoned during bringup (slot k offline, slot k+1
         * online) -- a dense [0, count) scan would skip a live high slot's
         * threaded_q and strand its DPCs forever. Offline/empty slots have
         * pending==0 and are skipped by the load below, so scanning all slots
         * costs only MAX_CPUS atomic reads. */
        for (ci = 0; ci < MAX_CPUS; ci++) {
            uint32_t budget = DPC_THREADED_BATCH_LIMIT;

            if (!__atomic_load_n(&threaded_q[ci].pending, __ATOMIC_ACQUIRE))
                continue;
            any_work = 1;

            /* Pop ONE node per iteration UNDER DPC_QLOCK, clear queued WHILE the
             * node is off all lists (so it is never owned-but-unreachable), then
             * release the lock and run the routine at PASSIVE_LEVEL. Bounded by
             * DPC_THREADED_BATCH_LIMIT per pass so one CPU's list (or a
             * self-rearming threaded DPC) cannot starve the other CPUs. */
            while (budget--) {
                KDPC              *dpc;
                KDEFERRED_ROUTINE  routine;
                void              *ctx, *a1, *a2;
                uint64_t           irq_flags;

                spin_lock_irqsave(DPC_QLOCK(ci), &irq_flags);
                dpc = threaded_q[ci].head;
                if (!dpc) {
                    __atomic_store_n(&threaded_q[ci].pending, 0, __ATOMIC_RELEASE);
                    spin_unlock_irqrestore(DPC_QLOCK(ci), irq_flags);
                    break;
                }
                threaded_q[ci].head = dpc->next;
                if (!threaded_q[ci].head)
                    threaded_q[ci].tail = (KDPC *)0;   /* list emptied */
                dpc->next       = (KDPC *)0;
                /* Snapshot the callback state BEFORE publishing queued=0: once
                 * queued is cleared, a concurrent KeInsertQueueDpc on another
                 * CPU (a DIFFERENT DPC_QLOCK) may legally requeue this KDPC and
                 * overwrite routine/deferred_ctx/system_arg*, racing our read.
                 * Capture first, clear ownership last -- all under DPC_QLOCK(ci)
                 * (matches the normal-queue snapshot-before-clear ordering). */
                routine = dpc->routine;
                ctx     = dpc->deferred_ctx;
                a1      = dpc->system_arg1;
                a2      = dpc->system_arg2;
                dpc->queued_cpu = MAX_CPUS;
                dpc->queued     = 0;          /* off all lists -- snapshot taken */
                /* Mark in-flight BEFORE releasing the lock so there is no window
                 * where this node is off the list (pending may read 0) yet its
                 * callback is uncounted -- KeFlushQueuedDpcs must see in-flight. */
                __atomic_fetch_add(&s_in_flight_threaded, 1, __ATOMIC_ACQ_REL);
                spin_unlock_irqrestore(DPC_QLOCK(ci), irq_flags);

                if (routine)
                    routine(dpc, ctx, a1, a2);

                /* Callback complete -- clear in-flight so a flush waiting on this
                 * KDPC's quiesce can proceed (and free the context) safely. */
                __atomic_fetch_sub(&s_in_flight_threaded, 1, __ATOMIC_ACQ_REL);
            }
        }

        /* Idle wait on the producer-signaled event via event_wait_timeout, NOT
         * the permanent event_wait. event_wait_timeout yield-polls and never
         * enqueues onto the event waiter queue, so it does NOT touch the
         * (currently unsynchronized) num_waiters/waiter arrays -- safe against
         * event_set() fired concurrently from multiple-CPU ISR drain paths.
         * The producer event_set() + dpc_watchdog_tick's per-tick re-signal
         * make the poll break promptly. TRADEOFF: yield-polling burns a
         * scheduler slot when idle; a truly blocking worker awaits an SMP-safe
         * event_t (deferred item below) before switching to permanent
         * event_wait. */
        if (!any_work)
            event_wait_timeout(&s_dpc_worker_event, DPC_THREADED_WORKER_IDLE_MS);
    }
}

/* 0 = no worker yet, 1 = worker created (or being created). CAS-guarded so
 * exactly one caller wins the spin-up regardless of how many threaded DPCs are
 * initialized concurrently. */
static volatile int s_worker_started = 0;

void dpc_start_threads(void)
{
    /* Single worker thread drains threaded DPC queues for ALL CPUs.
     * Safe because threaded DPCs run at PASSIVE_LEVEL (no CPU affinity
     * requirement). If per-CPU workers are needed later, use IPI to
     * create tasks on each AP.
     *
     * Started unconditionally at boot (after the scheduler is up) so a
     * threaded DPC queued from ANY IRQL always has a worker to consume it --
     * a lazy "start on first PASSIVE init" would strand a threaded DPC that
     * was initialized above PASSIVE and then queued. The worker idles on
     * event_wait_timeout (yield-poll); the deferred SMP-safe-event_t item
     * replaces that with a true blocking wait to drop the idle CPU cost.
     * CAS-idempotent so a duplicate call (e.g. the unit test) is a no-op. */
    extern int task_create(void (*entry)(void), const char *name);
    int expected = 0;
    int tid;

    /* CAS 0->1: only the first caller proceeds to task_create. */
    if (!__atomic_compare_exchange_n(&s_worker_started, &expected, 1,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;  /* already started (or another caller is starting it) */

    tid = task_create(dpc_thread_fn, "dpc_thread");
    if (tid < 0) {
        /* Roll back so a later explicit dpc_start_threads() call can retry the
         * spin-up. Surface the degraded mode instead of logging a false
         * success. */
        __atomic_store_n(&s_worker_started, 0, __ATOMIC_RELEASE);
        klog(LOG_ERROR, "dpc",
             "threaded DPC worker creation FAILED -- threaded DPCs will NOT run");
        return;
    }
    klog(LOG_INFO, "dpc", "Threaded DPC worker started (all-CPU drain)");
}

/* Test/diagnostic: 1 once the threaded-DPC worker has been spun up. */
int dpc_worker_started(void)
{
    return __atomic_load_n(&s_worker_started, __ATOMIC_ACQUIRE);
}

/* Test/diagnostic: count of threaded DPC callbacks currently in flight (popped
 * off the list but still running). 0 when the worker is idle. */
uint32_t dpc_in_flight_threaded(void)
{
    return __atomic_load_n(&s_in_flight_threaded, __ATOMIC_ACQUIRE);
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
        __atomic_sub_fetch(&q->depth, 1u, __ATOMIC_RELAXED);

        routine  = dpc->routine;
        ctx      = dpc->deferred_ctx;
        a1       = dpc->system_arg1;
        a2       = dpc->system_arg2;
        threaded = dpc->threaded;   /* snapshot under the lock */

        q->executed++;                /* stats under the lock */

        /* Threaded DPCs: hand off to the threaded list UNDER THE SAME LOCK,
         * keeping dpc->queued=1 + queued_cpu=cpu_id so the KDPC is never
         * un-owned between the two lists -- this closes BOTH the producer/worker
         * race on the threaded list AND the double-owner window where a
         * concurrent KeInsertQueueDpc could re-insert a momentarily-unqueued
         * KDPC. The worker clears queued when it pops the node off all lists. */
        if (threaded) {
            /* FIFO append to the tail (DPCs execute in insertion order). */
            dpc->next = (KDPC *)0;
            if (threaded_q[cpu_id].tail)
                threaded_q[cpu_id].tail->next = dpc;
            else
                threaded_q[cpu_id].head = dpc;
            threaded_q[cpu_id].tail = dpc;
            __atomic_store_n(&threaded_q[cpu_id].pending, 1, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);
            event_set(&s_dpc_worker_event);   /* ISR-safe wake of the worker */
            dispatched++;
            continue;
        }

        /* Non-threaded: clear ownership, count this callback in-flight UNDER the
         * lock (so a KeFlushQueuedDpcs sampling head + in_flight under this lock
         * sees a consistent snapshot -- the DPC is off the queue but counted as
         * running), unlock, then run inline. fetch_add (not a flag) so a nested
         * timer-ISR drain on this CPU does not lose the outer callback's count. */
        dpc->next       = (KDPC *)0;
        dpc->queued_cpu = MAX_CPUS;   /* invalidate before clearing queued */
        dpc->queued     = 0;
        __atomic_fetch_add(&s_wd[cpu_id].in_flight, 1, __ATOMIC_ACQ_REL);
        spin_unlock_irqrestore(DPC_QLOCK(cpu_id), irq_flags);

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
        /* Callback returned: drop this CPU's in-flight count so a teardown flush
         * waiting on this normal DPC can proceed (and free the KDPC/context)
         * once the count reaches 0 (covers nested same-CPU drains). */
        __atomic_fetch_sub(&s_wd[cpu_id].in_flight, 1, __ATOMIC_ACQ_REL);

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

    /* CONTRACT (NT KeFlushQueuedDpcs): called at PASSIVE_LEVEL; flushes both the
     * normal per-CPU DPC queues AND the threaded-DPC worker, waiting for work
     * outstanding AT CALL TIME to complete. It does NOT prevent a producer from
     * queueing a new DPC after the wait samples empty -- a teardown that frees a
     * KDPC/context must STOP its producers (cancel timers, disable the device)
     * BEFORE calling this, exactly as on Windows. Both waits are bounded so a
     * self-rearming DPC cannot hang the caller forever.
     *
     * Entry validation (matches NT: PASSIVE_LEVEL, and never from a DPC
     * routine). Any DPC callback that calls KeFlushQueuedDpcs would wait on its
     * own in-flight count and hang to the cap; fail fast instead of letting it
     * look like a quiesce timeout. Two cases:
     *   - A NORMAL DPC callback runs at DISPATCH_LEVEL, so any caller above
     *     PASSIVE is illegal (also covers the at-DISPATCH self-flush). 0x4.
     *   - A THREADED DPC callback runs at PASSIVE in the worker task, so the
     *     IRQL check passes -- catch it by identity. 0x3. */
    {
        KIRQL irql = KeGetCurrentIrql();
        if (irql != PASSIVE_LEVEL)
            KeBugCheckEx(BUGCHECK_DPC_WATCHDOG_VIOLATION, 0x4, (uint64_t)irql, 0, 0);
    }
    {
        struct task *worker = __atomic_load_n(&s_worker_task, __ATOMIC_ACQUIRE);
        if (worker && task_current() == worker)
            KeBugCheckEx(BUGCHECK_DPC_WATCHDOG_VIOLATION, 0x3, 0, 0, 0);
    }

    /* Drain our own queue at DISPATCH_LEVEL via KiDispatchDpc (which raises to
     * DISPATCH, drains one batch, lowers). DPC callbacks must run at
     * DISPATCH_LEVEL, not the flush caller's (PASSIVE) level. A bounded single
     * batch deliberately avoids spinning forever on a DPC that re-arms itself
     * during the flush. */
    {
        struct per_cpu_data *me = smp_this_cpu();
        if (me && me->cpu_id < MAX_CPUS)
            KiDispatchDpc();
    }

    /* Completion barrier: wait until, for every CPU, the normal queue AND the
     * threaded list are empty, no NORMAL callback is mid-execution on that CPU
     * (s_wd[cpu].in_flight), and no THREADED callback is in flight globally.
     * Each round samples a CPU's normal head, threaded head, and normal
     * in_flight TOGETHER under that CPU's DPC_QLOCK -- a lock-free sample would
     * race the drain_queue hand-off / dequeue (on x86 the cpu_queues head clear
     * becomes visible before the threaded_q pending publication or the in_flight
     * set, all done in one critical section, so an unlocked observer can see the
     * head empty in the gap and return while a DPC is mid-hand-off or about to
     * run). The global threaded in-flight count is read after the per-CPU
     * snapshots (the single worker increments it under DPC_QLOCK at pop).
     *
     * Yields (not pause) between rounds: the threaded worker is a SCHEDULED
     * PASSIVE thread, so on a single CPU only a yield lets it run; legal because
     * the contract pins the caller at PASSIVE_LEVEL. Scans ALL MAX_CPUS slots
     * (cpu_id slots can be sparse after an abandoned AP). */
    {
        extern void yield(void);
        uint32_t spin = 0;

        for (;;) {
            int      busy = 0;
            uint32_t inflight;

            for (cpu = 0; cpu < MAX_CPUS; cpu++) {
                uint64_t f;
                spin_lock_irqsave(DPC_QLOCK(cpu), &f);
                if (cpu_queues[cpu].head != (KDPC *)0 ||
                    threaded_q[cpu].head != (KDPC *)0 ||
                    s_wd[cpu].in_flight != 0)
                    busy = 1;
                spin_unlock_irqrestore(DPC_QLOCK(cpu), f);
                if (busy)
                    break;
            }

            inflight = __atomic_load_n(&s_in_flight_threaded, __ATOMIC_ACQUIRE);
            if (!busy && inflight == 0)
                break;   /* all work outstanding at call time has drained */

            if (++spin >= DPC_FLUSH_THREADED_YIELD_CAP) {
                /* Fail CLOSED. KeFlushQueuedDpcs is void (NT ABI -- a status
                 * return would break Win32/NT parity), so a caller CANNOT
                 * observe a soft failure: returning here would let teardown free
                 * a KDPC/context while threaded work is still queued or running
                 * (use-after-free). Reaching this cap means the caller did not
                 * stop its producers (a self-rearming DPC) or a callback is
                 * wedged -- a bug, never normal operation. Bugcheck rather than
                 * corrupt, exactly as NT bugchecks DPC_WATCHDOG_VIOLATION.
                 * Sub-case 0x2 distinguishes the flush-quiesce timeout from the
                 * single-DPC overrun (0x0). Does not return. */
                KeBugCheckEx(BUGCHECK_DPC_WATCHDOG_VIOLATION, 0x2,
                             (uint64_t)inflight,
                             (uint64_t)DPC_FLUSH_THREADED_YIELD_CAP, 0);
            }
            yield();
        }
    }
}
