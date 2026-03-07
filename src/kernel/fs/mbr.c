/* ============================================================================
 * mbr.c — MBR Partition Table Parser
 *
 * Reads the 4 primary partition entries from sector 0 of a block device.
 * Each entry is 16 bytes starting at offset 446:
 *
 *   Offset  Size  Description
 *   ------  ----  -----------
 *   0       1     Status (0x80 = bootable, 0x00 = inactive)
 *   1       3     CHS of first sector (ignored — we use LBA)
 *   4       1     Partition type ID
 *   5       3     CHS of last sector (ignored)
 *   8       4     Start LBA (little-endian)
 *   12      4     Sector count (little-endian)
 *
 * The MBR is valid if bytes 510–511 contain 0x55 0xAA.
 * ============================================================================ */

#include "kernel/fs/mbr.h"

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

struct mbr_table mbr_parse(const void *sector0)
{
    struct mbr_table tbl;
    const uint8_t *buf = (const uint8_t *)sector0;
    uint16_t sig;
    int i;

    tbl.valid = 0;
    tbl.count = 0;

    /* Check MBR boot signature at offset 510 */
    sig = read_le16(buf + 510);
    if (sig != MBR_SIGNATURE)
        return tbl;

    tbl.valid = 1;

    /* Parse 4 partition entries starting at offset 446 */
    for (i = 0; i < MBR_MAX_PARTITIONS; i++) {
        const uint8_t *entry = buf + MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE;
        struct mbr_entry *p = &tbl.parts[tbl.count];

        uint8_t  status   = entry[0];
        uint8_t  type     = entry[4];
        uint32_t start    = read_le32(entry + 8);
        uint32_t sectors  = read_le32(entry + 12);

        /* Skip empty entries */
        if (type == MBR_TYPE_EMPTY)
            continue;

        /* Skip entries with zero size */
        if (sectors == 0)
            continue;

        p->status       = status;
        p->type         = type;
        p->start_lba    = start;
        p->sector_count = sectors;
        tbl.count++;
    }

    return tbl;
}

const char *mbr_type_name(uint8_t type)
{
    switch (type) {
    case MBR_TYPE_EMPTY:      return "Empty";
    case MBR_TYPE_FAT32_CHS:  return "FAT32 (CHS)";
    case MBR_TYPE_FAT32_LBA:  return "FAT32 (LBA)";
    case MBR_TYPE_LINUX:      return "Linux";
    case MBR_TYPE_IXFS:       return "IXFS";
    case MBR_TYPE_GPT:        return "GPT Protective";
    default:                  return "Unknown";
    }
}
