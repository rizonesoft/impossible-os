/* Pure-helper coverage for boot-perf trend median + growth-pct math.
 * Per CLAUDE.md test policy, no live boot infra calls. */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_trend.h"

static void test_median3_basic(void)
{
    TEST_ASSERT_EQ(boot_trend_median3(80, 80, 80), 80u, "all-equal -> 80");
    TEST_ASSERT_EQ(boot_trend_median3(80, 90, 100), 90u, "sorted -> middle");
    TEST_ASSERT_EQ(boot_trend_median3(100, 80, 90), 90u, "unsorted -> middle");
    TEST_ASSERT_EQ(boot_trend_median3(0, 0, 0), 0u, "all-zero -> 0");
    TEST_ASSERT_EQ(boot_trend_median3(150, 80, 80), 80u, "outlier -> drop");
}

static void test_growth_pct_zero_prior(void)
{
    TEST_ASSERT_EQ(boot_trend_compute_growth_pct(0, 100), 0u,
                   "prior=0 -> 0%% (cannot divide)");
}

static void test_growth_pct_no_growth(void)
{
    TEST_ASSERT_EQ(boot_trend_compute_growth_pct(80, 80), 0u,
                   "newest == prior -> 0%%");
    TEST_ASSERT_EQ(boot_trend_compute_growth_pct(80, 60), 0u,
                   "newest < prior -> 0%% (improvement, not regression)");
}

static void test_growth_pct_canonical_alarm_fixture(void)
{
    /* Test checkpoint: prior median 80 vs newest median 150 -> 87%
     * (the alarm scenario named in TODO-29). */
    TEST_ASSERT_EQ(boot_trend_compute_growth_pct(80, 150), 87u,
                   "80 -> 150 yields 87%% growth");
    /* Silent fixture: prior 80 vs newest 82 -> 2% (under 15%% threshold). */
    TEST_ASSERT_EQ(boot_trend_compute_growth_pct(80, 82), 2u,
                   "80 -> 82 yields 2%% (within noise band)");
}

static void test_growth_pct_threshold_boundary(void)
{
    /* At exactly 15%, boot_trend should NOT warn (strict greater). */
    TEST_ASSERT_EQ(boot_trend_compute_growth_pct(100, 115), 15u,
                   "100 -> 115 yields exactly 15%%");
    TEST_ASSERT_EQ(boot_trend_compute_growth_pct(100, 116), 16u,
                   "100 -> 116 yields 16%% (warns)");
}

void test_register_boot_trend(void)
{
    test_suite_register_cat("Boot trend: median3 truth table",
        test_median3_basic, TEST_CAT_BOOT);
    test_suite_register_cat("Boot trend: growth-pct prior=0 returns 0",
        test_growth_pct_zero_prior, TEST_CAT_BOOT);
    test_suite_register_cat("Boot trend: growth-pct improvement returns 0",
        test_growth_pct_no_growth, TEST_CAT_BOOT);
    test_suite_register_cat("Boot trend: 80->150 alarm + 80->82 silent",
        test_growth_pct_canonical_alarm_fixture, TEST_CAT_BOOT);
    test_suite_register_cat("Boot trend: 15%% threshold boundary",
        test_growth_pct_threshold_boundary, TEST_CAT_BOOT);
}

#endif
