/* ============================================================================
 * semaphore.c -- Kernel counting semaphore
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
#include "kernel/sched/irql.h"
#include "kernel/sched/task.h"
void sem_init(semaphore_t *s, const char *name, int32_t initial_count)
{
    s->count = initial_count;
    s->num_waiters = 0;
    s->name = name;
}

void sem_wait(semaphore_t *s)
{
    ASSERT_IRQL_PASSIVE_OR_APC();

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
            /* Wait queue full -- spin-yield as fallback */
            yield();
        }
    }

    /* Decrement the count */
    s->count--;
}

/* Dequeue and ready the head waiter (caller has already published the
 * permit into s->count).  Shared by sem_signal and sem_signal_n so the
 * queue-shift + wake sequence stays identical across both. */
static void sem_wake_head(semaphore_t *s)
{
    uint32_t wake_task = s->waiter_tasks[0];
    uint32_t wake_thread = s->waiter_threads[0];
    uint32_t i;

    for (i = 1; i < s->num_waiters; i++) {
        s->waiter_tasks[i - 1] = s->waiter_tasks[i];
        s->waiter_threads[i - 1] = s->waiter_threads[i];
    }
    s->num_waiters--;

    {
        struct task *wt = task_get_by_pid(wake_task);
        if (wt && wake_thread < wt->num_threads)
            task_wake_thread(wt, wake_thread);
    }
}

void sem_signal(semaphore_t *s)
{
    /* Increment the count BEFORE checking waiters, so a thread racing into
     * sem_wait sees the permit instead of blocking. */
    s->count++;

    if (s->num_waiters > 0)
        sem_wake_head(s);
}

void sem_signal_n(semaphore_t *s, int32_t n)
{
    uint32_t budget;

    if (n <= 0)
        return;

    /* Pair each permit that has a queued waiter with that waiter's wake,
     * exactly as a sequence of sem_signal() calls would (count++ BEFORE
     * the wake).  Publishing per-permit preserves sem_signal's
     * count-before-waiter-check ordering: a thread racing into sem_wait
     * always observes the just-published permit rather than blocking.
     * The wake budget is SNAPSHOTTED (min of n and the waiters present
     * now) so a concurrent sem_wait replenishing num_waiters cannot extend
     * this loop back to O(release_count) -- it stays O(waiters). */
    budget = ((uint32_t)n < s->num_waiters) ? (uint32_t)n : s->num_waiters;
    while (budget > 0 && s->num_waiters > 0) {
        s->count++;
        sem_wake_head(s);
        n--;
        budget--;
    }

    if (n <= 0)
        return;

    /* Remaining permits have no queued waiter at the snapshot -- publish
     * the excess, then a snapshot-capped re-check for a thread that raced
     * into sem_wait between the loop above and this publish (it
     * enqueued+blocked seeing the old count; now that count is up it must
     * be readied, not stranded).  Budget = min(n, waiters-now) so a
     * concurrent producer cannot extend this loop either; surplus waiters
     * re-block in sem_wait (correct).  The remaining fully-atomic sem_wait
     * vs sem_signal_n serialization is the lockless-primitive atomicity
     * gap owned by the advanced-sync semaphore/event atomicity backfill. */
    s->count += n;
    budget = ((uint32_t)n < s->num_waiters) ? (uint32_t)n : s->num_waiters;
    while (budget > 0 && s->num_waiters > 0) {
        sem_wake_head(s);
        budget--;
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
