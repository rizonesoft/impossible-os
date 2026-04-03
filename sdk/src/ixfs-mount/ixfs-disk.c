/* ============================================================================
 * ixfs-disk.c -- Platform-abstracted raw disk/image I/O with GPT parsing
 *
 * Linux: open() + pread() / pwrite()
 * ============================================================================ */

#define _POSIX_C_SOURCE 200809L
#include "ixfs-disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512

/* --- GPT structures --- */

#define GPT_HEADER_LBA     1
#define GPT_ENTRY_LBA      2
#define GPT_ENTRY_SIZE     128
#define GPT_SIGNATURE      0x5452415020494645ULL  /* "EFI PART" */

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

/* Parse GPT and find partition by 1-based index.
 * Returns 0 on success, fills start_lba and sector_count. */
static int gpt_find_partition(FILE *fp, int partition_index,
                              uint64_t *start_lba, uint64_t *sector_count)
{
    uint8_t hdr[SECTOR_SIZE];
    uint8_t entry[GPT_ENTRY_SIZE];
    uint32_t num_entries, entry_size;
    uint64_t entry_lba;
    int found = 0;
    int idx = 0;

    /* Read GPT header at LBA 1 */
    if (fseeko(fp, (off_t)(GPT_HEADER_LBA * SECTOR_SIZE), SEEK_SET) != 0)
        return -1;
    if (fread(hdr, SECTOR_SIZE, 1, fp) != 1)
        return -1;

    /* Verify GPT signature */
    if (read_le64(hdr) != GPT_SIGNATURE) {
        fprintf(stderr, "disk: no GPT signature found\n");
        return -1;
    }

    entry_lba  = read_le64(hdr + 72);
    num_entries = read_le32(hdr + 80);
    entry_size  = read_le32(hdr + 84);

    if (entry_size < GPT_ENTRY_SIZE)
        entry_size = GPT_ENTRY_SIZE;

    /* Seek to partition entry array */
    if (fseeko(fp, (off_t)(entry_lba * SECTOR_SIZE), SEEK_SET) != 0)
        return -1;

    for (uint32_t i = 0; i < num_entries; i++) {
        /* Read entry (may be larger than 128 bytes, but we only need first 128) */
        if (fread(entry, GPT_ENTRY_SIZE, 1, fp) != 1)
            break;

        /* Skip padding if entry_size > 128 */
        if (entry_size > GPT_ENTRY_SIZE) {
            if (fseeko(fp, (off_t)(entry_size - GPT_ENTRY_SIZE), SEEK_CUR) != 0)
                break;
        }

        /* Check if entry is used (type GUID not all zeros) */
        int all_zero = 1;
        for (int j = 0; j < 16; j++) {
            if (entry[j] != 0) { all_zero = 0; break; }
        }
        if (all_zero)
            continue;

        idx++;  /* 1-based partition counter */

        if (idx == partition_index) {
            uint64_t first = read_le64(entry + 32);
            uint64_t last  = read_le64(entry + 40);
            *start_lba = first;
            *sector_count = last - first + 1;
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "disk: partition %d not found (found %d partitions)\n",
                partition_index, idx);
        return -1;
    }

    return 0;
}

/* --- Public API --- */

ixfs_disk_ctx_t *disk_open(const char *path, int partition_index)
{
    FILE *fp;
    uint64_t start_lba, sector_count;
    ixfs_disk_ctx_t *disk;

    fp = fopen(path, "r+b");
    if (!fp) fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    if (partition_index == 0) {
        /* Partition index 0: treat entire file as raw IXFS volume (no GPT) */
        fseeko(fp, 0, SEEK_END);
        uint64_t file_size = (uint64_t)ftello(fp);
        fseeko(fp, 0, SEEK_SET);

        disk = calloc(1, sizeof(ixfs_disk_ctx_t));
        if (!disk) { fclose(fp); return NULL; }

        disk->fp = fp;
        disk->part_offset = 0;
        disk->part_size = file_size;
        return disk;
    }

    if (gpt_find_partition(fp, partition_index, &start_lba, &sector_count) != 0) {
        fclose(fp);
        return NULL;
    }

    disk = calloc(1, sizeof(ixfs_disk_ctx_t));
    if (!disk) {
        fclose(fp);
        return NULL;
    }

    disk->fp = fp;
    disk->part_offset = start_lba * SECTOR_SIZE;
    disk->part_size = sector_count * SECTOR_SIZE;

    return disk;
}

int disk_read_sectors(ixfs_disk_ctx_t *disk, uint64_t lba, uint32_t count,
                      void *buf)
{
    uint64_t byte_offset = disk->part_offset + lba * SECTOR_SIZE;
    uint64_t byte_count = (uint64_t)count * SECTOR_SIZE;

    if (lba * SECTOR_SIZE + byte_count > disk->part_size)
        return -1;

    if (fseeko(disk->fp, (off_t)byte_offset, SEEK_SET) != 0)
        return -1;

    if (fread(buf, byte_count, 1, disk->fp) != 1)
        return -1;

    return 0;
}

int disk_write_sectors(ixfs_disk_ctx_t *disk, uint64_t lba, uint32_t count,
                       const void *buf)
{
    uint64_t byte_offset = disk->part_offset + lba * SECTOR_SIZE;
    uint64_t byte_count = (uint64_t)count * SECTOR_SIZE;

    if (lba * SECTOR_SIZE + byte_count > disk->part_size)
        return -1;

    if (fseeko(disk->fp, (off_t)byte_offset, SEEK_SET) != 0)
        return -1;

    if (fwrite(buf, byte_count, 1, disk->fp) != 1)
        return -1;

    return 0;
}

void disk_close(ixfs_disk_ctx_t *disk)
{
    if (!disk)
        return;
    if (disk->fp) {
        fflush(disk->fp);
        fclose(disk->fp);
    }
    free(disk);
}
