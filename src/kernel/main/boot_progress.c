/* ============================================================================
 * boot_progress.c -- Named-stage boot progress implementation
 *
 * XREF: 01-boot-platform/TODO-14-boot-diagnostics.md
 * ============================================================================ */

#include "kernel/boot_progress.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"
#include "kernel/boot_info.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_perf_budget.h"
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

/* ---- Stage history (cap BOOT_STAGE_HISTORY_MAX, no wrap) ---------------- */

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

/* TSC delta to milliseconds -- avoids divide-by-zero when freq / 1000 == 0 */
static uint32_t boot_prog_tsc_delta_ms(uint64_t delta, uint64_t freq)
{
    if (freq < 1000)
        return 0;
    return (uint32_t)((delta * 1000ULL) / freq);
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
    /* Reject undersized modes: the 4-digit overlay needs at least its footprint
     * plus margin. A narrower mode would underflow x0 (unsigned wrap) and the
     * post-fb_init fb_fill_rect / fb_put_pixel path would write out of bounds. */
    if (scr_w <= POST16_TOTAL_W + POST16_MARGIN) return;

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

/* Worst case per record: 32-char stage + 10-digit start_ms + 10-digit
 * duration_ms + JSON scaffolding ~ 160 bytes. With BOOT_TIMING_MAX_STEPS=64
 * TSC entries and 5 FPDT entries the upper bound is ~11 KB. Round up to
 * 16 KB (4 frames) so a worst-case timeline never truncates. */
#define BOOT_TIMELINE_BUF_PAGES 4
#define BOOT_TIMELINE_BUF_SIZE  (BOOT_TIMELINE_BUF_PAGES * 4096u)
#define BOOT_TIMELINE_RECORD_MAX 192   /* slack over the ~160 worst-case */

/* Safe TSC delta -> ms: clamps reverse-ordered timestamps to 0 instead of
 * wrapping the unsigned subtraction. */
static uint32_t safe_tsc_delta_ms(uint64_t later, uint64_t earlier)
{
    if (later <= earlier) return 0;
    return boot_timing_tsc_delta_ms(later - earlier);
}

/* Saturating uint32 add. Returns UINT32_MAX on overflow so callers can
 * detect the saturation (any UINT32_MAX result with a non-saturated input
 * is by definition a wrapped sum). */
static uint32_t sat_add_u32(uint32_t a, uint32_t b)
{
    uint32_t s = a + b;
    if (s < a) return 0xFFFFFFFFu;
    return s;
}

void boot_timeline_dump_json(void)
{
    const boot_timing_step_t *steps;
    uint32_t count, i;
    struct vfs_node *f;
    uint8_t *buf;
    uint32_t pos = 0;
    uint64_t freq, base;

    count = boot_timing_get_steps(&steps);
    if (count < 2) return;

    freq = boot_timing_tsc_freq();
    if (freq < 1000)
        return;
    (void)freq;  /* delta math now goes through boot_timing_tsc_delta_ms */
    base = steps[0].tsc;

    buf = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(BOOT_TIMELINE_BUF_PAGES);
    if (!buf) return;

    /* Anchor TSC steps to firmware reset when FPDT is reliable, otherwise
     * to bl_entry. boot_timing_bl_entry_ms_since_reset() returns 0 in the
     * unreliable case so the offset becomes a no-op. The safe delta
     * refuses wraparound when steps[0] precedes bl_entry.
     *
     * tsc_unreliable propagates to every emitted TSC record so consumers
     * can distinguish ms-since-reset (FPDT reliable, anchored) from
     * ms-since-bl_entry (FPDT unavailable/garbage, fallback). Without
     * this, fallback boots looked like absolute timelines.  */
    uint32_t reset_anchor_ms = boot_timing_bl_entry_ms_since_reset();
    uint64_t bl_entry = g_boot_info.timing.bl_entry;
    uint32_t step0_offset_ms = safe_tsc_delta_ms(steps[0].tsc, bl_entry);
    uint32_t tsc_anchor_ms = sat_add_u32(reset_anchor_ms, step0_offset_ms);
    /* TSC entries are absolute ms-since-reset only when the FPDT firmware
     * anchor AND the bootloader bl_entry sample are valid AND none of the
     * arithmetic saturated. step0_offset_ms == UINT32_MAX or the anchor
     * sum at UINT32_MAX both signal an overflowed base. */
    int tsc_unreliable = (reset_anchor_ms == 0) || (bl_entry == 0)
                         || (steps[0].tsc < bl_entry)
                         || (step0_offset_ms == 0xFFFFFFFFu)
                         || (tsc_anchor_ms == 0xFFFFFFFFu);

    /* Prepend up to 5 FPDT phase entries so the timeline starts at firmware
     * reset (or all-zero with unreliable=true on VirtualBox-style firmware
     * that publishes an empty FPDT record). */
    boot_timing_fpdt_entry_t fpdt[5];
    uint32_t fpdt_n = boot_timing_get_fpdt_entries(fpdt, 5);
    uint32_t total = fpdt_n + count;

    /* JSON array */
    buf[pos++] = '[';
    buf[pos++] = '\n';

    uint32_t emitted = 0;

    for (uint32_t k = 0; k < fpdt_n; k++) {
        if (pos + BOOT_TIMELINE_RECORD_MAX >= BOOT_TIMELINE_BUF_SIZE)
            goto close;
        int last = (emitted + 1 == total);
        int written = snprintf((char *)buf + pos,
            BOOT_TIMELINE_BUF_SIZE - pos,
            "  {\"stage\":\"%s\",\"phase\":0,\"post\":\"0x0000\","
            "\"start_ms\":%u,\"duration_ms\":%u,\"target_ms\":0,"
            "\"source\":\"fpdt\",\"unreliable\":%s}%s\n",
            fpdt[k].stage,
            (unsigned)fpdt[k].start_ms,
            (unsigned)fpdt[k].duration_ms,
            fpdt[k].unreliable ? "true" : "false",
            last ? "" : ",");
        if (written <= 0 ||
            (uint32_t)written >= BOOT_TIMELINE_BUF_SIZE - pos)
            goto close;
        pos += (uint32_t)written;
        emitted++;
    }

    for (i = 0; i < count; i++) {
        if (pos + BOOT_TIMELINE_RECORD_MAX >= BOOT_TIMELINE_BUF_SIZE)
            goto close;
        /* Detect reverse-ordered TSC samples explicitly: silent clamping
         * to 0 in safe_tsc_delta_ms() would otherwise emit a plausible
         * `unreliable:false` record from corrupt timing data. Once a
         * reverse step is seen, this and every subsequent record is
         * stamped unreliable. */
        if (steps[i].tsc < base) tsc_unreliable = 1;
        if (i + 1 < count && steps[i + 1].tsc < steps[i].tsc)
            tsc_unreliable = 1;
        uint32_t step_off = safe_tsc_delta_ms(steps[i].tsc, base);
        uint32_t start_ms = sat_add_u32(tsc_anchor_ms, step_off);
        uint32_t dur_ms = 0;
        if (i + 1 < count)
            dur_ms = safe_tsc_delta_ms(steps[i + 1].tsc, steps[i].tsc);
        /* Per-record saturation marks this and the rest unreliable.
         * dur_ms saturation is the same class -- a forward TSC delta
         * large enough to saturate is corrupt timing data, not a real
         * boot duration. */
        if (start_ms == 0xFFFFFFFFu || step_off == 0xFFFFFFFFu ||
            dur_ms == 0xFFFFFFFFu)
            tsc_unreliable = 1;

        /* Truncate step name to 32 chars max for JSON safety */
        char safe_name[33];
        const char *raw = steps[i].step ? steps[i].step : "?";
        uint32_t sn;
        for (sn = 0; sn < 32 && raw[sn] && raw[sn] != '"' && raw[sn] != '\\'; sn++)
            safe_name[sn] = raw[sn];
        safe_name[sn] = '\0';

        /* Per-step target_ms (0 when no budget defined). */
        const struct boot_phase_budget *bp =
            boot_perf_budget_lookup(steps[i].step);
        uint32_t target_ms = bp ? bp->target_ms : 0u;

        int last = (emitted + 1 == total);
        int written = snprintf((char *)buf + pos,
            BOOT_TIMELINE_BUF_SIZE - pos,
            "  {\"stage\":\"%s\",\"phase\":%u,\"post\":\"0x%02x\","
            "\"start_ms\":%u,\"duration_ms\":%u,\"target_ms\":%u,"
            "\"source\":\"tsc\",\"unreliable\":%s}%s\n",
            safe_name,
            (unsigned)steps[i].phase,
            (unsigned)steps[i].postcode,
            (unsigned)start_ms,
            (unsigned)dur_ms,
            (unsigned)target_ms,
            tsc_unreliable ? "true" : "false",
            last ? "" : ",");
        if (written <= 0 ||
            (uint32_t)written >= BOOT_TIMELINE_BUF_SIZE - pos)
            goto close;
        pos += (uint32_t)written;
        emitted++;
    }

close:
    /* If we broke early, the previous record carries a trailing comma
     * because comma selection was based on the planned total. Strip it
     * so the JSON closes cleanly. The last byte before the close is '\n';
     * the comma is at pos-2. */
    if (emitted < total && pos >= 2 && buf[pos - 1] == '\n' && buf[pos - 2] == ',')
        buf[pos - 2] = ' ';

    if (pos + 2 < BOOT_TIMELINE_BUF_SIZE) {
        buf[pos++] = ']';
        buf[pos++] = '\n';
    }

    /* Write to X:\Perf\ (BlackBox) or C:\Impossible\System\Logs\ (fallback).
     * Path moved from X:\Boot\ when FPDT entries joined the timeline so
     * the file lives next to boot-profile.log under the perf dump tree. */
    char tl_path[64];
    {
        const char *tl_dir = klog_using_blackbox ? "X:\\Perf\\" : klog_dir;
        int tp = 0, tj;
        for (tj = 0; tl_dir[tj]; tj++) tl_path[tp++] = tl_dir[tj];
        {
            const char *fn = "boot-timeline.json";
            for (tj = 0; fn[tj]; tj++) tl_path[tp++] = fn[tj];
        }
        tl_path[tp] = '\0';
        /* Single-open create + truncate (same pattern as boot_version.c /
         * boot_trend.c / boot_health.c). Truncation matters: a later boot
         * with fewer records would otherwise leave stale tail bytes from
         * the previous timeline and the JSON would not parse. */
        f = vfs_open(tl_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    }
    if (f) {
        int wr = vfs_write(f, 0, pos, buf);
        vfs_close(f);
        if (wr < 0 || (uint32_t)wr != pos) {
            /* Fail closed (same pattern as boot_health.c): TRUNC already
             * destroyed the previous boot's file, so a partial write is a
             * malformed JSON prefix no consumer can parse. Re-truncate to
             * empty -- "no data this boot" is a recoverable signal. */
            klog(LOG_WARN, "boot",
                 "boot-timeline.json short write (%d of %u); truncating to empty",
                 (int64_t)wr, (uint64_t)pos);
            struct vfs_node *trunc =
                vfs_open(tl_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
            if (trunc)
                vfs_close(trunc);
        }
    }

    {
        uint32_t pg;
        for (pg = 0; pg < BOOT_TIMELINE_BUF_PAGES; pg++)
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
        if (freq >= 1000) {
            uint64_t delta = now - s_kernel_entry_tsc;
            elapsed = boot_prog_tsc_delta_ms(delta, freq);
        }
    }

    /* Record in stage history (drops when buffer full) */
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

    /* Desktop ready: clear POST display (match post_display16 footprint).
     * Guard against undersized modes -- same x0 underflow as post_display16. */
    if (stage == BOOT_STAGE_DESKTOP_READY && kernel_subsystem_ready(SUBSYS_FB)) {
        uint32_t fbw = fb_get_width();
        if (fbw > POST16_TOTAL_W + POST16_MARGIN) {
            uint32_t cx = fbw - POST16_TOTAL_W - POST16_MARGIN;
            fb_fill_rect(cx, POST16_TOP, POST16_TOTAL_W, POST16_TOTAL_H, 0x00000000);
            fb_swap_rect(cx, POST16_TOP, POST16_TOTAL_W, POST16_TOTAL_H);
        }
    }

    /* Forward to boot_progress (phase, step, postcode) */
    boot_progress(m->phase, msg ? msg : m->name, m->postcode);
}

uint32_t boot_get_elapsed_ms(void)
{
    uint64_t freq, delta;
    if (s_kernel_entry_tsc == 0) return 0;
    freq = boot_timing_tsc_freq();
    if (freq < 1000) return 0;
    delta = rdtsc() - s_kernel_entry_tsc;
    return boot_prog_tsc_delta_ms(delta, freq);
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
