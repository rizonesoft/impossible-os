/* ============================================================================
 * vmm.h — Virtual Memory Manager (x86-64 4-level paging)
 *
 * Manages the kernel's PML4 page tables. Provides fine-grained 4 KiB
 * page mapping on top of the boot-time 2 MiB identity mapping.
 * ============================================================================ */

#pragma once

#include "types.h"

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

/* Initialize the VMM (takes over the boot page tables, registers page fault handler) */
void vmm_init(void);

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
