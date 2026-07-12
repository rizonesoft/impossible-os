/* ============================================================================
 * nt_file.c -- NtXxx file metadata, device control, and I/O completion
 *
 * Implements SSDT handlers for NtQueryInformationFile, NtSetInformationFile,
 * NtDeleteFile, NtDeviceIoControlFile, NtFsControlFile, NtFlushBuffersFile,
 * NtLockFile, NtUnlockFile, NtNotifyChangeDirectoryFile,
 * NtQueryVolumeInformationFile, NtQueryAttributesFile, NtCancelIoFile,
 * NtCancelIoFileEx, NtCreateIoCompletion, NtSetIoCompletion,
 * NtRemoveIoCompletion, NtCreateMailslotFile, NtReadFileScatter,
 * NtWriteFileGather.
 *
 * XREF: 02-kernel-core/TODO-12-native-api-ssdt.md S13
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/nt_file.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_file.h"
#include "kernel/ob/handle_table.h"
#include "kernel/fs/vfs.h"
#include "kernel/sched/task.h"
#include "kernel/nt/pledge.h"
#include "kernel/nt/zw.h"           /* ssdt_previous_mode via pledge_user_mode */
#include "kernel/mm/heap.h"
#include "kernel/timer.h"

/* External OB types */
#include "kernel/ob/ob.h"

/* ---- Helpers ------------------------------------------------------------ */

/* Look up a file handle -> FILE_OBJECT, validating type. */
static FILE_OBJECT *file_from_handle(HANDLE h)
{
    HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
        &task_current()->handle_table, h);
    if (!entry || !entry->object)
        return (FILE_OBJECT *)0;

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpFileType)
        return (FILE_OBJECT *)0;

    return (FILE_OBJECT *)entry->object;
}

/* Convert VFS seconds-since-boot to FILETIME (100-ns ticks since 1601).
 * Adds boot epoch offset: boot_epoch_filetime() returns the FILETIME of
 * system boot. If unavailable, returns a fixed 2026 epoch. */
static FILETIME vfs_seconds_to_filetime(uint32_t secs)
{
    /* Approximate: use 2026-01-01 00:00:00 UTC as epoch fallback */
    FILETIME base = 133800288000000000ULL;  /* 2026-01-01 in FILETIME */
    return base + (uint64_t)secs * FILETIME_TICKS_PER_SECOND;
}

/* Copy ASCII name to UTF-16LE buffer. Returns bytes written (not chars). */
static uint32_t ascii_to_utf16(const char *src, uint16_t *dst, uint32_t max_chars)
{
    uint32_t i;
    for (i = 0; i < max_chars && src[i]; i++)
        dst[i] = (uint16_t)(uint8_t)src[i];
    return i * 2;  /* byte count */
}

/* Narrow a counted UTF-16 name to an ASCII C string in out[cap], validating the
 * full counted-string contract and failing closed. Callers MUST pass values
 * ALREADY snapshotted from caller-owned memory into locals (len_bytes/max_bytes/
 * buf read once), so the validation is bound to the data used -- no TOCTOU
 * re-read of a mutable UNICODE_STRING between the check and the copy. Rejects:
 * odd len (not whole WCHARs), len past the declared capacity, over-long (> cap-1
 * chars), embedded NUL (would truncate the counted name into a SHORTER aliased C
 * string), and any non-ASCII code unit (a lossy fold would alias to an unrelated
 * path). Real UTF-16/code-page conversion at this boundary is deferred; this is
 * the fail-closed ASCII narrowing shared by the file-name syscalls. */
static NTSTATUS nt_wname_to_ascii(const uint16_t *buf, uint32_t len_bytes,
                                  uint32_t max_bytes, char *out, uint32_t cap,
                                  uint32_t *out_chars)
{
    uint32_t chars, i;
    if (!buf || cap == 0)
        return STATUS_INVALID_PARAMETER;
    if ((len_bytes & 1u) || len_bytes > max_bytes)
        return STATUS_OBJECT_NAME_INVALID;
    chars = len_bytes / 2u;
    if (chars + 1u > cap)
        return STATUS_NAME_TOO_LONG;
    for (i = 0; i < chars; i++) {
        uint16_t wc = buf[i];
        if (wc == 0u || wc > 0x7Fu)
            return STATUS_OBJECT_NAME_INVALID;
        out[i] = (char)wc;
    }
    out[chars] = '\0';
    if (out_chars)
        *out_chars = chars;
    return STATUS_SUCCESS;
}

/* ---- NtQueryInformationFile --------------------------------------------- */

/* Fill st from the file's VFS node (stat op, else raw vfs_node fields). Called
 * only by the metadata info classes and only AFTER their buffer-size check, so
 * a too-small or cheap query never pays the (possibly allocating, disk-reading)
 * stat cost. */
static void nt_file_fill_stat(FILE_OBJECT *fo, struct vfs_stat *st)
{
    int vfs_ret = -1;
    if (fo->vfs_node->ops && fo->vfs_node->ops->stat)
        vfs_ret = fo->vfs_node->ops->stat(fo->vfs_node, st);
    if (vfs_ret != 0) {
        st->size = fo->vfs_node->size;
        st->type = fo->vfs_node->type;
        st->ctime = 0;
        st->mtime = 0;
        st->atime = 0;
        st->blocks = 0;
    }
}

static NTSTATUS NtQueryInformationFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fh = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    void *info = (void *)a3;
    uint32_t length = (uint32_t)a4;
    uint32_t info_class = (uint32_t)a5;
    FILE_OBJECT *fo;
    struct vfs_stat st;

    (void)a6;

    fo = file_from_handle(fh);
    if (!fo || !fo->vfs_node)
        return STATUS_INVALID_HANDLE;

    if (!info)
        return STATUS_INVALID_PARAMETER;

    /* stat is fetched lazily inside the metadata cases AFTER their buffer-size
     * check (nt_file_fill_stat) -- a cheap class, an invalid class, or a
     * too-small buffer must not pay the stat cost (on NTFS a stat allocates a
     * PMM frame and reads an MFT record, so an eager stat would let an
     * error/cheap-query loop force allocation + disk I/O: local DoS). */
    switch (info_class) {
    case FileBasicInformation: {
        FILE_BASIC_INFORMATION *bi;
        if (length < sizeof(FILE_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        nt_file_fill_stat(fo, &st);
        bi = (FILE_BASIC_INFORMATION *)info;
        bi->CreationTime = vfs_seconds_to_filetime(st.ctime);
        bi->LastAccessTime = vfs_seconds_to_filetime(st.atime);
        bi->LastWriteTime = vfs_seconds_to_filetime(st.mtime);
        bi->ChangeTime = vfs_seconds_to_filetime(st.mtime);
        bi->FileAttributes = (st.type == VFS_DIRECTORY)
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        bi->_pad = 0;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = sizeof(FILE_BASIC_INFORMATION);
        }
        return STATUS_SUCCESS;
    }
    case FileStandardInformation: {
        FILE_STANDARD_INFORMATION *si;
        if (length < sizeof(FILE_STANDARD_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        nt_file_fill_stat(fo, &st);
        si = (FILE_STANDARD_INFORMATION *)info;
        si->AllocationSize = (st.size + 4095) & ~(uint64_t)4095;
        si->EndOfFile = st.size;
        si->NumberOfLinks = 1;
        si->DeletePending = fo->vfs_node->delete_on_close;
        si->Directory = (st.type == VFS_DIRECTORY) ? 1 : 0;
        si->_pad = 0;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = sizeof(FILE_STANDARD_INFORMATION);
        }
        return STATUS_SUCCESS;
    }
    case FileNameInformation: {
        FILE_NAME_INFORMATION *ni;
        uint32_t avail_chars, full_chars, copy_chars, full_bytes, copied;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        ni = (FILE_NAME_INFORMATION *)info;
        /* Full required name length. Bound the scan to VFS_MAX_NAME so a
         * corrupted/unterminated node name cannot read past the fixed-size
         * name storage. */
        for (full_chars = 0;
             full_chars < VFS_MAX_NAME && fo->vfs_node->name[full_chars];
             full_chars++)
            ;
        full_bytes = full_chars * 2;
        /* Copy count is bounded by BOTH the caller buffer AND the scanned
         * name length -- the buffer cap alone would let ascii_to_utf16 read
         * past name[] on an unterminated node. */
        avail_chars = (length - sizeof(uint32_t)) / sizeof(uint16_t);
        if (avail_chars > 260) avail_chars = 260;
        copy_chars = (avail_chars < full_chars) ? avail_chars : full_chars;
        copied = ascii_to_utf16(fo->vfs_node->name, ni->FileName, copy_chars);
        /* Win11 ZwQueryInformationFile contract: FileNameLength is the FULL
         * required byte length even when the buffer only held part of it, so
         * a grow-and-retry caller learns the real size. On truncation report
         * STATUS_BUFFER_OVERFLOW, not SUCCESS. */
        ni->FileNameLength = full_bytes;
        if (iosb) {
            iosb->Status = (copied < full_bytes)
                ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
            iosb->Information = sizeof(uint32_t) + copied;
        }
        return (copied < full_bytes)
            ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    case FilePositionInformation: {
        FILE_POSITION_INFORMATION *pi;
        if (length < sizeof(FILE_POSITION_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        pi = (FILE_POSITION_INFORMATION *)info;
        pi->CurrentByteOffset = fo->offset;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = sizeof(FILE_POSITION_INFORMATION);
        }
        return STATUS_SUCCESS;
    }
    case FileNetworkOpenInformation: {
        FILE_NETWORK_OPEN_INFORMATION *noi;
        if (length < sizeof(FILE_NETWORK_OPEN_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        nt_file_fill_stat(fo, &st);
        noi = (FILE_NETWORK_OPEN_INFORMATION *)info;
        noi->CreationTime = vfs_seconds_to_filetime(st.ctime);
        noi->LastAccessTime = vfs_seconds_to_filetime(st.atime);
        noi->LastWriteTime = vfs_seconds_to_filetime(st.mtime);
        noi->ChangeTime = vfs_seconds_to_filetime(st.mtime);
        noi->AllocationSize = (st.size + 4095) & ~(uint64_t)4095;
        noi->EndOfFile = st.size;
        noi->FileAttributes = (st.type == VFS_DIRECTORY)
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        noi->_pad = 0;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);
        }
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtSetInformationFile ----------------------------------------------- */

static NTSTATUS NtSetInformationFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fh = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    void *info = (void *)a3;
    uint32_t length = (uint32_t)a4;
    uint32_t info_class = (uint32_t)a5;
    FILE_OBJECT *fo;
    struct vfs_node *node;

    (void)a6;

    fo = file_from_handle(fh);
    if (!fo || !fo->vfs_node)
        return STATUS_INVALID_HANDLE;

    if (!info)
        return STATUS_INVALID_PARAMETER;

    node = fo->vfs_node;

    /* Handle access boundary: a destructive info class must not be reachable
     * through a read-only (or access-reduced) handle regardless of process
     * pledge/unveil -- otherwise an inherited/duplicated read handle could
     * truncate, delete, or rename. Require the handle to hold write access.
     * (The finer NT DELETE-vs-WRITE ACCESS_MASK distinction needs granted-mask
     * tracking on FILE_OBJECT -- filed in TODO-12 s13.) */
    switch (info_class) {
    case FileBasicInformation:
    case FileEndOfFileInformation:
    case FileAllocationInformation:
    case FileDispositionInformation:
    case FileRenameInformation:
        if (!(fo->access & VFS_O_WRITE))
            return STATUS_ACCESS_DENIED;
        break;
    default:
        break;
    }

    /* Argument-dependent pledge check: rename/dispose require cpath,
     * truncate/allocate/set-attributes require wpath (the coarse dispatcher
     * classifies NtSetInformationFile as core because the category depends on
     * info_class). A violation returns and the dispatcher terminates. */
    if (pledge_user_mode()) {
        NTSTATUS pr = pledge_check_setinfo(task_current(), info_class);
        if (pr != STATUS_SUCCESS)
            return pr;
    }

    /* unveil: mutating an ALREADY-OPEN file still requires the appropriate
     * write/create permission on its path -- an r-only unveiled handle must not
     * truncate, delete, rename, or alter metadata. Reconstruct the source path
     * and check it fail-closed (rename also checks the destination in its case). */
    if (pledge_user_mode() &&
        __atomic_load_n(&task_current()->unveil_active, __ATOMIC_ACQUIRE)) {
        uint8_t need_u = 0;
        switch (info_class) {
        case FileBasicInformation:
        case FileEndOfFileInformation:
        case FileAllocationInformation:
            need_u = UNVEIL_W; break;
        case FileDispositionInformation:
        case FileRenameInformation:   /* removes the source name -> cpath on source */
            need_u = UNVEIL_C; break;
        default: break;
        }
        if (need_u) {
            /* fo->path is the authoritative canonical open path (node parent
             * chains are unreliable on IXFS/FAT32). Fail closed if absent (NULL
             * on a pipe / path-alloc failure) OR stale (a prior rename via this
             * handle -- the path no longer names the file, so it cannot be
             * authorized; the process must reopen at the new name to mutate). */
            if (fo->path_stale || !fo->path || fo->path[0] == '\0' ||
                unveil_check(task_current(), fo->path, need_u) != STATUS_SUCCESS)
                return STATUS_ACCESS_DENIED;
        }
    }

    switch (info_class) {
    case FileBasicInformation: {
        FILE_BASIC_INFORMATION *bi;
        if (length < sizeof(FILE_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        bi = (FILE_BASIC_INFORMATION *)info;
        if (node->ops && node->ops->set_times) {
            filetime_t ct, mt, at;
            ct.seconds = (uint32_t)filetime_to_unix_seconds(bi->CreationTime);
            mt.seconds = (uint32_t)filetime_to_unix_seconds(bi->LastWriteTime);
            at.seconds = (uint32_t)filetime_to_unix_seconds(bi->LastAccessTime);
            node->ops->set_times(node, &ct, &mt, &at);
        }
        if (bi->FileAttributes && node->ops && node->ops->set_attr)
            node->ops->set_attr(node, bi->FileAttributes);
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }
    case FileDispositionInformation: {
        FILE_DISPOSITION_INFORMATION *di;
        if (length < sizeof(FILE_DISPOSITION_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        di = (FILE_DISPOSITION_INFORMATION *)info;
        node->delete_on_close = di->DeleteFile ? 1 : 0;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }
    case FilePositionInformation: {
        FILE_POSITION_INFORMATION *pi;
        if (length < sizeof(FILE_POSITION_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        pi = (FILE_POSITION_INFORMATION *)info;
        fo->offset = pi->CurrentByteOffset;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }
    case FileEndOfFileInformation: {
        FILE_END_OF_FILE_INFORMATION *ei;
        if (length < sizeof(FILE_END_OF_FILE_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        ei = (FILE_END_OF_FILE_INFORMATION *)info;
        if (node->ops && node->ops->truncate) {
            if (node->ops->truncate(node, ei->EndOfFile) != 0)
                return STATUS_UNSUCCESSFUL;
        }
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }
    case FileRenameInformation: {
        FILE_RENAME_INFORMATION *ri;
        char ascii_name[260];
        uint32_t fixed_size, fnl;
        NTSTATUS rns;
        /* Fixed header: ReplaceIfExists(1) + pad(7) + RootDirectory(8) + FileNameLength(4) = 20 */
        fixed_size = 20;
        if (length < fixed_size)
            return STATUS_BUFFER_TOO_SMALL;
        ri = (FILE_RENAME_INFORMATION *)info;
        /* Snapshot the caller-controlled name length ONCE (the info buffer is
         * caller memory), reject empty, and bound it against the info buffer
         * with subtraction (fixed_size + fnl can wrap; length >= fixed_size was
         * checked above). */
        fnl = ri->FileNameLength;
        if (fnl == 0)
            return STATUS_OBJECT_NAME_INVALID;
        if (fnl > length - fixed_size)
            return STATUS_BUFFER_TOO_SMALL;
        /* Fail-closed narrow (odd length, embedded NUL, non-ASCII, over-long all
         * rejected before any rename) -- same helper as NtDeleteFile so a rename
         * target cannot lossily alias to an unrelated path. */
        rns = nt_wname_to_ascii(ri->FileName, fnl, fnl, ascii_name,
                                sizeof(ascii_name), (uint32_t *)0);
        if (rns != STATUS_SUCCESS)
            return rns;
        /* Unveil the DESTINATION, not just the (already-open) source: a rename
         * to a sibling name would otherwise escape a file-granularity unveil.
         * Rename is same-parent-dir, so dest = dirname(source) + '\' + newname. */
        if (pledge_user_mode() &&
            __atomic_load_n(&task_current()->unveil_active, __ATOMIC_ACQUIRE)) {
            const char *src_path = fo->path;   /* authoritative canonical path */
            char dst_path[VFS_MAX_PATH];
            uint32_t cut, j, k, namelen, plen;
            /* Fail CLOSED: without the source path the destination cannot be
             * verified against unveil -- deny (NULL on a path-alloc failure). */
            if (!src_path || src_path[0] == '\0')
                return STATUS_ACCESS_DENIED;
            for (plen = 0; src_path[plen]; plen++)
                ;
            cut = plen;
            while (cut > 0 && src_path[cut - 1] != '\\')
                cut--;                         /* strip the source's last component */
            for (namelen = 0; ascii_name[namelen]; namelen++)
                ;
            /* Deny on truncation: unveil must authorize the SAME complete path
             * the rename will use, not a silently shortened prefix. */
            if (cut + namelen >= VFS_MAX_PATH)
                return STATUS_ACCESS_DENIED;
            j = 0;
            for (k = 0; k < cut; k++)
                dst_path[j++] = src_path[k];
            for (k = 0; k < namelen; k++)
                dst_path[j++] = ascii_name[k];
            dst_path[j] = '\0';
            if (unveil_check(task_current(), dst_path, UNVEIL_C) != STATUS_SUCCESS)
                return STATUS_ACCESS_DENIED;
        }
        /* Rename must be supported by the FS; a node with no parent/rename op
         * cannot be renamed -- report the failure instead of a false success
         * that would also wrongly poison the handle's path below. */
        if (!node->parent || !node->parent->ops || !node->parent->ops->rename)
            return STATUS_NOT_SUPPORTED;
        if (node->parent->ops->rename(node->parent, node->name, ascii_name, 0) != 0)
            return STATUS_UNSUCCESSFUL;
        /* fo->path is IMMUTABLE for this handle's life (allocated at open, freed
         * once at file_on_delete): mutating it here would reintroduce a cross-
         * handle UAF/double-free and a rename-OOM bypass. Instead, MARK the path
         * stale (only after a REAL rename) so a later same-handle path-mutating
         * setinfo fails closed under unveil (it would otherwise authorize against
         * the old name). Node-shared canonical-path tracking (which would let it
         * succeed correctly) is the filed TODO-12 s13 work. */
        fo->path_stale = 1;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }
    case FileAllocationInformation: {
        /* Pre-allocate disk space -- map to truncate for simplicity */
        FILE_ALLOCATION_INFORMATION *ai;
        if (length < sizeof(FILE_ALLOCATION_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        ai = (FILE_ALLOCATION_INFORMATION *)info;
        if (node->ops && node->ops->truncate) {
            /* Report a failed pre-allocation (e.g. no space) instead of
             * silently returning success -- matches FileEndOfFileInformation. */
            if (ai->AllocationSize > node->size &&
                node->ops->truncate(node, ai->AllocationSize) != 0)
                return STATUS_UNSUCCESSFUL;
        }
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtDeleteFile ------------------------------------------------------- */

static NTSTATUS NtDeleteFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    /* a1 = OBJECT_ATTRIBUTES* (we extract the path) */
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a1;
    char path[260];
    char resolved[VFS_MAX_PATH];
    int ret;
    NTSTATUS ns;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return STATUS_INVALID_PARAMETER;

    /* Snapshot the mutable UNICODE_STRING fields ONCE, then validate + narrow
     * only the snapshot via the fail-closed helper (no TOCTOU re-read). */
    {
        const uint16_t *buf = oa->ObjectName->Buffer;
        uint32_t nlen = oa->ObjectName->Length;
        uint32_t nmax = oa->ObjectName->MaximumLength;
        ns = nt_wname_to_ascii(buf, nlen, nmax, path, sizeof(path), (uint32_t *)0);
        if (ns != STATUS_SUCCESS)
            return ns;
    }

    /* Resolve relative paths against the caller's cwd before the VFS call. */
    if (task_resolve_path(path, resolved, sizeof(resolved)) != 0)
        return STATUS_OBJECT_PATH_INVALID;

    /* Deleting a file is a cpath operation (pledge) and needs unveil 'c' on the
     * path. VFS_O_DELETE_ON_CLOSE maps to both. A pledge violation returns and
     * the dispatcher terminates; an unveil denial returns STATUS_ACCESS_DENIED. */
    if (pledge_user_mode()) {
        struct task *t = task_current();
        NTSTATUS pr = pledge_check_file(t, VFS_O_DELETE_ON_CLOSE);
        if (pr != STATUS_SUCCESS)
            return pr;
        pr = unveil_check(t, resolved, UNVEIL_C);
        if (pr != STATUS_SUCCESS)
            return pr;
    }

    ret = vfs_unlink(resolved);
    return (ret == 0) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

/* ---- NtFlushBuffersFile ------------------------------------------------- */

static NTSTATUS NtFlushBuffersFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fh = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    FILE_OBJECT *fo;

    (void)a3; (void)a4; (void)a5; (void)a6;

    fo = file_from_handle(fh);
    if (!fo || !fo->vfs_node)
        return STATUS_INVALID_HANDLE;

    /* vfs_flush is the durability boundary (cache write-back + device
     * cache sync); a swallowed failure here would let FlushFileBuffers
     * acknowledge data the disk never accepted. */
    {
        NTSTATUS st = (vfs_flush(fo->vfs_node) == 0)
                      ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        if (iosb) {
            iosb->Status = st;
            iosb->Information = 0;
        }
        return st;
    }
}

/* ---- NtLockFile / NtUnlockFile ------------------------------------------ */

static NTSTATUS NtLockFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fh = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a5;
    uint64_t offset = a3;
    uint64_t length = a4;
    FILE_OBJECT *fo;
    int ret;

    (void)a2; /* Event */ (void)a6;

    fo = file_from_handle(fh);
    if (!fo || !fo->vfs_node)
        return STATUS_INVALID_HANDLE;

    ret = vfs_lock_file(fo->vfs_node, task_current()->pid,
                        offset, length, 1 /* exclusive */);
    if (ret != 0)
        return STATUS_LOCK_NOT_GRANTED;

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NtUnlockFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fh = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    uint64_t offset = a3;
    uint64_t length = a4;
    FILE_OBJECT *fo;
    int ret;

    (void)a5; (void)a6;

    fo = file_from_handle(fh);
    if (!fo || !fo->vfs_node)
        return STATUS_INVALID_HANDLE;

    ret = vfs_unlock_file(fo->vfs_node, task_current()->pid,
                          offset, length);
    if (ret != 0)
        return STATUS_RANGE_NOT_LOCKED;

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = 0;
    }
    return STATUS_SUCCESS;
}

/* ---- NtDeviceIoControlFile ---------------------------------------------- */

static NTSTATUS NtDeviceIoControlFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    /* a1=Handle, a2=Event, a3=ApcRoutine, a4=ApcContext,
     * a5=IoStatusBlock, a6 encodes IoControlCode.
     * For now: route to VFS ioctl if available. */
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Device I/O control requires the IRP framework which is not yet
     * implemented. Return STATUS_INVALID_DEVICE_REQUEST until IRP
     * dispatch is available (-> XREF: TODO-04 drivers). */
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ---- NtFsControlFile ---------------------------------------------------- */

static NTSTATUS NtFsControlFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ---- NtNotifyChangeDirectoryFile ---------------------------------------- */

static NTSTATUS NtNotifyChangeDirectoryFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Change notifications require async I/O completion (IRP pending).
     * Return STATUS_PENDING is incorrect without actual queuing.
     * Return STATUS_INVALID_DEVICE_REQUEST for now. */
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ---- NtQueryVolumeInformationFile --------------------------------------- */

static NTSTATUS NtQueryVolumeInformationFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fh = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    void *buf = (void *)a3;
    uint32_t length = (uint32_t)a4;
    uint32_t fs_class = (uint32_t)a5;
    FILE_OBJECT *fo;

    (void)a6;

    fo = file_from_handle(fh);
    if (!fo || !fo->vfs_node)
        return STATUS_INVALID_HANDLE;

    if (!buf)
        return STATUS_INVALID_PARAMETER;

    switch (fs_class) {
    case FileFsSizeInformation: {
        FILE_FS_SIZE_INFORMATION *si;
        if (length < sizeof(FILE_FS_SIZE_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        si = (FILE_FS_SIZE_INFORMATION *)buf;
        si->TotalAllocationUnits = 256 * 1024;  /* 1 GiB in 4K units */
        si->AvailableAllocationUnits = 128 * 1024;  /* 512 MiB free */
        si->SectorsPerAllocationUnit = 8;  /* 4K = 8 * 512 */
        si->BytesPerSector = 512;
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = sizeof(FILE_FS_SIZE_INFORMATION);
        }
        return STATUS_SUCCESS;
    }
    case FileFsVolumeInformation: {
        /* Fixed header ends at VolumeLabel; the label is a variable field.
         * The caller buffer must hold the header AND the full label or we
         * would write past its declared Length -- report BUFFER_OVERFLOW
         * with the required size instead of overrunning. */
        FILE_FS_VOLUME_INFORMATION *vi;
        const uint32_t hdr =
            (uint32_t)__builtin_offsetof(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
        const uint32_t label_bytes = 10 * 2;  /* "Impossible" in UTF-16 */
        if (length < hdr)
            return STATUS_BUFFER_TOO_SMALL;
        vi = (FILE_FS_VOLUME_INFORMATION *)buf;
        vi->VolumeCreationTime = vfs_seconds_to_filetime(0);
        vi->VolumeSerialNumber = 0x494D5053;  /* "IMPS" */
        vi->SupportsObjects = 1;
        vi->_pad = 0;
        vi->VolumeLabelLength = label_bytes;
        if (length < hdr + label_bytes) {
            if (iosb) {
                iosb->Status = STATUS_BUFFER_OVERFLOW;
                iosb->Information = hdr + label_bytes;
            }
            return STATUS_BUFFER_OVERFLOW;
        }
        (void)ascii_to_utf16("Impossible", vi->VolumeLabel, 32);
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = hdr + label_bytes;
        }
        return STATUS_SUCCESS;
    }
    case FileFsAttributeInformation: {
        FILE_FS_ATTRIBUTE_INFORMATION *ai;
        const uint32_t hdr =
            (uint32_t)__builtin_offsetof(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
        const uint32_t name_bytes = 4 * 2;  /* "IXFS" in UTF-16 */
        if (length < hdr)
            return STATUS_BUFFER_TOO_SMALL;
        ai = (FILE_FS_ATTRIBUTE_INFORMATION *)buf;
        ai->FileSystemAttributes = 0x00000003;  /* case sensitive + case preserved */
        ai->MaximumComponentNameLength = 255;
        ai->FileSystemNameLength = name_bytes;
        if (length < hdr + name_bytes) {
            if (iosb) {
                iosb->Status = STATUS_BUFFER_OVERFLOW;
                iosb->Information = hdr + name_bytes;
            }
            return STATUS_BUFFER_OVERFLOW;
        }
        (void)ascii_to_utf16("IXFS", ai->FileSystemName, 16);
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = hdr + name_bytes;
        }
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

/* ---- NtQueryAttributesFile ---------------------------------------------- */

static NTSTATUS NtQueryAttributesFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a1;
    FILE_BASIC_INFORMATION *bi = (FILE_BASIC_INFORMATION *)a2;
    char path[260];
    char resolved[VFS_MAX_PATH];
    struct vfs_stat st;
    int ret;
    NTSTATUS ns;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer || !bi)
        return STATUS_INVALID_PARAMETER;

    /* Snapshot the mutable UNICODE_STRING fields ONCE, then validate + narrow
     * only the snapshot via the fail-closed helper (no TOCTOU re-read). */
    {
        const uint16_t *buf = oa->ObjectName->Buffer;
        uint32_t nlen = oa->ObjectName->Length;
        uint32_t nmax = oa->ObjectName->MaximumLength;
        ns = nt_wname_to_ascii(buf, nlen, nmax, path, sizeof(path), (uint32_t *)0);
        if (ns != STATUS_SUCCESS)
            return ns;
    }

    /* Resolve relative paths against the caller's cwd before the VFS call. */
    if (task_resolve_path(path, resolved, sizeof(resolved)) != 0)
        return STATUS_OBJECT_PATH_INVALID;

    /* Reading metadata by path needs unveil 'r' (the rpath pledge category is
     * enforced coarsely at the dispatcher). Denied paths look nonexistent. */
    if (pledge_user_mode() &&
        unveil_check(task_current(), resolved, UNVEIL_R) != STATUS_SUCCESS)
        return STATUS_ACCESS_DENIED;

    ret = vfs_stat(resolved, &st);
    if (ret != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    bi->CreationTime = vfs_seconds_to_filetime(st.ctime);
    bi->LastAccessTime = vfs_seconds_to_filetime(st.atime);
    bi->LastWriteTime = vfs_seconds_to_filetime(st.mtime);
    bi->ChangeTime = vfs_seconds_to_filetime(st.mtime);
    bi->FileAttributes = (st.type == VFS_DIRECTORY)
        ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    bi->_pad = 0;
    return STATUS_SUCCESS;
}

/* ---- NtCancelIoFile / NtCancelIoFileEx ---------------------------------- */

static NTSTATUS NtCancelIoFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    (void)a1; (void)a3; (void)a4; (void)a5; (void)a6;
    /* No async I/O queue yet -- all I/O is synchronous.
     * Return success (nothing to cancel). */
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NtCancelIoFileEx_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a4; (void)a5;
    return NtCancelIoFile_handler(a1, a3, 0, 0, 0, a6);
}

/* ---- I/O Completion Ports -----------------------------------------------
 *
 * IOCP "handle" encoding: idx + 0x10000. Not a real Ob handle; a real
 * handle-table integration lands alongside the full I/O completion port
 * rewrite. For now the wide offset keeps IOCP handles distinguishable
 * from Ob handles at a glance when debugging.
 *
 * Lock: each port has an internal spinlock that protects head/tail/count
 * /entries. Added 2026-04-15 with the ALPC completion-list work when ALPC
 * started posting completion packets concurrently with user-mode NtSet/Remove.
 */

static IO_COMPLETION_PORT s_iocp_pool[16];
static uint32_t s_iocp_allocated;
static spinlock_t s_iocp_allocator_lock;   /* protects s_iocp_allocated */

static NTSTATUS iocp_index_from_handle(HANDLE h, uint32_t *out_idx)
{
    uint64_t val = (uint64_t)(uintptr_t)h;
    if (val < 0x10000u)
        return STATUS_INVALID_HANDLE;
    uint32_t idx = (uint32_t)(val - 0x10000u);
    uint64_t irqf;
    uint32_t allocated;
    spin_lock_irqsave(&s_iocp_allocator_lock, &irqf);
    allocated = s_iocp_allocated;
    spin_unlock_irqrestore(&s_iocp_allocator_lock, irqf);
    if (idx >= allocated)
        return STATUS_INVALID_HANDLE;
    *out_idx = idx;
    return STATUS_SUCCESS;
}

NTSTATUS io_completion_validate_handle(HANDLE iocp_handle)
{
    uint32_t idx;
    return iocp_index_from_handle(iocp_handle, &idx);
}

static NTSTATUS iocp_enqueue_locked_call(IO_COMPLETION_PORT *port,
                                         uint64_t key, uint64_t apc,
                                         NTSTATUS status, uint64_t info)
{
    uint64_t irqf;
    IO_COMPLETION_ENTRY *entry;

    spin_lock_irqsave(&port->lock, &irqf);
    if (port->count >= IOCP_MAX_ENTRIES) {
        spin_unlock_irqrestore(&port->lock, irqf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    entry = &port->entries[port->tail];
    entry->KeyContext = key;
    entry->ApcContext = apc;
    entry->IoStatus = status;
    entry->_pad = 0;
    entry->IoStatusInformation = info;
    port->tail = (port->tail + 1) % IOCP_MAX_ENTRIES;
    port->count++;
    spin_unlock_irqrestore(&port->lock, irqf);
    return STATUS_SUCCESS;
}

NTSTATUS io_completion_post(HANDLE iocp_handle, uint64_t key,
                            uint64_t apc, NTSTATUS status, uint64_t info)
{
    uint32_t idx;
    NTSTATUS st = iocp_index_from_handle(iocp_handle, &idx);
    if (!NT_SUCCESS(st))
        return st;
    return iocp_enqueue_locked_call(&s_iocp_pool[idx], key, apc,
                                    status, info);
}

static NTSTATUS NtCreateIoCompletion_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    uint64_t irqf;
    uint32_t idx;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    spin_lock_irqsave(&s_iocp_allocator_lock, &irqf);
    if (s_iocp_allocated >= 16) {
        spin_unlock_irqrestore(&s_iocp_allocator_lock, irqf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* Reserve the next slot but do NOT publish it to iocp_index_from_handle
     * (via s_iocp_allocated bump) until head/tail/count/lock are
     * initialized. Otherwise a concurrent NtSet/Remove or
     * io_completion_post could walk freshly-bumped storage with stale
     * head/tail/count values, or with lock.flag left at whatever the
     * zero-initialized static value was. Init-then-publish closes the
     * race; the allocator lock serializes it against other creators. */
    idx = s_iocp_allocated;
    s_iocp_pool[idx].head = 0;
    s_iocp_pool[idx].tail = 0;
    s_iocp_pool[idx].count = 0;
    s_iocp_pool[idx]._pad_count = 0;
    s_iocp_pool[idx].lock.flag = 0;
    s_iocp_allocated = idx + 1;
    spin_unlock_irqrestore(&s_iocp_allocator_lock, irqf);

    *out = (HANDLE)(uintptr_t)(idx + 0x10000u);
    return STATUS_SUCCESS;
}

static NTSTATUS NtSetIoCompletion_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t idx;
    NTSTATUS st;

    (void)a6;
    st = iocp_index_from_handle((HANDLE)(uintptr_t)a1, &idx);
    if (!NT_SUCCESS(st))
        return st;

    return iocp_enqueue_locked_call(&s_iocp_pool[idx], a2, a3,
                                    (NTSTATUS)(int32_t)a4, a5);
}

static NTSTATUS NtRemoveIoCompletion_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t idx;
    uint64_t *key_out = (uint64_t *)a2;
    uint64_t *apc_out = (uint64_t *)a3;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a4;
    IO_COMPLETION_PORT *port;
    IO_COMPLETION_ENTRY *entry;
    uint64_t irqf;
    NTSTATUS st;

    (void)a5; /* Timeout -- non-blocking for now */
    (void)a6;

    st = iocp_index_from_handle((HANDLE)(uintptr_t)a1, &idx);
    if (!NT_SUCCESS(st))
        return st;

    port = &s_iocp_pool[idx];
    spin_lock_irqsave(&port->lock, &irqf);
    if (port->count == 0) {
        spin_unlock_irqrestore(&port->lock, irqf);
        return STATUS_TIMEOUT;
    }
    entry = &port->entries[port->head];
    uint64_t e_key = entry->KeyContext;
    uint64_t e_apc = entry->ApcContext;
    NTSTATUS e_st  = entry->IoStatus;
    uint64_t e_info = entry->IoStatusInformation;
    port->head = (port->head + 1) % IOCP_MAX_ENTRIES;
    port->count--;
    spin_unlock_irqrestore(&port->lock, irqf);

    if (key_out) *key_out = e_key;
    if (apc_out) *apc_out = e_apc;
    if (iosb) {
        iosb->Status = e_st;
        iosb->Information = e_info;
    }
    return STATUS_SUCCESS;
}

/* ---- NtCreateMailslotFile ----------------------------------------------- */

static NTSTATUS NtCreateMailslotFile_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Mailslots are one-way IPC; map to named pipe with read-only end */
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ---- NtReadFileScatter / NtWriteFileGather ------------------------------ */

static NTSTATUS NtReadFileScatter_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Scatter/gather requires page-aligned buffer segments.
     * Map to sequential reads when needed. */
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS NtWriteFileGather_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ---- Registration ------------------------------------------------------- */

void nt_file_register_ssdt(void)
{
    ssdt_register(SSDT_NtQueryInformationFile,
                  (SSDT_HANDLER)NtQueryInformationFile_handler);
    ssdt_register(SSDT_NtSetInformationFile,
                  (SSDT_HANDLER)NtSetInformationFile_handler);
    ssdt_register(SSDT_NtDeleteFile,
                  (SSDT_HANDLER)NtDeleteFile_handler);
    ssdt_register(SSDT_NtFlushBuffersFile,
                  (SSDT_HANDLER)NtFlushBuffersFile_handler);
    ssdt_register(SSDT_NtDeviceIoControlFile,
                  (SSDT_HANDLER)NtDeviceIoControlFile_handler);
    ssdt_register(SSDT_NtFsControlFile,
                  (SSDT_HANDLER)NtFsControlFile_handler);
    ssdt_register(SSDT_NtLockFile,
                  (SSDT_HANDLER)NtLockFile_handler);
    ssdt_register(SSDT_NtUnlockFile,
                  (SSDT_HANDLER)NtUnlockFile_handler);
    ssdt_register(SSDT_NtNotifyChangeDirectoryFile,
                  (SSDT_HANDLER)NtNotifyChangeDirectoryFile_handler);
    ssdt_register(SSDT_NtQueryVolumeInformationFile,
                  (SSDT_HANDLER)NtQueryVolumeInformationFile_handler);
    ssdt_register(SSDT_NtQueryAttributesFile,
                  (SSDT_HANDLER)NtQueryAttributesFile_handler);
    ssdt_register(SSDT_NtCancelIoFile,
                  (SSDT_HANDLER)NtCancelIoFile_handler);
    ssdt_register(SSDT_NtCancelIoFileEx,
                  (SSDT_HANDLER)NtCancelIoFileEx_handler);
    ssdt_register(SSDT_NtCreateIoCompletion,
                  (SSDT_HANDLER)NtCreateIoCompletion_handler);
    ssdt_register(SSDT_NtSetIoCompletion,
                  (SSDT_HANDLER)NtSetIoCompletion_handler);
    ssdt_register(SSDT_NtRemoveIoCompletion,
                  (SSDT_HANDLER)NtRemoveIoCompletion_handler);
    ssdt_register(SSDT_NtCreateMailslotFile,
                  (SSDT_HANDLER)NtCreateMailslotFile_handler);
    ssdt_register(SSDT_NtReadFileScatter,
                  (SSDT_HANDLER)NtReadFileScatter_handler);
    ssdt_register(SSDT_NtWriteFileGather,
                  (SSDT_HANDLER)NtWriteFileGather_handler);

    klog(LOG_INFO, "nt", "NT file metadata: 19 handlers registered (S13)");
}
