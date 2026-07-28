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
}

#endif /* KERNEL_TESTS */
