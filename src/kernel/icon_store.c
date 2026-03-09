/* ============================================================================
 * icon_store.c — Hybrid Icon Store (font glyphs + IRES color icons)
 *
 * THIS FILE IS COMPILED WITH -msse2 (stb_truetype needs floating point).
 *
 * Two rendering backends:
 *   1. Font-based: monochrome icons rasterized from Fluent UI icon fonts
 *      via stbtt_GetCodepointBitmap(). Alpha bitmap → BGRA with tint color.
 *      Cached in an LRU table keyed by (icon_id, size, color).
 *
 *   2. IRES-based: full-color icons loaded from icons.ires at boot.
 *      Pre-rendered BGRA bitmaps at multiple sizes. (Stub until §4.5)
 *
 * Memory: cached bitmaps use kmalloc for small icons (≤4 KB), PMM for
 * large icons (>4 KB). See rules.md: PMM is default for large allocs.
 * ============================================================================ */

#include "icon_store.h"
#include "font_mgr.h"
#include "stb_truetype.h"
#include "gfx.h"
#include "gfx_simd.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/types.h"

/* Forward-declare string functions */
typedef unsigned long icon_size_t;
extern void *memset(void *s, int c, icon_size_t n);
extern void *memcpy(void *dst, const void *src, icon_size_t n);
static int kstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(*(const unsigned char *)a) - (int)(*(const unsigned char *)b);
}

/* ---- Configuration ---- */

#define ICON_CACHE_MAX      128   /* Max cached icon bitmaps (LRU) */
#define LARGE_ALLOC_THRESH  4096  /* Bytes threshold for PMM vs kmalloc */

/* ---- Icon font state ---- */

typedef struct icon_font {
    stbtt_fontinfo  info;
    uint8_t        *ttf_data;
    uint32_t        ttf_size;
    int             loaded;
    int             ascent;
    int             descent;
} icon_font_t;

static icon_font_t icon_fonts[ICON_FONT_COUNT];

static const char *icon_font_filenames[ICON_FONT_COUNT] = {
    "FluentSystemIcons-Filled.ttf",
    "FluentSystemIcons-Regular.ttf",
    "FluentSystemIcons-Light.ttf",
    "FluentSystemIcons-Resizable.ttf"
};

/* ---- Codepoint mapping (auto-generated from Fluent CSS) ---- */

/* icon_codepoints[] is defined in the generated header.
 * Regenerate with: bash tools/gen_icon_map.sh
 * Also provides fluent_all_icons[] (9500+ icons) and fluent_lookup(). */
#include "generated/fluent_codepoints.h"

/* ---- Name mapping for icon_get_by_name() ---- */

static const char *icon_names[ICON_TOTAL_COUNT] = {
    /* Monochrome toolbar/action */
    "cut", "copy", "paste", "undo", "redo",
    "save", "open", "new", "delete", "refresh",
    "zoom_in", "zoom_out", "bold", "italic", "underline",
    "align_left", "align_center", "align_right", "print",
    "home", "back", "forward", "up",
    /* Monochrome system */
    "settings", "search", "lock", "user", "power",
    "info", "warning", "error", "question",
    "close", "minimize", "maximize", "restore", "menu",
    "chevron_down", "chevron_right", "chevron_left", "chevron_up",
    "check", "add", "subtract", "star", "heart", "share",
    "download", "upload", "clock", "calendar",
    "sort", "filter", "grid", "list", "link", "attach",
    "pin", "clipboard", "fullscreen",
    /* Color icons */
    "folder_closed", "folder_open", "folder_documents",
    "folder_pictures", "folder_music", "folder_downloads",
    "file_default", "file_text", "file_image", "file_audio",
    "file_video", "file_archive", "file_exe", "file_code", "file_pdf",
    "drive_local", "drive_removable", "drive_network", "drive_optical",
    "computer", "recycle_bin_empty", "recycle_bin_full",
    "printer", "network",
    "app_default", "app_text_editor", "app_media_player",
    "app_settings", "app_terminal", "app_file_manager",
    "app_calculator", "app_paint", "app_browser"
};

/* ---- LRU Icon Cache ---- */

typedef struct cache_entry {
    system_icon_t  icon_id;
    uint32_t       size;
    gfx_color_t    color;
    icon_bitmap_t  bitmap;
    uint32_t       last_access;   /* Monotonic counter for LRU eviction */
    int            valid;         /* 1 = entry is in use */
} cache_entry_t;

static cache_entry_t icon_cache[ICON_CACHE_MAX];
static uint32_t      cache_access_counter = 0;

/* ---- Theme color ---- */

static gfx_color_t icon_theme_color = 0xFFFFFFFF;  /* Default: white */

/* ---- Initialization state ---- */

static int icon_store_ready = 0;

/* ---- Internal: load a TTF font file from VFS ---- */

static int load_icon_font(int variant, const char *filename)
{
    char path[128];
    struct vfs_node *f;
    uint32_t size;
    uint8_t *data;
    int32_t bytes_read;
    int offset;

    const char *prefix = "C:\\Impossible\\Fonts\\";
    int pi = 0, fi = 0;

    /* Build path */
    while (prefix[pi] && pi < 120) {
        path[pi] = prefix[pi];
        pi++;
    }
    while (filename[fi] && pi < 127) {
        path[pi++] = filename[fi++];
    }
    path[pi] = '\0';

    f = vfs_open(path, VFS_O_READ);
    if (!f) return -1;

    size = f->size;
    if (size == 0 || size > 16 * 1024 * 1024) {
        vfs_close(f);
        return -1;
    }

    data = (uint8_t *)kmalloc(size);
    if (!data) {
        vfs_close(f);
        return -1;
    }

    bytes_read = vfs_read(f, 0, size, data);
    vfs_close(f);

    if (bytes_read <= 0) {
        kfree(data);
        return -1;
    }

    offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0) {
        kfree(data);
        return -1;
    }

    if (!stbtt_InitFont(&icon_fonts[variant].info, data, offset)) {
        kfree(data);
        return -1;
    }

    icon_fonts[variant].ttf_data = data;
    icon_fonts[variant].ttf_size = size;
    icon_fonts[variant].loaded   = 1;

    stbtt_GetFontVMetrics(&icon_fonts[variant].info,
                          &icon_fonts[variant].ascent,
                          &icon_fonts[variant].descent,
                          (int *)0);

    return 0;
}

/* ---- Internal: allocate pixel buffer ---- */

static uint32_t *alloc_pixels(uint32_t byte_count, uint8_t *from_pmm)
{
    if (byte_count > LARGE_ALLOC_THRESH) {
        uint64_t frames = (byte_count + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        if (phys != 0) {
            *from_pmm = 1;
            return (uint32_t *)phys;
        }
    }
    *from_pmm = 0;
    return (uint32_t *)kmalloc(byte_count);
}

static void free_pixels(icon_bitmap_t *bmp)
{
    if (!bmp->pixels) return;
    if (bmp->from_pmm) {
        uint64_t frames = (bmp->alloc_size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uint64_t fi;
        for (fi = 0; fi < frames; fi++)
            pmm_free_frame((uintptr_t)bmp->pixels + fi * PMM_FRAME_SIZE);
    } else {
        kfree(bmp->pixels);
    }
    bmp->pixels = (uint32_t *)0;
}

/* ---- Internal: find or evict cache entry ---- */

static cache_entry_t *cache_lookup(system_icon_t id, uint32_t size,
                                    gfx_color_t color)
{
    int i;
    for (i = 0; i < ICON_CACHE_MAX; i++) {
        if (icon_cache[i].valid &&
            icon_cache[i].icon_id == id &&
            icon_cache[i].size == size &&
            icon_cache[i].color == color) {
            icon_cache[i].last_access = ++cache_access_counter;
            return &icon_cache[i];
        }
    }
    return (cache_entry_t *)0;
}

static cache_entry_t *cache_alloc(void)
{
    int i;
    int lru_idx = -1;
    uint32_t lru_val = 0xFFFFFFFF;

    /* Find an empty slot */
    for (i = 0; i < ICON_CACHE_MAX; i++) {
        if (!icon_cache[i].valid)
            return &icon_cache[i];
    }

    /* Evict least recently used */
    for (i = 0; i < ICON_CACHE_MAX; i++) {
        if (icon_cache[i].last_access < lru_val) {
            lru_val = icon_cache[i].last_access;
            lru_idx = i;
        }
    }

    if (lru_idx >= 0) {
        free_pixels(&icon_cache[lru_idx].bitmap);
        icon_cache[lru_idx].valid = 0;
        return &icon_cache[lru_idx];
    }

    return (cache_entry_t *)0;  /* should not happen */
}

/* ---- Internal: rasterize a monochrome icon from font ---- */

static icon_bitmap_t *rasterize_glyph(system_icon_t id, uint32_t size,
                                       gfx_color_t color,
                                       icon_font_variant_t variant)
{
    icon_font_t *font;
    cache_entry_t *entry;
    uint32_t codepoint;
    float scale;
    int width, height, xoff, yoff;
    unsigned char *alpha_bmp;
    uint32_t *pixels;
    uint32_t byte_count;
    uint8_t from_pmm;
    uint32_t cr, cg, cb;
    int row, col;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    if ((uint32_t)id >= ICON_MONO_COUNT) return (icon_bitmap_t *)0;

    codepoint = icon_codepoints[id];
    if (codepoint == 0) return (icon_bitmap_t *)0;

    /* Pick font variant — fall back to Filled if variant not loaded */
    font = &icon_fonts[variant];
    if (!font->loaded) {
        font = &icon_fonts[ICON_FONT_FILLED];
        if (!font->loaded) return (icon_bitmap_t *)0;
    }

    /* Rasterize glyph */
    simd_save_state(&fpu_state);
    scale = stbtt_ScaleForPixelHeight(&font->info, (float)size);
    alpha_bmp = stbtt_GetCodepointBitmap(&font->info, 0, scale,
                                          (int)codepoint,
                                          &width, &height, &xoff, &yoff);
    simd_restore_state(&fpu_state);

    if (!alpha_bmp || width <= 0 || height <= 0) {
        if (alpha_bmp) kfree(alpha_bmp);
        return (icon_bitmap_t *)0;
    }

    /* Allocate BGRA pixel buffer */
    byte_count = (uint32_t)(width * height) * 4;
    pixels = alloc_pixels(byte_count, &from_pmm);
    if (!pixels) {
        kfree(alpha_bmp);
        return (icon_bitmap_t *)0;
    }

    /* Convert alpha bitmap → BGRA with foreground color tint */
    cr = GFX_RED(color);
    cg = GFX_GREEN(color);
    cb = GFX_BLUE(color);

    for (row = 0; row < height; row++) {
        for (col = 0; col < width; col++) {
            uint32_t alpha = alpha_bmp[row * width + col];
            if (alpha == 0) {
                pixels[row * width + col] = 0;
            } else {
                pixels[row * width + col] = GFX_RGBA(cr, cg, cb, alpha);
            }
        }
    }

    kfree(alpha_bmp);

    /* Store in cache */
    entry = cache_alloc();
    if (!entry) {
        /* Cache full — shouldn't happen with LRU but handle gracefully */
        if (from_pmm) {
            uint64_t frames = (byte_count + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
            uint64_t fi;
            for (fi = 0; fi < frames; fi++)
                pmm_free_frame((uintptr_t)pixels + fi * PMM_FRAME_SIZE);
        } else {
            kfree(pixels);
        }
        return (icon_bitmap_t *)0;
    }

    entry->icon_id              = id;
    entry->size                 = size;
    entry->color                = color;
    entry->bitmap.pixels        = pixels;
    entry->bitmap.width         = (uint16_t)width;
    entry->bitmap.height        = (uint16_t)height;
    entry->bitmap.alloc_size    = byte_count;
    entry->bitmap.from_pmm      = from_pmm;
    entry->last_access          = ++cache_access_counter;
    entry->valid                = 1;

    return &entry->bitmap;
}

/* ---- Public API ---- */

void icon_store_init(void)
{
    int i;
    int loaded = 0;
    fxsave_area_t fpu_state __attribute__((aligned(16)));

    memset(icon_cache, 0, sizeof(icon_cache));

    simd_save_state(&fpu_state);

    /* Load icon font variants */
    for (i = 0; i < ICON_FONT_COUNT; i++) {
        if (load_icon_font(i, icon_font_filenames[i]) == 0) {
            printk("[OK] Icon font loaded: %s\n",
                   (uint64_t)(uintptr_t)icon_font_filenames[i]);
            loaded++;
        } else {
            printk("[--] Icon font not found: %s\n",
                   (uint64_t)(uintptr_t)icon_font_filenames[i]);
        }
    }

    simd_restore_state(&fpu_state);

    /* TODO (§4.5): Load icons.ires for color icons */
    /* ires_load("C:\\Impossible\\System\\icons.ires"); */

    icon_store_ready = 1;

    printk("[OK] Icon store initialized (%d/%d font variants, cache=%d slots)\n",
           (uint64_t)loaded, (uint64_t)ICON_FONT_COUNT,
           (uint64_t)ICON_CACHE_MAX);
}

icon_bitmap_t *icon_get(system_icon_t id, uint32_t size)
{
    return icon_get_colored(id, size, icon_theme_color);
}

icon_bitmap_t *icon_get_variant(system_icon_t id, uint32_t size,
                                 gfx_color_t color,
                                 icon_font_variant_t variant)
{
    cache_entry_t *cached;

    if (!icon_store_ready || (uint32_t)id >= ICON_TOTAL_COUNT)
        return (icon_bitmap_t *)0;

    /* Check cache first */
    cached = cache_lookup(id, size, color);
    if (cached)
        return &cached->bitmap;

    /* Monochrome icons: rasterize from font */
    if ((uint32_t)id < ICON_MONO_COUNT) {
        return rasterize_glyph(id, size, color, variant);
    }

    /* Color icons: look up in IRES (not yet implemented — §4.5) */
    /* TODO: search IRES sizes for closest match */

    return (icon_bitmap_t *)0;
}

icon_bitmap_t *icon_get_colored(system_icon_t id, uint32_t size,
                                 gfx_color_t color)
{
    return icon_get_variant(id, size, color, ICON_FONT_FILLED);
}

system_icon_t icon_get_by_name(const char *name)
{
    int i;
    if (!name) return ICON_TOTAL_COUNT;

    for (i = 0; i < (int)ICON_TOTAL_COUNT; i++) {
        if (icon_names[i] && kstrcmp(name, icon_names[i]) == 0)
            return (system_icon_t)i;
    }
    return ICON_TOTAL_COUNT;
}

void icon_draw(gfx_surface_t *s, const icon_bitmap_t *bmp,
               int32_t x, int32_t y)
{
    int row, col;

    if (!s || !bmp || !bmp->pixels) return;

    for (row = 0; row < (int)bmp->height; row++) {
        int32_t py = y + row;
        if (py < 0 || (uint32_t)py >= s->height) continue;

        for (col = 0; col < (int)bmp->width; col++) {
            int32_t px = x + col;
            uint32_t pixel;
            if (px < 0 || (uint32_t)px >= s->width) continue;

            pixel = bmp->pixels[row * (int)bmp->width + col];
            if (GFX_ALPHA(pixel) == 0) continue;

            if (GFX_ALPHA(pixel) == 255)
                gfx_put_pixel(s, px, py, pixel);
            else
                gfx_blend_pixel(s, px, py, pixel);
        }
    }
}

void icon_draw_scaled(gfx_surface_t *s, system_icon_t id,
                       int32_t x, int32_t y, uint32_t target_size)
{
    icon_bitmap_t *bmp = icon_get(id, target_size);
    if (bmp) {
        icon_draw(s, bmp, x, y);
    }
}

gfx_color_t icon_get_theme_color(void)
{
    return icon_theme_color;
}

void icon_set_theme_color(gfx_color_t color)
{
    icon_theme_color = color;
    /* Note: existing cached monochrome icons retain their old color.
     * They'll be re-rendered with the new color on next cache miss. */
}
