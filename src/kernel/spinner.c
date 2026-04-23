/* ============================================================================
 * spinner.c -- Progressive arc-ring spinner (breathing animation engine)
 *
 * Windows 11-style Fluent 2 spinner: a dynamic arc that rotates while its
 * length oscillates.  Animation is driven by sleep_ms() polling loop
 * via the Unified Timer Subsystem (UTS).
 *
 * Two concurrent behaviors (per Fluent Design spec):
 *   1. Continuous base rotation at constant speed (100°/s)
 *   2. Expand/contract cycle: head extends, then tail catches up
 *
 * Easing uses the Smoothstep polynomial (3x² - 2x³), which is the exact
 * closed-form simplification of the Fluent EasyEase cubic-bezier
 * (0.33, 0.0, 0.67, 1.0).  All math is integer-only -- no FPU/SSE.
 *
 * Part of the Progressive Spinner component.
 * ============================================================================ */

#include "kernel/spinner.h"
#include "kernel/gfx/arc_ring.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/timer.h"

/* ---- Animation timing ---- */

/* Animation runs at ~10fps via sleep_ms(100) polling loop */
#define SPINNER_CYCLE_FRAMES 20  /* frames per cycle (2.0s @ 10fps) */
#define SPINNER_HALF_CYCLE   10  /* expand phase = contract phase */

/* Arc sweep limits (in 0–255 angle units, mapping to 0–360°) */
#define SPINNER_MIN_SWEEP    11  /* ~15° -- forms a dot with rounded tips */
#define SPINNER_MAX_SWEEP    192 /* ~270° -- 3/4 of a circle */
#define SPINNER_DELTA        (SPINNER_MAX_SWEEP - SPINNER_MIN_SWEEP) /* 181 */

/* Start at 12 o'clock: -90° = 192 in uint8 (256 * 270/360) */
#define SPINNER_START_OFFSET 192

/* Base rotation speed: 100°/s at 10fps = 10°/frame.
 * In 256-step units: 10 × 256/360 ≈ 7.11/frame.
 * Integer approx: frame × 64 / 9 ≈ 7.11/frame. */
#define SPINNER_ROT_NUMER    64
#define SPINNER_ROT_DENOM    9

/* ---- State ---- */

static volatile uint8_t  s_active;   /* 1 = animation running */
static volatile uint32_t s_frame;    /* monotonic frame counter */
static int32_t  s_cx, s_cy;          /* ring center position */
static int32_t  s_radius;            /* outer radius (scaled) */
static int32_t  s_stroke;            /* ring thickness (scaled) */
static uint32_t s_color;             /* accent color (0x00RRGGBB) */

/* ---- Smoothstep easing ----
 *
 * f(x) = 3x² - 2x³  (Hermite interpolation)
 * Equivalent to Fluent EasyEase cubic-bezier(0.33, 0, 0.67, 1).
 *
 * Input:  x in [0, 256] (0.0 to 1.0 in fixed-point)
 * Output: f in [0, 256] (0.0 to 1.0 in fixed-point) */
static uint32_t smoothstep256(uint32_t x)
{
    if (x >= 256) return 256;
    /* x² max = 65536, x³ max = 16777216 -- fits uint32_t */
    uint32_t x2 = x * x;
    uint32_t x3 = x2 * x;
    return (3 * x2 / 256) - (2 * x3 / 65536);
}

/* ---- Frame computation ----
 *
 * Direct translation of the Fluent Design ProgressRing algorithm:
 *   cycle_offset = completed_cycles × delta  (cumulative, wrapping)
 *   Phase 1 (expand):  tail = cycle_offset, head eases forward
 *   Phase 2 (contract): head = cycle_offset + max, tail eases to catch up
 *   continuous_rotation = frame × 100°/s  (always clockwise)
 *   final angles = phase angles + rotation + start_offset(-90°) */
static void spinner_compute_frame(uint32_t frame,
                                  uint8_t *out_start, uint8_t *out_sweep)
{
    /* Cycle and phase within cycle */
    uint32_t cycle = frame / SPINNER_CYCLE_FRAMES;
    uint32_t phase = frame % SPINNER_CYCLE_FRAMES;

    /* Cumulative offset from completed cycles (wraps via & 0xFF) */
    uint32_t cycle_offset = cycle * SPINNER_DELTA;

    uint32_t tail, head;

    if (phase < SPINNER_HALF_CYCLE) {
        /* Expanding: tail anchored, head eases forward */
        uint32_t progress = phase * 256 / SPINNER_HALF_CYCLE;
        uint32_t ease = smoothstep256(progress);

        tail = cycle_offset;
        head = cycle_offset + SPINNER_MIN_SWEEP
             + SPINNER_DELTA * ease / 256;
    } else {
        /* Contracting: head anchored, tail eases forward to catch up */
        uint32_t p = phase - SPINNER_HALF_CYCLE;
        uint32_t progress = p * 256 / SPINNER_HALF_CYCLE;
        uint32_t ease = smoothstep256(progress);

        tail = cycle_offset + SPINNER_DELTA * ease / 256;
        head = cycle_offset + SPINNER_MAX_SWEEP;
    }

    /* Continuous base rotation (100°/s, independent of cycle) */
    uint32_t rotation = frame * SPINNER_ROT_NUMER / SPINNER_ROT_DENOM;

    /* Apply rotation and 12-o'clock offset */
    tail = (tail + rotation + SPINNER_START_OFFSET) & 0xFF;
    head = (head + rotation + SPINNER_START_OFFSET) & 0xFF;

    /* Sweep = angular distance from tail to head (unsigned wrap) */
    uint32_t sweep = (head - tail) & 0xFF;
    if (sweep < SPINNER_MIN_SWEEP) sweep = SPINNER_MIN_SWEEP;

    *out_start = (uint8_t)tail;
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

/* Non-blocking start -- registers a periodic timer tick callback.
 * The timer ISR drives animation at ~10fps (100Hz / 10).
 * spinner_advance() runs in interrupt context -- keep it fast. */
void spinner_start(void)
{
    if (s_active) return;
    s_active = 1;
    s_frame  = 0;
    timer_register_tick_callback(spinner_advance, 10);
}

/* Advance one animation frame.  Called from timer tick ISR callback.
 * Renders the current frame and swaps the spinner bounding rect.
 * Runs in interrupt context -- no sleeping, no locks. */
void spinner_advance(void)
{
    if (!s_active) return;

    s_frame++;
    spinner_render(255);

    /* Swap only the spinner's bounding rect (no full-screen swap) */
    uint32_t bx, by, bw, bh;
    spinner_get_bounds(&bx, &by, &bw, &bh);
    fb_swap_rect(bx, by, bw, bh);
}

void spinner_stop(void)
{
    s_active = 0;
    timer_unregister_tick_callback();
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
