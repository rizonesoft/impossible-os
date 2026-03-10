/* ============================================================================
 * gfx_text.c — TrueType font manager + glyph cache + text rendering
 *
 * THIS FILE IS COMPILED WITH -msse2 (floating point needed for stb_truetype).
 *
 * Loads TTF fonts from C:\Impossible\Fonts\ at boot via VFS, pre-rasterizes
 * ASCII 32-126 at common pixel sizes into a glyph cache, and renders text
 * onto gfx_surface_t using alpha blending.
 *
 * Cache layout:  glyph_cache[slot][size_index][codepoint - 32]
 *   - 5 active slots × 5 sizes × 95 chars = 2375 cached glyphs
 *   - Each entry stores a persistent bitmap + metrics (no per-frame alloc)
 *   - Uncached glyphs (non-ASCII or unusual sizes) fall through to stb_truetype
 * ============================================================================ */

#include "font_mgr.h"
#include "gfx.h"
#include "stb_truetype.h"
#include "kernel/mm/heap.h"
#include "kernel/fs/vfs.h"
#include "kernel/printk.h"
/* Forward-declare string functions (provided by stb_truetype_impl.c weak symbols) */
typedef unsigned long gfx_size_t;
extern void *memset(void *s, int c, gfx_size_t n);
extern void *memcpy(void *dst, const void *src, gfx_size_t n);
#include "kernel/types.h"
#include "gfx_simd.h"

/* ---- Internal state ---- */

/* We store stbtt_fontinfo inline (it's a small struct) */
typedef struct ttf_internal {
    ttf_font_t      pub;       /* public handle */
    stbtt_fontinfo  info;      /* stb_truetype font info */
} ttf_internal_t;

static ttf_internal_t ttf_slots[FONT_MAX_SLOTS];
static int ttf_mgr_ready = 0;

/* Font file mapping: slot → filename on disk
 * Primary:  Selawik (UI), Cascadia Code (mono)
 * Fallback: Inter (UI alternative), Selawik (mono fallback) */
static const char *ttf_filenames[FONT_MAX_SLOTS] = {
    "selawk.ttf",              /* FONT_UI — Selawik Regular */
    "selawksb.ttf",            /* FONT_UI_BOLD — Selawik Semibold */
    "CascadiaCode-Regular.ttf",/* FONT_MONO — Cascadia Code Regular */
    "CascadiaCode-Bold.ttf",   /* FONT_MONO_BOLD — Cascadia Code Bold */
    "selawkb.ttf",             /* FONT_UI_HEAVY — Selawik Bold */
    NULL,                      /* FONT_FLUENT_ICONS — loaded via PMM later */
    NULL, NULL
};

/* Fallback filenames if primary not found */
static const char *ttf_fallbacks[FONT_MAX_SLOTS] = {
    "Inter-Regular.ttf",   /* fallback UI */
    "Inter-Bold.ttf",      /* fallback UI bold */
    "selawk.ttf",          /* fallback mono → UI regular */
    "selawksb.ttf",        /* fallback mono bold → UI semibold */
    "Inter-Bold.ttf",      /* fallback UI heavy → Inter bold */
    NULL, NULL, NULL
};

/* ---- Glyph cache ---- */

/* Pixel sizes to cache at boot */
static const int cache_sizes[GLYPH_CACHE_SIZES] = { 12, 14, 16, 20, 24 };

/* The cache: [slot][size_index][glyph_index] */
static glyph_entry_t glyph_cache[FONT_MAX_SLOTS][GLYPH_CACHE_SIZES][GLYPH_CACHE_COUNT];

/* Pre-computed scaled ascent for each (slot, size_index) — avoids FPU at draw time */
static int32_t cached_ascent[FONT_MAX_SLOTS][GLYPH_CACHE_SIZES];

/* Map a pixel size to a cache size index, or -1 if not cached */
static int cache_size_index(int pixel_size)
{
    int i;
    for (i = 0; i < GLYPH_CACHE_SIZES; i++) {
        if (cache_sizes[i] == pixel_size)
            return i;
    }
    return -1;
}

/* Pre-rasterize all ASCII glyphs for one (slot, size) pair.
 * MUST be called with FPU state already saved. */
static void cache_rasterize_slot_size(int slot, int size_idx)
{
    ttf_internal_t *fi = &ttf_slots[slot];
    int px = cache_sizes[size_idx];
    float scale;
    int cp;

    if (!fi->pub.loaded)
        return;

    scale = stbtt_ScaleForPixelHeight(&fi->info, (float)px);

    /* Store pre-computed scaled ascent */
    cached_ascent[slot][size_idx] = (int32_t)(scale * (float)fi->pub.ascent);

    for (cp = GLYPH_CACHE_FIRST; cp <= GLYPH_CACHE_LAST; cp++) {
        int gi = cp - GLYPH_CACHE_FIRST;
        glyph_entry_t *ge = &glyph_cache[slot][size_idx][gi];
        int width, height, xoff, yoff;
        int advance, lsb;
        unsigned char *bmp;

        /* Rasterize glyph */
        bmp = stbtt_GetCodepointBitmap(&fi->info, 0, scale,
                                       cp, &width, &height, &xoff, &yoff);

        stbtt_GetCodepointHMetrics(&fi->info, cp, &advance, &lsb);

        if (bmp && width > 0 && height > 0) {
            /* Allocate persistent copy (stb_truetype bitmap must be freed) */
            ge->bitmap = (uint8_t *)kmalloc((uint32_t)(width * height));
            if (ge->bitmap) {
                memcpy(ge->bitmap, bmp, (gfx_size_t)(width * height));
            }
            kfree(bmp);
        } else {
            ge->bitmap = (void *)0;
            if (bmp) kfree(bmp);
        }

        ge->width   = (int16_t)width;
        ge->height  = (int16_t)height;
        ge->xoff    = (int16_t)xoff;
        ge->yoff    = (int16_t)yoff;
        ge->advance = (int16_t)(scale * (float)advance);
        ge->_pad    = 0;
    }
}

/* ---- Load a TTF file from VFS ---- */

static int load_ttf_file(const char *path, uint8_t **out_data, uint32_t *out_size)
{
    struct vfs_node *f;
    uint32_t size;
    uint8_t *buf;

    f = vfs_open(path, VFS_O_READ);
    if (!f)
        return -1;

    size = f->size;
    if (size == 0 || size > 16 * 1024 * 1024) {  /* Max 16 MB font file */
        vfs_close(f);
        return -1;
    }

    buf = (uint8_t *)kmalloc(size);
    if (!buf) {
        vfs_close(f);
        return -1;
    }

    {
        int32_t bytes_read = vfs_read(f, 0, size, buf);
        if (bytes_read <= 0) {
            kfree(buf);
            vfs_close(f);
            return -1;
        }
    }

    vfs_close(f);
    *out_data = buf;
    *out_size = size;
    return 0;
}

/* ---- Try to load a font into a slot ---- */

static int load_ttf_slot(int slot, const char *filename)
{
    char path[128];
    ttf_internal_t *fi = &ttf_slots[slot];
    uint8_t *data;
    uint32_t size;
    int offset;
    const char *prefix = "C:\\Impossible\\Fonts\\";
    int pi = 0;
    int fi2 = 0;

    /* Build path: "C:\Impossible\Fonts\<filename>" */
    while (prefix[pi] && pi < 120) {
        path[pi] = prefix[pi];
        pi++;
    }
    while (filename[fi2] && pi < 127) {
        path[pi++] = filename[fi2++];
    }
    path[pi] = '\0';

    if (load_ttf_file(path, &data, &size) != 0)
        return -1;

    /* Find the font offset (usually 0 for single-font files) */
    offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0) {
        kfree(data);
        return -1;
    }

    if (!stbtt_InitFont(&fi->info, data, offset)) {
        kfree(data);
        return -1;
    }

    fi->pub.stb_info  = &fi->info;
    fi->pub.ttf_data  = data;
    fi->pub.ttf_size  = size;
    fi->pub.loaded    = 1;

    /* Get font metrics */
    stbtt_GetFontVMetrics(&fi->info,
        &fi->pub.ascent, &fi->pub.descent, &fi->pub.line_gap);

    /* Default scale for 16px */
    fi->pub.pixel_size = 16;
    fi->pub.scale = stbtt_ScaleForPixelHeight(&fi->info, 16.0f);

    return 0;
}

/* ---- Public API ---- */

void ttf_mgr_init(void)
{
    int slot;
    int loaded = 0;
    int cached_glyphs = 0;
    uint64_t cache_bytes = 0;

    /* Protect FPU state */
    fxsave_area_t fpu_state __attribute__((aligned(16)));
    simd_save_state(&fpu_state);

    memset(ttf_slots, 0, sizeof(ttf_slots));
    memset(glyph_cache, 0, sizeof(glyph_cache));

    for (slot = 0; slot < FONT_MAX_SLOTS; slot++) {
        if (!ttf_filenames[slot])
            continue;

        /* Try primary filename */
        if (load_ttf_slot(slot, ttf_filenames[slot]) == 0) {
            printk("[OK] TTF slot %d loaded: %s\n",
                   (uint64_t)slot, (uint64_t)(uintptr_t)ttf_filenames[slot]);
            loaded++;
            continue;
        }

        /* Try fallback */
        if (ttf_fallbacks[slot] &&
            load_ttf_slot(slot, ttf_fallbacks[slot]) == 0) {
            printk("[OK] TTF slot %d loaded: %s (fallback)\n",
                   (uint64_t)slot, (uint64_t)(uintptr_t)ttf_fallbacks[slot]);
            loaded++;
            continue;
        }

        printk("[--] TTF slot %d: no font found\n", (uint64_t)slot);
    }

    /* Build glyph cache for all loaded slots at all cached sizes */
    for (slot = 0; slot < FONT_MAX_SLOTS; slot++) {
        int si;
        if (!ttf_slots[slot].pub.loaded)
            continue;
        for (si = 0; si < GLYPH_CACHE_SIZES; si++) {
            int gi;
            cache_rasterize_slot_size(slot, si);
            /* Count cached glyphs and total bitmap bytes */
            for (gi = 0; gi < GLYPH_CACHE_COUNT; gi++) {
                glyph_entry_t *ge = &glyph_cache[slot][si][gi];
                if (ge->bitmap) {
                    cached_glyphs++;
                    cache_bytes += (uint64_t)(ge->width * ge->height);
                }
            }
        }
    }

    ttf_mgr_ready = 1;
    simd_restore_state(&fpu_state);

    printk("[OK] TTF font manager initialized (%d/%d slots loaded)\n",
           (uint64_t)loaded, (uint64_t)FONT_MAX_SLOTS);
    printk("[OK] Glyph cache: %d glyphs, %d KB bitmap data\n",
           (uint64_t)cached_glyphs, (uint64_t)((cache_bytes + 1023) / 1024));
}

ttf_font_t *ttf_get(int slot, int pixel_size)
{
    ttf_internal_t *fi;

    if (!ttf_mgr_ready || slot < 0 || slot >= FONT_MAX_SLOTS)
        return (void *)0;

    fi = &ttf_slots[slot];
    if (!fi->pub.loaded)
        return (void *)0;

    /* Rescale if pixel size changed */
    if (fi->pub.pixel_size != pixel_size) {
        fxsave_area_t fpu_state __attribute__((aligned(16)));
        simd_save_state(&fpu_state);

        fi->pub.pixel_size = pixel_size;
        fi->pub.scale = stbtt_ScaleForPixelHeight(&fi->info, (float)pixel_size);

        simd_restore_state(&fpu_state);
    }

    return &fi->pub;
}

/* ---- Glyph cache helper: blit a cached glyph onto surface ---- */

static void blit_cached_glyph(gfx_surface_t *s, const glyph_entry_t *ge,
                               int32_t x, int32_t base_y,
                               uint32_t cr, uint32_t cg, uint32_t cb,
                               gfx_color_t color)
{
    int row, col;
    int32_t py, px;

    for (row = 0; row < ge->height; row++) {
        py = base_y + ge->yoff + row;
        if (py < 0 || (uint32_t)py >= s->height) continue;

        for (col = 0; col < ge->width; col++) {
            uint32_t alpha;
            px = x + ge->xoff + col;
            if (px < 0 || (uint32_t)px >= s->width) continue;

            alpha = ge->bitmap[row * ge->width + col];
            if (alpha == 0) continue;

            if (alpha == 255)
                gfx_put_pixel(s, px, py, color);
            else
                gfx_blend_pixel(s, px, py, GFX_RGBA(cr, cg, cb, alpha));
        }
    }
}

/* ---- Drawing functions ---- */

int ttf_draw_char(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                  int codepoint, gfx_color_t color)
{
    /* Determine which slot this font handle belongs to */
    int slot = (int)((ttf_internal_t *)f - ttf_slots);
    int size_idx;

    if (!f || !f->loaded)
        return 0;

    /* Try glyph cache first */
    size_idx = cache_size_index(f->pixel_size);
    if (size_idx >= 0 && codepoint >= GLYPH_CACHE_FIRST && codepoint <= GLYPH_CACHE_LAST &&
        slot >= 0 && slot < FONT_MAX_SLOTS) {
        int gi = codepoint - GLYPH_CACHE_FIRST;
        const glyph_entry_t *ge = &glyph_cache[slot][size_idx][gi];
        int32_t base_y = y + cached_ascent[slot][size_idx];

        if (ge->bitmap) {
            uint32_t cr = GFX_RED(color);
            uint32_t cg = GFX_GREEN(color);
            uint32_t cb = GFX_BLUE(color);
            blit_cached_glyph(s, ge, x, base_y, cr, cg, cb, color);
        }
        return ge->advance;
    }

    /* Fallback: uncached path via stb_truetype */
    {
        ttf_internal_t *fi = (ttf_internal_t *)f;
        int width, height, xoff, yoff;
        int advance, lsb;
        unsigned char *bitmap;
        fxsave_area_t fpu_state __attribute__((aligned(16)));

        uint32_t cr = GFX_RED(color);
        uint32_t cg = GFX_GREEN(color);
        uint32_t cb = GFX_BLUE(color);

        simd_save_state(&fpu_state);

        bitmap = stbtt_GetCodepointBitmap(&fi->info, 0, f->scale,
                                          codepoint, &width, &height, &xoff, &yoff);
        stbtt_GetCodepointHMetrics(&fi->info, codepoint, &advance, &lsb);

        simd_restore_state(&fpu_state);

        if (bitmap) {
            int32_t base_y = y + (int32_t)(f->scale * (float)f->ascent) + yoff;
            int row, col;

            for (row = 0; row < height; row++) {
                int32_t py = base_y + row;
                if (py < 0 || (uint32_t)py >= s->height) continue;
                for (col = 0; col < width; col++) {
                    int32_t px = x + xoff + col;
                    uint32_t alpha;
                    if (px < 0 || (uint32_t)px >= s->width) continue;
                    alpha = bitmap[row * width + col];
                    if (alpha == 0) continue;
                    if (alpha == 255)
                        gfx_put_pixel(s, px, py, color);
                    else
                        gfx_blend_pixel(s, px, py, GFX_RGBA(cr, cg, cb, alpha));
                }
            }
            kfree(bitmap);
        }

        return (int)(f->scale * (float)advance);
    }
}

int ttf_draw_string(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                    const char *text, gfx_color_t color)
{
    int slot = (int)((ttf_internal_t *)f - ttf_slots);
    int size_idx;
    int32_t cursor_x = x;
    int i;

    if (!f || !f->loaded || !text)
        return 0;

    size_idx = cache_size_index(f->pixel_size);

    /* Fast path: all ASCII at a cached size — single FPU bracket for kerning */
    if (size_idx >= 0 && slot >= 0 && slot < FONT_MAX_SLOTS) {
        ttf_internal_t *fi = (ttf_internal_t *)f;
        int32_t base_y = y + cached_ascent[slot][size_idx];
        uint32_t cr = GFX_RED(color);
        uint32_t cg = GFX_GREEN(color);
        uint32_t cb = GFX_BLUE(color);
        fxsave_area_t fpu_state __attribute__((aligned(16)));
        uint8_t fpu_saved = 0;

        for (i = 0; text[i]; i++) {
            int cp = (unsigned char)text[i];

            if (cp >= GLYPH_CACHE_FIRST && cp <= GLYPH_CACHE_LAST) {
                /* Cached glyph — no stb_truetype call needed */
                const glyph_entry_t *ge = &glyph_cache[slot][size_idx][cp - GLYPH_CACHE_FIRST];

                if (ge->bitmap) {
                    blit_cached_glyph(s, ge, cursor_x, base_y, cr, cg, cb, color);
                }
                cursor_x += ge->advance;
            } else {
                /* Non-ASCII: fall through to stb_truetype */
                int width, height, xoff, yoff;
                int advance, lsb;
                unsigned char *bitmap;

                if (!fpu_saved) { simd_save_state(&fpu_state); fpu_saved = 1; }
                bitmap = stbtt_GetCodepointBitmap(&fi->info, 0, f->scale,
                                                  cp, &width, &height, &xoff, &yoff);
                stbtt_GetCodepointHMetrics(&fi->info, cp, &advance, &lsb);

                if (bitmap) {
                    int32_t fb_y = y + (int32_t)(f->scale * (float)f->ascent) + yoff;
                    int row, col;
                    for (row = 0; row < height; row++) {
                        int32_t py = fb_y + row;
                        if (py < 0 || (uint32_t)py >= s->height) continue;
                        for (col = 0; col < width; col++) {
                            int32_t px = cursor_x + xoff + col;
                            uint32_t alpha;
                            if (px < 0 || (uint32_t)px >= s->width) continue;
                            alpha = bitmap[row * width + col];
                            if (alpha == 0) continue;
                            if (alpha == 255)
                                gfx_put_pixel(s, px, py, color);
                            else
                                gfx_blend_pixel(s, px, py, GFX_RGBA(cr, cg, cb, alpha));
                        }
                    }
                    kfree(bitmap);
                }
                cursor_x += (int32_t)(f->scale * (float)advance);
            }

            /* Kerning with next character */
            if (text[i + 1]) {
                if (!fpu_saved) { simd_save_state(&fpu_state); fpu_saved = 1; }
                {
                    int kern = stbtt_GetCodepointKernAdvance(
                        &fi->info, cp, (unsigned char)text[i + 1]);
                    cursor_x += (int32_t)(f->scale * (float)kern);
                }
            }
        }

        if (fpu_saved)
            simd_restore_state(&fpu_state);

        return (int)(cursor_x - x);
    }

    /* Slow path: uncached size — full stb_truetype rendering */
    {
        ttf_internal_t *fi = (ttf_internal_t *)f;
        fxsave_area_t fpu_state __attribute__((aligned(16)));

        simd_save_state(&fpu_state);

        for (i = 0; text[i]; i++) {
            int advance, lsb;
            int codepoint = (unsigned char)text[i];
            int width, height, xoff, yoff;
            unsigned char *bitmap = stbtt_GetCodepointBitmap(
                &fi->info, 0, f->scale, codepoint, &width, &height, &xoff, &yoff);

            stbtt_GetCodepointHMetrics(&fi->info, codepoint, &advance, &lsb);

            if (bitmap) {
                int32_t base_y = y + (int32_t)(f->scale * (float)f->ascent) + yoff;
                int row, col;
                uint32_t cr = GFX_RED(color);
                uint32_t cg = GFX_GREEN(color);
                uint32_t cb = GFX_BLUE(color);

                for (row = 0; row < height; row++) {
                    int32_t py = base_y + row;
                    if (py < 0 || (uint32_t)py >= s->height) continue;
                    for (col = 0; col < width; col++) {
                        int32_t px = cursor_x + xoff + col;
                        uint32_t alpha;
                        if (px < 0 || (uint32_t)px >= s->width) continue;
                        alpha = bitmap[row * width + col];
                        if (alpha == 0) continue;
                        if (alpha == 255)
                            gfx_put_pixel(s, px, py, color);
                        else
                            gfx_blend_pixel(s, px, py, GFX_RGBA(cr, cg, cb, alpha));
                    }
                }
                kfree(bitmap);
            }

            cursor_x += (int32_t)(f->scale * (float)advance);

            if (text[i + 1]) {
                int kern = stbtt_GetCodepointKernAdvance(
                    &fi->info, codepoint, (unsigned char)text[i + 1]);
                cursor_x += (int32_t)(f->scale * (float)kern);
            }
        }

        simd_restore_state(&fpu_state);

        return (int)(cursor_x - x);
    }
}

int ttf_measure_width(ttf_font_t *f, const char *text)
{
    int slot = (int)((ttf_internal_t *)f - ttf_slots);
    int size_idx;
    int total = 0;
    int i;

    if (!f || !f->loaded || !text)
        return 0;

    size_idx = cache_size_index(f->pixel_size);

    /* Fast path: use cached advance values */
    if (size_idx >= 0 && slot >= 0 && slot < FONT_MAX_SLOTS) {
        ttf_internal_t *fi = (ttf_internal_t *)f;

        for (i = 0; text[i]; i++) {
            int cp = (unsigned char)text[i];

            if (cp >= GLYPH_CACHE_FIRST && cp <= GLYPH_CACHE_LAST) {
                total += glyph_cache[slot][size_idx][cp - GLYPH_CACHE_FIRST].advance;
            } else {
                /* Non-ASCII: need stb_truetype */
                fxsave_area_t fpu_state __attribute__((aligned(16)));
                int advance, lsb;
                simd_save_state(&fpu_state);
                stbtt_GetCodepointHMetrics(&fi->info, cp, &advance, &lsb);
                total += (int)(f->scale * (float)advance);
                simd_restore_state(&fpu_state);
            }

            /* Kerning */
            if (text[i + 1]) {
                fxsave_area_t fpu_state __attribute__((aligned(16)));
                simd_save_state(&fpu_state);
                {
                    int kern = stbtt_GetCodepointKernAdvance(
                        &fi->info, cp, (unsigned char)text[i + 1]);
                    total += (int)(f->scale * (float)kern);
                }
                simd_restore_state(&fpu_state);
            }
        }

        return total;
    }

    /* Slow path: uncached */
    {
        ttf_internal_t *fi = (ttf_internal_t *)f;
        fxsave_area_t fpu_state __attribute__((aligned(16)));

        simd_save_state(&fpu_state);

        for (i = 0; text[i]; i++) {
            int advance, lsb;
            int codepoint = (unsigned char)text[i];

            stbtt_GetCodepointHMetrics(&fi->info, codepoint, &advance, &lsb);
            total += (int)(f->scale * (float)advance);

            if (text[i + 1]) {
                int kern = stbtt_GetCodepointKernAdvance(
                    &fi->info, codepoint, (unsigned char)text[i + 1]);
                total += (int)(f->scale * (float)kern);
            }
        }

        simd_restore_state(&fpu_state);

        return total;
    }
}

int ttf_line_height(ttf_font_t *f)
{
    if (!f || !f->loaded)
        return 16;  /* fallback */

    return (int)(f->scale * (float)(f->ascent - f->descent + f->line_gap));
}
