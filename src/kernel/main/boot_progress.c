/* ============================================================================
 * boot_progress.c -- Named-stage boot progress implementation
 *
 * XREF: 01-boot-platform/TODO-02-boot-diagnostics.md §2
 * ============================================================================ */

#include "kernel/boot_progress.h"
#include "kernel/boot_init.h"
#include "kernel/boot_info.h"
#include "kernel/boot_timing.h"
/* boot_timing_get_steps() used by render_debug_bar() */
#include "kernel/boot_splash.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"

/* ---- Stage metadata table ----------------------------------------------- */

typedef struct {
    uint8_t     phase;      /* boot phase number (0-3) */
    uint8_t     postcode;   /* POSTCODE_* constant */
    uint8_t     percent;    /* splash progress 0-100 */
    const char *name;       /* stage name for serial log */
} stage_meta_t;

static const stage_meta_t s_meta[BOOT_STAGE_COUNT] = {
    [BOOT_STAGE_UEFI_INIT]     = { 0, 0x10,   0, "UEFI_INIT"     },
    [BOOT_STAGE_ELF_LOADED]    = { 0, 0x11,   3, "ELF_LOADED"    },
    [BOOT_STAGE_KERNEL_ENTRY]  = { 0, 0x10,   5, "KERNEL_ENTRY"  },
    [BOOT_STAGE_GDT_IDT]       = { 1, 0x30,  10, "GDT_IDT"       },
    [BOOT_STAGE_APIC]          = { 1, 0x33,  15, "APIC"          },
    [BOOT_STAGE_PMM]           = { 0, 0x20,  20, "PMM"           },
    [BOOT_STAGE_VMM]           = { 0, 0x21,  25, "VMM"           },
    [BOOT_STAGE_HEAP]          = { 0, 0x22,  30, "HEAP"          },
    [BOOT_STAGE_KLOG]          = { 0, 0x23,  35, "KLOG"          },
    [BOOT_STAGE_VFS]           = { 2, 0x52,  50, "VFS"           },
    [BOOT_STAGE_REGISTRY]      = { 2, 0x53,  60, "REGISTRY"      },
    [BOOT_STAGE_DRIVERS]       = { 2, 0x51,  45, "DRIVERS"       },
    [BOOT_STAGE_NETWORK]       = { 2, 0x55,  55, "NETWORK"       },
    [BOOT_STAGE_SCHEDULER]     = { 3, 0x60,  75, "SCHEDULER"     },
    [BOOT_STAGE_DESKTOP_READY] = { 3, 0x63, 100, "DESKTOP_READY" },
};

/* ---- History ring ------------------------------------------------------- */

static boot_stage_entry_t s_history[BOOT_STAGE_HISTORY_MAX];
static uint32_t           s_history_count;

/* TSC at kernel entry -- used as elapsed-ms baseline */
static uint64_t s_kernel_entry_tsc;

/* ---- Inline TSC read ---------------------------------------------------- */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Hex helpers for serial output -------------------------------------- */

static void serial_write_dec(uint32_t val)
{
    char buf[12];
    int i = 0;
    if (val == 0) { serial_putchar('0'); return; }
    while (val > 0) { buf[i++] = '0' + (char)(val % 10); val /= 10; }
    while (i > 0) serial_putchar(buf[--i]);
}

/* ---- POST hex display (framebuffer corner + I/O port 0x80) -------------- */

/* 8x8 hex font: 0-9, A-F (16 glyphs x 8 bytes = 128 bytes) */
static const uint8_t s_hex_font[16][8] = {
    { 0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00 }, /* 0 */
    { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 }, /* 1 */
    { 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 }, /* 2 */
    { 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 }, /* 3 */
    { 0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00 }, /* 4 */
    { 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 }, /* 5 */
    { 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 }, /* 6 */
    { 0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00 }, /* 7 */
    { 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 }, /* 8 */
    { 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 }, /* 9 */
    { 0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00 }, /* A */
    { 0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00 }, /* B */
    { 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00 }, /* C */
    { 0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00 }, /* D */
    { 0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00 }, /* E */
    { 0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00 }, /* F */
};

#define POST_GLYPH_W 12  /* 8 * 1.5 = 12 px */
#define POST_GLYPH_H 12  /* 8 * 1.5 = 12 px */
#define POST_GAP     2   /* gap between digits */
#define POST_MARGIN  6   /* margin from screen edge */
#define POST_TOP     6   /* top padding */
#define POST_TOTAL_W (POST_GLYPH_W * 2 + POST_GAP)  /* 26 px */
#define POST_TOTAL_H (POST_GLYPH_H + 2)              /* 14 px */

/* Port 0x80 write */
static inline void outb_post(uint8_t code)
{
    __asm__ volatile ("outb %0, $0x80" :: "a"(code));
}

void post_display(uint8_t code)
{
    uint32_t scr_w, x0, y0;
    int hi, lo;

    /* Always write to I/O port 0x80 for hardware POST cards */
    outb_post(code);

    /* Disabled by boot.conf postcode=0 */
    if (!g_boot_info.config.postcode)
        return;

    /* Stop rendering once desktop compositor takes over */
    if (kernel_subsystem_ready(SUBSYS_DESKTOP))
        return;

    /* Skip pixel writes if framebuffer not ready */
    if (!kernel_subsystem_ready(SUBSYS_FB))
        return;

    scr_w = fb_get_width();
    if (scr_w == 0) return;

    x0 = scr_w - POST_TOTAL_W - POST_MARGIN;
    y0 = POST_TOP;

    /* Clear background */
    fb_fill_rect(x0, y0, POST_TOTAL_W, POST_TOTAL_H, 0x00000000);

    hi = (code >> 4) & 0x0F;
    lo = code & 0x0F;

    /* Draw high nibble (1.5x scale: 8px -> 12px via nearest-neighbor) */
    {
        const uint8_t *g = s_hex_font[hi];
        uint32_t py, px;
        for (py = 0; py < POST_GLYPH_H; py++) {
            uint32_t sy = (py * 8) / POST_GLYPH_H;  /* map back to 0-7 */
            uint8_t bits = g[sy];
            for (px = 0; px < POST_GLYPH_W; px++) {
                uint32_t sx = (px * 8) / POST_GLYPH_W;
                if (bits & (0x80 >> sx))
                    fb_put_pixel(x0 + px, y0 + py, 0x00FFFFFF);
            }
        }
    }

    /* Draw low nibble (1.5x scale) */
    {
        const uint8_t *g = s_hex_font[lo];
        uint32_t gx = x0 + POST_GLYPH_W + POST_GAP;
        uint32_t py, px;
        for (py = 0; py < POST_GLYPH_H; py++) {
            uint32_t sy = (py * 8) / POST_GLYPH_H;
            uint8_t bits = g[sy];
            for (px = 0; px < POST_GLYPH_W; px++) {
                uint32_t sx = (px * 8) / POST_GLYPH_W;
                if (bits & (0x80 >> sx))
                    fb_put_pixel(gx + px, y0 + py, 0x00FFFFFF);
            }
        }
    }

    fb_swap_rect(x0, y0, POST_TOTAL_W, POST_TOTAL_H);
}

/* ---- Debug bar: proportional boot stage waterfall ----------------------- */

#define BAR_HEIGHT  4
#define BAR_Y       0   /* top of screen */

/* Map postcode to a color for the debug bar */
static uint32_t postcode_color(uint8_t pc)
{
    if (pc >= 0x10 && pc <= 0x19) return 0x00404040;  /* Phase 0 early: dark gray */
    if (pc == 0x20) return 0x000000FF;  /* PMM: blue */
    if (pc == 0x21) return 0x008000FF;  /* VMM: purple */
    if (pc == 0x22) return 0x004080FF;  /* HEAP: light blue */
    if (pc == 0x23) return 0x004080FF;  /* KLOG: light blue */
    if (pc >= 0x30 && pc <= 0x34) return 0x00808080;  /* GDT/IDT/ACPI/LAPIC: gray */
    if (pc == 0x35) return 0x00C0C000;  /* TIMER: yellow */
    if (pc >= 0x40 && pc <= 0x41) return 0x00606060;  /* SMBIOS/FB: dim gray */
    if (pc == 0x50) return 0x00FF8000;  /* PCI: orange */
    if (pc == 0x51) return 0x00FF8000;  /* STORAGE: orange */
    if (pc == 0x52) return 0x0000C000;  /* VFS: green */
    if (pc == 0x53) return 0x0000FFAA;  /* REGISTRY: teal */
    if (pc == 0x54) return 0x00808080;  /* SMP: gray */
    if (pc == 0x55) return 0x0000FFFF;  /* NET: cyan */
    if (pc == 0x60) return 0x00FF0000;  /* SCHED: red */
    if (pc == 0x63) return 0x00FFFFFF;  /* DESKTOP: white */
    return 0x00303030;
}

void render_debug_bar(void)
{
    const boot_timing_step_t *steps;
    uint32_t count, scr_w, i, x;
    uint64_t total_tsc;

    /* Show when postcode=1 (default) or debug=1 */
    if (!g_boot_info.config.postcode && !g_boot_info.config.debug)
        return;
    if (!kernel_subsystem_ready(SUBSYS_FB))
        return;

    count = boot_timing_get_steps(&steps);
    if (count < 2) return;

    scr_w = fb_get_width();
    if (scr_w == 0) return;

    total_tsc = steps[count - 1].tsc - steps[0].tsc;
    if (total_tsc == 0) return;

    x = 0;
    for (i = 0; i + 1 < count; i++) {
        uint64_t seg_tsc = steps[i + 1].tsc - steps[i].tsc;
        uint32_t seg_w = (uint32_t)((seg_tsc * (uint64_t)scr_w) / total_tsc);
        uint32_t color = postcode_color(steps[i].postcode);
        uint32_t row, col;

        if (seg_w == 0) seg_w = 1;
        if (x + seg_w > scr_w) seg_w = scr_w - x;

        for (row = 0; row < BAR_HEIGHT; row++)
            for (col = 0; col < seg_w; col++)
                fb_put_pixel(x + col, BAR_Y + row, color);

        x += seg_w;
    }

    if (x < scr_w) {
        uint32_t color = postcode_color(steps[count - 1].postcode);
        uint32_t row, col;
        for (row = 0; row < BAR_HEIGHT; row++)
            for (col = x; col < scr_w; col++)
                fb_put_pixel(col, BAR_Y + row, color);
    }

    fb_swap_rect(0, BAR_Y, scr_w, BAR_HEIGHT);
}

/* ---- API ---------------------------------------------------------------- */

void boot_stage_report(boot_stage_t stage, const char *msg)
{
    uint64_t now = rdtsc();
    uint32_t elapsed = 0;
    const stage_meta_t *m;

    if ((uint32_t)stage >= BOOT_STAGE_COUNT)
        return;

    m = &s_meta[stage];

    /* Record kernel entry baseline */
    if (stage == BOOT_STAGE_KERNEL_ENTRY)
        s_kernel_entry_tsc = now;

    /* Compute elapsed ms */
    if (s_kernel_entry_tsc > 0) {
        uint64_t freq = boot_timing_tsc_freq();
        if (freq > 0) {
            uint64_t delta = now - s_kernel_entry_tsc;
            elapsed = (uint32_t)(delta / (freq / 1000));
        }
    }

    /* Record in history ring */
    if (s_history_count < BOOT_STAGE_HISTORY_MAX) {
        boot_stage_entry_t *e = &s_history[s_history_count++];
        e->stage      = stage;
        e->tsc        = now;
        e->elapsed_ms = elapsed;
        e->msg        = msg;
    }

    /* Serial log: [+NNNms] STAGE_NAME: msg */
    serial_write("[+");
    serial_write_dec(elapsed);
    serial_write("ms] ");
    serial_write(m->name);
    serial_write(": ");
    serial_write(msg ? msg : "");
    serial_write("\n");

    /* POST hex display on framebuffer + I/O port 0x80 */
    post_display(m->postcode);

    /* Desktop ready: clear POST display + render debug bar */
    if (stage == BOOT_STAGE_DESKTOP_READY && kernel_subsystem_ready(SUBSYS_FB)) {
        uint32_t cx = fb_get_width() - POST_TOTAL_W - POST_MARGIN;
        fb_fill_rect(cx, POST_TOP, POST_TOTAL_W, POST_TOTAL_H, 0x00000000);
        fb_swap_rect(cx, POST_TOP, POST_TOTAL_W, POST_TOTAL_H);
        render_debug_bar();
    }

    /* Forward to boot_progress (phase, step, postcode) */
    boot_progress(m->phase, msg ? msg : m->name, m->postcode);
}

uint32_t boot_get_elapsed_ms(void)
{
    uint64_t freq, delta;
    if (s_kernel_entry_tsc == 0) return 0;
    freq = boot_timing_tsc_freq();
    if (freq == 0) return 0;
    delta = rdtsc() - s_kernel_entry_tsc;
    return (uint32_t)(delta / (freq / 1000));
}

const boot_stage_entry_t *boot_stage_history_get(uint32_t *out_count)
{
    if (out_count) *out_count = s_history_count;
    return s_history;
}

void boot_progress_poll(void)
{
    if (s_history_count > 0 && boot_splash_active()) {
        const boot_stage_entry_t *last = &s_history[s_history_count - 1];
        boot_splash_status(last->msg ? last->msg : "");
    }
}
