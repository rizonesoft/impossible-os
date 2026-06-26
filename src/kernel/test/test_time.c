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

void test_register_time(void)
{
    test_suite_register_cat("time: PMTMR delta basic",
                            test_pmtmr_delta_basic, TEST_CAT_SCHED);
    test_suite_register_cat("time: PMTMR delta wrap-extend",
                            test_pmtmr_delta_wrap, TEST_CAT_SCHED);
    test_suite_register_cat("time: LAPIC ticks-to-ns scaling",
                            test_lapic_ticks_to_ns, TEST_CAT_SCHED);
}
