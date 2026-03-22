/* ============================================================================
 * mbr.h — MBR Partition Table Parser
 *
 * Parses the Master Boot Record (sector 0) to discover partitions.
 * Decodes the complete 512-byte MBR layout:
 *   - Bootstrap code (440 bytes, not interpreted)
 *   - 32-bit Unique Disk Signature (offsets 440–443)
 *   - Reserved field (offsets 444–445)
 *   - 4 × 16-byte partition entries (offsets 446–509)
 *   - Boot Record Signature 0x55AA (offsets 510–511)
 *
 * Each partition entry contains:
 *   - Boot Indicator (0x80 active, 0x00 inactive)
 *   - CHS start (3 bytes, bit-packed)
 *   - Partition Type ID
 *   - CHS end (3 bytes, bit-packed)
 *   - Starting LBA (4 bytes LE)
 *   - Total Sectors (4 bytes LE)
 *
 * Recognized partition types:
 *   0x0C — FAT32 with LBA          0x0B — FAT32 with CHS
 *   0x83 — Linux (ext2/ext4)       0xDA — IXFS (Impossible OS)
 *   0xEE — GPT Protective MBR      0x05 — Extended (CHS)
 *   0x0F — Extended (LBA)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* MBR layout constants */
#define MBR_SIGNATURE        0xAA55
#define MBR_ENTRY_OFFSET     446
#define MBR_MAX_PARTITIONS   4
#define MBR_ENTRY_SIZE       16
#define MBR_DISK_SIG_OFFSET  440
#define MBR_RESERVED_OFFSET  444
#define MBR_SIG_OFFSET       510

/* Known partition type IDs */
#define MBR_TYPE_EMPTY       0x00
#define MBR_TYPE_EXT_CHS     0x05
#define MBR_TYPE_FAT32_CHS   0x0B
#define MBR_TYPE_FAT32_LBA   0x0C
#define MBR_TYPE_EXT_LBA     0x0F
#define MBR_TYPE_LINUX       0x83
#define MBR_TYPE_IXFS        0xDA
#define MBR_TYPE_GPT         0xEE

/* CHS address (decoded from 3-byte bit-packed MBR format).
 * The 10-bit cylinder is split across two bytes:
 *   byte[0] = head, byte[1] low 6 bits = sector,
 *   byte[1] high 2 bits = cylinder bits 9–8, byte[2] = cylinder bits 7–0. */
struct mbr_chs {
    uint16_t cylinder;  /* 0–1023 (10 bits) */
    uint8_t  head;      /* 0–254 */
    uint8_t  sector;    /* 1–63  */
};

/* Partition entry (parsed from 16-byte MBR entry) */
struct mbr_entry {
    uint8_t        status;       /* 0x80 = bootable, 0x00 = inactive */
    uint8_t        type;         /* Partition type ID */
    uint32_t       start_lba;    /* Starting LBA */
    uint32_t       sector_count; /* Total sectors */
    struct mbr_chs chs_start;   /* CHS of first sector */
    struct mbr_chs chs_end;     /* CHS of last sector */
    int            chs_overflow; /* 1 if CHS is FE FF FF or FF FF FF */
};

/* Partition scan result (from 512-byte MBR sector) */
struct mbr_table {
    int              valid;          /* 1 if 0xAA55 signature found */
    int              count;          /* Number of non-empty partitions */
    uint32_t         disk_signature; /* 32-bit Unique Disk Signature (LE) */
    uint16_t         reserved;       /* Reserved field (offsets 444–445) */
    int              boot_count;     /* Number of entries with status 0x80 */
    struct mbr_entry parts[MBR_MAX_PARTITIONS];
};

/* ---- API ---- */

/* Parse MBR from a 512-byte sector buffer.
 * Returns filled mbr_table. Check .valid for signature presence. */
struct mbr_table mbr_parse(const void *sector0);

/* Return human-readable name for a partition type ID. */
const char *mbr_type_name(uint8_t type);
