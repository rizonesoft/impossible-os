/* ============================================================================
 * handle_table.h — Per-process handle table
 *
 * Maps opaque HANDLE integers to (object pointer, granted access, flags).
 * HANDLE values are slot index * 4 (low 2 bits reserved).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Handle attribute flags */
#define OBJ_INHERIT        (1u << 0)  /* handle is inheritable across CreateProcess */
#define OBJ_PROTECT_CLOSE  (1u << 1)  /* NtClose fails unless protection is cleared */

/* Special pseudo-handles (resolved without a table entry) */
#define INVALID_HANDLE_VALUE  ((int32_t)-1)
#define CURRENT_PROCESS       ((int32_t)-2)
#define CURRENT_THREAD        ((int32_t)-3)

/* HANDLE type — always a multiple of 4 */
typedef int32_t HANDLE;

/* Initial and maximum capacity */
#define HANDLE_TABLE_INIT_CAP  64
#define HANDLE_TABLE_MAX_CAP   4096

/* --- HANDLE_TABLE_ENTRY -------------------------------------------------- */

typedef struct handle_table_entry {
    void    *object;          /* body pointer (NULL = free slot) */
    uint32_t granted_access;  /* access rights granted at open time */
    uint32_t attributes;      /* OBJ_INHERIT, OBJ_PROTECT_CLOSE */
} HANDLE_TABLE_ENTRY;

/* --- HANDLE_TABLE -------------------------------------------------------- */

typedef struct handle_table {
    HANDLE_TABLE_ENTRY *entries;   /* array of entries */
    uint32_t            capacity;  /* current allocated slots */
    uint32_t            count;     /* number of occupied slots */
} HANDLE_TABLE;

/* --- API ----------------------------------------------------------------- */

/* Initialise a handle table (allocates the initial entry array).
 * Returns 0 on success, -1 on allocation failure. */
int ob_handle_table_init(HANDLE_TABLE *table);

/* Destroy a handle table: close all handles and free the entry array. */
void ob_handle_table_destroy(HANDLE_TABLE *table);

/* Allocate a handle for an object. Calls ObReferenceObject.
 * Returns HANDLE (>= 0) on success, INVALID_HANDLE_VALUE on failure. */
HANDLE ObpAllocateHandle(HANDLE_TABLE *table, void *object,
                         uint32_t access, uint32_t attrs);

/* Free a handle. Calls on_close (if handle_count drops to 0) and
 * ObDereferenceObject. Returns 0 on success, -1 on invalid handle. */
int ObpFreeHandle(HANDLE_TABLE *table, HANDLE handle);

/* Look up a handle. Returns entry pointer, or NULL if invalid. */
HANDLE_TABLE_ENTRY *ObpLookupHandle(HANDLE_TABLE *table, HANDLE handle);
