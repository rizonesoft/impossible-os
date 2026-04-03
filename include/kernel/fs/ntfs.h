/* ============================================================================
 * ntfs.h -- NTFS Filesystem Driver (Read-Only)
 *
 * Parses NTFS volumes via the block device abstraction layer.
 * Reads the Boot Sector / BPB, locates the $MFT, and provides
 * VFS operations for directory listing and file reading.
 *
 * Each mounted NTFS partition gets its own ntfs_volume instance.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/spinlock.h"

/* Forward declarations */
struct blkdev;
struct ntfs_data_run;
struct ntfs_mft_cache_entry;

/* Maximum data runs we store per cached non-resident attribute */
#define NTFS_MAX_BITMAP_RUNS  256

/* NTFS volume context -- parsed from boot sector BPB */
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

    /* ---- Cluster Bitmap (§12.1) ---- */
    uint64_t total_clusters;       /* Total clusters on the volume */
    uint64_t free_clusters;        /* Free clusters (cached count) */
    struct ntfs_data_run *bitmap_runs; /* PMM-allocated array of bitmap data runs */
    int      bitmap_run_count;     /* Number of valid bitmap data runs */
    uint64_t bitmap_size;          /* $Bitmap real size in bytes */
    uint8_t *bitmap_data;          /* PMM-allocated in-memory copy of entire $Bitmap */
    uint32_t bitmap_data_pages;    /* Number of PMM pages allocated for bitmap_data */

    /* MFT Zone -- reserved for MFT growth (first 12.5% of volume) */
    uint64_t mft_zone_start;       /* First LCN of MFT zone */
    uint64_t mft_zone_end;         /* Last LCN + 1 of MFT zone */

    spinlock_t bitmap_lock;        /* Serializes bitmap modifications */
    uint8_t  bitmap_loaded;        /* 1 if bitmap runs are loaded */

    /* ---- MFT Record Allocator (§12.3) ---- */
    struct ntfs_data_run *mft_data_runs;   /* $MFT's $DATA runs (where records live) */
    int      mft_data_run_count;           /* Number of $MFT data runs */
    uint64_t mft_data_size;                /* $MFT $DATA real size in bytes */

    struct ntfs_data_run *mft_bitmap_runs; /* $MFT's $BITMAP runs (which records exist) */
    int      mft_bitmap_run_count;         /* Number of $MFT bitmap data runs */
    uint64_t mft_bitmap_size;              /* $MFT $BITMAP real size in bytes */
    uint64_t mft_total_records;            /* mft_data_size / frs_size */

    spinlock_t mft_alloc_lock;             /* Serializes MFT record allocation */
    uint8_t  mft_alloc_loaded;             /* 1 if MFT bitmap is loaded */

    /* ---- System Metafiles (§7.1) ---- */
    char     volume_name[128];             /* Volume label from $Volume (inode 3) */
    uint8_t  ntfs_version_major;           /* NTFS major version (typically 3) */
    uint8_t  ntfs_version_minor;           /* NTFS minor version (typically 1) */
    uint8_t  volume_dirty;                 /* 1 if volume was not cleanly unmounted */
    uint16_t *upcase_table;                /* Unicode uppercase mapping (65536 entries, PMM-allocated) */
    uint8_t  sysfiles_loaded;              /* 1 if system metafiles are loaded */

    /* ---- MFT Record Cache (§10.1) ---- */
    struct ntfs_mft_cache_entry *mft_cache; /* LRU cache array (PMM-allocated) */
    uint32_t mft_cache_size;               /* Number of cache entries (default 64) */
    uint64_t mft_cache_hits;               /* Telemetry: cache hits */
    uint64_t mft_cache_misses;             /* Telemetry: cache misses */
    uint64_t mft_cache_evictions;          /* Telemetry: LRU evictions */
    uint8_t  mft_cache_loaded;             /* 1 if cache is initialized */

    /* ---- $LogFile Journal (§13.1) ---- */
    struct ntfs_data_run *log_runs;        /* $LogFile data runs */
    int      log_run_count;                /* Number of $LogFile data runs */
    uint64_t log_size;                     /* $LogFile real size in bytes */
    uint64_t log_page_size;                /* Log page size (typically 4096) */
    uint64_t log_current_lsn;              /* Last committed LSN */
    uint64_t log_write_pos;                /* Current write offset (circular) */
    uint64_t log_data_start;               /* Byte offset where data pages begin */
    uint64_t log_seq_bits;                 /* Sequence number bits in LSN */
    uint32_t log_next_txn_id;              /* Next transaction ID */
    spinlock_t log_lock;                   /* Serializes journal writes */
    uint8_t  journal_loaded;               /* 1 if journal is initialized */
};

/* Well-known NTFS system inode numbers */
#define NTFS_INODE_MFT       0   /* $MFT -- Master File Table */
#define NTFS_INODE_MFTMIRR   1   /* $MFTMirr -- MFT mirror (first 4 records) */
#define NTFS_INODE_LOGFILE   2   /* $LogFile -- Transaction journal */
#define NTFS_INODE_VOLUME    3   /* $Volume -- Volume name and flags */
#define NTFS_INODE_ATTRDEF   4   /* $AttrDef -- Attribute definitions */
#define NTFS_INODE_BITMAP    6   /* $Bitmap -- Cluster allocation bitmap */
#define NTFS_INODE_BOOT      7   /* $Boot -- Boot sector backup */
#define NTFS_INODE_BADCLUS   8   /* $BadClus -- Bad cluster list */
#define NTFS_INODE_UPCASE   10   /* $UpCase -- Unicode uppercase table */

/* ---- API ---- */

/* Parse the NTFS boot sector and create a volume context.
 * Returns a pointer to the volume on success, NULL on failure.
 * The volume struct is allocated via kmalloc (< 100 bytes). */
struct ntfs_volume *ntfs_init(const struct blkdev *dev);

/* Check if sector 0 of a block device contains a valid NTFS OEM ID.
 * Used by the partition scanner to identify NTFS partitions.
 * Returns 1 if NTFS, 0 otherwise. */
int ntfs_probe(const uint8_t *sector);

/* ---- System Metafile Readers (§7.1) ---- */

/* Load all system metafiles during mount.
 * Reads $Volume (inode 3) for label/version/dirty flag,
 * $Bitmap (inode 6) for free space counting,
 * $UpCase (inode 10) for Unicode uppercase table,
 * $MFTMirr (inode 1) for integrity check.
 * Non-fatal: individual failures are logged but don't abort mount.
 * Call after ntfs_init() and ntfs_bitmap_load(). */
int ntfs_load_sysfiles(struct ntfs_volume *vol);

/* Return the uppercase equivalent of a Unicode code point.
 * Uses the $UpCase table if loaded, falls back to ASCII towupper. */
uint16_t ntfs_upcase_char(const struct ntfs_volume *vol, uint16_t ch);

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
#define NTFS_ERR_BAD_RECORD -2  /* Magic is "BAAD" -- corrupt record */
#define NTFS_ERR_BAD_MAGIC  -3  /* Magic is unrecognized */
#define NTFS_ERR_FREE       -4  /* Record is not in-use (deleted) */
#define NTFS_ERR_FIXUP      -5  /* USA fixup failed -- sector tear */
#define NTFS_ERR_NOT_FOUND  -6  /* File not found in directory */
#define NTFS_ERR_READ_ONLY     -7  /* Write operation on read-only driver */
#define NTFS_ERR_FULL          -8  /* Disk full -- no free clusters or MFT records */
#define NTFS_ERR_ACCESS_DENIED -9  /* EFS: user's cert not in $EFS DDF list */
#define NTFS_ERR_NOT_READY    -10  /* EFS: CNG key store not initialized */

/* Parsed MFT record header -- matches on-disk layout at documented offsets */
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

/* Read a single MFT record by inode number (CACHED).
 * Checks the LRU cache first; on miss, reads from disk and caches.
 * buf must point to vol->frs_size bytes of writable memory.
 * On success, hdr is filled with the parsed header fields.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_read_mft_record(struct ntfs_volume *vol, uint64_t inode,
                         void *buf, struct ntfs_mft_header *hdr);

/* Read a single MFT record by inode number (UNCACHED).
 * Always reads from disk. Used by the cache module itself and
 * during early boot before the cache is initialized.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_read_mft_record_raw(struct ntfs_volume *vol, uint64_t inode,
                             void *buf, struct ntfs_mft_header *hdr);

/* ---- MFT Record Cache (§10.1) ---- */

/* Default cache size (number of MFT record slots) */
#define NTFS_MFT_CACHE_DEFAULT_SIZE  64

/* Pinned inode flags (never evicted from cache) */
#define NTFS_CACHE_PIN_MFT   (1u << 0)  /* Inode 0: $MFT */
#define NTFS_CACHE_PIN_ROOT  (1u << 1)  /* Inode 5: root dir */

/* Initialize the MFT record cache. Call after ntfs_init().
 * cache_size: number of entries (0 = use default of 64). */
int ntfs_cache_init(struct ntfs_volume *vol, uint32_t cache_size);

/* Invalidate a specific inode's cache entry (e.g., after write). */
void ntfs_cache_invalidate(struct ntfs_volume *vol, uint64_t inode);

/* Log cache telemetry: hit rate, evictions. */
void ntfs_cache_log_stats(const struct ntfs_volume *vol);

/* Apply Update Sequence Array fixup to a raw record buffer.
 * Detects sector tears and restores original last-2-bytes-per-sector.
 * Must be called AFTER reading from disk, BEFORE parsing attributes.
 * Works on both "FILE" (MFT) and "INDX" (index) records.
 * record_size: typically vol->frs_size (1024) or vol->index_size (4096).
 * sector_size: typically vol->bytes_per_sector (512).
 * Returns NTFS_OK on success, NTFS_ERR_FIXUP on sector tear. */
int ntfs_apply_fixup(uint8_t *buf, uint32_t record_size,
                     uint16_t sector_size);

/* Regenerate the Update Sequence Array before writing a record to disk.
 * This is the REVERSE of ntfs_apply_fixup():
 *   1. Increment USN (wrap 0→1 since USN 0 is invalid)
 *   2. Save each sector's last 2 bytes into the USA replacement entries
 *   3. Stamp each sector's last 2 bytes with the new USN
 * Must be called BEFORE writing to disk, AFTER modifying attributes.
 * Works on both "FILE" (MFT) and "INDX" (index) records.
 * Returns NTFS_OK on success, NTFS_ERR_FIXUP on bad USA layout. */
int ntfs_regenerate_fixup(uint8_t *buf, uint32_t record_size,
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

/* Parsed attribute common header -- works for both resident and non-resident.
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
 * name is a kernel ASCII string -- compared against UTF-16LE attr name.
 * Returns the raw pointer, or NULL if not found. */
const uint8_t *ntfs_attr_find_named(const uint8_t *record,
                                    const struct ntfs_mft_header *hdr,
                                    uint32_t type_id,
                                    const char *name,
                                    struct ntfs_attr_header *out);

/* ---- $ATTRIBUTE_LIST Handler (§3.4, attribute type 0x20) ---- */

/* A parsed entry from the $ATTRIBUTE_LIST attribute.
 * The list maps which attributes live in which MFT record. */
struct ntfs_attrlist_entry {
    uint32_t type;         /* 0x00: Attribute type ID */
    uint16_t entry_length; /* 0x04: Length of this list entry */
    uint8_t  name_length;  /* 0x06: Name length (UTF-16 chars) */
    uint8_t  name_offset;  /* 0x07: Offset to name from entry start */
    uint64_t start_vcn;    /* 0x08: Starting VCN (for split non-res attrs) */
    uint64_t mft_reference; /* 0x10: MFT reference of record holding attr */
    uint64_t mft_inode;    /* Extracted: low 48 bits of mft_reference */
    uint16_t attr_id;      /* 0x18: Attribute instance ID */
};

/* Extended attribute search: tries base record first, then follows
 * $ATTRIBUTE_LIST references to extension MFT records.
 *
 * vol: NTFS volume (needed to read extension records).
 * record: raw base MFT record buffer (after fixup).
 * hdr: parsed base MFT record header.
 * type_id: attribute type to find.
 * out: filled with parsed header of found attribute.
 * ext_record: if the attribute was found in an extension record, this is
 *   set to the PMM-allocated buffer. Caller MUST call pmm_free_frame()
 *   on it when done. Set to 0 if attribute found in base record.
 * ext_hdr: if ext_record is set, filled with the extension record header.
 *
 * Returns raw pointer to the attribute, or NULL if not found anywhere.
 * The returned pointer is valid within either `record` (if ext_record==0)
 * or within `ext_record` (if ext_record!=0). */
const uint8_t *ntfs_attr_find_ext(struct ntfs_volume *vol,
                                   const uint8_t *record,
                                   const struct ntfs_mft_header *hdr,
                                   uint32_t type_id,
                                   struct ntfs_attr_header *out,
                                   uintptr_t *ext_record,
                                   struct ntfs_mft_header *ext_hdr);

/* Extended named attribute search (type + name match, with $ATTRIBUTE_LIST
 * fallback). Same semantics as ntfs_attr_find_ext() but also matches name. */
const uint8_t *ntfs_attr_find_named_ext(struct ntfs_volume *vol,
                                         const uint8_t *record,
                                         const struct ntfs_mft_header *hdr,
                                         uint32_t type_id,
                                         const char *name,
                                         struct ntfs_attr_header *out,
                                         uintptr_t *ext_record,
                                         struct ntfs_mft_header *ext_hdr);

/* ---- $FILE_NAME Decoder (attribute type 0x30) ---- */

/* Filename namespace values */
#define NTFS_NS_POSIX     0x00  /* Case-sensitive, any chars */
#define NTFS_NS_WIN32     0x01  /* Case-insensitive, restricted chars */
#define NTFS_NS_DOS       0x02  /* 8.3 short name */
#define NTFS_NS_WIN32DOS  0x03  /* Compliant with both Win32 and DOS */

/* Maximum filename length (NTFS allows up to 255 UTF-16 chars) */
#define NTFS_MAX_NAME  255

/* Parsed $FILE_NAME attribute content */
struct ntfs_file_name {
    /* Parent directory reference */
    uint64_t parent_inode;       /* Low 48 bits of parent MFT ref */
    uint16_t parent_seq;         /* High 16 bits -- sequence number */

    /* Duplicated timestamps (100-ns intervals since 1601-01-01) */
    uint64_t creation_time;      /* 0x08 */
    uint64_t modification_time;  /* 0x10 */
    uint64_t mft_change_time;    /* 0x18 */
    uint64_t access_time;        /* 0x20 */

    /* Sizes */
    uint64_t allocated_size;     /* 0x28 */
    uint64_t real_size;          /* 0x30 */

    /* Flags and name */
    uint32_t flags;              /* 0x38: directory, compressed, hidden, etc. */
    uint8_t  name_length;        /* 0x40: filename length in UTF-16 chars */
    uint8_t  name_space;         /* 0x41: NTFS_NS_* */
    char     name[NTFS_MAX_NAME + 1]; /* Decoded ASCII (lossy for non-ASCII) */
};

/* Decode the first (or best) $FILE_NAME attribute from an MFT record.
 * Scans all $FILE_NAME attrs, prefers Win32 (0x01) or Win32/DOS (0x03).
 * record: raw MFT record buffer (after fixup).
 * hdr: parsed MFT record header.
 * out: filled with decoded filename data.
 * Returns NTFS_OK on success, NTFS_ERR_BAD_MAGIC if no $FILE_NAME found. */
int ntfs_decode_file_name(const uint8_t *record,
                          const struct ntfs_mft_header *hdr,
                          struct ntfs_file_name *out);

/* ---- Data Run Decoder (§4.1) ---- */

/* Sentinel LCN value for sparse (unallocated) runs -- reads as all zeros */
#define NTFS_LCN_SPARSE  ((uint64_t)-1)

/* Attribute flags (at offset 0x0C in attribute header) */
#define NTFS_ATTR_FLAG_COMPRESSED  0x0001  /* Data is LZNT1-compressed */
#define NTFS_ATTR_FLAG_ENCRYPTED   0x4000  /* Data is encrypted (EFS) */
#define NTFS_ATTR_FLAG_SPARSE      0x8000  /* Data has sparse ranges */

/* A single data run: maps a range of Virtual Cluster Numbers to disk LCNs */
struct ntfs_data_run {
    uint64_t vcn_start;    /* First VCN this run covers */
    uint64_t lcn;          /* Starting LCN on disk (NTFS_LCN_SPARSE if sparse) */
    uint64_t length;       /* Number of clusters in this run */
};

/* Non-resident attribute header fields (parsed from attr raw bytes) */
struct ntfs_nonres_header {
    uint64_t start_vcn;      /* 0x10: Starting VCN */
    uint64_t last_vcn;       /* 0x18: Last VCN */
    uint16_t data_run_off;   /* 0x20: Offset to data runs from attr start */
    uint16_t compression_unit; /* 0x22: Compression unit shift (0 = none, 4 = 16 clusters) */
    uint64_t alloc_size;     /* 0x28: Allocated size in bytes */
    uint64_t real_size;      /* 0x30: Real (used) size in bytes */
    uint64_t init_size;      /* 0x38: Initialized size in bytes */
};

/* Decode data runs from a non-resident attribute.
 * attr: raw pointer to the attribute (from iterator).
 * runs: output array to fill with decoded runs.
 * max_runs: capacity of runs[].
 * nrhdr: if non-NULL, filled with non-resident header fields.
 * Returns the number of runs decoded (0..max_runs), or -1 on error. */
int ntfs_decode_data_runs(const uint8_t *attr, struct ntfs_data_run *runs,
                          int max_runs, struct ntfs_nonres_header *nrhdr);

/* ---- $STANDARD_INFORMATION Decoder (attribute type 0x10) ---- */

/* DOS file attribute flags (from $STANDARD_INFORMATION offset 0x20) */
#define NTFS_FILE_ATTR_READONLY    0x0001
#define NTFS_FILE_ATTR_HIDDEN      0x0002
#define NTFS_FILE_ATTR_SYSTEM      0x0004
#define NTFS_FILE_ATTR_ARCHIVE     0x0020
#define NTFS_FILE_ATTR_DEVICE      0x0040
#define NTFS_FILE_ATTR_NORMAL      0x0080
#define NTFS_FILE_ATTR_TEMPORARY   0x0100
#define NTFS_FILE_ATTR_SPARSE      0x0200
#define NTFS_FILE_ATTR_REPARSE     0x0400
#define NTFS_FILE_ATTR_COMPRESSED  0x0800
#define NTFS_FILE_ATTR_OFFLINE     0x1000
#define NTFS_FILE_ATTR_NOT_INDEXED 0x2000
#define NTFS_FILE_ATTR_ENCRYPTED   0x4000

/* Parsed $STANDARD_INFORMATION attribute content */
struct ntfs_std_info {
    /* Timestamps -- raw FILETIME (100-ns intervals since 1601-01-01) */
    uint64_t creation_time;      /* 0x00 */
    uint64_t modification_time;  /* 0x08 */
    uint64_t mft_change_time;    /* 0x10 */
    uint64_t access_time;        /* 0x18 */

    /* Converted Unix timestamps (seconds since 1970-01-01) */
    uint64_t creation_unix;
    uint64_t modification_unix;
    uint64_t mft_change_unix;
    uint64_t access_unix;

    /* DOS permission flags */
    uint32_t dos_permissions;    /* 0x20: NTFS_FILE_ATTR_* */

    /* NTFS 3.0+ extended fields (valid when content_length >= 0x48) */
    uint32_t max_versions;       /* 0x24: Maximum versions (0 = disabled) */
    uint32_t version_number;     /* 0x28: Version number */
    uint32_t class_id;           /* 0x2C: Class ID */
    uint32_t owner_id;           /* 0x30: Owner ID (quota) */
    uint32_t security_id;        /* 0x34: Security ID → $Secure:$SII index */
    uint64_t quota_charged;      /* 0x38: Quota charged */
    uint64_t usn;                /* 0x40: Update Sequence Number (USN) */
    uint8_t  has_extended;       /* Set to 1 if extended fields are present */
};

/* Convert a Windows FILETIME (100-ns since 1601-01-01) to Unix timestamp.
 * Returns seconds since 1970-01-01, or 0 if the FILETIME is before epoch. */
uint64_t ntfs_filetime_to_unix(uint64_t filetime);

/* Decode the $STANDARD_INFORMATION attribute from an MFT record.
 * record: raw MFT record buffer (after fixup).
 * hdr: parsed MFT record header.
 * out: filled with timestamps and DOS permissions.
 * Returns NTFS_OK on success, NTFS_ERR_BAD_MAGIC if attr not found. */
int ntfs_decode_std_info(const uint8_t *record,
                         const struct ntfs_mft_header *hdr,
                         struct ntfs_std_info *out);

/* ---- $SECURITY_DESCRIPTOR Parser (§3.5, attribute type 0x50) ---- */

/* Maximum sub-authorities in a SID (Windows limit is 15) */
#define NTFS_SID_MAX_SUB_AUTH  15

/* Maximum ACEs in an ACL we'll parse */
#define NTFS_ACL_MAX_ACES  64

/* Parsed Security Identifier (SID).
 * Format: S-{revision}-{authority}-{sub1}-{sub2}-...
 * Common SIDs: S-1-5-18 (SYSTEM), S-1-5-32-544 (Administrators) */
struct ntfs_sid {
    uint8_t  revision;           /* Must be 1 */
    uint8_t  sub_auth_count;     /* Number of sub-authorities */
    uint8_t  authority[6];       /* 6-byte big-endian identifier authority */
    uint64_t authority_value;    /* Decoded authority as integer */
    uint32_t sub_authorities[NTFS_SID_MAX_SUB_AUTH];
};

/* ACE types */
#define NTFS_ACE_ACCESS_ALLOWED  0x00
#define NTFS_ACE_ACCESS_DENIED   0x01
#define NTFS_ACE_SYSTEM_AUDIT    0x02
#define NTFS_ACE_SYSTEM_ALARM    0x03

/* ACE flags */
#define NTFS_ACE_OBJECT_INHERIT      0x01
#define NTFS_ACE_CONTAINER_INHERIT   0x02
#define NTFS_ACE_NO_PROPAGATE        0x04
#define NTFS_ACE_INHERIT_ONLY        0x08
#define NTFS_ACE_INHERITED           0x10

/* Parsed Access Control Entry */
struct ntfs_ace {
    uint8_t  type;               /* NTFS_ACE_* type */
    uint8_t  flags;              /* NTFS_ACE_* flags */
    uint16_t size;               /* Total ACE size in bytes */
    uint32_t access_mask;        /* Access rights bitmask */
    struct ntfs_sid sid;         /* Principal this ACE applies to */
};

/* Parsed Access Control List (DACL or SACL) */
struct ntfs_acl {
    uint8_t  revision;           /* ACL revision (2 or 4) */
    uint16_t size;               /* Total ACL size in bytes */
    uint16_t ace_count;          /* Number of ACEs in the list */
    struct ntfs_ace aces[NTFS_ACL_MAX_ACES]; /* Parsed ACEs */
    uint16_t parsed_count;       /* ACEs actually parsed (<= ace_count) */
};

/* Security descriptor control flags */
#define NTFS_SD_OWNER_DEFAULTED   0x0001
#define NTFS_SD_GROUP_DEFAULTED   0x0002
#define NTFS_SD_DACL_PRESENT      0x0004
#define NTFS_SD_DACL_DEFAULTED    0x0008
#define NTFS_SD_SACL_PRESENT      0x0010
#define NTFS_SD_SACL_DEFAULTED    0x0020
#define NTFS_SD_DACL_AUTO_INHERIT 0x0400
#define NTFS_SD_SACL_AUTO_INHERIT 0x0800
#define NTFS_SD_DACL_PROTECTED    0x1000
#define NTFS_SD_SACL_PROTECTED    0x2000
#define NTFS_SD_SELF_RELATIVE     0x8000

/* Parsed security descriptor (self-relative format) */
struct ntfs_security_desc {
    uint8_t  revision;           /* 0x00: Must be 1 */
    uint16_t control;            /* 0x02: NTFS_SD_* control flags */
    struct ntfs_sid owner;       /* Owner SID */
    struct ntfs_sid group;       /* Group SID */
    struct ntfs_acl dacl;        /* Discretionary ACL (if DACL_PRESENT) */
    struct ntfs_acl sacl;        /* System ACL (if SACL_PRESENT) */
    uint8_t  has_owner;          /* 1 if owner SID parsed */
    uint8_t  has_group;          /* 1 if group SID parsed */
    uint8_t  has_dacl;           /* 1 if DACL parsed */
    uint8_t  has_sacl;           /* 1 if SACL parsed */
};

/* Decode a self-relative security descriptor from raw bytes.
 * data: pointer to the security descriptor bytes.
 * data_len: length of the data.
 * out: filled with parsed security descriptor.
 * Returns NTFS_OK on success, error code on failure. */
int ntfs_parse_security_desc(const uint8_t *data, uint32_t data_len,
                              struct ntfs_security_desc *out);

/* Decode the security descriptor from an MFT record.
 * First tries inline $SECURITY_DESCRIPTOR (type 0x50).
 * Falls back to $STANDARD_INFORMATION security_id (NTFS 3.0+).
 * Note: $Secure lookup is logged but not implemented (requires inode 9).
 * Returns NTFS_OK if a descriptor was found and parsed. */
int ntfs_decode_security(const uint8_t *record,
                          const struct ntfs_mft_header *hdr,
                          struct ntfs_volume *vol,
                          struct ntfs_security_desc *out);

/* Format a parsed SID into the string "S-1-5-21-..." representation.
 * buf: output buffer.
 * buf_len: capacity of buf.
 * Returns the number of characters written (excluding null terminator). */
int ntfs_format_sid(const struct ntfs_sid *sid, char *buf, int buf_len);

/* ---- $REPARSE_POINT Parser (§3.6, attribute type 0xC0) ---- */

/* Reparse tag values */
#define NTFS_REPARSE_TAG_MOUNT_POINT  0xA0000003  /* Junction (directory) */
#define NTFS_REPARSE_TAG_SYMLINK      0xA000000C  /* Symbolic link */
#define NTFS_REPARSE_TAG_WOF          0x80000017  /* Windows Overlay FS */

/* Symlink flags */
#define NTFS_SYMLINK_FLAG_RELATIVE    0x00000001  /* Relative symlink */

/* Reparse point types (decoded) */
#define NTFS_REPARSE_JUNCTION   1  /* Junction / mount point */
#define NTFS_REPARSE_SYMLINK    2  /* Symbolic link */
#define NTFS_REPARSE_OTHER      3  /* Other/unknown reparse tag */

/* Maximum target path length we'll decode */
#define NTFS_REPARSE_MAX_PATH   512

/* Parsed reparse point data */
struct ntfs_reparse_data {
    uint32_t tag;                  /* Raw reparse tag */
    uint8_t  type;                 /* NTFS_REPARSE_* decoded type */
    uint16_t data_length;          /* Reparse data length (after header) */
    uint32_t symlink_flags;        /* Symlink flags (0x01 = relative) */

    /* Decoded target path (ASCII, lossy conversion from UTF-16LE) */
    char     substitute_name[NTFS_REPARSE_MAX_PATH + 1];
    char     print_name[NTFS_REPARSE_MAX_PATH + 1];

    uint8_t  is_relative;          /* 1 if relative symlink */
};

/* Decode the $REPARSE_POINT attribute from an MFT record.
 * record: raw MFT record buffer (after fixup).
 * hdr: parsed MFT record header.
 * out: filled with parsed reparse data.
 * Returns NTFS_OK on success, NTFS_ERR_NOT_FOUND if no reparse point. */
int ntfs_decode_reparse(const uint8_t *record,
                         const struct ntfs_mft_header *hdr,
                         struct ntfs_reparse_data *out);

/* Check if an MFT record has a reparse point attribute.
 * Returns 1 if present, 0 if not. */
int ntfs_is_reparse_point(const uint8_t *record,
                           const struct ntfs_mft_header *hdr);

/* ---- Cluster Allocator (§12.1) ---- */

/* Load $Bitmap (inode 6) data runs into vol->bitmap_runs.
 * Must be called during mount, after MFT is accessible.
 * Also computes total_clusters, free_clusters, and MFT zone. */
int ntfs_bitmap_load(struct ntfs_volume *vol);

/* Allocate `count` contiguous clusters near `hint_lcn`.
 * Searches for a free run, skipping the MFT zone unless
 * no other space is available (last resort).
 * Returns starting LCN on success, or 0 on failure (disk full). */
uint64_t ntfs_alloc_clusters(struct ntfs_volume *vol,
                              uint64_t count, uint64_t hint_lcn);

/* Free `count` clusters starting at `lcn`.
 * Clears their bits in $Bitmap and writes back to disk.
 * Returns NTFS_OK on success. */
int ntfs_free_clusters(struct ntfs_volume *vol,
                        uint64_t lcn, uint64_t count);

/* Count free clusters in the $Bitmap.
 * Performs a full scan of the bitmap -- O(total_clusters/8). */
uint64_t ntfs_get_free_space(struct ntfs_volume *vol);

/* ---- MFT Record Allocator (§12.3) ---- */

/* Load $MFT's own $BITMAP and $DATA runs into vol->mft_bitmap_runs
 * and vol->mft_data_runs. Must be called during mount, after MFT is
 * accessible. This is NOT inode 6 ($Bitmap) -- it's $MFT's internal
 * bitmap attribute that tracks which MFT records are allocated. */
int ntfs_mft_alloc_load(struct ntfs_volume *vol);

/* Allocate a new MFT record.
 * Scans $MFT's $BITMAP for the first free inode slot, sets the bit,
 * initializes the record header (magic, USA, flags, $END marker),
 * applies USA regeneration, and writes the record to disk.
 * is_directory: set to 1 for directory records (flags = 0x03).
 * Returns the new inode number (>= 16), or 0 on failure. */
uint64_t ntfs_alloc_mft_record(struct ntfs_volume *vol, int is_directory);

/* Free an MFT record.
 * Clears the in-use flag but does NOT zero the record (preserves
 * data for deleted file recovery). Increments sequence number for
 * stale reference detection. Clears the MFT bitmap bit.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_free_mft_record(struct ntfs_volume *vol, uint64_t inode);

/* ---- Attribute Writer (§12.4) ---- */

/* Encode an array of data runs into the on-disk byte format.
 * Reverse of ntfs_decode_data_runs() in ntfs_runlist.c:
 *   header = (off_size << 4) | len_size
 *   length = unsigned LE (len_size bytes)
 *   offset = signed LE (off_size bytes), relative to previous LCN
 * Terminates with 0x00 byte.
 * Returns bytes written on success, -1 on error. */
int ntfs_encode_data_runs(const struct ntfs_data_run *runs, int count,
                          uint8_t *buffer, int buf_size);

/* Add a new attribute to an MFT record.
 * Inserts at the correct sorted position (attributes sorted by type ID).
 * Creates resident if data fits, non-resident if too large (allocates
 * clusters via ntfs_alloc_clusters and encodes data runs).
 * hdr is updated on return with new used_size.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_attr_add(struct ntfs_volume *vol, uint8_t *rec,
                  struct ntfs_mft_header *hdr, uint32_t frs_size,
                  uint32_t type, const char *name,
                  const void *data, uint32_t data_len);

/* Update an existing attribute's content.
 * Resident: updates in-place, adjusting header lengths.
 * Non-resident: writes to existing clusters, extends if needed.
 * Handles resident → non-resident conversion when data outgrows record.
 * Returns NTFS_OK, NTFS_ERR_NOT_FOUND, or NTFS_ERR_IO. */
int ntfs_attr_update(struct ntfs_volume *vol, uint8_t *rec,
                     struct ntfs_mft_header *hdr, uint32_t frs_size,
                     uint32_t type, const char *name,
                     const void *data, uint32_t data_len);

/* Remove an attribute from an MFT record.
 * Frees allocated clusters for non-resident attributes.
 * Shifts subsequent attributes left to close the gap.
 * Returns NTFS_OK or NTFS_ERR_NOT_FOUND. */
int ntfs_attr_remove(struct ntfs_volume *vol, uint8_t *rec,
                     struct ntfs_mft_header *hdr, uint32_t frs_size,
                     uint32_t type, const char *name);

/* ---- File Data Reader (§4.2) ---- */

/* Read file data from non-resident runs.
 * vol: NTFS volume context (for cluster_size, sector_size, dev).
 * runs: decoded run array from ntfs_decode_data_runs().
 * run_count: number of runs in the array.
 * real_size: file's actual data size (from non-resident header).
 * file_offset: byte offset within the file to start reading.
 * length: number of bytes to read.
 * buffer: output buffer (must be at least 'length' bytes).
 * Returns bytes actually read, or -1 on error. */
int64_t ntfs_read_data(struct ntfs_volume *vol,
                       const struct ntfs_data_run *runs, int run_count,
                       uint64_t real_size,
                       uint64_t file_offset, uint64_t length,
                       void *buffer);

/* Read data from a resident attribute (inline content).
 * attr: raw attribute pointer (from iterator).
 * offset: byte offset within the attribute content.
 * length: number of bytes to read.
 * buffer: output buffer.
 * Returns bytes actually read, or -1 on error. */
int64_t ntfs_read_resident_data(const uint8_t *attr,
                                uint64_t offset, uint64_t length,
                                void *buffer);

/* Auto-detecting file data reader.
 * Finds the unnamed $DATA attribute in the record, determines if it is
 * resident or non-resident, and reads data accordingly.
 * record: raw MFT record buffer (after fixup).
 * hdr: parsed MFT record header.
 * vol: NTFS volume context.
 * file_offset: byte offset within the file.
 * length: number of bytes to read.
 * buffer: output buffer.
 * Returns bytes actually read, or -1 on error. */
int64_t ntfs_read_file_data(const uint8_t *record,
                            const struct ntfs_mft_header *hdr,
                            struct ntfs_volume *vol,
                            uint64_t file_offset, uint64_t length,
                            void *buffer);

/* ---- LZNT1 Decompression (§9.1) ---- */

/* Decompress LZNT1-compressed data.
 * LZNT1 is the compression algorithm used by NTFS transparent compression.
 * Processes 4096-byte sub-blocks, each with a 2-byte header (bit 15 =
 * compressed flag, bits 0-11 = data size - 1). Compressed sub-blocks
 * contain literal bytes and back-references with variable-length
 * displacement encoding.
 * Returns decompressed bytes on success, -1 on error. */
int ntfs_lznt1_decompress(const uint8_t *src, uint32_t src_len,
                          uint8_t *dst, uint32_t dst_len);

/* Read data from a compressed non-resident attribute.
 * Processes compression units (2^compression_unit_shift clusters each):
 *   - run_length == unit_size: stored uncompressed
 *   - run_length <  unit_size: LZNT1-compressed, decompress on-the-fly
 *   - run is sparse (LCN == -1): fill with zeros
 * Returns bytes read on success, -1 on error. */
int64_t ntfs_read_compressed_data(struct ntfs_volume *vol,
                                  const struct ntfs_data_run *runs,
                                  int run_count,
                                  uint64_t real_size,
                                  uint16_t compression_unit_shift,
                                  uint64_t file_offset,
                                  uint64_t length,
                                  void *buffer);

/* ---- LZNT1 Compression (§9.2) ---- */

/* Compress data using LZNT1.
 * Produces a byte stream compatible with ntfs_lznt1_decompress().
 * src:     input buffer.
 * src_len: number of bytes to compress.
 * dst:     output buffer. Must be at least src_len + 2 * ceil(src_len/4096)
 *          to guarantee space for the worst case (all uncompressed sub-blocks
 *          plus 2 bytes null terminator).
 * dst_len: capacity of dst.
 * Returns the total compressed stream size (including per-block headers and
 * the 2-byte null terminator), or -1 if dst_len is too small. */
int ntfs_lznt1_compress(const uint8_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_len);

/* Write data to a compressed NTFS file (CU-aware, journaled).
 * vol:    NTFS volume context.
 * inode:  MFT inode of the file to write.
 * offset: byte offset within the file to start writing.
 * length: number of bytes to write.
 * buffer: source data.
 *
 * Internally compresses each touched compression unit via ntfs_lznt1_compress(),
 * allocates the minimum number of clusters needed, writes compressed data,
 * updates the data runs in $DATA, and journals the changes via $LogFile.
 * Partial-CU writes read-decompress-modify-recompress the existing unit first.
 * All-zero CUs are stored as sparse runs (no disk allocation).
 *
 * Requires: $DATA non_resident == 1 AND compression_unit != 0.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_write_compressed_data(struct ntfs_volume *vol, uint64_t inode,
                                uint64_t offset, uint64_t length,
                                const void *buffer);

/* ---- $INDEX_ROOT Parser (§5.1, attribute type 0x90) ---- */

/* Index entry flags */
#define NTFS_INDEX_ENTRY_SUBNODE  0x01  /* Entry has sub-node (child VCN) */
#define NTFS_INDEX_ENTRY_LAST    0x02  /* Last entry (sentinel, no filename) */

/* Parsed index root header (from $INDEX_ROOT attribute content) */
struct ntfs_index_root_header {
    uint32_t indexed_attr_type;   /* 0x00: Type of attr being indexed (0x30) */
    uint32_t collation_rule;      /* 0x04: Collation rule (0x01 = filename) */
    uint32_t index_record_size;   /* 0x08: Size of INDX records (typ. 4096) */
    uint8_t  clusters_per_index;  /* 0x0C: Clusters per index record */
};

/* Parsed index node header (follows the index root header) */
struct ntfs_index_node_header {
    uint32_t entries_offset;    /* Offset to first entry (from node start) */
    uint32_t total_size;        /* Total size of entries area */
    uint32_t alloc_size;        /* Allocated size of entries area */
    uint8_t  flags;             /* 0x01 = has children (not a leaf) */
};

/* A single parsed index entry */
struct ntfs_index_entry {
    /* From entry header */
    uint64_t mft_reference;     /* MFT ref (low 48 = inode, high 16 = seq) */
    uint64_t mft_inode;         /* Extracted: low 48 bits */
    uint16_t mft_seq;           /* Extracted: high 16 bits */
    uint16_t entry_length;      /* Total length of this entry */
    uint16_t stream_length;     /* Length of $FILE_NAME payload */
    uint8_t  flags;             /* NTFS_INDEX_ENTRY_* flags */

    /* Decoded $FILE_NAME from payload (valid if !LAST) */
    struct ntfs_file_name fn;

    /* Child INDX VCN (valid if flags & SUBNODE) */
    uint64_t child_vcn;

    /* Raw pointer for advanced use */
    const uint8_t *raw;
};

/* Parse $INDEX_ROOT from a directory's MFT record.
 * root_hdr: filled with index root header fields.
 * node_hdr: filled with index node header fields.
 * entries_base: set to pointer where index entries start.
 * Returns NTFS_OK, or NTFS_ERR_BAD_MAGIC if not found/invalid. */
int ntfs_parse_index_root(const uint8_t *record,
                          const struct ntfs_mft_header *hdr,
                          struct ntfs_index_root_header *root_hdr,
                          struct ntfs_index_node_header *node_hdr,
                          const uint8_t **entries_base);

/* Get the first index entry from a parsed index node.
 * entries_base: from ntfs_parse_index_root().
 * node_hdr: parsed node header.
 * out: filled with parsed entry data.
 * Returns pointer to entry, or NULL if empty. */
const uint8_t *ntfs_index_entry_first(const uint8_t *entries_base,
                                      const struct ntfs_index_node_header *nh,
                                      struct ntfs_index_entry *out);

/* Advance to the next index entry.
 * Returns pointer to next entry, or NULL at end. */
const uint8_t *ntfs_index_entry_next(const uint8_t *entry,
                                     const uint8_t *entries_base,
                                     const struct ntfs_index_node_header *nh,
                                     struct ntfs_index_entry *out);

/* ---- INDX Buffer Reader (§5.2, attribute type 0xA0) ---- */

/* INDX magic: "INDX" in little-endian */
#define NTFS_INDX_MAGIC  0x58444E49U

/* Read an INDX buffer from disk at the specified VCN.
 * vol: NTFS volume context.
 * index_runs: decoded data runs from $INDEX_ALLOCATION.
 * index_run_count: number of runs.
 * vcn: Virtual Cluster Number of the INDX record to read.
 * index_record_size: size of one INDX record (from $INDEX_ROOT header).
 * buffer: output buffer (must be >= index_record_size bytes).
 * Returns NTFS_OK on success, error code on failure. */
int ntfs_read_indx(struct ntfs_volume *vol,
                   const struct ntfs_data_run *index_runs,
                   int index_run_count,
                   uint64_t vcn, uint32_t index_record_size,
                   uint8_t *buffer);

/* Parse a validated INDX buffer to extract node header and entries base.
 * buffer: INDX record (already read + fixup applied by ntfs_read_indx).
 * node_hdr: filled with parsed node header.
 * entries_base: set to pointer where index entries start.
 * Returns NTFS_OK on success. */
int ntfs_parse_indx_entries(const uint8_t *buffer,
                            struct ntfs_index_node_header *node_hdr,
                            const uint8_t **entries_base);

/* ---- Directory Lookup (§5.3) ---- */

/* NTFS root directory inode */
#define NTFS_ROOT_INODE  5

/* Search a directory's B+ tree for a single filename component.
 * dir_inode: MFT inode of the parent directory.
 * name: ASCII filename to search for (case-insensitive).
 * out_inode: filled with the matching file's MFT inode on success.
 * Returns NTFS_OK on match, NTFS_ERR_NOT_FOUND if absent, or error. */
int ntfs_lookup(struct ntfs_volume *vol, uint64_t dir_inode,
               const char *name, uint64_t *out_inode);

/* Resolve a full backslash-separated path to an MFT inode number.
 * path: absolute path like "\Impossible\Fonts\Inter.ttf".
 *        Leading backslash is optional.  Forward slashes also accepted.
 * out_inode: filled with the final component's MFT inode.
 * Returns NTFS_OK on success, NTFS_ERR_NOT_FOUND if any component missing. */
int ntfs_resolve_path(struct ntfs_volume *vol, const char *path,
                      uint64_t *out_inode);

/* ---- VFS Integration (§6.1) ---- */

struct vfs_fs_driver;
struct vfs_node;

/* Get the singleton VFS driver descriptor for NTFS (read-only). */
struct vfs_fs_driver *ntfs_get_driver(void);

/* Get the root VFS node for a mounted NTFS volume. */
struct vfs_node *ntfs_get_root(struct ntfs_volume *vol);

/* Run NTFS self-test suite (§8.1).
 * Only executes if vol->volume_name is "NTFS_TEST".
 * Logs pass/fail results to serial via klog. */
void ntfs_run_self_test(struct ntfs_volume *vol, struct vfs_node *root);

/* Enumerate a directory entry by index (for VFS readdir).
 * dir_inode: MFT inode of the directory.
 * index: 0-based index of entry to retrieve.
 * out_name: filled with the filename (ASCII, null-terminated).
 * out_name_max: capacity of out_name buffer.
 * out_inode: filled with the entry's MFT inode.
 * out_is_dir: set to 1 if entry is a directory.
 * out_size: set to file size (0 for directories).
 * Returns NTFS_OK on success, NTFS_ERR_NOT_FOUND if index >= entry count. */
int ntfs_readdir_entry(struct ntfs_volume *vol, uint64_t dir_inode,
                       uint32_t index, char *out_name, int out_name_max,
                       uint64_t *out_inode, int *out_is_dir,
                       uint64_t *out_size);

/* ---- Directory Enumeration (§5.4) ---- */

/* Directory entry passed to readdir callback */
struct ntfs_dir_entry {
    char     name[NTFS_MAX_NAME + 1];  /* Filename (ASCII, null-terminated) */
    uint64_t inode;                     /* MFT inode number */
    uint64_t file_size;                 /* Real file size (0 for dirs) */
    uint64_t creation_time;             /* FILETIME: 100-ns since 1601 */
    uint64_t modification_time;
    uint64_t access_time;
    uint32_t flags;                     /* $FILE_NAME flags */
    uint8_t  name_space;                /* 0=POSIX, 1=Win32, 2=DOS, 3=Both */
    uint8_t  is_directory;              /* 1 if directory */
};

/* Callback for ntfs_readdir().  Return 0 to continue, non-zero to stop. */
typedef int (*ntfs_readdir_cb)(const struct ntfs_dir_entry *entry,
                                void *user_data);

/* Enumerate all entries in an NTFS directory.
 * Walks $INDEX_ROOT then all active INDX buffers (via $BITMAP).
 * Skips DOS-only names (namespace 0x02) and sentinel entries.
 * callback: invoked for each visible entry.
 * user_data: opaque pointer passed to callback.
 * Returns NTFS_OK, or error code. */
int ntfs_readdir(struct ntfs_volume *vol, uint64_t dir_inode,
                 ntfs_readdir_cb callback, void *user_data);

/* ---- File Lifecycle Operations (§12.5) ---- */

/* Write an in-memory MFT record back to disk.
 * Applies USA regeneration (§12.2) before writing.
 * Invalidates the MFT cache entry for this inode.
 * rec must be in "after fixup removal" state (editable).
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_write_mft_record(struct ntfs_volume *vol, uint64_t inode,
                          uint8_t *rec);

/* Map an MFT inode number to the disk LBA of its record.
 * Uses $MFT's $DATA runs (handles fragmented MFTs).
 * Returns 0 on success, -1 if inode is beyond MFT extent. */
int ntfs_mft_inode_to_lba(struct ntfs_volume *vol, uint64_t inode,
                          uint64_t *out_lba);

/* Insert a directory entry into a directory's $INDEX_ROOT.
 * child_inode: MFT inode of the file/dir being added.
 * child_seq: sequence number of the child MFT record.
 * fn_data: raw $FILE_NAME attribute content (0x42 + name_len*2 bytes).
 * fn_data_len: length of fn_data.
 * Handles full B+ tree operations: root splits, INDX buffer splits,
 * recursive promotion. All modifications journaled (§13) and USA-regenerated.
 * Returns NTFS_OK on success, NTFS_ERR_FULL if disk full. */
int ntfs_dir_insert_entry(struct ntfs_volume *vol, uint64_t dir_inode,
                          uint64_t child_inode, uint16_t child_seq,
                          const uint8_t *fn_data, uint32_t fn_data_len);

/* Remove a directory entry by filename from a directory's B+ tree index.
 * Searches both $INDEX_ROOT and INDX buffers.
 * Handles node merging, entry redistribution, and empty buffer cleanup.
 * All modifications journaled (§13) and USA-regenerated.
 * Returns NTFS_OK on success, NTFS_ERR_NOT_FOUND if name absent. */
int ntfs_dir_remove_entry(struct ntfs_volume *vol, uint64_t dir_inode,
                          const char *name);

/* ---- B+ Tree Index Mutation (§14.1) ---- */

/* Full B+ tree insert with INDX buffer split support.
 * This is the implementation behind ntfs_dir_insert_entry().
 * Handles root overflow → INDX allocation, INDX overflow → split + promote,
 * recursive splits, $BITMAP updates, USA regeneration, and journaling.
 * Returns NTFS_OK, NTFS_ERR_FULL, or NTFS_ERR_IO. */
int ntfs_index_insert(struct ntfs_volume *vol, uint64_t dir_inode,
                      uint64_t child_inode, uint16_t child_seq,
                      const uint8_t *fn_data, uint32_t fn_data_len);

/* Full B+ tree delete with INDX buffer merge support.
 * This is the implementation behind ntfs_dir_remove_entry().
 * Handles leaf/internal delete, node underflow → redistribute/merge,
 * empty buffer cleanup, $BITMAP updates, USA regeneration, and journaling.
 * Returns NTFS_OK, NTFS_ERR_NOT_FOUND, or NTFS_ERR_IO. */
int ntfs_index_delete(struct ntfs_volume *vol, uint64_t dir_inode,
                      const char *name);

/* Write an INDX buffer to disk at the specified VCN.
 * Counterpart to ntfs_read_indx(). Applies USA regeneration before writing.
 * Returns NTFS_OK on success, NTFS_ERR_IO on failure. */
int ntfs_write_indx(struct ntfs_volume *vol,
                    const struct ntfs_data_run *index_runs,
                    int index_run_count,
                    uint64_t vcn, uint32_t index_record_size,
                    uint8_t *buffer);

/* ---- File Write Engine (§16.1) ---- */

/* Write bytes to a file's unnamed $DATA attribute.
 * For resident files: updates content in-place, converting to non-resident
 * if the data grows beyond the MFT record capacity (via ntfs_attr_update).
 * For non-resident files: maps offset to clusters, writes data, extends
 * allocation if offset+length exceeds current alloc_size.
 * All modifications are journaled (§13) and USA-regenerated before writing.
 * Returns NTFS_OK, NTFS_ERR_FULL (disk full), or NTFS_ERR_*. */
int ntfs_write_data(struct ntfs_volume *vol, uint64_t inode,
                    uint64_t offset, uint64_t length, const void *buffer);

/* Truncate or extend a file to exactly new_size bytes.
 * Shrink: frees tail clusters, shortens the last run, re-encodes runs.
 * Grow: allocates additional clusters, extends or appends runs.
 * Truncate to 0: frees all clusters and converts $DATA back to resident.
 * Journaled and USA-regenerated before writing.
 * Returns NTFS_OK, NTFS_ERR_FULL, or NTFS_ERR_*. */
int ntfs_truncate(struct ntfs_volume *vol, uint64_t inode, uint64_t new_size);

/* Update the DOS file attribute flags stored in $STANDARD_INFORMATION.
 * attrs: bitmask of NTFS_FILE_ATTR_* flags.
 * Journaled (UPDATE_RESIDENT) and USA-regenerated before writing.
 * Returns NTFS_OK or NTFS_ERR_*. */
int ntfs_set_file_attributes(struct ntfs_volume *vol, uint64_t inode,
                              uint32_t attrs);

/* Set file timestamps from Unix time (seconds since 1970-01-01).
 * Converts Unix → FILETIME (100-ns since 1601-01-01) and
 * writes creation, modification, and access times into $STANDARD_INFORMATION.
 * mft_change_time is set equal to modify_unix.
 * Journaled (UPDATE_RESIDENT) and USA-regenerated before writing.
 * Returns NTFS_OK or NTFS_ERR_*. */
int ntfs_set_file_time(struct ntfs_volume *vol, uint64_t inode,
                       uint64_t create_unix, uint64_t modify_unix,
                       uint64_t access_unix);

/* Create a new file in an NTFS directory.
 * Allocates MFT record, adds $STANDARD_INFORMATION + $FILE_NAME + $DATA,
 * inserts directory entry into parent, updates parent timestamps.
 * attrs: DOS file attribute flags (NTFS_FILE_ATTR_*).
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_create_file(struct ntfs_volume *vol, uint64_t parent_inode,
                     const char *name, uint32_t attrs);

/* Create a new directory in an NTFS directory.
 * Like ntfs_create_file but sets directory flag and adds empty $INDEX_ROOT.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_create_directory(struct ntfs_volume *vol, uint64_t parent_inode,
                          const char *name);

/* Delete a file from an NTFS directory.
 * Removes directory entry, frees data clusters and MFT record if last link.
 * Returns NTFS_OK on success, NTFS_ERR_* on failure. */
int ntfs_delete_file(struct ntfs_volume *vol, uint64_t parent_inode,
                     const char *name);

/* Rename/move a file between NTFS directories.
 * Removes from old parent, updates $FILE_NAME, inserts into new parent.
 * Returns NTFS_OK, NTFS_ERR_NOT_FOUND, or NTFS_ERR_IO if target exists. */
int ntfs_rename_file(struct ntfs_volume *vol,
                     uint64_t old_parent, const char *old_name,
                     uint64_t new_parent, const char *new_name);

/* ---- $LogFile Log Operation Codes (§13.1) ---- */

#define NTFS_LOG_OP_NOOP                 0x00
#define NTFS_LOG_OP_COMPENSATION         0x01
#define NTFS_LOG_OP_INIT_FRS             0x02  /* InitializeFileRecordSegment */
#define NTFS_LOG_OP_DEALLOC_FRS          0x03  /* DeallocateFileRecordSegment */
#define NTFS_LOG_OP_WRITE_END_FRS        0x04  /* WriteEndOfFileRecordSegment */
#define NTFS_LOG_OP_CREATE_ATTR          0x05  /* CreateAttribute */
#define NTFS_LOG_OP_DELETE_ATTR          0x06  /* DeleteAttribute */
#define NTFS_LOG_OP_UPDATE_RESIDENT      0x07  /* UpdateResidentValue */
#define NTFS_LOG_OP_UPDATE_NONRES        0x08  /* UpdateNonResidentValue */
#define NTFS_LOG_OP_UPDATE_MAPPING       0x09  /* UpdateMappingPairs */
#define NTFS_LOG_OP_DELETE_DIRTY_CLUS    0x0A  /* DeleteDirtyClusters */
#define NTFS_LOG_OP_SET_ATTR_SIZES       0x0B  /* SetNewAttributeSizes */
#define NTFS_LOG_OP_ADD_IDX_ROOT         0x0C  /* AddIndexEntryRoot */
#define NTFS_LOG_OP_DEL_IDX_ROOT         0x0D  /* DeleteIndexEntryRoot */
#define NTFS_LOG_OP_ADD_IDX_ALLOC        0x0E  /* AddIndexEntryAllocation */
#define NTFS_LOG_OP_DEL_IDX_ALLOC        0x0F  /* DeleteIndexEntryAllocation */
#define NTFS_LOG_OP_SET_IDX_VCN          0x10  /* SetIndexEntryVcnAllocation */
#define NTFS_LOG_OP_UPDATE_FN_ROOT       0x11  /* UpdateFileNameRoot */
#define NTFS_LOG_OP_UPDATE_FN_ALLOC      0x12  /* UpdateFileNameAllocation */
#define NTFS_LOG_OP_SET_BITS_BITMAP      0x13  /* SetBitsInNonResidentBitMap */
#define NTFS_LOG_OP_CLEAR_BITS_BITMAP    0x14  /* ClearBitsInNonResidentBitMap */
#define NTFS_LOG_OP_DIRTY_PAGE_DUMP      0x19  /* DirtyPageTableDump */
#define NTFS_LOG_OP_TXN_TABLE_DUMP       0x1A  /* TransactionTableDump */
#define NTFS_LOG_OP_ATTR_NAMES_DUMP      0x1B  /* AttributeNamesDump */

/* ---- Transaction Context (§13.1) ---- */

struct ntfs_txn {
    struct ntfs_volume *vol;     /* Volume this transaction belongs to */
    uint64_t start_lsn;         /* LSN at transaction start */
    uint64_t last_lsn;          /* LSN of last logged record */
    uint32_t txn_id;            /* Transaction ID */
    uint32_t record_count;      /* Number of logged records */
    uint8_t  committed;         /* 1 if committed */
    uint8_t  aborted;           /* 1 if aborted */
};

/* ---- Journal API (§13.1) ---- */

/* Initialize the journal engine from $LogFile (inode 2).
 * Parses restart area, caches CurrentLsn and write position.
 * Called during NTFS volume mount. */
int ntfs_journal_init(struct ntfs_volume *vol);

/* Begin a new transaction.
 * Returns a transaction context, or NULL on failure. */
struct ntfs_txn *ntfs_txn_begin(struct ntfs_volume *vol);

/* Log a redo/undo record pair to $LogFile.
 * Must be called BEFORE the actual metadata write (WAL protocol).
 * Returns the new LSN, or 0 on failure. */
uint64_t ntfs_txn_log(struct ntfs_txn *txn,
                      uint16_t redo_op, const void *redo_data,
                      uint16_t redo_len,
                      uint16_t undo_op, const void *undo_data,
                      uint16_t undo_len,
                      uint64_t target_mft, uint16_t target_attr_off);

/* Commit a transaction.
 * Writes commit record, updates restart area CurrentLsn.
 * After commit, metadata writes are safe to persist. */
int ntfs_txn_commit(struct ntfs_txn *txn);

/* Abort a transaction (rollback).
 * Writes a CompensationLogRecord and marks the txn as aborted.
 * Actual undo application requires the recovery engine (§13.2). */
int ntfs_txn_abort(struct ntfs_txn *txn);

/* Free a transaction context (must be committed or aborted first). */
void ntfs_txn_free(struct ntfs_txn *txn);

/* Clean shutdown of the journal.
 * Updates restart area with final LSN. Called during unmount. */
void ntfs_journal_shutdown(struct ntfs_volume *vol);

/* ---- Recovery Replay (§13.2) ---- */

/* Replay the $LogFile to restore consistency on a dirty mount.
 * Three-phase ARIES recovery:
 *   1. Analysis -- scan log, build transaction table
 *   2. Redo -- replay committed operations
 *   3. Undo -- roll back uncommitted operations
 * Clears the dirty flag and resets $LogFile after recovery. */
int ntfs_recovery_replay(struct ntfs_volume *vol);

/* ---- EFS -- Encrypting File System (§9.3) ---- */

/* EFS uses the $LOGGED_UTILITY_STREAM attribute (type 0x100) named "$EFS".
 * The attribute contains Data Decryption Fields (DDFs), each holding an
 * RSA-wrapped copy of the File Encryption Key (FEK) for one authorized user.
 * File data in $DATA is encrypted with AES-256 using the FEK.
 *
 * NTFS mutual exclusion: a file CANNOT be both compressed AND encrypted.
 * If NTFS_ATTR_FLAG_ENCRYPTED is set, compression_unit must be 0. */

/* Maximum DDF entries we parse per $EFS attribute */
#define NTFS_EFS_MAX_DDF  8

/* FEK cache size (per volume) */
#define NTFS_EFS_FEK_CACHE_SIZE  16

/* FEK length in bytes (AES-256 key) */
#define NTFS_EFS_FEK_LEN  32

/* Certificate thumbprint length (SHA-1 = 20 bytes) */
#define NTFS_EFS_THUMB_LEN  20

/* Parsed Data Decryption Field (one per authorized user) */
struct ntfs_efs_ddf {
    uint8_t  cert_thumbprint[NTFS_EFS_THUMB_LEN]; /* User cert SHA-1 */
    uint32_t encrypted_fek_offset;  /* Offset to RSA-wrapped FEK blob */
    uint32_t encrypted_fek_length;  /* Length of RSA-wrapped FEK blob */
    uint32_t sid_offset;            /* Offset to user SID */
    uint32_t sid_length;            /* Length of user SID */
};

/* Parsed EFS attribute header */
struct ntfs_efs_info {
    uint32_t version;              /* EFS version (2 or 3) */
    uint32_t ddf_count;            /* Number of DDF entries */
    struct ntfs_efs_ddf ddfs[NTFS_EFS_MAX_DDF]; /* Parsed DDF array */
    const uint8_t *raw_data;       /* Pointer to raw $EFS content */
    uint32_t raw_length;           /* Length of raw $EFS content */
};

/* FEK cache entry */
struct ntfs_efs_fek_entry {
    uint64_t inode;                /* MFT inode this FEK belongs to */
    uint8_t  fek[NTFS_EFS_FEK_LEN]; /* Plaintext AES-256 FEK */
    uint8_t  valid;                /* 1 if entry is populated */
    uint64_t last_access;          /* Simple counter for LRU eviction */
};

/* Parse the $EFS attribute from an MFT record.
 * Locates the $LOGGED_UTILITY_STREAM (type 0x100) named "$EFS",
 * decodes the EFS header and DDF entries.
 * record: raw MFT record buffer (after fixup).
 * hdr: parsed MFT record header.
 * out: filled with parsed EFS info.
 * Returns NTFS_OK if $EFS found and parsed, NTFS_ERR_NOT_FOUND if absent. */
int ntfs_efs_parse(const uint8_t *record, const struct ntfs_mft_header *hdr,
                   struct ntfs_efs_info *out);

/* Find the DDF entry matching a certificate thumbprint.
 * info: parsed EFS info from ntfs_efs_parse().
 * thumbprint: 20-byte SHA-1 certificate thumbprint to match.
 * out_ddf: filled with the matching DDF entry.
 * Returns NTFS_OK on match, NTFS_ERR_ACCESS_DENIED if no match. */
int ntfs_efs_find_ddf(const struct ntfs_efs_info *info,
                      const uint8_t *thumbprint,
                      struct ntfs_efs_ddf *out_ddf);

/* Unwrap the FEK using CNG RSA private key.
 * ddf: DDF entry containing the RSA-wrapped FEK blob.
 * efs_raw: raw $EFS attribute content (for blob access).
 * fek_out: 32-byte buffer for the plaintext AES-256 FEK.
 * Returns NTFS_OK on success, NTFS_ERR_NOT_READY if CNG not initialized. */
int ntfs_efs_unwrap_fek(const struct ntfs_efs_ddf *ddf,
                        const uint8_t *efs_raw,
                        uint8_t *fek_out);

/* Decrypt file data in-place using the FEK.
 * fek: 32-byte AES-256 key.
 * buf: buffer to decrypt (modified in-place).
 * len: number of bytes to decrypt.
 * Returns NTFS_OK on success, NTFS_ERR_NOT_READY if CNG not initialized. */
int ntfs_efs_decrypt_data(const uint8_t *fek, void *buf, uint64_t len);

/* Encrypt file data in-place using the FEK.
 * fek: 32-byte AES-256 key.
 * buf: buffer to encrypt (modified in-place).
 * len: number of bytes to encrypt.
 * Returns NTFS_OK on success, NTFS_ERR_NOT_READY if CNG not initialized. */
int ntfs_efs_encrypt_data(const uint8_t *fek, void *buf, uint64_t len);

/* Cache operations for FEK per-inode caching. */
int ntfs_efs_cache_lookup(struct ntfs_volume *vol, uint64_t inode,
                          uint8_t *fek_out);
void ntfs_efs_cache_store(struct ntfs_volume *vol, uint64_t inode,
                          const uint8_t *fek);
void ntfs_efs_cache_evict(struct ntfs_volume *vol, uint64_t inode);

/* Add an authorized user to a file's $EFS DDF list.
 * inode: MFT inode of the encrypted file.
 * cert_thumbprint: 20-byte SHA-1 of the user's certificate.
 * Returns NTFS_OK, NTFS_ERR_NOT_FOUND, or NTFS_ERR_NOT_READY. */
int ntfs_efs_add_user(struct ntfs_volume *vol, uint64_t inode,
                      const uint8_t *cert_thumbprint);

/* Read data from an encrypted non-resident attribute.
 * Reads raw data from disk, then decrypts via FEK.
 * Returns bytes read on success, -1 on error. */
int64_t ntfs_read_encrypted_data(struct ntfs_volume *vol,
                                  const uint8_t *record,
                                  const struct ntfs_mft_header *hdr,
                                  const struct ntfs_data_run *runs,
                                  int run_count,
                                  uint64_t real_size,
                                  uint64_t file_offset,
                                  uint64_t length,
                                  void *buffer);

/* Write data to an encrypted file.
 * Encrypts data with FEK before writing to disk.
 * Returns NTFS_OK, NTFS_ERR_NOT_READY, or NTFS_ERR_*. */
int ntfs_write_encrypted_data(struct ntfs_volume *vol, uint64_t inode,
                               uint64_t offset, uint64_t length,
                               const void *buffer);
