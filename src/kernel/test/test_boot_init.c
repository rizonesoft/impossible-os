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
#include "kernel/klog.h"

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
    /* set_ready on out-of-range index should be a no-op (not crash) */
    kernel_subsystem_set_ready(SUBSYS_COUNT, true);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_COUNT) == false,
                "set_ready(SUBSYS_COUNT) is a no-op");
}

static void test_boot_defer_null_fn(void)
{
    int ret = boot_defer("null-test", (boot_result_t (*)(void))0);
    TEST_ASSERT_EQ(ret, -1, "boot_defer rejects NULL function pointer");
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

/* ---- §1 ABI: kernel_subsys_t enum layout ----
 *
 * Pin numeric values that other code uses by index (g_subsys_ready[],
 * crash-dump readers, recovery decoders).  Reordering would silently
 * desync those consumers.
 */
static void test_subsys_enum_layout(void)
{
    TEST_ASSERT_EQ(SUBSYS_SERIAL, 0, "SUBSYS_SERIAL == 0 (first slot)");
    TEST_ASSERT_EQ(SUBSYS_PMM,    1, "SUBSYS_PMM == 1");
    TEST_ASSERT_EQ(SUBSYS_OB,    20, "SUBSYS_OB == 20 (last named slot)");
    TEST_ASSERT_EQ(SUBSYS_COUNT, 21, "SUBSYS_COUNT == 21 (sentinel)");
}

/* ---- §1 BOOT_STEP readiness mapping ----
 *
 * BOOT_STEP must mark a subsystem ready ONLY for BOOT_OK or BOOT_DEGRADED.
 * BOOT_FATAL and BOOT_DEFERRED must leave the subsystem NOT ready so
 * BOOT_REQUIRE downstream catches the failure.
 */
static boot_result_t bs_return_ok(void)       { return BOOT_OK; }
static boot_result_t bs_return_degraded(void) { return BOOT_DEGRADED; }
static boot_result_t bs_return_fatal(void)    { return BOOT_FATAL; }
static boot_result_t bs_return_deferred(void) { return BOOT_DEFERRED; }

static void test_boot_step_readiness_mapping(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    BOOT_STEP(SUBSYS_PMM, bs_return_ok);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == true,
                "BOOT_STEP marks ready on BOOT_OK");

    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    BOOT_STEP(SUBSYS_PMM, bs_return_degraded);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == true,
                "BOOT_STEP marks ready on BOOT_DEGRADED");

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    BOOT_STEP(SUBSYS_PMM, bs_return_fatal);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == false,
                "BOOT_STEP clears ready on BOOT_FATAL");

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    BOOT_STEP(SUBSYS_PMM, bs_return_deferred);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == false,
                "BOOT_STEP clears ready on BOOT_DEFERRED");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

/* ---- §1 boot_progress() happy-path side effects ----
 *
 * boot_progress(non-NULL) must record exactly one timing step with the
 * supplied phase/postcode/step pointer.  boot_progress(NULL) must NOT
 * record a timing step (it's used by tests for null safety only).
 */
static void test_boot_progress_records_step(void)
{
    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t before = boot_timing_get_steps(&steps);

    /* If the boot timing ring is already at BOOT_TIMING_MAX_STEPS=64 the test
     * cannot validate recording semantics -- record_step would early-return.
     * Fail loudly so the buffer can be enlarged or boot_progress calls trimmed,
     * rather than silently passing and masking a real recording regression. */
    TEST_ASSERT(before < 64,
                "boot_timing ring saturated before test ran -- raise "
                "BOOT_TIMING_MAX_STEPS or trim boot_progress calls");

    boot_progress(0, "VERIFY_TEST", 0xCAFE);

    uint32_t after = boot_timing_get_steps(&steps);
    TEST_ASSERT_EQ(after, before + 1,
                   "boot_progress(non-NULL) records exactly one step");

    /* Verify the recorded fields match what we passed in */
    TEST_ASSERT_EQ((int)steps[after - 1].phase, 0,
                   "recorded step phase matches");
    TEST_ASSERT_EQ((int)steps[after - 1].postcode, 0xCAFE,
                   "recorded step postcode matches");
    TEST_ASSERT(steps[after - 1].step != (const char *)0 &&
                steps[after - 1].step[0] == 'V',
                "recorded step pointer matches (starts with V)");

    /* NULL step must NOT advance the recorder (existing behavior) */
    uint32_t mid = boot_timing_get_steps(&steps);
    boot_progress(0, (const char *)0, 0xBEEF);
    uint32_t after_null = boot_timing_get_steps(&steps);
    TEST_ASSERT_EQ(after_null, mid,
                   "boot_progress(NULL) does NOT record a step");
}

/* ---- §1 kernel_subsystem_dump() smoke test ----
 *
 * Dump must run to completion and emit at least one klog entry.
 * Stricter equality (== SUBSYS_COUNT + 1) is fragile under rate limiting,
 * so we assert "advanced by at least 1" -- enough to catch a no-op regression.
 */
static void test_kernel_subsystem_dump_emits(void)
{
    uint64_t seq_before = klog_get_seq();
    kernel_subsystem_dump();
    uint64_t seq_after = klog_get_seq();
    TEST_ASSERT(seq_after > seq_before,
                "kernel_subsystem_dump emits at least one klog entry");
}

/* ---- §1 POSTCODE_* (8-bit phase) constants ----
 *
 * Pin sentinel values used by serial diagnostics and verify the
 * Phase 0/Phase 1 starting points are non-overlapping.
 */
static void test_postcode_phase_constants(void)
{
    TEST_ASSERT_EQ(POSTCODE_BOOT_OK,     0xFF, "POSTCODE_BOOT_OK == 0xFF");
    TEST_ASSERT_EQ(POSTCODE_BOOT_FAILED, 0xFE, "POSTCODE_BOOT_FAILED == 0xFE");
    TEST_ASSERT(POSTCODE_PMM_INIT  != POSTCODE_VMM_INIT,
                "POSTCODE_PMM_INIT != POSTCODE_VMM_INIT");
    TEST_ASSERT(POSTCODE_GDT_INIT  != POSTCODE_IDT_INIT,
                "POSTCODE_GDT_INIT != POSTCODE_IDT_INIT");
    TEST_ASSERT(POSTCODE_VFS_INIT  != POSTCODE_REGISTRY_INIT,
                "POSTCODE_VFS_INIT != POSTCODE_REGISTRY_INIT");
    TEST_ASSERT(POSTCODE_SERIAL_INIT < POSTCODE_GDT_INIT,
                "Phase 0 POSTCODE range below Phase 1");
    TEST_ASSERT(POSTCODE_GDT_INIT    < POSTCODE_PCI_INIT,
                "Phase 1 POSTCODE range below Phase 2");
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

/* ---- Async init POST codes and IPI vector ---- */

static void test_async_post_codes(void)
{
    TEST_ASSERT(POST16_ASYNC != 0, "POST16_ASYNC is non-zero");
    TEST_ASSERT(POST16_ASYNC_DONE != 0, "POST16_ASYNC_DONE is non-zero");
    TEST_ASSERT(POST16_ASYNC != POST16_ASYNC_AP,
                "ASYNC != ASYNC_AP");
    TEST_ASSERT(POST16_ASYNC_BARRIER != POST16_ASYNC_DONE,
                "ASYNC_BARRIER != ASYNC_DONE");
    /* No overlap with other debug ranges */
    TEST_ASSERT(POST16_ASYNC != POST16_DEFERRED,
                "ASYNC != DEFERRED");
    TEST_ASSERT(POST16_ASYNC != POST16_BOOTPERF,
                "ASYNC != BOOTPERF");
}

static void test_async_ipi_vector(void)
{
    TEST_ASSERT_EQ(IPI_VECTOR_ASYNC_INIT, 0xFC, "IPI_VECTOR_ASYNC_INIT == 0xFC");
    /* Must not collide with existing IPI vectors */
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFD,
                "ASYNC_INIT != RESCHEDULE (0xFD)");
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFE,
                "ASYNC_INIT != TLB_SHOOTDOWN (0xFE)");
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
    test_suite_register_cat("Boot init: subsys enum layout", test_subsys_enum_layout, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: BOOT_STEP mapping", test_boot_step_readiness_mapping, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: progress records step", test_boot_progress_records_step, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: dump emits entries", test_kernel_subsystem_dump_emits, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: POSTCODE phase constants", test_postcode_phase_constants, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer NULL fn", test_boot_defer_null_fn, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: BOOT_DEFERRED value", test_boot_deferred_value, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer register", test_boot_defer_register, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: deferred POST codes", test_deferred_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf record size", test_boot_perf_record_size, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf magic", test_boot_perf_header_magic, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: bootperf POST codes", test_bootperf_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async POST codes", test_async_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async IPI vector", test_async_ipi_vector, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
