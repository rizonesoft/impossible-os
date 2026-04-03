/* ============================================================================
 * ntfs_metadata.c -- $STANDARD_INFORMATION, $SECURITY_DESCRIPTOR, $REPARSE_POINT
 *
 * Decoders for NTFS metadata attributes:
 *   - $STANDARD_INFORMATION (type 0x10): timestamps, DOS flags, NTFS 3.0+ fields
 *   - $SECURITY_DESCRIPTOR (type 0x50): SIDs, DACLs, SACLs
 *   - $REPARSE_POINT (type 0xC0): symlinks, junctions, mount points
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/klog.h"

/* ============================================================================
 * $STANDARD_INFORMATION Decoder -- attribute type 0x10
 *
 * Every MFT record has exactly one $STANDARD_INFORMATION attribute.
 * It is always resident (content is small -- 48 or 72 bytes).
 *
 * Content layout:
 *   0x00  Creation time         (8 bytes, FILETIME)
 *   0x08  Modification time     (8 bytes, FILETIME)
 *   0x10  MFT change time       (8 bytes, FILETIME)
 *   0x18  Last access time      (8 bytes, FILETIME)
 *   0x20  DOS permissions/flags (4 bytes)
 *   (0x24+ : Max versions, version number, class ID -- NTFS 3.0+)
 *   (0x30+ : Owner ID, Security ID, Quota, USN -- NTFS 3.0+)
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
 * $SECURITY_DESCRIPTOR Parser -- §3.5 (attribute type 0x50)
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

    /* Inline descriptor not found -- try $Secure via security_id */
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
 * $REPARSE_POINT Parser -- §3.6 (attribute type 0xC0)
 * ============================================================================ */

/* Decode a UTF-16LE path to ASCII (lossy: non-ASCII chars become '?') */
static void decode_utf16_path(const uint8_t *utf16, uint16_t byte_len,
                               char *out, int out_max)
{
    uint16_t chars = byte_len / 2;
    uint16_t i;
    int pos = 0;

    for (i = 0; i < chars && pos < out_max - 1; i++) {
        uint16_t wc = ntfs_le16(utf16 + i * 2);
        if (wc == 0)
            break;
        /* Convert backslash to forward slash for internal use, or keep */
        out[pos++] = (wc < 128) ? (char)wc : '?';
    }
    out[pos] = '\0';
}

/* Strip `\??\` prefix from a substitute name -- it means "NT native path" */
static void strip_nt_prefix(char *path)
{
    /* Check for `\??\` (4 chars) */
    if (path[0] == '\\' && path[1] == '?' && path[2] == '?' &&
        path[3] == '\\') {
        int i;
        for (i = 0; path[i + 4]; i++)
            path[i] = path[i + 4];
        path[i] = '\0';
    }
}

int ntfs_decode_reparse(const uint8_t *record,
                         const struct ntfs_mft_header *hdr,
                         struct ntfs_reparse_data *out)
{
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    const uint8_t *data;
    uint32_t data_len;
    uint32_t tag;
    uint16_t reparse_data_len;

    if (!record || !hdr || !out)
        return NTFS_ERR_IO;

    out->tag = 0;
    out->type = 0;
    out->data_length = 0;
    out->symlink_flags = 0;
    out->substitute_name[0] = '\0';
    out->print_name[0] = '\0';
    out->is_relative = 0;

    /* Find $REPARSE_POINT attribute (type 0xC0) */
    attr = ntfs_attr_find(record, hdr, NTFS_ATTR_REPARSE_POINT, &ah);
    if (!attr)
        return NTFS_ERR_NOT_FOUND;

    if (ah.non_resident != 0)
        return NTFS_ERR_NOT_FOUND;  /* Should always be resident */

    if (ah.content_length < 8)
        return NTFS_ERR_BAD_MAGIC;

    data = attr + ah.content_offset;
    data_len = ah.content_length;

    tag = ntfs_le32(data + 0x00);
    reparse_data_len = ntfs_le16(data + 0x04);

    out->tag = tag;
    out->data_length = reparse_data_len;

    if (tag == NTFS_REPARSE_TAG_MOUNT_POINT) {
        /* Junction / mount point */
        uint16_t sub_off, sub_len, print_off, print_len;
        uint32_t path_buf_start;

        out->type = NTFS_REPARSE_JUNCTION;

        if (data_len < 0x10)
            return NTFS_ERR_BAD_MAGIC;

        sub_off   = ntfs_le16(data + 0x08);
        sub_len   = ntfs_le16(data + 0x0A);
        print_off = ntfs_le16(data + 0x0C);
        print_len = ntfs_le16(data + 0x0E);
        path_buf_start = 0x10;  /* Path buffer starts here */

        /* Decode substitute name */
        if (path_buf_start + sub_off + sub_len <= data_len) {
            decode_utf16_path(data + path_buf_start + sub_off, sub_len,
                               out->substitute_name, NTFS_REPARSE_MAX_PATH);
            strip_nt_prefix(out->substitute_name);
        }

        /* Decode print name */
        if (path_buf_start + print_off + print_len <= data_len) {
            decode_utf16_path(data + path_buf_start + print_off, print_len,
                               out->print_name, NTFS_REPARSE_MAX_PATH);
        }

    } else if (tag == NTFS_REPARSE_TAG_SYMLINK) {
        /* Symbolic link -- same layout but with flags at 0x10 */
        uint16_t sub_off, sub_len, print_off, print_len;
        uint32_t flags;
        uint32_t path_buf_start;

        out->type = NTFS_REPARSE_SYMLINK;

        if (data_len < 0x14)
            return NTFS_ERR_BAD_MAGIC;

        sub_off   = ntfs_le16(data + 0x08);
        sub_len   = ntfs_le16(data + 0x0A);
        print_off = ntfs_le16(data + 0x0C);
        print_len = ntfs_le16(data + 0x0E);
        flags     = ntfs_le32(data + 0x10);
        path_buf_start = 0x14;  /* After the extra flags field */

        out->symlink_flags = flags;
        out->is_relative = (flags & NTFS_SYMLINK_FLAG_RELATIVE) ? 1 : 0;

        /* Decode substitute name */
        if (path_buf_start + sub_off + sub_len <= data_len) {
            decode_utf16_path(data + path_buf_start + sub_off, sub_len,
                               out->substitute_name, NTFS_REPARSE_MAX_PATH);
            if (!out->is_relative)
                strip_nt_prefix(out->substitute_name);
        }

        /* Decode print name */
        if (path_buf_start + print_off + print_len <= data_len) {
            decode_utf16_path(data + path_buf_start + print_off, print_len,
                               out->print_name, NTFS_REPARSE_MAX_PATH);
        }

    } else {
        /* Unknown reparse tag -- store tag but can't decode paths */
        out->type = NTFS_REPARSE_OTHER;
    }

    return NTFS_OK;
}

int ntfs_is_reparse_point(const uint8_t *record,
                           const struct ntfs_mft_header *hdr)
{
    if (!record || !hdr)
        return 0;

    return (ntfs_attr_find(record, hdr,
                            NTFS_ATTR_REPARSE_POINT, NULL) != NULL) ? 1 : 0;
}
