/* ============================================================================
 * test_pmm.c -- PMM unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/pmm.h"

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

/* Registration */
void test_register_pmm(void)
{
    test_suite_register_cat("PMM: alloc+free", test_pmm_alloc_free, TEST_CAT_MM);
    test_suite_register_cat("PMM: contiguous", test_pmm_contiguous, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
