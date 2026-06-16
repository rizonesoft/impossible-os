/* ============================================================================
 * test_ixfs_fsck.c -- IXFS fsck pure-validator unit tests
 *
 * Exercises the I/O-free fsck validators on crafted in-memory structures:
 *   - ixfs_fsck_check_superblock  (magic / version / layout / CRC32C)
 *   - ixfs_fsck_reconcile_bitmap  (detect + repair bitmap inconsistency)
 *   - ixfs_fsck_journal_entry_valid (target range + payload checksum)
 *
 * The orchestrator passes (orphan detection, free-count fix, journal replay)
 * operate on a mounted volume and are validated by the recovery flow on the
 * serial log; unit tests must not stand up live boot/mount infrastructure.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/ixfs.h"
#include "kernel/types.h"

/* ixfs_internal.h carries ixfs_crc32c() + ixfs_journal_checksum() + the
 * journal structs. Mirrors test_ixfs.c including fat32_internal.h for the
 * volume struct -- internal headers are kernel-side and test-includable. */
#include "../fs/ixfs/ixfs_internal.h"

/* Build a well-formed superblock with a correct CRC32C. */
static void fsck_make_valid_sb(struct ixfs_superblock *sb)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)sb;
    for (i = 0; i < sizeof(*sb); i++)
        p[i] = 0;
    sb->s_magic       = IXFS_MAGIC;
    sb->s_version     = IXFS_VERSION;
    sb->s_block_size  = IXFS_BLOCK_SIZE;
    sb->s_total_blocks = 1000;
    sb->s_free_blocks = 900;
    sb->s_total_inodes = 256;
    sb->s_free_inodes = 255;
    sb->s_bitmap_start = 1;
    sb->s_bitmap_blocks = 1;
    sb->s_inode_start = 2;
    sb->s_inode_blocks = 8;
    sb->s_data_start = 64;
    sb->s_root_inode = IXFS_ROOT_INODE;
    sb->s_checksum = ixfs_crc32c(sb, 112);
}

static void test_fsck_sb_clean(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    TEST_ASSERT_EQ(ixfs_fsck_check_superblock(&sb), 0,
                   "clean superblock passes validation");
}

static void test_fsck_sb_bad_magic(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_magic = 0xDEADBEEF;            /* CRC still old -> magic + CRC fault */
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "bad magic detected");
}

static void test_fsck_sb_bad_layout(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_inode_start = 1;              /* bitmap(1) not < inode(1): bad order */
    sb.s_checksum = ixfs_crc32c(&sb, 112); /* fix CRC so only layout faults */
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "bad layout ordering detected");
}

static void test_fsck_sb_bad_crc(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_checksum = sb.s_checksum ^ 0xFFFFFFFFu; /* corrupt only the CRC */
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "bad CRC32C detected");
}

static void test_fsck_sb_data_past_end(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_data_start = 2000;            /* > s_total_blocks (1000) */
    sb.s_checksum = ixfs_crc32c(&sb, 112);
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "data region past end-of-volume detected");
}

static void test_fsck_sb_null(void)
{
    TEST_ASSERT(ixfs_fsck_check_superblock((const struct ixfs_superblock *)0) >= 1,
                "NULL superblock rejected");
}

static void test_fsck_sb_version_zero(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_version = 0;
    sb.s_checksum = ixfs_crc32c(&sb, 112);
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "version 0 rejected");
}

static void test_fsck_sb_version_too_new(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_version = IXFS_VERSION + 1;
    sb.s_checksum = ixfs_crc32c(&sb, 112);
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "version newer than IXFS_VERSION rejected");
}

static void test_fsck_sb_bad_block_size(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_block_size = 512;             /* not IXFS_BLOCK_SIZE (4096) */
    sb.s_checksum = ixfs_crc32c(&sb, 112);
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "wrong block size rejected");
}

static void test_fsck_sb_zero_total_blocks(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_total_blocks = 0;
    sb.s_checksum = ixfs_crc32c(&sb, 112);
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "total_blocks==0 rejected");
}

static void test_fsck_sb_bad_root_inode(void)
{
    struct ixfs_superblock sb;
    fsck_make_valid_sb(&sb);
    sb.s_root_inode = 5;               /* must be IXFS_ROOT_INODE (1) */
    sb.s_checksum = ixfs_crc32c(&sb, 112);
    TEST_ASSERT(ixfs_fsck_check_superblock(&sb) >= 1,
                "non-1 root inode rejected");
}

static void test_fsck_bitmap_null_inputs(void)
{
    uint8_t buf[8];
    uint32_t i;
    for (i = 0; i < 8; i++)
        buf[i] = 0;
    TEST_ASSERT_EQ(ixfs_fsck_reconcile_bitmap((const uint8_t *)0, buf, 64, 0), 0,
                   "NULL expected: no work, returns 0");
    TEST_ASSERT_EQ(ixfs_fsck_reconcile_bitmap(buf, (uint8_t *)0, 64, 0), 0,
                   "NULL actual: no work, returns 0");
}

static void test_fsck_bitmap_zero_nbits(void)
{
    uint8_t exp[1] = { 0xFF }, act[1] = { 0x00 };
    TEST_ASSERT_EQ(ixfs_fsck_reconcile_bitmap(exp, act, 0, 1), 0,
                   "nbits==0: scans nothing, returns 0");
    TEST_ASSERT_EQ(act[0], 0x00, "nbits==0: actual untouched");
}

static void test_fsck_bitmap_partial_byte_ignores_padding(void)
{
    /* nbits=5: only bits 0..4 are reconciled; bits 5..7 are padding and
     * must be ignored even when they differ. */
    uint8_t exp[1] = { 0x00 };         /* bits 0..4 all clear */
    uint8_t act[1] = { 0xE0 };         /* bits 5,6,7 set (padding only) */
    TEST_ASSERT_EQ(ixfs_fsck_reconcile_bitmap(exp, act, 5, 1), 0,
                   "padding bits beyond nbits are ignored");
    TEST_ASSERT_EQ(act[0], 0xE0, "fix mode left padding bits untouched");
}

static void test_fsck_bitmap_clean(void)
{
    uint8_t exp[8], act[8];
    uint32_t i;
    for (i = 0; i < 8; i++) { exp[i] = 0xA5; act[i] = 0xA5; }
    TEST_ASSERT_EQ(ixfs_fsck_reconcile_bitmap(exp, act, 64, 0), 0,
                   "identical bitmaps: zero mismatches");
}

static void test_fsck_bitmap_detect(void)
{
    uint8_t exp[8], act[8];
    uint32_t i;
    for (i = 0; i < 8; i++) { exp[i] = 0x00; act[i] = 0x00; }
    /* expected says block 5 used; on-disk bitmap says free */
    exp[0] = 0x20;                     /* bit 5 set */
    TEST_ASSERT_EQ(ixfs_fsck_reconcile_bitmap(exp, act, 64, 0), 1,
                   "one-bit bitmap inconsistency detected (no repair)");
    TEST_ASSERT_EQ(act[0], 0x00,
                   "check-only mode left the bitmap unmodified");
}

static void test_fsck_bitmap_repair(void)
{
    uint8_t exp[8], act[8];
    uint32_t i;
    for (i = 0; i < 8; i++) { exp[i] = 0x00; act[i] = 0x00; }
    exp[0] = 0x20;                     /* expected: block 5 used */
    act[3] = 0x01;                     /* on-disk: spurious block-24 used bit */
    /* Two mismatches: block 5 (missing) + block 24 (spurious). */
    TEST_ASSERT_EQ(ixfs_fsck_reconcile_bitmap(exp, act, 64, 1), 2,
                   "two bitmap mismatches detected");
    TEST_ASSERT_EQ(act[0], 0x20, "repair set the missing used bit");
    TEST_ASSERT_EQ(act[3], 0x00, "repair cleared the spurious used bit");
}

static void test_fsck_journal_valid_data(void)
{
    uint8_t data[4080];
    uint32_t i, csum;
    for (i = 0; i < 4080; i++)
        data[i] = (uint8_t)(i & 0xFF);
    csum = ixfs_journal_checksum(data, 4080);
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_DATA, 100, csum,
                                                 data, 4080, 1000), 1,
                   "well-formed DATA entry is valid");
}

static void test_fsck_journal_commit(void)
{
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_COMMIT, 0, 0,
                                                 (const uint8_t *)0, 0, 1000), 1,
                   "COMMIT record is valid (no payload)");
}

static void test_fsck_journal_bad_target(void)
{
    uint8_t data[4080];
    uint32_t i, csum;
    for (i = 0; i < 4080; i++)
        data[i] = (uint8_t)(i & 0xFF);
    csum = ixfs_journal_checksum(data, 4080);
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_DATA, 5000, csum,
                                                 data, 4080, 1000), 0,
                   "DATA entry with out-of-range target rejected");
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_DATA, 0, csum,
                                                 data, 4080, 1000), 0,
                   "DATA entry with target 0 rejected");
}

static void test_fsck_journal_bad_checksum(void)
{
    uint8_t data[4080];
    uint32_t i, csum;
    for (i = 0; i < 4080; i++)
        data[i] = (uint8_t)(i & 0xFF);
    csum = ixfs_journal_checksum(data, 4080) + 1; /* wrong */
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_DATA, 100, csum,
                                                 data, 4080, 1000), 0,
                   "DATA entry with bad payload checksum rejected");
}

static void test_fsck_journal_unknown_type(void)
{
    uint8_t data[1] = { 0 };
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(99, 100, 0, data, 1, 1000), 0,
                   "unknown journal entry type rejected");
}

static void test_fsck_journal_null_payload(void)
{
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_DATA, 100, 0,
                                                 (const uint8_t *)0, 4080, 1000), 0,
                   "DATA entry with NULL payload rejected");
}

static void test_fsck_journal_zero_len(void)
{
    uint8_t data[1] = { 0 };
    /* A real DATA entry always carries payload; len 0 is malformed even
     * though checksum(data,0)==0 would otherwise match a 0 checksum. */
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_DATA, 100, 0,
                                                 data, 0, 1000), 0,
                   "DATA entry with zero-length payload rejected");
}

static void test_fsck_journal_zero_total_blocks(void)
{
    uint8_t data[4080];
    uint32_t i, csum;
    for (i = 0; i < 4080; i++)
        data[i] = (uint8_t)(i & 0xFF);
    csum = ixfs_journal_checksum(data, 4080);
    /* total_blocks==0 makes every target out of range. */
    TEST_ASSERT_EQ(ixfs_fsck_journal_entry_valid(IXFS_JE_DATA, 1, csum,
                                                 data, 4080, 0), 0,
                   "DATA entry rejected when total_blocks==0");
}

static void test_fsck_get_block_clamps_extent_count(void)
{
    /* A corrupt on-disk inode with i_extent_count past the inline array
     * must not over-read; ixfs_get_block clamps to IXFS_INLINE_EXTENTS.
     * vol is unused for inline extents, so NULL is safe here. */
    struct ixfs_inode inode;
    uint8_t *p = (uint8_t *)&inode;
    uint32_t i;
    for (i = 0; i < sizeof(inode); i++)
        p[i] = 0;
    inode.i_mode = IXFS_S_FILE;
    inode.i_extent_count = 99;          /* corrupt: array holds only 4 */
    inode.i_extent_flags = 0;
    inode.i_extents[0].e_start = 64;
    inode.i_extents[0].e_count = 1;
    TEST_ASSERT_EQ(ixfs_get_block((struct ixfs_volume *)0, &inode, 0), 64,
                   "clamped extent walk returns first mapped block");
    TEST_ASSERT_EQ(ixfs_get_block((struct ixfs_volume *)0, &inode, 50), 0,
                   "index beyond clamped extents returns 0 (no over-read)");
}

void test_register_ixfs_fsck(void)
{
    test_suite_register_cat("IXFS fsck: superblock clean",
                            test_fsck_sb_clean, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bad magic",
                            test_fsck_sb_bad_magic, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bad layout order",
                            test_fsck_sb_bad_layout, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bad CRC32C",
                            test_fsck_sb_bad_crc, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: data past end",
                            test_fsck_sb_data_past_end, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: NULL superblock",
                            test_fsck_sb_null, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: version 0",
                            test_fsck_sb_version_zero, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: version too new",
                            test_fsck_sb_version_too_new, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bad block size",
                            test_fsck_sb_bad_block_size, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: zero total blocks",
                            test_fsck_sb_zero_total_blocks, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bad root inode",
                            test_fsck_sb_bad_root_inode, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bitmap NULL inputs",
                            test_fsck_bitmap_null_inputs, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bitmap zero nbits",
                            test_fsck_bitmap_zero_nbits, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bitmap partial-byte padding",
                            test_fsck_bitmap_partial_byte_ignores_padding, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bitmap clean",
                            test_fsck_bitmap_clean, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bitmap inconsistency detected",
                            test_fsck_bitmap_detect, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: bitmap repair",
                            test_fsck_bitmap_repair, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal valid DATA",
                            test_fsck_journal_valid_data, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal COMMIT",
                            test_fsck_journal_commit, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal bad target",
                            test_fsck_journal_bad_target, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal bad checksum",
                            test_fsck_journal_bad_checksum, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal unknown type",
                            test_fsck_journal_unknown_type, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal NULL payload",
                            test_fsck_journal_null_payload, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal zero-length DATA",
                            test_fsck_journal_zero_len, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: journal zero total blocks",
                            test_fsck_journal_zero_total_blocks, TEST_CAT_FS);
    test_suite_register_cat("IXFS fsck: get_block clamps extent count",
                            test_fsck_get_block_clamps_extent_count, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
