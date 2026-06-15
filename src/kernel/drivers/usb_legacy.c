/* ============================================================================
 * usb_legacy.c -- Boot-time legacy USB controller detection + graceful skip
 *                 (TODO-19 USB boot hardening -- boot integration)
 *
 * See usb_legacy.h for the contract. Detection + diagnostics only: pure PCI
 * config-space reads plus klog. No MMIO, no DMA, no host-controller driver.
 * ============================================================================ */

#include "kernel/drivers/usb_legacy.h"
#include "kernel/drivers/pci.h"
#include "kernel/klog.h"

#define USB_PCI_CLASS     0x0Cu  /* Serial Bus Controller */
#define USB_PCI_SUBCLASS  0x03u  /* USB Controller */

/* Per-class detected-controller counts, indexed by usb_legacy_class_t. Written
 * once during usb_legacy_scan() (boot Phase 2, single-threaded) and read after.
 * Same boot-once-then-read model as xHCI's num_controllers. */
static uint32_t s_counts[4];
static uint8_t  s_scanned;

usb_legacy_class_t usb_legacy_classify(uint8_t cls, uint8_t sub, uint8_t prog_if)
{
    if (cls != USB_PCI_CLASS || sub != USB_PCI_SUBCLASS)
        return USB_LEGACY_NONE;
    switch (prog_if) {
    case USB_PROGIF_UHCI: return USB_LEGACY_UHCI;
    case USB_PROGIF_OHCI: return USB_LEGACY_OHCI;
    case USB_PROGIF_EHCI: return USB_LEGACY_EHCI;
    default:              return USB_LEGACY_NONE;  /* xHCI (0x30) or unknown */
    }
}

const char *usb_legacy_class_name(usb_legacy_class_t c)
{
    switch (c) {
    case USB_LEGACY_UHCI: return "UHCI";
    case USB_LEGACY_OHCI: return "OHCI";
    case USB_LEGACY_EHCI: return "EHCI";
    default:              return "none";
    }
}

uint32_t usb_legacy_count(usb_legacy_class_t c)
{
    if ((unsigned)c >= 4u)
        return 0;
    return s_counts[c];
}

void usb_legacy_scan(void)
{
    uint16_t bus;
    uint8_t  dev, func;

    if (s_scanned)
        return;
    s_scanned = 1;

    for (bus = 0; bus < PCI_MAX_BUS; bus++) {
        /* Bus-empty fast skip (mirrors pci_scan): if device 0 is absent the whole
         * bus is empty -- avoids ~65K config reads on bare metal. */
        if (pci_read16((uint8_t)bus, 0, 0, PCI_VENDOR_ID) == 0xFFFF)
            continue;

        for (dev = 0; dev < PCI_MAX_DEV; dev++) {
            for (func = 0; func < PCI_MAX_FUNC; func++) {
                uint16_t vid = pci_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID);
                if (vid == 0xFFFF)
                    continue;

                uint8_t cls     = pci_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub     = pci_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                uint8_t prog_if = pci_read8((uint8_t)bus, dev, func, PCI_PROG_IF);

                usb_legacy_class_t lc = usb_legacy_classify(cls, sub, prog_if);
                if (lc != USB_LEGACY_NONE) {
                    s_counts[lc]++;
                    klog(LOG_INFO, "usb",
                         "Found %s controller at PCI %u:%u.%u (VID %04x)",
                         usb_legacy_class_name(lc), (uint64_t)bus, (uint64_t)dev,
                         (uint64_t)func, (uint64_t)vid);
                }

                /* Skip phantom functions on single-function devices (mirrors
                 * pci_scan): only probe func 1-7 when the multifunction header
                 * bit is set, so a device that mirrors func-0 config across
                 * inactive functions is not counted 8 times. */
                if (func == 0) {
                    uint8_t hdr = pci_read8((uint8_t)bus, dev, func, PCI_HEADER_TYPE);
                    if (!(hdr & 0x80))
                        break;
                }
            }
        }
    }
}

void usb_legacy_announce(int xhci_count)
{
    uint32_t ehci, uhci, ohci;

    if (xhci_count > 0)
        return;  /* xHCI present -- the real USB path is live, nothing to announce */

    ehci = s_counts[USB_LEGACY_EHCI];
    uhci = s_counts[USB_LEGACY_UHCI];
    ohci = s_counts[USB_LEGACY_OHCI];

    if (ehci == 0 && uhci == 0 && ohci == 0)
        return;  /* no USB controller at all -- xhci_init already logged that */

    if (ehci != 0 || uhci != 0) {
        /* USB 2.0/1.1 host controller(s) exist but we have no driver for them
         * yet. Degrade gracefully: storage boot continues on SATA/NVMe; USB
         * input falls back to PS/2. The EHCI/UHCI HCD is pending the USB-core
         * abstraction work in the USB stack TODO. */
        klog(LOG_WARN, "usb",
             "%u EHCI + %u UHCI controller(s) present but no xHCI -- USB boot "
             "storage via legacy controllers not yet available; continuing "
             "without USB boot (no hang)",
             (uint64_t)ehci, (uint64_t)uhci);
        return;
    }

    /* OHCI-only hardware: no OHCI driver is planned for boot storage. */
    klog(LOG_WARN, "usb",
         "OHCI-only controller -- USB storage not supported on this hardware");
}
