/* ============================================================================
 * nt_timer.h -- Timer SSDT
 *
 * Registers NtCreateTimer, NtOpenTimer, NtSetTimer, NtCancelTimer,
 * NtQueryTimer, NtSetTimerEx at SSDT 0x007E-0x0083. Also exposes the
 * tick hook (nt_timer_tick) that the timer ISR calls every tick to scan
 * the armed-timer list.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/nt_types.h"
#include "kernel/ob/ob_timer.h"

/* TIMER_INFORMATION_CLASS (NtQueryTimer) */
#define TimerBasicInformation   0u

/* TIMER_SET_INFORMATION_CLASS (NtSetTimerEx) */
#define TimerSetCoalescableTimer 0u

/* NtSetTimer a5 packing: low 32 = Period (ms), high 32 = ResumeTimer flag */
#define NT_SETTIMER_PACK(period, resume) \
    (((uint64_t)(uint32_t)(period)) | ((uint64_t)((resume) ? 1u : 0u) << 32))

/*
 * TIMER_BASIC_INFORMATION layout matches Windows SDK ntddk.h:
 *   offset 0:  LARGE_INTEGER RemainingTime;  (8B, 100-ns units; signed)
 *   offset 8:  BOOLEAN       TimerState;     (1B)
 *   offset 9:  7 bytes trailing pad to 8B alignment (struct alignment)
 * Total: 16 bytes (LARGE_INTEGER enforces 8B struct alignment).
 */
typedef struct _TIMER_BASIC_INFORMATION {
    LARGE_INTEGER RemainingTime;   /* 0x00 */
    uint8_t       TimerState;      /* 0x08 */
    uint8_t       _pad[7];         /* 0x09 */
} TIMER_BASIC_INFORMATION;
_Static_assert(__builtin_offsetof(TIMER_BASIC_INFORMATION, TimerState) == 8,
               "TIMER_BASIC_INFORMATION.TimerState must be at offset 8 (Win ABI)");
_Static_assert(sizeof(TIMER_BASIC_INFORMATION) == 16,
               "TIMER_BASIC_INFORMATION size must be 16 bytes (Win ABI)");

/* Called from the timer ISR (LAPIC/PIT) on every tick. Walks the armed
 * list under an irqsave spinlock and signals every timer whose due_ns
 * has been reached. Safe for concurrent invocation on multiple CPUs. */
void nt_timer_tick(void);

/* Called from the ObpTimerType on_close/on_delete callbacks to ensure
 * a closing timer is removed from the armed list before it is freed. */
void nt_timer_detach(TIMER_OBJECT *to);

/* Initialize state (zeros the armed list and spinlock) and register the
 * six SSDT handlers 0x007E-0x0083. Called from boot_desktop. */
void nt_timer_register_ssdt(void);
