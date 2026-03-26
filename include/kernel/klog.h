/* ============================================================================
 * klog.h — Unified kernel logging
 *
 * Levels:
 *   LOG_DEBUG  [INFO] — serial only (suppressed from framebuffer)
 *   LOG_INFO   [ OK ] — serial + framebuffer (green prefix)
 *   LOG_WARN   [WARN] — serial + framebuffer (yellow prefix)
 *   LOG_ERROR  [FAIL] — serial + framebuffer (red prefix)
 *   LOG_FATAL  [CRIT] — serial + framebuffer (red prefix), then halt
 *
 * Usage:
 *   klog(LOG_INFO, "drv", "PS/2 mouse initialized (IRQ %u)", 12);
 *   klog(LOG_ERROR, "fs", "failed to mount C:");
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    LOG_DEBUG = 0,   /* [INFO] serial only */
    LOG_INFO  = 1,   /* [ OK ] serial + framebuffer */
    LOG_WARN  = 2,   /* [WARN] serial + framebuffer */
    LOG_ERROR = 3,   /* [FAIL] serial + framebuffer */
    LOG_FATAL = 4,   /* [CRIT] serial + framebuffer, then halt */
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

/* ---- Unified disk logging (klog_disk.c) ----
 * Handles all disk output: C:\...\kernel.log (IXFS) + X:\BOOT_NNN.LOG (FAT32).
 * Call klog_disk_flush() after VFS is mounted. */
void klog_disk_init(void);             /* Allocate buffer, scan for log number */
void klog_disk_flush(void);            /* Write ring to C: + buffer to X: */
void klog_disk_set_live(int on);       /* Enable/disable per-entry live mode */
int  klog_disk_live_active(void);      /* Returns 1 if live mode is on */
void klog_disk_append(const klog_entry_t *e);  /* Append entry to FAT32 buffer */

