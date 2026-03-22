/* ============================================================================
 * ntfs_mft.c — MFT Record Reader & Update Sequence Array Fixup
 *
 * Reads individual MFT records by inode number and implements the
 * USA fixup algorithm to detect sector tears.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/klog.h"

/* ============================================================================
 * ntfs_read_mft_record — Read and parse a single MFT record by inode number.
 *
 * Calculates byte offset: mft_byte_offset + (inode × frs_size)
 * Converts to LBA and reads frs_size/sector_size sectors.
 * Parses the record header and validates magic + flags.
 *
 * The caller must provide a buffer of at least vol->frs_size bytes.
 * ============================================================================ */

int ntfs_read_mft_record(struct ntfs_volume *vol, uint64_t inode,
                         void *buf, struct ntfs_mft_header *hdr)
{
    uint64_t byte_offset;
    uint64_t lba;
    uint32_t sector_count;
    uint8_t *rec;
    uint32_t magic;

    if (!vol || !buf || !hdr) {
        return NTFS_ERR_IO;
    }

    /* Calculate disk byte offset of this MFT record */
    byte_offset = vol->mft_byte_offset + (inode * (uint64_t)vol->frs_size);

    /* Convert byte offset to LBA */
    lba = byte_offset / (uint64_t)vol->bytes_per_sector;

    /* Number of sectors to read for one FRS */
    sector_count = vol->frs_size / vol->bytes_per_sector;
    if (sector_count == 0)
        sector_count = 1;

    /* Read from disk */
    if (blkdev_read(vol->dev, lba, sector_count, buf) != 0) {
        klog(LOG_DEBUG, "ntfs", "MFT read failed: inode %u, LBA %u",
             inode, lba);
        return NTFS_ERR_IO;
    }

    /* Parse the record header */
    rec = (uint8_t *)buf;

    /* 0x00: Magic number (4 bytes LE) */
    magic = ntfs_le32(rec + 0x00);

    if (magic == NTFS_MAGIC_BAAD) {
        klog(LOG_DEBUG, "ntfs", "MFT Record %u: BAAD (corrupt)", inode);
        return NTFS_ERR_BAD_RECORD;
    }

    if (magic != NTFS_MAGIC_FILE) {
        klog(LOG_DEBUG, "ntfs", "MFT Record %u: bad magic 0x%x",
             inode, (uint64_t)magic);
        return NTFS_ERR_BAD_MAGIC;
    }

    /* Fill parsed header struct */
    hdr->magic           = magic;
    hdr->usa_offset      = ntfs_le16(rec + 0x04);
    hdr->usa_size        = ntfs_le16(rec + 0x06);
    hdr->lsn             = ntfs_le64(rec + 0x08);
    hdr->seq_number      = ntfs_le16(rec + 0x10);
    hdr->hard_link_count = ntfs_le16(rec + 0x12);
    hdr->attrs_offset    = ntfs_le16(rec + 0x14);
    hdr->flags           = ntfs_le16(rec + 0x16);
    hdr->used_size       = ntfs_le32(rec + 0x18);
    hdr->alloc_size      = ntfs_le32(rec + 0x1C);
    hdr->base_record_ref = ntfs_le64(rec + 0x20);

    /* Check in-use flag */
    if (!(hdr->flags & NTFS_MFT_FLAG_IN_USE)) {
        klog(LOG_DEBUG, "ntfs", "MFT Record %u: not in-use (deleted/free)",
             inode);
        return NTFS_ERR_FREE;
    }

    klog(LOG_DEBUG, "ntfs",
         "MFT Record %u: flags=0x%x, attrs_at=0x%x, links=%u",
         inode, (uint64_t)hdr->flags,
         (uint64_t)hdr->attrs_offset,
         (uint64_t)hdr->hard_link_count);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_apply_fixup — Update Sequence Array fixup
 *
 * NTFS writes a 2-byte "Update Sequence Number" (USN) over the last 2 bytes
 * of each sector in a multi-sector record (FILE, INDX).  The original bytes
 * are saved in the Update Sequence Array (USA) that follows the USN.
 *
 * On read, we must:
 *   1. Verify that each sector's last 2 bytes match the USN (detect tears)
 *   2. Restore the original bytes from the USA replacement entries
 *
 * Layout of USA (starting at usa_offset in the record):
 *   [USN: 2 bytes] [replacement_0: 2 bytes] [replacement_1: 2 bytes] ...
 *
 * usa_size (in words) = 1 (USN) + num_sectors.
 * ============================================================================ */

int ntfs_apply_fixup(uint8_t *buf, uint32_t record_size, uint16_t sector_size)
{
    uint16_t usa_offset;
    uint16_t usa_size_words;
    uint16_t usn;
    uint32_t num_sectors;
    uint32_t i;
    uint16_t *usa_array;

    if (!buf || sector_size == 0 || record_size < sector_size)
        return NTFS_ERR_FIXUP;

    /* Read USA offset and size from the record header */
    usa_offset     = ntfs_le16(buf + 0x04);
    usa_size_words = ntfs_le16(buf + 0x06);

    /* Sanity: USA must fit within the record */
    if (usa_offset + usa_size_words * 2 > record_size) {
        klog(LOG_DEBUG, "ntfs",
             "FIXUP: USA overflows record (off=%u, words=%u, rec=%u)",
             (uint64_t)usa_offset, (uint64_t)usa_size_words,
             (uint64_t)record_size);
        return NTFS_ERR_FIXUP;
    }

    num_sectors = record_size / sector_size;

    /* usa_size_words should be 1 (USN) + num_sectors */
    if (usa_size_words != num_sectors + 1) {
        klog(LOG_DEBUG, "ntfs",
             "FIXUP: USA size mismatch (words=%u, expected %u)",
             (uint64_t)usa_size_words, (uint64_t)(num_sectors + 1));
        return NTFS_ERR_FIXUP;
    }

    /* Pointer to the USA array in the buffer */
    usa_array = (uint16_t *)(buf + usa_offset);

    /* First entry is the Update Sequence Number */
    usn = usa_array[0];

    /* Verify and replace for each sector */
    for (i = 0; i < num_sectors; i++) {
        uint32_t last_word_off = (i + 1) * sector_size - 2;
        uint16_t on_disk_val   = ntfs_le16(buf + last_word_off);

        if (on_disk_val != usn) {
            klog(LOG_DEBUG, "ntfs",
                 "FIXUP FAILED: sector %u, expected USN 0x%x, got 0x%x (sector tear)",
                 (uint64_t)i, (uint64_t)usn, (uint64_t)on_disk_val);
            return NTFS_ERR_FIXUP;
        }

        /* Restore original bytes from USA replacement entry [i+1] */
        buf[last_word_off]     = (uint8_t)(usa_array[i + 1] & 0xFF);
        buf[last_word_off + 1] = (uint8_t)(usa_array[i + 1] >> 8);
    }

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_regenerate_fixup — Regenerate Update Sequence Array for writing
 *
 * This is the REVERSE of ntfs_apply_fixup().  Before writing a multi-sector
 * record (FILE, INDX) back to disk, we must:
 *   1. Increment the USN (wrap 0 → 1, since USN 0 is invalid)
 *   2. For each sector, save the original last 2 bytes into usa_array[i+1]
 *   3. Stamp each sector's last 2 bytes with the new USN
 *
 * After this, the record can be safely written to disk.  On the next read,
 * ntfs_apply_fixup() will verify the USN stamps and restore the originals.
 * ============================================================================ */

int ntfs_regenerate_fixup(uint8_t *buf, uint32_t record_size,
                          uint16_t sector_size)
{
    uint16_t usa_offset;
    uint16_t usa_size_words;
    uint16_t usn;
    uint32_t num_sectors;
    uint32_t i;

    if (!buf || sector_size == 0 || record_size < sector_size)
        return NTFS_ERR_FIXUP;

    /* Read USA offset and size from the record header */
    usa_offset     = ntfs_le16(buf + 0x04);
    usa_size_words = ntfs_le16(buf + 0x06);

    /* Sanity: USA must fit within the record */
    if (usa_offset + usa_size_words * 2 > record_size) {
        klog(LOG_DEBUG, "ntfs",
             "REGEN FIXUP: USA overflows record (off=%u, words=%u, rec=%u)",
             (uint64_t)usa_offset, (uint64_t)usa_size_words,
             (uint64_t)record_size);
        return NTFS_ERR_FIXUP;
    }

    num_sectors = record_size / sector_size;

    /* usa_size_words should be 1 (USN) + num_sectors */
    if (usa_size_words != num_sectors + 1) {
        klog(LOG_DEBUG, "ntfs",
             "REGEN FIXUP: USA size mismatch (words=%u, expected %u)",
             (uint64_t)usa_size_words, (uint64_t)(num_sectors + 1));
        return NTFS_ERR_FIXUP;
    }

    /* Step 1: Increment USN (wrap 0 → 1 since 0 is invalid) */
    usn = ntfs_le16(buf + usa_offset);
    usn++;
    if (usn == 0)
        usn = 1;

    /* Write new USN back to the USA header (first word) */
    ntfs_le16_write(buf + usa_offset, usn);

    /* Step 2+3: For each sector, save original last-2-bytes, stamp with USN */
    for (i = 0; i < num_sectors; i++) {
        uint32_t last_word_off = (i + 1) * sector_size - 2;
        uint16_t usa_entry_off = usa_offset + 2 + i * 2; /* usa_array[i+1] */

        /* Save original last 2 bytes into USA replacement entry */
        buf[usa_entry_off]     = buf[last_word_off];
        buf[usa_entry_off + 1] = buf[last_word_off + 1];

        /* Stamp last 2 bytes of sector with new USN */
        ntfs_le16_write(buf + last_word_off, usn);
    }

    return NTFS_OK;
}
