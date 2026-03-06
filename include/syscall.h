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
#define SYS_WRITE   1   /* sys_write(fd, buf, len) → bytes written */
#define SYS_READ    2   /* sys_read(fd, buf, len)  → bytes read */
#define SYS_EXIT    3   /* sys_exit(code)           → no return */
#define SYS_YIELD   4   /* sys_yield()              → 0 */
#define SYS_FORK    5   /* sys_fork()               → child PID (parent) / 0 (child) */
#define SYS_EXEC    6   /* sys_exec(data, size)     → 0 on success, -1 on error */
#define SYS_WAITPID 7   /* sys_waitpid(child_pid)   → exit status */

/* File descriptors */
#define STDOUT_FD   1
#define STDIN_FD    0

/* Initialize the syscall handler (registers INT 0x80) */
void syscall_init(void);
