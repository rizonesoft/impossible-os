/* ============================================================================
 * ixfs-core.c — Shared IXFS parser for host tools
 *
 * Ported from kernel: ixfs_core.c, ixfs_inode.c, ixfs_extent.c, ixfs_ops.c
 * No kernel dependencies — uses <stdint.h>, <stdio.h>, <stdlib.h>, <string.h>.
 * ============================================================================ */

#define _POSIX_C_SOURCE 200809L
#include "ixfs-core.h"
#include <stdlib.h>
#include <string.h>

/* --- Disk I/O --- */

int ixfs_disk_read_block(ixfs_disk_ctx_t *disk, uint32_t block, void *buf)
{
    uint64_t byte_offset = disk->part_offset + (uint64_t)block * IXFS_BLOCK_SIZE;

    if (fseeko(disk->fp, (off_t)byte_offset, SEEK_SET) != 0)
        return -1;

    if (fread(buf, IXFS_BLOCK_SIZE, 1, disk->fp) != 1)
        return -1;

    return 0;
}

/* --- Volume operations --- */

ixfs_vol_t *ixfs_open(ixfs_disk_ctx_t *disk)
{
    ixfs_vol_t *vol;
    uint8_t block_buf[IXFS_BLOCK_SIZE];

    if (ixfs_disk_read_block(disk, 0, block_buf) != 0) {
        fprintf(stderr, "ixfs: failed to read superblock\n");
        return NULL;
    }

    vol = calloc(1, sizeof(ixfs_vol_t));
    if (!vol)
        return NULL;

    vol->disk = disk;
    memcpy(&vol->sb, block_buf, sizeof(struct ixfs_superblock));

    if (vol->sb.s_magic != IXFS_MAGIC) {
        fprintf(stderr, "ixfs: bad magic 0x%08X (expected 0x%08X)\n",
                vol->sb.s_magic, IXFS_MAGIC);
        free(vol);
        return NULL;
    }

    if (vol->sb.s_version > IXFS_VERSION) {
        fprintf(stderr, "ixfs: unsupported version %u (max %u)\n",
                vol->sb.s_version, IXFS_VERSION);
        free(vol);
        return NULL;
    }

    return vol;
}

void ixfs_close(ixfs_vol_t *vol)
{
    if (vol)
        free(vol);
}

/* --- Inode operations --- */

int ixfs_read_inode(ixfs_vol_t *vol, uint32_t ino, struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block = vol->sb.s_inode_start + (ino / inodes_per_block);
    uint32_t offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);
    uint8_t block_buf[IXFS_BLOCK_SIZE];

    if (ixfs_disk_read_block(vol->disk, block, block_buf) != 0)
        return -1;

    memcpy(inode, block_buf + offset, sizeof(struct ixfs_inode));
    return 0;
}

uint32_t ixfs_extent_lookup(const struct ixfs_inode *inode, uint32_t file_block)
{
    uint32_t file_offset = 0;

    if (inode->i_extent_flags & IXFS_INLINE)
        return 0;

    for (uint32_t i = 0; i < inode->i_extent_count; i++) {
        uint32_t count = inode->i_extents[i].e_count;
        if (count == 0)
            continue;

        if (file_block < file_offset + count) {
            if (inode->i_extents[i].e_start == 0)
                return 0;  /* hole (sparse) */
            return (uint32_t)(inode->i_extents[i].e_start +
                              (file_block - file_offset));
        }
        file_offset += count;
    }

    return 0;  /* not mapped */
}

/* --- Directory operations --- */

int ixfs_readdir(ixfs_vol_t *vol, const struct ixfs_inode *dir_inode,
                 ixfs_readdir_cb callback, void *ctx)
{
    uint32_t total_entries = (uint32_t)(dir_inode->i_size / sizeof(struct ixfs_dir_entry));
    uint8_t block_buf[IXFS_BLOCK_SIZE];

    for (uint32_t i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
        uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
        uint32_t disk_block;
        const struct ixfs_dir_entry *de;

        disk_block = ixfs_extent_lookup(dir_inode, blk_idx);
        if (disk_block == 0)
            break;

        if (ixfs_disk_read_block(vol->disk, disk_block, block_buf) != 0)
            break;

        de = (const struct ixfs_dir_entry *)(block_buf + blk_off);

        /* Skip free/deleted entries */
        if (de->d_inode == 0)
            continue;

        /* Skip . and .. */
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;

        /* Read the inode for this entry */
        struct ixfs_inode entry_inode;
        if (ixfs_read_inode(vol, de->d_inode, &entry_inode) != 0)
            continue;

        if (callback(de, &entry_inode, ctx) != 0)
            return 0;  /* caller requested stop */
    }

    return 0;
}

uint32_t ixfs_lookup(ixfs_vol_t *vol, const struct ixfs_inode *dir_inode,
                     const char *name)
{
    uint32_t total_entries = (uint32_t)(dir_inode->i_size / sizeof(struct ixfs_dir_entry));
    uint8_t block_buf[IXFS_BLOCK_SIZE];

    for (uint32_t i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
        uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
        uint32_t disk_block;
        const struct ixfs_dir_entry *de;

        disk_block = ixfs_extent_lookup(dir_inode, blk_idx);
        if (disk_block == 0)
            break;

        if (ixfs_disk_read_block(vol->disk, disk_block, block_buf) != 0)
            break;

        de = (const struct ixfs_dir_entry *)(block_buf + blk_off);

        if (de->d_inode != 0 && strcmp(de->d_name, name) == 0)
            return de->d_inode;
    }

    return 0;  /* not found */
}

/* --- File data --- */

int64_t ixfs_read_data(ixfs_vol_t *vol, const struct ixfs_inode *inode,
                       uint64_t offset, void *buf, uint64_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining;
    uint64_t total_read = 0;

    /* Clamp to file size */
    if (offset >= inode->i_size)
        return 0;
    remaining = inode->i_size - offset;
    if (len > remaining)
        len = remaining;

    /* Handle inline files (data stored directly in extent area) */
    if (inode->i_extent_flags & IXFS_INLINE) {
        if (offset >= IXFS_INLINE_MAX)
            return 0;
        if (len > IXFS_INLINE_MAX - offset)
            len = IXFS_INLINE_MAX - offset;
        memcpy(dst, (const uint8_t *)inode->i_extents + offset, len);
        return (int64_t)len;
    }

    /* Read block by block */
    while (len > 0) {
        uint32_t file_block = (uint32_t)(offset / IXFS_BLOCK_SIZE);
        uint32_t blk_off = (uint32_t)(offset % IXFS_BLOCK_SIZE);
        uint32_t chunk = IXFS_BLOCK_SIZE - blk_off;
        uint32_t disk_block;
        uint8_t block_buf[IXFS_BLOCK_SIZE];

        if (chunk > len)
            chunk = (uint32_t)len;

        disk_block = ixfs_extent_lookup(inode, file_block);
        if (disk_block == 0) {
            /* Hole — zero fill */
            memset(dst, 0, chunk);
        } else {
            if (ixfs_disk_read_block(vol->disk, disk_block, block_buf) != 0)
                return (total_read > 0) ? (int64_t)total_read : -1;
            memcpy(dst, block_buf + blk_off, chunk);
        }

        dst += chunk;
        offset += chunk;
        len -= chunk;
        total_read += chunk;
    }

    return (int64_t)total_read;
}
