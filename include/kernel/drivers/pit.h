/* ============================================================================
 * pit.h — Programmable Interval Timer (8253/8254) driver
 *
 * Programs PIT channel 0 to fire IRQ 0 at ~100 Hz for system timekeeping.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* PIT frequency constants */
#define PIT_BASE_FREQ   1193182     /* PIT oscillator frequency (Hz) */
#define PIT_TARGET_FREQ 100         /* Target tick rate (Hz) */

/* Initialize PIT channel 0 at the target frequency */
void pit_init(void);

/* Increment the tick counter from an external timer source (e.g., LAPIC timer).
 * Also fires the registered callback if one is active.
 * Returns non-zero if a callback was fired (for caller info). */
int pit_tick_increment(void);

/* Set the actual timer frequency (used when LAPIC timer replaces PIT). */
void pit_set_freq(uint32_t hz);

/* Stop the PIT hardware (channel 0). Called after LAPIC timer takes over.
 * Leaves the tick counter and sleep_ms/uptime infrastructure intact. */
void pit_stop(void);

/* Get current tick count (increments at PIT_TARGET_FREQ Hz) */
uint64_t pit_get_ticks(void);

/* Sleep for approximately the given number of milliseconds */
void sleep_ms(uint32_t ms);

/* Get seconds elapsed since boot */
uint64_t uptime(void);

/* Register a periodic callback that fires from the PIT IRQ handler.
 * fn is called every 'every_n_ticks' PIT ticks (e.g., 7 = ~15 fps at 100 Hz).
 * Only one callback can be active at a time. Use for boot splash animation. */
void pit_register_callback(void (*fn)(void), uint32_t every_n_ticks);

/* Unregister the PIT callback. */
void pit_unregister_callback(void);
