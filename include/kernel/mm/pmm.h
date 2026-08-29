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

/* Highest physical address pmm_init tracks. The boot identity map covers the
 * first 4 GiB, so a frame above this has no virtual address here and the
 * bitmap deliberately stops at it. Named because two other places need the
 * SAME number to derive the largest bitmap the kernel could ever build:
 * boot_stack_image_envelope_end() consumer-side, and BL_PMM_BITMAP_PHYS_CAP
 * in src/boot/uefi/bootx64.c producer-side. */
#define PMM_PHYS_ADDR_CAP  0x0000000100000000ULL

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

/* Free N contiguous frames previously obtained from pmm_alloc_contiguous().
 * pmm_free_frame() releases exactly ONE frame, so rolling back a multi-frame
 * allocation with it leaks every frame but the first; this is the symmetric
 * counterpart. Takes the PHYSICAL base -- never an HHDM alias. */
void pmm_free_contiguous(uintptr_t base, uint64_t count);

/* Allocate 'bytes' (rounded up to whole frames) of contiguous physical memory
 * and return its direct-map (HHDM) alias, or NULL on failure.
 *
 * This is the sanctioned way to back a large kernel pool that would otherwise
 * be a static BSS array (CLAUDE.md: kmalloc() for <= 4 KB, pmm_alloc_contiguous()
 * for everything larger). It validates the FULL extent against the direct-map
 * window -- not just the base -- and frees the frames again if the alias cannot
 * be formed, so no caller has to re-derive that rollback. The memory is NOT
 * zeroed; the caller owns initialization.
 *
 * On success 'out_phys'/'out_pages' receive the physical base and frame count
 * to hand to pmm_free_contiguous() later. Both are optional (may be NULL).
 *
 * CALLER CONTRACT: the PMM bitmap is not yet SMP-locked (the PMM bitmap
 * SMP-locking work in todo/03-memory-concurrency/TODO-03-advanced-allocator.md
 * owns that), so a caller must run where no other CPU is calling the PMM
 * concurrently. Be precise about why the boot-time callers qualify, because two
 * plausible-sounding reasons are BOTH WRONG: it is not "before APs exist" (APs
 * are up from smp_init in phase 2, well before these callers), and it is not
 * merely "the scheduler is disabled" (the scheduler is not what runs AP boot
 * work). What actually holds on the normal path is that AP boot work is
 * dispatched through boot_async_group, which BARRIERS until every AP reports
 * done before the sequence proceeds, so no AP is mid-allocation later.
 *
 * That barrier has one documented hole: boot_async_group gives up after a 10s
 * timeout and proceeds while the timed-out AP KEEPS RUNNING (see the "still
 * running" note on its timeout path in boot_init.c). A timed-out storage worker
 * can therefore race a later allocation. That is the unlocked-bitmap defect
 * itself rather than something a caller can code around; it is owned by the PMM
 * bitmap SMP-locking work above and only opens on an already-degraded boot.
 *
 * Never allocate lazily on a call path: a site reached once the scheduler is
 * live races the bitmap unconditionally. */
void *pmm_alloc_pages_hhdm(uint64_t bytes, uintptr_t *out_phys,
                           uint64_t *out_pages);

/* Mark a physical address range as used (e.g. for ELF segment reservations) */
void pmm_mark_region_used(uintptr_t base, uint64_t length);

/* Get memory statistics */
/* Is the frame containing `addr` currently ALLOCATABLE? Read-only; touches no
 * bitmap state. Returns 0 for an out-of-range address, which is the
 * conservative answer for every caller (a frame the allocator does not know
 * about is one it will never hand out).
 *
 * Exists so a reservation can be PROVEN rather than assumed: the acceptance
 * check for the loader-owned kernel boot stack (TODO-10 sec32) asks the
 * bitmap directly whether the frames the kernel is executing on are still
 * available, which is the only evidence that distinguishes a correct
 * reservation from a boot that survived on placement luck. */
int pmm_frame_is_free(uintptr_t addr);

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
 * clears all four state fields between suites. The release build flavor
 * (`make KERNEL_TESTS=off`) compiles the entire surface out and prunes
 * src/kernel/test/ from the build. */
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
