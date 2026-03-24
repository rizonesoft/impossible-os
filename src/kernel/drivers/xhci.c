/* ============================================================================
 * xhci.c — xHCI (USB 3.x) Host Controller driver
 *
 * PCI discovery, MMIO BAR mapping, controller halt/reset, DCBAA,
 * scratchpad buffer allocation, TRB ring setup, and controller start.
 *
 * Reference: xHCI specification 1.2, §4.2 (Host Controller Initialization)
 * ============================================================================ */

#include "kernel/drivers/xhci.h"
#include "kernel/drivers/xhci_ring.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/pci.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/pmm.h"
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

static inline void xhci_write32(volatile uint8_t *base, uint32_t offset,
                                uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static inline void xhci_write64(volatile uint8_t *base, uint32_t offset,
                                uint64_t value)
{
    /* Write as two 32-bit halves — some xHCI controllers don't support
     * 64-bit MMIO writes.  Low word first per xHCI spec §5.4.6. */
    *(volatile uint32_t *)(base + offset)     = (uint32_t)(value & 0xFFFFFFFF);
    *(volatile uint32_t *)(base + offset + 4) = (uint32_t)(value >> 32);
}

static void xhci_zero(void *dst, uint64_t bytes)
{
    uint8_t *p = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < bytes; i++)
        p[i] = 0;
}

/* Simple microsecond-granularity busy wait (PIT-based, ~1 µs accuracy) */
static void xhci_delay_us(uint32_t us)
{
    /* Port 0x80 write takes ~1 µs on x86 */
    uint32_t i;
    for (i = 0; i < us; i++)
        __asm__ volatile("outb %%al, $0x80" ::: "memory");
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

    /* ---- Log PCI discovery ---- */
    klog(LOG_INFO, "xhci",
         "Found controller at PCI %02x:%02x.%x, MMIO @ 0x%x",
         (uint64_t)bus, (uint64_t)dev, (uint64_t)func, mmio_phys);

    /* ---- §1.2: Controller initialization sequence ---- */

    /* Step 1: Read HCSPARAMS2 for scratchpad buffer count */
    {
        uint32_t hcsparams2 = xhci_read32(hc->mmio_base, XHCI_CAP_HCSPARAMS2);
        uint32_t spb_hi = (hcsparams2 & XHCI_HCS2_SPB_HI_MASK) >> XHCI_HCS2_SPB_HI_SHIFT;
        uint32_t spb_lo = (hcsparams2 & XHCI_HCS2_SPB_LO_MASK) >> XHCI_HCS2_SPB_LO_SHIFT;
        hc->max_scratchpads = (spb_hi << 5) | spb_lo;
    }

    /* Step 2: Set up register base pointers */
    hc->rt_base = hc->mmio_base + hc->rts_offset;
    hc->db_base = hc->mmio_base + hc->db_offset;

    /* Step 3: Halt controller (USBCMD.RS = 0, wait USBSTS.HCH = 1) */
    {
        uint32_t cmd = xhci_read32(hc->op_base, XHCI_OP_USBCMD);
        cmd &= ~XHCI_CMD_RUN;
        xhci_write32(hc->op_base, XHCI_OP_USBCMD, cmd);

        uint32_t timeout = XHCI_HALT_TIMEOUT_US / XHCI_POLL_INTERVAL_US;
        while (!(xhci_read32(hc->op_base, XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
            if (--timeout == 0) {
                klog(LOG_ERROR, "xhci", "Controller failed to halt");
                return -1;
            }
            xhci_delay_us(XHCI_POLL_INTERVAL_US);
        }
    }
    klog(LOG_DEBUG, "xhci", "Controller halted");

    /* Step 4: Reset controller (USBCMD.HCRST = 1, wait HCRST=0 AND CNR=0) */
    {
        xhci_write32(hc->op_base, XHCI_OP_USBCMD, XHCI_CMD_HCRST);

        uint32_t timeout = XHCI_RESET_TIMEOUT_US / XHCI_POLL_INTERVAL_US;
        while (1) {
            uint32_t cmd = xhci_read32(hc->op_base, XHCI_OP_USBCMD);
            uint32_t sts = xhci_read32(hc->op_base, XHCI_OP_USBSTS);
            if (!(cmd & XHCI_CMD_HCRST) && !(sts & XHCI_STS_CNR))
                break;
            if (--timeout == 0) {
                klog(LOG_ERROR, "xhci", "Controller failed to reset");
                return -1;
            }
            xhci_delay_us(XHCI_POLL_INTERVAL_US);
        }
    }
    klog(LOG_DEBUG, "xhci", "Controller reset complete");

    /* Step 5: Configure MaxSlotsEn */
    xhci_write32(hc->op_base, XHCI_OP_CONFIG, hc->max_slots);

    /* Step 6: Allocate DCBAA — (MaxSlots + 1) × 8 bytes, 64-byte aligned.
     * pmm_alloc_contiguous returns page-aligned memory (4 KiB), which
     * exceeds the 64-byte alignment requirement. */
    {
        uint32_t dcbaa_entries = hc->max_slots + 1;
        /* Always fits in one page (65 entries × 8B = 520B max) */
        uintptr_t dcbaa_phys = pmm_alloc_contiguous(1);
        if (dcbaa_phys == 0) {
            klog(LOG_ERROR, "xhci", "Failed to allocate DCBAA");
            return -1;
        }
        /* Identity-map the DCBAA page */
        xhci_map_mmio(dcbaa_phys, 0x1000);
        hc->dcbaa      = (uint64_t *)dcbaa_phys;
        hc->dcbaa_phys = dcbaa_phys;
        xhci_zero(hc->dcbaa, dcbaa_entries * sizeof(uint64_t));
    }

    /* Step 7: Allocate scratchpad buffers if requested */
    if (hc->max_scratchpads > 0) {
        uint32_t i;
        uint32_t array_pages;
        uintptr_t array_phys;

        klog(LOG_DEBUG, "xhci", "Allocating %u scratchpad buffers",
             (uint64_t)hc->max_scratchpads);

        /* Scratchpad buffer array: max_scratchpads × 8 bytes */
        array_pages = ((hc->max_scratchpads * 8) + 0xFFF) / 0x1000;
        array_phys = pmm_alloc_contiguous(array_pages);
        if (array_phys == 0) {
            klog(LOG_ERROR, "xhci", "Failed to allocate scratchpad array");
            return -1;
        }
        xhci_map_mmio(array_phys, array_pages * 0x1000);
        hc->scratchpad_array      = (uint64_t *)array_phys;
        hc->scratchpad_array_phys = array_phys;
        xhci_zero(hc->scratchpad_array, hc->max_scratchpads * sizeof(uint64_t));

        /* Allocate individual scratchpad pages */
        for (i = 0; i < hc->max_scratchpads; i++) {
            uintptr_t sp_phys = pmm_alloc_contiguous(1);
            if (sp_phys == 0) {
                klog(LOG_ERROR, "xhci", "Failed to allocate scratchpad %u",
                     (uint64_t)i);
                return -1;
            }
            xhci_map_mmio(sp_phys, 0x1000);
            xhci_zero((void *)sp_phys, 0x1000);
            hc->scratchpad_array[i] = sp_phys;
        }

        /* Store scratchpad array pointer at DCBAA[0] */
        hc->dcbaa[0] = hc->scratchpad_array_phys;
    }

    /* Step 8: Write DCBAAP (64-bit physical address of DCBAA) */
    xhci_write64(hc->op_base, XHCI_OP_DCBAAP, hc->dcbaa_phys);

    /* Step 9: Initialize TRB rings (Command Ring, Event Ring, ERST)
     * Must be done before USBCMD.RS=1 per xHCI spec §4.2 step 6-7 */
    if (xhci_rings_init(hc) != 0)
        return -1;

    /* Step 10: Start controller (USBCMD.RS = 1, wait USBSTS.HCH = 0) */
    {
        uint32_t cmd = xhci_read32(hc->op_base, XHCI_OP_USBCMD);
        cmd |= XHCI_CMD_RUN;
        xhci_write32(hc->op_base, XHCI_OP_USBCMD, cmd);

        uint32_t timeout = XHCI_HALT_TIMEOUT_US / XHCI_POLL_INTERVAL_US;
        while (xhci_read32(hc->op_base, XHCI_OP_USBSTS) & XHCI_STS_HCH) {
            if (--timeout == 0) {
                klog(LOG_ERROR, "xhci", "Controller failed to start");
                return -1;
            }
            xhci_delay_us(XHCI_POLL_INTERVAL_US);
        }
    }

    hc->active = 1;
    num_controllers++;

    /* ---- Log initialization complete ---- */
    klog(LOG_INFO, "xhci",
         "xHCI v%u.%u ready, %u slots, %u ports, %u intrs, %u scratchpads",
         (uint64_t)((hc->hci_version >> 8) & 0xFF),
         (uint64_t)(hc->hci_version & 0xFF),
         (uint64_t)hc->max_slots,
         (uint64_t)hc->max_ports,
         (uint64_t)hc->max_intrs,
         (uint64_t)hc->max_scratchpads);

    /* Step 11: Enumerate any already-connected devices */
    xhci_enumerate_ports(hc);

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

struct xhci_controller *xhci_get_controller_mut(int index)
{
    if (index < 0 || index >= num_controllers)
        return (struct xhci_controller *)0;
    return &controllers[index];
}

int xhci_controller_count(void)
{
    return num_controllers;
}
