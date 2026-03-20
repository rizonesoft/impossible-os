/* ============================================================================
 * gpt.h — GUID Partition Table (GPT) Parser
 *
 * Parses the GPT header (LBA 1) and partition entry array (LBA 2+) after
 * detecting a protective MBR with type 0xEE at LBA 0.
 *
 * GPT uses 128-byte partition entries identified by GUIDs rather than
 * single-byte type IDs. The header and partition array are both protected
 * by CRC32 checksums.
 *
 * Full type GUID registry (25+ types) — see gpt.c for definitions.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* GPT constants */
#define GPT_SIGNATURE       0x5452415020494645ULL  /* "EFI PART" as uint64 LE */
#define GPT_HEADER_LBA      1
#define GPT_ENTRY_SIZE      128
#define GPT_MAX_PARTITIONS  128
#define GPT_MAX_RESULTS     32   /* Max partitions we report */
#define GPT_NAME_MAX        36   /* UTF-16LE chars in entry name field */

/* 16-byte GUID (stored in mixed-endian per UEFI spec) */
struct gpt_guid {
    uint32_t data1;      /* Little-endian */
    uint16_t data2;      /* Little-endian */
    uint16_t data3;      /* Little-endian */
    uint8_t  data4[8];   /* Big-endian (byte array) */
};

/* On-disk GPT header (92 bytes at LBA 1) */
struct gpt_header {
    uint64_t signature;          /* "EFI PART" = 0x5452415020494645 */
    uint32_t revision;           /* Usually 0x00010000 */
    uint32_t header_size;        /* Size of this header (usually 92) */
    uint32_t header_crc32;       /* CRC32 of header (with this field zeroed) */
    uint32_t reserved;           /* Must be zero */
    uint64_t my_lba;             /* LBA of this header */
    uint64_t alt_lba;            /* LBA of alternate header */
    uint64_t first_usable_lba;   /* First usable LBA for partitions */
    uint64_t last_usable_lba;    /* Last usable LBA for partitions */
    struct gpt_guid disk_guid;   /* Unique disk GUID */
    uint64_t part_entry_lba;     /* Starting LBA of partition entries */
    uint32_t num_part_entries;   /* Number of partition entries */
    uint32_t part_entry_size;    /* Size of each entry (usually 128) */
    uint32_t part_entry_crc32;   /* CRC32 of entire partition array */
};

/* Parsed partition entry (kernel representation) */
struct gpt_entry {
    struct gpt_guid type_guid;   /* Partition type GUID */
    struct gpt_guid unique_guid; /* Unique partition GUID */
    uint64_t start_lba;          /* First LBA */
    uint64_t end_lba;            /* Last LBA (inclusive) */
    uint64_t attributes;         /* Attribute flags */
    char     name[GPT_NAME_MAX + 1]; /* ASCII name (converted from UTF-16LE) */
};

/* Partition scan result */
struct gpt_table {
    int              valid;   /* 1 if GPT sig + CRC verified */
    int              count;   /* Number of non-empty partitions */
    struct gpt_entry parts[GPT_MAX_RESULTS];
};

/* Well-known partition type GUIDs */
extern const struct gpt_guid GPT_GUID_EMPTY;
extern const struct gpt_guid GPT_GUID_EFI_SYSTEM;
extern const struct gpt_guid GPT_GUID_MS_BASIC_DATA;
extern const struct gpt_guid GPT_GUID_LINUX_FS;
extern const struct gpt_guid GPT_GUID_IXFS;
extern const struct gpt_guid GPT_GUID_BIOS_BOOT;
extern const struct gpt_guid GPT_GUID_MS_RESERVED;
extern const struct gpt_guid GPT_GUID_MS_LDM_META;
extern const struct gpt_guid GPT_GUID_MS_LDM_DATA;
extern const struct gpt_guid GPT_GUID_MS_RECOVERY;
extern const struct gpt_guid GPT_GUID_MS_STORAGE_SPACES;
extern const struct gpt_guid GPT_GUID_LINUX_SWAP;
extern const struct gpt_guid GPT_GUID_LINUX_ROOT_X64;
extern const struct gpt_guid GPT_GUID_LINUX_HOME;
extern const struct gpt_guid GPT_GUID_LINUX_SRV;
extern const struct gpt_guid GPT_GUID_LINUX_LVM;
extern const struct gpt_guid GPT_GUID_LINUX_RAID;
extern const struct gpt_guid GPT_GUID_APPLE_HFS;
extern const struct gpt_guid GPT_GUID_APPLE_APFS;
extern const struct gpt_guid GPT_GUID_FREEBSD_ZFS;
extern const struct gpt_guid GPT_GUID_SOLARIS_ROOT;
extern const struct gpt_guid GPT_GUID_VMWARE_VMFS;
extern const struct gpt_guid GPT_GUID_CHROMEOS_KERNEL;
extern const struct gpt_guid GPT_GUID_CEPH_OSD;

/* ---- API ---- */

/* Forward declaration */
struct blkdev;

/* Parse GPT from a block device.  sector0 is the already-read MBR sector
 * (used to verify protective MBR).  Additional sectors are read via blkdev. */
struct gpt_table gpt_parse(const struct blkdev *dev, const void *sector0);

/* Compare two GUIDs. Returns 1 if equal. */
int gpt_guid_equal(const struct gpt_guid *a, const struct gpt_guid *b);

/* Return human-readable name for a known partition type GUID. */
const char *gpt_type_name(const struct gpt_guid *guid);

/* Compute CRC32 (used internally and available for other modules). */
uint32_t gpt_crc32(const void *data, uint32_t len);
