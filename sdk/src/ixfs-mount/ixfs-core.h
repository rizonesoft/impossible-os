/* ============================================================================
 * ixfs-core.h — Shared IXFS parser API for host tools
 *
 * No kernel dependencies. Uses <stdint.h>, <stdio.h>, <string.h>.
 * ============================================================================ */

#ifndef IXFS_CORE_H
#define IXFS_CORE_H

#include "ixfs-structs.h"
#include <stdio.h>

/* Disk context — abstracts raw sector I/O from a file or device */
typedef struct ixfs_disk_ctx {
    FILE *fp;                           /* file handle */
    uint64_t part_offset;               /* byte offset to partition start */
    uint64_t part_size;                 /* partition size in bytes */
} ixfs_disk_ctx_t;

/* Volume context — holds parsed superblock, bitmap, and disk context */
typedef struct ixfs_vol {
    ixfs_disk_ctx_t *disk;
    struct ixfs_superblock sb;
    uint8_t *bitmap;            /* block bitmap (NULL if read-only) */
    uint32_t bitmap_bytes;      /* size of bitmap in bytes */
    int dirty;                  /* 1 if any write occurred */
} ixfs_vol_t;

/* Directory entry callback — return 0 to continue, non-zero to stop */
typedef int (*ixfs_readdir_cb)(const struct ixfs_dir_entry *de,
                               const struct ixfs_inode *inode, void *ctx);

/* --- Disk I/O --- */

/* Read a single IXFS block (4 KiB) from the partition */
int ixfs_disk_read_block(ixfs_disk_ctx_t *disk, uint32_t block, void *buf);

/* --- Volume operations --- */

/* Open and validate an IXFS volume. Returns NULL on failure. */
ixfs_vol_t *ixfs_open(ixfs_disk_ctx_t *disk);

/* Close a volume (frees the vol struct, does NOT close the disk ctx) */
void ixfs_close(ixfs_vol_t *vol);

/* --- Inode operations --- */

/* Read an inode by number */
int ixfs_read_inode(ixfs_vol_t *vol, uint32_t ino, struct ixfs_inode *inode);

/* Resolve a file block index to a disk block number via extent lookup */
uint32_t ixfs_extent_lookup(const struct ixfs_inode *inode, uint32_t file_block);

/* --- Directory operations --- */

/* Enumerate directory entries (skips deleted, . and ..) */
int ixfs_readdir(ixfs_vol_t *vol, const struct ixfs_inode *dir_inode,
                 ixfs_readdir_cb callback, void *ctx);

/* Look up a name in a directory. Returns inode number or 0 on not found. */
uint32_t ixfs_lookup(ixfs_vol_t *vol, const struct ixfs_inode *dir_inode,
                     const char *name);

/* --- File data --- */

/* Read file data at offset into buf. Returns bytes read or -1 on error. */
int64_t ixfs_read_data(ixfs_vol_t *vol, const struct ixfs_inode *inode,
                       uint64_t offset, void *buf, uint64_t len);

/* --- Write operations (require R/W disk open) --- */

/* Write a single IXFS block (4 KiB) to the partition */
int ixfs_disk_write_block(ixfs_disk_ctx_t *disk, uint32_t block, const void *buf);

/* Load bitmap into memory (required before any write operations) */
int ixfs_load_bitmap(ixfs_vol_t *vol);

/* Write an inode back to disk */
int ixfs_write_inode(ixfs_vol_t *vol, uint32_t ino, const struct ixfs_inode *inode);

/* Allocate a new disk block. Returns block number or 0 on failure. */
uint32_t ixfs_alloc_block(ixfs_vol_t *vol);

/* Free a disk block back to the bitmap. */
void ixfs_free_block(ixfs_vol_t *vol, uint32_t block);

/* Add a new block to an inode's extent list (merges if contiguous).
 * Returns the allocated block number or 0 on failure. */
uint32_t ixfs_extent_alloc(ixfs_vol_t *vol, struct ixfs_inode *inode);

/* Write file data at offset. Returns bytes written or -1 on error. */
int64_t ixfs_write_data(ixfs_vol_t *vol, uint32_t ino, struct ixfs_inode *inode,
                        uint64_t offset, const void *buf, uint64_t len);

/* Create a file or directory in a parent directory.
 * type: IXFS_S_FILE or IXFS_S_DIR. Returns new inode number or 0 on failure. */
uint32_t ixfs_create(ixfs_vol_t *vol, uint32_t parent_ino,
                     struct ixfs_inode *parent_inode,
                     const char *name, uint16_t type);

/* Delete a file or directory entry from a parent directory. Returns 0 on success. */
int ixfs_delete(ixfs_vol_t *vol, uint32_t parent_ino,
                struct ixfs_inode *parent_inode, const char *name);

/* Rename an entry within the same parent directory. Returns 0 on success. */
int ixfs_rename(ixfs_vol_t *vol, uint32_t parent_ino,
                struct ixfs_inode *parent_inode,
                const char *old_name, const char *new_name);

/* Flush all dirty state to disk (bitmap, superblock). */
int ixfs_flush(ixfs_vol_t *vol);

#endif /* IXFS_CORE_H */
