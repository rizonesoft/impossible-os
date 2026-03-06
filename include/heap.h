/* ============================================================================
 * heap.h — Kernel Heap Allocator
 *
 * First-fit free-list allocator with block coalescing.
 * Backs onto the VMM/PMM for page allocation.
 * ============================================================================ */

#pragma once

#include "types.h"

/* Initialize the kernel heap (call after VMM is ready) */
void heap_init(void);

/* Allocate 'size' bytes of kernel memory. Returns NULL on failure */
void *kmalloc(size_t size);

/* Free a previously allocated block */
void kfree(void *ptr);

/* Resize a previously allocated block. Returns NULL on failure */
void *krealloc(void *ptr, size_t new_size);

/* Get heap statistics */
uint64_t heap_get_total(void);
uint64_t heap_get_used(void);
uint64_t heap_get_free(void);
