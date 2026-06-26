/* ============================================================================
 * ktimer.c -- Lightweight tick-based kernel timer with DPC association
 *
 * Per-CPU singly-linked timer lists, mirroring the per-CPU layout of dpc.c.
 * A timer lives on the CPU that armed it; that CPU's timer ISR scans its list
 * each tick (ktimer_expire_current_cpu), fires due timers, and queues each
 * fired timer's DPC via KeInsertQueueDpc.
 *
 * LOCK ORDERING: the per-CPU ktimer lock is acquired strictly BEFORE the DPC
 * queue lock (one-way: dpc.c never takes a ktimer lock, so the nesting cannot
 * cycle into deadlock). The expiry scan queues each fired timer's DPC via
 * KeInsertQueueDpcEx WHILE STILL HOLDING the ktimer lock, so the unlink + the
 * "active = 0" publish + the DPC handoff are atomic with respect to a
 * concurrent KeCancelTimer (which also takes the ktimer lock). That closes the
 * free-before-queue lifetime gap: a canceller either cancels BEFORE the fire or
 * observes the DPC already QUEUED (it cannot free the timer in a window where
 * the DPC is about to be queued with the timer as arg1). Whether that queued
 * DPC has COMPLETED is a separate barrier owned by the DPC subsystem -- see the
 * LIFETIME note in ktimer.h. KeInsertQueueDpcEx is bounded, allocation-free,
 * and does NO serial I/O under the lock (it reports a queue-depth warning via
 * an out-param that we klog only after releasing the lock), so holding the
 * ktimer lock across it does not violate the lock-hold-time gate.
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
     * ISR's KeInsertQueueDpc. */
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

    /* Re-arming an active timer: cancel first (cross-CPU safe), then insert on
     * the service CPU. Never holds two ktimer locks -- KeCancelTimer fully
     * releases the owner CPU's lock before we acquire the service CPU's. */
    KeCancelTimer(timer);

    /* Arm on the BSP service CPU (not the caller's CPU): only the BSP's timer
     * ISR scans the list, so a timer armed from an AP would otherwise never
     * fire. The owner-cpu field still drives cross-CPU KeCancelTimer. */
    cpu = KTIMER_SERVICE_CPU;
    spin_lock_irqsave(KTIMER_LOCK(cpu), &irq_flags);
    timer->due_time_ticks = due_time_ticks;
    timer->period_ticks   = period_ticks;
    timer->dpc            = dpc;
    timer->cpu            = cpu;          /* set BEFORE active (cancel reads cpu) */
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
    /* Bitmask of CPUs whose DPC queue hit the warn depth this batch (timers can
     * target different CPUs via KeSetTargetProcessorDpc). klog each AFTER
     * releasing KTIMER_LOCK so no starvation warning is dropped. MAX_CPUS <= 32
     * fits a uint32_t. */
    uint32_t warn_mask = 0;
    _Static_assert(MAX_CPUS <= 32, "ktimer warn_mask is a uint32_t bitmask");

    /* Single pass with the DPC handoff DONE UNDER THE LOCK (see file header):
     * unlink/re-arm + active publish + KeInsertQueueDpc are atomic vs a
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
         * timer between the unlink and the DPC publish. KeInsertQueueDpcEx does
         * NO serial I/O under the lock -- it reports a depth warning via
         * warn_cpu, which we klog only after releasing KTIMER_LOCK. `t` stays a
         * valid caller-owned pointer until we release. */
        if (dpc) {
            int wc = -1;
            KeInsertQueueDpcEx(dpc, t, (void *)0, &wc);
            if (wc >= 0 && wc < (int)MAX_CPUS)
                warn_mask |= (1u << (uint32_t)wc);   /* record, do not drop */
        }
        if (!keep)
            t->active = 0;                    /* terminal publish (after queue) */
        fired++;
    }
    spin_unlock_irqrestore(KTIMER_LOCK(cpu), irq_flags);

    /* Deferred DPC-queue-depth warnings: klog every recorded CPU only after the
     * ktimer lock is released (serial I/O must never run under the spinlock).
     * Per-CPU so multi-target starvation is not collapsed to a single line. */
    for (uint32_t c = 0; warn_mask; c++, warn_mask >>= 1) {
        if (warn_mask & 1u)
            klog(LOG_WARN, "ktimer",
                 "DPC queue depth warning while expiring timers (CPU %u)",
                 (uint64_t)c);
    }

    return fired;
}
