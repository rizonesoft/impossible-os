/* ============================================================================
 * fat32_core.c — String helpers, sector cache, sector I/O, FAT operations
 *
 * Contains the low-level building blocks used by all other FAT32 modules:
 * string utilities, LRU sector cache, block device I/O wrappers, FAT entry
 * manipulation, cluster allocation/free, and FSInfo sector management.
 * ============================================================================ */

#include "fat32_internal.h"

/* ---- Global state definitions ---- */

struct fat32_bpb bpb;
const struct blkdev *fat32_dev;
struct fat32_file root_file;
struct fat32_file dir_files[FAT32_MAX_DIR_ENTRIES];
uint32_t dir_file_count;
struct vfs_dirent fat32_dirent;
uint8_t sector_buf[512];

/* Sector cache */
scache_entry_t scache[SCACHE_SLOTS];
uint32_t scache_access = 0;

/* FSInfo state */
uint32_t fsinfo_sector;
uint32_t fsinfo_free_count;
uint32_t fsinfo_next_free;
int      fsinfo_valid;
int      fsinfo_dirty;

/* ---- String helpers ---- */

void fat32_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Case-insensitive compare — returns 1 on match */
int fat32_strcasecmp(const char *a, const char *b)
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

/* Convert 8.3 short name to readable string */
void fat32_short_name_to_str(const uint8_t *raw, char *out)
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

/* Extract characters from a single LFN entry into a buffer.
 * Each LFN entry contributes 13 UTF-16LE characters.
 * seq_index is 0-based (0 = chars 0-12, 1 = chars 13-25, etc.) */
void lfn_extract_chars(const struct fat32_lfn_entry *lfn,
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

/* ---- Sector cache (LRU with dirty tracking) ----
 * Caches 64 sectors (~32 KB) to avoid repeated disk I/O.  The main win is
 * FAT table reads: fat32_get_fat_entry() is called once per cluster in a
 * chain walk, and adjacent clusters usually share the same FAT sector.
 * Without the cache, each call does a blkdev_read().  With the cache, only
 * the first read hits disk; the rest are served from SRAM. */

/* Find a cached entry by sector LBA, or NULL if not cached */
static scache_entry_t *scache_lookup(uint32_t sector)
{
    int i;
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (scache[i].valid && scache[i].sector == sector) {
            scache[i].last_access = ++scache_access;
            return &scache[i];
        }
    }
    return (scache_entry_t *)0;
}

/* Find LRU slot for eviction.  If dirty, flush it first. */
static scache_entry_t *scache_evict(void)
{
    int i;
    int lru_idx = 0;
    uint32_t lru_val = 0xFFFFFFFF;

    /* Prefer an empty slot */
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (!scache[i].valid)
            return &scache[i];
    }

    /* Find least-recently-used */
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (scache[i].last_access < lru_val) {
            lru_val = scache[i].last_access;
            lru_idx = i;
        }
    }

    /* Flush dirty entry before evicting */
    if (scache[lru_idx].dirty) {
        blkdev_write(fat32_dev, scache[lru_idx].sector, 1,
                     scache[lru_idx].data);
        scache[lru_idx].dirty = 0;
    }

    scache[lru_idx].valid = 0;
    return &scache[lru_idx];
}

/* Flush all dirty cache entries to disk */
void scache_flush(void)
{
    int i;
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (scache[i].valid && scache[i].dirty) {
            blkdev_write(fat32_dev, scache[i].sector, 1,
                         scache[i].data);
            scache[i].dirty = 0;
        }
    }
}

/* Invalidate entire cache (used on unmount or format) */
void scache_invalidate(void)
{
    int i;
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (scache[i].valid && scache[i].dirty) {
            blkdev_write(fat32_dev, scache[i].sector, 1,
                         scache[i].data);
        }
        scache[i].valid = 0;
        scache[i].dirty = 0;
    }
    scache_access = 0;
}

/* ---- Block device I/O (cached) ---- */

/* Read a single sector — served from cache if available */
int fat32_read_sector(uint32_t sector, void *buf)
{
    scache_entry_t *e = scache_lookup(sector);
    uint8_t *dst = (uint8_t *)buf;
    int i;

    if (e) {
        for (i = 0; i < 512; i++)
            dst[i] = e->data[i];
        return 0;
    }

    /* Cache miss — read from disk and populate cache */
    e = scache_evict();
    if (blkdev_read(fat32_dev, sector, 1, e->data) != 0)
        return -1;

    e->sector      = sector;
    e->valid       = 1;
    e->dirty       = 0;
    e->last_access = ++scache_access;

    for (i = 0; i < 512; i++)
        dst[i] = e->data[i];

    return 0;
}

/* Read multiple contiguous sectors (bypasses cache — used for data clusters) */
int fat32_read_sectors_multi(uint32_t sector, uint32_t count, void *buf)
{
    return blkdev_read(fat32_dev, sector, count, buf);
}

/* Write a single sector — writes to cache with dirty flag */
int fat32_write_sector(uint32_t sector, const void *buf)
{
    scache_entry_t *e = scache_lookup(sector);
    const uint8_t *src = (const uint8_t *)buf;
    int i;

    if (!e) {
        e = scache_evict();
        e->sector = sector;
        e->valid  = 1;
    }

    for (i = 0; i < 512; i++)
        e->data[i] = src[i];

    e->dirty       = 1;
    e->last_access = ++scache_access;

    return 0;
}

/* Write multiple contiguous sectors */
int fat32_write_sectors_multi(uint32_t sector, uint32_t count,
                               const void *buf)
{
    return blkdev_write(fat32_dev, sector, count, buf);
}

/* ---- FAT entry manipulation ---- */

/* Read a FAT entry for a given cluster */
uint32_t fat32_get_fat_entry(uint32_t cluster)
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
int fat32_set_fat_entry(uint32_t cluster, uint32_t value)
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

/* Get next cluster from FAT */
uint32_t fat32_next_cluster(uint32_t cluster)
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
uint32_t cluster_to_sector(uint32_t cluster)
{
    return bpb.first_data_sector + (cluster - 2) * bpb.sectors_per_cluster;
}

/* ---- FSInfo sector I/O ---- */

/* Flush in-memory FSInfo state to disk (both copies if num_fats > 1). */
void fat32_fsinfo_flush(void)
{
    uint8_t buf[512];

    if (!fsinfo_valid || !fsinfo_dirty)
        return;

    if (fat32_read_sector(fsinfo_sector, buf) != 0)
        return;

    /* Verify lead signature before writing */
    if (*(uint32_t *)&buf[0] != FSINFO_LEAD_SIG)
        return;

    /* Update the two mutable fields */
    *(uint32_t *)&buf[488] = fsinfo_free_count;
    *(uint32_t *)&buf[492] = fsinfo_next_free;

    fat32_write_sector(fsinfo_sector, buf);

    /* FAT32 backup FSInfo is at fsinfo_sector + 6 (backup boot sector area) */
    if (fsinfo_sector + 6 < bpb.reserved_sectors) {
        if (fat32_read_sector(fsinfo_sector + 6, buf) == 0) {
            if (*(uint32_t *)&buf[0] == FSINFO_LEAD_SIG) {
                *(uint32_t *)&buf[488] = fsinfo_free_count;
                *(uint32_t *)&buf[492] = fsinfo_next_free;
                fat32_write_sector(fsinfo_sector + 6, buf);
            }
        }
    }

    fsinfo_dirty = 0;
}

/* ---- Free cluster allocation ---- */

/* Find a free cluster starting from the FSInfo hint.
 * Uses wraparound: if no free cluster found from hint→end, retries from 2.
 * Returns cluster number or 0 on failure. */
uint32_t fat32_alloc_cluster(void)
{
    uint32_t total_data_clusters;
    uint32_t max_cluster;
    uint32_t start;
    uint32_t cluster;
    int pass;

    total_data_clusters = (bpb.total_sectors - bpb.first_data_sector)
                        / bpb.sectors_per_cluster;
    max_cluster = total_data_clusters + 2;

    /* Start from hint if valid, otherwise cluster 2 */
    start = (fsinfo_valid && fsinfo_next_free >= 2
             && fsinfo_next_free < max_cluster)
          ? fsinfo_next_free : 2;

    /* Two passes: hint→end, then 2→hint (wraparound) */
    for (pass = 0; pass < 2; pass++) {
        uint32_t begin = (pass == 0) ? start : 2;
        uint32_t end   = (pass == 0) ? max_cluster : start;

        for (cluster = begin; cluster < end; cluster++) {
            if (fat32_get_fat_entry(cluster) == FAT32_FREE) {
                if (fat32_set_fat_entry(cluster, 0x0FFFFFFF) != 0)
                    return 0;

                /* Update FSInfo state */
                fsinfo_next_free = cluster + 1;
                if (fsinfo_next_free >= max_cluster)
                    fsinfo_next_free = 2;
                if (fsinfo_free_count != FSINFO_UNKNOWN
                    && fsinfo_free_count > 0)
                    fsinfo_free_count--;
                fsinfo_dirty = 1;

                return cluster;
            }
        }
    }

    return 0;  /* Disk full */
}

/* Free a cluster chain starting from 'cluster' */
void fat32_free_chain(uint32_t cluster)
{
    while (cluster >= 2 && cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t next = fat32_get_fat_entry(cluster);
        fat32_set_fat_entry(cluster, FAT32_FREE);

        /* Update FSInfo: increment free count, lower hint if applicable */
        if (fsinfo_free_count != FSINFO_UNKNOWN)
            fsinfo_free_count++;
        if (cluster < fsinfo_next_free)
            fsinfo_next_free = cluster;
        fsinfo_dirty = 1;

        cluster = next;
    }
}

/* Zero a cluster (clear data area) */
int fat32_zero_cluster(uint32_t cluster)
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

/* Flush FAT32 — write all dirty cached sectors to disk */
int fat32_flush_disk(void)
{
    scache_flush();
    return 0;
}
