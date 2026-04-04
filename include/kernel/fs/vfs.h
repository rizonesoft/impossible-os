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
#define VFS_O_DELETE_ON_CLOSE  0x20

/* Share mode flags (compatible with Win32 FILE_SHARE_*) */
#define VFS_SHARE_READ   0x01
#define VFS_SHARE_WRITE  0x02
#define VFS_SHARE_DELETE 0x04

/* Per-file open handle tracking (for share-mode enforcement) */
#define VFS_MAX_HANDLES  8
#define VFS_MAX_LOCKS    16

/* Byte-range lock entry */
typedef struct {
    uint64_t offset;
    uint64_t length;
    uint32_t owner_id;      /* opaque handle identifier */
    uint8_t  exclusive;     /* 1 = exclusive, 0 = shared */
    uint8_t  active;        /* 1 = slot in use */
} vfs_lock_t;

typedef struct {
    uint32_t access_mode;   /* VFS_O_READ | VFS_O_WRITE */
    uint32_t share_mode;    /* VFS_SHARE_READ | VFS_SHARE_WRITE | VFS_SHARE_DELETE */
    uint8_t  active;        /* 1 = slot in use */
} vfs_open_handle_t;

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
    /* Share-mode enforcement (§8) */
    vfs_open_handle_t open_handles[VFS_MAX_HANDLES];
    uint8_t          delete_on_close;  /* 1 = delete when last handle closes (§9) */
    /* Byte-range locks (§10) */
    vfs_lock_t       locks[VFS_MAX_LOCKS];
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

/* ASCII uppercase fold for case-insensitive path resolution.
 * Copies in -> out, uppercasing a-z. Preserves \, /, :. */
void vfs_path_fold(const char *in, char *out, uint32_t max);

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

/* Check share-mode compatibility for a new open.
 * Returns 0 if compatible, STATUS_SHARING_VIOLATION on conflict.
 * Called internally by vfs_open(); exposed for testing. */
int vfs_check_sharing(struct vfs_node *node, uint32_t access, uint32_t share);

/* --- Win32 feature-spoofing stubs (§11) --- */

/* Volume filesystem attribute flags (Win32 FILE_FS_ATTRIBUTE_INFORMATION) */
#define VFS_VOL_UNICODE_ON_DISK       0x00000004
#define VFS_VOL_CASE_PRESERVED_NAMES  0x00000002
#define VFS_VOL_PERSISTENT_ACLS       0x00000008

/* Query volume attributes for a drive letter.
 * Returns filesystem attributes flags, max component length, and FS name. */
uint32_t vfs_query_volume_flags(char drive_letter, char *fs_name, uint32_t name_max);

/* Query the default stream info for a file (ADS stub).
 * Always returns 1 stream: "::$DATA" with length = file size.
 * Returns the number of streams (always 1). */
int vfs_query_streams(struct vfs_node *node, char *stream_name, uint32_t name_max,
                      uint64_t *stream_size);

/* Query the default security descriptor for a file.
 * Returns a pointer to a static self-relative SD (do not free).
 * out_size receives the SD byte size. */
const void *vfs_query_security(struct vfs_node *node, uint32_t *out_size);

/* Query reparse point status.
 * Returns STATUS_NOT_A_REPARSE_POINT for all non-reparse nodes. */
int vfs_query_reparse(struct vfs_node *node);

/* Byte-range lock: acquire a lock on a file range.
 * Returns 0 on success, STATUS_FILE_LOCK_CONFLICT on conflict. */
int vfs_lock_file(struct vfs_node *node, uint32_t owner_id,
                  uint64_t offset, uint64_t length, int exclusive);

/* Byte-range unlock: release a lock matching owner + offset + length.
 * Returns 0 on success, -1 if no matching lock found. */
int vfs_unlock_file(struct vfs_node *node, uint32_t owner_id,
                    uint64_t offset, uint64_t length);
