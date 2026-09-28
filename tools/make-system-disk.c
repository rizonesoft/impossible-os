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
    int ab_mode = 0;                             /* A/B dual-slot boot */
    uint8_t *disk;
    uint8_t *mbr, *hdr, *entries;
    uint32_t entry_crc, hdr_crc;
    uint64_t total_sectors;
    uint64_t efi_start, efi_end;
    uint64_t bb_start, bb_end;
    uint64_t ixfs_start, ixfs_end;
    uint64_t ixfs_b_start, ixfs_b_end;  /* Slot B (A/B mode only) */
    uint64_t recovery_start = 0, recovery_end = 0;  /* Recovery partition (A/B mode only) */
    uint64_t meta_start = 0, meta_end = 0;  /* A/B metadata partition (A/B mode only) */
    /* Metadata partition holds the two redundant ab_boot_metadata blocks; 1 MiB
     * is alignment-friendly overkill (the record is 60 bytes x2). */
    const uint64_t meta_sectors = (1ULL * 1024 * 1024) / SECTOR_SIZE;
    FILE *fp;
    char info_path[512];
    char tmp_path[520];
    int i;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            disk_size = parse_size(argv[++i]);
        else if (strcmp(argv[i], "--efi-size") == 0 && i + 1 < argc)
            efi_size = parse_size(argv[++i]);
        else if (strcmp(argv[i], "--ab") == 0)
            ab_mode = 1;
        else if (strcmp(argv[i], "-h") == 0 ||
                 strcmp(argv[i], "--help") == 0) {
            printf("Usage: make-system-disk -o FILE [-s SIZE] [--efi-size SIZE] [--ab]\n\n"
                   "  -o FILE        Output image (required)\n"
                   "  -s SIZE        Total disk size (default: 512M)\n"
                   "  --efi-size SZ  EFI partition size (default: 64M)\n"
                   "  --ab           A/B dual-slot layout (6 partitions incl. Recovery)\n"
                   "\nDefault layout (3 partitions):\n"
                   "  Partition 1: EFI System (FAT32, 64 MiB)\n"
                   "  Partition 2: BlackBox (FAT32, 128 MiB) -- logs, crash dumps, diagnostics\n"
                   "  Partition 3: IXFS (System) fills remaining space\n"
                   "\nA/B layout (5 partitions):\n"
                   "  Partition 1: EFI System (FAT32, 64 MiB)\n"
                   "  Partition 2: BlackBox (FAT32, 128 MiB)\n"
                   "  Partition 3: IXFS Slot A (half remaining space)\n"
                   "  Partition 4: IXFS Slot B (half remaining space)\n"
                   "  Partition 5: A/B metadata (1 MiB)\n");
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

    /* Ensure disk is large enough. Standard: EFI + BlackBox + IXFS + GPT.
     * A/B: also a 1 MiB metadata partition + two usable IXFS slots. Fail
     * closed rather than silently producing undersized slots. */
    {
        if (efi_size < SECTOR_SIZE || (efi_size % SECTOR_SIZE) != 0) {
            fprintf(stderr, "make-system-disk: --efi-size must be a non-zero multiple of %u (got %llu)\n",
                    SECTOR_SIZE, (unsigned long long)efi_size);
            return 1;
        }
        uint64_t min_size = efi_size + bb_size + 64ULL * 1024 * 1024;
        if (ab_mode)
            min_size = efi_size + bb_size
                     + (1ULL * 1024 * 1024)            /* metadata partition */
                     + 2ULL * (96ULL * 1024 * 1024)    /* two usable IXFS slots */
                     + (2ULL * 1024 * 1024);           /* GPT + alignment slack */
        if (disk_size < min_size) {
            fprintf(stderr, "make-system-disk: disk too small (%llu < %llu for %s layout)\n",
                    (unsigned long long)disk_size,
                    (unsigned long long)min_size,
                    ab_mode ? "A/B" : "standard");
            return 1;
        }
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
    ixfs_b_start = 0;
    ixfs_b_end = 0;

    if (ab_mode) {
        /* A/B: a 1 MiB metadata partition sits after BlackBox; Slot A and
         * Slot B then split the remaining space evenly (both 1 MiB aligned). */
        meta_start = ((bb_end + 1 + 2047) / 2048) * 2048;
        meta_end = meta_start + meta_sectors - 1;
        ixfs_start = ((meta_end + 1 + 2047) / 2048) * 2048;
        /* Recovery partition (read-only) carved at the very end; Slot A and
         * Slot B split the space BEFORE it. Recovery start is aligned DOWN so
         * the partition is at least this reserve. The reserve is 34 MiB, not a
         * bare 32 MiB: the Makefile formats it FAT32 with 1 sector/cluster, and
         * after reserved + FAT-table overhead a 32 MiB (65536-sector) volume
         * yields ~64496 data clusters -- below the FAT32 floor of 65525, which
         * UEFI firmware (OVMF) rejects. 34 MiB clears 65525 with margin. */
        uint64_t recovery_sectors = (34ULL * 1024 * 1024) / SECTOR_SIZE;
        recovery_end = total_sectors - 34 - 1;
        recovery_start = ((recovery_end + 1 - recovery_sectors) / 2048) * 2048;
        uint64_t remaining = recovery_start - ixfs_start;  /* both slots fit before recovery */
        uint64_t half = (remaining / 2 / 2048) * 2048;  /* 1 MiB aligned */
        ixfs_end = ixfs_start + half - 1;
        ixfs_b_start = ((ixfs_end + 1 + 2047) / 2048) * 2048;
        ixfs_b_end = recovery_start - 1;
    } else {
        /* IXFS: 1 MiB-aligned after BlackBox, fills remaining space */
        ixfs_start = ((bb_end + 1 + 2047) / 2048) * 2048;
        ixfs_end = total_sectors - 34 - 1;
    }

    if (ab_mode) {
        /* Validate the REALIZED aligned layout (the pre-alloc size check budgets
         * a fixed slack that a non-1MiB-aligned --efi-size can exceed): strict
         * ordering + no overlap/wrap, within LastUsableLBA, both slots >= 96 MiB. */
        uint64_t last_usable = total_sectors - 34;
        uint64_t a_sec = (ixfs_end >= ixfs_start) ? (ixfs_end - ixfs_start + 1) : 0;
        uint64_t b_sec = (ixfs_b_end >= ixfs_b_start) ? (ixfs_b_end - ixfs_b_start + 1) : 0;
        uint64_t rec_sec = (recovery_end >= recovery_start) ? (recovery_end - recovery_start + 1) : 0;
        uint64_t min_slot = (96ULL * 1024 * 1024) / SECTOR_SIZE;
        /* FAT32 floor: the Makefile formats recovery FAT32 at 1 sector/cluster;
         * after reserved + FAT-table overhead the data-cluster count must reach
         * the FAT32 minimum of 65525, so the partition must hold >= 66581
         * sectors. Fail closed rather than emit a firmware-hostile volume. */
        uint64_t min_recovery = 66581;
        if (!(efi_start <= efi_end && efi_end < bb_start &&
              bb_start <= bb_end && bb_end < meta_start &&
              meta_start <= meta_end && meta_end < ixfs_start &&
              ixfs_start <= ixfs_end && ixfs_end < ixfs_b_start &&
              ixfs_b_start <= ixfs_b_end && ixfs_b_end < recovery_start &&
              recovery_start <= recovery_end && recovery_end < last_usable &&
              a_sec >= min_slot && b_sec >= min_slot && rec_sec >= min_recovery)) {
            fprintf(stderr, "make-system-disk: A/B layout invalid -- slots %llu/%llu MiB "
                    "(need >= 96 MiB each), recovery %llu MiB (need >= 34 MiB for FAT32); "
                    "grow the disk or align --efi-size\n",
                    (unsigned long long)(a_sec * SECTOR_SIZE / (1024 * 1024)),
                    (unsigned long long)(b_sec * SECTOR_SIZE / (1024 * 1024)),
                    (unsigned long long)(rec_sec * SECTOR_SIZE / (1024 * 1024)));
            return 1;
        }
    } else {
        /* Validate the REALIZED standard layout the same way the A/B path does:
         * strict ordering, no overlap/wrap, IXFS slot non-empty, within
         * LastUsableLBA. Guards a non-1MiB-aligned or oversized --efi-size. */
        uint64_t last_usable = total_sectors - 34;
        if (!(efi_start <= efi_end && efi_end < bb_start &&
              bb_start <= bb_end && bb_end < ixfs_start &&
              ixfs_start <= ixfs_end && ixfs_end < last_usable)) {
            fprintf(stderr, "make-system-disk: standard layout invalid -- "
                    "partition ordering or bounds violated (grow the disk or align --efi-size)\n");
            return 1;
        }
    }

    printf("make-system-disk: %s -- %llu MiB (%s)\n", output,
           (unsigned long long)(disk_size / (1024 * 1024)),
           ab_mode ? "A/B dual-slot" : "standard");
    printf("  Part 1 (EFI):      LBA %llu - %llu (%llu MiB)\n",
           (unsigned long long)efi_start, (unsigned long long)efi_end,
           (unsigned long long)(efi_size / (1024 * 1024)));
    printf("  Part 2 (BlackBox): LBA %llu - %llu (%llu MiB)\n",
           (unsigned long long)bb_start, (unsigned long long)bb_end,
           (unsigned long long)(bb_size / (1024 * 1024)));
    printf("  Part 3 (IXFS%s):  LBA %llu - %llu (%llu MiB)\n",
           ab_mode ? " A" : "",
           (unsigned long long)ixfs_start, (unsigned long long)ixfs_end,
           (unsigned long long)((ixfs_end - ixfs_start + 1) * SECTOR_SIZE /
                                (1024 * 1024)));
    if (ab_mode) {
        printf("  Part 5 (ABMeta):  LBA %llu - %llu (%llu KiB)\n",
               (unsigned long long)meta_start, (unsigned long long)meta_end,
               (unsigned long long)((meta_end - meta_start + 1) * SECTOR_SIZE /
                                    1024));
        printf("  Part 4 (IXFS B):  LBA %llu - %llu (%llu MiB)\n",
               (unsigned long long)ixfs_b_start, (unsigned long long)ixfs_b_end,
               (unsigned long long)((ixfs_b_end - ixfs_b_start + 1) * SECTOR_SIZE /
                                    (1024 * 1024)));
        printf("  Part 6 (Recovery): LBA %llu - %llu (%llu MiB, read-only)\n",
               (unsigned long long)recovery_start, (unsigned long long)recovery_end,
               (unsigned long long)((recovery_end - recovery_start + 1) * SECTOR_SIZE /
                                    (1024 * 1024)));
    }

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

    /* ---- LBA 2-33: Partition entry array ---- */
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

    /* Entry 2: IXFS System Partition (Slot A in A/B mode) */
    write_partition_entry(entries + 2 * GPT_ENTRY_SIZE,
        0xDA000000, 0x0000, 0x4978, 0x46, 0x53, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01,
        ixfs_start, ixfs_end,
        ab_mode ? "Impossible OS A" : "Impossible OS");

    /* Entry 3: IXFS Slot B (A/B mode only) */
    if (ab_mode) {
        write_partition_entry(entries + 3 * GPT_ENTRY_SIZE,
            0xDA000000, 0x0000, 0x4978, 0x46, 0x53, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x02,
            ixfs_b_start, ixfs_b_end, "Impossible OS B");

        /* Entry 4: A/B metadata partition. Distinct type GUID (...4D44... "MD"
         * vs IXFS ...4653... "FS") so the kernel's IXFS scan never mounts it
         * and the bootloader can locate the ab_boot_metadata blocks by type. */
        write_partition_entry(entries + 4 * GPT_ENTRY_SIZE,
            0xDA000000, 0x0000, 0x4978, 0x4D, 0x44, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x01,
            meta_start, meta_end, "Impossible OS ABMeta");

        /* Entry 5: Recovery partition. Type GUID 49504F53-7265-636F-7665-
         * 727900000001 ("IPOSrecovery" in the GUID bytes). Holds the last
         * known-good kernel.bak now; recovery.exe + ixfs-fsck land with the
         * recovery bootloader + fsck tool. Marked GPT read-only (attribute bit
         * 60) so the recovery image is not mutated by a normal-mode mount. */
        write_partition_entry(entries + 5 * GPT_ENTRY_SIZE,
            0x49504F53, 0x7265, 0x636F, 0x76, 0x65, 0x72, 0x79, 0x00, 0x00,
            0x00, 0x01,
            recovery_start, recovery_end, "Impossible OS Recovery");
        /* GPT read-only: attributes field at entry offset 48, bit 60. */
        w64(entries + 5 * GPT_ENTRY_SIZE + 48, 1ULL << 60);
    }

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

    /* ---- Compute the sidecar paths BEFORE the destructive image open, so
     *      every failure after that open can also drop a stale prior .info
     *      (never leave old offsets beside a new or partial image) ---- */
    {
        int n = snprintf(info_path, sizeof(info_path), "%s.info", output);
        if (n < 0 || (size_t)n >= sizeof(info_path)) {
            fprintf(stderr, "make-system-disk: .info path too long\n");
            free(disk);
            return 1;
        }
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", info_path);

    /* ---- Write disk image FIRST, so the .info sidecar never advertises
     *      offsets for an image write that later failed. fopen("wb") truncates
     *      on success; once it returns OK, any later failure must also drop the
     *      now-stale prior .info and the partial image. ---- */
    fp = fopen(output, "wb");
    if (!fp) {
        /* output untouched -- the old image+.info pair stays consistent */
        fprintf(stderr, "make-system-disk: cannot create '%s'\n", output);
        free(disk);
        return 1;
    }
    if (fwrite(disk, 1, (size_t)disk_size, fp) != (size_t)disk_size) {
        fprintf(stderr, "make-system-disk: write error\n");
        fclose(fp);
        free(disk);
        remove(output);
        remove(info_path);
        return 1;
    }
    if (fclose(fp) != 0) {
        fprintf(stderr, "make-system-disk: close error on '%s'\n", output);
        free(disk);
        remove(output);
        remove(info_path);
        return 1;
    }
    free(disk);

    /* ---- Write the partition info sidecar transactionally. The Makefile and
     *      the boot-metadata tooling source these offsets, so a missing, stale,
     *      or partial .info must FAIL the build, not pass silently. Write to a
     *      temp file, check every error, then atomically rename into place. ---- */
    {
        FILE *info_fp = fopen(tmp_path, "w");
        if (!info_fp) {
            fprintf(stderr, "make-system-disk: cannot create '%s'\n", tmp_path);
            remove(info_path);  /* drop stale prior sidecar */
            remove(output);     /* drop the new image so make retries cleanly */
            return 1;
        }
        fprintf(info_fp, "EFI_OFFSET=%llu\n",
                (unsigned long long)(efi_start * SECTOR_SIZE));
        fprintf(info_fp, "EFI_SIZE=%llu\n", (unsigned long long)efi_size);
        fprintf(info_fp, "BB_OFFSET=%llu\n",
                (unsigned long long)(bb_start * SECTOR_SIZE));
        fprintf(info_fp, "BB_SIZE=%llu\n", (unsigned long long)bb_size);
        fprintf(info_fp, "IXFS_OFFSET=%llu\n",
                (unsigned long long)(ixfs_start * SECTOR_SIZE));
        fprintf(info_fp, "IXFS_SIZE=%llu\n",
                (unsigned long long)((ixfs_end - ixfs_start + 1) * SECTOR_SIZE));
        if (ab_mode) {
            fprintf(info_fp, "META_OFFSET=%llu\n",
                    (unsigned long long)(meta_start * SECTOR_SIZE));
            fprintf(info_fp, "META_SIZE=%llu\n",
                    (unsigned long long)(meta_sectors * SECTOR_SIZE));
            fprintf(info_fp, "IXFS_B_OFFSET=%llu\n",
                    (unsigned long long)(ixfs_b_start * SECTOR_SIZE));
            fprintf(info_fp, "IXFS_B_SIZE=%llu\n",
                    (unsigned long long)((ixfs_b_end - ixfs_b_start + 1) * SECTOR_SIZE));
            fprintf(info_fp, "RECOVERY_OFFSET=%llu\n",
                    (unsigned long long)(recovery_start * SECTOR_SIZE));
            fprintf(info_fp, "RECOVERY_SIZE=%llu\n",
                    (unsigned long long)((recovery_end - recovery_start + 1) * SECTOR_SIZE));
        }
        {
            /* Evaluate each step separately so fclose ALWAYS runs (no
             * short-circuit fd leak). On any failure drop the temp AND any
             * stale prior sidecar so a failed run never leaves old offset
             * metadata beside the freshly written image. */
            int werr = ferror(info_fp);
            if (fflush(info_fp) != 0) werr = 1;
            if (fclose(info_fp) != 0) werr = 1;
            if (werr) {
                fprintf(stderr, "make-system-disk: error writing '%s'\n", tmp_path);
                remove(tmp_path);
                remove(info_path);
                remove(output);
                return 1;
            }
        }
        if (rename(tmp_path, info_path) != 0) {
            fprintf(stderr, "make-system-disk: cannot rename '%s' -> '%s'\n",
                    tmp_path, info_path);
            remove(tmp_path);
            remove(info_path);
            remove(output);
            return 1;
        }
    }

    printf("make-system-disk: created %s (%llu MiB GPT: EFI + BlackBox + IXFS%s)\n",
           output, (unsigned long long)(disk_size / (1024 * 1024)),
           ab_mode ? " A/B" : "");
    return 0;
}
