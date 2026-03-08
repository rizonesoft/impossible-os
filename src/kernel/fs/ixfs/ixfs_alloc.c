/* ============================================================================
 * ixfs_alloc.c — Bitmap operations, block groups, alloc/free, flush
 * ============================================================================ */

#include "ixfs_internal.h"

/* --- Internal: bitmap operations --- */

void bitmap_set(uint8_t *bmap, uint32_t bit)
{
    bmap[bit / 8] |= (uint8_t)(1 << (bit % 8));
}

void bitmap_clear(uint8_t *bmap, uint32_t bit)
{
    bmap[bit / 8] &= (uint8_t)~(1 << (bit % 8));
}

int bitmap_test(const uint8_t *bmap, uint32_t bit)
{
    return (bmap[bit / 8] >> (bit % 8)) & 1;
}

/* --- Block group state (in-memory only) --- */

/* Initialize block group descriptors from the bitmap.
 * Must be called after the bitmap is loaded into memory. */
void ixfs_init_groups(struct ixfs_volume *vol)
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
uint32_t ixfs_alloc_block_near(struct ixfs_volume *vol, uint32_t preferred_group)
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
uint32_t ixfs_alloc_block(struct ixfs_volume *vol)
{
    /* Default: prefer group 0 (where data starts), fallback to any */
    uint32_t data_group = vol->sb.s_data_start / IXFS_BLOCKS_PER_GROUP;
    return ixfs_alloc_block_near(vol, data_group);
}

/* Free a block back to the bitmap and update its group descriptor */
void ixfs_free_block(struct ixfs_volume *vol, uint32_t block)
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
int ixfs_flush_bitmap(struct ixfs_volume *vol)
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
int ixfs_flush_superblock(struct ixfs_volume *vol)
{
    uint32_t i;
    uint8_t *sp = (uint8_t *)&vol->sb;

    /* Recompute superblock self-checksum before writing */
    vol->sb.s_checksum = 0;
    vol->sb.s_checksum = ixfs_crc32c(&vol->sb, 112);

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
