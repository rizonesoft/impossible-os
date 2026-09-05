/* ============================================================================
 * pipe.h -- Kernel IPC pipes
 *
 * Unidirectional byte-stream pipe with a 4 KiB ring buffer.
 * Synchronized with mutex (buffer access) and semaphores (blocking).
 *
 * Usage:
 *   int fds[2];
 *   pipe_create(fds);    // fds[0] = read end, fds[1] = write end
 *   pipe_write(fds[1], data, len);
 *   pipe_read(fds[0], buf, len);
 *   pipe_close(fds[0], PIPE_READ);
 *   pipe_close(fds[1], PIPE_WRITE);
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"
#include "kernel/sched/mutex.h"
#include "kernel/sched/semaphore.h"

/* Ring buffer size */
#define PIPE_BUF_SIZE  4096

/* Maximum number of concurrent pipes */
#define PIPE_MAX       16

/* Pipe end identifiers */
#define PIPE_READ      0
#define PIPE_WRITE     1

/* Pipe structure */
typedef struct pipe {
    uint8_t     buf[PIPE_BUF_SIZE];  /* ring buffer */
    uint32_t    read_pos;            /* read cursor */
    uint32_t    write_pos;           /* write cursor */
    uint32_t    count;               /* bytes currently in buffer */
    mutex_t     lock;                /* protects buffer state */
    semaphore_t readable;            /* signaled when data available */
    semaphore_t writable;            /* signaled when space available */
    uint32_t    read_open;           /* 1 = read end is open */
    uint32_t    write_open;          /* 1 = write end is open */
    uint32_t    in_use;              /* 1 = pipe slot is allocated */
} pipe_t;

/* --- API --- */

/* Create a new pipe. Returns 0 on success, -1 on failure.
 * fds[0] = read end (pipe ID), fds[1] = write end (same pipe ID). */
int pipe_create(int fds[2]);

/* Write up to 'len' bytes to a pipe. Blocks if buffer is full.
 * Returns bytes written, or -1 if write end is closed (SIGPIPE). */
int32_t pipe_write(int pipe_id, const void *data, uint32_t len);

/* Read up to 'len' bytes from a pipe. Blocks if buffer is empty.
 * Returns bytes read, or 0 on EOF (write end closed + buffer empty). */
int32_t pipe_read(int pipe_id, void *buf, uint32_t len);

/* Close one end of a pipe.
 * 'end' = PIPE_READ or PIPE_WRITE.
 * When both ends are closed, the pipe is freed. */
void pipe_close(int pipe_id, int end);

/* Initialize the pipe subsystem. Safe to call multiple times (idempotent),
 * and a repeat call is a NO-OP that reports the state it found -- it does NOT
 * reset live pipes, which the pre-TODO-33-s15 static-array version silently
 * did. Returns BOOT_OK once the pool is usable, or BOOT_DEGRADED if the pool
 * could not be allocated (or another CPU is still allocating it). The pool is
 * frame-backed via pmm_alloc_pages_hhdm, so this CAN fail; boot_desktop.c
 * routes BOOT_DEGRADED into the degraded-subsystem path rather than halting. */
boot_result_t pipe_init(void);

/* Is the pipe pool allocated and published? Every entry point above gates on
 * this: over a frame-backed pool an un-initialized or degraded subsystem must
 * refuse, where the old static array happened to return -1 off a zeroed
 * in_use. A caller that ignores per-call return values can check up front
 * instead of reporting a false success. */
int pipe_ready(void);

#ifdef KERNEL_TESTS
/* Test-only: free the pool and reset to uninitialized so a test can drive
 * pipe_init()'s OOM path via pmm_alloc_fail_next(). Never called outside
 * KERNEL_TESTS. REFUSES (returns 0, pool untouched) while any slot is still
 * claimed, because a caller parked inside pipe_read() holds a pointer into
 * the frames this would free. Returns 1 when the pool was reclaimed. */
int pipe_test_reset_for_fault_injection(void);
#endif
