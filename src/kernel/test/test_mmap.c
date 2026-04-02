/* ============================================================================
 * test_mmap.c — mmap subsystem unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/mmap.h"
#include "kernel/fs/vfs.h"

static void test_mmap_file(void)
{
    if (!vfs_is_mounted('C')) {
        TEST_SKIP("C:\\ not mounted");
        return;
    }

    struct vfs_node *f = vfs_open("C:\\hello.txt", VFS_O_READ);
    if (!f) {
        TEST_SKIP("hello.txt not found");
        return;
    }

    void *mapped = mmap((void *)0, 4096, PROT_READ, MAP_PRIVATE, f, 0);
    TEST_ASSERT(mapped != MAP_FAILED, "mmap returns valid address");

    if (mapped != MAP_FAILED) {
        const char *txt = (const char *)mapped;
        TEST_ASSERT(txt[0] != '\0', "mmap content is non-empty");
        munmap(mapped, 4096);
    }

    vfs_close(f);
}

void test_register_mmap(void)
{
    test_suite_register_cat("Mmap: file mapping", test_mmap_file, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
