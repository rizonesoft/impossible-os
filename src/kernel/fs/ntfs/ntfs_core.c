/* ============================================================================
 * ntfs_core.c — NTFS Boot Sector / BPB Parsing
 *
 * Reads the first sector (LBA 0) of an NTFS partition, validates the
 * OEM ID ("NTFS    "), extracts all critical BPB fields, and locates
 * the $MFT.  This is the foundation for all subsequent NTFS operations.
 *
 * Reference: NTFS Documentation (unofficial), Microsoft NTFS.sys behavior.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ---- Little-endian field readers ---- */

static uint16_t ntfs_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ntfs_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t ntfs_le64(const uint8_t *p)
{
    return (uint64_t)ntfs_le32(p) | ((uint64_t)ntfs_le32(p + 4) << 32);
}

/* ---- String comparison (8 bytes, no libc) ---- */

static int ntfs_memcmp(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

/* ---- Probe: check if sector 0 has NTFS OEM ID ---- */

/* Expected OEM ID at offset 0x03: "NTFS    " (4 letters + 4 spaces) */
static const uint8_t ntfs_oem_id[8] = { 'N','T','F','S',' ',' ',' ',' ' };

int ntfs_probe(const uint8_t *sector)
{
    /* Check OEM ID at offset 0x03 */
    if (ntfs_memcmp(sector + 0x03, ntfs_oem_id, 8) != 0)
        return 0;

    /* Validate boot signature 0x55AA at offset 0x1FE */
    if (sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA)
        return 0;

    return 1;
}

/* ---- Decode the Clusters-Per-FRS / Clusters-Per-Index quirk ----
 * If the 1-byte signed value at offset 0x40 (or 0x44) is positive,
 * the record size = value × cluster_size.
 * If negative, the record size = 2^|value| (e.g., 0xF6 = −10 → 1024). */

static uint32_t decode_record_size(int8_t raw, uint32_t cluster_size)
{
    if (raw > 0) {
        return (uint32_t)raw * cluster_size;
    }
    /* Negative: 2^|raw| */
    int shift = -raw;
    if (shift > 31)
        shift = 31; /* Sanity cap */
    return (uint32_t)1 << shift;
}

/* ---- Public API ---- */

struct ntfs_volume *ntfs_init(const struct blkdev *dev)
{
    uint8_t sector[512];
    struct ntfs_volume *vol;
    uint16_t bps;
    uint8_t  spc;
    int8_t   clusters_per_frs_raw;
    int8_t   clusters_per_index_raw;

    if (!dev) {
        klog(LOG_DEBUG, "ntfs", "ntfs_init: NULL device");
        return NULL;
    }

    /* Read sector 0 (boot sector) */
    if (blkdev_read(dev, 0, 1, sector) != 0) {
        klog(LOG_DEBUG, "ntfs", "Failed to read boot sector");
        return NULL;
    }

    /* Validate OEM ID at offset 0x03 */
    if (ntfs_memcmp(sector + 0x03, ntfs_oem_id, 8) != 0) {
        klog(LOG_DEBUG, "ntfs", "OEM ID mismatch (not NTFS)");
        return NULL;
    }

    /* Validate boot signature */
    if (sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA) {
        klog(LOG_DEBUG, "ntfs", "Invalid boot signature");
        return NULL;
    }

    /* Extract BPB fields */
    bps = ntfs_le16(sector + 0x0B);
    spc = sector[0x0D];

    /* Sanity checks */
    if (bps == 0 || (bps & (bps - 1)) != 0) {
        klog(LOG_DEBUG, "ntfs", "Invalid bytes_per_sector: %u", (uint64_t)bps);
        return NULL;
    }
    if (spc == 0 || (spc & (spc - 1)) != 0) {
        klog(LOG_DEBUG, "ntfs", "Invalid sectors_per_cluster: %u", (uint64_t)spc);
        return NULL;
    }

    /* Allocate volume context (< 100 bytes — safe for kmalloc) */
    vol = (struct ntfs_volume *)kmalloc(sizeof(struct ntfs_volume));
    if (!vol) {
        klog(LOG_DEBUG, "ntfs", "Failed to allocate ntfs_volume");
        return NULL;
    }

    vol->dev = dev;
    vol->bytes_per_sector = bps;
    vol->sectors_per_cluster = spc;
    vol->cluster_size = (uint32_t)bps * (uint32_t)spc;

    /* Total sectors (64-bit at offset 0x28) */
    vol->total_sectors = ntfs_le64(sector + 0x28);

    /* MFT location */
    vol->mft_lcn     = ntfs_le64(sector + 0x30);
    vol->mftmirr_lcn = ntfs_le64(sector + 0x38);

    /* File Record Segment size (offset 0x40, signed 1-byte) */
    clusters_per_frs_raw = (int8_t)sector[0x40];
    vol->frs_size = decode_record_size(clusters_per_frs_raw, vol->cluster_size);

    /* Index record size (offset 0x44, same signed encoding) */
    clusters_per_index_raw = (int8_t)sector[0x44];
    vol->index_size = decode_record_size(clusters_per_index_raw, vol->cluster_size);

    /* Volume serial number (offset 0x48, 8 bytes LE) */
    vol->volume_serial = ntfs_le64(sector + 0x48);

    /* MFT byte offset = LCN × cluster_size */
    vol->mft_byte_offset = vol->mft_lcn * (uint64_t)vol->cluster_size;

    /* Log volume information */
    klog(LOG_INFO, "ntfs",
         "Volume: %u sectors, cluster=%u bytes, MFT at LCN %u (byte 0x%x)",
         vol->total_sectors, (uint64_t)vol->cluster_size,
         vol->mft_lcn, vol->mft_byte_offset);

    klog(LOG_INFO, "ntfs",
         "FRS size=%u bytes, INDX size=%u bytes, serial=0x%x",
         (uint64_t)vol->frs_size, (uint64_t)vol->index_size,
         vol->volume_serial);

    return vol;
}

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

/* ============================================================================
 * $FILE_NAME Decoder — attribute type 0x30
 *
 * Every MFT record has at least one $FILE_NAME attribute. Records with both
 * a long name and a DOS 8.3 name have TWO. The attribute content layout:
 *
 *   0x00  Parent directory MFT reference (8 bytes: low 6 = inode, high 2 = seq)
 *   0x08  Creation time (8 bytes, FILETIME)
 *   0x10  Modification time (8 bytes)
 *   0x18  MFT change time (8 bytes)
 *   0x20  Access time (8 bytes)
 *   0x28  Allocated size (8 bytes)
 *   0x30  Real size (8 bytes)
 *   0x38  Flags (4 bytes)
 *   0x3C  Reparse value / EA size (4 bytes)
 *   0x40  Filename length (1 byte, in UTF-16 chars)
 *   0x41  Namespace (1 byte: 0=POSIX, 1=Win32, 2=DOS, 3=Win32/DOS)
 *   0x42  Filename (name_length × 2 bytes, UTF-16LE, NOT null-terminated)
 * ============================================================================ */

/* Parse a single $FILE_NAME attribute's content into out. */
static void parse_fn_content(const uint8_t *data, uint32_t data_len,
                             struct ntfs_file_name *out)
{
    uint64_t parent_ref;
    uint8_t  nlen;
    uint8_t  i;

    if (data_len < 0x42)
        return;

    /* Parent directory reference: low 48 bits = inode, high 16 = seq */
    parent_ref = ntfs_le64(data + 0x00);
    out->parent_inode = parent_ref & 0x0000FFFFFFFFFFFFULL;
    out->parent_seq   = (uint16_t)((parent_ref >> 48) & 0xFFFF);

    /* Duplicated timestamps */
    out->creation_time     = ntfs_le64(data + 0x08);
    out->modification_time = ntfs_le64(data + 0x10);
    out->mft_change_time   = ntfs_le64(data + 0x18);
    out->access_time       = ntfs_le64(data + 0x20);

    /* Sizes and flags */
    out->allocated_size = ntfs_le64(data + 0x28);
    out->real_size      = ntfs_le64(data + 0x30);
    out->flags          = ntfs_le32(data + 0x38);

    /* Filename metadata */
    out->name_length = data[0x40];
    out->name_space  = data[0x41];

    /* Decode UTF-16LE filename to ASCII (lossy for non-ASCII chars) */
    nlen = out->name_length;
    if (nlen > NTFS_MAX_NAME)
        nlen = NTFS_MAX_NAME;

    /* Verify we have enough data for the filename */
    if (data_len < (uint32_t)(0x42 + nlen * 2))
        nlen = (uint8_t)((data_len - 0x42) / 2);

    for (i = 0; i < nlen; i++) {
        uint16_t wc = ntfs_le16(data + 0x42 + i * 2);
        /* Lossy conversion: non-ASCII chars become '?' */
        out->name[i] = (wc < 0x80) ? (char)wc : '?';
    }
    out->name[nlen] = '\0';
}

int ntfs_decode_file_name(const uint8_t *record,
                          const struct ntfs_mft_header *hdr,
                          struct ntfs_file_name *out)
{
    const uint8_t *attr;
    int found = 0;
    int best_ns = -1;  /* Track best namespace found so far */

    if (!record || !hdr || !out)
        return NTFS_ERR_IO;

    /* Zero output */
    out->parent_inode = 0;
    out->parent_seq   = 0;
    out->name_length  = 0;
    out->name_space   = 0;
    out->name[0]      = '\0';

    /* Iterate all attributes looking for $FILE_NAME (0x30) */
    attr = ntfs_attr_first(record, hdr);
    while (attr) {
        uint32_t type = ntfs_le32(attr + 0x00);
        if (type == NTFS_ATTR_END)
            break;

        if (type == NTFS_ATTR_FILE_NAME) {
            uint8_t non_res = attr[0x08];

            /* $FILE_NAME is always resident */
            if (non_res == 0) {
                uint32_t clen = ntfs_le32(attr + 0x10);
                uint16_t coff = ntfs_le16(attr + 0x14);
                const uint8_t *data = attr + coff;
                uint8_t ns;

                if (clen >= 0x42) {
                    ns = data[0x41];

                    /* Namespace priority: Win32 (1) or Win32/DOS (3) > POSIX (0) > DOS (2)
                     * We want to end up with the best display name. */
                    int priority;
                    if (ns == NTFS_NS_WIN32 || ns == NTFS_NS_WIN32DOS)
                        priority = 3;  /* Best */
                    else if (ns == NTFS_NS_POSIX)
                        priority = 2;
                    else /* DOS 8.3 */
                        priority = 1;

                    if (priority > best_ns) {
                        parse_fn_content(data, clen, out);
                        best_ns = priority;
                        found = 1;
                    }
                }
            }
        }

        attr = ntfs_attr_next(attr, record, hdr->used_size);
    }

    return found ? NTFS_OK : NTFS_ERR_BAD_MAGIC;
}

/* ============================================================================
 * Data Run Decoder — §4.1
 *
 * Non-resident attributes store their cluster mappings as a "run list":
 * a sequence of variable-length encoded (length, offset) pairs.
 *
 * Each run starts with a header byte:
 *   low nibble  (L) = number of bytes for the run length (unsigned)
 *   high nibble (F) = number of bytes for the run offset (signed, relative)
 *
 * Header byte 0x00 terminates the list.
 *
 * The offset is RELATIVE to the previous run's LCN (or LCN 0 for first run).
 * It is stored as a signed integer and must be sign-extended.
 *
 * Sparse runs have F=0 (no offset field) — they read as all zeros.
 *
 * Example: header 0x31 → L=1 byte length, F=3 byte offset
 *   Read 1 byte unsigned → cluster count
 *   Read 3 bytes signed  → relative offset from previous LCN
 * ============================================================================ */

/* Read N bytes as an unsigned integer (little-endian, N ≤ 8) */
static uint64_t read_unsigned(const uint8_t *p, int n)
{
    uint64_t val = 0;
    int i;
    for (i = 0; i < n && i < 8; i++)
        val |= (uint64_t)p[i] << (i * 8);
    return val;
}

/* Read N bytes as a signed integer (little-endian, sign-extended, N ≤ 8) */
static int64_t read_signed(const uint8_t *p, int n)
{
    uint64_t val = 0;
    int i;
    for (i = 0; i < n && i < 8; i++)
        val |= (uint64_t)p[i] << (i * 8);

    /* Sign-extend: if the high bit of the last byte is set, fill upper bits */
    if (n > 0 && n < 8 && (p[n - 1] & 0x80)) {
        uint64_t mask = ~((1ULL << (n * 8)) - 1);
        val |= mask;
    }
    return (int64_t)val;
}

int ntfs_decode_data_runs(const uint8_t *attr, struct ntfs_data_run *runs,
                          int max_runs, struct ntfs_nonres_header *nrhdr)
{
    const uint8_t *p;
    uint16_t run_off;
    uint64_t vcn;
    int64_t  prev_lcn = 0;
    int      count = 0;
    uint8_t  header;

    if (!attr || !runs || max_runs <= 0)
        return -1;

    /* Verify this is a non-resident attribute */
    if (attr[0x08] != 1)
        return -1;

    /* Parse non-resident header fields */
    run_off = ntfs_le16(attr + 0x20);

    if (nrhdr) {
        nrhdr->start_vcn    = ntfs_le64(attr + 0x10);
        nrhdr->last_vcn     = ntfs_le64(attr + 0x18);
        nrhdr->data_run_off = run_off;
        nrhdr->alloc_size   = ntfs_le64(attr + 0x28);
        nrhdr->real_size    = ntfs_le64(attr + 0x30);
        nrhdr->init_size    = ntfs_le64(attr + 0x38);
    }

    /* Starting VCN for run tracking */
    vcn = ntfs_le64(attr + 0x10);

    /* Point to the start of the run-list data */
    p = attr + run_off;

    /* Walk the run-list */
    while (count < max_runs) {
        int len_size;
        int off_size;
        uint64_t run_length;
        int64_t  run_offset;

        header = *p;
        if (header == 0x00)
            break;  /* End of run-list */

        len_size = header & 0x0F;
        off_size = (header >> 4) & 0x0F;

        if (len_size == 0 || len_size > 8 || off_size > 8)
            break;  /* Corrupt run-list */

        p++;  /* Advance past header byte */

        /* Read run length (unsigned) */
        run_length = read_unsigned(p, len_size);
        p += len_size;

        /* Read run offset (signed, relative) — or sparse if off_size == 0 */
        if (off_size > 0) {
            run_offset = read_signed(p, off_size);
            p += off_size;

            prev_lcn += run_offset;
            runs[count].lcn = (uint64_t)prev_lcn;
        } else {
            /* Sparse run — no disk location, reads as zeros */
            runs[count].lcn = NTFS_LCN_SPARSE;
        }

        runs[count].vcn_start = vcn;
        runs[count].length    = run_length;

        vcn += run_length;
        count++;
    }

    return count;
}

/* ============================================================================
 * $STANDARD_INFORMATION Decoder — attribute type 0x10
 *
 * Every MFT record has exactly one $STANDARD_INFORMATION attribute.
 * It is always resident (content is small — 48 or 72 bytes).
 *
 * Content layout:
 *   0x00  Creation time         (8 bytes, FILETIME)
 *   0x08  Modification time     (8 bytes, FILETIME)
 *   0x10  MFT change time       (8 bytes, FILETIME)
 *   0x18  Last access time      (8 bytes, FILETIME)
 *   0x20  DOS permissions/flags (4 bytes)
 *   (0x24+ : Max versions, version number, class ID — NTFS 3.0+)
 *   (0x30+ : Owner ID, Security ID, Quota, USN — NTFS 3.0+)
 *
 * FILETIME = 100-nanosecond intervals since January 1, 1601 00:00:00 UTC.
 * To convert to Unix: subtract 11644473600 seconds, divide by 10,000,000.
 * ============================================================================ */

/* Delta between Windows (1601-01-01) and Unix (1970-01-01) epochs in seconds */
#define NTFS_EPOCH_DELTA  11644473600ULL

/* 100-nanosecond intervals per second */
#define NTFS_TICKS_PER_SEC  10000000ULL

uint64_t ntfs_filetime_to_unix(uint64_t filetime)
{
    uint64_t secs;

    if (filetime == 0)
        return 0;

    /* Convert 100-ns ticks to seconds */
    secs = filetime / NTFS_TICKS_PER_SEC;

    /* Subtract epoch delta (1601 → 1970) */
    if (secs <= NTFS_EPOCH_DELTA)
        return 0;  /* Before Unix epoch */

    return secs - NTFS_EPOCH_DELTA;
}

int ntfs_decode_std_info(const uint8_t *record,
                         const struct ntfs_mft_header *hdr,
                         struct ntfs_std_info *out)
{
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    const uint8_t *data;

    if (!record || !hdr || !out)
        return NTFS_ERR_IO;

    /* Find the $STANDARD_INFORMATION attribute */
    attr = ntfs_attr_find(record, hdr, NTFS_ATTR_STANDARD_INFORMATION, &ah);
    if (!attr)
        return NTFS_ERR_BAD_MAGIC;

    /* Must be resident */
    if (ah.non_resident != 0)
        return NTFS_ERR_BAD_MAGIC;

    /* Need at least 0x24 bytes (4 timestamps + flags) */
    if (ah.content_length < 0x24)
        return NTFS_ERR_BAD_MAGIC;

    data = attr + ah.content_offset;

    /* Extract raw FILETIME timestamps */
    out->creation_time     = ntfs_le64(data + 0x00);
    out->modification_time = ntfs_le64(data + 0x08);
    out->mft_change_time   = ntfs_le64(data + 0x10);
    out->access_time       = ntfs_le64(data + 0x18);

    /* Convert to Unix timestamps */
    out->creation_unix     = ntfs_filetime_to_unix(out->creation_time);
    out->modification_unix = ntfs_filetime_to_unix(out->modification_time);
    out->mft_change_unix   = ntfs_filetime_to_unix(out->mft_change_time);
    out->access_unix       = ntfs_filetime_to_unix(out->access_time);

    /* DOS permission flags */
    out->dos_permissions = ntfs_le32(data + 0x20);

    /* NTFS 3.0+ extended fields (at least 0x48 bytes: 0x24..0x47) */
    out->has_extended = 0;
    out->max_versions = 0;
    out->version_number = 0;
    out->class_id = 0;
    out->owner_id = 0;
    out->security_id = 0;
    out->quota_charged = 0;
    out->usn = 0;

    if (ah.content_length >= 0x48) {
        out->has_extended = 1;
        out->max_versions   = ntfs_le32(data + 0x24);
        out->version_number = ntfs_le32(data + 0x28);
        out->class_id       = ntfs_le32(data + 0x2C);
        out->owner_id       = ntfs_le32(data + 0x30);
        out->security_id    = ntfs_le32(data + 0x34);
        out->quota_charged  = ntfs_le64(data + 0x38);
        out->usn            = ntfs_le64(data + 0x40);
    }

    return NTFS_OK;
}

/* ============================================================================
 * $SECURITY_DESCRIPTOR Parser — §3.5 (attribute type 0x50)
 *
 * NTFS stores security descriptors in self-relative format:
 *   0x00  Revision (1 byte, must be 1)
 *   0x01  Sbz1 (1 byte, reserved)
 *   0x02  Control flags (2 bytes, LE)
 *   0x04  Owner SID offset (4 bytes, LE, from descriptor start)
 *   0x08  Group SID offset (4 bytes, LE)
 *   0x0C  SACL offset (4 bytes, LE, 0 if absent)
 *   0x10  DACL offset (4 bytes, LE, 0 if absent)
 *
 * SID format:
 *   0x00  Revision (1 byte)
 *   0x01  Sub-authority count (1 byte)
 *   0x02  Identifier authority (6 bytes, big-endian)
 *   0x08  Sub-authorities (4 bytes × count, LE)
 *
 * ACL format:
 *   0x00  Revision (1 byte)
 *   0x01  Sbz1 (1 byte)
 *   0x02  ACL size (2 bytes, LE)
 *   0x04  ACE count (2 bytes, LE)
 *   0x06  Sbz2 (2 bytes)
 *   0x08  ACEs...
 *
 * ACE format (ACCESS_ALLOWED/DENIED_ACE):
 *   0x00  Type (1 byte)
 *   0x01  Flags (1 byte)
 *   0x02  Size (2 bytes, LE)
 *   0x04  Access mask (4 bytes, LE)
 *   0x08  SID (variable)
 * ============================================================================ */

/* Parse a SID from raw bytes. Returns bytes consumed, or 0 on error. */
static uint32_t parse_sid(const uint8_t *data, uint32_t max_len,
                           struct ntfs_sid *out)
{
    uint8_t i;
    uint32_t sid_size;

    if (max_len < 8)
        return 0;

    out->revision = data[0x00];
    out->sub_auth_count = data[0x01];

    if (out->revision != 1)
        return 0;

    if (out->sub_auth_count > NTFS_SID_MAX_SUB_AUTH)
        return 0;

    sid_size = 8 + (uint32_t)out->sub_auth_count * 4;
    if (sid_size > max_len)
        return 0;

    /* 6-byte big-endian identifier authority */
    for (i = 0; i < 6; i++)
        out->authority[i] = data[0x02 + i];

    /* Decode as 48-bit big-endian integer */
    out->authority_value = ((uint64_t)data[0x02] << 40) |
                           ((uint64_t)data[0x03] << 32) |
                           ((uint64_t)data[0x04] << 24) |
                           ((uint64_t)data[0x05] << 16) |
                           ((uint64_t)data[0x06] << 8) |
                           ((uint64_t)data[0x07]);

    /* Sub-authorities (little-endian 32-bit each) */
    for (i = 0; i < out->sub_auth_count; i++)
        out->sub_authorities[i] = ntfs_le32(data + 0x08 + i * 4);

    return sid_size;
}

/* Parse an ACL (DACL or SACL) from raw bytes. */
static int parse_acl(const uint8_t *data, uint32_t max_len,
                      struct ntfs_acl *out)
{
    uint16_t i;
    uint32_t offset;

    if (max_len < 8)
        return NTFS_ERR_BAD_MAGIC;

    out->revision = data[0x00];
    out->size     = ntfs_le16(data + 0x02);
    out->ace_count = ntfs_le16(data + 0x04);
    out->parsed_count = 0;

    if (out->size > max_len)
        return NTFS_ERR_BAD_MAGIC;

    /* Walk ACEs */
    offset = 8;  /* ACL header is 8 bytes */
    for (i = 0; i < out->ace_count && out->parsed_count < NTFS_ACL_MAX_ACES;
         i++) {
        struct ntfs_ace *ace = &out->aces[out->parsed_count];
        uint16_t ace_size;
        uint32_t sid_offset;
        uint32_t sid_max;

        if (offset + 4 > out->size)
            break;

        ace->type  = data[offset + 0x00];
        ace->flags = data[offset + 0x01];
        ace->size  = ntfs_le16(data + offset + 0x02);
        ace_size = ace->size;

        if (ace_size < 8 || offset + ace_size > out->size)
            break;

        /* ACCESS_ALLOWED_ACE and ACCESS_DENIED_ACE share the same layout */
        if (ace->type <= NTFS_ACE_SYSTEM_ALARM) {
            ace->access_mask = ntfs_le32(data + offset + 0x04);

            /* SID starts at offset 0x08 within the ACE */
            sid_offset = offset + 0x08;
            sid_max = (offset + ace_size > out->size) ?
                      0 : (offset + ace_size - sid_offset);

            if (sid_max >= 8) {
                if (parse_sid(data + sid_offset, sid_max, &ace->sid) > 0)
                    out->parsed_count++;
            }
        }

        offset += ace_size;
    }

    return NTFS_OK;
}

int ntfs_parse_security_desc(const uint8_t *data, uint32_t data_len,
                              struct ntfs_security_desc *out)
{
    uint32_t owner_off, group_off, sacl_off, dacl_off;

    if (!data || !out || data_len < 0x14)
        return NTFS_ERR_BAD_MAGIC;

    /* Zero-init */
    out->has_owner = 0;
    out->has_group = 0;
    out->has_dacl = 0;
    out->has_sacl = 0;

    out->revision = data[0x00];
    if (out->revision != 1)
        return NTFS_ERR_BAD_MAGIC;

    out->control = ntfs_le16(data + 0x02);

    owner_off = ntfs_le32(data + 0x04);
    group_off = ntfs_le32(data + 0x08);
    sacl_off  = ntfs_le32(data + 0x0C);
    dacl_off  = ntfs_le32(data + 0x10);

    /* Must be self-relative */
    if (!(out->control & NTFS_SD_SELF_RELATIVE))
        return NTFS_ERR_BAD_MAGIC;

    /* Parse Owner SID */
    if (owner_off != 0 && owner_off + 8 <= data_len) {
        if (parse_sid(data + owner_off, data_len - owner_off,
                       &out->owner) > 0)
            out->has_owner = 1;
    }

    /* Parse Group SID */
    if (group_off != 0 && group_off + 8 <= data_len) {
        if (parse_sid(data + group_off, data_len - group_off,
                       &out->group) > 0)
            out->has_group = 1;
    }

    /* Parse SACL */
    if ((out->control & NTFS_SD_SACL_PRESENT) &&
        sacl_off != 0 && sacl_off + 8 <= data_len) {
        if (parse_acl(data + sacl_off, data_len - sacl_off,
                       &out->sacl) == NTFS_OK)
            out->has_sacl = 1;
    }

    /* Parse DACL */
    if ((out->control & NTFS_SD_DACL_PRESENT) &&
        dacl_off != 0 && dacl_off + 8 <= data_len) {
        if (parse_acl(data + dacl_off, data_len - dacl_off,
                       &out->dacl) == NTFS_OK)
            out->has_dacl = 1;
    }

    return NTFS_OK;
}

int ntfs_decode_security(const uint8_t *record,
                          const struct ntfs_mft_header *hdr,
                          struct ntfs_volume *vol,
                          struct ntfs_security_desc *out)
{
    struct ntfs_attr_header ah;
    const uint8_t *attr;

    if (!record || !hdr || !out)
        return NTFS_ERR_IO;

    /* Try inline $SECURITY_DESCRIPTOR (type 0x50) first */
    attr = ntfs_attr_find(record, hdr,
                           NTFS_ATTR_SECURITY_DESCRIPTOR, &ah);
    if (attr && ah.non_resident == 0 && ah.content_length >= 0x14) {
        const uint8_t *sd_data = attr + ah.content_offset;
        return ntfs_parse_security_desc(sd_data, ah.content_length, out);
    }

    /* Inline descriptor not found — try $Secure via security_id */
    if (vol) {
        struct ntfs_std_info si;
        int rc = ntfs_decode_std_info(record, hdr, &si);
        if (rc == NTFS_OK && si.has_extended && si.security_id != 0) {
            /* TODO: Look up security_id in $Secure (inode 9) $SII/$SDS.
             * This requires reading $Secure's $INDEX_ROOT ($SII stream)
             * and finding the matching security_id entry, which points
             * to an offset in the $SDS data stream where the descriptor
             * is stored. For now, log and return "not found". */
            klog(LOG_DEBUG, "ntfs",
                 "Security ID %u found (needs $Secure lookup)",
                 (uint64_t)si.security_id);
            return NTFS_ERR_NOT_FOUND;
        }
    }

    return NTFS_ERR_NOT_FOUND;
}

/* Format a SID as "S-1-5-21-123456-789012-..." string */

/* Write a decimal integer into buf. Returns chars written. */
static int uint_to_str(uint64_t val, char *buf, int buf_len)
{
    char tmp[20];
    int len = 0;
    int i;

    if (buf_len <= 0)
        return 0;

    /* Special case: zero */
    if (val == 0) {
        if (buf_len >= 2) {
            buf[0] = '0';
            buf[1] = '\0';
            return 1;
        }
        return 0;
    }

    /* Build digits in reverse */
    while (val > 0 && len < 20) {
        tmp[len++] = '0' + (char)(val % 10);
        val /= 10;
    }

    if (len >= buf_len)
        len = buf_len - 1;

    /* Reverse into output buffer */
    for (i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';

    return len;
}

/* Append a character to buf at position pos.  Returns new pos. */
static int sid_append_char(char *buf, int buf_len, int pos, char c)
{
    if (pos < buf_len - 1) {
        buf[pos] = c;
        buf[pos + 1] = '\0';
        return pos + 1;
    }
    return pos;
}

int ntfs_format_sid(const struct ntfs_sid *sid, char *buf, int buf_len)
{
    int pos = 0;
    uint8_t i;

    if (!sid || !buf || buf_len < 8)
        return 0;

    buf[0] = '\0';

    /* "S-" prefix */
    pos = sid_append_char(buf, buf_len, pos, 'S');
    pos = sid_append_char(buf, buf_len, pos, '-');

    /* Revision */
    pos += uint_to_str((uint64_t)sid->revision, buf + pos, buf_len - pos);

    /* "-{authority}" */
    pos = sid_append_char(buf, buf_len, pos, '-');
    pos += uint_to_str(sid->authority_value, buf + pos, buf_len - pos);

    /* "-{sub1}-{sub2}-..." */
    for (i = 0; i < sid->sub_auth_count && pos < buf_len - 2; i++) {
        pos = sid_append_char(buf, buf_len, pos, '-');
        pos += uint_to_str((uint64_t)sid->sub_authorities[i],
                           buf + pos, buf_len - pos);
    }

    return pos;
}

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

/* Local helpers — no stdlib available in freestanding kernel */
static void ntfs_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < n; i++)
        d[i] = val;
}

static void ntfs_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

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

        return ntfs_read_data(vol, runs, run_count, nrhdr.real_size,
                              file_offset, length, buffer);
    }
}

/* ============================================================================
 * $INDEX_ROOT Parser — §5.1 (attribute type 0x90)
 *
 * Directories in NTFS store filenames in a B+ tree. The root node of the
 * tree lives in the always-resident $INDEX_ROOT attribute (named "$I30").
 *
 * $INDEX_ROOT content layout:
 *   0x00  Index Root Header (16 bytes):
 *         0x00  Indexed attribute type (4B) — always 0x30 ($FILE_NAME)
 *         0x04  Collation rule (4B) — 0x01 = filename collation
 *         0x08  Index record size (4B) — typically 4096
 *         0x0C  Clusters per index record (1B)
 *   0x10  Node Header (16 bytes):
 *         0x00  Offset to first entry (from node header start, 4B)
 *         0x04  Total size of entries (4B)
 *         0x08  Allocated size of entries (4B)
 *         0x0C  Flags (1B) — 0x01 = has children
 *   0x20+ Index Entries (variable length):
 *         Each entry: MFT ref (8B), length (2B), stream len (2B),
 *                     flags (4B, but only low byte used), $FILE_NAME payload,
 *                     optional child VCN (8B at end if has-sub-node flag set)
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
        parse_fn_content(entry + 0x10, out->stream_length, &out->fn);
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
 *
 * When a directory's B+ tree overflows the $INDEX_ROOT, child nodes are
 * stored as INDX records on disk via the $INDEX_ALLOCATION attribute.
 *
 * Each INDX record:
 *   0x00  Magic: "INDX" (4 bytes)
 *   0x04  USA offset (2 bytes)
 *   0x06  USA size (2 bytes, in 16-bit words)
 *   0x08  LSN (8 bytes)
 *   0x10  VCN of this INDX record (8 bytes)
 *   0x18  Node header (16 bytes):
 *         0x00  Offset to first entry (from node header start)
 *         0x04  Total size of entries
 *         0x08  Allocated size of entries
 *         0x0C  Flags (0x01 = has children)
 *   0x28+ Index entries (same format as $INDEX_ROOT entries)
 *
 * The INDX buffer must be fixup-verified (USA) before parsing entries.
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
 *
 * Resolves paths by walking each directory's B+ tree.  The algorithm:
 *   1. Parse $INDEX_ROOT to get the root node's sorted entries
 *   2. For each entry, compare the search name (case-insensitive)
 *   3. If match → return the entry's MFT inode
 *   4. If name < entry and entry has a sub-node → descend to child INDX
 *   5. If name < entry and no sub-node → not found (leaf)
 *   6. If we pass the sentinel (LAST) entry and it has a sub-node → descend
 *   7. Repeat in the child INDX buffer until match or leaf
 *
 * Case-insensitive comparison uses ASCII toupper() as fallback.  Full
 * Unicode $UpCase table support is added in §7.1.
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

/* Search sorted index entries in a single node for 'name'.
 *
 * Returns:
 *   NTFS_OK          — match found, *out_inode set
 *   NTFS_ERR_NOT_FOUND — not found; if *out_child_vcn != (uint64_t)-1,
 *                         descend to that VCN; else it's a leaf (definitive miss).
 */
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
            /* If sentinel has a sub-node, descend there
             * (name sorts after all entries in this node) */
            if (ie.flags & NTFS_INDEX_ENTRY_SUBNODE)
                *out_child_vcn = ie.child_vcn;
            return NTFS_ERR_NOT_FOUND;
        }

        /* Compare search name against this entry's $FILE_NAME.
         * The $FILE_NAME content starts at entry+0x10.
         * name_length is at fn_data[0x40], UTF-16LE name at fn_data[0x42]. */
        {
            const uint8_t *fn_data = entry + 0x10;  /* $FILE_NAME content */
            uint8_t entry_nlen = fn_data[0x40];      /* name_length (chars) */
            const uint8_t *entry_name = fn_data + 0x42;  /* UTF-16LE name */

            cmp = ntfs_name_cmp_i(name, name_len, entry_name, (int)entry_nlen);
        }

        if (cmp == 0) {
            /* Match! Return this entry's MFT inode */
            *out_inode = ie.mft_inode;
            return NTFS_OK;
        }

        if (cmp < 0) {
            /* name < entry: if sub-node exists, descend; else not found */
            if (ie.flags & NTFS_INDEX_ENTRY_SUBNODE)
                *out_child_vcn = ie.child_vcn;
            return NTFS_ERR_NOT_FOUND;
        }

        /* cmp > 0: name > entry, continue to next entry */
        entry = ntfs_index_entry_next(entry, entries_base, nh, &ie);
    }

    /* Fell off the end without hitting sentinel — shouldn't happen on
     * well-formed NTFS, but handle gracefully */
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

    /* Apply fixup */
    rc = ntfs_apply_fixup(rec_buf, vol->frs_size, vol->bytes_per_sector);
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
        /* Root says "has children" but no $INDEX_ALLOCATION — corrupt */
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

        /* Otherwise, loop: read next INDX at the new child_vcn */
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
 *
 * Walks all entries in a directory in B+ tree order.  For each non-sentinel,
 * non-DOS entry, builds an ntfs_dir_entry and invokes the user callback.
 *
 * Algorithm:
 *   1. Read directory's MFT record
 *   2. Parse $INDEX_ROOT → walk entries in the root node
 *   3. If $INDEX_ALLOCATION exists:
 *      a. Read $BITMAP ($I30) to find which INDX VCNs are in-use
 *      b. For each active VCN: read INDX buffer, walk entries
 *   4. Skip DOS-only names (namespace 0x02) and sentinel entries
 *
 * The $BITMAP attribute (type 0xB0, named "$I30") is a bitfield where
 * bit N corresponds to the INDX record at VCN = N * clusters_per_indx.
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

/* Walk entries in a single index node, invoking cb for each visible entry.
 * Returns NTFS_OK if enumeration completed, or a callback's non-zero return
 * to signal early stop. */
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

    rc = ntfs_apply_fixup(rec_buf, vol->frs_size, vol->bytes_per_sector);
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
