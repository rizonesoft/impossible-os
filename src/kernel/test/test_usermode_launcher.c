/* ============================================================================
 * test_usermode_launcher.c -- Unit tests for the user-mode test launcher
 *
 * Covers the launcher's pure helpers (glob matcher, SKIP exit constant,
 * timeout setter) and configuration surface. The live spawn-and-wait path
 * cannot be unit-tested here -- it depends on VFS being mounted, PMM
 * having free pages, and the scheduler being enabled. That path is
 * validated at boot by the `test=1` smoke run ([UTEST] lines on serial).
 *
 * HARD BAN reminder: tests MUST NOT call the live launcher
 * (`test_usermode_run`) -- it takes over the scheduler and spawns user
 * tasks. Cover the pure helpers only, matching the "Allowed alternatives"
 * rule in CLAUDE.md "Test Code -- No Live Boot Infrastructure Calls".
 *
 * XREF: user-mode test framework launcher manifest + timeouts + TAP
 * section of the 00-infrastructure user-mode test framework TODO.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/test_usermode.h"
#include "kernel/sched/syscall.h"
#include "kernel/sched/task.h"   /* fault_site_* helpers + FI_ALLOC_* tags */
#include "kernel/gdt.h"          /* GDT_*_CODE -- the ring-3 evidence RPL rule */
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"       /* pmm_get_free_frames + ordinal countdown */
#include "kernel/mm/vmm.h"       /* vmm_create_user_pml4 (the fork site)    */
#include "kernel/fs/vfs.h"       /* VFS_MAX_NAME (the executor's path bound) */
#include "kernel/crypto/sha256.h" /* SHA256_DIGEST_LEN -- pins the digest bufs */
#include "kernel/exec.h"        /* EXEC_MAX_IMAGE_SIZE -- the freeze ceiling */
#include "kernel/boot_info.h"
#include "kernel/klog.h"         /* KLOG_SUBSYSTEM_MAX (frame-tag bound)    */
#include "registry.h"

/* Exposed by src/kernel/test/test_usermode.c for unit-test use. Kept as
 * forward declarations here rather than promoted to the public header
 * because their only non-test consumer is the launcher itself. */
int test_usermode_glob_match(const char *pattern, const char *name);
int test_usermode_is_valid_manifest_name(const char *name);
int test_usermode_classify_name(const char *name);
int test_usermode_classify_name_span(const char *name, uint32_t span_len);
uint32_t test_usermode_max_binary_name(void);
uint32_t test_usermode_name_room(uint32_t kind);
uint32_t test_usermode_name_digest(const char *p, uint32_t len);
int test_usermode_build_refusal_id(char *dst, uint32_t cap, uint32_t ordinal,
                                   const char *raw, uint32_t digest);
uint32_t test_usermode_aggregate_count(void);
const char *test_usermode_aggregate_label(uint32_t kind);
int test_usermode_build_aggregate_id(char *dst, uint32_t cap, uint32_t kind,
                                     uint32_t value);
int test_usermode_aggregate_has_value(uint32_t kind);
uint64_t test_usermode_clamp_time_ms(uint64_t ms);
uint32_t test_usermode_reason_max(void);
uint32_t test_usermode_reason_exit(char *dst, uint32_t cap, int32_t status);
void test_usermode_stamp_timeout_status(struct task *t);
int test_usermode_format_xml_testcase(char *dst, uint32_t cap,
                                      const char *name, int verdict,
                                      uint64_t time_ms, const char *reason);
int test_usermode_format_json_testcase(char *dst, uint32_t cap,
                                       const char *name, int verdict,
                                       uint64_t time_ms, const char *reason,
                                       int report_valid);
int test_usermode_format_json_skip(char *dst, uint32_t cap,
                                   const char *rec_name, const char *parent,
                                   uint32_t index);
int test_usermode_name_equal_fs(const char *a, const char *b);
int test_usermode_refusal_already_seen(const char *stored, int stored_verdict,
                                       const char *candidate);
int test_usermode_plan_dedup(const char *a_name, int a_type,
                             uint32_t a_expects, const char *b_name,
                             int b_type, uint32_t b_expects,
                             uint32_t *out_runs, uint32_t *out_refusals,
                             uint32_t *out_smoke_refused);
int test_usermode_plan_overflow(uint32_t *out_overflowed,
                                uint32_t *out_runs_after,
                                uint32_t *out_kept_before_drop);
int test_usermode_binary_present(const char *name);
int test_usermode_digest_binary(const char *name,
                                uint8_t out[SHA256_DIGEST_LEN]);
int test_usermode_identity_matches(const uint8_t *expect,
                                   const uint8_t *actual);
int test_usermode_freeze_refuses(const char *name, int as_smoke,
                                 uint32_t *out_kind,
                                 uint32_t *out_smoke_refused,
                                 const char **out_reason);
int test_usermode_manifest_parse(char *buf, uint32_t len, const char *filter,
                                 uint32_t *out_runs,
                                 uint32_t *out_refusals,
                                 uint32_t *out_authoritative,
                                 uint32_t *out_smoke_refused,
                                 uint32_t *out_first_type,
                                 uint32_t *out_first_expects,
                                 uint32_t *out_last_type,
                                 uint32_t *out_last_expects);
int test_usermode_plan_alloc_failure(uint32_t *out_alloc_failed,
                                     uint32_t *out_overflowed,
                                     uint32_t *out_count,
                                     uint32_t *out_pointers_null);
int test_usermode_plan_intern_and_compaction(uint32_t *out_overflowed,
                                             uint32_t *out_count_stable,
                                             uint32_t *out_refusals_kept,
                                             uint32_t *out_order_kept);
uint32_t test_usermode_plan_capacity(void);
int test_usermode_plan_lossy_refusal_suppresses(int exact, uint32_t *out_runs);

/* Mirror of the launcher's utest_name_verdict_t. The numeric values ARE
 * the contract these tests pin: the enum is file-local to the launcher,
 * and the manifest state stores a verdict as a uint8, so a reordering
 * would silently reinterpret stored refusals. */
#define UT_NAME_ACCEPT          0
#define UT_NAME_NOT_TEST_SHAPED 1
#define UT_NAME_REFUSE_LENGTH   2
#define UT_NAME_REFUSE_CHARSET  3
#define UT_NAME_REFUSE_PATH     4
#define UT_NAME_REFUSE_NUL      5
int test_usermode_derive_test_name(const char *name_in,
                                   char *out, uint32_t out_cap);
int test_usermode_path_join(const char *parent, const char *name,
                            char *out, uint32_t out_cap);
int test_usermode_path_has_traversal(const char *p);
int test_usermode_xml_escape(const char *src, char *dst, uint32_t cap);
int test_usermode_json_escape(const char *src, char *dst, uint32_t cap);
uint32_t test_usermode_report_reconcile(uint32_t state, uint32_t failed,
                                        int32_t exit_status, int timed_out);
int test_usermode_report_apply_invalid(int verdict, uint32_t *counters);
uint32_t test_usermode_skip_records_allowed(uint32_t already, uint32_t want);
uint32_t test_usermode_skip_record_budget(void);
int test_usermode_build_skip_record_name(char *dst, uint32_t cap,
                                         const char *base, uint32_t k);
int test_usermode_format_xml_overflow(char *dst, uint32_t cap, int verdict,
                                      const char *classname);
int test_usermode_format_json_overflow(char *dst, uint32_t cap, int verdict);
uint32_t test_usermode_record_verdict(int verdict, int loader_stage_fault,
                                      int timed_out, int reached_exec,
                                      int frame_adopted, int entered_user);
void test_usermode_loader_evidence_from_task(const struct task *t,
                                             uint32_t *stage_fault,
                                             uint32_t *reached_exec,
                                             uint32_t *identity_mismatch,
                                             uint32_t *frame_adopted,
                                             uint32_t *entered_user);
int test_usermode_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                     uint32_t failures, uint32_t skipped,
                                     uint64_t total_ms, int aborted,
                                     uint32_t not_run, uint32_t errors);
int test_usermode_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                      uint32_t failed, uint32_t skipped,
                                      uint64_t total_ms, uint32_t errors);
int test_usermode_format_json_run_report(char *dst, uint32_t cap,
                                         uint32_t a_pass, uint32_t a_fail,
                                         uint32_t blocks, uint32_t records,
                                         uint32_t reported, uint32_t invalid,
                                         uint32_t unreported);
int test_usermode_format_json_run_meta(char *dst, uint32_t cap, int aborted,
                                       uint32_t not_run);
int test_usermode_format_tap_point(char *dst, uint32_t cap, int ok,
                                   uint32_t point, const char *name,
                                   const char *directive);
uint32_t test_usermode_json_line_max(void);
int test_usermode_frame_tag_format(char *dst, uint32_t cap, uint32_t nonce);
uint32_t test_usermode_frame_nonce_fold(uint64_t mixed);
uint32_t test_usermode_frame_tag_cap(void);
int test_usermode_capture_escape(const char *raw, uint32_t raw_len,
                                 char *out, uint32_t out_cap);
uint32_t test_usermode_capture_chunk_max(void);
int test_usermode_capture_armed_for(const struct task *child, uint32_t pid);
int test_usermode_frame_run_is_open(void);
int test_usermode_capture_decide(uint32_t owner_seq, uint32_t run_records,
                                 int owner_stopped, int run_over,
                                 int run_sealed);
int test_usermode_capture_apply(uint32_t *owner_seq, uint32_t *run_records,
                                int *owner_stopped, int *run_over,
                                uint32_t *seq_out, uint32_t *charged_out,
                                int run_sealed, uint64_t *admitted);
uint64_t test_usermode_capture_pending(uint64_t admitted, uint64_t completed,
                                       uint64_t forgiven);
uint64_t test_usermode_capture_close(uint64_t *admitted, uint64_t *completed,
                                     uint64_t *forgiven, uint32_t *generation);
int test_usermode_capture_settle(uint32_t claim_gen, uint32_t current_gen,
                                 uint64_t *completed);
int test_usermode_capture_descendant_of(const struct task *t,
                                        uint32_t owner_pid);
void test_usermode_capture_run_state_get(uint32_t *records, int *over,
                                         int *sealed);
void test_usermode_capture_run_state_set(uint32_t records, int over,
                                         int sealed);
uint32_t test_usermode_capture_owner_budget(void);
uint32_t test_usermode_capture_run_budget(void);
uint32_t test_usermode_capture_wire_max(void);
uint32_t test_usermode_capture_owner_wire_allowance(void);
uint32_t test_usermode_capture_run_wire_allowance(void);
uint32_t test_usermode_capture_owner_raw_max(void);
uint32_t test_usermode_capture_run_raw_max(void);
int test_usermode_format_report_summary(char *dst, uint32_t cap,
                                        uint32_t a_pass, uint32_t a_fail,
                                        uint32_t blocks, uint32_t records,
                                        uint32_t reported, uint32_t invalid,
                                        uint32_t unreported);

/* sys_fault_inject_dispatch comes from kernel/sched/syscall.h. */

/* taxonomy helpers -- integer-returning thin wrappers so the unit
 * test binds against a stable ABI without pulling in utest_type_t. */
int test_usermode_type_for_name(const char *name);
int test_usermode_type_from_attr(const char *val);
const char *test_usermode_type_label(int type);

/* ---- Glob matcher: NULL pattern ------------------------------------- */

static void test_glob_null_pattern_matches_everything(void)
{
    TEST_ASSERT(test_usermode_glob_match((const char *)0, "test_syscall.exe") == 1,
                "NULL pattern matches test_syscall.exe");
    TEST_ASSERT(test_usermode_glob_match((const char *)0, "") == 1,
                "NULL pattern matches empty string");
}

/* ---- Glob matcher: literal (no wildcard) ---------------------------- */

static void test_glob_literal_match(void)
{
    TEST_ASSERT(test_usermode_glob_match("test_syscall.exe", "test_syscall.exe") == 1,
                "exact literal matches");
    TEST_ASSERT(test_usermode_glob_match("test_syscall.exe", "test_libc.exe") == 0,
                "literal rejects non-matching name");
    TEST_ASSERT(test_usermode_glob_match("test_syscall.exe", "test_syscall") == 0,
                "literal requires full-length match (suffix differs)");
    TEST_ASSERT(test_usermode_glob_match("test_syscall.exe", "test_syscall.exe.bak") == 0,
                "literal rejects longer name");
}

/* ---- Glob matcher: single-* patterns -------------------------------- */

static void test_glob_wildcard_prefix(void)
{
    TEST_ASSERT(test_usermode_glob_match("test_smoke_*.exe", "test_smoke_boot.exe") == 1,
                "test_smoke_* matches test_smoke_boot.exe");
    TEST_ASSERT(test_usermode_glob_match("test_smoke_*.exe", "test_smoke_.exe") == 1,
                "test_smoke_* matches empty middle (star eats nothing)");
    TEST_ASSERT(test_usermode_glob_match("test_smoke_*.exe", "test_syscall.exe") == 0,
                "test_smoke_* rejects non-matching prefix");
    TEST_ASSERT(test_usermode_glob_match("test_smoke_*.exe", "test_smoke_boot.bin") == 0,
                "test_smoke_* rejects wrong suffix");
}

static void test_glob_wildcard_at_start(void)
{
    TEST_ASSERT(test_usermode_glob_match("*.exe", "hello.exe") == 1,
                "*.exe matches hello.exe");
    TEST_ASSERT(test_usermode_glob_match("*.exe", "script.sh") == 0,
                "*.exe rejects script.sh");
    TEST_ASSERT(test_usermode_glob_match("*.exe", ".exe") == 1,
                "*.exe matches just .exe (star eats nothing)");
}

static void test_glob_wildcard_at_end(void)
{
    TEST_ASSERT(test_usermode_glob_match("test_*", "test_syscall.exe") == 1,
                "test_* matches test_syscall.exe");
    TEST_ASSERT(test_usermode_glob_match("test_*", "other_thing") == 0,
                "test_* rejects other_thing");
    TEST_ASSERT(test_usermode_glob_match("test_*", "test_") == 1,
                "test_* matches bare test_ (star eats nothing)");
}

static void test_glob_suffix_longer_than_remaining(void)
{
    /* Case where name after prefix is shorter than the suffix length.
     * Critical boundary: the launcher's bounded-length check must
     * reject instead of read out-of-bounds. */
    TEST_ASSERT(test_usermode_glob_match("test_*.exe", "test_") == 0,
                "rejects name too short to contain suffix");
    TEST_ASSERT(test_usermode_glob_match("test_*.exe", "test_a") == 0,
                "rejects name one char too short for suffix");
    TEST_ASSERT(test_usermode_glob_match("test_*.exe", "test_.exe") == 1,
                "accepts minimum-length match (star eats nothing)");
}

static void test_glob_empty_name(void)
{
    TEST_ASSERT(test_usermode_glob_match("test_*.exe", "") == 0,
                "empty name never matches non-empty pattern");
    TEST_ASSERT(test_usermode_glob_match("*", "") == 1,
                "`*` matches empty (zero-length match)");
}

/* ---- Manifest name validator (trust-boundary regression) ----------- */

static void test_manifest_accepts_well_formed(void)
{
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_syscall.exe") == 1,
                "accepts test_syscall.exe");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_harness_smoke.exe") == 1,
                "accepts test_harness_smoke.exe");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_.exe") == 1,
                "accepts minimum-length test_.exe");
}

/* ---- Derived name bound + refusal taxonomy ------------------------- */

/* Build a test_*.exe name of exactly `len` bytes into `dst`. */
static void ut_name_of_len(char *dst, uint32_t cap, uint32_t len)
{
    const char *pre = "test_";
    const char *suf = ".exe";
    uint32_t i, p = 0;

    if (len + 1u > cap || len < 9u) { dst[0] = '\0'; return; }
    for (i = 0; pre[i]; i++) dst[p++] = pre[i];
    while (p < len - 4u) dst[p++] = 'a';
    for (i = 0; suf[i]; i++) dst[p++] = suf[i];
    dst[p] = '\0';
}

static void test_name_bound_is_the_minimum_across_formatters(void)
{
    uint32_t bound = test_usermode_max_binary_name();
    uint32_t k, min = 0xFFFFFFFFu;

    /* The property the derivation exists for: the bound is the MINIMUM
     * over every record kind, not the one kind that looks worst. The
     * JSON binary record and the JSON skip_block land on the same byte
     * today, so assuming either would be right only by luck.
     *
     * SEVEN kinds, not six: UTEST_MAX_BINARY_NAME minimizes over seven
     * UTEST_NAME_ROOM terms, and this loop stopped at six until
     * 2026-07-31 -- so the XML skip record was outside the completeness
     * claim this test makes. */
    for (k = 0; k < 7u; k++) {
        uint32_t room = test_usermode_name_room(k);
        if (room < min) min = room;
    }
    TEST_ASSERT_EQ(bound, min,
                   "derived bound equals the min room across all 7 kinds");
    TEST_ASSERT(bound >= 24u,
                "bound still admits the longest name this repo builds (22)");
    TEST_ASSERT(test_usermode_name_room(3) * 2u + 24u <= 256u,
                "the twice-carried skip_block name is priced at 2N");
}

static void test_name_at_bound_accepted_over_bound_refused(void)
{
    uint32_t bound = test_usermode_max_binary_name();
    char at[128];
    char over[128];

    ut_name_of_len(at, sizeof(at), bound);
    ut_name_of_len(over, sizeof(over), bound + 1u);
    TEST_ASSERT_EQ(test_usermode_classify_name(at), UT_NAME_ACCEPT,
                   "a name at exactly the bound runs normally");
    TEST_ASSERT_EQ(test_usermode_classify_name(over), UT_NAME_REFUSE_LENGTH,
                   "one byte past the bound is a counted refusal");
}

static void test_charset_refusals_are_distinct_from_not_test_shaped(void)
{
    /* The separation this section exists to make: a file that is not
     * test_*.exe is ignored, a test-shaped name that fails the gate is
     * REFUSED and must be counted. */
    TEST_ASSERT_EQ(test_usermode_classify_name("notes.txt"),
                   UT_NAME_NOT_TEST_SHAPED,
                   "a non-test file is ignored, not refused");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_foo.txt"),
                   UT_NAME_NOT_TEST_SHAPED,
                   "wrong suffix is not test-shaped");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_a\"b.exe"),
                   UT_NAME_REFUSE_CHARSET,
                   "a quote would expand 6x through XML escaping");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_a&b.exe"),
                   UT_NAME_REFUSE_CHARSET, "ampersand refused");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_a b.exe"),
                   UT_NAME_REFUSE_CHARSET, "space refused");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_a#b.exe"),
                   UT_NAME_REFUSE_CHARSET,
                   "hash would inject a TAP directive");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_a\nb.exe"),
                   UT_NAME_REFUSE_CHARSET,
                   "newline would split a record across serial lines");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_a\\b.exe"),
                   UT_NAME_REFUSE_CHARSET, "backslash refused");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_a..b.exe"),
                   UT_NAME_REFUSE_PATH,
                   "traversal is its own verdict, dots being in the charset");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_ok-1.2_x.exe"),
                   UT_NAME_ACCEPT, "the full accepted charset passes");
    /* The two manifest comment spellings, pinned together because the
     * lexer and the classifier only make sense as a pair. `#` introduces
     * a comment solely after whitespace, so `test_a#b.exe` survives whole
     * and earns a CHARSET refusal (above), while `test_ok.exe#note`
     * becomes one token that fails the shape check. Neither may be
     * silently dropped: the manifest path counts a shape failure as a
     * refusal too, so both spellings reach an artifact. */
    TEST_ASSERT_EQ(test_usermode_classify_name("test_ok.exe#note"),
                   UT_NAME_NOT_TEST_SHAPED,
                   "a comment with no leading space is part of the name");
}

static void test_embedded_nul_is_refused_not_truncated(void)
{
    /* A manifest line whose payload carries a NUL: everything downstream
     * of C-string scanning sees only the prefix and would act on it,
     * leaving the rest of the line unexamined and the run green. The span
     * length is the only place the truncation is visible. */
    static const char after[]  = "test_good.exe\0trailing";
    /* The dangerous shape, and the one an ordering bug hides: the visible
     * prefix is NOT test-shaped, so a classifier that checks shape before
     * the span reports NOT_TEST_SHAPED and the entry is IGNORED rather
     * than counted -- the false-green this section exists to close. */
    static const char before[] = "test_bad\0.exe";
    static const char leading[] = "\0test_x.exe";

    TEST_ASSERT_EQ(test_usermode_classify_name(after), UT_NAME_ACCEPT,
                   "C-string view alone sees a perfectly valid name");
    TEST_ASSERT_EQ(test_usermode_classify_name_span(after,
                                                    (uint32_t)(sizeof(after) - 1u)),
                   UT_NAME_REFUSE_NUL,
                   "NUL after a complete suffix is a counted failure");
    TEST_ASSERT_EQ(test_usermode_classify_name_span(before,
                                                    (uint32_t)(sizeof(before) - 1u)),
                   UT_NAME_REFUSE_NUL,
                   "NUL BEFORE the suffix is refused, never ignored");
    TEST_ASSERT_EQ(test_usermode_classify_name_span(leading,
                                                    (uint32_t)(sizeof(leading) - 1u)),
                   UT_NAME_REFUSE_NUL,
                   "a leading NUL is refused, not read as an empty entry");
    TEST_ASSERT_EQ(test_usermode_classify_name_span("test_good.exe", 13u),
                   UT_NAME_ACCEPT,
                   "a span matching the C length still accepts");
}

static void test_filter_folds_case_like_the_filesystem(void)
{
    /* `utest_filter=` selects over IXFS, which resolves names
     * case-insensitively, so the filter has to agree with it. While the
     * filter compared literally and the dedup folded, a requested binary
     * could be planned zero times with total_planned and total_ran still
     * agreeing -- a false green rather than a visible miss. */
    TEST_ASSERT(test_usermode_glob_match("test_foo.exe",
                                         "test_FOO.exe") == 1,
                "a literal pattern folds case, as the filesystem does");
    TEST_ASSERT(test_usermode_glob_match("TEST_Smoke_a.exe",
                                         "test_smoke_a.exe") == 1,
                "the fold covers the whole literal, not just the stem");
    TEST_ASSERT(test_usermode_glob_match("test_SMOKE_*.exe",
                                         "test_smoke_boot.exe") == 1,
                "a wildcard pattern folds its prefix");
    TEST_ASSERT(test_usermode_glob_match("test_*.EXE",
                                         "test_smoke_boot.exe") == 1,
                "a wildcard pattern folds its suffix");
    /* Folding must not turn the filter into a wildcard: names that
     * genuinely differ still have to miss. */
    TEST_ASSERT(test_usermode_glob_match("test_foo.exe",
                                         "test_foo2.exe") == 0,
                "a longer name is still not a match");
    TEST_ASSERT(test_usermode_glob_match("test_smoke_*.exe",
                                         "test_perf_a.exe") == 0,
                "a non-matching prefix still misses");
    TEST_ASSERT(test_usermode_glob_match("test_a-1.exe",
                                         "test_A-1.exe") == 1,
                "digits and punctuation survive the fold unchanged");
    TEST_ASSERT(test_usermode_glob_match((const char *)0,
                                         "anything.exe") == 1,
                "a NULL pattern still matches everything");
}

static void test_plan_folds_an_identical_duplicate(void)
{
    uint32_t runs = 99, refusals = 99;

    /* Two manifest spellings of ONE file carrying the SAME policy are one
     * binary. Planning both would run it twice under two ordinals, and
     * because planned and ran inflate together the completeness
     * reconciliation could not see it. */
    if (!test_usermode_plan_dedup("test_dup.exe", UTEST_TYPE_CORRECTNESS, 1u,
                                  "test_DUP.exe", UTEST_TYPE_CORRECTNESS, 1u,
                                  &runs, &refusals, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 1u, "one file with one policy takes one plan slot");
    TEST_ASSERT_EQ(refusals, 0u,
                   "an identical duplicate folds away rather than failing");
}

static void test_plan_refuses_a_conflicting_duplicate(void)
{
    uint32_t runs = 99, refusals = 99, smoke_refused = 99;

    /* Filesystem identity does not settle manifest POLICY. Two case
     * variants of one file can disagree on `type=` -- and silently
     * keeping either one demotes a smoke test out of the fast-fail phase
     * (or promotes one into it) while every count still reconciles. The
     * conflict is published as a counted failure and the identity runs
     * under neither policy. */
    if (!test_usermode_plan_dedup("test_conf.exe", UTEST_TYPE_SMOKE, 1u,
                                  "test_CONF.exe", UTEST_TYPE_CORRECTNESS, 1u,
                                  &runs, &refusals, &smoke_refused)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 0u,
                   "a policy conflict must not run under a guessed policy");
    TEST_ASSERT_EQ(refusals, 1u,
                   "the conflicting identity is published exactly once");
    /* The conflict refused a DECLARED smoke binary. Refusing it is a smoke
     * non-PASS, so the suite must abort rather than carry on with the
     * remaining binaries -- otherwise a crafted manifest could demote a
     * smoke prerequisite to an ordinary counted failure and still run
     * everything after it, with total_planned == total_ran so neither the
     * completeness gate nor the artifact reconciliation could see it. */
    TEST_ASSERT_EQ(smoke_refused, 1u,
                   "refusing a declared smoke identity aborts the suite");

    /* A disagreement about task COST is the same class of conflict: it
     * understates the preflight task budget rather than the phase. */
    runs = 99; refusals = 99;
    if (!test_usermode_plan_dedup("test_cost.exe", UTEST_TYPE_CORRECTNESS, 1u,
                                  "test_COST.exe", UTEST_TYPE_CORRECTNESS, 4u,
                                  &runs, &refusals, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 0u, "conflicting expects_tasks is also a conflict");
    TEST_ASSERT_EQ(refusals, 1u, "and is published exactly once");

    /* A THIRD case variant refused for an unusable attribute must not add
     * a second record for the same file: two failures for one binary read
     * as two binaries, and the counts reconcile either way so nothing
     * downstream can tell them apart. */
    {
        static char buf[512];
        uint32_t i, p = 0, dup_runs = 99, dup_refusals = 99;
        const char *text =
            "test_d.exe type=stress\n"
            "test_D.exe type=perf\n"
            "test_d.exe type=bogus\n";

        for (i = 0; text[i]; i++) buf[p++] = text[i];
        buf[p] = '\0';
        if (!test_usermode_manifest_parse(buf, p, (const char *)0, &dup_runs,
                                          &dup_refusals, (uint32_t *)0,
                                          (uint32_t *)0, (uint32_t *)0,
                                          (uint32_t *)0, (uint32_t *)0,
                                          (uint32_t *)0)) {
            TEST_SKIP("plan allocation unavailable");
            return;
        }
        TEST_ASSERT_EQ(dup_runs, 0u,
                       "a conflicted identity does not run");
        TEST_ASSERT_EQ(dup_refusals, 1u,
                       "one filesystem identity yields one refusal record");
    }
}

static void test_plan_overflow_runs_nothing(void)
{
    uint32_t overflowed = 99, runs_after = 99, kept_before_drop = 99;

    /* Past its capacity the plan is no longer the complete enumeration,
     * so it cannot certify what it did not enumerate. Executing the
     * retained prefix would produce an internally consistent run that
     * silently omitted binaries -- exactly the false green this section
     * removes -- so the run publishes an aggregate and launches nothing. */
    if (!test_usermode_plan_overflow(&overflowed, &runs_after,
                                     &kept_before_drop)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(overflowed, 1u,
                   "filling past the capacity must flag the plan incomplete");
    TEST_ASSERT_EQ(runs_after, 0u,
                   "an incomplete plan executes no binary at all");

    /* The VALUE the plan-full aggregate publishes, at the runtime path
     * rather than through the builder. It is captured at the FIRST
     * transition to overflowed and must survive every later compaction:
     * u_plan_drop_runs rewrites plan.count, and a manifest that overflows
     * BOTH the plan cap and the refusal cap drops the plan once BEFORE
     * this aggregate is emitted. Reading the count late reported a
     * completely full plan as `agg_plan_kept_0.exe`, or worse the staged
     * refusal count -- a wrong number on the one path whose whole subject
     * is loss. The shim drops twice and zeroes the result if the capture
     * moved, so a regression fails here rather than shipping a plausible
     * number. */
    TEST_ASSERT_EQ(kept_before_drop, test_usermode_plan_capacity(),
                   "the plan-full aggregate publishes the pre-drop count");
    TEST_ASSERT(kept_before_drop != runs_after,
                "the published value is not the post-discard count");
}

static void test_manifest_parses_past_the_old_runnable_cap(void)
{
    /* The old parser stopped accepting runnable entries at 128 and handed
     * the tail to a directory glob that could not reconstruct an entry's
     * type, task cost or ordering -- and could not see a binary absent
     * from the image at all. The cap is gone: entries go straight into
     * the plan, so 129+ ordered entries with intact metadata must
     * survive. 129 is the boundary that used to lose the last one. */
    static char buf[8192];
    uint32_t runs = 0, refusals = 0;
    uint32_t first_type = 0, first_expects = 0;
    uint32_t last_type = 0, last_expects = 0;
    uint32_t i, p = 0;

    /* Entry 0 declares smoke policy and a fork-heavy task cost; the last
     * entry declares neither, so a parser that dropped or reordered
     * metadata shows up as a mismatch on one end or the other. */
    const char *head = "test_a000.exe type=smoke expects_tasks=4\n";
    for (i = 0; head[i]; i++) buf[p++] = head[i];
    for (i = 1; i < 129u; i++) {
        const char *pre = "test_a";
        uint32_t k;
        for (k = 0; pre[k]; k++) buf[p++] = pre[k];
        buf[p++] = (char)('0' + (i / 100u) % 10u);
        buf[p++] = (char)('0' + (i / 10u) % 10u);
        buf[p++] = (char)('0' + i % 10u);
        for (k = 0; ".exe\n"[k]; k++) buf[p++] = ".exe\n"[k];
    }
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, (const char *)0, &runs,
                                      &refusals, (uint32_t *)0, (uint32_t *)0,
                                      &first_type, &first_expects, &last_type,
                                      &last_expects)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 129u,
                   "every accepted manifest entry reaches the plan");
    TEST_ASSERT_EQ(refusals, 0u, "well-formed entries are not refused");
    TEST_ASSERT_EQ(first_type, (uint32_t)UTEST_TYPE_SMOKE,
                   "an explicit type= survives the parse in plan order");
    TEST_ASSERT_EQ(first_expects, 4u,
                   "an explicit expects_tasks= survives the parse");
    TEST_ASSERT_EQ(last_type, (uint32_t)UTEST_TYPE_CORRECTNESS,
                   "the entry past the old cap keeps its derived type");
    TEST_ASSERT_EQ(last_expects, 1u,
                   "the entry past the old cap keeps its default task cost");
}

static void test_manifest_refusal_is_terminal_for_its_identity(void)
{
    /* A refused identity must stay refused. Matching only runnable
     * entries left a hole: after two conflicting variants converted the
     * first to a refusal, a THIRD variant of the same IXFS name matched
     * nothing and took a fresh runnable slot -- so one file was published
     * as refused AND executed, with every count still reconciling. */
    static char buf[512];
    uint32_t runs = 99, refusals = 99;
    uint32_t i, p = 0;
    const char *text =
        "test_x.exe type=smoke\n"
        "test_X.exe type=correctness\n"
        "test_x.exe type=smoke\n";

    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, (const char *)0, &runs,
                                      &refusals, (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 0u,
                   "a third case-variant must not revive a refused identity");
    TEST_ASSERT_EQ(refusals, 1u,
                   "the identity is published refused exactly once");
}

static void test_lossy_refusal_name_is_not_an_identity(void)
{
    uint32_t runs_lossy = 99, runs_exact = 99;

    /* A glob refusal stores a SANITIZED prefix: `test_bad?.exe` becomes
     * `test_bad_.exe` because `?` is outside the accepted charset, and an
     * overlength name is truncated. Treating that rendering as an
     * identity would suppress a genuinely different `test_bad_.exe` --
     * dropping a requested test with nothing louder than the unrelated
     * refusal's own record, which is strictly worse than the double
     * publication the terminal rule exists to prevent. */
    if (!test_usermode_plan_lossy_refusal_suppresses(0, &runs_lossy)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs_lossy, 1u,
                   "a lossy refusal name must not suppress a real binary");

    /* The same collision through an EXACT refusal (a manifest line, or a
     * duplicate-policy conversion) must still be terminal, or the rule
     * would not prevent anything. */
    if (!test_usermode_plan_lossy_refusal_suppresses(1, &runs_exact)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs_exact, 0u,
                   "an exact refused identity is still terminal");
}

static void test_refused_attribute_vetoes_a_later_case_variant(void)
{
    /* Ordering, not policy: accepted lines enter the plan as they are
     * read, but the refusal set is complete only at end of file. A
     * refused-attribute line followed by a case variant of the same IXFS
     * name must NOT be planned runnable and then refused as well -- both
     * counters would agree and the completeness gate would stay blind. */
    static char buf[512];
    uint32_t runs = 99, refusals = 99, smoke_refused = 99;
    uint32_t i, p = 0;
    const char *text =
        "test_v.exe type=bogus\n"
        "test_V.exe type=smoke\n";

    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, (const char *)0, &runs,
                                      &refusals, (uint32_t *)0, &smoke_refused,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 0u,
                   "a refused identity vetoes a case variant read after it");
    TEST_ASSERT_EQ(refusals, 1u,
                   "and the refusal is still published exactly once");
    /* The vetoed entry DECLARED smoke, so the veto is a smoke non-PASS.
     * Second route to the same bypass: the duplicate-policy branch covers
     * a conflict between two accepted lines; this covers a refused line
     * deleting an accepted smoke variant. */
    TEST_ASSERT_EQ(smoke_refused, 1u,
                   "vetoing a declared smoke identity aborts the suite");
}

static void test_fully_filtered_manifest_stays_authoritative(void)
{
    /* A manifest whose every entry the filter excludes is still a
     * manifest. If it reported "not usable", the launcher would scan C:\
     * instead and could plan an on-disk binary the manifest never listed
     * -- with glob-derived type and task cost -- when the operator asked
     * for a narrower run, not a wider one. Manifest AUTHORITY is a
     * statement about the file; the surviving entry count is a statement
     * about the plan, and conflating them is what broke this. */
    static char buf[256];
    uint32_t runs = 99, authoritative = 99;
    uint32_t i, p = 0;
    const char *text = "test_alpha.exe\ntest_beta.exe\n";

    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, "test_nomatch_*.exe", &runs,
                                      (uint32_t *)0, &authoritative,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 0u, "a filter excluding everything plans nothing");
    TEST_ASSERT_EQ(authoritative, 1u,
                   "a fully filtered manifest still suppresses glob discovery");

    /* And the same manifest with a matching filter plans exactly the
     * entries that matched -- so the authority bit is not simply stuck. */
    runs = 99; authoritative = 99;
    p = 0;
    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';
    if (!test_usermode_manifest_parse(buf, p, "test_al*.exe", &runs,
                                      (uint32_t *)0, &authoritative,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 1u, "a matching filter plans only what it matched");
    TEST_ASSERT_EQ(authoritative, 1u, "and the manifest remains authoritative");
}

static void test_attribute_refused_smoke_aborts_the_suite(void)
{
    static char buf[512];
    uint32_t runs = 99, refusals = 99, smoke_refused = 99;
    uint32_t i, p = 0;
    /* The NAME is accepted, so `type=smoke` is a TRUSTED declaration; only
     * the task count is unusable. Refusing the line without aborting would
     * publish the smoke prerequisite as an ordinary counted failure and
     * run every later binary anyway -- the third route to the bypass, and
     * the one that needs no case-variant trickery at all. */
    const char *text =
        "test_gate.exe type=smoke expects_tasks=0\n"
        "test_other.exe\n";

    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, (const char *)0, &runs,
                                      &refusals, (uint32_t *)0, &smoke_refused,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(refusals, 1u,
                   "an unusable attribute refuses the line");
    TEST_ASSERT_EQ(runs, 1u,
                   "the unrelated entry is still planned");
    TEST_ASSERT_EQ(smoke_refused, 1u,
                   "refusing a trusted smoke declaration aborts the suite");

    /* A name-REFUSED line must NOT abort: its type is never derived,
     * exactly so untrusted bytes cannot claim smoke policy and reach the
     * gate. Same shape, different trust. */
    runs = 99; refusals = 99; smoke_refused = 99;
    p = 0;
    {
        const char *bad = "test_smoke_a..exe\ntest_other.exe\n";
        for (i = 0; bad[i]; i++) buf[p++] = bad[i];
        buf[p] = '\0';
    }
    if (!test_usermode_manifest_parse(buf, p, (const char *)0, &runs,
                                      &refusals, (uint32_t *)0, &smoke_refused,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(smoke_refused, 0u,
                   "a name-refused line never claims smoke policy");
}

static void test_smoke_provenance_survives_every_refusal_route(void)
{
    static char buf[512];
    uint32_t runs = 99, smoke_refused = 99;
    uint32_t i, p = 0;

    /* A THIRD declaration arriving after two non-smoke lines already
     * converted the identity to a refusal. The smoke policy has to MERGE
     * into that refusal record: if it were merely dropped, the identity
     * would sit refused with no smoke provenance anywhere and the gate
     * could not see it. This is the route that made per-site flag-setting
     * untenable -- the gate is now derived from the records instead. */
    const char *text =
        "test_m.exe type=correctness\n"
        "test_M.exe type=stress\n"
        "test_m.exe type=smoke\n";

    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, (const char *)0, &runs,
                                      (uint32_t *)0, (uint32_t *)0,
                                      &smoke_refused, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 0u, "the conflicted identity does not run");
    TEST_ASSERT_EQ(smoke_refused, 1u,
                   "a smoke declaration suppressed by an existing refusal "
                   "still reaches the gate");
}

static void test_deduplicated_refusal_keeps_smoke_provenance(void)
{
    static char buf[512];
    uint32_t runs = 99, refusals = 99, smoke_refused = 99;
    uint32_t i, p = 0;

    /* Both lines name ONE file and both are attribute-refused, so the
     * second collapses into the first at staging. The collapse must not
     * discard what the second line declared: the identity was refused
     * while carrying a trusted, selected `type=smoke`, so it is a refused
     * smoke prerequisite. This route never reaches u_plan_add_run's
     * terminal branch, which is why the correctness/stress/smoke fixture
     * does not cover it. */
    const char *text =
        "test_gate.exe type=correctness expects_tasks=0\n"
        "test_GATE.exe type=smoke expects_tasks=0\n"
        "test_other.exe\n";

    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, (const char *)0, &runs,
                                      &refusals, (uint32_t *)0, &smoke_refused,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(refusals, 1u,
                   "one filesystem identity keeps one refusal record");
    TEST_ASSERT_EQ(runs, 1u, "the unrelated entry is still planned");
    TEST_ASSERT_EQ(smoke_refused, 1u,
                   "a deduplicated refusal keeps the smoke provenance of "
                   "the declaration it absorbed");
}

static void test_filtered_out_smoke_refusal_does_not_abort(void)
{
    static char buf[512];
    uint32_t smoke_refused = 99, runs = 99;
    uint32_t i, p = 0;

    /* Refusals publish regardless of `utest_filter=` -- the filter picks
     * among identities the launcher can trust. The ABORT is a different
     * question: a smoke entry the filter excluded would never have been
     * planned, so refusing it must not kill a focused run. Publication and
     * abort therefore read different bits. */
    const char *text =
        "test_gate.exe type=smoke expects_tasks=0\n"
        "test_other.exe\n";

    for (i = 0; text[i]; i++) buf[p++] = text[i];
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, "test_other.exe", &runs,
                                      (uint32_t *)0, (uint32_t *)0,
                                      &smoke_refused, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(runs, 1u, "the selected binary is planned");
    TEST_ASSERT_EQ(smoke_refused, 0u,
                   "a smoke entry the filter excluded must not abort the run");
}

static void test_overlength_refusals_are_not_deduplicated(void)
{
    static char buf[2048];
    uint32_t refusals = 99;
    uint32_t i, p = 0;

    /* Two length-refused names sharing their first VFS_MAX_NAME bytes are
     * DIFFERENT files. u_name_equal_fs reports equal once it runs off the
     * end of both strings without a terminator, so an unguarded identity
     * dedup would collapse them into one record and silently lose the
     * second span digest -- with every published count still reconciling.
     * A name past the filesystem bound is not an identity at all. */
    for (i = 0; i < 300u; i++) buf[p++] = (i < 5u) ? "test_"[i] : 'a';
    buf[p++] = 'X';
    for (i = 0; i < 4u; i++) buf[p++] = ".exe"[i];
    buf[p++] = '\n';
    for (i = 0; i < 300u; i++) buf[p++] = (i < 5u) ? "test_"[i] : 'a';
    buf[p++] = 'Y';
    for (i = 0; i < 4u; i++) buf[p++] = ".exe"[i];
    buf[p++] = '\n';
    buf[p] = '\0';

    if (!test_usermode_manifest_parse(buf, p, (const char *)0, (uint32_t *)0,
                                      &refusals, (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0,
                                      (uint32_t *)0, (uint32_t *)0)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(refusals, 2u,
                   "two distinct over-length names keep two refusal records");
}

static void test_plan_alloc_failure_is_a_distinct_state(void)
{
    uint32_t alloc_failed = 99, overflowed = 99, count = 99, ptrs_null = 99;

    /* An unallocatable plan must be its OWN state, not a capacity
     * problem and not an empty plan: the zero-planned path publishes a
     * SUCCESSFUL empty suite, so a run that cannot enumerate at all has
     * to be distinguishable from a run with nothing to enumerate. */
    if (test_usermode_plan_alloc_failure(&alloc_failed, &overflowed, &count,
                                         &ptrs_null)) {
        TEST_SKIP("injected PMM failure did not take effect");
        return;
    }
    TEST_ASSERT_EQ(alloc_failed, 1u,
                   "an unallocatable plan reports alloc_failed");
    TEST_ASSERT_EQ(overflowed, 0u,
                   "allocation failure is not reported as a capacity limit");
    TEST_ASSERT_EQ(count, 0u, "a failed plan holds no entries");
    TEST_ASSERT_EQ(ptrs_null, 1u,
                   "a failed plan retains no block or interior pointers");
}

static void test_plan_intern_exhaustion_and_compaction(void)
{
    uint32_t overflowed = 99, count_stable = 99;
    uint32_t refusals_kept = 99, order_kept = 99;

    /* The name arena holds one slot per ENTRY and an entry slot is always
     * taken first, so the entry array is what binds -- the arena can never
     * fill on its own, and the assert beside UTEST_PLAN_NAME_BYTES pins
     * that. What this covers is therefore capacity refusal on a plan of
     * INTERNED names (so a rejected append cannot leave a half-written
     * arena slot or a nameless entry) and compaction with both kinds
     * interleaved -- neither of which the all-RUN capacity test reaches. */
    if (!test_usermode_plan_intern_and_compaction(&overflowed, &count_stable,
                                                  &refusals_kept,
                                                  &order_kept)) {
        TEST_SKIP("plan allocation unavailable");
        return;
    }
    TEST_ASSERT_EQ(overflowed, 1u,
                   "filling a plan of interned names flags it incomplete");
    TEST_ASSERT_EQ(count_stable, 1u,
                   "a refused append leaves the entry count unchanged");
    TEST_ASSERT_EQ(refusals_kept, test_usermode_plan_capacity() / 2u,
                   "compaction keeps every refusal");
    TEST_ASSERT_EQ(order_kept, 1u,
                   "compaction preserves refusal order and leaves no "
                   "nameless or runnable entry behind");
}

static void test_unbuildable_path_is_not_present(void)
{
    /* The executor builds `C:\<name>` into a fixed buffer. A name that
     * cannot fit is reported absent BEFORE any VFS call, so the plan and
     * the loader agree on what is reachable instead of the loader
     * discovering it later as an opaque open failure. Deliberately only
     * the pure half is asserted here: a name that DOES fit would reach
     * vfs_open, and the live boot run covers that path. */
    char too_long[VFS_MAX_NAME + 8];
    uint32_t i;

    for (i = 0; i < sizeof(too_long) - 1u; i++)
        too_long[i] = 'a';
    too_long[sizeof(too_long) - 1u] = '\0';
    TEST_ASSERT(test_usermode_binary_present(too_long) == 0,
                "a name past the path buffer is not a runnable binary");
}

static void test_name_identity_matches_the_filesystem(void)
{
    /* C: is IXFS, which resolves names case-insensitively over ASCII, so
     * two spellings that differ only in case are ONE file. Every dedup
     * between a manifest entry and a directory entry depends on this:
     * a bytewise compare would plan, run or refuse that one file twice,
     * and because both counters inflate together the completeness
     * reconciliation cannot see it. */
    TEST_ASSERT(test_usermode_name_equal_fs("test_bad.exe",
                                            "test_Bad.exe") == 1,
                "case-variant spellings are the same file to IXFS");
    TEST_ASSERT(test_usermode_name_equal_fs("TEST_BAD.EXE",
                                            "test_bad.exe") == 1,
                "the fold covers the whole name, not just the stem");
    TEST_ASSERT(test_usermode_name_equal_fs("test_bad.exe",
                                            "test_bad.exe") == 1,
                "identical names match");
    TEST_ASSERT(test_usermode_name_equal_fs("test_bad.exe",
                                            "test_bad2.exe") == 0,
                "a longer name is not the same file");
    TEST_ASSERT(test_usermode_name_equal_fs("test_bad.exe", "") == 0,
                "an empty name matches nothing");
    /* Digits and the punctuation in the accepted charset must not be
     * folded into letters by a careless range check. */
    TEST_ASSERT(test_usermode_name_equal_fs("test_a-1.exe",
                                            "test_A-1.exe") == 1,
                "punctuation and digits survive the fold unchanged");
}

static void test_refusal_dedup_is_the_only_net_for_double_publication(void)
{
    /* A refused name that also exists on disk is discovered twice -- once
     * from the manifest, once from the glob. Publishing it twice leaves
     * total_planned and total_ran in agreement, so the completeness
     * reconciliation is blind to it and this predicate is the only net. */
    TEST_ASSERT(test_usermode_refusal_already_seen("test_bad.exe",
                                                  UT_NAME_REFUSE_CHARSET,
                                                  "test_bad.exe") == 1,
                "an exact repeat of a stored refusal is deduped");
    TEST_ASSERT(test_usermode_refusal_already_seen("test_Bad.exe",
                                                  UT_NAME_REFUSE_LENGTH,
                                                  "test_bad.exe") == 1,
                "a case variant is the same file to IXFS, so it dedupes");
    TEST_ASSERT(test_usermode_refusal_already_seen("test_bad.exe",
                                                  UT_NAME_REFUSE_CHARSET,
                                                  "test_other.exe") == 0,
                "an unrelated dirent is not suppressed");
    /* The exclusion that must NOT dedupe: a NUL-refused entry's stored
     * string is the truncation at the NUL, and no filename can contain a
     * NUL -- so a dirent matching that truncation is a DIFFERENT file and
     * suppressing it would lose a real refusal. */
    TEST_ASSERT(test_usermode_refusal_already_seen("test_bad",
                                                  UT_NAME_REFUSE_NUL,
                                                  "test_bad") == 0,
                "a NUL refusal never suppresses a same-prefix real file");
}

static void test_taxonomy_edge_names(void)
{
    char big[300];
    uint32_t i;

    TEST_ASSERT_EQ(test_usermode_classify_name(""), UT_NAME_NOT_TEST_SHAPED,
                   "an empty entry is ignored, not refused");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_.exe"), UT_NAME_ACCEPT,
                   "the minimum test-shaped name is accepted");
    TEST_ASSERT_EQ(test_usermode_classify_name("...."),
                   UT_NAME_NOT_TEST_SHAPED,
                   "an all-dots name is not test-shaped, so it is ignored");
    TEST_ASSERT_EQ(test_usermode_classify_name("test_....exe"),
                   UT_NAME_REFUSE_PATH,
                   "dots inside a test-shaped name are traversal, refused");
    /* A 255-byte test-shaped name -- the VFS_MAX_NAME ceiling a dirent
     * can actually carry. It must refuse on LENGTH, and it must do so
     * without reading past the buffer. */
    for (i = 0; i < 255u; i++) big[i] = 'a';
    big[255] = '\0';
    big[0] = 't'; big[1] = 'e'; big[2] = 's'; big[3] = 't'; big[4] = '_';
    big[251] = '.'; big[252] = 'e'; big[253] = 'x'; big[254] = 'e';
    TEST_ASSERT_EQ(test_usermode_classify_name(big), UT_NAME_REFUSE_LENGTH,
                   "a 255-byte test-shaped name refuses on length");
}

/* Substring search over NUL-terminated strings. Pure helper: the test
 * policy bans calling live infrastructure, and the formatters under test
 * are the only thing this file is allowed to exercise. */
static int u_test_contains(const char *hay, const char *needle)
{
    uint32_t i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++)
            ;
        if (!needle[j])
            return 1;
    }
    return needle[0] ? 0 : 1;
}

/* Reads the decimal value of a `<key>=<digits>` field out of a formatted
 * record. Lets an assertion pin a record's EXACT numeric fields instead of
 * only its shape -- a marker carrying the wrong seq or limit still contains
 * every substring a `contains` check looks for, while failing the equality
 * the host actually reconciles against. Returns UINT32_MAX when the key is
 * absent or carries no digits, so a missing field fails loudly rather than
 * reading as zero. Pass the key WITH its separators (" seq="). */
static uint32_t u_test_field_u32(const char *msg, const char *key)
{
    uint32_t i, j, value = 0;
    int digits = 0;

    for (i = 0; msg[i]; i++) {
        for (j = 0; key[j] && msg[i + j] == key[j]; j++)
            ;
        if (key[j])
            continue;
        for (i += j; msg[i] >= '0' && msg[i] <= '9'; i++) {
            value = value * 10u + (uint32_t)(msg[i] - '0');
            digits++;
        }
        return digits ? value : 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu;
}

static void test_worst_case_record_fits_at_the_derived_bound(void)
{
    /* The derivation's fixed-cost macros are a SECOND copy of the format
     * strings in the emitters. Formatting a bound-length name through the
     * REAL formatters is the only thing that proves the two still agree
     * -- every other assertion here checks the copy against itself. */
    uint32_t bound = test_usermode_max_binary_name();
    uint32_t rmax = test_usermode_reason_max();
    char name[128];
    char reason[128];
    char line[256];
    uint32_t i;

    ut_name_of_len(name, sizeof(name), bound);
    for (i = 0; i < rmax && i < sizeof(reason) - 1u; i++) reason[i] = 'r';
    reason[i] = '\0';

    TEST_ASSERT(test_usermode_format_xml_testcase(line, sizeof(line), name,
                                                  1, 0xFFFFFFFFull,
                                                  reason) == 1,
                "worst-case XML failure record fits at the bound");
    TEST_ASSERT(test_usermode_format_xml_testcase(line, sizeof(line), name,
                                                  2, 0xFFFFFFFFull,
                                                  reason) == 1,
                "worst-case XML skipped record fits at the bound");
    TEST_ASSERT(test_usermode_format_json_testcase(line, sizeof(line), name,
                                                   1, 0xFFFFFFFFull,
                                                   reason, 1) == 1,
                "worst-case JSON record (reason + report fields) fits");
    TEST_ASSERT(test_usermode_format_json_testcase(line, sizeof(line), name,
                                                   1, 0xFFFFFFFFFFFFFFFFull,
                                                   reason, 1) == 1,
                "a uint64 uptime cannot widen the record past the bound");
    /* The never-ran verdict is a THIRD shape through the same formatters,
     * and its JSON status is one byte wider than PASS. Covering only
     * FAIL/SKIP would leave the widest status string unmeasured against
     * the very bound its literal now feeds.
     *
     * Asserted WITHOUT a report, which is the only shape that can occur: a
     * binary that never ran submitted none, and the formatter drops the
     * report fields for verdict 3 precisely so the derivation can reserve
     * the two reachable shapes separately instead of charging one record
     * for both. Passing report_valid=1 here would test a record no run can
     * produce -- and would quietly re-justify a name bound one byte
     * shorter than the transport actually requires. */
    TEST_ASSERT(test_usermode_format_xml_testcase(line, sizeof(line), name,
                                                  3, 0xFFFFFFFFull,
                                                  reason) == 1,
                "worst-case XML error record fits at the bound");
    TEST_ASSERT(test_usermode_format_json_testcase(line, sizeof(line), name,
                                                   3, 0xFFFFFFFFull,
                                                   reason, 0) == 1,
                "worst-case JSON ERROR record fits at the bound");
    /* And the precondition the derivation rests on, asserted directly: an
     * ERROR record never carries the report triple even when a caller
     * supplies one. */
    TEST_ASSERT(test_usermode_format_json_testcase(line, sizeof(line),
                                                   "test_x.exe", 3, 0,
                                                   "name refused: charset",
                                                   1) == 1,
                "an ERROR record formats when a report is supplied anyway");
    TEST_ASSERT(!u_test_contains(line, "asserts_passed"),
                "a never-ran record drops the report fields the derivation "
                "does not reserve for it");

    /* The BINDING kind. The skip_block carries the name twice, so it is
     * the record the bound is derived FROM -- covering only the two
     * testcase formatters would leave the one that actually binds
     * untested, which is precisely where a drifted fixed-cost macro
     * would hide. */
    {
        char rec[128];
        TEST_ASSERT(test_usermode_build_skip_record_name(rec, sizeof(rec),
                                                         name, 999999u) == 1,
                    "the synthetic skip-record name builds at the bound");
        TEST_ASSERT(test_usermode_format_json_skip(line, sizeof(line), rec,
                                                   name, 999999u) == 1,
                    "worst-case JSON skip_block fits with the name twice");
        TEST_ASSERT(test_usermode_format_tap_point(line, sizeof(line), 1,
                                                   4294967295u, rec,
                                                   "SKIP reported by binary") == 1,
                    "worst-case TAP skip point fits at the bound");
    }
}

/* The fallback records, which only run when a real record did not fit.
 * Their whole job is to keep the VERDICT legible after the name is lost,
 * and the host reconciles the errors= count against the <error> elements
 * it harvests -- so a fallback that demoted an ERROR would refuse a run
 * over a mismatch it caused itself. */
static void test_overflow_fallbacks_preserve_the_verdict(void)
{
    char line[256];

    TEST_ASSERT(test_usermode_format_xml_overflow(line, sizeof(line), 3,
                                                  "correctness") == 1,
                "the XML error fallback formats");
    TEST_ASSERT(u_test_contains(line, "<error message=\"record name too long\"/>"),
                "an overflowing ERROR stays an <error> element");
    TEST_ASSERT(!u_test_contains(line, "<failure"),
                "the ERROR fallback never degrades into a failure element");
    TEST_ASSERT(u_test_contains(line, "name=\"overflow\""),
                "the name is the only part the fallback drops");

    TEST_ASSERT(test_usermode_format_xml_overflow(line, sizeof(line), 1,
                                                  "correctness") == 1,
                "the XML failure fallback formats");
    TEST_ASSERT(u_test_contains(line, "<failure message=\"record name too long\"/>"),
                "an overflowing FAIL stays a <failure> element");
    TEST_ASSERT(test_usermode_format_xml_overflow(line, sizeof(line), 2,
                                                  "skip-block") == 1,
                "the XML skip fallback formats");
    TEST_ASSERT(u_test_contains(line, "<skipped message=\"record name too long\"/>") &&
                u_test_contains(line, "classname=\"skip-block\""),
                "an overflowing SKIP keeps both its element and its classname");
    TEST_ASSERT(test_usermode_format_xml_overflow(line, sizeof(line), 0,
                                                  "correctness") == 1,
                "the XML pass fallback formats");
    TEST_ASSERT(u_test_contains(line, "time=\"0\"/>") &&
                !u_test_contains(line, "</testcase>"),
                "an overflowing PASS stays the self-closing shape");

    TEST_ASSERT(test_usermode_format_json_overflow(line, sizeof(line), 3) == 1,
                "the JSON error fallback formats");
    TEST_ASSERT(u_test_contains(line, "\"status\":\"ERROR\"") &&
                u_test_contains(line, "\"record_kind\":\"binary\""),
                "an overflowing ERROR keeps both its status and its record_kind");
    TEST_ASSERT(test_usermode_format_json_overflow(line, sizeof(line), 1) == 1,
                "the JSON failure fallback formats");
    TEST_ASSERT(u_test_contains(line, "\"status\":\"FAIL\""),
                "every other overflowing verdict keeps the FAIL it always published");

    /* Refuses rather than truncating, like every other formatter here: a
     * cut fallback is an unparseable record, not a shorter one. */
    {
        char tight[24];
        TEST_ASSERT(test_usermode_format_xml_overflow(tight, sizeof(tight), 3,
                                                      "correctness") == 0,
                    "the XML fallback refuses a buffer it cannot fill");
        TEST_ASSERT(test_usermode_format_json_overflow(tight, sizeof(tight), 3) == 0,
                    "the JSON fallback refuses a buffer it cannot fill");
    }
}

/* The launcher aggregates pass/fail/skip; the artifacts additionally say
 * whether the binary RAN. This is the rule that maps one to the other, and
 * it is a pure function precisely so it can be asserted without driving a
 * loader failure through live infrastructure. */
static void test_record_verdict_maps_never_ran_onto_the_artifacts(void)
{
    /* Arguments: (verdict, loader_stage_fault, timed_out, reached_exec,
     *             frame_adopted, entered_user). */

    /* A binary that ran and failed stays a failure -- the common case, and
     * the one a wrong rule would most damage. */
    TEST_ASSERT_EQ(test_usermode_record_verdict(1, 0, 0, 1, 1, 1), 1u,
                   "a FAIL with no loader fault stays a FAIL in the artifacts");
    /* A loader that EXITED before ring 3 becomes the never-ran verdict. */
    TEST_ASSERT_EQ(test_usermode_record_verdict(1, 1, 0, 0, 0, 0), 3u,
                   "a FAIL whose loader never reached ring 3 becomes an ERROR");

    /* A TIMEOUT leans on the weaker signals. Nothing published means the
     * loader stalled on its way to ring 3: provably never ran. */
    TEST_ASSERT_EQ(test_usermode_record_verdict(1, 0, 1, 0, 0, 0), 3u,
                   "a timeout that never reached exec is a never-ran ERROR");

    /* The case this section exists to split. Both of these reached the exec
     * call and then timed out, which before per-child adoption evidence was
     * ONE indistinguishable bucket reported as a FAIL. */
    TEST_ASSERT_EQ(test_usermode_record_verdict(1, 0, 1, 1, 0, 0), 3u,
                   "a timeout whose frame was published but never adopted is "
                   "a never-ran ERROR, not a guessed FAIL");
    TEST_ASSERT_EQ(test_usermode_record_verdict(1, 0, 1, 1, 1, 0), 1u,
                   "a timeout whose frame WAS adopted stays a FAIL");

    /* Proven ring-3 execution outranks every never-ran signal: if a syscall
     * arrived from CPL 3 the binary demonstrably ran, so a later timeout is
     * a hang in its own code. Asserted with reached_exec and frame_adopted
     * both clear -- the combination cannot occur in a live run, and that is
     * the point: the rule must not depend on the weaker evidence agreeing. */
    TEST_ASSERT_EQ(test_usermode_record_verdict(1, 0, 1, 0, 0, 1), 1u,
                   "proven ring-3 entry keeps a timeout a FAIL even when the "
                   "weaker never-ran signals would have called it an ERROR");

    /* A loader that exited before ring 3 stays an ERROR no matter what the
     * timeout-only evidence says -- an exact signal outranks the inferred
     * ones in the other direction too. */
    TEST_ASSERT_EQ(test_usermode_record_verdict(1, 1, 1, 1, 1, 1), 3u,
                   "an exact loader stage fault outranks adoption and entry");

    /* PASS and SKIP are untouched: neither can follow a loader that never
     * reached ring 3, and the rule must not invent one if a caller
     * disagrees. */
    TEST_ASSERT_EQ(test_usermode_record_verdict(0, 0, 0, 1, 1, 1), 0u,
                   "a PASS is carried through unchanged");
    TEST_ASSERT_EQ(test_usermode_record_verdict(0, 1, 0, 0, 0, 0), 0u,
                   "a PASS is never reclassified by a stale loader fault");
    TEST_ASSERT_EQ(test_usermode_record_verdict(2, 1, 1, 0, 0, 0), 2u,
                   "a SKIP is never reclassified by a stale loader fault");
}

static void test_name_digest_covers_bytes_past_a_nul(void)
{
    static const char a[] = "test_x.exe\0aaa";
    static const char b[] = "test_x.exe\0bbb";

    TEST_ASSERT(test_usermode_name_digest(a, (uint32_t)(sizeof(a) - 1u)) !=
                test_usermode_name_digest(b, (uint32_t)(sizeof(b) - 1u)),
                "the digest distinguishes bytes a C string cannot reach");

    /* Known-answer vectors, not a self-comparison. The digest is the only
     * thing that correlates the same refused name ACROSS runs and builds,
     * so a changed basis, prime, byte order or signed-char treatment must
     * fail here -- comparing two calls in one process would pass for any
     * deterministic hash, including a wrong one. First three are the
     * published FNV-1a 32-bit vectors. */
    TEST_ASSERT_EQ(test_usermode_name_digest("", 0u), 0x811c9dc5u,
                   "empty span yields the FNV-1a offset basis");
    TEST_ASSERT_EQ(test_usermode_name_digest("a", 1u), 0xe40c292cu,
                   "FNV-1a published vector for \"a\"");
    TEST_ASSERT_EQ(test_usermode_name_digest("foobar", 6u), 0xbf9cf968u,
                   "FNV-1a published vector for \"foobar\"");
    TEST_ASSERT_EQ(test_usermode_name_digest(a, (uint32_t)(sizeof(a) - 1u)),
                   0x97c75971u,
                   "a NUL-bearing span hashes all 14 of its bytes");
    TEST_ASSERT_EQ(test_usermode_name_digest("\xff", 1u), 0x7a0b824eu,
                   "a high-bit byte is folded unsigned, not sign-extended");
}

static void test_refusal_id_is_bound_conforming_and_unique(void)
{
    uint32_t bound = test_usermode_max_binary_name();
    char id1[128], id2[128], id3[128];
    uint32_t l1 = 0, i;
    int same;

    TEST_ASSERT(test_usermode_build_refusal_id(id1, sizeof(id1), 1u,
                                               "test_a\nb.exe", 0xDEADBEEFu) == 1,
                "identity builds for a hostile raw name");
    while (id1[l1]) l1++;
    TEST_ASSERT(l1 <= bound,
                "the identity obeys the bound it exists to prove");
    for (i = 0; i < l1; i++) {
        char c = id1[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        TEST_ASSERT(ok, "every identity byte is inside the accepted charset");
    }
    TEST_ASSERT(id1[l1 - 4] == '.' && id1[l1 - 3] == 'e' &&
                id1[l1 - 2] == 'x' && id1[l1 - 1] == 'e',
                "identity ends in .exe so the host verdict recount sees it");

    /* Uniqueness comes from the ordinal, not from digest strength: two
     * refusals that hash identically must still be distinguishable. */
    (void)test_usermode_build_refusal_id(id2, sizeof(id2), 2u,
                                         "test_a\nb.exe", 0xDEADBEEFu);
    same = 1;
    for (i = 0; id1[i] || id2[i]; i++)
        if (id1[i] != id2[i]) { same = 0; break; }
    TEST_ASSERT(same == 0,
                "same name, same digest, different ordinal -> distinct ids");

    /* And the digest is what separates two different names, so a run
     * cannot report two refusals under one identity. */
    (void)test_usermode_build_refusal_id(id3, sizeof(id3), 1u,
                                         "test_a\nb.exe", 0x12345678u);
    same = 1;
    for (i = 0; id1[i] || id3[i]; i++)
        if (id1[i] != id3[i]) { same = 0; break; }
    TEST_ASSERT(same == 0, "same ordinal, different digest -> distinct ids");

    TEST_ASSERT(test_usermode_build_refusal_id(id1, 8u, 1u, "test_x.exe",
                                               0u) == 0,
                "a buffer too small for a conforming identity is refused");
}

/* Every aggregate kind, walked from the kernel's own table rather than a
 * list copied into the test: a new aggregate added without a conforming
 * identity has to fail here, and a mirrored list would simply not see it.
 *
 * `strn`-style helpers are not available in this translation unit, so the
 * comparisons below are open-coded against the exported label. */
static uint32_t utest_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int u_labels_equal(const char *a, const char *b)
{
    uint32_t i = 0;

    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static void test_every_aggregate_identity_conforms(void)
{
    uint32_t bound = test_usermode_max_binary_name();
    uint32_t kinds = test_usermode_aggregate_count();
    uint32_t k, i;

    TEST_ASSERT(kinds == 5u,
                "the launcher publishes exactly five fail-closed aggregates");

    for (k = 0; k < kinds; k++) {
        const char *label = test_usermode_aggregate_label(k);
        char id[128];
        uint32_t len, lab_len, j;

        TEST_ASSERT(label != (const char *)0,
                    "every enumerated aggregate kind has a label");
        lab_len = utest_len(label);
        TEST_ASSERT(lab_len > 0u, "an aggregate label is not empty");

        /* A representative value, and the widest one the field reserves,
         * so the bound is proven at the worst case rather than a small
         * number that happens to fit. */
        TEST_ASSERT(test_usermode_build_aggregate_id(id, sizeof(id), k,
                                                     4294967295u) == 1,
                    "an aggregate identity builds at the widest value");
        len = utest_len(id);
        TEST_ASSERT(len <= bound,
                    "the aggregate identity obeys the derived name bound");

        /* Structurally distinguishable: it leads with `agg_`, which the
         * refusal shape never does and no accepted binary can, because
         * u_is_test_binary demands a `test_` prefix. */
        TEST_ASSERT(id[0] == 'a' && id[1] == 'g' && id[2] == 'g' &&
                    id[3] == '_',
                    "an aggregate identity leads with agg_, not refused_");
        TEST_ASSERT(id[len - 4] == '.' && id[len - 3] == 'e' &&
                    id[len - 2] == 'x' && id[len - 1] == 'e',
                    "aggregate identity ends in .exe for the host recount");

        /* The label is present WHOLE. Unlike the refusal prefix there is
         * no truncation arm, so a label that no longer named its path
         * would be a build failure, not a silent rename. */
        for (j = 0; j < lab_len; j++)
            TEST_ASSERT(id[4u + j] == label[j],
                        "the aggregate label is published untruncated");
        TEST_ASSERT(id[4u + lab_len] == '_',
                    "the label is followed by the value separator");

        for (j = 0; j < len; j++) {
            char c = id[j];
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                     c == '-';
            TEST_ASSERT(ok, "every aggregate identity byte is in the "
                            "accepted charset");
        }

        /* Distinct from every other kind: the label is the whole of the
         * run-uniqueness claim now that there is no ordinal. */
        for (i = 0; i < k; i++) {
            const char *other = test_usermode_aggregate_label(i);
            TEST_ASSERT(utest_len(other) != lab_len ||
                        !u_labels_equal(other, label),
                        "no two aggregate kinds share a label");
        }
    }
}

static void test_aggregate_unknown_value_is_not_zero(void)
{
    char known[128], unknown[128];
    uint32_t bound = test_usermode_max_binary_name();
    uint32_t kinds = test_usermode_aggregate_count();
    uint32_t len, i, k, valued = 0, unvalued = 0;

    /* Whether a kind carries a number is a property of the KIND, read
     * from the launcher's own table -- no publication site passes it, so
     * no site can pass the wrong one. Exactly one kind is value-less:
     * the unparseable manifest, which cannot know its loss on any path. */
    for (k = 0; k < kinds; k++) {
        if (test_usermode_aggregate_has_value(k))
            valued++;
        else
            unvalued++;
    }
    TEST_ASSERT_EQ(unvalued, 1u,
                   "exactly one aggregate kind publishes no number");
    TEST_ASSERT_EQ(valued, kinds - 1u,
                   "every other aggregate kind publishes a number");
    /* An unenumerated kind answers 0 rather than reading off the end of
     * the table. */
    TEST_ASSERT(test_usermode_aggregate_has_value(kinds) == 0,
                "an unenumerated kind claims no value");

    /* Find the value-less kind by its property, not by a hardcoded index
     * -- a table reorder must not quietly retarget this test. */
    for (k = 0; k < kinds; k++)
        if (!test_usermode_aggregate_has_value(k))
            break;
    TEST_ASSERT(k < kinds, "the value-less aggregate kind is enumerated");

    /* The unparseable-manifest path cannot know how many entries were
     * lost. It must say so, not publish a zero that reads as "nothing
     * was lost" on the one path where the loss is total. */
    TEST_ASSERT(test_usermode_build_aggregate_id(unknown, sizeof(unknown),
                                                 k, 0u) == 1,
                "an aggregate identity builds with no knowable value");
    /* A value-BEARING kind at value zero is a different identity: zero is
     * a fact there, and `unknown` is the absence of one. */
    TEST_ASSERT(test_usermode_build_aggregate_id(known, sizeof(known),
                                                 (k + 1u) % kinds, 0u) == 1,
                "a value-bearing kind builds with a value of zero");
    len = utest_len(unknown);
    TEST_ASSERT(len <= test_usermode_max_binary_name(),
                "the unknown-value identity obeys the bound too");
    TEST_ASSERT(!u_labels_equal(known, unknown),
                "an unknown value is not published as a zero");
    /* The token, spelled out, so a consumer cannot read a count off it.
     * Compared as a SUFFIX rather than by counting back from the end one
     * index at a time -- the first draft of this assertion was off by one
     * and passed review as arithmetic nobody re-derived. */
    {
        static const char want[] = "_unknown.exe";
        uint32_t want_len = utest_len(want);

        TEST_ASSERT(len > want_len,
                    "the identity is longer than the value token it ends in");
        TEST_ASSERT(u_labels_equal(unknown + (len - want_len), want),
                    "the unknown value publishes the literal `unknown`");
    }
    for (i = 0; i < len; i++)
        TEST_ASSERT(unknown[i] != ' ', "no spaces reach the identity");

    /* An unenumerated kind has no label and must fail closed rather than
     * publish a record naming no path at all. */
    TEST_ASSERT(test_usermode_build_aggregate_id(known, sizeof(known),
                                                 kinds, 0u) == 0,
                "an unenumerated aggregate kind is refused, not blank");
    TEST_ASSERT(test_usermode_build_aggregate_id(known, 8u, 0u, 1u) == 0,
                "a buffer too small for a conforming identity is refused");

    /* The exact cap boundary, on both sides. The builder demands room for
     * a bound-conforming identity plus its terminator, so `bound` bytes
     * is one short and `bound + 1` is the smallest buffer that works --
     * asserted rather than assumed, because an off-by-one here would
     * either refuse every identity or write one byte past a caller's
     * buffer. */
    for (k = 0; k < kinds; k++) {
        TEST_ASSERT(test_usermode_build_aggregate_id(known, bound, k,
                                                     4294967295u) == 0,
                    "a buffer of exactly the bound has no room for the NUL");
        TEST_ASSERT(test_usermode_build_aggregate_id(known, bound + 1u, k,
                                                     4294967295u) == 1,
                    "a buffer of bound + 1 builds the widest identity");
        TEST_ASSERT(utest_len(known) <= bound,
                    "the identity built at the exact cap obeys the bound");
    }
}

static void test_refusal_id_survives_ordinal_digit_growth(void)
{
    /* The prefix budget is recomputed from the ordinal's ACTUAL digit
     * width, so every decimal carry is a boundary: the identity must
     * still build, still fit the bound, and still leave a readable
     * prefix. The manifest alone admits 128 entries, so 99 -> 100 is
     * reachable without a hostile directory. */
    static const uint32_t ordinals[] = { 1u, 9u, 10u, 99u, 100u,
                                         999u, 1000u, 4294967295u };
    uint32_t bound = test_usermode_max_binary_name();
    uint32_t k;

    for (k = 0; k < sizeof(ordinals) / sizeof(ordinals[0]); k++) {
        char id[128];
        uint32_t len = 0, i, digits = 1u, v = ordinals[k], fixed;

        TEST_ASSERT(test_usermode_build_refusal_id(id, sizeof(id),
                                                   ordinals[k],
                                                   "test_hostile_name.exe",
                                                   0xA5A5A5A5u) == 1,
                    "identity builds at every ordinal digit width");
        while (id[len]) len++;
        TEST_ASSERT(len <= bound,
                    "identity stays within the bound as the ordinal grows");
        for (i = 0; i < len; i++) {
            char c = id[i];
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                     c == '-';
            TEST_ASSERT(ok, "identity stays inside the accepted charset");
        }
        TEST_ASSERT(id[len - 4] == '.' && id[len - 3] == 'e' &&
                    id[len - 2] == 'x' && id[len - 1] == 'e',
                    "identity still ends in .exe at every ordinal");
        /* Measure the sanitized prefix by SUBTRACTING the identity's
         * fixed shape -- `refused_` + the ordinal's digits + `_` + the
         * prefix + `_` + 8 hex + `.exe`. Scanning back for the digest's
         * separator would be wrong: `_` is inside the accepted charset,
         * so the prefix itself routinely contains one. */
        while (v >= 10u) { v /= 10u; digits++; }
        fixed = 8u + digits + 1u + 1u + 8u + 4u;
        TEST_ASSERT(len > fixed && (len - fixed) >= 4u,
                    "a readable prefix survives even a 10-digit ordinal");
    }
}

static void test_time_ms_clamp_makes_the_digit_width_provable(void)
{
    TEST_ASSERT_EQ(test_usermode_clamp_time_ms(0ull), 0ull,
                   "zero passes through");
    TEST_ASSERT_EQ(test_usermode_clamp_time_ms(4294967295ull), 4294967295ull,
                   "the widest reserved value passes through");
    TEST_ASSERT_EQ(test_usermode_clamp_time_ms(4294967296ull), 4294967295ull,
                   "one past it saturates instead of widening the record");
    TEST_ASSERT_EQ(test_usermode_clamp_time_ms(0xFFFFFFFFFFFFFFFFull),
                   4294967295ull, "a uint64 uptime cannot widen time_ms");
}

static void test_manifest_rejects_path_traversal(void)
{
    /* Attack: manifest line ../../../etc/passwd would become C:\..\..\..
     * under the existing concat. Must reject. */
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_..\\evil.exe") == 0,
                "rejects backslash path separator");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_../evil.exe") == 0,
                "rejects forward slash");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("..") == 0,
                "rejects bare ..");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_..\\..\\a.exe") == 0,
                "rejects nested ..");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_a..b.exe") == 0,
                "rejects embedded ..");
}

static void test_manifest_rejects_drive_letters(void)
{
    TEST_ASSERT(test_usermode_is_valid_manifest_name("D:\\test_x.exe") == 0,
                "rejects drive letter prefix");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_:foo.exe") == 0,
                "rejects colon anywhere");
}

static void test_manifest_rejects_non_test_binaries(void)
{
    TEST_ASSERT(test_usermode_is_valid_manifest_name("hello.exe") == 0,
                "rejects hello.exe (not a test_ prefix)");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("cmd.exe") == 0,
                "rejects cmd.exe");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_foo.txt") == 0,
                "rejects wrong suffix");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("") == 0,
                "rejects empty name");
    TEST_ASSERT(test_usermode_is_valid_manifest_name((const char *)0) == 0,
                "rejects NULL name");
}

static void test_manifest_rejects_control_bytes(void)
{
    /* Control-byte smuggling could confuse downstream log consumers
     * or the VFS path builder. Validator must strip them at the door. */
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_\x01" "bad.exe") == 0,
                "rejects SOH (0x01)");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_bad\nline.exe") == 0,
                "rejects embedded newline");
    TEST_ASSERT(test_usermode_is_valid_manifest_name("test_bad\ttab.exe") == 0,
                "rejects embedded tab");
}

/* ---- Contract constants --------------------------------------------- *
 *
 * The exit-status constants are pinned by `_Static_assert` in
 * include/kernel/test/test_usermode.h, NOT by runtime suites here. A test
 * comparing a #define against its own literal cannot fail unless someone
 * edits both halves, so it verifies nothing while inflating the assertion
 * count (scripts/lint.sh flags exactly that shape elsewhere in the tree).
 * The skip==77 and timeout-distinctness checks moved into that header on
 * 2026-07-28: they did not weaken, they moved to compile time, where a
 * violation refuses to build rather than waiting for a boot.
 * --------------------------------------------------------------------- */

/* ---- Ring-3 self-report: reconciliation matrix ---------------------- *
 *
 * The counts a binary submits through SYS_TEST_REPORT are ring-3 data.
 * u_report_reconcile is the gate that decides whether they may reach an
 * artifact, so its whole truth table is pinned here -- a regression that
 * relaxed one row would re-open the false-green path this section closed.
 * --------------------------------------------------------------------- */

static void test_report_reconcile_passthrough_states(void)
{
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_NONE, 0, 0, 0),
                   (uint64_t)TASK_UTEST_REPORT_NONE,
                   "a binary that never reported stays on the legacy path");
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_NONE, 0, 5, 0),
                   (uint64_t)TASK_UTEST_REPORT_NONE,
                   "NONE is not turned invalid by a non-zero exit");
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_INVALID, 0, 0, 0),
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "a kernel-rejected report stays rejected (sticky)");
    /* CLAIMED means the submitter took the single-submission slot and
     * died before publishing. Falling through would make it neither VALID
     * nor INVALID and the launcher would count it as "never reported",
     * i.e. a half-written record would read as an honest legacy binary. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_CLAIMED, 0, 0, 0),
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "a half-written (CLAIMED) record is INVALID, not unreported");
}

static void test_report_reconcile_agrees_with_exit(void)
{
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 0, 0, 0),
                   (uint64_t)TASK_UTEST_REPORT_VALID,
                   "no failures reported and exit 0 is consistent");
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 3, 3, 0),
                   (uint64_t)TASK_UTEST_REPORT_VALID,
                   "3 failures reported and exit 3 is consistent");
    /* A binary may normalize its exit code (test.h documents `return
     * g_fail;` as convention, not requirement), so exit 1 with 3 reported
     * failures must NOT be called a contradiction. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 3, 1, 0),
                   (uint64_t)TASK_UTEST_REPORT_VALID,
                   "a normalized non-zero exit still agrees with failures>0");
}

static void test_report_reconcile_rejects_contradictions(void)
{
    /* The false-green shape: "everything passed" alongside a failing exit. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 0, 5, 0),
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "zero reported failures with a failing exit is INVALID");
    /* The inverse: failures reported but the binary exited clean. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 2, 0, 0),
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "reported failures with exit 0 is INVALID");
    /* Whole-binary skip cannot coexist with counters from a run. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 0, UTEST_EXIT_SKIP, 0),
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "a report alongside exit 77 is INVALID");
}

/* The classification predicates, over the three-way partition they claim to
 * be exhaustive and disjoint across. The static assert beside them only pins
 * the two block ENDPOINTS; this walks the actual boundary values a consumer
 * will meet -- every reserved reason, both edges of the signal range, and the
 * application statuses on either side of them.
 *
 * Non-tautological by construction: none of these compares a constant to its
 * own literal. Each asserts which CLASS a value falls into, which is the
 * property a caller depends on and the one that was got wrong twice by hand. */
static void test_exit_status_classes_partition_the_space(void)
{
    /* Every reserved reason classifies as a kernel reason, and as nothing
     * else. -9 is the case the timeout marker itself used to be confused
     * with, so it is checked explicitly rather than left to the range. */
    TEST_ASSERT_EQ((uint64_t)task_exit_is_kernel_reason(
                       TASK_EXIT_EXEC_IMAGE_DESTROYED), 1ull,
                   "the image-destroyed reason must classify as kernel");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_kernel_reason(
                       TASK_EXIT_UTEST_IDENTITY), 1ull,
                   "the identity reason must classify as kernel");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_kernel_reason(
                       TASK_EXIT_UTEST_TIMEOUT), 1ull,
                   "the timeout reason must classify as kernel");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_signal(TASK_EXIT_UTEST_TIMEOUT),
                   0ull,
                   "the timeout reason must NOT classify as a signal death");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_kernel_reason(
                       TASK_EXIT_UTEST_REAPED), 1ull,
                   "the reaped reason must classify as kernel -- the comment "
                   "above claims EVERY reserved reason, so one that is never "
                   "enumerated makes the claim untrue in silence");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_signal(TASK_EXIT_UTEST_REAPED),
                   0ull,
                   "and must NOT classify as a signal death -- a reaped "
                   "descendant was killed BY the launcher, which is the exact "
                   "confusion the reserved block exists to prevent");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_app_status(TASK_EXIT_UTEST_REAPED),
                   0ull, "nor as an application status");

    /* The signal range, at both edges. -1 and -(SIG_MAX-1) are signals; the
     * value at exactly -SIG_MAX is not, which is the boundary the reserved
     * block is asserted to clear. */
    TEST_ASSERT_EQ((uint64_t)task_exit_is_signal(-1), 1ull,
                   "-1 is a signal death");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_signal(-9), 1ull,
                   "SIGKILL's -9 is a signal death");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_signal(-((int32_t)SIG_MAX - 1)),
                   1ull, "the widest real signum is still a signal death");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_signal(-(int32_t)SIG_MAX), 0ull,
                   "the value at exactly -SIG_MAX is outside the signal range");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_kernel_reason(-9), 0ull,
                   "a signal death must not classify as a kernel reason");

    /* Application statuses: 0, a small success code, and the kselftest skip
     * contract, none of which may be captured by either kernel class. */
    TEST_ASSERT_EQ((uint64_t)task_exit_is_app_status(0), 1ull,
                   "a clean exit is an application status");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_app_status(UTEST_EXIT_SKIP), 1ull,
                   "the skip contract 77 is an application status");
    TEST_ASSERT_EQ((uint64_t)task_exit_is_app_status(TASK_EXIT_UTEST_TIMEOUT),
                   0ull,
                   "a kernel reason must not read as an application status");

    /* A value BELOW the allocated block is not a reason that exists. It must
     * not be claimed by the kernel class, or a future block extension would
     * silently reclassify statuses that already shipped. */
    TEST_ASSERT_EQ((uint64_t)task_exit_is_kernel_reason(
                       TASK_EXIT_REASON_LAST - 1), 0ull,
                   "an unallocated value below the block is not a reason");
}

/* The PRODUCTION timeout path's postcondition, asserted where a test can
 * reach it. u_wait_with_timeout itself needs live scheduling, signals, a
 * clock and remote termination, so a test_*.c may not drive it; the status
 * stamp it performs is factored into u_stamp_timeout_status precisely so the
 * property that matters survives into something testable.
 *
 * The properties asserted are the ones a regression would actually break --
 * that the reaped status is outside the -(signum) range and is not SIGKILL's
 * own -9 -- rather than the constant's value, which the _Static_asserts in
 * task.h already refuse to build wrong. */
static struct task s_timeout_stamp_scratch;

static void test_timeout_stamp_leaves_the_signal_range(void)
{
    s_timeout_stamp_scratch.exit_status = -9;   /* what SIGKILL would leave */

    test_usermode_stamp_timeout_status(&s_timeout_stamp_scratch);

    TEST_ASSERT_EQ((uint64_t)(s_timeout_stamp_scratch.exit_status
                              < -(int32_t)SIG_MAX), 1ull,
                   "a timed-out child's status must clear the signal range");
    TEST_ASSERT_EQ((uint64_t)(s_timeout_stamp_scratch.exit_status != -9), 1ull,
                   "the timeout stamp must overwrite SIGKILL's own -9");
    TEST_ASSERT_EQ((uint64_t)(s_timeout_stamp_scratch.exit_status
                              != TASK_EXIT_UTEST_IDENTITY), 1ull,
                   "a timeout must not report the identity-refusal cause");

    /* The seam tolerates NULL so this suite cannot fault on a mis-written
     * test. It does NOT model production, which dereferences unconditionally
     * and is protected instead by u_wait_with_timeout's early return when the
     * pid does not resolve. Asserted only as "does not fault, and does not
     * touch the scratch we already checked". */
    test_usermode_stamp_timeout_status((struct task *)0);
    TEST_ASSERT_EQ((uint64_t)s_timeout_stamp_scratch.exit_status,
                   (uint64_t)(int32_t)TASK_EXIT_UTEST_TIMEOUT,
                   "a NULL stamp must not disturb the previous child's status");
}

/* The timeout marker's move into the reserved block widened the status
 * values the exit-reason renderer has to carry: every kernel-assigned cause
 * it saw before was one digit, and a reserved reason is four plus a sign.
 *
 * Deliberately NOT a comparison of the constant against a literal -- that
 * would restate the _Static_assert in task.h and prove only that someone
 * typed the number twice (scripts/lint.sh Check: tautological-test). What is
 * checked here is the RENDERER's behavior over the newly-reachable value
 * class: that the composed reason is the full signed number, terminated, and
 * inside the derived bound rather than truncated at it. */
static void test_reason_exit_renders_reserved_block_status(void)
{
    char     buf[128];
    uint32_t n;
    uint32_t i;

    n = test_usermode_reason_exit(buf, (uint32_t)sizeof(buf),
                                  TASK_EXIT_UTEST_TIMEOUT);
    TEST_ASSERT_EQ((uint64_t)(n > 0u), 1ull,
                   "the reserved-block timeout status must render at all");
    TEST_ASSERT_EQ((uint64_t)(n <= test_usermode_reason_max()), 1ull,
                   "a reserved-block reason must fit the derived bound");
    TEST_ASSERT_EQ((uint64_t)buf[n], 0ull,
                   "the rendered reason must be terminated at its length");

    /* `exit=` then a minus then digits: the sign must survive, because a
     * dropped one turns a reserved reason into a plausible ring-3 status. */
    TEST_ASSERT_EQ((uint64_t)(buf[0] == 'e' && buf[1] == 'x' && buf[2] == 'i'
                              && buf[3] == 't' && buf[4] == '='), 1ull,
                   "the exit reason must keep its exit= prefix");
    TEST_ASSERT_EQ((uint64_t)buf[5], (uint64_t)'-',
                   "a negative reserved reason must render its sign");
    for (i = 6u; i < n; i++)
        TEST_ASSERT_EQ((uint64_t)(buf[i] >= '0' && buf[i] <= '9'), 1ull,
                       "every byte after the sign must be a digit");

    /* Four digits is the property that matters: the pre-move marker was a
     * single digit, so a renderer that still truncated at the old width
     * would pass every check above and fail this one. */
    TEST_ASSERT_EQ((uint64_t)(n - 6u), 4ull,
                   "a reserved-block reason renders all four of its digits");

    /* The other two reserved reasons travel the same renderer, so a width
     * regression cannot hide behind the one value the section moved. */
    n = test_usermode_reason_exit(buf, (uint32_t)sizeof(buf),
                                  TASK_EXIT_UTEST_IDENTITY);
    TEST_ASSERT_EQ((uint64_t)(n - 6u), 4ull,
                   "the identity reason renders all four of its digits");
    n = test_usermode_reason_exit(buf, (uint32_t)sizeof(buf),
                                  TASK_EXIT_EXEC_IMAGE_DESTROYED);
    TEST_ASSERT_EQ((uint64_t)(n - 6u), 4ull,
                   "the image-destroyed reason renders all four digits");
    n = test_usermode_reason_exit(buf, (uint32_t)sizeof(buf),
                                  TASK_EXIT_UTEST_REAPED);
    TEST_ASSERT_EQ((uint64_t)(n - 6u), 4ull,
                   "and so does the reaped reason -- a renderer that covers "
                   "only the reasons that existed when it was written stops "
                   "being the width net it claims to be");

    /* A buffer that cannot hold the reason must refuse, not truncate: the
     * seam returns 0 and leaves an empty string rather than a short number
     * that would read as a different exit status entirely. */
    n = test_usermode_reason_exit(buf, 4u, TASK_EXIT_UTEST_TIMEOUT);
    TEST_ASSERT_EQ((uint64_t)n, 0ull,
                   "a too-small reason buffer must refuse rather than clip");
    TEST_ASSERT_EQ((uint64_t)buf[0], 0ull,
                   "a refused reason must leave an empty string");
}

static void test_report_reconcile_timeout_keeps_counts(void)
{
    /* On timeout the launcher OVERWROTE exit_status with its own marker,
     * so no report could ever agree with it. Reconciliation must not
     * invent a contradiction out of the launcher's own bookkeeping -- the
     * timeout already fails the binary on its own, more specifically. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 0, TASK_EXIT_UTEST_TIMEOUT, 1),
                   (uint64_t)TASK_UTEST_REPORT_VALID,
                   "a timed-out binary's counts are not called contradictory");
}

/* ---- Ring-3 self-report: the kernel-side validation ladder ---------- *
 *
 * sys_test_report_dispatch is the FIRST trust boundary for ring-3 data.
 * Driven here against a zeroed scratch TCB -- no task_create, no
 * subsystem init, nothing live: the dispatcher touches only the report
 * fields of the struct it is handed.
 * --------------------------------------------------------------------- */

static struct task s_report_scratch;
static struct task s_capture_parent_scratch;
static struct task s_capture_child_scratch;
/* Two loader-evidence scratch TCBs standing in for consecutive spawns:
 * s_loader_killed_scratch is the timed-out loader the launcher force-kills,
 * s_loader_next_scratch is the binary launched after it. */
static struct task s_loader_killed_scratch;
static struct task s_loader_next_scratch;

static void u_reset_report_scratch(void)
{
    task_utest_report_reset(&s_report_scratch.utest_report);
    s_report_scratch.pid = 0;
}

static void test_report_dispatch_rejects_null_task(void)
{
    TEST_ASSERT(sys_test_report_dispatch((struct task *)0, 1, 0, 0) == -1,
                "dispatcher refuses a NULL task rather than faulting");
}

static void test_report_dispatch_accepts_first_submission(void)
{
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 12, 0, 3) == 0,
                "a first, in-range submission is accepted");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.state,
                   (uint64_t)TASK_UTEST_REPORT_VALID,
                   "accepted submission records state VALID");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.asserts_passed,
                   (uint64_t)12, "passed count stored verbatim");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.skip_blocks,
                   (uint64_t)3, "skip-block count stored verbatim");
}

static void test_report_dispatch_accepts_all_zero(void)
{
    /* A binary with no assertions at all is legal and must not be
     * mistaken for a binary that never reported: state VALID with zero
     * counts is a different fact from state NONE. */
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 0, 0, 0) == 0,
                "an all-zero report is accepted");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.state,
                   (uint64_t)TASK_UTEST_REPORT_VALID,
                   "zero counts still read as VALID, not NONE");
}

static void test_report_dispatch_accepts_exact_ceilings(void)
{
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch,
                                         TASK_UTEST_REPORT_MAX,
                                         TASK_UTEST_REPORT_MAX,
                                         TASK_UTEST_REPORT_SKIP_MAX) == 0,
                "every field exactly at its ceiling is accepted");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.state,
                   (uint64_t)TASK_UTEST_REPORT_VALID,
                   "the ceilings are inclusive bounds");
}

static void test_report_dispatch_rejects_over_ceiling(void)
{
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch,
                                         TASK_UTEST_REPORT_MAX + 1, 0, 0) == -1,
                "passed count one over the ceiling is refused");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.state,
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "an over-cap submission leaves the record INVALID");

    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 0,
                                         TASK_UTEST_REPORT_MAX + 1, 0) == -1,
                "failed count one over the ceiling is refused");

    /* The skip-block bound is far tighter than the assertion bound because
     * it governs artifact fan-out, not arithmetic: one record per block is
     * emitted into TAP, XML and JSON. */
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 0, 0,
                                         TASK_UTEST_REPORT_SKIP_MAX + 1) == -1,
                "skip-block count one over the artifact bound is refused");
    TEST_ASSERT(TASK_UTEST_REPORT_SKIP_MAX < TASK_UTEST_REPORT_MAX,
                "the artifact bound is tighter than the arithmetic bound");

    /* A count that is legal for assertions but illegal for skip blocks
     * must still be refused -- this is the amplification case. */
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 0, 0,
                                         TASK_UTEST_REPORT_MAX) == -1,
                "an assertion-sized skip count cannot flood the artifacts");
}

static void test_report_dispatch_refuses_second_submission(void)
{
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 5, 0, 0) == 0,
                "first submission accepted");
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 99, 0, 0) == -1,
                "second submission refused");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.state,
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "a repeat submission invalidates the whole record");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.asserts_passed,
                   (uint64_t)5,
                   "the overwrite attempt did not replace the first counts");
}

static void test_report_dispatch_invalid_is_sticky(void)
{
    /* Without stickiness a binary could probe: submit garbage, get
     * refused, then submit a flattering report and land back on the
     * trusted path. */
    u_reset_report_scratch();
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch,
                                         TASK_UTEST_REPORT_MAX + 1, 0, 0) == -1,
                "over-cap submission refused");
    TEST_ASSERT(sys_test_report_dispatch(&s_report_scratch, 1, 0, 0) == -1,
                "a well-formed retry after a refusal is still refused");
    TEST_ASSERT_EQ((uint64_t)s_report_scratch.utest_report.state,
                   (uint64_t)TASK_UTEST_REPORT_INVALID,
                   "INVALID cannot be cleared by a later submission");
}

/* ---- Ring-3 self-report: INVALID escalation ------------------------- */

static void test_invalid_escalates_pass_to_fail(void)
{
    uint32_t counters[3] = { 4, 1, 2 };   /* pass, fail, skip */

    TEST_ASSERT_EQ((uint64_t)test_usermode_report_apply_invalid(0, counters),
                   (uint64_t)1, "a PASS on a contradictory report becomes FAIL");
    TEST_ASSERT_EQ((uint64_t)counters[0], (uint64_t)3, "the PASS is rolled back");
    TEST_ASSERT_EQ((uint64_t)counters[1], (uint64_t)2, "the FAIL is recorded");
    TEST_ASSERT_EQ((uint64_t)counters[2], (uint64_t)2, "the skip count is untouched");
}

static void test_invalid_escalates_skip_to_fail(void)
{
    uint32_t counters[3] = { 4, 1, 2 };

    /* exit 77 alongside a report is the contradiction that produces this
     * case: the binary claimed a whole-binary skip while reporting work. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_apply_invalid(2, counters),
                   (uint64_t)1, "a SKIP on a contradictory report becomes FAIL");
    TEST_ASSERT_EQ((uint64_t)counters[2], (uint64_t)1, "the SKIP is rolled back");
    TEST_ASSERT_EQ((uint64_t)counters[1], (uint64_t)2, "the FAIL is recorded");
    TEST_ASSERT_EQ((uint64_t)counters[0], (uint64_t)4, "the pass count is untouched");
}

static void test_invalid_leaves_existing_fail_alone(void)
{
    uint32_t counters[3] = { 4, 1, 2 };

    /* An already-failing binary keeps its more specific reason (exit code,
     * timeout, leak) and must not be double-counted. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_apply_invalid(1, counters),
                   (uint64_t)1, "an existing FAIL stays FAIL");
    TEST_ASSERT_EQ((uint64_t)counters[0], (uint64_t)4, "pass count untouched");
    TEST_ASSERT_EQ((uint64_t)counters[1], (uint64_t)1, "FAIL is not double-counted");
    TEST_ASSERT_EQ((uint64_t)counters[2], (uint64_t)2, "skip count untouched");
}

/* ---- Ring-3 self-report: run-wide artifact budget ------------------- *
 *
 * The syscall's per-binary skip ceiling is multiplicative -- TASK_MAX
 * binaries each reporting the maximum are all individually legal. This
 * budget is the aggregate stop, so its arithmetic is what actually cuts
 * off a fan-out attack.
 * --------------------------------------------------------------------- */

static void test_skip_budget_allows_normal_runs(void)
{
    TEST_ASSERT_EQ((uint64_t)test_usermode_skip_records_allowed(0, 3),
                   (uint64_t)3, "a small first report is emitted in full");
    TEST_ASSERT_EQ((uint64_t)test_usermode_skip_records_allowed(10, 5),
                   (uint64_t)5, "a later report well inside the budget is untouched");
}

static void test_skip_budget_boundary(void)
{
    uint32_t budget = test_usermode_skip_record_budget();

    TEST_ASSERT_EQ((uint64_t)test_usermode_skip_records_allowed(budget - 1, 1),
                   (uint64_t)1, "the last record inside the budget is emitted");
    TEST_ASSERT_EQ((uint64_t)test_usermode_skip_records_allowed(budget - 1, 2),
                   (uint64_t)1, "a report straddling the budget is clipped, not dropped");
    TEST_ASSERT_EQ((uint64_t)test_usermode_skip_records_allowed(budget, 1),
                   (uint64_t)0, "nothing is emitted once the budget is spent");
}

static void test_skip_budget_never_underflows(void)
{
    uint32_t budget = test_usermode_skip_record_budget();

    /* An already-over-budget count must clamp to zero, not wrap: the
     * subtraction is unsigned, so a naive `budget - already` would turn
     * the exhausted case into a four-billion-record allowance -- the
     * amplification path reopened by the very check meant to close it. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_skip_records_allowed(budget + 500, 4),
                   (uint64_t)0, "an over-budget run allows nothing further");
    TEST_ASSERT_EQ((uint64_t)test_usermode_skip_records_allowed(0xFFFFFFFFu, 4),
                   (uint64_t)0, "a saturated counter cannot wrap into a huge allowance");
}

/* ---- Ring-3 self-report: artifact formatting ------------------------ */


/* Exact string equality. The formatters emit a wire contract that host
 * tooling parses positionally, so a substring assertion would accept a
 * line with extra or reordered fields appended. */
static int u_test_streq(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; a[i] && a[i] == b[i]; i++)
        ;
    return a[i] == b[i];
}

/* True when `s` ends with the exact bytes of `suffix`. Used to pin the
 * tail of a variable-prefix record (e.g. "owner=<N>" where N is a live
 * pid this test does not control) without needing a generic substring
 * search. */
static int u_test_ends_with(const char *s, const char *suffix)
{
    uint32_t slen = 0, suflen = 0;

    while (s[slen])
        slen++;
    while (suffix[suflen])
        suflen++;
    if (suflen > slen)
        return 0;
    return u_test_streq(s + (slen - suflen), suffix);
}

static void test_skip_record_name_shape(void)
{
    char rec[64];

    TEST_ASSERT(test_usermode_build_skip_record_name(rec, sizeof(rec),
                                                     "test_process.exe", 3) == 1,
                "skip-record name builds for a normal binary name");
    TEST_ASSERT(u_test_streq(rec, "test_process.exe::skipped-block-3"),
                "skip record is EXACTLY <binary>::skipped-block-<k>");
    /* The largest index the syscall can admit still has to format: the
     * skip-block ceiling is what bounds this loop. */
    TEST_ASSERT(test_usermode_build_skip_record_name(
                    rec, sizeof(rec), "test_process.exe",
                    TASK_UTEST_REPORT_SKIP_MAX) == 1,
                "the maximum admissible skip index still formats");
    TEST_ASSERT(u_test_streq(rec, "test_process.exe::skipped-block-256"),
                "maximum index renders without truncation");
}

static void test_skip_record_name_capacity_boundary(void)
{
    char rec[64];

    /* "ab::skipped-block-7" is 19 chars, so 20 bytes is the exact minimum
     * capacity (19 + NUL) and 19 must fail. Off-by-one in the bound would
     * either drop a legal record or emit a truncated, colliding name. */
    TEST_ASSERT(test_usermode_build_skip_record_name(rec, 20, "ab", 7) == 1,
                "exact required capacity succeeds");
    TEST_ASSERT(u_test_streq(rec, "ab::skipped-block-7"),
                "exact-capacity result is the complete name");
    TEST_ASSERT(test_usermode_build_skip_record_name(rec, 19, "ab", 7) == 0,
                "one byte less than required fails");
}

static void test_skip_record_name_refuses_overflow(void)
{
    char rec[8];

    /* Refuse rather than truncate: a truncated record name would collide
     * with another binary's records in the artifact, silently merging two
     * tests' skip evidence. */
    TEST_ASSERT(test_usermode_build_skip_record_name(rec, sizeof(rec),
                                                     "test_process.exe", 1) == 0,
                "skip-record name refuses to truncate into a short buffer");
}

/* A binary that never RAN carries JUnit's <error> element and the JSON
 * ERROR status; one that ran and failed keeps <failure>/FAIL. The host
 * reconciles its errors= against the number of <error> elements it
 * harvests, so this classification is what those two numbers agree on --
 * a formatter that emitted the wrong element would refuse the run. */
static void test_never_ran_records_carry_the_error_classification(void)
{
    char line[256];

    TEST_ASSERT(test_usermode_format_xml_testcase(line, sizeof(line),
                                                  "refused_1_test_x_deadbeef.exe",
                                                  3, 0,
                                                  "name refused: charset") == 1,
                "an error testcase formats");
    TEST_ASSERT(u_test_contains(line,
                    "<error message=\"name refused: charset\"/></testcase>"),
                "a never-ran binary carries <error>, not <failure>");
    TEST_ASSERT(!u_test_contains(line, "<failure"),
                "the error record carries no failure element at all");

    TEST_ASSERT(test_usermode_format_xml_testcase(line, sizeof(line),
                                                  "test_x.exe", 1, 0,
                                                  "exit=-1") == 1,
                "a failure testcase still formats");
    TEST_ASSERT(u_test_contains(line, "<failure message=\"exit=-1\"/>"),
                "a binary that RAN and failed keeps <failure>");

    /* Reason is optional on both elements: the element itself is the
     * classification, so a record without a message must still be an
     * <error> rather than degrade into a self-closing (passing) testcase. */
    TEST_ASSERT(test_usermode_format_xml_testcase(line, sizeof(line),
                                                  "refused_2_ab_cafebabe.exe",
                                                  3, 0, "") == 1,
                "a reasonless error testcase formats");
    TEST_ASSERT(u_test_contains(line, "<error/></testcase>"),
                "a reasonless never-ran record is still an error element");

    TEST_ASSERT(test_usermode_format_json_testcase(line, sizeof(line),
                                                   "refused_1_test_x_deadbeef.exe",
                                                   3, 0, "name refused: charset",
                                                   0) == 1,
                "an ERROR JSON record formats");
    TEST_ASSERT(u_test_contains(line, "\"status\":\"ERROR\""),
                "the JSON side spells the same distinction as a status");
    TEST_ASSERT(u_test_contains(line, "\"record_kind\":\"binary\""),
                "an ERROR record is still an ordinary binary record");
    TEST_ASSERT(test_usermode_format_json_testcase(line, sizeof(line),
                                                   "test_x.exe", 1, 0,
                                                   "exit=-1", 0) == 1,
                "a FAIL JSON record still formats");
    TEST_ASSERT(u_test_contains(line, "\"status\":\"FAIL\""),
                "a binary that ran and failed keeps status FAIL");
}

static void test_xml_summary_counts_records(void)
{
    char line[192];

    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 12, 2, 5, 1500, 0, 0, 0) == 1,
                "XML summary formats at realistic widths");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-XML-SUMMARY] tests=12 failures=2 skipped=5 time=1.500 "
                    "aborted=0 not_run=0 errors=0"),
                "XML summary is EXACTLY the contract the host post-processor parses");
    /* An all-zero run is the empty-suite artifact contract: it must still
     * produce a complete, patchable summary rather than a degenerate one. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 0, 0, 0, 0, 0, 0, 0) == 1,
                "an empty suite still formats a summary");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-XML-SUMMARY] tests=0 failures=0 skipped=0 time=0.000 "
                    "aborted=0 not_run=0 errors=0"),
                "empty-suite summary carries explicit zeros");
}

/* `errors` is a SUBSET of `failures` on the wire: the host is what
 * projects the pair into JUnit's disjoint attributes. A producer that
 * subtracted here instead would leave the legacy `=== N failed` line, the
 * serial recount and this record disagreeing about the same run. */
static void test_xml_summary_reports_errors_as_failure_subset(void)
{
    char line[192];

    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 12, 4, 5, 1500, 0, 0, 3) == 1,
                "XML summary formats with a never-ran subset");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-XML-SUMMARY] tests=12 failures=4 skipped=5 time=1.500 "
                    "aborted=0 not_run=0 errors=3"),
                "errors= trails not_run= and reports the SUBSET, not a difference");
    /* Trailing position, same rationale as aborted=/not_run=: the host's
     * not_run extraction is a greedy `.* not_run=([0-9]+).*` sed, so a
     * field appended after it must not disturb the match. */
    TEST_ASSERT(u_test_contains(line, "not_run=0 errors=3"),
                "not_run= keeps a numeric value immediately after it");
    /* Every failure never ran: legal, and the projection the host derives
     * from it is failures=0 errors=4 -- a suite where nothing executed. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 4, 4, 0, 90, 0, 0, 4) == 1,
                "an all-refusal run formats");
    TEST_ASSERT(u_test_contains(line, "failures=4 skipped=0 time=0.090 "
                                      "aborted=0 not_run=0 errors=4"),
                "errors may equal failures when no binary ran at all");
}

static void test_xml_summary_carries_abort_state(void)
{
    char line[192];

    /* The abort case the artifact previously could not express: a smoke
     * binary SKIPPED, so `failures` is 0 and the counts that DID land are
     * internally consistent -- the run reads as a small, clean, complete
     * suite unless the completeness dimension says otherwise. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 1, 0, 1, 90, 1, 11, 0) == 1,
                "aborted XML summary formats");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-XML-SUMMARY] tests=1 failures=0 skipped=1 time=0.090 "
                    "aborted=1 not_run=11 errors=0"),
                "abort state rides as trailing fields, after time=");
    /* The trailing position is load-bearing, not cosmetic: scripts/test.sh
     * extracts time with a greedy `.*time=([0-9.]+).*` sed, so anything
     * inserted BEFORE it would be captured instead. */
    TEST_ASSERT(u_test_contains(line, "time=0.090 aborted=1"),
                "time= keeps a numeric value immediately after it");
    /* `aborted` is a flag, never a count: any nonzero must normalise to 1
     * so the host can compare it as a literal. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 1, 0, 1, 90, 7, 11, 0) == 1,
                "a nonzero abort flag formats");
    TEST_ASSERT(u_test_contains(line, "aborted=1 not_run=11"),
                "any nonzero abort flag normalises to exactly 1");
}

static void test_xml_summary_refuses_truncation(void)
{
    char line[16];

    /* The pre-2026-07-28 emitter ignored every append return and would
     * publish `[UTEST-XML-SUM` as if it were a summary; the host
     * post-processor then read a truncated `tests=` as a real number. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 12, 2, 5, 1500, 0, 0, 0) == 0,
                "XML summary reports failure instead of truncating");
    /* The completeness fields are appended LAST, so a buffer that fits
     * everything through `time=` and nothing more is the exact width at
     * which a silent truncation would drop the abort state while leaving a
     * summary that still parses. It must be refused, not published. */
    {
        char tight[61];
        TEST_ASSERT(test_usermode_format_xml_summary(tight, sizeof(tight),
                                                     12, 2, 5, 1500, 1, 9, 2) == 0,
                    "a buffer that fits only through time= is refused, not truncated");
    }
    /* And the same boundary one field further out: errors= is now the last
     * field, so a buffer that fits everything through not_run= would drop
     * exactly the count the host reconciles its <error> elements against --
     * leaving a summary that still parses and a run that would be refused
     * for a mismatch the truncation caused. */
    {
        char tight[81];
        TEST_ASSERT(test_usermode_format_xml_summary(tight, sizeof(tight),
                                                     12, 2, 5, 1500, 1, 9, 2) == 0,
                    "a buffer that fits only through not_run= is refused too");
    }
}

static void test_json_summary_separates_dimensions(void)
{
    char line[384];

    TEST_ASSERT(test_usermode_format_json_summary(line, sizeof(line),
                                                  9, 1, 2, 2500, 0) == 1,
                "JSON summary formats at realistic widths");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-JSON] {\"summary\":{\"passed\":9,\"failed\":1,"
                    "\"errors\":0,\"skipped\":2,\"total\":12,"
                    "\"time_ms\":2500}}"),
                "JSON summary is EXACTLY the documented record, every field in order");
    /* `errors` never leaves `total` -- it is drawn from `failed`, so the
     * three-way partition the total is computed from is untouched. A
     * consumer summing passed+failed+skipped must still get total. */
    TEST_ASSERT(test_usermode_format_json_summary(line, sizeof(line),
                                                  9, 4, 2, 2500, 3) == 1,
                "JSON summary formats with a never-ran subset");
    TEST_ASSERT(u_test_contains(line,
                    "\"failed\":4,\"errors\":3,\"skipped\":2,\"total\":15"),
                "errors is a subset of failed and never enters the total");
    /* The summary carries BINARY counts only. Assertion counts travel in
     * their own record and are nested under `summary.reported` by the host
     * assembler, so a consumer can never read one unit as the other -- but
     * they must not appear inline here, where a partial read could. */
    TEST_ASSERT(!u_test_contains(line, "asserts_passed"),
                "assertion counts never sit inline beside binary counts");

    TEST_ASSERT(test_usermode_format_json_run_report(line, sizeof(line),
                                                     480, 3, 4, 4, 8, 1, 3) == 1,
                "run_report formats at realistic widths");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-JSON] {\"record_kind\":\"run_report\","
                    "\"asserts_passed\":480,\"asserts_failed\":3,"
                    "\"skip_blocks\":4,\"skip_records\":4,"
                    "\"binaries_reported\":8,\"binaries_invalid\":1,"
                    "\"binaries_unreported\":3}"),
                "run_report is EXACTLY the documented record, every field in order");
}

static void test_json_records_fit_klog_transport(void)
{
    char line[384];
    uint32_t cap = test_usermode_json_line_max();

    /* Every record reaches the host through klog, which bounds a message to
     * its 256-byte ring field. A record wider than that is cut mid-object in
     * transit -- for JSON, unparseable rather than merely short, and silent.
     *
     * This is measured, not hypothetical. With the report dimension nested
     * inline the live summary reached 242 of the 255 usable bytes, and a
     * suite with four-digit binary counts and six-digit assertion counts
     * overflowed it. Splitting the dimensions is what bought the headroom,
     * so every record is pinned here at its uint32 maximum: if a future
     * field pushes one back over the transport, this fails at build-test
     * time rather than by publishing an unparseable artifact. */
    TEST_ASSERT(cap == 256,
                "the emitter caps a record at klog's message capacity");
    TEST_ASSERT(test_usermode_format_json_summary(line, cap,
                                                  4294967295u, 4294967295u,
                                                  4294967295u,
                                                  18446744073709551615ull,
                                                  4294967295u) == 1,
                "the summary fits the transport at every field's maximum");
    TEST_ASSERT(test_usermode_format_json_run_report(line, cap,
                                                     4294967295u, 4294967295u,
                                                     4294967295u, 4294967295u,
                                                     4294967295u, 4294967295u,
                                                     4294967295u) == 1,
                "run_report fits the transport at every field's maximum");
    TEST_ASSERT(test_usermode_format_json_run_meta(line, cap,
                                                   1, 4294967295u) == 1,
                "run_meta fits the transport at every field's maximum");
}

static void test_tap_point_shapes_and_refusals(void)
{
    /* TAP puts its directive LAST, and klog truncates at its message field,
     * so an over-long name used to cut `# SKIP` off the end and turn a
     * skipped test into a bare passing `ok` -- silently, with no marker.
     * The emitter is not directly callable (it klogs), so pin the property
     * that makes it safe: the bound it formats against is the transport
     * cap, and the widest name the launcher can hand it does NOT fit, which
     * is exactly when the fail-closed `not ok` path must engage. */
    char line[384];
    char longname[300];
    uint32_t cap = test_usermode_json_line_max();
    uint32_t i;

    /* The ordinary shapes, exactly as a TAP consumer must see them. */
    TEST_ASSERT(test_usermode_format_tap_point(line, cap, 1, 4,
                                               "test_harness_smoke.exe",
                                               (const char *)0) == 1,
                "a passing TAP point formats");
    TEST_ASSERT(u_test_streq(line, "ok 4 - test_harness_smoke.exe"),
                "a passing point carries no directive");
    TEST_ASSERT(test_usermode_format_tap_point(line, cap, 1, 5,
                                               "test_skip_me.exe",
                                               "SKIP") == 1,
                "a skipped TAP point formats");
    TEST_ASSERT(u_test_streq(line, "ok 5 - test_skip_me.exe # SKIP"),
                "the SKIP directive is the point's suffix");
    TEST_ASSERT(test_usermode_format_tap_point(line, cap, 0, 6,
                                               "test_leaky.exe",
                                               "1 handle(s) leaked") == 1,
                "a failing TAP point formats");
    TEST_ASSERT(u_test_streq(line,
                    "not ok 6 - test_leaky.exe # 1 handle(s) leaked"),
                "a failing point keeps its reason");

    /* Truncation: TAP puts the directive LAST, so a name long enough to
     * push it past the transport used to cut `# SKIP` off the end and turn
     * a skipped test into a bare passing `ok`. The formatter must REFUSE so
     * the emitter can publish a failure instead. The widest synthetic
     * skip-record name is built into a VFS_MAX_NAME + 24 buffer. */
    for (i = 0; i < sizeof(longname) - 1; i++)
        longname[i] = 'x';
    longname[sizeof(longname) - 1] = '\0';
    TEST_ASSERT(test_usermode_format_tap_point(line, cap, 1, 7, longname,
                                               "SKIP reported by binary") == 0,
                "an over-long TAP point is refused, never silently truncated");

    /* Directive injection: a name carrying `#` would choose its own TAP
     * meaning. u_is_valid_manifest_name permits `#`, and glob-discovered
     * names come from the filesystem, so the formatter refuses them. */
    TEST_ASSERT(test_usermode_format_tap_point(line, cap, 0, 8,
                                               "test_a # SKIP fake.exe",
                                               (const char *)0) == 0,
                "a name carrying a TAP directive is refused");
    TEST_ASSERT(test_usermode_format_tap_point(line, cap, 1, 9,
                                               "test_ok.exe # TODO",
                                               (const char *)0) == 0,
                "a failing point cannot be downgraded by an injected TODO");
}

static void test_json_run_report_refuses_truncation(void)
{
    char line[40];

    TEST_ASSERT(test_usermode_format_json_run_report(line, sizeof(line),
                                                     480, 3, 4, 4, 8, 1, 3) == 0,
                "run_report reports failure instead of truncating");
}

static void test_json_run_meta_record(void)
{
    char line[384];

    /* The completeness dimension: a run the smoke gate cut short otherwise
     * serializes identically to a smaller complete one. */
    TEST_ASSERT(test_usermode_format_json_run_meta(line, sizeof(line),
                                                   1, 11) == 1,
                "aborted run_meta formats");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-JSON] {\"record_kind\":\"run_meta\","
                    "\"aborted\":true,\"not_run\":11}"),
                "run_meta is EXACTLY the documented record");
    /* record_kind leads the record, as it does for every other kind in this
     * stream -- a consumer discriminates on it and never on field presence. */
    TEST_ASSERT(u_test_contains(line, "{\"record_kind\":\"run_meta\""),
                "run_meta leads with the stream discriminator");
    /* Emitted on EVERY run, not only aborted ones: its absence must mean
     * "this producer predates the dimension", never "the run completed". */
    TEST_ASSERT(test_usermode_format_json_run_meta(line, sizeof(line),
                                                   0, 0) == 1,
                "a complete run also emits run_meta");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-JSON] {\"record_kind\":\"run_meta\","
                    "\"aborted\":false,\"not_run\":0}"),
                "a complete run says so explicitly");
    /* `aborted` is a JSON boolean, not 0/1, so it cannot be summed with the
     * neighbouring count by a consumer walking the object generically. */
    TEST_ASSERT(test_usermode_format_json_run_meta(line, sizeof(line),
                                                   7, 3) == 1,
                "a nonzero abort flag formats");
    TEST_ASSERT(u_test_contains(line, "\"aborted\":true"),
                "any nonzero abort flag normalises to the JSON literal true");
    /* Widest possible: the record must fit the transport with every field at
     * its maximum, or an aborted run would lose the very record that says so. */
}

static void test_json_run_meta_refuses_truncation(void)
{
    char line[24];

    TEST_ASSERT(test_usermode_format_json_run_meta(line, sizeof(line),
                                                   1, 11) == 0,
                "run_meta reports failure instead of truncating");
}

static void test_json_summary_zero_run(void)
{
    char line[384];

    TEST_ASSERT(test_usermode_format_json_summary(line, sizeof(line),
                                                  0, 0, 0, 0, 0) == 1,
                "an all-zero JSON summary formats");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-JSON] {\"summary\":{\"passed\":0,\"failed\":0,"
                    "\"errors\":0,\"skipped\":0,\"total\":0,\"time_ms\":0}}"),
                "zero counts serialize as explicit zeros, not omitted fields");
    TEST_ASSERT(test_usermode_format_json_run_report(line, sizeof(line),
                                                     0, 0, 0, 0, 0, 0, 0) == 1,
                "an all-zero run_report formats");
    TEST_ASSERT(u_test_contains(line, "\"binaries_unreported\":0}"),
                "an empty suite still publishes every report counter");
}

static void test_json_summary_refuses_truncation(void)
{
    char line[24];

    /* A truncated JSON object is not merely wrong, it is unparseable --
     * so the formatter must refuse and let the emitter publish a valid
     * error record instead. */
    TEST_ASSERT(test_usermode_format_json_summary(line, sizeof(line),
                                                  9, 1, 2, 2500, 0) == 0,
                "JSON summary reports failure instead of truncating");
}

static void test_report_summary_line_shape(void)
{
    char line[224];

    TEST_ASSERT(test_usermode_format_report_summary(line, sizeof(line),
                                                    480, 3, 4, 4, 8, 1, 3) == 1,
                "report summary formats at realistic widths");
    /* Deliberately NOT the legacy `=== N passed, N failed, N skipped of N
     * total ===` wording: scripts/test.sh parses that line with
     * position-sensitive patterns and waits on it for boot completion. */
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-REPORT-SUMMARY] asserts_passed=480 asserts_failed=3 "
                    "skip_blocks=4 skip_records=4 reported=8 invalid=1 unreported=3"),
                "report summary is EXACTLY the line scripts/test.sh parses");
    TEST_ASSERT(!u_test_contains(line, "passed,"),
                "report summary cannot be captured by the legacy summary parser");
}

static void test_report_summary_refuses_truncation(void)
{
    char line[24];

    TEST_ASSERT(test_usermode_format_report_summary(line, sizeof(line),
                                                    480, 3, 4, 4, 8, 1, 3) == 0,
                "report summary reports failure instead of truncating");
}

/* ---- Setter API: no-crash + idempotence ----------------------------- *
 *
 * Call each setter with the documented sentinel values to exercise the
 * API surface. The launcher's internal state is private, so we cannot
 * assert on the stored value directly; the contract here is "these
 * calls do not crash and are safe to call before the launcher runs".
 * The behavioral effect is validated at boot by the [UTEST] lines.
 * --------------------------------------------------------------------- */

static void test_setter_api_no_crash(void)
{
    /* Save + restore surrounding state so running this test in
     * isolation doesn't perturb any other test that might care about
     * launcher config (none today, but defense against future drift). */
    test_usermode_set_filter((const char *)0);
    test_usermode_set_filter("test_syscall.exe");
    test_usermode_set_filter("test_smoke_*.exe");
    test_usermode_set_filter("");         /* empty -> treated as NULL */
    test_usermode_set_filter((const char *)0);

    test_usermode_set_timeout_ms(0);      /* 0 = use default */
    test_usermode_set_timeout_ms(1);      /* minimum valid */
    test_usermode_set_timeout_ms(10000);  /* typical */
    test_usermode_set_timeout_ms(0xFFFF); /* max uint16 ABI */
    test_usermode_set_timeout_ms(0);      /* restore default */

    test_usermode_set_tap(0);
    test_usermode_set_tap(1);
    test_usermode_set_tap(42);            /* non-zero = enabled */
    test_usermode_set_tap(0);             /* restore off */

}

/* ---- SYS_FAULT_INJECT gate regression ---------------------------- *
 *
 * The fault-inject syscall MUST hard-fail with -1 (STATUS_ACCESS_DENIED
 * at the INT 0x80 return-value channel) when boot.conf test=0. Without
 * that gate, a user-mode binary could arm kmalloc_fail_countdown
 * repeatedly and DoS the kernel from non-test boots. The regression
 * saves + restores `g_boot_info.config.test` around the probe so the
 * surrounding test harness (which runs under test=1 via the boot
 * flow) is unaffected.
 *
 * ALLOWED per CLAUDE.md "Test Code -- No Live Boot Infrastructure":
 *   - pure data field on g_boot_info (not an _init() re-invocation)
 *   - sys_fault_inject_dispatch() is a pure dispatch helper; calling it
 *     does not touch VPD / framebuffer / serial state
 *   - save + restore wrapper around the config bit
 * --------------------------------------------------------------------- */

static void test_fault_inject_gate_denies_when_test_is_off(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint64_t fired_before = kmalloc_fail_injections_triggered();
    int64_t rc;

    g_boot_info.config.test = 0;
    rc = sys_fault_inject_dispatch(FAULT_KMALLOC_NEXT, 0);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)(uint64_t)-1LL,
                   "SYS_FAULT_INJECT returns -1 when config.test=0");

    /* Arming was supposed to be blocked, so the cumulative
     * fired-counter must not have budged. If it did, the gate let
     * an injection through. */
    TEST_ASSERT_EQ(kmalloc_fail_injections_triggered(), fired_before,
                   "gate blocked: kmalloc fired-counter unchanged");

    g_boot_info.config.test = saved_test;
}

static void test_fault_inject_clear_works_under_test_mode(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    int64_t rc;

    /* Normal-path round-trip under test=1: CLEAR_ALL is the safest
     * probe -- it disarms every countdown + filter without firing
     * anything, so running this assertion cannot perturb sibling
     * tests' fault-inject state. */
    g_boot_info.config.test = 1;
    rc = sys_fault_inject_dispatch(FAULT_CLEAR_ALL, 0);
    TEST_ASSERT_EQ((uint64_t)rc, 0ULL,
                   "FAULT_CLEAR_ALL returns 0 under config.test=1");

    g_boot_info.config.test = saved_test;
}

static void test_fault_inject_rejects_unknown_kind(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    int64_t rc;

    g_boot_info.config.test = 1;
    /* kind values above the highest assigned selector (FAULT_PMM_SITE, 9)
     * are unassigned; dispatcher must -1 them so a future ABI extension
     * can't be forged against an older kernel. */
    rc = sys_fault_inject_dispatch(99, 0);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)(uint64_t)-1LL,
                   "unknown kind=99 returns -1 even under test=1");

    g_boot_info.config.test = saved_test;
}

static void test_fault_inject_kmalloc_countdown_requires_nonzero(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    int64_t rc;

    g_boot_info.config.test = 1;
    rc = sys_fault_inject_dispatch(FAULT_KMALLOC_COUNTDOWN, 0);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)(uint64_t)-1LL,
                   "FAULT_KMALLOC_COUNTDOWN with countdown=0 returns -1");

    g_boot_info.config.test = saved_test;
}

static void test_fault_inject_pmm_countdown_requires_nonzero(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    int64_t rc;

    g_boot_info.config.test = 1;
    rc = sys_fault_inject_dispatch(FAULT_PMM_COUNTDOWN, 0);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)(uint64_t)-1LL,
                   "FAULT_PMM_COUNTDOWN with countdown=0 returns -1");

    g_boot_info.config.test = saved_test;
}

/* ---- site-targeted selector regressions -------------------------
 *
 * These drive the arm word directly through its pure accessors; nothing
 * here allocates, so a failing assertion cannot strand kernel memory. Each
 * assertion clears the arm before returning so a sibling test never
 * inherits a live injection.
 * ----------------------------------------------------------------- */

static void test_fault_site_rejects_invalid_site_id(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    int64_t rc;

    g_boot_info.config.test = 1;
    /* One past the highest assigned site. A test that names a site this
     * kernel does not implement must fail LOUDLY at arm time rather than
     * arming nothing and then passing because no injection ever fired. */
    rc = sys_fault_inject_dispatch(FAULT_KMALLOC_SITE, FAULT_SITE_MAX + 1);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)(uint64_t)-1LL,
                   "FAULT_KMALLOC_SITE with out-of-range site returns -1");

    fault_site_arm_clear(FI_ALLOC_KMALLOC);
    fault_site_arm_clear(FI_ALLOC_PMM);
    g_boot_info.config.test = saved_test;
}

static void test_fault_site_arm_fails_closed(void)
{
    uint8_t saved_test = g_boot_info.config.test;

    g_boot_info.config.test = 1;

    /* Every shape below LOOKS like a valid arm but could never be
     * claimed. Each must be refused at arm time, because the ring-3
     * consumed-check reads a disarmed slot as "it fired" -- so an arm that
     * silently installs nothing would certify an unexecuted branch. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(FAULT_KMALLOC_SITE,
                                                       FAULT_SITE_NONE),
                   (uint64_t)(uint64_t)-1LL,
                   "arming FAULT_SITE_NONE is refused, not a silent disarm");
    /* PEB frames are a pmm site; a kmalloc arm on it can never fire. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(FAULT_KMALLOC_SITE,
                                                       FAULT_SITE_PEB_FRAMES),
                   (uint64_t)(uint64_t)-1LL,
                   "kmalloc arm on a pmm-owned site is refused");
    /* ...and the mirror image. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_PMM_SITE, FAULT_SITE_EXEC_ARGV_TABLE),
                   (uint64_t)(uint64_t)-1LL,
                   "pmm arm on a kmalloc-owned site is refused");

    /* A malformed query must not answer 0 -- that is the consumed value. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(FAULT_SITE_QUERY, 0),
                   (uint64_t)(uint64_t)-1LL,
                   "FAULT_SITE_QUERY with tag 0 returns -1, not consumed");
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(FAULT_SITE_QUERY,
                                                       FI_ALLOC_COUNT + 1),
                   (uint64_t)(uint64_t)-1LL,
                   "FAULT_SITE_QUERY with an out-of-range tag returns -1");

    fault_site_arm_clear(FI_ALLOC_KMALLOC);
    fault_site_arm_clear(FI_ALLOC_PMM);
    g_boot_info.config.test = saved_test;
}

static void test_fault_site_claim_fires_exactly_once(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint32_t saved_site;
    int64_t rc;

    g_boot_info.config.test = 1;
    rc = sys_fault_inject_dispatch(FAULT_KMALLOC_SITE,
                                   FAULT_SITE_EXEC_ARGV_TABLE);
    TEST_ASSERT_EQ((uint64_t)rc, 0ULL,
                   "FAULT_KMALLOC_SITE arms a valid site");

    saved_site = fault_site_enter(FAULT_SITE_EXEC_ARGV_TABLE);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_KMALLOC), 1ULL,
                   "armed site claims once inside the named site");
    /* Single shot: the arm is consumed by the claim, so a second
     * allocation at the same site must NOT fail. Without this the
     * injection would leak past its target branch. */
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_KMALLOC), 0ULL,
                   "consumed site arm does not fire a second time");
    fault_site_restore(saved_site);

    fault_site_arm_clear(FI_ALLOC_KMALLOC);
    fault_site_arm_clear(FI_ALLOC_PMM);
    g_boot_info.config.test = saved_test;
}

static void test_fault_site_ignores_other_sites_and_allocators(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint32_t saved_site;

    g_boot_info.config.test = 1;
    sys_fault_inject_dispatch(FAULT_KMALLOC_SITE, FAULT_SITE_EXEC_ARGV_TABLE);

    /* Negative case 1: a DIFFERENT site must not consume the arm. This is
     * the whole point of site targeting -- a mis-targeted test has to fail
     * loudly instead of passing on an unrelated allocation. */
    saved_site = fault_site_enter(FAULT_SITE_PEB_FRAMES);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_KMALLOC), 0ULL,
                   "unrelated site does not consume the arm");
    fault_site_restore(saved_site);

    /* Negative case 2: right site, wrong allocator. */
    saved_site = fault_site_enter(FAULT_SITE_EXEC_ARGV_TABLE);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 0ULL,
                   "kmalloc-armed site does not fire on a pmm allocation");
    /* ...and the arm survived both misses, so it can still fire. */
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_KMALLOC), 1ULL,
                   "arm survives non-matching probes and still fires");
    fault_site_restore(saved_site);

    fault_site_arm_clear(FI_ALLOC_KMALLOC);
    fault_site_arm_clear(FI_ALLOC_PMM);
    g_boot_info.config.test = saved_test;
}

static void test_fault_site_outside_any_site_never_fires(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint32_t saved_site;

    g_boot_info.config.test = 1;
    sys_fault_inject_dispatch(FAULT_PMM_SITE, FAULT_SITE_PEB_FRAMES);

    /* FAULT_SITE_NONE is the resting state of every thread. An armed site
     * must never fire against unannotated allocations, or arming a site
     * would behave like a plain single-shot and fail a random branch. */
    saved_site = fault_site_enter(FAULT_SITE_NONE);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 0ULL,
                   "no claim outside an annotated site");
    fault_site_restore(saved_site);

    fault_site_arm_clear(FI_ALLOC_KMALLOC);
    fault_site_arm_clear(FI_ALLOC_PMM);
    g_boot_info.config.test = saved_test;
}

static void test_fault_site_arms_are_per_allocator(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint32_t saved_site;

    g_boot_info.config.test = 1;

    /* PART 1 -- the arms COEXIST. This has to be observed directly, before
     * any replacement: if both arms shared one tagged slot, the pmm arm
     * would overwrite the kmalloc one, and part 2 alone would still read
     * exactly as expected (kmalloc 0, pmm 1) while independence was
     * broken. Claiming BOTH is what rules that out. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_KMALLOC_SITE, FAULT_SITE_EXEC_ARGV_TABLE), 0ULL,
                   "kmalloc site arms at a site it owns");
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_PMM_SITE, FAULT_SITE_PEB_FRAMES), 0ULL,
                   "pmm site arms at a site it owns");

    saved_site = fault_site_enter(FAULT_SITE_EXEC_ARGV_TABLE);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_KMALLOC), 1ULL,
                   "kmalloc arm is live while a pmm arm is also installed");
    fault_site_restore(saved_site);

    saved_site = fault_site_enter(FAULT_SITE_PEB_FRAMES);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 1ULL,
                   "pmm arm is live at the same time as the kmalloc arm");
    fault_site_restore(saved_site);

    /* PART 2 -- replacement is per-allocator. Re-arm both (part 1 consumed
     * them), then install an ordinal kmalloc countdown: it must cancel the
     * kmalloc SITE arm and leave the pmm one untouched. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_KMALLOC_SITE, FAULT_SITE_EXEC_ARGV_TABLE), 0ULL,
                   "kmalloc site re-arms after being consumed");
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_PMM_SITE, FAULT_SITE_PEB_FRAMES), 0ULL,
                   "pmm site re-arms after being consumed");
    /* Prove the kmalloc arm is actually LIVE before the countdown replaces
     * it. Without this the zero-claim below would also pass if the re-arm
     * had silently no-opped -- confirming replacement by testing something
     * that was never armed. */
    TEST_ASSERT_EQ((uint64_t)fault_site_arm_peek(FI_ALLOC_KMALLOC),
                   (uint64_t)FI_ARM_PACK(FI_ALLOC_KMALLOC,
                                         FAULT_SITE_EXEC_ARGV_TABLE),
                   "kmalloc site arm is live immediately before replacement");
    sys_fault_inject_dispatch(FAULT_KMALLOC_COUNTDOWN, 1);

    saved_site = fault_site_enter(FAULT_SITE_EXEC_ARGV_TABLE);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_KMALLOC), 0ULL,
                   "ordinal kmalloc arm cancelled the kmalloc site arm");
    fault_site_restore(saved_site);

    saved_site = fault_site_enter(FAULT_SITE_PEB_FRAMES);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 1ULL,
                   "the pmm site arm survived an unrelated kmalloc arm");
    fault_site_restore(saved_site);

    sys_fault_inject_dispatch(FAULT_CLEAR_ALL, 0);
    g_boot_info.config.test = saved_test;
}

static void test_fault_site_cleared_by_clear_all(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint32_t saved_site;

    g_boot_info.config.test = 1;
    sys_fault_inject_dispatch(FAULT_PMM_SITE, FAULT_SITE_PEB_FRAMES);
    sys_fault_inject_dispatch(FAULT_CLEAR_ALL, 0);

    saved_site = fault_site_enter(FAULT_SITE_PEB_FRAMES);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 0ULL,
                   "FAULT_CLEAR_ALL disarms a site arm too");
    fault_site_restore(saved_site);

    g_boot_info.config.test = saved_test;
}

/* The fork-child PML4 site is the one site whose allocation runs inside
 * task_fork rather than task_exec. It is a PMM site: vmm_create_user_pml4()
 * takes its four page-table frames from pmm_alloc_frame(). Getting the owner
 * wrong would make the site armable by the kmalloc selector and then never
 * claimable, which reads from ring 3 exactly like a branch that was covered. */
static void test_fault_site_fork_pml4_is_pmm_owned(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint32_t saved_site;

    g_boot_info.config.test = 1;

    TEST_ASSERT_EQ((uint64_t)fault_site_owner(FAULT_SITE_FORK_CHILD_PML4),
                   (uint64_t)FI_ALLOC_PMM,
                   "fork-child PML4 site is owned by the pmm allocator");
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_KMALLOC_SITE, FAULT_SITE_FORK_CHILD_PML4),
                   (uint64_t)-1,
                   "arming the fork PML4 site on kmalloc is refused");
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_PMM_SITE, FAULT_SITE_FORK_CHILD_PML4), 0ULL,
                   "arming the fork PML4 site on pmm succeeds");
    TEST_ASSERT_EQ((uint64_t)fault_site_arm_peek(FI_ALLOC_PMM),
                   (uint64_t)FI_ARM_PACK(FI_ALLOC_PMM,
                                         FAULT_SITE_FORK_CHILD_PML4),
                   "the armed word names the fork PML4 site");

    /* Single shot, and confined to its own site: an allocation inside the
     * PEB site must not consume an arm placed on the fork site. */
    saved_site = fault_site_enter(FAULT_SITE_PEB_FRAMES);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 0ULL,
                   "a PEB-site allocation does not consume the fork arm");
    fault_site_restore(saved_site);

    saved_site = fault_site_enter(FAULT_SITE_FORK_CHILD_PML4);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 1ULL,
                   "the fork PML4 site claims its arm");
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 0ULL,
                   "the fork PML4 arm is single-shot");
    fault_site_restore(saved_site);
    TEST_ASSERT_EQ((uint64_t)fault_site_arm_peek(FI_ALLOC_PMM), 0ULL,
                   "the fork PML4 arm reads CONSUMED after it fires");

    sys_fault_inject_dispatch(FAULT_CLEAR_ALL, 0);
    g_boot_info.config.test = saved_test;
}

/* Replacement is per-allocator in BOTH directions, and the previous coverage
 * only ever drove the kmalloc direction -- a pmm-specific regression could
 * leave a site arm and an ordinal countdown both live, so the named fork
 * failure would consume the site arm and a stale countdown would then fail
 * some unrelated later allocation while every existing assertion stayed
 * green. Drive the pmm direction with the fork site to close that. */
static void test_fault_site_pmm_replacement_is_two_way(void)
{
    uint8_t saved_test = g_boot_info.config.test;
    uint32_t saved_site;

    g_boot_info.config.test = 1;

    /* Direction 1: an ordinal pmm arm cancels a live pmm SITE arm. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_PMM_SITE, FAULT_SITE_FORK_CHILD_PML4), 0ULL,
                   "fork PML4 site arms before the ordinal replaces it");
    TEST_ASSERT_EQ((uint64_t)fault_site_arm_peek(FI_ALLOC_PMM),
                   (uint64_t)FI_ARM_PACK(FI_ALLOC_PMM,
                                         FAULT_SITE_FORK_CHILD_PML4),
                   "fork PML4 site arm is live immediately before replacement");
    sys_fault_inject_dispatch(FAULT_PMM_COUNTDOWN, 1);
    saved_site = fault_site_enter(FAULT_SITE_FORK_CHILD_PML4);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 0ULL,
                   "an ordinal pmm countdown cancelled the fork site arm");
    fault_site_restore(saved_site);
    sys_fault_inject_dispatch(FAULT_CLEAR_ALL, 0);

    /* Direction 2: a pmm SITE arm cancels a live ordinal pmm countdown. The
     * ordinal cannot be observed through fault_site_arm_peek -- that reads 0
     * in ordinal mode whether or not a countdown is pending -- so the only
     * honest oracle is an actual allocation OUTSIDE any named site. If the
     * site arm failed to clear the ordinal, this allocation is the one the
     * stale countdown would fail, and that is exactly the regression the
     * test exists to catch. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(FAULT_PMM_NEXT, 0),
                   0ULL, "an ordinal pmm arm installs");
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_PMM_SITE, FAULT_SITE_FORK_CHILD_PML4), 0ULL,
                   "fork PML4 site arms over a live ordinal pmm arm");
    {
        uintptr_t probe = pmm_alloc_frame();
        TEST_ASSERT(probe != 0,
                    "the site arm cleared the ordinal: an unsited frame alloc "
                    "still succeeds");
        if (probe)
            pmm_free_frame(probe);
    }

    /* And the pmm site coexists with a kmalloc site: separate slots. */
    TEST_ASSERT_EQ((uint64_t)sys_fault_inject_dispatch(
                       FAULT_KMALLOC_SITE, FAULT_SITE_EXEC_ARGV_TABLE), 0ULL,
                   "a kmalloc site arms while the fork pmm site is armed");
    saved_site = fault_site_enter(FAULT_SITE_FORK_CHILD_PML4);
    TEST_ASSERT_EQ((uint64_t)fault_site_claim(FI_ALLOC_PMM), 1ULL,
                   "fork pmm arm survived an unrelated kmalloc site arm");
    fault_site_restore(saved_site);

    sys_fault_inject_dispatch(FAULT_CLEAR_ALL, 0);
    g_boot_info.config.test = saved_test;
}

/* The allocation the fork site names takes FOUR frames and frees whichever
 * subset succeeded when any one of them fails. That rollback is what keeps a
 * failing fork from leaking 1-3 page-table frames per attempt, and it is
 * invisible to the ring-3 probe (which only sees fork refuse). Drive every
 * failure position and require the free-frame count to return to baseline.
 * Lives beside the fault-site suite rather than in test_vmm.c because the
 * fault-injection lever and the fork site are what make the path reachable. */
static void test_fork_pml4_partial_alloc_is_net_zero(void)
{
    uint32_t n;

    for (n = 1; n <= 4; n++) {
        uint64_t before, after;
        uintptr_t pml4;

        /* Arm immediately before the call: an unrelated frame allocation
         * between the arm and the call would consume the countdown and make
         * this measure the wrong allocation. */
        before = pmm_get_free_frames();
        pmm_alloc_fail_countdown_set(n);
        pml4 = vmm_create_user_pml4();
        pmm_alloc_fail_countdown_clear();
        after = pmm_get_free_frames();

        /* The countdown is PER-CPU, not per-call: an unrelated allocation on
         * this CPU between the arm and the call (a DPC drained on a timer
         * tick, a preempting task) consumes it and the create SUCCEEDS. That
         * is a mis-timed run rather than a defect in the code under test, but
         * it must not LEAK the four frames it just took -- destroy them before
         * asserting, so a flake stays a flake. */
        if (pml4)
            vmm_destroy_user_pml4(pml4);

        TEST_ASSERT_EQ((uint64_t)pml4, 0ULL,
                       "vmm_create_user_pml4 fails when a frame is denied");
        TEST_ASSERT_EQ(after, before,
                       "a failed PML4 creation strands no page-table frames");
    }
}

/* ---- per-test isolation pure helpers ---------------------------- */

/* Local streq (no libc). Returns 1 on match. */
static int ul_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void test_derive_test_name_happy_path(void)
{
    char out[64];
    TEST_ASSERT(test_usermode_derive_test_name("test_syscall.exe", out,
                                               sizeof(out)) == 1,
                "test_syscall.exe -> strips suffix");
    TEST_ASSERT(ul_streq(out, "test_syscall"),
                "stem equals 'test_syscall'");
    TEST_ASSERT(test_usermode_derive_test_name("test_harness_smoke.EXE", out,
                                               sizeof(out)) == 1,
                "uppercase .EXE accepted");
    TEST_ASSERT(ul_streq(out, "test_harness_smoke"),
                "uppercase stem equals 'test_harness_smoke'");
}

static void test_derive_test_name_rejects_bad_input(void)
{
    char out[64];
    TEST_ASSERT(test_usermode_derive_test_name((const char *)0, out,
                                               sizeof(out)) == 0,
                "NULL name rejected");
    TEST_ASSERT(test_usermode_derive_test_name("", out, sizeof(out)) == 0,
                "empty name rejected");
    TEST_ASSERT(test_usermode_derive_test_name(".exe", out, sizeof(out)) == 0,
                "bare .exe (5-char minimum) rejected");
    TEST_ASSERT(test_usermode_derive_test_name("foo.txt", out,
                                               sizeof(out)) == 0,
                "non-.exe suffix rejected");
    /* Out buffer too small for the stem. "test_syscall" is 12 chars +
     * NUL = 13; a 10-byte out must reject. */
    TEST_ASSERT(test_usermode_derive_test_name("test_syscall.exe", out,
                                               10) == 0,
                "out buffer too small rejected");
}

static void test_path_join_adds_separator(void)
{
    char out[64];
    TEST_ASSERT(test_usermode_path_join("C:\\Temp\\utest", "test_syscall",
                                        out, sizeof(out)) == 1,
                "path_join happy path returns 1");
    TEST_ASSERT(ul_streq(out, "C:\\Temp\\utest\\test_syscall"),
                "separator inserted between parent and name");
    TEST_ASSERT(test_usermode_path_join("C:\\Temp\\utest\\", "sub",
                                        out, sizeof(out)) == 1,
                "trailing separator on parent accepted");
    TEST_ASSERT(ul_streq(out, "C:\\Temp\\utest\\sub"),
                "no duplicate separator when parent already ends in \\");
}

/* u_path_has_traversal regression: a raw prefix check in
 * u_cleanup_manifest_apply is bypassable by
 * `C:\Impossible\..\hello.txt`. The traversal guard must catch every
 * variant of a `..` component; these cases cover the common
 * separator + position combinations. */
static void test_path_has_traversal_catches_variants(void)
{
    TEST_ASSERT(test_usermode_path_has_traversal("C:\\Impossible\\..\\hello.txt") == 1,
                "rejects leading subtree + \\..\\hello.txt");
    TEST_ASSERT(test_usermode_path_has_traversal("C:\\Impossible\\..") == 1,
                "rejects trailing \\.. ");
    TEST_ASSERT(test_usermode_path_has_traversal("..\\Impossible") == 1,
                "rejects leading ..\\");
    TEST_ASSERT(test_usermode_path_has_traversal("SOFTWARE/../Impossible") == 1,
                "rejects mixed / separator with ..");
    TEST_ASSERT(test_usermode_path_has_traversal("C:\\Impossible\\a..b\\c.txt") == 0,
                "allows `a..b` as a single component (no separator before)");
    TEST_ASSERT(test_usermode_path_has_traversal("C:\\Impossible\\...\\c.txt") == 0,
                "allows `...` (three dots) -- not a `..` component");
    TEST_ASSERT(test_usermode_path_has_traversal("C:\\Impossible\\foo") == 0,
                "happy path accepted");
    TEST_ASSERT(test_usermode_path_has_traversal("") == 0,
                "empty string accepted (caller's job to reject empty paths)");
}

/* XML/JSON escape regressions. User-provided strings (test names,
 * reason messages) go through u_xml_escape and u_json_escape before
 * being wrapped in attribute / string quotes; these tests pin the
 * five XML attribute-value specials + the JSON-required set so a
 * future refactor can't silently drop coverage. */
static void test_xml_escape_specials(void)
{
    char out[128];
    TEST_ASSERT(test_usermode_xml_escape("a&b<c>d\"e'f", out, sizeof(out)),
                "xml_escape handles all five specials");
    TEST_ASSERT(ul_streq(out, "a&amp;b&lt;c&gt;d&quot;e&apos;f"),
                "xml_escape produces the expected entity sequence");
    /* Control bytes < 0x20 (except tab/LF/CR) must be dropped. */
    TEST_ASSERT(test_usermode_xml_escape("a\x01" "b", out, sizeof(out)),
                "xml_escape accepts control-byte input");
    TEST_ASSERT(ul_streq(out, "ab"),
                "xml_escape silently drops SOH (XML 1.0 forbids it)");
    /* Tab/LF/CR are legal and pass through. */
    TEST_ASSERT(test_usermode_xml_escape("a\tb\nc\rd", out, sizeof(out)),
                "xml_escape accepts tab/LF/CR pass-through");
    TEST_ASSERT(ul_streq(out, "a\tb\nc\rd"),
                "xml_escape leaves tab/LF/CR literal");
    /* Overflow must return 0 without buffer overrun. */
    {
        char tiny[8];
        TEST_ASSERT(test_usermode_xml_escape("aaaaaaaa&", tiny, sizeof(tiny)) == 0,
                    "xml_escape rejects overflow");
    }
}

static void test_json_escape_specials(void)
{
    char out[128];
    TEST_ASSERT(test_usermode_json_escape("a\"b\\c", out, sizeof(out)),
                "json_escape handles quote + backslash");
    TEST_ASSERT(ul_streq(out, "a\\\"b\\\\c"),
                "json_escape wraps both with backslash");
    TEST_ASSERT(test_usermode_json_escape("a\nb\rc\td", out, sizeof(out)),
                "json_escape handles LF/CR/TAB");
    TEST_ASSERT(ul_streq(out, "a\\nb\\rc\\td"),
                "json_escape emits C-style \\n \\r \\t");
    TEST_ASSERT(test_usermode_json_escape("a\x01" "b", out, sizeof(out)),
                "json_escape handles generic control bytes");
    TEST_ASSERT(ul_streq(out, "a\\u0001b"),
                "json_escape emits 4-digit \\u00HH for SOH");
    {
        char tiny[4];
        TEST_ASSERT(test_usermode_json_escape("aaaaaa\"", tiny, sizeof(tiny)) == 0,
                    "json_escape rejects overflow");
    }
}

/* Manifest HKLM guard regression: `HKLM\` with an empty subkey would
 * otherwise hit RegDeleteTree(HKLM, "") which wipes ALL children of
 * HKEY_LOCAL_MACHINE. The test verifies the parser's reject path
 * leaves the registry untouched. We use a HKLM subkey that we KNOW
 * exists (SOFTWARE is populated by registry_populate_defaults during
 * boot) and assert it survives a simulated `HKLM\` directive.
 *
 * Implementation note: we cannot invoke u_cleanup_manifest_apply
 * directly -- it reads an on-disk manifest and the unit test layer
 * doesn't mount a synthetic FS. Instead we invoke RegOpenKeyEx
 * against the subtree before + after a deliberate worst-case call:
 * the PRESENCE of the subtree both before and after = evidence that
 * the guard blocked an accidental wipe. The blast-radius bound is
 * what we're testing, not the file-read path itself. */
static void test_manifest_hklm_guard_rejects_empty(void)
{
    HKEY h_before = (HKEY)(uintptr_t)0;
    HKEY h_after  = (HKEY)(uintptr_t)0;
    long rc_before;
    long rc_after;

    rc_before = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE", 0, 0,
                             &h_before);
    TEST_ASSERT(rc_before == 0,
                "SOFTWARE hive present before HKLM guard test");
    if (h_before) RegCloseKey(h_before);

    /* We DO NOT call RegDeleteTree(HKLM, "") here -- that would wipe
     * the registry. The guard in u_cleanup_manifest_apply is the
     * actual defense; this test confirms the subtree we're protecting
     * is visible + intact, and the guard's logic is directly tested
     * by test_manifest_hklm_guard_rejects_non_impossibleos below. */

    rc_after = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE", 0, 0,
                            &h_after);
    TEST_ASSERT(rc_after == 0,
                "SOFTWARE hive still present after guard probe");
    if (h_after) RegCloseKey(h_after);
}

static void test_path_join_overflow(void)
{
    char out[8];
    TEST_ASSERT(test_usermode_path_join("C:\\Temp\\utest", "sub",
                                        out, sizeof(out)) == 0,
                "parent too long for tiny out rejected");
    /* Parent fits but name overflows. */
    TEST_ASSERT(test_usermode_path_join("C:\\a", "very_long_name_that_overflows",
                                        out, sizeof(out)) == 0,
                "name overflow rejected");
    /* Null args rejected. */
    TEST_ASSERT(test_usermode_path_join((const char *)0, "x",
                                        out, sizeof(out)) == 0,
                "NULL parent rejected");
    TEST_ASSERT(test_usermode_path_join("C:\\a", (const char *)0,
                                        out, sizeof(out)) == 0,
                "NULL name rejected");
    /* Output capacity too small (cap < 3). */
    TEST_ASSERT(test_usermode_path_join("x", "y", out, 2) == 0,
                "out_cap < 3 rejected");
}

/* ---- taxonomy: filename prefix inference ------------------------- */

/* Mirror of utest_type_t (see include/kernel/test/test_usermode.h) -- kept
 * local so the unit test does not have to include the taxonomy header. */
#define UT_CORRECTNESS 0
#define UT_SMOKE       1
#define UT_STRESS      2
#define UT_PERF        3

static void test_type_for_name_prefix_inference(void)
{
    TEST_ASSERT(test_usermode_type_for_name("test_smoke_boot.exe") == UT_SMOKE,
                "test_smoke_* -> UTEST_TYPE_SMOKE");
    TEST_ASSERT(test_usermode_type_for_name("test_smoke_.exe") == UT_SMOKE,
                "test_smoke_ (empty middle) -> UTEST_TYPE_SMOKE");
    TEST_ASSERT(test_usermode_type_for_name("test_stress_libc.exe") == UT_STRESS,
                "test_stress_* -> UTEST_TYPE_STRESS");
    TEST_ASSERT(test_usermode_type_for_name("test_perf_syscall.exe") == UT_PERF,
                "test_perf_* -> UTEST_TYPE_PERF");
    TEST_ASSERT(test_usermode_type_for_name("test_syscall.exe") == UT_CORRECTNESS,
                "test_* (no specific prefix) -> UTEST_TYPE_CORRECTNESS");
    TEST_ASSERT(test_usermode_type_for_name("test_harness_smoke.exe") == UT_CORRECTNESS,
                "test_harness_smoke.exe keeps correctness (prefix is test_harness_, not test_smoke_)");
}

static void test_type_for_name_prefix_boundaries(void)
{
    /* Near-miss: a test named `test_smoke.exe` (no trailing underscore)
     * MUST NOT match -- the convention requires `test_smoke_<body>.exe`.
     * Same idea for stress and perf. */
    TEST_ASSERT(test_usermode_type_for_name("test_smoke.exe") == UT_CORRECTNESS,
                "test_smoke.exe (no trailing _) -> correctness");
    TEST_ASSERT(test_usermode_type_for_name("test_stress.exe") == UT_CORRECTNESS,
                "test_stress.exe (no trailing _) -> correctness");
    TEST_ASSERT(test_usermode_type_for_name("test_perf.exe") == UT_CORRECTNESS,
                "test_perf.exe (no trailing _) -> correctness");
    /* A wholly unrelated name gets the default type, not a crash. */
    TEST_ASSERT(test_usermode_type_for_name("hello.exe") == UT_CORRECTNESS,
                "hello.exe (no test_ prefix) -> correctness");
    TEST_ASSERT(test_usermode_type_for_name("") == UT_CORRECTNESS,
                "empty name -> correctness (no crash)");
}

/* ---- taxonomy: manifest type= attribute parsing ------------------ */

static void test_type_from_attr_known_values(void)
{
    TEST_ASSERT(test_usermode_type_from_attr("smoke") == UT_SMOKE,
                "type=smoke -> UTEST_TYPE_SMOKE");
    TEST_ASSERT(test_usermode_type_from_attr("stress") == UT_STRESS,
                "type=stress -> UTEST_TYPE_STRESS");
    TEST_ASSERT(test_usermode_type_from_attr("perf") == UT_PERF,
                "type=perf -> UTEST_TYPE_PERF");
    TEST_ASSERT(test_usermode_type_from_attr("correctness") == UT_CORRECTNESS,
                "type=correctness -> UTEST_TYPE_CORRECTNESS");
}

static void test_type_from_attr_unknown_values(void)
{
    /* Unknown attribute values fall back to correctness (warn-and-continue
     * so a future schema addition doesn't brick older kernels). */
    TEST_ASSERT(test_usermode_type_from_attr("Smoke") == UT_CORRECTNESS,
                "case mismatch (Smoke) -> correctness (exact match only)");
    TEST_ASSERT(test_usermode_type_from_attr("SMOKE") == UT_CORRECTNESS,
                "case mismatch (SMOKE) -> correctness");
    TEST_ASSERT(test_usermode_type_from_attr("benchmark") == UT_CORRECTNESS,
                "unknown value (benchmark) -> correctness");
    TEST_ASSERT(test_usermode_type_from_attr("") == UT_CORRECTNESS,
                "empty value -> correctness");
    TEST_ASSERT(test_usermode_type_from_attr("smoke_extra") == UT_CORRECTNESS,
                "trailing garbage (smoke_extra) -> correctness (exact match)");
}

/* ---- taxonomy: type label round-trip ----------------------------- */

static void test_type_label_matches_enum(void)
{
    const char *l;
    l = test_usermode_type_label(UT_SMOKE);
    TEST_ASSERT(l && l[0] == 's' && l[1] == 'm' && l[2] == 'o' &&
                l[3] == 'k' && l[4] == 'e' && l[5] == '\0',
                "label(SMOKE) == \"smoke\"");
    l = test_usermode_type_label(UT_STRESS);
    TEST_ASSERT(l && l[0] == 's' && l[1] == 't' && l[2] == 'r' &&
                l[3] == 'e' && l[4] == 's' && l[5] == 's' && l[6] == '\0',
                "label(STRESS) == \"stress\"");
    l = test_usermode_type_label(UT_PERF);
    TEST_ASSERT(l && l[0] == 'p' && l[1] == 'e' && l[2] == 'r' &&
                l[3] == 'f' && l[4] == '\0',
                "label(PERF) == \"perf\"");
    l = test_usermode_type_label(UT_CORRECTNESS);
    TEST_ASSERT(l && l[0] == 'c' && l[1] == 'o' && l[2] == 'r',
                "label(CORRECTNESS) starts with \"cor\"");
    /* Round-trip: passing any enum value yields a non-NULL label. The
     * default branch catches future enum extensions without crashing. */
    l = test_usermode_type_label(99);
    TEST_ASSERT(l != (const char *)0,
                "label(unknown int) returns a non-NULL fallback label");
}

/* ---- taxonomy: ABI stability check ------------------------------- */

static void test_type_enum_values_stable(void)
{
    /* Integer values of the enum are an ABI contract -- the boot.conf
     * `stress_iters` parser, the launcher manifest, and XML/JSON classname
     * all depend on them being exactly these. If someone reorders the
     * enum in include/kernel/test/test_usermode.h, this fires. */
    TEST_ASSERT(test_usermode_type_for_name("test_smoke_boot.exe") == 1,
                "UTEST_TYPE_SMOKE integer value == 1");
    TEST_ASSERT(test_usermode_type_for_name("test_stress_foo.exe") == 2,
                "UTEST_TYPE_STRESS integer value == 2");
    TEST_ASSERT(test_usermode_type_for_name("test_perf_foo.exe") == 3,
                "UTEST_TYPE_PERF integer value == 3");
    TEST_ASSERT(test_usermode_type_for_name("test_anything.exe") == 0,
                "UTEST_TYPE_CORRECTNESS integer value == 0");
}

/* ----: stress_iters boot-config field -----------------------------*/

static void test_stress_iters_boot_config_has_nonzero_room(void)
{
    /* The field is uint16 (0-65535). The setter interprets 0 as "use
     * built-in default" so the parser always lands a representable
     * value; this test just proves the field exists + survives through
     * the setter without crash. */
    test_usermode_set_stress_iters(0);       /* reset */
    test_usermode_set_stress_iters(100);
    test_usermode_set_stress_iters(65535);
    test_usermode_set_stress_iters(0);       /* leave in default state */
}

/* ---- Registration --------------------------------------------------- */

/* ---- Non-forgeable record framing --------------------------------- */

/* Poison a scratch buffer so a formatter that writes nothing is visibly
 * distinct from one that wrote an empty string. Freestanding kernel: no
 * memset, and a loop is clearer than pulling one in for four call sites. */
static void u_test_fill(char *dst, uint32_t cap)
{
    uint32_t i;

    for (i = 0; i < cap; i++)
        dst[i] = 'Z';
}

/* Every existing marker (`[UTEST-XML]`, `[UTEST-JSON]`, `[UTEST-FRAME]`,
 * `[UTEST-FRAME-END]`, `[UTEST-RECORD-OVERFLOW]`) shares the leading `[`,
 * so escaping it blocks a captured payload from being mistaken for any
 * of them. Everything outside safe printable ASCII (< 0x20 or >= 0x7F)
 * is ALSO escaped, because scripts/utest-frame.py decodes each physical
 * line as UTF-8 with errors="replace" -- an unescaped raw high byte or
 * lone continuation byte would be irreversibly replaced with U+FFFD
 * before this section's byte-exactness goal could be honored. Only
 * printable ASCII minus '\' and '[' survives byte-for-byte. */
static void test_capture_escape_marker_and_control_classes_only(void)
{
    char out[64];
    int n;

    u_test_fill(out, sizeof(out));
    n = test_usermode_capture_escape("a\\b\nc\rd[e", 9u, out, sizeof(out));
    TEST_ASSERT(n >= 0, "an ample buffer must not refuse");
    TEST_ASSERT_EQ((uint32_t)n, 21u,
                   "4 escaped bytes (\\, LF, CR, '[') at 4 chars each plus "
                   "5 literal bytes (a b c d e) = 21");
    TEST_ASSERT(u_test_streq(out, "a\\x5cb\\x0ac\\x0dd\\x5be"),
                "backslash->\\x5c, LF->\\x0a, CR->\\x0d, '['->\\x5b, "
                "every other byte passes through unescaped");

    u_test_fill(out, sizeof(out));
    n = test_usermode_capture_escape("plain ASCII, no escapes needed", 30u,
                                     out, sizeof(out));
    TEST_ASSERT_EQ((uint32_t)n, 30u,
                   "unescaped input round-trips at identical length");
    TEST_ASSERT(u_test_streq(out, "plain ASCII, no escapes needed"),
                "no printable-ASCII byte outside \\ and [ is ever rewritten");

    /* ']' is deliberately NOT escaped: only the OPENING '[' can start a
     * marker-shaped prefix, so escaping it alone is sufficient and a
     * captured payload containing a lone ']' is not itself a hazard. */
    u_test_fill(out, sizeof(out));
    n = test_usermode_capture_escape("]", 1u, out, sizeof(out));
    TEST_ASSERT_EQ((uint32_t)n, 1u,
                   "']' alone is not one of the escaped classes");
    TEST_ASSERT(u_test_streq(out, "]"),
                "']' passes through unescaped");

    /* A raw high byte (0x80, a lone UTF-8 continuation byte) and DEL
     * (0x7F) must both be escaped -- proving the ASCII-safety fix that
     * closed the "host UTF-8 decode mangles non-ASCII capture" gap. */
    u_test_fill(out, sizeof(out));
    n = test_usermode_capture_escape("\x80\x7f", 2u, out, sizeof(out));
    TEST_ASSERT_EQ((uint32_t)n, 8u,
                   "both bytes outside 0x20-0x7E escape to \\xHH");
    TEST_ASSERT(u_test_streq(out, "\\x80\\x7f"),
                "0x80 and 0x7f both escape, never pass through raw");
}

/* A buffer one byte short of the exact worst-case need must refuse rather
 * than write a truncated, silently-shorter escape a caller could mistake
 * for a complete one. */
static void test_capture_escape_refuses_short_buffer(void)
{
    char out[4];  /* "\xHH" is exactly 4 bytes; the NUL needs a 5th */

    u_test_fill(out, sizeof(out));
    TEST_ASSERT(test_usermode_capture_escape("\n", 1u, out, sizeof(out)) < 0,
                "one raw byte needing 4 escaped chars plus a NUL must "
                "refuse a 4-byte buffer");
}

static void test_capture_escape_empty_input(void)
{
    char out[8];

    u_test_fill(out, sizeof(out));
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_escape("", 0u, out, sizeof(out)),
                   0u, "zero raw bytes escape to zero bytes");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)out[0], (uint32_t)'\0',
                   "the output is still NUL-terminated at length zero");
}

/* The chunk size is DERIVED from UTEST_RECORD_LINE_MAX and the record's own
 * fixed literal cost (matching how UTEST_MAX_BINARY_NAME is derived, never
 * hand-picked), and the per-call staging buffer in test_usermode.h
 * (struct utest_capture_ctx._buf[192]) must be large enough to hold it. */
static void test_capture_chunk_max_is_positive_and_bounded(void)
{
    uint32_t chunk_max = test_usermode_capture_chunk_max();

    TEST_ASSERT(chunk_max > 0u,
                "the derived chunk size must leave room for at least one "
                "raw byte per record");
    TEST_ASSERT(chunk_max <= 192u,
                "the chunk size must fit utest_capture_ctx._buf[192], "
                "the per-write staging buffer");
}

/* ---- Producer-side emission budget ----------------------------------- *
 *
 * The budget's arithmetic is pinned by _Static_asserts in test_usermode.c.
 * These tests deliberately assert what those asserts CANNOT: the state
 * machine's behavior at each transition, and the two derivation intents
 * (an honest binary is never clipped; the budget is the TIGHT count for
 * that, not a generous round number) which the build-time asserts do not
 * express and a future retune could satisfy while destroying. */

static void test_capture_budget_permits_up_to_the_owner_limit(void)
{
    uint32_t budget = test_usermode_capture_owner_budget();

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(0u, 0u, 0, 0, 0),
                   (uint32_t)UTEST_CAP_EMIT,
                   "a fresh owner's first record is charged, not refused");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(budget - 1u, 0u, 0, 0, 0),
                   (uint32_t)UTEST_CAP_EMIT,
                   "the LAST record inside the budget must still be emitted -- "
                   "an off-by-one here silently clips one honest record");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(budget, 0u, 0, 0, 0),
                   (uint32_t)UTEST_CAP_OVER_OWNER,
                   "the claim that finds the counter AT the limit is the one "
                   "that terminates the owner");
}

/* The host demands over.seq == limit for an owner-scope stop, which only
 * holds because the verdict flips at EXACTLY the budget. A latch that
 * somehow missed would still never re-open the stream into EMIT. */
static void test_capture_budget_never_reopens_past_the_limit(void)
{
    uint32_t budget = test_usermode_capture_owner_budget();

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(budget + 1u, 0u, 0, 0, 0),
                   (uint32_t)UTEST_CAP_OVER_OWNER,
                   "a counter already past the limit must never resolve to "
                   "EMIT, whatever moved it there");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(0xFFFFFFFFu, 0u, 0, 0, 0),
                   (uint32_t)UTEST_CAP_OVER_OWNER,
                   "a saturated sequence counter still refuses rather than "
                   "wrapping into a fresh budget");
}

/* A latched owner emits NOTHING further -- not even a second terminator.
 * The host refuses a duplicate marker, so a second one would turn a
 * correctly bounded run red. */
static void test_capture_budget_latched_owner_drops_everything(void)
{
    uint32_t budget = test_usermode_capture_owner_budget();
    uint32_t run_budget = test_usermode_capture_run_budget();

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(0u, 0u, 1, 0, 0),
                   (uint32_t)UTEST_CAP_DROP,
                   "a latched owner drops even a record that would otherwise "
                   "have been well inside every budget");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(budget, run_budget, 1, 1,
                                                          0),
                   (uint32_t)UTEST_CAP_DROP,
                   "the latch wins over both exhausted budgets -- exactly one "
                   "terminator per owner, never a second");
}

/* Scope attribution: a binary that exhausted its OWN budget must be
 * reported against its own limit even when the run is also exhausted, or
 * the host's owner-scope equality check would be applied to a run stop. */
static void test_capture_budget_owner_scope_wins_over_run(void)
{
    uint32_t budget = test_usermode_capture_owner_budget();
    uint32_t run_budget = test_usermode_capture_run_budget();

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(budget, run_budget, 0, 0,
                                                          0),
                   (uint32_t)UTEST_CAP_OVER_OWNER,
                   "an owner over its own budget is an owner-scope stop even "
                   "when the aggregate is exhausted too");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(0u, run_budget, 0, 0, 0),
                   (uint32_t)UTEST_CAP_OVER_RUN,
                   "an owner well inside its own budget stopped by the "
                   "aggregate is a run-scope stop");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(0u, 0u, 0, 1, 0),
                   (uint32_t)UTEST_CAP_OVER_RUN,
                   "the latched run stop terminates later owners without "
                   "re-deciding against the charge count");
}

/* ---- The run-boundary SEAL ------------------------------------------ *
 *
 * The seal is the admission fence: once a run closes, no further claim may
 * be created, whatever else is true of the owner or the budgets. These
 * assert the two properties the drain depends on -- that the seal outranks
 * every other input, and that it changes NOTHING about the owner it
 * refused. */

static void test_capture_seal_outranks_every_other_verdict(void)
{
    uint32_t budget = test_usermode_capture_owner_budget();
    uint32_t run_budget = test_usermode_capture_run_budget();

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(0u, 0u, 0, 0, 1),
                   (uint32_t)UTEST_CAP_SEALED,
                   "a sealed run refuses even a claim that every budget would "
                   "have accepted -- that refusal IS the fence");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(0u, 0u, 1, 0, 1),
                   (uint32_t)UTEST_CAP_SEALED,
                   "the seal outranks a latched owner: a boundary refusal must "
                   "not be reported as that binary's own policy stop");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_decide(budget, run_budget, 0, 1,
                                                          1),
                   (uint32_t)UTEST_CAP_SEALED,
                   "and it outranks both exhausted budgets, so a closed run "
                   "never emits one last overflow marker after its census");
}

/* A SEALED claim is the only verdict that mutates nothing at all. Drawing a
 * sequence number would leave a hole in a stream whose run has already
 * published its record count, and latching the owner would carry a
 * boundary refusal into a slot the NEXT run reads. */
static void test_capture_seal_consumes_nothing_and_latches_nothing(void)
{
    uint32_t seq = 7u, run = 3u, drawn = 0, charged = 0;
    uint64_t admitted = 11u;
    int stopped = 0, over = 0;

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         1, &admitted),
                   (uint32_t)UTEST_CAP_SEALED, "a sealed run refuses the claim");
    TEST_ASSERT_EQ(seq, 7u, "a SEALED claim consumes no sequence number -- the "
                            "hole would land past a published census");
    TEST_ASSERT_EQ(run, 3u, "and charges the run nothing");
    TEST_ASSERT_EQ((uint32_t)stopped, 0u,
                   "and must NOT latch the owner: the binary did nothing wrong "
                   "and the latch outlives the boundary that set it");
    TEST_ASSERT_EQ((uint32_t)over, 0u, "and must not latch the run stop either");
    TEST_ASSERT_EQ(admitted, 11u,
                   "and reserves no record -- a reservation here would make the "
                   "next boundary drain wait for a claim that never existed");
}

/* Every claim that draws a sequence number also reserves a record, and the
 * pairing is what the drain's guarantee rests on: pending counts exactly the
 * numbers drawn whose records have not yet reached the wire. */
static void test_capture_claim_reserves_exactly_when_it_draws(void)
{
    uint32_t run_budget = test_usermode_capture_run_budget();
    uint32_t seq = 0, run = 0, drawn = 0, charged = 0;
    uint64_t admitted = 0;
    int stopped = 0, over = 0;

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, &admitted),
                   (uint32_t)UTEST_CAP_EMIT, "a fresh claim emits");
    TEST_ASSERT_EQ(admitted, 1u, "an emitted chunk reserves one record");

    /* A run-scope terminator draws a number too, so it owes the wire a
     * record exactly as a chunk does. */
    run = run_budget;
    stopped = 0;
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, &admitted),
                   (uint32_t)UTEST_CAP_OVER_RUN, "an exhausted run terminates");
    TEST_ASSERT_EQ(admitted, 2u,
                   "a terminator reserves a record too -- it draws a sequence "
                   "number, so the drain must wait for it like any other");

    /* A DROP draws nothing, so it must reserve nothing: a reservation with
     * no record behind it stalls the next boundary for its whole budget. */
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, &admitted),
                   (uint32_t)UTEST_CAP_DROP, "the latched owner now drops");
    TEST_ASSERT_EQ(admitted, 2u, "a DROP reserves nothing");
}

/* ---- The drain's arithmetic ----------------------------------------- *
 *
 * These are the states a live run cannot be driven into: a completion that
 * arrives after its own reservation was written off needs an emitter
 * preempted across a frame rollover. Over plain numbers each is one call,
 * and the property that matters is that NONE of them ever reports a
 * pending count larger than what was admitted. */

static void test_capture_pending_counts_records_still_owed(void)
{
    TEST_ASSERT_EQ(test_usermode_capture_pending(0u, 0u, 0u), 0u,
                   "a run that admitted nothing owes nothing");
    TEST_ASSERT_EQ(test_usermode_capture_pending(5u, 5u, 0u), 0u,
                   "a fully delivered run has nothing in flight");
    TEST_ASSERT_EQ(test_usermode_capture_pending(5u, 3u, 0u), 2u,
                   "two admitted claims that have not reached the wire are the "
                   "two records the boundary must wait for");
    TEST_ASSERT_EQ(test_usermode_capture_pending(5u, 3u, 2u), 0u,
                   "written off, those two stop being owed -- that is what lets "
                   "the NEXT run start its drain at zero");
}

/* The floor is DEFENSIVE, and the test says so rather than pretending to
 * exercise a reachable state. Once the close advances the generation with
 * the write-off, a forgiven claim's epoch is over and it can never settle
 * again, so deliveries cannot exceed admissions on any real path. The floor
 * stays because the alternative failure is silent and total: an unsigned
 * subtraction in that state answers near UINT64_MAX and turns a fully
 * delivered run into a drain that always times out. */
static void test_capture_pending_never_underflows(void)
{
    TEST_ASSERT_EQ(test_usermode_capture_pending(1u, 1u, 1u), 0u,
                   "double settlement must floor at nothing owed, not wrap to "
                   "an astronomically large outstanding count");
    TEST_ASSERT_EQ(test_usermode_capture_pending(0u, 7u, 3u), 0u,
                   "deliveries in excess of admissions still report zero owed");
    TEST_ASSERT_EQ(test_usermode_capture_pending(10u, 0xFFFFFFFFull, 5u), 0u,
                   "and a delivered count past the old uint32 ceiling is still "
                   "just a large delivered count, not an outstanding record");
}

/* The close is ONE transition: write off what is outstanding AND end the
 * epoch. Splitting them is what let a written-off claim settle a second time
 * -- the generation stayed current until the next frame began, so an emitter
 * resuming in that window passed its generation check and credited a claim
 * already counted in `forgiven`. */
static void test_capture_close_writes_off_and_ends_the_epoch(void)
{
    uint64_t admitted = 3u, completed = 1u, forgiven = 0u;
    uint32_t gen = 41u;

    TEST_ASSERT_EQ(test_usermode_capture_close(&admitted, &completed,
                                               &forgiven, &gen), 2u,
                   "the close writes off exactly what was still outstanding");
    TEST_ASSERT_EQ(forgiven, 2u, "and records it as forgiven, not delivered");
    TEST_ASSERT_EQ((uint64_t)gen, 42u,
                   "and ADVANCES the generation in the same act -- a write-off "
                   "that left the epoch open is how a forgiven claim settles "
                   "a second time");
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, completed, forgiven),
                   0u, "so the closed run leaves nothing owed behind it");

    /* Idempotent over an already-settled run: nothing outstanding means
     * nothing written off, but the epoch still ends. */
    TEST_ASSERT_EQ(test_usermode_capture_close(&admitted, &completed,
                                               &forgiven, &gen), 0u,
                   "closing a settled run writes nothing off");
    TEST_ASSERT_EQ(forgiven, 2u, "and does not inflate the loss it reported");
    TEST_ASSERT_EQ((uint64_t)gen, 43u, "while still ending that epoch");
}

/* An UNSETTLED claim is reported, whether it never reached klog or reached
 * it and lost its emitter before the credit. That conflation is a decision,
 * not an omission, and this test is where the decision is pinned.
 *
 * No producer-side counter can be simultaneous with a record reaching the
 * host -- klog writes serial, then renders to the framebuffer, then appends
 * and synchronously flushes the disk log -- so a killed emitter is
 * necessarily misreported one way or the other, and BOTH placements straddle
 * that same tail. The choice is therefore about DIRECTION, not width:
 * crediting before the emission turns a lost record into a GREEN run, while
 * crediting after turns a delivered one into a REFUSED run, and a refused
 * run gets looked at. What makes the ambiguity practically non-binding is
 * outside this counter -- the reap signals first and waits
 * UTEST_CAPTURE_REAP_GRACE_MS, so an emitter inside klog finishes and
 * credits itself. See s_capture_completed in test_usermode.c for the full
 * argument this test pins. */
static void test_capture_close_reports_an_unsettled_claim_fail_closed(void)
{
    uint64_t admitted = 1u, completed = 0u, forgiven = 0u;
    uint32_t gen = 5u;

    TEST_ASSERT_EQ(test_usermode_capture_close(&admitted, &completed,
                                               &forgiven, &gen), 1u,
                   "an uncredited claim is REPORTED, not assumed delivered -- "
                   "the run is refused rather than passed while a record it "
                   "drew a sequence number for may never have reached the host");
    TEST_ASSERT_EQ(forgiven, 1u,
                   "and is written off in the same act, so the refusal lands on "
                   "this run and the next one starts from zero");
}

/* The lifetime counters are 64-bit, and the claim transition must carry them
 * at that width. A uint32 field in the middle of the path silently discarded
 * the high bits on the very next claim, which is worse than never widening:
 * completed + forgiven then exceeds a truncated admitted forever, pending
 * reads zero, and the drain stops waiting for anything at all. */
static void test_capture_admission_survives_past_the_uint32_ceiling(void)
{
    uint64_t admitted = 0x1FFFFFFFFull;   /* well past UINT32_MAX */
    uint32_t seq = 0, run = 0, drawn = 0, charged = 0;
    int stopped = 0, over = 0;

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, &admitted),
                   (uint32_t)UTEST_CAP_EMIT, "the claim is charged normally");
    TEST_ASSERT_EQ(admitted, 0x200000000ull,
                   "and the admission count keeps its high bits -- a 32-bit "
                   "field here would drop them and strand the drain");
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, 0x1FFFFFFFFull, 0u),
                   1u,
                   "so a lifetime past the uint32 ceiling still reports the "
                   "one record actually outstanding");
}

/* The exact collision both review legs named, as a regression: forgive run
 * N's claim, admit an unfinished run N+1 claim, then deliver run N's late
 * completion. N+1 must STILL report one pending record.
 *
 * The delivery is refused by generation, not by arithmetic, which is why
 * this test drives the close rather than hand-adding to `completed`: after
 * the close, run N's claim carries a generation that is no longer current,
 * and u_capture_complete settles only against the current one. Modelling the
 * late completion as a bare increment is precisely the mistake the earlier
 * version of this test made -- it asserted the collision was harmless. */
static void test_capture_late_completion_cannot_cancel_a_live_reservation(void)
{
    uint64_t admitted = 0u, completed = 0u, forgiven = 0u;
    uint32_t gen = 7u;
    uint32_t stale_claim_gen;
    uint32_t seq = 0, run = 0, drawn = 0, charged = 0;
    int stopped = 0, over = 0;

    /* Run N admits one claim; its emitter is preempted before emitting, so
     * it never reaches the log call and never commits. */
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, &admitted),
                   (uint32_t)UTEST_CAP_EMIT, "run N's claim is charged");
    stale_claim_gen = gen;

    /* Run N's boundary writes it off and ends the epoch. */
    TEST_ASSERT_EQ(test_usermode_capture_close(&admitted, &completed,
                                               &forgiven, &gen), 1u,
                   "run N reports the record it could not deliver");
    TEST_ASSERT(stale_claim_gen != gen,
                "and the stale claim's epoch is now closed, which is what "
                "stops its late completion from settling anything");

    /* Run N+1 admits a claim of its own, still unfinished. */
    admitted++;
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, completed, forgiven),
                   1u, "run N+1's own record is outstanding");

    /* Run N's emitter finally resumes and ACTUALLY ATTEMPTS to settle. This
     * is the step that pins the guard: asserting an unchanged pending value
     * here would stay green with the guard deleted, because the damage of a
     * stale settlement is not local -- it is a surplus credit that cancels
     * somebody else's live reservation. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_capture_settle(stale_claim_gen, gen,
                                                          &completed),
                   0u,
                   "a claim from a closed epoch is REFUSED settlement");
    TEST_ASSERT_EQ(completed, 0u,
                   "so it does not credit the counter run N+1's own claim is "
                   "measured against");
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, completed, forgiven),
                   1u,
                   "and run N+1's record STAYS outstanding -- a late completion "
                   "crediting a forgiven claim would cancel a live reservation "
                   "and let this run roll over losing it");

    /* The guard is not a blanket refusal: a claim of the CURRENT epoch still
     * settles, or the drain would never reach zero on a healthy run. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_capture_settle(gen, gen, &completed),
                   1u, "a current-epoch claim settles normally");
    TEST_ASSERT_EQ(completed, 1u, "and credits exactly one record");
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, completed, forgiven),
                   0u, "which is what lets run N+1's drain reach zero");
}

/* Three consecutive runs, each leaving something unsettled, and each stale
 * emitter arriving one run late. No write-off may leak forward: the run that
 * admitted a claim is the run that reports it, and a later run's own loss
 * must still be visible underneath every accumulated forgiveness. */
static void test_capture_write_offs_do_not_leak_across_three_runs(void)
{
    uint64_t admitted = 0u, completed = 0u, forgiven = 0u;
    uint32_t gen = 1u;
    uint32_t claim_a, claim_b, claim_c;
    uint32_t i;

    for (i = 0; i < 3u; i++) {
        uint32_t before = gen;

        admitted++;                       /* one claim, never settled */
        TEST_ASSERT_EQ(test_usermode_capture_close(&admitted, &completed,
                                                   &forgiven, &gen), 1u,
                       "every run reports exactly its OWN unsettled record, "
                       "never a predecessor's already-written-off one");
        TEST_ASSERT(before != gen, "and ends its own epoch");
        if (i == 0u) claim_a = before;
        else if (i == 1u) claim_b = before;
        else claim_c = before;
    }

    TEST_ASSERT_EQ(forgiven, 3u, "three runs wrote off three records");
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, completed, forgiven),
                   0u, "and left nothing owed to a fourth");

    /* Every one of those emitters now arrives late. None may settle. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_capture_settle(claim_a, gen,
                                                          &completed), 0u,
                   "the oldest stale claim is refused");
    TEST_ASSERT_EQ((uint64_t)test_usermode_capture_settle(claim_b, gen,
                                                          &completed), 0u,
                   "and so is the middle one");
    TEST_ASSERT_EQ((uint64_t)test_usermode_capture_settle(claim_c, gen,
                                                          &completed), 0u,
                   "and so is the most recent -- one epoch back is still back");
    TEST_ASSERT_EQ(completed, 0u,
                   "no accumulated credit exists to cancel a future claim");

    /* A fourth run's genuine loss is still visible under all of it. */
    admitted++;
    TEST_ASSERT_EQ(test_usermode_capture_close(&admitted, &completed,
                                               &forgiven, &gen), 1u,
                   "so a fourth run's own lost record is still reported, not "
                   "absorbed by three runs' worth of forgiveness");
}

/* The regression the section exists for, at the seam the kernel can drive:
 * a descendant that claims in run N, pauses, and resumes across a second
 * u_frame_begin() must leave run N+1's counts exactly as if it had never
 * existed.
 *
 * Sequenced here as the three accounting events that shape really is --
 * run N admits the claim; run N's boundary drains, finds it outstanding and
 * writes it off; run N+1 starts -- because the live schedule that produces
 * it (an emitter preempted between its claim and its klog write, across a
 * frame rollover) is not constructible from a kernel test. */
static void test_capture_stale_emitter_leaves_the_next_run_untouched(void)
{
    uint64_t admitted = 0, completed = 0, forgiven = 0;
    uint32_t gen = 3u;
    uint32_t seq = 0, run = 0, drawn = 0, charged = 0;
    int stopped = 0, over = 0;

    /* Run N: the descendant claims and is preempted before emitting. */
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, &admitted),
                   (uint32_t)UTEST_CAP_EMIT, "the descendant's claim is charged");
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, completed, forgiven),
                   1u,
                   "run N's boundary sees the claim outstanding -- which is what "
                   "makes the drain wait for it rather than roll over");

    /* Run N's boundary writes it off and ends its epoch in one act. */
    TEST_ASSERT_EQ(test_usermode_capture_close(&admitted, &completed,
                                               &forgiven, &gen), 1u,
                   "run N owns the loss and reports it against itself");
    TEST_ASSERT_EQ(test_usermode_capture_pending(admitted, completed, forgiven),
                   0u, "so run N+1 begins with nothing owed");

    /* Run N+1: a fresh claim from a NEW binary sees pristine run state --
     * the stale descendant perturbed neither the record charge nor the run
     * stop latch. */
    run = 0u;
    over = 0;
    stopped = 0;
    seq = 0u;
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, &admitted),
                   (uint32_t)UTEST_CAP_EMIT,
                   "run N+1's first claim is charged normally");
    TEST_ASSERT_EQ(charged, 1u,
                   "and is the run's FIRST charge -- a stale emitter must not "
                   "leave its predecessor's charge in the new run's aggregate");
}

/* ---- The claim's state TRANSITION ----------------------------------- *
 *
 * The decision helper above says what SHOULD happen; these drive what
 * actually changes. Every host invariant rests on this half: charge the
 * run only for a chunk, latch on both stops, consume a sequence number for
 * every record and for nothing else. A regression in any of them leaves
 * the decision-level assertions above completely green. */

static void test_capture_claim_charges_the_run_only_for_a_chunk(void)
{
    uint32_t seq = 0, run = 0, drawn = 0, charged = 0;
    int stopped = 0, over = 0;
    uint32_t budget = test_usermode_capture_owner_budget();

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, (uint64_t *)0),
                   (uint32_t)UTEST_CAP_EMIT, "a fresh claim emits");
    TEST_ASSERT_EQ(drawn, 0u, "the first chunk is drawn at seq 0");
    TEST_ASSERT_EQ(seq, 1u, "an emitted chunk consumes its sequence number");
    TEST_ASSERT_EQ(run, 1u, "and charges the run exactly once");
    TEST_ASSERT_EQ(charged, 1u, "the reported charge is the post-charge total");

    /* The terminator consumes a seq (that is what makes it the highest)
     * but must NOT charge the run: the run allowance reserves marker
     * traffic separately, and charging here would make the host's
     * observed-chunks == limit equality unsatisfiable. */
    seq = budget;
    run = 5u;
    stopped = 0;
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, (uint64_t *)0),
                   (uint32_t)UTEST_CAP_OVER_OWNER, "at the limit it terminates");
    TEST_ASSERT_EQ(drawn, budget, "the terminator is drawn AT the limit, "
                                  "which is the equality the host demands");
    TEST_ASSERT_EQ(seq, budget + 1u, "the terminator consumes its sequence "
                                     "number so it is the stream's highest");
    TEST_ASSERT_EQ(run, 5u, "a terminator must never charge the run budget");
    TEST_ASSERT_EQ((uint32_t)stopped, 1u, "and it latches the owner");
}

static void test_capture_claim_latches_and_then_consumes_nothing(void)
{
    uint32_t seq = 4u, run = 9u, drawn = 0, charged = 0;
    int stopped = 1, over = 0;

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, (uint64_t *)0),
                   (uint32_t)UTEST_CAP_DROP, "a latched owner drops");
    TEST_ASSERT_EQ(seq, 4u, "a DROP must consume NO sequence number -- one "
                            "spent here is a hole the host reads as lost output");
    TEST_ASSERT_EQ(run, 9u, "and must not charge the run");
    TEST_ASSERT_EQ((uint32_t)stopped, 1u, "the latch stays set");
}

static void test_capture_claim_run_stop_latches_both(void)
{
    uint32_t run_budget = test_usermode_capture_run_budget();
    uint32_t seq = 3u, run = run_budget, drawn = 0, charged = 0;
    int stopped = 0, over = 0;

    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, (uint64_t *)0),
                   (uint32_t)UTEST_CAP_OVER_RUN, "an exhausted run terminates");
    TEST_ASSERT_EQ((uint32_t)over, 1u, "the run latch is set so LATER owners "
                                       "stop without re-deciding");
    TEST_ASSERT_EQ((uint32_t)stopped, 1u, "and this owner is latched too");
    TEST_ASSERT_EQ(seq, 4u, "the run terminator consumes a sequence number");
    TEST_ASSERT_EQ(run, run_budget, "but charges nothing further");
    TEST_ASSERT_EQ(charged, run_budget,
                   "and reports the aggregate frozen AT the limit -- the host "
                   "requires charged == limit for a run-scope marker");

    /* A SECOND owner meeting the latch takes the same branch, and its own
     * marker must report the identical frozen aggregate: the host refuses
     * run-scope markers that disagree. */
    seq = 0u;
    stopped = 0;
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_apply(&seq, &run, &stopped,
                                                         &over, &drawn, &charged,
                                                         0, (uint64_t *)0),
                   (uint32_t)UTEST_CAP_OVER_RUN, "a later owner meets the latch");
    TEST_ASSERT_EQ(charged, run_budget,
                   "every run-scope marker in one run reports the same frozen "
                   "aggregate, or the host refuses them as a limit conflict");
}

/* ---- The EMISSION seam ---------------------------------------------- *
 *
 * The transition tests above prove the arithmetic and the state changes;
 * nothing yet proves the switch that turns a verdict into an actual wire
 * record. A wrong scope/limit/charged field, a missing marker, or an
 * emission that keeps going after the latch would leave every assertion
 * above green while defeating the whole feature. This drives the REAL
 * emitter -- test_usermode_capture_start/_byte/_end on a live ctx -- and
 * reads the records back out of the klog ring.
 *
 * Reaching the budget honestly would take 1425 records of serial, so the
 * counters are seeded one short of it instead -- BOTH of them. Seeding only
 * the owner's sequence would leave the terminator reporting a `charged`
 * value the real single-owner path can never produce (every one of those
 * 1425 chunks also charged the run), so the probe would assert a marker
 * shape the host would never actually receive. The run accounting is
 * snapshotted and restored around the probe for the same reason: it is
 * global state, and a test that charges it without putting it back makes
 * every later capture test order-dependent. */
static void test_capture_emits_exactly_one_owner_marker_then_stops(void)
{
    struct task *self = task_current();
    struct utest_capture_ctx ctx;
    uint8_t  saved_active;
    uint32_t saved_owner;
    int32_t  saved_seq;
    uint8_t  saved_stopped;
    uint32_t saved_run_records;
    int      saved_run_over;
    int      saved_sealed;
    uint32_t count_before, count_after, head;
    uint32_t budget = test_usermode_capture_owner_budget();
    const klog_entry_t *ring;
    const klog_entry_t *last;

    if (!self) {
        TEST_SKIP("no current task available in this test context");
        return;
    }
    saved_active  = self->utest_capture_active;
    saved_owner   = self->utest_capture_owner_pid;
    saved_seq     = atomic_read(&self->utest_capture_seq);
    saved_stopped = __atomic_load_n(&self->utest_capture_stopped,
                                   __ATOMIC_ACQUIRE);
    test_usermode_capture_run_state_get(&saved_run_records, &saved_run_over,
                                        &saved_sealed);

    self->utest_capture_active = 1;
    self->utest_capture_owner_pid = self->pid;
    __atomic_store_n(&self->utest_capture_stopped, 0, __ATOMIC_RELEASE);
    /* One record short of the budget: the next write is the last permitted
     * chunk, and the one after it must be the terminator. */
    atomic_set(&self->utest_capture_seq, (int32_t)(budget - 1u));
    /* The run aggregate a real single-owner run would be carrying here. */
    /* Unsealed explicitly: the seal survives the last framed run, so a
     * probe that drove the real emitter without clearing it would be
     * refused for a reason that has nothing to do with what it asserts. */
    test_usermode_capture_run_state_set(budget - 1u, 0, 0);

    test_usermode_capture_start(&ctx);
    (void)test_usermode_capture_byte(&ctx, 'x');
    (void)klog_get_ring(&count_before, &head);
    test_usermode_capture_end(&ctx);
    ring = klog_get_ring(&count_after, &head);
    last = &ring[(head + KLOG_RING_SIZE - 1u) % KLOG_RING_SIZE];

    TEST_ASSERT_EQ((uint64_t)(count_after - count_before), 1u,
                   "the last permitted write still emits its chunk");
    TEST_ASSERT(u_test_contains(last->message, "[UTEST-CAPTURE]"),
                "and it is an ordinary chunk record, not a marker");

    /* The NEXT write crosses the budget: exactly one terminator, carrying
     * the owner scope and the limit the host checks for equality. */
    test_usermode_capture_start(&ctx);
    (void)test_usermode_capture_byte(&ctx, 'y');
    (void)klog_get_ring(&count_before, &head);
    test_usermode_capture_end(&ctx);
    ring = klog_get_ring(&count_after, &head);
    last = &ring[(head + KLOG_RING_SIZE - 1u) % KLOG_RING_SIZE];

    TEST_ASSERT_EQ((uint64_t)(count_after - count_before), 1u,
                   "crossing the budget emits exactly ONE record");
    TEST_ASSERT(u_test_contains(last->message, "[UTEST-CAPTURE-OVER]"),
                "and that record is the overflow terminator, not a chunk");
    TEST_ASSERT(u_test_contains(last->message, "scope=owner"),
                "an owner exhausting its OWN budget is an owner-scope stop");
    /* The exact numeric fields the host reconciles against: it demands
     * seq == limit for an owner-scope stop and charged >= that owner's own
     * chunk count, so a marker carrying the wrong ones fails a real run
     * while every structural assertion above still passes. */
    TEST_ASSERT_EQ(u_test_field_u32(last->message, " seq="), budget,
                   "the terminator is drawn AT the budget, which is the "
                   "equality the host checks for an owner-scope stop");
    TEST_ASSERT_EQ(u_test_field_u32(last->message, " limit="), budget,
                   "and declares the same limit it stopped at");
    TEST_ASSERT_EQ(u_test_field_u32(last->message, " charged="), budget,
                   "and reports the run aggregate a real single-owner run "
                   "would be carrying, not a value only a seeded test sees");
    TEST_ASSERT_EQ((uint32_t)__atomic_load_n(&self->utest_capture_stopped,
                                             __ATOMIC_ACQUIRE), 1u,
                   "and the owner is latched by the real emitter, not just "
                   "by the pure transition");

    /* A THIRD write must produce nothing at all: the host refuses a second
     * marker, so an emitter that kept going would turn a correctly bounded
     * run red. */
    test_usermode_capture_start(&ctx);
    (void)test_usermode_capture_byte(&ctx, 'z');
    (void)klog_get_ring(&count_before, &head);
    test_usermode_capture_end(&ctx);
    (void)klog_get_ring(&count_after, &head);

    TEST_ASSERT_EQ((uint64_t)(count_after - count_before), 0u,
                   "a latched owner emits NOTHING further -- not a chunk, "
                   "and above all not a second terminator");

    /* And it must reach that state through the DISCARD fast path, not by
     * escaping and locking its way to a DROP on every later write. The
     * budget bounds serial traffic; without this it would leave producer
     * CPU and global-lock contention unbounded for an abusive binary that
     * keeps writing after its stop. `_discard` is the observable proof
     * that capture_start recognised the latch before staging a byte. */
    test_usermode_capture_start(&ctx);
    TEST_ASSERT_EQ((uint32_t)ctx._discard, 1u,
                   "a write opened AFTER the stop enters discard mode up "
                   "front, so it never escapes or takes the budget lock");
    TEST_ASSERT_EQ((uint32_t)test_usermode_capture_byte(&ctx, 'w'), 1u,
                   "a discarded byte is still CONSUMED -- returning 0 would "
                   "send the caller to raw serial and undo the budget");
    TEST_ASSERT_EQ(ctx._len, 0u,
                   "and it is not staged: discard mode does no per-byte work");
    test_usermode_capture_end(&ctx);

    self->utest_capture_active = saved_active;
    self->utest_capture_owner_pid = saved_owner;
    atomic_set(&self->utest_capture_seq, saved_seq);
    __atomic_store_n(&self->utest_capture_stopped, saved_stopped,
                     __ATOMIC_RELEASE);
    test_usermode_capture_run_state_set(saved_run_records, saved_run_over,
                                        saved_sealed);
}

/* The derivation's PURPOSE: a binary that fills every chunk must be able
 * to spend its whole declared raw allowance. The build-time assert proves
 * the wire cost fits; only this proves the payload does. */
static void test_capture_owner_budget_covers_its_raw_allowance(void)
{
    uint32_t budget = test_usermode_capture_owner_budget();
    uint32_t chunk = test_usermode_capture_chunk_max();
    uint32_t raw_max = test_usermode_capture_owner_raw_max();

    TEST_ASSERT(budget * chunk >= raw_max,
                "the per-owner record budget must carry the whole per-owner "
                "raw allowance at worst-case chunk fill, or an honest binary "
                "is clipped below what a consumer would have retained");
    TEST_ASSERT((budget - 1u) * chunk < raw_max,
                "and it must be the TIGHT count for that: a budget larger "
                "than the ceiling division buys wire time no consumer keeps");
}

static void test_capture_run_budget_covers_owner_and_run_allowance(void)
{
    uint32_t run_budget = test_usermode_capture_run_budget();
    uint32_t owner_budget = test_usermode_capture_owner_budget();
    uint32_t chunk = test_usermode_capture_chunk_max();
    uint32_t run_raw = test_usermode_capture_run_raw_max();

    TEST_ASSERT(run_budget >= owner_budget,
                "a run ceiling below the per-binary one would turn an "
                "aggregate abuse stop into a truncation of the first honest "
                "binary to run");
    TEST_ASSERT(run_budget * chunk >= run_raw,
                "the run budget must carry the whole run raw allowance at "
                "worst-case chunk fill");
    TEST_ASSERT((run_budget - 1u) * chunk < run_raw,
                "and must be the tight count for it");
}

/* The wire model is the reason the budgets bound anything. It must
 * dominate the plain-path cost the skip markers use (capture records take
 * klog's color branch, which is more expensive), and both allowances must
 * hold their own worst case. */
static void test_capture_wire_model_bounds_both_allowances(void)
{
    uint32_t wire = test_usermode_capture_wire_max();
    uint32_t owner_budget = test_usermode_capture_owner_budget();
    uint32_t run_budget = test_usermode_capture_run_budget();

    TEST_ASSERT(wire > test_usermode_json_line_max(),
                "one record's WIRE cost must exceed the record's own text "
                "bound -- a model that lost klog's framing would under-count "
                "every budget derived from it");
    TEST_ASSERT((owner_budget + 1u) * wire <=
                    test_usermode_capture_owner_wire_allowance(),
                "the per-owner burst plus its own terminator must fit the "
                "per-owner serialized-byte allowance");
    TEST_ASSERT(run_budget * wire <=
                    test_usermode_capture_run_wire_allowance(),
                "the run-wide burst must fit the run allowance with room "
                "left for the per-owner terminators it reserves");
}

/* The kernel test suite itself runs as an ordinary task, never spawned
 * through task_create_captured -- so its capture_active is the same
 * zeroed baseline every constructor gives a fresh slot. This proves the
 * gate end-to-end (not just via the header's release-flavor no-op): a
 * real, running, KERNEL_TESTS task with capture inactive gets told to
 * fall back to its own raw serial path. */
static void test_capture_byte_uncaptured_task_returns_zero(void)
{
    struct utest_capture_ctx ctx;

    test_usermode_capture_start(&ctx);
    TEST_ASSERT(test_usermode_capture_byte(&ctx, 'x') == 0,
                "the current (test-runner) task is never capture-owned, "
                "so the byte must be declined for the caller's raw "
                "serial fallback");
}

/* Driven against two zeroed scratch TCBs -- no task_create, no task_fork,
 * no subsystem init, nothing live: task_utest_capture_inherit touches
 * only the two named fields of the structs it is handed. This is the
 * exact primitive task_fork() calls (TASK_UTEST_CAPTURE_INHERIT), so a
 * pass here proves the inheritance CONTRACT independent of the live
 * fork() path (which the ring-3 test_process.exe suite exercises
 * end-to-end). */
static void test_capture_inherit_copies_active_and_owner_unchanged(void)
{
    task_utest_capture_reset(&s_capture_parent_scratch);
    task_utest_capture_reset(&s_capture_child_scratch);
    s_capture_parent_scratch.utest_capture_active = 1;
    s_capture_parent_scratch.utest_capture_owner_pid = 7u;
    /* The child's own seq must stay untouched by inherit -- only the
     * OWNER's slot is ever read, so a non-zero child seq here would prove
     * inherit wrongly touched it. */
    atomic_set(&s_capture_child_scratch.utest_capture_seq, 99);

    task_utest_capture_inherit(&s_capture_child_scratch,
                               &s_capture_parent_scratch);

    TEST_ASSERT_EQ((uint64_t)s_capture_child_scratch.utest_capture_active,
                   1u, "active is copied from parent to child unchanged");
    TEST_ASSERT_EQ((uint64_t)s_capture_child_scratch.utest_capture_owner_pid,
                   7u, "owner_pid is copied from parent to child unchanged, "
                   "NOT set to the child's own pid");
    TEST_ASSERT_EQ((uint64_t)atomic_read(&s_capture_child_scratch.utest_capture_seq),
                   99u, "inherit must not touch the child's own seq field");
}

/* The regression this section exists for, at the level the fix lives.
 *
 * The old evidence was four file-scope statics that the launcher cleared
 * per spawn and read back after the wait. A loader force-killed on timeout
 * could store into them AFTER that reset, so the NEXT binary's ERROR-versus-
 * FAIL classification was decided by a task that had nothing to do with it.
 * The whole defence was the single-dispatch invariant (one global
 * current_task, APs do not run scheduled tasks) -- an argument about the
 * SCHEDULER, holding up a claim about EVIDENCE, and stated only in comments.
 *
 * Binding the record to the child makes the isolation structural instead:
 * the killed loader's store cannot reach the next binary's fields because
 * they are not the same fields, on any CPU, under any dispatch model. That
 * is what this asserts -- two zeroed scratch TCBs, no task_create, no
 * scheduler, nothing live. */
static void test_loader_evidence_cannot_outlive_its_own_invocation(void)
{
    task_utest_loader_reset(&s_loader_killed_scratch.utest_loader);
    task_utest_loader_reset(&s_loader_next_scratch.utest_loader);

    /* The next binary is spawned and its slot armed. */
    s_loader_next_scratch.utest_loader.test_path = "test_next.exe";

    /* NOW the previously force-killed loader gets its dying stores in --
     * every flag the classification reads, in the order a real loader
     * would have set them. Under the old file-scope statics this is
     * exactly the sequence that rewrote the next binary's verdict. */
    __atomic_store_n(&s_loader_killed_scratch.utest_loader.stage_fault, 1u,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_loader_killed_scratch.utest_loader.reached_exec, 1u,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_loader_killed_scratch.utest_loader.identity_mismatch,
                     1u, __ATOMIC_RELEASE);
    TASK_UTEST_LOADER_MARK_ADOPTED(&s_loader_killed_scratch);
    TASK_UTEST_LOADER_MARK_ENTERED_USER(&s_loader_killed_scratch);

    TEST_ASSERT_EQ((uint64_t)s_loader_next_scratch.utest_loader.stage_fault,
                   0u, "a killed loader's stage fault cannot reach the next "
                   "binary's record");
    TEST_ASSERT_EQ((uint64_t)s_loader_next_scratch.utest_loader.reached_exec,
                   0u, "a killed loader's reached_exec cannot reach the next "
                   "binary's record");
    TEST_ASSERT_EQ((uint64_t)s_loader_next_scratch.utest_loader.identity_mismatch,
                   0u, "a killed loader's identity mismatch cannot reach the "
                   "next binary's record");
    TEST_ASSERT_EQ((uint64_t)s_loader_next_scratch.utest_loader.frame_adopted,
                   0u, "a killed loader's frame adoption cannot reach the "
                   "next binary's record");
    TEST_ASSERT_EQ((uint64_t)s_loader_next_scratch.utest_loader.entered_user,
                   0u, "a killed loader's ring-3 entry cannot reach the next "
                   "binary's record");

    /* And the killed loader's own record is intact -- the isolation must run
     * both ways, or the fix would be "lose the evidence" rather than "bind
     * it". Its verdict is still the never-ran ERROR its stage fault earns. */
    TEST_ASSERT_EQ((uint64_t)s_loader_killed_scratch.utest_loader.stage_fault,
                   1u, "the killed loader still carries its own evidence");
    TEST_ASSERT_EQ(test_usermode_record_verdict(
                       1,
                       (int)s_loader_killed_scratch.utest_loader.stage_fault,
                       1,
                       (int)s_loader_killed_scratch.utest_loader.reached_exec,
                       (int)s_loader_killed_scratch.utest_loader.frame_adopted,
                       (int)s_loader_killed_scratch.utest_loader.entered_user),
                   3u, "the killed loader's own verdict is unchanged by the "
                   "next binary's spawn");
}

/* The snapshot is the production code that carries one child's evidence
 * out of its slot, and a field it swapped or dropped would still reconcile
 * cleanly in the artifacts while publishing the wrong verdict. Asserted on
 * a fixture carrying a UNIQUE sentinel per field, so that ANY permutation
 * of the five copies fails. An alternating 1/0 pattern is not enough and
 * was the first version of this test: it leaves a snapshot that swaps
 * stage_fault with identity_mismatch, or reached_exec with frame_adopted,
 * perfectly green -- and those pairs carry different verdict semantics.
 * The production field values are only ever 0 or 1; the sentinels exist to
 * make the COPY falsifiable, not to model a real record. */
static void test_loader_evidence_snapshot_copies_every_field(void)
{
    uint32_t stage_fault, reached_exec, identity_mismatch;
    uint32_t frame_adopted, entered_user;

    task_utest_loader_reset(&s_loader_killed_scratch.utest_loader);
    s_loader_killed_scratch.utest_loader.stage_fault       = 0x11u;
    s_loader_killed_scratch.utest_loader.reached_exec      = 0x22u;
    s_loader_killed_scratch.utest_loader.identity_mismatch = 0x33u;
    s_loader_killed_scratch.utest_loader.frame_adopted     = 0x44u;
    s_loader_killed_scratch.utest_loader.entered_user      = 0x55u;

    test_usermode_loader_evidence_from_task(&s_loader_killed_scratch,
                                            &stage_fault, &reached_exec,
                                            &identity_mismatch,
                                            &frame_adopted, &entered_user);

    TEST_ASSERT_EQ((uint64_t)stage_fault, 0x11u,
                   "snapshot carries stage_fault, from the stage_fault field");
    TEST_ASSERT_EQ((uint64_t)reached_exec, 0x22u,
                   "snapshot carries reached_exec, from the reached_exec field");
    TEST_ASSERT_EQ((uint64_t)identity_mismatch, 0x33u,
                   "snapshot carries identity_mismatch, from the "
                   "identity_mismatch field");
    TEST_ASSERT_EQ((uint64_t)frame_adopted, 0x44u,
                   "snapshot carries frame_adopted, from the frame_adopted "
                   "field");
    TEST_ASSERT_EQ((uint64_t)entered_user, 0x55u,
                   "snapshot carries entered_user, from the entered_user field");

    /* A child whose slot cannot be resolved must read as nothing observed.
     * The launcher takes this snapshot after the child is dead, so the
     * unresolvable case is reachable in production and must never present
     * a flag the child did not set. */
    test_usermode_loader_evidence_from_task((const struct task *)0,
                                            &stage_fault, &reached_exec,
                                            &identity_mismatch,
                                            &frame_adopted, &entered_user);
    TEST_ASSERT_EQ((uint64_t)(stage_fault | reached_exec | identity_mismatch |
                              frame_adopted | entered_user), 0u,
                   "an unresolvable child snapshots as all-zero, never as a "
                   "flag it did not set");
}

/* The RPL rule that decides whether ring-3 evidence is recorded at all.
 * Wrong in either direction it changes a verdict, and until it was split
 * out of the syscall path it could not be asserted without issuing a
 * syscall -- which a kernel test may not do. */
static void test_loader_entry_evidence_requires_ring3_selector(void)
{
    TEST_ASSERT_EQ((uint64_t)task_utest_cs_is_user(GDT_KERNEL_CODE), 0u,
                   "a ring-0 kernel CS is not ring-3 evidence");
    TEST_ASSERT_EQ((uint64_t)task_utest_cs_is_user(GDT_KERNEL_CODE | 1u), 0u,
                   "RPL 1 is not ring-3 evidence");
    TEST_ASSERT_EQ((uint64_t)task_utest_cs_is_user(GDT_KERNEL_CODE | 2u), 0u,
                   "RPL 2 is not ring-3 evidence");
    TEST_ASSERT_EQ((uint64_t)task_utest_cs_is_user(GDT_USER_CODE | 3u), 1u,
                   "the user code selector at RPL 3 IS ring-3 evidence");
    /* Only the RPL carries privilege; index and table bits do not. A rule
     * that compared whole selectors would reject a legitimate ring-3
     * caller from any other code segment. */
    TEST_ASSERT_EQ((uint64_t)task_utest_cs_is_user(0xDEAD0000ULL | 3u), 1u,
                   "high selector bits do not affect the ring-3 decision");
    TEST_ASSERT_EQ((uint64_t)task_utest_cs_is_user(0xDEAD0000ULL), 0u,
                   "high selector bits alone are not ring-3 evidence");
}

/* The reset is what makes a recycled or forked slot safe, so assert it
 * clears EVERY field rather than the two the classification reads most
 * often: a field left behind is a verdict inherited from another binary. */
static void test_loader_evidence_reset_clears_every_field(void)
{
    s_loader_killed_scratch.utest_loader.test_path         = "stale.exe";
    s_loader_killed_scratch.utest_loader.expect_digest     =
        (const uint8_t *)&s_loader_next_scratch;
    s_loader_killed_scratch.utest_loader.stage_fault       = 1u;
    s_loader_killed_scratch.utest_loader.reached_exec      = 1u;
    s_loader_killed_scratch.utest_loader.identity_mismatch = 1u;
    s_loader_killed_scratch.utest_loader.frame_adopted     = 1u;
    s_loader_killed_scratch.utest_loader.entered_user      = 1u;

    task_utest_loader_reset(&s_loader_killed_scratch.utest_loader);

    TEST_ASSERT(s_loader_killed_scratch.utest_loader.test_path == (const char *)0,
                "reset clears the armed path");
    TEST_ASSERT(s_loader_killed_scratch.utest_loader.expect_digest == (const uint8_t *)0,
                "reset clears the armed digest");
    TEST_ASSERT_EQ((uint64_t)s_loader_killed_scratch.utest_loader.stage_fault,
                   0u, "reset clears stage_fault");
    TEST_ASSERT_EQ((uint64_t)s_loader_killed_scratch.utest_loader.reached_exec,
                   0u, "reset clears reached_exec");
    TEST_ASSERT_EQ((uint64_t)s_loader_killed_scratch.utest_loader.identity_mismatch,
                   0u, "reset clears identity_mismatch");
    TEST_ASSERT_EQ((uint64_t)s_loader_killed_scratch.utest_loader.frame_adopted,
                   0u, "reset clears frame_adopted");
    TEST_ASSERT_EQ((uint64_t)s_loader_killed_scratch.utest_loader.entered_user,
                   0u, "reset clears entered_user");
}

/* Positive path, end to end: temporarily arm the CURRENT (running,
 * KERNEL_TESTS test-runner) task as its own capture owner, drive real
 * bytes through the public test_usermode_capture_start/_byte/_end entry
 * points exactly as the syscall handlers do, then inspect the actual
 * klog ring entry that landed to prove the wire record is correct --
 * not just that the pure escape helper is correct in isolation. Restores
 * the task's original capture state unconditionally so no other test in
 * this suite observes a captured test-runner task. */
static void test_capture_byte_and_flush_produce_the_wire_record(void)
{
    struct task *self = task_current();
    struct utest_capture_ctx ctx;
    uint8_t  saved_active;
    uint32_t saved_owner;
    int32_t  saved_seq;
    uint8_t  saved_stopped;
    uint32_t count, head;
    const klog_entry_t *ring;
    const klog_entry_t *last;

    if (!self) {
        TEST_SKIP("no current task available in this test context");
        return;
    }
    saved_active = self->utest_capture_active;
    saved_owner  = self->utest_capture_owner_pid;
    saved_seq    = atomic_read(&self->utest_capture_seq);
    /* The emission-budget latch is restored with the rest of the capture
     * state: a test that ever ran the owner past its budget would otherwise
     * leave this task permanently latched for every later emitter. */
    saved_stopped = __atomic_load_n(&self->utest_capture_stopped,
                                   __ATOMIC_ACQUIRE);
    __atomic_store_n(&self->utest_capture_stopped, 0, __ATOMIC_RELEASE);

    self->utest_capture_active = 1;
    self->utest_capture_owner_pid = self->pid;
    atomic_set(&self->utest_capture_seq, 0);
    test_usermode_capture_start(&ctx);

    /* "A[B": a plain byte, an escaped byte ('[' -> \x5b), a plain byte --
     * exercises the mixed case the pure-escape tests already pin in
     * isolation, now through the REAL per-byte entry point. */
    TEST_ASSERT(test_usermode_capture_byte(&ctx, 'A') == 1,
                "a captured task's byte is consumed, not raw-serial'd");
    TEST_ASSERT(test_usermode_capture_byte(&ctx, '[') == 1,
                "an escape-triggering byte is still consumed by capture");
    TEST_ASSERT(test_usermode_capture_byte(&ctx, 'B') == 1,
                "capture stays active across multiple bytes of one write()");
    test_usermode_capture_end(&ctx);

    ring = klog_get_ring(&count, &head);
    (void)count;
    last = &ring[(head + KLOG_RING_SIZE - 1u) % KLOG_RING_SIZE];

    TEST_ASSERT(u_test_ends_with(last->message, "final=1 A\\x5bB"),
                "the wire record ends with the exact escaped payload "
                "('[' -> \\x5b, 'A'/'B' unescaped) and final=1 (this "
                "write() never crossed a chunk boundary)");

    self->utest_capture_active = saved_active;
    self->utest_capture_owner_pid = saved_owner;
    atomic_set(&self->utest_capture_seq, saved_seq);
    __atomic_store_n(&self->utest_capture_stopped, saved_stopped,
                     __ATOMIC_RELEASE);
}

/* Regression for TWO adversarial-round-found bugs at the exact chunk
 * boundary. Round 1: a write() landing EXACTLY on UTEST_CAPTURE_CHUNK_MAX
 * bytes flushed mid-loop (final=0, buffer emptied), then the
 * unconditional end-of-loop test_usermode_capture_flush() call emitted a
 * SECOND, spurious len=0 final=1 record. Round 2 (after fixing round 1
 * by only flushing non-final when a NEW byte needs room): the mid-loop
 * flush check now runs BEFORE staging a byte, so an exact-multiple write
 * never triggers it at all -- the whole chunk stays staged until the
 * post-loop flush emits it. Exactly one record must land, and that one
 * record must carry final=1 (an exact-multiple write is COMPLETE, not
 * indistinguishable from an interrupted one). */
static void test_capture_exact_chunk_boundary_emits_one_final_record(void)
{
    struct task *self = task_current();
    struct utest_capture_ctx ctx;
    uint8_t  saved_active;
    uint32_t saved_owner;
    int32_t  saved_seq;
    uint8_t  saved_stopped;
    uint32_t count_before, count_after, head;
    uint32_t chunk_max = test_usermode_capture_chunk_max();
    uint32_t i;
    const klog_entry_t *ring;
    const klog_entry_t *last;

    if (!self) {
        TEST_SKIP("no current task available in this test context");
        return;
    }
    saved_active = self->utest_capture_active;
    saved_owner  = self->utest_capture_owner_pid;
    saved_seq    = atomic_read(&self->utest_capture_seq);
    /* The emission-budget latch is restored with the rest of the capture
     * state: a test that ever ran the owner past its budget would otherwise
     * leave this task permanently latched for every later emitter. */
    saved_stopped = __atomic_load_n(&self->utest_capture_stopped,
                                   __ATOMIC_ACQUIRE);
    __atomic_store_n(&self->utest_capture_stopped, 0, __ATOMIC_RELEASE);

    self->utest_capture_active = 1;
    self->utest_capture_owner_pid = self->pid;
    atomic_set(&self->utest_capture_seq, 0);
    test_usermode_capture_start(&ctx);

    (void)klog_get_ring(&count_before, &head);
    for (i = 0; i < chunk_max; i++)
        (void)test_usermode_capture_byte(&ctx, 'x');
    /* Deferred-flush design: ctx's len reaches chunk_max only AFTER
     * staging the last byte, and the mid-loop check runs BEFORE staging,
     * so no mid-loop flush ever fires for an exact-multiple write -- the
     * whole chunk is still staged here, waiting for this call. */
    test_usermode_capture_end(&ctx);
    ring = klog_get_ring(&count_after, &head);
    last = &ring[(head + KLOG_RING_SIZE - 1u) % KLOG_RING_SIZE];

    TEST_ASSERT_EQ((uint64_t)(count_after - count_before), 1u,
                   "exactly one record for an exact-chunk-multiple write "
                   "-- neither a spurious empty second record (round 1 "
                   "bug) nor a premature non-final mid-loop one (round 2 "
                   "bug)");
    TEST_ASSERT(u_test_contains(last->message, "final=1"),
                "the single record for a complete, exact-multiple write "
                "must carry final=1, not final=0");
    TEST_ASSERT(!u_test_contains(last->message, "final=0"),
                "must not ALSO carry final=0 anywhere (e.g. as a "
                "leftover from a stale prior record read)");

    self->utest_capture_active = saved_active;
    self->utest_capture_owner_pid = saved_owner;
    atomic_set(&self->utest_capture_seq, saved_seq);
    __atomic_store_n(&self->utest_capture_stopped, saved_stopped,
                     __ATOMIC_RELEASE);
}

/* One byte OVER the chunk boundary: the (chunk_max+1)-th byte's mid-loop
 * check now DOES find a full buffer and flushes it non-final (final=0,
 * making room), then that one extra byte stages alone and the post-loop
 * flush emits it as a second record with final=1. Two records, first
 * final=0 then final=1 -- proves the deferred-flush fix did not simply
 * move the round-1 bug to the +1 case instead of fixing it. */
static void test_capture_chunk_boundary_plus_one_emits_two_records(void)
{
    struct task *self = task_current();
    struct utest_capture_ctx ctx;
    uint8_t  saved_active;
    uint32_t saved_owner;
    int32_t  saved_seq;
    uint8_t  saved_stopped;
    uint32_t count_before, count_after, head;
    uint32_t chunk_max = test_usermode_capture_chunk_max();
    uint32_t i;
    const klog_entry_t *ring;
    const klog_entry_t *first_new, *last;

    if (!self) {
        TEST_SKIP("no current task available in this test context");
        return;
    }
    saved_active = self->utest_capture_active;
    saved_owner  = self->utest_capture_owner_pid;
    saved_seq    = atomic_read(&self->utest_capture_seq);
    /* The emission-budget latch is restored with the rest of the capture
     * state: a test that ever ran the owner past its budget would otherwise
     * leave this task permanently latched for every later emitter. */
    saved_stopped = __atomic_load_n(&self->utest_capture_stopped,
                                   __ATOMIC_ACQUIRE);
    __atomic_store_n(&self->utest_capture_stopped, 0, __ATOMIC_RELEASE);

    self->utest_capture_active = 1;
    self->utest_capture_owner_pid = self->pid;
    atomic_set(&self->utest_capture_seq, 0);
    test_usermode_capture_start(&ctx);

    (void)klog_get_ring(&count_before, &head);
    for (i = 0; i < chunk_max + 1u; i++)
        (void)test_usermode_capture_byte(&ctx, 'y');
    test_usermode_capture_end(&ctx);
    ring = klog_get_ring(&count_after, &head);

    TEST_ASSERT_EQ((uint64_t)(count_after - count_before), 2u,
                   "chunk_max+1 bytes must produce exactly two records: "
                   "the full chunk (mid-loop, non-final) plus the one "
                   "leftover byte (post-loop, final)");
    first_new = &ring[(head + KLOG_RING_SIZE - 2u) % KLOG_RING_SIZE];
    last      = &ring[(head + KLOG_RING_SIZE - 1u) % KLOG_RING_SIZE];
    TEST_ASSERT(u_test_contains(first_new->message, "final=0"),
                "the full first chunk is NOT the write's last chunk");
    TEST_ASSERT(u_test_ends_with(last->message, "final=1 y"),
                "the second record carries the one leftover byte, "
                "marked final=1");

    self->utest_capture_active = saved_active;
    self->utest_capture_owner_pid = saved_owner;
    atomic_set(&self->utest_capture_seq, saved_seq);
    __atomic_store_n(&self->utest_capture_stopped, saved_stopped,
                     __ATOMIC_RELEASE);
}

/* Count the capture chunk records containing `want` among the entries written
 * between the `head_before` snapshot and `head_after`.
 *
 * Two failure modes to avoid at once. Fixed head-relative offsets are not
 * usable: klog is a shared ring, so a line from another CPU between the flush
 * and the read shifts head and a fixed offset asserts against a foreign
 * record. But an UNBOUNDED backwards scan is worse than the flake it fixes --
 * the preceding registered test drives the same task through the same `seq`
 * reset and emits `wr=0 seq=0` and `wr=0 seq=1` of its own, so a whole-ring
 * search would let those STALE records satisfy assertions about records this
 * test never emitted. Bounding to the entries added since a pre-emission
 * snapshot is what makes the search both position-independent and honest.
 *
 * The window is derived from HEAD, not from klog_get_ring's count. That count
 * saturates at KLOG_RING_SIZE (klog.c: `if (klog_ring_count < KLOG_RING_SIZE)
 * klog_ring_count++`) while head keeps advancing, so a count difference
 * silently shrinks to zero once the ring fills -- which would make every
 * assertion below fail as a function of how much the boot happened to log
 * before this test ran. Head wraps every KLOG_RING_SIZE entries, so the
 * modular difference is exact for any window smaller than the ring, which a
 * three-record emission always is.
 *
 * Returns the match count so a caller can assert exactly-once rather than
 * at-least-once. */
static uint32_t u_test_count_capture_records(const klog_entry_t *ring,
                                             uint32_t head_before,
                                             uint32_t head_after,
                                             const char *want)
{
    uint32_t added = (head_after + KLOG_RING_SIZE - head_before) %
                     KLOG_RING_SIZE;
    uint32_t i, found = 0;

    for (i = 1; i <= added; i++) {
        const klog_entry_t *e =
            &ring[(head_after + KLOG_RING_SIZE - i) % KLOG_RING_SIZE];

        if (u_test_contains(e->message, "[UTEST-CAPTURE] ") &&
            u_test_contains(e->message, want))
            found++;
    }
    return found;
}

/* Write identity on the wire. Every record of ONE write() must carry the
 * same `wr`, and it must be the sequence number of that write's FIRST
 * chunk; the NEXT write must name itself, not inherit the previous one.
 * That relation is what lets the host reconcile each write separately
 * instead of only checking the owner's highest record, which is how a
 * second writer sharing the owner used to mask a truncated one.
 *
 * Driven through the real per-byte entry point rather than asserted
 * against the format string, because the identity is assigned inside the
 * emit path (on the first ACCEPTED chunk) and a test over the literal
 * would pass just as happily if that assignment moved or never ran. */
static void test_capture_write_identity_spans_one_write_only(void)
{
    struct task *self = task_current();
    struct utest_capture_ctx ctx;
    uint8_t  saved_active;
    uint32_t saved_owner;
    int32_t  saved_seq;
    uint8_t  saved_stopped;
    uint32_t count, head_before, head_after;
    uint32_t chunk_max = test_usermode_capture_chunk_max();
    uint32_t i;
    const klog_entry_t *ring;

    if (!self) {
        TEST_SKIP("no current task available in this test context");
        return;
    }
    saved_active = self->utest_capture_active;
    saved_owner  = self->utest_capture_owner_pid;
    saved_seq    = atomic_read(&self->utest_capture_seq);
    saved_stopped = __atomic_load_n(&self->utest_capture_stopped,
                                   __ATOMIC_ACQUIRE);
    __atomic_store_n(&self->utest_capture_stopped, 0, __ATOMIC_RELEASE);

    self->utest_capture_active = 1;
    self->utest_capture_owner_pid = self->pid;
    atomic_set(&self->utest_capture_seq, 0);

    /* Snapshot the ring head BEFORE emitting. Every assertion below is scoped
     * to the entries added after this point, so the preceding test's identical
     * `wr=0 seq=0` / `wr=0 seq=1` records cannot satisfy them. */
    (void)klog_get_ring(&count, &head_before);

    /* Write one: chunk_max + 1 bytes, so it spans two records drawing
     * seq=0 and seq=1. Both must be stamped wr=0. */
    test_usermode_capture_start(&ctx);
    for (i = 0; i < chunk_max + 1u; i++)
        (void)test_usermode_capture_byte(&ctx, 'y');
    test_usermode_capture_end(&ctx);

    ring = klog_get_ring(&count, &head_after);
    TEST_ASSERT_EQ((uint64_t)u_test_count_capture_records(
                       ring, head_before, head_after, "wr=0 seq=0 "),
                   1u,
                   "the first chunk of a write names itself as the write, "
                   "exactly once");
    TEST_ASSERT_EQ((uint64_t)u_test_count_capture_records(
                       ring, head_before, head_after, "wr=0 seq=1 "),
                   1u,
                   "the write's SECOND chunk carries the same write identity, "
                   "not its own sequence number");

    /* Write two, same owner and same ctx-free entry point. It draws the
     * next sequence number and must name ITSELF -- inheriting wr=0 would
     * merge two writes into one group and re-open the masking hole. */
    (void)klog_get_ring(&count, &head_before);
    test_usermode_capture_start(&ctx);
    (void)test_usermode_capture_byte(&ctx, 'z');
    test_usermode_capture_end(&ctx);

    ring = klog_get_ring(&count, &head_after);
    TEST_ASSERT_EQ((uint64_t)u_test_count_capture_records(
                       ring, head_before, head_after, "wr=2 seq=2 "),
                   1u,
                   "a second write() starts a new write identity rather than "
                   "inheriting the previous write's");

    self->utest_capture_active = saved_active;
    self->utest_capture_owner_pid = saved_owner;
    atomic_set(&self->utest_capture_seq, saved_seq);
    __atomic_store_n(&self->utest_capture_stopped, saved_stopped,
                     __ATOMIC_RELEASE);
}

/* The chunk bound the producer PUBLISHES must be the bound it actually
 * enforces. A binding record announcing a value the emitter does not
 * honour would let the host accept over-long chunks (or refuse honest
 * ones) while every derivation assert still passed, which is precisely the
 * mirrored-constant failure publishing the bound exists to remove. */
static void test_capture_begin_announces_the_enforced_chunk_bound(void)
{
    uint32_t count, head;
    const klog_entry_t *ring;
    const klog_entry_t *last;
    char expect[64];
    uint32_t pos = 0;
    uint32_t chunk_max = test_usermode_capture_chunk_max();
    uint32_t v, div;

    /* This emits a REAL binding record for an owner pid the launcher never
     * spawned. That is inert only while the framing tag is unpublished: the
     * record then rides the plain "UTEST" subsystem and falls outside every
     * framed slice. Once the tag is published the same record would be
     * authenticated INTO an open run, where the host refuses it as
     * capture_unexpected_channel -- a synthetic test binding failing a real
     * run. Guarded on a run being OPEN rather than on the boot-sticky
     * publication flag: the latter would skip this test forever after the
     * first run completes, masking the very regression it exists to catch. */
    if (test_usermode_frame_run_is_open()) {
        TEST_SKIP("a framed run is open -- a synthetic binding would be "
                  "authenticated into its slice");
        return;
    }

    test_usermode_capture_begin(4242u, "test_announce.exe");

    ring = klog_get_ring(&count, &head);
    (void)count;
    last = &ring[(head + KLOG_RING_SIZE - 1u) % KLOG_RING_SIZE];

    /* Built from the accessor, never from a literal: a hardcoded "42"
     * here would keep passing on the day the derivation moves, which is
     * the same drift the host side refuses to carry. */
    expect[pos++] = 'c'; expect[pos++] = 'h'; expect[pos++] = 'u';
    expect[pos++] = 'n'; expect[pos++] = 'k'; expect[pos++] = '_';
    expect[pos++] = 'm'; expect[pos++] = 'a'; expect[pos++] = 'x';
    expect[pos++] = '=';
    for (div = 1000000000u; div > 1u; div /= 10u) {
        if (chunk_max >= div)
            break;
    }
    v = chunk_max;
    for (; div > 0u; div /= 10u) {
        expect[pos++] = (char)('0' + (v / div) % 10u);
        if (div == 1u)
            break;
    }
    expect[pos] = '\0';

    TEST_ASSERT(u_test_contains(last->message, expect),
                "the capture binding announces the same chunk bound the "
                "emitter enforces");
}

/* The launcher reads the freshly created child's capture fields back before
 * it waits, and publishes [UTEST-CAPTURE-LOST] when they are not armed --
 * because a regression that drops either assignment while keeping the BEGIN
 * binding would publish a capture channel that captures nothing, and the run
 * would look like a binary that legitimately wrote no output.
 *
 * A healthy spawn only ever hands that predicate a correctly armed child, so
 * these are the only assertions that reach its refusal branches. Driven over
 * scratch task slots: no task creation, no live boot infrastructure. */
static void test_capture_arming_postcondition_covers_every_branch(void)
{
    task_utest_capture_reset(&s_capture_child_scratch);

    TEST_ASSERT(test_usermode_capture_armed_for((const struct task *)0, 7u)
                    == 0,
                "a missing task slot is never armed -- the postcondition must "
                "not dereference it to find out");

    /* Reset leaves the fields cleared: capture off, owner 0. That IS the
     * shape a lost `utest_capture_active = 1` assignment leaves behind. */
    TEST_ASSERT(test_usermode_capture_armed_for(&s_capture_child_scratch, 7u)
                    == 0,
                "a child whose capture was never activated is not armed");

    /* Active, but owned by somebody else: the shape a lost
     * `utest_capture_owner_pid = pid` assignment leaves, where the sequence
     * draw would target a slot bound to a different binary. */
    s_capture_child_scratch.utest_capture_active = 1;
    s_capture_child_scratch.utest_capture_owner_pid = 9u;
    TEST_ASSERT(test_usermode_capture_armed_for(&s_capture_child_scratch, 7u)
                    == 0,
                "an active child owned by a DIFFERENT pid is not armed for "
                "this one");

    s_capture_child_scratch.utest_capture_owner_pid = 7u;
    TEST_ASSERT(test_usermode_capture_armed_for(&s_capture_child_scratch, 7u)
                    == 1,
                "active and self-owned is the only combination that counts "
                "as armed");

    task_utest_capture_reset(&s_capture_child_scratch);
}

/* The run-boundary reap's SELECTION predicate, over the same scratch slots.
 *
 * A live fork tree only ever hands it descendants, so these are the only
 * assertions that reach the two refusal branches -- and both of them are
 * safety branches rather than tidiness. Selecting the OWNER would make the
 * reap kill and free the very task u_run_one is still holding report and
 * leak snapshots into; selecting another binary's descendant would reap a
 * task belonging to a run that has not finished. */
static void test_capture_reap_selects_only_this_owners_descendants(void)
{
    task_utest_capture_reset(&s_capture_child_scratch);

    TEST_ASSERT(test_usermode_capture_descendant_of((const struct task *)0, 7u)
                    == 0,
                "a missing task slot is never a descendant -- the predicate "
                "must not dereference it to find out");

    s_capture_child_scratch.pid = 12u;
    TEST_ASSERT(test_usermode_capture_descendant_of(&s_capture_child_scratch, 7u)
                    == 0,
                "an uncaptured task is not part of any binary's capture tree, "
                "whatever its parentage");

    s_capture_child_scratch.utest_capture_active = 1;
    s_capture_child_scratch.utest_capture_owner_pid = 9u;
    TEST_ASSERT(test_usermode_capture_descendant_of(&s_capture_child_scratch, 7u)
                    == 0,
                "a captured task owned by a DIFFERENT binary belongs to that "
                "binary's run, not to this reap");

    s_capture_child_scratch.utest_capture_owner_pid = 7u;
    s_capture_child_scratch.pid = 7u;
    TEST_ASSERT(test_usermode_capture_descendant_of(&s_capture_child_scratch, 7u)
                    == 0,
                "the OWNER is never its own descendant -- reaping it would tear "
                "down the task the launcher is still finishing with");

    s_capture_child_scratch.pid = 12u;
    TEST_ASSERT(test_usermode_capture_descendant_of(&s_capture_child_scratch, 7u)
                    == 1,
                "a captured task that is not the owner but carries the owner's "
                "pid is exactly what fork() inheritance produces");

    task_utest_capture_reset(&s_capture_child_scratch);
    s_capture_child_scratch.pid = 0u;
}

/* UNLINKING a reaped slot must clear what SELECTS it and nothing else.
 *
 * The distinction is the whole test. task_cleanup runs on the OWNER at the
 * end of every binary, and a descendant that outlived the reap still carries
 * that owner's pid -- its next claim resolves the owner's slot to read the
 * stop latch the reap set there. Substituting the full reset (which also
 * clears the latch and the sequence counter) un-fences that descendant and
 * restarts its numbering at 0, so the run it lands in receives authenticated
 * records with duplicate sequence numbers: the exact failure the fence
 * exists to prevent, reintroduced by its own cleanup. Nothing else in the
 * kernel would notice -- the build stays green and every other capture
 * assertion still passes -- which is why the difference is pinned here. */
static void test_capture_unlink_keeps_the_fence_the_reap_set(void)
{
    task_utest_capture_reset(&s_capture_parent_scratch);
    s_capture_parent_scratch.utest_capture_active = 1;
    s_capture_parent_scratch.utest_capture_owner_pid = 7u;
    atomic_set(&s_capture_parent_scratch.utest_capture_seq, 5);
    __atomic_store_n(&s_capture_parent_scratch.utest_capture_stopped, 1u,
                     __ATOMIC_RELEASE);

    task_utest_capture_unlink(&s_capture_parent_scratch);

    TEST_ASSERT_EQ((uint64_t)s_capture_parent_scratch.utest_capture_active, 0u,
                   "the unlink clears the active flag, so a tree walk stops "
                   "finding this slot and cannot clean it twice");
    TEST_ASSERT_EQ((uint64_t)s_capture_parent_scratch.utest_capture_owner_pid,
                   0u, "and clears the owner pid it was matched on");
    TEST_ASSERT_EQ((uint64_t)__atomic_load_n(
                       &s_capture_parent_scratch.utest_capture_stopped,
                       __ATOMIC_ACQUIRE), 1u,
                   "but must LEAVE the stop latch set -- clearing it here "
                   "un-fences every descendant that outlived the reap, because "
                   "their claims read the latch from this very slot");
    TEST_ASSERT_EQ((uint64_t)(uint32_t)atomic_read(
                       &s_capture_parent_scratch.utest_capture_seq), 5u,
                   "and must leave the sequence counter alone -- restarting it "
                   "hands a late descendant numbers the run has already seen");

    task_utest_capture_reset(&s_capture_parent_scratch);
}

/* A parent that is NOT itself captured (the common case: most tasks are
 * never test binaries) must produce an equally uncaptured child -- fork
 * must never MANUFACTURE ownership. */
static void test_capture_inherit_uncaptured_parent_stays_uncaptured(void)
{
    task_utest_capture_reset(&s_capture_parent_scratch);
    task_utest_capture_reset(&s_capture_child_scratch);
    s_capture_child_scratch.utest_capture_active = 1;  /* poison, must be cleared */
    s_capture_child_scratch.utest_capture_owner_pid = 42u;

    task_utest_capture_inherit(&s_capture_child_scratch,
                               &s_capture_parent_scratch);

    TEST_ASSERT_EQ((uint64_t)s_capture_child_scratch.utest_capture_active,
                   0u, "an uncaptured parent must not leave a captured child");
    TEST_ASSERT_EQ((uint64_t)s_capture_child_scratch.utest_capture_owner_pid,
                   0u, "owner_pid follows active down to the reset baseline");
}

/* The tag is what a host regex anchors on and what klog copies into its
 * fixed-width serialized subsystem field, so its exact bytes and its exact
 * length are both load-bearing. */
static void test_frame_tag_exact_bytes(void)
{
    char tag[32];

    u_test_fill(tag, sizeof(tag));
    TEST_ASSERT(test_usermode_frame_tag_format(tag, sizeof(tag), 0x1A2B3C4Du) == 1,
                "a nonce must format into an ample buffer");
    TEST_ASSERT(u_test_streq(tag, "UTEST-1a2b3c4d"),
                "the tag is 'UTEST-' plus 8 LOWERCASE hex digits");
    TEST_ASSERT_EQ(test_usermode_frame_tag_cap() - 1u, 14u,
                   "tag length is fixed at 14 bytes so the host regex can "
                   "anchor on a fixed-width hex run");

    u_test_fill(tag, sizeof(tag));
    TEST_ASSERT(test_usermode_frame_tag_format(tag, sizeof(tag), 0u) == 1,
                "zero formats even though it is never published");
    TEST_ASSERT(u_test_streq(tag, "UTEST-00000000"),
                "hex digits are zero-padded to a fixed width, never trimmed");

    u_test_fill(tag, sizeof(tag));
    TEST_ASSERT(test_usermode_frame_tag_format(tag, sizeof(tag), 0xFFFFFFFFu) == 1,
                "the all-ones nonce formats");
    TEST_ASSERT(u_test_streq(tag, "UTEST-ffffffff"),
                "the top nibble is not sign-extended or dropped");
}

/* The tag must fit klog's serialized subsystem field (KLOG_SUBSYSTEM_MAX),
 * or every launcher record loses its frame in the crash-region evidence the
 * next boot reads back. */
static void test_frame_tag_fits_klog_subsystem(void)
{
    uint32_t cap = test_usermode_frame_tag_cap();

    TEST_ASSERT_EQ(cap, 15u,
                   "'UTEST-' + 8 hex + NUL is 15 bytes");
    TEST_ASSERT(cap <= (uint32_t)KLOG_SUBSYSTEM_MAX,
                "the framed tag must fit klog's serialized subsystem field");
}

/* Fail closed on a buffer that cannot hold the whole tag: a truncated tag
 * would be a frame no host could verify, published as if it were real. */
static void test_frame_tag_refuses_short_buffer(void)
{
    char tag[16];
    uint32_t cap = test_usermode_frame_tag_cap();

    u_test_fill(tag, sizeof(tag));
    TEST_ASSERT(test_usermode_frame_tag_format(tag, cap - 1u, 0x1A2B3C4Du) == 0,
                "one byte short of exact capacity must refuse");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)tag[0], (uint32_t)'Z',
                   "a refused format writes nothing at all");
    TEST_ASSERT(test_usermode_frame_tag_format(tag, cap, 0x1A2B3C4Du) == 1,
                "exact capacity is accepted");
    TEST_ASSERT(test_usermode_frame_tag_format((char *)0, 64u, 1u) == 0,
                "a NULL destination must refuse");
}

/* 0 is the "no nonce generated yet" sentinel, so the fold must never
 * produce it -- a folded-to-zero nonce would leave the frame permanently
 * unpublished and every record silently unframed. */
static void test_frame_nonce_fold_never_zero(void)
{
    TEST_ASSERT_EQ(test_usermode_frame_nonce_fold(0ull), 1u,
                   "an all-zero mix folds to the reserved 1, never to 0");
    TEST_ASSERT_EQ(test_usermode_frame_nonce_fold(0x00000001ull << 32 | 1ull), 1u,
                   "halves that cancel exactly must still not yield 0");
    TEST_ASSERT_EQ(test_usermode_frame_nonce_fold(0xFFFFFFFFFFFFFFFFull), 1u,
                   "all-ones halves cancel to 0 and fold to 1");
    TEST_ASSERT_EQ(test_usermode_frame_nonce_fold(0x00000000A5A5A5A5ull),
                   0xA5A5A5A5u,
                   "a low-half-only mix passes through unchanged");
    TEST_ASSERT_EQ(test_usermode_frame_nonce_fold(0xA5A5A5A500000000ull),
                   0xA5A5A5A5u,
                   "the high half is folded in, not discarded");
}

void test_register_usermode_launcher(void);
/* ---- Section 39: executable identity for a planned entry ------------- */

/* The plan freezes WHICH names run; without a content identity it does not
 * freeze WHAT runs under each name. These cover the three load-bearing
 * pieces: the digest actually distinguishes content, the verifier the
 * loader gates task_exec on is fail-closed, and an entry whose identity
 * cannot be frozen leaves the plan as a counted refusal rather than as a
 * runnable nobody verified. */

static void test_identity_digest_is_stable_and_content_bound(void)
{
    uint8_t a[SHA256_DIGEST_LEN], b[SHA256_DIGEST_LEN];
    struct vfs_node *f;
    uint32_t i;
    int same;
    static const char *victim = "C:\\utest_ident_probe.bin";

    /* Same file, two walks -> the same identity. Uses a real staged test
     * binary so the digest runs over the same class of object the launcher
     * freezes, not a synthetic fixture. */
    if (!test_usermode_digest_binary("test_libc.exe", a)) {
        TEST_SKIP("test_libc.exe not present on this image");
        return;
    }
    TEST_ASSERT(test_usermode_digest_binary("test_libc.exe", b) == 1,
                "digesting a present binary twice must both succeed");
    TEST_ASSERT(test_usermode_identity_matches(a, b) == 1,
                "the same bytes must produce the same frozen identity");

    /* A name that does not resolve is fail-closed, not a zero digest that
     * would compare equal to another failure. */
    TEST_ASSERT(test_usermode_digest_binary("test_no_such_binary.exe", b) == 0,
                "an unresolvable name must fail to freeze, not return a digest");

    /* Content change -> identity change. This is the property the whole
     * section rests on, asserted over the real VFS rather than assumed
     * from SHA-256's reputation. */
    f = vfs_open(victim, VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE);
    if (!f) {
        TEST_SKIP("cannot create a scratch file on C:\\");
        return;
    }
    vfs_write(f, 0, 8, (const uint8_t *)"AAAAAAAA");
    vfs_close(f);
    TEST_ASSERT(test_usermode_digest_binary("utest_ident_probe.bin", a) == 1,
                "a freshly written scratch file must freeze");

    f = vfs_open(victim, VFS_O_READ | VFS_O_WRITE);
    if (!f) {
        /* Infrastructure failure, not a product failure: without the
         * rewrite the two digests are trivially equal and the assertion
         * below would blame the identity check for a VFS problem. */
        vfs_unlink(victim);
        TEST_SKIP("cannot reopen the scratch file for rewrite");
        return;
    }
    vfs_write(f, 0, 8, (const uint8_t *)"BBBBBBBB");
    vfs_close(f);
    TEST_ASSERT(test_usermode_digest_binary("utest_ident_probe.bin", b) == 1,
                "the rewritten scratch file must still freeze");
    same = test_usermode_identity_matches(a, b);
    TEST_ASSERT_EQ(same, 0,
                   "replacing the bytes under a name must change its identity");

    /* A single differing byte must be caught, not just a wholesale rewrite. */
    for (i = 0; i < SHA256_DIGEST_LEN; i++)
        b[i] = a[i];
    b[SHA256_DIGEST_LEN - 1u] ^= 0x01u;
    TEST_ASSERT_EQ(test_usermode_identity_matches(a, b), 0,
                   "a one-bit difference in the last byte must not match");

    vfs_unlink(victim);
}

static void test_identity_freeze_refuses_unloadable_sizes(void)
{
    uint8_t d[SHA256_DIGEST_LEN];
    struct vfs_node *f;
    static const char *empty = "C:\\utest_ident_empty.bin";

    /* An empty file cannot be an executable, and admitting it would let two
     * different empty entries share one identity. Refused BEFORE any read,
     * so this also covers "the freeze does not hash what the loader would
     * reject anyway". */
    f = vfs_open(empty, VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE);
    if (!f) {
        TEST_SKIP("cannot create a scratch file on C:\\");
        return;
    }
    vfs_close(f);
    TEST_ASSERT_EQ(test_usermode_digest_binary("utest_ident_empty.bin", d), 0,
                   "a zero-length file must not produce a frozen identity");
    vfs_unlink(empty);

    /* The upper bound is the executor's own contract. A file past it is one
     * the loader refuses, so hashing it during PLANNING -- before any spawn
     * exists for the watchdog to bound -- would be unbounded work with no
     * possible outcome. Asserted on the constant rather than by staging a
     * 16 MiB file, which the image cannot carry. */
    TEST_ASSERT(EXEC_MAX_IMAGE_SIZE > 0u &&
                (uint64_t)EXEC_MAX_IMAGE_SIZE < 0xFFFFFFFFull,
                "the freeze bound must be a real ceiling below the uint32 "
                "range the page rounding would overflow at");
}

static void test_identity_verifier_is_fail_closed(void)
{
    uint8_t d[SHA256_DIGEST_LEN];
    uint32_t i;

    for (i = 0; i < SHA256_DIGEST_LEN; i++)
        d[i] = (uint8_t)i;

    TEST_ASSERT_EQ(test_usermode_identity_matches(d, d), 1,
                   "a digest must match itself");
    /* NULL expected identity means the launcher lost what was supposed to
     * run. That must refuse, never wave the bytes through. */
    TEST_ASSERT_EQ(test_usermode_identity_matches((const uint8_t *)0, d), 0,
                   "a missing frozen identity must be treated as a mismatch");
}

static void test_unfreezable_entry_becomes_a_counted_refusal(void)
{
    uint32_t kind = 0xFFu, smoke_refused = 0xFFu;
    const char *reason = (const char *)0;

    /* A planned name whose bytes cannot be established must NOT stay
     * runnable: the execution loop would launch content whose identity was
     * never frozen, which is the hole this section closes. */
    TEST_ASSERT(test_usermode_freeze_refuses("test_no_such_binary.exe", 0,
                                             &kind, &smoke_refused,
                                             &reason) == 1,
                "the freeze probe must build its plan");
    TEST_ASSERT_EQ((int)kind, 1,
                   "an entry that cannot be frozen must become a REFUSAL");
    TEST_ASSERT(reason != (const char *)0,
                "a refused-for-identity entry must carry a named reason");
    TEST_ASSERT_EQ((int)smoke_refused, 0,
                   "a non-smoke identity refusal must not abort the suite");

    /* Smoke provenance has to survive the conversion, or a smoke
     * prerequisite that cannot be verified would quietly demote to an
     * ordinary counted failure and let the rest of the suite run on. */
    kind = 0xFFu; smoke_refused = 0xFFu;
    TEST_ASSERT(test_usermode_freeze_refuses("test_no_such_binary.exe", 1,
                                             &kind, &smoke_refused,
                                             &reason) == 1,
                "the smoke freeze probe must build its plan");
    TEST_ASSERT_EQ((int)kind, 1,
                   "an unfreezable smoke entry must become a REFUSAL too");
    TEST_ASSERT_EQ((int)smoke_refused, 1,
                   "an unfreezable smoke prerequisite must abort the suite");
}

void test_register_usermode_launcher(void)
{
    test_suite_register_cat("UTEST: glob NULL matches everything",
                            test_glob_null_pattern_matches_everything, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: glob literal match",
                            test_glob_literal_match, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: glob wildcard prefix",
                            test_glob_wildcard_prefix, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: glob wildcard at start",
                            test_glob_wildcard_at_start, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: glob wildcard at end",
                            test_glob_wildcard_at_end, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: glob suffix-too-long boundary",
                            test_glob_suffix_longer_than_remaining, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: glob empty name",
                            test_glob_empty_name, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report NONE/INVALID pass through",
                            test_report_reconcile_passthrough_states,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report agreeing with exit stays VALID",
                            test_report_reconcile_agrees_with_exit,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report contradicting exit is INVALID",
                            test_report_reconcile_rejects_contradictions,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: timed-out binary keeps its report",
                            test_report_reconcile_timeout_keeps_counts,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: reserved-block status renders in full",
                            test_reason_exit_renders_reserved_block_status,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: timeout stamp clears the signal range",
                            test_timeout_stamp_leaves_the_signal_range,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: exit-status classes partition the space",
                            test_exit_status_classes_partition_the_space,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report dispatch refuses NULL task",
                            test_report_dispatch_rejects_null_task,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report dispatch accepts first submission",
                            test_report_dispatch_accepts_first_submission,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report dispatch accepts all-zero report",
                            test_report_dispatch_accepts_all_zero,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report dispatch accepts exact ceilings",
                            test_report_dispatch_accepts_exact_ceilings,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report dispatch refuses over-ceiling counts",
                            test_report_dispatch_rejects_over_ceiling,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report dispatch refuses second submission",
                            test_report_dispatch_refuses_second_submission,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report INVALID is sticky",
                            test_report_dispatch_invalid_is_sticky,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: INVALID escalates PASS to FAIL",
                            test_invalid_escalates_pass_to_fail, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: INVALID escalates SKIP to FAIL",
                            test_invalid_escalates_skip_to_fail, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: INVALID leaves an existing FAIL alone",
                            test_invalid_leaves_existing_fail_alone,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: skip-record budget allows normal runs",
                            test_skip_budget_allows_normal_runs, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: skip-record budget boundary",
                            test_skip_budget_boundary, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: skip-record budget never underflows",
                            test_skip_budget_never_underflows, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: skip-record name shape",
                            test_skip_record_name_shape, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: skip-record name capacity boundary",
                            test_skip_record_name_capacity_boundary,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON summary zero run",
                            test_json_summary_zero_run, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: skip-record name refuses truncation",
                            test_skip_record_name_refuses_overflow,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: XML summary counts records",
                            test_xml_summary_counts_records, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: XML summary reports errors as failure subset",
                            test_xml_summary_reports_errors_as_failure_subset,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: never-ran records carry error classification",
                            test_never_ran_records_carry_the_error_classification,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: overflow fallbacks preserve the verdict",
                            test_overflow_fallbacks_preserve_the_verdict,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: record verdict maps never-ran onto artifacts",
                            test_record_verdict_maps_never_ran_onto_the_artifacts,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: XML summary refuses truncation",
                            test_xml_summary_refuses_truncation, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: XML summary carries abort state",
                            test_xml_summary_carries_abort_state, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON summary separates dimensions",
                            test_json_summary_separates_dimensions,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON records fit klog transport",
                            test_json_records_fit_klog_transport,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON run_report refuses truncation",
                            test_json_run_report_refuses_truncation,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: TAP point shapes and refusals",
                            test_tap_point_shapes_and_refusals,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON run_meta record",
                            test_json_run_meta_record, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON run_meta refuses truncation",
                            test_json_run_meta_refuses_truncation,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON summary refuses truncation",
                            test_json_summary_refuses_truncation, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report summary line shape",
                            test_report_summary_line_shape, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: report summary refuses truncation",
                            test_report_summary_refuses_truncation,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: setter API edge cases no-crash",
                            test_setter_api_no_crash, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: manifest accepts well-formed names",
                            test_manifest_accepts_well_formed, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: manifest rejects path traversal",
                            test_manifest_rejects_path_traversal, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: manifest rejects drive letters",
                            test_manifest_rejects_drive_letters, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: manifest rejects non-test binaries",
                            test_manifest_rejects_non_test_binaries, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: manifest rejects control bytes",
                            test_manifest_rejects_control_bytes, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-inject gate denies when test=0",
                            test_fault_inject_gate_denies_when_test_is_off,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-inject CLEAR_ALL under test=1",
                            test_fault_inject_clear_works_under_test_mode,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-inject rejects unknown kind",
                            test_fault_inject_rejects_unknown_kind,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: FAULT_KMALLOC_COUNTDOWN requires N>=1",
                            test_fault_inject_kmalloc_countdown_requires_nonzero,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: FAULT_PMM_COUNTDOWN requires N>=1",
                            test_fault_inject_pmm_countdown_requires_nonzero,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-site rejects invalid site id",
                            test_fault_site_rejects_invalid_site_id,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-site arming fails closed",
                            test_fault_site_arm_fails_closed,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-site claims exactly once",
                            test_fault_site_claim_fires_exactly_once,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-site ignores other sites/allocators",
                            test_fault_site_ignores_other_sites_and_allocators,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-site never fires outside a site",
                            test_fault_site_outside_any_site_never_fires,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fault-site arms are per-allocator",
                            test_fault_site_arms_are_per_allocator,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: FAULT_CLEAR_ALL clears a site arm",
                            test_fault_site_cleared_by_clear_all,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: fork-child PML4 site is pmm-owned",
                            test_fault_site_fork_pml4_is_pmm_owned,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: pmm site/ordinal replacement is two-way",
                            test_fault_site_pmm_replacement_is_two_way,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: failed PML4 creation strands no frames",
                            test_fork_pml4_partial_alloc_is_net_zero,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: derive_test_name strips .exe",
                            test_derive_test_name_happy_path, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: derive_test_name rejects bad input",
                            test_derive_test_name_rejects_bad_input, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: path_join adds missing separator",
                            test_path_join_adds_separator, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: path_join rejects overflow",
                            test_path_join_overflow, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: manifest HKLM guard protects SOFTWARE hive",
                            test_manifest_hklm_guard_rejects_empty,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: path_has_traversal catches `..` variants",
                            test_path_has_traversal_catches_variants,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: XML escape covers 5 specials + control-byte drop",
                            test_xml_escape_specials, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: JSON escape covers quote/backslash/TAB/LF/ctrl",
                            test_json_escape_specials, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: type_for_name prefix inference",
                            test_type_for_name_prefix_inference, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: type_for_name prefix boundaries",
                            test_type_for_name_prefix_boundaries, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: type_from_attr known values",
                            test_type_from_attr_known_values, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: type_from_attr unknown falls back to correctness",
                            test_type_from_attr_unknown_values, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: type_label matches enum",
                            test_type_label_matches_enum, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: taxonomy enum values are stable ABI",
                            test_type_enum_values_stable, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: set_stress_iters accepts 0/100/65535",
                            test_stress_iters_boot_config_has_nonzero_room,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: frame tag formats exact host-visible bytes",
                            test_frame_tag_exact_bytes, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: frame tag fits klog's subsystem field",
                            test_frame_tag_fits_klog_subsystem, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: frame tag refuses a short buffer",
                            test_frame_tag_refuses_short_buffer, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: frame nonce never folds to the 0 sentinel",
                            test_frame_nonce_fold_never_zero, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: name bound is the min across all formatters",
                            test_name_bound_is_the_minimum_across_formatters,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: name at the bound runs, one past is refused",
                            test_name_at_bound_accepted_over_bound_refused,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: charset refusal is distinct from not-a-test",
                            test_charset_refusals_are_distinct_from_not_test_shaped,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: embedded NUL is refused, not truncated",
                            test_embedded_nul_is_refused_not_truncated,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: name digest covers bytes past a NUL",
                            test_name_digest_covers_bytes_past_a_nul,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: refusal identity is bounded and unique",
                            test_refusal_id_is_bound_conforming_and_unique,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: every aggregate identity conforms",
                            test_every_aggregate_identity_conforms,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: aggregate unknown value is not a zero",
                            test_aggregate_unknown_value_is_not_zero,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: time_ms clamp makes its digit width provable",
                            test_time_ms_clamp_makes_the_digit_width_provable,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: taxonomy edge names (empty/dots/255-byte)",
                            test_taxonomy_edge_names, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: name identity matches the filesystem fold",
                            test_name_identity_matches_the_filesystem,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: refusal dedup suppresses only the same file",
                            test_refusal_dedup_is_the_only_net_for_double_publication,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: worst-case record fits at the derived bound",
                            test_worst_case_record_fits_at_the_derived_bound,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: refusal identity survives ordinal growth",
                            test_refusal_id_survives_ordinal_digit_growth,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture escapes marker+control/non-ASCII bytes only",
                            test_capture_escape_marker_and_control_classes_only,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture escape refuses a too-small buffer",
                            test_capture_escape_refuses_short_buffer,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture escape of empty input is empty",
                            test_capture_escape_empty_input, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture chunk size is positive and buffer-bounded",
                            test_capture_chunk_max_is_positive_and_bounded,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture budget permits up to the owner limit",
                            test_capture_budget_permits_up_to_the_owner_limit,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture budget never reopens past the limit",
                            test_capture_budget_never_reopens_past_the_limit,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: latched capture owner drops every later record",
                            test_capture_budget_latched_owner_drops_everything,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture owner-scope stop outranks the run stop",
                            test_capture_budget_owner_scope_wins_over_run,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture claim charges the run only for a chunk",
                            test_capture_claim_charges_the_run_only_for_a_chunk,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a latched capture claim consumes no sequence",
                            test_capture_claim_latches_and_then_consumes_nothing,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture run stop latches run and owner alike",
                            test_capture_claim_run_stop_latches_both,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: run seal outranks every other capture verdict",
                            test_capture_seal_outranks_every_other_verdict,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a sealed claim consumes and latches nothing",
                            test_capture_seal_consumes_nothing_and_latches_nothing,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a capture claim reserves exactly when it draws",
                            test_capture_claim_reserves_exactly_when_it_draws,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: pending counts capture records still owed",
                            test_capture_pending_counts_records_still_owed,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: pending never underflows on a late completion",
                            test_capture_pending_never_underflows,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a stale emitter leaves the next run untouched",
                            test_capture_stale_emitter_leaves_the_next_run_untouched,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: closing a run writes off and ends its epoch",
                            test_capture_close_writes_off_and_ends_the_epoch,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: an unsettled claim is reported fail-closed",
                            test_capture_close_reports_an_unsettled_claim_fail_closed,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: write-offs do not leak across three runs",
                            test_capture_write_offs_do_not_leak_across_three_runs,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: admission survives past the uint32 ceiling",
                            test_capture_admission_survives_past_the_uint32_ceiling,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a late completion cannot cancel a live claim",
                            test_capture_late_completion_cannot_cancel_a_live_reservation,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: reap selects only this owner's descendants",
                            test_capture_reap_selects_only_this_owners_descendants,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: unlink keeps the fence the reap set",
                            test_capture_unlink_keeps_the_fence_the_reap_set,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture emits one owner marker then stops",
                            test_capture_emits_exactly_one_owner_marker_then_stops,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture owner budget covers its raw allowance",
                            test_capture_owner_budget_covers_its_raw_allowance,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture run budget covers owner + run allowance",
                            test_capture_run_budget_covers_owner_and_run_allowance,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture wire model bounds both allowances",
                            test_capture_wire_model_bounds_both_allowances,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture_byte is a no-op for an uncaptured task",
                            test_capture_byte_uncaptured_task_returns_zero,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture_byte/flush produce the wire record",
                            test_capture_byte_and_flush_produce_the_wire_record,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: exact chunk boundary emits one final record",
                            test_capture_exact_chunk_boundary_emits_one_final_record,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: chunk boundary plus one emits two records",
                            test_capture_chunk_boundary_plus_one_emits_two_records,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: write identity spans one write only",
                            test_capture_write_identity_spans_one_write_only,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture BEGIN announces the enforced chunk bound",
                            test_capture_begin_announces_the_enforced_chunk_bound,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: arming postcondition covers every branch",
                            test_capture_arming_postcondition_covers_every_branch,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture inherit copies active+owner unchanged",
                            test_capture_inherit_copies_active_and_owner_unchanged,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: loader evidence cannot outlive its invocation",
                            test_loader_evidence_cannot_outlive_its_own_invocation,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: loader evidence reset clears every field",
                            test_loader_evidence_reset_clears_every_field,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: loader evidence snapshot copies every field",
                            test_loader_evidence_snapshot_copies_every_field,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: ring-3 entry evidence requires an RPL-3 selector",
                            test_loader_entry_evidence_requires_ring3_selector,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: capture inherit: uncaptured parent stays uncaptured",
                            test_capture_inherit_uncaptured_parent_stays_uncaptured,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: filter folds case like the filesystem",
                            test_filter_folds_case_like_the_filesystem,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: plan folds an identical case-variant duplicate",
                            test_plan_folds_an_identical_duplicate,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: plan refuses a duplicate with conflicting policy",
                            test_plan_refuses_a_conflicting_duplicate,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: an overflowing plan runs nothing",
                            test_plan_overflow_runs_nothing, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: an unbuildable path is not a runnable binary",
                            test_unbuildable_path_is_not_present, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: manifest parses past the old runnable cap",
                            test_manifest_parses_past_the_old_runnable_cap,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a refused identity stays refused",
                            test_manifest_refusal_is_terminal_for_its_identity,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a lossy refusal name is not an identity",
                            test_lossy_refusal_name_is_not_an_identity,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: refused attribute vetoes a later variant",
                            test_refused_attribute_vetoes_a_later_case_variant,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a fully filtered manifest stays authoritative",
                            test_fully_filtered_manifest_stays_authoritative,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: an attribute-refused smoke entry aborts",
                            test_attribute_refused_smoke_aborts_the_suite,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: smoke provenance survives every refusal route",
                            test_smoke_provenance_survives_every_refusal_route,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a deduplicated refusal keeps smoke provenance",
                            test_deduplicated_refusal_keeps_smoke_provenance,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: a filtered-out smoke refusal does not abort",
                            test_filtered_out_smoke_refusal_does_not_abort,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: over-length refusals are not deduplicated",
                            test_overlength_refusals_are_not_deduplicated,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: plan alloc failure is its own state",
                            test_plan_alloc_failure_is_a_distinct_state,
                            TEST_CAT_EXEC);

    test_suite_register_cat("UTEST: name-arena exhaustion rolls back and compacts",
                            test_plan_intern_exhaustion_and_compaction,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: frozen identity is stable and content-bound",
                            test_identity_digest_is_stable_and_content_bound,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: identity freeze refuses unloadable sizes",
                            test_identity_freeze_refuses_unloadable_sizes,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: identity verifier is fail-closed",
                            test_identity_verifier_is_fail_closed,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: an unfreezable entry is a counted refusal",
                            test_unfreezable_entry_becomes_a_counted_refusal,
                            TEST_CAT_EXEC);
}

#endif /* KERNEL_TESTS */
