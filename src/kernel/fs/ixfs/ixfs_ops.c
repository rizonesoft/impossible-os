/* ============================================================================
 * ixfs_ops.c -- VFS file and directory operations
 * ============================================================================ */

#include "ixfs_internal.h"

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

    /* Inline fast path: data is stored directly in i_extents[] */
    if (v->inode.i_extent_flags & IXFS_INLINE) {
        const uint8_t *src = (const uint8_t *)v->inode.i_extents;
        uint32_t i;
        for (i = 0; i < size; i++)
            buffer[i] = src[offset + i];
        v->inode.i_atime = (uint32_t)uptime();
        ixfs_write_inode(vol, v->ino, &v->inode);
        return (int)size;
    }

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    blk_index = offset / IXFS_BLOCK_SIZE;
    blk_offset = offset % IXFS_BLOCK_SIZE;

    while (bytes_read < size) {
        uint32_t disk_block = ixfs_get_block(vol, &v->inode, blk_index);
        uint32_t to_read;
        uint32_t i;

        if (disk_block == 0) {
            /* Sparse hole: return zeroes without disk I/O */
            uint32_t to_read = IXFS_BLOCK_SIZE - blk_offset;
            if (to_read > size - bytes_read)
                to_read = size - bytes_read;
            for (i = 0; i < to_read; i++)
                buffer[bytes_read + i] = 0;
            bytes_read += to_read;
            blk_offset = 0;
            blk_index++;
            continue;
        }

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

    if (vol->read_only) return -1;
    uint8_t *data_buf;

    if (!v) return -1;

    /* Inline path: data fits entirely within the 48-byte extent area */
    if (v->inode.i_extent_flags & IXFS_INLINE) {
        uint32_t end = offset + size;

        if (end <= IXFS_INLINE_MAX) {
            /* Still fits inline -- write directly to i_extents[] */
            uint8_t *dst = (uint8_t *)v->inode.i_extents;
            uint32_t i;
            for (i = 0; i < size; i++)
                dst[offset + i] = buffer[i];
            if (end > v->inode.i_size)
                v->inode.i_size = end;
            v->inode.i_mtime = (uint32_t)uptime();
            v->inode.i_atime = v->inode.i_mtime;
            v->node.size = v->inode.i_size;
            ixfs_write_inode(vol, v->ino, &v->inode);
            return (int)size;
        }

        /* Promotion: data exceeds 48 bytes -- move to extent-based.
         * 1. Save current inline data
         * 2. Clear inline flag and extent area
         * 3. Allocate a block, write old data + first block of new data
         * 4. Fall through to the normal write loop for the rest */
        {
            uint8_t saved[IXFS_INLINE_MAX];
            uint32_t old_size = v->inode.i_size;
            uint32_t disk_block;
            uint32_t i;
            uint32_t first_chunk;

            /* Save existing inline data */
            {
                uint8_t *src = (uint8_t *)v->inode.i_extents;
                for (i = 0; i < IXFS_INLINE_MAX; i++)
                    saved[i] = (i < old_size) ? src[i] : 0;
            }

            /* Clear inline state */
            v->inode.i_extent_flags &= (uint8_t)~IXFS_INLINE;
            v->inode.i_extent_count = 0;
            v->inode.i_size = 0;
            v->inode.i_blocks = 0;
            {
                uint8_t *p = (uint8_t *)v->inode.i_extents;
                for (i = 0; i < IXFS_INLINE_MAX; i++)
                    p[i] = 0;
            }

            /* Allocate first block */
            disk_block = ixfs_add_block_to_extent(vol, &v->inode);
            if (disk_block == 0) return -1;

            /* Build the first block: old inline data + new data (capped) */
            data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
            if (!data_buf) return -1;
            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                data_buf[i] = 0;

            /* Copy old inline data into block */
            for (i = 0; i < old_size; i++)
                data_buf[i] = saved[i];

            /* Copy first chunk of new data (only up to block boundary) */
            first_chunk = IXFS_BLOCK_SIZE - offset;
            if (first_chunk > size)
                first_chunk = size;
            for (i = 0; i < first_chunk; i++)
                data_buf[offset + i] = buffer[i];

            ixfs_write_block(vol, disk_block, data_buf);
            kfree(data_buf);

            bytes_written = first_chunk;
            v->inode.i_size = (offset + first_chunk > old_size)
                            ? offset + first_chunk : old_size;
            v->node.size = v->inode.i_size;

            /* If the entire write fit in the first block, we're done */
            if (bytes_written >= size) {
                v->inode.i_mtime = (uint32_t)uptime();
                v->inode.i_atime = v->inode.i_mtime;
                ixfs_txn_begin(vol);
                ixfs_write_inode(vol, v->ino, &v->inode);
                ixfs_flush_bitmap(vol);
                ixfs_flush_superblock(vol);
                ixfs_txn_commit(vol);
                return (int)bytes_written;
            }

            /* Fall through to normal write loop for remaining data */
        }
    }

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    /* After promotion, bytes_written may be > 0; compute from current pos */
    blk_index = (offset + bytes_written) / IXFS_BLOCK_SIZE;
    blk_offset = (offset + bytes_written) % IXFS_BLOCK_SIZE;

    while (bytes_written < size) {
        uint32_t disk_block = ixfs_get_block(vol, &v->inode, blk_index);
        uint32_t to_write;
        uint32_t i;

        /* Allocate a new block if needed */
        if (disk_block == 0) {
            /* Sparse gap: if writing past current file blocks, insert hole */
            if (blk_index > v->inode.i_blocks) {
                uint32_t gap = blk_index - v->inode.i_blocks;
                uint32_t ec = v->inode.i_extent_count;
                /* Insert hole extent (e_start=0, e_count=gap) */
                if (ec < IXFS_INLINE_EXTENTS) {
                    v->inode.i_extents[ec].e_start = 0;
                    v->inode.i_extents[ec].e_count = gap;
                    v->inode.i_extent_count = ec + 1;
                    v->inode.i_size = blk_index * IXFS_BLOCK_SIZE;
                }
            }
            disk_block = ixfs_add_block_to_extent(vol, &v->inode);
            if (disk_block == 0) break;

            /* Zero the new block */
            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                data_buf[i] = 0;
        } else {
            /* CoW: if block is shared by a snapshot, copy to new block */
            disk_block = ixfs_cow_block(vol, &v->inode, blk_index,
                                         disk_block);
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

/* Forward declarations for file ops */
static int ixfs_vfs_stat(struct vfs_node *node, struct vfs_stat *st);
static int ixfs_vfs_truncate(struct vfs_node *node, uint64_t new_size);
static int ixfs_vfs_flush(struct vfs_node *node);

struct vfs_ops ixfs_file_ops = {
    .open    = ixfs_file_open,
    .close   = ixfs_file_close,
    .read    = ixfs_file_read,
    .write   = ixfs_file_write,
    .readdir = (void *)0,
    .finddir = (void *)0,
    .create  = (void *)0,
    .unlink  = (void *)0,
    .rename  = (void *)0,
    .stat    = ixfs_vfs_stat,
    .truncate = ixfs_vfs_truncate,
    .mkdir   = (void *)0,
    .rmdir   = (void *)0,
    .set_attr = (void *)0,
    .set_times = (void *)0,
    .flush   = ixfs_vfs_flush,
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
            uint32_t d_ino;

            if (disk_block != 0 &&
                ixfs_read_block(vol, disk_block, data_buf) == 0) {
                de = (struct ixfs_dir_entry *)(data_buf + blk_off);
                /* `de` aliases data_buf, so the inode number must be read
                 * BEFORE the free -- reading it after is a use-after-free
                 * whose value is deterministically 0 under
                 * HEAP_ZERO_ON_FREE (heap.c scrubs the whole payload), and
                 * ixfs_get_vnode(vol, 0) then fabricates a zero-size inode-0
                 * vnode that is returned as if it were the file. */
                d_ino = de->d_inode;
                kfree(data_buf);
                found = ixfs_get_vnode(vol, d_ino);
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
            /* Same aliasing rule as the hash branch above: read before free. */
            uint32_t d_ino = de->d_inode;

            kfree(data_buf);
            found = ixfs_get_vnode(vol, d_ino);
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

    if (vol->read_only) return -1;
    struct ixfs_inode new_inode;
    uint32_t dir_block;
    uint8_t *data_buf;
    struct ixfs_dir_entry *de;
    uint32_t total_entries;
    uint32_t i;
    int found_slot = -1;

    if (!pv) return -1;

    /* Idempotent: if an entry with this name already exists, succeed silently.
     * This prevents boot-time code from creating duplicate directory entries
     * when directories already exist from mkfs. */
    {
        struct vfs_node *existing = ixfs_finddir(parent, name);
        if (existing)
            return 0;
    }

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

    /* Regular files start as inline (no block allocation needed) */
    if (type != VFS_DIRECTORY)
        new_inode.i_extent_flags = IXFS_INLINE;

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
        /* Append at end -- may need to grow the directory */
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

    /* Invalidate hash index -- will rebuild on next finddir */
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

            /* Invalidate hash index -- will rebuild on next finddir */
            ixfs_hash_free(pv);

            kfree(data_buf);
            return 0;
        }
    }

    kfree(data_buf);
    return -1;  /* file not found */
}

/* Rename a file or directory within the same parent directory.
 * IXFS rejects VFS_RENAME_REPLACE_EXISTING until ixfs_rename grows
 * destination-collision handling (current impl rewrites d_name without
 * checking dst, which would create duplicate dirents). */
int ixfs_rename(struct vfs_node *parent, const char *old_name,
               const char *new_name, uint32_t flags)
{
    struct ixfs_vnode *pv = (struct ixfs_vnode *)parent->fs_data;
    struct ixfs_volume *vol = pv->vol;
    uint32_t total_entries;
    uint32_t i;
    uint8_t *data_buf;

    if (!pv || !old_name || !new_name)
        return -1;

    if (flags & VFS_RENAME_REPLACE_EXISTING) {
        klog(LOG_WARN, "ixfs",
             "rename: REPLACE_EXISTING not supported (refused)");
        return -1;
    }

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
            /* Found -- update the name */
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

/* VFS-compatible stat: called by vfs_stat() */
static int ixfs_vfs_stat(struct vfs_node *node, struct vfs_stat *st)
{
    struct ixfs_vnode *v;
    if (!node || !st || !node->fs_data)
        return -1;

    v = (struct ixfs_vnode *)node->fs_data;
    st->size   = v->inode.i_size;
    st->type   = node->type;
    st->ctime  = v->inode.i_ctime;
    st->mtime  = v->inode.i_mtime;
    st->atime  = v->inode.i_atime;
    st->blocks = v->inode.i_blocks;
    return 0;
}

/* VFS-compatible truncate: called by vfs_truncate() */
static int ixfs_vfs_truncate(struct vfs_node *node, uint64_t new_size)
{
    struct ixfs_vnode *v;
    struct ixfs_volume *vol;
    uint32_t vi;

    if (!node || !node->fs_data)
        return -1;

    /* Only truncate files */
    if (node->type & VFS_DIRECTORY)
        return -1;

    v = (struct ixfs_vnode *)node->fs_data;

    /* Find the volume */
    vol = (struct ixfs_volume *)0;
    for (vi = 0; volumes && vi < IXFS_MAX_VOLUMES; vi++) {
        if (volumes[vi].in_use) {
            vol = &volumes[vi];
            break;
        }
    }
    if (!vol)
        return -1;

    if (new_size == 0) {
        /* Truncate-to-zero: free all extents */
        ixfs_free_all_extents(vol, &v->inode);
        v->inode.i_size = 0;
    } else if (new_size < v->inode.i_size) {
        /* Partial truncate: free trailing blocks */
        uint32_t new_blocks = ((uint32_t)new_size + IXFS_BLOCK_SIZE - 1)
                            / IXFS_BLOCK_SIZE;
        uint32_t old_blocks = v->inode.i_blocks;
        uint32_t blk;

        /* Free blocks beyond new_blocks */
        for (blk = new_blocks; blk < old_blocks; blk++) {
            uint32_t disk_blk = ixfs_get_block(vol, &v->inode, blk);
            if (disk_blk > 0)
                ixfs_free_block(vol, disk_blk);
        }

        /* Update extent metadata */
        {
            uint32_t kept = 0;
            uint32_t ei;
            /* Clamp the bound: a corrupt i_extent_count must not index
             * past the inline i_extents[] array. */
            uint32_t ne = ixfs_extent_count_clamped(&v->inode);
            for (ei = 0; ei < ne; ei++) {
                uint32_t ext_count = v->inode.i_extents[ei].e_count;
                if (kept + ext_count <= new_blocks) {
                    kept += ext_count;
                } else {
                    /* Trim this extent */
                    uint32_t keep = new_blocks - kept;
                    v->inode.i_extents[ei].e_count = keep;
                    /* Zero remaining extents */
                    for (ei++; ei < ne; ei++) {
                        v->inode.i_extents[ei].e_start = 0;
                        v->inode.i_extents[ei].e_count = 0;
                    }
                    break;
                }
            }
        }

        v->inode.i_size = new_size;
        v->inode.i_blocks = new_blocks;
    } else {
        /* Extend: just update size (sparse file) */
        v->inode.i_size = new_size;
    }

    v->inode.i_mtime = (uint32_t)uptime();
    node->size = v->inode.i_size;
    ixfs_write_inode(vol, v->ino, &v->inode);
    return 0;
}

/* VFS-compatible mkdir: creates a directory via ixfs_create */
static int ixfs_vfs_mkdir(struct vfs_node *parent, const char *name)
{
    return ixfs_create(parent, name, VFS_DIRECTORY);
}

/* VFS-compatible rmdir: ixfs_unlink already checks for empty directories */
static int ixfs_vfs_rmdir(struct vfs_node *parent, const char *name)
{
    return ixfs_unlink(parent, name);
}

/* VFS-compatible set_attr: update permission bits in inode i_mode */
static int ixfs_vfs_set_attr(struct vfs_node *node, uint32_t attributes)
{
    struct ixfs_vnode *v;
    if (!node || !node->fs_data)
        return -1;

    v = (struct ixfs_vnode *)node->fs_data;

    /* Update lower 12 bits (permissions), preserve type bits */
    v->inode.i_mode = (v->inode.i_mode & IXFS_S_TYPEMASK)
                    | (uint16_t)(attributes & 0x0FFF);
    v->inode.i_mtime = (uint32_t)uptime();
    ixfs_write_inode(v->vol, v->ino, &v->inode);
    return 0;
}

/* VFS-compatible set_times: update inode timestamps selectively */
static int ixfs_vfs_set_times(struct vfs_node *node,
                              const filetime_t *ctime_p,
                              const filetime_t *mtime_p,
                              const filetime_t *atime_p)
{
    struct ixfs_vnode *v;
    if (!node || !node->fs_data)
        return -1;

    v = (struct ixfs_vnode *)node->fs_data;

    if (ctime_p)
        v->inode.i_ctime = ctime_p->seconds;
    if (mtime_p)
        v->inode.i_mtime = mtime_p->seconds;
    if (atime_p)
        v->inode.i_atime = atime_p->seconds;

    ixfs_write_inode(v->vol, v->ino, &v->inode);
    return 0;
}

/* VFS-compatible flush: flush bitmap and superblock to disk */
static int ixfs_vfs_flush(struct vfs_node *node)
{
    struct ixfs_vnode *v;
    if (!node || !node->fs_data)
        return -1;

    v = (struct ixfs_vnode *)node->fs_data;
    ixfs_txn_begin(v->vol);
    ixfs_flush_bitmap(v->vol);
    ixfs_flush_superblock(v->vol);
    ixfs_txn_commit(v->vol);
    return 0;
}

struct vfs_ops ixfs_dir_ops = {
    .open    = ixfs_file_open,
    .close   = ixfs_file_close,
    .read    = (void *)0,
    .write   = (void *)0,
    .readdir = ixfs_readdir,
    .finddir = ixfs_finddir,
    .create  = ixfs_create,
    .unlink  = ixfs_unlink,
    .rename  = ixfs_rename,
    .stat    = ixfs_vfs_stat,
    .truncate = (void *)0,
    .mkdir   = ixfs_vfs_mkdir,
    .rmdir   = ixfs_vfs_rmdir,
    .set_attr = ixfs_vfs_set_attr,
    .set_times = ixfs_vfs_set_times,
    .flush   = ixfs_vfs_flush,
};
