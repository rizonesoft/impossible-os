/* ============================================================================
 * xhci.h — xHCI (USB 3.x) Host Controller driver
 *
 * Extensible Host Controller Interface for USB 3.x/2.0/1.1 devices.
 * This header defines PCI identification constants, MMIO capability register
 * offsets, and the controller state structure.
 *
 * Reference: xHCI specification 1.2, §5 (Register Interface)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- PCI identification -------------------------------------------------- */
#define XHCI_PCI_CLASS      0x0C    /* Serial Bus Controller */
#define XHCI_PCI_SUBCLASS   0x03    /* USB Controller */
#define XHCI_PCI_PROG_IF    0x30    /* xHCI (USB 3.x) */

/* ---- xHCI Capability Registers (offset from MMIO base) ------------------- */
#define XHCI_CAP_CAPLENGTH  0x00    /* Capability Register Length (1 byte) */
#define XHCI_CAP_HCIVERSION 0x02    /* Interface Version Number (2 bytes) */
#define XHCI_CAP_HCSPARAMS1 0x04    /* Structural Parameters 1 */
#define XHCI_CAP_HCSPARAMS2 0x08    /* Structural Parameters 2 */
#define XHCI_CAP_HCSPARAMS3 0x0C    /* Structural Parameters 3 */
#define XHCI_CAP_HCCPARAMS1 0x10    /* Capability Parameters 1 */
#define XHCI_CAP_DBOFF      0x14    /* Doorbell Offset */
#define XHCI_CAP_RTSOFF     0x18    /* Runtime Register Space Offset */
#define XHCI_CAP_HCCPARAMS2 0x1C    /* Capability Parameters 2 */

/* HCSPARAMS1 field masks */
#define XHCI_HCS1_MAX_SLOTS_MASK   0xFF        /* bits 7:0  */
#define XHCI_HCS1_MAX_INTRS_MASK   0x7FF00     /* bits 18:8 */
#define XHCI_HCS1_MAX_INTRS_SHIFT  8
#define XHCI_HCS1_MAX_PORTS_MASK   0xFF000000  /* bits 31:24 */
#define XHCI_HCS1_MAX_PORTS_SHIFT  24

/* HCCPARAMS1 field masks */
#define XHCI_HCC1_AC64     (1 << 0)    /* 64-bit Addressing Capability */
#define XHCI_HCC1_CSZ      (1 << 2)    /* Context Size (1=64B, 0=32B) */

/* ---- xHCI Operational Registers (offset from MMIO base + CAPLENGTH) ------ */
#define XHCI_OP_USBCMD     0x00    /* USB Command */
#define XHCI_OP_USBSTS     0x04    /* USB Status */
#define XHCI_OP_PAGESIZE    0x08    /* Page Size */
#define XHCI_OP_DNCTRL     0x14    /* Device Notification Control */
#define XHCI_OP_CRCR       0x18    /* Command Ring Control Register (8 bytes) */
#define XHCI_OP_DCBAAP     0x30    /* Device Context Base Address Array Pointer (8 bytes) */
#define XHCI_OP_CONFIG     0x38    /* Configure */

/* USBCMD bits */
#define XHCI_CMD_RUN        (1 << 0)    /* Run/Stop */
#define XHCI_CMD_HCRST      (1 << 1)    /* Host Controller Reset */
#define XHCI_CMD_INTE       (1 << 2)    /* Interrupter Enable */

/* USBSTS bits */
#define XHCI_STS_HCH        (1 << 0)    /* HC Halted */
#define XHCI_STS_CNR        (1 << 11)   /* Controller Not Ready */

/* ---- Controller state ---------------------------------------------------- */

/* Maximum xHCI controllers supported (most systems have 1-2) */
#define XHCI_MAX_CONTROLLERS 4

struct xhci_controller {
    /* PCI location */
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;

    /* MMIO region */
    volatile uint8_t *mmio_base;    /* Virtual address of MMIO region */
    uint64_t          mmio_phys;    /* Physical address of MMIO region */
    uint32_t          mmio_size;    /* Size of mapped MMIO region */

    /* Capability register cache (read once at init) */
    uint8_t  cap_length;            /* Offset to Operational Registers */
    uint16_t hci_version;           /* xHCI version (e.g. 0x0100 = 1.0) */
    uint32_t max_slots;             /* Maximum Device Slots */
    uint32_t max_intrs;             /* Maximum Interrupters */
    uint32_t max_ports;             /* Maximum Ports */
    uint32_t db_offset;             /* Doorbell Array offset */
    uint32_t rts_offset;            /* Runtime Register Space offset */
    uint8_t  ac64;                  /* 64-bit addressing capable */
    uint8_t  csz;                   /* Context size (0=32B, 1=64B) */

    /* Operational register base (mmio_base + cap_length) */
    volatile uint8_t *op_base;

    uint8_t  active;                /* 1 if initialized successfully */
};

/* ---- API ----------------------------------------------------------------- */

/* Scan PCI for xHCI controllers and initialize them.
 * Returns number of controllers found (0 if none). */
int xhci_init(void);

/* Get a controller by index. Returns NULL if index out of range. */
const struct xhci_controller *xhci_get_controller(int index);

/* Get number of active xHCI controllers. */
int xhci_controller_count(void);
