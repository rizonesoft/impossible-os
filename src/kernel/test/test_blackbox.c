/* ============================================================================
 * test_blackbox.c -- BlackBox service partition unit tests
 *
 * Verifies the BlackBox partition is mounted as X:\ with all expected
 * directories, correct volume label, and available free space.
 *
 * XREF: 01-boot-platform/TODO-24-blackbox-service-partition.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/fat32.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"

/* ---- Cleanup path builder ---- */

static void test_bb_cleanup_path(void)
{
    char name[VFS_MAX_NAME];
    char path[VFS_MAX_PATH];
    char small[12];
    const char *dir = "X:\\Logs\\";   /* 8 characters */
    uint32_t i;

    /* The longest name a directory entry can carry. The builder this replaced
     * stopped at 60 characters, so this name reached vfs_unlink as a different
     * file. */
    for (i = 0; i < VFS_MAX_NAME - 1; i++) name[i] = 'a';
    name[VFS_MAX_NAME - 1] = '\0';
    TEST_ASSERT_EQ(boot_blackbox_cleanup_path(path, sizeof(path), dir, name), 1,
                   "a maximum-length name fits VFS_MAX_PATH");
    for (i = 0; path[i]; i++) { }
    TEST_ASSERT_EQ(i, 8u + VFS_MAX_NAME - 1,
                   "every character of the name reached the path");
    TEST_ASSERT_EQ(path[7], '\\', "the directory prefix is intact");
    TEST_ASSERT_EQ(path[8 + VFS_MAX_NAME - 2], 'a', "the name's last character survived");

    /* Exact fit, counting the terminator: 8 + 3 + NUL in 12 bytes. */
    TEST_ASSERT_EQ(boot_blackbox_cleanup_path(small, sizeof(small), dir, "abc"), 1,
                   "a path filling the buffer exactly is accepted");
    TEST_ASSERT_EQ(small[10], 'c', "the exact fit keeps its last character");
    TEST_ASSERT_EQ(small[11], '\0', "the exact fit is terminated");

    /* One over refuses and leaves no partial path to act on. */
    TEST_ASSERT_EQ(boot_blackbox_cleanup_path(small, sizeof(small), dir, "abcd"), 0,
                   "one character over is refused, not cut");
    TEST_ASSERT_EQ(small[0], '\0', "a refused name leaves dst empty");
    TEST_ASSERT_EQ(boot_blackbox_cleanup_path(small, 4, dir, "a"), 0,
                   "a directory that does not fit is refused too");
    TEST_ASSERT_EQ(small[0], '\0', "a refused directory leaves dst empty");
    TEST_ASSERT_EQ(boot_blackbox_cleanup_path(small, 0, dir, "a"), 0,
                   "a zero-capacity buffer is refused without a write");
}

/* ---- X:\ mount check ---- */

static void test_bb_mounted(void)
{
    if (!vfs_is_mounted('X')) {
        TEST_SKIP("BlackBox partition not present");
        return;
    }
    TEST_ASSERT(vfs_is_mounted('X'), "X:\\ is mounted");
}

/* ---- Directory existence ---- */

static void test_bb_dir(const char *path, const char *desc)
{
    struct vfs_node *n;
    if (!vfs_is_mounted('X')) { TEST_SKIP("no BlackBox"); return; }
    n = vfs_open(path, 0);
    TEST_ASSERT(n != 0, desc);
    if (n) vfs_close(n);
}

static void test_bb_logs_dir(void)
{
    test_bb_dir("X:\\Logs", "X:\\Logs\\ directory exists");
}

static void test_bb_boot_dir(void)
{
    test_bb_dir("X:\\Boot", "X:\\Boot\\ directory exists");
}

static void test_bb_crash_dir(void)
{
    test_bb_dir("X:\\Crash", "X:\\Crash\\ directory exists");
}

static void test_bb_crash_wer_dir(void)
{
    test_bb_dir("X:\\Crash\\WER", "X:\\Crash\\WER\\ directory exists");
}

static void test_bb_perf_dir(void)
{
    test_bb_dir("X:\\Perf", "X:\\Perf\\ directory exists");
}

static void test_bb_diag_dir(void)
{
    test_bb_dir("X:\\Diag", "X:\\Diag\\ directory exists");
}

static void test_bb_tools_dir(void)
{
    test_bb_dir("X:\\Tools", "X:\\Tools\\ directory exists");
}

/* ---- klog_dir resolution ---- */

static void test_bb_klog_dir(void)
{
    extern const char *klog_dir;
    extern int klog_using_blackbox;

    if (!vfs_is_mounted('X')) {
        /* Fallback mode -- klog_dir should be C:\ path */
        TEST_ASSERT(klog_using_blackbox == 0,
                    "klog_using_blackbox == 0 (no BlackBox)");
        return;
    }
    TEST_ASSERT(klog_using_blackbox == 1,
                "klog_using_blackbox == 1 (BlackBox present)");
    TEST_ASSERT(klog_dir[0] == 'X' && klog_dir[1] == ':',
                "klog_dir starts with X:\\");
}

/* ---- Volume label ---- */

static void test_bb_volume_label(void)
{
    struct vfs_node *root;
    struct fat32_volume *vol;

    if (!vfs_is_mounted('X')) { TEST_SKIP("no BlackBox"); return; }

    root = vfs_get_drive_root('X');
    TEST_ASSERT(root != 0, "X:\\ root node exists");
    if (!root) return;

    vol = fat32_volume_from_root(root);
    TEST_ASSERT(vol != 0, "X:\\ is a FAT32 volume");
    if (!vol) return;

    {
        const char *label = fat32_get_label(vol);
        TEST_ASSERT(label[0] == 'B' && label[1] == 'L' &&
                    label[2] == 'A' && label[3] == 'C' &&
                    label[4] == 'K' && label[5] == 'B' &&
                    label[6] == 'O' && label[7] == 'X',
                    "FAT32 volume label is BLACKBOX");
    }
}

/* ---- Free space ---- */

static void test_bb_free_space(void)
{
    struct vfs_node *root;
    struct fat32_volume *vol;
    uint64_t free_bytes;

    if (!vfs_is_mounted('X')) { TEST_SKIP("no BlackBox"); return; }

    root = vfs_get_drive_root('X');
    if (!root) { TEST_SKIP("no root"); return; }

    vol = fat32_volume_from_root(root);
    if (!vol) { TEST_SKIP("not FAT32"); return; }

    free_bytes = fat32_get_free_bytes(vol);
    TEST_ASSERT(free_bytes > 0, "BlackBox free space > 0");
}

/* ---- Idempotent skeleton creation (regression) ----
 * The BlackBox skeleton is (re)created on every boot. fat32_create_dir_vol
 * must refuse to create a directory that already exists; without the guard
 * each boot allocated a fresh cluster + wrote a duplicate dirent, leaking
 * space on X: behind false "created" logs. A refused create returns != 0
 * and writes nothing, so re-running is a true no-op. */
static void test_bb_mkdir_idempotent(void)
{
    if (!vfs_is_mounted('X')) { TEST_SKIP("no BlackBox"); return; }
    TEST_ASSERT(vfs_create("X:\\Logs", VFS_DIRECTORY) != 0,
                "re-creating existing X:\\Logs is refused (no duplicate leak)");
}

/* ---- Registration ---- */

void test_register_blackbox(void)
{
    test_suite_register_cat("BB: X:\\ mounted",
                            test_bb_mounted, TEST_CAT_FS);
    test_suite_register_cat("BB: X:\\Logs\\ exists",
                            test_bb_logs_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: X:\\Boot\\ exists",
                            test_bb_boot_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: X:\\Crash\\ exists",
                            test_bb_crash_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: X:\\Crash\\WER\\ exists",
                            test_bb_crash_wer_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: X:\\Perf\\ exists",
                            test_bb_perf_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: X:\\Diag\\ exists",
                            test_bb_diag_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: cleanup path refuses to truncate",
                            test_bb_cleanup_path, TEST_CAT_BOOT);
    test_suite_register_cat("BB: X:\\Tools\\ exists",
                            test_bb_tools_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: klog_dir resolved",
                            test_bb_klog_dir, TEST_CAT_FS);
    test_suite_register_cat("BB: volume label BLACKBOX",
                            test_bb_volume_label, TEST_CAT_FS);
    test_suite_register_cat("BB: free space > 0",
                            test_bb_free_space, TEST_CAT_FS);
    test_suite_register_cat("BB: idempotent skeleton mkdir",
                            test_bb_mkdir_idempotent, TEST_CAT_FS);
}

#else
void test_register_blackbox(void) {}
#endif
