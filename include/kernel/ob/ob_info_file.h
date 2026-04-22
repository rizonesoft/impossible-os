/* ============================================================================
 * ob_info_file.h -- Read-only kernel-metadata pseudo-files in the Ob namespace
 *
 * An info file is a named Ob object whose Read handler is a kernel-supplied
 * callback that writes a caller-defined byte layout on demand. It lets the
 * kernel expose live subsystem state (frame stats, scheduler counters,
 * etc.) through the object-manager namespace without needing per-surface
 * syscalls, and without any VFS backing.
 *
 * Registered files live under `\ObjectManager\<name>`. The kernel-side
 * open + read API is in this header; Win32 CreateFile / NtReadFile
 * routing to the Ob namespace is planned as a follow-on and would plug
 * into `ob_info_file_read` via a handle-table dispatch path.
 *
 * First consumer: `\ObjectManager\FrameStats`. The `wm_get_frame_stats()`
 * snapshot is the read payload; schema is pinned in `include/kernel/etw.h`
 * alongside `ETW_EVT_WM_FRAME_PRESENTED`.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/ob/handle_table.h"

/*
 * Read callback. Fills `buf` (up to `size` bytes starting from `offset` in
 * the pseudo-file's virtual byte stream) and returns the number of bytes
 * written. A return of 0 means EOF -- the caller is reading past the end
 * of the file's fixed size. Negative return is an error.
 *
 * Callbacks must be side-effect free (pure snapshot reads) and fast
 * enough to run from any context; they run under the caller's stack
 * without locking. If the underlying counter needs SMP-safe access,
 * the callback is responsible for its own seqlock / spinlock.
 */
typedef int32_t (*ob_info_read_fn)(uint8_t *buf, uint32_t size, uint32_t offset);

/*
 * OB_INFO_FILE body. `size` is the fixed byte-size of the virtual file
 * (used by the read path to return EOF when offset >= size), which also
 * lets future stat() style APIs report the full size without calling
 * the read callback.
 */
typedef struct ob_info_file {
    ob_info_read_fn read_fn;
    uint32_t        size;
    uint32_t        _pad;
} OB_INFO_FILE;

/*
 * Type singleton registered in ob_init. Named "ObjectInfoFile".
 */
extern const struct object_type *ObpInfoFileType;

/*
 * Register the info-file type. Called from ob_init() during boot phase 2
 * AFTER ob_ns_init has created the root namespace, so the registration
 * function can immediately insert named entries under \ObjectManager\.
 */
void ob_info_file_type_init(void);

/*
 * Register a named info file under `\ObjectManager\<name>`. Creates the
 * `\ObjectManager` directory on first call. `read_fn` must be non-NULL;
 * `size` declares the file's virtual byte size. Returns 0 on success, -1
 * on any failure (type not registered yet, namespace full, duplicate
 * name, zero size, null callback).
 *
 * The registered object holds OB_FLAG_PERMANENT so it survives all
 * handle closes; uninstall is not supported today.
 */
int ob_info_file_register(const char *name, ob_info_read_fn read_fn,
                          uint32_t size);

/*
 * Open an info file by name under `\ObjectManager\`. Allocates a handle
 * in `ht` that can be passed to `ob_info_file_read`. Returns
 * INVALID_HANDLE_VALUE on failure (unknown name, handle-table full).
 */
HANDLE ob_info_file_open_handle(HANDLE_TABLE *ht, const char *name);

/*
 * Read `size` bytes starting at `offset` into `buf`. Returns the number
 * of bytes written. A return of 0 means EOF (offset >= file size); any
 * negative return is an error (bad handle, type mismatch, null buf). The
 * offset is NOT persisted in the handle today -- callers that want
 * stream-style reads track the position themselves. This keeps the
 * handle state read-only across concurrent readers.
 */
int32_t ob_info_file_read(HANDLE_TABLE *ht, HANDLE h, void *buf,
                          uint32_t size, uint32_t offset);
