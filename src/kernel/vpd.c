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

#define VPD_SCALE          2   /* render text at 2× for readability */
#define VPD_LEFT_MARGIN    8
#define VPD_TOP_MARGIN     8
#define VPD_ROW_HEIGHT    (VPD_GLYPH_H * VPD_SCALE + 4)  /* 18px: 14px text + 4px gap */
#define VPD_SQUARE_SIZE   (VPD_GLYPH_H * VPD_SCALE)      /* 14px square */
#define VPD_SQUARE_GAP     6
#define VPD_NAME_X        (VPD_LEFT_MARGIN + VPD_SQUARE_SIZE + VPD_SQUARE_GAP)
#define VPD_MAX_ROWS      30
#define VPD_SEPARATOR_H    1
#define VPD_SEPARATOR_W  300  /* fixed width, not full screen */

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

/* 2× scaled text rendering — each font pixel becomes a 2×2 block */
static void vpd_puts_scaled(uint32_t x, uint32_t y, const char *s,
                              uint32_t color)
{
    while (*s) {
        char c = *s;
        const uint8_t *glyph;
        uint32_t row, col;

        if (c < VPD_FIRST || c > VPD_LAST) c = '?';
        glyph = vpd_font_data[c - VPD_FIRST];

        for (row = 0; row < VPD_GLYPH_H; row++) {
            uint8_t bits = glyph[row];
            for (col = 0; col < VPD_GLYPH_W; col++) {
                if (bits & (0x80 >> col)) {
                    uint32_t px = x + col * VPD_SCALE;
                    uint32_t py = y + row * VPD_SCALE;
                    if (px + 1 < s_width && py + 1 < s_height) {
                        s_fb[py       * s_pitch_px + px]     = color;
                        s_fb[py       * s_pitch_px + px + 1] = color;
                        s_fb[(py + 1) * s_pitch_px + px]     = color;
                        s_fb[(py + 1) * s_pitch_px + px + 1] = color;
                    }
                }
            }
        }
        x += VPD_CELL_W * VPD_SCALE;
        s++;
    }
}

static void vpd_puthex16_scaled(uint32_t x, uint32_t y, uint16_t val,
                                  uint32_t color)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[5];
    buf[0] = hex[(val >> 12) & 0xF];
    buf[1] = hex[(val >>  8) & 0xF];
    buf[2] = hex[(val >>  4) & 0xF];
    buf[3] = hex[ val        & 0xF];
    buf[4] = '\0';
    vpd_puts_scaled(x, y, buf, color);
}

static void vpd_putu32_scaled(uint32_t x, uint32_t y, uint32_t val,
                                uint32_t color)
{
    char buf[12];
    int i = 0, j;
    if (val == 0) { vpd_puts_scaled(x, y, "0", color); return; }
    while (val > 0 && i < 11) { buf[i++] = (char)('0' + val % 10); val /= 10; }
    /* Reverse into a proper string */
    {
        char rev[12];
        for (j = 0; j < i; j++) rev[j] = buf[i - 1 - j];
        rev[i] = '\0';
        vpd_puts_scaled(x, y, rev, color);
    }
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
            uint32_t tx = VPD_NAME_X + 18 * VPD_CELL_W * VPD_SCALE;
            vpd_putu32_scaled(tx, s_last_row_y, ms, VPD_COLOR_PENDING);
            /* Count digits to position "ms" suffix */
            {
                uint32_t v = ms, digits = 0;
                if (v == 0) digits = 1;
                while (v > 0) { digits++; v /= 10; }
                vpd_puts_scaled(tx + digits * VPD_CELL_W * VPD_SCALE,
                                 s_last_row_y, "ms", VPD_COLOR_PENDING);
            }
        }
    }

    /* Phase separator */
    if (s_last_phase != 0xFF && phase != s_last_phase) {
        vpd_fill_rect(VPD_LEFT_MARGIN, s_row,
                       VPD_SEPARATOR_W, VPD_SEPARATOR_H,
                       VPD_COLOR_SEPARATOR);
        s_row += VPD_SEPARATOR_H + 3;
    }
    s_last_phase = phase;

    /* Bounds check */
    if (s_row + VPD_ROW_HEIGHT > s_height || s_row / VPD_ROW_HEIGHT > VPD_MAX_ROWS)
        return;

    /* Draw status square (yellow = in progress) */
    vpd_fill_rect(VPD_LEFT_MARGIN, s_row, VPD_SQUARE_SIZE, VPD_SQUARE_SIZE,
                   VPD_COLOR_PROGRESS);

    /* Draw stage name at 2× scale */
    x = VPD_NAME_X;
    vpd_puts_scaled(x, s_row, name, VPD_COLOR_TEXT);

    /* Draw POST code after name at 2× scale */
    {
        const char *p = name;
        uint32_t name_len = 0;
        while (*p) { name_len++; p++; }
        x = VPD_NAME_X + (name_len + 1) * VPD_CELL_W * VPD_SCALE;
        vpd_puthex16_scaled(x, s_row, postcode, VPD_COLOR_PENDING);
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
        uint32_t tx = VPD_SEPARATOR_W - 80;
        vpd_putu32_scaled(tx, s_last_row_y, ms, VPD_COLOR_PENDING);
        {
            uint32_t v = ms, digits = 0;
            if (v == 0) digits = 1;
            while (v > 0) { digits++; v /= 10; }
            vpd_puts_scaled(tx + digits * VPD_CELL_W * VPD_SCALE,
                             s_last_row_y, "ms", VPD_COLOR_PENDING);
        }
    }

    s_has_current = 0;
}

void vpd_stage_fail(void)
{
    if (!s_active || !s_has_current)
        return;

    vpd_fill_rect(VPD_LEFT_MARGIN, s_last_row_y,
                   VPD_SQUARE_SIZE, VPD_SQUARE_SIZE, VPD_COLOR_FAIL);

    /* Draw FAIL text at 2× scale */
    vpd_puts_scaled(VPD_SEPARATOR_W - 60, s_last_row_y, "FAIL", VPD_COLOR_FAIL);

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
