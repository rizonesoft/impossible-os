/* ============================================================================
 * fat32_format.c — FAT32 formatting
 *
 * Formats a raw block device as FAT32: writes boot sector (BPB), FSInfo,
 * both FAT copies, and an empty root directory cluster.
 *
 * This module is standalone — it operates directly on the blkdev and does
 * not use the per-volume globals (bpb, fat32_dev, etc.).
 * ============================================================================ */

#include "fat32_internal.h"

int fat32_format(const struct blkdev *dev, const char *label)
{
    /* Invalidate cache — formatting rewrites the entire disk layout */
    scache_invalidate();

    uint8_t boot[512];
    uint8_t fat_sec[512];
    uint32_t total_sectors;
    uint32_t fat_size;
    uint32_t data_start;
    uint32_t total_clusters;
    uint32_t si;
    uint32_t k;
    uint8_t spc;  /* sectors per cluster */

    if (!dev)
        return -1;

    total_sectors = (uint32_t)dev->sector_count;
    if (total_sectors < 65536) {
        klog(LOG_ERROR, "fat32", "FAT32: volume too small (%u sectors)",
               (uint64_t)total_sectors);
        return -1;
    }

    /* Choose sectors per cluster based on volume size */
    if (total_sectors < 532480)       spc = 1;   /* < 260 MiB */
    else if (total_sectors < 16777216) spc = 8;   /* < 8 GiB */
    else if (total_sectors < 33554432) spc = 16;  /* < 16 GiB */
    else                               spc = 32;  /* >= 16 GiB */

    /* Calculate FAT size (sectors per FAT) */
    {
        uint32_t tmp = total_sectors - 32;   /* minus reserved */
        fat_size = (tmp + 2 * spc + 127 * spc)
                 / (128 * spc + 2);          /* 128 entries per sector */
        fat_size++;  /* round up */
    }
    data_start = 32 + 2 * fat_size;
    total_clusters = (total_sectors - data_start) / spc;

    /* ---- Write boot sector (BPB) ---- */
    for (k = 0; k < 512; k++)
        boot[k] = 0;

    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;  /* Jump */
    /* OEM name */
    boot[3]='I'; boot[4]='M'; boot[5]='P'; boot[6]='O';
    boot[7]='S'; boot[8]='S'; boot[9]='O'; boot[10]='S';

    *(uint16_t *)&boot[11] = 512;              /* Bytes per sector */
    boot[13] = spc;                            /* Sectors per cluster */
    *(uint16_t *)&boot[14] = 32;               /* Reserved sectors */
    boot[16] = 2;                              /* Number of FATs */
    *(uint16_t *)&boot[17] = 0;                /* Root entry count */
    *(uint16_t *)&boot[19] = 0;                /* Total sectors 16 */
    boot[21] = 0xF8;                           /* Media type */
    *(uint16_t *)&boot[22] = 0;                /* Sectors per FAT 16 */
    *(uint16_t *)&boot[24] = 63;               /* Sectors per track */
    *(uint16_t *)&boot[26] = 255;              /* Number of heads */
    *(uint32_t *)&boot[32] = total_sectors;    /* Total sectors 32 */
    *(uint32_t *)&boot[36] = fat_size;         /* Sectors per FAT 32 */
    *(uint32_t *)&boot[44] = 2;                /* Root cluster */
    *(uint16_t *)&boot[48] = 1;                /* FSInfo sector */
    *(uint16_t *)&boot[50] = 6;                /* Backup boot sector */

    /* FAT32 extended fields */
    boot[66] = 0x29;                           /* Extended boot sig */
    *(uint32_t *)&boot[67] = 0x12345678;       /* Volume serial */
    /* Volume label (11 bytes, space-padded) */
    {
        int li;
        for (li = 0; li < 11; li++)
            boot[71 + li] = ' ';
        if (label) {
            for (li = 0; li < 11 && label[li]; li++) {
                char c = label[li];
                if (c >= 'a' && c <= 'z') c -= 32;
                boot[71 + li] = (uint8_t)c;
            }
        }
    }
    /* FS type string */
    boot[82]='F'; boot[83]='A'; boot[84]='T'; boot[85]='3';
    boot[86]='2'; boot[87]=' '; boot[88]=' '; boot[89]=' ';

    boot[510] = 0x55;
    boot[511] = 0xAA;

    if (blkdev_write(dev, 0, 1, boot) != 0)
        return -1;

    /* Write backup boot sector at sector 6 */
    if (blkdev_write(dev, 6, 1, boot) != 0)
        return -1;

    /* ---- Write FSInfo sector at sector 1 ---- */
    for (k = 0; k < 512; k++)
        fat_sec[k] = 0;

    *(uint32_t *)&fat_sec[0] = 0x41615252;      /* FSInfo signature */
    *(uint32_t *)&fat_sec[484] = 0x61417272;     /* Second signature */
    *(uint32_t *)&fat_sec[488] = total_clusters - 1;  /* Free clusters */
    *(uint32_t *)&fat_sec[492] = 3;              /* Next free cluster */
    fat_sec[510] = 0x55;
    fat_sec[511] = 0xAA;

    if (blkdev_write(dev, 1, 1, fat_sec) != 0)
        return -1;

    /* ---- Write FAT tables (both copies) ---- */
    for (k = 0; k < 512; k++)
        fat_sec[k] = 0;

    {
        uint32_t fi;
        for (fi = 0; fi < 2; fi++) {
            uint32_t fat_start = 32 + fi * fat_size;

            /* Write all FAT sectors as zero first */
            for (si = 0; si < fat_size; si++) {
                if (blkdev_write(dev, fat_start + si, 1, fat_sec) != 0)
                    return -1;
            }

            /* Write first FAT sector with special entries */
            {
                uint8_t first_fat[512];
                for (k = 0; k < 512; k++)
                    first_fat[k] = 0;

                *(uint32_t *)&first_fat[0] = 0x0FFFFFF8;  /* Entry 0 */
                *(uint32_t *)&first_fat[4] = 0x0FFFFFFF;  /* Entry 1 */
                *(uint32_t *)&first_fat[8] = 0x0FFFFFFF;  /* Entry 2 (root) */

                if (blkdev_write(dev, fat_start, 1, first_fat) != 0)
                    return -1;
            }
        }
    }

    /* ---- Zero root directory cluster ---- */
    {
        uint32_t root_sector = data_start;
        for (si = 0; si < (uint32_t)spc; si++) {
            for (k = 0; k < 512; k++)
                fat_sec[k] = 0;
            if (blkdev_write(dev, root_sector + si, 1, fat_sec) != 0)
                return -1;
        }

        /* Write volume label entry in root directory */
        if (label && label[0]) {
            uint8_t vol_ent[512];
            int li;
            for (k = 0; k < 512; k++)
                vol_ent[k] = 0;

            for (li = 0; li < 11; li++)
                vol_ent[li] = ' ';
            for (li = 0; li < 11 && label[li]; li++) {
                char c = label[li];
                if (c >= 'a' && c <= 'z') c -= 32;
                vol_ent[li] = (uint8_t)c;
            }
            vol_ent[11] = FAT32_ATTR_VOLUME_ID;

            if (blkdev_write(dev, root_sector, 1, vol_ent) != 0)
                return -1;
        }
    }

    klog(LOG_DEBUG, "fat32", "FAT32: formatted %u MiB (%u clusters, %u sec/cluster)",
           (uint64_t)total_sectors * 512 / (1024 * 1024),
           (uint64_t)total_clusters,
           (uint64_t)spc);

    return 0;
}
