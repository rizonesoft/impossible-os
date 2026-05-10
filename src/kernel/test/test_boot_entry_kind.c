/* test_boot_entry_kind.c -- per-kind validator tests.
 *
 * The validator source lives in src/boot/uefi/boot_entry_kind.c (compiled
 * into the bootloader binary). This test file pulls a private copy via
 * #include so the kernel test binary can call boot_entry_kind_validate()
 * directly. The validator is pure C with no UEFI types or kmalloc, so
 * the #include is self-contained.
 *
 * XREF: 01-boot-platform/TODO-07-boot-entry-store-menu-policy.md "Entry
 * Kinds: Split, UKI, Chainload, Network, Resume"
 *
 * PURE-HELPER TESTS ONLY. Per CLAUDE.md "Test Code Policy", these tests
 * must NEVER call live boot infrastructure. The validator is a string
 * walker over a synthetic byte buffer; tests synthesize payload bytes
 * inline.
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"

/* Pull the validator implementation into this translation unit. */
#include "../../boot/uefi/boot_entry_kind.c"

#include "kernel/types.h"

/* ---- Helper: run validate against a fixed payload string ---- */

static int run_validate_kind(unsigned int kind, const char *payload,
                             boot_entry_decoded_t *out)
{
    unsigned int len = 0;
    if (payload) {
        const char *p = payload;
        while (*p) { len++; p++; }
    }
    unsigned int flags = 0;
    int sb_active = 0;
    return boot_entry_kind_validate(kind, flags, sb_active,
                                    (const unsigned char *)payload, len, out);
}

/* ---- SPLIT validator: happy path + rejection cases ---- */

static void test_split_valid_minimal(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"\\\\boot\\\\kernel.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "SPLIT minimal payload (kernel only) accepted");
    TEST_ASSERT_EQ((uint32_t)dec.valid, 1u,
        "decoded.valid set on accept");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.has_kernel, 1u,
        "has_kernel flagged");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.kernel[0], (uint32_t)'\\',
        "kernel path starts with backslash");
}

static void test_split_full_payload(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\","
        "\"cmdline\": \"quiet splash\","
        "\"root\": \"slot-a\","
        "\"initrd\": [\"\\\\boot\\\\initrd.img\"]}",
        &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "SPLIT full payload accepted");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.has_kernel, 1u, "kernel present");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.has_cmdline, 1u, "cmdline present");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.has_root, 1u, "root present");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.initrd_count, 1u,
        "one initrd recorded");
}

static void test_split_missing_kernel(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"cmdline\": \"\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_FIELD_MISSING,
        "SPLIT without kernel field rejected");
    TEST_ASSERT_EQ((uint32_t)dec.valid, 0u,
        "decoded.valid cleared on reject");
}

static void test_split_no_payload_object(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT, NULL, &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_PAYLOAD_MISSING,
        "SPLIT with NULL payload rejected as PAYLOAD_MISSING");
}

static void test_split_path_escape(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"\\\\EFI\\\\..\\\\evil.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_FIELD_VALUE,
        "SPLIT path with .. traversal rejected");
}

static void test_split_disallowed_prefix(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"vendor/path/kernel.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_FIELD_VALUE,
        "SPLIT path without leading backslash rejected");
}

/* The bare-backslash fallback accepts `\<file>` only -- a NESTED bare
 * path like `\Windows\kernel.exe` or `\vendor\boot.exe` is OUTSIDE
 * the allowed roots and must be rejected. Without this gate the
 * allowlist effectively accepts every ESP path. */
static void test_split_rejects_nested_bare_path(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"\\\\Windows\\\\kernel.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_FIELD_VALUE,
        "nested bare path \\Windows\\kernel.exe rejected");
}

static void test_split_rejects_nested_vendor_path(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"\\\\vendor\\\\boot.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_FIELD_VALUE,
        "nested bare path \\vendor\\boot.exe rejected");
}

/* `\kernel.exe` (single segment, no nested directory) is the
 * legitimate bare-path fallback the bootloader's existing
 * kernel_paths search uses. */
static void test_split_accepts_single_segment_bare_path(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"\\\\kernel.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "single-segment bare path \\kernel.exe accepted");
}

static void test_split_unknown_key_skipped(void)
{
    /* Forward-compat: unknown payload keys must skip-with-accept so a
     * vendor extension does not break older readers. */
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SPLIT,
        "{\"kernel\": \"\\\\boot\\\\kernel.exe\","
        "\"vendor_x\": \"hello\"}",
        &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "unknown payload keys must be tolerated");
}

/* ---- UKI validator: payload-forbidden ---- */

static void test_uki_no_payload(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_UKI, NULL, &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "UKI without payload object accepted");
    TEST_ASSERT_EQ((uint32_t)dec.valid, 1u,
        "decoded.valid set on accept");
}

static void test_uki_empty_payload_object(void)
{
    /* Forward-compat: empty `{}` tolerated. */
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_UKI, "{}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "UKI with empty {} payload accepted");
}

static void test_uki_cmdline_smuggle_rejected(void)
{
    /* Disk-side UKI MUST NOT carry a cmdline override -- the
     * Secure-Boot-signed PE is the trust anchor. */
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_UKI,
        "{\"cmdline\": \"smuggled-override\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_PAYLOAD_FORBIDDEN,
        "UKI with cmdline payload rejected (smuggled override)");
}

static void test_uki_kernel_override_rejected(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_UKI,
        "{\"kernel\": \"\\\\boot\\\\malicious.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_PAYLOAD_FORBIDDEN,
        "UKI with kernel payload rejected (smuggled override)");
}

static void test_uki_uki_path_accepted(void)
{
    /* The schema's documented UKI payload field. Advisory under runtime
     * (the running BOOTX64.UKI.efi already located the kernel via PE
     * .linux), but the validator must accept it so conforming stores
     * are not demoted at boot. */
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_UKI,
        "{\"uki_path\": \"\\\\EFI\\\\Linux\\\\impossible.efi\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "UKI with documented uki_path accepted");
}

static void test_uki_uki_path_with_profile(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_UKI,
        "{\"uki_path\": \"\\\\EFI\\\\Linux\\\\impossible.efi\","
        "\"profile\": 2}",
        &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "UKI with uki_path + profile accepted");
}

/* ---- SAFE: same shape as SPLIT ---- */

static void test_safe_valid(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SAFE,
        "{\"kernel\": \"\\\\boot\\\\kernel.exe\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "SAFE accepts SPLIT-shaped payload");
}

/* A SAFE entry with a non-default kernel path must populate
 * decoded.u.split.kernel so the bootloader's authoritative-load
 * branch (which treats SAFE alongside SPLIT) actually opens the
 * intended kernel. Without this, audit would report selection of
 * the SAFE entry while the ambient kernel boots. */
static void test_safe_with_custom_kernel_path(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_SAFE,
        "{\"kernel\": \"\\\\EFI\\\\ImpossibleOS\\\\safe-kernel.exe\"}",
        &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_OK,
        "SAFE with custom kernel path accepted");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.has_kernel, 1u,
        "SAFE decoded.u.split.has_kernel set so loader can honor the path");
    TEST_ASSERT_EQ((uint32_t)(dec.u.split.kernel[0] == '\\'), 1u,
        "SAFE kernel path begins with backslash");
}

/* ---- Stub validators: deferred kinds reject ---- */

static void test_chainload_stub(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_CHAINLOAD,
        "{\"path\": \"\\\\EFI\\\\Other\\\\bootmgr.efi\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_NOT_SUPPORTED,
        "CHAINLOAD validator stubbed; rejects until consumer ships");
}

static void test_network_stub(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_NETWORK,
        "{\"url\": \"http://example/k\"}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_NOT_SUPPORTED,
        "NETWORK validator stubbed");
}

static void test_resume_stub(void)
{
    boot_entry_decoded_t dec;
    int rc = run_validate_kind(BOOT_ENTRY_KIND_RESUME, "{}", &dec);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)BOOT_ENTRY_KIND_REJ_NOT_SUPPORTED,
        "RESUME validator stubbed");
}

/* ---- Reject-name lookup ---- */

static int strs_equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void test_reject_name_table(void)
{
    TEST_ASSERT_EQ((uint32_t)strs_equal(
        boot_entry_kind_reject_name(BOOT_ENTRY_KIND_OK), "OK"), 1u,
        "OK -> 'OK'");
    TEST_ASSERT_EQ((uint32_t)strs_equal(
        boot_entry_kind_reject_name(BOOT_ENTRY_KIND_REJ_PAYLOAD_FORBIDDEN),
        "REJ_PAYLOAD_FORBIDDEN"), 1u,
        "PAYLOAD_FORBIDDEN -> 'REJ_PAYLOAD_FORBIDDEN'");
    TEST_ASSERT_EQ((uint32_t)strs_equal(
        boot_entry_kind_reject_name(0xFFFF), "UNKNOWN"), 1u,
        "out-of-range -> 'UNKNOWN'");
}

/* ---- Decoded-default state ---- */

static void test_decoded_zero_after_reject(void)
{
    boot_entry_decoded_t dec;
    /* Pre-fill with garbage so we can verify zero_decoded is called. */
    unsigned char *p = (unsigned char *)&dec;
    for (size_t i = 0; i < sizeof(dec); i++) p[i] = 0xAB;
    (void)run_validate_kind(BOOT_ENTRY_KIND_SPLIT, NULL, &dec);
    TEST_ASSERT_EQ((uint32_t)dec.valid, 0u,
        "decoded.valid cleared after reject");
    TEST_ASSERT_EQ((uint32_t)dec.u.split.has_kernel, 0u,
        "has_kernel cleared after reject");
}

void test_register_boot_entry_kind(void);
void test_register_boot_entry_kind(void)
{
    test_suite_register_cat("boot_entry_kind SPLIT minimal valid",
        test_split_valid_minimal, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT full payload",
        test_split_full_payload, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT rejects missing kernel",
        test_split_missing_kernel, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT rejects NULL payload",
        test_split_no_payload_object, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT rejects path traversal",
        test_split_path_escape, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT rejects disallowed prefix",
        test_split_disallowed_prefix, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT rejects nested bare path (Windows)",
        test_split_rejects_nested_bare_path, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT rejects nested bare path (vendor)",
        test_split_rejects_nested_vendor_path, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT accepts single-segment bare path",
        test_split_accepts_single_segment_bare_path, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SPLIT tolerates unknown keys",
        test_split_unknown_key_skipped, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind UKI no payload OK",
        test_uki_no_payload, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind UKI empty {} OK",
        test_uki_empty_payload_object, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind UKI rejects cmdline override",
        test_uki_cmdline_smuggle_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind UKI rejects kernel override",
        test_uki_kernel_override_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind UKI accepts uki_path",
        test_uki_uki_path_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind UKI accepts uki_path + profile",
        test_uki_uki_path_with_profile, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SAFE accepts SPLIT shape",
        test_safe_valid, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind SAFE with custom kernel path",
        test_safe_with_custom_kernel_path, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind CHAINLOAD stubbed (REJ_NOT_SUPPORTED)",
        test_chainload_stub, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind NETWORK stubbed",
        test_network_stub, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind RESUME stubbed",
        test_resume_stub, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind reject name lookup",
        test_reject_name_table, TEST_CAT_BOOT);
    test_suite_register_cat("boot_entry_kind decoded zeroed after reject",
        test_decoded_zero_after_reject, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
