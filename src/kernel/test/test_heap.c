/* ============================================================================
 * test_heap.c — Kernel heap (kmalloc/kfree) unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/heap.h"
#include "kernel/types.h"

/* Test: basic kmalloc + kfree round-trip */
static void test_heap_alloc_free(void)
{
    void *p = kmalloc(128);
    TEST_ASSERT(p != NULL, "kmalloc(128) returns non-NULL");

    /* Write pattern to verify memory is usable */
    uint8_t *bytes = (uint8_t *)p;
    for (int i = 0; i < 128; i++) {
        bytes[i] = (uint8_t)(i & 0xFF);
    }
    TEST_ASSERT(bytes[0] == 0 && bytes[127] == 127, "allocated memory is writable");

    kfree(p);
}

/* Test: zero-byte allocation */
static void test_heap_zero_alloc(void)
{
    void *p = kmalloc(0);
    TEST_ASSERT(p == NULL, "kmalloc(0) returns NULL");
}

/* Test: multiple allocations don't overlap */
static void test_heap_no_overlap(void)
{
    void *a = kmalloc(64);
    void *b = kmalloc(64);
    TEST_ASSERT(a != NULL && b != NULL, "both allocations succeed");
    TEST_ASSERT(a != b, "two allocations return different pointers");

    /* Verify they don't overlap: write to both, check integrity */
    uint8_t *pa = (uint8_t *)a;
    uint8_t *pb = (uint8_t *)b;
    for (int i = 0; i < 64; i++) {
        pa[i] = 0xAA;
        pb[i] = 0x55;
    }
    TEST_ASSERT(pa[0] == 0xAA && pb[0] == 0x55, "allocations do not overlap");

    kfree(b);
    kfree(a);
}

/* Test: krealloc grows allocation */
static void test_heap_realloc(void)
{
    void *p = kmalloc(32);
    TEST_ASSERT(p != NULL, "initial kmalloc(32) for realloc test");

    uint8_t *bytes = (uint8_t *)p;
    for (int i = 0; i < 32; i++) {
        bytes[i] = (uint8_t)i;
    }

    void *p2 = krealloc(p, 128);
    TEST_ASSERT(p2 != NULL, "krealloc(p, 128) returns non-NULL");

    /* Original data should be preserved */
    bytes = (uint8_t *)p2;
    TEST_ASSERT(bytes[0] == 0 && bytes[31] == 31, "krealloc preserves original data");

    kfree(p2);
}

/* Registration */
void test_register_heap(void)
{
    test_suite_register("Heap: alloc+free", test_heap_alloc_free);
    test_suite_register("Heap: zero alloc", test_heap_zero_alloc);
    test_suite_register("Heap: no overlap", test_heap_no_overlap);
    test_suite_register("Heap: realloc", test_heap_realloc);
}

#endif /* KERNEL_TESTS */
