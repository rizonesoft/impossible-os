/* ============================================================================
 * log.h — Kernel logging subsystem
 *
 * Structured logging with severity levels.  All output goes to the serial
 * port (COM1, 0x3F8) for host-side capture — no framebuffer output, so logs
 * do not pollute the graphical desktop.
 *
 * Each message is prefixed with a severity tag and optional subsystem name:
 *   [INFO ][NET] DHCP lease obtained: 10.0.2.15
 *   [WARN ][FS]  Inode 42 has invalid block pointer
 *   [ERROR][MM]  Out of physical memory!
 *
 * Usage:
 *   log_info("NET", "DHCP lease obtained: %s", ip_str);
 *   log_warn("FS",  "Inode %u has invalid block pointer", inode);
 *   log_error("MM", "Out of physical memory!");
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Log levels ---- */

#define LOG_LEVEL_INFO   0
#define LOG_LEVEL_WARN   1
#define LOG_LEVEL_ERROR  2

/* ---- Minimum log level (compile-time filter) ---- */
/* Set to LOG_LEVEL_WARN to suppress INFO messages, etc. */

#ifndef LOG_MIN_LEVEL
#define LOG_MIN_LEVEL LOG_LEVEL_INFO
#endif

/* ---- API ---- */

/* Initialize the logging subsystem (call after serial_init) */
void log_init(void);

/* Log at INFO level — normal operational messages */
void log_info(const char *subsystem, const char *fmt, ...);

/* Log at WARN level — recoverable issues or unexpected states */
void log_warn(const char *subsystem, const char *fmt, ...);

/* Log at ERROR level — critical failures */
void log_error(const char *subsystem, const char *fmt, ...);
