/* ============================================================================
 * fat32.c — FAT32 Read-Only Filesystem Driver
 *
 * Reads a FAT32 volume via the block device abstraction layer, parses the
 * BPB, follows cluster chains, and provides VFS operations for directory
 * listing and file reading.
 *
 * Features:
 *   - BPB parsing and layout calculation
 *   - Cluster chain traversal via FAT
 *   - 8.3 short name and LFN (Long File Name) support
 *   - VFS integration (open/close/read/readdir/finddir)
 *   - fat32_stat() for file metadata
 *
 * Key concepts:
 *   BPB (sector 0) → describes layout
 *   FAT region → maps cluster → next cluster chain
 *   Data region → actual file/directory data
 *   Directory entries → 32 bytes each
 *   LFN entries → 32 bytes each, preceding the 8.3 entry, reverse order
 * ============================================================================ */

#include "kernel/fs/fat32.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"

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

/* Maximum directory entries we'll track */
#define FAT32_MAX_DIR_ENTRIES 128
#define FAT32_MAX_NAME        256   /* Support long filenames */
#define FAT32_LFN_CHARS       13    /* Characters per LFN entry */
#define FAT32_LFN_MAX_ENTRIES 20    /* Max LFN entries (260 chars / 13) */

/* In-memory file/directory node */
struct fat32_file {
    struct vfs_node node;
    uint32_t first_cluster;
    uint32_t file_size;
};

/* Static storage */
static struct fat32_bpb bpb;
static const struct blkdev *fat32_dev;   /* Block device for this volume */
static struct fat32_file root_file;
static struct fat32_file dir_files[FAT32_MAX_DIR_ENTRIES];
static uint32_t dir_file_count;
static struct vfs_dirent fat32_dirent;

/* Sector buffer */
static uint8_t sector_buf[512];

/* ---- Block device I/O ---- */

/* Read a single sector relative to partition start (handled by sub-blkdev) */
static int fat32_read_sector(uint32_t sector, void *buf)
{
    return blkdev_read(fat32_dev, sector, 1, buf);
}

/* Read multiple contiguous sectors */
static int fat32_read_sectors_multi(uint32_t sector, uint32_t count, void *buf)
{
    return blkdev_read(fat32_dev, sector, count, buf);
}

/* ---- FAT cluster operations ---- */

/* Get next cluster from FAT */
static uint32_t fat32_next_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = bpb.first_fat_sector + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;
    uint32_t next;

    if (fat32_read_sector(fat_sector, sector_buf) != 0)
        return FAT32_EOC;

    next = *(uint32_t *)&sector_buf[entry_offset];
    next &= 0x0FFFFFFF;   /* mask upper 4 bits */

    return next;
}

/* Get first sector of a cluster */
static uint32_t cluster_to_sector(uint32_t cluster)
{
    return bpb.first_data_sector + (cluster - 2) * bpb.sectors_per_cluster;
}

/* ---- String helpers ---- */

/* Convert 8.3 short name to readable string */
static void fat32_short_name_to_str(const uint8_t *raw, char *out)
{
    int i, j = 0;

    /* Copy name part (8 chars), trim trailing spaces */
    for (i = 0; i < 8; i++) {
        if (raw[i] != ' ')
            out[j++] = (char)(raw[i] >= 'A' && raw[i] <= 'Z'
                        ? raw[i] + 32 : raw[i]);   /* lowercase */
    }

    /* Add extension if present */
    if (raw[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11; i++) {
            if (raw[i] != ' ')
                out[j++] = (char)(raw[i] >= 'A' && raw[i] <= 'Z'
                            ? raw[i] + 32 : raw[i]);
        }
    }

    out[j] = '\0';
}

static void fat32_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Case-insensitive compare */
static int fat32_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- LFN (Long File Name) support ---- */

/* Extract characters from a single LFN entry into a buffer.
 * Each LFN entry contributes 13 UTF-16LE characters.
 * seq_index is 0-based (0 = chars 0-12, 1 = chars 13-25, etc.) */
static void lfn_extract_chars(const struct fat32_lfn_entry *lfn,
                              char *name_buf, int seq_index)
{
    int base = seq_index * FAT32_LFN_CHARS;
    int i;

    /* name1: 5 chars at buf positions base+0..base+4 */
    for (i = 0; i < 5; i++) {
        uint16_t ch = lfn->name1[i];
        if (ch == 0 || ch == 0xFFFF) return;
        name_buf[base + i] = (ch < 128) ? (char)ch : '_';
    }
    /* name2: 6 chars at buf positions base+5..base+10 */
    for (i = 0; i < 6; i++) {
        uint16_t ch = lfn->name2[i];
        if (ch == 0 || ch == 0xFFFF) return;
        name_buf[base + 5 + i] = (ch < 128) ? (char)ch : '_';
    }
    /* name3: 2 chars at buf positions base+11..base+12 */
    for (i = 0; i < 2; i++) {
        uint16_t ch = lfn->name3[i];
        if (ch == 0 || ch == 0xFFFF) return;
        name_buf[base + 11 + i] = (ch < 128) ? (char)ch : '_';
    }
}

/* ---- Directory reading with LFN support ---- */

static void fat32_read_dir(uint32_t cluster)
{
    uint32_t bytes_per_cluster;
    uint8_t *cluster_buf;
    uint32_t i;

    /* LFN assembly state */
    char lfn_buf[FAT32_MAX_NAME];
    int lfn_active = 0;     /* 1 if we're collecting LFN entries */

    dir_file_count = 0;
    bytes_per_cluster = bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return;

    /* Zero the LFN buffer */
    {
        int k;
        for (k = 0; k < FAT32_MAX_NAME; k++)
            lfn_buf[k] = '\0';
    }

    while (cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t sector = cluster_to_sector(cluster);

        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        /* Parse directory entries in this cluster */
        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de;
            struct fat32_file *f;
            char name[FAT32_MAX_NAME];
            uint32_t fc;

            if (dir_file_count >= FAT32_MAX_DIR_ENTRIES)
                break;

            de = (struct fat32_dir_entry *)&cluster_buf[i];

            /* End of directory */
            if (de->name[0] == 0x00)
                goto done;

            /* Deleted entry */
            if (de->name[0] == 0xE5) {
                lfn_active = 0;
                continue;
            }

            /* LFN entry — collect characters */
            if (de->attr == FAT32_ATTR_LFN) {
                struct fat32_lfn_entry *lfn =
                    (struct fat32_lfn_entry *)&cluster_buf[i];
                int seq = lfn->seq & LFN_SEQ_MASK;

                if (lfn->seq & LFN_LAST_ENTRY) {
                    /* This is the last (first encountered) LFN entry.
                     * Clear buffer and start collecting. */
                    int k;
                    for (k = 0; k < FAT32_MAX_NAME; k++)
                        lfn_buf[k] = '\0';
                    lfn_active = 1;
                }

                if (lfn_active && seq >= 1 && seq <= FAT32_LFN_MAX_ENTRIES)
                    lfn_extract_chars(lfn, lfn_buf, seq - 1);

                continue;
            }

            /* Volume label — skip */
            if (de->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_active = 0;
                continue;
            }

            /* Skip . and .. */
            if (de->name[0] == '.') {
                lfn_active = 0;
                continue;
            }

            /* This is a regular 8.3 entry. Use LFN name if available. */
            if (lfn_active && lfn_buf[0] != '\0') {
                fat32_strcpy(name, lfn_buf, FAT32_MAX_NAME);
            } else {
                fat32_short_name_to_str(de->name, name);
            }
            lfn_active = 0;

            fc = ((uint32_t)de->first_cluster_hi << 16)
               | (uint32_t)de->first_cluster_lo;

            f = &dir_files[dir_file_count];
            fat32_strcpy(f->node.name, name, VFS_MAX_NAME);
            f->node.type = (de->attr & FAT32_ATTR_DIRECTORY)
                           ? VFS_DIRECTORY : VFS_FILE;
            f->node.inode = fc;
            f->node.size = de->file_size;
            f->first_cluster = fc;
            f->file_size = de->file_size;

            dir_file_count++;
        }

        cluster = fat32_next_cluster(cluster);
    }

done:
    kfree(cluster_buf);
}

/* ---- VFS operations for FAT32 files ---- */

static int fat32_file_open(struct vfs_node *node, uint32_t flags)
{
    (void)node;
    (void)flags;
    return 0;
}

static int fat32_file_close(struct vfs_node *node)
{
    (void)node;
    return 0;
}

static int fat32_file_read(struct vfs_node *node, uint32_t offset,
                           uint32_t size, uint8_t *buffer)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t cluster;
    uint32_t bytes_per_cluster;
    uint32_t bytes_read = 0;
    uint32_t cluster_offset;
    uint32_t to_read;
    uint8_t *cluster_buf;

    if (!f || offset >= f->file_size)
        return 0;

    if (offset + size > f->file_size)
        size = f->file_size - offset;

    bytes_per_cluster = bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    /* Skip clusters to reach the offset */
    cluster = f->first_cluster;
    {
        uint32_t skip = offset / bytes_per_cluster;
        uint32_t s;
        for (s = 0; s < skip && cluster < FAT32_EOC; s++)
            cluster = fat32_next_cluster(cluster);
    }
    cluster_offset = offset % bytes_per_cluster;

    /* Read data cluster by cluster */
    while (bytes_read < size && cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t sector = cluster_to_sector(cluster);
        uint32_t ci;

        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        to_read = bytes_per_cluster - cluster_offset;
        if (to_read > size - bytes_read)
            to_read = size - bytes_read;

        for (ci = 0; ci < to_read; ci++)
            buffer[bytes_read + ci] = cluster_buf[cluster_offset + ci];

        bytes_read += to_read;
        cluster_offset = 0;   /* only first cluster may have an offset */
        cluster = fat32_next_cluster(cluster);
    }

    kfree(cluster_buf);
    return (int)bytes_read;
}

static struct vfs_ops fat32_file_ops = {
    .open    = fat32_file_open,
    .close   = fat32_file_close,
    .read    = fat32_file_read,
    .write   = (void *)0,
    .readdir = (void *)0,
    .finddir = (void *)0,
    .create  = (void *)0,
    .unlink  = (void *)0,
};

/* ---- VFS operations for FAT32 directories ---- */

static struct vfs_dirent *fat32_readdir(struct vfs_node *node, uint32_t index)
{
    (void)node;

    /* Lazily load root directory entries */
    if (dir_file_count == 0)
        fat32_read_dir(bpb.root_cluster);

    if (index >= dir_file_count)
        return (struct vfs_dirent *)0;

    fat32_strcpy(fat32_dirent.name, dir_files[index].node.name, VFS_MAX_NAME);
    fat32_dirent.inode = dir_files[index].node.inode;
    fat32_dirent.type = dir_files[index].node.type;

    return &fat32_dirent;
}

static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name)
{
    uint32_t i;
    (void)node;

    if (dir_file_count == 0)
        fat32_read_dir(bpb.root_cluster);

    for (i = 0; i < dir_file_count; i++) {
        if (fat32_strcasecmp(dir_files[i].node.name, name)) {
            dir_files[i].node.ops = &fat32_file_ops;
            dir_files[i].node.fs_data = &dir_files[i];
            return &dir_files[i].node;
        }
    }

    return (struct vfs_node *)0;
}

static struct vfs_ops fat32_dir_ops = {
    .open    = fat32_file_open,
    .close   = (void *)0,
    .read    = (void *)0,
    .write   = (void *)0,
    .readdir = fat32_readdir,
    .finddir = fat32_finddir,
    .create  = (void *)0,
    .unlink  = (void *)0,
};

static struct vfs_fs_driver fat32_driver = {
    .name      = "FAT32",
    .ops       = &fat32_dir_ops,
    .priv_data = (void *)0,
};

/* ---- fat32_stat: file metadata lookup ---- */

int fat32_stat(const char *path, struct fat32_stat_info *info)
{
    uint32_t cluster;
    uint32_t bytes_per_cluster;
    uint8_t *cluster_buf;
    uint32_t i;
    const char *component;
    const char *next;
    int found;

    if (!info || !path)
        return -1;

    /* Start from root directory */
    cluster = bpb.root_cluster;
    bytes_per_cluster = bpb.sectors_per_cluster * 512;

    /* Skip leading separators */
    while (*path == '/' || *path == '\\')
        path++;

    if (*path == '\0') {
        /* Root directory itself */
        info->size = 0;
        info->attributes = FAT32_ATTR_DIRECTORY;
        info->type = VFS_DIRECTORY;
        info->create_date = 0;
        info->create_time = 0;
        info->modify_date = 0;
        info->modify_time = 0;
        info->first_cluster = cluster;
        return 0;
    }

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    component = path;

    while (component && *component) {
        char target[FAT32_MAX_NAME];
        int ti = 0;

        /* Extract current path component */
        next = component;
        while (*next && *next != '/' && *next != '\\')
            next++;
        while (component < next && ti < FAT32_MAX_NAME - 1)
            target[ti++] = *component++;
        target[ti] = '\0';

        /* Skip separator */
        while (*component == '/' || *component == '\\')
            component++;

        /* Search this directory for the target */
        found = 0;
        {
            uint32_t cur_cluster = cluster;
            /* LFN assembly */
            char lfn_name[FAT32_MAX_NAME];
            int lfn_active_s = 0;
            int k;

            for (k = 0; k < FAT32_MAX_NAME; k++)
                lfn_name[k] = '\0';

            while (cur_cluster < FAT32_EOC && cur_cluster != FAT32_FREE) {
                uint32_t sector = cluster_to_sector(cur_cluster);

                if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                             cluster_buf) != 0)
                    break;

                for (i = 0; i < bytes_per_cluster; i += 32) {
                    struct fat32_dir_entry *de =
                        (struct fat32_dir_entry *)&cluster_buf[i];
                    char ename[FAT32_MAX_NAME];

                    if (de->name[0] == 0x00) goto stat_not_found;
                    if (de->name[0] == 0xE5) { lfn_active_s = 0; continue; }

                    if (de->attr == FAT32_ATTR_LFN) {
                        struct fat32_lfn_entry *lfn =
                            (struct fat32_lfn_entry *)&cluster_buf[i];
                        int seq = lfn->seq & LFN_SEQ_MASK;
                        if (lfn->seq & LFN_LAST_ENTRY) {
                            for (k = 0; k < FAT32_MAX_NAME; k++)
                                lfn_name[k] = '\0';
                            lfn_active_s = 1;
                        }
                        if (lfn_active_s && seq >= 1 &&
                            seq <= FAT32_LFN_MAX_ENTRIES)
                            lfn_extract_chars(lfn, lfn_name, seq - 1);
                        continue;
                    }

                    if (de->attr & FAT32_ATTR_VOLUME_ID) {
                        lfn_active_s = 0; continue;
                    }

                    /* Build name */
                    if (lfn_active_s && lfn_name[0] != '\0')
                        fat32_strcpy(ename, lfn_name, FAT32_MAX_NAME);
                    else
                        fat32_short_name_to_str(de->name, ename);
                    lfn_active_s = 0;

                    if (fat32_strcasecmp(ename, target)) {
                        uint32_t fc = ((uint32_t)de->first_cluster_hi << 16)
                                    | (uint32_t)de->first_cluster_lo;

                        if (*component == '\0') {
                            /* This is the final component — fill info */
                            info->size = de->file_size;
                            info->attributes = de->attr;
                            info->type = (de->attr & FAT32_ATTR_DIRECTORY)
                                       ? VFS_DIRECTORY : VFS_FILE;
                            info->create_date = de->create_date;
                            info->create_time = de->create_time;
                            info->modify_date = de->modify_date;
                            info->modify_time = de->modify_time;
                            info->first_cluster = fc;
                            kfree(cluster_buf);
                            return 0;
                        }

                        /* Directory — descend */
                        if (!(de->attr & FAT32_ATTR_DIRECTORY))
                            goto stat_not_found;

                        cluster = fc;
                        found = 1;
                        goto next_component;
                    }
                }

                cur_cluster = fat32_next_cluster(cur_cluster);
            }
        }

next_component:
        if (!found && *component != '\0')
            goto stat_not_found;
    }

stat_not_found:
    kfree(cluster_buf);
    return -1;
}

/* ---- Public API ---- */

int fat32_init(const struct blkdev *dev)
{
    uint32_t root_dir_sectors;

    if (!dev) {
        printk("[FAIL] FAT32: null block device\n");
        return -1;
    }

    fat32_dev = dev;
    dir_file_count = 0;

    /* Read the boot sector (BPB) — LBA 0 relative to the sub-blkdev */
    if (fat32_read_sector(0, sector_buf) != 0) {
        printk("[FAIL] FAT32: cannot read boot sector\n");
        return -1;
    }

    /* Verify boot signature */
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) {
        printk("[FAIL] FAT32: invalid boot signature\n");
        return -1;
    }

    /* Parse BPB fields */
    bpb.bytes_per_sector    = *(uint16_t *)&sector_buf[11];
    bpb.sectors_per_cluster = sector_buf[13];
    bpb.reserved_sectors    = *(uint16_t *)&sector_buf[14];
    bpb.num_fats            = sector_buf[16];
    bpb.fat_size_sectors    = *(uint32_t *)&sector_buf[36];
    bpb.root_cluster        = *(uint32_t *)&sector_buf[44];

    /* Total sectors: use the 32-bit field */
    bpb.total_sectors = *(uint16_t *)&sector_buf[19];
    if (bpb.total_sectors == 0)
        bpb.total_sectors = *(uint32_t *)&sector_buf[32];

    /* Compute layout */
    bpb.first_fat_sector  = bpb.reserved_sectors;
    root_dir_sectors      = 0;   /* FAT32 has no fixed root dir */
    bpb.first_data_sector = bpb.reserved_sectors
                          + (bpb.num_fats * bpb.fat_size_sectors)
                          + root_dir_sectors;

    /* Validate */
    if (bpb.bytes_per_sector != 512) {
        printk("[FAIL] FAT32: unsupported sector size %u\n",
               (uint64_t)bpb.bytes_per_sector);
        return -1;
    }

    /* Set up root node */
    fat32_strcpy(root_file.node.name, "A:\\", VFS_MAX_NAME);
    root_file.node.type = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root_file.node.inode = bpb.root_cluster;
    root_file.node.size = 0;
    root_file.node.ops = &fat32_dir_ops;
    root_file.node.fs_data = &root_file;
    root_file.node.parent = (struct vfs_node *)0;
    root_file.first_cluster = bpb.root_cluster;
    root_file.file_size = 0;

    {
        uint64_t vol_mb = (uint64_t)bpb.total_sectors * 512 / (1024 * 1024);
        printk("[OK] FAT32: %u MiB, %u sectors/cluster, root cluster %u\n",
               vol_mb,
               (uint64_t)bpb.sectors_per_cluster,
               (uint64_t)bpb.root_cluster);
    }

    return 0;
}

struct vfs_fs_driver *fat32_get_driver(void)
{
    return &fat32_driver;
}

struct vfs_node *fat32_get_root(void)
{
    return &root_file.node;
}
