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

/* Exposed by src/kernel/test/test_usermode.c for unit-test use. Kept as
 * forward declarations here rather than promoted to the public header
 * because their only non-test consumer is the launcher itself. */
int test_usermode_glob_match(const char *pattern, const char *name);
int test_usermode_is_valid_manifest_name(const char *name);

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
}

#endif /* KERNEL_TESTS */
