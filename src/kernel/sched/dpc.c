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
#include "kernel/sched/irql.h"
#include "kernel/sched/spinlock.h"
#include "kernel/smp.h"
#include "kernel/klog.h"
#include "kernel/barrier.h"

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
static int dpc_insert_core(KDPC *dpc, void *arg1, void *arg2, int *warn_cpu_out)
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

        /* Resolve the concrete target CPU for a fresh insert. */
        if (dpc->cpu_target == DPC_TARGET_CURRENT)
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
    int r = dpc_insert_core(dpc, arg1, arg2, &warn_cpu);
    if (warn_cpu >= 0)
        klog(LOG_WARN, "dpc",
             "CPU %u DPC queue depth reached %u (possible starvation)",
             (uint64_t)(uint32_t)warn_cpu, (uint64_t)DPC_QUEUE_WARN_DEPTH);
    return r;
}

/* Queue a DPC WITHOUT logging: if the warn depth is hit, *warn_cpu_out is set
 * to the CPU id (else -1) so a caller holding a spinlock can defer the klog
 * until after it unlocks. Used by ktimer_expire_current_cpu under the ktimer
 * lock. Same return value as KeInsertQueueDpc (1 newly queued, 0 already). */
int KeInsertQueueDpcEx(KDPC *dpc, void *arg1, void *arg2, int *warn_cpu_out)
{
    return dpc_insert_core(dpc, arg1, arg2, warn_cpu_out);
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

        if (routine)
            routine(dpc, ctx, a1, a2);

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

    KeRaiseIrql(DISPATCH_LEVEL, &old_irql);
    n = drain_queue(cpu_id);
    KeLowerIrql(old_irql);

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
