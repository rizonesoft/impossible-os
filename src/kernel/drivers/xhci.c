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
#include "kernel/irq.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/boot_splash.h"
#include "kernel/timer.h"

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

/* ---- BIOS/OS handoff (xHCI spec §4.22.1) -------------------------------- */

/* Perform USBLEGSUP handoff: take xHCI ownership from BIOS/firmware.
 * Must be called BEFORE halt/reset so firmware can clean up gracefully.
 * The Extended Capabilities list starts at HCCPARAMS1 bits 31:16 (dword offset
 * from MMIO base).  We search for capability ID 1 (USB Legacy Support). */
static void xhci_bios_handoff(struct xhci_controller *hc)
{
    uint32_t hccparams1 = xhci_read32(hc->mmio_base, XHCI_CAP_HCCPARAMS1);
    uint32_t xecp_off = ((hccparams1 >> 16) & 0xFFFF) << 2;  /* dword → byte offset */

    if (xecp_off == 0) {
        klog(LOG_DEBUG, "xhci", "No extended capabilities — skipping BIOS handoff");
        return;
    }

    /* Walk the extended capabilities linked list */
    uint32_t max_iter = 64;  /* safety limit */
    while (xecp_off != 0 && max_iter-- > 0) {
        uint32_t cap = xhci_read32(hc->mmio_base, xecp_off);
        uint8_t  cap_id   = (uint8_t)(cap & 0xFF);
        uint8_t  next_ptr = (uint8_t)((cap >> 8) & 0xFF);

        if (cap_id == 1) {  /* USB Legacy Support (USBLEGSUP) */
            /* Check if BIOS owns the controller (bit 16 = HC BIOS Owned Semaphore) */
            if (!(cap & (1 << 16))) {
                klog(LOG_DEBUG, "xhci", "BIOS does not own controller — no handoff needed");
                return;
            }

            klog(LOG_DEBUG, "xhci", "USBLEGSUP at offset 0x%x — requesting ownership",
                 (uint64_t)xecp_off);

            /* Set HC OS Owned Semaphore (bit 24) */
            xhci_write32(hc->mmio_base, xecp_off, cap | (1 << 24));

            /* Wait up to 1s for BIOS Owned Semaphore (bit 16) to clear */
            uint32_t timeout = 1000;
            while (timeout > 0) {
                cap = xhci_read32(hc->mmio_base, xecp_off);
                if (!(cap & (1 << 16))) {
                    klog(LOG_INFO, "xhci", "BIOS handoff complete");
                    /* Clear any legacy SMI enables (USBLEGCTLSTS at xecp_off + 4) */
                    xhci_write32(hc->mmio_base, xecp_off + 4, 0);
                    return;
                }
                xhci_delay_us(1000);  /* 1ms */
                timeout--;
            }

            /* Timeout — force-clear BIOS bit and proceed */
            klog(LOG_WARN, "xhci", "BIOS handoff timeout — forcing ownership");
            cap |= (1 << 24);         /* OS owned */
            cap &= ~(1 << 16);        /* Clear BIOS owned */
            xhci_write32(hc->mmio_base, xecp_off, cap);
            xhci_write32(hc->mmio_base, xecp_off + 4, 0);
            return;
        }

        /* Advance to next capability (next_ptr is dword offset) */
        if (next_ptr == 0)
            break;
        xecp_off += (uint32_t)next_ptr << 2;
    }

    klog(LOG_DEBUG, "xhci", "USBLEGSUP not found — no BIOS handoff needed");
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
    {
        void *va = vmm_map_mmio_uc(mmio_phys, hc->mmio_size);
        if (!va) {
            klog(LOG_ERROR, "xhci", "vmm_map_mmio_uc failed for 0x%llx",
                 (uint64_t)mmio_phys);
            return -1;
        }
        hc->mmio_base = (volatile uint8_t *)va;
    }

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

    /* ---- BIOS/OS handoff — BEFORE halt/reset (xHCI spec §4.22.1) ---- */
    POST16(0xD750);
    xhci_bios_handoff(hc);

    /* ---- TODO-09 §5: Inherit controller from bootloader (zero-delay) ---- */
    /* If the bootloader allocated persistent DMA and took over the controller
     * (usb_handover_complete=1), skip ALL kernel init (halt/reset/DCBAA/rings).
     * The controller is still running with our DMA structures — we just need
     * to point kernel data structures at the bootloader's physical addresses
     * and go straight to port enumeration. */
    if (g_boot_info.usb_handover_complete &&
        g_boot_info.usb_controller.active) {
        const struct boot_usb_controller *bc = &g_boot_info.usb_controller;

        klog(LOG_INFO, "xhci",
             "Zero-delay handover: inheriting bootloader DMA (%u pages)",
             (uint64_t)bc->dma_page_count);

        /* Populate controller struct from boot_info */
        hc->max_scratchpads = bc->max_scratchpads;
        hc->rt_base = hc->mmio_base + hc->rts_offset;
        hc->db_base = hc->mmio_base + hc->db_offset;

        /* Point at bootloader-allocated DMA (PMM already reserved these pages) */
        hc->dcbaa      = (uint64_t *)(uintptr_t)bc->dcbaa_phys;
        hc->dcbaa_phys = bc->dcbaa_phys;
        hc->scratchpad_array      = bc->scratchpad_array_phys ?
            (uint64_t *)(uintptr_t)bc->scratchpad_array_phys : (uint64_t *)0;
        hc->scratchpad_array_phys = bc->scratchpad_array_phys;

        /* Point rings at bootloader-allocated memory */
        hc->cmd_ring.trbs    = (struct xhci_trb *)(uintptr_t)bc->cmd_ring_phys;
        hc->cmd_ring.phys    = bc->cmd_ring_phys;
        hc->cmd_ring.size    = 256;
        hc->cmd_ring.enqueue = 0;
        hc->cmd_ring.dequeue = 0;
        hc->cmd_ring.cycle   = 1;

        hc->evt_ring.trbs    = (struct xhci_trb *)(uintptr_t)bc->evt_ring_phys;
        hc->evt_ring.phys    = bc->evt_ring_phys;
        hc->evt_ring.size    = 256;
        hc->evt_ring.enqueue = 0;
        hc->evt_ring.dequeue = 0;
        hc->evt_ring.cycle   = 1;

        hc->erst      = (struct xhci_erst_entry *)(uintptr_t)bc->erst_phys;
        hc->erst_phys = bc->erst_phys;

        /* Verify controller is running (USBSTS.HCH should be 0) */
        {
            uint32_t sts = xhci_read32(hc->op_base, XHCI_OP_USBSTS);
            if (sts & XHCI_STS_HCH) {
                klog(LOG_WARN, "xhci",
                     "Handover controller not running (HCH=1) — falling back to full init");
                /* DEBUG: show fallback on splash */
                boot_splash_status("xHCI HANDOVER FAIL — HCH=1, full init (35s)");
                sleep_ms(35000);
                goto full_init;
            }
        }

        hc->active = 1;
        num_controllers++;

        klog(LOG_INFO, "xhci",
             "xHCI v%u.%u inherited — %u slots, %u ports, skipping halt/reset/alloc",
             (uint64_t)((hc->hci_version >> 8) & 0xFF),
             (uint64_t)(hc->hci_version & 0xFF),
             (uint64_t)hc->max_slots,
             (uint64_t)hc->max_ports);

        POST16(0xD751);

        /* DEBUG: show handover result on splash for bare-metal verification */
        boot_splash_status("xHCI HANDOVER OK — inherited DMA, no halt/reset (35s)");
        sleep_ms(35000);
        boot_splash_status("Enumerating USB ports...");

        /* Skip Intel routing — bootloader already handled it */
        /* Go straight to port enumeration */
        xhci_enumerate_ports(hc);
        return 0;
    }

    /* Log boot_info USB inventory (informational, even if not using handover) */
    if (g_boot_info.usb_discovery_ok && g_boot_info.usb_device_count > 0) {
        uint32_t bi;
        klog(LOG_INFO, "xhci",
             "Bootloader found %u USB device(s) (handover not active — full init)",
             (uint64_t)g_boot_info.usb_device_count);
        for (bi = 0; bi < g_boot_info.usb_device_count; bi++) {
            const struct boot_usb_device *bd = &g_boot_info.usb_devices[bi];
            if (!bd->active) continue;
            klog(LOG_INFO, "xhci", "  boot_info[%u]: VID:%04x PID:%04x class=%u/%u/%u%s%s",
                 (uint64_t)bi,
                 (uint64_t)bd->vendor_id, (uint64_t)bd->product_id,
                 (uint64_t)bd->iface_class, (uint64_t)bd->iface_subclass,
                 (uint64_t)bd->iface_protocol,
                 bd->is_msc ? " [MSC]" : "",
                 bd->is_hid ? " [HID]" : "");
        }
    }
    POST16(0xD751);

    /* DEBUG: no handover — show on splash */
    if (!g_boot_info.usb_handover_complete) {
        boot_splash_status("xHCI NO HANDOVER — full init path (35s)");
        sleep_ms(35000);
    }

full_init:

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

    /* Step 4: Reset controller (USBCMD.HCRST = 1, wait HCRST=0 AND CNR=0)
     * Always reset — halt-without-reset leaves command ring in unknown state
     * and Enable Slot commands fail.  Reset clears port CCS, but that's OK
     * because we optimize the port routing delay below instead. */
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

    /* ---- Intel USB port routing (EHCI→xHCI, 7/8/9-series only) ---- */
    /* XUSB2PR (0xD0) and USB3_PSSEN (0xD8) only exist on Intel 7/8/9-series
     * chipsets that have both EHCI and xHCI.  On 100-series+ (Sunrise Point
     * and later, including 500-series i5-11600K), EHCI is removed entirely
     * and these registers are reserved/repurposed.  Writing to them on modern
     * hardware causes false positives and wasted 500ms delays.
     *
     * Check: only touch XUSB2PR if an EHCI controller (prog-if 0x20) exists
     * on the same PCI bus as this xHCI controller. */
    {
        uint16_t vid = pci_read16(bus, dev, func, PCI_VENDOR_ID);
        if (vid == 0x8086) {  /* Intel only */
            /* Scan for EHCI controller on same bus */
            int ehci_found = 0;
            {
                uint8_t d, f;
                for (d = 0; d < PCI_MAX_DEV && !ehci_found; d++) {
                    for (f = 0; f < PCI_MAX_FUNC && !ehci_found; f++) {
                        uint16_t v = pci_read16(bus, d, f, PCI_VENDOR_ID);
                        if (v == 0xFFFF) continue;
                        if (pci_read8(bus, d, f, PCI_CLASS)    == 0x0C &&
                            pci_read8(bus, d, f, PCI_SUBCLASS) == 0x03 &&
                            pci_read8(bus, d, f, PCI_PROG_IF)  == 0x20) {
                            ehci_found = 1;
                            klog(LOG_INFO, "xhci",
                                 "EHCI found at %u:%u.%u — XUSB2PR routing needed",
                                 (uint64_t)bus, (uint64_t)d, (uint64_t)f);
                        }
                    }
                }
            }

            if (ehci_found) {
                /* 7/8/9-series: route USB 2.0 ports from EHCI to xHCI */
                uint32_t xusb2pr = pci_read32(bus, dev, func, 0xD0);
                pci_write32(bus, dev, func, 0xD0, xusb2pr | 0xFFFFFFFF);
                klog(LOG_DEBUG, "xhci", "Intel XUSB2PR: 0x%x -> 0x%x",
                     (uint64_t)xusb2pr,
                     (uint64_t)pci_read32(bus, dev, func, 0xD0));

                uint32_t usb3pssen = pci_read32(bus, dev, func, 0xD8);
                pci_write32(bus, dev, func, 0xD8, usb3pssen | 0xFFFFFFFF);
                klog(LOG_DEBUG, "xhci", "Intel USB3_PSSEN: 0x%x -> 0x%x",
                     (uint64_t)usb3pssen,
                     (uint64_t)pci_read32(bus, dev, func, 0xD8));

                /* Wait for devices to re-appear after EHCI→xHCI routing */
                xhci_delay_us(500000);
            } else {
                klog(LOG_INFO, "xhci",
                     "No EHCI on bus %u — skipping XUSB2PR + 500ms (modern Intel)",
                     (uint64_t)bus);
            }
        }
    }

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
                    uint16_t did = pci_read16((uint8_t)bus, dev, func, PCI_DEVICE_ID);
                    klog(LOG_INFO, "xhci", "Found xHCI at PCI %u:%u.%u (VID:DID %04x:%04x)",
                         (uint64_t)bus, (uint64_t)dev, (uint64_t)func,
                         (uint64_t)vid, (uint64_t)did);
                    if (xhci_init_controller((uint8_t)bus, dev, func) != 0)
                        klog(LOG_ERROR, "xhci", "Controller init failed at %u:%u.%u",
                             (uint64_t)bus, (uint64_t)dev, (uint64_t)func);
                }
            }
        }
    }

    if (num_controllers == 0) {
        klog(LOG_DEBUG, "xhci", "No xHCI controllers found");
    }

    return num_controllers;
}

/* ---- §5 Phase C: Interrupt-driven hot-plug -------------------------------- */

static uint8_t xhci_irq_vector = 0;

/* Process Port Status Change events from the event ring.
 * Called from ISR context — must not block. */
static void xhci_process_port_events(struct xhci_controller *hc)
{
    struct xhci_trb evt;

    while (xhci_event_poll(hc, &evt)) {
        uint32_t trb_type = (evt.control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;

        if (trb_type == XHCI_TRB_PORT_STATUS) {
            /* Port Status Change Event: parameter bits 31:24 = port ID */
            uint8_t port_id = (uint8_t)((evt.parameter >> 24) & 0xFF);
            uint32_t portsc_offset = XHCI_PORTSC_BASE + (port_id - 1) * XHCI_PORTSC_STRIDE;
            uint32_t portsc = xhci_read32(hc->op_base, portsc_offset);

            klog(LOG_INFO, "xhci", "Port %u status change: PORTSC=0x%x (CCS=%u CSC=%u)",
                 (uint64_t)port_id, (uint64_t)portsc,
                 (uint64_t)((portsc & XHCI_PORTSC_CCS) ? 1 : 0),
                 (uint64_t)((portsc & XHCI_PORTSC_CSC) ? 1 : 0));

            /* Clear CSC by writing 1 (RW1C), preserve other bits */
            if (portsc & XHCI_PORTSC_CSC) {
                portsc = (portsc & XHCI_PORTSC_PRESERVE_MASK) | XHCI_PORTSC_CSC;
                xhci_write32(hc->op_base, portsc_offset, portsc);
            }

            /* Re-read after clearing change bits */
            portsc = xhci_read32(hc->op_base, portsc_offset);

            if (portsc & XHCI_PORTSC_CCS) {
                /* Device connected — enumerate it */
                uint8_t speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
                klog(LOG_INFO, "xhci", "Hot-plug: device connected on port %u (speed=%u)",
                     (uint64_t)port_id, (uint64_t)speed);
                xhci_enumerate_device(hc, port_id, speed);
            } else {
                /* Device disconnected — log for now (slot cleanup requires
                 * Disable Slot command which is deferred to TODO-07 §5C full) */
                klog(LOG_INFO, "xhci", "Hot-unplug: device removed from port %u",
                     (uint64_t)port_id);
            }
        }
        /* Command Completion and Transfer events are handled by polling
         * loops in xhci_dev.c — they don't arrive unsolicited. */
    }
}

/* xHCI MSI interrupt handler */
static void xhci_irq_handler(uint8_t vector, void *ctx)
{
    int i;
    (void)vector;
    (void)ctx;

    for (i = 0; i < num_controllers; i++) {
        struct xhci_controller *hc = &controllers[i];
        if (!hc->active)
            continue;

        /* Check and clear Interrupt Pending (IMAN.IP) on Interrupter 0 */
        volatile uint8_t *ir = hc->rt_base + XHCI_IR_OFFSET;
        uint32_t iman = xhci_read32(ir, XHCI_IR_IMAN);
        if (iman & XHCI_IMAN_IP) {
            /* Clear IP by writing 1, keep IE set */
            xhci_write32(ir, XHCI_IR_IMAN, iman | XHCI_IMAN_IP);
            /* Also clear USBSTS.EINT (Event Interrupt) */
            uint32_t sts = xhci_read32(hc->op_base, XHCI_OP_USBSTS);
            if (sts & (1 << 3))  /* EINT bit */
                xhci_write32(hc->op_base, XHCI_OP_USBSTS, (1 << 3));
            xhci_process_port_events(hc);
        }
    }
}

/* Set up MSI for the first xHCI controller (following AHCI pattern).
 * Called after interrupts are enabled and IOAPIC is configured. */
void xhci_setup_interrupts(void)
{
    struct xhci_controller *hc;
    uint8_t bus, dev, func;
    uint16_t status;
    uint8_t cap_off;

    POST16(0xD752);

    if (num_controllers == 0) {
        POST16(0xD753);
        return;
    }

    hc = &controllers[0];
    bus  = hc->pci_bus;
    dev  = hc->pci_dev;
    func = hc->pci_func;

    /* Check PCI Status bit 4 (Capabilities List) */
    status = pci_read16(bus, dev, func, PCI_STATUS);
    if (!(status & (1 << 4))) {
        klog(LOG_WARN, "xhci", "No PCI capabilities — interrupt-driven hot-plug unavailable");
        POST16(0xD753);
        return;
    }

    /* Walk PCI capability list looking for MSI (cap ID 0x05) */
    cap_off = pci_read8(bus, dev, func, 0x34) & 0xFC;  /* Capabilities Pointer */
    while (cap_off != 0) {
        uint8_t cap_id = pci_read8(bus, dev, func, cap_off);
        if (cap_id == 0x05) {
            /* Found MSI capability */
            uint16_t msi_ctrl = pci_read16(bus, dev, func, cap_off + 2);
            uint8_t  msi_data_off;

            xhci_irq_vector = irq_alloc_vector();
            if (!xhci_irq_vector) {
                klog(LOG_WARN, "xhci", "No free IRQ vectors for xHCI MSI");
                break;
            }

            irq_register(xhci_irq_vector, xhci_irq_handler, NULL, "xhci");

            /* Message Address: 0xFEE00000 targets BSP (LAPIC ID 0) */
            pci_write32(bus, dev, func, cap_off + 4, 0xFEE00000);

            /* 64-bit capable? (bit 7 of MSI Control) */
            if (msi_ctrl & (1U << 7)) {
                pci_write32(bus, dev, func, cap_off + 8, 0);  /* Upper addr = 0 */
                msi_data_off = cap_off + 12;
            } else {
                msi_data_off = cap_off + 8;
            }

            /* Message Data: vector number */
            pci_write16(bus, dev, func, msi_data_off, (uint16_t)xhci_irq_vector);

            /* Enable MSI, 1 vector */
            msi_ctrl &= ~(0x7U << 4);  /* MME = 0 (1 message) */
            msi_ctrl |= (1U << 0);     /* MSI Enable */
            pci_write16(bus, dev, func, cap_off + 2, msi_ctrl);

            /* Disable legacy INTx */
            {
                uint16_t cmd = pci_read16(bus, dev, func, PCI_COMMAND);
                cmd |= PCI_CMD_INT_DISABLE;
                pci_write16(bus, dev, func, PCI_COMMAND, cmd);
            }

            klog(LOG_INFO, "xhci", "MSI vector 0x%x registered for hot-plug",
                 (uint64_t)xhci_irq_vector);
            POST16(0xD753);
            return;
        }

        /* MSI-X (cap ID 0x11) — try if MSI not found */
        if (cap_id == 0x11) {
            /* MSI-X is more complex; defer to TODO-02 §5 pci_enable_msix() */
            klog(LOG_DEBUG, "xhci", "MSI-X capability found — deferring to pci_enable_msix()");
        }

        cap_off = pci_read8(bus, dev, func, cap_off + 1) & 0xFC;
    }

    /* No MSI found — hot-plug events still work via event ring polling
     * (existing xhci_wait_command/xhci_wait_transfer consume them).
     * Port status changes won't fire interrupts but boot-time devices
     * are already enumerated. */
    klog(LOG_INFO, "xhci", "No MSI — hot-plug via event ring polling only");
    POST16(0xD753);
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
