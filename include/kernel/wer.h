/* ============================================================================
 * wer.h -- Windows Error Reporting style crash report staging
 *
 * Writes structured JSON crash reports to X:\Crash\WER\ when a user
 * process crashes. FAT32-readable by any OS for post-mortem analysis.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

struct interrupt_frame;

/* Max crash frames captured into the structured report (top-of-stack first).
 * Frame 0 is the faulting RIP; the remainder are the kernel terminal call
 * chain from RtlCaptureStackBackTrace (TODO-23 s7). User-space frames beyond
 * frame 0 are NOT walkable yet (the RBP walker rejects user PCs and the
 * user-mode RtlWalkFrameChain path is deferred), so the report marks them
 * unavailable rather than emitting a misleading or attacker-forgeable trace. */
#define WER_CRASH_MAX_FRAMES 8u

/* Fixed-size buffer for wer_format_fault_line's output. Worst case: "wer: fault
 * report code=" (23) + "0x"+8 code hex (10) + ", addr=" (7) + "0x"+16 addr hex
 * (18) + "\n" (1) + NUL (1) = 60; 72 leaves margin. */
#define WER_FAULT_LINE_MAX 72

/* Format the COMPLETE WerpReportFault logical line into buf, including the "wer: "
 * prefix and trailing LF:
 *   "wer: fault report code=0x<code>, addr=0x<addr>\n"
 * (lowercase hex, no leading-zero padding). This is the exact buffer
 * WerpReportFault hands to serial_write, so the unit test asserts the whole
 * logical line (prefix, body, and LF), not just the body. Note serial_write
 * normalizes the LF to CRLF on the UART, so the on-wire terminator is CR LF while
 * this logical buffer ends in a single LF. Pure and allocation-free. Returns the
 * bytes written excluding the NUL (0 if buf is NULL or bufsz < WER_FAULT_LINE_MAX). */
int wer_format_fault_line(char *buf, uint32_t bufsz, uint32_t code, uint64_t fault_addr);

/* WER fault hook (TODO-23 s12). The terminal path calls this once a user fault
 * is unhandled. A real werfault.exe would receive the report over a named pipe;
 * that user-mode reporter is ntdll/user side (SetUnhandledExceptionFilter and
 * the crash dialog) -> XREF: 12-user-platform-sdk/TODO-04 s5. For now it is a
 * serial-only stub, safe to call from fault context (no VFS, no allocation).
 * code: the exception NTSTATUS (e.g. 0xC0000005 STATUS_ACCESS_VIOLATION).
 * fault_addr: the faulting linear address (CR2 for #PF; 0 when unknown). */
void WerpReportFault(uint32_t code, uint64_t fault_addr);

/* Write a WER crash report for the current task.
 * Called from the exception handler when a user-mode fault is fatal.
 * frame: interrupt frame at the time of the crash.
 * exception: exception vector number (e.g. 14 for #PF).
 * fault_addr: faulting linear address if known (0 otherwise); emitted in the
 * report alongside the faulting RIP as report frame 0. */
void wer_write_crash_report(struct interrupt_frame *frame, uint32_t exception,
                            uint64_t fault_addr);

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
