/* ============================================================================
 * fat32_internal.h -- Shared internal types, structs, and function declarations
 *
 * Included by all FAT32 module files. NOT part of the public API.
 * Each mounted FAT32 partition gets its own fat32_volume context.
 * ============================================================================ */

#pragma once

#include "kernel/fs/fat32.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "kernel/timer.h"
#include "kernel/nt/filetime.h"
#include "kernel/time/wall_clock.h"
#include "kernel/time/timezone.h"

/* FAT32 special cluster values */
#define FAT32_EOC       0x0FFFFFF8   /* end of chain (>= this value) */
#define FAT32_FREE      0x00000000
#define FAT32_BAD       0x0FFFFFF7

/* FAT32 directory entry attributes */
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F

/* LFN sequence number mask and last-entry flag */
#define LFN_SEQ_MASK    0x1F
#define LFN_LAST_ENTRY  0x40

/* Maximum directory entries we'll track per volume */
#define FAT32_MAX_DIR_ENTRIES 128
#define FAT32_MAX_NAME        256   /* Support long filenames */
#define FAT32_LFN_CHARS       13    /* Characters per LFN entry */
#define FAT32_LFN_MAX_ENTRIES 20    /* Max LFN entries (260 chars / 13) */

/* Sector cache configuration */
#define SCACHE_SLOTS   64

/* FSInfo sector (FAT32 spec §7.1) */
#define FSINFO_LEAD_SIG   0x41615252
#define FSINFO_STRUCT_SIG 0x61417272
#define FSINFO_TRAIL_SIG  0xAA550000
#define FSINFO_UNKNOWN    0xFFFFFFFF  /* "not known" sentinel */

/* On-disk directory entry (32 bytes) */
struct fat32_dir_entry {
    uint8_t  name[11];          /* 8.3 short name */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed));

/* On-disk LFN entry (32 bytes, overlaid on dir entry) */
struct fat32_lfn_entry {
    uint8_t  seq;               /* Sequence number (ORed with 0x40 for last) */
    uint16_t name1[5];          /* Characters 1-5 (UTF-16LE) */
    uint8_t  attr;              /* Always 0x0F */
    uint8_t  type;              /* Always 0 for LFN */
    uint8_t  checksum;          /* Short name checksum */
    uint16_t name2[6];          /* Characters 6-11 (UTF-16LE) */
    uint16_t first_cluster;     /* Always 0 */
    uint16_t name3[2];          /* Characters 12-13 (UTF-16LE) */
} __attribute__((packed));

/* Sector cache entry */
typedef struct {
    uint32_t sector;       /* LBA of cached sector (0 = unused sentinel) */
    uint8_t  data[512];    /* sector payload */
    uint32_t last_access;  /* monotonic counter for LRU eviction */
    uint8_t  valid;        /* 1 = slot contains valid data */
    uint8_t  dirty;        /* 1 = modified, needs write-back */
} scache_entry_t;

/* Forward declaration */
struct fat32_volume;

/* In-memory file/directory node */
struct fat32_file {
    struct vfs_node node;
    uint32_t first_cluster;
    uint32_t file_size;
    uint32_t dir_cluster;          /* parent directory cluster */
    struct fat32_volume *volume;   /* back-pointer to owning volume */
};

/* Per-volume state -- one for each mounted FAT32 partition.
 * Allocated via PMM (~40 KB). */
struct fat32_volume {
    struct fat32_bpb    bpb;
    const struct blkdev *dev;
    spinlock_t          lock;    /* protects writes, FAT mods, dir updates */
    struct fat32_file   root_file;
    struct fat32_file   dir_files[FAT32_MAX_DIR_ENTRIES];
    uint32_t            dir_file_count;
    struct vfs_dirent   dirent;
    uint8_t             sector_buf[512];
    /* Sector cache */
    scache_entry_t      cache[SCACHE_SLOTS];
    uint32_t            cache_access;
    /* FSInfo state */
    uint32_t            fsinfo_sector;
    uint32_t            fsinfo_free_count;
    uint32_t            fsinfo_next_free;
    int                 fsinfo_valid;
    int                 fsinfo_dirty;
    /* Volume health */
    uint8_t             volume_dirty;   /* 1 if FAT[1] dirty bit was clear on mount */
};

/* ---- fat32_core.c: String helpers, sector I/O, cache, FAT ops ---- */

/* String helpers (stateless -- no vol needed) */
void     fat32_strcpy(char *dst, const char *src, uint32_t max);
int      fat32_strcasecmp(const char *a, const char *b);
void     fat32_short_name_to_str(const uint8_t *raw, char *out);
void     lfn_extract_chars(const struct fat32_lfn_entry *lfn,
                           char *name_buf, int seq_index);

/* Sector cache */
void     scache_flush(struct fat32_volume *vol);
void     scache_invalidate(struct fat32_volume *vol);

/* Sector I/O */
int      fat32_read_sector(struct fat32_volume *vol, uint32_t sector,
                            void *buf);
int      fat32_read_sectors_multi(struct fat32_volume *vol, uint32_t sector,
                                   uint32_t count, void *buf);
int      fat32_write_sector(struct fat32_volume *vol, uint32_t sector,
                             const void *buf);
int      fat32_write_sectors_multi(struct fat32_volume *vol, uint32_t sector,
                                    uint32_t count, const void *buf);

/* FAT entry manipulation */
uint32_t fat32_get_fat_entry(struct fat32_volume *vol, uint32_t cluster);
int      fat32_set_fat_entry(struct fat32_volume *vol, uint32_t cluster,
                              uint32_t value);
uint32_t fat32_next_cluster(struct fat32_volume *vol, uint32_t cluster);
uint32_t cluster_to_sector(struct fat32_volume *vol, uint32_t cluster);

/* BPB validation */
int      fat32_validate_bpb(struct fat32_volume *vol);

/* Dual-FAT compare and repair */
void     fat32_compare_repair_fats(struct fat32_volume *vol);

/* Dirty volume marker -- reads/writes FAT[1] bit 27 */
void     fat32_read_dirty_marker(struct fat32_volume *vol);
void     fat32_set_clean_marker(struct fat32_volume *vol);

/* FSInfo flush and free cluster scan */
void     fat32_fsinfo_flush(struct fat32_volume *vol);
uint32_t fat32_count_free_clusters(struct fat32_volume *vol);

/* Cluster allocation */
uint32_t fat32_alloc_cluster(struct fat32_volume *vol);
void     fat32_free_chain(struct fat32_volume *vol, uint32_t cluster);
int      fat32_zero_cluster(struct fat32_volume *vol, uint32_t cluster);

/* ---- fat32_dir.c: Directory operations ---- */

void     fat32_make_short_name(const char *name, uint8_t *short_name);

/* LFN helpers */
uint8_t  fat32_lfn_checksum(const uint8_t sfn[11]);
int      fat32_lfn_slot_count(const char *name);
int      fat32_needs_lfn(const char *name);
int      fat32_generate_sfn(struct fat32_volume *vol, uint32_t dir_cluster,
                            const char *name, uint8_t *sfn);
int      fat32_find_free_dir_slots(struct fat32_volume *vol,
                                    uint32_t dir_cluster, uint32_t count,
                                    uint32_t *out_sector, uint32_t *out_offset,
                                    uint32_t *out_cluster);
int      fat32_lfn_write_slots(struct fat32_volume *vol,
                                uint32_t dir_cluster,
                                const uint8_t sfn[11],
                                const char *utf8_name,
                                const struct fat32_dir_entry *sfn_entry);
int      fat32_find_free_dir_slot(struct fat32_volume *vol,
                                   uint32_t dir_cluster,
                                   uint32_t *out_sector,
                                   uint32_t *out_offset,
                                   uint32_t *out_cluster);
int      fat32_write_dir_entry(struct fat32_volume *vol, uint32_t sector,
                                uint32_t offset,
                                const struct fat32_dir_entry *entry);
void     fat32_read_dir(struct fat32_volume *vol, uint32_t cluster);
int      fat32_update_dir_size(struct fat32_volume *vol, uint32_t search_dir,
                                uint32_t target_fc, uint32_t new_size);
void     seconds_to_fat_datetime(uint32_t secs, uint16_t *out_time,
                                  uint16_t *out_date);
void     fat32_stamp_create(struct fat32_dir_entry *de);
void     fat32_stamp_modify(struct fat32_dir_entry *de);

/* ---- fat32_write.c: Write API ---- */

int      fat32_truncate(struct fat32_volume *vol, uint32_t dir_cluster,
                         const char *name, uint32_t new_size);
int      fat32_rmdir(struct fat32_volume *vol, uint32_t parent_cluster,
                      const char *name);
int      fat32_set_attr(struct fat32_volume *vol, uint32_t dir_cluster,
                         const char *name, uint8_t new_attr);
int      fat32_set_times(struct fat32_volume *vol, uint32_t dir_cluster,
                          const char *name, const filetime_t *ctime_p,
                          const filetime_t *mtime_p, const filetime_t *atime_p);
int      fat32_flush_disk(struct fat32_volume *vol);

/* Volume-aware write functions (used by VFS ops layer) */
int      fat32_create_file_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                                const char *name);
int      fat32_create_dir_vol(struct fat32_volume *vol, uint32_t parent_cluster,
                               const char *name);
int      fat32_delete_file_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                                const char *name);
int      fat32_rename_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                           const char *old_name, const char *new_name);

/* ---- fat32_fsck.c: Filesystem consistency checker ---- */

int      fat32_fsck(struct fat32_volume *vol, int fix);

/* ---- fat32_ops.c: VFS ops tables ---- */

extern struct vfs_ops fat32_file_ops;
extern struct vfs_ops fat32_dir_ops;
