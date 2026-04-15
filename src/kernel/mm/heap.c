/* ============================================================================
 * heap.c -- Kernel Heap Allocator
 *
 * First-fit free-list allocator with coalescing of adjacent free blocks.
 *
 * The heap uses contiguous physical frames from the PMM, accessed via
 * the boot-time identity mapping (phys == virt for the first 4 GiB).
 * This avoids conflicts with the 2 MiB huge pages in the page tables.
 *
 * Each block has a header:
 *   [size | is_free | next]
 *
 * Free blocks are linked in memory order.
 * On kfree(), adjacent free blocks are coalesced to reduce fragmentation.
 * ============================================================================ */

#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"
#ifdef KERNEL_TESTS
#include "kernel/smp.h"                 /* smp_this_cpu() for per-CPU countdown */
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql for thread-context gate */
#endif
/* Heap size constants */
#define HEAP_INITIAL_PAGES  512      /* 512 pages = 2 MiB */
#define HEAP_PAGE_SIZE      4096

/* Block header -- sits before every allocation */
struct block_header {
    uint64_t size;             /* size of the data area (not including header) */
    uint8_t  is_free;          /* 1 = free, 0 = allocated */
    struct block_header *next; /* next block in memory order */
};

#define HEADER_SIZE  sizeof(struct block_header)

/* Minimum usable block size (avoid tiny fragments) */
#define MIN_BLOCK_SIZE  16

/* Head of the block list */
static struct block_header *heap_start_block;
static uint64_t total_heap_size;
static uint64_t used_bytes;

/* --- Internal: split a block if it's large enough --- */
static void block_split(struct block_header *block, size_t size)
{
    if (block->size >= size + HEADER_SIZE + MIN_BLOCK_SIZE) {
        struct block_header *new_block;
        new_block = (struct block_header *)((uint8_t *)block + HEADER_SIZE + size);
        new_block->size = block->size - size - HEADER_SIZE;
        new_block->is_free = 1;
        new_block->next = block->next;

        block->size = size;
        block->next = new_block;
    }
}

/* --- Internal: coalesce adjacent free blocks --- */
static void coalesce_free_blocks(void)
{
    struct block_header *curr = heap_start_block;

    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += HEADER_SIZE + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

boot_result_t heap_init(void)
{
    uintptr_t heap_base;
    size_t i;

    total_heap_size = 0;
    used_bytes = 0;

    /* Allocate contiguous physical frames for the initial heap.
     * Since we're identity-mapped, phys addr == virt addr. */
    heap_base = pmm_alloc_frame();
    if (heap_base == 0) {
        klog(LOG_ERROR, "mm", "Heap: out of physical memory");
        return BOOT_FATAL;
    }

    /* Allocate remaining pages -- they must be contiguous for a simple heap.
     * PMM allocates linearly from the bitmap, so consecutive calls give
     * consecutive frames (as long as the region is free). */
    for (i = 1; i < HEAP_INITIAL_PAGES; i++) {
        uintptr_t frame = pmm_alloc_frame();
        if (frame == 0) {
            klog(LOG_ERROR, "mm", "Heap: out of physical memory at page %u",
                   (uint64_t)i);
            break;
        }
        /* Verify contiguity */
        if (frame != heap_base + i * HEAP_PAGE_SIZE) {
            /* Non-contiguous -- still usable but we stop here */
            pmm_free_frame(frame);
            break;
        }
    }

    total_heap_size = i * HEAP_PAGE_SIZE;

    /* Zero the heap region */
    {
        uint8_t *p = (uint8_t *)heap_base;
        uint64_t j;
        for (j = 0; j < total_heap_size; j++)
            p[j] = 0;
    }

    /* Create the initial free block spanning the entire heap */
    heap_start_block = (struct block_header *)heap_base;
    heap_start_block->size = total_heap_size - HEADER_SIZE;
    heap_start_block->is_free = 1;
    heap_start_block->next = (struct block_header *)0;

    /* Install guard page immediately after heap end.
     * The PMM bitmap has the next frame free, so allocate it as a sentinel.
     * If heap_init exhausted contiguous frames, the guard may fail -- non-fatal. */
    {
        uintptr_t heap_end = heap_base + total_heap_size;
        uintptr_t guard = pmm_alloc_frame();
        if (guard == heap_end) {
            vmm_install_guard_page(guard, "GUARD: kernel heap overflow");
            klog(LOG_INFO, "mm", "Kernel heap: %u KiB at %p (guard at %p)",
                   (uint64_t)(total_heap_size / 1024), heap_base, guard);
        } else {
            /* Non-contiguous -- free and skip guard */
            if (guard) pmm_free_frame(guard);
            klog(LOG_INFO, "mm", "Kernel heap: %u KiB at %p (no guard -- non-contiguous)",
                   (uint64_t)(total_heap_size / 1024), heap_base);
        }
    }

    return (heap_start_block && total_heap_size > 0) ? BOOT_OK : BOOT_FATAL;
}

#ifdef KERNEL_TESTS
/* Aggregate count across all CPUs of kmalloc calls that were forced to
 * return NULL by the fault-injection hook. Updated with __atomic ops so
 * tests on either CPU read a coherent value. */
static uint64_t s_kmalloc_fault_injections;

void kmalloc_fail_countdown_set(uint32_t n)
{
    /* smp_this_cpu() returns this CPU's per-CPU block; the counter is
     * per-CPU by construction so concurrent tests on different CPUs
     * never step on each other. */
    smp_this_cpu()->kmalloc_fail_countdown = n;
}

void kmalloc_fail_countdown_clear(void)
{
    smp_this_cpu()->kmalloc_fail_countdown = 0;
}

void kmalloc_fail_next(void)
{
    smp_this_cpu()->kmalloc_fail_countdown = 1;
}

uint64_t kmalloc_fail_injections_triggered(void)
{
    return __atomic_load_n(&s_kmalloc_fault_injections, __ATOMIC_RELAXED);
}
#endif /* KERNEL_TESTS */

void *kmalloc(size_t size)
{
    struct block_header *curr;

    if (size == 0)
        return (void *)0;

#ifdef KERNEL_TESTS
    /* Test-only fault-injection hook. Checked BEFORE walking the block
     * list so the heap state is unaltered on a forced failure; a
     * returning test then sees the exact same free-list shape it would
     * on a real OOM. Decrement reaches 0 on the armed call: return NULL.
     *
     * Thread-context gate: the hook only consumes the countdown when
     * we are at PASSIVE_LEVEL. IRQ / DPC / spinlock-holding callers on
     * the same CPU (e.g. the RTL8139 RX ISR calling kmalloc for a work
     * packet) would otherwise steal the pending injection from the
     * thread-context test that armed it -- turning the test
     * nondeterministic and giving false confidence. Tests that want to
     * inject into IRQ-context allocations need a different harness
     * scoped to that context; §1 is thread-context only. */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        struct per_cpu_data *pc = smp_this_cpu();
        if (pc && pc->kmalloc_fail_countdown) {
            if (--pc->kmalloc_fail_countdown == 0) {
                __atomic_fetch_add(&s_kmalloc_fault_injections, 1ull,
                                   __ATOMIC_RELAXED);
                return (void *)0;
            }
        }
    }
#endif

    /* Align size to 16 bytes */
    size = (size + 15) & ~((size_t)15);

    /* First-fit: walk the block list */
    curr = heap_start_block;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            block_split(curr, size);
            curr->is_free = 0;
            used_bytes += curr->size;
            return (void *)((uint8_t *)curr + HEADER_SIZE);
        }
        curr = curr->next;
    }

    /* Out of heap memory */
    return (void *)0;
}

void kfree(void *ptr)
{
    struct block_header *block;

    if (!ptr)
        return;

    block = (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);

    if (block->is_free)
        return;   /* double-free protection */

    block->is_free = 1;
    used_bytes -= block->size;

    coalesce_free_blocks();
}

void *krealloc(void *ptr, size_t new_size)
{
    struct block_header *block;
    void *new_ptr;
    size_t copy_size;

    if (!ptr)
        return kmalloc(new_size);

    if (new_size == 0) {
        kfree(ptr);
        return (void *)0;
    }

    new_size = (new_size + 15) & ~((size_t)15);

    block = (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);

    /* Already large enough */
    if (block->size >= new_size)
        return ptr;

    /* Try to merge with next free block */
    if (block->next && block->next->is_free) {
        uint64_t combined = block->size + HEADER_SIZE + block->next->size;
        if (combined >= new_size) {
            used_bytes -= block->size;
            block->size = combined;
            block->next = block->next->next;
            block_split(block, new_size);
            used_bytes += block->size;
            return ptr;
        }
    }

    /* Allocate new, copy, free old */
    new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return (void *)0;

    copy_size = block->size < new_size ? block->size : new_size;
    {
        uint8_t *src = (uint8_t *)ptr;
        uint8_t *dst = (uint8_t *)new_ptr;
        size_t i;
        for (i = 0; i < copy_size; i++)
            dst[i] = src[i];
    }

    kfree(ptr);
    return new_ptr;
}

uint64_t heap_get_total(void)
{
    return total_heap_size;
}

uint64_t heap_get_used(void)
{
    return used_bytes;
}

uint64_t heap_get_free(void)
{
    struct block_header *curr = heap_start_block;
    uint64_t free_bytes = 0;

    while (curr) {
        if (curr->is_free)
            free_bytes += curr->size;
        curr = curr->next;
    }

    return free_bytes;
}
