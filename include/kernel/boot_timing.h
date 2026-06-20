/* ============================================================================
 * boot_timing.h -- Boot performance timeline
 *
 * Parses the boot timing data passed from the bootloader (TSC timestamps
 * + FPDT firmware data) and provides human-readable boot phase durations.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Initialize boot timing -- parse boot_info.timing and log a summary. */
void boot_timing_init(void);

/* Returns TSC frequency in Hz (0 = unknown). */
uint64_t boot_timing_tsc_freq(void);

/* ---- Boot step timeline -------------------------------------------------- */

#define BOOT_TIMING_MAX_STEPS 64

/* Record a named boot step with its raw TSC timestamp and POST code.
 * Safe to call before boot_timing_init() -- captures TSC immediately.
 * phase: 0–3; step: short ASCII label; postcode: POSTCODE_* constant. */
void boot_timing_record_step(uint8_t phase, const char *step, uint16_t postcode);

/* Print all recorded step timings relative to the first step.
 * Call after boot_timing_init() so TSC frequency is calibrated. */
void boot_timing_print_steps(void);

/* Write the boot step timing report. Path resolution (codified at
 * src/kernel/boot_timing.c:188-207): primary is `X:\Perf\boot-profile
 * .log` when the BlackBox partition is mounted (klog_using_blackbox);
 * fallback is `C:\Impossible\System\Logs\boot-profile.log` if BlackBox
 * is unavailable. */
void boot_timing_write_report(void);

/* Write POST code history to X:\Diag\postcode.log (or C:\ fallback). */
void boot_postcode_write_log(void);

/* Step entry for debug bar rendering */
typedef struct {
    uint64_t    tsc;
    uint8_t     phase;
    uint16_t    postcode;
    const char *step;
} boot_timing_step_t;

/* Get the recorded step array. Returns count. */
uint32_t boot_timing_get_steps(const boot_timing_step_t **out);

/* ---- FPDT + TSC normalization (firmware + bootloader unified timeline) ---- */

/* Returns 1 when the FPDT firmware-performance record is unreliable for
 * timeline use: not present, all zero, non-monotonic, or implausibly large.
 * Consumers either skip FPDT entries or stamp them `unreliable: true`. */
int boot_timing_fpdt_unreliable(void);

/* Pure variant for synthetic testing. `available=0` always returns 1.
 * Same monotonicity/zero/sanity rules as boot_timing_fpdt_unreliable(),
 * but takes the 5 FBPT phase nanosecond fields as explicit arguments. */
int boot_timing_fpdt_unreliable_eval(int available,
                                     uint64_t reset_end,
                                     uint64_t os_loader_load_start,
                                     uint64_t os_loader_start_start,
                                     uint64_t exit_bs_entry,
                                     uint64_t exit_bs_exit);

/* UEFI bootloader total wall time in ms (kernel_jump - bl_entry). Returns 0
 * if tsc_freq or bl_entry are unset. Single source of truth for both
 * boot_timing_init() log line and the VPD "UEFI Boot:" cell, so they
 * cannot drift. */
uint32_t boot_timing_uefi_total_ms(void);

/* Convert a TSC tick delta to milliseconds using the calibrated tsc_freq.
 * Returns 0 if tsc_freq is 0. Used by JSON exporter and VPD. */
uint32_t boot_timing_tsc_delta_ms(uint64_t ticks);

/* Anchor for unified timeline: ms-since-firmware-reset of the bootloader
 * `bl_entry` TSC sample. Returns 0 when FPDT is unreliable, in which case
 * callers fall back to ms-since-bl_entry as the timeline base. */
uint32_t boot_timing_bl_entry_ms_since_reset(void);

/* Normalized FPDT phase entry, ms-since-reset (or 0 when unreliable). */
typedef struct {
    const char *stage;          /* "fpdt:reset_end" etc. */
    uint32_t    start_ms;       /* ms since firmware reset */
    uint32_t    duration_ms;    /* delta to next FPDT phase, 0 for last */
    uint8_t     unreliable;     /* 1 = FPDT data not trustworthy */
} boot_timing_fpdt_entry_t;

/* Fill `out[]` (max `cap` entries) with the FPDT phase timeline in
 * monotonic order. Returns the number of entries written. Always emits
 * a fixed phase set so JSON consumers see consistent fields; entries are
 * stamped `unreliable=1` when boot_timing_fpdt_unreliable() is true. */
uint32_t boot_timing_get_fpdt_entries(boot_timing_fpdt_entry_t *out, uint32_t cap);

/* ---- Boot performance regression detection ------------------------------- */

#define BOOT_PERF_MAX_RECORDS  32
#define BOOT_PERF_NAME_LEN     16  /* truncated step name stored in NVRAM */
#define BOOT_PERF_MAGIC        0x50455246  /* "PERF" */
/* Sanity ceiling for a per-step elapsed_ms (10 minutes). Applied identically on
 * save, compare, and read so a corrupt/saturated tsc_to_ms() value cannot make
 * the NVRAM record non-round-trippable or overflow the regression math. */
#define BOOT_PERF_MS_SANITY_CAP 600000u

/* Per-step performance record stored in UEFI NVRAM across reboots.
 * Fixed-size so the NVRAM variable is a flat array with no pointers. */
typedef struct {
    char     name[BOOT_PERF_NAME_LEN];  /* step name (null-terminated) */
    uint32_t elapsed_ms;                /* duration from boot start */
    uint8_t  phase;                     /* boot phase 0-3 */
    uint8_t  _pad[3];                   /* alignment */
} boot_perf_record_t;

/* NVRAM variable header -- prefixed before the record array. */
typedef struct {
    uint32_t magic;                     /* BOOT_PERF_MAGIC */
    uint32_t count;                     /* number of records following */
} boot_perf_header_t;

/* Read previous boot perf from NVRAM into internal buffer.
 * Call early in boot (after uefi_runtime_init). */
void boot_perf_read_prev(void);

/* Compare current boot timings against previous and log regressions.
 * Call after boot_timing_print_steps() when all timings are finalized. */
void boot_perf_compare(void);

/* Write current boot timings to NVRAM for next-boot comparison.
 * Call once at end of Phase 3 (one NVRAM write per boot). */
void boot_perf_save(void);

/* Print all subsystem timings as a sorted table to serial. */
void boot_perf_dump(void);
