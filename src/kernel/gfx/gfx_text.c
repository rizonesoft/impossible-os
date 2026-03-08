/* ============================================================================
 * gfx_text.c — TrueType font manager + text rendering via stb_truetype
 *
 * THIS FILE IS COMPILED WITH -msse2 (floating point needed for stb_truetype).
 *
 * Loads TTF fonts from C:\Impossible\Fonts\ at boot via VFS, stores them in
 * font slots, and renders glyphs onto gfx_surface_t using alpha blending.
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
    NULL, NULL, NULL
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

    /* Protect FPU state */
    fxsave_area_t fpu_state __attribute__((aligned(16)));
    simd_save_state(&fpu_state);

    memset(ttf_slots, 0, sizeof(ttf_slots));

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

    ttf_mgr_ready = 1;
    simd_restore_state(&fpu_state);

    printk("[OK] TTF font manager initialized (%d/%d slots loaded)\n",
           (uint64_t)loaded, (uint64_t)FONT_MAX_SLOTS);
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

int ttf_draw_char(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                  int codepoint, gfx_color_t color)
{
    ttf_internal_t *fi = (ttf_internal_t *)f;
    int width, height, xoff, yoff;
    int advance, lsb;
    unsigned char *bitmap;
    int row, col;
    uint32_t cr, cg, cb;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    if (!f || !f->loaded)
        return 0;

    cr = GFX_RED(color);
    cg = GFX_GREEN(color);
    cb = GFX_BLUE(color);

    simd_save_state(&fpu_state);

    /* Get glyph bitmap from stb_truetype */
    bitmap = stbtt_GetCodepointBitmap(&fi->info, 0, f->scale,
                                      codepoint, &width, &height, &xoff, &yoff);

    stbtt_GetCodepointHMetrics(&fi->info, codepoint, &advance, &lsb);

    simd_restore_state(&fpu_state);

    if (bitmap) {
        /* Render the glyph bitmap onto the surface with alpha blending */
        int32_t base_y = y + (int32_t)(f->scale * (float)f->ascent) + yoff;

        for (row = 0; row < height; row++) {
            int32_t py = base_y + row;
            if (py < 0 || (uint32_t)py >= s->height) continue;

            for (col = 0; col < width; col++) {
                int32_t px = x + xoff + col;
                uint32_t alpha;

                if (px < 0 || (uint32_t)px >= s->width) continue;

                alpha = bitmap[row * width + col];
                if (alpha == 0) continue;

                if (alpha == 255) {
                    gfx_put_pixel(s, px, py, color);
                } else {
                    gfx_blend_pixel(s, px, py, GFX_RGBA(cr, cg, cb, alpha));
                }
            }
        }

        /* Free the bitmap (allocated by stb_truetype via kmalloc) */
        kfree(bitmap);
    }

    return (int)(f->scale * (float)advance);
}

int ttf_draw_string(gfx_surface_t *s, ttf_font_t *f, int32_t x, int32_t y,
                    const char *text, gfx_color_t color)
{
    ttf_internal_t *fi = (ttf_internal_t *)f;
    int32_t cursor_x = x;
    int i;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    if (!f || !f->loaded || !text)
        return 0;

    simd_save_state(&fpu_state);

    for (i = 0; text[i]; i++) {
        int advance, lsb;
        int codepoint = (unsigned char)text[i];

        /* Draw the character */
        {
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
        }

        cursor_x += (int32_t)(f->scale * (float)advance);

        /* Apply kerning with next character */
        if (text[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(
                &fi->info, codepoint, (unsigned char)text[i + 1]);
            cursor_x += (int32_t)(f->scale * (float)kern);
        }
    }

    simd_restore_state(&fpu_state);

    return (int)(cursor_x - x);
}

int ttf_measure_width(ttf_font_t *f, const char *text)
{
    ttf_internal_t *fi = (ttf_internal_t *)f;
    int total = 0;
    int i;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    if (!f || !f->loaded || !text)
        return 0;

    simd_save_state(&fpu_state);

    for (i = 0; text[i]; i++) {
        int advance, lsb;
        int codepoint = (unsigned char)text[i];

        stbtt_GetCodepointHMetrics(&fi->info, codepoint, &advance, &lsb);
        total += (int)(f->scale * (float)advance);

        /* Kerning */
        if (text[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(
                &fi->info, codepoint, (unsigned char)text[i + 1]);
            total += (int)(f->scale * (float)kern);
        }
    }

    simd_restore_state(&fpu_state);

    return total;
}

int ttf_line_height(ttf_font_t *f)
{
    if (!f || !f->loaded)
        return 16;  /* fallback */

    return (int)(f->scale * (float)(f->ascent - f->descent + f->line_gap));
}
