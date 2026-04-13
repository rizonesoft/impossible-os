/* ============================================================================
 * pmc.c -- Performance Monitoring Counters (Intel PMU + AMD PMC)
 *
 * XREF: 02-kernel-core/TODO-19-x86-64-architecture.md S10
 * ============================================================================ */

#include "kernel/pmc.h"
#include "kernel/cpuid.h"
#include "kernel/msr.h"
#include "kernel/klog.h"

pmc_info_t g_pmc_info;

/* ---- Intel PMU ---- */

static int pmc_intel_start(uint32_t slot, uint64_t event)
{
    uint64_t evtsel;

    if (slot >= g_pmc_info.num_counters)
        return -1;

    /* Clear counter first */
    msr_write(MSR_IA32_PMC0 + slot, 0);

    /* Configure event: event_select[7:0] + unit_mask[15:8] + OS + USR + EN */
    evtsel = (event & 0xFFFF) | PERFEVTSEL_OS | PERFEVTSEL_USR | PERFEVTSEL_EN;
    msr_write(MSR_IA32_PERFEVTSEL0 + slot, evtsel);

    /* Enable in global control (bit N = counter N) */
    {
        uint64_t global = msr_read(MSR_IA32_PERF_GLOBAL_CTRL);
        global |= (1ULL << slot);
        msr_write(MSR_IA32_PERF_GLOBAL_CTRL, global);
    }

    return 0;
}

static uint64_t pmc_intel_read(uint32_t slot)
{
    if (slot >= g_pmc_info.num_counters)
        return 0;
    return msr_read(MSR_IA32_PMC0 + slot);
}

static int pmc_intel_stop(uint32_t slot)
{
    if (slot >= g_pmc_info.num_counters)
        return -1;

    /* Clear enable bit in event select */
    msr_write(MSR_IA32_PERFEVTSEL0 + slot, 0);

    /* Clear in global control */
    {
        uint64_t global = msr_read(MSR_IA32_PERF_GLOBAL_CTRL);
        global &= ~(1ULL << slot);
        msr_write(MSR_IA32_PERF_GLOBAL_CTRL, global);
    }

    return 0;
}

/* ---- AMD PMC ---- */

#define AMD_PERF_CTL_ENABLE  (1ULL << 22)

static int pmc_amd_start(uint32_t slot, uint64_t event)
{
    if (slot >= g_pmc_info.num_counters)
        return -1;

    /* Clear counter */
    msr_write(MSR_AMD_PERF_CTR0 + slot * 2, 0);

    /* Configure: event code + enable + OS + USR */
    {
        uint64_t ctl = (event & 0xFFFF) | AMD_PERF_CTL_ENABLE
                     | PERFEVTSEL_OS | PERFEVTSEL_USR;
        msr_write(MSR_AMD_PERF_CTL0 + slot * 2, ctl);
    }

    return 0;
}

static uint64_t pmc_amd_read(uint32_t slot)
{
    if (slot >= g_pmc_info.num_counters)
        return 0;
    return msr_read(MSR_AMD_PERF_CTR0 + slot * 2);
}

static int pmc_amd_stop(uint32_t slot)
{
    if (slot >= g_pmc_info.num_counters)
        return -1;

    /* Clear enable bit */
    msr_write(MSR_AMD_PERF_CTL0 + slot * 2, 0);
    return 0;
}

/* ---- Init ---- */

void pmc_init(void)
{
    extern struct cpu_features g_cpu;

    g_pmc_info.available = 0;
    g_pmc_info.version = 0;
    g_pmc_info.num_counters = 0;
    g_pmc_info.counter_width = 0;
    g_pmc_info.is_amd = 0;

    /* Check vendor */
    if (g_cpu.vendor[0] == 'A') {
        /* AMD: probe PMC via msr_try_read on the first counter MSR,
         * then verify write works (hypervisors may allow reads but
         * trap writes, same issue as Intel PERFEVTSEL probe). */
        uint64_t val;
        if (msr_try_read(MSR_AMD_PERF_CTR0, &val) != 0) {
            klog(LOG_INFO, "pmc", "AMD PMC: counter MSR read failed; unavailable");
            return;
        }
        /* Write probe: write zero to CTL0 (disabled state), read back */
        msr_write(MSR_AMD_PERF_CTL0, 0);
        if (msr_try_read(MSR_AMD_PERF_CTL0, &val) != 0) {
            klog(LOG_WARN, "pmc", "AMD PMC: control MSR write/read failed; unavailable");
            return;
        }
        g_pmc_info.is_amd = 1;
        g_pmc_info.num_counters = 6;  /* Zen 2+ has 6 per core */
        g_pmc_info.counter_width = 48;
        g_pmc_info.available = 1;
        klog(LOG_INFO, "pmc", "AMD PMC: 6 counters, 48-bit width");
        return;
    }

    /* Intel: CPUID leaf 0x0A for PMU architectural info */
    if (g_cpu.max_leaf >= 0x0A) {
        uint32_t eax, ebx, ecx, edx;
        cpuid_raw(0x0A, 0, &eax, &ebx, &ecx, &edx);

        g_pmc_info.version       = (uint8_t)(eax & 0xFF);
        g_pmc_info.num_counters  = (uint8_t)((eax >> 8) & 0xFF);
        g_pmc_info.counter_width = (uint8_t)((eax >> 16) & 0xFF);

        if (g_pmc_info.version == 0 || g_pmc_info.num_counters == 0) {
            klog(LOG_INFO, "pmc", "Intel PMU: version 0 or no counters; unavailable");
            return;
        }

        if (g_pmc_info.num_counters > PMC_MAX_SLOTS)
            g_pmc_info.num_counters = PMC_MAX_SLOTS;

        /* Probe: try reading PERF_GLOBAL_CTRL to confirm MSR access works.
         * WHPX may trap PMU MSRs and #GP. Use CPUID-first, msr_try_read
         * as safety net (per kernel-code-quality Gate 6). */
        {
            uint64_t probe;
            if (msr_try_read(MSR_IA32_PERF_GLOBAL_CTRL, &probe) != 0) {
                klog(LOG_WARN, "pmc",
                     "Intel PMU v%u: PERF_GLOBAL_CTRL MSR probe failed; "
                     "hypervisor may block PMC access",
                     (uint64_t)g_pmc_info.version);
                return;
            }
        }

        /* Verify a write to PERFEVTSEL0 works (WHPX may trap writes
         * even if reads succeed). Write zero (disabled), then read back. */
        {
            uint64_t evtsel_probe;
            msr_write(MSR_IA32_PERFEVTSEL0, 0);
            if (msr_try_read(MSR_IA32_PERFEVTSEL0, &evtsel_probe) != 0) {
                klog(LOG_WARN, "pmc",
                     "Intel PMU v%u: PERFEVTSEL0 write/read failed; "
                     "PMC disabled", (uint64_t)g_pmc_info.version);
                return;
            }
        }

        g_pmc_info.available = 1;
        klog(LOG_INFO, "pmc",
             "Intel PMU v%u: %u counters, %u-bit width",
             (uint64_t)g_pmc_info.version,
             (uint64_t)g_pmc_info.num_counters,
             (uint64_t)g_pmc_info.counter_width);
    } else {
        klog(LOG_INFO, "pmc", "CPUID leaf 0x0A not available; PMC unavailable");
    }
}

/* ---- Vendor-neutral dispatch ---- */

int pmc_start(uint32_t slot, uint64_t event)
{
    if (!g_pmc_info.available || slot >= g_pmc_info.num_counters)
        return -1;
    return g_pmc_info.is_amd ? pmc_amd_start(slot, event)
                             : pmc_intel_start(slot, event);
}

uint64_t pmc_read(uint32_t slot)
{
    if (!g_pmc_info.available || slot >= g_pmc_info.num_counters)
        return 0;
    return g_pmc_info.is_amd ? pmc_amd_read(slot)
                             : pmc_intel_read(slot);
}

int pmc_stop(uint32_t slot)
{
    if (!g_pmc_info.available || slot >= g_pmc_info.num_counters)
        return -1;
    return g_pmc_info.is_amd ? pmc_amd_stop(slot)
                             : pmc_intel_stop(slot);
}

uint32_t pmc_ipc(void)
{
    uint64_t inst, cycles;
    uint64_t inst_event, cycle_event;

    if (!g_pmc_info.available || g_pmc_info.num_counters < 2)
        return 0;

    /* Select events based on vendor */
    if (g_pmc_info.is_amd) {
        inst_event  = PMC_AMD_INST_RETIRED;
        cycle_event = PMC_AMD_CPU_CLOCKS;
    } else {
        inst_event  = PMC_INTEL_INST_RETIRED;
        cycle_event = PMC_INTEL_UNHALTED_CYCLES;
    }

    /* Start both counters */
    pmc_start(0, inst_event);
    pmc_start(1, cycle_event);

    /* Spin for ~1M iterations as a measurement window.
     * Using a tight loop rather than a timer delay to avoid
     * scheduler/interrupt overhead that would skew the measurement. */
    {
        volatile uint32_t spin = 0;
        uint32_t j;
        for (j = 0; j < 1000000; j++)
            spin++;
        (void)spin;
    }

    /* Read and stop */
    inst   = pmc_read(0);
    cycles = pmc_read(1);
    pmc_stop(0);
    pmc_stop(1);

    if (cycles == 0)
        return 0;

    /* Return IPC as 8.8 fixed-point: (inst * 256) / cycles.
     * Clamp both the multiply and the quotient to uint32_t range. */
    if (inst > (0xFFFFFFFFFFFFFFFFULL / 256))
        return 0xFFFF;
    {
        uint64_t result = (inst * 256) / cycles;
        return (result > 0xFFFF) ? 0xFFFF : (uint32_t)result;
    }
}
