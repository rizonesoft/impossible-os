/* ============================================================================
 * timer_resolution.h -- Timer resolution management
 *
 * Windows-style per-process timer resolution control. The shortest
 * requested resolution from any active process wins. Default is 15.625 ms
 * (64 Hz); minimum is 0.5 ms (2000 Hz) on hardware that supports it.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Resolution values in 100 ns units */
#define TIMER_RES_DEFAULT   156250   /* 15.625 ms = 64 Hz (Win32 default) */
#define TIMER_RES_MINIMUM    5000    /* 0.5 ms = 2000 Hz (hardware minimum) */

/* Initialize timer resolution subsystem. */
void timer_resolution_init(void);

/* Kernel API: request or release a timer resolution.
 * desired_100ns: requested period in 100 ns units.
 * set: 1 = request, 0 = release.
 * Returns the actual resolution after arbitration. */
uint32_t KeSetTimerResolution(uint32_t desired_100ns, int set);

/* Query current resolution state. */
void KeQueryTimerResolution(uint32_t *max_time, uint32_t *min_time,
                             uint32_t *current_time);

/* Register SSDT handlers for NtSetTimerResolution / NtQueryTimerResolution. */
void timer_resolution_register_ssdt(void);
