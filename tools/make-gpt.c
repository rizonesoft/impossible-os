/* ============================================================================
 * make-gpt.c — Host tool to generate a disk image with a GPT partition table
 *
 * Creates a test disk image containing:
 *   LBA 0:   Protective MBR (type 0xEE)
 *   LBA 1:   Primary GPT header with valid CRC32
 *   LBA 2–5: Partition entry array (128 entries × 128 bytes = 16384 bytes)
 *   ...data area...
 *   Last LBA: Backup GPT header
 *
 * Partitions:
 *   1. EFI System    — 1 MiB   (LBAs 2048–4095)
 *   2. Basic Data    — 16 MiB  (LBAs 4096–36863)
 *   3. IXFS          — 32 MiB  (LBAs 36864–102399)
 *
 * Usage: make-gpt <output_image_path>
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define DISK_SIZE_MB    64
#define DISK_SIZE       ((uint64_t)DISK_SIZE_MB * 1024 * 1024)
#define SECTOR_SIZE     512
#define TOTAL_SECTORS   (DISK_SIZE / SECTOR_SIZE)

#define GPT_ENTRY_SIZE  128
#define GPT_NUM_ENTRIES 128
#define GPT_ENTRY_ARRAY_BYTES (GPT_NUM_ENTRIES * GPT_ENTRY_SIZE)   /* 16384 */
#define GPT_ENTRY_SECTORS     (GPT_ENTRY_ARRAY_BYTES / SECTOR_SIZE) /* 32 */

/* CRC32 (IEEE 802.3) */
static uint32_t crc32_table[256];

static void crc32_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? ((c >> 1) ^ 0xEDB88320) : (c >> 1);
        crc32_table[i] = c;
    }
}

static uint32_t crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i;
    for (i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* Write little-endian values */
static void w16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void w32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p+4, (uint32_t)(v>>32)); }

/* Write a GUID in mixed-endian format */
static void write_guid(uint8_t *p,
                       uint32_t d1, uint16_t d2, uint16_t d3,
                       uint8_t d4_0, uint8_t d4_1, uint8_t d4_2, uint8_t d4_3,
                       uint8_t d4_4, uint8_t d4_5, uint8_t d4_6, uint8_t d4_7)
{
    w32(p, d1);
    w16(p+4, d2);
    w16(p+6, d3);
    p[8]=d4_0; p[9]=d4_1; p[10]=d4_2; p[11]=d4_3;
    p[12]=d4_4; p[13]=d4_5; p[14]=d4_6; p[15]=d4_7;
}

/* Write a UTF-16LE name into the partition entry name field */
static void write_name(uint8_t *entry, const char *name)
{
    int i;
    for (i = 0; i < 36 && name[i]; i++) {
        entry[56 + i*2]     = (uint8_t)name[i];
        entry[56 + i*2 + 1] = 0;
    }
}

static void write_partition_entry(uint8_t *entry,
                                  uint32_t d1, uint16_t d2, uint16_t d3,
                                  uint8_t d4_0, uint8_t d4_1, uint8_t d4_2,
                                  uint8_t d4_3, uint8_t d4_4, uint8_t d4_5,
                                  uint8_t d4_6, uint8_t d4_7,
                                  uint64_t start_lba, uint64_t end_lba,
                                  const char *name)
{
    memset(entry, 0, GPT_ENTRY_SIZE);
    /* Type GUID */
    write_guid(entry, d1, d2, d3, d4_0, d4_1, d4_2, d4_3, d4_4, d4_5, d4_6, d4_7);
    /* Unique GUID (just use a simple incrementing pattern) */
    write_guid(entry + 16, 0x11111111, 0x2222, 0x3333,
               0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, (uint8_t)(start_lba & 0xFF));
    w64(entry + 32, start_lba);
    w64(entry + 40, end_lba);
    w64(entry + 48, 0);   /* attributes */
    write_name(entry, name);
}

int main(int argc, char *argv[])
{
    uint8_t *disk;
    uint8_t *mbr, *hdr, *entries;
    uint32_t entry_crc, hdr_crc;
    FILE *fp;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <output_image_path>\n", argv[0]);
        return 1;
    }

    crc32_init();

    disk = calloc(1, (size_t)DISK_SIZE);
    if (!disk) {
        fprintf(stderr, "Error: failed to allocate %llu bytes\n",
                (unsigned long long)DISK_SIZE);
        return 1;
    }

    /* ---- LBA 0: Protective MBR ---- */
    mbr = disk;
    /* Partition 1: type 0xEE, spanning entire disk */
    mbr[446 + 0] = 0x00;          /* Not bootable */
    mbr[446 + 4] = 0xEE;          /* GPT protective */
    w32(mbr + 446 + 8, 1);        /* Start LBA = 1 */
    w32(mbr + 446 + 12, (uint32_t)(TOTAL_SECTORS - 1 > 0xFFFFFFFF
                          ? 0xFFFFFFFF : TOTAL_SECTORS - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    /* ---- LBA 2–33: Partition entry array (128 entries × 128 bytes) ---- */
    entries = disk + 2 * SECTOR_SIZE;  /* LBA 2 */

    /* Entry 0: EFI System Partition (C12A7328-F81F-11D2-BA4B-00A0C93EC93B) */
    write_partition_entry(entries + 0 * GPT_ENTRY_SIZE,
        0xC12A7328, 0xF81F, 0x11D2, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
        2048, 4095, "EFI System");

    /* Entry 1: Microsoft Basic Data (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7) */
    write_partition_entry(entries + 1 * GPT_ENTRY_SIZE,
        0xEBD0A0A2, 0xB9E5, 0x4433, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
        4096, 36863, "Windows Data");

    /* Entry 2: IXFS (DA000000-0000-4978-4653-000000000001) */
    write_partition_entry(entries + 2 * GPT_ENTRY_SIZE,
        0xDA000000, 0x0000, 0x4978, 0x46, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        36864, 102399, "Impossible OS");

    /* Compute CRC32 of the partition entry array */
    entry_crc = crc32(entries, GPT_ENTRY_ARRAY_BYTES);

    /* ---- LBA 1: Primary GPT Header ---- */
    hdr = disk + 1 * SECTOR_SIZE;

    /* Signature: "EFI PART" */
    hdr[0]='E'; hdr[1]='F'; hdr[2]='I'; hdr[3]=' ';
    hdr[4]='P'; hdr[5]='A'; hdr[6]='R'; hdr[7]='T';

    w32(hdr + 8, 0x00010000);     /* Revision 1.0 */
    w32(hdr + 12, 92);            /* Header size */
    w32(hdr + 16, 0);             /* CRC32 (filled below) */
    w32(hdr + 20, 0);             /* Reserved */
    w64(hdr + 24, 1);             /* MyLBA */
    w64(hdr + 32, TOTAL_SECTORS - 1);  /* AlternateLBA */
    w64(hdr + 40, 34);            /* FirstUsableLBA */
    w64(hdr + 48, TOTAL_SECTORS - 34); /* LastUsableLBA */

    /* Disk GUID */
    write_guid(hdr + 56, 0xDEADBEEF, 0xCAFE, 0xF00D,
               0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0);

    w64(hdr + 72, 2);             /* PartitionEntryLBA */
    w32(hdr + 80, GPT_NUM_ENTRIES);
    w32(hdr + 84, GPT_ENTRY_SIZE);
    w32(hdr + 88, entry_crc);     /* Partition entry array CRC32 */

    /* Compute header CRC32 (with CRC field zeroed — it already is) */
    hdr_crc = crc32(hdr, 92);
    w32(hdr + 16, hdr_crc);

    /* ---- Write disk image ---- */
    fp = fopen(argv[1], "wb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s' for writing\n", argv[1]);
        free(disk);
        return 1;
    }

    if (fwrite(disk, 1, (size_t)DISK_SIZE, fp) != (size_t)DISK_SIZE) {
        fprintf(stderr, "Error: failed to write disk image\n");
        fclose(fp);
        free(disk);
        return 1;
    }

    fclose(fp);
    free(disk);

    printf("Created %s (%d MiB GPT disk: EFI+BasicData+IXFS)\n",
           argv[1], DISK_SIZE_MB);
    return 0;
}
