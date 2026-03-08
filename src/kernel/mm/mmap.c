/* ============================================================================
 * mmap.c — Memory-mapped file support
 *
 * Eager-loaded file mapping. Pages are read from the backing file into
 * physical frames during the mmap() call (not demand-paged from the fault
 * handler, because VFS/AHCI I/O requires interrupts inside ISR 14).
 *
 * For MAP_PRIVATE, writes trigger a COW fault: the page fault handler
 * copies the page and remaps it writable.  For MAP_SHARED, dirty pages
 * are written back to the file on msync() or munmap().
 *
 * Address space for mmap regions: 0x1_0000_0000 .. 0x1_4000_0000 (1 GiB)
 * ============================================================================ */

#include "kernel/mm/mmap.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/drivers/framebuffer.h"

/* mmap virtual address allocation range (above identity-mapped 4 GiB) */
#define MMAP_BASE   0x100000000ULL   /* 4 GiB */
#define MMAP_END    0x140000000ULL   /* 5 GiB */

/* Global region table */
static mmap_region_t regions[MMAP_MAX_REGIONS];
static uintptr_t     mmap_next_addr = MMAP_BASE;
static uint32_t      mmap_inited = 0;

/* --- Helpers --- */

static uint64_t page_align_up(uint64_t val)
{
    return (val + VMM_PAGE_SIZE - 1) & ~((uint64_t)VMM_PAGE_SIZE - 1);
}

static mmap_region_t *find_region(uintptr_t addr)
{
    uint32_t i;
    for (i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (!regions[i].in_use)
            continue;
        if (addr >= regions[i].base &&
            addr < regions[i].base + regions[i].length)
            return &regions[i];
    }
    return (void *)0;
}

/* --- Init --- */

void mmap_init(void)
{
    uint32_t i;
    for (i = 0; i < MMAP_MAX_REGIONS; i++)
        regions[i].in_use = 0;
    mmap_next_addr = MMAP_BASE;
    mmap_inited = 1;
}

/* --- mmap --- */

void *mmap(void *addr, uint64_t length, uint32_t prot, uint32_t flags,
           struct vfs_node *file, uint32_t offset)
{
    uint32_t i;
    uintptr_t base;
    uint64_t aligned_len;
    mmap_region_t *r;

    if (!mmap_inited)
        mmap_init();

    if (length == 0)
        return MAP_FAILED;

    /* Require a file unless anonymous */
    if (!(flags & MAP_ANON) && !file)
        return MAP_FAILED;

    /* Page-align the length */
    aligned_len = page_align_up(length);

    /* Choose a base address */
    if (addr && (flags & MAP_FIXED)) {
        base = (uintptr_t)addr & ~((uintptr_t)0xFFF);
    } else {
        base = mmap_next_addr;
        mmap_next_addr += aligned_len;
        if (mmap_next_addr > MMAP_END)
            return MAP_FAILED;  /* out of mmap address space */
    }

    /* Find a free region slot */
    r = (void *)0;
    for (i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (!regions[i].in_use) {
            r = &regions[i];
            break;
        }
    }
    if (!r)
        return MAP_FAILED;  /* no free region slots */

    /* Fill in the region descriptor */
    r->base = base;
    r->length = aligned_len;
    r->prot = prot;
    r->flags = flags;
    r->file = file;
    r->offset = offset;
    r->in_use = 1;

    /* For anonymous mappings, pre-map zero-filled pages now.
     * For file-backed mappings, pre-load file data now (eager-load).
     * Note: We cannot demand-page from the page fault handler because
     * VFS/AHCI I/O requires interrupts, which are unavailable inside ISR 14. */
    {
        uintptr_t va;
        for (va = base; va < base + aligned_len; va += VMM_PAGE_SIZE) {
            uintptr_t frame_addr = pmm_alloc_frame();
            uint64_t vmm_flags;

            if (!frame_addr) {
                /* Out of memory — unmap what we've done */
                uintptr_t undo;
                for (undo = base; undo < va; undo += VMM_PAGE_SIZE)
                    vmm_unmap_page(undo, 1);
                r->in_use = 0;
                return MAP_FAILED;
            }

            /* Zero the frame via its identity-mapped address */
            {
                uint8_t *p = (uint8_t *)frame_addr;
                uint32_t k;
                for (k = 0; k < VMM_PAGE_SIZE; k++)
                    p[k] = 0;
            }

            /* For file-backed mappings, read file data into the frame */
            if (file && !(flags & MAP_ANON)) {
                uint32_t file_off_page = offset + (uint32_t)(va - base);
                vfs_read(file, file_off_page, VMM_PAGE_SIZE, (uint8_t *)frame_addr);
            }

            /* Set page flags */
            vmm_flags = VMM_FLAG_PRESENT;
            if (flags & MAP_PRIVATE) {
                /* MAP_PRIVATE: map read-only — COW fault handler upgrades on write */
                if (!(prot & PROT_WRITE)) {
                    /* read-only mapping, no writable flag */
                } else {
                    /* PROT_WRITE + MAP_PRIVATE: map read-only initially for COW */
                }
            } else {
                /* MAP_SHARED or MAP_ANON: directly writable if requested */
                if (prot & PROT_WRITE)
                    vmm_flags |= VMM_FLAG_WRITABLE;
            }

            vmm_map_page(va, frame_addr, vmm_flags);
        }
    }

    return (void *)base;
}

/* --- munmap --- */

int munmap(void *addr, uint64_t length)
{
    mmap_region_t *r;
    uintptr_t base = (uintptr_t)addr & ~((uintptr_t)0xFFF);
    uintptr_t va;

    (void)length;  /* we unmap the entire region */

    r = find_region(base);
    if (!r)
        return -1;

    /* Flush dirty shared pages to file before unmapping */
    if ((r->flags & MAP_SHARED) && r->file) {
        msync((void *)r->base, r->length);
    }

    /* Unmap all pages in the region */
    for (va = r->base; va < r->base + r->length; va += VMM_PAGE_SIZE) {
        if (vmm_get_physical(va))
            vmm_unmap_page(va, 1);
    }

    r->in_use = 0;
    return 0;
}

/* --- msync --- */

int msync(void *addr, uint64_t length)
{
    mmap_region_t *r;
    uintptr_t base = (uintptr_t)addr & ~((uintptr_t)0xFFF);
    uintptr_t va;
    uint32_t file_off;

    (void)length;  /* we sync the entire region */

    r = find_region(base);
    if (!r || !r->file)
        return -1;

    /* Only MAP_SHARED can be synced back */
    if (!(r->flags & MAP_SHARED))
        return 0;

    /* Write each mapped page back to the file */
    for (va = r->base; va < r->base + r->length; va += VMM_PAGE_SIZE) {
        uintptr_t phys = vmm_get_physical(va);
        if (!phys)
            continue;  /* page was never loaded */

        file_off = r->offset + (uint32_t)(va - r->base);
        vfs_write(r->file, file_off, VMM_PAGE_SIZE, (const uint8_t *)va);
    }

    return 0;
}

/* --- Page fault handler hook (COW for MAP_PRIVATE writes) --- */

int mmap_handle_fault(uintptr_t fault_addr, uint64_t error_code)
{
    mmap_region_t *r;
    uintptr_t page_addr;
    uintptr_t frame;
    uintptr_t old_phys;

    /* Only handle write faults (error_code bit 1 = write) */
    if (!(error_code & 2))
        return 0;

    /* Page-align the fault address */
    page_addr = fault_addr & ~((uintptr_t)0xFFF);

    r = find_region(page_addr);
    if (!r)
        return 0;  /* not an mmap region */

    /* Only MAP_PRIVATE pages with PROT_WRITE get COW treatment */
    if (!(r->flags & MAP_PRIVATE) || !(r->prot & PROT_WRITE))
        return 0;

    old_phys = vmm_get_physical(page_addr);
    if (!old_phys)
        return 0;  /* page not loaded — shouldn't happen with eager load */

    /* Copy-on-write: allocate new frame, copy data, remap writable */
    frame = pmm_alloc_frame();
    if (!frame)
        return 0;  /* OOM */

    /* Copy via identity-mapped physical addresses */
    {
        uint8_t *dst = (uint8_t *)frame;
        uint8_t *src = (uint8_t *)old_phys;
        uint32_t k;
        for (k = 0; k < VMM_PAGE_SIZE; k++)
            dst[k] = src[k];
    }

    /* Remap with write permission */
    vmm_unmap_page(page_addr, 1);
    vmm_map_page(page_addr, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    return 1;  /* handled — retry instruction */
}

