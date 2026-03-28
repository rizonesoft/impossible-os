/* ============================================================================
 * vpd.c -- Visual POST Display, Tier 1 (pre-splash)
 *
 * Renders named boot stages with status indicators directly to VRAM
 * using the embedded 5×7 micro-font.  Zero dependencies on heap, PMM,
 * klog, or framebuffer driver.
 *
 * Layout (top-left corner of screen):
 *   Each stage: [status_square 5×5] [name text] [POST hex] [+NNNms]
 *   10px per row (7px text + 3px gap)
 *   Phase separators: 1px gray line between phase groups
 * ============================================================================ */

#include "kernel/vpd.h"
#include "kernel/vpd_font.h"
#include "kernel/boot_info.h"

/* ---- State --------------------------------------------------------------- */

static uint32_t *s_fb;
static uint32_t  s_pitch_px;
static uint32_t  s_width;
static uint32_t  s_height;
static int       s_active;
static uint32_t  s_row;        /* current Y position (next stage row) */
static uint8_t   s_last_phase; /* phase of the previous stage */
static uint32_t  s_last_row_y; /* Y position of the current in-progress stage */
static int       s_has_current; /* 1 if a stage is currently in-progress */

/* TSC for timing */
static uint64_t  s_stage_tsc;  /* TSC when current stage began */

static inline uint64_t vpd_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Layout constants ---------------------------------------------------- */

#define VPD_LEFT_MARGIN   4
#define VPD_TOP_MARGIN    4
#define VPD_ROW_HEIGHT   10   /* 7px text + 3px gap */
#define VPD_SQUARE_SIZE   5
#define VPD_SQUARE_GAP    3
#define VPD_NAME_X       (VPD_LEFT_MARGIN + VPD_SQUARE_SIZE + VPD_SQUARE_GAP)
#define VPD_MAX_ROWS     30   /* max stages visible */
#define VPD_SEPARATOR_H   1

/* ---- Colors -------------------------------------------------------------- */

#define VPD_COLOR_BG       0x00000000
#define VPD_COLOR_TEXT     0x00C0C0C0
#define VPD_COLOR_DONE     0x0000CC00
#define VPD_COLOR_PROGRESS 0x00CCCC00
#define VPD_COLOR_FAIL     0x00FF2222
#define VPD_COLOR_PENDING  0x00404040
#define VPD_COLOR_SEPARATOR 0x00333333

/* ---- Helpers ------------------------------------------------------------- */

static void vpd_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t color)
{
    uint32_t cy, cx;
    for (cy = y; cy < y + h && cy < s_height; cy++)
        for (cx = x; cx < x + w && cx < s_width; cx++)
            s_fb[cy * s_pitch_px + cx] = color;
}

/* ---- Public API ---------------------------------------------------------- */

void vpd_init(void)
{
    if (!g_boot_info.fb_available || !g_boot_info.fb.addr) {
        s_active = 0;
        return;
    }

    s_fb       = (uint32_t *)(uintptr_t)g_boot_info.fb.addr;
    s_pitch_px = g_boot_info.fb.pitch / 4;
    s_width    = g_boot_info.fb.width;
    s_height   = g_boot_info.fb.height;
    s_row      = VPD_TOP_MARGIN;
    s_last_phase = 0xFF; /* no previous phase */
    s_has_current = 0;
    s_active   = 1;
}

void vpd_stage_begin(uint8_t phase, const char *name, uint16_t postcode)
{
    uint32_t x;

    if (!s_active)
        return;

    /* Mark previous stage as done */
    if (s_has_current) {
        vpd_fill_rect(VPD_LEFT_MARGIN, s_last_row_y,
                       VPD_SQUARE_SIZE, VPD_SQUARE_SIZE, VPD_COLOR_DONE);

        /* Render elapsed time if TSC is available */
        if (g_boot_info.timing.tsc_freq > 0 && s_stage_tsc > 0) {
            uint64_t elapsed_tsc = vpd_rdtsc() - s_stage_tsc;
            uint32_t ms = (uint32_t)(elapsed_tsc * 1000 /
                                      g_boot_info.timing.tsc_freq);
            /* Right-align timing: +NNNms */
            uint32_t tx = s_width - 60;
            vpd_putchar(s_fb, s_pitch_px, tx, s_last_row_y, '+', VPD_COLOR_TEXT);
            tx += VPD_CELL_W;
            vpd_putu32(s_fb, s_pitch_px, tx, s_last_row_y, ms, VPD_COLOR_TEXT);
        }
    }

    /* Phase separator */
    if (s_last_phase != 0xFF && phase != s_last_phase) {
        vpd_fill_rect(VPD_LEFT_MARGIN, s_row,
                       s_width - VPD_LEFT_MARGIN * 2, VPD_SEPARATOR_H,
                       VPD_COLOR_SEPARATOR);
        s_row += VPD_SEPARATOR_H + 2;
    }
    s_last_phase = phase;

    /* Bounds check */
    if (s_row + VPD_ROW_HEIGHT > s_height || s_row / VPD_ROW_HEIGHT > VPD_MAX_ROWS)
        return;

    /* Draw status square (yellow = in progress) */
    vpd_fill_rect(VPD_LEFT_MARGIN, s_row, VPD_SQUARE_SIZE, VPD_SQUARE_SIZE,
                   VPD_COLOR_PROGRESS);

    /* Draw stage name */
    x = VPD_NAME_X;
    vpd_puts(s_fb, s_pitch_px, x, s_row, name, VPD_COLOR_TEXT);

    /* Draw POST code after name */
    {
        /* Find end of name */
        const char *p = name;
        uint32_t name_len = 0;
        while (*p) { name_len++; p++; }
        x = VPD_NAME_X + (name_len + 1) * VPD_CELL_W;
        vpd_puthex16(s_fb, s_pitch_px, x, s_row, postcode, VPD_COLOR_PENDING);
    }

    /* Record state for done/timing */
    s_last_row_y = s_row;
    s_has_current = 1;
    s_stage_tsc = vpd_rdtsc();

    /* Advance to next row */
    s_row += VPD_ROW_HEIGHT;
}

void vpd_stage_done(void)
{
    if (!s_active || !s_has_current)
        return;

    /* Mark current stage green */
    vpd_fill_rect(VPD_LEFT_MARGIN, s_last_row_y,
                   VPD_SQUARE_SIZE, VPD_SQUARE_SIZE, VPD_COLOR_DONE);

    /* Render elapsed time */
    if (g_boot_info.timing.tsc_freq > 0 && s_stage_tsc > 0) {
        uint64_t elapsed_tsc = vpd_rdtsc() - s_stage_tsc;
        uint32_t ms = (uint32_t)(elapsed_tsc * 1000 /
                                  g_boot_info.timing.tsc_freq);
        uint32_t tx = s_width - 60;
        vpd_putchar(s_fb, s_pitch_px, tx, s_last_row_y, '+', VPD_COLOR_TEXT);
        tx += VPD_CELL_W;
        vpd_putu32(s_fb, s_pitch_px, tx, s_last_row_y, ms, VPD_COLOR_TEXT);
    }

    s_has_current = 0;
}

void vpd_stage_fail(void)
{
    if (!s_active || !s_has_current)
        return;

    vpd_fill_rect(VPD_LEFT_MARGIN, s_last_row_y,
                   VPD_SQUARE_SIZE, VPD_SQUARE_SIZE, VPD_COLOR_FAIL);

    /* Draw FAIL text */
    vpd_puts(s_fb, s_pitch_px, s_width - 36, s_last_row_y,
             "FAIL", VPD_COLOR_FAIL);

    s_has_current = 0;
}

int vpd_is_active(void)
{
    return s_active;
}

void vpd_stop_tier1(void)
{
    if (s_has_current)
        vpd_stage_done();
    s_active = 0;
}
