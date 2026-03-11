/* ============================================================================
 * cursor.h — Cursor manager with Adwaita X11 cursor support
 *
 * Loads cursor shapes from Xcur binary files at boot, supports 11 shapes
 * with alpha blending, and provides save/restore rendering for the
 * compositor loop.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* --------------------------------------------------------------------
 * Cursor shape identifiers
 * -------------------------------------------------------------------- */

typedef enum {
    CURSOR_ARROW = 0,       /* default pointer                */
    CURSOR_HAND,            /* clickable element (pointer)    */
    CURSOR_TEXT,            /* text I-beam (xterm)            */
    CURSOR_MOVE,            /* 4-way arrows (fleur)           */
    CURSOR_RESIZE_NS,       /* vertical resize                */
    CURSOR_RESIZE_EW,       /* horizontal resize              */
    CURSOR_RESIZE_NWSE,     /* diagonal NW-SE resize          */
    CURSOR_RESIZE_NESW,     /* diagonal NE-SW resize          */
    CURSOR_WAIT,            /* busy / progress spinner        */
    CURSOR_CROSSHAIR,       /* crosshair / precision select   */
    CURSOR_FORBIDDEN,       /* not-allowed / no-drop          */
    CURSOR_COUNT            /* total number of shapes         */
} cursor_shape_t;

/* Maximum cursor dimension (Adwaita ships up to 96px, we use 32) */
#define CURSOR_MAX_SIZE  48

/* Maximum number of size variants per cursor */
#define CURSOR_MAX_SIZES 8

/* --------------------------------------------------------------------
 * Cursor image — one size variant of a cursor shape
 * -------------------------------------------------------------------- */

typedef struct {
    uint32_t  width;
    uint32_t  height;
    int32_t   hotspot_x;
    int32_t   hotspot_y;
    uint32_t *pixels;       /* BGRA pixel data (w * h uint32_t) */
} cursor_image_t;

/* --------------------------------------------------------------------
 * Cursor sprite — all size variants for one shape
 * -------------------------------------------------------------------- */

typedef struct {
    cursor_image_t images[CURSOR_MAX_SIZES];
    uint8_t        num_sizes;
} cursor_sprite_t;

/* --------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------- */

/* Initialize the cursor manager.
 * Loads Xcur files from C:\Impossible\System\Cursors\.
 * Falls back to an embedded arrow if VFS is unavailable. */
void cursor_init(void);

/* Set the active cursor shape. */
void cursor_set_shape(cursor_shape_t shape);

/* Get the current cursor shape. */
cursor_shape_t cursor_get_shape(void);

/* Get the hotspot offset of the active cursor. */
void cursor_get_hotspot(int32_t *hx, int32_t *hy);

/* Draw the cursor at (x, y), saving pixels underneath.
 * Call after compositing, before fb_swap(). */
void cursor_draw(int32_t x, int32_t y);

/* Restore the framebuffer region under the last drawn cursor.
 * Call before compositing to undo the previous cursor blit. */
void cursor_restore(void);

/* Get the bounding rectangle of the last drawn cursor.
 * Returns 0 if no cursor is visible, 1 otherwise. */
int cursor_get_rect(int32_t *rx, int32_t *ry, uint32_t *rw, uint32_t *rh);
