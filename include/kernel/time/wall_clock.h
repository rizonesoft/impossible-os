/* ============================================================================
 * wall_clock.h -- Kernel wall clock (UTC FILETIME)
 *
 * The wall clock is a FILETIME anchor point paired with the monotonic
 * counter reading at that instant. KeQuerySystemTime() returns the
 * current UTC wall time by adding the elapsed monotonic delta.
 * ============================================================================ */

#pragma once

#include "kernel/nt/filetime.h"

/* Initialize wall clock from UEFI GetTime (preferred) or RTC (fallback).
 * Call once in Phase 2 after UEFI runtime and mono_clock_init(). */
void wall_clock_init(void);

/* Query current UTC wall time as FILETIME. Lock-free (seqlock read). */
FILETIME KeQuerySystemTime(void);

/* Set wall time (e.g., from NTP adjustment). Seqlock write-protected. */
void KeSetSystemTime(FILETIME new_time);

/* Sub-microsecond precise wall time via TSC/HPET interpolation.
 * Currently identical to KeQuerySystemTime() since both use mono_ns().
 * Will diverge when §12 (KUSER_SHARED_DATA) adds a coarse tick path. */
FILETIME KeQuerySystemTimePrecise(void);

/* Returns 1 if wall clock has been initialized. */
int wall_clock_ready(void);

/* ---- Kernel time service API (§6) ---------------------------------------- */

/* 100 ns tick count since boot (same as mono_filetime_units()). */
void KeQueryTickCount(uint64_t *tick_count);

/* Periodic timer interrupt increment in 100 ns units. */
void KeQueryTimeIncrement(uint32_t *increment);

/* Delay current thread. Uses sleep_ms() internally.
 * interval is negative = relative (100 ns units). */
void KeDelayExecutionThread(FILETIME interval);

/* Returns 1 when KeQuerySystemTime() is usable. */
int time_service_ready(void);

/* ---- Interrupt time APIs (§7) -------------------------------------------- */

/* 100 ns since boot, including suspend bias. Monotonically increasing. */
uint64_t KeQueryInterruptTime(void);

/* Same as above but sub-tick interpolated; also returns QPC value. */
uint64_t KeQueryInterruptTimePrecise(uint64_t *qpc_value);

/* Interrupt time minus suspend bias. Pauses during S3/S4. */
uint64_t KeQueryUnbiasedInterruptTime(void);

/* Add suspend bias (called from S3/S4 resume path -- §14). */
void ke_suspend_bias_update(uint64_t bias_100ns);

/* Register NtQuerySystemTime/NtSetSystemTime/NtQueryPerformanceCounter in SSDT. */
void wall_clock_register_ssdt(void);
