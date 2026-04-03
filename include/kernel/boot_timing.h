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

/* Write the boot step timing report to C:\Impossible\System\Logs\boot-profile.log.
 * Call at desktop-ready, after VFS is mounted on C:. */
void boot_timing_write_report(void);

/* Step entry for debug bar rendering */
typedef struct {
    uint64_t    tsc;
    uint8_t     phase;
    uint16_t    postcode;
    const char *step;
} boot_timing_step_t;

/* Get the recorded step array. Returns count. */
uint32_t boot_timing_get_steps(const boot_timing_step_t **out);

/* ---- Boot performance regression detection ------------------------------- */

#define BOOT_PERF_MAX_RECORDS  32
#define BOOT_PERF_NAME_LEN     16  /* truncated step name stored in NVRAM */
#define BOOT_PERF_MAGIC        0x50455246  /* "PERF" */

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
