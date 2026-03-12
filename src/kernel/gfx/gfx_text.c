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
 *
 * ⚠️  ALLOCATION RULES — READ BEFORE MODIFYING ⚠️
 *
 *   The kernel heap is only 2 MiB.  Font files and glyph bitmaps are
 *   LONG-LIVED allocations that persist for the entire kernel lifetime.
 *
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │  FONT FILE DATA  → pmm_alloc_contiguous() (files can be MBs)  │
 *   │  GLYPH BITMAPS   → pmm_alloc_contiguous() (collectively large) │
 *   │  TEMP stb_truetype buffers → kmalloc OK (small, freed quickly) │
 *   │  NEVER kmalloc for anything > 4 KB                             │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 *   Violating this causes SILENT heap exhaustion that breaks unrelated
 *   features (hover detection, VFS nodes, task scheduling).
 *   See rules.md "Known Gotchas" and commit 5ea919b.
 *
 * TODO: Migrate load_ttf_file() and glyph cache to PMM allocation.
 *       Current code uses kmalloc (tech debt from initial implementation).
 * ============================================================================ */

#include "font_mgr.h"
#include "gfx.h"
#include "stb_truetype.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/printk.h"
/* Forward-declare string functions (provided by stb_truetype_impl.c weak symbols) */
typedef unsigned long gfx_size_t;
extern void *memset(void *s, int c, gfx_size_t n);
extern void *memcpy(void *dst, const void *src, gfx_size_t n);
#include "kernel/types.h"
#include "gfx_simd.h"

/* ---- Per-(slot, size) texture atlas storage ---- */

/* Each atlas is 256×256 single-channel (64 KB).
 * stbtt_BakeFontBitmap packs all 95 ASCII glyphs into one bitmap. */
#define GLYPH_ATLAS_W  256
#define GLYPH_ATLAS_H  256
#define GLYPH_ATLAS_BYTES  (GLYPH_ATLAS_W * GLYPH_ATLAS_H)
#define GLYPH_ATLAS_PAGES  ((GLYPH_ATLAS_BYTES + 4095) / 4096)

static glyph_atlas_t glyph_atlases[FONT_MAX_SLOTS][GLYPH_CACHE_SIZES];

/* ---- LRU cache for non-ASCII fallback glyphs ---- */

/* Ring buffer cache for glyphs outside ASCII 32-126 (e.g. é, ñ, Unicode).
 * Avoids per-frame stbtt_GetCodepointBitmap + FPU save/restore overhead.
 * Oversized glyphs (bitmap > LRU_BMP_MAX) bypass the cache. */
#define LRU_SIZE       128
#define LRU_BMP_MAX    512   /* max inline bitmap bytes per entry */

typedef struct lru_glyph {
    uint32_t key;           /* hash of (codepoint, slot, px_size), 0 = empty */
    int16_t  width, height;
    int16_t  xoff, yoff;
    int16_t  advance;
    int16_t  _pad;
    uint8_t  bitmap[LRU_BMP_MAX];
} lru_glyph_t;

static lru_glyph_t *lru_cache;  /* PMM-allocated array of LRU_SIZE entries */
static uint32_t lru_next;        /* ring pointer for next eviction */

/* Hash (codepoint, slot, px_size) into a non-zero key */
static uint32_t lru_key(int codepoint, int slot, int px_size)
{
    uint32_t h = (uint32_t)codepoint * 2654435761u;
    h ^= (uint32_t)slot * 2246822519u;
    h ^= (uint32_t)px_size * 3266489917u;
    if (h == 0) h = 1;  /* 0 = empty sentinel */
    return h;
}

/* Search LRU cache for a matching entry. O(n) linear scan is fine for 128 entries. */
static lru_glyph_t *lru_find(uint32_t key)
{
    uint32_t i;
    if (!lru_cache) return (void *)0;
    for (i = 0; i < LRU_SIZE; i++) {
        if (lru_cache[i].key == key)
            return &lru_cache[i];
    }
    return (void *)0;
}

/* Insert a glyph into the LRU cache (evicts oldest via ring pointer).
 * Returns the entry, or NULL if bitmap too large. */
static lru_glyph_t *lru_insert(uint32_t key, const uint8_t *bmp,
                                 int w, int h, int xoff, int yoff, int advance)
{
    lru_glyph_t *ent;
    uint32_t bmp_bytes = (uint32_t)(w * h);
    if (!lru_cache || bmp_bytes > LRU_BMP_MAX)
        return (void *)0;
    ent = &lru_cache[lru_next % LRU_SIZE];
    lru_next++;
    ent->key     = key;
    ent->width   = (int16_t)w;
    ent->height  = (int16_t)h;
    ent->xoff    = (int16_t)xoff;
    ent->yoff    = (int16_t)yoff;
    ent->advance = (int16_t)advance;
    ent->_pad    = 0;
    memcpy(ent->bitmap, bmp, bmp_bytes);
    return ent;
}

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

/* Pre-rasterize all ASCII glyphs for one (slot, size) pair into a texture atlas.
 * Uses stbtt_BakeFontBitmap() to pack all 95 chars into a single 256×256 bitmap.
 * MUST be called with FPU state already saved. */
static void cache_rasterize_slot_size(int slot, int size_idx)
{
    ttf_internal_t *fi = &ttf_slots[slot];
    int px = cache_sizes[size_idx];
    float scale;
    glyph_atlas_t *atlas = &glyph_atlases[slot][size_idx];
    uintptr_t atlas_phys;
    stbtt_bakedchar chardata[GLYPH_CACHE_COUNT];
    int bake_result;
    int i;

    if (!fi->pub.loaded)
        return;

    scale = stbtt_ScaleForPixelHeight(&fi->info, (float)px);

    /* Store pre-computed scaled ascent */
    cached_ascent[slot][size_idx] = (int32_t)(scale * (float)fi->pub.ascent);

    /* Allocate atlas bitmap via PMM (256×256 = 64 KB, single channel) */
    atlas_phys = pmm_alloc_contiguous(GLYPH_ATLAS_PAGES);
    if (!atlas_phys) {
        printk("[!!] Atlas PMM alloc failed for slot %d size %dpx\n",
               (uint64_t)slot, (uint64_t)px);
        return;
    }
    atlas->pixels = (uint8_t *)atlas_phys;
    atlas->width  = GLYPH_ATLAS_W;
    atlas->height = GLYPH_ATLAS_H;

    /* Clear atlas to all zeros (transparent) */
    memset(atlas->pixels, 0, GLYPH_ATLAS_BYTES);

    /* Bake all 95 ASCII glyphs into the atlas bitmap */
    bake_result = stbtt_BakeFontBitmap(
        fi->pub.ttf_data, 0, (float)px,
        atlas->pixels, GLYPH_ATLAS_W, GLYPH_ATLAS_H,
        GLYPH_CACHE_FIRST, GLYPH_CACHE_COUNT,
        chardata);

    if (bake_result <= 0) {
        printk("[!!] BakeFontBitmap failed for slot %d size %dpx (result=%d)\n",
               (uint64_t)slot, (uint64_t)px, (uint64_t)bake_result);
    }

    /* Convert stbtt_bakedchar results into glyph_entry_t atlas coords.
     * Use proper rounding (not truncation) for float→int to avoid
     * ~1px glyph positioning drift that causes uneven letter spacing. */
    for (i = 0; i < GLYPH_CACHE_COUNT; i++) {
        glyph_entry_t *ge = &glyph_cache[slot][size_idx][i];
        stbtt_bakedchar *bc = &chardata[i];

        ge->atlas_x = bc->x0;
        ge->atlas_y = bc->y0;
        ge->width   = (int16_t)(bc->x1 - bc->x0);
        ge->height  = (int16_t)(bc->y1 - bc->y0);
        /* Round float offsets instead of truncating */
        ge->xoff    = (int16_t)(bc->xoff >= 0 ? bc->xoff + 0.5f : bc->xoff - 0.5f);
        ge->yoff    = (int16_t)(bc->yoff >= 0 ? bc->yoff + 0.5f : bc->yoff - 0.5f);
        /* Use bakedchar's advance for consistency with baked glyph positions */
        ge->advance = (int16_t)(bc->xadvance + 0.5f);
        ge->_pad    = 0;
    }
}

/* ---- Load a TTF file from VFS ---- */

static int load_ttf_file(const char *path, uint8_t **out_data, uint32_t *out_size)
{
    struct vfs_node *f;
    uint32_t size;
    uint8_t *buf;
    uint32_t pages;
    uintptr_t phys;

    f = vfs_open(path, VFS_O_READ);
    if (!f)
        return -1;

    size = f->size;
    if (size == 0 || size > 16 * 1024 * 1024) {  /* Max 16 MB font file */
        vfs_close(f);
        return -1;
    }

    /* Allocate via PMM — font data is read-only after init and can be
     * 50 KB to 3 MB.  NEVER use kmalloc for this (see rules.md). */
    pages = (size + 4095) / 4096;
    phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        printk("[!!] PMM alloc failed for font %s (%u bytes, %u pages)\n",
               path, size, pages);
        vfs_close(f);
        return -1;
    }
    buf = (uint8_t *)phys;  /* identity-mapped */

    {
        int32_t bytes_read = vfs_read(f, 0, size, buf);
        if (bytes_read <= 0) {
            /* PMM pages not freed — acceptable for boot-time assets */
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

    /* Initialize atlas storage (individual atlases allocated per slot/size
     * inside cache_rasterize_slot_size via PMM) */
    memset(glyph_atlases, 0, sizeof(glyph_atlases));

    /* Allocate LRU cache for non-ASCII fallback glyphs */
    {
        uint32_t lru_bytes = LRU_SIZE * sizeof(lru_glyph_t);
        uint32_t lru_pages = (lru_bytes + 4095) / 4096;
        uintptr_t lru_phys = pmm_alloc_contiguous(lru_pages);
        if (lru_phys) {
            lru_cache = (lru_glyph_t *)lru_phys;
            memset(lru_cache, 0, lru_bytes);
            lru_next = 0;
            printk("[OK] LRU glyph cache: %u entries (%u KB, %u pages)\n",
                   (uint64_t)LRU_SIZE,
                   (uint64_t)(lru_bytes / 1024),
                   (uint64_t)lru_pages);
        } else {
            printk("[!!] Failed to allocate LRU glyph cache\n");
            lru_cache = (void *)0;
        }
    }

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
                if (ge->width > 0 && ge->height > 0) {
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

/* ---- Glyph cache helper: blit a cached glyph from atlas onto surface ---- */
/*
 * Optimized path: pre-clip column range per row, write directly to pixel
 * buffer (no per-pixel bounds checks or function call overhead).
 * Fast paths: skip transparent pixels, direct-write opaque pixels.
 * Reads glyph data from atlas bitmap at (atlas_x, atlas_y).
 */
static void blit_cached_glyph(gfx_surface_t *s, const glyph_atlas_t *atlas,
                               const glyph_entry_t *ge,
                               int32_t x, int32_t base_y,
                               uint32_t cr, uint32_t cg, uint32_t cb,
                               gfx_color_t color)
{
    int row;

    if (!atlas || !atlas->pixels || ge->width <= 0 || ge->height <= 0)
        return;

    for (row = 0; row < ge->height; row++) {
        int32_t py = base_y + ge->yoff + row;
        int32_t gx0 = x + ge->xoff;
        int col_start, col_end, col;
        uint32_t *dst_row;
        const uint8_t *src_row;

        if (py < 0 || (uint32_t)py >= s->height) continue;

        /* Pre-clip column range to surface bounds */
        col_start = 0;
        col_end = ge->width;
        if (gx0 < 0)
            col_start = -gx0;
        if (gx0 + col_end > (int32_t)s->width)
            col_end = (int32_t)s->width - gx0;
        if (col_start >= col_end) continue;

        /* Read from atlas at (atlas_x, atlas_y) offset */
        dst_row = s->pixels + (uint32_t)py * s->stride + (uint32_t)(gx0 + col_start);
        src_row = atlas->pixels +
                  ((uint32_t)ge->atlas_y + (uint32_t)row) * atlas->width +
                  (uint32_t)ge->atlas_x + (uint32_t)col_start;

        for (col = 0; col < (col_end - col_start); col++) {
            uint32_t alpha = src_row[col];

            if (alpha == 0) continue;

            if (alpha == 255) {
                dst_row[col] = color;
            } else {
                /* Inline alpha blend — avoids gfx_blend_pixel call overhead */
                uint32_t inv = 255 - alpha;
                uint32_t d = dst_row[col];
                uint32_t dr = (d >> 16) & 0xFF;
                uint32_t dg = (d >>  8) & 0xFF;
                uint32_t db =  d        & 0xFF;
                dst_row[col] = 0xFF000000
                    | (((cr * alpha + dr * inv) >> 8) << 16)
                    | (((cg * alpha + dg * inv) >> 8) <<  8)
                    |  ((cb * alpha + db * inv) >> 8);
            }
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
        const glyph_atlas_t *atlas = &glyph_atlases[slot][size_idx];
        int32_t base_y = y + cached_ascent[slot][size_idx];

        if (atlas->pixels && ge->width > 0 && ge->height > 0) {
            uint32_t cr = GFX_RED(color);
            uint32_t cg = GFX_GREEN(color);
            uint32_t cb = GFX_BLUE(color);
            blit_cached_glyph(s, atlas, ge, x, base_y, cr, cg, cb, color);
        }
        return ge->advance;
    }

    /* Fallback: check LRU cache, then stb_truetype */
    {
        ttf_internal_t *fi = (ttf_internal_t *)f;
        uint32_t cr = GFX_RED(color);
        uint32_t cg = GFX_GREEN(color);
        uint32_t cb = GFX_BLUE(color);
        uint32_t lk = lru_key(codepoint, slot, f->pixel_size);
        lru_glyph_t *lhit = lru_find(lk);

        if (lhit) {
            /* LRU hit — blit directly, no FPU needed */
            int32_t base_y = y + cached_ascent[slot][cache_size_index(f->pixel_size) >= 0
                ? cache_size_index(f->pixel_size) : 0];
            glyph_entry_t tmp_ge;
            glyph_atlas_t tmp_atlas;
            tmp_ge.atlas_x = 0;
            tmp_ge.atlas_y = 0;
            tmp_ge.width  = lhit->width;
            tmp_ge.height = lhit->height;
            tmp_ge.xoff   = lhit->xoff;
            tmp_ge.yoff   = lhit->yoff;
            tmp_atlas.pixels = lhit->bitmap;
            tmp_atlas.width  = (uint16_t)lhit->width;
            tmp_atlas.height = (uint16_t)lhit->height;
            blit_cached_glyph(s, &tmp_atlas, &tmp_ge, x, base_y, cr, cg, cb, color);
            return lhit->advance;
        }

        /* LRU miss — rasterize via stb_truetype */
        {
            int width, height, xoff, yoff;
            int advance, lsb;
            unsigned char *bitmap;
            fxsave_area_t fpu_state __attribute__((aligned(16)));

            simd_save_state(&fpu_state);
            bitmap = stbtt_GetCodepointBitmap(&fi->info, 0, f->scale,
                                              codepoint, &width, &height, &xoff, &yoff);
            stbtt_GetCodepointHMetrics(&fi->info, codepoint, &advance, &lsb);
            simd_restore_state(&fpu_state);

            if (bitmap) {
                int adv_px = (int)(f->scale * (float)advance);
                /* Try to cache in LRU for next time */
                lru_insert(lk, bitmap, width, height, xoff, yoff, adv_px);

                /* Blit the rasterized glyph */
                {
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
                }
                kfree(bitmap);
                return adv_px;
            }
            return (int)(f->scale * (float)advance);
        }
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
        const glyph_atlas_t *atlas = &glyph_atlases[slot][size_idx];
        int32_t base_y = y + cached_ascent[slot][size_idx];
        uint32_t cr = GFX_RED(color);
        uint32_t cg = GFX_GREEN(color);
        uint32_t cb = GFX_BLUE(color);
        fxsave_area_t fpu_state __attribute__((aligned(16)));
        uint8_t fpu_saved = 0;

        for (i = 0; text[i]; i++) {
            int cp = (unsigned char)text[i];

            if (cp >= GLYPH_CACHE_FIRST && cp <= GLYPH_CACHE_LAST) {
                /* Cached glyph — blit from atlas */
                const glyph_entry_t *ge = &glyph_cache[slot][size_idx][cp - GLYPH_CACHE_FIRST];

                if (atlas->pixels && ge->width > 0 && ge->height > 0) {
                    blit_cached_glyph(s, atlas, ge, cursor_x, base_y, cr, cg, cb, color);
                }
                cursor_x += ge->advance;
            } else {
                /* Non-ASCII: check LRU, then fall through to stb_truetype */
                {
                    uint32_t lk = lru_key(cp, slot, f->pixel_size);
                    lru_glyph_t *lhit = lru_find(lk);

                    if (lhit) {
                        /* LRU hit — blit from cache (no FPU needed) */
                        glyph_entry_t tmp_ge;
                        glyph_atlas_t tmp_atlas;
                        tmp_ge.atlas_x = 0;
                        tmp_ge.atlas_y = 0;
                        tmp_ge.width  = lhit->width;
                        tmp_ge.height = lhit->height;
                        tmp_ge.xoff   = lhit->xoff;
                        tmp_ge.yoff   = lhit->yoff;
                        tmp_atlas.pixels = lhit->bitmap;
                        tmp_atlas.width  = (uint16_t)lhit->width;
                        tmp_atlas.height = (uint16_t)lhit->height;
                        blit_cached_glyph(s, &tmp_atlas, &tmp_ge, cursor_x, base_y,
                                          cr, cg, cb, color);
                        cursor_x += lhit->advance;
                    } else {
                        /* LRU miss — rasterize */
                        int width, height, xoff, yoff;
                        int advance, lsb;
                        unsigned char *bitmap;

                        if (!fpu_saved) { simd_save_state(&fpu_state); fpu_saved = 1; }
                        bitmap = stbtt_GetCodepointBitmap(&fi->info, 0, f->scale,
                                                          cp, &width, &height, &xoff, &yoff);
                        stbtt_GetCodepointHMetrics(&fi->info, cp, &advance, &lsb);

                        if (bitmap) {
                            int adv_px = (int)(f->scale * (float)advance);
                            lru_insert(lk, bitmap, width, height, xoff, yoff, adv_px);

                            {
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
                            }
                            kfree(bitmap);
                            cursor_x += adv_px;
                        } else {
                            cursor_x += (int32_t)(f->scale * (float)advance);
                        }
                    }
                }
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

/* ============================================================================
 * Boot splash font API — used before font_mgr_init()
 *
 * These functions use an embedded TTF font (Selawik Regular, 44 KB) and
 * render anti-aliased text directly to the framebuffer via fb_put_pixel().
 * They are called from boot_splash.c which is compiled without SSE2, so
 * all FPU work stays inside this SSE2-compiled translation unit.
 * ============================================================================ */

#include "boot_splash_font.h"
#include "kernel/drivers/framebuffer.h"
#include "gfx_simd.h"

/* Dedicated stbtt_fontinfo for the embedded boot font */
static stbtt_fontinfo boot_stb_info;
static int boot_font_ready = 0;
static float boot_font_scale = 0;
static int boot_font_ascent_px = 0;

int boot_font_init(int pixel_size)
{
    int offset;
    int ascent_raw, descent_raw, linegap_raw;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    simd_save_state(&fpu_state);

    offset = stbtt_GetFontOffsetForIndex(boot_font_data, 0);
    if (offset < 0) {
        simd_restore_state(&fpu_state);
        return -1;
    }

    if (!stbtt_InitFont(&boot_stb_info, boot_font_data, offset)) {
        simd_restore_state(&fpu_state);
        return -1;
    }

    boot_font_scale = stbtt_ScaleForPixelHeight(&boot_stb_info, (float)pixel_size);
    stbtt_GetFontVMetrics(&boot_stb_info, &ascent_raw, &descent_raw, &linegap_raw);
    boot_font_ascent_px = (int)(boot_font_scale * (float)ascent_raw);

    simd_restore_state(&fpu_state);

    boot_font_ready = 1;
    return 0;
}

int boot_font_measure(const char *text)
{
    int total = 0;
    int i;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    if (!boot_font_ready || !text)
        return 0;

    simd_save_state(&fpu_state);

    for (i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&boot_stb_info, (unsigned char)text[i], &advance, &lsb);
        total += (int)(boot_font_scale * (float)advance);

        if (text[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(
                &boot_stb_info, (unsigned char)text[i], (unsigned char)text[i + 1]);
            total += (int)(boot_font_scale * (float)kern);
        }
    }

    simd_restore_state(&fpu_state);
    return total;
}

void boot_font_render(const char *text, int32_t x, int32_t y, uint32_t color)
{
    int i;
    int32_t cursor_x = x;
    uint32_t scr_w, scr_h;
    uint8_t cr, cg, cb;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    if (!boot_font_ready || !text)
        return;

    scr_w = fb_get_width();
    scr_h = fb_get_height();
    cr = (uint8_t)(color >> 16);
    cg = (uint8_t)(color >> 8);
    cb = (uint8_t)(color);

    simd_save_state(&fpu_state);

    for (i = 0; text[i]; i++) {
        int codepoint = (unsigned char)text[i];
        int width, height, xoff, yoff;
        int advance, lsb;
        unsigned char *bitmap;

        bitmap = stbtt_GetCodepointBitmap(&boot_stb_info, 0, boot_font_scale,
                                           codepoint, &width, &height, &xoff, &yoff);
        stbtt_GetCodepointHMetrics(&boot_stb_info, codepoint, &advance, &lsb);

        if (bitmap) {
            int32_t base_y = y + boot_font_ascent_px + yoff;
            int row, col;

            for (row = 0; row < height; row++) {
                int32_t py = base_y + row;
                if (py < 0 || (uint32_t)py >= scr_h) continue;

                for (col = 0; col < width; col++) {
                    int32_t px = cursor_x + xoff + col;
                    uint32_t alpha;
                    if (px < 0 || (uint32_t)px >= scr_w) continue;

                    alpha = bitmap[row * width + col];
                    if (alpha == 0) continue;

                    if (alpha == 255) {
                        fb_put_pixel((uint32_t)px, (uint32_t)py, color);
                    } else {
                        /* Blend onto black background (simple multiply) */
                        uint8_t ob = (uint8_t)((cb * alpha) / 255);
                        uint8_t og = (uint8_t)((cg * alpha) / 255);
                        uint8_t or_ = (uint8_t)((cr * alpha) / 255);
                        fb_put_pixel((uint32_t)px, (uint32_t)py,
                            (uint32_t)ob | ((uint32_t)og << 8) | ((uint32_t)or_ << 16));
                    }
                }
            }
            kfree(bitmap);
        }

        cursor_x += (int32_t)(boot_font_scale * (float)advance);

        if (text[i + 1]) {
            int kern = stbtt_GetCodepointKernAdvance(
                &boot_stb_info, codepoint, (unsigned char)text[i + 1]);
            cursor_x += (int32_t)(boot_font_scale * (float)kern);
        }
    }

    simd_restore_state(&fpu_state);
}
