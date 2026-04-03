/* ============================================================================
 * gfx_core.c -- 2D Compositing Library: Core Surface and Primitives
 *
 * All drawing functions operate on gfx_surface_t, a hardware-independent
 * pixel buffer.  The surface can wrap the framebuffer back buffer or any
 * heap-allocated off-screen buffer.
 *
 * Rendering rules:
 *   - Coordinates are signed (negative = off-screen, clipped)
 *   - All functions perform bounds checking (no OOB writes)
 *   - Color format: 0xAARRGGBB
 *   - Anti-aliased rounded corners use sub-pixel distance blending
 * ============================================================================ */

#include "gfx.h"
#include "kernel/mm/heap.h"
#include "kernel/types.h"

/* ---- Helpers ---- */

/* Clamp a value to [lo, hi] */
static inline int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Integer square root (Newton's method) */
static uint32_t isqrt(uint32_t n)
{
    uint32_t x;
    uint32_t x1;

    if (n == 0) return 0;
    x = n;
    x1 = (x + 1) / 2;
    while (x1 < x) {
        x = x1;
        x1 = (x + n / x) / 2;
    }
    return x;
}

/* Absolute value */
static inline int32_t abs_i(int32_t v)
{
    return v < 0 ? -v : v;
}

/* Fast memset for uint32_t */
static void mem_set32(uint32_t *dst, uint32_t val, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        dst[i] = val;
}

/* ---- Surface management ---- */

void gfx_surface_init(gfx_surface_t *s, uint32_t *pixels,
                       uint32_t w, uint32_t h, uint32_t stride)
{
    s->pixels = pixels;
    s->width  = w;
    s->height = h;
    s->stride = stride;
}

int gfx_surface_create(gfx_surface_t *s, uint32_t w, uint32_t h)
{
    uint32_t *buf = (uint32_t *)kmalloc(w * h * sizeof(uint32_t));
    if (!buf)
        return -1;

    s->pixels = buf;
    s->width  = w;
    s->height = h;
    s->stride = w;
    return 0;
}

void gfx_surface_destroy(gfx_surface_t *s)
{
    if (s->pixels) {
        kfree(s->pixels);
        s->pixels = (void *)0;
    }
    s->width = s->height = s->stride = 0;
}

void gfx_clear(gfx_surface_t *s, gfx_color_t color)
{
    uint32_t y;
    for (y = 0; y < s->height; y++)
        mem_set32(s->pixels + y * s->stride, color, s->width);
}

/* ---- Pixel operations ---- */

void gfx_put_pixel(gfx_surface_t *s, int32_t x, int32_t y, gfx_color_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height)
        return;
    s->pixels[(uint32_t)y * s->stride + (uint32_t)x] = color;
}

void gfx_blend_pixel(gfx_surface_t *s, int32_t x, int32_t y, gfx_color_t color)
{
    uint32_t sa, sr, sg, sb;
    uint32_t da, dr, dg, db;
    uint32_t inv_a;
    gfx_color_t dst;

    if (x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height)
        return;

    sa = GFX_ALPHA(color);
    if (sa == 0xFF) {
        /* Fully opaque -- just overwrite */
        s->pixels[(uint32_t)y * s->stride + (uint32_t)x] = color;
        return;
    }
    if (sa == 0x00)
        return;  /* Fully transparent -- skip */

    sr = GFX_RED(color);
    sg = GFX_GREEN(color);
    sb = GFX_BLUE(color);

    dst = s->pixels[(uint32_t)y * s->stride + (uint32_t)x];
    da = GFX_ALPHA(dst);
    dr = GFX_RED(dst);
    dg = GFX_GREEN(dst);
    db = GFX_BLUE(dst);

    inv_a = 255 - sa;

    /* Standard alpha blend: out = src * alpha + dst * (1 - alpha) */
    dr = (sr * sa + dr * inv_a) / 255;
    dg = (sg * sa + dg * inv_a) / 255;
    db = (sb * sa + db * inv_a) / 255;
    da = sa + (da * inv_a) / 255;

    s->pixels[(uint32_t)y * s->stride + (uint32_t)x] =
        GFX_RGBA(dr, dg, db, da);
}

/* ---- Filled rectangle ---- */

void gfx_fill_rect(gfx_surface_t *s, int32_t x, int32_t y,
                    uint32_t w, uint32_t h, gfx_color_t color)
{
    int32_t x0, y0, x1, y1;
    int32_t row;

    /* Clip to surface bounds */
    x0 = clamp_i(x, 0, (int32_t)s->width);
    y0 = clamp_i(y, 0, (int32_t)s->height);
    x1 = clamp_i(x + (int32_t)w, 0, (int32_t)s->width);
    y1 = clamp_i(y + (int32_t)h, 0, (int32_t)s->height);

    if (x0 >= x1 || y0 >= y1)
        return;

    for (row = y0; row < y1; row++)
        mem_set32(s->pixels + (uint32_t)row * s->stride + (uint32_t)x0,
                  color, (uint32_t)(x1 - x0));
}

/* ---- Outline rectangle ---- */

void gfx_draw_rect(gfx_surface_t *s, int32_t x, int32_t y,
                    uint32_t w, uint32_t h, uint32_t thickness,
                    gfx_color_t color)
{
    uint32_t t;

    if (w == 0 || h == 0 || thickness == 0)
        return;

    /* Clamp thickness to half the smallest dimension */
    t = thickness;
    if (t > w / 2) t = w / 2;
    if (t > h / 2) t = h / 2;

    /* Top edge */
    gfx_fill_rect(s, x, y, w, t, color);
    /* Bottom edge */
    gfx_fill_rect(s, x, y + (int32_t)(h - t), w, t, color);
    /* Left edge */
    gfx_fill_rect(s, x, y + (int32_t)t, t, h - 2 * t, color);
    /* Right edge */
    gfx_fill_rect(s, x + (int32_t)(w - t), y + (int32_t)t, t, h - 2 * t, color);
}

/* ---- Filled rounded rectangle ---- */

void gfx_fill_rounded_rect(gfx_surface_t *s, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t radius,
                            gfx_color_t color)
{
    int32_t row;
    uint32_t r;
    uint32_t sa;
    uint32_t sr;
    uint32_t sg;
    uint32_t sb;

    if (w == 0 || h == 0)
        return;

    r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    sa = GFX_ALPHA(color);
    sr = GFX_RED(color);
    sg = GFX_GREEN(color);
    sb = GFX_BLUE(color);

    for (row = 0; row < (int32_t)h; row++) {
        int32_t fill_x0 = x;
        int32_t fill_x1 = x + (int32_t)w;
        uint32_t edge_alpha = 0;
        int is_corner_row = 0;

        /* Apply corner rounding using half-pixel centered circle.
         * Uses 2x fixed-point coordinates for sub-pixel accuracy.
         * Circle center is at (r-0.5, r-0.5) from corner, which
         * produces smoother curves than integer centering. */
        if ((uint32_t)row < r) {
            /* Top corners */
            uint32_t r2 = r * 2;        /* radius in 2x coords */
            uint32_t dy2 = r2 - (uint32_t)row * 2 - 1;  /* half-pixel offset */
            uint32_t r2_sq = r2 * r2;
            uint32_t dx2_sq = (r2_sq > dy2 * dy2) ? (r2_sq - dy2 * dy2) : 0;
            uint32_t dx2 = isqrt(dx2_sq);
            uint32_t inset = r - dx2 / 2;

            /* AA: fractional coverage from 2x-precision remainder */
            {
                uint32_t frac = dx2 % 2;  /* 0 or 1 in 2x coords */
                /* For better AA, compute at higher precision */
                uint32_t dx16_sq = dx2_sq * 64;  /* 16x precision (2x * 8x) */
                uint32_t dx16 = isqrt(dx16_sq);
                uint32_t dx_whole_16 = (dx2 / 2) * 16;
                uint32_t subfrac = dx16 - dx_whole_16;
                if (subfrac > 15) subfrac = 15;
                edge_alpha = (sa * subfrac) / 16;
                (void)frac;
            }

            fill_x0 = x + (int32_t)inset;
            fill_x1 = x + (int32_t)(w - inset);
            is_corner_row = 1;
        } else if ((uint32_t)row >= h - r) {
            /* Bottom corners */
            uint32_t bot_row = (uint32_t)row - (h - r);  /* 0-based from bottom zone start */
            uint32_t r2 = r * 2;
            uint32_t dy2 = bot_row * 2 + 1;  /* half-pixel offset from bottom */
            uint32_t r2_sq = r2 * r2;
            uint32_t dx2_sq = (r2_sq > dy2 * dy2) ? (r2_sq - dy2 * dy2) : 0;
            uint32_t dx2 = isqrt(dx2_sq);
            uint32_t inset = r - dx2 / 2;

            /* AA for bottom corners */
            {
                uint32_t dx16_sq = dx2_sq * 64;
                uint32_t dx16 = isqrt(dx16_sq);
                uint32_t dx_whole_16 = (dx2 / 2) * 16;
                uint32_t subfrac = dx16 - dx_whole_16;
                if (subfrac > 15) subfrac = 15;
                edge_alpha = (sa * subfrac) / 16;
            }

            fill_x0 = x + (int32_t)inset;
            fill_x1 = x + (int32_t)(w - inset);
            is_corner_row = 1;
        }

        /* Fill the scanline */
        if (fill_x0 < fill_x1) {
            if (sa == 0xFF) {
                gfx_fill_rect(s, fill_x0, y + row, (uint32_t)(fill_x1 - fill_x0), 1, color);
            } else {
                /* Alpha-blended fill */
                int32_t px;
                gfx_color_t c = GFX_RGBA(sr, sg, sb, sa);
                for (px = fill_x0; px < fill_x1; px++)
                    gfx_blend_pixel(s, px, y + row, c);
            }

            /* Anti-alias: blend the edge pixels just outside the fill area */
            if (is_corner_row && edge_alpha > 0 && edge_alpha < sa) {
                gfx_color_t aa_color = GFX_RGBA(sr, sg, sb, edge_alpha);
                /* Left edge pixel */
                if (fill_x0 > x)
                    gfx_blend_pixel(s, fill_x0 - 1, y + row, aa_color);
                /* Right edge pixel */
                if (fill_x1 < x + (int32_t)w)
                    gfx_blend_pixel(s, fill_x1, y + row, aa_color);
            }
        }
    }
}

/* ---- Outline rounded rectangle ----
 * Draws only the border edges + rounded corners.
 * DOES NOT fill the interior -- safe to use on top of acrylic. */

void gfx_draw_rounded_rect(gfx_surface_t *s, int32_t x, int32_t y,
                            uint32_t w, uint32_t h, uint32_t radius,
                            uint32_t thickness, gfx_color_t color)
{
    int32_t row;
    uint32_t r, t;

    if (w == 0 || h == 0 || thickness == 0)
        return;

    t = thickness;
    r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (t > w / 2) t = w / 2;
    if (t > h / 2) t = h / 2;

    for (row = 0; row < (int32_t)h; row++) {
        int32_t outer_x0 = x;
        int32_t outer_x1 = x + (int32_t)w;

        /* Compute outer edge inset for rounded corners */
        if ((uint32_t)row < r) {
            uint32_t r2 = r * 2;
            uint32_t dy2 = r2 - (uint32_t)row * 2 - 1;
            uint32_t r2_sq = r2 * r2;
            uint32_t dx2_sq = (r2_sq > dy2 * dy2) ? (r2_sq - dy2 * dy2) : 0;
            uint32_t dx2 = isqrt(dx2_sq);
            uint32_t inset = r - dx2 / 2;
            outer_x0 = x + (int32_t)inset;
            outer_x1 = x + (int32_t)(w - inset);
        } else if ((uint32_t)row >= h - r) {
            uint32_t bot_row = (uint32_t)row - (h - r);
            uint32_t r2 = r * 2;
            uint32_t dy2 = bot_row * 2 + 1;
            uint32_t r2_sq = r2 * r2;
            uint32_t dx2_sq = (r2_sq > dy2 * dy2) ? (r2_sq - dy2 * dy2) : 0;
            uint32_t dx2 = isqrt(dx2_sq);
            uint32_t inset = r - dx2 / 2;
            outer_x0 = x + (int32_t)inset;
            outer_x1 = x + (int32_t)(w - inset);
        }

        /* Top or bottom edge rows: fill entire span */
        if ((uint32_t)row < t || (uint32_t)row >= h - t) {
            if (outer_x0 < outer_x1)
                gfx_fill_rect(s, outer_x0, y + row,
                               (uint32_t)(outer_x1 - outer_x0), 1, color);
        } else {
            /* Middle rows: draw only left and right edge strips */
            if (outer_x0 < outer_x1) {
                /* Left edge */
                int32_t left_end = outer_x0 + (int32_t)t;
                if (left_end > outer_x1) left_end = outer_x1;
                gfx_fill_rect(s, outer_x0, y + row,
                               (uint32_t)(left_end - outer_x0), 1, color);
                /* Right edge */
                int32_t right_start = outer_x1 - (int32_t)t;
                if (right_start < outer_x0) right_start = outer_x0;
                if (right_start < left_end) right_start = left_end;
                if (right_start < outer_x1)
                    gfx_fill_rect(s, right_start, y + row,
                                   (uint32_t)(outer_x1 - right_start), 1, color);
            }
        }
    }
}

/* ---- Filled circle (midpoint algorithm) ---- */

void gfx_fill_circle(gfx_surface_t *s, int32_t cx, int32_t cy,
                      int32_t r, gfx_color_t color)
{
    int32_t x_off = 0;
    int32_t y_off = r;
    int32_t d = 1 - r;

    if (r <= 0) {
        gfx_put_pixel(s, cx, cy, color);
        return;
    }

    /* Draw horizontal spans for each scanline of the circle */
    while (x_off <= y_off) {
        /* Fill horizontal lines across the circle */
        gfx_fill_rect(s, cx - y_off, cy + x_off, (uint32_t)(2 * y_off + 1), 1, color);
        gfx_fill_rect(s, cx - y_off, cy - x_off, (uint32_t)(2 * y_off + 1), 1, color);
        gfx_fill_rect(s, cx - x_off, cy + y_off, (uint32_t)(2 * x_off + 1), 1, color);
        gfx_fill_rect(s, cx - x_off, cy - y_off, (uint32_t)(2 * x_off + 1), 1, color);

        if (d < 0) {
            d += 2 * x_off + 3;
        } else {
            d += 2 * (x_off - y_off) + 5;
            y_off--;
        }
        x_off++;
    }
}

/* ---- Line (Bresenham with thickness) ---- */

void gfx_draw_line(gfx_surface_t *s, int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1, uint32_t thickness,
                    gfx_color_t color)
{
    int32_t dx = abs_i(x1 - x0);
    int32_t dy = -abs_i(y1 - y0);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    int32_t e2;

    if (thickness <= 1) {
        /* Standard Bresenham -- single pixel width */
        for (;;) {
            gfx_put_pixel(s, x0, y0, color);
            if (x0 == x1 && y0 == y1) break;
            e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    } else {
        /* Thick line: draw a filled circle at each point along the line */
        int32_t half_t = (int32_t)(thickness / 2);
        for (;;) {
            gfx_fill_circle(s, x0, y0, half_t, color);
            if (x0 == x1 && y0 == y1) break;
            e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

/* ---- Dirty Rectangle Tracker ---- */

void gfx_dirty_reset(gfx_dirty_tracker_t *dt)
{
    dt->count = 0;
}

void gfx_dirty_add(gfx_dirty_tracker_t *dt, int32_t x, int32_t y,
                    uint32_t w, uint32_t h)
{
    if (dt->count < GFX_MAX_DIRTY) {
        dt->rects[dt->count].x = x;
        dt->rects[dt->count].y = y;
        dt->rects[dt->count].w = w;
        dt->rects[dt->count].h = h;
        dt->count++;
    } else {
        /* Overflow: merge into the first rect as a bounding box */
        int32_t bx0 = dt->rects[0].x;
        int32_t by0 = dt->rects[0].y;
        int32_t bx1 = bx0 + (int32_t)dt->rects[0].w;
        int32_t by1 = by0 + (int32_t)dt->rects[0].h;
        uint32_t i;

        for (i = 1; i < dt->count; i++) {
            int32_t rx0 = dt->rects[i].x;
            int32_t ry0 = dt->rects[i].y;
            int32_t rx1 = rx0 + (int32_t)dt->rects[i].w;
            int32_t ry1 = ry0 + (int32_t)dt->rects[i].h;
            if (rx0 < bx0) bx0 = rx0;
            if (ry0 < by0) by0 = ry0;
            if (rx1 > bx1) bx1 = rx1;
            if (ry1 > by1) by1 = ry1;
        }

        /* Include the new rect */
        if (x < bx0) bx0 = x;
        if (y < by0) by0 = y;
        if (x + (int32_t)w > bx1) bx1 = x + (int32_t)w;
        if (y + (int32_t)h > by1) by1 = y + (int32_t)h;

        dt->rects[0].x = bx0;
        dt->rects[0].y = by0;
        dt->rects[0].w = (uint32_t)(bx1 - bx0);
        dt->rects[0].h = (uint32_t)(by1 - by0);
        dt->count = 1;
    }
}

int gfx_dirty_bounds(gfx_dirty_tracker_t *dt, int32_t *x, int32_t *y,
                     uint32_t *w, uint32_t *h)
{
    int32_t bx0, by0, bx1, by1;
    uint32_t i;

    if (dt->count == 0)
        return 0;

    bx0 = dt->rects[0].x;
    by0 = dt->rects[0].y;
    bx1 = bx0 + (int32_t)dt->rects[0].w;
    by1 = by0 + (int32_t)dt->rects[0].h;

    for (i = 1; i < dt->count; i++) {
        int32_t rx0 = dt->rects[i].x;
        int32_t ry0 = dt->rects[i].y;
        int32_t rx1 = rx0 + (int32_t)dt->rects[i].w;
        int32_t ry1 = ry0 + (int32_t)dt->rects[i].h;
        if (rx0 < bx0) bx0 = rx0;
        if (ry0 < by0) by0 = ry0;
        if (rx1 > bx1) bx1 = rx1;
        if (ry1 > by1) by1 = ry1;
    }

    *x = bx0;
    *y = by0;
    *w = (uint32_t)(bx1 - bx0);
    *h = (uint32_t)(by1 - by0);
    return 1;
}
