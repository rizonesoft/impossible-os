/* ============================================================================
 * partition.h -- Partition Scanner & Sub-Block-Device Layer
 *
 * Scans all registered block devices for partition tables (GPT first,
 * MBR fallback), creates sub-block-devices for each discovered partition
 * (offset reads/writes by the partition's start LBA), and probes each
 * partition for a known filesystem (FAT32, IXFS).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/fs/gpt.h"   /* struct gpt_guid (partition_info.unique_guid) */

/* Maximum partition sub-devices we can create */
#define PART_MAX  32

/* Detected filesystem type */
#define PART_FS_UNKNOWN  0
#define PART_FS_FAT32    1
#define PART_FS_IXFS     2
#define PART_FS_EXT2     3
#define PART_FS_NTFS     4

/* Partition info (stored alongside each sub-blkdev) */
struct partition_info {
    const struct blkdev *parent;    /* Parent physical block device */
    uint64_t start_lba;             /* Partition start on parent */
    uint64_t sector_count;          /* Partition size in sectors */
    int      fs_type;               /* PART_FS_* constant */
    int      disk_index;            /* Parent disk number (0-based) */
    int      part_index;            /* Partition number (1-based) */
    int      is_efi;                /* 1 if EFI System Partition (hidden) */
    int      ixfs_slot;             /* A/B dual-slot (TODO-21): 0=Slot A,
                                     * 1=Slot B, -1=not an IXFS-family GUID */
    struct gpt_guid unique_guid;    /* GPT unique partition GUID (zeroed for
                                     * MBR/raw); used to bind A/B root selection
                                     * to the boot disk (TODO-21) */
    char     gpt_name[37];          /* GPT partition name (ASCII, from UTF-16) */
};

/* ---- API ---- */

/* Scan all registered block devices for partitions.
 * Creates sub-blkdevs named "disk0p1", "disk0p2", etc.
 * Call once at boot after all disk drivers and IDT/PIC are initialized. */
void partition_scan_all(void);

/* Return a human-readable name for a filesystem type constant. */
const char *partition_fs_name(int fs_type);

/* Mount discovered filesystems (FAT32, IXFS) to VFS drive letters.
 * Call after partition_scan_all(). `active_slot` is the A/B slot the
 * bootloader selected (boot_info.active_slot: 0=A, 1=B); the IXFS partition
 * matching it is mounted as C:. Pass 0 for non-A/B boots (mounts the first
 * IXFS, preserving single-slot behavior). */
void partition_mount_filesystems(int active_slot);

/* A/B dual-slot (TODO-21): the slot whose IXFS was actually mounted as C:
 * (0=A, 1=B, -1=no IXFS mounted), and whether it differs from the slot the
 * bootloader selected. A mismatch means the selected slot's root was absent;
 * the mark-boot-successful path must refuse to mark the selected slot good. */
int  ab_boot_mounted_slot(void);
int  ab_boot_slot_mismatch(void);
