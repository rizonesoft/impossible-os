/* ============================================================================
 * xhci_dev.c -- USB device enumeration for xHCI
 *
 * Implements the full USB enumeration sequence:
 *   1. Port scanning -- detect connected devices via PORTSC
 *   2. Port reset -- assert PR, wait for PRC
 *   3. Enable Slot -- Command TRB type 9
 *   4. Build Input Context -- Slot Context + EP0 Context
 *   5. Address Device -- Command TRB type 11
 *   6. GET_DESCRIPTOR (Device) -- 18-byte device descriptor
 *   7. GET_DESCRIPTOR (Configuration) -- two-stage: 9B header + full
 *   8. SET_CONFIGURATION
 *   9. Configure Endpoint -- Command TRB type 12
 *
 * Reference: xHCI spec 1.2 §4.3, USB 2.0 spec §9.1 (Device Enumeration)
 * ============================================================================ */

#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/xhci_ring.h"
#include "kernel/drivers/usb_msc.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"
#include "kernel/timer.h"

/* ---- Static state -------------------------------------------------------- */

static struct xhci_device devices[XHCI_MAX_DEVICES];

/* HID report polling -- defined later but armed from xhci_enumerate_device
 * (after the device is published active, so the poller never races a still-
 * inactive device). */
static int  xhci_hid_queue_report(struct xhci_controller *hc,
                                  struct xhci_device *dev);
static void xhci_hid_start_polling(void);

/* Debug: tracks how far USB enumeration got (readable from diagnostic splash)
 * 0=no device, 1=CCS detected, 2=port reset, 3=slot enabled, 4=addressed,
 * 5=descriptor, 6=configured, 7=MSC identified, 8=MSC init done */
static int g_usb_enum_stage = 0;
static int g_usb_ccs_count = 0;

int xhci_get_enum_stage(void) { return g_usb_enum_stage; }
int xhci_get_ccs_count(void)  { return g_usb_ccs_count; }

/* ---- MMIO / memory helpers ----------------------------------------------- */

static inline uint32_t dev_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static inline void dev_write32(volatile uint8_t *base, uint32_t offset,
                               uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void dev_zero(void *dst, uint64_t bytes)
{
    uint8_t *p = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < bytes; i++)
        p[i] = 0;
}

static void dev_copy(void *dst, const void *src, uint64_t bytes)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i;
    for (i = 0; i < bytes; i++)
        d[i] = s[i];
}

/* Simple microsecond-granularity busy wait */
static void dev_delay_us(uint32_t us)
{
    uint32_t i;
    for (i = 0; i < us; i++)
        __asm__ volatile("outb %%al, $0x80" ::: "memory");
}

/* Map physical page identity-mapped with uncacheable flags */
static void dev_map_page(uint64_t phys, uint32_t size)
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

/* ---- Ring doorbell helpers ----------------------------------------------- */

static void dev_ring_doorbell(struct xhci_controller *hc, uint32_t slot_id,
                              uint32_t target)
{
    /* Doorbell array: hc->db_base + slot_id*4.
     * Slot 0 = Host Controller Command.  Slots 1..MaxSlots = device doorbells.
     * Target: 0 = reserved, 1 = EP0, 2 = EP1-OUT, 3 = EP1-IN, etc. */
    dev_write32(hc->db_base, slot_id * 4, target);
}

/* ---- Command helpers ----------------------------------------------------- */

/* Wait for a Command Completion Event (TRB type 33).
 * Returns completion code.  Sets *out_slot_id if non-NULL. */
static uint8_t xhci_wait_command(struct xhci_controller *hc,
                                 uint8_t *out_slot_id)
{
    struct xhci_trb evt;
    uint32_t timeout = 500000; /* 500 ms max */
    uint32_t trb_type;
    uint8_t cc;

    while (timeout > 0) {
        if (xhci_event_poll(hc, &evt)) {
            trb_type = (evt.control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            if (trb_type == XHCI_TRB_CMD_COMPLETE) {
                cc = (evt.status >> XHCI_TRB_CC_SHIFT) & 0xFF;
                if (out_slot_id)
                    *out_slot_id = (evt.control >> XHCI_TRB_SLOT_SHIFT) & 0xFF;
                return cc;
            }
            /* Consume non-command events (e.g. port status change) */
        }
        dev_delay_us(10);
        timeout -= 10;
    }

    klog(LOG_ERROR, "usb", "Command timed out waiting for completion");
    return 0xFF; /* Timeout sentinel */
}

/* Wait for a Transfer Event (TRB type 32).
 * Returns completion code.  Sets *out_bytes if non-NULL (residual length). */
static uint8_t xhci_wait_transfer(struct xhci_controller *hc,
                                  uint32_t *out_bytes)
{
    struct xhci_trb evt;
    uint32_t timeout = 500000; /* 500 ms max */
    uint32_t trb_type;
    uint8_t cc;

    while (timeout > 0) {
        if (xhci_event_poll(hc, &evt)) {
            trb_type = (evt.control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            if (trb_type == XHCI_TRB_TRANSFER_EVT) {
                cc = (evt.status >> XHCI_TRB_CC_SHIFT) & 0xFF;
                if (out_bytes)
                    *out_bytes = evt.status & 0xFFFFFF; /* bits 23:0 */
                return cc;
            }
            /* Consume non-transfer events */
        }
        dev_delay_us(10);
        timeout -= 10;
    }

    klog(LOG_ERROR, "usb", "Transfer timed out waiting for completion");
    return 0xFF;
}

/* ---- EP0 Transfer Ring management ---------------------------------------- */

/* Initialize a Transfer Ring for EP0.
 * Returns 0 on success, -1 on failure. */
static int ep0_ring_init(struct xhci_ring *ring)
{
    uintptr_t phys;
    struct xhci_trb *link;
    uint32_t ring_bytes = XHCI_RING_SIZE * sizeof(struct xhci_trb);

    phys = pmm_alloc_contiguous(1);
    if (phys == 0) {
        klog(LOG_ERROR, "usb", "Failed to allocate EP0 Transfer Ring");
        return -1;
    }
    dev_map_page(phys, ring_bytes);

    ring->trbs    = (struct xhci_trb *)phys;
    ring->phys    = phys;
    ring->size    = XHCI_RING_SIZE;
    ring->enqueue = 0;
    ring->dequeue = 0;
    ring->cycle   = 1;

    dev_zero(ring->trbs, ring_bytes);

    /* Link TRB at last slot -- wraps back to start, toggles cycle */
    link = &ring->trbs[XHCI_RING_SIZE - 1];
    link->parameter = ring->phys;
    link->status    = 0;
    link->control   = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT)
                    | XHCI_TRB_TOGGLE_CYCLE
                    | XHCI_TRB_CYCLE;

    return 0;
}

/* Free a Transfer Ring allocated by ep0_ring_init (single contiguous page). */
static void ep0_ring_free(struct xhci_ring *ring)
{
    if (ring->phys) {
        pmm_free_frame(ring->phys);
        ring->phys = 0;
        ring->trbs = 0;
    }
}

/* Enqueue a TRB on a Transfer Ring (no doorbell -- caller must ring it). */
static int ep0_ring_enqueue(struct xhci_ring *ring, struct xhci_trb *trb)
{
    struct xhci_trb *dest;
    uint32_t next;

    if (ring->enqueue >= ring->size - 1) {
        klog(LOG_ERROR, "usb", "EP0 Transfer Ring overflow");
        return -1;
    }

    dest = &ring->trbs[ring->enqueue];
    dest->parameter = trb->parameter;
    dest->status    = trb->status;
    dest->control   = (trb->control & ~XHCI_TRB_CYCLE)
                    | (ring->cycle ? XHCI_TRB_CYCLE : 0);

    next = ring->enqueue + 1;
    if (next >= ring->size - 1) {
        /* Reached Link TRB -- update its cycle bit and wrap */
        struct xhci_trb *link = &ring->trbs[ring->size - 1];
        link->control = (link->control & ~XHCI_TRB_CYCLE)
                      | (ring->cycle ? XHCI_TRB_CYCLE : 0);
        ring->cycle ^= 1;
        next = 0;
    }
    ring->enqueue = next;

    /* Memory barrier -- ensure TRB is visible before doorbell */
    __asm__ volatile("mfence" ::: "memory");

    return 0;
}

/* ---- Control transfer ---------------------------------------------------- */

/* Perform a control transfer on EP0 of the given slot.
 * setup[8] contains the 8-byte USB Setup Packet.
 * data/data_len: optional data phase buffer (NULL/0 for no-data transfers).
 * dir_in: 1 for device-to-host (IN), 0 for host-to-device (OUT).
 *
 * Returns 0 on success, -1 on failure. */
static int xhci_control_transfer(struct xhci_controller *hc,
                                 struct xhci_device *dev,
                                 const uint8_t setup[8],
                                 void *data, uint32_t data_len,
                                 int dir_in)
{
    struct xhci_trb trb;
    struct xhci_ring *ring = &dev->ep0_ring;
    uint8_t cc;
    int has_data = (data != NULL && data_len > 0);

    /* ---- Setup Stage TRB (type 2) ---- */
    /* Parameter = 8-byte setup packet (Immediate Data) */
    dev_copy(&trb.parameter, setup, 8);
    trb.status  = 8;  /* TRB Transfer Length = 8 */
    trb.control = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT)
                | XHCI_TRB_IDT;  /* Immediate Data -- setup bytes inline */
    if (has_data)
        trb.control |= (dir_in ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT);
    else
        trb.control |= XHCI_TRB_TRT_NO_DATA;

    if (ep0_ring_enqueue(ring, &trb) != 0)
        return -1;

    /* ---- Data Stage TRB (type 3) -- optional ---- */
    if (has_data) {
        trb.parameter = (uint64_t)(uintptr_t)data;  /* DMA buffer address */
        trb.status    = data_len;
        trb.control   = (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT);
        if (dir_in)
            trb.control |= XHCI_TRB_DIR_IN;
        /* No IOC on data stage -- wait for status stage */
        if (ep0_ring_enqueue(ring, &trb) != 0)
            return -1;
    }

    /* ---- Status Stage TRB (type 4) ---- */
    trb.parameter = 0;
    trb.status    = 0;
    trb.control   = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT)
                  | XHCI_TRB_IOC;   /* Interrupt on completion */
    /* Status stage direction is opposite of data stage */
    if (has_data && !dir_in)
        trb.control |= XHCI_TRB_DIR_IN;

    if (ep0_ring_enqueue(ring, &trb) != 0)
        return -1;

    /* Ring doorbell: Doorbell[slot_id] = 1 (DCI for EP0) */
    dev_ring_doorbell(hc, dev->slot_id, 1);

    /* Wait for transfer completion */
    cc = xhci_wait_transfer(hc, NULL);
    if (cc != XHCI_TRB_CC_SUCCESS && cc != XHCI_TRB_CC_SHORT_PKT) {
        klog(LOG_ERROR, "usb",
             "Control transfer failed (slot %u, cc=%u)",
             (uint64_t)dev->slot_id, (uint64_t)cc);
        return -1;
    }

    return 0;
}

/* ---- Bulk transfer ------------------------------------------------------- */

/* Perform a bulk transfer on a non-EP0 endpoint.
 * dir_in: 1 = bulk IN (device to host), 0 = bulk OUT (host to device).
 * Returns 0 on success, -1 on failure. */
int xhci_bulk_transfer(struct xhci_controller *hc,
                       struct xhci_device *dev,
                       struct xhci_ring *ring,
                       void *buf, uint32_t len, int dir_in)
{
    /* xHCI 4.11.7.1: a Normal TRB data buffer must fit in 64 KiB AND must not
     * cross a 64 KiB physical-address boundary. The caller's buffer has no
     * alignment guarantee, so split the request into per-TRB fragments bounded
     * by the next 64 KiB boundary, transferring and waiting on each in turn.
     * A bulk endpoint streams the bytes contiguously regardless of how many
     * TRBs carry them, so this is transparent to the BOT data phase and avoids
     * chained-TD Link-TRB handling. */
    const uint32_t BOUNDARY = 0x10000u;   /* 64 KiB */
    uintptr_t addr = (uintptr_t)buf;
    uint32_t remaining = len;
    uint32_t dci;

    if (dir_in)
        dci = XHCI_DCI(dev->bulk_in_ep, 1);
    else
        dci = XHCI_DCI(dev->bulk_out_ep, 0);

    do {
        struct xhci_trb trb;
        uint8_t cc;
        uint32_t to_boundary = BOUNDARY - (uint32_t)(addr & (BOUNDARY - 1));
        uint32_t frag = remaining < to_boundary ? remaining : to_boundary;

        trb.parameter = (uint64_t)addr;
        trb.status    = frag;        /* 0 only when len==0: a valid zero-length TRB */
        trb.control   = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;

        if (ep0_ring_enqueue(ring, &trb) != 0)
            return -1;

        dev_write32(hc->db_base, dev->slot_id * 4, dci);

        cc = xhci_wait_transfer(hc, NULL);
        if (cc != XHCI_TRB_CC_SUCCESS && cc != XHCI_TRB_CC_SHORT_PKT) {
            klog(LOG_ERROR, "usb", "Bulk %s failed (slot %u, cc=%u)",
                 dir_in ? "IN" : "OUT", (uint64_t)dev->slot_id, (uint64_t)cc);
            return -1;
        }

        addr      += frag;
        remaining -= frag;
    } while (remaining > 0);

    return 0;
}

/* ---- Port speed to max packet size -------------------------------------- */

static uint16_t speed_to_max_packet(uint8_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:   return 8;
    case USB_SPEED_FULL:  return 8;    /* Start with 8, update after GET_DESCRIPTOR */
    case USB_SPEED_HIGH:  return 64;
    case USB_SPEED_SUPER: return 512;
    default:              return 64;
    }
}

static const char *speed_to_str(uint8_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_FULL:  return "full";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    default:              return "unknown";
    }
}

/* ---- Context size helper ------------------------------------------------- */

/* Context struct size: 32 bytes if CSZ=0, 64 bytes if CSZ=1 */
static uint32_t ctx_size(struct xhci_controller *hc)
{
    return hc->csz ? 64 : 32;
}

/* ---- Device enumeration -------------------------------------------------- */

/* Enumerate a single USB device on the given port.
 * port is 1-based (per xHCI spec -- Port 1 = PORTSC offset 0x400).
 * speed is the USB_SPEED_* constant read from PORTSC.
 * Returns 0 on success, -1 on failure. */
int xhci_enumerate_device(struct xhci_controller *hc,
                                 uint8_t port, uint8_t speed)
{
    struct xhci_device *dev = NULL;
    struct xhci_trb cmd;
    uint8_t cc, slot_id;
    uint32_t portsc_offset;
    uint32_t portsc;
    uint32_t csz;
    uint8_t *input_ctx;
    struct xhci_input_ctrl_ctx *input_ctrl;
    struct xhci_slot_ctx *slot_ctx;
    struct xhci_ep_ctx *ep_ctx;
    uint64_t output_ctx_phys, input_ctx_phys;
    uint16_t max_pkt;
    int i;

    /* ---- Step 1: Reset port ---- */
    portsc_offset = XHCI_PORTSC_BASE + (port - 1) * XHCI_PORTSC_STRIDE;
    portsc = dev_read32(hc->op_base, portsc_offset);
    /* Set PR (Port Reset) while preserving RW1C bits */
    portsc = (portsc & XHCI_PORTSC_PRESERVE_MASK) | XHCI_PORTSC_PR;
    dev_write32(hc->op_base, portsc_offset, portsc);

    /* Wait for Port Reset Change (PRC) -- indicates reset complete */
    {
        uint32_t timeout = 500000; /* 500 ms */
        while (timeout > 0) {
            portsc = dev_read32(hc->op_base, portsc_offset);
            if (portsc & XHCI_PORTSC_PRC)
                break;
            dev_delay_us(100);
            timeout -= 100;
        }
        if (!(portsc & XHCI_PORTSC_PRC)) {
            klog(LOG_ERROR, "usb", "Port %u: reset timed out",
                 (uint64_t)port);
            return -1;
        }
    }

    /* Clear PRC by writing 1 to it (RW1C) */
    portsc = dev_read32(hc->op_base, portsc_offset);
    portsc = (portsc & XHCI_PORTSC_PRESERVE_MASK) | XHCI_PORTSC_PRC;
    dev_write32(hc->op_base, portsc_offset, portsc);

    /* Re-read speed after reset (may have changed for USB 2.0 devices) */
    portsc = dev_read32(hc->op_base, portsc_offset);
    speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;

    /* Verify port is enabled after reset */
    if (!(portsc & XHCI_PORTSC_PED)) {
        klog(LOG_WARN, "usb", "Port %u: not enabled after reset",
             (uint64_t)port);
        return -1;
    }

    g_usb_enum_stage = 2;  /* port reset complete */
    klog(LOG_DEBUG, "usb", "Port %u: reset complete, speed=%s",
         (uint64_t)port, speed_to_str(speed));

    /* ---- Step 2: Enable Slot Command (TRB type 9) ---- */
    dev_zero(&cmd, sizeof(cmd));
    cmd.control = (XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);
    if (xhci_cmd_submit(hc, &cmd) != 0) {
        klog(LOG_ERROR, "usb", "Failed to submit Enable Slot command");
        return -1;
    }

    cc = xhci_wait_command(hc, &slot_id);
    if (cc != XHCI_TRB_CC_SUCCESS || slot_id == 0) {
        klog(LOG_ERROR, "usb", "Enable Slot failed (cc=%u, slot=%u)",
             (uint64_t)cc, (uint64_t)slot_id);
        return -1;
    }
    g_usb_enum_stage = 3;  /* slot enabled */
    klog(LOG_DEBUG, "usb", "Slot %u enabled for port %u",
         (uint64_t)slot_id, (uint64_t)port);

    /* ---- Find a free device tracking slot ---- */
    for (i = 0; i < XHCI_MAX_DEVICES; i++) {
        if (!devices[i].active) {
            dev = &devices[i];
            break;
        }
    }
    if (!dev) {
        klog(LOG_ERROR, "usb", "No free device tracking slots");
        return -1;
    }
    dev_zero(dev, sizeof(struct xhci_device));
    dev->slot_id = slot_id;
    dev->port    = port;
    dev->speed   = speed;
    dev->owner   = hc;   /* slot IDs are per-controller; bind the device here */

    /* ---- Step 3: Allocate Output Device Context ---- */
    /* Output Device Context: 1 Slot + 31 Endpoint contexts.
     * Size: 32 × ctx_size bytes.  Must be 64-byte aligned (page-aligned ok). */
    csz = ctx_size(hc);
    {
        uint32_t out_ctx_bytes = 32 * csz;
        uint32_t out_ctx_pages = (out_ctx_bytes + 0xFFF) / 0x1000;
        output_ctx_phys = pmm_alloc_contiguous(out_ctx_pages);
        if (output_ctx_phys == 0) {
            klog(LOG_ERROR, "usb", "Failed to allocate Output Device Context");
            return -1;
        }
        dev_map_page(output_ctx_phys, out_ctx_pages * 0x1000);
        dev_zero((void *)output_ctx_phys, out_ctx_pages * 0x1000);
        dev->output_ctx_phys = output_ctx_phys;
    }

    /* Store in DCBAA[slot_id] */
    hc->dcbaa[slot_id] = output_ctx_phys;

    /* ---- Step 4: Build Input Context ---- */
    /* Input Context layout:
     *   [0] Input Control Context  (1 × csz bytes)
     *   [1] Slot Context           (1 × csz bytes)
     *   [2] EP0 Context            (1 × csz bytes)
     *   ... up to 31 more endpoint contexts */
    {
        uint32_t in_ctx_bytes = 33 * csz;  /* 1 input ctrl + 32 dev ctx entries */
        uint32_t in_ctx_pages = (in_ctx_bytes + 0xFFF) / 0x1000;
        input_ctx_phys = pmm_alloc_contiguous(in_ctx_pages);
        if (input_ctx_phys == 0) {
            klog(LOG_ERROR, "usb", "Failed to allocate Input Context");
            return -1;
        }
        dev_map_page(input_ctx_phys, in_ctx_pages * 0x1000);
        dev_zero((void *)input_ctx_phys, in_ctx_pages * 0x1000);
        dev->input_ctx_phys = input_ctx_phys;
    }
    input_ctx = (uint8_t *)input_ctx_phys;

    /* Input Control Context -- add Slot (A0) and EP0 (A1) */
    input_ctrl = (struct xhci_input_ctrl_ctx *)input_ctx;
    input_ctrl->add_flags = XHCI_INPUT_ADD_SLOT | XHCI_INPUT_ADD_EP0;
    input_ctrl->drop_flags = 0;

    /* Slot Context (at offset 1 × csz from Input Context start) */
    slot_ctx = (struct xhci_slot_ctx *)(input_ctx + 1 * csz);
    slot_ctx->field0 = XHCI_SCTX_ROUTE(0)       /* Route string = 0 for root hub */
                     | XHCI_SCTX_SPEED(speed)
                     | XHCI_SCTX_ENTRIES(1);     /* Context Entries = 1 (Slot+EP0) */
    slot_ctx->field1 = XHCI_SCTX_ROOT_PORT(port); /* Root hub port number */
    slot_ctx->field2 = 0;
    slot_ctx->field3 = 0;

    /* EP0 Context (at offset 2 × csz from Input Context start) */
    max_pkt = speed_to_max_packet(speed);
    dev->max_packet_size = (uint8_t)max_pkt;

    /* Initialize EP0 Transfer Ring */
    if (ep0_ring_init(&dev->ep0_ring) != 0)
        return -1;

    ep_ctx = (struct xhci_ep_ctx *)(input_ctx + 2 * csz);
    ep_ctx->field0 = 0;  /* Interval=0 for control endpoints */
    ep_ctx->field1 = XHCI_EPCTX_CERR(3)      /* 3 retries on error */
                   | XHCI_EPCTX_TYPE(XHCI_EP_TYPE_CONTROL_BI)
                   | XHCI_EPCTX_MAXPKT(max_pkt);
    /* TR Dequeue Pointer: physical address of EP0 ring | DCS (cycle state = 1) */
    ep_ctx->tr_dequeue = dev->ep0_ring.phys | 1;
    ep_ctx->field4 = XHCI_EPCTX_AVG_TRB_LEN(8);  /* Average TRB = 8 (control) */

    /* ---- Step 5: Address Device Command (TRB type 11) ---- */
    dev_zero(&cmd, sizeof(cmd));
    cmd.parameter = input_ctx_phys;
    cmd.control   = (XHCI_TRB_ADDRESS_DEV << XHCI_TRB_TYPE_SHIFT)
                  | ((uint32_t)slot_id << XHCI_TRB_SLOT_SHIFT);
    if (xhci_cmd_submit(hc, &cmd) != 0) {
        klog(LOG_ERROR, "usb", "Failed to submit Address Device command");
        return -1;
    }

    cc = xhci_wait_command(hc, NULL);
    if (cc != XHCI_TRB_CC_SUCCESS) {
        klog(LOG_ERROR, "usb", "Address Device failed (slot %u, cc=%u)",
             (uint64_t)slot_id, (uint64_t)cc);
        return -1;
    }
    g_usb_enum_stage = 4;  /* device addressed */
    klog(LOG_DEBUG, "usb", "Device addressed (slot %u)", (uint64_t)slot_id);

    /* ---- Step 6: GET_DESCRIPTOR (Device, 18 bytes) ---- */
    {
        /* DMA buffer for device descriptor -- must be physically contiguous
         * and not on the stack (stack pages may not be identity-mapped).
         * Use a page from PMM (overkill but safe). */
        uintptr_t desc_phys = pmm_alloc_contiguous(1);
        struct usb_device_descriptor *desc;
        uint8_t setup[8];

        if (desc_phys == 0) {
            klog(LOG_ERROR, "usb", "Failed to allocate descriptor buffer");
            return -1;
        }
        dev_map_page(desc_phys, 0x1000);
        dev_zero((void *)desc_phys, 0x1000);
        desc = (struct usb_device_descriptor *)desc_phys;

        /* USB Setup Packet: GET_DESCRIPTOR (Device) */
        setup[0] = 0x80;  /* bmRequestType: Device-to-Host, Standard, Device */
        setup[1] = USB_REQ_GET_DESCRIPTOR;
        setup[2] = 0x00;  /* Descriptor Index = 0 */
        setup[3] = USB_DESC_DEVICE;  /* Descriptor Type = Device */
        setup[4] = 0x00;  /* wIndex = 0 */
        setup[5] = 0x00;
        setup[6] = 18;    /* wLength = 18 (sizeof device descriptor) */
        setup[7] = 0x00;

        if (xhci_control_transfer(hc, dev, setup, desc, 18, 1) != 0) {
            klog(LOG_ERROR, "usb", "GET_DESCRIPTOR (Device) failed (slot %u)",
                 (uint64_t)slot_id);
            /* Free the DMA buffer (pmm_free is not needed for this flow) */
            return -1;
        }

        /* Parse device descriptor */
        dev->vendor_id  = desc->idVendor;
        dev->product_id = desc->idProduct;
        dev->bcd_usb    = desc->bcdUSB;
        dev->num_configurations = desc->bNumConfigurations;

        /* Update max packet size from descriptor if it differs from default */
        if (desc->bMaxPacketSize0 > 0 && desc->bMaxPacketSize0 != max_pkt) {
            dev->max_packet_size = desc->bMaxPacketSize0;
            /* Note: for a full implementation, we'd need to issue an
             * Evaluate Context Command to update the EP0 max packet size.
             * QEMU's xHCI is lenient about this. */
        }

        g_usb_enum_stage = 5;  /* device descriptor received */
        klog(LOG_INFO, "usb",
             "Device descriptor: USB %x.%02x, VID=%04x PID=%04x, "
             "MaxPkt0=%u, %u configs",
             (uint64_t)(desc->bcdUSB >> 8), (uint64_t)(desc->bcdUSB & 0xFF),
             (uint64_t)desc->idVendor, (uint64_t)desc->idProduct,
             (uint64_t)desc->bMaxPacketSize0,
             (uint64_t)desc->bNumConfigurations);
    }

    /* ---- Step 7: GET_DESCRIPTOR (Configuration) -- two-stage ---- */
    {
        uintptr_t cfg_phys = pmm_alloc_contiguous(1);
        struct usb_config_descriptor *cfg_hdr;
        uint16_t total_len;
        uint8_t setup[8];

        if (cfg_phys == 0) {
            klog(LOG_ERROR, "usb", "Failed to allocate config buffer");
            return -1;
        }
        dev_map_page(cfg_phys, 0x1000);
        dev_zero((void *)cfg_phys, 0x1000);

        /* Stage 1: request 9-byte config descriptor header */
        setup[0] = 0x80;
        setup[1] = USB_REQ_GET_DESCRIPTOR;
        setup[2] = 0x00;  /* Index 0 = first configuration */
        setup[3] = USB_DESC_CONFIGURATION;
        setup[4] = 0x00;
        setup[5] = 0x00;
        setup[6] = 9;     /* wLength = 9 */
        setup[7] = 0x00;

        if (xhci_control_transfer(hc, dev, setup, (void *)cfg_phys, 9, 1) != 0) {
            klog(LOG_ERROR, "usb", "GET_DESCRIPTOR (Config header) failed");
            return -1;
        }

        cfg_hdr = (struct usb_config_descriptor *)cfg_phys;
        total_len = cfg_hdr->wTotalLength;

        /* Validate wTotalLength -- cap at 4096 to protect against malicious devices */
        if (total_len < 9) {
            klog(LOG_ERROR, "usb", "Config descriptor wTotalLength too small (%u)",
                 (uint64_t)total_len);
            return -1;
        }
        if (total_len > XHCI_CONFIG_BUF_MAX) {
            klog(LOG_WARN, "usb",
                 "Config descriptor wTotalLength %u capped to %u",
                 (uint64_t)total_len, (uint64_t)XHCI_CONFIG_BUF_MAX);
            total_len = XHCI_CONFIG_BUF_MAX;
        }

        /* Stage 2: request full configuration descriptor */
        dev_zero((void *)cfg_phys, 0x1000);
        setup[6] = (uint8_t)(total_len & 0xFF);
        setup[7] = (uint8_t)(total_len >> 8);

        if (xhci_control_transfer(hc, dev, setup,
                                  (void *)cfg_phys, total_len, 1) != 0) {
            klog(LOG_ERROR, "usb", "GET_DESCRIPTOR (Config full) failed");
            return -1;
        }

        /* Copy into device tracking struct */
        dev->config_len = total_len;
        dev->config_buf = dev->config_data;
        dev_copy(dev->config_data, (const void *)cfg_phys, total_len);

        cfg_hdr = (struct usb_config_descriptor *)dev->config_data;
        dev->config_value = cfg_hdr->bConfigurationValue;

        klog(LOG_DEBUG, "usb",
             "Config descriptor: %u bytes, %u interfaces, value=%u",
             (uint64_t)total_len,
             (uint64_t)cfg_hdr->bNumInterfaces,
             (uint64_t)cfg_hdr->bConfigurationValue);
    }

    /* ---- Step 8: SET_CONFIGURATION ---- */
    {
        uint8_t setup[8];
        setup[0] = 0x00;  /* bmRequestType: Host-to-Device, Standard, Device */
        setup[1] = USB_REQ_SET_CONFIG;
        setup[2] = dev->config_value;  /* wValue = bConfigurationValue */
        setup[3] = 0x00;
        setup[4] = 0x00;  /* wIndex = 0 */
        setup[5] = 0x00;
        setup[6] = 0x00;  /* wLength = 0 */
        setup[7] = 0x00;

        if (xhci_control_transfer(hc, dev, setup, NULL, 0, 0) != 0) {
            klog(LOG_ERROR, "usb", "SET_CONFIGURATION failed (slot %u)",
                 (uint64_t)slot_id);
            return -1;
        }
        g_usb_enum_stage = 6;  /* configured */
        klog(LOG_DEBUG, "usb", "Configuration %u set (slot %u)",
             (uint64_t)dev->config_value, (uint64_t)slot_id);
    }

    /* ---- Step 9: MSC identification + Configure Endpoint ----
     * Walk config descriptor to find MSC BOT interface, extract Bulk-IN/OUT
     * endpoints, allocate Transfer Rings, and issue Configure Endpoint. */
    xhci_msc_identify(hc, dev);
    if (dev->is_msc) g_usb_enum_stage = 7;  /* MSC identified */

    /* ---- Step 10: MSC BOT initialization (INQUIRY + READ CAPACITY) ---- */
    if (dev->is_msc) {
        if (usb_msc_init(hc, dev) == 0)
            g_usb_enum_stage = 8;  /* MSC init complete */
    } else {
        /* Not an MSC device -- probe for a HID boot keyboard/mouse and
         * configure its Interrupt-IN endpoint (periodic report polling is
         * added by the interrupt-transfer polling work). Boot keyboards/mice
         * are HID-only; a composite MSC+HID device (a flash drive that is also
         * a keyboard) is out of scope here -- it initializes as storage only,
         * which is acceptable for boot input. */
        xhci_hid_identify(hc, dev);
    }

    /* ---- Enumeration complete ---- */
    dev->active = 1;
    klog(LOG_INFO, "usb",
         "Device %04x:%04x enumerated on port %u (slot %u)%s%s",
         (uint64_t)dev->vendor_id, (uint64_t)dev->product_id,
         (uint64_t)port, (uint64_t)slot_id,
         dev->is_msc ? " [MSC]" : "",
         dev->is_hid ? (dev->hid_proto == USB_PROTO_HID_KEYBOARD
                        ? " [HID kbd]" : " [HID mouse]") : "");

    /* Arm HID report polling AFTER the device is published active, so the tick
     * poller (which can fire in ISR context the moment it is registered) can
     * never drain a completion for a not-yet-active device and drop it.
     * Publish int_in_polling BEFORE ringing the doorbell: with the poller
     * already live from an earlier HID device, the first completion of THIS
     * device could otherwise be drained before the gate is set and dropped. */
    if (dev->is_hid && dev->int_in_report) {
        dev->int_in_polling = 1;
        if (xhci_hid_queue_report(hc, dev) == 0)
            xhci_hid_start_polling();
        else
            dev->int_in_polling = 0;   /* queue failed -> not actually polling */
    }

    return 0;
}

/* ---- MSC Identification & Endpoint Configuration ----------------------- */

int xhci_msc_identify(struct xhci_controller *hc, struct xhci_device *dev)
{
    const uint8_t *buf = dev->config_data;
    uint16_t len = dev->config_len;
    uint16_t offset = 0;
    int found_msc = 0;
    int found_bulk_in = 0;
    int found_bulk_out = 0;
    uint8_t current_iface_class = 0;
    uint8_t current_iface_sub   = 0;
    uint8_t current_iface_proto = 0;
    uint8_t current_iface_num   = 0;

    dev->is_msc = 0;

    /* Walk the config descriptor tree linearly (bLength/bDescriptorType) */
    while (offset + 2 <= len) {
        uint8_t desc_len  = buf[offset];
        uint8_t desc_type = buf[offset + 1];

        /* Safety: descriptor length must be >= 2 to avoid infinite loop */
        if (desc_len < 2)
            break;
        /* Don't read past end of buffer */
        if (offset + desc_len > len)
            break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
            /* Parse Interface Descriptor */
            const struct usb_interface_descriptor *iface =
                (const struct usb_interface_descriptor *)(buf + offset);

            current_iface_class = iface->bInterfaceClass;
            current_iface_sub   = iface->bInterfaceSubClass;
            current_iface_proto = iface->bInterfaceProtocol;
            current_iface_num   = iface->bInterfaceNumber;

            /* Check for MSC BOT triple */
            if (current_iface_class == USB_CLASS_MASS_STORAGE &&
                current_iface_sub   == USB_SUBCLASS_SCSI &&
                current_iface_proto == USB_PROTO_BOT) {
                found_msc = 1;
                dev->msc_iface = current_iface_num;
                /* Reset endpoint search for this interface */
                found_bulk_in  = 0;
                found_bulk_out = 0;
            } else {
                /* Not MSC -- if we already found one, stop looking
                 * (next interface descriptor means end of previous) */
                if (found_msc && found_bulk_in && found_bulk_out)
                    break;
                if (found_msc) {
                    /* MSC interface without both endpoints -- bogus device */
                    found_msc = 0;
                }
            }
        } else if (desc_type == USB_DESC_ENDPOINT && desc_len >= 7 && found_msc) {
            /* Parse Endpoint Descriptor (only if inside an MSC interface) */
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(buf + offset);

            uint8_t xfer_type = ep->bmAttributes & USB_EP_ATTR_TYPE_MASK;

            /* Only care about Bulk endpoints for BOT */
            if (xfer_type == USB_EP_ATTR_BULK) {
                uint8_t ep_num = ep->bEndpointAddress & USB_EP_NUM_MASK;
                uint8_t is_in  = (ep->bEndpointAddress & USB_EP_DIR_IN) ? 1 : 0;

                if (is_in && !found_bulk_in) {
                    dev->bulk_in_addr    = ep->bEndpointAddress;
                    dev->bulk_in_ep      = ep_num;
                    dev->bulk_in_max_pkt = ep->wMaxPacketSize;
                    found_bulk_in = 1;
                } else if (!is_in && !found_bulk_out) {
                    dev->bulk_out_addr    = ep->bEndpointAddress;
                    dev->bulk_out_ep      = ep_num;
                    dev->bulk_out_max_pkt = ep->wMaxPacketSize;
                    found_bulk_out = 1;
                }
            }
            /* Ignore interrupt endpoints (xfer_type != BULK) -- per BOT spec */
        }

        offset += desc_len;
    }

    /* Verify we found the MSC BOT triple with both required endpoints */
    if (!found_msc || !found_bulk_in || !found_bulk_out) {
        klog(LOG_DEBUG, "usb",
             "Slot %u: no MSC BOT interface found (msc=%d, in=%d, out=%d)",
             (uint64_t)dev->slot_id, (uint64_t)found_msc,
             (uint64_t)found_bulk_in, (uint64_t)found_bulk_out);
        return -1;
    }

    klog(LOG_INFO, "usb-msc",
         "BOT interface %u: Bulk-IN EP%u (pkt=%u), Bulk-OUT EP%u (pkt=%u)",
         (uint64_t)dev->msc_iface,
         (uint64_t)dev->bulk_in_ep, (uint64_t)dev->bulk_in_max_pkt,
         (uint64_t)dev->bulk_out_ep, (uint64_t)dev->bulk_out_max_pkt);

    /* ---- Allocate Transfer Rings for Bulk endpoints ---- */
    if (ep0_ring_init(&dev->bulk_in_ring) != 0) {
        klog(LOG_ERROR, "usb-msc", "Failed to allocate Bulk-IN Transfer Ring");
        return -1;
    }
    if (ep0_ring_init(&dev->bulk_out_ring) != 0) {
        klog(LOG_ERROR, "usb-msc", "Failed to allocate Bulk-OUT Transfer Ring");
        ep0_ring_free(&dev->bulk_in_ring);   /* don't leak the IN ring */
        return -1;
    }

    /* ---- Rebuild Input Context with Bulk endpoints ---- */
    {
        uint32_t csz = ctx_size(hc);
        uint8_t *input_ctx = (uint8_t *)dev->input_ctx_phys;
        struct xhci_input_ctrl_ctx *ctrl;
        struct xhci_slot_ctx *slot_ctx;
        struct xhci_ep_ctx *ep_in_ctx;
        struct xhci_ep_ctx *ep_out_ctx;
        uint32_t dci_in  = XHCI_DCI(dev->bulk_in_ep, 1);   /* Bulk-IN DCI */
        uint32_t dci_out = XHCI_DCI(dev->bulk_out_ep, 0);   /* Bulk-OUT DCI */
        uint32_t max_dci = (dci_in > dci_out) ? dci_in : dci_out;
        struct xhci_trb cmd;
        uint8_t cc;

        /* Zero the Input Context first */
        dev_zero(input_ctx, 33 * csz);

        /* Input Control Context: add Slot + Bulk-IN + Bulk-OUT */
        ctrl = (struct xhci_input_ctrl_ctx *)input_ctx;
        ctrl->add_flags = XHCI_INPUT_ADD_SLOT
                        | (1u << dci_in)
                        | (1u << dci_out);
        ctrl->drop_flags = 0;

        /* Slot Context: update Context Entries to include highest DCI */
        slot_ctx = (struct xhci_slot_ctx *)(input_ctx + 1 * csz);
        slot_ctx->field0 = XHCI_SCTX_ROUTE(0)
                         | XHCI_SCTX_SPEED(dev->speed)
                         | XHCI_SCTX_ENTRIES(max_dci);
        slot_ctx->field1 = XHCI_SCTX_ROOT_PORT(dev->port);

        /* Bulk-IN Endpoint Context */
        ep_in_ctx = (struct xhci_ep_ctx *)(input_ctx + (dci_in + 1) * csz);
        ep_in_ctx->field0 = 0;
        ep_in_ctx->field1 = XHCI_EPCTX_CERR(3)
                          | XHCI_EPCTX_TYPE(XHCI_EP_TYPE_BULK_IN)
                          | XHCI_EPCTX_MAXPKT(dev->bulk_in_max_pkt);
        ep_in_ctx->tr_dequeue = dev->bulk_in_ring.phys | 1; /* DCS = 1 */
        ep_in_ctx->field4 = XHCI_EPCTX_AVG_TRB_LEN(1024);

        /* Bulk-OUT Endpoint Context */
        ep_out_ctx = (struct xhci_ep_ctx *)(input_ctx + (dci_out + 1) * csz);
        ep_out_ctx->field0 = 0;
        ep_out_ctx->field1 = XHCI_EPCTX_CERR(3)
                           | XHCI_EPCTX_TYPE(XHCI_EP_TYPE_BULK_OUT)
                           | XHCI_EPCTX_MAXPKT(dev->bulk_out_max_pkt);
        ep_out_ctx->tr_dequeue = dev->bulk_out_ring.phys | 1; /* DCS = 1 */
        ep_out_ctx->field4 = XHCI_EPCTX_AVG_TRB_LEN(1024);

        /* Submit Configure Endpoint Command (TRB type 12) */
        dev_zero(&cmd, sizeof(cmd));
        cmd.parameter = dev->input_ctx_phys;
        cmd.control   = (XHCI_TRB_CONFIG_EP << XHCI_TRB_TYPE_SHIFT)
                      | ((uint32_t)dev->slot_id << XHCI_TRB_SLOT_SHIFT);

        if (xhci_cmd_submit(hc, &cmd) != 0) {
            klog(LOG_ERROR, "usb-msc",
                 "Failed to submit Configure Endpoint command");
            /* Command never reached hardware -- both rings are safe to free. */
            ep0_ring_free(&dev->bulk_in_ring);
            ep0_ring_free(&dev->bulk_out_ring);
            return -1;
        }

        cc = xhci_wait_command(hc, NULL);
        if (cc != XHCI_TRB_CC_SUCCESS) {
            klog(LOG_ERROR, "usb-msc",
                 "Configure Endpoint failed (slot %u, cc=%u)",
                 (uint64_t)dev->slot_id, (uint64_t)cc);
            /* Definite error -> reclaim; timeout (0xFF) may still publish the
             * TR dequeue pointers, so quarantine then (same rule as HID). */
            if (cc != 0xFF) {
                ep0_ring_free(&dev->bulk_in_ring);
                ep0_ring_free(&dev->bulk_out_ring);
            }
            return -1;
        }
    }

    dev->is_msc = 1;
    klog(LOG_INFO, "usb-msc",
         "BOT device ready: Bulk-IN EP%u, Bulk-OUT EP%u, MaxPkt=%u",
         (uint64_t)dev->bulk_in_ep, (uint64_t)dev->bulk_out_ep,
         (uint64_t)dev->bulk_in_max_pkt);

    return 0;
}

/* ---- HID Interrupt Transfer Polling ----------------------------------- */

/* Queue a Normal TRB on the HID Interrupt-IN ring pointing at the device's
 * report DMA buffer, with the Interrupter Target field set to 1 so its
 * completion lands on the dedicated HID event ring, then ring the endpoint
 * doorbell. Returns 0 on success. */
static int xhci_hid_queue_report(struct xhci_controller *hc,
                                 struct xhci_device *dev)
{
    struct xhci_trb trb;
    uint32_t dci = XHCI_DCI(dev->int_in_ep, 1);

    if (dev->int_in_report_phys == 0)
        return -1;

    dev_zero(&trb, sizeof(trb));
    trb.parameter = dev->int_in_report_phys;
    /* status: TRB Transfer Length (16:0) | Interrupter Target (31:22) = IR1. */
    trb.status    = (uint32_t)dev->int_in_max_pkt | (1u << 22);
    trb.control   = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;

    if (ep0_ring_enqueue(&dev->int_in_ring, &trb) != 0)
        return -1;
    dev_write32(hc->db_base, dev->slot_id * 4, dci);
    return 0;
}

/* Find an active device by owning controller + slot id. Slot IDs are scoped
 * per controller, so both must match or a multi-controller system could
 * resolve an event to the wrong device's transfer ring / report buffer. */
static struct xhci_device *hid_dev_by_slot(struct xhci_controller *hc, uint8_t slot)
{
    int i;
    for (i = 0; i < XHCI_MAX_DEVICES; i++)
        if (devices[i].active && devices[i].owner == hc &&
            devices[i].slot_id == slot)
            return &devices[i];
    return (struct xhci_device *)0;
}

/* Tick-mux callback (~10 ms): drain the dedicated HID event ring on every
 * controller, log each report, and re-queue a TRB for continuous polling.
 * Report PARSING into keystrokes/movement is the keyboard/mouse sections. */
void xhci_hid_poll(void)
{
    int ci, n = xhci_controller_count();

    for (ci = 0; ci < n; ci++) {
        struct xhci_controller *hc = xhci_get_controller_mut(ci);
        struct xhci_trb evt;
        if (!hc || !hc->active || !hc->hid_intr_active)
            continue;
        while (xhci_hid_event_poll(hc, &evt)) {
            uint32_t type = (evt.control & XHCI_TRB_TYPE_MASK)
                          >> XHCI_TRB_TYPE_SHIFT;
            uint8_t  slot = (uint8_t)((evt.control >> XHCI_TRB_SLOT_SHIFT) & 0xFF);
            uint8_t  cc;
            struct xhci_device *dev;

            if (type != XHCI_TRB_TRANSFER_EVT)
                continue;
            dev = hid_dev_by_slot(hc, slot);
            if (!dev || !dev->is_hid || !dev->int_in_report || !dev->int_in_polling)
                continue;

            /* Only a successful (or short-packet) completion carries a valid
             * report. On a halt / device-removed / error completion, stop
             * polling this device instead of re-queueing forever to a dead
             * endpoint -- proper slot teardown is the USB hot-plug lifecycle
             * work in the usb-stack driver TODO. */
            cc = (uint8_t)((evt.status >> XHCI_TRB_CC_SHIFT) & 0xFF);
            if (cc != XHCI_TRB_CC_SUCCESS && cc != XHCI_TRB_CC_SHORT_PKT) {
                klog(LOG_WARN, "usb-hid",
                     "slot %u report transfer cc=%u -- stopping poll",
                     (uint64_t)slot, (uint64_t)cc);
                dev->int_in_polling = 0;
                continue;
            }

            klog(LOG_DEBUG, "usb-hid",
                 "report slot %u: %02x %02x %02x %02x %02x %02x %02x %02x",
                 (uint64_t)slot,
                 dev->int_in_report[0], dev->int_in_report[1],
                 dev->int_in_report[2], dev->int_in_report[3],
                 dev->int_in_report[4], dev->int_in_report[5],
                 dev->int_in_report[6], dev->int_in_report[7]);
            /* Re-queue for the next report; if enqueue fails, stop polling. */
            if (xhci_hid_queue_report(hc, dev) != 0)
                dev->int_in_polling = 0;
        }
    }
}

/* Register the HID report poller on the multi-subscriber tick (once). The tick
 * runs at ~100 Hz, so every_n_ticks = 1 gives ~10 ms polling. */
static void xhci_hid_start_polling(void)
{
    static uint8_t registered;
    if (registered)
        return;
    if (timer_add_tick_subscriber(xhci_hid_poll, 1) == 0)
        registered = 1;
}

/* ---- HID Identification & Interrupt Endpoint Configuration ------------- */

/* Convert an endpoint descriptor bInterval to the xHCI EP-context Interval
 * field. The Interval field expresses the polling period as 125us * 2^Interval
 * (xHCI 6.2.3.6 Table 6-12), but bInterval itself is encoded differently by
 * device speed:
 *   - High/SuperSpeed interrupt: bInterval is a microframe exponent already,
 *     so the period is 2^(bInterval-1) microframes -> Interval = bInterval-1.
 *   - Full/Low speed interrupt: bInterval is a frame count (1ms = 8 microframes
 *     = 2^3 * 125us) -> Interval = floor(log2(bInterval)) + 3.
 * Exposed (non-static) so the encoding can be unit-tested without hardware. */
uint8_t xhci_hid_interval_encode(uint8_t speed, uint8_t b_interval)
{
    if (b_interval < 1)
        b_interval = 1;

    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER) {
        if (b_interval > 16)
            b_interval = 16;
        return (uint8_t)(b_interval - 1);   /* 0..15 */
    }

    /* Full/Low speed: floor(log2(bInterval)) + 3, clamped to [3,15]. */
    {
        uint8_t log2 = 0;
        uint32_t v = b_interval;
        uint32_t field;
        while (v > 1) { v >>= 1; log2++; }
        field = (uint32_t)log2 + 3;
        if (field > 15)
            field = 15;
        return (uint8_t)field;
    }
}

int xhci_hid_identify(struct xhci_controller *hc, struct xhci_device *dev)
{
    const uint8_t *buf = dev->config_data;
    uint16_t len = dev->config_len;
    uint16_t offset = 0;
    int found_hid = 0;
    int found_int_in = 0;
    int in_hid_iface = 0;
    uint8_t hid_proto = 0;

    dev->is_hid = 0;

    /* Walk the config descriptor tree linearly (bLength/bDescriptorType) */
    while (offset + 2 <= len) {
        uint8_t desc_len  = buf[offset];
        uint8_t desc_type = buf[offset + 1];

        if (desc_len < 2)
            break;
        if (offset + desc_len > len)
            break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= 9) {
            const struct usb_interface_descriptor *iface =
                (const struct usb_interface_descriptor *)(buf + offset);

            if (iface->bInterfaceClass    == USB_CLASS_HID &&
                iface->bInterfaceSubClass == USB_SUBCLASS_HID_BOOT &&
                (iface->bInterfaceProtocol == USB_PROTO_HID_KEYBOARD ||
                 iface->bInterfaceProtocol == USB_PROTO_HID_MOUSE)) {
                found_hid     = 1;
                in_hid_iface  = 1;
                hid_proto     = iface->bInterfaceProtocol;
                dev->hid_iface = iface->bInterfaceNumber;
                dev->hid_proto = hid_proto;
                found_int_in  = 0;   /* search this interface's endpoints */
            } else {
                /* A different interface descriptor ends the HID EP list */
                if (found_hid && found_int_in)
                    break;
                in_hid_iface = 0;
            }
        } else if (desc_type == USB_DESC_ENDPOINT && desc_len >= 7 && in_hid_iface) {
            const struct usb_endpoint_descriptor *ep =
                (const struct usb_endpoint_descriptor *)(buf + offset);
            uint8_t xfer_type = ep->bmAttributes & USB_EP_ATTR_TYPE_MASK;
            uint8_t is_in     = (ep->bEndpointAddress & USB_EP_DIR_IN) ? 1 : 0;

            if (xfer_type == USB_EP_ATTR_INTERRUPT && is_in && !found_int_in) {
                /* Validate untrusted descriptor fields before trusting them:
                 * EP number 0 is the control endpoint (DCI 1) -- a HID
                 * descriptor claiming it would overwrite EP0's context. The
                 * wMaxPacketSize byte count is bits 10:0 (bits 12:11 are the
                 * HS high-bandwidth transaction count, not used for boot HID);
                 * boot reports are <= 8 bytes, so cap well below that range.
                 * A bogus endpoint is skipped, not stored -- keep scanning. */
                uint8_t  ep_num  = ep->bEndpointAddress & USB_EP_NUM_MASK;
                uint16_t wmps    = ep->wMaxPacketSize;
                uint16_t max_pkt = wmps & 0x07FF;
                /* Reject high-bandwidth interrupt endpoints (bits 12:11 set):
                 * boot HID is single-transaction and we do not program the
                 * Mult / Max-ESIT-payload fields those bits imply. */
                if (ep_num != 0 && (wmps & 0x1800) == 0 &&
                    max_pkt != 0 && max_pkt <= 64) {
                    dev->int_in_addr     = ep->bEndpointAddress;
                    dev->int_in_ep       = ep_num;
                    dev->int_in_max_pkt  = max_pkt;
                    dev->int_in_interval =
                        xhci_hid_interval_encode(dev->speed, ep->bInterval);
                    found_int_in = 1;
                }
            }
        }

        offset += desc_len;
    }

    if (!found_hid || !found_int_in) {
        klog(LOG_DEBUG, "usb",
             "Slot %u: no HID boot interface (hid=%d, int_in=%d)",
             (uint64_t)dev->slot_id, (uint64_t)found_hid, (uint64_t)found_int_in);
        return -1;
    }

    /* ---- Allocate Transfer Ring for the Interrupt-IN endpoint ---- */
    if (ep0_ring_init(&dev->int_in_ring) != 0) {
        klog(LOG_ERROR, "usb-hid",
             "Failed to allocate Interrupt-IN Transfer Ring");
        return -1;
    }

    /* ---- Build Input Context with the Interrupt-IN endpoint ----
     * Boot keyboards/mice are HID-only (no MSC), so the single Interrupt-IN
     * endpoint plus the Slot context is the whole configuration. */
    {
        uint32_t csz = ctx_size(hc);
        uint8_t *input_ctx = (uint8_t *)dev->input_ctx_phys;
        struct xhci_input_ctrl_ctx *ctrl;
        struct xhci_slot_ctx *slot_ctx;
        struct xhci_ep_ctx *ep_in_ctx;
        uint32_t dci_in = XHCI_DCI(dev->int_in_ep, 1);   /* Interrupt-IN DCI */
        struct xhci_trb cmd;
        uint8_t cc;

        dev_zero(input_ctx, 33 * csz);

        /* Input Control Context: add Slot + Interrupt-IN */
        ctrl = (struct xhci_input_ctrl_ctx *)input_ctx;
        ctrl->add_flags  = XHCI_INPUT_ADD_SLOT | (1u << dci_in);
        ctrl->drop_flags = 0;

        /* Slot Context: Context Entries up to the Interrupt-IN DCI */
        slot_ctx = (struct xhci_slot_ctx *)(input_ctx + 1 * csz);
        slot_ctx->field0 = XHCI_SCTX_ROUTE(0)
                         | XHCI_SCTX_SPEED(dev->speed)
                         | XHCI_SCTX_ENTRIES(dci_in);
        slot_ctx->field1 = XHCI_SCTX_ROOT_PORT(dev->port);

        /* Interrupt-IN Endpoint Context */
        ep_in_ctx = (struct xhci_ep_ctx *)(input_ctx + (dci_in + 1) * csz);
        ep_in_ctx->field0 = XHCI_EPCTX_INTERVAL(dev->int_in_interval);
        ep_in_ctx->field1 = XHCI_EPCTX_CERR(3)
                          | XHCI_EPCTX_TYPE(XHCI_EP_TYPE_INTERRUPT_IN)
                          | XHCI_EPCTX_MAXPKT(dev->int_in_max_pkt);
        ep_in_ctx->tr_dequeue = dev->int_in_ring.phys | 1; /* DCS = 1 */
        /* Interrupt endpoints are periodic: program the Max ESIT Payload so
         * strict (bare-metal) controllers schedule a service budget. Boot HID
         * is single-transaction, so the per-interval payload == wMaxPacketSize. */
        ep_in_ctx->field4 = XHCI_EPCTX_AVG_TRB_LEN(dev->int_in_max_pkt)
                          | XHCI_EPCTX_MAX_ESIT_LO(dev->int_in_max_pkt);

        /* Submit Configure Endpoint Command */
        dev_zero(&cmd, sizeof(cmd));
        cmd.parameter = dev->input_ctx_phys;
        cmd.control   = (XHCI_TRB_CONFIG_EP << XHCI_TRB_TYPE_SHIFT)
                      | ((uint32_t)dev->slot_id << XHCI_TRB_SLOT_SHIFT);

        if (xhci_cmd_submit(hc, &cmd) != 0) {
            klog(LOG_ERROR, "usb-hid",
                 "Failed to submit Configure Endpoint command");
            ep0_ring_free(&dev->int_in_ring);
            return -1;
        }

        cc = xhci_wait_command(hc, NULL);
        if (cc != XHCI_TRB_CC_SUCCESS) {
            klog(LOG_ERROR, "usb-hid",
                 "Configure Endpoint failed (slot %u, cc=%u)",
                 (uint64_t)dev->slot_id, (uint64_t)cc);
            /* On a definite error completion the command was rejected and the
             * EP context was not applied, so the ring is safe to reclaim. On a
             * timeout (0xFF sentinel) the command may still complete later and
             * publish this ring's TR dequeue pointer into hardware -- leave the
             * page quarantined rather than risk a DMA-visible use-after-free. */
            if (cc != 0xFF)
                ep0_ring_free(&dev->int_in_ring);
            return -1;
        }
    }

    dev->is_hid = 1;
    klog(LOG_INFO, "usb-hid",
         "HID %s found on port %u (EP%u, interval field=%u, MaxPkt=%u)",
         hid_proto == USB_PROTO_HID_KEYBOARD ? "keyboard" : "mouse",
         (uint64_t)dev->port, (uint64_t)dev->int_in_ep,
         (uint64_t)dev->int_in_interval, (uint64_t)dev->int_in_max_pkt);

    /* Allocate the report DMA buffer now (only when the dedicated HID
     * interrupter is up). The first TRB queue + poller arm happen later in
     * xhci_enumerate_device, AFTER dev->active is published, so an early tick
     * poll cannot drain a completion for a still-inactive device. */
    if (hc->hid_intr_active) {
        dev->int_in_report_phys = pmm_alloc_contiguous(1);
        if (dev->int_in_report_phys != 0) {
            dev_map_page(dev->int_in_report_phys, 0x1000);
            dev->int_in_report = (uint8_t *)dev->int_in_report_phys;
            dev_zero(dev->int_in_report, 0x1000);
        }
    }

    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

int xhci_enumerate_ports(struct xhci_controller *hc)
{
    uint32_t port;
    uint32_t portsc;
    uint32_t portsc_offset;
    uint8_t speed;
    int count = 0;

    klog(LOG_DEBUG, "usb", "Scanning %u ports for connected devices",
         (uint64_t)hc->max_ports);

    for (port = 1; port <= hc->max_ports; port++) {
        portsc_offset = XHCI_PORTSC_BASE + (port - 1) * XHCI_PORTSC_STRIDE;
        portsc = dev_read32(hc->op_base, portsc_offset);

        /* Check Current Connect Status (CCS) */
        if (!(portsc & XHCI_PORTSC_CCS))
            continue;

        speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;

        g_usb_ccs_count++;
        g_usb_enum_stage = 1;  /* CCS detected */
        klog(LOG_INFO, "usb", "Port %u: device connected, speed=%s",
             (uint64_t)port, speed_to_str(speed));

        /* Clear any pending CSC (Connect Status Change) */
        portsc = dev_read32(hc->op_base, portsc_offset);
        if (portsc & XHCI_PORTSC_CSC) {
            portsc = (portsc & XHCI_PORTSC_PRESERVE_MASK) | XHCI_PORTSC_CSC;
            dev_write32(hc->op_base, portsc_offset, portsc);
        }

        /* Enumerate this device */
        if (xhci_enumerate_device(hc, (uint8_t)port, speed) == 0)
            count++;
    }

    klog(LOG_INFO, "usb", "%u device(s) enumerated", (uint64_t)count);
    return count;
}

/* ---- Device accessors ---- */

struct xhci_device *xhci_get_device(int index)
{
    if (index < 0 || index >= XHCI_MAX_DEVICES)
        return NULL;
    return devices[index].active ? &devices[index] : NULL;
}

int xhci_msc_device_count(void)
{
    int count = 0;
    for (int i = 0; i < XHCI_MAX_DEVICES; i++) {
        if (devices[i].active && devices[i].is_msc)
            count++;
    }
    return count;
}

/* Get the N-th active MSC device (0-based). Returns device index in global array. */
int xhci_msc_device_index(int nth)
{
    int count = 0;
    for (int i = 0; i < XHCI_MAX_DEVICES; i++) {
        if (devices[i].active && devices[i].is_msc) {
            if (count == nth)
                return i;
            count++;
        }
    }
    return -1;
}
