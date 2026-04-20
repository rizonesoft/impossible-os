/* ============================================================================
 * test_stress_libc.c -- §8 stress binary: libc string surface under repetition
 *
 * The launcher (src/kernel/test/test_usermode.c u_run_one) spawns every
 * test binary exactly once; the stress policy is IN-BINARY, not
 * launcher-side. Rationale: the kernel's task_create monotonically
 * increments `num_tasks` with no slot reuse in task_cleanup, so a
 * launcher-side loop of N spawns would exhaust TASK_MAX (32) after
 * ~20 binaries. See the Codex finding in §8 stamps.
 *
 * This body loops the entire libc string surface 1000 times inside
 * ONE process, matching the user-mode test framework stress-taxonomy
 * checkpoint ("test_stress_libc looping strlen 1000x stays green" in
 * the 00-infrastructure usermode-test-framework TODO). If any
 * iteration breaks the expected invariant the whole binary exits
 * non-zero and the launcher reports FAIL. The `stress_iters` boot.conf
 * knob is reserved for a future env-passing syscall that would let a
 * stress binary query a runtime-tunable iteration count; today the
 * count is hardcoded here.
 * ============================================================================ */

#include "test.h"

#define STRESS_ITERS 1000u

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_stress_libc");

    /* Report the iteration count once so the [UTEST] log shows the
     * actual workload, not just a single pass line. */
    UTEST_ASSERT(STRESS_ITERS == 1000u, "stress iteration count = 1000");

    unsigned int iter;
    unsigned int checks_ok = 0;
    for (iter = 0; iter < STRESS_ITERS; iter++) {
        if (strlen("hello") != 5) break;
        if (strcmp("abc", "abc") != 0) break;
        if (strcmp("abc", "abd") >= 0) break;

        char buf[16];
        memset(buf, 0xA5, sizeof(buf));
        if (buf[0] != (char)0xA5 || buf[15] != (char)0xA5) break;

        const char *src = "stress-pattern-01";
        char dst[32];
        memset(dst, 0, sizeof(dst));
        memcpy(dst, src, 18);  /* 17 chars + NUL */
        if (strcmp(dst, src) != 0) break;

        checks_ok++;
    }

    /* One assertion summarizes the 1000-iteration outcome instead of
     * 1000 individual [PASS] lines polluting the serial log. */
    UTEST_ASSERT(checks_ok == STRESS_ITERS,
                 "all 1000 iterations held libc invariants");

    UTEST_END();
    return g_fail;
}
