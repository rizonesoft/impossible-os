/* ============================================================================
 * heap.c — Kernel Heap Allocator
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

#include "heap.h"
#include "pmm.h"
#include "printk.h"

/* Heap size constants */
#define HEAP_INITIAL_PAGES  64       /* 64 pages = 256 KiB */
#define HEAP_PAGE_SIZE      4096

/* Block header — sits before every allocation */
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

void heap_init(void)
{
    uintptr_t heap_base;
    size_t i;

    total_heap_size = 0;
    used_bytes = 0;

    /* Allocate contiguous physical frames for the initial heap.
     * Since we're identity-mapped, phys addr == virt addr. */
    heap_base = pmm_alloc_frame();
    if (heap_base == 0) {
        printk("[FAIL] Heap: out of physical memory\n");
        return;
    }

    /* Allocate remaining pages — they must be contiguous for a simple heap.
     * PMM allocates linearly from the bitmap, so consecutive calls give
     * consecutive frames (as long as the region is free). */
    for (i = 1; i < HEAP_INITIAL_PAGES; i++) {
        uintptr_t frame = pmm_alloc_frame();
        if (frame == 0) {
            printk("[FAIL] Heap: out of physical memory at page %u\n",
                   (uint64_t)i);
            break;
        }
        /* Verify contiguity */
        if (frame != heap_base + i * HEAP_PAGE_SIZE) {
            /* Non-contiguous — still usable but we stop here */
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

    printk("[OK] Kernel heap: %u KiB at %p\n",
           (uint64_t)(total_heap_size / 1024),
           heap_base);
}

void *kmalloc(size_t size)
{
    struct block_header *curr;

    if (size == 0)
        return (void *)0;

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
