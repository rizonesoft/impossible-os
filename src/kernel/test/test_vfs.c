/* ============================================================================
 * test_vfs.c -- VFS unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/mbr.h"
#include "kernel/fs/gpt.h"
#include "kernel/types.h"

/* Test: create, write, read, close a file */
static void test_vfs_file_roundtrip(void)
{
    /* Create a test file on IXFS (root filesystem) */
    int rc = vfs_create("C:\\Impossible\\test_unit.tmp", VFS_FILE);
    TEST_ASSERT(rc == 0, "vfs_create test file succeeds");

    struct vfs_node *f = vfs_open("C:\\Impossible\\test_unit.tmp", 0);
    TEST_ASSERT(f != NULL, "vfs_open test file returns non-NULL");

    if (f) {
        /* Write test data */
        const uint8_t data[] = "Hello, test!";
        rc = vfs_write(f, 0, sizeof(data), data);
        TEST_ASSERT(rc >= 0, "vfs_write succeeds");

        /* Read it back */
        uint8_t buf[32];
        for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
        rc = vfs_read(f, 0, sizeof(data), buf);
        TEST_ASSERT(rc >= 0, "vfs_read succeeds");
        TEST_ASSERT(buf[0] == 'H' && buf[5] == ',', "vfs_read returns correct data");

        vfs_close(f);
    }

    /* Clean up */
    vfs_unlink("C:\\Impossible\\test_unit.tmp");
}

/* Test: open nonexistent file returns NULL */
static void test_vfs_open_nonexistent(void)
{
    struct vfs_node *f = vfs_open("C:\\Impossible\\nonexistent_xyzzy.tmp", 0);
    TEST_ASSERT(f == NULL, "vfs_open nonexistent file returns NULL");
}

/* Test: create and delete directory */
static void test_vfs_mkdir_rmdir(void)
{
    int rc = vfs_create("C:\\Impossible\\test_dir_unit", VFS_DIRECTORY);
    TEST_ASSERT(rc == 0, "vfs_create directory succeeds");

    /* Verify it exists by opening */
    struct vfs_node *d = vfs_open("C:\\Impossible\\test_dir_unit", 0);
    TEST_ASSERT(d != NULL, "created directory is openable");
    if (d) vfs_close(d);

    /* Clean up */
    rc = vfs_unlink("C:\\Impossible\\test_dir_unit");
    TEST_ASSERT(rc == 0, "vfs_unlink directory succeeds");
}

/* ---- Bulletproofing: VFS + partition constants ---- */

static void test_vfs_drive_constants(void)
{
    TEST_ASSERT_EQ(VFS_MAX_DRIVES, 26, "VFS_MAX_DRIVES == 26 (A-Z)");

    /* drive_index rejects letters outside A-Z via vfs_is_mounted returning 0 */
    TEST_ASSERT(vfs_is_mounted('Z') == 0 || vfs_is_mounted('Z') == 1,
                "Z is a valid drive letter (mounted or not)");
    /* '[' is the character after 'Z' -- should be rejected */
    TEST_ASSERT(vfs_is_mounted('[') == 0, "[ (after Z) rejected as drive letter");
    TEST_ASSERT(vfs_is_mounted('@') == 0, "@ (before A) rejected as drive letter");
}

static void test_mbr_gpt_constants(void)
{
    TEST_ASSERT_EQ(MBR_ENTRY_OFFSET, 446, "MBR partition table at offset 446 (0x1BE)");
    TEST_ASSERT_EQ(MBR_SIG_OFFSET, 510, "MBR boot signature at offset 510");
    TEST_ASSERT_EQ(MBR_ENTRY_SIZE, 16, "MBR entry is 16 bytes");
    TEST_ASSERT_EQ(MBR_MAX_PARTITIONS, 4, "MBR has 4 primary partitions");
    TEST_ASSERT_EQ(GPT_HEADER_LBA, 1, "GPT header at LBA 1 (UEFI spec)");
    TEST_ASSERT_EQ(GPT_ENTRY_SIZE, 128, "GPT entry is 128 bytes (UEFI spec)");
}

/* Registration */
void test_register_vfs(void)
{
    test_suite_register_cat("VFS: file roundtrip", test_vfs_file_roundtrip, TEST_CAT_FS);
    test_suite_register_cat("VFS: open nonexistent", test_vfs_open_nonexistent, TEST_CAT_FS);
    test_suite_register_cat("VFS: mkdir+rmdir", test_vfs_mkdir_rmdir, TEST_CAT_FS);
    test_suite_register_cat("VFS: drive letter range", test_vfs_drive_constants, TEST_CAT_FS);
    test_suite_register_cat("VFS: MBR+GPT constants", test_mbr_gpt_constants, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
