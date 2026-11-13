/* ============================================================================
 * test_optin_hang.c -- opt-in fixture: reach ring 3, then never exit
 *
 * Proves the launcher's timeout integration on a live boot: the binary must
 * be reaped carrying TASK_EXIT_UTEST_TIMEOUT, not SIGKILL's -9. It yields in
 * a loop rather than spinning, so it keeps entering the kernel, which is the
 * boundary where a pending signal is delivered once signal_check() has a
 * caller. Driven by scripts/test-timeout-path.sh.
 * ============================================================================ */

#include "test.h"
#include "syscall.h"

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_optin_hang");
    {
        static const char line[] = "[OPTIN-HANG] in ring 3, yielding forever\n";
        sys_write(1, line, sizeof(line) - 1u);
    }
    for (;;)
        sys_yield();
}
