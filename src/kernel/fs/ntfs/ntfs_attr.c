/* ============================================================================
 * ntfs_attr.c — Attribute Iterator & $ATTRIBUTE_LIST Handler
 *
 * Provides functions to walk the attribute sequence in MFT records,
 * parse attribute headers, and handle $ATTRIBUTE_LIST for records
 * that overflow into extension records.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ============================================================================
 * Attribute Iterator
 *
 * NTFS MFT records contain a sequence of variable-length attributes.
 * Each attribute has a common header (16+ bytes) followed by type-specific
 * data. The sequence ends with a 4-byte $END marker (0xFFFFFFFF).
 *
 * Layout per attribute:
 *   [type:4][length:4][non_res:1][name_len:1][name_off:2][flags:2][id:2]
 *   For resident:   [content_len:4][content_off:2][indexed:2]
 *   For non-resident: (run-list fields — decoded in §4)
 * ============================================================================ */

const uint8_t *ntfs_attr_first(const uint8_t *record,
                               const struct ntfs_mft_header *hdr)
{
    if (!record || !hdr)
        return NULL;

    /* attrs_offset must be at least past the fixed header (0x38 typical) */
    if (hdr->attrs_offset < 0x18 || hdr->attrs_offset >= hdr->used_size)
        return NULL;

    return record + hdr->attrs_offset;
}

const uint8_t *ntfs_attr_next(const uint8_t *attr, const uint8_t *record,
                              uint32_t used_size)
{
    uint32_t type;
    uint32_t length;
    uint32_t offset;

    if (!attr || !record)
        return NULL;

    type = ntfs_le32(attr + 0x00);
    if (type == NTFS_ATTR_END)
        return NULL;

    length = ntfs_le32(attr + 0x04);
    if (length == 0 || length > used_size)
        return NULL;  /* Prevent infinite loop on corrupt records */

    offset = (uint32_t)(attr - record) + length;
    if (offset + 4 > used_size)
        return NULL;  /* Next attr would be out of bounds */

    /* Check if next position is $END */
    type = ntfs_le32(record + offset);
    if (type == NTFS_ATTR_END)
        return NULL;

    return record + offset;
}

int ntfs_attr_parse(const uint8_t *attr, struct ntfs_attr_header *out)
{
    uint32_t type;

    if (!attr || !out)
        return NTFS_ERR_IO;

    type = ntfs_le32(attr + 0x00);
    if (type == NTFS_ATTR_END)
        return NTFS_ERR_BAD_MAGIC;

    out->type         = type;
    out->total_length = ntfs_le32(attr + 0x04);
    out->non_resident = attr[0x08];
    out->name_length  = attr[0x09];
    out->name_offset  = ntfs_le16(attr + 0x0A);
    out->flags        = ntfs_le16(attr + 0x0C);
    out->attr_id      = ntfs_le16(attr + 0x0E);
    out->raw          = attr;

    /* Parse resident-specific fields */
    if (out->non_resident == 0) {
        out->content_length = ntfs_le32(attr + 0x10);
        out->content_offset = ntfs_le16(attr + 0x14);
    } else {
        out->content_length = 0;
        out->content_offset = 0;
    }

    return NTFS_OK;
}

const uint8_t *ntfs_attr_find(const uint8_t *record,
                              const struct ntfs_mft_header *hdr,
                              uint32_t type_id,
                              struct ntfs_attr_header *out)
{
    const uint8_t *attr;

    attr = ntfs_attr_first(record, hdr);
    while (attr) {
        uint32_t type = ntfs_le32(attr + 0x00);
        if (type == NTFS_ATTR_END)
            break;

        if (type == type_id) {
            if (out)
                ntfs_attr_parse(attr, out);
            return attr;
        }

        attr = ntfs_attr_next(attr, record, hdr->used_size);
    }
    return NULL;
}

/* Compare ASCII kernel string against UTF-16LE attribute name.
 * NTFS attribute names are stored as UTF-16LE (2 bytes per char).
 * For standard names ($DATA, $I30, etc.) all chars are ASCII-range. */
static int ntfs_name_match(const uint8_t *utf16le_name, uint8_t name_len,
                           const char *ascii_name)
{
    uint8_t i;
    for (i = 0; i < name_len; i++) {
        uint16_t wc = ntfs_le16(utf16le_name + i * 2);
        uint8_t  ac = (uint8_t)ascii_name[i];
        if (ac == 0 || wc != (uint16_t)ac)
            return 0;
    }
    /* Both must end at the same length */
    return (ascii_name[name_len] == '\0') ? 1 : 0;
}

const uint8_t *ntfs_attr_find_named(const uint8_t *record,
                                    const struct ntfs_mft_header *hdr,
                                    uint32_t type_id,
                                    const char *name,
                                    struct ntfs_attr_header *out)
{
    const uint8_t *attr;

    if (!name)
        return ntfs_attr_find(record, hdr, type_id, out);

    attr = ntfs_attr_first(record, hdr);
    while (attr) {
        uint32_t type = ntfs_le32(attr + 0x00);
        if (type == NTFS_ATTR_END)
            break;

        if (type == type_id) {
            uint8_t  nlen = attr[0x09];
            uint16_t noff = ntfs_le16(attr + 0x0A);

            if (nlen > 0 && ntfs_name_match(attr + noff, nlen, name)) {
                if (out)
                    ntfs_attr_parse(attr, out);
                return attr;
            }
        }

        attr = ntfs_attr_next(attr, record, hdr->used_size);
    }
    return NULL;
}

/* ============================================================================
 * $ATTRIBUTE_LIST Handler — §3.4
 *
 * When a file's attributes overflow a single 1024-byte MFT record, NTFS
 * creates extension records. The base record contains a $ATTRIBUTE_LIST
 * (type 0x20) that maps every attribute to the MFT record that holds it.
 *
 * Entry layout (variable-length, walk by entry_length at 0x04):
 *   0x00  Attribute Type ID       (4 bytes, LE)
 *   0x04  Entry length            (2 bytes, LE)
 *   0x06  Name length             (1 byte, UTF-16 chars)
 *   0x07  Name offset             (1 byte, from entry start)
 *   0x08  Starting VCN            (8 bytes, LE) — for split non-res attrs
 *   0x10  MFT Reference           (8 bytes, LE: low 48 = inode, high 16 = seq)
 *   0x18  Attribute Instance ID   (2 bytes, LE)
 *
 * Minimum entry length: 0x1A (26 bytes).
 *
 * The $ATTRIBUTE_LIST itself can be resident (small) or non-resident (rare,
 * for files with many attributes spread across many extension records).
 * ============================================================================ */

/* Parse a single $ATTRIBUTE_LIST entry from raw bytes */
static void parse_attrlist_entry(const uint8_t *p,
                                  struct ntfs_attrlist_entry *out)
{
    out->type          = ntfs_le32(p + 0x00);
    out->entry_length  = ntfs_le16(p + 0x04);
    out->name_length   = p[0x06];
    out->name_offset   = p[0x07];
    out->start_vcn     = ntfs_le64(p + 0x08);
    out->mft_reference = ntfs_le64(p + 0x10);
    out->mft_inode     = out->mft_reference & 0x0000FFFFFFFFFFFF;
    out->attr_id       = ntfs_le16(p + 0x18);
}

/* Internal: search $ATTRIBUTE_LIST entries for a given type (and optional
 * name), read the extension MFT record, and return the attribute pointer.
 *
 * attrlist_data: pointer to $ATTRIBUTE_LIST content bytes.
 * attrlist_len: length of the content.
 * base_inode: inode of the base record (to skip self-referencing entries).
 * vol: volume context for reading extension records.
 * type_id: target attribute type.
 * name: optional ASCII name to match (NULL for unnamed).
 * out: filled with parsed attribute header on success.
 * ext_record: set to PMM-allocated extension record buffer. Caller frees.
 * ext_hdr: filled with extension record header.
 *
 * Returns raw pointer to the attribute within ext_record, or NULL. */
static const uint8_t *attrlist_search(const uint8_t *attrlist_data,
                                       uint32_t attrlist_len,
                                       uint64_t base_inode,
                                       struct ntfs_volume *vol,
                                       uint32_t type_id,
                                       const char *name,
                                       struct ntfs_attr_header *out,
                                       uintptr_t *ext_record,
                                       struct ntfs_mft_header *ext_hdr)
{
    uint32_t offset = 0;

    while (offset + 0x1A <= attrlist_len) {
        struct ntfs_attrlist_entry ale;
        parse_attrlist_entry(attrlist_data + offset, &ale);

        /* Sanity: entry_length must be >= 0x1A and not exceed remaining */
        if (ale.entry_length < 0x1A ||
            offset + ale.entry_length > attrlist_len)
            break;

        /* Match type */
        if (ale.type == type_id) {
            /* Match name if specified */
            int name_ok = 1;
            if (name && ale.name_length > 0) {
                const uint8_t *ename = attrlist_data + offset +
                                       ale.name_offset;
                name_ok = ntfs_name_match(ename, ale.name_length, name);
            } else if (name && ale.name_length == 0) {
                /* Caller wants named, entry is unnamed */
                name_ok = (name[0] == '\0') ? 1 : 0;
            } else if (!name && ale.name_length > 0) {
                /* Caller wants unnamed, entry is named — skip */
                name_ok = 0;
            }

            if (name_ok && ale.mft_inode != base_inode) {
                /* Attribute lives in an extension record — read it */
                uintptr_t rec_phys = pmm_alloc_contiguous(1);
                if (!rec_phys)
                    return NULL;

                {
                    uint8_t *rec_buf = (uint8_t *)(uintptr_t)rec_phys;
                    struct ntfs_mft_header ehdr;
                    const uint8_t *found;
                    int rc;

                    rc = ntfs_read_mft_record(vol, ale.mft_inode,
                                               rec_buf, &ehdr);
                    if (rc != NTFS_OK) {
                        pmm_free_frame(rec_phys);
                        goto next_entry;
                    }

                    rc = ntfs_apply_fixup(rec_buf, vol->frs_size,
                                           vol->bytes_per_sector);
                    if (rc != NTFS_OK) {
                        pmm_free_frame(rec_phys);
                        goto next_entry;
                    }

                    /* Search for the attribute in the extension record */
                    if (name)
                        found = ntfs_attr_find_named(rec_buf, &ehdr,
                                                      type_id, name, out);
                    else
                        found = ntfs_attr_find(rec_buf, &ehdr,
                                                type_id, out);

                    if (found) {
                        *ext_record = rec_phys;
                        if (ext_hdr)
                            *ext_hdr = ehdr;
                        return found;
                    }

                    pmm_free_frame(rec_phys);
                }
            }
        }

next_entry:
        offset += ale.entry_length;
    }

    return NULL;
}

/* Read $ATTRIBUTE_LIST content, handling both resident and non-resident.
 * Returns a pointer to the content data and sets *data_len.
 * For resident: returns a pointer into the record buffer (no alloc).
 * For non-resident: allocates via PMM and sets *alloc_phys (caller frees).
 * Returns NULL on failure. */
static const uint8_t *read_attrlist_content(struct ntfs_volume *vol,
                                             const uint8_t *al_attr,
                                             const struct ntfs_attr_header *al_ah,
                                             uint32_t *data_len,
                                             uintptr_t *alloc_phys)
{
    *alloc_phys = 0;

    if (al_ah->non_resident == 0) {
        /* Resident — data is inline */
        *data_len = al_ah->content_length;
        return al_attr + al_ah->content_offset;
    }

    /* Non-resident $ATTRIBUTE_LIST — decode data runs and read from disk */
    {
        struct ntfs_data_run runs[32];
        struct ntfs_nonres_header nrhdr;
        int run_count;
        uint32_t total_len;
        uint32_t pages;
        uintptr_t buf_phys;
        uint8_t *buf;
        int ri;
        uint32_t buf_off;

        run_count = ntfs_decode_data_runs(al_attr, runs, 32, &nrhdr);
        if (run_count <= 0 || nrhdr.real_size == 0)
            return NULL;

        total_len = (uint32_t)nrhdr.real_size;
        pages = (total_len + 4095) / 4096;
        buf_phys = pmm_alloc_contiguous(pages);
        if (!buf_phys)
            return NULL;

        buf = (uint8_t *)(uintptr_t)buf_phys;
        buf_off = 0;

        for (ri = 0; ri < run_count && buf_off < total_len; ri++) {
            uint64_t byte_off = runs[ri].lcn * vol->cluster_size;
            uint64_t byte_len = runs[ri].length * vol->cluster_size;
            uint32_t to_read;

            if (runs[ri].lcn == NTFS_LCN_SPARSE)
                continue;  /* Sparse run — skip */

            if (byte_len > total_len - buf_off)
                byte_len = total_len - buf_off;
            to_read = (uint32_t)byte_len;

            if (blkdev_read(vol->dev,
                    byte_off / vol->dev->sector_size,
                    to_read / vol->dev->sector_size + 1,
                    buf + buf_off) != 0) {
                pmm_free_frame(buf_phys);
                return NULL;
            }
            buf_off += to_read;
        }

        *data_len = total_len;
        *alloc_phys = buf_phys;
        return (const uint8_t *)(uintptr_t)buf_phys;
    }
}

/* ---- Public API: extended attribute search with $ATTRIBUTE_LIST ---- */

const uint8_t *ntfs_attr_find_ext(struct ntfs_volume *vol,
                                   const uint8_t *record,
                                   const struct ntfs_mft_header *hdr,
                                   uint32_t type_id,
                                   struct ntfs_attr_header *out,
                                   uintptr_t *ext_record,
                                   struct ntfs_mft_header *ext_hdr)
{
    const uint8_t *found;

    if (ext_record)
        *ext_record = 0;

    /* Try base record first */
    found = ntfs_attr_find(record, hdr, type_id, out);
    if (found)
        return found;

    /* Not in base record — check for $ATTRIBUTE_LIST */
    {
        struct ntfs_attr_header al_ah;
        const uint8_t *al_attr;

        al_attr = ntfs_attr_find(record, hdr,
                                  NTFS_ATTR_ATTRIBUTE_LIST, &al_ah);
        if (!al_attr)
            return NULL;  /* No $ATTRIBUTE_LIST — attribute doesn't exist */

        {
            const uint8_t *al_data;
            uint32_t al_len;
            uintptr_t al_phys;
            uint64_t base_inode;
            const uint8_t *result;

            al_data = read_attrlist_content(vol, al_attr, &al_ah,
                                             &al_len, &al_phys);
            if (!al_data)
                return NULL;

            /* Base inode from the MFT record header (for self-ref skip) */
            base_inode = ntfs_le32(record + 0x2C);  /* MFT record number */
            /* For NTFS 3.1+, the 48-bit reference is at 0x2C..0x31 */
            base_inode = ntfs_le32(record + 0x2C) |
                         ((uint64_t)ntfs_le16(record + 0x30) << 32);

            result = attrlist_search(al_data, al_len, base_inode, vol,
                                      type_id, NULL, out,
                                      ext_record, ext_hdr);

            if (al_phys)
                pmm_free_frame(al_phys);

            return result;
        }
    }
}

const uint8_t *ntfs_attr_find_named_ext(struct ntfs_volume *vol,
                                         const uint8_t *record,
                                         const struct ntfs_mft_header *hdr,
                                         uint32_t type_id,
                                         const char *name,
                                         struct ntfs_attr_header *out,
                                         uintptr_t *ext_record,
                                         struct ntfs_mft_header *ext_hdr)
{
    const uint8_t *found;

    if (ext_record)
        *ext_record = 0;

    /* Try base record first */
    found = ntfs_attr_find_named(record, hdr, type_id, name, out);
    if (found)
        return found;

    /* Not in base record — check for $ATTRIBUTE_LIST */
    {
        struct ntfs_attr_header al_ah;
        const uint8_t *al_attr;

        al_attr = ntfs_attr_find(record, hdr,
                                  NTFS_ATTR_ATTRIBUTE_LIST, &al_ah);
        if (!al_attr)
            return NULL;

        {
            const uint8_t *al_data;
            uint32_t al_len;
            uintptr_t al_phys;
            uint64_t base_inode;
            const uint8_t *result;

            al_data = read_attrlist_content(vol, al_attr, &al_ah,
                                             &al_len, &al_phys);
            if (!al_data)
                return NULL;

            base_inode = ntfs_le32(record + 0x2C) |
                         ((uint64_t)ntfs_le16(record + 0x30) << 32);

            result = attrlist_search(al_data, al_len, base_inode, vol,
                                      type_id, name, out,
                                      ext_record, ext_hdr);

            if (al_phys)
                pmm_free_frame(al_phys);

            return result;
        }
    }
}
