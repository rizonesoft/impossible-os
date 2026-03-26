/* ============================================================================
 * boot_timing.c — Boot performance timeline
 *
 * Reads TSC timestamps captured by the bootloader at each phase boundary,
 * converts them to milliseconds using the calibrated TSC frequency, and
 * logs a detailed boot performance summary.
 *
 * Also parses FPDT (Firmware Performance Data Table) timestamps if
 * available — these give firmware-phase durations (SEC, PEI, DXE, BDS).
 * ============================================================================ */

#include "kernel/boot_timing.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

static uint64_t s_tsc_freq;

/* Forward declaration — defined after boot_timing_init below. */
static uint32_t tsc_to_ms(uint64_t ticks);

/* ---- Boot step timeline -------------------------------------------------- */

static struct {
    uint64_t    tsc;
    const char *step;
    uint8_t     phase;
    uint8_t     postcode;
} s_steps[BOOT_TIMING_MAX_STEPS];

static uint32_t s_step_count;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void boot_timing_record_step(uint8_t phase, const char *step, uint8_t postcode)
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
        klog(LOG_INFO, "BOOT", "  [PHASE%u] +%ums 0x%02x %s",
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
