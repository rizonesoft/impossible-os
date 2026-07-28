/* ============================================================================
 * test_harness_smoke.c -- smoke binary for user/include/test.h
 *
 * Proves the assertion macro compiles, links against syscall + libc, and
 * produces the documented [UTEST-BEGIN] / [PASS] / [UTEST-END] lines.
 * Designed to PASS unconditionally so when launcher ships and picks
 * this up automatically, exit code 0 means "the harness itself is healthy".
 *
 * The FAIL-path of UTEST_ASSERT is exercised by code review of test.h
 * and by the build-integration "deliberate-fail" smoke (if added);
 * a runtime FAIL here would just trip the launcher unnecessarily.
 *
 * Linked against the same crt0 + libc as hello.exe / cmd.exe.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_harness_smoke");

    /* 1. Trivial arithmetic -- the macro fires + the message format is right. */
    UTEST_ASSERT(1 + 1 == 2, "arithmetic: 1+1==2");

    /* 2. libc string surface -- proves test.h pulls in string.h cleanly. */
    UTEST_ASSERT(strlen("hello") == 5, "strlen(\"hello\") == 5");

    /* 3. Syscall surface -- proves test.h pulls in syscall.h and a real
     *    sys_uptime() call returns a non-negative tick count. The
     *    exact value depends on boot timing; >= 0 is the only invariant
     *    we can portably assert. */
    UTEST_ASSERT(sys_uptime() >= 0, "sys_uptime() >= 0");

    /* 4. Skip path -- the harness's THIRD outcome, exercised deliberately.
     *    This binary is the only place a skip can be taken on purpose: it
     *    exists to prove the harness's own documented behaviour, and the
     *    skip-reporting path has no other unconditional producer (every
     *    real UTEST_SKIP is guarded by a precondition that is normally
     *    available, so a green run would exercise none of it).
     *
     *    The resulting shape is exactly the case the launcher had to stop
     *    mishandling: this binary PASSES, exits 0, and must simultaneously
     *    report a NON-ZERO skip count into TAP, JUnit XML and JSON. If the
     *    launcher ever regresses to inferring "no skips" from exit 0, the
     *    run's [UTEST-REPORT-SUMMARY] skip_blocks drops to 0 and the
     *    per-skip records disappear from every artifact. */
    UTEST_SKIP("harness skip path: deliberate probe, this binary must report 1 skip block");

    UTEST_END();
    return g_fail;
}
