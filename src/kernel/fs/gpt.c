/* ============================================================================
 * gpt.c -- GUID Partition Table Parser
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
#include "kernel/mm/heap.h"

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

/* DA000000-0000-4978-4653-000000000001 (custom IXFS GUID -- A/B Slot A) */
const struct gpt_guid GPT_GUID_IXFS = {
    0xDA000000, 0x0000, 0x4978,
    {0x46, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}
};

/* DA000000-0000-4978-4653-000000000002 (IXFS A/B Slot B). Same family as
 * GPT_GUID_IXFS; only the trailing discriminator byte differs (01=A, 02=B)
 * so the bootloader-selected slot can be matched at mount time (TODO-21). */
const struct gpt_guid GPT_GUID_IXFS_B = {
    0xDA000000, 0x0000, 0x4978,
    {0x46, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}
};

/* 49504F53-7265-636F-7665-727900000001 ("IPOSrecovery") -- read-only Recovery
 * partition (TODO-22). Distinct family from IXFS/ABMeta; the kernel excludes it
 * from drive-letter mounting so a normal boot cannot mutate the recovery image. */
const struct gpt_guid GPT_GUID_RECOVERY = {
    0x49504F53, 0x7265, 0x636F,
    {0x76, 0x65, 0x72, 0x79, 0x00, 0x00, 0x00, 0x01}
};

/* 21686148-6449-6E6F-744E-656564454649  -- BIOS Boot Partition */
const struct gpt_guid GPT_GUID_BIOS_BOOT = {
    0x21686148, 0x6449, 0x6E6F,
    {0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}
};

/* E3C9E316-0B5C-4DB8-817D-F92DF00215AE  -- Microsoft Reserved */
const struct gpt_guid GPT_GUID_MS_RESERVED = {
    0xE3C9E316, 0x0B5C, 0x4DB8,
    {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}
};

/* 5808C8AA-7E8F-42E0-85D2-E1E90434CFB3  -- Microsoft LDM Metadata */
const struct gpt_guid GPT_GUID_MS_LDM_META = {
    0x5808C8AA, 0x7E8F, 0x42E0,
    {0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3}
};

/* AF9B60A0-1431-4F62-BC68-3311714A69AD  -- Microsoft LDM Data */
const struct gpt_guid GPT_GUID_MS_LDM_DATA = {
    0xAF9B60A0, 0x1431, 0x4F62,
    {0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD}
};

/* DE94BBA4-06D1-4D40-A16A-BFD50179D6AC  -- Microsoft Recovery */
const struct gpt_guid GPT_GUID_MS_RECOVERY = {
    0xDE94BBA4, 0x06D1, 0x4D40,
    {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}
};

/* E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D  -- Microsoft Storage Spaces */
const struct gpt_guid GPT_GUID_MS_STORAGE_SPACES = {
    0xE75CAF8F, 0xF680, 0x4CEE,
    {0xAF, 0xA3, 0xB0, 0x01, 0xE5, 0x6E, 0xFC, 0x2D}
};

/* 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F  -- Linux Swap */
const struct gpt_guid GPT_GUID_LINUX_SWAP = {
    0x0657FD6D, 0xA4AB, 0x43C4,
    {0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F}
};

/* 4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709  -- Linux Root (x86-64) */
const struct gpt_guid GPT_GUID_LINUX_ROOT_X64 = {
    0x4F68BCE3, 0xE8CD, 0x4DB1,
    {0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09}
};

/* 933AC7E1-2EB4-4F13-B844-0E14E2AEF915  -- Linux /home */
const struct gpt_guid GPT_GUID_LINUX_HOME = {
    0x933AC7E1, 0x2EB4, 0x4F13,
    {0xB8, 0x44, 0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15}
};

/* 3B8F8425-20E0-4F3B-907F-1A25A76F98E8  -- Linux /srv */
const struct gpt_guid GPT_GUID_LINUX_SRV = {
    0x3B8F8425, 0x20E0, 0x4F3B,
    {0x90, 0x7F, 0x1A, 0x25, 0xA7, 0x6F, 0x98, 0xE8}
};

/* E6D6D379-F507-44C2-A23C-238F2A3DF928  -- Linux LVM */
const struct gpt_guid GPT_GUID_LINUX_LVM = {
    0xE6D6D379, 0xF507, 0x44C2,
    {0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28}
};

/* A19D880F-05FC-4D3B-A006-743F0F84911E  -- Linux RAID */
const struct gpt_guid GPT_GUID_LINUX_RAID = {
    0xA19D880F, 0x05FC, 0x4D3B,
    {0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E}
};

/* 48465300-0000-11AA-AA11-00306543ECAC  -- Apple HFS+ */
const struct gpt_guid GPT_GUID_APPLE_HFS = {
    0x48465300, 0x0000, 0x11AA,
    {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}
};

/* 7C3457EF-0000-11AA-AA11-00306543ECAC  -- Apple APFS */
const struct gpt_guid GPT_GUID_APPLE_APFS = {
    0x7C3457EF, 0x0000, 0x11AA,
    {0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC}
};

/* 516E7CBA-6ECF-11D6-8FF8-00022D09712B  -- FreeBSD ZFS */
const struct gpt_guid GPT_GUID_FREEBSD_ZFS = {
    0x516E7CBA, 0x6ECF, 0x11D6,
    {0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B}
};

/* 6A85CF4D-1DD2-11B2-99A6-080020736631  -- Solaris Root */
const struct gpt_guid GPT_GUID_SOLARIS_ROOT = {
    0x6A85CF4D, 0x1DD2, 0x11B2,
    {0x99, 0xA6, 0x08, 0x00, 0x20, 0x73, 0x66, 0x31}
};

/* AA31E02A-400F-11DB-9590-000C2911D1B8  -- VMware VMFS */
const struct gpt_guid GPT_GUID_VMWARE_VMFS = {
    0xAA31E02A, 0x400F, 0x11DB,
    {0x95, 0x90, 0x00, 0x0C, 0x29, 0x11, 0xD1, 0xB8}
};

/* FE3A2A5D-4F32-41A7-B725-ACCC3285A309  -- ChromeOS Kernel */
const struct gpt_guid GPT_GUID_CHROMEOS_KERNEL = {
    0xFE3A2A5D, 0x4F32, 0x41A7,
    {0xB7, 0x25, 0xAC, 0xCC, 0x32, 0x85, 0xA3, 0x09}
};

/* 4FBD7E29-9D25-41B8-AFD0-062C0CEFF05D  -- Ceph OSD */
const struct gpt_guid GPT_GUID_CEPH_OSD = {
    0x4FBD7E29, 0x9D25, 0x41B8,
    {0xAF, 0xD0, 0x06, 0x2C, 0x0C, 0xEF, 0xF0, 0x5D}
};

/* ---- Linux systemd Discoverable Partitions ---- */

/* 8484680C-9521-48C6-9C11-B0720656F69B  -- Linux /usr (x86-64) */
const struct gpt_guid GPT_GUID_LINUX_USR_X64 = {
    0x8484680C, 0x9521, 0x48C6,
    {0x9C, 0x11, 0xB0, 0x72, 0x06, 0x56, 0xF6, 0x9B}
};

/* 4D21B016-B534-4796-B7EB-173692D58176  -- Linux /var */
const struct gpt_guid GPT_GUID_LINUX_VAR = {
    0x4D21B016, 0xB534, 0x4796,
    {0xB7, 0xEB, 0x17, 0x36, 0x92, 0xD5, 0x81, 0x76}
};

/* 7EC6F557-3BC5-4ACA-B293-16EF5DF639D1  -- Linux /tmp */
const struct gpt_guid GPT_GUID_LINUX_TMP = {
    0x7EC6F557, 0x3BC5, 0x4ACA,
    {0xB2, 0x93, 0x16, 0xEF, 0x5D, 0xF6, 0x39, 0xD1}
};

/* BC13C2FF-59E6-4262-A352-B275FD6F7172  -- Linux Extended Boot (XBOOTLDR) */
const struct gpt_guid GPT_GUID_LINUX_XBOOT = {
    0xBC13C2FF, 0x59E6, 0x4262,
    {0xA3, 0x52, 0xB2, 0x75, 0xFD, 0x6F, 0x71, 0x72}
};

/* ---- FreeBSD (complete set) ---- */

/* 83BD6B9D-7F41-11DC-BE0B-001560B84F0F  -- FreeBSD Boot */
const struct gpt_guid GPT_GUID_FREEBSD_BOOT = {
    0x83BD6B9D, 0x7F41, 0x11DC,
    {0xBE, 0x0B, 0x00, 0x15, 0x60, 0xB8, 0x4F, 0x0F}
};

/* 516E7CB4-6ECF-11D6-8FF8-00022D09712B  -- FreeBSD Data */
const struct gpt_guid GPT_GUID_FREEBSD_DATA = {
    0x516E7CB4, 0x6ECF, 0x11D6,
    {0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B}
};

/* 516E7CB5-6ECF-11D6-8FF8-00022D09712B  -- FreeBSD Swap */
const struct gpt_guid GPT_GUID_FREEBSD_SWAP = {
    0x516E7CB5, 0x6ECF, 0x11D6,
    {0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B}
};

/* 516E7CB6-6ECF-11D6-8FF8-00022D09712B  -- FreeBSD UFS */
const struct gpt_guid GPT_GUID_FREEBSD_UFS = {
    0x516E7CB6, 0x6ECF, 0x11D6,
    {0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B}
};

/* 516E7CB8-6ECF-11D6-8FF8-00022D09712B  -- FreeBSD Vinum/RAID */
const struct gpt_guid GPT_GUID_FREEBSD_VINUM = {
    0x516E7CB8, 0x6ECF, 0x11D6,
    {0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B}
};

/* ---- NetBSD / OpenBSD ---- */

/* 49F48D32-B10E-11DC-B99B-0019D1879648  -- NetBSD Swap */
const struct gpt_guid GPT_GUID_NETBSD_SWAP = {
    0x49F48D32, 0xB10E, 0x11DC,
    {0xB9, 0x9B, 0x00, 0x19, 0xD1, 0x87, 0x96, 0x48}
};

/* 49F48D5A-B10E-11DC-B99B-0019D1879648  -- NetBSD FFS */
const struct gpt_guid GPT_GUID_NETBSD_FFS = {
    0x49F48D5A, 0xB10E, 0x11DC,
    {0xB9, 0x9B, 0x00, 0x19, 0xD1, 0x87, 0x96, 0x48}
};

/* 824CC7A0-36A8-11E3-890A-952519AD3F61  -- OpenBSD Data */
const struct gpt_guid GPT_GUID_OPENBSD_DATA = {
    0x824CC7A0, 0x36A8, 0x11E3,
    {0x89, 0x0A, 0x95, 0x25, 0x19, 0xAD, 0x3F, 0x61}
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

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_le64(uint8_t *p, uint64_t v)
{
    write_le32(p, (uint32_t)v);
    write_le32(p + 4, (uint32_t)(v >> 32));
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

/* A/B dual-slot identity (TODO-21): return 0 for the Slot A IXFS type GUID,
 * 1 for Slot B, or -1 for any non-IXFS-family GUID. Filesystem detection is
 * content-based (probe_ixfs), so both slots already mount as IXFS; this is the
 * only way to tell which physical slot a partition is, so the kernel can mount
 * EXACTLY the slot the bootloader selected. */
int gpt_ixfs_slot(const struct gpt_guid *guid)
{
    if (gpt_guid_equal(guid, &GPT_GUID_IXFS))   return 0;
    if (gpt_guid_equal(guid, &GPT_GUID_IXFS_B)) return 1;
    return -1;
}

int gpt_is_recovery(const struct gpt_guid *guid)
{
    return gpt_guid_equal(guid, &GPT_GUID_RECOVERY) ? 1 : 0;
}

const char *gpt_type_name(const struct gpt_guid *guid)
{
    if (gpt_guid_equal(guid, &GPT_GUID_EFI_SYSTEM))       return "EFI System";
    if (gpt_guid_equal(guid, &GPT_GUID_MS_BASIC_DATA))     return "Basic Data";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_FS))          return "Linux";
    if (gpt_guid_equal(guid, &GPT_GUID_IXFS))              return "IXFS";
    if (gpt_guid_equal(guid, &GPT_GUID_IXFS_B))            return "IXFS";
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
    if (gpt_guid_equal(guid, &GPT_GUID_FREEBSD_BOOT))      return "FreeBSD Boot";
    if (gpt_guid_equal(guid, &GPT_GUID_FREEBSD_DATA))      return "FreeBSD Data";
    if (gpt_guid_equal(guid, &GPT_GUID_FREEBSD_SWAP))      return "FreeBSD Swap";
    if (gpt_guid_equal(guid, &GPT_GUID_FREEBSD_UFS))       return "FreeBSD UFS";
    if (gpt_guid_equal(guid, &GPT_GUID_FREEBSD_VINUM))     return "FreeBSD Vinum";
    if (gpt_guid_equal(guid, &GPT_GUID_SOLARIS_ROOT))      return "Solaris Root";
    if (gpt_guid_equal(guid, &GPT_GUID_VMWARE_VMFS))       return "VMware VMFS";
    if (gpt_guid_equal(guid, &GPT_GUID_CHROMEOS_KERNEL))   return "ChromeOS Kernel";
    if (gpt_guid_equal(guid, &GPT_GUID_CEPH_OSD))          return "Ceph OSD";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_USR_X64))     return "Linux /usr (x86-64)";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_VAR))         return "Linux /var";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_TMP))         return "Linux /tmp";
    if (gpt_guid_equal(guid, &GPT_GUID_LINUX_XBOOT))       return "Linux Extended Boot";
    if (gpt_guid_equal(guid, &GPT_GUID_NETBSD_SWAP))       return "NetBSD Swap";
    if (gpt_guid_equal(guid, &GPT_GUID_NETBSD_FFS))        return "NetBSD FFS";
    if (gpt_guid_equal(guid, &GPT_GUID_OPENBSD_DATA))      return "OpenBSD Data";
    return "Unknown";
}

/* ---- GUID Write / String / Generate ---- */

void write_guid(const struct gpt_guid *g, uint8_t *p)
{
    int i;
    /* Mixed endian: data1 (LE32), data2 (LE16), data3 (LE16), data4 (bytes) */
    write_le32(p, g->data1);
    write_le16(p + 4, g->data2);
    write_le16(p + 6, g->data3);
    for (i = 0; i < 8; i++)
        p[8 + i] = g->data4[i];
}

/* Convert a nibble (0–15) to a lowercase hex character */
static char hex_nibble(uint8_t n)
{
    return (n < 10) ? ('0' + (char)n) : ('a' + (char)(n - 10));
}

/* Convert a byte to two hex characters */
static void hex_byte(uint8_t b, char *out)
{
    out[0] = hex_nibble((b >> 4) & 0x0F);
    out[1] = hex_nibble(b & 0x0F);
}

void guid_to_string(const struct gpt_guid *g, char *buf)
{
    uint8_t raw[16];
    int i, pos;
    /* Serialize to raw bytes (mixed endian) */
    write_guid(g, raw);

    /* Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars + NUL) */
    pos = 0;
    for (i = 0; i < 4; i++) { hex_byte(raw[i], buf + pos); pos += 2; }
    buf[pos++] = '-';
    for (i = 4; i < 6; i++) { hex_byte(raw[i], buf + pos); pos += 2; }
    buf[pos++] = '-';
    for (i = 6; i < 8; i++) { hex_byte(raw[i], buf + pos); pos += 2; }
    buf[pos++] = '-';
    for (i = 8; i < 10; i++) { hex_byte(raw[i], buf + pos); pos += 2; }
    buf[pos++] = '-';
    for (i = 10; i < 16; i++) { hex_byte(raw[i], buf + pos); pos += 2; }
    buf[pos] = '\0';
}

/* Parse a single hex character. Returns 0–15 or -1 on error. */
static int parse_hex_char(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int guid_from_string(const char *str, struct gpt_guid *g)
{
    uint8_t raw[16];
    int byte_idx = 0;
    int i = 0;

    /* Parse 32 hex digits with dashes at positions 8, 13, 18, 23 */
    while (byte_idx < 16) {
        int hi, lo;
        /* Skip dashes */
        if (str[i] == '-') { i++; continue; }
        if (str[i] == '\0') return -1;

        hi = parse_hex_char(str[i++]);
        if (hi < 0 || str[i] == '\0') return -1;
        lo = parse_hex_char(str[i++]);
        if (lo < 0) return -1;

        raw[byte_idx++] = (uint8_t)((hi << 4) | lo);
    }

    /* Decode raw bytes back to struct (mixed endian) */
    read_guid(raw, g);
    return 0;
}

/* ---- GUID v4 Generation (random) ---- */

/* GUID randomness comes from the kernel CSPRNG (csprng_fill, seeded at
 * Phase 1); RDRAND + XorShift survive only as the pre-seed fallback.
 * XREF: 16-architecture-ports/TODO-01-arch-abstraction-layer.md -- HAL random source */
#include "kernel/csprng.h"
#include "kernel/random.h"

/* XorShift64 PRNG fallback seeded from TSC */
static uint64_t prng_state;

static uint64_t xorshift64(void)
{
    uint64_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prng_state = x;
    return x;
}

static void prng_fill(uint8_t *buf, int n)
{
    int i;
    /* Seed from TSC if not yet seeded */
    if (prng_state == 0) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        prng_state = ((uint64_t)hi << 32) | lo;
        if (prng_state == 0) prng_state = 0xDEADBEEFCAFEBABEULL;
    }

    for (i = 0; i < n; i += 8) {
        uint64_t val = xorshift64();
        int j;
        int remain = n - i;
        if (remain > 8) remain = 8;
        for (j = 0; j < remain; j++)
            buf[i + j] = (uint8_t)(val >> (j * 8));
    }
}

struct gpt_guid guid_generate(void)
{
    struct gpt_guid g;
    uint8_t raw[16];

    /* Kernel CSPRNG first (seeded at Phase 1 by early_entropy_init,
     * long before any GPT write path): never blocks, and on RDRAND-less
     * systems it still mixes firmware/TPM/jitter sources instead of the
     * predictable TSC XorShift. The RDRAND + XorShift chain survives
     * ONLY as the pre-seed defensive fallback. */
    if (csprng_is_seeded())
        csprng_fill(raw, sizeof(raw));
    else if (!rdrand_bytes(raw, 16))
        prng_fill(raw, 16);

    /* Set version 4 (random): bits 48–51 = 0100 */
    raw[6] = (raw[6] & 0x0F) | 0x40;
    /* Set variant 2 (RFC 4122): bits 64–65 = 10 */
    raw[8] = (raw[8] & 0x3F) | 0x80;

    /* Decode as mixed-endian GUID */
    read_guid(raw, &g);
    return g;
}

/* ---- GPT Header Parsing ---- */

static int parse_header(const uint8_t *buf, struct gpt_header *hdr,
                        uint32_t sector_size)
{
    uint32_t stored_crc, computed_crc;
    uint8_t tmp[512];  /* CRC buffer -- header is always ≤92 bytes */
    int i;

    /* Read fields from the raw sector buffer */
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
    if (hdr->header_size > sector_size || hdr->header_size < 92) {
        serial_write("[GPT] Invalid header size\n");
        return -1;
    }

    for (i = 0; i < (int)hdr->header_size && i < 512; i++)
        tmp[i] = buf[i];
    /* Zero the CRC field (bytes 16–19) */
    tmp[16] = 0; tmp[17] = 0; tmp[18] = 0; tmp[19] = 0;

    stored_crc = hdr->header_crc32;
    computed_crc = gpt_crc32(tmp, hdr->header_size);

    if (computed_crc != stored_crc) {
        klog(LOG_DEBUG, "blk", "Header CRC32 mismatch: stored=0x%x computed=0x%x",
               (uint64_t)stored_crc, (uint64_t)computed_crc);
        return -1;
    }

    return 0;
}

/* Serialize a gpt_header back to a raw sector buffer.
 * Computes and fills in the Header CRC32 automatically.
 * 'buf' must be at least 'sector_size' bytes. */
static void serialize_header(const struct gpt_header *hdr, uint8_t *buf,
                             uint32_t sector_size)
{
    uint32_t i;
    uint32_t crc;

    /* Zero the entire sector */
    for (i = 0; i < sector_size; i++)
        buf[i] = 0;

    /* Write all 13 fields at their spec offsets */
    write_le64(buf + 0,  hdr->signature);
    write_le32(buf + 8,  hdr->revision);
    write_le32(buf + 12, hdr->header_size);
    /* bytes 16-19 (CRC32) left as zero for now */
    write_le32(buf + 20, hdr->reserved);
    write_le64(buf + 24, hdr->my_lba);
    write_le64(buf + 32, hdr->alt_lba);
    write_le64(buf + 40, hdr->first_usable_lba);
    write_le64(buf + 48, hdr->last_usable_lba);
    /* Disk GUID (mixed endian) */
    write_le32(buf + 56, hdr->disk_guid.data1);
    write_le16(buf + 60, hdr->disk_guid.data2);
    write_le16(buf + 62, hdr->disk_guid.data3);
    for (i = 0; i < 8; i++)
        buf[64 + i] = hdr->disk_guid.data4[i];
    write_le64(buf + 72, hdr->part_entry_lba);
    write_le32(buf + 80, hdr->num_part_entries);
    write_le32(buf + 84, hdr->part_entry_size);
    write_le32(buf + 88, hdr->part_entry_crc32);

    /* Compute Header CRC32 with CRC field zeroed (already zero) */
    crc = gpt_crc32(buf, hdr->header_size);
    write_le32(buf + 16, crc);
}

/* ---- Backup Sync on Write ---- */

int gpt_sync_backup(const struct blkdev *dev,
                    const struct gpt_header *primary_hdr,
                    const void *entry_array, uint32_t entry_array_bytes)
{
    uint32_t sec_sz = dev->sector_size;
    uint64_t backup_hdr_lba;
    uint64_t backup_entry_lba;
    uint32_t entry_sectors;
    struct gpt_header backup_hdr;
    uint8_t *hdr_buf;
    uint32_t si;

    if (sec_sz < 512)
        sec_sz = 512;

    if (dev->sector_count == 0)
        return -1;

    /* Backup header is at the last LBA */
    backup_hdr_lba = dev->sector_count - 1;

    /* Entry array sectors */
    entry_sectors = (entry_array_bytes + sec_sz - 1) / sec_sz;

    /* Backup entry array lives BEFORE the backup header.
     * backup_entry_lba = backup_hdr_lba - entry_sectors */
    backup_entry_lba = backup_hdr_lba - entry_sectors;

    /* Step 1: Write entry array to backup location first (crash safety).
     * If we crash after entries but before header, the old backup header
     * CRC will mismatch, so the stale header is safely rejected. */
    {
        const uint8_t *src = (const uint8_t *)entry_array;
        for (si = 0; si < entry_sectors; si++) {
            if (blkdev_write(dev, backup_entry_lba + si, 1,
                             src + si * sec_sz) != 0) {
                serial_write("[GPT] WARNING: Failed to write backup"
                             " entry array\n");
                return -1;
            }
        }
    }

    /* Step 2: Build backup header from primary with swapped LBAs */
    backup_hdr = *primary_hdr;
    backup_hdr.my_lba  = backup_hdr_lba;
    backup_hdr.alt_lba = GPT_HEADER_LBA;  /* Primary is at LBA 1 */
    backup_hdr.part_entry_lba = backup_entry_lba;
    /* part_entry_crc32 remains the same -- same array data */

    /* Step 3: Serialize backup header (recomputes Header CRC32) */
    hdr_buf = (uint8_t *)kmalloc(sec_sz);
    if (!hdr_buf) {
        serial_write("[GPT] WARNING: Failed to allocate backup"
                     " header buffer\n");
        return -1;
    }

    serialize_header(&backup_hdr, hdr_buf, sec_sz);

    /* Step 4: Write backup header to last LBA */
    if (blkdev_write(dev, backup_hdr_lba, 1, hdr_buf) != 0) {
        serial_write("[GPT] WARNING: Failed to write backup header\n");
        kfree(hdr_buf);
        return -1;
    }

    kfree(hdr_buf);
    serial_write("[GPT] Synced backup GPT\n");
    return 0;
}
/* ---- Main Parser ---- */

struct gpt_table gpt_parse(const struct blkdev *dev, const void *sector0)
{
    struct gpt_table tbl;
    const uint8_t *mbr_buf = (const uint8_t *)sector0;
    struct gpt_header hdr;
    uint8_t *hdr_sect = NULL;
    uint8_t *entry_buf = NULL;
    uint32_t sec_sz;
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

    /*: Use device sector size instead of assuming 512 */
    sec_sz = dev->sector_size;
    if (sec_sz < 512)
        sec_sz = 512;  /* Minimum per UEFI spec */

    /* Allocate sector-sized I/O buffers */
    hdr_sect = (uint8_t *)kmalloc(sec_sz);
    entry_buf = (uint8_t *)kmalloc(sec_sz);
    if (!hdr_sect || !entry_buf) {
        serial_write("[GPT] Failed to allocate sector buffers\n");
        if (hdr_sect) kfree(hdr_sect);
        if (entry_buf) kfree(entry_buf);
        return tbl;
    }

    /* Step 1: Verify protective MBR has type 0xEE */
    {
        int has_ee = 0;
        int ee_slot = -1;
        for (i = 0; i < 4; i++) {
            uint8_t ptype = mbr_buf[MBR_ENTRY_OFFSET + i * MBR_ENTRY_SIZE + 4];
            if (ptype == MBR_TYPE_GPT) {
                has_ee = 1;
                ee_slot = i;
                break;
            }
        }
        if (!has_ee) {
            serial_write("[GPT] No protective MBR (0xEE) found\n");
            goto out_free;
        }

        /* Hybrid MBR Detection:
         * Standard PMBR: 0xEE entry spans entire disk, slots 2-4 are empty.
         * Hybrid MBR: 0xEE is partial AND other non-zero entries exist.
         * Always prefer GPT -- log warning for hybrid. */
        {
            const uint8_t *ee_entry = mbr_buf + MBR_ENTRY_OFFSET
                                      + ee_slot * MBR_ENTRY_SIZE;
            uint32_t ee_size_lba = read_le32(ee_entry + 12);
            int other_entries = 0;

            /* Check other MBR slots for non-zero type */
            for (i = 0; i < 4; i++) {
                if (i == ee_slot) continue;
                uint8_t ptype = mbr_buf[MBR_ENTRY_OFFSET
                                        + i * MBR_ENTRY_SIZE + 4];
                if (ptype != 0x00)
                    other_entries++;
            }

            /* Partial 0xEE + other entries = Hybrid MBR */
            if (other_entries > 0 && dev->sector_count > 0 &&
                (uint64_t)ee_size_lba < dev->sector_count - 1) {
                tbl.hybrid_mbr = 1;
                serial_write("[GPT] WARNING: Hybrid MBR detected"
                             " -- ignoring legacy MBR entries\n");
            }
        }
    }

    /* Step 2: Read GPT header at LBA 1 */
    if (blkdev_read(dev, GPT_HEADER_LBA, 1, hdr_sect) != 0) {
        serial_write("[GPT] Failed to read LBA 1\n");
        goto out_free;
    }

    /* Step 3: Parse and validate header (signature + CRC32) */
    primary_ok = (parse_header(hdr_sect, &hdr, sec_sz) == 0);

    /* Backup Header Fallback: if primary fails, try backup at last LBA */
    if (!primary_ok) {
        if (dev->sector_count == 0) {
            serial_write("[GPT] Primary header corrupt, no sector count for backup\n");
            goto out_free;
        }
        backup_lba = dev->sector_count - 1;
        if (blkdev_read(dev, backup_lba, 1, hdr_sect) != 0) {
            serial_write("[GPT] Failed to read backup header\n");
            goto out_free;
        }
        if (parse_header(hdr_sect, &hdr, sec_sz) != 0) {
            serial_write("[GPT] Both primary and backup headers corrupt\n");
            goto out_free;
        }
        /* Verify backup header's self-referencing fields */
        if (hdr.my_lba != backup_lba) {
            serial_write("[GPT] Backup header my_lba mismatch\n");
            goto out_free;
        }
        if (hdr.alt_lba != GPT_HEADER_LBA) {
            serial_write("[GPT] Backup header alt_lba != 1\n");
            goto out_free;
        }
        klog(LOG_WARN, "blk",
             "Primary GPT header corrupt - using backup at LBA %u",
             (uint64_t)backup_lba);
        using_backup = 1;
    }

    /* Sanity-check entry parameters (applies to whichever header we used) */
    if (hdr.part_entry_size < 128 || hdr.num_part_entries == 0) {
        serial_write("[GPT] Invalid partition entry params\n");
        goto out_free;
    }

    /* Step 4: Read and validate partition entry array CRC32.
     * We read sector-by-sector, computing CRC32 incrementally. */
    total_entries = hdr.num_part_entries;
    if (total_entries > GPT_MAX_PARTITIONS)
        total_entries = GPT_MAX_PARTITIONS;

    total_entry_bytes = total_entries * hdr.part_entry_size;
    entries_per_sector = sec_sz / hdr.part_entry_size;
    sectors_needed = (total_entry_bytes + sec_sz - 1) / sec_sz;
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
                goto out_free;
            }

            chunk = bytes_remaining > sec_sz ? sec_sz : bytes_remaining;
            for (i = 0; i < (int)chunk; i++)
                crc = (crc >> 8) ^ crc32_table[(crc ^ entry_buf[i]) & 0xFF];
            bytes_remaining -= chunk;
        }
        entry_crc = crc ^ 0xFFFFFFFF;
    }

    if (entry_crc != hdr.part_entry_crc32) {
        klog(LOG_DEBUG, "blk", "Entry array CRC32 mismatch: stored=0x%x computed=0x%x",
               (uint64_t)hdr.part_entry_crc32, (uint64_t)entry_crc);
        goto out_free;
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

            /* Read type GUID -- skip empty entries */
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

            /* Log attribute flags for notable partitions */
            if (p->attributes & (GPT_ATTR_REQUIRED |
                                 GPT_ATTR_LEGACY_BIOS_BOOT)) {
                serial_write("[GPT] Partition ");
                serial_write(p->name[0] ? p->name : "(unnamed)");
                serial_write(": Required=");
                serial_write((p->attributes & GPT_ATTR_REQUIRED)
                              ? "1" : "0");
                serial_write(", BIOSBoot=");
                serial_write((p->attributes & GPT_ATTR_LEGACY_BIOS_BOOT)
                              ? "1" : "0");
                serial_write("\n");
            }

            tbl.count++;
        }
    }

    /* Auto-recover primary header from valid backup */
    if (using_backup) {
        struct gpt_header primary_hdr;
        uint64_t primary_entry_lba = GPT_HEADER_LBA + 1; /* LBA 2 */
        uint64_t backup_entry_lba = hdr.part_entry_lba;
        int recovery_ok = 1;

        /* Build reconstructed primary header from backup */
        primary_hdr = hdr;
        primary_hdr.my_lba  = GPT_HEADER_LBA;   /* 1 */
        primary_hdr.alt_lba = backup_lba;        /* last LBA */
        primary_hdr.part_entry_lba = primary_entry_lba; /* LBA 2 */

        /* Serialize header into hdr_sect buffer (reused, sec_sz bytes) */
        serialize_header(&primary_hdr, hdr_sect, sec_sz);

        /* Write reconstructed primary header to LBA 1 */
        if (blkdev_write(dev, GPT_HEADER_LBA, 1, hdr_sect) != 0) {
            klog(LOG_WARN, "blk", "Failed to write recovered primary GPT header");
            recovery_ok = 0;
        }

        /* Copy backup entry array to primary location (LBA 2+) */
        if (recovery_ok) {
            for (si = 0; si < sectors_needed; si++) {
                if (blkdev_read(dev, backup_entry_lba + si, 1, entry_buf) != 0) {
                    klog(LOG_WARN, "blk",
                         "Failed to read backup entry sector %u",
                         (uint64_t)si);
                    recovery_ok = 0;
                    break;
                }
                if (blkdev_write(dev, primary_entry_lba + si, 1, entry_buf) != 0) {
                    klog(LOG_WARN, "blk",
                         "Failed to write primary entry sector %u",
                         (uint64_t)si);
                    recovery_ok = 0;
                    break;
                }
            }
        }

        /* Verify: re-read primary and check CRC matches */
        if (recovery_ok) {
            uint32_t verify_crc = 0xFFFFFFFF;
            uint32_t vbytes = total_entry_bytes;

            for (si = 0; si < sectors_needed; si++) {
                uint32_t chunk;
                if (blkdev_read(dev, primary_entry_lba + si, 1, entry_buf) != 0) {
                    recovery_ok = 0;
                    break;
                }
                chunk = vbytes > sec_sz ? sec_sz : vbytes;
                for (i = 0; i < (int)chunk; i++)
                    verify_crc = (verify_crc >> 8)
                               ^ crc32_table[(verify_crc ^ entry_buf[i]) & 0xFF];
                vbytes -= chunk;
            }
            verify_crc ^= 0xFFFFFFFF;

            if (recovery_ok && verify_crc != hdr.part_entry_crc32) {
                klog(LOG_WARN, "blk",
                     "Primary entry array CRC32 verify failed after recovery");
                recovery_ok = 0;
            }
        }

        if (recovery_ok) {
            klog(LOG_INFO, "blk",
                 "Auto-recovered primary GPT header from backup");
        } else {
            klog(LOG_WARN, "blk",
                 "Primary GPT recovery attempted but failed - backup still valid");
        }
    }

out_free:
    kfree(hdr_sect);
    kfree(entry_buf);
    return tbl;
}
