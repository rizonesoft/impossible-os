/* ============================================================================
 * test.h — Minimal kernel unit test framework
 *
 * Provides test_assert(), test suite registration, and a test runner.
 * Output goes to serial (via log subsystem) for host-side capture.
 *
 * Usage:
 *   #include "kernel/test/test.h"
 *
 *   static void test_pmm_alloc(void) {
 *       void *p = pmm_alloc_frame();
 *       TEST_ASSERT(p != NULL, "pmm_alloc_frame returns non-NULL");
 *       pmm_free_frame(p);
 *   }
 *
 *   // In a registration function called from test_runner_init():
 *   test_suite_register("PMM", test_pmm_alloc);
 *
 * Conditional compilation:
 *   Build with -DKERNEL_TESTS to enable. Without it, all test macros
 *   and functions compile to nothing.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Maximum limits ---- */
#define TEST_MAX_SUITES     64
#define TEST_MAX_NAME_LEN   32

/* ---- Test suite entry ---- */
typedef void (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t   fn;
} test_suite_t;

/* ---- Test state (global, used by TEST_ASSERT) ---- */
typedef struct {
    uint32_t passed;
    uint32_t failed;
    uint32_t suite_count;
    const char *current_suite;
    test_suite_t suites[TEST_MAX_SUITES];
} test_state_t;

extern test_state_t g_test_state;

/* ---- API ---- */

#ifdef KERNEL_TESTS

/* Register a test suite (name + function) */
void test_suite_register(const char *name, test_fn_t fn);

/* Run all registered test suites and print summary */
void test_runner_run(void);

/* Initialize the test framework and register all built-in tests */
void test_runner_init(void);

/* Internal: called by TEST_ASSERT macro */
void _test_assert(int condition, const char *msg, const char *file, int line);

/* ---- TEST_ASSERT macro ---- */
#define TEST_ASSERT(cond, msg) \
    _test_assert((cond), (msg), __FILE__, __LINE__)

#else /* !KERNEL_TESTS */

/* When tests are disabled, everything compiles to nothing */

static inline void test_suite_register(const char *name __attribute__((unused)),
                                        test_fn_t fn __attribute__((unused))) {}
static inline void test_runner_run(void) {}
static inline void test_runner_init(void) {}

#define TEST_ASSERT(cond, msg) ((void)0)

#endif /* KERNEL_TESTS */
