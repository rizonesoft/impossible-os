/* ============================================================================
 * wm.h — Stacking window manager
 *
 * Manages overlapping windows with per-window framebuffers, title bars,
 * decorations, z-order stacking, dragging, and painter's-algorithm
 * compositing onto the screen framebuffer.
 * ============================================================================ */

#pragma once

#include "types.h"

/* ---- Constants ---- */

#define WM_MAX_WINDOWS     32
#define WM_TITLE_MAX       64
#define WM_TITLEBAR_HEIGHT 24
#define WM_BORDER_WIDTH    1

/* Window flags */
#define WM_FLAG_VISIBLE    0x01
#define WM_FLAG_DECORATED  0x02    /* has title bar + border */
#define WM_FLAG_MOVABLE    0x04
#define WM_FLAG_RESIZABLE  0x08
#define WM_FLAG_FOCUSED    0x10

#define WM_DEFAULT_FLAGS   (WM_FLAG_VISIBLE | WM_FLAG_DECORATED | \
                            WM_FLAG_MOVABLE | WM_FLAG_RESIZABLE)

/* ---- Color palette for decorations ---- */

#define WM_COLOR_TITLEBAR_ACTIVE   0x003A3A5C
#define WM_COLOR_TITLEBAR_INACTIVE 0x002A2A3C
#define WM_COLOR_TITLE_TEXT        0x00E0E0E0
#define WM_COLOR_BORDER            0x00505070
#define WM_COLOR_CLOSE_BTN         0x00FF5555
#define WM_COLOR_CLOSE_HOVER       0x00FF7777
#define WM_COLOR_MIN_BTN           0x00FFAA33
#define WM_COLOR_MAX_BTN           0x0055CC55
#define WM_COLOR_CLIENT_BG         0x001E1E2E

/* ---- Window struct ---- */

struct wm_window {
    int32_t  x, y;                  /* position (top-left, outer) */
    uint32_t width, height;         /* client area size (pixels) */
    uint32_t flags;
    char     title[WM_TITLE_MAX];
    uint32_t *framebuffer;          /* per-window pixel buffer (client area) */
    uint32_t fb_pitch;              /* pixels per row in framebuffer */
    int32_t  z_order;               /* higher = on top */
    uint8_t  active;                /* 1 = in use */

    /* Internal drag state */
    uint8_t  dragging;
    int32_t  drag_offset_x;
    int32_t  drag_offset_y;
};

/* ---- Lifecycle ---- */

/* Initialize the window manager (call after fb_init + heap_init) */
void wm_init(void);

/* ---- Window management ---- */

/* Create a window. Returns a handle (index) or -1 on failure. */
int wm_create_window(const char *title, int32_t x, int32_t y,
                     uint32_t width, uint32_t height, uint32_t flags);

/* Destroy a window and free its buffer. */
void wm_destroy_window(int handle);

/* ---- Manipulation ---- */

/* Move a window to a new position. */
void wm_move_window(int handle, int32_t x, int32_t y);

/* Resize a window (reallocates the framebuffer). */
void wm_resize_window(int handle, uint32_t width, uint32_t height);

/* Bring a window to the front. */
void wm_raise_window(int handle);

/* Set focus to a window. */
void wm_focus_window(int handle);

/* ---- Rendering ---- */

/* Composite all windows onto the screen framebuffer (painter's algorithm).
 * Call this once per frame. The caller should call fb_swap() afterward. */
void wm_composite(void);

/* Force a full redraw on the next wm_composite() call. */
void wm_mark_dirty(void);

/* Get a window's client-area framebuffer for drawing into. */
uint32_t *wm_get_framebuffer(int handle);

/* Get the client area dimensions. */
uint32_t wm_get_client_width(int handle);
uint32_t wm_get_client_height(int handle);

/* Write a pixel to a window's client-area framebuffer. */
void wm_put_pixel(int handle, uint32_t x, uint32_t y, uint32_t color);

/* Fill a rectangle in a window's client area. */
void wm_fill_rect(int handle, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t color);

/* ---- Input dispatching ---- */

/* Process mouse input — handles dragging, focus, button clicks.
 * Call once per frame with current mouse state. */
void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons);

/* Find which window is under the given screen coordinate.
 * Returns handle or -1 if none. */
int wm_window_at(int32_t x, int32_t y);
