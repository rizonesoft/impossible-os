/* ============================================================================
 * test_optin_smoke_abort.c -- opt-in fixture: a smoke that fails on purpose
 *
 * Proves the launcher's abort path on a live boot (the user-mode test framework abort-path proof): a
 * smoke non-PASS stops the run, the JUnit artifact gains its synthetic
 * <error> testcase, and the JSON summary carries aborted plus a not_run count
 * of every binary left behind.
 *
 * The `test_optin_` prefix keeps it out of every run unless utest_filter=
 * names it exactly; naming it adds it to the default plan (see
 * UTEST_OPT_IN_PREFIX in include/kernel/test/test_usermode.h). The
 * `test_optin_smoke_` stem types it as a smoke with or without a manifest.
 * Driven by scripts/test-abort-path.sh.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_optin_smoke_abort");
    UTEST_ASSERT(0, "deliberate smoke failure (abort-path fixture)");
    UTEST_END();
    return g_fail;
}
