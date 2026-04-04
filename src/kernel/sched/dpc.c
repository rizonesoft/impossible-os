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

/* One queue per CPU, indexed by cpu_id.  Static allocation avoids any
 * heap dependency during early boot. */
static struct dpc_queue cpu_queues[MAX_CPUS];

/* Per-CPU spinlock protecting the DPC queue.  Separate from the queue
 * struct to keep the spinlock cache-line aligned. */
static spinlock_t queue_locks[MAX_CPUS];

/* Per-CPU threaded DPC pending list (separate from DISPATCH_LEVEL queue) */
static KDPC *threaded_head[MAX_CPUS];
static volatile uint32_t threaded_pending[MAX_CPUS];

/* ---- Initialization ------------------------------------------------------ */

void dpc_init(void)
{
    uint32_t i;
    for (i = 0; i < MAX_CPUS; i++) {
        cpu_queues[i].head      = (KDPC *)0;
        cpu_queues[i].tail      = (KDPC *)0;
        cpu_queues[i].depth     = 0;
        cpu_queues[i].executed  = 0;
        cpu_queues[i].max_depth = 0;
        queue_locks[i].flag     = 0;
        threaded_head[i]        = (KDPC *)0;
        threaded_pending[i]     = 0;
    }
    klog(LOG_INFO, "dpc", "DPC subsystem initialized (%u CPU queues)", (uint64_t)MAX_CPUS);
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

int KeInsertQueueDpc(KDPC *dpc, void *arg1, void *arg2)
{
    uint32_t cpu_id;
    struct dpc_queue *q;
    uint64_t irq_flags;

    /* Determine target CPU */
    if (dpc->cpu_target == DPC_TARGET_CURRENT)
        cpu_id = smp_this_cpu()->cpu_id;
    else
        cpu_id = dpc->cpu_target;

    if (cpu_id >= MAX_CPUS)
        return 0;

    q = &cpu_queues[cpu_id];

    /* Lock the queue -- ISR-safe (saves RFLAGS + cli) */
    spin_lock_irqsave(&queue_locks[cpu_id], &irq_flags);

    /* Always update arguments (even if already queued -- NT behavior) */
    dpc->system_arg1 = arg1;
    dpc->system_arg2 = arg2;

    /* If already queued, just update args and return */
    if (dpc->queued) {
        spin_unlock_irqrestore(&queue_locks[cpu_id], irq_flags);
        return 0;
    }

    /* Insert: HighImportance -> head, otherwise -> tail (FIFO) */
    dpc->next   = (KDPC *)0;
    dpc->queued = 1;

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

    /* Track high-water mark */
    if (q->depth > q->max_depth)
        q->max_depth = q->depth;

    /* Warn on excessive queue depth (but don't block) */
    if (q->depth == DPC_QUEUE_WARN_DEPTH) {
        klog(LOG_WARN, "dpc",
             "CPU %u DPC queue depth reached %u (possible starvation)",
             (uint64_t)cpu_id, (uint64_t)q->depth);
    }

    spin_unlock_irqrestore(&queue_locks[cpu_id], irq_flags);

    return 1;
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

    /* Determine which CPU's queue this DPC is in.
     * We check the target CPU; if DPC_TARGET_CURRENT was used at insert
     * time, the DPC is on the CPU that called KeInsertQueueDpc. Since we
     * can't know which CPU that was, scan the current CPU first (most
     * common case), then fall through. For targeted DPCs, go direct. */
    if (dpc->cpu_target != DPC_TARGET_CURRENT)
        cpu_id = dpc->cpu_target;
    else
        cpu_id = smp_this_cpu()->cpu_id;

    if (cpu_id >= MAX_CPUS)
        return 0;

    q = &cpu_queues[cpu_id];

    spin_lock_irqsave(&queue_locks[cpu_id], &irq_flags);

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

            cur->next   = (KDPC *)0;
            cur->queued = 0;
            q->depth--;
            found = 1;
            break;
        }
        prev = cur;
        cur  = cur->next;
    }

    spin_unlock_irqrestore(&queue_locks[cpu_id], irq_flags);

    return found;
}

/* ---- Threaded DPC worker ------------------------------------------------- */

static void dpc_thread_fn(void)
{
    struct per_cpu_data *cpu = smp_this_cpu();
    uint32_t cpu_id = cpu ? cpu->cpu_id : 0;

    klog(LOG_DEBUG, "dpc", "threaded DPC thread started on CPU %u",
         (uint64_t)cpu_id);

    for (;;) {
        /* Wait for work */
        while (!__atomic_load_n(&threaded_pending[cpu_id], __ATOMIC_ACQUIRE)) {
            extern void yield(void);
            yield();
        }

        /* Drain threaded DPC list at PASSIVE_LEVEL */
        while (threaded_head[cpu_id]) {
            KDPC *dpc = threaded_head[cpu_id];
            threaded_head[cpu_id] = dpc->next;
            dpc->next = (KDPC *)0;

            if (dpc->routine)
                dpc->routine(dpc, dpc->deferred_ctx,
                             dpc->system_arg1, dpc->system_arg2);
        }

        __atomic_store_n(&threaded_pending[cpu_id], 0, __ATOMIC_RELEASE);
    }
}

void dpc_start_threads(void)
{
    /* Create one DPC worker thread per online CPU.
     * For now, only create on BSP -- AP threads require cross-CPU task create. */
    extern int task_create(void (*entry)(void), const char *name);
    task_create(dpc_thread_fn, "dpc_thread");
    klog(LOG_INFO, "dpc", "Threaded DPC worker started (BSP)");
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
        uint64_t irq_flags;

        spin_lock_irqsave(&queue_locks[cpu_id], &irq_flags);

        dpc = q->head;
        if (!dpc) {
            spin_unlock_irqrestore(&queue_locks[cpu_id], irq_flags);
            break;
        }

        q->head = dpc->next;
        if (!q->head)
            q->tail = (KDPC *)0;
        q->depth--;

        routine = dpc->routine;
        ctx     = dpc->deferred_ctx;
        a1      = dpc->system_arg1;
        a2      = dpc->system_arg2;

        dpc->next   = (KDPC *)0;
        dpc->queued = 0;

        spin_unlock_irqrestore(&queue_locks[cpu_id], irq_flags);

        /* Threaded DPCs: move to threaded list instead of executing inline */
        if (dpc->threaded) {
            dpc->next = threaded_head[cpu_id];
            threaded_head[cpu_id] = dpc;
            __atomic_store_n(&threaded_pending[cpu_id], 1, __ATOMIC_RELEASE);
            dispatched++;
            q->executed++;
            continue;
        }

        if (routine)
            routine(dpc, ctx, a1, a2);

        dispatched++;
        q->executed++;
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

    /* Drain our own queue directly */
    {
        struct per_cpu_data *me = smp_this_cpu();
        if (me && me->cpu_id < MAX_CPUS)
            drain_queue(me->cpu_id);
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
