/* ============================================================================
 * ob_file.h — File object type for the Object Manager
 *
 * FILE_OBJECT wraps a VFS node pointer with open-state fields (offset,
 * access flags).  Handle-based file I/O routes through these objects.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/ob/handle_table.h"

/* Forward-declare VFS node (avoids pulling in the full VFS header) */
struct vfs_node;

/* --- FILE_OBJECT body ---------------------------------------------------- */

typedef struct file_object {
    struct vfs_node *vfs_node;   /* VFS node pointer (owned by VFS); NULL for pipes */
    uint32_t         access;     /* granted access flags */
    uint64_t         offset;     /* current file position */
    int32_t          pipe_id;    /* pipe index (-1 if not a pipe) */
    int32_t          pipe_end;   /* PIPE_READ (0) or PIPE_WRITE (1) */
} FILE_OBJECT;

/* --- API ----------------------------------------------------------------- */

/*
 * ob_file_type_init — register ObpFileType with proper body size and callbacks.
 * Called from ob_init() during boot_phase2.
 */
void ob_file_type_init(void);

/*
 * ob_create_file_handle — open a file by path and return a process HANDLE.
 *
 * Calls vfs_open(), wraps the result in a FILE_OBJECT, and allocates a
 * handle in the current task's handle table.
 * Returns INVALID_HANDLE_VALUE on failure.
 */
HANDLE ob_create_file_handle(const char *path, uint32_t access);

/*
 * ob_file_read — read from a file handle, advancing the file offset.
 *
 * Returns bytes read, or -1 on error (bad handle, type mismatch, etc.).
 */
int64_t ob_file_read(HANDLE_TABLE *ht, HANDLE h, void *buf, uint32_t size);

/*
 * ob_file_write — write to a file handle (pipes only for now).
 *
 * Returns bytes written, or -1 on error.
 */
int64_t ob_file_write(HANDLE_TABLE *ht, HANDLE h, const void *buf, uint32_t size);

/*
 * ob_create_pipe_handles — create a pipe and return two HANDLEs.
 *
 * handles[0] = read end, handles[1] = write end.
 * Returns 0 on success, -1 on failure.
 */
int ob_create_pipe_handles(HANDLE_TABLE *ht, HANDLE handles[2]);
