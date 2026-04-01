/* ============================================================================
 * ob_thread.c — Thread object type: callbacks, namespace integration
 *
 * Implements TODO-03 §5: ObpThreadType wrapping thread sub-struct.
 * Each thread is inserted into \KernelObjects\Thread<PID>.<TID>.
 * ============================================================================ */

#include "kernel/ob/ob_thread.h"
#include "kernel/ob/ob.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Callbacks ----------------------------------------------------------- */

/*
 * thread_on_delete — called when ref_count hits 0 after ObMakeTemporaryObject.
 * Thread memory lives in the task's threads[] array; just NULL the pointer.
 */
static void thread_on_delete(void *body)
{
    THREAD_OBJECT *to = (THREAD_OBJECT *)body;
    to->thread = NULL;
}

/* --- Type registration --------------------------------------------------- */

void ob_thread_type_init(void)
{
    extern const OBJECT_TYPE *ObpThreadType;

    ObpThreadType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Thread",
        .body_size = sizeof(THREAD_OBJECT),
        .on_close  = NULL,
        .on_delete = thread_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpThreadType)
        klog(LOG_ERROR, "ob", "Failed to register ObpThreadType");
}

/* --- Namespace integration ----------------------------------------------- */

void ob_thread_create(struct thread *thr, uint32_t task_pid)
{
    THREAD_OBJECT *to;
    OBJECT_HEADER *hdr;
    void *ko_dir = NULL;
    char name_buf[32];

    if (!thr)
        return;

    to = (THREAD_OBJECT *)ob_alloc_object(ObpThreadType);
    if (!to) {
        klog(LOG_ERROR, "ob", "Failed to alloc Thread object for PID %u TID %u",
             (uint64_t)task_pid, (uint64_t)thr->id);
        return;
    }

    to->thread    = thr;
    to->task_pid  = task_pid;
    to->thread_id = thr->id;

    /* Mark permanent while thread is alive */
    hdr = OB_HEADER_FROM_BODY(to);
    hdr->flags |= OB_FLAG_PERMANENT;

    /* Insert into \KernelObjects\Thread<PID>.<TID> */
    snprintf(name_buf, sizeof(name_buf), "Thread%u.%u", task_pid, thr->id);

    if (ObLookupObjectByName("\\KernelObjects", ObpDirectoryType, 0, &ko_dir) == 0
        && ko_dir) {
        ObInsertObject(to, name_buf, ko_dir);
        ObDereferenceObject(ko_dir);
    } else {
        klog(LOG_WARN, "ob", "\\KernelObjects not found — Thread%u.%u not inserted",
             (uint64_t)task_pid, (uint64_t)thr->id);
    }

    /* Drop the creation ref — namespace holds its own */
    ObDereferenceObject(to);
}

void ob_thread_mark_dead(uint32_t task_pid, uint32_t tid)
{
    void *body = NULL;
    char path[64];

    snprintf(path, sizeof(path), "\\KernelObjects\\Thread%u.%u", task_pid, tid);

    if (ObLookupObjectByName(path, ObpThreadType, 0, &body) == 0 && body) {
        ObMakeTemporaryObject(body);
        ObDereferenceObject(body);  /* release lookup ref */
    }
}
