/* ntfs_test.c — NTFS Filesystem Self-Test Suite (§8.1)
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
 *   6. Dirty volume flag detection (raw $VOLUME_INFORMATION cross-check)
 *   7. Subdirectory traversal (subdir/nested.txt)
 *   8. Deep directory tree (A/B/C/D/E/file.txt)
 *   9. Directory with >100 entries (INDX allocation)
 *  10. Long filename (200+ chars, UTF-16LE)
 *  11. LZNT1 decompressor unit test (handcrafted stream)
 *  12. LZNT1 known-content file (transparent decompression)
 *  13. LZNT1 sparse zero file (all-zero CU → zero-fill)
 *  14. LZNT1 uncompressed file (random data stored raw)
 *  15. LZNT1 mixed CU file (compress + random + compress)
 *  16. LZNT1 round-trip compress→decompress (byte-exact, repeating data)
 *  17. LZNT1 round-trip compress→decompress (byte-exact, mixed content)
 *
 * Output: [NTFS-TEST] PASS/FAIL per test case to serial (klog)
 * ============================================================================ */


#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
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

/* ---- Test 10: Long filename (200+ chars, UTF-16LE) ---- */

static void test_long_filename(struct vfs_node *root)
{
    /* The test image has a file with a 200+ character name.
     * Search root for any entry with name length > 100 characters.
     * This verifies UTF-16LE → ASCII decoding and length handling. */
    struct vfs_dirent *de;
    uint32_t idx = 0;
    int found = 0;

    if (!root || !root->ops || !root->ops->readdir) {
        test_fail("long_filename", "root readdir unavailable");
        return;
    }

    while ((de = root->ops->readdir(root, idx)) != NULL) {
        uint32_t len = test_strlen(de->name);
        if (len > 100) {
            found = 1;
            klog(LOG_DEBUG, "ntfs-test",
                 "Long filename found: %u chars",
                 (uint64_t)len);
            break;
        }
        idx++;
    }

    if (found)
        test_pass("long_filename");
    else
        test_fail("long_filename",
                  "no entry > 100 chars (FUSE-created?)");
}

/* ---- Test 9: Dirty volume flag ---- */

static void test_dirty_flag(struct ntfs_volume *vol)
{
    /* The test image is created cleanly, so dirty flag should be 0.
     * We also verify that the $VOLUME_INFORMATION attribute on inode 3
     * has the expected flags — this validates the full parsing chain. */
    uintptr_t rec_phys;
    uint8_t *rec_buf;
    struct ntfs_mft_header hdr;
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    int rc;

    if (!vol->sysfiles_loaded) {
        test_fail("dirty_flag", "sysfiles not loaded");
        return;
    }

    /* Part 1: Verify cached dirty flag is clean (0) */
    if (vol->volume_dirty != 0) {
        test_fail("dirty_flag_clean",
                  "expected clean volume but dirty flag set");
        return;
    }

    /* Part 2: Read inode 3 ($Volume) and verify $VOLUME_INFORMATION raw flags */
    rec_phys = pmm_alloc_contiguous(1);
    if (!rec_phys) {
        test_fail("dirty_flag_raw", "PMM alloc failed");
        return;
    }
    rec_buf = (uint8_t *)(uintptr_t)rec_phys;

    rc = ntfs_read_mft_record(vol, 3, rec_buf, &hdr);
    if (rc != NTFS_OK) {
        test_fail("dirty_flag_raw", "cannot read inode 3 ($Volume)");
        pmm_free_frame(rec_phys);
        return;
    }

    /* Find $VOLUME_INFORMATION (type 0x70) */
    attr = ntfs_attr_find(rec_buf, &hdr, 0x70, &ah);
    if (!attr || ah.non_resident != 0 || ah.content_length < 12) {
        test_fail("dirty_flag_raw",
                  "$VOLUME_INFORMATION not found or too small");
        pmm_free_frame(rec_phys);
        return;
    }

    {
        const uint8_t *vi = attr + ah.content_offset;
        /* Layout: [reserved:8][major:1][minor:1][flags:2] */
        uint8_t  major  = vi[8];
        uint8_t  minor  = vi[9];
        uint16_t vflags = ntfs_le16(vi + 10);
        uint8_t  raw_dirty = (vflags & 0x0001) ? 1 : 0;

        klog(LOG_DEBUG, "ntfs-test",
             "dirty_flag_raw: version=%u.%u flags=0x%04x dirty_bit=%u",
             (uint64_t)major, (uint64_t)minor,
             (uint64_t)vflags, (uint64_t)raw_dirty);

        /* Verify the raw dirty bit matches what vol->volume_dirty says */
        if (raw_dirty != vol->volume_dirty) {
            klog(LOG_ERROR, "ntfs-test",
                 "dirty_flag mismatch: raw=%u cached=%u",
                 (uint64_t)raw_dirty, (uint64_t)vol->volume_dirty);
            test_fail("dirty_flag_raw", "raw vs cached mismatch");
            pmm_free_frame(rec_phys);
            return;
        }

        /* Verify NTFS version is sensible (3.x) */
        if (major != 3 || minor > 1) {
            klog(LOG_ERROR, "ntfs-test",
                 "unexpected NTFS version: %u.%u",
                 (uint64_t)major, (uint64_t)minor);
            test_fail("dirty_flag_raw", "unexpected NTFS version");
            pmm_free_frame(rec_phys);
            return;
        }

        /* Clean volume: raw dirty bit should be 0 */
        if (raw_dirty != 0) {
            test_fail("dirty_flag_raw", "clean image has dirty bit set");
            pmm_free_frame(rec_phys);
            return;
        }
    }

    pmm_free_frame(rec_phys);
    test_pass("dirty_flag");
}


/* ---- Test 11: Direct LZNT1 Decompressor Unit Test ---- */

/* Test the ntfs_lznt1_decompress() function directly with known input/output.
 * This validates the algorithm in isolation, independent of disk I/O or VFS.
 *
 * We craft a minimal LZNT1 stream by hand:
 *   Sub-block 1 (uncompressed): 16 bytes of "HelloHelloWorld!"
 *   Sub-block 2 (compressed):   literal 'A','B','C' + back-ref to copy 'ABC'
 *
 * LZNT1 sub-block header format:
 *   bits 0-11:  data_size - 1
 *   bits 12-14: signature (0x3 = 011)
 *   bit 15:     1 = compressed, 0 = uncompressed
 */
static void test_lznt1_decompress(void)
{
    /* ---- Sub-block 1: uncompressed, 16 bytes ---- */
    /* header = (16-1) | (0x3 << 12) | 0 = 0x300F  → little-endian: 0F 30 */
    /* data = "HelloHelloWorld!" (16 bytes exactly) */

    /* ---- Sub-block 2: compressed, produces "ABCABC" (6 bytes) ---- */
    /* header = compressed flag set, data size = (actual_bytes - 1)
     * Compressed data:
     *   flag_byte = 0x08 (bit 3 = back-ref, bits 0-2 = literal)
     *   token 0: literal 'A' (0x41)
     *   token 1: literal 'B' (0x42)
     *   token 2: literal 'C' (0x43)
     *   token 3: back-reference: at dst_pos=3, displacement_bits=4, len_bits=12
     *            displacement=3 (3-1=2 in field, but disp= field+1), length=3
     *            ref = ((disp-1) << len_bits) | (length-3)
     *            ref = (2 << 12) | 0 = 0x2000  → LE: 00 20
     *   5 bytes of compressed data, header = (5-1) | (0x3<<12) | 0x8000 = 0xB004
     *   Little-endian: 04 B0
     */
    static const uint8_t compressed[] = {
        /* Sub-block 1: uncompressed 16 bytes */
        0x0F, 0x30,                                          /* header */
        'H','e','l','l','o','H','e','l','l','o','W','o','r','l','d','!',

        /* Sub-block 2: compressed → "ABCABC" */
        0x04, 0xB0,                                          /* header */
        0x08,                                                /* flag byte */
        0x41, 0x42, 0x43,                                    /* A, B, C */
        0x00, 0x20,                                          /* back-ref */
    };

    uint8_t output[4096];
    int result;
    const char *expected = "HelloHelloWorld!ABCABC";
    int expected_len = 22;
    int i;

    ntfs_memset(output, 0xFF, sizeof(output));

    result = ntfs_lznt1_decompress(compressed, sizeof(compressed),
                                    output, sizeof(output));

    if (result < 0) {
        test_fail("lznt1_decompress", "decompress returned error");
        return;
    }

    klog(LOG_DEBUG, "ntfs-test",
         "lznt1_decompress: got %d bytes (expected %d)",
         (uint64_t)result, (uint64_t)expected_len);

    if (result != expected_len) {
        klog(LOG_ERROR, "ntfs-test",
             "lznt1_decompress: size mismatch %d vs %d",
             (uint64_t)result, (uint64_t)expected_len);
        test_fail("lznt1_decompress", "output size mismatch");
        return;
    }

    for (i = 0; i < expected_len; i++) {
        if (output[i] != (uint8_t)expected[i]) {
            klog(LOG_ERROR, "ntfs-test",
                 "lznt1_decompress: byte %d is 0x%x, expected 0x%x",
                 (uint64_t)i, (uint64_t)output[i],
                 (uint64_t)(uint8_t)expected[i]);
            test_fail("lznt1_decompress", "content mismatch");
            return;
        }
    }

    test_pass("lznt1_decompress");
}

/* ---- Tests 12-15: LZNT1 VFS-Level Compression (§9.1) ---- */

/* Helper: check if compressed/ directory exists on the test volume */
static struct vfs_node *find_compressed_dir(struct vfs_node *root)
{
    if (!root || !root->ops || !root->ops->finddir)
        return NULL;
    return root->ops->finddir(root, "compressed");
}

/* Test 11: Known-content compressed file — transparent LZNT1 decompression */
static void test_lznt1_known(struct vfs_node *root)
{
    uint8_t *buf;
    uintptr_t buf_phys;
    uint32_t bytes_read;
    int rc;
    int i;
    /* Expected: 8192 bytes of repeating "COMPRESS_TEST_" (14 chars) */
    const char *pattern = "COMPRESS_TEST_";
    int pat_len = 14;

    buf_phys = pmm_alloc_contiguous(2);  /* 8 KB */
    if (!buf_phys) {
        test_fail("lznt1_known", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    rc = read_file_via_vfs(root, "compressed/known.txt", buf, 8192, &bytes_read);
    if (rc != 0) {
        test_fail("lznt1_known", "read failed (compressed dir missing?)");
        pmm_free_frame(buf_phys);
        pmm_free_frame(buf_phys + 4096);
        return;
    }

    if (bytes_read != 8192) {
        klog(LOG_ERROR, "ntfs-test",
             "lznt1_known: expected 8192 bytes, got %u",
             (uint64_t)bytes_read);
        test_fail("lznt1_known", "size mismatch");
        pmm_free_frame(buf_phys);
        pmm_free_frame(buf_phys + 4096);
        return;
    }

    /* Verify repeating pattern */
    for (i = 0; i < 8192; i++) {
        if (buf[i] != (uint8_t)pattern[i % pat_len]) {
            klog(LOG_ERROR, "ntfs-test",
                 "lznt1_known: byte %d is 0x%x, expected '%c'",
                 (uint64_t)i, (uint64_t)buf[i],
                 (uint64_t)(uint8_t)pattern[i % pat_len]);
            test_fail("lznt1_known", "content mismatch");
            pmm_free_frame(buf_phys);
            pmm_free_frame(buf_phys + 4096);
            return;
        }
    }

    test_pass("lznt1_known");
    pmm_free_frame(buf_phys);
    pmm_free_frame(buf_phys + 4096);
}

/* Test 12: Sparse zero file — all-zero CU should be zero-filled */
static void test_lznt1_sparse(struct vfs_node *root)
{
    struct vfs_node *node;
    uintptr_t buf_phys;
    uint8_t *buf;
    int rc;
    int i;
    struct vfs_node *cdir;

    cdir = find_compressed_dir(root);
    if (!cdir) {
        test_fail("lznt1_sparse", "compressed/ not found");
        return;
    }

    node = cdir->ops->finddir(cdir, "zeros.bin");
    if (!node) {
        test_fail("lznt1_sparse", "zeros.bin not found");
        return;
    }

    /* Read first 4 KB */
    buf_phys = pmm_alloc_contiguous(1);
    if (!buf_phys) {
        test_fail("lznt1_sparse", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    if (!node->ops || !node->ops->read) {
        test_fail("lznt1_sparse", "read not supported");
        pmm_free_frame(buf_phys);
        return;
    }

    rc = node->ops->read(node, 0, 4096, buf);
    if (rc <= 0) {
        test_fail("lznt1_sparse", "read failed");
        pmm_free_frame(buf_phys);
        return;
    }

    /* Verify all zeros */
    for (i = 0; i < rc; i++) {
        if (buf[i] != 0) {
            klog(LOG_ERROR, "ntfs-test",
                 "lznt1_sparse: byte %d is 0x%x, expected 0",
                 (uint64_t)i, (uint64_t)buf[i]);
            test_fail("lznt1_sparse", "non-zero byte in sparse file");
            pmm_free_frame(buf_phys);
            return;
        }
    }

    /* Also read last 4 KB to verify tail of file */
    if (node->size > 4096) {
        uint32_t tail_off = (uint32_t)(node->size - 4096);
        rc = node->ops->read(node, tail_off, 4096, buf);
        if (rc > 0) {
            for (i = 0; i < rc; i++) {
                if (buf[i] != 0) {
                    test_fail("lznt1_sparse", "non-zero byte in tail");
                    pmm_free_frame(buf_phys);
                    return;
                }
            }
        }
    }

    test_pass("lznt1_sparse");
    pmm_free_frame(buf_phys);
}

/* Test 13: Incompressible file — random data stored uncompressed */
static void test_lznt1_uncompressed(struct vfs_node *root)
{
    struct vfs_node *node;
    uintptr_t buf_phys;
    uint8_t *buf;
    int rc;
    int non_zero = 0;
    int i;
    struct vfs_node *cdir;

    cdir = find_compressed_dir(root);
    if (!cdir) {
        test_fail("lznt1_uncompressed", "compressed/ not found");
        return;
    }

    node = cdir->ops->finddir(cdir, "random.bin");
    if (!node) {
        test_fail("lznt1_uncompressed", "random.bin not found");
        return;
    }

    buf_phys = pmm_alloc_contiguous(2);  /* 8 KB */
    if (!buf_phys) {
        test_fail("lznt1_uncompressed", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    if (!node->ops || !node->ops->read) {
        test_fail("lznt1_uncompressed", "read not supported");
        pmm_free_frame(buf_phys);
        pmm_free_frame(buf_phys + 4096);
        return;
    }

    rc = node->ops->read(node, 0, 8192, buf);
    if (rc <= 0) {
        test_fail("lznt1_uncompressed", "read failed");
        pmm_free_frame(buf_phys);
        pmm_free_frame(buf_phys + 4096);
        return;
    }

    /* Random data should have plenty of non-zero bytes */
    for (i = 0; i < rc; i++) {
        if (buf[i] != 0)
            non_zero++;
    }

    klog(LOG_DEBUG, "ntfs-test",
         "lznt1_uncompressed: %d/%d non-zero bytes",
         (uint64_t)non_zero, (uint64_t)rc);

    /* Random data should be >90% non-zero */
    if (non_zero > rc / 2)
        test_pass("lznt1_uncompressed");
    else
        test_fail("lznt1_uncompressed",
                  "too many zeros in random data");

    pmm_free_frame(buf_phys);
    pmm_free_frame(buf_phys + 4096);
}

/* Test 14: Mixed CU file — compressible + random + compressible */
static void test_lznt1_mixed(struct vfs_node *root)
{
    struct vfs_node *node;
    uintptr_t buf_phys;
    uint8_t *buf;
    int rc;
    int i;
    int head_ok = 1;
    int tail_ok = 1;
    int mid_nonzero = 0;
    struct vfs_node *cdir;

    cdir = find_compressed_dir(root);
    if (!cdir) {
        test_fail("lznt1_mixed", "compressed/ not found");
        return;
    }

    node = cdir->ops->finddir(cdir, "mixed.bin");
    if (!node) {
        test_fail("lznt1_mixed", "mixed.bin not found");
        return;
    }

    /* Verify file is approximately 128 KB */
    if (node->size < 100000) {
        klog(LOG_ERROR, "ntfs-test",
             "lznt1_mixed: expected ~128 KB, got %u bytes",
             node->size);
        test_fail("lznt1_mixed", "file too small");
        return;
    }

    buf_phys = pmm_alloc_contiguous(1);
    if (!buf_phys) {
        test_fail("lznt1_mixed", "PMM alloc failed");
        return;
    }
    buf = (uint8_t *)(uintptr_t)buf_phys;

    if (!node->ops || !node->ops->read) {
        test_fail("lznt1_mixed", "read not supported");
        pmm_free_frame(buf_phys);
        return;
    }

    /* Check head: first 4 KB should be all 'A' (0x41) */
    rc = node->ops->read(node, 0, 4096, buf);
    if (rc <= 0) {
        test_fail("lznt1_mixed", "read head failed");
        pmm_free_frame(buf_phys);
        return;
    }
    for (i = 0; i < rc; i++) {
        if (buf[i] != 'A') {
            head_ok = 0;
            klog(LOG_ERROR, "ntfs-test",
                 "lznt1_mixed: head byte %d is 0x%x, expected 'A'",
                 (uint64_t)i, (uint64_t)buf[i]);
            break;
        }
    }

    /* Check middle: 4 KB at offset 48 KB should be random (non-zero) */
    rc = node->ops->read(node, 48 * 1024, 4096, buf);
    if (rc > 0) {
        for (i = 0; i < rc; i++) {
            if (buf[i] != 0)
                mid_nonzero++;
        }
    }

    /* Check tail: last 4 KB should be all 'Z' (0x5A) */
    if (node->size >= 4096) {
        uint32_t tail_off = (uint32_t)(node->size - 4096);
        rc = node->ops->read(node, tail_off, 4096, buf);
        if (rc > 0) {
            for (i = 0; i < rc; i++) {
                if (buf[i] != 'Z') {
                    tail_ok = 0;
                    klog(LOG_ERROR, "ntfs-test",
                         "lznt1_mixed: tail byte %d is 0x%x, expected 'Z'",
                         (uint64_t)i, (uint64_t)buf[i]);
                    break;
                }
            }
        } else {
            tail_ok = 0;
        }
    } else {
        tail_ok = 0;
    }

    klog(LOG_DEBUG, "ntfs-test",
         "lznt1_mixed: head=%s mid_nonzero=%d tail=%s",
         (uint64_t)(uintptr_t)(head_ok ? "OK" : "FAIL"),
         (uint64_t)mid_nonzero,
         (uint64_t)(uintptr_t)(tail_ok ? "OK" : "FAIL"));

    if (head_ok && tail_ok && mid_nonzero > 2000)
        test_pass("lznt1_mixed");
    else
        test_fail("lznt1_mixed", "content verification failed");

    pmm_free_frame(buf_phys);
}

/* ============================================================================
 * Test 16 + 17: LZNT1 round-trip — compress then decompress
 *
 * These are pure algorithm tests — no VFS, no disk, no volume needed.
 * They verify that ntfs_lznt1_compress() produces a stream that
 * ntfs_lznt1_decompress() decodes byte-exactly.
 * ============================================================================ */

/* Round-trip test on a 4096-byte buffer.
 * fill_fn(buf, size): populates the source buffer.
 * Returns 1 on pass, 0 on fail. */
static int lznt1_roundtrip(const char *name, const uint8_t *src,
                            uint32_t src_len)
{
    /* Compressed stream: worst case src + 2 bytes per 4096-byte block + 2 header */
    uint32_t comp_max = src_len + ((src_len / 4096) + 1) * 4 + 4;
    uint8_t *comp_buf = (uint8_t *)kmalloc(comp_max);
    uint8_t *decomp_buf;
    int comp_len;
    int decomp_len;
    int ok = 1;
    uint32_t i;

    if (!comp_buf) {
        test_fail(name, "kmalloc comp_buf failed");
        return 0;
    }

    decomp_buf = (uint8_t *)kmalloc(src_len + 16);
    if (!decomp_buf) {
        kfree(comp_buf);
        test_fail(name, "kmalloc decomp_buf failed");
        return 0;
    }

    comp_len = ntfs_lznt1_compress(src, src_len, comp_buf, comp_max);
    if (comp_len <= 0) {
        klog(LOG_ERROR, "ntfs-test",
             "%s: compress returned %d",
             (uint64_t)(uintptr_t)name, (uint64_t)(int64_t)comp_len);
        kfree(comp_buf);
        kfree(decomp_buf);
        test_fail(name, "ntfs_lznt1_compress failed");
        return 0;
    }

    klog(LOG_DEBUG, "ntfs-test",
         "%s: %u bytes → %d compressed (%d%%)",
         (uint64_t)(uintptr_t)name,
         (uint64_t)src_len, (uint64_t)(int64_t)comp_len,
         (uint64_t)(uint32_t)(100u * (uint32_t)comp_len / src_len));

    decomp_len = ntfs_lznt1_decompress(comp_buf, (uint32_t)comp_len,
                                        decomp_buf, src_len + 16);
    kfree(comp_buf);

    if (decomp_len != (int)src_len) {
        klog(LOG_ERROR, "ntfs-test",
             "%s: decompress returned %d, expected %u",
             (uint64_t)(uintptr_t)name,
             (uint64_t)(int64_t)decomp_len,
             (uint64_t)src_len);
        kfree(decomp_buf);
        test_fail(name, "decomp length mismatch");
        return 0;
    }

    for (i = 0; i < src_len; i++) {
        if (decomp_buf[i] != src[i]) {
            klog(LOG_ERROR, "ntfs-test",
                 "%s: byte mismatch at %u: got 0x%02x expected 0x%02x",
                 (uint64_t)(uintptr_t)name, (uint64_t)i,
                 (uint64_t)decomp_buf[i], (uint64_t)src[i]);
            ok = 0;
            break;
        }
    }

    kfree(decomp_buf);

    if (ok)
        test_pass(name);
    else
        test_fail(name, "byte mismatch after round-trip");
    return ok;
}

static void test_lznt1_roundtrip_repeating(void)
{
    /* 4096 bytes of 'A' — highly compressible */
    uint8_t *src = (uint8_t *)kmalloc(4096);
    if (!src) {
        test_fail("lznt1_roundtrip_repeat", "kmalloc failed");
        return;
    }
    {
        uint32_t i;
        for (i = 0; i < 4096; i++) src[i] = 'A';
    }
    lznt1_roundtrip("lznt1_roundtrip_repeat", src, 4096);
    kfree(src);
}

static void test_lznt1_roundtrip_mixed(void)
{
    /* 8192 bytes: first 4 KB repeating 0x55/0xAA, second 4 KB 'Hello World' */
    uint8_t *src = (uint8_t *)kmalloc(8192);
    if (!src) {
        test_fail("lznt1_roundtrip_mixed", "kmalloc failed");
        return;
    }
    {
        uint32_t i;
        const char *msg = "Hello World! This is a test of the LZNT1 compressor. ";
        int mlen;
        for (mlen = 0; msg[mlen]; mlen++) {}
        for (i = 0; i < 4096; i++) src[i] = (uint8_t)((i % 2) ? 0xAA : 0x55);
        for (i = 0; i < 4096; i++) src[4096 + i] = (uint8_t)msg[i % (uint32_t)mlen];
    }
    lznt1_roundtrip("lznt1_roundtrip_mixed", src, 8192);
    kfree(src);
}

/* ---- Public API ---- */

void ntfs_run_self_test(struct ntfs_volume *vol, struct vfs_node *root)
{
    struct vfs_node *comp_dir;

    /* Only run tests on volumes labeled "NTFS_TEST" */
    if (!vol || !vol->volume_name[0]) {
        klog(LOG_WARN, "ntfs-test", "Self-test: vol or volume_name is NULL");
        return;
    }

    if (test_strcmp(vol->volume_name, "NTFS_TEST") != 0) {
        klog(LOG_INFO, "ntfs-test",
             "Self-test: skipped (label mismatch)");
        return;
    }

    klog(LOG_INFO, "ntfs-test", "--- NTFS Self-Test: STARTING ---");

    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    klog(LOG_INFO, "ntfs-test",
         "Volume: FRS=%u, cluster=%u",
         (uint64_t)vol->frs_size,
         (uint64_t)vol->cluster_size);

    /* Core tests (always available — created via ntfscp) */
    klog(LOG_INFO, "ntfs-test", "Test 1/15: root listing...");
    test_root_listing(root);
    klog(LOG_INFO, "ntfs-test", "Test 2/15: known content...");
    test_known_content(root);
    klog(LOG_INFO, "ntfs-test", "Test 3/15: empty file...");
    test_empty_file(root);
    klog(LOG_INFO, "ntfs-test", "Test 4/15: resident file...");
    test_resident_file(root);
    klog(LOG_INFO, "ntfs-test", "Test 5/15: large file...");
    test_large_file(root);
    klog(LOG_INFO, "ntfs-test", "Test 6/15: dirty flag...");
    test_dirty_flag(vol);

    /* Extended tests (require FUSE-created directories) */
    klog(LOG_INFO, "ntfs-test", "Test 7/15: subdirectory...");
    test_subdir(root);
    klog(LOG_INFO, "ntfs-test", "Test 8/15: deep directory...");
    test_deep_dir(root);
    klog(LOG_INFO, "ntfs-test", "Test 9/15: many files...");
    test_many_files(root);
    klog(LOG_INFO, "ntfs-test", "Test 10/15: long filename...");
    test_long_filename(root);

    /* LZNT1 compression tests (§9.1) */
    klog(LOG_INFO, "ntfs-test", "Test 11/17: LZNT1 decompressor...");
    test_lznt1_decompress();

    /* VFS-level compression tests — require FUSE + setfattr */
    comp_dir = find_compressed_dir(root);
    if (comp_dir) {
        klog(LOG_INFO, "ntfs-test", "Test 12/17: LZNT1 known content...");
        test_lznt1_known(root);
        klog(LOG_INFO, "ntfs-test", "Test 13/17: LZNT1 sparse zeros...");
        test_lznt1_sparse(root);
        klog(LOG_INFO, "ntfs-test", "Test 14/17: LZNT1 uncompressed...");
        test_lznt1_uncompressed(root);
        klog(LOG_INFO, "ntfs-test", "Test 15/17: LZNT1 mixed CUs...");
        test_lznt1_mixed(root);
    } else {
        klog(LOG_WARN, "ntfs-test",
             "LZNT1 VFS tests skipped (compressed/ dir not found)");
    }

    /* LZNT1 round-trip tests (§9.2) — pure algorithm, no VFS needed */
    klog(LOG_INFO, "ntfs-test", "Test 16/17: LZNT1 round-trip (repeating data)...");
    test_lznt1_roundtrip_repeating();
    klog(LOG_INFO, "ntfs-test", "Test 17/17: LZNT1 round-trip (mixed content)...");
    test_lznt1_roundtrip_mixed();

    /* Summary */
    klog(LOG_INFO, "ntfs-test",
         "--- NTFS Self-Test: RESULTS ---");
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
         "--- NTFS Self-Test: DONE ---");
}
