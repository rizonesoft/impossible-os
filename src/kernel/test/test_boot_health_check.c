/* ============================================================================
 * test_boot_health_check.c -- unit tests for per-entry health-gated mark-good
 *
 * PURE-HELPER TESTS ONLY. Per CLAUDE.md "Test Code Policy", these tests
 * must NEVER call uefi_var_set/get, RuntimeServices, vfs_*, klog (live
 * subsystems), boot_health_check_run, or any other live boot infrastructure.
 *
 * Allowed surface:
 *   - boot_health_handoff struct layout + size + offsets (compile + runtime)
 *   - boot_health_handoff_compute_crc()        pure function
 *   - boot_health_mark_good_is_valid()         pure function
 *   - boot_health_cur_boot_ctr_is_valid()      pure function
 *   - boot_health_subset_is_valid()            pure function
 *   - boot_health_check_aggregate()            pure function
 *   - boot_health_check_in_subset()            pure function
 *   - boot_health_check_register() + test_reset (registry mechanics)
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/test/test.h"
#include "kernel/boot_health_check.h"
#include "boot/boot_health_handoff.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);

/* ---- Schema asserts (mirror static_asserts in the header) -------------- */

static void test_mark_good_size(void)
{
    TEST_ASSERT_EQ(sizeof(struct boot_health_mark_good_record),
                    BOOT_HEALTH_MARK_GOOD_SIZE,
        "boot_health_mark_good_record must match BOOT_HEALTH_MARK_GOOD_SIZE");
    TEST_ASSERT_EQ(sizeof(struct boot_health_mark_good_record), 88u,
        "boot_health_mark_good_record must be exactly 88 bytes");
}

static void test_cur_boot_ctr_size(void)
{
    TEST_ASSERT_EQ(sizeof(struct boot_health_cur_boot_ctr_record),
                    BOOT_HEALTH_CUR_BOOT_CTR_SIZE,
        "boot_health_cur_boot_ctr_record must match BOOT_HEALTH_CUR_BOOT_CTR_SIZE");
}

static void test_mark_good_offsets(void)
{
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_health_mark_good_record, magic), 0,
        "magic at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_health_mark_good_record, entry_id), 8,
        "entry_id at offset 8");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_health_mark_good_record, tries_left), 72,
        "tries_left at offset 72");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_health_mark_good_record, crc32), 84,
        "crc32 at offset 84");
}

static void test_subset_offsets(void)
{
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_health_subset_record, magic), 0,
        "subset magic at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_health_subset_record, names), 16,
        "subset names at offset 16");
}

/* ---- Validator round-trip ---------------------------------------------- */

static void seed_mark_good(struct boot_health_mark_good_record *r)
{
    memset(r, 0, sizeof(*r));
    r->magic   = BOOT_HEALTH_MARK_GOOD_MAGIC;
    r->version = BOOT_HEALTH_VAR_VERSION;
    const char *id = "impossible-os-a";
    for (unsigned int i = 0; id[i] && i < BOOT_HEALTH_HANDOFF_ID_LEN - 1u; i++)
        r->entry_id[i] = id[i];
    r->tries_left = 2u;
    r->tries_done = 1u;
    r->reserved   = 0u;
    r->crc32      = boot_health_handoff_compute_crc(r,
                        (unsigned int)sizeof(*r));
}

static void test_mark_good_valid(void)
{
    struct boot_health_mark_good_record r;
    seed_mark_good(&r);
    TEST_ASSERT_EQ((uint32_t)boot_health_mark_good_is_valid(&r), 1u,
        "freshly-stamped MarkGood must validate");
}

static void test_mark_good_rejects_bad_magic(void)
{
    struct boot_health_mark_good_record r;
    seed_mark_good(&r);
    r.magic = 0xDEADBEEFu;
    TEST_ASSERT_EQ((uint32_t)boot_health_mark_good_is_valid(&r), 0u,
        "bad magic must invalidate");
}

static void test_mark_good_rejects_bad_version(void)
{
    struct boot_health_mark_good_record r;
    seed_mark_good(&r);
    r.version = 0u;
    /* CRC matches stale layout, but version check fires before CRC. */
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_mark_good_is_valid(&r), 0u,
        "version=0 must invalidate");
}

static void test_mark_good_rejects_empty_id(void)
{
    struct boot_health_mark_good_record r;
    seed_mark_good(&r);
    memset(r.entry_id, 0, sizeof(r.entry_id));
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_mark_good_is_valid(&r), 0u,
        "empty entry_id must invalidate");
}

static void test_mark_good_rejects_non_terminated_id(void)
{
    struct boot_health_mark_good_record r;
    seed_mark_good(&r);
    /* Fill entire id buffer with non-NUL bytes so the terminator scan
     * runs to the buffer end without finding a NUL. */
    for (unsigned int i = 0; i < BOOT_HEALTH_HANDOFF_ID_LEN; i++)
        r.entry_id[i] = 'x';
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_mark_good_is_valid(&r), 0u,
        "non-NUL-terminated entry_id must invalidate");
}

static void test_mark_good_rejects_nonzero_reserved(void)
{
    struct boot_health_mark_good_record r;
    seed_mark_good(&r);
    r.reserved = 1u;
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_mark_good_is_valid(&r), 0u,
        "non-zero reserved must invalidate (forward-compat sentinel)");
}

static void test_mark_good_rejects_byte_flip(void)
{
    struct boot_health_mark_good_record r;
    seed_mark_good(&r);
    r.tries_left ^= 1u;
    /* Don't recompute CRC -- a real byte flip must be caught. */
    TEST_ASSERT_EQ((uint32_t)boot_health_mark_good_is_valid(&r), 0u,
        "byte flip without CRC update must invalidate");
}

static void test_cur_boot_ctr_distinct_magic(void)
{
    /* A MarkGood-magic record must NOT validate as a CurBootCtr record,
     * even if all other fields match. Distinct magics surface
     * cross-wired records cleanly. */
    struct boot_health_cur_boot_ctr_record r;
    memset(&r, 0, sizeof(r));
    r.magic   = BOOT_HEALTH_MARK_GOOD_MAGIC;   /* WRONG magic for this struct */
    r.version = BOOT_HEALTH_VAR_VERSION;
    r.entry_id[0] = 'x';
    r.tries_left = 1u;
    r.tries_done = 1u;
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_cur_boot_ctr_is_valid(&r), 0u,
        "MarkGood magic in CurBootCtr buffer must be rejected");
}

/* ---- Subset validator -------------------------------------------------- */

static void seed_subset(struct boot_health_subset_record *r,
                        const char *names[], unsigned int n)
{
    memset(r, 0, sizeof(*r));
    r->magic   = BOOT_HEALTH_SUBSET_MAGIC;
    r->version = BOOT_HEALTH_VAR_VERSION;
    if (n > BOOT_HEALTH_SUBSET_MAX_NAMES) n = BOOT_HEALTH_SUBSET_MAX_NAMES;
    r->count = n;
    for (unsigned int i = 0; i < n; i++) {
        unsigned int j;
        for (j = 0; j < BOOT_HEALTH_SUBSET_NAME_LEN - 1u; j++) {
            char c = names[i][j];
            if (c == 0) break;
            r->names[i][j] = c;
        }
        r->names[i][j] = 0;
    }
    r->crc32 = boot_health_handoff_compute_crc(r,
                  (unsigned int)sizeof(*r));
}

static void test_subset_valid(void)
{
    struct boot_health_subset_record r;
    const char *names[] = { "desktop_ready", "no_panic" };
    seed_subset(&r, names, 2);
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 1u,
        "seeded subset must validate");
}

static void test_subset_empty_valid(void)
{
    struct boot_health_subset_record r;
    seed_subset(&r, (const char **)0, 0);
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 1u,
        "count==0 subset must validate (kernel interprets as default)");
}

static void test_subset_rejects_overflow_count(void)
{
    struct boot_health_subset_record r;
    seed_subset(&r, (const char **)0, 0);
    r.count = BOOT_HEALTH_SUBSET_MAX_NAMES + 1u;
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 0u,
        "count > MAX_NAMES must invalidate");
}

static void test_subset_rejects_empty_name(void)
{
    struct boot_health_subset_record r;
    seed_subset(&r, (const char **)0, 0);
    r.count = 1u;
    /* names[0] left as all-zero by memset, so the first byte is NUL */
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 0u,
        "empty name in populated slot must invalidate");
}

static void test_subset_rejects_unzeroed_unused_slot(void)
{
    /* Producer-bug guard: stale bytes in names[count..MAX) must cause
     * the validator to reject the record even when the CRC matches. */
    struct boot_health_subset_record r;
    const char *names[] = { "desktop_ready" };
    seed_subset(&r, names, 1);
    /* Poke a stale byte in slot 1, recompute CRC, and confirm reject. */
    r.names[1][3] = 'x';
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 0u,
        "garbage in names[count..MAX) must invalidate");
}

static void test_subset_rejects_non_printable_name(void)
{
    /* Names must be printable ASCII (matches validate.py + parser
     * grammar). A control char in a populated slot must reject. */
    struct boot_health_subset_record r;
    const char *names[] = { "desktop_ready" };
    seed_subset(&r, names, 1);
    r.names[0][3] = 0x01;  /* control char */
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 0u,
        "control char in name must invalidate");
}

static void test_subset_rejects_backslash_in_name(void)
{
    struct boot_health_subset_record r;
    const char *names[] = { "desktop_ready" };
    seed_subset(&r, names, 1);
    r.names[0][3] = '\\';
    r.crc32 = boot_health_handoff_compute_crc(&r,
                  (unsigned int)sizeof(r));
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 0u,
        "backslash in name must invalidate (registry-name safety)");
}

static void test_subset_boundary_max_count(void)
{
    /* Exactly MAX_NAMES populated must validate. */
    struct boot_health_subset_record r;
    const char *names[BOOT_HEALTH_SUBSET_MAX_NAMES];
    static const char *fill[] = {
        "n1", "n2", "n3", "n4", "n5", "n6", "n7", "n8"
    };
    for (unsigned int i = 0; i < BOOT_HEALTH_SUBSET_MAX_NAMES; i++)
        names[i] = fill[i];
    seed_subset(&r, names, BOOT_HEALTH_SUBSET_MAX_NAMES);
    TEST_ASSERT_EQ((uint32_t)boot_health_subset_is_valid(&r), 1u,
        "count==MAX must validate when all slots are well-formed");
}

/* ---- Aggregator -------------------------------------------------------- */

static void test_aggregate_all_ok_passes(void)
{
    enum boot_health_aggregate a =
        boot_health_check_aggregate(3, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)a, (uint32_t)BOOT_HEALTH_AGG_PASS,
        "all required OK -> PASS");
}

static void test_aggregate_hard_fail_blocks(void)
{
    enum boot_health_aggregate a =
        boot_health_check_aggregate(2, 0, 1, 0);
    TEST_ASSERT_EQ((uint32_t)a, (uint32_t)BOOT_HEALTH_AGG_INDETERMINATE,
        "any required HARD_FAIL -> INDETERMINATE");
}

static void test_aggregate_soft_fail_blocks(void)
{
    enum boot_health_aggregate a =
        boot_health_check_aggregate(2, 1, 0, 0);
    TEST_ASSERT_EQ((uint32_t)a, (uint32_t)BOOT_HEALTH_AGG_INDETERMINATE,
        "required SOFT_FAIL is not a clean pass -> INDETERMINATE");
}

static void test_aggregate_skipped_blocks(void)
{
    enum boot_health_aggregate a =
        boot_health_check_aggregate(2, 0, 0, 1);
    TEST_ASSERT_EQ((uint32_t)a, (uint32_t)BOOT_HEALTH_AGG_INDETERMINATE,
        "required SKIPPED -> INDETERMINATE (cannot prove good)");
}

static void test_aggregate_no_required_blocks(void)
{
    enum boot_health_aggregate a =
        boot_health_check_aggregate(0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)a, (uint32_t)BOOT_HEALTH_AGG_INDETERMINATE,
        "zero required checks ran -> INDETERMINATE");
}

/* ---- Subset filter ---------------------------------------------------- */

static void test_subset_filter_empty_runs_all(void)
{
    char names[1][BOOT_HEALTH_CHECK_NAME_LEN];
    TEST_ASSERT_EQ((uint32_t)boot_health_check_in_subset(
                    "desktop_ready", names, 0u), 1u,
        "empty subset must run every check");
}

static void test_subset_filter_matches(void)
{
    char names[2][BOOT_HEALTH_CHECK_NAME_LEN];
    memset(names, 0, sizeof(names));
    const char *a = "desktop_ready";
    const char *b = "no_panic";
    for (unsigned int i = 0; a[i] && i < BOOT_HEALTH_CHECK_NAME_LEN - 1u; i++)
        names[0][i] = a[i];
    for (unsigned int i = 0; b[i] && i < BOOT_HEALTH_CHECK_NAME_LEN - 1u; i++)
        names[1][i] = b[i];
    TEST_ASSERT_EQ((uint32_t)boot_health_check_in_subset(
                    "no_panic", names, 2u), 1u,
        "name in subset must match");
    TEST_ASSERT_EQ((uint32_t)boot_health_check_in_subset(
                    "x_mountable", names, 2u), 0u,
        "name NOT in subset must not match");
}

static void test_subset_filter_rejects_null(void)
{
    TEST_ASSERT_EQ((uint32_t)boot_health_check_in_subset(
                    (const char *)0, (const char (*)[BOOT_HEALTH_CHECK_NAME_LEN])0, 0u),
                    0u,
        "NULL name must not match");
}

/* ---- Registry mechanics ----------------------------------------------- */

static enum boot_health_check_result dummy_ok(void)
{
    return BOOT_HEALTH_OK;
}

static enum boot_health_check_result dummy_soft(void)
{
    return BOOT_HEALTH_SOFT_FAIL;
}

static void test_registry_register_and_count(void)
{
    boot_health_check_test_reset();
    TEST_ASSERT_EQ((uint32_t)boot_health_check_registered_count(), 0u,
        "registry must start empty after reset");
    int ok = boot_health_check_register("test_alpha",
                                          BOOT_HEALTH_KIND_REQUIRED,
                                          dummy_ok);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "register must succeed");
    TEST_ASSERT_EQ((uint32_t)boot_health_check_registered_count(), 1u,
        "count must reflect successful register");
    boot_health_check_test_reset();
}

static void test_registry_rejects_duplicate(void)
{
    boot_health_check_test_reset();
    (void)boot_health_check_register("test_dup",
                                      BOOT_HEALTH_KIND_REQUIRED, dummy_ok);
    int dup = boot_health_check_register("test_dup",
                                          BOOT_HEALTH_KIND_WANTED, dummy_soft);
    TEST_ASSERT_EQ((uint32_t)dup, 0u,
        "duplicate name must be rejected");
    TEST_ASSERT_EQ((uint32_t)boot_health_check_registered_count(), 1u,
        "count must not increase on duplicate");
    boot_health_check_test_reset();
}

static void test_registry_rejects_bad_kind(void)
{
    boot_health_check_test_reset();
    int bad = boot_health_check_register("test_bad_kind",
                                          (enum boot_health_check_kind)99,
                                          dummy_ok);
    TEST_ASSERT_EQ((uint32_t)bad, 0u,
        "invalid kind must be rejected");
    boot_health_check_test_reset();
}

static void test_registry_rejects_null_fn(void)
{
    boot_health_check_test_reset();
    int bad = boot_health_check_register("test_null_fn",
                                          BOOT_HEALTH_KIND_REQUIRED,
                                          (boot_health_check_fn)0);
    TEST_ASSERT_EQ((uint32_t)bad, 0u, "NULL fn must be rejected");
    boot_health_check_test_reset();
}

static void test_registry_rejects_empty_name(void)
{
    boot_health_check_test_reset();
    int bad = boot_health_check_register("",
                                          BOOT_HEALTH_KIND_REQUIRED, dummy_ok);
    TEST_ASSERT_EQ((uint32_t)bad, 0u, "empty name must be rejected");
    boot_health_check_test_reset();
}

static void test_registry_full_capacity(void)
{
    boot_health_check_test_reset();
    char name[BOOT_HEALTH_CHECK_NAME_LEN];
    /* Fill to capacity. Names are unique per slot. */
    for (unsigned int i = 0; i < BOOT_HEALTH_CHECK_MAX; i++) {
        unsigned int j;
        for (j = 0; j < sizeof(name) - 1u; j++) name[j] = 0;
        name[0] = 'c'; name[1] = 'h'; name[2] = 'k';
        /* Two-digit decimal index: tens + units. */
        name[3] = (char)('0' + (i / 10) % 10);
        name[4] = (char)('0' + (i % 10));
        name[5] = 0;
        int ok = boot_health_check_register(name,
                                              BOOT_HEALTH_KIND_REQUIRED,
                                              dummy_ok);
        TEST_ASSERT_EQ((uint32_t)ok, 1u,
            "registration within cap must succeed");
    }
    int over = boot_health_check_register("over_cap",
                                            BOOT_HEALTH_KIND_REQUIRED,
                                            dummy_ok);
    TEST_ASSERT_EQ((uint32_t)over, 0u,
        "registration past cap must fail");
    boot_health_check_test_reset();
}

static void test_registry_defaults_idempotent(void)
{
    boot_health_check_test_reset();
    boot_health_check_register_defaults();
    unsigned int first = boot_health_check_registered_count();
    boot_health_check_register_defaults();
    unsigned int second = boot_health_check_registered_count();
    TEST_ASSERT_EQ((uint32_t)second, (uint32_t)first,
        "second register_defaults must be a no-op");
    TEST_ASSERT_EQ((uint32_t)first, 6u,
        "defaults must register exactly 6 checks");
    boot_health_check_test_reset();
}

void test_register_boot_health_check(void);
void test_register_boot_health_check(void)
{
    test_suite_register_cat("boot_health_check mark_good size",
        test_mark_good_size, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check cur_boot_ctr size",
        test_cur_boot_ctr_size, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good offsets",
        test_mark_good_offsets, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset offsets",
        test_subset_offsets, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good valid",
        test_mark_good_valid, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good rejects bad magic",
        test_mark_good_rejects_bad_magic, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good rejects bad version",
        test_mark_good_rejects_bad_version, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good rejects empty id",
        test_mark_good_rejects_empty_id, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good rejects non-terminated id",
        test_mark_good_rejects_non_terminated_id, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good rejects nonzero reserved",
        test_mark_good_rejects_nonzero_reserved, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check mark_good rejects byte flip",
        test_mark_good_rejects_byte_flip, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check cur_boot_ctr distinct magic",
        test_cur_boot_ctr_distinct_magic, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset valid",
        test_subset_valid, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset empty valid",
        test_subset_empty_valid, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset rejects overflow count",
        test_subset_rejects_overflow_count, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset rejects empty name",
        test_subset_rejects_empty_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset rejects unzeroed unused slot",
        test_subset_rejects_unzeroed_unused_slot, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset rejects non-printable name",
        test_subset_rejects_non_printable_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset rejects backslash in name",
        test_subset_rejects_backslash_in_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset boundary max count",
        test_subset_boundary_max_count, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check aggregate all OK passes",
        test_aggregate_all_ok_passes, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check aggregate hard_fail blocks",
        test_aggregate_hard_fail_blocks, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check aggregate soft_fail blocks",
        test_aggregate_soft_fail_blocks, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check aggregate skipped blocks",
        test_aggregate_skipped_blocks, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check aggregate no required blocks",
        test_aggregate_no_required_blocks, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset filter empty runs all",
        test_subset_filter_empty_runs_all, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset filter matches",
        test_subset_filter_matches, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check subset filter rejects NULL",
        test_subset_filter_rejects_null, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check registry register and count",
        test_registry_register_and_count, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check registry rejects duplicate",
        test_registry_rejects_duplicate, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check registry rejects bad kind",
        test_registry_rejects_bad_kind, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check registry rejects NULL fn",
        test_registry_rejects_null_fn, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check registry rejects empty name",
        test_registry_rejects_empty_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check registry full capacity",
        test_registry_full_capacity, TEST_CAT_BOOT);
    test_suite_register_cat("boot_health_check defaults idempotent",
        test_registry_defaults_idempotent, TEST_CAT_BOOT);
}
