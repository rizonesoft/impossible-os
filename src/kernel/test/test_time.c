/* ============================================================================
 * test_time.c -- monotonic clock + time service unit tests
 *
 * Pure-logic tests of the clock-source helpers (no hardware access): the
 * PMTMR wrap-extend math and the LAPIC/PIT tick scaling. Hardware-dependent
 * behavior (live source selection, ISR-driven epoch advance) is validated on
 * WHPX / bare metal via the serial log per the section Test checkpoints.
 *
 * Started for section 2 (PMTMR clocksource); later sections extend this file
 * per the TODO-08 Unit Tests plan.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/time/mono_clock.h"
#include "kernel/time/timer_resolution.h"
#include "kernel/sched/task.h"
#include "kernel/time/wall_clock.h"
#include "kernel/time/timezone.h"
#include "kernel/time/ntp_adj.h"
#include "kernel/nt/filetime.h"
#include "kernel/drivers/rtc.h"
#include "kernel/uefi_runtime.h"
#include "libc/string.h"

/* mono_pmtmr_delta_ns: masked-delta -> ns at the fixed 3.579545 MHz PMTMR. */
static void test_pmtmr_delta_basic(void)
{
    /* Exactly one second of counts (3 579 545) converts to ~1e9 ns. */
    TEST_ASSERT_EQ(mono_pmtmr_delta_ns(0, PMTMR_FREQ_HZ, 0xFFFFFFFFu),
                   1000000000ULL, "PMTMR one second -> 1e9 ns");
    /* No advance -> zero. */
    TEST_ASSERT_EQ(mono_pmtmr_delta_ns(12345, 12345, 0xFFFFFFFFu),
                   0u, "PMTMR no delta -> 0 ns");
}

/* The 24-bit PMTMR wraps at 0x01000000; the masked subtraction must extend
 * across the wrap rather than reading a huge backward jump. */
static void test_pmtmr_delta_wrap(void)
{
    /* 0xFFFFFF -> 0x000000 is a single tick across the wrap, ~279 ns. */
    uint64_t one = mono_pmtmr_delta_ns(0x00FFFFFFu, 0x00000000u, PMTMR_24BIT_MASK);
    TEST_ASSERT(one > 0 && one < 1000, "PMTMR 24-bit wrap of 1 tick is ~279 ns");
    /* Half the 24-bit range (0x800000 counts) is ~2.34 s -- the refresh cadence
     * must stay well under this. */
    uint64_t half = mono_pmtmr_delta_ns(0u, 0x00800000u, PMTMR_24BIT_MASK);
    TEST_ASSERT(half > 2000000000ULL && half < 2700000000ULL,
                "PMTMR 24-bit half-range is ~2.34 s");
}

/* mono_lapic_ticks_to_ns: tick-count scaling for the LAPIC/PIT fallback. */
static void test_lapic_ticks_to_ns(void)
{
    TEST_ASSERT_EQ(mono_lapic_ticks_to_ns(0, 1000), 0u, "0 ticks -> 0 ns");
    TEST_ASSERT_EQ(mono_lapic_ticks_to_ns(1000, 1000), 1000000000ULL,
                   "1000 ticks at 1000 Hz -> 1 s");
    TEST_ASSERT_EQ(mono_lapic_ticks_to_ns(500, 0), 0u, "freq 0 -> 0 ns");
}

/* rtc_time_plausible: pure date-tuple validator behind the CMOS-RTC absence
 * gate. A valid tuple passes; out-of-range fields and the year-0 sentinel a
 * refused rtc_read() zero-fills must both be rejected. */
static void test_rtc_time_plausible_valid(void)
{
    struct rtc_time t = { .second = 22, .minute = 35, .hour = 14,
                          .day = 25, .month = 3, .year = 2026, .weekday = 4 };
    TEST_ASSERT(rtc_time_plausible(&t), "well-formed 2026-03-25 14:35:22 valid");
    /* FILETIME-era boundary years are valid. */
    t.year = 1601; t.month = 1; t.day = 1;
    TEST_ASSERT(rtc_time_plausible(&t), "1601-01-01 (FILETIME epoch) valid");
    /* Leap day is valid in a leap year. */
    t.year = 2024; t.month = 2; t.day = 29;
    TEST_ASSERT(rtc_time_plausible(&t), "2024-02-29 leap day valid");
}

static void test_rtc_time_plausible_rejects(void)
{
    struct rtc_time t = { .second = 0, .minute = 0, .hour = 0,
                          .day = 1, .month = 1, .year = 2026, .weekday = 1 };
    /* The year-0 sentinel a refused rtc_read() produces. */
    struct rtc_time zero = { 0, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT(!rtc_time_plausible(&zero), "year-0 zero-fill sentinel rejected");
    TEST_ASSERT(!rtc_time_plausible((const struct rtc_time *)0),
                "NULL rejected");

    t.year = 1600; TEST_ASSERT(!rtc_time_plausible(&t), "year < 1601 rejected");
    t.year = 2026; t.month = 13;
    TEST_ASSERT(!rtc_time_plausible(&t), "month 13 rejected");
    t.month = 0; TEST_ASSERT(!rtc_time_plausible(&t), "month 0 rejected");
    t.month = 4; t.day = 31;
    TEST_ASSERT(!rtc_time_plausible(&t), "April 31 rejected");
    t.month = 2; t.day = 29; t.year = 2025;
    TEST_ASSERT(!rtc_time_plausible(&t), "Feb 29 in non-leap year rejected");
    t.year = 2026; t.month = 1; t.day = 1; t.hour = 24;
    TEST_ASSERT(!rtc_time_plausible(&t), "hour 24 rejected");
    t.hour = 0; t.minute = 60;
    TEST_ASSERT(!rtc_time_plausible(&t), "minute 60 rejected");
    t.minute = 0; t.second = 60;
    TEST_ASSERT(!rtc_time_plausible(&t), "second 60 rejected");
}

/* rtc_resolve_year: trust the century byte only when advertised AND plausible;
 * a century-less register reads bus-float and must fall back to 2000+yy so a
 * working RTC is not wrongly latched unavailable. */
static void test_rtc_resolve_year(void)
{
    /* Advertised + plausible century. */
    TEST_ASSERT_EQ(rtc_resolve_year(26, 20, 1), 2026u, "century 20 + yy 26 = 2026");
    TEST_ASSERT_EQ(rtc_resolve_year(99, 21, 1), 2199u, "century 21 + yy 99 = 2199");
    /* Advertised but garbage century (0xFF BCD -> 165, binary -> 255): reject. */
    TEST_ASSERT_EQ(rtc_resolve_year(26, 165, 1), 2026u, "garbage century 165 -> 2000+yy");
    TEST_ASSERT_EQ(rtc_resolve_year(26, 255, 1), 2026u, "garbage century 255 -> 2000+yy");
    TEST_ASSERT_EQ(rtc_resolve_year(26, 0, 1), 2026u, "zero century -> 2000+yy");
    /* No century register advertised: ignore the byte entirely. */
    TEST_ASSERT_EQ(rtc_resolve_year(26, 20, 0), 2026u, "century not present -> 2000+yy");
    /* A corrupt year byte (>99) must resolve to the year-0 sentinel that
     * rtc_time_plausible() rejects -- never a fabricated plausible year. */
    TEST_ASSERT_EQ(rtc_resolve_year(126, 0, 0), 0u, "yy 126 (garbage) -> year 0");
    TEST_ASSERT_EQ(rtc_resolve_year(255, 20, 1), 0u, "yy 255 (bus-float) -> year 0");
}

/* ke_delay_interval_to_ms: NT delay-interval resolution. negative = relative
 * 100ns magnitude; positive = absolute deadline vs now; zero/expired -> 0;
 * sub-ms rounds up to 1; clamped to UINT32_MAX ms. */
static void test_ke_delay_interval_to_ms(void)
{
    /* Relative -10 ms (-100000 100ns) -> 10 ms. The pre-fix bug folded this
     * unsigned into a ~49.7-day clamp. */
    TEST_ASSERT_EQ(ke_delay_interval_to_ms(-100000, 0), 10u,
                   "relative -10ms -> 10ms");
    TEST_ASSERT_EQ(ke_delay_interval_to_ms(-10000, 0), 1u,
                   "relative -1ms -> 1ms");
    /* Sub-ms relative rounds up to 1 ms. */
    TEST_ASSERT_EQ(ke_delay_interval_to_ms(-1000, 0), 1u,
                   "relative -100us -> 1ms (round up)");
    /* Zero -> 0 (immediate). */
    TEST_ASSERT_EQ(ke_delay_interval_to_ms(0, 0), 0u, "zero -> 0");
    /* Absolute deadline in the future: 50 ms past `now`. */
    TEST_ASSERT_EQ(ke_delay_interval_to_ms((int64_t)(1000000 + 500000), 1000000),
                   50u, "absolute +50ms vs now -> 50ms");
    /* Absolute deadline already passed -> 0. */
    TEST_ASSERT_EQ(ke_delay_interval_to_ms(1000000, 2000000), 0u,
                   "absolute past deadline -> 0");
    /* Huge relative clamps to UINT32_MAX ms, never wraps. */
    TEST_ASSERT_EQ(ke_delay_interval_to_ms((int64_t)-1, 0), 1u,
                   "relative -1 (100ns) -> 1ms");
}

/* timezone: bias sign + total_bias + filetime_to/from_local round-trip,
 * placeholder pass-through, and set-range clamping. Restores UTC at the end so
 * the global tz state does not leak into other suites. */
static void test_timezone_bias(void)
{
    struct tz_info tz = { 0, 0, 0 };
    FILETIME utc = 132000000000000000ULL;  /* ~2019, well above any bias */
    const int64_t five_h = 5LL * 3600 * (int64_t)FILETIME_TICKS_PER_SECOND;

    /* US Eastern: bias -300 min (UTC-5), no DST. */
    tz.bias_minutes = -300; tz.dst_bias_minutes = 60; tz.dst_active = 0;
    timezone_set(&tz);
    TEST_ASSERT_EQ((uint64_t)timezone_total_bias(), (uint64_t)(int64_t)-300,
                   "total_bias = -300 (DST inactive)");
    TEST_ASSERT_EQ(filetime_to_local(utc), utc - five_h, "to_local UTC-5");
    TEST_ASSERT_EQ(filetime_from_local(filetime_to_local(utc)), utc,
                   "from_local inverts to_local");

    /* DST active adds the DST bias. */
    tz.dst_active = 1;
    timezone_set(&tz);
    TEST_ASSERT_EQ((uint64_t)timezone_total_bias(), (uint64_t)(int64_t)-240,
                   "total_bias = -300+60 with DST active");

    /* Placeholder passes through unconverted. */
    TEST_ASSERT_EQ(filetime_to_local(FILETIME_NOW_PLACEHOLDER),
                   (uint64_t)FILETIME_NOW_PLACEHOLDER, "to_local placeholder pass-through");

    /* Out-of-range bias clamps (does not overflow the packed field). */
    tz.bias_minutes = 99999; tz.dst_bias_minutes = 0; tz.dst_active = 0;
    timezone_set(&tz);
    TEST_ASSERT_EQ((uint64_t)timezone_total_bias(), (uint64_t)(int64_t)(24*60),
                   "bias 99999 clamped to +24h");

    /* Restore UTC. */
    tz.bias_minutes = 0; tz.dst_bias_minutes = 0; tz.dst_active = 0;
    timezone_set(&tz);
    TEST_ASSERT_EQ((uint64_t)timezone_total_bias(), 0u, "restored to UTC");
}

/* Leap second policy: FILETIME counts SI seconds (86400 s/day), leap seconds
 * are never counted. 1483228799 is 2016-12-31T23:59:59Z -- the second BEFORE
 * the 2016 leap event. POSIX skips the inserted 23:59:60, so the next second
 * (1483228800) is 2017-01-01T00:00:00Z. The string must show :59, never :60. */
static void test_filetime_leap_second_policy(void)
{
    char buf[32];
    FILETIME ft = filetime_from_unix_seconds(1483228799ULL);

    /* Linear math, no leap correction: (unix + offset) * 1e7. */
    TEST_ASSERT_EQ(ft, 131277023990000000ULL,
                   "filetime_from_unix_seconds(1483228799) is linear (no leap)");

    int n = filetime_to_string(ft, buf, sizeof(buf));
    TEST_ASSERT_EQ((uint64_t)n, 24u, "ISO string is 24 chars");

    /* Whole string must be the leap boundary second, ending :59 not :60. */
    int eq = 1;
    const char *exp = "2016-12-31T23:59:59.000Z";
    for (int i = 0; i < 25; i++) {
        if (buf[i] != exp[i]) { eq = 0; break; }
    }
    TEST_ASSERT(eq, "1483228799 renders 2016-12-31T23:59:59.000Z (never :60)");

    /* Seconds field (buf[17..18]) is 0..59 by construction. */
    TEST_ASSERT(buf[17] >= '0' && buf[17] <= '5', "seconds tens digit 0..5");

    /* The next second crosses the day boundary (POSIX ignores the leap). */
    FILETIME ft2 = filetime_from_unix_seconds(1483228800ULL);
    (void)filetime_to_string(ft2, buf, sizeof(buf));
    eq = 1;
    const char *exp2 = "2017-01-01T00:00:00.000Z";
    for (int i = 0; i < 25; i++) {
        if (buf[i] != exp2[i]) { eq = 0; break; }
    }
    TEST_ASSERT(eq, "1483228800 renders 2017-01-01T00:00:00.000Z (no leap second)");

    /* Struct converters clamp a malformed/leap ":60" onto :59 so wall time is
     * never seeded one second ahead (FILETIME_LEAP_SECOND_POLICY). */
    struct efi_time e60 = { .year = 2017, .month = 1, .day = 1, .hour = 0,
                            .minute = 0, .second = 60, .nanosecond = 0,
                            .timezone = 0x07FF };
    struct efi_time e59 = e60; e59.second = 59;
    TEST_ASSERT_EQ(filetime_from_efi_time(&e60), filetime_from_efi_time(&e59),
                   "efi_time :60 clamps to :59 (no minute roll-forward)");

    struct rtc_time r60 = { .second = 60, .minute = 0, .hour = 0, .day = 1,
                            .month = 1, .year = 2017, .weekday = 1 };
    struct rtc_time r59 = r60; r59.second = 59;
    TEST_ASSERT_EQ(filetime_from_rtc(&r60), filetime_from_rtc(&r59),
                   "rtc_time :60 clamps to :59 (no minute roll-forward)");

    /* DOS 2-second field 30/31 decode to 60/62 s; both clamp to :59 and must
     * render :59, never roll into the next minute. date=1980-01-01, time=00:00. */
    uint16_t dos_date = (uint16_t)((0u << 9) | (1u << 5) | 1u);  /* 1980-01-01 */
    FILETIME d30 = filetime_from_dos_datetime(dos_date, (uint16_t)30, 0); /* secs 60 */
    FILETIME d31 = filetime_from_dos_datetime(dos_date, (uint16_t)31, 0); /* secs 62 */
    TEST_ASSERT_EQ(d30, d31, "DOS seconds field 30 and 31 both clamp to :59");
    (void)filetime_to_string(d30, buf, sizeof(buf));
    eq = 1;
    const char *exp3 = "1980-01-01T00:00:59.000Z";
    for (int i = 0; i < 25; i++) {
        if (buf[i] != exp3[i]) { eq = 0; break; }
    }
    TEST_ASSERT(eq, "DOS malformed :60 renders 1980-01-01T00:00:59 (no roll-forward)");
}

/* Coarse time fast path: the Ke*Coarse() APIs return the value cached by the
 * last timer tick (no clocksource read). By the time tests run the BSP timer
 * ISR has been publishing, so the coarse values are live. The cache is updated
 * concurrently by the ISR, so assert only ISR-race-robust relations: coarse
 * never leads the precise live read, and coarse interrupt time is monotonic. */
/* The raw counter exposed as a WATCHDOG source.
 *
 * The property it is exported for is the one asserted: it keeps moving while
 * the selected monotonic source may not. That is why the comparison is against
 * mono_ns_coarse() rather than a value of its own -- a watchdog whose reads
 * could return the same number twice while the CPU retires instructions would
 * be as stuck as the clock it is supposed to outlive, and every bounded wait
 * built on it would lose its escape without any of them failing.
 *
 * Deliberately NOT a claim about rate, scale or per-CPU coherence: the accessor
 * promises none of those, and asserting one here would invite a caller to
 * convert it to a duration, which is exactly what its header forbids. */
static void test_mono_tsc_raw_advances(void)
{
    uint64_t a = mono_tsc_raw();
    uint64_t coarse_a = mono_ns_coarse();
    uint64_t b;
    uint32_t spins = 0;

    /* Bounded: a counter that never moves fails the assertion below rather
     * than hanging the suite, which is the same discipline the waits this
     * source backs are held to. */
    do {
        b = mono_tsc_raw();
        spins++;
    } while (b == a && spins < 4096u);

    TEST_ASSERT(b > a,
                "the watchdog counter must advance while the CPU is retiring "
                "instructions -- a source that can plateau here cannot bound "
                "a wait whose deadline clock has stopped");
    TEST_ASSERT(mono_ns_coarse() >= coarse_a,
                "and the coarse monotonic read it backstops stays "
                "non-decreasing across the same window");
}

static void test_coarse_time(void)
{
    /* Coarse interrupt time is non-decreasing across reads (ISR only advances
     * it; a plain re-read can only stay equal or grow). */
    uint64_t ci_a = KeQueryInterruptTimeCoarse();
    uint64_t ci_b = KeQueryInterruptTimeCoarse();
    TEST_ASSERT(ci_b >= ci_a, "coarse interrupt time monotonic non-decreasing");

    /* Coarse value was published at-or-before now, so the precise live read is
     * never behind it (coarse lags precise, never leads). */
    uint64_t pi = KeQueryInterruptTime();
    if (ci_b != 0)
        TEST_ASSERT(pi >= ci_b, "precise interrupt time >= coarse (coarse lags)");

    /* Same relation for system time, once the wall clock is seeded + a tick has
     * published the coarse cache (else coarse is the 0 placeholder). */
    if (time_service_ready()) {
        FILETIME cs = KeQuerySystemTimeCoarse();
        FILETIME ps = KeQuerySystemTime();
        if (cs != FILETIME_NOW_PLACEHOLDER)
            TEST_ASSERT(ps >= cs, "precise system time >= coarse (coarse lags)");
    }
}

/* NTP wall-time discipline math (pure): freq sign, slew cap + sign, no-op. */
static void test_ntp_tick_adjust(void)
{
    int64_t consumed;

    /* Frequency: positive freq_ppb = wall running fast -> negative adjust.
     * 1 s elapsed * 1000 ppb / 1e9 = 1000 ns, negated. No slew. */
    int64_t a = ntp_tick_adjust_ns(1000000000ULL, 1000, 0, &consumed);
    TEST_ASSERT_EQ((uint64_t)a, (uint64_t)(int64_t)-1000,
                   "freq +1000ppb over 1s yields -1000ns (slows a fast clock)");
    TEST_ASSERT_EQ(consumed, 0u, "no slew consumed when remaining = 0");

    /* Slew cap: 500 ppm over 10 ms = 5000 ns; huge remaining clamps to the cap. */
    a = ntp_tick_adjust_ns(10000000ULL, 0, 1000000000LL, &consumed);
    TEST_ASSERT_EQ(consumed, 5000u, "slew capped at 500ppm (5us per 10ms tick)");
    TEST_ASSERT_EQ((uint64_t)a, 5000u, "adjust == slew chunk when freq = 0");

    /* Negative slew under the cap is fully consumed (sign-aware). */
    a = ntp_tick_adjust_ns(10000000ULL, 0, -100, &consumed);
    TEST_ASSERT_EQ((uint64_t)consumed, (uint64_t)(int64_t)-100,
                   "negative slew below cap fully consumed");
    TEST_ASSERT_EQ((uint64_t)a, (uint64_t)(int64_t)-100, "adjust == -100 (freq 0)");

    /* Zero monotonic elapsed yields zero adjustment regardless of freq/slew. */
    a = ntp_tick_adjust_ns(0, 1000, 1000000000LL, &consumed);
    TEST_ASSERT_EQ((uint64_t)a, 0u, "zero elapsed yields zero adjust");
    TEST_ASSERT_EQ(consumed, 0u, "zero elapsed consumes no slew");

    /* Huge elapsed (suspend/quiesce gap) is capped at 1 s so freq*elapsed cannot
     * overflow: 1 s * 1000 ppb / 1e9 = 1000 ns, negated -- same as the 1 s case. */
    a = ntp_tick_adjust_ns(1000000000000ULL, 1000, 0, &consumed);
    TEST_ASSERT_EQ((uint64_t)a, (uint64_t)(int64_t)-1000,
                   "elapsed capped at 1s (no overflow on a quiesce gap)");

    /* At the freq clamp (NTP_FREQ_MAX_PPB = 500000 ppb), a 1 s tick nudges by
     * -500000 ns = -0.5 ms -- far below the 1 s natural advance, so wall time
     * still moves forward (never reverses). */
    a = ntp_tick_adjust_ns(1000000000ULL, NTP_FREQ_MAX_PPB, 0, &consumed);
    TEST_ASSERT_EQ((uint64_t)a, (uint64_t)(int64_t)-500000,
                   "max freq over 1s nudges -0.5ms (< 1s advance: wall stays forward)");
}

/* NTP step-target validation (pure): valid, lower-reject, magnitude-reject,
 * overflow-reject. Guards ke_ntp_adjtime's step path without a live clock set. */
static void test_ntp_step_target_valid(void)
{
    int64_t t;
    const int64_t now = 132000000000000000LL;   /* ~2019 in FILETIME 100ns units */

    /* Small +0.5 s offset against a sourced clock -> valid; target = now + 5e6. */
    TEST_ASSERT(ntp_step_target_valid(now, 500000000LL, &t),
                "small +offset is a valid step");
    TEST_ASSERT_EQ((uint64_t)t, (uint64_t)(now + 5000000LL),
                   "step target = now + 0.5s (100ns units)");

    /* Large negative offset that drives the target below the 1601 placeholder. */
    TEST_ASSERT(!ntp_step_target_valid(1000000LL, -100000000000000000LL, &t),
                "step below placeholder rejected");

    /* Offset magnitude beyond NTP_STEP_MAX_NS (eons) -> rejected. */
    TEST_ASSERT(!ntp_step_target_valid(now, 9000000000000000000LL, &t),
                "implausible offset magnitude rejected");

    /* now above the plausible FILETIME bound -> rejected before any signed math
     * (an out-of-range stored time cannot bypass via a signed cast). */
    TEST_ASSERT(!ntp_step_target_valid((uint64_t)NTP_FILETIME_MAX + 1ULL,
                                       500000000LL, &t),
                "now above NTP_FILETIME_MAX rejected");

    /* now > INT64_MAX (unsigned FILETIME) -> rejected as out-of-range, no UB. */
    TEST_ASSERT(!ntp_step_target_valid(0x8000000000000000ULL, 500000000LL, &t),
                "now > INT64_MAX rejected (unsigned-checked)");
}

/* A rejected ke_ntp_adjtime request must mutate NO NTP state (freq included).
 * An oversized step offset is rejected and has zero side effects, so the status
 * is identical before and after -- safe to exercise the live API in a test. */
static void test_ntp_adjtime_reject_preserves(void)
{
    struct ntp_status before, after;
    ke_ntp_get_status(&before);

    ntp_adj_t bad;
    bad.offset_ns = 9000000000000000000LL;  /* > NTP_STEP_MAX_NS -> step rejected */
    bad.freq_ppb  = 123456;                 /* must NOT be committed on reject */
    ke_ntp_adjtime(&bad);

    ke_ntp_get_status(&after);
    TEST_ASSERT_EQ((uint64_t)after.freq_ppb, (uint64_t)before.freq_ppb,
                   "rejected step leaves freq unchanged (no partial commit)");
    TEST_ASSERT_EQ((uint64_t)(int64_t)after.offset_ns,
                   (uint64_t)(int64_t)before.offset_ns,
                   "rejected step leaves slew unchanged");
}

/* KeSetSystemTime rejects an absurd-future absolute time, so a corrupt anchor
 * can never become the sourced wall clock (the invariant the NTP discipline +
 * interpolation rely on). A rejected set has no side effect, so calling it live
 * here is safe: the wall clock keeps its plausible value. */
static void test_set_system_time_rejects_absurd(void)
{
    KeSetSystemTime((FILETIME)(FILETIME_MAX_PLAUSIBLE + 1000000000ULL));
    TEST_ASSERT((uint64_t)KeQuerySystemTime() <= FILETIME_MAX_PLAUSIBLE,
                "KeSetSystemTime rejects absurd-future time (clock stays plausible)");
}

/* The honest discipline-state label must report "ntp" ONLY when the correction
 * is fully applied (a pure step, or a zero no-op) and "ntp-pending" whenever any
 * stored slew/freq awaits the deferred continuous discipline -- including a
 * MIXED step+freq request, whose step moved the clock but whose freq is only
 * stored. A reader must never mistake a stored-but-unapplied correction for a
 * completed sync. Pure -- no live NTP state mutated. */
static void test_ntp_source_for_correction(void)
{
    TEST_ASSERT(strcmp(ntp_source_for_correction(2000000000LL, 0), "ntp") == 0,
                "pure step (>1s, no freq) reports applied discipline (ntp)");
    TEST_ASSERT(strcmp(ntp_source_for_correction(-2000000000LL, 0), "ntp") == 0,
                "pure negative step reports applied discipline (ntp)");
    TEST_ASSERT(strcmp(ntp_source_for_correction(0, 0), "ntp") == 0,
                "zero no-op against a sourced clock reports synced (ntp)");
    TEST_ASSERT(strcmp(ntp_source_for_correction(500000000LL, 0), "ntp-pending") == 0,
                "sub-second slew reports pending (not a completed sync)");
    TEST_ASSERT(strcmp(ntp_source_for_correction(0, 1000), "ntp-pending") == 0,
                "freq-only (offset 0, nonzero freq) reports pending");
    TEST_ASSERT(strcmp(ntp_source_for_correction(2000000000LL, 1000), "ntp-pending") == 0,
                "mixed step+freq reports pending (freq application deferred)");
}

/* A non-NTP wall-clock set (KeSetSystemTime) must invalidate NTP discipline
 * state, so status can never report a stale "synced" against an anchor NTP no
 * longer tracks. Re-anchoring to the CURRENT time is non-disruptive (the wall
 * clock keeps ~its value) but must still clear source/slew/freq -- the
 * deterministic, single-thread observable of the s_ntp_lock coordination that
 * also closes the concurrent step-vs-setter race. */
static void test_set_system_time_invalidates_ntp(void)
{
    if (!wall_clock_ready()) {
        TEST_SKIP("wall clock not sourced -- cannot exercise NTP invalidation");
        return;
    }

    /* Store a pending slew + freq (not applied; continuous discipline deferred). */
    ntp_adj_t adj;
    adj.offset_ns = 250000000LL;   /* 0.25 s -> stored slew (<1 s, not a step) */
    adj.freq_ppb  = 1000;
    ke_ntp_adjtime(&adj);

    /* A manual set to ~the current time re-anchors and must wipe NTP state. */
    KeSetSystemTime(KeQuerySystemTime());

    struct ntp_status st;
    ke_ntp_get_status(&st);
    TEST_ASSERT(strcmp(st.source, "none") == 0,
                "manual KeSetSystemTime invalidates NTP source -> none");
    TEST_ASSERT_EQ((uint64_t)st.freq_ppb, 0, "manual set clears stored NTP freq");
    TEST_ASSERT_EQ((uint64_t)(int64_t)st.offset_ns, 0,
                   "manual set clears stored NTP slew");
}

/* Clocksource watchdog drift classify: mono_drift_ppm() computes the absolute
 * ppm deviation between a reference-clock delta and the active-source delta over
 * the same window. The watchdog demotes when this exceeds MONO_DRIFT_UNSTABLE_PPM.
 * Pure -- no hardware, no live clocksource state. */
static void test_mono_drift_ppm(void)
{
    /* No drift: identical deltas -> 0 ppm. */
    TEST_ASSERT_EQ((uint64_t)mono_drift_ppm(1000000000ULL, 1000000000ULL), 0,
                   "equal deltas report 0 ppm drift");
    /* Active fast by 600 ppm (src 600 ppm above ref) -> ~600 ppm, > threshold. */
    TEST_ASSERT_EQ((uint64_t)mono_drift_ppm(1000000000ULL, 1000600000ULL), 600,
                   "src 600 ppm fast classifies as 600 ppm");
    TEST_ASSERT((uint32_t)mono_drift_ppm(1000000000ULL, 1000600000ULL)
                    > MONO_DRIFT_UNSTABLE_PPM,
                "600 ppm exceeds the unstable threshold");
    /* Active slow by 600 ppm (src below ref) -> absolute value, also unstable. */
    TEST_ASSERT_EQ((uint64_t)mono_drift_ppm(1000000000ULL, 999400000ULL), 600,
                   "src 600 ppm slow classifies as 600 ppm (absolute)");
    /* A 100 ppm deviation is within tolerance (a stable clock). */
    TEST_ASSERT((uint32_t)mono_drift_ppm(1000000000ULL, 1000100000ULL)
                    <= MONO_DRIFT_UNSTABLE_PPM,
                "100 ppm stays under the unstable threshold");
    /* ref == 0 (no window) -> 0, never divides. */
    TEST_ASSERT_EQ((uint64_t)mono_drift_ppm(0, 1000000000ULL), 0,
                   "zero reference window reports 0 (no divide)");
    /* Diff >= ref saturates at 100 % (and is overflow-safe). */
    TEST_ASSERT_EQ((uint64_t)mono_drift_ppm(1000ULL, 1000000000000ULL),
                   (uint64_t)MONO_DRIFT_PPM_MAX,
                   ">=100%% drift saturates at MONO_DRIFT_PPM_MAX");
    /* Large window where diff < ref but diff * 1e6 overflows u64 (~6e21): the
     * 128-bit multiply must still report the true 600000 ppm, not a wrapped
     * value. ref=1e16 ns (~116 days), src 60 %% high. */
    TEST_ASSERT_EQ((uint64_t)mono_drift_ppm(10000000000000000ULL,
                                            16000000000000000ULL), 600000,
                   "large-window drift uses 128-bit multiply (no u64 overflow)");
}

/* The wall-time floor keeps wall reads monotonic across an NTP negative
 * re-anchor (same generation -> clamp up) while NOT clamping a legitimate
 * backward manual set (generation mismatch -> pass through, so the pre-set value
 * cannot raise the new floor). Pure -- no live wall-clock state. */
static void test_wall_floor_clamp(void)
{
    /* Same generation: a backward candidate is clamped UP to the floor. */
    TEST_ASSERT_EQ(wall_floor_clamp(900, 1000, 5, 5), 1000,
                   "same gen: backward cand clamps up to floor (monotonic)");
    /* Same generation: a forward candidate passes (and would become new floor). */
    TEST_ASSERT_EQ(wall_floor_clamp(1100, 1000, 5, 5), 1100,
                   "same gen: forward cand passes through");
    TEST_ASSERT_EQ(wall_floor_clamp(1000, 1000, 5, 5), 1000,
                   "same gen: equal cand returns floor");
    /* Generation mismatch (a manual set bumped the gen): a stale pre-set reader
     * returns its value UNCHANGED -- even a low value -- so it cannot raise the
     * new (post-set) floor. This is what lets a backward manual set stand. */
    TEST_ASSERT_EQ(wall_floor_clamp(900, 1000, 4, 5), 900,
                   "gen mismatch: low cand passes through (backward set stands)");
    TEST_ASSERT_EQ(wall_floor_clamp(1100, 1000, 4, 5), 1100,
                   "gen mismatch: high cand also passes through (no poison)");
}

/* timer_resolution_release_process: the process-exit reap of leaked timer
 * resolution requests (process exit cleanup, TODO-21). The common death-path
 * case -- a task that never requested a resolution -- must be a safe no-op that
 * returns 0 and does NOT touch the arbitrated hardware state (released==0
 * short-circuits arbitrate()). This is hardware-independent and always runs. */
static void test_timer_res_release_noop(void)
{
    TEST_ASSERT_EQ(timer_resolution_release_process(0xFFFFFFFFu), 0,
                   "release for a pid holding no request returns 0");
    TEST_ASSERT_EQ(timer_resolution_release_process(0xFFFFFFFFu), 0,
                   "repeated no-op release stays 0 (idempotent)");
}

/* Round-trip: request a fast tick as the current task, then bulk-release by
 * pid and confirm exactly one coalesced slot was reclaimed and the release is
 * idempotent. Hardware-dependent: the tick-source transition is refused on a
 * fixed-rate (PIT) backend or an AP caller, so the request never lands -- that
 * path is validated on WHPX / bare metal per the section Test checkpoint, and
 * this test SKIPs rather than widening the assertion. Self-restoring: releasing
 * the request re-arbitrates back to the prior resolution. */
static void test_timer_res_release_roundtrip(void)
{
    struct task *cur = task_current();
    uint32_t actual = 0;

    if (!cur) { TEST_SKIP("no task context"); return; }

    if (KeSetTimerResolution(TIMER_RES_MINIMUM, 1, &actual) != STATUS_SUCCESS ||
        actual != TIMER_RES_MINIMUM) {
        /* Backend refused the transition; drop any partial slot and skip. */
        (void)timer_resolution_release_process(cur->pid);
        TEST_SKIP("timer backend not re-programmable in this environment");
        return;
    }

    TEST_ASSERT_EQ(timer_resolution_release_process(cur->pid), 1,
                   "release reclaims the process's one coalesced slot");
    TEST_ASSERT_EQ(timer_resolution_release_process(cur->pid), 0,
                   "second release is an idempotent no-op");
}

void test_register_time(void)
{
    test_suite_register_cat("time: PMTMR delta basic",
                            test_pmtmr_delta_basic, TEST_CAT_SCHED);
    test_suite_register_cat("time: PMTMR delta wrap-extend",
                            test_pmtmr_delta_wrap, TEST_CAT_SCHED);
    test_suite_register_cat("time: LAPIC ticks-to-ns scaling",
                            test_lapic_ticks_to_ns, TEST_CAT_SCHED);
    test_suite_register_cat("time: rtc_time_plausible accepts valid",
                            test_rtc_time_plausible_valid, TEST_CAT_SCHED);
    test_suite_register_cat("time: rtc_time_plausible rejects invalid",
                            test_rtc_time_plausible_rejects, TEST_CAT_SCHED);
    test_suite_register_cat("time: rtc_resolve_year century handling",
                            test_rtc_resolve_year, TEST_CAT_SCHED);
    test_suite_register_cat("time: ke_delay_interval_to_ms NT semantics",
                            test_ke_delay_interval_to_ms, TEST_CAT_SCHED);
    test_suite_register_cat("time: timezone bias + local conversion",
                            test_timezone_bias, TEST_CAT_SCHED);
    test_suite_register_cat("time: FILETIME leap second policy",
                            test_filetime_leap_second_policy, TEST_CAT_SCHED);
    test_suite_register_cat("time: coarse time fast path",
                            test_coarse_time, TEST_CAT_SCHED);
    test_suite_register_cat("time: raw watchdog counter advances",
                            test_mono_tsc_raw_advances, TEST_CAT_SCHED);
    test_suite_register_cat("time: NTP tick adjustment math",
                            test_ntp_tick_adjust, TEST_CAT_SCHED);
    test_suite_register_cat("time: NTP step-target validation",
                            test_ntp_step_target_valid, TEST_CAT_SCHED);
    test_suite_register_cat("time: NTP adjtime reject preserves state",
                            test_ntp_adjtime_reject_preserves, TEST_CAT_SCHED);
    test_suite_register_cat("time: NTP source label step vs pending slew/freq",
                            test_ntp_source_for_correction, TEST_CAT_SCHED);
    test_suite_register_cat("time: manual set invalidates NTP discipline",
                            test_set_system_time_invalidates_ntp, TEST_CAT_SCHED);
    test_suite_register_cat("time: clocksource watchdog drift ppm classify",
                            test_mono_drift_ppm, TEST_CAT_SCHED);
    test_suite_register_cat("time: NTP wall-floor clamp + generation gate",
                            test_wall_floor_clamp, TEST_CAT_SCHED);
    test_suite_register_cat("time: KeSetSystemTime rejects absurd anchor",
                            test_set_system_time_rejects_absurd, TEST_CAT_SCHED);
    test_suite_register_cat("time: timer-res release-process no-op (exit cleanup)",
                            test_timer_res_release_noop, TEST_CAT_SCHED);
    test_suite_register_cat("time: timer-res release-process round-trip",
                            test_timer_res_release_roundtrip, TEST_CAT_SCHED);
}
