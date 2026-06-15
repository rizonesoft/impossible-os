/* ============================================================================
 * test_acpi_power.c -- ACPI power management unit tests
 *
 * Verifies sleep state discovery, sleep entry API, and fixed event setup.
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/acpi.h"

/* ---- S-state discovery ---- */

static void test_acpi_s5_always_supported(void)
{
    TEST_ASSERT(acpi_sleep_supported(5) == 1,
                "S5 (shutdown) is always supported");
}

static void test_acpi_s5_slp_typa_valid(void)
{
    uint16_t typa = acpi_get_slp_typa(5);
    TEST_ASSERT(typa != 0xFFFF,
                "S5 SLP_TYPa is valid (not 0xFFFF)");
}

static void test_acpi_invalid_state_not_supported(void)
{
    TEST_ASSERT(acpi_sleep_supported(0) == 0,
                "S0 is not a sleep state");
    TEST_ASSERT(acpi_sleep_supported(2) == 0,
                "S2 is not supported (deprecated)");
    TEST_ASSERT(acpi_sleep_supported(6) == 0,
                "S6 does not exist");
}

static void test_acpi_invalid_state_slp_typa(void)
{
    TEST_ASSERT(acpi_get_slp_typa(0) == 0xFFFF,
                "S0 SLP_TYPa is INVALID");
    TEST_ASSERT(acpi_get_slp_typa(6) == 0xFFFF,
                "S6 SLP_TYPa is INVALID");
}

static void test_acpi_sleep_states_consistent(void)
{
    /* Every supported sleep state must expose a valid SLP_TYPa.  Walk
     * S1..S5 under a single aggregated assertion so the serial log
     * emits one PASS line instead of up to five near-duplicates. */
    uint8_t s;
    int all_valid = 1;
    for (s = 1; s <= 5; s++) {
        if (acpi_sleep_supported(s) && acpi_get_slp_typa(s) == 0xFFFF) {
            all_valid = 0;
            break;
        }
    }
    TEST_ASSERT_EQ(all_valid, 1,
                   "every supported sleep state has a valid SLP_TYPa");
}

/* ---- Sleep entry API ---- */

static void test_acpi_enter_unsupported_fails(void)
{
    /* Entering an unsupported state must return -1, not crash */
    int rc = acpi_enter_sleep_state(2);  /* S2 is deprecated/absent */
    TEST_ASSERT(rc == -1,
                "acpi_enter_sleep_state(S2) returns -1 (unsupported)");
}

static void test_acpi_enter_invalid_fails(void)
{
    int rc = acpi_enter_sleep_state(6);
    TEST_ASSERT(rc == -1,
                "acpi_enter_sleep_state(S6) returns -1 (invalid)");
}

static void test_acpi_enter_s5_rejected(void)
{
    /* S5 is soft-off, not a resumable sleep state. The generic sleep API must
     * reject it (return -1 before any SLP write or storage quiesce) so the
     * S1-S4 resume tail can never run after a storage quiesce. acpi_shutdown()
     * is the only supported S5 path. */
    int rc = acpi_enter_sleep_state(5);
    TEST_ASSERT(rc == -1,
                "acpi_enter_sleep_state(S5) returns -1 (use acpi_shutdown)");
}

/* ---- Registration ---- */

void test_register_acpi_power(void)
{
    test_suite_register_cat("ACPI: S5 always supported",
                            test_acpi_s5_always_supported, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: S5 SLP_TYPa valid",
                            test_acpi_s5_slp_typa_valid, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: invalid states not supported",
                            test_acpi_invalid_state_not_supported, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: invalid state SLP_TYPa",
                            test_acpi_invalid_state_slp_typa, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: sleep state consistency",
                            test_acpi_sleep_states_consistent, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: enter unsupported state fails",
                            test_acpi_enter_unsupported_fails, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: enter invalid state fails",
                            test_acpi_enter_invalid_fails, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI: generic sleep API rejects S5",
                            test_acpi_enter_s5_rejected, TEST_CAT_BOOT);
}

#else
void test_register_acpi_power(void) {}
#endif
