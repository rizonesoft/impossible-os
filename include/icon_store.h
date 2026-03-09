/* ============================================================================
 * icon_store.h — Hybrid Icon Store API
 *
 * Centralized icon system with two rendering backends:
 *
 *   1. Font-based (monochrome): ~60 system/toolbar/file type icons rendered
 *      from Fluent UI icon fonts (TTF) via stb_truetype. Vector glyphs give
 *      resolution-independent rendering at any size. Icons are tinted with
 *      a foreground color and cached as BGRA bitmaps.
 *
 *   2. IRES-based (color): ~40 desktop/app/folder/file type icons stored
 *      as pre-rendered BGRA bitmaps in an .ires (Icon Resource) file at
 *      multiple sizes (16–256) for full-color detail.
 *
 * Usage:
 *   icon_store_init();
 *   icon_bitmap_t *bmp = icon_get(ICON_FOLDER_CLOSED, 32);
 *   icon_draw(surface, bmp, x, y);
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "gfx.h"

/* ---- Icon font variants ---- */

typedef enum {
    ICON_FONT_FILLED,      /* Solid icons (toolbars, active states) */
    ICON_FONT_REGULAR,     /* Outlined icons (menus, secondary) */
    ICON_FONT_LIGHT,       /* Thin strokes (disabled states, hints) */
    ICON_FONT_RESIZABLE,   /* Optimised for small sizes (16px and below) */
    ICON_FONT_COUNT
} icon_font_variant_t;

/* ---- System icon IDs ---- */

typedef enum {
    /* ---- Monochrome system icons (font-rendered) ---- */

    /* Toolbar / action icons */
    ICON_CUT,
    ICON_COPY,
    ICON_PASTE,
    ICON_UNDO,
    ICON_REDO,
    ICON_SAVE,
    ICON_OPEN,
    ICON_NEW,
    ICON_DELETE,
    ICON_REFRESH,
    ICON_ZOOM_IN,
    ICON_ZOOM_OUT,
    ICON_BOLD,
    ICON_ITALIC,
    ICON_UNDERLINE,
    ICON_ALIGN_LEFT,
    ICON_ALIGN_CENTER,
    ICON_ALIGN_RIGHT,
    ICON_PRINT,
    ICON_HOME,
    ICON_BACK,
    ICON_FORWARD,
    ICON_UP,

    /* System UI icons */
    ICON_SETTINGS,
    ICON_SEARCH,
    ICON_LOCK,
    ICON_USER,
    ICON_POWER,
    ICON_INFO,
    ICON_WARNING,
    ICON_ERROR,
    ICON_QUESTION,
    ICON_CLOSE,
    ICON_MINIMIZE,
    ICON_MAXIMIZE,
    ICON_RESTORE,
    ICON_MENU,
    ICON_CHEVRON_DOWN,
    ICON_CHEVRON_RIGHT,
    ICON_CHEVRON_LEFT,
    ICON_CHEVRON_UP,
    ICON_CHECK,
    ICON_ADD,
    ICON_SUBTRACT,
    ICON_STAR,
    ICON_HEART,
    ICON_SHARE,
    ICON_DOWNLOAD,
    ICON_UPLOAD,
    ICON_CLOCK,
    ICON_CALENDAR,
    ICON_SORT,
    ICON_FILTER,
    ICON_GRID,
    ICON_LIST,
    ICON_LINK,
    ICON_ATTACH,
    ICON_PIN,
    ICON_CLIPBOARD,
    ICON_FULLSCREEN,

    ICON_MONO_COUNT,  /* sentinel: end of monochrome icons */

    /* ---- Color icons (IRES-based) ---- */

    ICON_FOLDER_CLOSED = ICON_MONO_COUNT,
    ICON_FOLDER_OPEN,
    ICON_FILE_DEFAULT,
    ICON_EXE_DEFAULT,
    ICON_DESKTOP_COMPUTER,
    ICON_RECYCLE_BIN_EMPTY,
    ICON_RECYCLE_BIN_FULL,
    ICON_CONTROL_DECK,

    ICON_COLOR_COUNT,  /* sentinel: end of color icons */

    ICON_TOTAL_COUNT = ICON_COLOR_COUNT
} system_icon_t;

/* ---- Icon bitmap (cached rasterized icon) ---- */

typedef struct icon_bitmap {
    uint32_t *pixels;      /* BGRA pixel data (GFX_RGBA format) */
    uint16_t  width;       /* Bitmap width */
    uint16_t  height;      /* Bitmap height */
    uint32_t  alloc_size;  /* Allocation size in bytes */
    uint8_t   from_pmm;    /* 1 = PMM allocation, 0 = kmalloc */
    uint8_t   _pad[3];
} icon_bitmap_t;

/* ---- API ---- */

/* Initialize the icon store: load Fluent icon fonts from
 * C:\Impossible\Fonts\ and color icons from C:\Impossible\System\icons.ires.
 * Call after ttf_mgr_init() and VFS are ready. */
void icon_store_init(void);

/* Get an icon bitmap at the requested pixel size.
 * For monochrome icons: rasterizes from icon font with default theme color.
 * For color icons: returns the closest available IRES size.
 * Returns NULL if the icon is not available.
 * The returned pointer is cached — do NOT free it. */
icon_bitmap_t *icon_get(system_icon_t id, uint32_t size);

/* Get an icon bitmap with a custom foreground color (monochrome icons only).
 * Color icons ignore the tint parameter and return their original colors.
 * Returns NULL if the icon is not available.
 * The returned pointer is cached — do NOT free it. */
icon_bitmap_t *icon_get_colored(system_icon_t id, uint32_t size,
                                 gfx_color_t color);

/* Get an icon bitmap with a specific font variant and color.
 * variant: ICON_FONT_FILLED (toolbars), ICON_FONT_REGULAR (menus),
 *          ICON_FONT_LIGHT (disabled), ICON_FONT_RESIZABLE (small). */
icon_bitmap_t *icon_get_variant(system_icon_t id, uint32_t size,
                                gfx_color_t color,
                                icon_font_variant_t variant);

/* Look up an icon by string name (e.g., "folder_closed", "cut", "save").
 * Returns ICON_TOTAL_COUNT if not found. */
system_icon_t icon_get_by_name(const char *name);

/* Draw an icon onto a GFX surface at (x, y) with alpha blending. */
void icon_draw(gfx_surface_t *s, const icon_bitmap_t *bmp,
               int32_t x, int32_t y);

/* Draw an icon at (x, y) scaled to target_size × target_size.
 * Uses the closest cached size as source, then scales. */
void icon_draw_scaled(gfx_surface_t *s, system_icon_t id,
                       int32_t x, int32_t y, uint32_t target_size);

/* Get the current default icon color (from Codex theme). */
gfx_color_t icon_get_theme_color(void);

/* Set the default icon color for monochrome icons. */
void icon_set_theme_color(gfx_color_t color);

