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
static int       s_banner_shown; /* 1 if crash banner was rendered */
static uint32_t  s_row;        /* current Y position (next stage row) */
static uint8_t   s_last_phase; /* phase of the previous stage */
static uint32_t  s_last_row_y; /* Y position of the current in-progress stage */
static int       s_has_current; /* 1 if a stage is currently in-progress */

/* TSC for timing */
static uint64_t  s_stage_tsc;  /* TSC when current stage began */
static uint32_t  s_stage_count; /* total stages seen (for progress estimate) */

/* Progress bar constants */
#define VPD_BAR_HEIGHT    4
#define VPD_BAR_GAP       8   /* gap between last row and progress bar */
#define VPD_BAR_COLOR     0x000078D4  /* accent blue */
#define VPD_BAR_BG        0x00282828  /* dark gray track */

static inline uint64_t vpd_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Layout constants ---------------------------------------------------- */

#define VPD_SCALE          2   /* render text at 2× for readability */
#define VPD_CHAR_W        (VPD_CELL_W * VPD_SCALE)  /* 12px per char at 2× */
#define VPD_LEFT_MARGIN   12
#define VPD_TOP_MARGIN    14
#define VPD_ROW_HEIGHT    (VPD_GLYPH_H * VPD_SCALE + 8)  /* 22px: 14px text + 8px gap */
#define VPD_SQUARE_SIZE   (VPD_GLYPH_H * VPD_SCALE)      /* 14px square */
#define VPD_SQUARE_GAP     6
#define VPD_NAME_X        (VPD_LEFT_MARGIN + VPD_SQUARE_SIZE + VPD_SQUARE_GAP)
#define VPD_CODE_X        (VPD_NAME_X + 14 * VPD_CHAR_W)  /* fixed column for POST hex */
#define VPD_TIME_X        (VPD_CODE_X + 6 * VPD_CHAR_W)   /* fixed column for timing */
#define VPD_MS_X          (VPD_TIME_X + 5 * VPD_CHAR_W)   /* "ms" suffix always here */
#define VPD_MAX_ROWS      30
#define VPD_SEPARATOR_H    1
#define VPD_SEPARATOR_W   (VPD_MS_X + 3 * VPD_CHAR_W)     /* width covers all columns */

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

/* Draw a checkmark at 2× scale using the micro-font 'v' shape but custom */
static void vpd_draw_check(uint32_t x, uint32_t y, uint32_t color)
{
    /* 5×7 checkmark glyph, rendered at 2× */
    static const uint8_t check[7] = {
        0x00, /* ..... */
        0x08, /* ....X */
        0x08, /* ....X */
        0x10, /* ...X. */
        0x90, /* X..X. */
        0x60, /* .XX.. */
        0x20, /* ..X.. */
    };
    uint32_t row, col;
    for (row = 0; row < 7; row++) {
        uint8_t bits = check[row];
        for (col = 0; col < 5; col++) {
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

/* Render right-aligned timing: "  NNNms" with ms suffix at fixed position */
static void vpd_render_timing(uint32_t y, uint32_t ms)
{
    uint32_t v = ms, digits = 0;
    if (v == 0) digits = 1;
    while (v > 0) { digits++; v /= 10; }
    /* Right-align: number ends just before VPD_MS_X */
    uint32_t tx = VPD_MS_X - digits * VPD_CHAR_W;
    vpd_putu32_scaled(tx, y, ms, VPD_COLOR_PENDING);
    vpd_puts_scaled(VPD_MS_X, y, "ms", VPD_COLOR_PENDING);
}

/* Phase heading names */
static const char *s_phase_names[] = {
    "PHASE 0",
    "PHASE 1",
    "PHASE 2",
    "PHASE 3",
};

/* ---- Public API ---------------------------------------------------------- */

void vpd_init(void)
{
    /* Respect boot.conf postbars setting (0=off, 1=on, 2=diag).
     * If config not yet parsed (config_found=0), default is off. */
    if (g_boot_info.config.config_found && !g_boot_info.config.postbars) {
        s_active = 0;
        return;
    }
    /* If config not parsed yet but postbars defaults to 0, still init —
     * bare-metal diagnostics need VPD before config is available.
     * vpd_stop_tier1() will clean up if postbars ends up being 0. */

    if (!g_boot_info.fb_available || !g_boot_info.fb.addr) {
        s_active = 0;
        return;
    }

    s_fb       = (uint32_t *)(uintptr_t)g_boot_info.fb.addr;
    s_pitch_px = g_boot_info.fb.pitch / 4;
    s_width    = g_boot_info.fb.width;
    s_height   = g_boot_info.fb.height;
    /* Push stage list down if crash banner is displayed above */
    s_row      = s_banner_shown ? (VPD_TOP_MARGIN + VPD_ROW_HEIGHT + 4) : VPD_TOP_MARGIN;
    s_last_phase = 0xFF; /* no previous phase */
    s_has_current = 0;
    s_active   = 1;
}

void vpd_stage_begin(uint8_t phase, const char *name, uint16_t postcode)
{
    if (!s_active)
        return;

    /* Mark previous stage as done */
    if (s_has_current) {
        vpd_fill_rect(VPD_LEFT_MARGIN, s_last_row_y,
                       VPD_SQUARE_SIZE, VPD_SQUARE_SIZE, VPD_COLOR_BG);
        vpd_draw_check(VPD_LEFT_MARGIN, s_last_row_y, VPD_COLOR_DONE);

        if (g_boot_info.timing.tsc_freq > 0 && s_stage_tsc > 0) {
            uint64_t elapsed_tsc = vpd_rdtsc() - s_stage_tsc;
            uint32_t ms = (uint32_t)(elapsed_tsc * 1000 /
                                      g_boot_info.timing.tsc_freq);
            vpd_render_timing(s_last_row_y, ms);
        }
    }

    /* Phase heading + separator when phase changes */
    if (s_last_phase != 0xFF && phase != s_last_phase) {
        s_row += VPD_ROW_HEIGHT / 2;  /* extra gap before separator */
        vpd_fill_rect(VPD_LEFT_MARGIN, s_row,
                       VPD_SEPARATOR_W, VPD_SEPARATOR_H,
                       VPD_COLOR_SEPARATOR);
        s_row += VPD_SEPARATOR_H + VPD_ROW_HEIGHT / 2;  /* gap after separator */
    }
    if (phase != s_last_phase && phase < 4) {
        vpd_puts_scaled(VPD_LEFT_MARGIN, s_row, s_phase_names[phase],
                         VPD_COLOR_SEPARATOR);
        s_row += VPD_ROW_HEIGHT + 2;  /* heading + padding before first stage */
    }
    s_last_phase = phase;

    /* Bounds check */
    if (s_row + VPD_ROW_HEIGHT > s_height || s_row / VPD_ROW_HEIGHT > VPD_MAX_ROWS)
        return;

    /* Draw status square (yellow = in progress) */
    vpd_fill_rect(VPD_LEFT_MARGIN, s_row, VPD_SQUARE_SIZE, VPD_SQUARE_SIZE,
                   VPD_COLOR_PROGRESS);

    /* Draw stage name at fixed column */
    vpd_puts_scaled(VPD_NAME_X, s_row, name, VPD_COLOR_TEXT);

    /* Draw POST code at fixed column */
    vpd_puthex16_scaled(VPD_CODE_X, s_row, postcode, VPD_COLOR_PENDING);

    /* Record state for done/timing */
    s_last_row_y = s_row;
    s_has_current = 1;
    s_stage_tsc = vpd_rdtsc();

    /* Advance to next row */
    s_row += VPD_ROW_HEIGHT;

    s_stage_count++;
}

void vpd_update_progress(uint8_t percent)
{
    (void)percent; /* progress bar removed — too much visual clutter */
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
        vpd_render_timing(s_last_row_y, ms);
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

/* ---- POST16 code → name lookup ------------------------------------------ */

struct post16_entry { uint16_t code; const char *name; };

static const struct post16_entry s_post16_names[] = {
    { 0xB001, "EFI_MAIN" },     { 0xB010, "GOP" },
    { 0xB020, "KERNEL_LOAD" },  { 0xB050, "EXIT_BS" },
    { 0xB060, "PAGE_TABLES" },  { 0xB070, "KERNEL_JUMP" },
    { 0x0010, "SERIAL" },       { 0x0011, "SERIAL" },
    { 0x0012, "UEFI_RT" },      { 0x0013, "UEFI_RT" },
    { 0x0014, "UEFI_VARS" },    { 0x0016, "SECUREBOOT" },
    { 0x0018, "TPM" },
    { 0x0020, "PMM" },          { 0x0021, "PMM" },
    { 0x0030, "VMM" },          { 0x0031, "VMM" },
    { 0x0040, "HEAP" },         { 0x0041, "HEAP" },
    { 0x0050, "KLOG" },         { 0x0051, "KLOG" },
    { 0x0060, "CPUID" },        { 0x0061, "CPUID" },
    { 0x0070, "CPU_HARDEN" },   { 0x0080, "NX_POLICY" },
    { 0x0090, "SIMD" },         { 0x0091, "SIMD" },
    { 0x1000, "GDT" },          { 0x1001, "GDT" },
    { 0x1010, "IDT" },          { 0x1011, "IDT" },
    { 0x1020, "ACPI" },         { 0x1021, "ACPI" },
    { 0x1030, "LAPIC" },        { 0x1031, "LAPIC" },
    { 0x1040, "TIMER" },        { 0x1041, "TIMER" },
    { 0x1050, "RTC" },          { 0x1051, "RTC" },
    { 0x1060, "KEYBOARD" },     { 0x1061, "KEYBOARD" },
    { 0x1070, "MOUSE" },        { 0x1071, "MOUSE" },
    { 0x1080, "FB" },           { 0x1081, "FB" },
    { 0x1090, "SPLASH" },       { 0x1091, "SPLASH" },
    { 0x2000, "PCI" },          { 0x2001, "PCI" },
    { 0x2050, "AHCI" },         { 0x2051, "AHCI" },
    { 0x2060, "VFS" },          { 0x2061, "VFS" },
    { 0x2080, "REGISTRY" },     { 0x2081, "REGISTRY" },
    { 0x2090, "SMP" },          { 0x2091, "SMP" },
    { 0x3000, "SCHED" },        { 0x3001, "SCHED" },
    { 0x3030, "DESKTOP" },      { 0x3031, "DESKTOP" },
    { 0x3040, "COMPOSITOR" },
    { 0, (const char *)0 },  /* sentinel */
};

const char *vpd_post16_name(uint16_t code)
{
    const struct post16_entry *e = s_post16_names;
    while (e->name) {
        if (e->code == code) return e->name;
        e++;
    }
    return "UNKNOWN";
}

/* ---- Crash banner ------------------------------------------------------- */

void vpd_crash_banner(uint16_t last_postcode)
{
    uint32_t x;
    const char *name;

    if (!g_boot_info.fb_available || !g_boot_info.fb.addr)
        return;

    /* Ensure VPD state is set for rendering helpers */
    if (!s_fb) {
        s_fb       = (uint32_t *)(uintptr_t)g_boot_info.fb.addr;
        s_pitch_px = g_boot_info.fb.pitch / 4;
        s_width    = g_boot_info.fb.width;
        s_height   = g_boot_info.fb.height;
    }

    name = vpd_post16_name(last_postcode);
    s_banner_shown = 1;

    /* Render: "Last boot failed: NAME 0xNNNN" in red at 2× scale */
    x = VPD_LEFT_MARGIN;
    vpd_puts_scaled(x, VPD_TOP_MARGIN, "Last boot failed:", VPD_COLOR_FAIL);
    x += 18 * VPD_CHAR_W;
    vpd_puts_scaled(x, VPD_TOP_MARGIN, name, VPD_COLOR_TEXT);
    {
        const char *p = name;
        uint32_t len = 0;
        while (*p) { len++; p++; }
        x += (len + 1) * VPD_CHAR_W;
        vpd_puts_scaled(x, VPD_TOP_MARGIN, "0x", VPD_COLOR_PENDING);
        x += 2 * VPD_CHAR_W;
        vpd_puthex16_scaled(x, VPD_TOP_MARGIN, last_postcode, VPD_COLOR_PENDING);
    }

}

void vpd_stop_tier1(void)
{
    if (s_has_current)
        vpd_stage_done();
    s_active = 0;
}
