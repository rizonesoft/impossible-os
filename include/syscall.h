/* ============================================================================
 * syscall.h — System call interface
 *
 * User-mode programs invoke system calls via INT 0x80.
 * Syscall number in RAX, arguments in RDI, RSI, RDX.
 * Return value in RAX.
 * ============================================================================ */

#pragma once

#include "types.h"

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

/* File descriptors */
#define STDOUT_FD   1
#define STDIN_FD    0

/* Initialize the syscall handler (registers INT 0x80) */
void syscall_init(void);
