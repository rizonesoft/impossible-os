/* ============================================================================
 * mbr.h -- MBR Partition Table Parser
 *
 * Parses the Master Boot Record (sector 0) to discover partitions.
 * Decodes the complete 512-byte MBR layout:
 *   - Bootstrap code (440 bytes, not interpreted)
 *   - 32-bit Unique Disk Signature (offsets 440–443)
 *   - Reserved field (offsets 444–445)
 *   - 4 × 16-byte partition entries (offsets 446–509)
 *   - Boot Record Signature 0x55AA (offsets 510–511)
 *
 * Recognizes 30+ partition type IDs for robust interoperability.
 * Flags GPT Protective (0xEE), Extended containers (0x05/0x0F/0x85),
 * hidden vendor recovery partitions, and Dynamic Disks (0x42).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* MBR layout constants */
#define MBR_SIGNATURE        0xAA55
#define MBR_ENTRY_OFFSET     446      /* 0x1BE -- industry standard */
#define MBR_MAX_PARTITIONS   4
#define MBR_ENTRY_SIZE       16
#define MBR_DISK_SIG_OFFSET  440
#define MBR_RESERVED_OFFSET  444
#define MBR_SIG_OFFSET       510

/* Bulletproofing: MBR layout constants are industry standards that must not drift.
 *   Offset  Size  Description
 *   ------  ----  -----------
 *   0       440   Bootstrap code
 *   440     4     Disk signature
 *   444     2     Reserved
 *   446     64    4 x 16-byte partition entries
 *   510     2     Boot signature (0x55AA)
 */
_Static_assert(MBR_ENTRY_OFFSET == 446,
    "MBR partition table must start at offset 446 (0x1BE)");
_Static_assert(MBR_SIG_OFFSET == 510,
    "MBR boot signature must be at offset 510");
_Static_assert(MBR_ENTRY_SIZE == 16,
    "MBR partition entry must be 16 bytes");
_Static_assert(MBR_ENTRY_OFFSET + MBR_MAX_PARTITIONS * MBR_ENTRY_SIZE == MBR_SIG_OFFSET,
    "4 partition entries (64 bytes) must fill exactly to boot signature");
#define MBR_MAX_LOGICAL      128  /* Safety cap for EBR chain traversal */

/* ---- Partition Type IDs ----
 * Comprehensive set for filesystem identification and interoperability. */

/* Basic filesystem types */
#define MBR_TYPE_EMPTY       0x00  /* Unallocated / empty slot */
#define MBR_TYPE_FAT12       0x01  /* FAT12 (floppy, small media) */
#define MBR_TYPE_FAT16_SM    0x04  /* FAT16 ≤32 MB */
#define MBR_TYPE_EXT_CHS     0x05  /* Extended Partition (CHS) → EBR chain */
#define MBR_TYPE_FAT16B      0x06  /* FAT16B >32 MB */
#define MBR_TYPE_NTFS        0x07  /* NTFS / HPFS / exFAT */
#define MBR_TYPE_FAT32_CHS   0x0B  /* FAT32 (CHS addressing) */
#define MBR_TYPE_FAT32_LBA   0x0C  /* FAT32 (LBA addressing) */
#define MBR_TYPE_FAT16_LBA   0x0E  /* FAT16 (LBA addressing) */
#define MBR_TYPE_EXT_LBA     0x0F  /* Extended Partition (LBA) → EBR chain */

/* Hidden / vendor recovery types (do NOT auto-mount) */
#define MBR_TYPE_HIDDEN_FAT12     0x11  /* Hidden FAT12 */
#define MBR_TYPE_HIDDEN_FAT16     0x14  /* Hidden FAT16 */
#define MBR_TYPE_HIDDEN_FAT32     0x1B  /* Hidden FAT32 */
#define MBR_TYPE_HIDDEN_FAT32_LBA 0x1C  /* Hidden FAT32 LBA */
#define MBR_TYPE_WINRE        0x27  /* Windows Recovery Environment */

/* Special Microsoft types */
#define MBR_TYPE_DYNAMIC      0x42  /* Windows Dynamic Disk (LDM) */

/* Linux types */
#define MBR_TYPE_LINUX_SWAP   0x82  /* Linux Swap */
#define MBR_TYPE_LINUX        0x83  /* Linux Native (ext2/3/4) */
#define MBR_TYPE_LINUX_EXT    0x85  /* Linux Extended (nested) */
#define MBR_TYPE_LINUX_LVM    0x8E  /* Linux LVM */

/* BSD / Unix types */
#define MBR_TYPE_FREEBSD      0xA5  /* FreeBSD */
#define MBR_TYPE_OPENBSD      0xA6  /* OpenBSD */
#define MBR_TYPE_NETBSD       0xA9  /* NetBSD */

/* macOS / Solaris */
#define MBR_TYPE_HFS_PLUS     0xAF  /* macOS HFS+ */
#define MBR_TYPE_SOLARIS      0xBF  /* Solaris / illumos */

/* Impossible OS */
#define MBR_TYPE_IXFS         0xDA  /* IXFS (Impossible OS native) */

/* Exotic / special */
#define MBR_TYPE_BEOS         0xEB  /* BeOS / Haiku */
#define MBR_TYPE_GPT          0xEE  /* GPT Protective MBR → redirect */
#define MBR_TYPE_EFI_SP       0xEF  /* EFI System Partition (Hybrid MBR) */
#define MBR_TYPE_VMFS         0xFB  /* VMware VMFS */
#define MBR_TYPE_LINUX_RAID   0xFD  /* Linux RAID autodetect */

/* Forward-declare blkdev for EBR walker */
struct blkdev;

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
    int            is_extended;  /* 1 if type is 0x05, 0x0F, or 0x85 */
    int            is_hidden;    /* 1 if type is hidden/recovery/WinRE */
};

/* Partition scan result (from 512-byte MBR sector) */
struct mbr_table {
    int              valid;          /* 1 if 0xAA55 signature found */
    int              count;          /* Number of non-empty partitions */
    uint32_t         disk_signature; /* 32-bit Unique Disk Signature (LE) */
    uint16_t         reserved;       /* Reserved field (offsets 444–445) */
    int              boot_count;     /* Number of entries with status 0x80 */
    int              has_gpt;        /* 1 if any entry is type 0xEE */
    struct mbr_entry parts[MBR_MAX_PARTITIONS];
};

/* ---- API ---- */

/* Parse MBR from a 512-byte sector buffer.
 * Returns filled mbr_table. Check .valid for signature presence.
 * If .has_gpt is set, caller should redirect to GPT parser. */
struct mbr_table mbr_parse(const void *sector0);

/* Return human-readable name for a partition type ID.
 * Returns static string -- never returns NULL. */
const char *mbr_type_name(uint8_t type);

/* Check if a partition type ID is an extended container (0x05/0x0F/0x85). */
int mbr_type_is_extended(uint8_t type);

/* Check if a partition type ID is hidden/vendor recovery. */
int mbr_type_is_hidden(uint8_t type);

/* Walk the EBR linked list starting from an extended partition entry.
 * Reads sectors from 'dev'. Populates 'out' with up to 'max_out' logical
 * partition entries (absolute LBAs). Returns count of logical partitions
 * found, or -1 on read error. Numbering starts at 5 per MBR convention.
 * Caps traversal at MBR_MAX_LOGICAL (128) to prevent infinite loops. */
int mbr_walk_ebr(const struct blkdev *dev, uint32_t ext_start_lba,
                 uint64_t disk_sectors,
                 struct mbr_entry *out, int max_out);
