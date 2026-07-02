/* ============================================================================
 * event.h -- Kernel wait/event objects
 *
 * Two variants:
 *   EVENT_MANUAL_RESET -- event_set() wakes ALL waiters and the event stays
 *                        set until event_reset() is called explicitly.
 *                        Equivalent to Windows manual-reset KEVENT.
 *
 *   EVENT_AUTO_RESET   -- event_set() wakes exactly ONE waiter and
 *                        automatically clears itself.  If there are no
 *                        waiters the event stays set until the next
 *                        event_wait() call consumes it.
 *                        Equivalent to Windows auto-reset KEVENT.
 *
 * Usage:
 *   event_t disk_ready = EVENT_INIT("disk_ready", EVENT_MANUAL_RESET, 0);
 *
 *   // Producer (e.g. AHCI init):
 *   event_set(&disk_ready);
 *
 *   // Consumer (e.g. VFS mount):
 *   event_wait(&disk_ready);
 *
 * Timeout variant:
 *   int ok = event_wait_timeout(&disk_ready, 5000);  // 5 s
 *   if (!ok) { panic("disk init timed out"); }
 *
 * IRQ safety:
 *   event_set() may be called from IRQ context (e.g. vsync from PIT
 *   callback).  The function only sets a flag and calls thread_ready()
 *   -- both are safe from IRQ context.
 *   event_wait() MUST be called from thread context only (it calls yield()).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/atomic.h"    /* atomic_t, atomic_read, atomic_set */

/* ---------------------------------------------------------------------------
 * Event type
 * ------------------------------------------------------------------------- */
typedef enum {
    EVENT_MANUAL_RESET,   /* stays set until event_reset(); wakes all waiters */
    EVENT_AUTO_RESET,     /* clears on first wake; wakes exactly one waiter   */
} event_type_t;

/* ---------------------------------------------------------------------------
 * Maximum simultaneous waiters on a single event
 * ------------------------------------------------------------------------- */
#define EVENT_MAX_WAITERS  16

/* ---------------------------------------------------------------------------
 * event_t
 * ------------------------------------------------------------------------- */
typedef struct event {
    atomic_t      state;       /* 1 = signalled, 0 = not signalled */
    event_type_t  type;        /* MANUAL_RESET or AUTO_RESET */

    /* Wait queue -- tasks/threads sleeping in event_wait() */
    uint32_t  waiter_tasks[EVENT_MAX_WAITERS];
    uint32_t  waiter_threads[EVENT_MAX_WAITERS];
    uint32_t  num_waiters;

    const char *name;          /* debug name */
} event_t;

/* Static initializers */
#define EVENT_INIT(n, t, s)  \
    { .state = ATOMIC_INIT(s), .type = (t), .num_waiters = 0, .name = (n) }

#define DEFINE_EVENT(name_id, type, initial_state) \
    event_t name_id = EVENT_INIT(#name_id, type, initial_state)

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Initialize an event at runtime. */
void event_init(event_t *ev, const char *name,
                event_type_t type, int initial_state);

/* Block until the event is signalled.
 * For AUTO_RESET: the event is cleared before this function returns.
 * For MANUAL_RESET: the event stays set; all waiters are woken together.
 * MUST be called from thread context only (uses yield()). */
void event_wait(event_t *ev);

/* Signal the event.
 *   MANUAL_RESET: wakes ALL waiters, event stays set.
 *   AUTO_RESET:   wakes ONE waiter (or remains set if no waiters).
 * Safe to call from IRQ context. */
void event_set(event_t *ev);

/* Clear the event (for MANUAL_RESET events only).
 * Has no effect on AUTO_RESET events (they self-clear). */
void event_reset(event_t *ev);

/* Wait with a timeout.
 * Returns 1 if the event was signalled before the timeout expired.
 * Returns 0 on timeout.
 * MUST be called from thread context only. */
int event_wait_timeout(event_t *ev, uint32_t timeout_ms);

/* Query state without blocking. Returns 1 if the event is set. */
int event_is_set(const event_t *ev);

/* Non-blocking consuming acquire.
 * AUTO_RESET: atomically claims the signal (CAS 1 -> 0); exactly one
 * concurrent caller wins a single signal. Returns 1 if claimed.
 * MANUAL_RESET: non-consuming peek, identical to event_is_set(). */
int event_try_consume(event_t *ev);
