/* ============================================================================
 * fat32_fsck.c -- FAT32 filesystem consistency checker
 *
 * Validates BPB, compares FAT1/FAT2, detects cross-linked cluster chains
 * and lost clusters. When fix=1, repairs FAT2 from FAT1 and truncates
 * cross-linked chains.
 *
 * Uses a visited[] bitset (1 bit per cluster) allocated from PMM to track
 * which clusters are reachable from directory entries.
 * ============================================================================ */

#include "fat32_internal.h"

/* Bitset helpers for the visited[] array */
static inline void bitset_set(uint8_t *bits, uint32_t idx)
{
    bits[idx / 8] |= (uint8_t)(1 << (idx % 8));
}

static inline int bitset_test(const uint8_t *bits, uint32_t idx)
{
    return (bits[idx / 8] >> (idx % 8)) & 1;
}

/* Walk a cluster chain and mark all clusters in visited[].
 * Returns the number of cross-links detected. */
static uint32_t walk_chain(struct fat32_volume *vol, uint8_t *visited,
                            uint32_t start_cluster, uint32_t max_cluster,
                            int fix)
{
    uint32_t cluster = start_cluster;
    uint32_t prev = 0;
    uint32_t cross_links = 0;

    while (cluster >= 2 && cluster < max_cluster) {
        uint32_t fat_val = fat32_get_fat_entry(vol, cluster);

        if (bitset_test(visited, cluster)) {
            /* Cross-link detected */
            cross_links++;
            klog(LOG_WARN, "fsck",
                 "cross-linked chain at cluster %u", (uint64_t)cluster);
            if (fix && prev) {
                /* Truncate the chain at the previous cluster */
                fat32_set_fat_entry(vol, prev, 0x0FFFFFFF);
                klog(LOG_INFO, "fsck",
                     "truncated chain at cluster %u", (uint64_t)prev);
            }
            break;
        }

        bitset_set(visited, cluster);
        prev = cluster;

        if (fat_val >= 0x0FFFFFF8)
            break;  /* end of chain */
        if (fat_val == FAT32_BAD)
            break;  /* bad cluster */
        if (fat_val < 2)
            break;  /* invalid */

        cluster = fat_val;
    }

    return cross_links;
}

/* Recursively walk directory entries and mark all reachable clusters. */
static uint32_t walk_directory(struct fat32_volume *vol, uint8_t *visited,
                                uint32_t dir_cluster, uint32_t max_cluster,
                                int fix)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t cur_cluster = dir_cluster;
    uint32_t cross_links = 0;

    cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    if (!cluster_buf)
        return 0;

    /* Mark directory's own cluster chain */
    cross_links += walk_chain(vol, visited, dir_cluster, max_cluster, fix);

    /* Walk directory entries */
    cur_cluster = dir_cluster;
    while (cur_cluster >= 2 && cur_cluster < max_cluster) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0)
            break;

        for (i = 0; i < bytes_per_cluster; i += 32) {
            struct fat32_dir_entry *de =
                (struct fat32_dir_entry *)&cluster_buf[i];
            uint32_t fc;

            if (de->name[0] == 0x00) goto dir_done;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            /* Skip . and .. */
            if (de->name[0] == '.' &&
                (de->name[1] == ' ' ||
                 (de->name[1] == '.' && de->name[2] == ' ')))
                continue;

            fc = ((uint32_t)de->first_cluster_hi << 16)
               | (uint32_t)de->first_cluster_lo;

            if (fc < 2 || fc >= max_cluster)
                continue;

            if (de->attr & FAT32_ATTR_DIRECTORY) {
                /* Recurse into subdirectory */
                cross_links += walk_directory(vol, visited, fc,
                                              max_cluster, fix);
            } else {
                /* Walk file's cluster chain */
                cross_links += walk_chain(vol, visited, fc,
                                          max_cluster, fix);
            }
        }

        {
            uint32_t next = fat32_get_fat_entry(vol, cur_cluster);
            if (next < 2 || next >= 0x0FFFFFF8)
                break;
            cur_cluster = next;
        }
    }

dir_done:
    kfree(cluster_buf);
    return cross_links;
}

int fat32_fsck(struct fat32_volume *vol, int fix)
{
    uint32_t total_data_clusters;
    uint32_t max_cluster;
    uint32_t bitset_bytes;
    uint32_t bitset_pages;
    uint8_t *visited;
    uint32_t cross_links = 0;
    uint32_t lost_clusters = 0;
    uint32_t errors = 0;
    uint32_t cluster;
    uint32_t i;

    if (!vol) return -1;

    klog(LOG_INFO, "fsck", "FAT32 fsck starting (%s mode)",
         fix ? "repair" : "read-only");

    /* Step 1: Validate BPB */
    if (fat32_validate_bpb(vol) != 0) {
        klog(LOG_ERROR, "fsck", "BPB validation failed");
        return -1;
    }

    /* Step 2: Compare FAT1 vs FAT2 */
    fat32_compare_repair_fats(vol);

    /* Step 3: Build visited bitset and walk all reachable clusters */
    total_data_clusters = (vol->bpb.total_sectors - vol->bpb.first_data_sector)
                        / vol->bpb.sectors_per_cluster;
    max_cluster = total_data_clusters + 2;

    bitset_bytes = (max_cluster + 7) / 8;
    bitset_pages = (bitset_bytes + 4095) / 4096;

    visited = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(bitset_pages);
    if (!visited) {
        klog(LOG_ERROR, "fsck", "cannot allocate visited bitset (%u pages)",
             (uint64_t)bitset_pages);
        return -1;
    }

    /* Zero the bitset */
    for (i = 0; i < bitset_pages * 4096; i++)
        visited[i] = 0;

    /* Mark clusters 0 and 1 as visited (reserved) */
    bitset_set(visited, 0);
    bitset_set(visited, 1);

    /* Walk from root directory */
    cross_links = walk_directory(vol, visited, vol->bpb.root_cluster,
                                 max_cluster, fix);

    /* Step 4: Detect lost clusters */
    for (cluster = 2; cluster < max_cluster; cluster++) {
        uint32_t fat_val = fat32_get_fat_entry(vol, cluster);
        if (fat_val != FAT32_FREE && !bitset_test(visited, cluster)) {
            lost_clusters++;
            if (fix) {
                /* Free the lost cluster */
                fat32_set_fat_entry(vol, cluster, FAT32_FREE);
            }
        }
    }

    /* Free the bitset */
    {
        uint32_t pg;
        uintptr_t base = (uintptr_t)visited;
        for (pg = 0; pg < bitset_pages; pg++)
            pmm_free_frame(base + pg * 4096);
    }

    errors = cross_links + lost_clusters;

    /* Report */
    klog(LOG_INFO, "fsck",
         "%u errors, %u cross-links, %u lost clusters -- %s",
         (uint64_t)errors, (uint64_t)cross_links, (uint64_t)lost_clusters,
         fix ? (errors ? "Fixed" : "Clean") :
               (errors ? "Errors remain" : "Clean"));

    return errors > 0 ? -1 : 0;
}
