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
    st->st_uid = inode.i_uid;
    st->st_gid = inode.i_gid;
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

    /* Read-only mount — reject writes */
    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        return -EROFS;

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

static const struct fuse_operations ixfs_fuse_ops = {
    .getattr = ixfs_fuse_getattr,
    .readdir = ixfs_fuse_readdir,
    .open    = ixfs_fuse_open,
    .read    = ixfs_fuse_read,
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

    if (part_idx < 1) {
        fprintf(stderr, "Error: partition index must be >= 1\n");
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

    fprintf(stderr, "IXFS: \"%s\" v%u, %llu blocks — mounting at %s\n",
            g_vol->sb.s_volume_name,
            g_vol->sb.s_version,
            (unsigned long long)g_vol->sb.s_total_blocks,
            argv[2]);

    /* Build FUSE args: prog, mountpoint, -o ro, plus any extra args */
    args = (struct fuse_args)FUSE_ARGS_INIT(0, NULL);
    fuse_opt_add_arg(&args, argv[0]);
    fuse_opt_add_arg(&args, argv[2]);
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "ro,default_permissions");
    /* Forward any extra FUSE options from argv[3..] */
    for (int i = 3; i < argc; i++)
        fuse_opt_add_arg(&args, argv[i]);

    ret = fuse_main(args.argc, args.argv, &ixfs_fuse_ops, NULL);

    fuse_opt_free_args(&args);
    ixfs_close(g_vol);
    disk_close(disk);
    return ret;
}
