/* ============================================================================
 * ntfs_mft_alloc.c -- MFT Record Allocator (§12.3)
 *
 * Manages allocation and deallocation of MFT records (inodes).
 * Uses $MFT's own $BITMAP attribute (NOT inode 6/$Bitmap) to track
 * which MFT records are in use.
 *
 * Key operations:
 *   ntfs_mft_alloc_load()   -- Load $MFT bitmap + data runs at mount time
 *   ntfs_alloc_mft_record() -- Find free inode, init record, write to disk
 *   ntfs_free_mft_record()  -- Clear in-use flag, free bitmap bit
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* First 16 MFT records (0–15) are reserved for system metafiles.
 * User allocations start at inode 24 (with 16–23 as NTFS padding). */
#define NTFS_FIRST_USER_INODE  24

/* Maximum data runs we store for $MFT's own runs */
#define MFT_MAX_RUNS  256

/* ============================================================================
 * Internal helpers -- MFT bitmap I/O
 *
 * Identical pattern to ntfs_bitmap.c but operates on $MFT's $BITMAP
 * attribute (mft_bitmap_runs) instead of inode 6's $Bitmap.
 * ============================================================================ */

/* Map a byte offset within $MFT's $BITMAP to a disk LBA */
static int mft_bitmap_offset_to_lba(struct ntfs_volume *vol,
                                     uint64_t byte_offset,
                                     uint64_t *out_lba,
                                     uint32_t *out_sector_offset)
{
    uint64_t cluster_off = byte_offset / vol->cluster_size;
    uint64_t in_cluster  = byte_offset % vol->cluster_size;
    uint64_t vcn = 0;
    int i;

    for (i = 0; i < vol->mft_bitmap_run_count; i++) {
        uint64_t run_len = vol->mft_bitmap_runs[i].length;
        if (cluster_off < vcn + run_len) {
            uint64_t lcn = vol->mft_bitmap_runs[i].lcn;
            if (lcn == NTFS_LCN_SPARSE)
                return -1;
            uint64_t disk_cluster = lcn + (cluster_off - vcn);
            uint64_t disk_byte = disk_cluster * vol->cluster_size + in_cluster;
            *out_lba = disk_byte / vol->bytes_per_sector;
            *out_sector_offset = (uint32_t)(disk_byte % vol->bytes_per_sector);
            return 0;
        }
        vcn += run_len;
    }
    return -1;
}

/* Read one byte from $MFT's $BITMAP */
static int mft_bitmap_read_byte(struct ntfs_volume *vol, uint64_t byte_offset)
{
    uint64_t lba;
    uint32_t sector_off;
    uint8_t  sector_buf[512];

    if (byte_offset >= vol->mft_bitmap_size)
        return -1;

    if (mft_bitmap_offset_to_lba(vol, byte_offset, &lba, &sector_off) < 0)
        return -1;

    if (blkdev_read(vol->dev, lba, 1, sector_buf) != 0)
        return -1;

    return sector_buf[sector_off];
}

/* Write one byte to $MFT's $BITMAP (read-modify-write) */
static int mft_bitmap_write_byte(struct ntfs_volume *vol,
                                  uint64_t byte_offset, uint8_t value)
{
    uint64_t lba;
    uint32_t sector_off;
    uint8_t  sector_buf[512];

    if (byte_offset >= vol->mft_bitmap_size)
        return -1;

    if (mft_bitmap_offset_to_lba(vol, byte_offset, &lba, &sector_off) < 0)
        return -1;

    if (blkdev_read(vol->dev, lba, 1, sector_buf) != 0)
        return -1;

    sector_buf[sector_off] = value;

    if (blkdev_write(vol->dev, lba, 1, sector_buf) != 0)
        return -1;

    return 0;
}

/* Map an MFT inode number to a disk LBA using $MFT's $DATA runs.
 * This handles fragmented MFTs where records span multiple extents.
 * Exposed for ntfs_file_ops.c (ntfs_write_mft_record). */
int ntfs_mft_inode_to_lba(struct ntfs_volume *vol, uint64_t inode,
                             uint64_t *out_lba)
{
    uint64_t byte_offset = inode * (uint64_t)vol->frs_size;
    uint64_t cluster_off = byte_offset / vol->cluster_size;
    uint64_t in_cluster  = byte_offset % vol->cluster_size;
    uint64_t vcn = 0;
    int i;

    for (i = 0; i < vol->mft_data_run_count; i++) {
        uint64_t run_len = vol->mft_data_runs[i].length;
        if (cluster_off < vcn + run_len) {
            uint64_t lcn = vol->mft_data_runs[i].lcn;
            if (lcn == NTFS_LCN_SPARSE)
                return -1;
            uint64_t disk_cluster = lcn + (cluster_off - vcn);
            uint64_t disk_byte = disk_cluster * vol->cluster_size + in_cluster;
            *out_lba = disk_byte / vol->bytes_per_sector;
            return 0;
        }
        vcn += run_len;
    }
    return -1;
}

/* ============================================================================
 * ntfs_mft_alloc_load -- Load $MFT's $BITMAP and $DATA runs
 *
 * Reads $MFT's own MFT record (inode 0) to extract:
 *   - $DATA (type 0x80) runs -- where MFT records live on disk
 *   - $BITMAP (type 0xB0) runs -- which records are allocated
 * ============================================================================ */

int ntfs_mft_alloc_load(struct ntfs_volume *vol)
{
    uint8_t *mft_buf;
    struct ntfs_mft_header mft_hdr;
    struct ntfs_nonres_header nrhdr;
    struct ntfs_attr_header ah;
    struct ntfs_data_run temp_runs[MFT_MAX_RUNS];
    const uint8_t *attr;
    int run_count;
    uint32_t runs_bytes;
    int i;

    if (!vol)
        return NTFS_ERR_IO;

    /* Allocate buffer for $MFT's own MFT record */
    mft_buf = (uint8_t *)kmalloc(vol->frs_size);
    if (!mft_buf)
        return NTFS_ERR_IO;

    /* Read inode 0 ($MFT) */
    if (ntfs_read_mft_record(vol, NTFS_INODE_MFT,
                              mft_buf, &mft_hdr) != NTFS_OK) {
        kfree(mft_buf);
        klog(LOG_ERROR, "ntfs", "Failed to read $MFT (inode 0)");
        return NTFS_ERR_IO;
    }

    /* --- Extract $MFT's $DATA runs (where MFT records live on disk) --- */
    attr = ntfs_attr_find(mft_buf, &mft_hdr, NTFS_ATTR_DATA, &ah);
    if (!attr || ah.non_resident == 0) {
        kfree(mft_buf);
        klog(LOG_ERROR, "ntfs", "$MFT has no non-resident $DATA attribute");
        return NTFS_ERR_BAD_MAGIC;
    }

    run_count = ntfs_decode_data_runs(attr, temp_runs, MFT_MAX_RUNS, &nrhdr);
    if (run_count <= 0) {
        kfree(mft_buf);
        klog(LOG_ERROR, "ntfs", "$MFT $DATA has no data runs");
        return NTFS_ERR_BAD_MAGIC;
    }

    runs_bytes = (uint32_t)(run_count * sizeof(struct ntfs_data_run));
    vol->mft_data_runs = (struct ntfs_data_run *)kmalloc(runs_bytes);
    if (!vol->mft_data_runs) {
        kfree(mft_buf);
        return NTFS_ERR_IO;
    }

    for (i = 0; i < run_count; i++)
        vol->mft_data_runs[i] = temp_runs[i];
    vol->mft_data_run_count = run_count;
    vol->mft_data_size = nrhdr.real_size;
    vol->mft_total_records = nrhdr.real_size / (uint64_t)vol->frs_size;

    /* --- Extract $MFT's $BITMAP runs (which records are allocated) --- */
    attr = ntfs_attr_find(mft_buf, &mft_hdr, NTFS_ATTR_BITMAP, &ah);
    if (!attr) {
        kfree(mft_buf);
        klog(LOG_ERROR, "ntfs", "$MFT has no $BITMAP attribute");
        return NTFS_ERR_BAD_MAGIC;
    }

    if (ah.non_resident == 0) {
        /* Resident $BITMAP -- small MFT, bitmap fits in the record.
         * This is unusual but valid for very small volumes. */
        const uint8_t *data = attr + ah.content_offset;
        uint32_t bm_size = ah.content_length;

        vol->mft_bitmap_runs = NULL;
        vol->mft_bitmap_run_count = 0;
        vol->mft_bitmap_size = bm_size;

        klog(LOG_INFO, "ntfs",
             "$MFT bitmap: resident, %u bytes, %u records",
             (uint64_t)bm_size, vol->mft_total_records);
        (void)data;  /* Resident path handled by direct record access */
    } else {
        /* Non-resident $BITMAP -- typical for real volumes */
        run_count = ntfs_decode_data_runs(attr, temp_runs,
                                           MFT_MAX_RUNS, &nrhdr);
        if (run_count <= 0) {
            kfree(mft_buf);
            klog(LOG_ERROR, "ntfs", "$MFT $BITMAP has no data runs");
            return NTFS_ERR_BAD_MAGIC;
        }

        runs_bytes = (uint32_t)(run_count * sizeof(struct ntfs_data_run));
        vol->mft_bitmap_runs = (struct ntfs_data_run *)kmalloc(runs_bytes);
        if (!vol->mft_bitmap_runs) {
            kfree(mft_buf);
            return NTFS_ERR_IO;
        }

        for (i = 0; i < run_count; i++)
            vol->mft_bitmap_runs[i] = temp_runs[i];
        vol->mft_bitmap_run_count = run_count;
        vol->mft_bitmap_size = nrhdr.real_size;
    }

    kfree(mft_buf);

    vol->mft_alloc_loaded = 1;

    klog(LOG_INFO, "ntfs",
         "MFT allocator loaded: %u records, bitmap %u bytes",
         vol->mft_total_records, vol->mft_bitmap_size);
    klog(LOG_INFO, "ntfs",
         "  %d data runs, %d bitmap runs",
         (uint64_t)vol->mft_data_run_count,
         (uint64_t)vol->mft_bitmap_run_count);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_mft_extend -- Grow $MFT by allocating new clusters
 *
 * Called when ntfs_alloc_mft_record() can't find any free MFT records.
 * Allocates MFT_EXTEND_CLUSTERS from the volume (preferring MFT zone),
 * zeroes the new disk area, appends a data run to the in-memory
 * mft_data_runs[], extends the MFT bitmap to cover the new records,
 * and updates $MFT's $DATA attribute on disk.
 *
 * Returns NTFS_OK if the MFT was extended, NTFS_ERR_* on failure.
 * On success, the caller should retry the bitmap scan.
 * ============================================================================ */

/* Number of clusters to add in each expansion (16 clusters = 64 KiB @4K cls,
 * which is 64 MFT records at 1024 bytes/record). */
#define MFT_EXTEND_CLUSTERS  16

static int ntfs_mft_extend(struct ntfs_volume *vol)
{
    uint64_t hint_lcn;
    uint64_t new_lcn;
    uint64_t new_clusters = MFT_EXTEND_CLUSTERS;
    uint64_t new_bytes;
    uint64_t new_records;
    uint64_t old_total;
    uint64_t disk_byte;
    uint64_t lba;
    uint32_t i;

    /* Choose hint: prefer end of last MFT extent for contiguity */
    if (vol->mft_data_run_count > 0) {
        struct ntfs_data_run *last = &vol->mft_data_runs[
            vol->mft_data_run_count - 1];
        hint_lcn = last->lcn + last->length;
    } else {
        hint_lcn = vol->mft_lcn;
    }

    /* Allocate clusters */
    new_lcn = ntfs_alloc_clusters(vol, new_clusters, hint_lcn);
    if (new_lcn == 0) {
        klog(LOG_ERROR, "ntfs", "MFT extend: failed to alloc %u clusters",
             (uint64_t)new_clusters);
        return NTFS_ERR_IO;
    }

    new_bytes   = new_clusters * (uint64_t)vol->cluster_size;
    new_records = new_bytes / (uint64_t)vol->frs_size;
    old_total   = vol->mft_total_records;

    klog(LOG_INFO, "ntfs",
         "MFT extend: +%u clusters at LCN %u (+%u records, %u -> %u)",
         (uint64_t)new_clusters, new_lcn, new_records,
         old_total, old_total + new_records);

    /* Format each new MFT record with a valid header (FILE magic, USA,
     * sequence=1, flags=0 free).  Without this, ntfs_alloc_mft_record()
     * would reject the record because it reads 0x00000000 instead of
     * the "FILE" signature. */
    {
        uint8_t *rec = (uint8_t *)kmalloc(vol->frs_size);
        if (!rec) {
            ntfs_free_clusters(vol, new_lcn, new_clusters);
            return NTFS_ERR_IO;
        }

        uint32_t spc = vol->frs_size / vol->bytes_per_sector;
        if (spc == 0) spc = 1;
        uint16_t usa_off = 0x30;
        uint16_t usa_words = (uint16_t)(1 + vol->frs_size /
                              vol->bytes_per_sector);
        uint32_t first_attr = ((uint32_t)usa_off + (uint32_t)usa_words * 2
                               + 7) & ~7u;
        uint32_t used = first_attr + 4;  /* header + $END marker */

        disk_byte = new_lcn * (uint64_t)vol->cluster_size;

        for (i = 0; i < (uint32_t)new_records; i++) {
            ntfs_memset(rec, 0, vol->frs_size);

            /* 0x00: Magic "FILE" */
            rec[0] = 'F'; rec[1] = 'I'; rec[2] = 'L'; rec[3] = 'E';

            /* 0x04: USA offset */
            ntfs_le16_write(rec + 0x04, usa_off);
            /* 0x06: USA size in words */
            ntfs_le16_write(rec + 0x06, usa_words);

            /* 0x10: Sequence number = 1 */
            ntfs_le16_write(rec + 0x10, 1);

            /* 0x14: First attribute offset */
            ntfs_le16_write(rec + 0x14, (uint16_t)first_attr);

            /* 0x16: Flags = 0 (not in use) */
            ntfs_le16_write(rec + 0x16, 0);

            /* 0x18: Used size (32-bit LE) */
            ntfs_le16_write(rec + 0x18, (uint16_t)(used & 0xFFFF));
            ntfs_le16_write(rec + 0x1A, (uint16_t)(used >> 16));

            /* 0x1C: Allocated size (32-bit LE) */
            ntfs_le16_write(rec + 0x1C,
                            (uint16_t)(vol->frs_size & 0xFFFF));
            ntfs_le16_write(rec + 0x1E,
                            (uint16_t)(vol->frs_size >> 16));

            /* $END terminator at first attribute offset */
            rec[first_attr + 0] = 0xFF;
            rec[first_attr + 1] = 0xFF;
            rec[first_attr + 2] = 0xFF;
            rec[first_attr + 3] = 0xFF;

            /* Initialize USN in USA */
            ntfs_le16_write(rec + usa_off, 1);

            /* Apply USA regeneration */
            ntfs_regenerate_fixup(rec, vol->frs_size,
                                   vol->bytes_per_sector);

            /* Write this record to disk */
            lba = (disk_byte + (uint64_t)i * (uint64_t)vol->frs_size) /
                  (uint64_t)vol->bytes_per_sector;

            if (blkdev_write(vol->dev, lba, spc, rec) != 0) {
                klog(LOG_ERROR, "ntfs",
                     "MFT extend: failed to write record %u at LBA %u",
                     (uint64_t)(old_total + i), lba);
                kfree(rec);
                ntfs_free_clusters(vol, new_lcn, new_clusters);
                return NTFS_ERR_IO;
            }
        }

        kfree(rec);
    }

    /* --- Try to merge with the last run if adjacent --- */
    if (vol->mft_data_run_count > 0) {
        struct ntfs_data_run *last = &vol->mft_data_runs[
            vol->mft_data_run_count - 1];
        if (last->lcn + last->length == new_lcn) {
            /* Adjacent -- just extend the last run */
            last->length += new_clusters;
            goto update_sizes;
        }
    }

    /* --- Append new run to in-memory array --- */
    {
        int new_count = vol->mft_data_run_count + 1;
        uint32_t new_size = (uint32_t)(new_count *
                            sizeof(struct ntfs_data_run));
        struct ntfs_data_run *new_arr =
            (struct ntfs_data_run *)kmalloc(new_size);
        if (!new_arr) {
            ntfs_free_clusters(vol, new_lcn, new_clusters);
            return NTFS_ERR_IO;
        }

        /* Copy old runs */
        for (i = 0; i < (uint32_t)vol->mft_data_run_count; i++)
            new_arr[i] = vol->mft_data_runs[i];

        /* Append new run */
        new_arr[vol->mft_data_run_count].vcn_start =
            vol->mft_data_size / (uint64_t)vol->cluster_size;
        new_arr[vol->mft_data_run_count].lcn = new_lcn;
        new_arr[vol->mft_data_run_count].length = new_clusters;

        /* Swap arrays */
        if (vol->mft_data_runs)
            kfree(vol->mft_data_runs);
        vol->mft_data_runs = new_arr;
        vol->mft_data_run_count = new_count;
    }

update_sizes:
    /* Update in-memory totals */
    vol->mft_data_size += new_bytes;
    vol->mft_total_records = vol->mft_data_size / (uint64_t)vol->frs_size;

    /* --- Extend MFT bitmap to cover new records --- */
    {
        uint64_t needed_bm_bytes = (vol->mft_total_records + 7) / 8;
        if (needed_bm_bytes > vol->mft_bitmap_size)
            vol->mft_bitmap_size = needed_bm_bytes;
    }

    /* --- Update $MFT's $DATA attribute on disk ---
     * Re-read inode 0 ($MFT), re-encode the data runs, and write back. */
    {
        uint8_t *mft_rec = (uint8_t *)kmalloc(vol->frs_size);
        struct ntfs_mft_header mft_hdr;

        if (!mft_rec) {
            klog(LOG_WARN, "ntfs",
                 "MFT extend: no memory for $MFT update (in-memory OK)");
            return NTFS_OK;  /* In-memory state is fine, disk update deferred */
        }

        if (ntfs_read_mft_record(vol, NTFS_INODE_MFT,
                                  mft_rec, &mft_hdr) == NTFS_OK) {
            /* Find $DATA attribute and update its data runs + sizes */
            const uint8_t *data_attr = ntfs_attr_find(
                mft_rec, &mft_hdr, NTFS_ATTR_DATA, NULL);
            if (data_attr) {
                uint32_t attr_off = (uint32_t)(data_attr - mft_rec);
                uint8_t *attr = mft_rec + attr_off;

                /* Re-encode data runs */
                uint8_t run_buf[512];
                int encoded = ntfs_encode_data_runs(
                    vol->mft_data_runs, vol->mft_data_run_count,
                    run_buf, (int)sizeof(run_buf));

                if (encoded > 0) {
                    /* Update non-resident header sizes */
                    uint16_t dr_off = ntfs_le16(attr + 0x20);
                    uint64_t new_data_size = vol->mft_data_size;
                    uint64_t new_alloc = vol->mft_total_records *
                                        (uint64_t)vol->frs_size;
                    /* round alloc up to cluster boundary */
                    new_alloc = ((new_alloc + vol->cluster_size - 1) /
                                 vol->cluster_size) * vol->cluster_size;

                    /* Update last VCN */
                    uint64_t total_vcn = new_alloc / vol->cluster_size;
                    ntfs_le64_write(attr + 0x18,
                                    total_vcn > 0 ? total_vcn - 1 : 0);

                    /* Update alloc/real/init sizes */
                    ntfs_le64_write(attr + 0x28, new_alloc);
                    ntfs_le64_write(attr + 0x30, new_data_size);
                    ntfs_le64_write(attr + 0x38, new_data_size);

                    /* Copy encoded runs over old runs */
                    uint32_t old_attr_len = ntfs_le32(attr + 0x04);
                    uint32_t new_runs_end = dr_off + (uint32_t)encoded;
                    uint32_t new_attr_len =
                        (new_runs_end + 7) & ~7u; /* align 8 */
                    if (new_attr_len <= old_attr_len) {
                        /* Runs fit in existing space */
                        ntfs_memset(attr + dr_off, 0,
                                    old_attr_len - dr_off);
                        ntfs_memcpy(attr + dr_off, run_buf,
                                    (uint32_t)encoded);
                    }
                    /* else: runs too long, skip on-disk update for now */

                    ntfs_write_mft_record(vol, NTFS_INODE_MFT, mft_rec);
                }
            }
        }
        kfree(mft_rec);
    }

    klog(LOG_INFO, "ntfs",
         "MFT extended: %u total records, %u data runs",
         vol->mft_total_records,
         (uint64_t)vol->mft_data_run_count);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_alloc_mft_record -- Allocate a new MFT record
 *
 * 1. Scan $MFT's $BITMAP for the first free bit (starting at inode 24)
 * 2. Set the bit in the MFT bitmap
 * 3. Read the existing record (may contain old deleted data)
 * 4. Initialize header: magic, USA, sequence, flags, $END marker
 * 5. Apply USA regeneration (§12.2) and write back to disk
 * If no free records exist, extends $MFT by allocating new clusters.
 * ============================================================================ */

uint64_t ntfs_alloc_mft_record(struct ntfs_volume *vol, int is_directory)
{
    uint64_t inode = 0;
    uint64_t byte_off;
    uint64_t total_bytes;
    uint64_t flags;
    uint8_t *rec;
    uint64_t lba;
    uint32_t sector_count;
    uint16_t old_seq;
    uint16_t usa_offset;
    uint16_t usa_size_words;
    uint16_t new_flags;
    uint32_t first_attr_off;
    int found = 0;
    int extended = 0;  /* prevent infinite extend loop */

    if (!vol || !vol->mft_alloc_loaded)
        return 0;

    spin_lock_irqsave(&vol->mft_alloc_lock, &flags);

retry_scan:
    /* Step 1: Scan MFT bitmap for first free bit (skip system inodes 0-23) */
    total_bytes = vol->mft_bitmap_size;

    /* Cap scan to actual MFT records -- the bitmap file may be larger than
     * what $MFT's $DATA runs can hold (padding for future growth). */
    {
        uint64_t needed_bytes = (vol->mft_total_records + 7) / 8;
        if (total_bytes > needed_bytes)
            total_bytes = needed_bytes;
    }

    klog(LOG_DEBUG, "ntfs", "mft_alloc: scanning %u bytes for free record",
         total_bytes);

    /* Start scanning from NTFS_FIRST_USER_INODE */
    found = 0;
    for (byte_off = NTFS_FIRST_USER_INODE / 8;
         byte_off < total_bytes && !found; byte_off++) {
        int val = mft_bitmap_read_byte(vol, byte_off);
        if (val < 0)
            break;

        if ((uint8_t)val != 0xFF) {
            /* At least one free bit in this byte */
            uint8_t bit;
            for (bit = 0; bit < 8; bit++) {
                uint64_t candidate = byte_off * 8 + bit;

                /* Skip system inodes */
                if (candidate < NTFS_FIRST_USER_INODE)
                    continue;

                /* Skip beyond what MFT data can hold */
                if (candidate >= vol->mft_total_records)
                    break;

                if (!((uint8_t)val & (1u << bit))) {
                    inode = candidate;
                    found = 1;
                    break;
                }
            }
        }
    }

    if (!found) {
        if (!extended) {
            /* Try to extend the MFT */
            spin_unlock_irqrestore(&vol->mft_alloc_lock, flags);

            if (ntfs_mft_extend(vol) == NTFS_OK) {
                spin_lock_irqsave(&vol->mft_alloc_lock, &flags);
                extended = 1;
                goto retry_scan;
            }

            /* Extension failed -- truly full */
            klog(LOG_ERROR, "ntfs",
                 "MFT full: no free records and extension failed "
                 "(total=%llu)", vol->mft_total_records);
            return 0;
        }

        /* Already extended once, still no space -- shouldn't happen */
        spin_unlock_irqrestore(&vol->mft_alloc_lock, flags);
        klog(LOG_ERROR, "ntfs",
             "MFT full after extension (total=%llu)",
             vol->mft_total_records);
        return 0;
    }

    /* Step 2: Set bit in MFT bitmap */
    {
        uint64_t bm_byte = inode / 8;
        uint8_t  bm_bit  = (uint8_t)(1u << (inode % 8));
        int val = mft_bitmap_read_byte(vol, bm_byte);
        if (val < 0) {
            spin_unlock_irqrestore(&vol->mft_alloc_lock, flags);
            return 0;
        }
        val |= bm_bit;
        if (mft_bitmap_write_byte(vol, bm_byte, (uint8_t)val) < 0) {
            spin_unlock_irqrestore(&vol->mft_alloc_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&vol->mft_alloc_lock, flags);

    klog(LOG_DEBUG, "ntfs", "mft_alloc: found inode %u, reading record", inode);

    /* Step 3: Read existing record (may contain old deleted data) */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return 0;

    /* Read the MFT record from disk via $MFT data runs */
    if (ntfs_mft_inode_to_lba(vol, inode, &lba) < 0) {
        klog(LOG_ERROR, "ntfs", "mft_alloc: inode_to_lba failed for %u", inode);
        kfree(rec);
        return 0;
    }

    sector_count = vol->frs_size / vol->bytes_per_sector;
    if (sector_count == 0)
        sector_count = 1;

    /* Bounds check: don't read beyond the disk */
    if (lba + sector_count > vol->total_sectors) {
        klog(LOG_ERROR, "ntfs",
             "mft_alloc: LBA %u + %u > disk %u sectors",
             lba, (uint64_t)sector_count, vol->total_sectors);
        kfree(rec);
        return 0;
    }

    klog(LOG_DEBUG, "ntfs", "mft_alloc: reading LBA %u (%u sectors)",
         lba, (uint64_t)sector_count);

    if (blkdev_read(vol->dev, lba, sector_count, rec) != 0) {
        kfree(rec);
        return 0;
    }

    /* Step 4: Extract old sequence number (if record was previously used) */
    old_seq = 0;
    if (ntfs_le32(rec) == NTFS_MAGIC_FILE) {
        old_seq = ntfs_le16(rec + 0x10);  /* Sequence number at offset 0x10 */
    }

    /* Step 5: Initialize the MFT record header */
    ntfs_memset(rec, 0, vol->frs_size);

    /* 0x00: Magic "FILE" */
    rec[0] = 'F'; rec[1] = 'I'; rec[2] = 'L'; rec[3] = 'E';

    /* 0x04: USA offset -- standard is 0x30 for 1024-byte records */
    usa_offset = 0x30;
    ntfs_le16_write(rec + 0x04, usa_offset);

    /* 0x06: USA size in words -- 1 (USN) + (frs_size / sector_size) entries */
    usa_size_words = (uint16_t)(1 + vol->frs_size / vol->bytes_per_sector);
    ntfs_le16_write(rec + 0x06, usa_size_words);

    /* 0x08: LSN -- set to 0 (no journal yet) */
    /* already zeroed */

    /* 0x10: Sequence number -- increment previous occupant's seq */
    old_seq++;
    if (old_seq == 0)
        old_seq = 1;  /* Sequence 0 is invalid */
    ntfs_le16_write(rec + 0x10, old_seq);

    /* 0x12: Hard link count -- 0 (caller will set this) */
    /* already zeroed */

    /* 0x14: First attribute offset -- after USA */
    first_attr_off = (uint32_t)usa_offset + (uint32_t)usa_size_words * 2;
    /* Align to 8-byte boundary (NTFS requirement) */
    first_attr_off = (first_attr_off + 7) & ~7u;
    ntfs_le16_write(rec + 0x14, (uint16_t)first_attr_off);

    /* 0x16: Flags -- in-use (+ directory if requested) */
    new_flags = NTFS_MFT_FLAG_IN_USE;
    if (is_directory)
        new_flags |= NTFS_MFT_FLAG_DIRECTORY;
    ntfs_le16_write(rec + 0x16, new_flags);

    /* 0x18: Used size -- header + $END marker (4 bytes) */
    {
        uint32_t used = first_attr_off + 4;  /* $END is 4 bytes (0xFFFFFFFF) */
        ntfs_le16_write(rec + 0x18, (uint16_t)(used & 0xFFFF));
        ntfs_le16_write(rec + 0x1A, (uint16_t)(used >> 16));
    }

    /* 0x1C: Allocated size (= frs_size) */
    {
        uint32_t alloc = vol->frs_size;
        ntfs_le16_write(rec + 0x1C, (uint16_t)(alloc & 0xFFFF));
        ntfs_le16_write(rec + 0x1E, (uint16_t)(alloc >> 16));
    }

    /* 0x20: Base record reference -- 0 (this IS the base record) */
    /* already zeroed */

    /* Write $END terminator at first attribute offset */
    rec[first_attr_off + 0] = 0xFF;
    rec[first_attr_off + 1] = 0xFF;
    rec[first_attr_off + 2] = 0xFF;
    rec[first_attr_off + 3] = 0xFF;

    /* Initialize USN in USA to 1 (will be incremented by regenerate) */
    ntfs_le16_write(rec + usa_offset, 0);

    /* Step 6: Apply USA regeneration before writing to disk */
    if (ntfs_regenerate_fixup(rec, vol->frs_size,
                               vol->bytes_per_sector) != NTFS_OK) {
        kfree(rec);
        klog(LOG_ERROR, "ntfs",
             "USA regeneration failed for new inode %llu", inode);
        return 0;
    }

    /* Step 7: Write initialized record back to disk */
    klog(LOG_DEBUG, "ntfs", "mft_alloc: writing inode %u at LBA %u",
         inode, lba);

    /* Bounds check before write */
    if (lba + sector_count > vol->total_sectors) {
        klog(LOG_ERROR, "ntfs",
             "mft_alloc: write LBA %u out of bounds", lba);
        kfree(rec);
        return 0;
    }

    if (blkdev_write(vol->dev, lba, sector_count, rec) != 0) {
        kfree(rec);
        klog(LOG_ERROR, "ntfs",
             "Failed to write new MFT record at inode %llu", inode);
        return 0;
    }

    kfree(rec);

    klog(LOG_DEBUG, "ntfs",
         "Allocated MFT record: inode %llu, seq %u, flags 0x%x",
         inode, (uint64_t)old_seq, (uint64_t)new_flags);

    return inode;
}

/* ============================================================================
 * ntfs_free_mft_record -- Free an MFT record
 *
 * 1. Read the record from disk
 * 2. Clear the in-use flag (bit 0) -- do NOT zero the record
 * 3. Increment sequence number (stale reference detection)
 * 4. Apply USA regeneration and write back
 * 5. Clear the bit in $MFT's $BITMAP
 * ============================================================================ */

int ntfs_free_mft_record(struct ntfs_volume *vol, uint64_t inode)
{
    uint8_t *rec;
    uint64_t lba;
    uint32_t sector_count;
    uint16_t old_flags;
    uint16_t old_seq;
    uint64_t lock_flags;

    if (!vol || !vol->mft_alloc_loaded)
        return NTFS_ERR_IO;

    /* Don't allow freeing system inodes */
    if (inode < NTFS_FIRST_USER_INODE) {
        klog(LOG_ERROR, "ntfs",
             "Refusing to free system inode %llu", inode);
        return NTFS_ERR_IO;
    }

    if (inode >= vol->mft_total_records)
        return NTFS_ERR_BAD_MAGIC;

    /* Read the MFT record */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    if (ntfs_mft_inode_to_lba(vol, inode, &lba) < 0) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    sector_count = vol->frs_size / vol->bytes_per_sector;
    if (sector_count == 0)
        sector_count = 1;

    if (blkdev_read(vol->dev, lba, sector_count, rec) != 0) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    /* Verify magic */
    if (ntfs_le32(rec) != NTFS_MAGIC_FILE) {
        kfree(rec);
        klog(LOG_WARN, "ntfs",
             "Free inode %llu: bad magic 0x%x",
             inode, (uint64_t)ntfs_le32(rec));
        return NTFS_ERR_BAD_MAGIC;
    }

    /* Apply fixup to get clean record (undo USA stamping) */
    if (ntfs_apply_fixup(rec, vol->frs_size,
                          vol->bytes_per_sector) != NTFS_OK) {
        kfree(rec);
        return NTFS_ERR_FIXUP;
    }

    /* Clear in-use flag (bit 0) -- do NOT zero the record */
    old_flags = ntfs_le16(rec + 0x16);
    old_flags &= (uint16_t)~NTFS_MFT_FLAG_IN_USE;
    ntfs_le16_write(rec + 0x16, old_flags);

    /* Increment sequence number (wrap 0 → 1) */
    old_seq = ntfs_le16(rec + 0x10);
    old_seq++;
    if (old_seq == 0)
        old_seq = 1;
    ntfs_le16_write(rec + 0x10, old_seq);

    /* Apply USA regeneration before writing */
    if (ntfs_regenerate_fixup(rec, vol->frs_size,
                               vol->bytes_per_sector) != NTFS_OK) {
        kfree(rec);
        return NTFS_ERR_FIXUP;
    }

    /* Write modified record back to disk */
    if (blkdev_write(vol->dev, lba, sector_count, rec) != 0) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    kfree(rec);

    /* Clear bit in MFT bitmap */
    spin_lock_irqsave(&vol->mft_alloc_lock, &lock_flags);

    {
        uint64_t bm_byte = inode / 8;
        uint8_t  bm_bit  = (uint8_t)(1u << (inode % 8));
        int val = mft_bitmap_read_byte(vol, bm_byte);
        if (val >= 0) {
            val &= ~bm_bit;
            mft_bitmap_write_byte(vol, bm_byte, (uint8_t)val);
        }
    }

    spin_unlock_irqrestore(&vol->mft_alloc_lock, lock_flags);

    klog(LOG_DEBUG, "ntfs",
         "Freed MFT record: inode %llu, new seq %u",
         inode, (uint64_t)old_seq);

    return NTFS_OK;
}
