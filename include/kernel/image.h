/* ============================================================================
 * image.h — Runtime image decoding and scaling API
 *
 * Decodes JPEG, PNG, BMP, GIF, and TGA images at runtime using stb_image.
 * Images are decoded to BGRA (0xAARRGGBB) format matching gfx_color_t.
 *
 * Scaling uses bilinear interpolation (upscale / small downscale) or
 * box filtering (downscale >2x) for quality.
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

/* --- Fit modes for image_scale() --- */
typedef enum {
    IMAGE_FIT_STRETCH,  /* Distort to exact target size */
    IMAGE_FIT_FILL,     /* Scale to cover target, crop excess */
    IMAGE_FIT_FIT,      /* Scale to fit within target, letterbox */
    IMAGE_FIT_CENTER,   /* No scaling, center on canvas */
    IMAGE_FIT_TILE      /* Repeat pattern to fill canvas */
} image_fit_t;

/* Load an image from a VFS path (e.g. "C:\\Impossible\\Wallpapers\\bg.jpg").
 * Returns 0 on success, -1 on failure. */
int image_load(image_t *img, const char *path);

/* Load an image from a memory buffer.
 * Returns 0 on success, -1 on failure. */
int image_load_mem(image_t *img, const void *data, uint32_t size);

/* Free a decoded image (handles both PMM and kmalloc allocations). */
void image_free(image_t *img);

/* Scale an image to target dimensions using the specified fit mode.
 * Uses box filtering for >2x downscale, bilinear interpolation otherwise.
 * Allocates a new image in *dst (caller must image_free it).
 * Returns 0 on success, -1 on failure. */
int image_scale(image_t *dst, const image_t *src,
                uint32_t target_w, uint32_t target_h, image_fit_t mode);
