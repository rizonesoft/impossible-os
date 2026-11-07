/* ============================================================================
 * event.c -- Kernel wait/event objects
 *
 * Implements:
 *   EVENT_MANUAL_RESET -- stays signalled until event_reset(); wakes ALL waiters
 *   EVENT_AUTO_RESET   -- clears on first wake; wakes exactly ONE waiter
 *
 * event_set() is safe from IRQ context on any CPU: an irqsave spinlock guards
 * the wait queue, and waking only moves a thread to THREAD_READY. event_wait()
 * must be from thread context only (it calls yield()).
 * ============================================================================ */

#include "kernel/sched/event.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/task.h"
#include "kernel/timer.h"         /* system_get_ticks(), system_get_freq() */
/* ---------------------------------------------------------------------------
 * event_prepare_block: recheck state and join the queue as one step under the
 * lock. event_set() sets state and takes waiters under the same lock, so it
 * either happens first (seen here) or finds this thread queued and blocked.
 *
 * A thread woken by something other than this event may retry with its entry
 * still queued, so a retry reuses that entry instead of taking a second slot.
 * ------------------------------------------------------------------------- */
int event_prepare_block(event_t *ev)
{
    struct task   *cur = task_current();
    struct thread *thr = thread_current();
    uint64_t flags;
    uint32_t i;
    int result = EVENT_BLOCK_NO_SLOT;

    spin_lock_irqsave(&ev->lock, &flags);

    if (atomic_read(&ev->state)) {
        result = EVENT_BLOCK_SIGNALLED;
    } else if (cur && thr) {
        for (i = 0; i < ev->num_waiters; i++) {
            if (ev->waiter_tasks[i] == cur->pid &&
                ev->waiter_threads[i] == thr->id)
                break;
        }
        if (i == ev->num_waiters && i < EVENT_MAX_WAITERS) {
            ev->waiter_tasks[i]   = cur->pid;
            ev->waiter_threads[i] = thr->id;
            ev->num_waiters++;
        }
        if (i < ev->num_waiters) {
            thr->event_blocked_on = ev;
            __atomic_store_n(&thr->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
            result = EVENT_BLOCK_QUEUED;
        }
    }

    spin_unlock_irqrestore(&ev->lock, flags);
    return result;
}

/* ---------------------------------------------------------------------------
 * Internal: block on the event, or poll with a plain yield when no slot exists.
 * ------------------------------------------------------------------------- */
static void enqueue_and_block(event_t *ev)
{
    if (event_prepare_block(ev) != EVENT_BLOCK_SIGNALLED)
        yield();
}

/* ---------------------------------------------------------------------------
 * Internal: a consumer observed the signal without the lock, so the setter may
 * still be inside event_set(), which publishes state before it releases the lock.
 * Taking the lock here waits that out: once this returns the setter no longer
 * touches the event, so the caller may free it (the ALPC connect path does).
 * ------------------------------------------------------------------------- */
static void event_settle_after_consume(event_t *ev)
{
    uint64_t flags;

    spin_lock_irqsave(&ev->lock, &flags);
    spin_unlock_irqrestore(&ev->lock, flags);
}

/* Same handshake for event_wait(), which also ends this thread's wait: its entry
 * can still be queued if something other than this event woke it. */
static void event_finish_wait(event_t *ev)
{
    struct task   *cur = task_current();
    struct thread *thr = thread_current();
    uint64_t flags;
    uint32_t i;

    spin_lock_irqsave(&ev->lock, &flags);
    if (cur && thr) {
        thr->event_blocked_on = (struct event *)0;
        for (i = 0; i < ev->num_waiters; i++) {
            if (ev->waiter_tasks[i] == cur->pid &&
                ev->waiter_threads[i] == thr->id) {
                for (; i + 1 < ev->num_waiters; i++) {
                    ev->waiter_tasks[i]   = ev->waiter_tasks[i + 1];
                    ev->waiter_threads[i] = ev->waiter_threads[i + 1];
                }
                ev->num_waiters--;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&ev->lock, flags);
}

/* Is queue entry i a thread blocked on THIS event right now? Blocked alone is not
 * enough: the thread could be blocked on something else after leaving this queue
 * by another route, or be a new thread in a reused slot. */
static struct task *event_entry_live_task(const event_t *ev, uint32_t i)
{
    struct task *t = task_get_by_pid(ev->waiter_tasks[i]);
    struct thread *thr;

    if (!t || ev->waiter_threads[i] >= t->num_threads)
        return (struct task *)0;
    thr = &t->threads[ev->waiter_threads[i]];
    if (thr->event_blocked_on != ev ||
        __atomic_load_n(&thr->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED)
        return (struct task *)0;
    return t;
}

/* ---------------------------------------------------------------------------
 * event_init
 * ------------------------------------------------------------------------- */
void event_init(event_t *ev, const char *name,
                event_type_t type, int initial_state)
{
    atomic_set(&ev->state, initial_state ? 1 : 0);
    ev->type        = type;
    ev->num_waiters = 0;
    ev->name        = name;
    ev->lock.flag   = 0;
}

/* ---------------------------------------------------------------------------
 * event_wait -- block until event is signalled
 *
 * MANUAL_RESET: woken along with all other waiters; event stays set.
 * AUTO_RESET:   woken alone; caller atomically "consumes" the signal.
 *
 * Must be called from thread context only.
 * ------------------------------------------------------------------------- */
void event_wait(event_t *ev)
{
    ASSERT_IRQL_PASSIVE_OR_APC();

    if (ev->type == EVENT_AUTO_RESET) {
        /* CAS-claim the signal so exactly one waiter consumes it (a
         * read-then-clear consume lets two CPUs both observe state==1 and
         * both return for one signal).  Read-before-CAS matches
         * event_try_consume / event_wait_timeout: block while unsignalled,
         * only CAS once state is observed set. */
        for (;;) {
            if (atomic_read(&ev->state) &&
                atomic_cmpxchg(&ev->state, 1, 0) == 1) {
                event_finish_wait(ev);
                return;
            }
            enqueue_and_block(ev);
        }
    }

    while (!atomic_read(&ev->state)) {
        enqueue_and_block(ev);
        /* Re-check on wake -- could have been a spurious wakeup due to
         * wait-queue overflow or scheduler reschedule. */
    }
    event_finish_wait(ev);
}

/* ---------------------------------------------------------------------------
 * event_set -- signal the event
 *
 * MANUAL_RESET: set state, wake ALL waiters.
 * AUTO_RESET:   if waiters exist, wake ONE and leave state=0;
 *               if no waiters, set state=1 (consumed by next event_wait).
 *
 * Safe from IRQ context.
 * ------------------------------------------------------------------------- */
void event_set(event_t *ev)
{
    uint32_t i, consumed = 0;
    uint64_t flags;

    /* Waking is lock-free (a CAS plus relaxed counters), so it stays under the
     * lock and no blocked-state check here can go stale before the wake. Nothing
     * after the unlock touches the event: a consumer may free it from then on. */
    spin_lock_irqsave(&ev->lock, &flags);

    atomic_set(&ev->state, 1);

    for (i = 0; i < ev->num_waiters; i++) {
        struct task *t = event_entry_live_task(ev, i);

        consumed = i + 1;
        if (t) {
            task_wake_thread(t, ev->waiter_threads[i]);
            /* AUTO_RESET: the oldest thread still blocked here gets the wake.
             * Stale entries ahead of it are dropped, or the signal would go to
             * nobody while a real waiter stayed blocked. */
            if (ev->type == EVENT_AUTO_RESET)
                break;
        }
    }
    for (i = consumed; i < ev->num_waiters; i++) {
        ev->waiter_tasks[i - consumed]   = ev->waiter_tasks[i];
        ev->waiter_threads[i - consumed] = ev->waiter_threads[i];
    }
    ev->num_waiters -= consumed;

    spin_unlock_irqrestore(&ev->lock, flags);
}

/* ---------------------------------------------------------------------------
 * event_reset -- clear a MANUAL_RESET event
 * ------------------------------------------------------------------------- */
void event_reset(event_t *ev)
{
    atomic_set(&ev->state, 0);
}

/* ---------------------------------------------------------------------------
 * event_wait_timeout -- wait with a deadline in milliseconds
 *
 * Returns 1 if the event was signalled before timeout, 0 on timeout.
 * Must be called from thread context only.
 * ------------------------------------------------------------------------- */
int event_wait_timeout(event_t *ev, uint32_t timeout_ms)
{
    /* Absolute monotonic deadline: an entry-time frequency snapshot of
     * raw ticks is wrong across a KeSetTimerResolution rate change (a
     * 5 s wait at 100 Hz becomes ~250 ms if the rate moves to 2000 Hz) */
    uint64_t deadline = uptime_ns() + (uint64_t)timeout_ms * 1000000ULL;

    for (;;) {
        if (ev->type == EVENT_AUTO_RESET) {
            /* CAS-claim so exactly one waiter consumes each signal; the
             * read-before-CAS avoids exclusive cache-line traffic while
             * the event is unsignalled. */
            if (atomic_read(&ev->state) &&
                atomic_cmpxchg(&ev->state, 1, 0) == 1) {
                event_settle_after_consume(ev);
                return 1;
            }
        } else if (atomic_read(&ev->state)) {
            event_settle_after_consume(ev);
            return 1;
        }

        /* Check deadline BEFORE yielding */
        if (uptime_ns() >= deadline)
            return 0;  /* timed out */

        /* Cooperative yield: DON'T use enqueue_and_block() here.
         *
         * enqueue_and_block() sets THREAD_BLOCKED + yield(), which means
         * the thread is NEVER rescheduled unless event_set() fires from
         * an IRQ handler.  If the IRQ never fires (e.g. VirtualBox AHCI
         * MSI not delivered), the thread stays blocked forever and the
         * timeout check above never runs -- a permanent hang.
         *
         * Instead, use plain yield() which keeps the thread READY.
         * The scheduler will reschedule us, and we'll re-check the
         * event state and deadline.  This burns some CPU but guarantees
         * the timeout actually fires. */
        yield();
    }
}

/* ---------------------------------------------------------------------------
 * event_is_set -- non-blocking state query
 * ------------------------------------------------------------------------- */
int event_is_set(const event_t *ev)
{
    return atomic_read(&ev->state) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * event_try_consume -- non-blocking consuming acquire
 *
 * AUTO_RESET:   atomically claims the signal (CAS 1 -> 0). Exactly one
 *               concurrent caller wins a single signal.
 * MANUAL_RESET: non-consuming peek -- the event stays set for all waiters.
 * ------------------------------------------------------------------------- */
int event_try_consume(event_t *ev)
{
    if (ev->type == EVENT_AUTO_RESET) {
        /* Read-before-CAS: skip the locked compare-exchange (and its
         * cache-line exclusive-ownership traffic) when the event is not
         * signalled -- the common case for a multi-CPU poll loop. */
        if (!atomic_read(&ev->state))
            return 0;
        if (atomic_cmpxchg(&ev->state, 1, 0) != 1)
            return 0;
        event_settle_after_consume(ev);
        return 1;
    }
    if (!atomic_read(&ev->state))
        return 0;
    event_settle_after_consume(ev);
    return 1;
}
