/* ============================================================================
 * syscall.h -- Userland system call wrappers
 *
 * Inline INT 0x80 wrappers matching the kernel ABI:
 *   RAX = syscall number
 *   RDI = arg1, RSI = arg2, RDX = arg3
 *   Return value in RAX
 *
 * Authoritative source for INT 0x80 syscall numbers + handler signatures:
 *   include/kernel/sched/syscall.h  (the SYS_* defines + dispatch table)
 *   src/kernel/sched/syscall.c      (syscall_handler_80() switch body)
 *
 * When the kernel adds, removes, or renumbers a SYS_* constant on the INT 0x80
 * path, this header MUST be patched in lockstep. Wrappers below are thin and
 * stateless; one inline `static long sys_<name>(...)` per syscall maps the
 * userland C signature to the (a1, a2, a3) register slots. Syscalls that the
 * kernel currently leaves unwired (default branch returns -1) get a #define
 * but NO wrapper, so user code does not get fooled into calling a stub.
 *
 * The SSDT alias block at the bottom of the kernel header (`SYS_NT_*`) is
 * NOT mirrored here -- those are SSDT service numbers used by the INT 0x2E
 * compatibility path, not the INT 0x80 fast path. Native API tests live in
 * a separate test binary that goes through ntdll thunks.
 * ============================================================================ */

#pragma once

#include "types.h"

/* HANDLE: kernel's per-task handle-table index. Mirrors
 * include/kernel/ob/handle_table.h:22 (int32_t HANDLE) so the user-mode
 * caller can store the return value of sys_openfile() / sys_pipe() /
 * sys_opendirobj() / sys_shmem_create() in the same slot the kernel
 * writes to its handle-table entries. */
typedef int32_t HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)-1)

/* Log levels (must match include/kernel/klog.h log_level_t). Used by
 * sys_log() so user-mode tests can route LOG_INFO / LOG_WARN / LOG_ERROR
 * lines through the kernel klog ring without managing a private logger.
 *
 * SECURITY: LOG_FATAL is REJECTED by the kernel SYS_LOG handler from
 * user mode -- klog(LOG_FATAL, ...) enters an infinite hlt loop, and
 * exposing that to ring 3 would let any user binary halt the OS with
 * a one-line call. Pass LOG_DEBUG..LOG_ERROR only; LOG_FATAL returns
 * -1 from sys_log(). The constant is kept here for kernel-header
 * parity and to make the rejection contract explicit. */
#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3
#define LOG_FATAL 4   /* Kernel-only; sys_log(LOG_FATAL, ...) returns -1 */

/* Syscall numbers + FAULT_* subcommand selectors come from the
 * generated abi_numbers.h. Hand-copied `#define SYS_* <N>` here was
 * the original root cause of TODO-04 -17: a kernel-side renumber would
 * silently drift from this file and user binaries would hit the wrong
 * handler at runtime. Regeneration + `make check-abi` catches drift
 * at build time; do not re-add the hand copies. */
#include "abi_numbers.h"

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

/* Net config (must match kernel net.h) */
struct user_net_config {
    unsigned int ip;
    unsigned int subnet;
    unsigned int gateway;
    unsigned int dns;
    unsigned char mac[6];
    unsigned char configured;
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

static inline void sys_yield(void) { syscall0(SYS_YIELD); }
static inline long sys_fork(void)  { return syscall0(SYS_FORK); }

/* Replace the current process image with `path`, passing argv/envp. argv/envp
 * are NULL-terminated arrays of NUL-terminated strings; either may be NULL
 * (NULL argv -> argc=1/program name; NULL envp -> inherit the current env). On
 * success exec does not return; on failure returns < 0. */
static inline long sys_exec(const char *path, char *const argv[],
                            char *const envp[])
{
    return syscall3(SYS_EXEC, (long)path, (long)argv, (long)envp);
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

static inline long sys_kill(int pid)       { return syscall1(SYS_KILL, pid); }
static inline long sys_uptime(void)        { return syscall0(SYS_UPTIME); }
static inline void sys_reboot(void)        { syscall0(SYS_REBOOT); for(;;); }
static inline void sys_shutdown(void)      { syscall0(SYS_SHUTDOWN); for(;;); }

static inline long sys_ping(unsigned int ip, int seq)
{
    return syscall2(SYS_PING, (long)ip, (long)seq);
}

static inline long sys_netinfo(struct user_net_config *buf, size_t size)
{
    return syscall2(SYS_NETINFO, (long)buf, (long)size);
}

/* --- Logging --- */

/* Returns the calling task's PID (>= 1; PID 0 is the kernel idle
 * task which never runs user code). Kernel handler in
 * src/kernel/sched/syscall.c returns task_current()->pid. */
static inline long sys_getpid(void) { return syscall0(SYS_GETPID); }

/* Route a user-mode log line through the kernel klog ring as
 * `[<level>] user: <msg>`. `len` is bytes (NOT including a NUL); kernel
 * silently caps at 120 bytes and rejects level > LOG_FATAL with -1. */
static inline long sys_log(int level, const char *msg, size_t len)
{
    return syscall3(SYS_LOG, level, (long)msg, (long)len);
}

/* --- IPC: pipes + shared memory --- */

/* Create an anonymous pipe pair. fds[0] = read end, fds[1] = write end.
 * Both handles are owned by the calling task's handle table. Returns 0
 * on success; -1 if the pointer is NULL or the kernel could not create
 * the pipe. The caller MUST sys_closehandle() each end. */
static inline long sys_pipe(HANDLE fds[2])
{
    return syscall1(SYS_PIPE, (long)fds);
}

/* Install (or query) a signal handler. Returns the previous handler
 * cast to long; the kernel handler stub itself is currently a no-op
 * for everything except SIGKILL/SIGTERM unwinding. */
static inline long sys_signal(int sig, void *handler)
{
    return syscall2(SYS_SIGNAL, sig, (long)handler);
}

/* Create a named or anonymous shared-memory section of `size` bytes.
 * Returns the section HANDLE, or INVALID_HANDLE_VALUE on failure
 * (NULL name + non-zero size still works -- creates anonymous). */
static inline HANDLE sys_shmem_create(const char *name, uint32_t size)
{
    return (HANDLE)syscall2(SYS_SHMEM_CREATE, (long)name, (long)size);
}

/* Map a previously-created shmem section into the current process's
 * address space. Returns the mapped user virtual address, or 0 on
 * failure (kernel returns 0 from ObMapViewOfSection). The mapping
 * must be torn down with sys_unmapview(addr) BEFORE the process
 * exits; sys_closehandle(sh) drops the handle-table ref but does
 * NOT drop the view pin (that's what sys_unmapview is for). */
static inline uintptr_t sys_shmem_map(HANDLE sh)
{
    return (uintptr_t)syscall1(SYS_SHMEM_MAP, (long)sh);
}

/* Release a shmem view mapped by sys_shmem_map. Drops the view slot
 * AND the ObReferenceObject pin that the map path took, so the
 * backing PMM frames can be reclaimed when the handle is closed.
 * Returns 0 on success, -1 if no view at that address. */
static inline long sys_unmapview(uintptr_t base)
{
    return syscall1(SYS_UNMAPVIEW, (long)base);
}

/* --- Object-Manager-backed file + directory handles --- */

/* Open a file by Windows-style canonical path (e.g. "C:\\hello.txt").
 *
 * `access` is forwarded verbatim to `ob_create_file_handle`. Today
 * the kernel gates `sys_readhandle` on the VFS bit `VFS_O_READ` (0x01)
 * and `sys_writehandle` on `VFS_O_WRITE` (0x02) -- see
 * src/kernel/ob/ob_file.c. The Windows `ACCESS_MASK` semantics
 * (GENERIC_READ = 0x80000000 mapped via RtlMapGenericMask +
 * SeAccessCheck) are NOT yet implemented; that mapping is deferred
 * to the Security Reference Monitor roadmap. Until SRM lands,
 * callers must pass raw VFS bits:
 *   0x01  -- read (required for sys_readhandle to succeed)
 *   0x02  -- write (required for sys_writehandle to succeed)
 *   0x03  -- read+write
 * Once SRM ships this wrapper's contract flips to full Win32
 * ACCESS_MASK and GENERIC_READ/WRITE/EXECUTE/ALL start working.
 *
 * Returns the file HANDLE, or INVALID_HANDLE_VALUE on failure. */
static inline HANDLE sys_openfile(const char *path, uint32_t access)
{
    return (HANDLE)syscall2(SYS_OPENFILE, (long)path, (long)access);
}

/* Close any handle (file, pipe, shmem section, dir object). Returns
 * 0 on success, -1 on failure (already-closed, foreign handle, etc.). */
static inline long sys_closehandle(HANDLE handle)
{
    return syscall1(SYS_CLOSEHANDLE, (long)handle);
}

/* Read up to `size` bytes from `handle` into `buf`. Returns the byte
 * count on success (may be < size at EOF), -1 on failure. Works on
 * file handles + pipe-read-end handles. */
static inline long sys_readhandle(HANDLE handle, void *buf, uint32_t size)
{
    return syscall3(SYS_READHANDLE, (long)handle, (long)buf, (long)size);
}

/* Write up to `size` bytes from `buf` to `handle`. Returns the byte
 * count written on success, -1 on failure (bad handle, type mismatch,
 * pipe closed). Mirror of sys_readhandle; works on pipe-write-end
 * handles today and on file-write handles once the OB file path
 * supports writes end-to-end. */
static inline long sys_writehandle(HANDLE handle, const void *buf, uint32_t size)
{
    return syscall3(SYS_WRITEHANDLE, (long)handle, (long)buf, (long)size);
}

/* Open an Object Manager directory by namespace path (e.g. "\\" for the
 * root, "\\RPC Control" for ALPC-style port directories). Returns the
 * directory HANDLE for use with sys_querydirobj(), or
 * INVALID_HANDLE_VALUE on failure. */
static inline HANDLE sys_opendirobj(const char *path, uint32_t access)
{
    return (HANDLE)syscall2(SYS_OPENDIROBJ, (long)path, (long)access);
}

/* Enumerate entries under a directory object handle. The kernel ABI
 * packs `count` (low 16 bits) and `ctx` (high 16 bits) into arg3 to
 * stay within the 3-register INT 0x80 calling convention; the wrapper
 * does that pack and the inverse unpack on return. On success returns
 * the number of entries written into `buf`; the OUT `*ctx_inout` is
 * updated with the resume cookie for the next call. Returns -1 on
 * failure; `*ctx_inout` is left untouched then. */
static inline long sys_querydirobj(HANDLE dir, void *buf, uint16_t count,
                                   uint16_t *ctx_inout)
{
    long packed = ((long)(*ctx_inout) << 16) | (long)count;
    long ret = syscall3(SYS_QUERYDIROBJ, (long)dir, (long)buf, packed);
    if (ret < 0)
        return ret;
    *ctx_inout = (uint16_t)((uint64_t)ret >> 16);
    return (long)((uint64_t)ret & 0xFFFFu);
}

/* User-mode fault-injection bridge. Arms the kernel
 * allocator countdowns under the test=1 gate. kind = FAULT_* selector,
 * countdown = N-th call to fail (ignored for FAULT_KMALLOC_NEXT which
 * always sets N=1 and for FAULT_CLEAR_ALL).
 *
 * Returns 0 on success. Returns -1 (STATUS_ACCESS_DENIED) when
 * boot.conf test=0 -- the kernel hard-fails the syscall so a
 * production user-mode binary cannot arm the countdowns. Returns -1
 * for unknown `kind` values as well.
 *
 * The kernel automatically sets the task filter to the caller's own
 * PID, so a test can ONLY fault-inject for itself (never arm a trap
 * that a sibling kthread or launcher task would consume). */
static inline long sys_fault_inject(uint32_t kind, uint32_t countdown)
{
    return syscall3(SYS_FAULT_INJECT, (long)kind, (long)countdown, 0L);
}

/* Test-harness self-report. Submits what this binary counted -- assertions
 * passed, assertions failed, and skip BLOCKS taken -- so the user-mode test
 * launcher can report a third outcome in TAP / JUnit XML / JSON. The process
 * exit code carries only two (0 = no failures, 77 = whole binary skipped),
 * which is why a binary that skipped one sub-test used to be indistinguishable
 * from one that ran everything.
 *
 * Call EXACTLY once per binary (UTEST_END does this for you). The kernel
 * treats a repeat submission, or any count above its per-binary ceiling, as a
 * self-contradiction: the record is marked invalid and the launcher escalates
 * the binary to FAIL rather than trusting the numbers.
 *
 * Returns 0 when the report was accepted. Returns -1 when it was rejected AND
 * when the kernel has no test flavor at all (syscall 48 is reserved but
 * undispatched in a release build), so callers must not treat -1 as fatal --
 * a non-reporting binary simply stays on the legacy exit-code-only path. */
static inline long sys_test_report(uint32_t passed, uint32_t failed,
                                   uint32_t skipped)
{
    return syscall3(SYS_TEST_REPORT, (long)passed, (long)failed,
                    (long)skipped);
}
