/* ============================================================================
 * test_boot_version.c -- tests for the boot-protocol version-negotiation
 * fault classifier + fault record layout.
 *
 * Covers:
 *   - classify happy path (matching magic/version/size) -> BOOT_OK.
 *   - classify rejects NULL header with NULL_HDR class.
 *   - classify rejects bad magic.
 *   - classify rejects bad version.
 *   - classify rejects bad size.
 *   - fault record is 48 bytes (ABI pin, NVRAM layout).
 *   - fault_class_name maps every enum value to a non-empty string.
 *
 * render_fatal is NOT tested here because it would terminate the running
 * kernel; persist_nvram and blackbox_transcribe are not exercised
 * because they go through the live uefi_runtime layer which requires a
 * real firmware context.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/boot_version.h"
#include "kernel/boot_info.h"

static void test_boot_version_classify_happy(void)
{
    struct boot_info_header hdr;
    struct boot_version_fault fault;
    hdr.magic   = BOOT_INFO_MAGIC;
    hdr.version = BOOT_INFO_VERSION;
    hdr.size    = (uint16_t)sizeof(struct boot_info);

    boot_result_t r = boot_version_classify(&hdr,
                                            sizeof(struct boot_info),
                                            &fault);
    TEST_ASSERT_EQ((int)r, (int)BOOT_OK, "matching header -> BOOT_OK");
    TEST_ASSERT_EQ((unsigned long)fault.fault_class,
                   (unsigned long)BOOT_VERSION_OK,
                   "fault_class == OK on match");
    TEST_ASSERT_EQ((unsigned long)fault.observed_magic,
                   (unsigned long)BOOT_INFO_MAGIC,
                   "observed_magic captured");
    TEST_ASSERT_EQ((unsigned long)fault.expected_version,
                   (unsigned long)BOOT_INFO_VERSION,
                   "expected_version pinned");
}

static void test_boot_version_classify_null_hdr(void)
{
    struct boot_version_fault fault;
    boot_result_t r = boot_version_classify((const struct boot_info_header *)0,
                                            sizeof(struct boot_info),
                                            &fault);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "NULL hdr -> BOOT_FATAL");
    TEST_ASSERT_EQ((unsigned long)fault.fault_class,
                   (unsigned long)BOOT_VERSION_FAULT_NULL_HDR,
                   "fault_class == NULL_HDR");
}

static void test_boot_version_classify_bad_magic(void)
{
    struct boot_info_header hdr;
    struct boot_version_fault fault;
    hdr.magic   = 0xDEADBEEFu;  /* not BOOT_INFO_MAGIC */
    hdr.version = BOOT_INFO_VERSION;
    hdr.size    = (uint16_t)sizeof(struct boot_info);

    boot_result_t r = boot_version_classify(&hdr,
                                            sizeof(struct boot_info),
                                            &fault);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "bad magic -> BOOT_FATAL");
    TEST_ASSERT_EQ((unsigned long)fault.fault_class,
                   (unsigned long)BOOT_VERSION_FAULT_BAD_MAGIC,
                   "fault_class == BAD_MAGIC");
    TEST_ASSERT_EQ((unsigned long)fault.observed_magic,
                   (unsigned long)0xDEADBEEFu,
                   "observed_magic captured");
    TEST_ASSERT_EQ((unsigned long)fault.expected_magic,
                   (unsigned long)BOOT_INFO_MAGIC,
                   "expected_magic pinned");
}

static void test_boot_version_classify_bad_version(void)
{
    struct boot_info_header hdr;
    struct boot_version_fault fault;
    hdr.magic   = BOOT_INFO_MAGIC;
    hdr.version = (uint16_t)(BOOT_INFO_VERSION - 1u);  /* stale bootloader */
    hdr.size    = (uint16_t)sizeof(struct boot_info);

    boot_result_t r = boot_version_classify(&hdr,
                                            sizeof(struct boot_info),
                                            &fault);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "stale version -> BOOT_FATAL");
    TEST_ASSERT_EQ((unsigned long)fault.fault_class,
                   (unsigned long)BOOT_VERSION_FAULT_BAD_VERSION,
                   "fault_class == BAD_VERSION");
    TEST_ASSERT_EQ((unsigned long)fault.observed_version,
                   (unsigned long)(BOOT_INFO_VERSION - 1u),
                   "observed_version captured");
}

static void test_boot_version_classify_bad_size(void)
{
    struct boot_info_header hdr;
    struct boot_version_fault fault;
    hdr.magic   = BOOT_INFO_MAGIC;
    hdr.version = BOOT_INFO_VERSION;
    hdr.size    = (uint16_t)(sizeof(struct boot_info) - 8u);  /* struct shrunk */

    boot_result_t r = boot_version_classify(&hdr,
                                            sizeof(struct boot_info),
                                            &fault);
    TEST_ASSERT_EQ((int)r, (int)BOOT_FATAL, "wrong size -> BOOT_FATAL");
    TEST_ASSERT_EQ((unsigned long)fault.fault_class,
                   (unsigned long)BOOT_VERSION_FAULT_BAD_SIZE,
                   "fault_class == BAD_SIZE");
    TEST_ASSERT_EQ((unsigned long)fault.observed_size,
                   (unsigned long)(sizeof(struct boot_info) - 8u),
                   "observed_size captured");
    TEST_ASSERT_EQ((unsigned long)fault.expected_size,
                   (unsigned long)sizeof(struct boot_info),
                   "expected_size pinned");
}

static void test_boot_version_record_size_pin(void)
{
    TEST_ASSERT_EQ((unsigned long)sizeof(struct boot_version_fault),
                   (unsigned long)48,
                   "boot_version_fault is 48 bytes (NVRAM ABI pin)");
}

static void test_boot_version_fault_class_name_coverage(void)
{
    const char *n;
    /* Every declared fault class must produce a non-empty string so
     * klog and BlackBox transcripts never show "(null)" for a valid
     * enum value. */
    n = boot_version_fault_class_name(BOOT_VERSION_OK);
    TEST_ASSERT_EQ((unsigned long)(n[0] != '\0' ? 1 : 0),
                   (unsigned long)1, "name(OK) non-empty");
    n = boot_version_fault_class_name(BOOT_VERSION_FAULT_NULL_HDR);
    TEST_ASSERT_EQ((unsigned long)(n[0] != '\0' ? 1 : 0),
                   (unsigned long)1, "name(NULL_HDR) non-empty");
    n = boot_version_fault_class_name(BOOT_VERSION_FAULT_BAD_MAGIC);
    TEST_ASSERT_EQ((unsigned long)(n[0] != '\0' ? 1 : 0),
                   (unsigned long)1, "name(BAD_MAGIC) non-empty");
    n = boot_version_fault_class_name(BOOT_VERSION_FAULT_BAD_VERSION);
    TEST_ASSERT_EQ((unsigned long)(n[0] != '\0' ? 1 : 0),
                   (unsigned long)1, "name(BAD_VERSION) non-empty");
    n = boot_version_fault_class_name(BOOT_VERSION_FAULT_BAD_SIZE);
    TEST_ASSERT_EQ((unsigned long)(n[0] != '\0' ? 1 : 0),
                   (unsigned long)1, "name(BAD_SIZE) non-empty");
    n = boot_version_fault_class_name(BOOT_VERSION_FAULT_SEC_ROLLBACK);
    TEST_ASSERT_EQ((unsigned long)(n[0] != '\0' ? 1 : 0),
                   (unsigned long)1, "name(SEC_ROLLBACK) non-empty");
    /* Unknown value falls back to "UNKNOWN" (also non-empty). */
    n = boot_version_fault_class_name(0xFFFFFFFFu);
    TEST_ASSERT_EQ((unsigned long)(n[0] != '\0' ? 1 : 0),
                   (unsigned long)1, "name(garbage) non-empty");
}

/* Anti-rollback operator-hint coverage. The full fatal-screen render
 * AND the BlackBox transcript both defer to
 * boot_version_fault_operator_hint() for their operator-response
 * wording, so testing the hint helper pins both paths against wording
 * drift. The fatal-render function itself is not called here because
 * it terminates via a halt sink (forbidden in tests per CLAUDE.md),
 * and the transcript writer is skipped because it writes a file via
 * VFS. Exercising the pure helper is the only test path that
 * survives the "no live boot calls" rule. */
static int contains_substr(const char *hay, const char *needle)
{
    if (hay == (const char *)0 || needle == (const char *)0) return 0;
    uint32_t i, j;
    for (i = 0u; hay[i] != '\0'; i++) {
        for (j = 0u; needle[j] != '\0'; j++)
            if (hay[i + j] != needle[j]) break;
        if (needle[j] == '\0') return 1;
    }
    return 0;
}

static void test_boot_version_hint_rollback_wording(void)
{
    const char *h = boot_version_fault_operator_hint(
        BOOT_VERSION_FAULT_SEC_ROLLBACK);
    TEST_ASSERT_EQ((unsigned long)(h != (const char *)0 ? 1 : 0),
                   (unsigned long)1, "SEC_ROLLBACK hint non-NULL");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "newer"),
                   (unsigned long)1,
                   "rollback hint mentions 'newer' (boot-newer-kernel advice)");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "IPOSRequiredSecVersion"),
                   (unsigned long)1,
                   "rollback hint names IPOSRequiredSecVersion NVRAM variable");
    /* Rollback advice MUST NOT mention 'rebuild' -- that is the
     * ABI-drift response, and conflating the two sends operators
     * down the wrong recovery path. */
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "rebuild"),
                   (unsigned long)0,
                   "rollback hint does NOT mention 'rebuild' "
                   "(distinct from ABI-drift advice)");
}

static void test_boot_version_hint_abi_drift_wording(void)
{
    /* Any of the structural-drift classes or the OK / unknown path
     * should return the ABI-drift hint. Pick BAD_VERSION as the
     * representative case. */
    const char *h = boot_version_fault_operator_hint(
        BOOT_VERSION_FAULT_BAD_VERSION);
    TEST_ASSERT_EQ((unsigned long)(h != (const char *)0 ? 1 : 0),
                   (unsigned long)1, "BAD_VERSION hint non-NULL");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "rebuild"),
                   (unsigned long)1,
                   "ABI-drift hint mentions 'rebuild' advice");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "scripts/build.sh"),
                   (unsigned long)1,
                   "ABI-drift hint names the build script");
}

static void test_boot_version_hint_bad_sha_distinct(void)
{
    /* BAD_SHA should produce a DIFFERENT hint than either rollback or
     * generic ABI drift -- it's a sub-version layout drift where both
     * halves need lockstep rebuild. */
    const char *h = boot_version_fault_operator_hint(
        BOOT_VERSION_FAULT_BAD_SHA);
    TEST_ASSERT_EQ((unsigned long)(h != (const char *)0 ? 1 : 0),
                   (unsigned long)1, "BAD_SHA hint non-NULL");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "lockstep"),
                   (unsigned long)1,
                   "BAD_SHA hint calls out lockstep rebuild");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "newer"),
                   (unsigned long)0,
                   "BAD_SHA hint does NOT conflate with rollback wording");
}

static void test_boot_version_hint_bad_parse_distinct(void)
{
    const char *h = boot_version_fault_operator_hint(
        BOOT_VERSION_FAULT_BAD_PARSE);
    TEST_ASSERT_EQ((unsigned long)(h != (const char *)0 ? 1 : 0),
                   (unsigned long)1, "BAD_PARSE hint non-NULL");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "malformed"),
                   (unsigned long)1,
                   "BAD_PARSE hint mentions malformed kernel");
    TEST_ASSERT_EQ((unsigned long)contains_substr(h, "reflash"),
                   (unsigned long)1,
                   "BAD_PARSE hint advises reflash kernel.exe");
}

void test_register_boot_version(void)
{
    test_suite_register_cat("boot_version: classify happy path",
                            test_boot_version_classify_happy, TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: classify NULL hdr",
                            test_boot_version_classify_null_hdr, TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: classify bad magic",
                            test_boot_version_classify_bad_magic, TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: classify bad version",
                            test_boot_version_classify_bad_version, TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: classify bad size",
                            test_boot_version_classify_bad_size, TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: fault record size pin",
                            test_boot_version_record_size_pin, TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: fault class name coverage",
                            test_boot_version_fault_class_name_coverage,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: rollback hint names newer + NVRAM var",
                            test_boot_version_hint_rollback_wording,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: abi-drift hint mentions rebuild + build.sh",
                            test_boot_version_hint_abi_drift_wording,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: bad_sha hint distinct from rollback",
                            test_boot_version_hint_bad_sha_distinct,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot_version: bad_parse hint names malformed + reflash",
                            test_boot_version_hint_bad_parse_distinct,
                            TEST_CAT_BOOT);
}
