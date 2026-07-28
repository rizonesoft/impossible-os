/* ============================================================================
 * boot_timing.c -- Boot performance timeline
 *
 * Reads TSC timestamps captured by the bootloader at each phase boundary,
 * converts them to milliseconds using the calibrated TSC frequency, and
 * logs a detailed boot performance summary.
 *
 * Also parses FPDT (Firmware Performance Data Table) timestamps if
 * available -- these give firmware-phase durations (SEC, PEI, DXE, BDS).
 * ============================================================================ */

#include "kernel/boot_timing.h"
#include "kernel/boot_perf_budget.h"
#include "kernel/boot_init.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "libc/string.h"
#include "kernel/uefi_runtime.h"
#include "kernel/smp.h"   /* smp_cpu_count() for the perf config tag */

static uint64_t s_tsc_freq;

/* Forward declaration -- defined after boot_timing_init below. */
static uint32_t tsc_to_ms(uint64_t ticks);

/* ---- Boot step timeline -------------------------------------------------- */

static struct {
    uint64_t    tsc;
    const char *step;
    uint8_t     phase;
    uint16_t    postcode;
} s_steps[BOOT_TIMING_MAX_STEPS];

static uint32_t s_step_count;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint32_t boot_timing_get_steps(const boot_timing_step_t **out)
{
    /* The internal struct is layout-compatible with boot_timing_step_t
     * (same fields, possibly different order). Copy to a static array
     * to guarantee the public layout. */
    static boot_timing_step_t s_pub[BOOT_TIMING_MAX_STEPS];
    uint32_t i;
    for (i = 0; i < s_step_count; i++) {
        s_pub[i].tsc      = s_steps[i].tsc;
        s_pub[i].phase    = s_steps[i].phase;
        s_pub[i].postcode = s_steps[i].postcode;
        s_pub[i].step     = s_steps[i].step;
    }
    if (out) *out = s_pub;
    return s_step_count;
}

void boot_timing_record_step(uint8_t phase, const char *step, uint16_t postcode)
{
    if (!step || !step[0]) return;
    if (s_step_count >= BOOT_TIMING_MAX_STEPS) return;
    uint32_t idx          = s_step_count++;
    s_steps[idx].tsc      = rdtsc();
    s_steps[idx].phase    = phase;
    s_steps[idx].postcode = postcode;
    s_steps[idx].step     = step;
}

void boot_timing_print_steps(void)
{
    if (s_tsc_freq == 0 || s_step_count == 0) return;
    klog(LOG_INFO, "BOOT", "--- Boot step timing (%u steps, base=step0) ---",
         s_step_count);
    uint64_t base = s_steps[0].tsc;
    for (uint32_t i = 0; i < s_step_count; i++) {
        uint32_t ms = tsc_to_ms(s_steps[i].tsc - base);
        klog(LOG_INFO, "BOOT", "  [PHASE%u] +%ums 0x%04x %s",
             (uint32_t)s_steps[i].phase, ms,
             (uint32_t)s_steps[i].postcode, s_steps[i].step);
    }
}

/* Convert TSC tick delta to milliseconds. Defends against corrupted
 * tick counts: when `ticks * 1000` would wrap uint64, saturate to
 * UINT32_MAX (real boots fit in a few thousand ms; only a garbage
 * timestamp can overflow). The freestanding kernel cannot link libgcc
 * helpers (__udivti3) so __uint128_t division is unavailable -- use a
 * pre-multiply bound check instead. */
static uint32_t tsc_to_ms(uint64_t ticks)
{
    if (s_tsc_freq == 0) return 0;
    /* Saturating bound: if ticks > UINT64_MAX/1000, the multiply wraps. */
    if (ticks > (uint64_t)0xFFFFFFFFFFFFFFFFULL / 1000ULL)
        return 0xFFFFFFFFu;
    uint64_t ms = ticks * 1000ULL / s_tsc_freq;
    if (ms > 0xFFFFFFFFu) return 0xFFFFFFFFu;
    return (uint32_t)ms;
}

uint32_t boot_timing_tsc_delta_ms(uint64_t ticks)
{
    return tsc_to_ms(ticks);
}

/* Convert FPDT nanoseconds to milliseconds */
static uint32_t ns_to_ms(uint64_t ns)
{
    return (uint32_t)(ns / 1000000);
}

/* ---- FPDT normalization helpers ----------------------------------------- */

/* 10 minutes in nanoseconds: any FPDT phase larger than this is implausible
 * (firmware boot never legitimately exceeds ~60s; 10min is a generous cap). */
#define FPDT_NS_SANITY_CAP (10ULL * 60ULL * 1000000000ULL)

int boot_timing_fpdt_unreliable_eval(int available,
                                     uint64_t reset_end,
                                     uint64_t os_loader_load_start,
                                     uint64_t os_loader_start_start,
                                     uint64_t exit_bs_entry,
                                     uint64_t exit_bs_exit)
{
    if (!available) return 1;

    uint64_t r  = reset_end;
    uint64_t ll = os_loader_load_start;
    uint64_t ls = os_loader_start_start;
    uint64_t be = exit_bs_entry;
    uint64_t bx = exit_bs_exit;

    /* All-zero record (VirtualBox EFI publishes FPDT with no data). */
    if (r == 0 && ll == 0 && ls == 0 && be == 0 && bx == 0) return 1;

    /* The unified timeline uses os_loader_start_start as its absolute
     * ms-since-reset anchor (boot_timing_bl_entry_ms_since_reset). A
     * record that left it zero cannot honestly label TSC entries as
     * unreliable=false, so reject the whole record. */
    if (ls == 0) return 1;

    /* Monotonicity: each phase must be >= the previous. Zero fields are
     * tolerated only at the head (firmware that did not record reset_end).
     * Treat any zero AFTER a non-zero predecessor as garbage. */
    uint64_t prev = 0;
    uint64_t seq[5] = { r, ll, ls, be, bx };
    for (int i = 0; i < 5; i++) {
        uint64_t v = seq[i];
        if (v == 0) {
            if (prev != 0) return 1;     /* zero after non-zero */
            continue;
        }
        if (v < prev) return 1;          /* non-monotonic */
        if (v > FPDT_NS_SANITY_CAP) return 1;
        prev = v;
    }

    return 0;
}

int boot_timing_fpdt_unreliable(void)
{
    return boot_timing_fpdt_unreliable_eval(
        g_boot_info.timing.fpdt_available ? 1 : 0,
        g_boot_info.timing.reset_end,
        g_boot_info.timing.os_loader_load_start,
        g_boot_info.timing.os_loader_start_start,
        g_boot_info.timing.exit_bs_entry,
        g_boot_info.timing.exit_bs_exit);
}

uint32_t boot_timing_uefi_total_ms(void)
{
    /* Read tsc_freq from g_boot_info directly, NOT via the static
     * s_tsc_freq cache. VPD vpd_init() runs in Phase 0 (before
     * boot_timing_init() in Phase 1), so the cache is still 0 at
     * VPD render time. The bootloader has already populated
     * g_boot_info.timing.tsc_freq by then. Same overflow guard as
     * tsc_to_ms() so a corrupted handoff cannot wrap. */
    uint64_t freq = g_boot_info.timing.tsc_freq;
    uint64_t bl_entry = g_boot_info.timing.bl_entry;
    uint64_t kjump = g_boot_info.timing.kernel_jump;
    if (freq == 0 || bl_entry == 0 || kjump <= bl_entry) return 0;
    uint64_t ticks = kjump - bl_entry;
    if (ticks > (uint64_t)0xFFFFFFFFFFFFFFFFULL / 1000ULL) return 0xFFFFFFFFu;
    uint64_t ms = ticks * 1000ULL / freq;
    if (ms > 0xFFFFFFFFu) return 0xFFFFFFFFu;
    return (uint32_t)ms;
}

uint32_t boot_timing_bl_entry_ms_since_reset(void)
{
    if (boot_timing_fpdt_unreliable()) return 0;
    /* The bootloader's bl_entry TSC sample is taken at efi_main entry,
     * which is the closest moment to FPDT's os_loader_start_start. Use
     * that field as the absolute anchor. */
    return ns_to_ms(g_boot_info.timing.os_loader_start_start);
}

uint32_t boot_timing_get_fpdt_entries(boot_timing_fpdt_entry_t *out, uint32_t cap)
{
    if (!out || cap == 0) return 0;

    int unreliable = boot_timing_fpdt_unreliable();

    /* Fixed phase order: matches the FPDT FBPT record fields. Always emit
     * the full 5-phase set so JSON consumers see consistent shape. */
    const char *names[5] = {
        "fpdt:reset_end",
        "fpdt:os_loader_load",
        "fpdt:os_loader_start",
        "fpdt:exit_bs_entry",
        "fpdt:exit_bs_exit",
    };
    uint64_t vals[5] = {
        g_boot_info.timing.reset_end,
        g_boot_info.timing.os_loader_load_start,
        g_boot_info.timing.os_loader_start_start,
        g_boot_info.timing.exit_bs_entry,
        g_boot_info.timing.exit_bs_exit,
    };

    uint32_t n = 0;
    for (int i = 0; i < 5 && n < cap; i++) {
        out[n].stage       = names[i];
        out[n].start_ms    = unreliable ? 0 : ns_to_ms(vals[i]);
        out[n].unreliable  = (uint8_t)(unreliable ? 1 : 0);
        out[n].duration_ms = 0;
        n++;
    }
    /* Compute durations as deltas to the next entry. Only meaningful when
     * reliable; unreliable entries already have all-zero start_ms. */
    for (uint32_t i = 0; i + 1 < n; i++) {
        if (out[i + 1].start_ms >= out[i].start_ms)
            out[i].duration_ms = out[i + 1].start_ms - out[i].start_ms;
    }
    return n;
}

void boot_timing_init(void)
{
    s_tsc_freq = g_boot_info.timing.tsc_freq;

    if (s_tsc_freq == 0) {
        klog(LOG_WARN, "BOOT", "TSC frequency unknown, boot timing unavailable");
        return;
    }

    klog(LOG_INFO, "BOOT", "TSC frequency: %u MHz",
         (uint32_t)(s_tsc_freq / 1000000));

    /* --- Bootloader phase timings (TSC-based) --- */
    uint64_t bl_entry = g_boot_info.timing.bl_entry;

    if (bl_entry == 0) {
        klog(LOG_WARN, "BOOT", "No bootloader timestamps available");
        return;
    }

    uint32_t gop_ms = tsc_to_ms(
        g_boot_info.timing.gop_end - g_boot_info.timing.gop_start);
    uint32_t conf_ms = tsc_to_ms(
        g_boot_info.timing.conf_end - g_boot_info.timing.conf_start);
    uint32_t kload_ms = tsc_to_ms(
        g_boot_info.timing.kernel_load_end -
        g_boot_info.timing.kernel_load_start);
    uint32_t exit_bs_ms = tsc_to_ms(
        g_boot_info.timing.kernel_jump - g_boot_info.timing.exit_bs);
    uint32_t total_bl_ms = boot_timing_uefi_total_ms();

    klog(LOG_INFO, "BOOT", "Bootloader: GOP %ums, Config %ums, "
         "Kernel Load %ums, ExitBS %ums, Total %ums",
         gop_ms, conf_ms, kload_ms, exit_bs_ms, total_bl_ms);

    /* --- FPDT firmware phase timings (nanosecond-based) ---
     * Only print if the FPDT record passes sanity checks; VirtualBox EFI
     * publishes a zero-filled FPDT and would otherwise log "0ms" lines. */
    if (g_boot_info.timing.fpdt_available && !boot_timing_fpdt_unreliable()) {
        uint32_t fw_init_ms = ns_to_ms(g_boot_info.timing.reset_end);
        uint32_t loader_load_ms = ns_to_ms(
            g_boot_info.timing.os_loader_load_start);
        uint32_t loader_start_ms = ns_to_ms(
            g_boot_info.timing.os_loader_start_start);

        klog(LOG_INFO, "BOOT", "Firmware: Reset %ums, "
             "Loader Load @%ums, Loader Start @%ums",
             fw_init_ms, loader_load_ms, loader_start_ms);

        /* Total firmware + bootloader time */
        uint64_t total_ns =
            g_boot_info.timing.os_loader_start_start;
        uint32_t fw_total_ms = ns_to_ms(total_ns);

        klog(LOG_INFO, "BOOT", "Total: Firmware %ums + Bootloader %ums = %ums",
             fw_total_ms, total_bl_ms, fw_total_ms + total_bl_ms);
    } else if (g_boot_info.timing.fpdt_available) {
        klog(LOG_WARN, "BOOT",
             "FPDT present but unreliable (zero/garbage record)");
    } else {
        klog(LOG_INFO, "BOOT", "FPDT not available (firmware timing unknown)");
    }
}

uint64_t boot_timing_tsc_freq(void)
{
    return s_tsc_freq;
}

/* ---- Boot profile log writer --------------------------------------------- */

static uint32_t u32_to_dec(char *buf, uint32_t val)
{
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[12];
    uint32_t n = 0;
    while (val) { tmp[n++] = (char)('0' + val % 10); val /= 10; }
    for (uint32_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static uint32_t u16_to_hex4(char *buf, uint16_t val)
{
    static const char hex[] = "0123456789abcdef";
    buf[0] = hex[(val >> 12) & 0xf];
    buf[1] = hex[(val >>  8) & 0xf];
    buf[2] = hex[(val >>  4) & 0xf];
    buf[3] = hex[ val        & 0xf];
    return 4;
}


void boot_timing_write_report(void)
{
    if (s_tsc_freq == 0 || s_step_count == 0) return;

    /* Write to X:\Perf\ (BlackBox) or C:\Impossible\System\Logs\ (fallback) */
    const char *bp_dir = klog_using_blackbox ? "X:\\Perf\\" : klog_dir;
    char bp_path[64];
    {
        int bp = 0, bj;
        for (bj = 0; bp_dir[bj]; bj++) bp_path[bp++] = bp_dir[bj];
        const char *fn = "boot-profile.log";
        for (bj = 0; fn[bj]; bj++) bp_path[bp++] = fn[bj];
        bp_path[bp] = '\0';
    }
    struct vfs_node *file = vfs_open(bp_path,
                                     VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!file) {
        klog(LOG_WARN, "BOOT", "boot-profile.log: cannot open for write");
        return;
    }

    char line[128];
    uint32_t offset = 0;
    uint64_t base = s_steps[0].tsc;

    /* Header */
    static const char hdr[] = "# Impossible OS Boot Profile\n"
                               "# +NNNms [PHASEx] 0xNNNN step\n";
    vfs_write(file, offset, sizeof(hdr) - 1, (const uint8_t *)hdr);
    offset += sizeof(hdr) - 1;

    for (uint32_t i = 0; i < s_step_count; i++) {
        uint32_t ms  = tsc_to_ms(s_steps[i].tsc - base);
        uint32_t pos = 0;

        line[pos++] = '+';
        pos += u32_to_dec(line + pos, ms);
        line[pos++] = 'm'; line[pos++] = 's';
        line[pos++] = ' ';
        line[pos++] = '['; line[pos++] = 'P'; line[pos++] = 'H';
        line[pos++] = 'A'; line[pos++] = 'S'; line[pos++] = 'E';
        line[pos++] = (char)('0' + (s_steps[i].phase & 0x0f));
        line[pos++] = ']'; line[pos++] = ' ';
        line[pos++] = '0'; line[pos++] = 'x';
        pos += u16_to_hex4(line + pos, s_steps[i].postcode);
        line[pos++] = ' ';
        /* Bound step name to remaining buffer space (leave room for newline) */
        {
            const char *sn = s_steps[i].step;
            uint32_t max = (pos + 2 < sizeof(line)) ? (uint32_t)(sizeof(line) - pos - 2) : 0;
            uint32_t j;
            for (j = 0; j < max && sn[j]; j++)
                line[pos + j] = sn[j];
            pos += j;
        }
        line[pos++] = '\n';

        vfs_write(file, offset, pos, (const uint8_t *)line);
        offset += pos;
    }

    vfs_close(file);
    klog(LOG_INFO, "BOOT", "Boot profile: %u steps written",
         (uint64_t)s_step_count);
}

/* ---- POST code history log ----------------------------------------------- */

void boot_postcode_write_log(void)
{
    if (s_step_count == 0) return;

    const char *diag_dir = klog_using_blackbox ? "X:\\Diag\\" : klog_dir;
    char path[64];
    int pi = 0, j;
    for (j = 0; diag_dir[j]; j++) path[pi++] = diag_dir[j];
    { const char *fn = "postcode.log";
      for (j = 0; fn[j]; j++) path[pi++] = fn[j]; }
    path[pi] = '\0';

    struct vfs_node *f = vfs_open(path,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        klog(LOG_WARN, "BOOT", "postcode.log: cannot open %s for write", path);
        return;
    }

    char line[80];
    uint32_t offset = 0;
    uint64_t base = s_steps[0].tsc;
    int write_ok = 1;

    for (uint32_t i = 0; i < s_step_count; i++) {
        uint64_t ms = (s_tsc_freq > 0) ? tsc_to_ms(s_steps[i].tsc - base) : 0;
        int pos = snprintf(line, sizeof(line), "[%4u.%03u] POST 0x%04X  P%u  %s\n",
                           (uint32_t)(ms / 1000), (uint32_t)(ms % 1000),
                           s_steps[i].postcode, s_steps[i].phase,
                           s_steps[i].step ? s_steps[i].step : "?");
        if (pos > 0) {
            /* snprintf returns the would-be length; on truncation only
             * sizeof(line)-1 bytes are actually resident in `line`. Clamp so
             * vfs_write never reads past the stack buffer. */
            uint32_t wlen = (pos < (int)sizeof(line)) ? (uint32_t)pos
                                                      : (uint32_t)(sizeof(line) - 1);
            int wr = vfs_write(f, offset, wlen, (const uint8_t *)line);
            if (wr < 0 || wr != (int)wlen) {
                klog(LOG_WARN, "BOOT",
                     "postcode.log entry %u write failed (%d of %u)",
                     (uint64_t)i, (int64_t)wr, (uint64_t)wlen);
                write_ok = 0;
                break;
            }
            offset += wlen;
        }
    }

    /* Durable boundary before declaring success (matches the X:\Crash and
     * X:\Perf writers). */
    if (write_ok && vfs_flush(f) != 0) {
        klog(LOG_WARN, "BOOT", "postcode.log flush failed -- artifact may not be durable");
        write_ok = 0;
    }
    vfs_close(f);
    if (write_ok)
        klog(LOG_INFO, "BOOT", "POST code history: %u entries written to %s",
             (uint64_t)s_step_count, path);
}

/* ---- Boot performance regression detection ------------------------------- */

/* GUID: same namespace as ImpossiblePOST -- all Impossible OS boot vars share it */
static const struct boot_uefi_guid s_perf_guid = {
    0x494D504F, 0x5354, 0x4F53,
    { 0x50, 0x4F, 0x53, 0x54, 0x47, 0x55, 0x49, 0x44 }
};

/* UCS-2 variable name: "ImpossibleBootPerf" */
static const uint16_t s_perf_name[] = {
    'I','m','p','o','s','s','i','b','l','e',
    'B','o','o','t','P','e','r','f', 0
};

#define PERF_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                    EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                    EFI_VARIABLE_RUNTIME_ACCESS)

/* Previous boot's perf data, read from NVRAM at early boot */
static boot_perf_record_t s_prev_records[BOOT_PERF_MAX_RECORDS];
static uint32_t            s_prev_count;

static void str_copy_trunc(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1 < max && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int boot_perf_enabled(void)
{
    return !g_boot_info.config.test && !g_boot_info.config.debug;
}

void boot_perf_read_prev(void)
{
    /* Buffer: header + max records */
    static uint8_t buf[sizeof(boot_perf_header_t) +
                       BOOT_PERF_MAX_RECORDS * sizeof(boot_perf_record_t)];
    uint64_t sz = sizeof(buf);
    uint32_t attrs = 0;

    POST16(POST16_BOOTPERF);

    uint64_t status = uefi_get_variable(&s_perf_guid, s_perf_name,
                                        &attrs, &sz, buf);
    if (status != 0) {
        klog(LOG_INFO, "PERF", "No previous boot perf data (first boot or cleared)");
        POST16(POST16_BOOTPERF_READ);
        return;
    }

    if (sz < sizeof(boot_perf_header_t)) {
        klog(LOG_WARN, "PERF", "Boot perf NVRAM too small (%u bytes)", (uint32_t)sz);
        POST16(POST16_BOOTPERF_READ);
        return;
    }

    boot_perf_header_t *hdr = (boot_perf_header_t *)buf;
    if (hdr->magic != BOOT_PERF_MAGIC) {
        klog(LOG_WARN, "PERF", "Boot perf NVRAM bad magic (0x%08x)", (uint64_t)hdr->magic);
        POST16(POST16_BOOTPERF_READ);
        return;
    }

    /* Decline the comparison outright when the baseline came from a different
     * machine configuration. Differencing timings across accelerators is what
     * produced 30 false "regressed" warnings on 2026-07-28: a baseline saved
     * under one accelerator, compared against a boot under another, made every
     * step look ~3x slower. Reporting nothing is correct here -- the data is
     * not comparable, and a warning would imply it was. */
    {
        uint32_t cur_cpus = smp_cpu_count();
        uint32_t cur_khz  = (uint32_t)(s_tsc_freq / 1000ULL);
        /* TSC frequency is compared with a tolerance: calibration jitters by a
         * few kHz between boots on the same host, which is not a config change.
         * 1% is far tighter than the gap between any two real configurations
         * and far looser than calibration noise. */
        uint32_t tol = hdr->tsc_khz / 100u;
        uint32_t lo  = hdr->tsc_khz > tol ? hdr->tsc_khz - tol : 0;
        uint32_t hi  = hdr->tsc_khz + tol;
        if (hdr->cpu_count != cur_cpus || cur_khz < lo || cur_khz > hi) {
            klog(LOG_INFO, "PERF",
                 "Previous boot ran a different config (cpus %u->%u, tsc %u->%u kHz)"
                 " -- comparison skipped",
                 (uint64_t)hdr->cpu_count, (uint64_t)cur_cpus,
                 (uint64_t)hdr->tsc_khz, (uint64_t)cur_khz);
            POST16(POST16_BOOTPERF_READ);
            return;
        }
    }

    uint32_t count = hdr->count;
    if (count > BOOT_PERF_MAX_RECORDS)
        count = BOOT_PERF_MAX_RECORDS;

    uint64_t expected = sizeof(boot_perf_header_t) + count * sizeof(boot_perf_record_t);
    if (sz < expected) {
        klog(LOG_WARN, "PERF", "Boot perf NVRAM truncated (%u < %u)",
             (uint32_t)sz, (uint32_t)expected);
        POST16(POST16_BOOTPERF_READ);
        return;
    }

    boot_perf_record_t *recs = (boot_perf_record_t *)(buf + sizeof(boot_perf_header_t));
    for (uint32_t i = 0; i < count; i++) {
        s_prev_records[i] = recs[i];
        /* NVRAM is untrusted: a corrupt/external record can fill all 16 name
         * bytes without a terminator (str_eq in the compare would then overread
         * past name[] into elapsed_ms/phase) and carry an implausible
         * elapsed_ms that overflows the regression arithmetic. Force a NUL
         * terminator and clamp elapsed_ms to a sane ceiling (10 minutes). */
        s_prev_records[i].name[BOOT_PERF_NAME_LEN - 1] = '\0';
        if (s_prev_records[i].elapsed_ms > BOOT_PERF_MS_SANITY_CAP)
            s_prev_records[i].elapsed_ms = BOOT_PERF_MS_SANITY_CAP;
    }
    s_prev_count = count;

    klog(LOG_INFO, "PERF", "Previous boot: %u step(s) loaded from NVRAM", count);
    POST16(POST16_BOOTPERF_READ);
}

void boot_perf_compare(void)
{
    if (s_tsc_freq == 0 || s_step_count == 0 || s_prev_count == 0)
        return;
    if (!boot_perf_enabled()) {
        klog(LOG_INFO, "PERF", "Boot perf comparison skipped in test/debug mode");
        return;
    }

    uint64_t base = s_steps[0].tsc;

    klog(LOG_INFO, "PERF", "--- Boot perf comparison (prev vs current) ---");

    /* COMPARE PER-STEP DURATIONS, NOT CUMULATIVE ARRIVAL TIMES.
     *
     * boot_perf_record_t.elapsed_ms is time since boot start (its own comment
     * reads "duration from boot start"), so differencing two of them compares
     * WHEN a step ran, not how long it took. One slow step early shifts every
     * later arrival time, and every subsequent step then reports a regression.
     * Observed 2026-07-28 on a 2-CPU WHPX boot: 30 consecutive "init regressed"
     * warnings, every one between 287% and 339%, with deltas growing
     * monotonically down the list (+16ms, +89ms, +92ms, +232ms ... +4285ms).
     * That is the signature of cumulative drift, not 30 independent
     * regressions, and all 30 lines were false.
     *
     * Records are stored in step order on both sides, so a per-step duration is
     * the delta to the next entry -- the same derivation the "Boot step
     * durations" report below already uses. The final entry of each array has
     * no successor and therefore no derivable duration; it is skipped rather
     * than guessed at. */
    for (uint32_t i = 0; i + 1 < s_step_count; i++) {
        uint32_t cur_start = tsc_to_ms(s_steps[i].tsc - base);
        uint32_t cur_next  = tsc_to_ms(s_steps[i + 1].tsc - base);
        if (cur_next < cur_start)
            continue;                   /* non-monotonic clock: unusable */
        uint64_t cur_dur = (uint64_t)(cur_next - cur_start);
        if (cur_dur > BOOT_PERF_MS_SANITY_CAP)
            cur_dur = BOOT_PERF_MS_SANITY_CAP;  /* ceiling save/read enforce */
        const char *name = s_steps[i].step;

        /* Match by name, and the previous boot must have a successor entry too
         * or its duration is equally underivable. */
        for (uint32_t j = 0; j + 1 < s_prev_count; j++) {
            if (!str_eq(s_prev_records[j].name, name))
                continue;

            uint32_t p_start = s_prev_records[j].elapsed_ms;
            uint32_t p_next  = s_prev_records[j + 1].elapsed_ms;
            if (p_next < p_start)
                break;                  /* corrupt NVRAM ordering: skip */
            uint64_t prev_dur = (uint64_t)(p_next - p_start);

            /* 64-bit math throughout: both operands are clamped, but compute in
             * uint64 so prev*2 and cur*100 cannot wrap and the delta cannot hit
             * signed-overflow UB. Only a real slowdown is flagged, never a
             * speed-up. */
            int64_t delta = (int64_t)cur_dur - (int64_t)prev_dur;

            /* Regression threshold: >200% of previous OR >500ms absolute */
            if (prev_dur > 0 && cur_dur > prev_dur * 2) {
                klog(LOG_WARN, "PERF",
                     "[PERF] WARNING: %s step regressed: %ums -> %ums (+%ums, %u%%)",
                     name, (uint32_t)prev_dur, (uint32_t)cur_dur,
                     (uint32_t)delta,
                     (uint32_t)(cur_dur * 100 / prev_dur));
            } else if (delta > BOOT_PERF_ABS_REGRESS_MS) {
                klog(LOG_WARN, "PERF",
                     "[PERF] WARNING: %s step regressed: %ums -> %ums (+%ums)",
                     name, (uint32_t)prev_dur, (uint32_t)cur_dur, (uint32_t)delta);
            }
            break;
        }
    }

    POST16(POST16_BOOTPERF_CMP);
}

void boot_perf_save(void)
{
    if (s_tsc_freq == 0 || s_step_count == 0)
        return;
    if (!boot_perf_enabled()) {
        klog(LOG_INFO, "PERF", "Boot perf save skipped in test/debug mode");
        return;
    }

    /* Build the NVRAM payload: header + records */
    static uint8_t buf[sizeof(boot_perf_header_t) +
                       BOOT_PERF_MAX_RECORDS * sizeof(boot_perf_record_t)];

    uint32_t count = s_step_count;
    if (count > BOOT_PERF_MAX_RECORDS)
        count = BOOT_PERF_MAX_RECORDS;

    boot_perf_header_t *hdr = (boot_perf_header_t *)buf;
    hdr->magic = BOOT_PERF_MAGIC;
    hdr->count = count;
    /* Stamp the machine configuration alongside the timings so the next boot
     * can tell whether they are comparable at all (see boot_perf_read_prev). */
    hdr->cpu_count = smp_cpu_count();
    hdr->tsc_khz   = (uint32_t)(s_tsc_freq / 1000ULL);

    boot_perf_record_t *recs = (boot_perf_record_t *)(buf + sizeof(boot_perf_header_t));
    uint64_t base = s_steps[0].tsc;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ms = tsc_to_ms(s_steps[i].tsc - base);
        if (ms > BOOT_PERF_MS_SANITY_CAP)
            ms = BOOT_PERF_MS_SANITY_CAP;   /* same ceiling the reader enforces */
        str_copy_trunc(recs[i].name, s_steps[i].step, BOOT_PERF_NAME_LEN);
        recs[i].elapsed_ms = ms;
        recs[i].phase      = s_steps[i].phase;
        recs[i]._pad[0]    = 0;
        recs[i]._pad[1]    = 0;
        recs[i]._pad[2]    = 0;
    }

    uint64_t total_sz = sizeof(boot_perf_header_t) + count * sizeof(boot_perf_record_t);

    uint64_t status = uefi_set_variable(&s_perf_guid, s_perf_name,
                                        PERF_ATTRS, total_sz, buf);
    if (status == 0) {
        klog(LOG_INFO, "PERF", "Boot perf saved to NVRAM (%u steps, %u bytes)",
             count, (uint32_t)total_sz);
    } else {
        klog(LOG_WARN, "PERF", "Boot perf NVRAM write failed (status 0x%x)",
             (uint32_t)status);
    }

    POST16(POST16_BOOTPERF_WRITE);
}

/* Pad a string to exactly `width` chars in `buf`, space-filled on right */
static void pad_right(char *buf, const char *src, uint32_t width)
{
    uint32_t i = 0;
    while (i < width && src && src[i]) { buf[i] = src[i]; i++; }
    while (i < width) buf[i++] = ' ';
    buf[width] = '\0';
}

void boot_perf_dump(void)
{
    if (s_tsc_freq == 0 || s_step_count == 0) return;

    uint64_t base = s_steps[0].tsc;

    /* Simple insertion sort by elapsed time (descending) for the dump */
    uint32_t indices[BOOT_TIMING_MAX_STEPS];
    uint32_t ms_vals[BOOT_TIMING_MAX_STEPS];
    uint32_t n = s_step_count;

    for (uint32_t i = 0; i < n; i++) {
        indices[i] = i;
        ms_vals[i] = tsc_to_ms(s_steps[i].tsc - base);
    }

    /* Compute per-step durations (delta between consecutive steps) */
    uint32_t durations[BOOT_TIMING_MAX_STEPS];
    for (uint32_t i = 0; i + 1 < n; i++)
        durations[i] = ms_vals[i + 1] - ms_vals[i];
    durations[n > 0 ? n - 1 : 0] = 0;  /* last step has no duration */

    /* Sort indices by duration descending */
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key_idx = indices[i];
        uint32_t key_dur = durations[key_idx];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && durations[indices[j]] < key_dur) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key_idx;
    }

    klog(LOG_INFO, "PERF", "--- Boot step durations (sorted by time) ---");

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = indices[i];
        if (durations[idx] == 0 && idx == n - 1) continue;  /* skip last */
        char padded[20];
        pad_right(padded, s_steps[idx].step, 18);
        klog(LOG_INFO, "PERF", "  %s %5ums  %5ums  P%u",
             padded, ms_vals[idx], durations[idx],
             (uint32_t)s_steps[idx].phase);
    }

    boot_perf_budget_check();
    boot_perf_total_check();
}
