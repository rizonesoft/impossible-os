/* ============================================================================
 * fat32_core.c -- String helpers, sector cache, sector I/O, FAT operations
 *
 * Contains the low-level building blocks used by all other FAT32 modules:
 * string utilities, LRU sector cache, block device I/O wrappers, FAT entry
 * manipulation, cluster allocation/free, and FSInfo sector management.
 *
 * All functions take a fat32_volume* for per-volume state access.
 * ============================================================================ */

#include "fat32_internal.h"

/* ---- String helpers (stateless) ---- */

void fat32_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

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

void fat32_short_name_to_str(const uint8_t *raw, char *out)
{
    int i, j = 0;

    for (i = 0; i < 8; i++) {
        if (raw[i] != ' ')
            out[j++] = (char)(raw[i] >= 'A' && raw[i] <= 'Z'
                        ? raw[i] + 32 : raw[i]);
    }

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

void lfn_extract_chars(const struct fat32_lfn_entry *lfn,
                       char *name_buf, int seq_index)
{
    int base = seq_index * FAT32_LFN_CHARS;
    int i;

    for (i = 0; i < 5; i++) {
        uint16_t ch = lfn->name1[i];
        if (ch == 0 || ch == 0xFFFF) return;
        name_buf[base + i] = (ch < 128) ? (char)ch : '_';
    }
    for (i = 0; i < 6; i++) {
        uint16_t ch = lfn->name2[i];
        if (ch == 0 || ch == 0xFFFF) return;
        name_buf[base + 5 + i] = (ch < 128) ? (char)ch : '_';
    }
    for (i = 0; i < 2; i++) {
        uint16_t ch = lfn->name3[i];
        if (ch == 0 || ch == 0xFFFF) return;
        name_buf[base + 11 + i] = (ch < 128) ? (char)ch : '_';
    }
}

/* ---- Sector cache (LRU with dirty tracking) ---- */

static scache_entry_t *scache_lookup(struct fat32_volume *vol, uint32_t sector)
{
    int i;
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (vol->cache[i].valid && vol->cache[i].sector == sector) {
            vol->cache[i].last_access = ++vol->cache_access;
            return &vol->cache[i];
        }
    }
    return (scache_entry_t *)0;
}

static scache_entry_t *scache_evict(struct fat32_volume *vol)
{
    int i;
    int lru_idx = 0;
    uint32_t lru_val = 0xFFFFFFFF;

    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (!vol->cache[i].valid)
            return &vol->cache[i];
    }

    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (vol->cache[i].last_access < lru_val) {
            lru_val = vol->cache[i].last_access;
            lru_idx = i;
        }
    }

    if (vol->cache[lru_idx].dirty) {
        blkdev_write(vol->dev, vol->cache[lru_idx].sector, 1,
                     vol->cache[lru_idx].data);
        vol->cache[lru_idx].dirty = 0;
    }

    vol->cache[lru_idx].valid = 0;
    return &vol->cache[lru_idx];
}

void scache_flush(struct fat32_volume *vol)
{
    int i;
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (vol->cache[i].valid && vol->cache[i].dirty) {
            blkdev_write(vol->dev, vol->cache[i].sector, 1,
                         vol->cache[i].data);
            vol->cache[i].dirty = 0;
        }
    }
}

void scache_invalidate(struct fat32_volume *vol)
{
    int i;
    for (i = 0; i < SCACHE_SLOTS; i++) {
        if (vol->cache[i].valid && vol->cache[i].dirty) {
            blkdev_write(vol->dev, vol->cache[i].sector, 1,
                         vol->cache[i].data);
        }
        vol->cache[i].valid = 0;
        vol->cache[i].dirty = 0;
    }
    vol->cache_access = 0;
}

/* ---- Block device I/O (cached) ---- */

int fat32_read_sector(struct fat32_volume *vol, uint32_t sector, void *buf)
{
    scache_entry_t *e = scache_lookup(vol, sector);
    uint8_t *dst = (uint8_t *)buf;
    int i;

    if (e) {
        for (i = 0; i < 512; i++)
            dst[i] = e->data[i];
        return 0;
    }

    e = scache_evict(vol);
    if (blkdev_read(vol->dev, sector, 1, e->data) != 0)
        return -1;

    e->sector      = sector;
    e->valid       = 1;
    e->dirty       = 0;
    e->last_access = ++vol->cache_access;

    for (i = 0; i < 512; i++)
        dst[i] = e->data[i];

    return 0;
}

int fat32_read_sectors_multi(struct fat32_volume *vol, uint32_t sector,
                              uint32_t count, void *buf)
{
    return blkdev_read(vol->dev, sector, count, buf);
}

int fat32_write_sector(struct fat32_volume *vol, uint32_t sector,
                        const void *buf)
{
    scache_entry_t *e = scache_lookup(vol, sector);
    const uint8_t *src = (const uint8_t *)buf;
    int i;

    if (!e) {
        e = scache_evict(vol);
        e->sector = sector;
        e->valid  = 1;
    }

    for (i = 0; i < 512; i++)
        e->data[i] = src[i];

    e->dirty       = 1;
    e->last_access = ++vol->cache_access;

    return 0;
}

int fat32_write_sectors_multi(struct fat32_volume *vol, uint32_t sector,
                               uint32_t count, const void *buf)
{
    return blkdev_write(vol->dev, sector, count, buf);
}

/* ---- FAT entry manipulation ---- */

uint32_t fat32_get_fat_entry(struct fat32_volume *vol, uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->bpb.first_fat_sector + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;
    uint32_t val;

    if (fat32_read_sector(vol, fat_sector, vol->sector_buf) != 0)
        return FAT32_EOC;

    val = *(uint32_t *)&vol->sector_buf[entry_offset];
    return val & 0x0FFFFFFF;
}

int fat32_set_fat_entry(struct fat32_volume *vol, uint32_t cluster,
                         uint32_t value)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t entry_offset = fat_offset % 512;
    uint32_t fat_relative_sector = fat_offset / 512;
    uint32_t existing;
    uint32_t fi;

    for (fi = 0; fi < vol->bpb.num_fats; fi++) {
        uint32_t fat_sector = vol->bpb.first_fat_sector
                            + fi * vol->bpb.fat_size_sectors
                            + fat_relative_sector;

        if (fat32_read_sector(vol, fat_sector, vol->sector_buf) != 0)
            return -1;

        existing = *(uint32_t *)&vol->sector_buf[entry_offset];
        existing = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
        *(uint32_t *)&vol->sector_buf[entry_offset] = existing;

        if (fat32_write_sector(vol, fat_sector, vol->sector_buf) != 0)
            return -1;
    }

    return 0;
}

uint32_t fat32_next_cluster(struct fat32_volume *vol, uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->bpb.first_fat_sector + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;
    uint32_t next;

    if (fat32_read_sector(vol, fat_sector, vol->sector_buf) != 0)
        return FAT32_EOC;

    next = *(uint32_t *)&vol->sector_buf[entry_offset];
    next &= 0x0FFFFFFF;

    return next;
}

uint32_t cluster_to_sector(struct fat32_volume *vol, uint32_t cluster)
{
    return vol->bpb.first_data_sector
         + (cluster - 2) * vol->bpb.sectors_per_cluster;
}

/* ---- FSInfo sector I/O ---- */

void fat32_fsinfo_flush(struct fat32_volume *vol)
{
    uint8_t buf[512];

    if (!vol->fsinfo_valid || !vol->fsinfo_dirty)
        return;

    if (fat32_read_sector(vol, vol->fsinfo_sector, buf) != 0)
        return;

    if (*(uint32_t *)&buf[0] != FSINFO_LEAD_SIG)
        return;

    *(uint32_t *)&buf[488] = vol->fsinfo_free_count;
    *(uint32_t *)&buf[492] = vol->fsinfo_next_free;

    fat32_write_sector(vol, vol->fsinfo_sector, buf);

    if (vol->fsinfo_sector + 6 < vol->bpb.reserved_sectors) {
        if (fat32_read_sector(vol, vol->fsinfo_sector + 6, buf) == 0) {
            if (*(uint32_t *)&buf[0] == FSINFO_LEAD_SIG) {
                *(uint32_t *)&buf[488] = vol->fsinfo_free_count;
                *(uint32_t *)&buf[492] = vol->fsinfo_next_free;
                fat32_write_sector(vol, vol->fsinfo_sector + 6, buf);
            }
        }
    }

    vol->fsinfo_dirty = 0;
}

/* ---- Free cluster allocation ---- */

uint32_t fat32_alloc_cluster(struct fat32_volume *vol)
{
    uint32_t total_data_clusters;
    uint32_t max_cluster;
    uint32_t start;
    uint32_t cluster;
    int pass;

    total_data_clusters = (vol->bpb.total_sectors - vol->bpb.first_data_sector)
                        / vol->bpb.sectors_per_cluster;
    max_cluster = total_data_clusters + 2;

    start = (vol->fsinfo_valid && vol->fsinfo_next_free >= 2
             && vol->fsinfo_next_free < max_cluster)
          ? vol->fsinfo_next_free : 2;

    for (pass = 0; pass < 2; pass++) {
        uint32_t begin = (pass == 0) ? start : 2;
        uint32_t end   = (pass == 0) ? max_cluster : start;

        for (cluster = begin; cluster < end; cluster++) {
            if (fat32_get_fat_entry(vol, cluster) == FAT32_FREE) {
                if (fat32_set_fat_entry(vol, cluster, 0x0FFFFFFF) != 0)
                    return 0;

                vol->fsinfo_next_free = cluster + 1;
                if (vol->fsinfo_next_free >= max_cluster)
                    vol->fsinfo_next_free = 2;
                if (vol->fsinfo_free_count != FSINFO_UNKNOWN
                    && vol->fsinfo_free_count > 0)
                    vol->fsinfo_free_count--;
                vol->fsinfo_dirty = 1;

                return cluster;
            }
        }
    }

    return 0;
}

void fat32_free_chain(struct fat32_volume *vol, uint32_t cluster)
{
    while (cluster >= 2 && cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t next = fat32_get_fat_entry(vol, cluster);
        fat32_set_fat_entry(vol, cluster, FAT32_FREE);

        if (vol->fsinfo_free_count != FSINFO_UNKNOWN)
            vol->fsinfo_free_count++;
        if (cluster < vol->fsinfo_next_free)
            vol->fsinfo_next_free = cluster;
        vol->fsinfo_dirty = 1;

        cluster = next;
    }
}

int fat32_zero_cluster(struct fat32_volume *vol, uint32_t cluster)
{
    uint32_t sector = cluster_to_sector(vol, cluster);
    uint8_t zero[512];
    uint32_t si;
    uint32_t k;

    for (k = 0; k < 512; k++)
        zero[k] = 0;

    for (si = 0; si < vol->bpb.sectors_per_cluster; si++) {
        if (fat32_write_sector(vol, sector + si, zero) != 0)
            return -1;
    }
    return 0;
}

int fat32_flush_disk(struct fat32_volume *vol)
{
    scache_flush(vol);
    return 0;
}
