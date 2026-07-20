/* ============================================================================
 * condvar.c -- Kernel condition variable
 *
 * cond_wait() sequence:
 *   1. Enqueue calling thread in condvar's wait queue
 *   2. Set thread state to THREAD_BLOCKED
 *   3. Release the mutex (mutex_unlock)
 *   4. yield() -- scheduler skips this thread until it is woken
 *   5. On return from yield(): re-acquire the mutex (mutex_lock)
 *
 * cond_signal() sequence:
 *   1. Wake the first waiter (set its thread state to THREAD_READY)
 *   2. Remove it from the wait queue
 *   (The woken thread will re-acquire the mutex in step 5 above)
 *
 * cond_broadcast() sequence:
 *   1. Wake ALL waiters simultaneously
 *   2. Clear the wait queue
 *   (All woken threads will compete to re-acquire the mutex -- first wins)
 * ============================================================================ */

#include "kernel/sched/condvar.h"
#include "kernel/sched/task.h"
#include "kernel/printk.h"

void cond_init(condvar_t *cond, const char *name)
{
    cond->num_waiters = 0;
    cond->name        = name;
}

void cond_wait(condvar_t *cond, mutex_t *mutex)
{
    struct task   *cur = task_current();
    struct thread *thr = thread_current();
    uint32_t i;

    /* Enqueue ourselves in the condvar wait queue */
    if (cond->num_waiters < CONDVAR_MAX_WAITERS) {
        cond->waiter_tasks[cond->num_waiters]   = cur->pid;
        cond->waiter_threads[cond->num_waiters] = thr ? thr->id : 0;
        cond->num_waiters++;
    } else {
        printk("[CONDVAR] \"%s\": wait queue full (max %d)!\n",
               cond->name ? cond->name : "?", CONDVAR_MAX_WAITERS);
        /* Degrade gracefully: spin-yield without blocking on the condvar */
        mutex_unlock(mutex);
        yield();
        mutex_lock(mutex);
        return;
    }

    /* Block the current thread */
    if (thr)
        thr->state = THREAD_BLOCKED;

    /* Atomically release the mutex before sleeping */
    mutex_unlock(mutex);

    /* Sleep -- scheduler will skip this thread until woken by signal/broadcast */
    yield();

    /* Woken up -- re-acquire the mutex before returning to caller */
    mutex_lock(mutex);

    /* Remove ourselves from the wait queue (we may still be in it if we were
     * woken by a spurious wake-up or broadcast -- scan and remove our entry) */
    for (i = 0; i < cond->num_waiters; i++) {
        if (cond->waiter_tasks[i]   == cur->pid &&
            cond->waiter_threads[i] == (thr ? thr->id : 0))
        {
            uint32_t j;
            for (j = i + 1; j < cond->num_waiters; j++) {
                cond->waiter_tasks[j - 1]   = cond->waiter_tasks[j];
                cond->waiter_threads[j - 1] = cond->waiter_threads[j];
            }
            cond->num_waiters--;
            break;
        }
    }
}

void cond_signal(condvar_t *cond)
{
    uint32_t i;

    if (cond->num_waiters == 0)
        return;

    /* Wake the first waiter (FIFO) */
    {
        struct task *wt = task_get_by_pid(cond->waiter_tasks[0]);
        if (wt && cond->waiter_threads[0] < wt->num_threads)
            task_wake_thread(wt, cond->waiter_threads[0]);
    }

    /* Remove from queue by shifting left */
    for (i = 1; i < cond->num_waiters; i++) {
        cond->waiter_tasks[i - 1]   = cond->waiter_tasks[i];
        cond->waiter_threads[i - 1] = cond->waiter_threads[i];
    }
    cond->num_waiters--;
}

void cond_broadcast(condvar_t *cond)
{
    uint32_t i;

    /* Wake every waiter */
    for (i = 0; i < cond->num_waiters; i++) {
        struct task *wt = task_get_by_pid(cond->waiter_tasks[i]);
        if (wt && cond->waiter_threads[i] < wt->num_threads)
            task_wake_thread(wt, cond->waiter_threads[i]);
    }

    /* Clear the wait queue -- all waiters will compete for the mutex */
    cond->num_waiters = 0;
}
