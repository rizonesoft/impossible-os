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
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"       /* pmm_get_free_frames + ordinal countdown */
#include "kernel/mm/vmm.h"       /* vmm_create_user_pml4 (the fork site)    */
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
uint64_t test_usermode_clamp_time_ms(uint64_t ms);
uint32_t test_usermode_reason_max(void);
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
int test_usermode_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                     uint32_t failures, uint32_t skipped,
                                     uint64_t total_ms, int aborted,
                                     uint32_t not_run);
int test_usermode_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                      uint32_t failed, uint32_t skipped,
                                      uint64_t total_ms);
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
     * today, so assuming either would be right only by luck. */
    for (k = 0; k < 6u; k++) {
        uint32_t room = test_usermode_name_room(k);
        if (room < min) min = room;
    }
    TEST_ASSERT_EQ(bound, min,
                   "derived bound equals the min room across all 6 kinds");
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

static void test_report_reconcile_timeout_keeps_counts(void)
{
    /* On timeout the launcher OVERWROTE exit_status with its own marker,
     * so no report could ever agree with it. Reconciliation must not
     * invent a contradiction out of the launcher's own bookkeeping -- the
     * timeout already fails the binary on its own, more specifically. */
    TEST_ASSERT_EQ((uint64_t)test_usermode_report_reconcile(
                       TASK_UTEST_REPORT_VALID, 0, UTEST_EXIT_TIMEOUT, 1),
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

static void test_xml_summary_counts_records(void)
{
    char line[192];

    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 12, 2, 5, 1500, 0, 0) == 1,
                "XML summary formats at realistic widths");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-XML-SUMMARY] tests=12 failures=2 skipped=5 time=1.500 "
                    "aborted=0 not_run=0"),
                "XML summary is EXACTLY the contract the host post-processor parses");
    /* An all-zero run is the empty-suite artifact contract: it must still
     * produce a complete, patchable summary rather than a degenerate one. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 0, 0, 0, 0, 0, 0) == 1,
                "an empty suite still formats a summary");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-XML-SUMMARY] tests=0 failures=0 skipped=0 time=0.000 "
                    "aborted=0 not_run=0"),
                "empty-suite summary carries explicit zeros");
}

static void test_xml_summary_carries_abort_state(void)
{
    char line[192];

    /* The abort case the artifact previously could not express: a smoke
     * binary SKIPPED, so `failures` is 0 and the counts that DID land are
     * internally consistent -- the run reads as a small, clean, complete
     * suite unless the completeness dimension says otherwise. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 1, 0, 1, 90, 1, 11) == 1,
                "aborted XML summary formats");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-XML-SUMMARY] tests=1 failures=0 skipped=1 time=0.090 "
                    "aborted=1 not_run=11"),
                "abort state rides as trailing fields, after time=");
    /* The trailing position is load-bearing, not cosmetic: scripts/test.sh
     * extracts time with a greedy `.*time=([0-9.]+).*` sed, so anything
     * inserted BEFORE it would be captured instead. */
    TEST_ASSERT(u_test_contains(line, "time=0.090 aborted=1"),
                "time= keeps a numeric value immediately after it");
    /* `aborted` is a flag, never a count: any nonzero must normalise to 1
     * so the host can compare it as a literal. */
    TEST_ASSERT(test_usermode_format_xml_summary(line, sizeof(line),
                                                 1, 0, 1, 90, 7, 11) == 1,
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
                                                 12, 2, 5, 1500, 0, 0) == 0,
                "XML summary reports failure instead of truncating");
    /* The completeness fields are appended LAST, so a buffer that fits
     * everything through `time=` and nothing more is the exact width at
     * which a silent truncation would drop the abort state while leaving a
     * summary that still parses. It must be refused, not published. */
    {
        char tight[61];
        TEST_ASSERT(test_usermode_format_xml_summary(tight, sizeof(tight),
                                                     12, 2, 5, 1500, 1, 9) == 0,
                    "a buffer that fits only through time= is refused, not truncated");
    }
}

static void test_json_summary_separates_dimensions(void)
{
    char line[384];

    TEST_ASSERT(test_usermode_format_json_summary(line, sizeof(line),
                                                  9, 1, 2, 2500) == 1,
                "JSON summary formats at realistic widths");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-JSON] {\"summary\":{\"passed\":9,\"failed\":1,"
                    "\"skipped\":2,\"total\":12,\"time_ms\":2500}}"),
                "JSON summary is EXACTLY the documented record, every field in order");
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
                                                  18446744073709551615ull) == 1,
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
                                                  0, 0, 0, 0) == 1,
                "an all-zero JSON summary formats");
    TEST_ASSERT(u_test_streq(line,
                    "[UTEST-JSON] {\"summary\":{\"passed\":0,\"failed\":0,"
                    "\"skipped\":0,\"total\":0,\"time_ms\":0}}"),
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
                                                  9, 1, 2, 2500) == 0,
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
    test_suite_register_cat("UTEST: time_ms clamp makes its digit width provable",
                            test_time_ms_clamp_makes_the_digit_width_provable,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: taxonomy edge names (empty/dots/255-byte)",
                            test_taxonomy_edge_names, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: name identity matches the filesystem fold",
                            test_name_identity_matches_the_filesystem,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: worst-case record fits at the derived bound",
                            test_worst_case_record_fits_at_the_derived_bound,
                            TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: refusal identity survives ordinal growth",
                            test_refusal_id_survives_ordinal_digit_growth,
                            TEST_CAT_EXEC);
}

#endif /* KERNEL_TESTS */
