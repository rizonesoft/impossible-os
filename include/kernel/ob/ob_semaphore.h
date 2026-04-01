/* ============================================================================
 * ob_semaphore.h — Semaphore object type for the Object Manager
 *
 * SEMAPHORE_OBJECT wraps an embedded semaphore_t.  Named semaphores are
 * inserted into \BaseNamedObjects for cross-process sharing.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/semaphore.h"
#include "kernel/ob/handle_table.h"

/* --- SEMAPHORE_OBJECT body ----------------------------------------------- */

typedef struct semaphore_object {
    semaphore_t semaphore;    /* embedded kernel semaphore */
    int32_t     max_count;    /* maximum count (for NtReleaseSemaphore bounds check) */
} SEMAPHORE_OBJECT;

/* --- API ----------------------------------------------------------------- */

void ob_semaphore_type_init(void);

/*
 * NtCreateSemaphore stub — create or open a named/unnamed semaphore.
 *
 * name:          NULL for unnamed, or a name in \BaseNamedObjects.
 * initial_count: starting count.
 * max_count:     maximum count (0 = no limit).
 *
 * Returns INVALID_HANDLE_VALUE on failure.
 */
HANDLE NtCreateSemaphore(HANDLE_TABLE *ht, const char *name,
                         int32_t initial_count, int32_t max_count);
