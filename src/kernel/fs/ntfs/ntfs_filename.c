/* ============================================================================
 * ntfs_filename.c — $FILE_NAME Decoder (attribute type 0x30)
 *
 * Parses $FILE_NAME attributes from MFT records, extracting parent
 * directory reference, timestamps, sizes, flags, and the filename
 * with namespace priority handling (Win32 > POSIX > DOS).
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"

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
