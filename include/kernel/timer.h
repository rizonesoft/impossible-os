/* ============================================================================
 * timer.h -- Unified Timer Subsystem (UTS) HAL
 *
 * Single source of truth for all kernel timekeeping.  Every subsystem
 * (UI, scheduler, VFS, drivers) calls the hardware-agnostic API below.
 * Direct access to PIT or LAPIC timer hardware is FORBIDDEN outside
 * the driver files themselves.
 *
 * The global pointer `g_system_timer` is set once during early boot by
 * timer_hal_init() and never changes.  Before it is set, the
 * functions below are safe to call -- they return immediately / return 0.
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
    uint64_t (*read_ns)(void);                       /* monotonic nanoseconds (0 if unsupported) */
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

/* Return nanoseconds elapsed since boot via the active timer's read_ns().
 * Falls back to ticks * (1000000000 / freq) if read_ns is not available. */
uint64_t uptime_ns(void);

/* ---- Timer tick callback (ISR-context) ----
 * One global callback invoked from the active timer ISR at configurable
 * frequency.  Used for animation (spinner), heartbeat LED, etc.
 * The callback runs in interrupt context -- keep it short, no sleeping. */

/* Register a periodic callback called every `every_n_ticks` timer ticks.
 * At 100 Hz timer, every_n_ticks=10 gives ~10 fps. */
void timer_register_tick_callback(void (*fn)(void), uint32_t every_n_ticks);

/* Unregister the tick callback. */
void timer_unregister_tick_callback(void);

/* Called from PIT/LAPIC ISR to fire the registered callback at the
 * configured frequency.  Not for external use. */
void timer_tick_callback_fire(void);

/* ---- UTS initialization ---- */

/* Initialize the Unified Timer Subsystem.
 * Detects platform, selects timer backend (PIT for TCG, LAPIC for all else),
 * calibrates the LAPIC timer via 3-tier waterfall, and assigns g_system_timer.
 * Must be called AFTER lapic_init()/ioapic_init() and BEFORE boot_splash_init(). */
void timer_hal_init(void);
