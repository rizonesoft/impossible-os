/* ============================================================================
 * mbr.c — MBR Partition Table Parser
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
 * The MBR is valid only if bytes 510–511 contain 0x55 0xAA.
 * ============================================================================ */

#include "kernel/fs/mbr.h"
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

        /* Skip completely empty entries (all 16 bytes zero) */
        if (type == MBR_TYPE_EMPTY && sectors == 0)
            continue;

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

        /* Log the parsed entry */
        klog(LOG_DEBUG, "mbr",
             "Entry %d: type=0x%02X, LBA=%u, sectors=%u, boot=%s",
             (uint64_t)i, (uint64_t)type, (uint64_t)start,
             (uint64_t)sectors,
             (boot_ind == 0x80) ? "active" : "inactive");

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

const char *mbr_type_name(uint8_t type)
{
    switch (type) {
    case MBR_TYPE_EMPTY:      return "Empty";
    case MBR_TYPE_EXT_CHS:    return "Extended (CHS)";
    case MBR_TYPE_FAT32_CHS:  return "FAT32 (CHS)";
    case MBR_TYPE_FAT32_LBA:  return "FAT32 (LBA)";
    case MBR_TYPE_EXT_LBA:    return "Extended (LBA)";
    case MBR_TYPE_LINUX:      return "Linux";
    case MBR_TYPE_IXFS:       return "IXFS";
    case MBR_TYPE_GPT:        return "GPT Protective";
    default:                  return "Unknown";
    }
}
