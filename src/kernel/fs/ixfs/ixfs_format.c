/* ============================================================================
 * ixfs_format.c — Format, init, public API, FS driver descriptor
 * ============================================================================ */

#include "ixfs_internal.h"

/* Global volume table */
struct ixfs_volume volumes[IXFS_MAX_VOLUMES];

/* --- FS driver descriptor --- */
static struct vfs_fs_driver ixfs_driver = {
    .name      = "IXFS",
    .ops       = &ixfs_dir_ops,
    .priv_data = (void *)0,
};

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
    uint32_t checksum_blocks;
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
    /* Checksum table: 1 uint32_t per block, 1024 entries per 4K block */
    checksum_blocks = (total_blocks * sizeof(uint32_t) + IXFS_BLOCK_SIZE - 1)
                    / IXFS_BLOCK_SIZE;

    used_blocks = 1                    /* superblock */
                + bitmap_blocks_needed /* bitmap */
                + checksum_blocks      /* checksum table */
                + inode_blocks         /* inodes */
                + IXFS_JOURNAL_BLOCKS  /* journal */
                + IXFS_REFCOUNT_BLOCKS /* refcount table */
                + IXFS_SNAPSHOT_BLOCKS /* snapshot table */
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
    vol->sb.s_checksum_start = 1 + bitmap_blocks_needed;
    vol->sb.s_checksum_blocks = checksum_blocks;
    vol->sb.s_inode_start   = 1 + bitmap_blocks_needed + checksum_blocks;
    vol->sb.s_inode_blocks  = inode_blocks;
    vol->sb.s_data_start    = 1 + bitmap_blocks_needed + checksum_blocks
                             + inode_blocks
                             + IXFS_JOURNAL_BLOCKS
                             + IXFS_REFCOUNT_BLOCKS
                             + IXFS_SNAPSHOT_BLOCKS;
    vol->sb.s_root_inode    = IXFS_ROOT_INODE;
    vol->sb.s_journal_start = 1 + bitmap_blocks_needed + checksum_blocks
                             + inode_blocks;
    vol->sb.s_journal_blocks = IXFS_JOURNAL_BLOCKS;
    vol->sb.s_journal_seq   = 0;
    vol->sb.s_refcount_start = vol->sb.s_journal_start + IXFS_JOURNAL_BLOCKS;
    vol->sb.s_refcount_blocks = IXFS_REFCOUNT_BLOCKS;
    vol->sb.s_snapshot_start = vol->sb.s_refcount_start + IXFS_REFCOUNT_BLOCKS;
    vol->sb.s_snapshot_count = 0;

    if (volume_name)
        ixfs_strcpy((char *)vol->sb.s_volume_name, volume_name, 32);
    else
        ixfs_strcpy((char *)vol->sb.s_volume_name, "IXFS", 32);

    /* Compute superblock self-checksum (CRC32C of bytes 0..103) */
    vol->sb.s_checksum = 0;  /* zero the field before computing */
    vol->sb.s_checksum = ixfs_crc32c(&vol->sb, 104);

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

    /* Initialize refcount table (all metadata blocks at refcount 1) */
    ixfs_refcount_init(vol);
    ixfs_refcount_flush(vol);

    /* Zero and initialize checksum table */
    for (i = 0; i < checksum_blocks; i++) {
        if (ixfs_zero_block(vol, vol->sb.s_checksum_start + i) != 0)
            return -1;
    }
    vol->checksum_count = total_blocks;
    vol->checksum_table = (uint32_t *)kmalloc(total_blocks * sizeof(uint32_t));
    if (vol->checksum_table) {
        for (i = 0; i < total_blocks; i++)
            vol->checksum_table[i] = 0;
    }

    /* Initialize snapshot table (empty) */
    {
        uint8_t *p2 = (uint8_t *)vol->snapshots;
        for (i = 0; i < sizeof(vol->snapshots); i++)
            p2[i] = 0;
    }
    ixfs_snapshot_flush(vol);

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

    /* Load refcount table and snapshot table */
    if (vol->sb.s_refcount_start != 0)
        ixfs_refcount_load(vol);
    else
        ixfs_refcount_init(vol);
    if (vol->sb.s_snapshot_start != 0)
        ixfs_snapshot_load(vol);

    /* Load checksum table */
    if (vol->sb.s_checksum_start != 0 && vol->sb.s_checksum_blocks != 0)
        ixfs_checksum_load(vol);

    /* Verify superblock self-checksum */
    {
        uint32_t saved = vol->sb.s_checksum;
        vol->sb.s_checksum = 0;
        uint32_t computed = ixfs_crc32c(&vol->sb, 104);
        vol->sb.s_checksum = saved;
        if (saved != 0 && computed != saved) {
            printk("[WARN] IXFS: superblock checksum mismatch "
                   "(stored=0x%x, computed=0x%x)\n",
                   (uint64_t)saved, (uint64_t)computed);
        }
    }

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

/* Full-volume integrity scan: verify CRC32C checksums on all allocated blocks.
 * Returns the number of corrupted blocks found (0 = clean). */
int ixfs_scrub(void)
{
    struct ixfs_volume *vol = ixfs_get_active_volume();
    uint32_t corrupted = 0;
    uint32_t checked = 0;
    uint32_t i;
    uint8_t *buf;

    if (!vol || !vol->checksum_table) {
        printk("[SKIP] IXFS scrub: no checksum table\n");
        return -1;
    }

    buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!buf) return -1;

    for (i = 0; i < vol->sb.s_total_blocks; i++) {
        uint32_t stored, computed;

        /* Skip metadata blocks (superblock, bitmap, checksum table,
         * inode table, journal, refcount, snapshot) — these are managed
         * by subsystems that use direct I/O and bypass checksums */
        if (i < vol->sb.s_data_start)
            continue;

        /* Only check allocated blocks with nonzero checksums */
        if (!bitmap_test(vol->block_bitmap, i))
            continue;
        stored = vol->checksum_table[i];
        if (stored == 0)
            continue;

        /* Read directly from disk (bypass cache for integrity) */
        if (ixfs_disk_read(vol, i, buf) != 0)
            continue;

        computed = ixfs_crc32c(buf, IXFS_BLOCK_SIZE);
        if (computed != stored) {
            printk("[WARN] IXFS scrub: block %u corrupted "
                   "(stored=0x%x, computed=0x%x)\n",
                   (uint64_t)i, (uint64_t)stored, (uint64_t)computed);
            corrupted++;
        }
        checked++;
    }

    kfree(buf);
    return (int)corrupted;
}
