/* ============================================================================
 * ntp_adj.h -- NTP clock adjustment hooks (wall-time discipline)
 *
 * Kernel interface for an NTP protocol client (network stack) to correct the
 * wall clock's phase (offset) and frequency (skew). The discipline applies to
 * WALL TIME ONLY: KeQuerySystemTime() is adjusted, but the monotonic clock
 * (mono_ns / mono_filetime_units / KeQueryPerformanceCounter / KeQueryInterrupt
 * Time) stays RAW and un-disciplined -- matching the Win11 contract (QPC is
 * independent of system time / Windows Time) and Linux CLOCK_MONOTONIC_RAW.
 *
 * The kernel never initiates network traffic; the NTP client calls
 * ke_ntp_adjtime() from THREAD (PASSIVE) context only.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/filetime.h"

/* Adjustment request passed to ke_ntp_adjtime(). */
typedef struct ntp_adj {
    int64_t offset_ns;   /* signed wall-clock phase correction.
                          * |offset| >  1 s  -> applied as an immediate step.
                          * |offset| <= 1 s  -> slewed in gradually (bounded). */
    int32_t freq_ppb;    /* parts-per-billion frequency correction; positive =
                          * wall clock currently running FAST (slow it down). */
} ntp_adj_t;

/* Status snapshot returned by ke_ntp_get_status(). */
struct ntp_status {
    int64_t  offset_ns;   /* slew remaining (un-consumed phase correction) */
    int32_t  freq_ppb;    /* current frequency correction in effect */
    FILETIME last_sync;   /* wall time of the most recent ke_ntp_adjtime() */
    const char *source;   /* discipline state: "ntp" = the clock is disciplined --
                           * a STEP moved it, OR the continuous applier is actively
                           * applying a stored slew/freq (so {freq_ppb != 0,
                           * source="ntp"} is the NORMAL ongoing-discipline state);
                           * "ntp-pending" = a slew/freq is accepted but the applier
                           * has NOT yet applied it (must not be read as synced);
                           * "none" = no correction / a manual set cleared it. */
};

/* Apply an NTP phase/frequency correction. THREAD (PASSIVE) context only.
 * offset > 1 s steps the wall clock (KeSetSystemTime) NOW and clears any pending
 * slew; offset <= 1 s stores a gradual slew. freq_ppb is always STORED, then
 * applied continuously per-tick by the wall-time discipline applier.
 * STORE-TIME status (this is what ntp_source_for_correction returns at accept):
 * "ntp" for a pure step (or a zero no-op) with zero freq; ANY stored slew OR
 * nonzero freq, INCLUDING a mixed step+freq, is "ntp-pending" (accepted but not
 * yet applied). The per-tick applier then flips "ntp-pending" -> "ntp" once it
 * starts applying the correction (so an in-effect freq with source="ntp" is the
 * normal disciplined state). The monotonic clock is never touched. */
void ke_ntp_adjtime(const ntp_adj_t *adj);

/* Fill *out with the current discipline state. NULL out is ignored. The read is
 * a COHERENT snapshot: it takes the same NTP writer lock ke_ntp_adjtime() holds
 * over its whole accept path, so the returned {offset, freq, last_sync, source}
 * always belong to one accepted request (never a torn mix). */
void ke_ntp_get_status(struct ntp_status *out);

/* Slew rate cap in parts-per-million (Linux adjtime default). */
#define NTP_SLEW_MAX_PPM  500

/* Frequency-correction clamp (parts-per-billion). 500 ppm is generous vs real
 * NTP (typically < 100 ppm) and keeps the per-tick wall nudge under 0.05 % of
 * the natural monotonic advance, so even a hostile network-derived freq_ppb can
 * never make the wall clock run backward. */
#define NTP_FREQ_MAX_PPB  500000

/* Maximum plausible NTP step offset (ns). NTP corrects drift (sub-second to,
 * at most, an RTC-wrong-by-years initial sync), never centuries -- 10 years is
 * a generous upper bound that rejects a hostile/garbage offset. */
#define NTP_STEP_MAX_NS   315576000000000000LL   /* 10 * 365.25 d in ns */

/* Absurd-future sanity bound for an NTP step TARGET -- the shared wall-time
 * plausibility bound (~year 4760), which KeSetSystemTime() also enforces. */
#define NTP_FILETIME_MAX  ((int64_t)FILETIME_MAX_PLAUSIBLE)

/* Pure: validate an NTP phase step. now_ft is the current wall time (UNSIGNED
 * FILETIME); it is range-checked (> FILETIME_NOW_PLACEHOLDER and <=
 * NTP_FILETIME_MAX) BEFORE any signed arithmetic so an out-of-range stored time
 * cannot bypass the model via an implementation-defined cast. Returns 1 and
 * writes *target = now_ft + off_ns/100 when off is a plausible magnitude
 * (|off_ns| <= NTP_STEP_MAX_NS) and the target stays in (placeholder,
 * NTP_FILETIME_MAX], else 0 (caller rejects). Side-effect-free -- unit-tested. */
int ntp_step_target_valid(uint64_t now_ft, int64_t off_ns, int64_t *target);

/* Pure: the wall-clock adjustment (ns) to apply this tick for a given monotonic
 * elapsed, frequency correction, and remaining phase slew. Returns freq + slew
 * contribution; *slew_consumed (if non-NULL) receives the slew ns taken this
 * tick (sign-aware, capped at NTP_SLEW_MAX_PPM). Side-effect-free -- unit-tested
 * directly. Positive freq_ppb (wall fast) yields a negative freq contribution.
 * This is the discipline math; the continuous per-tick application (behind the
 * wall-time floor) is driven by the NTP wall-time discipline applier. */
int64_t ntp_tick_adjust_ns(uint64_t elapsed_ns, int32_t freq_ppb,
                           int64_t slew_remaining, int64_t *slew_consumed);

/* Step vs slew threshold: |offset| > 1 s is applied as an immediate step,
 * otherwise stored as a gradual slew. */
#define NTP_STEP_THRESHOLD_NS  1000000000LL

/* Pure: the STORE-TIME discipline-state label for a just-accepted correction (at
 * the moment ke_ntp_adjtime stores it, BEFORE the per-tick applier has run).
 * Reports "ntp" only when the correction is already in effect at store time -- a
 * pure step (|off| > NTP_STEP_THRESHOLD_NS that moved the wall clock NOW) with no
 * residual stored slew/freq, or a zero no-op. ANY stored-but-not-yet-applied
 * correction -- a sub-second slew OR a nonzero freq_ppb -- reports "ntp-pending"
 * so a reader does not treat it as synced before the applier consumes it; the
 * applier flips it to "ntp" once applying. Side-effect-free, unit-tested.
 * freq_ppb is the CLAMPED value that will be stored (matches ntp_status). */
const char *ntp_source_for_correction(int64_t off_ns, int32_t freq_ppb);
