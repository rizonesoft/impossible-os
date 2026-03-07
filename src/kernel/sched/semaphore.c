/* ============================================================================
 * semaphore.c — Kernel counting semaphore
 *
 * Blocking counting semaphore with FIFO wait queue.
 *
 * sem_wait():
 *   - If count > 0: decrement and return immediately
 *   - If count <= 0: add thread to wait queue, block, yield
 *
 * sem_signal():
 *   - Increment count
 *   - If waiters are queued: wake the first one (FIFO)
 * ============================================================================ */

#include "kernel/sched/semaphore.h"
#include "kernel/sched/task.h"
#include "kernel/printk.h"

void sem_init(semaphore_t *s, const char *name, int32_t initial_count)
{
    s->count = initial_count;
    s->num_waiters = 0;
    s->name = name;
}

void sem_wait(semaphore_t *s)
{
    /* Fast path: count > 0, just decrement */
    while (s->count <= 0) {
        /* Add to wait queue */
        if (s->num_waiters < SEM_MAX_WAITERS) {
            struct task *cur = task_current();
            struct thread *thr = thread_current();

            s->waiter_tasks[s->num_waiters] = cur->pid;
            s->waiter_threads[s->num_waiters] = thr ? thr->id : 0;
            s->num_waiters++;

            /* Block and yield */
            if (thr) {
                thr->state = THREAD_BLOCKED;
            }
            yield();
        } else {
            /* Wait queue full — spin-yield as fallback */
            yield();
        }
    }

    /* Decrement the count */
    s->count--;
}

void sem_signal(semaphore_t *s)
{
    uint32_t i;

    /* Increment the count */
    s->count++;

    /* Wake the first waiter if any */
    if (s->num_waiters > 0) {
        uint32_t wake_task = s->waiter_tasks[0];
        uint32_t wake_thread = s->waiter_threads[0];

        /* Remove from queue by shifting */
        for (i = 1; i < s->num_waiters; i++) {
            s->waiter_tasks[i - 1] = s->waiter_tasks[i];
            s->waiter_threads[i - 1] = s->waiter_threads[i];
        }
        s->num_waiters--;

        /* Wake the thread */
        {
            struct task *wt = task_get_by_pid(wake_task);
            if (wt && wake_thread < wt->num_threads) {
                wt->threads[wake_thread].state = THREAD_READY;
            }
        }
    }
}

int sem_trywait(semaphore_t *s)
{
    if (s->count <= 0)
        return 0;  /* would block */

    s->count--;
    return 1;  /* success */
}

int32_t sem_value(semaphore_t *s)
{
    return s->count;
}
