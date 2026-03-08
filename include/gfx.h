/* ============================================================================
 * gfx.h — 2D Compositing Library: Core Types and Primitives
 *
 * Provides a hardware-independent surface abstraction for 2D rendering.
 * All drawing operations target a `gfx_surface_t` which can represent
 * the framebuffer back buffer, an off-screen sprite, a window buffer, etc.
 *
 * Color format: 0xAARRGGBB (alpha in high byte, blue in low byte)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Color type and macros ---- */

typedef uint32_t gfx_color_t;

/* Construct a color from RGBA components (0–255 each) */
#define GFX_RGBA(r, g, b, a) \
    ((gfx_color_t)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
                   ((uint32_t)(g) << 8)  | ((uint32_t)(b))))

/* Construct an opaque color from RGB */
#define GFX_RGB(r, g, b) GFX_RGBA((r), (g), (b), 0xFF)

/* Extract individual channels */
#define GFX_ALPHA(c)  (((c) >> 24) & 0xFF)
#define GFX_RED(c)    (((c) >> 16) & 0xFF)
#define GFX_GREEN(c)  (((c) >> 8)  & 0xFF)
#define GFX_BLUE(c)   ((c)         & 0xFF)

/* Common colors */
#define GFX_COLOR_TRANSPARENT  GFX_RGBA(0, 0, 0, 0)
#define GFX_COLOR_BLACK        GFX_RGB(0, 0, 0)
#define GFX_COLOR_WHITE        GFX_RGB(255, 255, 255)
#define GFX_COLOR_RED          GFX_RGB(255, 0, 0)
#define GFX_COLOR_GREEN        GFX_RGB(0, 255, 0)
#define GFX_COLOR_BLUE         GFX_RGB(0, 0, 255)

/* ---- Surface ---- */

/* A rectangular pixel buffer for 2D rendering. */
typedef struct gfx_surface {
    uint32_t   *pixels;   /* ARGB pixel data */
    uint32_t    width;    /* surface width in pixels */
    uint32_t    height;   /* surface height in pixels */
    uint32_t    stride;   /* pixels per row (may be > width for alignment) */
} gfx_surface_t;

/* Initialize a surface from an existing pixel buffer */
void gfx_surface_init(gfx_surface_t *s, uint32_t *pixels,
                       uint32_t w, uint32_t h, uint32_t stride);

/* Allocate a new surface (pixels from kmalloc). Returns 0 on success.</a> */
int gfx_surface_create(gfx_surface_t *s, uint32_t w, uint32_t h);

/* Free a surface allocated with gfx_surface_create */
void gfx_surface_destroy(gfx_surface_t *s);

/* Clear the entire surface to a solid color */
void gfx_clear(gfx_surface_t *s, gfx_color_t color);

/* ---- Drawing primitives ---- */

/* Solid filled rectangle */
void gfx_fill_rect(gfx_surface_t *s, int32_t x, int32_t y,
                    uint32_t w, uint32_t h, gfx_color_t color);

/* Outline rectangle with configurable thickness */
void gfx_draw_rect(gfx_surface_t *s, int32_t x, int32_t y,
                    uint32_t w, uint32_t h, uint32_t thickness,
                    gfx_color_t color);

/* Filled rounded rectangle (anti-aliased corners) */
void gfx_fill_rounded_rect(gfx_surface_t *s, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t radius,
                            gfx_color_t color);

/* Outline rounded rectangle */
void gfx_draw_rounded_rect(gfx_surface_t *s, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t radius,
                            uint32_t thickness, gfx_color_t color);

/* Filled circle */
void gfx_fill_circle(gfx_surface_t *s, int32_t cx, int32_t cy,
                      int32_t r, gfx_color_t color);

/* Line (Bresenham with configurable thickness) */
void gfx_draw_line(gfx_surface_t *s, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1, uint32_t thickness,
                    gfx_color_t color);

/* Put a single pixel (with bounds checking) */
void gfx_put_pixel(gfx_surface_t *s, int32_t x, int32_t y, gfx_color_t color);

/* Alpha-blend a pixel onto the surface */
void gfx_blend_pixel(gfx_surface_t *s, int32_t x, int32_t y, gfx_color_t color);

/* ---- Dirty Rectangle Tracker ---- */

#define GFX_MAX_DIRTY  32

typedef struct gfx_dirty_tracker {
    struct {
        int32_t  x, y;
        uint32_t w, h;
    } rects[GFX_MAX_DIRTY];
    uint32_t count;
} gfx_dirty_tracker_t;

/* Reset the dirty tracker */
void gfx_dirty_reset(gfx_dirty_tracker_t *dt);

/* Mark a rectangular region as dirty */
void gfx_dirty_add(gfx_dirty_tracker_t *dt, int32_t x, int32_t y,
                    uint32_t w, uint32_t h);

/* Get the bounding box of all dirty regions.
 * Returns 0 if no dirty regions, 1 if a bounding box was computed. */
int gfx_dirty_bounds(gfx_dirty_tracker_t *dt, int32_t *x, int32_t *y,
                     uint32_t *w, uint32_t *h);
