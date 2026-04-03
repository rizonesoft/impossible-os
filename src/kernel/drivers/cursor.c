/* ============================================================================
 * cursor.c -- Cursor manager with Adwaita X11 cursor (Xcur) support
 *
 * Parses Xcur binary files at boot, stores decoded BGRA cursor images,
 * supports 11 cursor shapes, and renders with ARGB alpha blending.
 *
 * Xcur format:
 *   Magic: "Xcur"  (4 bytes)
 *   Header size    (4 bytes, LE)
 *   Version        (4 bytes, LE)
 *   TOC count      (4 bytes, LE)
 *   TOC entries:   type (4), subtype (4), position (4)
 *   Image chunks:  header_size (4), type=0xFFFD0002 (4), subtype=size (4),
 *                  version (4), width (4), height (4), hotspot_x (4),
 *                  hotspot_y (4), delay (4), pixels[w*h] (ARGB LE)
 * ============================================================================ */

#include "cursor.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/drivers/framebuffer.h"

/* ---- External libc-like functions (freestanding kernel) ---- */
extern void *memset(void *s, int c, __SIZE_TYPE__ n);
extern void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n);

/* ---- Xcur format constants ---- */
#define XCUR_MAGIC       0x72756358  /* "Xcur" LE */
#define XCUR_IMAGE_TYPE  0xFFFD0002
#define XCUR_TARGET_SIZE 24          /* preferred cursor size */
#define XCUR_MAX_FILE    (512 * 1024) /* max file size: 512 KB */

/* ---- Read little-endian uint32 from buffer ---- */
static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ---- Embedded fallback cursors (BGRA) ----
 * 0=transparent, 1=black outline, 2=white fill
 * Each shape has its own pixel map, width, height, hotspot. */

typedef struct {
    const uint8_t *map;
    uint32_t       w;
    uint32_t       h;
    int32_t        hx;
    int32_t        hy;
} fallback_def_t;

/* Arrow 12×19 (hotspot 0,0) */
static const uint8_t fb_arrow[] = {
    1,0,0,0,0,0,0,0,0,0,0,0,
    1,1,0,0,0,0,0,0,0,0,0,0,
    1,2,1,0,0,0,0,0,0,0,0,0,
    1,2,2,1,0,0,0,0,0,0,0,0,
    1,2,2,2,1,0,0,0,0,0,0,0,
    1,2,2,2,2,1,0,0,0,0,0,0,
    1,2,2,2,2,2,1,0,0,0,0,0,
    1,2,2,2,2,2,2,1,0,0,0,0,
    1,2,2,2,2,2,2,2,1,0,0,0,
    1,2,2,2,2,2,2,2,2,1,0,0,
    1,2,2,2,2,2,2,2,2,2,1,0,
    1,2,2,2,2,2,2,1,1,1,1,1,
    1,2,2,2,1,2,2,1,0,0,0,0,
    1,2,2,1,0,1,2,2,1,0,0,0,
    1,2,1,0,0,1,2,2,1,0,0,0,
    1,1,0,0,0,0,1,2,2,1,0,0,
    1,0,0,0,0,0,1,2,2,1,0,0,
    0,0,0,0,0,0,0,1,2,1,0,0,
    0,0,0,0,0,0,0,1,1,0,0,0,
};

/* Hand / pointer 12×17 (hotspot 5,1) */
static const uint8_t fb_hand[] = {
    0,0,0,0,0,1,1,0,0,0,0,0,
    0,0,0,0,1,2,2,1,0,0,0,0,
    0,0,0,0,1,2,2,1,0,0,0,0,
    0,0,0,0,1,2,2,1,0,0,0,0,
    0,0,0,0,1,2,2,1,1,1,0,0,
    0,0,0,0,1,2,2,1,2,2,1,0,
    0,1,1,0,1,2,2,1,2,2,1,0,
    1,2,2,1,1,2,2,2,2,2,1,0,
    1,2,2,1,2,2,2,2,2,2,1,0,
    0,1,2,2,2,2,2,2,2,2,1,0,
    0,0,1,2,2,2,2,2,2,2,1,0,
    0,0,1,2,2,2,2,2,2,1,0,0,
    0,0,0,1,2,2,2,2,2,1,0,0,
    0,0,0,1,2,2,2,2,2,1,0,0,
    0,0,0,1,2,2,2,2,2,1,0,0,
    0,0,0,0,1,2,2,2,1,0,0,0,
    0,0,0,0,0,1,1,1,0,0,0,0,
};

/* Text / I-beam 7×15 (hotspot 3,7) */
static const uint8_t fb_text[] = {
    0,1,1,0,1,1,0,
    1,0,0,1,0,0,1,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    0,0,0,1,0,0,0,
    1,0,0,1,0,0,1,
    0,1,1,0,1,1,0,
};

/* Move / four-way arrow 15×15 (hotspot 7,7) */
static const uint8_t fb_move[] = {
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,
    0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,
    0,0,0,0,1,1,1,2,1,1,1,0,0,0,0,
    0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,
    0,0,1,1,0,0,1,2,1,0,0,1,1,0,0,
    0,1,2,1,1,1,1,2,1,1,1,1,2,1,0,
    1,2,2,2,2,2,2,2,2,2,2,2,2,2,1,
    0,1,2,1,1,1,1,2,1,1,1,1,2,1,0,
    0,0,1,1,0,0,1,2,1,0,0,1,1,0,0,
    0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,
    0,0,0,0,1,1,1,2,1,1,1,0,0,0,0,
    0,0,0,0,0,1,2,2,2,1,0,0,0,0,0,
    0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
};

/* Resize N-S (vertical double arrow) 9×16 (hotspot 4,8) */
static const uint8_t fb_resize_ns[] = {
    0,0,0,0,1,0,0,0,0,
    0,0,0,1,2,1,0,0,0,
    0,0,1,2,2,2,1,0,0,
    0,1,2,2,2,2,2,1,0,
    1,1,1,1,2,1,1,1,1,
    0,0,0,1,2,1,0,0,0,
    0,0,0,1,2,1,0,0,0,
    0,0,0,1,2,1,0,0,0,
    0,0,0,1,2,1,0,0,0,
    0,0,0,1,2,1,0,0,0,
    0,0,0,1,2,1,0,0,0,
    1,1,1,1,2,1,1,1,1,
    0,1,2,2,2,2,2,1,0,
    0,0,1,2,2,2,1,0,0,
    0,0,0,1,2,1,0,0,0,
    0,0,0,0,1,0,0,0,0,
};

/* Resize E-W (horizontal double arrow) 16×9 (hotspot 8,4) */
static const uint8_t fb_resize_ew[] = {
    0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,
    0,0,0,1,1,0,0,0,0,0,0,1,1,0,0,0,
    0,0,1,2,1,0,0,0,0,0,0,1,2,1,0,0,
    0,1,2,2,1,1,1,1,1,1,1,1,2,2,1,0,
    1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,
    0,1,2,2,1,1,1,1,1,1,1,1,2,2,1,0,
    0,0,1,2,1,0,0,0,0,0,0,1,2,1,0,0,
    0,0,0,1,1,0,0,0,0,0,0,1,1,0,0,0,
    0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,
};

/* Resize NW-SE (diagonal double arrow) 12×12 (hotspot 6,6) */
static const uint8_t fb_resize_nwse[] = {
    1,1,1,1,1,1,0,0,0,0,0,0,
    1,2,2,2,2,1,0,0,0,0,0,0,
    1,2,2,2,1,0,0,0,0,0,0,0,
    1,2,2,2,2,1,0,0,0,0,0,0,
    1,2,1,2,2,2,1,0,0,0,0,0,
    1,1,0,1,2,2,2,1,0,0,0,0,
    0,0,0,0,1,2,2,2,1,0,1,1,
    0,0,0,0,0,1,2,2,2,1,2,1,
    0,0,0,0,0,0,1,2,2,2,2,1,
    0,0,0,0,0,0,0,1,2,2,2,1,
    0,0,0,0,0,0,1,2,2,2,2,1,
    0,0,0,0,0,0,1,1,1,1,1,1,
};

/* Resize NE-SW (diagonal double arrow) 12×12 (hotspot 6,6) */
static const uint8_t fb_resize_nesw[] = {
    0,0,0,0,0,0,1,1,1,1,1,1,
    0,0,0,0,0,0,1,2,2,2,2,1,
    0,0,0,0,0,0,0,0,1,2,2,1,
    0,0,0,0,0,0,0,1,2,2,2,1,
    0,0,0,0,0,0,1,2,2,2,1,1,
    0,0,0,0,0,1,2,2,2,1,0,1,
    1,1,0,0,1,2,2,2,1,0,0,0,
    1,2,1,0,1,2,2,1,0,0,0,0,
    1,2,2,1,2,2,1,0,0,0,0,0,
    1,2,2,2,2,1,0,0,0,0,0,0,
    1,2,2,2,2,1,0,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,0,
};

/* Wait / hourglass 11×16 (hotspot 5,8) */
static const uint8_t fb_wait[] = {
    1,1,1,1,1,1,1,1,1,1,1,
    1,2,2,2,2,2,2,2,2,2,1,
    0,1,2,2,2,2,2,2,2,1,0,
    0,0,1,2,2,2,2,2,1,0,0,
    0,0,0,1,2,2,2,1,0,0,0,
    0,0,0,0,1,2,1,0,0,0,0,
    0,0,0,0,0,1,0,0,0,0,0,
    0,0,0,0,1,2,1,0,0,0,0,
    0,0,0,0,1,2,1,0,0,0,0,
    0,0,0,0,1,2,1,0,0,0,0,
    0,0,0,1,2,1,2,1,0,0,0,
    0,0,1,2,1,0,1,2,1,0,0,
    0,1,2,1,0,0,0,1,2,1,0,
    0,1,2,2,2,2,2,2,2,1,0,
    1,2,2,2,2,2,2,2,2,2,1,
    1,1,1,1,1,1,1,1,1,1,1,
};

/* Crosshair 15×15 (hotspot 7,7) */
static const uint8_t fb_crosshair[] = {
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,
    1,1,1,1,1,0,1,2,1,0,1,1,1,1,1,
    0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
};

/* Forbidden / no-entry 13×13 (hotspot 6,6) */
static const uint8_t fb_forbidden[] = {
    0,0,0,0,1,1,1,1,1,0,0,0,0,
    0,0,1,1,2,2,2,2,2,1,1,0,0,
    0,1,2,2,2,2,2,2,1,2,2,1,0,
    0,1,2,2,2,2,2,1,2,2,2,1,0,
    1,2,2,2,2,2,1,2,2,2,2,2,1,
    1,2,2,2,2,1,2,2,2,2,2,2,1,
    1,2,2,2,1,2,2,2,2,2,2,2,1,
    1,2,2,1,2,2,2,2,2,2,2,2,1,
    1,2,1,2,2,2,2,2,2,2,2,2,1,
    0,1,2,2,2,2,2,2,2,2,2,1,0,
    0,1,2,2,2,2,2,2,2,2,2,1,0,
    0,0,1,1,2,2,2,2,2,1,1,0,0,
    0,0,0,0,1,1,1,1,1,0,0,0,0,
};

/* ---- Fallback definitions table ---- */

static const fallback_def_t fallback_defs[CURSOR_COUNT] = {
    [CURSOR_ARROW]       = { fb_arrow,       12, 19, 0, 0 },
    [CURSOR_HAND]        = { fb_hand,        12, 17, 5, 1 },
    [CURSOR_TEXT]        = { fb_text,         7, 15, 3, 7 },
    [CURSOR_MOVE]        = { fb_move,        15, 15, 7, 7 },
    [CURSOR_RESIZE_NS]   = { fb_resize_ns,    9, 16, 4, 8 },
    [CURSOR_RESIZE_EW]   = { fb_resize_ew,   16,  9, 8, 4 },
    [CURSOR_RESIZE_NWSE] = { fb_resize_nwse, 12, 12, 6, 6 },
    [CURSOR_RESIZE_NESW] = { fb_resize_nesw, 12, 12, 6, 6 },
    [CURSOR_WAIT]        = { fb_wait,        11, 16, 5, 8 },
    [CURSOR_CROSSHAIR]   = { fb_crosshair,   15, 15, 7, 7 },
    [CURSOR_FORBIDDEN]   = { fb_forbidden,   13, 13, 6, 6 },
};

/* Pre-rendered BGRA pixel buffers for all fallbacks */
#define MAX_FB_PIXELS (16 * 19)  /* largest fallback */
static uint32_t fallback_pixels_all[CURSOR_COUNT][MAX_FB_PIXELS];

static void build_fallback_cursors(void)
{
    for (int s = 0; s < CURSOR_COUNT; s++) {
        const fallback_def_t *def = &fallback_defs[s];
        uint32_t *dst = fallback_pixels_all[s];
        uint32_t total = def->w * def->h;
        for (uint32_t i = 0; i < total; i++) {
            uint8_t v = def->map[i];
            if (v == 0)      dst[i] = 0x00000000;  /* transparent */
            else if (v == 1) dst[i] = 0xFF000000;  /* black outline */
            else             dst[i] = 0xFFFFFFFF;  /* white fill */
        }
    }
}

/* ---- Cursor shape → Adwaita filename mapping ---- */

static const char *cursor_filenames[CURSOR_COUNT] = {
    [CURSOR_ARROW]       = "C:\\Impossible\\System\\Cursors\\default",
    [CURSOR_HAND]        = "C:\\Impossible\\System\\Cursors\\pointer",
    [CURSOR_TEXT]        = "C:\\Impossible\\System\\Cursors\\text",
    [CURSOR_MOVE]        = "C:\\Impossible\\System\\Cursors\\fleur",
    [CURSOR_RESIZE_NS]   = "C:\\Impossible\\System\\Cursors\\sb_v_double_arrow",
    [CURSOR_RESIZE_EW]   = "C:\\Impossible\\System\\Cursors\\sb_h_double_arrow",
    [CURSOR_RESIZE_NWSE] = "C:\\Impossible\\System\\Cursors\\bd_double_arrow",
    [CURSOR_RESIZE_NESW] = "C:\\Impossible\\System\\Cursors\\fd_double_arrow",
    [CURSOR_WAIT]        = "C:\\Impossible\\System\\Cursors\\progress",
    [CURSOR_CROSSHAIR]   = "C:\\Impossible\\System\\Cursors\\crosshair",
    [CURSOR_FORBIDDEN]   = "C:\\Impossible\\System\\Cursors\\not-allowed",
};

/* ---- Cursor state ---- */

static cursor_sprite_t  cursors[CURSOR_COUNT];
static cursor_shape_t   active_shape = CURSOR_ARROW;

/* Save/restore buffer for cursor compositing */
static uint32_t saved_under[CURSOR_MAX_SIZE * CURSOR_MAX_SIZE];
static int32_t  saved_x = -1;
static int32_t  saved_y = -1;
static uint32_t saved_w = 0;
static uint32_t saved_h = 0;
static uint8_t  cursor_visible = 0;

/* ---- Get the active cursor image (best size) ---- */

static const cursor_image_t *get_active_image(void)
{
    cursor_sprite_t *spr = &cursors[active_shape];
    if (spr->num_sizes == 0) {
        /* Fallback to arrow */
        spr = &cursors[CURSOR_ARROW];
    }
    if (spr->num_sizes == 0)
        return (void *)0;  /* should never happen after init */

    /* Find closest size to XCUR_TARGET_SIZE */
    const cursor_image_t *best = &spr->images[0];
    int32_t best_diff = (int32_t)best->width - XCUR_TARGET_SIZE;
    if (best_diff < 0) best_diff = -best_diff;

    for (uint8_t i = 1; i < spr->num_sizes; i++) {
        int32_t diff = (int32_t)spr->images[i].width - XCUR_TARGET_SIZE;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) {
            best = &spr->images[i];
            best_diff = diff;
        }
    }
    return best;
}

/* ---- Xcur file parser ---- */

static int xcur_parse(const uint8_t *data, uint32_t file_size,
                      cursor_sprite_t *out, uint32_t *pixel_buf)
{
    if (file_size < 16)
        return -1;

    /* Validate magic */
    uint32_t magic = read_le32(data);
    if (magic != XCUR_MAGIC)
        return -1;

    uint32_t header_size = read_le32(data + 4);
    /* uint32_t version = read_le32(data + 8); */
    uint32_t toc_count  = read_le32(data + 12);

    /* Sanity check */
    if (header_size + toc_count * 12 > file_size)
        return -1;

    out->num_sizes = 0;

    /* First pass: find the best-fit image chunk (closest to target size).
     * We only keep ONE size per cursor to avoid heap exhaustion. */
    uint32_t best_pos   = 0;
    int32_t  best_diff  = 9999;
    uint8_t  found_any  = 0;

    for (uint32_t i = 0; i < toc_count; i++) {
        uint32_t toc_off = header_size + i * 12;
        if (toc_off + 12 > file_size)
            break;

        uint32_t type     = read_le32(data + toc_off);
        uint32_t subtype  = read_le32(data + toc_off + 4);  /* = nominal size */
        uint32_t position = read_le32(data + toc_off + 8);

        if (type != XCUR_IMAGE_TYPE)
            continue;

        /* Pick the size closest to XCUR_TARGET_SIZE */
        int32_t diff = (int32_t)subtype - XCUR_TARGET_SIZE;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) {
            best_diff = diff;
            best_pos  = position;
            found_any = 1;
        }
    }

    if (!found_any)
        return -1;

    /* Second pass: decode only the best-fit image */
    if (best_pos + 36 > file_size)
        return -1;

    const uint8_t *chunk = data + best_pos;
    uint32_t chunk_type = read_le32(chunk + 4);
    uint32_t w  = read_le32(chunk + 16);
    uint32_t h  = read_le32(chunk + 20);
    int32_t  hx = (int32_t)read_le32(chunk + 24);
    int32_t  hy = (int32_t)read_le32(chunk + 28);

    if (chunk_type != XCUR_IMAGE_TYPE)
        return -1;
    if (w == 0 || h == 0 || w > 256 || h > 256)
        return -1;

    uint32_t pixel_bytes = w * h * 4;
    if (best_pos + 36 + pixel_bytes > file_size)
        return -1;

    /* Copy ARGB pixels into the pre-allocated buffer.
     * Xcur stores as BGRA in little-endian, which matches our
     * framebuffer format directly (B in low byte) */
    const uint8_t *src = chunk + 36;
    for (uint32_t p = 0; p < w * h; p++) {
        uint8_t b = src[p * 4 + 0];
        uint8_t g = src[p * 4 + 1];
        uint8_t r = src[p * 4 + 2];
        uint8_t a = src[p * 4 + 3];
        pixel_buf[p] = (uint32_t)b
                     | ((uint32_t)g << 8)
                     | ((uint32_t)r << 16)
                     | ((uint32_t)a << 24);
    }

    cursor_image_t *img = &out->images[0];
    img->width     = w;
    img->height    = h;
    img->hotspot_x = hx;
    img->hotspot_y = hy;
    img->pixels    = pixel_buf;
    out->num_sizes = 1;

    return 0;
}

/* ---- Load one cursor from VFS ---- */

static int xcur_load(const char *path, cursor_sprite_t *out,
                     uint32_t *pixel_buf)
{
    struct vfs_node *node = vfs_open(path, VFS_O_READ);
    if (!node)
        return -1;

    /* Clamp to file size if known, otherwise use max */
    uint32_t file_sz = XCUR_MAX_FILE;
    if (node->size > 0 && node->size < XCUR_MAX_FILE)
        file_sz = (uint32_t)node->size;

    /* Read file into a temporary PMM buffer */
    uint32_t pages = (file_sz + 4095) / 4096;
    uint8_t *buf = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(pages);
    if (!buf) {
        vfs_close(node);
        return -1;
    }

    uint32_t offset = 0;
    int32_t n;
    while (offset < file_sz) {
        n = vfs_read(node, offset, file_sz - offset, buf + offset);
        if (n <= 0) break;
        offset += (uint32_t)n;
    }
    vfs_close(node);

    if (offset < 16) {
        /* Free temp buffer */
        for (uint32_t p = 0; p < pages; p++)
            pmm_free_frame(((uintptr_t)buf / 4096) + p);
        return -1;
    }

    int rc = xcur_parse(buf, offset, out, pixel_buf);

    /* Free temp buffer */
    for (uint32_t p = 0; p < pages; p++)
        pmm_free_frame(((uintptr_t)buf / 4096) + p);

    return rc;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void cursor_init(void)
{
    /* Build all 11 fallback cursors from the hardcoded pixel maps */
    build_fallback_cursors();

    /* Set up fallback for every shape */
    for (int i = 0; i < CURSOR_COUNT; i++) {
        const fallback_def_t *def = &fallback_defs[i];
        cursor_image_t *img = &cursors[i].images[0];
        img->width     = def->w;
        img->height    = def->h;
        img->hotspot_x = def->hx;
        img->hotspot_y = def->hy;
        img->pixels    = fallback_pixels_all[i];
        cursors[i].num_sizes = 1;
    }

    /* Pre-allocate ALL cursor pixel buffers as one contiguous PMM block.
     * This prevents the temp-buffer free inside xcur_load from reclaiming
     * pixel pages (the PMM was returning the same page for every cursor). */
    #define PIXELS_PER_CURSOR (XCUR_TARGET_SIZE * XCUR_TARGET_SIZE)
    #define BYTES_PER_CURSOR  (PIXELS_PER_CURSOR * 4)
    uint32_t total_pixel_pages = (CURSOR_COUNT * BYTES_PER_CURSOR + 4095) / 4096;
    uint32_t *pixel_pool = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(total_pixel_pages);

    /* Attempt to load Adwaita cursors from the sysroot */
    uint32_t loaded = 0;
    if (pixel_pool) {
        for (int i = 0; i < CURSOR_COUNT; i++) {
            cursor_sprite_t tmp;
            memset(&tmp, 0, sizeof(tmp));

            uint32_t *slot = pixel_pool + (i * PIXELS_PER_CURSOR);
            if (xcur_load(cursor_filenames[i], &tmp, slot) == 0) {
                cursors[i] = tmp;
                loaded++;
            }
        }
    }

    active_shape = CURSOR_ARROW;
    klog(LOG_INFO, "gfx", "Cursor manager initialized (%u/%d Adwaita cursors loaded)",
           loaded, CURSOR_COUNT);
}

void cursor_set_shape(cursor_shape_t shape)
{
    if (shape < CURSOR_COUNT)
        active_shape = shape;
}

cursor_shape_t cursor_get_shape(void)
{
    return active_shape;
}

void cursor_get_hotspot(int32_t *hx, int32_t *hy)
{
    const cursor_image_t *img = get_active_image();
    if (img) {
        if (hx) *hx = img->hotspot_x;
        if (hy) *hy = img->hotspot_y;
    } else {
        if (hx) *hx = 0;
        if (hy) *hy = 0;
    }
}

void cursor_restore(void)
{
    if (!cursor_visible)
        return;

    uint32_t scr_w = fb_get_width();
    uint32_t scr_h = fb_get_height();

    for (uint32_t py = 0; py < saved_h; py++) {
        for (uint32_t px = 0; px < saved_w; px++) {
            uint32_t sx = (uint32_t)(saved_x + (int32_t)px);
            uint32_t sy = (uint32_t)(saved_y + (int32_t)py);
            if (sx < scr_w && sy < scr_h)
                fb_put_pixel(sx, sy, saved_under[py * saved_w + px]);
        }
    }

    cursor_visible = 0;
}

void cursor_draw(int32_t x, int32_t y)
{
    const cursor_image_t *img = get_active_image();
    if (!img)
        return;

    uint32_t w = img->width;
    uint32_t h = img->height;

    /* Adjust for hotspot */
    int32_t cx = x - img->hotspot_x;
    int32_t cy = y - img->hotspot_y;

    uint32_t scr_w = fb_get_width();
    uint32_t scr_h = fb_get_height();

    /* Save pixels underneath */
    saved_x = cx;
    saved_y = cy;
    saved_w = w;
    saved_h = h;

    for (uint32_t py = 0; py < h; py++) {
        for (uint32_t px = 0; px < w; px++) {
            int32_t sx = cx + (int32_t)px;
            int32_t sy = cy + (int32_t)py;
            if (sx >= 0 && (uint32_t)sx < scr_w &&
                sy >= 0 && (uint32_t)sy < scr_h)
                saved_under[py * w + px] = fb_read_pixel((uint32_t)sx,
                                                          (uint32_t)sy);
            else
                saved_under[py * w + px] = 0;
        }
    }
    cursor_visible = 1;

    /* Draw cursor with alpha blending */
    for (uint32_t py = 0; py < h; py++) {
        for (uint32_t px = 0; px < w; px++) {
            uint32_t src_pixel = img->pixels[py * w + px];
            uint8_t sa = (uint8_t)(src_pixel >> 24);

            if (sa == 0)
                continue;  /* fully transparent */

            int32_t sx = cx + (int32_t)px;
            int32_t sy = cy + (int32_t)py;
            if (sx < 0 || (uint32_t)sx >= scr_w ||
                sy < 0 || (uint32_t)sy >= scr_h)
                continue;

            if (sa == 255) {
                /* Fully opaque -- no blending needed */
                fb_put_pixel((uint32_t)sx, (uint32_t)sy, src_pixel);
            } else {
                /* Alpha blend: out = src*a + dst*(255-a) / 255 */
                uint32_t dst_pixel = fb_read_pixel((uint32_t)sx,
                                                    (uint32_t)sy);
                uint8_t sb = (uint8_t)(src_pixel);
                uint8_t sg = (uint8_t)(src_pixel >> 8);
                uint8_t sr = (uint8_t)(src_pixel >> 16);

                uint8_t db = (uint8_t)(dst_pixel);
                uint8_t dg = (uint8_t)(dst_pixel >> 8);
                uint8_t dr = (uint8_t)(dst_pixel >> 16);

                uint8_t inv_a = 255 - sa;
                uint8_t ob = (uint8_t)((sa * sb + inv_a * db) / 255);
                uint8_t og = (uint8_t)((sa * sg + inv_a * dg) / 255);
                uint8_t or_ = (uint8_t)((sa * sr + inv_a * dr) / 255);

                uint32_t out = (uint32_t)ob
                             | ((uint32_t)og << 8)
                             | ((uint32_t)or_ << 16)
                             | 0xFF000000;
                fb_put_pixel((uint32_t)sx, (uint32_t)sy, out);
            }
        }
    }
}

int cursor_get_rect(int32_t *rx, int32_t *ry, uint32_t *rw, uint32_t *rh)
{
    if (!cursor_visible)
        return 0;
    if (rx) *rx = saved_x;
    if (ry) *ry = saved_y;
    if (rw) *rw = saved_w;
    if (rh) *rh = saved_h;
    return 1;
}
