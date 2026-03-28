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

/* ---- Per-CPU DPC queues -------------------------------------------------- */

/* One queue per CPU, indexed by cpu_id.  Static allocation avoids any
 * heap dependency during early boot. */
static struct dpc_queue cpu_queues[MAX_CPUS];

/* Per-CPU spinlock protecting the DPC queue.  Separate from the queue
 * struct to keep the spinlock cache-line aligned. */
static spinlock_t queue_locks[MAX_CPUS];

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

    /* Append to tail (FIFO) */
    dpc->next   = (KDPC *)0;
    dpc->queued = 1;

    if (q->tail) {
        q->tail->next = dpc;
        q->tail       = dpc;
    } else {
        q->head = dpc;
        q->tail = dpc;
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
