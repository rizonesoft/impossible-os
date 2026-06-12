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

/* ---- LFN helpers ---- */

uint8_t fat32_lfn_checksum(const uint8_t sfn[11])
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

int fat32_lfn_slot_count(const char *name)
{
    int len = 0;
    while (name[len]) len++;
    return (len + 12) / 13;
}

int fat32_needs_lfn(const char *name)
{
    int i, len = 0, dot_pos = -1, dot_count = 0;

    for (i = 0; name[i]; i++) {
        len++;
        if (name[i] == '.') { dot_pos = i; dot_count++; }
        /* Non-ASCII or special chars require LFN */
        if ((uint8_t)name[i] > 127) return 1;
        /* Lowercase requires LFN (8.3 is uppercase only) */
        if (name[i] >= 'a' && name[i] <= 'z') return 1;
        /* Spaces in name require LFN */
        if (name[i] == ' ') return 1;
    }

    /* More than one dot requires LFN */
    if (dot_count > 1) return 1;

    /* Name part > 8 chars or extension > 3 chars */
    if (dot_pos < 0) {
        if (len > 8) return 1;
    } else {
        if (dot_pos > 8) return 1;
        if (len - dot_pos - 1 > 3) return 1;
    }

    return 0;
}

/* Check if an SFN already exists in the directory.
 * Returns 1 if found, 0 if not found. */
static int sfn_exists_in_dir(struct fat32_volume *vol, uint32_t dir_cluster,
                              const uint8_t *sfn)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t cur_cluster = dir_cluster;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return 0;

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

            if (de->name[0] == 0x00) goto not_found;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != sfn[j]) { match = 0; break; }
            }
            if (match) {
                kfree(cluster_buf);
                return 1;
            }
        }

        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

not_found:
    kfree(cluster_buf);
    return 0;
}

int fat32_generate_sfn(struct fat32_volume *vol, uint32_t dir_cluster,
                        const char *name, uint8_t *sfn)
{
    int tail;

    /* Start with the basic 8.3 conversion */
    fat32_make_short_name(name, sfn);

    /* If the name doesn't need LFN, the SFN is the direct conversion */
    if (!fat32_needs_lfn(name))
        return 0;

    /* Try ~1 through ~9 (6-char base) */
    for (tail = 1; tail <= 9; tail++) {
        sfn[6] = '~';
        sfn[7] = (uint8_t)('0' + tail);
        if (!sfn_exists_in_dir(vol, dir_cluster, sfn))
            return 0;
    }

    /* Try ~10 through ~99 (5-char base) */
    for (tail = 10; tail <= 99; tail++) {
        sfn[5] = '~';
        sfn[6] = (uint8_t)('0' + (tail / 10));
        sfn[7] = (uint8_t)('0' + (tail % 10));
        if (!sfn_exists_in_dir(vol, dir_cluster, sfn))
            return 0;
    }

    /* All 99 tails exhausted -- return the last one anyway */
    return -1;
}

/* Find N contiguous free directory entries. Returns sector/offset of the
 * FIRST free slot. Extends the directory cluster chain if needed. */
int fat32_find_free_dir_slots(struct fat32_volume *vol,
                               uint32_t dir_cluster, uint32_t count,
                               uint32_t *out_sector, uint32_t *out_offset,
                               uint32_t *out_cluster)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t cur_cluster = dir_cluster;
    uint32_t run_start_sector = 0, run_start_offset = 0, run_start_cluster = 0;
    uint32_t run_len = 0;
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
                if (run_len == 0) {
                    run_start_sector = sector + (i / 512);
                    run_start_offset = i % 512;
                    run_start_cluster = cur_cluster;
                }
                run_len++;
                if (run_len >= count) {
                    *out_sector = run_start_sector;
                    *out_offset = run_start_offset;
                    *out_cluster = run_start_cluster;
                    kfree(cluster_buf);
                    return 0;
                }
            } else {
                run_len = 0;
            }
        }

        prev_cluster = cur_cluster;
        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

    /* Need to extend directory -- allocate a new cluster */
    {
        uint32_t new_cluster = fat32_alloc_cluster(vol);
        if (new_cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }
        if (prev_cluster)
            fat32_set_fat_entry(vol, prev_cluster, new_cluster);
        fat32_zero_cluster(vol, new_cluster);

        /* The new cluster starts with all-zero entries (free) */
        if (run_len == 0) {
            *out_sector = cluster_to_sector(vol, new_cluster);
            *out_offset = 0;
            *out_cluster = new_cluster;
        } else {
            /* Continuation of run from previous cluster */
            *out_sector = run_start_sector;
            *out_offset = run_start_offset;
            *out_cluster = run_start_cluster;
        }
    }

    kfree(cluster_buf);
    return 0;
}

/* Write LFN slot chain + SFN entry into the directory. */
int fat32_lfn_write_slots(struct fat32_volume *vol,
                           uint32_t dir_cluster,
                           const uint8_t sfn[11],
                           const char *utf8_name,
                           const struct fat32_dir_entry *sfn_entry)
{
    int slots = fat32_lfn_slot_count(utf8_name);
    int total_entries = slots + 1;  /* LFN slots + 1 SFN entry */
    uint8_t checksum = fat32_lfn_checksum(sfn);
    uint32_t start_sector, start_offset, start_cluster;
    int name_len = 0;
    int s;

    while (utf8_name[name_len]) name_len++;

    /* Find contiguous free entries */
    if (fat32_find_free_dir_slots(vol, dir_cluster, (uint32_t)total_entries,
                                   &start_sector, &start_offset,
                                   &start_cluster) != 0)
        return -1;

    /* Write LFN slots (reverse order: last slot first) */
    for (s = slots; s >= 1; s--) {
        struct fat32_lfn_entry lfn;
        uint8_t *p = (uint8_t *)&lfn;
        int base = (s - 1) * 13;  /* character offset for this slot */
        int ci;
        uint32_t entry_idx = (uint32_t)(slots - s);  /* 0-based position in dir */
        uint32_t byte_offset;
        uint32_t wr_sector;
        uint8_t sec_buf[512];

        /* Zero the entry */
        for (ci = 0; ci < 32; ci++) p[ci] = 0;

        /* Sequence number */
        lfn.seq = (uint8_t)s;
        if (s == slots) lfn.seq |= LFN_LAST_ENTRY;

        lfn.attr = FAT32_ATTR_LFN;
        lfn.type = 0;
        lfn.checksum = checksum;
        lfn.first_cluster = 0;

        /* Pack UTF-16LE chars into Name1 (5), Name2 (6), Name3 (2) */
        for (ci = 0; ci < 5; ci++) {
            int idx = base + ci;
            if (idx < name_len)
                lfn.name1[ci] = (uint16_t)(uint8_t)utf8_name[idx];
            else if (idx == name_len)
                lfn.name1[ci] = 0x0000;
            else
                lfn.name1[ci] = 0xFFFF;
        }
        for (ci = 0; ci < 6; ci++) {
            int idx = base + 5 + ci;
            if (idx < name_len)
                lfn.name2[ci] = (uint16_t)(uint8_t)utf8_name[idx];
            else if (idx == name_len)
                lfn.name2[ci] = 0x0000;
            else
                lfn.name2[ci] = 0xFFFF;
        }
        for (ci = 0; ci < 2; ci++) {
            int idx = base + 11 + ci;
            if (idx < name_len)
                lfn.name3[ci] = (uint16_t)(uint8_t)utf8_name[idx];
            else if (idx == name_len)
                lfn.name3[ci] = 0x0000;
            else
                lfn.name3[ci] = 0xFFFF;
        }

        /* Calculate the absolute position of this entry */
        byte_offset = start_offset + entry_idx * 32;
        wr_sector = start_sector + (byte_offset / 512);
        byte_offset = byte_offset % 512;

        if (fat32_read_sector(vol, wr_sector, sec_buf) != 0)
            return -1;

        for (ci = 0; ci < 32; ci++)
            sec_buf[byte_offset + ci] = p[ci];

        if (fat32_write_sector(vol, wr_sector, sec_buf) != 0)
            return -1;
    }

    /* Write SFN entry after all LFN slots */
    {
        uint32_t sfn_byte_offset = start_offset + (uint32_t)slots * 32;
        uint32_t sfn_sector = start_sector + (sfn_byte_offset / 512);
        uint8_t sec_buf[512];
        const uint8_t *src = (const uint8_t *)sfn_entry;
        int ci;

        sfn_byte_offset = sfn_byte_offset % 512;

        if (fat32_read_sector(vol, sfn_sector, sec_buf) != 0)
            return -1;

        for (ci = 0; ci < 32; ci++)
            sec_buf[sfn_byte_offset + ci] = src[ci];

        if (fat32_write_sector(vol, sfn_sector, sec_buf) != 0)
            return -1;
    }

    return 0;
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
    /* Same-directory refresh? Slots refilled by the SAME on-disk file
     * (matching SFN) keep their live open state below. */
    int same_dir = (vol->dir_cached_cluster == cluster);

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
            /* Slot identity can change on rebuild: reset the per-open
             * bookkeeping unless this is the SAME directory and the slot
             * is being refilled by the SAME on-disk file (SFN match) --
             * then a live handle's state survives the refresh. Stale
             * state from a DIFFERENT previous occupant gave phantom
             * "file is still open" unlink failures (2026-06-12); open
             * handles whose slot gets reassigned to another file remain
             * a tracked node-lifetime gap (storage domain). */
            {
                int same_file = same_dir;
                int sk;
                if (same_file) {
                    for (sk = 0; sk < 11; sk++) {
                        if (f->sfn[sk] != de->name[sk]) {
                            same_file = 0;
                            break;
                        }
                    }
                }
                if (!same_file) {
                    uint32_t hk;
                    uint8_t *hb = (uint8_t *)f->node.open_handles;
                    f->node.ref_count = 0;
                    f->node.delete_on_close = 0;
                    f->node.flags = 0;
                    f->node.oplock_level = 0;
                    f->node.oplock_owner = 0;
                    for (hk = 0; hk < sizeof(f->node.open_handles); hk++)
                        hb[hk] = 0;
                }
            }
            fat32_strcpy(f->node.name, name, VFS_MAX_NAME);
            f->node.type = (de->attr & FAT32_ATTR_DIRECTORY)
                           ? VFS_DIRECTORY : VFS_FILE;
            f->node.inode = fc;
            f->node.size = de->file_size;
            f->first_cluster = fc;
            f->file_size = de->file_size;
            f->volume = vol;
            /* Cache the on-disk SFN for downstream truncate/unlink
             * matching (collision suffix is part of the on-disk name
             * but absent from fat32_make_short_name). */
            {
                int sk;
                for (sk = 0; sk < 11; sk++)
                    f->sfn[sk] = de->name[sk];
            }

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
                fat32_stamp_modify(de);
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

/* ---- Timestamp helpers (FILETIME-based) ---- */

void fat32_stamp_create(struct fat32_dir_entry *de)
{
    FILETIME now = KeQuerySystemTime();
    int tz_bias = (int)timezone_total_bias();
    uint16_t t, d;

    filetime_to_dos_datetime(now, tz_bias, &d, &t);
    de->create_time = t;
    de->create_date = d;
    de->modify_time = t;
    de->modify_date = d;
    de->access_date = d;
    /* CrtTimeTenth: 10ms resolution sub-second component (0-199) */
    {
        uint64_t sub_sec = (now / FILETIME_TICKS_PER_MS) % 1000;
        de->create_time_tenth = (uint8_t)((sub_sec / 10) * 2);
    }
}

void fat32_stamp_modify(struct fat32_dir_entry *de)
{
    FILETIME now = KeQuerySystemTime();
    int tz_bias = (int)timezone_total_bias();
    uint16_t t, d;

    filetime_to_dos_datetime(now, tz_bias, &d, &t);
    de->modify_time = t;
    de->modify_date = d;
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
