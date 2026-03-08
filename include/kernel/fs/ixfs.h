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
 *   - Inodes hold 12 direct + 1 single-indirect + 1 double-indirect ptr
 *   - Directories are files containing ixfs_dir_entry records
 *   - Max filename: 251 characters (252 bytes with null terminator)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/fs/vfs.h"

/* --- Constants --- */

#define IXFS_MAGIC           0x49584653   /* "IXFS" */
#define IXFS_VERSION         1
#define IXFS_BLOCK_SIZE      4096         /* 4 KiB blocks */
#define IXFS_SECTORS_PER_BLK (IXFS_BLOCK_SIZE / 512)

/* Inode limits */
#define IXFS_DIRECT_BLOCKS   12          /* direct block pointers per inode */
#define IXFS_INDIRECT_BLOCKS 1           /* single-indirect pointers */
#define IXFS_PTRS_PER_BLOCK  (IXFS_BLOCK_SIZE / sizeof(uint32_t))  /* 1024 */
#define IXFS_MAX_FILE_BLOCKS (IXFS_DIRECT_BLOCKS + IXFS_PTRS_PER_BLOCK \
                            + IXFS_PTRS_PER_BLOCK * IXFS_PTRS_PER_BLOCK)

/* Max filename length in directory entries */
#define IXFS_MAX_NAME        252         /* 251 chars + null terminator */

/* Special inode numbers */
#define IXFS_ROOT_INODE      1           /* root directory is always inode 1 */
#define IXFS_INODE_FREE      0           /* inode 0 is reserved/unused */

/* Inode type flags (stored in upper bits of i_mode) */
#define IXFS_S_FILE          0x8000      /* regular file */
#define IXFS_S_DIR           0x4000      /* directory */
#define IXFS_S_TYPEMASK      0xF000      /* mask for type bits */

/* Permission bits (stored in lower 9 bits of i_mode, Unix-style) */
#define IXFS_S_IRUSR         0x0100      /* owner read */
#define IXFS_S_IWUSR         0x0080      /* owner write */
#define IXFS_S_IXUSR         0x0040      /* owner execute */
#define IXFS_S_IRGRP         0x0020      /* group read */
#define IXFS_S_IWGRP         0x0010      /* group write */
#define IXFS_S_IXGRP         0x0008      /* group execute */
#define IXFS_S_IROTH         0x0004      /* other read */
#define IXFS_S_IWOTH         0x0002      /* other write */
#define IXFS_S_IXOTH         0x0001      /* other execute */

/* Common permission combos */
#define IXFS_PERM_FILE       0x01B4      /* rw-rw-r-- (0664) */
#define IXFS_PERM_DIR        0x01ED      /* rwxrwxr-x (0775) */
#define IXFS_PERM_READONLY   0x0124      /* r--r--r-- (0444) */

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
    uint16_t i_mode;               /* type (upper 4) + permissions (lower 9) */
    uint16_t i_links;              /* hard link count */
    uint16_t i_uid;                /* owner user ID */
    uint16_t i_gid;                /* owner group ID */
    uint32_t i_size;               /* file size in bytes */
    uint32_t i_blocks;             /* number of data blocks used */
    uint32_t i_ctime;              /* creation time (seconds since epoch) */
    uint32_t i_mtime;              /* modification time */
    uint32_t i_atime;              /* access time */
    uint32_t i_direct[IXFS_DIRECT_BLOCKS];   /* direct block pointers */
    uint32_t i_indirect;           /* single-indirect block pointer */
    uint32_t i_dindirect;          /* double-indirect block pointer */
    uint8_t  i_reserved[4];        /* pad to 128 bytes */
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

/* Permission check: returns 0 if access allowed, -1 if denied */
int ixfs_check_perm(const struct ixfs_inode *inode, uint16_t uid,
                    uint16_t gid, int want_write);
