/* ============================================================================
 * syscall.h — Userland system call wrappers
 *
 * Inline INT 0x80 wrappers matching the kernel ABI:
 *   RAX = syscall number
 *   RDI = arg1, RSI = arg2, RDX = arg3
 *   Return value in RAX
 * ============================================================================ */

#pragma once

#include "types.h"

/* Syscall numbers (must match kernel syscall.h) */
#define SYS_WRITE    1
#define SYS_READ     2
#define SYS_EXIT     3
#define SYS_YIELD    4
#define SYS_FORK     5
#define SYS_EXEC     6
#define SYS_WAITPID  7
#define SYS_READFILE 8
#define SYS_READDIR  9
#define SYS_GETPROCS 10
#define SYS_KILL     11
#define SYS_UPTIME   12
#define SYS_REBOOT   13
#define SYS_SHUTDOWN 14

/* Task states (must match kernel task.h) */
#define TASK_READY    0
#define TASK_RUNNING  1
#define TASK_DEAD     2
#define TASK_WAITING  3

/* Process info (must match kernel syscall.c) */
struct proc_info {
    unsigned int pid;
    unsigned int state;
    char         name[32];
};

/* File descriptors */
#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

/* --- Inline syscall wrappers --- */

static inline long syscall0(long nr)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall1(long nr, long a1)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall2(long nr, long a1, long a2)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* --- Convenience wrappers --- */

static inline long sys_write(int fd, const void *buf, size_t len)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

static inline long sys_read(int fd, void *buf, size_t len)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

static inline void sys_exit(int code)
{
    syscall1(SYS_EXIT, code);
    for (;;) ;
}

static inline void sys_yield(void)
{
    syscall0(SYS_YIELD);
}

static inline long sys_fork(void)
{
    return syscall0(SYS_FORK);
}

static inline long sys_exec(const char *path, size_t len)
{
    return syscall2(SYS_EXEC, (long)path, (long)len);
}

static inline long sys_waitpid(int pid)
{
    return syscall1(SYS_WAITPID, pid);
}

static inline long sys_readfile(const char *name, void *buf, size_t size)
{
    return syscall3(SYS_READFILE, (long)name, (long)buf, (long)size);
}

static inline long sys_readdir(char *name_buf, size_t bufsize,
                               unsigned int index)
{
    return syscall3(SYS_READDIR, (long)name_buf, (long)bufsize, (long)index);
}

static inline long sys_getprocs(struct proc_info *buf, size_t bufsize)
{
    return syscall2(SYS_GETPROCS, (long)buf, (long)bufsize);
}

static inline long sys_kill(int pid)
{
    return syscall1(SYS_KILL, pid);
}

static inline long sys_uptime(void)
{
    return syscall0(SYS_UPTIME);
}

static inline void sys_reboot(void)
{
    syscall0(SYS_REBOOT);
    for (;;) ;
}

static inline void sys_shutdown(void)
{
    syscall0(SYS_SHUTDOWN);
    for (;;) ;
}
