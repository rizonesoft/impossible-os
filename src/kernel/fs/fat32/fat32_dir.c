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
 * Returns 1 if found OR if absence could not be proven (allocation, read or
 * FAT failure), 0 only after a complete scan. Reporting "free" on a degraded
 * read is what would let a duplicate short name onto the disk. */
static int sfn_exists_in_dir(struct fat32_volume *vol, uint32_t dir_cluster,
                              const uint8_t *sfn)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t cur_cluster = dir_cluster;
    int result = 1;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return 1;

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            goto out;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) { result = 0; goto out; }
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != sfn[j]) { match = 0; break; }
            }
            if (match)
                goto out;
        }

        if (fat32_next_cluster_checked(vol, cur_cluster, &cur_cluster) != 0)
            goto out;
    }
    result = 0;

out:
    kfree(cluster_buf);
    return result;
}

/* Lay a numeric tail over the base: it follows a short base directly and cuts
 * a long one to fit, because FAT forbids a space inside the name part. */
static void sfn_apply_tail(uint8_t *sfn, int base_len, int tail)
{
    int digits = (tail >= 10) ? 2 : 1;
    int pos = base_len;
    int k;

    if (pos > 7 - digits)
        pos = 7 - digits;
    for (k = 0; k < 8; k++)
        if (k >= pos)
            sfn[k] = ' ';
    sfn[pos++] = '~';
    if (digits == 2)
        sfn[pos++] = (uint8_t)('0' + (tail / 10));
    sfn[pos] = (uint8_t)('0' + (tail % 10));
}

int fat32_generate_sfn(struct fat32_volume *vol, uint32_t dir_cluster,
                        const char *name, uint8_t *sfn)
{
    int tail;
    int base_len = 0;

    /* Start with the basic 8.3 conversion */
    fat32_make_short_name(name, sfn);

    /* If the name doesn't need LFN, the SFN is the direct conversion */
    if (!fat32_needs_lfn(name))
        return 0;

    while (base_len < 8 && sfn[base_len] != ' ')
        base_len++;

    for (tail = 1; tail <= FAT32_SFN_MAX_TAIL; tail++) {
        sfn_apply_tail(sfn, base_len, tail);
        if (!sfn_exists_in_dir(vol, dir_cluster, sfn))
            return 0;
    }

    /* Every tail is taken, or the directory could not be read. */
    return -1;
}

/* Clear `cluster` from byte `from` to its end, on the disk rather than in the
 * cache, skipping the write when it is already clear. */
static int dir_scrub_cluster(struct fat32_volume *vol, uint32_t cluster,
                             uint8_t *cluster_buf, uint32_t from)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint32_t i;
    int dirty = 0;

    for (i = from; i < bytes_per_cluster; i++) {
        if (cluster_buf[i] != 0) {
            cluster_buf[i] = 0;
            dirty = 1;
        }
    }
    if (!dirty)
        return 0;
    return fat32_write_sectors_multi(vol, cluster_to_sector(vol, cluster),
                                     vol->bpb.sectors_per_cluster, cluster_buf);
}

/* Read-only walk of a directory chain: 0 when it reaches a clean end through
 * valid data clusters, -1 on a cycle, an out-of-range, reserved, BAD or free link,
 * or a FAT read failure. A chain longer than the volume has clusters must repeat
 * one. */
static int dir_chain_valid(struct fat32_volume *vol, uint32_t cluster)
{
    uint32_t last_cluster = fat32_last_cluster(vol);
    uint32_t data_clusters = last_cluster - 1u;
    uint32_t steps = 0;

    while (cluster < FAT32_EOC) {
        if (cluster < 2 || cluster > last_cluster || ++steps > data_clusters)
            return -1;
        if (fat32_next_cluster_checked(vol, cluster, &cluster) != 0)
            return -1;
    }
    return 0;
}

/* Find N contiguous free directory entries. Returns the sector/offset of the
 * FIRST one and the cluster holding it; the run may continue into later
 * clusters of the chain. Extends the directory if needed.
 *
 * The first 0x00 ends the directory, and FAT defines everything after it as
 * free whatever bytes it holds. Those bytes are cleared when the scan reaches
 * them: left alone they would either reset the run, placing a new file beyond
 * the terminator where no reader looks, or come back to life as entries once
 * the terminator is consumed. So a scan past the terminator walks to the end of
 * the chain even when the run is already long enough, and the chain is validated
 * first: scrubbing a cluster reached a second time through a corrupt link would
 * erase the live entries in front of its terminator. */
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
    int past_end = 0;

    if (dir_chain_valid(vol, dir_cluster) != 0) {
        klog(LOG_WARN, "fat32", "directory chain from cluster %u is corrupt; "
             "refusing to allocate entries", (uint64_t)dir_cluster);
        return -1;
    }

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;
        int scrubbed = 0;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }

        for (i = 0; i < bytes_per_cluster; i += 32) {
            if (past_end || cluster_buf[i] == 0x00) {
                past_end = 1;
                if (!scrubbed) {
                    if (dir_scrub_cluster(vol, cur_cluster, cluster_buf, i) != 0) {
                        kfree(cluster_buf);
                        return -1;
                    }
                    scrubbed = 1;
                }
            }
            if (cluster_buf[i] == 0x00 || cluster_buf[i] == 0xE5) {
                if (run_len == 0) {
                    run_start_sector = sector + (i / 512);
                    run_start_offset = i % 512;
                    run_start_cluster = cur_cluster;
                }
                run_len++;
                if (run_len >= count && !past_end)
                    goto found;
            } else {
                run_len = 0;
            }
        }

        prev_cluster = cur_cluster;
        if (fat32_next_cluster_checked(vol, cur_cluster, &cur_cluster) != 0) {
            kfree(cluster_buf);
            return -1;
        }
    }

    if (prev_cluster == 0) {
        kfree(cluster_buf);
        return -1;
    }

    /* Extend the directory until the run fits: a long name can need more
     * entries than one small cluster holds. The zeros go through the
     * write-through path before the cached FAT link exists, so the flush that
     * publishes the link cannot overtake them and expose a recycled cluster's
     * old bytes as directory entries. (The device's own volatile cache is not
     * ordered here: a sync can sleep and this runs under vol->lock.) */
    while (run_len < count) {
        uint32_t new_cluster = fat32_alloc_cluster(vol);
        uint32_t k;

        if (new_cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }
        for (k = 0; k < bytes_per_cluster; k++)
            cluster_buf[k] = 0;
        if (fat32_write_sectors_multi(vol, cluster_to_sector(vol, new_cluster),
                                      vol->bpb.sectors_per_cluster,
                                      cluster_buf) != 0) {
            fat32_free_chain(vol, new_cluster);
            kfree(cluster_buf);
            return -1;
        }
        /* A failed link may still have reached the first FAT copy, so the
         * cluster stays allocated: a lost cluster is what fsck reclaims, while
         * freeing a cluster the chain may reference is a cross-link. */
        if (fat32_set_fat_entry(vol, prev_cluster, new_cluster) != 0) {
            klog(LOG_WARN, "fat32",
                 "directory extension: link %u -> %u failed; cluster retained",
                 (uint64_t)prev_cluster, (uint64_t)new_cluster);
            kfree(cluster_buf);
            return -1;
        }
        if (run_len == 0) {
            run_start_sector = cluster_to_sector(vol, new_cluster);
            run_start_offset = 0;
            run_start_cluster = new_cluster;
        }
        run_len += bytes_per_cluster / 32;
        prev_cluster = new_cluster;
    }

found:
    *out_sector = run_start_sector;
    *out_offset = run_start_offset;
    *out_cluster = run_start_cluster;
    kfree(cluster_buf);
    return 0;
}

/* Advance a directory cursor one entry, following the cluster chain at a
 * boundary. Directory clusters are not contiguous on disk, so an entry position
 * can never be computed by adding to a sector number. */
static int dir_cursor_next(struct fat32_volume *vol, uint32_t *cluster,
                           uint32_t *byte_offset)
{
    uint32_t next;

    *byte_offset += 32;
    if (*byte_offset < vol->bpb.sectors_per_cluster * 512u)
        return 0;
    if (fat32_next_cluster_checked(vol, *cluster, &next) != 0 ||
        next < 2 || next >= FAT32_EOC)
        return -1;
    *cluster = next;
    *byte_offset = 0;
    return 0;
}

static int dir_cursor_write(struct fat32_volume *vol, uint32_t cluster,
                            uint32_t byte_offset,
                            const struct fat32_dir_entry *entry)
{
    return fat32_write_dir_entry(vol,
                                 cluster_to_sector(vol, cluster) + byte_offset / 512u,
                                 byte_offset % 512u, entry);
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
    uint32_t start_sector, start_offset, cur_cluster, cur_byte;
    int name_len = 0;
    int s;

    while (utf8_name[name_len]) name_len++;

    /* Find contiguous free entries */
    if (fat32_find_free_dir_slots(vol, dir_cluster, (uint32_t)total_entries,
                                   &start_sector, &start_offset,
                                   &cur_cluster) != 0)
        return -1;
    cur_byte = (start_sector - cluster_to_sector(vol, cur_cluster)) * 512u +
               start_offset;

    /* Write LFN slots (reverse order: last slot first) */
    for (s = slots; s >= 1; s--) {
        struct fat32_lfn_entry lfn;
        uint8_t *p = (uint8_t *)&lfn;
        int base = (s - 1) * 13;  /* character offset for this slot */
        int ci;

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

        if (dir_cursor_write(vol, cur_cluster, cur_byte,
                             (const struct fat32_dir_entry *)&lfn) != 0 ||
            dir_cursor_next(vol, &cur_cluster, &cur_byte) != 0)
            return -1;
    }

    /* The SFN entry follows the last slot and publishes the whole run. */
    return dir_cursor_write(vol, cur_cluster, cur_byte, sfn_entry);
}

/* ---- Directory entry manipulation ---- */

int fat32_find_free_dir_slot(struct fat32_volume *vol,
                              uint32_t dir_cluster,
                              uint32_t *out_sector,
                              uint32_t *out_offset,
                              uint32_t *out_cluster)
{
    return fat32_find_free_dir_slots(vol, dir_cluster, 1u, out_sector,
                                     out_offset, out_cluster);
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
    /* CrtTimeTenth (0-199): recovers the half-second that create_time drops
     * (DOS stores seconds/2) plus 10 ms granularity --
     * (seconds & 1) * 100 + centiseconds. The old (ms/10)*2 only produced even
     * values and dropped the odd-second bit. tz bias is whole minutes, so the
     * second parity + sub-second are the same for UTC and local. */
    {
        uint64_t total_ms = now / FILETIME_TICKS_PER_MS;
        uint32_t odd_sec  = (uint32_t)((now / FILETIME_TICKS_PER_SECOND) & 1u);
        uint32_t cs       = (uint32_t)((total_ms % 1000) / 10);  /* 0-99 */
        uint32_t tenth    = odd_sec * 100u + cs;
        de->create_time_tenth = (uint8_t)(tenth > 199u ? 199u : tenth);
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
