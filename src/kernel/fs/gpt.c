/* ============================================================================
 * gpt.c — GUID Partition Table Parser
 *
 * Parses the UEFI GPT layout:
 *   LBA 0: Protective MBR (type 0xEE spanning entire disk)
 *   LBA 1: Primary GPT header (92 bytes, CRC32-protected)
 *   LBA 2+: Partition entry array (128 bytes each, CRC32-protected)
 *
 * Validation steps:
 *   1. Verify protective MBR at LBA 0 has type 0xEE
 *   2. Read GPT header at LBA 1, check "EFI PART" signature
 *   3. Validate header CRC32 (zero the CRC field, compute, compare)
 *   4. Read partition entry array, validate array CRC32
 *   5. Parse non-empty entries (type_guid != all-zeros)
 * ============================================================================ */

#include "kernel/fs/gpt.h"
#include "kernel/fs/mbr.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/drivers/serial.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- Well-known GUIDs ---- */

const struct gpt_guid GPT_GUID_EMPTY = {0, 0, 0, {0,0,0,0,0,0,0,0}};

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
const struct gpt_guid GPT_GUID_EFI_SYSTEM = {
    0xC12A7328, 0xF81F, 0x11D2,
    {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}
};

/* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
const struct gpt_guid GPT_GUID_MS_BASIC_DATA = {
    0xEBD0A0A2, 0xB9E5, 0x4433,
    {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}
};

/* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
const struct gpt_guid GPT_GUID_LINUX_FS = {
    0x0FC63DAF, 0x8483, 0x4772,
    {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}
};

/* DA000000-0000-4978-4653-000000000001 (custom IXFS GUID) */
const struct gpt_guid GPT_GUID_IXFS = {
    0xDA000000, 0x0000, 0x4978,
    {0x46, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}
};

/* 21686148-6449-6E6F-744E-656564454649  — BIOS Boot Partition */
const struct gpt_guid GPT_GUID_BIOS_BOOT = {
    0x21686148, 0x6449, 0x6E6F,
    {0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}
};

/* E3C9E316-0B5C-4DB8-817D-F92DF00215AE  — Microsoft Reserved */
const struct gpt_guid GPT_GUID_MS_RESERVED = {
    0xE3C9E316, 0x0B5C, 0x4DB8,
    {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}
};

/* 5808C8AA-7E8F-42E0-85D2-E1E90434CFB3  — Microsoft LDM Metadata */
const struct gpt_guid GPT_GUID_MS_LDM_META = {
    0x5808C8AA, 0x7E8F, 0x42E0,
    {0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3}
};

/* AF9B60A0-1431-4F62-BC68-3311714A69AD  — Microsoft LDM Data */
const struct gpt_guid GPT_GUID_MS_LDM_DATA = {
    0xAF9B60A0, 0x1431, 0x4F62,
    {0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD}
};

/* DE94BBA4-06D1-4D40-A16A-BFD50179D6AC  — Microsoft Recovery */
const struct gpt_guid GPT_GUID_MS_RECOVERY = {
    0xDE94BBA4, 0x06D1, 0x4D40,
    {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}
};

/* E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D  — Microsoft Storage Spaces */
const struct gpt_guid GPT_GUID_MS_STORAGE_SPACES = {
    0xE75CAF8F, 0xF680, 0x4CEE,
    {0xAF, 0xA3, 0xB0, 0x01, 0xE5, 0x6E, 0xFC, 0x2D}
};

/* 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F  — Linux Swap */
const struct gpt_guid GPT_GUID_LINUX_SWAP = {
    0x0657FD6D, 0xA4AB, 0x43C4,
    {0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F}
};

/* 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709  — Linux Root (x86-64) */
const struct gpt_guid GPT_GUID_LINUX_ROOT_X64 = {
    0x4F68BCE3, 0xE8CD, 0x4DB1,
    {0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09}
};

/* 933AC7E1-2EB4-4F13-B844-0E14E2AEF915  — Linux /home */
const struct gpt_guid GPT_GUID_LINUX_HOME = {
    0x933AC7E1, 0x2EB4, 0x4F13,
    {0xB8, 0x44, 0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15}
};

/* 3B8F8425-20E0-4F3B-907F-1A25A76F98E8  — Linux /srv */
const struct gpt_guid GPT_GUID_LINUX_SRV = {
    0x3B8F8425, 0x20E0, 0x4F3B,
    {0x90, 0x7F, 0x1A, 0x25, 0xA7, 0x6F, 0x98, 0xE8}
};

/* E6D6D379-F507-44C2-A23C-238F2A3DF928  — Linux LVM */
const struct gpt_guid GPT_GUID_LINUX_LVM = {
    0xE6D6D379, 0xF507, 0x44C2,
    {0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28}
};

/* A19D880F-05FC-4D3B-A006-743F0F84911E  — Linux RAID */
const struct gpt_guid GPT_GUID_LINUX_RAID = {
    0xA19D880F, 0x05FC, 0x4D3B,
    {0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E}
};

/* 48465300-0000-11AA-AA11-00306543ECAC  — Apple HFS+ */
const struct gpt_guid GPT_GUID_APPLE_HFS = {
    0x48465300, 0x0000, 0x11AA,
    {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}
};

/* 7C3457EF-0000-11AA-AA11-00306543ECAC  — Apple APFS */
const struct gpt_guid GPT_GUID_APPLE_APFS = {
    0x7C3457EF, 0x0000, 0x11AA,
    {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}
};

/* 516E7CB5-6ECF-11D6-8FF8-00022D09712B  — FreeBSD ZFS */
const struct gpt_guid GPT_GUID_FREEBSD_ZFS = {
    0x516E7CB5, 0x6ECF, 0x11D6,
    {0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B}
};

/* 6A85CF4D-1DD2-11B2-99A6-080020736631  — Solaris Root */
const struct gpt_guid GPT_GUID_SOLARIS_ROOT = {
    0x6A85CF4D, 0x1DD2, 0x11B2,
    {0x99, 0xA6, 0x08, 0x00, 0x20, 0x73, 0x66, 0x31}
};

/* AA31E02A-400F-11DB-9590-000C2911D1B8  — VMware VMFS */
const struct gpt_guid GPT_GUID_VMWARE_VMFS = {
    0xAA31E02A, 0x400F, 0x11DB,
    {0x95, 0x90, 0x00, 0x0C, 0x29, 0x11, 0xD1, 0xB8}
};

/* FE3A2A5D-4F32-41A7-B725-ACCC3285A309  — ChromeOS Kernel */
const struct gpt_guid GPT_GUID_CHROMEOS_KERNEL = {
    0xFE3A2A5D, 0x4F32, 0x41A7,
    {0xB7, 0x25, 0xAC, 0xCC, 0x32, 0x85, 0xA3, 0x09}
};

/* 4FBD7E29-9D25-41B8-AFD0-062C0CEFF05D  — Ceph OSD */
const struct gpt_guid GPT_GUID_CEPH_OSD = {
    0x4FBD7E29, 0x9D25, 0x41B8,
    {0xAF, 0xD0, 0x06, 0x2C, 0x0C, 0xEF, 0xF0, 0x5D}
};

/* ---- CRC32 (IEEE 802.3, polynomial 0xEDB88320) ---- */

static uint32_t crc32_table[256];
static int crc32_table_ready;

static void crc32_init_table(void)
{
    uint32_t i, j, crc;
    for (i = 0; i < 256; i++) {
        crc = i;
        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_table_ready = 1;
}

uint32_t gpt_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i;

    if (!crc32_table_ready)
        crc32_init_table();

    for (i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFF];

    return crc ^ 0xFFFFFFFF;
}

/* ---- Helper functions ---- */

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static void read_guid(const uint8_t *p, struct gpt_guid *g)
{
    int i;
    /* Mixed endian: data1 (LE32), data2 (LE16), data3 (LE16), data4 (bytes) */
    g->data1 = read_le32(p);
    g->data2 = read_le16(p + 4);
    g->data3 = read_le16(p + 6);
    for (i = 0; i < 8; i++)
        g->data4[i] = p[8 + i];
}

int gpt_guid_equal(const struct gpt_guid *a, const struct gpt_guid *b)
{
    int i;
    if (a->data1 != b->data1) return 0;
    if (a->data2 != b->data2) return 0;
    if (a->data3 != b->data3) return 0;
    for (i = 0; i < 8; i++) {
        if (a->data4[i] != b->data4[i]) return 0;
    }
    return 1;
}

const char *gpt_type_name(const struct gpt_guid *guid)
{
    if (gpt_guid_equal(guid, &GPT_GUID_EFI_SYSTEM))       return "EFI System";
    if (gpt_guid_equal(guid, &GPT_GUID_MS_BASIC_DATA))     return "Basic Data";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_FS))          return "Linux";
    if (gpt_guid_equal(guid, &GPT_GUID_IXFS))              return "IXFS";
    if (gpt_guid_equal(guid, &GPT_GUID_BIOS_BOOT))         return "BIOS Boot";
    if (gpt_guid_equal(guid, &GPT_GUID_MS_RESERVED))       return "MS Reserved";
    if (gpt_guid_equal(guid, &GPT_GUID_MS_LDM_META))       return "MS LDM Metadata";
    if (gpt_guid_equal(guid, &GPT_GUID_MS_LDM_DATA))       return "MS LDM Data";
    if (gpt_guid_equal(guid, &GPT_GUID_MS_RECOVERY))       return "MS Recovery";
    if (gpt_guid_equal(guid, &GPT_GUID_MS_STORAGE_SPACES)) return "MS Storage Spaces";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_SWAP))        return "Linux Swap";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_ROOT_X64))    return "Linux Root (x86-64)";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_HOME))        return "Linux /home";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_SRV))         return "Linux /srv";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_LVM))         return "Linux LVM";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_RAID))        return "Linux RAID";
    if (gpt_guid_equal(guid, &GPT_GUID_APPLE_HFS))         return "Apple HFS+";
    if (gpt_guid_equal(guid, &GPT_GUID_APPLE_APFS))        return "Apple APFS";
    if (gpt_guid_equal(guid, &GPT_GUID_FREEBSD_ZFS))       return "FreeBSD ZFS";
    if (gpt_guid_equal(guid, &GPT_GUID_SOLARIS_ROOT))      return "Solaris Root";
    if (gpt_guid_equal(guid, &GPT_GUID_VMWARE_VMFS))       return "VMware VMFS";
    if (gpt_guid_equal(guid, &GPT_GUID_CHROMEOS_KERNEL))   return "ChromeOS Kernel";
    if (gpt_guid_equal(guid, &GPT_GUID_CEPH_OSD))          return "Ceph OSD";
    return "Unknown";
}

/* ---- GPT Header Parsing ---- */

static int parse_header(const uint8_t *buf, struct gpt_header *hdr)
{
    uint32_t stored_crc, computed_crc;
    uint8_t tmp[92];
    int i;

    /* Read fields from the raw 512-byte sector */
    hdr->signature       = read_le64(buf + 0);
    hdr->revision        = read_le32(buf + 8);
    hdr->header_size     = read_le32(buf + 12);
    hdr->header_crc32    = read_le32(buf + 16);
    hdr->reserved        = read_le32(buf + 20);
    hdr->my_lba          = read_le64(buf + 24);
    hdr->alt_lba         = read_le64(buf + 32);
    hdr->first_usable_lba= read_le64(buf + 40);
    hdr->last_usable_lba = read_le64(buf + 48);
    read_guid(buf + 56, &hdr->disk_guid);
    hdr->part_entry_lba  = read_le64(buf + 72);
    hdr->num_part_entries= read_le32(buf + 80);
    hdr->part_entry_size = read_le32(buf + 84);
    hdr->part_entry_crc32= read_le32(buf + 88);

    /* Verify signature "EFI PART" */
    if (hdr->signature != GPT_SIGNATURE) {
        serial_write("[GPT] Bad signature\n");
        return -1;
    }

    /* Validate header CRC32: zero the CRC field, compute, compare */
    if (hdr->header_size > 512 || hdr->header_size < 92) {
        serial_write("[GPT] Invalid header size\n");
        return -1;
    }

    for (i = 0; i < (int)hdr->header_size && i < 92; i++)
        tmp[i] = buf[i];
    /* Zero the CRC field (bytes 16–19) */
    tmp[16] = 0; tmp[17] = 0; tmp[18] = 0; tmp[19] = 0;

    stored_crc = hdr->header_crc32;
    computed_crc = gpt_crc32(tmp, hdr->header_size);

    if (computed_crc != stored_crc) {
        klog(LOG_DEBUG, "blk", "Header CRC32 mismatch: stored=%x computed=%x",
               (uint64_t)stored_crc, (uint64_t)computed_crc);
        return -1;
    }

    return 0;
}

/* ---- Main Parser ---- */

struct gpt_table gpt_parse(const struct blkdev *dev, const void *sector0)
{
    struct gpt_table tbl;
    const uint8_t *mbr_buf = (const uint8_t *)sector0;
    struct gpt_header hdr;
    uint8_t hdr_sect[512];
    uint8_t entry_buf[512];     /* Read one sector at a time */
    uint32_t entries_per_sector;
    uint32_t total_entries;
    uint32_t total_entry_bytes;
    uint32_t entry_crc;
    uint32_t sectors_needed;
    uint64_t entry_lba;
    uint64_t backup_lba;
    uint32_t si, ei;
    int primary_ok;
    int using_backup = 0;
    int i;

    tbl.valid = 0;
    tbl.count = 0;

    /* Step 1: Verify protective MBR has type 0xEE */
    {
        int has_ee = 0;
        for (i = 0; i < 4; i++) {
            uint8_t ptype = mbr_buf[MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE + 4];
            if (ptype == MBR_TYPE_GPT) {
                has_ee = 1;
                break;
            }
        }
        if (!has_ee) {
            serial_write("[GPT] No protective MBR (0xEE) found\n");
            return tbl;
        }
    }

    /* Step 2: Read GPT header at LBA 1 */
    if (blkdev_read(dev, GPT_HEADER_LBA, 1, hdr_sect) != 0) {
        serial_write("[GPT] Failed to read LBA 1\n");
        return tbl;
    }

    /* Step 3: Parse and validate header (signature + CRC32) */
    primary_ok = (parse_header(hdr_sect, &hdr) == 0);

    /* §2.1 Backup Header Fallback: if primary fails, try backup at last LBA */
    if (!primary_ok) {
        if (dev->sector_count == 0) {
            serial_write("[GPT] Primary header corrupt, no sector count for backup\n");
            return tbl;
        }
        backup_lba = dev->sector_count - 1;
        if (blkdev_read(dev, backup_lba, 1, hdr_sect) != 0) {
            serial_write("[GPT] Failed to read backup header\n");
            return tbl;
        }
        if (parse_header(hdr_sect, &hdr) != 0) {
            serial_write("[GPT] Both primary and backup headers corrupt\n");
            return tbl;
        }
        /* Verify backup header's self-referencing fields */
        if (hdr.my_lba != backup_lba) {
            serial_write("[GPT] Backup header my_lba mismatch\n");
            return tbl;
        }
        if (hdr.alt_lba != GPT_HEADER_LBA) {
            serial_write("[GPT] Backup header alt_lba != 1\n");
            return tbl;
        }
        klog(LOG_WARN, "blk",
             "Primary GPT header corrupt - using backup at LBA %u",
             (uint64_t)backup_lba);
        using_backup = 1;
    }

    /* Sanity-check entry parameters (applies to whichever header we used) */
    if (hdr.part_entry_size < 128 || hdr.num_part_entries == 0) {
        serial_write("[GPT] Invalid partition entry params\n");
        return tbl;
    }

    /* Step 4: Read and validate partition entry array CRC32.
     * We read sector-by-sector, computing CRC32 incrementally. */
    total_entries = hdr.num_part_entries;
    if (total_entries > GPT_MAX_PARTITIONS)
        total_entries = GPT_MAX_PARTITIONS;

    total_entry_bytes = total_entries * hdr.part_entry_size;
    entries_per_sector = 512 / hdr.part_entry_size;
    sectors_needed = (total_entry_bytes + 511) / 512;
    entry_lba = hdr.part_entry_lba;

    /* Compute CRC32 over the entire partition entry array */
    {
        uint32_t crc = 0xFFFFFFFF;
        uint32_t bytes_remaining = total_entry_bytes;

        if (!crc32_table_ready)
            crc32_init_table();

        for (si = 0; si < sectors_needed; si++) {
            uint32_t chunk;
            if (blkdev_read(dev, entry_lba + si, 1, entry_buf) != 0) {
                serial_write("[GPT] Failed to read entry sector\n");
                return tbl;
            }

            chunk = bytes_remaining > 512 ? 512 : bytes_remaining;
            for (i = 0; i < (int)chunk; i++)
                crc = (crc >> 8) ^ crc32_table[(crc ^ entry_buf[i]) & 0xFF];
            bytes_remaining -= chunk;
        }
        entry_crc = crc ^ 0xFFFFFFFF;
    }

    if (entry_crc != hdr.part_entry_crc32) {
        klog(LOG_DEBUG, "blk", "Entry array CRC32 mismatch: stored=%x computed=%x",
               (uint64_t)hdr.part_entry_crc32, (uint64_t)entry_crc);
        return tbl;
    }

    tbl.valid = 1;

    /* Step 5: Re-read entries and populate the result table */
    for (si = 0; si < sectors_needed && tbl.count < GPT_MAX_RESULTS; si++) {
        if (blkdev_read(dev, entry_lba + si, 1, entry_buf) != 0)
            break;

        for (ei = 0; ei < entries_per_sector && tbl.count < GPT_MAX_RESULTS; ei++) {
            uint32_t idx = si * entries_per_sector + ei;
            const uint8_t *e;
            struct gpt_entry *p;

            if (idx >= total_entries)
                break;

            e = entry_buf + ei * hdr.part_entry_size;

            /* Read type GUID — skip empty entries */
            {
                struct gpt_guid type_g;
                read_guid(e, &type_g);
                if (gpt_guid_equal(&type_g, &GPT_GUID_EMPTY))
                    continue;

                p = &tbl.parts[tbl.count];
                p->type_guid = type_g;
            }

            read_guid(e + 16, &p->unique_guid);
            p->start_lba  = read_le64(e + 32);
            p->end_lba    = read_le64(e + 40);
            p->attributes = read_le64(e + 48);

            /* Convert UTF-16LE name to ASCII (best-effort) */
            for (i = 0; i < GPT_NAME_MAX; i++) {
                uint16_t ch = read_le16(e + 56 + i * 2);
                p->name[i] = (ch < 128 && ch > 0) ? (char)ch : '\0';
                if (ch == 0) break;
            }
            p->name[GPT_NAME_MAX] = '\0';

            tbl.count++;
        }
    }

    (void)using_backup; /* Will be used by §2.2 auto-recovery */

    return tbl;
}
