/* ============================================================================
 * nt_syscall.c -- NT native syscall wrappers (SSDT migration layer)
 *
 * Wraps the existing SYS_* implementations in NtXxx functions with proper
 * NTSTATUS returns and SSDT_HANDLER signatures. Each function is registered
 * at the SSDT index defined in service_numbers.h.
 *
 * This is the §5 migration: existing logic, new names and return types.
 * The INT 0x80 handler continues to work for backward compatibility.
 * ============================================================================ */

#include "kernel/nt/nt_syscall.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
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
#include "kernel/ob/ob_section.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/serial.h"
#include "desktop/terminal.h"

/* ---- NtWriteFile --------------------------------------------------------
 * SSDT 0x0013 -- wraps SYS_WRITE logic.
 * a1 = HANDLE (fd), a2 = IO_STATUS_BLOCK* (may be NULL for legacy),
 * a3 = buffer, a4 = length.
 * Returns NTSTATUS. Bytes written stored in IOSB.Information if provided.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtWriteFile(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fd = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    const char *buf = (const char *)a3;
    uint64_t len = a4;
    uint64_t i;

    (void)a5; (void)a6;

    if (!buf || len == 0) {
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    /* Handle-based file/pipe write */
    if (fd != (HANDLE)STDOUT_FD) {
        int64_t written = ob_file_write(&task_current()->handle_table,
                                        fd, buf, (uint32_t)len);
        if (written < 0) {
            if (iosb) {
                iosb->Status = STATUS_INVALID_HANDLE;
                iosb->Information = 0;
            }
            return STATUS_INVALID_HANDLE;
        }
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = (uint64_t)written;
        }
        return STATUS_SUCCESS;
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
 * SSDT 0x0012 -- wraps SYS_READ logic.
 * a1 = HANDLE (fd), a2 = IO_STATUS_BLOCK*, a3 = buffer, a4 = length.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtReadFile(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE fd = (HANDLE)(int32_t)a1;
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    char *dst = (char *)a3;
    uint64_t len = a4;
    uint64_t i;
    char c;

    (void)a5; (void)a6;

    if (!dst || len == 0) {
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = 0;
        }
        return STATUS_SUCCESS;
    }

    /* Handle-based file/pipe read */
    if (fd != (HANDLE)STDIN_FD) {
        int64_t bytes = ob_file_read(&task_current()->handle_table,
                                     fd, dst, (uint32_t)len);
        if (bytes < 0) {
            if (iosb) {
                iosb->Status = STATUS_INVALID_HANDLE;
                iosb->Information = 0;
            }
            return STATUS_INVALID_HANDLE;
        }
        if (iosb) {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = (uint64_t)bytes;
        }
        return STATUS_SUCCESS;
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

/* ---- NtTerminateProcess -------------------------------------------------
 * SSDT 0x0033 -- wraps SYS_EXIT and SYS_KILL logic.
 * a1 = HANDLE (CURRENT_PROCESS or target PID), a2 = NTSTATUS exit code.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtTerminateProcess(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    NTSTATUS exit_code = (NTSTATUS)(int32_t)a2;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (handle == CURRENT_PROCESS || handle == 0) {
        /* Terminate self */
        task_exit(exit_code);
        /* task_exit does not return */
        return STATUS_SUCCESS;
    }

    /* Terminate another process by PID (handle is PID for now) */
    {
        uint32_t pid = (uint32_t)(uint64_t)handle;
        struct task *t;

        if (pid == 0 || pid >= task_count())
            return STATUS_INVALID_HANDLE;

        t = task_get_by_pid(pid);
        if (!t || t->state == TASK_DEAD)
            return STATUS_INVALID_HANDLE;

        t->state = TASK_DEAD;
        t->exit_status = (int32_t)exit_code;
        klog(LOG_DEBUG, "nt", "NtTerminateProcess: PID %u terminated (0x%x)",
             (uint64_t)pid, (uint64_t)(uint32_t)exit_code);
        return STATUS_SUCCESS;
    }
}

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

/* ---- NtWaitForSingleObject ----------------------------------------------
 * SSDT 0x0006 -- wraps SYS_WAITPID.
 * a1 = HANDLE (child PID for now), a2 = alertable (ignored),
 * a3 = timeout pointer (NULL = infinite).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtWaitForSingleObject(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE handle = (HANDLE)(int32_t)a1;
    int32_t result;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Currently only supports waiting on a child process (PID handle) */
    result = task_waitpid((uint32_t)(uint64_t)handle);
    if (result < 0)
        return STATUS_INVALID_HANDLE;
    return STATUS_SUCCESS;
}

/* ---- NtQueryDirectoryFile -----------------------------------------------
 * SSDT 0x0017 -- wraps SYS_READDIR.
 * a1 = HANDLE (directory; ignored, uses C:\ root for now),
 * a2 = IO_STATUS_BLOCK*, a3 = buffer, a4 = buffer size,
 * a5 = info class (ignored), a6 = index.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtQueryDirectoryFile(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    IO_STATUS_BLOCK *iosb = (IO_STATUS_BLOCK *)a2;
    char *buf = (char *)a3;
    uint64_t buf_size = a4;
    uint32_t index = (uint32_t)a6;
    struct vfs_node *root;
    struct vfs_dirent *entry;
    uint64_t i;

    (void)a1; (void)a5;

    if (!buf || buf_size == 0)
        return STATUS_INVALID_PARAMETER;

    root = vfs_get_drive_root('C');
    if (!root)
        return STATUS_OBJECT_PATH_NOT_FOUND;

    entry = vfs_readdir(root, index);
    if (!entry) {
        if (iosb) {
            iosb->Status = STATUS_NO_MORE_FILES;
            iosb->Information = 0;
        }
        return STATUS_NO_MORE_FILES;
    }

    for (i = 0; i < buf_size - 1 && entry->name[i]; i++)
        buf[i] = entry->name[i];
    buf[i] = '\0';

    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = i;
    }
    return STATUS_SUCCESS;
}

/* ---- SystemProcessInformation class ID for NtQuerySystemInformation ---- */
#define SystemProcessInformation        5
#define SystemTimeOfDayInformation      3

/* ---- NtQuerySystemInformation -------------------------------------------
 * SSDT 0x00D0 -- wraps SYS_GETPROCS and SYS_UPTIME.
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
    case SystemProcessInformation: {
        /* Process info structure -- matches user/include/syscall.h */
        struct {
            uint32_t pid;
            uint32_t state;
            char     name[32];
        } *out = buffer;
        uint32_t count = task_count();
        uint32_t max = buf_size / 40;  /* sizeof(proc_info) = 40 */
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
    case SystemTimeOfDayInformation: {
        /* Return uptime in seconds in the buffer */
        uint64_t *out = (uint64_t *)buffer;
        if (!buffer || buf_size < sizeof(uint64_t))
            return STATUS_BUFFER_TOO_SMALL;
        *out = uptime();
        if (return_length)
            *return_length = (uint32_t)sizeof(uint64_t);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
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

/* ---- NtCreateSection ----------------------------------------------------
 * SSDT 0x005C -- wraps SYS_SHMEM_CREATE.
 * a1 = HANDLE* out, a2 = ACCESS_MASK, a3 = OBJECT_ATTRIBUTES* (name),
 * a4 = size.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateSection(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    uint32_t access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    uint32_t size = (uint32_t)a4;
    const char *name = (const char *)0;
    HANDLE h;

    (void)a5; (void)a6;

    if (!out_handle || size == 0)
        return STATUS_INVALID_PARAMETER;

    /* Extract name from OBJECT_ATTRIBUTES if provided */
    if (oa && oa->ObjectName) {
        /* UNICODE_STRING.Buffer is a char* in our kernel */
        name = (const char *)oa->ObjectName->Buffer;
    }

    h = ObCreateSection(&task_current()->handle_table, size, access, name);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- NtMapViewOfSection -------------------------------------------------
 * SSDT 0x005E -- wraps SYS_SHMEM_MAP.
 * a1 = HANDLE (section), a2 = HANDLE (process, ignored for now),
 * a3 = uintptr_t* base address output.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtMapViewOfSection(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE section_handle = (HANDLE)(int32_t)a1;
    uintptr_t *base_out = (uintptr_t *)a3;
    uintptr_t addr;

    (void)a2; (void)a4; (void)a5; (void)a6;

    if (!base_out)
        return STATUS_INVALID_PARAMETER;

    addr = ObMapViewOfSection(&task_current()->handle_table, section_handle);
    if (addr == 0)
        return STATUS_INVALID_HANDLE;

    *base_out = addr;
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
    ssdt_register(SSDT_NtReadFile,              (SSDT_HANDLER)NtReadFile);
    ssdt_register(SSDT_NtWriteFile,             (SSDT_HANDLER)NtWriteFile);
    ssdt_register(SSDT_NtQueryDirectoryFile,    (SSDT_HANDLER)NtQueryDirectoryFile);
    ssdt_register(SSDT_NtCreateNamedPipeFile,   (SSDT_HANDLER)NtCreateNamedPipeFile);

    /* Core object operations */
    ssdt_register(SSDT_NtClose,                 (SSDT_HANDLER)Nt_Close);
    ssdt_register(SSDT_NtWaitForSingleObject,   (SSDT_HANDLER)NtWaitForSingleObject);

    /* Process and thread */
    ssdt_register(SSDT_NtTerminateProcess,      (SSDT_HANDLER)NtTerminateProcess);
    ssdt_register(SSDT_NtYieldExecution,        (SSDT_HANDLER)NtYieldExecution);

    /* System information */
    ssdt_register(SSDT_NtQuerySystemInformation, (SSDT_HANDLER)NtQuerySystemInformation);

    /* Shutdown */
    ssdt_register(SSDT_NtShutdownSystem,        (SSDT_HANDLER)NtShutdownSystem);

    /* Memory sections */
    ssdt_register(SSDT_NtCreateSection,         (SSDT_HANDLER)NtCreateSection);
    ssdt_register(SSDT_NtMapViewOfSection,      (SSDT_HANDLER)NtMapViewOfSection);

    klog(LOG_INFO, "nt", "NT syscall migration: 12 NtXxx handlers registered in SSDT");
}
