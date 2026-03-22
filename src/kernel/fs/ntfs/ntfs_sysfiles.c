/* ============================================================================
 * ntfs_sysfiles.c — System Metafile Readers (§7.1)
 *
 * Reads key NTFS system files needed for full operation:
 *   $Volume   (inode 3)  — Volume label, NTFS version, dirty flag
 *   $Bitmap   (inode 6)  — Cluster allocation bitmap, free space counting
 *   $UpCase   (inode 10) — Unicode uppercase mapping for case-insensitive ops
 *   $MFTMirr  (inode 1)  — Mirror of first 4 MFT records for integrity check
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ============================================================================
 * $Volume Reader (inode 3)
 *
 * Extracts:
 *   - $VOLUME_NAME (attr 0x60): UTF-16LE volume label → ASCII in vol->volume_name
 *   - $VOLUME_INFORMATION (attr 0x70): NTFS version + flags
 *     Flags bit 0: volume is dirty (not cleanly unmounted)
 *     Flags bit 1: resize log needed (after volume resize)
 * ============================================================================ */

/* $VOLUME_NAME attribute type */
#define NTFS_ATTR_VOLUME_NAME  0x60

/* $VOLUME_INFORMATION attribute type */
#define NTFS_ATTR_VOLUME_INFO  0x70

/* Volume flags */
#define NTFS_VOLUME_FLAG_DIRTY     0x0001
#define NTFS_VOLUME_FLAG_RESIZE    0x0002
#define NTFS_VOLUME_FLAG_UPGRADE   0x0004
#define NTFS_VOLUME_FLAG_CHKDSK    0x8000

static int read_volume_info(struct ntfs_volume *vol)
{
    uintptr_t rec_phys;
    uint8_t *rec_buf;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    int rc;

    rec_phys = pmm_alloc_contiguous(1);
    if (!rec_phys)
        return NTFS_ERR_IO;
    rec_buf = (uint8_t *)(uintptr_t)rec_phys;

    rc = ntfs_read_mft_record(vol, NTFS_INODE_VOLUME, rec_buf, &hdr);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        return rc;
    }

    /* ---- Read $VOLUME_NAME (0x60) ---- */
    attr = ntfs_attr_find(rec_buf, &hdr, NTFS_ATTR_VOLUME_NAME, &ah);
    if (attr && ah.non_resident == 0 && ah.content_length > 0) {
        const uint8_t *name_data = attr + ah.content_offset;
        uint32_t name_bytes = ah.content_length;
        uint32_t name_chars = name_bytes / 2;
        uint32_t i;
        uint32_t max_chars = 127;

        if (name_chars > max_chars)
            name_chars = max_chars;

        for (i = 0; i < name_chars; i++) {
            uint16_t wc = ntfs_le16(name_data + i * 2);
            vol->volume_name[i] = (wc < 0x80) ? (char)wc : '?';
        }
        vol->volume_name[name_chars] = '\0';

        klog(LOG_INFO, "ntfs", "Volume label: \"%s\"",
             (uint64_t)(uintptr_t)vol->volume_name, 0, 0);
    } else {
        vol->volume_name[0] = '\0';
    }

    /* ---- Read $VOLUME_INFORMATION (0x70) ---- */
    attr = ntfs_attr_find(rec_buf, &hdr, NTFS_ATTR_VOLUME_INFO, &ah);
    if (attr && ah.non_resident == 0 && ah.content_length >= 12) {
        const uint8_t *vi = attr + ah.content_offset;
        /* Layout: [reserved:8][major:1][minor:1][flags:2] */
        vol->ntfs_version_major = vi[8];
        vol->ntfs_version_minor = vi[9];
        {
            uint16_t vflags = ntfs_le16(vi + 10);

            klog(LOG_INFO, "ntfs", "NTFS version %u.%u, flags 0x%04x",
                 (uint64_t)vol->ntfs_version_major,
                 (uint64_t)vol->ntfs_version_minor,
                 (uint64_t)vflags);

            if (vflags & NTFS_VOLUME_FLAG_DIRTY) {
                vol->volume_dirty = 1;
                klog(LOG_WARN, "ntfs",
                     "WARNING: Volume was not cleanly unmounted (dirty flag set)");
            }
            if (vflags & NTFS_VOLUME_FLAG_CHKDSK) {
                klog(LOG_WARN, "ntfs",
                     "WARNING: Volume needs chkdsk (flag 0x8000 set)");
            }
        }
    }

    pmm_free_frame(rec_phys);
    return NTFS_OK;
}

/* ============================================================================
 * $Bitmap Reader (inode 6)
 *
 * The cluster allocation bitmap tracks which clusters are in use.
 * Each bit = 1 cluster: bit set = in use, bit clear = free.
 * The bitmap data is a non-resident $DATA attribute of $Bitmap (inode 6).
 *
 * This reader counts free clusters. The bitmap data runs are already loaded
 * by ntfs_bitmap_load() (§12.1) — this just does the free space counting.
 * ============================================================================ */

static int read_bitmap_free_space(struct ntfs_volume *vol)
{
    uint64_t free_count = 0;
    uint64_t total_bytes;
    uint64_t byte_pos = 0;
    int ri;

    if (!vol->bitmap_loaded || !vol->bitmap_runs ||
        vol->bitmap_run_count <= 0) {
        klog(LOG_WARN, "ntfs",
             "Bitmap not loaded — cannot count free clusters");
        return NTFS_ERR_IO;
    }

    total_bytes = vol->bitmap_size;
    if (total_bytes == 0)
        total_bytes = (vol->total_clusters + 7) / 8;

    /* Read bitmap data one sector at a time, counting free bits */
    for (ri = 0; ri < vol->bitmap_run_count && byte_pos < total_bytes; ri++) {
        uint64_t run_bytes;
        uint64_t run_lba;
        uint32_t sectors_in_run;
        uint32_t si;
        uint8_t sector_buf[512];

        if (vol->bitmap_runs[ri].lcn == NTFS_LCN_SPARSE) {
            /* Sparse run — all zeros means all free */
            uint64_t bits = vol->bitmap_runs[ri].length *
                            vol->cluster_size * 8;
            free_count += bits;
            byte_pos += vol->bitmap_runs[ri].length * vol->cluster_size;
            continue;
        }

        run_bytes = vol->bitmap_runs[ri].length * vol->cluster_size;
        run_lba = vol->bitmap_runs[ri].lcn * vol->sectors_per_cluster;
        sectors_in_run = (uint32_t)(run_bytes / vol->bytes_per_sector);

        for (si = 0; si < sectors_in_run && byte_pos < total_bytes; si++) {
            uint32_t bytes_in_sector;
            uint32_t bi;

            if (blkdev_read(vol->dev, run_lba + si, 1, sector_buf) != 0)
                continue;

            bytes_in_sector = vol->bytes_per_sector;
            if (byte_pos + bytes_in_sector > total_bytes)
                bytes_in_sector = (uint32_t)(total_bytes - byte_pos);

            for (bi = 0; bi < bytes_in_sector; bi++) {
                uint8_t byte = sector_buf[bi];
                uint64_t bit_offset = (byte_pos + bi) * 8;
                int bit;

                for (bit = 0; bit < 8; bit++) {
                    if (bit_offset + (uint64_t)bit >= vol->total_clusters)
                        goto done_counting;
                    if (!(byte & (1u << bit)))
                        free_count++;
                }
            }

            byte_pos += bytes_in_sector;
        }
    }

done_counting:
    vol->free_clusters = free_count;

    klog(LOG_INFO, "ntfs", "Free clusters: %llu / %llu (%llu MB free)",
         free_count, vol->total_clusters,
         (free_count * vol->cluster_size) / (1024 * 1024));

    return NTFS_OK;
}

/* ============================================================================
 * $UpCase Reader (inode 10)
 *
 * $UpCase contains a 128 KB table mapping each Unicode code point (0-65535)
 * to its uppercase equivalent. Used for case-insensitive filename comparison
 * in B+ tree lookups and directory searches.
 *
 * Table format: 65536 entries × 2 bytes = 131072 bytes (128 KB).
 * Each entry at index N contains the uppercase equivalent of U+N.
 * ============================================================================ */

#define NTFS_UPCASE_TABLE_SIZE  (65536 * 2)  /* 128 KB */
#define NTFS_UPCASE_ENTRIES     65536

static int read_upcase_table(struct ntfs_volume *vol)
{
    uintptr_t rec_phys;
    uint8_t *rec_buf;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    int rc;

    rec_phys = pmm_alloc_contiguous(1);
    if (!rec_phys)
        return NTFS_ERR_IO;
    rec_buf = (uint8_t *)(uintptr_t)rec_phys;

    rc = ntfs_read_mft_record(vol, NTFS_INODE_UPCASE, rec_buf, &hdr);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        klog(LOG_WARN, "ntfs",
             "$UpCase read failed — using ASCII fallback");
        return rc;
    }

    /* Find $DATA attribute */
    attr = ntfs_attr_find(rec_buf, &hdr, NTFS_ATTR_DATA, &ah);
    if (!attr) {
        pmm_free_frame(rec_phys);
        klog(LOG_WARN, "ntfs", "$UpCase: no $DATA attribute");
        return NTFS_ERR_BAD_MAGIC;
    }

    if (ah.non_resident == 0) {
        /* Resident $UpCase — very unlikely but handle it */
        if (ah.content_length < NTFS_UPCASE_TABLE_SIZE) {
            pmm_free_frame(rec_phys);
            klog(LOG_WARN, "ntfs", "$UpCase too small: %u bytes",
                 (uint64_t)ah.content_length, 0, 0);
            return NTFS_ERR_BAD_MAGIC;
        }

        {
            /* Allocate 128 KB for the table (32 pages) */
            uint32_t pages = (NTFS_UPCASE_TABLE_SIZE + 4095) / 4096;
            uintptr_t table_phys = pmm_alloc_contiguous(pages);
            if (!table_phys) {
                pmm_free_frame(rec_phys);
                return NTFS_ERR_IO;
            }
            ntfs_memcpy((uint8_t *)(uintptr_t)table_phys,
                        attr + ah.content_offset, NTFS_UPCASE_TABLE_SIZE);
            vol->upcase_table = (uint16_t *)(uintptr_t)table_phys;
        }
    } else {
        /* Non-resident $UpCase — typical case (128 KB on disk) */
        struct ntfs_data_run runs[32];
        struct ntfs_nonres_header nrhdr;
        int run_count;
        int64_t bytes;
        uint32_t pages;
        uintptr_t table_phys;

        run_count = ntfs_decode_data_runs(attr, runs, 32, &nrhdr);
        if (run_count <= 0 || nrhdr.real_size < NTFS_UPCASE_TABLE_SIZE) {
            pmm_free_frame(rec_phys);
            klog(LOG_WARN, "ntfs", "$UpCase: bad data runs");
            return NTFS_ERR_BAD_MAGIC;
        }

        /* Allocate 128 KB for the table */
        pages = (NTFS_UPCASE_TABLE_SIZE + 4095) / 4096;
        table_phys = pmm_alloc_contiguous(pages);
        if (!table_phys) {
            pmm_free_frame(rec_phys);
            return NTFS_ERR_IO;
        }

        /* Read from data runs */
        bytes = ntfs_read_data(vol, runs, run_count, nrhdr.real_size,
                                0, NTFS_UPCASE_TABLE_SIZE,
                                (uint8_t *)(uintptr_t)table_phys);
        if (bytes != NTFS_UPCASE_TABLE_SIZE) {
            pmm_free_frame(table_phys);
            pmm_free_frame(rec_phys);
            klog(LOG_WARN, "ntfs", "$UpCase: read returned %lld bytes",
                 (uint64_t)bytes, 0, 0);
            return NTFS_ERR_IO;
        }

        vol->upcase_table = (uint16_t *)(uintptr_t)table_phys;
    }

    pmm_free_frame(rec_phys);

    klog(LOG_INFO, "ntfs", "$UpCase loaded: %u KB Unicode uppercase table",
         (uint64_t)(NTFS_UPCASE_TABLE_SIZE / 1024), 0, 0);

    return NTFS_OK;
}

/* ============================================================================
 * $MFTMirr Reader (inode 1)
 *
 * $MFTMirr is a backup of the first 4 MFT records ($MFT, $MFTMirr, $LogFile,
 * $Volume). On mount, compare these against the real $MFT records to detect
 * corruption. If they differ, log a warning — the user should run chkdsk.
 * ============================================================================ */

#define NTFS_MFTMIRR_COUNT  4  /* Number of mirrored records */

static int read_mftmirr_check(struct ntfs_volume *vol)
{
    uintptr_t rec_phys;
    uintptr_t mirr_phys;
    uint8_t *rec_buf;
    uint8_t *mirr_buf;
    struct ntfs_mft_header hdr;
    struct ntfs_mft_header mirr_hdr;
    int i;
    int mismatches = 0;
    int rc;

    rec_phys = pmm_alloc_contiguous(1);
    mirr_phys = pmm_alloc_contiguous(1);
    if (!rec_phys || !mirr_phys) {
        if (rec_phys) pmm_free_frame(rec_phys);
        if (mirr_phys) pmm_free_frame(mirr_phys);
        return NTFS_ERR_IO;
    }
    rec_buf = (uint8_t *)(uintptr_t)rec_phys;
    mirr_buf = (uint8_t *)(uintptr_t)mirr_phys;

    /* Read $MFTMirr's MFT record to get its data runs */
    rc = ntfs_read_mft_record(vol, NTFS_INODE_MFTMIRR, rec_buf, &hdr);
    if (rc != NTFS_OK) {
        klog(LOG_WARN, "ntfs", "$MFTMirr: cannot read inode 1");
        pmm_free_frame(rec_phys);
        pmm_free_frame(mirr_phys);
        return rc;
    }

    /* Get $MFTMirr's $DATA runs */
    {
        struct ntfs_attr_header ah;
        const uint8_t *attr;
        struct ntfs_data_run mirr_runs[16];
        struct ntfs_nonres_header nrhdr;
        int mirr_run_count;

        attr = ntfs_attr_find(rec_buf, &hdr, NTFS_ATTR_DATA, &ah);
        if (!attr || ah.non_resident == 0) {
            klog(LOG_WARN, "ntfs", "$MFTMirr: no non-resident $DATA");
            pmm_free_frame(rec_phys);
            pmm_free_frame(mirr_phys);
            return NTFS_ERR_BAD_MAGIC;
        }

        mirr_run_count = ntfs_decode_data_runs(attr, mirr_runs, 16, &nrhdr);
        if (mirr_run_count <= 0) {
            pmm_free_frame(rec_phys);
            pmm_free_frame(mirr_phys);
            return NTFS_ERR_BAD_MAGIC;
        }

        /* Compare each of the first 4 records */
        for (i = 0; i < NTFS_MFTMIRR_COUNT; i++) {
            int64_t bytes;

            /* Read mirrored record */
            bytes = ntfs_read_data(vol, mirr_runs, mirr_run_count,
                                    nrhdr.real_size,
                                    (uint64_t)i * vol->frs_size,
                                    vol->frs_size, mirr_buf);
            if (bytes != (int64_t)vol->frs_size) {
                klog(LOG_WARN, "ntfs",
                     "$MFTMirr: cannot read mirrored record %d", (uint64_t)i,
                     0, 0);
                mismatches++;
                continue;
            }

            /* Read original $MFT record (raw — compare pre-fixup bytes) */
            rc = ntfs_read_mft_record_raw(vol, (uint64_t)i, rec_buf, &mirr_hdr);
            if (rc != NTFS_OK) {
                mismatches++;
                continue;
            }

            /* Compare raw bytes (before fixup — both should have same USA) */
            if (ntfs_memcmp(rec_buf, mirr_buf, vol->frs_size) != 0) {
                klog(LOG_WARN, "ntfs",
                     "$MFTMirr mismatch: inode %d differs from mirror",
                     (uint64_t)i, 0, 0);
                mismatches++;
            }
        }
    }

    pmm_free_frame(rec_phys);
    pmm_free_frame(mirr_phys);

    if (mismatches > 0) {
        klog(LOG_WARN, "ntfs",
             "$MFTMirr: %d of %d records differ — volume may need chkdsk",
             (uint64_t)mismatches, (uint64_t)NTFS_MFTMIRR_COUNT, 0);
    } else {
        klog(LOG_INFO, "ntfs",
             "$MFTMirr: all %d mirrored records consistent",
             (uint64_t)NTFS_MFTMIRR_COUNT, 0, 0);
    }

    return (mismatches > 0) ? NTFS_ERR_BAD_MAGIC : NTFS_OK;
}

/* ============================================================================
 * ntfs_upcase_char — Case-insensitive character comparison helper
 *
 * Uses the loaded $UpCase table if available, falls back to ASCII.
 * ============================================================================ */

uint16_t ntfs_upcase_char(const struct ntfs_volume *vol, uint16_t ch)
{
    if (vol && vol->upcase_table)
        return vol->upcase_table[ch];

    /* ASCII fallback */
    if (ch >= 'a' && ch <= 'z')
        return ch - ('a' - 'A');
    return ch;
}

/* ============================================================================
 * Public API: Load all system metafiles
 *
 * Called during mount after ntfs_init() and ntfs_bitmap_load().
 * Loads $Volume, $Bitmap free space, $UpCase, and $MFTMirr.
 * Non-fatal individual failures are logged but don't abort the mount.
 * ============================================================================ */

int ntfs_load_sysfiles(struct ntfs_volume *vol)
{
    int rc;
    int errors = 0;

    if (!vol)
        return NTFS_ERR_IO;

    if (vol->sysfiles_loaded)
        return NTFS_OK;  /* Already loaded */

    klog(LOG_INFO, "ntfs", "Loading system metafiles...");

    /* $Volume — volume label, version, dirty flag */
    rc = read_volume_info(vol);
    if (rc != NTFS_OK) {
        klog(LOG_WARN, "ntfs", "$Volume read failed (rc=%d)",
             (uint64_t)rc, 0, 0);
        errors++;
    }

    /* $Bitmap — free space counting (requires bitmap_loaded from §12.1) */
    if (vol->bitmap_loaded) {
        rc = read_bitmap_free_space(vol);
        if (rc != NTFS_OK) {
            klog(LOG_WARN, "ntfs", "$Bitmap free space count failed (rc=%d)",
                 (uint64_t)rc, 0, 0);
            errors++;
        }
    }

    /* $UpCase — Unicode uppercase table */
    rc = read_upcase_table(vol);
    if (rc != NTFS_OK) {
        klog(LOG_WARN, "ntfs",
             "$UpCase load failed — using ASCII fallback (rc=%d)",
             (uint64_t)rc, 0, 0);
        vol->upcase_table = NULL;
        errors++;
    }

    /* $MFTMirr — consistency check (non-fatal) */
    rc = read_mftmirr_check(vol);
    if (rc != NTFS_OK) {
        /* Warning already logged by read_mftmirr_check */
        errors++;
    }

    vol->sysfiles_loaded = 1;

    if (errors > 0) {
        klog(LOG_WARN, "ntfs",
             "System metafiles loaded with %d warning(s)",
             (uint64_t)errors, 0, 0);
    } else {
        klog(LOG_INFO, "ntfs", "All system metafiles loaded successfully");
    }

    return NTFS_OK;  /* Non-fatal — mount proceeds */
}
