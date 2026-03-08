/* ============================================================================
 * firstboot.c — First-Boot Setup Module
 *
 * On the very first boot (empty IXFS C:\ partition), this module:
 *   1. Creates the default Windows-like directory hierarchy
 *   2. Copies essential files from initrd (B:\) to C:\Impossible\System\
 *
 * Removal: To disable first-boot setup, delete this file and firstboot.h,
 *          and remove the firstboot_setup() call from main.c.
 * ============================================================================ */

#include "kernel/fs/firstboot.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/heap.h"
#include "kernel/printk.h"

/* ---------- helpers ---------- */

static void fb_strcpy(char *dst, const char *src)
{
    while (*src)
        *dst++ = *src++;
    *dst = '\0';
}

static int fb_strlen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

/* Copy a single file from src_dir to dst_dir using the given filename.
 * Allocates a temporary buffer for the file data. */
static int fb_copy_file(struct vfs_node *src_dir, struct vfs_node *dst_dir,
                        const char *name)
{
    struct vfs_node *src_file;
    uint8_t *buf;
    int bytes_read;

    if (!src_dir || !dst_dir || !name)
        return -1;

    /* Find the source file */
    if (!src_dir->ops || !src_dir->ops->finddir)
        return -1;

    src_file = src_dir->ops->finddir(src_dir, name);
    if (!src_file || src_file->size == 0)
        return -1;

    /* Allocate buffer for file contents */
    buf = (uint8_t *)kmalloc(src_file->size);
    if (!buf) return -1;

    /* Read the entire source file */
    if (!src_file->ops || !src_file->ops->read) {
        kfree(buf);
        return -1;
    }

    bytes_read = src_file->ops->read(src_file, 0, src_file->size, buf);
    if (bytes_read <= 0) {
        kfree(buf);
        return -1;
    }

    /* Create the file on the destination */
    if (!dst_dir->ops || !dst_dir->ops->create) {
        kfree(buf);
        return -1;
    }

    dst_dir->ops->create(dst_dir, name, VFS_FILE);

    /* Find the newly created file and write to it */
    {
        struct vfs_node *dst_file = dst_dir->ops->finddir(dst_dir, name);
        if (dst_file && dst_file->ops && dst_file->ops->write) {
            dst_file->ops->write(dst_file, 0, (uint32_t)bytes_read, buf);
        }
    }

    kfree(buf);
    return 0;
}

/* ---------- directory hierarchy ---------- */

static void fb_create_hierarchy(struct vfs_node *root)
{
    struct vfs_node *imp;
    struct vfs_node *usr;

    if (!root || !root->ops || !root->ops->create)
        return;

    /* Top-level directories */
    root->ops->create(root, "Impossible", VFS_DIRECTORY);
    root->ops->create(root, "Users",      VFS_DIRECTORY);
    root->ops->create(root, "Programs",   VFS_DIRECTORY);

    /* Impossible\ subdirectories */
    imp = root->ops->finddir(root, "Impossible");
    if (imp && imp->ops && imp->ops->create) {
        imp->ops->create(imp, "System",   VFS_DIRECTORY);
        imp->ops->create(imp, "Commands", VFS_DIRECTORY);
    }

    /* Users\ subdirectories */
    usr = root->ops->finddir(root, "Users");
    if (usr && usr->ops && usr->ops->create) {
        usr->ops->create(usr, "Default", VFS_DIRECTORY);
    }

    printk("[OK] First boot: created default hierarchy on C:\\\n");
}

/* ---------- initrd file copy ---------- */

static void fb_copy_initrd_files(struct vfs_node *root)
{
    struct vfs_node *initrd_root;
    struct vfs_node *sys_dir;
    struct vfs_node *imp_dir;
    uint32_t idx;
    int copied = 0;

    /* Get initrd root (B:\) */
    if (!vfs_is_mounted('B'))
        return;

    initrd_root = vfs_get_drive_root('B');
    if (!initrd_root || !initrd_root->ops || !initrd_root->ops->readdir)
        return;

    /* Target: C:\Impossible\System\ */
    imp_dir = root->ops->finddir(root, "Impossible");
    if (!imp_dir) return;

    sys_dir = imp_dir->ops->finddir(imp_dir, "System");
    if (!sys_dir) return;

    /* Copy every file from B:\ to C:\Impossible\System\ */
    for (idx = 0; ; idx++) {
        struct vfs_dirent *de = initrd_root->ops->readdir(initrd_root, idx);
        if (!de) break;

        /* Skip directories (initrd is flat) */
        if (de->type == VFS_DIRECTORY)
            continue;

        if (fb_copy_file(initrd_root, sys_dir, de->name) == 0)
            copied++;
    }

    if (copied > 0) {
        printk("[OK] First boot: copied %u files from B:\\ to C:\\Impossible\\System\\",
               (uint64_t)copied);
        printk("\n");
    }
}

/* ---------- public API ---------- */

int firstboot_setup(void)
{
    struct vfs_node *c_root;

    /* Both C:\ and B:\ must be mounted */
    if (!vfs_is_mounted('C'))
        return 0;  /* no system drive — nothing to do */

    c_root = vfs_get_drive_root('C');
    if (!c_root)
        return 0;

    /* Detect first boot: check if root directory has any entries
     * (excluding . and ..) — if readdir(root, 0) returns NULL,
     * the directory is empty → first boot */
    if (c_root->ops && c_root->ops->readdir) {
        struct vfs_dirent *first = c_root->ops->readdir(c_root, 0);
        if (first != (void *)0) {
            /* Not first boot — C:\ already has files */
            return 0;
        }
    }

    printk("[....] First boot detected — populating C:\\\n");

    fb_create_hierarchy(c_root);
    fb_copy_initrd_files(c_root);

    (void)fb_strlen;
    (void)fb_strcpy;

    return 0;
}
