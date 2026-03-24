/* ============================================================================
 * ntfs_index_insert.c — B+ Tree Directory Index Insert (§14.1)
 *
 * Implements ntfs_index_insert():
 *   1. Navigate B+ tree to correct leaf position
 *   2. Insert entry at leaf (may be in root or an INDX buffer)
 *   3. If the node overflows:
 *      a. If root: move all entries to a new INDX buffer, reset root as
 *         separator with one child VCN.
 *      b. If INDX buffer: split into two halves, promote median to parent.
 *         Recurse up the path until a node fits or root splits.
 *   4. All INDX writes use USA regeneration + journal transactions.
 *
 * Helper declarations (defined in ntfs_index_helpers.c):
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* External helpers from ntfs_index_helpers.c */
extern int  ntfs_index_compare(const struct ntfs_volume *vol,
                                const uint8_t *name_a, int len_a,
                                const uint8_t *name_b, int len_b);
extern void ntfs_index_find_pos(const struct ntfs_volume *vol,
                                 const uint8_t *entries_base,
                                 uint32_t total_entries_size,
                                 const uint8_t *search_utf16, int search_len,
                                 uint32_t *out_offset, int *out_match,
                                 uint64_t *out_child_vcn);
extern void ntfs_build_indx_buf(uint8_t *buf, uint32_t record_size,
                                 uint16_t bytes_per_sector, uint64_t vcn);

/* Maximum B+ tree depth for path tracking */
#define NTFS_MAX_DEPTH  16
/* Maximum index runs */
#define NTFS_INDEX_MAX_RUNS  64
/* Node header offset within INDX buffer */
#define INDX_NODE_HDR_OFF  0x18
/* Sentinel marker */
#define VCN_NONE  ((uint64_t)-1)

/* ============================================================================
 * Internal: build_leaf_entry
 * Build a raw on-disk index entry (no child VCN — leaf node entry).
 * Returns entry length (8-byte aligned).
 * ============================================================================ */
static uint32_t build_leaf_entry(uint8_t *buf, uint64_t mft_ref,
                                  const uint8_t *fn_data, uint32_t fn_len)
{
    uint32_t entry_len = 0x10 + fn_len;
    entry_len = (entry_len + 7) & ~7u;

    ntfs_memset(buf, 0, entry_len);
    ntfs_le64_write(buf + 0x00, mft_ref);        /* MFT reference */
    ntfs_le16_write(buf + 0x08, (uint16_t)entry_len);
    ntfs_le16_write(buf + 0x0A, (uint16_t)fn_len);
    buf[0x0C] = 0;                               /* LEAF flag */
    ntfs_memcpy(buf + 0x10, fn_data, fn_len);
    return entry_len;
}

/* ============================================================================
 * Internal: allocate_indx_vcn
 * Allocate one cluster for a new INDX buffer, create/extend
 * $INDEX_ALLOCATION and update $BITMAP.
 *
 * On success: fills *out_vcn with the VCN, updates ia_runs/ia_run_count
 * in place (via the directory's MFT record), returns NTFS_OK.
 * ============================================================================ */
static int allocate_indx_vcn(struct ntfs_volume *vol,
                              uint64_t dir_inode,
                              uint64_t *out_vcn)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header bm_ah;
    const uint8_t *bm_attr;
    uint64_t lcn;
    uint64_t new_vcn;
    int rc;

    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec) return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
    if (rc != NTFS_OK) { kfree(rec); return rc; }

    /* Determine next free VCN from existing $INDEX_ALLOCATION size */
    uint64_t alloc_hint = 0;  /* locality hint for cluster allocator */
    {
        struct ntfs_attr_header ia_ah;
        const uint8_t *ia_attr = ntfs_attr_find_named(rec, &hdr,
                                      NTFS_ATTR_INDEX_ALLOCATION, "$I30", &ia_ah);
        struct ntfs_nonres_header nrhdr;
        if (ia_attr && ia_ah.non_resident) {
            struct ntfs_data_run runs[NTFS_INDEX_MAX_RUNS];
            int rc2 = ntfs_decode_data_runs(ia_attr, runs, NTFS_INDEX_MAX_RUNS,
                                             &nrhdr);
            if (rc2 > 0) {
                /* Next VCN = last_vcn+1 (each INDX = vol->index_size / cluster_size clusters) */
                new_vcn = (nrhdr.real_size / vol->cluster_size);
                /* Hint: allocate right after the last run for contiguity */
                alloc_hint = runs[rc2 - 1].lcn + runs[rc2 - 1].length;
            } else {
                new_vcn = 0;
            }
        } else {
            new_vcn = 0;
        }
    }

    /* Allocate one (or more) contiguous clusters for INDX record */
    uint64_t clusters_per_indx = vol->index_size / vol->cluster_size;
    if (clusters_per_indx == 0) clusters_per_indx = 1;

    lcn = ntfs_alloc_clusters(vol, clusters_per_indx, alloc_hint);
    if (lcn == 0) { kfree(rec); return NTFS_ERR_FULL; }

    klog(LOG_DEBUG, "ntfs", "alloc_indx_vcn: VCN %llu -> LCN %llu", new_vcn, lcn);

    /* Extend or create $INDEX_ALLOCATION attribute to include new run.
     * For simplicity: remove old IA attr, rebuild with extended run list.
     * (In production: ntfs_attr_update handles run extension.) */
    {
        /* Encode new single run (or extend existing) */
        struct ntfs_data_run old_runs[NTFS_INDEX_MAX_RUNS];
        int old_count = 0;
        struct ntfs_attr_header ia_ah;
        const uint8_t *ia_attr = ntfs_attr_find_named(rec, &hdr,
                                      NTFS_ATTR_INDEX_ALLOCATION, "$I30", &ia_ah);
        struct ntfs_data_run new_runs[NTFS_INDEX_MAX_RUNS + 1];
        int new_count;

        if (ia_attr && ia_ah.non_resident) {
            old_count = ntfs_decode_data_runs(ia_attr, old_runs,
                                               NTFS_INDEX_MAX_RUNS, NULL);
            if (old_count < 0) old_count = 0;
        }

        /* Append new run — or coalesce with last run if contiguous.
         * Merging contiguous runs keeps the run list compact and prevents
         * the non-resident header from growing beyond MFT capacity. */
        ntfs_memcpy(new_runs, old_runs,
                    (uint32_t)(old_count * (int)sizeof(struct ntfs_data_run)));

        if (old_count > 0 &&
            new_runs[old_count - 1].lcn + new_runs[old_count - 1].length == lcn) {
            /* Contiguous with last run — just extend it */
            new_runs[old_count - 1].length += clusters_per_indx;
            new_count = old_count;
        } else {
            /* Non-contiguous — append as a new run */
            new_runs[old_count].vcn_start = new_vcn;
            new_runs[old_count].lcn       = lcn;
            new_runs[old_count].length    = clusters_per_indx;
            new_count = old_count + 1;
        }

        /* Remove old IA, rebuild as proper non-resident attribute.
         * $INDEX_ALLOCATION MUST be non-resident — it contains INDX
         * buffers on disk, referenced by the run list.  ntfs_attr_add()
         * would create a resident attribute for small data, which is
         * invalid for type 0xA0.
         *
         * Pass NULL for vol to SKIP freeing the old attribute's clusters —
         * those clusters are still in use and will be re-injected into the
         * rebuilt attribute via old_runs[]. */
        ntfs_attr_remove(NULL, rec, &hdr, vol->frs_size,
                          NTFS_ATTR_INDEX_ALLOCATION, "$I30");

        /* Re-read MFT header — ntfs_attr_remove updated used_size in rec */
        hdr.used_size  = ntfs_le32(rec + 0x18);

        /* Calculate total allocation size across all runs */
        {
            uint64_t total_vcn = 0;
            int ri;
            for (ri = 0; ri < new_count; ri++)
                total_vcn += new_runs[ri].length;

            uint64_t alloc_sz = total_vcn * (uint64_t)vol->cluster_size;
            uint64_t real_sz  = alloc_sz;

            /* Build non-resident attribute header with embedded run list */
            uint8_t attr_buf[1024];
            uint32_t attr_len = build_nonresident_attr(
                attr_buf, NTFS_ATTR_INDEX_ALLOCATION,
                "$I30", 4,  /* name = "$I30", name_len = 4 */
                next_attr_id(rec, &hdr),
                new_runs, new_count,
                alloc_sz, real_sz);

            if (attr_len == 0) {
                ntfs_free_clusters(vol, lcn, clusters_per_indx);
                kfree(rec);
                return NTFS_ERR_IO;
            }

            /* Find insertion point (sorted by type) */
            uint32_t insert_off = find_insert_point(rec, &hdr,
                                                     NTFS_ATTR_INDEX_ALLOCATION);

            /* Check that it fits in the MFT record */
            uint32_t free_space = hdr.alloc_size - hdr.used_size;
            if (attr_len > free_space) {
                ntfs_free_clusters(vol, lcn, clusters_per_indx);
                kfree(rec);
                klog(LOG_ERROR, "ntfs",
                     "alloc_indx_vcn: IA attr %u bytes > free %u",
                     (uint64_t)attr_len, (uint64_t)free_space);
                return NTFS_ERR_FULL;
            }

            /* Open gap and insert */
            uint32_t new_used = shift_attrs(rec, vol->frs_size,
                                             insert_off, hdr.used_size,
                                             (int32_t)attr_len);
            if (new_used == 0) {
                ntfs_free_clusters(vol, lcn, clusters_per_indx);
                kfree(rec);
                return NTFS_ERR_IO;
            }

            ntfs_memcpy(rec + insert_off, attr_buf, attr_len);
            reparse_header(rec, &hdr);
        }

        rc = NTFS_OK;
    }

    /* Update $BITMAP to mark new_vcn as active */
    {
        uint64_t byte_idx = new_vcn / 8;
        uint8_t  bit_mask = (uint8_t)(1u << (new_vcn % 8));
        uint8_t  bm_buf[64];
        uint32_t bm_buf_len;

        bm_attr = ntfs_attr_find_named(rec, &hdr, NTFS_ATTR_BITMAP,
                                       "$I30", &bm_ah);
        if (bm_attr && !bm_ah.non_resident) {
            /* Resident bitmap — copy, set bit, re-add */
            bm_buf_len = bm_ah.content_length;
            if (bm_buf_len > 64) bm_buf_len = 64;
            ntfs_memcpy(bm_buf, bm_attr + bm_ah.content_offset, bm_buf_len);
            /* Extend if needed */
            while (byte_idx >= bm_buf_len && bm_buf_len < 64) {
                bm_buf[bm_buf_len++] = 0;
            }
            if (byte_idx < bm_buf_len)
                bm_buf[byte_idx] |= bit_mask;

            ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                              NTFS_ATTR_BITMAP, "$I30");
            ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                           NTFS_ATTR_BITMAP, "$I30",
                           bm_buf, bm_buf_len);
        } else if (!bm_attr) {
            /* No bitmap yet — create one */
            ntfs_memset(bm_buf, 0, sizeof(bm_buf));
            bm_buf_len = (uint32_t)(byte_idx + 1);
            if (bm_buf_len > 64) bm_buf_len = 64;
            if (byte_idx < bm_buf_len)
                bm_buf[byte_idx] |= bit_mask;
            ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                           NTFS_ATTR_BITMAP, "$I30",
                           bm_buf, bm_buf_len);
        }
        /* Non-resident bitmap: handled via ntfs_attr_update (later) */
    }

    /* Write updated directory record */
    rc = ntfs_write_mft_record(vol, dir_inode, rec);
    kfree(rec);

    if (rc != NTFS_OK) return rc;

    *out_vcn = new_vcn;
    return NTFS_OK;
}

/* ============================================================================
 * ntfs_index_insert — Full B+ tree insert with split support
 * ============================================================================ */
int ntfs_index_insert(struct ntfs_volume *vol,
                      uint64_t dir_inode,
                      uint64_t child_inode, uint16_t child_seq,
                      const uint8_t *fn_data, uint32_t fn_data_len)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ia_ah;
    const uint8_t *ia_attr;
    struct ntfs_data_run ia_runs[NTFS_INDEX_MAX_RUNS];
    int ia_run_count = 0;
    uint32_t indx_size;
    uint64_t child_ref;

    /* New entry buffer (max ~600 bytes) */
    uint8_t new_entry[640];
    uint32_t new_entry_len;

    /* Path tracking for splits */
    uint64_t path_vcn[NTFS_MAX_DEPTH];   /* VCN_NONE = root */
    uint32_t path_pos[NTFS_MAX_DEPTH];   /* byte offset of insertion in that node */
    int depth = 0;

    int rc;
    struct ntfs_txn *txn;

    if (!vol || !fn_data) return NTFS_ERR_IO;

    child_ref = (child_inode & 0x0000FFFFFFFFFFFFULL) |
                ((uint64_t)child_seq << 48);

    /* Build new leaf index entry */
    new_entry_len = build_leaf_entry(new_entry, child_ref, fn_data, fn_data_len);

    /* Get the search name from fn_data (UTF-16LE at offset 0x42, len at 0x40) */
    int search_name_len = (int)(uint8_t)fn_data[0x40];
    const uint8_t *search_name_utf16 = fn_data + 0x42;

    /* Start journal transaction */
    txn = ntfs_txn_begin(vol);
    /* txn may be NULL if journal not loaded — continue without journaling */

    /* ---- Step 1: Read directory MFT record, find $INDEX_ROOT ---- */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec) { if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn); return NTFS_ERR_IO; }

    rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
    if (rc != NTFS_OK) { kfree(rec); if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn); return rc; }

    /* Get $INDEX_ALLOCATION runs (for child node access) */
    ia_attr = ntfs_attr_find_named(rec, &hdr, NTFS_ATTR_INDEX_ALLOCATION,
                                   "$I30", &ia_ah);
    if (ia_attr && ia_ah.non_resident)
        ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs,
                                              NTFS_INDEX_MAX_RUNS, NULL);
    if (ia_run_count < 0) ia_run_count = 0;

    /* Get INDX record size */
    {
        struct ntfs_index_root_header rh;
        struct ntfs_index_node_header nh;
        const uint8_t *eb;
        if (ntfs_parse_index_root(rec, &hdr, &rh, &nh, &eb) == NTFS_OK)
            indx_size = rh.index_record_size;
        else
            indx_size = 4096;
        if (indx_size == 0) indx_size = 4096;
    }

    kfree(rec);

    /* ---- Step 2: Navigate B+ tree to leaf ---- */
    /* We'll navigate down from root, recording path for potential splits. */
    {
        uint64_t child_vcn = VCN_NONE;
        int at_root = 1;
        int d;

        /* Read root again to navigate */
        rec = (uint8_t *)kmalloc(vol->frs_size);
        if (!rec) { if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn); return NTFS_ERR_IO; }

        rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
        if (rc != NTFS_OK) {
            kfree(rec);
            if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
            return rc;
        }

        /* Search root node */
        {
            struct ntfs_attr_header root_ah;
            const uint8_t *root_attr = ntfs_attr_find_named(rec, &hdr,
                                           NTFS_ATTR_INDEX_ROOT, "$I30", &root_ah);
            if (!root_attr || root_ah.non_resident) {
                kfree(rec);
                if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
                return NTFS_ERR_IO;
            }
            const uint8_t *root_content = root_attr + root_ah.content_offset;
            const uint8_t *node         = root_content + 0x10;
            uint32_t entries_off        = ntfs_le32(node + 0x00);
            uint32_t total_entries_size = ntfs_le32(node + 0x04);
            const uint8_t *entries_base = node + entries_off;
            uint32_t insert_off;
            int match;

            /* Convert IndexLength to entries-only size for search */
            uint32_t entries_size = (total_entries_size >= entries_off)
                                     ? total_entries_size - entries_off : 0;
            ntfs_index_find_pos(vol, entries_base, entries_size,
                                 search_name_utf16, search_name_len,
                                 &insert_off, &match, &child_vcn);

            path_vcn[depth] = VCN_NONE; /* root */
            path_pos[depth] = insert_off;
            depth++;
        }

        kfree(rec);
        at_root = 0; (void)at_root;

        /* Descend into INDX buffers until leaf */
        if (child_vcn != VCN_NONE && ia_run_count > 0) {
            uintptr_t indx_phys;
            uint8_t *indx_buf;
            uint32_t indx_pages = (indx_size + 4095) / 4096;

            indx_phys = pmm_alloc_contiguous(indx_pages);
            if (!indx_phys) {
                if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
                return NTFS_ERR_IO;
            }
            indx_buf = (uint8_t *)(uintptr_t)indx_phys;

            for (d = 0; d < NTFS_MAX_DEPTH && child_vcn != VCN_NONE; d++) {
                struct ntfs_index_node_header nh;
                const uint8_t *eb;
                uint32_t ins_off;
                int match;
                uint64_t next_vcn;

                rc = ntfs_read_indx(vol, ia_runs, ia_run_count, child_vcn,
                                     indx_size, indx_buf);
                if (rc != NTFS_OK) break;

                rc = ntfs_parse_indx_entries(indx_buf, &nh, &eb);
                if (rc != NTFS_OK) break;

                /* Convert IndexLength to entries-only */
                uint32_t nh_entries_size = (nh.total_size >= nh.entries_offset)
                                            ? nh.total_size - nh.entries_offset : 0;
                ntfs_index_find_pos(vol, eb, nh_entries_size,
                                     search_name_utf16, search_name_len,
                                     &ins_off, &match, &next_vcn);
                path_vcn[depth] = child_vcn;
                path_pos[depth] = ins_off;
                depth++;

                child_vcn = next_vcn;
            }

            pmm_free_frame(indx_phys);
        }
    }

    /* ---- Step 3: Insert at the leaf level and propagate splits ---- */
    {
        uint8_t promote_entry[640];
        uint32_t promote_len = 0;
        int need_promote = 0;
        uint64_t promote_right_vcn = VCN_NONE; /* right sibling VCN to patch into parent's LAST */

        /* Allocate INDX buffer for operations.
         * Always allocate even if ia_run_count is 0: the root overflow path
         * creates $INDEX_ALLOCATION on the fly and needs the buffer to write
         * saved entries into the new INDX node. */
        uintptr_t indx_phys = 0;
        uint8_t *indx_buf   = NULL;
        {
            uint32_t indx_pages = (indx_size + 4095) / 4096;
            indx_phys = pmm_alloc_contiguous(indx_pages);
            if (indx_phys)
                indx_buf = (uint8_t *)(uintptr_t)indx_phys;
        }

        /* Start from deepest level, work upward */
        int level = depth - 1;
        const uint8_t *cur_entry = new_entry;
        uint32_t cur_len = new_entry_len;

        while (level >= 0) {
            uint64_t cur_vcn = path_vcn[level];
            uint32_t ins_off = path_pos[level];

            if (cur_vcn == VCN_NONE) {
                /* ---- Insert into $INDEX_ROOT ---- */
                rec = (uint8_t *)kmalloc(vol->frs_size);
                if (!rec) { rc = NTFS_ERR_IO; break; }

                rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
                if (rc != NTFS_OK) { kfree(rec); break; }

                {
                    struct ntfs_attr_header root_ah;
                    const uint8_t *root_attr = ntfs_attr_find_named(rec, &hdr,
                                                   NTFS_ATTR_INDEX_ROOT, "$I30",
                                                   &root_ah);
                    const uint8_t *root_content = root_attr + root_ah.content_offset;
                    uint8_t *node = (uint8_t *)root_content + 0x10;
                    uint32_t entries_off = ntfs_le32(node + 0x00);
                    uint32_t total_sz_raw = ntfs_le32(node + 0x04);
                    uint32_t alloc_sz_raw = ntfs_le32(node + 0x08);
                    /* Convert to entries-only sizes */
                    uint32_t total_sz = (total_sz_raw >= entries_off)
                                         ? total_sz_raw - entries_off : 0;
                    uint8_t *eb = node + entries_off;
                    (void)alloc_sz_raw;

                    /* Check if MFT record can accommodate the grown root.
                     * $INDEX_ROOT is resident and resizable — the node header's
                     * alloc_sz is NOT the constraint; MFT free space is. */
                    int fits = ((uint32_t)(root_ah.total_length + cur_len) <=
                                hdr.alloc_size - hdr.used_size + root_ah.total_length);

                    if (fits) {
                        /* Insert directly into root node */
                        /* Build new root content (old + cur_len bytes) */
                        uint32_t old_cs = root_ah.content_length;
                        uint32_t new_cs = old_cs + cur_len;
                        uint8_t *new_root = (uint8_t *)kmalloc(new_cs);
                        if (!new_root) { rc = NTFS_ERR_IO; kfree(rec); break; }


                        ntfs_memcpy(new_root, root_content, old_cs);
                        /* Shift entries from ins_off */
                        uint8_t *new_node = new_root + 0x10;
                        uint8_t *new_eb   = new_node + entries_off;
                        ntfs_memmove(new_eb + ins_off + cur_len,
                                     new_eb + ins_off,
                                     total_sz - ins_off);
                        ntfs_memcpy(new_eb + ins_off, cur_entry, cur_len);
                        /* Update node header total_size and alloc_size */
                        ntfs_le32_write(new_node + 0x04,
                                        entries_off + total_sz + cur_len);
                        ntfs_le32_write(new_node + 0x08,
                                        entries_off + total_sz + cur_len);

                        /* If this insert was a promoted separator from a child
                         * split, update the LAST sentinel's child_vcn to point
                         * to the right sibling (the new INDX node). The LAST
                         * sentinel previously pointed to cur_vcn (the pre-split
                         * node, now the left sibling). */
                        if (promote_right_vcn != VCN_NONE) {
                            uint32_t new_total_ent = total_sz + cur_len;
                            uint32_t spos = 0;
                            uint8_t *sentw = new_eb;
                            while (spos < new_total_ent) {
                                uint16_t sl = ntfs_le16(sentw + spos + 0x08);
                                if (sl < 0x10) break;
                                if (sentw[spos + 0x0C] & NTFS_INDEX_ENTRY_LAST) {
                                    if (sentw[spos + 0x0C] & NTFS_INDEX_ENTRY_SUBNODE)
                                        ntfs_le64_write(sentw + spos + sl - 8, promote_right_vcn);
                                    break;
                                }
                                spos += sl;
                            }
                            promote_right_vcn = VCN_NONE;
                        }


                        /* Update root attr */
                        ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                                          NTFS_ATTR_INDEX_ROOT, "$I30");
                        rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                                            NTFS_ATTR_INDEX_ROOT, "$I30",
                                            new_root, new_cs);
                        kfree(new_root);
                        if (rc == NTFS_OK) {
                            /* Log: ADd_IDX_ROOT */
                            if (txn)
                                ntfs_txn_log(txn,
                                             NTFS_LOG_OP_ADD_IDX_ROOT, cur_entry, (uint16_t)cur_len,
                                             NTFS_LOG_OP_DEL_IDX_ROOT, cur_entry, (uint16_t)cur_len,
                                             dir_inode, 0);
                            rc = ntfs_write_mft_record(vol, dir_inode, rec);
                        }
                        kfree(rec);
                        need_promote = 0;
                        break; /* Done */
                    }

                    /* Root overflows — need to push root contents to a new INDX,
                     * then make root a single-separator internal node.
                     *
                     * KEY: We must shrink $INDEX_ROOT BEFORE calling
                     * allocate_indx_vcn(), because $INDEX_ROOT is bloated
                     * with ~700 bytes of entries and there's no room in the
                     * MFT record for the new $INDEX_ALLOCATION attribute.
                     *
                     * Strategy:
                     *   1. Save old root entries + new entry into temp buffer
                     *   2. Shrink $INDEX_ROOT to an empty stub (frees ~700 bytes)
                     *   3. Write MFT to persist the shrunk root
                     *   4. Call allocate_indx_vcn() — now has room for IA+bitmap
                     *   5. Build INDX buffer from saved entries, write it
                     *   6. Update $INDEX_ROOT with child VCN pointer
                     */
                    {
                        uint64_t new_vcn = 0;

                        /* --- Save root entries + new entry into a single buffer --- */
                        uint32_t saved_total = total_sz + cur_len;
                        uint8_t *saved_entries = (uint8_t *)kmalloc(saved_total);
                        if (!saved_entries) { rc = NTFS_ERR_IO; kfree(rec); break; }

                        ntfs_memcpy(saved_entries, eb, ins_off);
                        ntfs_memcpy(saved_entries + ins_off, cur_entry, cur_len);
                        ntfs_memcpy(saved_entries + ins_off + cur_len,
                                     eb + ins_off,
                                     total_sz - ins_off);

                        /* Remember the root header (first 0x10 bytes of root content) */
                        uint8_t saved_root_hdr[0x10];
                        ntfs_memcpy(saved_root_hdr, root_content, 0x10);

                        /* --- Shrink $INDEX_ROOT to empty stub --- */
                        {
                            uint32_t sentinel_len = 0x10 + 8; /* LAST + SUBNODE = 24 bytes */
                            uint32_t stub_size = 0x10 + entries_off + sentinel_len;
                            uint8_t *stub = (uint8_t *)kmalloc(stub_size);
                            if (!stub) { kfree(saved_entries); rc = NTFS_ERR_IO; kfree(rec); break; }

                            ntfs_memcpy(stub, saved_root_hdr, 0x10);
                            uint8_t *stub_nh = stub + 0x10;
                            ntfs_le32_write(stub_nh + 0x00, entries_off);
                            ntfs_le32_write(stub_nh + 0x04, entries_off + sentinel_len);
                            ntfs_le32_write(stub_nh + 0x08, entries_off + sentinel_len);
                            stub_nh[0x0C] = 0x01; /* has_children */
                            uint8_t *sent = stub_nh + entries_off;
                            ntfs_memset(sent, 0, sentinel_len);
                            ntfs_le16_write(sent + 0x08, (uint16_t)sentinel_len);
                            sent[0x0C] = NTFS_INDEX_ENTRY_LAST | NTFS_INDEX_ENTRY_SUBNODE;
                            /* VCN will be patched after allocate_indx_vcn */
                            ntfs_le64_write(sent + sentinel_len - 8, 0);

                            ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                                              NTFS_ATTR_INDEX_ROOT, "$I30");
                            ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                                           NTFS_ATTR_INDEX_ROOT, "$I30",
                                           stub, stub_size);
                            kfree(stub);
                        }

                        /* Write the shrunk MFT record so allocate_indx_vcn
                         * reads a record with plenty of free space. */
                        ntfs_write_mft_record(vol, dir_inode, rec);
                        kfree(rec);

                        /* --- Now allocate the INDX VCN (plenty of MFT space) --- */
                        rc = allocate_indx_vcn(vol, dir_inode, &new_vcn);
                        if (rc != NTFS_OK) { kfree(saved_entries); break; }

                        /* Re-read updated MFT (allocation modified it) */
                        rec = (uint8_t *)kmalloc(vol->frs_size);
                        if (!rec) { kfree(saved_entries); rc = NTFS_ERR_IO; break; }
                        rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
                        if (rc != NTFS_OK) { kfree(saved_entries); kfree(rec); break; }

                        /* Reload ia_runs after alloc */
                        ia_attr = ntfs_attr_find_named(rec, &hdr,
                                    NTFS_ATTR_INDEX_ALLOCATION, "$I30", &ia_ah);
                        if (ia_attr && ia_ah.non_resident)
                            ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs,
                                                                  NTFS_INDEX_MAX_RUNS, NULL);

                        /* --- Build INDX buffer from saved entries --- */
                        if (indx_buf) {
                            ntfs_build_indx_buf(indx_buf, indx_size,
                                                 vol->bytes_per_sector, new_vcn);

                            uint8_t *inode_hdr = indx_buf + INDX_NODE_HDR_OFF;
                            uint32_t i_entries_off = ntfs_le32(inode_hdr + 0x00);
                            uint8_t *ieb = inode_hdr + i_entries_off;
                            uint32_t i_alloc_raw = ntfs_le32(inode_hdr + 0x08);
                            uint32_t i_alloc = (i_alloc_raw >= i_entries_off)
                                                ? i_alloc_raw - i_entries_off : 0;

                            if (saved_total <= i_alloc) {
                                ntfs_memcpy(ieb, saved_entries, saved_total);
                                ntfs_le32_write(inode_hdr + 0x04,
                                                i_entries_off + saved_total);
                                ntfs_le32_write(inode_hdr + 0x08,
                                                i_entries_off + saved_total);
                            }

                            if (txn)
                                ntfs_txn_log(txn,
                                             NTFS_LOG_OP_ADD_IDX_ALLOC, ieb, (uint16_t)saved_total,
                                             NTFS_LOG_OP_DEL_IDX_ALLOC, NULL, 0,
                                             dir_inode, 0);
                            ntfs_write_indx(vol, ia_runs, ia_run_count,
                                             new_vcn, indx_size, indx_buf);
                        }
                        kfree(saved_entries);

                        /* --- Patch $INDEX_ROOT sentinel VCN to point to new_vcn --- */
                        {
                            struct ntfs_attr_header root_ah2;
                            const uint8_t *ra2 = ntfs_attr_find_named(rec, &hdr,
                                                     NTFS_ATTR_INDEX_ROOT, "$I30", &root_ah2);
                            if (ra2) {
                                uint8_t *rc2 = (uint8_t *)ra2 + root_ah2.content_offset;
                                uint8_t *nh2 = rc2 + 0x10;
                                uint32_t eo2 = ntfs_le32(nh2 + 0x00);
                                uint32_t il2 = ntfs_le32(nh2 + 0x04);
                                /* Sentinel is the last entry in the node */
                                uint8_t *sent2 = nh2 + eo2;
                                /* Walk to LAST entry */
                                while (sent2 < nh2 + il2) {
                                    uint16_t elen = ntfs_le16(sent2 + 0x08);
                                    if (elen == 0) break;
                                    if (sent2[0x0C] & NTFS_INDEX_ENTRY_LAST) break;
                                    sent2 += elen;
                                }
                                /* Write VCN at end of sentinel */
                                uint16_t s_len = ntfs_le16(sent2 + 0x08);
                                if (s_len >= 24)
                                    ntfs_le64_write(sent2 + s_len - 8, new_vcn);
                            }
                        }

                        rc = ntfs_write_mft_record(vol, dir_inode, rec);
                        need_promote = 0;
                        kfree(rec);
                        break;
                    }
                }
            } else if (indx_buf) {
                /* ---- Insert into INDX buffer ---- */
                rc = ntfs_read_indx(vol, ia_runs, ia_run_count, cur_vcn,
                                     indx_size, indx_buf);
                if (rc != NTFS_OK) break;

                uint8_t *inode_hdr    = indx_buf + INDX_NODE_HDR_OFF;
                uint32_t i_entries_off = ntfs_le32(inode_hdr + 0x00);
                uint32_t i_total_raw  = ntfs_le32(inode_hdr + 0x04);
                uint32_t i_alloc_raw  = ntfs_le32(inode_hdr + 0x08);
                /* Convert from IndexLength (from node header start) to
                 * entries-only size (from entries_base start).  All entry
                 * offsets returned by ntfs_index_find_pos are relative to
                 * entries_base, so all arithmetic must use entries-only. */
                uint32_t i_total = (i_total_raw >= i_entries_off)
                                    ? i_total_raw - i_entries_off : 0;
                uint32_t i_alloc = (i_alloc_raw >= i_entries_off)
                                    ? i_alloc_raw - i_entries_off : 0;
                uint8_t *ieb          = inode_hdr + i_entries_off;

                /* Re-compute insert position from the CURRENT buffer state.
                 * The path_pos[] from the search phase may be stale if a
                 * child-level B-tree split modified this node in a prior
                 * loop iteration.  Use the CURRENT entry's embedded name
                 * (it may be a promoted separator, not the original file). */
                {
                    int re_match = 0;
                    uint64_t re_child = VCN_NONE;
                    const uint8_t *re_fn = cur_entry + 0x10;
                    int re_nlen = (int)(uint8_t)re_fn[0x40];
                    const uint8_t *re_name = re_fn + 0x42;
                    ntfs_index_find_pos(vol, ieb, i_total,
                                         re_name, re_nlen,
                                         &ins_off, &re_match, &re_child);
                }

                if (i_total + cur_len <= i_alloc) {
                    /* Fits — simple insert */
                    ntfs_memmove(ieb + ins_off + cur_len,
                                 ieb + ins_off,
                                 i_total - ins_off);
                    ntfs_memcpy(ieb + ins_off, cur_entry, cur_len);
                    i_total += cur_len;
                    ntfs_le32_write(inode_hdr + 0x04, i_entries_off + i_total);
                    ntfs_le32_write(inode_hdr + 0x08, i_entries_off + i_total);

                    /* If this entry was promoted from a child split, the
                     * LAST sentinel's child_vcn needs updating to the right
                     * sibling VCN (it currently points to the pre-split node). */
                    if (promote_right_vcn != VCN_NONE) {
                        uint32_t spos = 0;
                        while (spos < i_total) {
                            uint16_t sl = ntfs_le16(ieb + spos + 0x08);
                            if (sl < 0x10) break;
                            if (ieb[spos + 0x0C] & NTFS_INDEX_ENTRY_LAST) {
                                if (ieb[spos + 0x0C] & NTFS_INDEX_ENTRY_SUBNODE)
                                    ntfs_le64_write(ieb + spos + sl - 8, promote_right_vcn);
                                break;
                            }
                            spos += sl;
                        }
                        promote_right_vcn = VCN_NONE;
                    }

                    if (txn)
                        ntfs_txn_log(txn,
                                     NTFS_LOG_OP_ADD_IDX_ALLOC, cur_entry, (uint16_t)cur_len,
                                     NTFS_LOG_OP_DEL_IDX_ALLOC, cur_entry, (uint16_t)cur_len,
                                     dir_inode, 0);
                    rc = ntfs_write_indx(vol, ia_runs, ia_run_count, cur_vcn,
                                          indx_size, indx_buf);
                    need_promote = 0;
                    break;
                }

                /* Overflow: split this node at midpoint */
                uint32_t total_with_new = i_total + cur_len;
                uint32_t half = total_with_new / 2;

                /* Build combined entries array */
                uint8_t *combined = (uint8_t *)kmalloc(total_with_new);
                if (!combined) { rc = NTFS_ERR_IO; break; }
                ntfs_memcpy(combined, ieb, ins_off);
                ntfs_memcpy(combined + ins_off, cur_entry, cur_len);
                ntfs_memcpy(combined + ins_off + cur_len,
                             ieb + ins_off, i_total - ins_off);

                /* Find median */
                uint32_t median_off = 0;
                uint32_t sz = 0;
                while (sz < half && median_off < total_with_new) {
                    uint16_t e_len = ntfs_le16(combined + median_off + 0x08);
                    if (e_len < 0x10) break;
                    sz += e_len;
                    median_off += e_len;
                }

                /* The entry at median_off is the promoted separator */
                uint16_t med_len = ntfs_le16(combined + median_off + 0x08);
                if (med_len < 0x10 || (uint32_t)(median_off + med_len) > total_with_new) {
                    kfree(combined); rc = NTFS_ERR_IO; break;
                }
                /* Save median as promote entry (promote to parent) */
                ntfs_memcpy(promote_entry, combined + median_off, med_len);
                promote_len = med_len;

                /* Allocate right sibling INDX */
                uint64_t right_vcn = 0;
                rc = allocate_indx_vcn(vol, dir_inode, &right_vcn);
                if (rc != NTFS_OK) { kfree(combined); break; }

                /* Reload ia_runs */
                {
                    uint8_t *rec2 = (uint8_t *)kmalloc(vol->frs_size);
                    struct ntfs_mft_header hdr2;
                    if (rec2 && ntfs_read_mft_record(vol, dir_inode, rec2, &hdr2) == NTFS_OK) {
                        ia_attr = ntfs_attr_find_named(rec2, &hdr2,
                                      NTFS_ATTR_INDEX_ALLOCATION, "$I30", &ia_ah);
                        if (ia_attr && ia_ah.non_resident)
                            ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs,
                                                                  NTFS_INDEX_MAX_RUNS, NULL);
                    }
                    if (rec2) kfree(rec2);
                }

                /* --- Compute actual data sizes, excluding the original LAST sentinel
                 * from the combined array.  The LAST sentinel will be re-created
                 * for both left and right nodes independently. --- */
                uint32_t data_end = total_with_new; /* default: everything is data */
                {
                    /* Walk combined entries to find the LAST sentinel */
                    uint32_t walk = 0;
                    while (walk < total_with_new) {
                        uint16_t wl = ntfs_le16(combined + walk + 0x08);
                        if (wl < 0x10) break;
                        if (combined[walk + 0x0C] & NTFS_INDEX_ENTRY_LAST) {
                            data_end = walk; /* exclude LAST sentinel and beyond */
                            break;
                        }
                        walk += wl;
                    }
                }

                /* Left data: entries [0, median_off) — only real entries */
                uint32_t left_data_sz = (median_off <= data_end) ? median_off : data_end;
                /* Right data: entries [median_off + med_len, data_end) */
                uint32_t right_data_start = median_off + med_len;
                uint32_t right_data_sz = (right_data_start < data_end)
                                          ? data_end - right_data_start : 0;

                /* Build LAST sentinel for leaf nodes (no children) */
                uint8_t last_sentinel[24];
                ntfs_memset(last_sentinel, 0, sizeof(last_sentinel));
                ntfs_le16_write(last_sentinel + 0x08, 0x10); /* length = 16 */
                last_sentinel[0x0C] = NTFS_INDEX_ENTRY_LAST;

                /* Left node: entries [0, median_off) + LAST sentinel */
                ntfs_memset(ieb, 0, i_alloc);
                ntfs_memcpy(ieb, combined, left_data_sz);
                ntfs_memcpy(ieb + left_data_sz, last_sentinel, 0x10);
                uint32_t left_total = left_data_sz + 0x10; /* data + LAST */
                ntfs_le32_write(inode_hdr + 0x04, i_entries_off + left_total);
                ntfs_write_indx(vol, ia_runs, ia_run_count, cur_vcn,
                                 indx_size, indx_buf);

                /* Right node: entries [median_off + med_len, data_end) + LAST sentinel */
                uintptr_t right_phys = pmm_alloc_contiguous((indx_size + 4095) / 4096);
                if (right_phys) {
                    uint8_t *rbuf = (uint8_t *)(uintptr_t)right_phys;
                    ntfs_build_indx_buf(rbuf, indx_size, vol->bytes_per_sector, right_vcn);

                    uint8_t *r_nh  = rbuf + INDX_NODE_HDR_OFF;
                    uint32_t r_eo  = ntfs_le32(r_nh + 0x00);
                    uint8_t *r_eb  = r_nh + r_eo;
                    uint32_t r_al  = ntfs_le32(r_nh + 0x08);
                    uint32_t r_al_entries = (r_al >= r_eo) ? r_al - r_eo : 0;
                    uint32_t right_total = right_data_sz + 0x10; /* data + LAST */
                    if (right_total <= r_al_entries) {
                        if (right_data_sz > 0)
                            ntfs_memcpy(r_eb, combined + right_data_start, right_data_sz);
                        ntfs_memcpy(r_eb + right_data_sz, last_sentinel, 0x10);
                        ntfs_le32_write(r_nh + 0x04, r_eo + right_total);
                    }
                    ntfs_write_indx(vol, ia_runs, ia_run_count, right_vcn,
                                     indx_size, rbuf);
                    pmm_free_frame(right_phys);
                }

                kfree(combined);

                /* Update promote entry: append right_vcn as its child VCN */
                {
                    uint32_t old_len = promote_len;
                    uint32_t new_len2;
                    uint8_t tmp[640];
                    new_len2 = (old_len + 8 + 7) & ~7u;
                    ntfs_memcpy(tmp, promote_entry, old_len);
                    ntfs_memset(tmp + old_len, 0, new_len2 - old_len);
                    ntfs_le16_write(tmp + 0x08, (uint16_t)new_len2);
                    tmp[0x0C] |= NTFS_INDEX_ENTRY_SUBNODE;
                    ntfs_le64_write(tmp + new_len2 - 8, cur_vcn);
                    ntfs_memcpy(promote_entry, tmp, new_len2);
                    promote_len = new_len2;
                }

                cur_entry   = promote_entry;
                cur_len     = promote_len;
                need_promote = 1;
                promote_right_vcn = right_vcn;
                level--;
                continue;
            } else {
                rc = NTFS_ERR_IO;
                break;
            }
        }

        if (indx_phys) pmm_free_frame(indx_phys);
        (void)need_promote;
        (void)promote_len;
    }

    /* Commit or abort transaction */
    if (txn) {
        if (rc == NTFS_OK)
            ntfs_txn_commit(txn);
        else
            ntfs_txn_abort(txn);
        ntfs_txn_free(txn);
    }

    if (rc == NTFS_OK)
        klog(LOG_DEBUG, "ntfs", "index_insert: inserted inode %llu into dir %llu",
             child_inode, dir_inode);
    else
        klog(LOG_ERROR, "ntfs", "index_insert: failed for dir %llu (err %d)",
             dir_inode, rc);

    return rc;
}
