/* ============================================================================
 * pmc.h -- Performance Monitoring Counters (Intel PMU + AMD PMC)
 *
 * Vendor-neutral API: pmc_start/pmc_read/pmc_stop dispatch to Intel or
 * AMD paths based on CPU vendor detected at boot.
 *
 * Intel: architectural PMU via CPUID leaf 0x0A. Uses IA32_PERFEVTSELx
 *        (MSR 0x186+N) and IA32_PMCx (MSR 0xC1+N). Enabled globally
 *        via IA32_PERF_GLOBAL_CTRL (MSR 0x38F).
 *
 * AMD:   per-counter control via MSR_AMD_PERF_CTL0 (0xC0010200) and
 *        MSR_AMD_PERF_CTR0 (0xC0010201), stride 2 per slot.
 *
 * XREF: 02-kernel-core/TODO-19-x86-64-architecture.md S10
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Intel PMU MSR addresses ---- */

#define MSR_IA32_PERFEVTSEL0    0x00000186
#define MSR_IA32_PMC0           0x000000C1

/* PERFEVTSEL bits */
#define PERFEVTSEL_EN           (1ULL << 22)  /* Enable counter */
#define PERFEVTSEL_OS           (1ULL << 17)  /* Count in ring 0 */
#define PERFEVTSEL_USR          (1ULL << 16)  /* Count in ring 3 */

/* ---- Intel pre-defined event codes ---- */

#define PMC_INTEL_INST_RETIRED      0xC0  /* Instructions retired */
#define PMC_INTEL_UNHALTED_CYCLES   0x3C  /* CPU_CLK_UNHALTED.THREAD */
#define PMC_INTEL_LLC_MISSES        0x2E  /* LAST_LEVEL_CACHE.MISS */
#define PMC_INTEL_BR_MISPREDICT     0xC5  /* BR_MISP_RETIRED */

/* ---- AMD pre-defined event codes ---- */

#define PMC_AMD_CPU_CLOCKS          0x76  /* CPU clocks not halted */
#define PMC_AMD_INST_RETIRED        0xC0  /* Retired instructions */
#define PMC_AMD_RET_BRANCHES        0xC2  /* Retired branches */
#define PMC_AMD_DRAM_ACCESSES       0x64  /* DRAM accesses (Zen 2+) */

/* ---- PMU capability info (populated by pmc_init) ---- */

typedef struct {
    uint8_t  version;       /* PMU architectural version (Intel CPUID 0x0A) */
    uint8_t  num_counters;  /* general-purpose counters per CPU */
    uint8_t  counter_width; /* counter bit width */
    uint8_t  available;     /* 1 if PMC infrastructure is usable */
    uint8_t  is_amd;        /* 1 if AMD PMC path, 0 if Intel PMU */
    uint8_t  _pad[3];
} pmc_info_t;

extern pmc_info_t g_pmc_info;

/* ---- Maximum slots ---- */

#define PMC_MAX_SLOTS  8  /* Intel architectural PMU v5 supports up to 8 */

/* ---- Vendor-neutral API ---- */

/* Initialize PMC subsystem. Probes CPUID and MSR availability. */
void pmc_init(void);

/* Start counting on a slot. event = architecture-specific event code.
 * Intel: event_select in bits 7:0, unit_mask in bits 15:8.
 * AMD: full event code (enable bit added automatically).
 * Returns 0 on success, -1 if slot out of range or PMC unavailable. */
int pmc_start(uint32_t slot, uint64_t event);

/* Read the current counter value for a slot. Returns 0 if unavailable. */
uint64_t pmc_read(uint32_t slot);

/* Stop counting on a slot. Returns 0 on success. */
int pmc_stop(uint32_t slot);

/* Measure IPC (instructions per cycle) over a short burst.
 * Returns IPC as 8.8 fixed-point (e.g., 0x180 = 1.5 IPC).
 * Returns 0 if PMC unavailable. */
uint32_t pmc_ipc(void);
