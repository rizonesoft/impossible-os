/* boot_trend.h -- rolling per-phase boot-perf trend + regression alarm.
 * Writes X:\Perf\boot-trend.json once per boot (last 16 boots) and emits
 * [WARN] BOOT-TREND when a phase's 3-run median grew >15% across the
 * non-overlapping prior/newest windows.  Atomic via vfs_rename_ex.
 * Schema v1 spec: docs/boot/boot-trend-schema.md. */

#ifndef KERNEL_BOOT_TREND_H
#define KERNEL_BOOT_TREND_H

#include "kernel/types.h"

#define BOOT_TREND_RING_DEPTH       16u
#define BOOT_TREND_WINDOW           3u
#define BOOT_TREND_WARN_THRESHOLD   15u

uint32_t boot_trend_compute_growth_pct(uint32_t prior_median,
                                        uint32_t newest_median);
uint32_t boot_trend_median3(uint32_t a, uint32_t b, uint32_t c);
void boot_trend_publish_json(void);

#endif
