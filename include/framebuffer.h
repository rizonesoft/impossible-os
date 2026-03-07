/* ============================================================================
 * framebuffer.h — Framebuffer graphics driver
 *
 * Renders text and graphics on the GOP framebuffer using an embedded bitmap
 * font.  Supports double buffering, drawing primitives, and block copy.
 * ============================================================================ */

#pragma once

#include "types.h"

/* ---- Lifecycle ---- */

/* Initialize the framebuffer console (call after multiboot2_parse + heap_init) */
void fb_init(void);

/* Clear the screen with background color */
void fb_clear(void);

/* ---- Text rendering ---- */

/* Write a single character at the current cursor position */
void fb_putchar(char c);

/* Write a string to the framebuffer */
void fb_write(const char *str);

/* Scroll the framebuffer up by one text row */
void fb_scroll(void);

/* Set text colors */
void fb_set_color(uint32_t fg, uint32_t bg);

/* ---- Pixel / drawing primitives ---- */

/* Put a pixel at (x, y) with the given RGB color */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);

/* Filled rectangle */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color);

/* Outline rectangle (1 px border) */
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color);

/* Bresenham line from (x0,y0) to (x1,y1) */
void fb_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  uint32_t color);

/* Circle outline (midpoint algorithm) */
void fb_draw_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color);

/* Filled circle */
void fb_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color);

/* ---- Double buffering ---- */

/* Block-copy a rectangular region from src buffer into the back buffer */
void fb_blit(uint32_t dst_x, uint32_t dst_y,
             const uint32_t *src, uint32_t w, uint32_t h, uint32_t src_pitch);

/* Copy the back buffer to the hardware framebuffer */
void fb_swap(void);

/* ---- Queries ---- */

uint32_t fb_get_width(void);
uint32_t fb_get_height(void);

/* ---- Predefined colors (32-bit ARGB) ---- */

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
