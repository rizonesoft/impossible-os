/* ============================================================================
 * ob_semaphore.c -- Semaphore object type: callbacks, NtCreateSemaphore stub
 *
 * ObpSemaphoreType wrapping semaphore_t.
 * ============================================================================ */

#include "kernel/ob/ob_semaphore.h"
#include "kernel/ob/ob.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

/* --- Callbacks ----------------------------------------------------------- */

static void semaphore_on_delete(void *body)
{
    SEMAPHORE_OBJECT *so = (SEMAPHORE_OBJECT *)body;

    /* Wake any blocked waiters so they don't hang forever */
    while (so->semaphore.num_waiters > 0)
        sem_signal(&so->semaphore);
}

/* --- Type registration --------------------------------------------------- */

void ob_semaphore_type_init(void)
{
    extern const OBJECT_TYPE *ObpSemaphoreType;

    ObpSemaphoreType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Semaphore",
        .body_size = sizeof(SEMAPHORE_OBJECT),
        .on_close  = NULL,
        .on_delete = semaphore_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpSemaphoreType)
        klog(LOG_ERROR, "ob", "Failed to register ObpSemaphoreType");
}

/* --- NtCreateSemaphore stub ---------------------------------------------- */

HANDLE NtCreateSemaphore(HANDLE_TABLE *ht, const char *name,
                         int32_t initial_count, int32_t max_count)
{
    SEMAPHORE_OBJECT *so;
    HANDLE h;

    if (!ht)
        return INVALID_HANDLE_VALUE;

    /* If named, try to open existing */
    if (name) {
        void *existing = NULL;
        char path[128];
        extern int snprintf(char *buf, size_t size, const char *fmt, ...);
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

        if (ObLookupObjectByName(path, ObpSemaphoreType, 0, &existing) == 0
            && existing) {
            h = ObpAllocateHandle(ht, existing, 0, 0);
            ObDereferenceObject(existing);
            return h;
        }
    }

    /* Create new semaphore object */
    so = (SEMAPHORE_OBJECT *)ob_alloc_object(ObpSemaphoreType);
    if (!so)
        return INVALID_HANDLE_VALUE;

    sem_init(&so->semaphore, name ? name : "ob_sem", initial_count);
    so->max_count = max_count;

    /* Insert into \BaseNamedObjects if named */
    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            ObInsertObject(so, name, bno_dir);
            ObDereferenceObject(bno_dir);
        }
    }

    h = ObpAllocateHandle(ht, so, 0, 0);
    ObDereferenceObject(so);
    return h;
}
