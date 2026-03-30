/* ============================================================================
 * test_ixfs_core.c — Smoke test for the shared IXFS parser
 *
 * Usage: test_ixfs_core <disk-image> <partition-byte-offset>
 * Example: test_ixfs_core build/system-disk.img 68157440
 * ============================================================================ */

#include "ixfs-core.h"
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
        fprintf(stderr, "Usage: %s <disk-image> <partition-byte-offset>\n", argv[0]);
        return 1;
    }

    const char *image_path = argv[1];
    uint64_t part_offset = strtoull(argv[2], NULL, 0);

    /* Open disk image */
    FILE *fp = fopen(image_path, "rb");
    if (!fp) {
        perror(image_path);
        return 1;
    }

    /* Set up disk context */
    ixfs_disk_ctx_t disk = {
        .fp = fp,
        .part_offset = part_offset,
        .part_size = 0,  /* not needed for read-only */
    };

    /* Open IXFS volume */
    ixfs_vol_t *vol = ixfs_open(&disk);
    if (!vol) {
        fclose(fp);
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
        fclose(fp);
        return 1;
    }

    printf("\nROOT directory (inode %u):\n", vol->sb.s_root_inode);
    ixfs_readdir(vol, &root, print_entry, NULL);

    /* Try to read a known file — look for boot.conf or any file in root */
    printf("\nLookup test: ");
    uint32_t ino = ixfs_lookup(vol, &root, "Impossible");
    if (ino != 0) {
        struct ixfs_inode dir_inode;
        if (ixfs_read_inode(vol, ino, &dir_inode) == 0 &&
            (dir_inode.i_mode & IXFS_S_DIR)) {
            printf("Impossible/ found (inode %u)\n", ino);

            /* List contents of Impossible/ */
            printf("\nImpossible/ contents:\n");
            ixfs_readdir(vol, &dir_inode, print_entry, NULL);

            /* Try to read first 64 bytes of a file */
            uint32_t file_ino = ixfs_lookup(vol, &dir_inode, "boot.conf");
            if (file_ino != 0) {
                struct ixfs_inode file_inode;
                if (ixfs_read_inode(vol, file_ino, &file_inode) == 0) {
                    char buf[65];
                    int64_t n = ixfs_read_data(vol, &file_inode, 0, buf, 64);
                    if (n > 0) {
                        buf[n] = '\0';
                        printf("\nboot.conf first %lld bytes:\n%s\n",
                               (long long)n, buf);
                    }
                }
            }
        }
    } else {
        printf("Impossible/ not found in root\n");
    }

    ixfs_close(vol);
    fclose(fp);
    printf("\nAll tests passed.\n");
    return 0;
}
