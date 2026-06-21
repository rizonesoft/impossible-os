/* ============================================================================
 * ob_event.c -- Event object type: callbacks, NtCreateEvent stub
 *
 * ObpEventType wrapping event_t.
 * ============================================================================ */

#include "kernel/ob/ob_event.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Callbacks ----------------------------------------------------------- */

static void event_on_delete(void *body)
{
    EVENT_OBJECT *eo = (EVENT_OBJECT *)body;

    /* Wake any threads still blocked on this event so they don't hang */
    event_set(&eo->event);
}

/* --- Type registration --------------------------------------------------- */

void ob_event_type_init(void)
{
    extern const OBJECT_TYPE *ObpEventType;

    ObpEventType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Event",
        .body_size = sizeof(EVENT_OBJECT),
        .on_close  = NULL,
        .on_delete = event_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpEventType)
        klog(LOG_ERROR, "ob", "Failed to register ObpEventType");
}

/* --- NtCreateEvent stub -------------------------------------------------- */

HANDLE NtCreateEvent(HANDLE_TABLE *ht, const char *name,
                     event_type_t type, int initial_state)
{
    EVENT_OBJECT *eo;
    HANDLE h;

    if (!ht)
        return INVALID_HANDLE_VALUE;

    /* If named, try to open existing */
    if (name) {
        void *existing = NULL;
        char path[128];
        extern int snprintf(char *buf, size_t size, const char *fmt, ...);
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

        if (ObLookupObjectByName(path, ObpEventType, 0, &existing) == 0
            && existing) {
            h = ObpAllocateHandle(ht, existing, 0, 0);
            ObDereferenceObject(existing);  /* drop lookup ref */
            return h;
        }
    }

    /* Create new event object */
    eo = (EVENT_OBJECT *)ob_alloc_object(ObpEventType);
    if (!eo)
        return INVALID_HANDLE_VALUE;

    event_init(&eo->event, name ? name : "ob_event", type, initial_state);

    /* Insert into \BaseNamedObjects if named */
    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            if (ObInsertObject(eo, name, bno_dir) < 0) {
                /* Name-collision race: another thread inserted the same name
                 * between our lookup-miss and this insert. Redirect the caller
                 * to the winner and free our loser so one name == one object
                 * (mirrors ObCreateTimerEx). */
                void *winner = NULL;
                char wpath[128];
                ObDereferenceObject(bno_dir);
                snprintf(wpath, sizeof(wpath), "\\BaseNamedObjects\\%s", name);
                if (ObLookupObjectByName(wpath, ObpEventType, 0, &winner) == 0
                    && winner) {
                    h = ObpAllocateHandle(ht, winner, 0, 0);
                    ObDereferenceObject(winner);
                    ObDereferenceObject(eo);
                    return h;
                }
                ObDereferenceObject(eo);
                return INVALID_HANDLE_VALUE;
            }
            ObDereferenceObject(bno_dir);
        }
    }

    h = ObpAllocateHandle(ht, eo, 0, 0);
    ObDereferenceObject(eo);  /* drop creation ref */
    return h;
}
