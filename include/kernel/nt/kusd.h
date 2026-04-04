/* ============================================================================
 * kusd.h -- KUSER_SHARED_DATA at 0x7FFE0000
 *
 * Windows maps a single physical page at fixed virtual address 0x7FFE0000
 * (user read-only) and a kernel writable alias. User-mode code reads time,
 * tick count, OS version, and processor features without a syscall.
 *
 * Only time-critical fields are defined here. Full population of static
 * fields (NtSystemRoot, ProcessorFeatures, Cookie, etc.) is deferred to
 * TODO-04 PEB/TEB §11 kusd_init().
 *
 * Reference: Windows SDK ntddk.h KUSER_SHARED_DATA layout (x64)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- KSYSTEM_TIME -- lock-free 64-bit time for 32-bit readers ----------- */

typedef struct {
    uint32_t LowPart;
    int32_t  High1Time;
    int32_t  High2Time;
} KSYSTEM_TIME;

/* Write a 64-bit value using the triple-write protocol.
 * Writer order: High1Time, LowPart, High2Time.
 * Reader order: read High1Time, LowPart, High2Time; retry if High1!=High2. */
static inline void ksystem_time_write(volatile KSYSTEM_TIME *dst, uint64_t val)
{
    int32_t hi = (int32_t)(val >> 32);
    uint32_t lo = (uint32_t)val;
    dst->High1Time = hi;
    __asm__ volatile ("" ::: "memory");  /* compiler barrier */
    dst->LowPart = lo;
    __asm__ volatile ("" ::: "memory");
    dst->High2Time = hi;
}

/* ---- KUSER_SHARED_DATA (partial -- time fields only) -------------------- */

/* Fixed user-mode virtual address (Windows standard) */
#define KUSD_USER_VA   0x7FFE0000ULL

/* Field offsets (must match Windows x64 layout exactly) */
#define KUSD_OFF_TICK_COUNT_MULTIPLIER  0x004
#define KUSD_OFF_INTERRUPT_TIME         0x008  /* KSYSTEM_TIME */
#define KUSD_OFF_SYSTEM_TIME            0x014  /* KSYSTEM_TIME */
#define KUSD_OFF_TIMEZONE_BIAS          0x020  /* KSYSTEM_TIME */
#define KUSD_OFF_NT_MAJOR_VERSION       0x026C
#define KUSD_OFF_NT_MINOR_VERSION       0x0270
#define KUSD_OFF_NT_BUILD_NUMBER        0x0260
#define KUSD_OFF_TICK_COUNT             0x0320 /* KSYSTEM_TIME / TickCountQuad */

/* Kernel-writable pointer to the KUSD page (set during init) */
extern volatile uint8_t *g_kusd;

/* Initialize the KUSD page: allocate physical frame, map at user VA
 * (read-only) and kernel alias (read-write). Must be called after VMM. */
void kusd_page_init(void);

/* Update time fields from timer ISR. Volatile writes only, no locks.
 * Called at CLOCK_LEVEL IRQL from LAPIC/PIT timer handler. */
void kusd_update_time(void);

/* Returns 1 after kusd_page_init() completes successfully. */
int kusd_ready(void);
