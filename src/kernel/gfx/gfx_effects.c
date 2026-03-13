/* ============================================================================
 * gfx_effects.c — Material effects: Acrylic, Mica, Drop Shadow, Reveal
 *
 * Windows 11-style material effects built on top of gfx_blur:
 *
 *   Acrylic:  blur + noise + tint overlay   (taskbar, start menu, menus)
 *   Mica:     wallpaper sample + desaturate + tint  (title bars)
 *   Shadow:   multi-layer soft shadow with blur
 *   Reveal:   radial glow following cursor position
 *
 * All use integer-only math, no floating point.
 * ============================================================================ */

#include "gfx.h"
#include "kernel/mm/heap.h"
#include "kernel/types.h"

/* ---- Fast div255 ---- */
static inline uint32_t div255(uint32_t n)
{
    return (n + 1 + (n >> 8)) >> 8;
}

/* Simple pseudo-random for noise (xorshift32) */
static uint32_t xorshift_state = 0xDEADBEEF;

static uint32_t xorshift32(void)
{
    uint32_t x = xorshift_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift_state = x;
    return x;
}

/* Clamp */
static inline int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint32_t clamp_u8(int32_t v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint32_t)v;
}

/* Integer sqrt */
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

/* ---- Acrylic effect ---- */

void gfx_acrylic(gfx_surface_t *s, int32_t x, int32_t y,
                  uint32_t w, uint32_t h,
                  gfx_color_t tint, uint8_t opacity,
                  uint32_t blur_radius)
{
    int32_t x0, y0, x1, y1;
    int32_t row, col;
    uint32_t tint_r, tint_g, tint_b;

    if (w == 0 || h == 0) return;

    /* Step 1: Apply box blur to the region */
    gfx_blur_rect(s, x, y, w, h, blur_radius);

    /* Clip */
    x0 = clamp_i(x, 0, (int32_t)s->width);
    y0 = clamp_i(y, 0, (int32_t)s->height);
    x1 = clamp_i(x + (int32_t)w, 0, (int32_t)s->width);
    y1 = clamp_i(y + (int32_t)h, 0, (int32_t)s->height);
    if (x0 >= x1 || y0 >= y1) return;

    tint_r = GFX_RED(tint);
    tint_g = GFX_GREEN(tint);
    tint_b = GFX_BLUE(tint);

    /* Step 2 + 3: Add noise texture + overlay tint */
    for (row = y0; row < y1; row++) {
        uint32_t *dp = s->pixels + (uint32_t)row * s->stride;
        for (col = x0; col < x1; col++) {
            uint32_t p = dp[col];
            uint32_t r = (p >> 16) & 0xFF;
            uint32_t g = (p >> 8)  & 0xFF;
            uint32_t b =  p        & 0xFF;

            /* Noise: ±3% random variation per pixel */
            {
                int32_t noise = (int32_t)(xorshift32() % 16) - 8;  /* -8..+7 */
                r = clamp_u8((int32_t)r + noise);
                g = clamp_u8((int32_t)g + noise);
                b = clamp_u8((int32_t)b + noise);
            }

            /* Tint overlay at opacity: out = blur * (1-opacity) + tint * opacity */
            {
                uint32_t inv_op = 255 - (uint32_t)opacity;
                r = div255(r * inv_op + tint_r * (uint32_t)opacity);
                g = div255(g * inv_op + tint_g * (uint32_t)opacity);
                b = div255(b * inv_op + tint_b * (uint32_t)opacity);
            }

            dp[col] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---- Mica effect ---- */

void gfx_mica(gfx_surface_t *s, int32_t x, int32_t y,
               uint32_t w, uint32_t h,
               const gfx_surface_t *wallpaper, gfx_color_t tint)
{
    int32_t x0, y0, x1, y1;
    int32_t row, col;
    uint32_t tint_r, tint_g, tint_b;

    if (w == 0 || h == 0) return;

    x0 = clamp_i(x, 0, (int32_t)s->width);
    y0 = clamp_i(y, 0, (int32_t)s->height);
    x1 = clamp_i(x + (int32_t)w, 0, (int32_t)s->width);
    y1 = clamp_i(y + (int32_t)h, 0, (int32_t)s->height);
    if (x0 >= x1 || y0 >= y1) return;

    tint_r = GFX_RED(tint);
    tint_g = GFX_GREEN(tint);
    tint_b = GFX_BLUE(tint);

    for (row = y0; row < y1; row++) {
        uint32_t *dp = s->pixels + (uint32_t)row * s->stride;

        for (col = x0; col < x1; col++) {
            uint32_t r, g, b, gray;

            /* Step 1: Sample wallpaper at this position */
            if (wallpaper && wallpaper->pixels &&
                (uint32_t)col < wallpaper->width &&
                (uint32_t)row < wallpaper->height)
            {
                uint32_t wp = wallpaper->pixels[(uint32_t)row * wallpaper->stride + (uint32_t)col];
                r = (wp >> 16) & 0xFF;
                g = (wp >> 8)  & 0xFF;
                b =  wp        & 0xFF;
            } else {
                r = g = b = 40;  /* Dark fallback */
            }

            /* Step 2: Desaturate (80% grayscale blend)
             * gray = 0.299R + 0.587G + 0.114B ≈ (77R + 150G + 29B) >> 8 */
            gray = (77 * r + 150 * g + 29 * b) >> 8;
            r = (r * 51 + gray * 204) / 255;   /* 20% color + 80% gray */
            g = (g * 51 + gray * 204) / 255;
            b = (b * 51 + gray * 204) / 255;

            /* Step 3: Tint with theme color (80% tint, 20% wallpaper hint) */
            r = (r * 51 + tint_r * 204) / 255;   /* 20% desaturated wp + 80% tint */
            g = (g * 51 + tint_g * 204) / 255;
            b = (b * 51 + tint_b * 204) / 255;

            dp[col] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---- Drop shadow ---- */

void gfx_drop_shadow(gfx_surface_t *s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, uint32_t radius,
                      uint32_t corner_radius,
                      int32_t offset_x, int32_t offset_y,
                      gfx_color_t color)
{
    gfx_surface_t shadow;
    uint32_t shadow_w, shadow_h;
    uint32_t pad;
    int32_t row, col;
    uint32_t sr, sg, sb, master_a;

    if (w == 0 || h == 0) return;

    /* Margin on each side: radius*2 gives the double blur pass enough room
     * to fully fade to zero without clipping at the surface boundary. */
    pad = radius * 4;
    shadow_w = w + pad;
    shadow_h = h + pad;

    sr = GFX_RED(color);
    sg = GFX_GREEN(color);
    sb = GFX_BLUE(color);
    master_a = GFX_ALPHA(color);   /* master opacity from alpha byte */

    /* Create a temporary surface for the shadow */
    if (gfx_surface_create(&shadow, shadow_w, shadow_h) != 0)
        return;

    /* Clear to transparent */
    gfx_clear(&shadow, GFX_COLOR_TRANSPARENT);

    /* Draw the shadow shape as a WHITE rounded rect, centered in the
     * padded surface.  After blur, the red channel becomes intensity. */
    gfx_fill_rounded_rect(&shadow, (int32_t)(radius * 2), (int32_t)(radius * 2),
                          w, h, corner_radius, 0xFFFFFFFF);

    /* Double blur pass for smoother Gaussian-like falloff */
    gfx_blur_rect(&shadow, 0, 0, shadow_w, shadow_h, radius);
    gfx_blur_rect(&shadow, 0, 0, shadow_w, shadow_h, radius);

    /* Blit the blurred shadow onto the target surface with alpha */
    {
        int32_t sx = x + offset_x - (int32_t)(radius * 2);
        int32_t sy = y + offset_y - (int32_t)(radius * 2);

        int32_t dx0 = clamp_i(sx, 0, (int32_t)s->width);
        int32_t dy0 = clamp_i(sy, 0, (int32_t)s->height);
        int32_t dx1 = clamp_i(sx + (int32_t)shadow_w, 0, (int32_t)s->width);
        int32_t dy1 = clamp_i(sy + (int32_t)shadow_h, 0, (int32_t)s->height);

        for (row = dy0; row < dy1; row++) {
            uint32_t *dp = s->pixels + (uint32_t)row * s->stride;
            int32_t srow = row - sy;
            if (srow < 0 || (uint32_t)srow >= shadow_h) continue;

            for (col = dx0; col < dx1; col++) {
                int32_t scol = col - sx;
                uint32_t sp;
                uint32_t intensity, sa;

                if (scol < 0 || (uint32_t)scol >= shadow_w) continue;

                sp = shadow.pixels[(uint32_t)srow * shadow.stride + (uint32_t)scol];
                intensity = (sp >> 16) & 0xFF;  /* red channel = blur intensity */
                sa = div255(intensity * master_a);  /* scale by master opacity */

                if (sa > 0) {
                    /* Blend shadow color at this alpha level */
                    uint32_t d = dp[col];
                    uint32_t inv_a = 255 - sa;
                    uint32_t dr = (d >> 16) & 0xFF;
                    uint32_t dg = (d >> 8)  & 0xFF;
                    uint32_t db =  d        & 0xFF;

                    uint32_t or_ = div255(sr * sa + dr * inv_a);
                    uint32_t og  = div255(sg * sa + dg * inv_a);
                    uint32_t ob  = div255(sb * sa + db * inv_a);

                    dp[col] = (0xFFu << 24) | (or_ << 16) | (og << 8) | ob;
                }
            }
        }
    }

    gfx_surface_destroy(&shadow);
}

/* ---- Reveal highlight (radial glow following cursor) ---- */

void gfx_reveal_highlight(gfx_surface_t *s, int32_t rx, int32_t ry,
                           uint32_t rw, uint32_t rh,
                           int32_t mouse_x, int32_t mouse_y,
                           uint32_t glow_radius, gfx_color_t highlight)
{
    int32_t x0, y0, x1, y1;
    int32_t row, col;
    uint32_t hr, hg, hb;

    if (rw == 0 || rh == 0 || glow_radius == 0) return;

    /* Clip to surface and to rect bounds */
    x0 = clamp_i(rx, 0, (int32_t)s->width);
    y0 = clamp_i(ry, 0, (int32_t)s->height);
    x1 = clamp_i(rx + (int32_t)rw, 0, (int32_t)s->width);
    y1 = clamp_i(ry + (int32_t)rh, 0, (int32_t)s->height);
    if (x0 >= x1 || y0 >= y1) return;

    hr = GFX_RED(highlight);
    hg = GFX_GREEN(highlight);
    hb = GFX_BLUE(highlight);

    for (row = y0; row < y1; row++) {
        uint32_t *dp = s->pixels + (uint32_t)row * s->stride;
        int32_t dy = row - mouse_y;

        for (col = x0; col < x1; col++) {
            int32_t dx_val = col - mouse_x;
            uint32_t dist_sq = (uint32_t)(dx_val * dx_val + dy * dy);
            uint32_t dist = isqrt(dist_sq);

            if (dist < glow_radius) {
                /* Glow intensity falls off linearly with distance */
                uint32_t intensity = ((glow_radius - dist) * 255) / glow_radius;
                uint32_t alpha = div255(intensity * GFX_ALPHA(highlight));
                uint32_t d = dp[col];
                uint32_t inv_a = 255 - alpha;

                uint32_t dr = (d >> 16) & 0xFF;
                uint32_t dg = (d >> 8)  & 0xFF;
                uint32_t db =  d        & 0xFF;

                uint32_t or_ = div255(hr * alpha + dr * inv_a);
                uint32_t og  = div255(hg * alpha + dg * inv_a);
                uint32_t ob  = div255(hb * alpha + db * inv_a);

                dp[col] = (0xFFu << 24) | (or_ << 16) | (og << 8) | ob;
            }
        }
    }
}
