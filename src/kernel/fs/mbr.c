/* ============================================================================
 * mbr.c -- MBR Partition Table Parser
 *
 * Decodes the complete 512-byte Master Boot Record:
 *
 *   Offset  Size  Description
 *   ------  ----  -----------
 *   0       440   Bootstrap code (not interpreted)
 *   440     4     Unique Disk Signature (Little-Endian)
 *   444     2     Reserved (usually 0x0000)
 *   446     64    4 × 16-byte partition entries
 *   510     2     Boot Record Signature (0x55, 0xAA)
 *
 * Each 16-byte partition entry:
 *   0       1     Boot Indicator (0x80 = active, 0x00 = inactive)
 *   1       3     CHS of first sector (bit-packed)
 *   4       1     Partition Type ID
 *   5       3     CHS of last sector (bit-packed)
 *   8       4     Starting LBA (Little-Endian)
 *   12      4     Total Sectors (Little-Endian)
 *
 * Recognizes 30+ partition types. Flags extended containers (0x05/0x0F/0x85),
 * hidden/vendor recovery partitions, Dynamic Disks (0x42), and GPT
 * Protective MBR (0xEE, triggers redirect to GPT parser).
 * ============================================================================ */

#include "kernel/fs/mbr.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/klog.h"

/* ---- Read little-endian uint32 from byte buffer ---- */
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ---- Read little-endian uint16 from byte buffer ---- */
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* ---- Decode 3-byte CHS tuple ---- */
static struct mbr_chs chs_decode(const uint8_t *b)
{
    struct mbr_chs c;
    c.head     = b[0];
    c.sector   = b[1] & 0x3F;
    c.cylinder = ((uint16_t)(b[1] & 0xC0) << 2) | (uint16_t)b[2];
    return c;
}

/* ---- Check if CHS tuple is the overflow sentinel ---- */
static int chs_is_overflow(const uint8_t *b)
{
    /* FE FF FF  (Head=254, Sector=63, Cylinder=1023) */
    if (b[0] == 0xFE && b[1] == 0xFF && b[2] == 0xFF)
        return 1;
    /* FF FF FF  (some tools use all-ones) */
    if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF)
        return 1;
    return 0;
}

/* ---- Type classification helpers ---- */

int mbr_type_is_extended(uint8_t type)
{
    return (type == MBR_TYPE_EXT_CHS
         || type == MBR_TYPE_EXT_LBA
         || type == MBR_TYPE_LINUX_EXT);
}

int mbr_type_is_hidden(uint8_t type)
{
    return (type == MBR_TYPE_HIDDEN_FAT12
         || type == MBR_TYPE_HIDDEN_FAT16
         || type == MBR_TYPE_HIDDEN_FAT32
         || type == MBR_TYPE_HIDDEN_FAT32_LBA
         || type == MBR_TYPE_WINRE);
}

/* ---- MBR parser ---- */

struct mbr_table mbr_parse(const void *sector0)
{
    struct mbr_table tbl;
    const uint8_t *buf = (const uint8_t *)sector0;
    uint16_t sig;
    int i;

    tbl.valid          = 0;
    tbl.count          = 0;
    tbl.disk_signature = 0;
    tbl.reserved       = 0;
    tbl.boot_count     = 0;
    tbl.has_gpt        = 0;

    /* ---- Validate Boot Record Signature at offset 510 ---- */
    sig = read_le16(buf + MBR_SIG_OFFSET);
    if (sig != MBR_SIGNATURE) {
        /* No 0xAA55 → not an MBR */
        return tbl;
    }

    tbl.valid = 1;

    /* ---- Extract 32-bit Disk Signature (offsets 440–443) ---- */
    tbl.disk_signature = read_le32(buf + MBR_DISK_SIG_OFFSET);
    klog(LOG_DEBUG, "mbr", "Disk Signature: 0x%08X",
         (uint64_t)tbl.disk_signature);

    /* ---- Extract reserved field (offsets 444–445) ---- */
    tbl.reserved = read_le16(buf + MBR_RESERVED_OFFSET);
    if (tbl.reserved != 0) {
        klog(LOG_DEBUG, "mbr", "Reserved field non-zero: 0x%04X",
             (uint64_t)tbl.reserved);
    }

    /* ---- Parse 4 partition entries starting at offset 446 ---- */
    for (i = 0; i < MBR_MAX_PARTITIONS; i++) {
        const uint8_t *entry = buf + MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE;
        struct mbr_entry *p;

        uint8_t  boot_ind = entry[0];
        uint8_t  type     = entry[4];
        uint32_t start    = read_le32(entry + 8);
        uint32_t sectors  = read_le32(entry + 12);

        /* Skip completely empty entries */
        if (type == MBR_TYPE_EMPTY && sectors == 0)
            continue;

        /* Detect GPT Protective MBR → set redirect flag */
        if (type == MBR_TYPE_GPT) {
            tbl.has_gpt = 1;
            klog(LOG_DEBUG, "mbr",
                 "Entry %d: GPT Protective (0xEE) -- redirect to GPT parser",
                 (uint64_t)i);
        }

        /* Warn on Dynamic Disk -- not supported */
        if (type == MBR_TYPE_DYNAMIC) {
            klog(LOG_WARN, "mbr",
                 "Entry %d: Dynamic Disk (0x42) -- LDM volumes not supported",
                 (uint64_t)i);
        }

        /* Validate Boot Indicator: only 0x00 or 0x80 are legal */
        if (boot_ind != 0x00 && boot_ind != 0x80) {
            klog(LOG_WARN, "mbr",
                 "Entry %d: invalid boot indicator 0x%02X (expected 0x00/0x80)",
                 (uint64_t)i, (uint64_t)boot_ind);
        }

        /* Track bootable entries */
        if (boot_ind == 0x80)
            tbl.boot_count++;

        /* Store parsed entry */
        p = &tbl.parts[tbl.count];
        p->status       = boot_ind;
        p->type         = type;
        p->start_lba    = start;
        p->sector_count = sectors;

        /* Decode CHS start (bytes 1–3) and CHS end (bytes 5–7) */
        p->chs_start    = chs_decode(entry + 1);
        p->chs_end      = chs_decode(entry + 5);
        p->chs_overflow  = chs_is_overflow(entry + 1)
                        || chs_is_overflow(entry + 5);

        /* Classify the partition type */
        p->is_extended  = mbr_type_is_extended(type);
        p->is_hidden    = mbr_type_is_hidden(type);

        /* Log the parsed entry with type name */
        klog(LOG_DEBUG, "mbr",
             "Entry %d: %s (0x%02X), LBA=%u, sectors=%u, boot=%s%s%s",
             (uint64_t)i, mbr_type_name(type), (uint64_t)type,
             (uint64_t)start, (uint64_t)sectors,
             (boot_ind == 0x80) ? "active" : "inactive",
             p->is_extended ? ", extended" : "",
             p->is_hidden ? ", hidden" : "");

        tbl.count++;
    }

    /* Warn if multiple entries have boot indicator 0x80 */
    if (tbl.boot_count > 1) {
        klog(LOG_WARN, "mbr",
             "Multiple bootable partitions detected (%d entries with 0x80)",
             (uint64_t)tbl.boot_count);
    }

    return tbl;
}

/* ---- Partition type name lookup ----
 * Returns a human-readable string for known types.
 * Unknown types return "Unknown" -- callers can use the hex code for detail. */

const char *mbr_type_name(uint8_t type)
{
    switch (type) {
    /* Basic types */
    case MBR_TYPE_EMPTY:           return "Empty";
    case MBR_TYPE_FAT12:           return "FAT12";
    case MBR_TYPE_FAT16_SM:        return "FAT16 (<=32M)";
    case MBR_TYPE_EXT_CHS:         return "Extended (CHS)";
    case MBR_TYPE_FAT16B:          return "FAT16B (>32M)";
    case MBR_TYPE_NTFS:            return "NTFS/HPFS/exFAT";
    case MBR_TYPE_FAT32_CHS:       return "FAT32 (CHS)";
    case MBR_TYPE_FAT32_LBA:       return "FAT32 (LBA)";
    case MBR_TYPE_FAT16_LBA:       return "FAT16 (LBA)";
    case MBR_TYPE_EXT_LBA:         return "Extended (LBA)";

    /* Hidden / vendor recovery */
    case MBR_TYPE_HIDDEN_FAT12:    return "Hidden FAT12";
    case MBR_TYPE_HIDDEN_FAT16:    return "Hidden FAT16";
    case MBR_TYPE_HIDDEN_FAT32:    return "Hidden FAT32";
    case MBR_TYPE_HIDDEN_FAT32_LBA: return "Hidden FAT32 LBA";
    case MBR_TYPE_WINRE:           return "Windows Recovery";

    /* Microsoft */
    case MBR_TYPE_DYNAMIC:         return "Dynamic Disk (LDM)";

    /* Linux */
    case MBR_TYPE_LINUX_SWAP:      return "Linux Swap";
    case MBR_TYPE_LINUX:           return "Linux";
    case MBR_TYPE_LINUX_EXT:       return "Linux Extended";
    case MBR_TYPE_LINUX_LVM:       return "Linux LVM";

    /* BSD */
    case MBR_TYPE_FREEBSD:         return "FreeBSD";
    case MBR_TYPE_OPENBSD:         return "OpenBSD";
    case MBR_TYPE_NETBSD:          return "NetBSD";

    /* macOS / Solaris */
    case MBR_TYPE_HFS_PLUS:        return "HFS+";
    case MBR_TYPE_SOLARIS:         return "Solaris";

    /* Impossible OS */
    case MBR_TYPE_IXFS:            return "IXFS";

    /* Special */
    case MBR_TYPE_BEOS:            return "BeOS/Haiku";
    case MBR_TYPE_GPT:             return "GPT Protective";
    case MBR_TYPE_EFI_SP:          return "EFI System";
    case MBR_TYPE_VMFS:            return "VMware VMFS";
    case MBR_TYPE_LINUX_RAID:      return "Linux RAID";

    default:                       return "Unknown";
    }
}

/* ---- EBR chain walker ----
 * Traverses Extended Boot Record linked list to discover logical partitions.
 *
 * EBR layout (same 512-byte format as MBR, but only entries 1 & 2 used):
 *   Entry 1 (offset 0x1BE): logical partition, LBA relative to THIS EBR
 *   Entry 2 (offset 0x1CE): next EBR pointer, LBA relative to FIRST EBR
 *   Entries 3 & 4: must be zero
 *
 * Addressing rules:
 *   volume_lba   = current_ebr_lba + entry1.start_lba  (Rule 1)
 *   next_ebr_lba = first_ebr_lba   + entry2.start_lba  (Rule 2)
 */

/* Check if a 16-byte entry is all zeros */
static int entry_is_zero(const uint8_t *entry)
{
    int j;
    for (j = 0; j < MBR_ENTRY_SIZE; j++) {
        if (entry[j] != 0)
            return 0;
    }
    return 1;
}

int mbr_walk_ebr(const struct blkdev *dev, uint32_t ext_start_lba,
                 uint64_t disk_sectors,
                 struct mbr_entry *out, int max_out)
{
    uint8_t sect[512];
    uint32_t first_ebr_lba = ext_start_lba;
    uint32_t current_ebr_lba = ext_start_lba;
    int logical_num = 0;  /* index into out[] */
    int part_label = 5;   /* MBR logical numbering starts at 5 */
    int cap;

    /* Determine actual cap */
    cap = max_out;
    if (cap > MBR_MAX_LOGICAL)
        cap = MBR_MAX_LOGICAL;

    klog(LOG_DEBUG, "mbr", "EBR chain: starting at LBA %u",
         (uint64_t)ext_start_lba);

    while (logical_num < cap) {
        const uint8_t *e1;  /* Entry 1: logical volume */
        const uint8_t *e2;  /* Entry 2: next EBR pointer */
        const uint8_t *e3;  /* Entry 3: must be zero */
        const uint8_t *e4;  /* Entry 4: must be zero */

        uint8_t  type1;
        uint32_t start1, sectors1;
        uint32_t start2;
        uint16_t sig;

        /* Bounds check: current EBR must be within disk */
        if ((uint64_t)current_ebr_lba >= disk_sectors) {
            klog(LOG_WARN, "mbr",
                 "EBR at LBA %u beyond disk (%u sectors) -- aborting",
                 (uint64_t)current_ebr_lba, disk_sectors);
            break;
        }

        /* Read EBR sector */
        if (blkdev_read(dev, (uint64_t)current_ebr_lba, 1, sect) != 0) {
            klog(LOG_WARN, "mbr", "EBR read failed at LBA %u",
                 (uint64_t)current_ebr_lba);
            return -1;
        }

        /* Validate EBR boot signature 0xAA55 */
        sig = read_le16(sect + MBR_SIG_OFFSET);
        if (sig != MBR_SIGNATURE) {
            klog(LOG_WARN, "mbr",
                 "EBR at LBA %u: missing 0xAA55 signature -- aborting chain",
                 (uint64_t)current_ebr_lba);
            break;
        }

        /* Entry 1 (offset 0x1BE = 446): local logical volume */
        e1 = sect + MBR_ENTRY_OFFSET;
        type1    = e1[4];
        start1   = read_le32(e1 + 8);
        sectors1 = read_le32(e1 + 12);

        /* Entry 2 (offset 0x1CE = 462): next EBR pointer */
        e2 = sect + MBR_ENTRY_OFFSET + MBR_ENTRY_SIZE;
        start2 = read_le32(e2 + 8);

        /* Entries 3 & 4 should be zero */
        e3 = sect + MBR_ENTRY_OFFSET + 2 * MBR_ENTRY_SIZE;
        e4 = sect + MBR_ENTRY_OFFSET + 3 * MBR_ENTRY_SIZE;
        if (!entry_is_zero(e3) || !entry_is_zero(e4)) {
            klog(LOG_WARN, "mbr",
                 "EBR at LBA %u: entries 3/4 non-zero (non-standard)",
                 (uint64_t)current_ebr_lba);
        }

        /* Parse logical volume from Entry 1 */
        if (type1 != MBR_TYPE_EMPTY && sectors1 > 0) {
            struct mbr_entry *p = &out[logical_num];

            /* Rule 1: volume LBA = current EBR LBA + entry1.start_lba */
            p->status       = e1[0];
            p->type         = type1;
            p->start_lba    = current_ebr_lba + start1;
            p->sector_count = sectors1;
            p->chs_start    = chs_decode(e1 + 1);
            p->chs_end      = chs_decode(e1 + 5);
            p->chs_overflow  = chs_is_overflow(e1 + 1)
                            || chs_is_overflow(e1 + 5);
            p->is_extended  = 0;
            p->is_hidden    = mbr_type_is_hidden(type1);

            klog(LOG_DEBUG, "mbr",
                 "Logical %d: %s (0x%02X), LBA=%u, sectors=%u",
                 (uint64_t)part_label, mbr_type_name(type1),
                 (uint64_t)type1, (uint64_t)p->start_lba,
                 (uint64_t)sectors1);

            logical_num++;
            part_label++;
        }

        /* Check Entry 2: next EBR pointer */
        if (entry_is_zero(e2)) {
            /* End of chain */
            break;
        }

        {
            /* Rule 2: next EBR LBA = first EBR LBA + entry2.start_lba */
            uint32_t next_ebr_lba = first_ebr_lba + start2;

            /* Circular link detection */
            if (next_ebr_lba == current_ebr_lba) {
                klog(LOG_WARN, "mbr",
                     "EBR circular link at LBA %u -- aborting",
                     (uint64_t)current_ebr_lba);
                break;
            }

            /* Bounds check */
            if ((uint64_t)next_ebr_lba >= disk_sectors) {
                klog(LOG_WARN, "mbr",
                     "EBR next pointer LBA %u beyond disk -- aborting",
                     (uint64_t)next_ebr_lba);
                break;
            }

            current_ebr_lba = next_ebr_lba;
        }
    }

    if (logical_num >= cap) {
        klog(LOG_WARN, "mbr",
             "EBR chain capped at %d logical partitions",
             (uint64_t)cap);
    }

    klog(LOG_DEBUG, "mbr", "EBR chain: found %d logical partition(s)",
         (uint64_t)logical_num);

    return logical_num;
}
