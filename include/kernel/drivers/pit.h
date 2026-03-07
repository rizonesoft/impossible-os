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

/* Get current tick count (increments at PIT_TARGET_FREQ Hz) */
uint64_t pit_get_ticks(void);

/* Sleep for approximately the given number of milliseconds */
void sleep_ms(uint32_t ms);

/* Get seconds elapsed since boot */
uint64_t uptime(void);
