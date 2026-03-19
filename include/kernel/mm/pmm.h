/* ============================================================================
 * pmm.h — Physical Memory Manager
 *
 * Bitmap-based allocator: 1 bit per 4 KiB physical frame.
 * Parses the UEFI memory map (with full type annotations) to discover
 * available RAM and classify reserved regions.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define PMM_FRAME_SIZE  4096     /* 4 KiB page frame */

/* Initialize the PMM from the boot memory map */
void pmm_init(void);

/* Allocate a single 4 KiB physical frame. Returns physical address, or 0 on failure */
uintptr_t pmm_alloc_frame(void);

/* Allocate N contiguous 4 KiB frames. Returns base physical address, or 0 on failure */
uintptr_t pmm_alloc_contiguous(uint64_t count);

/* Free a previously allocated frame */
void pmm_free_frame(uintptr_t addr);

/* Get memory statistics */
uint64_t pmm_get_total_frames(void);
uint64_t pmm_get_used_frames(void);
uint64_t pmm_get_free_frames(void);
