/* ============================================================================
 * vmm.c — Virtual Memory Manager (x86-64 4-level paging)
 *
 * Takes over the boot-time page tables from entry.asm and provides
 * fine-grained 4 KiB page mapping via the PML4 → PDPT → PD → PT hierarchy.
 *
 * The boot page tables already identity-map the first 4 GiB with 2 MiB pages.
 * This VMM adds the ability to map individual 4 KiB pages for dynamic use
 * (heap, userspace, device MMIO, etc).
 *
 * Page table structure (x86-64):
 *   Virtual address: [PML4 idx][PDPT idx][PD idx][PT idx][offset]
 *                     9 bits    9 bits    9 bits   9 bits  12 bits
 * ============================================================================ */

#include "kernel/mm/vmm.h"
#include "kernel/mm/pmm.h"
#include "kernel/cpuid.h"
#include "kernel/idt.h"
#include "kernel/klog.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/mm/swap.h"
#include "kernel/mm/mmap.h"
#include "kernel/panic.h"

/* Page table entry — 64-bit */
typedef uint64_t pte_t;

/* Number of entries per page table level */
#define PT_ENTRIES 512

/* Mask for extracting physical address from a PTE (bits 12-51) */
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Kernel PML4 — read from CR3 at init */
static pte_t *kernel_pml4;

/* --- Address decomposition --- */

static inline uint64_t pml4_index(uintptr_t addr)
{
    return (addr >> 39) & 0x1FF;
}

static inline uint64_t pdpt_index(uintptr_t addr)
{
    return (addr >> 30) & 0x1FF;
}

static inline uint64_t pd_index(uintptr_t addr)
{
    return (addr >> 21) & 0x1FF;
}

static inline uint64_t pt_index(uintptr_t addr)
{
    return (addr >> 12) & 0x1FF;
}

/* --- CR3 helpers --- */

static inline uintptr_t read_cr3(void)
{
    uintptr_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uintptr_t read_cr2(void)
{
    uintptr_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uintptr_t val)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

/* --- TLB management --- */

void vmm_flush_tlb(uintptr_t virt)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_flush_tlb_all(void)
{
    write_cr3(read_cr3());
}

/* --- Zero a page-sized allocation --- */

static void zero_page(uintptr_t phys_addr)
{
    uint64_t *page = (uint64_t *)phys_addr;
    uint32_t i;
    for (i = 0; i < VMM_PAGE_SIZE / 8; i++)
        page[i] = 0;
}

/* --- Page table walking / creation --- */

/*
 * Get or create a page table at the next level.
 * If 'create' is true and the entry doesn't exist, allocates a new frame.
 * Returns pointer to the next-level table, or NULL on failure.
 */
static pte_t *get_or_create_table(pte_t *table, uint64_t index, int create)
{
    pte_t entry = table[index];

    /* If already present, return the physical address as a pointer */
    if (entry & VMM_FLAG_PRESENT) {
        /* If it's a huge page (2 MiB), we can't drill down further */
        if (entry & VMM_FLAG_HUGE)
            return (pte_t *)0;

        return (pte_t *)(entry & PTE_ADDR_MASK);
    }

    /* Not present — create if requested */
    if (!create)
        return (pte_t *)0;

    /* Allocate a new page for the table */
    uintptr_t new_frame = pmm_alloc_frame();
    if (new_frame == 0)
        return (pte_t *)0;   /* out of memory */

    /* Zero the new table */
    zero_page(new_frame);

    /* Install the entry: present + writable (+ user if needed later) */
    table[index] = new_frame | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

    return (pte_t *)new_frame;
}

/* --- Public API --- */

int vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

    /* Align addresses to page boundaries */
    virt &= ~((uintptr_t)0xFFF);
    phys &= ~((uintptr_t)0xFFF);

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Walk/create PML4 → PDPT */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 1);
    if (!pdpt) return -1;

    /* Walk/create PDPT → PD */
    pd = get_or_create_table(pdpt, pdpti, 1);
    if (!pd) return -1;

    /* Walk/create PD → PT
     * Note: if PD[pdi] is a 2 MiB huge page, we can't create a PT here.
     * In that case we'd need to split the huge page — for now, fail. */
    pt = get_or_create_table(pd, pdi, 1);
    if (!pt) return -1;

    /* Clear NX flag if CPU doesn't support it (older hardware) */
    if (!cpu_has(CPU_FEATURE_NX))
        flags &= ~VMM_FLAG_NX;

    /* Set the PT entry */
    pt[pti] = phys | flags;

    /* Flush the TLB for this virtual address */
    vmm_flush_tlb(virt);

    return 0;
}

void vmm_unmap_page(uintptr_t virt, int free_frame)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;
    uintptr_t phys;

    virt &= ~((uintptr_t)0xFFF);

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Walk PML4 → PDPT (don't create) */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 0);
    if (!pdpt) return;

    pd = get_or_create_table(pdpt, pdpti, 0);
    if (!pd) return;

    pt = get_or_create_table(pd, pdi, 0);
    if (!pt) return;

    /* Get the physical address before clearing */
    phys = pt[pti] & PTE_ADDR_MASK;

    /* Clear the entry */
    pt[pti] = 0;

    /* Flush TLB */
    vmm_flush_tlb(virt);

    /* Optionally free the physical frame */
    if (free_frame && phys)
        pmm_free_frame(phys);
}

uintptr_t vmm_get_physical(uintptr_t virt)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;
    uint64_t offset;

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);
    offset = virt & 0xFFF;

    /* Walk PML4 → PDPT */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 0);
    if (!pdpt) return 0;

    pd = get_or_create_table(pdpt, pdpti, 0);
    if (!pd) return 0;

    /* Check for 2 MiB huge page at PD level */
    if (pd[pdi] & VMM_FLAG_HUGE) {
        uintptr_t huge_base = pd[pdi] & PTE_ADDR_MASK;
        return huge_base + (virt & 0x1FFFFF);   /* offset within 2 MiB page */
    }

    pt = get_or_create_table(pd, pdi, 0);
    if (!pt) return 0;

    if (!(pt[pti] & VMM_FLAG_PRESENT))
        return 0;

    return (pt[pti] & PTE_ADDR_MASK) + offset;
}

/* --- Page Fault Handler (ISR 14) --- */

static uint64_t page_fault_handler(struct interrupt_frame *frame)
{
    uintptr_t fault_addr = read_cr2();

    /* Try swap handler first — if the page was swapped, bring it back */
    if (swap_handle_fault(fault_addr, frame->err_code)) {
        return (uint64_t)frame;  /* page swapped in, retry instruction */
    }

    /* Try mmap handler — if the page is in an mmap'd region, load it */
    if (mmap_handle_fault(fault_addr, frame->err_code)) {
        return (uint64_t)frame;  /* page loaded from file, retry instruction */
    }

    /* Unhandled page fault — show styled panic screen */
    panic_screen(frame, frame->err_code, "PAGE_FAULT", "vmm.c", 0);

    return (uint64_t)frame;  /* unreachable */
}

/* --- Initialization --- */

void vmm_init(void)
{
    /* Take over the PML4 created by entry.asm */
    kernel_pml4 = (pte_t *)(read_cr3() & PTE_ADDR_MASK);

    /* Register the page fault handler (ISR 14) */
    idt_register_handler(14, page_fault_handler);

    klog(LOG_INFO, "mm", "VMM initialized (PML4 at %p, page fault handler registered)",
           (uint64_t)(uintptr_t)kernel_pml4);
}

/* ---- NX policy: mark all non-text pages as non-executable --------------- */

/* Linker symbols for kernel text boundaries */
extern char __text_start[];
extern char __text_end[];

void vmm_apply_nx_policy(void)
{
    uint64_t pml4i, pdpti, pdi;
    uintptr_t text_start, text_end;
    uint32_t nx_count = 0;

    if (!cpu_has(CPU_FEATURE_NX))
        return;

    text_start = (uintptr_t)__text_start;
    text_end   = (uintptr_t)__text_end;

    /* Only apply NX to the kernel's own address range (1 MiB .. __kernel_end).
     * Skip firmware regions, MMIO, UEFI runtime, and high memory to avoid
     * breaking WHPX synthetic pages and UEFI callbacks. */
    uintptr_t kernel_base = 0x100000;  /* 1 MiB — kernel load address */
    extern char __kernel_end[];
    uintptr_t kernel_top = (uintptr_t)__kernel_end;

    /* Walk all PML4 entries (only first few are populated by boot) */
    for (pml4i = 0; pml4i < 512; pml4i++) {
        pte_t pml4e = kernel_pml4[pml4i];
        pte_t *pdpt;

        if (!(pml4e & VMM_FLAG_PRESENT))
            continue;

        pdpt = (pte_t *)(pml4e & PTE_ADDR_MASK);

        for (pdpti = 0; pdpti < 512; pdpti++) {
            pte_t pdpte = pdpt[pdpti];
            pte_t *pd;

            if (!(pdpte & VMM_FLAG_PRESENT))
                continue;

            /* 1 GiB huge page — skip (too coarse for NX policy) */
            if (pdpte & VMM_FLAG_HUGE)
                continue;

            pd = (pte_t *)(pdpte & PTE_ADDR_MASK);

            for (pdi = 0; pdi < 512; pdi++) {
                pte_t pde = pd[pdi];
                uintptr_t page_base, page_end_addr;

                if (!(pde & VMM_FLAG_PRESENT))
                    continue;

                /* 2 MiB huge page — the common case for boot mappings */
                if (pde & VMM_FLAG_HUGE) {
                    page_base     = (pml4i << 39) | (pdpti << 30) | (pdi << 21);
                    page_end_addr = page_base + (1UL << 21);

                    /* Only NX pages within the kernel's own range */
                    if (page_base < kernel_base || page_end_addr > kernel_top)
                        continue;

                    /* If this 2 MiB page overlaps the text section, leave it executable */
                    if (page_base < text_end && page_end_addr > text_start)
                        continue;

                    pd[pdi] |= VMM_FLAG_NX;
                    nx_count++;
                    continue;
                }

                /* 4 KiB page table — walk PT entries */
                {
                    pte_t *pt = (pte_t *)(pde & PTE_ADDR_MASK);
                    uint32_t pti;
                    for (pti = 0; pti < 512; pti++) {
                        pte_t pte = pt[pti];
                        uintptr_t va;
                        if (!(pte & VMM_FLAG_PRESENT))
                            continue;
                        va = (pml4i << 39) | (pdpti << 30) | (pdi << 21) | (pti << 12);
                        if (va < kernel_base || va >= kernel_top)
                            continue;  /* outside kernel range */
                        if (va >= text_start && va < text_end)
                            continue;  /* text — must execute */
                        pt[pti] |= VMM_FLAG_NX;
                        nx_count++;
                    }
                }
            }
        }
    }

    vmm_flush_tlb_all();
    klog(LOG_INFO, "mm", "NX policy applied: %u pages marked non-executable (text: 0x%x-0x%x)",
         (uint64_t)nx_count, (uint64_t)text_start, (uint64_t)text_end);
}
