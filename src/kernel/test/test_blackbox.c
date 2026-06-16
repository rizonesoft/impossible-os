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
