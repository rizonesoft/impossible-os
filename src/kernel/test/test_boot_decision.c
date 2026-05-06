/* ============================================================================
 * test_boot_decision.c -- unit tests for the boot-path provenance and
 * decision record validator (boot_decision_validate) plus the bit/enum
 * rendering helpers.
 *
 * Covers every fail path the validator must reject, the four
 * round-trip fixtures the TODO section asks for (cold boot, recovery
 * boot, network boot, rejected-resume-fallback-to-cold), and the
 * helpers that turn enum values into operator-readable strings.
 *
 * Validator emits klog(LOG_ERROR, "boot", ...) on reject paths;
 * klog_suppress silences them so the log stays clean during test
 * boot. Happy path emits a single LOG_INFO summary line that is NOT
 * suppressed by the reject-path tests (different test bodies), so
 * the success cases suppress it locally.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"
#include "kernel/boot_info.h"

static struct boot_info s_dec_buf;

static void dec_zero(void)
{
    uint64_t *p = (uint64_t *)&s_dec_buf;
    uint32_t i;
    for (i = 0u; i < sizeof(s_dec_buf) / sizeof(uint64_t); i++)
        p[i] = 0u;
    /* boot_media_role: Rule 3.5 (always-on range gate) rejects UNSET as
     * a producer-must-overwrite sentinel. Default to NORMAL so existing
     * happy-path fixtures don't trip the gate; tests exercising the
     * UNSET sentinel set the field explicitly after dec_zero(). */
    s_dec_buf.boot_media_role = BOOT_MEDIA_ROLE_NORMAL;
}

static void test_boot_decision_null_info(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate((struct boot_info *)0, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                  "NULL info -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_NULL_INFO, "err=NULL_INFO");
}

static void test_boot_decision_cold_boot_happy(void)
{
    TEST_KLOG_SUPPRESS("boot");   /* LOG_INFO summary */
    dec_zero();
    /* Cold boot, no flags, zero fallback depth. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "cold boot -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_recovery_boot_happy(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* Forced recovery boot -- trigger flag is set, path/reason match. */
    s_dec_buf.boot_path           = BOOT_PATH_RECOVERY;
    s_dec_buf.boot_reason         = BOOT_REASON_RECOVERY_TRIGGER;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED |
                                    BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "recovery boot -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_network_boot_happy(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* Network boot flagged insecure. Loader selected NETWORK but
     * reason reflects the insecure-channel condition so attestation
     * knows this boot was not transport-authenticated. */
    s_dec_buf.boot_path           = BOOT_PATH_NETWORK;
    s_dec_buf.boot_reason         = BOOT_REASON_NETWORK_INSECURE;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_NETWORK_INSECURE;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "network boot -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_rejected_resume_fallback_happy(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* Resume image rejected -> loader fell back to cold boot. Path is
     * NORMAL (the path that actually ran), reason is
     * RESUME_INVALIDATED (the policy that made the loader take the
     * fallback), fallback_depth=1 (one rung traversed). */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_RESUME_INVALIDATED;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_RESUME_INVALIDATED |
                                    BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth = 1u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "resume-reject fallback -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_bad_path_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* Value just past the enum range. */
    s_dec_buf.boot_path           = (uint32_t)BOOT_PATH_TYPE_MAX + 1u;
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = 0u;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                 "bad boot_path -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_BAD_PATH, "err=BAD_PATH");
}

static void test_boot_decision_bad_reason_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = (uint32_t)BOOT_REASON_CODE_MAX + 1u;
    s_dec_buf.boot_source_flags   = 0u;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                   "bad boot_reason -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_BAD_REASON, "err=BAD_REASON");
}

static void test_boot_decision_unknown_flag_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* Bit 31 is guaranteed reserved today (MASK_KNOWN covers bits 0..9). */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = (1u << 31);
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                     "unknown flag -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_UNKNOWN_FLAG, "err=UNKNOWN_FLAG");
}

static void test_boot_decision_fallback_depth_cap(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_FALLBACK;
    s_dec_buf.boot_source_flags   = 0u;
    s_dec_buf.boot_fallback_depth = (uint32_t)BOOT_FALLBACK_DEPTH_MAX + 1u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                     "fallback OOR -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_FALLBACK_OOR, "err=FALLBACK_OOR");
}

static void test_boot_decision_fallback_depth_at_cap(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* Exactly MAX is accepted (boundary); MAX+1 rejects. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_FALLBACK;
    s_dec_buf.boot_source_flags   = 0u;
    s_dec_buf.boot_fallback_depth = (uint32_t)BOOT_FALLBACK_DEPTH_MAX;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "fallback at MAX -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_all_known_flags_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* All INFORMATIONAL flags (no trigger semantics) at once must pass
     * with reason=NORMAL. Trigger flags (ROLLBACK_TRIGGERED,
     * RECOVERY_TRIGGERED, RESUME_INVALIDATED, NETWORK_INSECURE,
     * MANIFEST_FAILED, MEASURED_BOOT_FAILED) are EXCLUDED here because
     * Rule 7b (flag -> reason) correctly rejects them when the reason
     * is NORMAL.  Asserting the full BOOT_SOURCE_FLAG_MASK_KNOWN as
     * valid with reason=NORMAL would let a producer set a recovery
     * trigger flag while the registry/attestation record claimed
     * reason=normal. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_BOOT_NEXT_SET
                                  | BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH
                                  | BOOT_SOURCE_FLAG_MEDIA_REMOVABLE
                                  | BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "all informational flags -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_trigger_flag_without_reason_rejected(void)
{
    /* Rule 7b regression test: a trigger flag set with a normal reason
     * is a provenance contradiction. Codex 2026-04-30 finding. */
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                       "trigger w/o reason -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_REASON_FLAG,    "err=REASON_FLAG");
}

static void test_boot_decision_unset_path_rejected(void)
{
    /* Rule 1 UNSET sentinel regression: a BSS-zero record from a
     * producer that never populated boot_path lands at UNSET (value 0)
     * and the validator MUST reject. Codex 2026-04-30 H1 finding. */
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();  /* leaves boot_path = BOOT_PATH_UNSET = 0 */
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = 0u;
    s_dec_buf.boot_fallback_depth = 0u;
    /* boot_path intentionally left at UNSET */

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                  "UNSET path -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_BAD_PATH,  "err=BAD_PATH");
}

/* ---- Cross-field invariant tests ---- */

static void test_boot_decision_reason_requires_path(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* RESUME_VALIDATED reason but path=NORMAL -- contradictory. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_RESUME_VALIDATED;
    s_dec_buf.boot_source_flags   = 0u;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                    "RESUME_VALIDATED + path=NORMAL -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_REASON_PATH, "err=REASON_PATH");
}

static void test_boot_decision_reason_forbids_path(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* RESUME_INVALIDATED means the resume image was rejected; path
     * MUST NOT be RESUME (we can't be resumed if the resume failed). */
    s_dec_buf.boot_path           = BOOT_PATH_RESUME;
    s_dec_buf.boot_reason         = BOOT_REASON_RESUME_INVALIDATED;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_RESUME_INVALIDATED;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                    "RESUME_INVALIDATED + path=RESUME -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_REASON_PATH, "err=REASON_PATH");
}

static void test_boot_decision_fallback_requires_fallback_reason(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* depth>0 with reason=NORMAL -- primary-path reason cannot explain
     * a fallback walk. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = 0u;
    s_dec_buf.boot_fallback_depth = 3u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                        "depth>0 reason=NORMAL -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_FALLBACK_REASON, "err=FALLBACK_REASON");
}

static void test_boot_decision_reason_requires_flag(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* reason=MANIFEST_FAILURE without MANIFEST_FAILED flag -- the
     * producer is asserting a policy outcome without the matching
     * audit flag. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;  /* allowed by R5 for this reason */
    s_dec_buf.boot_reason         = BOOT_REASON_MANIFEST_FAILURE;
    s_dec_buf.boot_source_flags   = 0u;               /* MANIFEST_FAILED bit NOT set */
    s_dec_buf.boot_fallback_depth = 1u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                    "MANIFEST_FAILURE without flag -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_REASON_FLAG, "err=REASON_FLAG");
}

static void test_boot_decision_boot_current_mismatch_fixture(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* Firmware landed on a non-primary Boot#### entry without
     * BootNext: path stays NORMAL but BOOT_CURRENT_MISMATCH flag MUST
     * be set so consumers can distinguish this from an ordinary cold
     * boot. Validator accepts; the flag is informational and compatible
     * with reason=NORMAL (R5 path-agnostic). */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH |
                                    BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "boot_current_mismatch -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_fallback_class_reasons_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* depth>0 with reason=ROLLBACK + matching flag -- should pass. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_ROLLBACK;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED;
    s_dec_buf.boot_fallback_depth = 2u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "rollback fallback -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_names(void)
{
    /* Smoke-test the rendering helpers: every valid enum value must
     * return a non-"invalid" string, and an out-of-range value must
     * return "invalid" (not NULL, not the last valid name). */
    const char *s;
    s = boot_path_name(BOOT_PATH_NORMAL);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "path_name(NORMAL) non-null");
    s = boot_path_name(BOOT_PATH_DIAGNOSTIC);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "path_name(DIAGNOSTIC) non-null");
    s = boot_path_name((uint32_t)BOOT_PATH_TYPE_MAX + 99u);
    /* Must be the literal "invalid" sentinel, not NULL. */
    TEST_ASSERT((s[0] == 'i' && s[1] == 'n' && s[2] == 'v'),
                "path_name(out-of-range) = invalid");

    s = boot_reason_name(BOOT_REASON_NORMAL);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "reason_name(NORMAL) non-null");
    s = boot_reason_name(BOOT_REASON_FALLBACK);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "reason_name(FALLBACK) non-null");
    s = boot_reason_name((uint32_t)BOOT_REASON_CODE_MAX + 99u);
    TEST_ASSERT((s[0] == 'i' && s[1] == 'n' && s[2] == 'v'),
                "reason_name(out-of-range) = invalid");
}

static void test_boot_decision_mask_known_stable(void)
{
    /* Guard against silent drift in BOOT_SOURCE_FLAG_MASK_KNOWN if a
     * new flag is added without widening the mask. The mask must equal
     * the OR of every BOOT_SOURCE_FLAG_* we define. */
    uint32_t expected =
        BOOT_SOURCE_FLAG_BOOT_NEXT_SET          |
        BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH  |
        BOOT_SOURCE_FLAG_MEDIA_REMOVABLE        |
        BOOT_SOURCE_FLAG_MEDIA_PRESENT          |
        BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED     |
        BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED     |
        BOOT_SOURCE_FLAG_RESUME_INVALIDATED     |
        BOOT_SOURCE_FLAG_NETWORK_INSECURE       |
        BOOT_SOURCE_FLAG_MANIFEST_FAILED        |
        BOOT_SOURCE_FLAG_MEASURED_BOOT_FAILED;
    TEST_ASSERT_EQ((uint64_t)BOOT_SOURCE_FLAG_MASK_KNOWN, (uint64_t)expected,
                   "BOOT_SOURCE_FLAG_MASK_KNOWN coverage");
}

/* Media-role marker reason (boot-media role-detection feature, v17). */
static void test_boot_decision_media_role_installer_happy(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path                = BOOT_PATH_INSTALLER;
    s_dec_buf.boot_reason              = BOOT_REASON_MEDIA_ROLE_MARKER;
    s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth      = 0u;
    s_dec_buf.boot_media_role          = BOOT_MEDIA_ROLE_INSTALLER;
    s_dec_buf.boot_media_role_mismatch = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "installer media-role -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_media_role_recovery_happy(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path                = BOOT_PATH_RECOVERY;
    s_dec_buf.boot_reason              = BOOT_REASON_MEDIA_ROLE_MARKER;
    s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth      = 0u;
    s_dec_buf.boot_media_role          = BOOT_MEDIA_ROLE_RECOVERY;
    s_dec_buf.boot_media_role_mismatch = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "recovery media-role -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "err=OK");
}

static void test_boot_decision_media_role_normal_path_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* MEDIA_ROLE_MARKER reason MUST NOT pair with NORMAL path. */
    s_dec_buf.boot_path           = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason         = BOOT_REASON_MEDIA_ROLE_MARKER;
    s_dec_buf.boot_source_flags   = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                  "media-role NORMAL path -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_REASON_PATH, "err=REASON_PATH (forbidden)");
}

/* Rule 8 (v17): MEDIA_ROLE_MARKER reason requires consistent role+path. */
static void test_boot_decision_media_role_mismatch_path_role_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* INSTALLER path with RECOVERY role -- path/role disagree, validator must reject. */
    s_dec_buf.boot_path                = BOOT_PATH_INSTALLER;
    s_dec_buf.boot_reason              = BOOT_REASON_MEDIA_ROLE_MARKER;
    s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth      = 0u;
    s_dec_buf.boot_media_role          = BOOT_MEDIA_ROLE_RECOVERY;
    s_dec_buf.boot_media_role_mismatch = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                  "path/role disagree -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_REASON_PATH, "err=REASON_PATH");
}

static void test_boot_decision_media_role_mismatch_flag_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    /* mismatch=1 with reason=MEDIA_ROLE_MARKER is contradictory: a
     * mismatch causes fallback to NORMAL, so the marker reason should
     * NOT be set on a mismatched boot. */
    s_dec_buf.boot_path                = BOOT_PATH_INSTALLER;
    s_dec_buf.boot_reason              = BOOT_REASON_MEDIA_ROLE_MARKER;
    s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth      = 0u;
    s_dec_buf.boot_media_role          = BOOT_MEDIA_ROLE_INSTALLER;
    s_dec_buf.boot_media_role_mismatch = 1u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                  "mismatch=1 with MARKER -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_REASON_PATH, "err=REASON_PATH");
}

/* Rule 8 table-driven coverage: every valid pair accepts; every other
 * combination is rejected with REASON_PATH. */
static void test_boot_decision_media_role_table_driven(void)
{
    TEST_KLOG_SUPPRESS("boot");
    struct row {
        uint32_t path;
        uint32_t role;
        uint8_t  mismatch;
        boot_result_t want_r;
        enum boot_decision_error want_err;
    };
    /* Three valid pairs; everything else rejected. */
    struct row rows[] = {
        /* Accept: installer/installer, recovery/recovery, diagnostics/diagnostic. */
        { BOOT_PATH_INSTALLER,  BOOT_MEDIA_ROLE_INSTALLER,     0u, BOOT_OK,    BOOT_DECISION_ERR_OK },
        { BOOT_PATH_RECOVERY,   BOOT_MEDIA_ROLE_RECOVERY,      0u, BOOT_OK,    BOOT_DECISION_ERR_OK },
        { BOOT_PATH_DIAGNOSTIC, BOOT_MEDIA_ROLE_DIAGNOSTICS,   0u, BOOT_OK,    BOOT_DECISION_ERR_OK },
        /* Reject: UNSET role with otherwise-allowed path -- caught by
         * Rule 3.5 (always-on UNSET sentinel reject), surfaces as
         * BAD_REASON before Rule 8 sees it. */
        { BOOT_PATH_INSTALLER,  BOOT_MEDIA_ROLE_UNSET,         0u, BOOT_FATAL, BOOT_DECISION_ERR_BAD_REASON },
        /* Reject: NORMAL/LIVE/MANUFACTURING role -- never paired with MARKER. */
        { BOOT_PATH_INSTALLER,  BOOT_MEDIA_ROLE_NORMAL,        0u, BOOT_FATAL, BOOT_DECISION_ERR_REASON_PATH },
        { BOOT_PATH_RECOVERY,   BOOT_MEDIA_ROLE_LIVE,          0u, BOOT_FATAL, BOOT_DECISION_ERR_REASON_PATH },
        { BOOT_PATH_DIAGNOSTIC, BOOT_MEDIA_ROLE_MANUFACTURING, 0u, BOOT_FATAL, BOOT_DECISION_ERR_REASON_PATH },
        /* Reject: out-of-range role (MAX + 1) -- caught by Rule 3.5
         * (always-on range gate) before Rule 8 sees it; err=BAD_REASON. */
        { BOOT_PATH_INSTALLER,  (BOOT_MEDIA_ROLE_MAX + 1u),    0u, BOOT_FATAL, BOOT_DECISION_ERR_BAD_REASON },
        /* Reject: mismatch=1 with otherwise-valid pair. */
        { BOOT_PATH_RECOVERY,   BOOT_MEDIA_ROLE_RECOVERY,      1u, BOOT_FATAL, BOOT_DECISION_ERR_REASON_PATH },
    };
    uint32_t i;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        dec_zero();
        s_dec_buf.boot_path                = rows[i].path;
        s_dec_buf.boot_reason              = BOOT_REASON_MEDIA_ROLE_MARKER;
        s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
        s_dec_buf.boot_fallback_depth      = 0u;
        s_dec_buf.boot_media_role          = rows[i].role;
        s_dec_buf.boot_media_role_mismatch = rows[i].mismatch;

        enum boot_decision_error err = BOOT_DECISION_ERR_OK;
        boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
        TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)rows[i].want_r,   "row.r");
        TEST_ASSERT_EQ((uint64_t)err, (uint64_t)rows[i].want_err, "row.err");
    }
}

/* Rule 3.5 (always-on): out-of-range boot_media_role on a normal boot
 * must be rejected -- not just MEDIA_ROLE_MARKER boots. Otherwise a
 * stale producer can ship garbage through to HKLM\SYSTEM\Boot\Device. */
static void test_boot_decision_media_role_range_normal_boot(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path                = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason              = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth      = 0u;
    s_dec_buf.boot_media_role          = (uint32_t)BOOT_MEDIA_ROLE_MAX + 1u;
    s_dec_buf.boot_media_role_mismatch = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                "OOR role on NORMAL boot -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_BAD_REASON, "err=BAD_REASON");
}

/* Rule 3.5: UNSET sentinel on a normal boot must be rejected -- same
 * doctrine as Rules 1 + 2 for boot_path and boot_reason. A producer
 * that left boot_media_role at BSS zero (alternate firmware, partial
 * v17 wiring) must NOT pass through to consumers as if the role had
 * been certified. */
static void test_boot_decision_media_role_unset_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path                = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason              = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth      = 0u;
    s_dec_buf.boot_media_role          = BOOT_MEDIA_ROLE_UNSET;
    s_dec_buf.boot_media_role_mismatch = 0u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                "UNSET role on NORMAL boot -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_BAD_REASON, "err=BAD_REASON");
}

static void test_boot_decision_media_role_mismatch_nonboolean(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path                = BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason              = BOOT_REASON_NORMAL;
    s_dec_buf.boot_source_flags        = BOOT_SOURCE_FLAG_MEDIA_PRESENT;
    s_dec_buf.boot_fallback_depth      = 0u;
    s_dec_buf.boot_media_role          = BOOT_MEDIA_ROLE_NORMAL;
    s_dec_buf.boot_media_role_mismatch = 255u;

    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                "non-boolean mismatch -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_BAD_REASON, "err=BAD_REASON");
}

/* boot_media_role_name() returns canonical lower-case strings matching
 * the on-disk /IPOS/role.txt content -- consumed by HKLM\SYSTEM\Boot\
 * Device\MediaRole and serial output. */
static int role_name_eq(const char *a, const char *b)
{
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

static void test_boot_media_role_name_canonical(void)
{
    struct row { uint32_t role; const char *want; const char *msg; };
    struct row rows[] = {
        { BOOT_MEDIA_ROLE_UNSET,         "unset",         "unset" },
        { BOOT_MEDIA_ROLE_NORMAL,        "normal",        "normal" },
        { BOOT_MEDIA_ROLE_INSTALLER,     "installer",     "installer" },
        { BOOT_MEDIA_ROLE_LIVE,          "live",          "live" },
        { BOOT_MEDIA_ROLE_RECOVERY,      "recovery",      "recovery" },
        { BOOT_MEDIA_ROLE_MANUFACTURING, "manufacturing", "manufacturing" },
        { BOOT_MEDIA_ROLE_DIAGNOSTICS,   "diagnostics",   "diagnostics" },
        { BOOT_MEDIA_ROLE_MAX + 1u,      "invalid",       "out-of-range" },
    };
    uint32_t i;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        TEST_ASSERT_EQ((uint64_t)role_name_eq(boot_media_role_name(rows[i].role),
                                              rows[i].want),
                       1u, rows[i].msg);
    }
}

/* Layout pin: boot_media_role + mismatch land at the v17 offsets. */
static void test_boot_media_role_layout(void)
{
    TEST_ASSERT_EQ((uint64_t)__builtin_offsetof(struct boot_info, boot_media_role),
                   (uint64_t)23968, "boot_media_role @ 23968");
    TEST_ASSERT_EQ((uint64_t)__builtin_offsetof(struct boot_info, boot_media_role_mismatch),
                   (uint64_t)23972, "boot_media_role_mismatch @ 23972");
    TEST_ASSERT_EQ((uint64_t)BOOT_MEDIA_ROLE_MAX, (uint64_t)6,
                   "BOOT_MEDIA_ROLE_MAX = DIAGNOSTICS = 6");
}

/* v18: trust-landscape layout pin. */
static void test_boot_trust_layout(void)
{
    TEST_ASSERT_EQ((uint64_t)__builtin_offsetof(struct boot_info, sbat_level_size),
                   (uint64_t)23976, "sbat_level_size @ 23976");
    TEST_ASSERT_EQ((uint64_t)__builtin_offsetof(struct boot_info, sbat_level),
                   (uint64_t)23980, "sbat_level @ 23980");
    TEST_ASSERT_EQ((uint64_t)__builtin_offsetof(struct boot_info, dbx_size),
                   (uint64_t)24044, "dbx_size @ 24044");
    TEST_ASSERT_EQ((uint64_t)__builtin_offsetof(struct boot_info, degraded_trust_flags),
                   (uint64_t)24048, "degraded_trust_flags @ 24048");
}

static void test_boot_trust_bit_names(void)
{
    struct row { uint32_t bit; const char *want; const char *msg; };
    struct row rows[] = {
        { BOOT_DEGRADED_TRUST_SECURE_BOOT_OFF,        "secure-boot-off",        "off" },
        { BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE, "secure-boot-unreadable", "unreadable" },
        { BOOT_DEGRADED_TRUST_SETUP_MODE,             "setup-mode",             "setup" },
        { BOOT_DEGRADED_TRUST_SBAT_ABSENT,            "sbat-absent",            "sbat" },
        { BOOT_DEGRADED_TRUST_DBX_ABSENT,             "dbx-absent",             "dbx" },
        { 0u,                                         "unknown",                "zero" },
        { 1u << 31,                                   "unknown",                "high-bit" },
        { 0xDEADBEEFu,                                "unknown",                "garbage" },
    };
    uint32_t i;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        TEST_ASSERT_EQ((uint64_t)role_name_eq(boot_degraded_trust_bit_name(rows[i].bit),
                                              rows[i].want),
                       1u, rows[i].msg);
    }
}

/* MASK_KNOWN must enumerate exactly the five bits the validator and
 * the registry populator iterate over. Drift here would silently
 * break the per-bit warning loop. */
static void test_boot_trust_mask_known_stable(void)
{
    uint32_t expected =
        BOOT_DEGRADED_TRUST_SECURE_BOOT_OFF |
        BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE |
        BOOT_DEGRADED_TRUST_SETUP_MODE |
        BOOT_DEGRADED_TRUST_SBAT_ABSENT |
        BOOT_DEGRADED_TRUST_DBX_ABSENT;
    TEST_ASSERT_EQ((uint64_t)BOOT_DEGRADED_TRUST_MASK_KNOWN,
                   (uint64_t)expected,
                   "MASK_KNOWN = exactly the five v18 trust bits");
}

/* Validator closed-mask gate: any bit inside MASK_KNOWN passes (the
 * advisory per-bit warning fires from uefi_secureboot_init() at
 * producer time, not from validate); a bit OUTSIDE MASK_KNOWN trips
 * BOOT_DECISION_ERR_UNKNOWN_FLAG and is FATAL. */
static void test_boot_decision_trust_flags_in_mask_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path     = (uint32_t)BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason   = (uint32_t)BOOT_REASON_NORMAL;
    s_dec_buf.degraded_trust_flags =
        BOOT_DEGRADED_TRUST_SECURE_BOOT_OFF |
        BOOT_DEGRADED_TRUST_SBAT_ABSENT |
        BOOT_DEGRADED_TRUST_DBX_ABSENT;
    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,                  "in-mask bits -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK,     "no err on in-mask bits");
}

static void test_boot_decision_trust_flags_unknown_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path   = (uint32_t)BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason = (uint32_t)BOOT_REASON_NORMAL;
    s_dec_buf.degraded_trust_flags =
        BOOT_DEGRADED_TRUST_SECURE_BOOT_OFF | (1u << 31);
    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                    "OOR trust bit -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_UNKNOWN_FLAG,"err=UNKNOWN_FLAG");
}

/* The "runtime unavailable" early-return in uefi_secureboot_init() must
 * publish a degraded posture, not BSS-zero. The exact trio the producer
 * sets is SECURE_BOOT_UNREADABLE | SBAT_ABSENT | DBX_ABSENT (we cannot
 * tell any of them without GetVariable). Validate the trio passes the
 * closed-mask gate so the Registry populator does not refuse it. */
static void test_boot_decision_trust_unreadable_trio(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path   = (uint32_t)BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason = (uint32_t)BOOT_REASON_NORMAL;
    uint32_t trio =
        BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE |
        BOOT_DEGRADED_TRUST_SBAT_ABSENT |
        BOOT_DEGRADED_TRUST_DBX_ABSENT;
    s_dec_buf.degraded_trust_flags = trio;
    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,                  "unreadable-trio -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK,     "no err");
    TEST_ASSERT_EQ((uint64_t)((trio & ~(uint32_t)BOOT_DEGRADED_TRUST_MASK_KNOWN) == 0u),
                   1u, "trio is fully inside MASK_KNOWN");
}

static void test_boot_decision_trust_flags_zero_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    dec_zero();
    s_dec_buf.boot_path   = (uint32_t)BOOT_PATH_NORMAL;
    s_dec_buf.boot_reason = (uint32_t)BOOT_REASON_NORMAL;
    s_dec_buf.degraded_trust_flags = 0u;
    enum boot_decision_error err = BOOT_DECISION_ERR_OK;
    boot_result_t r = boot_decision_validate(&s_dec_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "clean trust -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_DECISION_ERR_OK, "no err");
}

void test_register_boot_decision(void)
{
    test_suite_register_cat("boot_decision: NULL info rejected",
                            test_boot_decision_null_info, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: cold boot fixture",
                            test_boot_decision_cold_boot_happy, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: recovery boot fixture",
                            test_boot_decision_recovery_boot_happy, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: network boot fixture",
                            test_boot_decision_network_boot_happy, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: rejected-resume fallback fixture",
                            test_boot_decision_rejected_resume_fallback_happy, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: bad boot_path rejected",
                            test_boot_decision_bad_path_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: bad boot_reason rejected",
                            test_boot_decision_bad_reason_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: unknown flag rejected",
                            test_boot_decision_unknown_flag_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: fallback_depth cap rejected",
                            test_boot_decision_fallback_depth_cap, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: fallback_depth at MAX accepted",
                            test_boot_decision_fallback_depth_at_cap, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: all known flags accepted",
                            test_boot_decision_all_known_flags_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: trigger flag without matching reason rejected (R7b)",
                            test_boot_decision_trigger_flag_without_reason_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: UNSET path rejected (BSS-zero sentinel)",
                            test_boot_decision_unset_path_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: reason requires specific path (R5)",
                            test_boot_decision_reason_requires_path, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: reason forbids originating path (R5)",
                            test_boot_decision_reason_forbids_path, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: media-role installer fixture",
                            test_boot_decision_media_role_installer_happy, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: media-role recovery fixture",
                            test_boot_decision_media_role_recovery_happy, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: media-role NORMAL path rejected",
                            test_boot_decision_media_role_normal_path_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: media-role layout offsets",
                            test_boot_media_role_layout, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: boot_media_role_name canonical strings",
                            test_boot_media_role_name_canonical, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: OOR boot_media_role rejected on NORMAL boot (R3.5)",
                            test_boot_decision_media_role_range_normal_boot, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: UNSET boot_media_role rejected on NORMAL boot (R3.5)",
                            test_boot_decision_media_role_unset_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: non-boolean mismatch flag rejected (R3.5)",
                            test_boot_decision_media_role_mismatch_nonboolean, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: media-role path/role disagreement rejected (R8)",
                            test_boot_decision_media_role_mismatch_path_role_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: media-role mismatch=1 with MARKER reason rejected (R8)",
                            test_boot_decision_media_role_mismatch_flag_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: media-role R8 table (9 rows: 3 accept + 6 reject)",
                            test_boot_decision_media_role_table_driven, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: fallback_depth requires fallback reason (R6)",
                            test_boot_decision_fallback_requires_fallback_reason, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: reason requires matching flag (R7)",
                            test_boot_decision_reason_requires_flag, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: BootCurrent mismatch fixture",
                            test_boot_decision_boot_current_mismatch_fixture, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: fallback-class reason accepted",
                            test_boot_decision_fallback_class_reasons_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: name helpers",
                            test_boot_decision_names, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: BOOT_SOURCE_FLAG_MASK_KNOWN coverage",
                            test_boot_decision_mask_known_stable, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: v18 trust-landscape layout offsets",
                            test_boot_trust_layout, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: boot_degraded_trust_bit_name canonical",
                            test_boot_trust_bit_names, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: BOOT_DEGRADED_TRUST_MASK_KNOWN coverage",
                            test_boot_trust_mask_known_stable, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: trust flags in MASK_KNOWN accepted (advisory)",
                            test_boot_decision_trust_flags_in_mask_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: trust flags zero accepted",
                            test_boot_decision_trust_flags_zero_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: trust flags outside MASK_KNOWN rejected (FATAL)",
                            test_boot_decision_trust_flags_unknown_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_decision: trust unreadable-trio (runtime-unavailable) accepted",
                            test_boot_decision_trust_unreadable_trio, TEST_CAT_BOOT);
}
