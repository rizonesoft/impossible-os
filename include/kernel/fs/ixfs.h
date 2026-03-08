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

/* Block group constants (in-memory allocation optimization) */
#define IXFS_BLOCKS_PER_GROUP (IXFS_BLOCK_SIZE * 8)  /* 32768 blocks = 128 MiB */
#define IXFS_MAX_BLOCK_GROUPS 256  /* supports up to 256 × 128 MiB = 32 GiB */

/* In-memory block group descriptor (not stored on disk) */
struct ixfs_block_group {
    uint32_t bg_start;       /* first block number in this group */
    uint32_t bg_count;       /* total blocks in this group */
    uint32_t bg_free;        /* number of free blocks */
    uint32_t bg_next_free;   /* hint: next block to try allocating */
};

/* Extent-based allocation */
#define IXFS_INLINE_EXTENTS  4    /* extents stored directly in inode */
#define IXFS_EXTENT_OVERFLOW 0x01 /* flag: overflow extent block in use */

/* Max blocks addressable via 4 inline extents: each can cover up to 2^32 blocks
 * With overflow tree: effectively unlimited */
#define IXFS_MAX_FILE_BLOCKS 0xFFFFFFFF  /* 4 KiB × 2^32 = 16 TiB (per extent) */

/* Extent — describes a contiguous run of blocks (12 bytes) */
struct ixfs_extent {
    uint64_t e_start;            /* first block number (64-bit for 64 TiB) */
    uint32_t e_count;            /* number of contiguous blocks (0 = unused) */
} __attribute__((packed));

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

/* --- Write-Ahead Log (Journal) --- */

#define IXFS_JOURNAL_BLOCKS  16          /* default journal size (64 KiB) */
#define IXFS_JOURNAL_MAGIC   0x4A584653  /* "JXFS" */
#define IXFS_JE_DATA         1           /* journal entry: block data */
#define IXFS_JE_COMMIT       2           /* journal entry: transaction committed */
#define IXFS_TXN_MAX_ENTRIES 8           /* max blocks per transaction */

/* --- Copy-on-Write + Snapshots --- */

#define IXFS_MAX_SNAPSHOTS   8           /* max simultaneous snapshots */
#define IXFS_SNAP_NAME_LEN   32          /* snapshot name length */
#define IXFS_REFCOUNT_BLOCKS 2           /* blocks for refcount table (8192 entries) */
#define IXFS_SNAPSHOT_BLOCKS 1           /* block for snapshot table */

/* On-disk snapshot entry (64 bytes) — 64 per block */
struct ixfs_snapshot_entry {
    char     se_name[IXFS_SNAP_NAME_LEN]; /* snapshot name */
    uint32_t se_timestamp;               /* creation time */
    uint32_t se_root_block;              /* block storing saved inode table */
    uint32_t se_inode_blocks;            /* number of inode blocks saved */
    uint32_t se_flags;                   /* 1=active, 0=deleted */
    uint8_t  se_reserved[12];            /* pad to 64 bytes */
} __attribute__((packed));

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
    uint32_t s_journal_start;      /* first block of journal area */
    uint32_t s_journal_blocks;     /* number of journal blocks */
    uint32_t s_journal_seq;        /* current transaction sequence */
    uint32_t s_refcount_start;     /* first block of refcount table */
    uint32_t s_refcount_blocks;    /* number of refcount blocks */
    uint32_t s_snapshot_start;     /* first block of snapshot table */
    uint32_t s_snapshot_count;     /* number of active snapshots */
    uint8_t  s_reserved[400];      /* pad to 512 bytes */
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
    struct ixfs_extent i_extents[IXFS_INLINE_EXTENTS]; /* 4 inline extents (48 bytes) */
    uint8_t  i_extent_count;       /* number of active inline extents (0-4) */
    uint8_t  i_extent_flags;       /* IXFS_EXTENT_OVERFLOW if overflow in use */
    uint16_t i_extent_pad;         /* alignment padding */
    uint64_t i_extent_block;       /* block for overflow extent tree (0=none) */
} __attribute__((packed));

#define IXFS_INODES_PER_BLOCK  (IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode))

/* Directory entry — 64 bytes each (64 entries per block) */
struct ixfs_dir_entry {
    uint32_t d_inode;              /* inode number (0 = deleted/free) */
    char     d_name[IXFS_MAX_NAME]; /* filename (null-terminated) */
} __attribute__((packed));

#define IXFS_DIRENTS_PER_BLOCK (IXFS_BLOCK_SIZE / sizeof(struct ixfs_dir_entry))

/* --- API --- */

/* Forward declaration */
struct blkdev;

/* Format a disk region as IXFS (creates superblock, bitmap, inodes, root dir) */
int ixfs_format(const struct blkdev *dev, const char *volume_name);

/* Mount an IXFS volume from a block device */
int ixfs_init(const struct blkdev *dev);

/* Get VFS driver and root node */
struct vfs_fs_driver *ixfs_get_driver(void);
struct vfs_node *ixfs_get_root(void);

/* Permission check: returns 0 if access allowed, -1 if denied */
int ixfs_check_perm(const struct ixfs_inode *inode, uint16_t uid,
                    uint16_t gid, int want_write);

/* Rename a file or directory in the given parent directory */
int ixfs_rename(struct vfs_node *parent, const char *old_name,
                const char *new_name);

/* Test block groups, buffer cache, and directory hash index */
void ixfs_test_performance(void);

/* Snapshot API */
int ixfs_snapshot_create(const char *name);
int ixfs_snapshot_list(void);
int ixfs_snapshot_restore(const char *name);
int ixfs_snapshot_delete(const char *name);

/* Stat: report file metadata including sparse info */
int ixfs_stat(struct vfs_node *node, uint32_t *logical_size,
              uint32_t *actual_blocks);
