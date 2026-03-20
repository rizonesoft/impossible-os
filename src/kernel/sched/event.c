/* ============================================================================
 * event.c — Kernel wait/event objects
 *
 * Implements:
 *   EVENT_MANUAL_RESET — stays signalled until event_reset(); wakes ALL waiters
 *   EVENT_AUTO_RESET   — clears on first wake; wakes exactly ONE waiter
 *
 * event_set() is safe to call from IRQ context — it only writes flags and
 * sets thread state to THREAD_READY.  event_wait() must be from thread
 * context only (it calls yield()).
 * ============================================================================ */

#include "kernel/sched/event.h"
#include "kernel/sched/task.h"
#include "kernel/timer.h"         /* system_get_ticks(), system_get_freq() */
#include "kernel/printk.h"

/* ---------------------------------------------------------------------------
 * Internal: wake all threads in the wait queue.
 * ------------------------------------------------------------------------- */
static void wake_all_waiters(event_t *ev)
{
    uint32_t i;
    for (i = 0; i < ev->num_waiters; i++) {
        struct task *t = task_get_by_pid(ev->waiter_tasks[i]);
        if (t && ev->waiter_threads[i] < t->num_threads)
            t->threads[ev->waiter_threads[i]].state = THREAD_READY;
    }
    ev->num_waiters = 0;
}

/* ---------------------------------------------------------------------------
 * Internal: wake only the first (oldest) waiter; shift queue left.
 * ------------------------------------------------------------------------- */
static void wake_first_waiter(event_t *ev)
{
    uint32_t i;
    uint32_t wake_task, wake_thread;

    if (ev->num_waiters == 0)
        return;

    wake_task   = ev->waiter_tasks[0];
    wake_thread = ev->waiter_threads[0];

    /* Shift queue left */
    for (i = 1; i < ev->num_waiters; i++) {
        ev->waiter_tasks[i - 1]   = ev->waiter_tasks[i];
        ev->waiter_threads[i - 1] = ev->waiter_threads[i];
    }
    ev->num_waiters--;

    /* Set thread ready */
    {
        struct task *t = task_get_by_pid(wake_task);
        if (t && wake_thread < t->num_threads)
            t->threads[wake_thread].state = THREAD_READY;
    }
}

/* ---------------------------------------------------------------------------
 * Internal: enqueue the current thread in the wait queue and block.
 * ------------------------------------------------------------------------- */
static void enqueue_and_block(event_t *ev)
{
    struct task   *cur = task_current();
    struct thread *thr = thread_current();

    if (ev->num_waiters < EVENT_MAX_WAITERS) {
        ev->waiter_tasks[ev->num_waiters]   = cur->pid;
        ev->waiter_threads[ev->num_waiters] = thr ? thr->id : 0;
        ev->num_waiters++;
    }
    /* else: queue full — fall through to yield as spin-fallback */

    if (thr)
        thr->state = THREAD_BLOCKED;
    yield();
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
}

/* ---------------------------------------------------------------------------
 * event_wait — block until event is signalled
 *
 * MANUAL_RESET: woken along with all other waiters; event stays set.
 * AUTO_RESET:   woken alone; caller atomically "consumes" the signal.
 *
 * Must be called from thread context only.
 * ------------------------------------------------------------------------- */
void event_wait(event_t *ev)
{
    while (!atomic_read(&ev->state)) {
        enqueue_and_block(ev);
        /* Re-check on wake — could have been a spurious wakeup due to
         * wait-queue overflow or scheduler reschedule. */
    }

    /* AUTO_RESET: consume the signal — clear before returning. */
    if (ev->type == EVENT_AUTO_RESET)
        atomic_set(&ev->state, 0);
}

/* ---------------------------------------------------------------------------
 * event_set — signal the event
 *
 * MANUAL_RESET: set state, wake ALL waiters.
 * AUTO_RESET:   if waiters exist, wake ONE and leave state=0;
 *               if no waiters, set state=1 (consumed by next event_wait).
 *
 * Safe from IRQ context.
 * ------------------------------------------------------------------------- */
void event_set(event_t *ev)
{
    if (ev->type == EVENT_MANUAL_RESET) {
        atomic_set(&ev->state, 1);
        wake_all_waiters(ev);
    } else {
        /* AUTO_RESET */
        if (ev->num_waiters > 0) {
            /* Don't set state — wake one waiter, let event_wait clear it */
            atomic_set(&ev->state, 1);
            wake_first_waiter(ev);
        } else {
            /* No waiters: set state so the next event_wait() returns
             * immediately and self-clears. */
            atomic_set(&ev->state, 1);
        }
    }
}

/* ---------------------------------------------------------------------------
 * event_reset — clear a MANUAL_RESET event
 * ------------------------------------------------------------------------- */
void event_reset(event_t *ev)
{
    atomic_set(&ev->state, 0);
}

/* ---------------------------------------------------------------------------
 * event_wait_timeout — wait with a deadline in milliseconds
 *
 * Returns 1 if the event was signalled before timeout, 0 on timeout.
 * Must be called from thread context only.
 * ------------------------------------------------------------------------- */
int event_wait_timeout(event_t *ev, uint32_t timeout_ms)
{
    uint64_t start   = system_get_ticks();
    uint32_t freq    = system_get_freq();
    uint64_t timeout = freq ? ((uint64_t)timeout_ms * freq) / 1000 : 0;

    while (!atomic_read(&ev->state)) {
        /* Check deadline before blocking */
        if ((system_get_ticks() - start) >= timeout)
            return 0;  /* timed out */

        enqueue_and_block(ev);
    }

    /* Consume signal for AUTO_RESET */
    if (ev->type == EVENT_AUTO_RESET)
        atomic_set(&ev->state, 0);

    return 1;
}

/* ---------------------------------------------------------------------------
 * event_is_set — non-blocking state query
 * ------------------------------------------------------------------------- */
int event_is_set(const event_t *ev)
{
    return atomic_read(&ev->state) ? 1 : 0;
}
