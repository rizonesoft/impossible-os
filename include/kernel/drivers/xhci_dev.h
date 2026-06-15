/* ============================================================================
 * xhci_dev.h -- USB device enumeration for xHCI
 *
 * PORTSC register bits, USB descriptor structures, xHCI device context
 * layout, and enumeration API.
 *
 * Reference: xHCI specification 1.2, §4.3 (USB Device Initialization)
 *            USB 2.0 specification, §9.6 (Standard USB Descriptor Definitions)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/drivers/xhci_ring.h"

/* Forward declarations */
struct xhci_controller;

/* ---- PORTSC register bits (Operational + 0x400 + port×0x10) -------------- */

#define XHCI_PORTSC_CCS     (1 << 0)     /* Current Connect Status (RO) */
#define XHCI_PORTSC_PED     (1 << 1)     /* Port Enabled/Disabled (RW1C) */
#define XHCI_PORTSC_OCA     (1 << 3)     /* Over-Current Active (RO) */
#define XHCI_PORTSC_PR      (1 << 4)     /* Port Reset (RW1S) */
#define XHCI_PORTSC_PP      (1 << 9)     /* Port Power (RW) */
#define XHCI_PORTSC_SPEED_MASK  (0xF << 10) /* Port Speed (RO, bits 13:10) */
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_CSC     (1 << 17)    /* Connect Status Change (RW1C) */
#define XHCI_PORTSC_PEC     (1 << 18)    /* Port Enabled/Disabled Change (RW1C) */
#define XHCI_PORTSC_PRC     (1 << 21)    /* Port Reset Change (RW1C) */

/* Preserve mask: bits that must NOT be accidentally written as 1 (RW1C bits).
 * When writing PORTSC, mask with this to avoid clearing change bits. */
#define XHCI_PORTSC_PRESERVE_MASK  (~(uint32_t)(XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
    XHCI_PORTSC_PRC | XHCI_PORTSC_PED | XHCI_PORTSC_OCA))

/* ---- Intel EHCI->xHCI USB 2.0 port-routing settle (XUSB2PR) -------------- */

/* After the Intel XUSB2PR / USB3_PSSEN routing writes, the routed USB 2.0 ports
 * re-present onto xHCI. The controller-init helper waits this bounded window for
 * the routed ports to settle. We do NOT early-exit on observed connects: the
 * XUSB2PR-write-to-CCS latency is vendor-specific and not bounded by any USB/xHCI
 * spec interval, and boot-time MSC enumeration is synchronous, so a timed/quiesced
 * early-exit could enumerate before a routed boot drive asserts CCS. The boot-time
 * win is the EHCI-presence gate (modern Intel skips routing + this wait entirely),
 * not a shortened wait. */
#define XHCI_XUSB2PR_ROUTE_MAX_US  500000u  /* bounded settle: 500ms (proven-safe value) */

/* ---- USB speed constants (xHCI PORTSC encoding) -------------------------- */

#define USB_SPEED_FULL      1   /* Full Speed (12 Mbps)    -- max pkt  64 */
#define USB_SPEED_LOW       2   /* Low Speed (1.5 Mbps)    -- max pkt   8 */
#define USB_SPEED_HIGH      3   /* High Speed (480 Mbps)   -- max pkt  64 */
#define USB_SPEED_SUPER     4   /* SuperSpeed (5 Gbps)     -- max pkt 512 */

/* ---- USB Descriptor types ------------------------------------------------ */

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

/* ---- USB standard request codes ------------------------------------------ */

#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09

/* ---- USB Mass Storage Class (MSC) constants ------------------------------ */

#define USB_CLASS_MASS_STORAGE  0x08
#define USB_SUBCLASS_SCSI       0x06  /* SCSI Transparent Command Set */
#define USB_PROTO_BOT           0x50  /* Bulk-Only Transport */

/* ---- USB Human Interface Device (HID) boot-protocol constants ------------ */

#define USB_CLASS_HID           0x03
#define USB_SUBCLASS_HID_BOOT   0x01  /* Boot interface subclass */
#define USB_PROTO_HID_KEYBOARD  0x01  /* Boot-protocol keyboard */
#define USB_PROTO_HID_MOUSE     0x02  /* Boot-protocol mouse */

/* Endpoint address / attributes helpers */
#define USB_EP_DIR_IN           0x80  /* bEndpointAddress bit 7 = IN */
#define USB_EP_NUM_MASK         0x0F  /* bEndpointAddress bits 3:0 = EP number */
#define USB_EP_ATTR_BULK        0x02  /* bmAttributes bits 1:0 = Bulk */
#define USB_EP_ATTR_INTERRUPT   0x03  /* bmAttributes bits 1:0 = Interrupt */
#define USB_EP_ATTR_TYPE_MASK   0x03  /* bmAttributes transfer type mask */

/* xHCI DCI (Device Context Index) for a given endpoint:
 *   DCI = ep_num * 2 + direction  (direction: 0=OUT, 1=IN)
 *   EP0 OUT = DCI 1, EP1 OUT = DCI 2, EP1 IN = DCI 3, etc. */
#define XHCI_DCI(ep_num, dir_in)  ((uint32_t)(ep_num) * 2 + ((dir_in) ? 1 : 0))

/* ---- USB Descriptor structures ------------------------------------------- */

struct usb_device_descriptor {
    uint8_t  bLength;               /* 18 */
    uint8_t  bDescriptorType;       /* USB_DESC_DEVICE = 0x01 */
    uint16_t bcdUSB;                /* USB version (e.g. 0x0200 = USB 2.0) */
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;       /* Max packet size for EP0 (8, 16, 32, 64) */
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;               /* 9 */
    uint8_t  bDescriptorType;       /* USB_DESC_CONFIGURATION = 0x02 */
    uint16_t wTotalLength;          /* Total length of config + sub-descriptors */
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;   /* Value to use in SET_CONFIGURATION */
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;               /* 9 */
    uint8_t  bDescriptorType;       /* USB_DESC_INTERFACE = 0x04 */
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;               /* 7 */
    uint8_t  bDescriptorType;       /* USB_DESC_ENDPOINT = 0x05 */
    uint8_t  bEndpointAddress;      /* bit 7 = direction (1=IN, 0=OUT) */
    uint8_t  bmAttributes;          /* bits 1:0 = transfer type */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* ---- xHCI Slot Context (§6.2.2) ----------------------------------------- */

/* Slot Context is 32 bytes (CSZ=0) or 64 bytes (CSZ=1, padded).
 * We define the 32-byte core and add padding where needed. */

struct xhci_slot_ctx {
    uint32_t field0;    /* Route String (19:0), Speed (23:20), MTT (25),
                           Hub (26), Context Entries (31:27) */
    uint32_t field1;    /* Max Exit Latency (15:0), Root Hub Port Number (23:16),
                           Number of Ports (31:24) */
    uint32_t field2;    /* TT Hub Slot ID, TT Port Number, TTT, Interrupter Target */
    uint32_t field3;    /* USB Device Address (7:0), Slot State (31:27) */
    uint32_t reserved[4];
} __attribute__((packed));

/* Slot Context field0 helpers */
#define XHCI_SCTX_SPEED(s)         (((uint32_t)(s) & 0xF) << 20)
#define XHCI_SCTX_ENTRIES(n)       (((uint32_t)(n) & 0x1F) << 27)
#define XHCI_SCTX_ROUTE(r)        ((uint32_t)(r) & 0xFFFFF)

/* Slot Context field1 helpers */
#define XHCI_SCTX_ROOT_PORT(p)    (((uint32_t)(p) & 0xFF) << 16)

/* ---- xHCI Endpoint Context (§6.2.3) ------------------------------------- */

struct xhci_ep_ctx {
    uint32_t field0;    /* EP State (2:0), Mult (9:8), MaxPStreams (14:10),
                           Interval (23:16), MaxESITPayloadHi (31:24) */
    uint32_t field1;    /* CErr (2:1), EP Type (5:3), MaxBurstSize (15:8),
                           MaxPacketSize (31:16) */
    uint64_t tr_dequeue; /* Transfer Ring Dequeue Pointer (63:4) | DCS (0) */
    uint32_t field4;    /* Average TRB Length (15:0), Max ESIT Payload Lo (31:16) */
    uint32_t reserved[3];
} __attribute__((packed));

/* Endpoint types (field1 bits 5:3) */
#define XHCI_EP_TYPE_CONTROL_BI  4   /* Control (bidirectional) */
#define XHCI_EP_TYPE_BULK_OUT    2   /* Bulk OUT */
#define XHCI_EP_TYPE_BULK_IN     6   /* Bulk IN */
#define XHCI_EP_TYPE_INTERRUPT_IN 7  /* Interrupt IN (HID) */

/* Endpoint Context EP State (field0 bits 2:0, xHCI 6.2.3.1) */
#define XHCI_EP_STATE_MASK       0x7
#define XHCI_EP_STATE_DISABLED   0
#define XHCI_EP_STATE_RUNNING    1
#define XHCI_EP_STATE_HALTED     2   /* stalled -- needs reset-endpoint recovery */
#define XHCI_EP_STATE_STOPPED    3

/* Endpoint Context field1 helpers */
#define XHCI_EPCTX_CERR(n)        (((uint32_t)(n) & 0x3) << 1)
#define XHCI_EPCTX_TYPE(t)        (((uint32_t)(t) & 0x7) << 3)
#define XHCI_EPCTX_MAXPKT(s)      (((uint32_t)(s) & 0xFFFF) << 16)
#define XHCI_EPCTX_MAXBURST(b)    (((uint32_t)(b) & 0xFF) << 8)

/* Endpoint Context field0 helper: Interval (bits 23:16) -- the polling period
 * as 125us * 2^Interval (xHCI 6.2.3.6). */
#define XHCI_EPCTX_INTERVAL(i)    (((uint32_t)(i) & 0xFF) << 16)

/* Endpoint Context field4 helpers */
#define XHCI_EPCTX_AVG_TRB_LEN(l)  ((uint32_t)(l) & 0xFFFF)
/* Max ESIT Payload Lo (field4 bits 31:16) -- per-service-interval byte budget
 * for periodic (interrupt/isoch) endpoints. For single-transaction boot HID
 * this equals wMaxPacketSize (<= 64), so the Hi field stays zero. */
#define XHCI_EPCTX_MAX_ESIT_LO(p)  (((uint32_t)(p) & 0xFFFF) << 16)

/* ---- xHCI Input Control Context (§6.2.5) -------------------------------- */

struct xhci_input_ctrl_ctx {
    uint32_t drop_flags;    /* Drop Context flags (D0-D31) */
    uint32_t add_flags;     /* Add Context flags (A0-A31) */
    uint32_t reserved[6];
} __attribute__((packed));

/* Input Control Context add flags:
 *   Bit 0 = Slot Context (A0)
 *   Bit 1 = EP0 (A1)
 *   Bit 2 = EP1 OUT (A2), Bit 3 = EP1 IN (A3), etc. */
#define XHCI_INPUT_ADD_SLOT     (1 << 0)
#define XHCI_INPUT_ADD_EP0      (1 << 1)

/* ---- Device tracking ----------------------------------------------------- */

#define XHCI_MAX_DEVICES    64  /* Max simultaneously enumerated devices */
#define XHCI_CONFIG_BUF_MAX 4096 /* Max config descriptor wTotalLength */

struct xhci_device {
    uint8_t   active;            /* 1 if slot is in use */
    uint8_t   slot_id;           /* xHCI slot identifier (1-based) */
    uint8_t   port;              /* Root hub port number (1-based) */
    uint8_t   speed;             /* USB_SPEED_* constant */

    uint16_t  vendor_id;
    uint16_t  product_id;
    uint16_t  bcd_usb;
    uint8_t   num_configurations;
    uint8_t   max_packet_size;   /* EP0 max packet size */

    /* Config descriptor (full, including sub-descriptors) */
    uint8_t  *config_buf;        /* Points into config_data[] */
    uint16_t  config_len;        /* Actual wTotalLength */
    uint8_t   config_value;      /* bConfigurationValue for SET_CONFIGURATION */

    /* DMA structures */
    uint64_t  output_ctx_phys;   /* Output Device Context physical address */
    uint64_t  input_ctx_phys;    /* Input Context physical address */
    struct xhci_ring ep0_ring;   /* EP0 (Default Control Pipe) Transfer Ring */

    /* MSC BOT fields (populated by xhci_msc_identify) */
    uint8_t   is_msc;            /* 1 if MSC BOT interface was found */
    uint8_t   msc_iface;         /* bInterfaceNumber of the MSC interface */
    uint8_t   bulk_in_addr;      /* Bulk-IN endpoint address (with dir bit) */
    uint8_t   bulk_out_addr;     /* Bulk-OUT endpoint address */
    uint8_t   bulk_in_ep;        /* Bulk-IN endpoint number (0-15) */
    uint8_t   bulk_out_ep;       /* Bulk-OUT endpoint number (0-15) */
    uint16_t  bulk_in_max_pkt;   /* Bulk-IN wMaxPacketSize */
    uint16_t  bulk_out_max_pkt;  /* Bulk-OUT wMaxPacketSize */
    struct xhci_ring bulk_in_ring;   /* Bulk-IN Transfer Ring */
    struct xhci_ring bulk_out_ring;  /* Bulk-OUT Transfer Ring */

    /* HID boot-protocol fields (populated by xhci_hid_identify) */
    uint8_t   is_hid;            /* 1 if a HID boot interface was found */
    uint8_t   hid_iface;         /* bInterfaceNumber of the HID interface */
    uint8_t   hid_proto;         /* USB_PROTO_HID_KEYBOARD or _MOUSE */
    uint8_t   int_in_addr;       /* Interrupt-IN endpoint address (with dir bit) */
    uint8_t   int_in_ep;         /* Interrupt-IN endpoint number (0-15) */
    uint8_t   int_in_interval;   /* EP-context Interval field (125us * 2^N) */
    uint16_t  int_in_max_pkt;    /* Interrupt-IN wMaxPacketSize */
    struct xhci_ring int_in_ring;    /* Interrupt-IN Transfer Ring */
    uint64_t  int_in_report_phys;    /* DMA buffer the controller writes reports to */
    uint8_t  *int_in_report;         /* virt alias of int_in_report_phys */
    uint8_t   int_in_polling;        /* 1 once a report TRB is queued + poller live */
    uint8_t   hid_prev_report[8];    /* last keyboard boot report (new-key diff) */
    struct xhci_controller *owner;   /* owning controller (slot IDs are per-controller) */

    /* Config descriptor inline storage (avoids separate allocation) */
    uint8_t   config_data[XHCI_CONFIG_BUF_MAX];
};

/* ---- API ----------------------------------------------------------------- */

/* Scan all ports on the controller and enumerate connected devices.
 * Called after controller initialization.
 * Returns number of devices successfully enumerated. */
int xhci_enumerate_ports(struct xhci_controller *hc);

/* Enumerate a single device on the given port.
 * Used by hot-plug ISR for newly connected devices.
 * Returns 0 on success, -1 on failure. */
int xhci_enumerate_device(struct xhci_controller *hc, uint8_t port, uint8_t speed);

/* Walk config descriptor to find MSC BOT interface, extract Bulk-IN/OUT
 * endpoints, allocate Transfer Rings, and issue Configure Endpoint command.
 * Returns 0 if MSC device identified and configured, -1 otherwise. */
int xhci_msc_identify(struct xhci_controller *hc, struct xhci_device *dev);

/* Walk config descriptor to find a HID boot-protocol keyboard/mouse interface,
 * extract the Interrupt-IN endpoint, allocate its Transfer Ring, and issue a
 * Configure Endpoint command. Returns 0 if a HID device was identified and
 * configured, -1 otherwise. */
int xhci_hid_identify(struct xhci_controller *hc, struct xhci_device *dev);

/* Drain the dedicated HID event ring(s) and log/re-queue reports. Registered on
 * the timer tick multiplexer by HID enumeration; not for direct external use. */
void xhci_hid_poll(void);

/* Encode an endpoint descriptor bInterval into the xHCI EP-context Interval
 * field (period = 125us * 2^Interval). Speed-dependent (xHCI 6.2.3.6):
 * HS/SS -> bInterval-1; FS/LS -> floor(log2(bInterval))+3. Exposed for tests. */
uint8_t xhci_hid_interval_encode(uint8_t speed, uint8_t b_interval);

/* Decode a boot-protocol mouse report [buttons, dx, dy] into screen-oriented
 * relative deltas (signed) and the three boot buttons (MOUSE_BTN_* bit layout).
 * Pure; exposed for tests. The optional wheel byte and trailing bytes are
 * ignored (boot protocol is movement + buttons only). */
void xhci_hid_decode_mouse(const uint8_t *report, int32_t *dx, int32_t *dy,
                           uint8_t *buttons);

/* Perform a bulk transfer on a non-EP0 endpoint.
 * dir_in: 1 = bulk IN, 0 = bulk OUT.
 * Returns 0 on success, -1 on failure. */
int xhci_bulk_transfer(struct xhci_controller *hc, struct xhci_device *dev,
                       struct xhci_ring *ring, void *buf, uint32_t len,
                       int dir_in);

/* Software bulk-transfer timeout (xHCI has no hardware transfer timeout). */
#define USB_BULK_TIMEOUT_MS   5000u

/* Like xhci_bulk_transfer but returns the raw xHCI Transfer Event completion
 * code (XHCI_TRB_CC_*) instead of 0/-1, so the BOT transport layer can tell a
 * recoverable endpoint halt (STALL/BABBLE/USB_TXN/DATA_BUFFER) from success.
 * Returns XHCI_TRB_CC_SHORT_PKT and STOPS on the first short packet (a further
 * IN TD would DMA the CSW into the buffer); 0xFF on a timeout (after aborting
 * the stuck endpoint). `*xfer_out` (when non-NULL) receives the host-observed
 * bytes transferred (requested - residual), so a caller can fail an
 * exact-length command on a host short independent of the CSW residue. Does
 * NOT perform stall recovery itself. */
uint8_t xhci_bulk_transfer_cc(struct xhci_controller *hc, struct xhci_device *dev,
                              struct xhci_ring *ring, void *buf, uint32_t len,
                              int dir_in, uint32_t *xfer_out);

/* Low-level control (EP0) transfer that returns 0 on success, -1 on failure and
 * performs NO endpoint recovery -- the no-recovery primitive used by stall
 * recovery (CLEAR_FEATURE) and the BOT mass-storage reset so recovery can never
 * recurse through EP0. */
int xhci_send_control(struct xhci_controller *hc, struct xhci_device *dev,
                      const uint8_t setup[8], void *data, uint32_t data_len,
                      int dir_in);

/* Return 1 if the endpoint's output context EP State is Halted, else 0. */
int xhci_endpoint_is_halted(struct xhci_controller *hc, struct xhci_device *dev,
                            uint32_t dci);

/* Send CLEAR_FEATURE(ENDPOINT_HALT) to `ep_addr` (with direction bit). Returns
 * 0 on success, -1 on failure. No-recovery (uses xhci_send_control). */
int xhci_clear_endpoint_halt(struct xhci_controller *hc, struct xhci_device *dev,
                             uint8_t ep_addr);

/* Recover a halted endpoint: CLEAR_FEATURE(ENDPOINT_HALT) -> Reset Endpoint
 * command -> Set TR Dequeue Pointer (past the failed TRB) -> ready to re-ring.
 * `dci` is the Device Context Index, `ep_addr` the USB endpoint address (with
 * direction bit), `ring` the endpoint's transfer ring. Returns 0 on success. */
int xhci_recover_endpoint(struct xhci_controller *hc, struct xhci_device *dev,
                          uint32_t dci, uint8_t ep_addr, struct xhci_ring *ring);

/* Get a device by global array index. Returns NULL if inactive. */
struct xhci_device *xhci_get_device(int index);

/* Count of active MSC devices across all controllers. */
int xhci_msc_device_count(void);

/* Get the global array index of the N-th MSC device (0-based). Returns -1 if not found. */
int xhci_msc_device_index(int nth);

/* Debug: USB enumeration progress (0-8) and CCS port count */
int xhci_get_enum_stage(void);
int xhci_get_ccs_count(void);
