/* ============================================================================
 * workqueue.c — Kernel work queue implementation
 *
 * Each work queue has:
 *   - A static pool of work_item_t nodes  (no kmalloc in IRQ path)
 *   - A raw spinlock protecting the list and free pool
 *   - A semaphore count (volatile uint32_t) the worker spins/sleeps on
 *   - A dedicated kernel task (spawned via task_create)
 *
 * workqueue_enqueue() (IRQ-safe):
 *   1. Acquire spinlock (CLI + CAS)
 *   2. Pop a node from the free pool
 *   3. Append to pending tail
 *   4. Release spinlock (STI)
 *   5. Increment sem_count
 *
 * Worker thread (normal context):
 *   loop:
 *     wait until sem_count > 0
 *     dequeue head under spinlock
 *     call fn(arg)
 *     return node to free pool under spinlock
 * ============================================================================ */

#include "kernel/sched/workqueue.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/barrier.h"

/* ---------------------------------------------------------------------------
 * System work queue — created at boot
 * ------------------------------------------------------------------------- */
workqueue_t *sys_wq = (workqueue_t *)0;

/* ---------------------------------------------------------------------------
 * Internal helpers: raw spinlock (avoids circular include with spinlock.h
 * while still being safe from IRQ context).
 * We embed the raw flag directly in workqueue_t.lock_flag.
 * ------------------------------------------------------------------------- */

/*
 * The worker thread entry is stored per-wq so task_create's zero-arg entry
 * can find its queue.  We use a global slot table (max 8 work queues).
 */
#define MAX_WORKQUEUES  8
static workqueue_t *wq_registry[MAX_WORKQUEUES];
static uint32_t     wq_reg_count = 0;

/* Each worker task calls this; it finds its own queue via pid. */
static void worker_thread_fn(void)
{
    /* Find our queue by matching our PID */
    workqueue_t *wq = (workqueue_t *)0;
    uint32_t i;
    struct task *me = task_current();

    for (i = 0; i < wq_reg_count; i++) {
        if (wq_registry[i] && (uint32_t)wq_registry[i]->worker_pid == me->pid) {
            wq = wq_registry[i];
            break;
        }
    }

    if (!wq) {
        /* Should never happen */
        klog(LOG_ERROR, "wq", "worker task could not find its queue!");
        task_exit(-1);
        return;
    }

    klog(LOG_DEBUG, "wq", "work queue \"%s\" worker started (PID %u)",
         wq->name, (uint64_t)me->pid);

    for (;;) {
        work_item_t *item = (work_item_t *)0;
        uint64_t     irq_flags;

        /* Wait (spin-yield) until work is available */
        while (wq->sem_count <= 0) {
            barrier();
            yield();
        }

        /* Dequeue one item under spinlock */
        spin_lock_irqsave((spinlock_t *)&wq->lock_flag, &irq_flags);
        if (wq->sem_count > 0 && wq->head) {
            wq->sem_count--;
            item = wq->head;
            wq->head = item->next;
            if (!wq->head)
                wq->tail = (work_item_t *)0;
            item->next = (work_item_t *)0;
        }
        spin_unlock_irqrestore((spinlock_t *)&wq->lock_flag, irq_flags);

        if (!item)
            continue;   /* race — sem_count was bumped but head was null */

        /* Execute the work item in thread context (can yield, alloc, etc.) */
        if (item->fn)
            item->fn(item->arg);

        /* Return node to free pool */
        spin_lock_irqsave((spinlock_t *)&wq->lock_flag, &irq_flags);
        item->fn   = (work_fn_t)0;
        item->arg  = (void *)0;
        item->next = wq->free_head;
        wq->free_head = item;
        spin_unlock_irqrestore((spinlock_t *)&wq->lock_flag, irq_flags);
    }
}

/* ---------------------------------------------------------------------------
 * workqueue_create — allocate and initialize a work queue, spawn its thread
 * ------------------------------------------------------------------------- */
workqueue_t *workqueue_create(const char *name)
{
    workqueue_t *wq;
    uint32_t i;
    int pid;

    if (wq_reg_count >= MAX_WORKQUEUES) {
        klog(LOG_ERROR, "wq", "workqueue_create: max queues reached");
        return (workqueue_t *)0;
    }

    wq = (workqueue_t *)kmalloc(sizeof(workqueue_t));
    if (!wq) {
        klog(LOG_ERROR, "wq", "workqueue_create: kmalloc failed");
        return (workqueue_t *)0;
    }

    /* Initialize pool — link all nodes into the free list */
    for (i = 0; i < WQ_POOL_SIZE - 1; i++)
        wq->pool[i].next = &wq->pool[i + 1];
    wq->pool[WQ_POOL_SIZE - 1].next = (work_item_t *)0;
    wq->free_head = &wq->pool[0];

    wq->head      = (work_item_t *)0;
    wq->tail      = (work_item_t *)0;
    wq->lock_flag = 0;
    wq->sem_count = 0;
    wq->name      = name;

    /* Register before spawning so the thread can find us by PID */
    wq->worker_pid = -1;
    wq_registry[wq_reg_count++] = wq;

    /* Spawn the worker kernel task */
    pid = task_create(worker_thread_fn, name);
    if (pid < 0) {
        klog(LOG_ERROR, "wq", "workqueue_create: task_create failed");
        wq_reg_count--;
        wq_registry[wq_reg_count] = (workqueue_t *)0;
        kfree(wq);
        return (workqueue_t *)0;
    }

    wq->worker_pid = pid;

    klog(LOG_DEBUG, "wq", "work queue \"%s\" created (worker PID %u)",
         name, (uint64_t)pid);

    return wq;
}

/* ---------------------------------------------------------------------------
 * workqueue_enqueue — IRQ-safe enqueue of a work item
 * ------------------------------------------------------------------------- */
int workqueue_enqueue(workqueue_t *wq, work_fn_t fn, void *arg)
{
    work_item_t *item;
    uint64_t     irq_flags;

    if (!wq || !fn)
        return 0;

    spin_lock_irqsave((spinlock_t *)&wq->lock_flag, &irq_flags);

    /* Grab a free node from the pool */
    item = wq->free_head;
    if (!item) {
        /* Pool exhausted — this is a programming error or runaway enqueue */
        spin_unlock_irqrestore((spinlock_t *)&wq->lock_flag, irq_flags);
        return 0;
    }
    wq->free_head = item->next;

    /* Fill in the item */
    item->fn   = fn;
    item->arg  = arg;
    item->next = (work_item_t *)0;

    /* Append to list tail */
    if (wq->tail)
        wq->tail->next = item;
    else
        wq->head = item;
    wq->tail = item;

    /* Signal the worker */
    wq->sem_count++;

    spin_unlock_irqrestore((spinlock_t *)&wq->lock_flag, irq_flags);

    return 1;
}

/* ---------------------------------------------------------------------------
 * workqueue_flush — block until the queue is empty
 * ------------------------------------------------------------------------- */
void workqueue_flush(workqueue_t *wq)
{
    if (!wq) return;
    while (wq->sem_count > 0 || wq->head) {
        barrier();
        yield();
    }
}

/* ---------------------------------------------------------------------------
 * workqueue_destroy — flush then free
 * Simplified: we don't kill the worker task (it loops forever by design).
 * In a more complete implementation we'd signal the worker to exit.
 * ------------------------------------------------------------------------- */
void workqueue_destroy(workqueue_t *wq)
{
    uint32_t i;
    if (!wq) return;
    workqueue_flush(wq);
    kfree(wq);
    /* Remove from registry */
    for (i = 0; i < wq_reg_count; i++) {
        if (wq_registry[i] == wq) {
            wq_registry[i] = wq_registry[--wq_reg_count];
            wq_registry[wq_reg_count] = (workqueue_t *)0;
            break;
        }
    }
}
