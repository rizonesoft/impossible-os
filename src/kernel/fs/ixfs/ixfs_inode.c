/* ============================================================================
 * ixfs_inode.c -- Inode I/O, vnode management, directory hash index
 * ============================================================================ */

#include "ixfs_internal.h"

/* --- Internal: inode I/O --- */

/* Read an inode from disk */
/* THE inode-number boundary for this filesystem. It lives here, beside the
 * block arithmetic it guards, because that arithmetic is what goes wrong:
 * `s_inode_start + ino / inodes_per_block` is computed from `ino` with no
 * validation, so an out-of-table number selects an arbitrary volume block --
 * which a read then interprets as an inode, and a write then OVERWRITES.
 *
 * A caller-side check is NOT enough, and placing one in `ixfs_get_vnode` was
 * the weaker first attempt: several callers reach this I/O with an inode
 * number taken straight from an on-disk directory entry without passing
 * through the vnode cache at all (`ixfs_unlink`'s `target_ino`, the
 * `ixfs_fsck` walks, `ixfs_cow`'s cache refresh), so a crafted directory
 * entry bypasses any guard placed only in that path.
 *
 * FAILS CLOSED on a zero or absent count. `s_total_inodes` is on-disk data and
 * therefore attacker-controlled, so it is also clamped to what the inode table
 * can physically hold: an inflated superblock count must not authorize a read
 * past the table it claims to describe. There is one formatter and it always
 * writes IXFS_DEFAULT_INODES, so a zero count is a corrupt volume rather than
 * a legacy shape needing tolerance. */
static int ixfs_ino_in_table(const struct ixfs_volume *vol, uint32_t ino)
{
    uint32_t per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    /* 64-bit throughout: `s_inode_blocks` and `s_inode_start` are RAW on-disk
     * superblock fields with no mount-time structural validation anywhere in
     * this filesystem (a pre-existing gap, filed separately -- see the TODO
     * XREF at this function). A 32-bit `s_inode_blocks * per_block` can wrap
     * on a crafted volume and read back SMALLER than the true table, which
     * would UNDER-restrict rather than over-restrict -- the opposite of what
     * a bounds check is for. Doing the arithmetic in 64 bits and clamping the
     * final block address is what keeps THIS function honest even though it
     * cannot fix the missing validation one layer up (mount time). */
    uint64_t cap       = vol->sb.s_total_inodes;
    uint64_t phys_cap  = (uint64_t)vol->sb.s_inode_blocks * per_block;
    uint64_t block64;

    if (ino < IXFS_ROOT_INODE)
        return 0;                  /* inode 0 is never a file */
    if (cap == 0 || phys_cap == 0)
        return 0;                  /* fail closed, never fail open */
    if (cap > phys_cap)
        cap = phys_cap;            /* an inflated on-disk count cannot widen the table */
    if ((uint64_t)ino >= cap)
        return 0;

    /* The block address this ino resolves to must also not wrap past
     * UINT32_MAX -- a near-max `s_inode_start` could otherwise wrap the
     * caller's 32-bit addition back into a low, plausible-looking block.
     * STRICT less-than, not <=: `struct ixfs_cache_entry.block == 0xFFFFFFFF`
     * is the cache's reserved EMPTY-ENTRY sentinel (ixfs_internal.h,
     * ixfs_core.c). Admitting that exact value here would let
     * ixfs_read_block treat a genuine inode-table block as an empty cache
     * slot (false cache hit, stale/zeroed contents, no disk I/O) and let
     * ixfs_write_block mark a sentinel entry dirty while flush deliberately
     * skips it -- a write that silently vanishes. */
    block64 = (uint64_t)vol->sb.s_inode_start + (uint64_t)ino / per_block;
    return block64 < 0xFFFFFFFFULL;
}

int ixfs_read_inode(struct ixfs_volume *vol, uint32_t ino, struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block;
    uint32_t offset;
    uint32_t i;
    uint8_t *dst;
    uint8_t *tmp_buf;

    if (!ixfs_ino_in_table(vol, ino))
        return -1;

    block  = vol->sb.s_inode_start + (ino / inodes_per_block);
    offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);

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
int ixfs_write_inode(struct ixfs_volume *vol, uint32_t ino, const struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block;
    uint32_t offset;
    uint32_t i;
    const uint8_t *src;
    uint8_t *tmp_buf;

    /* Same boundary as ixfs_read_inode: an out-of-table `ino` here does not
     * merely misread, it OVERWRITES an arbitrary volume block. */
    if (!ixfs_ino_in_table(vol, ino))
        return -1;

    block  = vol->sb.s_inode_start + (ino / inodes_per_block);
    offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);

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

/* --- FNV-1a hash for directory names --- */

uint32_t ixfs_fnv1a(const char *name)
{
    uint32_t hash = 0x811C9DC5;  /* FNV offset basis */
    while (*name) {
        /* Lowercase fold for case-insensitive hash (matches VFS walk_path fold) */
        uint8_t c = (uint8_t)*name++;
        if (c >= 'A' && c <= 'Z') c += 32;
        hash ^= c;
        hash *= 0x01000193;      /* FNV prime */
    }
    return hash;
}

/* Free a directory hash index */
void ixfs_hash_free(struct ixfs_vnode *v)
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
void ixfs_hash_build(struct ixfs_volume *vol, struct ixfs_vnode *v)
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

    /* Allocate chain nodes -- one per active entry */
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
uint32_t ixfs_hash_lookup(struct ixfs_volume *vol, struct ixfs_vnode *v, const char *name,
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
struct ixfs_vnode *ixfs_get_vnode(struct ixfs_volume *vol, uint32_t ino)
{
    uint32_t i;
    struct ixfs_vnode *v;

    /* Reject before the CACHE SCAN, which is the part `ixfs_read_inode`'s
     * boundary cannot cover: `ixfs_unlink` retires a slot by writing
     * `vol->vnodes[j].ino = 0`, so a lookup for inode 0 MATCHES that retired
     * slot and returns a deleted file's stale node fields without ever
     * reaching disk. The range half of this check is deliberately NOT repeated
     * here -- it belongs to `ixfs_ino_in_table` at the I/O boundary, and the
     * failing `ixfs_read_inode` below rejects an out-of-table number for every
     * caller rather than only this one. */
    if (ino < IXFS_ROOT_INODE)
        return (struct ixfs_vnode *)0;

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
