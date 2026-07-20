/* ============================================================================
 * ob_timer.c -- Timer object type: callbacks, ObCreateTimerEx / ObOpenTimer
 *
 * ObpTimerType with event signalling.
 * Named timers live in \BaseNamedObjects and are looked up by
 * ObLookupObjectByName on open.
 * ============================================================================ */

#include "kernel/ob/ob_timer.h"
#include "kernel/ob/ob.h"
#include "kernel/klog.h"
#include "kernel/sched/task.h"   /* task_current, task_acct_note_timer_create */

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* Forward declaration: nt_timer.c removes a closing timer from the armed
 * list and quiesces its state. Weak-linked via an extern so ob_timer.c can
 * live independently of nt_timer.c during early boot. */
extern void nt_timer_detach(TIMER_OBJECT *to);

/* --- Callbacks ----------------------------------------------------------- */

static void timer_on_close(void *body, uint32_t handle_count)
{
    TIMER_OBJECT *to = (TIMER_OBJECT *)body;

    (void)handle_count;

    /* Last handle closed: remove from armed list and clear state. */
    nt_timer_detach(to);
    to->active = 0;
    to->due_ns = 0;
    to->period_ms = 0;
}

static void timer_on_delete(void *body)
{
    TIMER_OBJECT *to = (TIMER_OBJECT *)body;

    /* Defensive: detach in case the object was freed without passing
     * through on_close (refcount path via ObDereferenceObject). */
    nt_timer_detach(to);
    to->active = 0;

    /* Wake anyone waiting on the event so they do not hang. */
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

/* --- ObCreateTimerEx ----------------------------------------------------- */

HANDLE ObCreateTimerEx(HANDLE_TABLE *ht, const char *name,
                       uint32_t timer_type, uint32_t access)
{
    TIMER_OBJECT *to;
    HANDLE h;
    int event_kind;
    int published = 0;      /* object reachable by name, independent of a handle */

    if (!ht)
        return INVALID_HANDLE_VALUE;
    if (timer_type > TIMER_TYPE_SYNCHRONIZATION)
        return INVALID_HANDLE_VALUE;

    /* If named, honor "open existing" semantics. */
    if (name) {
        void *existing = NULL;
        char path[128];

        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

        if (ObLookupObjectByName(path, ObpTimerType, 0, &existing) == 0
            && existing) {
            h = ObpAllocateHandle(ht, existing, access, 0);
            ObDereferenceObject(existing);
            return h;
        }
    }

    to = (TIMER_OBJECT *)ob_alloc_object(ObpTimerType);
    if (!to)
        return INVALID_HANDLE_VALUE;

    event_kind = (timer_type == TIMER_TYPE_SYNCHRONIZATION)
                   ? EVENT_AUTO_RESET
                   : EVENT_MANUAL_RESET;
    event_init(&to->event, name ? name : "ob_timer", event_kind, 0);

    to->due_ns      = 0;
    to->period_ms   = 0;
    to->active      = 0;
    to->timer_type  = timer_type;
    to->next_armed  = (TIMER_OBJECT *)0;
    to->on_queue    = 0;
    to->apc_routine = (void *)0;
    to->apc_context = (void *)0;

    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            if (ObInsertObject(to, name, bno_dir) < 0) {
                /* Collision: another thread inserted the same name while
                 * we were setting up. Redirect to the winner. */
                void *winner = NULL;
                char path[128];

                ObDereferenceObject(bno_dir);
                snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);
                if (ObLookupObjectByName(path, ObpTimerType, 0, &winner) == 0
                    && winner) {
                    h = ObpAllocateHandle(ht, winner, access, 0);
                    ObDereferenceObject(winner);
                    ObDereferenceObject(to);
                    return h;
                }
                ObDereferenceObject(to);
                klog(LOG_WARN, "ob",
                     "Timer named-insert failed with no winner; aborting");
                return INVALID_HANDLE_VALUE;
            }
            ObDereferenceObject(bno_dir);
            published = 1;      /* the directory now holds a reference */
        }
    }

    h = ObpAllocateHandle(ht, to, access, 0);
    ObDereferenceObject(to);
    /* Count the timer-creation event for the calling process, at the OBJECT
     * PUBLICATION boundary rather than the handle boundary.
     *
     * Both are needed because a timer can outlive its handle attempt: a NAMED
     * timer is inserted into \BaseNamedObjects first, and that directory takes
     * its own reference, so the object stays alive and reachable by name even
     * if ObpAllocateHandle then fails (handle-table or handle-quota
     * exhaustion). Counting only on a returned handle would let a caller create
     * persistent named timers while every one of them was reported as a
     * failure and none reached the churn signal battery policy reads.
     *
     * The early returns above are still deliberately uncounted: they hand back
     * a handle to a timer that ALREADY existed (open-by-name, and the
     * named-insert collision redirect), and opening an existing timer creates
     * nothing. An unnamed timer whose handle allocation fails is likewise not
     * counted -- the deref above was its last reference, so nothing persists. */
    if (h != INVALID_HANDLE_VALUE || published)
        task_acct_note_timer_create(task_current());
    return h;
}

/* --- ObOpenTimer --------------------------------------------------------- */

HANDLE ObOpenTimer(HANDLE_TABLE *ht, const char *name, uint32_t access)
{
    void *body = NULL;
    char path[128];
    HANDLE h;

    if (!ht || !name)
        return INVALID_HANDLE_VALUE;

    snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);
    if (ObLookupObjectByName(path, ObpTimerType, 0, &body) != 0 || !body)
        return INVALID_HANDLE_VALUE;

    h = ObpAllocateHandle(ht, body, access, 0);
    ObDereferenceObject(body);
    return h;
}

