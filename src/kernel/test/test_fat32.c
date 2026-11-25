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
#include "kernel/nt/nt_types.h"
#include "kernel/nt/nt_file.h"
#include "../fs/fat32/fat32_internal.h"
#include "kernel/test/klog_suppress.h"   /* TEST_KLOG_EXPECT */

#define FX_SECTORS      4096u
#define FX_RESERVED     32u
#define FX_FAT_SECTORS  32u
#define FX_ROOT_CLUSTER 2u
#define FX_ENTRY_SIZE   32u
#define FX_DIR_ENTRIES  (512u / FX_ENTRY_SIZE)
#define FX_NO_FAULT     (~(uint64_t)0)
#define FX_NO_CRASH     (~(uint32_t)0)
/* 128 FAT entries per sector, so this cluster's entry is not in the root's. */
#define FX_FAR_CLUSTER  300u

struct fx_volume {
    struct fat32_volume *vol;
    uint8_t             *disk;
    struct blkdev        dev;
    uint64_t             vol_pages;
    uint64_t             disk_pages;
    uint64_t             fail_read_lba;     /* FX_NO_FAULT when disarmed */
    uint32_t             fail_read_skip;    /* reads of that LBA to let through first */
    uint64_t             fail_write_lba;    /* FX_NO_FAULT when disarmed */
    uint32_t             writes;            /* device writes that landed */
    uint32_t             crash_after;       /* FX_NO_CRASH when disarmed */
};

static int fx_read(uint64_t lba, uint32_t count, void *buf, void *data)
{
    struct fx_volume *fx = (struct fx_volume *)data;

    if (lba + count > FX_SECTORS)
        return -1;
    if (fx->fail_read_lba >= lba && fx->fail_read_lba < lba + count) {
        if (fx->fail_read_skip == 0)
            return -1;
        fx->fail_read_skip--;
    }
    memcpy(buf, fx->disk + lba * 512u, (size_t)count * 512u);
    return 0;
}

static int fx_write(uint64_t lba, uint32_t count, const void *buf, void *data)
{
    struct fx_volume *fx = (struct fx_volume *)data;

    if (lba + count > FX_SECTORS)
        return -1;
    if (fx->fail_write_lba >= lba && fx->fail_write_lba < lba + count)
        return -1;
    /* A crash loses this write and every later one, with no error to react to. */
    if (fx->crash_after != FX_NO_CRASH && fx->writes >= fx->crash_after)
        return 0;
    fx->writes++;
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
    fx->fail_write_lba = FX_NO_FAULT;
    fx->crash_after = FX_NO_CRASH;
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
    memcpy(fat + FX_FAT_SECTORS * FAT32_ENTRIES_PER_SECTOR, fat, 3u * sizeof(uint32_t));
    fx->disk[0] = 0xEB;     /* the one boot-sector byte fsck's BPB check reads */
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

static uint32_t fx_first_cluster(const struct fat32_dirent_loc *loc)
{
    return ((uint32_t)loc->de.first_cluster_hi << 16) | loc->de.first_cluster_lo;
}

static uint32_t fx_cluster_of(struct fat32_volume *vol, uint32_t dir, const char *name)
{
    struct fat32_dirent_loc loc;

    return (fat32_dir_lookup(vol, dir, name, &loc) == 1) ? fx_first_cluster(&loc) : 0u;
}

/* The field proof: both names reduce to BOOT-T~1JSO, so create gave the second
 * ~2. Every operation must reach the file it was named for. */
static void test_fat32_colliding_names_reach_their_own_file(void)
{
    struct fx_volume fx;
    uint32_t timeline, trend;
    filetime_t when;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-timeline.json"), 0,
                   "first colliding file created");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json"), 0,
                   "second colliding file created");
    timeline = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-timeline.json");
    trend = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json");
    TEST_ASSERT(timeline >= 2u && trend >= 2u && timeline != trend,
                "each name resolves, and to its own file");

    TEST_ASSERT_EQ(fat32_set_attr(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json",
                                  FAT32_ATTR_HIDDEN), 0, "set_attr by long name");
    {
        struct fat32_dirent_loc loc;
        TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "boot-timeline.json", &loc), 1,
                       "first file still found");
        TEST_ASSERT_EQ(loc.de.attr, FAT32_ATTR_ARCHIVE, "set_attr left the other file alone");
    }
    memset(&when, 0, sizeof(when));
    when.seconds = 1000000u;
    TEST_ASSERT_EQ(fat32_set_times(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json",
                                   (filetime_t *)0, &when, (filetime_t *)0), 0,
                   "set_times by long name");

    TEST_ASSERT_EQ(fat32_delete_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json"), 0,
                   "delete by long name");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-timeline.json"), 1u,
                   "delete left the colliding file in place");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-trend.json"), 0u,
                   "delete removed the named file");
    TEST_ASSERT_EQ(fat32_get_fat_entry(fx.vol, trend), FAT32_FREE, "its cluster is free");
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, timeline) != FAT32_FREE,
                "the other file's cluster is not");
    fx_close(&fx);
}

/* A guessed alias must never match. Only the alias actually on disk does, and a
 * real long name outranks an alias. */
static void test_fat32_lookup_alias_rules(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc loc;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-timeline.json"), 0,
                   "file created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json", &loc), 0,
                   "a name that merely mangles to the same alias is absent");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "BOOT-T~1.JSO", &loc), 1,
                   "the on-disk alias resolves");

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "a-very-long-name.txt"), 0,
                   "second file created");
    {
        uint32_t expect = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "a-very-long-name.txt");
        TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "A-VERY-LONG-NAME.TXT", &loc), 1,
                       "long names match without regard to case");
        TEST_ASSERT_EQ(fx_first_cluster(&loc), expect, "and find the same file");
    }
    fx_close(&fx);
}

/* A run whose checksum does not bind to the following short entry names nothing,
 * so a crafted directory cannot alias a long name onto an unrelated file. */
static void test_fat32_lookup_ignores_unbound_run(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc loc;
    struct fat32_lfn_entry *lfn;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "report.json"), 0,
                   "file created");
    scache_flush(fx.vol);
    lfn = (struct fat32_lfn_entry *)
        (fx.disk + (uint64_t)cluster_to_sector(fx.vol, FX_ROOT_CLUSTER) * 512u);
    TEST_ASSERT_EQ(lfn->attr, FAT32_ATTR_LFN, "the first entry is the run");
    lfn->checksum ^= 0x5Au;
    scache_invalidate(fx.vol);

    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "report.json", &loc), 0,
                   "an unbound run does not name the file");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "REPORT~1.JSO", &loc), 1,
                   "the file stays reachable by its short name");
    fx_close(&fx);
}

/* The long-name run state machine on its own: a run names the short entry
 * only when it starts at a LAST slot, counts down one at a time to slot 1
 * under one checksum, and that checksum is the short name's. */
static void fx_lfn(struct fat32_lfn_entry *e, uint8_t seq, uint8_t chk, char c)
{
    uint32_t i;
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    e->attr = FAT32_ATTR_LFN;
    e->checksum = chk;
    for (i = 0; i < 5; i++) e->name1[i] = (uint16_t)c;
    for (i = 0; i < 6; i++) e->name2[i] = (uint16_t)c;
    for (i = 0; i < 2; i++) e->name3[i] = (uint16_t)c;
}

/* FSInfo is a hint: a free count above the volume's cluster count is forged
 * and becomes "unknown" so the driver rescans; a plausible one is kept. */
static void fx_put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

static void test_fat32_fsinfo_free_count_bounded(void)
{
    static uint8_t fsi[512];
    struct fat32_bpb bpb;
    uint32_t free_count, next_free;

    memset(fsi, 0, sizeof(fsi));
    memset(&bpb, 0, sizeof(bpb));
    bpb.total_sectors = 1000 + 64;
    bpb.first_data_sector = 64;
    bpb.sectors_per_cluster = 1;              /* 1000 data clusters */
    fx_put32(fsi + 0, FSINFO_LEAD_SIG);
    fx_put32(fsi + 484, FSINFO_STRUCT_SIG);
    fx_put32(fsi + 508, FSINFO_TRAIL_SIG);
    fx_put32(fsi + 492, 5);

    fx_put32(fsi + 488, 1000);
    TEST_ASSERT_EQ(fat32_parse_fsinfo(fsi, &bpb, &free_count, &next_free), 1, "FSInfo accepted");
    TEST_ASSERT_EQ(free_count, 1000, "a count equal to the cluster count is kept");
    TEST_ASSERT_EQ(next_free, 5, "an in-range hint is kept");

    fx_put32(fsi + 488, 1001);
    fat32_parse_fsinfo(fsi, &bpb, &free_count, &next_free);
    TEST_ASSERT_EQ(free_count, FSINFO_UNKNOWN, "a count above the cluster count is unknown");

    fx_put32(fsi + 488, FSINFO_UNKNOWN);
    fat32_parse_fsinfo(fsi, &bpb, &free_count, &next_free);
    TEST_ASSERT_EQ(free_count, FSINFO_UNKNOWN, "unknown stays unknown");
}

static void test_fat32_lfn_run_sequence_rules(void)
{
    static const uint8_t sfn[11] = { 'R','E','P','O','R','T','~','1','J','S','O' };
    uint8_t chk = fat32_lfn_checksum(sfn);
    fat32_lfn_run_t run;
    struct fat32_lfn_entry e;

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, LFN_LAST_ENTRY | 2, chk, 'b'); fat32_lfn_run_feed(&run, &e);
    fx_lfn(&e, 1, chk, 'a');                  fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 1, "a complete descending run binds");
    TEST_ASSERT_EQ(run.name[0], 'a', "slot 1 holds the first characters");
    TEST_ASSERT_EQ(run.name[13], 'b', "slot 2 follows");
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 0, "a run binds once");

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, LFN_LAST_ENTRY | 2, chk, 'b'); fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 0, "a run missing slot 1 does not bind");

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, LFN_LAST_ENTRY | 3, chk, 'c'); fat32_lfn_run_feed(&run, &e);
    fx_lfn(&e, 1, chk, 'a');                  fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 0, "a skipped slot breaks the run");

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, LFN_LAST_ENTRY | 2, chk, 'b'); fat32_lfn_run_feed(&run, &e);
    fx_lfn(&e, 2, chk, 'b');                  fat32_lfn_run_feed(&run, &e);
    fx_lfn(&e, 1, chk, 'a');                  fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 0, "a repeated slot breaks the run");

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, LFN_LAST_ENTRY | 2, chk, 'b');          fat32_lfn_run_feed(&run, &e);
    fx_lfn(&e, 1, (uint8_t)(chk ^ 1u), 'a');           fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 0, "a slot under another checksum breaks the run");

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, LFN_LAST_ENTRY | 0, chk, 'a'); fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 0, "sequence 0 never binds");

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, LFN_LAST_ENTRY | (FAT32_LFN_MAX_ENTRIES + 1), chk, 'a');
    fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT(run.active == 0, "a sequence past the longest name is dropped");

    fat32_lfn_run_reset(&run);
    fx_lfn(&e, 1, chk, 'a');                  fat32_lfn_run_feed(&run, &e);
    TEST_ASSERT_EQ(fat32_lfn_run_bind(&run, sfn), 0, "a run that never started at a LAST slot does not bind");
}

/* The listing and stat use the same binding rule as lookup: a run whose
 * checksum does not match the short name that follows it names nothing, so
 * neither shows a long name that lookup would then refuse to open. */
static void test_fat32_listing_and_stat_ignore_unbound_run(void)
{
    struct fx_volume fx;
    struct fat32_stat_info info;
    struct fat32_lfn_entry *lfn;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "report.json"), 0,
                   "file created");
    scache_flush(fx.vol);
    fat32_read_dir(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT_EQ(fx.vol->dir_file_count, 1, "the listing holds the file");
    TEST_ASSERT_EQ(strcmp(fx.vol->dir_files[0].node.name, "report.json"), 0,
                   "a bound run gives the long name");

    lfn = (struct fat32_lfn_entry *)
        (fx.disk + (uint64_t)cluster_to_sector(fx.vol, FX_ROOT_CLUSTER) * 512u);
    TEST_ASSERT_EQ(lfn->attr, FAT32_ATTR_LFN, "the first entry is the run");
    lfn->checksum ^= 0x5Au;
    scache_invalidate(fx.vol);
    fx.vol->dir_cached_cluster = 0xFFFFFFFFu;

    fat32_read_dir(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT_EQ(fx.vol->dir_file_count, 1, "the listing still holds the file");
    TEST_ASSERT_EQ(strcmp(fx.vol->dir_files[0].node.name, "report~1.jso"), 0,
                   "an unbound run leaves the short name in the listing");
    TEST_ASSERT(fat32_stat(fx.vol, "report.json", &info) != 0,
                "stat does not find the file by an unbound long name");
    TEST_ASSERT_EQ(fat32_stat(fx.vol, "REPORT~1.JSO", &info), 0,
                   "stat finds it by its short name");
    fx_close(&fx);
}

/* Delete and rmdir must clear a run that starts in an earlier directory cluster,
 * with no slot left behind to bind to a future short name. */
static void test_fat32_remove_run_across_clusters(void)
{
    struct fx_volume fx;
    uint32_t i;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES - 2u);
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-reserved.json"), 0,
                   "file whose run crosses the boundary created");
    TEST_ASSERT_EQ(fat32_delete_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-reserved.json"), 0,
                   "the crossing run is deleted");
    for (i = FX_DIR_ENTRIES - 2u; i < FX_DIR_ENTRIES; i++)
        TEST_ASSERT_EQ(fx_entry(&fx, FX_ROOT_CLUSTER, i)->name[0], (uint8_t)0xE5,
                       "the slots in the first cluster are gone");
    {
        uint32_t next = fat32_get_fat_entry(fx.vol, FX_ROOT_CLUSTER);
        TEST_ASSERT_EQ(fx_entry(&fx, next, 0u)->name[0], (uint8_t)0xE5,
                       "and so is the short entry in the second");
    }

    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "long-directory-name"), 0,
                   "long-named directory created");
    {
        struct fat32_dirent_loc loc;
        TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "long-directory-name", &loc), 1,
                       "directory found");
        TEST_ASSERT(fat32_delete_file_vol(fx.vol, FX_ROOT_CLUSTER, "long-directory-name") != 0,
                    "delete refuses a directory");
        TEST_ASSERT_EQ(fat32_rmdir(fx.vol, FX_ROOT_CLUSTER, "long-directory-name"), 0,
                       "rmdir removes it by long name");
        TEST_ASSERT_EQ(fx_entry(&fx, loc.run_cluster, loc.run_offset / FX_ENTRY_SIZE)->name[0],
                       (uint8_t)0xE5, "and clears its run, not only its short entry");
    }
    fx_close(&fx);
}

/* A read failure while checking a directory is not proof it is empty. */
static void test_fat32_rmdir_read_failure_keeps_directory(void)
{
    struct fx_volume fx;
    uint32_t dir;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "HOLDER"), 0,
                   "directory created");
    dir = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "HOLDER");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);
    fx.fail_read_lba = cluster_to_sector(fx.vol, dir);
    TEST_ASSERT(fat32_rmdir(fx.vol, FX_ROOT_CLUSTER, "HOLDER") != 0,
                "rmdir refuses what it cannot read");
    fx.fail_read_lba = FX_NO_FAULT;
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "HOLDER"), dir, "the directory stays");
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, dir) != FAT32_FREE, "with its clusters");
    fx_close(&fx);
}

/* Attribute and time changes go to the directory holding the file. The ops layer
 * used to search the root whatever the file's parent was. */
static void test_fat32_set_attr_uses_parent_directory(void)
{
    struct fx_volume fx;
    struct fat32_file file;
    struct fat32_dirent_loc loc;
    uint32_t sub;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "SUB"), 0, "subdirectory created");
    sub = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "SUB");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, sub, "SHARED.DAT"), 0, "file in subdirectory");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "SHARED.DAT"), 0,
                   "same name in the root");

    memset(&file, 0, sizeof(file));
    file.volume = fx.vol;
    file.dir_cluster = sub;
    file.node.fs_data = &file;
    fat32_strcpy(file.node.name, "SHARED.DAT", sizeof(file.node.name));

    TEST_ASSERT_EQ(fat32_file_ops.set_attr(&file.node, FAT32_ATTR_HIDDEN), 0,
                   "set_attr through the ops table");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, sub, "SHARED.DAT", &loc), 1, "subdirectory file found");
    TEST_ASSERT_EQ(loc.de.attr, FAT32_ATTR_HIDDEN, "the subdirectory's file changed");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "SHARED.DAT", &loc), 1, "root file found");
    TEST_ASSERT_EQ(loc.de.attr, FAT32_ATTR_ARCHIVE, "the root's file did not");
    fx_close(&fx);
}

/* Renaming writes a fresh run bound to a fresh short name. */
static void test_fat32_rename_rebinds_long_name(void)
{
    struct fx_volume fx;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-timeline.json"), 0,
                   "colliding neighbour created");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp"), 0,
                   "source created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");

    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp",
                                    "boot-trend.json", 0u), 0, "rename to a free long name");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-trend.json"), 1u,
                   "the new long name is bound to its short entry");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-trend.json.tmp"), 0u,
                   "the old long name is gone");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json"), data,
                   "the renamed file keeps its data");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-timeline.json"), 1u,
                   "the neighbour is untouched");

    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json",
                                 "boot-timeline.json", 0u) != 0,
                "rename onto an existing name needs the replace flag");
    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json",
                                    "boot-trend.json", 0u), 0, "rename to itself is a no-op");
    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json",
                                    "BOOT-TREND.json", 0u), 0, "a respelling succeeds");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "BOOT-TREND.json"), 1u,
                   "and takes the new spelling");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-trend.json"), 0u,
                   "leaving no entry with the old one");
    fx_close(&fx);
}

/* The publish idiom a second boot exercises: write a temp, replace the real file,
 * again and again, with the old chain freed each time. */
static void test_fat32_replace_repeatedly(void)
{
    struct fx_volume fx;
    uint32_t round;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-timeline.json"), 0,
                   "colliding neighbour created");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json"), 0,
                   "destination created");
    for (round = 0; round < 8u; round++) {
        uint32_t old_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json");
        uint32_t new_data;

        TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp"), 0,
                       "temp created");
        new_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");
        TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp",
                                        "boot-trend.json", VFS_RENAME_REPLACE_EXISTING), 0,
                       "replace succeeds");
        TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json"), new_data,
                       "the destination holds the new data");
        TEST_ASSERT_EQ(fat32_get_fat_entry(fx.vol, old_data), FAT32_FREE,
                       "the old chain was freed");
        TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-trend.json"), 1u,
                       "one bound destination");
        TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-trend.json.tmp"), 0u,
                       "no leftover temp");
    }
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "boot-timeline.json"), 1u,
                   "the neighbour survived every round");
    fx_close(&fx);
}

/* A read-only fsck reports a stale FAT mirror and leaves it on disk; only a
 * repair run rewrites FAT2. Before 2026-11-09 fix=0 rewrote FAT2 and still
 * reported the volume clean. */
static void test_fat32_fsck_readonly_reports_fat_mirror(void)
{
    struct fx_volume fx;
    uint8_t *fat2;
    uint8_t saved;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }
    TEST_ASSERT(fx.vol->bpb.num_fats >= 2u, "the fixture keeps a FAT mirror");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 0), 0, "a fresh volume is clean");

    fat2 = fx.disk + ((uint64_t)fx.vol->bpb.first_fat_sector + fx.vol->bpb.fat_size_sectors) * 512u;
    saved = fat2[8];
    fat2[8] = (uint8_t)(saved ^ 0x5Au);
    TEST_ASSERT(fat32_fsck(fx.vol, 0) != 0, "a read-only fsck reports the stale mirror");
    TEST_ASSERT_EQ(fat2[8], (uint8_t)(saved ^ 0x5Au), "and does not write it");
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 1), 0, "a repair run fixes it");
    TEST_ASSERT_EQ(fat2[8], saved, "FAT2 now matches FAT1");
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 0), 0, "and the volume reads clean again");
    fx_close(&fx);
}

/* An unreadable FAT sector truncates every chain through it, so a repair run
 * must stop before its lost-cluster pass frees what it could not see. */
static void test_fat32_fsck_unreadable_fat_repairs_nothing(void)
{
    TEST_KLOG_EXPECT("fsck", "FAT unreadable; refusing to walk or repair");
    struct fx_volume fx;
    uint64_t fat_off, fat_len, i;
    uint8_t *fat1, *fat2;
    uint32_t sum_before = 0, sum_after = 0;
    const uint32_t lost = 100u;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);
    fat_off = (uint64_t)fx.vol->bpb.first_fat_sector * 512u;
    fat_len = (uint64_t)fx.vol->bpb.fat_size_sectors * 512u * fx.vol->bpb.num_fats;
    TEST_ASSERT(fx.vol->bpb.fat_size_sectors >= 2u, "the FAT spans more than one sector");
    fat1 = fx.disk + fat_off;
    fat2 = fat1 + (uint64_t)fx.vol->bpb.fat_size_sectors * 512u;

    /* A lost cluster in FAT sector 0, in both copies, which a repair would free. */
    *(uint32_t *)(fat1 + lost * 4u) = FAT32_EOC;
    *(uint32_t *)(fat2 + lost * 4u) = FAT32_EOC;
    for (i = 0; i < fat_len; i++) sum_before = sum_before * 31u + fat1[i];

    fx.fail_read_lba = fx.vol->bpb.first_fat_sector + 1u;
    TEST_ASSERT(fat32_fsck(fx.vol, 1) != 0, "fsck reports the unreadable FAT");
    fx.fail_read_lba = FX_NO_FAULT;
    scache_flush(fx.vol);
    for (i = 0; i < fat_len; i++) sum_after = sum_after * 31u + fat1[i];
    TEST_ASSERT_EQ(sum_after, sum_before, "and writes nothing to either FAT");
    TEST_ASSERT_EQ(*(uint32_t *)(fat1 + lost * 4u), FAT32_EOC, "the lost cluster is untouched");
    fx_close(&fx);
}

/* A FAT sector that passes the mirror compare and then fails during the walk
 * must not turn a live chain's tail into lost clusters. Chain: c -> 200 -> 100,
 * with entry 200 in FAT sector 1 and the tail's entry in readable sector 0, so
 * the lost-cluster scan reaches the tail before it reaches the bad sector. */
static void test_fat32_fsck_walk_read_failure_keeps_live_tail(void)
{
    TEST_KLOG_EXPECT("fsck", "FAT entry for cluster 200 unreadable");
    TEST_KLOG_EXPECT("fsck", "directory traversal incomplete; SKIPPING lost-cluster freeing pass");
    struct fx_volume fx;
    uint8_t *fat1, *fat2;
    uint32_t c, f;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "live.dat"), 0, "file created");
    c = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "live.dat");
    TEST_ASSERT(c >= 2u && c < 100u, "its first cluster is in FAT sector 0, below the tail");
    TEST_ASSERT(fx.vol->bpb.fat_size_sectors >= 2u, "the FAT spans two sectors");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);

    fat1 = fx.disk + (uint64_t)fx.vol->bpb.first_fat_sector * 512u;
    for (f = 0; f < fx.vol->bpb.num_fats; f++) {
        fat2 = fat1 + (uint64_t)f * fx.vol->bpb.fat_size_sectors * 512u;
        *(uint32_t *)(fat2 + c * 4u) = 200u;
        *(uint32_t *)(fat2 + 200u * 4u) = 100u;
        *(uint32_t *)(fat2 + 100u * 4u) = 0x0FFFFFFFu;
    }
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 0), 0, "the hand-built chain is consistent");
    scache_invalidate(fx.vol);

    /* The compare's read of FAT sector 1 passes; every later read fails. */
    fx.fail_read_lba = fx.vol->bpb.first_fat_sector + 1u;
    fx.fail_read_skip = 1u;
    TEST_ASSERT(fat32_fsck(fx.vol, 1) != 0, "fsck reports the incomplete walk");
    fx.fail_read_lba = FX_NO_FAULT;
    fx.fail_read_skip = 0u;
    scache_flush(fx.vol);
    TEST_ASSERT_EQ(*(uint32_t *)(fat1 + 100u * 4u), 0x0FFFFFFFu, "the live tail stays allocated");
    TEST_ASSERT_EQ(*(uint32_t *)(fat1 + 200u * 4u), 100u, "and so does its link");
    fx_close(&fx);
}

/* The BlackBox audit rotation (`audit_append_file()` in boot_audit.c): the
 * current history.jsonl replaces the previous generation history.1.jsonl, whose
 * two-slot long name begins in the directory's first cluster and ends in its
 * second. One generation must survive every rotation, with the old data freed. */
static void test_fat32_audit_rotation_replaces_generation_across_clusters(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc loc;
    uint32_t second, round;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES - 2u);
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "history.1.jsonl"), 0,
                   "previous generation created");
    second = fat32_get_fat_entry(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT(second >= 2u && second < FAT32_EOC, "the directory spans two clusters");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "history.1.jsonl", &loc), 1,
                   "previous generation found");
    TEST_ASSERT(loc.run_cluster == FX_ROOT_CLUSTER && loc.cluster == second && loc.run_entries == 3u,
                "its long-name run starts in the first cluster and ends in the second");

    for (round = 0; round < 4u; round++) {
        uint32_t old_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "history.1.jsonl");
        uint32_t cur_data;

        TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "history.jsonl"), 0,
                       "current history created");
        cur_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "history.jsonl");
        TEST_ASSERT(cur_data != 0u && old_data != 0u && cur_data != old_data,
                    "both generations own distinct data");
        scache_flush(fx.vol);
        scache_invalidate(fx.vol);
        TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "history.jsonl", "history.1.jsonl",
                                        VFS_RENAME_REPLACE_EXISTING), 0,
                       "the rotation rename succeeds");
        TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "history.1.jsonl"), cur_data,
                       "the previous generation now holds the rotated data");
        TEST_ASSERT_EQ(fat32_get_fat_entry(fx.vol, old_data), FAT32_FREE,
                       "the replaced generation's data was freed");
        TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "history.1.jsonl"), 1u,
                       "exactly one previous generation");
        TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "history.jsonl"), 0u,
                       "the current file is gone until the next append creates it");
    }
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 0), 0, "a read-only fsck finds no errors");
    fx_close(&fx);
}

/* Crash at every device write of a rename, then look at what a fresh mount of
 * that disk would see. `replace` picks the replace path. */
static void fx_rename_crash_sweep(int replace)
{
    uint32_t crash_at = 0;
    uint32_t total = FX_NO_CRASH;

    while (total == FX_NO_CRASH || crash_at <= total) {
        struct fx_volume fx, after;
        uint32_t old_data = 0, new_data;
        uint32_t seen_new, seen_tmp;

        TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
        TEST_ASSERT_EQ(fx_open(&after), 0, "second view allocated");
        if (!fx.vol || !fx.disk || !after.vol || !after.disk) {
            fx_close(&fx);
            fx_close(&after);
            return;
        }

        /* Pad so runs cross a cluster boundary, as they do on the BlackBox. */
        fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES - 2u);
        if (replace) {
            fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json");
            old_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json");
        }
        fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");
        new_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");
        scache_flush(fx.vol);

        fx.writes = 0;
        if (total != FX_NO_CRASH)
            fx.crash_after = crash_at;
        (void)fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp",
                               "boot-trend.json",
                               replace ? VFS_RENAME_REPLACE_EXISTING : 0u);
        if (total == FX_NO_CRASH) {
            total = fx.writes;
            TEST_ASSERT(total > 0u, "the rename reached the device");
        }

        memcpy(after.disk, fx.disk, FX_SECTORS * 512u);
        seen_new = fx_cluster_of(after.vol, FX_ROOT_CLUSTER, "boot-trend.json");
        seen_tmp = fx_cluster_of(after.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");

        if (replace) {
            TEST_ASSERT(seen_new == old_data || seen_new == new_data,
                        "the destination is the old file or the new one at every crash point");
            TEST_ASSERT(seen_new != 0u &&
                        fat32_get_fat_entry(after.vol, seen_new) != FAT32_FREE,
                        "and its data was not returned to the free pool");
            TEST_ASSERT(seen_new != seen_tmp, "two names never own one chain");
        } else {
            TEST_ASSERT(seen_new == new_data || seen_tmp == new_data,
                        "the file has a name at every crash point");
            /* The dirty mount after a crash must leave one owner. */
            TEST_ASSERT_EQ(fat32_fsck(after.vol, 1), 0, "fsck repairs the volume");
            seen_new = fx_cluster_of(after.vol, FX_ROOT_CLUSTER, "boot-trend.json");
            seen_tmp = fx_cluster_of(after.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");
            TEST_ASSERT((seen_new == new_data) != (seen_tmp == new_data),
                        "after fsck exactly one name owns the file");
            TEST_ASSERT(fat32_get_fat_entry(after.vol, new_data) != FAT32_FREE,
                        "and its data is still allocated");
        }

        fx_close(&fx);
        fx_close(&after);
        crash_at++;
    }
}

/* Creating a name that exists must not add a second entry: unconditional callers
 * grew X:\\Diag by one hwdump.txt on every boot. */
static void test_fat32_create_refuses_existing_name(void)
{
    struct fx_volume fx;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "hwdump.txt"), 0,
                   "file created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "hwdump.txt");
    TEST_ASSERT(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "hwdump.txt") != 0,
                "creating it again is refused");
    TEST_ASSERT(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "HWDUMP.TXT") != 0,
                "whatever the case");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "hwdump.txt"), 1u,
                   "one entry remains");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "hwdump.txt"), data,
                   "still holding its data");
    fx_close(&fx);
}

/* A rename that fails before the new entries are on the device must not leave two
 * live names: truncating the source would free the chain the other still holds. */
static void test_fat32_failed_rename_rolls_back(void)
{
    struct fx_volume fx;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp"), 0,
                   "source created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp");
    scache_flush(fx.vol);
    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);

    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp",
                                 "report.json", 0u) != 0, "rename reports the failed write");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json"), 0u,
                   "the new name is gone");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp"), data,
                   "the source still owns the file");

    fx.fail_write_lba = FX_NO_FAULT;
    TEST_ASSERT_EQ(fat32_delete_file_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp"), 0,
                   "the source can be deleted once the device recovers");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "report.json"), 0u,
                   "and no name was left holding the freed chain");
    fx_close(&fx);
}

/* Rollback must remove the entry it wrote, whatever else its names resolve to. In
 * the first case the published alias is another file's long name; in the second the
 * new name is a respelling that also resolves to the source. */
static void fx_rollback_case(const char *neighbour, const char *source,
                             const char *target)
{
    struct fx_volume fx;
    uint32_t data, i;
    uint32_t live = 0;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    if (neighbour)
        TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, neighbour), 0,
                       "neighbour created");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, source), 0, "source created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, source);
    scache_flush(fx.vol);
    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);

    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, source, target, 0u) != 0,
                "rename reports the failed write");

    /* Count live short entries holding the chain, in the view every lookup uses. */
    {
        uint8_t sector_buf[512];

        TEST_ASSERT_EQ(fat32_read_sector(fx.vol, cluster_to_sector(fx.vol, FX_ROOT_CLUSTER),
                                         sector_buf), 0, "directory read");
        for (i = 0; i < FX_DIR_ENTRIES; i++) {
            const struct fat32_dir_entry *de =
                (const struct fat32_dir_entry *)&sector_buf[i * FX_ENTRY_SIZE];

            if (de->name[0] != 0x00 && de->name[0] != 0xE5 &&
                de->attr != FAT32_ATTR_LFN &&
                (((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo) == data)
                live++;
        }
    }
    TEST_ASSERT_EQ(live, 1u, "exactly one entry owns the chain after the failed rename");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, source), data,
                   "and it is the source");
    fx.fail_write_lba = FX_NO_FAULT;
    fx_close(&fx);
}

static void test_fat32_rollback_alias_is_a_long_name(void)
{
    fx_rollback_case("report~2.jso", "report.json.tmp", "report.json");
}

static void test_fat32_rollback_respelling(void)
{
    fx_rollback_case((const char *)0, "report.txt", "REPORT.TXT");
}

/* Dot entries belong to the directory's structure and are nothing to act on by
 * name: removing one frees a cluster the parent still points at. */
static void test_fat32_dot_names_are_refused(void)
{
    struct fx_volume fx;
    uint32_t sub;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "SUB"), 0, "subdirectory created");
    sub = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "SUB");

    TEST_ASSERT(fat32_rmdir(fx.vol, sub, ".") != 0, "rmdir refuses the dot entry");
    TEST_ASSERT(fat32_rmdir(fx.vol, sub, "..") != 0, "rmdir refuses the dotdot entry");
    TEST_ASSERT(fat32_rename_vol(fx.vol, sub, ".", "HERE", 0u) != 0, "rename refuses dot as source");
    TEST_ASSERT(fat32_rename_vol(fx.vol, sub, "..", "UP", 0u) != 0, "rename refuses dotdot as source");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, sub, "FILE.DAT"), 0, "file created");
    TEST_ASSERT(fat32_rename_vol(fx.vol, sub, "FILE.DAT", ".",
                                 VFS_RENAME_REPLACE_EXISTING) != 0, "rename refuses dot as target");
    TEST_ASSERT(fat32_create_file_vol(fx.vol, sub, "..") != 0, "create refuses dotdot");

    TEST_ASSERT_EQ(fx_entry(&fx, sub, 0u)->name[0], (uint8_t)'.', "the dot entry is intact");
    TEST_ASSERT_EQ(fx_entry(&fx, sub, 1u)->name[1], (uint8_t)'.', "the dotdot entry is intact");
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, sub) != FAT32_FREE, "the directory keeps its cluster");
    fx_close(&fx);
}

/* If fsck cannot tell whether a shared chain is a duplicate, it must not report the
 * volume clean: it would then mount writable with two owners of one chain. */
static void test_fat32_fsck_owner_scan_failure_is_not_clean(void)
{
    TEST_KLOG_EXPECT("fsck", "directory traversal incomplete; SKIPPING lost-cluster freeing pass");
    struct fx_volume fx;
    struct fat32_dirent_loc first, second;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "first-long-name.dat"), 0,
                   "first name created");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "second-long-name.dat"), 0,
                   "second name created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "first-long-name.dat", &first), 1,
                   "first found");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "second-long-name.dat", &second), 1,
                   "second found");
    fat32_free_chain(fx.vol, fx_first_cluster(&second));
    second.de.first_cluster_hi = first.de.first_cluster_hi;
    second.de.first_cluster_lo = first.de.first_cluster_lo;
    TEST_ASSERT_EQ(fat32_dir_rewrite_entry(fx.vol, &second), 0, "second name shares the chain");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);

    /* The walk's own read of the directory succeeds; the owner scan's does not. */
    fx.fail_read_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    fx.fail_read_skip = 1u;
    TEST_ASSERT(fat32_fsck(fx.vol, 1) != 0, "fsck does not report the volume clean");
    fx.fail_read_lba = FX_NO_FAULT;
    fx_close(&fx);
}

/* A failed FAT read during the owner scan must not read as a clean end of chain. */
static void test_fat32_owner_scan_fat_failure(void)
{
    struct fx_volume fx;
    uint32_t next;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES + 1u);
    next = fat32_get_fat_entry(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT(next >= 2u && next < FAT32_EOC, "the directory spans two clusters");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);
    fx.fail_read_lba = FX_RESERVED;

    TEST_ASSERT_EQ(fat32_fsck_earlier_owner(fx.vol, FX_ROOT_CLUSTER, next, 0u, 0x7FFFu, 0u,
                                            FX_SECTORS), -1,
                   "the scan reports it could not tell");
    fx.fail_read_lba = FX_NO_FAULT;
    TEST_ASSERT_EQ(fat32_fsck_earlier_owner(fx.vol, FX_ROOT_CLUSTER, next, 0u, 0x7FFFu, 0u,
                                            FX_SECTORS), 0,
                   "and answers no once the read works");
    fx_close(&fx);
}

/* A file and a directory on one chain is corruption, not a rename. Dropping the
 * directory would hand its children to the lost-cluster pass. */
static void test_fat32_fsck_file_directory_conflict(void)
{
    TEST_KLOG_EXPECT("fsck", "directory traversal incomplete; SKIPPING lost-cluster freeing pass");
    struct fx_volume fx;
    struct fat32_dirent_loc file, dir;
    uint32_t child, dir_cluster;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "FIRST.DAT"), 0, "file created");
    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "SECOND"), 0, "directory created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "FIRST.DAT", &file), 1, "file found");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "SECOND", &dir), 1, "directory found");
    dir_cluster = fx_first_cluster(&dir);
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, dir_cluster, "CHILD.DAT"), 0, "child created");
    child = fx_cluster_of(fx.vol, dir_cluster, "CHILD.DAT");
    fat32_free_chain(fx.vol, fx_first_cluster(&file));
    file.de.first_cluster_hi = dir.de.first_cluster_hi;
    file.de.first_cluster_lo = dir.de.first_cluster_lo;
    TEST_ASSERT_EQ(fat32_dir_rewrite_entry(fx.vol, &file), 0, "file shares the directory's chain");

    TEST_ASSERT(fat32_fsck(fx.vol, 1) != 0, "fsck does not call this clean");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "SECOND"), dir_cluster,
                   "the directory keeps its name");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, dir_cluster, "CHILD.DAT"), child, "the child keeps its entry");
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, child) != FAT32_FREE, "and its clusters");
    fx_close(&fx);
}

/* A cold destination sector that cannot be read must stop the replace before the
 * source goes, or the file loses its only name. */
static void test_fat32_replace_cold_destination_read_failure(void)
{
    struct fx_volume fx;
    uint32_t src_data, dst_data, next;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp"), 0,
                   "source created in the first cluster");
    fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES - 3u);
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "report.json"), 0,
                   "destination created in the second cluster");
    src_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp");
    dst_data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json");
    next = fat32_get_fat_entry(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT(next >= 2u && next < FAT32_EOC, "the directory spans two clusters");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);

    /* The destination lookup's read succeeds; the next read of that sector fails. */
    fx.fail_read_lba = cluster_to_sector(fx.vol, next);
    fx.fail_read_skip = 1u;
    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp", "report.json",
                                 VFS_RENAME_REPLACE_EXISTING) != 0,
                "the replace reports the failed read");
    fx.fail_read_lba = FX_NO_FAULT;
    fx.fail_read_skip = 0u;
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp"), src_data,
                   "the source keeps its long name and its data");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json"), dst_data,
                   "the destination is unchanged");
    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp", "report.json",
                                    VFS_RENAME_REPLACE_EXISTING), 0,
                   "and a retry succeeds");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json"), src_data,
                   "publishing the new data");
    fx_close(&fx);
}

/* Build: PADs, then a source run whose slots end one cluster and whose short entry
 * opens the next, then a destination after it. */
static void fx_source_across_clusters(struct fx_volume *fx, uint32_t *src_data,
                                      uint32_t *dst_data, uint32_t *second)
{
    fx_pad_short(fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES - 2u);
    fat32_create_file_vol(fx->vol, FX_ROOT_CLUSTER, "report.json.tmp");
    fat32_create_file_vol(fx->vol, FX_ROOT_CLUSTER, "report.json");
    *src_data = fx_cluster_of(fx->vol, FX_ROOT_CLUSTER, "report.json.tmp");
    *dst_data = fx_cluster_of(fx->vol, FX_ROOT_CLUSTER, "report.json");
    *second = fat32_get_fat_entry(fx->vol, FX_ROOT_CLUSTER);
    scache_flush(fx->vol);
    scache_invalidate(fx->vol);
}

/* Once the source's short entry is gone it has no name, so a failed slot cleanup
 * must not stop the switch that gives its data one. */
static void test_fat32_replace_survives_slot_cleanup_failure(void)
{
    struct fx_volume fx;
    uint32_t src_data, dst_data, second;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_source_across_clusters(&fx, &src_data, &dst_data, &second);
    TEST_ASSERT(second >= 2u && second < FAT32_EOC, "the directory spans two clusters");

    /* Two lookups read the slot sector; the cleanup's read of it fails. */
    fx.fail_read_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    fx.fail_read_skip = 2u;
    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp", "report.json",
                                    VFS_RENAME_REPLACE_EXISTING), 0,
                   "the replace completes");
    fx.fail_read_lba = FX_NO_FAULT;
    fx.fail_read_skip = 0u;
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json"), src_data,
                   "the destination holds the new data");
    fx_close(&fx);
}

/* If the source's removal cannot reach the device, a returned error must leave the
 * source named, or a retry has nothing to find. */
/* A truncate whose directory write fails must leave the file as it was: the old
 * entry over its still-allocated chain, never an entry pointing at clusters the
 * free pool can hand to another file. The FAT sectors stay writable, which is
 * the case where freeing first used to corrupt. */
static void test_fat32_truncate_dir_write_failure_keeps_chain(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc loc;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "evidence.json"), 0,
                   "file created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "evidence.json");
    TEST_ASSERT(data >= 2u, "the file owns a data cluster");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);

    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT(fat32_truncate(fx.vol, FX_ROOT_CLUSTER, "evidence.json", 0u) != 0,
                "the truncate reports the failed directory write");
    TEST_ASSERT_EQ(fat32_get_fat_entry(fx.vol, data) != 0u, 1,
                   "the data cluster is still allocated");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "evidence.json"), data,
                   "the cached entry still names its chain");

    fx.fail_write_lba = FX_NO_FAULT;
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "evidence.json"), data,
                   "on disk too, the entry still names its chain");
    TEST_ASSERT_EQ(fat32_get_fat_entry(fx.vol, data) != 0u, 1,
                   "and on disk the cluster is not free");

    TEST_ASSERT_EQ(fat32_truncate(fx.vol, FX_ROOT_CLUSTER, "evidence.json", 0u), 0,
                   "a retry succeeds");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "evidence.json", &loc), 1,
                   "the file remains");
    TEST_ASSERT_EQ(fx_first_cluster(&loc), 0u, "with no chain");
    TEST_ASSERT_EQ(loc.de.file_size, 0u, "and no bytes");
    TEST_ASSERT_EQ(fat32_get_fat_entry(fx.vol, data), 0u, "its old cluster is free");
    fx_close(&fx);
}

/* A shrink cuts the chain in the cache, and the cache flushes in slot order, so
 * with the retained cluster and the tail in different FAT sectors the tail could
 * reach the disk free while the retained cluster still links to it. The cut must
 * land first; when it cannot, the tail stays allocated. */
static void test_fat32_shrink_keeps_tail_until_cut_lands(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc loc;
    uint32_t head;
    const uint32_t *fat1;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "two.bin"), 0,
                   "file created");
    head = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "two.bin");
    TEST_ASSERT(head >= 2u && head / FAT32_ENTRIES_PER_SECTOR !=
                FX_FAR_CLUSTER / FAT32_ENTRIES_PER_SECTOR,
                "head and tail clusters sit in different FAT sectors");
    TEST_ASSERT_EQ(fat32_set_fat_entry(fx.vol, head, FX_FAR_CLUSTER), 0, "head links to tail");
    TEST_ASSERT_EQ(fat32_set_fat_entry(fx.vol, FX_FAR_CLUSTER, 0x0FFFFFFFu), 0, "tail ends");
    TEST_ASSERT_EQ(fat32_truncate(fx.vol, FX_ROOT_CLUSTER, "two.bin", 1024u), 0,
                   "the entry records two clusters");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);

    /* The FAT1 sector holding the head's entry refuses writes; the tail's is fine. */
    fx.fail_write_lba = FX_RESERVED + head / FAT32_ENTRIES_PER_SECTOR;
    TEST_ASSERT_EQ(fat32_truncate(fx.vol, FX_ROOT_CLUSTER, "two.bin", 512u), 0,
                   "the new size is committed");
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, FX_FAR_CLUSTER) != 0u,
                "the tail is not freed while the cut is not on the device");
    scache_flush(fx.vol);
    fat1 = (const uint32_t *)(fx.disk + FX_RESERVED * 512u);
    TEST_ASSERT_EQ(fat1[head] & 0x0FFFFFFFu, FX_FAR_CLUSTER, "on disk the head still links");
    TEST_ASSERT(fat1[FX_FAR_CLUSTER] != 0u, "and on disk the tail it links to is allocated");

    fx.fail_write_lba = FX_NO_FAULT;
    scache_flush(fx.vol);
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "two.bin", &loc), 1, "file remains");
    TEST_ASSERT_EQ(loc.de.file_size, 512u, "at its new size");
    TEST_ASSERT_EQ(fat1[head] & 0x0FFFFFFFu, 0x0FFFFFFFu, "the cut lands once the sector writes");
    fx_close(&fx);
}

static void test_fat32_replace_failed_removal_keeps_source(void)
{
    struct fx_volume fx;
    uint32_t src_data, dst_data, second;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_source_across_clusters(&fx, &src_data, &dst_data, &second);
    TEST_ASSERT(second >= 2u && second < FAT32_EOC, "the directory spans two clusters");

    fx.fail_write_lba = cluster_to_sector(fx.vol, second);
    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp", "report.json",
                                 VFS_RENAME_REPLACE_EXISTING) != 0,
                "the replace reports the failed write");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp"), src_data,
                   "the source keeps its name and data");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json"), dst_data,
                   "the destination is unchanged");
    fx.fail_write_lba = FX_NO_FAULT;
    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp", "report.json",
                                    VFS_RENAME_REPLACE_EXISTING), 0, "a retry succeeds");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "report.json"), src_data,
                   "publishing the new data");
    fx_close(&fx);
}

/* A crash must never publish a short entry ahead of its long-name slots: with the
 * new run in a hole before the source, fsck would keep that bare alias and drop the
 * source, leaving the file under neither requested name. */
static void test_fat32_rename_into_hole_crash_sweep(void)
{
    uint32_t crash_at = 0;
    uint32_t total = FX_NO_CRASH;

    while (total == FX_NO_CRASH || crash_at <= total) {
        struct fx_volume fx, after;
        uint32_t data, seen_new, seen_tmp;

        TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
        TEST_ASSERT_EQ(fx_open(&after), 0, "second view allocated");
        if (!fx.vol || !fx.disk || !after.vol || !after.disk) {
            fx_close(&fx);
            fx_close(&after);
            return;
        }

        /* A deleted run spanning the cluster boundary, then the source after it. */
        fx_pad_short(&fx, FX_ROOT_CLUSTER, FX_DIR_ENTRIES - 2u);
        fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "hole-maker.json");
        fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");
        fat32_delete_file_vol(fx.vol, FX_ROOT_CLUSTER, "hole-maker.json");
        data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");
        scache_flush(fx.vol);
        scache_invalidate(fx.vol);
        /* Cache the short entry's sector first, so a slot-order flush writes it
         * first. Lookups read around the cache, so this has to be a direct read. */
        {
            uint8_t sector_buf[512];
            uint32_t second = fat32_get_fat_entry(fx.vol, FX_ROOT_CLUSTER);

            scache_invalidate(fx.vol);
            TEST_ASSERT_EQ(fat32_read_sector(fx.vol, cluster_to_sector(fx.vol, second),
                                             sector_buf), 0, "short entry's sector cached");
        }

        fx.writes = 0;
        if (total != FX_NO_CRASH)
            fx.crash_after = crash_at;
        (void)fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp",
                               "boot-trend.json", 0u);
        if (total == FX_NO_CRASH)
            total = fx.writes;

        memcpy(after.disk, fx.disk, FX_SECTORS * 512u);
        TEST_ASSERT_EQ(fat32_fsck(after.vol, 1), 0, "fsck repairs the volume");
        seen_new = fx_cluster_of(after.vol, FX_ROOT_CLUSTER, "boot-trend.json");
        seen_tmp = fx_cluster_of(after.vol, FX_ROOT_CLUSTER, "boot-trend.json.tmp");
        TEST_ASSERT((seen_new == data) != (seen_tmp == data),
                    "exactly one requested name owns the file at every crash point");

        fx_close(&fx);
        fx_close(&after);
        crash_at++;
    }
}

/* A slot cleanup that failed leaves a whole run in front of the freed short entry. A
 * new short entry there with a colliding checksum must not inherit the long name. */
static void test_fat32_orphan_run_after_cleanup_failure_stays_dead(void)
{
    struct fx_volume fx;
    uint32_t src_data, dst_data, second;
    struct fat32_dirent_loc loc;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    fx_source_across_clusters(&fx, &src_data, &dst_data, &second);
    fx.fail_read_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    fx.fail_read_skip = 2u;
    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp", "report.json",
                                    VFS_RENAME_REPLACE_EXISTING), 0, "the replace completes");
    fx.fail_read_lba = FX_NO_FAULT;
    fx.fail_read_skip = 0u;

    /* NEWB16.TXT and REPORT~1.TMP share LFN checksum 33. */
    {
        uint8_t a[11], b[11];

        memcpy(a, "NEWB16  TXT", 11);
        memcpy(b, "REPORT~1TMP", 11);
        TEST_ASSERT_EQ(fat32_lfn_checksum(a), fat32_lfn_checksum(b), "the checksums collide");
    }
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "NEWB16.TXT"), 0, "file created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "report.json.tmp", &loc), 0,
                   "the removed name stays gone");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "NEWB16.TXT", &loc), 1,
                   "and the new file answers to its own name");
    fx_close(&fx);
}

/* A create whose flush failed leaves its slots behind. They must not name the next
 * file that lands after them. */
static void test_fat32_orphan_run_after_failed_publish_stays_dead(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc loc;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    scache_flush(fx.vol);
    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "example.txt") != 0,
                "the create reports the failed write");
    fx.fail_write_lba = FX_NO_FAULT;

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "EXAMPL~1.TXT"), 0,
                   "a file with the matching short name is created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "example.txt", &loc), 0,
                   "the abandoned long name stays absent");
    fx_close(&fx);
}

/* A name that spells differently but reduces to a short name in use must be refused:
 * two entries with one short name make every lookup of it a coin toss. */
static void test_fat32_short_name_collision_refused(void)
{
    struct fx_volume fx;
    uint32_t foo, bar;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "FOO"), 0, "FOO created");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "BAR"), 0, "BAR created");
    foo = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "FOO");
    bar = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "BAR");

    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "BAR", "FOO.",
                                 VFS_RENAME_REPLACE_EXISTING) != 0,
                "rename onto the same short name is refused");
    TEST_ASSERT(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "FOO.") != 0,
                "and so is create");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "FOO"), foo, "FOO keeps its data");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "BAR"), bar, "BAR keeps its data");
    fx_close(&fx);
}

/* Slot numbers come from the disk. No sequence value may write outside the buffer. */
static void test_fat32_lfn_extract_stays_in_bounds(void)
{
    struct {
        char before[16];
        char name[FAT32_MAX_NAME];
        char after[16];
    } guard;
    struct fat32_lfn_entry lfn;
    uint32_t i;
    int bad = 0;

    memset(&guard, 0x5A, sizeof(guard));
    memset(&lfn, 0, sizeof(lfn));
    for (i = 0; i < 5u; i++) lfn.name1[i] = 'x';
    for (i = 0; i < 6u; i++) lfn.name2[i] = 'x';
    for (i = 0; i < 2u; i++) lfn.name3[i] = 'x';

    lfn_extract_chars(&lfn, guard.name, 19);    /* highest slot lookups accept */
    lfn_extract_chars(&lfn, guard.name, 30);    /* highest the 5-bit field encodes */
    lfn_extract_chars(&lfn, guard.name, -1);    /* a zero sequence number */
    for (i = 0; i < 16u; i++) {
        if (guard.before[i] != 0x5A || guard.after[i] != 0x5A)
            bad++;
    }
    TEST_ASSERT_EQ(bad, 0, "nothing is written outside the name buffer");
    TEST_ASSERT_EQ(guard.name[FAT32_MAX_NAME - 1], 0x5A, "and the terminator slot is left alone");
}

/* FAT caps a long name at 255 characters. */
static void test_fat32_long_name_limit(void)
{
    struct fx_volume fx;
    char name[257];
    uint32_t i;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    for (i = 0; i < 256u; i++)
        name[i] = (char)('a' + (i % 26u));
    name[256] = '\0';
    TEST_ASSERT(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, name) != 0,
                "a 256-character name is refused");
    name[255] = '\0';
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, name), 0,
                   "a 255-character name is accepted");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, name), 1u,
                   "and reads back whole");
    TEST_ASSERT(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, name) >= 2u, "and resolves");
    fx_close(&fx);
}

/* A file's own short alias is a name it can be renamed to: that drops the long name. */
static void test_fat32_rename_to_own_alias(void)
{
    struct fx_volume fx;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "example.txt"), 0, "file created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "example.txt");
    TEST_ASSERT_EQ(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "example.txt", "EXAMPL~1.TXT", 0u), 0,
                   "rename to the file's own alias succeeds");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "EXAMPL~1.TXT"), data, "keeping its data");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "example.txt"), 0u,
                   "and the long name is gone");
    TEST_ASSERT_EQ(fx_entry(&fx, FX_ROOT_CLUSTER, 0u)->name[0], (uint8_t)0xE5,
                   "with its slot deleted");
    fx_close(&fx);
}

/* Removing the slots IS that rename, so its failure is the rename's failure. */
static void test_fat32_rename_to_own_alias_reports_failure(void)
{
    struct fx_volume fx;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "example.txt"), 0, "file created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "example.txt");
    scache_flush(fx.vol);
    scache_invalidate(fx.vol);

    /* Both lookups read the directory; the slot removal's read of it fails. */
    fx.fail_read_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    fx.fail_read_skip = 2u;
    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "example.txt", "EXAMPL~1.TXT", 0u) != 0,
                "the rename reports the failed read");
    fx.fail_read_lba = FX_NO_FAULT;
    fx.fail_read_skip = 0u;
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "example.txt"), data,
                   "the file keeps its long name");

    /* A failed flush of that removal is reported too. */
    scache_flush(fx.vol);
    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT(fat32_rename_vol(fx.vol, FX_ROOT_CLUSTER, "example.txt", "EXAMPL~1.TXT", 0u) != 0,
                "the rename reports the failed write");
    fx.fail_write_lba = FX_NO_FAULT;
    fx_close(&fx);
}

/* An interrupted rename of a populated directory leaves it under two names. The
 * repair keeps one name and must not treat the children as duplicates. */
static void test_fat32_fsck_duplicate_directory_keeps_children(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc first, second;
    uint32_t dir;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "first-directory"), 0,
                   "first name created");
    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "second-directory"), 0,
                   "second name created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "first-directory", &first), 1,
                   "first found");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "second-directory", &second), 1,
                   "second found");
    dir = fx_first_cluster(&first);
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, dir, "child-file.dat"), 0, "child created");
    fat32_free_chain(fx.vol, fx_first_cluster(&second));
    second.de.first_cluster_hi = first.de.first_cluster_hi;
    second.de.first_cluster_lo = first.de.first_cluster_lo;
    TEST_ASSERT_EQ(fat32_dir_rewrite_entry(fx.vol, &second), 0, "second name shares the chain");

    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 1), 0, "fsck repairs it");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "second-directory"), 0u,
                   "the second name is gone");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, dir, "child-file.dat"), 1u,
                   "the child keeps its entry");
    TEST_ASSERT(fx_cluster_of(fx.vol, dir, "child-file.dat") >= 2u, "and its data");
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 0), 0, "a second pass finds nothing");
    fx_close(&fx);
}

/* The same chain from two different directories is real corruption, not a rename:
 * nothing says which name is right, so the old report-only behaviour stands. */
static void test_fat32_fsck_keeps_names_across_directories(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc in_a, in_b;
    uint32_t a, b;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "AAA"), 0, "first directory");
    TEST_ASSERT_EQ(fat32_create_dir_vol(fx.vol, FX_ROOT_CLUSTER, "BBB"), 0, "second directory");
    a = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "AAA");
    b = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "BBB");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, a, "ONE.DAT"), 0, "file in first");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, b, "TWO.DAT"), 0, "file in second");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, a, "ONE.DAT", &in_a), 1, "first found");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, b, "TWO.DAT", &in_b), 1, "second found");
    fat32_free_chain(fx.vol, fx_first_cluster(&in_b));
    in_b.de.first_cluster_hi = in_a.de.first_cluster_hi;
    in_b.de.first_cluster_lo = in_a.de.first_cluster_lo;
    TEST_ASSERT_EQ(fat32_dir_rewrite_entry(fx.vol, &in_b), 0, "chain shared across directories");

    (void)fat32_fsck(fx.vol, 1);
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, a, "ONE.DAT", &in_a), 1, "the first name stays");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, b, "TWO.DAT", &in_b), 1, "and so does the second");
    fx_close(&fx);
}

/* A known instant stored through set_times decodes back to that instant. */
static void test_fat32_set_times_stores_the_date_given(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc loc;
    filetime_t when;
    uint16_t before_date;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "DATED.DAT"), 0,
                   "file created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "DATED.DAT", &loc), 1, "file found");
    before_date = loc.de.create_date;

    when.seconds = 961070400u;      /* 2000-06-15 12:00:00 UTC */
    TEST_ASSERT_EQ(fat32_set_times(fx.vol, FX_ROOT_CLUSTER, "DATED.DAT",
                                   (filetime_t *)0, &when, (filetime_t *)0), 0, "set_times");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "DATED.DAT", &loc), 1, "file found");
    TEST_ASSERT_EQ((loc.de.modify_date >> 9) + 1980u, 2000u, "the year is the one given");
    TEST_ASSERT_EQ((loc.de.modify_date >> 5) & 0xFu, 6u, "and the month");
    TEST_ASSERT_EQ(loc.de.modify_date & 0x1Fu, 15u, "and the day");
    TEST_ASSERT_EQ(loc.de.create_date, before_date, "a NULL time is left alone");
    fx_close(&fx);
}

static const filetime_t *s_seen_times[3];
static uint32_t s_times_calls;
static uint32_t s_attr_calls;

static int fx_stub_set_times(struct vfs_node *node, const filetime_t *c,
                             const filetime_t *m, const filetime_t *a)
{
    (void)node;
    s_seen_times[0] = c;
    s_seen_times[1] = m;
    s_seen_times[2] = a;
    s_times_calls++;
    return 0;
}

static int fx_stub_set_attr(struct vfs_node *node, uint32_t attr)
{
    (void)node;
    (void)attr;
    s_attr_calls++;
    return 0;
}

/* A file object as the driver builds one, from the entry on disk. */
static void fx_file_from_entry(struct fx_volume *fx, const char *name, struct fat32_file *f)
{
    struct fat32_dirent_loc loc;

    memset(f, 0, sizeof(*f));
    TEST_ASSERT_EQ(fat32_dir_lookup(fx->vol, FX_ROOT_CLUSTER, name, &loc), 1, "entry found");
    memcpy(f->sfn, loc.de.name, 11);
    f->first_cluster = fx_first_cluster(&loc);
    f->file_size = loc.de.file_size;
    f->dir_cluster = FX_ROOT_CLUSTER;
    f->volume = fx->vol;
    f->node.fs_data = f;
    f->node.size = f->file_size;
    f->node.ops = &fat32_file_ops;
    f->node.type = VFS_FILE;
    for (uint32_t i = 0; name[i] && i + 1u < sizeof(f->node.name); i++)
        f->node.name[i] = name[i];
}

/* Free clusters counted from the FAT itself, the oracle the allocator reads. */
static uint32_t fx_free_clusters(struct fx_volume *fx)
{
    uint32_t max = (fx->vol->bpb.total_sectors - fx->vol->bpb.first_data_sector)
                   / fx->vol->bpb.sectors_per_cluster + 2u;
    uint32_t n = 0;

    for (uint32_t c = 2; c < max; c++)
        if (fat32_get_fat_entry(fx->vol, c) == FAT32_FREE)
            n++;
    return n;
}

static uint32_t fx_size_on_disk(struct fx_volume *fx, const char *name)
{
    struct fat32_dirent_loc loc;

    scache_flush(fx->vol);
    return (fat32_dir_lookup(fx->vol, FX_ROOT_CLUSTER, name, &loc) == 1) ? loc.de.file_size : 0xFFFFFFFFu;
}

/* A write is only part of the file once the entry records its size. When that
 * directory write fails, the driver must not report the bytes past the size
 * the disk still holds (it used to report all of them). */
static void test_fat32_write_reports_only_recorded_bytes(void)
{
    struct fx_volume fx;
    struct fat32_file f;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "seq.txt"), 0, "file created");
    fx_file_from_entry(&fx, "seq.txt", &f);

    TEST_ASSERT_EQ(fat32_file_ops.write(&f.node, 0, 3, (const uint8_t *)"42\n"), 3,
                   "a healthy write reports every byte");
    TEST_ASSERT_EQ(fx_size_on_disk(&fx, "seq.txt"), 3u, "and the entry records them");

    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT_EQ(fat32_file_ops.write(&f.node, 0, 5, (const uint8_t *)"4343\n"), 3,
                   "a failed size update reports only the bytes below the recorded size");
    TEST_ASSERT_EQ((uint32_t)f.node.size, 3u, "the in-memory size stays the disk's");
    TEST_ASSERT_EQ(f.file_size, 3u, "and so does the driver's");
    TEST_ASSERT_EQ(fat32_file_ops.write(&f.node, 3, 2, (const uint8_t *)"9\n"), 0,
                   "an append whose size cannot be recorded reports nothing");
    fx.fail_write_lba = FX_NO_FAULT;
    TEST_ASSERT_EQ(fx_size_on_disk(&fx, "seq.txt"), 3u, "the entry still records 3 bytes");
    fx_close(&fx);
}

/* The first write to a chainless file points its entry at a new cluster. If that
 * directory write fails, the write must fail. The cluster must stay allocated:
 * the entry may have landed, and a freed cluster the disk still references
 * would be handed to the next file that allocates. */
static void test_fat32_unlinked_first_cluster_fails_write(void)
{
    struct fx_volume fx;
    struct fat32_file f;
    uint32_t free_before;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "empty.txt"), 0, "file created");
    fx_file_from_entry(&fx, "empty.txt", &f);
    /* An open with VFS_O_TRUNC leaves the file with no chain; the next write
     * must allocate and link one. */
    TEST_ASSERT_EQ(fat32_file_ops.truncate(&f.node, 0), 0, "truncated to zero");
    fx_file_from_entry(&fx, "empty.txt", &f);
    TEST_ASSERT_EQ(f.first_cluster, 0u, "a truncated file has no chain");
    free_before = fx_free_clusters(&fx);

    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT_EQ(fat32_file_ops.write(&f.node, 0, 3, (const uint8_t *)"42\n"), -1,
                   "a write whose cluster cannot be linked fails");
    fx.fail_write_lba = FX_NO_FAULT;
    TEST_ASSERT_EQ(f.first_cluster, 0u, "the file object keeps no unlinked chain");
    TEST_ASSERT_EQ(fx_free_clusters(&fx), free_before - 1u,
                   "the cluster was not returned to the free pool");
    TEST_ASSERT_EQ(fat32_file_ops.write(&f.node, 0, 3, (const uint8_t *)"42\n"), 3,
                   "the write succeeds once the device recovers");
    TEST_ASSERT_EQ(fx_size_on_disk(&fx, "empty.txt"), 3u, "and the entry records it");
    fx_close(&fx);
}

/* Zero fields in FileBasicInformation mean "unchanged", and must not reach the
 * filesystem as a date. */
static void test_nt_basic_info_zero_means_unchanged(void)
{
    static struct vfs_ops ops;
    struct vfs_node node;
    FILE_BASIC_INFORMATION bi;

    memset(&ops, 0, sizeof(ops));
    memset(&node, 0, sizeof(node));
    memset(&bi, 0, sizeof(bi));
    ops.set_times = fx_stub_set_times;
    ops.set_attr = fx_stub_set_attr;
    node.ops = &ops;
    s_times_calls = 0;
    s_attr_calls = 0;

    bi.FileAttributes = FAT32_ATTR_HIDDEN;
    nt_file_apply_basic_info(&node, &bi);
    TEST_ASSERT_EQ(s_times_calls, 0u, "an attributes-only request touches no time");
    TEST_ASSERT_EQ(s_attr_calls, 1u, "and sets the attributes");

    bi.FileAttributes = 0;
    bi.LastWriteTime = filetime_from_unix_seconds(961070400u);
    nt_file_apply_basic_info(&node, &bi);
    TEST_ASSERT_EQ(s_times_calls, 1u, "a time reaches the filesystem");
    TEST_ASSERT(s_seen_times[0] == (filetime_t *)0, "with zero creation time as NULL");
    TEST_ASSERT(s_seen_times[1] != (filetime_t *)0, "the given write time present");
    TEST_ASSERT(s_seen_times[2] == (filetime_t *)0, "and zero access time as NULL");
    TEST_ASSERT_EQ(s_attr_calls, 1u, "zero attributes are not applied");
}

/* A removal that never reached the disk leaves a live entry there, so the chain
 * it points at must not go back to the free pool. */
static void test_fat32_failed_removal_keeps_chain(void)
{
    struct fx_volume fx;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "DOOMED.DAT"), 0,
                   "file created");
    data = fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "DOOMED.DAT");
    scache_flush(fx.vol);
    fx.fail_write_lba = cluster_to_sector(fx.vol, FX_ROOT_CLUSTER);
    TEST_ASSERT(fat32_delete_file_vol(fx.vol, FX_ROOT_CLUSTER, "DOOMED.DAT") != 0,
                "delete reports the failed directory write");
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, data) != FAT32_FREE,
                "the chain stays allocated");
    fx.fail_write_lba = FX_NO_FAULT;
    fx_close(&fx);
}

/* fsck's duplicate-name repair removes the whole run, and only from the later name. */
static void test_fat32_fsck_drops_duplicate_name(void)
{
    struct fx_volume fx;
    struct fat32_dirent_loc first, second;
    uint32_t data;

    TEST_ASSERT_EQ(fx_open(&fx), 0, "fixture volume allocated");
    if (!fx.vol || !fx.disk) { fx_close(&fx); return; }

    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "first-long-name.dat"), 0,
                   "first name created");
    TEST_ASSERT_EQ(fat32_create_file_vol(fx.vol, FX_ROOT_CLUSTER, "second-long-name.dat"), 0,
                   "second name created");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "first-long-name.dat", &first), 1,
                   "first found");
    TEST_ASSERT_EQ(fat32_dir_lookup(fx.vol, FX_ROOT_CLUSTER, "second-long-name.dat", &second), 1,
                   "second found");
    data = fx_first_cluster(&first);
    fat32_free_chain(fx.vol, fx_first_cluster(&second));
    second.de.first_cluster_hi = first.de.first_cluster_hi;
    second.de.first_cluster_lo = first.de.first_cluster_lo;
    TEST_ASSERT_EQ(fat32_dir_rewrite_entry(fx.vol, &second), 0, "second name shares the chain");

    TEST_ASSERT(fat32_fsck(fx.vol, 0) != 0, "read-only fsck reports the shared chain");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "second-long-name.dat"), data,
                   "and does not change it");
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 1), 0, "fsck repairs it");
    TEST_ASSERT_EQ(fx_count_bound_long_name(&fx, FX_ROOT_CLUSTER, "first-long-name.dat"), 1u,
                   "the first name stays");
    TEST_ASSERT_EQ(fx_cluster_of(fx.vol, FX_ROOT_CLUSTER, "second-long-name.dat"), 0u,
                   "the second is gone");
    TEST_ASSERT_EQ(fx_entry(&fx, second.run_cluster, second.run_offset / FX_ENTRY_SIZE)->name[0],
                   (uint8_t)0xE5, "along with its long-name run");
    TEST_ASSERT(fat32_get_fat_entry(fx.vol, data) != FAT32_FREE, "and the data is kept");
    TEST_ASSERT_EQ(fat32_fsck(fx.vol, 0), 0, "a second pass finds nothing");
    fx_close(&fx);
}

static void test_fat32_replace_crash_sweep(void)   { fx_rename_crash_sweep(1); }
static void test_fat32_rename_crash_sweep(void)    { fx_rename_crash_sweep(0); }

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
    test_suite_register_cat("FAT32: colliding names reach their own file",
                            test_fat32_colliding_names_reach_their_own_file, TEST_CAT_FS);
    test_suite_register_cat("FAT32: lookup alias rules",
                            test_fat32_lookup_alias_rules, TEST_CAT_FS);
    test_suite_register_cat("FAT32: lookup ignores an unbound run",
                            test_fat32_lookup_ignores_unbound_run, TEST_CAT_FS);
    test_suite_register_cat("FAT32: FSInfo free count bounded by the volume",
                            test_fat32_fsinfo_free_count_bounded, TEST_CAT_FS);
    test_suite_register_cat("FAT32: long-name run sequence rules",
                            test_fat32_lfn_run_sequence_rules, TEST_CAT_FS);
    test_suite_register_cat("FAT32: listing and stat ignore an unbound run",
                            test_fat32_listing_and_stat_ignore_unbound_run, TEST_CAT_FS);
    test_suite_register_cat("FAT32: removal clears a run across clusters",
                            test_fat32_remove_run_across_clusters, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rmdir keeps a directory it cannot read",
                            test_fat32_rmdir_read_failure_keeps_directory, TEST_CAT_FS);
    test_suite_register_cat("FAT32: set_attr uses the file's parent directory",
                            test_fat32_set_attr_uses_parent_directory, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rename rebinds the long name",
                            test_fat32_rename_rebinds_long_name, TEST_CAT_FS);
    test_suite_register_cat("FAT32: fsck walk read failure keeps a live tail",
                            test_fat32_fsck_walk_read_failure_keeps_live_tail, TEST_CAT_FS);
    test_suite_register_cat("FAT32: fsck repairs nothing when the FAT is unreadable",
                            test_fat32_fsck_unreadable_fat_repairs_nothing, TEST_CAT_FS);
    test_suite_register_cat("FAT32: read-only fsck reports a stale FAT mirror",
                            test_fat32_fsck_readonly_reports_fat_mirror, TEST_CAT_FS);
    test_suite_register_cat("FAT32: audit rotation replaces a generation across clusters",
                            test_fat32_audit_rotation_replaces_generation_across_clusters, TEST_CAT_FS);
    test_suite_register_cat("FAT32: replace repeatedly",
                            test_fat32_replace_repeatedly, TEST_CAT_FS);
    test_suite_register_cat("FAT32: create refuses an existing name",
                            test_fat32_create_refuses_existing_name, TEST_CAT_FS);
    test_suite_register_cat("FAT32: failed rename rolls back",
                            test_fat32_failed_rename_rolls_back, TEST_CAT_FS);
    test_suite_register_cat("fat32: write reports only recorded bytes",
                            test_fat32_write_reports_only_recorded_bytes, TEST_CAT_FS);
    test_suite_register_cat("fat32: unlinked first cluster fails the write",
                            test_fat32_unlinked_first_cluster_fails_write, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rollback when the alias is a long name",
                            test_fat32_rollback_alias_is_a_long_name, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rollback of a respelling",
                            test_fat32_rollback_respelling, TEST_CAT_FS);
    test_suite_register_cat("FAT32: dot names are refused",
                            test_fat32_dot_names_are_refused, TEST_CAT_FS);
    test_suite_register_cat("FAT32: fsck owner scan failure is not clean",
                            test_fat32_fsck_owner_scan_failure_is_not_clean, TEST_CAT_FS);
    test_suite_register_cat("FAT32: owner scan reports a FAT read failure",
                            test_fat32_owner_scan_fat_failure, TEST_CAT_FS);
    test_suite_register_cat("FAT32: fsck keeps both in a file and directory conflict",
                            test_fat32_fsck_file_directory_conflict, TEST_CAT_FS);
    test_suite_register_cat("FAT32: replace stops on a cold destination read failure",
                            test_fat32_replace_cold_destination_read_failure, TEST_CAT_FS);
    test_suite_register_cat("FAT32: replace survives a slot cleanup failure",
                            test_fat32_replace_survives_slot_cleanup_failure, TEST_CAT_FS);
    test_suite_register_cat("FAT32: replace keeps the source if its removal fails",
                            test_fat32_replace_failed_removal_keeps_source, TEST_CAT_FS);
    test_suite_register_cat("fat32: truncate keeps its chain when the directory write fails",
                            test_fat32_truncate_dir_write_failure_keeps_chain, TEST_CAT_FS);
    test_suite_register_cat("fat32: a shrink keeps its tail until the chain cut lands",
                            test_fat32_shrink_keeps_tail_until_cut_lands, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rename into a hole survives a crash at every write",
                            test_fat32_rename_into_hole_crash_sweep, TEST_CAT_FS);
    test_suite_register_cat("FAT32: orphan run after a cleanup failure stays dead",
                            test_fat32_orphan_run_after_cleanup_failure_stays_dead, TEST_CAT_FS);
    test_suite_register_cat("FAT32: orphan run after a failed publish stays dead",
                            test_fat32_orphan_run_after_failed_publish_stays_dead, TEST_CAT_FS);
    test_suite_register_cat("FAT32: short name collision is refused",
                            test_fat32_short_name_collision_refused, TEST_CAT_FS);
    test_suite_register_cat("FAT32: LFN extraction stays in bounds",
                            test_fat32_lfn_extract_stays_in_bounds, TEST_CAT_FS);
    test_suite_register_cat("FAT32: long name limit",
                            test_fat32_long_name_limit, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rename to the file's own alias",
                            test_fat32_rename_to_own_alias, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rename to own alias reports failure",
                            test_fat32_rename_to_own_alias_reports_failure, TEST_CAT_FS);
    test_suite_register_cat("FAT32: fsck keeps children of a duplicate directory",
                            test_fat32_fsck_duplicate_directory_keeps_children, TEST_CAT_FS);
    test_suite_register_cat("FAT32: fsck keeps names across directories",
                            test_fat32_fsck_keeps_names_across_directories, TEST_CAT_FS);
    test_suite_register_cat("FAT32: set_times stores the date given",
                            test_fat32_set_times_stores_the_date_given, TEST_CAT_FS);
    test_suite_register_cat("NT: FileBasicInformation zero means unchanged",
                            test_nt_basic_info_zero_means_unchanged, TEST_CAT_FS);
    test_suite_register_cat("FAT32: failed removal keeps the chain",
                            test_fat32_failed_removal_keeps_chain, TEST_CAT_FS);
    test_suite_register_cat("FAT32: fsck drops a duplicate name",
                            test_fat32_fsck_drops_duplicate_name, TEST_CAT_FS);
    test_suite_register_cat("FAT32: replace survives a crash at every write",
                            test_fat32_replace_crash_sweep, TEST_CAT_FS);
    test_suite_register_cat("FAT32: rename survives a crash at every write",
                            test_fat32_rename_crash_sweep, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
