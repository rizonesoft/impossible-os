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
#include "kernel/klog.h"
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

/* ---- IRES color icon support ---- */

/* Must match irespack.c format exactly */
#define IRES_MAGIC          0x53455249  /* "IRES" little-endian */
#define IRES_VERSION        1
#define IRES_MAX_SIZES      16
#define IRES_COLOR_COUNT    (ICON_COLOR_COUNT - ICON_MONO_COUNT)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t icon_count;
    uint8_t  size_count;
    uint8_t  reserved[3];
    uint32_t file_size;
} ires_header_t;

typedef struct __attribute__((packed)) {
    uint32_t data_offset;
    uint16_t width;
    uint16_t height;
} ires_size_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t icon_id;
    uint16_t name_offset;
} ires_index_entry_t;

/* Runtime: per color icon, per size, store pointer + dimensions */
typedef struct {
    uint32_t *pixels;   /* Points into PMM-loaded IRES data */
    uint16_t  width;
    uint16_t  height;
} ires_icon_size_t;

typedef struct {
    ires_icon_size_t sizes[IRES_MAX_SIZES];
    int              has_any;  /* 1 if at least one size loaded */
} ires_icon_t;

static ires_icon_t  ires_icons[IRES_COLOR_COUNT];
static uint8_t     *ires_file_data;    /* PMM buffer holding entire file */
static uint32_t     ires_file_size;
static int          ires_loaded;
static uint8_t      ires_size_count;
static uint16_t     ires_sizes[IRES_MAX_SIZES];

/* ---- Name mapping for icon_get_by_name() and IRES name resolution ---- */

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
    "folder_closed", "folder_open",
    "file_default", "exe_default",
    "dll_default", "text_file",
    "computer", "recycle_bin_empty", "recycle_bin_full",
    "control_panel"
};

/* ---- Internal: load IRES file from VFS ---- */

static int ires_load(const char *path)
{
    struct vfs_node *f;
    uint32_t size;
    uint64_t frames;
    int32_t bytes_read;
    uint8_t *data;
    ires_header_t *hdr;
    uint8_t *cursor;
    int i, si;

    f = vfs_open(path, VFS_O_READ);
    if (!f) return -1;

    size = f->size;
    if (size < sizeof(ires_header_t) || size > 32 * 1024 * 1024) {
        vfs_close(f);
        return -2;
    }

    /* Allocate via PMM — IRES can be several MB */
    frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    data = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(frames);
    if (!data) {
        vfs_close(f);
        return -3;
    }

    bytes_read = vfs_read(f, 0, size, data);
    vfs_close(f);
    if (bytes_read <= 0 || (uint32_t)bytes_read < sizeof(ires_header_t)) {
        uint64_t fi;
        for (fi = 0; fi < frames; fi++)
            pmm_free_frame((uintptr_t)data + fi * PMM_FRAME_SIZE);
        return -4;
    }

    /* Parse header */
    hdr = (ires_header_t *)data;
    if (hdr->magic != IRES_MAGIC || hdr->version != IRES_VERSION) {
        uint64_t fi;
        for (fi = 0; fi < frames; fi++)
            pmm_free_frame((uintptr_t)data + fi * PMM_FRAME_SIZE);
        return -5;
    }

    ires_file_data = data;
    ires_file_size = size;
    ires_size_count = hdr->size_count;
    if (ires_size_count > IRES_MAX_SIZES)
        ires_size_count = IRES_MAX_SIZES;

    /* Read size table */
    cursor = data + sizeof(ires_header_t);
    for (si = 0; si < ires_size_count; si++) {
        ires_sizes[si] = *(uint16_t *)cursor;
        cursor += 2;
    }

    /* Parse index entries — resolve icon IDs by name, not by hardcoded number.
     * This decouples irespack from the kernel's enum numbering. */
    memset(ires_icons, 0, sizeof(ires_icons));

    /* Calculate name table base: after header + size table + all index entries */
    {
        uint32_t index_entry_sz = sizeof(ires_index_entry_t) +
                                  ires_size_count * sizeof(ires_size_entry_t);
        uint8_t *name_table_base = data + sizeof(ires_header_t) +
                                   ires_size_count * 2 +
                                   (uint32_t)hdr->icon_count * index_entry_sz;

        for (i = 0; i < (int)hdr->icon_count; i++) {
            ires_index_entry_t *idx = (ires_index_entry_t *)cursor;
            cursor += sizeof(ires_index_entry_t);

            /* Resolve icon name → system_icon_t via icon_names[] */
            const char *icon_name = (const char *)(name_table_base + idx->name_offset);
            int color_idx = -1;
            {
                int k;
                for (k = (int)ICON_MONO_COUNT; k < (int)ICON_TOTAL_COUNT; k++) {
                    if (kstrcmp(icon_names[k], icon_name) == 0) {
                        color_idx = k - (int)ICON_MONO_COUNT;
                        break;
                    }
                }
            }

            if (color_idx < 0 || color_idx >= IRES_COLOR_COUNT) {
                cursor += ires_size_count * sizeof(ires_size_entry_t);
                continue;
            }

            for (si = 0; si < ires_size_count; si++) {
                ires_size_entry_t *se = (ires_size_entry_t *)cursor;
                cursor += sizeof(ires_size_entry_t);

                if (se->data_offset != 0 && se->data_offset < size &&
                    se->width > 0 && se->height > 0) {
                    ires_icons[color_idx].sizes[si].pixels =
                        (uint32_t *)(data + se->data_offset);
                    ires_icons[color_idx].sizes[si].width  = se->width;
                    ires_icons[color_idx].sizes[si].height = se->height;
                    ires_icons[color_idx].has_any = 1;
                }
            }
        }
    }

    ires_loaded = 1;
    return (int)hdr->icon_count;
}



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
    uint64_t frames;

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
    if (!f) return -1;  /* File not found */

    size = f->size;
    if (size == 0 || size > 16 * 1024 * 1024) {
        vfs_close(f);
        return -2;  /* Invalid size */
    }

    /* Use PMM for font data — fonts can be 1–3 MB, way too big for
     * the 2 MiB kernel heap. See rules.md: PMM for large allocs. */
    frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    data = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(frames);
    if (!data) {
        vfs_close(f);
        return -3;  /* Allocation failed */
    }

    bytes_read = vfs_read(f, 0, size, data);
    vfs_close(f);

    if (bytes_read <= 0) {
        uint64_t i;
        for (i = 0; i < frames; i++)
            pmm_free_frame((uintptr_t)data + i * PMM_FRAME_SIZE);
        return -4;  /* Read failed */
    }

    offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0) {
        uint64_t i;
        for (i = 0; i < frames; i++)
            pmm_free_frame((uintptr_t)data + i * PMM_FRAME_SIZE);
        return -5;  /* Not a valid font */
    }

    if (!stbtt_InitFont(&icon_fonts[variant].info, data, offset)) {
        uint64_t i;
        for (i = 0; i < frames; i++)
            pmm_free_frame((uintptr_t)data + i * PMM_FRAME_SIZE);
        return -6;  /* Font init failed */
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

/* ---- Internal: get color icon bitmap from IRES ---- */

static icon_bitmap_t *ires_get_bitmap(system_icon_t id, uint32_t size)
{
    int color_idx = (int)id - (int)ICON_MONO_COUNT;
    int best_si = -1;
    uint32_t best_diff = 0xFFFFFFFF;
    ires_icon_size_t *is;
    cache_entry_t *entry;
    int si;

    if (color_idx < 0 || color_idx >= IRES_COLOR_COUNT)
        return (icon_bitmap_t *)0;
    if (!ires_icons[color_idx].has_any)
        return (icon_bitmap_t *)0;

    /* Find closest available size */
    for (si = 0; si < ires_size_count; si++) {
        if (ires_icons[color_idx].sizes[si].pixels) {
            uint32_t diff = (ires_sizes[si] > size)
                ? ires_sizes[si] - size
                : size - ires_sizes[si];
            if (diff < best_diff) {
                best_diff = diff;
                best_si   = si;
            }
        }
    }

    if (best_si < 0) return (icon_bitmap_t *)0;
    is = &ires_icons[color_idx].sizes[best_si];

    /* Store in cache (pixels point into IRES file buffer — no alloc needed) */
    entry = cache_alloc();
    if (!entry) return (icon_bitmap_t *)0;

    entry->icon_id              = id;
    entry->size                 = size;
    entry->color                = 0;  /* Color icons don't have tint */
    entry->bitmap.pixels        = is->pixels;
    entry->bitmap.width         = is->width;
    entry->bitmap.height        = is->height;
    entry->bitmap.alloc_size    = 0;  /* Don't free — points into IRES buffer */
    entry->bitmap.from_pmm      = 0;
    entry->last_access          = ++cache_access_counter;
    entry->valid                = 1;

    return &entry->bitmap;
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

    /* Pick font variant -- fall back to Filled if variant not loaded */
    font = &icon_fonts[variant];
    if (!font->loaded) {
        font = &icon_fonts[ICON_FONT_FILLED];
        if (!font->loaded) return (icon_bitmap_t *)0;
    }

    /* Rasterize glyph (manual FPU save until lazy XSAVE alignment is fixed) */
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
        int err = load_icon_font(i, icon_font_filenames[i]);
        if (err == 0) {
            klog(LOG_INFO, "ICON", "Icon font loaded: %s",
                   (uint64_t)(uintptr_t)icon_font_filenames[i]);
            loaded++;
        } else {
            klog(LOG_WARN, "ICON", "Icon font failed (%d): %s",
                   (uint64_t)err,
                   (uint64_t)(uintptr_t)icon_font_filenames[i]);
        }
    }

    simd_restore_state(&fpu_state);

    /* Load color icons from IRES file */
    {
        int ires_result = ires_load("C:\\Impossible\\Icons\\icons.ires");
        if (ires_result > 0) {
            klog(LOG_INFO, "ICON", "IRES loaded: icons.ires (%d icons, %d sizes)",
                   (uint64_t)ires_result, (uint64_t)ires_size_count);
        } else if (ires_result == -1) {
            klog(LOG_WARN, "ICON", "IRES not found: icons.ires");
        } else {
            klog(LOG_ERROR, "ICON", "IRES load failed (%d): icons.ires",
                   (uint64_t)ires_result);
        }
    }

    icon_store_ready = 1;

    klog(LOG_INFO, "ICON", "Icon store initialized (%d/%d fonts, cache=%d, color=%s)",
           (uint64_t)loaded, (uint64_t)ICON_FONT_COUNT,
           (uint64_t)ICON_CACHE_MAX,
           (uint64_t)(uintptr_t)(ires_loaded ? "yes" : "no"));
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

    /* Color icons: look up in IRES */
    if (ires_loaded) {
        return ires_get_bitmap(id, size);
    }
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

/* ---- File extension → icon mapping ---- */

typedef struct {
    const char     *ext;
    system_icon_t   icon;
} ext_map_entry_t;

static const ext_map_entry_t ext_map[] = {
    /* Executables / libraries */
    { ".exe",  ICON_EXE_DEFAULT },
    { ".dll",  ICON_DLL_DEFAULT },
    { ".sys",  ICON_DLL_DEFAULT },
    /* Text files */
    { ".txt",  ICON_TEXT_FILE },
    { ".md",   ICON_TEXT_FILE },
    { ".log",  ICON_TEXT_FILE },
    { ".cfg",  ICON_TEXT_FILE },
    { ".ini",  ICON_TEXT_FILE },
    { NULL,    ICON_FILE_DEFAULT }
};

system_icon_t icon_for_extension(const char *ext)
{
    int i;

    if (!ext) return ICON_FILE_DEFAULT;

    /* Skip leading dot if missing */
    for (i = 0; ext_map[i].ext; i++) {
        if (kstrcmp(ext, ext_map[i].ext) == 0)
            return ext_map[i].icon;
    }

    return ICON_FILE_DEFAULT;
}
