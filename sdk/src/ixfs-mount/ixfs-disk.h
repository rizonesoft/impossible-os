/* ============================================================================
 * ixfs-disk.h — Platform-abstracted raw disk/image I/O with GPT parsing
 *
 * Opens a disk image or device, parses GPT to find a partition by index,
 * and provides sector-level read/write within that partition.
 * ============================================================================ */

#ifndef IXFS_DISK_H
#define IXFS_DISK_H

#include "ixfs-core.h"

/* Open a disk image or device, parse GPT, locate partition by 1-based index.
 * Returns a disk context on success, NULL on failure.
 * The caller must call disk_close() when done. */
ixfs_disk_ctx_t *disk_open(const char *path, int partition_index);

/* Read sectors within the partition.
 * lba is relative to partition start. Returns 0 on success. */
int disk_read_sectors(ixfs_disk_ctx_t *disk, uint64_t lba, uint32_t count,
                      void *buf);

/* Write sectors within the partition.
 * lba is relative to partition start. Returns 0 on success. */
int disk_write_sectors(ixfs_disk_ctx_t *disk, uint64_t lba, uint32_t count,
                       const void *buf);

/* Flush and close the disk context. */
void disk_close(ixfs_disk_ctx_t *disk);

#endif /* IXFS_DISK_H */
