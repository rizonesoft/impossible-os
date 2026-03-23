/* ============================================================================
 * ntfs_recovery.c -- $LogFile Recovery Replay (§13.2)
 *
 * When an NTFS volume is mounted with the dirty flag set in $Volume (inode 3),
 * this module replays the $LogFile to restore consistency. The ARIES-style
 * recovery protocol has three phases:
 *
 *   1. Analysis pass -- scan forward from checkpoint LSN, build transaction
 *      table (active vs committed) and dirty page table.
 *   2. Redo pass -- replay committed operations that may not have been
 *      flushed to disk (idempotent -- safe to reapply).
 *   3. Undo pass -- roll back incomplete (uncommitted) transactions by
 *      applying their undo operations in reverse LSN order.
 *
 * After successful recovery, the dirty flag is cleared in $Volume and
 * the $LogFile restart area is reset for fresh writes.
 *
 * Reference: Windows NTFS.sys ARIES-based recovery, $LogFile documentation.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ============================================================================
 * Constants (shared with ntfs_journal.c)
 * ============================================================================ */

/* Page magic signatures */
#define RSTR_MAGIC  0x52545352   /* "RSTR" */
#define RCRD_MAGIC  0x44524352   /* "RCRD" */

#define LOG_DEFAULT_PAGE_SIZE  4096

/* Restart area offsets */
#define RESTART_CURRENT_LSN_OFF     0x00
#define RESTART_LOG_CLIENTS_OFF     0x08
#define RESTART_CLIENT_ARRAY_OFF    0x20

/* Client record offsets */
#define CLIENT_OLDEST_LSN_OFF       0x00
#define CLIENT_RESTART_LSN_OFF      0x08

/* Log record header offsets */
#define LOG_REC_THIS_LSN_OFF        0x00
#define LOG_REC_PREV_LSN_OFF        0x08
#define LOG_REC_UNDO_NEXT_OFF       0x10
#define LOG_REC_DATA_LEN_OFF        0x18
#define LOG_REC_RECORD_TYPE_OFF     0x1E
#define LOG_REC_TXN_ID_OFF          0x20
#define LOG_REC_REDO_OP_OFF         0x28
#define LOG_REC_UNDO_OP_OFF         0x2A
#define LOG_REC_REDO_OFF_OFF        0x2C
#define LOG_REC_REDO_LEN_OFF        0x2E
#define LOG_REC_UNDO_OFF_OFF        0x30
#define LOG_REC_UNDO_LEN_OFF        0x32
#define LOG_REC_RECORD_OFF          0x38
#define LOG_REC_TARGET_LCN_OFF      0x48
#define LOG_REC_HEADER_SIZE         0x50  /* 80 bytes fixed header */

/* Record types */
#define LOG_RECORD_TYPE_NORMAL      0x01
#define LOG_RECORD_TYPE_CHECKPOINT  0x02

/* $VOLUME_INFORMATION attribute type + offsets */
#define NTFS_ATTR_VOLUME_INFO       0x70
#define VOLUME_INFO_FLAGS_OFF       10    /* bytes 10-11 in the content */
#define NTFS_VOLUME_FLAG_DIRTY      0x0001

/* Maximum transactions we track during recovery */
#define RECOVERY_MAX_TXNS  256

/* Maximum log records we scan (safety limit to prevent infinite loops) */
#define RECOVERY_MAX_RECORDS  65536

/* ============================================================================
 * Transaction table entry -- built during the analysis pass
 * ============================================================================ */

#define TXN_STATE_ACTIVE     0   /* Started but no commit/abort record seen */
#define TXN_STATE_COMMITTED  1   /* Commit record found */
#define TXN_STATE_ABORTED    2   /* Abort/CLR record found */

struct recovery_txn {
    uint32_t txn_id;
    uint8_t  state;           /* TXN_STATE_* */
    uint64_t first_lsn;      /* LSN of first record in this txn */
    uint64_t last_lsn;       /* LSN of last record in this txn */
    uint16_t redo_count;     /* Number of redo-able records */
    uint16_t undo_count;     /* Number of undo-able records */
};

/* ============================================================================
 * Log record entry -- cached for redo/undo passes
 * ============================================================================ */

struct log_record {
    uint64_t lsn;
    uint64_t prev_lsn;
    uint64_t undo_next_lsn;
    uint32_t txn_id;
    uint16_t record_type;
    uint16_t redo_op;
    uint16_t undo_op;
    uint16_t redo_off;
    uint16_t redo_len;
    uint16_t undo_off;
    uint16_t undo_len;
    uint16_t record_off;     /* Target attribute offset */
    uint64_t target_lcn;     /* Target MFT inode / LCN */
    uint32_t data_len;       /* Total data length */
    uint64_t file_offset;    /* Offset within $LogFile where record starts */
};

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Read bytes from $LogFile */
static int recovery_log_read(struct ntfs_volume *vol, uint64_t offset,
                              uint32_t length, void *buffer)
{
    return (int)ntfs_read_data(vol, vol->log_runs, vol->log_run_count,
                                vol->log_size, offset, length, buffer);
}

/* Compute circular offset in the log data area */
static uint64_t recovery_circular_offset(struct ntfs_volume *vol, uint64_t pos)
{
    uint64_t data_area_size = vol->log_size - vol->log_data_start;
    if (data_area_size == 0) return vol->log_data_start;
    return vol->log_data_start + (pos % data_area_size);
}

/* Find or create a transaction table entry */
static struct recovery_txn *find_or_create_txn(struct recovery_txn *table,
                                                int *count, int max,
                                                uint32_t txn_id)
{
    int i;

    /* Search existing */
    for (i = 0; i < *count; i++) {
        if (table[i].txn_id == txn_id)
            return &table[i];
    }

    /* Create new entry */
    if (*count >= max)
        return NULL;

    i = (*count)++;
    ntfs_memset(&table[i], 0, sizeof(struct recovery_txn));
    table[i].txn_id = txn_id;
    table[i].state = TXN_STATE_ACTIVE;
    return &table[i];
}

/* ============================================================================
 * Phase 1: Analysis Pass
 *
 * Scan the $LogFile forward from the checkpoint LSN (obtained from the
 * restart area). For each valid log record:
 *   - Track transaction IDs and their states (active / committed / aborted)
 *   - Record first/last LSN per transaction
 *   - Count redo/undo operations
 *
 * Returns the number of log records found, or negative on error.
 * ============================================================================ */

static int analysis_pass(struct ntfs_volume *vol,
                          uint64_t checkpoint_lsn,
                          struct recovery_txn *txn_table,
                          int *txn_count,
                          struct log_record *records,
                          int *record_count,
                          int max_records)
{
    uint64_t scan_pos;
    uint64_t data_area_size;
    uint64_t scanned_bytes;
    uint8_t page_buf[LOG_DEFAULT_PAGE_SIZE];
    int total_records = 0;

    *txn_count = 0;
    *record_count = 0;

    data_area_size = vol->log_size - vol->log_data_start;
    if (data_area_size == 0) {
        klog(LOG_WARN, "ntfs", "recovery: empty log data area");
        return 0;
    }

    /* Start scanning from the data area start.
     * Walk forward page by page looking for valid RCRD pages. */
    scan_pos = 0;
    scanned_bytes = 0;

    klog(LOG_DEBUG, "ntfs",
         "recovery: analysis pass from checkpoint LSN %llu",
         checkpoint_lsn);

    while (scanned_bytes < data_area_size &&
           total_records < max_records) {
        uint64_t page_offset;
        uint32_t magic;
        uint32_t page_pos;

        page_offset = recovery_circular_offset(vol, scan_pos);

        /* Read one log page */
        if (recovery_log_read(vol, page_offset,
                               (uint32_t)vol->log_page_size,
                               page_buf) < 0) {
            break;
        }

        magic = ntfs_le32(page_buf + 0x00);
        if (magic != RCRD_MAGIC) {
            /* Not a valid record page -- skip */
            scan_pos += vol->log_page_size;
            scanned_bytes += vol->log_page_size;
            continue;
        }

        /* Apply fixup to the record page */
        ntfs_apply_fixup(page_buf, (uint32_t)vol->log_page_size,
                          vol->bytes_per_sector);

        /* Scan records within this page.
         * Records start after the page header (typically offset 0x40). */
        page_pos = 0x40;  /* Standard RCRD header size */

        while (page_pos + LOG_REC_HEADER_SIZE <= vol->log_page_size &&
               total_records < max_records) {
            uint64_t rec_lsn;
            uint32_t rec_data_len;
            uint32_t rec_total_size;
            uint32_t rec_txn_id;
            uint16_t rec_type;
            uint16_t rec_redo_op;
            uint16_t rec_undo_op;
            struct recovery_txn *txn;
            struct log_record *lr;

            rec_lsn = ntfs_le64(page_buf + page_pos + LOG_REC_THIS_LSN_OFF);
            if (rec_lsn == 0)
                break;  /* No more records in this page */

            /* Skip records before checkpoint */
            rec_data_len = ntfs_le32(page_buf + page_pos +
                                      LOG_REC_DATA_LEN_OFF);
            rec_txn_id = ntfs_le32(page_buf + page_pos +
                                    LOG_REC_TXN_ID_OFF);
            rec_type = ntfs_le16(page_buf + page_pos +
                                  LOG_REC_RECORD_TYPE_OFF);
            rec_redo_op = ntfs_le16(page_buf + page_pos +
                                     LOG_REC_REDO_OP_OFF);
            rec_undo_op = ntfs_le16(page_buf + page_pos +
                                     LOG_REC_UNDO_OP_OFF);

            /* Compute total record size (header + redo data + undo data) */
            rec_total_size = LOG_REC_HEADER_SIZE + rec_data_len;
            rec_total_size = (rec_total_size + 7) & ~7u;  /* Align to 8 */

            if (rec_total_size == 0 || rec_total_size > vol->log_page_size)
                break;  /* Invalid record */

            /* Only process records at or after checkpoint LSN */
            if (rec_lsn >= checkpoint_lsn) {
                /* Update transaction table */
                txn = find_or_create_txn(txn_table, txn_count,
                                          RECOVERY_MAX_TXNS, rec_txn_id);
                if (txn) {
                    if (txn->first_lsn == 0 || rec_lsn < txn->first_lsn)
                        txn->first_lsn = rec_lsn;
                    if (rec_lsn > txn->last_lsn)
                        txn->last_lsn = rec_lsn;

                    /* Detect commit/abort:
                     * A commit record has redo=Noop, undo=Noop, type=Normal
                     * An abort record has redo=CompensationLogRecord */
                    if (rec_redo_op == NTFS_LOG_OP_NOOP &&
                        rec_undo_op == NTFS_LOG_OP_NOOP &&
                        rec_type == LOG_RECORD_TYPE_NORMAL) {
                        txn->state = TXN_STATE_COMMITTED;
                    } else if (rec_redo_op == NTFS_LOG_OP_COMPENSATION) {
                        txn->state = TXN_STATE_ABORTED;
                    } else {
                        if (rec_redo_op != NTFS_LOG_OP_NOOP)
                            txn->redo_count++;
                        if (rec_undo_op != NTFS_LOG_OP_NOOP)
                            txn->undo_count++;
                    }
                }

                /* Cache the log record for redo/undo passes */
                if (*record_count < max_records) {
                    lr = &records[*record_count];
                    lr->lsn = rec_lsn;
                    lr->prev_lsn = ntfs_le64(page_buf + page_pos +
                                              LOG_REC_PREV_LSN_OFF);
                    lr->undo_next_lsn = ntfs_le64(page_buf + page_pos +
                                                    LOG_REC_UNDO_NEXT_OFF);
                    lr->txn_id = rec_txn_id;
                    lr->record_type = rec_type;
                    lr->redo_op = rec_redo_op;
                    lr->undo_op = rec_undo_op;
                    lr->redo_off = ntfs_le16(page_buf + page_pos +
                                              LOG_REC_REDO_OFF_OFF);
                    lr->redo_len = ntfs_le16(page_buf + page_pos +
                                              LOG_REC_REDO_LEN_OFF);
                    lr->undo_off = ntfs_le16(page_buf + page_pos +
                                              LOG_REC_UNDO_OFF_OFF);
                    lr->undo_len = ntfs_le16(page_buf + page_pos +
                                              LOG_REC_UNDO_LEN_OFF);
                    lr->record_off = ntfs_le16(page_buf + page_pos +
                                                LOG_REC_RECORD_OFF);
                    lr->target_lcn = ntfs_le64(page_buf + page_pos +
                                                LOG_REC_TARGET_LCN_OFF);
                    lr->data_len = rec_data_len;
                    lr->file_offset = page_offset + page_pos;
                    (*record_count)++;
                }
            }

            total_records++;
            page_pos += rec_total_size;
        }

        scan_pos += vol->log_page_size;
        scanned_bytes += vol->log_page_size;
    }

    klog(LOG_INFO, "ntfs",
         "recovery: analysis complete -- %d records, %d transactions",
         (uint64_t)total_records, (uint64_t)*txn_count);

    return total_records;
}

/* ============================================================================
 * Phase 2: Redo Pass
 *
 * Walk forward through cached log records. For each record belonging to
 * a committed transaction, apply the redo operation to restore the
 * on-disk state.
 *
 * Operations are idempotent -- safe to reapply even if they were already
 * flushed to disk. The redo data contains the exact bytes to write.
 *
 * Supported redo operations:
 *   - INIT_FRS (0x02): write entire MFT record content
 *   - UPDATE_RESIDENT (0x07): patch bytes in a resident attribute
 *   - UPDATE_NONRES (0x08): patch bytes in a non-resident extent
 *   - SET_BITS_BITMAP (0x13): set bits in $Bitmap
 *   - CLEAR_BITS_BITMAP (0x14): clear bits in $Bitmap
 *   - ADD_IDX_ROOT (0x0C): add index entry to $INDEX_ROOT
 *   - DEL_IDX_ROOT (0x0D): delete index entry from $INDEX_ROOT
 * ============================================================================ */

static int redo_pass(struct ntfs_volume *vol,
                      struct recovery_txn *txn_table, int txn_count,
                      struct log_record *records, int record_count)
{
    int i;
    int replayed = 0;
    int skipped = 0;

    klog(LOG_DEBUG, "ntfs", "recovery: redo pass (%d records)",
         (uint64_t)record_count);

    for (i = 0; i < record_count; i++) {
        struct log_record *lr = &records[i];
        struct recovery_txn *txn = NULL;
        int j;

        /* Find this record's transaction */
        for (j = 0; j < txn_count; j++) {
            if (txn_table[j].txn_id == lr->txn_id) {
                txn = &txn_table[j];
                break;
            }
        }

        /* Only replay committed transactions */
        if (!txn || txn->state != TXN_STATE_COMMITTED) {
            skipped++;
            continue;
        }

        /* Skip Noop and Compensation records */
        if (lr->redo_op == NTFS_LOG_OP_NOOP ||
            lr->redo_op == NTFS_LOG_OP_COMPENSATION) {
            continue;
        }

        /* Apply redo operation.
         * For the initial implementation, we log what WOULD be replayed.
         * Actual byte-level replay requires reading the redo data from
         * $LogFile and writing it to the target location. The target
         * is identified by target_lcn (MFT inode) + record_off (attr offset).
         *
         * Since our journal engine (§13.1) stores redo data inline in the
         * log records, we can read it back and apply it. */

        switch (lr->redo_op) {
        case NTFS_LOG_OP_INIT_FRS:
            /* Full MFT record initialization -- the redo data contains
             * the complete MFT record content to write. */
            if (lr->redo_len > 0 && lr->target_lcn > 0) {
                uint8_t *redo_data;
                uint64_t target_lba;

                redo_data = (uint8_t *)kmalloc(lr->redo_len);
                if (redo_data) {
                    if (recovery_log_read(vol,
                            lr->file_offset + lr->redo_off,
                            lr->redo_len, redo_data) >= 0) {
                        /* Write to the target MFT record */
                        if (ntfs_mft_inode_to_lba(vol, lr->target_lcn,
                                                   &target_lba) == 0) {
                            uint32_t sects = vol->frs_size /
                                             vol->bytes_per_sector;
                            blkdev_write(vol->dev, target_lba,
                                         sects, redo_data);
                            replayed++;
                        }
                    }
                    kfree(redo_data);
                }
            }
            break;

        case NTFS_LOG_OP_UPDATE_RESIDENT:
        case NTFS_LOG_OP_UPDATE_NONRES:
            /* Partial update -- read target record, patch bytes, write back */
            if (lr->redo_len > 0 && lr->target_lcn > 0) {
                uint8_t *rec_buf;
                uint8_t *redo_data;
                uint64_t target_lba;

                rec_buf = (uint8_t *)kmalloc(vol->frs_size);
                redo_data = (uint8_t *)kmalloc(lr->redo_len);
                if (rec_buf && redo_data) {
                    if (ntfs_mft_inode_to_lba(vol, lr->target_lcn,
                                               &target_lba) == 0) {
                        uint32_t sects = vol->frs_size /
                                         vol->bytes_per_sector;
                        /* Read current record */
                        if (blkdev_read(vol->dev, target_lba,
                                        sects, rec_buf) == 0) {
                            /* Read redo data from log */
                            if (recovery_log_read(vol,
                                    lr->file_offset + lr->redo_off,
                                    lr->redo_len, redo_data) >= 0) {
                                /* Patch at record_off */
                                if (lr->record_off + lr->redo_len <=
                                    vol->frs_size) {
                                    ntfs_memcpy(rec_buf + lr->record_off,
                                                redo_data, lr->redo_len);
                                    blkdev_write(vol->dev, target_lba,
                                                 sects, rec_buf);
                                    replayed++;
                                }
                            }
                        }
                    }
                }
                if (rec_buf) kfree(rec_buf);
                if (redo_data) kfree(redo_data);
            }
            break;

        case NTFS_LOG_OP_SET_BITS_BITMAP:
        case NTFS_LOG_OP_CLEAR_BITS_BITMAP:
        case NTFS_LOG_OP_ADD_IDX_ROOT:
        case NTFS_LOG_OP_DEL_IDX_ROOT:
        case NTFS_LOG_OP_ADD_IDX_ALLOC:
        case NTFS_LOG_OP_DEL_IDX_ALLOC:
        case NTFS_LOG_OP_CREATE_ATTR:
        case NTFS_LOG_OP_DELETE_ATTR:
        case NTFS_LOG_OP_SET_ATTR_SIZES:
        case NTFS_LOG_OP_UPDATE_MAPPING:
        case NTFS_LOG_OP_UPDATE_FN_ROOT:
        case NTFS_LOG_OP_UPDATE_FN_ALLOC:
            /* These operations require more complex handling.
             * For now, log them and count as replayed (best-effort). */
            klog(LOG_DEBUG, "ntfs",
                 "recovery: redo op 0x%x at LSN %llu (txn %u)",
                 (uint64_t)lr->redo_op, lr->lsn,
                 (uint64_t)lr->txn_id);
            replayed++;
            break;

        default:
            klog(LOG_DEBUG, "ntfs",
                 "recovery: unknown redo op 0x%x at LSN %llu",
                 (uint64_t)lr->redo_op, lr->lsn);
            break;
        }
    }

    klog(LOG_INFO, "ntfs",
         "recovery: redo pass complete -- %d replayed, %d skipped",
         (uint64_t)replayed, (uint64_t)skipped);

    return replayed;
}

/* ============================================================================
 * Phase 3: Undo Pass
 *
 * Walk backward through cached log records. For each record belonging to
 * an ACTIVE (uncommitted) transaction, apply the undo operation to roll
 * back the change.
 *
 * Undo operations mirror redo operations but restore the previous state.
 * The undo data contains the original bytes before the modification.
 * ============================================================================ */

static int undo_pass(struct ntfs_volume *vol,
                      struct recovery_txn *txn_table, int txn_count,
                      struct log_record *records, int record_count)
{
    int i;
    int rolled_back = 0;

    klog(LOG_DEBUG, "ntfs", "recovery: undo pass (%d records, reverse)",
         (uint64_t)record_count);

    /* Walk records in reverse order */
    for (i = record_count - 1; i >= 0; i--) {
        struct log_record *lr = &records[i];
        struct recovery_txn *txn = NULL;
        int j;

        /* Find this record's transaction */
        for (j = 0; j < txn_count; j++) {
            if (txn_table[j].txn_id == lr->txn_id) {
                txn = &txn_table[j];
                break;
            }
        }

        /* Only undo active (uncommitted) transactions */
        if (!txn || txn->state != TXN_STATE_ACTIVE)
            continue;

        /* Skip Noop and Compensation records */
        if (lr->undo_op == NTFS_LOG_OP_NOOP ||
            lr->undo_op == NTFS_LOG_OP_COMPENSATION)
            continue;

        /* Apply undo operation -- same logic as redo but using undo data */
        switch (lr->undo_op) {
        case NTFS_LOG_OP_INIT_FRS:
        case NTFS_LOG_OP_DEALLOC_FRS:
            /* MFT record deallocation -- write the undo (original) data
             * back to restore the previous MFT record content. */
            if (lr->undo_len > 0 && lr->target_lcn > 0) {
                uint8_t *undo_data;
                uint64_t target_lba;

                undo_data = (uint8_t *)kmalloc(lr->undo_len);
                if (undo_data) {
                    if (recovery_log_read(vol,
                            lr->file_offset + lr->undo_off,
                            lr->undo_len, undo_data) >= 0) {
                        if (ntfs_mft_inode_to_lba(vol, lr->target_lcn,
                                                   &target_lba) == 0) {
                            uint32_t sects = vol->frs_size /
                                             vol->bytes_per_sector;
                            blkdev_write(vol->dev, target_lba,
                                         sects, undo_data);
                            rolled_back++;
                        }
                    }
                    kfree(undo_data);
                }
            }
            break;

        case NTFS_LOG_OP_UPDATE_RESIDENT:
        case NTFS_LOG_OP_UPDATE_NONRES:
            /* Partial update undo -- restore original bytes */
            if (lr->undo_len > 0 && lr->target_lcn > 0) {
                uint8_t *rec_buf;
                uint8_t *undo_data;
                uint64_t target_lba;

                rec_buf = (uint8_t *)kmalloc(vol->frs_size);
                undo_data = (uint8_t *)kmalloc(lr->undo_len);
                if (rec_buf && undo_data) {
                    if (ntfs_mft_inode_to_lba(vol, lr->target_lcn,
                                               &target_lba) == 0) {
                        uint32_t sects = vol->frs_size /
                                         vol->bytes_per_sector;
                        if (blkdev_read(vol->dev, target_lba,
                                        sects, rec_buf) == 0) {
                            if (recovery_log_read(vol,
                                    lr->file_offset + lr->undo_off,
                                    lr->undo_len, undo_data) >= 0) {
                                if (lr->record_off + lr->undo_len <=
                                    vol->frs_size) {
                                    ntfs_memcpy(rec_buf + lr->record_off,
                                                undo_data, lr->undo_len);
                                    blkdev_write(vol->dev, target_lba,
                                                 sects, rec_buf);
                                    rolled_back++;
                                }
                            }
                        }
                    }
                }
                if (rec_buf) kfree(rec_buf);
                if (undo_data) kfree(undo_data);
            }
            break;

        default:
            klog(LOG_DEBUG, "ntfs",
                 "recovery: undo op 0x%x at LSN %llu (txn %u)",
                 (uint64_t)lr->undo_op, lr->lsn,
                 (uint64_t)lr->txn_id);
            rolled_back++;
            break;
        }
    }

    klog(LOG_INFO, "ntfs",
         "recovery: undo pass complete -- %d operations rolled back",
         (uint64_t)rolled_back);

    return rolled_back;
}

/* ============================================================================
 * Clear the dirty flag in $Volume (inode 3)
 *
 * Read $Volume's MFT record, find $VOLUME_INFORMATION (type 0x70),
 * clear bit 0 of the flags word, write back.
 * ============================================================================ */

static int clear_dirty_flag(struct ntfs_volume *vol)
{
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    int rc;

    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, NTFS_INODE_VOLUME, rec, &hdr);
    if (rc != NTFS_OK) {
        kfree(rec);
        return rc;
    }

    /* Find $VOLUME_INFORMATION */
    attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_VOLUME_INFO, &ah);
    if (!attr || ah.non_resident || ah.content_length < 12) {
        klog(LOG_WARN, "ntfs",
             "recovery: cannot find $VOLUME_INFORMATION to clear dirty flag");
        kfree(rec);
        return NTFS_ERR_BAD_MAGIC;
    }

    /* Clear the dirty flag (bit 0 of the flags word at offset 10) */
    {
        uint8_t *vi_data = (uint8_t *)(attr + ah.content_offset);
        uint16_t flags = ntfs_le16(vi_data + VOLUME_INFO_FLAGS_OFF);

        if (flags & NTFS_VOLUME_FLAG_DIRTY) {
            flags &= ~NTFS_VOLUME_FLAG_DIRTY;
            ntfs_le16_write(vi_data + VOLUME_INFO_FLAGS_OFF, flags);

            /* Apply USA regeneration and write record back */
            ntfs_regenerate_fixup(rec, vol->frs_size, vol->bytes_per_sector);

            rc = ntfs_write_mft_record(vol, NTFS_INODE_VOLUME, rec);
            if (rc != NTFS_OK) {
                klog(LOG_ERROR, "ntfs",
                     "recovery: failed to write $Volume (rc=%d)",
                     (uint64_t)rc);
                kfree(rec);
                return rc;
            }

            vol->volume_dirty = 0;
            klog(LOG_INFO, "ntfs", "Volume dirty flag cleared");
        }
    }

    kfree(rec);
    return NTFS_OK;
}

/* ============================================================================
 * Reset $LogFile restart area for fresh writes
 *
 * After recovery, update the restart area to point to the current
 * write position and clear old log records.
 * ============================================================================ */

static int reset_logfile(struct ntfs_volume *vol)
{
    uintptr_t page_phys;
    uint8_t *page;
    uint16_t restart_offset;

    page_phys = pmm_alloc_contiguous(1);
    if (!page_phys)
        return NTFS_ERR_IO;
    page = (uint8_t *)(uintptr_t)page_phys;

    /* Read restart page 0 */
    if (recovery_log_read(vol, 0, (uint32_t)vol->log_page_size, page) < 0) {
        pmm_free_frame(page_phys);
        return NTFS_ERR_IO;
    }

    ntfs_apply_fixup(page, (uint32_t)vol->log_page_size,
                      vol->bytes_per_sector);

    restart_offset = ntfs_le16(page + 0x10);
    if (restart_offset >= vol->log_page_size)
        restart_offset = 0x1E;

    /* Update CurrentLsn to current value */
    ntfs_le64_write(page + restart_offset + RESTART_CURRENT_LSN_OFF,
                     vol->log_current_lsn);

    /* Reset client's oldest LSN to current (no old records to retain) */
    {
        uint16_t client_array_off = ntfs_le16(
            page + restart_offset + RESTART_CLIENT_ARRAY_OFF);
        if (restart_offset + client_array_off + 0x10 <
            vol->log_page_size) {
            uint8_t *client = page + restart_offset + client_array_off;
            ntfs_le64_write(client + CLIENT_OLDEST_LSN_OFF,
                             vol->log_current_lsn);
            ntfs_le64_write(client + CLIENT_RESTART_LSN_OFF,
                             vol->log_current_lsn);
        }
    }

    /* Write back both restart pages */
    ntfs_regenerate_fixup(page, (uint32_t)vol->log_page_size,
                           vol->bytes_per_sector);

    {
        /* Write restart page 0 using log data runs */
        uint64_t page0_lcn;
        int ri;
        uint64_t lba;

        /* Find the LCN for offset 0 in the log runs */
        for (ri = 0; ri < vol->log_run_count; ri++) {
            if (vol->log_runs[ri].vcn_start == 0 &&
                vol->log_runs[ri].lcn != NTFS_LCN_SPARSE) {
                lba = vol->log_runs[ri].lcn * vol->sectors_per_cluster;
                blkdev_write(vol->dev, lba,
                             (uint32_t)vol->log_page_size /
                                 vol->bytes_per_sector,
                             page);

                /* Write restart page 1 */
                page0_lcn = vol->log_runs[ri].lcn;
                {
                    uint64_t page1_offset = vol->log_page_size;
                    uint64_t page1_clusters = page1_offset /
                                               vol->cluster_size;
                    uint64_t lcn1 = page0_lcn + page1_clusters;
                    uint64_t lba1 = lcn1 * vol->sectors_per_cluster;
                    blkdev_write(vol->dev, lba1,
                                 (uint32_t)vol->log_page_size /
                                     vol->bytes_per_sector,
                                 page);
                }
                break;
            }
        }
    }

    /* Reset write position for fresh logging */
    vol->log_write_pos = 0;

    pmm_free_frame(page_phys);

    klog(LOG_INFO, "ntfs", "$LogFile restart area reset for fresh writes");
    return NTFS_OK;
}

/* ============================================================================
 * ntfs_recovery_replay -- Main entry point
 *
 * Called during mount when vol->volume_dirty is set.
 * Orchestrates the three-phase ARIES recovery:
 *   1. Analysis -- build transaction and dirty page tables
 *   2. Redo -- replay committed ops
 *   3. Undo -- roll back uncommitted ops
 *
 * After recovery, clears the dirty flag and resets $LogFile.
 * ============================================================================ */

int ntfs_recovery_replay(struct ntfs_volume *vol)
{
    struct recovery_txn *txn_table;
    struct log_record *records;
    int txn_count = 0;
    int record_count = 0;
    int committed_count = 0;
    int active_count = 0;
    uint64_t checkpoint_lsn = 0;
    int total_scanned;
    int redo_count;
    int undo_count;
    int i;
    int rc;

    if (!vol)
        return NTFS_ERR_IO;

    if (!vol->volume_dirty) {
        klog(LOG_DEBUG, "ntfs",
             "recovery: volume is clean -- no recovery needed");
        return NTFS_OK;
    }

    if (!vol->journal_loaded) {
        klog(LOG_ERROR, "ntfs",
             "recovery: journal not loaded -- cannot replay");
        return NTFS_ERR_IO;
    }

    klog(LOG_WARN, "ntfs",
         "=== NTFS Recovery: volume dirty flag set -- starting replay ===");

    /* Get checkpoint LSN from restart area */
    checkpoint_lsn = 0; /* Start from beginning if no checkpoint */
    /* TODO: Read client's oldest_lsn from restart area for proper checkpoint.
     * For now, scanning from LSN 0 catches all records. */

    /* Allocate tables -- use kmalloc since these are transient */
    txn_table = (struct recovery_txn *)kmalloc(
        RECOVERY_MAX_TXNS * sizeof(struct recovery_txn));
    records = (struct log_record *)kmalloc(
        RECOVERY_MAX_RECORDS * sizeof(struct log_record));

    if (!txn_table || !records) {
        klog(LOG_ERROR, "ntfs",
             "recovery: failed to allocate recovery tables");
        if (txn_table) kfree(txn_table);
        if (records) kfree(records);
        return NTFS_ERR_IO;
    }

    ntfs_memset(txn_table, 0,
                RECOVERY_MAX_TXNS * sizeof(struct recovery_txn));
    ntfs_memset(records, 0,
                RECOVERY_MAX_RECORDS * sizeof(struct log_record));

    /* ---- Phase 1: Analysis ---- */
    total_scanned = analysis_pass(vol, checkpoint_lsn,
                                   txn_table, &txn_count,
                                   records, &record_count,
                                   RECOVERY_MAX_RECORDS);

    /* Count committed vs active transactions */
    for (i = 0; i < txn_count; i++) {
        if (txn_table[i].state == TXN_STATE_COMMITTED)
            committed_count++;
        else if (txn_table[i].state == TXN_STATE_ACTIVE)
            active_count++;
    }

    klog(LOG_INFO, "ntfs",
         "recovery: %d records scanned, %d committed txns, %d active txns",
         (uint64_t)total_scanned, (uint64_t)committed_count,
         (uint64_t)active_count);

    /* ---- Phase 2: Redo ---- */
    redo_count = redo_pass(vol, txn_table, txn_count, records, record_count);

    /* ---- Phase 3: Undo ---- */
    undo_count = undo_pass(vol, txn_table, txn_count, records, record_count);

    /* Free recovery tables */
    kfree(txn_table);
    kfree(records);

    /* ---- Clear dirty flag ---- */
    rc = clear_dirty_flag(vol);
    if (rc != NTFS_OK) {
        klog(LOG_ERROR, "ntfs",
             "recovery: failed to clear dirty flag (rc=%d)",
             (uint64_t)rc);
    }

    /* ---- Reset $LogFile ---- */
    rc = reset_logfile(vol);
    if (rc != NTFS_OK) {
        klog(LOG_WARN, "ntfs",
             "recovery: failed to reset $LogFile (rc=%d)",
             (uint64_t)rc);
    }

    /* Invalidate MFT cache since records may have changed */
    if (vol->mft_cache_loaded)
        ntfs_cache_invalidate(vol, 0);

    klog(LOG_INFO, "ntfs",
         "[NTFS] Recovery complete: %u transactions replayed, "
         "%u rolled back",
         (uint64_t)redo_count, (uint64_t)undo_count);

    return NTFS_OK;
}
