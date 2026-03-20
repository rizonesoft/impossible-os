/* ============================================================================
 * ntfs.h — NTFS Filesystem Driver (Read-Only)
 *
 * Parses NTFS volumes via the block device abstraction layer.
 * Reads the Boot Sector / BPB, locates the $MFT, and provides
 * VFS operations for directory listing and file reading.
 *
 * Each mounted NTFS partition gets its own ntfs_volume instance.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declarations */
struct blkdev;

/* NTFS volume context — parsed from boot sector BPB */
struct ntfs_volume {
    const struct blkdev *dev;       /* Underlying block device */

    /* BPB fields */
    uint16_t bytes_per_sector;      /* Typically 512 or 4096 */
    uint8_t  sectors_per_cluster;   /* Typically 8 */
    uint32_t cluster_size;          /* bytes_per_sector × sectors_per_cluster */
    uint64_t total_sectors;         /* 64-bit total sectors (offset 0x28) */

    /* MFT location */
    uint64_t mft_lcn;              /* Logical Cluster Number of $MFT */
    uint64_t mftmirr_lcn;         /* LCN of $MFTMirr */
    uint64_t mft_byte_offset;     /* mft_lcn × cluster_size */

    /* Record sizes */
    uint32_t frs_size;             /* File Record Segment size in bytes */
    uint32_t index_size;           /* Index record (INDX) size in bytes */

    /* Volume identity */
    uint64_t volume_serial;        /* Volume serial number (offset 0x48) */
};

/* ---- API ---- */

/* Parse the NTFS boot sector and create a volume context.
 * Returns a pointer to the volume on success, NULL on failure.
 * The volume struct is allocated via kmalloc (< 100 bytes). */
struct ntfs_volume *ntfs_init(const struct blkdev *dev);

/* Check if sector 0 of a block device contains a valid NTFS OEM ID.
 * Used by the partition scanner to identify NTFS partitions.
 * Returns 1 if NTFS, 0 otherwise. */
int ntfs_probe(const uint8_t *sector);
