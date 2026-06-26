/* ============================================================================
 * ktimer.h -- Lightweight tick-based kernel timer with DPC association
 *
 * Bridges kernel timer expiry to the DPC subsystem: arm a timer for an
 * absolute tick deadline (optionally periodic) with an associated KDPC, and
 * the timer ISR auto-queues that DPC when the deadline is reached -- no manual
 * KeInsertQueueDpc in a timer callback.
 *
 *   KeSetTimerEx(t, due, period, dpc):  arm on the BSP service list (today all
 *                      timers are pinned to KTIMER_SERVICE_CPU regardless of the
 *                      arming CPU, since only the BSP timer ISR scans)
 *   <timer ISR tick>:  ktimer_expire_current_cpu() fires due timers and queues
 *                      each DPC via KeInsertQueueDpcOnCpu(dpc, KTIMER_SERVICE_CPU,
 *                      t, NULL, NULL) so it lands on a queue that drains
 *   KeCancelTimer(t):  remove from its list, stop further DPC queueing
 *
 * SCOPE: this is the MINIMAL prerequisite for the full NT KTIMER. Due times are
 * raw UTS ticks (system_get_ticks()), not FILETIME/QPC, and every timer is
 * serviced on the BSP (the per-CPU list array is retained for the future
 * per-CPU-tick KTIMER, when timers will be serviced by their owner CPU). The
 * full KTIMER upgrade
 * (FILETIME due times, QPC-based expiry, timer coalescing, a global timer
 * table) lands with the kernel time-service work (the full KTIMER upgrade).
 *
 * OWNERSHIP (NT caller precondition, mirrors KDPC): a single kernel_timer_t
 * must not be armed/cancelled concurrently from more than one CPU. All armed
 * timers live on the BSP service-CPU list, so KeSetTimerEx/KeCancelTimer from
 * ANY CPU operate on that one list under its lock; the recorded owner `cpu`
 * generalizes to the per-CPU KTIMER but is always the service CPU today. Concurrent
 * same-timer operations from two CPUs are caller misuse.
 *
 * DPC OWNERSHIP (NT caller precondition): the timer owns the queueing of its
 * associated KDPC. On expiry the DPC is queued PINNED to the service CPU (so it
 * lands on the only queue that is guaranteed to drain), overriding any caller
 * KeSetTargetProcessorDpc without mutating the caller's cpu_target field. The
 * caller must NOT independently KeInsertQueueDpc a timer's DPC -- doing so (e.g.
 * onto an AP, whose DPC queue has no guaranteed drain trigger yet) is misuse,
 * the same class as queueing one KDPC twice.
 *
 * LIFETIME (NT caller precondition, mirrors KDPC): a caller-owned timer (and
 * its associated KDPC) must remain allocated until it is cancelled AND any
 * already-queued DPC has fully RUN -- KeCancelTimer does NOT dequeue or wait
 * for an in-flight DPC. The expiry path queues the DPC and clears `active`
 * atomically under the ktimer lock (and KeCancelTimer has no lock-free path),
 * so a canceller that sees the timer inactive is guaranteed the DPC is already
 * QUEUED. To also guarantee the DPC has COMPLETED before freeing a dynamically
 * allocated timer, call KeFlushQueuedDpcs AFTER cancelling: it is now a true
 * in-flight completion barrier (waits for already-queued/running normal AND
 * threaded callbacks to finish), so once producers are stopped/cancelled it is
 * safe teardown for a fired timer's DPC (which takes the timer as arg1). It does
 * NOT stop NEW production -- cancel the timer first. The common case -- static /
 * subsystem-lifetime timers -- needs no flush.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/dpc.h"

/* ---- kernel_timer_t ------------------------------------------------------ */

typedef struct _kernel_timer {
    uint64_t              due_time_ticks; /* absolute system_get_ticks() expiry */
    uint64_t              period_ticks;   /* 0 = single-shot; else re-arm interval */
    KDPC                 *dpc;            /* DPC queued on expiry (NULL = none) */
    struct _kernel_timer *next;          /* per-CPU list link (NULL = not listed) */
    uint32_t              cpu;            /* owner CPU (valid iff active; else MAX_CPUS) */
    volatile uint32_t     active;         /* 1 = armed and in a per-CPU list */
} kernel_timer_t;

/* ---- API ----------------------------------------------------------------- */

/* Initialize a timer object. Must be called before first arm.
 * The kernel_timer_t is caller-owned (static, heap, or pool). */
void KeInitializeTimer(kernel_timer_t *timer);

/* Arm a timer for an absolute tick deadline.
 *   due_time_ticks: absolute system_get_ticks() value at which it expires
 *   period_ticks:   0 = single-shot; >0 = re-arm this many ticks after expiry
 *   dpc:            DPC to queue on each expiry (NULL = timer fires silently)
 * If the timer is already active it is first cancelled (cross-CPU safe), then
 * re-armed on the BSP service-CPU list (the only CPU whose timer ISR services
 * timers -- AP LAPIC timers are masked). May be called at any IRQL up to
 * DIRQL and from any CPU. */
void KeSetTimerEx(kernel_timer_t *timer, uint64_t due_time_ticks,
                  uint64_t period_ticks, KDPC *dpc);

/* Single-shot convenience wrapper (period_ticks = 0). */
void KeSetTimer(kernel_timer_t *timer, uint64_t due_time_ticks, KDPC *dpc);

/* Cancel a timer: remove it from its CPU's list and stop further DPC queueing.
 * Returns 1 if the timer was active (and is now cancelled), 0 if not armed.
 * Does NOT dequeue an already-queued DPC -- if the timer already fired this
 * tick and its DPC is in a DPC queue, the caller must KeFlushQueuedDpcs() to
 * synchronize. Safe at any IRQL up to DISPATCH_LEVEL. */
int KeCancelTimer(kernel_timer_t *timer);

/* ---- Per-CPU timer list (internal) --------------------------------------- */

/* Phase 1: initialize per-CPU timer lists before sti. Allocation-free.
 * The lists are BSS-zero by default; this is the explicit, logged init for
 * parity with dpc_init_queues() and a one-line boot record. */
void ktimer_init_lists(void);

/* Fire all expired kernel timers (on the BSP service list). Called from the
 * LAPIC and PIT timer ISRs each tick, immediately BEFORE the DPC drain so a
 * fired timer's DPC drains in the SAME tick (matching nt_timer_tick placement).
 * Each fired timer's DPC is queued while the ktimer lock is still held so the
 * unlink/active-clear and the DPC handoff are atomic vs a concurrent
 * KeCancelTimer. Returns the number of timers fired. Caller is in ISR
 * context (interrupts already off). */
uint32_t ktimer_expire_current_cpu(void);
