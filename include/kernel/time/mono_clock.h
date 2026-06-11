/* ============================================================================
 * mono_clock.h -- Monotonic nanosecond clock
 *
 * Selects the highest-resolution monotonic source: invariant TSC, HPET
 * (when available), or LAPIC timer as last resort. Provides nanosecond
 * and FILETIME-unit reads without division in the hot path.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Clock source IDs */
#define MONO_SRC_NONE    0
#define MONO_SRC_TSC     1
#define MONO_SRC_HPET    2
#define MONO_SRC_LAPIC   3

/* Initialize the monotonic clock. Call once after CPUID, TSC freq
 * measurement, and LAPIC calibration are complete (Phase 1). */
void mono_clock_init(void);

/* Read current monotonic time in nanoseconds since boot.
 * Never wraps for ~584 years. */
uint64_t mono_ns(void);

/* Read current monotonic time in FILETIME units (100 ns intervals). */
uint64_t mono_filetime_units(void);

/* Pure tick-counter scaling for the LAPIC/PIT fallback source (no
 * hardware access; unit-testable). freq 0 = no tick source -> 0. */
uint64_t mono_lapic_ticks_to_ns(uint64_t ticks, uint32_t freq_hz);

/* Bank accumulated tick time into the ns epoch at the OLD stored rate
 * and publish the NEW rate. MUST be called BEFORE the tick hardware
 * reprograms or the fallback clock rewinds across a resolution change.
 * The reader consumes only this snapshot (epoch ns/ticks/freq). */
void mono_clock_tick_rebase(uint32_t new_freq_hz);

/* Return the selected clock source name ("TSC", "HPET", "LAPIC", "none"). */
const char *mono_clock_source_name(void);

/* Return the selected clock source ID (MONO_SRC_*). */
uint32_t mono_clock_source_id(void);

/* Fast inline: read TSC and convert to nanoseconds using pre-computed scale.
 * Applies per-CPU TSC offset for SMP coherence. Only valid when source is TSC. */
uint64_t rdtsc_ns(void);
