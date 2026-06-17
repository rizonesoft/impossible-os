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

/* Prune the WER report directory to at most `max` JSON reports, deleting
 * oldest-first by 14-digit timestamp (malformed names sort oldest). Loops to a
 * fixed point past the 128-entry vfs_readdir cache. Call ONLY at X: mount and
 * in the BlackBox low-space disk cleanup -- never the exception path. Returns the
 * number pruned. */
uint32_t wer_prune_reports(const char *dir, uint32_t max);
