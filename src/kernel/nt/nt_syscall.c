/* ============================================================================
 * nt_syscall.c -- NT native syscall wrappers (SSDT migration layer)
 *
 * Wraps the existing SYS_* implementations in NtXxx functions with proper
 * NTSTATUS returns and SSDT_HANDLER signatures. Each function is registered
 * at the SSDT index defined in service_numbers.h.
 *
 *: existing SYS_* wrappers with NtXxx naming and NTSTATUS returns.
 *: proper NtCreateFile/NtOpenFile/NtReadFile/NtWriteFile/NtClose via OB.
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
#include "kernel/test/test_usermode.h"  /* test_usermode_capture_start/_byte/_end -- KERNEL_TESTS
                                          * no-op in release builds */
#include "kernel/nt/pledge.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/timer.h"
#include "kernel/acpi.h"
#include "kernel/smbios.h"
#include "kernel/nt/zw.h"
#include "kernel/security/privileges.h"  /* SeSinglePrivilegeCheck + privilege LUIDs */
#include "kernel/cpu_security.h"
#include "kernel/config.h"     /* SystemKernelConfigInformation snapshot source */
#include "kernel/tunables.h"   /* tunable count + lock phase for the config query */
#include "kernel/feature.h"    /* feature count for the config query */
#include "kernel/nt/sysconfig_info.h" /* SYSTEM_KERNEL_CONFIG_INFORMATION ABI */
#include "kernel/nt/nls_syscall_info.h" /* SYSTEM_NLS_INFORMATION ABI */
#include "kernel/nt/knf_syscall_info.h" /* SYSTEM_NOTIFICATION_INFORMATION ABI */
#include "kernel/nt/quota_pressure_info.h" /* SYSTEM_RESOURCE_PRESSURE_INFORMATION ABI */
#include "kernel/quota/quota_stall.h"  /* stall telemetry snapshot source */

/* The wire row count and the kernel domain count are two independent
 * declarations of the same fact. Pinning them together means adding a fourth
 * stall domain is a compile error here rather than a silently truncated class
 * that reports three of four resources. */
_Static_assert(SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT == QUOTA_STALL_DOMAIN_COUNT,
    "resource-pressure row count must match quota_stall_domain_t");
_Static_assert(SYSTEM_RESOURCE_PRESSURE_WINDOW_COUNT == QUOTA_STALL_WINDOW_COUNT,
    "resource-pressure window count must match quota_stall_window_t");
#include "kernel/knf/knf.h"             /* KNF diagnostics counters */
#include "kernel/nt/nls.h"             /* nls_get_version */
#include "kernel/nt/nls_cp.h"          /* nls_cp_get_acp / nls_cp_get_oemcp */
#include "kernel/nt/nls_locale.h"      /* nls_locale_get_system / _user */
#include "kernel/nt/nt_misc.h"         /* nt_locale_get_ui_language / _install */
#include "kernel/policy_lock.h"       /* kernel_lockdown_level_get */

extern void *memcpy(void *dst, const void *src, uint64_t n);
extern void *memset(void *s, int c, uint64_t n);
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

/* ---- Helper: extract path from OBJECT_ATTRIBUTES into a bounded buffer ----
 * The ObjectName Buffer carries an ASCII path in this codebase's ASCII-in-char
 * convention (see the note in nt_file.c and test_alpc.c). The copy is bounded
 * by BOTH the declared Length and the output buffer so a non-NUL-terminated or
 * oversized Buffer cannot overread. The full UTF-16-vs-ASCII decode-convention
 * unification across the NT file-path surface is tracked in TODO-12. */
static NTSTATUS oa_extract_path(OBJECT_ATTRIBUTES *oa, char *out, uint32_t out_size)
{
    const char *src;
    uint32_t max, i;

    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer || !out || out_size == 0)
        return STATUS_INVALID_PARAMETER;

    src = (const char *)oa->ObjectName->Buffer;
    /* Length is the authoritative declared byte count. Validate it against
     * MaximumLength, then REJECT (never truncate) a name that will not fit --
     * a silent truncation would alias an overlong name to a shorter existing
     * object. A zero Length is an empty name (Buffer never scanned) -> caller
     * maps that to OBJECT_NAME_NOT_FOUND. */
    max = (uint32_t)oa->ObjectName->Length;
    if (max > (uint32_t)oa->ObjectName->MaximumLength)
        return STATUS_OBJECT_NAME_INVALID;
    if (max > out_size - 1)
        return STATUS_NAME_TOO_LONG;
    for (i = 0; i < max && src[i]; i++)
        out[i] = src[i];
    out[i] = '\0';

    /* Strip \??\ prefix (NT device namespace -> drive letter) in place. */
    if (out[0] == '\\' && out[1] == '?' && out[2] == '?' && out[3] == '\\') {
        uint32_t j = 0;
        for (i = 4; out[i]; i++)
            out[j++] = out[i];
        out[j] = '\0';
    }
    return STATUS_SUCCESS;
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
    char raw_path[VFS_MAX_PATH];
    char resolved_path[VFS_MAX_PATH];
    uint32_t vfs_flags;
    int existed;
    HANDLE h;
    NTSTATUS ep;

    (void)share_access;  /* future: pass to vfs_open share mode bits */

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;
    if (!oa || !oa->ObjectName)
        return STATUS_INVALID_PARAMETER;

    ep = oa_extract_path(oa, raw_path, sizeof(raw_path));
    if (ep != STATUS_SUCCESS)
        return ep;
    if (!raw_path[0])
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Resolve relative paths against the caller's cwd (absolute paths pass
     * through unchanged, still normalized). vfs_open stays absolute-only. */
    if (task_resolve_path(raw_path, resolved_path, sizeof(resolved_path)) != 0)
        return STATUS_OBJECT_PATH_INVALID;
    path = resolved_path;

    vfs_flags = disposition_to_vfs(disposition, access, options);

    /* pledge/unveil gate (user-originated calls only), BEFORE any side effect.
     * pledge_check_file returns STATUS_PLEDGE_VIOLATION -> the SSDT dispatcher
     * terminates the process after this handler returns; unveil_check returns
     * STATUS_ACCESS_DENIED (no termination). */
    if (pledge_user_mode()) {
        struct task *t = task_current();
        NTSTATUS pr = pledge_check_file(t, vfs_flags);
        if (pr != STATUS_SUCCESS)
            return pr;
        pr = unveil_check(t, path, unveil_perms_for_vfs(vfs_flags));
        if (pr != STATUS_SUCCESS)
            return pr;
    }

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
                /* Pipe direction gate: only the write-end may write.
                 * Mirror of the INT 0x80 SYS_WRITEHANDLE gate in
                 * ob_file_write. Codex quality 2026-04-21 H1. */
                if (fo->pipe_end != PIPE_WRITE)
                    goto write_bad_handle;
                int64_t written = pipe_write(fo->pipe_id, buf, (uint32_t)len);
                if (written < 0)
                    goto write_bad_handle;
                /* Same counters as the file path: a pipe write is a write. */
                if (written > 0)
                    task_acct_note_write_io(task_current(), (uint64_t)written);
                if (iosb) {
                    iosb->Status = STATUS_SUCCESS;
                    iosb->Information = (uint64_t)written;
                }
                return STATUS_SUCCESS;
            }

            if (fo->vfs_node) {
                /* VFS access gate: require VFS_O_WRITE in the open
                 * access mask. Same check as ob_file_write. */
                if (!(fo->access & VFS_O_WRITE))
                    goto write_bad_handle;
                int64_t written = vfs_write(fo->vfs_node,
                    (uint32_t)fo->offset, (uint32_t)len,
                    (const uint8_t *)buf);
                if (written < 0)
                    goto write_bad_handle;
                fo->offset += (uint64_t)written;
                if (written > 0)
                    task_acct_note_write_io(task_current(), (uint64_t)written);
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

    /* stdout: echo to serial + terminal. Mirrors the legacy INT 0x80
     * SYS_WRITE path in syscall.c exactly (single fetch of `c`, capture
     * pipeline first, raw serial fallback only when capture declines) so
     * a test binary reaching stdout through EITHER ABI gets the same
     * source-level framing -- a binary using only this SSDT path would
     * otherwise bypass the capture entirely. cap_ctx is a local on this
     * call's own stack; see test_usermode.h for why it is not on struct
     * task. */
    {
        struct utest_capture_ctx cap_ctx;
        test_usermode_capture_start(&cap_ctx);

        for (i = 0; i < len; i++) {
            char c = buf[i];

            if (c == '\0')
                break;
            if (terminal_is_open())
                terminal_putchar(c);
            if (!test_usermode_capture_byte(&cap_ctx, c))
                serial_putchar(c);
        }
        test_usermode_capture_end(&cap_ctx);
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
                /* Pipe direction gate: only the read-end may read.
                 * Mirror of the ob_file_read gate. Codex quality
                 * 2026-04-21 H1. */
                if (fo->pipe_end != PIPE_READ)
                    goto read_bad_handle;
                int64_t bytes = pipe_read(fo->pipe_id, dst, (uint32_t)len);
                if (bytes < 0)
                    goto read_bad_handle;
                /* Same counters as the file path: a pipe read is a read. */
                if (bytes > 0)
                    task_acct_note_read_io(task_current(), (uint64_t)bytes);
                if (iosb) {
                    iosb->Status = STATUS_SUCCESS;
                    iosb->Information = (uint64_t)bytes;
                }
                return STATUS_SUCCESS;
            }

            if (fo->vfs_node) {
                /* VFS access gate: require VFS_O_READ in the open
                 * access mask. Same check as ob_file_read. */
                if (!(fo->access & VFS_O_READ))
                    goto read_bad_handle;
                int64_t bytes = vfs_read(fo->vfs_node,
                    (uint32_t)fo->offset, (uint32_t)len,
                    (uint8_t *)dst);
                if (bytes < 0)
                    goto read_bad_handle;
                fo->offset += (uint64_t)bytes;
                if (bytes > 0)
                    task_acct_note_read_io(task_current(), (uint64_t)bytes);
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

/* NtTerminateProcess moved to nt_process.c */

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

/* NtWaitForSingleObject moved to nt_sync.c -- now handles all object types */

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
    uint64_t last_entry_end = 0;   /* actual end of the last entry (no trailing pad) */
    uint32_t entries_written = 0;
    uint8_t *prev_entry = (uint8_t *)0;

    if (!buf || buf_size == 0)
        return STATUS_INVALID_PARAMETER;

    /* Probe the output buffer + IO_STATUS_BLOCK FIRST -- before any branch
     * writes iosb -- so an invalid class/handle combined with a kernel-range or
     * unmapped iosb pointer is rejected with STATUS_ACCESS_VIOLATION rather than
     * driving a raw kernel write (matches the sibling handlers' probe pattern).
     * Fault-recoverable copy_to_user via a kernel bounce buffer is the systemic
     * usercopy follow-up. Every error exit below sets iosb {status, 0}. */
    {
        NTSTATUS ps = ProbeForWriteIfUser(buf, buf_size, 1);
        if (ps != STATUS_SUCCESS)
            return ps;
        if (iosb) {
            ps = ProbeForWriteIfUser(iosb, sizeof(IO_STATUS_BLOCK), 1);
            if (ps != STATUS_SUCCESS)
                return ps;
        }
    }

    /* Validate the info class up front (not inside the loop): an invalid class
     * on an empty directory must return STATUS_INVALID_INFO_CLASS -- not
     * STATUS_NO_MORE_FILES. */
    if (info_class != 0 &&
        info_class != FileDirectoryInformation &&
        info_class != FileBothDirectoryInformation &&
        info_class != FileIdBothDirectoryInformation) {
        if (iosb) { iosb->Status = STATUS_INVALID_INFO_CLASS; iosb->Information = 0; }
        return STATUS_INVALID_INFO_CLASS;
    }

    /* Resolve directory handle -> FILE_OBJECT -> vfs_node. Any nonzero handle
     * (INCLUDING the INVALID_HANDLE_VALUE sentinel (HANDLE)-1) MUST resolve to
     * a live File object; an invalid/closed/wrong-type handle returns
     * STATUS_INVALID_HANDLE (never silent C:\ enumeration), and a File object
     * whose node is not a directory returns STATUS_NOT_A_DIRECTORY. Only the
     * explicit no-handle value fh==0 uses the C:\ root legacy shim. */
    fo = (FILE_OBJECT *)0;
    if (fh != 0) {
        HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(
            &task_current()->handle_table, fh);
        OBJECT_HEADER *hdr;
        if (!entry || !entry->object) {
            if (iosb) { iosb->Status = STATUS_INVALID_HANDLE; iosb->Information = 0; }
            return STATUS_INVALID_HANDLE;
        }
        hdr = OB_HEADER_FROM_BODY(entry->object);
        if (hdr->type != ObpFileType) {
            if (iosb) { iosb->Status = STATUS_INVALID_HANDLE; iosb->Information = 0; }
            return STATUS_INVALID_HANDLE;
        }
        fo = (FILE_OBJECT *)entry->object;
        /* VFS type is a bit flag: a mounted drive root is VFS_DIRECTORY |
         * VFS_MOUNTPOINT, so test the DIRECTORY bit, not exact equality
         * (== would wrongly reject a real C:\ handle as not-a-directory). */
        if (!fo->vfs_node || !(fo->vfs_node->type & VFS_DIRECTORY)) {
            if (iosb) { iosb->Status = STATUS_NOT_A_DIRECTORY; iosb->Information = 0; }
            return STATUS_NOT_A_DIRECTORY;
        }
        /* Gate enumeration on list/read access, like NtReadFile gates reads on
         * VFS_O_READ. A directory handle opened write-only (GENERIC_WRITE with
         * no read) must not enumerate. Normal opens (GENERIC_READ /
         * FILE_LIST_DIRECTORY / default) set VFS_O_READ via disposition_to_vfs,
         * so this passes for legitimate enumeration. Inside the fh!=0 branch, so
         * fo is non-NULL (the fh==0 shim has no FILE_OBJECT). */
        if (!(fo->access & VFS_O_READ)) {
            if (iosb) { iosb->Status = STATUS_ACCESS_DENIED; iosb->Information = 0; }
            return STATUS_ACCESS_DENIED;
        }
        dir_node = fo->vfs_node;
    } else {
        /* Explicit no-handle legacy shape: C:\ root. This implicit open bypasses
         * NtCreateFile's unveil check, so gate the root enumeration on unveil 'r'
         * (a task unveiled to a child subtree must not enumerate the root). */
        if (pledge_user_mode() &&
            unveil_check(task_current(), "C:\\", UNVEIL_R) != STATUS_SUCCESS) {
            if (iosb) { iosb->Status = STATUS_ACCESS_DENIED; iosb->Information = 0; }
            return STATUS_ACCESS_DENIED;
        }
        dir_node = vfs_get_drive_root('C');
        if (!dir_node) {
            if (iosb) { iosb->Status = STATUS_OBJECT_PATH_NOT_FOUND; iosb->Information = 0; }
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
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

            /* Compute filename length in bytes (UTF-16). Capped at 255, the
             * Windows max path-component length; a disk-sourced name longer
             * than that is intentionally truncated to the component limit. */
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

            /* The ENTRY itself must fit; its trailing 8-byte pad is only needed
             * when another entry follows (the last entry keeps NextEntryOffset
             * 0 and no pad). Checking aligned_size here would wrongly reject an
             * exact-size single-entry / ReturnSingleEntry buffer. */
            if (offset + entry_size > buf_size)
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

                /* Zero the entry region. aligned_size <= remaining by the
                 * space check above; clamp defensively, then memset. */
                {
                    uint32_t clear = aligned_size;
                    if (offset + clear > buf_size)
                        clear = (uint32_t)(buf_size - offset);
                    memset(dst, 0, clear);
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
                last_entry_end = offset + entry_size;  /* unpadded end of this (so far last) entry */
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
        iosb->Information = last_entry_end;  /* actual bytes; last entry has no trailing pad */
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
#define SystemFirmwareTableInformation      76    /* GetSystemFirmwareTable / EnumSystemFirmwareTables */
/* SystemKernelConfigInformation (0x1000) is defined in nt/sysconfig_info.h. */

/* SYSTEM_FIRMWARE_TABLE_ACTION values per Win32 SDK winternl.h.  */
#define SystemFirmwareTable_Enumerate       0
#define SystemFirmwareTable_Get             1

/* SYSTEM_FIRMWARE_TABLE_PROVIDER values (4-byte LE-packed ASCII).
 * 'ACPI' = 0x49504341, 'FIRM' = 0x4D524946, 'RSMB' = 0x424D5352. */
#define FW_PROVIDER_ACPI                    0x49504341U
#define FW_PROVIDER_FIRM                    0x4D524946U
#define FW_PROVIDER_RSMB                    0x424D5352U

/* RSMB header per Microsoft "Raw SMBIOS firmware table provider" doc:
 * 4 bytes Used20CallingMethod | MajorVersion | MinorVersion | DmiRevision
 * + 4 bytes Length + raw SMBIOS table bytes. */
struct raw_smbios_data {
    uint8_t  used20_calling_method;
    uint8_t  smbios_major_version;
    uint8_t  smbios_minor_version;
    uint8_t  dmi_revision;
    uint32_t length;
    /* uint8_t smbios_table_data[]; */
} __attribute__((packed));

/* ---- NtQuerySystemInformation -------------------------------------------
 * SSDT 0x00D0 -- system-wide information queries.
 * a1 = info class, a2 = buffer, a3 = buffer size,
 * a4 = return length pointer.
 * ----------------------------------------------------------------------- */
/* SYSTEM_KERNEL_CONFIG_INFORMATION marshaller (Impossible OS extension): a
 * read-only summary of the immutable kernel_config_t snapshot plus the runtime
 * tunable / feature counts and the policy lock phase. Read-only -- no privilege
 * required. Non-static so the config syscall unit tests exercise the marshalling
 * directly (at CPL 0 the IfUser probes are no-ops and copy_to_user to a kernel
 * buffer works). The set path is owned by NtSetSystemInformation and gated on
 * the security reference monitor's SeSinglePrivilegeCheck + per-token lock. */
NTSTATUS nt_query_kernel_config_information(void *buffer, uint32_t buf_size,
                                            uint32_t *return_length)
{
    SYSTEM_KERNEL_CONFIG_INFORMATION info;
    const uint32_t need = (uint32_t)sizeof(info);

    const kernel_config_t *kc = kernel_config_get();
    if (!kc)
        return STATUS_UNSUCCESSFUL;   /* snapshot not published yet */

    /* Report the required size to a two-pass caller before the capacity check
     * (return_length is itself a user pointer). */
    if (return_length) {
        NTSTATUS pst = ProbeForWriteIfUser(return_length,
                                           (uint32_t)sizeof(uint32_t), 4);
        if (pst != STATUS_SUCCESS)
            return pst;
        if (copy_to_user(return_length, &need, (uint32_t)sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
    }
    if (!buffer || buf_size < need)
        return STATUS_INFO_LENGTH_MISMATCH;

    info.Version        = kc->version;
    info.Size           = kc->size;
    info.BootMode       = kc->boot_mode;
    info.SafeMode       = kc->safe_mode;
    info.SafeModeReason = kc->safe_mode_reason;
    info.DebugEnabled   = kc->debug_enabled;
    info.TestMode       = kc->test_mode;
    info.LockPhase      = (uint8_t)kernel_tunable_lock_phase_get();
    info.LockdownLevel  = (uint8_t)kernel_lockdown_level_get();
    info.Reserved[0]    = 0;
    info.BootReason      = kc->boot_reason;
    info.SelectionReason = kc->selection_reason;
    info.TunableCount    = kernel_tunable_count();
    info.FeatureCount    = kernel_feature_count();

    NTSTATUS pst = ProbeForWriteIfUser(buffer, need, 4);
    if (pst != STATUS_SUCCESS)
        return pst;
    if (copy_to_user(buffer, &info, need) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* SYSTEM_NOTIFICATION_INFORMATION marshaller (Impossible OS extension 0x1002): a
 * read-only Kernel Notification Facility diagnostics snapshot -- live state /
 * subscriber counts + cumulative publish / coalesced / security-denial + trace
 * drop/skip counters + KNF init health. All source values are best-effort
 * relaxed-atomic tallies or the subsystem-readiness oracle, so no lock is held.
 * Non-static so the KNF diagnostics unit test exercises the marshalling directly. */
NTSTATUS nt_query_notification_information(void *buffer, uint32_t buf_size,
                                          uint32_t *return_length)
{
    SYSTEM_NOTIFICATION_INFORMATION info;
    const uint32_t need = (uint32_t)sizeof(info);

    if (return_length) {
        NTSTATUS pst = ProbeForWriteIfUser(return_length,
                                           (uint32_t)sizeof(uint32_t), 4);
        if (pst != STATUS_SUCCESS)
            return pst;
        if (copy_to_user(return_length, &need, (uint32_t)sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
    }
    if (!buffer || buf_size < need)
        return STATUS_INFO_LENGTH_MISMATCH;

    memset(&info, 0, sizeof(info));
    info.Version = SYSTEM_NOTIFICATION_INFORMATION_VERSION;
    info.Size    = (uint16_t)sizeof(info);
    /* Three distinct health states. apply_result records BOTH DEGRADED and FATAL
     * in degraded_mask, so the degraded_mask bit alone cannot tell them apart --
     * READY (BOOT_OK/DEGRADED both set ready) is the discriminator. Not ready =>
     * fatal init (UNAVAILABLE); ready + degraded bit => partial (DEGRADED). */
    if (kernel_subsystem_ready(SUBSYS_KNF)) {
        info.Flags |= SYSTEM_NOTIFICATION_FLAG_READY;
        if (g_boot_info.degraded_mask & (1u << SUBSYS_KNF))
            info.Flags |= SYSTEM_NOTIFICATION_FLAG_DEGRADED;
    } else {
        info.Flags |= SYSTEM_NOTIFICATION_FLAG_UNAVAILABLE;
    }
    info.LiveStateCount  = knf_diag_live_state_count();
    info.SubscriberCount = knf_diag_subscriber_count();
    info.PublishCount    = knf_diag_publish_count();
    info.CoalescedCount  = knf_diag_coalesced_count();
    info.SecurityDenials = knf_diag_security_denial_count();
    info.DropsAtDispatch = knf_trace_drops_at_dispatch_count();
    info.TraceGuardSkips = knf_trace_skips_guard_count();

    NTSTATUS pst = ProbeForWriteIfUser(buffer, need, 4);
    if (pst != STATUS_SUCCESS)
        return pst;
    if (copy_to_user(buffer, &info, need) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* SYSTEM_RESOURCE_PRESSURE_INFORMATION marshaller (Impossible OS extension
 * 0x1003): a read-only PSI-shaped stall-telemetry snapshot -- per resource
 * domain, cumulative some/full stall time plus 10/60/300 s averages in permille,
 * each row carrying its own VALID flag.
 *
 * The VALID flag is why this class exists in this shape. All three stall seams
 * (scheduler, allocator, block I/O) are owned by other TODOs and unwired, so
 * every row reports VALID clear today. A reader must treat that as "not
 * measured" -- reporting the zeroed averages as "no pressure" would be a
 * fabrication, and it is exactly what a flagless PSI-style class forces a reader
 * to do. The snapshot source takes the aggregator lock per row and returns a
 * copy, so nothing is held while marshalling. Non-static so the stall telemetry
 * unit test exercises the marshalling directly. */
NTSTATUS nt_query_resource_pressure_information(void *buffer, uint32_t buf_size,
                                                uint32_t *return_length)
{
    SYSTEM_RESOURCE_PRESSURE_INFORMATION info;
    quota_stall_summary_t summary;
    const uint32_t need = (uint32_t)sizeof(info);

    if (return_length) {
        NTSTATUS pst = ProbeForWriteIfUser(return_length,
                                           (uint32_t)sizeof(uint32_t), 4);
        if (pst != STATUS_SUCCESS)
            return pst;
        if (copy_to_user(return_length, &need, (uint32_t)sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
    }
    if (!buffer || buf_size < need)
        return STATUS_INFO_LENGTH_MISMATCH;

    memset(&info, 0, sizeof(info));

    /* ONE coherent generation of the whole state. Reading the metadata and each
     * row through separate queries would let the 2 s aggregation land between
     * two of them, so user mode could receive a window count from after a fold
     * beside rows from before it -- a torn snapshot an unprivileged caller can
     * provoke just by querying in a loop. */
    if (quota_stall_snapshot_all(&summary) != 0)
        return STATUS_UNSUCCESSFUL;

    info.Version          = SYSTEM_RESOURCE_PRESSURE_INFORMATION_VERSION;
    info.Size             = (uint16_t)sizeof(info);
    info.DomainCount      = SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT;
    info.DomainSize       = (uint32_t)sizeof(SYSTEM_RESOURCE_PRESSURE_DOMAIN);
    info.UpdateIntervalNs = QUOTA_STALL_UPDATE_NS;
    info.LastUpdateNs     = summary.last_update_ns;
    info.WindowsClosed    = summary.windows_closed;
    /* The composite reports QUOTA_PRESSURE_UNKNOWN when no stall domain has been
     * measured. That sentinel is kernel-internal and deliberately does NOT cross
     * this boundary: SystemLevel keeps its published 0..3 space and the honesty
     * moves to a flag, so a reader that predates the flag decodes exactly what it
     * always did instead of meeting a 255 it has no case for. */
    if (quota_pressure_level_measured((quota_pressure_level_t)summary.system_level)) {
        info.SystemLevel = (uint32_t)summary.system_level;
        info.Flags |= SYSTEM_RESOURCE_PRESSURE_FLAG_SYSTEM_LEVEL_VALID;
    } else {
        info.SystemLevel = 0;   /* nothing measured; the flag above says so */
    }
    if (info.WindowsClosed != 0)
        info.Flags |= SYSTEM_RESOURCE_PRESSURE_FLAG_ARMED;

    for (uint32_t d = 0; d < SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT; d++) {
        SYSTEM_RESOURCE_PRESSURE_DOMAIN *row = &info.Domains[d];
        const quota_stall_snapshot_t s = summary.domain[d];

        row->Domain = d;

        if (s.valid)
            row->Flags |= SYSTEM_RESOURCE_PRESSURE_FLAG_VALID;
        if (s.instrumented)
            row->Flags |= SYSTEM_RESOURCE_PRESSURE_FLAG_INSTRUMENTED;
        if (s.full_undefined)
            row->Flags |= SYSTEM_RESOURCE_PRESSURE_FLAG_FULL_UNDEFINED;

        row->SomeTotalNs = s.some_total_ns;
        row->FullTotalNs = s.full_total_ns;
        row->SomeAvg10   = s.some_avg[QUOTA_STALL_AVG10];
        row->SomeAvg60   = s.some_avg[QUOTA_STALL_AVG60];
        row->SomeAvg300  = s.some_avg[QUOTA_STALL_AVG300];
        row->FullAvg10   = s.full_avg[QUOTA_STALL_AVG10];
        row->FullAvg60   = s.full_avg[QUOTA_STALL_AVG60];
        row->FullAvg300  = s.full_avg[QUOTA_STALL_AVG300];
        row->Level       = s.level;
    }

    NTSTATUS pst = ProbeForWriteIfUser(buffer, need, 4);
    if (pst != STATUS_SUCCESS)
        return pst;
    if (copy_to_user(buffer, &info, need) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* SYSTEM_NLS_INFORMATION marshaller (Impossible OS extension 0x1001): a
 * read-only snapshot of the active code-page / locale / UI-language policy and
 * the NLS/collation version. All source values are published (boot-cached ACP/
 * OEMCP, atomic locale caches, immutable NLS version) so no lock is held.
 * Non-static so the NLS syscall unit tests exercise the marshalling directly. */
NTSTATUS nt_query_nls_information(void *buffer, uint32_t buf_size,
                                 uint32_t *return_length)
{
    SYSTEM_NLS_INFORMATION info;
    const uint32_t need = (uint32_t)sizeof(info);

    /* Report the required size to a two-pass caller before the capacity check. */
    if (return_length) {
        NTSTATUS pst = ProbeForWriteIfUser(return_length,
                                           (uint32_t)sizeof(uint32_t), 4);
        if (pst != STATUS_SUCCESS)
            return pst;
        if (copy_to_user(return_length, &need, (uint32_t)sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
    }
    if (!buffer || buf_size < need)
        return STATUS_INFO_LENGTH_MISMATCH;

    /* Probe the output buffer before snapshotting so an invalid-but-sized buffer
     * fails without doing the field reads. */
    NTSTATUS pst = ProbeForWriteIfUser(buffer, need, 4);
    if (pst != STATUS_SUCCESS)
        return pst;

    info.AnsiCodePage    = nls_cp_get_acp();
    info.OemCodePage     = nls_cp_get_oemcp();
    info.SystemLcid      = nls_locale_get_system();
    info.UserLcid        = nls_locale_get_user();
    info.NlsVersion      = nls_get_version();
    /* Lock-free active UI language; the SAME source the MUI-query handlers read,
     * so all the NLS query surfaces agree on the UI language (the locked
     * nt_locale store is the settable NtQueryDefaultUILanguage value, a
     * separate concept). */
    info.UiLangId        = nls_locale_get_ui_language();
    info.InstallUiLangId = nt_locale_get_install_ui_language();

    if (copy_to_user(buffer, &info, need) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

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

        /* System performance counters require SeSystemProfilePrivilege for a
         * UserMode caller (KernelMode is trusted). */
        if (!SeSinglePrivilegeCheck(&SeSystemProfilePrivilege, ssdt_previous_mode()))
            return STATUS_PRIVILEGE_NOT_HELD;

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
    case SystemFirmwareTableInformation: {
        /* SYSTEM_FIRMWARE_TABLE_INFORMATION (Win32 SDK winternl.h):
         *   ULONG ProviderSignature;       caller input ('ACPI'/'RSMB'/'FIRM')
         *   SYSTEM_FIRMWARE_TABLE_ACTION Action;  0=enumerate, 1=get
         *   ULONG TableID;                 caller input (signature for get)
         *   ULONG TableBufferLength;       in: capacity, out: bytes-needed
         *   UCHAR TableBuffer[ANYSIZE_ARRAY];
         *
         * User-mode callers MUST be probed; previous-mode-aware
         * copy_from_user / copy_to_user wrappers are mandatory for every
         * read/write across the buffer.  Existing nt_section.c is the
         * reference pattern.  Every length addition uses
         * __builtin_add_overflow so a hostile firmware-reported size
         * cannot wrap and bypass the capacity check.
         */
        struct fwti_hdr {
            uint32_t provider_signature;
            uint32_t action;
            uint32_t table_id;
            uint32_t table_buffer_length;
        } __attribute__((packed));
        const uint32_t fwti_hdr_size = (uint32_t)sizeof(struct fwti_hdr);

        if (!buffer || buf_size < fwti_hdr_size)
            return STATUS_BUFFER_TOO_SMALL;

        /* F1: probe + copy header into a kernel local. */
        NTSTATUS pst = ProbeForReadIfUser(buffer, buf_size, 4);
        if (pst != STATUS_SUCCESS)
            return pst;
        struct fwti_hdr local;
        if (copy_from_user(&local, buffer, fwti_hdr_size) != 0)
            return STATUS_ACCESS_VIOLATION;

        uint32_t provider = local.provider_signature;
        uint32_t action   = local.action;
        uint32_t table_id = local.table_id;
        uint32_t out_cap  = local.table_buffer_length;

        if (out_cap > (buf_size - fwti_hdr_size))
            return STATUS_INFO_LENGTH_MISMATCH;
        uint8_t *out_buf = (uint8_t *)buffer + fwti_hdr_size;
        /* F1: probe the entire writable area we may touch (header
         * write-back + table buffer), so all subsequent copy_to_user
         * calls operate on a region we have already validated. */
        pst = ProbeForWriteIfUser(buffer, fwti_hdr_size + out_cap, 4);
        if (pst != STATUS_SUCCESS)
            return pst;
        /* F3 (round-2 re-adversarial 2026-04-29): return_length is
         * also a user pointer; probe it once here so each write below
         * goes through copy_to_user safely. NULL is a valid Windows
         * caller convention (caller does not need the returned size). */
        if (return_length) {
            pst = ProbeForWriteIfUser(return_length,
                                       (uint32_t)sizeof(uint32_t), 4);
            if (pst != STATUS_SUCCESS)
                return pst;
        }

        if (provider == FW_PROVIDER_ACPI) {
            if (action == SystemFirmwareTable_Enumerate) {
                uint32_t total = 0;
                if (!acpi_enumerate_signatures((uint32_t *)0, 0, &total))
                    return STATUS_NOT_FOUND;
                /* F2: total * sizeof(uint32_t) is bounded by
                 * ACPI_ROOT_ENTRY_MAX (1024) * 4 = 4096; cannot wrap. */
                uint32_t needed_bytes = total * (uint32_t)sizeof(uint32_t);
                /* Write the bytes-needed back to user before length check
                 * so two-pass callers see the right value. */
                if (copy_to_user(&((struct fwti_hdr *)buffer)->table_buffer_length,
                                  &needed_bytes, (uint32_t)sizeof(uint32_t)) != 0)
                    return STATUS_ACCESS_VIOLATION;
                uint32_t total_out;
                if (__builtin_add_overflow(fwti_hdr_size, needed_bytes,
                                            &total_out))
                    return STATUS_INVALID_PARAMETER;
                if (return_length) {
                    uint32_t _rl_tmp = (uint32_t)(total_out);
                    if (copy_to_user(return_length, &_rl_tmp,
                                      (uint32_t)sizeof(uint32_t)) != 0)
                        return STATUS_ACCESS_VIOLATION;
                }
                if (out_cap < needed_bytes)
                    return STATUS_BUFFER_TOO_SMALL;
                uint32_t fit = needed_bytes / (uint32_t)sizeof(uint32_t);
                /* F4 (round-2 re-adversarial 2026-04-29): zero-initialize
                 * the local sigs[] AND use the second-pass total to
                 * derive the actual byte count. If the firmware view
                 * changes between the count probe and the fill pass,
                 * we must NOT copy_to_user the residual stack bytes
                 * past the actual write count -- doing so leaks
                 * uninitialized kernel stack to user-mode. */
                #define FW_SIG_LOCAL_MAX 1024U
                uint32_t sigs[FW_SIG_LOCAL_MAX];
                uint32_t z;
                for (z = 0; z < FW_SIG_LOCAL_MAX; z++) sigs[z] = 0;
                if (fit > FW_SIG_LOCAL_MAX) fit = FW_SIG_LOCAL_MAX;
                uint32_t actual = 0;
                acpi_enumerate_signatures(sigs, fit, &actual);
                /* `actual` is the (possibly-changed) total found this
                 * pass; the bytes we may copy out are bounded by
                 * min(actual, fit). */
                uint32_t copy_count = (actual < fit) ? actual : fit;
                uint32_t copy_bytes = copy_count * (uint32_t)sizeof(uint32_t);
                #undef FW_SIG_LOCAL_MAX
                if (copy_count > 0 &&
                    copy_to_user(out_buf, sigs, copy_bytes) != 0)
                    return STATUS_ACCESS_VIOLATION;
                return STATUS_SUCCESS;
            } else if (action == SystemFirmwareTable_Get) {
                const uint8_t *tbl_addr = (const uint8_t *)0;
                uint32_t tbl_size = 0;
                if (!acpi_get_raw_table(table_id, &tbl_addr, &tbl_size))
                    return STATUS_NOT_FOUND;
                if (copy_to_user(&((struct fwti_hdr *)buffer)->table_buffer_length,
                                  &tbl_size, (uint32_t)sizeof(uint32_t)) != 0)
                    return STATUS_ACCESS_VIOLATION;
                uint32_t total_out;
                if (__builtin_add_overflow(fwti_hdr_size, tbl_size,
                                            &total_out))
                    return STATUS_INVALID_PARAMETER;
                if (return_length) {
                    uint32_t _rl_tmp = (uint32_t)(total_out);
                    if (copy_to_user(return_length, &_rl_tmp,
                                      (uint32_t)sizeof(uint32_t)) != 0)
                        return STATUS_ACCESS_VIOLATION;
                }
                if (out_cap < tbl_size)
                    return STATUS_BUFFER_TOO_SMALL;
                if (copy_to_user(out_buf, tbl_addr, tbl_size) != 0)
                    return STATUS_ACCESS_VIOLATION;
                return STATUS_SUCCESS;
            }
            return STATUS_INVALID_PARAMETER;
        }

        if (provider == FW_PROVIDER_RSMB) {
            const uint8_t *raw_addr = (const uint8_t *)0;
            uint32_t raw_size = 0;
            if (!smbios_get_raw_table(&raw_addr, &raw_size))
                return STATUS_NOT_FOUND;
            const struct smbios_system_info *si = smbios_get_info();
            uint32_t hdr_size = (uint32_t)sizeof(struct raw_smbios_data);
            /* F2: total = hdr_size + raw_size MUST be overflow-checked.
             * raw_size is firmware-sourced uint32; an adversarial value
             * near UINT32_MAX would wrap a plain add, letting the
             * downstream memcpy(raw_size) copy gigabytes despite the
             * tiny capacity check passing. */
            uint32_t total;
            if (__builtin_add_overflow(hdr_size, raw_size, &total))
                return STATUS_INVALID_PARAMETER;

            if (action == SystemFirmwareTable_Enumerate) {
                uint32_t needed = (uint32_t)sizeof(uint32_t);
                if (copy_to_user(&((struct fwti_hdr *)buffer)->table_buffer_length,
                                  &needed, (uint32_t)sizeof(uint32_t)) != 0)
                    return STATUS_ACCESS_VIOLATION;
                if (return_length) {
                    uint32_t _rl_tmp = (uint32_t)(fwti_hdr_size + needed);
                    if (copy_to_user(return_length, &_rl_tmp,
                                      (uint32_t)sizeof(uint32_t)) != 0)
                        return STATUS_ACCESS_VIOLATION;
                }
                if (out_cap < needed)
                    return STATUS_BUFFER_TOO_SMALL;
                uint32_t one_zero = 0;
                if (copy_to_user(out_buf, &one_zero, needed) != 0)
                    return STATUS_ACCESS_VIOLATION;
                return STATUS_SUCCESS;
            } else if (action == SystemFirmwareTable_Get) {
                if (copy_to_user(&((struct fwti_hdr *)buffer)->table_buffer_length,
                                  &total, (uint32_t)sizeof(uint32_t)) != 0)
                    return STATUS_ACCESS_VIOLATION;
                uint32_t total_out;
                if (__builtin_add_overflow(fwti_hdr_size, total, &total_out))
                    return STATUS_INVALID_PARAMETER;
                if (return_length) {
                    uint32_t _rl_tmp = (uint32_t)(total_out);
                    if (copy_to_user(return_length, &_rl_tmp,
                                      (uint32_t)sizeof(uint32_t)) != 0)
                        return STATUS_ACCESS_VIOLATION;
                }
                if (out_cap < total)
                    return STATUS_BUFFER_TOO_SMALL;
                struct raw_smbios_data rsd_local;
                rsd_local.used20_calling_method = 0;
                rsd_local.smbios_major_version = si ? si->smbios_major : 0;
                rsd_local.smbios_minor_version = si ? si->smbios_minor : 0;
                rsd_local.dmi_revision = 0;
                rsd_local.length = raw_size;
                if (copy_to_user(out_buf, &rsd_local, hdr_size) != 0)
                    return STATUS_ACCESS_VIOLATION;
                if (copy_to_user(out_buf + hdr_size, raw_addr, raw_size) != 0)
                    return STATUS_ACCESS_VIOLATION;
                return STATUS_SUCCESS;
            }
            return STATUS_INVALID_PARAMETER;
        }

        if (provider == FW_PROVIDER_FIRM) {
            /* Legacy BIOS shadow region (0xC0000-0xFFFFF). Impossible OS
             * is UEFI-only; the bootloader does not preserve the BIOS
             * shadow segment. Matches Linux's posture on UEFI-only
             * systems. SCOPE-GAP-ALLOWED: legacy provider intentionally
             * unsupported on a UEFI-only kernel. */
            return STATUS_NOT_FOUND;
        }

        return STATUS_INVALID_PARAMETER;
    }
    case SystemKernelConfigInformation:
        return nt_query_kernel_config_information(buffer, buf_size, return_length);
    case SystemNlsInformation:
        return nt_query_nls_information(buffer, buf_size, return_length);
    case SystemNotificationInformation:
        return nt_query_notification_information(buffer, buf_size, return_length);
    case SystemResourcePressureInformation:
        return nt_query_resource_pressure_information(buffer, buf_size, return_length);
    default:
        /* SCOPE-GAP-ALLOWED: NT API contract default for unrecognized
         * SystemInformationClass values; Windows ntoskrnl returns the
         * same. Adding new classes is per-class work, not a stub gap. */
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

    /* Shutting down / rebooting requires SeShutdownPrivilege (KernelMode/Zw is
     * trusted). NtReboot is the ShutdownReboot action of this same syscall. */
    if (!SeSinglePrivilegeCheck(&SeShutdownPrivilege, ssdt_previous_mode()))
        return STATUS_PRIVILEGE_NOT_HELD;

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
    /* File I/O */
    ssdt_register(SSDT_NtCreateFile,            (SSDT_HANDLER)NtCreateFile_handler);
    ssdt_register(SSDT_NtOpenFile,              (SSDT_HANDLER)NtOpenFile_handler);
    ssdt_register(SSDT_NtReadFile,              (SSDT_HANDLER)NtReadFile);
    ssdt_register(SSDT_NtWriteFile,             (SSDT_HANDLER)NtWriteFile);
    ssdt_register(SSDT_NtQueryDirectoryFile,    (SSDT_HANDLER)NtQueryDirectoryFile);
    ssdt_register(SSDT_NtCreateNamedPipeFile,   (SSDT_HANDLER)NtCreateNamedPipeFile);

    /* Core object operations (NtWaitForSingleObject moved to nt_sync.c) */
    ssdt_register(SSDT_NtClose,                 (SSDT_HANDLER)Nt_Close);

    /* Process and thread (NtTerminateProcess moved to nt_process.c) */
    ssdt_register(SSDT_NtYieldExecution,        (SSDT_HANDLER)NtYieldExecution);

    /* System information */
    ssdt_register(SSDT_NtQuerySystemInformation, (SSDT_HANDLER)NtQuerySystemInformation);
    ssdt_register(SSDT_NtSetSystemInformation,   (SSDT_HANDLER)NtSetSystemInformation);

    /* Shutdown */
    ssdt_register(SSDT_NtShutdownSystem,        (SSDT_HANDLER)NtShutdownSystem);

    /* File metadata, device control, I/O completion (S13) */
    nt_file_register_ssdt();

    klog(LOG_INFO, "nt", "NT syscall: 31 NtXxx handlers registered");
}
