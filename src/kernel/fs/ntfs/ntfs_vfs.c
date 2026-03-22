/* ============================================================================
 * ntfs_vfs.c — NTFS VFS Driver (Read-Only)
 *
 * Wires the NTFS read-path functions into the VFS abstraction layer.
 * Provides open/close/read/readdir/finddir/stat callbacks.
 * All write operations return -1 (read-only filesystem).
 *
 * Each VFS node carries an ntfs_node_data as fs_data, which stores the
 * volume pointer, MFT inode, file size, and directory flag.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ---- Per-node filesystem data ---- */

struct ntfs_node_data {
    struct ntfs_volume *vol;    /* Volume this node belongs to */
    uint64_t inode;             /* MFT inode number */
    uint64_t file_size;         /* File size (0 for directories) */
    uint8_t  is_directory;      /* 1 = directory, 0 = file */
};

/* ---- Inline helpers ---- */

static void ntfs_vfs_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- Forward declarations ---- */

static int ntfs_vfs_open(struct vfs_node *node, uint32_t flags);
static int ntfs_vfs_close(struct vfs_node *node);
static int ntfs_vfs_read(struct vfs_node *node, uint32_t offset,
                          uint32_t size, uint8_t *buffer);
static struct vfs_dirent *ntfs_vfs_readdir(struct vfs_node *node,
                                            uint32_t index);
static struct vfs_node *ntfs_vfs_finddir(struct vfs_node *node,
                                          const char *name);
static int ntfs_vfs_stat(struct vfs_node *node, struct vfs_stat *st);

/* Write-op stubs (read-only driver) */
static int ntfs_vfs_write_ro(struct vfs_node *n, uint32_t o,
                              uint32_t s, const uint8_t *b)
{
    (void)n; (void)o; (void)s; (void)b; return -1;
}
static int ntfs_vfs_create_ro(struct vfs_node *p, const char *nm, uint8_t t)
{
    (void)p; (void)nm; (void)t; return -1;
}
static int ntfs_vfs_unlink_ro(struct vfs_node *p, const char *nm)
{
    (void)p; (void)nm; return -1;
}
static int ntfs_vfs_rename_ro(struct vfs_node *p, const char *a, const char *b)
{
    (void)p; (void)a; (void)b; return -1;
}
static int ntfs_vfs_truncate_ro(struct vfs_node *n, uint64_t sz)
{
    (void)n; (void)sz; return -1;
}
static int ntfs_vfs_mkdir_ro(struct vfs_node *p, const char *nm)
{
    (void)p; (void)nm; return -1;
}
static int ntfs_vfs_rmdir_ro(struct vfs_node *p, const char *nm)
{
    (void)p; (void)nm; return -1;
}
static int ntfs_vfs_set_attr_ro(struct vfs_node *n, uint32_t a)
{
    (void)n; (void)a; return -1;
}
static int ntfs_vfs_set_times_ro(struct vfs_node *n, const filetime_t *c,
                                  const filetime_t *m, const filetime_t *a)
{
    (void)n; (void)c; (void)m; (void)a; return -1;
}
static int ntfs_vfs_flush_noop(struct vfs_node *n)
{
    (void)n; return 0;
}

/* ---- VFS ops tables ---- */

static struct vfs_ops ntfs_file_ops = {
    .open      = ntfs_vfs_open,
    .close     = ntfs_vfs_close,
    .read      = ntfs_vfs_read,
    .write     = ntfs_vfs_write_ro,
    .readdir   = (void *)0,
    .finddir   = (void *)0,
    .create    = (void *)0,
    .unlink    = (void *)0,
    .rename    = (void *)0,
    .stat      = ntfs_vfs_stat,
    .truncate  = ntfs_vfs_truncate_ro,
    .mkdir     = (void *)0,
    .rmdir     = (void *)0,
    .set_attr  = (void *)0,
    .set_times = (void *)0,
    .flush     = ntfs_vfs_flush_noop,
};

static struct vfs_ops ntfs_dir_ops = {
    .open      = ntfs_vfs_open,
    .close     = ntfs_vfs_close,
    .read      = (void *)0,
    .write     = (void *)0,
    .readdir   = ntfs_vfs_readdir,
    .finddir   = ntfs_vfs_finddir,
    .create    = ntfs_vfs_create_ro,
    .unlink    = ntfs_vfs_unlink_ro,
    .rename    = ntfs_vfs_rename_ro,
    .stat      = ntfs_vfs_stat,
    .truncate  = (void *)0,
    .mkdir     = ntfs_vfs_mkdir_ro,
    .rmdir     = ntfs_vfs_rmdir_ro,
    .set_attr  = ntfs_vfs_set_attr_ro,
    .set_times = ntfs_vfs_set_times_ro,
    .flush     = ntfs_vfs_flush_noop,
};

static struct vfs_fs_driver ntfs_driver = {
    .name      = "NTFS",
    .ops       = &ntfs_dir_ops,
    .priv_data = (void *)0,
};

/* ---- VFS callback implementations ---- */

static int ntfs_vfs_open(struct vfs_node *node, uint32_t flags)
{
    (void)node;
    /* Read-only: reject write flags */
    if (flags & (VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC))
        return -1;
    return 0;
}

static int ntfs_vfs_close(struct vfs_node *node)
{
    (void)node;
    return 0;  /* Read-only: no flush needed */
}

static int ntfs_vfs_read(struct vfs_node *node, uint32_t offset,
                          uint32_t size, uint8_t *buffer)
{
    struct ntfs_node_data *nd;
    uintptr_t rec_phys;
    uint8_t *rec_buf;
    struct ntfs_mft_header hdr;
    int64_t bytes_read;
    int rc;

    if (!node || !buffer)
        return -1;

    nd = (struct ntfs_node_data *)node->fs_data;
    if (!nd || !nd->vol)
        return -1;

    if (nd->is_directory)
        return -1;  /* Can't read from a directory */

    if (offset >= (uint32_t)nd->file_size)
        return 0;

    if (offset + size > (uint32_t)nd->file_size)
        size = (uint32_t)nd->file_size - offset;

    /* Allocate MFT record buffer */
    rec_phys = pmm_alloc_contiguous(1);
    if (!rec_phys)
        return -1;
    rec_buf = (uint8_t *)(uintptr_t)rec_phys;

    /* Read MFT record */
    rc = ntfs_read_mft_record(nd->vol, nd->inode, rec_buf, &hdr);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        return -1;
    }

    /* Apply fixup */
    rc = ntfs_apply_fixup(rec_buf, nd->vol->frs_size,
                           nd->vol->bytes_per_sector);
    if (rc != NTFS_OK) {
        pmm_free_frame(rec_phys);
        return -1;
    }

    /* Read file data */
    bytes_read = ntfs_read_file_data(rec_buf, &hdr, nd->vol,
                                      (uint64_t)offset, (uint64_t)size,
                                      buffer);
    pmm_free_frame(rec_phys);

    return (bytes_read >= 0) ? (int)bytes_read : -1;
}

/* ---- readdir: enumerate by index ---- */

/* Static dirent for readdir return (single-threaded kernel) */
static struct vfs_dirent ntfs_dirent;

static struct vfs_dirent *ntfs_vfs_readdir(struct vfs_node *node,
                                            uint32_t index)
{
    struct ntfs_node_data *nd;
    uint64_t entry_inode;
    int is_dir;
    uint64_t entry_size;
    int rc;

    if (!node)
        return (struct vfs_dirent *)0;

    nd = (struct ntfs_node_data *)node->fs_data;
    if (!nd || !nd->vol || !nd->is_directory)
        return (struct vfs_dirent *)0;

    rc = ntfs_readdir_entry(nd->vol, nd->inode, index,
                             ntfs_dirent.name, VFS_MAX_NAME,
                             &entry_inode, &is_dir, &entry_size);
    if (rc != NTFS_OK)
        return (struct vfs_dirent *)0;

    ntfs_dirent.inode = (uint32_t)(entry_inode & 0xFFFFFFFF);
    ntfs_dirent.type = is_dir ? VFS_DIRECTORY : VFS_FILE;

    return &ntfs_dirent;
}

/* ---- finddir: name lookup ---- */

/* Static node for finddir return.  We keep a small pool to allow
 * multiple outstanding nodes (e.g., during path resolution). */
#define NTFS_NODE_POOL_SIZE  16

static struct vfs_node ntfs_node_pool[NTFS_NODE_POOL_SIZE];
static struct ntfs_node_data ntfs_nd_pool[NTFS_NODE_POOL_SIZE];
static int ntfs_pool_idx;

static struct vfs_node *alloc_ntfs_node(void)
{
    int idx = ntfs_pool_idx;
    ntfs_pool_idx = (ntfs_pool_idx + 1) % NTFS_NODE_POOL_SIZE;
    return &ntfs_node_pool[idx];
}

static struct vfs_node *ntfs_vfs_finddir(struct vfs_node *node,
                                          const char *name)
{
    struct ntfs_node_data *parent_nd;
    struct vfs_node *child;
    struct ntfs_node_data *child_nd;
    uint64_t child_inode;
    int rc;

    if (!node || !name)
        return (struct vfs_node *)0;

    parent_nd = (struct ntfs_node_data *)node->fs_data;
    if (!parent_nd || !parent_nd->vol || !parent_nd->is_directory)
        return (struct vfs_node *)0;

    rc = ntfs_lookup(parent_nd->vol, parent_nd->inode, name, &child_inode);
    if (rc != NTFS_OK)
        return (struct vfs_node *)0;

    /* Build a VFS node for the child */
    child = alloc_ntfs_node();
    child_nd = &ntfs_nd_pool[((ntfs_pool_idx - 1 + NTFS_NODE_POOL_SIZE)
                               % NTFS_NODE_POOL_SIZE)];

    child_nd->vol = parent_nd->vol;
    child_nd->inode = child_inode;
    child_nd->file_size = 0;
    child_nd->is_directory = 0;

    /* Read the child's MFT record to determine type and size */
    {
        uintptr_t rec_phys = pmm_alloc_contiguous(1);
        if (rec_phys) {
            uint8_t *rec_buf = (uint8_t *)(uintptr_t)rec_phys;
            struct ntfs_mft_header hdr;

            if (ntfs_read_mft_record(parent_nd->vol, child_inode,
                                      rec_buf, &hdr) == NTFS_OK) {
                ntfs_apply_fixup(rec_buf, parent_nd->vol->frs_size,
                                  parent_nd->vol->bytes_per_sector);

                if (hdr.flags & NTFS_MFT_FLAG_DIRECTORY) {
                    child_nd->is_directory = 1;
                } else {
                    /* Get file size from unnamed $DATA attribute */
                    struct ntfs_attr_header ah;
                    const uint8_t *attr;

                    attr = ntfs_attr_find(rec_buf, &hdr,
                                           NTFS_ATTR_DATA, &ah);
                    if (attr) {
                        if (ah.non_resident) {
                            struct ntfs_nonres_header nrh;
                            nrh.real_size = 0;
                            /* Read real_size from non-res header */
                            {
                                const uint8_t *p = attr;
                                nrh.real_size = ((uint64_t)p[0x30]) |
                                    ((uint64_t)p[0x31] << 8) |
                                    ((uint64_t)p[0x32] << 16) |
                                    ((uint64_t)p[0x33] << 24) |
                                    ((uint64_t)p[0x34] << 32) |
                                    ((uint64_t)p[0x35] << 40) |
                                    ((uint64_t)p[0x36] << 48) |
                                    ((uint64_t)p[0x37] << 56);
                            }
                            child_nd->file_size = nrh.real_size;
                        } else {
                            child_nd->file_size = (uint64_t)ah.content_length;
                        }
                    }
                }

                /* Decode filename */
                {
                    struct ntfs_file_name fn;
                    if (ntfs_decode_file_name(rec_buf, &hdr, &fn) == NTFS_OK)
                        ntfs_vfs_strcpy(child->name, fn.name, VFS_MAX_NAME);
                    else
                        ntfs_vfs_strcpy(child->name, name, VFS_MAX_NAME);
                }
            }
            pmm_free_frame(rec_phys);
        } else {
            ntfs_vfs_strcpy(child->name, name, VFS_MAX_NAME);
        }
    }

    child->type = child_nd->is_directory
                  ? (VFS_DIRECTORY) : (VFS_FILE);
    child->inode = (uint32_t)(child_inode & 0xFFFFFFFF);
    child->size = child_nd->file_size;
    child->flags = 0;
    child->ref_count = 0;
    child->ops = child_nd->is_directory ? &ntfs_dir_ops : &ntfs_file_ops;
    child->fs_data = child_nd;
    child->parent = node;

    return child;
}

/* ---- stat ---- */

static int ntfs_vfs_stat(struct vfs_node *node, struct vfs_stat *st)
{
    struct ntfs_node_data *nd;

    if (!node || !st)
        return -1;

    nd = (struct ntfs_node_data *)node->fs_data;

    st->size = nd ? nd->file_size : 0;
    st->type = node->type;
    st->blocks = 0;

    /* Try to read timestamps from $STANDARD_INFORMATION */
    if (nd && nd->vol) {
        uintptr_t rec_phys = pmm_alloc_contiguous(1);
        if (rec_phys) {
            uint8_t *rec_buf = (uint8_t *)(uintptr_t)rec_phys;
            struct ntfs_mft_header hdr;

            if (ntfs_read_mft_record(nd->vol, nd->inode,
                                      rec_buf, &hdr) == NTFS_OK) {
                ntfs_apply_fixup(rec_buf, nd->vol->frs_size,
                                  nd->vol->bytes_per_sector);
                {
                    struct ntfs_std_info si;
                    if (ntfs_decode_std_info(rec_buf, &hdr, &si) == NTFS_OK) {
                        st->ctime = (uint32_t)si.creation_unix;
                        st->mtime = (uint32_t)si.modification_unix;
                        st->atime = (uint32_t)si.access_unix;
                    } else {
                        st->ctime = 0;
                        st->mtime = 0;
                        st->atime = 0;
                    }
                }
            } else {
                st->ctime = 0;
                st->mtime = 0;
                st->atime = 0;
            }
            pmm_free_frame(rec_phys);
        } else {
            st->ctime = 0;
            st->mtime = 0;
            st->atime = 0;
        }
    } else {
        st->ctime = 0;
        st->mtime = 0;
        st->atime = 0;
    }

    return 0;
}

/* ============================================================================
 * ntfs_readdir_entry — enumerate a directory by index
 *
 * Uses ntfs_readdir() callback internally, counting entries until the
 * target index is reached.  Inherits $BITMAP support from ntfs_readdir().
 * ============================================================================ */

/* Context for the index-based readdir callback */
struct readdir_by_index_ctx {
    uint32_t target_index;
    uint32_t current;
    char    *out_name;
    int      out_name_max;
    uint64_t *out_inode;
    int     *out_is_dir;
    uint64_t *out_size;
    int      found;
};

static int readdir_by_index_cb(const struct ntfs_dir_entry *entry,
                                void *user_data)
{
    struct readdir_by_index_ctx *ctx =
        (struct readdir_by_index_ctx *)user_data;

    if (ctx->current == ctx->target_index) {
        ntfs_vfs_strcpy(ctx->out_name, entry->name, ctx->out_name_max);
        *ctx->out_inode = entry->inode;
        *ctx->out_size = entry->file_size;
        *ctx->out_is_dir = entry->is_directory;
        ctx->found = 1;
        return 1;  /* Stop enumeration */
    }
    ctx->current++;
    return 0;  /* Continue */
}

int ntfs_readdir_entry(struct ntfs_volume *vol, uint64_t dir_inode,
                       uint32_t index, char *out_name, int out_name_max,
                       uint64_t *out_inode, int *out_is_dir,
                       uint64_t *out_size)
{
    struct readdir_by_index_ctx ctx;

    if (!vol || !out_name || !out_inode || !out_is_dir || !out_size)
        return NTFS_ERR_IO;

    ctx.target_index = index;
    ctx.current = 0;
    ctx.out_name = out_name;
    ctx.out_name_max = out_name_max;
    ctx.out_inode = out_inode;
    ctx.out_is_dir = out_is_dir;
    ctx.out_size = out_size;
    ctx.found = 0;

    ntfs_readdir(vol, dir_inode, readdir_by_index_cb, &ctx);

    return ctx.found ? NTFS_OK : NTFS_ERR_NOT_FOUND;
}

/* ---- Public API ---- */

/* Root node + its ntfs_node_data are kept in the volume's ntfs_node_data
 * so they survive beyond function scope. We use a small static pool for
 * the root nodes of up to 4 mounted NTFS volumes. */
#define NTFS_MAX_MOUNTS  4

static struct vfs_node ntfs_root_nodes[NTFS_MAX_MOUNTS];
static struct ntfs_node_data ntfs_root_nd[NTFS_MAX_MOUNTS];
static int ntfs_mount_count;

struct vfs_fs_driver *ntfs_get_driver(void)
{
    return &ntfs_driver;
}

struct vfs_node *ntfs_get_root(struct ntfs_volume *vol)
{
    struct vfs_node *root;
    struct ntfs_node_data *nd;
    int idx;

    if (!vol)
        return (struct vfs_node *)0;

    if (ntfs_mount_count >= NTFS_MAX_MOUNTS) {
        klog(LOG_WARN, "ntfs", "Max NTFS mounts reached (%d)",
             (uint64_t)NTFS_MAX_MOUNTS);
        return (struct vfs_node *)0;
    }

    idx = ntfs_mount_count++;
    root = &ntfs_root_nodes[idx];
    nd = &ntfs_root_nd[idx];

    nd->vol = vol;
    nd->inode = NTFS_ROOT_INODE;
    nd->file_size = 0;
    nd->is_directory = 1;

    ntfs_vfs_strcpy(root->name, "\\", VFS_MAX_NAME);
    root->type = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root->inode = NTFS_ROOT_INODE;
    root->size = 0;
    root->flags = 0;
    root->ref_count = 0;
    root->ops = &ntfs_dir_ops;
    root->fs_data = nd;
    root->parent = (struct vfs_node *)0;

    return root;
}
