/* ============================================================================
 * test_fat32.c -- FAT32 directory-entry tests against an in-memory volume
 *
 * Every case formats its own RAM disk with ONE-SECTOR clusters, the geometry
 * the BlackBox partition really has. At 16 entries per cluster an LFN run
 * routinely crosses a cluster boundary, which no larger-cluster fixture hits.
 * Nothing here touches a mounted volume.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/types.h"
#include "kernel/mm/pmm.h"
#include "libc/string.h"
#include "../fs/fat32/fat32_internal.h"

#define FX_SECTORS      4096u
#define FX_RESERVED     32u
#define FX_FAT_SECTORS  32u
#define FX_ROOT_CLUSTER 2u
#define FX_ENTRY_SIZE   32u
#define FX_DIR_ENTRIES  (512u / FX_ENTRY_SIZE)
#define FX_NO_FAULT     (~(uint64_t)0)
/* 128 FAT entries per sector, so this cluster's entry is not in the root's. */
#define FX_FAR_CLUSTER  300u

struct fx_volume {
    struct fat32_volume *vol;
    uint8_t             *disk;
    struct blkdev        dev;
    uint64_t             vol_pages;
    uint64_t             disk_pages;
    uint64_t             fail_read_lba;     /* FX_NO_FAULT when disarmed */
};

static int fx_read(uint64_t lba, uint32_t count, void *buf, void *data)
{
    struct fx_volume *fx = (struct fx_volume *)data;

    if (lba + count > FX_SECTORS ||
        (fx->fail_read_lba >= lba && fx->fail_read_lba < lba + count))
        return -1;
    memcpy(buf, fx->disk + lba * 512u, (size_t)count * 512u);
    return 0;
}

static int fx_write(uint64_t lba, uint32_t count, const void *buf, void *data)
{
    struct fx_volume *fx = (struct fx_volume *)data;

    if (lba + count > FX_SECTORS)
        return -1;
    memcpy(fx->disk + lba * 512u, buf, (size_t)count * 512u);
    return 0;
}

static int fx_open(struct fx_volume *fx)
{
    uint32_t *fat;

    memset(fx, 0, sizeof(*fx));
    fx->vol_pages = (sizeof(struct fat32_volume) + 4095u) / 4096u;
    fx->disk_pages = (FX_SECTORS * 512u + 4095u) / 4096u;
    fx->vol = (struct fat32_volume *)pmm_alloc_contiguous(fx->vol_pages);
    fx->disk = (uint8_t *)pmm_alloc_contiguous(fx->disk_pages);
    if (!fx->vol || !fx->disk)
        return -1;
    memset(fx->vol, 0, sizeof(*fx->vol));
    memset(fx->disk, 0, FX_SECTORS * 512u);

    fx->fail_read_lba = FX_NO_FAULT;
    fx->dev.sector_size = 512u;
    fx->dev.sector_count = FX_SECTORS;
    fx->dev.read = fx_read;
    fx->dev.write = fx_write;
    fx->dev.driver_data = fx;
    fx->dev.active = 1;

    fx->vol->dev = &fx->dev;
    fx->vol->bpb.bytes_per_sector = 512u;
    fx->vol->bpb.sectors_per_cluster = 1u;
    fx->vol->bpb.reserved_sectors = FX_RESERVED;
    fx->vol->bpb.num_fats = 2u;
    fx->vol->bpb.total_sectors = FX_SECTORS;
    fx->vol->bpb.fat_size_sectors = FX_FAT_SECTORS;
    fx->vol->bpb.root_cluster = FX_ROOT_CLUSTER;
    fx->vol->bpb.first_fat_sector = FX_RESERVED;
    fx->vol->bpb.first_data_sector = FX_RESERVED + 2u * FX_FAT_SECTORS;
    fx->vol->fsinfo_free_count = FSINFO_UNKNOWN;

    fat = (uint32_t *)(fx->disk + FX_RESERVED * 512u);
    fat[0] = 0x0FFFFFF8u;
    fat[1] = 0x0FFFFFFFu;
    fat[FX_ROOT_CLUSTER] = 0x0FFFFFFFu;
    return 0;
}

static void fx_close(struct fx_volume *fx)
{
    if (fx->vol)
        pmm_free_contiguous((uintptr_t)fx->vol, fx->vol_pages);
    if (fx->disk)
        pmm_free_contiguous((uintptr_t)fx->disk, fx->disk_pages);
    fx->vol = (struct fat32_volume *)0;
    fx->disk = (uint8_t *)0;
}

/* The disk bytes of entry `idx` of `cluster`, after flushing the cache. */
static const struct fat32_dir_entry *fx_entry(struct fx_volume *fx,
                                              uint32_t cluster, uint32_t idx)
{
    scache_flush(fx->vol);
    return (const struct fat32_dir_entry *)
        (fx->disk + (uint64_t)cluster_to_sector(fx->vol, cluster) * 512u +
         (uint64_t)idx * FX_ENTRY_SIZE);
}

/* Independent oracle, deliberately sharing no lookup code with the driver:
 * walks the directory chain and returns how many entries carry `name` as a
 * complete LFN run whose checksum binds to the SFN that follows it. */
static uint32_t fx_count_bound_long_name(struct fx_volume *fx, uint32_t dir,
                                         const char *name)
{
    char long_name[FAT32_MAX_NAME];
    uint32_t cluster = dir;
    uint32_t hits = 0;
    uint32_t steps = 0;
    int expect = 0;
    uint8_t chk = 0;

    memset(long_name, 0, sizeof(long_name));
    while (cluster >= 2u && cluster < FAT32_EOC && steps++ < FX_SECTORS) {
        uint32_t i;

        for (i = 0; i < FX_DIR_ENTRIES; i++) {
            const struct fat32_dir_entry *de = fx_entry(fx, cluster, i);

            if (de->name[0] == 0x00)
                return hits;
            if (de->name[0] == 0xE5) {
                expect = 0;
            } else if (de->attr == FAT32_ATTR_LFN) {
                const struct fat32_lfn_entry *lfn =
                    (const struct fat32_lfn_entry *)de;
                int seq = lfn->seq & LFN_SEQ_MASK;

                if (lfn->seq & LFN_LAST_ENTRY) {
                    memset(long_name, 0, sizeof(long_name));
                    expect = seq;
                    chk = lfn->checksum;
                }
                if (expect == seq && lfn->checksum == chk) {
                    lfn_extract_chars(lfn, long_name, seq - 1);
                    expect = seq - 1;
                } else {
                    expect = -1;
                }
            } else {
                if (expect == 0 && long_name[0] != '\0' &&
                    fat32_lfn_checksum(de->name) == chk &&
                    strcmp(long_name, name) == 0)
                    hits++;
                memset(long_name, 0, sizeof(long_name));
                expect = 0;
            }
        }
        cluster = fat32_get_fat_entry(fx->vol, cluster);
    }
    return hits;
}

/* A short-name-only entry takes exactly one slot, so these pad a directory
 * to a chosen fill level. */
static void fx_pad_short(struct fx_volume *fx, uint32_t dir, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        char name[9] = "PAD00";

        name[3] = (char)('0' + (i / 10u));
        name[4] = (char)('0' + (i % 10u));
        TEST_ASSERT_EQ(fat32_create_file_vol(fx->vol, dir, name), 0,
                       "pad entry created");
    }
}

/* The field defect: BlackBox `X:\Tools` first slot overwritten by another
 * directory's new file. Two slots remain in the parent's cluster and the new
 * run needs three, so its last entries belong in a freshly linked cluster, not
 * in whichever cluster happens to follow on disk. */
static void test_fat32_lfn_run_crossing_cluster(void)
{
    struct fx_volume fx;
    uint32_t victim;
    const struct fat32_dir_entry *dot;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "VICTIM"), 0,
                   "neighbour directory created");
    victim = FX_ROOT_CLUSTER + 1u;
    fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES - 3u);

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER,
                                         "boot-reserved.json"), 0,
                   "file whose run crosses the cluster boundary created");

    dot = fx_entry(&fx, victim, 0u);
    TEST_ASSERT_EQ(dot->name[0], (uint8_t)'.',
                   "the neighbouring directory keeps its dot entry");
    TEST_ASSERT_EQ(dot->attr, FAT32_ATTR_DIRECTORY,
                   "the neighbouring dot entry is still a directory");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER,
                                            "boot-reserved.json"), 1u,
                   "the crossing run reads back whole and bound to its SFN");
    fx_close(&fx);
}

/* A name longer than one cluster's worth of slots needs the directory extended
 * by more than one cluster. */
static void test_fat32_lfn_run_longer_than_cluster(void)
{
    struct fx_volume fx;
    char name[201];
    uint32_t i;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    for (i = 0; i < 200u; i++)
        name[i] = (char)('a' + (i % 26u));
    name[200] = '\0';

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, name), 0,
                   "a 16-slot name fits by extending the directory");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, name), 1u,
                   "the 17-entry run reads back whole across the extension");
    fx_close(&fx);
}

/* The numeric tail follows the base. A tail at fixed offset 6 after a short
 * base leaves spaces inside the name part, which FAT forbids. */
static void test_fat32_short_tail_follows_base(void)
{
    struct fx_volume fx;
    uint8_t sfn[11];

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_generate_sfn(fx.vol, FX_ROOT_CLUSTER, "abc.txt", sfn), 0,
                   "short name generated");
    TEST_ASSERT(memcmp(sfn, "ABC~1   TXT", 11) == 0,
                "a short base takes its tail directly after the base");

    TEST_ASSERT_EQ(fat32_generate_sfn(fx.vol, FX_ROOT_CLUSTER,
                                      "boot-reserved.json", sfn), 0,
                   "long base generated");
    TEST_ASSERT(memcmp(sfn, "BOOT-R~1JSO", 11) == 0,
                "a long base is cut to six characters for the tail");
    fx_close(&fx);
}

/* Entries after the first 0x00 are free whatever they hold. A new run must stay
 * in front of them, where readers look, and must not bring them back to life. */
static void test_fat32_stale_bytes_after_terminator(void)
{
    struct fx_volume fx;
    struct fat32_dir_entry *root;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    root = (struct fat32_dir_entry *)
        (fx.disk + (uint64_t)cluster_to_sector(fx.vol, FX_ROOT_CLUSTER) * 512u);
    root[0].name[0] = 0xE5;
    memcpy(root[2].name, "STALE   TXT", 11);
    root[2].attr = FAT32_ATTR_ARCHIVE;
    memcpy(root[3].name, "GHOST   TXT", 11);
    root[3].attr = FAT32_ATTR_ARCHIVE;

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER,
                                         "boot-trend.json"), 0,
                   "file created over the terminator");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER,
                                            "boot-trend.json"), 1u,
                   "the file is in front of the terminator readers stop at");
    TEST_ASSERT_EQ(fx_entry(&fx, FX_ROOT_CLUSTER, 3u)->name[0], (uint8_t)0x00,
                   "a terminator follows the run, so stale bytes stay dead");
    fx_close(&fx);
}

/* The FAT link is flushed later from the write-back cache in slot order. A
 * recycled cluster's old bytes must already be gone from the disk by then. */
static void test_fat32_extension_zeroed_on_disk_before_link(void)
{
    struct fx_volume fx;
    uint8_t *raw;
    uint32_t sector, offset, cluster, i;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES);
    fx.vol->fsinfo_valid = 1;
    fx.vol->fsinfo_next_free = FX_FAR_CLUSTER;
    raw = fx.disk + (uint64_t)cluster_to_sector(fx.vol, FX_FAR_CLUSTER) * 512u;
    memset(raw, 0xAA, 512u);

    TEST_ASSERT_EQ(fat32_find_free_dir_slots(fx.vol, FX_ROOT_CLUSTER, 1u, &sector,
                                             &offset, &cluster), 0,
                   "directory extended");
    TEST_ASSERT_EQ(cluster, FX_FAR_CLUSTER, "the recycled cluster was taken");
    for (i = 0; i < 512u && raw[i] == 0; i++)
        ;
    TEST_ASSERT_EQ(i, 512u, "its old bytes are gone from the disk, unflushed");
    fx_close(&fx);
}

/* FAT1 takes the link, then reading FAT2 fails. The chain may now reference the
 * new cluster, so returning it to the free pool would set up a cross-link. */
static void test_fat32_failed_link_retains_cluster(void)
{
    struct fx_volume fx;
    uint32_t sector, offset, cluster;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES);
    fx.vol->fsinfo_valid = 1;
    fx.vol->fsinfo_next_free = FX_FAR_CLUSTER;
    scache_invalidate(fx.vol);
    fx.fail_read_lba = FX_RESERVED + FX_FAT_SECTORS;

    TEST_ASSERT(fat32_find_free_dir_slots(fx.vol, FX_ROOT_CLUSTER, 1u, &sector,
                                          &offset, &cluster) != 0,
                "the extension reports the failed link");
    fx.fail_read_lba = FX_NO_FAULT;
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, FX_FAR_CLUSTER) != FAT32_FREE,
                "the cluster the chain may reference stays allocated");
    fx_close(&fx);
}

/* A corrupt chain that loops back must fail without touching anything: the
 * second visit to a cluster would otherwise scrub it from offset 0. `second` is
 * 0 for a self-loop, or the other member of a two-cluster cycle. */
static void fx_cycle_case(uint32_t second)
{
    struct fx_volume fx;
    struct fat32_dir_entry *root;
    uint32_t *fat;
    uint32_t sector, offset, cluster;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fat = (uint32_t *)(fx.disk + FX_RESERVED * 512u);
    root = (struct fat32_dir_entry *)
        (fx.disk + (uint64_t)cluster_to_sector(fx.vol, FX_ROOT_CLUSTER) * 512u);
    memcpy(root[0].name, "LIVE    TXT", 11);
    root[0].attr = FAT32_ATTR_ARCHIVE;
    if (second) {
        fat[FX_ROOT_CLUSTER] = second;
        fat[second] = FX_ROOT_CLUSTER;
    } else {
        fat[FX_ROOT_CLUSTER] = FX_ROOT_CLUSTER;
    }

    TEST_ASSERT(fat32_find_free_dir_slots(fx.vol, FX_ROOT_CLUSTER, 2u, &sector,
                                          &offset, &cluster) != 0,
                "a cyclic chain is refused");
    TEST_ASSERT(memcmp(root[0].name, "LIVE    TXT", 11) == 0,
                "the live entry survives on disk");
    scache_flush(fx.vol);
    TEST_ASSERT(memcmp(root[0].name, "LIVE    TXT", 11) == 0,
                "and survives a flush of the cache");
    fx_close(&fx);
}

static void test_fat32_cycle_self_loop(void)      { fx_cycle_case(0u); }
static void test_fat32_cycle_two_clusters(void)   { fx_cycle_case(3u); }

/* A chain pointing out of the data area must be refused the same way. */
static void test_fat32_chain_out_of_range(void)
{
    struct fx_volume fx;
    struct fat32_dir_entry *root;
    uint32_t sector, offset, cluster;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    root = (struct fat32_dir_entry *)
        (fx.disk + (uint64_t)cluster_to_sector(fx.vol, FX_ROOT_CLUSTER) * 512u);
    memcpy(root[0].name, "LIVE    TXT", 11);
    root[0].attr = FAT32_ATTR_ARCHIVE;
    ((uint32_t *)(fx.disk + FX_RESERVED * 512u))[FX_ROOT_CLUSTER] = FX_SECTORS;

    TEST_ASSERT(fat32_find_free_dir_slots(fx.vol, FX_ROOT_CLUSTER, 2u, &sector,
                                          &offset, &cluster) != 0,
                "a chain leaving the data area is refused");
    TEST_ASSERT(memcmp(root[0].name, "LIVE    TXT", 11) == 0,
                "the live entry survives");
    fx_close(&fx);
}

/* The bound is the tightest of three limits, including on geometry a forged BPB
 * can make arbitrarily large. */
static void test_fat32_last_cluster_bound(void)
{
    struct fx_volume fx;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_last_cluster(fx.vol), FX_SECTORS - fx.vol->bpb.first_data_sector + 1u,
                   "a healthy volume is bounded by its data area");

    fx.vol->bpb.fat_size_sectors = 1u;
    TEST_ASSERT_EQ(fat32_last_cluster(fx.vol), 127u,
                   "a short FAT bounds it to the entries the table has");

    fx.vol->bpb.fat_size_sectors = 0x00800000u;
    fx.vol->bpb.total_sectors = 0x20000000u;
    fx.vol->bpb.first_data_sector = 0x00010000u;
    TEST_ASSERT_EQ(fat32_last_cluster(fx.vol), FAT32_MAX_DATA_CLUSTER,
                   "values from the reserved range up are never clusters");
    fx_close(&fx);
}

/* A BAD link is refused whatever the geometry, and the last legal cluster is
 * still accepted. */
static void test_fat32_bad_link_and_last_cluster(void)
{
    struct fx_volume fx;
    uint32_t *fat;
    uint32_t sector, offset, cluster, last;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fat = (uint32_t *)(fx.disk + FX_RESERVED * 512u);
    last = fat32_last_cluster(fx.vol);
    fat[FX_ROOT_CLUSTER] = last;
    fat[last] = 0x0FFFFFFFu;
    TEST_ASSERT_EQ(fat32_find_free_dir_slots(fx.vol, FX_ROOT_CLUSTER, 1u, &sector,
                                             &offset, &cluster), 0,
                   "a chain ending on the last legal cluster is accepted");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);
    fat[FX_ROOT_CLUSTER] = FAT32_BAD;
    TEST_ASSERT(fat32_find_free_dir_slots(fx.vol, FX_ROOT_CLUSTER, 1u, &sector,
                                          &offset, &cluster) != 0,
                "a BAD link is refused");
    fx_close(&fx);
}

static void test_fat32_bpb_refuses_short_fat(void)
{
    struct fx_volume fx;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx.vol->sector_buf[0] = 0xEB;
    fat32_validate_set_quiet(1);
    TEST_ASSERT_EQ(fat32_validate_bpb(fx.vol), 0, "the fixture geometry is valid");
    fx.vol->bpb.fat_size_sectors = 1u;
    TEST_ASSERT(fat32_validate_bpb(fx.vol) != 0,
                "a FAT too short for the data area is refused at mount");
    fat32_validate_set_quiet(0);
    fx_close(&fx);
}

void test_register_fat32(void)
{
    test_suite_register_cat("FAT32: LFN run crossing a directory cluster",
                            test_fat32_lfn_run_crossing_cluster, TEST_CAT_FS);
    test_suite_register_cat("FAT32: LFN run longer than one cluster",
                            test_fat32_lfn_run_longer_than_cluster, TEST_CAT_FS);
    test_suite_register_cat("FAT32: numeric tail follows a short base",
                            test_fat32_short_tail_follows_base, TEST_CAT_FS);
    test_suite_register_cat("FAT32: stale bytes after the terminator",
                            test_fat32_stale_bytes_after_terminator, TEST_CAT_FS);
    test_suite_register_cat("FAT32: extension zeroed on disk before its link",
                            test_fat32_extension_zeroed_on_disk_before_link,
                            TEST_CAT_FS);
    test_suite_register_cat("FAT32: failed FAT link retains the cluster",
                            test_fat32_failed_link_retains_cluster, TEST_CAT_FS);
    test_suite_register_cat("FAT32: self-looping directory chain is refused intact",
                            test_fat32_cycle_self_loop, TEST_CAT_FS);
    test_suite_register_cat("FAT32: two-cluster directory cycle is refused intact",
                            test_fat32_cycle_two_clusters, TEST_CAT_FS);
    test_suite_register_cat("FAT32: out-of-range directory chain is refused intact",
                            test_fat32_chain_out_of_range, TEST_CAT_FS);
    test_suite_register_cat("FAT32: last cluster takes the tightest bound",
                            test_fat32_last_cluster_bound, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BAD link refused, last cluster accepted",
                            test_fat32_bad_link_and_last_cluster, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB with a short FAT is refused",
                            test_fat32_bpb_refuses_short_fat, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
