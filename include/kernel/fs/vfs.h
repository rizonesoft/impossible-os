/* ============================================================================
 * vfs.h -- Virtual Filesystem with Windows-style Drive Letters
 *
 * Drive letter mounting: A:\, C:\, D:\ etc.
 * Backslash path parsing: C:\Users\Default\file.txt
 * Pluggable filesystem drivers (IXFS, FAT32).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum limits */
#define VFS_MAX_NAME     256
#define VFS_MAX_PATH     512
#define VFS_MAX_DRIVES   26     /* A: through Z: */

/* Bulletproofing: drive count must be exactly 26 (A-Z) */
_Static_assert(VFS_MAX_DRIVES == 26, "VFS drive letters must be A-Z (26)");

/* Node types */
#define VFS_FILE         0x01
#define VFS_DIRECTORY    0x02
#define VFS_MOUNTPOINT   0x04

/* Open flags */
#define VFS_O_READ       0x01
#define VFS_O_WRITE      0x02
#define VFS_O_CREATE     0x04
#define VFS_O_APPEND     0x08
#define VFS_O_TRUNC      0x10

/* Forward declarations */
struct vfs_node;

/* Directory entry (returned by readdir) */
struct vfs_dirent {
    char     name[VFS_MAX_NAME];
    uint32_t inode;
    uint8_t  type;   /* VFS_FILE or VFS_DIRECTORY */
};

/* File metadata (returned by vfs_stat) */
struct vfs_stat {
    uint64_t size;       /* file size in bytes */
    uint8_t  type;       /* VFS_FILE or VFS_DIRECTORY */
    uint32_t ctime;      /* creation time (seconds since boot) */
    uint32_t mtime;      /* modification time */
    uint32_t atime;      /* access time */
    uint32_t blocks;     /* number of disk blocks used */
};

/* Timestamp for set_times callback (NULL = don't change) */
typedef struct {
    uint32_t seconds;    /* seconds since boot */
} filetime_t;

/* Filesystem driver operations -- implemented by each FS (IXFS, FAT32, etc.) */
struct vfs_ops {
    int      (*open)(struct vfs_node *node, uint32_t flags);
    int      (*close)(struct vfs_node *node);
    int      (*read)(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
    int      (*write)(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer);
    struct vfs_dirent *(*readdir)(struct vfs_node *node, uint32_t index);
    struct vfs_node *(*finddir)(struct vfs_node *node, const char *name);
    int      (*create)(struct vfs_node *parent, const char *name, uint8_t type);
    int      (*unlink)(struct vfs_node *parent, const char *name);
    int      (*rename)(struct vfs_node *parent, const char *old_name,
                       const char *new_name);
    int      (*stat)(struct vfs_node *node, struct vfs_stat *st);
    int      (*truncate)(struct vfs_node *node, uint64_t new_size);
    int      (*mkdir)(struct vfs_node *parent, const char *name);
    int      (*rmdir)(struct vfs_node *parent, const char *name);
    int      (*set_attr)(struct vfs_node *node, uint32_t attributes);
    int      (*set_times)(struct vfs_node *node, const filetime_t *ctime,
                          const filetime_t *mtime, const filetime_t *atime);
    int      (*flush)(struct vfs_node *node);
};

/* VFS node -- represents a file, directory, or mountpoint */
struct vfs_node {
    char             name[VFS_MAX_NAME];
    uint8_t          type;       /* VFS_FILE, VFS_DIRECTORY, VFS_MOUNTPOINT */
    uint32_t         inode;      /* inode number (FS-specific) */
    uint64_t         size;       /* file size in bytes */
    uint32_t         flags;      /* open flags */
    uint32_t         ref_count;  /* open reference count (>0 = in use) */
    struct vfs_ops  *ops;        /* filesystem operations */
    void            *fs_data;    /* filesystem-private data */
    struct vfs_node *parent;     /* parent directory */
};

/* Filesystem driver descriptor -- registered by each FS implementation */
struct vfs_fs_driver {
    const char      *name;       /* "IXFS", "FAT32" */
    struct vfs_ops  *ops;        /* filesystem operations */
    void            *priv_data;  /* driver-private data */
};

/* --- VFS API --- */

/* Initialize the VFS */
void vfs_init(void);

/* Mount a filesystem driver at a drive letter ('A' through 'Z').
 * root_node is the root directory node of the mounted filesystem. */
int vfs_mount(char drive_letter, struct vfs_fs_driver *driver, struct vfs_node *root_node);

/* Unmount a drive letter */
int vfs_unmount(char drive_letter);

/* Open a file/directory by path (e.g. "C:\\Users\\file.txt") */
struct vfs_node *vfs_open(const char *path, uint32_t flags);

/* Close a file/directory */
int vfs_close(struct vfs_node *node);

/* Read from an open file */
int vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);

/* Write to an open file */
int vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer);

/* Read a directory entry at index */
struct vfs_dirent *vfs_readdir(struct vfs_node *dir_node, uint32_t index);

/* Find a named entry in a directory */
struct vfs_node *vfs_finddir(struct vfs_node *dir_node, const char *name);

/* Create a file or directory */
int vfs_create(const char *path, uint8_t type);

/* Delete a file or directory */
int vfs_unlink(const char *path);

/* Rename a file or directory (both paths must be on the same drive) */
int vfs_rename(const char *old_path, const char *new_path);

/* Get file/directory metadata without opening */
int vfs_stat(const char *path, struct vfs_stat *st);

/* Resize a file (truncate or extend) */
int vfs_truncate(const char *path, uint64_t new_size);

/* Get the root node of a mounted drive */
struct vfs_node *vfs_get_drive_root(char drive_letter);

/* Check if a drive letter is mounted */
int vfs_is_mounted(char drive_letter);
