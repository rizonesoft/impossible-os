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
#include "kernel/time/wall_clock.h"
#include "kernel/time/timezone.h"
#include "kernel/nt/filetime.h"
#include "kernel/drivers/rtc.h"
#include "kernel/uefi_runtime.h"

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
}
