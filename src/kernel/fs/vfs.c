/* ============================================================================
 * vfs.c — Virtual Filesystem with Windows-style Drive Letters
 *
 * Implements a VFS layer that:
 *   - Mounts filesystem drivers at drive letters (A:\, C:\, D:\, etc.)
 *   - Parses backslash-separated paths (C:\Users\Default\file.txt)
 *   - Dispatches file operations to the appropriate FS driver
 *
 * Drive letter assignment convention:
 *   A:\ — EFI System Partition (FAT32, read-only after boot)
 *   C:\ — Root OS partition (IXFS)
 *   D:\, E:\… — Additional drives / USB
 * ============================================================================ */

#include "vfs.h"
#include "heap.h"
#include "printk.h"

/* Drive mount table: one entry per drive letter A-Z */
struct drive_mount {
    uint8_t             mounted;   /* 1 if a FS is mounted here */
    char                letter;    /* drive letter 'A'-'Z' */
    struct vfs_fs_driver *driver;  /* filesystem driver */
    struct vfs_node     *root;     /* root directory node */
};

static struct drive_mount mounts[VFS_MAX_DRIVES];

/* --- Internal: convert drive letter to index --- */
static int drive_index(char letter)
{
    if (letter >= 'a' && letter <= 'z')
        letter -= 32;   /* to uppercase */
    if (letter < 'A' || letter > 'Z')
        return -1;
    return letter - 'A';
}

/* --- Internal: parse drive letter from path --- */
/* Returns the drive index and sets *rest to point after "X:\" */
static int parse_drive(const char *path, const char **rest)
{
    int idx;

    /* Path must be at least 3 chars: "X:\" or "X:/" */
    if (!path || !path[0] || path[1] != ':')
        return -1;

    if (path[2] != '\\' && path[2] != '/' && path[2] != '\0')
        return -1;

    idx = drive_index(path[0]);
    if (idx < 0)
        return -1;

    /* Point rest to the path after "X:\" */
    if (path[2] == '\\' || path[2] == '/')
        *rest = &path[3];
    else
        *rest = &path[2];

    return idx;
}

/* --- Internal: string helpers --- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t str_len(const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* --- Internal: walk a path from a root node --- */
/* Splits path by backslash and walks each component via finddir */
static struct vfs_node *walk_path(struct vfs_node *root, const char *path)
{
    struct vfs_node *current = root;
    char component[VFS_MAX_NAME];
    uint32_t ci = 0;
    const char *p = path;

    /* Empty path = root */
    if (!path || !path[0])
        return root;

    while (1) {
        if (*p == '\\' || *p == '/' || *p == '\0') {
            if (ci > 0) {
                component[ci] = '\0';

                /* Look up this component in the current directory */
                if (!current->ops || !current->ops->finddir)
                    return (struct vfs_node *)0;

                current = current->ops->finddir(current, component);
                if (!current)
                    return (struct vfs_node *)0;

                ci = 0;
            }

            if (*p == '\0')
                break;
        } else {
            if (ci < VFS_MAX_NAME - 1)
                component[ci++] = *p;
        }
        p++;
    }

    return current;
}

/* --- Public API --- */

void vfs_init(void)
{
    uint32_t i;

    for (i = 0; i < VFS_MAX_DRIVES; i++) {
        mounts[i].mounted = 0;
        mounts[i].letter = (char)('A' + i);
        mounts[i].driver = (struct vfs_fs_driver *)0;
        mounts[i].root = (struct vfs_node *)0;
    }

    printk("[OK] VFS initialized (26 drive letters A:\\ - Z:\\)\n");
}

int vfs_mount(char drive_letter, struct vfs_fs_driver *driver, struct vfs_node *root_node)
{
    int idx = drive_index(drive_letter);

    if (idx < 0)
        return -1;

    if (mounts[idx].mounted) {
        printk("[WARN] VFS: drive %c:\\ already mounted\n", mounts[idx].letter);
        return -1;
    }

    mounts[idx].mounted = 1;
    mounts[idx].driver = driver;
    mounts[idx].root = root_node;

    /* Set root node properties */
    if (root_node) {
        root_node->type = VFS_DIRECTORY | VFS_MOUNTPOINT;
        root_node->ops = driver->ops;
        root_node->fs_data = driver->priv_data;
    }

    printk("[OK] VFS: mounted \"%s\" at %c:\\\n",
           driver->name, mounts[idx].letter);

    return 0;
}

int vfs_unmount(char drive_letter)
{
    int idx = drive_index(drive_letter);

    if (idx < 0)
        return -1;

    if (!mounts[idx].mounted)
        return -1;

    mounts[idx].mounted = 0;
    mounts[idx].driver = (struct vfs_fs_driver *)0;
    mounts[idx].root = (struct vfs_node *)0;

    return 0;
}

struct vfs_node *vfs_open(const char *path, uint32_t flags)
{
    const char *rest;
    int idx;
    struct vfs_node *node;

    idx = parse_drive(path, &rest);
    if (idx < 0 || !mounts[idx].mounted)
        return (struct vfs_node *)0;

    /* Walk the path to find the node */
    node = walk_path(mounts[idx].root, rest);
    if (!node)
        return (struct vfs_node *)0;

    /* Call the FS-specific open if available */
    node->flags = flags;
    if (node->ops && node->ops->open) {
        if (node->ops->open(node, flags) != 0)
            return (struct vfs_node *)0;
    }

    return node;
}

int vfs_close(struct vfs_node *node)
{
    if (!node)
        return -1;

    if (node->ops && node->ops->close)
        return node->ops->close(node);

    return 0;
}

int vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    if (!node || !node->ops || !node->ops->read)
        return -1;

    return node->ops->read(node, offset, size, buffer);
}

int vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer)
{
    if (!node || !node->ops || !node->ops->write)
        return -1;

    return node->ops->write(node, offset, size, buffer);
}

struct vfs_dirent *vfs_readdir(struct vfs_node *dir_node, uint32_t index)
{
    if (!dir_node || !(dir_node->type & VFS_DIRECTORY))
        return (struct vfs_dirent *)0;

    if (!dir_node->ops || !dir_node->ops->readdir)
        return (struct vfs_dirent *)0;

    return dir_node->ops->readdir(dir_node, index);
}

struct vfs_node *vfs_finddir(struct vfs_node *dir_node, const char *name)
{
    if (!dir_node || !(dir_node->type & VFS_DIRECTORY))
        return (struct vfs_node *)0;

    if (!dir_node->ops || !dir_node->ops->finddir)
        return (struct vfs_node *)0;

    return dir_node->ops->finddir(dir_node, name);
}

int vfs_create(const char *path, uint8_t type)
{
    const char *rest;
    int idx;
    struct vfs_node *parent;
    const char *name;
    const char *p;
    char parent_path[VFS_MAX_PATH];
    uint32_t len;

    idx = parse_drive(path, &rest);
    if (idx < 0 || !mounts[idx].mounted)
        return -1;

    /* Find the last path separator to split parent/name */
    name = rest;
    p = rest;
    while (*p) {
        if (*p == '\\' || *p == '/')
            name = p + 1;
        p++;
    }

    /* Build parent path */
    len = (uint32_t)(name - rest);
    if (len > 0) {
        str_copy(parent_path, rest, len < VFS_MAX_PATH ? len + 1 : VFS_MAX_PATH);
        parent_path[len > 0 ? len - 1 : 0] = '\0';   /* remove trailing slash */
        parent = walk_path(mounts[idx].root, parent_path);
    } else {
        parent = mounts[idx].root;
    }

    if (!parent || !parent->ops || !parent->ops->create)
        return -1;

    return parent->ops->create(parent, name, type);

    (void)str_eq;
    (void)str_len;
}

int vfs_unlink(const char *path)
{
    const char *rest;
    int idx;
    struct vfs_node *parent;
    const char *name;
    const char *p;
    char parent_path[VFS_MAX_PATH];
    uint32_t len;

    idx = parse_drive(path, &rest);
    if (idx < 0 || !mounts[idx].mounted)
        return -1;

    name = rest;
    p = rest;
    while (*p) {
        if (*p == '\\' || *p == '/')
            name = p + 1;
        p++;
    }

    len = (uint32_t)(name - rest);
    if (len > 0) {
        str_copy(parent_path, rest, len < VFS_MAX_PATH ? len + 1 : VFS_MAX_PATH);
        parent_path[len > 0 ? len - 1 : 0] = '\0';
        parent = walk_path(mounts[idx].root, parent_path);
    } else {
        parent = mounts[idx].root;
    }

    if (!parent || !parent->ops || !parent->ops->unlink)
        return -1;

    return parent->ops->unlink(parent, name);
}

struct vfs_node *vfs_get_drive_root(char drive_letter)
{
    int idx = drive_index(drive_letter);

    if (idx < 0 || !mounts[idx].mounted)
        return (struct vfs_node *)0;

    return mounts[idx].root;
}

int vfs_is_mounted(char drive_letter)
{
    int idx = drive_index(drive_letter);

    if (idx < 0)
        return 0;

    return mounts[idx].mounted;
}
