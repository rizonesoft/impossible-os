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

/* ---- MFT Record Reader ---- */

/* MFT record magic numbers (little-endian) */
#define NTFS_MAGIC_FILE  0x454C4946  /* "FILE" */
#define NTFS_MAGIC_BAAD  0x44414142  /* "BAAD" */

/* MFT record flags */
#define NTFS_MFT_FLAG_IN_USE     0x0001  /* Record is allocated (not free) */
#define NTFS_MFT_FLAG_DIRECTORY  0x0002  /* Record is a directory */

/* Error codes for ntfs_read_mft_record() */
#define NTFS_OK              0
#define NTFS_ERR_IO         -1  /* Block device read failed */
#define NTFS_ERR_BAD_RECORD -2  /* Magic is "BAAD" — corrupt record */
#define NTFS_ERR_BAD_MAGIC  -3  /* Magic is unrecognized */
#define NTFS_ERR_FREE       -4  /* Record is not in-use (deleted) */
#define NTFS_ERR_FIXUP      -5  /* USA fixup failed — sector tear */

/* Parsed MFT record header — matches on-disk layout at documented offsets */
struct ntfs_mft_header {
    uint32_t magic;              /* 0x00: "FILE" or "BAAD" */
    uint16_t usa_offset;         /* 0x04: Offset to Update Sequence Array */
    uint16_t usa_size;           /* 0x06: Size of USA in words */
    uint64_t lsn;                /* 0x08: $LogFile Sequence Number */
    uint16_t seq_number;         /* 0x10: Sequence number (stale ref detect) */
    uint16_t hard_link_count;    /* 0x12: Number of hard links */
    uint16_t attrs_offset;       /* 0x14: Offset to first attribute */
    uint16_t flags;              /* 0x16: NTFS_MFT_FLAG_* */
    uint32_t used_size;          /* 0x18: Used size of record */
    uint32_t alloc_size;         /* 0x1C: Allocated size (should == frs_size) */
    uint64_t base_record_ref;    /* 0x20: Base record ref (0 if this IS base) */
};

/* Read a single MFT record by inode number.
 * buf must point to vol->frs_size bytes of writable memory.
 * On success, hdr is filled with the parsed header fields.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_read_mft_record(struct ntfs_volume *vol, uint64_t inode,
                         void *buf, struct ntfs_mft_header *hdr);

/* Apply Update Sequence Array fixup to a raw record buffer.
 * Detects sector tears and restores original last-2-bytes-per-sector.
 * Must be called AFTER reading from disk, BEFORE parsing attributes.
 * Works on both "FILE" (MFT) and "INDX" (index) records.
 * record_size: typically vol->frs_size (1024) or vol->index_size (4096).
 * sector_size: typically vol->bytes_per_sector (512).
 * Returns NTFS_OK on success, NTFS_ERR_FIXUP on sector tear. */
int ntfs_apply_fixup(uint8_t *buf, uint32_t record_size,
                     uint16_t sector_size);
