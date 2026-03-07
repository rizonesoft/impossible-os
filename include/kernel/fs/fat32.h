/* ============================================================================
 * fat32.h — FAT32 Read-Only Filesystem Driver
 *
 * Parses FAT32 volumes from ATA disk, provides VFS operations for
 * directory listing and file reading. Mounted at A:\ for the ESP.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/fs/vfs.h"

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

/* Initialize FAT32 driver from ATA disk starting at given LBA offset.
 * partition_lba = 0 for a raw FAT32 image (no partition table). */
int fat32_init(uint32_t partition_lba);

/* Get the VFS driver for the FAT32 filesystem */
struct vfs_fs_driver *fat32_get_driver(void);

/* Get the root VFS node */
struct vfs_node *fat32_get_root(void);
