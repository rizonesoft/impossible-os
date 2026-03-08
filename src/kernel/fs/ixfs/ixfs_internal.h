/* ============================================================================
 * ixfs_internal.h — Shared internal types, structs, and function declarations
 *
 * Included by all IXFS module files. NOT part of the public API.
 * ============================================================================ */

#pragma once

#include "kernel/fs/ixfs.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/drivers/pit.h"
#include "kernel/printk.h"

/* Number of inodes to allocate (fixed at format time) */
#define IXFS_DEFAULT_INODES  256

/* --- Multi-volume types and constants --- */

#define IXFS_MAX_VOLUMES     4
#define IXFS_CACHE_SIZE      64   /* cached blocks per volume */
#define IXFS_MAX_OPEN_NODES  128  /* open vnodes per volume */

/* Directory hash index constants */
#define IXFS_HASH_BUCKETS   128
#define IXFS_HASH_THRESHOLD 64
#define IXFS_HASH_CHAIN_END 0xFFFFFFFF

/* Buffer cache entry (per-volume, in-memory) */
struct ixfs_cache_entry {
    uint32_t block;           /* block number (0xFFFFFFFF = unused) */
    uint8_t  data[IXFS_BLOCK_SIZE];
    uint8_t  dirty;           /* 1 = needs write-back */
    uint32_t lru_tick;        /* higher = more recently used */
};

/* Hash index for large directories (per-vnode, in-memory) */
struct ixfs_hash_node {
    uint32_t entry_index;
    uint32_t next;
};

struct ixfs_dir_hash {
    uint32_t bucket[128];
    struct ixfs_hash_node *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
};

/* Per-file/directory in-memory node */
struct ixfs_vnode {
    struct vfs_node    node;
    uint32_t           ino;
    struct ixfs_inode  inode;
    struct ixfs_dir_hash *dir_hash;
    struct ixfs_volume *vol;      /* back-pointer to owning volume */
};

/* Per-volume state — one for each mounted IXFS partition */
struct ixfs_volume {
    int                      in_use;
    const struct blkdev     *dev;
    struct ixfs_superblock   sb;
    uint8_t                 *block_bitmap;
    uint32_t                 bitmap_bytes;
    uint8_t                  blk_buf[IXFS_BLOCK_SIZE];
    struct ixfs_block_group  groups[IXFS_MAX_BLOCK_GROUPS];
    uint32_t                 group_count;
    struct ixfs_cache_entry  cache[IXFS_CACHE_SIZE];
    uint32_t                 cache_tick;
    struct ixfs_vnode        vnodes[IXFS_MAX_OPEN_NODES];
    uint32_t                 vnode_count;
    struct vfs_dirent        dirent;

    /* WAL journal state */
    uint32_t                 j_head;       /* next free journal slot (1-based) */
    struct {
        int      active;
        uint32_t txn_id;
        uint32_t count;
        uint32_t blocks[IXFS_TXN_MAX_ENTRIES];
        uint32_t j_slots[IXFS_TXN_MAX_ENTRIES];  /* journal block for each */
    } txn;

    /* CoW + Snapshot state */
    uint8_t                 *refcount_table;  /* per-block refcounts */
    uint32_t                 refcount_bytes;
    struct ixfs_snapshot_entry snapshots[IXFS_MAX_SNAPSHOTS];

    /* Volume state */
    uint8_t                  read_only;        /* 1 = v1 volume, writes blocked */

    /* Per-block checksum state */
    uint32_t                *checksum_table;   /* CRC32C per block */
    uint32_t                 checksum_count;   /* total blocks tracked */
};

/* On-disk journal header — stored at journal block 0 */
struct ixfs_journal_header {
    uint32_t jh_magic;               /* IXFS_JOURNAL_MAGIC */
    uint32_t jh_head;                /* next free entry (1-based offset) */
    uint32_t jh_tail;                /* oldest unfinished entry */
    uint32_t jh_seq;                 /* current sequence number */
} __attribute__((packed));

/* On-disk journal entry — one per journal block (16-byte header + 4080 data) */
struct ixfs_journal_entry {
    uint32_t je_txn_id;              /* transaction ID that owns this entry */
    uint32_t je_type;                /* IXFS_JE_DATA or IXFS_JE_COMMIT */
    uint32_t je_target;              /* target disk block (for DATA entries) */
    uint32_t je_checksum;            /* simple additive checksum of je_data */
    uint8_t  je_data[4080];          /* first 4080 bytes of target block */
} __attribute__((packed));

/* --- Global volume table (defined in ixfs_format.c) --- */
extern struct ixfs_volume volumes[IXFS_MAX_VOLUMES];

/* --- ixfs_core.c: String helpers, disk I/O, buffer cache --- */
void ixfs_strcpy(char *dst, const char *src, uint32_t max);
int  ixfs_strcmp(const char *a, const char *b);
int  ixfs_disk_read(struct ixfs_volume *vol, uint32_t block, void *buf);
int  ixfs_disk_write(struct ixfs_volume *vol, uint32_t block, const void *buf);
void ixfs_cache_init(struct ixfs_volume *vol);
struct ixfs_cache_entry *ixfs_cache_find(struct ixfs_volume *vol, uint32_t block);
struct ixfs_cache_entry *ixfs_cache_lru(struct ixfs_volume *vol);
int  ixfs_cache_flush(struct ixfs_volume *vol);
int  ixfs_read_block(struct ixfs_volume *vol, uint32_t block, void *buf);
int  ixfs_write_block(struct ixfs_volume *vol, uint32_t block, const void *buf);
int  ixfs_zero_block(struct ixfs_volume *vol, uint32_t block);
uint32_t ixfs_crc32c(const void *data, uint32_t len);
int  ixfs_checksum_load(struct ixfs_volume *vol);
int  ixfs_checksum_flush(struct ixfs_volume *vol);
void ixfs_checksum_update(struct ixfs_volume *vol, uint32_t block,
                           const void *data);
int  ixfs_checksum_verify(struct ixfs_volume *vol, uint32_t block,
                           const void *data);

/* --- ixfs_alloc.c: Bitmap, block groups, alloc/free --- */
void     bitmap_set(uint8_t *bmap, uint32_t bit);
void     bitmap_clear(uint8_t *bmap, uint32_t bit);
int      bitmap_test(const uint8_t *bmap, uint32_t bit);
void     ixfs_init_groups(struct ixfs_volume *vol);
uint32_t ixfs_alloc_block_near(struct ixfs_volume *vol, uint32_t preferred_group);
uint32_t ixfs_alloc_block(struct ixfs_volume *vol);
void     ixfs_free_block(struct ixfs_volume *vol, uint32_t block);
int      ixfs_flush_bitmap(struct ixfs_volume *vol);
int      ixfs_flush_superblock(struct ixfs_volume *vol);

/* --- ixfs_journal.c: Write-ahead log --- */
uint32_t ixfs_journal_checksum(const uint8_t *data, uint32_t len);
int  ixfs_journal_init(struct ixfs_volume *vol);
int  ixfs_journal_recover(struct ixfs_volume *vol);
void ixfs_txn_begin(struct ixfs_volume *vol);
int  ixfs_txn_write(struct ixfs_volume *vol, uint32_t target_block,
                     const void *data);
int  ixfs_txn_commit(struct ixfs_volume *vol);

/* --- ixfs_cow.c: Copy-on-Write + Snapshots --- */
int      ixfs_refcount_init(struct ixfs_volume *vol);
int      ixfs_refcount_flush(struct ixfs_volume *vol);
int      ixfs_refcount_load(struct ixfs_volume *vol);
int      ixfs_snapshot_flush(struct ixfs_volume *vol);
int      ixfs_snapshot_load(struct ixfs_volume *vol);
uint32_t ixfs_cow_block(struct ixfs_volume *vol, struct ixfs_inode *inode,
                         uint32_t file_block_idx, uint32_t old_disk_block);
struct ixfs_volume *ixfs_get_active_volume(void);

/* --- ixfs_inode.c: Inode I/O, vnodes, dir hash index --- */
int  ixfs_read_inode(struct ixfs_volume *vol, uint32_t ino,
                      struct ixfs_inode *inode);
int  ixfs_write_inode(struct ixfs_volume *vol, uint32_t ino,
                       const struct ixfs_inode *inode);
uint32_t ixfs_fnv1a(const char *name);
void     ixfs_hash_free(struct ixfs_vnode *v);
void     ixfs_hash_build(struct ixfs_volume *vol, struct ixfs_vnode *v);
uint32_t ixfs_hash_lookup(struct ixfs_volume *vol, struct ixfs_vnode *v,
                           const char *name, uint8_t *data_buf);
struct ixfs_vnode *ixfs_get_vnode(struct ixfs_volume *vol, uint32_t ino);

/* --- ixfs_extent.c: Extent-based block lookup --- */
uint32_t ixfs_get_block(struct ixfs_volume *vol,
                         const struct ixfs_inode *inode, uint32_t index);
uint32_t ixfs_add_block_to_extent(struct ixfs_volume *vol,
                                    struct ixfs_inode *inode);
void     ixfs_free_all_extents(struct ixfs_volume *vol,
                                struct ixfs_inode *inode);

/* --- ixfs_ops.c: VFS ops tables (defined there) --- */
extern struct vfs_ops ixfs_file_ops;
extern struct vfs_ops ixfs_dir_ops;
