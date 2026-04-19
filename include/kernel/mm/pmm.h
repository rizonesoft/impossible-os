/* ============================================================================
 * pmm.h -- Physical Memory Manager
 *
 * Bitmap-based allocator: 1 bit per 4 KiB physical frame.
 * Parses the UEFI memory map (with full type annotations) to discover
 * available RAM and classify reserved regions.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define PMM_FRAME_SIZE  4096     /* 4 KiB page frame */

/* Initialize the PMM from the boot memory map.
 * Returns BOOT_OK on success, BOOT_FATAL if no usable memory. */
#include "kernel/boot_init.h"
boot_result_t pmm_init(void);

/* Allocate a single 4 KiB physical frame. Returns physical address, or 0 on failure */
uintptr_t pmm_alloc_frame(void);

/* Allocate N contiguous 4 KiB frames. Returns base physical address, or 0 on failure */
uintptr_t pmm_alloc_contiguous(uint64_t count);

/* Free a previously allocated frame */
void pmm_free_frame(uintptr_t addr);

/* Mark a physical address range as used (e.g. for ELF segment reservations) */
void pmm_mark_region_used(uintptr_t base, uint64_t length);

/* Get memory statistics */
uint64_t pmm_get_total_frames(void);
uint64_t pmm_get_used_frames(void);
uint64_t pmm_get_free_frames(void);

#ifdef KERNEL_TESTS
/* ---- Test-only PMM fault injection (mirrors the kmalloc primitives
 * in include/kernel/mm/heap.h; part of the kernel-test-harness roadmap).
 *
 * Arms a per-CPU countdown that forces the next (or Nth) subsequent
 * pmm_alloc_frame() / pmm_alloc_contiguous() on the same CPU to return
 * 0 without consuming any physical frames. Same gate set as kmalloc:
 *   - IRQL must be PASSIVE_LEVEL (IRQ/DPC callers are bypassed).
 *   - Optional task-pid filter (non-zero = only that task's calls fire).
 *   - Optional max-injections cap (0 = single-shot; N = fail at most N
 *     times since last _set before auto-disarming).
 *
 * Setters all return void. Calling _countdown_set(0), _task_filter_set(0),
 * or _max_injections_set(0) disables the respective gate. Test runner
 * clears all four state fields between suites. Released builds compile
 * the entire surface out via KERNEL_TESTS. */
void     pmm_alloc_fail_countdown_set(uint32_t n);
void     pmm_alloc_fail_countdown_clear(void);
void     pmm_alloc_fail_next(void);
uint64_t pmm_alloc_fail_injections_triggered(void);
void     pmm_alloc_fail_task_filter_set(uint32_t task_pid);
void     pmm_alloc_fail_task_filter_clear(void);
void     pmm_alloc_fail_max_injections_set(uint32_t max);
void     pmm_alloc_fail_max_injections_clear(void);
uint32_t pmm_alloc_fail_fired_counter(void);
#endif /* KERNEL_TESTS */
