/* ============================================================================
 * ob_timer.h -- Timer object type for the Object Manager
 *
 * TIMER_OBJECT holds a signalling event plus arm/period state. Named timers
 * are inserted into \BaseNamedObjects. The NT timer subsystem (src/kernel/nt/
 * nt_timer.c) walks a global list of armed timers from the tick ISR and
 * signals each `event` when due_ns is reached.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/event.h"
#include "kernel/ob/handle_table.h"

/* Windows timer types (NtCreateTimer TimerType argument). */
#define TIMER_TYPE_NOTIFICATION     0u  /* manual-reset: all waiters released */
#define TIMER_TYPE_SYNCHRONIZATION  1u  /* auto-reset: one waiter released    */

/* --- TIMER_OBJECT body ---------------------------------------------------
 *
 * next_armed / on_queue / armed_seq are owned by nt_timer.c and protected
 * by the global armed-timer spinlock in that file. All other fields are
 * set at create time or under the same lock when the timer is armed.
 * ------------------------------------------------------------------------- */

typedef struct timer_object {
    event_t       event;         /* signalled when timer fires               */
    uint64_t      due_ns;        /* absolute fire time (uptime_ns)           */
    uint64_t      period_ms;     /* 0 = one-shot, >0 = periodic (ms)         */
    uint32_t      active;        /* 1 = armed (on queue or pending fire)     */
    uint32_t      timer_type;    /* TIMER_TYPE_NOTIFICATION/SYNCHRONIZATION  */

    /* Armed-list link; owned by nt_timer.c. Do not touch outside that file. */
    struct timer_object *next_armed;
    uint32_t      on_queue;      /* 1 = currently linked into armed list     */

    /* APC delivery (deferred: APC infra not yet implemented).
     * These pointers are recorded by NtSetTimer so later APC enablement
     * can deliver without an ABI change. */
    void         *apc_routine;   /* PTIMER_APC_ROUTINE                       */
    void         *apc_context;   /* routine context arg                      */
} TIMER_OBJECT;

/* --- API ----------------------------------------------------------------- */

void ob_timer_type_init(void);

/*
 * ObCreateTimerEx -- create or open a named/unnamed timer with type +
 * desired_access. Used by NtCreateTimer / NtOpenTimer handlers.
 *
 * name:       NULL for unnamed, or ASCII name relative to \BaseNamedObjects.
 * timer_type: TIMER_TYPE_NOTIFICATION or TIMER_TYPE_SYNCHRONIZATION.
 * access:     ACCESS_MASK stored on the handle.
 *
 * Returns INVALID_HANDLE_VALUE on failure.
 */
HANDLE ObCreateTimerEx(HANDLE_TABLE *ht, const char *name,
                       uint32_t timer_type, uint32_t access);

/*
 * ObOpenTimer -- open an existing named timer.
 */
HANDLE ObOpenTimer(HANDLE_TABLE *ht, const char *name, uint32_t access);
