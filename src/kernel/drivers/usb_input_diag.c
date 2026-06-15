/* ============================================================================
 * usb_input_diag.c -- Boot-time input-source diagnostic summary
 *
 * Reports which input sources came up (PS/2 keyboard/mouse, USB HID keyboard/
 * mouse, VirtIO tablet), the identity of each enumerated USB HID device, and
 * the HID interrupt-poll error statistics. Read-only over driver state; called
 * once at boot after every input driver has initialized.
 * ============================================================================ */

#include "kernel/drivers/usb_input_diag.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/virtio/input.h"
#include "kernel/klog.h"

/* Bounded append: copy s into buf at pos, never exceeding cap-1 (room for NUL).
 * Returns the new position. */
static int diag_append(char *buf, int cap, int pos, const char *s)
{
    while (*s && pos < cap - 1)
        buf[pos++] = *s++;
    return pos;
}

int usb_input_diag_format(char *buf, int cap, int ps2, int usb_kbd,
                          int usb_mouse, int virtio)
{
    int pos = 0;

    if (!buf || cap < 1)
        return 0;

    pos = diag_append(buf, cap, pos, "[INPUT] Sources: PS/2=");
    pos = diag_append(buf, cap, pos, ps2 ? "yes" : "no");
    pos = diag_append(buf, cap, pos, " USB_KBD=");
    pos = diag_append(buf, cap, pos, usb_kbd ? "yes" : "no");
    pos = diag_append(buf, cap, pos, " USB_MOUSE=");
    pos = diag_append(buf, cap, pos, usb_mouse ? "yes" : "no");
    pos = diag_append(buf, cap, pos, " VIRTIO=");
    pos = diag_append(buf, cap, pos, virtio ? "yes" : "no");
    buf[pos] = '\0';
    return pos;
}

void usb_input_diag_report(void)
{
    char line[96];
    int usb_kbd = 0, usb_mouse = 0;
    int ps2;
    uint32_t ep_errors = 0, requeue_fails = 0, reports = 0;
    int i, nc;

    /* Scan enumerated USB devices for HID boot keyboards/mice. */
    for (i = 0; i < XHCI_MAX_DEVICES; i++) {
        struct xhci_device *dev = xhci_get_device(i);
        if (!dev || !dev->is_hid)
            continue;
        if (dev->hid_proto == USB_PROTO_HID_KEYBOARD)
            usb_kbd = 1;
        else if (dev->hid_proto == USB_PROTO_HID_MOUSE)
            usb_mouse = 1;
    }

    ps2 = (keyboard_is_present() || mouse_is_present()) ? 1 : 0;

    usb_input_diag_format(line, (int)sizeof(line), ps2, usb_kbd, usb_mouse,
                          virtio_input_available() ? 1 : 0);
    klog(LOG_INFO, "input", "%s", line);

    /* Per-USB-HID device identity: vendor, product, protocol, EP interval. */
    for (i = 0; i < XHCI_MAX_DEVICES; i++) {
        struct xhci_device *dev = xhci_get_device(i);
        if (!dev || !dev->is_hid)
            continue;
        klog(LOG_INFO, "input",
             "  USB HID %s: VID=%04x PID=%04x port=%u interval=%u maxpkt=%u",
             dev->hid_proto == USB_PROTO_HID_KEYBOARD ? "keyboard" : "mouse",
             (uint64_t)dev->vendor_id, (uint64_t)dev->product_id,
             (uint64_t)dev->port, (uint64_t)dev->int_in_interval,
             (uint64_t)dev->int_in_max_pkt);
    }

    /* Aggregate HID interrupt-poll error statistics across controllers. */
    nc = xhci_controller_count();
    for (i = 0; i < nc; i++) {
        const struct xhci_controller *hc = xhci_get_controller(i);
        if (!hc)
            continue;
        ep_errors     += hc->hid_ep_errors;
        requeue_fails += hc->hid_requeue_fails;
        reports       += hc->hid_reports;
    }
    klog(LOG_INFO, "input",
         "  HID poll: reports=%u ep_errors=%u requeue_fails=%u",
         (uint64_t)reports, (uint64_t)ep_errors, (uint64_t)requeue_fails);
}
