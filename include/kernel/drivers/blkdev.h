/* ============================================================================
 * blkdev.h — Block Device Abstraction Layer
 *
 * Provides a unified interface for all block devices (ATA, VirtIO-blk, AHCI).
 * Each driver registers its device(s) at init time. Filesystem code uses
 * blkdev_read/write() without knowing which hardware driver backs the device.
 *
 * Device naming: "ata0", "virtio0", "sata0", "sata1", etc.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum registered block devices */
#define BLKDEV_MAX  16

/* Block device read/write/flush function pointer types */
typedef int (*blkdev_read_fn)(uint64_t lba, uint32_t count, void *buf,
                               void *driver_data);
typedef int (*blkdev_write_fn)(uint64_t lba, uint32_t count, const void *buf,
                                void *driver_data);
typedef int (*blkdev_flush_fn)(void *driver_data);

/* Block device descriptor */
struct blkdev {
    char         name[16];       /* Human-readable name ("sata0", "virtio0") */
    uint32_t     sector_size;    /* Bytes per sector (typically 512) */
    uint64_t     sector_count;   /* Total sectors */
    blkdev_read_fn  read;        /* Driver read function */
    blkdev_write_fn write;       /* Driver write function */
    blkdev_flush_fn flush;       /* Driver flush function (NULL = no cache) */
    void        *driver_data;    /* Opaque pointer passed to callbacks */
    uint8_t      active;         /* 1 if registered and valid */
};

/* ---- API ---- */

/* Register a block device. Returns 0 on success, -1 if full. */
int blkdev_register(const struct blkdev *dev);

/* Look up a block device by name. Returns pointer or NULL if not found. */
const struct blkdev *blkdev_get(const char *name);

/* Look up a block device by index (0-based). Returns pointer or NULL. */
const struct blkdev *blkdev_get_by_index(int index);

/* Read 'count' sectors starting at LBA into 'buf'.
 * Returns 0 on success, -1 on error. */
int blkdev_read(const struct blkdev *dev, uint64_t lba, uint32_t count,
                void *buf);

/* Write 'count' sectors starting at LBA from 'buf'.
 * Returns 0 on success, -1 on error. */
int blkdev_write(const struct blkdev *dev, uint64_t lba, uint32_t count,
                 const void *buf);

/* Flush volatile write cache to persistent storage.
 * Calls the driver's flush callback if available.
 * Returns 0 on success (or if no flush needed), -1 on error. */
int blkdev_sync(const struct blkdev *dev);

/* Get total number of registered block devices. */
int blkdev_count(void);

/* Print all registered block devices to serial/console. */
void blkdev_list(void);
