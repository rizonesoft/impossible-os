/* ============================================================================
 * arc_ring.c — Anti-aliased arc ring drawing primitive
 *
 * Draws a stroked arc (partial ring) to a pixel buffer using integer-only
 * math.  No FPU, no SSE — safe for timer ISR context.
 *
 * Algorithm:
 *   For each pixel in the arc's bounding box, compute:
 *     1. Radial distance from center → is pixel inside the ring?
 *     2. Angular position → is pixel within the arc sweep?
 *   Anti-aliasing uses smooth blend zones on all edges (radial + angular).
 *
 * Part of the Progressive Spinner component (TODO-010.97).
 * ============================================================================ */

#include "kernel/gfx/arc_ring.h"
#include "gfx/arc_lut.h"

/* ---- HiDPI scaling ---- */

struct arc_ring_size arc_ring_size_for_height(uint32_t scr_h)
{
    struct arc_ring_size s;
    if (scr_h >= 2160)      { s.radius = 48; s.stroke = 5; }
    else if (scr_h >= 1440) { s.radius = 32; s.stroke = 4; }
    else if (scr_h >= 1080) { s.radius = 24; s.stroke = 3; }
    else                    { s.radius = 20; s.stroke = 3; }
    return s;
}

/* ---- Angle computation via atan2 approximation ----
 *
 * Returns angle in 0..255 range (mapping to 0°..360°).
 * Convention matches the LUT:
 *   0   = right  (3 o'clock, cos=+1, sin=0)
 *   64  = down   (6 o'clock, cos=0, sin=+1)  [screen coords: y+ is down]
 *   128 = left   (9 o'clock, cos=-1, sin=0)
 *   192 = up     (12 o'clock, cos=0, sin=-1)
 *
 * Uses quadrant decomposition + linear atan approximation.
 * Max error: ~1 LUT step (~1.4°) — invisible at spinner scale. */

static uint8_t angle256(int32_t dx, int32_t dy)
{
    if (dx == 0 && dy == 0) return 0;

    int32_t ax = dx < 0 ? -dx : dx;
    int32_t ay = dy < 0 ? -dy : dy;

    /* Compute angle within [0, 64) for the first quadrant (dx>=0, dy>=0).
     * When ax >= ay, angle is in [0, 32]  (0°..45°).
     * When ay > ax,  angle is in [32, 64) (45°..90°). */
    int32_t sub;
    if (ax >= ay)
        sub = (ax > 0) ? (ay * 32 / ax) : 0;      /* 0..32 */
    else
        sub = 64 - ((ax > 0) ? (ax * 32 / ay) : 0); /* 32..64 */

    /* Map to full circle based on sign of dx, dy */
    int32_t angle;
    if (dx >= 0 && dy >= 0)
        angle = sub;            /*   0.. 64 : Q1 right→down  */
    else if (dx < 0 && dy >= 0)
        angle = 128 - sub;      /*  64..128 : Q2 down→left   */
    else if (dx < 0 && dy < 0)
        angle = 128 + sub;      /* 128..192 : Q3 left→up     */
    else /* dx >= 0 && dy < 0 */
        angle = 256 - sub;      /* 192..256 : Q4 up→right    */

    return (uint8_t)(angle & 0xFF);
}

/* ---- Check if angle is within arc sweep ----
 *
 * All angles are in 0..255 range, wrapping around.
 * Returns 0–255 indicating how far inside the sweep the angle is,
 * or -1 if outside the sweep entirely.
 *
 * For AA on arc endpoints, returns fractional closeness to the edge. */

static int32_t angle_in_sweep(uint8_t angle, uint8_t start, uint8_t sweep)
{
    /* Normalize: how far past 'start' is 'angle'? (wrapping) */
    uint8_t offset = (uint8_t)(angle - start);

    if (offset <= sweep)
        return (int32_t)offset;  /* inside the sweep */
    return -1;  /* outside */
}

/* ---- Core arc ring renderer ---- */

void arc_ring_draw(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                   int32_t cx, int32_t cy,
                   int32_t radius, int32_t stroke,
                   uint8_t start_256, uint8_t sweep_256,
                   uint32_t color)
{
    if (!buf || radius <= 0 || stroke <= 0 || sweep_256 == 0)
        return;

    /* Clamp stroke to radius */
    if (stroke > radius) stroke = radius;

    /* Extract color channels */
    uint8_t cr = (uint8_t)((color >> 16) & 0xFF);
    uint8_t cg = (uint8_t)((color >> 8) & 0xFF);
    uint8_t cb = (uint8_t)(color & 0xFF);

    /* Inner and outer radii */
    int32_t r_outer = radius;
    int32_t r_inner = radius - stroke;

    /* AA blend zones (1.5px on each radial edge).
     * Using fixed-point *256 for sub-pixel precision. */
    int32_t r_outer_sq_max = (r_outer + 1) * (r_outer + 1);  /* full transparent */
    int32_t r_outer_sq_min = (r_outer - 1) * (r_outer - 1);  /* full opaque */
    int32_t r_inner_sq_max = (r_inner + 1) * (r_inner + 1);  /* full opaque */
    int32_t r_inner_sq_min = (r_inner - 1) * (r_inner - 1);  /* full transparent */

    /* Angular AA zone: blend over ~2 LUT steps at each arc endpoint */
    int32_t aa_angle_steps = 3;

    /* Bounding box (slightly oversize for AA fringe) */
    int32_t bound = r_outer + 2;
    int32_t y_min = cy - bound;
    int32_t y_max = cy + bound;
    int32_t x_min = cx - bound;
    int32_t x_max = cx + bound;

    /* Clamp to buffer */
    if (y_min < 0) y_min = 0;
    if (x_min < 0) x_min = 0;
    if (y_max >= (int32_t)buf_h) y_max = (int32_t)buf_h - 1;
    if (x_max >= (int32_t)buf_w) x_max = (int32_t)buf_w - 1;

    for (int32_t py = y_min; py <= y_max; py++) {
        int32_t dy = py - cy;
        for (int32_t px = x_min; px <= x_max; px++) {
            int32_t dx = px - cx;

            /* 1. Radial test: is this pixel in the ring thickness? */
            int32_t d2 = dx * dx + dy * dy;

            /* Quick reject: outside outer AA zone or inside inner AA zone */
            if (d2 > r_outer_sq_max) continue;
            if (d2 < r_inner_sq_min) continue;

            /* Compute radial alpha (AA on inner and outer edges) */
            int32_t radial_alpha = 255;

            if (d2 > r_outer_sq_min) {
                /* Outer fringe: fade out */
                int32_t range = r_outer_sq_max - r_outer_sq_min;
                if (range > 0) {
                    int32_t frac = r_outer_sq_max - d2;
                    radial_alpha = frac * 255 / range;
                }
            } else if (d2 < r_inner_sq_max) {
                /* Inner fringe: fade in */
                int32_t range = r_inner_sq_max - r_inner_sq_min;
                if (range > 0) {
                    int32_t frac = d2 - r_inner_sq_min;
                    radial_alpha = frac * 255 / range;
                }
            }

            if (radial_alpha <= 0) continue;
            if (radial_alpha > 255) radial_alpha = 255;

            /* 2. Angular test: is this pixel within the arc sweep? */
            uint8_t pixel_angle = angle256(dx, dy);
            int32_t offset = angle_in_sweep(pixel_angle, start_256, sweep_256);

            if (offset < 0) continue;  /* outside arc */

            /* Angular AA at endpoints */
            int32_t angular_alpha = 255;

            if (offset < aa_angle_steps) {
                /* Near start endpoint */
                angular_alpha = offset * 255 / aa_angle_steps;
            } else if (offset > (int32_t)sweep_256 - aa_angle_steps) {
                /* Near end endpoint */
                int32_t from_end = (int32_t)sweep_256 - offset;
                angular_alpha = from_end * 255 / aa_angle_steps;
            }

            if (angular_alpha <= 0) continue;
            if (angular_alpha > 255) angular_alpha = 255;

            /* Combined alpha */
            int32_t alpha = radial_alpha * angular_alpha / 255;
            if (alpha <= 0) continue;
            if (alpha > 255) alpha = 255;

            /* Blend onto existing pixel (assumed black bg during splash,
             * but we do proper alpha blend for compositor use). */
            uint32_t existing = buf[py * (int32_t)buf_w + px];
            uint8_t eb = (uint8_t)(existing & 0xFF);
            uint8_t eg = (uint8_t)((existing >> 8) & 0xFF);
            uint8_t er = (uint8_t)((existing >> 16) & 0xFF);

            uint8_t ob = (uint8_t)(((int32_t)cb * alpha
                                    + (int32_t)eb * (255 - alpha)) / 255);
            uint8_t og = (uint8_t)(((int32_t)cg * alpha
                                    + (int32_t)eg * (255 - alpha)) / 255);
            uint8_t or_ = (uint8_t)(((int32_t)cr * alpha
                                     + (int32_t)er * (255 - alpha)) / 255);

            buf[py * (int32_t)buf_w + px] =
                (uint32_t)ob | ((uint32_t)og << 8) | ((uint32_t)or_ << 16);
        }
    }

    /* Rounded end caps: small filled circles at arc start and end points.
     * Cap radius = stroke/2 (centered on the ring's mid-radius).
     * Skip caps for thin strokes (stroke <= 4) — cap_r would be ≤ 2px,
     * producing pixelated single-pixel dots. Angular AA handles endpoints. */
    if (stroke <= 4)
        return;

    int32_t mid_r = r_inner + stroke / 2;
    int32_t cap_r = stroke / 2;
    if (cap_r < 2) cap_r = 2;

    uint8_t endpoints[2] = { start_256,
                             (uint8_t)(start_256 + sweep_256) };

    for (int ep = 0; ep < 2; ep++) {
        uint8_t a = endpoints[ep];
        /* Center of the cap on the ring mid-radius */
        int32_t cap_cx = cx + (int32_t)arc_cos_lut[a] * mid_r / 256;
        int32_t cap_cy = cy + (int32_t)arc_sin_lut[a] * mid_r / 256;

        int32_t cap_bound = cap_r + 2;
        int32_t cap_r_inner_sq = (cap_r - 1) * (cap_r - 1);
        int32_t cap_r_outer_sq = (cap_r + 1) * (cap_r + 1);

        for (int32_t cpy = cap_cy - cap_bound;
             cpy <= cap_cy + cap_bound; cpy++) {
            if (cpy < 0 || (uint32_t)cpy >= buf_h) continue;
            for (int32_t cpx = cap_cx - cap_bound;
                 cpx <= cap_cx + cap_bound; cpx++) {
                if (cpx < 0 || (uint32_t)cpx >= buf_w) continue;

                int32_t cdx = cpx - cap_cx;
                int32_t cdy = cpy - cap_cy;
                int32_t cd2 = cdx * cdx + cdy * cdy;

                if (cd2 > cap_r_outer_sq) continue;

                int32_t cap_alpha;
                if (cd2 <= cap_r_inner_sq) {
                    cap_alpha = 255;
                } else {
                    int32_t range = cap_r_outer_sq - cap_r_inner_sq;
                    if (range <= 0) continue;
                    cap_alpha = (cap_r_outer_sq - cd2) * 255 / range;
                }
                if (cap_alpha <= 0) continue;
                if (cap_alpha > 255) cap_alpha = 255;

                /* Alpha blend cap pixel */
                uint32_t existing = buf[cpy * (int32_t)buf_w + cpx];
                uint8_t eb = (uint8_t)(existing & 0xFF);
                uint8_t eg = (uint8_t)((existing >> 8) & 0xFF);
                uint8_t er = (uint8_t)((existing >> 16) & 0xFF);

                uint8_t ob = (uint8_t)(((int32_t)cb * cap_alpha
                                        + (int32_t)eb * (255 - cap_alpha))
                                       / 255);
                uint8_t og = (uint8_t)(((int32_t)cg * cap_alpha
                                        + (int32_t)eg * (255 - cap_alpha))
                                       / 255);
                uint8_t or_ = (uint8_t)(((int32_t)cr * cap_alpha
                                         + (int32_t)er * (255 - cap_alpha))
                                        / 255);

                buf[cpy * (int32_t)buf_w + cpx] =
                    (uint32_t)ob | ((uint32_t)og << 8)
                    | ((uint32_t)or_ << 16);
            }
        }
    }
}
