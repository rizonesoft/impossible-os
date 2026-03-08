/* ============================================================================
 * gfx_blend.c — Alpha blending and compositing
 *
 * Per-pixel alpha blitting, global-alpha blit, and alpha-filled rectangles.
 *
 * Blending uses **pre-multiplied alpha** for the hot path:
 *   out = src + dst * (1 - src_a)
 * This avoids one multiply per channel vs. straight alpha (50% fewer muls).
 *
 * All arithmetic is integer-only (no floating point).  Division by 255 is
 * approximated with the fast formula: (x + 1 + (x >> 8)) >> 8.
 * ============================================================================ */

#include "gfx.h"
#include "kernel/types.h"

/* ---- Fast divide-by-255 ---- */

/* Approximate n/255 using integer math.
 * Exact for all n in [0, 65535].  Much faster than actual division. */
static inline uint32_t div255(uint32_t n)
{
    return (n + 1 + (n >> 8)) >> 8;
}

/* ---- Clamp helper ---- */

static inline int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ---- Per-pixel alpha blit ---- */

void gfx_blit(gfx_surface_t *dst, int32_t dx, int32_t dy,
              const gfx_surface_t *src, int32_t sx, int32_t sy,
              uint32_t w, uint32_t h)
{
    int32_t src_x0, src_y0, dst_x0, dst_y0;
    int32_t copy_w, copy_h;
    int32_t row;

    /* Clip source region to source surface */
    src_x0 = clamp_i(sx, 0, (int32_t)src->width);
    src_y0 = clamp_i(sy, 0, (int32_t)src->height);

    copy_w = (int32_t)w;
    copy_h = (int32_t)h;

    /* Adjust for source clipping */
    if (sx < 0) { dx -= sx; copy_w += sx; src_x0 = 0; }
    if (sy < 0) { dy -= sy; copy_h += sy; src_y0 = 0; }
    if (src_x0 + copy_w > (int32_t)src->width)
        copy_w = (int32_t)src->width - src_x0;
    if (src_y0 + copy_h > (int32_t)src->height)
        copy_h = (int32_t)src->height - src_y0;

    /* Clip destination region */
    dst_x0 = dx;
    dst_y0 = dy;

    if (dst_x0 < 0) { src_x0 -= dst_x0; copy_w += dst_x0; dst_x0 = 0; }
    if (dst_y0 < 0) { src_y0 -= dst_y0; copy_h += dst_y0; dst_y0 = 0; }
    if (dst_x0 + copy_w > (int32_t)dst->width)
        copy_w = (int32_t)dst->width - dst_x0;
    if (dst_y0 + copy_h > (int32_t)dst->height)
        copy_h = (int32_t)dst->height - dst_y0;

    if (copy_w <= 0 || copy_h <= 0)
        return;

    /* Blit with per-pixel alpha blending (pre-multiplied alpha path) */
    for (row = 0; row < copy_h; row++) {
        const uint32_t *sp = src->pixels +
            (uint32_t)(src_y0 + row) * src->stride + (uint32_t)src_x0;
        uint32_t *dp = dst->pixels +
            (uint32_t)(dst_y0 + row) * dst->stride + (uint32_t)dst_x0;
        int32_t col;

        for (col = 0; col < copy_w; col++) {
            uint32_t s = sp[col];
            uint32_t sa = (s >> 24) & 0xFF;

            if (sa == 0xFF) {
                /* Fully opaque — direct copy (fast path) */
                dp[col] = s;
            } else if (sa == 0x00) {
                /* Fully transparent — skip */
            } else {
                /* Alpha blend: out = src + dst * (1 - src_a) */
                uint32_t d = dp[col];
                uint32_t inv_a = 255 - sa;

                uint32_t sr = (s >> 16) & 0xFF;
                uint32_t sg = (s >> 8)  & 0xFF;
                uint32_t sb =  s        & 0xFF;

                uint32_t dr = (d >> 16) & 0xFF;
                uint32_t dg = (d >> 8)  & 0xFF;
                uint32_t db =  d        & 0xFF;
                uint32_t da = (d >> 24) & 0xFF;

                /* Pre-multiplied alpha blend */
                uint32_t or_ = sr + div255(dr * inv_a);
                uint32_t og  = sg + div255(dg * inv_a);
                uint32_t ob  = sb + div255(db * inv_a);
                uint32_t oa  = sa + div255(da * inv_a);

                /* Clamp to 255 (shouldn't exceed if src is truly pre-mult) */
                if (or_ > 255) or_ = 255;
                if (og  > 255) og  = 255;
                if (ob  > 255) ob  = 255;
                if (oa  > 255) oa  = 255;

                dp[col] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
            }
        }
    }
}

/* ---- Blit with global alpha ---- */

void gfx_blit_alpha(gfx_surface_t *dst, int32_t dx, int32_t dy,
                    const gfx_surface_t *src, uint8_t alpha)
{
    int32_t dst_x0, dst_y0;
    int32_t copy_w, copy_h;
    int32_t row;

    if (alpha == 0)
        return;  /* Completely invisible */

    /* Clip to destination */
    dst_x0 = dx;
    dst_y0 = dy;
    copy_w = (int32_t)src->width;
    copy_h = (int32_t)src->height;

    if (dst_x0 < 0) { copy_w += dst_x0; dst_x0 = 0; }
    if (dst_y0 < 0) { copy_h += dst_y0; dst_y0 = 0; }
    if (dst_x0 + copy_w > (int32_t)dst->width)
        copy_w = (int32_t)dst->width - dst_x0;
    if (dst_y0 + copy_h > (int32_t)dst->height)
        copy_h = (int32_t)dst->height - dst_y0;

    if (copy_w <= 0 || copy_h <= 0)
        return;

    for (row = 0; row < copy_h; row++) {
        int32_t src_row = row + (dy < 0 ? -dy : 0);
        int32_t src_col_start = (dx < 0 ? -dx : 0);
        const uint32_t *sp = src->pixels +
            (uint32_t)src_row * src->stride + (uint32_t)src_col_start;
        uint32_t *dp = dst->pixels +
            (uint32_t)(dst_y0 + row) * dst->stride + (uint32_t)dst_x0;
        int32_t col;

        if (alpha == 0xFF) {
            /* Global alpha is opaque — use per-pixel alpha only */
            for (col = 0; col < copy_w; col++) {
                uint32_t s = sp[col];
                uint32_t sa = (s >> 24) & 0xFF;

                if (sa == 0xFF) {
                    dp[col] = s;
                } else if (sa > 0) {
                    uint32_t d = dp[col];
                    uint32_t inv_a = 255 - sa;
                    uint32_t or_ = ((s >> 16) & 0xFF) + div255(((d >> 16) & 0xFF) * inv_a);
                    uint32_t og  = ((s >> 8)  & 0xFF) + div255(((d >> 8)  & 0xFF) * inv_a);
                    uint32_t ob  = ( s        & 0xFF) + div255(( d        & 0xFF) * inv_a);
                    uint32_t oa  = sa + div255(((d >> 24) & 0xFF) * inv_a);
                    if (or_ > 255) or_ = 255;
                    if (og  > 255) og  = 255;
                    if (ob  > 255) ob  = 255;
                    if (oa  > 255) oa  = 255;
                    dp[col] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
                }
            }
        } else {
            /* Multiply src alpha by global alpha */
            for (col = 0; col < copy_w; col++) {
                uint32_t s = sp[col];
                uint32_t sa = (s >> 24) & 0xFF;

                /* Combined alpha = pixel_alpha * global_alpha / 255 */
                uint32_t combined_a = div255(sa * (uint32_t)alpha);

                if (combined_a == 0)
                    continue;

                {
                    uint32_t d = dp[col];
                    uint32_t inv_a = 255 - combined_a;

                    /* Scale source channels by combined alpha ratio */
                    uint32_t sr = div255(((s >> 16) & 0xFF) * combined_a);
                    uint32_t sg = div255(((s >> 8)  & 0xFF) * combined_a);
                    uint32_t sb = div255(( s        & 0xFF) * combined_a);

                    uint32_t or_ = sr + div255(((d >> 16) & 0xFF) * inv_a);
                    uint32_t og  = sg + div255(((d >> 8)  & 0xFF) * inv_a);
                    uint32_t ob  = sb + div255(( d        & 0xFF) * inv_a);
                    uint32_t oa  = combined_a + div255(((d >> 24) & 0xFF) * inv_a);

                    if (or_ > 255) or_ = 255;
                    if (og  > 255) og  = 255;
                    if (ob  > 255) ob  = 255;
                    if (oa  > 255) oa  = 255;

                    dp[col] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
                }
            }
        }
    }
}

/* ---- Alpha-filled rectangle ---- */

void gfx_fill_rect_alpha(gfx_surface_t *s, int32_t x, int32_t y,
                          uint32_t w, uint32_t h, gfx_color_t color)
{
    int32_t x0, y0, x1, y1;
    int32_t row, col;
    uint32_t sa, sr, sg, sb;
    uint32_t inv_a;

    sa = GFX_ALPHA(color);
    if (sa == 0)
        return;  /* Transparent — nothing to draw */

    if (sa == 0xFF) {
        /* Fully opaque — use fast solid fill */
        gfx_fill_rect(s, x, y, w, h, color);
        return;
    }

    /* Pre-multiply the source color channels */
    sr = div255(GFX_RED(color)   * sa);
    sg = div255(GFX_GREEN(color) * sa);
    sb = div255(GFX_BLUE(color)  * sa);
    inv_a = 255 - sa;

    /* Clip to surface */
    x0 = clamp_i(x, 0, (int32_t)s->width);
    y0 = clamp_i(y, 0, (int32_t)s->height);
    x1 = clamp_i(x + (int32_t)w, 0, (int32_t)s->width);
    y1 = clamp_i(y + (int32_t)h, 0, (int32_t)s->height);

    if (x0 >= x1 || y0 >= y1)
        return;

    /* Blend each pixel: out = premult_src + dst * (1 - src_a) */
    for (row = y0; row < y1; row++) {
        uint32_t *dp = s->pixels + (uint32_t)row * s->stride;
        for (col = x0; col < x1; col++) {
            uint32_t d = dp[col];
            uint32_t dr = (d >> 16) & 0xFF;
            uint32_t dg = (d >> 8)  & 0xFF;
            uint32_t db =  d        & 0xFF;
            uint32_t da = (d >> 24) & 0xFF;

            uint32_t or_ = sr + div255(dr * inv_a);
            uint32_t og  = sg + div255(dg * inv_a);
            uint32_t ob  = sb + div255(db * inv_a);
            uint32_t oa  = sa + div255(da * inv_a);

            dp[col] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
        }
    }
}
