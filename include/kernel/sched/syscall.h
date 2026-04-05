/* ============================================================================
 * syscall.h -- System call interface
 *
 * User-mode programs invoke system calls via INT 0x80.
 * Syscall number in RAX, arguments in RDI, RSI, RDX.
 * Return value in RAX.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Syscall numbers */
#define SYS_WRITE    1   /* sys_write(fd, buf, len) → bytes written */
#define SYS_READ     2   /* sys_read(fd, buf, len)  → bytes read */
#define SYS_EXIT     3   /* sys_exit(code)           → no return */
#define SYS_YIELD    4   /* sys_yield()              → 0 */
#define SYS_FORK     5   /* sys_fork()               → child PID / 0 */
#define SYS_EXEC     6   /* sys_exec(name, len)      → 0 / -1 */
#define SYS_WAITPID  7   /* sys_waitpid(child_pid)   → exit status */
#define SYS_READFILE 8   /* sys_readfile(name, buf, bufsize) → bytes read */
#define SYS_READDIR  9   /* sys_readdir(name_buf, bufsize, index) → 0=ok / -1 */
#define SYS_GETPROCS 10  /* sys_getprocs(buf, bufsize) → num processes */
#define SYS_KILL     11  /* sys_kill(pid)             → 0 / -1 */
#define SYS_UPTIME   12  /* sys_uptime()              → seconds */
#define SYS_REBOOT   13  /* sys_reboot()              → no return */
#define SYS_SHUTDOWN 14  /* sys_shutdown()             → no return */
#define SYS_PING     15  /* sys_ping(ip_addr)           → 0 / -1 */
#define SYS_NETINFO  16  /* sys_netinfo(buf, size)       → 0 */
#define SYS_LOG      17  /* sys_log(level, msg, len)     → 0 / -1 */
#define SYS_PIPE     33  /* sys_pipe(fds)                → 0 / -1 */
#define SYS_SIGNAL   34  /* sys_signal(sig, handler)      → old handler */
#define SYS_SHMEM_CREATE 35 /* sys_shmem_create(name, size) → id / -1 */
#define SYS_SHMEM_MAP    36 /* sys_shmem_map(id)            → ptr / 0 */
#define SYS_MMAP         37 /* sys_mmap(addr, len, prot, flags, fd, off) → ptr */
#define SYS_MUNMAP       38 /* sys_munmap(addr, len)         → 0 / -1 */
#define SYS_OPENFILE     39 /* sys_openfile(path, access)    → HANDLE / -1 */
#define SYS_CLOSEHANDLE  40 /* sys_closehandle(handle)       → 0 / -1 */
#define SYS_READHANDLE   41 /* sys_readhandle(handle, buf, size) → bytes / -1 */
#define SYS_OPENDIROBJ   42 /* NtOpenDirectoryObject(name, access, &handle) → 0 / -1 */
#define SYS_QUERYDIROBJ  43 /* NtQueryDirectoryObject(handle, buf, count, &ctx, &ret) → 0 / -1 */

/* --- SSDT index aliases for transition ---
 * These map existing SYS_* names to their SSDT NtXxx equivalents.
 * Use with INT 0x2E or SYSCALL instruction (not INT 0x80).
 * INT 0x80 continues to use the legacy numbers above. */
#define SYS_NT_WRITE        0x0013  /* SSDT_NtWriteFile */
#define SYS_NT_READ         0x0012  /* SSDT_NtReadFile */
#define SYS_NT_EXIT         0x0033  /* SSDT_NtTerminateProcess */
#define SYS_NT_YIELD        0x0044  /* SSDT_NtYieldExecution */
#define SYS_NT_WAITPID      0x0006  /* SSDT_NtWaitForSingleObject */
#define SYS_NT_READDIR      0x0017  /* SSDT_NtQueryDirectoryFile */
#define SYS_NT_GETPROCS     0x00D0  /* SSDT_NtQuerySystemInformation */
#define SYS_NT_SHUTDOWN     0x00D7  /* SSDT_NtShutdownSystem */
#define SYS_NT_PIPE         0x001B  /* SSDT_NtCreateNamedPipeFile */
#define SYS_NT_SHMEM_CREATE 0x005C  /* SSDT_NtCreateSection */
#define SYS_NT_SHMEM_MAP    0x005E  /* SSDT_NtMapViewOfSection */
#define SYS_NT_CLOSE        0x0000  /* SSDT_NtClose */

/* File descriptors */
#define STDOUT_FD   1
#define STDIN_FD    0

/* Initialize the syscall handler (registers INT 0x80) */
void syscall_init(void);
