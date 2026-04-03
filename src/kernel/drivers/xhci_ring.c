/* ============================================================================
 * xhci_ring.c -- xHCI TRB Ring allocation and management
 *
 * Allocates Command Ring, Event Ring, and ERST.  Provides functions to
 * submit commands and poll for events.
 *
 * Reference: xHCI specification 1.2, §4.9 (TRB Rings), §4.11 (ERST)
 * ============================================================================ */

#include "kernel/drivers/xhci_ring.h"
#include "kernel/drivers/xhci.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"

/* ---- MMIO helpers (shared with xhci.c -- keep static to avoid linker dup) - */

static inline uint32_t ring_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static inline void ring_write32(volatile uint8_t *base, uint32_t offset,
                                uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static inline void ring_write64(volatile uint8_t *base, uint32_t offset,
                                uint64_t value)
{
    *(volatile uint32_t *)(base + offset)     = (uint32_t)(value & 0xFFFFFFFF);
    *(volatile uint32_t *)(base + offset + 4) = (uint32_t)(value >> 32);
}

static void ring_zero(void *dst, uint64_t bytes)
{
    uint8_t *p = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < bytes; i++)
        p[i] = 0;
}

/* Map physical pages identity-mapped with uncacheable flags */
static void ring_map(uint64_t phys, uint32_t size)
{
    uint64_t page_start = phys & ~(uint64_t)0xFFF;
    uint64_t page_end   = (phys + size + 0xFFF) & ~(uint64_t)0xFFF;
    uint64_t page;
    uint64_t flags = VMM_KERNEL_RW | VMM_FLAG_NOCACHE | VMM_FLAG_WRITETHROUGH;

    for (page = page_start; page < page_end; page += VMM_PAGE_SIZE) {
        if (vmm_get_physical(page) == 0)
            vmm_map_page(page, page, flags);
    }
}

/* ---- Interrupter 0 register helpers -------------------------------------- */

static volatile uint8_t *ir0_base(struct xhci_controller *hc)
{
    return hc->rt_base + XHCI_IR_OFFSET;
}

/* ---- Command Ring init --------------------------------------------------- */

static int cmd_ring_init(struct xhci_controller *hc)
{
    struct xhci_ring *ring = &hc->cmd_ring;
    struct xhci_trb *link;
    uintptr_t phys;
    uint32_t ring_bytes = XHCI_RING_SIZE * sizeof(struct xhci_trb);

    /* Allocate -- 256 TRBs × 16 B = 4096 B = exactly 1 page */
    phys = pmm_alloc_contiguous(1);
    if (phys == 0) {
        klog(LOG_ERROR, "xhci", "Failed to allocate Command Ring");
        return -1;
    }
    ring_map(phys, ring_bytes);

    ring->trbs    = (struct xhci_trb *)phys;
    ring->phys    = phys;
    ring->size    = XHCI_RING_SIZE;
    ring->enqueue = 0;
    ring->dequeue = 0;
    ring->cycle   = 1;

    ring_zero(ring->trbs, ring_bytes);

    /* Set Link TRB at last slot -- points back to ring start, toggles cycle */
    link = &ring->trbs[XHCI_RING_SIZE - 1];
    link->parameter = ring->phys;
    link->status    = 0;
    link->control   = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT)
                    | XHCI_TRB_TOGGLE_CYCLE
                    | XHCI_TRB_CYCLE;  /* initial cycle = 1 */

    /* Write CRCR -- physical address of ring | cycle bit */
    ring_write64(hc->op_base, XHCI_OP_CRCR, ring->phys | ring->cycle);

    klog(LOG_DEBUG, "xhci", "Command Ring at 0x%x, %u TRBs",
         ring->phys, (uint64_t)ring->size);

    return 0;
}

/* ---- Event Ring init ----------------------------------------------------- */

static int event_ring_init(struct xhci_controller *hc)
{
    struct xhci_ring *ring = &hc->evt_ring;
    struct xhci_erst_entry *erst;
    uintptr_t evt_phys, erst_phys;
    uint32_t ring_bytes = XHCI_RING_SIZE * sizeof(struct xhci_trb);
    volatile uint8_t *ir = ir0_base(hc);

    /* Allocate Event Ring segment -- 256 TRBs × 16 B = 4096 B = 1 page */
    evt_phys = pmm_alloc_contiguous(1);
    if (evt_phys == 0) {
        klog(LOG_ERROR, "xhci", "Failed to allocate Event Ring");
        return -1;
    }
    ring_map(evt_phys, ring_bytes);

    ring->trbs    = (struct xhci_trb *)evt_phys;
    ring->phys    = evt_phys;
    ring->size    = XHCI_RING_SIZE;
    ring->enqueue = 0;
    ring->dequeue = 0;
    ring->cycle   = 1;

    ring_zero(ring->trbs, ring_bytes);

    /* Allocate ERST -- 1 entry × 16 B, fits in one page (over-allocated
     * but pmm_alloc_contiguous minimum is 1 page) */
    erst_phys = pmm_alloc_contiguous(1);
    if (erst_phys == 0) {
        klog(LOG_ERROR, "xhci", "Failed to allocate ERST");
        return -1;
    }
    ring_map(erst_phys, 0x1000);
    ring_zero((void *)erst_phys, 0x1000);

    erst = (struct xhci_erst_entry *)erst_phys;
    erst->ring_base = evt_phys;
    erst->ring_size = XHCI_RING_SIZE;
    erst->reserved  = 0;

    hc->erst      = erst;
    hc->erst_phys = erst_phys;

    /* Configure Interrupter 0 registers */
    ring_write32(ir, XHCI_IR_ERSTSZ, 1);           /* 1 segment */
    ring_write64(ir, XHCI_IR_ERDP,   evt_phys);    /* dequeue = start */
    ring_write64(ir, XHCI_IR_ERSTBA, erst_phys);    /* ERSTBA last per spec */

    klog(LOG_DEBUG, "xhci", "Event Ring at 0x%x, ERST at 0x%x, %u TRBs",
         evt_phys, erst_phys, (uint64_t)ring->size);

    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

int xhci_rings_init(struct xhci_controller *hc)
{
    volatile uint8_t *ir;

    /* Command Ring must be set up BEFORE controller start (USBCMD.RS=1) */
    if (cmd_ring_init(hc) != 0)
        return -1;

    if (event_ring_init(hc) != 0)
        return -1;

    /* Enable interrupts: USBCMD.INTE = 1 */
    {
        uint32_t cmd = ring_read32(hc->op_base, XHCI_OP_USBCMD);
        cmd |= XHCI_CMD_INTE;
        ring_write32(hc->op_base, XHCI_OP_USBCMD, cmd);
    }

    /* Enable Interrupter 0: IMAN.IE = 1 */
    ir = ir0_base(hc);
    ring_write32(ir, XHCI_IR_IMAN, XHCI_IMAN_IE);

    klog(LOG_DEBUG, "xhci", "TRB rings initialized, interrupts enabled");
    return 0;
}

int xhci_cmd_submit(struct xhci_controller *hc, struct xhci_trb *trb)
{
    struct xhci_ring *ring = &hc->cmd_ring;
    struct xhci_trb *dest;
    uint32_t next;

    /* Check for full ring (enqueue catches up to Link TRB slot) */
    if (ring->enqueue >= ring->size - 1) {
        /* This shouldn't happen -- Link TRB is at size-1, we wrap before */
        klog(LOG_ERROR, "xhci", "Command Ring overflow");
        return -1;
    }

    /* Copy TRB to ring slot, set cycle bit */
    dest = &ring->trbs[ring->enqueue];
    dest->parameter = trb->parameter;
    dest->status    = trb->status;
    dest->control   = (trb->control & ~XHCI_TRB_CYCLE)
                    | (ring->cycle ? XHCI_TRB_CYCLE : 0);

    /* Advance enqueue pointer */
    next = ring->enqueue + 1;
    if (next >= ring->size - 1) {
        /* Reached Link TRB -- update its cycle bit and wrap */
        struct xhci_trb *link = &ring->trbs[ring->size - 1];
        link->control = (link->control & ~XHCI_TRB_CYCLE)
                      | (ring->cycle ? XHCI_TRB_CYCLE : 0);
        ring->cycle ^= 1;    /* Toggle cycle on wrap */
        next = 0;
    }
    ring->enqueue = next;

    /* Memory barrier -- ensure TRB write is visible before doorbell */
    __asm__ volatile("mfence" ::: "memory");

    /* Ring doorbell 0 (Host Controller Command) with target = 0 */
    ring_write32(hc->db_base, 0, 0);

    return 0;
}

int xhci_event_poll(struct xhci_controller *hc, struct xhci_trb *out)
{
    struct xhci_ring *ring = &hc->evt_ring;
    struct xhci_trb *evt;
    uint8_t evt_cycle;

    evt = &ring->trbs[ring->dequeue];

    /* Check cycle bit -- if it matches our expected cycle, event is valid */
    evt_cycle = (evt->control & XHCI_TRB_CYCLE) ? 1 : 0;
    if (evt_cycle != ring->cycle)
        return 0;  /* No event available */

    /* Copy event out */
    out->parameter = evt->parameter;
    out->status    = evt->status;
    out->control   = evt->control;

    /* Advance dequeue pointer */
    ring->dequeue++;
    if (ring->dequeue >= ring->size) {
        ring->dequeue = 0;
        ring->cycle ^= 1;  /* Toggle consumer cycle on wrap */
    }

    /* Write ERDP -- tell controller we've consumed this event.
     * Bit 3 (EHB -- Event Handler Busy) must be set to clear it. */
    {
        volatile uint8_t *ir = ir0_base(hc);
        uint64_t erdp = ring->phys
                      + (ring->dequeue * sizeof(struct xhci_trb));
        erdp |= (1 << 3);  /* EHB -- clear Event Handler Busy */
        ring_write64(ir, XHCI_IR_ERDP, erdp);
    }

    return 1;
}
