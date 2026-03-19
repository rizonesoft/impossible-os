/* ============================================================================
 * spinner.c — Progressive arc-ring spinner (breathing animation engine)
 *
 * Windows 11-style Fluent 2 spinner: a dynamic arc that rotates while its
 * sweep angle oscillates.  Animation is driven by PIT timer callbacks.
 *
 * Two simultaneous motions:
 *   1. Rotation — the arc rotates around the ring center
 *   2. Sweep oscillation — the arc length breathes (grows and shrinks)
 *
 * Both motions use eased timing (quarter-sine LUT) for organic feel.
 * All math is integer-only — no FPU/SSE.
 *
 * Part of the Progressive Spinner component (TODO-010.97 §2).
 * ============================================================================ */

#include "kernel/spinner.h"
#include "kernel/gfx/arc_ring.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/pit.h"
#include "gfx/ease_lut.h"

/* ---- Animation parameters ---- */

#define SPINNER_TICK_DIVISOR  5  /* PIT at 100Hz / 5 = 20 fps */
#define SPINNER_ROT_FRAMES   36 /* frames per full rotation (1.8s @ 20fps) */
#define SPINNER_SWEEP_FRAMES 30 /* frames per sweep oscillation (1.5s @ 20fps) */
#define SPINNER_SWEEP_MIN    20 /* min arc sweep (out of 256 ≈ 28°) */
#define SPINNER_SWEEP_MAX    192 /* max arc sweep (out of 256 ≈ 270°) */

/* ---- State ---- */

static volatile uint8_t  s_active;   /* 1 = animation running */
static volatile uint32_t s_frame;    /* monotonic frame counter */
static int32_t  s_cx, s_cy;          /* ring center position */
static int32_t  s_radius;            /* outer radius (scaled) */
static int32_t  s_stroke;            /* ring thickness (scaled) */
static uint32_t s_color;             /* accent color (0x00RRGGBB) */

/* ---- Frame computation ---- */

/* Compute rotation angle and sweep for the current frame.
 * Returns start angle and sweep as 0–255 values. */
static void spinner_compute_frame(uint32_t frame,
                                  uint8_t *out_start, uint8_t *out_sweep)
{
    /* Rotation: linear sweep through 0–255, but at a non-constant rate.
     * The rotation leads the sweep by having a slightly different period. */
    uint32_t rot = (frame * 256 / SPINNER_ROT_FRAMES) & 0xFF;

    /* Sweep oscillation: phase goes 0→127→0→127... in SWEEP_FRAMES period.
     * We fold the phase into 0–63 and mirror for the second half. */
    uint32_t sweep_phase = (frame * 128 / SPINNER_SWEEP_FRAMES) % 128;
    uint32_t ease_idx;
    if (sweep_phase < 64)
        ease_idx = sweep_phase;
    else
        ease_idx = 127 - sweep_phase;

    uint32_t eased = (uint32_t)ease_lut[ease_idx];

    /* Map eased value to sweep range */
    uint32_t sweep = SPINNER_SWEEP_MIN
                   + (SPINNER_SWEEP_MAX - SPINNER_SWEEP_MIN) * eased / 255;

    *out_start = (uint8_t)rot;
    *out_sweep = (uint8_t)sweep;
}

/* ---- Rendering ---- */

/* Draw one spinner frame into the framebuffer back buffer.
 * fade: 0–255 opacity (255 = fully opaque). */
static void spinner_render(uint8_t fade)
{
    uint32_t *buf = fb_get_backbuffer();
    uint32_t buf_w = fb_get_width();
    uint32_t buf_h = fb_get_height();
    if (!buf || buf_w == 0 || buf_h == 0) return;

    /* Compute animation parameters */
    uint8_t start, sweep;
    spinner_compute_frame(s_frame, &start, &sweep);

    /* Apply fade to color */
    uint32_t color = s_color;
    if (fade < 255) {
        uint8_t r = (uint8_t)((((color >> 16) & 0xFF) * (uint32_t)fade) / 255);
        uint8_t g = (uint8_t)((((color >> 8)  & 0xFF) * (uint32_t)fade) / 255);
        uint8_t b = (uint8_t)(((color & 0xFF)          * (uint32_t)fade) / 255);
        color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    /* Clear the spinner area first (black bg) */
    int32_t bound = s_radius + 4;
    int32_t clear_x = s_cx - bound;
    int32_t clear_y = s_cy - bound;
    int32_t clear_sz = bound * 2;

    if (clear_x < 0) clear_x = 0;
    if (clear_y < 0) clear_y = 0;
    if (clear_x + clear_sz > (int32_t)buf_w)
        clear_sz = (int32_t)buf_w - clear_x;

    int32_t clear_h = bound * 2;
    if (clear_y + clear_h > (int32_t)buf_h)
        clear_h = (int32_t)buf_h - clear_y;

    /* Fill clear area with black */
    for (int32_t py = clear_y; py < clear_y + clear_h; py++) {
        for (int32_t px = clear_x; px < clear_x + clear_sz; px++) {
            buf[py * (int32_t)buf_w + px] = 0x00000000;
        }
    }

    /* Draw the arc ring */
    arc_ring_draw(buf, buf_w, buf_h,
                  s_cx, s_cy, s_radius, s_stroke,
                  start, sweep, color);
}

/* ---- PIT timer callback ---- */

static void spinner_timer_callback(void)
{
    if (!s_active) return;

    s_frame++;
    spinner_render(255);

    /* Swap only the spinner's bounding rect (no full-screen swap) */
    uint32_t bx, by, bw, bh;
    spinner_get_bounds(&bx, &by, &bw, &bh);
    fb_swap_rect(bx, by, bw, bh);
}

/* ---- Public API ---- */

void spinner_init(int32_t cx, int32_t cy,
                  int32_t radius, int32_t stroke,
                  uint32_t color)
{
    s_cx     = cx;
    s_cy     = cy;
    s_radius = radius;
    s_stroke = stroke;
    s_color  = color;
    s_frame  = 0;
    s_active = 0;
}

void spinner_start(void)
{
    if (s_active) return;
    s_active = 1;
    s_frame  = 0;
    pit_register_callback(spinner_timer_callback, SPINNER_TICK_DIVISOR);
}

void spinner_stop(void)
{
    if (!s_active) return;
    s_active = 0;
    pit_unregister_callback();
}

void spinner_draw_faded(uint8_t fade)
{
    spinner_render(fade);
}

void spinner_get_bounds(uint32_t *out_x, uint32_t *out_y,
                        uint32_t *out_w, uint32_t *out_h)
{
    int32_t bound = s_radius + 4;  /* includes AA fringe + end caps */
    int32_t bx = s_cx - bound;
    int32_t by = s_cy - bound;

    if (bx < 0) bx = 0;
    if (by < 0) by = 0;

    *out_x = (uint32_t)bx;
    *out_y = (uint32_t)by;
    *out_w = (uint32_t)(bound * 2);
    *out_h = (uint32_t)(bound * 2);
}

int spinner_is_active(void)
{
    return s_active;
}
