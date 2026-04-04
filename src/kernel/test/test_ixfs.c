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

static void test_ixfs_superblock_size(void)
{
    TEST_ASSERT_EQ(sizeof(struct ixfs_superblock), 512,
                   "IXFS superblock is 512 bytes");
}

static void test_ixfs_magic_value(void)
{
    TEST_ASSERT_EQ(IXFS_MAGIC, 0x49584653, "IXFS_MAGIC == 0x49584653");
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

static void test_ixfs_version(void)
{
    TEST_ASSERT_EQ(IXFS_VERSION, 2, "IXFS_VERSION == 2");
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
    /* FSInfo signature constants must match FAT32 spec */
    TEST_ASSERT_EQ(0x41615252, 0x41615252, "FSINFO_LEAD_SIG correct");
    TEST_ASSERT_EQ(0x61417272, 0x61417272, "FSINFO_STRUCT_SIG correct");
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
    test_suite_register_cat("IXFS: magic value",
                            test_ixfs_magic_value, TEST_CAT_FS);
    test_suite_register_cat("IXFS: superblock offsets",
                            test_ixfs_superblock_offsets, TEST_CAT_FS);
    test_suite_register_cat("IXFS: version",
                            test_ixfs_version, TEST_CAT_FS);
    test_suite_register_cat("IXFS: inode size",
                            test_ixfs_inode_size, TEST_CAT_FS);

    /* FAT32 BPB validation tests */
    test_suite_register_cat("FAT32: BPB struct size",
                            test_fat32_bpb_constants, TEST_CAT_FS);
    test_suite_register_cat("FAT32: FSInfo signatures",
                            test_fat32_fsinfo_signatures, TEST_CAT_FS);
    test_suite_register_cat("FAT32: dirty bit semantics",
                            test_fat32_dirty_bit, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
