/* ============================================================================
 * klog.h — Unified kernel logging
 *
 * Levels:
 *   LOG_DEBUG  [??] — serial only (suppressed from framebuffer)
 *   LOG_INFO   [OK] — serial + framebuffer (green prefix)
 *   LOG_WARN   [--] — serial + framebuffer (yellow prefix)
 *   LOG_ERROR  [!!] — serial + framebuffer (red prefix)
 *   LOG_FATAL  [**] — serial + framebuffer (red prefix), then halt
 *
 * Usage:
 *   klog(LOG_INFO, "drv", "PS/2 mouse initialized (IRQ %u)", 12);
 *   klog(LOG_ERROR, "fs", "failed to mount C:");
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    LOG_DEBUG = 0,   /* [??] serial only */
    LOG_INFO  = 1,   /* [OK] serial + framebuffer */
    LOG_WARN  = 2,   /* [--] serial + framebuffer */
    LOG_ERROR = 3,   /* [!!] serial + framebuffer */
    LOG_FATAL = 4,   /* [**] serial + framebuffer, then halt */
} log_level_t;

/* Log a message with level and subsystem tag.
 * fmt supports: %d, %u, %x, %p, %s, %c, %%  (same as printk) */
void klog(log_level_t level, const char *subsystem, const char *fmt, ...);

/* Ring buffer access for debug console */
typedef struct {
    log_level_t level;
    const char *subsystem;
    uint32_t    timestamp;  /* PIT ticks */
    char        message[128];
} klog_entry_t;

/* Get pointer to ring buffer and current count.
 * Returns pointer to static array of KLOG_RING_SIZE entries. */
#define KLOG_RING_SIZE 1000

const klog_entry_t *klog_get_ring(uint32_t *out_count, uint32_t *out_head);

/* Set minimum level that appears on the framebuffer.
 * Default: LOG_INFO (i.e., DEBUG is serial-only).
 * Set to LOG_DEBUG to show everything on screen. */
void klog_set_screen_level(log_level_t min_level);

/* Flush ring buffer entries to C:\Impossible\System\Logs\kernel.log.
 * Call after VFS is mounted. Appends only new entries since last flush. */
void klog_flush_to_disk(void);

/* ---- Live debug log (X:\debug.log) ----
 * When enabled, every klog() entry is immediately written to X:\debug.log.
 * Used for debugging boot hangs on real hardware. */
void klog_live_enable(void);     /* Turn on — allocates PMM buffer, creates file */
int  klog_live_active(void);     /* Returns 1 if live mode is on */
void klog_live_append(const klog_entry_t *e);  /* Append entry to buffer */
void klog_live_flush(void);      /* Write entire buffer to X:\debug.log */
void klog_live_hw_dump(void);    /* Dump hardware info into live buffer */
