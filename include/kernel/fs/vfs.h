/* ============================================================================
 * vfs.h — Virtual Filesystem with Windows-style Drive Letters
 *
 * Drive letter mounting: A:\, C:\, D:\ etc.
 * Backslash path parsing: C:\Users\Default\file.txt
 * Pluggable filesystem drivers (IXFS, FAT32, initrd).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum limits */
#define VFS_MAX_NAME     256
#define VFS_MAX_PATH     512
#define VFS_MAX_DRIVES   26     /* A: through Z: */

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

/* Filesystem driver operations — implemented by each FS (IXFS, FAT32, etc.) */
struct vfs_ops {
    int      (*open)(struct vfs_node *node, uint32_t flags);
    int      (*close)(struct vfs_node *node);
    int      (*read)(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
    int      (*write)(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer);
    struct vfs_dirent *(*readdir)(struct vfs_node *node, uint32_t index);
    struct vfs_node *(*finddir)(struct vfs_node *node, const char *name);
    int      (*create)(struct vfs_node *parent, const char *name, uint8_t type);
    int      (*unlink)(struct vfs_node *parent, const char *name);
};

/* VFS node — represents a file, directory, or mountpoint */
struct vfs_node {
    char             name[VFS_MAX_NAME];
    uint8_t          type;       /* VFS_FILE, VFS_DIRECTORY, VFS_MOUNTPOINT */
    uint32_t         inode;      /* inode number (FS-specific) */
    uint64_t         size;       /* file size in bytes */
    uint32_t         flags;      /* open flags */
    struct vfs_ops  *ops;        /* filesystem operations */
    void            *fs_data;    /* filesystem-private data */
    struct vfs_node *parent;     /* parent directory */
};

/* Filesystem driver descriptor — registered by each FS implementation */
struct vfs_fs_driver {
    const char      *name;       /* "IXFS", "FAT32", "initrd" */
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

/* Get the root node of a mounted drive */
struct vfs_node *vfs_get_drive_root(char drive_letter);

/* Check if a drive letter is mounted */
int vfs_is_mounted(char drive_letter);
