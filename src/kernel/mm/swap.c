/* ============================================================================
 * swap.c — Swap / Page File Support
 *
 * Disk-backed swap with Clock (second-chance) page replacement.
 * Uses pagefile.sys on the IXFS partition as the backing store.
 *
 *   1. Copy page contents to a swap slot
 *   2. Unmap the page (free the physical frame)
 *   3. Write SWAP_ENCODE_PTE(slot_id) into the PTE
 *
 * Swap in flow (on page fault):
 *   1. Read PTE → decode swap slot ID
 *   2. Allocate a new physical frame
 *   3. Copy swap slot contents to the frame
 *   4. Map the page back (virt → new phys)
 *   5. Free the swap slot
 *
 * Clock algorithm:
 *   Circular scan of tracked pages. For each candidate:
 *   - If Accessed bit clear → victim found
 *   - If Accessed bit set → clear it, advance clock hand
 * ============================================================================ */

#include "kernel/mm/swap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/drivers/framebuffer.h"
#include "codex.h"

/* Path to the pagefile on disk */
#define PAGEFILE_PATH  "C:\\Impossible\\System\\pagefile.sys"

/* --- Swap slot storage (disk-backed via pagefile.sys) --- */
static struct vfs_node *pagefile = (void *)0;  /* open file handle */
static uint8_t   swap_temp_buf[SWAP_SLOT_SIZE] __attribute__((aligned(4096)));
swap_slot_t  swap_slots[SWAP_MAX_SLOTS];
static uint32_t     swap_num_slots = 0;
uint32_t     swap_used = 0;
static uint32_t     swap_inited = 0;

/* --- Clock replacement state --- */
static clock_entry_t clock_entries[CLOCK_MAX_PAGES];
static uint32_t      clock_hand = 0;
static uint32_t      clock_count = 0;

/* --- PTE manipulation helpers --- */

/* Read a PTE value for a virtual address.
 * We walk the page tables manually to get the raw PTE. */
static uint64_t swap_read_pte(uintptr_t virt)
{
    /* The VMM uses identity-mapped page tables.
     * (PML4 → PDPT → PD → PT → Physical frame)
     * We'll read CR3 then walk. */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
    uint32_t pml4_idx = (uint32_t)((virt >> 39) & 0x1FF);
    uint32_t pdpt_idx = (uint32_t)((virt >> 30) & 0x1FF);
    uint32_t pd_idx   = (uint32_t)((virt >> 21) & 0x1FF);
    uint32_t pt_idx   = (uint32_t)((virt >> 12) & 0x1FF);

    if (!(pml4[pml4_idx] & 1)) return 0;
    uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & 1)) return 0;
    /* Check for 1 GiB huge page */
    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) return pdpt[pdpt_idx];

    uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & 1)) {
        /* PD entry not present — but it might have swap bits.
         * However, swap is stored at PT level, so check if PD points to a PT. */
        return 0;
    }

    /* Check for 2 MiB huge page */
    if (pd[pd_idx] & VMM_FLAG_HUGE) return pd[pd_idx];

    uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);
    return pt[pt_idx];
}

/* Write a PTE value for a virtual address.
 * Used to encode swap metadata when Present=0. */
static void swap_write_pte(uintptr_t virt, uint64_t pte_val)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
    uint32_t pml4_idx = (uint32_t)((virt >> 39) & 0x1FF);
    uint32_t pdpt_idx = (uint32_t)((virt >> 30) & 0x1FF);
    uint32_t pd_idx   = (uint32_t)((virt >> 21) & 0x1FF);
    uint32_t pt_idx   = (uint32_t)((virt >> 12) & 0x1FF);

    if (!(pml4[pml4_idx] & 1)) return;
    uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & 1)) return;
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & 1)) return;
    uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);

    pt[pt_idx] = pte_val;
    vmm_flush_tlb(virt);
}

/* --- Page copy helper --- */
static void page_copy(void *dst, const void *src)
{
    uint64_t *d = (uint64_t *)dst;
    const uint64_t *s = (const uint64_t *)src;
    uint32_t i;
    for (i = 0; i < SWAP_SLOT_SIZE / sizeof(uint64_t); i++)
        d[i] = s[i];
}

/* --- API implementation --- */

void swap_init(uint32_t num_slots)
{
    uint32_t i;
    int32_t codex_slots;

    if (num_slots == 0)
        num_slots = SWAP_MAX_SLOTS;
    if (num_slots > SWAP_MAX_SLOTS)
        num_slots = SWAP_MAX_SLOTS;

    /* Check Codex for configured swap size */
    {
        codex_key_t *mem_key = codex_open("System\\Memory");
        if (!mem_key)
            mem_key = codex_create("System\\Memory");
        if (mem_key) {
            if (codex_get_int32(mem_key, "SwapSlots", &codex_slots) == 0) {
                if (codex_slots > 0 && (uint32_t)codex_slots <= SWAP_MAX_SLOTS)
                    num_slots = (uint32_t)codex_slots;
            } else {
                /* Store default value in Codex */
                codex_set_int32(mem_key, "SwapSlots", (int32_t)num_slots);
            }
        }
    }

    /* Create the pagefile on disk */
    if (!vfs_is_mounted('C')) {
        klog(LOG_DEBUG, "swap", "C: drive not mounted — swap disabled");
        return;
    }

    vfs_create(PAGEFILE_PATH, VFS_FILE);
    pagefile = vfs_open(PAGEFILE_PATH, VFS_O_READ | VFS_O_WRITE);
    if (!pagefile) {
        klog(LOG_WARN, "swap", "Failed to open pagefile — swap disabled");
        return;
    }

    /* Pre-allocate pagefile size by writing a zero byte at the end */
    {
        uint8_t zero = 0;
        uint32_t end_offset = num_slots * SWAP_SLOT_SIZE - 1;
        vfs_write(pagefile, end_offset, 1, &zero);
    }

    /* Initialize slot table */
    for (i = 0; i < num_slots; i++) {
        swap_slots[i].in_use = 0;
        swap_slots[i].virt_addr = 0;
    }

    /* Initialize clock entries */
    for (i = 0; i < CLOCK_MAX_PAGES; i++) {
        clock_entries[i].in_use = 0;
        clock_entries[i].virt_addr = 0;
    }

    swap_num_slots = num_slots;
    swap_used = 0;
    clock_hand = 0;
    clock_count = 0;
    swap_inited = 1;

    klog(LOG_INFO, "swap", "Initialized: %u slots (%u KiB) — pagefile.sys",
           (uint64_t)num_slots, (uint64_t)(num_slots * 4));
}

int swap_out(uintptr_t virt_addr)
{
    uint32_t i;
    uintptr_t phys;
    uint32_t slot_offset;
    int rc;

    if (!swap_inited || !pagefile)
        return -1;

    /* Find a free swap slot */
    for (i = 0; i < swap_num_slots; i++) {
        if (!swap_slots[i].in_use)
            break;
    }

    if (i >= swap_num_slots) {
        klog(LOG_WARN, "swap", "No free swap slots");
        return -1;
    }

    /* Get the physical address of the page */
    phys = vmm_get_physical(virt_addr);
    if (!phys) {
        klog(LOG_WARN, "swap", "Cannot swap out %p: not mapped", virt_addr);
        return -1;
    }

    /* Copy page contents to temp buffer, then write to pagefile */
    page_copy(swap_temp_buf, (const void *)virt_addr);
    slot_offset = i * SWAP_SLOT_SIZE;
    rc = vfs_write(pagefile, slot_offset, SWAP_SLOT_SIZE, swap_temp_buf);
    if (rc < 0) {
        klog(LOG_ERROR, "swap", "Failed to write slot %u to pagefile", (uint64_t)i);
        return -1;
    }

    /* Unmap the page and free the physical frame */
    vmm_unmap_page(virt_addr, 1);

    /* Write swap-encoded PTE */
    swap_write_pte(virt_addr, SWAP_ENCODE_PTE(i));

    /* Update slot metadata */
    swap_slots[i].in_use = 1;
    swap_slots[i].virt_addr = virt_addr;
    swap_used++;

    return (int)i;
}

int swap_in(uint32_t swap_id, uintptr_t virt_addr)
{
    uintptr_t new_frame;
    uint32_t slot_offset;
    int rc;

    if (!swap_inited || !pagefile || swap_id >= swap_num_slots)
        return -1;

    if (!swap_slots[swap_id].in_use)
        return -1;

    /* Allocate a new physical frame */
    new_frame = pmm_alloc_frame();
    if (!new_frame) {
        /* If OOM, try to swap out another page first */
        uintptr_t victim = swap_clock_victim();
        if (victim) {
            swap_out(victim);
            new_frame = pmm_alloc_frame();
        }
        if (!new_frame) {
            klog(LOG_ERROR, "swap", "Cannot swap in: out of physical memory");
            return -1;
        }
    }

    /* Read swap slot contents from pagefile into temp buffer */
    slot_offset = swap_id * SWAP_SLOT_SIZE;
    rc = vfs_read(pagefile, slot_offset, SWAP_SLOT_SIZE, swap_temp_buf);
    if (rc < 0) {
        klog(LOG_ERROR, "swap", "Failed to read slot %u from pagefile", (uint64_t)swap_id);
        pmm_free_frame(new_frame);
        return -1;
    }

    /* Map the new frame at the virtual address */
    vmm_map_page(virt_addr, new_frame, VMM_KERNEL_RW);

    /* Copy temp buffer contents to the new page */
    page_copy((void *)virt_addr, swap_temp_buf);

    /* Free the swap slot */
    swap_slots[swap_id].in_use = 0;
    swap_slots[swap_id].virt_addr = 0;
    swap_used--;

    /* Re-register the page for clock tracking */
    swap_clock_register(virt_addr);

    return 0;
}

int swap_handle_fault(uintptr_t fault_addr, uint64_t error_code)
{
    uint64_t pte;
    uint32_t swap_id;

    if (!swap_inited)
        return 0;

    /* Only handle "page not present" faults */
    if (error_code & 1)
        return 0;  /* protection violation, not a swap fault */

    /* Page-align the fault address */
    fault_addr &= ~(uintptr_t)0xFFF;

    /* Read the PTE */
    pte = swap_read_pte(fault_addr);

    /* Check if this PTE has our swap marker */
    if (!SWAP_IS_SWAPPED(pte))
        return 0;  /* not a swapped page */

    /* Decode the swap slot ID */
    swap_id = SWAP_DECODE_PTE(pte);

    klog(LOG_DEBUG, "swap", "Swapping in page at %p (slot %u)",
           fault_addr, (uint64_t)swap_id);

    if (swap_in(swap_id, fault_addr) == 0)
        return 1;  /* handled — retry the faulting instruction */

    return 0;  /* swap_in failed */
}

void swap_clock_register(uintptr_t virt_addr)
{
    uint32_t i;

    /* Check if already registered */
    for (i = 0; i < CLOCK_MAX_PAGES; i++) {
        if (clock_entries[i].in_use && clock_entries[i].virt_addr == virt_addr)
            return;  /* already tracked */
    }

    /* Find a free entry */
    for (i = 0; i < CLOCK_MAX_PAGES; i++) {
        if (!clock_entries[i].in_use) {
            clock_entries[i].virt_addr = virt_addr;
            clock_entries[i].in_use = 1;
            clock_count++;
            return;
        }
    }
}

uintptr_t swap_clock_victim(void)
{
    uint32_t passes;
    uint32_t i;

    if (clock_count == 0)
        return 0;

    /* Scan up to 2× the array to give every page a second chance */
    for (passes = 0; passes < CLOCK_MAX_PAGES * 2; passes++) {
        i = clock_hand;
        clock_hand = (clock_hand + 1) % CLOCK_MAX_PAGES;

        if (!clock_entries[i].in_use)
            continue;

        {
            uintptr_t vaddr = clock_entries[i].virt_addr;
            uint64_t pte = swap_read_pte(vaddr);

            /* Skip non-present pages */
            if (!(pte & VMM_FLAG_PRESENT))
                continue;

            /* Check Accessed bit */
            if (pte & VMM_FLAG_ACCESSED) {
                /* Second chance: clear the Accessed bit */
                swap_write_pte(vaddr, pte & ~VMM_FLAG_ACCESSED);
                continue;
            }

            /* Victim found! Remove from clock tracking */
            clock_entries[i].in_use = 0;
            clock_count--;
            return vaddr;
        }
    }

    /* Fallback: return the first valid entry */
    for (i = 0; i < CLOCK_MAX_PAGES; i++) {
        if (clock_entries[i].in_use) {
            uintptr_t vaddr = clock_entries[i].virt_addr;
            clock_entries[i].in_use = 0;
            clock_count--;
            return vaddr;
        }
    }

    return 0;  /* nothing to evict */
}

uint32_t swap_get_total_slots(void)
{
    return swap_num_slots;
}

uint32_t swap_get_used_slots(void)
{
    return swap_used;
}

uint32_t swap_get_free_slots(void)
{
    return swap_num_slots - swap_used;
}
