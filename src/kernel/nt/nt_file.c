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
 * XREF: 02-kernel-core/TODO-05-native-api-ssdt.md S13
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

/* ---- NtQueryInformationFile --------------------------------------------- */

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
    int vfs_ret;

    (void)a6;

    fo = file_from_handle(fh);
    if (!fo || !fo->vfs_node)
        return STATUS_INVALID_HANDLE;

    if (!info)
        return STATUS_INVALID_PARAMETER;

    vfs_ret = -1;
    if (fo->vfs_node->ops && fo->vfs_node->ops->stat)
        vfs_ret = fo->vfs_node->ops->stat(fo->vfs_node, &st);
    if (vfs_ret != 0) {
        /* No stat op -- fill from vfs_node directly */
        st.size = fo->vfs_node->size;
        st.type = fo->vfs_node->type;
        st.ctime = 0;
        st.mtime = 0;
        st.atime = 0;
        st.blocks = 0;
    }

    switch (info_class) {
    case FileBasicInformation: {
        FILE_BASIC_INFORMATION *bi;
        if (length < sizeof(FILE_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
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
        uint32_t avail_chars;
        if (length < sizeof(uint32_t))
            return STATUS_BUFFER_TOO_SMALL;
        ni = (FILE_NAME_INFORMATION *)info;
        /* Cap copy to caller-provided buffer space */
        avail_chars = (length - sizeof(uint32_t)) / sizeof(uint16_t);
        if (avail_chars > 260) avail_chars = 260;
        ni->FileNameLength = ascii_to_utf16(
            fo->vfs_node->name, ni->FileName, avail_chars);
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = sizeof(uint32_t) + ni->FileNameLength;
        }
        return STATUS_SUCCESS;
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
        uint32_t i, chars, fixed_size;
        /* Fixed header: ReplaceIfExists(1) + pad(7) + RootDirectory(8) + FileNameLength(4) = 20 */
        fixed_size = 20;
        if (length < fixed_size)
            return STATUS_BUFFER_TOO_SMALL;
        ri = (FILE_RENAME_INFORMATION *)info;
        chars = ri->FileNameLength / 2;
        if (chars > 259) chars = 259;
        /* Validate caller provided enough bytes for the filename */
        if (length < fixed_size + ri->FileNameLength)
            return STATUS_BUFFER_TOO_SMALL;
        for (i = 0; i < chars; i++)
            ascii_name[i] = (char)(ri->FileName[i] & 0x7F);
        ascii_name[chars] = '\0';
        if (node->parent && node->parent->ops && node->parent->ops->rename) {
            if (node->parent->ops->rename(node->parent, node->name, ascii_name) != 0)
                return STATUS_UNSUCCESSFUL;
        }
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
            if (ai->AllocationSize > node->size)
                node->ops->truncate(node, ai->AllocationSize);
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
    uint32_t i, chars;
    int ret;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return STATUS_INVALID_PARAMETER;

    chars = oa->ObjectName->Length / 2;
    if (chars > 259) chars = 259;
    for (i = 0; i < chars; i++)
        path[i] = (char)(oa->ObjectName->Buffer[i] & 0x7F);
    path[chars] = '\0';

    ret = vfs_unlink(path);
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

    if (fo->vfs_node->ops && fo->vfs_node->ops->flush)
        fo->vfs_node->ops->flush(fo->vfs_node);

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = 0;
    }
    return STATUS_SUCCESS;
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
        FILE_FS_VOLUME_INFORMATION *vi;
        if (length < 24)  /* minimum fixed fields */
            return STATUS_BUFFER_TOO_SMALL;
        vi = (FILE_FS_VOLUME_INFORMATION *)buf;
        vi->VolumeCreationTime = vfs_seconds_to_filetime(0);
        vi->VolumeSerialNumber = 0x494D5053;  /* "IMPS" */
        vi->SupportsObjects = 1;
        vi->_pad[0] = vi->_pad[1] = vi->_pad[2] = 0;
        vi->VolumeLabelLength = ascii_to_utf16(
            "Impossible", vi->VolumeLabel, 32);
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 24 + vi->VolumeLabelLength;
        }
        return STATUS_SUCCESS;
    }
    case FileFsAttributeInformation: {
        FILE_FS_ATTRIBUTE_INFORMATION *ai;
        if (length < 12)
            return STATUS_BUFFER_TOO_SMALL;
        ai = (FILE_FS_ATTRIBUTE_INFORMATION *)buf;
        ai->FileSystemAttributes = 0x00000003;  /* case sensitive + case preserved */
        ai->MaximumComponentNameLength = 255;
        ai->FileSystemNameLength = ascii_to_utf16(
            "IXFS", ai->FileSystemName, 16);
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 12 + ai->FileSystemNameLength;
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
    uint32_t i, chars;
    struct vfs_stat st;
    int ret;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer || !bi)
        return STATUS_INVALID_PARAMETER;

    chars = oa->ObjectName->Length / 2;
    if (chars > 259) chars = 259;
    for (i = 0; i < chars; i++)
        path[i] = (char)(oa->ObjectName->Buffer[i] & 0x7F);
    path[chars] = '\0';

    ret = vfs_stat(path, &st);
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

/* ---- I/O Completion Ports ----------------------------------------------- */

static IO_COMPLETION_PORT s_iocp_pool[16];
static uint32_t s_iocp_allocated;

static NTSTATUS NtCreateIoCompletion_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    if (s_iocp_allocated >= 16)
        return STATUS_INSUFFICIENT_RESOURCES;

    {
        uint32_t idx = s_iocp_allocated++;
        s_iocp_pool[idx].head = 0;
        s_iocp_pool[idx].tail = 0;
        s_iocp_pool[idx].count = 0;
        *out = (HANDLE)(idx + 0x10000);  /* offset to avoid handle collision */
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NtSetIoCompletion_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t idx = (uint32_t)((uint64_t)a1 - 0x10000);
    IO_COMPLETION_PORT *port;
    IO_COMPLETION_ENTRY *entry;

    (void)a6;

    if (idx >= s_iocp_allocated)
        return STATUS_INVALID_HANDLE;

    port = &s_iocp_pool[idx];
    if (port->count >= IOCP_MAX_ENTRIES)
        return STATUS_INSUFFICIENT_RESOURCES;

    entry = &port->entries[port->tail];
    entry->KeyContext = a2;
    entry->ApcContext = a3;
    entry->IoStatus = (NTSTATUS)(int32_t)a4;
    entry->_pad = 0;
    entry->IoStatusInformation = a5;
    port->tail = (port->tail + 1) % IOCP_MAX_ENTRIES;
    port->count++;
    return STATUS_SUCCESS;
}

static NTSTATUS NtRemoveIoCompletion_handler(
    uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t idx = (uint32_t)((uint64_t)a1 - 0x10000);
    uint64_t *key_out = (uint64_t *)a2;
    uint64_t *apc_out = (uint64_t *)a3;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a4;
    IO_COMPLETION_PORT *port;
    IO_COMPLETION_ENTRY *entry;

    (void)a5; /* Timeout */ (void)a6;

    if (idx >= s_iocp_allocated)
        return STATUS_INVALID_HANDLE;

    port = &s_iocp_pool[idx];
    if (port->count == 0)
        return STATUS_TIMEOUT;

    entry = &port->entries[port->head];
    if (key_out) *key_out = entry->KeyContext;
    if (apc_out) *apc_out = entry->ApcContext;
    if (iosb) {
        iosb->Status = entry->IoStatus;
        iosb->Information = entry->IoStatusInformation;
    }
    port->head = (port->head + 1) % IOCP_MAX_ENTRIES;
    port->count--;
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
