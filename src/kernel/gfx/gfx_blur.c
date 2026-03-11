/* ============================================================================
 * gfx_blur.c — Box blur (2-pass, O(n) per pixel)
 *
 * Implements a separable box blur: one horizontal pass and one vertical pass.
 * Each pass runs in O(width) or O(height) per scanline using a running sum,
 * making the total cost O(w*h) regardless of blur radius.
 *
 * Operates in-place on a region of a gfx_surface_t.  Uses a temporary
 * scanline buffer from the heap.
 * ============================================================================ */

#include "gfx.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"
#include "kernel/types.h"

/* ---- Helpers ---- */

static inline int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint32_t min_u(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/* ---- 2-pass box blur ---- */

void gfx_blur_rect(gfx_surface_t *s, int32_t x, int32_t y,
                    uint32_t w, uint32_t h, uint32_t radius)
{
    int32_t x0, y0, x1, y1;
    int32_t row, col;
    uint32_t *tmp;
    uint32_t region_w, region_h;
    uint32_t kernel;

    if (radius == 0 || w == 0 || h == 0)
        return;

    /* Clip to surface */
    x0 = clamp_i(x, 0, (int32_t)s->width);
    y0 = clamp_i(y, 0, (int32_t)s->height);
    x1 = clamp_i(x + (int32_t)w, 0, (int32_t)s->width);
    y1 = clamp_i(y + (int32_t)h, 0, (int32_t)s->height);

    if (x0 >= x1 || y0 >= y1)
        return;

    region_w = (uint32_t)(x1 - x0);
    region_h = (uint32_t)(y1 - y0);
    kernel = 2 * radius + 1;

    /* Allocate temporary buffer for one scanline (max dimension) */
    {
        uint32_t max_dim = region_w > region_h ? region_w : region_h;
        tmp = (uint32_t *)kmalloc(max_dim * sizeof(uint32_t));
        if (!tmp) return;
    }

    /* --- Pass 1: Horizontal blur --- */
    for (row = y0; row < y1; row++) {
        uint32_t *dp = s->pixels + (uint32_t)row * s->stride;

        /* Running sum for each channel */
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
        uint32_t count = 0;

        /* Initialize the window: [x0, x0 + radius] */
        {
            int32_t i;
            int32_t win_end = (int32_t)min_u((uint32_t)(x0 + (int32_t)radius),
                                              (uint32_t)(x1 - 1));
            for (i = x0; i <= win_end; i++) {
                uint32_t p = dp[i];
                sum_a += (p >> 24) & 0xFF;
                sum_r += (p >> 16) & 0xFF;
                sum_g += (p >> 8)  & 0xFF;
                sum_b +=  p        & 0xFF;
                count++;
            }
        }

        for (col = x0; col < x1; col++) {
            /* Add the pixel entering the right side of the window */
            {
                int32_t add_col = col + (int32_t)radius;
                if (add_col >= x0 && add_col < x1 && col > x0) {
                    uint32_t p = dp[add_col];
                    sum_a += (p >> 24) & 0xFF;
                    sum_r += (p >> 16) & 0xFF;
                    sum_g += (p >> 8)  & 0xFF;
                    sum_b +=  p        & 0xFF;
                    count++;
                }
            }

            /* Store averaged pixel */
            if (count > 0) {
                tmp[col - x0] = ((sum_a / count) << 24) |
                                ((sum_r / count) << 16) |
                                ((sum_g / count) << 8)  |
                                 (sum_b / count);
            }

            /* Remove the pixel leaving the left side of the window */
            {
                int32_t rem_col = col - (int32_t)radius;
                if (rem_col >= x0 && rem_col < x1) {
                    uint32_t p = dp[rem_col];
                    sum_a -= (p >> 24) & 0xFF;
                    sum_r -= (p >> 16) & 0xFF;
                    sum_g -= (p >> 8)  & 0xFF;
                    sum_b -=  p        & 0xFF;
                    if (count > 0) count--;
                }
            }
        }

        /* Write back horizontal result */
        {
            uint32_t i;
            for (i = 0; i < region_w; i++)
                dp[x0 + (int32_t)i] = tmp[i];
        }
    }

    /* --- Pass 2: Vertical blur --- */
    for (col = x0; col < x1; col++) {
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
        uint32_t count = 0;

        /* Initialize vertical window */
        {
            int32_t i;
            int32_t win_end = (int32_t)min_u((uint32_t)(y0 + (int32_t)radius),
                                              (uint32_t)(y1 - 1));
            for (i = y0; i <= win_end; i++) {
                uint32_t p = s->pixels[(uint32_t)i * s->stride + (uint32_t)col];
                sum_a += (p >> 24) & 0xFF;
                sum_r += (p >> 16) & 0xFF;
                sum_g += (p >> 8)  & 0xFF;
                sum_b +=  p        & 0xFF;
                count++;
            }
        }

        for (row = y0; row < y1; row++) {
            {
                int32_t add_row = row + (int32_t)radius;
                if (add_row >= y0 && add_row < y1 && row > y0) {
                    uint32_t p = s->pixels[(uint32_t)add_row * s->stride + (uint32_t)col];
                    sum_a += (p >> 24) & 0xFF;
                    sum_r += (p >> 16) & 0xFF;
                    sum_g += (p >> 8)  & 0xFF;
                    sum_b +=  p        & 0xFF;
                    count++;
                }
            }

            if (count > 0) {
                tmp[row - y0] = ((sum_a / count) << 24) |
                                ((sum_r / count) << 16) |
                                ((sum_g / count) << 8)  |
                                 (sum_b / count);
            }

            {
                int32_t rem_row = row - (int32_t)radius;
                if (rem_row >= y0 && rem_row < y1) {
                    uint32_t p = s->pixels[(uint32_t)rem_row * s->stride + (uint32_t)col];
                    sum_a -= (p >> 24) & 0xFF;
                    sum_r -= (p >> 16) & 0xFF;
                    sum_g -= (p >> 8)  & 0xFF;
                    sum_b -=  p        & 0xFF;
                    if (count > 0) count--;
                }
            }
        }

        /* Write back vertical result */
        {
            uint32_t i;
            for (i = 0; i < region_h; i++)
                s->pixels[(uint32_t)(y0 + (int32_t)i) * s->stride + (uint32_t)col] = tmp[i];
        }
    }

    kfree(tmp);
    (void)kernel;
}
