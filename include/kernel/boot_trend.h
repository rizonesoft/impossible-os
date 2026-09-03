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

/* Upper bound on entries EXAMINED per publish. A conforming file holds at
 * most BOOT_TREND_RING_DEPTH; 4x that tolerates a file written by an older or
 * hand-edited writer without walking an abusive one. The read is already
 * capped at the publisher's buffer size, so this is a work bound rather than
 * a safety bound. */
#define BOOT_TREND_MAX_SCAN         (4u * BOOT_TREND_RING_DEPTH)

/* A scan cap below the ring depth would stop the walk before the ring could
 * fill, silently shortening history on every publish. The two constants are
 * chosen independently, so the relation is asserted rather than assumed. */
_Static_assert(BOOT_TREND_MAX_SCAN >= BOOT_TREND_RING_DEPTH,
               "boot_trend scan cap must admit a full ring");

enum boot_trend_scan_action {
    BOOT_TREND_SCAN_STOP       = 0, /* past the scan bound: stop, then warn */
    BOOT_TREND_SCAN_COUNT_ONLY = 1, /* ring full: advance without emitting */
    BOOT_TREND_SCAN_CONSIDER   = 2, /* consider this entry for emission */
};

/* The publisher's whole scan policy, as a pure decision over the 1-based
 * position just consumed and the number of entries kept so far.
 *
 * COUNT_ONLY is the load-bearing case and the reason this is a named function
 * rather than two comparisons inline in the loop: a full ring must NOT end
 * the walk. The ring fills after 15 existing entries (kept starts at 1 for
 * the current boot), so stopping there would leave the counter at 15, and an
 * oversized array whose leading entries happen to be valid would be truncated
 * with no warning -- exactly the input the bound exists to report. That
 * defect was written once and caught in review; the host-side fixtures in
 * tools/boot-trend-scan-tests/ are what pin it now.
 *
 * Header-inline deliberately: the kernel image is at its ceiling, so this has
 * to cost nothing over the comparisons it replaces, and a host test can
 * include this header alone and exercise the SHIPPED policy rather than a
 * copy of it. */
static inline enum boot_trend_scan_action
boot_trend_scan_action(uint32_t seen, uint32_t kept)
{
    if (seen > BOOT_TREND_MAX_SCAN)      return BOOT_TREND_SCAN_STOP;
    if (kept >= BOOT_TREND_RING_DEPTH)   return BOOT_TREND_SCAN_COUNT_ONLY;
    return BOOT_TREND_SCAN_CONSIDER;
}

uint32_t boot_trend_compute_growth_pct(uint32_t prior_median,
                                        uint32_t newest_median);
uint32_t boot_trend_median3(uint32_t a, uint32_t b, uint32_t c);
void boot_trend_publish_json(void);

#endif
