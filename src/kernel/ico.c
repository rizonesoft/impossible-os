/* ============================================================================
 * ico.c -- ICO file loader for Impossible OS
 *
 * Parses Windows .ico containers. Each entry can hold either:
 *   - PNG data (detected by 0x89504E47 magic) → decoded via image_load_mem()
 *   - BMP DIB data (headerless bitmap) → parsed directly here
 *
 * All decoded images are in BGRA format matching gfx_color_t / icon_bitmap_t.
 * ============================================================================ */

#include "ico.h"
#include "kernel/fs/vfs.h"
#include "kernel/image.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"

extern void *memset(void *s, int c, __SIZE_TYPE__ n);

/* ---- Internal: free contiguous PMM frames ---- */

static void pmm_free_range(uintptr_t addr, uint64_t frame_count)
{
    uint64_t i;
    for (i = 0; i < frame_count; i++)
        pmm_free_frame(addr + i * PMM_FRAME_SIZE);
}

/* ---- ICO file structures (all little-endian, packed) ---- */

typedef struct __attribute__((packed)) {
    uint16_t reserved;   /* Must be 0 */
    uint16_t type;       /* 1 = ICO, 2 = CUR */
    uint16_t count;      /* Number of images */
} ico_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  width;      /* Width (0 = 256) */
    uint8_t  height;     /* Height (0 = 256) */
    uint8_t  color_count;/* Colors in palette (0 = no palette) */
    uint8_t  reserved;
    uint16_t planes;     /* Color planes (should be 0 or 1) */
    uint16_t bpp;        /* Bits per pixel */
    uint32_t data_size;  /* Size of image data in bytes */
    uint32_t data_offset;/* Offset from file start to image data */
} ico_dir_entry_t;

/* BMP DIB header (BITMAPINFOHEADER, 40 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t header_size;    /* 40 */
    int32_t  width;
    int32_t  height;         /* Doubled if AND mask present */
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_dib_header_t;

/* PNG magic bytes */
#define PNG_MAGIC  0x474E5089  /* "\x89PNG" little-endian */

/* ---- Internal: decode a BMP DIB (headerless bitmap) ---- */

static int decode_bmp_dib(icon_bitmap_t *out, const uint8_t *data,
                          uint32_t data_size)
{
    const bmp_dib_header_t *dib;
    uint32_t w, h, bpp, row_stride, pixel_bytes;
    uint32_t *pixels;
    const uint8_t *src;
    uint32_t x, y;
    int has_alpha_mask;

    if (data_size < sizeof(bmp_dib_header_t))
        return -1;

    dib = (const bmp_dib_header_t *)data;

    if (dib->header_size < 40)
        return -1;

    w   = (uint32_t)(dib->width < 0 ? -dib->width : dib->width);
    /* Height is doubled in ICO (includes AND mask) */
    h   = (uint32_t)(dib->height < 0 ? -dib->height : dib->height);
    bpp = dib->bpp;

    /* ICO BMP height is doubled to include the AND (transparency) mask */
    has_alpha_mask = 0;
    if (h == w * 2) {
        h = w;
        has_alpha_mask = 1;
    }

    if (w == 0 || h == 0 || w > 256 || h > 256)
        return -1;

    /* Only handle 32bpp (BGRA) and 24bpp (BGR) */
    if (bpp != 32 && bpp != 24)
        return -1;

    pixel_bytes = w * h * 4;
    if (pixel_bytes <= 4096) {
        pixels = (uint32_t *)kmalloc(pixel_bytes);
    } else {
        uint64_t frames = (pixel_bytes + 4095) / 4096;
        pixels = (uint32_t *)pmm_alloc_contiguous(frames);
    }
    if (!pixels) return -1;

    /* Pixel data starts after the DIB header */
    src = data + dib->header_size;
    row_stride = ((w * bpp / 8) + 3) & ~3u;  /* BMP rows are 4-byte aligned */

    if (bpp == 32) {
        /* 32bpp BGRA -- already in our format, but BMP rows are bottom-up */
        for (y = 0; y < h; y++) {
            const uint8_t *row = src + (h - 1 - y) * row_stride;
            for (x = 0; x < w; x++) {
                uint8_t b = row[x * 4 + 0];
                uint8_t g = row[x * 4 + 1];
                uint8_t r = row[x * 4 + 2];
                uint8_t a = row[x * 4 + 3];
                pixels[y * w + x] = ((uint32_t)a << 24) |
                                    ((uint32_t)r << 16) |
                                    ((uint32_t)g <<  8) |
                                    (uint32_t)b;
            }
        }
    } else {
        /* 24bpp BGR -- no alpha channel, use AND mask or default opaque */
        for (y = 0; y < h; y++) {
            const uint8_t *row = src + (h - 1 - y) * row_stride;
            for (x = 0; x < w; x++) {
                uint8_t b = row[x * 3 + 0];
                uint8_t g = row[x * 3 + 1];
                uint8_t r = row[x * 3 + 2];
                pixels[y * w + x] = 0xFF000000 |
                                    ((uint32_t)r << 16) |
                                    ((uint32_t)g <<  8) |
                                    (uint32_t)b;
            }
        }

        /* Apply AND mask for transparency (1-bit per pixel, bottom-up) */
        if (has_alpha_mask) {
            uint32_t mask_stride = ((w + 31) / 32) * 4;
            const uint8_t *mask_data = src + h * row_stride;
            for (y = 0; y < h; y++) {
                const uint8_t *mask_row = mask_data + (h - 1 - y) * mask_stride;
                for (x = 0; x < w; x++) {
                    uint8_t mask_bit = (mask_row[x / 8] >> (7 - (x % 8))) & 1;
                    if (mask_bit) {
                        /* AND mask bit = 1 means transparent */
                        pixels[y * w + x] &= 0x00FFFFFF;  /* Clear alpha */
                    }
                }
            }
        }
    }

    out->pixels     = pixels;
    out->width      = (uint16_t)w;
    out->height     = (uint16_t)h;
    out->alloc_size = pixel_bytes;
    out->ownership  = (pixel_bytes > 4096) ? ICON_PX_PMM : ICON_PX_HEAP;

    return 0;
}

/* ---- Public API ---- */

int ico_load(ico_file_t *ico, const char *path)
{
    struct vfs_node *f;
    uint32_t file_size;
    uint8_t *file_data;
    uint64_t frames;
    int32_t bytes_read;
    ico_header_t *hdr;
    ico_dir_entry_t *dir;
    int i, decoded;

    if (!ico || !path) return -1;

    memset(ico, 0, sizeof(ico_file_t));

    /* Open and read entire file */
    f = vfs_open(path, VFS_O_READ);
    if (!f) return -1;

    file_size = f->size;
    if (file_size < sizeof(ico_header_t) || file_size > 4 * 1024 * 1024) {
        vfs_close(f);
        return -1;
    }

    /* Allocate buffer for entire file via PMM */
    frames = (file_size + 4095) / 4096;
    file_data = (uint8_t *)pmm_alloc_contiguous(frames);
    if (!file_data) {
        vfs_close(f);
        return -1;
    }

    bytes_read = vfs_read(f, 0, file_size, file_data);
    vfs_close(f);

    if (bytes_read < (int32_t)sizeof(ico_header_t)) {
        pmm_free_range((uintptr_t)file_data, frames);
        return -1;
    }

    /* Parse header */
    hdr = (ico_header_t *)file_data;
    if (hdr->reserved != 0 || hdr->type != 1 || hdr->count == 0) {
        pmm_free_range((uintptr_t)file_data, frames);
        return -1;
    }

    /* Validate directory fits in file */
    uint32_t dir_end = sizeof(ico_header_t) +
                       (uint32_t)hdr->count * sizeof(ico_dir_entry_t);
    if (dir_end > file_size) {
        pmm_free_range((uintptr_t)file_data, frames);
        return -1;
    }

    dir = (ico_dir_entry_t *)(file_data + sizeof(ico_header_t));
    decoded = 0;

    for (i = 0; i < (int)hdr->count && decoded < ICO_MAX_ENTRIES; i++) {
        uint32_t entry_w = dir[i].width  ? dir[i].width  : 256;
        uint32_t entry_h = dir[i].height ? dir[i].height : 256;
        uint32_t offset  = dir[i].data_offset;
        uint32_t dsize   = dir[i].data_size;

        /* Validate offset + size within file */
        if (offset + dsize > file_size || dsize < 4)
            continue;

        const uint8_t *img_data = file_data + offset;

        /* Check for PNG magic */
        uint32_t magic = *(const uint32_t *)img_data;
        if (magic == PNG_MAGIC) {
            /* PNG data -- use image_load_mem() */
            image_t img;
            if (image_load_mem(&img, img_data, dsize) == 0) {
                ico->entries[decoded].bitmap.pixels     = img.pixels;
                ico->entries[decoded].bitmap.width      = (uint16_t)img.width;
                ico->entries[decoded].bitmap.height     = (uint16_t)img.height;
                ico->entries[decoded].bitmap.alloc_size = img.alloc_size;
                ico->entries[decoded].bitmap.ownership  =
                    img.from_pmm ? ICON_PX_PMM : ICON_PX_HEAP;
                ico->entries[decoded].valid = 1;
                decoded++;
            }
        } else {
            /* BMP DIB data */
            if (decode_bmp_dib(&ico->entries[decoded].bitmap,
                               img_data, dsize) == 0) {
                ico->entries[decoded].valid = 1;
                decoded++;
            }
        }

        (void)entry_w;
        (void)entry_h;
    }

    ico->count = decoded;

    /* Free the file buffer (decoded pixels are in separate allocations) */
    pmm_free_range((uintptr_t)file_data, frames);

    if (decoded == 0) return -1;

    printk("[OK] ICO loaded: %s (%d sizes)\n", path, decoded);
    return 0;
}

icon_bitmap_t *ico_get_best(const ico_file_t *ico, uint32_t target_size)
{
    int i;
    int best = -1;
    uint32_t best_diff = 0xFFFFFFFF;

    if (!ico) return (icon_bitmap_t *)0;

    for (i = 0; i < ico->count; i++) {
        if (!ico->entries[i].valid) continue;

        uint32_t w = ico->entries[i].bitmap.width;
        uint32_t diff = (w >= target_size) ? w - target_size
                                            : target_size - w;
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }

    if (best < 0) return (icon_bitmap_t *)0;

    /* Cast away const -- caller should not modify pixels */
    return (icon_bitmap_t *)&ico->entries[best].bitmap;
}

void ico_free(ico_file_t *ico)
{
    int i;

    if (!ico) return;

    for (i = 0; i < ico->count; i++) {
        if (!ico->entries[i].valid) continue;

        icon_bitmap_t *bmp = &ico->entries[i].bitmap;
        if (bmp->pixels && bmp->alloc_size > 0) {
            if (bmp->ownership == ICON_PX_PMM) {
                uint64_t frames = (bmp->alloc_size + 4095) / 4096;
                pmm_free_range((uintptr_t)bmp->pixels, frames);
            } else {
                kfree(bmp->pixels);
            }
        }
        bmp->pixels = (uint32_t *)0;
        ico->entries[i].valid = 0;
    }

    ico->count = 0;
}
