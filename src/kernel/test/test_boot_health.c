/* ============================================================================
 * test_boot_health.c -- TEST_CAT_BOOT coverage for boot_health publisher
 *
 * Tests the pure classifier (boot_health_classify_secureboot +
 * boot_health_secureboot_name) without invoking the live publish path.
 * Per CLAUDE.md "Test Code Policy", tests must NEVER call live boot
 * infrastructure -- boot_health_publish_json writes to VFS + reads from
 * every subsystem oracle, so its only test surface is the behavioural
 * checkpoint (smoke confirms `wrote X:\Diag\boot-health.json`) plus the
 * pure-helper coverage in this file.
 *
 * The classifier's priority ordering (UNKNOWN > SETUP > ENABLED > DISABLED)
 * is security-relevant: if it ever reorders to put DISABLED above UNKNOWN,
 * an unreadable Secure Boot state would silently render as DISABLED,
 * which is a false statement to operators.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_health.h"

static int streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

/* ---- Classifier priority tests ---- */

static void test_secureboot_unknown_wins(void)
{
    /* state_valid=0 -> UNKNOWN regardless of other fields. The whole point
     * of state_valid is to suppress the rest. */
    TEST_ASSERT(boot_health_classify_secureboot(0, 0, 0) == BOOT_HEALTH_SB_UNKNOWN,
                "!valid + !setup + !enabled -> UNKNOWN");
    TEST_ASSERT(boot_health_classify_secureboot(0, 1, 0) == BOOT_HEALTH_SB_UNKNOWN,
                "!valid + setup -> UNKNOWN (setup_mode does NOT override invalid)");
    TEST_ASSERT(boot_health_classify_secureboot(0, 0, 1) == BOOT_HEALTH_SB_UNKNOWN,
                "!valid + enabled -> UNKNOWN (enabled does NOT override invalid)");
    TEST_ASSERT(boot_health_classify_secureboot(0, 1, 1) == BOOT_HEALTH_SB_UNKNOWN,
                "!valid + setup + enabled -> UNKNOWN");
}

static void test_secureboot_setup_beats_enabled(void)
{
    /* SETUP mode is a transient firmware state where db/dbx/KEK can be
     * enrolled.  It must beat ENABLED so an operator sees the configuration
     * gap rather than thinking Secure Boot is fully on. */
    TEST_ASSERT(boot_health_classify_secureboot(1, 1, 1) == BOOT_HEALTH_SB_SETUP,
                "valid + setup + enabled -> SETUP (SETUP beats ENABLED)");
    TEST_ASSERT(boot_health_classify_secureboot(1, 1, 0) == BOOT_HEALTH_SB_SETUP,
                "valid + setup + !enabled -> SETUP");
}

static void test_secureboot_enabled_disabled(void)
{
    TEST_ASSERT(boot_health_classify_secureboot(1, 0, 1) == BOOT_HEALTH_SB_ENABLED,
                "valid + !setup + enabled -> ENABLED");
    TEST_ASSERT(boot_health_classify_secureboot(1, 0, 0) == BOOT_HEALTH_SB_DISABLED,
                "valid + !setup + !enabled -> DISABLED");
}

/* ---- Name decoder tests ---- */

static void test_secureboot_name_decoder(void)
{
    TEST_ASSERT(streq(boot_health_secureboot_name(BOOT_HEALTH_SB_ENABLED),
                      "ENABLED"),
                "secureboot_name(ENABLED) == \"ENABLED\"");
    TEST_ASSERT(streq(boot_health_secureboot_name(BOOT_HEALTH_SB_DISABLED),
                      "DISABLED"),
                "secureboot_name(DISABLED) == \"DISABLED\"");
    TEST_ASSERT(streq(boot_health_secureboot_name(BOOT_HEALTH_SB_SETUP),
                      "SETUP"),
                "secureboot_name(SETUP) == \"SETUP\"");
    TEST_ASSERT(streq(boot_health_secureboot_name(BOOT_HEALTH_SB_UNKNOWN),
                      "UNKNOWN"),
                "secureboot_name(UNKNOWN) == \"UNKNOWN\"");
    /* Out-of-range / corrupted enum value falls into the default arm.
     * UNKNOWN is the safe default -- never claim DISABLED for a value
     * we don't recognize. */
    TEST_ASSERT(streq(boot_health_secureboot_name(
                          (enum boot_health_secureboot_state)99),
                      "UNKNOWN"),
                "secureboot_name(garbage) defaults to \"UNKNOWN\"");
}

/* ---- Truth table -- all 8 (state_valid, setup_mode, enabled) combinations ---- *
 * Pinning the full truth table catches any future refactor that subtly
 * shifts the priority.  Each row asserts both the classified state AND
 * the rendered name to lock the security-facing contract end-to-end. */
static void test_secureboot_full_truth_table(void)
{
    struct {
        int valid, setup, enabled;
        enum boot_health_secureboot_state want;
        const char *want_name;
    } cases[] = {
        { 0, 0, 0, BOOT_HEALTH_SB_UNKNOWN,  "UNKNOWN"  },
        { 0, 0, 1, BOOT_HEALTH_SB_UNKNOWN,  "UNKNOWN"  },
        { 0, 1, 0, BOOT_HEALTH_SB_UNKNOWN,  "UNKNOWN"  },
        { 0, 1, 1, BOOT_HEALTH_SB_UNKNOWN,  "UNKNOWN"  },
        { 1, 0, 0, BOOT_HEALTH_SB_DISABLED, "DISABLED" },
        { 1, 0, 1, BOOT_HEALTH_SB_ENABLED,  "ENABLED"  },
        { 1, 1, 0, BOOT_HEALTH_SB_SETUP,    "SETUP"    },
        { 1, 1, 1, BOOT_HEALTH_SB_SETUP,    "SETUP"    },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        enum boot_health_secureboot_state got =
            boot_health_classify_secureboot(cases[i].valid,
                                            cases[i].setup,
                                            cases[i].enabled);
        TEST_ASSERT(got == cases[i].want,
                    "truth table: classify priority matches expected state");
        TEST_ASSERT(streq(boot_health_secureboot_name(got), cases[i].want_name),
                    "truth table: name decoder matches expected string");
    }
}

void test_register_boot_health(void)
{
    test_suite_register_cat("Boot health: secureboot UNKNOWN suppresses other fields",
        test_secureboot_unknown_wins, TEST_CAT_BOOT);
    test_suite_register_cat("Boot health: secureboot SETUP beats ENABLED",
        test_secureboot_setup_beats_enabled, TEST_CAT_BOOT);
    test_suite_register_cat("Boot health: secureboot ENABLED + DISABLED happy paths",
        test_secureboot_enabled_disabled, TEST_CAT_BOOT);
    test_suite_register_cat("Boot health: secureboot_name decoder + garbage default",
        test_secureboot_name_decoder, TEST_CAT_BOOT);
    test_suite_register_cat("Boot health: secureboot full 8-row truth table",
        test_secureboot_full_truth_table, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
