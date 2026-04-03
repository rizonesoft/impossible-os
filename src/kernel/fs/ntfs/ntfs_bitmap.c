/* ============================================================================
 * ntfs_bitmap.c -- Cluster Allocator (§12.1)
 *
 * Uses $Bitmap (inode 6) to track cluster allocation.
 * One bit per cluster: 0 = free, 1 = allocated.
 *
 * The bitmap is a non-resident $DATA attribute read via data runs.
 * Modifications are written back sector-at-a-time to disk.
 *
 * MFT Zone: first 12.5% of the volume is reserved for MFT growth.
 * We skip the MFT zone during allocation unless no other space exists.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* $Bitmap is always inode 6 */
#define NTFS_INODE_BITMAP  6

/* Popcount: count set bits in a byte (number of allocated clusters) */
static int popcount8(uint8_t b)
{
    int c = 0;
    while (b) { c += b & 1; b >>= 1; }
    return c;
}

/* Map a byte offset within the $Bitmap data to an LBA on disk.
 * Uses the bitmap data runs to translate file offset → disk sector.
 * Returns 0 on success, -1 if offset is out of range. */
static int bitmap_offset_to_lba(struct ntfs_volume *vol,
                                 uint64_t byte_offset,
                                 uint64_t *out_lba,
                                 uint32_t *out_sector_offset)
{
    uint64_t cluster_off = byte_offset / vol->cluster_size;
    uint64_t in_cluster  = byte_offset % vol->cluster_size;
    uint64_t vcn = 0;
    int i;

    for (i = 0; i < vol->bitmap_run_count; i++) {
        uint64_t run_len = vol->bitmap_runs[i].length;
        if (cluster_off < vcn + run_len) {
            /* Found the run containing this offset */
            uint64_t lcn = vol->bitmap_runs[i].lcn;
            if (lcn == NTFS_LCN_SPARSE)
                return -1;  /* Sparse run -- should not happen for $Bitmap */
            uint64_t disk_cluster = lcn + (cluster_off - vcn);
            uint64_t disk_byte = disk_cluster * vol->cluster_size + in_cluster;
            *out_lba = disk_byte / vol->bytes_per_sector;
            *out_sector_offset = (uint32_t)(disk_byte % vol->bytes_per_sector);
            return 0;
        }
        vcn += run_len;
    }
    return -1;  /* Offset beyond bitmap extent */
}

/* Read one byte from the $Bitmap at the given byte offset.
 * Returns the byte directly from the in-memory bitmap buffer.
 * Returns -1 if the offset is out of range or bitmap not loaded. */
static int bitmap_read_byte(struct ntfs_volume *vol, uint64_t byte_offset)
{
    if (!vol->bitmap_data || byte_offset >= vol->bitmap_size)
        return -1;
    return vol->bitmap_data[byte_offset];
}

/* Set or clear a range of bits in the bitmap.
 * set=1: mark clusters as allocated.  set=0: mark as free.
 *
 * Modifies the in-memory bitmap_data buffer directly (authoritative copy),
 * then writes changed sectors back to disk (write-through).
 * Since reads always come from bitmap_data, there is no stale-read issue. */
static int bitmap_set_range(struct ntfs_volume *vol,
                             uint64_t lcn, uint64_t count, int set)
{
    uint64_t cur_lba = (uint64_t)-1;
    int      need_flush = 0;
    uint64_t i;

    if (!vol->bitmap_data)
        return NTFS_ERR_IO;

    for (i = 0; i < count; i++) {
        uint64_t cluster  = lcn + i;
        uint64_t byte_off = cluster / 8;
        uint8_t  bit_mask = (uint8_t)(1u << (cluster % 8));
        uint64_t lba;
        uint32_t sector_off;

        if (byte_off >= vol->bitmap_size)
            return NTFS_ERR_IO;

        /* Modify the in-memory bitmap (always authoritative) */
        if (set)
            vol->bitmap_data[byte_off] |= bit_mask;
        else
            vol->bitmap_data[byte_off] &= ~bit_mask;

        /* Track which disk sector needs flushing */
        if (bitmap_offset_to_lba(vol, byte_off, &lba, &sector_off) < 0)
            return NTFS_ERR_IO;

        /* When we cross to a new sector, flush the previous one */
        if (lba != cur_lba) {
            if (need_flush) {
                /* Build sector from in-memory bitmap and write to disk */
                uint8_t sector_buf[512];
                uint64_t sec_byte_start;
                uint32_t dummy_off;

                /* Determine which bitmap byte starts this sector */
                bitmap_offset_to_lba(vol, (cluster - 1) / 8,
                                     &cur_lba, &dummy_off);
                sec_byte_start = ((cluster - 1) / 8) - dummy_off;

                /* Copy from in-memory bitmap into sector buffer */
                {
                    uint32_t ci;
                    for (ci = 0; ci < 512; ci++) {
                        uint64_t boff = sec_byte_start + ci;
                        sector_buf[ci] = (boff < vol->bitmap_size) ?
                                         vol->bitmap_data[boff] : 0;
                    }
                }

                if (blkdev_write(vol->dev, cur_lba, 1, sector_buf) != 0)
                    return NTFS_ERR_IO;
            }
            cur_lba = lba;
            need_flush = 1;
        }
    }

    /* Flush the last sector */
    if (need_flush && cur_lba != (uint64_t)-1) {
        uint8_t sector_buf[512];
        uint64_t last_byte_off = (lcn + count - 1) / 8;
        uint32_t sector_off_last;
        uint64_t dummy_lba;

        if (bitmap_offset_to_lba(vol, last_byte_off,
                                 &dummy_lba, &sector_off_last) < 0)
            return NTFS_ERR_IO;

        uint64_t sec_byte_start = last_byte_off - sector_off_last;
        {
            uint32_t ci;
            for (ci = 0; ci < 512; ci++) {
                uint64_t boff = sec_byte_start + ci;
                sector_buf[ci] = (boff < vol->bitmap_size) ?
                                 vol->bitmap_data[boff] : 0;
            }
        }

        if (blkdev_write(vol->dev, cur_lba, 1, sector_buf) != 0)
            return NTFS_ERR_IO;
    }

    return NTFS_OK;
}

/* Check if a cluster is within the MFT zone */
static int in_mft_zone(struct ntfs_volume *vol, uint64_t lcn)
{
    return (lcn >= vol->mft_zone_start && lcn < vol->mft_zone_end) ? 1 : 0;
}

/* Search the bitmap for `count` contiguous free clusters,
 * starting the search near `start_lcn`.
 * If skip_mft_zone is true, skip clusters in the MFT zone.
 * Returns the starting LCN, or 0 if not found. */
static uint64_t find_contiguous_free(struct ntfs_volume *vol,
                                      uint64_t count,
                                      uint64_t start_lcn,
                                      int skip_mft_zone)
{
    uint64_t total = vol->total_clusters;
    uint64_t lcn = start_lcn;
    uint64_t searched = 0;
    uint64_t run_start = 0;
    uint64_t run_len = 0;

    if (lcn >= total)
        lcn = 0;

    while (searched < total) {
        uint64_t byte_off = lcn / 8;
        uint8_t  bit_pos  = (uint8_t)(lcn % 8);
        int val;

        /* Skip MFT Zone if requested */
        if (skip_mft_zone && in_mft_zone(vol, lcn)) {
            /* Jump past the MFT zone */
            lcn = vol->mft_zone_end;
            if (lcn >= total)
                lcn = 0;
            run_len = 0;
            searched++;
            continue;
        }

        val = bitmap_read_byte(vol, byte_off);
        if (val < 0)
            break;

        if (!(val & (1u << bit_pos))) {
            /* Cluster is free */
            if (run_len == 0)
                run_start = lcn;
            run_len++;
            if (run_len >= count)
                return run_start;
        } else {
            /* Cluster is allocated -- reset run */
            run_len = 0;
        }

        lcn++;
        if (lcn >= total)
            lcn = 0;
        searched++;
    }

    return 0;  /* Not found */
}

int ntfs_bitmap_load(struct ntfs_volume *vol)
{
    uint8_t *mft_buf;
    struct ntfs_mft_header mft_hdr;
    struct ntfs_nonres_header nrhdr;
    struct ntfs_data_run temp_runs[NTFS_MAX_BITMAP_RUNS];
    int run_count;
    uint64_t byte_off;
    uint64_t free_count;
    uint64_t total_bytes;

    if (!vol)
        return NTFS_ERR_IO;

    /* Allocate MFT record buffer */
    mft_buf = (uint8_t *)kmalloc(vol->frs_size);
    if (!mft_buf)
        return NTFS_ERR_IO;

    /* Read $Bitmap MFT record (inode 6) */
    if (ntfs_read_mft_record(vol, NTFS_INODE_BITMAP,
                              mft_buf, &mft_hdr) != NTFS_OK) {
        kfree(mft_buf);
        return NTFS_ERR_IO;
    }

    /* Decode data runs from the unnamed $DATA attribute */
    run_count = ntfs_decode_data_runs(
        ntfs_attr_find(mft_buf, &mft_hdr, NTFS_ATTR_DATA, NULL),
        temp_runs, NTFS_MAX_BITMAP_RUNS, &nrhdr);

    kfree(mft_buf);

    if (run_count <= 0) {
        klog(LOG_ERROR, "ntfs", "$Bitmap has no data runs");
        return NTFS_ERR_BAD_MAGIC;
    }

    /* Allocate persistent storage for bitmap runs */
    uint32_t runs_bytes = (uint32_t)(run_count * sizeof(struct ntfs_data_run));
    vol->bitmap_runs = (struct ntfs_data_run *)kmalloc(runs_bytes);
    if (!vol->bitmap_runs)
        return NTFS_ERR_IO;

    /* Copy runs to volume context */
    int i;
    for (i = 0; i < run_count; i++)
        vol->bitmap_runs[i] = temp_runs[i];
    vol->bitmap_run_count = run_count;
    vol->bitmap_size = nrhdr.real_size;

    /* ---- Load entire $Bitmap into memory ----
     * This eliminates the VirtIO write-back cache coherency issue:
     * all bitmap reads come from this authoritative in-memory copy,
     * and writes update both the buffer and the disk (write-through). */
    {
        uint32_t bm_pages = (uint32_t)((vol->bitmap_size + 4095) / 4096);
        uintptr_t bm_phys = pmm_alloc_contiguous(bm_pages);
        vol->bitmap_data_pages = bm_pages;

        if (!bm_phys) {
            klog(LOG_ERROR, "ntfs",
                 "$Bitmap: failed to allocate %u pages", (uint64_t)bm_pages);
            kfree(vol->bitmap_runs);
            vol->bitmap_runs = NULL;
            return NTFS_ERR_IO;
        }
        vol->bitmap_data = (uint8_t *)bm_phys;

        /* Zero the buffer first (handles partial last page) */
        {
            uint64_t zi;
            for (zi = 0; zi < (uint64_t)bm_pages * 4096; zi++)
                vol->bitmap_data[zi] = 0;
        }

        /* Read bitmap data from disk using the data runs */
        {
            uint64_t file_offset = 0;
            int ri;
            for (ri = 0; ri < run_count && file_offset < vol->bitmap_size; ri++) {
                uint64_t lcn = vol->bitmap_runs[ri].lcn;
                uint64_t len = vol->bitmap_runs[ri].length;
                uint64_t ci;

                if (lcn == NTFS_LCN_SPARSE) {
                    /* Sparse run: leave as zeros */
                    file_offset += len * vol->cluster_size;
                    continue;
                }

                for (ci = 0; ci < len && file_offset < vol->bitmap_size; ci++) {
                    uint64_t disk_lba = (lcn + ci) *
                                        (uint64_t)vol->sectors_per_cluster;
                    uint32_t sectors = vol->sectors_per_cluster;
                    uint64_t bytes_left = vol->bitmap_size - file_offset;
                    uint64_t bytes_this = (uint64_t)sectors * vol->bytes_per_sector;
                    if (bytes_this > bytes_left)
                        bytes_this = bytes_left;

                    /* Read directly into bitmap_data buffer */
                    if (blkdev_read(vol->dev, disk_lba, sectors,
                                   vol->bitmap_data + file_offset) != 0) {
                        klog(LOG_ERROR, "ntfs",
                             "$Bitmap: failed to read LCN %llu", lcn + ci);
                        {
                            uint32_t pi;
                            for (pi = 0; pi < bm_pages; pi++)
                                pmm_free_frame(bm_phys + pi * 4096);
                        }
                        vol->bitmap_data = NULL;
                        kfree(vol->bitmap_runs);
                        vol->bitmap_runs = NULL;
                        return NTFS_ERR_IO;
                    }
                    file_offset += bytes_this;
                }
            }
        }

        klog(LOG_DEBUG, "ntfs",
             "$Bitmap loaded: %llu bytes (%u pages) in memory",
             vol->bitmap_size, (uint64_t)bm_pages);
    }

    /* Calculate total clusters */
    vol->total_clusters = vol->total_sectors /
                           (uint64_t)vol->sectors_per_cluster;

    /* MFT Zone: MFT location + 12.5% of volume */
    vol->mft_zone_start = vol->mft_lcn;
    vol->mft_zone_end = vol->mft_lcn + (vol->total_clusters / 8);
    if (vol->mft_zone_end > vol->total_clusters)
        vol->mft_zone_end = vol->total_clusters;

    /* Count free clusters by scanning the entire bitmap */
    free_count = 0;
    total_bytes = (vol->total_clusters + 7) / 8;

    for (byte_off = 0; byte_off < total_bytes; byte_off++) {
        int val = bitmap_read_byte(vol, byte_off);
        if (val < 0)
            break;
        /* Count zero bits (free clusters) */
        free_count += 8 - popcount8((uint8_t)val);
    }

    /* Adjust for trailing bits beyond total_clusters */
    uint8_t tail_bits = (uint8_t)(vol->total_clusters % 8);
    if (tail_bits != 0) {
        /* The last byte has (8 - tail_bits) padding bits at the top.
         * Those are marked allocated but don't count as real clusters. */
        int last_val = bitmap_read_byte(vol, total_bytes - 1);
        if (last_val >= 0) {
            uint8_t padding_mask = (uint8_t)(0xFF << tail_bits);
            int padding_free = 8 - popcount8((uint8_t)(last_val | padding_mask));
            /* Recalculate: only tail_bits are real clusters */
            free_count = free_count + popcount8(padding_mask)
                         - (8 - popcount8((uint8_t)last_val));
            /* Actually simpler: recount just the real bits */
            free_count = 0;
            for (byte_off = 0; byte_off < total_bytes - 1; byte_off++) {
                int v = bitmap_read_byte(vol, byte_off);
                if (v < 0) break;
                free_count += 8 - popcount8((uint8_t)v);
            }
            /* Last byte: only count tail_bits worth of bits */
            uint8_t real_mask = (uint8_t)((1u << tail_bits) - 1);
            free_count += tail_bits - popcount8((uint8_t)(last_val & real_mask));
            (void)padding_free;
        }
    }

    vol->free_clusters = free_count;
    vol->bitmap_loaded = 1;

    klog(LOG_INFO, "ntfs",
         "$Bitmap loaded: %llu total clusters, %llu free (%llu MB free)",
         vol->total_clusters, vol->free_clusters,
         (vol->free_clusters * vol->cluster_size) / (1024 * 1024));
    klog(LOG_INFO, "ntfs",
         "MFT Zone: LCN %llu - %llu (%llu clusters reserved)",
         vol->mft_zone_start, vol->mft_zone_end,
         vol->mft_zone_end - vol->mft_zone_start);

    return NTFS_OK;
}

uint64_t ntfs_alloc_clusters(struct ntfs_volume *vol,
                              uint64_t count, uint64_t hint_lcn)
{
    uint64_t result;
    uint64_t flags;

    if (!vol || !vol->bitmap_loaded || count == 0)
        return 0;

    spin_lock_irqsave(&vol->bitmap_lock, &flags);

    /* First try: search near hint, skipping MFT zone */
    result = find_contiguous_free(vol, count, hint_lcn, 1);

    /* Second try: search from LCN 0, still skipping MFT zone */
    if (result == 0 && hint_lcn != 0)
        result = find_contiguous_free(vol, count, 0, 1);

    /* Last resort: allow MFT zone */
    if (result == 0) {
        klog(LOG_WARN, "ntfs", "MFT Zone breached - volume nearly full");
        result = find_contiguous_free(vol, count, 0, 0);
    }

    if (result != 0) {
        /* Mark clusters as allocated */
        if (bitmap_set_range(vol, result, count, 1) != NTFS_OK) {
            spin_unlock_irqrestore(&vol->bitmap_lock, flags);
            return 0;
        }
        vol->free_clusters -= count;
    }

    spin_unlock_irqrestore(&vol->bitmap_lock, flags);
    return result;
}

int ntfs_free_clusters(struct ntfs_volume *vol,
                        uint64_t lcn, uint64_t count)
{
    uint64_t flags;
    int rc;

    if (!vol || !vol->bitmap_loaded || count == 0)
        return NTFS_ERR_IO;

    if (lcn + count > vol->total_clusters)
        return NTFS_ERR_IO;

    spin_lock_irqsave(&vol->bitmap_lock, &flags);

    rc = bitmap_set_range(vol, lcn, count, 0);
    if (rc == NTFS_OK)
        vol->free_clusters += count;

    spin_unlock_irqrestore(&vol->bitmap_lock, flags);
    return rc;
}

uint64_t ntfs_get_free_space(struct ntfs_volume *vol)
{
    uint64_t byte_off;
    uint64_t total_bytes;
    uint64_t free_count = 0;
    uint64_t flags;

    if (!vol || !vol->bitmap_loaded)
        return 0;

    spin_lock_irqsave(&vol->bitmap_lock, &flags);

    total_bytes = (vol->total_clusters + 7) / 8;

    for (byte_off = 0; byte_off < total_bytes; byte_off++) {
        int val = bitmap_read_byte(vol, byte_off);
        if (val < 0)
            break;
        free_count += 8 - popcount8((uint8_t)val);
    }

    /* Correct for padding bits in the last byte */
    uint8_t tail_bits = (uint8_t)(vol->total_clusters % 8);
    if (tail_bits != 0) {
        int last_val = bitmap_read_byte(vol, total_bytes - 1);
        if (last_val >= 0) {
            /* Remove previously counted free padding bits */
            uint8_t padding_zeros = (uint8_t)(8 - tail_bits);
            /* Re-count only the real bits in the last byte */
            uint8_t real_mask = (uint8_t)((1u << tail_bits) - 1);
            int real_free = tail_bits - popcount8((uint8_t)(last_val & real_mask));
            int total_free_in_byte = 8 - popcount8((uint8_t)last_val);
            free_count -= (uint64_t)total_free_in_byte;
            free_count += (uint64_t)real_free;
            (void)padding_zeros;
        }
    }

    vol->free_clusters = free_count;

    spin_unlock_irqrestore(&vol->bitmap_lock, flags);
    return free_count;
}
