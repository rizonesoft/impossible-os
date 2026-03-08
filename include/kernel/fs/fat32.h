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

/* ---- Write operations ---- */

/* Create an empty file in the given directory cluster.
 * Returns 0 on success, -1 on failure. */
int fat32_create_file(uint32_t dir_cluster, const char *name);

/* Create a subdirectory with "." and ".." entries.
 * Returns 0 on success, -1 on failure. */
int fat32_create_dir(uint32_t parent_cluster, const char *name);

/* Write data to a file (overwrite mode — replaces existing content).
 * Creates the directory entry if it doesn't exist.
 * Returns 0 on success, -1 on failure. */
int fat32_write_file(uint32_t dir_cluster, const char *name,
                     const void *data, uint32_t size);

/* Delete a file or directory by name from the given directory.
 * Frees the cluster chain and marks the directory entry as deleted.
 * Returns 0 on success, -1 if not found. */
int fat32_delete_file(uint32_t dir_cluster, const char *name);

/* Rename a file or directory within the same directory.
 * Returns 0 on success, -1 if not found. */
int fat32_rename(uint32_t dir_cluster,
                 const char *old_name, const char *new_name);

/* Format a block device as FAT32.
 * Writes BPB, FSInfo, both FAT copies, and empty root directory.
 * Returns 0 on success, -1 on failure. */
int fat32_format(const struct blkdev *dev, const char *label);

