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
 * Will diverge when (KUSER_SHARED_DATA) adds a coarse tick path. */
FILETIME KeQuerySystemTimePrecise(void);

/* Returns 1 if wall clock has been initialized. */
int wall_clock_ready(void);

/* ---- Kernel time service API ---------------------------------------- */

/* Timer-tick counter since boot (one per timer interrupt). Pair with
 * KeQueryTimeIncrement(): tick_count * increment ~= uptime in 100 ns units. */
void KeQueryTickCount(uint64_t *tick_count);

/* Base periodic timer interrupt increment in 100 ns units (10 ms at 100 Hz). */
void KeQueryTimeIncrement(uint32_t *increment);

/* Delay the current thread (PASSIVE_LEVEL only -- refused at DISPATCH_LEVEL+).
 * NT interval semantics: negative = relative (100 ns magnitude), positive =
 * absolute FILETIME deadline; zero / already-expired returns immediately. */
void KeDelayExecutionThread(int64_t interval);

/* Pure: resolve an NT delay interval to a clamped millisecond sleep duration.
 * negative = relative magnitude; positive = absolute deadline vs `now`;
 * returns 0 for zero / already-expired (caller returns immediately). Sub-ms
 * non-zero waits round up to 1 ms; the result is clamped to UINT32_MAX ms.
 * Side-effect-free -- unit-tested directly. */
uint32_t ke_delay_interval_to_ms(int64_t interval, FILETIME now);

/* Returns 1 when KeQuerySystemTime() is usable. */
int time_service_ready(void);

/* ---- Interrupt time APIs -------------------------------------------- */

/* 100 ns since boot, including suspend bias. Monotonically increasing. */
uint64_t KeQueryInterruptTime(void);

/* Same as above but sub-tick interpolated; also returns QPC value. */
uint64_t KeQueryInterruptTimePrecise(uint64_t *qpc_value);

/* Interrupt time minus suspend bias. Pauses during S3/S4. */
uint64_t KeQueryUnbiasedInterruptTime(void);

/* Add suspend bias (called from S3/S4 resume path --). */
void ke_suspend_bias_update(uint64_t bias_100ns);

/* Register NtQuerySystemTime/NtSetSystemTime/NtQueryPerformanceCounter in SSDT. */
void wall_clock_register_ssdt(void);
