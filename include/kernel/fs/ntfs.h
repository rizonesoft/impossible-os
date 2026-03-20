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

/* ---- Attribute Iterator ---- */

/* Well-known attribute type IDs */
#define NTFS_ATTR_STANDARD_INFORMATION  0x10
#define NTFS_ATTR_ATTRIBUTE_LIST        0x20
#define NTFS_ATTR_FILE_NAME             0x30
#define NTFS_ATTR_OBJECT_ID             0x40
#define NTFS_ATTR_SECURITY_DESCRIPTOR   0x50
#define NTFS_ATTR_VOLUME_NAME           0x60
#define NTFS_ATTR_VOLUME_INFORMATION    0x70
#define NTFS_ATTR_DATA                  0x80
#define NTFS_ATTR_INDEX_ROOT            0x90
#define NTFS_ATTR_INDEX_ALLOCATION      0xA0
#define NTFS_ATTR_BITMAP                0xB0
#define NTFS_ATTR_REPARSE_POINT         0xC0
#define NTFS_ATTR_LOGGED_UTILITY_STREAM 0x100
#define NTFS_ATTR_END                   0xFFFFFFFF  /* $END marker */

/* Attribute flags (at common header offset 0x0C) */
#define NTFS_ATTR_FLAG_COMPRESSED  0x0001
#define NTFS_ATTR_FLAG_ENCRYPTED   0x4000
#define NTFS_ATTR_FLAG_SPARSE      0x8000

/* Parsed attribute common header — works for both resident and non-resident.
 * The iterator fills this from raw record bytes at each position. */
struct ntfs_attr_header {
    /* Common header (all attributes) */
    uint32_t type;               /* 0x00: Attribute type ID */
    uint32_t total_length;       /* 0x04: Total length including header */
    uint8_t  non_resident;       /* 0x08: 0=resident, 1=non-resident */
    uint8_t  name_length;        /* 0x09: Name length in UTF-16LE chars */
    uint16_t name_offset;        /* 0x0A: Offset to name from attr start */
    uint16_t flags;              /* 0x0C: NTFS_ATTR_FLAG_* */
    uint16_t attr_id;            /* 0x0E: Per-record attribute instance ID */

    /* Resident-only fields (valid when non_resident == 0) */
    uint32_t content_length;     /* 0x10: Length of attribute content */
    uint16_t content_offset;     /* 0x14: Offset to content from attr start */

    /* Raw pointer into the record buffer (not a copy) */
    const uint8_t *raw;          /* Points to start of this attribute */
};

/* Get pointer to the first attribute in a raw MFT record buffer.
 * Uses attrs_offset from the parsed header (hdr->attrs_offset).
 * Returns pointer into buf, or NULL if offset is invalid. */
const uint8_t *ntfs_attr_first(const uint8_t *record,
                               const struct ntfs_mft_header *hdr);

/* Advance to the next attribute. Returns NULL at $END or on bounds error.
 * used_size: hdr->used_size (bounds check). */
const uint8_t *ntfs_attr_next(const uint8_t *attr, const uint8_t *record,
                              uint32_t used_size);

/* Parse the common header fields from a raw attribute pointer.
 * Returns NTFS_OK, or NTFS_ERR_BAD_MAGIC if type == NTFS_ATTR_END. */
int ntfs_attr_parse(const uint8_t *attr, struct ntfs_attr_header *out);

/* Find the first attribute of a given type in a record.
 * Iterates from first to $END. Returns the raw pointer, or NULL.
 * If out is non-NULL, fills it with parsed header fields. */
const uint8_t *ntfs_attr_find(const uint8_t *record,
                              const struct ntfs_mft_header *hdr,
                              uint32_t type_id,
                              struct ntfs_attr_header *out);

/* Find a named attribute (type + UTF-16LE name match).
 * name is a kernel ASCII string — compared against UTF-16LE attr name.
 * Returns the raw pointer, or NULL if not found. */
const uint8_t *ntfs_attr_find_named(const uint8_t *record,
                                    const struct ntfs_mft_header *hdr,
                                    uint32_t type_id,
                                    const char *name,
                                    struct ntfs_attr_header *out);
