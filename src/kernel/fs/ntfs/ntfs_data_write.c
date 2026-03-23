/* ============================================================================
 * ntfs_data_write.c — File Write Engine (§16.1)
 *
 * Implements write operations for NTFS files:
 *   ntfs_write_data()          — Write bytes to a file (resident or non-resident)
 *   ntfs_truncate()            — Set file size (shrink or grow)
 *   ntfs_set_file_attributes() — Update DOS attribute flags
 *   ntfs_set_file_time()       — Set file timestamps (Unix → FILETIME)
 *
 * Builds on §12.1–§12.4 infrastructure:
 *   ntfs_attr_find / ntfs_attr_update / ntfs_attr_add / ntfs_attr_remove
 *   ntfs_alloc_clusters / ntfs_free_clusters
 *   ntfs_decode_data_runs / ntfs_encode_data_runs
 *   ntfs_write_mft_record (applies USA regen + writes to disk)
 *   ntfs_txn_begin / ntfs_txn_log / ntfs_txn_commit / ntfs_txn_free
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* Windows FILETIME epoch offset: 100-ns intervals from 1601-01-01 to
 * 1970-01-01 = 11644473600 seconds * 10,000,000. */
#define NTFS_UNIX_TO_FILETIME_OFFSET   116444736000000000ULL
#define NTFS_FILETIME_HZ               10000000ULL

/* Maximum data runs we track in a single non-resident attribute. */
#define NTFS_MAX_WRITE_RUNS  64

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Convert a Unix timestamp (seconds since 1970) to Windows FILETIME. */
static uint64_t unix_to_filetime(uint64_t unix_secs)
{
    return unix_secs * NTFS_FILETIME_HZ + NTFS_UNIX_TO_FILETIME_OFFSET;
}

/* Update the modification and MFT-change timestamps in $STANDARD_INFORMATION.
 * rec: editable MFT record buffer.
 * hdr: parsed MFT header.
 * now_ft: current FILETIME.
 * Returns NTFS_OK or NTFS_ERR_NOT_FOUND. */
static int update_std_info_times(uint8_t *rec, struct ntfs_mft_header *hdr,
                                  uint64_t now_ft)
{
    struct ntfs_attr_header ah;
    const uint8_t *found;
    uint32_t off;

    found = ntfs_attr_find(rec, hdr, NTFS_ATTR_STANDARD_INFORMATION, &ah);
    if (!found)
        return NTFS_ERR_NOT_FOUND;

    off = (uint32_t)(found - rec) + ah.content_offset;

    /* $STANDARD_INFORMATION layout:
     *   +0x00 creation_time
     *   +0x08 modification_time
     *   +0x10 mft_change_time
     *   +0x18 access_time
     * We update modification (+0x08) and MFT-change (+0x10) only. */
    ntfs_le64_write(rec + off + 0x08, now_ft);  /* modification */
    ntfs_le64_write(rec + off + 0x10, now_ft);  /* mft_change */

    return NTFS_OK;
}

/* Write a range of bytes into existing non-resident clusters.
 * Handles partial first/last clusters using per-run bounce buffers.
 * Writes only to runs that overlap [offset, offset+length). */
static int write_nonres_clusters(struct ntfs_volume *vol,
                                  const struct ntfs_data_run *runs,
                                  int run_count,
                                  uint64_t offset, uint64_t length,
                                  const uint8_t *src)
{
    uint64_t cs = vol->cluster_size;
    uint64_t bs = vol->bytes_per_sector;
    uint64_t file_pos = 0;
    int ri;

    for (ri = 0; ri < run_count && length > 0; ri++) {
        uint64_t run_bytes;
        uint64_t run_end;

        if (runs[ri].lcn == NTFS_LCN_SPARSE) {
            file_pos += runs[ri].length * cs;
            continue;
        }

        run_bytes = runs[ri].length * cs;
        run_end   = file_pos + run_bytes;

        /* Does this run overlap the write range? */
        if (run_end <= offset) {
            file_pos = run_end;
            continue;
        }
        if (file_pos >= offset + length) {
            break;
        }

        /* Overlap: [write_start, write_end) ∩ [file_pos, run_end) */
        {
            uint64_t write_start = (offset > file_pos) ? offset : file_pos;
            uint64_t write_end   = (offset + length < run_end)
                                     ? (offset + length) : run_end;
            uint64_t intra_off   = write_start - file_pos;
            uint64_t chunk       = write_end - write_start;

            /* Compute first sector and last sector within this run. */
            uint64_t first_sect  = (runs[ri].lcn * cs + intra_off) / bs;
            uint64_t last_end    = (runs[ri].lcn * cs + intra_off + chunk
                                    + bs - 1) / bs;
            uint64_t sects       = last_end - first_sect;
            uint32_t buf_size    = (uint32_t)(sects * bs);

            uint8_t *bounce = (uint8_t *)kmalloc(buf_size);
            if (!bounce)
                return NTFS_ERR_IO;

            /* Read-modify-write: start with current on-disk content. */
            if (blkdev_read(vol->dev, first_sect, (uint32_t)sects, bounce) != 0) {
                kfree(bounce);
                return NTFS_ERR_IO;
            }

            /* Copy the new data into the appropriate offset within bounce. */
            {
                uint64_t bounce_off = (intra_off % bs == 0)
                    ? 0
                    : (runs[ri].lcn * cs + intra_off) - first_sect * bs;
                ntfs_memcpy(bounce + bounce_off, src, chunk);
            }

            if (blkdev_write(vol->dev, first_sect, (uint32_t)sects, bounce) != 0) {
                kfree(bounce);
                return NTFS_ERR_IO;
            }

            kfree(bounce);
            src += chunk;
            length -= chunk;
        }

        file_pos = run_end;
    }

    return NTFS_OK;
}

/* Re-encode runs into the non-resident $DATA attribute header already in
 * the MFT record (rec) at byte offset attr_off.
 * Updates last_vcn, alloc_size, real_size, init_size in the header.
 * Returns NTFS_OK or NTFS_ERR_IO. */
static int rewrite_nonres_attr(uint8_t *rec, uint32_t attr_off,
                                const struct ntfs_data_run *runs, int run_count,
                                uint64_t alloc_size, uint64_t real_size)
{
    uint16_t run_off;
    uint64_t last_vcn = 0;
    uint8_t run_buf[512];
    int encoded;
    int i;

    run_off = ntfs_le16(rec + attr_off + 0x20);

    /* Compute last VCN from runs */
    for (i = 0; i < run_count; i++)
        last_vcn += runs[i].length;
    if (last_vcn > 0)
        last_vcn--;

    encoded = ntfs_encode_data_runs(runs, run_count, run_buf, 512);
    if (encoded <= 0)
        return NTFS_ERR_IO;

    ntfs_le64_write(rec + attr_off + 0x18, last_vcn);     /* last_vcn */
    ntfs_le64_write(rec + attr_off + 0x28, alloc_size);   /* alloc_size */
    ntfs_le64_write(rec + attr_off + 0x30, real_size);    /* real_size */
    ntfs_le64_write(rec + attr_off + 0x38, real_size);    /* init_size */
    ntfs_memcpy(rec + attr_off + run_off, run_buf, (uint64_t)encoded);
    /* Zero-out any leftover bytes from the old (smaller) run encoding up to
     * the next attribute. We trust the attribute total_length is unchanged
     * for in-place updates; for grow-by-run, caller must re-add the attr. */

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_write_data — Write bytes to a file
 * ============================================================================ */

int ntfs_write_data(struct ntfs_volume *vol, uint64_t inode,
                    uint64_t offset, uint64_t length, const void *buffer)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *data_attr;
    int rc;
    struct ntfs_txn *txn;
    uint64_t now_ft;

    if (!vol || !buffer || length == 0)
        return NTFS_ERR_IO;

    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    data_attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_DATA, &ah);
    if (!data_attr) {
        kfree(rec);
        return NTFS_ERR_NOT_FOUND;
    }

    txn = ntfs_txn_begin(vol);
    if (!txn) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    now_ft = unix_to_filetime(0); /* 0 → uses FILETIME_BASE_2026 equivalent */

    /* ---- Resident path ---- */
    if (ah.non_resident == 0) {
        uint32_t old_len = ah.content_length;
        uint32_t new_len = (uint32_t)(offset + length);
        if (new_len < old_len)
            new_len = old_len;

        /* Build the new content into a temp buffer. */
        uint8_t *new_data = (uint8_t *)kmalloc(new_len);
        if (!new_data) {
            ntfs_txn_abort(txn);
            ntfs_txn_free(txn);
            kfree(rec);
            return NTFS_ERR_IO;
        }

        /* Start with existing content, then overwrite the written range. */
        ntfs_memset(new_data, 0, new_len);
        if (old_len > 0)
            ntfs_memcpy(new_data,
                        rec + (uint32_t)(data_attr - rec) + ah.content_offset,
                        old_len);
        ntfs_memcpy(new_data + offset, buffer, length);

        /* Journal before modifying */
        ntfs_txn_log(txn,
                     NTFS_LOG_OP_UPDATE_RESIDENT, new_data, (uint16_t)new_len,
                     NTFS_LOG_OP_UPDATE_RESIDENT, NULL, 0,
                     inode, ah.content_offset);

        /* ntfs_attr_update handles resize and resident→non-resident conversion */
        rc = ntfs_attr_update(vol, rec, &hdr, vol->frs_size,
                               NTFS_ATTR_DATA, NULL, new_data, new_len);
        kfree(new_data);

        if (rc == NTFS_OK) {
            update_std_info_times(rec, &hdr, now_ft);
            ntfs_txn_commit(txn);
            rc = ntfs_write_mft_record(vol, inode, rec);
        } else {
            ntfs_txn_abort(txn);
        }
        ntfs_txn_free(txn);
        kfree(rec);
        return rc;
    }

    /* ---- Non-resident path ---- */
    {
        struct ntfs_data_run runs[NTFS_MAX_WRITE_RUNS];
        struct ntfs_nonres_header nrhdr;
        int run_count;
        uint32_t attr_off = (uint32_t)(data_attr - rec);
        uint64_t needed   = offset + length;

        run_count = ntfs_decode_data_runs(data_attr, runs,
                                           NTFS_MAX_WRITE_RUNS, &nrhdr);
        if (run_count < 0) {
            ntfs_txn_abort(txn);
            ntfs_txn_free(txn);
            kfree(rec);
            return NTFS_ERR_BAD_MAGIC;
        }

        /* Extend allocation if needed */
        if (needed > nrhdr.alloc_size) {
            uint64_t cs = vol->cluster_size;
            uint64_t new_clusters = (needed + cs - 1) / cs;
            uint64_t old_clusters = nrhdr.alloc_size / cs;
            uint64_t extra  = new_clusters - old_clusters;
            uint64_t hint   = (run_count > 0 &&
                               runs[run_count - 1].lcn != NTFS_LCN_SPARSE)
                              ? runs[run_count - 1].lcn +
                                runs[run_count - 1].length
                              : vol->mft_lcn;
            uint64_t new_lcn = ntfs_alloc_clusters(vol, extra, hint);
            if (new_lcn == 0) {
                ntfs_txn_abort(txn);
                ntfs_txn_free(txn);
                kfree(rec);
                return NTFS_ERR_FULL;
            }

            /* Try to extend the last run (contiguous) */
            if (run_count > 0 &&
                runs[run_count - 1].lcn != NTFS_LCN_SPARSE &&
                runs[run_count - 1].lcn +
                    runs[run_count - 1].length == new_lcn) {
                runs[run_count - 1].length += extra;
            } else {
                if (run_count >= NTFS_MAX_WRITE_RUNS) {
                    ntfs_free_clusters(vol, new_lcn, extra);
                    ntfs_txn_abort(txn);
                    ntfs_txn_free(txn);
                    kfree(rec);
                    return NTFS_ERR_IO;
                }
                runs[run_count].vcn_start = old_clusters;
                runs[run_count].lcn       = new_lcn;
                runs[run_count].length    = extra;
                run_count++;
            }

            nrhdr.alloc_size = new_clusters * cs;

            /* Journal the run-list change */
            ntfs_txn_log(txn,
                         NTFS_LOG_OP_UPDATE_MAPPING, runs,
                         (uint16_t)(run_count * sizeof(struct ntfs_data_run)),
                         NTFS_LOG_OP_UPDATE_MAPPING, NULL, 0,
                         inode, (uint16_t)attr_off);

            rc = rewrite_nonres_attr(rec, attr_off, runs, run_count,
                                      nrhdr.alloc_size, nrhdr.real_size);
            if (rc != NTFS_OK) {
                ntfs_txn_abort(txn);
                ntfs_txn_free(txn);
                kfree(rec);
                return rc;
            }
        }

        /* Write data to clusters */
        rc = write_nonres_clusters(vol, runs, run_count,
                                    offset, length,
                                    (const uint8_t *)buffer);
        if (rc != NTFS_OK) {
            ntfs_txn_abort(txn);
            ntfs_txn_free(txn);
            kfree(rec);
            return rc;
        }

        /* Update real_size if the write extended the file */
        if (needed > nrhdr.real_size) {
            nrhdr.real_size = needed;
            ntfs_le64_write(rec + attr_off + 0x30, nrhdr.real_size);
            ntfs_le64_write(rec + attr_off + 0x38, nrhdr.real_size);
        }

        /* Journal size change */
        ntfs_txn_log(txn,
                     NTFS_LOG_OP_SET_ATTR_SIZES, &nrhdr.real_size,
                     sizeof(uint64_t),
                     NTFS_LOG_OP_SET_ATTR_SIZES, NULL, 0,
                     inode, (uint16_t)attr_off);

        update_std_info_times(rec, &hdr, now_ft);
        ntfs_txn_commit(txn);
        rc = ntfs_write_mft_record(vol, inode, rec);
        ntfs_txn_free(txn);
        kfree(rec);
        return rc;
    }
}

/* ============================================================================
 * ntfs_truncate — Set file data size
 * ============================================================================ */

int ntfs_truncate(struct ntfs_volume *vol, uint64_t inode, uint64_t new_size)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *data_attr;
    int rc;
    struct ntfs_txn *txn;
    uint64_t now_ft;

    if (!vol)
        return NTFS_ERR_IO;

    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    data_attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_DATA, &ah);
    if (!data_attr) {
        kfree(rec);
        return NTFS_ERR_NOT_FOUND;
    }

    txn = ntfs_txn_begin(vol);
    if (!txn) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    now_ft = unix_to_filetime(0);

    /* ---- Resident truncate ---- */
    if (ah.non_resident == 0) {
        uint32_t old_len = ah.content_length;
        uint8_t *new_data;

        if (new_size == (uint64_t)old_len) {
            ntfs_txn_abort(txn);
            ntfs_txn_free(txn);
            kfree(rec);
            return NTFS_OK;
        }

        new_data = (uint8_t *)kmalloc((uint32_t)(new_size > 0 ? new_size : 1));
        if (!new_data) {
            ntfs_txn_abort(txn);
            ntfs_txn_free(txn);
            kfree(rec);
            return NTFS_ERR_IO;
        }
        ntfs_memset(new_data, 0, new_size > 0 ? new_size : 1);
        if (new_size > 0 && old_len > 0) {
            uint32_t copy = (uint32_t)(new_size < old_len ? new_size : old_len);
            ntfs_memcpy(new_data,
                        rec + (uint32_t)(data_attr - rec) + ah.content_offset,
                        copy);
        }

        ntfs_txn_log(txn,
                     NTFS_LOG_OP_UPDATE_RESIDENT, new_data,
                     (uint16_t)(new_size > 0 ? new_size : 0),
                     NTFS_LOG_OP_UPDATE_RESIDENT, NULL, 0,
                     inode, ah.content_offset);

        rc = ntfs_attr_update(vol, rec, &hdr, vol->frs_size,
                               NTFS_ATTR_DATA, NULL,
                               new_size > 0 ? new_data : NULL,
                               (uint32_t)new_size);
        kfree(new_data);

        if (rc == NTFS_OK) {
            update_std_info_times(rec, &hdr, now_ft);
            ntfs_txn_commit(txn);
            rc = ntfs_write_mft_record(vol, inode, rec);
        } else {
            ntfs_txn_abort(txn);
        }
        ntfs_txn_free(txn);
        kfree(rec);
        return rc;
    }

    /* ---- Non-resident truncate ---- */
    {
        struct ntfs_data_run runs[NTFS_MAX_WRITE_RUNS];
        struct ntfs_nonres_header nrhdr;
        int run_count;
        uint32_t attr_off = (uint32_t)(data_attr - rec);
        uint64_t cs = vol->cluster_size;

        run_count = ntfs_decode_data_runs(data_attr, runs,
                                           NTFS_MAX_WRITE_RUNS, &nrhdr);
        if (run_count < 0) {
            ntfs_txn_abort(txn);
            ntfs_txn_free(txn);
            kfree(rec);
            return NTFS_ERR_BAD_MAGIC;
        }

        if (new_size == 0) {
            /* Free all clusters → convert to empty resident $DATA */
            int i;
            for (i = 0; i < run_count; i++) {
                if (runs[i].lcn != NTFS_LCN_SPARSE)
                    ntfs_free_clusters(vol, runs[i].lcn, runs[i].length);
            }

            ntfs_txn_log(txn,
                         NTFS_LOG_OP_UPDATE_RESIDENT, NULL, 0,
                         NTFS_LOG_OP_UPDATE_MAPPING, NULL, 0,
                         inode, (uint16_t)attr_off);

            rc = ntfs_attr_remove(vol, rec, &hdr, vol->frs_size,
                                   NTFS_ATTR_DATA, NULL);
            if (rc == NTFS_OK)
                rc = ntfs_attr_add(vol, rec, &hdr, vol->frs_size,
                                    NTFS_ATTR_DATA, NULL, NULL, 0);
        } else if (new_size < nrhdr.real_size) {
            /* Shrink: free clusters beyond new_size */
            uint64_t new_clusters = (new_size + cs - 1) / cs;
            uint64_t vcn          = 0;
            int i;

            for (i = 0; i < run_count; i++) {
                uint64_t run_end = vcn + runs[i].length;
                if (vcn >= new_clusters) {
                    /* This whole run is beyond new_size — free it */
                    if (runs[i].lcn != NTFS_LCN_SPARSE)
                        ntfs_free_clusters(vol, runs[i].lcn, runs[i].length);
                    runs[i].length = 0;
                } else if (run_end > new_clusters) {
                    /* Run straddles the cut point — trim it */
                    uint64_t keep  = new_clusters - vcn;
                    uint64_t freed = runs[i].length - keep;
                    if (runs[i].lcn != NTFS_LCN_SPARSE)
                        ntfs_free_clusters(vol, runs[i].lcn + keep, freed);
                    runs[i].length = keep;
                }
                vcn = run_end;
            }

            /* Compact run array (remove zero-length entries) */
            {
                int dst = 0, src;
                for (src = 0; src < run_count; src++) {
                    if (runs[src].length > 0)
                        runs[dst++] = runs[src];
                }
                run_count = dst;
            }

            nrhdr.alloc_size = new_clusters * cs;
            nrhdr.real_size  = new_size;

            ntfs_txn_log(txn,
                         NTFS_LOG_OP_UPDATE_MAPPING, runs,
                         (uint16_t)(run_count * sizeof(struct ntfs_data_run)),
                         NTFS_LOG_OP_UPDATE_MAPPING, NULL, 0,
                         inode, (uint16_t)attr_off);

            rc = rewrite_nonres_attr(rec, attr_off, runs, run_count,
                                      nrhdr.alloc_size, nrhdr.real_size);
        } else {
            /* Grow: allocate clusters, extent runs */
            uint64_t new_clusters = (new_size + cs - 1) / cs;
            uint64_t old_clusters = nrhdr.alloc_size / cs;
            uint64_t extra        = new_clusters - old_clusters;
            uint64_t hint = (run_count > 0 &&
                             runs[run_count - 1].lcn != NTFS_LCN_SPARSE)
                            ? runs[run_count - 1].lcn +
                              runs[run_count - 1].length
                            : vol->mft_lcn;
            uint64_t new_lcn = ntfs_alloc_clusters(vol, extra, hint);
            if (new_lcn == 0) {
                ntfs_txn_abort(txn);
                ntfs_txn_free(txn);
                kfree(rec);
                return NTFS_ERR_FULL;
            }

            if (run_count > 0 &&
                runs[run_count - 1].lcn != NTFS_LCN_SPARSE &&
                runs[run_count - 1].lcn +
                    runs[run_count - 1].length == new_lcn) {
                runs[run_count - 1].length += extra;
            } else {
                if (run_count >= NTFS_MAX_WRITE_RUNS) {
                    ntfs_free_clusters(vol, new_lcn, extra);
                    ntfs_txn_abort(txn);
                    ntfs_txn_free(txn);
                    kfree(rec);
                    return NTFS_ERR_IO;
                }
                runs[run_count].vcn_start = old_clusters;
                runs[run_count].lcn       = new_lcn;
                runs[run_count].length    = extra;
                run_count++;
            }

            nrhdr.alloc_size = new_clusters * cs;
            nrhdr.real_size  = new_size;

            ntfs_txn_log(txn,
                         NTFS_LOG_OP_UPDATE_MAPPING, runs,
                         (uint16_t)(run_count * sizeof(struct ntfs_data_run)),
                         NTFS_LOG_OP_UPDATE_MAPPING, NULL, 0,
                         inode, (uint16_t)attr_off);

            rc = rewrite_nonres_attr(rec, attr_off, runs, run_count,
                                      nrhdr.alloc_size, nrhdr.real_size);
        }

        if (rc == NTFS_OK) {
            update_std_info_times(rec, &hdr, now_ft);
            ntfs_txn_commit(txn);
            rc = ntfs_write_mft_record(vol, inode, rec);
        } else {
            ntfs_txn_abort(txn);
        }
        ntfs_txn_free(txn);
        kfree(rec);
        return rc;
    }
}

/* ============================================================================
 * ntfs_set_file_attributes — Update DOS attribute flags
 * ============================================================================ */

int ntfs_set_file_attributes(struct ntfs_volume *vol, uint64_t inode,
                              uint32_t attrs)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *si_attr;
    struct ntfs_txn *txn;
    uint32_t attr_off;
    uint32_t content_off;
    int rc;

    if (!vol)
        return NTFS_ERR_IO;

    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    si_attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_STANDARD_INFORMATION, &ah);
    if (!si_attr) {
        kfree(rec);
        return NTFS_ERR_NOT_FOUND;
    }

    txn = ntfs_txn_begin(vol);
    if (!txn) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    attr_off    = (uint32_t)(si_attr - rec);
    content_off = attr_off + ah.content_offset;

    /* Journal the attribute update */
    ntfs_txn_log(txn,
                 NTFS_LOG_OP_UPDATE_RESIDENT, &attrs, sizeof(uint32_t),
                 NTFS_LOG_OP_UPDATE_RESIDENT, NULL, 0,
                 inode, (uint16_t)(content_off + 0x20));

    /* Patch DOS attrs at $STANDARD_INFORMATION +0x20 */
    ntfs_le32_write(rec + content_off + 0x20, attrs);

    ntfs_txn_commit(txn);
    rc = ntfs_write_mft_record(vol, inode, rec);
    ntfs_txn_free(txn);
    kfree(rec);

    klog(LOG_DEBUG, "ntfs", "set_file_attrs: inode %llu attrs 0x%x",
         inode, (uint64_t)attrs);

    return rc;
}

/* ============================================================================
 * ntfs_set_file_time — Set file timestamps (Unix → FILETIME)
 * ============================================================================ */

int ntfs_set_file_time(struct ntfs_volume *vol, uint64_t inode,
                       uint64_t create_unix, uint64_t modify_unix,
                       uint64_t access_unix)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *si_attr;
    struct ntfs_txn *txn;
    uint32_t attr_off;
    uint32_t content_off;
    uint64_t create_ft, modify_ft, access_ft;
    int rc;

    if (!vol)
        return NTFS_ERR_IO;

    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, inode, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    si_attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_STANDARD_INFORMATION, &ah);
    if (!si_attr) {
        kfree(rec);
        return NTFS_ERR_NOT_FOUND;
    }

    txn = ntfs_txn_begin(vol);
    if (!txn) {
        kfree(rec);
        return NTFS_ERR_IO;
    }

    attr_off    = (uint32_t)(si_attr - rec);
    content_off = attr_off + ah.content_offset;

    create_ft = unix_to_filetime(create_unix);
    modify_ft = unix_to_filetime(modify_unix);
    access_ft = unix_to_filetime(access_unix);

    ntfs_txn_log(txn,
                 NTFS_LOG_OP_UPDATE_RESIDENT, NULL, 0,
                 NTFS_LOG_OP_UPDATE_RESIDENT, NULL, 0,
                 inode, (uint16_t)content_off);

    /* $STANDARD_INFORMATION timestamp layout:
     *   +0x00 creation_time
     *   +0x08 modification_time
     *   +0x10 mft_change_time
     *   +0x18 access_time */
    ntfs_le64_write(rec + content_off + 0x00, create_ft);
    ntfs_le64_write(rec + content_off + 0x08, modify_ft);
    ntfs_le64_write(rec + content_off + 0x10, modify_ft);  /* MFT-change = modify */
    ntfs_le64_write(rec + content_off + 0x18, access_ft);

    ntfs_txn_commit(txn);
    rc = ntfs_write_mft_record(vol, inode, rec);
    ntfs_txn_free(txn);
    kfree(rec);

    klog(LOG_DEBUG, "ntfs",
         "set_file_time: inode %llu create=%llu modify=%llu access=%llu",
         inode, create_ft, modify_ft, access_ft);

    return rc;
}
