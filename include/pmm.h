/* ============================================================================
 * pmm.h — Physical Memory Manager
 *
 * Bitmap-based allocator: 1 bit per 4 KiB physical frame.
 * Parses the Multiboot2 memory map to discover available RAM.
 * ============================================================================ */

#pragma once

#include "types.h"

#define PMM_FRAME_SIZE  4096     /* 4 KiB page frame */

/* Initialize the PMM from the boot memory map */
void pmm_init(void);

/* Allocate a single 4 KiB physical frame. Returns physical address, or 0 on failure */
uintptr_t pmm_alloc_frame(void);

/* Free a previously allocated frame */
void pmm_free_frame(uintptr_t addr);

/* Get memory statistics */
uint64_t pmm_get_total_frames(void);
uint64_t pmm_get_used_frames(void);
uint64_t pmm_get_free_frames(void);
