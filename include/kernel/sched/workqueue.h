/* ============================================================================
 * workqueue.h -- Kernel deferred-work (work queue) API
 *
 * Work queues decouple time-critical IRQ top-halves from slow processing.
 * An IRQ handler calls workqueue_enqueue() (IRQ-safe, < 1 µs) and returns.
 * A dedicated kernel thread picks up the work item and executes it in a
 * normal, yieldable context where it can call VFS, allocate memory, etc.
 *
 * Equivalent to: Linux  workqueue_struct / INIT_WORK / schedule_work
 *                Windows DPC (Deferred Procedure Call) + work items
 *
 * Usage:
 *   // At boot (thread context):
 *   workqueue_t *wq = workqueue_create("my_wq");
 *
 *   // From IRQ handler (or any context):
 *   workqueue_enqueue(wq, my_callback, arg_ptr);
 *
 *   // my_callback(arg) runs in the work queue thread -- may yield, alloc, etc.
 *   static void my_callback(void *arg) {
 *       net_rx(arg, len);  // safe -- in thread context
 *   }
 *
 * System work queue:
 *   extern workqueue_t *sys_wq;     // default queue, created at boot
 *   workqueue_enqueue(sys_wq, fn, arg);
 *
 * IRQ safety rules:
 *   workqueue_enqueue() -- IRQ-safe (spinlock + no yield)
 *   workqueue_create()  -- thread context only (spawns a kernel task)
 *   workqueue_flush()   -- thread context only (blocks until queue drains)
 *   workqueue_destroy() -- thread context only
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---------------------------------------------------------------------------
 * Work item callback type
 * ------------------------------------------------------------------------- */
typedef void (*work_fn_t)(void *arg);

/* ---------------------------------------------------------------------------
 * work_item_t -- singly-linked node in the pending work list
 * ------------------------------------------------------------------------- */
typedef struct work_item {
    work_fn_t        fn;    /* callback to invoke */
    void            *arg;   /* opaque argument passed to fn */
    struct work_item *next; /* intrusive linked-list pointer */
} work_item_t;

/* ---------------------------------------------------------------------------
 * workqueue_t -- owns the pending list, spinlock, and semaphore
 * ------------------------------------------------------------------------- */
#define WQ_POOL_SIZE  64   /* pre-allocated work item nodes (avoids kmalloc in IRQ) */

typedef struct workqueue {
    /* Pending work list -- protected by lock */
    work_item_t *head;   /* oldest item (next to execute) */
    work_item_t *tail;   /* newest item (last enqueued)   */

    /* Free-list pool -- pre-allocated work_item_t nodes.
     * workqueue_enqueue() takes a node from the free list (IRQ-safe).
     * The worker thread returns nodes to the free list after calling fn. */
    work_item_t *free_head;
    work_item_t  pool[WQ_POOL_SIZE];  /* static allocation -- no kmalloc in IRQ */

    /* Spinlock protecting head, tail, free_head */
    volatile uint32_t lock_flag;   /* raw spinlock (avoids circular include) */

    /* Semaphore count -- incremented by enqueue, decremented by worker */
    volatile int32_t  sem_count;   /* > 0: work available; <= 0: worker sleeps */

    /* Worker PID (for flush/destroy) */
    int worker_pid;

    /* Debug name */
    const char *name;
} workqueue_t;

/* ---------------------------------------------------------------------------
 * Global system work queue -- created at boot, usable from any driver
 * ------------------------------------------------------------------------- */
extern workqueue_t *sys_wq;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Create a work queue and spawn its kernel worker thread.
 * Returns a pointer to the new queue, or NULL on failure.
 * MUST be called from thread context. */
workqueue_t *workqueue_create(const char *name);

/* Enqueue a work item.  fn(arg) will be called in the worker thread.
 * IRQ-safe -- uses a spinlock, no yield, no allocation.
 * Returns 1 on success, 0 if the work item pool is exhausted. */
int workqueue_enqueue(workqueue_t *wq, work_fn_t fn, void *arg);

/* Block until all currently-pending items have been processed.
 * MUST be called from thread context. */
void workqueue_flush(workqueue_t *wq);

/* Destroy a work queue (flushes first). Thread context only. */
void workqueue_destroy(workqueue_t *wq);
