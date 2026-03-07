/* ============================================================================
 * mutex.h — Kernel mutex synchronization
 *
 * Provides mutual exclusion primitives for kernel threads. Mutexes support
 * blocking lock (yield-based sleep), non-blocking trylock, and basic
 * deadlock detection via lock-ordering checks.
 *
 * Design:
 *   - Non-recursive: locking a mutex you already hold triggers deadlock warning
 *   - Owner-tracked: only the owning thread/task can unlock
 *   - Wait queue: blocked threads yield() until released
 *   - Lock order: optional sequential ID for detecting potential deadlocks
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum number of threads that can wait on a single mutex */
#define MUTEX_MAX_WAITERS  16

/* Mutex structure */
typedef struct mutex {
    volatile uint32_t locked;              /* 1 = locked, 0 = unlocked */
    uint32_t          owner_task;          /* task index of current owner */
    uint32_t          owner_thread;        /* thread index of current owner */
    uint32_t          lock_order;          /* ordering ID for deadlock detection (0 = unchecked) */
    /* Wait queue: tasks/threads blocked waiting for this mutex */
    uint32_t          waiter_tasks[MUTEX_MAX_WAITERS];
    uint32_t          waiter_threads[MUTEX_MAX_WAITERS];
    uint32_t          num_waiters;
    const char       *name;               /* debug name (e.g., "heap_lock") */
} mutex_t;

/* Static initializer macro */
#define MUTEX_INIT(n) { .locked = 0, .owner_task = 0, .owner_thread = 0, \
                        .lock_order = 0, .num_waiters = 0, .name = (n) }

/* --- API --- */

/* Initialize a mutex at runtime. */
void mutex_init(mutex_t *m, const char *name);

/* Initialize a mutex with a lock ordering ID for deadlock detection.
 * Lock order must be acquired in ascending order (lower ID first). */
void mutex_init_ordered(mutex_t *m, const char *name, uint32_t order);

/* Acquire the mutex. Blocks (yields) if already locked by another thread.
 * Detects self-deadlock (locking a mutex you already hold). */
void mutex_lock(mutex_t *m);

/* Release the mutex. Must be called by the owning thread.
 * Wakes one waiter if any are blocked. */
void mutex_unlock(mutex_t *m);

/* Try to acquire the mutex without blocking.
 * Returns 1 on success (lock acquired), 0 on failure (already locked). */
int mutex_trylock(mutex_t *m);

/* Check if the mutex is currently locked (for debugging). */
int mutex_is_locked(mutex_t *m);
