/* ============================================================================
 * test_ab_boot.c -- unit tests for the A/B boot metadata wire ABI (TODO-21).
 *
 * Covers the sec 1 pure-logic surface in include/boot/ab_boot_metadata.h:
 *   - default record is valid; factory slot/priority values
 *   - validator rejects bad magic / version / reserved-nonzero /
 *     out-of-range active_slot / wrong CRC
 *   - CRC covers payload but excludes the trailing crc32 field
 *   - newest-valid-copy selection (sec 7 redundant-copy read)
 *
 * Pure freestanding logic; no UEFI RT, no disk I/O (that storage adapter
 * lands in sec 2). Host smoke test covers the on-disk round trip later.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "boot/ab_boot_metadata.h"

static void ab_make_valid(struct ab_boot_metadata *m, unsigned int gen)
{
    unsigned int i;
    for (i = 0u; i < AB_BOOT_META_SIZE; i++)
        ((unsigned char *)m)[i] = 0u;
    m->generation = gen;
    m->active_slot = AB_BOOT_SLOT_A;
    m->slot[AB_BOOT_SLOT_A].priority = 1u;
    ab_boot_meta_finalize(m);
}

static void test_ab_boot_default_is_valid(void)
{
    struct ab_boot_metadata m;
    ab_boot_meta_default(&m);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 1u, "default record validates");
    TEST_ASSERT_EQ((uint64_t)m.active_slot, (uint64_t)AB_BOOT_SLOT_A, "default active slot = A");
    TEST_ASSERT_EQ((uint64_t)m.slot[AB_BOOT_SLOT_A].priority, 1u, "default slot A priority = 1");
    TEST_ASSERT_EQ((uint64_t)m.slot[AB_BOOT_SLOT_A].tries, 0u, "default slot A tries = 0");
    TEST_ASSERT_EQ((uint64_t)m.slot[AB_BOOT_SLOT_A].successful, 0u, "default slot A not successful");
}

static void test_ab_boot_validate_rejects_bad_magic(void)
{
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    m.magic = 0xDEADBEEFu;  /* tamper after finalize */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "bad magic rejected");
}

static void test_ab_boot_validate_rejects_bad_version(void)
{
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    m.version = AB_BOOT_META_VERSION + 1u;
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "future version rejected");
}

static void test_ab_boot_validate_rejects_reserved_nonzero(void)
{
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    m.reserved = 1u;  /* forward-sentinel violation, CRC now also wrong */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "reserved-nonzero rejected");
}

static void test_ab_boot_validate_rejects_bad_active_slot(void)
{
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    m.active_slot = AB_BOOT_SLOT_COUNT;  /* out of range */
    ab_boot_meta_finalize(&m);           /* re-CRC so only the range check fires */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "out-of-range active_slot rejected");
}

static void test_ab_boot_validate_rejects_bad_crc(void)
{
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    m.slot[AB_BOOT_SLOT_B].tries ^= 0x5u;  /* flip payload, leave stored crc stale */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "payload tamper fails CRC");
}

static void test_ab_boot_crc_excludes_crc_field(void)
{
    /* Mutating only the crc32 field must not change the COMPUTED crc (it is
     * outside the covered range); is_valid then fails because stored != computed. */
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    unsigned int computed = ab_boot_meta_compute_crc(&m, AB_BOOT_META_SIZE);
    m.crc32 = computed ^ 0xFFFFFFFFu;
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_compute_crc(&m, AB_BOOT_META_SIZE),
                   (uint64_t)computed, "crc excludes the crc32 field itself");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "mismatched stored crc rejected");
}

static void test_ab_boot_select_newest_higher_generation_wins(void)
{
    struct ab_boot_metadata a, b;
    const struct ab_boot_metadata *win = (const struct ab_boot_metadata *)0;
    ab_make_valid(&a, 3u);
    ab_make_valid(&b, 7u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_select_newest(&a, &b, &win), 1u, "both valid -> select");
    TEST_ASSERT_EQ((uint64_t)(win == &b), 1u, "higher generation copy wins");
}

static void test_ab_boot_select_newest_one_invalid(void)
{
    struct ab_boot_metadata a, b;
    const struct ab_boot_metadata *win = (const struct ab_boot_metadata *)0;
    ab_make_valid(&a, 3u);
    ab_make_valid(&b, 7u);
    b.magic = 0u;  /* corrupt the newer copy */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_select_newest(&a, &b, &win), 1u, "one valid -> select");
    TEST_ASSERT_EQ((uint64_t)(win == &a), 1u, "the valid (older) copy is chosen");
}

static void test_ab_boot_select_newest_both_invalid(void)
{
    struct ab_boot_metadata a, b;
    const struct ab_boot_metadata *win = (const struct ab_boot_metadata *)0;
    ab_make_valid(&a, 3u);
    ab_make_valid(&b, 7u);
    a.magic = 0u;
    b.magic = 0u;
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_select_newest(&a, &b, &win), 0u, "both invalid -> 0");
}

static void test_ab_boot_validate_rejects_oob_tries(void)
{
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    m.slot[AB_BOOT_SLOT_B].tries = AB_BOOT_MAX_TRIES + 1u;
    ab_boot_meta_finalize(&m);  /* CRC valid; only the domain check fires */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "tries > MAX rejected");
}

static void test_ab_boot_validate_rejects_oob_successful(void)
{
    struct ab_boot_metadata m;
    ab_make_valid(&m, 1u);
    m.slot[AB_BOOT_SLOT_A].successful = 2u;  /* non-boolean */
    ab_boot_meta_finalize(&m);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_is_valid(&m), 0u, "successful > 1 rejected");
}

void test_register_ab_boot(void)
{
    test_suite_register_cat("ab_boot: default record is valid",
                            test_ab_boot_default_is_valid, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: validator rejects bad magic",
                            test_ab_boot_validate_rejects_bad_magic, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: validator rejects bad version",
                            test_ab_boot_validate_rejects_bad_version, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: validator rejects reserved-nonzero",
                            test_ab_boot_validate_rejects_reserved_nonzero, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: validator rejects out-of-range active_slot",
                            test_ab_boot_validate_rejects_bad_active_slot, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: validator rejects payload tamper (CRC)",
                            test_ab_boot_validate_rejects_bad_crc, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: validator rejects out-of-domain tries",
                            test_ab_boot_validate_rejects_oob_tries, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: validator rejects non-boolean successful",
                            test_ab_boot_validate_rejects_oob_successful, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: CRC excludes the crc32 field",
                            test_ab_boot_crc_excludes_crc_field, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: select newest -- higher generation wins",
                            test_ab_boot_select_newest_higher_generation_wins, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: select newest -- one invalid copy",
                            test_ab_boot_select_newest_one_invalid, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: select newest -- both invalid",
                            test_ab_boot_select_newest_both_invalid, TEST_CAT_BOOT);
}
