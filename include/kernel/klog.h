/* ============================================================================
 * klog.h -- Unified kernel logging
 *
 * Levels:
 *   LOG_DEBUG  [INFO] -- serial only (suppressed from framebuffer)
 *   LOG_INFO   [ OK ] -- serial + framebuffer (green prefix)
 *   LOG_WARN   [WARN] -- serial + framebuffer (yellow prefix)
 *   LOG_ERROR  [FAIL] -- serial + framebuffer (red prefix)
 *   LOG_FATAL  [CRIT] -- serial + framebuffer (red prefix), then halt
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

/* Primary log directory (BlackBox X:\Logs\) with C:\ fallback.
 * Resolved at runtime by klog_disk_init(): prefers X:\Logs\ if BlackBox
 * is mounted, falls back to C:\Impossible\System\Logs\ if not. */
#define KLOG_DIR_BLACKBOX  "X:\\Logs\\"
#define KLOG_DIR_FALLBACK  "C:\\Impossible\\System\\Logs\\"

/* Runtime-resolved log directory (set by klog_disk_init) */
extern const char *klog_dir;
extern int klog_using_blackbox;  /* 1 if X:\Logs\, 0 if C:\ fallback */

/* Log a message with level and subsystem tag.
 * fmt supports: %d, %u, %x, %p, %s, %c, %%  (same as printk) */
void klog(log_level_t level, const char *subsystem, const char *fmt, ...);

/* Ring buffer access for debug console */
typedef struct {
    log_level_t level;
    const char *subsystem;
    uint32_t    timestamp;  /* PIT ticks */
    uint8_t     cpu_id;     /* CPU that logged this entry (0 = BSP) */
    uint8_t     _pad[3];    /* alignment padding */
    uint32_t    pid;        /* process ID (0 during boot) */
    uint32_t    tid;        /* thread ID (0 during boot) */
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

/* Set per-subsystem minimum log level.
 * Entries below this level are dropped entirely (not stored in ring).
 * subsystem: tag string (e.g. "net", "fs"). NULL or "" sets the global default.
 * Up to 32 subsystem overrides can be active simultaneously. */
void klog_set_level(const char *subsystem, log_level_t min_level);

/* Load per-subsystem log levels from Registry.
 * Reads HKLM\SYSTEM\Logs\Levels\<subsystem> for each known tag.
 * Call after registry_init(). */
void klog_load_levels_from_registry(void);

/* ---- Split init (Phase 0 / Phase 2) ----
 * klog_early_init(): Phase 0 safe -- ring buffer + serial only, no VFS.
 * klog_disk_enable(): Phase 2 safe -- opens log files, starts disk flushing. */
void klog_early_init(void);            /* Phase 0: ring buffer ready */
void klog_disk_enable(void);           /* Phase 2: VFS-backed disk logging */

/* Get rate-limited dropped count for a subsystem (0 if no drops). */
uint32_t klog_get_dropped(const char *subsystem);

/* ---- Unified disk logging (klog_disk.c) ---- */
void klog_disk_init(void);             /* Allocate buffer, scan for log number */
void klog_disk_flush(void);            /* Write ring to C: + buffer to X: */
void klog_disk_set_live(int on);       /* Enable/disable per-entry live mode */
int  klog_disk_live_active(void);      /* Returns 1 if live mode is on */
void klog_disk_append(const klog_entry_t *e);  /* Append entry to FAT32 buffer */

/* ---- Crash-persistent log capture (klog.c) ---- */

#define KLOG_CRASH_MAGIC    0x4B4C4F47  /* "KLOG" */
#define KLOG_CRASH_PAGES    32          /* 128 KiB reserved region */

/* Header at start of crash persistence region (physical memory) */
typedef struct {
    uint32_t magic;             /* KLOG_CRASH_MAGIC */
    uint32_t entry_count;       /* number of ring entries saved */
    uint32_t crc32;             /* IEEE CRC32 of entries after header */
    uint32_t ring_head;         /* ring head at time of crash */
    uint64_t boot_timestamp;    /* PIT ticks at crash time */
} klog_crash_header_t;

/* Persist ring buffer to reserved physical memory (no kmalloc, no VFS).
 * Called from panic_screen() after BSOD render, before halt. */
void klog_crash_persist(void);

/* Check reserved region for valid crash data from previous boot.
 * If found, replays to serial with [CRASH-PREV] prefix.
 * Called early in klog_early_init(). */
void klog_crash_recover(void);

/* Write recovered crash entries to disk log file.
 * Called after VFS mount in klog_disk_enable(). */
void klog_crash_write_to_disk(void);

