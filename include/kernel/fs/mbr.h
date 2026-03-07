/* ============================================================================
 * mbr.h — MBR Partition Table Parser
 *
 * Parses the Master Boot Record (sector 0) to discover partitions.
 * The MBR contains 4 primary partition entries at byte offset 446,
 * each 16 bytes, followed by the boot signature 0x55AA at offset 510.
 *
 * Recognized partition types:
 *   0x0C — FAT32 with LBA
 *   0x0B — FAT32 with CHS
 *   0x83 — Linux (ext2/ext4)
 *   0xDA — IXFS (Impossible OS custom)
 *   0xEE — GPT protective MBR
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* MBR constants */
#define MBR_SIGNATURE       0xAA55
#define MBR_ENTRY_OFFSET    446
#define MBR_MAX_PARTITIONS  4
#define MBR_ENTRY_SIZE      16

/* Known partition type IDs */
#define MBR_TYPE_EMPTY      0x00
#define MBR_TYPE_FAT32_CHS  0x0B
#define MBR_TYPE_FAT32_LBA  0x0C
#define MBR_TYPE_LINUX      0x83
#define MBR_TYPE_IXFS       0xDA
#define MBR_TYPE_GPT        0xEE

/* Partition entry (parsed) */
struct mbr_entry {
    uint8_t  status;        /* 0x80 = bootable, 0x00 = inactive */
    uint8_t  type;          /* Partition type ID */
    uint32_t start_lba;     /* Starting LBA */
    uint32_t sector_count;  /* Total sectors */
};

/* Partition scan result */
struct mbr_table {
    int              valid;   /* 1 if MBR signature found */
    int              count;   /* Number of non-empty partitions */
    struct mbr_entry parts[MBR_MAX_PARTITIONS];
};

/* ---- API ---- */

/* Parse MBR from a 512-byte sector buffer.
 * Returns filled mbr_table. Check .valid for signature presence. */
struct mbr_table mbr_parse(const void *sector0);

/* Return human-readable name for a partition type ID. */
const char *mbr_type_name(uint8_t type);
