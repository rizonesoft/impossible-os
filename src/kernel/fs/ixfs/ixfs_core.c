/* ============================================================================
 * ixfs_core.c -- String helpers, raw disk I/O, and buffer cache
 * ============================================================================ */

#include "ixfs_internal.h"

/* --- Internal: string helpers --- */

void ixfs_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

int ixfs_strcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* --- Raw disk I/O (low-level, used by cache layer only) --- */

/* Read a single IXFS block directly from disk (4 KiB = 8 sectors) */
int ixfs_disk_read(struct ixfs_volume *vol, uint32_t block, void *buf)
{
    uint64_t lba = (uint64_t)block * IXFS_SECTORS_PER_BLK;
    return blkdev_read(vol->dev, lba, IXFS_SECTORS_PER_BLK, buf);
}

/* Write a single IXFS block directly to disk (4 KiB = 8 sectors) */
int ixfs_disk_write(struct ixfs_volume *vol, uint32_t block, const void *buf)
{
    uint64_t lba = (uint64_t)block * IXFS_SECTORS_PER_BLK;
    return blkdev_write(vol->dev, lba, IXFS_SECTORS_PER_BLK, buf);
}

/* --- Buffer cache (LRU, write-back) --- */

/* Initialize the cache (call on format/mount) */
void ixfs_cache_init(struct ixfs_volume *vol)
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
struct ixfs_cache_entry *ixfs_cache_find(struct ixfs_volume *vol, uint32_t block)
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
struct ixfs_cache_entry *ixfs_cache_lru(struct ixfs_volume *vol)
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
int ixfs_cache_flush(struct ixfs_volume *vol)
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
    /* Flush checksum table to disk after dirty blocks are written */
    if (vol->checksum_table)
        ixfs_checksum_flush(vol);
    return err;
}

/* --- Cached block I/O (used by all ixfs code) --- */

/* Read a block via the cache. Returns 0 on success. */
int ixfs_read_block(struct ixfs_volume *vol, uint32_t block, void *buf)
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

    /* Cache miss -- find slot (evict LRU if needed) */
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

    /* Verify checksum on data just read from disk */
    ixfs_checksum_verify(vol, block, ce->data);

    /* Copy to caller's buffer */
    {
        uint8_t *dst = (uint8_t *)buf;
        const uint8_t *src = ce->data;
        for (i = 0; i < IXFS_BLOCK_SIZE; i++)
            dst[i] = src[i];
    }
    return 0;
}

/* Write a block via the cache (deferred -- only marks dirty). */
int ixfs_write_block(struct ixfs_volume *vol, uint32_t block, const void *buf)
{
    struct ixfs_cache_entry *ce;
    uint32_t i;
    const uint8_t *src = (const uint8_t *)buf;

    /* Check if already cached */
    ce = ixfs_cache_find(vol, block);
    if (!ce) {
        /* Not cached -- get a slot */
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

    /* Update checksum for this block */
    ixfs_checksum_update(vol, block, buf);

    return 0;
}

/* Zero a block (via cache -- deferred to disk) */
int ixfs_zero_block(struct ixfs_volume *vol, uint32_t block)
{
    uint32_t i;
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        vol->blk_buf[i] = 0;
    return ixfs_write_block(vol, block, vol->blk_buf);
}

/* --- CRC32C (software, Castagnoli polynomial 0x82F63B78) --- */

uint32_t ixfs_crc32c(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i, j;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0x82F63B78;
            else
                crc = crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/* --- Per-block checksum table --- */

/* Load checksum table from disk into memory */
int ixfs_checksum_load(struct ixfs_volume *vol)
{
    uint32_t start = vol->sb.s_checksum_start;
    uint32_t blocks = vol->sb.s_checksum_blocks;
    uint32_t total = vol->sb.s_total_blocks;
    uint32_t i, bi;

    if (start == 0 || blocks == 0)
        return -1;

    vol->checksum_count = total;
    vol->checksum_table = (uint32_t *)kmalloc(total * sizeof(uint32_t));
    if (!vol->checksum_table)
        return -1;

    /* Zero the table first */
    for (i = 0; i < total; i++)
        vol->checksum_table[i] = 0;

    /* Read from disk (each block holds 1024 uint32_t entries) */
    for (bi = 0; bi < blocks; bi++) {
        uint8_t *buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
        if (!buf) return -1;

        if (ixfs_disk_read(vol, start + bi, buf) != 0) {
            kfree(buf);
            return -1;
        }

        /* Copy entries from this block */
        {
            uint32_t *entries = (uint32_t *)buf;
            uint32_t base = bi * (IXFS_BLOCK_SIZE / sizeof(uint32_t));
            uint32_t count = IXFS_BLOCK_SIZE / sizeof(uint32_t);
            for (i = 0; i < count && (base + i) < total; i++)
                vol->checksum_table[base + i] = entries[i];
        }
        kfree(buf);
    }
    return 0;
}

/* Flush checksum table from memory to disk */
int ixfs_checksum_flush(struct ixfs_volume *vol)
{
    uint32_t start = vol->sb.s_checksum_start;
    uint32_t blocks = vol->sb.s_checksum_blocks;
    uint32_t total = vol->checksum_count;
    uint32_t bi, i;

    if (!vol->checksum_table || start == 0)
        return -1;

    for (bi = 0; bi < blocks; bi++) {
        uint8_t *buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
        if (!buf) return -1;

        /* Zero the block buffer */
        for (i = 0; i < IXFS_BLOCK_SIZE; i++)
            buf[i] = 0;

        /* Copy entries into this block */
        {
            uint32_t *entries = (uint32_t *)buf;
            uint32_t base = bi * (IXFS_BLOCK_SIZE / sizeof(uint32_t));
            uint32_t count = IXFS_BLOCK_SIZE / sizeof(uint32_t);
            for (i = 0; i < count && (base + i) < total; i++)
                entries[i] = vol->checksum_table[base + i];
        }

        ixfs_disk_write(vol, start + bi, buf);
        kfree(buf);
    }
    return 0;
}

/* Update the checksum for a single block (called on write) */
void ixfs_checksum_update(struct ixfs_volume *vol, uint32_t block,
                           const void *data)
{
    if (vol->checksum_table && block < vol->checksum_count)
        vol->checksum_table[block] = ixfs_crc32c(data, IXFS_BLOCK_SIZE);
}

/* Verify the checksum for a single block (called on read from disk).
 * Returns 0 on match, -1 on mismatch. Logs warning but does not fail. */
int ixfs_checksum_verify(struct ixfs_volume *vol, uint32_t block,
                          const void *data)
{
    uint32_t stored, computed;

    if (!vol->checksum_table || block >= vol->checksum_count)
        return 0;  /* no table -- skip check */

    /* Skip metadata blocks: superblock, bitmap, checksum table, inode table,
     * journal, refcount table, snapshot table. These are managed by subsystems
     * that write via ixfs_disk_write() directly, bypassing checksum updates. */
    if (block < vol->sb.s_data_start)
        return 0;

    stored = vol->checksum_table[block];
    if (stored == 0)
        return 0;  /* uninitialized entry -- skip */

    computed = ixfs_crc32c(data, IXFS_BLOCK_SIZE);
    if (computed != stored) {
        printk("[WARN] IXFS: checksum mismatch on block %u "
               "(stored=0x%x, computed=0x%x)\n",
               (uint64_t)block, (uint64_t)stored, (uint64_t)computed);
        return -1;
    }
    return 0;
}
