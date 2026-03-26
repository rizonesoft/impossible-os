/* ============================================================================
 * log.h — Kernel logging subsystem (DEPRECATED)
 *
 * This API has been superseded by klog() in "kernel/klog.h".
 * Use klog(LOG_INFO/WARN/ERROR, subsystem, fmt, ...) instead.
 * This file is retained for reference only; no active callers remain.
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
