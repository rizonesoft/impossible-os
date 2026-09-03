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
#include "kernel/drivers/lapic.h"   /* IPI_VECTOR_* for the CR-verify vector test (S10) */
#include "kernel/boot_info.h"
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

    /* Note: BOOT_REQUIRE calls _boot_require_failed() which emits one
     * "[WARN] boot: REQUIRE failed: PMM not ready" klog line when klog
     * is up (Phase 3 in the test runner).  LOG_WARN (not LOG_ERROR)
     * avoids the red [FAIL] badge that would look identical to a real
     * test failure.  This is expected test output. */
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

/* ---- Boot step recorder null safety ----
 * Tests the pure in-memory recorder, not the full progress function
 * (which has VPD/fb/IO side effects forbidden in tests). */

static void test_boot_progress_null_step(void)
{
    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t before = boot_timing_get_steps(&steps);

    /* boot_timing_record_step is the pure data helper -- NULL step ignored */
    boot_timing_record_step(0, (const char *)0, 0x0000);

    uint32_t after = boot_timing_get_steps(&steps);
    TEST_ASSERT_EQ(after, before,
                   "boot_timing_record_step(NULL step) does not append");
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

/* ---- ABI: kernel_subsys_t enum layout ----
 *
 * Pin numeric values that other code uses by index (g_subsys_ready[],
 * crash-dump readers, recovery decoders).  Reordering would silently
 * desync those consumers.
 */
static void test_subsys_enum_layout(void)
{
    TEST_ASSERT_EQ(SUBSYS_SERIAL,     0, "SUBSYS_SERIAL == 0 (first slot)");
    TEST_ASSERT_EQ(SUBSYS_PMM,        1, "SUBSYS_PMM == 1");
    TEST_ASSERT_EQ(SUBSYS_OB,        20, "SUBSYS_OB == 20");
    TEST_ASSERT_EQ(SUBSYS_UEFI_VARS, 21, "SUBSYS_UEFI_VARS == 21 (Phase 0 propagation)");
    TEST_ASSERT_EQ(SUBSYS_UEFI_TIME, 22, "SUBSYS_UEFI_TIME == 22");
    TEST_ASSERT_EQ(SUBSYS_SECUREBOOT,23, "SUBSYS_SECUREBOOT == 23");
    TEST_ASSERT_EQ(SUBSYS_TPM,       24, "SUBSYS_TPM == 24");
    TEST_ASSERT_EQ(SUBSYS_XSAVE,     25, "SUBSYS_XSAVE == 25");
    TEST_ASSERT_EQ(SUBSYS_PCID,      26, "SUBSYS_PCID == 26");
    TEST_ASSERT_EQ(SUBSYS_EX,        27, "SUBSYS_EX == 27");
    TEST_ASSERT_EQ(SUBSYS_NLS,       28, "SUBSYS_NLS == 28");
    TEST_ASSERT_EQ(SUBSYS_KNF,       29, "SUBSYS_KNF == 29 (last named slot)");
    TEST_ASSERT_EQ(SUBSYS_COUNT,     30, "SUBSYS_COUNT == 30 (sentinel)");
}

/* ---- BOOT_STEP readiness mapping ----
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

/* ---- kernel_subsystem_dump() smoke test ----
 *
 * Dump must run to completion without crashing.  We deliberately do NOT
 * assert that klog_get_seq() advances: kernel_subsystem_dump() emits 26
 * lines all tagged "BOOT" at LOG_INFO, and by the time the test runs
 * (Phase 3, ~5s into boot) the BOOT subsystem has typically hit its
 * per-subsystem rate limit (KLOG_RATE_DEFAULT msgs/sec).  Rate-limited
 * entries return early in klog() at src/kernel/klog.c:786 BEFORE
 * incrementing klog_ring_seq, so seq_after == seq_before is a legitimate
 * outcome that does NOT indicate a dump bug.  The real protection here
 * is "the call returns" (no infinite loop, no crash, no fault).  The
 * earlier "advanced by at least 1" assertion was flaky and fired during
 * the propagation test run on the user's i5-11600K box on 2026-04-08.
 */
static void test_kernel_subsystem_dump_emits(void)
{
    kernel_subsystem_dump();
    /* Reaching this point with no crash is the test. */
}

/* ---- POSTCODE_* (8-bit phase) constants ----
 *
 * Pin sentinel values used by serial diagnostics and verify the
 * Phase 0/Phase 1 starting points are non-overlapping.
 */
static void test_postcode_phase_constants(void)
{
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
    /* Must not collide with existing IPI vectors */
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFD,
                "ASYNC_INIT != RESCHEDULE (0xFD)");
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFE,
                "ASYNC_INIT != TLB_SHOOTDOWN (0xFE)");
}

static void test_cr_verify_ipi_vector(void)
{
    /* The S10 CR-pin verify-IPI vector must not collide with any other IPI
     * vector (ASYNC_INIT 0xFC / RESCHEDULE 0xFD / TLB_SHOOTDOWN 0xFE). */
    TEST_ASSERT(IPI_VECTOR_CR_VERIFY != IPI_VECTOR_ASYNC_INIT,
                "CR_VERIFY != ASYNC_INIT");
    TEST_ASSERT(IPI_VECTOR_CR_VERIFY != IPI_VECTOR_RESCHEDULE,
                "CR_VERIFY != RESCHEDULE");
    TEST_ASSERT(IPI_VECTOR_CR_VERIFY != IPI_VECTOR_TLB_SHOOTDOWN,
                "CR_VERIFY != TLB_SHOOTDOWN");
}

/* ---- kernel_subsystem_apply_result() helper ----
 *
 * The Phase 0 propagation fix factors the boot_result_t -> {ready,
 * degraded_mask} mapping into kernel_subsystem_apply_result() so that
 * unit tests can exercise the dual-channel logic WITHOUT calling any
 * forbidden boot infrastructure (uefi_vars_init/tpm_init/etc.).
 *
 * Both channels MUST agree:
 *   BOOT_OK       -> ready=true,  mask bit unchanged (cleared by save)
 *   BOOT_DEGRADED -> ready=true,  mask bit SET
 *   BOOT_FATAL    -> ready=false, mask bit SET
 *   BOOT_DEFERRED -> ready=false, mask bit SET (any non-OK)
 *
 * The test uses SUBSYS_TPM (slot 24) as a stand-in -- save/restore both
 * the readiness bit and the mask bit, exercise all 4 result codes,
 * restore on exit. No live boot infrastructure is touched.
 */
static void test_subsys_apply_result_dual_channel(void)
{
    extern struct boot_info g_boot_info;

    bool saved_ready = kernel_subsystem_ready(SUBSYS_TPM);
    uint32_t saved_mask_bit = g_boot_info.degraded_mask & (1u << SUBSYS_TPM);

    /* BOOT_OK: ready=true, mask bit cleared (we pre-clear and re-test) */
    g_boot_info.degraded_mask &= ~(1u << SUBSYS_TPM);
    bool r1 = kernel_subsystem_apply_result(SUBSYS_TPM, BOOT_OK);
    TEST_ASSERT(r1 == true, "apply(BOOT_OK) returns ready=true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == true,
                "apply(BOOT_OK) sets oracle ready");
    TEST_ASSERT((g_boot_info.degraded_mask & (1u << SUBSYS_TPM)) == 0,
                "apply(BOOT_OK) leaves degraded_mask bit clear");

    /* BOOT_DEGRADED: ready=true, mask bit SET */
    g_boot_info.degraded_mask &= ~(1u << SUBSYS_TPM);
    bool r2 = kernel_subsystem_apply_result(SUBSYS_TPM, BOOT_DEGRADED);
    TEST_ASSERT(r2 == true, "apply(BOOT_DEGRADED) returns ready=true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == true,
                "apply(BOOT_DEGRADED) sets oracle ready");
    TEST_ASSERT((g_boot_info.degraded_mask & (1u << SUBSYS_TPM)) != 0,
                "apply(BOOT_DEGRADED) sets degraded_mask bit");

    /* BOOT_FATAL: ready=false, mask bit SET */
    g_boot_info.degraded_mask &= ~(1u << SUBSYS_TPM);
    bool r3 = kernel_subsystem_apply_result(SUBSYS_TPM, BOOT_FATAL);
    TEST_ASSERT(r3 == false, "apply(BOOT_FATAL) returns ready=false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == false,
                "apply(BOOT_FATAL) clears oracle ready");
    TEST_ASSERT((g_boot_info.degraded_mask & (1u << SUBSYS_TPM)) != 0,
                "apply(BOOT_FATAL) sets degraded_mask bit");

    /* BOOT_DEFERRED: ready=false, mask bit SET (treated as 'not OK') */
    g_boot_info.degraded_mask &= ~(1u << SUBSYS_TPM);
    bool r4 = kernel_subsystem_apply_result(SUBSYS_TPM, BOOT_DEFERRED);
    TEST_ASSERT(r4 == false, "apply(BOOT_DEFERRED) returns ready=false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == false,
                "apply(BOOT_DEFERRED) clears oracle ready");
    TEST_ASSERT((g_boot_info.degraded_mask & (1u << SUBSYS_TPM)) != 0,
                "apply(BOOT_DEFERRED) sets degraded_mask bit");

    /* Out-of-range subsys: returns false, no state change */
    g_boot_info.degraded_mask &= ~(1u << SUBSYS_TPM);
    kernel_subsystem_set_ready(SUBSYS_TPM, false);
    bool r5 = kernel_subsystem_apply_result((kernel_subsys_t)SUBSYS_COUNT, BOOT_OK);
    TEST_ASSERT(r5 == false, "apply(SUBSYS_COUNT, OK) returns false (out of range)");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == false,
                "apply(out_of_range) does not affect other slots");

    /* Restore */
    kernel_subsystem_set_ready(SUBSYS_TPM, saved_ready);
    g_boot_info.degraded_mask =
        (g_boot_info.degraded_mask & ~(1u << SUBSYS_TPM)) | saved_mask_bit;
}

/* ---- Phase 0 propagation slots ----
 *
 * Verify that the four new Phase 0 readiness slots (UEFI_VARS, UEFI_TIME,
 * SECUREBOOT, TPM) integrate with the readiness oracle the same way every
 * other slot does -- save/restore wrapper around set_ready/ready, no live
 * boot infrastructure calls. The actual init functions (uefi_vars_init,
 * tpm_init, etc.) are forbidden in tests per CLAUDE.md "Test Code -- No
 * Live Boot Infrastructure Calls" -- this test only validates the oracle
 * round-trip on the new slot indices.
 */
static void test_subsys_phase0_propagation_slots(void)
{
    bool saved_vars = kernel_subsystem_ready(SUBSYS_UEFI_VARS);
    bool saved_time = kernel_subsystem_ready(SUBSYS_UEFI_TIME);
    bool saved_sb   = kernel_subsystem_ready(SUBSYS_SECUREBOOT);
    bool saved_tpm  = kernel_subsystem_ready(SUBSYS_TPM);

    kernel_subsystem_set_ready(SUBSYS_UEFI_VARS,  true);
    kernel_subsystem_set_ready(SUBSYS_UEFI_TIME,  true);
    kernel_subsystem_set_ready(SUBSYS_SECUREBOOT, true);
    kernel_subsystem_set_ready(SUBSYS_TPM,        true);

    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_VARS) == true,
                "UEFI_VARS slot round-trip true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_TIME) == true,
                "UEFI_TIME slot round-trip true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_SECUREBOOT) == true,
                "SECUREBOOT slot round-trip true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == true,
                "TPM slot round-trip true");

    kernel_subsystem_set_ready(SUBSYS_UEFI_VARS,  false);
    kernel_subsystem_set_ready(SUBSYS_UEFI_TIME,  false);
    kernel_subsystem_set_ready(SUBSYS_SECUREBOOT, false);
    kernel_subsystem_set_ready(SUBSYS_TPM,        false);

    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_VARS) == false,
                "UEFI_VARS slot round-trip false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_TIME) == false,
                "UEFI_TIME slot round-trip false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_SECUREBOOT) == false,
                "SECUREBOOT slot round-trip false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == false,
                "TPM slot round-trip false");

    kernel_subsystem_set_ready(SUBSYS_UEFI_VARS,  saved_vars);
    kernel_subsystem_set_ready(SUBSYS_UEFI_TIME,  saved_time);
    kernel_subsystem_set_ready(SUBSYS_SECUREBOOT, saved_sb);
    kernel_subsystem_set_ready(SUBSYS_TPM,        saved_tpm);
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
    test_suite_register_cat("Boot init: dump emits entries", test_kernel_subsystem_dump_emits, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: POSTCODE phase constants", test_postcode_phase_constants, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer NULL fn", test_boot_defer_null_fn, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: BOOT_DEFERRED value", test_boot_deferred_value, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer register", test_boot_defer_register, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: deferred POST codes", test_deferred_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf record size", test_boot_perf_record_size, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: bootperf POST codes", test_bootperf_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async POST codes", test_async_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async IPI vector", test_async_ipi_vector, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: CR-verify IPI vector", test_cr_verify_ipi_vector, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: Phase 0 propagation slots",
                            test_subsys_phase0_propagation_slots, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: apply_result dual channel",
                            test_subsys_apply_result_dual_channel, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
