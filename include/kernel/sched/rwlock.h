/* ============================================================================
 * rwlock.h — Kernel read-write lock
 *
 * Allows multiple concurrent readers OR a single exclusive writer.
 * More efficient than a plain mutex for read-heavy kernel data structures
 * such as the VFS mount table, loaded module list, and process table.
 *
 * Design:
 *   - Reader count tracked as volatile uint32_t (multiple simultaneous readers OK)
 *   - Writer flag: set to 1 while a writer holds the lock
 *   - Writer wait: reader_lock blocks new readers while a writer is waiting,
 *     preventing writer starvation (writer_pending flag)
 *   - Separate wait queues for readers and writers
 *   - All use yield()-based blocking (same as mutex.c / semaphore.c)
 *
 * Usage:
 *   rwlock_t lock = RWLOCK_INIT;
 *   rwlock_read_lock(&lock);   ... read shared data ...   rwlock_read_unlock(&lock);
 *   rwlock_write_lock(&lock);  ... write shared data ...  rwlock_write_unlock(&lock);
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum waiters per queue */
#define RWLOCK_MAX_WAITERS  16

typedef struct rwlock {
    volatile uint32_t  reader_count;    /* number of active readers */
    volatile uint32_t  writer_held;     /* 1 = writer holds the lock */
    volatile uint32_t  writer_pending;  /* 1 = a writer is waiting (blocks new readers) */

    /* Reader wait queue (blocked because writer holds or is pending) */
    uint32_t  r_waiter_tasks[RWLOCK_MAX_WAITERS];
    uint32_t  r_waiter_threads[RWLOCK_MAX_WAITERS];
    uint32_t  r_num_waiters;

    /* Writer wait queue (blocked because readers or writer active) */
    uint32_t  w_waiter_tasks[RWLOCK_MAX_WAITERS];
    uint32_t  w_waiter_threads[RWLOCK_MAX_WAITERS];
    uint32_t  w_num_waiters;

    const char *name;   /* debug name */
} rwlock_t;

/* Static initializer */
#define RWLOCK_INIT  { 0, 0, 0, {0}, {0}, 0, {0}, {0}, 0, NULL }

/* --- API --- */

/* Initialize an rwlock at runtime. */
void rwlock_init(rwlock_t *rw, const char *name);

/* Acquire for reading. Blocks if a writer holds or is pending.
 * Multiple readers may hold simultaneously. */
void rwlock_read_lock(rwlock_t *rw);

/* Release a read lock. Wakes a pending writer if reader count reaches 0. */
void rwlock_read_unlock(rwlock_t *rw);

/* Acquire for writing. Blocks until all readers finish and no writer holds.
 * Sets writer_pending to prevent new readers from slipping in (starvation guard). */
void rwlock_write_lock(rwlock_t *rw);

/* Release the write lock. Wakes all pending readers (or next writer if any). */
void rwlock_write_unlock(rwlock_t *rw);

/* Non-blocking read attempt. Returns 1 on success, 0 if writer active/pending. */
int rwlock_try_read(rwlock_t *rw);

/* Non-blocking write attempt. Returns 1 on success, 0 if any reader or writer active. */
int rwlock_try_write(rwlock_t *rw);
