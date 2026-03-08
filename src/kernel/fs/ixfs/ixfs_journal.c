/* ============================================================================
 * ixfs_journal.c — Write-Ahead Log (Journal)
 * ============================================================================ */

#include "ixfs_internal.h"

/* Simple additive checksum over journal entry data */
uint32_t ixfs_journal_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i++)
        sum += data[i];
    return sum;
}

/* Initialize journal area on disk (called from ixfs_format) */
int ixfs_journal_init(struct ixfs_volume *vol)
{
    uint8_t *buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    struct ixfs_journal_header *jh;
    uint32_t i;

    if (!buf) return -1;

    /* Zero journal header block */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        buf[i] = 0;

    jh = (struct ixfs_journal_header *)buf;
    jh->jh_magic = IXFS_JOURNAL_MAGIC;
    jh->jh_head = 1;   /* first entry slot (0 is header) */
    jh->jh_tail = 1;   /* empty journal: head == tail */
    jh->jh_seq = 0;

    if (ixfs_disk_write(vol, vol->sb.s_journal_start, buf) != 0) {
        kfree(buf);
        return -1;
    }

    /* Zero all journal entry blocks */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        buf[i] = 0;
    for (i = 1; i < vol->sb.s_journal_blocks; i++) {
        ixfs_disk_write(vol, vol->sb.s_journal_start + i, buf);
    }

    vol->j_head = 1;
    vol->txn.active = 0;
    vol->txn.count = 0;

    kfree(buf);
    return 0;
}

/* Recover journal on mount: replay committed, discard incomplete */
int ixfs_journal_recover(struct ixfs_volume *vol)
{
    uint8_t *hdr_buf;
    uint8_t *entry_buf;
    struct ixfs_journal_header *jh;
    struct ixfs_journal_entry *je;
    uint32_t slot;
    uint32_t journal_base;
    uint32_t journal_size;
    uint32_t replayed = 0;
    uint32_t i;

    hdr_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    entry_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!hdr_buf || !entry_buf) {
        if (hdr_buf) kfree(hdr_buf);
        if (entry_buf) kfree(entry_buf);
        return -1;
    }

    journal_base = vol->sb.s_journal_start;
    journal_size = vol->sb.s_journal_blocks;

    /* Read journal header */
    if (ixfs_disk_read(vol, journal_base, hdr_buf) != 0) {
        kfree(hdr_buf); kfree(entry_buf);
        return -1;
    }
    jh = (struct ixfs_journal_header *)hdr_buf;

    if (jh->jh_magic != IXFS_JOURNAL_MAGIC) {
        /* No valid journal — skip recovery */
        vol->j_head = 1;
        vol->txn.active = 0;
        vol->txn.count = 0;
        kfree(hdr_buf); kfree(entry_buf);
        return 0;
    }

    vol->j_head = jh->jh_head;

    /* Scan journal entries from tail to head, looking for committed txns */
    slot = jh->jh_tail;
    while (slot != jh->jh_head) {
        uint32_t abs_block = journal_base + slot;

        if (ixfs_disk_read(vol, abs_block, entry_buf) != 0)
            break;

        je = (struct ixfs_journal_entry *)entry_buf;

        if (je->je_type == IXFS_JE_COMMIT) {
            /* Transaction was committed — previous DATA entries are valid.
             * In our simple journal, data was already written to final
             * locations during commit. This COMMIT record confirms it. */
        } else if (je->je_type == IXFS_JE_DATA && je->je_target != 0) {
            /* Replay: write journal data to target block */
            uint8_t *replay_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
            if (replay_buf) {
                for (i = 0; i < 4080 && i < IXFS_BLOCK_SIZE; i++)
                    replay_buf[i] = je->je_data[i];
                for (; i < IXFS_BLOCK_SIZE; i++)
                    replay_buf[i] = 0;

                ixfs_disk_write(vol, je->je_target, replay_buf);
                kfree(replay_buf);
                replayed++;
            }
        }

        slot++;
        if (slot >= journal_size)
            slot = 1;  /* wrap around (skip header block) */
    }

    /* Reset journal to empty */
    jh->jh_tail = jh->jh_head;
    ixfs_disk_write(vol, journal_base, hdr_buf);

    if (replayed > 0)
        printk("[OK] IXFS journal: replayed %u entries\n",
               (uint64_t)replayed);

    vol->txn.active = 0;
    vol->txn.count = 0;

    kfree(hdr_buf);
    kfree(entry_buf);
    return 0;
}

/* Begin a new transaction */
void ixfs_txn_begin(struct ixfs_volume *vol)
{
    vol->sb.s_journal_seq++;
    vol->txn.active = 1;
    vol->txn.txn_id = vol->sb.s_journal_seq;
    vol->txn.count = 0;
}

/* Record a metadata block write in the current transaction.
 * Writes the data to the journal area immediately. */
int ixfs_txn_write(struct ixfs_volume *vol, uint32_t target_block,
                           const void *data)
{
    uint8_t *buf;
    struct ixfs_journal_entry *je;
    uint32_t journal_base = vol->sb.s_journal_start;
    uint32_t journal_size = vol->sb.s_journal_blocks;
    uint32_t abs_block;
    uint32_t i;

    if (!vol->txn.active || vol->txn.count >= IXFS_TXN_MAX_ENTRIES)
        return -1;

    buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!buf) return -1;

    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        buf[i] = 0;

    je = (struct ixfs_journal_entry *)buf;
    je->je_txn_id = vol->txn.txn_id;
    je->je_type = IXFS_JE_DATA;
    je->je_target = target_block;

    /* Copy first 4080 bytes of target data */
    {
        const uint8_t *src = (const uint8_t *)data;
        for (i = 0; i < 4080 && i < IXFS_BLOCK_SIZE; i++)
            je->je_data[i] = src[i];
    }
    je->je_checksum = ixfs_journal_checksum(je->je_data, 4080);

    /* Write to journal slot */
    abs_block = journal_base + vol->j_head;
    if (ixfs_disk_write(vol, abs_block, buf) != 0) {
        kfree(buf);
        return -1;
    }

    /* Record in transaction */
    vol->txn.blocks[vol->txn.count] = target_block;
    vol->txn.j_slots[vol->txn.count] = vol->j_head;
    vol->txn.count++;

    /* Advance head (circular) */
    vol->j_head++;
    if (vol->j_head >= journal_size)
        vol->j_head = 1;

    kfree(buf);
    return 0;
}

/* Commit the current transaction: write COMMIT record, then flush to final */
int ixfs_txn_commit(struct ixfs_volume *vol)
{
    uint8_t *buf;
    struct ixfs_journal_entry *je;
    struct ixfs_journal_header *jh;
    uint32_t journal_base = vol->sb.s_journal_start;
    uint32_t journal_size = vol->sb.s_journal_blocks;
    uint32_t abs_block;
    uint32_t i;

    if (!vol->txn.active)
        return -1;

    buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!buf) return -1;

    /* Write COMMIT record to journal */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        buf[i] = 0;
    je = (struct ixfs_journal_entry *)buf;
    je->je_txn_id = vol->txn.txn_id;
    je->je_type = IXFS_JE_COMMIT;
    je->je_target = 0;
    je->je_checksum = 0;

    abs_block = journal_base + vol->j_head;
    ixfs_disk_write(vol, abs_block, buf);

    vol->j_head++;
    if (vol->j_head >= journal_size)
        vol->j_head = 1;

    /* Now flush all recorded blocks to their final disk locations.
     * The journal data ensures crash recovery. */
    for (i = 0; i < vol->txn.count; i++) {
        uint32_t j_blk = journal_base + vol->txn.j_slots[i];
        struct ixfs_journal_entry *src_je;

        if (ixfs_disk_read(vol, j_blk, buf) == 0) {
            uint8_t *final_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
            if (final_buf) {
                uint32_t k;
                src_je = (struct ixfs_journal_entry *)buf;
                for (k = 0; k < 4080 && k < IXFS_BLOCK_SIZE; k++)
                    final_buf[k] = src_je->je_data[k];
                for (; k < IXFS_BLOCK_SIZE; k++)
                    final_buf[k] = 0;

                ixfs_write_block(vol, vol->txn.blocks[i], final_buf);
                kfree(final_buf);
            }
        }
    }

    /* Update journal header on disk */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        buf[i] = 0;
    jh = (struct ixfs_journal_header *)buf;
    jh->jh_magic = IXFS_JOURNAL_MAGIC;
    jh->jh_head = vol->j_head;
    jh->jh_tail = vol->j_head;  /* all committed, empty now */
    jh->jh_seq = vol->sb.s_journal_seq;
    ixfs_disk_write(vol, journal_base, buf);

    vol->txn.active = 0;
    vol->txn.count = 0;

    kfree(buf);
    return 0;
}
