/* ============================================================================
 * ob_file.c — File object type: callbacks, handle-based open/read
 *
 * Implements TODO-03 §5: ObpFileType with VFS node wrapper.
 * ============================================================================ */

#include "kernel/ob/ob_file.h"
#include "kernel/ob/ob.h"
#include "kernel/fs/vfs.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

/* --- Callbacks ----------------------------------------------------------- */

/*
 * file_on_close — called when the last handle to the file is closed.
 * Closes the underlying VFS node.
 */
static void file_on_close(void *body, uint32_t handle_count)
{
    FILE_OBJECT *fo = (FILE_OBJECT *)body;

    (void)handle_count;

    if (fo->vfs_node) {
        vfs_close(fo->vfs_node);
        fo->vfs_node = NULL;
    }
}

/*
 * file_on_delete — safety net when ref_count hits 0.
 * If the VFS node was never closed via on_close (kernel-internal ref
 * with no handle), close it here.
 */
static void file_on_delete(void *body)
{
    FILE_OBJECT *fo = (FILE_OBJECT *)body;

    if (fo->vfs_node) {
        vfs_close(fo->vfs_node);
        fo->vfs_node = NULL;
    }
}

/* --- Type registration --------------------------------------------------- */

void ob_file_type_init(void)
{
    extern const OBJECT_TYPE *ObpFileType;

    ObpFileType = ob_create_type(&(OBJECT_TYPE){
        .name      = "File",
        .body_size = sizeof(FILE_OBJECT),
        .on_close  = file_on_close,
        .on_delete = file_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,   /* full on_parse dispatch is TODO-05 §6 */
    });

    if (!ObpFileType)
        klog(LOG_ERROR, "ob", "Failed to register ObpFileType");
}

/* --- Handle-based file operations ---------------------------------------- */

HANDLE ob_create_file_handle(const char *path, uint32_t access)
{
    struct vfs_node *node;
    FILE_OBJECT *fo;
    HANDLE h;

    if (!path)
        return INVALID_HANDLE_VALUE;

    node = vfs_open(path, access);
    if (!node)
        return INVALID_HANDLE_VALUE;

    fo = (FILE_OBJECT *)ob_alloc_object(ObpFileType);
    if (!fo) {
        vfs_close(node);
        return INVALID_HANDLE_VALUE;
    }

    fo->vfs_node = node;
    fo->access   = access;
    fo->offset   = 0;

    h = ObpAllocateHandle(&task_current()->handle_table, fo, access, 0);
    /* Drop the creation ref — the handle holds its own */
    ObDereferenceObject(fo);

    if (h == INVALID_HANDLE_VALUE) {
        /* Handle alloc failed; deref already triggered on_delete -> vfs_close */
        return INVALID_HANDLE_VALUE;
    }

    return h;
}

int64_t ob_file_read(HANDLE_TABLE *ht, HANDLE h, void *buf, uint32_t size)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    FILE_OBJECT *fo;
    int bytes;

    if (!ht || !buf || size == 0)
        return -1;

    entry = ObpLookupHandle(ht, h);
    if (!entry)
        return -1;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpFileType)
        return -1;

    fo = (FILE_OBJECT *)entry->object;
    if (!fo->vfs_node)
        return -1;

    bytes = vfs_read(fo->vfs_node, (uint32_t)fo->offset, size, (uint8_t *)buf);
    if (bytes > 0)
        fo->offset += (uint64_t)bytes;

    return (int64_t)bytes;
}
