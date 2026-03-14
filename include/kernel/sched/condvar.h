/* ============================================================================
 * condvar.h — Kernel condition variable
 *
 * Condition variables allow a thread to atomically release a mutex and sleep
 * until another thread signals a condition. On wake, the mutex is re-acquired
 * before returning to the caller.
 *
 * Design:
 *   - Pairs exclusively with mutex_t (same as POSIX pthread_cond_t)
 *   - cond_wait: unlocks mutex → enqueues thread → yields → re-locks mutex
 *   - cond_signal: wakes one waiter (FIFO order)
 *   - cond_broadcast: wakes all waiters simultaneously
 *   - Wait queue uses the same task/thread id array pattern as mutex.c
 *
 * Usage (producer-consumer):
 *   mutex_lock(&q->lock);
 *   while (q->count == 0)
 *       cond_wait(&q->nonempty, &q->lock);   // sleeps, re-locks on return
 *   item = dequeue(q);
 *   mutex_unlock(&q->lock);
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/mutex.h"

/* Maximum threads that can wait on a single condvar */
#define CONDVAR_MAX_WAITERS  16

typedef struct condvar {
    uint32_t  waiter_tasks[CONDVAR_MAX_WAITERS];
    uint32_t  waiter_threads[CONDVAR_MAX_WAITERS];
    uint32_t  num_waiters;
    const char *name;   /* debug name */
} condvar_t;

/* Static initializer */
#define CONDVAR_INIT  { {0}, {0}, 0, NULL }

/* --- API --- */

/* Initialize a condition variable at runtime. */
void cond_init(condvar_t *cond, const char *name);

/* Atomically release mutex, sleep until signalled, then re-acquire mutex.
 * The calling thread MUST hold the mutex before calling cond_wait. */
void cond_wait(condvar_t *cond, mutex_t *mutex);

/* Wake one thread waiting on cond (FIFO). No-op if no waiters. */
void cond_signal(condvar_t *cond);

/* Wake ALL threads waiting on cond. No-op if no waiters. */
void cond_broadcast(condvar_t *cond);
