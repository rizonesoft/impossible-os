/* ============================================================================
 * nt_syscall.c -- NT native syscall wrappers (SSDT migration layer)
 *
 * Wraps the existing SYS_* implementations in NtXxx functions with proper
 * NTSTATUS returns and SSDT_HANDLER signatures. Each function is registered
 * at the SSDT index defined in service_numbers.h.
 *
 * §5: existing SYS_* wrappers with NtXxx naming and NTSTATUS returns.
 * §6: proper NtCreateFile/NtOpenFile/NtReadFile/NtWriteFile/NtClose via OB.
 * The INT 0x80 handler continues to work for backward compatibility.
 * ============================================================================ */

#include "kernel/nt/nt_syscall.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/nt_file.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/syscall.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/timer.h"
#include "kernel/acpi.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/shmem.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_file.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/pmm.h"
#include "kernel/smp.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/serial.h"
#include "desktop/terminal.h"

/* ---- Helper: extract path from OBJECT_ATTRIBUTES ----------------------- */
static const char *oa_extract_path(OBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return (const char *)0;
    /* Strip \??\ prefix if present (NT device namespace → drive letter) */
    {
        const char *p = (const char *)oa->ObjectName->Buffer;
        if (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\')
            return p + 4;
        return p;
    }
}

/* ---- Helper: map CreateDisposition to VFS flags ------------------------ */
static uint32_t disposition_to_vfs(uint32_t disp, uint32_t access,
                                   uint32_t options)
{
    uint32_t vfs = 0;

    if (access & GENERIC_READ)  vfs |= VFS_O_READ;
    if (access & GENERIC_WRITE) vfs |= VFS_O_WRITE;
    /* If neither generic flag set, default to read */
    if (!(vfs & (VFS_O_READ | VFS_O_WRITE)))
        vfs |= VFS_O_READ;

    switch (disp) {
    case FILE_OPEN:         /* must exist */
        break;
    case FILE_CREATE:       /* must NOT exist -- create */
        vfs |= VFS_O_CREATE;
        break;
    case FILE_OPEN_IF:      /* open or create */
        vfs |= VFS_O_CREATE;
        break;
    case FILE_SUPERSEDE:    /* replace or create */
        vfs |= VFS_O_CREATE | VFS_O_TRUNC;
        break;
    case FILE_OVERWRITE:    /* must exist, truncate */
        vfs |= VFS_O_TRUNC;
        break;
    case FILE_OVERWRITE_IF: /* truncate or create */
        vfs |= VFS_O_CREATE | VFS_O_TRUNC;
        break;
    }

    if (options & FILE_DELETE_ON_CLOSE)
        vfs |= VFS_O_DELETE_ON_CLOSE;

    return vfs;
}

/* ---- NtCreateFile -------------------------------------------------------
 * SSDT 0x0010 -- full NT file creation/open.
 * a1 = HANDLE* FileHandle (out), a2 = ACCESS_MASK DesiredAccess,
 * a3 = OBJECT_ATTRIBUTES*, a4 = IO_STATUS_BLOCK*,
 * a5 = CreateDisposition, a6 = ShareAccess | (CreateOptions << 16).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateFile_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    ACCESS_MASK access = (ACCESS_MASK)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a4;
    uint32_t disposition = (uint32_t)a5;
    uint32_t share_access = (uint32_t)(a6 & 0xFFFF);
    uint32_t options = (uint32_t)(a6 >> 16);
    const char *path;
    uint32_t vfs_flags;
    int existed;
    HANDLE h;

    (void)share_access;  /* future: pass to vfs_open share mode bits */

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;
    if (!oa || !oa->ObjectName)
        return STATUS_INVALID_PARAMETER;

    path = oa_extract_path(oa);
    if (!path)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Check if file exists before open (for IOSB Information) */
    {
        struct vfs_node *probe = vfs_open(path, VFS_O_READ);
        existed = (probe != (struct vfs_node *)0);
        if (probe)
            vfs_close(probe);
    }

    /* FILE_CREATE requires file not to exist */
    if (disposition == FILE_CREATE && existed) {
        if (iosb) {
            iosb->Status = STATUS_OBJECT_NAME_COLLISION;
            iosb->Information = FILE_EXISTS;
        }
        return STATUS_OBJECT_NAME_COLLISION;
    }

    /* FILE_OPEN and FILE_OVERWRITE require file to exist */
    if ((disposition == FILE_OPEN || disposition == FILE_OVERWRITE) && !existed) {
        if (iosb) {
            iosb->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            iosb->Information = FILE_DOES_NOT_EXIST;
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    vfs_flags = disposition_to_vfs(disposition, access, options);

    /* Use ob_create_file_handle for the open+wrap+handle allocation */
    h = ob_create_file_handle(path, vfs_flags);
    if (h == INVALID_HANDLE_VALUE) {
        if (iosb) {
            iosb->Status = STATUS_UNSUCCESSFUL;
            iosb->Information = 0;
        }
        return STATUS_UNSUCCESSFUL;
    }

    *out_handle = h;

    /* Populate IOSB Information */
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        switch (disposition) {
        case FILE_SUPERSEDE:
            iosb->Information = existed ? FILE_SUPERSEDED : FILE_CREATED;
            break;
        case FILE_OPEN:
            iosb->Information = FILE_OPENED;
            break;
        case FILE_CREATE:
            iosb->Information = FILE_CREATED;
            break;
        case FILE_OPEN_IF:
            iosb->Information = existed ? FILE_OPENED : FILE_CREATED;
            break;
        case FILE_OVERWRITE:
            iosb->Information = FILE_OVERWRITTEN;
            break;
        case FILE_OVERWRITE_IF:
            iosb->Information = existed ? FILE_OVERWRITTEN : FILE_CREATED;
            break;
        default:
            iosb->Information = FILE_OPENED;
            break;
        }
    }

    return STATUS_SUCCESS;
}

/* ---- NtOpenFile ---------------------------------------------------------
 * SSDT 0x0011 -- subset of NtCreateFile with FILE_OPEN disposition.
 * a1 = HANDLE* FileHandle (out), a2 = ACCESS_MASK DesiredAccess,
 * a3 = OBJECT_ATTRIBUTES*, a4 = IO_STATUS_BLOCK*,
 * a5 = ShareAccess, a6 = OpenOptions.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtOpenFile_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    /* Pack share_access and options for NtCreateFile: a5=FILE_OPEN,
     * a6 = share | (options << 16) */
    uint32_t share = (uint32_t)a5;
    uint32_t opts = (uint32_t)a6;
    return NtCreateFile_handler(a1, a2, a3, a4,
                                FILE_OPEN,
                                (uint64_t)share | ((uint64_t)opts << 16));
}

/* ---- NtWriteFile --------------------------------------------------------
 * SSDT 0x0013 -- file/pipe/stdout write.
 * a1 = HANDLE, a2 = IO_STATUS_BLOCK*, a3 = buffer, a4 = length,
 * a5 = uint64_t* ByteOffset (NULL = use current offset).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtWriteFile(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fd = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    const char *buf = (const char *)a3;
    uint64_t len = a4;
    uint64_t *byte_offset = (uint64_t *)a5;
    uint64_t i;

    (void)a6;

    if (!buf || len == 0) {
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    /* Handle-based file/pipe write */
    if (fd != (HANDLE)STDOUT_FD) {
        HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
            &task_current()->handle_table, fd);
        if (!entry || !entry->object)
            goto write_bad_handle;

        {
            OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(entry->object);
            FILE_OBJECT *fo = (FILE_OBJECT *)entry->object;

            if (hdr->type != ObpFileType)
                goto write_bad_handle;

            /* ByteOffset override */
            if (byte_offset)
                fo->offset = *byte_offset;

            if (fo->pipe_id >= 0) {
                int64_t written = pipe_write(fo->pipe_id, buf, (uint32_t)len);
                if (written < 0)
                    goto write_bad_handle;
                if (iosb) {
                    iosb->Status = STATUS_SUCCESS;
                    iosb->Information = (uint64_t)written;
                }
                return STATUS_SUCCESS;
            }

            if (fo->vfs_node) {
                int64_t written = vfs_write(fo->vfs_node,
                    (uint32_t)fo->offset, (uint32_t)len,
                    (const uint8_t *)buf);
                if (written < 0)
                    goto write_bad_handle;
                fo->offset += (uint64_t)written;
                if (iosb) {
                    iosb->Status = STATUS_SUCCESS;
                    iosb->Information = (uint64_t)written;
                }
                return STATUS_SUCCESS;
            }
        }
write_bad_handle:
        if (iosb) {
            iosb->Status = STATUS_INVALID_HANDLE;
            iosb->Information = 0;
        }
        return STATUS_INVALID_HANDLE;
    }

    /* stdout: echo to serial + terminal */
    for (i = 0; i < len; i++) {
        if (buf[i] == '\0')
            break;
        serial_putchar(buf[i]);
        if (terminal_is_open())
            terminal_putchar(buf[i]);
    }

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = i;
    }
    return STATUS_SUCCESS;
}

/* ---- NtReadFile ---------------------------------------------------------
 * SSDT 0x0012 -- file/pipe/stdin read.
 * a1 = HANDLE, a2 = IO_STATUS_BLOCK*, a3 = buffer, a4 = length,
 * a5 = uint64_t* ByteOffset (NULL = use current offset).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtReadFile(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fd = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    char *dst = (char *)a3;
    uint64_t len = a4;
    uint64_t *byte_offset = (uint64_t *)a5;
    uint64_t i;
    char c;

    (void)a6;

    if (!dst || len == 0) {
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    /* Handle-based file/pipe read */
    if (fd != (HANDLE)STDIN_FD) {
        HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
            &task_current()->handle_table, fd);
        if (!entry || !entry->object)
            goto read_bad_handle;

        {
            OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(entry->object);
            FILE_OBJECT *fo = (FILE_OBJECT *)entry->object;

            if (hdr->type != ObpFileType)
                goto read_bad_handle;

            /* ByteOffset override */
            if (byte_offset)
                fo->offset = *byte_offset;

            if (fo->pipe_id >= 0) {
                int64_t bytes = pipe_read(fo->pipe_id, dst, (uint32_t)len);
                if (bytes < 0)
                    goto read_bad_handle;
                if (iosb) {
                    iosb->Status = STATUS_SUCCESS;
                    iosb->Information = (uint64_t)bytes;
                }
                return STATUS_SUCCESS;
            }

            if (fo->vfs_node) {
                int64_t bytes = vfs_read(fo->vfs_node,
                    (uint32_t)fo->offset, (uint32_t)len,
                    (uint8_t *)dst);
                if (bytes < 0)
                    goto read_bad_handle;
                fo->offset += (uint64_t)bytes;
                if (iosb) {
                    iosb->Status = (bytes == 0) ? STATUS_END_OF_FILE
                                                : STATUS_SUCCESS;
                    iosb->Information = (uint64_t)bytes;
                }
                return (bytes == 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
            }
        }
read_bad_handle:
        if (iosb) {
            iosb->Status = STATUS_INVALID_HANDLE;
            iosb->Information = 0;
        }
        return STATUS_INVALID_HANDLE;
    }

    /* stdin: blocking read from keyboard/serial/terminal */
    for (;;) {
        if (terminal_is_open())
            c = terminal_trygetchar();
        else {
            c = keyboard_trygetchar();
            if (!c)
                c = serial_trygetchar();
        }
        if (c != 0)
            break;
        yield();
    }
    dst[0] = c;

    for (i = 1; i < len; i++) {
        if (terminal_is_open())
            c = terminal_trygetchar();
        else {
            c = keyboard_trygetchar();
            if (!c)
                c = serial_trygetchar();
        }
        if (c == 0)
            break;
        dst[i] = c;
    }

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = i;
    }
    return STATUS_SUCCESS;
}

/* NtTerminateProcess moved to nt_process.c (§7) */

/* ---- NtYieldExecution ---------------------------------------------------
 * SSDT 0x0044 -- wraps SYS_YIELD.
 * No arguments. Always returns STATUS_SUCCESS.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtYieldExecution(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    yield();
    return STATUS_SUCCESS;
}

/* NtWaitForSingleObject moved to nt_sync.c (§8) -- now handles all object types */

/* ---- NtQueryDirectoryFile -----------------------------------------------
 * SSDT 0x0017.
 * a1 = HANDLE (directory), a2 = IO_STATUS_BLOCK*,
 * a3 = buffer, a4 = buffer size,
 * a5 = FileInformationClass, a6 = flags (bit 0 = ReturnSingleEntry, bit 1 = RestartScan).
 *
 * Supported info classes:
 *   0 (legacy): filename-only ASCII (original S6 behavior)
 *   1: FILE_DIRECTORY_INFORMATION
 *   3: FILE_BOTH_DIR_INFORMATION
 *  37: FILE_ID_BOTH_DIR_INFORMATION
 * ----------------------------------------------------------------------- */

/* Convert VFS seconds-since-boot to FILETIME */
static FILETIME dirq_vfs_to_filetime(uint32_t secs)
{
    FILETIME base = 133800288000000000ULL;  /* 2026-01-01 in FILETIME */
    return base + (uint64_t)secs * FILETIME_TICKS_PER_SECOND;
}

/* Generate 8.3 short name from a long name (uppercase, truncated) */
static uint32_t dirq_make_short_name(const char *name, uint16_t *out)
{
    uint32_t i, len = 0;
    for (i = 0; i < 12 && name[i]; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[i] = (uint16_t)(uint8_t)c;
        len++;
    }
    return len * 2;  /* byte count */
}

static NTSTATUS NtQueryDirectoryFile(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fh = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    uint8_t *buf = (uint8_t *)a3;
    uint64_t buf_size = a4;
    uint32_t info_class = (uint32_t)a5;
    uint32_t flags = (uint32_t)a6;
    uint32_t return_single = flags & 1;
    uint32_t restart_scan = (flags >> 1) & 1;
    struct vfs_node *dir_node;
    FILE_OBJECT *fo;
    uint64_t offset = 0;
    uint32_t entries_written = 0;
    uint8_t *prev_entry = (uint8_t *)0;

    if (!buf || buf_size == 0)
        return STATUS_INVALID_PARAMETER;

    /* Resolve directory handle -> FILE_OBJECT -> vfs_node */
    fo = (FILE_OBJECT *)0;
    if (fh != 0 && fh != (HANDLE)-1) {
        HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
            &task_current()->handle_table, fh);
        if (entry && entry->object) {
            OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(entry->object);
            if (hdr->type == ObpFileType)
                fo = (FILE_OBJECT *)entry->object;
        }
    }

    if (fo && fo->vfs_node) {
        dir_node = fo->vfs_node;
    } else {
        /* Fallback: C:\ root (legacy compat) */
        dir_node = vfs_get_drive_root('C');
        if (!dir_node)
            return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* Handle RestartScan */
    if (restart_scan && fo)
        fo->dir_enum_index = 0;

    /* Get starting index */
    {
        uint32_t start_index = fo ? fo->dir_enum_index : 0;

        /* Legacy info class 0: filename-only ASCII (original S6 behavior) */
        if (info_class == 0) {
            struct vfs_dirent *de = vfs_readdir(dir_node, start_index);
            if (!de) {
                if (iosb) { iosb->Status = STATUS_NO_MORE_FILES; iosb->Information = 0; }
                return STATUS_NO_MORE_FILES;
            }
            {
                uint64_t i;
                for (i = 0; i < buf_size - 1 && de->name[i]; i++)
                    buf[i] = (uint8_t)de->name[i];
                buf[i] = '\0';
                if (fo) fo->dir_enum_index = start_index + 1;
                if (iosb) { iosb->Status = STATUS_SUCCESS; iosb->Information = i; }
            }
            return STATUS_SUCCESS;
        }

        /* Extended info classes: pack entries into the output buffer */
        while (1) {
            struct vfs_dirent *de = vfs_readdir(dir_node, start_index);
            struct vfs_stat st;
            struct vfs_node *child;
            uint32_t name_bytes, entry_size, aligned_size;
            uint32_t name_len, j;

            if (!de)
                break;

            /* Get per-entry metadata via finddir + stat */
            st.size = 0; st.type = de->type; st.ctime = 0; st.mtime = 0; st.atime = 0; st.blocks = 0;
            child = vfs_finddir(dir_node, de->name);
            if (child && child->ops && child->ops->stat)
                child->ops->stat(child, &st);
            else if (child)
                st.size = child->size;

            /* Compute filename length in bytes (UTF-16) */
            for (name_len = 0; name_len < 255 && de->name[name_len]; name_len++)
                ;
            name_bytes = name_len * 2;

            /* Compute entry size based on info class */
            switch (info_class) {
            case FileDirectoryInformation:
                entry_size = FILE_DIR_INFO_FIXED_SIZE + name_bytes;
                break;
            case FileBothDirectoryInformation:
                entry_size = FILE_BOTH_DIR_INFO_FIXED_SIZE + name_bytes;
                break;
            case FileIdBothDirectoryInformation:
                entry_size = FILE_ID_BOTH_DIR_INFO_FIXED_SIZE + name_bytes;
                break;
            default:
                return STATUS_INVALID_INFO_CLASS;
            }

            /* 8-byte align */
            aligned_size = (entry_size + 7) & ~(uint32_t)7;

            /* Check buffer space (must fit the full aligned extent) */
            if (offset + aligned_size > buf_size)
                break;

            /* Fill the entry */
            {
                uint8_t *dst = buf + offset;
                FILETIME ct = dirq_vfs_to_filetime(st.ctime);
                FILETIME at = dirq_vfs_to_filetime(st.atime);
                FILETIME wt = dirq_vfs_to_filetime(st.mtime);
                uint64_t eof = st.size;
                uint64_t alloc = (eof + 4095) & ~(uint64_t)4095;
                uint32_t attrs = (st.type == VFS_DIRECTORY)
                    ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

                /* Zero the entry region */
                {
                    uint32_t z;
                    uint32_t clear = aligned_size;
                    if (offset + clear > buf_size) clear = (uint32_t)(buf_size - offset);
                    for (z = 0; z < clear; z++)
                        dst[z] = 0;
                }

                if (info_class == FileDirectoryInformation) {
                    FILE_DIRECTORY_INFORMATION *di = (FILE_DIRECTORY_INFORMATION *)dst;
                    di->NextEntryOffset = 0;  /* updated below if more entries */
                    di->FileIndex = start_index;
                    di->CreationTime = ct;
                    di->LastAccessTime = at;
                    di->LastWriteTime = wt;
                    di->ChangeTime = wt;
                    di->EndOfFile = eof;
                    di->AllocationSize = alloc;
                    di->FileAttributes = attrs;
                    di->FileNameLength = name_bytes;
                    for (j = 0; j < name_len; j++)
                        di->FileName[j] = (uint16_t)(uint8_t)de->name[j];
                } else if (info_class == FileBothDirectoryInformation) {
                    FILE_BOTH_DIR_INFORMATION *bi = (FILE_BOTH_DIR_INFORMATION *)dst;
                    bi->NextEntryOffset = 0;
                    bi->FileIndex = start_index;
                    bi->CreationTime = ct;
                    bi->LastAccessTime = at;
                    bi->LastWriteTime = wt;
                    bi->ChangeTime = wt;
                    bi->EndOfFile = eof;
                    bi->AllocationSize = alloc;
                    bi->FileAttributes = attrs;
                    bi->FileNameLength = name_bytes;
                    bi->EaSize = 0;
                    bi->ShortNameLength = (uint8_t)dirq_make_short_name(de->name, bi->ShortName);
                    for (j = 0; j < name_len; j++)
                        bi->FileName[j] = (uint16_t)(uint8_t)de->name[j];
                } else {
                    FILE_ID_BOTH_DIR_INFORMATION *ii = (FILE_ID_BOTH_DIR_INFORMATION *)dst;
                    ii->NextEntryOffset = 0;
                    ii->FileIndex = start_index;
                    ii->CreationTime = ct;
                    ii->LastAccessTime = at;
                    ii->LastWriteTime = wt;
                    ii->ChangeTime = wt;
                    ii->EndOfFile = eof;
                    ii->AllocationSize = alloc;
                    ii->FileAttributes = attrs;
                    ii->FileNameLength = name_bytes;
                    ii->EaSize = 0;
                    ii->ShortNameLength = (uint8_t)dirq_make_short_name(de->name, ii->ShortName);
                    ii->FileId = (uint64_t)de->inode;
                    for (j = 0; j < name_len; j++)
                        ii->FileName[j] = (uint16_t)(uint8_t)de->name[j];
                }

                /* Link previous entry's NextEntryOffset */
                if (prev_entry) {
                    uint32_t *prev_neo = (uint32_t *)prev_entry;
                    *prev_neo = (uint32_t)(dst - prev_entry);
                }

                prev_entry = dst;
                offset += aligned_size;
                entries_written++;
                start_index++;
            }

            if (return_single)
                break;
        }

        /* Update cursor */
        if (fo)
            fo->dir_enum_index = start_index;
    }

    if (entries_written == 0) {
        /* Distinguish: buffer too small vs. directory exhausted.
         * Check if there's a next entry at the current cursor. */
        uint32_t check_idx = fo ? fo->dir_enum_index : 0;
        struct vfs_dirent *check = vfs_readdir(dir_node, check_idx);
        if (check) {
            /* Entry exists but didn't fit -> buffer too small */
            if (iosb) { iosb->Status = STATUS_BUFFER_OVERFLOW; iosb->Information = 0; }
            return STATUS_BUFFER_OVERFLOW;
        }
        if (iosb) { iosb->Status = STATUS_NO_MORE_FILES; iosb->Information = 0; }
        return STATUS_NO_MORE_FILES;
    }

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = offset;
    }
    return STATUS_SUCCESS;
}

/* ---- SystemInformationClass constants ----------------------------------- */
#define SystemBasicInformation              0
#define SystemProcessorInformation          1
#define SystemPerformanceInformation        2
#define SystemTimeOfDayInformation          3
#define SystemProcessInformation            5
#define SystemProcessorPerformanceInfo      8
#define SystemModuleInformation             11
#define SystemHandleInformation             16
#define SystemObjectInformation             17
#define SystemInterruptInformation          23
#define SystemExceptionInformation          33
#define SystemRegistryQuotaInformation      37

/* ---- NtQuerySystemInformation -------------------------------------------
 * SSDT 0x00D0 -- system-wide information queries.
 * a1 = info class, a2 = buffer, a3 = buffer size,
 * a4 = return length pointer.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtQuerySystemInformation(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t info_class = (uint32_t)a1;
    void *buffer = (void *)a2;
    uint32_t buf_size = (uint32_t)a3;
    uint32_t *return_length = (uint32_t *)a4;

    (void)a5; (void)a6;

    switch (info_class) {
    case SystemBasicInformation: {
        /* SYSTEM_BASIC_INFORMATION: page size, processor count, address limits */
        struct {
            uint32_t Reserved;
            uint32_t TimerResolution;
            uint32_t PageSize;
            uint32_t NumberOfPhysicalPages;
            uint32_t LowestPhysicalPageNumber;
            uint32_t HighestPhysicalPageNumber;
            uint32_t AllocationGranularity;
            uint64_t MinimumUserModeAddress;
            uint64_t MaximumUserModeAddress;
            uint64_t ActiveProcessorsAffinityMask;
            uint8_t  NumberOfProcessors;
            uint8_t  _pad[7];
        } *info = buffer;

        if (!buffer || buf_size < 64)
            return STATUS_BUFFER_TOO_SMALL;

        info->Reserved = 0;
        info->TimerResolution = 10000;  /* 10ms default */
        info->PageSize = VMM_PAGE_SIZE;
        info->NumberOfPhysicalPages = (uint32_t)pmm_get_total_frames();
        info->LowestPhysicalPageNumber = 1;
        info->HighestPhysicalPageNumber = info->NumberOfPhysicalPages;
        info->AllocationGranularity = 0x10000;  /* 64 KB */
        info->MinimumUserModeAddress = 0x10000;
        info->MaximumUserModeAddress = 0x7FFFFFFEFFFF;
        info->ActiveProcessorsAffinityMask = ((uint64_t)1 << smp_cpu_count()) - 1;
        info->NumberOfProcessors = (uint8_t)smp_cpu_count();
        {
            int pi;
            for (pi = 0; pi < 7; pi++) info->_pad[pi] = 0;
        }
        if (return_length) *return_length = 64;
        return STATUS_SUCCESS;
    }
    case SystemProcessorInformation: {
        /* SYSTEM_PROCESSOR_INFORMATION: architecture and level */
        struct {
            uint16_t ProcessorArchitecture;
            uint16_t ProcessorLevel;
            uint16_t ProcessorRevision;
            uint16_t MaximumProcessors;
            uint32_t ProcessorFeatureBits;
        } *info = buffer;

        if (!buffer || buf_size < 12)
            return STATUS_BUFFER_TOO_SMALL;

        info->ProcessorArchitecture = 9;  /* AMD64 */
        info->ProcessorLevel = 6;
        info->ProcessorRevision = 0;
        info->MaximumProcessors = (uint16_t)smp_cpu_count();
        info->ProcessorFeatureBits = 0;
        if (return_length) *return_length = 12;
        return STATUS_SUCCESS;
    }
    case SystemPerformanceInformation: {
        /* SYSTEM_PERFORMANCE_INFORMATION: memory stats */
        struct {
            uint64_t AvailablePages;
            uint64_t CommittedPages;
            uint64_t CommitLimit;
        } *info = buffer;

        if (!buffer || buf_size < 24)
            return STATUS_BUFFER_TOO_SMALL;

        info->AvailablePages = pmm_get_free_frames();
        info->CommittedPages = pmm_get_used_frames();
        info->CommitLimit = pmm_get_total_frames();
        if (return_length) *return_length = 24;
        return STATUS_SUCCESS;
    }
    case SystemTimeOfDayInformation: {
        /* Return uptime in seconds */
        uint64_t *out = (uint64_t *)buffer;
        if (!buffer || buf_size < sizeof(uint64_t))
            return STATUS_BUFFER_TOO_SMALL;
        *out = uptime();
        if (return_length)
            *return_length = (uint32_t)sizeof(uint64_t);
        return STATUS_SUCCESS;
    }
    case SystemProcessInformation: {
        /* Process list with PID, state, name */
        struct {
            uint32_t pid;
            uint32_t state;
            char     name[32];
        } *out = buffer;
        uint32_t count = task_count();
        uint32_t max = buf_size / 40;
        uint32_t i, j;

        if (!buffer)
            return STATUS_INVALID_PARAMETER;

        if (return_length)
            *return_length = count * 40;

        if (max == 0)
            return STATUS_BUFFER_TOO_SMALL;

        for (i = 0; i < count && i < max; i++) {
            struct task *t = task_get_by_pid(i);
            if (!t) {
                out[i].pid = i;
                out[i].state = 0xFF;
                out[i].name[0] = '\0';
                continue;
            }
            out[i].pid = t->pid;
            out[i].state = t->state;
            if (t->name) {
                for (j = 0; j < 31 && t->name[j]; j++)
                    out[i].name[j] = t->name[j];
                out[i].name[j] = '\0';
            } else {
                out[i].name[0] = '\0';
            }
        }
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_IMPLEMENTED;
    }
}

/* ---- NtSetSystemInformation (0x00D1) ------------------------------------
 * a1 = SystemInformationClass, a2 = buffer, a3 = length.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtSetSystemInformation(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Privileged operation -- requires SeSystemtimePrivilege (TODO-11) */
    return STATUS_NOT_IMPLEMENTED;
}

/* ---- Shutdown action codes for NtShutdownSystem ------------------------ */
#define ShutdownReboot      0
#define ShutdownPowerOff    1

/* ---- NtShutdownSystem ---------------------------------------------------
 * SSDT 0x00D7 -- wraps SYS_REBOOT and SYS_SHUTDOWN.
 * a1 = action (ShutdownReboot / ShutdownPowerOff).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtShutdownSystem(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t action = (uint32_t)a1;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    switch (action) {
    case ShutdownReboot:
        klog(LOG_INFO, "nt", "NtShutdownSystem: reboot requested");
        acpi_reboot();
        return STATUS_SUCCESS;  /* acpi_reboot does not return */
    case ShutdownPowerOff:
        klog(LOG_INFO, "nt", "NtShutdownSystem: power off requested");
        acpi_shutdown();
        return STATUS_SUCCESS;  /* acpi_shutdown does not return */
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/* ---- NtCreateNamedPipeFile ----------------------------------------------
 * SSDT 0x001B -- wraps SYS_PIPE.
 * a1 = HANDLE* out array [2] (read end, write end).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateNamedPipeFile(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *user_handles = (HANDLE *)a1;
    HANDLE pipe_handles[2];
    int ret;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!user_handles)
        return STATUS_INVALID_PARAMETER;

    ret = ob_create_pipe_handles(&task_current()->handle_table, pipe_handles);
    if (ret != 0)
        return STATUS_NO_MEMORY;

    user_handles[0] = pipe_handles[0];  /* read end */
    user_handles[1] = pipe_handles[1];  /* write end */
    return STATUS_SUCCESS;
}

/* ---- NtClose ------------------------------------------------------------
 * SSDT 0x0000 -- wraps SYS_CLOSEHANDLE.
 * a1 = HANDLE.
 * ----------------------------------------------------------------------- */
static NTSTATUS Nt_Close(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    int ret;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    ret = NtClose(&task_current()->handle_table, handle);
    if (ret != 0)
        return STATUS_INVALID_HANDLE;
    return STATUS_SUCCESS;
}

/* ---- Registration ------------------------------------------------------- */

void nt_syscall_register_ssdt(void)
{
    /* File I/O (§6) */
    ssdt_register(SSDT_NtCreateFile,            (SSDT_HANDLER)NtCreateFile_handler);
    ssdt_register(SSDT_NtOpenFile,              (SSDT_HANDLER)NtOpenFile_handler);
    ssdt_register(SSDT_NtReadFile,              (SSDT_HANDLER)NtReadFile);
    ssdt_register(SSDT_NtWriteFile,             (SSDT_HANDLER)NtWriteFile);
    ssdt_register(SSDT_NtQueryDirectoryFile,    (SSDT_HANDLER)NtQueryDirectoryFile);
    ssdt_register(SSDT_NtCreateNamedPipeFile,   (SSDT_HANDLER)NtCreateNamedPipeFile);

    /* Core object operations (NtWaitForSingleObject moved to nt_sync.c §8) */
    ssdt_register(SSDT_NtClose,                 (SSDT_HANDLER)Nt_Close);

    /* Process and thread (NtTerminateProcess moved to nt_process.c §7) */
    ssdt_register(SSDT_NtYieldExecution,        (SSDT_HANDLER)NtYieldExecution);

    /* System information (§10) */
    ssdt_register(SSDT_NtQuerySystemInformation, (SSDT_HANDLER)NtQuerySystemInformation);
    ssdt_register(SSDT_NtSetSystemInformation,   (SSDT_HANDLER)NtSetSystemInformation);

    /* Shutdown */
    ssdt_register(SSDT_NtShutdownSystem,        (SSDT_HANDLER)NtShutdownSystem);

    /* File metadata, device control, I/O completion (S13) */
    nt_file_register_ssdt();

    klog(LOG_INFO, "nt", "NT syscall: 31 NtXxx handlers registered");
}
