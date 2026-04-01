/* ============================================================================
 * ob_thread.h — Thread object type for the Object Manager
 *
 * THREAD_OBJECT wraps a thread sub-struct pointer.  Each thread is
 * inserted into \KernelObjects\Thread<PID>.<TID> at creation.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward-declare thread struct */
struct thread;

/* --- THREAD_OBJECT body -------------------------------------------------- */

typedef struct thread_object {
    struct thread *thread;     /* pointer into task's threads[] array */
    uint32_t       task_pid;   /* owning task PID */
    uint32_t       thread_id;  /* thread ID within task */
} THREAD_OBJECT;

/* --- API ----------------------------------------------------------------- */

/*
 * ob_thread_type_init — register ObpThreadType with proper body and callbacks.
 * Called from ob_init().
 */
void ob_thread_type_init(void);

/*
 * ob_thread_create — allocate a THREAD_OBJECT and insert it into
 * \KernelObjects\Thread<PID>.<TID>.  Called after thread creation.
 */
void ob_thread_create(struct thread *thr, uint32_t task_pid);

/*
 * ob_thread_mark_dead — clear OB_FLAG_PERMANENT so the object can be
 * freed when all references are released.  Called from thread_exit().
 */
void ob_thread_mark_dead(uint32_t task_pid, uint32_t tid);
