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
#include "kernel/drivers/rtc.h"

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
}
