/* ============================================================================
 * vmm.h -- Virtual Memory Manager (x86-64 4-level paging)
 *
 * Manages the kernel's PML4 page tables. Provides fine-grained 4 KiB
 * page mapping on top of the boot-time 2 MiB identity mapping.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Page flags (bits in page table entries) */
#define VMM_FLAG_PRESENT    (1ULL << 0)
#define VMM_FLAG_WRITABLE   (1ULL << 1)
#define VMM_FLAG_USER       (1ULL << 2)
#define VMM_FLAG_WRITETHROUGH (1ULL << 3)
#define VMM_FLAG_NOCACHE    (1ULL << 4)
#define VMM_FLAG_ACCESSED   (1ULL << 5)
#define VMM_FLAG_DIRTY      (1ULL << 6)
#define VMM_FLAG_HUGE       (1ULL << 7)   /* 2 MiB page (PD level) */
#define VMM_FLAG_GLOBAL     (1ULL << 8)
#define VMM_FLAG_NX         (1ULL << 63)  /* No-Execute */

/* Common flag combinations */
#define VMM_KERNEL_RW  (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE)
#define VMM_KERNEL_RO  (VMM_FLAG_PRESENT)
#define VMM_USER_RW    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define VMM_USER_RO    (VMM_FLAG_PRESENT | VMM_FLAG_USER)

/* Page size */
#define VMM_PAGE_SIZE  4096

/* Initialize the VMM (takes over the boot page tables, registers page fault handler).
 * Returns BOOT_OK on success, BOOT_FATAL on failure. */
#include "kernel/boot_init.h"
boot_result_t vmm_init(void);

/* Map a single 4 KiB page: virtual address → physical address with flags */
int vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Unmap a single 4 KiB page. If free_frame is non-zero, returns the frame to PMM */
void vmm_unmap_page(uintptr_t virt, int free_frame);

/* Get the physical address mapped to a virtual address. Returns 0 if not mapped */
uintptr_t vmm_get_physical(uintptr_t virt);

/* Flush a single TLB entry */
void vmm_flush_tlb(uintptr_t virt);

/* Flush the entire TLB by reloading CR3 */
void vmm_flush_tlb_all(void);

/* Create a per-process PML4 cloned from the kernel.  The user ELF region
 * (0x800000–0x9FFFFF) is split from a 2 MiB huge page into 4 KiB pages
 * so individual pages can have the User bit set.  Returns physical address
 * of the new PML4, or 0 on failure. */
uintptr_t vmm_create_user_pml4(void);

/* Set User bit on a specific 4 KiB page in a per-process PML4.
 * Auto-splits 2 MiB huge pages on demand and propagates User bit
 * at all 4 levels (PML4, PDPT, PD, PT).  Works for ANY address. */
void vmm_set_user_page(uintptr_t pml4_phys, uintptr_t virt);

/* Map a PMM-allocated physical frame at an arbitrary user VA in a per-process
 * PML4.  Creates intermediate tables as needed with User+Writable flags.
 * Zero-fills the frame before mapping to prevent kernel data leaking.
 * Auto-splits 2 MiB huge pages at the PD level.
 * Returns 0 on success, -1 on allocation failure. */
int vmm_map_user_page(uintptr_t cr3, uintptr_t virt, uintptr_t phys);

/* Unmap a user page from a per-process PML4.  Clears the PTE, frees the
 * physical frame via pmm_free_frame().  Does NOT free intermediate tables. */
void vmm_unmap_user_page(uintptr_t cr3, uintptr_t virt);

/* Free a per-process PML4 and all intermediate tables (not physical data pages). */
void vmm_destroy_user_pml4(uintptr_t pml4_phys);

/* Return the kernel PML4 physical address (for kernel tasks). */
uintptr_t vmm_get_kernel_cr3(void);

/* Map device MMIO as Uncacheable (PCD=1 + PWT=1).
 * phys_base must be page-aligned.  Returns UC-mapped virtual address,
 * or NULL on failure.  Uses a bump allocator above the 4 GiB identity map. */
void *vmm_map_mmio_uc(uint64_t phys_base, uint32_t size);

/* Map device MMIO as Write-Combining (PWT=1, PCD=0 -> PAT entry 1 = WC).
 * Requires PAT MSR to have WC at entry 1 (programmed in boot_phase0).
 * phys_base must be page-aligned.  Returns WC-mapped virtual address,
 * or NULL on failure.  Uses same bump allocator as vmm_map_mmio_uc(). */
void *vmm_map_mmio_wc(uint64_t phys_base, uint32_t size);

/* Unmap a previous vmm_map_mmio_uc()/wc() mapping. Does NOT free physical frames. */
void vmm_unmap_mmio(void *virt, uint32_t size);

/* Split a 2 MiB huge page into 512 x 4 KiB pages (identity-preserving).
 * Idempotent -- returns 0 if already split.  Required before unmapping
 * individual 4 KiB pages within the boot-time identity map. */
int vmm_split_huge_page(uintptr_t virt);

/* Install a guard page: split the containing huge page, clear the PTE,
 * and register the address for detection by the page fault handler.
 * On hit, panic_screen shows the label instead of generic "PAGE_FAULT". */
int vmm_install_guard_page(uintptr_t virt, const char *label);

/* Change protection flags on an already-mapped page.
 * Updates the PTE flags without changing the physical address.
 * Flushes the TLB entry. Returns 0 on success, -1 if page not mapped. */
int vmm_protect(uintptr_t virt, uint64_t new_flags);

/* Change protection on a range of pages (page-aligned addr, byte count).
 * Calls vmm_protect() for each page in the range.
 * Returns 0 on success, -1 if any page is not mapped. */
int vmm_protect_range(uintptr_t addr, uint64_t size, uint64_t new_flags);

/* Apply NX policy: mark all non-text kernel pages as non-executable.
 * Call after vmm_init() and cpu_enable_nx(). */
void vmm_apply_nx_policy(void);
