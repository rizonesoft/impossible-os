/* ============================================================================
 * fat32_dir.c -- Directory operations
 *
 * 8.3 short name generation, directory reading (with LFN support),
 * directory entry manipulation (find free slot, write entry),
 * directory size updates, and FAT datetime conversion.
 * ============================================================================ */

#include "fat32_internal.h"

/* ---- 8.3 short name generation (stateless) ---- */

void fat32_make_short_name(const char *name, uint8_t *short_name)
{
    int i, j;
    int name_len = 0;
    const char *dot = (const char *)0;

    for (i = 0; i < 11; i++)
        short_name[i] = ' ';

    for (i = 0; name[i]; i++) {
        if (name[i] == '.')
            dot = &name[i];
        name_len++;
    }

    j = 0;
    for (i = 0; name[i] && j < 8; i++) {
        if (&name[i] == dot) break;
        if (name[i] == ' ' || name[i] == '.') continue;
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[j++] = (uint8_t)c;
    }

    if (dot && (dot - name) > 8) {
        short_name[6] = '~';
        short_name[7] = '1';
    } else if (!dot && name_len > 8) {
        short_name[6] = '~';
        short_name[7] = '1';
    }

    if (dot) {
        dot++;
        for (i = 0; dot[i] && i < 3; i++) {
            char c = dot[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            short_name[8 + i] = (uint8_t)c;
        }
    }
}

/* ---- Directory entry manipulation ---- */

int fat32_find_free_dir_slot(struct fat32_volume *vol,
                              uint32_t dir_cluster,
                              uint32_t *out_sector,
                              uint32_t *out_offset,
                              uint32_t *out_cluster)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t cur_cluster = dir_cluster;
    uint32_t prev_cluster = 0;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }

        for (i = 0; i < bytes_per_cluster; i += 32) {
            uint8_t first_byte = cluster_buf[i];
            if (first_byte == 0x00 || first_byte == 0xE5) {
                *out_sector = sector + (i / 512);
                *out_offset = i % 512;
                *out_cluster = cur_cluster;
                kfree(cluster_buf);
                return 0;
            }
        }

        prev_cluster = cur_cluster;
        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

    {
        uint32_t new_cluster = fat32_alloc_cluster(vol);
        if (new_cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }

        fat32_set_fat_entry(vol, prev_cluster, new_cluster);
        fat32_zero_cluster(vol, new_cluster);

        *out_sector = cluster_to_sector(vol, new_cluster);
        *out_offset = 0;
        *out_cluster = new_cluster;
    }

    kfree(cluster_buf);
    return 0;
}

int fat32_write_dir_entry(struct fat32_volume *vol, uint32_t sector,
                           uint32_t offset,
                           const struct fat32_dir_entry *entry)
{
    uint8_t buf[512];
    uint32_t i;
    const uint8_t *src = (const uint8_t *)entry;

    if (fat32_read_sector(vol, sector, buf) != 0)
        return -1;

    for (i = 0; i < 32; i++)
        buf[offset + i] = src[i];

    return fat32_write_sector(vol, sector, buf);
}

/* ---- Directory reading with LFN support ---- */

void fat32_read_dir(struct fat32_volume *vol, uint32_t cluster)
{
    uint32_t bytes_per_cluster;
    uint8_t *cluster_buf;
    uint32_t i;
    char lfn_buf[FAT32_MAX_NAME];
    int lfn_active = 0;

    vol->dir_file_count = 0;
    bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return;

    {
        int k;
        for (k = 0; k < FAT32_MAX_NAME; k++)
            lfn_buf[k] = '\0';
    }

    while (cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t sector = cluster_to_sector(vol, cluster);

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de;
            struct fat32_file *f;
            char name[FAT32_MAX_NAME];
            uint32_t fc;

            if (vol->dir_file_count >= FAT32_MAX_DIR_ENTRIES)
                break;

            de = (struct fat32_dir_entry *)&cluster_buf[i];

            if (de->name[0] == 0x00)
                goto done;

            if (de->name[0] == 0xE5) {
                lfn_active = 0;
                continue;
            }

            if (de->attr == FAT32_ATTR_LFN) {
                struct fat32_lfn_entry *lfn =
                    (struct fat32_lfn_entry *)&cluster_buf[i];
                int seq = lfn->seq & LFN_SEQ_MASK;

                if (lfn->seq & LFN_LAST_ENTRY) {
                    int k;
                    for (k = 0; k < FAT32_MAX_NAME; k++)
                        lfn_buf[k] = '\0';
                    lfn_active = 1;
                }

                if (lfn_active && seq >= 1 && seq <= FAT32_LFN_MAX_ENTRIES)
                    lfn_extract_chars(lfn, lfn_buf, seq - 1);

                continue;
            }

            if (de->attr & FAT32_ATTR_VOLUME_ID) {
                lfn_active = 0;
                continue;
            }

            if (de->name[0] == '.') {
                lfn_active = 0;
                continue;
            }

            if (lfn_active && lfn_buf[0] != '\0') {
                fat32_strcpy(name, lfn_buf, FAT32_MAX_NAME);
            } else {
                fat32_short_name_to_str(de->name, name);
            }
            lfn_active = 0;

            fc = ((uint32_t)de->first_cluster_hi << 16)
               | (uint32_t)de->first_cluster_lo;

            f = &vol->dir_files[vol->dir_file_count];
            fat32_strcpy(f->node.name, name, VFS_MAX_NAME);
            f->node.type = (de->attr & FAT32_ATTR_DIRECTORY)
                           ? VFS_DIRECTORY : VFS_FILE;
            f->node.inode = fc;
            f->node.size = de->file_size;
            f->first_cluster = fc;
            f->file_size = de->file_size;
            f->volume = vol;

            vol->dir_file_count++;
        }

        cluster = fat32_next_cluster(vol, cluster);
    }

done:
    kfree(cluster_buf);
}

/* ---- Helper: update file_size in the on-disk directory entry ---- */

int fat32_update_dir_size(struct fat32_volume *vol, uint32_t search_dir,
                           uint32_t target_fc, uint32_t new_size)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cbuf;
    uint32_t cur_cluster = search_dir;

    cbuf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cbuf)
        return -1;

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cbuf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de = (struct fat32_dir_entry *)&cbuf[i];
            uint32_t fc;

            if (de->name[0] == 0x00) goto upd_not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            fc = ((uint32_t)de->first_cluster_hi << 16)
               | (uint32_t)de->first_cluster_lo;

            if (fc == target_fc) {
                de->file_size = new_size;
                fat32_write_sectors_multi(vol, sector,
                                          vol->bpb.sectors_per_cluster, cbuf);
                kfree(cbuf);
                return 0;
            }
        }
        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

upd_not_found:
    kfree(cbuf);
    return -1;
}

/* ---- FAT datetime conversion (stateless) ---- */

void seconds_to_fat_datetime(uint32_t secs, uint16_t *out_time,
                              uint16_t *out_date)
{
    uint32_t hours, mins, s;
    uint32_t days, year, month, day;
    static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t m;

    s     = secs % 60;
    mins  = (secs / 60) % 60;
    hours = (secs / 3600) % 24;
    days  = secs / 86400;

    year = 2025;
    while (days >= 365) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t yd = leap ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        year++;
    }

    month = 0;
    for (m = 0; m < 12; m++) {
        uint32_t d = dpm[m];
        if (m == 1 && (year % 4 == 0 &&
            (year % 100 != 0 || year % 400 == 0)))
            d = 29;
        if (days < d) break;
        days -= d;
        month++;
    }
    day = days + 1;
    month += 1;

    *out_time = (uint16_t)((hours << 11) | (mins << 5) | (s / 2));
    *out_date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}
