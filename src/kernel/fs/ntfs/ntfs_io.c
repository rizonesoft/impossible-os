/* ============================================================================
 * ntfs_io.c — File Data Reader, Index Parsing, Directory Operations
 *
 * Contains:
 *   - File data reader (resident + non-resident via data runs)
 *   - $INDEX_ROOT parser (B+ tree root node)
 *   - INDX buffer reader ($INDEX_ALLOCATION)
 *   - Directory lookup (B+ tree search)
 *   - Path resolution
 *   - Directory enumeration (readdir)
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ============================================================================
 * File Data Reader — §4.2
 *
 * Reads actual file content via two paths:
 *   1. Resident: small files stored inline in the MFT record attribute
 *   2. Non-resident: data stored on disk clusters, accessed via run-list
 *
 * For non-resident reads:
 *   - Convert file_offset to VCN (which cluster?)
 *   - Find the run covering that VCN
 *   - Translate VCN to LCN via the run's base LCN
 *   - Read clusters from disk via blkdev_read()
 *   - Handle partial first/last cluster, sparse runs, multi-run spanning
 * ============================================================================ */

int64_t ntfs_read_resident_data(const uint8_t *attr,
                                uint64_t offset, uint64_t length,
                                void *buffer)
{
    uint32_t content_len;
    uint16_t content_off;
    const uint8_t *data;
    uint64_t avail;

    if (!attr || !buffer)
        return -1;

    /* Must be resident */
    if (attr[0x08] != 0)
        return -1;

    content_len = ntfs_le32(attr + 0x10);
    content_off = ntfs_le16(attr + 0x14);
    data = attr + content_off;

    if (offset >= content_len)
        return 0;  /* Past end of content */

    avail = content_len - offset;
    if (length > avail)
        length = avail;

    ntfs_memcpy(buffer, data + offset, length);
    return (int64_t)length;
}

int64_t ntfs_read_data(struct ntfs_volume *vol,
                       const struct ntfs_data_run *runs, int run_count,
                       uint64_t real_size,
                       uint64_t file_offset, uint64_t length,
                       void *buffer)
{
    uint8_t *buf = (uint8_t *)buffer;
    uint64_t cluster_size;
    uint64_t bytes_read = 0;
    int i;

    if (!vol || !runs || !buffer || run_count <= 0)
        return -1;

    cluster_size = vol->cluster_size;

    /* Cap read at real_size */
    if (file_offset >= real_size)
        return 0;
    if (file_offset + length > real_size)
        length = real_size - file_offset;

    while (bytes_read < length) {
        uint64_t remaining = length - bytes_read;
        uint64_t vcn = (file_offset + bytes_read) / cluster_size;
        uint64_t cluster_off = (file_offset + bytes_read) % cluster_size;
        uint64_t run_vcn_end;
        uint64_t clusters_in_run;
        uint64_t vcn_in_run;
        uint64_t chunk;

        /* Find the run covering this VCN */
        for (i = 0; i < run_count; i++) {
            run_vcn_end = runs[i].vcn_start + runs[i].length;
            if (vcn >= runs[i].vcn_start && vcn < run_vcn_end)
                break;
        }
        if (i >= run_count)
            break;  /* VCN not covered by any run — truncated file? */

        vcn_in_run = vcn - runs[i].vcn_start;
        clusters_in_run = runs[i].length - vcn_in_run;

        /* How many bytes can we read from this run? */
        chunk = clusters_in_run * cluster_size - cluster_off;
        if (chunk > remaining)
            chunk = remaining;

        if (runs[i].lcn == NTFS_LCN_SPARSE) {
            /* Sparse run — fill with zeros */
            ntfs_memset(buf + bytes_read, 0, chunk);
        } else {
            uint64_t disk_lcn = runs[i].lcn + vcn_in_run;

            if (cluster_off == 0 && chunk >= cluster_size) {
                /* Aligned, full-cluster read — fast path */
                uint64_t full_clusters = chunk / cluster_size;
                uint64_t full_bytes = full_clusters * cluster_size;
                uint64_t lba = disk_lcn * vol->sectors_per_cluster;
                uint32_t sectors = (uint32_t)(full_clusters *
                                              vol->sectors_per_cluster);

                if (blkdev_read(vol->dev, lba, sectors,
                                buf + bytes_read) != 0)
                    return -1;

                /* Handle trailing partial cluster */
                if (full_bytes < chunk) {
                    /* Read one more cluster into a bounce buffer */
                    uintptr_t bounce_phys = pmm_alloc_contiguous(1);
                    uint8_t *bounce = (uint8_t *)(uintptr_t)bounce_phys;
                    uint64_t trail_lba;
                    if (!bounce)
                        return -1;

                    trail_lba = (disk_lcn + full_clusters) *
                                 vol->sectors_per_cluster;
                    if (blkdev_read(vol->dev, trail_lba,
                                    vol->sectors_per_cluster,
                                    bounce) != 0) {
                        pmm_free_frame(bounce_phys);
                        return -1;
                    }
                    ntfs_memcpy(buf + bytes_read + full_bytes, bounce,
                                chunk - full_bytes);
                    pmm_free_frame(bounce_phys);
                }
            } else {
                /* Partial cluster read — use bounce buffer */
                uintptr_t bounce_phys = pmm_alloc_contiguous(1);
                uint8_t *bounce = (uint8_t *)(uintptr_t)bounce_phys;
                uint64_t pos = 0;
                uint64_t cur_off = cluster_off;
                uint64_t cur_lcn = disk_lcn;

                if (!bounce)
                    return -1;

                /* Read one cluster at a time */
                while (pos < chunk) {
                    uint64_t lba = cur_lcn * vol->sectors_per_cluster;
                    uint64_t avail = cluster_size - cur_off;
                    uint64_t to_copy = chunk - pos;
                    if (to_copy > avail)
                        to_copy = avail;

                    if (blkdev_read(vol->dev, lba,
                                    vol->sectors_per_cluster,
                                    bounce) != 0) {
                        pmm_free_frame(bounce_phys);
                        return -1;
                    }
                    ntfs_memcpy(buf + bytes_read + pos,
                                bounce + cur_off, to_copy);

                    pos += to_copy;
                    cur_off = 0;  /* Subsequent clusters start at offset 0 */
                    cur_lcn++;
                }

                pmm_free_frame(bounce_phys);
            }
        }

        bytes_read += chunk;
    }

    return (int64_t)bytes_read;
}

/* Maximum data runs we decode for a single $DATA attribute */
#define NTFS_MAX_DATA_RUNS  64

int64_t ntfs_read_file_data(const uint8_t *record,
                            const struct ntfs_mft_header *hdr,
                            struct ntfs_volume *vol,
                            uint64_t file_offset, uint64_t length,
                            void *buffer)
{
    struct ntfs_attr_header ah;
    const uint8_t *attr;

    if (!record || !hdr || !vol || !buffer)
        return -1;

    /* Find the unnamed $DATA attribute (type 0x80) */
    attr = ntfs_attr_find(record, hdr, NTFS_ATTR_DATA, &ah);
    if (!attr)
        return -1;

    /* Skip named $DATA attributes (ADS — alternate data streams) */
    while (attr && ah.name_length > 0) {
        attr = ntfs_attr_next(attr, record, hdr->used_size);
        if (attr) {
            uint32_t type = ntfs_le32(attr + 0x00);
            if (type != NTFS_ATTR_DATA)
                attr = NULL;
            else
                ntfs_attr_parse(attr, &ah);
        }
    }
    if (!attr)
        return -1;

    if (ah.non_resident == 0) {
        /* Resident — direct copy from attribute content */
        return ntfs_read_resident_data(attr, file_offset, length, buffer);
    } else {
        /* Non-resident — decode runs and read from disk */
        struct ntfs_data_run runs[NTFS_MAX_DATA_RUNS];
        struct ntfs_nonres_header nrhdr;
        int run_count;

        run_count = ntfs_decode_data_runs(attr, runs, NTFS_MAX_DATA_RUNS,
                                          &nrhdr);
        if (run_count <= 0)
            return -1;

        /* Check for compressed attribute (flag 0x0001 + compression_unit > 0) */
        if ((ah.flags & NTFS_ATTR_FLAG_COMPRESSED) &&
            nrhdr.compression_unit > 0) {
            return ntfs_read_compressed_data(vol, runs, run_count,
                                              nrhdr.real_size,
                                              nrhdr.compression_unit,
                                              file_offset, length, buffer);
        }

        /* Check for encrypted attribute (flag 0x4000) — EFS §9.3 */
        if (ah.flags & NTFS_ATTR_FLAG_ENCRYPTED) {
            /* Mutual exclusion: cannot be both compressed and encrypted */
            if (ah.flags & NTFS_ATTR_FLAG_COMPRESSED)
                return -1;
            return ntfs_read_encrypted_data(vol, record, hdr,
                                             runs, run_count,
                                             nrhdr.real_size,
                                             file_offset, length, buffer);
        }

        return ntfs_read_data(vol, runs, run_count, nrhdr.real_size,
                              file_offset, length, buffer);
    }
}

/* ============================================================================
 * $INDEX_ROOT Parser — §5.1 (attribute type 0x90)
 * ============================================================================ */

/* Parse a single index entry at 'entry' into 'out'.
 * Returns 0 on success, -1 if entry looks corrupt. */
static int ntfs_parse_index_entry(const uint8_t *entry,
                                  struct ntfs_index_entry *out)
{
    uint64_t ref;

    out->raw = entry;

    /* MFT reference at 0x00 */
    ref = ntfs_le64(entry + 0x00);
    out->mft_reference = ref;
    out->mft_inode = ref & 0x0000FFFFFFFFFFFFULL;
    out->mft_seq   = (uint16_t)((ref >> 48) & 0xFFFF);

    /* Entry header */
    out->entry_length  = ntfs_le16(entry + 0x08);
    out->stream_length = ntfs_le16(entry + 0x0A);
    out->flags         = entry[0x0C];

    if (out->entry_length < 0x10)
        return -1;  /* Entry too small */

    /* Child VCN (if sub-node flag is set, stored at end of entry) */
    if (out->flags & NTFS_INDEX_ENTRY_SUBNODE) {
        out->child_vcn = ntfs_le64(entry + out->entry_length - 8);
    } else {
        out->child_vcn = 0;
    }

    /* Decode embedded $FILE_NAME payload if not the last (sentinel) entry */
    if (!(out->flags & NTFS_INDEX_ENTRY_LAST) && out->stream_length > 0) {
        /* parse_fn_content is in ntfs_filename.c — we decode inline here
         * using the same layout since $FILE_NAME content is at entry+0x10 */
        const uint8_t *fn_data = entry + 0x10;
        uint32_t fn_len = out->stream_length;
        uint64_t parent_ref;
        uint8_t nlen, fi;

        if (fn_len >= 0x42) {
            parent_ref = ntfs_le64(fn_data + 0x00);
            out->fn.parent_inode = parent_ref & 0x0000FFFFFFFFFFFFULL;
            out->fn.parent_seq   = (uint16_t)((parent_ref >> 48) & 0xFFFF);
            out->fn.creation_time     = ntfs_le64(fn_data + 0x08);
            out->fn.modification_time = ntfs_le64(fn_data + 0x10);
            out->fn.mft_change_time   = ntfs_le64(fn_data + 0x18);
            out->fn.access_time       = ntfs_le64(fn_data + 0x20);
            out->fn.allocated_size = ntfs_le64(fn_data + 0x28);
            out->fn.real_size      = ntfs_le64(fn_data + 0x30);
            out->fn.flags          = ntfs_le32(fn_data + 0x38);
            out->fn.name_length = fn_data[0x40];
            out->fn.name_space  = fn_data[0x41];

            nlen = out->fn.name_length;
            if (nlen > NTFS_MAX_NAME)
                nlen = NTFS_MAX_NAME;
            if (fn_len < (uint32_t)(0x42 + nlen * 2))
                nlen = (uint8_t)((fn_len - 0x42) / 2);

            for (fi = 0; fi < nlen; fi++) {
                uint16_t wc = ntfs_le16(fn_data + 0x42 + fi * 2);
                out->fn.name[fi] = (wc < 0x80) ? (char)wc : '?';
            }
            out->fn.name[nlen] = '\0';
        } else {
            out->fn.name[0] = '\0';
            out->fn.name_length = 0;
        }
    } else {
        /* Sentinel entry — clear filename */
        out->fn.name[0] = '\0';
        out->fn.name_length = 0;
    }

    return 0;
}

int ntfs_parse_index_root(const uint8_t *record,
                          const struct ntfs_mft_header *hdr,
                          struct ntfs_index_root_header *root_hdr,
                          struct ntfs_index_node_header *node_hdr,
                          const uint8_t **entries_base)
{
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    const uint8_t *data;
    const uint8_t *node;

    if (!record || !hdr || !root_hdr || !node_hdr || !entries_base)
        return NTFS_ERR_IO;

    /* Find $INDEX_ROOT named "$I30" (directory index on $FILE_NAME) */
    attr = ntfs_attr_find_named(record, hdr, NTFS_ATTR_INDEX_ROOT,
                                "$I30", &ah);
    if (!attr)
        return NTFS_ERR_BAD_MAGIC;

    /* Must be resident */
    if (ah.non_resident != 0)
        return NTFS_ERR_BAD_MAGIC;

    /* Need at least 0x20 bytes (root header + node header) */
    if (ah.content_length < 0x20)
        return NTFS_ERR_BAD_MAGIC;

    data = attr + ah.content_offset;

    /* Parse Index Root Header at data+0x00 */
    root_hdr->indexed_attr_type  = ntfs_le32(data + 0x00);
    root_hdr->collation_rule     = ntfs_le32(data + 0x04);
    root_hdr->index_record_size  = ntfs_le32(data + 0x08);
    root_hdr->clusters_per_index = data[0x0C];

    /* Parse Node Header at data+0x10 */
    node = data + 0x10;
    node_hdr->entries_offset = ntfs_le32(node + 0x00);
    node_hdr->total_size     = ntfs_le32(node + 0x04);
    node_hdr->alloc_size     = ntfs_le32(node + 0x08);
    node_hdr->flags          = node[0x0C];

    /* Entries start at node + entries_offset */
    *entries_base = node + node_hdr->entries_offset;

    return NTFS_OK;
}

const uint8_t *ntfs_index_entry_first(const uint8_t *entries_base,
                                      const struct ntfs_index_node_header *nh,
                                      struct ntfs_index_entry *out)
{
    if (!entries_base || !nh || !out)
        return NULL;

    if (nh->total_size < nh->entries_offset)
        return NULL;

    if (ntfs_parse_index_entry(entries_base, out) != 0)
        return NULL;

    return entries_base;
}

const uint8_t *ntfs_index_entry_next(const uint8_t *entry,
                                     const uint8_t *entries_base,
                                     const struct ntfs_index_node_header *nh,
                                     struct ntfs_index_entry *out)
{
    const uint8_t *next;
    struct ntfs_index_entry prev;

    if (!entry || !entries_base || !nh || !out)
        return NULL;

    /* Parse current entry to get its length (need to advance by it) */
    if (ntfs_parse_index_entry(entry, &prev) != 0)
        return NULL;

    /* If current entry is the last (sentinel), stop */
    if (prev.flags & NTFS_INDEX_ENTRY_LAST)
        return NULL;

    if (prev.entry_length == 0)
        return NULL;  /* Prevent infinite loop */

    next = entry + prev.entry_length;

    /* Bounds check: don't walk past allocated entries area */
    if ((uint32_t)(next - entries_base) >= nh->total_size)
        return NULL;

    if (ntfs_parse_index_entry(next, out) != 0)
        return NULL;

    return next;
}

/* ============================================================================
 * INDX Buffer Reader — §5.2 (attribute type 0xA0)
 * ============================================================================ */

int ntfs_read_indx(struct ntfs_volume *vol,
                   const struct ntfs_data_run *index_runs,
                   int index_run_count,
                   uint64_t vcn, uint32_t index_record_size,
                   uint8_t *buffer)
{
    uint64_t byte_offset;
    uint64_t clusters_per_indx;
    uint64_t target_vcn;
    uint64_t run_vcn_end;
    uint64_t vcn_in_run;
    uint64_t disk_lcn;
    uint64_t lba;
    uint32_t sectors;
    uint32_t magic;
    int i;
    int rc;

    if (!vol || !index_runs || !buffer || index_run_count <= 0)
        return NTFS_ERR_IO;

    /* How many allocation clusters per INDX record? */
    clusters_per_indx = index_record_size / vol->cluster_size;
    if (clusters_per_indx == 0)
        clusters_per_indx = 1;

    /* The VCN in $INDEX_ALLOCATION maps to INDX records, each spanning
     * clusters_per_indx clusters. The VCN from the index entry's child_vcn
     * is relative to the $INDEX_ALLOCATION's data runs. */
    byte_offset = vcn * vol->cluster_size;
    target_vcn = byte_offset / vol->cluster_size;

    /* Find the run covering this VCN */
    for (i = 0; i < index_run_count; i++) {
        run_vcn_end = index_runs[i].vcn_start + index_runs[i].length;
        if (target_vcn >= index_runs[i].vcn_start && target_vcn < run_vcn_end)
            break;
    }
    if (i >= index_run_count)
        return NTFS_ERR_IO;  /* VCN not found in runs */

    if (index_runs[i].lcn == NTFS_LCN_SPARSE) {
        /* Sparse INDX — shouldn't happen but handle gracefully */
        ntfs_memset(buffer, 0, index_record_size);
        return NTFS_ERR_BAD_MAGIC;
    }

    vcn_in_run = target_vcn - index_runs[i].vcn_start;
    disk_lcn   = index_runs[i].lcn + vcn_in_run;

    /* Read from disk */
    lba     = disk_lcn * vol->sectors_per_cluster;
    sectors = index_record_size / vol->bytes_per_sector;

    if (blkdev_read(vol->dev, lba, sectors, buffer) != 0)
        return NTFS_ERR_IO;

    /* Validate INDX magic */
    magic = ntfs_le32(buffer);
    if (magic != NTFS_INDX_MAGIC)
        return NTFS_ERR_BAD_MAGIC;

    /* Apply fixup (USA) — same as FILE records */
    rc = ntfs_apply_fixup(buffer, index_record_size, vol->bytes_per_sector);
    if (rc != NTFS_OK)
        return rc;

    return NTFS_OK;
}

int ntfs_parse_indx_entries(const uint8_t *buffer,
                            struct ntfs_index_node_header *node_hdr,
                            const uint8_t **entries_base)
{
    const uint8_t *node;

    if (!buffer || !node_hdr || !entries_base)
        return NTFS_ERR_IO;

    /* Node header is at offset 0x18 in the INDX record */
    node = buffer + 0x18;

    node_hdr->entries_offset = ntfs_le32(node + 0x00);
    node_hdr->total_size     = ntfs_le32(node + 0x04);
    node_hdr->alloc_size     = ntfs_le32(node + 0x08);
    node_hdr->flags          = node[0x0C];

    /* Entries start at node + entries_offset */
    *entries_base = node + node_hdr->entries_offset;

    return NTFS_OK;
}

/* ============================================================================
 * Directory Lookup — §5.3
 * ============================================================================ */

/* Maximum B+ tree depth to prevent infinite loops on corrupt volumes */
#define NTFS_MAX_TREE_DEPTH  16

/* Maximum data runs for $INDEX_ALLOCATION */
#define NTFS_MAX_INDEX_RUNS  64

/* ASCII uppercase (fallback until $UpCase loaded in §7.1) */
static uint16_t ntfs_toupper(uint16_t c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

/* Case-insensitive comparison of ASCII name vs UTF-16LE index entry name.
 * Returns negative if name < entry, 0 if equal, positive if name > entry.
 * Both sides are uppercased before comparison. */
static int ntfs_name_cmp_i(const char *name, int name_len,
                            const uint8_t *entry_name_utf16, int entry_name_len)
{
    int i;
    int min_len = name_len < entry_name_len ? name_len : entry_name_len;

    for (i = 0; i < min_len; i++) {
        uint16_t a = ntfs_toupper((uint16_t)(uint8_t)name[i]);
        uint16_t b = ntfs_toupper(ntfs_le16(entry_name_utf16 + i * 2));

        if (a != b)
            return (int)a - (int)b;
    }

    /* If all compared chars are equal, shorter name sorts first */
    return name_len - entry_name_len;
}

/* Search sorted index entries in a single node for 'name'. */
static int ntfs_search_index_entries(const uint8_t *entries_base,
                                      const struct ntfs_index_node_header *nh,
                                      const char *name, int name_len,
                                      uint64_t *out_inode,
                                      uint64_t *out_child_vcn)
{
    struct ntfs_index_entry ie;
    const uint8_t *entry;
    int cmp;

    *out_child_vcn = (uint64_t)-1;  /* Default: leaf, no descent */

    entry = ntfs_index_entry_first(entries_base, nh, &ie);
    while (entry) {
        /* Sentinel (LAST) entry: no filename to compare */
        if (ie.flags & NTFS_INDEX_ENTRY_LAST) {
            if (ie.flags & NTFS_INDEX_ENTRY_SUBNODE)
                *out_child_vcn = ie.child_vcn;
            return NTFS_ERR_NOT_FOUND;
        }

        /* Compare search name against this entry's $FILE_NAME. */
        {
            const uint8_t *fn_data = entry + 0x10;
            uint8_t entry_nlen = fn_data[0x40];
            const uint8_t *entry_name = fn_data + 0x42;

            cmp = ntfs_name_cmp_i(name, name_len, entry_name, (int)entry_nlen);
        }

        if (cmp == 0) {
            *out_inode = ie.mft_inode;
            return NTFS_OK;
        }

        if (cmp < 0) {
            if (ie.flags & NTFS_INDEX_ENTRY_SUBNODE)
                *out_child_vcn = ie.child_vcn;
            return NTFS_ERR_NOT_FOUND;
        }

        entry = ntfs_index_entry_next(entry, entries_base, nh, &ie);
    }

    return NTFS_ERR_NOT_FOUND;
}

int ntfs_lookup(struct ntfs_volume *vol, uint64_t dir_inode,
               const char *name, uint64_t *out_inode)
{
    uintptr_t rec_phys = 0;
    uintptr_t indx_phys = 0;
    uint8_t *rec_buf = NULL;
    uint8_t *indx_buf = NULL;
    struct ntfs_mft_header hdr;
    struct ntfs_index_root_header root_hdr;
    struct ntfs_index_node_header node_hdr;
    const uint8_t *entries_base;
    struct ntfs_attr_header ia_ah;
    const uint8_t *ia_attr;
    struct ntfs_data_run ia_runs[NTFS_MAX_INDEX_RUNS];
    int ia_run_count = 0;
    uint32_t indx_size;
    uint64_t child_vcn;
    int name_len;
    int depth;
    int rc;

    if (!vol || !name || !out_inode)
        return NTFS_ERR_IO;

    /* Compute name length */
    name_len = 0;
    while (name[name_len])
        name_len++;
    if (name_len == 0)
        return NTFS_ERR_NOT_FOUND;

    /* Allocate MFT record buffer (1 page = 4096 bytes, frs_size <= 4096) */
    rec_phys = pmm_alloc_contiguous(1);
    if (!rec_phys)
        return NTFS_ERR_IO;
    rec_buf = (uint8_t *)(uintptr_t)rec_phys;

    /* Read the directory's MFT record */
    rc = ntfs_read_mft_record(vol, dir_inode, rec_buf, &hdr);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        return rc;
    }

    /* Verify this is a directory */
    if (!(hdr.flags & NTFS_MFT_FLAG_DIRECTORY)) {
        pmm_free_frame(rec_phys);
        return NTFS_ERR_NOT_FOUND;  /* Not a directory */
    }

    /* Parse $INDEX_ROOT */
    rc = ntfs_parse_index_root(rec_buf, &hdr, &root_hdr, &node_hdr,
                                &entries_base);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        return rc;
    }

    indx_size = root_hdr.index_record_size;
    if (indx_size == 0)
        indx_size = 4096;  /* Default INDX size */

    /* Search in root node first */
    rc = ntfs_search_index_entries(entries_base, &node_hdr, name, name_len,
                                    out_inode, &child_vcn);
    if (rc == NTFS_OK) {
        /* Found in root node */
        pmm_free_frame(rec_phys);
        return NTFS_OK;
    }

    /* If no sub-node to descend to, it's definitively not found */
    if (child_vcn == (uint64_t)-1) {
        pmm_free_frame(rec_phys);
        return NTFS_ERR_NOT_FOUND;
    }

    /* Need to descend into INDX buffers — get $INDEX_ALLOCATION data runs */
    ia_attr = ntfs_attr_find_named(rec_buf, &hdr,
                                    NTFS_ATTR_INDEX_ALLOCATION, "$I30", &ia_ah);
    if (!ia_attr || ia_ah.non_resident != 1) {
        pmm_free_frame(rec_phys);
        return NTFS_ERR_NOT_FOUND;
    }

    ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs,
                                          NTFS_MAX_INDEX_RUNS, NULL);
    if (ia_run_count <= 0) {
        pmm_free_frame(rec_phys);
        return NTFS_ERR_NOT_FOUND;
    }

    /* Allocate INDX buffer */
    {
        uint32_t indx_pages = (indx_size + 4095) / 4096;
        indx_phys = pmm_alloc_contiguous(indx_pages);
    }
    if (!indx_phys) {
        pmm_free_frame(rec_phys);
        return NTFS_ERR_IO;
    }
    indx_buf = (uint8_t *)(uintptr_t)indx_phys;

    /* Descend through INDX buffers */
    for (depth = 0; depth < NTFS_MAX_TREE_DEPTH; depth++) {
        struct ntfs_index_node_header indx_nh;
        const uint8_t *indx_entries;

        /* Read INDX at child_vcn */
        rc = ntfs_read_indx(vol, ia_runs, ia_run_count, child_vcn,
                             indx_size, indx_buf);
        if (rc != NTFS_OK)
            break;

        /* Parse entries from the INDX buffer */
        rc = ntfs_parse_indx_entries(indx_buf, &indx_nh, &indx_entries);
        if (rc != NTFS_OK)
            break;

        /* Search this node */
        rc = ntfs_search_index_entries(indx_entries, &indx_nh, name, name_len,
                                        out_inode, &child_vcn);
        if (rc == NTFS_OK) {
            /* Found! */
            pmm_free_frame(rec_phys);
            pmm_free_frame(indx_phys);
            return NTFS_OK;
        }

        /* If no sub-node to descend to, it's definitively not found */
        if (child_vcn == (uint64_t)-1)
            break;
    }

    pmm_free_frame(rec_phys);
    pmm_free_frame(indx_phys);
    return NTFS_ERR_NOT_FOUND;
}

int ntfs_resolve_path(struct ntfs_volume *vol, const char *path,
                      uint64_t *out_inode)
{
    uint64_t current_inode = NTFS_ROOT_INODE;
    char component[NTFS_MAX_NAME + 1];
    int ci;
    int rc;

    if (!vol || !path || !out_inode)
        return NTFS_ERR_IO;

    /* Skip leading separator(s) */
    while (*path == '\\' || *path == '/')
        path++;

    /* Empty path after stripping → root directory itself */
    if (*path == '\0') {
        *out_inode = NTFS_ROOT_INODE;
        return NTFS_OK;
    }

    while (*path) {
        /* Extract next path component */
        ci = 0;
        while (*path && *path != '\\' && *path != '/' && ci < NTFS_MAX_NAME) {
            component[ci++] = *path++;
        }
        component[ci] = '\0';

        /* Skip separator(s) between components */
        while (*path == '\\' || *path == '/')
            path++;

        /* Skip empty components (e.g., double backslash) */
        if (ci == 0)
            continue;

        /* Look up this component in the current directory */
        rc = ntfs_lookup(vol, current_inode, component, &current_inode);
        if (rc != NTFS_OK)
            return rc;
    }

    *out_inode = current_inode;
    return NTFS_OK;
}

/* ============================================================================
 * Directory Enumeration — §5.4
 * ============================================================================ */

/* Helper: fill ntfs_dir_entry from a parsed ntfs_index_entry */
static void fill_dir_entry(struct ntfs_dir_entry *de,
                            const struct ntfs_index_entry *ie)
{
    int i;

    /* Copy filename */
    for (i = 0; i < NTFS_MAX_NAME && ie->fn.name[i]; i++)
        de->name[i] = ie->fn.name[i];
    de->name[i] = '\0';

    de->inode = ie->mft_inode;
    de->file_size = ie->fn.real_size;
    de->creation_time = ie->fn.creation_time;
    de->modification_time = ie->fn.modification_time;
    de->access_time = ie->fn.access_time;
    de->flags = ie->fn.flags;
    de->name_space = ie->fn.name_space;
    de->is_directory = (ie->fn.flags & 0x10000000) ? 1 : 0;
}

/* Walk entries in a single index node, invoking cb for each visible entry. */
static int walk_node_entries(const uint8_t *entries_base,
                              const struct ntfs_index_node_header *nh,
                              ntfs_readdir_cb cb, void *user_data)
{
    struct ntfs_index_entry ie;
    struct ntfs_dir_entry de;
    const uint8_t *entry;
    int cb_rc;

    entry = ntfs_index_entry_first(entries_base, nh, &ie);
    while (entry) {
        if (ie.flags & NTFS_INDEX_ENTRY_LAST)
            break;  /* Sentinel — no filename */

        /* Skip DOS-only namespace (0x02) */
        if (ie.fn.name_space != 0x02) {
            fill_dir_entry(&de, &ie);
            cb_rc = cb(&de, user_data);
            if (cb_rc != 0)
                return cb_rc;  /* Caller wants to stop */
        }

        entry = ntfs_index_entry_next(entry, entries_base, nh, &ie);
    }

    return NTFS_OK;
}

int ntfs_readdir(struct ntfs_volume *vol, uint64_t dir_inode,
                 ntfs_readdir_cb callback, void *user_data)
{
    uintptr_t rec_phys = 0;
    uint8_t *rec_buf = NULL;
    struct ntfs_mft_header hdr;
    struct ntfs_index_root_header root_hdr;
    struct ntfs_index_node_header node_hdr;
    const uint8_t *entries_base;
    int rc;

    if (!vol || !callback)
        return NTFS_ERR_IO;

    /* Allocate MFT record buffer */
    rec_phys = pmm_alloc_contiguous(1);
    if (!rec_phys)
        return NTFS_ERR_IO;
    rec_buf = (uint8_t *)(uintptr_t)rec_phys;

    /* Read and fixup directory's MFT record */
    rc = ntfs_read_mft_record(vol, dir_inode, rec_buf, &hdr);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        return rc;
    }

    if (!(hdr.flags & NTFS_MFT_FLAG_DIRECTORY)) {
        pmm_free_frame(rec_phys);
        return NTFS_ERR_NOT_FOUND;  /* Not a directory */
    }

    /* Parse $INDEX_ROOT */
    rc = ntfs_parse_index_root(rec_buf, &hdr, &root_hdr, &node_hdr,
                                &entries_base);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        return rc;
    }

    /* Walk entries in $INDEX_ROOT */
    rc = walk_node_entries(entries_base, &node_hdr, callback, user_data);
    if (rc != NTFS_OK && rc != NTFS_ERR_NOT_FOUND) {
        /* Callback signalled stop (positive rc) — not an error */
        pmm_free_frame(rec_phys);
        return NTFS_OK;
    }

    /* If index root has children, walk INDX buffers */
    if (node_hdr.flags & 0x01) {
        struct ntfs_attr_header ia_ah;
        const uint8_t *ia_attr;

        ia_attr = ntfs_attr_find_named(rec_buf, &hdr,
                                        NTFS_ATTR_INDEX_ALLOCATION,
                                        "$I30", &ia_ah);
        if (ia_attr && ia_ah.non_resident == 1) {
            struct ntfs_data_run ia_runs[64];
            struct ntfs_nonres_header ia_nrhdr;
            int ia_run_count;

            ia_run_count = ntfs_decode_data_runs(ia_attr, ia_runs, 64,
                                                  &ia_nrhdr);
            if (ia_run_count > 0) {
                uint32_t indx_size = root_hdr.index_record_size;
                uintptr_t indx_phys;
                uint32_t indx_pages;

                /* Read $BITMAP ($I30) to find active INDX records */
                struct ntfs_attr_header bm_ah;
                const uint8_t *bm_attr;
                const uint8_t *bitmap = NULL;
                uint32_t bitmap_len = 0;
                uintptr_t bm_phys = 0;

                if (indx_size == 0)
                    indx_size = 4096;

                bm_attr = ntfs_attr_find_named(rec_buf, &hdr,
                                                NTFS_ATTR_BITMAP,
                                                "$I30", &bm_ah);
                if (bm_attr) {
                    if (bm_ah.non_resident == 0) {
                        /* Resident bitmap — inline data */
                        bitmap = bm_attr + bm_ah.content_offset;
                        bitmap_len = bm_ah.content_length;
                    } else {
                        /* Non-resident bitmap — read from disk */
                        struct ntfs_data_run bm_runs[16];
                        struct ntfs_nonres_header bm_nrhdr;
                        int bm_run_count;

                        bm_run_count = ntfs_decode_data_runs(bm_attr, bm_runs,
                                                              16, &bm_nrhdr);
                        if (bm_run_count > 0 && bm_nrhdr.real_size > 0) {
                            uint32_t bm_pages;
                            bitmap_len = (uint32_t)bm_nrhdr.real_size;
                            bm_pages = (bitmap_len + 4095) / 4096;
                            bm_phys = pmm_alloc_contiguous(bm_pages);
                            if (bm_phys) {
                                uint8_t *bm_buf;
                                bm_buf = (uint8_t *)(uintptr_t)bm_phys;
                                /* Read bitmap clusters */
                                {
                                    int ri;
                                    uint32_t buf_off = 0;
                                    for (ri = 0; ri < bm_run_count &&
                                         buf_off < bitmap_len; ri++) {
                                        uint64_t byte_off =
                                            bm_runs[ri].lcn *
                                            vol->cluster_size;
                                        uint64_t byte_len =
                                            bm_runs[ri].length *
                                            vol->cluster_size;
                                        uint32_t to_read;
                                        if (byte_len > bitmap_len - buf_off)
                                            byte_len = bitmap_len - buf_off;
                                        to_read = (uint32_t)byte_len;
                                        if (blkdev_read(vol->dev,
                                                byte_off / vol->dev->sector_size,
                                                to_read / vol->dev->sector_size
                                                    + 1,
                                                bm_buf + buf_off) != 0) {
                                            pmm_free_frame(bm_phys);
                                            bm_phys = 0;
                                            bitmap = NULL;
                                            bitmap_len = 0;
                                            break;
                                        }
                                        buf_off += to_read;
                                    }
                                }
                                if (bm_phys)
                                    bitmap = (const uint8_t *)(uintptr_t)
                                             bm_phys;
                            }
                        }
                    }
                }

                /* Allocate INDX buffer */
                indx_pages = (indx_size + 4095) / 4096;
                indx_phys = pmm_alloc_contiguous(indx_pages);
                if (indx_phys) {
                    uint8_t *indx_buf = (uint8_t *)(uintptr_t)indx_phys;
                    uint64_t clusters_per_indx = indx_size / vol->cluster_size;
                    uint64_t total_bytes = ia_nrhdr.alloc_size;
                    uint64_t vcn;
                    uint32_t indx_index = 0;

                    if (clusters_per_indx == 0)
                        clusters_per_indx = 1;

                    for (vcn = 0; vcn * vol->cluster_size < total_bytes;
                         vcn += clusters_per_indx) {
                        struct ntfs_index_node_header indx_nh;
                        const uint8_t *indx_entries;

                        /* Check $BITMAP if available */
                        if (bitmap && bitmap_len > 0) {
                            uint32_t bit_idx = indx_index;
                            uint32_t byte_idx = bit_idx / 8;
                            uint8_t bit_mask = (uint8_t)(1 << (bit_idx % 8));

                            if (byte_idx < bitmap_len &&
                                !(bitmap[byte_idx] & bit_mask)) {
                                /* This INDX VCN is not in-use — skip */
                                indx_index++;
                                continue;
                            }
                        }

                        rc = ntfs_read_indx(vol, ia_runs, ia_run_count,
                                             vcn, indx_size, indx_buf);
                        if (rc != NTFS_OK) {
                            indx_index++;
                            continue;
                        }

                        rc = ntfs_parse_indx_entries(indx_buf, &indx_nh,
                                                      &indx_entries);
                        if (rc != NTFS_OK) {
                            indx_index++;
                            continue;
                        }

                        rc = walk_node_entries(indx_entries, &indx_nh,
                                               callback, user_data);
                        if (rc != NTFS_OK && rc != NTFS_ERR_NOT_FOUND) {
                            /* Callback signalled stop */
                            break;
                        }

                        indx_index++;
                    }

                    pmm_free_frame(indx_phys);
                }

                if (bm_phys)
                    pmm_free_frame(bm_phys);
            }
        }
    }

    pmm_free_frame(rec_phys);
    return NTFS_OK;
}
