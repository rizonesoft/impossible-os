/* libFuzzer harness: FAT32 boot sector, FSInfo and directory entries.
 *
 * Input layout: 512 bytes of boot sector, 512 bytes of FSInfo, then directory
 * entries, 32 bytes each, walked the way fat32_read_dir walks a cluster. All
 * of it is on-disk bytes a removable drive supplies. The code under test is
 * the driver's own decoding (src/kernel/fs/fat32/fat32_parse.c). Contract: no
 * input makes it read outside the buffer, an accepted BPB has the geometry
 * the cluster math relies on, and every name it produces is terminated.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fat32_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct fat32_bpb bpb;
    char label[12];
    fat32_lfn_run_t run;
    uint32_t free_count, next_free;

    if (size < 512)
        return 0;
    fat32_validate_set_quiet(1);
    memset(&bpb, 0, sizeof(bpb));

    if (fat32_parse_boot_sector(data, (uint64_t)data[0] << 12, &bpb, label) == 0) {
        if (bpb.sectors_per_cluster == 0 ||
            bpb.first_data_sector >= bpb.total_sectors ||
            bpb.root_cluster < 2 || strlen(label) > 11)
            abort();
        if (size >= 1024 &&
            fat32_parse_fsinfo(data + 512, &bpb, &free_count, &next_free) &&
            next_free < 2)
            abort();
    }

    fat32_lfn_run_reset(&run);
    for (size_t off = 1024; off + 32 <= size; off += 32) {
        const struct fat32_dir_entry *de = (const struct fat32_dir_entry *)(data + off);
        char name[FAT32_MAX_NAME];

        if (de->name[0] == 0x00)
            break;
        if (de->name[0] == 0xE5) { run.active = 0; continue; }
        if (de->attr == FAT32_ATTR_LFN) {
            fat32_lfn_run_feed(&run, (const struct fat32_lfn_entry *)de);
            continue;
        }
        if (de->attr & FAT32_ATTR_VOLUME_ID) { run.active = 0; continue; }
        (void)fat32_dirent_name(&run, de, name);
        if (memchr(name, '\0', sizeof(name)) == NULL)
            abort();
    }
    return 0;
}
