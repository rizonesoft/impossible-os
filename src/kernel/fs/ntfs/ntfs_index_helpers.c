/* ============================================================================
 * ntfs_index_helpers.c -- B+ Tree Index Mutation Helpers (§14.1)
 *
 * Low-level primitives shared by ntfs_index_insert.c and ntfs_index_delete.c:
 *   - ntfs_write_indx()       Write an INDX buffer to disk (USA + blkdev_write)
 *   - ntfs_indx_alloc_vcn()   Allocate a new INDX VCN slot + $BITMAP bit
 *   - ntfs_indx_free_vcn()    Free an INDX VCN slot + clear $BITMAP bit
 *   - ntfs_index_compare()    Case-insensitive UTF-16LE name comparison
 *   - ntfs_index_find_pos()   Scan a node for insert/compare position
 *   - ntfs_build_indx_buf()   Build a fresh empty INDX buffer at a given VCN
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* Maximum data runs for $INDEX_ALLOCATION (shared with ntfs_io.c) */
#define NTFS_INDEX_MAX_RUNS  64

/* INDX record header: magic(4) + usa_off(2) + usa_size(2) + lsn(8) + vcn(8) */
#define INDX_HEADER_SIZE     0x28
/* Node header offset within INDX buffer (after the INDX record header) */
#define INDX_NODE_HDR_OFF    0x18

/* Sentinel that means "no child VCN" -- used throughout B+ tree traversal */
#define VCN_NONE  ((uint64_t)-1)

/* ============================================================================
 * ntfs_write_indx -- Write INDX buffer to disk at a given VCN
 *
 * Counterpart to ntfs_read_indx().  Applies USA regeneration before writing
 * so the on-disk page has correct sector-end stamps.
 * Buffer must be in the "after fixup removal" (editable) state.
 * ============================================================================ */
int ntfs_write_indx(struct ntfs_volume *vol,
                    const struct ntfs_data_run *index_runs,
                    int index_run_count,
                    uint64_t vcn, uint32_t index_record_size,
                    uint8_t *buffer)
{
    uint64_t target_vcn;
    uint64_t run_vcn_end;
    uint64_t vcn_in_run;
    uint64_t disk_lcn;
    uint64_t lba;
    uint32_t sectors;
    int i;

    if (!vol || !index_runs || !buffer || index_run_count <= 0)
        return NTFS_ERR_IO;

    /* Apply USA regeneration before writing */
    if (ntfs_regenerate_fixup(buffer, index_record_size,
                               vol->bytes_per_sector) != NTFS_OK) {
        klog(LOG_ERROR, "ntfs", "write_indx: fixup failed at VCN %llu", vcn);
        return NTFS_ERR_FIXUP;
    }

    target_vcn = vcn;

    /* Find the data run covering this VCN */
    for (i = 0; i < index_run_count; i++) {
        run_vcn_end = index_runs[i].vcn_start + index_runs[i].length;
        if (target_vcn >= index_runs[i].vcn_start && target_vcn < run_vcn_end)
            break;
    }
    if (i >= index_run_count) {
        klog(LOG_ERROR, "ntfs",
             "write_indx: VCN %llu not within any data run", vcn);
        return NTFS_ERR_IO;
    }

    if (index_runs[i].lcn == NTFS_LCN_SPARSE)
        return NTFS_ERR_IO;

    vcn_in_run = target_vcn - index_runs[i].vcn_start;
    disk_lcn   = index_runs[i].lcn + vcn_in_run;

    lba     = disk_lcn * vol->sectors_per_cluster;
    sectors = index_record_size / vol->bytes_per_sector;
    if (sectors == 0) sectors = 1;

    if (blkdev_write(vol->dev, lba, sectors, buffer) != 0) {
        klog(LOG_ERROR, "ntfs",
             "write_indx: blkdev_write failed at VCN %llu (lba %llu)",
             vcn, lba);
        return NTFS_ERR_IO;
    }

    /* Re-apply fixup so the caller's buffer remains in editable state */
    ntfs_apply_fixup(buffer, index_record_size, vol->bytes_per_sector);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_index_compare -- Case-insensitive UTF-16LE name comparison
 *
 * Compares two UTF-16LE names using vol->upcase_table (from $UpCase).
 * Falls back to ASCII toupper if table is not loaded.
 * Returns negative / 0 / positive (like strcmp).
 * ============================================================================ */
int ntfs_index_compare(const struct ntfs_volume *vol,
                       const uint8_t *name_a, int len_a,
                       const uint8_t *name_b, int len_b)
{
    int i;
    int min_len = len_a < len_b ? len_a : len_b;

    for (i = 0; i < min_len; i++) {
        uint16_t a = ntfs_le16(name_a + i * 2);
        uint16_t b = ntfs_le16(name_b + i * 2);

        if (vol && vol->upcase_table) {
            a = vol->upcase_table[a & 0xFFFF];
            b = vol->upcase_table[b & 0xFFFF];
        } else {
            /* ASCII fallback */
            if (a >= 'a' && a <= 'z') a -= ('a' - 'A');
            if (b >= 'a' && b <= 'z') b -= ('a' - 'A');
        }

        if (a != b)
            return (int)a - (int)b;
    }
    return len_a - len_b;
}

/* ============================================================================
 * ntfs_index_find_pos -- Find position within a node's entry array
 *
 * Scans entries in the node (entries_base, total_entries_size) to find
 * where `name` sorts.
 *
 * On return:
 *   *out_offset = byte offset within entries area of the first entry >= name
 *                 (where the new entry should be inserted, or the entry to delete)
 *   *out_match  = 1 if an exact match was found (cmp == 0)
 *   *out_child_vcn = child VCN of the entry just before/at the position
 *                    (VCN_NONE if leaf)
 * ============================================================================ */
void ntfs_index_find_pos(const struct ntfs_volume *vol,
                         const uint8_t *entries_base,
                         uint32_t total_entries_size,
                         const uint8_t *search_name_utf16, int search_name_len,
                         uint32_t *out_offset,
                         int *out_match,
                         uint64_t *out_child_vcn)
{
    uint32_t pos = 0;

    *out_match     = 0;
    *out_child_vcn = VCN_NONE;

    while (pos < total_entries_size) {
        uint16_t e_len   = ntfs_le16(entries_base + pos + 0x08);
        uint8_t  e_flags = entries_base[pos + 0x0C];

        if (e_len < 0x10) break; /* Corrupt */

        /* Sentinel -- insert / stop before it */
        if (e_flags & NTFS_INDEX_ENTRY_LAST) {
            if (e_flags & NTFS_INDEX_ENTRY_SUBNODE)
                *out_child_vcn = ntfs_le64(entries_base + pos + e_len - 8);
            *out_offset = pos;
            return;
        }

        /* Get this entry's UTF-16LE name */
        {
            const uint8_t *e_fn    = entries_base + pos + 0x10;
            int            e_nlen  = (int)(uint8_t)e_fn[0x40];
            const uint8_t *e_name  = e_fn + 0x42;
            int cmp = ntfs_index_compare(vol,
                                         search_name_utf16, search_name_len,
                                         e_name, e_nlen);

            if (cmp == 0) {
                *out_match     = 1;
                *out_offset    = pos;
                if (e_flags & NTFS_INDEX_ENTRY_SUBNODE)
                    *out_child_vcn = ntfs_le64(entries_base + pos + e_len - 8);
                return;
            }
            if (cmp < 0) {
                /* New entry sorts before this one -- insert here */
                if (e_flags & NTFS_INDEX_ENTRY_SUBNODE)
                    *out_child_vcn = ntfs_le64(entries_base + pos + e_len - 8);
                *out_offset = pos;
                return;
            }
        }

        pos += e_len;
    }

    *out_offset = pos; /* Should not happen normally */
}

/* ============================================================================
 * ntfs_build_indx_buf -- Initialise a blank INDX buffer in-memory
 *
 * Sets INDX magic, LSN=0, VCN, USA (size 9 for 4096-byte record / 512-byte
 * sectors = 8 sectors + 1 header word), and an empty node with sentinel.
 * Caller must apply ntfs_regenerate_fixup() before writing to disk.
 * ============================================================================ */
void ntfs_build_indx_buf(uint8_t *buf, uint32_t record_size,
                          uint16_t bytes_per_sector, uint64_t vcn)
{
    uint32_t sectors        = record_size / bytes_per_sector;
    uint16_t usa_size       = (uint16_t)(sectors + 1); /* +1 for USN header */
    uint16_t usa_offset     = 0x28;                    /* Fixed offset after INDX header */
    uint32_t node_hdr_off   = INDX_NODE_HDR_OFF;
    uint32_t entries_off    = usa_offset + usa_size * 2; /* USA array end */
    /* Align entries_off to 8 bytes */
    entries_off = (entries_off + 7) & ~7u;

    ntfs_memset(buf, 0, record_size);

    /* Magic "INDX" */
    buf[0] = 'I'; buf[1] = 'N'; buf[2] = 'D'; buf[3] = 'X';

    /* USA offset and size */
    ntfs_le16_write(buf + 0x04, usa_offset);
    ntfs_le16_write(buf + 0x06, usa_size);

    /* LSN = 0 (will be updated by journal) */

    /* VCN of this buffer within $INDEX_ALLOCATION */
    ntfs_le64_write(buf + 0x10, vcn);

    /* Node header at INDX_NODE_HDR_OFF (0x18):
     *   entries_offset (relative to node header start)
     *   total_size
     *   alloc_size
     *   flags (0 = leaf)
     */
    uint32_t rel_entries = entries_off - node_hdr_off; /* offset from node hdr */
    uint32_t sentinel_len = 0x10; /* 16-byte sentinel entry */
    uint32_t total_entries = rel_entries + sentinel_len;

    ntfs_le32_write(buf + node_hdr_off + 0x00, rel_entries);
    ntfs_le32_write(buf + node_hdr_off + 0x04, total_entries);
    ntfs_le32_write(buf + node_hdr_off + 0x08,
                    record_size - node_hdr_off - rel_entries);
    buf[node_hdr_off + 0x0C] = 0; /* leaf */

    /* Sentinel entry */
    uint8_t *sent = buf + node_hdr_off + rel_entries;
    ntfs_le16_write(sent + 0x08, (uint16_t)sentinel_len); /* entry_length */
    sent[0x0C] = NTFS_INDEX_ENTRY_LAST;
}

/* ============================================================================
 * ntfs_indx_get_ia_runs -- Load $INDEX_ALLOCATION and $BITMAP runs for dir
 *
 * Reads the directory's MFT record (already in 'rec'), finds the
 * $INDEX_ALLOCATION ($I30) and $BITMAP ($I30) attributes, decodes their
 * data runs / content, and returns them via out params.
 *
 * ia_runs / ia_run_count: $INDEX_ALLOCATION data runs
 * bm_runs / bm_run_count: $BITMAP data runs (or bm_resident + bm_res_data)
 * ============================================================================ */
int ntfs_indx_get_ia_runs(const uint8_t *rec,
                          const struct ntfs_mft_header *hdr,
                          struct ntfs_data_run *ia_runs, int *ia_run_count,
                          struct ntfs_data_run *bm_runs, int *bm_run_count,
                          uint8_t *bm_resident_data, uint32_t *bm_resident_len,
                          int *bm_is_resident)
{
    struct ntfs_attr_header ia_ah, bm_ah;
    const uint8_t *ia_attr, *bm_attr;

    *ia_run_count = 0;
    *bm_run_count = 0;
    *bm_resident_len = 0;
    *bm_is_resident = 0;

    ia_attr = ntfs_attr_find_named(rec, hdr, NTFS_ATTR_INDEX_ALLOCATION,
                                   "$I30", &ia_ah);
    if (ia_attr && ia_ah.non_resident) {
        *ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs,
                                               NTFS_INDEX_MAX_RUNS, NULL);
        if (*ia_run_count < 0) *ia_run_count = 0;
    }

    bm_attr = ntfs_attr_find_named(rec, hdr, NTFS_ATTR_BITMAP, "$I30", &bm_ah);
    if (!bm_attr)
        return NTFS_OK; /* No bitmap yet -- ok for leaf-only directories */

    if (bm_ah.non_resident) {
        *bm_run_count = ntfs_decode_data_runs(bm_attr, bm_runs,
                                               NTFS_INDEX_MAX_RUNS, NULL);
        if (*bm_run_count < 0) *bm_run_count = 0;
    } else {
        /* Resident $BITMAP -- small dirs -- copy content */
        uint32_t len = bm_ah.content_length;
        if (len > 64) len = 64; /* Safety cap */
        ntfs_memcpy(bm_resident_data, bm_attr + bm_ah.content_offset, len);
        *bm_resident_len = len;
        *bm_is_resident  = 1;
    }

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_indx_vcn_is_active -- Check if a VCN slot is active in $I30 $BITMAP
 *
 * Each bit in the bitmap corresponds to one $INDEX_ALLOCATION VCN.
 * Returns 1 if active, 0 if free.
 * For resident bitmaps, bm_resident_data / bm_resident_len are used.
 * For non-resident bitmaps, reads a byte from disk via ntfs_read_data.
 * ============================================================================ */
int ntfs_indx_vcn_is_active(struct ntfs_volume *vol,
                             uint64_t vcn,
                             const uint8_t *bm_resident, uint32_t bm_res_len,
                             int bm_is_resident,
                             const struct ntfs_data_run *bm_runs,
                             int bm_run_count)
{
    uint64_t byte_idx = vcn / 8;
    uint8_t  bit_mask = (uint8_t)(1u << (vcn % 8));
    uint8_t  byte_val = 0;

    if (bm_is_resident) {
        if (byte_idx >= bm_res_len) return 0;
        return (bm_resident[byte_idx] & bit_mask) ? 1 : 0;
    }

    if (bm_run_count > 0 && bm_runs) {
        ntfs_read_data(vol, bm_runs, bm_run_count, 0,
                       byte_idx, 1, &byte_val);
    }
    return (byte_val & bit_mask) ? 1 : 0;
}
