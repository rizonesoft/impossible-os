/* ============================================================================
 * mouse.h -- PS/2 Mouse driver
 *
 * Handles IRQ 12, parses 3-byte mouse packets, and tracks cursor position.
 * Cursor rendering is handled by the cursor manager (cursor.h).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Mouse button bit masks */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

/* Mouse state (updated by IRQ handler) */
struct mouse_state {
    int32_t  x;         /* cursor X position (pixels) */
    int32_t  y;         /* cursor Y position (pixels) */
    uint8_t  buttons;   /* current button state (MOUSE_BTN_*) */
};

/* Initialize the PS/2 mouse driver (registers IRQ 12) */
/* Probe and initialize the PS/2 auxiliary (mouse) port. Returns a
 * `boot_probe_result`: OK on a working mouse, ABSENT when there is no i8042 or
 * no auxiliary port at all, FAILED when the device answered and then failed
 * (reset NACK, BAT failure, device-ID timeout, command NACK). */
int mouse_init(void);

/* Get the current mouse state (position + buttons) */
struct mouse_state mouse_get_state(void);
uint32_t mouse_get_irq_count(void);

/* 1 if the PS/2 mouse init reached the success path (i8042 present + ACKed),
 * 0 on every early-exit (no i8042 / ACPI hardware-reduced / NACK). For the
 * input-source diagnostic summary. */
int mouse_is_present(void);
void mouse_set_position(int32_t x, int32_t y);

/* Inject absolute mouse state (used by Hyper-V synthetic mouse).
 * Sets position and button state directly -- no PS/2 parsing. */
void mouse_inject_state(int32_t x, int32_t y, uint8_t buttons);

/* Merge a button-bearing absolute pointing source (VirtIO tablet) into the
 * shared cursor. Position is edge-triggered (moves only when the absolute
 * report changes), so a relative mouse's deltas survive between absolute
 * updates; absolute buttons are OR-merged with the other sources. Compositor-
 * thread use; the shared cursor is the single merged state read via
 * mouse_get_state. */
void mouse_merge_absolute(int32_t x, int32_t y, uint8_t buttons);

/* Merge a position-ONLY absolute source (VBox VMMDev, which has no buttons of
 * its own -- its button state is read back from the PS/2 mouse). Edge-triggered
 * position; does not touch the absolute button slot. Compositor-thread use. */
void mouse_merge_absolute_position(int32_t x, int32_t y);

#ifdef KERNEL_TESTS
/* Test-only seam: set the PS/2 source button slot and OR-publish, exactly as
 * the PS/2 IRQ packet commit does, without driving live port I/O or mouse_init.
 * Lets unit tests cover the PS/2+USB cross-device button OR-merge. */
void mouse_test_set_ps2_buttons(uint8_t buttons);
/* Test-only seam: one USB mouse's slot, unaffected by what other mice hold. */
uint8_t mouse_test_usb_source_buttons(int usb_source);
#endif

/* USB mice tracked independently for button state. Must cover every value of
 * xhci_device_index(); xhci_dev.c pins that with a _Static_assert. */
#define MOUSE_USB_SOURCE_COUNT 64

/* Store one mouse's buttons in slots[MOUSE_USB_SOURCE_COUNT] and return the new
 * OR of all slots, given merged, the current OR. Pure, so the merge can be tested
 * on private slots while real mice report; source must already be in range. */
uint8_t mouse_usb_slot_store(uint8_t *slots, int source, uint8_t buttons, uint8_t merged);

/* Apply a relative cursor delta + button state (USB HID boot-protocol mouse).
 * usb_source identifies the mouse (its xhci_device_index()): buttons replace
 * that mouse's slot only, and the published state is the OR of all slots, so
 * one mouse's idle report cannot release a button held on another. Returns 0,
 * or -1 with nothing applied when usb_source is out of range.
 * Shares the PS/2 cursor state so the compositor stays source-blind in the
 * relative-source path. dx/dy are screen-oriented (+X right, +Y down -- USB
 * HID reports Y downward already, no PS/2-style inversion). Clamps to the
 * framebuffer. Cursor state is guarded by an irqsave lock (callable from the
 * USB tick-ISR poller alongside the PS/2 IRQ and the compositor thread). */
int mouse_update_relative(int usb_source, int32_t dx, int32_t dy, uint8_t buttons);

/* Release every button held by one USB mouse. Call when it stops reporting: no
 * later report can clear its slot, so a button held at that moment would stay
 * pressed. Returns 0, or -1 when usb_source is out of range. ISR-safe. */
int mouse_usb_source_release(int usb_source);
