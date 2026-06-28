/*
 * hw_profile.h -- Boot-time hardware self-benchmark + auto-tune (TODO-09 S14)
 *
 * A one-shot Phase-3 benchmark (after task_init, before the compositor) that
 * measures the host's memory/SIMD/scheduling characteristics, persists the
 * result in HKLM\SYSTEM\HwProfile keyed by the CPU brand string, and auto-tunes
 * a small set of runtime knobs (SIMD dispatch flags) to the detected hardware.
 *
 * The profile is brand-keyed: a stored profile is reused on the next boot and
 * the benchmark is re-run only when absent or when the CPU brand changed
 * (different hardware). All five micro-benchmarks degrade gracefully: a metric
 * that cannot be taken (e.g. scratch allocation failed) is left zero and its
 * validity bit is cleared rather than failing the boot.
 *
 * Scope boundary: this owns the benchmark + the SIMD auto-tune. The scheduler-
 * quantum, compositor triple-buffer, and memops small-copy-threshold auto-tunes
 * named in the original plan need runtime knobs that do not exist yet; they are
 * filed as scope gaps in TODO-09 S14 (see the section's deferred bullets).
 */
#ifndef KERNEL_HW_PROFILE_H
#define KERNEL_HW_PROFILE_H

#include "kernel/types.h"

/* Validity bits in hw_profile_t.flags. HW_PROFILE_VALID marks a usable record;
 * the per-metric bits say which individual measurements succeeded. */
#define HW_PROFILE_VALID        0x00000001u  /* record is populated + persisted */
#define HW_PROFILE_BW_OK        0x00000002u  /* mem_bandwidth_mb_s valid        */
#define HW_PROFILE_LAT_OK       0x00000004u  /* mem_latency_ns valid            */
#define HW_PROFILE_CACHE_OK     0x00000008u  /* l1/l2/l3 sizes valid            */
#define HW_PROFILE_SIMD_OK      0x00000010u  /* simd_*_gpix_s valid             */
#define HW_PROFILE_CTX_OK       0x00000020u  /* ctx_switch_ns valid             */

/* Persisted hardware profile. Stored field-by-field in the Registry (not as a
 * raw blob) so the layout can evolve without an ABI bump. */
typedef struct {
    uint32_t mem_bandwidth_mb_s;   /* sequential write throughput (MB/s)       */
    uint32_t mem_latency_ns;       /* pointer-chase latency (ns/access)        */
    uint32_t l1_size_kb;           /* per-core L1 data cache (CPUID-sourced)   */
    uint32_t l2_size_kb;           /* per-core L2 cache                        */
    uint32_t l3_size_kb;           /* shared L3 cache                          */
    uint32_t simd_sse2_gpix_s;     /* SSE2 copy throughput (Mpix/s)            */
    uint32_t simd_avx_gpix_s;      /* AVX2 copy throughput (Mpix/s)            */
    uint32_t simd_avx512_gpix_s;   /* AVX-512 copy throughput (Mpix/s)         */
    uint32_t ctx_switch_ns;        /* measured scheduler context switch (ns)   */
    uint32_t tsc_mhz;              /* TSC frequency in MHz                     */
    uint32_t flags;                /* HW_PROFILE_* validity bits               */
} hw_profile_t;

/*
 * Run (or reuse) the boot self-benchmark and apply the SIMD auto-tune.
 * BSP-only, called once from boot_phase3() after task_init() and before
 * wm_init(). Never fails the boot: on any error it logs and returns.
 */
void hw_profile_init(void);

/* Read-only accessor for the active profile (NULL until hw_profile_init ran). */
const hw_profile_t *hw_profile_get(void);

/*
 * Pure helpers (no side effects) -- exposed for unit testing.
 *
 * hw_profile_is_stale: a stored profile is stale if it is not marked valid or
 * its brand string differs from the live CPU brand.
 */
int hw_profile_is_stale(const hw_profile_t *stored,
                        const char *stored_brand, const char *live_brand);

/*
 * SIMD auto-tune decision (pure). Given per-ISA throughput (Mpix/s, 0 = ISA
 * absent/unmeasured), decide which dispatch flags should stay enabled.
 * Mirrors the memops dispatch order (AVX-512 -> AVX2 -> SSE2):
 *   - clear AVX-512 if it is not at least HW_PROFILE_SIMD_GAIN_PCT% faster
 *     than AVX2 (consumer-CPU AVX-512 frequency throttling).
 *   - clear AVX2 if it is not at least HW_PROFILE_SIMD_GAIN_PCT% faster than
 *     SSE2 (no win, avoid the AVX/SSE transition + frequency cost).
 * Outputs are written only when non-NULL; 1 = keep enabled, 0 = disable.
 */
#define HW_PROFILE_SIMD_GAIN_PCT  10   /* require a >=10% gain to keep an ISA tier */

void hw_profile_simd_decision(uint32_t sse2_gpix, uint32_t avx_gpix,
                             uint32_t avx512_gpix,
                             int *keep_avx2, int *keep_avx512);

#endif /* KERNEL_HW_PROFILE_H */
