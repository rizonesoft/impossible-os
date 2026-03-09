/* ============================================================================
 * image.h — Runtime image decoding API
 *
 * Decodes JPEG, PNG, BMP, GIF, and TGA images at runtime using stb_image.
 * Images are decoded to BGRA (0xAARRGGBB) format matching gfx_color_t.
 *
 * Memory: Large pixel buffers (>64 KB) use PMM (pmm_alloc_contiguous),
 * small allocations use kmalloc. image_free() handles both cases.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef struct {
    uint32_t *pixels;      /* BGRA pixel data (gfx_color_t compatible) */
    uint32_t  width;       /* Image width in pixels */
    uint32_t  height;      /* Image height in pixels */
    uint32_t  alloc_size;  /* Total allocation size in bytes */
    int       from_pmm;    /* 1 = pixels allocated via PMM, 0 = kmalloc */
} image_t;

/* Load an image from a VFS path (e.g. "C:\\Impossible\\Wallpapers\\bg.jpg").
 * Returns 0 on success, -1 on failure. */
int image_load(image_t *img, const char *path);

/* Load an image from a memory buffer.
 * Returns 0 on success, -1 on failure. */
int image_load_mem(image_t *img, const void *data, uint32_t size);

/* Free a decoded image (handles both PMM and kmalloc allocations). */
void image_free(image_t *img);
