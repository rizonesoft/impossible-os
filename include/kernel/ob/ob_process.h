/* ============================================================================
 * ob_process.h -- Process object type for the Object Manager
 *
 * PROCESS_OBJECT wraps a task_t pointer.  Each process is inserted into
 * \KernelObjects\Process<PID> at creation and made temporary at death.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward-declare task struct */
struct task;

/* --- PROCESS_OBJECT body ------------------------------------------------- */

typedef struct process_object {
    struct task *task;     /* pointer into static tasks[] array */
    uint32_t     pid;     /* cached PID */
} PROCESS_OBJECT;

/* --- API ----------------------------------------------------------------- */

/*
 * ob_process_type_init -- register ObpProcessType with proper body and callbacks.
 * Called from ob_init().
 */
void ob_process_type_init(void);

/*
 * ob_process_create -- allocate a PROCESS_OBJECT and insert it into
 * \KernelObjects\Process<PID>.  Called after task_create / task_fork.
 */
void ob_process_create(struct task *t);

/*
 * ob_process_mark_dead -- clear OB_FLAG_PERMANENT so the object can be
 * freed when all references are released.  Called from task_exit().
 */
void ob_process_mark_dead(uint32_t pid);
