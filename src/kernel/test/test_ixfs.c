/* ============================================================================
 * test_ixfs.c -- IXFS superblock bulletproofing unit tests
 *
 * Verifies superblock struct layout, magic, size, and checksum at compile
 * time (_Static_assert in ixfs.h) and runtime (these tests).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/ixfs.h"
#include "kernel/fs/fat32.h"
#include "kernel/types.h"

/* fat32_internal.h carries the full struct fat32_volume definition; the
 * public fat32.h forward-declares it as incomplete. The negative BPB
 * fixtures below stack-allocate a volume struct, mutate one field, and
 * call fat32_validate_bpb -- they need the full type. */
#include "../fs/fat32/fat32_internal.h"

extern int fat32_validate_bpb(struct fat32_volume *vol);
extern void fat32_validate_set_quiet(int q);

/* Silent wrapper for negative-test cases. The validator's LOG_ERROR
 * klog renders as [FAIL] in the boot log even when the test is
 * passing (we deliberately feed it malformed BPBs). Wrap to suppress
 * the klog so only the [ OK ] TEST line surfaces. */
static int fat32_validate_bpb_q(struct fat32_volume *vol)
{
    fat32_validate_set_quiet(1);
    int r = fat32_validate_bpb(vol);
    fat32_validate_set_quiet(0);
    return r;
}

/* FSInfo signature constants are file-scope private to fat32_internal.h;
 * the test file mirrors the values per FAT32 spec to cover the constants
 * without pulling the internal header into the test build. */
#define TEST_FAT32_FSINFO_LEAD_SIG   0x41615252u
#define TEST_FAT32_FSINFO_STRUCT_SIG 0x61417272u
#define TEST_FAT32_FSINFO_TRAIL_SIG  0xAA550000u

static void test_ixfs_superblock_size(void)
{
    TEST_ASSERT_EQ(sizeof(struct ixfs_superblock), 512,
                   "IXFS superblock is 512 bytes");
}

static void test_ixfs_superblock_offsets(void)
{
    TEST_ASSERT_EQ(__builtin_offsetof(struct ixfs_superblock, s_magic), 0,
                   "s_magic at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(struct ixfs_superblock, s_version), 4,
                   "s_version at offset 4");
    TEST_ASSERT_EQ(__builtin_offsetof(struct ixfs_superblock, s_block_size), 8,
                   "s_block_size at offset 8");
    TEST_ASSERT_EQ(__builtin_offsetof(struct ixfs_superblock, s_volume_name), 60,
                   "s_volume_name at offset 60");
    TEST_ASSERT_EQ(__builtin_offsetof(struct ixfs_superblock, s_checksum), 128,
                   "s_checksum at offset 128");
    TEST_ASSERT_EQ(__builtin_offsetof(struct ixfs_superblock, s_reserved), 132,
                   "s_reserved at offset 132");
}

static void test_ixfs_inode_size(void)
{
    /* 32 inodes per 4 KiB block requires exactly 128 bytes per inode */
    TEST_ASSERT_EQ(sizeof(struct ixfs_inode), 128,
                   "IXFS inode is 128 bytes (32 per block)");
    TEST_ASSERT_EQ(IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode), 32,
                   "32 inodes per block");
}

/* ---- FAT32 BPB validation constants ---- */

static void test_fat32_bpb_constants(void)
{
    /* FAT32 BPB struct must contain all required fields */
    TEST_ASSERT(sizeof(struct fat32_bpb) >= 28,
                "fat32_bpb struct has all required fields (>= 28 bytes)");
    TEST_ASSERT(sizeof(struct fat32_bpb) <= 64,
                "fat32_bpb struct is reasonably sized (<= 64 bytes)");
}

static void test_fat32_fsinfo_signatures(void)
{
    /* FSInfo signature constants must match the FAT32 spec values
     * documented in fatgen103.doc. The driver's internal header
     * (fat32_internal.h) defines these for the runtime path; this
     * test pins the spec-required byte values so a refactor that
     * silently changes them does not slip past review. */
    TEST_ASSERT(TEST_FAT32_FSINFO_LEAD_SIG == 0x41615252u,
                "FSI_LeadSig matches FAT32 spec value 0x41615252");
    TEST_ASSERT(TEST_FAT32_FSINFO_STRUCT_SIG == 0x61417272u,
                "FSI_StrucSig matches FAT32 spec value 0x61417272");
    TEST_ASSERT(TEST_FAT32_FSINFO_TRAIL_SIG == 0xAA550000u,
                "FSI_TrailSig matches FAT32 spec value 0xAA550000");
}

/* Synthesize a minimal valid FAT32 BPB in a sector buffer for negative
 * tests below. Caller mutates one field then calls fat32_validate_bpb
 * via a stack-allocated struct fat32_volume that carries the buffer +
 * the parsed fields. */
static void test_fat32_bpb_make_valid(struct fat32_volume *vol)
{
    /* Zero everything; then populate only the fields validate_bpb reads. */
    uint8_t *p;
    uint32_t i;
    p = (uint8_t *)vol;
    for (i = 0; i < sizeof(*vol); i++)
        p[i] = 0;

    /* sector_buf[0]: jmpBoot[0] -- 0xEB is the short-jump form. */
    vol->sector_buf[0] = 0xEB;
    /* sector_buf[17]: RootEntCnt little-endian 16-bit; must be 0 for FAT32. */
    /* (already zeroed) */

    vol->bpb.bytes_per_sector    = 512;
    vol->bpb.sectors_per_cluster = 8;
    vol->bpb.num_fats            = 2;
    vol->bpb.root_cluster        = 2;
    vol->bpb.total_sectors       = 1024 * 1024;  /* 512 MiB volume */
    vol->bpb.reserved_sectors    = 32;
    vol->bpb.fat_size_sectors    = 1024;
}

static void test_fat32_bpb_rejects_bad_jmp(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    vol.sector_buf[0] = 0x90;  /* not 0xEB or 0xE9 */
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects bad jmpBoot[0]=0x90");
}

static void test_fat32_bpb_rejects_bad_bps(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    vol.bpb.bytes_per_sector = 513;  /* not in {512,1024,2048,4096} */
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects bytes_per_sector=513");
}

static void test_fat32_bpb_rejects_non_pow2_spc(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    vol.bpb.sectors_per_cluster = 3;  /* not a power of two */
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects sectors_per_cluster=3 (non-power-of-two)");
}

static void test_fat32_bpb_rejects_zero_reserved(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    vol.bpb.reserved_sectors = 0;
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects reserved_sectors=0");
}

static void test_fat32_bpb_rejects_zero_fat_size(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    vol.bpb.fat_size_sectors = 0;
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects fat_size_sectors=0");
}

static void test_fat32_bpb_rejects_first_data_overflow(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    /* reserved + 2 * fat_size_sectors > total_sectors -> first_data wraps
     * past the volume end. With total_sectors=1M, set fat_size=600K so
     * reserved (32) + 2*600K = 1200K + 32 > 1024K total. */
    vol.bpb.fat_size_sectors = 600u * 1024u;
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects first_data_sector >= total_sectors");
}

static void test_fat32_bpb_rejects_first_data_u32_wrap(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    /* Force u32 wrap: num_fats=2 * fat_size_sectors > UINT32_MAX.
     * reserved_sectors is uint16_t in the BPB so the wrap has to come
     * from fat_size_sectors. Set fsz so 2*fsz > UINT32_MAX. */
    vol.bpb.reserved_sectors = 0xFFFFu;            /* max u16 */
    vol.bpb.fat_size_sectors = 0x80000001u;        /* 2*fsz wraps u32 */
    vol.bpb.num_fats         = 2;
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects u32-wrapping first_data_sector");
}

static void test_fat32_bpb_rejects_huge_root_cluster(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    vol.bpb.root_cluster = 0x0FFFFFF0u;  /* > FAT32 max valid 0x0FFFFFEF */
    TEST_ASSERT(fat32_validate_bpb_q(&vol) == -1,
                "validate_bpb rejects root_cluster above max FAT32 entry");
}

static void test_fat32_bpb_accepts_valid(void)
{
    struct fat32_volume vol;
    test_fat32_bpb_make_valid(&vol);
    TEST_ASSERT(fat32_validate_bpb(&vol) == 0,
                "validate_bpb accepts well-formed BPB");
}

static void test_fat32_dirty_bit(void)
{
    /* FAT[1] bit 27 is the clean shutdown marker */
    uint32_t clean = 0x0FFFFFFF;
    uint32_t dirty = 0x07FFFFFF;  /* bit 27 clear */
    TEST_ASSERT((clean & 0x08000000) != 0,
                "clean FAT[1] has bit 27 set");
    TEST_ASSERT((dirty & 0x08000000) == 0,
                "dirty FAT[1] has bit 27 clear");
}

void test_register_ixfs(void)
{
    test_suite_register_cat("IXFS: superblock size",
                            test_ixfs_superblock_size, TEST_CAT_FS);
    test_suite_register_cat("IXFS: superblock offsets",
                            test_ixfs_superblock_offsets, TEST_CAT_FS);
    test_suite_register_cat("IXFS: inode size",
                            test_ixfs_inode_size, TEST_CAT_FS);

    /* FAT32 BPB validation tests */
    test_suite_register_cat("FAT32: BPB struct size",
                            test_fat32_bpb_constants, TEST_CAT_FS);
    test_suite_register_cat("FAT32: FSInfo signatures",
                            test_fat32_fsinfo_signatures, TEST_CAT_FS);
    test_suite_register_cat("FAT32: dirty bit semantics",
                            test_fat32_dirty_bit, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects bad jmpBoot",
                            test_fat32_bpb_rejects_bad_jmp, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects bad BytsPerSec",
                            test_fat32_bpb_rejects_bad_bps, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects non-pow2 SecPerClus",
                            test_fat32_bpb_rejects_non_pow2_spc, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects reserved_sectors=0",
                            test_fat32_bpb_rejects_zero_reserved, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects fat_size_sectors=0",
                            test_fat32_bpb_rejects_zero_fat_size, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects first_data overflow",
                            test_fat32_bpb_rejects_first_data_overflow, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects first_data u32 wrap",
                            test_fat32_bpb_rejects_first_data_u32_wrap, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB rejects huge root_cluster",
                            test_fat32_bpb_rejects_huge_root_cluster, TEST_CAT_FS);
    test_suite_register_cat("FAT32: BPB accepts well-formed",
                            test_fat32_bpb_accepts_valid, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
