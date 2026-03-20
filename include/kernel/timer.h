/* ============================================================================
 * timer.h — Unified Timer Subsystem (UTS) HAL
 *
 * Single source of truth for all kernel timekeeping.  Every subsystem
 * (UI, scheduler, VFS, drivers) calls the hardware-agnostic API below.
 * Direct access to PIT or LAPIC timer hardware is FORBIDDEN outside
 * the driver files themselves.
 *
 * The global pointer `g_system_timer` is set once during early boot by
 * timer_hal_init() (§6.4) and never changes.  Before it is set, the
 * functions below are safe to call — they return immediately / return 0.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declaration for interrupt frame (used by tick handler) */
struct interrupt_frame;

/* ---- Timer driver vtable ---- */

typedef struct timer_driver {
    const char *name;                                /* e.g., "LAPIC", "PIT" */
    void (*init)(uint32_t hz);                       /* start periodic ticks */
    uint64_t (*get_ticks)(void);                     /* monotonic tick counter */
    void (*sleep_ms)(uint32_t ms);                   /* blocking delay */
    uint32_t (*get_freq)(void);                      /* current tick freq (Hz) */
} timer_driver_t;

/* ---- THE single source of truth ---- */

extern timer_driver_t *g_system_timer;

/* ---- Hardware-agnostic API (all kernel code calls these) ---- */

/* Sleep for approximately the given number of milliseconds.
 * Safe to call before g_system_timer is set (returns immediately). */
void sleep_ms(uint32_t ms);

/* Return the monotonic tick counter from the active timer backend.
 * Safe to call before g_system_timer is set (returns 0). */
uint64_t system_get_ticks(void);

/* Return the current timer tick frequency in Hz.
 * Safe to call before g_system_timer is set (returns 0). */
uint32_t system_get_freq(void);

/* Return seconds elapsed since boot (ticks / freq).
 * Safe to call before g_system_timer is set (returns 0). */
uint64_t uptime(void);
