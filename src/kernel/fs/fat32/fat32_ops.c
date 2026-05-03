/* ============================================================================
 * fat32_ops.c -- VFS operations, stat, init, driver registration
 *
 * VFS ops tables (file + directory), all VFS wrapper functions,
 * fat32_stat (path-based metadata lookup), fat32_init, fat32_get_root,
 * and fat32_get_driver.
 *
 * All VFS wrappers extract the fat32_volume via:
 *   struct fat32_file *f = (struct fat32_file *)node->fs_data;
 *   struct fat32_volume *vol = f->volume;
 * ============================================================================ */

#include "fat32_internal.h"

/* ---- Forward declarations for VFS ops tables ---- */

static int fat32_file_open(struct vfs_node *node, uint32_t flags);
static int fat32_file_close(struct vfs_node *node);
static int fat32_file_read(struct vfs_node *node, uint32_t offset,
                           uint32_t size, uint8_t *buffer);
static int fat32_file_write_vfs(struct vfs_node *node, uint32_t offset,
                                uint32_t size, const uint8_t *buffer);
static struct vfs_dirent *fat32_readdir(struct vfs_node *node, uint32_t index);
static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name);
static int fat32_vfs_create(struct vfs_node *parent, const char *name,
                            uint8_t type);
static int fat32_vfs_unlink(struct vfs_node *parent, const char *name);
static int fat32_vfs_rename(struct vfs_node *parent,
                            const char *old_name, const char *new_name);
static int fat32_vfs_stat(struct vfs_node *node, struct vfs_stat *st);
static int fat32_vfs_truncate(struct vfs_node *node, uint64_t new_size);
static int fat32_vfs_mkdir(struct vfs_node *parent, const char *name);
static int fat32_vfs_rmdir(struct vfs_node *parent, const char *name);
static int fat32_vfs_set_attr(struct vfs_node *node, uint32_t attributes);
static int fat32_vfs_set_times(struct vfs_node *node,
                               const filetime_t *ctime_p,
                               const filetime_t *mtime_p,
                               const filetime_t *atime_p);
static int fat32_vfs_flush(struct vfs_node *node);

/* Helper: extract volume from a VFS node */
static struct fat32_volume *vol_from_node(struct vfs_node *node)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    return f ? f->volume : (struct fat32_volume *)0;
}

/* Helper: extract dir_cluster from a parent VFS node */
static uint32_t dir_cluster_from_node(struct vfs_node *node)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    struct fat32_volume *vol = f ? f->volume : (struct fat32_volume *)0;
    if (f && f->first_cluster >= 2)
        return f->first_cluster;
    return vol ? vol->bpb.root_cluster : 2;
}

/* ---- VFS ops tables ---- */

struct vfs_ops fat32_file_ops = {
    .open    = fat32_file_open,
    .close   = fat32_file_close,
    .read    = fat32_file_read,
    .write   = fat32_file_write_vfs,
    .readdir = (void *)0,
    .finddir = (void *)0,
    .create  = (void *)0,
    .unlink  = (void *)0,
    .rename  = (void *)0,
    .stat    = fat32_vfs_stat,
    .truncate = fat32_vfs_truncate,
    .mkdir   = (void *)0,
    .rmdir   = (void *)0,
    .set_attr = (void *)0,
    .set_times = (void *)0,
    .flush   = fat32_vfs_flush,
};

struct vfs_ops fat32_dir_ops = {
    .open    = fat32_file_open,
    .close   = (void *)0,
    .read    = (void *)0,
    .write   = (void *)0,
    .readdir = fat32_readdir,
    .finddir = fat32_finddir,
    .create  = fat32_vfs_create,
    .unlink  = fat32_vfs_unlink,
    .rename  = fat32_vfs_rename,
    .stat    = fat32_vfs_stat,
    .truncate = (void *)0,
    .mkdir   = fat32_vfs_mkdir,
    .rmdir   = fat32_vfs_rmdir,
    .set_attr = fat32_vfs_set_attr,
    .set_times = fat32_vfs_set_times,
    .flush   = fat32_vfs_flush,
};

static struct vfs_fs_driver fat32_driver = {
    .name      = "FAT32",
    .ops       = &fat32_dir_ops,
    .priv_data = (void *)0,
};

/* ---- File operations ---- */

static int fat32_file_open(struct vfs_node *node, uint32_t flags)
{
    (void)node;
    (void)flags;
    return 0;
}

static int fat32_file_close(struct vfs_node *node)
{
    struct fat32_volume *vol = vol_from_node(node);
    if (vol) {
        spin_lock(&vol->lock);
        scache_flush(vol);
        fat32_fsinfo_flush(vol);
        spin_unlock(&vol->lock);
    }
    return 0;
}

static int fat32_file_read(struct vfs_node *node, uint32_t offset,
                           uint32_t size, uint8_t *buffer)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    struct fat32_volume *vol;
    uint32_t cluster;
    uint32_t bytes_per_cluster;
    uint32_t bytes_read = 0;
    uint32_t cluster_offset;
    uint32_t to_read;
    uint8_t *cluster_buf;

    if (!f) return 0;
    vol = f->volume;
    if (!vol || offset >= f->file_size)
        return 0;

    if (offset + size > f->file_size)
        size = f->file_size - offset;

    bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    cluster = f->first_cluster;
    {
        uint32_t skip = offset / bytes_per_cluster;
        uint32_t s;
        for (s = 0; s < skip && cluster < FAT32_EOC; s++)
            cluster = fat32_next_cluster(vol, cluster);
    }
    cluster_offset = offset % bytes_per_cluster;

    while (bytes_read < size && cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t sector = cluster_to_sector(vol, cluster);
        uint32_t ci;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        to_read = bytes_per_cluster - cluster_offset;
        if (to_read > size - bytes_read)
            to_read = size - bytes_read;

        for (ci = 0; ci < to_read; ci++)
            buffer[bytes_read + ci] = cluster_buf[cluster_offset + ci];

        bytes_read += to_read;
        cluster_offset = 0;
        cluster = fat32_next_cluster(vol, cluster);
    }

    kfree(cluster_buf);
    return (int)bytes_read;
}

static int fat32_file_write_vfs(struct vfs_node *node, uint32_t offset,
                                uint32_t size, const uint8_t *buffer)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    struct fat32_volume *vol;
    uint32_t bytes_per_cluster;
    uint32_t cluster;
    uint32_t bytes_written = 0;
    uint32_t cluster_offset;
    uint8_t *cluster_buf;

    if (!f || !buffer || size == 0)
        return -1;

    /* FAT32 4 GiB file size guard -- directory entry file_size is 32-bit */
    if ((uint64_t)offset + (uint64_t)size > 0xFFFFFFFF) {
        klog(LOG_WARN, "fat32", "4 GiB file size limit: %s", node->name);
        return -1;
    }

    vol = f->volume;
    if (!vol)
        return -1;

    bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    spin_lock(&vol->lock);

    cluster = f->first_cluster;
    if (cluster < 2 || cluster >= FAT32_EOC) {
        cluster = fat32_alloc_cluster(vol);
        if (cluster == 0) {
            spin_unlock(&vol->lock);
            kfree(cluster_buf);
            return -1;
        }
        fat32_zero_cluster(vol, cluster);
        f->first_cluster = cluster;
        node->inode = cluster;

        {
            uint32_t bpc = vol->bpb.sectors_per_cluster * 512;
            uint8_t *dbuf = (uint8_t *)kmalloc(bpc);
            if (dbuf) {
                uint32_t dc = f->dir_cluster;
                /* Match dirent by cached on-disk SFN; fat32_make_short_name
                 * misses the ~N collision suffix fat32_generate_sfn writes
                 * for LFN-required (lowercase 8.3) names. */
                const uint8_t *short_name = f->sfn;

                while (dc >= 2 && dc < FAT32_EOC) {
                    uint32_t sec = cluster_to_sector(vol, dc);
                    uint32_t di;
                    if (fat32_read_sectors_multi(vol, sec,
                            vol->bpb.sectors_per_cluster, dbuf) != 0)
                        break;
                    for (di = 0; di < bpc; di += 32) {
                        struct fat32_dir_entry *de =
                            (struct fat32_dir_entry *)&dbuf[di];
                        int j, match;
                        if (de->name[0] == 0x00) goto fc_done;
                        if (de->name[0] == 0xE5) continue;
                        if (de->attr == FAT32_ATTR_LFN) continue;
                        match = 1;
                        for (j = 0; j < 11; j++) {
                            if (de->name[j] != short_name[j]) {
                                match = 0; break;
                            }
                        }
                        if (match) {
                            de->first_cluster_hi = (uint16_t)(cluster >> 16);
                            de->first_cluster_lo = (uint16_t)(cluster & 0xFFFF);
                            fat32_write_sectors_multi(vol, sec,
                                vol->bpb.sectors_per_cluster, dbuf);
                            goto fc_done;
                        }
                    }
                    dc = fat32_get_fat_entry(vol, dc);
                }
fc_done:
                kfree(dbuf);
            }
        }
    }

    {
        uint32_t skip = offset / bytes_per_cluster;
        uint32_t s;
        for (s = 0; s < skip; s++) {
            uint32_t next = fat32_next_cluster(vol, cluster);
            if (next < 2 || next >= FAT32_EOC) {
                next = fat32_alloc_cluster(vol);
                if (next == 0) {
                    spin_unlock(&vol->lock);
                    kfree(cluster_buf);
                    return -1;
                }
                fat32_zero_cluster(vol, next);
                fat32_set_fat_entry(vol, cluster, next);
            }
            cluster = next;
        }
    }
    cluster_offset = offset % bytes_per_cluster;

    while (bytes_written < size) {
        uint32_t sector = cluster_to_sector(vol, cluster);
        uint32_t to_write;
        uint32_t ci;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        to_write = bytes_per_cluster - cluster_offset;
        if (to_write > size - bytes_written)
            to_write = size - bytes_written;

        for (ci = 0; ci < to_write; ci++)
            cluster_buf[cluster_offset + ci] = buffer[bytes_written + ci];

        if (fat32_write_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                      cluster_buf) != 0)
            break;

        bytes_written += to_write;
        cluster_offset = 0;

        if (bytes_written < size) {
            uint32_t next = fat32_next_cluster(vol, cluster);
            if (next < 2 || next >= FAT32_EOC) {
                next = fat32_alloc_cluster(vol);
                if (next == 0)
                    break;
                fat32_zero_cluster(vol, next);
                fat32_set_fat_entry(vol, cluster, next);
            }
            cluster = next;
        }
    }

    {
        uint32_t end_pos = offset + bytes_written;
        if (end_pos > f->file_size) {
            f->file_size = end_pos;
            node->size = end_pos;
            fat32_update_dir_size(vol, f->dir_cluster, f->first_cluster,
                                   end_pos);
        }
    }

    vol->dir_file_count = 0;
    spin_unlock(&vol->lock);

    kfree(cluster_buf);
    return (int)bytes_written;
}

/* ---- Directory operations ---- */

static struct vfs_dirent *fat32_readdir(struct vfs_node *node, uint32_t index)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    struct fat32_volume *vol = f ? f->volume : (struct fat32_volume *)0;
    uint32_t dir_cluster;

    if (!vol)
        return (struct vfs_dirent *)0;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : vol->bpb.root_cluster;

    if (vol->dir_file_count == 0 || vol->dir_cached_cluster != dir_cluster) {
        vol->dir_file_count = 0;
        fat32_read_dir(vol, dir_cluster);
        vol->dir_cached_cluster = dir_cluster;
    }

    if (index >= vol->dir_file_count)
        return (struct vfs_dirent *)0;

    fat32_strcpy(vol->dirent.name, vol->dir_files[index].node.name,
                  VFS_MAX_NAME);
    vol->dirent.inode = vol->dir_files[index].node.inode;
    vol->dirent.type = vol->dir_files[index].node.type;

    return &vol->dirent;
}

static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    struct fat32_volume *vol = f ? f->volume : (struct fat32_volume *)0;
    uint32_t dir_cluster;
    uint32_t i;

    if (!vol)
        return (struct vfs_node *)0;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : vol->bpb.root_cluster;

    /* Re-read if cache is empty or holds a different directory */
    if (vol->dir_file_count == 0 || vol->dir_cached_cluster != dir_cluster) {
        vol->dir_file_count = 0;
        fat32_read_dir(vol, dir_cluster);
        vol->dir_cached_cluster = dir_cluster;
    }

    for (i = 0; i < vol->dir_file_count; i++) {
        if (fat32_strcasecmp(vol->dir_files[i].node.name, name)) {
            if (vol->dir_files[i].node.type & VFS_DIRECTORY)
                vol->dir_files[i].node.ops = &fat32_dir_ops;
            else
                vol->dir_files[i].node.ops = &fat32_file_ops;
            vol->dir_files[i].node.fs_data = &vol->dir_files[i];
            vol->dir_files[i].dir_cluster = dir_cluster;
            vol->dir_files[i].volume = vol;
            return &vol->dir_files[i].node;
        }
    }

    return (struct vfs_node *)0;
}

/* ---- VFS wrapper functions ---- */

static int fat32_vfs_create(struct vfs_node *parent, const char *name,
                            uint8_t type)
{
    struct fat32_volume *vol = vol_from_node(parent);
    uint32_t dir_cluster = dir_cluster_from_node(parent);
    int rc;

    if (!vol) return -1;

    spin_lock(&vol->lock);
    if (type & VFS_DIRECTORY)
        rc = fat32_create_dir_vol(vol, dir_cluster, name);
    else
        rc = fat32_create_file_vol(vol, dir_cluster, name);
    if (rc == 0) {
        /* Invalidate the dir cache (parent walks during vfs_create may
         * have repopulated dir_cached_cluster with a sibling's cluster;
         * cheapest correct fix is unconditional). */
        vol->dir_file_count = 0;
        vol->dir_cached_cluster = 0;
        /* Flush scache so the new dirent reaches disk before any
         * subsequent fat32_read_sectors_multi (which bypasses scache
         * via direct blkdev_read). Without this, vfs_open's
         * VFS_O_CREATE re-walk reads stale on-disk sectors and misses
         * the freshly-created file. */
        scache_flush(vol);
    }
    spin_unlock(&vol->lock);
    return rc;
}

static int fat32_vfs_unlink(struct vfs_node *parent, const char *name)
{
    struct fat32_volume *vol = vol_from_node(parent);
    uint32_t dir_cluster = dir_cluster_from_node(parent);
    int rc;

    if (!vol) return -1;
    spin_lock(&vol->lock);
    rc = fat32_delete_file_vol(vol, dir_cluster, name);
    if (rc == 0) {
        vol->dir_file_count = 0;
        vol->dir_cached_cluster = 0;
        scache_flush(vol);
    }
    spin_unlock(&vol->lock);
    return rc;
}

static int fat32_vfs_rename(struct vfs_node *parent,
                            const char *old_name, const char *new_name)
{
    struct fat32_volume *vol = vol_from_node(parent);
    uint32_t dir_cluster = dir_cluster_from_node(parent);
    int rc;

    if (!vol) return -1;
    spin_lock(&vol->lock);
    rc = fat32_rename_vol(vol, dir_cluster, old_name, new_name);
    spin_unlock(&vol->lock);
    return rc;
}

static int fat32_vfs_stat(struct vfs_node *node, struct vfs_stat *st)
{
    struct fat32_file *f;
    if (!node || !st)
        return -1;

    f = (struct fat32_file *)node->fs_data;
    st->size   = f ? (uint64_t)f->file_size : 0;
    st->type   = node->type;
    st->ctime  = 0;
    st->mtime  = 0;
    st->atime  = 0;
    st->blocks = 0;
    return 0;
}

static int fat32_vfs_truncate(struct vfs_node *node, uint64_t new_size)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    struct fat32_volume *vol = f ? f->volume : (struct fat32_volume *)0;
    int rc;

    if (!vol) return -1;
    if (node->type & VFS_DIRECTORY)
        return -1;

    /* FAT32 4 GiB file size guard */
    if (new_size > 0xFFFFFFFF) {
        klog(LOG_WARN, "fat32", "4 GiB file size limit: %s", node->name);
        return -1;
    }

    if (node->name[0]) {
        /* Use the file's actual parent dir cluster + cached on-disk
         * SFN. Lowercase 8.3 names ("postcode.log") get a ~1 collision
         * suffix from fat32_generate_sfn at create time; the cached
         * sfn[] is the only authoritative match key. */
        uint32_t dir_cluster = (f->dir_cluster >= 2)
                             ? f->dir_cluster : vol->bpb.root_cluster;
        spin_lock(&vol->lock);
        rc = fat32_truncate_by_sfn(vol, dir_cluster, f->sfn,
                                   (uint32_t)new_size);
        if (rc == 0) {
            /* Refresh cached state: post-truncate writes consult
             * f->file_size + f->first_cluster directly. Without this
             * a truncate-on-open + short write would skip the dirent
             * update because end_pos <= stale f->file_size. */
            f->file_size = (uint32_t)new_size;
            if (new_size == 0)
                f->first_cluster = 0;
            node->size = new_size;
            scache_flush(vol);
        }
        spin_unlock(&vol->lock);
        return rc;
    }

    return -1;
}

static int fat32_vfs_mkdir(struct vfs_node *parent, const char *name)
{
    struct fat32_volume *vol = vol_from_node(parent);
    uint32_t dir_cluster = dir_cluster_from_node(parent);
    int rc;

    if (!vol) return -1;
    spin_lock(&vol->lock);
    rc = fat32_create_dir_vol(vol, dir_cluster, name);
    spin_unlock(&vol->lock);
    return rc;
}

static int fat32_vfs_rmdir(struct vfs_node *parent, const char *name)
{
    struct fat32_volume *vol = vol_from_node(parent);
    uint32_t dir_cluster = dir_cluster_from_node(parent);
    int rc;

    if (!vol) return -1;
    spin_lock(&vol->lock);
    rc = fat32_rmdir(vol, dir_cluster, name);
    spin_unlock(&vol->lock);
    return rc;
}

static int fat32_vfs_set_attr(struct vfs_node *node, uint32_t attributes)
{
    struct fat32_volume *vol = vol_from_node(node);
    int rc;

    if (!vol || !node->name[0]) return -1;
    spin_lock(&vol->lock);
    rc = fat32_set_attr(vol, vol->bpb.root_cluster, node->name,
                        (uint8_t)attributes);
    spin_unlock(&vol->lock);
    return rc;
}

static int fat32_vfs_set_times(struct vfs_node *node,
                               const filetime_t *ctime_p,
                               const filetime_t *mtime_p,
                               const filetime_t *atime_p)
{
    struct fat32_volume *vol = vol_from_node(node);
    int rc;

    if (!vol || !node->name[0]) return -1;
    spin_lock(&vol->lock);
    rc = fat32_set_times(vol, vol->bpb.root_cluster, node->name,
                         ctime_p, mtime_p, atime_p);
    spin_unlock(&vol->lock);
    return rc;
}

static int fat32_vfs_flush(struct vfs_node *node)
{
    struct fat32_volume *vol = vol_from_node(node);
    int rc;
    if (!vol) return -1;
    spin_lock(&vol->lock);
    fat32_fsinfo_flush(vol);
    rc = fat32_flush_disk(vol);
    spin_unlock(&vol->lock);
    return rc;
}

/* ---- fat32_stat: file metadata lookup ---- */

int fat32_stat(struct fat32_volume *vol, const char *path,
               struct fat32_stat_info *info)
{
    uint32_t cluster;
    uint32_t bytes_per_cluster;
    uint8_t *cluster_buf;
    uint32_t i;
    const char *component;
    const char *next;
    int found;

    if (!vol || !info || !path)
        return -1;

    cluster = vol->bpb.root_cluster;
    bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;

    while (*path == '/' || *path == '\\')
        path++;

    if (*path == '\0') {
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

        next = component;
        while (*next && *next != '/' && *next != '\\')
            next++;
        while (component < next && ti < FAT32_MAX_NAME - 1)
            target[ti++] = *component++;
        target[ti] = '\0';

        while (*component == '/' || *component == '\\')
            component++;

        found = 0;
        {
            uint32_t cur_cluster = cluster;
            char lfn_name[FAT32_MAX_NAME];
            int lfn_active_s = 0;
            int k;

            for (k = 0; k < FAT32_MAX_NAME; k++)
                lfn_name[k] = '\0';

            while (cur_cluster < FAT32_EOC && cur_cluster != FAT32_FREE) {
                uint32_t sector = cluster_to_sector(vol, cur_cluster);

                if (fat32_read_sectors_multi(vol, sector,
                        vol->bpb.sectors_per_cluster, cluster_buf) != 0)
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

                    if (lfn_active_s && lfn_name[0] != '\0')
                        fat32_strcpy(ename, lfn_name, FAT32_MAX_NAME);
                    else
                        fat32_short_name_to_str(de->name, ename);
                    lfn_active_s = 0;

                    if (fat32_strcasecmp(ename, target)) {
                        uint32_t fc = ((uint32_t)de->first_cluster_hi << 16)
                                    | (uint32_t)de->first_cluster_lo;

                        if (*component == '\0') {
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

                        if (!(de->attr & FAT32_ATTR_DIRECTORY))
                            goto stat_not_found;

                        cluster = fc;
                        found = 1;
                        goto next_component;
                    }
                }

                cur_cluster = fat32_next_cluster(vol, cur_cluster);
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

struct fat32_volume *fat32_init(const struct blkdev *dev)
{
    struct fat32_volume *vol;
    uint32_t root_dir_sectors;
    uint32_t pages_needed;

    if (!dev) {
        klog(LOG_ERROR, "fat32", "FAT32: null block device");
        return (struct fat32_volume *)0;
    }

    /* Allocate volume via PMM -- ~40 KB, too large for kmalloc */
    pages_needed = (sizeof(struct fat32_volume) + 4095) / 4096;
    vol = (struct fat32_volume *)pmm_alloc_contiguous(pages_needed);
    if (!vol) {
        klog(LOG_ERROR, "fat32", "FAT32: cannot allocate volume (%u pages)",
               (uint64_t)pages_needed);
        return (struct fat32_volume *)0;
    }

    /* Zero the volume struct */
    {
        uint8_t *p = (uint8_t *)vol;
        uint32_t sz = pages_needed * 4096;
        uint32_t i;
        for (i = 0; i < sz; i++)
            p[i] = 0;
    }

    vol->dev = dev;
    vol->lock = (spinlock_t)SPINLOCK_INIT;

    /* Read the boot sector */
    if (fat32_read_sector(vol, 0, vol->sector_buf) != 0) {
        klog(LOG_ERROR, "fat32", "FAT32: cannot read boot sector");
        goto fail;
    }

    if (vol->sector_buf[510] != 0x55 || vol->sector_buf[511] != 0xAA) {
        klog(LOG_ERROR, "fat32", "FAT32: invalid boot signature");
        goto fail;
    }

    /* Parse BPB fields */
    vol->bpb.bytes_per_sector    = *(uint16_t *)&vol->sector_buf[11];
    vol->bpb.sectors_per_cluster = vol->sector_buf[13];
    vol->bpb.reserved_sectors    = *(uint16_t *)&vol->sector_buf[14];
    vol->bpb.num_fats            = vol->sector_buf[16];
    vol->bpb.fat_size_sectors    = *(uint32_t *)&vol->sector_buf[36];
    vol->bpb.root_cluster        = *(uint32_t *)&vol->sector_buf[44];
    vol->bpb.fs_info_sector      = *(uint16_t *)&vol->sector_buf[48];

    vol->bpb.total_sectors = *(uint16_t *)&vol->sector_buf[19];
    if (vol->bpb.total_sectors == 0)
        vol->bpb.total_sectors = *(uint32_t *)&vol->sector_buf[32];

    if (dev->sector_count > 0 &&
        vol->bpb.total_sectors > (uint32_t)dev->sector_count)
        vol->bpb.total_sectors = (uint32_t)dev->sector_count;

    vol->bpb.first_fat_sector  = vol->bpb.reserved_sectors;
    root_dir_sectors            = 0;
    vol->bpb.first_data_sector = vol->bpb.reserved_sectors
                               + (vol->bpb.num_fats * vol->bpb.fat_size_sectors)
                               + root_dir_sectors;

    /* Volume label: BPB offset 71, 11 bytes, space-padded */
    {
        int i, end;
        for (i = 0; i < 11; i++)
            vol->label[i] = (char)vol->sector_buf[71 + i];
        vol->label[11] = '\0';
        /* Trim trailing spaces */
        for (end = 10; end >= 0 && vol->label[end] == ' '; end--)
            vol->label[end] = '\0';
    }

    /* Strict BPB validation (Microsoft FAT spec S3) */
    if (fat32_validate_bpb(vol) != 0)
        goto fail;

    /* Check dirty volume marker via FAT[1] bit 27 */
    fat32_read_dirty_marker(vol);

    /* Auto-run fsck on dirty mount BEFORE exposing the volume to VFS.
     * Without this, a power-loss-corrupted FAT32 volume would mount
     * cleanly with stale FAT/cluster state, then silently corrupt
     * subsequent writes. Previously the only auto-fsck path was the
     * BlackBox-specific gate in partition.c (GPT name == "BlackBox"),
     * leaving every other FAT32 volume unrepaired. The dirty-mount
     * repair belongs in the FS driver itself so all volumes get the
     * same self-heal contract. fat32_fsck(vol, fix=1) walks the full
     * directory tree, detects cross-linked chains and lost clusters,
     * and repairs them; on success, clear the in-memory dirty flag so
     * set_clean_marker on unmount fires correctly. Failure leaves
     * volume_dirty=1 so the next mount retries. */
    if (vol->volume_dirty) {
        klog(LOG_WARN, "fat32",
             "auto-fsck: dirty volume detected; running repair before mount");
        int fsck_rc = fat32_fsck(vol, 1);
        if (fsck_rc == 0) {
            klog(LOG_INFO, "fat32",
                 "auto-fsck: repair completed; volume now clean");
            vol->volume_dirty = 0;
        } else {
            /* Fail-safe: refuse to expose a writable volume that fsck
             * could not fully repair (traversal failed -- partial walk
             * means lost-cluster scan was skipped, leaving the FS in
             * an unknown state). A degraded mount would let normal
             * VFS writes corrupt a FAT that fsck just declared
             * inconsistent. Free the volume struct via the same
             * pmm_free_frame loop the caller would use; otherwise
             * repeated mount attempts on a corrupted volume leak
             * pages_needed PMM frames per try. The dirty bit stays
             * set on disk (no clean marker written), so the next
             * mount retries fsck automatically once the underlying
             * issue is gone. Operator workaround: boot to recovery,
             * run chkdsk D: /fat32 /fix manually, then remount. */
            klog(LOG_ERROR, "fat32",
                 "auto-fsck: errors remain (%d); refusing to mount "
                 "writable; operator must run chkdsk from recovery",
                 (uint64_t)fsck_rc);
            goto fail;
        }
    }

    /* Compare FAT1 vs FAT2; repair FAT2 if they differ. Runs on every
     * mount as defense in depth -- fsck above already includes a FAT
     * compare on dirty volumes; this catches drift on clean volumes. */
    fat32_compare_repair_fats(vol);

    /* Set up root node */
    fat32_strcpy(vol->root_file.node.name, "A:\\", VFS_MAX_NAME);
    vol->root_file.node.type = VFS_DIRECTORY | VFS_MOUNTPOINT;
    vol->root_file.node.inode = vol->bpb.root_cluster;
    vol->root_file.node.size = 0;
    vol->root_file.node.ops = &fat32_dir_ops;
    vol->root_file.node.fs_data = &vol->root_file;
    vol->root_file.node.parent = (struct vfs_node *)0;
    vol->root_file.first_cluster = vol->bpb.root_cluster;
    vol->root_file.file_size = 0;
    vol->root_file.volume = vol;

    {
        uint64_t vol_mb = (uint64_t)vol->bpb.total_sectors * 512 / (1024*1024);
        klog(LOG_INFO, "fat32",
             "FAT32: \"%s\" %u MiB, %u sectors/cluster, root cluster %u",
               vol->label,
               vol_mb,
               (uint64_t)vol->bpb.sectors_per_cluster,
               (uint64_t)vol->bpb.root_cluster);
    }

    /* ---- Read FSInfo sector ---- */
    vol->fsinfo_valid = 0;
    vol->fsinfo_dirty = 0;
    vol->fsinfo_free_count = FSINFO_UNKNOWN;
    vol->fsinfo_next_free = 2;
    vol->fsinfo_sector = vol->bpb.fs_info_sector;

    if (vol->fsinfo_sector >= 1 &&
        vol->fsinfo_sector < vol->bpb.reserved_sectors) {
        uint8_t fsi[512];
        if (fat32_read_sector(vol, vol->fsinfo_sector, fsi) == 0) {
            uint32_t lead   = *(uint32_t *)&fsi[0];
            uint32_t struc  = *(uint32_t *)&fsi[484];
            uint32_t trail  = *(uint32_t *)&fsi[508];

            if (lead == FSINFO_LEAD_SIG && struc == FSINFO_STRUCT_SIG
                && trail == FSINFO_TRAIL_SIG) {
                vol->fsinfo_free_count = *(uint32_t *)&fsi[488];
                vol->fsinfo_next_free  = *(uint32_t *)&fsi[492];
                vol->fsinfo_valid = 1;

                if (vol->fsinfo_next_free < 2 ||
                    vol->fsinfo_next_free >= (uint32_t)(
                        (vol->bpb.total_sectors - vol->bpb.first_data_sector)
                        / vol->bpb.sectors_per_cluster + 2))
                    vol->fsinfo_next_free = 2;

                klog(LOG_DEBUG, "fat32",
                     "FSInfo: %u free clusters, hint cluster %u",
                     (uint64_t)vol->fsinfo_free_count,
                     (uint64_t)vol->fsinfo_next_free);
            } else {
                klog(LOG_WARN, "fat32",
                     "FSInfo: invalid signatures, full FAT scan mode");
            }
        }
    }

    /* FSInfo FreeCount fallback: if unknown (0xFFFFFFFF), do a full FAT scan */
    if (vol->fsinfo_valid && vol->fsinfo_free_count == FSINFO_UNKNOWN) {
        vol->fsinfo_free_count = fat32_count_free_clusters(vol);
        vol->fsinfo_dirty = 1;
        klog(LOG_INFO, "fat32",
             "FSInfo: FreeCount was unknown -- scanned: %u free clusters",
             (uint64_t)vol->fsinfo_free_count);
    }

    return vol;

fail:
    /* Centralized cleanup for every post-allocation failure path.
     * Without this, repeated mount attempts on a corrupted, malformed,
     * or auto-fsck-failing volume would leak `pages_needed` PMM frames
     * per try. Every failure label above goes through here so the
     * frames are always returned. */
    {
        uintptr_t base = (uintptr_t)vol;
        uint32_t pg;
        for (pg = 0; pg < pages_needed; pg++)
            pmm_free_frame(base + (uintptr_t)pg * 4096u);
    }
    return (struct fat32_volume *)0;
}

struct vfs_fs_driver *fat32_get_driver(void)
{
    return &fat32_driver;
}

struct vfs_node *fat32_get_root(struct fat32_volume *vol)
{
    if (!vol)
        return (struct vfs_node *)0;
    return &vol->root_file.node;
}

uint64_t fat32_get_free_bytes(struct fat32_volume *vol)
{
    if (!vol)
        return 0;
    uint32_t free_cl = vol->fsinfo_free_count;
    if (free_cl == 0xFFFFFFFF)  /* FSINFO_UNKNOWN */
        free_cl = fat32_count_free_clusters(vol);
    return (uint64_t)free_cl * vol->bpb.sectors_per_cluster * 512;
}

uint64_t fat32_get_total_bytes(struct fat32_volume *vol)
{
    if (!vol) return 0;
    return (uint64_t)vol->bpb.total_sectors * 512;
}

struct fat32_volume *fat32_volume_from_root(struct vfs_node *root)
{
    if (!root || !root->fs_data)
        return (struct fat32_volume *)0;
    struct fat32_file *ff = (struct fat32_file *)root->fs_data;
    return ff->volume;
}

const char *fat32_get_label(struct fat32_volume *vol)
{
    if (!vol) return "";
    return vol->label;
}

int fat32_is_dirty(struct fat32_volume *vol)
{
    return vol ? vol->volume_dirty : 0;
}

void fat32_mark_dirty(struct fat32_volume *vol)
{
    if (vol) fat32_set_dirty_marker(vol);
}

void fat32_mark_clean(struct fat32_volume *vol)
{
    if (vol) {
        fat32_set_clean_marker(vol);
        fat32_fsinfo_flush(vol);
    }
}

int fat32_run_fsck(struct fat32_volume *vol, int fix)
{
    return vol ? fat32_fsck(vol, fix) : -1;
}
