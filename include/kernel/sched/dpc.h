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
 * Called at DISPATCH_LEVEL with interrupts enabled.
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

/* Queue a DPC for execution at DISPATCH_LEVEL on the current CPU.
 * ISR-safe: does not block, does not allocate memory.
 * If the DPC is already queued, updates arg1/arg2 and returns 0 (no-op).
 * Returns 1 if newly queued, 0 if already queued (args still updated).
 *
 * May be called at any IRQL up to DIRQL. */
int KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2);

/* Remove a DPC from its CPU's queue before it executes.
 * Returns 1 if the DPC was found and removed, 0 if not queued.
 * Safe to call at any IRQL up to DISPATCH_LEVEL. */
int KeRemoveQueueDpc(KDPC *dpc);

/* Initialize a threaded DPC -- runs at PASSIVE_LEVEL in a dedicated thread.
 * Allows paging, mutex acquisition, and other blocking operations. */
void KeInitializeThreadedDpc(KDPC *dpc, KDEFERRED_ROUTINE routine, void *context);

/* Create per-CPU DPC worker threads. Call after scheduler init. */
void dpc_start_threads(void);

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
};

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
 * automatic draining currently happens ONLY at the LAPIC timer ISR (via the
 * lightweight dpc_drain_current_cpu below) -- KeLowerIrql does NOT yet auto-
 * drain on crossing below DISPATCH_LEVEL (planned as part of APC delivery). */
void KiDispatchDpc(void);

/* Maximum DPCs to drain per KiDispatchDpc() call.
 * After this many, yield to let the scheduler run, then re-enter. */
#define DPC_BATCH_LIMIT  32

/* Lightweight drain for use from the timer ISR.
 * Skips IRQL management -- caller must be at or above DISPATCH_LEVEL.
 * Returns the number of DPCs executed. */
uint32_t dpc_drain_current_cpu(void);
