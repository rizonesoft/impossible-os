/* ============================================================================
 * test_pmm.c -- PMM unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/memmap.h"    /* MM_HHDM_BASE/_SIZE + extent predicate */
#include "kernel/sched/irql.h"   /* KeRaiseIrql / KeLowerIrql for IRQL-gate test */

/* Test: single alloc + free round-trip */
static void test_pmm_alloc_free(void)
{
    uintptr_t frame = pmm_alloc_frame();
    TEST_ASSERT(frame != 0, "pmm_alloc_frame returns non-zero");

    /* Frame should be page-aligned (4 KB) */
    TEST_ASSERT((frame & 0xFFF) == 0, "allocated frame is page-aligned");

    pmm_free_frame(frame);

    /* After free, re-alloc should succeed (frame reuse) */
    uintptr_t frame2 = pmm_alloc_frame();
    TEST_ASSERT(frame2 != 0, "re-alloc after free succeeds");
    pmm_free_frame(frame2);
}

/* Test: contiguous allocation */
static void test_pmm_contiguous(void)
{
    /* Allocate 4 contiguous frames (16 KB) */
    uintptr_t block = pmm_alloc_contiguous(4);
    TEST_ASSERT(block != 0, "pmm_alloc_contiguous(4) returns non-zero");
    TEST_ASSERT((block & 0xFFF) == 0, "contiguous block is page-aligned");

    /* Free all 4 frames */
    for (int i = 0; i < 4; i++) {
        pmm_free_frame(block + (uint64_t)i * 4096);
    }
}

/* Test: pmm_free_contiguous releases EVERY frame in the run.
 *
 * Regression net for the multi-frame rollback leak: pmm_free_frame() releases
 * exactly one frame, so a caller rolling back an N-frame pool with a single
 * call leaks N-1 frames. Asserted as an exact net-zero on the free-frame count
 * (a structural invariant), not an approximate bound. */
static void test_pmm_free_contiguous_frees_all(void)
{
    const uint64_t count = 8;
    uint64_t before = pmm_get_free_frames();

    uintptr_t block = pmm_alloc_contiguous(count);
    TEST_ASSERT(block != 0, "pmm_alloc_contiguous(8) returns non-zero");
    TEST_ASSERT_EQ(pmm_get_free_frames(), before - count,
                   "8-frame alloc consumes exactly 8 frames");

    pmm_free_contiguous(block, count);
    TEST_ASSERT_EQ(pmm_get_free_frames(), before,
                   "pmm_free_contiguous returns every frame (net zero)");
}

/* Test: pmm_free_contiguous rejects degenerate input without touching state. */
static void test_pmm_free_contiguous_degenerate(void)
{
    uint64_t before = pmm_get_free_frames();

    pmm_free_contiguous(0, 8);          /* NULL base */
    pmm_free_contiguous(0x100000, 0);   /* zero count */

    TEST_ASSERT_EQ(pmm_get_free_frames(), before,
                   "degenerate pmm_free_contiguous calls are no-ops");
}

/* Test: pmm_alloc_pages_hhdm round-trip through the direct-map alias. */
static void test_pmm_alloc_pages_hhdm(void)
{
    uintptr_t phys = 0;
    uint64_t pages = 0;
    uint64_t before = pmm_get_free_frames();

    /* 9000 bytes rounds up to 3 frames (12 KiB). */
    void *p = pmm_alloc_pages_hhdm(9000, &phys, &pages);
    TEST_ASSERT(p != (void *)0, "pmm_alloc_pages_hhdm(9000) succeeds");
    /* TEST_ASSERT records and CONTINUES -- it does not abort. Every dereference
     * and every free below must therefore be guarded, or a regression in the
     * code under test would turn this suite into a wild write / wild free. */
    if (!p)
        return;
    TEST_ASSERT_EQ(pages, 3, "9000 bytes rounds up to 3 frames");
    TEST_ASSERT(phys != 0, "physical base is reported");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)p, phys + MM_HHDM_BASE,
                   "returned pointer is the HHDM alias of the physical base");

    /* The alias must be writable and read back what was written. */
    uint8_t *bytes = (uint8_t *)p;
    bytes[0] = 0xA5;
    bytes[9000 - 1] = 0x5A;
    TEST_ASSERT_EQ(bytes[0], 0xA5, "first byte reads back through the alias");
    TEST_ASSERT_EQ(bytes[9000 - 1], 0x5A, "last byte reads back through the alias");

    pmm_free_contiguous(phys, pages);
    TEST_ASSERT_EQ(pmm_get_free_frames(), before,
                   "pmm_alloc_pages_hhdm + pmm_free_contiguous is net zero");
}

/* Test: pmm_alloc_pages_hhdm rejects a zero-byte request. */
static void test_pmm_alloc_pages_hhdm_zero(void)
{
    uint64_t before = pmm_get_free_frames();
    TEST_ASSERT(pmm_alloc_pages_hhdm(0, (uintptr_t *)0, (uint64_t *)0) == (void *)0,
                "zero-byte pmm_alloc_pages_hhdm returns NULL");
    TEST_ASSERT_EQ(pmm_get_free_frames(), before,
                   "rejected zero-byte request consumes no frames");
}

/* Test: pmm_alloc_pages_hhdm with BOTH output pointers NULL.
 *
 * This is how nt_misc_atoms_init() calls it, so the success path with NULL
 * outputs is production-reachable and must not dereference them. The physical
 * base is recovered from the returned alias to free the run. */
static void test_pmm_alloc_pages_hhdm_null_outputs(void)
{
    uint64_t before = pmm_get_free_frames();

    void *p = pmm_alloc_pages_hhdm(4096, (uintptr_t *)0, (uint64_t *)0);
    TEST_ASSERT(p != (void *)0, "alloc_pages_hhdm succeeds with NULL outputs");
    if (!p)
        return;   /* asserts do not abort; never free an unvalidated base */

    /* Recover the physical base through the inverse relation and free it. */
    uint64_t phys = mm_hhdm_to_phys(p);
    TEST_ASSERT(phys != 0, "HHDM inverse recovers the physical base");
    if (!phys)
        return;
    pmm_free_contiguous((uintptr_t)phys, 1);
    TEST_ASSERT_EQ(pmm_get_free_frames(), before,
                   "NULL-output alloc + free is net zero");
}

/* Test: byte -> frame rounding at the exact page boundaries. */
static void test_pmm_alloc_pages_hhdm_rounding(void)
{
    struct { uint64_t bytes; uint64_t want_pages; } cases[] = {
        { 1,    1 },   /* smallest non-zero request still takes a whole frame */
        { 4096, 1 },   /* exact frame -- must NOT round up to 2 */
        { 4097, 2 },   /* one byte over -- must round up */
    };

    for (uint32_t i = 0; i < 3; i++) {
        uintptr_t phys = 0;
        uint64_t pages = 0;
        uint64_t before = pmm_get_free_frames();

        void *p = pmm_alloc_pages_hhdm(cases[i].bytes, &phys, &pages);
        TEST_ASSERT(p != (void *)0, "boundary-size alloc succeeds");
        if (!p)
            continue;   /* asserts do not abort; phys/pages are unset on failure */
        TEST_ASSERT_EQ(pages, cases[i].want_pages, "byte count rounds to the exact frame count");

        pmm_free_contiguous(phys, pages);
        TEST_ASSERT_EQ(pmm_get_free_frames(), before, "boundary-size alloc + free is net zero");
    }
}

/* Test: a byte count whose page rounding would wrap is rejected outright. */
static void test_pmm_alloc_pages_hhdm_overflow(void)
{
    uint64_t before = pmm_get_free_frames();
    void *p = pmm_alloc_pages_hhdm((uint64_t)-1, (uintptr_t *)0, (uint64_t *)0);
    TEST_ASSERT(p == (void *)0, "UINT64_MAX byte request is rejected, not wrapped");
    TEST_ASSERT_EQ(pmm_get_free_frames(), before,
                   "rejected overflow request consumes no frames");
}

/* Test: pmm_alloc_pages_hhdm surfaces an underlying OOM as NULL and leaves the
 * frame accounting untouched (fault injection, no real exhaustion needed). */
static void test_pmm_alloc_pages_hhdm_oom(void)
{
    uintptr_t phys = 0xDEAD;
    uint64_t pages = 0xBEEF;
    uint64_t before = pmm_get_free_frames();

    pmm_alloc_fail_next();
    void *p = pmm_alloc_pages_hhdm(9000, &phys, &pages);
    TEST_ASSERT(p == (void *)0, "alloc_pages_hhdm returns NULL when the PMM fails");
    TEST_ASSERT_EQ(pmm_get_free_frames(), before, "failed alloc consumes no frames");
    /* Outputs must be left alone on failure -- callers key off the NULL return. */
    TEST_ASSERT_EQ(phys, 0xDEAD, "phys output untouched on failure");
    TEST_ASSERT_EQ(pages, 0xBEEF, "pages output untouched on failure");

    /* The injection is single-shot: the next allocation must succeed.
     *
     * Reset the sentinels FIRST. On failure this wrapper leaves the outputs
     * untouched (asserted above), and TEST_ASSERT does not abort -- so if this
     * allocation ever regressed to failing, freeing the stale 0xDEAD/0xBEEF
     * sentinels would walk ~48k frames from frame 13 and free the kernel image,
     * the PMM bitmap, and the live pools out from under every later suite. */
    phys = 0;
    pages = 0;
    void *ok = pmm_alloc_pages_hhdm(4096, &phys, &pages);
    TEST_ASSERT(ok != (void *)0, "allocation succeeds again after the single-shot injection");
    if (!ok || !phys || pages != 1)
        return;
    pmm_free_contiguous(phys, pages);
}

/* Test: mm_phys_extent_in_hhdm bounds the WHOLE extent, not just the base.
 *
 * This is the distinction the single-address mm_phys_in_hhdm() cannot make: an
 * extent can start inside the direct-map window and end past its top, which
 * would leave a multi-page pool's tail aliasing nothing. */
static void test_mm_phys_extent_in_hhdm(void)
{
    /* A base one frame below the top is a valid ADDRESS ... */
    TEST_ASSERT(mm_phys_in_hhdm(MM_HHDM_SIZE - 4096) != 0,
                "base one frame below the HHDM top is a valid address");
    /* ... but a two-frame EXTENT from there runs off the top. */
    TEST_ASSERT_EQ(mm_phys_extent_in_hhdm(MM_HHDM_SIZE - 4096, 8192), 0,
                   "extent overrunning the HHDM top is rejected");
    /* Exactly reaching the top is legal (end is exclusive). */
    TEST_ASSERT(mm_phys_extent_in_hhdm(MM_HHDM_SIZE - 4096, 4096) != 0,
                "extent ending exactly at the HHDM top is accepted");

    TEST_ASSERT_EQ(mm_phys_extent_in_hhdm(0x100000, 0), 0,
                   "zero-length extent is rejected");
    TEST_ASSERT_EQ(mm_phys_extent_in_hhdm(0, 4096), 0,
                   "physical 0 is rejected (it is the failure sentinel)");
    TEST_ASSERT_EQ(mm_phys_extent_in_hhdm(0x100000, (uint64_t)-1), 0,
                   "extent wrapping the 64-bit space is rejected");
    TEST_ASSERT(mm_phys_extent_in_hhdm(0x100000, 0x10000) != 0,
                "ordinary in-window extent is accepted");
}

/* PMM fault injection -- mirrors the kmalloc tests in test_heap.c.
 * Arms pmm_alloc_fail_next(), expects next pmm_alloc_frame() to return
 * 0 without consuming bitmap state, and proves subsequent allocations
 * succeed normally. */
static void test_pmm_fault_inject_next(void)
{
    uint64_t pre = pmm_alloc_fail_injections_triggered();
    uint64_t used_pre = pmm_get_used_frames();

    pmm_alloc_fail_next();
    uintptr_t a = pmm_alloc_frame();
    TEST_ASSERT(a == 0, "armed pmm_alloc_fail_next makes next pmm_alloc_frame fail");
    TEST_ASSERT_EQ(pmm_get_used_frames(), used_pre,
                   "forced fail leaves used-frame counter unchanged");

    uintptr_t b = pmm_alloc_frame();
    TEST_ASSERT(b != 0, "auto-cleared after fire -- next alloc succeeds");
    if (b) pmm_free_frame(b);

    uint64_t post = pmm_alloc_fail_injections_triggered();
    TEST_ASSERT_EQ(post - pre, 1u,
                   "pmm injection counter advanced by exactly 1");
}

/* Countdown(3): arm for the 3rd call. Calls 1-2 succeed; call 3 fails. */
static void test_pmm_fault_inject_countdown(void)
{
    uint64_t pre = pmm_alloc_fail_injections_triggered();
    pmm_alloc_fail_countdown_set(3);

    uintptr_t a = pmm_alloc_frame();
    uintptr_t b = pmm_alloc_frame();
    uintptr_t c = pmm_alloc_frame();

    TEST_ASSERT(a != 0, "call 1 succeeds (countdown at 2)");
    TEST_ASSERT(b != 0, "call 2 succeeds (countdown at 1)");
    TEST_ASSERT(c == 0, "call 3 fails (countdown crosses to 0)");

    if (a) pmm_free_frame(a);
    if (b) pmm_free_frame(b);

    TEST_ASSERT_EQ(pmm_alloc_fail_injections_triggered() - pre, 1u,
                   "exactly 1 fire across countdown(3)");
}

/* IRQL gate: arming the countdown and then calling pmm_alloc_frame at
 * DISPATCH_LEVEL must NOT consume the countdown (mirrors the kmalloc
 * IRQL-gate regression). After lowering, the fire still happens. */
static void test_pmm_fault_inject_irql_gate(void)
{
    uint64_t pre = pmm_alloc_fail_injections_triggered();
    pmm_alloc_fail_next();

    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    uintptr_t hi = pmm_alloc_frame();
    KeLowerIrql(old);
    TEST_ASSERT(hi != 0,
                "pmm_alloc_frame at DISPATCH_LEVEL does NOT consume the pending injection");
    if (hi) pmm_free_frame(hi);

    uintptr_t lo = pmm_alloc_frame();
    TEST_ASSERT(lo == 0,
                "countdown still armed at PASSIVE -- fires now");
    uint64_t post = pmm_alloc_fail_injections_triggered();
    TEST_ASSERT_EQ(post - pre, 1u,
                   "IRQ-context call did not increment injection counter");
}

/* Task-filter: armed for foreign PID, current-CPU alloc must NOT fire
 * and countdown stays armed. Mirrors test_heap_fault_inject_task_filter
 * exactly so a field mix-up in pmm_fault_should_fire is caught. */
static void test_pmm_fault_inject_task_filter(void)
{
    uint64_t pre = pmm_alloc_fail_injections_triggered();

    pmm_alloc_fail_next();
    pmm_alloc_fail_task_filter_set(0xFFFFFFFFu);
    uintptr_t a = pmm_alloc_frame();
    TEST_ASSERT(a != 0, "task-filter skips non-matching task");
    if (a) pmm_free_frame(a);
    TEST_ASSERT_EQ(pmm_alloc_fail_injections_triggered(), pre,
                   "pmm injection counter unchanged when task-filter blocks");

    pmm_alloc_fail_task_filter_clear();
    uintptr_t b = pmm_alloc_frame();
    TEST_ASSERT(b == 0, "countdown preserved across filter-blocked calls -- fires now");
    TEST_ASSERT_EQ(pmm_alloc_fail_injections_triggered() - pre, 1u,
                   "exactly 1 pmm fire after filter clear");
}

/* Max-injections auto-reload: arm once, expect N fires. Mirror of the
 * heap-side test -- catches auto-reload regressions in the pmm copy. */
static void test_pmm_fault_inject_max_cap(void)
{
    uint64_t pre = pmm_alloc_fail_injections_triggered();
    pmm_alloc_fail_max_injections_set(3);
    pmm_alloc_fail_next();

    int null_hits = 0;
    uintptr_t frames[10] = {0};
    for (int i = 0; i < 10; i++) {
        frames[i] = pmm_alloc_frame();
        if (!frames[i])
            null_hits++;
    }
    TEST_ASSERT_EQ(null_hits, 3, "pmm max_injections=3 + single arm produces 3 NULLs");
    TEST_ASSERT_EQ(pmm_alloc_fail_fired_counter(), 3u,
                   "pmm fired_counter equals cap after auto-reload stops");
    TEST_ASSERT_EQ(pmm_alloc_fail_injections_triggered() - pre, 3u,
                   "pmm injection counter advanced by exactly 3");

    /* Free the 7 allocations that succeeded. */
    for (int i = 0; i < 10; i++)
        if (frames[i]) pmm_free_frame(frames[i]);

    pmm_alloc_fail_max_injections_clear();
}

/* Registration */
void test_register_pmm(void)
{
    test_suite_register_cat("PMM: alloc+free", test_pmm_alloc_free, TEST_CAT_MM);
    test_suite_register_cat("PMM: contiguous", test_pmm_contiguous, TEST_CAT_MM);
    test_suite_register_cat("PMM: free_contiguous frees every frame",
                            test_pmm_free_contiguous_frees_all, TEST_CAT_MM);
    test_suite_register_cat("PMM: free_contiguous degenerate input",
                            test_pmm_free_contiguous_degenerate, TEST_CAT_MM);
    test_suite_register_cat("PMM: alloc_pages_hhdm round-trip",
                            test_pmm_alloc_pages_hhdm, TEST_CAT_MM);
    test_suite_register_cat("PMM: alloc_pages_hhdm rejects zero bytes",
                            test_pmm_alloc_pages_hhdm_zero, TEST_CAT_MM);
    test_suite_register_cat("PMM: alloc_pages_hhdm NULL outputs",
                            test_pmm_alloc_pages_hhdm_null_outputs, TEST_CAT_MM);
    test_suite_register_cat("PMM: alloc_pages_hhdm page rounding boundaries",
                            test_pmm_alloc_pages_hhdm_rounding, TEST_CAT_MM);
    test_suite_register_cat("PMM: alloc_pages_hhdm rejects rounding overflow",
                            test_pmm_alloc_pages_hhdm_overflow, TEST_CAT_MM);
    test_suite_register_cat("PMM: alloc_pages_hhdm surfaces OOM as NULL",
                            test_pmm_alloc_pages_hhdm_oom, TEST_CAT_MM);
    test_suite_register_cat("MM: HHDM extent bounds whole range",
                            test_mm_phys_extent_in_hhdm, TEST_CAT_MM);
    test_suite_register_cat("PMM: fault-inject pmm_alloc_fail_next",
                            test_pmm_fault_inject_next, TEST_CAT_MM);
    test_suite_register_cat("PMM: fault-inject countdown(3)",
                            test_pmm_fault_inject_countdown, TEST_CAT_MM);
    test_suite_register_cat("PMM: fault-inject IRQL gate (IRQ context bypass)",
                            test_pmm_fault_inject_irql_gate, TEST_CAT_MM);
    test_suite_register_cat("PMM: fault-inject task-filter",
                            test_pmm_fault_inject_task_filter, TEST_CAT_MM);
    test_suite_register_cat("PMM: fault-inject max-injections cap",
                            test_pmm_fault_inject_max_cap, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
