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
