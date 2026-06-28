/* ============================================================================
 * test_heap.c -- Kernel heap (kmalloc/kfree) unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/heap.h"
#include "kernel/sched/irql.h"          /* KeRaiseIrql / KeLowerIrql for gate test */
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

/* Test: kmalloc fault injection -- next call returns NULL, subsequent OK.
 * Verifies the per-CPU countdown hook arms correctly, triggers exactly
 * once, auto-resets to 0, and bumps the global trigger counter. */
static void test_heap_fault_inject_next(void)
{
    uint64_t pre = kmalloc_fail_injections_triggered();

    kmalloc_fail_next();
    void *a = kmalloc(16);
    TEST_ASSERT(a == NULL, "armed kmalloc_fail_next makes the next kmalloc fail");

    void *b = kmalloc(16);
    TEST_ASSERT(b != NULL, "subsequent kmalloc succeeds (counter auto-cleared)");
    kfree(b);

    uint64_t post = kmalloc_fail_injections_triggered();
    TEST_ASSERT_EQ(post - pre, 1u,
                   "injection counter increments by exactly 1 per forced NULL");
}

/* Test: kmalloc_fail_countdown_set(N) fails the N-th call, not earlier. */
static void test_heap_fault_inject_countdown(void)
{
    uint64_t pre = kmalloc_fail_injections_triggered();

    kmalloc_fail_countdown_set(3);
    void *a = kmalloc(32);  /* decrements to 2 */
    void *b = kmalloc(32);  /* decrements to 1 */
    void *c = kmalloc(32);  /* decrements to 0 -- fails */
    void *d = kmalloc(32);  /* counter cleared -- succeeds */

    TEST_ASSERT(a != NULL, "countdown=3: call 1 succeeds");
    TEST_ASSERT(b != NULL, "countdown=3: call 2 succeeds");
    TEST_ASSERT(c == NULL, "countdown=3: call 3 fails");
    TEST_ASSERT(d != NULL, "countdown=3: call 4 succeeds (counter cleared)");

    if (a) kfree(a);
    if (b) kfree(b);
    if (d) kfree(d);

    uint64_t post = kmalloc_fail_injections_triggered();
    TEST_ASSERT_EQ(post - pre, 1u,
                   "countdown=3 triggers injection counter exactly once");
}

/* Test: kmalloc_fail_countdown_clear disarms a pending trap. */
static void test_heap_fault_inject_clear(void)
{
    uint64_t pre = kmalloc_fail_injections_triggered();

    kmalloc_fail_countdown_set(1);   /* arm */
    kmalloc_fail_countdown_clear();  /* disarm */

    void *a = kmalloc(16);
    TEST_ASSERT(a != NULL,
                "clear disarms a pending next-call-fails trap");
    kfree(a);

    uint64_t post = kmalloc_fail_injections_triggered();
    TEST_ASSERT_EQ(post - pre, 0u,
                   "cleared countdown does not touch the injection counter");
}

/* Test: heap state is unchanged on a forced NULL. */
static void test_heap_fault_inject_no_state_change(void)
{
    uint64_t used_before = heap_get_used();
    kmalloc_fail_next();
    void *a = kmalloc(64);
    uint64_t used_after = heap_get_used();
    TEST_ASSERT(a == NULL, "forced NULL on arm");
    TEST_ASSERT_EQ(used_after, used_before,
                   "forced NULL does not mutate heap used_bytes");
}

/* Test: fault-injection hook is thread-context-only. An armed countdown
 * must not be consumed by a kmalloc made at DISPATCH_LEVEL (simulating
 * an IRQ-context allocator on the same CPU). This closes the Codex
 * medium finding that IRQ-path allocations could otherwise steal the
 * injection from the thread-context test that armed it. */
static void test_heap_fault_inject_irql_gate(void)
{
    uint64_t pre = kmalloc_fail_injections_triggered();
    kmalloc_fail_next();

    KIRQL old;
    KeRaiseIrql(DISPATCH_LEVEL, &old);
    void *a = kmalloc(16);
    KeLowerIrql(old);
    TEST_ASSERT(a != NULL,
                "kmalloc at DISPATCH_LEVEL does NOT consume the pending injection");
    if (a) kfree(a);

    /* Back at PASSIVE_LEVEL, the injection should still be pending. */
    void *b = kmalloc(16);
    TEST_ASSERT(b == NULL,
                "injection pending after IRQ-context bypass -- thread context still fails");

    uint64_t post = kmalloc_fail_injections_triggered();
    TEST_ASSERT_EQ(post - pre, 1u,
                   "IRQ-context allocation did not increment injection counter");
}

/* task-filter: when kmalloc_fail_task_filter_set(pid) is armed,
 * only calls from the matching task fire the countdown. Calls from
 * foreign tasks skip the hook without consuming the countdown. */
static void test_heap_fault_inject_task_filter(void)
{
    uint64_t pre = kmalloc_fail_injections_triggered();

    /* Arm for foreign PID -- task_current()->pid is the runner thread,
     * so our kmalloc should SKIP the hook with countdown untouched. */
    kmalloc_fail_next();
    kmalloc_fail_task_filter_set(0xFFFFFFFFu);
    void *a = kmalloc(16);
    TEST_ASSERT(a != NULL,
                "task-filter skips non-matching task -- kmalloc succeeds");
    kfree(a);
    TEST_ASSERT_EQ(kmalloc_fail_injections_triggered(), pre,
                   "injection counter unchanged when task-filter does not match");

    /* Verify the countdown is STILL armed -- foreign-task filter path
     * must not consume the countdown. Clear the filter and expect the
     * next kmalloc to fire. */
    kmalloc_fail_task_filter_clear();
    void *b = kmalloc(16);
    TEST_ASSERT(b == NULL,
                "countdown still armed after task-filter skip -- fires now");
    uint64_t post = kmalloc_fail_injections_triggered();
    TEST_ASSERT_EQ(post - pre, 1u, "exactly one forced NULL after filter clear");
}

/* max-injections cap with auto-reload: arming max_injections_set(N)
 * and kmalloc_fail_next() ONCE causes the next N calls to return NULL;
 * subsequent calls succeed. Proves the hook auto-reloads the countdown
 * between fires while the cap has not been reached, then stops. */
static void test_heap_fault_inject_max_cap(void)
{
    uint64_t pre = kmalloc_fail_injections_triggered();

    kmalloc_fail_max_injections_set(3);
    kmalloc_fail_next();   /* single arm; auto-reload fires the rest */

    int null_hits = 0;
    for (int i = 0; i < 10; i++) {
        void *p = kmalloc(16);
        if (!p) {
            null_hits++;
        } else {
            kfree(p);
        }
    }
    TEST_ASSERT_EQ(null_hits, 3,
                   "max_injections=3 + single arm produces exactly 3 NULLs via auto-reload");
    TEST_ASSERT_EQ(kmalloc_fail_injections_triggered() - pre, 3u,
                   "injection counter advanced by exactly 3");
    TEST_ASSERT_EQ(kmalloc_fail_fired_counter(), 3u,
                   "per-CPU fired counter equals cap after auto-reload stops");

    kmalloc_fail_max_injections_clear();
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

/* Test: kmalloc returns 16-byte-aligned user pointers (hardened header is
 * sized to a multiple of 16 so DMA / cast-to-struct callers can rely on it). */
static void test_heap_alignment(void)
{
    static const size_t sizes[] = { 1, 8, 15, 16, 17, 64, 100, 256, 4096 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = kmalloc(sizes[i]);
        TEST_ASSERT(p != NULL, "kmalloc returns non-NULL for alignment test");
        TEST_ASSERT_EQ((uint64_t)((uintptr_t)p & 15u), 0u,
                       "kmalloc pointer is 16-byte aligned");
        kfree(p);
    }
}

/* Test: init-on-alloc zeroes the user region (Linux INIT_ON_ALLOC parity). */
static void test_heap_init_on_alloc(void)
{
    uint8_t *p = (uint8_t *)kmalloc(96);
    TEST_ASSERT(p != NULL, "kmalloc(96) non-NULL");
    int all_zero = 1;
    for (int i = 0; i < 96; i++)
        if (p[i] != 0) { all_zero = 0; break; }
    TEST_ASSERT(all_zero, "kmalloc user region is zeroed on allocation");
    kfree(p);
}

/* Test: kmalloc_zeroed always returns a zeroed region. */
static void test_heap_zeroed_alloc(void)
{
    uint8_t *p = (uint8_t *)kmalloc_zeroed(200);
    TEST_ASSERT(p != NULL, "kmalloc_zeroed(200) non-NULL");
    TEST_ASSERT_EQ((uint64_t)((uintptr_t)p & 15u), 0u, "kmalloc_zeroed 16-aligned");
    int all_zero = 1;
    for (int i = 0; i < 200; i++)
        if (p[i] != 0) { all_zero = 0; break; }
    TEST_ASSERT(all_zero, "kmalloc_zeroed region is fully zeroed");
    kfree(p);
}

/* Test: tagged alloc + matched-tag free completes without a corruption
 * BugCheck (the mismatch path halts and so cannot be exercised here). */
static void test_heap_tagged_roundtrip(void)
{
    uint32_t tag = 0x54534554u; /* 'TEST' */
    uint8_t *p = (uint8_t *)kmalloc_tagged(48, tag);
    TEST_ASSERT(p != NULL, "kmalloc_tagged(48) non-NULL");
    for (int i = 0; i < 48; i++) p[i] = (uint8_t)i;
    kfree_tagged(p, tag);   /* matching tag -> clean free, no panic */
    /* Heap remains usable after the tagged free (proves it did not poison
     * or corrupt the free list). */
    void *p2 = kmalloc(32);
    TEST_ASSERT(p2 != NULL, "heap usable after matched kfree_tagged");
    kfree(p2);
}

/* Test: writing exactly req_size bytes then freeing does NOT trip the back
 * redzone (it sits immediately after the requested region, not within it). */
static void test_heap_redzone_exact_fit(void)
{
    uint8_t *p = (uint8_t *)kmalloc(100);
    TEST_ASSERT(p != NULL, "kmalloc(100) non-NULL");
    for (int i = 0; i < 100; i++) p[i] = 0xAB;   /* fill the whole request */
    kfree(p);   /* redzones intact -> no corruption BugCheck */
    /* Heap remains usable after the exact-fit free (no false-positive
     * redzone trip would have poisoned it). */
    void *p2 = kmalloc(100);
    TEST_ASSERT(p2 != NULL, "heap usable after exact-fit free");
    kfree(p2);
}

/* Registration */
void test_register_heap(void)
{
    test_suite_register_cat("Heap: 16-byte alignment", test_heap_alignment, TEST_CAT_MM);
    test_suite_register_cat("Heap: init-on-alloc zeroing", test_heap_init_on_alloc, TEST_CAT_MM);
    test_suite_register_cat("Heap: kmalloc_zeroed", test_heap_zeroed_alloc, TEST_CAT_MM);
    test_suite_register_cat("Heap: tagged alloc/free round-trip", test_heap_tagged_roundtrip, TEST_CAT_MM);
    test_suite_register_cat("Heap: redzone exact-fit no false positive", test_heap_redzone_exact_fit, TEST_CAT_MM);
    test_suite_register_cat("Heap: alloc+free", test_heap_alloc_free, TEST_CAT_MM);
    test_suite_register_cat("Heap: zero alloc", test_heap_zero_alloc, TEST_CAT_MM);
    test_suite_register_cat("Heap: no overlap", test_heap_no_overlap, TEST_CAT_MM);
    test_suite_register_cat("Heap: realloc", test_heap_realloc, TEST_CAT_MM);
    test_suite_register_cat("Heap: fault-inject kmalloc_fail_next",
                            test_heap_fault_inject_next, TEST_CAT_MM);
    test_suite_register_cat("Heap: fault-inject countdown(N)",
                            test_heap_fault_inject_countdown, TEST_CAT_MM);
    test_suite_register_cat("Heap: fault-inject clear disarms",
                            test_heap_fault_inject_clear, TEST_CAT_MM);
    test_suite_register_cat("Heap: fault-inject no state change",
                            test_heap_fault_inject_no_state_change, TEST_CAT_MM);
    test_suite_register_cat("Heap: fault-inject IRQL gate (IRQ context bypass)",
                            test_heap_fault_inject_irql_gate, TEST_CAT_MM);
    test_suite_register_cat("Heap: fault-inject task-filter",
                            test_heap_fault_inject_task_filter, TEST_CAT_MM);
    test_suite_register_cat("Heap: fault-inject max-injections cap",
                            test_heap_fault_inject_max_cap, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
