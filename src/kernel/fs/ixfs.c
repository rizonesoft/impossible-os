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

#include "ixfs.h"
#include "ata.h"
#include "heap.h"
#include "printk.h"

/* Number of inodes to allocate (fixed at format time) */
#define IXFS_DEFAULT_INODES  256

/* Partition LBA offset on the ATA disk */
static uint32_t ixfs_part_lba;

/* In-memory superblock */
static struct ixfs_superblock sb;

/* In-memory block bitmap (loaded at mount) */
static uint8_t *block_bitmap;
static uint32_t bitmap_bytes;

/* Scratch buffer for block-level I/O (4 KiB) */
static uint8_t blk_buf[IXFS_BLOCK_SIZE];

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

/* --- Internal: block I/O via ATA --- */

/* Read a single IXFS block (4 KiB = 8 sectors) */
static int ixfs_read_block(uint32_t block, void *buf)
{
    uint32_t lba = ixfs_part_lba + block * IXFS_SECTORS_PER_BLK;
    uint32_t i;
    uint8_t *p = (uint8_t *)buf;

    for (i = 0; i < IXFS_SECTORS_PER_BLK; i++) {
        if (ata_read_sectors(lba + i, 1, p + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* Write a single IXFS block (4 KiB = 8 sectors) */
static int ixfs_write_block(uint32_t block, const void *buf)
{
    uint32_t lba = ixfs_part_lba + block * IXFS_SECTORS_PER_BLK;
    uint32_t i;
    const uint8_t *p = (const uint8_t *)buf;

    for (i = 0; i < IXFS_SECTORS_PER_BLK; i++) {
        if (ata_write_sectors(lba + i, 1, p + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* Zero a block on disk */
static int ixfs_zero_block(uint32_t block)
{
    uint32_t i;
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        blk_buf[i] = 0;
    return ixfs_write_block(block, blk_buf);
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

/* Allocate a free block from the bitmap. Returns block number or 0 on failure. */
static uint32_t ixfs_alloc_block(void)
{
    uint32_t i;
    for (i = sb.s_data_start; i < sb.s_total_blocks; i++) {
        if (!bitmap_test(block_bitmap, i)) {
            bitmap_set(block_bitmap, i);
            sb.s_free_blocks--;
            return i;
        }
    }
    return 0;  /* no free blocks */
}

/* Free a block back to the bitmap */
static void ixfs_free_block(uint32_t block)
{
    if (block >= sb.s_data_start && block < sb.s_total_blocks) {
        if (bitmap_test(block_bitmap, block)) {
            bitmap_clear(block_bitmap, block);
            sb.s_free_blocks++;
        }
    }
}

/* Flush bitmap to disk */
static int ixfs_flush_bitmap(void)
{
    uint32_t i;
    for (i = 0; i < sb.s_bitmap_blocks; i++) {
        uint32_t offset = i * IXFS_BLOCK_SIZE;
        uint32_t remaining = bitmap_bytes - offset;
        uint32_t j;

        if (remaining > IXFS_BLOCK_SIZE)
            remaining = IXFS_BLOCK_SIZE;

        /* Copy bitmap chunk to buffer, zero-pad */
        for (j = 0; j < remaining; j++)
            blk_buf[j] = block_bitmap[offset + j];
        for (; j < IXFS_BLOCK_SIZE; j++)
            blk_buf[j] = 0;

        if (ixfs_write_block(sb.s_bitmap_start + i, blk_buf) != 0)
            return -1;
    }
    return 0;
}

/* Flush superblock to disk */
static int ixfs_flush_superblock(void)
{
    uint32_t i;
    uint8_t *sp = (uint8_t *)&sb;

    /* Zero the buffer, copy superblock into it */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        blk_buf[i] = 0;
    for (i = 0; i < sizeof(struct ixfs_superblock); i++)
        blk_buf[i] = sp[i];

    return ixfs_write_block(0, blk_buf);
}

/* --- Internal: inode I/O --- */

/* Read an inode from disk */
static int ixfs_read_inode(uint32_t ino, struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block = sb.s_inode_start + (ino / inodes_per_block);
    uint32_t offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);
    uint32_t i;
    uint8_t *dst;
    uint8_t *tmp_buf;

    tmp_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!tmp_buf) return -1;

    if (ixfs_read_block(block, tmp_buf) != 0) {
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
static int ixfs_write_inode(uint32_t ino, const struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block = sb.s_inode_start + (ino / inodes_per_block);
    uint32_t offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);
    uint32_t i;
    const uint8_t *src;
    uint8_t *tmp_buf;

    tmp_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!tmp_buf) return -1;

    /* Read the block first (to preserve other inodes in the same block) */
    if (ixfs_read_block(block, tmp_buf) != 0) {
        kfree(tmp_buf);
        return -1;
    }

    src = (const uint8_t *)inode;
    for (i = 0; i < sizeof(struct ixfs_inode); i++)
        tmp_buf[offset + i] = src[i];

    i = ixfs_write_block(block, tmp_buf);
    kfree(tmp_buf);
    return i;
}

/* --- VFS node storage --- */

#define IXFS_MAX_OPEN_NODES 64

struct ixfs_vnode {
    struct vfs_node    node;
    uint32_t           ino;
    struct ixfs_inode  inode;
};

static struct ixfs_vnode vnodes[IXFS_MAX_OPEN_NODES];
static uint32_t vnode_count;
static struct vfs_dirent ixfs_dirent;

/* Forward declarations */
static struct vfs_ops ixfs_file_ops;
static struct vfs_ops ixfs_dir_ops;

/* Get or create a vnode for an inode number */
static struct ixfs_vnode *ixfs_get_vnode(uint32_t ino)
{
    uint32_t i;
    struct ixfs_vnode *v;

    /* Check if already cached */
    for (i = 0; i < vnode_count; i++) {
        if (vnodes[i].ino == ino)
            return &vnodes[i];
    }

    /* Create new */
    if (vnode_count >= IXFS_MAX_OPEN_NODES)
        return (struct ixfs_vnode *)0;

    v = &vnodes[vnode_count];
    v->ino = ino;

    if (ixfs_read_inode(ino, &v->inode) != 0)
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

    vnode_count++;
    return v;
}

/* --- Internal: get block number for a given file block index --- */
static uint32_t ixfs_get_block(const struct ixfs_inode *inode, uint32_t index)
{
    if (index < IXFS_DIRECT_BLOCKS) {
        return inode->i_direct[index];
    }

    /* Single indirect */
    index -= IXFS_DIRECT_BLOCKS;
    if (index < IXFS_PTRS_PER_BLOCK && inode->i_indirect != 0) {
        uint32_t *ptrs;
        uint32_t blk;

        if (ixfs_read_block(inode->i_indirect, blk_buf) != 0)
            return 0;

        ptrs = (uint32_t *)blk_buf;
        blk = ptrs[index];
        return blk;
    }

    return 0;
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
        uint32_t disk_block = ixfs_get_block(&v->inode, blk_index);
        uint32_t to_read;
        uint32_t i;

        if (disk_block == 0) break;

        if (ixfs_read_block(disk_block, data_buf) != 0)
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
    return (int)bytes_read;
}

static int ixfs_file_write(struct vfs_node *node, uint32_t offset,
                           uint32_t size, const uint8_t *buffer)
{
    struct ixfs_vnode *v = (struct ixfs_vnode *)node->fs_data;
    uint32_t bytes_written = 0;
    uint32_t blk_index, blk_offset;
    uint8_t *data_buf;

    if (!v) return -1;

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    blk_index = offset / IXFS_BLOCK_SIZE;
    blk_offset = offset % IXFS_BLOCK_SIZE;

    while (bytes_written < size) {
        uint32_t disk_block = ixfs_get_block(&v->inode, blk_index);
        uint32_t to_write;
        uint32_t i;

        /* Allocate a new block if needed */
        if (disk_block == 0) {
            disk_block = ixfs_alloc_block();
            if (disk_block == 0) break;

            /* Store in inode */
            if (blk_index < IXFS_DIRECT_BLOCKS) {
                v->inode.i_direct[blk_index] = disk_block;
            } else {
                /* Would need indirect block handling for large files */
                ixfs_free_block(disk_block);
                break;
            }
            v->inode.i_blocks++;

            /* Zero the new block */
            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                data_buf[i] = 0;
        } else {
            /* Read existing block for partial writes */
            if (ixfs_read_block(disk_block, data_buf) != 0)
                break;
        }

        to_write = IXFS_BLOCK_SIZE - blk_offset;
        if (to_write > size - bytes_written)
            to_write = size - bytes_written;

        for (i = 0; i < to_write; i++)
            data_buf[blk_offset + i] = buffer[bytes_written + i];

        if (ixfs_write_block(disk_block, data_buf) != 0)
            break;

        bytes_written += to_write;
        blk_offset = 0;
        blk_index++;
    }

    /* Update inode size if we wrote past the end */
    if (offset + bytes_written > v->inode.i_size)
        v->inode.i_size = offset + bytes_written;

    /* Flush inode to disk */
    v->node.size = v->inode.i_size;
    ixfs_write_inode(v->ino, &v->inode);
    ixfs_flush_bitmap();
    ixfs_flush_superblock();

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

        disk_block = ixfs_get_block(&v->inode, blk_index);
        if (disk_block == 0) break;

        if (ixfs_read_block(disk_block, data_buf) != 0)
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
            ixfs_strcpy(ixfs_dirent.name, de->d_name, VFS_MAX_NAME);
            ixfs_dirent.inode = de->d_inode;

            /* Look up the inode to determine type */
            {
                struct ixfs_inode tmp;
                if (ixfs_read_inode(de->d_inode, &tmp) == 0) {
                    ixfs_dirent.type = (tmp.i_mode & IXFS_S_DIR)
                                     ? VFS_DIRECTORY : VFS_FILE;
                } else {
                    ixfs_dirent.type = VFS_FILE;
                }
            }

            kfree(data_buf);
            return &ixfs_dirent;
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
    uint32_t total_entries;
    uint32_t i;
    uint8_t *data_buf;

    if (!v) return (struct vfs_node *)0;

    total_entries = v->inode.i_size / sizeof(struct ixfs_dir_entry);

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return (struct vfs_node *)0;

    for (i = 0; i < total_entries; i++) {
        uint32_t byte_offset = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_index = byte_offset / IXFS_BLOCK_SIZE;
        uint32_t blk_offset = byte_offset % IXFS_BLOCK_SIZE;
        uint32_t disk_block;
        struct ixfs_dir_entry *de;

        disk_block = ixfs_get_block(&v->inode, blk_index);
        if (disk_block == 0) break;

        if (ixfs_read_block(disk_block, data_buf) != 0)
            break;

        de = (struct ixfs_dir_entry *)(data_buf + blk_offset);

        if (de->d_inode != 0 && ixfs_strcmp(de->d_name, name)) {
            struct ixfs_vnode *found;

            kfree(data_buf);
            found = ixfs_get_vnode(de->d_inode);
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
    for (i = 2; i < sb.s_total_inodes; i++) {
        struct ixfs_inode tmp;
        if (ixfs_read_inode(i, &tmp) == 0 && tmp.i_mode == 0) {
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

    new_inode.i_mode = (type == VFS_DIRECTORY) ? IXFS_S_DIR : IXFS_S_FILE;
    new_inode.i_links = 1;
    new_inode.i_size = 0;
    new_inode.i_blocks = 0;

    /* If directory, allocate an initial data block for . and .. */
    if (type == VFS_DIRECTORY) {
        uint32_t blk = ixfs_alloc_block();
        if (blk == 0) return -1;

        new_inode.i_direct[0] = blk;
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

        ixfs_write_block(blk, data_buf);
        kfree(data_buf);
    }

    ixfs_write_inode(new_ino, &new_inode);
    sb.s_free_inodes--;

    /* Add directory entry to parent */
    total_entries = pv->inode.i_size / sizeof(struct ixfs_dir_entry);

    data_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!data_buf) return -1;

    /* Look for a free slot in existing entries */
    for (i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t bi = byte_off / IXFS_BLOCK_SIZE;
        uint32_t bo = byte_off % IXFS_BLOCK_SIZE;

        dir_block = ixfs_get_block(&pv->inode, bi);
        if (dir_block == 0) break;

        if (ixfs_read_block(dir_block, data_buf) != 0) break;

        de = (struct ixfs_dir_entry *)(data_buf + bo);
        if (de->d_inode == 0) {
            found_slot = (int)i;
            de->d_inode = new_ino;
            ixfs_strcpy(de->d_name, name, IXFS_MAX_NAME);
            ixfs_write_block(dir_block, data_buf);
            break;
        }
    }

    if (found_slot < 0) {
        /* Append at end — may need to grow the directory */
        uint32_t byte_off = total_entries * sizeof(struct ixfs_dir_entry);
        uint32_t bi = byte_off / IXFS_BLOCK_SIZE;
        uint32_t bo = byte_off % IXFS_BLOCK_SIZE;

        dir_block = ixfs_get_block(&pv->inode, bi);

        if (dir_block == 0 && bi < IXFS_DIRECT_BLOCKS) {
            /* Allocate a new block for the directory */
            dir_block = ixfs_alloc_block();
            if (dir_block == 0) { kfree(data_buf); return -1; }
            pv->inode.i_direct[bi] = dir_block;
            pv->inode.i_blocks++;

            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                data_buf[i] = 0;
        } else if (dir_block == 0) {
            kfree(data_buf);
            return -1;
        } else {
            if (ixfs_read_block(dir_block, data_buf) != 0) {
                kfree(data_buf);
                return -1;
            }
        }

        de = (struct ixfs_dir_entry *)(data_buf + bo);
        de->d_inode = new_ino;
        ixfs_strcpy(de->d_name, name, IXFS_MAX_NAME);
        ixfs_write_block(dir_block, data_buf);

        pv->inode.i_size += sizeof(struct ixfs_dir_entry);
    }

    /* Flush parent inode */
    pv->node.size = pv->inode.i_size;
    ixfs_write_inode(pv->ino, &pv->inode);
    ixfs_flush_bitmap();
    ixfs_flush_superblock();

    kfree(data_buf);
    return 0;
}

static int ixfs_unlink(struct vfs_node *parent, const char *name)
{
    struct ixfs_vnode *pv = (struct ixfs_vnode *)parent->fs_data;
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

        dir_block = ixfs_get_block(&pv->inode, bi);
        if (dir_block == 0) break;

        if (ixfs_read_block(dir_block, data_buf) != 0) break;

        de = (struct ixfs_dir_entry *)(data_buf + bo);

        if (de->d_inode != 0 && ixfs_strcmp(de->d_name, name)) {
            struct ixfs_inode target;
            uint32_t target_ino = de->d_inode;
            uint32_t j;

            /* Read the target inode */
            if (ixfs_read_inode(target_ino, &target) != 0) {
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
                    uint32_t d_blk = ixfs_get_block(&target, d_bi);
                    struct ixfs_dir_entry *child;

                    if (d_blk == 0) break;
                    if (ixfs_read_block(d_blk, dir_buf) != 0) break;

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

            /* Free all direct data blocks */
            for (j = 0; j < IXFS_DIRECT_BLOCKS; j++) {
                if (target.i_direct[j] != 0)
                    ixfs_free_block(target.i_direct[j]);
            }

            /* Free indirect block and its referenced blocks */
            if (target.i_indirect != 0) {
                uint8_t *ind_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
                if (ind_buf) {
                    if (ixfs_read_block(target.i_indirect, ind_buf) == 0) {
                        uint32_t *ptrs = (uint32_t *)ind_buf;
                        for (j = 0; j < IXFS_PTRS_PER_BLOCK; j++) {
                            if (ptrs[j] != 0)
                                ixfs_free_block(ptrs[j]);
                        }
                    }
                    kfree(ind_buf);
                }
                ixfs_free_block(target.i_indirect);
            }

            /* Zero the inode on disk */
            {
                uint8_t *p = (uint8_t *)&target;
                for (j = 0; j < sizeof(struct ixfs_inode); j++)
                    p[j] = 0;
            }
            ixfs_write_inode(target_ino, &target);
            sb.s_free_inodes++;

            /* Clear the directory entry */
            de->d_inode = 0;
            de->d_name[0] = '\0';
            ixfs_write_block(dir_block, data_buf);

            /* Remove from vnode cache if present */
            for (j = 0; j < vnode_count; j++) {
                if (vnodes[j].ino == target_ino) {
                    vnodes[j].ino = 0;
                    break;
                }
            }

            /* Flush metadata */
            ixfs_write_inode(pv->ino, &pv->inode);
            ixfs_flush_bitmap();
            ixfs_flush_superblock();

            kfree(data_buf);
            return 0;
        }
    }

    kfree(data_buf);
    return -1;  /* file not found */
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

int ixfs_format(uint32_t partition_lba, uint32_t total_sectors,
                const char *volume_name)
{
    uint32_t total_blocks = total_sectors / IXFS_SECTORS_PER_BLK;
    uint32_t bitmap_blocks_needed;
    uint32_t inode_blocks;
    uint32_t used_blocks;
    uint32_t i;
    struct ixfs_inode root_inode;
    struct ixfs_dir_entry root_dirs[2];
    uint32_t root_data_block;
    uint8_t *p;

    ixfs_part_lba = partition_lba;

    /* Calculate layout */
    bitmap_blocks_needed = (total_blocks + (IXFS_BLOCK_SIZE * 8) - 1)
                         / (IXFS_BLOCK_SIZE * 8);
    inode_blocks = (IXFS_DEFAULT_INODES * sizeof(struct ixfs_inode)
                   + IXFS_BLOCK_SIZE - 1) / IXFS_BLOCK_SIZE;

    used_blocks = 1                    /* superblock */
                + bitmap_blocks_needed /* bitmap */
                + inode_blocks         /* inodes */
                + 1;                   /* root directory data block */

    /* Build superblock */
    p = (uint8_t *)&sb;
    for (i = 0; i < sizeof(struct ixfs_superblock); i++)
        p[i] = 0;

    sb.s_magic         = IXFS_MAGIC;
    sb.s_version       = IXFS_VERSION;
    sb.s_block_size    = IXFS_BLOCK_SIZE;
    sb.s_total_blocks  = total_blocks;
    sb.s_free_blocks   = total_blocks - used_blocks;
    sb.s_total_inodes  = IXFS_DEFAULT_INODES;
    sb.s_free_inodes   = IXFS_DEFAULT_INODES - 2; /* inode 0 reserved, inode 1 root */
    sb.s_bitmap_start  = 1;
    sb.s_bitmap_blocks = bitmap_blocks_needed;
    sb.s_inode_start   = 1 + bitmap_blocks_needed;
    sb.s_inode_blocks  = inode_blocks;
    sb.s_data_start    = 1 + bitmap_blocks_needed + inode_blocks;
    sb.s_root_inode    = IXFS_ROOT_INODE;

    if (volume_name)
        ixfs_strcpy((char *)sb.s_volume_name, volume_name, 32);
    else
        ixfs_strcpy((char *)sb.s_volume_name, "IXFS", 32);

    /* Write superblock */
    if (ixfs_flush_superblock() != 0) {
        printk("[FAIL] IXFS format: cannot write superblock\n");
        return -1;
    }

    /* Zero the bitmap blocks */
    for (i = 0; i < bitmap_blocks_needed; i++) {
        if (ixfs_zero_block(sb.s_bitmap_start + i) != 0)
            return -1;
    }

    /* Zero the inode table blocks */
    for (i = 0; i < inode_blocks; i++) {
        if (ixfs_zero_block(sb.s_inode_start + i) != 0)
            return -1;
    }

    /* Set up in-memory bitmap */
    bitmap_bytes = (total_blocks + 7) / 8;
    block_bitmap = (uint8_t *)kmalloc(bitmap_bytes);
    if (!block_bitmap) return -1;

    for (i = 0; i < bitmap_bytes; i++)
        block_bitmap[i] = 0;

    /* Mark metadata blocks as used in bitmap */
    for (i = 0; i < used_blocks; i++)
        bitmap_set(block_bitmap, i);

    /* Flush bitmap to disk */
    ixfs_flush_bitmap();

    /* Create root directory inode (inode 1) */
    p = (uint8_t *)&root_inode;
    for (i = 0; i < sizeof(struct ixfs_inode); i++)
        p[i] = 0;

    root_data_block = sb.s_data_start; /* first data block */
    root_inode.i_mode      = IXFS_S_DIR;
    root_inode.i_links     = 1;
    root_inode.i_size      = 2 * sizeof(struct ixfs_dir_entry); /* . and .. */
    root_inode.i_blocks    = 1;
    root_inode.i_direct[0] = root_data_block;

    if (ixfs_write_inode(IXFS_ROOT_INODE, &root_inode) != 0)
        return -1;

    /* Write root directory data block with . and .. */
    for (i = 0; i < IXFS_BLOCK_SIZE; i++)
        blk_buf[i] = 0;

    p = (uint8_t *)&root_dirs[0];
    for (i = 0; i < sizeof(root_dirs); i++)
        p[i] = 0;

    root_dirs[0].d_inode = IXFS_ROOT_INODE;
    ixfs_strcpy(root_dirs[0].d_name, ".", IXFS_MAX_NAME);
    root_dirs[1].d_inode = IXFS_ROOT_INODE;
    ixfs_strcpy(root_dirs[1].d_name, "..", IXFS_MAX_NAME);

    p = (uint8_t *)root_dirs;
    for (i = 0; i < sizeof(root_dirs); i++)
        blk_buf[i] = p[i];

    if (ixfs_write_block(root_data_block, blk_buf) != 0)
        return -1;

    printk("[OK] IXFS formatted: %u blocks, %u inodes, \"%s\"\n",
           (uint64_t)total_blocks,
           (uint64_t)IXFS_DEFAULT_INODES,
           sb.s_volume_name);

    return 0;
}

int ixfs_init(uint32_t partition_lba)
{
    uint32_t i;
    uint8_t *p;

    ixfs_part_lba = partition_lba;
    vnode_count = 0;

    /* Read block 0 (superblock) */
    if (ixfs_read_block(0, blk_buf) != 0) {
        printk("[FAIL] IXFS: cannot read superblock\n");
        return -1;
    }

    /* Copy superblock from buffer */
    p = (uint8_t *)&sb;
    for (i = 0; i < sizeof(struct ixfs_superblock); i++)
        p[i] = blk_buf[i];

    /* Verify magic */
    if (sb.s_magic != IXFS_MAGIC) {
        printk("[FAIL] IXFS: bad magic (0x%x, expected 0x%x)\n",
               (uint64_t)sb.s_magic, (uint64_t)IXFS_MAGIC);
        return -1;
    }

    if (sb.s_version != IXFS_VERSION) {
        printk("[FAIL] IXFS: unsupported version %u\n",
               (uint64_t)sb.s_version);
        return -1;
    }

    /* Load block bitmap into memory */
    bitmap_bytes = (sb.s_total_blocks + 7) / 8;
    block_bitmap = (uint8_t *)kmalloc(bitmap_bytes);
    if (!block_bitmap) {
        printk("[FAIL] IXFS: cannot allocate bitmap (%u bytes)\n",
               (uint64_t)bitmap_bytes);
        return -1;
    }

    for (i = 0; i < bitmap_bytes; i++)
        block_bitmap[i] = 0;

    for (i = 0; i < sb.s_bitmap_blocks; i++) {
        uint32_t offset = i * IXFS_BLOCK_SIZE;
        uint32_t remaining = bitmap_bytes - offset;
        uint32_t j;

        if (remaining > IXFS_BLOCK_SIZE)
            remaining = IXFS_BLOCK_SIZE;

        if (ixfs_read_block(sb.s_bitmap_start + i, blk_buf) != 0) {
            kfree(block_bitmap);
            return -1;
        }

        for (j = 0; j < remaining; j++)
            block_bitmap[offset + j] = blk_buf[j];
    }

    /* Create root vnode */
    {
        struct ixfs_vnode *root = ixfs_get_vnode(IXFS_ROOT_INODE);
        if (!root) {
            printk("[FAIL] IXFS: cannot read root inode\n");
            kfree(block_bitmap);
            return -1;
        }
        ixfs_strcpy(root->node.name, "C:\\", VFS_MAX_NAME);
        root->node.type = VFS_DIRECTORY | VFS_MOUNTPOINT;
    }

    {
        uint64_t vol_mb = (uint64_t)sb.s_total_blocks * IXFS_BLOCK_SIZE
                        / (1024 * 1024);
        printk("[OK] IXFS: \"%s\" v%u, %u MiB, %u/%u blocks free, %u inodes\n",
               sb.s_volume_name,
               (uint64_t)sb.s_version,
               vol_mb,
               (uint64_t)sb.s_free_blocks,
               (uint64_t)sb.s_total_blocks,
               (uint64_t)sb.s_total_inodes);
    }

    return 0;
}

struct vfs_fs_driver *ixfs_get_driver(void)
{
    return &ixfs_driver;
}

struct vfs_node *ixfs_get_root(void)
{
    if (vnode_count == 0)
        return (struct vfs_node *)0;
    return &vnodes[0].node;  /* root is always vnode[0] */
}
