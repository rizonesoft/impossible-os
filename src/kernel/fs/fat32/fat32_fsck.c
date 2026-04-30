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
                            int fix, int *repair_failed)
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
                /* Truncate the chain at the previous cluster.
                 * fat32_set_fat_entry returns -1 on FAT sector R/W
                 * failure; without propagating, a torn truncate would
                 * declare the FS clean while the chain still aliases. */
                if (fat32_set_fat_entry(vol, prev, 0x0FFFFFFF) != 0) {
                    klog(LOG_ERROR, "fsck",
                         "truncate FAT write failed at cluster %u",
                         (uint64_t)prev);
                    *repair_failed = 1;
                }
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
/* walk_directory: traverse a directory cluster chain, marking every
 * reached cluster in `visited`. Returns the count of cross-linked
 * clusters detected. Sets *traversal_failed = 1 on ANY allocation or
 * read failure that left the visited bitmap incomplete -- the caller
 * MUST treat a partial walk as a "do not run lost-cluster scan in fix
 * mode" signal, otherwise the post-walk lost-cluster pass would free
 * every cluster the incomplete traversal failed to reach (data wipe).
 *
 * Allocation note: `bytes_per_cluster = sectors_per_cluster * 512`
 * ranges 512..65536 across valid BPBs. CLAUDE.md mandates
 * pmm_alloc_contiguous for allocations > 4 KiB; use it when the
 * cluster spills past one page so cluster sizes >= 8 KiB do not
 * implicitly fail kmalloc and pretend success. Allocation failure
 * here is now FATAL to the walk, not a silent partial scan. */
static uint32_t walk_directory(struct fat32_volume *vol, uint8_t *visited,
                                uint32_t dir_cluster, uint32_t max_cluster,
                                int fix, int *traversal_failed,
                                int *repair_failed)
{
    uint32_t bytes_per_cluster = vol->bpb.sectors_per_cluster * 512;
    uint8_t *cluster_buf;
    uint32_t pmm_pages = 0;
    uint32_t cur_cluster = dir_cluster;
    uint32_t cross_links = 0;

    if (bytes_per_cluster <= 4096u) {
        cluster_buf = (uint8_t *)kmalloc(bytes_per_cluster);
    } else {
        pmm_pages = (bytes_per_cluster + 4095u) / 4096u;
        cluster_buf = (uint8_t *)pmm_alloc_contiguous(pmm_pages);
    }
    if (!cluster_buf) {
        *traversal_failed = 1;
        return 0;
    }

    /* Mark directory's own cluster chain */
    cross_links += walk_chain(vol, visited, dir_cluster, max_cluster, fix,
                              repair_failed);

    /* Walk directory entries */
    cur_cluster = dir_cluster;
    while (cur_cluster >= 2 && cur_cluster < max_cluster) {
        uint32_t sector = cluster_to_sector(vol, cur_cluster);
        uint32_t i;

        if (fat32_read_sectors_multi(vol, sector, vol->bpb.sectors_per_cluster,
                                     cluster_buf) != 0) {
            /* Read failure mid-walk: the visited bitmap is incomplete
             * for any subdirectories not yet recursed into. Mark the
             * walk failed so fsck skips the lost-cluster freeing pass. */
            *traversal_failed = 1;
            break;
        }

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
                /* Recurse into subdirectory; failure propagates via
                 * traversal_failed and repair_failed -- no need to
                 * inspect the count. */
                cross_links += walk_directory(vol, visited, fc,
                                              max_cluster, fix,
                                              traversal_failed,
                                              repair_failed);
            } else {
                /* Walk file's cluster chain */
                cross_links += walk_chain(vol, visited, fc,
                                          max_cluster, fix,
                                          repair_failed);
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
    if (pmm_pages > 0) {
        uintptr_t base = (uintptr_t)cluster_buf;
        uint32_t pg;
        for (pg = 0; pg < pmm_pages; pg++)
            pmm_free_frame(base + (uintptr_t)pg * 4096u);
    } else {
        kfree(cluster_buf);
    }
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
    int traversal_failed = 0;
    int repair_failed = 0;
    cross_links = walk_directory(vol, visited, vol->bpb.root_cluster,
                                 max_cluster, fix, &traversal_failed,
                                 &repair_failed);

    /* Step 4: Detect lost clusters
     *
     * SAFETY GATE: only run this in fix mode if the directory walk
     * fully completed. A partial walk leaves the visited bitmap with
     * unmarked file/subdir clusters; running the lost-cluster freeing
     * pass on an incomplete map would free live data (catastrophic on
     * an auto-fsck-on-mount path -- a transient OOM during walk would
     * wipe the volume). Read-only mode (fix=0) still scans to count
     * "lost-or-unwalked" clusters for the diagnostic, but does not
     * mutate the FAT. */
    if (traversal_failed) {
        klog(LOG_ERROR, "fsck",
             "directory traversal incomplete; SKIPPING lost-cluster "
             "freeing pass (would free live data on a partial walk)");
    }
    for (cluster = 2; cluster < max_cluster; cluster++) {
        uint32_t fat_val = fat32_get_fat_entry(vol, cluster);
        if (fat_val != FAT32_FREE && !bitset_test(visited, cluster)) {
            lost_clusters++;
            if (fix && !traversal_failed) {
                /* Free the lost cluster. fat32_set_fat_entry returns
                 * -1 on FAT R/W error -- propagate so the auto-fsck-
                 * on-mount path does NOT clear the dirty bit and
                 * expose a still-corrupted FAT. */
                if (fat32_set_fat_entry(vol, cluster, FAT32_FREE) != 0) {
                    klog(LOG_ERROR, "fsck",
                         "free FAT write failed at cluster %u",
                         (uint64_t)cluster);
                    repair_failed = 1;
                }
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
         traversal_failed ? "Errors remain (partial walk)" :
         (fix ? (errors ? "Fixed" : "Clean") :
                (errors ? "Errors remain" : "Clean")));

    /* Return contract:
     *   0  = filesystem is now consistent. In fix=1 mode this means
     *        the walk completed AND every repair write (truncate +
     *        free) succeeded; the FS no longer has the errors fsck
     *        found. In fix=0 mode it means no errors were found.
     *   -1 = filesystem state is NOT trustworthy. Possible causes:
     *        (a) traversal_failed -- incomplete walk; no mutations
     *            possible without risking data loss.
     *        (b) repair_failed -- fix=1 attempted a FAT write
     *            (truncate or free) that returned -1; the FS may now
     *            be in a torn state somewhere between original and
     *            repaired.
     *        (c) fix=0 reported errors that have not been repaired.
     *
     * The auto-fsck-on-dirty-mount path in fat32_init relies on this
     * split: returning -1 for "I just fixed N errors" would falsely
     * fail every dirty mount that fsck actually repaired (service-
     * outage regression for common power-loss recovery). Conversely,
     * returning 0 after a torn repair would expose a corrupt FAT to
     * normal VFS writes -- worse than the original dirty mount. */
    if (traversal_failed || (fix && repair_failed))
        return -1;
    if (!fix && errors > 0)
        return -1;
    return 0;
}
