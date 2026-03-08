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

/* ---- Sector write helpers ---- */

/* Forward declarations for functions defined later */
static uint32_t cluster_to_sector(uint32_t cluster);
static uint32_t fat32_next_cluster(uint32_t cluster);

/* Write a single sector */
static int fat32_write_sector(uint32_t sector, const void *buf)
{
    return blkdev_write(fat32_dev, sector, 1, buf);
}

/* Write multiple contiguous sectors */
static int fat32_write_sectors_multi(uint32_t sector, uint32_t count,
                                     const void *buf)
{
    return blkdev_write(fat32_dev, sector, count, buf);
}

/* ---- FAT entry manipulation ---- */

/* Read a FAT entry for a given cluster */
static uint32_t fat32_get_fat_entry(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = bpb.first_fat_sector + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;
    uint32_t val;

    if (fat32_read_sector(fat_sector, sector_buf) != 0)
        return FAT32_EOC;

    val = *(uint32_t *)&sector_buf[entry_offset];
    return val & 0x0FFFFFFF;
}

/* Write a FAT entry for a given cluster.
 * Writes to both FAT copies for consistency. */
static int fat32_set_fat_entry(uint32_t cluster, uint32_t value)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t entry_offset = fat_offset % 512;
    uint32_t fat_relative_sector = fat_offset / 512;
    uint32_t existing;
    uint32_t fi;

    for (fi = 0; fi < bpb.num_fats; fi++) {
        uint32_t fat_sector = bpb.first_fat_sector
                            + fi * bpb.fat_size_sectors
                            + fat_relative_sector;

        if (fat32_read_sector(fat_sector, sector_buf) != 0)
            return -1;

        /* Preserve upper 4 bits of existing entry */
        existing = *(uint32_t *)&sector_buf[entry_offset];
        existing = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
        *(uint32_t *)&sector_buf[entry_offset] = existing;

        if (fat32_write_sector(fat_sector, sector_buf) != 0)
            return -1;
    }

    return 0;
}

/* ---- Free cluster allocation ---- */

/* Find a free cluster (FAT entry == 0x00000000).
 * Returns cluster number or 0 on failure.
 * total_data_clusters = total_sectors minus reserved/FAT/root overhead. */
static uint32_t fat32_alloc_cluster(void)
{
    uint32_t total_data_clusters;
    uint32_t cluster;

    total_data_clusters = (bpb.total_sectors - bpb.first_data_sector)
                        / bpb.sectors_per_cluster;

    /* Scan from cluster 2 (first valid data cluster) */
    for (cluster = 2; cluster < total_data_clusters + 2; cluster++) {
        if (fat32_get_fat_entry(cluster) == FAT32_FREE) {
            /* Mark as end-of-chain (caller will link if needed) */
            if (fat32_set_fat_entry(cluster, 0x0FFFFFFF) != 0)
                return 0;
            return cluster;
        }
    }

    return 0;  /* Disk full */
}

/* Free a cluster chain starting from 'cluster' */
static void fat32_free_chain(uint32_t cluster)
{
    while (cluster >= 2 && cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t next = fat32_get_fat_entry(cluster);
        fat32_set_fat_entry(cluster, FAT32_FREE);
        cluster = next;
    }
}

/* ---- Zero a cluster (clear data area) ---- */
static int fat32_zero_cluster(uint32_t cluster)
{
    uint32_t sector = cluster_to_sector(cluster);
    uint8_t zero[512];
    uint32_t si;
    uint32_t k;

    for (k = 0; k < 512; k++)
        zero[k] = 0;

    for (si = 0; si < bpb.sectors_per_cluster; si++) {
        if (fat32_write_sector(sector + si, zero) != 0)
            return -1;
    }
    return 0;
}

/* ---- 8.3 short name generation ---- */

/* Generate an 8.3 short name from a long name.
 * Converts to uppercase, truncates name to 6 chars + "~1" if needed. */
static void fat32_make_short_name(const char *name, uint8_t *short_name)
{
    int i, j;
    int has_ext = 0;
    int name_len = 0;
    const char *dot = (const char *)0;

    /* Fill with spaces */
    for (i = 0; i < 11; i++)
        short_name[i] = ' ';

    /* Find last dot for extension */
    for (i = 0; name[i]; i++) {
        if (name[i] == '.')
            dot = &name[i];
        name_len++;
    }

    /* Copy name part (up to 8 chars, uppercase) */
    j = 0;
    for (i = 0; name[i] && j < 8; i++) {
        if (&name[i] == dot) break;
        if (name[i] == ' ' || name[i] == '.') continue;
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[j++] = (uint8_t)c;
    }

    /* If name was truncated, add ~1 */
    if (dot && (dot - name) > 8) {
        short_name[6] = '~';
        short_name[7] = '1';
    } else if (!dot && name_len > 8) {
        short_name[6] = '~';
        short_name[7] = '1';
    }

    /* Copy extension (up to 3 chars, uppercase) */
    if (dot) {
        has_ext = 1;
        dot++;  /* skip the dot */
        for (i = 0; dot[i] && i < 3; i++) {
            char c = dot[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            short_name[8 + i] = (uint8_t)c;
        }
    }

    (void)has_ext;
}

/* ---- Directory entry manipulation ---- */

/* Find an empty 32-byte slot in a directory cluster chain.
 * Returns the LBA sector and offset within that sector.
 * If no space, extends the directory by allocating a new cluster. */
static int fat32_find_free_dir_slot(uint32_t dir_cluster,
                                     uint32_t *out_sector,
                                     uint32_t *out_offset,
                                     uint32_t *out_cluster)
{
    uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t cur_cluster = dir_cluster;
    uint32_t prev_cluster = 0;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }

        for (i = 0; i < bytes_per_cluster; i += 32) {
            uint8_t first_byte = cluster_buf[i];
            if (first_byte == 0x00 || first_byte == 0xE5) {
                /* Found a free slot */
                *out_sector = sector + (i / 512);
                *out_offset = i % 512;
                *out_cluster = cur_cluster;
                kfree(cluster_buf);
                return 0;
            }
        }

        prev_cluster = cur_cluster;
        cur_cluster = fat32_get_fat_entry(cur_cluster);
    }

    /* No free slot — extend directory with a new cluster */
    {
        uint32_t new_cluster = fat32_alloc_cluster();
        if (new_cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }

        /* Link new cluster to the chain */
        fat32_set_fat_entry(prev_cluster, new_cluster);
        fat32_zero_cluster(new_cluster);

        *out_sector = cluster_to_sector(new_cluster);
        *out_offset = 0;
        *out_cluster = new_cluster;
    }

    kfree(cluster_buf);
    return 0;
}

/* Write a directory entry at a specific sector + offset */
static int fat32_write_dir_entry(uint32_t sector, uint32_t offset,
                                  const struct fat32_dir_entry *entry)
{
    uint8_t buf[512];
    uint32_t i;
    const uint8_t *src = (const uint8_t *)entry;

    if (fat32_read_sector(sector, buf) != 0)
        return -1;

    for (i = 0; i < 32; i++)
        buf[offset + i] = src[i];

    return fat32_write_sector(sector, buf);
}

/* ---- Public write API ---- */

int fat32_create_file(uint32_t dir_cluster, const char *name)
{
    struct fat32_dir_entry de;
    uint32_t slot_sector, slot_offset, slot_cluster;
    uint32_t first_cluster;
    int i;

    if (!name || !name[0])
        return -1;

    /* Allocate first data cluster */
    first_cluster = fat32_alloc_cluster();
    if (first_cluster == 0)
        return -1;

    fat32_zero_cluster(first_cluster);

    /* Find a free directory entry slot */
    if (fat32_find_free_dir_slot(dir_cluster, &slot_sector,
                                  &slot_offset, &slot_cluster) != 0) {
        fat32_free_chain(first_cluster);
        return -1;
    }

    /* Build directory entry */
    for (i = 0; i < 32; i++)
        ((uint8_t *)&de)[i] = 0;

    fat32_make_short_name(name, de.name);
    de.attr = FAT32_ATTR_ARCHIVE;
    de.first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de.file_size = 0;

    /* Write the entry */
    if (fat32_write_dir_entry(slot_sector, slot_offset, &de) != 0) {
        fat32_free_chain(first_cluster);
        return -1;
    }

    /* Invalidate cached directory listing */
    dir_file_count = 0;

    return 0;
}

int fat32_create_dir(uint32_t parent_cluster, const char *name)
{
    struct fat32_dir_entry de;
    uint32_t slot_sector, slot_offset, slot_cluster;
    uint32_t new_cluster;
    uint8_t dot_buf[512];
    int i;

    if (!name || !name[0])
        return -1;

    /* Allocate cluster for the new directory */
    new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0)
        return -1;

    fat32_zero_cluster(new_cluster);

    /* Create "." entry (points to self) */
    for (i = 0; i < 512; i++)
        dot_buf[i] = 0;

    /* "." entry at offset 0 */
    dot_buf[0] = '.';
    for (i = 1; i < 11; i++) dot_buf[i] = ' ';
    dot_buf[11] = FAT32_ATTR_DIRECTORY;
    *(uint16_t *)&dot_buf[20] = (uint16_t)(new_cluster >> 16);
    *(uint16_t *)&dot_buf[26] = (uint16_t)(new_cluster & 0xFFFF);

    /* ".." entry at offset 32 */
    dot_buf[32] = '.';
    dot_buf[33] = '.';
    for (i = 34; i < 43; i++) dot_buf[i] = ' ';
    dot_buf[43] = FAT32_ATTR_DIRECTORY;
    *(uint16_t *)&dot_buf[52] = (uint16_t)(parent_cluster >> 16);
    *(uint16_t *)&dot_buf[58] = (uint16_t)(parent_cluster & 0xFFFF);

    if (fat32_write_sector(cluster_to_sector(new_cluster), dot_buf) != 0) {
        fat32_free_chain(new_cluster);
        return -1;
    }

    /* Add entry in parent directory */
    if (fat32_find_free_dir_slot(parent_cluster, &slot_sector,
                                  &slot_offset, &slot_cluster) != 0) {
        fat32_free_chain(new_cluster);
        return -1;
    }

    for (i = 0; i < 32; i++)
        ((uint8_t *)&de)[i] = 0;

    fat32_make_short_name(name, de.name);
    de.attr = FAT32_ATTR_DIRECTORY;
    de.first_cluster_hi = (uint16_t)(new_cluster >> 16);
    de.first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    de.file_size = 0;

    if (fat32_write_dir_entry(slot_sector, slot_offset, &de) != 0) {
        fat32_free_chain(new_cluster);
        return -1;
    }

    dir_file_count = 0;
    return 0;
}

int fat32_write_file(uint32_t dir_cluster, const char *name,
                     const void *data, uint32_t size)
{
    uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t clusters_needed;
    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;
    uint32_t bytes_written = 0;
    uint32_t ci;

    /* Calculate clusters needed */
    clusters_needed = (size + bytes_per_cluster - 1) / bytes_per_cluster;
    if (clusters_needed == 0)
        clusters_needed = 1;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    /* First, find existing file and free its chain (overwrite mode) */
    {
        uint32_t cur_cluster = dir_cluster;
        while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
            uint32_t sector = cluster_to_sector(cur_cluster);
            uint32_t i;
            uint8_t short_name[11];

            if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                         cluster_buf) != 0)
                break;

            fat32_make_short_name(name, short_name);

            for (i = 0; i < bytes_per_cluster; i += 32) {
                struct fat32_dir_entry *de =
                    (struct fat32_dir_entry *)&cluster_buf[i];
                int j, match;

                if (de->name[0] == 0x00) goto not_found;
                if (de->name[0] == 0xE5) continue;
                if (de->attr == FAT32_ATTR_LFN) continue;
                if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

                match = 1;
                for (j = 0; j < 11; j++) {
                    if (de->name[j] != short_name[j]) {
                        match = 0;
                        break;
                    }
                }

                if (match) {
                    /* Free existing data chain */
                    uint32_t old_fc = ((uint32_t)de->first_cluster_hi << 16)
                                    | (uint32_t)de->first_cluster_lo;
                    if (old_fc >= 2)
                        fat32_free_chain(old_fc);
                    /* Mark entry as deleted — we'll recreate below */
                    cluster_buf[i] = 0xE5;
                    fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                              cluster_buf);
                    goto not_found;
                }
            }

            cur_cluster = fat32_get_fat_entry(cur_cluster);
        }
    }

not_found:
    /* Allocate cluster chain for new data */
    for (ci = 0; ci < clusters_needed; ci++) {
        uint32_t new_cluster = fat32_alloc_cluster();
        if (new_cluster == 0) {
            if (first_cluster)
                fat32_free_chain(first_cluster);
            kfree(cluster_buf);
            return -1;
        }

        if (ci == 0)
            first_cluster = new_cluster;
        else
            fat32_set_fat_entry(prev_cluster, new_cluster);

        prev_cluster = new_cluster;
    }

    /* Write data to clusters */
    {
        uint32_t write_cluster = first_cluster;
        const uint8_t *src = (const uint8_t *)data;

        for (ci = 0; ci < clusters_needed && write_cluster >= 2
             && write_cluster < FAT32_EOC; ci++) {
            uint32_t sector = cluster_to_sector(write_cluster);
            uint32_t chunk = bytes_per_cluster;
            uint32_t k;

            if (chunk > size - bytes_written)
                chunk = size - bytes_written;

            /* Clear buffer, copy data */
            for (k = 0; k < bytes_per_cluster; k++)
                cluster_buf[k] = 0;
            for (k = 0; k < chunk; k++)
                cluster_buf[k] = src[bytes_written + k];

            if (fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                          cluster_buf) != 0) {
                fat32_free_chain(first_cluster);
                kfree(cluster_buf);
                return -1;
            }

            bytes_written += chunk;
            write_cluster = fat32_get_fat_entry(write_cluster);
        }
    }

    kfree(cluster_buf);

    /* Create directory entry */
    {
        struct fat32_dir_entry de;
        uint32_t slot_sector, slot_offset, slot_cluster;
        int i;

        if (fat32_find_free_dir_slot(dir_cluster, &slot_sector,
                                      &slot_offset, &slot_cluster) != 0) {
            fat32_free_chain(first_cluster);
            return -1;
        }

        for (i = 0; i < 32; i++)
            ((uint8_t *)&de)[i] = 0;

        fat32_make_short_name(name, de.name);
        de.attr = FAT32_ATTR_ARCHIVE;
        de.first_cluster_hi = (uint16_t)(first_cluster >> 16);
        de.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
        de.file_size = size;

        if (fat32_write_dir_entry(slot_sector, slot_offset, &de) != 0) {
            fat32_free_chain(first_cluster);
            return -1;
        }
    }

    dir_file_count = 0;
    return 0;
}

int fat32_delete_file(uint32_t dir_cluster, const char *name)
{
    uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(name, short_name);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) goto del_not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != short_name[j]) { match = 0; break; }
            }

            if (match) {
                uint32_t fc = ((uint32_t)de->first_cluster_hi << 16)
                            | (uint32_t)de->first_cluster_lo;

                /* Free the cluster chain */
                if (fc >= 2)
                    fat32_free_chain(fc);

                /* Mark directory entry as deleted */
                cluster_buf[i] = 0xE5;
                fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                          cluster_buf);

                dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }

        cur_cluster = fat32_get_fat_entry(cur_cluster);
    }

del_not_found:
    kfree(cluster_buf);
    return -1;
}

int fat32_rename(uint32_t dir_cluster,
                 const char *old_name, const char *new_name)
{
    uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t old_short[11];
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(old_name, old_short);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) goto ren_not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != old_short[j]) { match = 0; break; }
            }

            if (match) {
                fat32_make_short_name(new_name, de->name);
                fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                          cluster_buf);
                dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }

        cur_cluster = fat32_get_fat_entry(cur_cluster);
    }

ren_not_found:
    kfree(cluster_buf);
    return -1;
}

int fat32_format(const struct blkdev *dev, const char *label)
{
    uint8_t boot[512];
    uint8_t fat_sec[512];
    uint32_t total_sectors;
    uint32_t fat_size;
    uint32_t data_start;
    uint32_t total_clusters;
    uint32_t si;
    uint32_t k;
    uint8_t spc;  /* sectors per cluster */

    if (!dev)
        return -1;

    total_sectors = (uint32_t)dev->sector_count;
    if (total_sectors < 65536) {
        printk("[FAIL] FAT32: volume too small (%u sectors)\n",
               (uint64_t)total_sectors);
        return -1;
    }

    /* Choose sectors per cluster based on volume size */
    if (total_sectors < 532480)       spc = 1;   /* < 260 MiB */
    else if (total_sectors < 16777216) spc = 8;   /* < 8 GiB */
    else if (total_sectors < 33554432) spc = 16;  /* < 16 GiB */
    else                               spc = 32;  /* >= 16 GiB */

    /* Calculate FAT size (sectors per FAT) */
    {
        uint32_t tmp = total_sectors - 32;   /* minus reserved */
        fat_size = (tmp + 2 * spc + 127 * spc)
                 / (128 * spc + 2);          /* 128 entries per sector */
        fat_size++;  /* round up */
    }
    data_start = 32 + 2 * fat_size;
    total_clusters = (total_sectors - data_start) / spc;

    /* ---- Write boot sector (BPB) ---- */
    for (k = 0; k < 512; k++)
        boot[k] = 0;

    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;  /* Jump */
    /* OEM name */
    boot[3]='I'; boot[4]='M'; boot[5]='P'; boot[6]='O';
    boot[7]='S'; boot[8]='S'; boot[9]='O'; boot[10]='S';

    *(uint16_t *)&boot[11] = 512;              /* Bytes per sector */
    boot[13] = spc;                            /* Sectors per cluster */
    *(uint16_t *)&boot[14] = 32;               /* Reserved sectors */
    boot[16] = 2;                              /* Number of FATs */
    *(uint16_t *)&boot[17] = 0;                /* Root entry count */
    *(uint16_t *)&boot[19] = 0;                /* Total sectors 16 */
    boot[21] = 0xF8;                           /* Media type */
    *(uint16_t *)&boot[22] = 0;                /* Sectors per FAT 16 */
    *(uint16_t *)&boot[24] = 63;               /* Sectors per track */
    *(uint16_t *)&boot[26] = 255;              /* Number of heads */
    *(uint32_t *)&boot[32] = total_sectors;    /* Total sectors 32 */
    *(uint32_t *)&boot[36] = fat_size;         /* Sectors per FAT 32 */
    *(uint32_t *)&boot[44] = 2;                /* Root cluster */
    *(uint16_t *)&boot[48] = 1;                /* FSInfo sector */
    *(uint16_t *)&boot[50] = 6;                /* Backup boot sector */

    /* FAT32 extended fields */
    boot[66] = 0x29;                           /* Extended boot sig */
    *(uint32_t *)&boot[67] = 0x12345678;       /* Volume serial */
    /* Volume label (11 bytes, space-padded) */
    {
        int li;
        for (li = 0; li < 11; li++)
            boot[71 + li] = ' ';
        if (label) {
            for (li = 0; li < 11 && label[li]; li++) {
                char c = label[li];
                if (c >= 'a' && c <= 'z') c -= 32;
                boot[71 + li] = (uint8_t)c;
            }
        }
    }
    /* FS type string */
    boot[82]='F'; boot[83]='A'; boot[84]='T'; boot[85]='3';
    boot[86]='2'; boot[87]=' '; boot[88]=' '; boot[89]=' ';

    boot[510] = 0x55;
    boot[511] = 0xAA;

    if (blkdev_write(dev, 0, 1, boot) != 0)
        return -1;

    /* Write backup boot sector at sector 6 */
    if (blkdev_write(dev, 6, 1, boot) != 0)
        return -1;

    /* ---- Write FSInfo sector at sector 1 ---- */
    for (k = 0; k < 512; k++)
        fat_sec[k] = 0;

    *(uint32_t *)&fat_sec[0] = 0x41615252;      /* FSInfo signature */
    *(uint32_t *)&fat_sec[484] = 0x61417272;     /* Second signature */
    *(uint32_t *)&fat_sec[488] = total_clusters - 1;  /* Free clusters */
    *(uint32_t *)&fat_sec[492] = 3;              /* Next free cluster */
    fat_sec[510] = 0x55;
    fat_sec[511] = 0xAA;

    if (blkdev_write(dev, 1, 1, fat_sec) != 0)
        return -1;

    /* ---- Write FAT tables (both copies) ---- */
    for (k = 0; k < 512; k++)
        fat_sec[k] = 0;

    {
        uint32_t fi;
        for (fi = 0; fi < 2; fi++) {
            uint32_t fat_start = 32 + fi * fat_size;

            /* Write all FAT sectors as zero first */
            for (si = 0; si < fat_size; si++) {
                if (blkdev_write(dev, fat_start + si, 1, fat_sec) != 0)
                    return -1;
            }

            /* Write first FAT sector with special entries */
            /* Entry 0: media type + 0x0FFFFF00 */
            /* Entry 1: end-of-chain marker */
            /* Entry 2: end-of-chain (root directory cluster) */
            {
                uint8_t first_fat[512];
                for (k = 0; k < 512; k++)
                    first_fat[k] = 0;

                *(uint32_t *)&first_fat[0] = 0x0FFFFFF8;  /* Entry 0 */
                *(uint32_t *)&first_fat[4] = 0x0FFFFFFF;  /* Entry 1 */
                *(uint32_t *)&first_fat[8] = 0x0FFFFFFF;  /* Entry 2 (root) */

                if (blkdev_write(dev, fat_start, 1, first_fat) != 0)
                    return -1;
            }
        }
    }

    /* ---- Zero root directory cluster ---- */
    {
        uint32_t root_sector = data_start;
        for (si = 0; si < (uint32_t)spc; si++) {
            for (k = 0; k < 512; k++)
                fat_sec[k] = 0;
            if (blkdev_write(dev, root_sector + si, 1, fat_sec) != 0)
                return -1;
        }

        /* Write volume label entry in root directory */
        if (label && label[0]) {
            uint8_t vol_ent[512];
            int li;
            for (k = 0; k < 512; k++)
                vol_ent[k] = 0;

            for (li = 0; li < 11; li++)
                vol_ent[li] = ' ';
            for (li = 0; li < 11 && label[li]; li++) {
                char c = label[li];
                if (c >= 'a' && c <= 'z') c -= 32;
                vol_ent[li] = (uint8_t)c;
            }
            vol_ent[11] = FAT32_ATTR_VOLUME_ID;

            if (blkdev_write(dev, root_sector, 1, vol_ent) != 0)
                return -1;
        }
    }

    printk("[OK] FAT32: formatted %u MiB (%u clusters, %u sec/cluster)\n",
           (uint64_t)total_sectors * 512 / (1024 * 1024),
           (uint64_t)total_clusters,
           (uint64_t)spc);

    return 0;
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

/* VFS-compatible file write: write data at offset into the file.
 * For simplicity, this always rewrites the entire file (FAT32 doesn't
 * support efficient partial writes without a lot more complexity). */
static int fat32_file_write_vfs(struct vfs_node *node, uint32_t offset,
                                uint32_t size, const uint8_t *buffer)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    (void)offset;  /* FAT32 write is always full-file overwrite */

    if (!f || !buffer || size == 0)
        return -1;

    /* Use the parent directory cluster to locate this file.
     * The root node stores the dir cluster; subdirectories use their own. */
    return fat32_write_file(bpb.root_cluster, node->name, buffer, size);
}

static struct vfs_ops fat32_file_ops = {
    .open    = fat32_file_open,
    .close   = fat32_file_close,
    .read    = fat32_file_read,
    .write   = fat32_file_write_vfs,
    .readdir = (void *)0,
    .finddir = (void *)0,
    .create  = (void *)0,
    .unlink  = (void *)0,
};

/* ---- VFS operations for FAT32 directories ---- */

/* Forward declaration — needed by fat32_finddir */
static struct vfs_ops fat32_dir_ops;

static struct vfs_dirent *fat32_readdir(struct vfs_node *node, uint32_t index)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t dir_cluster;

    /* Determine which directory to list */
    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    /* Lazily load directory entries */
    if (dir_file_count == 0)
        fat32_read_dir(dir_cluster);

    if (index >= dir_file_count)
        return (struct vfs_dirent *)0;

    fat32_strcpy(fat32_dirent.name, dir_files[index].node.name, VFS_MAX_NAME);
    fat32_dirent.inode = dir_files[index].node.inode;
    fat32_dirent.type = dir_files[index].node.type;

    return &fat32_dirent;
}

static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t dir_cluster;
    uint32_t i;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    if (dir_file_count == 0)
        fat32_read_dir(dir_cluster);

    for (i = 0; i < dir_file_count; i++) {
        if (fat32_strcasecmp(dir_files[i].node.name, name)) {
            /* Set directory ops for subdirectories, file ops for files */
            if (dir_files[i].node.type & VFS_DIRECTORY)
                dir_files[i].node.ops = &fat32_dir_ops;
            else
                dir_files[i].node.ops = &fat32_file_ops;
            dir_files[i].node.fs_data = &dir_files[i];
            return &dir_files[i].node;
        }
    }

    return (struct vfs_node *)0;
}

/* VFS-compatible create: called by vfs_create() */
static int fat32_vfs_create(struct vfs_node *parent, const char *name,
                            uint8_t type)
{
    struct fat32_file *f = (struct fat32_file *)parent->fs_data;
    uint32_t dir_cluster;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    if (type & VFS_DIRECTORY)
        return fat32_create_dir(dir_cluster, name);
    else
        return fat32_create_file(dir_cluster, name);
}

/* VFS-compatible unlink: called by vfs_unlink() */
static int fat32_vfs_unlink(struct vfs_node *parent, const char *name)
{
    struct fat32_file *f = (struct fat32_file *)parent->fs_data;
    uint32_t dir_cluster;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    return fat32_delete_file(dir_cluster, name);
}

static struct vfs_ops fat32_dir_ops = {
    .open    = fat32_file_open,
    .close   = (void *)0,
    .read    = (void *)0,
    .write   = (void *)0,
    .readdir = fat32_readdir,
    .finddir = fat32_finddir,
    .create  = fat32_vfs_create,
    .unlink  = fat32_vfs_unlink,
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
