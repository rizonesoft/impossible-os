/* ============================================================================
 * initrd.h — Initial RAM Filesystem
 *
 * Simple flat archive: header + file entries + data.
 * Mounted at C:\ during early boot via VFS.
 *
 * Archive format:
 *   [initrd_header]                  — magic + file count
 *   [initrd_file_entry] × count      — name, offset, size per file
 *   [file data...]                   — raw file contents
 * ============================================================================ */

#pragma once

#include "types.h"
#include "vfs.h"

/* Archive magic: "IXRD" = 0x44525849 */
#define INITRD_MAGIC     0x44525849

/* Maximum files in an initrd */
#define INITRD_MAX_FILES 64

/* Maximum filename length */
#define INITRD_MAX_NAME  64

/* On-disk archive header */
struct initrd_header {
    uint32_t magic;
    uint32_t file_count;
} __attribute__((packed));

/* On-disk file entry */
struct initrd_file_entry {
    char     name[INITRD_MAX_NAME];
    uint32_t offset;     /* byte offset from start of archive */
    uint32_t size;       /* file size in bytes */
} __attribute__((packed));

/* Initialize the initrd from a memory address (loaded by GRUB as a module) */
void initrd_init(uintptr_t initrd_addr, uint64_t initrd_size);

/* Get the VFS driver for the initrd (to mount at a drive letter) */
struct vfs_fs_driver *initrd_get_driver(void);

/* Get the root VFS node of the initrd */
struct vfs_node *initrd_get_root(void);
