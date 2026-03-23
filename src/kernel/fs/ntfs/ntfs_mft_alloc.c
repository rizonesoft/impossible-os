/* ============================================================================
 * ntfs_mft_alloc.c — MFT Record Allocator (§12.3)
 *
 * Manages allocation and deallocation of MFT records (inodes).
 * Uses $MFT's own $BITMAP attribute (NOT inode 6/$Bitmap) to track
 * which MFT records are in use.
 *
 * Key operations:
 *   ntfs_mft_alloc_load()   — Load $MFT bitmap + data runs at mount time
 *   ntfs_alloc_mft_record() — Find free inode, init record, write to disk
 *   ntfs_free_mft_record()  — Clear in-use flag, free bitmap bit
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
 * Internal helpers — MFT bitmap I/O
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
 * ntfs_mft_alloc_load — Load $MFT's $BITMAP and $DATA runs
 *
 * Reads $MFT's own MFT record (inode 0) to extract:
 *   - $DATA (type 0x80) runs — where MFT records live on disk
 *   - $BITMAP (type 0xB0) runs — which records are allocated
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
        /* Resident $BITMAP — small MFT, bitmap fits in the record.
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
        /* Non-resident $BITMAP — typical for real volumes */
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
 * ntfs_alloc_mft_record — Allocate a new MFT record
 *
 * 1. Scan $MFT's $BITMAP for the first free bit (starting at inode 24)
 * 2. Set the bit in the MFT bitmap
 * 3. Read the existing record (may contain old deleted data)
 * 4. Initialize header: magic, USA, sequence, flags, $END marker
 * 5. Apply USA regeneration (§12.2) and write back to disk
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

    if (!vol || !vol->mft_alloc_loaded)
        return 0;

    spin_lock_irqsave(&vol->mft_alloc_lock, &flags);

    /* Step 1: Scan MFT bitmap for first free bit (skip system inodes 0–23) */
    total_bytes = vol->mft_bitmap_size;

    /* Cap scan to actual MFT records — the bitmap file may be larger than
     * what $MFT's $DATA runs can hold (padding for future growth). */
    {
        uint64_t needed_bytes = (vol->mft_total_records + 7) / 8;
        if (total_bytes > needed_bytes)
            total_bytes = needed_bytes;
    }

    klog(LOG_DEBUG, "ntfs", "mft_alloc: scanning %u bytes for free record",
         total_bytes);

    /* Start scanning from NTFS_FIRST_USER_INODE */
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
        /* No free records — MFT extension would go here.
         * For now, log error. MFT extension (allocating clusters from
         * MFT Zone, appending data runs) is deferred to §12.3 follow-up. */
        spin_unlock_irqrestore(&vol->mft_alloc_lock, flags);
        klog(LOG_ERROR, "ntfs",
             "MFT full: no free records (total=%llu)",
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

    /* 0x04: USA offset — standard is 0x30 for 1024-byte records */
    usa_offset = 0x30;
    ntfs_le16_write(rec + 0x04, usa_offset);

    /* 0x06: USA size in words — 1 (USN) + (frs_size / sector_size) entries */
    usa_size_words = (uint16_t)(1 + vol->frs_size / vol->bytes_per_sector);
    ntfs_le16_write(rec + 0x06, usa_size_words);

    /* 0x08: LSN — set to 0 (no journal yet) */
    /* already zeroed */

    /* 0x10: Sequence number — increment previous occupant's seq */
    old_seq++;
    if (old_seq == 0)
        old_seq = 1;  /* Sequence 0 is invalid */
    ntfs_le16_write(rec + 0x10, old_seq);

    /* 0x12: Hard link count — 0 (caller will set this) */
    /* already zeroed */

    /* 0x14: First attribute offset — after USA */
    first_attr_off = (uint32_t)usa_offset + (uint32_t)usa_size_words * 2;
    /* Align to 8-byte boundary (NTFS requirement) */
    first_attr_off = (first_attr_off + 7) & ~7u;
    ntfs_le16_write(rec + 0x14, (uint16_t)first_attr_off);

    /* 0x16: Flags — in-use (+ directory if requested) */
    new_flags = NTFS_MFT_FLAG_IN_USE;
    if (is_directory)
        new_flags |= NTFS_MFT_FLAG_DIRECTORY;
    ntfs_le16_write(rec + 0x16, new_flags);

    /* 0x18: Used size — header + $END marker (4 bytes) */
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

    /* 0x20: Base record reference — 0 (this IS the base record) */
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
 * ntfs_free_mft_record — Free an MFT record
 *
 * 1. Read the record from disk
 * 2. Clear the in-use flag (bit 0) — do NOT zero the record
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

    /* Clear in-use flag (bit 0) — do NOT zero the record */
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
