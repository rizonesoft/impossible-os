/* ============================================================================
 * make-mbr.c — Host tool to generate a 1 MiB disk image with an MBR
 *
 * Creates a test disk image containing a valid MBR partition table with
 * three partitions: FAT32 (LBA), Linux, and IXFS.
 *
 * Usage: make-mbr <output_image_path>
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define DISK_SIZE       (64 * 1024 * 1024)   /* 64 MiB */
#define SECTOR_SIZE     512
#define MBR_PART_OFFSET 446
#define MBR_ENTRY_SIZE  16

static void write_le16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val);
    buf[1] = (uint8_t)(val >> 8);
}

static void write_le32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val);
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val >> 16);
    buf[3] = (uint8_t)(val >> 24);
}

static void write_part(uint8_t *disk, int idx,
                       uint8_t status, uint8_t type,
                       uint32_t start_lba, uint32_t num_sectors)
{
    uint8_t *entry = disk + MBR_PART_OFFSET + (idx * MBR_ENTRY_SIZE);

    entry[0] = status;          /* Boot indicator (0x80 = bootable) */
    entry[1] = 0;               /* CHS start (ignored) */
    entry[2] = 0;
    entry[3] = 0;
    entry[4] = type;            /* Partition type */
    entry[5] = 0;               /* CHS end (ignored) */
    entry[6] = 0;
    entry[7] = 0;
    write_le32(&entry[8],  start_lba);
    write_le32(&entry[12], num_sectors);
}

int main(int argc, char *argv[])
{
    uint8_t *disk;
    FILE *fp;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <output_image_path>\n", argv[0]);
        return 1;
    }

    disk = calloc(1, (size_t)DISK_SIZE);
    if (!disk) {
        fprintf(stderr, "Error: failed to allocate %d bytes\n", DISK_SIZE);
        return 1;
    }

    /* MBR boot signature */
    disk[510] = 0x55;
    disk[511] = 0xAA;

    /* Partition 1: FAT32 LBA (0x0C), bootable, 16 MiB */
    write_part(disk, 0, 0x80, 0x0C, 2048, 32768);

    /* Partition 2: Linux (0x83), 16 MiB */
    write_part(disk, 1, 0x00, 0x83, 34816, 32768);

    /* Partition 3: IXFS (0xDA), 32 MiB */
    write_part(disk, 2, 0x00, 0xDA, 67584, 65536);

    /* Partition 4: empty (all zeros — already zeroed by calloc) */

    /* ---- Write filesystem signatures into partition data areas ---- */

    /* Partition 1 (FAT32 LBA, LBA 2048): minimal FAT32 BPB */
    {
        uint8_t *bpb = disk + 2048 * SECTOR_SIZE;
        bpb[0]  = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90;
        memcpy(bpb + 3, "MSDOS5.0", 8);
        write_le16(bpb + 11, 512);       /* Bytes per sector */
        bpb[13] = 8;                     /* Sectors per cluster */
        write_le16(bpb + 14, 32);        /* Reserved sectors */
        bpb[16] = 2;                     /* Number of FATs */
        write_le16(bpb + 17, 0);         /* Root entry count */
        write_le16(bpb + 19, 0);         /* Total sectors 16 */
        bpb[21] = 0xF8;                  /* Media type */
        write_le16(bpb + 22, 0);         /* Sectors per FAT 16 */
        write_le32(bpb + 32, 32768);     /* Total sectors 32 */
        write_le32(bpb + 36, 16);        /* Sectors per FAT 32 */
        write_le32(bpb + 44, 2);         /* Root cluster */
        write_le16(bpb + 48, 1);         /* FSInfo sector */
        write_le16(bpb + 50, 6);         /* Backup boot sector */
        memcpy(bpb + 82, "FAT32   ", 8);
        bpb[510] = 0x55;
        bpb[511] = 0xAA;
    }

    /* Partition 3 (IXFS, LBA 67584): IXFS superblock */
    {
        uint8_t *sb = disk + (uint64_t)67584 * SECTOR_SIZE;
        uint32_t total_sectors = 65536;
        uint32_t total_blocks  = total_sectors / 8;

        write_le32(sb + 0, 0x49584653);  /* s_magic */
        write_le32(sb + 4, 1);           /* s_version */
        write_le32(sb + 8, 4096);        /* s_block_size */
        write_le32(sb + 12, total_blocks);
        write_le32(sb + 16, total_blocks - 4);
        write_le32(sb + 20, 128);        /* s_total_inodes */
        write_le32(sb + 24, 127);        /* s_free_inodes */
        write_le32(sb + 28, 1);          /* s_bitmap_start */
        write_le32(sb + 32, 1);          /* s_bitmap_blocks */
        write_le32(sb + 36, 2);          /* s_inode_start */
        write_le32(sb + 40, 1);          /* s_inode_blocks */
        write_le32(sb + 44, 3);          /* s_data_start */
        write_le32(sb + 48, 1);          /* s_root_inode */
        memcpy(sb + 52, "Impossible OS", 13);
    }

    fp = fopen(argv[1], "wb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s' for writing\n", argv[1]);
        free(disk);
        return 1;
    }

    if (fwrite(disk, 1, (size_t)DISK_SIZE, fp) != (size_t)DISK_SIZE) {
        fprintf(stderr, "Error: failed to write %d bytes\n", DISK_SIZE);
        fclose(fp);
        free(disk);
        return 1;
    }

    fclose(fp);
    free(disk);

    printf("Created %s (64 MiB disk with MBR: FAT32+Linux+IXFS)\n", argv[1]);
    return 0;
}
