/* ============================================================================
 * mouse.h — PS/2 Mouse driver
 *
 * Handles IRQ 12, parses 3-byte mouse packets, tracks cursor position,
 * and renders a mouse cursor sprite on the framebuffer.
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
void mouse_init(void);

/* Get the current mouse state (position + buttons) */
struct mouse_state mouse_get_state(void);
uint32_t mouse_get_irq_count(void);
void mouse_set_position(int32_t x, int32_t y);

/* Draw / redraw the mouse cursor at its current position.
 * Call this once per frame after compositing. */
void mouse_draw_cursor(void);

/* Save and restore the framebuffer region under the cursor.
 * Call save before drawing, restore before the next redraw. */
void mouse_save_under(void);
void mouse_restore_under(void);
