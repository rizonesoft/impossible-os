/* ============================================================================
 * make-system-disk.c -- Build a bootable GPT system disk image
 *
 * Creates a GPT-formatted disk with:
 *   Partition 1: EFI System Partition (FAT32) -- formatted externally via mkfs.fat
 *   Partition 2: BlackBox (FAT32, 128 MiB)    -- formatted externally via mkfs.fat
 *   Partition 3: IXFS v2 (System)             -- formatted via mkfs-ixfs at offset
 *
 * Usage:
 *   make-system-disk -o output.img [-s SIZE] [--efi-size SIZE]
 *
 * The tool creates the raw image with GPT headers.  The Makefile then:
 *   1. Formats partition 1 with mkfs.fat + mcopy (boot files)
 *   2. Writes IXFS into partition 2 via mkfs-ixfs --offset
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SECTOR_SIZE 512

/* GPT constants */
#define GPT_ENTRY_SIZE 128
#define GPT_NUM_ENTRIES 128
#define GPT_ENTRY_ARRAY_BYTES (GPT_NUM_ENTRIES * GPT_ENTRY_SIZE)
#define GPT_ENTRY_SECTORS (GPT_ENTRY_ARRAY_BYTES / SECTOR_SIZE)

/* CRC32 (IEEE 802.3) */
static uint32_t crc32_table[256];

static void crc32_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320 : c >> 1;
        crc32_table[i] = c;
    }
}

static uint32_t crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i;
    for (i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

/* Little-endian writers */
static void w16(uint8_t *p, uint16_t v)
{ p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void w64(uint8_t *p, uint64_t v)
{ w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

/* Write a GUID in mixed-endian format */
static void write_guid(uint8_t *p,
                       uint32_t d1, uint16_t d2, uint16_t d3,
                       uint8_t d4_0, uint8_t d4_1, uint8_t d4_2, uint8_t d4_3,
                       uint8_t d4_4, uint8_t d4_5, uint8_t d4_6, uint8_t d4_7)
{
    w32(p, d1);
    w16(p + 4, d2);
    w16(p + 6, d3);
    p[8]  = d4_0; p[9]  = d4_1; p[10] = d4_2; p[11] = d4_3;
    p[12] = d4_4; p[13] = d4_5; p[14] = d4_6; p[15] = d4_7;
}

/* Write a UTF-16LE name into the partition entry name field */
static void write_name(uint8_t *entry, const char *name)
{
    int i;
    for (i = 0; name[i] && i < 36; i++) {
        entry[56 + i * 2] = (uint8_t)name[i];
        entry[56 + i * 2 + 1] = 0;
    }
}

/* Write a partition entry */
static void write_partition_entry(uint8_t *entry,
                                  uint32_t d1, uint16_t d2, uint16_t d3,
                                  uint8_t d4_0, uint8_t d4_1, uint8_t d4_2,
                                  uint8_t d4_3, uint8_t d4_4, uint8_t d4_5,
                                  uint8_t d4_6, uint8_t d4_7,
                                  uint64_t start_lba, uint64_t end_lba,
                                  const char *name)
{
    /* Type GUID */
    write_guid(entry, d1, d2, d3, d4_0, d4_1, d4_2, d4_3, d4_4, d4_5,
               d4_6, d4_7);
    /* Unique partition GUID (pseudo-random from LBAs) */
    write_guid(entry + 16,
               0xAAAA0000 | (uint32_t)(start_lba & 0xFFFF),
               (uint16_t)(end_lba & 0xFFFF), 0x4000,
               0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               (uint8_t)(start_lba >> 16));
    w64(entry + 32, start_lba);
    w64(entry + 40, end_lba);
    w64(entry + 48, 0);  /* Attributes */
    write_name(entry, name);
}

/* Parse size string: "256M", "1G", "512" (bytes) */
static uint64_t parse_size(const char *str)
{
    char *end;
    uint64_t val = strtoull(str, &end, 10);
    if (*end == 'G' || *end == 'g') val *= 1024ULL * 1024 * 1024;
    else if (*end == 'M' || *end == 'm') val *= 1024ULL * 1024;
    else if (*end == 'K' || *end == 'k') val *= 1024ULL;
    return val;
}

int main(int argc, char *argv[])
{
    const char *output = NULL;
    uint64_t disk_size = 512ULL * 1024 * 1024;  /* 512 MiB default */
    uint64_t efi_size = 64ULL * 1024 * 1024;    /* 64 MiB EFI partition */
    uint64_t bb_size = 128ULL * 1024 * 1024;     /* 128 MiB BlackBox partition */
    uint8_t *disk;
    uint8_t *mbr, *hdr, *entries;
    uint32_t entry_crc, hdr_crc;
    uint64_t total_sectors;
    uint64_t efi_start, efi_end;
    uint64_t bb_start, bb_end;
    uint64_t ixfs_start, ixfs_end;
    FILE *fp;
    int i;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            disk_size = parse_size(argv[++i]);
        else if (strcmp(argv[i], "--efi-size") == 0 && i + 1 < argc)
            efi_size = parse_size(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 ||
                 strcmp(argv[i], "--help") == 0) {
            printf("Usage: make-system-disk -o FILE [-s SIZE] [--efi-size SIZE]\n\n"
                   "  -o FILE        Output image (required)\n"
                   "  -s SIZE        Total disk size (default: 512M)\n"
                   "  --efi-size SZ  EFI partition size (default: 64M)\n"
                   "\nCreates a GPT disk with 3 partitions:\n"
                   "  Partition 1: EFI System (FAT32, 64 MiB)\n"
                   "  Partition 2: BlackBox (FAT32, 128 MiB) -- logs, crash dumps, diagnostics\n"
                   "  Partition 3: IXFS (System) fills remaining space\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!output) {
        fprintf(stderr, "make-system-disk: -o output.img is required\n");
        return 1;
    }

    /* Ensure disk is large enough for EFI + BlackBox + IXFS + GPT overhead */
    if (disk_size < efi_size + bb_size + 64 * 1024 * 1024) {
        fprintf(stderr, "make-system-disk: disk too small (%llu < EFI %llu + BB %llu + 64M)\n",
                (unsigned long long)disk_size,
                (unsigned long long)efi_size,
                (unsigned long long)bb_size);
        return 1;
    }

    crc32_init();
    total_sectors = disk_size / SECTOR_SIZE;

    /* Partition layout:
     * LBA 0:          Protective MBR
     * LBA 1:          Primary GPT header
     * LBA 2-33:       Partition entry array (128 entries)
     * LBA 2048:       EFI System Partition start (1 MiB aligned)
     * LBA 2048+N:     IXFS partition start (aligned to 2048)
     * ...
     * LBA total-34:   Backup partition entry array
     * LBA total-1:    Backup GPT header
     */
    efi_start = 2048;
    efi_end = efi_start + (efi_size / SECTOR_SIZE) - 1;
    /* BlackBox: 1 MiB-aligned after EFI */
    bb_start = ((efi_end + 1 + 2047) / 2048) * 2048;
    bb_end = bb_start + (bb_size / SECTOR_SIZE) - 1;
    /* IXFS: 1 MiB-aligned after BlackBox */
    ixfs_start = ((bb_end + 1 + 2047) / 2048) * 2048;
    ixfs_end = total_sectors - 34 - 1;

    printf("make-system-disk: %s -- %llu MiB\n", output,
           (unsigned long long)(disk_size / (1024 * 1024)));
    printf("  Part 1 (EFI):      LBA %llu - %llu (%llu MiB)\n",
           (unsigned long long)efi_start, (unsigned long long)efi_end,
           (unsigned long long)(efi_size / (1024 * 1024)));
    printf("  Part 2 (BlackBox): LBA %llu - %llu (%llu MiB)\n",
           (unsigned long long)bb_start, (unsigned long long)bb_end,
           (unsigned long long)(bb_size / (1024 * 1024)));
    printf("  Part 3 (IXFS):     LBA %llu - %llu (%llu MiB)\n",
           (unsigned long long)ixfs_start, (unsigned long long)ixfs_end,
           (unsigned long long)((ixfs_end - ixfs_start + 1) * SECTOR_SIZE /
                                (1024 * 1024)));

    /* Allocate disk image */
    disk = calloc(1, (size_t)disk_size);
    if (!disk) {
        fprintf(stderr, "make-system-disk: cannot allocate %llu bytes\n",
                (unsigned long long)disk_size);
        return 1;
    }

    /* ---- LBA 0: Protective MBR ---- */
    mbr = disk;
    mbr[446 + 0] = 0x00;           /* Not bootable */
    mbr[446 + 4] = 0xEE;           /* GPT protective */
    w32(mbr + 446 + 8, 1);         /* Start LBA = 1 */
    w32(mbr + 446 + 12, (uint32_t)(total_sectors - 1 > 0xFFFFFFFF
                          ? 0xFFFFFFFF : total_sectors - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    /* ---- LBA 2–33: Partition entry array ---- */
    entries = disk + 2 * SECTOR_SIZE;

    /* Entry 0: EFI System Partition */
    write_partition_entry(entries + 0 * GPT_ENTRY_SIZE,
        0xC12A7328, 0xF81F, 0x11D2, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E,
        0xC9, 0x3B,
        efi_start, efi_end, "EFI System");

    /* Entry 1: BlackBox Service Partition (Microsoft Basic Data GUID) */
    write_partition_entry(entries + 1 * GPT_ENTRY_SIZE,
        0xEBD0A0A2, 0xB9E5, 0x4433, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26,
        0x99, 0xC7,
        bb_start, bb_end, "BlackBox");

    /* Entry 2: IXFS System Partition */
    write_partition_entry(entries + 2 * GPT_ENTRY_SIZE,
        0xDA000000, 0x0000, 0x4978, 0x46, 0x53, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01,
        ixfs_start, ixfs_end, "Impossible OS");

    /* Compute CRC32 of the partition entry array */
    entry_crc = crc32(entries, GPT_ENTRY_ARRAY_BYTES);

    /* ---- LBA 1: Primary GPT Header ---- */
    hdr = disk + 1 * SECTOR_SIZE;
    hdr[0]='E'; hdr[1]='F'; hdr[2]='I'; hdr[3]=' ';
    hdr[4]='P'; hdr[5]='A'; hdr[6]='R'; hdr[7]='T';
    w32(hdr + 8, 0x00010000);      /* Revision 1.0 */
    w32(hdr + 12, 92);             /* Header size */
    w32(hdr + 16, 0);              /* CRC32 (filled below) */
    w32(hdr + 20, 0);              /* Reserved */
    w64(hdr + 24, 1);              /* MyLBA */
    w64(hdr + 32, total_sectors - 1);  /* AlternateLBA */
    w64(hdr + 40, 34);             /* FirstUsableLBA */
    w64(hdr + 48, total_sectors - 34); /* LastUsableLBA */
    /* Disk GUID */
    write_guid(hdr + 56, 0x1ECCBA1D, 0x0505, 0x4978,
               0x46, 0x53, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01);
    w64(hdr + 72, 2);              /* PartitionEntryLBA */
    w32(hdr + 80, GPT_NUM_ENTRIES);
    w32(hdr + 84, GPT_ENTRY_SIZE);
    w32(hdr + 88, entry_crc);      /* Partition entry array CRC32 */

    /* Compute header CRC32 */
    hdr_crc = crc32(hdr, 92);
    w32(hdr + 16, hdr_crc);

    /* ---- Backup GPT (end of disk) ---- */
    {
        uint8_t *backup_entries = disk + (total_sectors - 33) * SECTOR_SIZE;
        uint8_t *backup_hdr = disk + (total_sectors - 1) * SECTOR_SIZE;

        /* Copy partition entries to backup location */
        memcpy(backup_entries, entries, GPT_ENTRY_ARRAY_BYTES);

        /* Create backup header (copy primary, fix LBA fields) */
        memcpy(backup_hdr, hdr, 92);
        w64(backup_hdr + 24, total_sectors - 1);  /* MyLBA (last sector) */
        w64(backup_hdr + 32, 1);                   /* AlternateLBA (primary) */
        w64(backup_hdr + 72, total_sectors - 33);  /* PartitionEntryLBA */

        /* Recompute backup header CRC32 */
        w32(backup_hdr + 16, 0);
        hdr_crc = crc32(backup_hdr, 92);
        w32(backup_hdr + 16, hdr_crc);
    }

    /* ---- Write partition info file for Makefile ---- */
    {
        char info_path[512];
        FILE *info_fp;
        snprintf(info_path, sizeof(info_path), "%s.info", output);
        info_fp = fopen(info_path, "w");
        if (info_fp) {
            fprintf(info_fp, "EFI_OFFSET=%llu\n",
                    (unsigned long long)(efi_start * SECTOR_SIZE));
            fprintf(info_fp, "EFI_SIZE=%llu\n",
                    (unsigned long long)efi_size);
            fprintf(info_fp, "BB_OFFSET=%llu\n",
                    (unsigned long long)(bb_start * SECTOR_SIZE));
            fprintf(info_fp, "BB_SIZE=%llu\n",
                    (unsigned long long)bb_size);
            fprintf(info_fp, "IXFS_OFFSET=%llu\n",
                    (unsigned long long)(ixfs_start * SECTOR_SIZE));
            fprintf(info_fp, "IXFS_SIZE=%llu\n",
                    (unsigned long long)((ixfs_end - ixfs_start + 1) *
                                         SECTOR_SIZE));
            fclose(info_fp);
        }
    }

    /* ---- Write disk image ---- */
    fp = fopen(output, "wb");
    if (!fp) {
        fprintf(stderr, "make-system-disk: cannot create '%s'\n", output);
        free(disk);
        return 1;
    }
    if (fwrite(disk, 1, (size_t)disk_size, fp) != (size_t)disk_size) {
        fprintf(stderr, "make-system-disk: write error\n");
        fclose(fp);
        free(disk);
        return 1;
    }
    fclose(fp);
    free(disk);

    printf("make-system-disk: created %s (%llu MiB GPT: EFI + BlackBox + IXFS)\n",
           output, (unsigned long long)(disk_size / (1024 * 1024)));
    return 0;
}
