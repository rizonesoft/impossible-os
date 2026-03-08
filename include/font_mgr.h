/* ============================================================================
 * font_mgr.h — TrueType Font Manager API
 *
 * Loads TrueType fonts from C:\Impossible\Fonts\ at boot time and provides
 * a simple API for text rendering on gfx_surface_t.
 *
 * Usage:
 *   ttf_mgr_init();                                // load fonts at boot
 *   ttf_font_t *f = ttf_get(FONT_SLOT_UI, 16);    // get 16px UI font
 *   ttf_draw_string(surface, f, 10, 10, "Hello", GFX_COLOR_WHITE);
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "gfx.h"

/* --- Font slots --- */

#define FONT_SLOT_UI      0   /* Primary UI font (segoeui.ttf or default) */
#define FONT_SLOT_MONO    1   /* Monospace font (consola.ttf or default) */
#define FONT_SLOT_TITLE   2   /* Window title font */
#define FONT_SLOT_ICON    3   /* Icon label font */
#define FONT_MAX_SLOTS    8   /* Maximum loaded fonts */

/* Maximum cached pixel sizes per font slot */
#define FONT_MAX_SIZES    4

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

/* Initialize the font manager — load fonts from C:\Impossible\Fonts\ */
void ttf_mgr_init(void);

/* Get a font handle for a given slot and pixel size.
 * Returns NULL if the slot is not loaded. */
ttf_font_t *ttf_get(int slot, int pixel_size);

/* Draw a single character. Returns the advance width in pixels. */
int ttf_draw_char(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                  int codepoint, gfx_color_t color);

/* Draw a string with kerning. Returns total width in pixels. */
int ttf_draw_string(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                    const char *text, gfx_color_t color);

/* Measure the width of a string without drawing. */
int ttf_measure_width(ttf_font_t *f, const char *text);

/* Get the line height (ascent - descent + line_gap, scaled). */
int ttf_line_height(ttf_font_t *f);
