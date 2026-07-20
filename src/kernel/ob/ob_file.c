/* ============================================================================
 * ob_file.c -- File object type: callbacks, handle-based open/read
 *
 * ObpFileType with VFS node wrapper and pipe support.
 * ============================================================================ */

#include "kernel/ob/ob_file.h"
#include "kernel/ob/ob.h"
#include "kernel/fs/vfs.h"
#include "kernel/ipc/pipe.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"   /* kmalloc / kfree for FILE_OBJECT.path */

/* --- Callbacks ----------------------------------------------------------- */

/*
 * file_on_close -- called when the last handle to the file is closed.
 * Closes the underlying VFS node or pipe end.
 */
static void file_on_close(void *body, uint32_t handle_count)
{
    FILE_OBJECT *fo = (FILE_OBJECT *)body;

    (void)handle_count;

    if (fo->pipe_id >= 0) {
        pipe_close(fo->pipe_id, fo->pipe_end);
        fo->pipe_id = -1;
    } else if (fo->vfs_node) {
        vfs_close(fo->vfs_node);
        fo->vfs_node = NULL;
    }
}

/*
 * file_on_delete -- safety net when ref_count hits 0.
 * If the resource was never closed via on_close, close it here.
 */
static void file_on_delete(void *body)
{
    FILE_OBJECT *fo = (FILE_OBJECT *)body;

    if (fo->pipe_id >= 0) {
        pipe_close(fo->pipe_id, fo->pipe_end);
        fo->pipe_id = -1;
    } else if (fo->vfs_node) {
        vfs_close(fo->vfs_node);
        fo->vfs_node = NULL;
    }
    if (fo->path) {
        kfree(fo->path);
        fo->path = NULL;
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
        .on_parse  = NULL,   /* full on_parse dispatch lands with NT path parsing */
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
    char kpath[VFS_MAX_PATH];
    uint32_t plen = 0;

    if (!path)
        return INVALID_HANDLE_VALUE;

    /* Snapshot the path ONCE into a bounded kernel buffer and use that same
     * snapshot for BOTH the VFS lookup and fo->path. A legacy SYS_OPENFILE
     * caller passes a raw user pointer; reading it twice (vfs_open, then the
     * fo->path copy) let a sibling thread mutate the buffer between reads so
     * fo->path could name a different (allowed) file than fo->vfs_node actually
     * opened -- an unveil provenance forge. The bound also caps the allocation
     * (an unbounded user path could otherwise exhaust the heap). Overlong ->
     * reject (VFS rejects overlong components anyway). */
    while (path[plen]) {
        if (plen >= VFS_MAX_PATH - 1)
            return INVALID_HANDLE_VALUE;
        kpath[plen] = path[plen];
        plen++;
    }
    kpath[plen] = '\0';

    node = vfs_open(kpath, access);
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
    fo->pipe_id  = -1;
    fo->pipe_end = 0;
    fo->path_stale = 0;
    /* Retain the canonical open path (exact-length allocation, not an inline
     * VFS_MAX_PATH array -- that would charge every file object + pipe endpoint
     * 512 bytes on the shared kernel heap) for later unveil re-checks. NULL on
     * alloc failure -> setinfo unveil re-checks fail closed for a restricted
     * task, which is safe. The bounded snapshot above is the SAME bytes vfs_open
     * saw. */
    {
        fo->path = (char *)kmalloc(plen + 1);
        if (fo->path) {
            uint32_t i = 0;
            for (; i < plen; i++)
                fo->path[i] = kpath[i];
            fo->path[i] = '\0';
        }
    }

    h = ObpAllocateHandle(&task_current()->handle_table, fo, access, 0);
    /* Drop the creation ref -- the handle holds its own */
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

    /* Pipe read path -- only the read-end handle is allowed to read.
     * Reject writes on a write-end or mis-tagged handle with -1 rather
     * than blocking on an empty ring / corrupting ring state. Codex
     * adversarial 2026-04-21 H1. */
    if (fo->pipe_id >= 0) {
        int64_t pbytes;
        if (fo->pipe_end != PIPE_READ)
            return -1;
        pbytes = (int64_t)pipe_read(fo->pipe_id, buf, size);
        /* A pipe read is a read: it belongs in the SAME counters as a file
         * read, or a pipe-heavy process reports zero I/O and every per-job
         * aggregate built on these counters is wrong for that workload. */
        if (pbytes > 0)
            task_acct_note_read_io(task_current(), (uint64_t)pbytes);
        return pbytes;
    }

    /* VFS read path -- require VFS_O_READ in the handle's access mask.
     * A handle opened WRITE-only has no business reading the file. */
    if (!fo->vfs_node)
        return -1;
    if (!(fo->access & VFS_O_READ))
        return -1;

    bytes = vfs_read(fo->vfs_node, (uint32_t)fo->offset, size, (uint8_t *)buf);
    if (bytes > 0) {
        fo->offset += (uint64_t)bytes;
        task_acct_note_read_io(task_current(), (uint64_t)bytes);
    }

    return (int64_t)bytes;
}

int64_t ob_file_write(HANDLE_TABLE *ht, HANDLE h, const void *buf, uint32_t size)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    FILE_OBJECT *fo;
    int64_t bytes;

    if (!ht || !buf || size == 0)
        return -1;

    entry = ObpLookupHandle(ht, h);
    if (!entry)
        return -1;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpFileType)
        return -1;

    fo = (FILE_OBJECT *)entry->object;

    /* Pipe write path -- only the write-end handle is allowed to write.
     * Writing to a read-end handle would corrupt the ring's readable
     * semaphore count and let a malicious caller forge data into a
     * pipe it should only be able to drain. */
    if (fo->pipe_id >= 0) {
        int64_t pbytes;
        if (fo->pipe_end != PIPE_WRITE)
            return -1;
        pbytes = (int64_t)pipe_write(fo->pipe_id, buf, size);
        if (pbytes > 0)
            task_acct_note_write_io(task_current(), (uint64_t)pbytes);
        return pbytes;
    }

    /* VFS write path -- require VFS_O_WRITE in the handle's access
     * mask. Without this gate, sys_writehandle could mutate a file
     * opened with VFS_O_READ only, turning a read-only handle into a
     * write handle. */
    if (!fo->vfs_node)
        return -1;
    if (!(fo->access & VFS_O_WRITE))
        return -1;

    bytes = (int64_t)vfs_write(fo->vfs_node, (uint32_t)fo->offset, size,
                               (const uint8_t *)buf);
    /* Advance the file offset on a positive write so repeated
     * sys_writehandle calls stream instead of overwriting byte 0.
     * Mirrors ob_file_read's post-read offset bump and the NT-path
     * NtWriteFile semantics. */
    if (bytes > 0) {
        fo->offset += (uint64_t)bytes;
        task_acct_note_write_io(task_current(), (uint64_t)bytes);
    }

    return bytes;
}

/* --- Pipe handle creation ------------------------------------------------ */

int ob_create_pipe_handles(HANDLE_TABLE *ht, HANDLE handles[2])
{
    int pipe_fds[2];
    FILE_OBJECT *fo_read, *fo_write;

    if (!ht || !handles)
        return -1;

    if (pipe_create(pipe_fds) < 0)
        return -1;

    /* Create read-end file object */
    fo_read = (FILE_OBJECT *)ob_alloc_object(ObpFileType);
    if (!fo_read) {
        pipe_close(pipe_fds[0], PIPE_READ);
        pipe_close(pipe_fds[0], PIPE_WRITE);
        return -1;
    }
    fo_read->vfs_node = NULL;
    fo_read->access   = 0;
    fo_read->offset   = 0;
    fo_read->pipe_id  = pipe_fds[0];
    fo_read->pipe_end = PIPE_READ;
    fo_read->path     = NULL;   /* pipes carry no path (ob_alloc does not zero) */
    fo_read->path_stale = 0;

    /* Create write-end file object */
    fo_write = (FILE_OBJECT *)ob_alloc_object(ObpFileType);
    if (!fo_write) {
        ObDereferenceObject(fo_read);  /* triggers pipe_close(read) */
        pipe_close(pipe_fds[1], PIPE_WRITE);
        return -1;
    }
    fo_write->vfs_node = NULL;
    fo_write->access   = 0;
    fo_write->offset   = 0;
    fo_write->pipe_id  = pipe_fds[1];
    fo_write->pipe_end = PIPE_WRITE;
    fo_write->path     = NULL;   /* pipes carry no path (ob_alloc does not zero) */
    fo_write->path_stale = 0;

    /* Allocate handles */
    handles[0] = ObpAllocateHandle(ht, fo_read, 0, 0);
    handles[1] = ObpAllocateHandle(ht, fo_write, 0, 0);

    /* Drop creation refs */
    ObDereferenceObject(fo_read);
    ObDereferenceObject(fo_write);

    if (handles[0] == INVALID_HANDLE_VALUE || handles[1] == INVALID_HANDLE_VALUE) {
        if (handles[0] != INVALID_HANDLE_VALUE)
            ObpFreeHandle(ht, handles[0]);
        if (handles[1] != INVALID_HANDLE_VALUE)
            ObpFreeHandle(ht, handles[1]);
        return -1;
    }

    return 0;
}
