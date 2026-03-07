/* ============================================================================
 * semaphore.h — Kernel counting semaphore
 *
 * Classic Dijkstra counting semaphore. Threads call sem_wait() to decrement
 * the count (blocking if it would go negative) and sem_signal() to increment
 * it (waking one blocked waiter).
 *
 * Use cases:
 *   - Binary semaphore (init count = 1): mutual exclusion (like a mutex)
 *   - Counting semaphore (init count = N): limit concurrent access to N slots
 *   - Event signaling (init count = 0): producer signals, consumer waits
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum number of threads that can wait on a single semaphore */
#define SEM_MAX_WAITERS  16

/* Semaphore structure */
typedef struct semaphore {
    volatile int32_t  count;                          /* current count */
    uint32_t          waiter_tasks[SEM_MAX_WAITERS];  /* blocked task indices */
    uint32_t          waiter_threads[SEM_MAX_WAITERS];/* blocked thread indices */
    uint32_t          num_waiters;                    /* number of blocked waiters */
    const char       *name;                           /* debug name */
} semaphore_t;

/* Static initializer macro */
#define SEM_INIT(n, c) { .count = (c), .num_waiters = 0, .name = (n) }

/* --- API --- */

/* Initialize a semaphore with an initial count. */
void sem_init(semaphore_t *s, const char *name, int32_t initial_count);

/* Decrement the semaphore. Blocks (yields) if count would go below 0. */
void sem_wait(semaphore_t *s);

/* Increment the semaphore. Wakes one blocked waiter if any. */
void sem_signal(semaphore_t *s);

/* Try to decrement without blocking. Returns 1 on success, 0 if would block. */
int sem_trywait(semaphore_t *s);

/* Get the current count (for debugging). */
int32_t sem_value(semaphore_t *s);
