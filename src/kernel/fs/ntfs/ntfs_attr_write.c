/* ============================================================================
 * ntfs_attr_write.c — Attribute Writer (§12.4)
 *
 * Provides functions to add, update, and remove attributes in MFT records,
 * and to encode data runs (reverse of §4.1 decoder).
 *
 * Key operations:
 *   ntfs_encode_data_runs()  — Encode run array → on-disk byte format
 *   ntfs_attr_add()          — Insert new attribute (resident or non-resident)
 *   ntfs_attr_update()       — Modify existing attribute content
 *   ntfs_attr_remove()       — Delete attribute and free clusters
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ============================================================================
 * Data Run Encoder — reverse of ntfs_runlist.c
 *
 * On-disk format per run: [header:1] [length:N] [offset:M]
 *   header = (off_size << 4) | len_size
 *   length = unsigned LE integer (N bytes, N = len_size)
 *   offset = signed LE integer (M bytes, M = off_size), relative to prev LCN
 *
 * Run-list ends with a 0x00 byte.
 * ============================================================================ */

/* Determine the minimum number of bytes to represent an unsigned value */
static int unsigned_size(uint64_t val)
{
    if (val == 0) return 1;
    if (val <= 0xFF) return 1;
    if (val <= 0xFFFF) return 2;
    if (val <= 0xFFFFFF) return 3;
    if (val <= 0xFFFFFFFF) return 4;
    if (val <= 0xFFFFFFFFFF) return 5;
    if (val <= 0xFFFFFFFFFFFF) return 6;
    if (val <= 0xFFFFFFFFFFFFFF) return 7;
    return 8;
}

/* Determine the minimum number of bytes to represent a signed value.
 * Must use sign-extended encoding — high bit of last byte is the sign. */
static int signed_size(int64_t val)
{
    if (val >= -0x80 && val <= 0x7F) return 1;
    if (val >= -0x8000 && val <= 0x7FFF) return 2;
    if (val >= -0x800000 && val <= 0x7FFFFF) return 3;
    if (val >= -0x80000000LL && val <= 0x7FFFFFFFLL) return 4;
    if (val >= -0x8000000000LL && val <= 0x7FFFFFFFFFLL) return 5;
    if (val >= -0x800000000000LL && val <= 0x7FFFFFFFFFFFLL) return 6;
    if (val >= -0x80000000000000LL && val <= 0x7FFFFFFFFFFFFFLL) return 7;
    return 8;
}

/* Write N bytes of an unsigned value (little-endian) */
static void write_unsigned(uint8_t *p, uint64_t val, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
}

/* Write N bytes of a signed value (little-endian) */
static void write_signed(uint8_t *p, int64_t val, int n)
{
    write_unsigned(p, (uint64_t)val, n);
}

int ntfs_encode_data_runs(const struct ntfs_data_run *runs, int count,
                          uint8_t *buffer, int buf_size)
{
    int pos = 0;
    int64_t prev_lcn = 0;
    int i;

    if (!runs || !buffer || count <= 0 || buf_size <= 0)
        return -1;

    for (i = 0; i < count; i++) {
        int len_size;
        int off_size;
        int64_t offset;
        int run_bytes;

        len_size = unsigned_size(runs[i].length);

        if (runs[i].lcn == NTFS_LCN_SPARSE) {
            /* Sparse run — no offset field */
            off_size = 0;
        } else {
            offset = (int64_t)runs[i].lcn - prev_lcn;
            off_size = signed_size(offset);
        }

        /* Check if we have room: 1 (header) + len_size + off_size + 1 (term) */
        run_bytes = 1 + len_size + off_size;
        if (pos + run_bytes + 1 > buf_size)
            return -1;  /* Buffer overflow */

        /* Write header byte */
        buffer[pos++] = (uint8_t)((off_size << 4) | len_size);

        /* Write length (unsigned) */
        write_unsigned(buffer + pos, runs[i].length, len_size);
        pos += len_size;

        /* Write offset (signed, relative) */
        if (off_size > 0) {
            offset = (int64_t)runs[i].lcn - prev_lcn;
            write_signed(buffer + pos, offset, off_size);
            pos += off_size;
            prev_lcn = (int64_t)runs[i].lcn;
        }
    }

    /* Write run-list terminator */
    if (pos < buf_size)
        buffer[pos++] = 0x00;
    else
        return -1;

    return pos;  /* Bytes written */
}

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Re-parse MFT record header from raw bytes (for in-memory record after edits) */
static void reparse_header(const uint8_t *rec, struct ntfs_mft_header *hdr)
{
    hdr->magic         = ntfs_le32(rec + 0x00);
    hdr->usa_offset    = ntfs_le16(rec + 0x04);
    hdr->usa_size      = ntfs_le16(rec + 0x06);
    hdr->lsn           = ntfs_le64(rec + 0x08);
    hdr->seq_number    = ntfs_le16(rec + 0x10);
    hdr->hard_link_count = ntfs_le16(rec + 0x12);
    hdr->attrs_offset  = ntfs_le16(rec + 0x14);
    hdr->flags         = ntfs_le16(rec + 0x16);
    hdr->used_size     = ntfs_le32(rec + 0x18);
    hdr->alloc_size    = ntfs_le32(rec + 0x1C);
    hdr->base_record_ref = ntfs_le64(rec + 0x20);
}

/* Find the insertion point for a new attribute: attributes must be sorted
 * by type ID. Returns the offset within the record where the new attribute
 * should be inserted (before the first attribute with type > new_type,
 * or before $END if none). */
static uint32_t find_insert_point(const uint8_t *rec,
                                   const struct ntfs_mft_header *hdr,
                                   uint32_t new_type)
{
    const uint8_t *attr = rec + hdr->attrs_offset;
    uint32_t offset = hdr->attrs_offset;

    while (offset + 4 <= hdr->used_size) {
        uint32_t type = ntfs_le32(attr);

        if (type == NTFS_ATTR_END || type > new_type)
            return offset;

        {
            uint32_t len = ntfs_le32(attr + 0x04);
            if (len == 0 || offset + len > hdr->used_size)
                break;
            offset += len;
            attr = rec + offset;
        }
    }

    return offset;  /* Before $END or end of attributes */
}

/* Move bytes within the record buffer to open/close a gap.
 * Positive delta = open gap (shift right), negative = close gap (shift left).
 * Returns new used_size after the move. */
static uint32_t shift_attrs(uint8_t *rec, uint32_t frs_size,
                             uint32_t from_off, uint32_t old_used,
                             int32_t delta)
{
    uint32_t new_used = (uint32_t)((int32_t)old_used + delta);
    uint32_t bytes_to_move;
    uint32_t i;

    if (new_used > frs_size)
        return 0;  /* Would overflow */

    bytes_to_move = old_used - from_off;

    if (delta > 0) {
        /* Shift right — move from end to avoid overlap */
        for (i = bytes_to_move; i > 0; i--)
            rec[from_off + (uint32_t)delta + i - 1] = rec[from_off + i - 1];
    } else if (delta < 0) {
        /* Shift left — move from start */
        uint32_t abs_delta = (uint32_t)(-delta);
        for (i = 0; i < bytes_to_move; i++)
            rec[from_off - abs_delta + i] = rec[from_off + i];
    }

    /* Update used_size in the record header */
    ntfs_le32_write(rec + 0x18, new_used);

    return new_used;
}

/* Find the next unused attribute instance ID in a record.
 * Scans all attributes and returns max_id + 1. */
static uint16_t next_attr_id(const uint8_t *rec,
                              const struct ntfs_mft_header *hdr)
{
    const uint8_t *attr;
    uint16_t max_id = 0;

    attr = rec + hdr->attrs_offset;
    while ((uint32_t)(attr - rec) + 4 <= hdr->used_size) {
        uint32_t type = ntfs_le32(attr);
        if (type == NTFS_ATTR_END)
            break;

        {
            uint16_t id = ntfs_le16(attr + 0x0E);
            uint32_t len = ntfs_le32(attr + 0x04);
            if (id >= max_id)
                max_id = id + 1;
            if (len == 0)
                break;
            attr += len;
        }
    }

    return max_id;
}

/* Build a resident attribute header + content into a buffer.
 * Returns the total attribute length (aligned to 8 bytes). */
static uint32_t build_resident_attr(uint8_t *out, uint32_t type,
                                     const char *name, uint8_t name_len,
                                     uint16_t attr_id,
                                     const void *data, uint32_t data_len)
{
    uint32_t name_off;
    uint32_t content_off;
    uint32_t total_len;

    /* Layout: [common header 0x00-0x0F][resident header 0x10-0x17]
     *         [name if any][content] */

    name_off = 0x18;  /* Resident header ends at 0x18 */
    content_off = name_off + (uint32_t)name_len * 2;
    /* Align content to 8 bytes */
    content_off = (content_off + 7) & ~7u;
    total_len = content_off + data_len;
    /* Align total to 8 bytes */
    total_len = (total_len + 7) & ~7u;

    /* Zero the entire attribute area */
    ntfs_memset(out, 0, total_len);

    /* Common header */
    ntfs_le32_write(out + 0x00, type);             /* Type */
    ntfs_le32_write(out + 0x04, total_len);        /* Total length */
    out[0x08] = 0;                                  /* Resident */
    out[0x09] = name_len;                           /* Name length */
    ntfs_le16_write(out + 0x0A, (uint16_t)name_off); /* Name offset */
    ntfs_le16_write(out + 0x0C, 0);                /* Flags */
    ntfs_le16_write(out + 0x0E, attr_id);          /* Attribute ID */

    /* Resident header */
    ntfs_le32_write(out + 0x10, data_len);         /* Content length */
    ntfs_le16_write(out + 0x14, (uint16_t)content_off); /* Content offset */
    ntfs_le16_write(out + 0x16, 0);                /* Indexed flag */

    /* Write attribute name (UTF-16LE) */
    if (name_len > 0 && name) {
        uint8_t i;
        for (i = 0; i < name_len; i++) {
            ntfs_le16_write(out + name_off + i * 2,
                            (uint16_t)(uint8_t)name[i]);
        }
    }

    /* Write content */
    if (data && data_len > 0)
        ntfs_memcpy(out + content_off, data, data_len);

    return total_len;
}

/* Build a non-resident attribute header with encoded data runs.
 * Returns the total attribute length (aligned to 8 bytes). */
static uint32_t build_nonresident_attr(uint8_t *out, uint32_t type,
                                        const char *name, uint8_t name_len,
                                        uint16_t attr_id,
                                        const struct ntfs_data_run *runs,
                                        int run_count,
                                        uint64_t alloc_size,
                                        uint64_t real_size)
{
    uint32_t name_off;
    uint32_t run_off;
    int encoded_len;
    uint32_t total_len;
    uint64_t last_vcn = 0;
    int i;

    /* Layout: [common header 0x00-0x0F][non-resident header 0x10-0x3F]
     *         [name if any][data runs][0x00 terminator] */

    name_off = 0x40;  /* Non-resident header ends at 0x40 */
    run_off = name_off + (uint32_t)name_len * 2;
    /* Align run offset to 8 bytes */
    run_off = (run_off + 7) & ~7u;

    /* Calculate last VCN from runs */
    for (i = 0; i < run_count; i++)
        last_vcn += runs[i].length;
    if (last_vcn > 0)
        last_vcn--;  /* last_vcn is inclusive */

    /* Encode data runs into a temp buffer */
    {
        uint8_t run_buf[512];
        encoded_len = ntfs_encode_data_runs(runs, run_count,
                                             run_buf, 512);
        if (encoded_len <= 0)
            return 0;

        total_len = run_off + (uint32_t)encoded_len;
        /* Align to 8 bytes */
        total_len = (total_len + 7) & ~7u;

        /* Zero the attribute area */
        ntfs_memset(out, 0, total_len);

        /* Common header */
        ntfs_le32_write(out + 0x00, type);
        ntfs_le32_write(out + 0x04, total_len);
        out[0x08] = 1;  /* Non-resident */
        out[0x09] = name_len;
        ntfs_le16_write(out + 0x0A, (uint16_t)name_off);
        ntfs_le16_write(out + 0x0C, 0);  /* Flags */
        ntfs_le16_write(out + 0x0E, attr_id);

        /* Non-resident header */
        ntfs_le64_write(out + 0x10, 0);            /* Start VCN */
        ntfs_le64_write(out + 0x18, last_vcn);     /* Last VCN */
        ntfs_le16_write(out + 0x20, (uint16_t)run_off); /* Run offset */
        ntfs_le16_write(out + 0x22, 0);            /* Compression unit */
        /* 4 bytes padding at 0x24 */
        ntfs_le64_write(out + 0x28, alloc_size);   /* Allocated size */
        ntfs_le64_write(out + 0x30, real_size);    /* Real size */
        ntfs_le64_write(out + 0x38, real_size);    /* Initialized size */

        /* Write name */
        if (name_len > 0 && name) {
            uint8_t ni;
            for (ni = 0; ni < name_len; ni++) {
                ntfs_le16_write(out + name_off + ni * 2,
                                (uint16_t)(uint8_t)name[ni]);
            }
        }

        /* Copy encoded runs */
        ntfs_memcpy(out + run_off, run_buf, (uint64_t)encoded_len);
    }

    return total_len;
}

/* ============================================================================
 * ntfs_attr_add — Add a new attribute to an MFT record
 *
 * Inserts a new attribute at the correct sorted position.
 * If data fits within the record, creates a resident attribute.
 * If data is too large, allocates clusters and creates non-resident.
 *
 * vol: needed for cluster allocation (non-resident path)
 * rec: in-memory MFT record buffer (after fixup removal)
 * hdr: parsed header (updated on return with new used_size)
 * frs_size: record size (typically 1024)
 * type: attribute type ID (must be sorted in type order)
 * name: ASCII name (NULL for unnamed)
 * data: attribute content bytes
 * data_len: number of content bytes
 * ============================================================================ */

int ntfs_attr_add(struct ntfs_volume *vol, uint8_t *rec,
                  struct ntfs_mft_header *hdr, uint32_t frs_size,
                  uint32_t type, const char *name,
                  const void *data, uint32_t data_len)
{
    uint8_t name_len = 0;
    uint32_t insert_off;
    uint32_t attr_len;
    uint32_t free_space;
    uint16_t attr_id;
    uint32_t new_used;

    if (!rec || !hdr || frs_size == 0)
        return NTFS_ERR_IO;

    if (name) {
        const char *p = name;
        while (*p) { name_len++; p++; }
    }

    insert_off = find_insert_point(rec, hdr, type);
    attr_id = next_attr_id(rec, hdr);
    free_space = hdr->alloc_size - hdr->used_size;

    /* Try resident first: calculate needed size */
    {
        uint32_t content_off = 0x18 + (uint32_t)name_len * 2;
        content_off = (content_off + 7) & ~7u;
        attr_len = content_off + data_len;
        attr_len = (attr_len + 7) & ~7u;
    }

    if (attr_len <= free_space) {
        /* Fits as resident — insert inline */
        uint8_t attr_buf[1024];

        attr_len = build_resident_attr(attr_buf, type, name, name_len,
                                        attr_id, data, data_len);

        /* Open gap in record for the new attribute */
        new_used = shift_attrs(rec, frs_size, insert_off,
                                hdr->used_size, (int32_t)attr_len);
        if (new_used == 0)
            return NTFS_ERR_IO;

        /* Copy attribute into the gap */
        ntfs_memcpy(rec + insert_off, attr_buf, attr_len);

        /* Refresh header */
        reparse_header(rec, hdr);

        klog(LOG_DEBUG, "ntfs",
             "attr_add: resident type 0x%x, %u bytes at offset %u",
             (uint64_t)type, (uint64_t)data_len, (uint64_t)insert_off);

        return NTFS_OK;
    }

    /* Data too large for resident — create non-resident */
    if (!vol)
        return NTFS_ERR_IO;  /* Need volume for cluster allocation */

    {
        uint64_t clusters_needed;
        uint64_t lcn;
        struct ntfs_data_run run;
        uint8_t attr_buf[1024];
        uint64_t alloc_size;

        clusters_needed = (data_len + vol->cluster_size - 1) / vol->cluster_size;
        lcn = ntfs_alloc_clusters(vol, clusters_needed, vol->mft_lcn);
        if (lcn == 0) {
            klog(LOG_ERROR, "ntfs",
                 "attr_add: failed to allocate %llu clusters for type 0x%x",
                 clusters_needed, (uint64_t)type);
            return NTFS_ERR_IO;
        }

        alloc_size = clusters_needed * vol->cluster_size;

        /* Build a single data run */
        run.vcn_start = 0;
        run.lcn = lcn;
        run.length = clusters_needed;

        attr_len = build_nonresident_attr(attr_buf, type, name, name_len,
                                           attr_id, &run, 1,
                                           alloc_size, data_len);
        if (attr_len == 0 || attr_len > free_space) {
            ntfs_free_clusters(vol, lcn, clusters_needed);
            return NTFS_ERR_IO;
        }

        /* Write data to allocated clusters */
        {
            uint64_t disk_lba = (lcn * vol->cluster_size) /
                                vol->bytes_per_sector;
            uint32_t sect_count = (uint32_t)(alloc_size /
                                              vol->bytes_per_sector);

            /* Use a bounce buffer to handle partial last cluster */
            uint8_t *bounce = (uint8_t *)kmalloc((uint32_t)alloc_size);
            if (!bounce) {
                ntfs_free_clusters(vol, lcn, clusters_needed);
                return NTFS_ERR_IO;
            }
            ntfs_memset(bounce, 0, alloc_size);
            if (data && data_len > 0)
                ntfs_memcpy(bounce, data, data_len);

            if (blkdev_write(vol->dev, disk_lba, sect_count, bounce) != 0) {
                kfree(bounce);
                ntfs_free_clusters(vol, lcn, clusters_needed);
                return NTFS_ERR_IO;
            }
            kfree(bounce);
        }

        /* Insert attribute header into record */
        new_used = shift_attrs(rec, frs_size, insert_off,
                                hdr->used_size, (int32_t)attr_len);
        if (new_used == 0) {
            ntfs_free_clusters(vol, lcn, clusters_needed);
            return NTFS_ERR_IO;
        }

        ntfs_memcpy(rec + insert_off, attr_buf, attr_len);
        reparse_header(rec, hdr);

        klog(LOG_DEBUG, "ntfs",
             "attr_add: non-resident type 0x%x, %u bytes, LCN %llu",
             (uint64_t)type, (uint64_t)data_len, lcn);

        return NTFS_OK;
    }
}

/* ============================================================================
 * ntfs_attr_update — Update an existing attribute's content
 *
 * For resident: updates content in-place, adjusting lengths if needed.
 * For non-resident: writes data to existing clusters (extends if needed).
 * ============================================================================ */

int ntfs_attr_update(struct ntfs_volume *vol, uint8_t *rec,
                     struct ntfs_mft_header *hdr, uint32_t frs_size,
                     uint32_t type, const char *name,
                     const void *data, uint32_t data_len)
{
    struct ntfs_attr_header ah;
    const uint8_t *found;
    uint32_t attr_off;

    if (!rec || !hdr)
        return NTFS_ERR_IO;

    /* Find the attribute */
    if (name)
        found = ntfs_attr_find_named(rec, hdr, type, name, &ah);
    else
        found = ntfs_attr_find(rec, hdr, type, &ah);

    if (!found)
        return NTFS_ERR_NOT_FOUND;

    attr_off = (uint32_t)(found - rec);

    if (ah.non_resident == 0) {
        /* ---- Resident attribute update ---- */
        uint32_t old_content_len = ah.content_length;
        uint32_t old_total_len = ah.total_length;
        int32_t content_delta = (int32_t)data_len - (int32_t)old_content_len;

        /* Calculate new total length */
        uint32_t new_total = ah.content_offset + data_len;
        new_total = (new_total + 7) & ~7u;

        int32_t total_delta = (int32_t)new_total - (int32_t)old_total_len;

        /* Check if the record has space */
        if (total_delta > 0 &&
            (uint32_t)total_delta > hdr->alloc_size - hdr->used_size) {
            /* Need to convert to non-resident */
            if (!vol)
                return NTFS_ERR_IO;

            /* Remove old attribute and re-add as non-resident */
            {
                int rc = ntfs_attr_remove(vol, rec, hdr, frs_size,
                                           type, name);
                if (rc != NTFS_OK)
                    return rc;
                return ntfs_attr_add(vol, rec, hdr, frs_size,
                                      type, name, data, data_len);
            }
        }

        /* Shift subsequent attributes if size changed */
        if (total_delta != 0) {
            uint32_t after_off = attr_off + old_total_len;
            uint32_t new_used = shift_attrs(rec, frs_size, after_off,
                                             hdr->used_size, total_delta);
            if (new_used == 0)
                return NTFS_ERR_IO;
        }

        /* Update attribute header lengths */
        ntfs_le32_write(rec + attr_off + 0x04, new_total);  /* Total length */
        ntfs_le32_write(rec + attr_off + 0x10, data_len);   /* Content length */

        /* Write new content */
        if (data && data_len > 0)
            ntfs_memcpy(rec + attr_off + ah.content_offset, data, data_len);

        /* Zero padding between content end and total end */
        {
            uint32_t content_end = ah.content_offset + data_len;
            if (content_end < new_total) {
                ntfs_memset(rec + attr_off + content_end, 0,
                            new_total - content_end);
            }
        }

        reparse_header(rec, hdr);
        (void)content_delta;
        (void)old_content_len;

        return NTFS_OK;
    }

    /* ---- Non-resident attribute update ---- */
    if (!vol)
        return NTFS_ERR_IO;

    {
        /* Read existing run info from the attribute */
        uint64_t old_alloc = ntfs_le64(rec + attr_off + 0x28);
        uint64_t old_real = ntfs_le64(rec + attr_off + 0x30);
        struct ntfs_data_run old_runs[64];
        struct ntfs_nonres_header nrhdr;
        int old_run_count;

        old_run_count = ntfs_decode_data_runs(rec + attr_off, old_runs,
                                               64, &nrhdr);
        if (old_run_count <= 0)
            return NTFS_ERR_BAD_MAGIC;

        if (data_len <= old_alloc) {
            /* Data fits in existing allocation — just write to clusters */
            uint32_t bytes_left = data_len;
            const uint8_t *src = (const uint8_t *)data;
            int ri;

            for (ri = 0; ri < old_run_count && bytes_left > 0; ri++) {
                uint64_t run_bytes;
                uint32_t chunk;
                uint64_t disk_lba;
                uint32_t sects;
                uint8_t *bounce;

                if (old_runs[ri].lcn == NTFS_LCN_SPARSE)
                    continue;

                run_bytes = old_runs[ri].length * vol->cluster_size;
                chunk = (uint32_t)(run_bytes > bytes_left ?
                                    bytes_left : run_bytes);

                disk_lba = (old_runs[ri].lcn * vol->cluster_size) /
                           vol->bytes_per_sector;
                sects = (uint32_t)((run_bytes + vol->bytes_per_sector - 1) /
                                    vol->bytes_per_sector);

                bounce = (uint8_t *)kmalloc((uint32_t)run_bytes);
                if (!bounce)
                    return NTFS_ERR_IO;

                ntfs_memset(bounce, 0, run_bytes);
                ntfs_memcpy(bounce, src, chunk);

                if (blkdev_write(vol->dev, disk_lba, sects, bounce) != 0) {
                    kfree(bounce);
                    return NTFS_ERR_IO;
                }

                kfree(bounce);
                src += chunk;
                bytes_left -= chunk;
            }

            /* Update real_size and init_size in the attribute header */
            ntfs_le64_write(rec + attr_off + 0x30, (uint64_t)data_len);
            ntfs_le64_write(rec + attr_off + 0x38, (uint64_t)data_len);
            reparse_header(rec, hdr);
        } else {
            /* Need more clusters — remove and re-add with new allocation */
            int rc = ntfs_attr_remove(vol, rec, hdr, frs_size, type, name);
            if (rc != NTFS_OK)
                return rc;
            return ntfs_attr_add(vol, rec, hdr, frs_size,
                                  type, name, data, data_len);
        }

        (void)old_real;
    }

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_attr_remove — Remove an attribute from an MFT record
 *
 * Frees allocated clusters for non-resident attributes, shifts subsequent
 * attributes left to close the gap, and updates used_size.
 * ============================================================================ */

int ntfs_attr_remove(struct ntfs_volume *vol, uint8_t *rec,
                     struct ntfs_mft_header *hdr, uint32_t frs_size,
                     uint32_t type, const char *name)
{
    struct ntfs_attr_header ah;
    const uint8_t *found;
    uint32_t attr_off;
    uint32_t attr_len;
    uint32_t new_used;

    if (!rec || !hdr)
        return NTFS_ERR_IO;

    /* Find the attribute */
    if (name)
        found = ntfs_attr_find_named(rec, hdr, type, name, &ah);
    else
        found = ntfs_attr_find(rec, hdr, type, &ah);

    if (!found)
        return NTFS_ERR_NOT_FOUND;

    attr_off = (uint32_t)(found - rec);
    attr_len = ah.total_length;

    /* Free clusters if non-resident */
    if (ah.non_resident && vol) {
        struct ntfs_data_run runs[64];
        struct ntfs_nonres_header nrhdr;
        int run_count;
        int i;

        run_count = ntfs_decode_data_runs(rec + attr_off, runs, 64, &nrhdr);
        for (i = 0; i < run_count; i++) {
            if (runs[i].lcn != NTFS_LCN_SPARSE)
                ntfs_free_clusters(vol, runs[i].lcn, runs[i].length);
        }
    }

    /* Close the gap: shift everything after this attribute left */
    {
        uint32_t after_off = attr_off + attr_len;
        new_used = shift_attrs(rec, frs_size, after_off,
                                hdr->used_size, -(int32_t)attr_len);
        if (new_used == 0)
            return NTFS_ERR_IO;
    }

    reparse_header(rec, hdr);

    klog(LOG_DEBUG, "ntfs",
         "attr_remove: type 0x%x, freed %u bytes at offset %u",
         (uint64_t)type, (uint64_t)attr_len, (uint64_t)attr_off);

    return NTFS_OK;
}
