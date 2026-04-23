/* ============================================================================
 * test_pmm.c -- PMM unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/pmm.h"
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
