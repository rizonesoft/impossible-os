/* ============================================================================
 * ixfs.h — Impossible X FileSystem (IXFS) On-Disk Layout
 *
 * Custom filesystem for Impossible OS root partition (C:\).
 *
 * Disk layout (4 KiB blocks):
 *   Block 0:        Superblock
 *   Block 1..N:     Block bitmap (1 bit per block)
 *   Block N+1..M:   Inode table (fixed-size inode entries)
 *   Block M+1..end: Data blocks (file/directory content)
 *
 * Design:
 *   - 4 KiB block size (matches page size for efficient I/O)
 *   - Inodes hold 12 direct + 1 single-indirect block pointer
 *   - Directories are files containing ixfs_dir_entry records
 *   - Max filename: 251 characters (252 bytes with null terminator)
 * ============================================================================ */

#pragma once

#include "types.h"
#include "vfs.h"

/* --- Constants --- */

#define IXFS_MAGIC           0x49584653   /* "IXFS" */
#define IXFS_VERSION         1
#define IXFS_BLOCK_SIZE      4096         /* 4 KiB blocks */
#define IXFS_SECTORS_PER_BLK (IXFS_BLOCK_SIZE / 512)

/* Inode limits */
#define IXFS_DIRECT_BLOCKS   12          /* direct block pointers per inode */
#define IXFS_INDIRECT_BLOCKS 1           /* single-indirect pointers */
#define IXFS_PTRS_PER_BLOCK  (IXFS_BLOCK_SIZE / sizeof(uint32_t))  /* 1024 */
#define IXFS_MAX_FILE_BLOCKS (IXFS_DIRECT_BLOCKS + IXFS_PTRS_PER_BLOCK)

/* Max filename length in directory entries */
#define IXFS_MAX_NAME        252         /* 251 chars + null terminator */

/* Special inode numbers */
#define IXFS_ROOT_INODE      1           /* root directory is always inode 1 */
#define IXFS_INODE_FREE      0           /* inode 0 is reserved/unused */

/* Inode type flags (stored in i_mode) */
#define IXFS_S_FILE          0x8000      /* regular file */
#define IXFS_S_DIR           0x4000      /* directory */
#define IXFS_S_RDONLY        0x0001      /* read-only */

/* --- On-Disk Structures --- */

/* Superblock — always in block 0 (first 4 KiB of the partition) */
struct ixfs_superblock {
    uint32_t s_magic;              /* IXFS_MAGIC */
    uint32_t s_version;            /* filesystem version */
    uint32_t s_block_size;         /* block size in bytes (4096) */
    uint32_t s_total_blocks;       /* total blocks on the volume */
    uint32_t s_free_blocks;        /* number of free blocks */
    uint32_t s_total_inodes;       /* total inodes allocated */
    uint32_t s_free_inodes;        /* number of free inodes */
    uint32_t s_bitmap_start;       /* first block of block bitmap */
    uint32_t s_bitmap_blocks;      /* number of blocks in bitmap */
    uint32_t s_inode_start;        /* first block of inode table */
    uint32_t s_inode_blocks;       /* number of blocks in inode table */
    uint32_t s_data_start;         /* first data block */
    uint32_t s_root_inode;         /* inode number of root directory */
    uint8_t  s_volume_name[32];    /* volume label (null-terminated) */
    uint8_t  s_reserved[428];      /* pad to 512 bytes */
} __attribute__((packed));

/* Inode — 128 bytes each (32 inodes per block) */
struct ixfs_inode {
    uint16_t i_mode;               /* type + permissions (IXFS_S_FILE, etc.) */
    uint16_t i_links;              /* hard link count */
    uint32_t i_size;               /* file size in bytes */
    uint32_t i_blocks;             /* number of data blocks used */
    uint32_t i_ctime;              /* creation time (seconds since epoch) */
    uint32_t i_mtime;              /* modification time */
    uint32_t i_atime;              /* access time */
    uint32_t i_direct[IXFS_DIRECT_BLOCKS];   /* direct block pointers */
    uint32_t i_indirect;           /* single-indirect block pointer */
    uint8_t  i_reserved[12];       /* pad to 128 bytes */
} __attribute__((packed));

#define IXFS_INODES_PER_BLOCK  (IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode))

/* Directory entry — 64 bytes each (64 entries per block) */
struct ixfs_dir_entry {
    uint32_t d_inode;              /* inode number (0 = deleted/free) */
    char     d_name[IXFS_MAX_NAME]; /* filename (null-terminated) */
} __attribute__((packed));

#define IXFS_DIRENTS_PER_BLOCK (IXFS_BLOCK_SIZE / sizeof(struct ixfs_dir_entry))

/* --- API --- */

/* Format a disk region as IXFS (creates superblock, bitmap, inodes, root dir) */
int ixfs_format(uint32_t partition_lba, uint32_t total_sectors,
                const char *volume_name);

/* Mount an IXFS volume from disk */
int ixfs_init(uint32_t partition_lba);

/* Get VFS driver and root node */
struct vfs_fs_driver *ixfs_get_driver(void);
struct vfs_node *ixfs_get_root(void);
