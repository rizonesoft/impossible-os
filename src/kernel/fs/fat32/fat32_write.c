/* ============================================================================
 * fat32_write.c -- Write API
 *
 * File creation, writing, deletion, renaming, truncation, rmdir,
 * attribute setting, and timestamp setting.
 * ============================================================================ */

#include "fat32_internal.h"

/* ---- Public write API ---- */

int fat32_create_file(uint32_t dir_cluster, const char *name)
{
    /* Legacy wrapper -- not used in multi-volume path.
     * VFS ops extract vol from node->fs_data and call internal functions. */
    (void)dir_cluster;
    (void)name;
    return -1;
}

int fat32_create_dir(uint32_t parent_cluster, const char *name)
{
    (void)parent_cluster;
    (void)name;
    return -1;
}

int fat32_write_file(uint32_t dir_cluster, const char *name,
                     const void *data, uint32_t size)
{
    (void)dir_cluster;
    (void)name;
    (void)data;
    (void)size;
    return -1;
}

/* ---- Internal volume-aware write functions ---- */

int fat32_create_file_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                           const char *name)
{
    struct fat32_dir_entry de;
    uint32_t first_cluster;
    int i;

    if (!name || !name[0])
        return -1;

    first_cluster = fat32_alloc_cluster(vol);
    if (first_cluster == 0)
        return -1;

    fat32_zero_cluster(vol, first_cluster);

    for (i = 0; i < 32; i++)
        ((uint8_t *)&de)[i] = 0;

    fat32_make_short_name(name, de.name);
    de.attr = FAT32_ATTR_ARCHIVE;
    de.first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de.file_size = 0;
    fat32_stamp_create(&de);

    if (fat32_needs_lfn(name)) {
        if (fat32_lfn_write_slots(vol, dir_cluster, de.name, name, &de) != 0) {
            fat32_free_chain(vol, first_cluster);
            return -1;
        }
    } else {
        uint32_t slot_sector, slot_offset, slot_cluster;
        if (fat32_find_free_dir_slot(vol, dir_cluster, &slot_sector,
                                      &slot_offset, &slot_cluster) != 0) {
            fat32_free_chain(vol, first_cluster);
            return -1;
        }
        if (fat32_write_dir_entry(vol, slot_sector, slot_offset, &de) != 0) {
            fat32_free_chain(vol, first_cluster);
            return -1;
        }
    }

    vol->dir_file_count = 0;
    return 0;
}

int fat32_create_dir_vol(struct fat32_volume *vol, uint32_t parent_cluster,
                          const char *name)
{
    struct fat32_dir_entry de;
    uint32_t new_cluster;
    uint8_t dot_buf[512];
    int i;

    if (!name || !name[0])
        return -1;

    new_cluster = fat32_alloc_cluster(vol);
    if (new_cluster == 0)
        return -1;

    fat32_zero_cluster(vol, new_cluster);

    /* Write . and .. entries in the new directory's first sector */
    for (i = 0; i < 512; i++)
        dot_buf[i] = 0;

    dot_buf[0] = '.';
    for (i = 1; i < 11; i++) dot_buf[i] = ' ';
    dot_buf[11] = FAT32_ATTR_DIRECTORY;
    *(uint16_t *)&dot_buf[20] = (uint16_t)(new_cluster >> 16);
    *(uint16_t *)&dot_buf[26] = (uint16_t)(new_cluster & 0xFFFF);

    dot_buf[32] = '.';
    dot_buf[33] = '.';
    for (i = 34; i < 43; i++) dot_buf[i] = ' ';
    dot_buf[43] = FAT32_ATTR_DIRECTORY;
    *(uint16_t *)&dot_buf[52] = (uint16_t)(parent_cluster >> 16);
    *(uint16_t *)&dot_buf[58] = (uint16_t)(parent_cluster & 0xFFFF);

    if (fat32_write_sector(vol, cluster_to_sector(vol, new_cluster),
                            dot_buf) != 0) {
        fat32_free_chain(vol, new_cluster);
        return -1;
    }

    /* Build the SFN directory entry for the parent */
    for (i = 0; i < 32; i++)
        ((uint8_t *)&de)[i] = 0;

    fat32_make_short_name(name, de.name);
    de.attr = FAT32_ATTR_DIRECTORY;
    de.first_cluster_hi = (uint16_t)(new_cluster >> 16);
    de.first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    de.file_size = 0;
    fat32_stamp_create(&de);

    if (fat32_needs_lfn(name)) {
        if (fat32_lfn_write_slots(vol, parent_cluster, de.name, name, &de) != 0) {
            fat32_free_chain(vol, new_cluster);
            return -1;
        }
    } else {
        uint32_t slot_sector, slot_offset, slot_cluster;
        if (fat32_find_free_dir_slot(vol, parent_cluster, &slot_sector,
                                      &slot_offset, &slot_cluster) != 0) {
            fat32_free_chain(vol, new_cluster);
            return -1;
        }
        if (fat32_write_dir_entry(vol, slot_sector, slot_offset, &de) != 0) {
            fat32_free_chain(vol, new_cluster);
            return -1;
        }
    }

    vol->dir_file_count = 0;
    return 0;
}

int fat32_delete_file_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                           const char *name)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(name, short_name);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
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

                if (fc >= 2)
                    fat32_free_chain(vol, fc);

                /* Mark SFN entry as deleted */
                cluster_buf[i] = 0xE5;

                /* Delete preceding LFN slots (scan backwards) */
                {
                    uint8_t chk = fat32_lfn_checksum(de->name);
                    int pos = (int)i - 32;
                    while (pos >= 0) {
                        struct fat32_lfn_entry *lfn =
                            (struct fat32_lfn_entry *)&cluster_buf[pos];
                        if (lfn->attr != FAT32_ATTR_LFN) break;
                        if (lfn->checksum != chk) break;
                        cluster_buf[pos] = 0xE5;
                        pos -= 32;
                    }
                }

                fat32_write_sectors_multi(vol, sector,
                                          vol->bpb.sectors_per_cluster,
                                          cluster_buf);

                vol->dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }

        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

del_not_found:
    kfree(cluster_buf);
    return -1;
}

int fat32_rename_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                      const char *old_name, const char *new_name)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t old_short[11];
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(old_name, old_short);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
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
                fat32_write_sectors_multi(vol, sector,
                                          vol->bpb.sectors_per_cluster,
                                          cluster_buf);
                vol->dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }

        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

ren_not_found:
    kfree(cluster_buf);
    return -1;
}

int fat32_truncate(struct fat32_volume *vol, uint32_t dir_cluster,
                    const char *name, uint32_t new_size)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(name, short_name);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) goto trunc_not_found;
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

                if (new_size == 0) {
                    if (fc >= 2)
                        fat32_free_chain(vol, fc);
                    de->first_cluster_hi = 0;
                    de->first_cluster_lo = 0;
                } else if (new_size < de->file_size && fc >= 2) {
                    uint32_t clusters_needed =
                        (new_size + bytes_per_cluster - 1) / bytes_per_cluster;
                    uint32_t walk = fc;
                    uint32_t ci;

                    for (ci = 1; ci < clusters_needed; ci++) {
                        uint32_t next = fat32_get_fat_entry(vol, walk);
                        if (next < 2 || next >= FAT32_EOC) break;
                        walk = next;
                    }
                    {
                        uint32_t tail = fat32_get_fat_entry(vol, walk);
                        fat32_set_fat_entry(vol, walk, 0x0FFFFFFF);
                        if (tail >= 2 && tail < FAT32_EOC)
                            fat32_free_chain(vol, tail);
                    }
                }

                de->file_size = new_size;
                fat32_write_sectors_multi(vol, sector,
                                          vol->bpb.sectors_per_cluster,
                                          cluster_buf);

                vol->dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }

        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

trunc_not_found:
    kfree(cluster_buf);
    return -1;
}

int fat32_rmdir(struct fat32_volume *vol, uint32_t parent_cluster,
                 const char *name)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = parent_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(name, short_name);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) goto rmdir_not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != short_name[j]) { match = 0; break; }
            }

            if (match) {
                uint32_t fc;
                uint8_t *dir_buf;
                uint32_t dc;

                if (!(de->attr & FAT32_ATTR_DIRECTORY)) {
                    kfree(cluster_buf);
                    return -1;
                }

                fc = ((uint32_t)de->first_cluster_hi << 16)
                   | (uint32_t)de->first_cluster_lo;

                dir_buf = (uint8_t *)kmalloc(bytes_per_cluster);
                if (!dir_buf) { kfree(cluster_buf); return -1; }

                dc = fc;
                while (dc >= 2 && dc < FAT32_EOC) {
                    uint32_t ds = cluster_to_sector(vol, dc);
                    uint32_t di;

                    if (fat32_read_sectors_multi(vol, ds,
                            vol->bpb.sectors_per_cluster, dir_buf) != 0)
                        break;

                    for (di = 0; di < bytes_per_cluster; di += 32) {
                        struct fat32_dir_entry *child =
                            (struct fat32_dir_entry *)&dir_buf[di];

                        if (child->name[0] == 0x00) goto fat32_dir_empty;
                        if (child->name[0] == 0xE5) continue;
                        if (child->attr == FAT32_ATTR_LFN) continue;
                        if (child->name[0] == '.' &&
                            (child->name[1] == ' ' ||
                             (child->name[1] == '.' &&
                              child->name[2] == ' ')))
                            continue;

                        kfree(dir_buf);
                        kfree(cluster_buf);
                        return -1;
                    }
                    dc = fat32_get_fat_entry(vol, dc);
                }

fat32_dir_empty:
                kfree(dir_buf);

                if (fc >= 2)
                    fat32_free_chain(vol, fc);
                cluster_buf[i] = 0xE5;
                fat32_write_sectors_multi(vol, sector,
                                          vol->bpb.sectors_per_cluster,
                                          cluster_buf);
                vol->dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }
        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

rmdir_not_found:
    kfree(cluster_buf);
    return -1;
}

int fat32_set_attr(struct fat32_volume *vol, uint32_t dir_cluster,
                    const char *name, uint8_t new_attr)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(name, short_name);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) goto attr_not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != short_name[j]) { match = 0; break; }
            }

            if (match) {
                uint8_t preserved = de->attr & (FAT32_ATTR_DIRECTORY |
                                                FAT32_ATTR_VOLUME_ID);
                de->attr = preserved | (new_attr & (FAT32_ATTR_READ_ONLY |
                                                    FAT32_ATTR_HIDDEN |
                                                    FAT32_ATTR_SYSTEM |
                                                    FAT32_ATTR_ARCHIVE));
                fat32_write_sectors_multi(vol, sector,
                                          vol->bpb.sectors_per_cluster,
                                          cluster_buf);
                vol->dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }
        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

attr_not_found:
    kfree(cluster_buf);
    return -1;
}

int fat32_set_times(struct fat32_volume *vol, uint32_t dir_cluster,
                     const char *name, const filetime_t *ctime_p,
                     const filetime_t *mtime_p, const filetime_t *atime_p)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(name, short_name);

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) goto times_not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != short_name[j]) { match = 0; break; }
            }

            if (match) {
                if (ctime_p) {
                    uint16_t ct, cd;
                    seconds_to_fat_datetime(ctime_p->seconds, &ct, &cd);
                    de->create_time = ct;
                    de->create_date = cd;
                }
                if (mtime_p) {
                    uint16_t mt, md;
                    seconds_to_fat_datetime(mtime_p->seconds, &mt, &md);
                    de->modify_time = mt;
                    de->modify_date = md;
                }
                if (atime_p) {
                    uint16_t at, ad;
                    seconds_to_fat_datetime(atime_p->seconds, &at, &ad);
                    de->access_date = ad;
                }
                fat32_write_sectors_multi(vol, sector,
                                          vol->bpb.sectors_per_cluster,
                                          cluster_buf);
                kfree(cluster_buf);
                return 0;
            }
        }
        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

times_not_found:
    kfree(cluster_buf);
    return -1;
}
