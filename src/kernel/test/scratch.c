/* ============================================================================
 * scratch.c -- Implementation of test_scratch_alloc / test_scratch_free.
 *
 * Sidecar ptr -> (pages, mode) table lets test_scratch_free route a free
 * to kfree or a per-page pmm_free_frame loop without embedding a header
 * in the user's buffer. This preserves whatever alignment the underlying
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

struct scratch_rec {
    void    *ptr;    /* pointer returned to user (matches buffer base) */
    uint64_t pages;  /* 0 = kmalloc allocation, >0 = pmm page count */
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
        klog(LOG_WARN, "TEST",
             "test_scratch_alloc: request too large (would overflow size_t)");
        return (void *)0;
    }

    if (s_rec_count >= TEST_SCRATCH_MAX) {
        klog(LOG_WARN, "TEST",
             "test_scratch_alloc: record table full (%u slots)",
             (uint64_t)TEST_SCRATCH_MAX);
        return (void *)0;
    }

    void    *ptr;
    uint64_t pages = 0;

    if (bytes <= PMM_FRAME_SIZE) {
        /* Fits in the heap per project <= 4 KiB kmalloc rule. */
        ptr = kmalloc(bytes);
    } else {
        /* Larger -- round up to page count and use PMM. Avoids
         * dominating the ~2 MiB kernel heap with a single buffer.
         * Keeps the page count as uint64_t end-to-end so pmm_free_frame
         * gets the same count the alloc requested, no truncation. */
        uint64_t count = (bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(count);
        if (!phys)
            return (void *)0;
        ptr   = (void *)phys;
        pages = count;
    }

    if (!ptr)
        return (void *)0;

    s_recs[s_rec_count].ptr   = ptr;
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
                for (uint64_t j = 0; j < pages; j++)
                    pmm_free_frame((uintptr_t)ctx + (uintptr_t)j * PMM_FRAME_SIZE);
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
    klog(LOG_WARN, "TEST",
         "test_scratch_free: unknown ptr (possible double-free)");
}

#endif /* KERNEL_TESTS */
