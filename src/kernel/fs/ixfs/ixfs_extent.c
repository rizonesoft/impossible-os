/* ============================================================================
 * ixfs_extent.c — Extent-based block lookup and allocation
 * ============================================================================ */

#include "ixfs_internal.h"

/* Given a file block index, return the disk block number via extent search.
 * Extents with e_start == 0 represent holes (sparse regions). */
uint32_t ixfs_get_block(struct ixfs_volume *vol, const struct ixfs_inode *inode, uint32_t index)
{
    uint32_t i;
    uint32_t file_offset = 0;

    (void)vol;  /* not needed for inline extents */

    /* Inline files have no disk blocks — data lives in i_extents[] */
    if (inode->i_extent_flags & IXFS_INLINE)
        return 0;

    for (i = 0; i < inode->i_extent_count; i++) {
        uint32_t count = inode->i_extents[i].e_count;
        if (count == 0) continue;

        if (index < file_offset + count) {
            /* Block is within this extent */
            if (inode->i_extents[i].e_start == 0)
                return 0;  /* Hole extent: sparse region */
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
uint32_t ixfs_add_block_to_extent(struct ixfs_volume *vol,
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
void ixfs_free_all_extents(struct ixfs_volume *vol,
                                   struct ixfs_inode *inode)
{
    uint32_t i, j;

    /* Inline files have no disk blocks — just zero the data area */
    if (inode->i_extent_flags & IXFS_INLINE) {
        uint8_t *data = (uint8_t *)inode->i_extents;
        for (i = 0; i < IXFS_INLINE_MAX; i++)
            data[i] = 0;
        inode->i_extent_flags &= (uint8_t)~IXFS_INLINE;
        inode->i_extent_count = 0;
        inode->i_blocks = 0;
        inode->i_size = 0;
        return;
    }

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
