/* ============================================================================
 * vmm.c -- Virtual Memory Manager (x86-64 4-level paging)
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
#include "kernel/mm/user_range.h"
#include "kernel/cpuid.h"
#include "kernel/idt.h"
#include "kernel/klog.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/mm/swap.h"
#include "kernel/mm/mmap.h"
#include "kernel/panic.h"

/* Page table entry -- 64-bit */
typedef uint64_t pte_t;

/* Number of entries per page table level */
#define PT_ENTRIES 512

/* Mask for extracting physical address from a PTE (bits 12-51) */
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

/* Kernel PML4 -- read from CR3 at init */
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

    /* Not present -- create if requested */
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
     * In that case we'd need to split the huge page -- for now, fail. */
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

/* --- Guard page tracking -------------------------------------------------- */

/* Track guard page ranges so the page fault handler can identify them.
 * Fixed-size table -- guard pages are allocated once at boot. */
#define MAX_GUARD_PAGES 32

struct guard_page_entry {
    uintptr_t addr;
    const char *label;
};

static struct guard_page_entry guard_pages[MAX_GUARD_PAGES];
static uint32_t guard_page_count;

static void guard_page_register(uintptr_t addr, const char *label)
{
    if (guard_page_count < MAX_GUARD_PAGES) {
        guard_pages[guard_page_count].addr = addr & ~((uintptr_t)0xFFF);
        guard_pages[guard_page_count].label = label;
        guard_page_count++;
    }
}

static const char *guard_page_lookup(uintptr_t fault_addr)
{
    uintptr_t page = fault_addr & ~((uintptr_t)0xFFF);
    uint32_t i;
    for (i = 0; i < guard_page_count; i++) {
        if (guard_pages[i].addr == page)
            return guard_pages[i].label;
    }
    return (const char *)0;
}

/* --- Page Fault Handler (ISR 14) --- */

static uint64_t page_fault_handler(struct interrupt_frame *frame)
{
    uintptr_t fault_addr = read_cr2();

    /* Check guard pages first -- these are intentional not-present pages.
     * A guard page hit means stack overflow, heap overflow, or buffer overrun.
     * Report with a specific message instead of a generic page fault. */
    {
        const char *label = guard_page_lookup(fault_addr);
        if (label) {
            panic_screen(frame, frame->err_code, label, "vmm.c", 0);
            return (uint64_t)frame;  /* unreachable */
        }
    }

    /* Try swap handler -- if the page was swapped, bring it back */
    if (swap_handle_fault(fault_addr, frame->err_code)) {
        return (uint64_t)frame;  /* page swapped in, retry instruction */
    }

    /* Try mmap handler -- if the page is in an mmap'd region, load it */
    if (mmap_handle_fault(fault_addr, frame->err_code)) {
        return (uint64_t)frame;  /* page loaded from file, retry instruction */
    }

    /* Unhandled page fault -- show styled panic screen */
    panic_screen(frame, frame->err_code, "PAGE_FAULT", "vmm.c", 0);

    return (uint64_t)frame;  /* unreachable */
}

/* --- Per-process page tables -------------------------------------------- */

/* USER_PD_INDEX provided by kernel/mm/user_range.h (single source of truth) */

uintptr_t vmm_create_user_pml4(void)
{
    uintptr_t pml4_phys, pdpt_phys, pd_phys, pt_phys;
    pte_t *pml4, *pdpt, *pd, *pt;
    pte_t *kern_pdpt, *kern_pd;
    uint32_t i;

    /* Allocate 4 pages: PML4, PDPT, PD, PT for the user 2 MiB region */
    pml4_phys = pmm_alloc_frame();
    pdpt_phys = pmm_alloc_frame();
    pd_phys   = pmm_alloc_frame();
    pt_phys   = pmm_alloc_frame();
    if (!pml4_phys || !pdpt_phys || !pd_phys || !pt_phys)
        return 0;

    pml4 = (pte_t *)pml4_phys;
    pdpt = (pte_t *)pdpt_phys;
    pd   = (pte_t *)pd_phys;
    pt   = (pte_t *)pt_phys;

    /* Zero all new tables */
    zero_page(pml4_phys);
    zero_page(pdpt_phys);
    zero_page(pd_phys);
    zero_page(pt_phys);

    /* Get kernel's PDPT and PD (via identity mapping) */
    kern_pdpt = (pte_t *)(kernel_pml4[0] & PTE_ADDR_MASK);
    kern_pd   = (pte_t *)(kern_pdpt[0] & PTE_ADDR_MASK);

    /* Clone kernel PD entries into the new PD (all 512 entries).
     * These are 2 MiB huge pages -- kernel-only, no User bit. */
    for (i = 0; i < PT_ENTRIES; i++)
        pd[i] = kern_pd[i] & ~((pte_t)VMM_FLAG_USER);

    /* Split PD[USER_PD_INDEX] (0x800000-0x9FFFFF) from a 2 MiB huge page
     * into 512 x 4 KiB pages. This lets us set User bit on individual pages. */
    {
        uintptr_t base_phys = (uintptr_t)USER_PD_INDEX << 21;
        uint32_t guard_idx = (USER_ELF_END - base_phys) / VMM_PAGE_SIZE;
        for (i = 0; i < PT_ENTRIES; i++) {
            uintptr_t page_phys = base_phys + (uintptr_t)i * VMM_PAGE_SIZE;
            /* Default: kernel-only (Present + Writable, no User) */
            pt[i] = page_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        }
        /* Guard page at USER_ELF_END (0x900000): clear Present bit entirely.
         * This catches both user-mode AND kernel-mode overflow past the user
         * range -- any access triggers #PF regardless of CPL. */
        pt[guard_idx] = 0;
    }

    /* Replace the huge page PD entry with the fine-grained PT */
    pd[USER_PD_INDEX] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                                | VMM_FLAG_USER;

    /* Copy remaining kernel PDPT entries (PDPT[1..3] for 1-4 GiB) */
    for (i = 0; i < PT_ENTRIES; i++)
        pdpt[i] = kern_pdpt[i];
    /* Override PDPT[0] to point to our cloned PD.
     * User bit required at EVERY level for ring 3 access. */
    pdpt[0] = pd_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    /* PML4[0] points to our cloned PDPT */
    pml4[0] = pdpt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    /* Copy any other kernel PML4 entries (currently only [0] is used) */
    for (i = 1; i < PT_ENTRIES; i++)
        pml4[i] = kernel_pml4[i];

    return pml4_phys;
}

void vmm_set_user_page(uintptr_t pml4_phys, uintptr_t virt)
{
    pte_t *pml4, *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

    pml4 = (pte_t *)pml4_phys;
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    pdpt = (pte_t *)(pml4[pml4i] & PTE_ADDR_MASK);
    if (!pdpt) return;
    pd = (pte_t *)(pdpt[pdpti] & PTE_ADDR_MASK);
    if (!pd) return;

    /* Ensure User bit is set at all upper levels (PML4, PDPT, PD).
     * x86-64 requires User bit at EVERY level for ring-3 access. */
    pml4[pml4i] |= VMM_FLAG_USER;
    pdpt[pdpti] |= VMM_FLAG_USER;

    /* If PD entry is a 2 MiB huge page, split it into 4 KiB PTEs so we
     * can set the User bit on individual pages.  This makes vmm_set_user_page
     * work for ANY address in the per-process PML4, not just the pre-split
     * user ELF range.  Required for future Win32 PE loading, VirtualAlloc,
     * and per-process heap at arbitrary addresses. */
    if (pd[pdi] & VMM_FLAG_HUGE) {
        uintptr_t huge_phys = pd[pdi] & PTE_ADDR_MASK;
        uint64_t  old_flags = pd[pdi] & ~(PTE_ADDR_MASK | VMM_FLAG_HUGE);
        uintptr_t pt_frame  = pmm_alloc_frame();
        uint32_t  j;
        if (!pt_frame) {
            klog(LOG_ERROR, "mm", "vmm_set_user_page: cannot split PD[%u] (OOM)",
                 (uint64_t)pdi);
            return;
        }
        pt = (pte_t *)pt_frame;
        for (j = 0; j < PT_ENTRIES; j++)
            pt[j] = (huge_phys + (uintptr_t)j * VMM_PAGE_SIZE) | old_flags;
        pd[pdi] = pt_frame | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
    }

    pt = (pte_t *)(pd[pdi] & PTE_ADDR_MASK);
    if (!pt) return;

    /* Set User bit on the specific 4 KiB page */
    pt[pti] |= VMM_FLAG_USER;
}

void vmm_destroy_user_pml4(uintptr_t pml4_phys)
{
    pte_t *pml4, *pdpt, *pd;

    if (!pml4_phys) return;

    pml4 = (pte_t *)pml4_phys;
    pdpt = (pte_t *)(pml4[0] & PTE_ADDR_MASK);
    if (pdpt) {
        pd = (pte_t *)(pdpt[0] & PTE_ADDR_MASK);
        if (pd) {
            /* Free the PT for the user region */
            pte_t *pt = (pte_t *)(pd[USER_PD_INDEX] & PTE_ADDR_MASK);
            if (pt && !(pd[USER_PD_INDEX] & VMM_FLAG_HUGE))
                pmm_free_frame((uintptr_t)pt);
            pmm_free_frame((uintptr_t)pd);
        }
        pmm_free_frame((uintptr_t)pdpt);
    }
    pmm_free_frame(pml4_phys);
}

uintptr_t vmm_get_kernel_cr3(void)
{
    return (uintptr_t)kernel_pml4;
}

/* --- MMIO mapping (UC -- Uncacheable) ------------------------------------ */

/* Bump allocator for MMIO virtual addresses.
 * Starts at 4 GiB (above the identity map) and grows upward.
 * Each mapping is page-aligned. */
#define MMIO_VA_BASE  0x100000000ULL  /* 4 GiB */
#define MMIO_VA_LIMIT 0x140000000ULL  /* 5 GiB -- 1 GiB for MMIO */

static uintptr_t s_mmio_next_va = MMIO_VA_BASE;

/* UC flags: PCD=1 (bit 4) + PWT=1 (bit 3) = Strong Uncacheable */
#define VMM_MMIO_UC  (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | \
                      VMM_FLAG_NOCACHE | VMM_FLAG_WRITETHROUGH | VMM_FLAG_NX)

void *vmm_map_mmio_uc(uint64_t phys_base, uint32_t size)
{
    uintptr_t va, va_start;
    uint32_t pages, i;

    if (size == 0 || (phys_base & 0xFFF) != 0)
        return (void *)0;

    pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    va_start = s_mmio_next_va;

    if (va_start + (uint64_t)pages * VMM_PAGE_SIZE > MMIO_VA_LIMIT)
        return (void *)0;  /* out of MMIO VA space */

    for (i = 0; i < pages; i++) {
        va = va_start + (uint64_t)i * VMM_PAGE_SIZE;
        if (vmm_map_page(va, phys_base + (uint64_t)i * VMM_PAGE_SIZE,
                          VMM_MMIO_UC) != 0)
            return (void *)0;
    }

    s_mmio_next_va = va_start + (uint64_t)pages * VMM_PAGE_SIZE;

    return (void *)va_start;
}

/* WC flags: PWT=1 (bit 3), PCD=0 -> PAT index 1 = Write-Combining.
 * Requires PAT MSR entry 1 to be programmed as WC (boot_phase0). */
#define VMM_MMIO_WC  (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | \
                      VMM_FLAG_WRITETHROUGH | VMM_FLAG_NX)

void *vmm_map_mmio_wc(uint64_t phys_base, uint32_t size)
{
    uintptr_t va, va_start;
    uint32_t pages, i;

    if (size == 0 || (phys_base & 0xFFF) != 0)
        return (void *)0;

    pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    va_start = s_mmio_next_va;

    if (va_start + (uint64_t)pages * VMM_PAGE_SIZE > MMIO_VA_LIMIT)
        return (void *)0;  /* out of MMIO VA space */

    for (i = 0; i < pages; i++) {
        va = va_start + (uint64_t)i * VMM_PAGE_SIZE;
        if (vmm_map_page(va, phys_base + (uint64_t)i * VMM_PAGE_SIZE,
                          VMM_MMIO_WC) != 0)
            return (void *)0;
    }

    s_mmio_next_va = va_start + (uint64_t)pages * VMM_PAGE_SIZE;

    return (void *)va_start;
}

void vmm_unmap_mmio(void *virt, uint32_t size)
{
    uintptr_t va = (uintptr_t)virt;
    uint32_t pages, i;

    if (!virt || size == 0)
        return;

    pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    for (i = 0; i < pages; i++)
        vmm_unmap_page(va + (uint64_t)i * VMM_PAGE_SIZE, 0);
}

/* --- Split 2 MiB huge page into 4 KiB pages ----------------------------- */

int vmm_split_huge_page(uintptr_t virt)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi;
    uintptr_t huge_phys;
    uint64_t old_flags;
    uintptr_t pt_frame;
    uint32_t i;

    virt &= ~((uintptr_t)0x1FFFFF);  /* align to 2 MiB boundary */

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);

    pdpt = get_or_create_table(kernel_pml4, pml4i, 0);
    if (!pdpt) return -1;

    pd = get_or_create_table(pdpt, pdpti, 0);
    if (!pd) return -1;

    /* Not a huge page -- already split or not present */
    if (!(pd[pdi] & VMM_FLAG_HUGE))
        return 0;  /* idempotent success */

    huge_phys = pd[pdi] & PTE_ADDR_MASK;
    old_flags = pd[pdi] & ~(PTE_ADDR_MASK | VMM_FLAG_HUGE);

    /* Allocate a page table to replace the huge page */
    pt_frame = pmm_alloc_frame();
    if (!pt_frame) return -1;

    pt = (pte_t *)pt_frame;

    /* Fill 512 x 4 KiB PTEs preserving the same mapping + flags */
    for (i = 0; i < PT_ENTRIES; i++)
        pt[i] = (huge_phys + (uintptr_t)i * VMM_PAGE_SIZE) | old_flags;

    /* Replace the huge page PDE with the new PT */
    pd[pdi] = pt_frame | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

    /* Flush TLB for the entire 2 MiB range */
    vmm_flush_tlb_all();

    return 0;
}

int vmm_install_guard_page(uintptr_t virt, const char *label)
{
    /* Split the containing 2 MiB huge page if needed */
    if (vmm_split_huge_page(virt) != 0)
        return -1;

    /* Clear the PTE to make the page not-present */
    vmm_unmap_page(virt, 0);

    guard_page_register(virt, label);
    return 0;
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
    uintptr_t kernel_base = 0x100000;  /* 1 MiB -- kernel load address */
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

            /* 1 GiB huge page -- skip (too coarse for NX policy) */
            if (pdpte & VMM_FLAG_HUGE)
                continue;

            pd = (pte_t *)(pdpte & PTE_ADDR_MASK);

            for (pdi = 0; pdi < 512; pdi++) {
                pte_t pde = pd[pdi];
                uintptr_t page_base, page_end_addr;

                if (!(pde & VMM_FLAG_PRESENT))
                    continue;

                /* 2 MiB huge page -- the common case for boot mappings */
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

                /* 4 KiB page table -- walk PT entries */
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
                            continue;  /* text -- must execute */
                        pt[pti] |= VMM_FLAG_NX;
                        nx_count++;
                    }
                }
            }
        }
    }

    /* NOTE: U/S bit clearing deferred until per-process page tables exist.
     * The shared identity-mapped address space uses 2 MiB pages; user stacks
     * are kmalloc'd from the kernel heap, so user and kernel data share the
     * same 2 MiB pages.  Clearing U/S on kernel pages breaks user-mode stack
     * access.  SMEP/SMAP are skipped on bare metal for the same reason.
     * See TODO-01-vmm-memory-protection.md §11 and TODO-04 advanced VM. */

    vmm_flush_tlb_all();
    klog(LOG_INFO, "mm", "NX policy applied: %u pages marked non-executable (text: 0x%x-0x%x)",
         (uint64_t)nx_count, (uint64_t)text_start, (uint64_t)text_end);
}
