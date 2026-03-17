/* ============================================================================
 * test_vfs.c — VFS unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/vfs.h"
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

/* Registration */
void test_register_vfs(void)
{
    test_suite_register("VFS: file roundtrip", test_vfs_file_roundtrip);
    test_suite_register("VFS: open nonexistent", test_vfs_open_nonexistent);
    test_suite_register("VFS: mkdir+rmdir", test_vfs_mkdir_rmdir);
}

#endif /* KERNEL_TESTS */
