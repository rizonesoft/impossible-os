/* ============================================================================
 * initrd.c — Initial RAM Filesystem
 *
 * Parses a flat archive loaded by GRUB as a Multiboot2 module and
 * provides VFS operations (open, read, readdir, finddir) to the VFS layer.
 *
 * The archive is read-only (it lives in memory loaded by the bootloader).
 * ============================================================================ */

#include "initrd.h"
#include "printk.h"

/* --- In-memory file representation --- */
struct initrd_file {
    struct vfs_node  node;         /* VFS node for this file */
    uint8_t         *data;         /* pointer to file data in memory */
    uint32_t         data_size;    /* file data size */
};

/* Static storage for files and root */
static struct initrd_file files[INITRD_MAX_FILES];
static uint32_t file_count;
static struct vfs_node root_node;
static struct vfs_dirent dir_entry;   /* reused for readdir */

/* Base address of the initrd in memory */
static uintptr_t initrd_base;

/* --- String helpers --- */
static void initrd_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int initrd_strcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* --- VFS operations for files --- */

static int initrd_file_open(struct vfs_node *node, uint32_t flags)
{
    (void)node;
    (void)flags;
    return 0;   /* always succeeds — data is in memory */
}

static int initrd_file_close(struct vfs_node *node)
{
    (void)node;
    return 0;
}

static int initrd_file_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer)
{
    struct initrd_file *f;
    uint32_t i;
    uint32_t to_read;

    /* Find our file struct from the node */
    f = (struct initrd_file *)node->fs_data;
    if (!f)
        return -1;

    if (offset >= f->data_size)
        return 0;   /* EOF */

    to_read = size;
    if (offset + to_read > f->data_size)
        to_read = f->data_size - offset;

    for (i = 0; i < to_read; i++)
        buffer[i] = f->data[offset + i];

    return (int)to_read;
}

static struct vfs_ops initrd_file_ops = {
    .open  = initrd_file_open,
    .close = initrd_file_close,
    .read  = initrd_file_read,
    .write = (void *)0,           /* read-only */
    .readdir = (void *)0,
    .finddir = (void *)0,
    .create  = (void *)0,
    .unlink  = (void *)0,
};

/* --- VFS operations for the root directory --- */

static struct vfs_dirent *initrd_readdir(struct vfs_node *node, uint32_t index)
{
    (void)node;

    if (index >= file_count)
        return (struct vfs_dirent *)0;

    initrd_strcpy(dir_entry.name, files[index].node.name, VFS_MAX_NAME);
    dir_entry.inode = files[index].node.inode;
    dir_entry.type = VFS_FILE;

    return &dir_entry;
}

static struct vfs_node *initrd_finddir(struct vfs_node *node, const char *name)
{
    uint32_t i;
    (void)node;

    for (i = 0; i < file_count; i++) {
        if (initrd_strcmp(files[i].node.name, name))
            return &files[i].node;
    }

    return (struct vfs_node *)0;
}

static int initrd_root_open(struct vfs_node *node, uint32_t flags)
{
    (void)node;
    (void)flags;
    return 0;
}

static struct vfs_ops initrd_dir_ops = {
    .open    = initrd_root_open,
    .close   = (void *)0,
    .read    = (void *)0,
    .write   = (void *)0,
    .readdir = initrd_readdir,
    .finddir = initrd_finddir,
    .create  = (void *)0,
    .unlink  = (void *)0,
};

/* --- FS driver descriptor --- */
static struct vfs_fs_driver initrd_driver = {
    .name      = "initrd",
    .ops       = &initrd_dir_ops,
    .priv_data = (void *)0,
};

/* --- Public API --- */

void initrd_init(uintptr_t initrd_addr, uint64_t initrd_size)
{
    struct initrd_header *header;
    struct initrd_file_entry *entries;
    uint32_t i;

    initrd_base = initrd_addr;

    header = (struct initrd_header *)initrd_addr;

    /* Validate magic */
    if (header->magic != INITRD_MAGIC) {
        printk("[FAIL] initrd: bad magic (got 0x%x, expected 0x%x)\n",
               (uint64_t)header->magic, (uint64_t)INITRD_MAGIC);
        return;
    }

    file_count = header->file_count;
    if (file_count > INITRD_MAX_FILES)
        file_count = INITRD_MAX_FILES;

    /* File entries follow immediately after the header */
    entries = (struct initrd_file_entry *)(initrd_addr + sizeof(struct initrd_header));

    /* Set up the root directory node */
    initrd_strcpy(root_node.name, "C:\\", VFS_MAX_NAME);
    root_node.type = VFS_DIRECTORY | VFS_MOUNTPOINT;
    root_node.inode = 0;
    root_node.size = 0;
    root_node.flags = 0;
    root_node.ops = &initrd_dir_ops;
    root_node.fs_data = (void *)0;
    root_node.parent = (struct vfs_node *)0;

    /* Parse each file entry */
    for (i = 0; i < file_count; i++) {
        initrd_strcpy(files[i].node.name, entries[i].name, VFS_MAX_NAME);
        files[i].node.type = VFS_FILE;
        files[i].node.inode = i + 1;
        files[i].node.size = entries[i].size;
        files[i].node.flags = 0;
        files[i].node.ops = &initrd_file_ops;
        files[i].node.fs_data = &files[i];
        files[i].node.parent = &root_node;

        files[i].data = (uint8_t *)(initrd_addr + entries[i].offset);
        files[i].data_size = entries[i].size;
    }

    printk("[OK] initrd: %u files, %u bytes\n",
           (uint64_t)file_count, initrd_size);

    (void)initrd_size;
}

struct vfs_fs_driver *initrd_get_driver(void)
{
    return &initrd_driver;
}

struct vfs_node *initrd_get_root(void)
{
    return &root_node;
}
