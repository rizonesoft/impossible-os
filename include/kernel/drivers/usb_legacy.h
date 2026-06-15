/* ============================================================================
 * usb_legacy.h -- Boot-time legacy USB controller detection + graceful skip
 *                 (TODO-19 USB boot hardening -- boot integration)
 *
 * Scans PCI for EHCI / UHCI / OHCI host controllers and, when no xHCI is
 * available, reports exactly which legacy controller is present and degrades
 * gracefully (no hang) instead of silently leaving USB boot storage
 * unavailable. This file is DETECTION + DIAGNOSTICS only -- no MMIO, no
 * transfers, no host-controller driver.
 *
 * The actual EHCI/UHCI host-controller DRIVER (port reset, async/periodic
 * schedule, bulk transfer, shared BOT layer) is owned by the USB stack TODO:
 * the transport-agnostic usb_core HCD abstraction and the EHCI HCD live in
 * 04-drivers-hardware/TODO-10. A real fallback that drives storage requires
 * that abstraction so the MSC/BOT class layer is shared rather than duplicated;
 * until it lands, this layer makes the "zero hangs on legacy hardware" promise
 * real by classifying + logging + skipping cleanly.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    USB_LEGACY_NONE = 0,  /* not a USB host controller, or it is xHCI */
    USB_LEGACY_UHCI = 1,  /* PCI prog-if 0x00 -- USB 1.1 (Intel/VIA UHCI) */
    USB_LEGACY_OHCI = 2,  /* PCI prog-if 0x10 -- USB 1.1 (OHCI) */
    USB_LEGACY_EHCI = 3   /* PCI prog-if 0x20 -- USB 2.0 (EHCI) */
} usb_legacy_class_t;

/* PCI prog-if values for the USB controller subclass (class 0x0C, subclass 0x03). */
#define USB_PROGIF_UHCI  0x00u
#define USB_PROGIF_OHCI  0x10u
#define USB_PROGIF_EHCI  0x20u
#define USB_PROGIF_XHCI  0x30u

/* Classify a PCI (class, subclass, prog-if) triple into a legacy USB controller
 * class. Returns USB_LEGACY_NONE for non-USB devices and for xHCI. Pure;
 * exposed for unit tests. */
usb_legacy_class_t usb_legacy_classify(uint8_t cls, uint8_t sub, uint8_t prog_if);

/* Human-readable class name ("UHCI"/"OHCI"/"EHCI"/"none"). Pure. */
const char *usb_legacy_class_name(usb_legacy_class_t c);

/* Scan the PCI bus for legacy USB controllers and record per-class counts. Call
 * once after pci_scan(). Idempotent (a second call is a no-op). */
void usb_legacy_scan(void);

/* Number of detected controllers of a given class (valid after usb_legacy_scan). */
uint32_t usb_legacy_count(usb_legacy_class_t c);

/* Called after xhci_init() with its controller count. When no xHCI is present
 * but a legacy USB controller is, log exactly which controller exists and that
 * USB boot storage via it is pending the EHCI/UHCI HCD, then return so the boot
 * continues without a hang. OHCI-only hardware gets the explicit "not supported"
 * message. No-op when an xHCI is present or no USB controller exists at all. */
void usb_legacy_announce(int xhci_count);
