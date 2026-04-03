/* ============================================================================
 * ixfs-core.c -- Shared IXFS parser for host tools
 *
 * Ported from kernel: ixfs_core.c, ixfs_inode.c, ixfs_extent.c, ixfs_ops.c
 * No kernel dependencies -- uses <stdint.h>, <stdio.h>, <stdlib.h>, <string.h>.
 * ============================================================================ */

#define _POSIX_C_SOURCE 200809L
#include "ixfs-core.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

int ixfs_disk_write_block(ixfs_disk_ctx_t *disk, uint32_t block, const void *buf)
{
    uint64_t byte_offset = disk->part_offset + (uint64_t)block * IXFS_BLOCK_SIZE;

    if (fseeko(disk->fp, (off_t)byte_offset, SEEK_SET) != 0)
        return -1;

    if (fwrite(buf, IXFS_BLOCK_SIZE, 1, disk->fp) != 1)
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
    if (!vol) return;
    if (vol->dirty)
        ixfs_flush(vol);
    free(vol->bitmap);
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
            /* Hole -- zero fill */
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

/* ========================================================================
 * Write support -- bitmap, block alloc, inode write, dir ops, flush
 * ======================================================================== */

/* --- Bitmap --- */

static void bmap_set(uint8_t *bmap, uint32_t bit)   { bmap[bit/8] |= (uint8_t)(1 << (bit%8)); }
static void bmap_clear(uint8_t *bmap, uint32_t bit)  { bmap[bit/8] &= (uint8_t)~(1 << (bit%8)); }
static int  bmap_test(const uint8_t *bmap, uint32_t bit) { return (bmap[bit/8] >> (bit%8)) & 1; }

int ixfs_load_bitmap(ixfs_vol_t *vol)
{
    if (vol->bitmap) return 0;  /* already loaded */

    vol->bitmap_bytes = (uint32_t)((vol->sb.s_total_blocks + 7) / 8);
    vol->bitmap = calloc(1, vol->bitmap_bytes);
    if (!vol->bitmap) return -1;

    for (uint32_t i = 0; i < vol->sb.s_bitmap_blocks; i++) {
        uint8_t blk[IXFS_BLOCK_SIZE];
        if (ixfs_disk_read_block(vol->disk, vol->sb.s_bitmap_start + i, blk) != 0) {
            free(vol->bitmap); vol->bitmap = NULL;
            return -1;
        }
        uint32_t off = i * IXFS_BLOCK_SIZE;
        uint32_t chunk = vol->bitmap_bytes - off;
        if (chunk > IXFS_BLOCK_SIZE) chunk = IXFS_BLOCK_SIZE;
        memcpy(vol->bitmap + off, blk, chunk);
    }
    return 0;
}

/* --- Block allocation --- */

uint32_t ixfs_alloc_block(ixfs_vol_t *vol)
{
    if (!vol->bitmap || vol->sb.s_free_blocks == 0) return 0;

    uint32_t start = vol->sb.s_data_start;
    uint32_t total = (uint32_t)vol->sb.s_total_blocks;

    for (uint32_t b = start; b < total; b++) {
        if (!bmap_test(vol->bitmap, b)) {
            bmap_set(vol->bitmap, b);
            vol->sb.s_free_blocks--;
            vol->dirty = 1;
            return b;
        }
    }
    return 0;
}

void ixfs_free_block(ixfs_vol_t *vol, uint32_t block)
{
    if (!vol->bitmap) return;
    if (block < vol->sb.s_data_start || block >= vol->sb.s_total_blocks) return;
    if (!bmap_test(vol->bitmap, block)) return;

    bmap_clear(vol->bitmap, block);
    vol->sb.s_free_blocks++;
    vol->dirty = 1;
}

/* --- Inode write --- */

int ixfs_write_inode(ixfs_vol_t *vol, uint32_t ino, const struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block = vol->sb.s_inode_start + (ino / inodes_per_block);
    uint32_t offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);
    uint8_t block_buf[IXFS_BLOCK_SIZE];

    if (ixfs_disk_read_block(vol->disk, block, block_buf) != 0)
        return -1;

    memcpy(block_buf + offset, inode, sizeof(struct ixfs_inode));

    if (ixfs_disk_write_block(vol->disk, block, block_buf) != 0)
        return -1;

    vol->dirty = 1;
    return 0;
}

/* --- Extent allocation --- */

uint32_t ixfs_extent_alloc(ixfs_vol_t *vol, struct ixfs_inode *inode)
{
    uint32_t new_blk = ixfs_alloc_block(vol);
    if (new_blk == 0) return 0;

    uint32_t ec = inode->i_extent_count;

    /* Try to merge with last extent */
    if (ec > 0 && inode->i_extents[ec-1].e_count > 0) {
        uint32_t last_end = (uint32_t)(inode->i_extents[ec-1].e_start +
                                        inode->i_extents[ec-1].e_count);
        if (new_blk == last_end) {
            inode->i_extents[ec-1].e_count++;
            inode->i_blocks++;
            return new_blk;
        }
    }

    /* New extent */
    if (ec < IXFS_INLINE_EXTENTS) {
        inode->i_extents[ec].e_start = (uint64_t)new_blk;
        inode->i_extents[ec].e_count = 1;
        inode->i_extent_count = ec + 1;
        inode->i_blocks++;
        return new_blk;
    }

    /* Out of inline extents */
    ixfs_free_block(vol, new_blk);
    return 0;
}

/* --- Write data --- */

int64_t ixfs_write_data(ixfs_vol_t *vol, uint32_t ino, struct ixfs_inode *inode,
                        uint64_t offset, const void *buf, uint64_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t total_written = 0;

    while (len > 0) {
        uint32_t file_block = (uint32_t)(offset / IXFS_BLOCK_SIZE);
        uint32_t blk_off = (uint32_t)(offset % IXFS_BLOCK_SIZE);
        uint32_t chunk = IXFS_BLOCK_SIZE - blk_off;
        uint32_t disk_block;
        uint8_t block_buf[IXFS_BLOCK_SIZE];

        if (chunk > len) chunk = (uint32_t)len;

        /* Find or allocate the disk block for this file block */
        disk_block = ixfs_extent_lookup(inode, file_block);
        if (disk_block == 0) {
            /* Need to allocate blocks up to this file_block */
            uint32_t existing_blocks = 0;
            for (uint32_t i = 0; i < inode->i_extent_count; i++)
                existing_blocks += inode->i_extents[i].e_count;

            while (existing_blocks <= file_block) {
                uint32_t nb = ixfs_extent_alloc(vol, inode);
                if (nb == 0) return (total_written > 0) ? (int64_t)total_written : -1;
                /* Zero the new block */
                memset(block_buf, 0, IXFS_BLOCK_SIZE);
                ixfs_disk_write_block(vol->disk, nb, block_buf);
                existing_blocks++;
            }
            disk_block = ixfs_extent_lookup(inode, file_block);
            if (disk_block == 0)
                return (total_written > 0) ? (int64_t)total_written : -1;
        }

        /* Read-modify-write for partial blocks */
        if (chunk < IXFS_BLOCK_SIZE) {
            if (ixfs_disk_read_block(vol->disk, disk_block, block_buf) != 0)
                return (total_written > 0) ? (int64_t)total_written : -1;
        }

        memcpy(block_buf + blk_off, src, chunk);

        if (ixfs_disk_write_block(vol->disk, disk_block, block_buf) != 0)
            return (total_written > 0) ? (int64_t)total_written : -1;

        src += chunk;
        offset += chunk;
        len -= chunk;
        total_written += chunk;
    }

    /* Update file size if we wrote past the end */
    if (offset > inode->i_size)
        inode->i_size = offset;

    inode->i_mtime = (uint32_t)time(NULL);
    ixfs_write_inode(vol, ino, inode);
    vol->dirty = 1;
    return (int64_t)total_written;
}

/* --- Directory operations (write) --- */

/* Allocate a free inode slot. Returns inode number or 0. */
static uint32_t alloc_inode(ixfs_vol_t *vol)
{
    for (uint32_t ino = 2; ino < vol->sb.s_total_inodes; ino++) {
        struct ixfs_inode tmp;
        if (ixfs_read_inode(vol, ino, &tmp) != 0) continue;
        if (tmp.i_mode == 0 && tmp.i_links == 0) {
            vol->sb.s_free_inodes--;
            vol->dirty = 1;
            return ino;
        }
    }
    return 0;
}

uint32_t ixfs_create(ixfs_vol_t *vol, uint32_t parent_ino,
                     struct ixfs_inode *parent_inode,
                     const char *name, uint16_t type)
{
    /* Check if name already exists */
    if (ixfs_lookup(vol, parent_inode, name) != 0)
        return 0;

    /* Allocate a new inode */
    uint32_t new_ino = alloc_inode(vol);
    if (new_ino == 0) return 0;

    /* Initialize new inode */
    struct ixfs_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = type | 0x01ED; /* rwxrwxr-x */
    new_inode.i_links = 1;
    new_inode.i_ctime = new_inode.i_mtime = new_inode.i_atime = (uint32_t)time(NULL);

    if (type == IXFS_S_DIR) {
        /* Allocate a block for . and .. entries */
        uint32_t dir_blk = ixfs_alloc_block(vol);
        if (dir_blk == 0) return 0;

        uint8_t blk[IXFS_BLOCK_SIZE];
        memset(blk, 0, IXFS_BLOCK_SIZE);

        struct ixfs_dir_entry *de = (struct ixfs_dir_entry *)blk;
        de[0].d_inode = new_ino;
        strncpy(de[0].d_name, ".", IXFS_MAX_NAME - 1);
        de[1].d_inode = parent_ino;
        strncpy(de[1].d_name, "..", IXFS_MAX_NAME - 1);

        ixfs_disk_write_block(vol->disk, dir_blk, blk);

        new_inode.i_extents[0].e_start = dir_blk;
        new_inode.i_extents[0].e_count = 1;
        new_inode.i_extent_count = 1;
        new_inode.i_blocks = 1;
        new_inode.i_size = 2 * sizeof(struct ixfs_dir_entry);
    }

    if (ixfs_write_inode(vol, new_ino, &new_inode) != 0)
        return 0;

    /* Add entry to parent directory */
    uint32_t total_entries = (uint32_t)(parent_inode->i_size / sizeof(struct ixfs_dir_entry));
    uint32_t slot = total_entries; /* append by default */

    /* Look for a deleted slot to reuse */
    for (uint32_t i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
        uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
        uint32_t disk_block = ixfs_extent_lookup(parent_inode, blk_idx);
        if (disk_block == 0) break;

        uint8_t blk[IXFS_BLOCK_SIZE];
        if (ixfs_disk_read_block(vol->disk, disk_block, blk) != 0) break;

        struct ixfs_dir_entry *de = (struct ixfs_dir_entry *)(blk + blk_off);
        if (de->d_inode == 0) {
            slot = i;
            break;
        }
    }

    /* Write the directory entry */
    uint32_t byte_off = slot * sizeof(struct ixfs_dir_entry);
    uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
    uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
    uint32_t disk_block = ixfs_extent_lookup(parent_inode, blk_idx);

    if (disk_block == 0) {
        /* Need to extend the parent directory */
        disk_block = ixfs_extent_alloc(vol, parent_inode);
        if (disk_block == 0) return 0;
        uint8_t zblk[IXFS_BLOCK_SIZE];
        memset(zblk, 0, IXFS_BLOCK_SIZE);
        ixfs_disk_write_block(vol->disk, disk_block, zblk);
    }

    uint8_t blk[IXFS_BLOCK_SIZE];
    ixfs_disk_read_block(vol->disk, disk_block, blk);

    struct ixfs_dir_entry *de = (struct ixfs_dir_entry *)(blk + blk_off);
    de->d_inode = new_ino;
    memset(de->d_name, 0, IXFS_MAX_NAME);
    strncpy(de->d_name, name, IXFS_MAX_NAME - 1);

    ixfs_disk_write_block(vol->disk, disk_block, blk);

    /* Update parent inode size if we appended */
    if (slot >= total_entries)
        parent_inode->i_size = (uint64_t)(slot + 1) * sizeof(struct ixfs_dir_entry);
    parent_inode->i_mtime = (uint32_t)time(NULL);
    ixfs_write_inode(vol, parent_ino, parent_inode);

    vol->dirty = 1;
    return new_ino;
}

int ixfs_delete(ixfs_vol_t *vol, uint32_t parent_ino,
                struct ixfs_inode *parent_inode, const char *name)
{
    uint32_t total_entries = (uint32_t)(parent_inode->i_size / sizeof(struct ixfs_dir_entry));

    for (uint32_t i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
        uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
        uint32_t disk_block = ixfs_extent_lookup(parent_inode, blk_idx);
        if (disk_block == 0) break;

        uint8_t blk[IXFS_BLOCK_SIZE];
        if (ixfs_disk_read_block(vol->disk, disk_block, blk) != 0) break;

        struct ixfs_dir_entry *de = (struct ixfs_dir_entry *)(blk + blk_off);
        if (de->d_inode != 0 && strcmp(de->d_name, name) == 0) {
            uint32_t victim_ino = de->d_inode;

            /* Read victim inode */
            struct ixfs_inode victim;
            if (ixfs_read_inode(vol, victim_ino, &victim) != 0)
                return -1;

            /* Don't delete non-empty directories */
            if (victim.i_mode & IXFS_S_DIR) {
                uint32_t dir_entries = (uint32_t)(victim.i_size / sizeof(struct ixfs_dir_entry));
                uint32_t real_count = 0;
                for (uint32_t j = 0; j < dir_entries; j++) {
                    uint32_t joff = j * sizeof(struct ixfs_dir_entry);
                    uint32_t jbi = joff / IXFS_BLOCK_SIZE;
                    uint32_t jbo = joff % IXFS_BLOCK_SIZE;
                    uint32_t jdb = ixfs_extent_lookup(&victim, jbi);
                    if (jdb == 0) break;
                    uint8_t jblk[IXFS_BLOCK_SIZE];
                    if (ixfs_disk_read_block(vol->disk, jdb, jblk) != 0) break;
                    struct ixfs_dir_entry *jde = (struct ixfs_dir_entry *)(jblk + jbo);
                    if (jde->d_inode != 0 &&
                        !(jde->d_name[0] == '.' && (jde->d_name[1] == '\0' ||
                          (jde->d_name[1] == '.' && jde->d_name[2] == '\0'))))
                        real_count++;
                }
                if (real_count > 0) return -1; /* not empty */
            }

            /* Free all data blocks */
            for (uint32_t e = 0; e < victim.i_extent_count; e++) {
                for (uint32_t b = 0; b < victim.i_extents[e].e_count; b++)
                    ixfs_free_block(vol, (uint32_t)(victim.i_extents[e].e_start + b));
            }

            /* Zero the inode */
            memset(&victim, 0, sizeof(victim));
            ixfs_write_inode(vol, victim_ino, &victim);
            vol->sb.s_free_inodes++;

            /* Clear directory entry */
            de->d_inode = 0;
            memset(de->d_name, 0, IXFS_MAX_NAME);
            ixfs_disk_write_block(vol->disk, disk_block, blk);

            parent_inode->i_mtime = (uint32_t)time(NULL);
            ixfs_write_inode(vol, parent_ino, parent_inode);
            vol->dirty = 1;
            return 0;
        }
    }
    return -1; /* not found */
}

int ixfs_rename(ixfs_vol_t *vol, uint32_t parent_ino,
                struct ixfs_inode *parent_inode,
                const char *old_name, const char *new_name)
{
    uint32_t total_entries = (uint32_t)(parent_inode->i_size / sizeof(struct ixfs_dir_entry));

    /* Check new name doesn't already exist */
    if (ixfs_lookup(vol, parent_inode, new_name) != 0)
        return -1;

    for (uint32_t i = 0; i < total_entries; i++) {
        uint32_t byte_off = i * sizeof(struct ixfs_dir_entry);
        uint32_t blk_idx = byte_off / IXFS_BLOCK_SIZE;
        uint32_t blk_off = byte_off % IXFS_BLOCK_SIZE;
        uint32_t disk_block = ixfs_extent_lookup(parent_inode, blk_idx);
        if (disk_block == 0) break;

        uint8_t blk[IXFS_BLOCK_SIZE];
        if (ixfs_disk_read_block(vol->disk, disk_block, blk) != 0) break;

        struct ixfs_dir_entry *de = (struct ixfs_dir_entry *)(blk + blk_off);
        if (de->d_inode != 0 && strcmp(de->d_name, old_name) == 0) {
            memset(de->d_name, 0, IXFS_MAX_NAME);
            strncpy(de->d_name, new_name, IXFS_MAX_NAME - 1);
            ixfs_disk_write_block(vol->disk, disk_block, blk);
            parent_inode->i_mtime = (uint32_t)time(NULL);
            ixfs_write_inode(vol, parent_ino, parent_inode);
            vol->dirty = 1;
            return 0;
        }
    }
    return -1;
}

/* --- Flush --- */

int ixfs_flush(ixfs_vol_t *vol)
{
    if (!vol->dirty) return 0;

    /* Flush bitmap */
    if (vol->bitmap) {
        for (uint32_t i = 0; i < vol->sb.s_bitmap_blocks; i++) {
            uint8_t blk[IXFS_BLOCK_SIZE];
            uint32_t off = i * IXFS_BLOCK_SIZE;
            uint32_t chunk = vol->bitmap_bytes - off;
            if (chunk > IXFS_BLOCK_SIZE) chunk = IXFS_BLOCK_SIZE;
            memset(blk, 0, IXFS_BLOCK_SIZE);
            memcpy(blk, vol->bitmap + off, chunk);
            if (ixfs_disk_write_block(vol->disk, vol->sb.s_bitmap_start + i, blk) != 0)
                return -1;
        }
    }

    /* Flush superblock */
    {
        uint8_t blk[IXFS_BLOCK_SIZE];
        memset(blk, 0, IXFS_BLOCK_SIZE);
        memcpy(blk, &vol->sb, sizeof(struct ixfs_superblock));
        if (ixfs_disk_write_block(vol->disk, 0, blk) != 0)
            return -1;
    }

    /* Flush file handle */
    fflush(vol->disk->fp);
    vol->dirty = 0;
    return 0;
}
