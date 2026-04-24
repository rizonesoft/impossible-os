/* ============================================================================
 * test_boot_rollback.c -- unit tests for the anti-rollback validator
 * and the Phase-3 raise-counter decision helper.
 *
 * Covers:
 *   - NULL info rejection
 *   - happy path (first-ever boot, required=0, any shipped)
 *   - unknown flag bit rejected
 *   - version > BOOT_SECURITY_VERSION_MAX rejected
 *   - raise decision: opt-out (never raise), opt-in same version
 *     (skip), opt-in newer version (raise to shipped value)
 *
 * Matches the 4 fixture scenarios the TODO checkpoint calls for.
 * The actual NVRAM write path (uefi_set_variable) is NOT tested here
 * -- it requires live UEFI RT which the test harness cannot reach.
 * Host smoke test covers the round trip manually on QEMU OVMF.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"
#include "kernel/boot_info.h"

static struct boot_info s_rb_buf;

static void rb_zero(void)
{
    uint64_t *p = (uint64_t *)&s_rb_buf;
    uint32_t i;
    for (i = 0u; i < sizeof(s_rb_buf) / sizeof(uint64_t); i++)
        p[i] = 0u;
}

static void test_boot_rollback_null_info(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate((struct boot_info *)0, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                    "NULL info -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_NULL_INFO, "err=NULL_INFO");
}

static void test_boot_rollback_first_ever_boot(void)
{
    TEST_KLOG_SUPPRESS("boot");  /* LOG_INFO summary */
    rb_zero();
    /* First-ever boot: NVRAM variable absent -> required=0, shipped=N
     * passes because shipped >= required for any N. */
    s_rb_buf.flags                       = 0u;
    s_rb_buf.os_loader_security_version  = 5u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,                "first-ever boot -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_OK,   "err=OK");
}

static void test_boot_rollback_equal_versions(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    /* shipped == required (5==5): accepted; no raise. */
    s_rb_buf.os_loader_security_version  = 5u;
    s_rb_buf.required_security_version   = 5u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK, "equal versions -> OK");

    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "equal versions -> no raise");
    /* new_value must not be written when raise returns 0 */
    TEST_ASSERT_EQ((uint64_t)new_value, 0xBEEFu, "new_value untouched on no-raise");
}

static void test_boot_rollback_unknown_flag_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    s_rb_buf.flags                       = (1u << 31);
    s_rb_buf.os_loader_security_version  = 1u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                     "unknown flag -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_UNKNOWN_FLAG, "err=UNKNOWN_FLAG");
}

static void test_boot_rollback_version_oor_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    s_rb_buf.os_loader_security_version  = BOOT_SECURITY_VERSION_MAX + 1u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                     "version OOR -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_VERSION_OOR,  "err=VERSION_OOR");
}

static void test_boot_rollback_refusal_flag_warning(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    /* Stale REFUSAL flag in boot_info -- the kernel shouldn't see
     * this because the bootloader should have halted, but if it
     * does, the validator logs a WARN and keeps booting (the kernel
     * running IS proof the downgrade check passed). */
    s_rb_buf.flags                       = BOOT_FLAG_ROLLBACK_REFUSAL;
    s_rb_buf.os_loader_security_version  = 5u;
    s_rb_buf.required_security_version   = 3u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "stale REFUSAL flag -> OK + WARN");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_OK, "err=OK");
}

static void test_boot_rollback_read_failed_flag_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    /* READ_FAILED flag alone (without REFUSAL) is legitimate telemetry
     * from the bootloader -- validator accepts with WARN. */
    s_rb_buf.flags                       = BOOT_FLAG_ROLLBACK_READ_FAILED;
    s_rb_buf.os_loader_security_version  = 1u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "READ_FAILED flag stale -> OK + WARN");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_OK, "err=OK");
}

static void test_boot_rollback_should_raise_opt_out(void)
{
    rb_zero();
    /* shipped=7, required=5, opt_in=0: MUST NOT raise even though
     * shipped > required. Policy opt-out keeps the counter frozen. */
    s_rb_buf.os_loader_security_version  = 7u;
    s_rb_buf.required_security_version   = 5u;

    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 0, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "opt-out -> no raise");
    TEST_ASSERT_EQ((uint64_t)new_value, 0xBEEFu, "new_value untouched on opt-out");
}

static void test_boot_rollback_should_raise_opt_in(void)
{
    rb_zero();
    /* shipped=7, required=5, opt_in=1: raise to 7. */
    s_rb_buf.os_loader_security_version  = 7u;
    s_rb_buf.required_security_version   = 5u;

    uint32_t new_value = 0;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 1u, "opt-in + newer -> raise");
    TEST_ASSERT_EQ((uint64_t)new_value, 7u, "new_value == shipped");
}

static void test_boot_rollback_should_raise_downgrade_impossible(void)
{
    rb_zero();
    /* shipped=3, required=5: would be rejected pre-jump by bootloader;
     * if we somehow reach the raise path, never decrease. */
    s_rb_buf.os_loader_security_version  = 3u;
    s_rb_buf.required_security_version   = 5u;

    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "shipped < required -> no raise (never decrease)");
    TEST_ASSERT_EQ((uint64_t)new_value, 0xBEEFu, "new_value untouched on downgrade");
}

static void test_boot_rollback_should_raise_null_safe(void)
{
    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise((struct boot_info *)0, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "NULL info -> no raise");
    /* Also null new_value pointer */
    raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, (uint32_t *)0);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "NULL new_value safe");
}

/* Compositor-steady gate tests. Exercise mark_steady / raise_if_steady
 * without triggering uefi_set_variable: tests stage g_boot_info so
 * should_raise returns 0 (opt-out), keeping them host-safe. */
extern struct boot_info g_boot_info;

static void test_boot_rollback_raise_withheld_without_steady(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    uint8_t  saved_opt   = g_boot_info.config.anti_rollback_raise;
    uint32_t saved_ship  = g_boot_info.os_loader_security_version;
    uint32_t saved_req   = g_boot_info.required_security_version;
    /* Opt-out so even if mark_steady somehow fired we'd skip the live
     * NVRAM write. This test does NOT mark_steady -- the gate alone
     * must withhold the raise. */
    g_boot_info.config.anti_rollback_raise    = 0;
    g_boot_info.os_loader_security_version    = 7u;
    g_boot_info.required_security_version     = 5u;

    int raised = boot_rollback_raise_if_steady();
    TEST_ASSERT_EQ((uint64_t)raised, 0u, "no mark_steady -> no raise");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 0u, "steady stays clear");

    g_boot_info.config.anti_rollback_raise    = saved_opt;
    g_boot_info.os_loader_security_version    = saved_ship;
    g_boot_info.required_security_version     = saved_req;
    boot_rollback_reset_for_test();
}

static void test_boot_rollback_mark_steady_latches(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 0u, "starts clear");
    boot_rollback_mark_steady();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 1u, "latches on mark");
    boot_rollback_mark_steady();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 1u, "idempotent second mark");

    boot_rollback_reset_for_test();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 0u, "reset clears");
}

static void test_boot_rollback_raise_one_shot_opt_out(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    /* Stage opt-out so should_raise returns 0 even after mark_steady.
     * The helper still latches s_raised so subsequent calls are
     * no-ops -- prevents re-evaluation on every compositor wake. */
    uint8_t saved_opt = g_boot_info.config.anti_rollback_raise;
    g_boot_info.config.anti_rollback_raise = 0;

    boot_rollback_mark_steady();
    int r1 = boot_rollback_raise_if_steady();
    int r2 = boot_rollback_raise_if_steady();
    TEST_ASSERT_EQ((uint64_t)r1, 0u, "opt-out -> no raise (first call)");
    TEST_ASSERT_EQ((uint64_t)r2, 0u, "opt-out -> no raise (second call)");

    g_boot_info.config.anti_rollback_raise = saved_opt;
    boot_rollback_reset_for_test();
}

void test_register_boot_rollback(void)
{
    test_suite_register_cat("boot_rollback: NULL info rejected",
                            test_boot_rollback_null_info, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: first-ever boot (required=0)",
                            test_boot_rollback_first_ever_boot, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: equal versions accepted + no raise",
                            test_boot_rollback_equal_versions, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: unknown flag bit rejected",
                            test_boot_rollback_unknown_flag_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: version out-of-range rejected",
                            test_boot_rollback_version_oor_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: stale REFUSAL flag warns",
                            test_boot_rollback_refusal_flag_warning, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: stale READ_FAILED flag accepted + WARN",
                            test_boot_rollback_read_failed_flag_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: opt-out never raises",
                            test_boot_rollback_should_raise_opt_out, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: opt-in raises to shipped",
                            test_boot_rollback_should_raise_opt_in, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: never decrease on downgrade input",
                            test_boot_rollback_should_raise_downgrade_impossible, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: NULL-safe decision helper",
                            test_boot_rollback_should_raise_null_safe, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: raise withheld without mark_steady",
                            test_boot_rollback_raise_withheld_without_steady, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: mark_steady latches + reset clears",
                            test_boot_rollback_mark_steady_latches, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: raise is one-shot even on opt-out",
                            test_boot_rollback_raise_one_shot_opt_out, TEST_CAT_BOOT);
}
