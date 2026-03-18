/* ============================================================================
 * fat32_ops.c — VFS operations, stat, init, driver registration
 *
 * VFS ops tables (file + directory), all VFS wrapper functions,
 * fat32_stat (path-based metadata lookup), fat32_init, fat32_get_root,
 * and fat32_get_driver.
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
    (void)node;
    /* Flush any dirty cached sectors on file close */
    scache_flush();
    fat32_fsinfo_flush();
    return 0;
}

static int fat32_file_read(struct vfs_node *node, uint32_t offset,
                           uint32_t size, uint8_t *buffer)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t cluster;
    uint32_t bytes_per_cluster;
    uint32_t bytes_read = 0;
    uint32_t cluster_offset;
    uint32_t to_read;
    uint8_t *cluster_buf;

    if (!f || offset >= f->file_size)
        return 0;

    if (offset + size > f->file_size)
        size = f->file_size - offset;

    bytes_per_cluster = bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    /* Skip clusters to reach the offset */
    cluster = f->first_cluster;
    {
        uint32_t skip = offset / bytes_per_cluster;
        uint32_t s;
        for (s = 0; s < skip && cluster < FAT32_EOC; s++)
            cluster = fat32_next_cluster(cluster);
    }
    cluster_offset = offset % bytes_per_cluster;

    /* Read data cluster by cluster */
    while (bytes_read < size && cluster < FAT32_EOC && cluster != FAT32_FREE) {
        uint32_t sector = cluster_to_sector(cluster);
        uint32_t ci;

        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        to_read = bytes_per_cluster - cluster_offset;
        if (to_read > size - bytes_read)
            to_read = size - bytes_read;

        for (ci = 0; ci < to_read; ci++)
            buffer[bytes_read + ci] = cluster_buf[cluster_offset + ci];

        bytes_read += to_read;
        cluster_offset = 0;   /* only first cluster may have an offset */
        cluster = fat32_next_cluster(cluster);
    }

    kfree(cluster_buf);
    return (int)bytes_read;
}

/* VFS-compatible file write: offset-aware write into existing cluster chain. */
static int fat32_file_write_vfs(struct vfs_node *node, uint32_t offset,
                                uint32_t size, const uint8_t *buffer)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t bytes_per_cluster;
    uint32_t cluster;
    uint32_t bytes_written = 0;
    uint32_t cluster_offset;
    uint8_t *cluster_buf;

    if (!f || !buffer || size == 0)
        return -1;

    bytes_per_cluster = bpb.sectors_per_cluster * 512;
    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return -1;

    /* If the file has no clusters yet (empty file), allocate the first one */
    cluster = f->first_cluster;
    if (cluster < 2 || cluster >= FAT32_EOC) {
        cluster = fat32_alloc_cluster();
        if (cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }
        fat32_zero_cluster(cluster);
        f->first_cluster = cluster;
        node->inode = cluster;

        /* Update directory entry's first_cluster fields. */
        {
            uint32_t bpc = bpb.sectors_per_cluster * 512;
            uint8_t *dbuf = (uint8_t *)kmalloc(bpc);
            if (dbuf) {
                uint8_t short_name[11];
                uint32_t dc = f->dir_cluster;
                fat32_make_short_name(node->name, short_name);

                while (dc >= 2 && dc < FAT32_EOC) {
                    uint32_t sec = cluster_to_sector(dc);
                    uint32_t di;
                    if (fat32_read_sectors_multi(sec, bpb.sectors_per_cluster,
                                                 dbuf) != 0)
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
                            fat32_write_sectors_multi(sec,
                                bpb.sectors_per_cluster, dbuf);
                            goto fc_done;
                        }
                    }
                    dc = fat32_get_fat_entry(dc);
                }
fc_done:
                kfree(dbuf);
            }
        }
    }

    /* Skip clusters to reach the offset */
    {
        uint32_t skip = offset / bytes_per_cluster;
        uint32_t s;
        for (s = 0; s < skip; s++) {
            uint32_t next = fat32_next_cluster(cluster);
            if (next < 2 || next >= FAT32_EOC) {
                /* Need to extend chain to reach the offset */
                next = fat32_alloc_cluster();
                if (next == 0) {
                    kfree(cluster_buf);
                    return -1;
                }
                fat32_zero_cluster(next);
                fat32_set_fat_entry(cluster, next);
            }
            cluster = next;
        }
    }
    cluster_offset = offset % bytes_per_cluster;

    /* Write data cluster by cluster */
    while (bytes_written < size) {
        uint32_t sector = cluster_to_sector(cluster);
        uint32_t to_write;
        uint32_t ci;

        /* Read existing cluster data (required for partial writes) */
        if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        to_write = bytes_per_cluster - cluster_offset;
        if (to_write > size - bytes_written)
            to_write = size - bytes_written;

        /* Copy buffer into the correct position within the cluster */
        for (ci = 0; ci < to_write; ci++)
            cluster_buf[cluster_offset + ci] = buffer[bytes_written + ci];

        /* Write modified cluster back to disk */
        if (fat32_write_sectors_multi(sector, bpb.sectors_per_cluster,
                                      cluster_buf) != 0)
            break;

        bytes_written += to_write;
        cluster_offset = 0;  /* only first cluster may have an offset */

        /* Move to next cluster, allocating if needed */
        if (bytes_written < size) {
            uint32_t next = fat32_next_cluster(cluster);
            if (next < 2 || next >= FAT32_EOC) {
                /* Extend chain: allocate new cluster */
                next = fat32_alloc_cluster();
                if (next == 0)
                    break;
                fat32_zero_cluster(next);
                fat32_set_fat_entry(cluster, next);
            }
            cluster = next;
        }
    }

    kfree(cluster_buf);

    /* Update file size if the write extended the file */
    {
        uint32_t end_pos = offset + bytes_written;
        if (end_pos > f->file_size) {
            f->file_size = end_pos;
            node->size = end_pos;
            fat32_update_dir_size(f->dir_cluster, f->first_cluster, end_pos);
        }
    }

    /* Invalidate dir cache so subsequent reads see updated size */
    dir_file_count = 0;

    return (int)bytes_written;
}

/* ---- Directory operations ---- */

static struct vfs_dirent *fat32_readdir(struct vfs_node *node, uint32_t index)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t dir_cluster;

    /* Determine which directory to list */
    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    /* Lazily load directory entries */
    if (dir_file_count == 0)
        fat32_read_dir(dir_cluster);

    if (index >= dir_file_count)
        return (struct vfs_dirent *)0;

    fat32_strcpy(fat32_dirent.name, dir_files[index].node.name, VFS_MAX_NAME);
    fat32_dirent.inode = dir_files[index].node.inode;
    fat32_dirent.type = dir_files[index].node.type;

    return &fat32_dirent;
}

static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t dir_cluster;
    uint32_t i;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    if (dir_file_count == 0)
        fat32_read_dir(dir_cluster);

    for (i = 0; i < dir_file_count; i++) {
        if (fat32_strcasecmp(dir_files[i].node.name, name)) {
            /* Set directory ops for subdirectories, file ops for files */
            if (dir_files[i].node.type & VFS_DIRECTORY)
                dir_files[i].node.ops = &fat32_dir_ops;
            else
                dir_files[i].node.ops = &fat32_file_ops;
            dir_files[i].node.fs_data = &dir_files[i];
            dir_files[i].dir_cluster = dir_cluster;
            return &dir_files[i].node;
        }
    }

    return (struct vfs_node *)0;
}

/* ---- VFS wrapper functions ---- */

static int fat32_vfs_create(struct vfs_node *parent, const char *name,
                            uint8_t type)
{
    struct fat32_file *f = (struct fat32_file *)parent->fs_data;
    uint32_t dir_cluster;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    if (type & VFS_DIRECTORY)
        return fat32_create_dir(dir_cluster, name);
    else
        return fat32_create_file(dir_cluster, name);
}

static int fat32_vfs_unlink(struct vfs_node *parent, const char *name)
{
    struct fat32_file *f = (struct fat32_file *)parent->fs_data;
    uint32_t dir_cluster;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    return fat32_delete_file(dir_cluster, name);
}

static int fat32_vfs_rename(struct vfs_node *parent,
                            const char *old_name, const char *new_name)
{
    struct fat32_file *f = (struct fat32_file *)parent->fs_data;
    uint32_t dir_cluster;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    return fat32_rename(dir_cluster, old_name, new_name);
}

static int fat32_vfs_stat(struct vfs_node *node, struct vfs_stat *st)
{
    struct fat32_file *f;
    if (!node || !st)
        return -1;

    f = (struct fat32_file *)node->fs_data;
    st->size   = f ? (uint64_t)f->file_size : 0;
    st->type   = node->type;
    st->ctime  = 0;  /* FAT timestamps not cached in memory */
    st->mtime  = 0;
    st->atime  = 0;
    st->blocks = 0;
    return 0;
}

static int fat32_vfs_truncate(struct vfs_node *node, uint64_t new_size)
{
    struct fat32_file *f = (struct fat32_file *)node->fs_data;
    uint32_t dir_cluster;

    /* Only truncate files, not directories */
    if (node->type & VFS_DIRECTORY)
        return -1;

    dir_cluster = bpb.root_cluster;

    if (node->name[0])
        return fat32_truncate(dir_cluster, node->name, (uint32_t)new_size);

    (void)f;
    return -1;
}

static int fat32_vfs_mkdir(struct vfs_node *parent, const char *name)
{
    struct fat32_file *f = (struct fat32_file *)parent->fs_data;
    uint32_t dir_cluster;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    return fat32_create_dir(dir_cluster, name);
}

static int fat32_vfs_rmdir(struct vfs_node *parent, const char *name)
{
    struct fat32_file *f = (struct fat32_file *)parent->fs_data;
    uint32_t dir_cluster;

    dir_cluster = (f && f->first_cluster >= 2)
                ? f->first_cluster : bpb.root_cluster;

    return fat32_rmdir(dir_cluster, name);
}

static int fat32_vfs_set_attr(struct vfs_node *node, uint32_t attributes)
{
    uint32_t dir_cluster = bpb.root_cluster;

    if (!node->name[0])
        return -1;

    return fat32_set_attr(dir_cluster, node->name, (uint8_t)attributes);
}

static int fat32_vfs_set_times(struct vfs_node *node,
                               const filetime_t *ctime_p,
                               const filetime_t *mtime_p,
                               const filetime_t *atime_p)
{
    uint32_t dir_cluster = bpb.root_cluster;

    if (!node->name[0])
        return -1;

    return fat32_set_times(dir_cluster, node->name, ctime_p, mtime_p, atime_p);
}

static int fat32_vfs_flush(struct vfs_node *node)
{
    (void)node;
    fat32_fsinfo_flush();
    return fat32_flush_disk();
}

/* ---- fat32_stat: file metadata lookup ---- */

int fat32_stat(const char *path, struct fat32_stat_info *info)
{
    uint32_t cluster;
    uint32_t bytes_per_cluster;
    uint8_t *cluster_buf;
    uint32_t i;
    const char *component;
    const char *next;
    int found;

    if (!info || !path)
        return -1;

    /* Start from root directory */
    cluster = bpb.root_cluster;
    bytes_per_cluster = bpb.sectors_per_cluster * 512;

    /* Skip leading separators */
    while (*path == '/' || *path == '\\')
        path++;

    if (*path == '\0') {
        /* Root directory itself */
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

        /* Extract current path component */
        next = component;
        while (*next && *next != '/' && *next != '\\')
            next++;
        while (component < next && ti < FAT32_MAX_NAME - 1)
            target[ti++] = *component++;
        target[ti] = '\0';

        /* Skip separator */
        while (*component == '/' || *component == '\\')
            component++;

        /* Search this directory for the target */
        found = 0;
        {
            uint32_t cur_cluster = cluster;
            /* LFN assembly */
            char lfn_name[FAT32_MAX_NAME];
            int lfn_active_s = 0;
            int k;

            for (k = 0; k < FAT32_MAX_NAME; k++)
                lfn_name[k] = '\0';

            while (cur_cluster < FAT32_EOC && cur_cluster != FAT32_FREE) {
                uint32_t sector = cluster_to_sector(cur_cluster);

                if (fat32_read_sectors_multi(sector, bpb.sectors_per_cluster,
                                             cluster_buf) != 0)
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

                    /* Build name */
                    if (lfn_active_s && lfn_name[0] != '\0')
                        fat32_strcpy(ename, lfn_name, FAT32_MAX_NAME);
                    else
                        fat32_short_name_to_str(de->name, ename);
                    lfn_active_s = 0;

                    if (fat32_strcasecmp(ename, target)) {
                        uint32_t fc = ((uint32_t)de->first_cluster_hi << 16)
                                    | (uint32_t)de->first_cluster_lo;

                        if (*component == '\0') {
                            /* This is the final component — fill info */
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

                        /* Directory — descend */
                        if (!(de->attr & FAT32_ATTR_DIRECTORY))
                            goto stat_not_found;

                        cluster = fc;
                        found = 1;
                        goto next_component;
                    }
                }

                cur_cluster = fat32_next_cluster(cur_cluster);
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

int fat32_init(const struct blkdev *dev)
{
    uint32_t root_dir_sectors;

    if (!dev) {
        klog(LOG_ERROR, "fat32", "FAT32: null block device");
        return -1;
    }

    fat32_dev = dev;
    dir_file_count = 0;

    /* Read the boot sector (BPB) — LBA 0 relative to the sub-blkdev */
    if (fat32_read_sector(0, sector_buf) != 0) {
        klog(LOG_ERROR, "fat32", "FAT32: cannot read boot sector");
        return -1;
    }

    /* Verify boot signature */
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) {
        klog(LOG_ERROR, "fat32", "FAT32: invalid boot signature");
        return -1;
    }

    /* Parse BPB fields */
    bpb.bytes_per_sector    = *(uint16_t *)&sector_buf[11];
    bpb.sectors_per_cluster = sector_buf[13];
    bpb.reserved_sectors    = *(uint16_t *)&sector_buf[14];
    bpb.num_fats            = sector_buf[16];
    bpb.fat_size_sectors    = *(uint32_t *)&sector_buf[36];
    bpb.root_cluster        = *(uint32_t *)&sector_buf[44];
    bpb.fs_info_sector      = *(uint16_t *)&sector_buf[48];

    /* Total sectors: use the 32-bit field */
    bpb.total_sectors = *(uint16_t *)&sector_buf[19];
    if (bpb.total_sectors == 0)
        bpb.total_sectors = *(uint32_t *)&sector_buf[32];

    /* Cap to actual partition size (BPB may exceed GPT partition boundary) */
    if (dev->sector_count > 0 && bpb.total_sectors > (uint32_t)dev->sector_count)
        bpb.total_sectors = (uint32_t)dev->sector_count;

    /* Compute layout */
    bpb.first_fat_sector  = bpb.reserved_sectors;
    root_dir_sectors      = 0;   /* FAT32 has no fixed root dir */
    bpb.first_data_sector = bpb.reserved_sectors
                          + (bpb.num_fats * bpb.fat_size_sectors)
                          + root_dir_sectors;

    /* Validate */
    if (bpb.bytes_per_sector != 512) {
        klog(LOG_ERROR, "fat32", "FAT32: unsupported sector size %u",
               (uint64_t)bpb.bytes_per_sector);
        return -1;
    }

    /* Set up root node */
    fat32_strcpy(root_file.node.name, "A:\\", VFS_MAX_NAME);
    root_file.node.type = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root_file.node.inode = bpb.root_cluster;
    root_file.node.size = 0;
    root_file.node.ops = &fat32_dir_ops;
    root_file.node.fs_data = &root_file;
    root_file.node.parent = (struct vfs_node *)0;
    root_file.first_cluster = bpb.root_cluster;
    root_file.file_size = 0;

    {
        uint64_t vol_mb = (uint64_t)bpb.total_sectors * 512 / (1024 * 1024);
        klog(LOG_DEBUG, "fat32", "FAT32: %u MiB, %u sectors/cluster, root cluster %u",
               vol_mb,
               (uint64_t)bpb.sectors_per_cluster,
               (uint64_t)bpb.root_cluster);
    }

    /* ---- Read FSInfo sector ---- */
    fsinfo_valid = 0;
    fsinfo_dirty = 0;
    fsinfo_free_count = FSINFO_UNKNOWN;
    fsinfo_next_free = 2;
    fsinfo_sector = bpb.fs_info_sector;

    if (fsinfo_sector >= 1 && fsinfo_sector < bpb.reserved_sectors) {
        uint8_t fsi[512];
        if (fat32_read_sector(fsinfo_sector, fsi) == 0) {
            uint32_t lead   = *(uint32_t *)&fsi[0];
            uint32_t struc  = *(uint32_t *)&fsi[484];
            uint32_t trail  = *(uint32_t *)&fsi[508];

            if (lead == FSINFO_LEAD_SIG && struc == FSINFO_STRUCT_SIG
                && trail == FSINFO_TRAIL_SIG) {
                fsinfo_free_count = *(uint32_t *)&fsi[488];
                fsinfo_next_free  = *(uint32_t *)&fsi[492];
                fsinfo_valid = 1;

                /* Sanity: if hint is out of range, reset to 2 */
                if (fsinfo_next_free < 2 || fsinfo_next_free >= (uint32_t)(
                    (bpb.total_sectors - bpb.first_data_sector)
                    / bpb.sectors_per_cluster + 2))
                    fsinfo_next_free = 2;

                klog(LOG_DEBUG, "fat32",
                     "FSInfo: %u free clusters, hint cluster %u",
                     (uint64_t)fsinfo_free_count,
                     (uint64_t)fsinfo_next_free);
            } else {
                klog(LOG_WARN, "fat32",
                     "FSInfo: invalid signatures, full FAT scan mode");
            }
        }
    }

    return 0;
}

struct vfs_fs_driver *fat32_get_driver(void)
{
    return &fat32_driver;
}

struct vfs_node *fat32_get_root(void)
{
    return &root_file.node;
}
