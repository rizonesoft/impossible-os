/* ============================================================================
 * dpc.h -- Deferred Procedure Call (DPC) objects and per-CPU queue
 *
 * DPCs allow interrupt service routines to defer non-trivial work to
 * DISPATCH_LEVEL, where it runs with interrupts enabled but preemption
 * disabled.  This is the standard NT top-half/bottom-half split:
 *
 *   ISR (DIRQL):  minimal register read, ACK, KeInsertQueueDpc()
 *   DPC (DISPATCH_LEVEL):  packet processing, state machine updates
 *   Workqueue (PASSIVE_LEVEL):  heavy work requiring blocking/allocation
 *
 * Usage:
 *   1. Driver allocates a KDPC (static or from pool) at init time
 *   2. KeInitializeDpc(&dpc, routine, context)
 *   3. ISR calls KeInsertQueueDpc(&dpc, arg1, arg2) -- ISR-safe, no alloc
 *   4. Timer/scheduler drains the per-CPU queue at DISPATCH_LEVEL (see section 5)
 *
 * KDPC objects are caller-owned.  The queue only links them -- no memory
 * allocation occurs during KeInsertQueueDpc().  A DPC can only be queued
 * on one CPU at a time; re-inserting a queued DPC is a safe no-op.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* DPC importance levels */
typedef enum {
    LowImportance        = 0,
    MediumImportance     = 1,
    MediumHighImportance = 2,
    HighImportance       = 3,
} KDPC_IMPORTANCE;

/* Forward declaration for the DPC routine signature */
struct _KDPC;

/* DPC routine callback.
 * IRQL: a NORMAL DPC (KeInitializeDpc) runs at DISPATCH_LEVEL with interrupts
 * enabled; a THREADED DPC (KeInitializeThreadedDpc) uses the SAME signature but
 * runs at PASSIVE_LEVEL in the DPC worker thread (so it may block/page/take
 * mutexes). Do not assume DISPATCH_LEVEL in a threaded DPC routine.
 *   dpc:      the KDPC object (for self-referencing patterns)
 *   context:  deferred context set at KeInitializeDpc time
 *   arg1/arg2: per-invocation arguments from KeInsertQueueDpc */
typedef void (*KDEFERRED_ROUTINE)(struct _KDPC *dpc, void *context,
                                   void *arg1, void *arg2);

/* ---- KDPC object --------------------------------------------------------- */

typedef struct _KDPC {
    KDEFERRED_ROUTINE   routine;        /* callback function */
    void               *deferred_ctx;   /* driver/subsystem context */
    void               *system_arg1;    /* per-invocation argument 1 */
    void               *system_arg2;    /* per-invocation argument 2 */
    struct _KDPC       *next;           /* intrusive queue link (NULL = not queued) */
    volatile uint32_t   queued;         /* 1 if currently in a CPU's DPC queue */
    uint32_t            cpu_target;     /* target CPU (0xFFFFFFFF = current CPU) */
    uint32_t            queued_cpu;     /* CPU this DPC lives on (valid iff queued==1; for cross-CPU remove) */
    KDPC_IMPORTANCE     importance;     /* queue insertion priority */
    uint8_t             threaded;      /* 1 = run at PASSIVE_LEVEL in DPC thread */
} KDPC;

/* Target CPU: use current CPU at insert time */
#define DPC_TARGET_CURRENT  0xFFFFFFFF

/* ---- DPC API ------------------------------------------------------------- */

/* Initialize a DPC object.  Must be called before first use.
 * The KDPC is caller-owned (static, heap, or pool -- not freed by DPC code).
 *   dpc:      pointer to caller-allocated KDPC
 *   routine:  function to call at DISPATCH_LEVEL
 *   context:  opaque context passed to every invocation */
void KeInitializeDpc(KDPC *dpc, KDEFERRED_ROUTINE routine, void *context);

/* Queue a DPC for execution at DISPATCH_LEVEL on the current (or targeted) CPU.
 * ISR-safe: does not block, does not allocate memory.
 * If the DPC is already queued, updates arg1/arg2 and returns 0 (no-op).
 * Returns 1 if newly queued, 0 if already queued (args still updated).
 *
 * Precondition (NT contract): a single KDPC must not be inserted concurrently
 * from more than one CPU. Like Windows ("a driver must not queue the same DPC
 * object more than once"), the caller owns DPC single-ownership. Sequential
 * cross-CPU operations are safe -- the DPC records the CPU it lives on
 * (queued_cpu), so KeRemoveQueueDpc and a re-insert from a different CPU find
 * the right queue. Concurrent same-DPC inserts from two CPUs are caller misuse.
 *
 * May be called at any IRQL up to DIRQL. */
int KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2);

/* Like KeInsertQueueDpc but does NOT call klog: if the queue reaches the warn
 * depth, *warn_cpu_out is set to that CPU id (else -1). For callers that hold a
 * spinlock at insert time (e.g. ktimer_expire_current_cpu under the ktimer
 * lock) and must defer the serial-I/O warning until after they unlock. Pass
 * a non-NULL int; same return value as KeInsertQueueDpc. */
int KeInsertQueueDpcEx(KDPC *dpc, void *arg1, void *arg2, int *warn_cpu_out);

/* Like KeInsertQueueDpcEx but pins a FRESH insert to cpu_id, overriding
 * dpc->cpu_target WITHOUT mutating it (caller-owned field preserved). Does NOT
 * move an already-queued DPC across CPUs (already-queued -> update args on its
 * current queue, return 0). cpu_id >= MAX_CPUS honors cpu_target. Used by ktimer
 * to pin timer DPCs to the service CPU under the timer-DPC ownership
 * precondition. */
int KeInsertQueueDpcOnCpu(KDPC *dpc, uint32_t cpu_id, void *arg1, void *arg2,
                          int *warn_cpu_out);

/* ISR top-half helper: queue a pre-initialized DPC from an interrupt handler.
 * This is the ENQUEUE side of the canonical NT ISR pattern only -- the device
 * register read / acknowledge / claim sequencing stays explicit in the driver
 * ISR (acking before the DPC runs is device-specific and must not be hidden
 * behind a callback). Allocation-free and bounded, so it is safe at DIRQL.
 *   driver ISR:  read+ack device status; KeRequestDpcFromIsr(&dpc, a1, a2); return claimed
 *   DPC (DISPATCH_LEVEL):  process the snapshotted work
 * Uses the NO-LOG insert (KeInsertQueueDpcEx): a queue-depth warning must never
 * trigger serial I/O at DIRQL, and a KINTERRUPT-bound ISR may hold the
 * interrupt spinlock across this call. The depth warning is intentionally
 * dropped in ISR context (the DPC still queues + drains normally).
 * Returns 1 if newly queued, 0 if the DPC was already queued (args refreshed). */
static inline int KeRequestDpcFromIsr(KDPC *dpc, void *arg1, void *arg2)
{
    int warn_cpu = -1;   /* depth warning suppressed in ISR context */
    return KeInsertQueueDpcEx(dpc, arg1, arg2, &warn_cpu);
}

/* Remove a DPC from its CPU's queue before it executes.
 * Returns 1 if the DPC was found and removed, 0 if not queued.
 * Safe to call at any IRQL up to DISPATCH_LEVEL. */
int KeRemoveQueueDpc(KDPC *dpc);

/* Initialize a threaded DPC -- runs at PASSIVE_LEVEL in a dedicated thread.
 * Allows paging, mutex acquisition, and other blocking operations. */
void KeInitializeThreadedDpc(KDPC *dpc, KDEFERRED_ROUTINE routine, void *context);

/* Spin up the single all-CPU threaded-DPC drain worker. Called once at boot
 * after scheduler init (boot_desktop.c); CAS-idempotent so a duplicate call is
 * a no-op. Boot-time (not lazy) so a threaded DPC queued from any IRQL always
 * has a worker to consume it. */
void dpc_start_threads(void);

/* Test/diagnostic: nonzero once the threaded-DPC worker has been started. */
int dpc_worker_started(void);

/* Test/diagnostic: threaded DPC callbacks currently in flight (0 when idle). */
uint32_t dpc_in_flight_threaded(void);

/* Set target CPU for DPC execution. Must be called before KeInsertQueueDpc. */
void KeSetTargetProcessorDpc(KDPC *dpc, uint32_t cpu_number);

/* Set DPC importance level. Affects queue position and dispatch urgency. */
void KeSetImportanceDpc(KDPC *dpc, KDPC_IMPORTANCE importance);

/* Block until all currently queued DPCs on all CPUs have completed.
 * Must be called at PASSIVE_LEVEL. Used for driver teardown. */
void KeFlushQueuedDpcs(void);

/* ---- Per-CPU DPC queue (internal) ---------------------------------------- */

/* Maximum DPC queue depth before overflow warning.  Not a hard limit --
 * DPCs are intrusive-linked so the queue is bounded only by the number
 * of KDPC objects that exist system-wide. */
#define DPC_QUEUE_WARN_DEPTH  64

/* Per-CPU DPC queue state.  Embedded in per-CPU data or indexed by CPU ID. */
struct dpc_queue {
    KDPC           *head;           /* queue head (FIFO) */
    KDPC           *tail;           /* queue tail for O(1) append */
    volatile uint32_t depth;        /* current queue depth */
    uint32_t        executed;       /* total DPCs executed (stats) */
    uint32_t        max_depth;      /* high-water mark (stats) */
    /* Pad to one cache line + align: the per-CPU cpu_queues[] array must not
     * false-share a line between adjacent CPUs on the ISR-hot drain path. */
    char            _pad[64 - (2 * sizeof(KDPC *) + 3 * sizeof(uint32_t))];
} __attribute__((aligned(64)));
_Static_assert(sizeof(struct dpc_queue) == 64,
               "struct dpc_queue must occupy exactly one cache line");

/* Phase 1: initialize per-CPU DPC queues before sti.
 * After this, ISRs can safely queue DPCs (drained later by timer ISR). */
void dpc_init_queues(void);

/* Phase 3: full DPC subsystem init (scheduler available for worker threads).
 * Calls dpc_init_queues() if not already done. */
void dpc_init(void);

/* Get the per-CPU DPC queue for the current CPU. */
struct dpc_queue *dpc_this_cpu_queue(void);

/* Get the per-CPU DPC queue for a specific CPU. */
struct dpc_queue *dpc_get_cpu_queue(uint32_t cpu_id);

/* Drain all queued DPCs on the current CPU at DISPATCH_LEVEL.
 * Raises IRQL to DISPATCH_LEVEL, executes DPC callbacks in FIFO order,
 * then restores previous IRQL. Bounded: drains at most DPC_BATCH_LIMIT
 * per invocation to prevent scheduler starvation.
 * Callable explicitly by any code that wants to flush the queue. NOTE:
 * automatic draining currently happens ONLY at a timer ISR (the LAPIC timer
 * and the PIT fallback both call the lightweight dpc_drain_current_cpu below
 * after each tick) -- KeLowerIrql does NOT yet auto-drain on crossing below
 * DISPATCH_LEVEL (planned as part of APC delivery). */
void KiDispatchDpc(void);

/* Maximum DPCs to drain per KiDispatchDpc() call.
 * After this many, yield to let the scheduler run, then re-enter. */
#define DPC_BATCH_LIMIT  32

/* Lightweight drain for use from the timer ISR.
 * Skips IRQL management -- caller must be at or above DISPATCH_LEVEL.
 * Returns the number of DPCs executed. */
uint32_t dpc_drain_current_cpu(void);

/* Cheap lock-free probe: 1 if any DPC is queued on the current CPU. Used by
 * KeLowerIrql to skip the DISPATCH-drain bracket (and its LAPIC TPR writes) on
 * the common empty path (e.g. every syscall-entry IRQL lower). */
int dpc_current_cpu_has_pending(void);

/* ---- DPC fairness budget + watchdog (section 14) ------------------------- */

/* Compute the single-DPC TSC cycle threshold + seed per-CPU token budgets.
 * Called from dpc_init() after TSC calibration; idempotent. */
void dpc_watchdog_init(void);

/* Per-timer-tick watchdog bookkeeping: refill the per-CPU token budget and fire
 * the sustained-queue-depth warning. Call from the timer ISR every tick. */
void dpc_watchdog_tick(void);

/* Enforcement mode for the single-DPC runtime watchdog: 0 (default) = warn,
 * 1 = escalate a >100us DPC to KeBugCheckEx(DPC_WATCHDOG_VIOLATION). */
void dpc_watchdog_set_strict(int on);
int  dpc_watchdog_strict_enabled(void);
