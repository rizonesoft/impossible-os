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

#define DISK_SIZE       (1024 * 1024)   /* 1 MiB */
#define MBR_PART_OFFSET 446
#define MBR_ENTRY_SIZE  16

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

    disk = calloc(1, DISK_SIZE);
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

    fp = fopen(argv[1], "wb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s' for writing\n", argv[1]);
        free(disk);
        return 1;
    }

    if (fwrite(disk, 1, DISK_SIZE, fp) != DISK_SIZE) {
        fprintf(stderr, "Error: failed to write %d bytes\n", DISK_SIZE);
        fclose(fp);
        free(disk);
        return 1;
    }

    fclose(fp);
    free(disk);

    printf("Created %s (1 MiB disk with MBR: FAT32+Linux+IXFS)\n", argv[1]);
    return 0;
}
