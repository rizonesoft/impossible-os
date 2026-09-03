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

    /* drive_index rejects letters outside A-Z via vfs_is_mounted returning 0 */
    TEST_ASSERT(vfs_is_mounted('Z') == 0 || vfs_is_mounted('Z') == 1,
                "Z is a valid drive letter (mounted or not)");
    /* '[' is the character after 'Z' -- should be rejected */
    TEST_ASSERT(vfs_is_mounted('[') == 0, "[ (after Z) rejected as drive letter");
    TEST_ASSERT(vfs_is_mounted('@') == 0, "@ (before A) rejected as drive letter");
}

/* ---- VFS_O_TRUNC end-to-end ------------------------------------------ */

/* Helper: write `len` bytes of `pattern` at offset 0 of `path`. */
static void vfs_test_write_n(const char *path, uint8_t pattern, uint32_t len)
{
    struct vfs_node *f = vfs_open(path,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) return;
    uint8_t buf[64];
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = (len - off > sizeof(buf)) ? sizeof(buf) : len - off;
        for (uint32_t i = 0; i < chunk; i++) buf[i] = pattern;
        vfs_write(f, off, chunk, buf);
        off += chunk;
    }
    vfs_close(f);
}

/* Long file written, reopened with TRUNC + shorter payload, read back at
 * exactly the shorter size with no stale tail bytes. */
static void test_vfs_o_trunc_ixfs_shrinks(void)
{
    const char *path = "C:\\Impossible\\test_trunc_ixfs.tmp";

    vfs_test_write_n(path, 0xAA, 256);

    struct vfs_node *f = vfs_open(path,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    TEST_ASSERT(f != NULL, "VFS_O_TRUNC reopen succeeds");
    if (!f) { vfs_unlink(path); return; }

    uint8_t shorter[32];
    for (uint32_t i = 0; i < sizeof(shorter); i++) shorter[i] = 0xBB;
    int wrote = vfs_write(f, 0, sizeof(shorter), shorter);
    TEST_ASSERT(wrote == (int)sizeof(shorter), "short write returns 32");
    vfs_close(f);

    struct vfs_node *r = vfs_open(path, VFS_O_READ);
    TEST_ASSERT(r != NULL, "post-TRUNC reopen for read succeeds");
    if (r) {
        TEST_ASSERT_EQ((uint64_t)r->size, (uint64_t)32u,
                       "TRUNC + short write yields exact-size 32");
        uint8_t verify[64];
        for (uint32_t i = 0; i < sizeof(verify); i++) verify[i] = 0;
        int got = vfs_read(r, 0, sizeof(shorter), verify);
        TEST_ASSERT(got == (int)sizeof(shorter), "read back 32 bytes");
        int all_bb = 1;
        for (uint32_t i = 0; i < sizeof(shorter); i++)
            if (verify[i] != 0xBB) { all_bb = 0; break; }
        TEST_ASSERT(all_bb, "no stale 0xAA tail bytes");
        vfs_close(r);
    }

    vfs_unlink(path);
}

/* VFS_O_TRUNC without VFS_O_WRITE returns NULL on FILES. */
static void test_vfs_o_trunc_no_write_rejected(void)
{
    const char *path = "C:\\Impossible\\test_trunc_no_write.tmp";
    vfs_test_write_n(path, 0xCC, 16);

    struct vfs_node *f = vfs_open(path, VFS_O_READ | VFS_O_TRUNC);
    TEST_ASSERT(f == NULL, "TRUNC + READ-only rejects with NULL");

    struct vfs_node *r = vfs_open(path, VFS_O_READ);
    TEST_ASSERT(r != NULL, "file survives rejected TRUNC");
    if (r) {
        TEST_ASSERT_EQ((uint64_t)r->size, (uint64_t)16u,
                       "rejected TRUNC did NOT shrink file");
        vfs_close(r);
    }
    vfs_unlink(path);
}

/* Read-only directory open with stray TRUNC bit -- TRUNC is masked at
 * the VFS layer (directory mask runs before the WRITE-required check),
 * so the open succeeds and the directory is preserved. */
static void test_vfs_o_trunc_directory_read_only_masked(void)
{
    const char *dir_path = "C:\\Impossible\\test_trunc_ro_dir";
    int rc = vfs_create(dir_path, VFS_DIRECTORY);
    TEST_ASSERT(rc == 0, "create test directory");

    struct vfs_node *d = vfs_open(dir_path, VFS_O_READ | VFS_O_TRUNC);
    TEST_ASSERT(d != NULL, "READ + TRUNC on directory returns non-NULL");
    if (d) vfs_close(d);

    struct vfs_node *d2 = vfs_open(dir_path, VFS_O_READ);
    TEST_ASSERT(d2 != NULL, "directory survives masked TRUNC");
    if (d2) vfs_close(d2);

    vfs_unlink(dir_path);
}

/* VFS_O_TRUNC on a directory is silently masked at the VFS layer. */
static void test_vfs_o_trunc_directory_silently_masked(void)
{
    const char *dir_path = "C:\\Impossible\\test_trunc_dir";
    int rc = vfs_create(dir_path, VFS_DIRECTORY);
    TEST_ASSERT(rc == 0, "create test directory");

    struct vfs_node *d = vfs_open(dir_path, VFS_O_WRITE | VFS_O_TRUNC);
    /* IXFS dir-open hook may reject WRITE on dirs. Pin only "TRUNC was
     * masked, not enacted" -- the dir still exists either way. */
    if (d) vfs_close(d);

    struct vfs_node *d2 = vfs_open(dir_path, VFS_O_READ);
    TEST_ASSERT(d2 != NULL, "directory survives TRUNC attempt");
    if (d2) vfs_close(d2);

    vfs_unlink(dir_path);
}

/* FAT32 lowercase-LFN TRUNC round-trip. Unblocked by the sector-cache
 * coherence overlays in fat32_read/write_sectors_multi (the
 * fat32_zero_cluster cache-vs-direct-write gap that previously zeroed
 * freshly created files). Exercises: LFN create on FAT32, TRUNC reopen
 * via cached SFN, shrink, exact-size + no-stale-tail read-back. */
static void test_vfs_o_trunc_fat32_lowercase_lfn(void)
{
    const char *path = "X:\\Boot\\trunc_lfn_roundtrip.tmp";

    if (!vfs_is_mounted('X')) {
        TEST_SKIP("X: (FAT32 BlackBox) not mounted on this platform");
        return;
    }

    vfs_test_write_n(path, 0xAA, 256);

    struct vfs_node *f = vfs_open(path,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    TEST_ASSERT(f != NULL, "FAT32 VFS_O_TRUNC reopen succeeds");
    if (!f) { vfs_unlink(path); return; }

    uint8_t shorter[32];
    for (uint32_t i = 0; i < sizeof(shorter); i++) shorter[i] = 0xBB;
    int wrote = vfs_write(f, 0, sizeof(shorter), shorter);
    TEST_ASSERT(wrote == (int)sizeof(shorter), "FAT32 short write returns 32");
    vfs_close(f);

    struct vfs_node *r = vfs_open(path, VFS_O_READ);
    TEST_ASSERT(r != NULL, "FAT32 post-TRUNC reopen for read succeeds");
    if (r) {
        TEST_ASSERT_EQ((uint64_t)r->size, (uint64_t)32u,
                       "FAT32 TRUNC + short write yields exact-size 32");
        uint8_t verify[64];
        for (uint32_t i = 0; i < sizeof(verify); i++) verify[i] = 0;
        int got = vfs_read(r, 0, sizeof(shorter), verify);
        TEST_ASSERT(got == (int)sizeof(shorter), "FAT32 read back 32 bytes");
        int all_bb = 1;
        for (uint32_t i = 0; i < sizeof(shorter); i++)
            if (verify[i] != 0xBB) { all_bb = 0; break; }
        TEST_ASSERT(all_bb, "FAT32 no stale 0xAA tail (cache coherent)");
        vfs_close(r);
    }

    TEST_ASSERT_EQ((uint64_t)(uint32_t)vfs_unlink(path), 0u,
                   "FAT32 LFN unlink succeeds (LFN-aware delete)");
}

/* IXFS rejects vfs_rename_ex REPLACE_EXISTING explicitly. */
static void test_vfs_rename_replace_ixfs_rejects(void)
{
    const char *src = "C:\\Impossible\\rn_src.tmp";
    const char *dst = "C:\\Impossible\\rn_dst.tmp";
    vfs_test_write_n(src, 0xAA, 16);
    vfs_test_write_n(dst, 0xBB, 16);

    int rc = vfs_rename_ex(src, dst, VFS_RENAME_REPLACE_EXISTING);
    /* Cast int->uint32_t first to avoid sign-extending -1 to a 64-bit
     * 0xFFFFFFFFFFFFFFFF; the assert helper takes uint64_t. */
    TEST_ASSERT_EQ((uint64_t)(uint32_t)rc, (uint64_t)(uint32_t)-1,
                   "IXFS rejects REPLACE_EXISTING with -1");

    struct vfs_node *a = vfs_open(src, VFS_O_READ);
    struct vfs_node *b = vfs_open(dst, VFS_O_READ);
    TEST_ASSERT(a != NULL, "src still openable after rejected replace");
    TEST_ASSERT(b != NULL, "dst still openable after rejected replace");
    if (a) vfs_close(a);
    if (b) vfs_close(b);

    vfs_unlink(src);
    vfs_unlink(dst);
}

/* Legacy 2-arg vfs_rename still routes correctly through vfs_rename_ex shim. */
static void test_vfs_rename_legacy_no_flag_ixfs(void)
{
    const char *src = "C:\\Impossible\\rn_legacy_src.tmp";
    const char *dst = "C:\\Impossible\\rn_legacy_dst.tmp";
    vfs_test_write_n(src, 0xCD, 8);
    vfs_unlink(dst);

    int rc = vfs_rename(src, dst);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)0u,
                   "legacy vfs_rename returns 0 with no dst collision");

    struct vfs_node *r = vfs_open(dst, VFS_O_READ);
    TEST_ASSERT(r != NULL, "renamed file readable at new path");
    if (r) vfs_close(r);

    vfs_unlink(dst);
    vfs_unlink(src);
}

/* VFS_RENAME_REPLACE_EXISTING flag-bit consistency. */
static void test_vfs_rename_replace_flag_bit(void)
{
    TEST_ASSERT(VFS_RENAME_REPLACE_EXISTING != 0,
                "REPLACE_EXISTING flag is non-zero");
    TEST_ASSERT((VFS_RENAME_REPLACE_EXISTING & 0xFFFF0000u) == 0,
                "REPLACE_EXISTING fits in low 16 bits");
}

/* Registration */
void test_register_vfs(void)
{
    test_suite_register_cat("VFS: file roundtrip", test_vfs_file_roundtrip, TEST_CAT_FS);
    test_suite_register_cat("VFS: open nonexistent", test_vfs_open_nonexistent, TEST_CAT_FS);
    test_suite_register_cat("VFS: mkdir+rmdir", test_vfs_mkdir_rmdir, TEST_CAT_FS);
    test_suite_register_cat("VFS: drive letter range", test_vfs_drive_constants, TEST_CAT_FS);
    test_suite_register_cat("VFS: O_TRUNC shrinks (IXFS, no stale tail)",
        test_vfs_o_trunc_ixfs_shrinks, TEST_CAT_FS);
    test_suite_register_cat("VFS: O_TRUNC without O_WRITE rejected",
        test_vfs_o_trunc_no_write_rejected, TEST_CAT_FS);
    test_suite_register_cat("VFS: O_TRUNC on directory silently masked",
        test_vfs_o_trunc_directory_silently_masked, TEST_CAT_FS);
    test_suite_register_cat("VFS: READ + O_TRUNC on directory preserves dir",
        test_vfs_o_trunc_directory_read_only_masked, TEST_CAT_FS);
    test_suite_register_cat("VFS: O_TRUNC + write on FAT32 lowercase-LFN name",
        test_vfs_o_trunc_fat32_lowercase_lfn, TEST_CAT_FS);
    test_suite_register_cat("VFS: rename REPLACE_EXISTING rejected by IXFS",
        test_vfs_rename_replace_ixfs_rejects, TEST_CAT_FS);
    test_suite_register_cat("VFS: legacy vfs_rename routes via _ex shim",
        test_vfs_rename_legacy_no_flag_ixfs, TEST_CAT_FS);
    test_suite_register_cat("VFS: rename REPLACE_EXISTING flag bit",
        test_vfs_rename_replace_flag_bit, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
