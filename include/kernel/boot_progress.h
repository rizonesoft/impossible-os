/* ============================================================================
 * boot_progress.h -- Named-stage boot progress API
 *
 * High-level wrapper over boot_progress() that adds typed stage tracking,
 * a 32-entry stage history (capped), elapsed-ms timing, and splash progress
 * forwarding. Used by panic forensics and boot timing reports.
 *
 * XREF: 01-boot-platform/TODO-14-boot-diagnostics.md
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Boot stage enum ---------------------------------------------------- */

typedef enum {
    BOOT_STAGE_UEFI_INIT     =  0,
    BOOT_STAGE_ELF_LOADED    =  1,
    BOOT_STAGE_KERNEL_ENTRY  =  2,
    BOOT_STAGE_GDT_IDT       =  3,
    BOOT_STAGE_APIC          =  4,
    BOOT_STAGE_PMM           =  5,
    BOOT_STAGE_VMM           =  6,
    BOOT_STAGE_HEAP          =  7,
    BOOT_STAGE_KLOG          =  8,
    BOOT_STAGE_VFS           =  9,
    BOOT_STAGE_REGISTRY      = 10,
    BOOT_STAGE_DRIVERS       = 11,
    BOOT_STAGE_NETWORK       = 12,
    BOOT_STAGE_SCHEDULER     = 13,
    BOOT_STAGE_DESKTOP_READY = 14,
    BOOT_STAGE_COUNT         = 15,
} boot_stage_t;

/* ---- History entry ------------------------------------------------------ */

#define BOOT_STAGE_HISTORY_MAX  32

typedef struct {
    boot_stage_t stage;
    uint64_t     tsc;          /* raw TSC at report time */
    uint32_t     elapsed_ms;   /* ms since KERNEL_ENTRY */
    const char  *msg;          /* short description */
} boot_stage_entry_t;

/* ---- API ---------------------------------------------------------------- */

/* Report a boot stage milestone. Calls boot_progress(), records history,
 * forwards to boot_splash_status() if splash is active. */
void boot_stage_report(boot_stage_t stage, const char *msg);

/* Returns milliseconds since BOOT_STAGE_KERNEL_ENTRY was reported.
 * Returns 0 if TSC frequency is unknown or kernel entry not yet reported. */
uint32_t boot_get_elapsed_ms(void);

/* Get the stage history array. *out_count = number of valid entries (<=32). */
const boot_stage_entry_t *boot_stage_history_get(uint32_t *out_count);

/* Lightweight refresh for timer callbacks -- re-sends the last stage
 * message to boot_splash_status() without recording a new entry. */
void boot_progress_poll(void);

/* Write boot timeline. Path resolution: primary is
 * `X:\Perf\boot-timeline.json` when the BlackBox partition is mounted
 * (klog_using_blackbox);
 * fallback is `C:\Impossible\System\Logs\boot-timeline.json` if BlackBox
 * is unavailable. JSON array of records: FPDT firmware-phase entries
 * first (source="fpdt", unreliable=true on zero/garbage records), then
 * bootloader+kernel TSC steps (source="tsc"). Each record carries
 * {stage, phase, post, start_ms, duration_ms, target_ms, source,
 * unreliable}; target_ms is the per-step boot_perf_budget target
 * (0 when the step has no budget row). On a short write the file is
 * re-truncated to empty (fail closed) so consumers never see a
 * malformed JSON prefix.
 * When FPDT is reliable, all start_ms values are ms-since-firmware-reset
 * so FPDT and TSC entries form one monotonic timeline; when unreliable,
 * FPDT entries collapse to start_ms=0 with unreliable=true, and TSC
 * entries fall back to ms-since-bl_entry (also stamped unreliable=true
 * so consumers cannot mistake fallback for absolute time).
 * Contract pinned by 01-boot-platform/TODO-04 firmware-table-platform-inventory
 * (item: "Export JSON to X:\Perf\boot-timeline.json"). */
void boot_timeline_dump_json(void);

/* Render 4-digit hex POST code at native 8 px scale in top-right corner.
 * Writes I/O port 0x80 (high byte) for hardware POST cards.
 * Skips pixel writes if SUBSYS_FB not ready. */
void post_display16(uint16_t code);
