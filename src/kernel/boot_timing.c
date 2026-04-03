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
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"

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

/* Convert TSC tick delta to milliseconds */
static uint32_t tsc_to_ms(uint64_t ticks)
{
    if (s_tsc_freq == 0) return 0;
    return (uint32_t)(ticks * 1000 / s_tsc_freq);
}

/* Convert FPDT nanoseconds to milliseconds */
static uint32_t ns_to_ms(uint64_t ns)
{
    return (uint32_t)(ns / 1000000);
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
    uint32_t total_bl_ms = tsc_to_ms(
        g_boot_info.timing.kernel_jump - bl_entry);

    klog(LOG_INFO, "BOOT", "Bootloader: GOP %ums, Config %ums, "
         "Kernel Load %ums, ExitBS %ums, Total %ums",
         gop_ms, conf_ms, kload_ms, exit_bs_ms, total_bl_ms);

    /* --- FPDT firmware phase timings (nanosecond-based) --- */
    if (g_boot_info.timing.fpdt_available) {
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

static uint32_t str_copy(char *dst, const char *src)
{
    uint32_t n = 0;
    while (src[n]) { dst[n] = src[n]; n++; }
    return n;
}

void boot_timing_write_report(void)
{
    if (s_tsc_freq == 0 || s_step_count == 0) return;

    struct vfs_node *file = vfs_open(
        KLOG_DIR "boot-profile.log",
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
        pos += str_copy(line + pos, s_steps[i].step);
        line[pos++] = '\n';

        vfs_write(file, offset, pos, (const uint8_t *)line);
        offset += pos;
    }

    vfs_close(file);
    klog(LOG_INFO, "BOOT", "Boot profile: %u steps written",
         (uint64_t)s_step_count);
}
