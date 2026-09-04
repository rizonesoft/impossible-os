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

/* DPC importance levels. Numeric values match the Windows WDK wdm.h ordering
 * (Low, Medium, High, MediumHigh) so a binary/WDK-facing caller that passes the
 * Windows numeric value gets the matching behavior: only HighImportance (== 2)
 * head-inserts; the rest tail-queue FIFO. */
typedef enum {
    LowImportance        = 0,
    MediumImportance     = 1,
    HighImportance       = 2,
    MediumHighImportance = 3,
} KDPC_IMPORTANCE;

/* Pin the NT/WDK numeric ABI -- a head-insert that keys on the symbol but a
 * caller that passes the raw Windows value must agree on HighImportance == 2. */
_Static_assert(LowImportance == 0 && MediumImportance == 1 &&
               HighImportance == 2 && MediumHighImportance == 3,
               "KDPC_IMPORTANCE must match WDK wdm.h numeric ordering");

/* Forward declaration for the DPC routine signature */
struct _KDPC;

/* DPC routine callback.
 * IRQL: a NORMAL DPC (KeInitializeDpc) runs at DISPATCH_LEVEL with interrupts
 * enabled; a THREADED DPC (KeInitializeThreadedDpc) uses the SAME signature but
 * runs at PASSIVE_LEVEL in the DPC worker thread (so it may block/page/take
 * mutexes). Do not assume DISPATCH_LEVEL in a threaded DPC routine.
 * AFFINITY: a NORMAL DPC runs on its target CPU, BUT only the BSP DPC queue has
 * a guaranteed drain trigger today (the LAPIC timer ISR). An idle AP does not
 * self-drain its DPC queue and there is no DPC IPI yet, so a NORMAL DPC targeted
 * (KeSetTargetProcessorDpc) at an otherwise-idle AP can sit until that AP next
 * lowers IRQL on its own. Until a remote drain trigger exists, pin time-critical
 * DPCs to the BSP service CPU (KeInsertQueueDpcOnCpu), as ktimer does. A THREADED
 * DPC has NO
 * CPU-affinity guarantee -- KeSetTargetProcessorDpc on a threaded DPC controls
 * only which CPU's threaded list it joins (queue ownership/order), NOT the CPU
 * the callback runs on: a single all-CPU worker thread drains every CPU's
 * threaded list and runs the callback on whatever CPU the scheduler gives the
 * worker. Do not rely on smp_this_cpu() or per-CPU state in a threaded DPC
 * routine. (Per-CPU affinity workers -- NT parity -- are a roadmap item that
 * awaits a task_set_affinity primitive.)
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

/* Queue a DPC for execution. A NORMAL DPC runs at DISPATCH_LEVEL on the current
 * (or KeSetTargetProcessorDpc-targeted) CPU. A THREADED DPC instead lands on its
 * target CPU's threaded list but the callback runs on the all-CPU worker's CPU,
 * NOT the target -- see the KDEFERRED_ROUTINE AFFINITY note.
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

/* Same insert as KeInsertQueueDpc. A depth crossing arms a per-CPU pending flag
 * emitted serial-safe by dpc_watchdog_tick, never at the caller's IRQL. NOTE:
 * dpc_watchdog_tick is BSP-only today, so the warning is emitted only for a
 * crossing on the BSP/ticking queue; a crossing armed on an AP's queue stays
 * pending until AP-queue warn_pending coverage lands (the BSP cross-CPU sweep
 * deferred to the AP-watchdog item). *warn_cpu_out, if non-NULL, is set to the
 * warn-depth CPU id (else -1) -- purely informational; callers must NOT klog
 * from it (this path is DIRQL-callable). Pass NULL when not needed. Same return
 * value as KeInsertQueueDpc. */
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
 * A queue-depth warning never triggers serial I/O at DIRQL: the insert arms a
 * per-CPU pending flag that dpc_watchdog_tick emits once per tick (on the
 * BSP/ticking queue; an AP-armed crossing stays pending until AP-queue coverage
 * lands -- see KeInsertQueueDpcEx), keeping this ISR-context call allocation-
 * free, bounded, and serial-I/O-free even when a KINTERRUPT-bound ISR holds the
 * interrupt spinlock across it.
 * Returns 1 if newly queued, 0 if the DPC was already queued (args refreshed). */
static inline int KeRequestDpcFromIsr(KDPC *dpc, void *arg1, void *arg2)
{
    return KeInsertQueueDpc(dpc, arg1, arg2);
}

/* Remove a DPC from its CPU's queue before it executes.
 * Returns 1 if the DPC was found and removed, 0 if not queued.
 * Safe to call at any IRQL up to DISPATCH_LEVEL. */
int KeRemoveQueueDpc(KDPC *dpc);

/* Initialize a threaded DPC -- runs at PASSIVE_LEVEL in a dedicated thread.
 * Allows paging, mutex acquisition, and other blocking operations. NO CPU
 * affinity: the callback runs on the single all-CPU worker's CPU, not the
 * KeSetTargetProcessorDpc target -- see KDEFERRED_ROUTINE AFFINITY note. */
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

/* Set target CPU for DPC execution. Must be called before KeInsertQueueDpc.
 * For a NORMAL DPC the target is the CPU the callback runs on. For a THREADED
 * DPC the target selects only which CPU's threaded list the DPC joins (queue
 * ownership/order); the callback still runs on the all-CPU worker's CPU, NOT
 * the target -- see KDEFERRED_ROUTINE AFFINITY note. */
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

/* Upper bound on the diagnostic queue walk in dpc_sample_queue(). This bounds
 * how long that helper holds DPC_QLOCK with interrupts off; it is NOT a queue
 * depth limit -- the normal DPC queue has none, and DPC_QUEUE_WARN_DEPTH above
 * is only a warning threshold. A walk that reaches the cap reports
 * truncated = 1, and its counts must then not be compared against depth. */
#define DPC_SAMPLE_MAX_WALK  1024u

/* Consistent snapshot of one CPU's normal DPC queue, taken inside a SINGLE
 * DPC_QLOCK critical section so the depth counter and the linked contents
 * cannot disagree because a remote CPU inserted between two reads. Any CPU may
 * target any CPU's queue, so a delta between two separately-locked reads of
 * depth proves nothing; depth == list_len within one snapshot does.
 *
 * Deliberately carries NO KDPC-owned fields (queued / queued_cpu): those are
 * protected by the lock of the queue the KDPC actually lives on, which
 * dpc_insert_core() re-resolves from dpc->queued_cpu and which need not be
 * DPC_QLOCK(cpu_id). Read them from the KDPC directly where the caller owns
 * the object. */
struct dpc_queue_sample {
    uint32_t depth;         /* q->depth, read under the lock                 */
    uint32_t list_len;      /* KDPCs actually linked on q->head              */
    uint32_t occurrences;   /* times the queried KDPC appears on that list   */
    uint32_t truncated;     /* 1 if the walk stopped at DPC_SAMPLE_MAX_WALK  */
};

/* Test/diagnostic: snapshot cpu_id's normal DPC queue. `dpc` may be NULL, in
 * which case occurrences is always 0. Returns 1 on success, 0 for an invalid
 * cpu_id or a NULL out pointer (out is then left untouched). Callable up to
 * DIRQL -- it takes DPC_QLOCK with interrupts saved, like the queue itself. */
int dpc_sample_queue(uint32_t cpu_id, const KDPC *dpc,
                     struct dpc_queue_sample *out);

/* Drain all queued DPCs on the current CPU at DISPATCH_LEVEL.
 * Raises IRQL to DISPATCH_LEVEL, executes DPC callbacks in FIFO order,
 * then restores previous IRQL. Bounded: drains at most DPC_BATCH_LIMIT
 * per invocation to prevent scheduler starvation.
 * Callable explicitly by any code that wants to flush the queue. NOTE:
 * automatic draining happens at a timer ISR (the LAPIC timer and the PIT
 * fallback both call the lightweight dpc_drain_current_cpu below after each
 * tick) AND when KeLowerIrql crosses below DISPATCH_LEVEL (drain-on-lower). The
 * residual gap is an IDLE AP that neither ticks (AP timer masked) nor lowers
 * IRQL: with no DPC IPI it has no trigger, so a DPC targeted there can strand
 * until that AP next lowers IRQL -- the deferred Remote-target DPC IPI work. */
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
/* One-shot summary of DPC threshold overruns seen so far: one line per
 * offending routine with its count and worst case. The per-occurrence
 * warning is burst-limited, so this is what makes a chronic offender
 * visible without letting it own the log. */
void dpc_watchdog_report(void);

/* Per-timer-tick watchdog bookkeeping: refill the per-CPU token budget and fire
 * the sustained-queue-depth warning. Call from the timer ISR every tick. */
void dpc_watchdog_tick(void);

/* Enforcement mode for the single-DPC runtime watchdog: 0 (default) = warn,
 * 1 = escalate a >100us DPC to KeBugCheckEx(DPC_WATCHDOG_VIOLATION). */
void dpc_watchdog_set_strict(int on);
int  dpc_watchdog_strict_enabled(void);
