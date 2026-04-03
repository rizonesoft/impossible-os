/* ============================================================================
 * test_boot_init.c -- Boot init sequencing unit tests
 *
 * Tests boot_result_t values, subsystem readiness tracking, BOOT_REQUIRE
 * macro, boot_progress() null safety, and POST code uniqueness.
 *
 * XREF: 02-kernel-core/TODO-01-kernel-init-sequencing.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_init.h"
#include "kernel/boot_timing.h"

/* ---- boot_result_t values ---- */

static void test_boot_result_values(void)
{
    TEST_ASSERT(BOOT_OK == 0, "BOOT_OK == 0");
    TEST_ASSERT(BOOT_DEGRADED == 1, "BOOT_DEGRADED == 1");
    TEST_ASSERT(BOOT_FATAL == 2, "BOOT_FATAL == 2");
}

/* ---- Subsystem readiness ---- */

static void test_subsys_set_ready_true(void)
{
    /* Save and restore state to avoid side effects */
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == true,
                "set_ready(PMM, true) then ready(PMM) returns true");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_subsys_set_ready_false(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == false,
                "set_ready(PMM, false) then ready(PMM) returns false");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_subsys_out_of_range(void)
{
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_COUNT) == false,
                "ready(SUBSYS_COUNT) returns false (out of range)");
}

/* ---- BOOT_REQUIRE macro ----
 * BOOT_REQUIRE uses `return BOOT_FATAL`, so we test it via a wrapper
 * function that has the matching return type. */

static boot_result_t require_pmm_wrapper(void)
{
    BOOT_REQUIRE(SUBSYS_PMM);
    return BOOT_OK;
}

static void test_boot_require_fails_when_not_ready(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    /* Note: BOOT_REQUIRE writes "[BOOT] REQUIRE failed: SUBSYS_PMM not ready"
     * directly to serial -- this is expected test output, not a real failure. */
    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    boot_result_t r = require_pmm_wrapper();
    TEST_ASSERT(r == BOOT_FATAL,
                "BOOT_REQUIRE returns BOOT_FATAL when PMM not ready");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_boot_require_passes_when_ready(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    boot_result_t r = require_pmm_wrapper();
    TEST_ASSERT(r == BOOT_OK,
                "BOOT_REQUIRE passes (returns BOOT_OK) when PMM ready");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

/* ---- boot_progress() null safety ---- */

static void test_boot_progress_null_step(void)
{
    /* Must not crash -- just call it and survive */
    boot_progress(0, (const char *)0, 0x0000);
    TEST_ASSERT(1, "boot_progress(NULL step) does not crash");
}

/* ---- POST code constants ---- */

static void test_post_codes_nonzero_and_unique(void)
{
    /* Spot-check representative codes from each phase are non-zero */
    TEST_ASSERT(POST16_SERIAL_OK != 0, "POST16_SERIAL_OK is non-zero");
    TEST_ASSERT(POST16_PMM_OK != 0, "POST16_PMM_OK is non-zero");
    TEST_ASSERT(POST16_GDT_OK != 0, "POST16_GDT_OK is non-zero");
    TEST_ASSERT(POST16_TIMER_OK != 0, "POST16_TIMER_OK is non-zero");
    TEST_ASSERT(POST16_VFS_OK != 0, "POST16_VFS_OK is non-zero");
    TEST_ASSERT(POST16_SCHED_OK != 0, "POST16_SCHED_OK is non-zero");

    /* Verify uniqueness across phases */
    TEST_ASSERT(POST16_PMM_OK != POST16_VMM_OK, "PMM_OK != VMM_OK");
    TEST_ASSERT(POST16_GDT_OK != POST16_IDT_OK, "GDT_OK != IDT_OK");
    TEST_ASSERT(POST16_SCHED_OK != POST16_DESKTOP_OK, "SCHED_OK != DESKTOP_OK");
    TEST_ASSERT(POST16_BOOT_OK != POST16_BOOT_FAILED, "BOOT_OK != BOOT_FAILED");
}

/* ---- BOOT_DEFERRED value ---- */

static void test_boot_deferred_value(void)
{
    TEST_ASSERT_EQ(BOOT_DEFERRED, 3, "BOOT_DEFERRED == 3");
}

/* ---- Deferred init registration ---- */

static boot_result_t deferred_test_fn(void)
{
    return BOOT_OK;
}

static void test_boot_defer_register(void)
{
    /* boot_defer returns 0 on success.
     * Note: this adds to the real deferred array, but boot_run_deferred()
     * has already run by the time tests execute, so these entries won't
     * cause problems -- they'd run only if boot_run_deferred() is called
     * again (which it won't be). */
    int r = boot_defer("test_deferred", deferred_test_fn);
    TEST_ASSERT_EQ(r, 0, "boot_defer returns 0 on success");
}

/* ---- Deferred POST codes ---- */

static void test_deferred_post_codes(void)
{
    TEST_ASSERT(POST16_DEFERRED != 0, "POST16_DEFERRED is non-zero");
    TEST_ASSERT(POST16_DEFERRED_OK != 0, "POST16_DEFERRED_OK is non-zero");
    TEST_ASSERT(POST16_DEFERRED != POST16_DEFERRED_OK,
                "DEFERRED != DEFERRED_OK");
    TEST_ASSERT(POST16_DEFERRED_NET != POST16_DEFERRED_INPUT,
                "DEFERRED_NET != DEFERRED_INPUT");
}

/* ---- Boot perf record struct ---- */

static void test_boot_perf_record_size(void)
{
    /* boot_perf_record_t must be 24 bytes for NVRAM layout stability */
    TEST_ASSERT_EQ(sizeof(boot_perf_record_t), 24,
                   "boot_perf_record_t is 24 bytes");
}

static void test_boot_perf_header_magic(void)
{
    TEST_ASSERT_EQ(BOOT_PERF_MAGIC, 0x50455246, "BOOT_PERF_MAGIC == 'PERF'");
}

/* ---- Boot perf POST codes ---- */

static void test_bootperf_post_codes(void)
{
    TEST_ASSERT(POST16_BOOTPERF != 0, "POST16_BOOTPERF is non-zero");
    TEST_ASSERT(POST16_BOOTPERF_WRITE != 0, "POST16_BOOTPERF_WRITE is non-zero");
    TEST_ASSERT(POST16_BOOTPERF != POST16_BOOTPERF_READ,
                "BOOTPERF != BOOTPERF_READ");
    TEST_ASSERT(POST16_BOOTPERF_CMP != POST16_BOOTPERF_WRITE,
                "BOOTPERF_CMP != BOOTPERF_WRITE");
    /* Confirm no overlap with deferred POST range */
    TEST_ASSERT(POST16_BOOTPERF != POST16_DEFERRED,
                "BOOTPERF != DEFERRED");
}

/* ---- Registration ---- */

void test_register_boot_init(void)
{
    test_suite_register_cat("Boot init: result values", test_boot_result_values, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: set_ready true", test_subsys_set_ready_true, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: set_ready false", test_subsys_set_ready_false, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: out of range", test_subsys_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: REQUIRE fails", test_boot_require_fails_when_not_ready, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: REQUIRE passes", test_boot_require_passes_when_ready, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: progress null", test_boot_progress_null_step, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: POST codes", test_post_codes_nonzero_and_unique, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: BOOT_DEFERRED value", test_boot_deferred_value, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer register", test_boot_defer_register, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: deferred POST codes", test_deferred_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf record size", test_boot_perf_record_size, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf magic", test_boot_perf_header_magic, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: bootperf POST codes", test_bootperf_post_codes, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
