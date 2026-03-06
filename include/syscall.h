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

/* File descriptors */
#define STDOUT_FD   1
#define STDIN_FD    0

/* Initialize the syscall handler (registers INT 0x80) */
void syscall_init(void);
