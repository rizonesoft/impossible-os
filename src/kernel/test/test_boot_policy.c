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
    in.ab_slot_valid = 1u;
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
    /* Public contract: selected envelope is valid for FALLBACK_NO_VIABLE
     * (boot_policy_decide synthesizes a fallback envelope so the caller
     * can boot). Verify the synthesized id is NOT the failing peak's id. */
    TEST_ASSERT(d.selected_entry_id[0] != '\0',
                "fallback envelope synthesized on FALLBACK_NO_VIABLE");
    TEST_ASSERT(!(d.selected_entry_id[0] == 'o' && d.selected_entry_id[1] == 'n'
                  && d.selected_entry_id[2] == 'l' && d.selected_entry_id[3] == 'y'),
                "fallback id is NOT the failing peak's id");
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

static void test_ladder_counters_overflow_fails_closed(void)
{
    /* Regression for re-adversarial round 3 [M counter]: when the
     * caller signals the counter directory scan saw more files than
     * the cap, the ladder MUST fall back rather than risk booting an
     * exhausted entry whose counter was dropped. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "ok");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.counters_overflow = 1;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "counter overflow -> fail-closed fallback");
    TEST_ASSERT(d.selected_entry_id[0] != '\0',
                "fallback envelope synthesized on overflow");
}

static void test_ladder_uki_mode_propagates_to_fallback(void)
{
    /* Regression for re-adversarial round 3 [H]: when invoked_via_uki
     * is set, FALLBACK_NO_VIABLE must synthesize a kind=uki envelope,
     * not a split-path one. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 0u;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.invoked_via_uki = 1;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "no entries -> fallback");
    TEST_ASSERT_EQ(d.selected.kind, BOOT_ENTRY_KIND_UKI,
                   "UKI mode produces kind=uki fallback envelope");
}

static void test_ladder_zeroed_inputs_no_ab_default(void)
{
    /* Regression for the H1 BSS-zero footgun: a freshly zeroed inputs
     * struct has ab_slot_index==0 but ab_slot_valid==0, so the A/B
     * priority MUST NOT fire and pick slot 0. Default behavior is
     * store-default. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "alpha");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "beta");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    /* Raw zeroed inputs -- mimics gBS->AllocateZeroPool() result. */
    boot_policy_inputs_t in;
    for (unsigned int i = 0; i < sizeof(in); i++) ((unsigned char *)&in)[i] = 0;
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "zero inputs -> store-default, NOT A/B slot 0");
    TEST_ASSERT(d.selected_entry_id[0] == 'a',
                "lowest sort_key wins (alpha), not slot-0 via A/B");
}

static void test_parser_empty_machine_id_wildcard(void)
{
    /* Regression for M3: empty machine_id is the documented wildcard;
     * parser must accept it without is_uuid_text rejection. The policy
     * filter handles "empty matches any machine" semantics; this test
     * proves the parser does not block the on-disk fixture from existing.
     */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "global");
    set_id(r.entries[0].sort_key, "10");
    /* Empty machine_id -- the wildcard. */
    r.entries[0].machine_id[0] = 0;
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    /* No local UUID either -- should still match the wildcard entry. */
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "empty machine_id wildcard matches any machine");
    TEST_ASSERT(d.selected_entry_id[0] == 'g', "global entry picked");
}

static void test_ladder_supported_kinds_mask_rejects_kind_ge_32(void)
{
    /* Closed-mask invariant: any kind >= 32 cannot be represented in
     * the 32-bit mask, so a non-zero mask MUST reject it with
     * KIND_UNAVAILABLE rather than fall-through-pass. Defense-in-depth
     * for the day a parser change leaves a vendor or reserved-future
     * envelope un-flagged with kind_skipped.
     */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    /* High kind, NOT marked kind_skipped (forced through the mask gate). */
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "high-kind");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = 100u;            /* BOOT_ENTRY_KIND_VENDOR_FIRST */
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    /* kind_skipped intentionally 0 -- exercise mask gate, not parser flag. */
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "split");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.supported_kinds_mask = (1u << BOOT_ENTRY_KIND_SPLIT);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "split wins despite high-kind being first");
    TEST_ASSERT(d.selected_entry_id[0] == 's', "split selected");
    TEST_ASSERT_EQ(d.rejected_count, 1u, "high-kind rejected");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_KIND_UNAVAILABLE,
                   "kind=100 rejected as KIND_UNAVAILABLE");
}

static void test_kind_to_path_recovery(void)
{
    boot_policy_path_override_t o;
    int r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_RECOVERY, &o);
    TEST_ASSERT_EQ(r, 1, "recovery is path-changing");
    TEST_ASSERT_EQ(o.boot_path, BOOT_POLICY_PATH_RECOVERY,
                   "boot_path = RECOVERY (3)");
    TEST_ASSERT_EQ(o.boot_reason, BOOT_POLICY_REASON_RECOVERY_TRIGGER,
                   "boot_reason = RECOVERY_TRIGGER (9)");
    TEST_ASSERT_EQ(o.src_flag_add, BOOT_POLICY_SRC_FLAG_RECOVERY_TRIGGERED,
                   "src_flag = RECOVERY_TRIGGERED (1<<5)");
}

static void test_kind_to_path_diagnostics(void)
{
    boot_policy_path_override_t o;
    int r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_DIAGNOSTICS, &o);
    TEST_ASSERT_EQ(r, 1, "diagnostics is path-changing");
    TEST_ASSERT_EQ(o.boot_path, BOOT_POLICY_PATH_DIAGNOSTIC, "DIAGNOSTIC (7)");
    TEST_ASSERT_EQ(o.boot_reason, BOOT_POLICY_REASON_DIAGNOSTIC_REQUEST, "DIAGNOSTIC_REQUEST (11)");
    TEST_ASSERT_EQ(o.src_flag_add, 0u, "no source flag for DIAGNOSTIC_REQUEST");
}

static void test_kind_to_path_network(void)
{
    boot_policy_path_override_t o;
    int r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_NETWORK, &o);
    TEST_ASSERT_EQ(r, 1, "network is path-changing");
    TEST_ASSERT_EQ(o.boot_path, BOOT_POLICY_PATH_NETWORK, "NETWORK (4)");
    TEST_ASSERT_EQ(o.boot_reason, BOOT_POLICY_REASON_USER_SELECTED, "USER_SELECTED (2)");
    TEST_ASSERT_EQ(o.src_flag_add, 0u, "no required source flag for USER_SELECTED");
}

static void test_kind_to_path_resume(void)
{
    boot_policy_path_override_t o;
    int r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_RESUME, &o);
    TEST_ASSERT_EQ(r, 1, "resume is path-changing");
    TEST_ASSERT_EQ(o.boot_path, BOOT_POLICY_PATH_RESUME, "RESUME (5)");
    TEST_ASSERT_EQ(o.boot_reason, BOOT_POLICY_REASON_USER_SELECTED, "USER_SELECTED (2)");
    TEST_ASSERT_EQ(o.src_flag_add, 0u, "no required source flag for USER_SELECTED");
}

static void test_kind_to_path_split_is_no_op(void)
{
    /* Non-path-changing kinds: helper returns 0 and leaves output
     * struct zeroed. Caller leaves boot_path / boot_reason / src_flags
     * alone; media-role override (if any) stands. */
    boot_policy_path_override_t o;
    int r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_SPLIT, &o);
    TEST_ASSERT_EQ(r, 0, "split is NOT path-changing");
    TEST_ASSERT_EQ(o.boot_path, 0u, "boot_path zeroed on no-op");
    TEST_ASSERT_EQ(o.boot_reason, 0u, "boot_reason zeroed on no-op");
    TEST_ASSERT_EQ(o.src_flag_add, 0u, "src_flag_add zeroed on no-op");

    r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_UKI, &o);
    TEST_ASSERT_EQ(r, 0, "uki is NOT path-changing");

    r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_INSTALLER, &o);
    TEST_ASSERT_EQ(r, 0, "installer is NOT path-changing (media-role owns)");

    r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_SAFE, &o);
    TEST_ASSERT_EQ(r, 0, "safe is NOT path-changing");

    r = boot_policy_kind_to_path(BOOT_ENTRY_KIND_TEST, &o);
    TEST_ASSERT_EQ(r, 0, "test is NOT path-changing");

    /* Vendor / reserved-future / out-of-range: also no-op. */
    r = boot_policy_kind_to_path(150u, &o);
    TEST_ASSERT_EQ(r, 0, "vendor kind is NOT path-changing");
    r = boot_policy_kind_to_path(0xFFFFFFFFu, &o);
    TEST_ASSERT_EQ(r, 0, "garbage kind is NOT path-changing");
}

static void test_ladder_uki_mode_rejects_split_entry(void)
{
    /* Cross-mode selection guard: bootloader is mode-locked at
     * detect_uki_sections() time; the policy ladder cannot switch
     * modes. Under UKI mode, mask admits only KIND_UKI. A split
     * entry must reject with KIND_UNAVAILABLE rather than be
     * selected and produce a lying selected_entry_id.
     */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "split-only");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.invoked_via_uki = 1;
    in.supported_kinds_mask = (1u << BOOT_ENTRY_KIND_UKI);  /* mode-locked */
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    /* No viable candidate -> ladder synthesizes UKI fallback envelope. */
    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "no viable when only-entry is wrong-mode kind");
    TEST_ASSERT_EQ(d.selected.kind, (unsigned int)BOOT_ENTRY_KIND_UKI,
                   "UKI fallback envelope synthesized in UKI mode");
    TEST_ASSERT_EQ(d.rejected_count, 1u, "split entry rejected");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_KIND_UNAVAILABLE,
                   "kind=SPLIT under UKI mode = KIND_UNAVAILABLE");
}

static void test_ladder_split_mode_rejects_uki_entry(void)
{
    /* Symmetric guard: split mode (invoked_via_uki=0) rejects a
     * kind=UKI entry. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "uki-only");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_UKI;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.invoked_via_uki = 0;
    in.supported_kinds_mask = (1u << BOOT_ENTRY_KIND_SPLIT);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "no viable when only-entry is wrong-mode kind");
    TEST_ASSERT_EQ(d.selected.kind, (unsigned int)BOOT_ENTRY_KIND_SPLIT,
                   "SPLIT fallback envelope synthesized in split mode");
    TEST_ASSERT_EQ(d.rejected_count, 1u, "uki entry rejected");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_KIND_UNAVAILABLE,
                   "kind=UKI under split mode = KIND_UNAVAILABLE");
}

static void test_counter_dedup_insert_raw_duplicates(void)
{
    /* counter-dedup worst-case demotion invariant: raw inputs foo+1-3 and foo+0-4
     * must merge to a single entry with tries_left=0, tries_done=4
     * (lowest left wins; highest done wins). Calls
     * boot_policy_counter_dedup_insert() directly so the merge logic
     * is exercised before the ladder filter sees the deduped output.
     */
    boot_counter_t out[8];
    unsigned int count = 0;

    boot_counter_t a;
    set_id(a.id, "foo");
    a.tries_left = 1u;
    a.tries_done = 3u;
    int r1 = boot_policy_counter_dedup_insert(out, &count, 8u, &a);
    TEST_ASSERT_EQ(r1, 1, "first insert appends");
    TEST_ASSERT_EQ(count, 1u, "count == 1 after first append");

    boot_counter_t b;
    set_id(b.id, "foo");
    b.tries_left = 0u;
    b.tries_done = 4u;
    int r2 = boot_policy_counter_dedup_insert(out, &count, 8u, &b);
    TEST_ASSERT_EQ(r2, 0, "duplicate id merges; does not append");
    TEST_ASSERT_EQ(count, 1u, "count stays 1 after merge");
    TEST_ASSERT_EQ(out[0].tries_left, 0u, "min(1, 0) wins -> tries_left=0");
    TEST_ASSERT_EQ(out[0].tries_done, 4u, "max(3, 4) wins -> tries_done=4");

    /* Reverse order: insert b first then a. Should produce the same
     * worst-case merged state (commutative). */
    boot_counter_t out2[8];
    unsigned int count2 = 0;
    boot_policy_counter_dedup_insert(out2, &count2, 8u, &b);
    int r3 = boot_policy_counter_dedup_insert(out2, &count2, 8u, &a);
    TEST_ASSERT_EQ(r3, 0, "reverse-order duplicate also merges");
    TEST_ASSERT_EQ(count2, 1u, "reverse: count stays 1");
    TEST_ASSERT_EQ(out2[0].tries_left, 0u, "reverse: tries_left=0");
    TEST_ASSERT_EQ(out2[0].tries_done, 4u, "reverse: tries_done=4");

    /* Different ids: both append, no merge. */
    boot_counter_t c;
    set_id(c.id, "bar");
    c.tries_left = 5u;
    c.tries_done = 1u;
    int r4 = boot_policy_counter_dedup_insert(out, &count, 8u, &c);
    TEST_ASSERT_EQ(r4, 1, "different id appends");
    TEST_ASSERT_EQ(count, 2u, "count == 2");
}

static void test_counter_dedup_insert_cap_full(void)
{
    /* Cap behavior: once count == cap, append returns 0 and out[]
     * is not modified. Existing entries can still be merged
     * (dedup is purely id-driven, independent of cap). */
    boot_counter_t out[2];
    unsigned int count = 0;
    boot_counter_t a;
    set_id(a.id, "a");
    a.tries_left = 3u;
    a.tries_done = 0u;
    boot_counter_t b;
    set_id(b.id, "b");
    b.tries_left = 3u;
    b.tries_done = 0u;
    boot_counter_t c;
    set_id(c.id, "c");
    c.tries_left = 3u;
    c.tries_done = 0u;

    TEST_ASSERT_EQ(boot_policy_counter_dedup_insert(out, &count, 2u, &a), 1, "1st fits");
    TEST_ASSERT_EQ(boot_policy_counter_dedup_insert(out, &count, 2u, &b), 1, "2nd fits");
    TEST_ASSERT_EQ(count, 2u, "count == cap");
    TEST_ASSERT_EQ(boot_policy_counter_dedup_insert(out, &count, 2u, &c), -1, "3rd new id rejected at cap (return -1)");
    TEST_ASSERT_EQ(count, 2u, "count unchanged on cap-reject");

    /* Even at cap, a duplicate of an existing id can still merge.
     * Return value 0 = merged (distinct from -1 = cap-full reject). */
    boot_counter_t a2;
    set_id(a2.id, "a");
    a2.tries_left = 0u;
    a2.tries_done = 5u;
    TEST_ASSERT_EQ(boot_policy_counter_dedup_insert(out, &count, 2u, &a2), 0, "cap full, duplicate merges (return 0)");
    TEST_ASSERT_EQ(out[0].tries_left, 0u, "cap-full merge: tries_left=0 (worst case)");
    TEST_ASSERT_EQ(out[0].tries_done, 5u, "cap-full merge: tries_done=5 (worst case)");
}

static void test_counter_torn_duplicate_resolution(void)
{
    /* End-to-end: feed raw torn-duplicate inputs through dedup, then
     * pass the deduped array to boot_policy_decide. The selected
     * entry must reach FALLBACK_NO_VIABLE because foo's worst-case
     * tries_left is 0 (TRIES_EXHAUSTED filter). */
    boot_counter_t arr[8];
    unsigned int cnt = 0;

    boot_counter_t torn_a;
    set_id(torn_a.id, "foo");
    torn_a.tries_left = 1u;
    torn_a.tries_done = 3u;
    boot_policy_counter_dedup_insert(arr, &cnt, 8u, &torn_a);

    boot_counter_t torn_b;
    set_id(torn_b.id, "foo");
    torn_b.tries_left = 0u;
    torn_b.tries_done = 4u;
    boot_policy_counter_dedup_insert(arr, &cnt, 8u, &torn_b);

    TEST_ASSERT_EQ(cnt, 1u, "torn pair merged to one slot");
    TEST_ASSERT_EQ(arr[0].tries_left, 0u, "merged tries_left=0");
    TEST_ASSERT_EQ(arr[0].tries_done, 4u, "merged tries_done=4");

    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 1u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "foo");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, arr, cnt, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_FALLBACK_NO_VIABLE,
                   "torn-duplicate end-to-end: foo demoted to tries_left=0 -> filtered out");
    TEST_ASSERT_EQ(d.rejected_count, 1u, "exactly one reject recorded");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_TRIES_EXHAUSTED,
                   "reject reason = TRIES_EXHAUSTED");
}

static void test_ladder_supported_kinds_mask_filters_unavailable(void)
{
    /* Caller-side capability gate: bootloader cannot execute kind=network
     * until the per-entry-kind handlers feature ships. With the mask set
     * to {SPLIT, UKI} only, a kind=network entry must be rejected with
     * BOOT_REJECT_REASON_KIND_UNAVAILABLE and the ladder must pick the
     * surviving SPLIT candidate instead. mask=0 leaves the gate disabled
     * (back-compat with every other test in this file).
     */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "net-boot");
    set_id(r.entries[0].sort_key, "10");
    r.entries[0].kind = BOOT_ENTRY_KIND_NETWORK;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "split-boot");
    set_id(r.entries[1].sort_key, "20");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_inputs_t in;
    zero_inputs(&in);
    in.supported_kinds_mask = (1u << BOOT_ENTRY_KIND_SPLIT) |
                              (1u << BOOT_ENTRY_KIND_UKI);
    boot_policy_decision_t d;
    boot_policy_decide(&in, &r, ((const boot_counter_t *)0), 0u, &d);

    TEST_ASSERT_EQ(d.reason, BOOT_SELECTION_STORE_DEFAULT,
                   "supported kind wins despite higher-priority unsupported");
    TEST_ASSERT(d.selected_entry_id[0] == 's',
                "split-boot picked, not net-boot");
    TEST_ASSERT_EQ(d.rejected_count, 1u, "one rejection recorded");
    TEST_ASSERT_EQ(d.rejected[0].reason, BOOT_REJECT_REASON_KIND_UNAVAILABLE,
                   "kind=network rejected as KIND_UNAVAILABLE");
}

/* ---- menu pure-logic tests ------------------------------------------ */

static void test_menu_should_show_store_default_two_entries(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    d.reason = BOOT_SELECTION_STORE_DEFAULT;

    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 2u), 1,
                   "STORE_DEFAULT + 2 candidates -> show");
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 1u), 0,
                   "single candidate -> skip");
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 0u), 0,
                   "no candidates -> skip");
}

static void test_menu_should_show_forced_selection_skips(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 4u;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;

    /* Each forced-selection reason MUST skip the menu. */
    d.reason = BOOT_SELECTION_HOTKEY;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 4u), 0, "HOTKEY skips menu");
    d.reason = BOOT_SELECTION_WATCHDOG_ROLLBACK;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 4u), 0, "WATCHDOG_ROLLBACK skips menu");
    d.reason = BOOT_SELECTION_AB_TRY_STATE;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 4u), 0, "AB_TRY_STATE skips menu");
    d.reason = BOOT_SELECTION_RECOVERY_REQUEST;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 4u), 0, "RECOVERY_REQUEST skips menu");
    d.reason = BOOT_SELECTION_FALLBACK_NO_VIABLE;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 4u), 0, "FALLBACK_NO_VIABLE skips menu");
    d.reason = BOOT_SELECTION_FALLBACK_STORE_INVALID;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 4u), 0, "FALLBACK_STORE_INVALID skips menu");
    d.reason = BOOT_SELECTION_UNSET;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 4u), 0, "UNSET sentinel skips menu");
}

static void test_menu_should_show_soft_reasons_render(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 3u;
    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;

    d.reason = BOOT_SELECTION_STORE_DEFAULT;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 3u), 1, "STORE_DEFAULT renders");
    d.reason = BOOT_SELECTION_BOOTNEXT_HINT;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 3u), 1, "BOOTNEXT_HINT renders");
    d.reason = BOOT_SELECTION_UNKNOWN_BOOTCURRENT;
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, &r, 3u), 1, "UNKNOWN_BOOTCURRENT renders");
}

static void test_menu_collect_filters_hidden_and_skipped(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 4u;
    /* Visible. */
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "alpha");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    /* HIDDEN -- should be filtered. */
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "hidden");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE | BOOT_ENTRY_FLAG_HIDDEN;
    /* Visible. */
    zero_envelope(&r.entries[2]);
    set_id(r.entries[2].id, "beta");
    r.entries[2].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[2].flags = BOOT_ENTRY_FLAG_ACTIVE;
    /* kind_skipped -- should be filtered. */
    zero_envelope(&r.entries[3]);
    set_id(r.entries[3].id, "vendor");
    r.entries[3].kind = 100u;
    r.entries[3].kind_skipped = 1;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    set_id(d.selected_entry_id, "beta");

    unsigned int idx[8];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 8u, &def);
    TEST_ASSERT_EQ(n, 2u, "2 visible candidates after HIDDEN + kind_skipped filter");
    TEST_ASSERT_EQ(idx[0], 0u, "first visible is alpha (entry 0)");
    TEST_ASSERT_EQ(idx[1], 2u, "second visible is beta (entry 2)");
    TEST_ASSERT_EQ(def, 1u, "default highlight points at beta (selected_entry_id match)");
}

static void test_menu_collect_rejected_store_offers_nothing(void)
{
    /* A REJECTED store must offer zero menu candidates. boot_policy_decide()
     * short-circuits to FALLBACK_STORE_INVALID WITHOUT filtering, so
     * decision->rejected[] is empty and the hard-hide filters have nothing to
     * match on -- every entry the parser retained before it hit the rejection
     * would otherwise be listed and selectable through F11, bypassing the
     * active/machine_id filtering the ladder would have applied.
     *
     * The concrete case is a store carrying valid entries followed by a
     * repeated `entries` key: the parser rejects it with DUPLICATE_KEY, but the
     * entries parsed before the repeat are still sitting in the result. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_REJECT_DUPLICATE_KEY;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "alpha");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "beta");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    d.reason = BOOT_SELECTION_FALLBACK_STORE_INVALID;

    unsigned int idx[8];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 8u, &def);
    TEST_ASSERT_EQ(n, 0u, "a rejected store offers no menu candidates");
    TEST_ASSERT_EQ(def, (unsigned int)BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "and names no default");
}

static void test_menu_collect_accepted_store_still_collects(void)
{
    /* Control for the case above: the SAME two entries with reject_code OK must
     * still be collected. Without this, a collector that returned 0
     * unconditionally would pass the rejection test. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "alpha");
    r.entries[0].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "beta");
    r.entries[1].kind = BOOT_ENTRY_KIND_SPLIT;
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    set_id(d.selected_entry_id, "beta");

    unsigned int idx[8];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 8u, &def);
    TEST_ASSERT_EQ(n, 2u, "an accepted store still collects both entries");
}

static void test_menu_collect_demote_not_drop_on_tries_exhausted(void)
{
    /* Demote-not-drop contract: TRIES_EXHAUSTED is a demote signal,
     * NOT a hide signal. The exhausted entry stays visible in the
     * menu so the operator can see it and manually inspect/select.
     * Greyed-out + last_failure_reason label belongs to the A/B and
     * Recovery integration UX layer. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 3u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "good");
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "exhausted");
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[2]);
    set_id(r.entries[2].id, "okay");
    r.entries[2].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    d.rejected_count = 1u;
    set_id(d.rejected[0].id, "exhausted");
    d.rejected[0].reason = BOOT_REJECT_REASON_TRIES_EXHAUSTED;
    set_id(d.selected_entry_id, "good");

    unsigned int idx[8];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 8u, &def);
    TEST_ASSERT_EQ(n, 3u,
                   "TRIES_EXHAUSTED stays visible (demote-not-drop)");
    TEST_ASSERT_EQ(idx[0], 0u, "good at index 0");
    TEST_ASSERT_EQ(idx[1], 1u, "exhausted at index 1 (still visible)");
    TEST_ASSERT_EQ(idx[2], 2u, "okay at index 2");
    TEST_ASSERT_EQ(def, 0u, "default highlight on good");
}

static void test_menu_collect_filters_hard_hide_reasons(void)
{
    /* Hard-hide reasons (everything except TRIES_EXHAUSTED) are
     * filtered from the menu: the loader cannot or will not boot
     * the entry, so showing it as selectable would let the operator
     * pick something the loader cannot execute. Tests both
     * KIND_SKIPPED and KIND_UNAVAILABLE (the most common cases:
     * vendor/reserved-future entries and entries whose kind is not
     * in supported_kinds_mask). */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 4u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "good");
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "vendor-reserved");
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[2]);
    set_id(r.entries[2].id, "recovery-no-handler");
    r.entries[2].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[3]);
    set_id(r.entries[3].id, "okay");
    r.entries[3].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    d.rejected_count = 2u;
    set_id(d.rejected[0].id, "vendor-reserved");
    d.rejected[0].reason = BOOT_REJECT_REASON_KIND_SKIPPED;
    set_id(d.rejected[1].id, "recovery-no-handler");
    d.rejected[1].reason = BOOT_REJECT_REASON_KIND_UNAVAILABLE;
    set_id(d.selected_entry_id, "good");

    unsigned int idx[8];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 8u, &def);
    TEST_ASSERT_EQ(n, 2u,
                   "KIND_SKIPPED + KIND_UNAVAILABLE both filtered");
    TEST_ASSERT_EQ(idx[0], 0u, "good at index 0");
    TEST_ASSERT_EQ(idx[1], 3u,
                   "okay at index 1 (both unsupported kinds skipped)");
    TEST_ASSERT_EQ(def, 0u, "default highlight on good");
}

static void test_menu_helpers_null_and_zero_cap_guards(void)
{
    /* menu-helper defensive guards: public helpers must reject NULL inputs +
     * cap==0 without dereferencing or writing past out_idx[]. Pinning
     * these so a future refactor cannot regress the pre-EBS firmware
     * contract into a crash class. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "a");
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "b");
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    d.reason = BOOT_SELECTION_STORE_DEFAULT;

    /* should_show NULL guards. */
    TEST_ASSERT_EQ(boot_policy_menu_should_show(NULL, &r, 2u), 0,
                   "should_show NULL decision -> 0");
    TEST_ASSERT_EQ(boot_policy_menu_should_show(&d, NULL, 2u), 0,
                   "should_show NULL parse -> 0");

    /* collect NULL guards. */
    unsigned int idx[8];
    unsigned int def = 99u;
    TEST_ASSERT_EQ(boot_policy_menu_collect(NULL, &d, idx, 8u, &def), 0u,
                   "collect NULL parse -> 0 count");
    TEST_ASSERT_EQ(def, BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "collect NULL parse leaves NOT_FOUND");

    def = 99u;
    TEST_ASSERT_EQ(boot_policy_menu_collect(&r, NULL, idx, 8u, &def), 0u,
                   "collect NULL decision -> 0 count");
    TEST_ASSERT_EQ(def, BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "collect NULL decision leaves NOT_FOUND");

    def = 99u;
    TEST_ASSERT_EQ(boot_policy_menu_collect(&r, &d, NULL, 8u, &def), 0u,
                   "collect NULL out_idx -> 0 count");
    TEST_ASSERT_EQ(def, BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "collect NULL out_idx leaves NOT_FOUND");

    /* cap==0 guard. */
    def = 99u;
    TEST_ASSERT_EQ(boot_policy_menu_collect(&r, &d, idx, 0u, &def), 0u,
                   "collect cap==0 -> 0 count");
    TEST_ASSERT_EQ(def, BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "collect cap==0 leaves NOT_FOUND");

    /* NULL out_default_idx is also valid (caller may not need it). */
    TEST_ASSERT_EQ(boot_policy_menu_collect(&r, &d, idx, 8u, NULL), 2u,
                   "collect NULL out_default_idx still produces visible count");
}

static void test_menu_collect_default_not_found_when_truncated(void)
{
    /* menu-helper regression: when the ladder pick is BEYOND the visible cap,
     * the helper MUST return BOOT_POLICY_MENU_DEFAULT_NOT_FOUND so the
     * caller suppresses the menu. Otherwise the visible default would
     * highlight a different entry than the one that actually boots on
     * timeout / Enter -- a boot-selection integrity failure. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 5u;
    for (unsigned i = 0; i < 5u; i++) {
        zero_envelope(&r.entries[i]);
        char id[8];
        id[0] = (char)('a' + i); id[1] = 0;
        set_id(r.entries[i].id, id);
        r.entries[i].flags = BOOT_ENTRY_FLAG_ACTIVE;
    }

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    /* Selected entry is at parse index 4 (id="e"); cap is 3 -> the
     * helper visits indices 0..2 and never sees "e". */
    set_id(d.selected_entry_id, "e");

    unsigned int idx[3];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 3u, &def);
    TEST_ASSERT_EQ(n, 3u, "cap=3 still caps visible entries at 3");
    TEST_ASSERT_EQ(def, BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "selected entry beyond cap -> NOT_FOUND sentinel");
}

static void test_menu_collect_default_not_found_when_filtered(void)
{
    /* Selected entry is filtered (HIDDEN flag) -> not in visible
     * candidates -> NOT_FOUND. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "visible");
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "ghost");
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE | BOOT_ENTRY_FLAG_HIDDEN;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    set_id(d.selected_entry_id, "ghost");  /* the hidden one */

    unsigned int idx[8];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 8u, &def);
    TEST_ASSERT_EQ(n, 1u, "1 visible candidate after HIDDEN filter");
    TEST_ASSERT_EQ(def, BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "selected hidden -> NOT_FOUND sentinel");
}

static void test_menu_collect_no_anchor_when_id_empty(void)
{
    /* Decision has empty selected_entry_id (STORE_INVALID sentinel).
     * Helper must NOT mark NOT_FOUND for that case -- it means "no
     * anchor", caller falls back to highlighting candidate 0. */
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 2u;
    zero_envelope(&r.entries[0]);
    set_id(r.entries[0].id, "first");
    r.entries[0].flags = BOOT_ENTRY_FLAG_ACTIVE;
    zero_envelope(&r.entries[1]);
    set_id(r.entries[1].id, "second");
    r.entries[1].flags = BOOT_ENTRY_FLAG_ACTIVE;

    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    /* selected_entry_id stays empty. */

    unsigned int idx[8];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 8u, &def);
    TEST_ASSERT_EQ(n, 2u, "2 visible candidates");
    TEST_ASSERT_EQ(def, BOOT_POLICY_MENU_DEFAULT_NOT_FOUND,
                   "empty decision id -> NOT_FOUND (caller treats as no-anchor)");
}

static void test_menu_collect_cap_enforced(void)
{
    boot_entries_parse_result_t r;
    for (unsigned i = 0; i < sizeof(r); i++) ((unsigned char *)&r)[i] = 0;
    r.reject_code = BOOT_ENTRIES_OK;
    r.entry_count = 5u;
    for (unsigned i = 0; i < 5u; i++) {
        zero_envelope(&r.entries[i]);
        char id[8];
        id[0] = (char)('a' + i); id[1] = 0;
        set_id(r.entries[i].id, id);
        r.entries[i].flags = BOOT_ENTRY_FLAG_ACTIVE;
    }
    boot_policy_decision_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;

    unsigned int idx[3];
    unsigned int def = 99u;
    unsigned int n = boot_policy_menu_collect(&r, &d, idx, 3u, &def);
    TEST_ASSERT_EQ(n, 3u, "cap=3 caps output at 3");
    TEST_ASSERT_EQ(idx[0], 0u, "first three retained in order");
    TEST_ASSERT_EQ(idx[1], 1u, "second");
    TEST_ASSERT_EQ(idx[2], 2u, "third");
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
    test_suite_register_cat("boot-policy: zeroed inputs do NOT auto-fire A/B slot 0 (regression)",
                            test_ladder_zeroed_inputs_no_ab_default, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: empty machine_id wildcard accepted (regression)",
                            test_parser_empty_machine_id_wildcard, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: counter_dedup_insert raw duplicates worst-case merge",
                            test_counter_dedup_insert_raw_duplicates, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: counter_dedup_insert cap-full + duplicate-merge-at-cap",
                            test_counter_dedup_insert_cap_full, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_should_show STORE_DEFAULT + 2 entries -> show",
                            test_menu_should_show_store_default_two_entries, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_should_show forced-selection reasons skip",
                            test_menu_should_show_forced_selection_skips, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_should_show soft reasons render",
                            test_menu_should_show_soft_reasons_render, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_collect filters HIDDEN + kind_skipped",
                            test_menu_collect_filters_hidden_and_skipped, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: rejected store offers no menu candidates",
                            test_menu_collect_rejected_store_offers_nothing, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: accepted store still collects candidates",
                            test_menu_collect_accepted_store_still_collects, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_collect demote-not-drop on TRIES_EXHAUSTED",
                            test_menu_collect_demote_not_drop_on_tries_exhausted, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_collect filters hard-hide reasons (KIND_SKIPPED + KIND_UNAVAILABLE)",
                            test_menu_collect_filters_hard_hide_reasons, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu helpers NULL + zero-cap guards",
                            test_menu_helpers_null_and_zero_cap_guards, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_collect default NOT_FOUND when truncated past cap",
                            test_menu_collect_default_not_found_when_truncated, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_collect default NOT_FOUND when selected is HIDDEN",
                            test_menu_collect_default_not_found_when_filtered, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_collect empty decision id -> NOT_FOUND no-anchor",
                            test_menu_collect_no_anchor_when_id_empty, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: menu_collect cap enforced",
                            test_menu_collect_cap_enforced, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: torn-duplicate end-to-end (dedup + ladder filter)",
                            test_counter_torn_duplicate_resolution, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: supported_kinds_mask filters unavailable kinds",
                            test_ladder_supported_kinds_mask_filters_unavailable,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: supported_kinds_mask closed for kind >= 32",
                            test_ladder_supported_kinds_mask_rejects_kind_ge_32,
                            TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: kind_to_path RECOVERY",
                            test_kind_to_path_recovery, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: kind_to_path DIAGNOSTICS",
                            test_kind_to_path_diagnostics, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: kind_to_path NETWORK",
                            test_kind_to_path_network, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: kind_to_path RESUME",
                            test_kind_to_path_resume, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: kind_to_path non-path-changing kinds are no-op",
                            test_kind_to_path_split_is_no_op, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: UKI mode rejects kind=SPLIT entry",
                            test_ladder_uki_mode_rejects_split_entry, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: split mode rejects kind=UKI entry",
                            test_ladder_split_mode_rejects_uki_entry, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: counters_overflow fails closed (regression)",
                            test_ladder_counters_overflow_fails_closed, TEST_CAT_BOOT);
    test_suite_register_cat("boot-policy: invoked_via_uki -> kind=uki fallback (regression)",
                            test_ladder_uki_mode_propagates_to_fallback, TEST_CAT_BOOT);
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
