/* ============================================================================
 * font.h — Desktop bitmap font renderer
 *
 * Position-based text rendering API for the GUI layer.  Draws characters and
 * strings at arbitrary (x, y) pixel positions on the framebuffer using an
 * embedded 8×16 bitmap font.
 *
 * Unlike the kernel console (fb_putchar/fb_write), this module does NOT manage
 * a text cursor or scrolling — that is the window manager's job.
 * ============================================================================ */

#pragma once

#include "types.h"

/* ---- Font metrics ---- */

#define FONT_WIDTH   8
#define FONT_HEIGHT  16

/* ---- Rendering ---- */

/* Draw a single character at pixel position (x, y).
 * Returns the x-advance (FONT_WIDTH) so callers can chain. */
uint32_t font_draw_char(uint32_t x, uint32_t y, char c,
                        uint32_t fg, uint32_t bg);

/* Draw a null-terminated string starting at pixel position (x, y).
 * Handles '\n' (drops to next line) and '\t' (4-char tab stop).
 * Returns the final x position after the last character. */
uint32_t font_draw_string(uint32_t x, uint32_t y, const char *str,
                          uint32_t fg, uint32_t bg);

/* Draw a string with a maximum length (not necessarily null-terminated). */
uint32_t font_draw_string_n(uint32_t x, uint32_t y, const char *str,
                            uint32_t len, uint32_t fg, uint32_t bg);

/* ---- Measurement ---- */

/* Return the width in pixels of a null-terminated string.
 * Does NOT account for newlines — measures as if on one line. */
uint32_t font_text_width(const char *str);

/* Return the width in pixels of a string with a given length. */
uint32_t font_text_width_n(const char *str, uint32_t len);

/* Return the height in pixels of a string (accounts for '\n' newlines). */
uint32_t font_text_height(const char *str);

/* Return the font's character height (constant FONT_HEIGHT). */
uint32_t font_char_height(void);

/* Return the font's character width (constant FONT_WIDTH). */
uint32_t font_char_width(void);

/* ---- Font data access ---- */

/* Get a pointer to the raw glyph data for a character.
 * Returns a pointer to FONT_HEIGHT bytes (one byte per row, MSB = leftmost).
 * Returns the space glyph for unprintable characters. */
const uint8_t *font_get_glyph(char c);
