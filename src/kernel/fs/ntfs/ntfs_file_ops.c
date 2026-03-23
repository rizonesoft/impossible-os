/* ============================================================================
 * ntfs_file_ops.c — File Lifecycle Operations (§12.5)
 *
 * Core CRUD operations for NTFS: create, delete, rename files and
 * directories.  Builds on the write infrastructure from §12.1–12.4:
 *   - ntfs_alloc_mft_record() / ntfs_free_mft_record()  (§12.3)
 *   - ntfs_attr_add() / ntfs_attr_update() / ntfs_attr_remove()  (§12.4)
 *   - ntfs_alloc_clusters() / ntfs_free_clusters()  (§12.1)
 *   - ntfs_regenerate_fixup()  (§12.2)
 *
 * NOTE: Journaling (§13) is NOT implemented yet — these operations are
 * un-journaled.  When §13 is done, wrap each public function in a
 * journal transaction.
 *
 * NOTE: B+ tree node split/merge (§14.1) is NOT implemented yet —
 * directory entry insert/remove only handles the $INDEX_ROOT node.
 * If the root node overflows, NTFS_ERR_IO is returned.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/timer.h"
#include "kernel/klog.h"

/* Windows FILETIME units: 100-nanosecond intervals. */
#define NTFS_FILETIME_HZ          10000000ULL

/* Base FILETIME for 2026-01-01T00:00:00Z — used when no RTC is available.
 * = 1601→2026 seconds * 10^7 */
#define NTFS_FILETIME_BASE_2026   134378784000000000ULL

/* $FILE_NAME on-disk content size without the variable-length name.
 * 0x42 bytes = parent_ref(8) + timestamps(32) + sizes(16) + flags(4) +
 *              reparse(4) + name_length(1) + name_space(1) */
#define NTFS_FN_FIXED_SIZE  0x42

/* $STANDARD_INFORMATION on-disk content size for NTFS 3.x (72 bytes) */
#define NTFS_STD_INFO_SIZE  0x48

/* Maximum entry size in $INDEX_ROOT = 16 (header) + $FILE_NAME content.
 * For a 255-char name: 16 + 0x42 + 255*2 = 16 + 66 + 510 = 592 bytes. */
#define MAX_INDEX_ENTRY_SIZE  600

/* $FILE_NAME namespace values */
#define NTFS_NS_POSIX      0x00
#define NTFS_NS_WIN32      0x01
#define NTFS_NS_DOS        0x02
#define NTFS_NS_WIN32DOS   0x03  /* Win32 + DOS combined (most common) */

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Generate a current Windows FILETIME.
 * Uses uptime() + a fixed base epoch since no RTC is available. */
static uint64_t ntfs_current_filetime(void)
{
    uint64_t secs = uptime();
    return NTFS_FILETIME_BASE_2026 + secs * NTFS_FILETIME_HZ;
}

/* Compute string length (no libc) */
static int ntfs_strlen(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    return len;
}

/* Case-insensitive comparison of two ASCII characters */
static int ntfs_toupper_ch(int c)
{
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

/* Case-insensitive ASCII name vs UTF-16LE comparison.
 * Returns negative if name < entry, 0 if equal, positive if name > entry. */
static int ntfs_name_compare(const char *name, int name_len,
                              const uint8_t *utf16_name, int utf16_len)
{
    int i;
    int min_len = name_len < utf16_len ? name_len : utf16_len;

    for (i = 0; i < min_len; i++) {
        uint16_t a = (uint16_t)ntfs_toupper_ch((uint8_t)name[i]);
        uint16_t b = ntfs_le16(utf16_name + i * 2);
        if (b >= 'a' && b <= 'z') b -= ('a' - 'A');

        if (a != b)
            return (int)a - (int)b;
    }
    return name_len - utf16_len;
}

/* Build the raw on-disk $STANDARD_INFORMATION content (NTFS 3.x, 72 bytes).
 * All 4 timestamps set to `now`.  DOS attrs set to `dos_attrs`. */
static void build_std_info(uint8_t *buf, uint64_t now, uint32_t dos_attrs)
{
    ntfs_memset(buf, 0, NTFS_STD_INFO_SIZE);

    /* Timestamps (4 × 8 bytes) */
    ntfs_le64_write(buf + 0x00, now);  /* Creation */
    ntfs_le64_write(buf + 0x08, now);  /* Modification */
    ntfs_le64_write(buf + 0x10, now);  /* MFT change */
    ntfs_le64_write(buf + 0x18, now);  /* Access */

    /* DOS permissions */
    ntfs_le32_write(buf + 0x20, dos_attrs);

    /* NTFS 3.0+ extended fields (0x24–0x47) — leave zeroed */
}

/* Build the raw on-disk $FILE_NAME content.
 * parent_ref: full MFT reference (inode | seq<<48).
 * name: ASCII filename.  name_len: length in chars.
 * namespace: NTFS_NS_WIN32DOS (0x03) normally.
 * flags: $FILE_NAME flags (0x10000000 for directories).
 * now: FILETIME for all timestamps.
 * Returns the total content size (0x42 + name_len * 2). */
static uint32_t build_file_name(uint8_t *buf, uint64_t parent_ref,
                                 const char *name, int name_len,
                                 uint8_t namespace, uint32_t flags,
                                 uint64_t now)
{
    uint32_t total = NTFS_FN_FIXED_SIZE + (uint32_t)name_len * 2;
    int i;

    ntfs_memset(buf, 0, total);

    /* 0x00: Parent directory MFT reference (8 bytes) */
    ntfs_le64_write(buf + 0x00, parent_ref);

    /* 0x08–0x27: Timestamps (4 × 8 bytes) */
    ntfs_le64_write(buf + 0x08, now);  /* Creation */
    ntfs_le64_write(buf + 0x10, now);  /* Modification */
    ntfs_le64_write(buf + 0x18, now);  /* MFT change */
    ntfs_le64_write(buf + 0x20, now);  /* Access */

    /* 0x28: Allocated size (0 for new empty files) */
    /* 0x30: Real size (0 for new empty files) */
    /* Already zeroed */

    /* 0x38: Flags */
    ntfs_le32_write(buf + 0x38, flags);

    /* 0x3C: Reparse value / EA size (0) */
    /* Already zeroed */

    /* 0x40: Name length in UTF-16 chars */
    buf[0x40] = (uint8_t)name_len;

    /* 0x41: Namespace */
    buf[0x41] = namespace;

    /* 0x42–: Name in UTF-16LE */
    for (i = 0; i < name_len; i++)
        ntfs_le16_write(buf + 0x42 + i * 2, (uint16_t)(uint8_t)name[i]);

    return total;
}

/* Build a raw index entry for insertion into $INDEX_ROOT.
 * child_ref: full MFT reference of the child file/dir.
 * fn_data: raw $FILE_NAME attribute content.
 * fn_data_len: length of fn_data.
 * Returns the total entry length (aligned to 8 bytes). */
static uint32_t build_index_entry(uint8_t *buf, uint64_t child_ref,
                                   const uint8_t *fn_data, uint32_t fn_data_len)
{
    uint32_t entry_len = 0x10 + fn_data_len;   /* header + stream */
    entry_len = (entry_len + 7) & ~7u;          /* align to 8 */

    ntfs_memset(buf, 0, entry_len);

    /* 0x00: MFT reference of the indexed file */
    ntfs_le64_write(buf + 0x00, child_ref);

    /* 0x08: Entry length */
    ntfs_le16_write(buf + 0x08, (uint16_t)entry_len);

    /* 0x0A: Stream ($FILE_NAME) length */
    ntfs_le16_write(buf + 0x0A, (uint16_t)fn_data_len);

    /* 0x0C: Flags (0 = leaf, no sub-node) */
    buf[0x0C] = 0;

    /* 0x10: $FILE_NAME content */
    ntfs_memcpy(buf + 0x10, fn_data, fn_data_len);

    return entry_len;
}

/* Build an empty $INDEX_ROOT attribute content for a new directory.
 * The content is: index_root_header(16) + node_header(16) + sentinel(16).
 * Returns content size (always 0x38 = 56 bytes). */
static uint32_t build_empty_index_root(uint8_t *buf)
{
    ntfs_memset(buf, 0, 0x38);

    /* Index Root Header (16 bytes) */
    ntfs_le32_write(buf + 0x00, NTFS_ATTR_FILE_NAME);  /* Indexed attr type */
    ntfs_le32_write(buf + 0x04, 0x01);                  /* Collation: filename */
    ntfs_le32_write(buf + 0x08, 4096);                  /* INDX record size */
    buf[0x0C] = 1;                                       /* Clusters per INDX */

    /* Node Header (16 bytes at offset 0x10) */
    ntfs_le32_write(buf + 0x10, 0x10);  /* Entries offset (from node start) */
    ntfs_le32_write(buf + 0x14, 0x20);  /* Total size of entries area */
    ntfs_le32_write(buf + 0x18, 0x20);  /* Allocated size */
    buf[0x1C] = 0;                       /* Flags: leaf, no children */

    /* Sentinel entry at offset 0x20 (node start + entries_offset) */
    /* = 0x10 (node header) + 0x10 (entries_offset) = 0x20 */
    ntfs_le16_write(buf + 0x28, 0x10);     /* Entry length = 16 */
    buf[0x2C] = NTFS_INDEX_ENTRY_LAST;     /* Flags = LAST */

    return 0x38;
}

/* ============================================================================
 * ntfs_write_mft_record — Write an in-memory MFT record back to disk
 *
 * 1. Apply USA regeneration (§12.2) — stamps sector last-2-bytes
 * 2. Map inode → LBA via $MFT data runs
 * 3. Write record to disk via blkdev_write()
 * 4. Invalidate the MFT cache entry
 * ============================================================================ */

int ntfs_write_mft_record(struct ntfs_volume *vol, uint64_t inode,
                          uint8_t *rec)
{
    uint64_t lba;
    uint32_t sector_count;

    if (!vol || !rec)
        return NTFS_ERR_IO;

    /* Apply USA regeneration before writing */
    if (ntfs_regenerate_fixup(rec, vol->frs_size,
                                vol->bytes_per_sector) != NTFS_OK) {
        klog(LOG_ERROR, "ntfs",
             "write_mft_record: fixup failed for inode %llu", inode);
        return NTFS_ERR_FIXUP;
    }

    /* Map inode to disk LBA */
    if (ntfs_mft_inode_to_lba(vol, inode, &lba) < 0) {
        klog(LOG_ERROR, "ntfs",
             "write_mft_record: LBA mapping failed for inode %llu", inode);
        return NTFS_ERR_IO;
    }

    sector_count = vol->frs_size / vol->bytes_per_sector;
    if (sector_count == 0)
        sector_count = 1;

    if (blkdev_write(vol->dev, lba, sector_count, rec) != 0) {
        klog(LOG_ERROR, "ntfs",
             "write_mft_record: disk write failed for inode %llu", inode);
        return NTFS_ERR_IO;
    }

    /* Invalidate cache so next read gets the fresh record */
    ntfs_cache_invalidate(vol, inode);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_dir_insert_entry — Insert a directory entry into $INDEX_ROOT
 *
 * Reads the directory's MFT record, finds $INDEX_ROOT, inserts the
 * new entry at the correct sorted position, updates the node header,
 * and writes the record back to disk.
 *
 * LIMITATION: Only handles $INDEX_ROOT (no INDX buffer allocation).
 * If the root is too full, returns NTFS_ERR_IO.  Full B+ tree support
 * comes with §14.1.
 * ============================================================================ */

int ntfs_dir_insert_entry(struct ntfs_volume *vol, uint64_t dir_inode,
                          uint64_t child_inode, uint16_t child_seq,
                          const uint8_t *fn_data, uint32_t fn_data_len)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    uint8_t *root_content;
    uint32_t content_off;
    uint32_t content_len;
    uint8_t *node;
    uint32_t entries_off;
    uint32_t total_entries_size;
    uint8_t  new_entry[MAX_INDEX_ENTRY_SIZE];
    uint32_t new_entry_len;
    uint64_t child_ref;
    int rc;

    /* Build the child MFT reference */
    child_ref = (child_inode & 0x0000FFFFFFFFFFFFULL) |
                ((uint64_t)child_seq << 48);

    /* Build the new index entry */
    new_entry_len = build_index_entry(new_entry, child_ref,
                                       fn_data, fn_data_len);

    /* Allocate and read directory MFT record */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Find $INDEX_ROOT named "$I30" */
    attr = ntfs_attr_find_named(rec, &hdr, NTFS_ATTR_INDEX_ROOT,
                                "$I30", &ah);
    if (!attr || ah.non_resident) {
        kfree(rec);
        return NTFS_ERR_BAD_MAGIC;
    }

    content_off = (uint32_t)(attr - rec) + ah.content_offset;
    content_len = ah.content_length;

    root_content = rec + content_off;

    /* Node header starts at content + 0x10 */
    node = root_content + 0x10;
    entries_off = ntfs_le32(node + 0x00);
    total_entries_size = ntfs_le32(node + 0x04);

    /* Find the sorted insertion point within entries.
     * Entries are sorted by filename using case-insensitive comparison. */
    {
        uint8_t *entries_base = node + entries_off;
        uint32_t pos = 0;
        uint32_t insert_pos = 0;  /* Offset within entries area */
        int found_spot = 0;
        int child_name_len = fn_data[0x40];
        const uint8_t *child_name_utf16 = fn_data + 0x42;

        while (pos < total_entries_size) {
            uint16_t e_len = ntfs_le16(entries_base + pos + 0x08);
            uint8_t  e_flags = entries_base[pos + 0x0C];

            if (e_len < 0x10) break;  /* Corrupt */

            /* Sentinel (LAST): insert before it */
            if (e_flags & NTFS_INDEX_ENTRY_LAST) {
                insert_pos = pos;
                found_spot = 1;
                break;
            }

            /* Compare new entry's name against this entry's name */
            {
                const uint8_t *e_fn = entries_base + pos + 0x10;
                int e_nlen = e_fn[0x40];
                const uint8_t *e_name = e_fn + 0x42;
                /* Compare child name (UTF-16) vs entry name (UTF-16) */
                int cmp;
                int min_len = child_name_len < e_nlen ?
                              child_name_len : e_nlen;
                int ci;

                cmp = 0;
                for (ci = 0; ci < min_len; ci++) {
                    uint16_t a = ntfs_le16(child_name_utf16 + ci * 2);
                    uint16_t b = ntfs_le16(e_name + ci * 2);
                    if (a >= 'a' && a <= 'z') a -= ('a' - 'A');
                    if (b >= 'a' && b <= 'z') b -= ('a' - 'A');
                    if (a != b) { cmp = (int)a - (int)b; break; }
                }
                if (cmp == 0)
                    cmp = child_name_len - e_nlen;

                if (cmp < 0) {
                    /* New entry sorts before this one */
                    insert_pos = pos;
                    found_spot = 1;
                    break;
                }
            }

            pos += e_len;
        }

        if (!found_spot) {
            /* Should not happen — sentinel always found */
            kfree(rec);
            return NTFS_ERR_IO;
        }

        /* Check if we have room in the attribute.
         * The $INDEX_ROOT is a resident attribute inside the MFT record.
         * We need new_entry_len extra bytes. */
        {
            uint32_t new_total_entries = total_entries_size + new_entry_len;
            uint32_t new_content_len = 0x10 + entries_off + new_total_entries;

            /* Check against MFT record free space */
            uint32_t old_attr_total = ah.total_length;
            uint32_t new_attr_total = ah.content_offset + new_content_len;
            new_attr_total = (new_attr_total + 7) & ~7u;
            int32_t growth = (int32_t)new_attr_total - (int32_t)old_attr_total;

            if (growth > 0 &&
                (uint32_t)growth > hdr.alloc_size - hdr.used_size) {
                klog(LOG_WARN, "ntfs",
                     "dir_insert: $INDEX_ROOT overflow (need %d bytes, "
                     "free %u) — B+ tree split needed (§14.1)",
                     growth,
                     (uint64_t)(hdr.alloc_size - hdr.used_size));
                kfree(rec);
                return NTFS_ERR_IO;  /* Need §14.1 for B+ tree split */
            }
        }

        /* Remove the old $INDEX_ROOT, build updated content, re-add it */
        {
            uint32_t old_root_size = content_len;
            uint32_t new_entries_total = total_entries_size + new_entry_len;
            uint32_t new_root_size = 0x10 + entries_off + new_entries_total;
            uint8_t *new_root;

            new_root = (uint8_t *)kmalloc(new_root_size);
            if (!new_root) {
                kfree(rec);
                return NTFS_ERR_IO;
            }

            /* Copy index root header (16 bytes) */
            ntfs_memcpy(new_root, root_content, 0x10);

            /* Copy node header (entries_off bytes) */
            ntfs_memcpy(new_root + 0x10, node, entries_off);

            /* Copy entries before insertion point */
            if (insert_pos > 0)
                ntfs_memcpy(new_root + 0x10 + entries_off,
                            entries_base, insert_pos);

            /* Insert new entry */
            ntfs_memcpy(new_root + 0x10 + entries_off + insert_pos,
                        new_entry, new_entry_len);

            /* Copy remaining entries (from insert_pos to end) */
            {
                uint32_t remaining = total_entries_size - insert_pos;
                if (remaining > 0)
                    ntfs_memcpy(new_root + 0x10 + entries_off +
                                insert_pos + new_entry_len,
                                entries_base + insert_pos, remaining);
            }

            /* Update node header: total_size and alloc_size */
            ntfs_le32_write(new_root + 0x10 + 0x04, new_entries_total);
            ntfs_le32_write(new_root + 0x10 + 0x08, new_entries_total);

            /* Remove old $INDEX_ROOT and add updated one */
            rc = ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                                   NTFS_ATTR_INDEX_ROOT, "$I30");
            if (rc != NTFS_OK) {
                kfree(new_root);
                kfree(rec);
                return rc;
            }

            rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                                NTFS_ATTR_INDEX_ROOT, "$I30",
                                new_root, new_root_size);
            kfree(new_root);

            if (rc != NTFS_OK) {
                kfree(rec);
                return rc;
            }

            (void)old_root_size;
        }
    }

    /* Write updated directory record to disk */
    rc = ntfs_write_mft_record(vol, dir_inode, rec);
    kfree(rec);

    return rc;
}

/* ============================================================================
 * ntfs_dir_remove_entry — Remove a directory entry by filename
 *
 * Reads the directory's MFT record, scans $INDEX_ROOT for the entry,
 * removes it, compacts entries, and writes the record back.
 *
 * LIMITATION: Only searches $INDEX_ROOT (not INDX buffers).
 * ============================================================================ */

int ntfs_dir_remove_entry(struct ntfs_volume *vol, uint64_t dir_inode,
                          const char *name)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    uint8_t *root_content;
    uint32_t content_off;
    uint8_t *node;
    uint32_t entries_off;
    uint32_t total_entries_size;
    int name_len;
    int rc;

    name_len = ntfs_strlen(name);
    if (name_len == 0)
        return NTFS_ERR_NOT_FOUND;

    /* Read directory MFT record */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, dir_inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Find $INDEX_ROOT "$I30" */
    attr = ntfs_attr_find_named(rec, &hdr, NTFS_ATTR_INDEX_ROOT,
                                "$I30", &ah);
    if (!attr || ah.non_resident) {
        kfree(rec);
        return NTFS_ERR_BAD_MAGIC;
    }

    content_off = (uint32_t)(attr - rec) + ah.content_offset;
    root_content = rec + content_off;

    node = root_content + 0x10;
    entries_off = ntfs_le32(node + 0x00);
    total_entries_size = ntfs_le32(node + 0x04);

    /* Scan entries for the matching filename */
    {
        uint8_t *entries_base = node + entries_off;
        uint32_t pos = 0;
        int found = 0;
        uint32_t found_pos = 0;
        uint16_t found_len = 0;

        while (pos < total_entries_size) {
            uint16_t e_len = ntfs_le16(entries_base + pos + 0x08);
            uint8_t  e_flags = entries_base[pos + 0x0C];

            if (e_len < 0x10) break;

            if (e_flags & NTFS_INDEX_ENTRY_LAST)
                break;  /* Sentinel — name not found */

            /* Compare */
            {
                const uint8_t *e_fn = entries_base + pos + 0x10;
                int e_nlen = e_fn[0x40];
                const uint8_t *e_name = e_fn + 0x42;
                int cmp = ntfs_name_compare(name, name_len,
                                             e_name, e_nlen);
                if (cmp == 0) {
                    found = 1;
                    found_pos = pos;
                    found_len = e_len;
                    break;
                }
            }

            pos += e_len;
        }

        if (!found) {
            kfree(rec);
            return NTFS_ERR_NOT_FOUND;
        }

        /* Remove the entry by compacting */
        {
            uint32_t new_total = total_entries_size - found_len;
            uint32_t new_root_size = 0x10 + entries_off + new_total;
            uint8_t *new_root;

            new_root = (uint8_t *)kmalloc(new_root_size);
            if (!new_root) {
                kfree(rec);
                return NTFS_ERR_IO;
            }

            /* Copy root header + node header */
            ntfs_memcpy(new_root, root_content, 0x10 + entries_off);

            /* Copy entries before the removed one */
            if (found_pos > 0)
                ntfs_memcpy(new_root + 0x10 + entries_off,
                            entries_base, found_pos);

            /* Copy entries after the removed one */
            {
                uint32_t after_off = found_pos + found_len;
                uint32_t remaining = total_entries_size - after_off;
                if (remaining > 0)
                    ntfs_memcpy(new_root + 0x10 + entries_off + found_pos,
                                entries_base + after_off, remaining);
            }

            /* Update node header */
            ntfs_le32_write(new_root + 0x10 + 0x04, new_total);
            ntfs_le32_write(new_root + 0x10 + 0x08, new_total);

            /* Replace $INDEX_ROOT attribute */
            rc = ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                                   NTFS_ATTR_INDEX_ROOT, "$I30");
            if (rc != NTFS_OK) {
                kfree(new_root);
                kfree(rec);
                return rc;
            }

            rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                                NTFS_ATTR_INDEX_ROOT, "$I30",
                                new_root, new_root_size);
            kfree(new_root);

            if (rc != NTFS_OK) {
                kfree(rec);
                return rc;
            }
        }
    }

    /* Write updated record to disk */
    rc = ntfs_write_mft_record(vol, dir_inode, rec);
    kfree(rec);

    return rc;
}

/* ============================================================================
 * ntfs_create_file — Create a new file in an NTFS directory
 *
 * 1. Allocate new MFT record (§12.3)
 * 2. Add $STANDARD_INFORMATION (type 0x10) with current timestamps
 * 3. Add $FILE_NAME (type 0x30) with parent ref, Win32/DOS namespace
 * 4. Add empty $DATA (type 0x80) — resident, zero length
 * 5. Set hard link count to 1
 * 6. Insert directory entry into parent's $INDEX_ROOT
 * 7. Update parent's $STANDARD_INFORMATION modification timestamp
 * ============================================================================ */

int ntfs_create_file(struct ntfs_volume *vol, uint64_t parent_inode,
                     const char *name, uint32_t attrs)
{
    uint64_t now;
    uint64_t new_inode;
    uint16_t new_seq;
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    uint8_t std_info[NTFS_STD_INFO_SIZE];
    uint8_t fn_data[NTFS_FN_FIXED_SIZE + NTFS_MAX_NAME * 2];
    uint32_t fn_len;
    int name_len;
    uint64_t parent_ref;
    int rc;

    if (!vol || !name)
        return NTFS_ERR_IO;

    name_len = ntfs_strlen(name);
    if (name_len == 0 || name_len > NTFS_MAX_NAME)
        return NTFS_ERR_IO;

    now = ntfs_current_filetime();

    /* Step 1: Allocate MFT record */
    new_inode = ntfs_alloc_mft_record(vol, 0 /* not directory */);
    if (new_inode == 0) {
        klog(LOG_ERROR, "ntfs", "create_file: MFT record allocation failed");
        return NTFS_ERR_IO;
    }

    /* Read the freshly allocated record */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record_raw(vol, new_inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Get sequence number from the new record */
    new_seq = hdr.seq_number;

    /* Step 2: Add $STANDARD_INFORMATION */
    build_std_info(std_info, now, attrs);
    rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                        NTFS_ATTR_STANDARD_INFORMATION, NULL,
                        std_info, NTFS_STD_INFO_SIZE);
    if (rc != NTFS_OK) {
        klog(LOG_ERROR, "ntfs", "create_file: failed to add $STD_INFO");
        kfree(rec);
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 3: Add $FILE_NAME (Win32/DOS namespace = 0x03) */
    {
        /* Read parent to get its sequence number for the MFT reference */
        uint8_t *parent_rec = (uint8_t *)kmalloc(vol->frs_size);
        struct ntfs_mft_header parent_hdr;
        uint16_t parent_seq = 0;

        if (parent_rec) {
            if (ntfs_read_mft_record(vol, parent_inode, parent_rec,
                                      &parent_hdr) == NTFS_OK) {
                parent_seq = parent_hdr.seq_number;
            }
            kfree(parent_rec);
        }

        parent_ref = (parent_inode & 0x0000FFFFFFFFFFFFULL) |
                     ((uint64_t)parent_seq << 48);
    }

    fn_len = build_file_name(fn_data, parent_ref, name, name_len,
                              NTFS_NS_WIN32DOS, 0, now);
    rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                        NTFS_ATTR_FILE_NAME, NULL, fn_data, fn_len);
    if (rc != NTFS_OK) {
        klog(LOG_ERROR, "ntfs", "create_file: failed to add $FILE_NAME");
        kfree(rec);
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 4: Add empty $DATA (resident, 0 bytes) */
    rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                        NTFS_ATTR_DATA, NULL, NULL, 0);
    if (rc != NTFS_OK) {
        klog(LOG_ERROR, "ntfs", "create_file: failed to add $DATA");
        kfree(rec);
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 5: Set hard link count to 1 */
    ntfs_le16_write(rec + 0x12, 1);
    hdr.hard_link_count = 1;

    /* Write the completed record to disk */
    rc = ntfs_write_mft_record(vol, new_inode, rec);
    kfree(rec);

    if (rc != NTFS_OK) {
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 6: Insert directory entry into parent */
    rc = ntfs_dir_insert_entry(vol, parent_inode, new_inode, new_seq,
                                fn_data, fn_len);
    if (rc != NTFS_OK) {
        klog(LOG_ERROR, "ntfs",
             "create_file: failed to insert dir entry for '%s'", name);
        /* Roll back: free the MFT record we just created */
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 7: Update parent's modification timestamp */
    {
        uint8_t *prec = (uint8_t *)kmalloc(vol->frs_size);
        struct ntfs_mft_header phdr;

        if (prec && ntfs_read_mft_record(vol, parent_inode,
                                          prec, &phdr) == NTFS_OK) {
            /* Find $STANDARD_INFORMATION and update modification time */
            const uint8_t *si = ntfs_attr_find(prec, &phdr,
                                                NTFS_ATTR_STANDARD_INFORMATION,
                                                NULL);
            if (si) {
                struct ntfs_attr_header si_ah;
                ntfs_attr_parse(si, &si_ah);
                if (!si_ah.non_resident) {
                    uint8_t *si_data = prec + (uint32_t)(si - prec) +
                                       si_ah.content_offset;
                    ntfs_le64_write(si_data + 0x08, now);  /* Modification */
                    ntfs_le64_write(si_data + 0x10, now);  /* MFT change */
                    ntfs_write_mft_record(vol, parent_inode, prec);
                }
            }
        }
        if (prec) kfree(prec);
    }

    klog(LOG_INFO, "ntfs",
         "Created file '%s' (inode %llu) in directory inode %llu",
         name, new_inode, parent_inode);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_create_directory — Create a new directory
 *
 * Same as ntfs_create_file but:
 *   - Sets directory flag in MFT record (bit 1)
 *   - Sets directory flag in $FILE_NAME (0x10000000)
 *   - Adds $INDEX_ROOT (type 0x90, named "$I30") with empty root node
 * ============================================================================ */

int ntfs_create_directory(struct ntfs_volume *vol, uint64_t parent_inode,
                          const char *name)
{
    uint64_t now;
    uint64_t new_inode;
    uint16_t new_seq;
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    uint8_t std_info[NTFS_STD_INFO_SIZE];
    uint8_t fn_data[NTFS_FN_FIXED_SIZE + NTFS_MAX_NAME * 2];
    uint32_t fn_len;
    uint8_t idx_root[0x38];
    uint32_t idx_root_len;
    int name_len;
    uint64_t parent_ref;
    int rc;

    if (!vol || !name)
        return NTFS_ERR_IO;

    name_len = ntfs_strlen(name);
    if (name_len == 0 || name_len > NTFS_MAX_NAME)
        return NTFS_ERR_IO;

    now = ntfs_current_filetime();

    /* Step 1: Allocate MFT record with directory flag */
    new_inode = ntfs_alloc_mft_record(vol, 1 /* directory */);
    if (new_inode == 0) {
        klog(LOG_ERROR, "ntfs", "create_dir: MFT record allocation failed");
        return NTFS_ERR_IO;
    }

    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record_raw(vol, new_inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    new_seq = hdr.seq_number;

    /* Step 2: Add $STANDARD_INFORMATION */
    build_std_info(std_info, now, NTFS_FILE_ATTR_HIDDEN);
    rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                        NTFS_ATTR_STANDARD_INFORMATION, NULL,
                        std_info, NTFS_STD_INFO_SIZE);
    if (rc != NTFS_OK) {
        kfree(rec);
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 3: Add $FILE_NAME with directory flag */
    {
        uint8_t *prec = (uint8_t *)kmalloc(vol->frs_size);
        struct ntfs_mft_header phdr;
        uint16_t parent_seq = 0;

        if (prec) {
            if (ntfs_read_mft_record(vol, parent_inode, prec,
                                      &phdr) == NTFS_OK) {
                parent_seq = phdr.seq_number;
            }
            kfree(prec);
        }

        parent_ref = (parent_inode & 0x0000FFFFFFFFFFFFULL) |
                     ((uint64_t)parent_seq << 48);
    }

    fn_len = build_file_name(fn_data, parent_ref, name, name_len,
                              NTFS_NS_WIN32DOS,
                              0x10000000,  /* Directory flag in $FILE_NAME */
                              now);
    rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                        NTFS_ATTR_FILE_NAME, NULL, fn_data, fn_len);
    if (rc != NTFS_OK) {
        kfree(rec);
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 4: Add $INDEX_ROOT (named "$I30") with empty root node */
    idx_root_len = build_empty_index_root(idx_root);
    rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                        NTFS_ATTR_INDEX_ROOT, "$I30",
                        idx_root, idx_root_len);
    if (rc != NTFS_OK) {
        kfree(rec);
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 5: Set hard link count to 1 */
    ntfs_le16_write(rec + 0x12, 1);
    hdr.hard_link_count = 1;

    /* Write the completed record to disk */
    rc = ntfs_write_mft_record(vol, new_inode, rec);
    kfree(rec);

    if (rc != NTFS_OK) {
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 6: Insert directory entry into parent */
    rc = ntfs_dir_insert_entry(vol, parent_inode, new_inode, new_seq,
                                fn_data, fn_len);
    if (rc != NTFS_OK) {
        ntfs_free_mft_record(vol, new_inode);
        return rc;
    }

    /* Step 7: Update parent modification timestamp */
    {
        uint8_t *prec = (uint8_t *)kmalloc(vol->frs_size);
        struct ntfs_mft_header phdr;

        if (prec && ntfs_read_mft_record(vol, parent_inode,
                                          prec, &phdr) == NTFS_OK) {
            const uint8_t *si = ntfs_attr_find(prec, &phdr,
                                                NTFS_ATTR_STANDARD_INFORMATION,
                                                NULL);
            if (si) {
                struct ntfs_attr_header si_ah;
                ntfs_attr_parse(si, &si_ah);
                if (!si_ah.non_resident) {
                    uint8_t *si_data = prec + (uint32_t)(si - prec) +
                                       si_ah.content_offset;
                    ntfs_le64_write(si_data + 0x08, now);
                    ntfs_le64_write(si_data + 0x10, now);
                    ntfs_write_mft_record(vol, parent_inode, prec);
                }
            }
        }
        if (prec) kfree(prec);
    }

    klog(LOG_INFO, "ntfs",
         "Created directory '%s' (inode %llu) in parent inode %llu",
         name, new_inode, parent_inode);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_delete_file — Delete a file from an NTFS directory
 *
 * 1. Look up file in parent directory → get inode
 * 2. Read the file's MFT record
 * 3. Remove directory entry from parent
 * 4. Decrement hard link count
 * 5. If link count == 0:
 *    a. Free all $DATA clusters
 *    b. Free the MFT record
 * 6. Update parent modification timestamp
 * ============================================================================ */

int ntfs_delete_file(struct ntfs_volume *vol, uint64_t parent_inode,
                     const char *name)
{
    uint64_t file_inode;
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    uint16_t link_count;
    uint64_t now;
    int rc;

    if (!vol || !name)
        return NTFS_ERR_IO;

    now = ntfs_current_filetime();

    /* Step 1: Look up the file in the parent directory */
    rc = ntfs_lookup(vol, parent_inode, name, &file_inode);
    if (rc != NTFS_OK)
        return rc;

    /* Step 2: Read the file's MFT record */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, file_inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Step 3: Remove directory entry from parent */
    rc = ntfs_dir_remove_entry(vol, parent_inode, name);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Step 4: Decrement hard link count */
    link_count = hdr.hard_link_count;
    if (link_count > 0)
        link_count--;

    if (link_count == 0) {
        /* Step 5: Free $DATA clusters and MFT record */
        struct ntfs_attr_header data_ah;
        const uint8_t *data_attr;

        data_attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_DATA, &data_ah);
        if (data_attr && data_ah.non_resident) {
            /* Free data clusters */
            struct ntfs_data_run runs[64];
            struct ntfs_nonres_header nrhdr;
            int run_count;
            int i;

            run_count = ntfs_decode_data_runs(data_attr, runs, 64, &nrhdr);
            for (i = 0; i < run_count; i++) {
                if (runs[i].lcn != NTFS_LCN_SPARSE)
                    ntfs_free_clusters(vol, runs[i].lcn, runs[i].length);
            }
        }

        kfree(rec);

        /* Free the MFT record */
        rc = ntfs_free_mft_record(vol, file_inode);
        if (rc != NTFS_OK)
            return rc;
    } else {
        /* Update link count in record and write back */
        ntfs_le16_write(rec + 0x12, link_count);
        rc = ntfs_write_mft_record(vol, file_inode, rec);
        kfree(rec);
        if (rc != NTFS_OK)
            return rc;
    }

    /* Step 6: Update parent modification timestamp */
    {
        uint8_t *prec = (uint8_t *)kmalloc(vol->frs_size);
        struct ntfs_mft_header phdr;

        if (prec && ntfs_read_mft_record(vol, parent_inode,
                                          prec, &phdr) == NTFS_OK) {
            const uint8_t *si = ntfs_attr_find(prec, &phdr,
                                                NTFS_ATTR_STANDARD_INFORMATION,
                                                NULL);
            if (si) {
                struct ntfs_attr_header si_ah;
                ntfs_attr_parse(si, &si_ah);
                if (!si_ah.non_resident) {
                    uint8_t *si_data = prec + (uint32_t)(si - prec) +
                                       si_ah.content_offset;
                    ntfs_le64_write(si_data + 0x08, now);
                    ntfs_le64_write(si_data + 0x10, now);
                    ntfs_write_mft_record(vol, parent_inode, prec);
                }
            }
        }
        if (prec) kfree(prec);
    }

    klog(LOG_INFO, "ntfs",
         "Deleted '%s' (inode %llu) from directory inode %llu",
         name, file_inode, parent_inode);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_rename_file — Rename/move a file between NTFS directories
 *
 * 1. Verify target doesn't already exist (unless replacing)
 * 2. Look up the file in the old directory → get inode
 * 3. Remove entry from old parent's directory index
 * 4. Update $FILE_NAME attribute in the file's MFT record
 * 5. Insert entry into new parent's directory index
 * 6. Update modification timestamps in both parents
 * ============================================================================ */

int ntfs_rename_file(struct ntfs_volume *vol,
                     uint64_t old_parent, const char *old_name,
                     uint64_t new_parent, const char *new_name)
{
    uint64_t file_inode;
    uint64_t existing_inode;
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    uint64_t now;
    int new_name_len;
    uint64_t new_parent_ref;
    uint8_t fn_data[NTFS_FN_FIXED_SIZE + NTFS_MAX_NAME * 2];
    uint32_t fn_len;
    int rc;

    if (!vol || !old_name || !new_name)
        return NTFS_ERR_IO;

    new_name_len = ntfs_strlen(new_name);
    if (new_name_len == 0 || new_name_len > NTFS_MAX_NAME)
        return NTFS_ERR_IO;

    now = ntfs_current_filetime();

    /* Step 1: Verify target doesn't already exist */
    rc = ntfs_lookup(vol, new_parent, new_name, &existing_inode);
    if (rc == NTFS_OK) {
        /* Target exists — for now, return error.
         * Replace-on-rename can be added later. */
        klog(LOG_WARN, "ntfs",
             "rename: target '%s' already exists (inode %llu)",
             new_name, existing_inode);
        return NTFS_ERR_IO;
    }

    /* Step 2: Look up the file in the old directory */
    rc = ntfs_lookup(vol, old_parent, old_name, &file_inode);
    if (rc != NTFS_OK)
        return rc;

    /* Step 3: Remove entry from old parent */
    rc = ntfs_dir_remove_entry(vol, old_parent, old_name);
    if (rc != NTFS_OK)
        return rc;

    /* Step 4: Update $FILE_NAME in the file's MFT record */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, file_inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Build new parent reference */
    {
        uint8_t *prec = (uint8_t *)kmalloc(vol->frs_size);
        struct ntfs_mft_header phdr;
        uint16_t parent_seq = 0;

        if (prec) {
            if (ntfs_read_mft_record(vol, new_parent, prec,
                                      &phdr) == NTFS_OK) {
                parent_seq = phdr.seq_number;
            }
            kfree(prec);
        }

        new_parent_ref = (new_parent & 0x0000FFFFFFFFFFFFULL) |
                         ((uint64_t)parent_seq << 48);
    }

    /* Remove old $FILE_NAME, add updated one */
    rc = ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                           NTFS_ATTR_FILE_NAME, NULL);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Determine if this is a directory from MFT flags */
    {
        uint32_t fn_flags = 0;
        if (hdr.flags & NTFS_MFT_FLAG_DIRECTORY)
            fn_flags = 0x10000000;  /* Directory flag in $FILE_NAME */

        fn_len = build_file_name(fn_data, new_parent_ref, new_name,
                                  new_name_len, NTFS_NS_WIN32DOS,
                                  fn_flags, now);
    }

    rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                        NTFS_ATTR_FILE_NAME, NULL, fn_data, fn_len);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Write updated file record */
    rc = ntfs_write_mft_record(vol, file_inode, rec);
    kfree(rec);

    if (rc != NTFS_OK)
        return rc;

    /* Step 5: Insert entry into new parent */
    rc = ntfs_dir_insert_entry(vol, new_parent, file_inode, hdr.seq_number,
                                fn_data, fn_len);
    if (rc != NTFS_OK) {
        klog(LOG_ERROR, "ntfs",
             "rename: failed to insert into new parent — file is orphaned!");
        return rc;
    }

    /* Step 6: Update timestamps in both parents */
    {
        uint64_t parents[2];
        int pi;
        parents[0] = old_parent;
        parents[1] = new_parent;

        for (pi = 0; pi < 2; pi++) {
            uint8_t *prec = (uint8_t *)kmalloc(vol->frs_size);
            struct ntfs_mft_header phdr;

            if (!prec) continue;

            if (ntfs_read_mft_record(vol, parents[pi], prec,
                                      &phdr) == NTFS_OK) {
                const uint8_t *si = ntfs_attr_find(
                    prec, &phdr, NTFS_ATTR_STANDARD_INFORMATION, NULL);
                if (si) {
                    struct ntfs_attr_header si_ah;
                    ntfs_attr_parse(si, &si_ah);
                    if (!si_ah.non_resident) {
                        uint8_t *si_data = prec +
                            (uint32_t)(si - prec) + si_ah.content_offset;
                        ntfs_le64_write(si_data + 0x08, now);
                        ntfs_le64_write(si_data + 0x10, now);
                        ntfs_write_mft_record(vol, parents[pi], prec);
                    }
                }
            }
            kfree(prec);
        }
    }

    klog(LOG_INFO, "ntfs",
         "Renamed inode %llu: '%s' (dir %llu) → '%s' (dir %llu)",
         file_inode, old_name, old_parent, new_name, new_parent);

    return NTFS_OK;
}
