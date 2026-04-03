/* ============================================================================
 * image_scale.c -- Image scaling with bilinear interpolation & box filtering
 *
 * Scaling strategy:
 *   - Upscale or downscale ≤2x: bilinear interpolation (smooth)
 *   - Downscale >2x: box filter (averages source pixels -> sharper results)
 *
 * Fit modes:
 *   STRETCH -- distort to fill exact target dimensions
 *   FILL    -- scale to cover target, center-crop excess
 *   FIT     -- scale to fit inside target, letterbox with black
 *   CENTER  -- no scaling, center source on target canvas
 *   TILE    -- repeat source to fill target
 *
 * All math is integer-only (fixed-point 16.16).
 * Memory: uses the same tiered allocator as image.c (PMM for > 64 KB).
 * ============================================================================ */

#include "kernel/image.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/types.h"

/* ---- Fixed-point 16.16 helpers ---------------------------------------- */

#define FP_SHIFT  16
#define FP_ONE    (1u << FP_SHIFT)
#define FP_HALF   (1u << (FP_SHIFT - 1))

/* ---- Pixel helpers ---------------------------------------------------- */

/* Extract BGRA channels from a packed 0xAARRGGBB pixel */
static inline uint8_t px_b(uint32_t p) { return (uint8_t)(p);       }
static inline uint8_t px_g(uint32_t p) { return (uint8_t)(p >> 8);  }
static inline uint8_t px_r(uint32_t p) { return (uint8_t)(p >> 16); }
static inline uint8_t px_a(uint32_t p) { return (uint8_t)(p >> 24); }

static inline uint32_t px_pack(uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    return (uint32_t)b | ((uint32_t)g << 8) |
           ((uint32_t)r << 16) | ((uint32_t)a << 24);
}

/* Fetch pixel with clamping (handles edge pixels) */
static inline uint32_t img_pixel(const image_t *img, int32_t x, int32_t y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= img->width)  x = (int32_t)(img->width - 1);
    if ((uint32_t)y >= img->height) y = (int32_t)(img->height - 1);
    return img->pixels[y * img->width + x];
}

/* ---- Allocate output image -------------------------------------------- */

static int alloc_image(image_t *img, uint32_t w, uint32_t h)
{
    uint32_t size = w * h * 4;
    img->width  = w;
    img->height = h;
    img->alloc_size = size;

    if (size > 64 * 1024) {
        /* Large -- use PMM */
        uint64_t frames = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        if (phys == 0) {
            klog(LOG_ERROR, "IMG", "scale: PMM alloc failed for %u bytes", size);
            return -1;
        }
        img->pixels = (uint32_t *)phys;
        img->from_pmm = 1;
    } else {
        img->pixels = (uint32_t *)kmalloc(size);
        if (!img->pixels) {
            klog(LOG_ERROR, "IMG", "scale: kmalloc failed for %u bytes", size);
            return -1;
        }
        img->from_pmm = 0;
    }
    return 0;
}

/* Clear image to black (0x00000000) */
static void clear_image(image_t *img)
{
    uint32_t count = img->width * img->height;
    uint32_t i;
    for (i = 0; i < count; i++)
        img->pixels[i] = 0xFF000000;  /* opaque black */
}

/* ---- Bilinear interpolation ------------------------------------------- */

/* Scale src into dst using bilinear interpolation.
 * src_x, src_y, src_w, src_h define the source region (in source pixels).
 * Output fills the entire dst image. */
static void scale_bilinear(image_t *dst, const image_t *src,
                           int32_t src_x, int32_t src_y,
                           uint32_t src_w, uint32_t src_h)
{
    uint32_t dx, dy;
    /* Fixed-point step: how much source we advance per destination pixel */
    uint32_t x_step = (src_w * FP_ONE) / dst->width;
    uint32_t y_step = (src_h * FP_ONE) / dst->height;

    for (dy = 0; dy < dst->height; dy++) {
        uint32_t sy_fp = dy * y_step;
        int32_t sy = (int32_t)(sy_fp >> FP_SHIFT) + src_y;
        uint32_t fy = sy_fp & (FP_ONE - 1);  /* fractional part */

        for (dx = 0; dx < dst->width; dx++) {
            uint32_t sx_fp = dx * x_step;
            int32_t sx = (int32_t)(sx_fp >> FP_SHIFT) + src_x;
            uint32_t fx = sx_fp & (FP_ONE - 1);

            /* Fast path: exact pixel boundary (no interpolation needed).
             * Also avoids uint32_t overflow: (65536 * 65536) = 2^32 -> 0. */
            if (fx == 0 && fy == 0) {
                dst->pixels[dy * dst->width + dx] = img_pixel(src, sx, sy);
                continue;
            }

            /* Sample 4 pixels */
            {
                uint32_t p00 = img_pixel(src, sx,     sy);
                uint32_t p10 = img_pixel(src, sx + 1, sy);
                uint32_t p01 = img_pixel(src, sx,     sy + 1);
                uint32_t p11 = img_pixel(src, sx + 1, sy + 1);

                /* Bilinear weights (use uint64_t to prevent overflow) */
                uint32_t ifx = FP_ONE - fx;
                uint32_t ify = FP_ONE - fy;
                uint32_t w00 = (uint32_t)(((uint64_t)ifx * ify) >> FP_SHIFT);
                uint32_t w10 = (uint32_t)(((uint64_t)fx  * ify) >> FP_SHIFT);
                uint32_t w01 = (uint32_t)(((uint64_t)ifx * fy)  >> FP_SHIFT);
                uint32_t w11 = (uint32_t)(((uint64_t)fx  * fy)  >> FP_SHIFT);

                /* Interpolate each channel */
                uint8_t b = (uint8_t)((px_b(p00) * w00 + px_b(p10) * w10 +
                                       px_b(p01) * w01 + px_b(p11) * w11) >> FP_SHIFT);
                uint8_t g = (uint8_t)((px_g(p00) * w00 + px_g(p10) * w10 +
                                       px_g(p01) * w01 + px_g(p11) * w11) >> FP_SHIFT);
                uint8_t r = (uint8_t)((px_r(p00) * w00 + px_r(p10) * w10 +
                                       px_r(p01) * w01 + px_r(p11) * w11) >> FP_SHIFT);
                uint8_t a = (uint8_t)((px_a(p00) * w00 + px_a(p10) * w10 +
                                       px_a(p01) * w01 + px_a(p11) * w11) >> FP_SHIFT);

                dst->pixels[dy * dst->width + dx] = px_pack(b, g, r, a);
            }
        }
    }
}

/* ---- Box filter downscaling ------------------------------------------- */

/* Average all source pixels that map to each destination pixel.
 * Produces sharper results than bilinear for >2x downscaling. */
static void scale_box(image_t *dst, const image_t *src,
                      int32_t src_x, int32_t src_y,
                      uint32_t src_w, uint32_t src_h)
{
    uint32_t dx, dy;

    for (dy = 0; dy < dst->height; dy++) {
        /* Source row range for this destination row */
        uint32_t sy0 = (dy * src_h) / dst->height;
        uint32_t sy1 = ((dy + 1) * src_h) / dst->height;
        if (sy1 == sy0) sy1 = sy0 + 1;

        for (dx = 0; dx < dst->width; dx++) {
            /* Source column range for this destination column */
            uint32_t sx0 = (dx * src_w) / dst->width;
            uint32_t sx1 = ((dx + 1) * src_w) / dst->width;
            if (sx1 == sx0) sx1 = sx0 + 1;

            /* Accumulate all source pixels in this box */
            uint32_t acc_b = 0, acc_g = 0, acc_r = 0, acc_a = 0;
            uint32_t count = 0;
            uint32_t sy, sx;

            for (sy = sy0; sy < sy1; sy++) {
                for (sx = sx0; sx < sx1; sx++) {
                    uint32_t p = img_pixel(src,
                                           (int32_t)(sx + (uint32_t)src_x),
                                           (int32_t)(sy + (uint32_t)src_y));
                    acc_b += px_b(p);
                    acc_g += px_g(p);
                    acc_r += px_r(p);
                    acc_a += px_a(p);
                    count++;
                }
            }

            if (count > 0) {
                dst->pixels[dy * dst->width + dx] = px_pack(
                    (uint8_t)(acc_b / count),
                    (uint8_t)(acc_g / count),
                    (uint8_t)(acc_r / count),
                    (uint8_t)(acc_a / count)
                );
            } else {
                dst->pixels[dy * dst->width + dx] = 0xFF000000;
            }
        }
    }
}

/* ---- Adaptive scaler: pick bilinear or box filter --------------------- */

static void scale_region(image_t *dst, const image_t *src,
                         int32_t src_x, int32_t src_y,
                         uint32_t src_w, uint32_t src_h)
{
    /* Use box filter if downscaling by more than 2x in either dimension */
    int box_x = (src_w > dst->width  * 2);
    int box_y = (src_h > dst->height * 2);

    if (box_x || box_y) {
        scale_box(dst, src, src_x, src_y, src_w, src_h);
    } else {
        scale_bilinear(dst, src, src_x, src_y, src_w, src_h);
    }
}

/* ---- Fit mode implementations ----------------------------------------- */

/* STRETCH: distort to fill exact target dimensions */
static int fit_stretch(image_t *dst, const image_t *src,
                       uint32_t tw, uint32_t th)
{
    if (alloc_image(dst, tw, th) < 0) return -1;
    scale_region(dst, src, 0, 0, src->width, src->height);
    return 0;
}

/* FILL: scale to cover target, center-crop excess */
static int fit_fill(image_t *dst, const image_t *src,
                    uint32_t tw, uint32_t th)
{
    uint32_t crop_w, crop_h;
    int32_t crop_x, crop_y;

    if (alloc_image(dst, tw, th) < 0) return -1;

    /* Scale factor: pick the larger of w-scale and h-scale */
    /* We want to find what region of source maps to target
     * such that the source completely covers the target. */
    /* ratio: target/source per axis (fixed-point 16.16) */
    uint32_t rx = (tw * FP_ONE) / src->width;   /* x scale */
    uint32_t ry = (th * FP_ONE) / src->height;  /* y scale */

    if (rx >= ry) {
        /* Width-limited: use full source width, crop height */
        crop_w = src->width;
        crop_h = (th * src->width) / tw;
        if (crop_h > src->height) crop_h = src->height;
        crop_x = 0;
        crop_y = (int32_t)(src->height - crop_h) / 2;
    } else {
        /* Height-limited: use full source height, crop width */
        crop_h = src->height;
        crop_w = (tw * src->height) / th;
        if (crop_w > src->width) crop_w = src->width;
        crop_y = 0;
        crop_x = (int32_t)(src->width - crop_w) / 2;
    }

    scale_region(dst, src, crop_x, crop_y, crop_w, crop_h);
    return 0;
}

/* FIT: scale to fit within target, letterbox with black */
static int fit_fit(image_t *dst, const image_t *src,
                   uint32_t tw, uint32_t th)
{
    uint32_t scaled_w, scaled_h;
    uint32_t off_x, off_y;
    uint32_t dy, dx;
    image_t tmp;

    if (alloc_image(dst, tw, th) < 0) return -1;
    clear_image(dst);  /* black letterbox */

    /* Scale factor: pick the smaller of w-scale and h-scale */
    uint32_t rx = (tw * FP_ONE) / src->width;
    uint32_t ry = (th * FP_ONE) / src->height;

    if (rx <= ry) {
        scaled_w = tw;
        scaled_h = (src->height * tw) / src->width;
        if (scaled_h > th) scaled_h = th;
    } else {
        scaled_h = th;
        scaled_w = (src->width * th) / src->height;
        if (scaled_w > tw) scaled_w = tw;
    }

    /* Scale into temp buffer */
    if (alloc_image(&tmp, scaled_w, scaled_h) < 0) return 0;  /* partial success */
    scale_region(&tmp, src, 0, 0, src->width, src->height);

    /* Center onto destination */
    off_x = (tw - scaled_w) / 2;
    off_y = (th - scaled_h) / 2;

    for (dy = 0; dy < scaled_h; dy++) {
        for (dx = 0; dx < scaled_w; dx++) {
            dst->pixels[(off_y + dy) * tw + (off_x + dx)] =
                tmp.pixels[dy * scaled_w + dx];
        }
    }

    image_free(&tmp);
    return 0;
}

/* CENTER: no scaling, center on canvas with black fill */
static int fit_center(image_t *dst, const image_t *src,
                      uint32_t tw, uint32_t th)
{
    int32_t off_x, off_y;
    uint32_t copy_w, copy_h;
    uint32_t src_x, src_y;
    uint32_t dy, dx;

    if (alloc_image(dst, tw, th) < 0) return -1;
    clear_image(dst);

    /* Compute centering offsets */
    off_x = ((int32_t)tw - (int32_t)src->width)  / 2;
    off_y = ((int32_t)th - (int32_t)src->height) / 2;

    /* Source/dest coordinates for the overlap region */
    src_x = (off_x < 0) ? (uint32_t)(-off_x) : 0;
    src_y = (off_y < 0) ? (uint32_t)(-off_y) : 0;
    copy_w = src->width - src_x;
    copy_h = src->height - src_y;

    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    if ((uint32_t)off_x + copy_w > tw) copy_w = tw - (uint32_t)off_x;
    if ((uint32_t)off_y + copy_h > th) copy_h = th - (uint32_t)off_y;

    for (dy = 0; dy < copy_h; dy++) {
        for (dx = 0; dx < copy_w; dx++) {
            dst->pixels[((uint32_t)off_y + dy) * tw + ((uint32_t)off_x + dx)] =
                src->pixels[(src_y + dy) * src->width + (src_x + dx)];
        }
    }

    return 0;
}

/* TILE: repeat source pattern to fill target */
static int fit_tile(image_t *dst, const image_t *src,
                    uint32_t tw, uint32_t th)
{
    uint32_t dy, dx;

    if (alloc_image(dst, tw, th) < 0) return -1;

    for (dy = 0; dy < th; dy++) {
        uint32_t sy = dy % src->height;
        for (dx = 0; dx < tw; dx++) {
            uint32_t sx = dx % src->width;
            dst->pixels[dy * tw + dx] =
                src->pixels[sy * src->width + sx];
        }
    }

    return 0;
}

/* ---- Public API ------------------------------------------------------- */

int image_scale(image_t *dst, const image_t *src,
                uint32_t target_w, uint32_t target_h, image_fit_t mode)
{
    int result;

    if (!dst || !src || !src->pixels || target_w == 0 || target_h == 0)
        return -1;

    /* Zero-init output */
    dst->pixels = (uint32_t *)0;
    dst->width = 0;
    dst->height = 0;
    dst->alloc_size = 0;
    dst->from_pmm = 0;

    switch (mode) {
    case IMAGE_FIT_STRETCH: result = fit_stretch(dst, src, target_w, target_h); break;
    case IMAGE_FIT_FILL:    result = fit_fill(dst, src, target_w, target_h);    break;
    case IMAGE_FIT_FIT:     result = fit_fit(dst, src, target_w, target_h);     break;
    case IMAGE_FIT_CENTER:  result = fit_center(dst, src, target_w, target_h);  break;
    case IMAGE_FIT_TILE:    result = fit_tile(dst, src, target_w, target_h);    break;
    default:
        klog(LOG_ERROR, "IMG", "scale: unknown fit mode %d", (int)mode);
        return -1;
    }

    if (result == 0) {
        klog(LOG_INFO, "IMG", "Scaled %ux%u -> %ux%u (mode %d, %s)",
               src->width, src->height, dst->width, dst->height,
               (int)mode, dst->from_pmm ? "PMM" : "heap");
    }

    return result;
}
