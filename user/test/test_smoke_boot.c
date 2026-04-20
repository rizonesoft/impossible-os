/* ============================================================================
 * test_smoke_boot.c -- §8 smoke binary: is the kernel even running?
 *
 * Smokes run FIRST in the two-phase launcher (src/kernel/test/test_usermode.c,
 * test_usermode_run). A FAIL or SKIP here aborts the entire run and skips
 * every non-smoke binary with "[UTEST] suite ABORT (smoke failed)". The
 * purpose is to catch a catastrophically broken boot (kernel alive but
 * unable to exec user-mode, or a libc/crt0 regression that would
 * masquerade as N trailing test failures) in under a second.
 *
 * Assertion stays minimal on purpose: any ring-3 syscall round-trip that
 * returns plausibly non-negative data proves the kernel reached a state
 * where the launcher can exec binaries, the syscall ABI is wired, and
 * the test.h macros format correctly. If this PASSes, any subsequent
 * FAIL is a test-specific bug, not an infrastructure collapse.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_smoke_boot");

    /* The whole point of a smoke: sys_uptime() returns a value the kernel
     * has been managing since Phase 0. A crashed kernel can't run us; a
     * kernel with a broken SYS_UPTIME dispatch can't return a sane value. */
    long uptime = sys_uptime();
    UTEST_ASSERT(uptime >= 0, "sys_uptime() >= 0 (kernel clock alive)");

    UTEST_END();
    return g_fail;
}
