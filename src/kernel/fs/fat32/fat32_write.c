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

/* Write `de` into the directory: with a long-name run when `name` needs one,
 * otherwise as a lone short entry. Reports where it went, so a caller undoing this
 * removes exactly that entry: a name could resolve to another file. */
static int fat32_dir_publish(struct fat32_volume *vol, uint32_t dir_cluster,
                             const char *name, const struct fat32_dir_entry *de,
                             struct fat32_dirent_loc *loc)
{
    uint32_t sector, offset, cluster;

    if (fat32_needs_lfn(name))
        return fat32_lfn_write_slots(vol, dir_cluster, de->name, name, de, loc);
    if (fat32_find_free_dir_slot(vol, dir_cluster, &sector, &offset, &cluster) != 0)
        return -1;
    loc->cluster = cluster;
    loc->byte_offset = (sector - cluster_to_sector(vol, cluster)) * 512u + offset;
    loc->run_cluster = loc->cluster;
    loc->run_offset = loc->byte_offset;
    loc->run_entries = 1u;
    loc->de = *de;
    return fat32_write_dir_entry(vol, sector, offset, de);
}

int fat32_create_file_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                           const char *name)
{
    struct fat32_dir_entry de;
    struct fat32_dirent_loc existing;
    uint32_t first_cluster;
    int i;

    if (!name || !name[0])
        return -1;

    /* A second entry for a name already taken would shadow nothing and leak on
     * every boot, so this refuses as fat32_create_dir_vol does: found, or absence
     * not proven. */
    if (fat32_dir_lookup(vol, dir_cluster, name, &existing) != 0)
        return -1;

    first_cluster = fat32_alloc_cluster(vol);
    if (first_cluster == 0)
        return -1;

    fat32_zero_cluster(vol, first_cluster);

    for (i = 0; i < 32; i++)
        ((uint8_t *)&de)[i] = 0;

    if (fat32_generate_sfn(vol, dir_cluster, name, de.name) != 0) {
        fat32_free_chain(vol, first_cluster);
        return -1;
    }
    de.attr = FAT32_ATTR_ARCHIVE;
    de.first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de.file_size = 0;
    fat32_stamp_create(&de);

    if (fat32_dir_publish(vol, dir_cluster, name, &de, &existing) != 0) {
        fat32_free_chain(vol, first_cluster);
        return -1;
    }

    vol->dir_file_count = 0;
    return 0;
}

int fat32_create_dir_vol(struct fat32_volume *vol, uint32_t parent_cluster,
                          const char *name)
{
    struct fat32_dir_entry de;
    struct fat32_dirent_loc existing;
    uint32_t new_cluster;
    uint8_t dot_buf[512];
    int i;

    if (!name || !name[0])
        return -1;

    /* Idempotent guard: refuse to create a duplicate directory. Without
     * this, re-running (e.g. the BlackBox X:\ skeleton on every boot)
     * would allocate a fresh cluster and write a second dirent for an
     * already-present name, leaking clusters and dir slots on the volume
     * while logging false "created" success. A non-zero result means
     * either the child already exists (1) or absence could not be proven
     * (-1, I/O or malformed chain); in BOTH cases we refuse rather than
     * risk a duplicate. Callers that re-run for self-healing rely on -1
     * meaning "already there or could not create", never "created a
     * duplicate". */
    if (fat32_dir_lookup(vol, parent_cluster, name, &existing) != 0)
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

    if (fat32_generate_sfn(vol, parent_cluster, name, de.name) != 0) {
        fat32_free_chain(vol, new_cluster);
        return -1;
    }
    de.attr = FAT32_ATTR_DIRECTORY;
    de.first_cluster_hi = (uint16_t)(new_cluster >> 16);
    de.first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    de.file_size = 0;
    fat32_stamp_create(&de);

    if (fat32_dir_publish(vol, parent_cluster, name, &de, &existing) != 0) {
        fat32_free_chain(vol, new_cluster);
        return -1;
    }

    vol->dir_file_count = 0;
    return 0;
}

int fat32_delete_file_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                           const char *name)
{
    struct fat32_dirent_loc loc;
    uint32_t first;

    if (fat32_dir_lookup(vol, dir_cluster, name, &loc) != 1)
        return -1;
    if (loc.de.attr & FAT32_ATTR_DIRECTORY)
        return -1;           /* would orphan its tree; rmdir owns directories */

    /* The entry leaves the disk before its clusters return to the free pool,
     * so no live entry ever points at freed clusters. */
    if (fat32_dir_remove_entries(vol, &loc) != 0)
        return -1;

    first = ((uint32_t)loc.de.first_cluster_hi << 16) | loc.de.first_cluster_lo;
    if (first >= 2)
        fat32_free_chain(vol, first);
    return 0;
}

/* Replace an existing file with `src`, keeping every failure and every crash
 * point on one side of "dst is the old file or the new one", with no point at
 * which two entries own one chain: a later truncate or delete through either name
 * would free the other's data, and a publisher reopens its temp with O_TRUNC.
 *
 * So the source entry goes first; an interruption there costs only the unpublished
 * data, as lost clusters fsck reclaims, and the caller was told it failed. The
 * switch is then one write: dst's own short entry takes src's first cluster, size
 * and attributes, in a single sector. dst keeps its slot and long name, so nothing
 * is allocated and no step can leave the name missing. The old chain is freed last.
 *
 * Each flush orders the phases as far as this layer can: scache_flush hands them to
 * the device in turn, but a device with a volatile cache may still reorder them,
 * because a sync can sleep and this runs under vol->lock.
 *
 * A case-only difference between the requested name and dst's stored long name
 * keeps the stored spelling. */
static int fat32_rename_replace(struct fat32_volume *vol,
                                const struct fat32_dirent_loc *src,
                                const struct fat32_dirent_loc *dst)
{
    struct fat32_dirent_loc switched = *dst;
    uint32_t src_first = ((uint32_t)src->de.first_cluster_hi << 16) |
                         src->de.first_cluster_lo;
    uint32_t old_first = ((uint32_t)dst->de.first_cluster_hi << 16) |
                         dst->de.first_cluster_lo;
    uint8_t sector_buf[512];
    int j;

    if ((src->de.attr | dst->de.attr) & FAT32_ATTR_DIRECTORY) {
        klog(LOG_WARN, "fat32", "rename replace-existing: directory; refused");
        return -1;
    }

    /* Whatever can fail without losing anything fails here. Reading dst's sector now
     * makes the switch below a write into the cache: removing src touches far fewer
     * sectors than the cache holds, so the sector cannot be evicted in between. */
    if (fat32_read_sector(vol, cluster_to_sector(vol, dst->cluster) +
                               dst->byte_offset / 512u, sector_buf) != 0)
        return -1;

    /* src's data and chain must be on the device before anything can publish it. */
    if (scache_flush(vol) != 0)
        return -1;

    if (fat32_dir_remove_entries(vol, src) != 0) {
        klog(LOG_WARN, "fat32", "rename: source entry not removed; nothing changed");
        return -1;
    }

    switched.de = src->de;
    for (j = 0; j < 11; j++)
        switched.de.name[j] = dst->de.name[j];
    if (fat32_dir_rewrite_entry(vol, &switched) != 0) {
        /* Not expected after the read above. Keep the data reachable by short name. */
        (void)fat32_dir_rewrite_entry(vol, src);
        klog(LOG_WARN, "fat32", "rename: destination entry not switched");
        return -1;
    }
    if (scache_flush(vol) != 0) {
        /* The switch stays dirty in the cache, which retries it on the next flush. */
        klog(LOG_WARN, "fat32", "rename: destination switch not yet on the device");
        return -1;
    }

    if (old_first >= 2 && old_first != src_first) {
        fat32_free_chain(vol, old_first);
        scache_flush(vol);
    }
    vol->dir_file_count = 0;
    return 0;
}

/* Give `src` a name nothing else holds. The new entries reach the device before the
 * old ones go, so a crash leaves the file under both names rather than none, since
 * a file losing its only name is unrecoverable. Two entries in different sectors
 * cannot change together, so that window cannot be ordered away; fat32_fsck closes
 * it on the dirty mount that follows, keeping one name. A returned error closes it
 * here instead, by discarding the new entries from the cache. A new short name and a new run are written together: a
 * run is bound to its short name by checksum. */
static int fat32_rename_to_free_name(struct fat32_volume *vol, uint32_t dir_cluster,
                                     const struct fat32_dirent_loc *src,
                                     const char *new_name)
{
    struct fat32_dir_entry de = src->de;
    struct fat32_dirent_loc pub;

    if (fat32_generate_sfn(vol, dir_cluster, new_name, de.name) != 0 ||
        fat32_dir_publish(vol, dir_cluster, new_name, &de, &pub) != 0) {
        klog(LOG_WARN, "fat32", "rename: could not write entries for '%s'", new_name);
        return -1;
    }
    if (scache_flush(vol) != 0) {
        klog(LOG_WARN, "fat32", "rename: entries for '%s' did not reach the device; "
             "rolled back", new_name);
        (void)fat32_dir_discard_entries(vol, &pub);
        return -1;
    }
    if (fat32_dir_remove_entries(vol, src) != 0) {
        /* src is still there, so the new entries are what must go. */
        (void)fat32_dir_discard_entries(vol, &pub);
        klog(LOG_WARN, "fat32", "rename: old entries for '%s' not removed", new_name);
        return -1;
    }
    /* Only src's orphaned slots can still be in the cache; the rename itself is done. */
    (void)scache_flush(vol);
    return 0;
}

int fat32_rename_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                      const char *old_name, const char *new_name,
                      uint32_t flags)
{
    struct fat32_dirent_loc src, dst;
    int rc, dst_rc;

    if (!new_name || !new_name[0])
        return -1;

    rc = fat32_dir_lookup(vol, dir_cluster, old_name, &src);
    if (rc != 1) {
        klog(LOG_WARN, "fat32", "rename: source '%s' not found (lookup rc=%d)",
             old_name, (uint64_t)rc);
        return -1;
    }

    dst_rc = fat32_dir_lookup(vol, dir_cluster, new_name, &dst);
    if (dst_rc < 0) {
        klog(LOG_WARN, "fat32", "rename: dst '%s' lookup failed", new_name);
        return -1;
    }

    /* The new name already resolves to this file: nothing to do if it is spelled
     * the same, otherwise this is a respelling and needs fresh entries. */
    if (dst_rc == 1 && dst.cluster == src.cluster &&
        dst.byte_offset == src.byte_offset) {
        const char *a = old_name, *b = new_name;
        char alias[FAT32_MAX_NAME];

        while (*a && *a == *b) { a++; b++; }
        if (*a == *b)
            return 0;

        /* Its own short name needs no new entry, only losing the long one. Fresh
         * entries could not take it anyway: the file already holds that short name. */
        fat32_short_name_to_str(src.de.name, alias);
        if (!fat32_needs_lfn(new_name) && fat32_strcasecmp(alias, new_name)) {
            if (fat32_dir_drop_long_name(vol, &src) != 0 || scache_flush(vol) != 0)
                return -1;
            return 0;
        }
        dst_rc = 0;
    }

    if (dst_rc == 1) {
        if (!(flags & VFS_RENAME_REPLACE_EXISTING))
            return -1;
        return fat32_rename_replace(vol, &src, &dst);
    }
    return fat32_rename_to_free_name(vol, dir_cluster, &src, new_name);
}

int fat32_truncate(struct fat32_volume *vol, uint32_t dir_cluster,
                    const char *name, uint32_t new_size)
{
    struct fat32_dirent_loc loc;

    if (fat32_dir_lookup(vol, dir_cluster, name, &loc) != 1)
        return -1;
    return fat32_truncate_by_sfn(vol, dir_cluster, loc.de.name, new_size);
}

int fat32_truncate_by_sfn(struct fat32_volume *vol, uint32_t dir_cluster,
                           const uint8_t sfn[11], uint32_t new_size)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = dir_cluster;
    int sk;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    /* Match the on-disk SFN: fat32_truncate resolves it from a name, and the
     * vfs ops layer passes the one cached in its fat32_file. */
    for (sk = 0; sk < 11; sk++) short_name[sk] = sfn[sk];

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
                struct fat32_dir_entry saved = *de;
                uint32_t keep_last = 0;   /* last kept cluster of a shrink, 0 = none */

                if (new_size < de->file_size && new_size > 0 && fc >= 2) {
                    uint32_t clusters_needed =
                        (new_size + bytes_per_cluster - 1) / bytes_per_cluster;
                    uint32_t ci;

                    keep_last = fc;
                    for (ci = 1; ci < clusters_needed; ci++) {
                        uint32_t next = fat32_get_fat_entry(vol, keep_last);
                        if (next < 2 || next >= FAT32_EOC) break;
                        keep_last = next;
                    }
                }

                /* The entry reaches the device BEFORE any cluster returns to the
                 * free pool, as in fat32_delete_file_vol: a failed or interrupted
                 * write then leaves the old entry over its still-allocated chain
                 * (at worst lost clusters fsck reclaims), never a live entry over
                 * clusters another file can be given. A failed flush restores
                 * the entry in the cache, so -1 means the file is unchanged. */
                if (new_size == 0) {
                    de->first_cluster_hi = 0;
                    de->first_cluster_lo = 0;
                }
                de->file_size = new_size;
                if (fat32_write_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                              cluster_buf) != 0 ||
                    scache_flush(vol) != 0) {
                    *de = saved;
                    (void)fat32_write_sectors_multi(vol, sector,
                                                    vol->bpb.sectors_per_cluster,
                                                    cluster_buf);
                    kfree(cluster_buf);
                    return -1;
                }

                if (new_size == 0) {
                    if (fc >= 2)
                        fat32_free_chain(vol, fc);
                } else if (keep_last >= 2) {
                    uint32_t tail = fat32_get_fat_entry(vol, keep_last);
                    if (tail >= 2 && tail < FAT32_EOC) {
                        fat32_set_fat_entry(vol, keep_last, 0x0FFFFFFF);
                        fat32_free_chain(vol, tail);
                    }
                }

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

/* 1 = holds nothing but its dot entries, 0 = holds something, -1 = could not be
 * read to the end. Only a complete read may call a directory empty, because the
 * answer decides whether its clusters are freed. */
static int fat32_dir_is_empty(struct fat32_volume *vol, uint32_t cluster)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint32_t steps = 0;
    uint8_t *buf = (uint8_t *)kmalloc(bytes_per_cluster);
    int result = -1;

    if (!buf)
        return -1;

    while (cluster < FAT32_EOC) {
        uint32_t i;

        if (cluster < 2 || cluster > fat32_last_cluster(vol) ||
            ++steps > fat32_last_cluster(vol) ||
            fat32_read_sectors_multi(vol, cluster_to_sector(vol, cluster),
                                     vol->bpb.sectors_per_cluster, buf) != 0)
            goto out;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            const struct fat32_dir_entry *child =
                (const struct fat32_dir_entry *)&buf[i];

            if (child->name[0] == 0x00) { result = 1; goto out; }
            if (child->name[0] == 0xE5 || child->attr == FAT32_ATTR_LFN)
                continue;
            if (child->name[0] == '.' &&
                (child->name[1] == ' ' ||
                 (child->name[1] == '.' && child->name[2] == ' ')))
                continue;
            result = 0;
            goto out;
        }
        if (fat32_next_cluster_checked(vol, cluster, &cluster) != 0)
            goto out;
    }
    result = 1;

out:
    kfree(buf);
    return result;
}

int fat32_rmdir(struct fat32_volume *vol, uint32_t parent_cluster,
                 const char *name)
{
    struct fat32_dirent_loc loc;
    uint32_t first;

    if (fat32_dir_lookup(vol, parent_cluster, name, &loc) != 1)
        return -1;
    if (!(loc.de.attr & FAT32_ATTR_DIRECTORY))
        return -1;

    first = ((uint32_t)loc.de.first_cluster_hi << 16) | loc.de.first_cluster_lo;
    if (fat32_dir_is_empty(vol, first) != 1)
        return -1;

    if (fat32_dir_remove_entries(vol, &loc) != 0)
        return -1;
    if (first >= 2)
        fat32_free_chain(vol, first);
    scache_flush(vol);
    return 0;
}

int fat32_set_attr(struct fat32_volume *vol, uint32_t dir_cluster,
                    const char *name, uint8_t new_attr)
{
    struct fat32_dirent_loc loc;
    int rc;

    if (fat32_dir_lookup(vol, dir_cluster, name, &loc) != 1)
        return -1;

    loc.de.attr = (uint8_t)((loc.de.attr & (FAT32_ATTR_DIRECTORY |
                                            FAT32_ATTR_VOLUME_ID)) |
                            (new_attr & (FAT32_ATTR_READ_ONLY |
                                         FAT32_ATTR_HIDDEN |
                                         FAT32_ATTR_SYSTEM |
                                         FAT32_ATTR_ARCHIVE)));
    rc = fat32_dir_rewrite_entry(vol, &loc);
    scache_flush(vol);
    vol->dir_file_count = 0;
    return rc;
}

int fat32_set_times(struct fat32_volume *vol, uint32_t dir_cluster,
                     const char *name, const filetime_t *ctime_p,
                     const filetime_t *mtime_p, const filetime_t *atime_p)
{
    struct fat32_dirent_loc loc;
    int tz_bias = (int)timezone_total_bias();
    int rc;

    if (fat32_dir_lookup(vol, dir_cluster, name, &loc) != 1)
        return -1;

    if (ctime_p)
        filetime_to_dos_datetime(filetime_from_unix_seconds(ctime_p->seconds), tz_bias,
                                 &loc.de.create_date, &loc.de.create_time);
    if (mtime_p)
        filetime_to_dos_datetime(filetime_from_unix_seconds(mtime_p->seconds), tz_bias,
                                 &loc.de.modify_date, &loc.de.modify_time);
    if (atime_p) {
        uint16_t unused_time;
        filetime_to_dos_datetime(filetime_from_unix_seconds(atime_p->seconds), tz_bias,
                                 &loc.de.access_date, &unused_time);
    }
    rc = fat32_dir_rewrite_entry(vol, &loc);
    scache_flush(vol);
    return rc;
}
