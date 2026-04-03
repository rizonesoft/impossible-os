/* ============================================================================
 * vbox_mouse.h -- VirtualBox VMMDev absolute mouse driver
 *
 * Provides absolute mouse coordinates via the VirtualBox Guest Device
 * (PCI vendor 0x80EE, device 0xCAFE).  Uses VMMDev packet-based protocol
 * to enable mouse integration (absolute coordinates in range 0-0xFFFF,
 * scaled to framebuffer resolution).
 *
 * Buttons are NOT provided by VMMDev -- they continue coming from the PS/2
 * mouse driver.  Only position is absolute.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/drivers/mouse.h"   /* struct mouse_state, MOUSE_BTN_* */

/* Initialize the VBoxGuest VMMDev mouse integration.
 * Returns 0 on success, -1 if the VBox device is not present. */
int vbox_mouse_init(void);

/* Query latest absolute mouse position (polled from VMMDev).
 * Coordinates are already scaled to framebuffer pixels.
 * Buttons are NOT available from VMMDev -- caller must merge with PS/2. */
struct mouse_state vbox_mouse_get_state(void);

/* Returns 1 if the VBox absolute mouse driver is active. */
uint8_t vbox_mouse_available(void);
