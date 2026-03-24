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
    {
        struct ntfs_attr_header ia_ah;
        const uint8_t *ia_attr = ntfs_attr_find_named(rec, &hdr,
                                      NTFS_ATTR_INDEX_ALLOCATION, "$I30", &ia_ah);
        struct ntfs_nonres_header nrhdr;
        if (ia_attr && ia_ah.non_resident) {
            struct ntfs_data_run runs[NTFS_INDEX_MAX_RUNS];
            int rc2 = ntfs_decode_data_runs(ia_attr, runs, NTFS_INDEX_MAX_RUNS,
                                             &nrhdr);
            if (rc2 > 0)
                /* Next VCN = last_vcn+1 (each INDX = vol->index_size / cluster_size clusters) */
                new_vcn = (nrhdr.real_size / vol->cluster_size);
            else
                new_vcn = 0;
        } else {
            new_vcn = 0;
        }
    }

    /* Allocate one (or more) contiguous clusters for INDX record */
    uint64_t clusters_per_indx = vol->index_size / vol->cluster_size;
    if (clusters_per_indx == 0) clusters_per_indx = 1;

    lcn = ntfs_alloc_clusters(vol, clusters_per_indx, 0);
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

        /* Append new run */
        ntfs_memcpy(new_runs, old_runs,
                    (uint32_t)(old_count * (int)sizeof(struct ntfs_data_run)));
        new_runs[old_count].vcn_start = new_vcn;
        new_runs[old_count].lcn       = lcn;
        new_runs[old_count].length    = clusters_per_indx;
        new_count = old_count + 1;

        /* Remove old IA, rebuild as proper non-resident attribute.
         * $INDEX_ALLOCATION MUST be non-resident — it contains INDX
         * buffers on disk, referenced by the run list.  ntfs_attr_add()
         * would create a resident attribute for small data, which is
         * invalid for type 0xA0. */
        ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                          NTFS_ATTR_INDEX_ALLOCATION, "$I30");

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

            ntfs_index_find_pos(vol, entries_base, total_entries_size,
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

                ntfs_index_find_pos(vol, eb, nh.total_size,
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

        /* Allocate INDX buffer for operations */
        uintptr_t indx_phys = 0;
        uint8_t *indx_buf   = NULL;
        if (ia_run_count > 0) {
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
                    uint32_t total_sz    = ntfs_le32(node + 0x04);
                    uint32_t alloc_sz   = ntfs_le32(node + 0x08);
                    uint8_t *eb = node + entries_off;

                    /* Check fit */
                    int fits = (total_sz + cur_len <= alloc_sz) &&
                               ((uint32_t)(root_ah.total_length + cur_len) <=
                                hdr.alloc_size - hdr.used_size + root_ah.total_length);

                    if (fits) {
                        /* Insert directly into root node */
                        /* Build new root content (old + cur_len bytes) */
                        uint32_t new_total = total_sz + cur_len;
                        uint32_t old_cs = root_ah.content_length;
                        uint32_t new_cs = old_cs + cur_len;
                        (void)new_total;
                        uint8_t *new_root = (uint8_t *)kmalloc(new_cs);
                        if (!new_root) { rc = NTFS_ERR_IO; kfree(rec); break; }

                        ntfs_memcpy(new_root, root_content, old_cs);
                        /* Shift entries from ins_off */
                        uint8_t *new_node = new_root + 0x10;
                        uint8_t *new_eb   = new_node + entries_off;
                        uint32_t move_len = total_sz - ins_off;
                        uint8_t *dst = new_eb + ins_off + cur_len;
                        uint8_t *src = new_eb + ins_off;
                        uint32_t mi;
                        for (mi = move_len; mi > 0; mi--) dst[mi-1] = src[mi-1];
                        ntfs_memcpy(new_eb + ins_off, cur_entry, cur_len);
                        /* Update node header total_size and alloc_size */
                        ntfs_le32_write(new_node + 0x04, new_total);
                        ntfs_le32_write(new_node + 0x08, new_total);

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
                     * then make root a single-separator internal node. */
                    {
                        uint64_t new_vcn = 0;
                        kfree(rec); /* Release — re-read after alloc */
                        rc = allocate_indx_vcn(vol, dir_inode, &new_vcn);
                        if (rc != NTFS_OK) break;

                        /* Re-read updated MFT (allocation modified it) */
                        rec = (uint8_t *)kmalloc(vol->frs_size);
                        if (!rec) { rc = NTFS_ERR_IO; break; }
                        rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
                        if (rc != NTFS_OK) { kfree(rec); break; }

                        /* Reload ia_runs after alloc */
                        ia_attr = ntfs_attr_find_named(rec, &hdr,
                                    NTFS_ATTR_INDEX_ALLOCATION, "$I30", &ia_ah);
                        if (ia_attr && ia_ah.non_resident)
                            ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs,
                                                                  NTFS_INDEX_MAX_RUNS, NULL);

                        /* Reload root content */
                        root_attr = ntfs_attr_find_named(rec, &hdr,
                                        NTFS_ATTR_INDEX_ROOT, "$I30", &root_ah);
                        root_content = root_attr + root_ah.content_offset;
                        node = (uint8_t *)root_content + 0x10;
                        entries_off = ntfs_le32(node + 0x00);
                        total_sz    = ntfs_le32(node + 0x04);
                        eb = node + entries_off;

                        /* Build new INDX buffer with all old root entries + new entry inserted */
                        if (indx_buf) {
                            ntfs_build_indx_buf(indx_buf, indx_size,
                                                 vol->bytes_per_sector, new_vcn);

                            /* Build node in indx_buf: node hdr at INDX_NODE_HDR_OFF */
                            uint8_t *inode_hdr = indx_buf + INDX_NODE_HDR_OFF;
                            uint32_t i_entries_off = ntfs_le32(inode_hdr + 0x00);
                            uint8_t *ieb = inode_hdr + i_entries_off;
                            uint32_t i_total = ntfs_le32(inode_hdr + 0x04);
                            uint32_t i_alloc = ntfs_le32(inode_hdr + 0x08);

                            /* Copy existing root entries into INDX */
                            uint32_t ins_total = total_sz + cur_len;
                            if (ins_total <= i_alloc) {
                                /* Insert at correct position */
                                ntfs_memcpy(ieb, eb, ins_off);
                                ntfs_memcpy(ieb + ins_off, cur_entry, cur_len);
                                ntfs_memcpy(ieb + ins_off + cur_len,
                                             eb + ins_off,
                                             total_sz - ins_off);
                                i_total = ins_total;
                                ntfs_le32_write(inode_hdr + 0x04, i_total);
                                ntfs_le32_write(inode_hdr + 0x08, i_total);
                            }

                            /* Write new INDX buffer */
                            if (txn)
                                ntfs_txn_log(txn,
                                             NTFS_LOG_OP_ADD_IDX_ALLOC, ieb, (uint16_t)i_total,
                                             NTFS_LOG_OP_DEL_IDX_ALLOC, NULL, 0,
                                             dir_inode, 0);
                            ntfs_write_indx(vol, ia_runs, ia_run_count,
                                             new_vcn, indx_size, indx_buf);

                            /* Reset root to empty internal node pointing to new_vcn */
                            {
                                uint32_t sentinel_len = 0x10 + 8; /* LAST + SUBNODE = 24 bytes */
                                uint32_t new_root_size = 0x10 + entries_off + sentinel_len;
                                uint8_t *new_root_buf = (uint8_t *)kmalloc(new_root_size);
                                if (new_root_buf) {
                                    /* root header (0x10 bytes) */
                                    ntfs_memcpy(new_root_buf, root_content, 0x10);
                                    /* node header */
                                    uint8_t *nh2 = new_root_buf + 0x10;
                                    ntfs_le32_write(nh2 + 0x00, entries_off);
                                    ntfs_le32_write(nh2 + 0x04, sentinel_len);
                                    ntfs_le32_write(nh2 + 0x08, sentinel_len);
                                    nh2[0x0C] = 0x01; /* has_children */
                                    /* Sentinel with child VCN */
                                    uint8_t *sent = nh2 + entries_off;
                                    ntfs_memset(sent, 0, sentinel_len);
                                    ntfs_le16_write(sent + 0x08, (uint16_t)sentinel_len);
                                    sent[0x0C] = NTFS_INDEX_ENTRY_LAST | NTFS_INDEX_ENTRY_SUBNODE;
                                    ntfs_le64_write(sent + sentinel_len - 8, new_vcn);

                                    ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                                                      NTFS_ATTR_INDEX_ROOT, "$I30");
                                    ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                                                   NTFS_ATTR_INDEX_ROOT, "$I30",
                                                   new_root_buf, new_root_size);
                                    kfree(new_root_buf);
                                }
                            }

                            rc = ntfs_write_mft_record(vol, dir_inode, rec);
                            need_promote = 0;
                        }
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
                uint32_t i_total      = ntfs_le32(inode_hdr + 0x04);
                uint32_t i_alloc      = ntfs_le32(inode_hdr + 0x08);
                uint8_t *ieb          = inode_hdr + i_entries_off;

                if (i_total + cur_len <= i_alloc) {
                    /* Fits — simple insert */
                    uint32_t move_len = i_total - ins_off;
                    uint8_t *dst = ieb + ins_off + cur_len;
                    uint8_t *src = ieb + ins_off;
                    uint32_t mi;
                    for (mi = move_len; mi > 0; mi--) dst[mi-1] = src[mi-1];
                    ntfs_memcpy(ieb + ins_off, cur_entry, cur_len);
                    i_total += cur_len;
                    ntfs_le32_write(inode_hdr + 0x04, i_total);
                    ntfs_le32_write(inode_hdr + 0x08, i_total);

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

                /* Left node: entries [0, median_off) */
                ntfs_memset(ieb, 0, i_alloc);
                ntfs_memcpy(ieb, combined, median_off);
                ntfs_le32_write(inode_hdr + 0x04, median_off);
                ntfs_write_indx(vol, ia_runs, ia_run_count, cur_vcn,
                                 indx_size, indx_buf);

                /* Right node: entries [median_off + med_len, end) */
                uintptr_t right_phys = pmm_alloc_contiguous((indx_size + 4095) / 4096);
                if (right_phys) {
                    uint8_t *rbuf = (uint8_t *)(uintptr_t)right_phys;
                    ntfs_build_indx_buf(rbuf, indx_size, vol->bytes_per_sector, right_vcn);

                    uint8_t *r_nh  = rbuf + INDX_NODE_HDR_OFF;
                    uint32_t r_eo  = ntfs_le32(r_nh + 0x00);
                    uint8_t *r_eb  = r_nh + r_eo;
                    uint32_t r_al  = ntfs_le32(r_nh + 0x08);
                    uint32_t right_sz = total_with_new - median_off - med_len;
                    if (right_sz <= r_al) {
                        ntfs_memcpy(r_eb, combined + median_off + med_len, right_sz);
                        ntfs_le32_write(r_nh + 0x04, right_sz);
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
                    ntfs_le64_write(tmp + new_len2 - 8, right_vcn);
                    ntfs_memcpy(promote_entry, tmp, new_len2);
                    promote_len = new_len2;
                }

                cur_entry   = promote_entry;
                cur_len     = promote_len;
                need_promote = 1;
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
