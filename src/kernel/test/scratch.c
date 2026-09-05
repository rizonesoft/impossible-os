/* ============================================================================
 * scratch.c -- Implementation of test_scratch_alloc / test_scratch_free.
 *
 * Sidecar ptr -> (phys, pages, mode) table lets test_scratch_free route a
 * free to kfree or pmm_free_contiguous without embedding a header in the
 * user's buffer. This preserves whatever alignment the underlying
 * allocator returned (pmm_alloc_contiguous gives page-aligned, kmalloc
 * gives its own alignment) and keeps the returned pointer byte-for-byte
 * what the caller would get from the raw allocator.
 *
 * Table size: TEST_SCRATCH_MAX (=32), matching the action-registry
 * capacity so the worst case is one scratch-per-action. The 33rd
 * alloc returns NULL so the caller's TEST_ASSERT_NOT_NULL surfaces it.
 * Free uses swap-with-last for O(1) record removal, so the sidecar is
 * bounded in size AND in per-call cost.
 *
 * No spinlock -- the test runner is sequential single-CPU (see
 * test_runner.c s_test_actions comment); if the runner ever fans out,
 * this sidecar needs revisiting.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/scratch.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* The allocator's diagnostics carry their OWN subsystem tag, deliberately NOT
 * "TEST". A test that exercises a refusal path here wants to suppress the
 * expected warning, and the test runner emits its ASSERTION FAILURES under
 * "TEST" at LOG_ERROR (test_runner.c: klog(LOG_ERROR, test_tag(), ...)), so
 * suppressing "TEST" to silence this warning would also discard that test's
 * own failure message, actual/expected values and file:line, leaving a bare
 * count. A separate tag lets the noise be suppressed while the evidence stays
 * readable. */
#define KLOG_TAG_SCRATCH  "SCRATCH"

struct scratch_rec {
    void     *ptr;   /* pointer returned to user (matches buffer base) */
    uintptr_t phys;  /* physical base of the PMM run (0 for the kmalloc route) */
    uint64_t  pages; /* 0 = kmalloc allocation, >0 = pmm page count */
};

static struct scratch_rec s_recs[TEST_SCRATCH_MAX];
static uint8_t s_rec_count;

void *test_scratch_alloc(size_t bytes)
{
    if (bytes == 0)
        return (void *)0;

    /* Reject requests whose ceiling-division would overflow size_t.
     * pmm_alloc_contiguous couldn't satisfy anything near that size
     * anyway (would need > SIZE_MAX - 4095 bytes of RAM); catching it
     * here keeps the arithmetic below wrap-free and the pages field
     * honest. */
    if (bytes > ((size_t)-1) - (PMM_FRAME_SIZE - 1)) {
        klog(LOG_WARN, KLOG_TAG_SCRATCH,
             "test_scratch_alloc: request too large (would overflow size_t)");
        return (void *)0;
    }

    if (s_rec_count >= TEST_SCRATCH_MAX) {
        klog(LOG_WARN, KLOG_TAG_SCRATCH,
             "test_scratch_alloc: record table full (%u slots)",
             (uint64_t)TEST_SCRATCH_MAX);
        return (void *)0;
    }

    void     *ptr;
    uintptr_t phys  = 0;
    uint64_t  pages = 0;

    if (bytes <= PMM_FRAME_SIZE) {
        /* Fits in the heap per project <= 4 KiB kmalloc rule. */
        ptr = kmalloc(bytes);
    } else {
        /* Larger -- round up to page count and use PMM. Avoids
         * dominating the ~2 MiB kernel heap with a single buffer.
         *
         * Reached through the HHDM, not as a raw physical-as-pointer
         * cast: the cast only holds while the kernel is identity-mapped,
         * and the identity-map teardown retires that map. pmm_alloc_pages_hhdm()
         * is the shared helper that already validates the WHOLE extent
         * against the direct-map window (a multi-frame run can start
         * inside it and end past its top) and releases the entire run on
         * any post-allocation rejection, so this does not re-implement
         * that rule. It reports failure by returning NULL, which the
         * caller's TEST_ASSERT_NOT_NULL surfaces as a visible failure. */
        ptr = pmm_alloc_pages_hhdm(bytes, &phys, &pages);
        if (!ptr)
            return (void *)0;
    }

    if (!ptr)
        return (void *)0;

    s_recs[s_rec_count].ptr   = ptr;
    s_recs[s_rec_count].phys  = phys;
    s_recs[s_rec_count].pages = pages;
    s_rec_count++;
    return ptr;
}

void test_scratch_free(void *ctx)
{
    if (!ctx)
        return;

    for (uint8_t i = 0; i < s_rec_count; i++) {
        if (s_recs[i].ptr == ctx) {
            uint64_t pages = s_recs[i].pages;
            if (pages) {
                /* Free the recorded PHYSICAL base, never the pointer the
                 * caller holds -- that is an HHDM alias now, and the
                 * reverse relation reports failure by returning 0, which
                 * pmm_free_frame() cannot distinguish from a real frame.
                 * Recording phys at alloc time keeps the free path off
                 * that ambiguity entirely. */
                pmm_free_contiguous(s_recs[i].phys, pages);
            } else {
                kfree(ctx);
            }
            /* Swap-with-last to remove the record in O(1). */
            s_rec_count--;
            s_recs[i] = s_recs[s_rec_count];
            return;
        }
    }

    /* Not found: either a double-free or a foreign pointer. Warn but
     * don't crash -- the test runner should continue. */
    klog(LOG_WARN, KLOG_TAG_SCRATCH,
         "test_scratch_free: unknown ptr (possible double-free)");
}

#endif /* KERNEL_TESTS */
