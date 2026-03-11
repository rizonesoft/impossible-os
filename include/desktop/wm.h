/* ============================================================================
 * wm.h — Stacking window manager
 *
 * Manages overlapping windows with per-window framebuffers, title bars,
 * decorations, z-order stacking, dragging, and painter's-algorithm
 * compositing onto the screen framebuffer.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Constants ---- */

#define WM_MAX_WINDOWS     32
#define WM_TITLE_MAX       64
#define WM_TITLEBAR_HEIGHT 32
#define WM_BORDER_WIDTH    1
#define WM_CORNER_RADIUS   6
#define WM_BTN_WIDTH       46     /* Windows 11 caption button width */
#define WM_BTN_HEIGHT      32     /* same as title bar */
#define WM_RESIZE_MARGIN   5      /* grab zone for edge/corner resize */

/* Window flags */
#define WM_FLAG_VISIBLE    0x01
#define WM_FLAG_DECORATED  0x02    /* has title bar + border */
#define WM_FLAG_MOVABLE    0x04
#define WM_FLAG_RESIZABLE  0x08
#define WM_FLAG_FOCUSED    0x10
#define WM_FLAG_DIALOG     0x20    /* dialog: close-only, cached acrylic bg */

#define WM_DEFAULT_FLAGS   (WM_FLAG_VISIBLE | WM_FLAG_DECORATED | \
                            WM_FLAG_MOVABLE | WM_FLAG_RESIZABLE)

#define WM_DIALOG_FLAGS    (WM_FLAG_VISIBLE | WM_FLAG_DECORATED | \
                            WM_FLAG_MOVABLE | WM_FLAG_DIALOG)

/* ---- Color palette for decorations ---- */

/* Windows 11 Dark Theme — neutral grays */
#define WM_COLOR_TITLEBAR_ACTIVE   0xFF202020   /* active title bar          */
#define WM_COLOR_TITLEBAR_INACTIVE 0xFF2B2B2B   /* inactive title bar        */
#define WM_COLOR_TITLE_TEXT        0xFFFFFFFF   /* active title text         */
#define WM_COLOR_TITLE_INACTIVE    0xFF999999   /* inactive title text       */
#define WM_COLOR_BORDER_ACTIVE     0xFF3A3A3A   /* subtle dark outline       */
#define WM_COLOR_BORDER_INACTIVE   0xFF333333   /* inactive border           */
#define WM_COLOR_CLOSE_HOVER_BG    0xFFC42B1C   /* Win11 close hover red     */
#define WM_COLOR_BTN_HOVER_BG      0x30FFFFFF   /* subtle white glow         */
#define WM_COLOR_BTN_GLYPH         0xFFFFFFFF   /* button glyph color        */
#define WM_COLOR_BTN_GLYPH_DIM     0xFF999999   /* inactive glyph            */
#define WM_COLOR_CLIENT_BG         0xFF202020   /* client area background    */

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

    /* Button hover state (for Windows 11 highlight-on-hover) */
    uint8_t  close_hover;
    uint8_t  min_hover;
    uint8_t  max_hover;

    /* Dialog acrylic cache (pre-blurred wallpaper snapshot) */
    uint32_t *acrylic_cache;        /* PMM buffer: blurred wallpaper */
    uint32_t  acrylic_w, acrylic_h; /* cached texture dimensions */
    uint8_t   acrylic_dirty;        /* 1 = needs re-blur */

    /* Mica title bar: cached flat tint color from wallpaper */
    uint32_t  mica_color;           /* 0 = not computed yet */
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

/* Force a redraw on the next wm_composite() call. */
void wm_mark_dirty(void);

/* Check if a redraw is pending (content changed). */
int wm_needs_redraw(void);

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

/* Determine cursor shape based on what's under the pointer.
 * Returns the appropriate cursor_shape_t for resize edges, close button, etc. */
#include "cursor.h"
cursor_shape_t wm_get_cursor_context(int32_t mx, int32_t my);

/* ---- Drag dirty-rect API ---- */

/* Returns 1 if a window is currently being dragged. */
int wm_is_dragging(void);

/* Get the dirty rect (union of old + new window positions) for the current drag.
 * Returns 1 and fills out params if drag is active, 0 otherwise. */
int wm_get_drag_dirty_rect(int32_t *out_x, int32_t *out_y,
                            uint32_t *out_w, uint32_t *out_h);
