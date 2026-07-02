/* ============================================================================
 * nt_sync.h -- NT synchronization object SSDT handlers
 *
 * NtCreateEvent, NtCreateMutant, NtCreateSemaphore, NtWaitForMultipleObjects,
 * NtSignalAndWaitForSingleObject, and keyed event stubs.
 * SSDT indices 0x0070-0x0087 + 0x0006-0x0008.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"  /* STATUS_WAIT_0 / STATUS_ABANDONED (single source of truth) */

/* Register all sync object SSDT handlers.
 * Call once during Phase 3, after ssdt_init(). */
void nt_sync_register_ssdt(void);

/* ---- Wait constants ----------------------------------------------------- */

#define MAXIMUM_WAIT_OBJECTS  64

/* WaitType for NtWaitForMultipleObjects */
#define WaitAll  0   /* all objects must be signalled */
#define WaitAny  1   /* any one object signalled suffices */

/* STATUS_WAIT_0 / STATUS_ABANDONED are defined canonically in ntstatus.h. */

/* ---- Event types -------------------------------------------------------- */

#define NotificationEvent     0   /* manual-reset */
#define SynchronizationEvent  1   /* auto-reset */

/* ---- Event basic information -------------------------------------------- */

typedef struct {
    uint32_t EventType;       /* NotificationEvent or SynchronizationEvent */
    uint32_t EventState;      /* 1 = signalled, 0 = not signalled */
} EVENT_BASIC_INFORMATION;

/* ---- Mutant basic information ------------------------------------------
 * Windows MUTANT_BASIC_INFORMATION: LONG CurrentCount + BOOLEAN
 * OwnedByCaller + BOOLEAN AbandonedState, tail-padded to 8 bytes.  The
 * two status bytes are BOOLEAN (uint8_t), not 32-bit, so an 8-byte caller
 * buffer is accepted at the correct field offsets. */
typedef struct {
    int32_t  CurrentCount;    /* lock depth (1 free, 0 held, negative = recursive) */
    uint8_t  OwnedByCaller;   /* 1 if current thread owns it */
    uint8_t  AbandonedState;  /* 1 if previous owner died */
    uint8_t  _pad[2];         /* tail padding to the Windows 8-byte size */
} MUTANT_BASIC_INFORMATION;

_Static_assert(sizeof(MUTANT_BASIC_INFORMATION) == 8,
    "MUTANT_BASIC_INFORMATION must match the Windows 8-byte layout");
_Static_assert(__builtin_offsetof(MUTANT_BASIC_INFORMATION, OwnedByCaller) == 4,
    "OwnedByCaller must follow the 4-byte CurrentCount");
_Static_assert(__builtin_offsetof(MUTANT_BASIC_INFORMATION, AbandonedState) == 5,
    "AbandonedState must follow OwnedByCaller");

/* ---- Semaphore basic information ---------------------------------------- */

typedef struct {
    int32_t  CurrentCount;
    int32_t  MaximumCount;
} SEMAPHORE_BASIC_INFORMATION;
