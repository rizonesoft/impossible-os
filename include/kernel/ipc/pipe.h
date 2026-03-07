/* ============================================================================
 * pipe.h — Kernel IPC pipes
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

/* Initialize the pipe subsystem. */
void pipe_init(void);
