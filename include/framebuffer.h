/* ============================================================================
 * framebuffer.h — Framebuffer console driver
 *
 * Renders text on the GOP framebuffer using an embedded bitmap font.
 * ============================================================================ */

#pragma once

#include "types.h"

/* Initialize the framebuffer console (call after multiboot2_parse) */
void fb_init(void);

/* Clear the screen with background color */
void fb_clear(void);

/* Write a single character at the current cursor position */
void fb_putchar(char c);

/* Write a string to the framebuffer */
void fb_write(const char *str);

/* Put a pixel at (x, y) with the given RGB color */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);

/* Scroll the framebuffer up by one text row */
void fb_scroll(void);

/* Set text colors */
void fb_set_color(uint32_t fg, uint32_t bg);

/* Predefined colors (32-bit ARGB) */
#define FB_COLOR_BLACK       0x00000000
#define FB_COLOR_WHITE       0x00FFFFFF
#define FB_COLOR_LIGHT_GRAY  0x00C0C0C0
#define FB_COLOR_DARK_GRAY   0x00404040
#define FB_COLOR_RED         0x00FF4444
#define FB_COLOR_GREEN       0x0044FF44
#define FB_COLOR_BLUE        0x004488FF
#define FB_COLOR_YELLOW      0x00FFFF44
#define FB_COLOR_CYAN        0x0044FFFF
#define FB_COLOR_MAGENTA     0x00FF44FF
#define FB_COLOR_ORANGE      0x00FF8800
#define FB_COLOR_BG_DEFAULT  0x001A1A2E
#define FB_COLOR_FG_DEFAULT  0x00E0E0E0
