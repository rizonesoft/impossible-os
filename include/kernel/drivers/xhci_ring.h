/* ============================================================================
 * xhci_ring.h -- xHCI TRB Ring Architecture
 *
 * Transfer Request Block (TRB) structures, Command Ring, Event Ring,
 * Event Ring Segment Table (ERST), and ring management functions.
 *
 * Reference: xHCI specification 1.2, §4.9 (TRB Rings), §4.11 (ERST)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declaration */
struct xhci_controller;

/* ---- TRB structure (16 bytes) -------------------------------------------- */

struct xhci_trb {
    uint64_t parameter;     /* Parameter -- address or inline data */
    uint32_t status;        /* Status -- transfer length, completion code */
    uint32_t control;       /* Control -- TRB type, cycle bit, flags */
} __attribute__((packed));

/* TRB control field layout */
#define XHCI_TRB_CYCLE          (1 << 0)    /* Cycle bit -- ownership toggle */
#define XHCI_TRB_TOGGLE_CYCLE   (1 << 1)    /* Toggle Cycle (Link TRB only) */
#define XHCI_TRB_CHAIN          (1 << 4)    /* Chain bit */
#define XHCI_TRB_IOC            (1 << 5)    /* Interrupt On Completion */
#define XHCI_TRB_IDT            (1 << 6)    /* Immediate Data (Setup Stage) */
#define XHCI_TRB_TYPE_SHIFT     10
#define XHCI_TRB_TYPE_MASK      (0x3F << 10) /* bits 15:10 */
#define XHCI_TRB_DIR_IN         (1 << 16)   /* Data Stage: direction IN */
/* Transfer Type field (Setup Stage TRB control bits 17:16) */
#define XHCI_TRB_TRT_NO_DATA    (0 << 16)   /* No Data Stage */
#define XHCI_TRB_TRT_OUT        (2 << 16)   /* OUT Data Stage */
#define XHCI_TRB_TRT_IN         (3 << 16)   /* IN Data Stage */

/* TRB types */
#define XHCI_TRB_NORMAL         1   /* Normal Transfer */
#define XHCI_TRB_SETUP_STAGE    2   /* Setup Stage */
#define XHCI_TRB_DATA_STAGE     3   /* Data Stage */
#define XHCI_TRB_STATUS_STAGE   4   /* Status Stage */
#define XHCI_TRB_LINK           6   /* Link -- wraps ring back to start */
#define XHCI_TRB_ENABLE_SLOT    9   /* Enable Slot Command */
#define XHCI_TRB_DISABLE_SLOT   10  /* Disable Slot Command */
#define XHCI_TRB_ADDRESS_DEV    11  /* Address Device Command */
#define XHCI_TRB_CONFIG_EP      12  /* Configure Endpoint Command */
#define XHCI_TRB_NOOP_CMD       23  /* No Op Command */

/* Event TRB types */
#define XHCI_TRB_TRANSFER_EVT   32  /* Transfer Event */
#define XHCI_TRB_CMD_COMPLETE   33  /* Command Completion Event */
#define XHCI_TRB_PORT_STATUS    34  /* Port Status Change Event */

/* Completion codes (status field bits 31:24) */
#define XHCI_TRB_CC_SHIFT       24
#define XHCI_TRB_CC_SUCCESS     1
#define XHCI_TRB_CC_SHORT_PKT   13

/* Command Completion Event: slot ID (control field bits 31:24) */
#define XHCI_TRB_SLOT_SHIFT     24
#define XHCI_TRB_SLOT_MASK      (0xFFu << 24)

/* ---- Ring state ---------------------------------------------------------- */

#define XHCI_RING_SIZE          256  /* TRBs per ring (including Link TRB) */

struct xhci_ring {
    struct xhci_trb *trbs;      /* Virtual address of TRB array */
    uint64_t         phys;      /* Physical address of TRB array */
    uint32_t         enqueue;   /* Producer index (next slot to write) */
    uint32_t         dequeue;   /* Consumer index (next slot to read) */
    uint32_t         size;      /* Total TRBs (including Link for cmd ring) */
    uint8_t          cycle;     /* Current producer/consumer cycle state */
};

/* ---- Event Ring Segment Table (ERST) ------------------------------------- */

struct xhci_erst_entry {
    uint64_t ring_base;         /* Physical address of event ring segment */
    uint32_t ring_size;         /* Number of TRBs in this segment */
    uint32_t reserved;          /* Must be zero */
} __attribute__((packed));

/* ---- Runtime Interrupter Register offsets (from rt_base + 0x20) ---------- */
#define XHCI_IR_OFFSET          0x20    /* Interrupter 0 base (rt_base + 0x20) */
#define XHCI_IR_IMAN            0x00    /* Interrupter Management */
#define XHCI_IR_IMOD            0x04    /* Interrupter Moderation */
#define XHCI_IR_ERSTSZ          0x08    /* Event Ring Segment Table Size */
#define XHCI_IR_ERSTBA          0x10    /* Event Ring Segment Table Base Addr (8B) */
#define XHCI_IR_ERDP            0x18    /* Event Ring Dequeue Pointer (8B) */

/* IMAN bits */
#define XHCI_IMAN_IP            (1 << 0)    /* Interrupt Pending */
#define XHCI_IMAN_IE            (1 << 1)    /* Interrupt Enable */

/* ---- API ----------------------------------------------------------------- */

/* Initialize Command Ring and Event Ring for a controller.
 * Must be called after controller reset and before USBCMD.RS=1.
 * Returns 0 on success, -1 on failure. */
int xhci_rings_init(struct xhci_controller *hc);

/* Submit a TRB on the Command Ring.  Enqueues, sets cycle bit,
 * advances enqueue pointer, and rings doorbell 0.
 * Returns 0 on success, -1 if ring is full. */
int xhci_cmd_submit(struct xhci_controller *hc, struct xhci_trb *trb);

/* Poll the Event Ring for a completed event.  If an event is available
 * (cycle bit matches), copies it to *out, advances dequeue, writes ERDP.
 * Returns 1 if event read, 0 if none available. */
int xhci_event_poll(struct xhci_controller *hc, struct xhci_trb *out);
