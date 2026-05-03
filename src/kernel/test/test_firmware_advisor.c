/* ============================================================================
 * test_firmware_advisor.c -- TEST_CAT_BOOT coverage for firmware_advisor
 *
 * Exercises the public oracle surface (status names, severity names, cache-
 * state names, bounds-checked accessors) without invoking the live init
 * path -- per CLAUDE.md "Test Code Policy", tests must NEVER call live boot
 * infrastructure (firmware_advisor_init writes to the registry + VFS, which
 * the test harness cannot scratch around).
 *
 * The internal classification logic (fa_classify) is the load-bearing part
 * we'd most want to test, but it's static to firmware_advisor.c.  Promoting
 * it to a public test seam would broaden the published API for one test --
 * the practical alternative is QEMU OVMF integration coverage, which is
 * already exercised by the smoke test (sysinfo.exe firmware-updates renders
 * the empty-ESRT path on every boot).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/firmware_advisor.h"

/* Tiny inline strcmp -- freestanding kernel test code, no <string.h>.
 * Returns 1 on equality, 0 otherwise.  Convention matches the rest of
 * the freestanding test code (boot_caps test_streq, fat32_strcasecmp). */
static int streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

/* ---- Decoder helpers -- pure tables, safe to exercise ---- */

static void test_advisor_status_names(void)
{
    TEST_ASSERT(streq(firmware_advisor_status_name(FW_ADVISOR_STATUS_UP_TO_DATE),
                      "up_to_date"),
                "status_name(UP_TO_DATE) == \"up_to_date\"");
    TEST_ASSERT(streq(firmware_advisor_status_name(FW_ADVISOR_STATUS_UPDATE_AVAILABLE),
                      "update_available"),
                "status_name(UPDATE_AVAILABLE) == \"update_available\"");
    TEST_ASSERT(streq(firmware_advisor_status_name(FW_ADVISOR_STATUS_UNKNOWN),
                      "unknown"),
                "status_name(UNKNOWN) == \"unknown\"");
    /* Out-of-range value falls into the default arm -- contract. */
    TEST_ASSERT(streq(firmware_advisor_status_name((enum firmware_advisor_status)99),
                      "unknown"),
                "status_name(garbage) defaults to \"unknown\"");
}

static void test_advisor_severity_names(void)
{
    TEST_ASSERT(streq(firmware_advisor_severity_name(FW_ADVISOR_SEVERITY_CRITICAL),
                      "critical"),
                "severity_name(CRITICAL) == \"critical\"");
    TEST_ASSERT(streq(firmware_advisor_severity_name(FW_ADVISOR_SEVERITY_RECOMMENDED),
                      "recommended"),
                "severity_name(RECOMMENDED) == \"recommended\"");
    TEST_ASSERT(streq(firmware_advisor_severity_name(FW_ADVISOR_SEVERITY_NONE),
                      ""),
                "severity_name(NONE) is empty string");
}

static void test_advisor_cache_state_names(void)
{
    TEST_ASSERT(streq(firmware_advisor_cache_state_name(FW_ADVISOR_CACHE_LOADED),
                      "loaded"),
                "cache_state_name(LOADED) == \"loaded\"");
    TEST_ASSERT(streq(firmware_advisor_cache_state_name(FW_ADVISOR_CACHE_MALFORMED),
                      "malformed"),
                "cache_state_name(MALFORMED) == \"malformed\"");
    TEST_ASSERT(streq(firmware_advisor_cache_state_name(FW_ADVISOR_CACHE_MISSING),
                      "missing"),
                "cache_state_name(MISSING) == \"missing\"");
}

/* ---- Bounds-checked accessors ---- */

static void test_advisor_oracle_bounds(void)
{
    /* These oracles return UNKNOWN/NONE for OOR by contract.  The advisor
     * may or may not have run by test time (init runs in Phase 3 desktop
     * setup; tests run after Phase 3 EXEC subsystem registration); the
     * out-of-range checks are the universal property. */
    TEST_ASSERT(firmware_advisor_status_by_index(0xFFFFFFFFu)
                == FW_ADVISOR_STATUS_UNKNOWN,
                "status_by_index(huge) returns UNKNOWN");
    TEST_ASSERT(firmware_advisor_severity_by_index(0xFFFFFFFFu)
                == FW_ADVISOR_SEVERITY_NONE,
                "severity_by_index(huge) returns NONE");

    /* count_with_status on a bogus enum value returns 0 (no entries match
     * a value the classifier never produces). */
    TEST_ASSERT(firmware_advisor_count_with_status(
                    (enum firmware_advisor_status)99) == 0,
                "count_with_status(garbage) returns 0");
}

/* ---- Symbol-existence tests for the refusal sentinel ---- *
 * The firmware_capsule_refused.c TU defines four symbols in the
 * .firmware_capsule_refused linker section.  Verify they exist (linker
 * would have stripped them otherwise) and carry their sentinel values.
 * Any future contributor who tries to delete the sentinel breaks these
 * checks before they break the policy. */

extern const uint8_t UpdateCapsule;
extern const uint8_t capsule_update_request;
extern const uint8_t capsule_submit;
extern const uint8_t firmware_capsule_refusal_sentinel;

static void test_capsule_refusal_sentinel_present(void)
{
    /* The four sentinel symbols all carry well-known values.  Reading
     * them via the address operator forces the linker to keep them in
     * the final image; if anyone deletes the sentinel TU, the kernel
     * fails to link and these tests never get a chance to run. */
    TEST_ASSERT(UpdateCapsule == 0,
                "UpdateCapsule sentinel == 0");
    TEST_ASSERT(capsule_update_request == 0,
                "capsule_update_request sentinel == 0");
    TEST_ASSERT(capsule_submit == 0,
                "capsule_submit sentinel == 0");
    TEST_ASSERT(firmware_capsule_refusal_sentinel == 0xFE,
                "anchor sentinel carries 0xFE marker");
}

void test_register_firmware_advisor(void)
{
    test_suite_register_cat("Advisor: status_name decoder",
        test_advisor_status_names, TEST_CAT_BOOT);
    test_suite_register_cat("Advisor: severity_name decoder",
        test_advisor_severity_names, TEST_CAT_BOOT);
    test_suite_register_cat("Advisor: cache_state_name decoder",
        test_advisor_cache_state_names, TEST_CAT_BOOT);
    test_suite_register_cat("Advisor: oracle bounds (OOR -> UNKNOWN/NONE)",
        test_advisor_oracle_bounds, TEST_CAT_BOOT);
    test_suite_register_cat("Advisor: capsule refusal sentinel symbols present",
        test_capsule_refusal_sentinel_present, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
