/* ============================================================================
 * mono_clock.h -- Monotonic nanosecond clock
 *
 * Selects the highest-resolution monotonic source: invariant TSC, HPET
 * (when available), or LAPIC timer as last resort. Provides nanosecond
 * and FILETIME-unit reads without division in the hot path.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Clock source IDs. Selection priority is TSC > HPET > PMTMR > LAPIC; the id
 * value is just an identifier, not the priority rank. */
#define MONO_SRC_NONE    0
#define MONO_SRC_TSC     1
#define MONO_SRC_HPET    2
#define MONO_SRC_LAPIC   3
#define MONO_SRC_PMTMR   4   /* ACPI PM timer (PMTMR), fixed 3.579545 MHz */

/* Initialize the monotonic clock. Call once after CPUID, TSC freq
 * measurement, and LAPIC calibration are complete (Phase 1). */
void mono_clock_init(void);

/* Read current monotonic time in nanoseconds since boot.
 * Never wraps for ~584 years. */
uint64_t mono_ns(void);

/* Cheap coarse monotonic read for the scheduler / uptime hot path: avoids the
 * precise source's per-call hardware read (notably the PMTMR's glitch-filtered
 * 3-port-read). Accurate to the timer-ISR advance interval; monotonic. For
 * cheap sources (TSC/HPET/LAPIC) it is identical to mono_ns(). */
uint64_t mono_ns_coarse(void);

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

/* PMTMR fixed frequency (3.579545 MHz) and 24-bit counter mask. The 24-bit
 * counter wraps every ~4.69 s, so the epoch below MUST be advanced from the
 * timer ISR far faster than half that window. */
#define PMTMR_FREQ_HZ    3579545u
#define PMTMR_24BIT_MASK 0x00FFFFFFu

/* Pure wrap-extend helper (unit-testable, no hardware): given the previously
 * banked epoch ns + the last raw PMTMR sample, the new raw sample, and the
 * counter mask, return the ns to add for the masked delta. */
uint64_t mono_pmtmr_delta_ns(uint32_t last_raw, uint32_t now_raw, uint32_t mask);

/* Advance the PMTMR 64-bit epoch from the latest raw counter sample. Call from
 * the timer ISR when the active source is PMTMR (cheap; one port read). No-op
 * for other sources. ISR-safe (seqlock writer, interrupts already off). */
void mono_clock_pmtmr_advance(void);

/* Unconditionally bank the PMTMR epoch (no skip counter), for the tick-quiesce
 * boundary where the timer ISR that drives mono_clock_pmtmr_advance() is masked.
 * Call right before masking the timer and right after unmasking so a sub-wrap
 * quiesce loses no wraps. No-op unless PMTMR is the active source. */
void mono_clock_pmtmr_sync(void);

/* Return the selected clock source name ("TSC", "HPET", "PMTMR", "LAPIC",
 * "none"). */
const char *mono_clock_source_name(void);

/* Return the selected clock source ID (MONO_SRC_*). */
uint32_t mono_clock_source_id(void);

/* Fast inline: read TSC and convert to nanoseconds using pre-computed scale.
 * Applies per-CPU TSC offset for SMP coherence. Only valid when source is TSC. */
uint64_t rdtsc_ns(void);

/* The RAW, UNSCALED time-stamp counter, with no source selection, no epoch and
 * no frequency implied by the value.
 *
 * Exposed for exactly one purpose: a WATCHDOG that has to stay useful when the
 * selected monotonic source has stopped. Every other read in this kernel goes
 * through mono_ns() / uptime_ns(), and those are the correct calls -- they are
 * monotonic, scaled, and demotion-safe. This one is none of those.
 *
 * Its value is that it is INDEPENDENT of the machinery mono_ns() depends on.
 * uptime_ns() resolves through the driver's read_ns to mono_ns_coarse(), which
 * for PMTMR returns an epoch banked by the timer ISR every PMTMR_ADVANCE_TICKS
 * ticks, and for the LAPIC/PIT source is derived from system_get_ticks(): both
 * stop advancing if the tick stops, and a caller waiting on either then has no
 * bound at all. The TSC needs no interrupt, no port I/O and no epoch -- it
 * advances whenever the CPU retires instructions -- so a loop that is executing
 * can always observe it move.
 *
 * Callers must NOT convert it to a duration with a measured frequency: it is
 * unscaled by design, may vary with P-states on an unstable-TSC part, and
 * carries no per-CPU offset correction. A watchdog converts with a deliberately
 * PESSIMISTIC upper-bound frequency so it can only ever fire LATE. */
uint64_t mono_tsc_raw(void);

/* The TRUSTED TSC frequency in Hz, or 0 when there is not one.
 *
 * Companion to mono_tsc_raw(): a watchdog holding a bound against the raw
 * counter needs some rate to turn ticks into a duration, and guessing one is
 * the failure mode -- boot qualification accepts anything from MONO_TSC_HZ_MIN
 * to MONO_TSC_HZ_MAX, so a constant picked from "the fastest CPU I can think
 * of" is not an upper bound at all. This returns the measured value when the
 * kernel still believes it.
 *
 * ZERO IS RETURNED IN THREE CASES, and a caller must not tell them apart: the
 * TSC was never measured, it failed boot qualification, or the drift watchdog
 * later DEMOTED it for running off its measured rate. The last one is why the
 * answer is gated on TSC being the ACTIVE source rather than on the boot
 * descriptor alone: qualification range-checks the descriptor once and never
 * revisits it, so a demoted TSC still passes, and handing out a rate the kernel
 * has stopped believing is worse than handing out none -- the caller gets a
 * confidently wrong duration instead of a known-unknown.
 *
 * So zero means "no trusted rate; fall back to a bound that holds for any
 * qualified TSC (MONO_TSC_HZ_MAX)", never "no counter". mono_tsc_raw() keeps
 * working and keeps advancing in every one of those cases. */
uint64_t mono_tsc_hz(void);

/* The band boot qualification will accept for a TSC. Named rather than left as
 * literals inside mono_source_qualify() because the CEILING is load-bearing
 * outside this subsystem: anything converting raw TSC ticks to a duration
 * without a measured rate has to assume a rate at or above it, and a bound
 * derived from "the fastest shipping part" is far below what this kernel
 * accepts from a scaled or virtual TSC. Raising the ceiling here without
 * revisiting those callers makes their bounds silently wrong. */
#define MONO_TSC_HZ_MIN  100000000ULL      /* 100 MHz */
#define MONO_TSC_HZ_MAX  100000000000ULL   /* 100 GHz */

/* ---- Clocksource quality watchdog (drift demotion) ----------------------- *
 * Continuously cross-checks the active monotonic source against an independent
 * reference (HPET or PMTMR) and demotes a drifting source (an unstable TSC) to
 * a trustworthy fallback, matching Linux clocksource.c (watchdog, "Marking TSC
 * unstable") and the Win11 HAL silent demotion. Source + scale are published as
 * an immutable per-source descriptor selected by an atomic active-source index,
 * so the lock-free mono_ns() reader never sees a torn source/scale across a
 * runtime demotion; a mono-wide floor (ALWAYS applied to every mono_ns() read,
 * and raised to the current value by a demotion before it publishes the new
 * source) plus the re-anchor-to-current-value on switch guarantee mono_ns()
 * never steps backward.
 * ------------------------------------------------------------------------- */

/* Pure (no hardware): the absolute drift in parts-per-million between a
 * reference-clock elapsed and the active-source elapsed over the SAME window.
 * ref_ns is the trusted reference delta, src_ns the active-source delta. Returns
 * 0 when ref_ns == 0 (no window). Saturates at MONO_DRIFT_PPM_MAX. Unit-tested. */
uint32_t mono_drift_ppm(uint64_t ref_ns, uint64_t src_ns);

/* Drift past this ppm marks the active source unstable (Linux uses ~500 ppm /
 * 0.05 % over its watchdog window; we use the same order). */
#define MONO_DRIFT_UNSTABLE_PPM  500u
#define MONO_DRIFT_PPM_MAX       1000000u   /* saturate (100 %) -- avoids overflow */

/* Max tolerated span between the two reference reads bracketing the active-source
 * read in one watchdog sample. A larger span means a delay (IRQ/SMI/preemption)
 * crept between the reads, contaminating the comparison -- that window is
 * discarded, not struck. Mirrors Linux cs_watchdog_read's max-skew retry. */
#define MONO_WATCHDOG_MAX_SKEW_NS  50000u   /* 50 us */


/* Demote the active monotonic source to to_src at runtime (PASSIVE/thread
 * context only -- the watchdog kworker job). Re-anchors to_src to the current
 * mono value, engages the mono-wide floor, then atomically publishes the new
 * active descriptor so mono_ns() never tears or steps backward. No-op if to_src
 * is not an initialized source or is already active. drift_ppm is logged. */
void mono_clock_demote(uint32_t to_src, uint32_t drift_ppm);

/* Register the drift watchdog on the kworker pool (Phase 3, after the scheduler
 * + kworker_init). Idempotent; no-op when no independent reference source exists
 * (e.g. PMTMR is already the active source and no HPET is present). */
void mono_clock_watchdog_init(void);
