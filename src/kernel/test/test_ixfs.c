/* ============================================================================
 * test_ixfs.c -- IXFS superblock bulletproofing unit tests
 *
 * Verifies superblock struct layout, magic, size, and checksum at compile
 * time (_Static_assert in ixfs.h) and runtime (these tests).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/ixfs.h"

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
    /* 32 inodes per 4 KiB block requires 128 bytes per inode */
    TEST_ASSERT_EQ(sizeof(struct ixfs_inode), 128,
                   "IXFS inode is 128 bytes (32 per block)");
    TEST_ASSERT_EQ(IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode), 32,
                   "32 inodes per block");
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
}

#endif /* KERNEL_TESTS */
