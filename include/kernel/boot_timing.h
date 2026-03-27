/* ============================================================================
 * boot_timing.h — Boot performance timeline
 *
 * Parses the boot timing data passed from the bootloader (TSC timestamps
 * + FPDT firmware data) and provides human-readable boot phase durations.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Initialize boot timing — parse boot_info.timing and log a summary. */
void boot_timing_init(void);

/* Returns TSC frequency in Hz (0 = unknown). */
uint64_t boot_timing_tsc_freq(void);

/* ---- Boot step timeline -------------------------------------------------- */

#define BOOT_TIMING_MAX_STEPS 64

/* Record a named boot step with its raw TSC timestamp and POST code.
 * Safe to call before boot_timing_init() — captures TSC immediately.
 * phase: 0–3; step: short ASCII label; postcode: POSTCODE_* constant. */
void boot_timing_record_step(uint8_t phase, const char *step, uint8_t postcode);

/* Print all recorded step timings relative to the first step.
 * Call after boot_timing_init() so TSC frequency is calibrated. */
void boot_timing_print_steps(void);

/* Write the boot step timing report to C:\Impossible\System\Logs\boot-profile.log.
 * Call at desktop-ready, after VFS is mounted on C:. */
void boot_timing_write_report(void);
