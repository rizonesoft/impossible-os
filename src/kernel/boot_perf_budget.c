/* SPDX-License-Identifier: MIT */
/* Per-phase boot perf budgets. See include/kernel/boot_perf_budget.h. */

#include "kernel/boot_perf_budget.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

/* Test-mode demotion: when boot.conf set test=1, every TEST_CAT_*
 * suite runs inside the boot phase between `record_step` calls and
 * inflates per-step deltas by 100ms-15s. Those deltas are not real
 * boot regressions; demote LOG_ERROR/LOG_WARN to LOG_INFO so the
 * advisory still surfaces but does not spam the boot log with
 * [FAIL]/[WARN] lines that the operator must mentally filter. The
 * boot-trend writer tags test-run entries separately so the
 * regression detector does not compare test runs against
 * production runs. */
static int boot_perf_budget_log_level(int prod_level)
{
    return g_boot_info.config.test ? LOG_INFO : prod_level;
}

static const char *boot_perf_budget_suffix(void)
{
    return g_boot_info.config.test
        ? " (test=1 active; budget advisory)"
        : "";
}

/* Total-boot-time budget. DESKTOP_READY is the last recorded step so
 * its per-step delta is always 0; the wall-clock total is the right
 * measurement instead. 4 seconds matches "fast clean boot" on QEMU
 * OVMF + leaves headroom for VirtualBox / bare metal. */
#define BOOT_PERF_TOTAL_TARGET_MS 4000u

/* 18 named per-step budgets. Numbers are first-pass operator-visible
 * targets; tighten as observability surfaces real outliers. DESKTOP_READY
 * intentionally absent (last step has no delta). */
static const struct boot_phase_budget s_budgets[] = {
    { "PMM",              8,    "PMM bitmap zero + free-list build over up to 8 GiB" },
    { "VMM",              12,   "PML4/PDPT/PD/PT walk + identity map of full RAM" },
    { "HEAP",             2,    "freelist init over 64 KiB initial pool" },
    { "ACPI",             100,  "RSDP search + XSDT walk + DSDT/SSDT enumeration" },
    { "FB",               200,  "GOP query + WC remap + back-buffer alloc" },
    { "SMBIOS",           100,  "entry-point parse + N-record walk for system info" },
    { "TIMER",            20,   "HPET MMIO probe + PIT IRQ wire + LAPIC timer calibration" },
    { "KEYBOARD",         50,   "PS/2 8042 reset + self-test + scancode set 2 enable" },
    { "MOUSE",            100,  "PS/2 mouse reset + ID + sample-rate + enable" },
    { "PCI_NET_DEFERRED", 200,  "PCI enumeration + NIC class probe (deferred path)" },
    { "SMP",              200,  "AP trampoline + INIT-SIPI-SIPI + per-CPU MSR program" },
    { "STORAGE_DRV",      30,   "AHCI / VirtIO controller probe + port init" },
    { "VFS",              500,  "VFS root + IXFS mount + FAT32 BPB validate" },
    { "OB",               100,  "Object Manager root + Type table + 11 named types" },
    { "REGISTRY",         200,  "Hive load + key tree build + default value populate" },
    { "SCHED",            50,   "scheduler ready + idle thread per CPU" },
    { "IPC",              120,  "ALPC port table + pipe table + shmem table init" },
    { "EXEC",             100,  "loader hook + EIF/PE/ELF format dispatch wire-up" },
};

#define S_BUDGET_COUNT (sizeof(s_budgets) / sizeof(s_budgets[0]))

_Static_assert(S_BUDGET_COUNT == 18,
               "boot_perf_budget table must hold all 18 named budgets");

uint32_t boot_perf_budget_count(void) { return (uint32_t)S_BUDGET_COUNT; }

const struct boot_phase_budget *boot_perf_budget_get(uint32_t i)
{
    if (i >= S_BUDGET_COUNT) return (const struct boot_phase_budget *)0;
    return &s_budgets[i];
}

/* Inline string compare; freestanding kernel has no <string.h>. */
static int sb_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

const struct boot_phase_budget *boot_perf_budget_lookup(const char *step)
{
    if (!step) return (const struct boot_phase_budget *)0;
    for (uint32_t i = 0; i < S_BUDGET_COUNT; i++) {
        if (sb_streq(s_budgets[i].step, step))
            return &s_budgets[i];
    }
    return (const struct boot_phase_budget *)0;
}

enum boot_perf_budget_class
boot_perf_budget_classify(uint32_t observed_ms, uint32_t target_ms)
{
    if (target_ms == 0) return BUDGET_OK;
    /* Compute thresholds with u64 to avoid u32 overflow on observed_ms
     * up to a 4 GiB count, then compare against the u32 observed value. */
    uint64_t soft = (uint64_t)target_ms * 3u / 2u; /* 1.5x */
    uint64_t hard = (uint64_t)target_ms * 4u;      /* 4.0x */
    if ((uint64_t)observed_ms <= soft) return BUDGET_OK;
    if ((uint64_t)observed_ms <= hard) return BUDGET_SOFT;
    return BUDGET_HARD;
}

void boot_perf_budget_check(void)
{
    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t n = boot_timing_get_steps(&steps);
    if (!steps || n < 2) return;

    if (boot_timing_tsc_freq() < 1000) return;

    /* Skip the last step -- delta-to-next is undefined. */
    for (uint32_t i = 0; i + 1 < n; i++) {
        const struct boot_phase_budget *b = boot_perf_budget_lookup(steps[i].step);
        if (!b) continue;

        uint64_t delta_tsc = (steps[i + 1].tsc > steps[i].tsc)
                           ? (steps[i + 1].tsc - steps[i].tsc) : 0;
        /* Saturating helper -- corrupt or extreme deltas clamp to
         * UINT32_MAX rather than wrapping back to a small value that
         * would silently classify as OK. */
        uint32_t observed_ms = boot_timing_tsc_delta_ms(delta_tsc);

        enum boot_perf_budget_class cls =
            boot_perf_budget_classify(observed_ms, b->target_ms);
        if (cls == BUDGET_OK) continue;

        int prod_level = (cls == BUDGET_HARD) ? LOG_ERROR : LOG_WARN;
        /* Name BOTH endpoints. What is measured is the INTERVAL from this
         * milestone to the next one, which is not the same thing as the cost of
         * the step the milestone is named after -- everything that runs between
         * them is inside it, recorded or not.
         *
         * The old "%s took %ums" wording hid that and misdirected the reader.
         * Two live examples from a 2026-07-27 boot: "VFS took 3212ms" was the
         * VFS -> PARTITION interval, whose real content is the partition scan,
         * the IXFS mount and a FAT32 dirty-volume fsck -- vfs_init itself just
         * registers 26 drive letters. "EXEC took 2272ms" was EXEC ->
         * DESKTOP_READY, whose real content is five TTF faces, a 1410-glyph
         * cache bake, four icon fonts and a 1935x1080 JPEG decode and rescale.
         * Neither number described the subsystem it named, and acting on either
         * would have meant optimizing code that was never the cost.
         *
         * The fix is to say what was measured. Narrowing an interval is then a
         * matter of recording a milestone inside it, which the reader can now
         * see is the actual remedy. */
        klog(boot_perf_budget_log_level(prod_level), "BOOT-BUDGET",
             "%s -> %s took %ums (target %ums): %s%s",
             b->step,
             steps[i + 1].step ? steps[i + 1].step : "?",
             observed_ms, b->target_ms, b->reason,
             boot_perf_budget_suffix());
    }
}

void boot_perf_total_check(void)
{
    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t n = boot_timing_get_steps(&steps);
    if (!steps || n < 2) return;

    if (boot_timing_tsc_freq() < 1000) return;

    uint64_t delta_tsc = (steps[n - 1].tsc > steps[0].tsc)
                       ? (steps[n - 1].tsc - steps[0].tsc) : 0;
    uint32_t total_ms = boot_timing_tsc_delta_ms(delta_tsc);

    enum boot_perf_budget_class cls =
        boot_perf_budget_classify(total_ms, BOOT_PERF_TOTAL_TARGET_MS);
    if (cls == BUDGET_OK) return;

    int prod_level = (cls == BUDGET_HARD) ? LOG_ERROR : LOG_WARN;
    klog(boot_perf_budget_log_level(prod_level), "BOOT-BUDGET",
         "TOTAL boot took %ums (target %ums): "
         "first-step to DESKTOP_READY wall clock%s",
         total_ms, (uint32_t)BOOT_PERF_TOTAL_TARGET_MS,
         boot_perf_budget_suffix());
}
