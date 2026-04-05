/* ============================================================================
 * fat32.h -- FAT32 Filesystem Driver (Multi-Volume)
 *
 * Parses FAT32 volumes via the block device abstraction layer, provides
 * VFS operations for directory listing and file reading. Supports both
 * 8.3 short names and LFN (Long File Name) entries.
 *
 * Each mounted FAT32 partition gets its own fat32_volume instance,
 * enabling simultaneous mounts (e.g., X: logs + D: user data).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/fs/vfs.h"

/* Forward declarations */
struct blkdev;
struct fat32_volume;

/* FAT32 BPB (BIOS Parameter Block) -- parsed from boot sector */
struct fat32_bpb {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t total_sectors;
    uint32_t fat_size_sectors;     /* sectors per FAT */
    uint32_t root_cluster;        /* first cluster of root directory */
    uint16_t fs_info_sector;      /* FSInfo sector number (BPB offset 48) */
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
 * sub-blkdev). Returns a volume context on success, NULL on failure.
 * The volume is allocated via PMM (~40 KB). */
struct fat32_volume *fat32_init(const struct blkdev *dev);

/* Stat a file or directory by path (e.g., "EFI/BOOT/BOOTX64.EFI").
 * Returns 0 on success, -1 if not found. */
int fat32_stat(struct fat32_volume *vol, const char *path,
               struct fat32_stat_info *info);

/* Get the VFS driver for the FAT32 filesystem */
struct vfs_fs_driver *fat32_get_driver(void);

/* Get the root VFS node for a specific volume */
struct vfs_node *fat32_get_root(struct fat32_volume *vol);

/* Get free space in bytes for a volume.
 * Returns cached FSInfo free count * cluster size. */
uint64_t fat32_get_free_bytes(struct fat32_volume *vol);

/* Get volume from a VFS root node (reverse of fat32_get_root).
 * Returns NULL if node is not a FAT32 root. */
struct fat32_volume *fat32_volume_from_root(struct vfs_node *root);

/* ---- Write operations ---- */

/* Create an empty file in the given directory cluster.
 * Returns 0 on success, -1 on failure. */
int fat32_create_file(uint32_t dir_cluster, const char *name);

/* Create a subdirectory with "." and ".." entries.
 * Returns 0 on success, -1 on failure. */
int fat32_create_dir(uint32_t parent_cluster, const char *name);

/* Write data to a file (overwrite mode -- replaces existing content).
 * Returns 0 on success, -1 on failure. */
int fat32_write_file(uint32_t dir_cluster, const char *name,
                     const void *data, uint32_t size);

/* Delete a file or directory by name from the given directory.
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

/* Filesystem consistency check.
 * Validates BPB, compares FAT1/FAT2, detects cross-linked chains and
 * lost clusters. fix=0: read-only scan. fix=1: repair (truncate cross-links,
 * free lost clusters). Returns 0 if clean, -1 if errors found/unfixable. */
int fat32_fsck(struct fat32_volume *vol, int fix);
