/* ============================================================================
 * test_ixfs_core.c — Smoke test for the shared IXFS parser + disk I/O layer
 *
 * Usage: test_ixfs_core <disk-image> <partition-index>
 * Example: test_ixfs_core build/system-disk.img 2
 * ============================================================================ */

#include "ixfs-core.h"
#include "ixfs-disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Callback: print each directory entry */
static int print_entry(const struct ixfs_dir_entry *de,
                       const struct ixfs_inode *inode, void *ctx)
{
    (void)ctx;
    const char *type = (inode->i_mode & IXFS_S_DIR) ? "DIR " : "FILE";
    printf("  %s  %6llu  %s\n", type,
           (unsigned long long)inode->i_size, de->d_name);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <disk-image> <partition-index>\n", argv[0]);
        return 1;
    }

    const char *image_path = argv[1];
    int part_idx = atoi(argv[2]);

    /* Open disk image via GPT parser */
    ixfs_disk_ctx_t *disk = disk_open(image_path, part_idx);
    if (!disk)
        return 1;

    printf("Disk: partition %d at offset %llu (%llu bytes)\n",
           part_idx,
           (unsigned long long)disk->part_offset,
           (unsigned long long)disk->part_size);

    /* Test sector read — read first sector, verify IXFS magic */
    {
        uint8_t sector[512];
        if (disk_read_sectors(disk, 0, 1, sector) != 0) {
            fprintf(stderr, "Failed to read sector 0\n");
            disk_close(disk);
            return 1;
        }
        uint32_t magic = (uint32_t)sector[0] | ((uint32_t)sector[1] << 8) |
                         ((uint32_t)sector[2] << 16) | ((uint32_t)sector[3] << 24);
        printf("Sector 0 magic: 0x%08X %s\n", magic,
               magic == 0x49584653 ? "(IXFS OK)" : "(NOT IXFS!)");
        if (magic != 0x49584653) {
            disk_close(disk);
            return 1;
        }
    }

    /* Open IXFS volume */
    ixfs_vol_t *vol = ixfs_open(disk);
    if (!vol) {
        disk_close(disk);
        return 1;
    }

    printf("IXFS: \"%s\" v%u, %llu blocks (%llu free)\n",
           vol->sb.s_volume_name,
           vol->sb.s_version,
           (unsigned long long)vol->sb.s_total_blocks,
           (unsigned long long)vol->sb.s_free_blocks);

    /* Read root directory */
    struct ixfs_inode root;
    if (ixfs_read_inode(vol, vol->sb.s_root_inode, &root) != 0) {
        fprintf(stderr, "Failed to read root inode\n");
        ixfs_close(vol);
        disk_close(disk);
        return 1;
    }

    printf("\nROOT directory (inode %u):\n", vol->sb.s_root_inode);
    ixfs_readdir(vol, &root, print_entry, NULL);

    /* Lookup test */
    printf("\nLookup test: ");
    uint32_t ino = ixfs_lookup(vol, &root, "Impossible");
    if (ino != 0) {
        struct ixfs_inode dir_inode;
        if (ixfs_read_inode(vol, ino, &dir_inode) == 0 &&
            (dir_inode.i_mode & IXFS_S_DIR)) {
            printf("Impossible/ found (inode %u)\n", ino);

            printf("\nImpossible/ contents:\n");
            ixfs_readdir(vol, &dir_inode, print_entry, NULL);
        }
    } else {
        printf("Impossible/ not found in root\n");
    }

    ixfs_close(vol);
    disk_close(disk);
    printf("\nAll tests passed.\n");
    return 0;
}
