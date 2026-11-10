/* Unit tests for the NTFS index-entry parser over in-memory fixtures.
 *
 * The directory walk reads the decoded $FILE_NAME of every non-sentinel entry,
 * so the parser must leave no field of it unwritten whatever the entry's
 * stream length says. No disk: the parsers are pure functions of the bytes,
 * and the one volume here is a zeroed struct over a device that fails reads. */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/ntfs.h"
#include "kernel/drivers/blkdev.h"

/* An index entry header with the given stream length and flags, no payload. */
static void ntfs_entry_fixture(uint8_t e[0x60], uint16_t stream_len, uint8_t flags)
{
    for (uint32_t i = 0; i < 0x60u; i++)
        e[i] = 0xEEu;                       /* poison the payload area */
    for (uint32_t i = 0; i < 8u; i++)
        e[i] = (uint8_t)(0x10u + i);        /* MFT reference */
    e[0x08] = 0x60; e[0x09] = 0x00;         /* entry_length */
    e[0x0A] = (uint8_t)stream_len;
    e[0x0B] = (uint8_t)(stream_len >> 8);
    e[0x0C] = flags;
}

static int ntfs_fn_is_zero(const struct ntfs_file_name *fn)
{
    const uint8_t *p = (const uint8_t *)fn;
    for (uint32_t i = 0; i < (uint32_t)sizeof *fn; i++)
        if (p[i] != 0u)
            return 0;
    return 1;
}

static void ntfs_entry_poison(struct ntfs_index_entry *ie)
{
    uint8_t *p = (uint8_t *)ie;
    for (uint32_t i = 0; i < (uint32_t)sizeof *ie; i++)
        p[i] = 0xA5u;
}

/* A non-sentinel entry whose stream is shorter than a $FILE_NAME header
 * decodes to an all-zero name record, never the caller's stale bytes. */
static void test_ntfs_index_entry_short_stream(void)
{
    static uint8_t e[0x60];
    static struct ntfs_index_entry ie;
    struct ntfs_index_node_header nh = { 0u, 0x60u, 0x60u, 0u };

    ntfs_entry_fixture(e, 0x10u, 0u);
    ntfs_entry_poison(&ie);
    TEST_ASSERT(ntfs_index_entry_first(e, &nh, &ie) == e,
                "short-stream entry still parses");
    TEST_ASSERT_EQ(ie.stream_length, 0x10u, "stream length decoded");
    TEST_ASSERT(ntfs_fn_is_zero(&ie.fn), "short stream leaves a zeroed name record");

    ntfs_entry_fixture(e, 0u, 0u);
    ntfs_entry_poison(&ie);
    TEST_ASSERT(ntfs_index_entry_first(e, &nh, &ie) == e, "empty-stream entry parses");
    TEST_ASSERT(ntfs_fn_is_zero(&ie.fn), "empty stream leaves a zeroed name record");
}

/* The sentinel carries no name: its record is zeroed in full. */
static void test_ntfs_index_entry_sentinel(void)
{
    static uint8_t e[0x60];
    static struct ntfs_index_entry ie;
    struct ntfs_index_node_header nh = { 0u, 0x60u, 0x60u, 0u };

    ntfs_entry_fixture(e, 0x52u, NTFS_INDEX_ENTRY_LAST);
    ntfs_entry_poison(&ie);
    TEST_ASSERT(ntfs_index_entry_first(e, &nh, &ie) == e, "sentinel parses");
    TEST_ASSERT(ntfs_fn_is_zero(&ie.fn), "sentinel has a zeroed name record");
}

/* A block device that counts reads and fails every one. */
static uint32_t g_ntfs_fixture_reads;

static int ntfs_fixture_read(uint64_t lba, uint32_t count, void *buf, void *ctx)
{
    (void)lba; (void)count; (void)buf; (void)ctx;
    g_ntfs_fixture_reads++;
    return -1;
}

/* Base record 0 with one resident $ATTRIBUTE_LIST of two entries, both living
 * in extension record 5: an unnamed $DATA and an $INDEX_ROOT named "$I30". */
static void ntfs_attr_list_record(uint8_t rec[0x100], struct ntfs_mft_header *hdr)
{
    static const char name[4] = { '$', 'I', '3', '0' };
    uint8_t *al = rec + 0x38;
    uint8_t *e;

    for (uint32_t i = 0; i < 0x100u; i++)
        rec[i] = 0u;
    al[0x00] = (uint8_t)NTFS_ATTR_ATTRIBUTE_LIST;    /* type */
    al[0x04] = 0x60;                                 /* total_length */
    al[0x10] = 0x48;                                 /* content_length */
    al[0x14] = 0x18;                                 /* content_offset */

    e = al + 0x18;                                   /* entry 1: unnamed $DATA */
    e[0x00] = (uint8_t)NTFS_ATTR_DATA;
    e[0x04] = 0x20;                                  /* entry_length */
    e[0x07] = 0x1A;                                  /* name_offset */
    e[0x10] = 5;                                     /* MFT reference: record 5 */

    e = al + 0x38;                                   /* entry 2: $INDEX_ROOT "$I30" */
    e[0x00] = (uint8_t)NTFS_ATTR_INDEX_ROOT;
    e[0x04] = 0x28;
    e[0x06] = 4;                                     /* name_length */
    e[0x07] = 0x1A;
    e[0x10] = 5;
    for (uint32_t i = 0; i < 4u; i++)
        e[0x1A + 2u * i] = (uint8_t)name[i];         /* UTF-16LE */

    rec[0x98] = 0xFF; rec[0x99] = 0xFF; rec[0x9A] = 0xFF; rec[0x9B] = 0xFF;  /* $END */
    for (uint32_t i = 0; i < (uint32_t)sizeof *hdr; i++)
        ((uint8_t *)hdr)[i] = 0u;
    hdr->attrs_offset = 0x38;
    hdr->used_size = 0xA0;
    hdr->alloc_size = 0x100;
}

/* An extension-record hit hands the caller a frame through ext_record, so a
 * caller that passes none is answered from the base record alone: the volume
 * is never read. The same lookups WITH an ext_record do reach the device,
 * which is the control showing the fixture leads to the extension read. */
static void test_ntfs_attr_find_ext_without_owner(void)
{
    static uint8_t rec[0x100];
    static struct blkdev dev;
    static struct ntfs_volume vol;
    struct ntfs_mft_header hdr, ehdr;
    struct ntfs_attr_header ah;
    uintptr_t ext = 0;

    dev.sector_size = 512;
    dev.read = ntfs_fixture_read;
    dev.active = 1;
    vol.dev = &dev;
    vol.bytes_per_sector = 512;
    vol.frs_size = 1024;
    ntfs_attr_list_record(rec, &hdr);
    TEST_ASSERT(ntfs_attr_find(rec, &hdr, NTFS_ATTR_ATTRIBUTE_LIST, &ah) == rec + 0x38,
                "fixture holds a parseable attribute list");

    g_ntfs_fixture_reads = 0;
    TEST_ASSERT(ntfs_attr_find_ext(&vol, rec, &hdr, NTFS_ATTR_DATA, &ah, NULL, NULL) == NULL,
                "find_ext: base miss without ext_record is not found");
    TEST_ASSERT(ntfs_attr_find_named_ext(&vol, rec, &hdr, NTFS_ATTR_INDEX_ROOT, "$I30", &ah,
                                         NULL, NULL) == NULL,
                "find_named_ext: base miss without ext_record is not found");
    TEST_ASSERT_EQ(g_ntfs_fixture_reads, 0u, "no extension record is read without ext_record");

    TEST_ASSERT(ntfs_attr_find_ext(&vol, rec, &hdr, NTFS_ATTR_ATTRIBUTE_LIST, &ah,
                                   NULL, NULL) == rec + 0x38,
                "find_ext: base hit needs no ext_record");

    /* Control: with an owner the same two lookups go to the device. */
    TEST_ASSERT(ntfs_attr_find_ext(&vol, rec, &hdr, NTFS_ATTR_DATA, &ah, &ext, &ehdr) == NULL,
                "find_ext: unreadable extension record is not found");
    TEST_ASSERT_EQ(g_ntfs_fixture_reads, 1u, "find_ext with ext_record reads the extension record");
    TEST_ASSERT(ntfs_attr_find_named_ext(&vol, rec, &hdr, NTFS_ATTR_INDEX_ROOT, "$I30", &ah,
                                         &ext, &ehdr) == NULL,
                "find_named_ext: unreadable extension record is not found");
    TEST_ASSERT_EQ(g_ntfs_fixture_reads, 2u,
                   "find_named_ext with ext_record reads the extension record");
    TEST_ASSERT(ext == 0, "a failed extension read hands back no frame");
}

void test_register_ntfs_parse(void)
{
    test_suite_register_cat("ntfs: extension lookup without an owner",
                            test_ntfs_attr_find_ext_without_owner, TEST_CAT_FS);
    test_suite_register_cat("ntfs: index entry short stream",
                            test_ntfs_index_entry_short_stream, TEST_CAT_FS);
    test_suite_register_cat("ntfs: index entry sentinel",
                            test_ntfs_index_entry_sentinel, TEST_CAT_FS);
}

#endif /* KERNEL_TESTS */
