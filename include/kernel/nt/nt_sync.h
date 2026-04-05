/* ============================================================================
 * nt_sync.h -- NT synchronization object SSDT handlers
 *
 * NtCreateEvent, NtCreateMutant, NtCreateSemaphore, NtWaitForMultipleObjects,
 * NtSignalAndWaitForSingleObject, and keyed event stubs.
 * SSDT indices 0x0070-0x0087 + 0x0006-0x0008.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Register all sync object SSDT handlers.
 * Call once during Phase 3, after ssdt_init(). */
void nt_sync_register_ssdt(void);

/* ---- Wait constants ----------------------------------------------------- */

#define MAXIMUM_WAIT_OBJECTS  64

/* WaitType for NtWaitForMultipleObjects */
#define WaitAll  0   /* all objects must be signalled */
#define WaitAny  1   /* any one object signalled suffices */

/* STATUS_WAIT_0 through STATUS_WAIT_63: index of satisfied object */
#define STATUS_WAIT_0   ((NTSTATUS)0x00000000)

/* ---- Event types -------------------------------------------------------- */

#define NotificationEvent     0   /* manual-reset */
#define SynchronizationEvent  1   /* auto-reset */

/* ---- Event basic information -------------------------------------------- */

typedef struct {
    uint32_t EventType;       /* NotificationEvent or SynchronizationEvent */
    uint32_t EventState;      /* 1 = signalled, 0 = not signalled */
} EVENT_BASIC_INFORMATION;

/* ---- Mutant basic information ------------------------------------------- */

typedef struct {
    int32_t  CurrentCount;    /* lock depth (negative = locked) */
    uint32_t OwnedByCaller;   /* 1 if current thread owns it */
    uint32_t AbandonedState;  /* 1 if previous owner died */
} MUTANT_BASIC_INFORMATION;

/* ---- Semaphore basic information ---------------------------------------- */

typedef struct {
    int32_t  CurrentCount;
    int32_t  MaximumCount;
} SEMAPHORE_BASIC_INFORMATION;
