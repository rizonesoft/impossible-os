/* ============================================================================
 * ob_mutex.h — Mutex (Mutant) object type for the Object Manager
 *
 * MUTEX_OBJECT wraps an embedded mutex_t.  Named mutexes are inserted into
 * \BaseNamedObjects for cross-process sharing.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/mutex.h"
#include "kernel/ob/handle_table.h"

/* Status returned when a mutex owner dies without releasing */
#define MUTEX_ABANDONED  0x00000080

/* --- MUTEX_OBJECT body --------------------------------------------------- */

typedef struct mutex_object {
    mutex_t  mutex;       /* embedded kernel mutex */
    uint32_t abandoned;   /* 1 if owner died without releasing */
} MUTEX_OBJECT;

/* --- API ----------------------------------------------------------------- */

void ob_mutex_type_init(void);

/*
 * NtCreateMutex stub — create or open a named/unnamed mutex.
 *
 * name:          NULL for unnamed, or a name in \BaseNamedObjects.
 * initial_owner: if 1, the calling thread immediately owns the mutex.
 *
 * Returns INVALID_HANDLE_VALUE on failure.
 */
HANDLE NtCreateMutex(HANDLE_TABLE *ht, const char *name, int initial_owner);
