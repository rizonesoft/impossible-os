/* ============================================================================
 * test_runner.c — Kernel unit test runner
 *
 * Runs all registered test suites and prints pass/fail summary to serial.
 * Only compiled when KERNEL_TESTS is defined (-DKERNEL_TESTS in CFLAGS).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/log.h"

/* ---- Global test state ---- */
test_state_t g_test_state = {
    .passed      = 0,
    .failed      = 0,
    .suite_count = 0,
    .current_suite = "unknown",
};

/* ---- Register a test suite ---- */
void test_suite_register(const char *name, test_fn_t fn)
{
    if (g_test_state.suite_count >= TEST_MAX_SUITES) {
        log_error("TEST", "Max test suites reached (%d)", TEST_MAX_SUITES);
        return;
    }
    test_suite_t *s = &g_test_state.suites[g_test_state.suite_count++];
    s->name = name;
    s->fn   = fn;
}

/* ---- Assert implementation ---- */
void _test_assert(int condition, const char *msg, const char *file, int line)
{
    if (condition) {
        g_test_state.passed++;
        log_info("TEST", "[PASS] %s :: %s", g_test_state.current_suite, msg);
    } else {
        g_test_state.failed++;
        log_error("TEST", "[FAIL] %s :: %s  (%s:%d)",
                  g_test_state.current_suite, msg, file, line);
    }
}

/* ---- Register built-in test suites ---- */
/* Add extern declarations for test registration functions here.
 * Each subsystem provides a test_register_xxx() function that calls
 * test_suite_register() for its tests. */

/* Example:
 *   extern void test_register_pmm(void);
 *   extern void test_register_heap(void);
 *   extern void test_register_vfs(void);
 */

void test_runner_init(void)
{
    log_info("TEST", "=== Kernel Test Framework ===");
    log_info("TEST", "Registering test suites...");

    /* Register test suites here as they are added:
     * test_register_pmm();
     * test_register_heap();
     * test_register_vfs();
     * test_register_sched();
     */

    log_info("TEST", "%u suite(s) registered", g_test_state.suite_count);
}

/* ---- Run all registered suites ---- */
void test_runner_run(void)
{
    log_info("TEST", "=== Running %u test suite(s) ===", g_test_state.suite_count);

    for (uint32_t i = 0; i < g_test_state.suite_count; i++) {
        test_suite_t *s = &g_test_state.suites[i];
        g_test_state.current_suite = s->name;
        log_info("TEST", "--- Suite: %s ---", s->name);
        s->fn();
    }

    /* ---- Summary ---- */
    uint32_t total = g_test_state.passed + g_test_state.failed;
    if (g_test_state.failed == 0) {
        log_info("TEST", "=== %u tests passed, 0 failed ===", total);
    } else {
        log_error("TEST", "=== %u passed, %u FAILED (of %u) ===",
                  g_test_state.passed, g_test_state.failed, total);
    }
}

#endif /* KERNEL_TESTS */
