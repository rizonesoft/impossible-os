/* ============================================================================
 * ob_process.c -- Process object type: callbacks, namespace integration
 *
 * ObpProcessType wrapping task_t.
 * Each process is inserted into \KernelObjects\Process<PID>.
 * ============================================================================ */

#include "kernel/ob/ob_process.h"
#include "kernel/ob/ob.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Callbacks ----------------------------------------------------------- */

/*
 * process_on_delete -- called when ref_count hits 0 after ObMakeTemporaryObject.
 * Task memory is already freed by task_cleanup(); just NULL the pointer.
 */
static void process_on_delete(void *body)
{
    PROCESS_OBJECT *po = (PROCESS_OBJECT *)body;
    po->task = NULL;
}

/* --- Type registration --------------------------------------------------- */

void ob_process_type_init(void)
{
    extern const OBJECT_TYPE *ObpProcessType;

    ObpProcessType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Process",
        .body_size = sizeof(PROCESS_OBJECT),
        .on_close  = NULL,
        .on_delete = process_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpProcessType)
        klog(LOG_ERROR, "ob", "Failed to register ObpProcessType");
}

/* --- Namespace integration ----------------------------------------------- */

void ob_process_create(struct task *t)
{
    PROCESS_OBJECT *po;
    OBJECT_HEADER *hdr;
    void *ko_dir = NULL;
    char name_buf[32];

    if (!t)
        return;

    po = (PROCESS_OBJECT *)ob_alloc_object(ObpProcessType);
    if (!po) {
        klog(LOG_ERROR, "ob", "Failed to alloc Process object for PID %u",
             (uint64_t)t->pid);
        return;
    }

    po->task = t;
    po->pid  = t->pid;

    /* Mark permanent while process is alive */
    hdr = OB_HEADER_FROM_BODY(po);
    hdr->flags |= OB_FLAG_PERMANENT;

    /* Insert into \KernelObjects\Process<PID> */
    snprintf(name_buf, sizeof(name_buf), "Process%u", t->pid);

    if (ObLookupObjectByName("\\KernelObjects", ObpDirectoryType, 0, &ko_dir) == 0
        && ko_dir) {
        ObInsertObject(po, name_buf, ko_dir);
        ObDereferenceObject(ko_dir);
    } else {
        klog(LOG_WARN, "ob", "\\KernelObjects not found -- Process%u not inserted",
             (uint64_t)t->pid);
    }

    /* Drop the creation ref -- namespace holds its own */
    ObDereferenceObject(po);
}

void ob_process_mark_dead(uint32_t pid)
{
    void *body = NULL;
    char path[64];

    snprintf(path, sizeof(path), "\\KernelObjects\\Process%u", pid);

    if (ObLookupObjectByName(path, ObpProcessType, 0, &body) == 0 && body) {
        ObMakeTemporaryObject(body);
        ObDereferenceObject(body);  /* release lookup ref */
    }
}
