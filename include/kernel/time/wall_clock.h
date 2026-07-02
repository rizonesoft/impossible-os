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

/* Set wall time and atomically capture the old effective wall time into
 * *previous_out (NULL to skip) under one writer hold, so a racing setter cannot
 * make the returned previous time stale. Placeholder new_time is rejected. */
void KeSetSystemTimeEx(FILETIME new_time, FILETIME *previous_out);

/* Sub-microsecond precise wall time via TSC/HPET interpolation.
 * Currently identical to KeQuerySystemTime() since both use mono_ns().
 * Will diverge when (KUSER_SHARED_DATA) adds a coarse tick path. */
FILETIME KeQuerySystemTimePrecise(void);

/* Coarse UTC wall time: returns the value cached by the last timer tick (no
 * clocksource read, no seqlock, single atomic load). Lags KeQuerySystemTime()
 * by up to one tick; returns FILETIME_NOW_PLACEHOLDER before the first tick /
 * wall-clock seed. For hot-path callers (klog, accounting) that can tolerate
 * tick granularity -- the analogue of Linux ktime_get_coarse(). */
FILETIME KeQuerySystemTimeCoarse(void);

/* Returns 1 if wall clock has been initialized. */
int wall_clock_ready(void);

/* Returns 1 only when the wall clock was seeded from a REAL time source
 * (UEFI/RTC or an explicit set) -- initialized-but-unsourced clocks report
 * 0.  Gate absolute-deadline conversions on this, not wall_clock_ready(). */
int wall_clock_time_sourced(void);

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

/* Sample the wall clock + interrupt time from ONE monotonic read, so both
 * describe the same instant (used by the KUSD ISR updater to avoid two
 * clocksource reads per tick). Either out-pointer may be NULL. */
void wall_clock_snapshot(FILETIME *system_out, uint64_t *interrupt_out);

/* Per-tick coarse cache update -- call ONCE per timer tick from the ISR (BSP).
 * Takes ONE precise mono_ns() sample, computes system + interrupt time, and
 * publishes them for the lock-free KeQuerySystemTimeCoarse() /
 * KeQueryInterruptTimeCoarse() readers. The same value feeds the KUSD page, so
 * the per-tick clocksource read is amortized across KUSD + every coarse reader
 * (the fast-path win is reader-side: no per-call clock read). Returns both via
 * the out-pointers (for the KUSD updater); either may be NULL. */
void wall_clock_tick_cache(FILETIME *system_out, uint64_t *interrupt_out);

/* ---- NTP continuous wall-time discipline --------------------------- */

/* Register the per-tick NTP discipline applier on the kworker pool (Phase 3,
 * after the scheduler + kworker_init). Idempotent. Consumes the freq/slew stored
 * by ke_ntp_adjtime into the wall clock continuously, behind a wall-time floor so
 * a negative correction is a brief slow not a backward step, and flips the NTP
 * status "ntp-pending" -> "ntp" once applied. */
void ke_ntp_discipline_init(void);

/* Pure (no hardware): the monotonic-clamp result for a wall read of `cand`
 * against `floor`. Clamps UP (max) when read_gen == cur_gen; returns `cand`
 * unchanged on a generation mismatch (a manual KeSetSystemTime happened since the
 * read snapshotted its anchor, so this pre-set value must not raise the new
 * floor). Unit-tested. */
uint64_t wall_floor_clamp(uint64_t cand, uint64_t floor,
                          uint32_t read_gen, uint32_t cur_gen);

/* ---- Interrupt time APIs -------------------------------------------- */

/* 100 ns since boot, including suspend bias. Monotonically increasing. */
uint64_t KeQueryInterruptTime(void);

/* Same as above but sub-tick interpolated; also returns QPC value. */
uint64_t KeQueryInterruptTimePrecise(uint64_t *qpc_value);

/* Coarse interrupt time (100 ns since boot, incl. suspend bias): the value
 * cached by the last timer tick. No clocksource read, no seqlock; lags
 * KeQueryInterruptTime() by up to one tick. Returns 0 before the first tick. */
uint64_t KeQueryInterruptTimeCoarse(void);

/* Interrupt time minus suspend bias. Pauses during S3/S4. */
uint64_t KeQueryUnbiasedInterruptTime(void);

/* Add suspend bias (called from S3/S4 resume path --). */
void ke_suspend_bias_update(uint64_t bias_100ns);

/* Register NtQuerySystemTime/NtSetSystemTime/NtQueryPerformanceCounter in SSDT. */
void wall_clock_register_ssdt(void);
