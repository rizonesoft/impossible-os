/* ============================================================================
 * fat32.h — FAT32 Read-Only Filesystem Driver
 *
 * Parses FAT32 volumes via the block device abstraction layer, provides
 * VFS operations for directory listing and file reading. Supports both
 * 8.3 short names and LFN (Long File Name) entries.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/fs/vfs.h"

/* Forward declaration */
struct blkdev;

/* FAT32 BPB (BIOS Parameter Block) — parsed from boot sector */
struct fat32_bpb {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t total_sectors;
    uint32_t fat_size_sectors;     /* sectors per FAT */
    uint32_t root_cluster;        /* first cluster of root directory */
    uint32_t first_data_sector;   /* computed: first sector of data region */
    uint32_t first_fat_sector;    /* computed: first sector of FAT */
};

/* File stat result */
struct fat32_stat_info {
    uint32_t size;            /* File size in bytes */
    uint8_t  attributes;     /* FAT32 attributes (read-only, hidden, etc.) */
    uint8_t  type;            /* VFS_FILE or VFS_DIRECTORY */
    uint16_t create_date;     /* FAT date format */
    uint16_t create_time;     /* FAT time format */
    uint16_t modify_date;
    uint16_t modify_time;
    uint32_t first_cluster;   /* First data cluster */
};

/* Initialize FAT32 driver from a block device (typically a partition
 * sub-blkdev). The sub-blkdev already handles LBA offset. */
int fat32_init(const struct blkdev *dev);

/* Stat a file or directory by path (e.g., "EFI/BOOT/BOOTX64.EFI").
 * Returns 0 on success, -1 if not found. */
int fat32_stat(const char *path, struct fat32_stat_info *info);

/* Get the VFS driver for the FAT32 filesystem */
struct vfs_fs_driver *fat32_get_driver(void);

/* Get the root VFS node */
struct vfs_node *fat32_get_root(void);
