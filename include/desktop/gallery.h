/* ============================================================================
 * gallery.h — Control Gallery dialog
 *
 * A showcase window displaying all implemented controls (Button, Label,
 * TextBox, ScrollBar) for visual testing and demonstration.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Open the Control Gallery window.
 * Creates a window with all available controls arranged in sections.
 * Safe to call multiple times — reopens if previously closed. */
void gallery_open(void);

/* Render the gallery controls into its window buffer.
 * Call each frame before wm_composite() (same as terminal_render). */
void gallery_render(void);

/* Route mouse input to gallery controls.
 * (cx, cy) are client-area coordinates. Returns 1 if consumed. */
int gallery_handle_mouse(int32_t cx, int32_t cy, uint8_t buttons);

/* Route keyboard input to gallery controls.
 * Returns 1 if consumed. */
int gallery_handle_key(char key);

/* Check if the gallery window is currently open */
int gallery_is_open(void);
