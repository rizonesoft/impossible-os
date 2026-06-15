/* ============================================================================
 * usb_boot_report.c -- USB boot diagnostic report (TODO-19 USB boot hardening)
 *
 * See usb_boot_report.h for the contract. Aggregator only: reads getters from
 * the xHCI driver, the MSC class driver, the legacy-controller detector, and the
 * boot-media probe, and emits a consolidated report via klog. No device state,
 * no MMIO, no transfers.
 * ============================================================================ */

#include "kernel/drivers/usb_boot_report.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/usb_msc.h"
#include "kernel/drivers/usb_legacy.h"
#include "kernel/boot_media.h"
#include "kernel/klog.h"

const char *usb_speed_name(uint8_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:   return "LS";
    case USB_SPEED_FULL:  return "FS";
    case USB_SPEED_HIGH:  return "HS";
    case USB_SPEED_SUPER: return "SS";
    case USB_SPEED_SUPER_PLUS: return "SS+";
    default:              return "?";
    }
}

const char *usb_boot_report_device_class(uint8_t is_msc, uint8_t is_hid)
{
    if (is_msc)
        return "MSC";
    if (is_hid)
        return "HID";
    return "other";
}

void usb_boot_report(void)
{
    int nc = xhci_controller_count();
    int i;
    int dev_count = 0;
    uint32_t ehci, uhci, ohci;
    uint16_t keys;

    klog(LOG_INFO, "usb", "=== USB Boot Report ===");

    /* Host controllers. */
    for (i = 0; i < nc; i++) {
        const struct xhci_controller *hc = xhci_get_controller(i);
        if (!hc)
            continue;
        klog(LOG_INFO, "usb", "  xHCI #%d v%u.%u",
             (uint64_t)i, (uint64_t)((hc->hci_version >> 8) & 0xFF),
             (uint64_t)(hc->hci_version & 0xFF));
    }
    ehci = usb_legacy_count(USB_LEGACY_EHCI);
    uhci = usb_legacy_count(USB_LEGACY_UHCI);
    ohci = usb_legacy_count(USB_LEGACY_OHCI);
    if (ehci || uhci || ohci)
        klog(LOG_INFO, "usb", "  legacy controllers (no driver): EHCI=%u UHCI=%u OHCI=%u",
             (uint64_t)ehci, (uint64_t)uhci, (uint64_t)ohci);

    /* Enumerated devices. */
    for (i = 0; i < XHCI_MAX_DEVICES; i++) {
        struct xhci_device *dev = xhci_get_device(i);
        if (!dev)
            continue;
        dev_count++;
        /* Include the raw PORTSC speed id alongside the name so an unrecognized
         * speed (a future PSIV, USB 3.2 Gen2x2, or a corrupt value -- anything
         * usb_speed_name renders "?") stays actionable in the report. */
        klog(LOG_INFO, "usb", "  dev port %u spd%u(%s) %04x:%04x %s",
             (uint64_t)dev->port, (uint64_t)dev->speed, usb_speed_name(dev->speed),
             (uint64_t)dev->vendor_id, (uint64_t)dev->product_id,
             usb_boot_report_device_class(dev->is_msc, dev->is_hid));
    }
    if (dev_count == 0)
        klog(LOG_INFO, "usb", "  (no USB devices enumerated)");

    /* Intel USB 2.0 port routing. */
    klog(LOG_INFO, "usb", "  XUSB2PR routing applied on %d controller(s)",
         (uint64_t)xhci_xusb2pr_routed_count());

    /* SCSI retry statistics. */
    keys = usb_msc_sense_keys_seen();
    klog(LOG_INFO, "usb", "  SCSI retries: %u; sense keys seen: 0x%04x",
         (uint64_t)usb_msc_total_retries(), (uint64_t)keys);
    for (i = 0; i < 16; i++) {
        if (keys & (uint16_t)(1u << i))
            klog(LOG_INFO, "usb", "    sense key %u (%s)",
                 (uint64_t)i, msc_sense_key_name((uint8_t)i));
    }

    /* Boot media speed class. */
    klog(LOG_INFO, "usb", "  Boot media speed: %s",
         boot_media_speed_name(boot_media_speed()));
    klog(LOG_INFO, "usb", "=== End USB Boot Report ===");
}
