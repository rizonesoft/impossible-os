/* ============================================================================
 * ntfs_core.c — NTFS Boot Sector / BPB Parsing
 *
 * Reads the first sector (LBA 0) of an NTFS partition, validates the
 * OEM ID ("NTFS    "), extracts all critical BPB fields, and locates
 * the $MFT.  This is the foundation for all subsequent NTFS operations.
 *
 * Reference: NTFS Documentation (unofficial), Microsoft NTFS.sys behavior.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ---- Little-endian field readers ---- */

static uint16_t ntfs_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ntfs_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t ntfs_le64(const uint8_t *p)
{
    return (uint64_t)ntfs_le32(p) | ((uint64_t)ntfs_le32(p + 4) << 32);
}

/* ---- String comparison (8 bytes, no libc) ---- */

static int ntfs_memcmp(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

/* ---- Probe: check if sector 0 has NTFS OEM ID ---- */

/* Expected OEM ID at offset 0x03: "NTFS    " (4 letters + 4 spaces) */
static const uint8_t ntfs_oem_id[8] = { 'N','T','F','S',' ',' ',' ',' ' };

int ntfs_probe(const uint8_t *sector)
{
    /* Check OEM ID at offset 0x03 */
    if (ntfs_memcmp(sector + 0x03, ntfs_oem_id, 8) != 0)
        return 0;

    /* Validate boot signature 0x55AA at offset 0x1FE */
    if (sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA)
        return 0;

    return 1;
}

/* ---- Decode the Clusters-Per-FRS / Clusters-Per-Index quirk ----
 * If the 1-byte signed value at offset 0x40 (or 0x44) is positive,
 * the record size = value × cluster_size.
 * If negative, the record size = 2^|value| (e.g., 0xF6 = −10 → 1024). */

static uint32_t decode_record_size(int8_t raw, uint32_t cluster_size)
{
    if (raw > 0) {
        return (uint32_t)raw * cluster_size;
    }
    /* Negative: 2^|raw| */
    int shift = -raw;
    if (shift > 31)
        shift = 31; /* Sanity cap */
    return (uint32_t)1 << shift;
}

/* ---- Public API ---- */

struct ntfs_volume *ntfs_init(const struct blkdev *dev)
{
    uint8_t sector[512];
    struct ntfs_volume *vol;
    uint16_t bps;
    uint8_t  spc;
    int8_t   clusters_per_frs_raw;
    int8_t   clusters_per_index_raw;

    if (!dev) {
        klog(LOG_DEBUG, "ntfs", "ntfs_init: NULL device");
        return NULL;
    }

    /* Read sector 0 (boot sector) */
    if (blkdev_read(dev, 0, 1, sector) != 0) {
        klog(LOG_DEBUG, "ntfs", "Failed to read boot sector");
        return NULL;
    }

    /* Validate OEM ID at offset 0x03 */
    if (ntfs_memcmp(sector + 0x03, ntfs_oem_id, 8) != 0) {
        klog(LOG_DEBUG, "ntfs", "OEM ID mismatch (not NTFS)");
        return NULL;
    }

    /* Validate boot signature */
    if (sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA) {
        klog(LOG_DEBUG, "ntfs", "Invalid boot signature");
        return NULL;
    }

    /* Extract BPB fields */
    bps = ntfs_le16(sector + 0x0B);
    spc = sector[0x0D];

    /* Sanity checks */
    if (bps == 0 || (bps & (bps - 1)) != 0) {
        klog(LOG_DEBUG, "ntfs", "Invalid bytes_per_sector: %u", (uint64_t)bps);
        return NULL;
    }
    if (spc == 0 || (spc & (spc - 1)) != 0) {
        klog(LOG_DEBUG, "ntfs", "Invalid sectors_per_cluster: %u", (uint64_t)spc);
        return NULL;
    }

    /* Allocate volume context (< 100 bytes — safe for kmalloc) */
    vol = (struct ntfs_volume *)kmalloc(sizeof(struct ntfs_volume));
    if (!vol) {
        klog(LOG_DEBUG, "ntfs", "Failed to allocate ntfs_volume");
        return NULL;
    }

    vol->dev = dev;
    vol->bytes_per_sector = bps;
    vol->sectors_per_cluster = spc;
    vol->cluster_size = (uint32_t)bps * (uint32_t)spc;

    /* Total sectors (64-bit at offset 0x28) */
    vol->total_sectors = ntfs_le64(sector + 0x28);

    /* MFT location */
    vol->mft_lcn     = ntfs_le64(sector + 0x30);
    vol->mftmirr_lcn = ntfs_le64(sector + 0x38);

    /* File Record Segment size (offset 0x40, signed 1-byte) */
    clusters_per_frs_raw = (int8_t)sector[0x40];
    vol->frs_size = decode_record_size(clusters_per_frs_raw, vol->cluster_size);

    /* Index record size (offset 0x44, same signed encoding) */
    clusters_per_index_raw = (int8_t)sector[0x44];
    vol->index_size = decode_record_size(clusters_per_index_raw, vol->cluster_size);

    /* Volume serial number (offset 0x48, 8 bytes LE) */
    vol->volume_serial = ntfs_le64(sector + 0x48);

    /* MFT byte offset = LCN × cluster_size */
    vol->mft_byte_offset = vol->mft_lcn * (uint64_t)vol->cluster_size;

    /* Log volume information */
    klog(LOG_INFO, "ntfs",
         "Volume: %u sectors, cluster=%u bytes, MFT at LCN %u (byte 0x%x)",
         vol->total_sectors, (uint64_t)vol->cluster_size,
         vol->mft_lcn, vol->mft_byte_offset);

    klog(LOG_INFO, "ntfs",
         "FRS size=%u bytes, INDX size=%u bytes, serial=0x%x",
         (uint64_t)vol->frs_size, (uint64_t)vol->index_size,
         vol->volume_serial);

    return vol;
}
