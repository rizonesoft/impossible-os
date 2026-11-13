/* ============================================================================
 * test_optin_victim.c -- opt-in fixture: the binary test_optin_swap replaces
 *
 * Planned, then rewritten by test_optin_swap.exe before its turn, so the
 * launcher must refuse it ("planned binary replaced") and never run it. The
 * marker below is the payload side effect: scripts/test-identity-path.sh
 * fails if it ever appears on serial.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_optin_victim");
    UTEST_ASSERT(1, "[OPTIN-VICTIM] replaced binary executed");
    UTEST_END();
    return g_fail;
}
