/* ============================================================================
 * ixfs-structs.h — IXFS on-disk structures for host tools
 *
 * COPY of on-disk structures from include/kernel/fs/ixfs.h, using <stdint.h>
 * instead of kernel types. Keep in sync manually — any IXFS format change
 * must be reflected here.
 * ============================================================================ */

#ifndef IXFS_STRUCTS_H
#define IXFS_STRUCTS_H

#include <stdint.h>

/* --- Constants --- */

#define IXFS_MAGIC           0x49584653   /* "IXFS" */
#define IXFS_VERSION         2
#define IXFS_BLOCK_SIZE      4096
#define IXFS_SECTORS_PER_BLK (IXFS_BLOCK_SIZE / 512)
#define IXFS_INLINE_EXTENTS  4
#define IXFS_EXTENT_OVERFLOW 0x01
#define IXFS_INLINE          0x02
#define IXFS_INLINE_MAX      48
#define IXFS_MAX_NAME        252
#define IXFS_ROOT_INODE      1
#define IXFS_S_FILE          0x8000
#define IXFS_S_DIR           0x4000
#define IXFS_S_TYPEMASK      0xF000

/* --- On-disk structures (packed, must match kernel) --- */

#pragma pack(push, 1)

struct ixfs_extent {
    uint64_t e_start;
    uint32_t e_count;
};

struct ixfs_superblock {
    uint32_t s_magic;
    uint32_t s_version;
    uint32_t s_block_size;
    uint64_t s_total_blocks;
    uint64_t s_free_blocks;
    uint32_t s_total_inodes;
    uint32_t s_free_inodes;
    uint32_t s_bitmap_start;
    uint32_t s_bitmap_blocks;
    uint32_t s_inode_start;
    uint32_t s_inode_blocks;
    uint32_t s_data_start;
    uint32_t s_root_inode;
    uint8_t  s_volume_name[32];
    uint32_t s_journal_start;
    uint32_t s_journal_blocks;
    uint32_t s_journal_seq;
    uint32_t s_refcount_start;
    uint32_t s_refcount_blocks;
    uint32_t s_snapshot_start;
    uint32_t s_snapshot_count;
    uint32_t s_checksum_start;
    uint32_t s_checksum_blocks;
    uint32_t s_checksum;
    uint8_t  s_reserved[380];
};

struct ixfs_inode {
    uint16_t i_mode;
    uint16_t i_links;
    uint16_t i_uid;
    uint16_t i_gid;
    uint64_t i_size;
    uint32_t i_blocks;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_atime;
    struct ixfs_extent i_extents[IXFS_INLINE_EXTENTS];
    uint8_t  i_extent_count;
    uint8_t  i_extent_flags;
    uint16_t i_extent_pad;
    uint64_t i_extent_block;
};

struct ixfs_dir_entry {
    uint32_t d_inode;
    char     d_name[IXFS_MAX_NAME];
};

#pragma pack(pop)

#define IXFS_INODES_PER_BLOCK  (IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode))
#define IXFS_DIRENTS_PER_BLOCK (IXFS_BLOCK_SIZE / sizeof(struct ixfs_dir_entry))

#endif /* IXFS_STRUCTS_H */
