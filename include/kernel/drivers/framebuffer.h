/* ============================================================================
 * framebuffer.h -- Framebuffer graphics driver
 *
 * Renders text and graphics on the GOP framebuffer using an embedded bitmap
 * font.  Supports double buffering, drawing primitives, and block copy.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

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

/* Read a pixel from the back buffer at (x, y) */
uint32_t fb_read_pixel(uint32_t x, uint32_t y);

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

/* Copy only a rectangular region of the back buffer to the hardware framebuffer.
 * Used by the dirty-rect compositor for partial updates (much faster). */
void fb_swap_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* ---- Queries ---- */

uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t *fb_get_backbuffer(void);
uint32_t fb_get_stride(void);

/* ---- Snapshot ---- */

/* Return the number of bytes a fb_snapshot() caller must allocate in dest_buf
 * to capture the current frame: width * height * 4. Returns 0 when the
 * framebuffer is not yet initialized. Callers should use pmm_alloc_contiguous
 * for the buffer since a 1280x720 snapshot is 3.5 MiB. */
uint64_t fb_snapshot_size(void);

/* Copy the current back buffer into dest_buf as a tightly-packed 32-bit
 * bitmap (width * height * 4 bytes). The channel order is the native GOP
 * pixel format the bootloader negotiated: BGRX on all shipping hardware
 * today, though fb_init also accepts RGBX per the guardrail at
 * framebuffer.c:218. Callers should treat the data as raw framebuffer
 * bytes and reuse the FB_COLOR_* constants in this header for any per-pixel
 * comparison; do NOT assume strict BGRA semantics.
 *
 * On success *width and *height are set to the framebuffer dimensions.
 * Returns 0 on success, -1 on invalid args or uninitialized framebuffer.
 * Caller must size dest_buf >= fb_snapshot_size().
 *
 * Concurrency: fb_snapshot does NOT hold a lock across the copy. A
 * concurrent fb_blit / fb_put_pixel / spinner_advance on any CPU (or in
 * an IRQ callback on this one) can tear the snapshot. Callers needing a
 * stable frame must quiesce writers first (e.g., stop the boot splash
 * spinner, or hold a compositor-wide snapshot mutex once the desktop
 * test-isolation layer wires one). Kernel test code calls this after
 * the compositor has rendered to verify the screen is not black /
 * not broken. */
int fb_snapshot(void *dest_buf, uint32_t *width, uint32_t *height);

/* ---- Multi-output snapshot (TODO-05 multi-monitor test matrix) ---- */

/* Number of framebuffer outputs currently addressable. Returns 1 today:
 * every shipping path (GOP, Bochs VGA) exposes a single scanout. The
 * API slot exists so callers can iterate outputs without a compile-
 * time assumption about count; when a virtio-gpu multi-output driver
 * lands (see the TODO-05 test-isolation section's virtio-gpu prereq
 * item), this returns the negotiated `max_outputs` value. */
uint32_t fb_get_output_count(void);

/* Per-output variant of fb_snapshot. `index` must be in [0, output_count).
 * `dest_buf` / `width` / `height` follow the same contract as
 * fb_snapshot. Returns 0 on success, -1 on invalid args or uninit fb,
 * -2 when `index >= fb_get_output_count()`.
 *
 * Single-output today (`index == 0` behaves exactly like fb_snapshot).
 * When multi-output support ships, each index captures that output's
 * back buffer. The test-matrix runner in
 * `scripts/debug/desktop/run-all-desktop-tests.bat` drives every
 * configured output through this call. */
int fb_snapshot_monitor(uint32_t index, void *dest_buf,
                        uint32_t *width, uint32_t *height);

/* ---- Compositor lock ---- */
/* When locked, fb_putchar/fb_draw_char become no-ops.
 * Call fb_lock_compositor() when the WM compositor takes over the screen. */
void fb_lock_compositor(void);
void fb_unlock_compositor(void);

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
