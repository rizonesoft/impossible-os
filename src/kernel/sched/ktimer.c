/* ============================================================================
 * ktimer.c -- Lightweight tick-based kernel timer with DPC association
 *
 * Per-CPU singly-linked timer lists (the array is retained for a future
 * per-CPU-tick KTIMER), but TODAY all timers are armed on KTIMER_SERVICE_CPU
 * (the BSP) because only the BSP timer ISR is unmasked. The BSP timer ISR scans
 * the service list each tick (ktimer_expire_current_cpu), fires due timers, and
 * queues each fired timer's DPC via KeInsertQueueDpcOnCpu, which force-pins it
 * to KTIMER_SERVICE_CPU so it lands on a queue that actually drains.
 *
 * LOCK ORDERING: the per-CPU ktimer lock is acquired strictly BEFORE the DPC
 * queue lock (one-way: dpc.c never takes a ktimer lock, so the nesting cannot
 * cycle into deadlock). The expiry scan queues each fired timer's DPC via
 * KeInsertQueueDpcOnCpu WHILE STILL HOLDING the ktimer lock, so the unlink + the
 * "active = 0" publish + the DPC handoff are atomic with respect to a
 * concurrent KeCancelTimer (which also takes the ktimer lock). That closes the
 * free-before-queue lifetime gap: a canceller either cancels BEFORE the fire or
 * observes the DPC already QUEUED (it cannot free the timer in a window where
 * the DPC is about to be queued with the timer as arg1). Whether that queued
 * DPC has COMPLETED is a separate barrier owned by the DPC subsystem -- see the
 * LIFETIME note in ktimer.h. KeInsertQueueDpcOnCpu is bounded, allocation-free,
 * and does NO serial I/O under the lock: a queue-depth crossing only arms a
 * per-CPU warn_pending flag (emitted later by dpc_watchdog_tick), never a klog
 * here, so holding the ktimer lock across it does not violate the lock-hold-time
 * gate.
 *
 * SERVICE CPU: the periodic timer heartbeat is BSP-only (AP LAPIC timers are
 * masked -- see lapic_timer_arm_oneshot), so only the BSP's timer ISR ever
 * calls ktimer_expire_current_cpu. Every timer is therefore armed on the BSP
 * (KTIMER_SERVICE_CPU) regardless of the arming CPU, so it is guaranteed to be
 * serviced -- a timer armed from an AP must never land on a list no ISR scans.
 * The per-CPU list array is retained for the eventual per-CPU-tick KTIMER.
 * ============================================================================ */

#include "kernel/sched/ktimer.h"
#include "kernel/sched/spinlock.h"
#include "kernel/smp.h"
#include "kernel/timer.h"
#include "kernel/klog.h"

/* Cache-line size for per-CPU storage padding (avoid false sharing between
 * CPUs on the ISR-hot expiry-scan path). Matches dpc.c DPC_CACHELINE. */
#define KTIMER_CACHELINE 64

/* The CPU whose timer ISR services kernel timers. The periodic heartbeat is
 * BSP-only (AP LAPIC timers are masked), so every timer is armed here and the
 * BSP's ISR is the sole expiry driver. Generalizes to per-CPU when every CPU's
 * LAPIC timer ticks (the full KTIMER upgrade). */
#define KTIMER_SERVICE_CPU 0u

/* One timer-list head per CPU, indexed by cpu_id. BSS-zero (NULL) by default;
 * ktimer_init_lists() makes the init explicit and logged. */
static kernel_timer_t *ktimer_heads[MAX_CPUS];

/* Per-CPU spinlock protecting that CPU's timer list, each padded to its own
 * cache line so two CPUs touching their own list never bounce a shared line. */
struct ktimer_lock_slot {
    spinlock_t lock;
    char       _pad[KTIMER_CACHELINE - sizeof(spinlock_t)];
} __attribute__((aligned(KTIMER_CACHELINE)));
static struct ktimer_lock_slot ktimer_lock_slots[MAX_CPUS];
_Static_assert(sizeof(struct ktimer_lock_slot) == KTIMER_CACHELINE,
               "ktimer lock slot must occupy exactly one cache line");

/* Accessor: pointer to CPU i's timer-list spinlock. */
#define KTIMER_LOCK(i) (&ktimer_lock_slots[(i)].lock)

/* ---- Initialization ------------------------------------------------------ */

void ktimer_init_lists(void)
{
    uint32_t i;
    for (i = 0; i < MAX_CPUS; i++) {
        ktimer_heads[i]            = (kernel_timer_t *)0;
        ktimer_lock_slots[i].lock.flag = 0;
    }
    klog(LOG_INFO, "ktimer", "Kernel timer lists initialized (%u CPUs)",
         (uint64_t)MAX_CPUS);
}

/* ---- KeInitializeTimer --------------------------------------------------- */

void KeInitializeTimer(kernel_timer_t *timer)
{
    if (!timer)
        return;
    timer->due_time_ticks = 0;
    timer->period_ticks   = 0;
    timer->dpc            = (KDPC *)0;
    timer->next           = (kernel_timer_t *)0;
    timer->cpu            = MAX_CPUS;     /* invalid until armed */
    timer->active         = 0;
}

/* ---- KeCancelTimer ------------------------------------------------------- */

int KeCancelTimer(kernel_timer_t *timer)
{
    uint64_t irq_flags;
    int was_active;

    if (!timer)
        return 0;

    /* Fully locked -- NO lock-free `active` fast path. Every armed timer lives
     * on the service CPU's list, so the canceller always takes KTIMER_LOCK
     * (KTIMER_SERVICE_CPU) and decides under it. This is what closes the
     * cancel-vs-expiry handoff race: the expiry path queues the DPC and clears
     * `active` entirely inside this same lock, so a canceller that observes
     * active==0 is GUARANTEED the DPC is already queued (KeFlushQueuedDpcs will
     * see it) -- there is no window where it can free the timer before the
     * ISR's KeInsertQueueDpcOnCpu. */
    spin_lock_irqsave(KTIMER_LOCK(KTIMER_SERVICE_CPU), &irq_flags);
    was_active = (int)timer->active;
    if (was_active) {
        kernel_timer_t **pp = &ktimer_heads[KTIMER_SERVICE_CPU];
        while (*pp && *pp != timer)
            pp = &(*pp)->next;
        if (*pp == timer)
            *pp = timer->next;           /* unlink */
        timer->next   = (kernel_timer_t *)0;
        timer->cpu    = MAX_CPUS;
        timer->active = 0;
    }
    spin_unlock_irqrestore(KTIMER_LOCK(KTIMER_SERVICE_CPU), irq_flags);
    return was_active;
}

/* ---- KeSetTimerEx -------------------------------------------------------- */

void KeSetTimerEx(kernel_timer_t *timer, uint64_t due_time_ticks,
                  uint64_t period_ticks, KDPC *dpc)
{
    uint64_t irq_flags;
    uint32_t cpu;

    if (!timer)
        return;

    /* Re-arming an active timer: cancel first, then insert on the service CPU.
     * Both KeCancelTimer and this insert take the SAME service-CPU lock
     * sequentially (cancel fully releases before we acquire), so two ktimer
     * locks are never held at once. */
    KeCancelTimer(timer);

    /* Arm on the BSP service CPU (not the caller's CPU): only the BSP's timer
     * ISR scans the list, so a timer armed from an AP would otherwise never
     * fire. The owner-cpu field is informational/forward-looking (per-CPU
     * KTIMER); KeCancelTimer locks the service list directly. */
    cpu = KTIMER_SERVICE_CPU;
    spin_lock_irqsave(KTIMER_LOCK(cpu), &irq_flags);
    timer->due_time_ticks = due_time_ticks;
    timer->period_ticks   = period_ticks;
    timer->dpc            = dpc;
    timer->cpu            = cpu;          /* all stores under the lock; active last */
    timer->next           = ktimer_heads[cpu];
    ktimer_heads[cpu]     = timer;
    timer->active         = 1;
    spin_unlock_irqrestore(KTIMER_LOCK(cpu), irq_flags);
}

/* ---- KeSetTimer (single-shot wrapper) ------------------------------------ */

void KeSetTimer(kernel_timer_t *timer, uint64_t due_time_ticks, KDPC *dpc)
{
    KeSetTimerEx(timer, due_time_ticks, 0, dpc);
}

/* ---- ktimer_expire_current_cpu ------------------------------------------- */

uint32_t ktimer_expire_current_cpu(void)
{
    /* Scan the SERVICE CPU's list: it is the only list that is ever populated
     * (KeSetTimerEx arms there) and the BSP is the only CPU whose timer ISR
     * calls this, so KTIMER_SERVICE_CPU == this CPU on the live path. Naming it
     * explicitly makes the function correct even if a non-service CPU calls it
     * (it then finds an empty list -- a harmless no-op). */
    uint32_t cpu = KTIMER_SERVICE_CPU;
    uint64_t now = system_get_ticks();
    uint32_t fired = 0;
    uint64_t irq_flags;

    /* Common-case fast path: no armed timers -> skip the lock + the cli-section
     * entirely. The head pointer is a naturally-aligned 8-byte read (atomic on
     * x86); a timer concurrently inserted by a cross-CPU KeSetTimerEx that we
     * miss here simply fires on the next tick (<=1 tick / ~10 ms later, never
     * lost -- due times are absolute). This keeps per-tick ISR cost near zero
     * when nothing is scheduled, which is the dominant case. */
    if (!ktimer_heads[cpu])
        return 0;

    /* Single pass with the DPC handoff DONE UNDER THE LOCK (see file header):
     * unlink/re-arm + active publish + KeInsertQueueDpcOnCpu are atomic vs a
     * concurrent KeCancelTimer. Periodic timers re-arm to the next period
     * boundary past `now` in O(1), so the scan strictly advances and never
     * re-fires a timer within this call -- no batch re-scan, no unbounded
     * catch-up loop. */
    spin_lock_irqsave(KTIMER_LOCK(cpu), &irq_flags);
    kernel_timer_t **pp = &ktimer_heads[cpu];
    while (*pp) {
        kernel_timer_t *t = *pp;
        if (now < t->due_time_ticks) {
            pp = &t->next;
            continue;
        }

        /* Due. Decide keep (periodic re-arm) vs unlink (single-shot/overflow). */
        int keep = 0;
        if (t->period_ticks) {
            /* Next period boundary strictly after `now`, computed in O(1)
             * (no per-missed-tick loop): align `now` down to a period boundary
             * then add one period. Bounded even when the timer fell millions of
             * periods behind. */
            uint64_t rem  = (now - t->due_time_ticks) % t->period_ticks;
            uint64_t next = (now - rem) + t->period_ticks;
            if (next > now) {                 /* normal case */
                t->due_time_ticks = next;     /* re-armed, stays linked */
                keep = 1;
            }
            /* else: pathological period near UINT64_MAX wrapped -- fall through
             * to unlink so the ISR cannot re-fire it every tick. */
        }

        KDPC *dpc = t->dpc;
        if (keep) {
            pp = &t->next;                    /* periodic: stays linked + active */
        } else {
            *pp     = t->next;                /* unlink (pp now aims past t) */
            t->next = (kernel_timer_t *)0;
            t->cpu  = MAX_CPUS;
            /* NOTE: `active` is cleared AFTER the DPC is queued below, so any
             * observer (under KTIMER_LOCK, since KeCancelTimer has no lock-free
             * path) that sees active==0 is guaranteed the DPC is already queued. */
        }

        /* Atomic handoff: queue while still holding the ktimer lock so a
         * concurrent KeCancelTimer (which also takes this lock) cannot free the
         * timer between the unlink and the DPC publish. KeInsertQueueDpcOnCpu
         * does NO serial I/O under the lock -- any depth warning is armed as a
         * per-CPU warn_pending flag and emitted serial-safe by dpc_watchdog_tick
         * once per tick, so nothing is logged at the timer ISR's IRQL here. `t`
         * stays a valid caller-owned pointer until we release. */
        if (dpc) {
            /* Pin the DPC to the service CPU (overriding any caller
             * KeSetTargetProcessorDpc WITHOUT mutating the caller's cpu_target).
             * The timer fires here and the service CPU drains its own DPC queue
             * immediately after this scan (dpc_drain_current_cpu, same ISR). An
             * idle AP has no guaranteed DPC drain trigger (its LAPIC timer is
             * masked and, absent a DPC IPI, nothing forces it to lower IRQL), so
             * an AP-targeted timer DPC would STRAND. Per the timer-DPC ownership
             * precondition
             * (ktimer.h) the caller must not independently queue the DPC, so it
             * is never already-queued on an AP here. */
            KeInsertQueueDpcOnCpu(dpc, KTIMER_SERVICE_CPU, t, (void *)0, (int *)0);
        }
        if (!keep)
            t->active = 0;                    /* terminal publish (after queue) */
        fired++;
    }
    spin_unlock_irqrestore(KTIMER_LOCK(cpu), irq_flags);

    return fired;
}
