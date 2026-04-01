/* ============================================================================
 * ob_event.h — Event object type for the Object Manager
 *
 * EVENT_OBJECT wraps an embedded event_t.  Named events are inserted into
 * \BaseNamedObjects for cross-process sharing.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/event.h"
#include "kernel/ob/handle_table.h"

/* --- EVENT_OBJECT body --------------------------------------------------- */

typedef struct event_object {
    event_t event;   /* embedded kernel event (not a pointer) */
} EVENT_OBJECT;

/* --- API ----------------------------------------------------------------- */

void ob_event_type_init(void);

/*
 * NtCreateEvent stub — create or open a named/unnamed event.
 *
 * name:          NULL for unnamed, or a name in \BaseNamedObjects.
 * type:          EVENT_MANUAL_RESET or EVENT_AUTO_RESET.
 * initial_state: 1 = signalled, 0 = not signalled.
 *
 * If name is non-NULL and an event with that name already exists,
 * opens the existing event (returns a new HANDLE to it).
 * Returns INVALID_HANDLE_VALUE on failure.
 */
HANDLE NtCreateEvent(HANDLE_TABLE *ht, const char *name,
                     event_type_t type, int initial_state);
