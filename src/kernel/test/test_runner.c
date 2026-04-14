/* ============================================================================
 * test_runner.c -- Kernel unit test runner
 *
 * Runs all registered test suites and prints pass/fail summary to serial.
 * Supports category filtering (test_suite= in boot.conf) and quiet mode
 * (test_quiet=1 suppresses PASS lines, only shows FAIL + summary).
 *
 * Only compiled when KERNEL_TESTS is defined (-DKERNEL_TESTS in CFLAGS).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/klog.h"
#include "kernel/timer.h"

/* ---- Category names (must match test_category_t order) ---- */

static const char *cat_names[] = {
    [TEST_CAT_MM]       = "mm",
    [TEST_CAT_FS]       = "fs",
    [TEST_CAT_SCHED]    = "sched",
    [TEST_CAT_OB]       = "ob",
    [TEST_CAT_SECURITY] = "security",
    [TEST_CAT_IPC]      = "ipc",
    [TEST_CAT_BOOT]     = "boot",
    [TEST_CAT_ABI]      = "abi",
    [TEST_CAT_STORAGE]  = "storage",
    [TEST_CAT_EXEC]     = "exec",
    [TEST_CAT_X86]      = "x86",
};

static const char *cat_labels[] = {
    [TEST_CAT_MM]       = "Memory Management",
    [TEST_CAT_FS]       = "Filesystem",
    [TEST_CAT_SCHED]    = "Scheduler",
    [TEST_CAT_OB]       = "Object Manager",
    [TEST_CAT_SECURITY] = "Security",
    [TEST_CAT_IPC]      = "IPC",
    [TEST_CAT_BOOT]     = "Boot & Logging",
    [TEST_CAT_ABI]      = "ABI Compatibility",
    [TEST_CAT_STORAGE]  = "Storage Drivers",
    [TEST_CAT_EXEC]     = "Binary System",
    [TEST_CAT_X86]      = "x86-64 Architecture",
};

/* ---- Global test state ---- */

test_state_t g_test_state = {
    .passed        = 0,
    .failed        = 0,
    .skipped       = 0,
    .pending       = 0,
    .suite_count   = 0,
    .suites_passed = 0,
    .suites_failed = 0,
    .current_suite = "unknown",
};

static test_category_t g_filter = TEST_CAT_ALL;
static int             g_quiet  = 0;

/* ---- Category helpers ---- */

const char *test_category_name(test_category_t cat)
{
    if (cat < TEST_CAT_COUNT)
        return cat_names[cat];
    return "all";
}

static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

test_category_t test_category_from_string(const char *str)
{
    if (!str || !str[0])
        return TEST_CAT_ALL;
    for (uint32_t i = 0; i < TEST_CAT_COUNT; i++) {
        if (streq_ci(str, cat_names[i]))
            return (test_category_t)i;
    }
    return TEST_CAT_ALL;
}

/* ---- Registration ---- */

void test_suite_register_cat(const char *name, test_fn_t fn, test_category_t cat)
{
    if (g_test_state.suite_count >= TEST_MAX_SUITES) {
        klog(LOG_ERROR, "TEST", "Max test suites reached (%u)",
             (uint64_t)TEST_MAX_SUITES);
        return;
    }
    test_suite_t *s = &g_test_state.suites[g_test_state.suite_count++];
    s->name = name;
    s->fn   = fn;
    s->cat  = cat;
}

void test_suite_register(const char *name, test_fn_t fn)
{
    test_suite_register_cat(name, fn, TEST_CAT_ALL);
}

/* ---- Filter & quiet ---- */

void test_runner_set_filter(test_category_t cat)
{
    g_filter = cat;
}

void test_runner_set_quiet(int quiet)
{
    g_quiet = quiet;
}

/* ---- Assert implementations ---- */

void _test_assert(int condition, const char *msg, const char *file, int line)
{
    if (condition) {
        g_test_state.passed++;
        if (!g_quiet)
            klog(LOG_INFO, "TEST", "%s :: %s", g_test_state.current_suite, msg);
    } else {
        g_test_state.failed++;
        klog(LOG_ERROR, "TEST", "%s :: %s  (%s:%d)",
             g_test_state.current_suite, msg, file, line);
    }
}

void _test_assert_eq(uint64_t a, uint64_t b, const char *msg,
                     const char *file, int line)
{
    if (a == b) {
        g_test_state.passed++;
        if (!g_quiet)
            klog(LOG_INFO, "TEST", "%s :: %s", g_test_state.current_suite, msg);
    } else {
        g_test_state.failed++;
        klog(LOG_ERROR, "TEST", "%s :: %s  (got %u, expected %u)  (%s:%d)",
             g_test_state.current_suite, msg, a, b, file, line);
    }
}

void _test_assert_neq(uint64_t a, uint64_t b, const char *msg,
                      const char *file, int line)
{
    if (a != b) {
        g_test_state.passed++;
        if (!g_quiet)
            klog(LOG_INFO, "TEST", "%s :: %s", g_test_state.current_suite, msg);
    } else {
        g_test_state.failed++;
        klog(LOG_ERROR, "TEST", "%s :: %s  (both are %u)  (%s:%d)",
             g_test_state.current_suite, msg, a, file, line);
    }
}

void _test_skip(const char *msg, const char *file, int line)
{
    (void)file;
    (void)line;
    g_test_state.skipped++;
    if (!g_quiet)
        klog(LOG_WARN, "TEST", "%s :: SKIP: %s",
             g_test_state.current_suite, msg);
}

/* TEST_PENDING -- contract holds AND feature is intentionally deferred.
 * On hold: log [STUB] line and bump pending counter. On break: log a
 * normal failure (the deferred contract was supposed to hold and did
 * not -- e.g. a slot that was supposed to return the deferred status
 * returned something else, indicating mis-registration).  */
void _test_pending(int condition, const char *msg,
                   const char *file, int line)
{
    if (condition) {
        g_test_state.pending++;
        klog(LOG_WARN, "TEST", "%s :: [STUB] %s",
             g_test_state.current_suite, msg);
    } else {
        g_test_state.failed++;
        klog(LOG_ERROR, "TEST",
             "%s :: PENDING-CONTRACT BROKEN: %s  (%s:%d)",
             g_test_state.current_suite, msg, file, line);
    }
}

/* ---- Register built-in test suites ---- */

extern void test_register_pmm(void);
extern void test_register_heap(void);
extern void test_register_vmm(void);
extern void test_register_swap(void);
extern void test_register_mmap(void);
extern void test_register_vfs(void);
extern void test_register_sched(void);
extern void test_register_registry(void);
extern void test_register_boot_init(void);
extern void test_register_boot_info(void);
extern void test_register_klog(void);
extern void test_register_uefi_boot(void);
extern void test_register_boot_device(void);
extern void test_register_x86(void);
extern void test_register_ob(void);
extern void test_register_security(void);
extern void test_register_peb_teb(void);
extern void test_register_nt_types(void);
extern void test_register_ipc(void);
extern void test_register_storage(void);
extern void test_register_ixfs(void);
extern void test_register_bulletproof(void);
extern void test_register_blackbox(void);
extern void test_register_acpi_power(void);
extern void test_register_exec(void);
extern void test_register_crashdump(void);

void test_runner_init(void)
{
    klog(LOG_INFO, "TEST", "============ KERNEL UNIT TESTS ============");

    /* MM */
    test_register_pmm();
    test_register_heap();
    test_register_vmm();
    test_register_swap();
    test_register_mmap();

    /* FS */
    test_register_vfs();
    test_register_ixfs();

    /* Sched */
    test_register_sched();

    /* ABI */
    test_register_registry();

    /* Boot */
    test_register_boot_init();
    test_register_boot_info();
    test_register_klog();
    test_register_uefi_boot();
    test_register_boot_device();

    /* x86-64 Architecture (CPUID, MSR, KPTI, CPU security) */
    test_register_x86();

    /* Exec / Binary System */
    test_register_exec();

    /* OB */
    test_register_ob();

    /* Security */
    test_register_security();

    /* Crash dump */
    test_register_crashdump();

    /* ABI */
    test_register_peb_teb();
    test_register_nt_types();
    test_register_bulletproof();

    /* IPC */
    test_register_ipc();

    /* Storage */
    test_register_storage();
    test_register_blackbox();
    test_register_acpi_power();

    klog(LOG_INFO, "TEST", "%u suite(s) registered",
         (uint64_t)g_test_state.suite_count);
}

/* ---- Run all registered suites ---- */

void test_runner_run(void)
{
    uint64_t run_start = system_get_ticks();
    uint32_t run_count = 0;

    /* Count suites that will run */
    for (uint32_t i = 0; i < g_test_state.suite_count; i++) {
        test_suite_t *s = &g_test_state.suites[i];
        if (g_filter == TEST_CAT_ALL || s->cat == g_filter || s->cat == TEST_CAT_ALL)
            run_count++;
    }

    if (g_filter != TEST_CAT_ALL && g_filter < TEST_CAT_COUNT) {
        klog(LOG_INFO, "TEST", "=== Running %u suite(s) [%s] ===",
             (uint64_t)run_count, cat_labels[g_filter]);
    } else {
        klog(LOG_INFO, "TEST", "=== Running %u suite(s) ===",
             (uint64_t)run_count);
    }

    test_category_t current_cat = TEST_CAT_COUNT; /* sentinel: no category printed yet */

    for (uint32_t i = 0; i < g_test_state.suite_count; i++) {
        test_suite_t *s = &g_test_state.suites[i];

        /* Skip suites not matching the filter */
        if (g_filter != TEST_CAT_ALL && s->cat != g_filter && s->cat != TEST_CAT_ALL)
            continue;

        /* Print category header on first suite in each category */
        if (s->cat < TEST_CAT_COUNT && s->cat != current_cat) {
            current_cat = s->cat;
            klog(LOG_INFO, "TEST", "--- [%s] %s ---",
                 cat_names[current_cat], cat_labels[current_cat]);
        }

        g_test_state.current_suite = s->name;

        uint32_t pre_pass = g_test_state.passed;
        uint32_t pre_fail = g_test_state.failed;

        s->fn();

        uint32_t suite_fails = g_test_state.failed - pre_fail;
        (void)(g_test_state.passed - pre_pass);

        if (suite_fails > 0)
            g_test_state.suites_failed++;
        else
            g_test_state.suites_passed++;
    }

    /* ---- Summary ---- */
    uint64_t run_ms   = (system_get_ticks() - run_start) * 10;
    uint64_t run_sec  = run_ms / 1000;
    uint64_t run_frac = (run_ms % 1000) / 100;
    uint32_t total = g_test_state.passed + g_test_state.failed;
    uint32_t skipped = g_test_state.skipped;
    uint32_t pending = g_test_state.pending;

    /* Single canonical summary including pending (deferred-feature
     * stubs). The pending count lets a glance at the boot log answer
     * "how many features are still incomplete?" without grepping. */
    if (g_test_state.failed == 0) {
        klog(LOG_INFO, "TEST",
             "=== %u tests passed, 0 failed, %u skipped, %u pending (%u.%us) ===",
             (uint64_t)total, (uint64_t)skipped, (uint64_t)pending,
             run_sec, run_frac);
    } else {
        klog(LOG_ERROR, "TEST",
             "=== %u passed, %u FAILED, %u skipped, %u pending (of %u) (%u.%us) ===",
             (uint64_t)g_test_state.passed, (uint64_t)g_test_state.failed,
             (uint64_t)skipped, (uint64_t)pending,
             (uint64_t)total, run_sec, run_frac);
    }
}

#endif /* KERNEL_TESTS */
