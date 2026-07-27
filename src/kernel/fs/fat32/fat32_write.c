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

    fat32_generate_sfn(vol, dir_cluster, name, de.name);
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

/* Read the FAT entry for `cluster`, distinguishing a real value from an
 * I/O failure: returns 0 and writes *out on success, -1 on FAT read
 * error. fat32_get_fat_entry returns FAT32_EOC on read failure, which a
 * tri-state chain walk MUST NOT confuse with a genuine end-of-chain --
 * doing so would report "absent" on a degraded FAT read and let a
 * duplicate dirent through. */
static int fat32_next_cluster_checked(struct fat32_volume *vol,
                                      uint32_t cluster, uint32_t *out)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->bpb.first_fat_sector + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    if (fat32_read_sector(vol, fat_sector, vol->sector_buf) != 0)
        return -1;
    *out = (*(uint32_t *)&vol->sector_buf[entry_offset]) & 0x0FFFFFFF;
    return 0;
}

/* Tri-state lookup of a child by name in parent_cluster.
 *   1  = a child named `name` exists,
 *   0  = it does not,
 *  -1  = the scan could not complete (alloc/read failure or a malformed
 *        FAT chain) -- i.e. absence could NOT be proven.
 * Read-only full-chain LFN-aware scan modeled on fat32_delete_file_vol.
 * Deliberately does NOT use the shared dir_files[] cache: that cache is
 * lossy (fat32_read_dir is void, silently bails on I/O/alloc failure and
 * caps at FAT32_MAX_DIR_ENTRIES) and side-effecting, so a cache-based
 * existence check could wrongly report "absent" on a degraded path and
 * let a duplicate dirent slip through. Walks the whole parent chain with
 * the same bounded FAT iteration the write paths use. Caller holds
 * vol->lock; device I/O under the lock matches the rest of the create
 * path (the tracked FAT32 locking gap). */
static int fat32_dir_find_child(struct fat32_volume *vol,
                                uint32_t parent_cluster, const char *name)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t short_name[11];
    uint32_t cur_cluster = parent_cluster;
    uint32_t spc = vol->bpb.sectors_per_cluster ? vol->bpb.sectors_per_cluster : 1u;
    uint32_t data_sectors = (vol->bpb.total_sectors > vol->bpb.first_data_sector)
                            ? (vol->bpb.total_sectors - vol->bpb.first_data_sector)
                            : 0;
    uint32_t last_valid_cluster = (data_sectors / spc) + 1u;
    uint32_t max_iter;
    uint32_t iter = 0;
    int result = 0;          /* default: not found */
    char lfn_name[FAT32_MAX_NAME];
    int lfn_active = 0;
    int lfn_expect_seq = 0;
    uint8_t lfn_chk = 0;
    int k;

    if (last_valid_cluster > 0x0FFFFFEFu) last_valid_cluster = 0x0FFFFFEFu;
    if (last_valid_cluster < 2) last_valid_cluster = 2;
    max_iter = last_valid_cluster + 1u;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;           /* cannot prove absence */

    /* An out-of-range parent means we cannot scan it -- refuse rather
     * than report a bogus "absent". */
    if (parent_cluster < 2 || parent_cluster > last_valid_cluster) {
        kfree(cluster_buf);
        return -1;
    }

    fat32_make_short_name(name, short_name);
    for (k = 0; k < FAT32_MAX_NAME; k++)
        lfn_name[k] = '\0';

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector;
        uint32_t i;

        if (++iter > max_iter || cur_cluster > last_valid_cluster) {
            result = -1;     /* malformed / cyclic chain */
            goto out;
        }
        sector = cluster_to_sector(vol, cur_cluster);
        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0) {
            result = -1;     /* read failure -- cannot prove absence */
            goto out;
        }

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            char ename[FAT32_MAX_NAME];
            int j, match;

            if (de->name[0] == 0x00)
                goto out;    /* end of directory -- not found (result=0) */
            if (de->name[0] == 0xE5) { lfn_active = 0; continue; }
            if (de->attr == FAT32_ATTR_LFN) {
                struct fat32_lfn_entry *lfn =
                    (struct fat32_lfn_entry *)&cluster_buf[i];
                int seq = lfn->seq & LFN_SEQ_MASK;
                if (lfn->seq & LFN_LAST_ENTRY) {
                    for (k = 0; k < FAT32_MAX_NAME; k++)
                        lfn_name[k] = '\0';
                    lfn_active = 1;
                    lfn_chk = lfn->checksum;
                    lfn_expect_seq = seq;
                }
                if (lfn_active &&
                    (seq != lfn_expect_seq || lfn->checksum != lfn_chk ||
                     seq < 1 || seq > FAT32_LFN_MAX_ENTRIES)) {
                    lfn_active = 0;
                } else if (lfn_active) {
                    lfn_extract_chars(lfn, lfn_name, seq - 1);
                    lfn_expect_seq = seq - 1;
                }
                continue;
            }
            if (de->attr & FAT32_ATTR_VOLUME_ID) { lfn_active = 0; continue; }

            if (lfn_active &&
                (lfn_expect_seq != 0 ||
                 fat32_lfn_checksum(de->name) != lfn_chk))
                lfn_active = 0;

            if (lfn_active && lfn_name[0] != '\0')
                fat32_strcpy(ename, lfn_name, FAT32_MAX_NAME);
            else
                fat32_short_name_to_str(de->name, ename);

            match = fat32_strcasecmp(ename, name);
            if (!match) {
                match = 1;
                for (j = 0; j < 11; j++) {
                    if (de->name[j] != short_name[j]) { match = 0; break; }
                }
            }
            lfn_active = 0;
            if (match) { result = 1; goto out; }
        }

        /* Advance with an I/O-checked FAT read so a read failure or a
         * malformed continuation cannot masquerade as a clean end of
         * chain (which would falsely prove absence). */
        {
            uint32_t next;
            if (fat32_next_cluster_checked(vol, cur_cluster, &next) != 0) {
                result = -1;            /* FAT read error */
                goto out;
            }
            if (next >= FAT32_EOC)
                goto out;               /* genuine end of chain (result=0) */
            if (next < 2) {
                result = -1;            /* FREE/reserved mid-chain == corrupt */
                goto out;
            }
            cur_cluster = next;         /* reserved/BAD caught by bound check */
        }
    }

out:
    kfree(cluster_buf);
    return result;
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
    if (fat32_dir_find_child(vol, parent_cluster, name) != 0)
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

    fat32_generate_sfn(vol, parent_cluster, name, de.name);
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
    /* Bounded FAT walk (mirrors fat32_rename_vol): this loop runs under
     * vol->lock (cli-spinlock) -- a cyclic or corrupt directory chain
     * must fail fast, never wedge the CPU with interrupts off. */
    uint32_t spc = vol->bpb.sectors_per_cluster ? vol->bpb.sectors_per_cluster : 1u;
    uint32_t data_sectors = (vol->bpb.total_sectors > vol->bpb.first_data_sector)
                            ? (vol->bpb.total_sectors - vol->bpb.first_data_sector)
                            : 0;
    uint32_t last_valid_cluster = (data_sectors / spc) + 1u;
    uint32_t max_iter;
    uint32_t iter = 0;

    if (last_valid_cluster > 0x0FFFFFEFu) last_valid_cluster = 0x0FFFFFEFu;
    if (last_valid_cluster < 2) last_valid_cluster = 2;
    max_iter = last_valid_cluster + 1u;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    fat32_make_short_name(name, short_name);

    /* LFN-aware scan (mirrors the stat/open lookup): accumulate long-name
     * chars across entries so an LFN file ("random-seed.new") matches by
     * its real name -- the generated short alias (RANDOM~1.NEW numeric
     * tail) is unpredictable from the long name, so the old SFN-only
     * match could never delete LFN files. */
    {
        char lfn_name[FAT32_MAX_NAME];
        int lfn_active = 0;
        uint32_t lfn_start_cluster = 0;
        uint8_t lfn_chk = 0;
        int lfn_expect_seq = 0;
        int k;

        for (k = 0; k < FAT32_MAX_NAME; k++)
            lfn_name[k] = '\0';

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector;
        uint32_t i;

        if (++iter > max_iter || cur_cluster > last_valid_cluster) {
            klog(LOG_WARN, "fat32",
                 "delete: FAT walk exceeded bounds (iter=%u cur=%u last=%u)",
                 (uint64_t)iter, (uint64_t)cur_cluster,
                 (uint64_t)last_valid_cluster);
            goto del_not_found;
        }
        sector = cluster_to_sector(vol, cur_cluster);

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            char ename[FAT32_MAX_NAME];
            int j, match;

            if (de->name[0] == 0x00) goto del_not_found;
            if (de->name[0] == 0xE5) { lfn_active = 0; continue; }
            if (de->attr == FAT32_ATTR_LFN) {
                struct fat32_lfn_entry *lfn =
                    (struct fat32_lfn_entry *)&cluster_buf[i];
                int seq = lfn->seq & LFN_SEQ_MASK;
                if (lfn->seq & LFN_LAST_ENTRY) {
                    for (k = 0; k < FAT32_MAX_NAME; k++)
                        lfn_name[k] = '\0';
                    lfn_active = 1;
                    lfn_start_cluster = cur_cluster;
                    lfn_chk = lfn->checksum;
                    lfn_expect_seq = seq;
                }
                /* Run integrity: descending contiguous sequence, one
                 * checksum across every slot. A crafted directory must
                 * not be able to alias an arbitrary long name onto an
                 * unrelated SFN (this scan DELETES on match). */
                if (lfn_active &&
                    (seq != lfn_expect_seq || lfn->checksum != lfn_chk ||
                     seq < 1 || seq > FAT32_LFN_MAX_ENTRIES)) {
                    lfn_active = 0;
                } else if (lfn_active) {
                    lfn_extract_chars(lfn, lfn_name, seq - 1);
                    lfn_expect_seq = seq - 1;
                }
                continue;
            }
            if (de->attr & FAT32_ATTR_VOLUME_ID) { lfn_active = 0; continue; }

            /* The accumulated LFN run binds to THIS SFN only when it is
             * complete (ran down to seq 1) and its checksum matches the
             * checksum derived from the SFN that follows it. */
            if (lfn_active &&
                (lfn_expect_seq != 0 ||
                 fat32_lfn_checksum(de->name) != lfn_chk))
                lfn_active = 0;

            /* Match by long name when one is present, else by 8.3; a
             * direct short-alias match is accepted either way (callers
             * may pass the alias). */
            if (lfn_active && lfn_name[0] != '\0')
                fat32_strcpy(ename, lfn_name, FAT32_MAX_NAME);
            else
                fat32_short_name_to_str(de->name, ename);

            match = fat32_strcasecmp(ename, name);
            if (!match) {
                match = 1;
                for (j = 0; j < 11; j++) {
                    if (de->name[j] != short_name[j]) { match = 0; break; }
                }
            }

            if (match && lfn_active && lfn_start_cluster != cur_cluster) {
                /* The LFN run begins in a prior directory cluster; the
                 * backward slot wipe below only walks this cluster, so a
                 * stale LFN tail would survive and could bind to a future
                 * SFN with the same checksum. Same refusal contract as
                 * rename replace-existing; cross-cluster LFN removal is
                 * the tracked follow-up there. */
                klog(LOG_WARN, "fat32",
                     "delete: LFN run crosses directory cluster boundary; "
                     "refused");
                goto del_not_found;
            }
            lfn_active = 0;

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
    }

del_not_found:
    kfree(cluster_buf);
    return -1;
}

/* Locate the SFN-matching dirent in this directory.  Bounded by
 * total_sectors / sectors_per_cluster iterations so a corrupted FAT
 * cycle (cluster pointing back at itself or earlier) cannot hang under
 * vol->lock.  Validates each next-cluster against the cluster range
 * derived from BPB before passing it to cluster_to_sector.
 * Returns 1 on hit, 0 on terminator-without-match, -1 on read error or
 * malformed FAT. */
static int fat32_find_dirent_by_sfn(struct fat32_volume *vol,
                                    uint32_t dir_cluster,
                                    const uint8_t sfn[11],
                                    uint32_t *out_cluster,
                                    uint32_t *out_sector,
                                    uint32_t *out_offset,
                                    struct fat32_dir_entry *out_de)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t cur_cluster = dir_cluster;
    uint32_t spc = vol->bpb.sectors_per_cluster ? vol->bpb.sectors_per_cluster : 1u;
    uint32_t data_sectors = (vol->bpb.total_sectors > vol->bpb.first_data_sector)
                            ? (vol->bpb.total_sectors - vol->bpb.first_data_sector)
                            : 0;
    uint32_t last_valid_cluster = (data_sectors / spc) + 1u;
    /* FAT32 reserved range starts at 0x0FFFFFF0 (bad/EOC markers). The
     * BPB validator caps the maximum addressable data cluster at
     * 0x0FFFFFEF; mirror that here so reserved markers cannot slip
     * through cluster_to_sector. */
    if (last_valid_cluster > 0x0FFFFFEFu) last_valid_cluster = 0x0FFFFFEFu;
    if (last_valid_cluster < 2) last_valid_cluster = 2;
    uint32_t max_iter = last_valid_cluster + 1u;
    uint32_t iter = 0;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    while (cur_cluster >= 2 && cur_cluster < FAT32_EOC) {
        uint32_t sector;
        uint32_t i;

        if (++iter > max_iter || cur_cluster > last_valid_cluster) {
            klog(LOG_WARN, "fat32",
                 "rename: FAT walk exceeded bounds (iter=%u cur=%u last=%u)",
                 (uint64_t)iter, (uint64_t)cur_cluster,
                 (uint64_t)last_valid_cluster);
            kfree(cluster_buf);
            return -1;
        }

        sector = cluster_to_sector(vol, cur_cluster);
        if (fat32_read_sectors_multi(vol, sector,
                                     vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            int j, match;

            if (de->name[0] == 0x00) {
                kfree(cluster_buf);
                return 0;
            }
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            match = 1;
            for (j = 0; j < 11; j++) {
                if (de->name[j] != sfn[j]) { match = 0; break; }
            }
            if (match) {
                *out_cluster = cur_cluster;
                *out_sector  = sector;
                *out_offset  = i;
                if (out_de) *out_de = *de;
                kfree(cluster_buf);
                return 1;
            }
        }

        cur_cluster = fat32_get_fat_entry(vol, cur_cluster);
    }

    kfree(cluster_buf);
    return 0;
}

/* Zero the SFN dirent at (sector, offset) AND walk LFN slots backwards
 * marking them 0xE5.  Reads + rewrites the cluster_buf in place; caller
 * is responsible for the surrounding read+flush.  Returns 0 on success,
 * -1 on read failure. */
static int fat32_remove_dirent_in_buf(uint8_t *cluster_buf, uint32_t offset,
                                      uint32_t bytes_per_cluster)
{
    struct fat32_dir_entry *de =
        (struct fat32_dir_entry *)&cluster_buf[offset];
    uint8_t chk;
    int pos;

    chk = fat32_lfn_checksum(de->name);
    cluster_buf[offset] = 0xE5;
    pos = (int)offset - 32;
    while (pos >= 0) {
        struct fat32_lfn_entry *lfn =
            (struct fat32_lfn_entry *)&cluster_buf[pos];
        if (lfn->attr != FAT32_ATTR_LFN) break;
        if (lfn->checksum != chk) break;
        cluster_buf[pos] = 0xE5;
        pos -= 32;
    }
    (void)bytes_per_cluster;
    return 0;
}

int fat32_rename_vol(struct fat32_volume *vol, uint32_t dir_cluster,
                      const char *old_name, const char *new_name,
                      uint32_t flags)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint8_t old_short[11], new_short[11];
    uint32_t src_cluster = 0, src_sector = 0, src_offset = 0;
    int rc;
    int j, same_sfn;

    fat32_make_short_name(old_name, old_short);
    fat32_make_short_name(new_name, new_short);

    /* Locate source dirent FIRST -- POSIX rename(2) returns ENOENT when
     * the source path doesn't exist, regardless of whether old==new. */
    rc = fat32_find_dirent_by_sfn(vol, dir_cluster, old_short,
                                  &src_cluster, &src_sector, &src_offset, NULL);
    if (rc != 1) {
        klog(LOG_WARN, "fat32",
             "rename: source '%s' not found (sfn lookup rc=%d)", old_name, (uint64_t)rc);
        return -1;
    }

    /* POSIX same-file no-op: src exists and resolves to the same SFN as
     * dst -- treat as success without touching the FAT. */
    same_sfn = 1;
    for (j = 0; j < 11; j++) {
        if (old_short[j] != new_short[j]) { same_sfn = 0; break; }
    }
    if (same_sfn)
        return 0;

    /* Replace-existing path: remove dst dirent + LFN slots, flush, free
     * dst chain, flush, then rename src.  Order chosen so a mid-sequence
     * write failure cannot leave a live dirent pointing at clusters that
     * have already been returned to the free pool. */
    if (flags & VFS_RENAME_REPLACE_EXISTING) {
        uint32_t dst_cluster = 0, dst_sector = 0, dst_offset = 0;
        struct fat32_dir_entry dst_de;
        int dst_rc = fat32_find_dirent_by_sfn(vol, dir_cluster, new_short,
                                              &dst_cluster, &dst_sector,
                                              &dst_offset, &dst_de);
        if (dst_rc < 0) {
            klog(LOG_WARN, "fat32",
                 "rename: dst '%s' lookup failed (rc=%d)", new_name, (uint64_t)dst_rc);
            return -1;
        }

        if (dst_rc == 1) {
            uint32_t dst_first;

            /* Refuse if dst is a directory: would orphan its tree. */
            if (dst_de.attr & FAT32_ATTR_DIRECTORY) {
                klog(LOG_WARN, "fat32",
                     "rename replace-existing: dst is a directory; refused");
                return -1;
            }

            /* Refuse rename onto same dirent slot (caller-paths can hand
             * us collision-equal SFNs from differing LFN inputs). */
            if (dst_cluster == src_cluster && dst_offset == src_offset)
                return 0;

            /* Refuse all non-first-cluster destinations.  An LFN chain can
             * be up to FAT32_LFN_MAX_ENTRIES * 32 bytes long and can
             * extend back into the prior cluster from any offset, not
             * just offset 0.  fat32_remove_dirent_in_buf only walks
             * within the current cluster_buf, so a stale LFN tail can
             * survive in the prior cluster and bind to a different SFN.
             * Cross-cluster LFN removal lands in a follow-up item; for
             * now refuse to keep the on-disk contract bulletproof.  The
             * first cluster of the directory chain has no prior cluster,
             * so any offset there is safe. */
            if (dst_cluster != dir_cluster) {
                klog(LOG_WARN, "fat32",
                     "rename replace-existing: dst in non-first directory "
                     "cluster (cluster=%u offset=%u); cross-cluster LFN "
                     "removal not implemented; refused",
                     (uint64_t)dst_cluster, (uint64_t)dst_offset);
                return -1;
            }

            cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
            if (!cluster_buf) {
                klog(LOG_WARN, "fat32",
                     "rename: OOM for %u-byte cluster buffer (replace path)",
                     (uint64_t)bytes_per_cluster);
                return -1;
            }

            /* Step 1: remove dst dirent + LFN slots in that cluster, flush. */
            if (fat32_read_sectors_multi(vol, dst_sector,
                                         vol->bpb.sectors_per_cluster,
                                         cluster_buf) != 0) {
                klog(LOG_WARN, "fat32",
                     "rename: dst cluster read failed (sector=%u)", (uint64_t)dst_sector);
                kfree(cluster_buf);
                return -1;
            }
            (void)fat32_remove_dirent_in_buf(cluster_buf, dst_offset,
                                             bytes_per_cluster);
            if (fat32_write_sectors_multi(vol, dst_sector,
                                          vol->bpb.sectors_per_cluster,
                                          cluster_buf) != 0) {
                klog(LOG_WARN, "fat32",
                     "rename: dst cluster write failed (sector=%u)", (uint64_t)dst_sector);
                kfree(cluster_buf);
                return -1;
            }
            scache_flush(vol);

            /* Step 2: free dst's cluster chain (now unreferenced), flush. */
            dst_first = ((uint32_t)dst_de.first_cluster_hi << 16)
                      | (uint32_t)dst_de.first_cluster_lo;
            if (dst_first >= 2)
                fat32_free_chain(vol, dst_first);
            scache_flush(vol);

            kfree(cluster_buf);
        }
    }

    /* Step 3: rename src dirent (rewrite the 11-byte SFN field in place). */
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf) {
        klog(LOG_WARN, "fat32", "rename: OOM for %u-byte cluster buffer (sfn rewrite)",
             (uint64_t)bytes_per_cluster);
        return -1;
    }
    if (fat32_read_sectors_multi(vol, src_sector,
                                 vol->bpb.sectors_per_cluster,
                                 cluster_buf) != 0) {
        klog(LOG_WARN, "fat32", "rename: src cluster read failed (sector=%u)",
             (uint64_t)src_sector);
        kfree(cluster_buf);
        return -1;
    }
    {
        struct fat32_dir_entry *de =
            (struct fat32_dir_entry *)&cluster_buf[src_offset];
        for (j = 0; j < 11; j++) de->name[j] = new_short[j];
    }
    if (fat32_write_sectors_multi(vol, src_sector,
                                  vol->bpb.sectors_per_cluster,
                                  cluster_buf) != 0) {
        klog(LOG_WARN, "fat32", "rename: src cluster write failed (sector=%u)",
             (uint64_t)src_sector);
        kfree(cluster_buf);
        return -1;
    }
    scache_flush(vol);

    vol->dir_file_count = 0;
    kfree(cluster_buf);
    return 0;
}

int fat32_truncate(struct fat32_volume *vol, uint32_t dir_cluster,
                    const char *name, uint32_t new_size)
{
    uint8_t short_name[11];
    fat32_make_short_name(name, short_name);
    return fat32_truncate_by_sfn(vol, dir_cluster, short_name, new_size);
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

    /* Match against the caller-provided SFN. The legacy entry above
     * derives it via fat32_make_short_name when the caller has only a
     * name; modern callers (vfs ops layer with a cached fat32_file)
     * pass the on-disk SFN directly because lowercase-8.3 inputs like
     * "postcode.log" become "POSTCO~1LOG" via fat32_generate_sfn's
     * unconditional ~1 suffix for LFN-required names. */
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
