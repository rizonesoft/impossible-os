/* ============================================================================
 * boot_progress.c -- Named-stage boot progress implementation
 *
 * XREF: 01-boot-platform/TODO-07-boot-diagnostics.md §2
 * ============================================================================ */

#include "kernel/boot_progress.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"
#include "kernel/boot_info.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_splash.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/mm/pmm.h"

/* ---- Stage metadata table ----------------------------------------------- */

typedef struct {
    uint8_t     phase;      /* boot phase number (0-3) */
    uint16_t    postcode;   /* POST16_* constant */
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
/* Thin 1-pixel stroke hex font (6×7 in an 8×8 cell, right-aligned).
 * Designed for subtle diagnostic display -- not bold like the old font. */
static const uint8_t s_hex_font[16][8] = {
    { 0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0x00 }, /* 0 */
    { 0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0x00 }, /* 1 */
    { 0x1C,0x22,0x02,0x04,0x08,0x10,0x3E,0x00 }, /* 2 */
    { 0x1C,0x22,0x02,0x0C,0x02,0x22,0x1C,0x00 }, /* 3 */
    { 0x04,0x0C,0x14,0x24,0x3E,0x04,0x04,0x00 }, /* 4 */
    { 0x3E,0x20,0x3C,0x02,0x02,0x22,0x1C,0x00 }, /* 5 */
    { 0x0C,0x10,0x20,0x3C,0x22,0x22,0x1C,0x00 }, /* 6 */
    { 0x3E,0x02,0x04,0x08,0x08,0x08,0x08,0x00 }, /* 7 */
    { 0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C,0x00 }, /* 8 */
    { 0x1C,0x22,0x22,0x1E,0x02,0x04,0x18,0x00 }, /* 9 */
    { 0x08,0x14,0x22,0x22,0x3E,0x22,0x22,0x00 }, /* A */
    { 0x3C,0x22,0x22,0x3C,0x22,0x22,0x3C,0x00 }, /* B */
    { 0x1C,0x22,0x20,0x20,0x20,0x22,0x1C,0x00 }, /* C */
    { 0x38,0x24,0x22,0x22,0x22,0x24,0x38,0x00 }, /* D */
    { 0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E,0x00 }, /* E */
    { 0x3E,0x20,0x20,0x3C,0x20,0x20,0x20,0x00 }, /* F */
};

#define POST_GLYPH_W 8   /* native 8px */
#define POST_GLYPH_H 8
#define POST_GAP     2   /* gap between digits */
#define POST_MARGIN  6   /* margin from screen edge */
#define POST_TOP     6   /* top padding */
#define POST_TOTAL_W (POST_GLYPH_W * 2 + POST_GAP)  /* 18 px */
#define POST_TOTAL_H (POST_GLYPH_H + 2)              /* 10 px */

/* ---- 16-bit POST display (4 hex digits, thin font, 1×1 native) ----------- */

#define POST16_GLYPH_W   8   /* 8 px wide -- native 1× */
#define POST16_GLYPH_H   8   /* 8 px tall -- native 1× */
#define POST16_GAP        2
#define POST16_MARGIN     6
#define POST16_TOP        6
#define POST16_DIGITS     4
#define POST16_TOTAL_W   (POST16_GLYPH_W * POST16_DIGITS + POST16_GAP * (POST16_DIGITS - 1))  /* 38 px */
#define POST16_TOTAL_H   (POST16_GLYPH_H + 2)  /* 10 px */

/* Render 4 hex digits. Two paths:
 *   Pre-fb_init:  direct VRAM write (works from kernel entry)
 *   Post-fb_init: fb_put_pixel + fb_swap_rect (respects page flip + back buffer) */
void post_display16(uint16_t code)
{
    uint32_t scr_w, x0, y0;
    int nibbles[4];
    int d;
    int use_fb_driver = kernel_subsystem_ready(SUBSYS_FB);

    /* Stop rendering once desktop compositor takes over */
    if (kernel_subsystem_ready(SUBSYS_DESKTOP))
        return;

    /* Respect boot.conf postcode=0 once config is parsed.
     * Before config parse (g_boot_info zeroed), always show --
     * early POST codes are the most important for diagnostics. */
    if (g_boot_info.config.config_found && !g_boot_info.config.postcode)
        return;

    if (!g_boot_info.fb_available || !g_boot_info.fb.addr)
        return;

    scr_w = use_fb_driver ? fb_get_width() : g_boot_info.fb.width;
    if (scr_w == 0) return;

    x0 = scr_w - POST16_TOTAL_W - POST16_MARGIN;
    y0 = POST16_TOP;

    /* Extract 4 nibbles */
    nibbles[0] = (code >> 12) & 0x0F;
    nibbles[1] = (code >>  8) & 0x0F;
    nibbles[2] = (code >>  4) & 0x0F;
    nibbles[3] =  code        & 0x0F;

    if (use_fb_driver) {
        /* Post-fb_init: use framebuffer driver (handles page flip + back buffer).
         * Clear + draw + swap in one batch -- no intermediate fb_swap between
         * clear and draw, so there's no visible flash frame. */
        fb_fill_rect(x0, y0, POST16_TOTAL_W, POST16_TOTAL_H, 0x00000000);
        for (d = 0; d < 4; d++) {
            const uint8_t *g = s_hex_font[nibbles[d]];
            uint32_t gx = x0 + d * (POST16_GLYPH_W + POST16_GAP);
            uint32_t row, col;
            for (row = 0; row < 8; row++) {
                uint8_t bits = g[row];
                for (col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        uint32_t px = gx + col;
                        uint32_t py = y0 + row;
                        fb_put_pixel(px, py, 0x00C0C0C0);
                    }
                }
            }
        }
        fb_swap_rect(x0, y0, POST16_TOTAL_W, POST16_TOTAL_H);
    } else {
        /* Pre-fb_init: direct VRAM write */
        uint32_t *fb = (uint32_t *)(uintptr_t)g_boot_info.fb.addr;
        uint32_t pitch_px = g_boot_info.fb.pitch / 4;
        uint32_t cy, cx;
        for (cy = y0; cy < y0 + POST16_TOTAL_H && cy < g_boot_info.fb.height; cy++)
            for (cx = x0; cx < x0 + POST16_TOTAL_W && cx < scr_w; cx++)
                fb[cy * pitch_px + cx] = 0x00000000;
        for (d = 0; d < 4; d++) {
            const uint8_t *g = s_hex_font[nibbles[d]];
            uint32_t gx = x0 + d * (POST16_GLYPH_W + POST16_GAP);
            uint32_t row, col;
            for (row = 0; row < 8; row++) {
                uint8_t bits = g[row];
                for (col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        uint32_t px = gx + col;
                        uint32_t py = y0 + row;
                        if (px < scr_w && py < g_boot_info.fb.height)
                            fb[py * pitch_px + px] = 0x00C0C0C0;
                    }
                }
            }
        }
    }
}

/* ---- Boot timeline JSON dump --------------------------------------------- */

#include "kernel/fs/vfs.h"
#include "libc/string.h"

void boot_timeline_dump_json(void)
{
    const boot_timing_step_t *steps;
    uint32_t count, i;
    struct vfs_node *f;
    uint8_t *buf;
    uint32_t pos = 0;
    uint32_t buf_size = 8192;
    uint64_t freq, base;

    count = boot_timing_get_steps(&steps);
    if (count < 2) return;

    freq = boot_timing_tsc_freq();
    if (freq == 0) return;
    base = steps[0].tsc;

    buf = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(2);  /* 8 KB */
    if (!buf) return;

    /* JSON array */
    buf[pos++] = '[';
    buf[pos++] = '\n';

    for (i = 0; i < count && pos < buf_size - 128; i++) {
        uint32_t start_ms = (uint32_t)((steps[i].tsc - base) / (freq / 1000));
        uint32_t dur_ms = 0;
        if (i + 1 < count)
            dur_ms = (uint32_t)((steps[i + 1].tsc - steps[i].tsc) / (freq / 1000));

        pos += (uint32_t)snprintf((char *)buf + pos, buf_size - pos,
            "  {\"stage\":\"%s\",\"phase\":%u,\"post\":\"0x%02x\","
            "\"start_ms\":%u,\"duration_ms\":%u}%s\n",
            steps[i].step ? steps[i].step : "?",
            (unsigned)steps[i].phase,
            (unsigned)steps[i].postcode,
            (unsigned)start_ms,
            (unsigned)dur_ms,
            (i + 1 < count) ? "," : "");
    }

    buf[pos++] = ']';
    buf[pos++] = '\n';

    /* Write to X:\Boot\ (BlackBox) or C:\Impossible\System\Logs\ (fallback) */
    {
        const char *tl_dir = klog_using_blackbox ? "X:\\Boot\\" : klog_dir;
        char tl_path[64];
        int tp = 0, tj;
        for (tj = 0; tl_dir[tj]; tj++) tl_path[tp++] = tl_dir[tj];
        {
            const char *fn = "boot-timeline.json";
            for (tj = 0; fn[tj]; tj++) tl_path[tp++] = fn[tj];
        }
        tl_path[tp] = '\0';
        /* Create file via parent dir to avoid FAT32 dir cache re-walk bug */
        {
            struct vfs_node *dir = vfs_open(tl_dir, VFS_O_READ);
            if (dir && dir->ops && dir->ops->create)
                dir->ops->create(dir, "boot-timeline.json", VFS_FILE);
        }
        f = vfs_open(tl_path, VFS_O_WRITE);
    }
    if (f) {
        vfs_write(f, 0, pos, buf);
        vfs_close(f);
    }

    {
        uint32_t pg;
        for (pg = 0; pg < 2; pg++)
            pmm_free_frame((uintptr_t)buf + pg * 4096);
    }
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
    post_display16((uint16_t)m->postcode);

    /* Desktop ready: clear POST display + render debug bar */
    if (stage == BOOT_STAGE_DESKTOP_READY && kernel_subsystem_ready(SUBSYS_FB)) {
        uint32_t cx = fb_get_width() - POST_TOTAL_W - POST_MARGIN;
        fb_fill_rect(cx, POST_TOP, POST_TOTAL_W, POST_TOTAL_H, 0x00000000);
        fb_swap_rect(cx, POST_TOP, POST_TOTAL_W, POST_TOTAL_H);
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
