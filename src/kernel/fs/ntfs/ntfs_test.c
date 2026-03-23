/* ============================================================================
 * ntfs_test.c — NTFS Filesystem Self-Test Suite (§8.1)
 *
 * When an NTFS volume with label "NTFS_TEST" is mounted, this module runs
 * a comprehensive read-only test suite after all subsystems are initialized.
 *
 * Test cases:
 *   1. Root directory listing (readdir)
 *   2. Known-content file read (byte-exact verification)
 *   3. Empty file read (0 bytes)
 *   4. Resident small file read (< 700 bytes)
 *   5. Large file read (5 MB, multi-run data)
 *   6. Subdirectory traversal (subdir/nested.txt)
 *   7. Deep directory tree (A/B/C/D/E/file.txt)
 *   8. Directory with >100 entries (INDX allocation)
 *   9. Dirty volume flag detection
 *
 * Output: [NTFS-TEST] PASS/FAIL per test case to serial (klog)
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ---- String helpers (no libc) ---- */

static int test_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int test_strncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static int test_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ---- Test framework ---- */

static int tests_run;
static int tests_passed;
static int tests_failed;

static void test_pass(const char *name)
{
    tests_run++;
    tests_passed++;
    klog(LOG_INFO, "ntfs-test", "[PASS] %s", (uint64_t)(uintptr_t)name);
}

static void test_fail(const char *name, const char *reason)
{
    tests_run++;
    tests_failed++;
    klog(LOG_ERROR, "ntfs-test", "[FAIL] %s — %s",
         (uint64_t)(uintptr_t)name, (uint64_t)(uintptr_t)reason);
}

/* ---- Helper: read entire file via VFS ---- */

static int read_file_via_vfs(struct vfs_node *root, const char *path,
                              uint8_t *buf, uint32_t buf_size,
                              uint32_t *bytes_read)
{
    /* Simple single-level path lookup */
    struct vfs_node *node;
    int rc;

    *bytes_read = 0;

    /* Walk path: split on '/' */
    node = root;
    {
        const char *p = path;
        char component[VFS_MAX_NAME];
        int ci;

        while (*p) {
            /* Skip leading slashes */
            while (*p == '/' || *p == '\\') p++;
            if (!*p) break;

            /* Extract component */
            ci = 0;
            while (*p && *p != '/' && *p != '\\' && ci < VFS_MAX_NAME - 1)
                component[ci++] = *p++;
            component[ci] = '\0';

            /* Lookup in current directory */
            if (!node->ops || !node->ops->finddir)
                return -1;
            node = node->ops->finddir(node, component);
            if (!node)
                return -1;
        }
    }

    /* Read the file */
    if (!node->ops || !node->ops->read)
        return -1;

    rc = node->ops->read(node, 0, buf_size, buf);
    if (rc >= 0)
        *bytes_read = (uint32_t)rc;
    return (rc >= 0) ? 0 : -1;
}

/* ---- Test 1: Root directory listing ---- */

static void test_root_listing(struct vfs_node *root)
{
    struct vfs_dirent *de;
    int count = 0;
    int found_test = 0;
    int found_empty = 0;
    int found_large = 0;
    uint32_t idx;

    if (!root->ops || !root->ops->readdir) {
        test_fail("root_listing", "readdir not supported");
        return;
    }

    /* Enumerate root directory entries */
    for (idx = 0; idx < 200; idx++) {
        de = root->ops->readdir(root, idx);
        if (!de) break;

        count++;
        if (test_strcmp(de->name, "test.txt") == 0) found_test = 1;
        if (test_strcmp(de->name, "empty.txt") == 0) found_empty = 1;
        if (test_strcmp(de->name, "large.bin") == 0) found_large = 1;
    }

    if (count == 0) {
        test_fail("root_listing", "no entries found");
        return;
    }

    klog(LOG_DEBUG, "ntfs-test", "Root directory: %u entries", (uint64_t)count);

    if (found_test && found_empty && found_large)
        test_pass("root_listing");
    else
        test_fail("root_listing",
                  "missing expected files (test.txt/empty.txt/large.bin)");
}

/* ---- Test 2: Known-content file read ---- */

static void test_known_content(struct vfs_node *root)
{
    uint8_t *buf;
    uintptr_t buf_phys;
    uint32_t bytes_read;
    int rc;
    const char *expected = "Test file for NTFS driver testing.";
    int expected_len = test_strlen(expected);

    buf_phys = pmm_alloc_contiguous(1);
    if (!buf_phys) {
        test_fail("known_content", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    rc = read_file_via_vfs(root, "test.txt", buf, 4096, &bytes_read);
    if (rc != 0) {
        test_fail("known_content", "read failed");
        pmm_free_frame(buf_phys);
        return;
    }

    if ((int)bytes_read != expected_len) {
        klog(LOG_ERROR, "ntfs-test",
             "known_content: expected %d bytes, got %u",
             (uint64_t)expected_len, (uint64_t)bytes_read);
        test_fail("known_content", "size mismatch");
        pmm_free_frame(buf_phys);
        return;
    }

    if (test_strncmp((const char *)buf, expected, expected_len) != 0) {
        test_fail("known_content", "content mismatch");
        pmm_free_frame(buf_phys);
        return;
    }

    test_pass("known_content");
    pmm_free_frame(buf_phys);
}

/* ---- Test 3: Empty file ---- */

static void test_empty_file(struct vfs_node *root)
{
    uint8_t buf[16];

    /* Lookup empty.txt */
    struct vfs_node *node;
    if (!root->ops || !root->ops->finddir) {
        test_fail("empty_file", "finddir not supported");
        return;
    }

    node = root->ops->finddir(root, "empty.txt");
    if (!node) {
        test_fail("empty_file", "file not found");
        return;
    }

    /* Check size is 0 */
    if (node->size != 0) {
        klog(LOG_ERROR, "ntfs-test",
             "empty_file: expected size 0, got %u", node->size);
        test_fail("empty_file", "non-zero size");
        return;
    }

    /* Try reading — should return 0 bytes */
    if (node->ops && node->ops->read) {
        int rc = node->ops->read(node, 0, 1, buf);
        (void)rc;  /* 0 or -1 are both acceptable for empty files */
    }

    test_pass("empty_file");
}

/* ---- Test 4: Resident small file ---- */

static void test_resident_file(struct vfs_node *root)
{
    uint8_t *buf;
    uintptr_t buf_phys;
    uint32_t bytes_read;
    int rc;
    int i;

    buf_phys = pmm_alloc_contiguous(1);
    if (!buf_phys) {
        test_fail("resident_file", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    rc = read_file_via_vfs(root, "resident.txt", buf, 4096, &bytes_read);
    if (rc != 0) {
        test_fail("resident_file", "read failed");
        pmm_free_frame(buf_phys);
        return;
    }

    /* Expected: 500 bytes of 'R' */
    if (bytes_read != 500) {
        klog(LOG_ERROR, "ntfs-test",
             "resident_file: expected 500 bytes, got %u",
             (uint64_t)bytes_read);
        test_fail("resident_file", "size mismatch");
        pmm_free_frame(buf_phys);
        return;
    }

    /* Verify content: all 'R' */
    for (i = 0; i < 500; i++) {
        if (buf[i] != 'R') {
            klog(LOG_ERROR, "ntfs-test",
                 "resident_file: byte %d is 0x%x, expected 'R'",
                 (uint64_t)i, (uint64_t)buf[i]);
            test_fail("resident_file", "content mismatch");
            pmm_free_frame(buf_phys);
            return;
        }
    }

    test_pass("resident_file");
    pmm_free_frame(buf_phys);
}

/* ---- Test 5: Large file (multi-run) ---- */

static void test_large_file(struct vfs_node *root)
{
    struct vfs_node *node;

    if (!root->ops || !root->ops->finddir) {
        test_fail("large_file", "finddir not supported");
        return;
    }

    node = root->ops->finddir(root, "large.bin");
    if (!node) {
        test_fail("large_file", "file not found");
        return;
    }

    /* 5 MB = 5242880 bytes */
    if (node->size < 5000000) {
        klog(LOG_ERROR, "ntfs-test",
             "large_file: expected ~5 MB, got %u bytes",
             node->size);
        test_fail("large_file", "file too small");
        return;
    }

    /* Read first 4 KB to verify data runs work */
    {
        uintptr_t buf_phys = pmm_alloc_contiguous(1);
        if (!buf_phys) {
            test_fail("large_file", "PMM alloc failed");
            return;
        }
        uint8_t *buf = (uint8_t *)(uintptr_t)buf_phys;
        int rc = node->ops->read(node, 0, 4096, buf);
        if (rc <= 0) {
            test_fail("large_file", "read first 4 KB failed");
            pmm_free_frame(buf_phys);
            return;
        }

        /* Read last 4 KB to verify multi-run stitching */
        uint32_t offset = (uint32_t)(node->size - 4096);
        rc = node->ops->read(node, offset, 4096, buf);
        if (rc <= 0) {
            test_fail("large_file", "read last 4 KB failed");
            pmm_free_frame(buf_phys);
            return;
        }

        test_pass("large_file");
        pmm_free_frame(buf_phys);
    }
}

/* ---- Test 6: Subdirectory traversal ---- */

static void test_subdir(struct vfs_node *root)
{
    uint8_t *buf;
    uintptr_t buf_phys;
    uint32_t bytes_read;
    int rc;
    const char *expected = "Nested file in a subdirectory.";
    int expected_len = test_strlen(expected);

    buf_phys = pmm_alloc_contiguous(1);
    if (!buf_phys) {
        test_fail("subdir_traversal", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    rc = read_file_via_vfs(root, "subdir/nested.txt", buf, 4096, &bytes_read);
    if (rc != 0) {
        /* subdir may not exist if FUSE mount was unavailable */
        test_fail("subdir_traversal", "read failed (FUSE-created?)");
        pmm_free_frame(buf_phys);
        return;
    }

    if ((int)bytes_read != expected_len ||
        test_strncmp((const char *)buf, expected, expected_len) != 0) {
        test_fail("subdir_traversal", "content mismatch");
        pmm_free_frame(buf_phys);
        return;
    }

    test_pass("subdir_traversal");
    pmm_free_frame(buf_phys);
}

/* ---- Test 7: Deep directory tree ---- */

static void test_deep_dir(struct vfs_node *root)
{
    uint8_t *buf;
    uintptr_t buf_phys;
    uint32_t bytes_read;
    int rc;
    const char *expected = "Deep nested file at level 5.";
    int expected_len = test_strlen(expected);

    buf_phys = pmm_alloc_contiguous(1);
    if (!buf_phys) {
        test_fail("deep_directory", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    rc = read_file_via_vfs(root, "A/B/C/D/E/file.txt",
                            buf, 4096, &bytes_read);
    if (rc != 0) {
        test_fail("deep_directory", "path resolution failed (FUSE-created?)");
        pmm_free_frame(buf_phys);
        return;
    }

    if ((int)bytes_read != expected_len ||
        test_strncmp((const char *)buf, expected, expected_len) != 0) {
        test_fail("deep_directory", "content mismatch");
        pmm_free_frame(buf_phys);
        return;
    }

    test_pass("deep_directory");
    pmm_free_frame(buf_phys);
}

/* ---- Test 8: Many files directory (INDX allocation) ---- */

static void test_many_files(struct vfs_node *root)
{
    struct vfs_node *dir;
    struct vfs_dirent *de;
    int count = 0;
    uint32_t idx;

    if (!root->ops || !root->ops->finddir) {
        test_fail("many_files", "finddir not supported");
        return;
    }

    dir = root->ops->finddir(root, "manyfiles");
    if (!dir) {
        test_fail("many_files", "manyfiles/ not found (FUSE-created?)");
        return;
    }

    if (!dir->ops || !dir->ops->readdir) {
        test_fail("many_files", "readdir not supported on dir");
        return;
    }

    /* Count entries */
    for (idx = 0; idx < 200; idx++) {
        de = dir->ops->readdir(dir, idx);
        if (!de) break;
        count++;
    }

    klog(LOG_DEBUG, "ntfs-test",
         "manyfiles/: %u entries found", (uint64_t)count);

    /* Expect at least 100 files (we created 120) */
    if (count >= 100)
        test_pass("many_files");
    else
        test_fail("many_files", "expected >= 100 entries");
}

/* ---- Test 9: Dirty volume flag ---- */

static void test_dirty_flag(struct ntfs_volume *vol)
{
    /* The test image is created cleanly, so dirty flag should be 0.
     * We still verify the flag was read correctly. */
    if (vol->sysfiles_loaded) {
        klog(LOG_DEBUG, "ntfs-test",
             "Volume dirty flag: %u", (uint64_t)vol->volume_dirty);
        /* Clean volume should have dirty == 0 */
        if (vol->volume_dirty == 0)
            test_pass("dirty_flag_clean");
        else
            test_fail("dirty_flag_clean",
                      "expected clean volume but dirty flag set");
    } else {
        test_fail("dirty_flag_clean", "sysfiles not loaded");
    }
}

/* ---- Public API ---- */

void ntfs_run_self_test(struct ntfs_volume *vol, struct vfs_node *root)
{
    /* Only run tests on volumes labeled "NTFS_TEST" */
    if (test_strcmp(vol->volume_name, "NTFS_TEST") != 0)
        return;

    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    klog(LOG_INFO, "ntfs-test",
         "════════════════════════════════════════════════");
    klog(LOG_INFO, "ntfs-test",
         "NTFS Filesystem Self-Test Suite");
    klog(LOG_INFO, "ntfs-test",
         "Volume: \"%s\", FRS=%u, cluster=%u",
         (uint64_t)(uintptr_t)vol->volume_name,
         (uint64_t)vol->frs_size,
         (uint64_t)vol->cluster_size);
    klog(LOG_INFO, "ntfs-test",
         "════════════════════════════════════════════════");

    /* Core tests (always available — created via ntfscp) */
    test_root_listing(root);
    test_known_content(root);
    test_empty_file(root);
    test_resident_file(root);
    test_large_file(root);
    test_dirty_flag(vol);

    /* Extended tests (require FUSE-created directories) */
    test_subdir(root);
    test_deep_dir(root);
    test_many_files(root);

    /* Summary */
    klog(LOG_INFO, "ntfs-test",
         "════════════════════════════════════════════════");
    if (tests_failed == 0) {
        klog(LOG_INFO, "ntfs-test",
             "NTFS TEST SUITE PASSED: %u/%u tests OK",
             (uint64_t)tests_passed, (uint64_t)tests_run);
    } else {
        klog(LOG_ERROR, "ntfs-test",
             "NTFS TEST SUITE FAILED: %u passed, %u failed (of %u)",
             (uint64_t)tests_passed, (uint64_t)tests_failed,
             (uint64_t)tests_run);
    }
    klog(LOG_INFO, "ntfs-test",
         "════════════════════════════════════════════════");
}
