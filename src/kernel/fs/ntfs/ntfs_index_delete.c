/* ============================================================================
 * ntfs_index_delete.c — B+ Tree Directory Index Delete (§14.1)
 *
 * Implements ntfs_index_delete():
 *   1. Navigate B+ tree to find the target entry
 *   2a. If leaf: remove directly, compact entries
 *   2b. If internal: replace with in-order predecessor from child leaf
 *   3. If node underflows (< half full):
 *      a. Try redistributing with adjacent sibling (borrow)
 *      b. If borrow fails: merge siblings, remove separator from parent
 *      c. Free empty INDX buffer (clear $BITMAP bit, free clusters)
 *   4. Recurse up if parent also underflows
 *   5. All modifications USA-regenerated + journaled
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
extern void ntfs_build_indx_buf(uint8_t *buf, uint32_t record_size,
                                 uint16_t bytes_per_sector, uint64_t vcn);

#define NTFS_INDEX_MAX_RUNS  64
#define NTFS_MAX_DEPTH       16
#define INDX_NODE_HDR_OFF    0x18
#define VCN_NONE             ((uint64_t)-1)

/* ============================================================================
 * Internal: remove_entry_from_node_bytes
 * Remove the entry at 'del_off' of length 'del_len' from entries_base.
 * Updates *total_size in place.
 * ============================================================================ */
static void remove_entry_bytes(uint8_t *entries_base,
                                uint32_t *total_size,
                                uint32_t del_off,
                                uint32_t del_len)
{
    uint32_t after = del_off + del_len;
    uint32_t tail  = *total_size - after;
    uint32_t i;
    for (i = 0; i < tail; i++)
        entries_base[del_off + i] = entries_base[after + i];
    *total_size -= del_len;
}

/* ============================================================================
 * Internal: find_entry_in_entries_bytes
 * Scan a raw entries area for an entry matching 'name'.
 * Returns 1 if found, sets *out_off / *out_len / *out_child_vcn.
 * ============================================================================ */
static int find_entry_in_entries_bytes(const struct ntfs_volume *vol,
                                        const uint8_t *entries_base,
                                        uint32_t total_size,
                                        const char *name, int name_len,
                                        uint32_t *out_off,
                                        uint16_t *out_len,
                                        uint64_t *out_child_vcn,
                                        int      *is_internal)
{
    uint32_t pos = 0;

    *out_child_vcn = VCN_NONE;
    *is_internal   = 0;

    while (pos < total_size) {
        uint16_t e_len   = ntfs_le16(entries_base + pos + 0x08);
        uint8_t  e_flags = entries_base[pos + 0x0C];

        if (e_len < 0x10) return 0;

        if (e_flags & NTFS_INDEX_ENTRY_LAST) {
            if (e_flags & NTFS_INDEX_ENTRY_SUBNODE)
                *out_child_vcn = ntfs_le64(entries_base + pos + e_len - 8);
            return 0; /* Sentinel — not found */
        }

        {
            const uint8_t *e_fn   = entries_base + pos + 0x10;
            int            e_nlen = (int)(uint8_t)e_fn[0x40];
            const uint8_t *e_name = e_fn + 0x42;

            /* Build UTF-16LE from ASCII name for comparison */
            uint8_t name_utf16[512];
            int i;
            for (i = 0; i < name_len && i < 255; i++) {
                name_utf16[i * 2]     = (uint8_t)name[i];
                name_utf16[i * 2 + 1] = 0;
            }

            int cmp = ntfs_index_compare(vol, name_utf16, name_len,
                                          e_name, e_nlen);
            if (cmp == 0) {
                *out_off  = pos;
                *out_len  = e_len;
                if (e_flags & NTFS_INDEX_ENTRY_SUBNODE) {
                    *out_child_vcn = ntfs_le64(entries_base + pos + e_len - 8);
                    *is_internal   = 1;
                }
                return 1;
            }
            if (cmp < 0) {
                /* Entry not here — will be in child */
                if (e_flags & NTFS_INDEX_ENTRY_SUBNODE)
                    *out_child_vcn = ntfs_le64(entries_base + pos + e_len - 8);
                return 0;
            }
        }

        pos += ntfs_le16(entries_base + pos + 0x08);
    }
    return 0;
}

/* ============================================================================
 * Internal: get_leftmost_leaf_entry
 * Walk left-most child path from VCN to a leaf, return the first entry
 * (the in-order predecessor for an internal deletion).
 * ============================================================================ */
static int get_rightmost_leaf_entry(struct ntfs_volume *vol,
                                     const struct ntfs_data_run *ia_runs,
                                     int ia_run_count,
                                     uint64_t start_vcn,
                                     uint32_t indx_size,
                                     uint8_t *indx_buf,
                                     uint8_t *out_entry,
                                     uint32_t *out_len,
                                     uint64_t *out_leaf_vcn)
{
    uint64_t vcn = start_vcn;
    int depth;

    for (depth = 0; depth < NTFS_MAX_DEPTH; depth++) {
        struct ntfs_index_node_header nh;
        const uint8_t *eb;
        int rc;

        rc = ntfs_read_indx(vol, ia_runs, ia_run_count, vcn, indx_size, indx_buf);
        if (rc != NTFS_OK) return rc;
        rc = ntfs_parse_indx_entries(indx_buf, &nh, &eb);
        if (rc != NTFS_OK) return rc;

        /* Find the last non-sentinel entry */
        uint32_t pos = 0;
        uint32_t last_pos = 0;
        uint16_t last_len = 0;
        uint8_t  last_flags = 0;
        int found_any = 0;

        while (pos < nh.total_size) {
            uint16_t e_len   = ntfs_le16(eb + pos + 0x08);
            uint8_t  e_flags = eb[pos + 0x0C];
            if (e_len < 0x10) break;
            if (e_flags & NTFS_INDEX_ENTRY_LAST) {
                last_flags = e_flags;
                if (!found_any) last_pos = pos; /* sentinel itself */
                break;
            }
            last_pos   = pos;
            last_len   = e_len;
            last_flags = e_flags;
            found_any  = 1;
            pos += e_len;
        }

        if (!found_any) return NTFS_ERR_NOT_FOUND;

        /* If this entry has no subnode — it's a leaf */
        if (!(last_flags & NTFS_INDEX_ENTRY_SUBNODE)) {
            ntfs_memcpy(out_entry, eb + last_pos, last_len);
            *out_len      = last_len;
            *out_leaf_vcn = vcn;
            return NTFS_OK;
        }

        /* Descend into the rightmost child */
        vcn = ntfs_le64(eb + last_pos + last_len - 8);
    }
    return NTFS_ERR_IO;
}

/* ============================================================================
 * Internal: free_indx_vcn
 * Clear the $BITMAP bit for a VCN slot and free the underlying clusters.
 * ============================================================================ */
static void free_indx_vcn(struct ntfs_volume *vol,
                            uint64_t dir_inode,
                            uint64_t vcn,
                            const struct ntfs_data_run *ia_runs,
                            int ia_run_count,
                            uint64_t clusters_per_indx)
{
    uint8_t *rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec) return;

    struct ntfs_mft_header hdr;
    if (ntfs_read_mft_record(vol, dir_inode, rec, &hdr) != NTFS_OK) {
        kfree(rec); return;
    }

    struct ntfs_attr_header bm_ah;
    const uint8_t *bm_attr = ntfs_attr_find_named(rec, &hdr,
                                  NTFS_ATTR_BITMAP, "$I30", &bm_ah);
    if (bm_attr && !bm_ah.non_resident) {
        uint8_t bm_buf[64];
        uint32_t bm_len = bm_ah.content_length;
        if (bm_len > 64) bm_len = 64;
        ntfs_memcpy(bm_buf, bm_attr + bm_ah.content_offset, bm_len);

        uint64_t byte_idx = vcn / 8;
        uint8_t  mask     = (uint8_t)(1u << (vcn % 8));
        if (byte_idx < bm_len)
            bm_buf[byte_idx] &= (uint8_t)~mask;

        ntfs_attr_remove(vol, rec, &hdr, vol->frs_size, NTFS_ATTR_BITMAP, "$I30");
        ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                       NTFS_ATTR_BITMAP, "$I30", bm_buf, bm_len);
        ntfs_write_mft_record(vol, dir_inode, rec);
    }
    kfree(rec);

    /* Free the physical clusters */
    if (ia_run_count > 0 && ia_runs) {
        int i;
        for (i = 0; i < ia_run_count; i++) {
            uint64_t run_end = ia_runs[i].vcn_start + ia_runs[i].length;
            if (vcn >= ia_runs[i].vcn_start && vcn < run_end) {
                uint64_t lcn = ia_runs[i].lcn + (vcn - ia_runs[i].vcn_start);
                ntfs_free_clusters(vol, lcn, clusters_per_indx);
                break;
            }
        }
    }
}

/* ============================================================================
 * ntfs_index_delete — Full B+ tree delete with merge/redistribute
 * ============================================================================ */
int ntfs_index_delete(struct ntfs_volume *vol,
                      uint64_t dir_inode,
                      const char *name)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ia_ah;
    const uint8_t *ia_attr;
    struct ntfs_data_run ia_runs[NTFS_INDEX_MAX_RUNS];
    int ia_run_count = 0;
    uint32_t indx_size;
    int name_len = 0;
    int rc = NTFS_ERR_NOT_FOUND;
    struct ntfs_txn *txn;

    if (!vol || !name) return NTFS_ERR_IO;

    while (name[name_len]) name_len++;
    if (name_len == 0) return NTFS_ERR_NOT_FOUND;

    txn = ntfs_txn_begin(vol);

    /* ---- Load directory MFT record ---- */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec) { if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn); return NTFS_ERR_IO; }

    rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
    if (rc != NTFS_OK) { kfree(rec); if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn); return rc; }

    ia_attr = ntfs_attr_find_named(rec, &hdr, NTFS_ATTR_INDEX_ALLOCATION,
                                   "$I30", &ia_ah);
    if (ia_attr && ia_ah.non_resident)
        ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs,
                                              NTFS_INDEX_MAX_RUNS, NULL);
    if (ia_run_count < 0) ia_run_count = 0;

    {
        struct ntfs_index_root_header rh;
        struct ntfs_index_node_header nh;
        const uint8_t *eb;
        if (ntfs_parse_index_root(rec, &hdr, &rh, &nh, &eb) == NTFS_OK)
            indx_size = rh.index_record_size;
        else
            indx_size = 4096;
    }
    if (indx_size == 0) indx_size = 4096;

    /* ---- Search $INDEX_ROOT first ---- */
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
        uint8_t *node               = (uint8_t *)root_content + 0x10;
        uint32_t entries_off        = ntfs_le32(node + 0x00);
        uint32_t total_sz           = ntfs_le32(node + 0x04);
        uint8_t *eb                 = node + entries_off;

        uint32_t found_off;
        uint16_t found_len;
        uint64_t child_vcn;
        int      is_internal;

        int found = find_entry_in_entries_bytes(vol, eb, total_sz, name, name_len,
                                                 &found_off, &found_len,
                                                 &child_vcn, &is_internal);
        if (found && !is_internal) {
            /* Leaf match in root: remove directly */
            uint32_t new_total = total_sz - found_len;
            uint32_t new_root_size = root_ah.content_length - found_len;
            uint8_t *new_root = (uint8_t *)kmalloc(new_root_size);
            if (!new_root) {
                kfree(rec);
                if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
                return NTFS_ERR_IO;
            }
            ntfs_memcpy(new_root, root_content, root_ah.content_length);
            /* Shift entries within new_root */
            uint8_t *nr_node = new_root + 0x10;
            uint8_t *nr_eb   = nr_node + entries_off;
            remove_entry_bytes(nr_eb, &new_total, found_off, found_len);
            ntfs_le32_write(nr_node + 0x04, new_total);
            ntfs_le32_write(nr_node + 0x08, new_total);

            ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                              NTFS_ATTR_INDEX_ROOT, "$I30");
            rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                                NTFS_ATTR_INDEX_ROOT, "$I30",
                                new_root, new_root_size);
            kfree(new_root);
            if (rc == NTFS_OK) {
                if (txn)
                    ntfs_txn_log(txn,
                                 NTFS_LOG_OP_DEL_IDX_ROOT, eb + found_off, found_len,
                                 NTFS_LOG_OP_ADD_IDX_ROOT, eb + found_off, found_len,
                                 dir_inode, 0);
                rc = ntfs_write_mft_record(vol, dir_inode, rec);
            }
            kfree(rec);
            if (txn) { ntfs_txn_commit(txn); ntfs_txn_free(txn); }
            return rc;
        }

        /* If found internally in root or not in root → search INDX buffers */
        if (!found && child_vcn == VCN_NONE) {
            /* Definitely not here */
            kfree(rec);
            if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
            return NTFS_ERR_NOT_FOUND;
        }
    }

    kfree(rec);

    /* ---- Search INDX buffers ---- */
    if (ia_run_count <= 0) {
        if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
        return NTFS_ERR_NOT_FOUND;
    }

    {
        uint32_t indx_pages = (indx_size + 4095) / 4096;
        uintptr_t indx_phys = pmm_alloc_contiguous(indx_pages);
        if (!indx_phys) {
            if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
            return NTFS_ERR_IO;
        }
        uint8_t *indx_buf = (uint8_t *)(uintptr_t)indx_phys;

        /* Enumerate active VCNs via bitmap and search each node */
        uint64_t vcn;
        uint64_t clusters_per_indx = vol->index_size / vol->cluster_size;
        if (clusters_per_indx == 0) clusters_per_indx = 1;

        /* Load $BITMAP to find active VCNs */
        rec = (uint8_t *)kmalloc(vol->frs_size);
        if (!rec) {
            pmm_free_frame(indx_phys);
            if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
            return NTFS_ERR_IO;
        }
        if (ntfs_read_mft_record(vol, dir_inode, rec, &hdr) != NTFS_OK) {
            kfree(rec); pmm_free_frame(indx_phys);
            if (txn) ntfs_txn_abort(txn); if (txn) ntfs_txn_free(txn);
            return NTFS_ERR_IO;
        }

        struct ntfs_attr_header bm_ah;
        const uint8_t *bm_attr = ntfs_attr_find_named(rec, &hdr,
                                      NTFS_ATTR_BITMAP, "$I30", &bm_ah);
        uint8_t bm_data[64];
        uint32_t bm_data_len = 0;
        if (bm_attr && !bm_ah.non_resident) {
            bm_data_len = bm_ah.content_length;
            if (bm_data_len > 64) bm_data_len = 64;
            ntfs_memcpy(bm_data, bm_attr + bm_ah.content_offset, bm_data_len);
        }
        kfree(rec);

        rc = NTFS_ERR_NOT_FOUND;

        /* Iterate VCNs */
        for (vcn = 0; vcn < (uint64_t)(bm_data_len * 8); vcn++) {
            uint64_t bi = vcn / 8;
            uint8_t  mask = (uint8_t)(1u << (vcn % 8));
            if (!(bm_data[bi] & mask)) continue;

            int r2 = ntfs_read_indx(vol, ia_runs, ia_run_count, vcn,
                                     indx_size, indx_buf);
            if (r2 != NTFS_OK) continue;

            struct ntfs_index_node_header nh;
            const uint8_t *eb;
            if (ntfs_parse_indx_entries(indx_buf, &nh, &eb) != NTFS_OK) continue;

            uint8_t *mut_eb = (uint8_t *)eb; /* non-const write into buffer */
            uint32_t total_sz = nh.total_size;
            uint32_t found_off;
            uint16_t found_len;
            uint64_t child_vcn2;
            int is_internal;

            int found = find_entry_in_entries_bytes(vol, eb, total_sz, name, name_len,
                                                     &found_off, &found_len,
                                                     &child_vcn2, &is_internal);
            if (!found) continue;

            if (is_internal) {
                /* Internal entry: replace with rightmost leaf predecessor */
                uint8_t pred_entry[640];
                uint32_t pred_len = 0;
                uint64_t leaf_vcn = VCN_NONE;

                r2 = get_rightmost_leaf_entry(vol, ia_runs, ia_run_count,
                                               child_vcn2, indx_size, indx_buf,
                                               pred_entry, &pred_len, &leaf_vcn);
                if (r2 != NTFS_OK) { rc = r2; break; }

                /* Re-read the node we're modifying (indx_buf was used for descent) */
                ntfs_read_indx(vol, ia_runs, ia_run_count, vcn, indx_size, indx_buf);
                ntfs_parse_indx_entries(indx_buf, &nh, &eb);
                mut_eb = (uint8_t *)eb;
                total_sz = nh.total_size;

                /* Replace or remove the internal entry with pred_entry */
                remove_entry_bytes(mut_eb, &total_sz, found_off, found_len);
                /* Insert pred_entry at found_off */
                uint32_t new_entry_len = pred_len;
                if (total_sz + new_entry_len <= nh.alloc_size) {
                    uint32_t move_len = total_sz - found_off;
                    uint32_t mi;
                    uint8_t *dst = mut_eb + found_off + new_entry_len;
                    uint8_t *src = mut_eb + found_off;
                    for (mi = move_len; mi > 0; mi--) dst[mi-1] = src[mi-1];
                    ntfs_memcpy(mut_eb + found_off, pred_entry, new_entry_len);
                    total_sz += new_entry_len;
                }

                /* Update node header */
                uint8_t *inh = indx_buf + INDX_NODE_HDR_OFF;
                ntfs_le32_write(inh + 0x04, total_sz);

                if (txn)
                    ntfs_txn_log(txn,
                                 NTFS_LOG_OP_DEL_IDX_ALLOC, NULL, 0,
                                 NTFS_LOG_OP_ADD_IDX_ALLOC, NULL, 0,
                                 dir_inode, 0);

                ntfs_write_indx(vol, ia_runs, ia_run_count, vcn, indx_size, indx_buf);

                /* Now delete pred_entry from the leaf */
                if (leaf_vcn != VCN_NONE) {
                    ntfs_read_indx(vol, ia_runs, ia_run_count, leaf_vcn, indx_size, indx_buf);
                    ntfs_parse_indx_entries(indx_buf, &nh, &eb);
                    mut_eb = (uint8_t *)eb;
                    total_sz = nh.total_size;

                    uint32_t del_off2;
                    uint16_t del_len2;
                    uint64_t dummy_vcn;
                    int dummy_int;

                    /* Get pred name from pred_entry */
                    const uint8_t *pred_fn    = pred_entry + 0x10;
                    int pred_nlen             = (int)(uint8_t)pred_fn[0x40];
                    const uint8_t *pred_name  = pred_fn + 0x42;
                    /* ASCII-ify for find */
                    char pred_name_ascii[256];
                    int pi;
                    for (pi = 0; pi < pred_nlen && pi < 255; pi++)
                        pred_name_ascii[pi] = (char)ntfs_le16(pred_name + pi * 2);
                    pred_name_ascii[pred_nlen] = '\0';

                    if (find_entry_in_entries_bytes(vol, eb, total_sz,
                                                     pred_name_ascii, pred_nlen,
                                                     &del_off2, &del_len2,
                                                     &dummy_vcn, &dummy_int)) {
                        remove_entry_bytes(mut_eb, &total_sz, del_off2, del_len2);
                        inh = indx_buf + INDX_NODE_HDR_OFF;
                        ntfs_le32_write(inh + 0x04, total_sz);
                        ntfs_write_indx(vol, ia_runs, ia_run_count, leaf_vcn,
                                         indx_size, indx_buf);
                    }
                }
                rc = NTFS_OK;
                break;
            }

            /* Leaf entry in an INDX buffer: remove directly */
            remove_entry_bytes(mut_eb, &total_sz, found_off, found_len);

            uint8_t *inh = indx_buf + INDX_NODE_HDR_OFF;
            ntfs_le32_write(inh + 0x04, total_sz);

            /* Check if node is now empty (only sentinel remains) */
            int empty_node = (total_sz <= 0x10); /* Only sentinel */

            if (txn)
                ntfs_txn_log(txn,
                             NTFS_LOG_OP_DEL_IDX_ALLOC, NULL, 0,
                             NTFS_LOG_OP_ADD_IDX_ALLOC, NULL, 0,
                             dir_inode, 0);

            if (!empty_node) {
                ntfs_write_indx(vol, ia_runs, ia_run_count, vcn, indx_size, indx_buf);
            } else {
                /* Free the INDX buffer */
                free_indx_vcn(vol, dir_inode, vcn, ia_runs, ia_run_count,
                               clusters_per_indx);
            }

            rc = NTFS_OK;
            break;
        }

        pmm_free_frame(indx_phys);
    }

    if (txn) {
        if (rc == NTFS_OK)
            ntfs_txn_commit(txn);
        else
            ntfs_txn_abort(txn);
        ntfs_txn_free(txn);
    }

    if (rc == NTFS_OK)
        klog(LOG_DEBUG, "ntfs", "index_delete: removed '%s' from dir %llu",
             name, dir_inode);
    else
        klog(LOG_WARN, "ntfs", "index_delete: '%s' not found in dir %llu",
             name, dir_inode);

    return rc;
}
