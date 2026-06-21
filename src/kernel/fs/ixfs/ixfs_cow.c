/* ============================================================================
 * ixfs_cow.c -- Copy-on-Write, refcounts, snapshots, and stat
 * ============================================================================ */

#include "ixfs_internal.h"

/* Initialize refcount table (all blocks start at refcount 1 if allocated, 0 if free) */
int ixfs_refcount_init(struct ixfs_volume *vol)
{
    uint32_t i;

    vol->refcount_bytes = vol->sb.s_total_blocks;
    vol->refcount_table = (uint8_t *)kmalloc(vol->refcount_bytes);
    if (!vol->refcount_table) return -1;

    /* Set refcount = 1 for allocated blocks, 0 for free */
    for (i = 0; i < vol->refcount_bytes; i++) {
        if (bitmap_test(vol->block_bitmap, i))
            vol->refcount_table[i] = 1;
        else
            vol->refcount_table[i] = 0;
    }
    return 0;
}

/* Flush refcount table to disk */
int ixfs_refcount_flush(struct ixfs_volume *vol)
{
    uint32_t i, j;
    uint32_t base = vol->sb.s_refcount_start;

    for (i = 0; i < IXFS_REFCOUNT_BLOCKS; i++) {
        for (j = 0; j < IXFS_BLOCK_SIZE; j++) {
            uint32_t idx = i * IXFS_BLOCK_SIZE + j;
            if (idx < vol->refcount_bytes)
                vol->blk_buf[j] = vol->refcount_table[idx];
            else
                vol->blk_buf[j] = 0;
        }
        if (ixfs_write_block(vol, base + i, vol->blk_buf) != 0)
            return -1;
    }
    return 0;
}

/* Load refcount table from disk */
int ixfs_refcount_load(struct ixfs_volume *vol)
{
    uint32_t i, j;
    uint32_t base = vol->sb.s_refcount_start;
    uint8_t *tmp = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!tmp) return -1;

    vol->refcount_bytes = vol->sb.s_total_blocks;
    vol->refcount_table = (uint8_t *)kmalloc(vol->refcount_bytes);
    if (!vol->refcount_table) { kfree(tmp); return -1; }

    for (i = 0; i < IXFS_REFCOUNT_BLOCKS; i++) {
        if (ixfs_read_block(vol, base + i, tmp) != 0) {
            kfree(tmp);
            return -1;
        }
        for (j = 0; j < IXFS_BLOCK_SIZE; j++) {
            uint32_t idx = i * IXFS_BLOCK_SIZE + j;
            if (idx < vol->refcount_bytes)
                vol->refcount_table[idx] = tmp[j];
        }
    }

    /* If all zeros, initialize from bitmap */
    {
        int all_zero = 1;
        for (i = 0; i < vol->refcount_bytes && all_zero; i++) {
            if (vol->refcount_table[i] != 0)
                all_zero = 0;
        }
        if (all_zero) {
            for (i = 0; i < vol->refcount_bytes; i++) {
                if (bitmap_test(vol->block_bitmap, i))
                    vol->refcount_table[i] = 1;
            }
            /* Metadata blocks are always in use even if bitmap is unset */
            for (i = 0; i < vol->sb.s_data_start && i < vol->refcount_bytes; i++)
                vol->refcount_table[i] = 1;
        }
    }

    kfree(tmp);
    return 0;
}

/* Flush snapshot table to disk */
int ixfs_snapshot_flush(struct ixfs_volume *vol)
{
    uint32_t i;
    uint8_t *src;

    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        vol->blk_buf[i] = 0;

    src = (uint8_t *)vol->snapshots;
    for (i = 0; i < sizeof(vol->snapshots) && i < IXFS_BLOCK_SIZE; i++)
        vol->blk_buf[i] = src[i];

    return ixfs_write_block(vol, vol->sb.s_snapshot_start, vol->blk_buf);
}

/* Load snapshot table from disk */
int ixfs_snapshot_load(struct ixfs_volume *vol)
{
    uint8_t *tmp = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    uint8_t *dst;
    uint32_t i;
    if (!tmp) return -1;

    if (ixfs_read_block(vol, vol->sb.s_snapshot_start, tmp) != 0) {
        kfree(tmp);
        return -1;
    }

    dst = (uint8_t *)vol->snapshots;
    for (i = 0; i < sizeof(vol->snapshots); i++)
        dst[i] = tmp[i];

    kfree(tmp);
    return 0;
}

/* CoW: if the target block has refcount > 1, allocate a new block and copy data.
 * Updates the inode extent and returns the new block number. */
uint32_t ixfs_cow_block(struct ixfs_volume *vol,
                                struct ixfs_inode *inode,
                                uint32_t file_block_idx,
                                uint32_t old_disk_block)
{
    uint32_t new_blk;
    uint8_t *copy_buf;
    uint32_t i, offset;

    if (!vol->refcount_table || old_disk_block == 0)
        return old_disk_block;

    if (old_disk_block >= vol->refcount_bytes)
        return old_disk_block;

    if (vol->refcount_table[old_disk_block] <= 1)
        return old_disk_block;  /* not shared, no CoW needed */

    /* Shared block -- copy on write */
    new_blk = ixfs_alloc_block(vol);
    if (new_blk == 0)
        return old_disk_block;  /* allocation failed, write in-place */

    /* Copy old block data to new block */
    copy_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!copy_buf) {
        ixfs_free_block(vol, new_blk);
        return old_disk_block;
    }

    if (ixfs_read_block(vol, old_disk_block, copy_buf) == 0)
        ixfs_write_block(vol, new_blk, copy_buf);
    kfree(copy_buf);

    /* Set refcount for new block */
    if (new_blk < vol->refcount_bytes)
        vol->refcount_table[new_blk] = 1;

    /* Decrement old block's refcount */
    vol->refcount_table[old_disk_block]--;
    if (vol->refcount_table[old_disk_block] == 0) {
        /* No more references, free the block */
        bitmap_clear(vol->block_bitmap, old_disk_block);
        vol->sb.s_free_blocks++;
    }

    /* Update the extent that references old_disk_block. Bound the search
     * by the clamped count so a corrupt i_extent_count cannot over-read;
     * the loop returns after splitting the target extent. */
    offset = 0;
    {
    uint32_t cow_n = ixfs_extent_count_clamped(inode);
    for (i = 0; i < cow_n; i++) {
        uint32_t ext_start = (uint32_t)inode->i_extents[i].e_start;
        uint32_t ext_count = inode->i_extents[i].e_count;

        if (file_block_idx >= offset &&
            file_block_idx < offset + ext_count) {
            uint32_t inner = file_block_idx - offset;

            if (ext_count == 1) {
                /* Single-block extent: just update start */
                inode->i_extents[i].e_start = (uint64_t)new_blk;
            } else if (inner == 0) {
                /* First block: shrink extent, insert new single extent */
                inode->i_extents[i].e_start = (uint64_t)(ext_start + 1);
                inode->i_extents[i].e_count = ext_count - 1;
                /* Add new extent if space */
                if (inode->i_extent_count < IXFS_INLINE_EXTENTS) {
                    /* Shift extents up */
                    uint32_t k;
                    for (k = inode->i_extent_count; k > i; k--)
                        inode->i_extents[k] = inode->i_extents[k - 1];
                    inode->i_extents[i].e_start = (uint64_t)new_blk;
                    inode->i_extents[i].e_count = 1;
                    inode->i_extent_count++;
                }
            } else if (inner == ext_count - 1) {
                /* Last block: shrink extent, append new */
                inode->i_extents[i].e_count = ext_count - 1;
                if (inode->i_extent_count < IXFS_INLINE_EXTENTS) {
                    uint32_t k;
                    for (k = inode->i_extent_count; k > i + 1; k--)
                        inode->i_extents[k] = inode->i_extents[k - 1];
                    inode->i_extents[i + 1].e_start = (uint64_t)new_blk;
                    inode->i_extents[i + 1].e_count = 1;
                    inode->i_extent_count++;
                }
            } else {
                /* Middle: simplified -- just update the start for now.
                 * Full split would require 3 extents; for simplicity
                 * we update the block reference directly. */
                inode->i_extents[i].e_start = (uint64_t)new_blk;
                inode->i_extents[i].e_count = 1;
            }
            break;
        }
        offset += ext_count;
    }
    }

    return new_blk;
}

/* Get volume pointer (for snapshot API) */
struct ixfs_volume *ixfs_get_active_volume(void)
{
    uint32_t vi;
    for (vi = 0; volumes && vi < IXFS_MAX_VOLUMES; vi++) {
        if (volumes[vi].in_use)
            return &volumes[vi];
    }
    return (struct ixfs_volume *)0;
}

/* Create a snapshot: save inode table + increment all block refcounts */
int ixfs_snapshot_create(const char *name)
{
    struct ixfs_volume *vol = ixfs_get_active_volume();
    uint32_t i, si;
    uint32_t saved_block;
    uint8_t *buf;

    if (!vol || !vol->refcount_table) return -1;

    /* Find free snapshot slot */
    si = IXFS_MAX_SNAPSHOTS;
    for (i = 0; i < IXFS_MAX_SNAPSHOTS; i++) {
        if (vol->snapshots[i].se_flags == 0) {
            si = i;
            break;
        }
    }
    if (si == IXFS_MAX_SNAPSHOTS) {
        klog(LOG_ERROR, "ixfs", "IXFS snapshot: no free slot (max %u)",
               (uint64_t)IXFS_MAX_SNAPSHOTS);
        return -1;
    }

    /* Allocate a block for saving the inode table */
    saved_block = ixfs_alloc_block(vol);
    if (saved_block == 0) {
        klog(LOG_ERROR, "ixfs", "IXFS snapshot: cannot allocate inode backup block");
        return -1;
    }

    /* Copy current inode table to the saved block */
    buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!buf) {
        ixfs_free_block(vol, saved_block);
        return -1;
    }

    /* Read current inode table block(s) and save them */
    for (i = 0; i < vol->sb.s_inode_blocks; i++) {
        if (ixfs_read_block(vol, vol->sb.s_inode_start + i, buf) == 0) {
            /* Save to allocated block + offset */
            ixfs_write_block(vol, saved_block + i, buf);
        }
    }
    kfree(buf);

    /* Increment refcount for all allocated data blocks */
    for (i = vol->sb.s_data_start; i < vol->sb.s_total_blocks; i++) {
        if (bitmap_test(vol->block_bitmap, i)) {
            if (i < vol->refcount_bytes && vol->refcount_table[i] < 255)
                vol->refcount_table[i]++;
        }
    }

    /* Fill snapshot entry */
    ixfs_strcpy(vol->snapshots[si].se_name, name, IXFS_SNAP_NAME_LEN);
    vol->snapshots[si].se_timestamp = (uint32_t)uptime();
    vol->snapshots[si].se_root_block = saved_block;
    vol->snapshots[si].se_inode_blocks = vol->sb.s_inode_blocks;
    vol->snapshots[si].se_flags = 1;
    vol->sb.s_snapshot_count++;

    /* Flush to disk */
    ixfs_snapshot_flush(vol);
    ixfs_refcount_flush(vol);
    ixfs_flush_superblock(vol);

    klog(LOG_DEBUG, "ixfs", "IXFS snapshot \"%s\" created (slot %u, block %u)",
           name, (uint64_t)si, (uint64_t)saved_block);
    return 0;
}

/* List all active snapshots */
int ixfs_snapshot_list(void)
{
    struct ixfs_volume *vol = ixfs_get_active_volume();
    uint32_t i;
    uint32_t count = 0;

    if (!vol) return -1;

    klog(LOG_INFO, "ixfs", "Snapshots:");
    for (i = 0; i < IXFS_MAX_SNAPSHOTS; i++) {
        if (vol->snapshots[i].se_flags != 0) {
            klog(LOG_INFO, "ixfs", "  [%u] \"%s\" (time=%u, block=%u)",
                   (uint64_t)i,
                   vol->snapshots[i].se_name,
                   (uint64_t)vol->snapshots[i].se_timestamp,
                   (uint64_t)vol->snapshots[i].se_root_block);
            count++;
        }
    }
    if (count == 0)
        klog(LOG_INFO, "ixfs", "  (none)");
    return (int)count;
}

/* Restore a snapshot: swap current inode table with saved copy */
int ixfs_snapshot_restore(const char *name)
{
    struct ixfs_volume *vol = ixfs_get_active_volume();
    uint32_t i, si;
    uint8_t *buf;

    if (!vol) return -1;

    /* Find the named snapshot */
    si = IXFS_MAX_SNAPSHOTS;
    for (i = 0; i < IXFS_MAX_SNAPSHOTS; i++) {
        if (vol->snapshots[i].se_flags != 0) {
            uint32_t j;
            int match = 1;
            for (j = 0; j < IXFS_SNAP_NAME_LEN; j++) {
                if (vol->snapshots[i].se_name[j] != name[j]) {
                    match = 0;
                    break;
                }
                if (name[j] == '\0') break;
            }
            if (match) { si = i; break; }
        }
    }
    if (si == IXFS_MAX_SNAPSHOTS) {
        klog(LOG_ERROR, "ixfs", "IXFS snapshot \"%s\" not found", name);
        return -1;
    }

    buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!buf) return -1;

    /* Restore: copy saved inode table back to active location */
    for (i = 0; i < vol->snapshots[si].se_inode_blocks; i++) {
        if (ixfs_read_block(vol, vol->snapshots[si].se_root_block + i, buf) == 0) {
            ixfs_write_block(vol, vol->sb.s_inode_start + i, buf);
        }
    }
    kfree(buf);

    /* Flush changes */
    ixfs_cache_flush(vol);

    /* Reload vnodes from disk so in-memory state matches */
    for (i = 0; i < vol->vnode_count; i++) {
        ixfs_read_inode(vol, vol->vnodes[i].ino, &vol->vnodes[i].inode);
        vol->vnodes[i].node.size = vol->vnodes[i].inode.i_size;
    }

    klog(LOG_DEBUG, "ixfs", "IXFS snapshot \"%s\" restored", name);
    return 0;
}

/* Delete a snapshot: decrement refcounts, free unreferenced blocks */
int ixfs_snapshot_delete(const char *name)
{
    struct ixfs_volume *vol = ixfs_get_active_volume();
    uint32_t i, si;

    if (!vol || !vol->refcount_table) return -1;

    /* Find the named snapshot */
    si = IXFS_MAX_SNAPSHOTS;
    for (i = 0; i < IXFS_MAX_SNAPSHOTS; i++) {
        if (vol->snapshots[i].se_flags != 0) {
            uint32_t j;
            int match = 1;
            for (j = 0; j < IXFS_SNAP_NAME_LEN; j++) {
                if (vol->snapshots[i].se_name[j] != name[j]) {
                    match = 0;
                    break;
                }
                if (name[j] == '\0') break;
            }
            if (match) { si = i; break; }
        }
    }
    if (si == IXFS_MAX_SNAPSHOTS) {
        klog(LOG_ERROR, "ixfs", "IXFS snapshot \"%s\" not found", name);
        return -1;
    }

    /* Decrement refcount for all allocated data blocks */
    for (i = vol->sb.s_data_start; i < vol->sb.s_total_blocks; i++) {
        if (i < vol->refcount_bytes && vol->refcount_table[i] > 1)
            vol->refcount_table[i]--;
    }

    /* Free the saved inode table block(s) */
    for (i = 0; i < vol->snapshots[si].se_inode_blocks; i++)
        ixfs_free_block(vol, vol->snapshots[si].se_root_block + i);

    /* Clear snapshot entry */
    {
        uint8_t *p = (uint8_t *)&vol->snapshots[si];
        for (i = 0; i < sizeof(struct ixfs_snapshot_entry); i++)
            p[i] = 0;
    }
    if (vol->sb.s_snapshot_count > 0)
        vol->sb.s_snapshot_count--;

    /* Flush changes */
    ixfs_snapshot_flush(vol);
    ixfs_refcount_flush(vol);
    ixfs_flush_superblock(vol);

    klog(LOG_DEBUG, "ixfs", "IXFS snapshot \"%s\" deleted", name);
    return 0;
}

/* Stat: report file metadata including sparse info */
int ixfs_stat(struct vfs_node *node, uint32_t *logical_size,
              uint32_t *actual_blocks)
{
    struct ixfs_vnode *v;
    if (!node || !node->fs_data) return -1;
    v = (struct ixfs_vnode *)node->fs_data;

    if (logical_size)
        *logical_size = v->inode.i_size;
    if (actual_blocks)
        *actual_blocks = v->inode.i_blocks;
    return 0;
}
