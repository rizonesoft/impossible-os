/* ============================================================================
 * pipe.c — Kernel IPC pipes
 *
 * 4 KiB ring buffer pipes with blocking read/write using mutex + semaphores.
 *
 * Write path:
 *   1. sem_wait(writable) — block if buffer full
 *   2. mutex_lock(lock)
 *   3. Copy byte to ring buffer, advance write_pos
 *   4. mutex_unlock(lock)
 *   5. sem_signal(readable) — wake a blocked reader
 *
 * Read path:
 *   1. sem_wait(readable) — block if buffer empty
 *   2. mutex_lock(lock)
 *   3. Copy byte from ring buffer, advance read_pos
 *   4. mutex_unlock(lock)
 *   5. sem_signal(writable) — wake a blocked writer
 * ============================================================================ */

#include "kernel/ipc/pipe.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"

/* Global pipe table */
static pipe_t pipes[PIPE_MAX];
static uint32_t pipe_inited = 0;

void pipe_init(void)
{
    uint32_t i;
    for (i = 0; i < PIPE_MAX; i++) {
        pipes[i].in_use = 0;
    }
    pipe_inited = 1;
}

int pipe_create(int fds[2])
{
    uint32_t i;

    if (!pipe_inited)
        pipe_init();

    /* Find a free pipe slot */
    for (i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].in_use)
            break;
    }

    if (i >= PIPE_MAX) {
        printk("[PIPE] No free pipe slots\n");
        return -1;
    }

    /* Initialize the pipe */
    pipes[i].read_pos = 0;
    pipes[i].write_pos = 0;
    pipes[i].count = 0;
    pipes[i].read_open = 1;
    pipes[i].write_open = 1;
    pipes[i].in_use = 1;

    mutex_init(&pipes[i].lock, "pipe_lock");
    sem_init(&pipes[i].readable, "pipe_readable", 0);
    sem_init(&pipes[i].writable, "pipe_writable", (int32_t)PIPE_BUF_SIZE);

    /* Both fds reference the same pipe ID */
    fds[0] = (int)i;  /* read end */
    fds[1] = (int)i;  /* write end */

    printk("[OK] Pipe %u created\n", (uint64_t)i);
    return 0;
}

int32_t pipe_write(int pipe_id, const void *data, uint32_t len)
{
    pipe_t *p;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;

    if (pipe_id < 0 || pipe_id >= (int)PIPE_MAX)
        return -1;

    p = &pipes[pipe_id];

    if (!p->in_use || !p->write_open)
        return -1;

    /* Check if read end is closed — broken pipe */
    if (!p->read_open) {
        printk("[PIPE] Broken pipe (write to closed read end)\n");
        return -1;  /* SIGPIPE equivalent */
    }

    /* Write one byte at a time, blocking if buffer full */
    for (i = 0; i < len; i++) {
        /* Check for broken pipe before each byte */
        if (!p->read_open) {
            return (i > 0) ? (int32_t)i : -1;
        }

        /* Wait for space in the buffer */
        sem_wait(&p->writable);

        mutex_lock(&p->lock);
        p->buf[p->write_pos] = src[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        p->count++;
        mutex_unlock(&p->lock);

        /* Signal that data is available */
        sem_signal(&p->readable);
    }

    return (int32_t)len;
}

int32_t pipe_read(int pipe_id, void *buf, uint32_t len)
{
    pipe_t *p;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t i;

    if (pipe_id < 0 || pipe_id >= (int)PIPE_MAX)
        return -1;

    p = &pipes[pipe_id];

    if (!p->in_use || !p->read_open)
        return -1;

    /* Read one byte at a time */
    for (i = 0; i < len; i++) {
        /* EOF check: write end closed and buffer empty */
        if (!p->write_open && p->count == 0)
            return (int32_t)i;  /* EOF */

        /* If no data and write end still open, block on first byte.
         * For subsequent bytes, only read what's available. */
        if (p->count == 0) {
            if (i > 0)
                return (int32_t)i;  /* return what we have so far */

            /* Block waiting for data (or EOF) */
            while (p->count == 0 && p->write_open) {
                sem_wait(&p->readable);
                /* Re-check conditions after wakeup */
                if (!p->write_open && p->count == 0)
                    return 0;  /* EOF */
            }

            /* If writer closed while we were waiting and buffer is empty */
            if (p->count == 0)
                return 0;  /* EOF */
        } else {
            /* Non-blocking consume of a semaphore token */
            sem_wait(&p->readable);
        }

        mutex_lock(&p->lock);
        dst[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
        mutex_unlock(&p->lock);

        /* Signal that space is available */
        sem_signal(&p->writable);
    }

    return (int32_t)len;
}

void pipe_close(int pipe_id, int end)
{
    pipe_t *p;

    if (pipe_id < 0 || pipe_id >= (int)PIPE_MAX)
        return;

    p = &pipes[pipe_id];

    if (!p->in_use)
        return;

    if (end == PIPE_READ) {
        p->read_open = 0;
        /* Wake any blocked writers */
        sem_signal(&p->writable);
    } else if (end == PIPE_WRITE) {
        p->write_open = 0;
        /* Wake any blocked readers so they see EOF */
        sem_signal(&p->readable);
    }

    /* Free pipe if both ends are closed */
    if (!p->read_open && !p->write_open) {
        p->in_use = 0;
        printk("[PIPE] Pipe %u destroyed\n", (uint64_t)pipe_id);
    }
}
