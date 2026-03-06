/* ============================================================================
 * fat32.c — FAT32 Read-Only Filesystem Driver
 *
 * Reads a FAT32 volume from the ATA disk, parses the BPB, reads the FAT,
 * follows cluster chains, and provides VFS operations for directory listing
 * and file reading.
 *
 * Key concepts:
 *   BPB (sector 0) → describes layout
 *   FAT region → maps cluster → next cluster chain
 *   Data region → actual file/directory data
 *   Directory entries → 32 bytes each (8.3 short names)
 * ============================================================================ */

#include "fat32.h"
#include "ata.h"
#include "heap.h"
#include "printk.h"

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

/* Maximum directory entries we'll track */
#define FAT32_MAX_DIR_ENTRIES 128
#define FAT32_MAX_NAME        64

/* In-memory file/directory node */
struct fat32_file {
    struct vfs_node node;
    uint32_t first_cluster;
    uint32_t file_size;
};

/* Static storage */
static struct fat32_bpb bpb;
static uint32_t part_lba;               /* partition start LBA */
static struct fat32_file root_file;
static struct fat32_file dir_files[FAT32_MAX_DIR_ENTRIES];
static uint32_t dir_file_count;
static struct vfs_dirent fat32_dirent;

/* Sector buffer */
static uint8_t sector_buf[512];

/* --- Internal: read a sector relative to partition start --- */
static int fat32_read_sector(uint32_t sector, void *buf)
{
    return ata_read_sectors(part_lba + sector, 1, buf);
}

/* --- Internal: read multiple sectors --- */
static int fat32_read_sectors_multi(uint32_t sector, uint32_t count, void *buf)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)buf;

    for (i = 0; i < count; i++) {
        if (ata_read_sectors(part_lba + sector + i, 1, p + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* --- Internal: get next cluster from FAT --- */
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

/* --- Internal: get first sector of a cluster --- */
static uint32_t cluster_to_sector(uint32_t cluster)
{
    return bpb.first_data_sector + (cluster - 2) * bpb.sectors_per_cluster;
}

/* --- Internal: convert 8.3 name to readable string --- */
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

/* --- Internal: string copy --- */
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

/* --- Internal: read directory entries from a cluster chain --- */
static void fat32_read_dir(uint32_t cluster)
{
    uint32_t bytes_per_cluster;
    uint8_t *cluster_buf;
    uint32_t i;

    dir_file_count = 0;
    bytes_per_cluster = bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return;

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
            if (de->name[0] == 0xE5)
                continue;

            /* Skip LFN entries and volume ID */
            if (de->attr == FAT32_ATTR_LFN)
                continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID)
                continue;

            /* Skip . and .. */
            if (de->name[0] == '.')
                continue;

            fat32_short_name_to_str(de->name, name);

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

/* --- VFS operations for FAT32 files --- */

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
        uint32_t i;

        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        to_read = bytes_per_cluster - cluster_offset;
        if (to_read > size - bytes_read)
            to_read = size - bytes_read;

        for (i = 0; i < to_read; i++)
            buffer[bytes_read + i] = cluster_buf[cluster_offset + i];

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

/* --- VFS operations for FAT32 directories --- */

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

/* --- Public API --- */

int fat32_init(uint32_t partition_lba)
{
    uint32_t root_dir_sectors;

    part_lba = partition_lba;
    dir_file_count = 0;

    /* Read the boot sector (BPB) */
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
