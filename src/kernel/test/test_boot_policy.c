/*
 * src/kernel/test/test_boot_policy.c -- Unit tests for boot policy ladder
 *
 * Tests run through the kernel test runner (TEST_CAT_BOOT). The ladder is
 * a pure C function that is cross-included from the bootloader source so
 * the same code-path exercised here is the one shipped in BOOTX64.EFI.
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/types.h"

/* Pull the parser + ladder source into the test object so we exercise the
 * same code-path the bootloader links. Both files are pure C and have no
 * UEFI types; this is the same pattern test_boot_entry_parser.c uses. */
#include "../../boot/uefi/boot_policy.c"

/* ---- Counter filename round-trip --------------------------------------- */

static void test_counter_filename_roundtrip(void)
{
    boot_counter_t in;
    in.tries_left = 3u;
    in.tries_done = 7u;
    /* memset-equivalent inline */
    for (unsigned i = 0; i < sizeof(in.id); i++) in.id[i] = 0;
    const char *name = "default-os";
    for (unsigned i = 0; name[i]; i++) in.id[i] = name[i];

    char buf[64];
    unsigned int n = boot_counter_format_filename(&in, buf, sizeof(buf));
    TEST_ASSERT(n > 0u, "format succeeds");

    boot_counter_t out;
    int rc = boot_counter_parse_filename(buf, n, &out);
    TEST_ASSERT(rc == 1, "parse round-trip succeeds");
    TEST_ASSERT_EQ(out.tries_left, in.tries_left, "tries_left preserved");
    TEST_ASSERT_EQ(out.tries_done, in.tries_done, "tries_done preserved");
    TEST_ASSERT(out.id[0] == 'd' && out.id[1] == 'e', "id preserved");
}

static void test_counter_filename_two_digit_done(void)
{
    /* Boundary: tries_done in [10..99] uses two digits in the filename.
     * Filename "a+0-42" is exactly 6 bytes excluding NUL. */
    boot_counter_t in = { .tries_left = 0u, .tries_done = 42u };
    in.id[0] = 'a'; in.id[1] = 0;
    char buf[64];
    unsigned int n = boot_counter_format_filename(&in, buf, sizeof(buf));
    TEST_ASSERT_EQ(n, 6u, "'a+0-42' length is 6");
    TEST_ASSERT(buf[2] == '0' && buf[3] == '-' && buf[4] == '4' && buf[5] == '2',
                "format suffix '0-42'");

    /* Round-trip parse must recover the original counter. */
    boot_counter_t out;
    int rc = boot_counter_parse_filename(buf, n, &out);
    TEST_ASSERT(rc == 1, "two-digit done round-trip parses");
    TEST_ASSERT_EQ(out.tries_done, 42u, "two-digit done preserved");
}

static void test_counter_filename_rejects_bad_grammar(void)
{
    boot_counter_t out;
    /* Empty id */
    TEST_ASSERT(boot_counter_parse_filename("+0-0", 4u, &out) == 0,
                "empty id rejected");
    /* No '+' separator */
    TEST_ASSERT(boot_counter_parse_filename("foo0-0", 6u, &out) == 0,
                "missing '+' rejected");
    /* tries_left out of range (>9 in single-digit grammar) */
    TEST_ASSERT(boot_counter_parse_filename("foo+a-0", 7u, &out) == 0,
                "non-digit tries_left rejected");
    /* Trailing garbage */
    TEST_ASSERT(boot_counter_parse_filename("foo+0-0z", 8u, &out) == 0,
                "trailing junk rejected");
    /* Consecutive dashes in id */
    TEST_ASSERT(boot_counter_parse_filename("foo--bar+0-0", 12u, &out) == 0,
                "consecutive dashes rejected");
    /* Trailing dash on id */
    TEST_ASSERT(boot_counter_parse_filename("foo-+0-0", 8u, &out) == 0,
                "trailing dash on id rejected");
    /* Leading dash on id */
    TEST_ASSERT(boot_counter_parse_filename("-foo+0-0", 8u, &out) == 0,
                "leading dash on id rejected");
}

static void test_counter_filename_boundary_caps(void)
{
    boot_counter_t out;
    /* tries_left == 9 (max) accepted */
    TEST_ASSERT(boot_counter_parse_filename("foo+9-0", 7u, &out) == 1,
                "tries_left=9 accepted");
    TEST_ASSERT_EQ(out.tries_left, 9u, "tries_left max preserved");
    /* tries_done == 99 (max) accepted */
    TEST_ASSERT(boot_counter_parse_filename("foo+0-99", 8u, &out) == 1,
                "tries_done=99 accepted");
    TEST_ASSERT_EQ(out.tries_done, 99u, "tries_done max preserved");
    /* tries_done == 100 (3 digits) rejected -- single-or-two-digit grammar */
    TEST_ASSERT(boot_counter_parse_filename("foo+0-100", 9u, &out) == 0,
                "3-digit tries_done rejected");

    /* Formatter: above-cap values rejected */
    boot_counter_t bad;
    bad.id[0] = 'a'; bad.id[1] = 0;
    char fbuf[64];
    bad.tries_left = 10u; bad.tries_done = 0u;
    TEST_ASSERT_EQ(boot_counter_format_filename(&bad, fbuf, sizeof(fbuf)), 0u,
                   "tries_left>9 rejected by formatter");
    bad.tries_left = 0u; bad.tries_done = 100u;
    TEST_ASSERT_EQ(boot_counter_format_filename(&bad, fbuf, sizeof(fbuf)), 0u,
                   "tries_done>99 rejected by formatter");

    /* Formatter: insufficient capacity */
    bad.tries_left = 0u; bad.tries_done = 0u;
    /* Need 1 (id) + 1 (+) + 1 (L) + 1 (-) + 1 (D) + 1 (NUL) = 6; cap 5 fails */
    TEST_ASSERT_EQ(boot_counter_format_filename(&bad, fbuf, 5u), 0u,
                   "below-minimum capacity rejected");

    /* Formatter: NULL inputs rejected */
    TEST_ASSERT_EQ(boot_counter_format_filename(((const boot_counter_t *)0),
                                                fbuf, sizeof(fbuf)), 0u,
                   "NULL counter rejected");
    TEST_ASSERT_EQ(boot_counter_format_filename(&bad, ((char *)0), sizeof(fbuf)), 0u,
                   "NULL output buffer rejected");

    /* Parser: NULL inputs rejected */
    TEST_ASSERT(boot_counter_parse_filename(((const char *)0), 7u, &out) == 0,
                "NULL name rejected");
    TEST_ASSERT(boot_counter_parse_filename("foo+0-0", 7u,
                                            ((boot_counter_t *)0)) == 0,
                "NULL out rejected");
    TEST_ASSERT(boot_counter_parse_filename("foo+0-0", 0u, &out) == 0,
                "zero-length name rejected");
}

/* ---- Ladder fixtures --------------------------------------------------- */

static void zero_envelope(boot_entry_envelope_t *e)
{
    for (unsigned int i = 0; i < sizeof(*e); i++) ((unsigned char *)e)[i] = 0;
    e->timeout_override = BOOT_ENTRIES_TIMEOUT_OVERRIDE_NONE;
}

static void zero_inputs(boot_policy_inputs_t *in)
{
    for (unsigned int i = 0; i < sizeof(*in); i++) ((unsigned char *)in)[i] = 0;
    in->boot_current = 0xFFFFu;
    in->ab_slot_index = BOOT_POLICY_NO_AB_SLOT;
}

static void set_id(char dst[64], const char *src)
{
    unsigned int i;
    for (i = 0; i < 63u && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ---- Ladder tests ------------------------------------------------------ */

static void test_ladder_store_default_picks_lowest_sort_key(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "second");
    set_id(r.entries[0].sort_key, "20");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "first");
    set_id(r.entries[1].sort_key, "10");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "store-default reason");
    TEST_ASSERT(d.selected_entry_id[0] == 'f', "lowest sort_key picked (first)");
}

static void test_ladder_inactive_filtered_into_rejected(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "inactive");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = 0u;             /* not active */
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "active");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT, "active wins");
    TEST_ASSERT(d.selected_entry_id[0] == 'a', "active id picked");
    TEST_ASSERT_EQ(d.rejected_count, 1u, "one rejection recorded");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_NOT_ACTIVE,
                   "reject reason = NOT_ACTIVE");
}

static void test_ladder_machine_id_filter(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "wrong-host");
    set_id(r.entries[0].sort_key, "10");
    set_id(r.entries[0].machine_id, "11111111-1111-1111-1111-111111111111");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "right-host");
    set_id(r.entries[1].sort_key, "20");
    set_id(r.entries[1].machine_id, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    set_id(in.local_machine_id, "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "machine match wins");
    TEST_ASSERT(d.selected_entry_id[0] == 'r', "right-host picked");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_MACHINE_ID_MISMATCH,
                   "reject reason = MACHINE_ID_MISMATCH");
}

static void test_ladder_tries_exhausted(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "doomed");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_counter_t counters[1];
    set_id(counters[0].id, "doomed");
    counters[0].tries_left = 0u;
    counters[0].tries_done = 5u;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, counters, 1u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "no viable when only candidate is exhausted");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_TRIES_EXHAUSTED,
                   "reject reason = TRIES_EXHAUSTED");
}

static void test_ladder_recovery_picks_recovery_kind(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "main");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "recover");
    set_id(r.entries[1].sort_key, "90");
    r.entries[1].kind = BOOT_ENTRY_KIND_RECOVERY;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.recovery_requested = 1;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_RECOVERY_REQUEST,
                   "recovery request wins");
    TEST_ASSERT(d.selected_entry_id[0] == 'r', "recovery entry picked");
}

static void test_ladder_watchdog_skips_peak_done(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "newer");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "older");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_counter_t counters[2];
    set_id(counters[0].id, "newer");
    counters[0].tries_left = 1u;
    counters[0].tries_done = 5u;        /* peak attempts */
    set_id(counters[1].id, "older");
    counters[1].tries_left = 2u;
    counters[1].tries_done = 1u;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.watchdog_rollback = 1;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, counters, 2u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_WATCHDOG_ROLLBACK,
                   "watchdog rollback wins");
    TEST_ASSERT(d.selected_entry_id[0] == 'o',
                "watchdog rolls back to older entry");
}

static void test_ladder_invalid_store_falls_back(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_REJECT_CRC_MISMATCH;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_STORE_INVALID,
                   "invalid store -> fallback");
}

static void test_ladder_hotkey_overrides_store_default(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "default-pick");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "operator-pick");
    set_id(r.entries[1].sort_key, "99");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.hotkey_override_index = 2u;       /* operator picked menu index 2 */
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_HOTKEY,
                   "hotkey overrides store default");
    TEST_ASSERT(d.selected_entry_id[0] == 'o',
                "operator-pick entry picked");
}

static void test_ladder_bootnext_hint_promotes(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "first-by-sort");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "boot-current-target");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.bootcurrent_known = 1;
    set_id(in.bootcurrent_entry_id, "boot-current-target");
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_BOOTNEXT_HINT,
                   "BootCurrent hint promotes target above sort_key default");
    TEST_ASSERT(d.selected_entry_id[0] == 'b',
                "boot-current-target picked");
}

static void test_ladder_path_escape_under_secure_boot(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "untrusted-chain");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_CHAINLOAD;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;     /* no TRUSTED_CHAINLOAD */

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.secure_boot_active = 1;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "untrusted chainload filtered under SB");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_PATH_ESCAPE,
                   "reject reason = PATH_ESCAPE");
}

static void test_ladder_kind_skipped_recorded(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "vendor");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = 150u;            /* vendor range, parser sets kind_skipped */
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    r.entries[0].kind_skipped = 1;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "stable");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT, "stable kind wins");
    TEST_ASSERT(d.selected_entry_id[0] == 's', "stable id picked");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_KIND_SKIPPED,
                   "kind_skipped reject recorded");
}

static void test_ladder_hidden_filtered(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "ghost");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE | BOOT_ENTRY_FLAG_HIDDEN;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "visible");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT, "visible wins");
    TEST_ASSERT(d.selected_entry_id[0] == 'v', "visible id picked");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_HIDDEN,
                   "hidden reject recorded");
}

static void test_ladder_empty_local_machine_id(void)
{
    /* When the machine has no SMBIOS UUID and the entry asserts a UUID,
     * the entry must be filtered (avoid mis-routing across machines). */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "elsewhere");
    set_id(r.entries[0].sort_key, "10");
    set_id(r.entries[0].machine_id, "11111111-1111-1111-1111-111111111111");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    /* in.local_machine_id is "" */
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "no candidate when local UUID empty + entry asserts UUID");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_MACHINE_ID_MISMATCH,
                   "machine_id mismatch on empty-local-id");
}

static void test_ladder_ab_slot_picks_index(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 3u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "slot-a");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "slot-b");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[2]);
    set_id(r.entries[2].id, "slot-c");
    set_id(r.entries[2].sort_key, "30");
    r.entries[2].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[2].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.ab_slot_index = 1u;               /* 0-based: pick slot-b */
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_AB_TRY_STATE,
                   "A/B priority 3 fires");
    TEST_ASSERT(d.selected_entry_id[5] == 'b', "slot-b picked at index 1");
}

static void test_ladder_priority_watchdog_beats_recovery(void)
{
    /* Multi-signal priority test: watchdog (priority 2) wins over recovery
     * (priority 4) when both inputs are asserted. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "main");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "rescue");
    set_id(r.entries[1].sort_key, "90");
    r.entries[1].kind = BOOT_ENTRY_KIND_RECOVERY;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_counter_t counters[1];
    set_id(counters[0].id, "main");
    counters[0].tries_left = 1u;
    counters[0].tries_done = 4u;         /* peak attempts */

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.watchdog_rollback = 1;
    in.recovery_requested = 1;           /* both signals asserted */
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, counters, 1u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_WATCHDOG_ROLLBACK,
                   "watchdog priority 2 beats recovery priority 4");
    TEST_ASSERT(d.selected_entry_id[0] == 'r',
                "watchdog rolls back to next-by-sort_key (rescue)");
}

static void test_ladder_watchdog_only_peak_is_no_viable(void)
{
    /* Regression for the H finding: watchdog asserted, the only viable
     * candidate IS the peak-attempted entry -> ladder must NOT fall
     * through and re-pick the peak via store-default. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "only-and-failing");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_counter_t counters[1];
    set_id(counters[0].id, "only-and-failing");
    counters[0].tries_left = 1u;
    counters[0].tries_done = 4u;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.watchdog_rollback = 1;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, counters, 1u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "no alternate -> fallback, NOT re-pick peak");
    TEST_ASSERT(d.selected_entry_id[0] == '\0',
                "no entry selected when peak suppressed and no alternate");
}

static void test_ladder_null_counters_with_count_safe(void)
{
    /* Regression for the M1 finding: NULL counter pointer with nonzero
     * count must be treated as "no counters tracked", not crash. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "only");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    /* Pass NULL with a stale-looking count of 5 -- must NOT crash. */
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 5u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "NULL counters with count > 0 treated as zero");
}

static void test_ladder_path_escape_demote_keeps_other_viable(void)
{
    /* Regression for the M2 finding: under Secure Boot, an untrusted
     * chainload entry must be demoted PER-ENTRY (rejected_entries)
     * while a viable trusted entry still gets to boot. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "untrusted-chain");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_CHAINLOAD;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;     /* no TRUSTED_CHAINLOAD */
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "trusted-os");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.secure_boot_active = 1;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "trusted entry boots while untrusted chainload demoted");
    TEST_ASSERT(d.selected_entry_id[0] == 't',
                "trusted-os picked, not untrusted-chain");
    TEST_ASSERT_EQ(d.rejected_count, 1u, "exactly one reject recorded");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_PATH_ESCAPE,
                   "untrusted chainload reject reason recorded");
}

static void test_ladder_empty_store_is_no_viable(void)
{
    /* Parser succeeded, but the resulting envelope set is empty. The
     * ladder must fall back, not pick a stale slot. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 0u;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "OK+0 entries -> fallback no viable");
    TEST_ASSERT_EQ(d.rejected_count, 0u, "no per-entry rejects on empty store");
}

/* ---- Registration ----------------------------------------------------- */

void test_register_boot_policy(void)
{
    test_suite_register_cat("boot-policy: counter filename roundtrip",
                            test_counter_filename_roundtrip, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: counter filename two-digit done",
                            test_counter_filename_two_digit_done, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: counter filename bad grammar rejected",
                            test_counter_filename_rejects_bad_grammar, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: counter filename boundary caps + NULLs",
                            test_counter_filename_boundary_caps, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: kind_skipped envelope filtered",
                            test_ladder_kind_skipped_recorded, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: hidden flag filtered",
                            test_ladder_hidden_filtered, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: empty local machine_id rejects entry UUID",
                            test_ladder_empty_local_machine_id, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: A/B slot index picks Nth candidate",
                            test_ladder_ab_slot_picks_index, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: watchdog beats recovery (priority 2 > 4)",
                            test_ladder_priority_watchdog_beats_recovery, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: empty store -> fallback no viable",
                            test_ladder_empty_store_is_no_viable, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: watchdog with only-peak -> no viable (regression)",
                            test_ladder_watchdog_only_peak_is_no_viable, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: NULL counters with count safe (regression)",
                            test_ladder_null_counters_with_count_safe, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: path-escape demote keeps other viable (regression)",
                            test_ladder_path_escape_demote_keeps_other_viable,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: store-default picks lowest sort_key",
                            test_ladder_store_default_picks_lowest_sort_key,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: inactive filtered to rejected list",
                            test_ladder_inactive_filtered_into_rejected,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: machine_id mismatch filtered",
                            test_ladder_machine_id_filter, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: tries-exhausted falls back",
                            test_ladder_tries_exhausted, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: recovery request picks recovery kind",
                            test_ladder_recovery_picks_recovery_kind,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: watchdog rolls back from peak done",
                            test_ladder_watchdog_skips_peak_done, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: invalid store -> fallback",
                            test_ladder_invalid_store_falls_back, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: hotkey overrides store default",
                            test_ladder_hotkey_overrides_store_default,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: BootCurrent hint promotes target",
                            test_ladder_bootnext_hint_promotes, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: path-escape under Secure Boot",
                            test_ladder_path_escape_under_secure_boot,
                            TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
