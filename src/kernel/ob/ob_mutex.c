/* ============================================================================
 * ob_mutex.c -- Mutex (Mutant) object type: callbacks, NtCreateMutex stub
 *
 * ObpMutexType wrapping mutex_t.
 * If the owning thread dies without releasing, the mutex is marked abandoned.
 * ============================================================================ */

#include "kernel/ob/ob_mutex.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Callbacks ----------------------------------------------------------- */

static void mutex_on_delete(void *body)
{
    MUTEX_OBJECT *mo = (MUTEX_OBJECT *)body;

    /* If the mutex is still held when the object is being destroyed,
     * mark it abandoned and force-unlock so waiters don't hang */
    if (atomic_read(&mo->mutex.locked)) {
        mo->abandoned = 1;
        mo->recursion = 0;
        klog(LOG_WARN, "ob", "Mutex '%s' abandoned (owner died)",
             mo->mutex.name ? mo->mutex.name : "?");
        atomic_set(&mo->mutex.locked, 0);
        /* Wake one waiter if any are blocked */
        if (mo->mutex.num_waiters > 0) {
            uint32_t tid = mo->mutex.waiter_tasks[0];
            uint32_t thid = mo->mutex.waiter_threads[0];
            uint32_t i;
            /* Shift waiters down */
            for (i = 0; i + 1 < mo->mutex.num_waiters; i++) {
                mo->mutex.waiter_tasks[i] = mo->mutex.waiter_tasks[i + 1];
                mo->mutex.waiter_threads[i] = mo->mutex.waiter_threads[i + 1];
            }
            mo->mutex.num_waiters--;
            /* Wake the waiter */
            struct task *t = task_get_by_pid(tid);
            if (t && thid < t->num_threads &&
                t->threads[thid].state == THREAD_BLOCKED) {
                t->threads[thid].state = THREAD_READY;
            }
        }
    }
}

/* --- Type registration --------------------------------------------------- */

void ob_mutex_type_init(void)
{
    extern const OBJECT_TYPE *ObpMutexType;

    ObpMutexType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Mutant",
        .body_size = sizeof(MUTEX_OBJECT),
        .on_close  = NULL,
        .on_delete = mutex_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpMutexType)
        klog(LOG_ERROR, "ob", "Failed to register ObpMutexType");
}

/* --- NtCreateMutex stub -------------------------------------------------- */

HANDLE NtCreateMutex(HANDLE_TABLE *ht, const char *name, int initial_owner)
{
    MUTEX_OBJECT *mo;
    HANDLE h;

    if (!ht)
        return INVALID_HANDLE_VALUE;

    /* If named, try to open existing */
    if (name) {
        void *existing = NULL;
        char path[128];
        extern int snprintf(char *buf, size_t size, const char *fmt, ...);
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

        if (ObLookupObjectByName(path, ObpMutexType, 0, &existing) == 0
            && existing) {
            h = ObpAllocateHandle(ht, existing, 0, 0);
            ObDereferenceObject(existing);
            return h;
        }
    }

    /* Create new mutex object */
    mo = (MUTEX_OBJECT *)ob_alloc_object(ObpMutexType);
    if (!mo)
        return INVALID_HANDLE_VALUE;

    mutex_init(&mo->mutex, name ? name : "ob_mutex");
    mo->abandoned = 0;
    mo->recursion = 0;

    /* If caller wants initial ownership, lock it now */
    if (initial_owner)
        mutex_lock(&mo->mutex);

    /* Insert into \BaseNamedObjects if named */
    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            if (ObInsertObject(mo, name, bno_dir) < 0) {
                /* Name-collision race: redirect to the winner, free our loser
                 * so one name == one mutex (mirrors ObCreateTimerEx). */
                void *winner = NULL;
                char wpath[128];
                ObDereferenceObject(bno_dir);
                snprintf(wpath, sizeof(wpath), "\\BaseNamedObjects\\%s", name);
                if (ObLookupObjectByName(wpath, ObpMutexType, 0, &winner) == 0
                    && winner) {
                    h = ObpAllocateHandle(ht, winner, 0, 0);
                    ObDereferenceObject(winner);
                    ObDereferenceObject(mo);
                    return h;
                }
                ObDereferenceObject(mo);
                return INVALID_HANDLE_VALUE;
            }
            ObDereferenceObject(bno_dir);
        }
    }

    h = ObpAllocateHandle(ht, mo, 0, 0);
    ObDereferenceObject(mo);
    return h;
}
