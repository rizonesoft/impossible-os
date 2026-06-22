/* ============================================================================
 * handle_table.h -- Per-process handle table
 *
 * Maps opaque HANDLE integers to (object pointer, granted access, flags).
 * HANDLE values are slot index * 4 (low 2 bits reserved).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Handle attribute flags (Windows-compatible values from OBJECT_ATTRIBUTES) */
#define OBJ_INHERIT        0x00000002  /* handle is inheritable across CreateProcess */
#define OBJ_PROTECT_CLOSE  0x00000001  /* NtClose fails unless protection is cleared */

/* Special pseudo-handles (resolved without a table entry) */
#define INVALID_HANDLE_VALUE  ((int32_t)-1)
#define CURRENT_PROCESS       ((int32_t)-2)
#define CURRENT_THREAD        ((int32_t)-3)

/* HANDLE type -- always a multiple of 4 */
typedef int32_t HANDLE;

/* Initial capacity. The table grows by doubling on demand; the hard ceiling is
 * HANDLE_TABLE_ABSOLUTE_MAX and the per-process quota (handle_limit) is the soft
 * cap enforced before each grow (S14 replaced the old fixed 4096 grow cap, which
 * made the 16384 default quota unreachable). */
#define HANDLE_TABLE_INIT_CAP       64

/* Per-process handle quota (S14) */
#define HANDLE_TABLE_DEFAULT_LIMIT  16384       /* default per-process handle limit */
#define HANDLE_TABLE_ABSOLUTE_MAX   (1 << 20)   /* 1M handles -- absolute ceiling */
#define HANDLE_TABLE_LIMIT_UNLIMITED 0u         /* handle_limit sentinel: no quota (still bounded by ABSOLUTE_MAX) */

/* --- HANDLE_TABLE_ENTRY -------------------------------------------------- */

typedef struct handle_table_entry {
    void    *object;          /* body pointer (NULL = free slot) */
    uint32_t granted_access;  /* access rights granted at open time */
    uint32_t attributes;      /* OBJ_INHERIT, OBJ_PROTECT_CLOSE */
} HANDLE_TABLE_ENTRY;

/* --- HANDLE_TABLE -------------------------------------------------------- */

typedef struct handle_table {
    HANDLE_TABLE_ENTRY *entries;      /* array of entries */
    uint32_t            capacity;     /* current allocated slots */
    uint32_t            count;        /* number of occupied slots */
    uint32_t            handle_limit; /* per-process quota (S14); 0 = unlimited */
    uint8_t             quota_warned; /* S14: one-shot -- exhaustion logs once per episode, not per denial */
} HANDLE_TABLE;

/* --- API ----------------------------------------------------------------- */

/* Initialise a handle table (allocates the initial entry array).
 * Returns 0 on success, -1 on allocation failure. */
int ob_handle_table_init(HANDLE_TABLE *table);

/* Destroy a handle table: close all handles and free the entry array. */
void ob_handle_table_destroy(HANDLE_TABLE *table);

/* Allocate a handle for an object. Calls ObReferenceObject and fires the
 * OB_OPERATION_HANDLE_CREATE pre/post callbacks (S13).
 * Returns HANDLE (>= 0) on success, INVALID_HANDLE_VALUE on failure. */
HANDLE ObpAllocateHandle(HANDLE_TABLE *table, void *object,
                         uint32_t access, uint32_t attrs);

/* Same as ObpAllocateHandle but does NOT fire HANDLE_CREATE object callbacks.
 * For the duplicate path (NtDuplicateObject), which fires its own
 * OB_OPERATION_HANDLE_DUPLICATE callbacks -- so a CREATE-only callback must not
 * also see the duplicate (the operation mask stays authoritative, matching
 * Win11 ObRegisterCallbacks). XREF: 02-kernel-core/TODO-05-object-manager.md S13. */
HANDLE ObpAllocateHandleNoCreateCb(HANDLE_TABLE *table, void *object,
                                   uint32_t access, uint32_t attrs);

/* Free a handle. Calls on_close (if handle_count drops to 0) and
 * ObDereferenceObject. Returns 0 on success, -1 on invalid handle. */
int ObpFreeHandle(HANDLE_TABLE *table, HANDLE handle);

/* Look up a handle. Returns entry pointer, or NULL if invalid. */
HANDLE_TABLE_ENTRY *ObpLookupHandle(HANDLE_TABLE *table, HANDLE handle);

/* Inherit handles from parent to child. Copies all entries with
 * OBJ_INHERIT attribute, preserving slot indices (HANDLE values match).
 * Calls ObReferenceObject for each inherited handle.
 * Child table must be freshly initialized (empty).
 * Best-effort under memory pressure: grows the child to cover the highest
 * inheritable slot; if a grow fails it inherits what fits and logs a warning.
 * Returns the number of handles inherited (>= 0), or -1 for invalid arguments. */
int ob_handle_table_inherit(HANDLE_TABLE *parent, HANDLE_TABLE *child);

/* Set per-process handle limit. Clamped to HANDLE_TABLE_ABSOLUTE_MAX. */
void ob_handle_table_set_limit(HANDLE_TABLE *table, uint32_t new_limit);
