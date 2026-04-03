/* ============================================================================
 * spinner.h -- Progressive arc-ring spinner component
 *
 * Windows 11-style breathing spinner: a dynamic arc that rotates while its
 * sweep angle oscillates (stretching and shrinking).  Pure integer math,
 * no FPU -- safe for any context.
 *
 * Usage (boot splash integration):
 *   spinner_init(cx, cy, radius, stroke, color);
 *   spinner_start();          // marks spinner active (non-blocking)
 *   spinner_advance();        // render + swap one frame (call from tick)
 *   ...boot sequence...
 *   spinner_stop();           // marks spinner inactive
 *
 * Part of the Progressive Spinner component (TODO-010.97).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Initialization ---- */

/* Set up the spinner at position (cx, cy) with given dimensions and color.
 * Does NOT start animation -- call spinner_start() when ready. */
void spinner_init(int32_t cx, int32_t cy,
                  int32_t radius, int32_t stroke,
                  uint32_t color);

/* ---- Animation control ---- */

/* Mark spinner as active (non-blocking -- returns immediately).
 * Call spinner_advance() periodically to render frames. */
void spinner_start(void);

/* Advance one animation frame: render + swap bounding rect.
 * Call from boot_splash_tick() or any periodic callback.
 * No-op if spinner is not active. */
void spinner_advance(void);

/* Mark spinner as inactive.
 * Leaves the last frame on screen -- caller should clear if needed. */
void spinner_stop(void);

/* ---- Rendering helpers ---- */

/* Draw one frame of the spinner at the given fade level (0–255).
 * Used during boot splash fade-in/fade-out sequences.
 * Purely a render call -- caller must fb_swap_rect() afterwards. */
void spinner_draw_faded(uint8_t fade);

/* Return the bounding box of the spinner area (for fb_swap_rect). */
void spinner_get_bounds(uint32_t *out_x, uint32_t *out_y,
                        uint32_t *out_w, uint32_t *out_h);

/* ---- State query ---- */

/* Returns 1 if the spinner is currently animating, 0 otherwise. */
int spinner_is_active(void);
