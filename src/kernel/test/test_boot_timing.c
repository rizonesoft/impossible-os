/* ============================================================================
 * test_boot_timing.c -- FPDT + bootloader TSC normalization unit tests
 *
 * Exercises the pure helpers that support boot_timing_fpdt_unreliable() and
 * boot_timing_get_fpdt_entries() so the JSON exporter and VPD totals can be
 * validated without re-running live boot infrastructure. Follows the
 * "Test Code Policy" rule -- pure helpers only, no _init / vpd_* / boot_progress.
 *
 * XREF: 01-boot-platform/TODO-04-firmware-table-platform-inventory (item:
 *   "Detect zero/garbage FPDT records and mark as unreliable" -- FPDT and
 *   boot timing normalization section)
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_timing.h"

/* ---- Unreliable detection ----------------------------------------------- */

static void test_fpdt_unreliable_when_unavailable(void)
{
    /* available=0 -> unreliable regardless of fields. */
    TEST_ASSERT(boot_timing_fpdt_unreliable_eval(0, 1, 2, 3, 4, 5) == 1,
                "available=0 -> unreliable");
}

static void test_fpdt_unreliable_when_all_zero(void)
{
    /* VirtualBox EFI publishes FPDT with no data. */
    TEST_ASSERT(boot_timing_fpdt_unreliable_eval(1, 0, 0, 0, 0, 0) == 1,
                "all-zero FPDT -> unreliable");
}

static void test_fpdt_reliable_clean_record(void)
{
    /* Plausible OVMF-ish record: reset 50ms, loader_load 100ms,
     * loader_start 110ms, exit_bs_entry 200ms, exit_bs_exit 205ms. */
    uint64_t MS = 1000000ULL;
    int u = boot_timing_fpdt_unreliable_eval(1,
        50  * MS, 100 * MS, 110 * MS, 200 * MS, 205 * MS);
    TEST_ASSERT(u == 0, "monotonic record -> reliable");
}

static void test_fpdt_unreliable_non_monotonic(void)
{
    uint64_t MS = 1000000ULL;
    /* loader_load < reset_end. */
    TEST_ASSERT(boot_timing_fpdt_unreliable_eval(1,
        100 * MS, 50 * MS, 110 * MS, 200 * MS, 205 * MS) == 1,
        "non-monotonic load < reset -> unreliable");
    /* exit_bs_exit < exit_bs_entry. */
    TEST_ASSERT(boot_timing_fpdt_unreliable_eval(1,
        50 * MS, 100 * MS, 110 * MS, 200 * MS, 199 * MS) == 1,
        "non-monotonic bs_exit < bs_entry -> unreliable");
}

static void test_fpdt_unreliable_zero_after_nonzero(void)
{
    uint64_t MS = 1000000ULL;
    /* exit_bs_entry zero after non-zero loader_start: garbage. */
    TEST_ASSERT(boot_timing_fpdt_unreliable_eval(1,
        50 * MS, 100 * MS, 110 * MS, 0, 0) == 1,
        "zero after non-zero -> unreliable");
}

static void test_fpdt_reliable_leading_zero(void)
{
    uint64_t MS = 1000000ULL;
    /* Firmware that did not record reset_end (leading zero) is OK. */
    int u = boot_timing_fpdt_unreliable_eval(1,
        0, 100 * MS, 110 * MS, 200 * MS, 205 * MS);
    TEST_ASSERT(u == 0, "leading-zero reset_end -> reliable");
}

static void test_fpdt_unreliable_zero_anchor(void)
{
    uint64_t MS = 1000000ULL;
    /* os_loader_start_start (the unified-timeline anchor) is zero but
     * later fields are non-zero. Without the anchor we cannot honestly
     * label TSC entries unreliable=false, so the record is unreliable. */
    TEST_ASSERT(boot_timing_fpdt_unreliable_eval(1,
        0, 0, 0, 200 * MS, 205 * MS) == 1,
        "zero os_loader_start_start anchor -> unreliable");
}

static void test_fpdt_unreliable_implausible_large(void)
{
    /* >10 minutes in nanoseconds (cap). */
    uint64_t HUGE = 11ULL * 60ULL * 1000000000ULL;
    TEST_ASSERT(boot_timing_fpdt_unreliable_eval(1,
        HUGE, HUGE, HUGE, HUGE, HUGE) == 1,
        "phase > 10 min cap -> unreliable");
}

/* ---- FPDT entry shape --------------------------------------------------- */

static void test_fpdt_get_entries_emits_fixed_set(void)
{
    boot_timing_fpdt_entry_t out[8];
    /* Behavior is observed via the global g_boot_info; we do not modify
     * timing fields. We only assert that the call returns a fixed phase
     * count (5) regardless of whether real boot data is reliable, and
     * that stage names are stable. */
    uint32_t n = boot_timing_get_fpdt_entries(out, 8);
    TEST_ASSERT(n == 5, "get_fpdt_entries returns 5 fixed phases");

    /* Stage names must be stable for JSON consumers. */
    static const char *expect[5] = {
        "fpdt:reset_end",
        "fpdt:os_loader_load",
        "fpdt:os_loader_start",
        "fpdt:exit_bs_entry",
        "fpdt:exit_bs_exit",
    };
    for (uint32_t i = 0; i < n; i++) {
        const char *a = out[i].stage;
        const char *b = expect[i];
        int eq = 1;
        while (*a && *b && *a == *b) { a++; b++; }
        eq = (*a == 0 && *b == 0);
        TEST_ASSERT(eq, "FPDT stage name matches fixed schema");
    }
}

static void test_fpdt_get_entries_cap_zero(void)
{
    boot_timing_fpdt_entry_t out[1];
    uint32_t n = boot_timing_get_fpdt_entries(out, 0);
    TEST_ASSERT(n == 0, "cap=0 returns 0");
    n = boot_timing_get_fpdt_entries(0, 5);
    TEST_ASSERT(n == 0, "NULL out returns 0");
}

/* ---- Registration ------------------------------------------------------- */

void test_register_boot_timing(void)
{
    test_suite_register_cat("Boot timing: unavailable -> unreliable",
        test_fpdt_unreliable_when_unavailable, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: all-zero -> unreliable",
        test_fpdt_unreliable_when_all_zero, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: clean monotonic -> reliable",
        test_fpdt_reliable_clean_record, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: non-monotonic -> unreliable",
        test_fpdt_unreliable_non_monotonic, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: zero after nonzero -> unreliable",
        test_fpdt_unreliable_zero_after_nonzero, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: leading zero -> reliable",
        test_fpdt_reliable_leading_zero, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: 10min cap -> unreliable",
        test_fpdt_unreliable_implausible_large, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: zero anchor -> unreliable",
        test_fpdt_unreliable_zero_anchor, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: FPDT entries fixed schema",
        test_fpdt_get_entries_emits_fixed_set, TEST_CAT_BOOT);
    test_suite_register_cat("Boot timing: FPDT entries cap/null",
        test_fpdt_get_entries_cap_zero, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
