/* ============================================================================
 * ntfs_journal.c — $LogFile Journal Engine (§13.1)
 *
 * Implements write-ahead transaction logging via NTFS's $LogFile (inode 2).
 * Every metadata modification (MFT record, bitmap, index) is wrapped in a
 * transaction: begin → log redo/undo pairs → commit.
 *
 * The $LogFile is a circular buffer of log records:
 *   - Two redundant restart pages (RSTR) at offset 0 and 0x1000
 *   - Log record pages (RCRD) fill the remainder
 *   - Each record: LSN, redo op/data, undo op/data, target info
 *
 * Write-ahead logging (WAL) protocol:
 *   1. Write log record to $LogFile
 *   2. Flush $LogFile to disk (write barrier)
 *   3. THEN write actual metadata to disk
 *   → If power lost after step 1 but before step 3, recovery redoes the op
 *   → If power lost before step 1, the op never happened
 *
 * Recovery replay is in §13.2 (separate file).
 *
 * Reference: Microsoft NTFS.sys behavior, $LogFile documentation (unofficial).
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ============================================================================
 * On-disk constants
 * ============================================================================ */

/* Page magic signatures */
#define RSTR_MAGIC  0x52545352   /* "RSTR" — Restart page */
#define RCRD_MAGIC  0x44524352   /* "RCRD" — Record page */

/* Default log page size (matches system page size in most NTFS volumes) */
#define LOG_DEFAULT_PAGE_SIZE  4096

/* Restart area offsets within the restart page (after USA) */
#define RESTART_CURRENT_LSN_OFF     0x00
#define RESTART_LOG_CLIENTS_OFF     0x08
#define RESTART_CLIENT_FREE_OFF     0x0A
#define RESTART_CLIENT_INUSE_OFF    0x0C
#define RESTART_FLAGS_OFF           0x0E
#define RESTART_SEQ_BITS_OFF        0x10
#define RESTART_LOG_FILE_SIZE_OFF   0x18
#define RESTART_CLIENT_ARRAY_OFF    0x20
#define RESTART_FILE_SIZE_OFF       0x28
#define RESTART_LAST_LSN_DATA_LEN   0x30
#define RESTART_RECORD_HDR_LEN      0x38
#define RESTART_LOG_PAGE_DATA_OFF   0x40
#define RESTART_LOG_AREA_LENGTH     0x48  /* Added: length of logging area */

/* Client record offsets (relative to client array start) */
#define CLIENT_OLDEST_LSN_OFF       0x00
#define CLIENT_RESTART_LSN_OFF      0x08
#define CLIENT_PREV_CLIENT_OFF      0x10
#define CLIENT_NEXT_CLIENT_OFF      0x12
#define CLIENT_SEQ_NUMBER_OFF       0x14
#define CLIENT_NAME_LEN_OFF         0x1C  /* in bytes */
#define CLIENT_NAME_OFF             0x20  /* UTF-16LE "NTFS" */

/* Log record page header offsets (after magic + USA) */
#define RCRD_LAST_LSN_OFF           0x08
#define RCRD_FLAGS_OFF              0x10
#define RCRD_PAGE_COUNT_OFF         0x12
#define RCRD_PAGE_POSITION_OFF      0x14
#define RCRD_NEXT_REC_OFF           0x18

/* Log record structure offsets (within page, after page header) */
#define LOG_REC_THIS_LSN_OFF        0x00
#define LOG_REC_PREV_LSN_OFF        0x08
#define LOG_REC_UNDO_NEXT_OFF       0x10
#define LOG_REC_DATA_LEN_OFF        0x18
#define LOG_REC_CLIENT_ID_OFF       0x1C
#define LOG_REC_RECORD_TYPE_OFF     0x1E
#define LOG_REC_TXN_ID_OFF          0x20
#define LOG_REC_FLAGS_OFF           0x24
#define LOG_REC_REDO_OP_OFF         0x28
#define LOG_REC_UNDO_OP_OFF         0x2A
#define LOG_REC_REDO_OFF_OFF        0x2C
#define LOG_REC_REDO_LEN_OFF        0x2E
#define LOG_REC_UNDO_OFF_OFF        0x30
#define LOG_REC_UNDO_LEN_OFF        0x32
#define LOG_REC_TARGET_ATTR_OFF     0x34
#define LOG_REC_LCNS_FOLLOW_OFF     0x36
#define LOG_REC_RECORD_OFF          0x38
#define LOG_REC_ATTR_OFF            0x3A
#define LOG_REC_MFT_CLUSTER_IDX     0x3C
#define LOG_REC_TARGET_VCN_OFF      0x40
#define LOG_REC_TARGET_LCN_OFF      0x48
#define LOG_REC_HEADER_SIZE         0x50  /* 80 bytes fixed header */

/* Record types */
#define LOG_RECORD_TYPE_NORMAL      0x01
#define LOG_RECORD_TYPE_CHECKPOINT  0x02

/* Max redo/undo data per record — keeps records within one page */
#define LOG_MAX_DATA_SIZE  3900

/* Max open transactions */
#define NTFS_MAX_TRANSACTIONS  16

/* ============================================================================
 * Internal helpers
 * ============================================================================ */



/* Read a range of bytes from the $LogFile via its data runs.
 * offset: byte offset within $LogFile.
 * length: bytes to read.
 * buffer: output buffer. */
static int log_read(struct ntfs_volume *vol, uint64_t offset,
                     uint32_t length, void *buffer)
{
    return (int)ntfs_read_data(vol, vol->log_runs, vol->log_run_count,
                                vol->log_size, offset, length, buffer);
}

/* Write a range of bytes to the $LogFile via its data runs.
 * Works by reading the containing cluster(s), patching, and writing back. */
static int log_write(struct ntfs_volume *vol, uint64_t offset,
                      uint32_t length, const void *data)
{
    uint64_t cluster_size = vol->cluster_size;
    uint64_t start_cluster_off = (offset / cluster_size) * cluster_size;
    uint32_t patch_off = (uint32_t)(offset - start_cluster_off);
    uint32_t total_bytes = patch_off + length;
    uint32_t clusters_needed = (total_bytes + (uint32_t)cluster_size - 1) /
                                (uint32_t)cluster_size;
    uint32_t bounce_size = clusters_needed * (uint32_t)cluster_size;
    uint8_t *bounce;
    int i;
    uint64_t vcn_start;
    uint64_t run_vcn_end;

    bounce = (uint8_t *)kmalloc(bounce_size);
    if (!bounce)
        return -1;

    /* Read existing cluster content */
    if (ntfs_read_data(vol, vol->log_runs, vol->log_run_count,
                        vol->log_size, start_cluster_off,
                        bounce_size, bounce) < 0) {
        kfree(bounce);
        return -1;
    }

    /* Patch in the new data */
    ntfs_memcpy(bounce + patch_off, data, length);

    /* Write back */
    vcn_start = start_cluster_off / cluster_size;
    for (i = 0; i < vol->log_run_count; i++) {
        run_vcn_end = vol->log_runs[i].vcn_start + vol->log_runs[i].length;
        if (vcn_start >= vol->log_runs[i].vcn_start &&
            vcn_start < run_vcn_end) {
            uint64_t vcn_in_run = vcn_start - vol->log_runs[i].vcn_start;
            uint64_t disk_lcn = vol->log_runs[i].lcn + vcn_in_run;
            uint64_t lba = disk_lcn * vol->sectors_per_cluster;
            uint32_t sects = bounce_size / vol->bytes_per_sector;

            if (vol->log_runs[i].lcn == NTFS_LCN_SPARSE) {
                kfree(bounce);
                return -1;
            }

            if (blkdev_write(vol->dev, lba, sects, bounce) != 0) {
                kfree(bounce);
                return -1;
            }
            kfree(bounce);
            return 0;
        }
    }

    kfree(bounce);
    return -1;
}

/* Compute the circular file offset for a given write position.
 * Wraps around the log data area (after restart pages). */
static uint64_t log_circular_offset(struct ntfs_volume *vol, uint64_t pos)
{
    uint64_t data_area_size = vol->log_size - vol->log_data_start;
    if (data_area_size == 0) return vol->log_data_start;
    return vol->log_data_start + (pos % data_area_size);
}

/* ============================================================================
 * ntfs_journal_init — Read and parse $LogFile at mount time
 *
 * 1. Read $LogFile (inode 2) data runs
 * 2. Read first restart page (RSTR at offset 0)
 * 3. Parse restart area: CurrentLsn, log clients, seq bits
 * 4. Determine write position from CurrentLsn
 * ============================================================================ */

int ntfs_journal_init(struct ntfs_volume *vol)
{
    uintptr_t page_phys;
    uint8_t *page;
    uint8_t *rec;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header data_ah;
    const uint8_t *data_attr;
    struct ntfs_nonres_header nrhdr;
    uint32_t magic;
    uint16_t restart_offset;
    const uint8_t *restart;
    const uint8_t *client;
    uint32_t client_array_offset;
    int rc;

    if (!vol)
        return NTFS_ERR_IO;

    if (vol->journal_loaded)
        return NTFS_OK;

    klog(LOG_INFO, "ntfs", "Initializing $LogFile journal (inode 2)...");

    /* Read $LogFile MFT record */
    rec = (uint8_t *)kmalloc(vol->frs_size);
    if (!rec)
        return NTFS_ERR_IO;

    rc = ntfs_read_mft_record(vol, NTFS_INODE_LOGFILE, rec, &hdr);
    if (rc != NTFS_OK) {
        klog(LOG_ERROR, "ntfs", "journal: failed to read $LogFile MFT record");
        kfree(rec);
        return rc;
    }

    /* Get $DATA attribute → data runs */
    data_attr = ntfs_attr_find(rec, &hdr, NTFS_ATTR_DATA, &data_ah);
    if (!data_attr || !data_ah.non_resident) {
        klog(LOG_ERROR, "ntfs",
             "journal: $LogFile $DATA not found or resident");
        kfree(rec);
        return NTFS_ERR_BAD_MAGIC;
    }

    /* Decode data runs — allocate via kmalloc for the run array */
    {
        struct ntfs_data_run temp_runs[128];
        int run_count;
        uint32_t runs_bytes;

        run_count = ntfs_decode_data_runs(data_attr, temp_runs, 128, &nrhdr);
        if (run_count <= 0) {
            klog(LOG_ERROR, "ntfs", "journal: failed to decode $LogFile runs");
            kfree(rec);
            return NTFS_ERR_BAD_MAGIC;
        }

        runs_bytes = (uint32_t)(run_count * sizeof(struct ntfs_data_run));
        vol->log_runs = (struct ntfs_data_run *)kmalloc(runs_bytes);
        if (!vol->log_runs) {
            kfree(rec);
            return NTFS_ERR_IO;
        }
        ntfs_memcpy(vol->log_runs, temp_runs, runs_bytes);
        vol->log_run_count = run_count;
        vol->log_size = nrhdr.real_size;
    }

    kfree(rec);

    klog(LOG_DEBUG, "ntfs", "journal: $LogFile size = %llu bytes, %d runs",
         vol->log_size, (uint64_t)vol->log_run_count);

    /* Read the first restart page (4 KB at offset 0) */
    page_phys = pmm_alloc_contiguous(1);
    if (!page_phys) {
        kfree(vol->log_runs);
        vol->log_runs = NULL;
        return NTFS_ERR_IO;
    }
    page = (uint8_t *)(uintptr_t)page_phys;

    if (log_read(vol, 0, LOG_DEFAULT_PAGE_SIZE, page) < 0) {
        klog(LOG_ERROR, "ntfs", "journal: failed to read restart page 0");
        pmm_free_frame(page_phys);
        kfree(vol->log_runs);
        vol->log_runs = NULL;
        return NTFS_ERR_IO;
    }

    /* Validate RSTR magic */
    magic = ntfs_le32(page + 0x00);
    if (magic != RSTR_MAGIC) {
        /* Try second restart page at offset 0x1000 */
        if (log_read(vol, 0x1000, LOG_DEFAULT_PAGE_SIZE, page) < 0 ||
            ntfs_le32(page + 0x00) != RSTR_MAGIC) {
            klog(LOG_WARN, "ntfs",
                 "journal: no valid RSTR page — $LogFile may be empty");
            /* Initialize defaults for a fresh volume */
            vol->log_page_size = LOG_DEFAULT_PAGE_SIZE;
            vol->log_current_lsn = 0;
            vol->log_data_start = 2 * LOG_DEFAULT_PAGE_SIZE;
            vol->log_write_pos = 0;
            vol->log_seq_bits = 0;
            /* spinlock already zeroed (unlocked) from kmalloc */
            vol->journal_loaded = 1;
            pmm_free_frame(page_phys);
            return NTFS_OK;
        }
    }

    /* Apply fixup to restart page */
    rc = ntfs_apply_fixup(page, LOG_DEFAULT_PAGE_SIZE, vol->bytes_per_sector);
    if (rc != NTFS_OK) {
        klog(LOG_WARN, "ntfs", "journal: restart page fixup failed (ignoring)");
    }

    /* Parse restart page header */
    vol->log_page_size = ntfs_le32(page + 0x0C);  /* LogPageSize at 0x0C */

    if (vol->log_page_size == 0)
        vol->log_page_size = LOG_DEFAULT_PAGE_SIZE;

    /* RestartOffset — where the LFS_RESTART_AREA starts within the page */
    restart_offset = ntfs_le16(page + 0x10);  /* RestartOffset at 0x10 */
    if (restart_offset < 0x1E || restart_offset >= LOG_DEFAULT_PAGE_SIZE) {
        klog(LOG_WARN, "ntfs", "journal: invalid restart offset %u",
             (uint64_t)restart_offset);
        restart_offset = 0x1E;
    }

    restart = page + restart_offset;

    /* Parse LFS_RESTART_AREA */
    vol->log_current_lsn = ntfs_le64(restart + RESTART_CURRENT_LSN_OFF);
    vol->log_seq_bits = ntfs_le32(restart + RESTART_SEQ_BITS_OFF);

    /* Determine log data area start (after two restart pages) */
    vol->log_data_start = 2 * vol->log_page_size;

    /* Set write position after the current LSN position.
     * For now, calculate based on the log page data offset field. */
    {
        uint16_t log_clients = ntfs_le16(restart + RESTART_LOG_CLIENTS_OFF);
        uint16_t flags = ntfs_le16(restart + RESTART_FLAGS_OFF);

        klog(LOG_DEBUG, "ntfs",
             "journal: CurrentLSN = %llu, Clients = %u, Flags = 0x%x",
             vol->log_current_lsn, (uint64_t)log_clients, (uint64_t)flags);

        /* Parse client record (NTFS always has exactly 1 client) */
        if (log_clients >= 1) {
            client_array_offset = ntfs_le16(restart + RESTART_CLIENT_ARRAY_OFF);
            if (restart_offset + client_array_offset + 0x28 <
                LOG_DEFAULT_PAGE_SIZE) {
                client = restart + client_array_offset;
                {
                    uint64_t oldest_lsn = ntfs_le64(
                        client + CLIENT_OLDEST_LSN_OFF);
                    uint64_t restart_lsn = ntfs_le64(
                        client + CLIENT_RESTART_LSN_OFF);

                    klog(LOG_DEBUG, "ntfs",
                         "journal: Client oldest LSN = %llu, "
                         "restart LSN = %llu",
                         oldest_lsn, restart_lsn);
                }
            }
        }

        (void)flags;
    }

    /* Initialize write position: start fresh after the data area start.
     * Recovery (§13.2) will calibrate this from actual log contents. */
    vol->log_write_pos = 0;

    /* spinlock is already zeroed (unlocked) from kmalloc */
    vol->journal_loaded = 1;

    pmm_free_frame(page_phys);

    klog(LOG_INFO, "ntfs",
         "Journal ready: LSN %llu, page size %u, data start 0x%x",
         vol->log_current_lsn, vol->log_page_size, vol->log_data_start);

    return NTFS_OK;
}

/* ============================================================================
 * Transaction management
 * ============================================================================ */

/* Allocate a new transaction context. */
struct ntfs_txn *ntfs_txn_begin(struct ntfs_volume *vol)
{
    struct ntfs_txn *txn;

    if (!vol || !vol->journal_loaded)
        return NULL;

    txn = (struct ntfs_txn *)kmalloc(sizeof(struct ntfs_txn));
    if (!txn)
        return NULL;

    ntfs_memset(txn, 0, sizeof(struct ntfs_txn));
    txn->vol = vol;
    txn->start_lsn = vol->log_current_lsn;
    txn->last_lsn = 0;
    txn->record_count = 0;
    txn->committed = 0;
    txn->aborted = 0;

    /* Assign transaction ID (simple incrementing counter) */
    spin_lock(&vol->log_lock);
    txn->txn_id = ++(vol->log_next_txn_id);
    spin_unlock(&vol->log_lock);

    klog(LOG_DEBUG, "ntfs", "txn_begin: txn %u started at LSN %llu",
         (uint64_t)txn->txn_id, txn->start_lsn);

    return txn;
}

/* Write a log record to $LogFile.
 * This is the core WAL primitive — records are written to the circular
 * $LogFile buffer BEFORE metadata is modified on disk.
 *
 * Parameters:
 *   txn: transaction context
 *   redo_op: operation code for redo (NTFS_LOG_OP_*)
 *   redo_data: data payload for redo (NULL if none)
 *   redo_len: length of redo_data
 *   undo_op: operation code for undo
 *   undo_data: data payload for undo (NULL if none)
 *   undo_len: length of undo_data
 *   target_mft: MFT inode of the target record (or 0)
 *   target_attr_off: attribute offset within the target record
 *
 * Returns: the new LSN, or 0 on failure. */
uint64_t ntfs_txn_log(struct ntfs_txn *txn,
                       uint16_t redo_op, const void *redo_data,
                       uint16_t redo_len,
                       uint16_t undo_op, const void *undo_data,
                       uint16_t undo_len,
                       uint64_t target_mft, uint16_t target_attr_off)
{
    struct ntfs_volume *vol;
    uint8_t record[LOG_DEFAULT_PAGE_SIZE];
    uint32_t data_off;
    uint32_t total_data_len;
    uint32_t record_size;
    uint64_t new_lsn;
    uint64_t write_offset;

    if (!txn || !txn->vol || txn->committed || txn->aborted)
        return 0;

    vol = txn->vol;

    if (redo_len > LOG_MAX_DATA_SIZE || undo_len > LOG_MAX_DATA_SIZE)
        return 0;

    spin_lock(&vol->log_lock);

    /* Allocate new LSN */
    new_lsn = vol->log_current_lsn + 1;

    /* Build the log record header */
    ntfs_memset(record, 0, LOG_REC_HEADER_SIZE);

    /* This LSN */
    ntfs_le64_write(record + LOG_REC_THIS_LSN_OFF, new_lsn);

    /* Previous client LSN (for undo chain walking) */
    ntfs_le64_write(record + LOG_REC_PREV_LSN_OFF, txn->last_lsn);

    /* Undo-next LSN (for CLR — same as previous for normal records) */
    ntfs_le64_write(record + LOG_REC_UNDO_NEXT_OFF, txn->last_lsn);

    /* Client data length (everything from redo_op onward) */
    data_off = LOG_REC_REDO_OP_OFF;
    total_data_len = (LOG_REC_HEADER_SIZE - data_off) + redo_len + undo_len;
    ntfs_le32_write(record + LOG_REC_DATA_LEN_OFF, total_data_len);

    /* Client ID (always 0 for NTFS — single client) */
    ntfs_le16_write(record + LOG_REC_CLIENT_ID_OFF, 0);

    /* Record type */
    ntfs_le16_write(record + LOG_REC_RECORD_TYPE_OFF,
                     LOG_RECORD_TYPE_NORMAL);

    /* Transaction ID */
    ntfs_le32_write(record + LOG_REC_TXN_ID_OFF, txn->txn_id);

    /* Flags (0 for normal record) */
    ntfs_le16_write(record + LOG_REC_FLAGS_OFF, 0);

    /* Redo operation */
    ntfs_le16_write(record + LOG_REC_REDO_OP_OFF, redo_op);

    /* Undo operation */
    ntfs_le16_write(record + LOG_REC_UNDO_OP_OFF, undo_op);

    /* Redo data offset and length */
    ntfs_le16_write(record + LOG_REC_REDO_OFF_OFF, LOG_REC_HEADER_SIZE);
    ntfs_le16_write(record + LOG_REC_REDO_LEN_OFF, redo_len);

    /* Undo data offset and length */
    ntfs_le16_write(record + LOG_REC_UNDO_OFF_OFF,
                     (uint16_t)(LOG_REC_HEADER_SIZE + redo_len));
    ntfs_le16_write(record + LOG_REC_UNDO_LEN_OFF, undo_len);

    /* Target attribute / record info */
    ntfs_le16_write(record + LOG_REC_TARGET_ATTR_OFF, 0);
    ntfs_le16_write(record + LOG_REC_LCNS_FOLLOW_OFF, 0);
    ntfs_le16_write(record + LOG_REC_RECORD_OFF, target_attr_off);
    ntfs_le16_write(record + LOG_REC_ATTR_OFF, 0);
    ntfs_le16_write(record + LOG_REC_MFT_CLUSTER_IDX, 0);
    ntfs_le64_write(record + LOG_REC_TARGET_VCN_OFF, 0);
    ntfs_le64_write(record + LOG_REC_TARGET_LCN_OFF, target_mft);

    /* Append redo data */
    if (redo_data && redo_len > 0)
        ntfs_memcpy(record + LOG_REC_HEADER_SIZE, redo_data, redo_len);

    /* Append undo data */
    if (undo_data && undo_len > 0)
        ntfs_memcpy(record + LOG_REC_HEADER_SIZE + redo_len,
                    undo_data, undo_len);

    record_size = LOG_REC_HEADER_SIZE + redo_len + undo_len;
    /* Align to 8 bytes */
    record_size = (record_size + 7) & ~7u;

    /* Calculate circular write offset */
    write_offset = log_circular_offset(vol, vol->log_write_pos);

    /* Write the log record to $LogFile */
    if (log_write(vol, write_offset, record_size, record) != 0) {
        klog(LOG_ERROR, "ntfs",
             "txn_log: failed to write log record at offset %llu",
             write_offset);
        spin_unlock(&vol->log_lock);
        return 0;
    }

    /* Advance write position */
    vol->log_write_pos += record_size;
    vol->log_current_lsn = new_lsn;

    spin_unlock(&vol->log_lock);

    /* Update transaction state */
    txn->last_lsn = new_lsn;
    txn->record_count++;

    klog(LOG_DEBUG, "ntfs",
         "txn_log: txn %u, LSN %llu, redo=0x%x undo=0x%x (%u+%u bytes)",
         (uint64_t)txn->txn_id, new_lsn,
         (uint64_t)redo_op, (uint64_t)undo_op,
         (uint64_t)redo_len, (uint64_t)undo_len);

    return new_lsn;
}

/* ============================================================================
 * ntfs_txn_commit — Commit a transaction
 *
 * 1. Write a commit record (ForgetTransaction) to $LogFile
 * 2. Update restart area with new CurrentLsn
 * 3. Transaction is now durable — metadata writes can proceed
 * ============================================================================ */

int ntfs_txn_commit(struct ntfs_txn *txn)
{
    struct ntfs_volume *vol;
    uint64_t commit_lsn;

    if (!txn || !txn->vol)
        return NTFS_ERR_IO;

    if (txn->committed || txn->aborted)
        return NTFS_ERR_IO;

    vol = txn->vol;

    /* Write a commit record — ForgetTransaction (redo=Noop, undo=Noop) */
    commit_lsn = ntfs_txn_log(txn,
                               NTFS_LOG_OP_NOOP, NULL, 0,
                               NTFS_LOG_OP_NOOP, NULL, 0,
                               0, 0);
    if (commit_lsn == 0) {
        klog(LOG_ERROR, "ntfs", "txn_commit: failed to write commit record");
        return NTFS_ERR_IO;
    }

    /* Update the restart area with the new CurrentLsn.
     * Read restart page, update CurrentLsn, write back. */
    {
        uintptr_t page_phys = pmm_alloc_contiguous(1);
        uint8_t *page;
        uint16_t restart_offset;
        int rc;

        if (!page_phys)
            return NTFS_ERR_IO;
        page = (uint8_t *)(uintptr_t)page_phys;

        /* Read restart page 0 */
        if (log_read(vol, 0, (uint32_t)vol->log_page_size, page) < 0) {
            pmm_free_frame(page_phys);
            return NTFS_ERR_IO;
        }

        /* Apply fixup for reading */
        rc = ntfs_apply_fixup(page, (uint32_t)vol->log_page_size,
                               vol->bytes_per_sector);
        (void)rc;

        restart_offset = ntfs_le16(page + 0x10);
        if (restart_offset >= vol->log_page_size)
            restart_offset = 0x1E;

        /* Update CurrentLsn */
        ntfs_le64_write(page + restart_offset + RESTART_CURRENT_LSN_OFF,
                         commit_lsn);

        /* Regenerate fixup and write back restart page 0 */
        ntfs_regenerate_fixup(page, (uint32_t)vol->log_page_size,
                               vol->bytes_per_sector);
        log_write(vol, 0, (uint32_t)vol->log_page_size, page);

        /* Also update redundant restart page 1 */
        ntfs_apply_fixup(page, (uint32_t)vol->log_page_size,
                          vol->bytes_per_sector);
        ntfs_le64_write(page + restart_offset + RESTART_CURRENT_LSN_OFF,
                         commit_lsn);
        ntfs_regenerate_fixup(page, (uint32_t)vol->log_page_size,
                               vol->bytes_per_sector);
        log_write(vol, vol->log_page_size, (uint32_t)vol->log_page_size, page);

        pmm_free_frame(page_phys);
    }

    txn->committed = 1;

    klog(LOG_DEBUG, "ntfs",
         "txn_commit: txn %u committed at LSN %llu (%u records)",
         (uint64_t)txn->txn_id, commit_lsn,
         (uint64_t)txn->record_count);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_txn_abort — Abort a transaction (rollback)
 *
 * Walk the undo chain backward (via PreviousClientLsn), apply each
 * undo operation, and write an abort record.
 *
 * NOTE: For the initial implementation, abort simply marks the transaction
 * as aborted and writes a CompensationLogRecord. Actual undo application
 * requires the recovery engine (§13.2) to interpret operation codes.
 * ============================================================================ */

int ntfs_txn_abort(struct ntfs_txn *txn)
{
    uint64_t abort_lsn;

    if (!txn || !txn->vol)
        return NTFS_ERR_IO;

    if (txn->committed || txn->aborted)
        return NTFS_ERR_IO;

    klog(LOG_WARN, "ntfs",
         "txn_abort: aborting txn %u (%u records)",
         (uint64_t)txn->txn_id, (uint64_t)txn->record_count);

    /* Write a CompensationLogRecord (CLR) to mark the abort */
    abort_lsn = ntfs_txn_log(txn,
                              NTFS_LOG_OP_COMPENSATION, NULL, 0,
                              NTFS_LOG_OP_NOOP, NULL, 0,
                              0, 0);

    txn->aborted = 1;

    if (abort_lsn == 0) {
        klog(LOG_ERROR, "ntfs", "txn_abort: failed to write abort record");
        return NTFS_ERR_IO;
    }

    klog(LOG_DEBUG, "ntfs",
         "txn_abort: txn %u aborted at LSN %llu",
         (uint64_t)txn->txn_id, abort_lsn);

    return NTFS_OK;
}

/* ============================================================================
 * ntfs_txn_free — Free a transaction context
 * ============================================================================ */

void ntfs_txn_free(struct ntfs_txn *txn)
{
    if (txn)
        kfree(txn);
}

/* ============================================================================
 * ntfs_journal_shutdown — Clean shutdown of the journal
 *
 * Updates the restart area with the final LSN and marks the volume clean.
 * Called during unmount or clean shutdown.
 * ============================================================================ */

void ntfs_journal_shutdown(struct ntfs_volume *vol)
{
    if (!vol || !vol->journal_loaded)
        return;

    klog(LOG_INFO, "ntfs",
         "Journal shutdown: final LSN %llu, %u bytes written",
         vol->log_current_lsn, vol->log_write_pos);

    /* Update restart area one final time */
    {
        uintptr_t page_phys = pmm_alloc_contiguous(1);
        uint8_t *page;
        uint16_t restart_offset;

        if (page_phys) {
            page = (uint8_t *)(uintptr_t)page_phys;

            if (log_read(vol, 0, (uint32_t)vol->log_page_size, page) >= 0) {
                ntfs_apply_fixup(page, (uint32_t)vol->log_page_size,
                                  vol->bytes_per_sector);

                restart_offset = ntfs_le16(page + 0x10);
                if (restart_offset >= vol->log_page_size)
                    restart_offset = 0x1E;

                ntfs_le64_write(
                    page + restart_offset + RESTART_CURRENT_LSN_OFF,
                    vol->log_current_lsn);

                ntfs_regenerate_fixup(page, (uint32_t)vol->log_page_size,
                                       vol->bytes_per_sector);
                log_write(vol, 0, (uint32_t)vol->log_page_size, page);
            }

            pmm_free_frame(page_phys);
        }
    }

    /* Free log data runs */
    if (vol->log_runs) {
        kfree(vol->log_runs);
        vol->log_runs = NULL;
    }
    vol->log_run_count = 0;
    vol->journal_loaded = 0;

    klog(LOG_INFO, "ntfs", "Journal shutdown complete");
}
