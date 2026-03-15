/* ============================================================================
 * font_mgr.h — TrueType Font Manager API
 *
 * Loads TrueType fonts from C:\Impossible\Fonts\ at boot time and provides
 * a simple API for text rendering on gfx_surface_t.
 *
 * Bundled fonts:
 *   Selawik Regular + Semibold + Bold (MIT) — UI font
 *   Cascadia Code Regular + Bold (OFL 1.1)  — Monospace font
 *   Inter Regular + Bold (OFL 1.1)          — Alternative UI font (fallback)
 *
 * Usage:
 *   ttf_mgr_init();                            // load fonts + build cache
 *   ttf_font_t *f = ttf_get(FONT_UI, 16);     // get 16px UI font
 *   ttf_draw_string(surface, f, 10, 10, "Hello", GFX_COLOR_WHITE);
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "gfx.h"

/* --- Font slots --- */

#define FONT_UI           0   /* UI font — Selawik Regular */
#define FONT_UI_BOLD      1   /* UI font bold — Selawik Semibold */
#define FONT_MONO         2   /* Monospace — Cascadia Code Regular */
#define FONT_MONO_BOLD    3   /* Monospace bold — Cascadia Code Bold */
#define FONT_UI_HEAVY     4   /* UI font heavy — Selawik Bold */
#define FONT_FLUENT_ICONS 5   /* Fluent System Icons — Regular */
#define FONT_MAX_SLOTS    8   /* Maximum loaded fonts */

/* Backwards-compatible aliases */
#define FONT_SLOT_UI      FONT_UI
#define FONT_SLOT_MONO    FONT_MONO
#define FONT_SLOT_TITLE   FONT_UI_BOLD
#define FONT_SLOT_ICON    FONT_UI

/* --- Glyph cache constants --- */

#define GLYPH_CACHE_FIRST   32   /* First cached codepoint (space) */
#define GLYPH_CACHE_LAST   126   /* Last cached codepoint (tilde) */
#define GLYPH_CACHE_COUNT   95   /* LAST - FIRST + 1 */

/* Common pixel sizes to pre-rasterize at boot — 3 covers 90%+ of UI text.
 * Other sizes (12, 24, 32...) go through the LRU cache on first use.
 * Fewer sizes = fewer stbtt_BakeFontBitmap calls at desktop init. */
#define GLYPH_CACHE_SIZES    3
/* Actual sizes: 14, 16, 20 — defined in gfx_text.c */

/* --- Glyph cache entry --- */

typedef struct glyph_entry {
    uint16_t atlas_x;    /* X position in atlas bitmap */
    uint16_t atlas_y;    /* Y position in atlas bitmap */
    int16_t  width;      /* Glyph width in pixels */
    int16_t  height;     /* Glyph height in pixels */
    int16_t  xoff;       /* X offset from pen position */
    int16_t  yoff;       /* Y offset from baseline */
    int16_t  advance;    /* Horizontal advance in pixels (pre-scaled) */
    int16_t  _pad;       /* Padding for alignment */
} glyph_entry_t;

/* --- Per-(slot, size) texture atlas --- */

typedef struct glyph_atlas {
    uint8_t *pixels;     /* Atlas bitmap (single-channel 8bpp, PMM-allocated) */
    uint16_t width;      /* Atlas width in pixels */
    uint16_t height;     /* Atlas height in pixels */
} glyph_atlas_t;

/* --- Font handle --- */

typedef struct ttf_font {
    void    *stb_info;     /* stbtt_fontinfo (opaque to callers) */
    uint8_t *ttf_data;     /* raw TTF file data */
    uint32_t ttf_size;     /* size of TTF data */
    float    scale;        /* current scaling factor */
    int      ascent;       /* unscaled ascent */
    int      descent;      /* unscaled descent */
    int      line_gap;     /* unscaled line gap */
    int      pixel_size;   /* current pixel size */
    int      loaded;       /* 1 if successfully loaded */
} ttf_font_t;

/* --- API --- */

/* Initialize the font manager — load fonts from C:\Impossible\Fonts\
 * and pre-rasterize glyph cache for common sizes. */
void ttf_mgr_init(void);

/* Get a font handle for a given slot and pixel size.
 * Returns NULL if the slot is not loaded. */
ttf_font_t *ttf_get(int slot, int pixel_size);

/* Draw a single character. Returns the advance width in pixels.
 * Uses glyph cache for ASCII 32-126 at cached sizes. */
int ttf_draw_char(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                  int codepoint, gfx_color_t color);

/* Draw a string with kerning. Returns total width in pixels.
 * Uses glyph cache for ASCII 32-126 at cached sizes. */
int ttf_draw_string(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                    const char *text, gfx_color_t color);

/* Measure the width of a string without drawing.
 * Uses cached advance values when available. */
int ttf_measure_width(ttf_font_t *f, const char *text);

/* Get the line height (ascent - descent + line_gap, scaled). */
int ttf_line_height(ttf_font_t *f);
