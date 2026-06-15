/* ============================================================================
 * usb_boot_report.h -- USB boot diagnostic report (TODO-19 USB boot hardening)
 *
 * Emits a consolidated "=== USB Boot Report ===" block to the kernel log at the
 * end of boot: host controllers (xHCI + version, legacy EHCI/UHCI/OHCI counts),
 * each enumerated device, Intel XUSB2PR routing status, SCSI retry statistics,
 * and the boot media speed class. Aggregator only -- it reads getters owned by
 * the xHCI driver, the MSC class driver, the legacy-controller detector, and the
 * boot-media probe; it owns no device state of its own.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Human-readable USB speed name for a USB_SPEED_* constant ("LS"/"FS"/"HS"/"SS",
 * "?" for unknown). Pure; exposed for tests. */
const char *usb_speed_name(uint8_t speed);

/* Device class tag for the report ("MSC"/"HID"/"other") from the per-device
 * is_msc / is_hid flags. Pure; exposed for tests. */
const char *usb_boot_report_device_class(uint8_t is_msc, uint8_t is_hid);

/* Emit the USB boot diagnostic report to the kernel log. Call once at the end of
 * USB enumeration, AFTER boot_media_probe() so the media-speed line is real
 * (not BOOT_MEDIA_UNKNOWN). Reads-only -- safe to skip if USB is absent. */
void usb_boot_report(void);
