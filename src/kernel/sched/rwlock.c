/* ============================================================================
 * rwlock.c -- Kernel read-write lock
 *
 * Allows multiple concurrent readers OR a single exclusive writer.
 *
 * Starvation prevention:
 *   When a writer calls rwlock_write_lock(), it sets writer_pending = 1.
 *   New readers that see writer_pending block themselves in the reader queue
 *   rather than acquiring the lock. This prevents a continuous stream of readers
 *   from starving the writer indefinitely.
 *
 *   On rwlock_write_unlock():
 *     - If no writer waiters: clear writer_pending and wake ALL reader waiters
 *     - If writer waiters exist: hand the lock directly to the next writer
 * ============================================================================ */

#include "kernel/sched/rwlock.h"
#include "kernel/sched/task.h"
#include "kernel/atomic.h"
#include "kernel/printk.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Wake all threads in a wait queue and clear it. */
static void wake_all_waiters(uint32_t *wtasks, uint32_t *wthreads,
                              uint32_t *num_waiters)
{
    uint32_t i;
    for (i = 0; i < *num_waiters; i++) {
        struct task *t = task_get_by_pid(wtasks[i]);
        if (t && wthreads[i] < t->num_threads)
            t->threads[wthreads[i]].state = THREAD_READY;
    }
    *num_waiters = 0;
}

/* Wake the first thread in a wait queue (FIFO). */
static void wake_first_waiter(uint32_t *wtasks, uint32_t *wthreads,
                               uint32_t *num_waiters)
{
    uint32_t i;
    if (*num_waiters == 0)
        return;

    struct task *t = task_get_by_pid(wtasks[0]);
    if (t && wthreads[0] < t->num_threads)
        t->threads[wthreads[0]].state = THREAD_READY;

    /* Shift queue left */
    for (i = 1; i < *num_waiters; i++) {
        wtasks[i - 1]   = wtasks[i];
        wthreads[i - 1] = wthreads[i];
    }
    (*num_waiters)--;
}

/* Enqueue current task/thread into a wait queue and block. */
static void enqueue_and_block(uint32_t *wtasks, uint32_t *wthreads,
                               uint32_t *num_waiters, uint32_t max_waiters)
{
    struct task   *cur  = task_current();
    struct thread *thr  = thread_current();

    if (*num_waiters < max_waiters) {
        wtasks[*num_waiters]   = cur->pid;
        wthreads[*num_waiters] = thr ? thr->id : 0;
        (*num_waiters)++;
    }
    /* else: queue full -- fall through to yield as spin-fallback */

    if (thr)
        thr->state = THREAD_BLOCKED;
    yield();
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void rwlock_init(rwlock_t *rw, const char *name)
{
    atomic_set(&rw->reader_count,   0);
    atomic_set(&rw->writer_held,    0);
    atomic_set(&rw->writer_pending, 0);
    rw->r_num_waiters = 0;
    rw->w_num_waiters = 0;
    rw->name          = name;
}

/* --- Read lock ----------------------------------------------------------- */

void rwlock_read_lock(rwlock_t *rw)
{
    /*
     * Block if:
     *   - A writer currently holds the lock, OR
     *   - A writer is pending (starvation guard -- writer gets priority)
     */
    while (atomic_read(&rw->writer_held) || atomic_read(&rw->writer_pending)) {
        enqueue_and_block(rw->r_waiter_tasks, rw->r_waiter_threads,
                          &rw->r_num_waiters, RWLOCK_MAX_WAITERS);
    }

    /* Increment reader count atomically */
    atomic_inc(&rw->reader_count);
}

void rwlock_read_unlock(rwlock_t *rw)
{
    if (atomic_read(&rw->reader_count) == 0) {
        printk("[RWLOCK] read_unlock \"%s\": reader_count already 0!\n",
               rw->name ? rw->name : "?");
        return;
    }

    atomic_dec(&rw->reader_count);

    /*
     * If this was the last reader and a writer is waiting,
     * wake that writer. The writer_pending flag stays set until
     * the writer actually acquires and then clears it in write_lock().
     */
    if (atomic_read(&rw->reader_count) == 0 && rw->w_num_waiters > 0)
        wake_first_waiter(rw->w_waiter_tasks, rw->w_waiter_threads,
                          &rw->w_num_waiters);
}

/* --- Write lock ---------------------------------------------------------- */

void rwlock_write_lock(rwlock_t *rw)
{
    /* Signal intent -- prevents new readers from acquiring */
    atomic_set(&rw->writer_pending, 1);

    /* Block until no readers and no other writer */
    while (atomic_read(&rw->reader_count) > 0 || atomic_read(&rw->writer_held)) {
        enqueue_and_block(rw->w_waiter_tasks, rw->w_waiter_threads,
                          &rw->w_num_waiters, RWLOCK_MAX_WAITERS);
    }

    /* Acquire */
    atomic_set(&rw->writer_held,    1);
    atomic_set(&rw->writer_pending, 0);  /* we hold it now -- clear pending */
}

void rwlock_write_unlock(rwlock_t *rw)
{
    if (!atomic_read(&rw->writer_held)) {
        printk("[RWLOCK] write_unlock \"%s\": not held!\n",
               rw->name ? rw->name : "?");
        return;
    }

    atomic_set(&rw->writer_held, 0);

    if (rw->w_num_waiters > 0) {
        /*
         * Another writer is waiting -- hand the lock to it directly.
         * Set writer_pending so new readers don't sneak in before
         * the woken writer gets scheduled.
         */
        atomic_set(&rw->writer_pending, 1);
        wake_first_waiter(rw->w_waiter_tasks, rw->w_waiter_threads,
                          &rw->w_num_waiters);
    } else {
        /* No writers waiting -- wake all pending readers */
        atomic_set(&rw->writer_pending, 0);
        wake_all_waiters(rw->r_waiter_tasks, rw->r_waiter_threads,
                         &rw->r_num_waiters);
    }
}

/* --- Non-blocking variants ---------------------------------------------- */

int rwlock_try_read(rwlock_t *rw)
{
    if (atomic_read(&rw->writer_held) || atomic_read(&rw->writer_pending))
        return 0;

    atomic_inc(&rw->reader_count);
    return 1;
}

int rwlock_try_write(rwlock_t *rw)
{
    if (atomic_read(&rw->reader_count) > 0 || atomic_read(&rw->writer_held))
        return 0;

    atomic_set(&rw->writer_held,    1);
    atomic_set(&rw->writer_pending, 0);
    return 1;
}
