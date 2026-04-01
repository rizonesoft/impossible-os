/* ============================================================================
 * test_runner.c — Kernel unit test runner
 *
 * Runs all registered test suites and prints pass/fail summary to serial.
 * Only compiled when KERNEL_TESTS is defined (-DKERNEL_TESTS in CFLAGS).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/klog.h"

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
        klog(LOG_ERROR, "TEST", "Max test suites reached (%d)", TEST_MAX_SUITES);
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
        klog(LOG_INFO, "TEST", "[ OK ] %s :: %s", g_test_state.current_suite, msg);
    } else {
        g_test_state.failed++;
        klog(LOG_ERROR, "TEST", "[FAIL] %s :: %s  (%s:%d)",
                  g_test_state.current_suite, msg, file, line);
    }
}

/* ---- Register built-in test suites ---- */
extern void test_register_pmm(void);
extern void test_register_heap(void);
extern void test_register_vfs(void);
extern void test_register_sched(void);
extern void test_register_registry(void);
extern void test_register_boot_init(void);

void test_runner_init(void)
{
    klog(LOG_INFO, "TEST", "=== Kernel Test Framework ===");
    klog(LOG_INFO, "TEST", "Registering test suites...");

    test_register_pmm();
    test_register_heap();
    test_register_vfs();
    test_register_sched();
    test_register_registry();
    test_register_boot_init();

    klog(LOG_INFO, "TEST", "%u suite(s) registered", g_test_state.suite_count);
}

/* ---- Run all registered suites ---- */
void test_runner_run(void)
{
    klog(LOG_INFO, "TEST", "=== Running %u test suite(s) ===", g_test_state.suite_count);

    for (uint32_t i = 0; i < g_test_state.suite_count; i++) {
        test_suite_t *s = &g_test_state.suites[i];
        g_test_state.current_suite = s->name;
        klog(LOG_INFO, "TEST", "--- Suite: %s ---", s->name);
        s->fn();
    }

    /* ---- Summary ---- */
    uint32_t total = g_test_state.passed + g_test_state.failed;
    if (g_test_state.failed == 0) {
        klog(LOG_INFO, "TEST", "=== %u tests passed, 0 failed ===", total);
    } else {
        klog(LOG_ERROR, "TEST", "=== %u passed, %u FAILED (of %u) ===",
                  g_test_state.passed, g_test_state.failed, total);
    }
}

#endif /* KERNEL_TESTS */
