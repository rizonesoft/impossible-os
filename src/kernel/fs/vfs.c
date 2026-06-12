/* ============================================================================
 * vfs.c -- Virtual Filesystem with Windows-style Drive Letters
 *
 * Implements a VFS layer that:
 *   - Mounts filesystem drivers at drive letters (A:\, C:\, D:\, etc.)
 *   - Parses backslash-separated paths (C:\Users\Default\file.txt)
 *   - Dispatches file operations to the appropriate FS driver
 *
 * Drive letter assignment convention:
 *   A:\ -- EFI System Partition (FAT32, read-only after boot)
 *   C:\ -- Root OS partition (IXFS)
 *   D:\, E:\… -- Additional drives / USB
 * ============================================================================ */

#include "kernel/fs/vfs.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/security/default_sds.h"

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

/* --- Public: ASCII uppercase fold for case-insensitive path resolution --- */

void vfs_path_fold(const char *in, char *out, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && in[i]; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    out[i] = '\0';
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

    klog(LOG_INFO, "vfs", "VFS initialized (26 drive letters A:\\ - Z:\\)");
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
        /* Only overwrite fs_data if the driver provides its own */
        if (driver->priv_data)
            root_node->fs_data = driver->priv_data;
    }

    klog(LOG_INFO, "vfs", "VFS: mounted \"%s\" at %c:\\",
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

/* --- Opportunistic locks --- */

int vfs_request_oplock(struct vfs_node *node, uint32_t owner_id, uint8_t level)
{
    if (!node) return -1;

    if (level == VFS_OPLOCK_LEVEL1) {
        /* Exclusive: only if no existing oplock */
        if (node->oplock_level != VFS_OPLOCK_NONE)
            return -1;
        node->oplock_level = VFS_OPLOCK_LEVEL1;
        node->oplock_owner = owner_id;
        return 0;
    }

    if (level == VFS_OPLOCK_LEVEL2) {
        /* Shared: compatible with other Level 2, not with Level 1 */
        if (node->oplock_level == VFS_OPLOCK_LEVEL1)
            return -1;
        node->oplock_level = VFS_OPLOCK_LEVEL2;
        return 0;
    }

    return -1;
}

void vfs_break_oplock(struct vfs_node *node, uint32_t access)
{
    if (!node || node->oplock_level == VFS_OPLOCK_NONE)
        return;

    if (node->oplock_level == VFS_OPLOCK_LEVEL1) {
        if (access & VFS_O_WRITE) {
            /* Write access breaks Level 1 to None */
            klog(LOG_DEBUG, "vfs", "oplock break: L1 -> None (%s)",
                 node->name);
            node->oplock_level = VFS_OPLOCK_NONE;
            node->oplock_owner = 0;
        } else {
            /* Read access breaks Level 1 to Level 2 */
            klog(LOG_DEBUG, "vfs", "oplock break: L1 -> L2 (%s)",
                 node->name);
            node->oplock_level = VFS_OPLOCK_LEVEL2;
            node->oplock_owner = 0;
        }
    } else if (node->oplock_level == VFS_OPLOCK_LEVEL2) {
        if (access & VFS_O_WRITE) {
            /* Write access breaks Level 2 to None */
            klog(LOG_DEBUG, "vfs", "oplock break: L2 -> None (%s)",
                 node->name);
            node->oplock_level = VFS_OPLOCK_NONE;
        }
    }
}

/* --- Byte-range locks --- */

int vfs_lock_file(struct vfs_node *node, uint32_t owner_id,
                  uint64_t offset, uint64_t length, int exclusive)
{
    uint32_t i;
    uint64_t new_end = offset + length;

    if (!node || length == 0)
        return -1;

    /* Check for conflicts */
    for (i = 0; i < VFS_MAX_LOCKS; i++) {
        vfs_lock_t *l = &node->locks[i];
        uint64_t ex_end;
        if (!l->active)
            continue;
        ex_end = l->offset + l->length;
        /* Overlap test */
        if (!(new_end <= l->offset || offset >= ex_end)) {
            /* Overlapping -- check compatibility */
            if (exclusive || l->exclusive)
                return STATUS_FILE_LOCK_CONFLICT;
            /* Both shared -- compatible */
        }
    }

    /* Find a free slot */
    for (i = 0; i < VFS_MAX_LOCKS; i++) {
        if (!node->locks[i].active) {
            node->locks[i].offset    = offset;
            node->locks[i].length    = length;
            node->locks[i].owner_id  = owner_id;
            node->locks[i].exclusive = (uint8_t)exclusive;
            node->locks[i].active    = 1;
            return 0;
        }
    }

    return -1;  /* no free lock slots */
}

int vfs_unlock_file(struct vfs_node *node, uint32_t owner_id,
                    uint64_t offset, uint64_t length)
{
    uint32_t i;

    if (!node)
        return -1;

    for (i = 0; i < VFS_MAX_LOCKS; i++) {
        vfs_lock_t *l = &node->locks[i];
        if (l->active && l->owner_id == owner_id &&
            l->offset == offset && l->length == length) {
            l->active = 0;
            return 0;
        }
    }

    return -1;  /* STATUS_RANGE_NOT_LOCKED */
}

/* --- Share-mode enforcement --- */

int vfs_check_sharing(struct vfs_node *node, uint32_t access, uint32_t share)
{
    uint32_t i;
    uint32_t new_access = access & (VFS_O_READ | VFS_O_WRITE);
    uint32_t new_share  = share;

    for (i = 0; i < VFS_MAX_HANDLES; i++) {
        vfs_open_handle_t *h = &node->open_handles[i];
        if (!h->active)
            continue;

        /* Check: does the new open conflict with existing handle? */
        if ((new_access & ~h->share_mode) != 0)
            return STATUS_SHARING_VIOLATION;
        /* Check: does the existing handle conflict with new share mode? */
        if ((h->access_mode & ~new_share) != 0)
            return STATUS_SHARING_VIOLATION;
    }

    return 0;
}

/* Returns slot index on success, -1 if no free slot. The slot index is
 * captured by vfs_open so a failed FS-specific open or TRUNC can clear
 * the EXACT slot it created -- a by-access remove would otherwise drop
 * a pre-existing compatible handle's share entry. */
static int vfs_add_handle(struct vfs_node *node, uint32_t access, uint32_t share)
{
    uint32_t i;
    for (i = 0; i < VFS_MAX_HANDLES; i++) {
        if (!node->open_handles[i].active) {
            node->open_handles[i].access_mode = access & (VFS_O_READ | VFS_O_WRITE);
            node->open_handles[i].share_mode  = share;
            node->open_handles[i].active      = 1;
            return (int)i;
        }
    }
    return -1;  /* no free handle slots */
}

/* Exact-slot cleanup -- used by vfs_open failure paths where the slot
 * index is known. Avoids the "by-access" matching that could remove a
 * surviving real handle whose access bits collide. */
static void vfs_remove_handle_at(struct vfs_node *node, int idx)
{
    if (idx < 0 || (uint32_t)idx >= VFS_MAX_HANDLES) return;
    node->open_handles[idx].active = 0;
}

/* By-access cleanup -- vfs_close releases its own handle without a
 * stored slot index; the access bits are sufficient there because the
 * close path tears down ONE handle and the per-handle access bits are
 * unique enough in practice. */
static void vfs_remove_handle(struct vfs_node *node, uint32_t access)
{
    uint32_t i;
    /* Remove first matching handle (LIFO-ish) */
    for (i = 0; i < VFS_MAX_HANDLES; i++) {
        if (node->open_handles[i].active &&
            node->open_handles[i].access_mode == (access & (VFS_O_READ | VFS_O_WRITE))) {
            node->open_handles[i].active = 0;
            return;
        }
    }
}

struct vfs_node *vfs_open(const char *path, uint32_t flags)
{
    const char *rest;
    int idx;
    int slot_idx = -1;
    struct vfs_node *node;
    /* Extract share mode from upper bits of flags (if provided) */
    uint32_t share = (flags >> 8) & 0x07;  /* bits 10:8 = share mode */

    /* Default share mode: share everything (compatible with existing callers) */
    if (share == 0)
        share = VFS_SHARE_READ | VFS_SHARE_WRITE | VFS_SHARE_DELETE;

    idx = parse_drive(path, &rest);
    if (idx < 0 || !mounts[idx].mounted)
        return (struct vfs_node *)0;

    /* Walk the path to find the node */
    node = walk_path(mounts[idx].root, rest);
    if (!node) {
        /* VFS_O_CREATE: create the file if it doesn't exist, then re-walk */
        if (flags & VFS_O_CREATE) {
            if (vfs_create(path, VFS_FILE) != 0)
                return (struct vfs_node *)0;
            node = walk_path(mounts[idx].root, rest);
        }
        if (!node)
            return (struct vfs_node *)0;
    }

    /* VFS-level policy: VFS_O_TRUNC on a directory is silently masked
     * BEFORE FS dispatch, so all 3 backends see consistent flags. NTFS
     * still rejects WRITE on directories via its own open hook; FAT32
     * and IXFS no-op on directory opens. */
    if (!(node->type & VFS_FILE) && (flags & VFS_O_TRUNC))
        flags &= ~VFS_O_TRUNC;

    /* VFS_O_TRUNC requires VFS_O_WRITE on FILES (truncating a read-only
     * file handle is contradictory). Run AFTER the directory mask so a
     * read-only directory open with a stray TRUNC bit is normalized
     * (TRUNC stripped) rather than rejected. */
    if ((node->type & VFS_FILE) &&
        (flags & VFS_O_TRUNC) && !(flags & VFS_O_WRITE))
        return (struct vfs_node *)0;

    /* Oplock break: break existing oplock before share-mode check */
    if (node->type & VFS_FILE)
        vfs_break_oplock(node, flags);

    /* Delete-on-close check: reject new openers on marked nodes */
    if (node->delete_on_close && !(flags & VFS_O_DELETE_ON_CLOSE))
        return (struct vfs_node *)0;  /* STATUS_DELETE_PENDING */

    /* Share-mode check: only for files, not directories */
    if ((node->type & VFS_FILE) && node->ref_count > 0) {
        if (vfs_check_sharing(node, flags, share) != 0)
            return (struct vfs_node *)0;
    }

    /* Mark for delete-on-close */
    if (flags & VFS_O_DELETE_ON_CLOSE)
        node->delete_on_close = 1;

    /* Track this open handle. Capture the exact slot index so failure
     * paths below clear the slot they created -- by-access remove can
     * delete a pre-existing compatible handle's share entry. */
    if (node->type & VFS_FILE) {
        slot_idx = vfs_add_handle(node, flags, share);
        if (slot_idx < 0)
            return (struct vfs_node *)0;  /* handle table full */
    }

    /* Call the FS-specific open if available */
    node->flags = flags;
    if (node->ops && node->ops->open) {
        if (node->ops->open(node, flags) != 0) {
            vfs_remove_handle_at(node, slot_idx);
            return (struct vfs_node *)0;
        }
    }

    /* VFS-level VFS_O_TRUNC: after the FS-specific open succeeds and
     * before ref_count++, truncate to zero. Files only -- directories
     * already had TRUNC masked above. NULL truncate op or non-zero
     * return fails the open and clears the exact handle slot. */
    if ((flags & VFS_O_TRUNC) && (node->type & VFS_FILE)) {
        if (!node->ops || !node->ops->truncate ||
            node->ops->truncate(node, 0) != 0) {
            if (node->ops && node->ops->close)
                node->ops->close(node);
            vfs_remove_handle_at(node, slot_idx);
            return (struct vfs_node *)0;
        }
        node->size = 0;
    }

    node->ref_count++;
    return node;
}

int vfs_flush(struct vfs_node *node)
{
    if (!node)
        return -1;
    if (!node->ops || !node->ops->flush)
        return 0;   /* no cache to flush -- write-through filesystem */
    return node->ops->flush(node);
}

int vfs_close(struct vfs_node *node)
{
    if (!node)
        return -1;

    if (node->ref_count > 0)
        node->ref_count--;

    /* Remove one handle tracking entry */
    if (node->type & VFS_FILE)
        vfs_remove_handle(node, node->flags);

    /* Delete-on-close: if last handle and marked, trigger delete */
    if (node->delete_on_close && node->ref_count == 0) {
        if (node->parent && node->parent->ops && node->parent->ops->unlink)
            node->parent->ops->unlink(node->parent, node->name);
        node->delete_on_close = 0;
    }

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

    /* Prevent deletion of open files */
    if (parent->ops->finddir) {
        struct vfs_node *target = parent->ops->finddir(parent, name);
        if (target && target->ref_count > 0)
            return -1;  /* file is still open */

        /* The finddir above can rebuild the filesystem's directory node
         * cache with THIS directory's children, clobbering the slot that
         * backs `parent` (FAT32 per-volume dir_files cache) -- its
         * fs_data would then describe a child and the unlink would scan
         * the wrong cluster. Re-resolve parent before dispatching. */
        parent = (len > 0) ? walk_path(mounts[idx].root, parent_path)
                           : mounts[idx].root;
        if (!parent || !parent->ops || !parent->ops->unlink)
            return -1;
    }

    return parent->ops->unlink(parent, name);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    return vfs_rename_ex(old_path, new_path, 0);
}

int vfs_rename_ex(const char *old_path, const char *new_path, uint32_t flags)
{
    const char *old_rest, *new_rest;
    int old_idx, new_idx;
    struct vfs_node *old_parent, *new_parent;
    const char *old_name, *new_name;
    const char *p;
    char parent_path[VFS_MAX_PATH];
    uint32_t len;

    /* Both paths must be on the same drive */
    old_idx = parse_drive(old_path, &old_rest);
    new_idx = parse_drive(new_path, &new_rest);
    if (old_idx < 0 || new_idx < 0 || old_idx != new_idx)
        return -1;
    if (!mounts[old_idx].mounted)
        return -1;

    /* Split old_path into parent + name */
    old_name = old_rest;
    p = old_rest;
    while (*p) {
        if (*p == '\\' || *p == '/')
            old_name = p + 1;
        p++;
    }
    len = (uint32_t)(old_name - old_rest);
    if (len > 0) {
        str_copy(parent_path, old_rest, len < VFS_MAX_PATH ? len + 1 : VFS_MAX_PATH);
        parent_path[len > 0 ? len - 1 : 0] = '\0';
        old_parent = walk_path(mounts[old_idx].root, parent_path);
    } else {
        old_parent = mounts[old_idx].root;
    }

    /* Split new_path into parent + name */
    new_name = new_rest;
    p = new_rest;
    while (*p) {
        if (*p == '\\' || *p == '/')
            new_name = p + 1;
        p++;
    }
    len = (uint32_t)(new_name - new_rest);
    if (len > 0) {
        str_copy(parent_path, new_rest, len < VFS_MAX_PATH ? len + 1 : VFS_MAX_PATH);
        parent_path[len > 0 ? len - 1 : 0] = '\0';
        new_parent = walk_path(mounts[new_idx].root, parent_path);
    } else {
        new_parent = mounts[new_idx].root;
    }

    if (!old_parent || !new_parent)
        return -1;

    /* Both must be in the same directory (cross-directory rename not yet supported) */
    if (old_parent != new_parent)
        return -1;

    if (!old_parent->ops || !old_parent->ops->rename)
        return -1;

    if ((flags & VFS_RENAME_REPLACE_EXISTING) && new_parent->ops->finddir) {
        struct vfs_node *dst = new_parent->ops->finddir(new_parent, new_name);
        if (dst && dst->ref_count > 0)
            return -1;
    }

    return old_parent->ops->rename(old_parent, old_name, new_name, flags);
}

int vfs_stat(const char *path, struct vfs_stat *st)
{
    const char *rest;
    int idx;
    struct vfs_node *node;

    if (!path || !st)
        return -1;

    idx = parse_drive(path, &rest);
    if (idx < 0 || !mounts[idx].mounted)
        return -1;

    node = walk_path(mounts[idx].root, rest);
    if (!node)
        return -1;

    /* Call FS-specific stat if available */
    if (node->ops && node->ops->stat)
        return node->ops->stat(node, st);

    /* Fallback: populate from node fields */
    st->size   = node->size;
    st->type   = node->type;
    st->ctime  = 0;
    st->mtime  = 0;
    st->atime  = 0;
    st->blocks = 0;
    return 0;
}

int vfs_truncate(const char *path, uint64_t new_size)
{
    const char *rest;
    int idx;
    struct vfs_node *node;

    if (!path)
        return -1;

    idx = parse_drive(path, &rest);
    if (idx < 0 || !mounts[idx].mounted)
        return -1;

    node = walk_path(mounts[idx].root, rest);
    if (!node)
        return -1;

    /* Can only truncate files, not directories */
    if (node->type & VFS_DIRECTORY)
        return -1;

    if (!node->ops || !node->ops->truncate)
        return -1;

    return node->ops->truncate(node, new_size);
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

/* Bounded strlen for a fixed-size name field.  Returns length up to max,
 * or max if no NUL found within [0, max). */
static uint32_t vfs_name_len_bounded(const char *name, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max; i++) {
        if (name[i] == '\0') return i;
    }
    return max;
}

/* Walk parent pointers to find the drive letter; detects cycles by depth
 * limit and by confirming the terminal node has NULL parent.
 * Returns the letter or '\0' on failure. */
static char vfs_find_drive_letter(const struct vfs_node *node)
{
    const struct vfs_node *cur = node;
    int depth = 0;

    /* Walk to root; bail on cycle (depth exceeded before finding NULL parent) */
    while (cur && cur->parent && depth < 64) {
        cur = cur->parent;
        depth++;
    }

    /* If we hit the depth limit without reaching a root, treat as failure
     * (cycle in parent chain). */
    if (!cur || cur->parent)
        return '\0';

    /* Match root node against mount table */
    {
        int i;
        for (i = 0; i < 26; i++) {
            if (mounts[i].mounted && mounts[i].root == cur)
                return (char)('A' + i);
        }
    }
    return '\0';
}

uint32_t vfs_get_path_from_node(const struct vfs_node *node,
                                char *buf, uint32_t buf_size)
{
    const struct vfs_node *chain[64];
    int depth = 0;
    const struct vfs_node *cur;
    char drive;
    uint32_t pos;
    int i;

    if (!node || !buf || buf_size < 4)
        return 0;

    /* Collect chain from node up to (but not including) root */
    cur = node;
    while (cur && cur->parent && depth < 64) {
        chain[depth++] = cur;
        cur = cur->parent;
    }

    /* Reject cycles: if depth limit was reached before hitting root, fail */
    if (depth == 64 && cur && cur->parent)
        return 0;

    /* cur is the root node; find its drive letter */
    drive = vfs_find_drive_letter(node);
    if (drive == '\0')
        return 0;

    /* Write "X:" prefix */
    pos = 0;
    buf[pos++] = drive;
    buf[pos++] = ':';

    /* Walk chain in reverse (root-to-leaf), appending "\name" each step.
     * Use bounded strlen (VFS_MAX_NAME) to guard against unterminated
     * names; subtraction-form bound check avoids uint32_t overflow. */
    for (i = depth - 1; i >= 0; i--) {
        uint32_t name_len = vfs_name_len_bounded(chain[i]->name, VFS_MAX_NAME);
        uint32_t j;

        /* Need: '\\' + name_len + NUL.  Use subtraction form to avoid overflow. */
        if (name_len >= buf_size || pos >= buf_size ||
            (buf_size - pos) < (name_len + 2))
            return 0;

        buf[pos++] = '\\';
        for (j = 0; j < name_len; j++)
            buf[pos++] = chain[i]->name[j];
    }

    buf[pos] = '\0';
    return pos;
}

/* --- Win32 feature-spoofing stubs --- */

uint32_t vfs_query_volume_flags(char drive_letter, char *fs_name, uint32_t name_max)
{
    int idx = drive_index(drive_letter);
    uint32_t flags = VFS_VOL_UNICODE_ON_DISK | VFS_VOL_CASE_PRESERVED_NAMES;
    const char *name = "UNKNOWN";

    if (idx < 0 || !mounts[idx].mounted) {
        if (fs_name && name_max > 0) fs_name[0] = '\0';
        return 0;
    }

    if (mounts[idx].driver && mounts[idx].driver->name) {
        name = mounts[idx].driver->name;
        /* NTFS and IXFS support persistent ACLs */
        if ((name[0] == 'N' && name[1] == 'T' && name[2] == 'F' && name[3] == 'S') ||
            (name[0] == 'I' && name[1] == 'X' && name[2] == 'F' && name[3] == 'S'))
            flags |= VFS_VOL_PERSISTENT_ACLS;
    }

    if (fs_name && name_max > 0)
        str_copy(fs_name, name, name_max);

    return flags;
}

int vfs_query_streams(struct vfs_node *node, char *stream_name, uint32_t name_max,
                      uint64_t *stream_size)
{
    /* Every file has exactly one stream: the default ::$DATA */
    if (stream_name && name_max > 7)
        str_copy(stream_name, "::$DATA", name_max);
    if (stream_size)
        *stream_size = node ? node->size : 0;
    return 1;  /* 1 stream */
}

const void *vfs_query_security(struct vfs_node *node, uint32_t *out_size)
{
    (void)node;
    /* Return the default SD: SY+BA=Full, WD=ReadControl */
    if (out_size)
        *out_size = SeGetDefaultSDSize(SE_SD_TYPE_DEFAULT);
    return SeCreateDefaultSD(SE_SD_TYPE_DEFAULT);
}

int vfs_query_reparse(struct vfs_node *node)
{
    (void)node;
    /* No filesystem currently supports reparse points */
    return STATUS_NOT_SUPPORTED;
}
