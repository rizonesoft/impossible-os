/* ============================================================================
 * wer.h -- Windows Error Reporting style crash report staging
 *
 * Writes structured JSON crash reports to X:\Crash\WER\ when a user
 * process crashes. FAT32-readable by any OS for post-mortem analysis.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

struct interrupt_frame;

/* Write a WER crash report for the current task.
 * Called from the exception handler when a user-mode fault is fatal.
 * frame: interrupt frame at the time of the crash.
 * exception: exception vector number (e.g. 14 for #PF). */
void wer_write_crash_report(struct interrupt_frame *frame, uint32_t exception);

/* Default MaxWerReports cap for X:\Crash\WER\ retention (registry-wiring to
 * HKLM\SYSTEM\BlackBox deferred -- shares the BlackBox cleanup MaxBootSessions registry-wiring follow-up). */
#define WER_MAX_REPORTS 64u

/* Prune the WER report directory toward at most `max` JSON reports, deleting
 * oldest-first by 14-digit timestamp (malformed names sort oldest). Best-effort,
 * not exact: vfs_readdir exposes only the first FAT32_MAX_DIR_ENTRIES live
 * entries with no truncation signal, so a directory padded with that many
 * foreign (non-report) files ahead of real reports can converge above `max`.
 * In practice X:\Crash\WER holds only PID_*.json reports, so the loop drives
 * the count to the cap. Call ONLY at X: mount and in the BlackBox low-space
 * disk cleanup -- never the exception path. Returns the number pruned. */
uint32_t wer_prune_reports(const char *dir, uint32_t max);
