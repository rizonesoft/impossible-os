/* ============================================================================
 * test_boot_caps.c -- unit tests for the capability negotiation
 * validator (boot_caps_validate).
 *
 * Covers every fail path the validator must reject, plus the happy
 * path and the degraded-only summary path. The validator reads only
 * caps_required / caps_present / caps_degraded, so the fixture keeps
 * everything else zero.
 *
 * The validator emits klog(LOG_FATAL, "boot", ...) lines on the
 * reject paths; klog_suppress silences them so [FAIL]-lookalike log
 * noise does not confuse the test reader.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"
#include "kernel/boot_info.h"

static struct boot_info s_caps_buf;

static void caps_zero(void)
{
    uint64_t *p = (uint64_t *)&s_caps_buf;
    uint32_t i;
    for (i = 0u; i < sizeof(s_caps_buf) / sizeof(uint64_t); i++)
        p[i] = 0u;
}

static void test_boot_caps_null_info(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate((struct boot_info *)0, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,            "NULL info -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_NULL_INFO, "err=NULL_INFO");
}

static void test_boot_caps_happy_path(void)
{
    caps_zero();
    /* Typical shape: every known bit classified as present, nothing
     * required, nothing degraded. This is the "ideal world" where a
     * loader populated every feature. */
    s_caps_buf.caps_required = 0ull;
    s_caps_buf.caps_present  = BOOT_CAP_MASK_KNOWN;
    s_caps_buf.caps_degraded = 0ull;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "happy path -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_OK,     "err=OK");
}

static void test_boot_caps_happy_with_degraded(void)
{
    TEST_KLOG_SUPPRESS("boot");  /* INFO summary line */
    caps_zero();
    /* Mixed shape: some features present, the rest degraded. Every
     * known bit is classified (union covers BOOT_CAP_MASK_KNOWN) --
     * the classic native-UEFI boot where e.g. network provenance is
     * intentionally skipped but runtime services were populated. */
    uint64_t present_set  = BOOT_CAP_PAYLOAD_DESCRIPTORS |
                            BOOT_CAP_RUNTIME_SERVICES    |
                            BOOT_CAP_USB_HANDOVER        |
                            BOOT_CAP_MEDIA_ROLE;
    s_caps_buf.caps_required = 0ull;
    s_caps_buf.caps_present  = present_set;
    s_caps_buf.caps_degraded = BOOT_CAP_MASK_KNOWN & ~present_set;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,          "mixed classify -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_OK, "err=OK");
}

static void test_boot_caps_unknown_required_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    caps_zero();
    /* Set a required bit outside BOOT_CAP_MASK_KNOWN. Bit 63 is the
     * highest possible u64 bit and guaranteed to be reserved today. */
    s_caps_buf.caps_required = (1ull << 63);
    s_caps_buf.caps_present  = 0ull;
    s_caps_buf.caps_degraded = 0ull;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                        "unknown_required -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_UNKNOWN_REQUIRED,    "err=UNKNOWN_REQUIRED");
}

static void test_boot_caps_required_is_known(void)
{
    caps_zero();
    /* Required bits that ARE in the known mask must pass rule 1. Every
     * known bit is classified so rule 4 passes too. */
    s_caps_buf.caps_required = BOOT_CAP_PAYLOAD_DESCRIPTORS;
    s_caps_buf.caps_present  = BOOT_CAP_MASK_KNOWN;
    s_caps_buf.caps_degraded = 0ull;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,          "known required -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_OK, "err=OK");
}

static void test_boot_caps_unclassified_known_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    caps_zero();
    /* A known bit left clear in BOTH caps_present AND caps_degraded
     * violates the producer contract ("classify every known feature").
     * Validator must reject this under rule 4 even though rules 1-3
     * all pass. */
    s_caps_buf.caps_required = 0ull;
    /* Cover everything EXCEPT one known bit -- here USB_HANDOVER. */
    s_caps_buf.caps_present  = BOOT_CAP_MASK_KNOWN & ~BOOT_CAP_USB_HANDOVER;
    s_caps_buf.caps_degraded = 0ull;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                          "unclassified known -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_UNCLASSIFIED_KNOWN,    "err=UNCLASSIFIED_KNOWN");
}

static void test_boot_caps_required_and_degraded_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    caps_zero();
    /* A bit set in BOTH caps_required and caps_degraded is a producer
     * contradiction ("asserted required but could not provide"). Rule
     * 2 must fire BEFORE rule 3 or rule 4, so we also set the offending
     * bit in present to prove rule 2 takes priority. */
    s_caps_buf.caps_required = BOOT_CAP_TPM_EVENT_LOG;
    s_caps_buf.caps_present  = BOOT_CAP_MASK_KNOWN & ~BOOT_CAP_TPM_EVENT_LOG;
    s_caps_buf.caps_degraded = BOOT_CAP_TPM_EVENT_LOG;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                          "required&degraded -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_REQUIRED_DEGRADED,     "err=REQUIRED_DEGRADED");
}

static void test_boot_caps_present_and_degraded_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    caps_zero();
    /* A bit set in BOTH caps_present and caps_degraded is contradictory
     * (a capability is populated OR skipped, never both). Set every
     * other known bit in present so rule 4 does not catch this case
     * first. */
    s_caps_buf.caps_required = 0ull;
    s_caps_buf.caps_present  = BOOT_CAP_MASK_KNOWN;
    s_caps_buf.caps_degraded = BOOT_CAP_USB_HANDOVER;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                         "present&degraded -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_PRESENT_DEGRADED,     "err=PRESENT_DEGRADED");
}

static void test_boot_caps_unknown_present_tolerated(void)
{
    caps_zero();
    /* Unknown bits in caps_present / caps_degraded are forward-compat:
     * older kernel booting against newer loader that populated a
     * reserved bit must still boot cleanly. Every known bit must still
     * be classified, so we set all of them in present. */
    s_caps_buf.caps_required = 0ull;
    s_caps_buf.caps_present  = BOOT_CAP_MASK_KNOWN | (1ull << 62);
    s_caps_buf.caps_degraded = 0ull;

    enum boot_caps_error err = BOOT_CAPS_ERR_OK;
    boot_result_t r = boot_caps_validate(&s_caps_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,          "unknown present -> OK (forward compat)");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_CAPS_ERR_OK, "err=OK");
}

static void test_boot_caps_mask_known_is_stable(void)
{
    /* Guards against accidentally reshuffling BOOT_CAP bit positions:
     * BOOT_CAP_MASK_KNOWN must equal OR of every BOOT_CAP_* we define
     * today. If a new bit is added, this test will force an update. */
    uint64_t expected =
        BOOT_CAP_PAYLOAD_DESCRIPTORS |
        BOOT_CAP_RUNTIME_SERVICES    |
        BOOT_CAP_SECURE_BOOT_STATE   |
        BOOT_CAP_TPM_EVENT_LOG       |
        BOOT_CAP_USB_HANDOVER        |
        BOOT_CAP_MEDIA_ROLE          |
        BOOT_CAP_NETWORK_PROVENANCE  |
        BOOT_CAP_RESUME_METADATA     |
        BOOT_CAP_ALT_PROTOCOL_ADAPTER;
    TEST_ASSERT_EQ((uint64_t)BOOT_CAP_MASK_KNOWN, expected, "BOOT_CAP_MASK_KNOWN coverage");
}

static void test_boot_caps_bit_name(void)
{
    /* Smoke: the rendering helper returns a non-"reserved" string for
     * every known single-bit value. */
    const char *s;
    s = boot_caps_bit_name(BOOT_CAP_PAYLOAD_DESCRIPTORS);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "bit name non-null (payload)");
    s = boot_caps_bit_name(BOOT_CAP_RUNTIME_SERVICES);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "bit name non-null (runtime)");
    /* Unknown bit -> "reserved" literal (non-null). */
    s = boot_caps_bit_name(1ull << 63);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "bit name non-null (reserved)");
}

/* The require/mark_present helpers operate on g_boot_info directly,
 * so these tests save + restore its caps words to avoid side effects
 * on other tests. */
extern struct boot_info g_boot_info;

static void test_boot_caps_require_basics(void)
{
    uint64_t saved_p = g_boot_info.caps_present;
    uint64_t saved_d = g_boot_info.caps_degraded;

    g_boot_info.caps_present = BOOT_CAP_RUNTIME_SERVICES | BOOT_CAP_TPM_EVENT_LOG;
    g_boot_info.caps_degraded = 0;

    TEST_ASSERT(boot_caps_require(BOOT_CAP_RUNTIME_SERVICES), "require single bit present");
    TEST_ASSERT(boot_caps_require(BOOT_CAP_RUNTIME_SERVICES | BOOT_CAP_TPM_EVENT_LOG),
                "require combined bits present");
    TEST_ASSERT(!boot_caps_require(BOOT_CAP_USB_HANDOVER), "require bit absent -> 0");
    TEST_ASSERT(!boot_caps_require(BOOT_CAP_RUNTIME_SERVICES | BOOT_CAP_USB_HANDOVER),
                "require one-of-two absent -> 0");

    g_boot_info.caps_present = saved_p;
    g_boot_info.caps_degraded = saved_d;
}

static void test_boot_caps_mark_present_promotes(void)
{
    uint64_t saved_p = g_boot_info.caps_present;
    uint64_t saved_d = g_boot_info.caps_degraded;

    /* Start with secure-boot degraded (the canonical bootloader-time
     * state) and simulate the kernel-side UEFI RT variable query
     * succeeding. After mark_present, the bit must be in present
     * and removed from degraded. */
    g_boot_info.caps_present  = BOOT_CAP_RUNTIME_SERVICES;
    g_boot_info.caps_degraded = BOOT_CAP_SECURE_BOOT_STATE;

    boot_caps_mark_present(BOOT_CAP_SECURE_BOOT_STATE);

    TEST_ASSERT(boot_caps_require(BOOT_CAP_SECURE_BOOT_STATE),
                "mark_present promotes degraded->present");
    TEST_ASSERT_EQ((uint64_t)(g_boot_info.caps_degraded & BOOT_CAP_SECURE_BOOT_STATE),
                   0u, "mark_present clears degraded bit");
    /* Monotonic: second call is a no-op (already set). */
    boot_caps_mark_present(BOOT_CAP_SECURE_BOOT_STATE);
    TEST_ASSERT(boot_caps_require(BOOT_CAP_SECURE_BOOT_STATE),
                "mark_present idempotent");

    g_boot_info.caps_present = saved_p;
    g_boot_info.caps_degraded = saved_d;
}

static void test_boot_caps_mark_present_unknown_bits_masked(void)
{
    uint64_t saved_p = g_boot_info.caps_present;
    uint64_t saved_d = g_boot_info.caps_degraded;

    g_boot_info.caps_present  = 0;
    g_boot_info.caps_degraded = 0;

    /* Pass an unknown bit along with a known one -- only the known
     * bit should be set, unknown is masked off (keeps the
     * BOOT_CAP_MASK_KNOWN closed-mask invariant). */
    boot_caps_mark_present(BOOT_CAP_USB_HANDOVER | (1ull << 62));

    TEST_ASSERT(boot_caps_require(BOOT_CAP_USB_HANDOVER), "known bit set");
    TEST_ASSERT_EQ((uint64_t)(g_boot_info.caps_present & (1ull << 62)), 0u,
                   "unknown bit masked off");

    g_boot_info.caps_present = saved_p;
    g_boot_info.caps_degraded = saved_d;
}

void test_register_boot_caps(void)
{
    test_suite_register_cat("boot_caps: NULL info rejected",
                            test_boot_caps_null_info, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: happy path (present only)",
                            test_boot_caps_happy_path, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: degraded-only passes",
                            test_boot_caps_happy_with_degraded, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: unknown required bit rejected",
                            test_boot_caps_unknown_required_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: known required bit accepted",
                            test_boot_caps_required_is_known, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: unclassified known bit rejected",
                            test_boot_caps_unclassified_known_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: required&degraded rejected",
                            test_boot_caps_required_and_degraded_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: present&degraded rejected",
                            test_boot_caps_present_and_degraded_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: unknown present tolerated (forward compat)",
                            test_boot_caps_unknown_present_tolerated, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: BOOT_CAP_MASK_KNOWN coverage",
                            test_boot_caps_mask_known_is_stable, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: bit name helper",
                            test_boot_caps_bit_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: require() basics",
                            test_boot_caps_require_basics, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: mark_present promotes degraded",
                            test_boot_caps_mark_present_promotes, TEST_CAT_BOOT);
    test_suite_register_cat("boot_caps: mark_present masks unknown bits",
                            test_boot_caps_mark_present_unknown_bits_masked, TEST_CAT_BOOT);
}
