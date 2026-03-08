/* ============================================================================
 * gfx_gradient.c — Gradient fill operations
 *
 * Linear gradients (horizontal + vertical) and radial gradients rendered
 * scanline-by-scanline.  All math is integer-only — colors are interpolated
 * using fixed-point 8.8 arithmetic.
 *
 * Gradient struct stores start/end colors and direction.  Rounded-corner
 * gradients combine the gradient color interpolation with the isqrt-based
 * corner clipping from gfx_core.c.
 * ============================================================================ */

#include "gfx.h"
#include "kernel/types.h"

/* ---- Helpers ---- */

/* Integer square root (Newton's method) */
static uint32_t isqrt(uint32_t n)
{
    uint32_t x, x1;
    if (n == 0) return 0;
    x = n;
    x1 = (x + 1) / 2;
    while (x1 < x) {
        x = x1;
        x1 = (x + n / x) / 2;
    }
    return x;
}

/* Clamp int to [lo, hi] */
static inline int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Interpolate between two colors at position t (0–255).
 * t=0 → c0, t=255 → c1.  Integer-only linear interpolation. */
static gfx_color_t color_lerp(gfx_color_t c0, gfx_color_t c1, uint32_t t)
{
    uint32_t inv_t = 255 - t;

    uint32_t a0 = (c0 >> 24) & 0xFF, a1 = (c1 >> 24) & 0xFF;
    uint32_t r0 = (c0 >> 16) & 0xFF, r1 = (c1 >> 16) & 0xFF;
    uint32_t g0 = (c0 >> 8)  & 0xFF, g1 = (c1 >> 8)  & 0xFF;
    uint32_t b0 =  c0        & 0xFF, b1 =  c1        & 0xFF;

    uint32_t a = (a0 * inv_t + a1 * t) / 255;
    uint32_t r = (r0 * inv_t + r1 * t) / 255;
    uint32_t g = (g0 * inv_t + g1 * t) / 255;
    uint32_t b = (b0 * inv_t + b1 * t) / 255;

    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* ---- Linear gradient rectangle ---- */

void gfx_fill_gradient_rect(gfx_surface_t *s, int32_t x, int32_t y,
                             uint32_t w, uint32_t h,
                             const gfx_gradient_t *grad)
{
    int32_t x0, y0, x1, y1;
    int32_t row, col;

    if (w == 0 || h == 0) return;

    /* Clip to surface */
    x0 = clamp_i(x, 0, (int32_t)s->width);
    y0 = clamp_i(y, 0, (int32_t)s->height);
    x1 = clamp_i(x + (int32_t)w, 0, (int32_t)s->width);
    y1 = clamp_i(y + (int32_t)h, 0, (int32_t)s->height);

    if (x0 >= x1 || y0 >= y1) return;

    if (grad->direction == GFX_GRAD_HORIZONTAL) {
        /* Color varies along x */
        for (row = y0; row < y1; row++) {
            uint32_t *dp = s->pixels + (uint32_t)row * s->stride;
            for (col = x0; col < x1; col++) {
                uint32_t t = (uint32_t)(col - x) * 255 / (w > 1 ? w - 1 : 1);
                if (t > 255) t = 255;
                dp[col] = color_lerp(grad->start, grad->end, t);
            }
        }
    } else {
        /* GFX_GRAD_VERTICAL: color varies along y */
        for (row = y0; row < y1; row++) {
            uint32_t t = (uint32_t)(row - y) * 255 / (h > 1 ? h - 1 : 1);
            if (t > 255) t = 255;
            {
                gfx_color_t c = color_lerp(grad->start, grad->end, t);
                uint32_t *dp = s->pixels + (uint32_t)row * s->stride;
                for (col = x0; col < x1; col++)
                    dp[col] = c;
            }
        }
    }
}

/* ---- Gradient rounded rectangle ---- */

void gfx_fill_gradient_rounded(gfx_surface_t *s, int32_t x, int32_t y,
                                uint32_t w, uint32_t h, uint32_t radius,
                                const gfx_gradient_t *grad)
{
    int32_t row, col;
    uint32_t r;

    if (w == 0 || h == 0) return;

    r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (row = 0; row < (int32_t)h; row++) {
        int32_t fill_x0 = x;
        int32_t fill_x1 = x + (int32_t)w;

        /* Top corner rounding */
        if ((uint32_t)row < r) {
            uint32_t dy = r - (uint32_t)row;
            uint32_t dx_sq = (r * r > dy * dy) ? (r * r - dy * dy) : 0;
            uint32_t dx = isqrt(dx_sq);
            uint32_t inset = r - dx;
            fill_x0 = x + (int32_t)inset;
            fill_x1 = x + (int32_t)(w - inset);
        }
        /* Bottom corner rounding */
        else if ((uint32_t)row >= h - r) {
            uint32_t dy = (uint32_t)row - (h - r - 1);
            uint32_t dx_sq;
            uint32_t dx;
            uint32_t inset;
            if (dy > r) dy = r;
            dx_sq = (r * r > dy * dy) ? (r * r - dy * dy) : 0;
            dx = isqrt(dx_sq);
            inset = r - dx;
            fill_x0 = x + (int32_t)inset;
            fill_x1 = x + (int32_t)(w - inset);
        }

        /* Clip to surface */
        {
            int32_t cx0 = clamp_i(fill_x0, 0, (int32_t)s->width);
            int32_t cy  = y + row;
            int32_t cx1 = clamp_i(fill_x1, 0, (int32_t)s->width);

            if (cy < 0 || cy >= (int32_t)s->height || cx0 >= cx1)
                continue;

            {
                uint32_t *dp = s->pixels + (uint32_t)cy * s->stride;

                if (grad->direction == GFX_GRAD_HORIZONTAL) {
                    for (col = cx0; col < cx1; col++) {
                        uint32_t t = (uint32_t)(col - x) * 255 / (w > 1 ? w - 1 : 1);
                        if (t > 255) t = 255;
                        dp[col] = color_lerp(grad->start, grad->end, t);
                    }
                } else {
                    uint32_t t = (uint32_t)row * 255 / (h > 1 ? h - 1 : 1);
                    if (t > 255) t = 255;
                    {
                        gfx_color_t c = color_lerp(grad->start, grad->end, t);
                        for (col = cx0; col < cx1; col++)
                            dp[col] = c;
                    }
                }
            }
        }
    }
}

/* ---- Radial gradient fill ---- */

void gfx_fill_radial_gradient(gfx_surface_t *s, int32_t cx, int32_t cy,
                               uint32_t radius,
                               gfx_color_t center_color,
                               gfx_color_t edge_color)
{
    int32_t x0, y0, x1, y1;
    int32_t row, col;
    int32_t r = (int32_t)radius;

    if (radius == 0) return;

    /* Bounding box of the circle */
    x0 = clamp_i(cx - r, 0, (int32_t)s->width);
    y0 = clamp_i(cy - r, 0, (int32_t)s->height);
    x1 = clamp_i(cx + r + 1, 0, (int32_t)s->width);
    y1 = clamp_i(cy + r + 1, 0, (int32_t)s->height);

    for (row = y0; row < y1; row++) {
        uint32_t *dp = s->pixels + (uint32_t)row * s->stride;
        int32_t dy = row - cy;

        for (col = x0; col < x1; col++) {
            int32_t dx_val = col - cx;
            uint32_t dist_sq = (uint32_t)(dx_val * dx_val + dy * dy);
            uint32_t dist = isqrt(dist_sq);

            if (dist <= radius) {
                uint32_t t = dist * 255 / radius;
                if (t > 255) t = 255;
                dp[col] = color_lerp(center_color, edge_color, t);
            }
        }
    }
}
