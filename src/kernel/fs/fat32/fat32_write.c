/* ============================================================================
 * fat32_write.c — Write API
 *
 * File creation, writing, deletion, renaming, truncation, rmdir,
 * attribute setting, and timestamp setting.
 * ============================================================================ */

#include "fat32_internal.h"

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

/* Truncate a file to new_size bytes. */
int fat32_truncate(uint32_t dir_cluster, const char *name, uint32_t new_size)
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
                    /* Truncate-to-zero: free entire chain */
                    if (fc >= 2)
                        fat32_free_chain(fc);
                    de->first_cluster_hi = 0;
                    de->first_cluster_lo = 0;
                } else if (new_size < de->file_size && fc >= 2) {
                    /* Partial truncate: keep clusters for new_size */
                    uint32_t clusters_needed =
                        (new_size + bytes_per_cluster - 1) / bytes_per_cluster;
                    uint32_t walk = fc;
                    uint32_t ci;

                    for (ci = 1; ci < clusters_needed; ci++) {
                        uint32_t next = fat32_get_fat_entry(walk);
                        if (next < 2 || next >= FAT32_EOC) break;
                        walk = next;
                    }
                    /* Free everything after the last kept cluster */
                    {
                        uint32_t tail = fat32_get_fat_entry(walk);
                        fat32_set_fat_entry(walk, 0x0FFFFFFF); /* EOC */
                        if (tail >= 2 && tail < FAT32_EOC)
                            fat32_free_chain(tail);
                    }
                }

                /* Update size in directory entry */
                de->file_size = new_size;
                fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                          cluster_buf);

                dir_file_count = 0;  /* invalidate cache */
                kfree(cluster_buf);
                return 0;
            }
        }

        cur_cluster = fat32_get_fat_entry(cur_cluster);
    }

trunc_not_found:
    kfree(cluster_buf);
    return -1;
}

/* Remove a directory — verify it's empty first, then delete. */
int fat32_rmdir(uint32_t parent_cluster, const char *name)
{
    uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = parent_cluster;

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
                    return -1;  /* not a directory */
                }

                fc = ((uint32_t)de->first_cluster_hi << 16)
                   | (uint32_t)de->first_cluster_lo;

                /* Check directory is empty (only . and .. allowed) */
                dir_buf = (uint8_t *)kmalloc(bytes_per_cluster);
                if (!dir_buf) { kfree(cluster_buf); return -1; }

                dc = fc;
                while (dc >= 2 && dc < FAT32_EOC) {
                    uint32_t ds = cluster_to_sector(dc);
                    uint32_t di;

                    if (fat32_read_sectors_multi(ds, bpb.sectors_per_cluster,
                                                 dir_buf) != 0)
                        break;

                    for (di = 0; di < bytes_per_cluster; di += 32) {
                        struct fat32_dir_entry *child =
                            (struct fat32_dir_entry *)&dir_buf[di];

                        if (child->name[0] == 0x00) goto fat32_dir_empty;
                        if (child->name[0] == 0xE5) continue;
                        if (child->attr == FAT32_ATTR_LFN) continue;
                        /* Skip . and .. */
                        if (child->name[0] == '.' &&
                            (child->name[1] == ' ' ||
                             (child->name[1] == '.' &&
                              child->name[2] == ' ')))
                            continue;

                        /* Found a real entry — not empty */
                        kfree(dir_buf);
                        kfree(cluster_buf);
                        return -1;
                    }
                    dc = fat32_get_fat_entry(dc);
                }

fat32_dir_empty:
                kfree(dir_buf);

                /* Directory is empty — free its chain and mark deleted */
                if (fc >= 2)
                    fat32_free_chain(fc);
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

rmdir_not_found:
    kfree(cluster_buf);
    return -1;
}

/* Set file attributes on a FAT32 directory entry */
int fat32_set_attr(uint32_t dir_cluster, const char *name, uint8_t new_attr)
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

            if (de->name[0] == 0x00) goto attr_not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != short_name[j]) { match = 0; break; }
            }

            if (match) {
                /* Preserve DIRECTORY and VOLUME_ID bits, update the rest */
                uint8_t preserved = de->attr & (FAT32_ATTR_DIRECTORY |
                                                FAT32_ATTR_VOLUME_ID);
                de->attr = preserved | (new_attr & (FAT32_ATTR_READ_ONLY |
                                                    FAT32_ATTR_HIDDEN |
                                                    FAT32_ATTR_SYSTEM |
                                                    FAT32_ATTR_ARCHIVE));
                fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                          cluster_buf);
                dir_file_count = 0;
                kfree(cluster_buf);
                return 0;
            }
        }
        cur_cluster = fat32_get_fat_entry(cur_cluster);
    }

attr_not_found:
    kfree(cluster_buf);
    return -1;
}

/* Set timestamps on a FAT32 directory entry */
int fat32_set_times(uint32_t dir_cluster, const char *name,
                    const filetime_t *ctime_p, const filetime_t *mtime_p,
                    const filetime_t *atime_p)
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
                fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                          cluster_buf);
                kfree(cluster_buf);
                return 0;
            }
        }
        cur_cluster = fat32_get_fat_entry(cur_cluster);
    }

times_not_found:
    kfree(cluster_buf);
    return -1;
}
