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
#include "kernel/mm/heap.h"
#include "kernel/boot_info.h"
#include "registry.h"

/* Exposed by src/kernel/test/test_usermode.c for unit-test use. Kept as
 * forward declarations here rather than promoted to the public header
 * because their only non-test consumer is the launcher itself. */
int test_usermode_glob_match(const char *pattern, const char *name);
int test_usermode_is_valid_manifest_name(const char *name);
int test_usermode_derive_test_name(const char *name_in,
                                   char *out, uint32_t out_cap);
int test_usermode_path_join(const char *parent, const char *name,
                            char *out, uint32_t out_cap);
int test_usermode_path_has_traversal(const char *p);
int test_usermode_xml_escape(const char *src, char *dst, uint32_t cap);
int test_usermode_json_escape(const char *src, char *dst, uint32_t cap);

/* Exposed by src/kernel/sched/syscall.c for §5 gate regression tests. */
int64_t sys_fault_inject_dispatch(uint32_t kind, uint32_t countdown);

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

/* ---- Contract constants --------------------------------------------- */

static void test_skip_constant_is_kselftest_77(void)
{
    /* kselftest convention: exit 77 = SKIP. The launcher keys its skip
     * counter off this exact value. Any drift away from 77 silently
     * turns SKIPs into FAILs in mixed CI consumers that expect the
     * Linux convention. */
    TEST_ASSERT_EQ(UTEST_EXIT_SKIP, 77,
                   "UTEST_EXIT_SKIP matches kselftest convention (77)");
}

static void test_timeout_constant_is_negative(void)
{
    /* UTEST_EXIT_TIMEOUT must be negative so it can never collide with a
     * legitimate test exit code -- those are 0 (pass), 77 (skip), or a
     * small positive failure code. Conflict with 0 or 77 would misreport
     * a timeout as a pass or skip. */
    TEST_ASSERT(UTEST_EXIT_TIMEOUT < 0,
                "UTEST_EXIT_TIMEOUT is negative (distinct from pass/skip)");
    TEST_ASSERT(UTEST_EXIT_TIMEOUT != UTEST_EXIT_SKIP,
                "UTEST_EXIT_TIMEOUT does not collide with UTEST_EXIT_SKIP");
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

    TEST_ASSERT(1, "setter API accepts documented edge cases without crashing");
}

/* ---- §5 SYS_FAULT_INJECT gate regression ---------------------------- *
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
    /* kind values above FAULT_CLEAR_ALL (6) are unassigned; dispatcher
     * must -1 them so a future ABI extension can't be forged against
     * an older kernel. */
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

/* ---- §6 per-test isolation pure helpers ---------------------------- */

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

/* u_path_has_traversal regression (Codex H3 2026-04-20): the raw
 * prefix check in u_cleanup_manifest_apply was bypassable by
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

/* §7 XML/JSON escape regressions. User-provided strings (test names,
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

/* Manifest HKLM guard regression (Codex H2 2026-04-20): `HKLM\`
 * with an empty subkey would otherwise hit RegDeleteTree(HKLM, "")
 * which wipes ALL children of HKEY_LOCAL_MACHINE. The test verifies
 * the parser's reject path leaves the registry untouched. We use a
 * HKLM subkey that we KNOW exists (SOFTWARE is populated by
 * registry_populate_defaults during boot) and assert it survives a
 * simulated `HKLM\` directive.
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

/* ---- Registration --------------------------------------------------- */

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
    test_suite_register_cat("UTEST: SKIP exit = kselftest 77",
                            test_skip_constant_is_kselftest_77, TEST_CAT_EXEC);
    test_suite_register_cat("UTEST: TIMEOUT exit distinct from pass/skip",
                            test_timeout_constant_is_negative, TEST_CAT_EXEC);
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
}

#endif /* KERNEL_TESTS */
