/* ============================================================================
 * ixfs.c — Impossible X FileSystem (IXFS) Driver
 *
 * Implements format (create a fresh IXFS volume) and mount (read existing
 * IXFS volume) with full VFS integration.
 *
 * Layout on a 64 MiB disk (16384 blocks):
 *   Block 0:      Superblock
 *   Block 1:      Block bitmap (1 block covers 32768 blocks)
 *   Blocks 2..9:  Inode table (8 blocks = 256 inodes)
 *   Blocks 10+:   Data blocks
 * ============================================================================ */

#include "kernel/fs/ixfs.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/drivers/pit.h"
#include "kernel/printk.h"

/* Number of inodes to allocate (fixed at format time) */
#define IXFS_DEFAULT_INODES  256

/* --- Multi-volume types and constants --- */

#define IXFS_MAX_VOLUMES     4
#define IXFS_CACHE_SIZE      64   /* cached blocks per volume */
#define IXFS_MAX_OPEN_NODES  64   /* open vnodes per volume */

/* Buffer cache entry (per-volume, in-memory) */
struct ixfs_cache_entry {
    uint32_t block;           /* block number (0xFFFFFFFF = unused) */
    uint8_t  data[IXFS_BLOCK_SIZE];
    uint8_t  dirty;           /* 1 = needs write-back */
    uint32_t lru_tick;        /* higher = more recently used */
};

/* Hash index for large directories (per-vnode, in-memory) */
struct ixfs_hash_node {
    uint32_t entry_index;
    uint32_t next;
};

struct ixfs_dir_hash {
    uint32_t bucket[128];
    struct ixfs_hash_node *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
};

/* Forward declaration */
struct ixfs_volume;

/* Per-file/directory in-memory node */
struct ixfs_vnode {
    struct vfs_node    node;
    uint32_t           ino;
    struct ixfs_inode  inode;
    struct ixfs_dir_hash *dir_hash;
    struct ixfs_volume *vol;      /* back-pointer to owning volume */
};

/* Per-volume state — one for each mounted IXFS partition */
struct ixfs_volume {
    int                      in_use;
    const struct blkdev     *dev;
    struct ixfs_superblock   sb;
    uint8_t                 *block_bitmap;
    uint32_t                 bitmap_bytes;
    uint8_t                  blk_buf[IXFS_BLOCK_SIZE];
    struct ixfs_block_group  groups[IXFS_MAX_BLOCK_GROUPS];
    uint32_t                 group_count;
    struct ixfs_cache_entry  cache[IXFS_CACHE_SIZE];
    uint32_t                 cache_tick;
    struct ixfs_vnode        vnodes[IXFS_MAX_OPEN_NODES];
    uint32_t                 vnode_count;
    struct vfs_dirent        dirent;

    /* WAL journal state */
    uint32_t                 j_head;       /* next free journal slot (1-based) */
    struct {
        int      active;
        uint32_t txn_id;
        uint32_t count;
        uint32_t blocks[IXFS_TXN_MAX_ENTRIES];
        uint32_t j_slots[IXFS_TXN_MAX_ENTRIES];  /* journal block for each */
    } txn;
};

/* On-disk journal header — stored at journal block 0 */
struct ixfs_journal_header {
    uint32_t jh_magic;               /* IXFS_JOURNAL_MAGIC */
    uint32_t jh_head;                /* next free entry (1-based offset) */
    uint32_t jh_tail;                /* oldest unfinished entry */
    uint32_t jh_seq;                 /* current sequence number */
} __attribute__((packed));

/* On-disk journal entry — one per journal block (16-byte header + 4080 data) */
struct ixfs_journal_entry {
    uint32_t je_txn_id;              /* transaction ID that owns this entry */
    uint32_t je_type;                /* IXFS_JE_DATA or IXFS_JE_COMMIT */
    uint32_t je_target;              /* target disk block (for DATA entries) */
    uint32_t je_checksum;            /* simple additive checksum of je_data */
    uint8_t  je_data[4080];          /* first 4080 bytes of target block */
} __attribute__((packed));

static struct ixfs_volume volumes[IXFS_MAX_VOLUMES];

/* Forward declarations for internal functions */
static int ixfs_disk_read(struct ixfs_volume *vol, uint32_t block, void *buf);
static int ixfs_disk_write(struct ixfs_volume *vol, uint32_t block, const void *buf);
static struct ixfs_cache_entry *ixfs_cache_find(struct ixfs_volume *vol, uint32_t block);
static struct ixfs_cache_entry *ixfs_cache_lru(struct ixfs_volume *vol);
static int ixfs_cache_flush(struct ixfs_volume *vol);
static int ixfs_txn_write(struct ixfs_volume *vol, uint32_t target_block,
                           const void *data);

/* --- Internal: string helpers --- */

static void ixfs_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int ixfs_strcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* --- Raw disk I/O (low-level, used by cache layer only) --- */

/* Read a single IXFS block directly from disk (4 KiB = 8 sectors) */
static int ixfs_disk_read(struct ixfs_volume *vol, uint32_t block, void *buf)
{
    uint64_t lba = (uint64_t)block * IXFS_SECTORS_PER_BLK;
    return blkdev_read(vol->dev, lba, IXFS_SECTORS_PER_BLK, buf);
}

/* Write a single IXFS block directly to disk (4 KiB = 8 sectors) */
static int ixfs_disk_write(struct ixfs_volume *vol, uint32_t block, const void *buf)
{
    uint64_t lba = (uint64_t)block * IXFS_SECTORS_PER_BLK;
    return blkdev_write(vol->dev, lba, IXFS_SECTORS_PER_BLK, buf);
}

/* --- Buffer cache (LRU, write-back) --- */


/* Initialize the cache (call on format/mount) */
static void ixfs_cache_init(struct ixfs_volume *vol)
{
    uint32_t i;
    for (i = 0; i < IXFS_CACHE_SIZE; i++) {
        vol->cache[i].block = 0xFFFFFFFF;
        vol->cache[i].dirty = 0;
        vol->cache[i].lru_tick = 0;
    }
    vol->cache_tick = 0;
}

/* Find a cache entry for the given block, or NULL on miss */
static struct ixfs_cache_entry *ixfs_cache_find(struct ixfs_volume *vol, uint32_t block)
{
    uint32_t i;
    for (i = 0; i < IXFS_CACHE_SIZE; i++) {
        if (vol->cache[i].block == block) {
            vol->cache[i].lru_tick = ++vol->cache_tick;
            return &vol->cache[i];
        }
    }
    return (struct ixfs_cache_entry *)0;
}

/* Find the LRU (least recently used) entry for eviction */
static struct ixfs_cache_entry *ixfs_cache_lru(struct ixfs_volume *vol)
{
    uint32_t i;
    uint32_t min_tick = 0xFFFFFFFF;
    uint32_t min_idx = 0;

    /* Prefer an empty slot */
    for (i = 0; i < IXFS_CACHE_SIZE; i++) {
        if (vol->cache[i].block == 0xFFFFFFFF)
            return &vol->cache[i];
    }

    /* Otherwise evict the least recently used */
    for (i = 0; i < IXFS_CACHE_SIZE; i++) {
        if (vol->cache[i].lru_tick < min_tick) {
            min_tick = vol->cache[i].lru_tick;
            min_idx = i;
        }
    }
    return &vol->cache[min_idx];
}

/* Flush all dirty cache entries to disk */
static int ixfs_cache_flush(struct ixfs_volume *vol)
{
    uint32_t i;
    int err = 0;
    for (i = 0; i < IXFS_CACHE_SIZE; i++) {
        if (vol->cache[i].block != 0xFFFFFFFF && vol->cache[i].dirty) {
            if (ixfs_disk_write(vol, vol->cache[i].block, vol->cache[i].data) != 0)
                err = -1;
            else
                vol->cache[i].dirty = 0;
        }
    }
    return err;
}

/* --- Cached block I/O (used by all ixfs code) --- */

/* Read a block via the cache. Returns 0 on success. */
static int ixfs_read_block(struct ixfs_volume *vol, uint32_t block, void *buf)
{
    struct ixfs_cache_entry *ce;
    uint32_t i;

    /* Cache hit */
    ce = ixfs_cache_find(vol, block);
    if (ce) {
        uint8_t *dst = (uint8_t *)buf;
        const uint8_t *src = ce->data;
        for (i = 0; i < IXFS_BLOCK_SIZE; i++)
            dst[i] = src[i];
        return 0;
    }

    /* Cache miss — find slot (evict LRU if needed) */
    ce = ixfs_cache_lru(vol);

    /* Flush dirty evictee */
    if (ce->block != 0xFFFFFFFF && ce->dirty) {
        ixfs_disk_write(vol, ce->block, ce->data);
        ce->dirty = 0;
    }

    /* Read from disk into cache */
    if (ixfs_disk_read(vol, block, ce->data) != 0)
        return -1;

    ce->block = block;
    ce->dirty = 0;
    ce->lru_tick = ++vol->cache_tick;

    /* Copy to caller's buffer */
    {
        uint8_t *dst = (uint8_t *)buf;
        const uint8_t *src = ce->data;
        for (i = 0; i < IXFS_BLOCK_SIZE; i++)
            dst[i] = src[i];
    }
    return 0;
}

/* Write a block via the cache (deferred — only marks dirty). */
static int ixfs_write_block(struct ixfs_volume *vol, uint32_t block, const void *buf)
{
    struct ixfs_cache_entry *ce;
    uint32_t i;
    const uint8_t *src = (const uint8_t *)buf;

    /* Check if already cached */
    ce = ixfs_cache_find(vol, block);
    if (!ce) {
        /* Not cached — get a slot */
        ce = ixfs_cache_lru(vol);
        if (ce->block != 0xFFFFFFFF && ce->dirty) {
            ixfs_disk_write(vol, ce->block, ce->data);
        }
        ce->block = block;
    }

    /* Copy data and mark dirty */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        ce->data[i] = src[i];
    ce->dirty = 1;
    ce->lru_tick = ++vol->cache_tick;

    return 0;
}

/* Zero a block (via cache — deferred to disk) */
static int ixfs_zero_block(struct ixfs_volume *vol, uint32_t block)
{
    uint32_t i;
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        vol->blk_buf[i] = 0;
    return ixfs_write_block(vol, block, vol->blk_buf);
}

/* --- Internal: bitmap operations --- */

static void bitmap_set(uint8_t *bmap, uint32_t bit)
{
    bmap[bit / 8] |= (uint8_t)(1 << (bit % 8));
}

static void bitmap_clear(uint8_t *bmap, uint32_t bit)
{
    bmap[bit / 8] &= (uint8_t)~(1 << (bit % 8));
}

static int bitmap_test(const uint8_t *bmap, uint32_t bit)
{
    return (bmap[bit / 8] >> (bit % 8)) & 1;
}

/* --- Block group state (in-memory only) --- */


/* Initialize block group descriptors from the bitmap.
 * Must be called after the bitmap is loaded into memory. */
static void ixfs_init_groups(struct ixfs_volume *vol)
{
    uint32_t g, b;
    uint32_t total = vol->sb.s_total_blocks;

    vol->group_count = (total + IXFS_BLOCKS_PER_GROUP - 1) / IXFS_BLOCKS_PER_GROUP;
    if (vol->group_count > IXFS_MAX_BLOCK_GROUPS)
        vol->group_count = IXFS_MAX_BLOCK_GROUPS;

    for (g = 0; g < vol->group_count; g++) {
        uint32_t start = g * IXFS_BLOCKS_PER_GROUP;
        uint32_t end = start + IXFS_BLOCKS_PER_GROUP;
        if (end > total) end = total;

        vol->groups[g].bg_start = start;
        vol->groups[g].bg_count = end - start;
        vol->groups[g].bg_free = 0;
        vol->groups[g].bg_next_free = start;

        /* Count free blocks and find the first free block */
        for (b = start; b < end; b++) {
            if (!bitmap_test(vol->block_bitmap, b)) {
                vol->groups[g].bg_free++;
                if (vol->groups[g].bg_next_free == start ||
                    b < vol->groups[g].bg_next_free)
                    vol->groups[g].bg_next_free = b;
            }
        }

        /* If no free blocks, set hint past end */
        if (vol->groups[g].bg_free == 0)
            vol->groups[g].bg_next_free = end;
    }
}

/* Allocate a block, preferring the given block group index for locality.
 * Pass preferred_group = 0xFFFFFFFF to allocate from any group.
 * Returns block number or 0 on failure. */
static uint32_t ixfs_alloc_block_near(struct ixfs_volume *vol, uint32_t preferred_group)
{
    uint32_t tries;
    uint32_t start_group;

    if (vol->sb.s_free_blocks == 0)
        return 0;

    /* Determine starting group */
    start_group = (preferred_group < vol->group_count) ? preferred_group : 0;

    /* Try each group, starting from the preferred one */
    for (tries = 0; tries < vol->group_count; tries++) {
        uint32_t gi = (start_group + tries) % vol->group_count;
        struct ixfs_block_group *bg = &vol->groups[gi];
        uint32_t b, end;

        if (bg->bg_free == 0)
            continue;

        /* Start from the hint, wrap around within the group */
        end = bg->bg_start + bg->bg_count;
        b = bg->bg_next_free;
        if (b < bg->bg_start || b >= end)
            b = bg->bg_start;

        /* Scan from hint to end of group */
        for (; b < end; b++) {
            if (!bitmap_test(vol->block_bitmap, b)) {
                bitmap_set(vol->block_bitmap, b);
                bg->bg_free--;
                vol->sb.s_free_blocks--;
                /* Advance hint to next block */
                bg->bg_next_free = b + 1;
                return b;
            }
        }

        /* Wrap: scan from group start to hint */
        for (b = bg->bg_start; b < bg->bg_next_free && b < end; b++) {
            if (!bitmap_test(vol->block_bitmap, b)) {
                bitmap_set(vol->block_bitmap, b);
                bg->bg_free--;
                vol->sb.s_free_blocks--;
                bg->bg_next_free = b + 1;
                return b;
            }
        }

        /* Shouldn't reach here if bg_free > 0, but mark full just in case */
        bg->bg_free = 0;
        bg->bg_next_free = end;
    }

    return 0;  /* no free blocks */
}

/* Convenience: allocate from any group (no locality preference) */
static uint32_t ixfs_alloc_block(struct ixfs_volume *vol)
{
    /* Default: prefer group 0 (where data starts), fallback to any */
    uint32_t data_group = vol->sb.s_data_start / IXFS_BLOCKS_PER_GROUP;
    return ixfs_alloc_block_near(vol, data_group);
}

/* Free a block back to the bitmap and update its group descriptor */
static void ixfs_free_block(struct ixfs_volume *vol, uint32_t block)
{
    uint32_t gi;

    if (block < vol->sb.s_data_start || block >= vol->sb.s_total_blocks)
        return;

    if (!bitmap_test(vol->block_bitmap, block))
        return;  /* already free */

    bitmap_clear(vol->block_bitmap, block);
    vol->sb.s_free_blocks++;

    /* Update the owning group */
    gi = block / IXFS_BLOCKS_PER_GROUP;
    if (gi < vol->group_count) {
        vol->groups[gi].bg_free++;
        /* Move hint back if this block is earlier */
        if (block < vol->groups[gi].bg_next_free)
            vol->groups[gi].bg_next_free = block;
    }
}

/* Flush bitmap to disk */
static int ixfs_flush_bitmap(struct ixfs_volume *vol)
{
    uint32_t i;
    for (i = 0; i < vol->sb.s_bitmap_blocks; i++) {
        uint32_t offset = i * IXFS_BLOCK_SIZE;
        uint32_t remaining = vol->bitmap_bytes - offset;
        uint32_t j;

        if (remaining > IXFS_BLOCK_SIZE)
            remaining = IXFS_BLOCK_SIZE;

        /* Copy bitmap chunk to buffer, zero-pad */
        for (j = 0; j < remaining; j++)
            vol->blk_buf[j] = vol->block_bitmap[offset + j];
        for (; j < IXFS_BLOCK_SIZE; j++)
            vol->blk_buf[j] = 0;

        if (ixfs_write_block(vol, vol->sb.s_bitmap_start + i, vol->blk_buf) != 0)
            return -1;

        /* Journal this bitmap block if transaction is active */
        if (vol->txn.active)
            ixfs_txn_write(vol, vol->sb.s_bitmap_start + i, vol->blk_buf);
    }
    ixfs_cache_flush(vol);  /* ensure bitmap blocks reach disk */
    return 0;
}

/* Flush superblock to disk */
static int ixfs_flush_superblock(struct ixfs_volume *vol)
{
    uint32_t i;
    uint8_t *sp = (uint8_t *)&vol->sb;

    /* Zero the buffer, copy superblock into it */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        vol->blk_buf[i] = 0;
    for (i = 0; i < sizeof(struct ixfs_superblock); i++)
        vol->blk_buf[i] = sp[i];

    if (ixfs_write_block(vol, 0, vol->blk_buf) != 0)
        return -1;

    /* Journal this superblock write if transaction is active */
    if (vol->txn.active)
        ixfs_txn_write(vol, 0, vol->blk_buf);

    ixfs_cache_flush(vol);  /* ensure superblock reaches disk */
    return 0;
}

/* --- Write-Ahead Log (Journal) --- */

/* Simple additive checksum over journal entry data */
static uint32_t ixfs_journal_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i++)
        sum += data[i];
    return sum;
}

/* Initialize journal area on disk (called from ixfs_format) */
static int ixfs_journal_init(struct ixfs_volume *vol)
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
static int ixfs_journal_recover(struct ixfs_volume *vol)
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
static void ixfs_txn_begin(struct ixfs_volume *vol)
{
    vol->sb.s_journal_seq++;
    vol->txn.active = 1;
    vol->txn.txn_id = vol->sb.s_journal_seq;
    vol->txn.count = 0;
}

/* Record a metadata block write in the current transaction.
 * Writes the data to the journal area immediately. */
static int ixfs_txn_write(struct ixfs_volume *vol, uint32_t target_block,
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
static int ixfs_txn_commit(struct ixfs_volume *vol)
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

/* --- Internal: inode I/O --- */

/* Read an inode from disk */
static int ixfs_read_inode(struct ixfs_volume *vol, uint32_t ino, struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block = vol->sb.s_inode_start + (ino / inodes_per_block);
    uint32_t offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);
    uint32_t i;
    uint8_t *dst;
    uint8_t *tmp_buf;

    tmp_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!tmp_buf) return -1;

    if (ixfs_read_block(vol, block, tmp_buf) != 0) {
        kfree(tmp_buf);
        return -1;
    }

    dst = (uint8_t *)inode;
    for (i = 0; i < sizeof(struct ixfs_inode); i++)
        dst[i] = tmp_buf[offset + i];

    kfree(tmp_buf);
    return 0;
}

/* Write an inode to disk */
static int ixfs_write_inode(struct ixfs_volume *vol, uint32_t ino, const struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block = vol->sb.s_inode_start + (ino / inodes_per_block);
    uint32_t offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);
    uint32_t i;
    const uint8_t *src;
    uint8_t *tmp_buf;

    tmp_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!tmp_buf) return -1;

    /* Read the block first (to preserve other inodes in the same block) */
    if (ixfs_read_block(vol, block, tmp_buf) != 0) {
        kfree(tmp_buf);
        return -1;
    }

    src = (const uint8_t *)inode;
    for (i = 0; i < sizeof(struct ixfs_inode); i++)
        tmp_buf[offset + i] = src[i];

    i = ixfs_write_block(vol, block, tmp_buf);
    kfree(tmp_buf);
    return i;
}

/* --- VFS node storage --- */
/* Directory hash index constants */
#define IXFS_HASH_BUCKETS   128
#define IXFS_HASH_THRESHOLD 64
#define IXFS_HASH_CHAIN_END 0xFFFFFFFF


/* Forward declarations */
static struct vfs_ops ixfs_file_ops;
static struct vfs_ops ixfs_dir_ops;
static uint32_t ixfs_get_block(struct ixfs_volume *vol, const struct ixfs_inode *inode, uint32_t index);

/* --- FNV-1a hash for directory names --- */

static uint32_t ixfs_fnv1a(const char *name)
{
    uint32_t hash = 0x811C9DC5;  /* FNV offset basis */
    while (*name) {
        hash ^= (uint8_t)*name++;
        hash *= 0x01000193;      /* FNV prime */
    }
    return hash;
}

/* Free a directory hash index */
static void ixfs_hash_free(struct ixfs_vnode *v)
{
    if (v->dir_hash) {
        if (v->dir_hash->nodes)
            kfree(v->dir_hash->nodes);
        kfree(v->dir_hash);
        v->dir_hash = (struct ixfs_dir_hash *)0;
    }
}

/* Build a hash index for a directory vnode.
 * Scans all directory entries and hashes their names into buckets. */
static void ixfs_hash_build(struct ixfs_volume *vol, struct ixfs_vnode *v)
{
    uint32_t total_entries;
    uint32_t i, b;
    uint8_t *data_buf;
    struct ixfs_dir_hash *dh;

    /* Free old index if any */
    ixfs_hash_free(v);

    total_entries = v->inode.i_size / sizeof(struct ixfs_dir_entry);
    if (total_entries == 0) return;

    /* Allocate hash table */
    dh = (struct ixfs_dir_hash *)kmalloc(sizeof(struct ixfs_dir_hash));
    if (!dh) return;

    for (b = 0; b < IXFS_HASH_BUCKETS; b++)
        dh->bucket[b] = IXFS_HASH_CHAIN_END;

    /* Allocate chain nodes — one per active entry */
    dh->nodes = (struct ixfs_hash_node *)kmalloc(
        total_entries * sizeof(struct ixfs_hash_node));
    if (!dh->nodes) {
        kfree(dh);
        return;
    }
    dh->node_count = 0;
    dh->node_capacity = total_entries;

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) {
        kfree(dh->nodes);
        kfree(dh);
        return;
    }

    /* Scan all entries and hash them */
    for (i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
        uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
        uint32_t disk_block;
        struct ixfs_dir_entry *de;
        uint32_t bucket_idx;
        uint32_t ni;

        disk_block = ixfs_get_block(vol, &v->inode, blk_idx);
        if (disk_block == 0) break;

        if (ixfs_read_block(vol, disk_block, data_buf) != 0)
            break;

        de = (struct ixfs_dir_entry *)(data_buf + blk_off);
        if (de->d_inode == 0) continue;  /* skip deleted */

        /* Insert into hash table */
        bucket_idx = ixfs_fnv1a(de->d_name) % IXFS_HASH_BUCKETS;
        ni = dh->node_count++;
        dh->nodes[ni].entry_index = i;
        dh->nodes[ni].next = dh->bucket[bucket_idx];
        dh->bucket[bucket_idx] = ni;
    }

    kfree(data_buf);
    v->dir_hash = dh;
}

/* Lookup a name in the hash index. Returns the entry index or 0xFFFFFFFF.
 * The caller must still read the block and verify the name (for collision). */
static uint32_t ixfs_hash_lookup(struct ixfs_volume *vol, struct ixfs_vnode *v, const char *name,
                                 uint8_t *data_buf)
{
    struct ixfs_dir_hash *dh = v->dir_hash;
    uint32_t bucket_idx;
    uint32_t ni;

    if (!dh) return IXFS_HASH_CHAIN_END;

    bucket_idx = ixfs_fnv1a(name) % IXFS_HASH_BUCKETS;
    ni = dh->bucket[bucket_idx];

    while (ni != IXFS_HASH_CHAIN_END) {
        uint32_t ei = dh->nodes[ni].entry_index;
        uint32_t byte_off = ei * sizeof(struct ixfs_dir_entry);
        uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
        uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
        uint32_t disk_block;
        struct ixfs_dir_entry *de;

        disk_block = ixfs_get_block(vol, &v->inode, blk_idx);
        if (disk_block == 0) break;

        if (ixfs_read_block(vol, disk_block, data_buf) != 0)
            break;

        de = (struct ixfs_dir_entry *)(data_buf + blk_off);
        if (de->d_inode != 0 && ixfs_strcmp(de->d_name, name))
            return ei;  /* found it */

        ni = dh->nodes[ni].next;
    }

    return IXFS_HASH_CHAIN_END;
}

/* Get or create a vnode for an inode number */
static struct ixfs_vnode *ixfs_get_vnode(struct ixfs_volume *vol, uint32_t ino)
{
    uint32_t i;
    struct ixfs_vnode *v;

    /* Check if already cached */
    for (i = 0; i < vol->vnode_count; i++) {
        if (vol->vnodes[i].ino == ino)
            return &vol->vnodes[i];
    }

    /* Create new */
    if (vol->vnode_count >= IXFS_MAX_OPEN_NODES)
        return (struct ixfs_vnode *)0;

    v = &vol->vnodes[vol->vnode_count];
    v->ino = ino;
    v->vol = vol;
    v->dir_hash = (struct ixfs_dir_hash *)0;

    if (ixfs_read_inode(vol, ino, &v->inode) != 0)
        return (struct ixfs_vnode *)0;

    v->node.inode = ino;
    v->node.size = v->inode.i_size;
    v->node.flags = 0;
    v->node.fs_data = v;
    v->node.parent = (struct vfs_node *)0;

    if (v->inode.i_mode & IXFS_S_DIR) {
        v->node.type = VFS_DIRECTORY;
        v->node.ops = &ixfs_dir_ops;
    } else {
        v->node.type = VFS_FILE;
        v->node.ops = &ixfs_file_ops;
    }

    vol->vnode_count++;
    return v;
}

/* --- Internal: extent-based block lookup --- */

/* Given a file block index, return the disk block number via extent search */
static uint32_t ixfs_get_block(struct ixfs_volume *vol, const struct ixfs_inode *inode, uint32_t index)
{
    uint32_t i;
    uint32_t file_offset = 0;

    (void)vol;  /* not needed for inline extents */

    for (i = 0; i < inode->i_extent_count; i++) {
        uint32_t count = inode->i_extents[i].e_count;
        if (count == 0) continue;

        if (index < file_offset + count) {
            /* Block is within this extent */
            return (uint32_t)(inode->i_extents[i].e_start +
                              (index - file_offset));
        }
        file_offset += count;
    }

    /* TODO: check overflow extent block if IXFS_EXTENT_OVERFLOW set */
    return 0;  /* block not mapped */
}

/* Allocate a new disk block and add it to the inode's extent list.
 * Merges with the last extent if contiguous. Returns the disk block or 0. */
static uint32_t ixfs_add_block_to_extent(struct ixfs_volume *vol,
                                          struct ixfs_inode *inode)
{
    uint32_t new_blk;
    uint32_t ec = inode->i_extent_count;
    uint32_t last_end;

    /* Try to allocate near the end of the last extent for contiguity */
    if (ec > 0 && inode->i_extents[ec - 1].e_count > 0) {
        last_end = (uint32_t)(inode->i_extents[ec - 1].e_start +
                              inode->i_extents[ec - 1].e_count);
    } else {
        last_end = 0;
    }

    new_blk = ixfs_alloc_block(vol);
    if (new_blk == 0) return 0;

    /* Try to merge with the last extent */
    if (ec > 0 && new_blk == last_end) {
        /* Contiguous — extend the last extent */
        inode->i_extents[ec - 1].e_count++;
        inode->i_blocks++;
        return new_blk;
    }

    /* Need a new extent */
    if (ec < IXFS_INLINE_EXTENTS) {
        inode->i_extents[ec].e_start = (uint64_t)new_blk;
        inode->i_extents[ec].e_count = 1;
        inode->i_extent_count = ec + 1;
        inode->i_blocks++;
        return new_blk;
    }

    /* Out of inline extents — would need overflow block (future) */
    ixfs_free_block(vol, new_blk);
    return 0;
}

/* Free all blocks referenced by an inode's extents */
static void ixfs_free_all_extents(struct ixfs_volume *vol,
                                   struct ixfs_inode *inode)
{
    uint32_t i, j;

    for (i = 0; i < inode->i_extent_count; i++) {
        uint64_t start = inode->i_extents[i].e_start;
        uint32_t count = inode->i_extents[i].e_count;
        for (j = 0; j < count; j++) {
            ixfs_free_block(vol, (uint32_t)(start + j));
        }
        inode->i_extents[i].e_start = 0;
        inode->i_extents[i].e_count = 0;
    }
    inode->i_extent_count = 0;
    inode->i_blocks = 0;
}

/* --- VFS file operations --- */

static int ixfs_file_open(struct vfs_node *node, uint32_t flags)
{
    (void)node; (void)flags;
    return 0;
}

static int ixfs_file_close(struct vfs_node *node)
{
    (void)node;
    return 0;
}

static int ixfs_file_read(struct vfs_node *node, uint32_t offset,
                          uint32_t size, uint8_t *buffer)
{
    struct ixfs_vnode *v = (struct ixfs_vnode *)node->fs_data;
    struct ixfs_volume *vol = v->vol;
    uint32_t bytes_read = 0;
    uint32_t blk_index, blk_offset;
    uint8_t *data_buf;

    if (!v || offset >= v->inode.i_size)
        return 0;

    if (offset + size > v->inode.i_size)
        size = v->inode.i_size - offset;

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    blk_index = offset / IXFS_BLOCK_SIZE;
    blk_offset = offset % IXFS_BLOCK_SIZE;

    while (bytes_read < size) {
        uint32_t disk_block = ixfs_get_block(vol, &v->inode, blk_index);
        uint32_t to_read;
        uint32_t i;

        if (disk_block == 0) break;

        if (ixfs_read_block(vol, disk_block, data_buf) != 0)
            break;

        to_read = IXFS_BLOCK_SIZE - blk_offset;
        if (to_read > size - bytes_read)
            to_read = size - bytes_read;

        for (i = 0; i < to_read; i++)
            buffer[bytes_read + i] = data_buf[blk_offset + i];

        bytes_read += to_read;
        blk_offset = 0;
        blk_index++;
    }

    kfree(data_buf);

    /* Update access time */
    if (bytes_read > 0) {
        v->inode.i_atime = (uint32_t)uptime();
        ixfs_write_inode(vol, v->ino, &v->inode);
    }

    return (int)bytes_read;
}

static int ixfs_file_write(struct vfs_node *node, uint32_t offset,
                           uint32_t size, const uint8_t *buffer)
{
    struct ixfs_vnode *v = (struct ixfs_vnode *)node->fs_data;
    struct ixfs_volume *vol = v->vol;
    uint32_t bytes_written = 0;
    uint32_t blk_index, blk_offset;
    uint8_t *data_buf;

    if (!v) return -1;

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    blk_index = offset / IXFS_BLOCK_SIZE;
    blk_offset = offset % IXFS_BLOCK_SIZE;

    while (bytes_written < size) {
        uint32_t disk_block = ixfs_get_block(vol, &v->inode, blk_index);
        uint32_t to_write;
        uint32_t i;

        /* Allocate a new block if needed */
        if (disk_block == 0) {
            disk_block = ixfs_add_block_to_extent(vol, &v->inode);
            if (disk_block == 0) break;

            /* Zero the new block */
            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                data_buf[i] = 0;
        } else {
            /* Read existing block for partial writes */
            if (ixfs_read_block(vol, disk_block, data_buf) != 0)
                break;
        }

        to_write = IXFS_BLOCK_SIZE - blk_offset;
        if (to_write > size - bytes_written)
            to_write = size - bytes_written;

        for (i = 0; i < to_write; i++)
            data_buf[blk_offset + i] = buffer[bytes_written + i];

        if (ixfs_write_block(vol, disk_block, data_buf) != 0)
            break;

        bytes_written += to_write;
        blk_offset = 0;
        blk_index++;
    }

    /* Update inode size and timestamps */
    if (offset + bytes_written > v->inode.i_size)
        v->inode.i_size = offset + bytes_written;

    v->inode.i_mtime = (uint32_t)uptime();
    v->inode.i_atime = v->inode.i_mtime;

    /* Flush metadata atomically via journal */
    ixfs_txn_begin(vol);
    v->node.size = v->inode.i_size;
    ixfs_write_inode(vol, v->ino, &v->inode);
    ixfs_flush_bitmap(vol);
    ixfs_flush_superblock(vol);
    ixfs_txn_commit(vol);

    kfree(data_buf);
    return (int)bytes_written;
}

static struct vfs_ops ixfs_file_ops = {
    .open    = ixfs_file_open,
    .close   = ixfs_file_close,
    .read    = ixfs_file_read,
    .write   = ixfs_file_write,
    .readdir = (void *)0,
    .finddir = (void *)0,
    .create  = (void *)0,
    .unlink  = (void *)0,
};

/* --- VFS directory operations --- */

static struct vfs_dirent *ixfs_readdir(struct vfs_node *node, uint32_t index)
{
    struct ixfs_vnode *v = (struct ixfs_vnode *)node->fs_data;
    struct ixfs_volume *vol = v->vol;
    uint32_t byte_offset;
    uint32_t blk_index, blk_offset;
    uint32_t disk_block;
    struct ixfs_dir_entry *de;
    uint32_t count = 0;
    uint32_t entries_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_dir_entry);
    uint32_t total_entries;
    uint32_t i;
    uint8_t *data_buf;

    if (!v) return (struct vfs_dirent *)0;

    total_entries = v->inode.i_size / sizeof(struct ixfs_dir_entry);

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return (struct vfs_dirent *)0;

    /* Walk through all entries, skipping deleted ones, until we reach index */
    for (i = 0; i < total_entries; i++) {
        byte_offset = i * sizeof(struct ixfs_dir_entry);
        blk_index = byte_offset / IXFS_BLOCK_SIZE;
        blk_offset = byte_offset % IXFS_BLOCK_SIZE;

        disk_block = ixfs_get_block(vol, &v->inode, blk_index);
        if (disk_block == 0) break;

        if (ixfs_read_block(vol, disk_block, data_buf) != 0)
            break;

        de = (struct ixfs_dir_entry *)(data_buf + blk_offset);

        /* Skip free/deleted entries */
        if (de->d_inode == 0)
            continue;

        /* Skip . and .. */
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;

        if (count == index) {
            ixfs_strcpy(vol->dirent.name, de->d_name, VFS_MAX_NAME);
            vol->dirent.inode = de->d_inode;

            /* Look up the inode to determine type */
            {
                struct ixfs_inode tmp;
                if (ixfs_read_inode(vol, de->d_inode, &tmp) == 0) {
                    vol->dirent.type = (tmp.i_mode & IXFS_S_DIR)
                                     ? VFS_DIRECTORY : VFS_FILE;
                } else {
                    vol->dirent.type = VFS_FILE;
                }
            }

            kfree(data_buf);
            return &vol->dirent;
        }
        count++;
    }

    kfree(data_buf);

    (void)entries_per_block;

    return (struct vfs_dirent *)0;
}

static struct vfs_node *ixfs_finddir(struct vfs_node *node, const char *name)
{
    struct ixfs_vnode *v = (struct ixfs_vnode *)node->fs_data;
    struct ixfs_volume *vol = v->vol;
    uint32_t total_entries;
    uint32_t i;
    uint8_t *data_buf;

    if (!v) return (struct vfs_node *)0;

    total_entries = v->inode.i_size / sizeof(struct ixfs_dir_entry);

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return (struct vfs_node *)0;

    /* Large directory: use hash index for O(1) lookup */
    if (total_entries > IXFS_HASH_THRESHOLD) {
        uint32_t ei;

        /* Build hash index lazily on first access */
        if (!v->dir_hash)
            ixfs_hash_build(vol, v);

        ei = ixfs_hash_lookup(vol, v, name, data_buf);
        if (ei != IXFS_HASH_CHAIN_END) {
            uint32_t byte_off = ei * sizeof(struct ixfs_dir_entry);
            uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
            uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
            uint32_t disk_block = ixfs_get_block(vol, &v->inode, blk_idx);
            struct ixfs_dir_entry *de;
            struct ixfs_vnode *found;

            if (disk_block != 0 &&
                ixfs_read_block(vol, disk_block, data_buf) == 0) {
                de = (struct ixfs_dir_entry *)(data_buf + blk_off);
                kfree(data_buf);
                found = ixfs_get_vnode(vol, de->d_inode);
                if (!found) return (struct vfs_node *)0;
                ixfs_strcpy(found->node.name, name, VFS_MAX_NAME);
                return &found->node;
            }
        }

        kfree(data_buf);
        return (struct vfs_node *)0;
    }

    /* Small directory: linear scan (< 64 entries) */
    for (i = 0; i < total_entries; i++) {
        uint32_t byte_offset = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_index = byte_offset / IXFS_BLOCK_SIZE;
        uint32_t blk_offset = byte_offset % IXFS_BLOCK_SIZE;
        uint32_t disk_block;
        struct ixfs_dir_entry *de;

        disk_block = ixfs_get_block(vol, &v->inode, blk_index);
        if (disk_block == 0) break;

        if (ixfs_read_block(vol, disk_block, data_buf) != 0)
            break;

        de = (struct ixfs_dir_entry *)(data_buf + blk_offset);

        if (de->d_inode != 0 && ixfs_strcmp(de->d_name, name)) {
            struct ixfs_vnode *found;

            kfree(data_buf);
            found = ixfs_get_vnode(vol, de->d_inode);
            if (!found) return (struct vfs_node *)0;

            ixfs_strcpy(found->node.name, name, VFS_MAX_NAME);
            return &found->node;
        }
    }

    kfree(data_buf);
    return (struct vfs_node *)0;
}

static int ixfs_create(struct vfs_node *parent, const char *name, uint8_t type)
{
    struct ixfs_vnode *pv = (struct ixfs_vnode *)parent->fs_data;
    struct ixfs_volume *vol = pv->vol;
    uint32_t new_ino;
    struct ixfs_inode new_inode;
    uint32_t dir_block;
    uint8_t *data_buf;
    struct ixfs_dir_entry *de;
    uint32_t total_entries;
    uint32_t i;
    int found_slot = -1;

    if (!pv) return -1;

    /* Find a free inode (0 = reserved, 1 = root) */
    new_ino = 0;
    for (i = 2; i < vol->sb.s_total_inodes; i++) {
        struct ixfs_inode tmp;
        if (ixfs_read_inode(vol, i, &tmp) == 0 && tmp.i_mode == 0) {
            new_ino = i;
            break;
        }
    }
    if (new_ino == 0) return -1;

    /* Initialize the new inode */
    {
        uint8_t *p = (uint8_t *)&new_inode;
        for (i = 0; i < sizeof(struct ixfs_inode); i++)
            p[i] = 0;
    }

    new_inode.i_mode = (type == VFS_DIRECTORY)
                      ? (IXFS_S_DIR | IXFS_PERM_DIR)
                      : (IXFS_S_FILE | IXFS_PERM_FILE);
    new_inode.i_links = 1;
    new_inode.i_uid = 0;
    new_inode.i_gid = 0;
    new_inode.i_size = 0;
    new_inode.i_blocks = 0;
    {
        uint32_t now = (uint32_t)uptime();
        new_inode.i_ctime = now;
        new_inode.i_mtime = now;
        new_inode.i_atime = now;
    }

    /* If directory, allocate an initial data block for . and .. */
    if (type == VFS_DIRECTORY) {
        uint32_t blk = ixfs_alloc_block(vol);
        if (blk == 0) return -1;

        new_inode.i_extents[0].e_start = (uint64_t)blk;
        new_inode.i_extents[0].e_count = 1;
        new_inode.i_extent_count = 1;
        new_inode.i_blocks = 1;
        new_inode.i_size = 2 * sizeof(struct ixfs_dir_entry);

        /* Write . and .. entries */
        data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
        if (!data_buf) return -1;

        for (i = 0; i < IXFS_BLOCK_SIZE; i++)
            data_buf[i] = 0;

        de = (struct ixfs_dir_entry *)data_buf;
        de[0].d_inode = new_ino;
        ixfs_strcpy(de[0].d_name, ".", IXFS_MAX_NAME);
        de[1].d_inode = pv->ino;
        ixfs_strcpy(de[1].d_name, "..", IXFS_MAX_NAME);

        ixfs_write_block(vol, blk, data_buf);
        kfree(data_buf);
    }

    ixfs_write_inode(vol, new_ino, &new_inode);
    vol->sb.s_free_inodes--;

    /* Add directory entry to parent */
    total_entries = pv->inode.i_size / sizeof(struct ixfs_dir_entry);

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    /* Look for a free slot in existing entries */
    for (i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t bi = byte_off / IXFS_BLOCK_SIZE;
        uint32_t bo = byte_off % IXFS_BLOCK_SIZE;

        dir_block = ixfs_get_block(vol, &pv->inode, bi);
        if (dir_block == 0) break;

        if (ixfs_read_block(vol, dir_block, data_buf) != 0) break;

        de = (struct ixfs_dir_entry *)(data_buf + bo);
        if (de->d_inode == 0) {
            found_slot = (int)i;
            de->d_inode = new_ino;
            ixfs_strcpy(de->d_name, name, IXFS_MAX_NAME);
            ixfs_write_block(vol, dir_block, data_buf);
            break;
        }
    }

    if (found_slot < 0) {
        /* Append at end — may need to grow the directory */
        uint32_t byte_off = total_entries * sizeof(struct ixfs_dir_entry);
        uint32_t bi = byte_off / IXFS_BLOCK_SIZE;
        uint32_t bo = byte_off % IXFS_BLOCK_SIZE;

        dir_block = ixfs_get_block(vol, &pv->inode, bi);

        if (dir_block == 0) {
            /* Allocate a new block for the directory via extent */
            dir_block = ixfs_add_block_to_extent(vol, &pv->inode);
            if (dir_block == 0) { kfree(data_buf); return -1; }

            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                data_buf[i] = 0;
        } else if (dir_block == 0) {
            kfree(data_buf);
            return -1;
        } else {
            if (ixfs_read_block(vol, dir_block, data_buf) != 0) {
                kfree(data_buf);
                return -1;
            }
        }

        de = (struct ixfs_dir_entry *)(data_buf + bo);
        de->d_inode = new_ino;
        ixfs_strcpy(de->d_name, name, IXFS_MAX_NAME);
        ixfs_write_block(vol, dir_block, data_buf);

        pv->inode.i_size += sizeof(struct ixfs_dir_entry);
    }

    /* Flush parent inode */
    pv->node.size = pv->inode.i_size;
    ixfs_write_inode(vol, pv->ino, &pv->inode);
    ixfs_flush_bitmap(vol);
    ixfs_flush_superblock(vol);

    /* Invalidate hash index — will rebuild on next finddir */
    ixfs_hash_free(pv);

    kfree(data_buf);
    return 0;
}

static int ixfs_unlink(struct vfs_node *parent, const char *name)
{
    struct ixfs_vnode *pv = (struct ixfs_vnode *)parent->fs_data;
    struct ixfs_volume *vol = pv->vol;
    uint32_t total_entries;
    uint32_t i;
    uint8_t *data_buf;

    if (!pv) return -1;

    total_entries = pv->inode.i_size / sizeof(struct ixfs_dir_entry);

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    for (i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t bi = byte_off / IXFS_BLOCK_SIZE;
        uint32_t bo = byte_off % IXFS_BLOCK_SIZE;
        uint32_t dir_block;
        struct ixfs_dir_entry *de;

        dir_block = ixfs_get_block(vol, &pv->inode, bi);
        if (dir_block == 0) break;

        if (ixfs_read_block(vol, dir_block, data_buf) != 0) break;

        de = (struct ixfs_dir_entry *)(data_buf + bo);

        if (de->d_inode != 0 && ixfs_strcmp(de->d_name, name)) {
            struct ixfs_inode target;
            uint32_t target_ino = de->d_inode;
            uint32_t j;

            /* Read the target inode */
            if (ixfs_read_inode(vol, target_ino, &target) != 0) {
                kfree(data_buf);
                return -1;
            }

            /* If it's a directory, check it's empty first */
            if (target.i_mode & IXFS_S_DIR) {
                uint32_t d_total = target.i_size
                                 / sizeof(struct ixfs_dir_entry);
                uint32_t d_real = 0;
                uint32_t d_idx;
                uint8_t *dir_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
                if (!dir_buf) { kfree(data_buf); return -1; }

                for (d_idx = 0; d_idx < d_total; d_idx++) {
                    uint32_t d_off = d_idx * sizeof(struct ixfs_dir_entry);
                    uint32_t d_bi = d_off / IXFS_BLOCK_SIZE;
                    uint32_t d_bo = d_off % IXFS_BLOCK_SIZE;
                    uint32_t d_blk = ixfs_get_block(vol, &target, d_bi);
                    struct ixfs_dir_entry *child;

                    if (d_blk == 0) break;
                    if (ixfs_read_block(vol, d_blk, dir_buf) != 0) break;

                    child = (struct ixfs_dir_entry *)(dir_buf + d_bo);
                    if (child->d_inode == 0) continue;
                    /* Skip . and .. */
                    if (child->d_name[0] == '.' &&
                        (child->d_name[1] == '\0' ||
                         (child->d_name[1] == '.'
                          && child->d_name[2] == '\0')))
                        continue;
                    d_real++;
                }
                kfree(dir_buf);

                if (d_real > 0) {
                    kfree(data_buf);
                    return -1;  /* directory not empty */
                }
            }

            /* Free all data blocks via extents */
            ixfs_free_all_extents(vol, &target);

            /* Zero the inode on disk */
            {
                uint8_t *p = (uint8_t *)&target;
                for (j = 0; j < sizeof(struct ixfs_inode); j++)
                    p[j] = 0;
            }
            ixfs_write_inode(vol, target_ino, &target);
            vol->sb.s_free_inodes++;

            /* Clear the directory entry */
            de->d_inode = 0;
            de->d_name[0] = '\0';
            ixfs_write_block(vol, dir_block, data_buf);

            /* Remove from vnode cache if present */
            for (j = 0; j < vol->vnode_count; j++) {
                if (vol->vnodes[j].ino == target_ino) {
                    vol->vnodes[j].ino = 0;
                    break;
                }
            }

            /* Flush metadata */
            ixfs_write_inode(vol, pv->ino, &pv->inode);
            ixfs_flush_bitmap(vol);
            ixfs_flush_superblock(vol);

            /* Invalidate hash index — will rebuild on next finddir */
            ixfs_hash_free(pv);

            kfree(data_buf);
            return 0;
        }
    }

    kfree(data_buf);
    return -1;  /* file not found */
}

/* Rename a file or directory within the same parent directory */
int ixfs_rename(struct vfs_node *parent, const char *old_name,
               const char *new_name)
{
    struct ixfs_vnode *pv = (struct ixfs_vnode *)parent->fs_data;
    struct ixfs_volume *vol = pv->vol;
    uint32_t total_entries;
    uint32_t i;
    uint8_t *data_buf;

    if (!pv || !old_name || !new_name)
        return -1;

    total_entries = pv->inode.i_size / sizeof(struct ixfs_dir_entry);

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    for (i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t bi = byte_off / IXFS_BLOCK_SIZE;
        uint32_t bo = byte_off % IXFS_BLOCK_SIZE;
        uint32_t dir_block;
        struct ixfs_dir_entry *de;

        dir_block = ixfs_get_block(vol, &pv->inode, bi);
        if (dir_block == 0) break;

        if (ixfs_read_block(vol, dir_block, data_buf) != 0) break;

        de = (struct ixfs_dir_entry *)(data_buf + bo);

        if (de->d_inode != 0 && ixfs_strcmp(de->d_name, old_name)) {
            /* Found — update the name */
            ixfs_strcpy(de->d_name, new_name, IXFS_MAX_NAME);
            ixfs_write_block(vol, dir_block, data_buf);

            /* Update parent timestamps */
            pv->inode.i_mtime = (uint32_t)uptime();
            ixfs_write_inode(vol, pv->ino, &pv->inode);

            kfree(data_buf);
            return 0;
        }
    }

    kfree(data_buf);
    return -1;  /* not found */
}

static struct vfs_ops ixfs_dir_ops = {
    .open    = ixfs_file_open,
    .close   = ixfs_file_close,
    .read    = (void *)0,
    .write   = (void *)0,
    .readdir = ixfs_readdir,
    .finddir = ixfs_finddir,
    .create  = ixfs_create,
    .unlink  = ixfs_unlink,
};

/* --- FS driver descriptor --- */
static struct vfs_fs_driver ixfs_driver = {
    .name      = "IXFS",
    .ops       = &ixfs_dir_ops,
    .priv_data = (void *)0,
};

/* ========================================================================
 * Public API
 * ======================================================================== */

int ixfs_format(const struct blkdev *dev, const char *volume_name)
{
    struct ixfs_volume *vol;
    uint32_t vi;

    /* Find a free volume slot */
    vol = (struct ixfs_volume *)0;
    for (vi = 0; vi < IXFS_MAX_VOLUMES; vi++) {
        if (!volumes[vi].in_use) {
            vol = &volumes[vi];
            break;
        }
    }
    if (!vol) {
        printk("[FAIL] IXFS: no free volume slot\n");
        return -1;
    }

    uint32_t total_blocks;
    uint32_t bitmap_blocks_needed;
    uint32_t inode_blocks;
    uint32_t used_blocks;
    uint32_t i;
    struct ixfs_inode root_inode;
    struct ixfs_dir_entry root_dirs[2];
    uint32_t root_data_block;
    uint8_t *p;

    vol->dev = dev;
    ixfs_cache_init(vol);
    total_blocks = (uint32_t)(dev->sector_count / IXFS_SECTORS_PER_BLK);

    /* Calculate layout */
    bitmap_blocks_needed = (total_blocks + (IXFS_BLOCK_SIZE * 8) - 1)
                         / (IXFS_BLOCK_SIZE * 8);
    inode_blocks = (IXFS_DEFAULT_INODES * sizeof(struct ixfs_inode)
                   + IXFS_BLOCK_SIZE - 1) / IXFS_BLOCK_SIZE;

    used_blocks = 1                    /* superblock */
                + bitmap_blocks_needed /* bitmap */
                + inode_blocks         /* inodes */
                + IXFS_JOURNAL_BLOCKS  /* journal */
                + 1;                   /* root directory data block */

    /* Build superblock */
    p = (uint8_t *)&vol->sb;
    for (i = 0; i < sizeof(struct ixfs_superblock); i++)
        p[i] = 0;

    vol->sb.s_magic         = IXFS_MAGIC;
    vol->sb.s_version       = IXFS_VERSION;
    vol->sb.s_block_size    = IXFS_BLOCK_SIZE;
    vol->sb.s_total_blocks  = total_blocks;
    vol->sb.s_free_blocks   = total_blocks - used_blocks;
    vol->sb.s_total_inodes  = IXFS_DEFAULT_INODES;
    vol->sb.s_free_inodes   = IXFS_DEFAULT_INODES - 2; /* inode 0 reserved, inode 1 root */
    vol->sb.s_bitmap_start  = 1;
    vol->sb.s_bitmap_blocks = bitmap_blocks_needed;
    vol->sb.s_inode_start   = 1 + bitmap_blocks_needed;
    vol->sb.s_inode_blocks  = inode_blocks;
    vol->sb.s_data_start    = 1 + bitmap_blocks_needed + inode_blocks
                             + IXFS_JOURNAL_BLOCKS;
    vol->sb.s_root_inode    = IXFS_ROOT_INODE;
    vol->sb.s_journal_start = 1 + bitmap_blocks_needed + inode_blocks;
    vol->sb.s_journal_blocks = IXFS_JOURNAL_BLOCKS;
    vol->sb.s_journal_seq   = 0;

    if (volume_name)
        ixfs_strcpy((char *)vol->sb.s_volume_name, volume_name, 32);
    else
        ixfs_strcpy((char *)vol->sb.s_volume_name, "IXFS", 32);

    /* Write superblock */
    if (ixfs_flush_superblock(vol) != 0) {
        printk("[FAIL] IXFS format: cannot write superblock\n");
        return -1;
    }

    /* Zero the bitmap blocks */
    for (i = 0; i < bitmap_blocks_needed; i++) {
        if (ixfs_zero_block(vol, vol->sb.s_bitmap_start + i) != 0)
            return -1;
    }

    /* Zero the inode table blocks */
    for (i = 0; i < inode_blocks; i++) {
        if (ixfs_zero_block(vol, vol->sb.s_inode_start + i) != 0)
            return -1;
    }

    /* Set up in-memory bitmap */
    vol->bitmap_bytes = (total_blocks + 7) / 8;
    vol->block_bitmap = (uint8_t *)kmalloc(vol->bitmap_bytes);
    if (!vol->block_bitmap) return -1;

    for (i = 0; i < vol->bitmap_bytes; i++)
        vol->block_bitmap[i] = 0;

    /* Mark metadata blocks as used in bitmap */
    for (i = 0; i < used_blocks; i++)
        bitmap_set(vol->block_bitmap, i);

    /* Flush bitmap to disk */
    ixfs_flush_bitmap(vol);

    /* Initialize block group descriptors */
    ixfs_init_groups(vol);

    /* Initialize write-ahead log */
    ixfs_journal_init(vol);

    /* Create root directory inode (inode 1) */
    p = (uint8_t *)&root_inode;
    for (i = 0; i < sizeof(struct ixfs_inode); i++)
        p[i] = 0;

    root_data_block = vol->sb.s_data_start; /* first data block */
    root_inode.i_mode      = IXFS_S_DIR | IXFS_PERM_DIR;
    root_inode.i_links     = 1;
    root_inode.i_size      = 2 * sizeof(struct ixfs_dir_entry); /* . and .. */
    root_inode.i_blocks    = 1;
    root_inode.i_extents[0].e_start = (uint64_t)root_data_block;
    root_inode.i_extents[0].e_count = 1;
    root_inode.i_extent_count = 1;

    if (ixfs_write_inode(vol, IXFS_ROOT_INODE, &root_inode) != 0)
        return -1;

    /* Write root directory data block with . and .. */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        vol->blk_buf[i] = 0;

    p = (uint8_t *)&root_dirs[0];
    for (i = 0; i < sizeof(root_dirs); i++)
        p[i] = 0;

    root_dirs[0].d_inode = IXFS_ROOT_INODE;
    ixfs_strcpy(root_dirs[0].d_name, ".", IXFS_MAX_NAME);
    root_dirs[1].d_inode = IXFS_ROOT_INODE;
    ixfs_strcpy(root_dirs[1].d_name, "..", IXFS_MAX_NAME);

    p = (uint8_t *)root_dirs;
    for (i = 0; i < sizeof(root_dirs); i++)
        vol->blk_buf[i] = p[i];

    if (ixfs_write_block(vol, root_data_block, vol->blk_buf) != 0)
        return -1;

    printk("[OK] IXFS formatted: %u blocks, %u inodes, \"%s\"\n",
           (uint64_t)total_blocks,
           (uint64_t)IXFS_DEFAULT_INODES,
           vol->sb.s_volume_name);

    vol->in_use = 1;
    return 0;
}

int ixfs_init(const struct blkdev *dev)
{
    struct ixfs_volume *vol;
    uint32_t vi;

    /* Find a free volume slot */
    vol = (struct ixfs_volume *)0;
    for (vi = 0; vi < IXFS_MAX_VOLUMES; vi++) {
        if (!volumes[vi].in_use) {
            vol = &volumes[vi];
            break;
        }
    }
    if (!vol) {
        printk("[FAIL] IXFS: no free volume slot\n");
        return -1;
    }

    uint32_t i;
    uint8_t *p;

    vol->dev = dev;
    ixfs_cache_init(vol);
    vol->vnode_count = 0;

    /* Read block 0 (superblock) */
    if (ixfs_read_block(vol, 0, vol->blk_buf) != 0) {
        printk("[FAIL] IXFS: cannot read superblock\n");
        return -1;
    }

    /* Copy superblock from buffer */
    p = (uint8_t *)&vol->sb;
    for (i = 0; i < sizeof(struct ixfs_superblock); i++)
        p[i] = vol->blk_buf[i];

    /* Verify magic */
    if (vol->sb.s_magic != IXFS_MAGIC) {
        printk("[FAIL] IXFS: bad magic (0x%x, expected 0x%x)\n",
               (uint64_t)vol->sb.s_magic, (uint64_t)IXFS_MAGIC);
        return -1;
    }

    if (vol->sb.s_version != IXFS_VERSION) {
        printk("[FAIL] IXFS: unsupported version %u\n",
               (uint64_t)vol->sb.s_version);
        return -1;
    }

    /* Load block bitmap into memory */
    vol->bitmap_bytes = (vol->sb.s_total_blocks + 7) / 8;
    vol->block_bitmap = (uint8_t *)kmalloc(vol->bitmap_bytes);
    if (!vol->block_bitmap) {
        printk("[FAIL] IXFS: cannot allocate bitmap (%u bytes)\n",
               (uint64_t)vol->bitmap_bytes);
        return -1;
    }

    for (i = 0; i < vol->bitmap_bytes; i++)
        vol->block_bitmap[i] = 0;

    for (i = 0; i < vol->sb.s_bitmap_blocks; i++) {
        uint32_t offset = i * IXFS_BLOCK_SIZE;
        uint32_t remaining = vol->bitmap_bytes - offset;
        uint32_t j;

        if (remaining > IXFS_BLOCK_SIZE)
            remaining = IXFS_BLOCK_SIZE;

        if (ixfs_read_block(vol, vol->sb.s_bitmap_start + i, vol->blk_buf) != 0) {
            kfree(vol->block_bitmap);
            return -1;
        }

        for (j = 0; j < remaining; j++)
            vol->block_bitmap[offset + j] = vol->blk_buf[j];
    }

    /* Initialize block group descriptors from bitmap */
    ixfs_init_groups(vol);

    /* Recover journal (replay committed, discard incomplete) */
    ixfs_journal_recover(vol);

    /* Create root vnode */
    {
        struct ixfs_vnode *root = ixfs_get_vnode(vol, IXFS_ROOT_INODE);
        if (!root) {
            printk("[FAIL] IXFS: cannot read root inode\n");
            kfree(vol->block_bitmap);
            return -1;
        }
        ixfs_strcpy(root->node.name, "C:\\", VFS_MAX_NAME);
        root->node.type = VFS_DIRECTORY | VFS_MOUNTPOINT;
    }

    {
        uint64_t vol_mb = (uint64_t)vol->sb.s_total_blocks * IXFS_BLOCK_SIZE
                        / (1024 * 1024);
        printk("[OK] IXFS: \"%s\" v%u, %u MiB, %u/%u blocks free, %u inodes\n",
               vol->sb.s_volume_name,
               (uint64_t)vol->sb.s_version,
               vol_mb,
               (uint64_t)vol->sb.s_free_blocks,
               (uint64_t)vol->sb.s_total_blocks,
               (uint64_t)vol->sb.s_total_inodes);
    }

    vol->in_use = 1;
    return 0;
}

struct vfs_fs_driver *ixfs_get_driver(void)
{
    return &ixfs_driver;
}

struct vfs_node *ixfs_get_root(void)
{
    uint32_t vi;
    struct ixfs_volume *vol = (struct ixfs_volume *)0;
    for (vi = 0; vi < IXFS_MAX_VOLUMES; vi++) {
        if (volumes[vi].in_use) {
            vol = &volumes[vi];
            break;
        }
    }
    if (!vol) return (struct vfs_node *)0;

    if (vol->vnode_count == 0)
        return (struct vfs_node *)0;
    return &vol->vnodes[0].node;  /* root is always vnode[0] */
}

int ixfs_check_perm(const struct ixfs_inode *inode, uint16_t uid,
                    uint16_t gid, int want_write)
{
    uint16_t perm;

    /* Root (uid 0) bypasses all permission checks */
    if (uid == 0) return 0;

    /* Determine which permission bits to check */
    if (uid == inode->i_uid) {
        /* Owner */
        perm = (inode->i_mode >> 6) & 0x7;
    } else if (gid == inode->i_gid) {
        /* Group */
        perm = (inode->i_mode >> 3) & 0x7;
    } else {
        /* Other */
        perm = inode->i_mode & 0x7;
    }

    /* Check: read requires bit 2 (r), write requires bit 1 (w) */
    if (want_write) {
        return (perm & 0x2) ? 0 : -1;
    } else {
        return (perm & 0x4) ? 0 : -1;
    }
}

/* ============================================================================
 * ixfs_test_performance — Targeted tests for block groups, cache, hash index
 * ============================================================================ */

void ixfs_test_performance(void)
{
    int pass;
    uint32_t vi;
    struct ixfs_volume *vol = (struct ixfs_volume *)0;
    for (vi = 0; vi < IXFS_MAX_VOLUMES; vi++) {
        if (volumes[vi].in_use) {
            vol = &volumes[vi];
            break;
        }
    }
    if (!vol) {
        printk("\n  [SKIP] No IXFS volume mounted\n\n");
        return;
    }

    printk("\n  --- IXFS Performance Features Test ---\n");

    /* --- Test 1: Block Group Allocator --- */
    {
        uint32_t b1, b2, b3;
        uint32_t g1, g2, g3;
        uint32_t saved_free = vol->sb.s_free_blocks;

        b1 = ixfs_alloc_block(vol);
        b2 = ixfs_alloc_block(vol);
        b3 = ixfs_alloc_block(vol);

        g1 = b1 / IXFS_BLOCKS_PER_GROUP;
        g2 = b2 / IXFS_BLOCKS_PER_GROUP;
        g3 = b3 / IXFS_BLOCKS_PER_GROUP;

        /* Locality: all 3 in same group */
        pass = (b1 != 0 && b2 != 0 && b3 != 0 && g1 == g2 && g2 == g3);
        printk("  [%s] Block groups: alloc 3 blocks -> group %u, locality=%s\n",
               pass ? "OK" : "FAIL", (uint64_t)g1, pass ? "yes" : "NO");

        /* Hint advancement: blocks should be sequential */
        pass = (b2 == b1 + 1 && b3 == b2 + 1);
        printk("  [%s] Block groups: hint b%u->b%u->b%u, sequential=%s\n",
               pass ? "OK" : "FAIL",
               (uint64_t)b1, (uint64_t)b2, (uint64_t)b3,
               pass ? "yes" : "NO");

        /* Free and verify count restored */
        ixfs_free_block(vol, b1);
        ixfs_free_block(vol, b2);
        ixfs_free_block(vol, b3);

        pass = (vol->sb.s_free_blocks == saved_free);
        printk("  [%s] Block groups: free restored (%u/%u)\n",
               pass ? "OK" : "FAIL",
               (uint64_t)vol->sb.s_free_blocks, (uint64_t)saved_free);

        /* Hint regression */
        pass = (g1 < vol->group_count && vol->groups[g1].bg_next_free <= b1);
        printk("  [%s] Block groups: hint regression (hint=%u, freed=%u)\n",
               pass ? "OK" : "FAIL",
               (uint64_t)vol->groups[g1].bg_next_free, (uint64_t)b1);
    }

    /* --- Test 2: Buffer Cache --- */
    {
        uint32_t test_blk;
        uint8_t write_buf[IXFS_BLOCK_SIZE];
        uint8_t read_buf1[IXFS_BLOCK_SIZE];
        uint8_t read_buf2[IXFS_BLOCK_SIZE];
        uint32_t i;
        int match;

        test_blk = ixfs_alloc_block(vol);
        if (test_blk == 0) {
            printk("  [FAIL] Cache: cannot allocate test block\n");
        } else {
            /* Write a known pattern via cache */
            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                write_buf[i] = (uint8_t)(i & 0xFF);
            ixfs_write_block(vol, test_blk, write_buf);

            /* Two reads — both should come from cache */
            ixfs_read_block(vol, test_blk, read_buf1);
            ixfs_read_block(vol, test_blk, read_buf2);

            match = 1;
            for (i = 0; i < IXFS_BLOCK_SIZE; i++) {
                if (read_buf1[i] != write_buf[i] ||
                    read_buf2[i] != write_buf[i]) {
                    match = 0;
                    break;
                }
            }

            printk("  [%s] Buffer cache: write->read integrity=%s (block %u)\n",
                   match ? "OK" : "FAIL",
                   match ? "ok" : "CORRUPTED", (uint64_t)test_blk);

            /* Check dirty state */
            {
                struct ixfs_cache_entry *ce = ixfs_cache_find(vol, test_blk);
                pass = (ce != (struct ixfs_cache_entry *)0 && ce->dirty == 1);
                printk("  [%s] Buffer cache: cached=%s, dirty=%s\n",
                       pass ? "OK" : "FAIL",
                       ce ? "yes" : "no",
                       (ce && ce->dirty) ? "yes" : "no");
            }

            /* Flush and verify clean */
            ixfs_cache_flush(vol);
            {
                struct ixfs_cache_entry *ce = ixfs_cache_find(vol, test_blk);
                pass = (ce != (struct ixfs_cache_entry *)0 && ce->dirty == 0);
                printk("  [%s] Buffer cache: post-flush dirty=%s\n",
                       pass ? "OK" : "FAIL",
                       (ce && ce->dirty) ? "yes" : "no");
            }

            ixfs_free_block(vol, test_blk);
        }
    }

    /* --- Test 3: Directory Hash Index --- */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            struct vfs_node *tdir;
            uint32_t created = 0, found = 0, i;
            char fname[8];

            c_root->ops->create(c_root, "_hashtest", VFS_DIRECTORY);
            tdir = c_root->ops->finddir(c_root, "_hashtest");

            if (!tdir) {
                printk("  [FAIL] Hash index: cannot create _hashtest dir\n");
            } else {
                /* Create 70 files (exceeds threshold of 64) */
                for (i = 0; i < 70; i++) {
                    fname[0] = 'h'; fname[1] = 'f'; fname[2] = '_';
                    fname[3] = (char)('0' + (i / 10));
                    fname[4] = (char)('0' + (i % 10));
                    fname[5] = '\0';
                    if (tdir->ops && tdir->ops->create &&
                        tdir->ops->create(tdir, fname, VFS_FILE) == 0)
                        created++;
                }

                printk("  [%s] Hash index: created %u/70 files (threshold=%u)\n",
                       created >= 65 ? "OK" : "FAIL",
                       (uint64_t)created, (uint64_t)IXFS_HASH_THRESHOLD);

                /* Lookup 7 files (every 10th) — triggers hash build */
                for (i = 0; i < 70; i += 10) {
                    fname[0] = 'h'; fname[1] = 'f'; fname[2] = '_';
                    fname[3] = (char)('0' + (i / 10));
                    fname[4] = (char)('0' + (i % 10));
                    fname[5] = '\0';
                    if (tdir->ops->finddir(tdir, fname))
                        found++;
                }

                pass = (found == 7);
                printk("  [%s] Hash index: finddir found %u/7 via hash\n",
                       pass ? "OK" : "FAIL", (uint64_t)found);

                /* Verify hash table was actually built */
                {
                    struct ixfs_vnode *tv;
                    tv = (struct ixfs_vnode *)tdir->fs_data;
                    pass = (tv && tv->dir_hash != (void *)0);
                    if (pass) {
                        printk("  [OK] Hash index: table built (%u nodes, %u buckets)\n",
                               (uint64_t)tv->dir_hash->node_count,
                               (uint64_t)IXFS_HASH_BUCKETS);
                    } else {
                        printk("  [FAIL] Hash index: table NOT built\n");
                    }
                }

                /* Cleanup */
                for (i = 0; i < 70; i++) {
                    fname[0] = 'h'; fname[1] = 'f'; fname[2] = '_';
                    fname[3] = (char)('0' + (i / 10));
                    fname[4] = (char)('0' + (i % 10));
                    fname[5] = '\0';
                    if (tdir->ops && tdir->ops->unlink)
                        tdir->ops->unlink(tdir, fname);
                }
                c_root->ops->unlink(c_root, "_hashtest");
            }
        }
    } else {
        printk("  [SKIP] Hash index: C:\\ not mounted\n");
    }

    /* --- Test 4: Extent-Based Allocation --- */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            int rc = c_root->ops->create(c_root, "_extent_test.dat", VFS_FILE);
            if (rc != 0) {
                printk("  [FAIL] Extents: cannot create test file\n");
            } else {
                struct vfs_node *f = vfs_open("C:\\_extent_test.dat", VFS_O_WRITE);
                if (f) {
                    struct ixfs_vnode *fv;
                    uint8_t wbuf[128];
                    uint32_t wi;

                    for (wi = 0; wi < 128; wi++)
                        wbuf[wi] = (uint8_t)(wi & 0xFF);

                    /* Write 5 blocks: each write goes to a different block */
                    for (wi = 0; wi < 5; wi++)
                        vfs_write(f, wi * IXFS_BLOCK_SIZE, 128, wbuf);

                    vfs_close(f);

                    /* Check extent layout */
                    f = vfs_open("C:\\_extent_test.dat", VFS_O_READ);
                    if (f) {
                        fv = (struct ixfs_vnode *)f->fs_data;
                        pass = (fv->inode.i_extent_count == 1 &&
                                fv->inode.i_extents[0].e_count == 5);
                        printk("  [%s] Extents: 5 blocks merged into %u extent(s)",
                               pass ? "OK" : "FAIL",
                               (uint64_t)fv->inode.i_extent_count);
                        printk(" (count=%u)\n",
                               (uint64_t)fv->inode.i_extents[0].e_count);

                        printk("  [OK] Extents: 64-bit block addressing");
                        printk(" (start=%u, max 64 TiB)\n",
                               (uint64_t)fv->inode.i_extents[0].e_start);

                        /* Verify data integrity */
                        {
                            uint8_t rbuf[128];
                            int n = vfs_read(f, 0, 128, rbuf);
                            int ok = 1;
                            if (n == 128) {
                                for (wi = 0; wi < 128; wi++) {
                                    if (rbuf[wi] != (uint8_t)(wi & 0xFF)) {
                                        ok = 0; break;
                                    }
                                }
                            } else { ok = 0; }
                            printk("  [%s] Extents: read-back data integrity\n",
                                   ok ? "OK" : "FAIL");
                        }
                        vfs_close(f);
                    }
                }
                c_root->ops->unlink(c_root, "_extent_test.dat");
            }
        }
    }
    /* --- Test 5: Write-Ahead Log (Journal) --- */
    {
        uint8_t *jbuf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
        if (jbuf) {
            struct ixfs_journal_header *jh;
            uint32_t j_start = vol->sb.s_journal_start;
            uint32_t j_blocks = vol->sb.s_journal_blocks;

            /* Re-read superblock from disk to get latest values */
            if (j_start == 0) {
                struct ixfs_superblock fresh_sb;
                uint8_t *sbuf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
                if (sbuf) {
                    ixfs_disk_read(vol, 0, sbuf);
                    {
                        uint8_t *d = (uint8_t *)&fresh_sb;
                        uint32_t k;
                        for (k = 0; k < sizeof(struct ixfs_superblock); k++)
                            d[k] = sbuf[k];
                    }
                    j_start = fresh_sb.s_journal_start;
                    j_blocks = fresh_sb.s_journal_blocks;
                    /* Update volume's superblock with correct values */
                    vol->sb.s_journal_start = j_start;
                    vol->sb.s_journal_blocks = j_blocks;
                    vol->sb.s_journal_seq = fresh_sb.s_journal_seq;
                    kfree(sbuf);
                }
            }

            if (j_start != 0) {
                /* Read journal header from disk */
                ixfs_disk_read(vol, j_start, jbuf);
                jh = (struct ixfs_journal_header *)jbuf;
                pass = (jh->jh_magic == IXFS_JOURNAL_MAGIC);
                printk("  [%s] Journal: header magic=0x%x",
                       pass ? "OK" : "FAIL",
                       (uint64_t)jh->jh_magic);
                printk(" (area=%u blocks at block %u)\n",
                       (uint64_t)j_blocks,
                       (uint64_t)j_start);

                /* Test transaction cycle */
                {
                    uint32_t pre_seq = vol->sb.s_journal_seq;
                    ixfs_txn_begin(vol);
                    pass = (vol->txn.active == 1 &&
                            vol->txn.txn_id == pre_seq + 1);
                    printk("  [%s] Journal: txn_begin (id=%u, active=%s)\n",
                           pass ? "OK" : "FAIL",
                           (uint64_t)vol->txn.txn_id,
                           vol->txn.active ? "yes" : "no");

                    /* Commit empty transaction */
                    ixfs_txn_commit(vol);
                    pass = (vol->txn.active == 0 &&
                            vol->sb.s_journal_seq == pre_seq + 1);
                    printk("  [%s] Journal: txn_commit (seq=%u, active=%s)\n",
                           pass ? "OK" : "FAIL",
                           (uint64_t)vol->sb.s_journal_seq,
                           vol->txn.active ? "yes" : "no");
                }
            } else {
                printk("  [SKIP] Journal: no journal area on this volume\n");
            }
            kfree(jbuf);
        }
    }

    printk("  --- IXFS Performance Tests Complete ---\n\n");
}
