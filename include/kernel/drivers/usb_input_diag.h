/* ============================================================================
 * usb_input_diag.h -- Boot-time input-source diagnostic summary
 *
 * Emits a structured snapshot of which input sources are present (PS/2, USB
 * HID keyboard/mouse, VirtIO tablet), per-USB-HID device identity, and the
 * HID interrupt-poll error statistics. Diagnostic-only; no input behavior.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Build the input-source summary line into buf (NUL-terminated), e.g.
 * "[INPUT] Sources: PS/2=yes USB_KBD=no USB_MOUSE=yes VIRTIO=no".
 * Returns the written length (excluding the NUL). Pure -- exposed for tests. */
int usb_input_diag_format(char *buf, int cap, int ps2, int usb_kbd,
                          int usb_mouse, int virtio);

/* Emit the boot input diagnostic summary to the kernel log: the source line,
 * one line per enumerated USB HID device (vendor/product/protocol/interval),
 * and the aggregate HID-poll error stats. Call once after all input drivers
 * have initialized. */
void usb_input_diag_report(void);
