/* ============================================================================
 * ixfs-fuse-linux.c — Linux FUSE3 read-only mount for IXFS partitions
 *
 * Usage:
 *   ixfs-mount <image>:<partition> <mountpoint>
 *   ixfs-mount build/system-disk.img:2 /mnt/ixfs
 *
 * Unmount:
 *   fusermount -u /mnt/ixfs
 * ============================================================================ */

#define FUSE_USE_VERSION 35
#define _POSIX_C_SOURCE 200809L

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "ixfs-core.h"
#include "ixfs-disk.h"

/* Global volume — single-mount, single-volume */
static ixfs_vol_t *g_vol;

/* --- Path resolution --- */

/* Resolve a FUSE path (e.g. "/Impossible/Fonts") to an inode number.
 * Returns 0 on failure. */
static uint32_t resolve_path(const char *path)
{
    struct ixfs_inode inode;
    uint32_t ino;
    char buf[1024];
    char *tok, *save;

    if (strcmp(path, "/") == 0)
        return g_vol->sb.s_root_inode;

    /* Start from root */
    ino = g_vol->sb.s_root_inode;
    if (ixfs_read_inode(g_vol, ino, &inode) != 0)
        return 0;

    /* Walk path components */
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    tok = strtok_r(buf, "/", &save);
    while (tok) {
        if (!(inode.i_mode & IXFS_S_DIR))
            return 0;  /* not a directory */

        ino = ixfs_lookup(g_vol, &inode, tok);
        if (ino == 0)
            return 0;  /* not found */

        if (ixfs_read_inode(g_vol, ino, &inode) != 0)
            return 0;

        tok = strtok_r(NULL, "/", &save);
    }

    return ino;
}

/* --- FUSE callbacks --- */

static int ixfs_fuse_getattr(const char *path, struct stat *st,
                             struct fuse_file_info *fi)
{
    struct ixfs_inode inode;
    uint32_t ino;

    (void)fi;
    memset(st, 0, sizeof(*st));

    ino = resolve_path(path);
    if (ino == 0)
        return -ENOENT;

    if (ixfs_read_inode(g_vol, ino, &inode) != 0)
        return -EIO;

    st->st_ino = ino;
    st->st_nlink = inode.i_links;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_size = (off_t)inode.i_size;
    st->st_blocks = inode.i_blocks * (IXFS_BLOCK_SIZE / 512);
    st->st_blksize = IXFS_BLOCK_SIZE;
    st->st_atime = inode.i_atime;
    st->st_mtime = inode.i_mtime;
    st->st_ctime = inode.i_ctime;

    if (inode.i_mode & IXFS_S_DIR) {
        st->st_mode = S_IFDIR | 0755;
    } else {
        st->st_mode = S_IFREG | 0644;
    }

    /* If bitmap loaded (R/W mode), allow writes */
    if (g_vol->bitmap)
        st->st_mode |= 0200; /* owner write */

    return 0;
}

/* Readdir callback context */
struct readdir_ctx {
    void *buf;
    fuse_fill_dir_t filler;
};

static int readdir_cb(const struct ixfs_dir_entry *de,
                      const struct ixfs_inode *inode, void *ctx)
{
    struct readdir_ctx *rc = (struct readdir_ctx *)ctx;
    (void)inode;
    rc->filler(rc->buf, de->d_name, NULL, 0, 0);
    return 0;
}

static int ixfs_fuse_readdir(const char *path, void *buf,
                             fuse_fill_dir_t filler,
                             off_t offset, struct fuse_file_info *fi,
                             enum fuse_readdir_flags flags)
{
    struct ixfs_inode inode;
    uint32_t ino;
    struct readdir_ctx rc;

    (void)offset;
    (void)fi;
    (void)flags;

    ino = resolve_path(path);
    if (ino == 0)
        return -ENOENT;

    if (ixfs_read_inode(g_vol, ino, &inode) != 0)
        return -EIO;

    if (!(inode.i_mode & IXFS_S_DIR))
        return -ENOTDIR;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    rc.buf = buf;
    rc.filler = filler;
    ixfs_readdir(g_vol, &inode, readdir_cb, &rc);

    return 0;
}

static int ixfs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    struct ixfs_inode inode;
    uint32_t ino;

    ino = resolve_path(path);
    if (ino == 0)
        return -ENOENT;

    if (ixfs_read_inode(g_vol, ino, &inode) != 0)
        return -EIO;

    /* Allow R/W -- write ops check bitmap availability */
    (void)fi;
    return 0;
}

static int ixfs_fuse_read(const char *path, char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    struct ixfs_inode inode;
    uint32_t ino;
    int64_t n;

    (void)fi;

    ino = resolve_path(path);
    if (ino == 0)
        return -ENOENT;

    if (ixfs_read_inode(g_vol, ino, &inode) != 0)
        return -EIO;

    n = ixfs_read_data(g_vol, &inode, (uint64_t)offset, buf, (uint64_t)size);
    if (n < 0)
        return -EIO;

    return (int)n;
}

static int ixfs_fuse_write(const char *path, const char *buf, size_t size,
                           off_t offset, struct fuse_file_info *fi)
{
    struct ixfs_inode inode;
    uint32_t ino;
    int64_t n;

    (void)fi;

    if (!g_vol->bitmap)
        return -EROFS;

    ino = resolve_path(path);
    if (ino == 0)
        return -ENOENT;

    if (ixfs_read_inode(g_vol, ino, &inode) != 0)
        return -EIO;

    n = ixfs_write_data(g_vol, ino, &inode, (uint64_t)offset, buf, (uint64_t)size);
    if (n < 0)
        return -EIO;

    return (int)n;
}

static int ixfs_fuse_create(const char *path, mode_t mode,
                            struct fuse_file_info *fi)
{
    char parent_path[1024];
    const char *name;

    (void)mode;
    (void)fi;

    if (!g_vol->bitmap) return -EROFS;

    /* Split path into parent + name */
    strncpy(parent_path, path, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = '\0';
    char *slash = strrchr(parent_path, '/');
    if (!slash) return -EINVAL;
    name = slash + 1;
    if (slash == parent_path)
        parent_path[1] = '\0'; /* root */
    else
        *slash = '\0';

    uint32_t parent_ino = resolve_path(parent_path);
    if (parent_ino == 0) return -ENOENT;

    struct ixfs_inode parent_inode;
    if (ixfs_read_inode(g_vol, parent_ino, &parent_inode) != 0) return -EIO;

    uint32_t new_ino = ixfs_create(g_vol, parent_ino, &parent_inode, name, IXFS_S_FILE);
    if (new_ino == 0) return -ENOSPC;

    return 0;
}

static int ixfs_fuse_mkdir(const char *path, mode_t mode)
{
    char parent_path[1024];
    const char *name;

    (void)mode;

    if (!g_vol->bitmap) return -EROFS;

    strncpy(parent_path, path, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = '\0';
    char *slash = strrchr(parent_path, '/');
    if (!slash) return -EINVAL;
    name = slash + 1;
    if (slash == parent_path)
        parent_path[1] = '\0';
    else
        *slash = '\0';

    uint32_t parent_ino = resolve_path(parent_path);
    if (parent_ino == 0) return -ENOENT;

    struct ixfs_inode parent_inode;
    if (ixfs_read_inode(g_vol, parent_ino, &parent_inode) != 0) return -EIO;

    uint32_t new_ino = ixfs_create(g_vol, parent_ino, &parent_inode, name, IXFS_S_DIR);
    if (new_ino == 0) return -ENOSPC;

    return 0;
}

static int ixfs_fuse_unlink(const char *path)
{
    char parent_path[1024];
    const char *name;

    if (!g_vol->bitmap) return -EROFS;

    strncpy(parent_path, path, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = '\0';
    char *slash = strrchr(parent_path, '/');
    if (!slash) return -EINVAL;
    name = slash + 1;
    if (slash == parent_path)
        parent_path[1] = '\0';
    else
        *slash = '\0';

    uint32_t parent_ino = resolve_path(parent_path);
    if (parent_ino == 0) return -ENOENT;

    struct ixfs_inode parent_inode;
    if (ixfs_read_inode(g_vol, parent_ino, &parent_inode) != 0) return -EIO;

    return ixfs_delete(g_vol, parent_ino, &parent_inode, name);
}

static int ixfs_fuse_rmdir(const char *path)
{
    return ixfs_fuse_unlink(path); /* ixfs_delete checks empty dirs */
}

static int ixfs_fuse_rename(const char *from, const char *to, unsigned int flags)
{
    char parent_path[1024];
    const char *old_name, *new_name;

    (void)flags;
    if (!g_vol->bitmap) return -EROFS;

    /* Only same-directory rename for now */
    strncpy(parent_path, from, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = '\0';
    char *slash = strrchr(parent_path, '/');
    if (!slash) return -EINVAL;
    old_name = slash + 1;
    if (slash == parent_path)
        parent_path[1] = '\0';
    else
        *slash = '\0';

    /* Extract new_name from 'to' */
    new_name = strrchr(to, '/');
    if (new_name) new_name++; else new_name = to;

    uint32_t parent_ino = resolve_path(parent_path);
    if (parent_ino == 0) return -ENOENT;

    struct ixfs_inode parent_inode;
    if (ixfs_read_inode(g_vol, parent_ino, &parent_inode) != 0) return -EIO;

    return ixfs_rename(g_vol, parent_ino, &parent_inode, old_name, new_name);
}

static int ixfs_fuse_truncate(const char *path, off_t size,
                              struct fuse_file_info *fi)
{
    (void)fi;
    (void)size;
    (void)path;
    /* Truncation not implemented yet -- silently succeed for zero-length truncate */
    if (size == 0) return 0;
    return -ENOSYS;
}

static const struct fuse_operations ixfs_fuse_ops = {
    .getattr  = ixfs_fuse_getattr,
    .readdir  = ixfs_fuse_readdir,
    .open     = ixfs_fuse_open,
    .read     = ixfs_fuse_read,
    .write    = ixfs_fuse_write,
    .create   = ixfs_fuse_create,
    .mkdir    = ixfs_fuse_mkdir,
    .unlink   = ixfs_fuse_unlink,
    .rmdir    = ixfs_fuse_rmdir,
    .rename   = ixfs_fuse_rename,
    .truncate = ixfs_fuse_truncate,
};

/* --- Main --- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <image>:<partition> <mountpoint> [FUSE options]\n"
        "\n"
        "Examples:\n"
        "  %s build/system-disk.img:2 /mnt/ixfs\n"
        "  %s /dev/sdb:2 /mnt/ixfs\n"
        "\n"
        "Unmount: fusermount -u /mnt/ixfs\n",
        prog, prog, prog);
}

int main(int argc, char *argv[])
{
    char *image_spec;
    char *colon;
    char image_path[1024];
    int part_idx;
    ixfs_disk_ctx_t *disk;
    struct fuse_args args;
    int ret;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    /* Parse image:partition from argv[1] */
    image_spec = argv[1];
    colon = strrchr(image_spec, ':');
    if (!colon || colon == image_spec) {
        fprintf(stderr, "Error: specify partition as <image>:<N>\n");
        usage(argv[0]);
        return 1;
    }

    /* Split at colon */
    size_t path_len = (size_t)(colon - image_spec);
    if (path_len >= sizeof(image_path)) {
        fprintf(stderr, "Error: image path too long\n");
        return 1;
    }
    memcpy(image_path, image_spec, path_len);
    image_path[path_len] = '\0';
    part_idx = atoi(colon + 1);

    if (part_idx < 0) {
        fprintf(stderr, "Error: partition index must be >= 0 (0 = raw image, no GPT)\n");
        return 1;
    }

    /* Open disk and IXFS volume */
    disk = disk_open(image_path, part_idx);
    if (!disk)
        return 1;

    g_vol = ixfs_open(disk);
    if (!g_vol) {
        disk_close(disk);
        return 1;
    }

    /* Load bitmap for R/W support */
    if (ixfs_load_bitmap(g_vol) == 0) {
        fprintf(stderr, "IXFS: \"%s\" v%u, %llu blocks (R/W) -- mounting at %s\n",
                g_vol->sb.s_volume_name,
                g_vol->sb.s_version,
                (unsigned long long)g_vol->sb.s_total_blocks,
                argv[2]);
    } else {
        fprintf(stderr, "IXFS: \"%s\" v%u, %llu blocks (read-only) -- mounting at %s\n",
                g_vol->sb.s_volume_name,
                g_vol->sb.s_version,
                (unsigned long long)g_vol->sb.s_total_blocks,
                argv[2]);
    }

    /* Build FUSE args */
    args = (struct fuse_args)FUSE_ARGS_INIT(0, NULL);
    fuse_opt_add_arg(&args, argv[0]);
    fuse_opt_add_arg(&args, argv[2]);
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "default_permissions");
    /* Forward any extra FUSE options from argv[3..] */
    for (int i = 3; i < argc; i++)
        fuse_opt_add_arg(&args, argv[i]);

    ret = fuse_main(args.argc, args.argv, &ixfs_fuse_ops, NULL);

    fuse_opt_free_args(&args);
    ixfs_close(g_vol);
    disk_close(disk);
    return ret;
}
