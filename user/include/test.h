/* ============================================================================
 * test.h -- Minimal user-mode test assertion harness
 *
 * Header-only macros for writing user-mode test binaries (test_*.exe). The
 * binaries get launched by the kernel test runner once ships; for now
 * the harness is consumed by the smoke binary in user/test/.
 *
 * Output contract (matches what the launcher will scrape):
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

/* Harness counters -- defined once per binary by UTEST_DEFINE_STATE().
 *
 * g_fail is the exit code: read by main()'s `return g_fail;`, mutated by
 * UTEST_ASSERT on FAIL. g_pass and g_skip exist so UTEST_END can report a
 * THIRD outcome the exit code has no room for: a binary that skipped one
 * sub-test and passed the rest used to reach the launcher as an
 * indistinguishable exit 0, which made every machine artifact claim full
 * coverage the run never had.
 *
 * g_skip counts skip BLOCKS (UTEST_SKIP sites taken), not assertions -- one
 * UTEST_SKIP typically guards a block containing several assertions that
 * never ran, so the two counters measure different things and are reported
 * as separate dimensions rather than summed. */
extern int g_fail;
extern int g_pass;
extern int g_skip;
#define UTEST_DEFINE_STATE() int g_pass = 0; int g_skip = 0; int g_fail = 0

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
            g_pass++;                                                        \
        } else {                                                             \
            sys_write(1, "[FAIL] ", 7);                                      \
            sys_write(1, _utest_m, _utest_mlen);                             \
            sys_write(1, UTEST_COLOR_OFF, UTEST_COLOR_OFF_LEN);               \
            sys_write(1, "\n", 1);                                           \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

/* Record a sub-test as SKIPPED without counting it as passed.
 *
 * The kernel-side harness has TEST_SKIP; the user-mode side had no in-binary
 * equivalent, so a test whose precondition was unavailable (typically fault
 * injection, which the kernel refuses under boot.conf test=0) had to choose
 * between failing and asserting something trivially true. The latter is worse:
 * a passing assertion makes a configuration that silently disabled the case
 * indistinguishable from one that ran it.
 *
 * Counts one skip BLOCK. The block this macro guards usually contains several
 * assertions that will not run, so the count is of skip SITES taken, never of
 * assertions -- UTEST_END submits it as its own dimension beside the assertion
 * counts, and the launcher keeps them separate all the way into the artifacts.
 *
 * Whole-binary skips remain exit 77; this is the per-sub-test form. */
#define UTEST_SKIP(msg)                                                      \
    do {                                                                     \
        const char *_utest_s = (msg);                                        \
        sys_write(1, UTEST_COLOR_ON, UTEST_COLOR_ON_LEN);                    \
        sys_write(1, "[SKIP] ", 7);                                          \
        sys_write(1, _utest_s, strlen(_utest_s));                            \
        sys_write(1, UTEST_COLOR_OFF, UTEST_COLOR_OFF_LEN);                  \
        sys_write(1, "\n", 1);                                               \
        g_skip++;                                                            \
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
 * verdict without scrolling for every [FAIL] line.
 *
 * Also submits the counters to the kernel (SYS_TEST_REPORT) so the launcher
 * can put the skip count in TAP / JUnit XML / JSON instead of inferring
 * "no skips" from exit 0. Submit EXACTLY once per binary: the kernel treats
 * a second submission as a self-contradiction and fails the binary. Fire and
 * forget -- the return is ignored on purpose, because a release-flavor
 * kernel legitimately refuses syscall 48 and the binary must still run and
 * exit normally there (the launcher only exists in the test flavor). The
 * report is submitted BEFORE the [UTEST-END] line so the serial verdict is
 * the last thing a human sees for this binary. */
#define UTEST_END()                                                          \
    do {                                                                     \
        (void)sys_test_report((uint32_t)g_pass, (uint32_t)g_fail,            \
                              (uint32_t)g_skip);                             \
        sys_write(1, UTEST_COLOR_ON, UTEST_COLOR_ON_LEN);                    \
        if (g_fail == 0) {                                                   \
            sys_write(1, "[UTEST-END] all pass", 20);                        \
        } else {                                                             \
            sys_write(1, "[UTEST-END] some FAIL", 21);                       \
        }                                                                    \
        sys_write(1, UTEST_COLOR_OFF, UTEST_COLOR_OFF_LEN);                  \
        sys_write(1, "\n", 1);                                               \
    } while (0)

/* Performance-report macro for `test_perf_*.exe` binaries. Emits one
 * `[PERF] <metric>=<value_ns>` line on stdout so operators and future
 * log-parsers can track timings over time. The value is ALWAYS in
 * nanoseconds by convention (even when the test measures milliseconds
 * -- convert at the call site) so all metrics share a single unit.
 *
 * Today the macro only reports; baseline-comparison against
 * `tests/perf-baseline.json` is done by the test itself via existing
 * UTEST_ASSERT against a hardcoded threshold. A future libc JSON
 * helper will read the baseline file and let perf tests do
 * automated drift checking; until that ships, perf tests either
 * hardcode the threshold or only report for trend analysis.
 *
 * Usage:
 *   uint64_t before = sys_uptime_ns();
 *   sys_yield();
 *   uint64_t after  = sys_uptime_ns();
 *   UTEST_PERF("sys_yield_ns", after - before);
 *   UTEST_ASSERT(after - before < 500, "sys_yield < 500ns baseline+15%"); */
#define UTEST_PERF(metric, value_ns)                                         \
    do {                                                                     \
        const char *_utest_pm = (metric);                                    \
        size_t _utest_plen = strlen(_utest_pm);                              \
        char _utest_vbuf[24];                                                \
        uint64_t _utest_v = (uint64_t)(value_ns);                            \
        uint32_t _utest_vi = 0;                                              \
        if (_utest_v == 0) { _utest_vbuf[_utest_vi++] = '0'; }               \
        else {                                                               \
            char _utest_tmp[24];                                             \
            uint32_t _utest_ti = 0;                                          \
            while (_utest_v && _utest_ti < sizeof(_utest_tmp)) {              \
                _utest_tmp[_utest_ti++] = (char)('0' + (_utest_v % 10));     \
                _utest_v /= 10;                                              \
            }                                                                \
            while (_utest_ti && _utest_vi < sizeof(_utest_vbuf))             \
                _utest_vbuf[_utest_vi++] = _utest_tmp[--_utest_ti];           \
        }                                                                    \
        sys_write(1, UTEST_COLOR_ON, UTEST_COLOR_ON_LEN);                    \
        sys_write(1, "[PERF] ", 7);                                          \
        sys_write(1, _utest_pm, _utest_plen);                                \
        sys_write(1, "=", 1);                                                \
        sys_write(1, _utest_vbuf, _utest_vi);                                \
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
