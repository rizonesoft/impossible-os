/* ============================================================================
 * xhci.c — xHCI (USB 3.x) Host Controller PCI discovery and MMIO mapping
 *
 * Scans PCI for xHCI controllers (class 0x0C:03:30), maps the MMIO BAR,
 * enables Bus Master + Memory Space, and reads capability registers.
 *
 * Reference: xHCI specification 1.2, §4.2 (Host Controller Initialization)
 * ============================================================================ */

#include "kernel/drivers/xhci.h"
#include "kernel/drivers/pci.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"

/* ---- Static state -------------------------------------------------------- */

static struct xhci_controller controllers[XHCI_MAX_CONTROLLERS];
static int num_controllers = 0;

/* ---- MMIO helpers -------------------------------------------------------- */

static inline uint32_t xhci_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static inline uint16_t xhci_read16(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint16_t *)(base + offset);
}

static inline uint8_t xhci_read8(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint8_t *)(base + offset);
}

/* Map MMIO region with uncacheable flags (PCD=1, PWT=1) */
static void xhci_map_mmio(uint64_t phys, uint32_t size)
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

/* ---- PCI discovery ------------------------------------------------------- */

/* Try to initialize an xHCI controller at the given PCI location.
 * Returns 0 on success, -1 on failure. */
static int xhci_init_controller(uint8_t bus, uint8_t dev, uint8_t func)
{
    struct xhci_controller *hc;
    uint32_t bar0, bar1;
    uint64_t mmio_phys;
    uint32_t hcsparams1, hccparams1;

    if (num_controllers >= XHCI_MAX_CONTROLLERS) {
        klog(LOG_WARN, "xhci", "Too many xHCI controllers (max %u)",
             (uint64_t)XHCI_MAX_CONTROLLERS);
        return -1;
    }

    hc = &controllers[num_controllers];

    /* ---- Read 64-bit BAR0/BAR1 ---- */
    bar0 = pci_read32(bus, dev, func, PCI_BAR0);
    bar1 = pci_read32(bus, dev, func, PCI_BAR1);

    /* Verify BAR type: memory (bit 0 = 0) and 64-bit (bits 2:1 = 10b) */
    if (bar0 & 0x01) {
        klog(LOG_ERROR, "xhci", "BAR0 is I/O space (expected memory)");
        return -1;
    }
    if (((bar0 >> 1) & 0x03) != 0x02) {
        klog(LOG_WARN, "xhci", "BAR0 is not 64-bit (type=%u), trying 32-bit",
             (uint64_t)((bar0 >> 1) & 0x03));
        /* Fall through with bar1=0 for 32-bit BARs */
        bar1 = 0;
    }

    /* Reconstruct MMIO base: mask lower 4 bits of BAR0, combine with BAR1 */
    mmio_phys = (uint64_t)(bar0 & 0xFFFFFFF0) | ((uint64_t)bar1 << 32);

    if (mmio_phys == 0) {
        klog(LOG_ERROR, "xhci", "BAR0 is zero — no MMIO base");
        return -1;
    }

    /* ---- Enable PCI Command Register ---- */
    {
        uint16_t cmd = pci_read16(bus, dev, func, PCI_COMMAND);
        cmd |= PCI_CMD_BUS_MASTER;     /* bit 2: enable DMA */
        cmd |= PCI_CMD_MEM_SPACE;      /* bit 1: enable MMIO */
        cmd |= PCI_CMD_INT_DISABLE;    /* bit 10: disable legacy INTx */
        pci_write16(bus, dev, func, PCI_COMMAND, cmd);
    }

    /* ---- Map MMIO region (minimum 64 KiB for xHCI) ---- */
    hc->mmio_phys = mmio_phys;
    hc->mmio_size = 0x10000;  /* 64 KiB minimum per xHCI spec */
    xhci_map_mmio(mmio_phys, hc->mmio_size);
    hc->mmio_base = (volatile uint8_t *)mmio_phys;

    /* ---- Read Capability Registers ---- */
    hc->cap_length  = xhci_read8(hc->mmio_base, XHCI_CAP_CAPLENGTH);
    hc->hci_version = xhci_read16(hc->mmio_base, XHCI_CAP_HCIVERSION);

    /* Operational registers start at mmio_base + cap_length */
    hc->op_base = hc->mmio_base + hc->cap_length;

    /* Parse HCSPARAMS1: max slots, interrupters, ports */
    hcsparams1 = xhci_read32(hc->mmio_base, XHCI_CAP_HCSPARAMS1);
    hc->max_slots = hcsparams1 & XHCI_HCS1_MAX_SLOTS_MASK;
    hc->max_intrs = (hcsparams1 & XHCI_HCS1_MAX_INTRS_MASK) >> XHCI_HCS1_MAX_INTRS_SHIFT;
    hc->max_ports = (hcsparams1 & XHCI_HCS1_MAX_PORTS_MASK) >> XHCI_HCS1_MAX_PORTS_SHIFT;

    /* Parse HCCPARAMS1: 64-bit addressing, context size */
    hccparams1 = xhci_read32(hc->mmio_base, XHCI_CAP_HCCPARAMS1);
    hc->ac64 = (hccparams1 & XHCI_HCC1_AC64) ? 1 : 0;
    hc->csz  = (hccparams1 & XHCI_HCC1_CSZ)  ? 1 : 0;

    /* Doorbell and Runtime offsets */
    hc->db_offset  = xhci_read32(hc->mmio_base, XHCI_CAP_DBOFF) & ~0x03;
    hc->rts_offset = xhci_read32(hc->mmio_base, XHCI_CAP_RTSOFF) & ~0x1F;

    /* Store PCI location */
    hc->pci_bus  = bus;
    hc->pci_dev  = dev;
    hc->pci_func = func;

    hc->active = 1;
    num_controllers++;

    /* ---- Log discovery ---- */
    klog(LOG_INFO, "xhci",
         "Found controller at PCI %02x:%02x.%x, MMIO @ 0x%x",
         (uint64_t)bus, (uint64_t)dev, (uint64_t)func, mmio_phys);
    klog(LOG_INFO, "xhci",
         "xHCI v%u.%u (raw 0x%x), %u slots, %u ports, %u intrs, %s-bit, ctx=%uB",
         (uint64_t)((hc->hci_version >> 8) & 0xFF),
         (uint64_t)(hc->hci_version & 0xFF),
         (uint64_t)hc->hci_version,
         (uint64_t)hc->max_slots,
         (uint64_t)hc->max_ports,
         (uint64_t)hc->max_intrs,
         hc->ac64 ? "64" : "32",
         (uint64_t)(hc->csz ? 64 : 32));

    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

int xhci_init(void)
{
    uint16_t bus;
    uint8_t dev, func;

    num_controllers = 0;

    for (bus = 0; bus < PCI_MAX_BUS && num_controllers < XHCI_MAX_CONTROLLERS; bus++) {
        for (dev = 0; dev < PCI_MAX_DEV; dev++) {
            for (func = 0; func < PCI_MAX_FUNC; func++) {
                uint16_t vid = pci_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID);
                if (vid == 0xFFFF)
                    continue;

                uint8_t cls     = pci_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub     = pci_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                uint8_t prog_if = pci_read8((uint8_t)bus, dev, func, PCI_PROG_IF);

                if (cls     == XHCI_PCI_CLASS &&
                    sub     == XHCI_PCI_SUBCLASS &&
                    prog_if == XHCI_PCI_PROG_IF) {
                    xhci_init_controller((uint8_t)bus, dev, func);
                }
            }
        }
    }

    if (num_controllers == 0) {
        klog(LOG_DEBUG, "xhci", "No xHCI controllers found");
    }

    return num_controllers;
}

const struct xhci_controller *xhci_get_controller(int index)
{
    if (index < 0 || index >= num_controllers)
        return (const struct xhci_controller *)0;
    return &controllers[index];
}

int xhci_controller_count(void)
{
    return num_controllers;
}
