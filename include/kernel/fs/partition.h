/* ============================================================================
 * partition.h — Partition Scanner & Sub-Block-Device Layer
 *
 * Scans all registered block devices for partition tables (GPT first,
 * MBR fallback), creates sub-block-devices for each discovered partition
 * (offset reads/writes by the partition's start LBA), and probes each
 * partition for a known filesystem (FAT32, IXFS).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum partition sub-devices we can create */
#define PART_MAX  32

/* Detected filesystem type */
#define PART_FS_UNKNOWN  0
#define PART_FS_FAT32    1
#define PART_FS_IXFS     2
#define PART_FS_EXT2     3

/* Partition info (stored alongside each sub-blkdev) */
struct partition_info {
    const struct blkdev *parent;    /* Parent physical block device */
    uint64_t start_lba;             /* Partition start on parent */
    uint64_t sector_count;          /* Partition size in sectors */
    int      fs_type;               /* PART_FS_* constant */
    int      disk_index;            /* Parent disk number (0-based) */
    int      part_index;            /* Partition number (1-based) */
};

/* ---- API ---- */

/* Scan all registered block devices for partitions.
 * Creates sub-blkdevs named "disk0p1", "disk0p2", etc.
 * Call once at boot after all disk drivers and IDT/PIC are initialized. */
void partition_scan_all(void);

/* Return a human-readable name for a filesystem type constant. */
const char *partition_fs_name(int fs_type);
