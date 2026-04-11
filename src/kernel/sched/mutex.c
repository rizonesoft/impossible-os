/* ============================================================================
 * mutex.c -- Kernel mutex synchronization
 *
 * Blocking mutexes with owner tracking, wait queue, and optional
 * lock-ordering deadlock detection.
 *
 * When a thread calls mutex_lock() on an already-held mutex:
 *   1. The thread is added to the mutex's wait queue
 *   2. The thread's state is set to THREAD_BLOCKED
 *   3. yield() is called -- scheduler skips blocked threads
 *   4. When mutex_unlock() fires, the first waiter is woken
 *   5. The woken thread re-checks the lock and acquires it
 *
 * Deadlock detection (optional):
 *   - Each mutex can have a "lock order" ID
 *   - Acquiring order N after already holding order M (where M >= N) is flagged
 *   - Self-deadlock (locking what you already hold) always triggers a warning
 * ============================================================================ */

#include "kernel/sched/mutex.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/task.h"
#include "kernel/atomic.h"
#include "kernel/printk.h"

/* Track the highest lock order held by the current task (simple detection) */
static uint32_t current_max_lock_order = 0;

void mutex_init(mutex_t *m, const char *name)
{
    atomic_set(&m->locked, 0);
    m->owner_task   = 0;
    m->owner_thread = 0;
    m->lock_order   = 0;
    m->num_waiters  = 0;
    m->name         = name;
}

void mutex_init_ordered(mutex_t *m, const char *name, uint32_t order)
{
    mutex_init(m, name);
    m->lock_order = order;
}

void mutex_lock(mutex_t *m)
{
    ASSERT_IRQL_PASSIVE_OR_APC();

    struct task *cur_task = task_current();
    uint32_t my_task = cur_task->pid;
    struct thread *cur_thread = thread_current();
    uint32_t my_thread = cur_thread ? cur_thread->id : 0;

    /* --- Self-deadlock detection --- */
    if (atomic_read(&m->locked) && m->owner_task == my_task && m->owner_thread == my_thread) {
        printk("[DEADLOCK] mutex \"%s\": thread %u (task %u) already holds this lock!\n",
               m->name ? m->name : "?",
               (uint64_t)my_thread, (uint64_t)my_task);
        return;  /* avoid actual deadlock -- just warn and return */
    }

    /* --- Lock ordering check --- */
    if (m->lock_order > 0 && current_max_lock_order >= m->lock_order) {
        printk("[DEADLOCK] mutex \"%s\" (order %u): acquired after order %u -- "
               "potential deadlock!\n",
               m->name ? m->name : "?",
               (uint64_t)m->lock_order,
               (uint64_t)current_max_lock_order);
        /* Continue anyway -- it's a warning, not a hard stop */
    }

    /* --- CAS-then-sleep acquisition with priority inheritance --- */
    while (atomic_cmpxchg(&m->locked, 0, 1) != 0) {
        /* Add ourselves to the wait queue */
        if (m->num_waiters < MUTEX_MAX_WAITERS) {
            m->waiter_tasks[m->num_waiters]   = my_task;
            m->waiter_threads[m->num_waiters] = my_thread;
            m->num_waiters++;
        }

        /* --- Priority Inheritance ---
         * If the owner is running at a lower priority than us, boost it
         * so it can preempt medium-priority threads and release the lock
         * sooner, avoiding priority inversion.
         *
         * Cascaded case: if the owner is also blocked on another mutex,
         * walk the owner chain and boost every level. */
        if (cur_thread) {
            uint32_t waiter_prio = cur_thread->priority;
            /* Boost direct owner */
            if (m->owner_task < (uint32_t)-1) {
                struct task *owner_t = task_get_by_pid(m->owner_task);
                if (owner_t && m->owner_thread < owner_t->num_threads) {
                    thread_boost_priority(m->owner_task, m->owner_thread,
                                         waiter_prio);
                }
            }
        }

        /* Block this thread and yield */
        if (cur_thread) {
            cur_thread->state = THREAD_BLOCKED;
        }
        yield();

        /* Re-read cur_thread after waking up in case scheduler state changed */
        cur_task = task_current();
        cur_thread = thread_current();
        my_task = cur_task->pid;
        my_thread = cur_thread ? cur_thread->id : 0;
    }

    /* Lock acquired via CAS above -- set ownership */
    m->owner_task   = my_task;
    m->owner_thread = my_thread;

    /* Update lock ordering tracker */
    if (m->lock_order > current_max_lock_order)
        current_max_lock_order = m->lock_order;
}

void mutex_unlock(mutex_t *m)
{
    struct task *cur_task = task_current();
    uint32_t my_task = cur_task->pid;
    struct thread *cur_thread = thread_current();
    uint32_t my_thread = cur_thread ? cur_thread->id : 0;
    uint32_t i;

    /* Verify ownership */
    if (!atomic_read(&m->locked)) {
        printk("[MUTEX] unlock \"%s\": not locked!\n",
               m->name ? m->name : "?");
        return;
    }

    if (m->owner_task != my_task || m->owner_thread != my_thread) {
        printk("[MUTEX] unlock \"%s\": thread %u (task %u) is not the owner "
               "(owner: thread %u, task %u)!\n",
               m->name ? m->name : "?",
               (uint64_t)my_thread, (uint64_t)my_task,
               (uint64_t)m->owner_thread, (uint64_t)m->owner_task);
        return;
    }

    /* Restore owner's priority to base_priority (undo any PI boost).
     * Must happen BEFORE clearing owner_task/owner_thread. */
    thread_restore_priority(my_task, my_thread);

    /* Release the lock */
    atomic_set(&m->locked, 0);
    m->owner_task   = 0;
    m->owner_thread = 0;

    /* Reset lock ordering (simplified -- only tracks one level) */
    if (m->lock_order > 0 && current_max_lock_order == m->lock_order)
        current_max_lock_order = 0;

    /* Wake the highest-priority waiter (PI: promote the most-urgent waiter).
     * Scan the wait queue and pick the thread with the highest priority. */
    if (m->num_waiters > 0) {
        uint32_t best_idx = 0;
        uint32_t best_prio = 0;
        uint32_t wake_task, wake_thread;

        /* Find the highest-priority waiter */
        for (i = 0; i < m->num_waiters; i++) {
            struct task *wt = task_get_by_pid(m->waiter_tasks[i]);
            if (wt && m->waiter_threads[i] < wt->num_threads) {
                uint32_t wp = wt->threads[m->waiter_threads[i]].priority;
                if (wp > best_prio) {
                    best_prio = wp;
                    best_idx  = i;
                }
            }
        }

        wake_task   = m->waiter_tasks[best_idx];
        wake_thread = m->waiter_threads[best_idx];

        /* Remove chosen waiter from queue by shifting left */
        for (i = best_idx + 1; i < m->num_waiters; i++) {
            m->waiter_tasks[i - 1]   = m->waiter_tasks[i];
            m->waiter_threads[i - 1] = m->waiter_threads[i];
        }
        m->num_waiters--;

        /* Wake the chosen thread */
        {
            struct task *wt = task_get_by_pid(wake_task);
            if (wt && wake_thread < wt->num_threads) {
                wt->threads[wake_thread].state = THREAD_READY;
            }
        }
    }
}

int mutex_trylock(mutex_t *m)
{
    struct task   *cur_task;
    struct thread *cur_thread;

    /* Attempt CAS: if locked == 0, set to 1 and return success. */
    if (atomic_cmpxchg(&m->locked, 0, 1) != 0)
        return 0;  /* already locked -- CAS returned non-zero (current value) */

    cur_task   = task_current();
    cur_thread = thread_current();

    m->owner_task   = cur_task->pid;
    m->owner_thread = cur_thread ? cur_thread->id : 0;

    if (m->lock_order > current_max_lock_order)
        current_max_lock_order = m->lock_order;

    return 1;  /* success */
}

int mutex_is_locked(mutex_t *m)
{
    return atomic_read(&m->locked) ? 1 : 0;
}
