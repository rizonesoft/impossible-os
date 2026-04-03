/* ============================================================================
 * ob_timer.h -- Timer object type for the Object Manager
 *
 * TIMER_OBJECT holds a signalling event and timer state.  Named timers
 * are inserted into \BaseNamedObjects.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/event.h"
#include "kernel/ob/handle_table.h"

/* --- TIMER_OBJECT body --------------------------------------------------- */

typedef struct timer_object {
    event_t  event;       /* signalled when timer fires */
    uint64_t due_ns;      /* absolute due time (uptime_ns), 0 = not armed */
    uint64_t period_ms;   /* 0 = one-shot, >0 = periodic */
    uint32_t active;      /* 1 = armed */
} TIMER_OBJECT;

/* --- API ----------------------------------------------------------------- */

void ob_timer_type_init(void);

/*
 * NtCreateTimer stub -- create or open a named/unnamed timer.
 *
 * name: NULL for unnamed, or a name in \BaseNamedObjects.
 *
 * Returns INVALID_HANDLE_VALUE on failure.
 */
HANDLE NtCreateTimer(HANDLE_TABLE *ht, const char *name);
