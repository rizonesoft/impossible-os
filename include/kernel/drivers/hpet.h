/* ============================================================================
 * hpet.h -- HPET main counter driver
 *
 * Provides access to the HPET 64-bit main counter for monotonic time.
 * Timer comparators are NOT handled here -- those belong in the scheduler
 * timer TODO. This driver covers only the free-running counter.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Initialize HPET: map MMIO, read capabilities, enable counter.
 * Returns 0 on success, -1 if HPET is absent or non-functional. */
int hpet_init(void);

/* Returns 1 if HPET was successfully initialized. */
int hpet_available(void);

/* Returns HPET frequency in Hz (0 if not available). */
uint64_t hpet_frequency_hz(void);

/* Read the HPET main counter value (64-bit, monotonically increasing). */
uint64_t hpet_read_counter(void);

/* Convert HPET counter value to nanoseconds. */
uint64_t hpet_ns(void);
