/* ============================================================================
 * test.h -- Minimal kernel unit test framework
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

/* ---- Maximum limits ----
 * Raised 2026-04-19 from 512 to 1024 after the suite count hit 496
 * (see kernel-test-harness roadmap, action-registry section) -- the
 * old ceiling would silently drop the next ~3 TODO sections' tests.
 * When suite_count approaches TEST_MAX_SUITES again, revisit the
 * testing infrastructure: consider kunit-style conditional compilation,
 * parallel execution on SMP, or splitting into loadable test modules.
 * Current growth rate: ~5 suites per TODO section. */
#define TEST_MAX_SUITES     1024
#define TEST_MAX_NAME_LEN   32

/* ---- Test categories for selective execution ---- */
typedef enum {
    TEST_CAT_MM = 0,
    TEST_CAT_FS,
    TEST_CAT_SCHED,
    TEST_CAT_OB,
    TEST_CAT_SECURITY,
    TEST_CAT_IPC,
    TEST_CAT_BOOT,
    TEST_CAT_ABI,
    TEST_CAT_STORAGE,
    TEST_CAT_EXEC,
    TEST_CAT_X86,       /* x86-64 architecture (CPUID, MSR, KPTI, CPU security) */
    TEST_CAT_DESKTOP,   /* desktop UI test framework (fb snapshot, input inject, WM state) */
    TEST_CAT_COUNT,
    TEST_CAT_ALL = 0xFF,
} test_category_t;

/* ---- Test suite entry ---- */
typedef void (*test_fn_t)(void);

typedef struct {
    const char     *name;
    test_fn_t       fn;
    test_category_t cat;
} test_suite_t;

/* ---- Test state (global, used by TEST_ASSERT / TEST_PENDING) ---- */
typedef struct {
    uint32_t passed;
    uint32_t failed;
    uint32_t skipped;
    uint32_t pending;       /* condition holds but feature is intentionally
                             * deferred (slot reserved before its subsystem
                             * ships) -- see TEST_PENDING below */
    uint32_t leaked;        /* suite bodies that left non-zero heap delta
                             * after the action drain, absent an
                             * explicit TEST_EXPECT_LEAK. Advisory until
                             * all existing tests clean up. */
    uint32_t suite_count;
    uint32_t suites_passed;
    uint32_t suites_failed;
    const char *current_suite;
    test_suite_t suites[TEST_MAX_SUITES];
} test_state_t;

extern test_state_t g_test_state;

/* ---- API ---- */

#ifdef KERNEL_TESTS

/* Register a test suite with category (preferred) */
void test_suite_register_cat(const char *name, test_fn_t fn, test_category_t cat);

/* Register a test suite (backward compat -- defaults to TEST_CAT_ALL) */
void test_suite_register(const char *name, test_fn_t fn);

/* Set category filter: only suites matching this category will run.
 * Pass TEST_CAT_ALL (default) to run everything. */
void test_runner_set_filter(test_category_t cat);

/* Set quiet mode: suppress [PASS] lines, only show [FAIL] + summary */
void test_runner_set_quiet(int quiet);

/* Run all registered test suites and print summary */
void test_runner_run(void);

/* Initialize the test framework and register all built-in tests */
void test_runner_init(void);

/* Internal: called by TEST_ASSERT macro */
void _test_assert(int condition, const char *msg, const char *file, int line);

/* Internal: called by TEST_SKIP macro */
void _test_skip(const char *msg, const char *file, int line);

/* Internal: called by TEST_PENDING macro -- the assertion held AND the
 * feature is intentionally deferred. Logged as [STUB] and counted in a
 * separate `pending` bucket so end-of-run summary shows how many
 * features are reserved-but-unimplemented at a glance. */
void _test_pending(int condition, const char *msg,
                   const char *file, int line);

/* Per-test heap-leak detection opt-out macros.
 *
 * TEST_EXPECT_LEAK(bytes, reason) -- the current test expects to leave
 * at least `bytes` bytes on the heap after the action drain. The
 * classifier renders [LEAK-OK] (no counter bump) when the observed
 * positive delta is >= `bytes`; otherwise the post-drain delta still
 * triggers [LEAK]. `bytes` MUST be > 0; non-positive values are
 * rejected as malformed and do NOT arm the opt-out (so a stale
 * TEST_EXPECT_LEAK(-1, ...) cannot mask a real leak). Use for tests
 * that register long-lived state (object type registration, SSDT slot
 * reservation).
 *
 * TEST_LEAK_IGNORE(reason) -- the current test does not use the heap
 * as its primary allocator (e.g. PMM or VMM tests). The delta check
 * is bypassed entirely; renders as [LEAK-SKIP]. Reserve for tests
 * that genuinely exercise a non-heap path; prefer TEST_EXPECT_LEAK
 * over TEST_LEAK_IGNORE when the exact leak amount is known.
 *
 * Both macros take effect for the current suite only; state clears at
 * the start of the next suite. */
void _test_expect_leak(int64_t bytes, const char *reason);
void _test_leak_ignore(const char *reason);

#define TEST_EXPECT_LEAK(bytes, reason) \
    _test_expect_leak((int64_t)(bytes), (reason))
#define TEST_LEAK_IGNORE(reason) \
    _test_leak_ignore((reason))

/* Diagnostic: the signed heap-usage delta across the most-recently-
 * completed suite (= heap_get_used() after action drain minus the
 * snapshot taken before the suite body). Carried across the suite
 * boundary intact: a "verify" suite reads the IMMEDIATELY-PRECEDING
 * completed suite's delta during its own body, then the post-drain
 * step at the end of the verify suite overwrites it with the verify
 * suite's own delta. Used by the harness regression tests to verify
 * the detector actually observed a leak without having to emit a
 * [LEAK] line that would pollute the summary L counter. */
int64_t test_runner_last_leak_delta(void);

/* Register a test-scoped cleanup action. Invoked in LIFO order after the
 * currently-running suite's body returns (pass or fail), before the
 * runner advances to the next suite. Returns 0 on success, -1 if the
 * per-suite action list is full (32 slots) or fn is NULL; a -1 also
 * emits a `[WARN] TEST: <name> :: action list full` line so the test
 * author sees it.
 *
 * Intended consumers: TEST_SCRATCH_KBUF, TEST_KLOG_SUPPRESS,
 * and any future test-scoped resource that needs deterministic cleanup
 * without adding bespoke list management to each test. Model follows
 * Linux KUnit's `kunit_add_action`.
 *
 * Constraints: actions must be non-blocking (they run during the
 * runner's sequential drain phase, not inside a yield-tolerant
 * context). Actions run at PASSIVE_LEVEL; the runner forcibly lowers
 * IRQL + warns if a suite left it elevated. */
int test_add_action(void (*fn)(void *), void *ctx);

/* Category name lookup */
const char *test_category_name(test_category_t cat);

/* Category from short string (e.g. "mm", "fs", "ob") -- returns TEST_CAT_ALL on no match */
test_category_t test_category_from_string(const char *str);

/* ---- Assertion macros ---- */
#define TEST_ASSERT(cond, msg) \
    _test_assert((cond), (msg), __FILE__, __LINE__)

#define TEST_ASSERT_EQ(a, b, msg) \
    _test_assert_eq((uint64_t)(a), (uint64_t)(b), (msg), __FILE__, __LINE__)

#define TEST_ASSERT_NEQ(a, b, msg) \
    _test_assert_neq((uint64_t)(a), (uint64_t)(b), (msg), __FILE__, __LINE__)

#define TEST_ASSERT_NULL(p, msg) \
    _test_assert((p) == (void *)0, (msg), __FILE__, __LINE__)

#define TEST_ASSERT_NOT_NULL(p, msg) \
    _test_assert((p) != (void *)0, (msg), __FILE__, __LINE__)

#define TEST_SKIP(msg) \
    _test_skip((msg), __FILE__, __LINE__)

/* TEST_PENDING -- the deferred-feature contract holds. Use when a slot
 * is intentionally reserved before its subsystem ships (e.g. SSDT slot
 * registered as a stub returning a documented "not implemented" status
 * pending the engine in another TODO). The condition still verifies the
 * deferred contract; this macro just renders the outcome as [STUB] in
 * the boot log and counts in a separate `pending` bucket so total
 * incomplete features are visible in the end-of-run summary. */
#define TEST_PENDING(cond, msg) \
    _test_pending((cond), (msg), __FILE__, __LINE__)

void _test_assert_eq(uint64_t a, uint64_t b, const char *msg,
                     const char *file, int line);
void _test_assert_neq(uint64_t a, uint64_t b, const char *msg,
                      const char *file, int line);

#else /* !KERNEL_TESTS */

/* When tests are disabled, everything compiles to nothing */

static inline void test_suite_register_cat(const char *name __attribute__((unused)),
                                           test_fn_t fn __attribute__((unused)),
                                           test_category_t cat __attribute__((unused))) {}
static inline void test_suite_register(const char *name __attribute__((unused)),
                                        test_fn_t fn __attribute__((unused))) {}
static inline void test_runner_set_filter(test_category_t cat __attribute__((unused))) {}
static inline void test_runner_set_quiet(int quiet __attribute__((unused))) {}
static inline void test_runner_run(void) {}
static inline void test_runner_init(void) {}

static inline int test_add_action(void (*fn)(void *) __attribute__((unused)),
                                   void *ctx __attribute__((unused))) { return 0; }

#define TEST_EXPECT_LEAK(bytes, reason) ((void)0)
#define TEST_LEAK_IGNORE(reason)        ((void)0)

#define TEST_ASSERT(cond, msg)      ((void)0)
#define TEST_ASSERT_EQ(a, b, msg)   ((void)0)
#define TEST_ASSERT_NEQ(a, b, msg)  ((void)0)
#define TEST_ASSERT_NULL(p, msg)    ((void)0)
#define TEST_ASSERT_NOT_NULL(p, msg)((void)0)
#define TEST_SKIP(msg)              ((void)0)
#define TEST_PENDING(cond, msg)     ((void)0)

#endif /* KERNEL_TESTS */
