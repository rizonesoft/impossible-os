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
    int (*arm_oneshot)(uint64_t deadline_mono_ns);   /* one-shot event at an
                                                      * absolute mono_ns()
                                                      * deadline; NULL when
                                                      * the backend has no
                                                      * one-shot support */
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
 * At 100 Hz timer, every_n_ticks=10 gives ~10 fps.
 * SINGLETON slot: a second registration silently replaces the first.
 * every_n_ticks == 0 is guarded -- the callback never fires.
 * Safe to call with interrupts enabled: fn is published with release
 * ordering after the divisor, and the ISR snapshots it with acquire. */
void timer_register_tick_callback(void (*fn)(void), uint32_t every_n_ticks);

/* Unregister the tick callback. Retracts fn before clearing the divisor,
 * so a concurrent ISR fire either runs the old callback or none. */
void timer_unregister_tick_callback(void);

/* Called from PIT/LAPIC ISR to fire the registered callback at the
 * configured frequency.  Not for external use. */
void timer_tick_callback_fire(void);

/* ---- One-shot timer event (tickless-idle enabler) ----
 * Arm a single timer event at an absolute mono_ns() deadline. Past
 * deadlines fire as soon as possible. Returns 0 on success, -1 when the
 * active backend has no one-shot support (PIT) or no usable conversion.
 * MECHANISM ONLY: arming temporarily replaces the periodic tick; the
 * timer ISR auto-restores periodic mode when the one-shot fires, so the
 * scheduler heartbeat can never silently stop.
 * CANCELLATION CONTRACT: a UEFI runtime call concurrent with an armed
 * one-shot may CANCEL it (resume forces periodic instead of risking a
 * deadline lost while masked) -- callers must tolerate a periodic tick
 * arriving instead of their event and re-arm. The tickless-idle
 * governor re-arms on every idle entry, which satisfies this naturally. */
int timer_arm_oneshot(uint64_t deadline_mono_ns);

/* Pure delegation core for timer_arm_oneshot (unit-testable with a fake
 * driver): -1 on NULL driver / no one-shot hook, else the backend's
 * return value, deadline forwarded unmodified. */
int timer_arm_oneshot_on(timer_driver_t *drv, uint64_t deadline_mono_ns);

/* ---- Backend-aware tick quiesce (UEFI runtime-call safety) ----
 * Mask the ACTIVE timer backend's tick delivery (LAPIC LVT timer, or the
 * PIT's routed IOAPIC GSI / PIC line) so firmware that re-enables
 * interrupts internally (SMI) cannot take a tick in firmware context.
 * SINGLE-USER and non-reentrant: serialized by the UEFI RT mutex; not
 * for general use. resume() restores the exact pre-quiesce state. */
void timer_hal_quiesce(void);
void timer_hal_resume(void);

/* Serialized tick-rate change (KeSetTimerResolution path): banks the
 * mono_clock tick epoch and reprograms the LAPIC rate as ONE mode
 * transition. Returns 0 on success, -1 when refused (PIT backend has a
 * fixed rate; AP callers are refused -- the heartbeat is BSP-only). */
int timer_set_tick_hz(uint32_t new_hz);

/* ---- UTS initialization ---- */

/* Initialize the Unified Timer Subsystem.
 * Detects platform, selects timer backend (PIT for TCG, LAPIC for all else),
 * calibrates the LAPIC timer via 3-tier waterfall, and assigns g_system_timer.
 * Must be called AFTER lapic_init()/ioapic_init() and BEFORE boot_splash_init(). */
void timer_hal_init(void);
