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
#include "kernel/fs/gpt.h"   /* gpt_ixfs_slot + IXFS slot type GUIDs (TODO-21 sec 3) */

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

/* ---- Selection state model (sec 3 ab_boot_meta_choose_slot) ---- */

/* Build a finalized record with explicit per-slot state. */
static void ab_make_record(struct ab_boot_metadata *m, unsigned int active,
                           unsigned int a_tries, unsigned int a_succ, unsigned int a_pri,
                           unsigned int b_tries, unsigned int b_succ, unsigned int b_pri)
{
    unsigned int i;
    for (i = 0u; i < AB_BOOT_META_SIZE; i++) ((unsigned char *)m)[i] = 0u;
    m->active_slot = active;
    m->slot[AB_BOOT_SLOT_A].tries = a_tries;
    m->slot[AB_BOOT_SLOT_A].successful = a_succ;
    m->slot[AB_BOOT_SLOT_A].priority = a_pri;
    m->slot[AB_BOOT_SLOT_B].tries = b_tries;
    m->slot[AB_BOOT_SLOT_B].successful = b_succ;
    m->slot[AB_BOOT_SLOT_B].priority = b_pri;
    ab_boot_meta_finalize(m);
}

static void test_ab_choose_fresh_prefers_a(void)
{
    struct ab_boot_metadata m;
    /* Factory shape: A pending priority 1, B blank. */
    ab_make_record(&m, AB_BOOT_SLOT_A, 0u, 0u, 1u, 0u, 0u, 0u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_choose_slot(&m), AB_BOOT_SLOT_A,
                   "fresh metadata boots Slot A");
}

static void test_ab_choose_rolls_back_exhausted_slot(void)
{
    struct ab_boot_metadata m;
    /* A exhausted (tries=MAX, never successful), B pending and viable.
     * Even though A has higher priority, A is unbootable -> choose B. */
    ab_make_record(&m, AB_BOOT_SLOT_A, AB_BOOT_MAX_TRIES, 0u, 5u, 0u, 0u, 1u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_choose_slot(&m), AB_BOOT_SLOT_B,
                   "exhausted active slot rolls back to the viable slot");
}

static void test_ab_choose_rolls_back_once_successful_exhausted(void)
{
    struct ab_boot_metadata m;
    /* A was marked successful but has since failed MAX times (tries=MAX),
     * B is viable. The exhausted slot must roll back EVEN THOUGH successful=1
     * and A has higher priority -- a once-good slot that later broke. */
    ab_make_record(&m, AB_BOOT_SLOT_A, AB_BOOT_MAX_TRIES, 1u, 9u, 0u, 0u, 1u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_choose_slot(&m), AB_BOOT_SLOT_B,
                   "exhausted once-successful slot still rolls back");
}

static void test_ab_choose_higher_priority_wins(void)
{
    struct ab_boot_metadata m;
    /* Both viable + successful; B has higher priority. */
    ab_make_record(&m, AB_BOOT_SLOT_A, 0u, 1u, 1u, 0u, 1u, 9u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_choose_slot(&m), AB_BOOT_SLOT_B,
                   "higher priority wins when both viable");
}

static void test_ab_choose_successful_breaks_priority_tie(void)
{
    struct ab_boot_metadata m;
    /* Equal priority; A successful, B only pending -> prefer successful A. */
    ab_make_record(&m, AB_BOOT_SLOT_B, 0u, 1u, 3u, 1u, 0u, 3u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_choose_slot(&m), AB_BOOT_SLOT_A,
                   "successful slot beats pending at equal priority");
}

static void test_ab_choose_lower_tries_breaks_tie(void)
{
    struct ab_boot_metadata m;
    /* Equal priority + both not-successful; A has more tries consumed. */
    ab_make_record(&m, AB_BOOT_SLOT_A, 2u, 0u, 1u, 0u, 0u, 1u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_choose_slot(&m), AB_BOOT_SLOT_B,
                   "lower-tries slot wins the final tie-break");
}

static void test_ab_choose_both_exhausted_falls_back_to_active(void)
{
    struct ab_boot_metadata m;
    /* Both unbootable -> fall back to the recorded active_slot (B here). */
    ab_make_record(&m, AB_BOOT_SLOT_B, AB_BOOT_MAX_TRIES, 0u, 1u,
                   AB_BOOT_MAX_TRIES, 0u, 1u);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_choose_slot(&m), AB_BOOT_SLOT_B,
                   "both exhausted -> recorded active_slot");
}

/* ---- GPT primary/backup reconciliation (sec 3 ab_boot_meta_reconcile_gpt) ---- */

static void test_ab_reconcile_primary_has_md(void)
{
    /* primary valid + has MD -> use primary regardless of backup. */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(1, 1, 1, 1),
                   (uint64_t)AB_GPT_USE_PRIMARY, "primary valid+MD -> USE_PRIMARY");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(1, 1, 0, 0),
                   (uint64_t)AB_GPT_USE_PRIMARY, "primary valid+MD, backup invalid -> USE_PRIMARY");
}

static void test_ab_reconcile_backup_recovers(void)
{
    /* primary invalid + backup valid+MD -> recover from backup. */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(0, 0, 1, 1),
                   (uint64_t)AB_GPT_USE_BACKUP, "primary invalid, backup valid+MD -> USE_BACKUP");
}

static void test_ab_reconcile_both_valid_no_md(void)
{
    /* both valid + both no MD -> legitimate non-A/B disk. */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(1, 0, 1, 0),
                   (uint64_t)AB_GPT_NO_AB, "both valid, both no-MD -> NO_AB");
}

static void test_ab_reconcile_corrupt_states_fail_closed(void)
{
    /* Every state that is not cleanly authoritative/recoverable/agreeing is
     * FATAL -- the rollback-safety fail-closed contract. */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(0, 0, 0, 0),
                   (uint64_t)AB_GPT_FATAL, "both invalid -> FATAL");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(1, 0, 1, 1),
                   (uint64_t)AB_GPT_FATAL, "primary no-MD, backup has-MD (split-brain) -> FATAL");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(1, 1, 1, 0),
                   (uint64_t)AB_GPT_FATAL, "primary has-MD, backup valid no-MD (split-brain) -> FATAL");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(1, 0, 0, 0),
                   (uint64_t)AB_GPT_FATAL, "primary valid no-MD, backup invalid -> FATAL");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_reconcile_gpt(0, 0, 1, 0),
                   (uint64_t)AB_GPT_FATAL, "primary invalid, backup valid no-MD -> FATAL");
}

/* ---- Slot identity from GPT type GUID (sec 3 gpt_ixfs_slot) ---- */

static void test_ab_gpt_ixfs_slot_a(void)
{
    TEST_ASSERT_EQ((uint64_t)(int64_t)gpt_ixfs_slot(&GPT_GUID_IXFS), 0u,
                   "Slot A IXFS GUID -> slot 0");
}

static void test_ab_gpt_ixfs_slot_b(void)
{
    TEST_ASSERT_EQ((uint64_t)(int64_t)gpt_ixfs_slot(&GPT_GUID_IXFS_B), 1u,
                   "Slot B IXFS GUID -> slot 1");
}

static void test_ab_gpt_ixfs_slot_non_ixfs(void)
{
    /* A non-IXFS-family GUID returns -1 (not a slot). */
    TEST_ASSERT_EQ((uint64_t)(int64_t)gpt_ixfs_slot(&GPT_GUID_EFI_SYSTEM),
                   (uint64_t)(int64_t)-1, "EFI System GUID -> not a slot (-1)");
}

/* ---- Power-fail-atomic write target + generation (sec 7) ---- */

static void test_ab_write_target_overwrites_invalid_first(void)
{
    /* An invalid copy is safe to overwrite (carries no good state). */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_write_target(0, 0u, 1, 5u), 0u,
                   "copy0 invalid -> overwrite copy0");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_write_target(1, 5u, 0, 0u), 1u,
                   "copy1 invalid -> overwrite copy1");
}

static void test_ab_write_target_overwrites_older_when_both_valid(void)
{
    /* Both valid: overwrite the LOWER-generation copy so the newer survives. */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_write_target(1, 3u, 1, 7u), 0u,
                   "copy0 older (gen 3 < 7) -> overwrite copy0");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_write_target(1, 9u, 1, 4u), 1u,
                   "copy1 older (gen 4 < 9) -> overwrite copy1");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_write_target(1, 6u, 1, 6u), 0u,
                   "generation tie -> deterministic copy0");
}

static void test_ab_next_generation_strictly_greater(void)
{
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_next_generation(0, 0u, 0, 0u), 1u,
                   "no valid copy (blank/corrupt) -> generation 1");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_next_generation(1, 4u, 0, 0u), 5u,
                   "one valid gen 4 -> 5");
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_next_generation(1, 4u, 1, 9u), 10u,
                   "max(4,9)+1 = 10");
}

static void test_ab_next_generation_ceiling(void)
{
    /* hi == 0xFFFFFFFE is still writable: +1 = 0xFFFFFFFF, strictly greater. */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_next_generation(1, 0xFFFFFFFEu, 0, 0u),
                   0xFFFFFFFFu, "gen 0xFFFFFFFE -> 0xFFFFFFFF (still writable)");
    /* hi == 0xFFFFFFFF is exhausted: no strictly-greater value -> refuse (0). */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_next_generation(1, 0xFFFFFFFFu, 0, 0u),
                   0u, "gen 0xFFFFFFFF exhausted -> refuse sentinel 0");
    /* An INVALID copy at 0xFFFFFFFF is ignored (only valid copies bound hi). */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_next_generation(0, 0xFFFFFFFFu, 1, 3u),
                   4u, "invalid copy ignored -> max(valid)=3 -> 4");
}

static void test_ab_write_compose_at_ceiling_wins(void)
{
    /* Interaction: copy0 valid gen 0xFFFFFFFE, copy1 invalid. write_target picks
     * copy1; next_generation = 0xFFFFFFFF. select_newest must then pick the
     * just-written copy1 (0xFFFFFFFF > 0xFFFFFFFE), not the preserved copy0. */
    struct ab_boot_metadata c0, c1;
    const struct ab_boot_metadata *win = (const struct ab_boot_metadata *)0;
    ab_make_valid(&c0, 0xFFFFFFFEu);     /* valid, gen 0xFFFFFFFE */
    for (unsigned int i = 0u; i < AB_BOOT_META_SIZE; i++)
        ((unsigned char *)&c1)[i] = 0u;  /* copy1 invalid (magic 0) */
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_write_target(1, 0xFFFFFFFEu, 0, 0u), 1u,
                   "ceiling: overwrite the invalid copy1");
    unsigned int ng = ab_boot_meta_next_generation(1, 0xFFFFFFFEu, 0, 0u);
    TEST_ASSERT_EQ((uint64_t)ng, 0xFFFFFFFFu, "ceiling: new gen 0xFFFFFFFF");
    /* Simulate the write landing in copy1 at the new generation. */
    ab_make_valid(&c1, ng);
    TEST_ASSERT_EQ((uint64_t)ab_boot_meta_select_newest(&c0, &c1, &win), 1u,
                   "both valid after ceiling write");
    TEST_ASSERT_EQ((uint64_t)(win == &c1), 1u,
                   "just-written copy1 (0xFFFFFFFF) wins over preserved copy0");
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
    test_suite_register_cat("ab_boot: choose slot -- fresh prefers A",
                            test_ab_choose_fresh_prefers_a, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: choose slot -- rolls back exhausted slot",
                            test_ab_choose_rolls_back_exhausted_slot, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: choose slot -- rolls back once-successful exhausted slot",
                            test_ab_choose_rolls_back_once_successful_exhausted, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: choose slot -- higher priority wins",
                            test_ab_choose_higher_priority_wins, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: choose slot -- successful breaks priority tie",
                            test_ab_choose_successful_breaks_priority_tie, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: choose slot -- lower tries breaks tie",
                            test_ab_choose_lower_tries_breaks_tie, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: choose slot -- both exhausted falls back to active",
                            test_ab_choose_both_exhausted_falls_back_to_active, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: reconcile GPT -- primary has metadata",
                            test_ab_reconcile_primary_has_md, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: reconcile GPT -- backup recovers primary",
                            test_ab_reconcile_backup_recovers, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: reconcile GPT -- both valid no metadata",
                            test_ab_reconcile_both_valid_no_md, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: reconcile GPT -- corrupt states fail closed",
                            test_ab_reconcile_corrupt_states_fail_closed, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: gpt_ixfs_slot -- Slot A GUID",
                            test_ab_gpt_ixfs_slot_a, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: gpt_ixfs_slot -- Slot B GUID",
                            test_ab_gpt_ixfs_slot_b, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: gpt_ixfs_slot -- non-IXFS GUID rejected",
                            test_ab_gpt_ixfs_slot_non_ixfs, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: write target -- overwrites invalid copy first",
                            test_ab_write_target_overwrites_invalid_first, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: write target -- overwrites older copy when both valid",
                            test_ab_write_target_overwrites_older_when_both_valid, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: next generation -- strictly greater than valid copies",
                            test_ab_next_generation_strictly_greater, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: next generation -- ceiling (0xFFFFFFFE writable, 0xFFFFFFFF refused)",
                            test_ab_next_generation_ceiling, TEST_CAT_BOOT);
    test_suite_register_cat("ab_boot: write compose -- just-written copy wins at ceiling",
                            test_ab_write_compose_at_ceiling_wins, TEST_CAT_BOOT);
}
