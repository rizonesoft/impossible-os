/* ============================================================================
 * hw_dump.h -- Structured hardware information dump
 *
 * Writes a hardware inventory to the kernel log via klog().
 * Extracted from klog_live.c to separate concerns.
 * ============================================================================ */

#pragma once

/* Dump all detected hardware info via klog() calls.
 * Output appears in serial, framebuffer (if debug), and all log files. */
void hw_dump_to_log(void);
