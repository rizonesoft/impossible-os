/* ============================================================================
 * ob_timer.c — Timer object type: callbacks, NtCreateTimer stub
 *
 * Implements TODO-03 §6: ObpTimerType with event signalling.
 * ============================================================================ */

#include "kernel/ob/ob_timer.h"
#include "kernel/ob/ob.h"
#include "kernel/klog.h"

/* --- Callbacks ----------------------------------------------------------- */

static void timer_on_close(void *body, uint32_t handle_count)
{
    TIMER_OBJECT *to = (TIMER_OBJECT *)body;

    (void)handle_count;

    /* Cancel the timer when the last handle is closed */
    to->active = 0;
    to->due_ns = 0;
    to->period_ms = 0;
}

static void timer_on_delete(void *body)
{
    TIMER_OBJECT *to = (TIMER_OBJECT *)body;

    to->active = 0;
    /* Wake anyone waiting on the event so they don't hang */
    event_set(&to->event);
}

/* --- Type registration --------------------------------------------------- */

void ob_timer_type_init(void)
{
    extern const OBJECT_TYPE *ObpTimerType;

    ObpTimerType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Timer",
        .body_size = sizeof(TIMER_OBJECT),
        .on_close  = timer_on_close,
        .on_delete = timer_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpTimerType)
        klog(LOG_ERROR, "ob", "Failed to register ObpTimerType");
}

/* --- NtCreateTimer stub -------------------------------------------------- */

HANDLE NtCreateTimer(HANDLE_TABLE *ht, const char *name)
{
    TIMER_OBJECT *to;
    HANDLE h;

    if (!ht)
        return INVALID_HANDLE_VALUE;

    /* If named, try to open existing */
    if (name) {
        void *existing = NULL;
        char path[128];
        extern int snprintf(char *buf, size_t size, const char *fmt, ...);
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

        if (ObLookupObjectByName(path, ObpTimerType, 0, &existing) == 0
            && existing) {
            h = ObpAllocateHandle(ht, existing, 0, 0);
            ObDereferenceObject(existing);
            return h;
        }
    }

    /* Create new timer object */
    to = (TIMER_OBJECT *)ob_alloc_object(ObpTimerType);
    if (!to)
        return INVALID_HANDLE_VALUE;

    event_init(&to->event, name ? name : "ob_timer", EVENT_MANUAL_RESET, 0);
    to->due_ns    = 0;
    to->period_ms = 0;
    to->active    = 0;

    /* Insert into \BaseNamedObjects if named */
    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            ObInsertObject(to, name, bno_dir);
            ObDereferenceObject(bno_dir);
        }
    }

    h = ObpAllocateHandle(ht, to, 0, 0);
    ObDereferenceObject(to);
    return h;
}
