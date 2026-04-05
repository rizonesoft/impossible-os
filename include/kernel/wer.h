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
