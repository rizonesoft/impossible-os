/* ============================================================================
 * test_smbios_parse.c -- pure SMBIOS Type 1 walker + UUID formatter tests
 *
 * Owns kernel-side coverage of the pure helpers shipped in
 * include/boot/boot_smbios_parse.h. The bootloader includes the same
 * header pre-EBS for machine_id extraction + LoaderDevicePartUUID
 * formatting (boot-entry loader-vars feature). The pure walker + UUID
 * formatter are stateless / no UEFI types, so this test file can
 * include the header directly and exercise them without touching any
 * live boot infrastructure (per the test-policy ban).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/types.h"
#include "boot/boot_smbios_parse.h"

extern void *memset(void *dst, int c, size_t n);

/* ---- UUID formatter ---------------------------------------------------- */

static void test_uuid_format_basic(void)
{
    /* Canonical SMBIOS Type 1 UUID bytes for a known textual form.
     * Per SMBIOS 2.6+ the first three fields are LE-on-wire and must
     * be byte-swapped for RFC 4122 text output. Source bytes
     * 01 02 03 04 | 05 06 | 07 08 | 09 0A | 0B 0C 0D 0E 0F 10
     * format to:
     * Data1 (LE swap): 04030201
     * Data2 (LE swap): 0605
     * Data3 (LE swap): 0807
     * Data4 (big-endian): 090a-0b0c0d0e0f10 */
    unsigned char src[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
    };
    char buf[37];
    int ok = boot_smbios_format_uuid(src, buf);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "non-sentinel UUID must format");
    /* Compare each byte of the expected string. */
    const char *expected = "04030201-0605-0807-090a-0b0c0d0e0f10";
    for (unsigned int i = 0; i < 37u; i++) {
        TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[i],
                       (uint32_t)(unsigned char)expected[i],
                       "UUID byte-for-byte match");
    }
}

static void test_uuid_format_all_zero_sentinel(void)
{
    unsigned char src[16];
    char buf[37];
    memset(src, 0, sizeof(src));
    buf[0] = 'x';
    int ok = boot_smbios_format_uuid(src, buf);
    TEST_ASSERT_EQ((uint32_t)ok, 0u, "all-zero UUID is the sentinel");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[0], 0u,
                   "sentinel must NUL-terminate output");
}

static void test_uuid_format_all_ff_sentinel(void)
{
    unsigned char src[16];
    char buf[37];
    memset(src, 0xFF, sizeof(src));
    buf[0] = 'x';
    int ok = boot_smbios_format_uuid(src, buf);
    TEST_ASSERT_EQ((uint32_t)ok, 0u, "all-FF UUID is the sentinel");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[0], 0u,
                   "sentinel must NUL-terminate output");
}

static void test_uuid_format_dash_positions(void)
{
    /* Verify dashes appear at positions 8, 13, 18, 23. */
    unsigned char src[16] = {
        0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11
    };
    char buf[37];
    (void)boot_smbios_format_uuid(src, buf);
    TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[8], (uint32_t)'-',
                   "dash at position 8");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[13], (uint32_t)'-',
                   "dash at position 13");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[18], (uint32_t)'-',
                   "dash at position 18");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[23], (uint32_t)'-',
                   "dash at position 23");
    TEST_ASSERT_EQ((uint32_t)(unsigned char)buf[36], 0u,
                   "NUL at position 36");
}

/* ---- Type 1 walker ----------------------------------------------------- */

/* Synthesize a 4-byte header + ASCII strings + double-NUL terminator
 * into buf at offset off. Returns the new offset. */
static unsigned int
emit_struct(unsigned char *buf, unsigned int off,
            unsigned char type, unsigned char length,
            const unsigned char *body, unsigned int body_len,
            const char **strings, unsigned int string_count)
{
    /* Body MUST include the 4-byte header; length is the total
     * formatted-area length. */
    unsigned int start = off;
    for (unsigned int i = 0; i < length; i++) {
        if (i == 0)      buf[off++] = type;
        else if (i == 1) buf[off++] = length;
        else if (i == 2) buf[off++] = 0;     /* handle low */
        else if (i == 3) buf[off++] = 0;     /* handle high */
        else if (body && i - 4 < body_len)
            buf[off++] = body[i - 4];
        else
            buf[off++] = 0;
    }
    for (unsigned int s = 0; s < string_count; s++) {
        const char *str = strings[s];
        for (unsigned int k = 0; str[k]; k++) buf[off++] = (unsigned char)str[k];
        buf[off++] = 0;
    }
    /* Trailing double-NUL: an empty strings array still needs the
     * second NUL because the string area always terminates with two
     * consecutive NULs. */
    if (string_count == 0) buf[off++] = 0;
    buf[off++] = 0;
    (void)start;
    return off;
}

static void test_walker_finds_type1(void)
{
    unsigned char buf[256];
    unsigned int off = 0;
    /* Type 0 (BIOS Info) -- 26-byte length per SMBIOS spec. */
    const char *bios_strings[] = { "Vendor", "1.0", "01/01/2026" };
    off = emit_struct(buf, off, 0, 26, NULL, 0, bios_strings, 3);
    /* Type 1 (System Info) -- 27-byte length (>= 25 to include UUID). */
    unsigned char type1_body[27 - 4];
    memset(type1_body, 0, sizeof(type1_body));
    /* UUID at body offset 4..19 (struct offset 8..23) */
    for (unsigned int i = 0; i < 16; i++) type1_body[4 + i] = (unsigned char)(i + 1);
    const char *sys_strings[] = { "Mfr", "Product" };
    off = emit_struct(buf, off, 1, 27, type1_body, sizeof(type1_body),
                      sys_strings, 2);
    /* End-of-table (type 127) */
    off = emit_struct(buf, off, 127, 4, NULL, 0, NULL, 0);

    const struct boot_smbios_header *h = boot_smbios_find_type1(buf, off);
    TEST_ASSERT_EQ((uint32_t)(h ? 1u : 0u), 1u,
                   "Type 1 must be found");
    if (h) {
        TEST_ASSERT_EQ((uint32_t)h->type, 1u, "found header is Type 1");
        TEST_ASSERT_EQ((uint32_t)h->length, 27u, "length matches");
    }
}

static void test_walker_absent_type1(void)
{
    unsigned char buf[64];
    unsigned int off = 0;
    /* Only BIOS info + end-of-table. */
    const char *bios_strings[] = { "Vendor" };
    off = emit_struct(buf, off, 0, 18, NULL, 0, bios_strings, 1);
    off = emit_struct(buf, off, 127, 4, NULL, 0, NULL, 0);
    const struct boot_smbios_header *h = boot_smbios_find_type1(buf, off);
    TEST_ASSERT_EQ((uint32_t)(h ? 1u : 0u), 0u,
                   "absent Type 1 returns NULL");
}

static void test_walker_rejects_null(void)
{
    const struct boot_smbios_header *h =
        boot_smbios_find_type1((const unsigned char *)0, 100);
    TEST_ASSERT_EQ((uint32_t)(h ? 1u : 0u), 0u,
                   "NULL base must return NULL");
}

static void test_walker_rejects_zero_length(void)
{
    unsigned char buf[8] = { 0 };
    const struct boot_smbios_header *h =
        boot_smbios_find_type1(buf, 0);
    TEST_ASSERT_EQ((uint32_t)(h ? 1u : 0u), 0u,
                   "zero len must return NULL");
}

static void test_walker_rejects_short_header(void)
{
    /* 4 bytes but header.length claims 200 -- must bail. */
    unsigned char buf[256];
    memset(buf, 0, sizeof(buf));
    buf[0] = 1;     /* type 1 */
    buf[1] = 200;   /* length > remaining */
    const struct boot_smbios_header *h =
        boot_smbios_find_type1(buf, 16);
    TEST_ASSERT_EQ((uint32_t)(h ? 1u : 0u), 0u,
                   "length exceeding remaining must return NULL");
}

static void test_walker_stops_at_eot_marker(void)
{
    /* Type 127 BEFORE Type 1 must hide the Type 1. */
    unsigned char buf[64];
    unsigned int off = 0;
    off = emit_struct(buf, off, 127, 4, NULL, 0, NULL, 0);
    /* Type 1 placed after EOT -- walker must not see it. */
    unsigned char body[27 - 4];
    memset(body, 0, sizeof(body));
    off = emit_struct(buf, off, 1, 27, body, sizeof(body), NULL, 0);
    const struct boot_smbios_header *h = boot_smbios_find_type1(buf, off);
    TEST_ASSERT_EQ((uint32_t)(h ? 1u : 0u), 0u,
                   "EOT marker stops the walk");
}

static void test_walker_caps_oversize_len(void)
{
    /* len > 1 MiB must be rejected without scanning. */
    const struct boot_smbios_header *h =
        boot_smbios_find_type1((const unsigned char *)0x1000, (1u << 21));
    TEST_ASSERT_EQ((uint32_t)(h ? 1u : 0u), 0u,
                   "oversize len rejected without deref");
}

/* ---- Registration ------------------------------------------------------ */

void test_register_smbios_parse(void);
void test_register_smbios_parse(void)
{
    test_suite_register_cat("smbios_parse: UUID format basic",
        test_uuid_format_basic, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: UUID all-zero sentinel",
        test_uuid_format_all_zero_sentinel, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: UUID all-FF sentinel",
        test_uuid_format_all_ff_sentinel, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: UUID dash positions",
        test_uuid_format_dash_positions, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: walker finds Type 1",
        test_walker_finds_type1, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: walker absent Type 1",
        test_walker_absent_type1, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: walker rejects NULL",
        test_walker_rejects_null, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: walker rejects zero length",
        test_walker_rejects_zero_length, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: walker rejects short header",
        test_walker_rejects_short_header, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: walker stops at EOT marker",
        test_walker_stops_at_eot_marker, TEST_CAT_BOOT);
    test_suite_register_cat("smbios_parse: walker caps oversize len",
        test_walker_caps_oversize_len, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
