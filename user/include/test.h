/* ============================================================================
 * test.h -- Minimal user-mode test assertion harness
 *
 * Header-only macros for writing user-mode test binaries (test_*.exe). The
 * binaries get launched by the kernel test runner once §3 ships; for now
 * the harness is consumed by the §1 smoke binary in user/test/.
 *
 * Output contract (matches what the §3 launcher will scrape):
 *   [PASS] <msg>             -- assertion held
 *   [FAIL] <msg>             -- assertion failed; g_fail incremented
 *   [UTEST-BEGIN] <name>     -- suite header
 *   [UTEST-END] all pass     -- suite footer when g_fail == 0
 *   [UTEST-END] some FAIL    -- suite footer when g_fail > 0
 *
 * Exit-code contract: each test binary's `main()` ends with `return g_fail;`
 * (or equivalently `sys_exit(g_fail)`). Zero = all assertions held.
 *
 * Usage (single translation unit -- typical case):
 *   #include "test.h"
 *
 *   UTEST_DEFINE_STATE();
 *
 *   int main(void) {
 *       UTEST_BEGIN("test_foo");
 *       UTEST_ASSERT(strlen("hi") == 2, "strlen(\"hi\") == 2");
 *       UTEST_END();
 *       return g_fail;
 *   }
 *
 * Multi-TU consumers: include this header in every TU; call
 * UTEST_DEFINE_STATE() exactly once in the main TU. Other TUs see the
 * extern declaration and link against the single definition.
 * ============================================================================ */

#pragma once

#include "syscall.h"
#include "string.h"

/* Failure counter -- defined once per binary by UTEST_DEFINE_STATE(),
 * read by main()'s `return g_fail;`, mutated by UTEST_ASSERT on FAIL. */
extern int g_fail;
#define UTEST_DEFINE_STATE() int g_fail = 0

/* 24-bit truecolor ANSI wrap matching the kernel klog UTEST palette
 * (#84B2E9 light blue). Wrapping every [PASS]/[FAIL]/[UTEST-BEGIN]/
 * [UTEST-END] line keeps the visual grouping with the kernel-side
 * UTEST klog lines so a boot log is one coherent blue band for the
 * whole test-launcher stream. scripts/test.sh strips ANSI via sed
 * before parsing, so color is never on the analysis hot path.
 *
 * The byte counts in each sys_write below match the literal string
 * lengths; updating the color sequence requires updating every
 * matching count. \033[38;2;132;178;233m is 19 bytes; \033[0m is 4. */
#define UTEST_COLOR_ON  "\033[38;2;132;178;233m"
#define UTEST_COLOR_OFF "\033[0m"
#define UTEST_COLOR_ON_LEN  19
#define UTEST_COLOR_OFF_LEN 4

/* Core assertion: emit [PASS]/[FAIL] line on stdout, count fails. The
 * macro is do/while(0)-wrapped so it composes with `if`/`else` without
 * the dangling-else gotcha. `cond` and `msg` are evaluated exactly once. */
#define UTEST_ASSERT(cond, msg)                                              \
    do {                                                                     \
        const char *_utest_m = (msg);                                        \
        size_t _utest_mlen = strlen(_utest_m);                               \
        sys_write(1, UTEST_COLOR_ON, UTEST_COLOR_ON_LEN);                    \
        if (cond) {                                                          \
            sys_write(1, "[PASS] ", 7);                                      \
            sys_write(1, _utest_m, _utest_mlen);                             \
            sys_write(1, UTEST_COLOR_OFF, UTEST_COLOR_OFF_LEN);              \
            sys_write(1, "\n", 1);                                           \
        } else {                                                             \
            sys_write(1, "[FAIL] ", 7);                                      \
            sys_write(1, _utest_m, _utest_mlen);                             \
            sys_write(1, UTEST_COLOR_OFF, UTEST_COLOR_OFF_LEN);               \
            sys_write(1, "\n", 1);                                           \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

/* Suite header -- print once at the top of main(). */
#define UTEST_BEGIN(name)                                                    \
    do {                                                                     \
        const char *_utest_n = (name);                                       \
        sys_write(1, UTEST_COLOR_ON, UTEST_COLOR_ON_LEN);                    \
        sys_write(1, "[UTEST-BEGIN] ", 14);                                  \
        sys_write(1, _utest_n, strlen(_utest_n));                            \
        sys_write(1, UTEST_COLOR_OFF, UTEST_COLOR_OFF_LEN);                  \
        sys_write(1, "\n", 1);                                               \
    } while (0)

/* Suite footer -- print once just before `return g_fail;`. The all-pass
 * vs some-fail branch lets a human reading raw serial output spot the
 * verdict without scrolling for every [FAIL] line. */
#define UTEST_END()                                                          \
    do {                                                                     \
        sys_write(1, UTEST_COLOR_ON, UTEST_COLOR_ON_LEN);                    \
        if (g_fail == 0) {                                                   \
            sys_write(1, "[UTEST-END] all pass", 20);                        \
        } else {                                                             \
            sys_write(1, "[UTEST-END] some FAIL", 21);                       \
        }                                                                    \
        sys_write(1, UTEST_COLOR_OFF, UTEST_COLOR_OFF_LEN);                  \
        sys_write(1, "\n", 1);                                               \
    } while (0)

/* Fault-injection bridge. Thin wrapper around the raw
 * SYS_FAULT_INJECT syscall that reads clean in test sources. `kind` is
 * one of the FAULT_* selectors from syscall.h; `countdown` is the N-th
 * subsequent kernel call to fail (ignored for FAULT_KMALLOC_NEXT /
 * FAULT_PMM_NEXT / FAULT_VMM_MAP_NEXT / FAULT_COPY_USER_NEXT which all
 * behave as countdown=1, and for FAULT_CLEAR_ALL which disarms).
 *
 * Returns 0 on success, -1 when the kernel hard-fails. The primary
 * -1 cause is boot.conf test=0 -- the gate is enforced server-side
 * in src/kernel/sched/syscall.c so user-mode binaries cannot poison
 * kernel allocators on a production boot. A test binary that needs
 * to be robust against running under test=0 should SKIP (exit 77)
 * instead of FAIL when this returns -1. */
static inline int utest_fault_inject(unsigned int kind,
                                     unsigned int countdown)
{
    return (int)sys_fault_inject((uint32_t)kind, (uint32_t)countdown);
}
