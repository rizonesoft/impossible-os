/* ============================================================================
 * test_boot_warm_update.c -- unit tests for the warm-kernel-update
 * handoff ABI validator.
 *
 * Covers:
 *   - NULL desc -> COLD_FALLBACK + NULL_DESC
 *   - wrong type -> COLD_FALLBACK + WRONG_TYPE
 *   - empty (length=0) -> COLD_FALLBACK + EMPTY
 *   - unaligned phys_start -> COLD_FALLBACK + UNALIGNED
 *   - unknown continuation bit -> COLD_FALLBACK + UNKNOWN_CONT_FLAG
 *   - all known continuation bits set -> ACCEPTED
 *   - zero continuation bits set (minimal descriptor) -> ACCEPTED
 *   - single continuation bit (PAGE_TABLES) -> ACCEPTED
 *   - name helper: known bits return feature names, unknown returns "reserved"
 *   - continuation mask coverage (every CONT_* bit is in MASK_KNOWN)
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"
#include "kernel/boot_info.h"

static struct boot_payload_desc s_wu_desc;

static void wu_zero(void)
{
    uint8_t *p = (uint8_t *)&s_wu_desc;
    uint32_t i;
    for (i = 0u; i < sizeof(s_wu_desc); i++)
        p[i] = 0u;
}

static void wu_make_valid(void)
{
    wu_zero();
    s_wu_desc.type       = BOOT_PAYLOAD_WARM_UPDATE_STATE;
    s_wu_desc.phys_start = 0x40000000ull;  /* page-aligned */
    s_wu_desc.length     = 0x100000ull;    /* 1 MiB */
    s_wu_desc.flags      = BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED;
    /* no continuation bits set -> minimal valid descriptor */
}

static void test_boot_wu_null_desc(void)
{
    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d =
        boot_warm_update_consume((const struct boot_payload_desc *)0, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK, "NULL desc -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_NULL_DESC, "err=NULL_DESC");
}

static void test_boot_wu_wrong_type(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    s_wu_desc.type = BOOT_PAYLOAD_MODULE;  /* not WARM_UPDATE_STATE */

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK, "wrong type -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_WRONG_TYPE, "err=WRONG_TYPE");
}

static void test_boot_wu_empty_length(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    s_wu_desc.length = 0u;

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK, "empty -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_EMPTY,     "err=EMPTY");
}

static void test_boot_wu_length_contract(void)
{
    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d;

    TEST_KLOG_SUPPRESS("boot");

    /* One page past the type's 64 MiB maximum, still page-multiple and still
     * nonzero -- so it satisfies every OTHER rule this consumer applies and
     * would have been accepted, and pinned, before the length contract
     * landed. Preserved subsystem state is metadata; an outgoing kernel
     * declaring more than the contract is malformed, not generous. */
    wu_make_valid();
    s_wu_desc.length = 67108864ull + 4096ull;
    d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d, (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK,
                   "over-contract length -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_LENGTH_CONTRACT,
                   "err=LENGTH_CONTRACT, distinct from EMPTY");

    /* EXACTLY at the maximum must still be accepted. Without this the test
     * above passes just as happily against a rule that refuses everything. */
    err = BOOT_WARM_UPDATE_ERR_OK;
    wu_make_valid();
    s_wu_desc.length = 67108864ull;
    d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_OK,
                   "a descriptor exactly at the maximum is not a length refusal");
}

static void test_boot_wu_unaligned_phys(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    s_wu_desc.phys_start = 0x40000100ull;  /* not 4K-aligned */

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK, "unaligned -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_UNALIGNED, "err=UNALIGNED");
}

static void test_boot_wu_unknown_cont_flag(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    /* Bit 20 -- NOT in BOOT_WARM_UPDATE_CONT_MASK_KNOWN (MASK covers bits 8..13).
     * Bit 20 is in the continuation range (>=8) so must fail. */
    s_wu_desc.flags |= (1u << 20);

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK,    "unknown cont -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_UNKNOWN_CONT_FLAG, "err=UNKNOWN_CONT_FLAG");
}

static void test_boot_wu_length_not_page_multiple(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    s_wu_desc.length = 4097u;  /* > page but not a page-multiple */

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK, "non-page-multiple len -> FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_UNALIGNED, "err=UNALIGNED");
}

static void test_boot_wu_missing_valid_flag(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    s_wu_desc.flags = BOOT_PAYLOAD_FLAG_RESERVED;  /* VALID bit NOT set */

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK,    "missing VALID -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_MISSING_FLAGS, "err=MISSING_FLAGS");
}

static void test_boot_wu_missing_reserved_flag(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    s_wu_desc.flags = BOOT_PAYLOAD_FLAG_VALID;  /* RESERVED bit NOT set */

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_COLD_FALLBACK,    "missing RESERVED -> COLD_FALLBACK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_MISSING_FLAGS, "err=MISSING_FLAGS");
}

static void test_boot_wu_all_known_cont_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");   /* LOG_INFO accept summary */
    wu_make_valid();
    s_wu_desc.flags |= BOOT_WARM_UPDATE_CONT_MASK_KNOWN;

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_ACCEPTED, "all known cont -> ACCEPTED");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_OK,   "err=OK");
}

static void test_boot_wu_zero_cont_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    /* Minimal descriptor: type + phys + length + standard flags only,
     * no continuation bits set. Still ACCEPTED -- zero continuation
     * means "no preserved subsystem state beyond raw memory". */

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_ACCEPTED, "zero cont -> ACCEPTED");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_OK,   "err=OK");
}

static void test_boot_wu_single_cont_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    wu_make_valid();
    s_wu_desc.flags |= BOOT_WARM_UPDATE_CONT_PAGE_TABLES;

    enum boot_warm_update_error err = BOOT_WARM_UPDATE_ERR_OK;
    enum boot_warm_update_decision d = boot_warm_update_consume(&s_wu_desc, &err);
    TEST_ASSERT_EQ((uint64_t)d,   (uint64_t)BOOT_WARM_UPDATE_ACCEPTED, "PAGE_TABLES cont -> ACCEPTED");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_WARM_UPDATE_ERR_OK,   "err=OK");
}

static void test_boot_wu_cont_mask_stable(void)
{
    /* Guard against drift: MASK_KNOWN must be the OR of every
     * BOOT_WARM_UPDATE_CONT_* bit defined today. */
    uint32_t expected =
        BOOT_WARM_UPDATE_CONT_PAGE_TABLES       |
        BOOT_WARM_UPDATE_CONT_SCHEDULER_QUIESCED |
        BOOT_WARM_UPDATE_CONT_VFS_WRITEBACK     |
        BOOT_WARM_UPDATE_CONT_FD_TABLE          |
        BOOT_WARM_UPDATE_CONT_OBJECT_HANDLES    |
        BOOT_WARM_UPDATE_CONT_HW_QUEUES;
    TEST_ASSERT_EQ((uint64_t)BOOT_WARM_UPDATE_CONT_MASK_KNOWN, (uint64_t)expected,
                   "BOOT_WARM_UPDATE_CONT_MASK_KNOWN coverage");
}

static void test_boot_wu_cont_name(void)
{
    const char *s = boot_warm_update_cont_name(BOOT_WARM_UPDATE_CONT_PAGE_TABLES);
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)s, 0u, "cont_name(PAGE_TABLES) non-null");
    TEST_ASSERT((s[0] == 'p' && s[1] == 'a' && s[2] == 'g'), "cont_name(PAGE_TABLES) starts with 'pag'");

    s = boot_warm_update_cont_name(BOOT_WARM_UPDATE_CONT_HW_QUEUES);
    TEST_ASSERT((s[0] == 'h' && s[1] == 'w'), "cont_name(HW_QUEUES) starts with 'hw'");

    /* Bit not in the mask -> "reserved" literal */
    s = boot_warm_update_cont_name(1u << 24);
    TEST_ASSERT((s[0] == 'r' && s[1] == 'e' && s[2] == 's'),
                "cont_name(reserved bit) starts with 'res'");
}

void test_register_boot_warm_update(void)
{
    test_suite_register_cat("boot_warm_update: length contract bounds",
                            test_boot_wu_length_contract, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: NULL desc",
                            test_boot_wu_null_desc, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: wrong type",
                            test_boot_wu_wrong_type, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: empty length",
                            test_boot_wu_empty_length, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: unaligned phys_start",
                            test_boot_wu_unaligned_phys, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: non-page-multiple length rejected",
                            test_boot_wu_length_not_page_multiple, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: unknown cont flag rejected",
                            test_boot_wu_unknown_cont_flag, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: missing VALID flag rejected",
                            test_boot_wu_missing_valid_flag, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: missing RESERVED flag rejected",
                            test_boot_wu_missing_reserved_flag, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: all known cont bits accepted",
                            test_boot_wu_all_known_cont_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: zero cont bits accepted",
                            test_boot_wu_zero_cont_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: single cont bit accepted",
                            test_boot_wu_single_cont_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: CONT_MASK_KNOWN coverage",
                            test_boot_wu_cont_mask_stable, TEST_CAT_BOOT);
    test_suite_register_cat("boot_warm_update: cont name helper",
                            test_boot_wu_cont_name, TEST_CAT_BOOT);
}
