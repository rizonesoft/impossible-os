/* ============================================================================
 * scratch.h -- Test-scoped scratch buffer primitive.
 *
 * `TEST_SCRATCH_KBUF(name, size)` declares a `size`-byte scratch buffer
 * for the current test suite and registers its free routine via the
 * Test action registry, so cleanup fires on normal suite exit AND after
 * a TEST_ASSERT-induced early return.
 *
 * Routes by size to respect the project's "kmalloc <= 4 KB" rule:
 *   size <= PMM_FRAME_SIZE  -- kmalloc(size)
 *   size >  PMM_FRAME_SIZE  -- pmm_alloc_pages_hhdm(size)
 *
 * For a 64 KiB test buffer this saves ~3% of the 2 MiB kernel heap per
 * test (the heap would otherwise be dominated by one allocation) and uses
 * whole physical frames instead, returned cleanly on free. The pointer
 * handed back for that route is a DIRECT-MAP (HHDM) address, never a raw
 * physical-as-pointer cast: the cast holds only while the kernel is
 * identity-mapped, which the identity-map teardown retires. Callers may
 * treat it as ordinary kernel memory; nothing may pass it anywhere a
 * PHYSICAL address is required (DMA, MMIO, a page-table entry).
 *
 * Usage:
 *   static void test_my_thing(void) {
 *       TEST_SCRATCH_KBUF(buf, 65536);
 *       // buf is a 64 KiB scratch buffer; freed automatically on
 *       // suite exit (pass or fail -- the test action drain handles it).
 *       fill_thing(buf, 65536);
 *       TEST_ASSERT(verify(buf), "thing verified");
 *   }
 *
 * Bounded:
 *   - Up to TEST_SCRATCH_MAX concurrent scratch allocations per suite.
 *   - The 33rd alloc returns NULL; TEST_ASSERT_NOT_NULL inside the
 *     macro expansion surfaces it as a visible failure instead of a
 *     silent tracking loss.
 *
 * KERNEL_TESTS-only -- release builds drop the entire translation unit.
 * ============================================================================ */

#pragma once

#ifdef KERNEL_TESTS

#include "kernel/types.h"
#include "kernel/test/test.h"  /* test_add_action, TEST_ASSERT_NOT_NULL */

#define TEST_SCRATCH_MAX  32  /* concurrent scratch allocations per suite */

/* Allocate a scratch buffer sized per the kmalloc / pmm_alloc_contiguous
 * threshold. Does NOT register cleanup -- the TEST_SCRATCH_KBUF macro
 * wraps the registration explicitly so the cleanup path is visible at
 * every call site (see section 7 Migration note). Returns NULL on
 * allocation failure or if the record table is full. */
void *test_scratch_alloc(size_t bytes);

/* Free the scratch buffer pointed to by ctx. Safe to call with ctx=NULL
 * (no-op). Intended to be registered via test_add_action(test_scratch_free,
 * ptr) so it runs on action drain. Looks up the record to determine
 * whether the allocation was kmalloc- or pmm-backed and routes the
 * free accordingly. Warns on unknown ptr (potential double-free or
 * foreign pointer). */
void test_scratch_free(void *ctx);

/* Declare a scratch buffer + register its auto-free.
 *
 * The macro does not introduce a new scope -- `name` is declared in the
 * enclosing scope so subsequent statements can use it directly.
 *
 * Two failure modes require early-return from the enclosing test
 * function (matches Linux KUnit's KUNIT_ASSERT_*-terminates-the-test
 * semantic):
 *
 *   1. test_scratch_alloc() returned NULL (OOM, 0-byte request, or
 *      the sidecar table was full). TEST_ASSERT_NOT_NULL records the
 *      failure; the `return` ensures the caller does not then
 *      dereference a NULL buffer and fault.
 *
 *   2. test_add_action() returned -1 (action stack full, or called
 *      during drain -- though drain-reentry is disallowed for suite
 *      bodies in practice). Without cleanup registration the buffer
 *      would persist across suites, so the macro immediately frees
 *      it, records a failing assertion, and returns.
 *
 * Because of case 2, callers MUST NOT use TEST_SCRATCH_KBUF inside an
 * action callback -- the drain path rejects test_add_action, which
 * triggers the rollback path and a failing assertion.
 */
#define TEST_SCRATCH_KBUF(name, size)                                     \
    void *name = test_scratch_alloc((size));                              \
    TEST_ASSERT_NOT_NULL(name, "scratch alloc " #name);                   \
    if (!(name))                                                          \
        return;                                                           \
    if (test_add_action(test_scratch_free, (name)) < 0) {                 \
        test_scratch_free((name));                                        \
        TEST_ASSERT(0, "test_add_action failed for scratch " #name);      \
        return;                                                           \
    }                                                                     \
    (void)0

#endif /* KERNEL_TESTS */
