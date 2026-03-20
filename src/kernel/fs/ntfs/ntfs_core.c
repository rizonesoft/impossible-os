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
