/* ============================================================================
 * test_uefi_advanced.c -- TODO-27 UEFI Advanced Features tests
 *
 * Covers the deterministic, pure-helper surface of TODO-27:
 *   - S6 SMBIOS extended type parsing: chassis mobile classifier +
 *     64-bit KB->byte conversion (the >4 GiB no-wrap regression).
 *   - S7 dbx freshness: added when S7 ships (membership/version compare).
 *
 * S5 (Secure Boot enforcement) is deferred to 02-kernel-core/TODO-10 S16;
 * its registry assertions land there. Full SMBIOS Type 2/3/16/19 parsing
 * is validated on real hardware (firmware tables differ from QEMU synthetic
 * tables); these tests exercise only the stateless helpers, per the
 * test-policy ban on live boot infrastructure.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/types.h"
#include "kernel/smbios.h"

/* ---- S6: chassis mobile classifier (DMTF DSP0134 Table 17) ------------- */

static void test_chassis_mobile_true(void)
{
    /* Mobile/portable form factors must classify as laptop. */
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x08), 1u, "Portable");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x09), 1u, "Laptop");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x0A), 1u, "Notebook");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x0B), 1u, "Hand Held");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x0E), 1u, "Sub Notebook");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x1E), 1u, "Tablet");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x1F), 1u, "Convertible");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x20), 1u, "Detachable");
}

static void test_chassis_mobile_false(void)
{
    /* Desktop / server / unknown form factors are not laptops. */
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x00), 0u, "zero");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x01), 0u, "Other");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x03), 0u, "Desktop");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x07), 0u, "Tower");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0x17), 0u, "Rack Mount");
    TEST_ASSERT_EQ((uint32_t)smbios_chassis_type_is_mobile(0xFF), 0u, "out of range");
}

/* ---- S6: KB->byte 64-bit conversion (Codex design D2 anti-wrap) -------- */

static void test_kb_to_bytes_basic(void)
{
    TEST_ASSERT_EQ((uint32_t)smbios_kb_to_bytes(0), 0u, "0 KB -> 0 bytes");
    TEST_ASSERT_EQ((uint32_t)smbios_kb_to_bytes(1), 1024u, "1 KB -> 1024 bytes");
    TEST_ASSERT_EQ((uint32_t)smbios_kb_to_bytes(1024), 1048576u, "1024 KB -> 1 MiB");
}

static void test_kb_to_bytes_no_wrap_above_4gib(void)
{
    /* 4 GiB expressed in KB is 4194304. A 32-bit multiply would wrap to 0;
     * the 64-bit helper must yield exactly 0x1_0000_0000. */
    uint64_t bytes = smbios_kb_to_bytes(4194304u);
    TEST_ASSERT_EQ((uint32_t)(bytes >> 32), 1u, "high dword of 4 GiB == 1");
    TEST_ASSERT_EQ((uint32_t)(bytes & 0xFFFFFFFFu), 0u, "low dword of 4 GiB == 0");
    /* 0xFFFFFFFF KB (max 32-bit KB field) must not truncate either. */
    uint64_t big = smbios_kb_to_bytes(0xFFFFFFFFu);
    TEST_ASSERT_EQ((uint32_t)(big >> 32), 0x3FFu, "high dword of 0xFFFFFFFF KB");
}

/* ---- S6: Type 16 capacity decode (pure, synthetic records) ------------- */

/* Build a minimal Type 16 record: rec[0]=type, rec[1]=length, rec[2..3]=handle,
 * then the formatted fields. Callers set cap (offset 7..0x0A) + extended
 * (0x0F..0x16) + device count (0x0D..0x0E) as needed. */
static void test_type16_capacity_normal_kb(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 16; rec[1] = 0x0F;
    /* 8 GiB in KB = 0x800000 (8388608) at offset 7 (LE) */
    rec[7] = 0x00; rec[8] = 0x00; rec[9] = 0x80; rec[0x0A] = 0x00;
    rec[0x0D] = 4; rec[0x0E] = 0;  /* 4 devices */
    uint64_t cap = 0xDEAD; uint16_t devs = 0xFFFF;
    int ok = smbios_type16_decode(rec, rec[1], &cap, &devs);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "decode succeeds");
    TEST_ASSERT_EQ((uint32_t)(cap >> 20), 8192u, "8 GiB capacity in MiB");
    TEST_ASSERT_EQ((uint32_t)devs, 4u, "device count LE decode");
}

static void test_type16_capacity_zero_unknown(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 16; rec[1] = 0x0F;
    uint64_t cap = 0xDEAD;
    int ok = smbios_type16_decode(rec, rec[1], &cap, (uint16_t *)0);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "decode succeeds");
    TEST_ASSERT_EQ((uint32_t)cap, 0u, "cap_kb=0 leaves capacity unknown");
}

static void test_type16_capacity_extended_sentinel(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 16; rec[1] = 0x17;
    /* sentinel 0x80000000 at offset 7 */
    rec[7] = 0x00; rec[8] = 0x00; rec[9] = 0x00; rec[0x0A] = 0x80;
    /* Extended capacity = 0x1_0000_0000 bytes (4 GiB) at 0x0F (LE) */
    rec[0x0F + 4] = 0x01;
    uint64_t cap = 0;
    int ok = smbios_type16_decode(rec, rec[1], &cap, (uint16_t *)0);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "decode succeeds");
    TEST_ASSERT_EQ((uint32_t)(cap >> 32), 1u, "extended 4 GiB high dword");
    TEST_ASSERT_EQ((uint32_t)(cap & 0xFFFFFFFFu), 0u, "extended 4 GiB low dword");
}

static void test_type16_extended_sentinel_short_record(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 16; rec[1] = 0x16;  /* one byte short of extended field at 0x0F..0x16 */
    rec[7] = 0x00; rec[8] = 0x00; rec[9] = 0x00; rec[0x0A] = 0x80;  /* sentinel */
    uint64_t cap = 0xDEAD;
    int ok = smbios_type16_decode(rec, rec[1], &cap, (uint16_t *)0);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "decode succeeds (length >= 0x0F)");
    TEST_ASSERT_EQ((uint32_t)cap, 0u, "short record does not read past formatted area");
}

/* ---- S6: Type 19 range decode (pure, synthetic records) ---------------- */

static void test_type19_kb_inclusive_end(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 19; rec[1] = 0x0F;
    /* start = 1 KB (offset 4), end = 1 KB (offset 8) */
    rec[4] = 0x01;
    rec[8] = 0x01;
    /* Memory Array Handle 0x1234 at offset 0x0C (LE) */
    rec[0x0C] = 0x34; rec[0x0D] = 0x12;
    uint64_t start = 0, end = 0; uint16_t handle = 0;
    int ok = smbios_type19_decode(rec, rec[1], &start, &end, &handle);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "decode succeeds");
    TEST_ASSERT_EQ((uint32_t)start, 1024u, "start = 1 KB -> 1024 bytes");
    TEST_ASSERT_EQ((uint32_t)end, 2047u, "end = 1 KB -> inclusive 2047 bytes");
    TEST_ASSERT_EQ((uint32_t)handle, 0x1234u, "memory array handle LE decode");
}

static void test_type19_extended_sentinel(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 19; rec[1] = 0x1F;
    rec[4] = 0xFF; rec[5] = 0xFF; rec[6] = 0xFF; rec[7] = 0xFF;  /* sentinel start */
    rec[8] = 0x00; rec[9] = 0x00; rec[0x0A] = 0x00; rec[0x0B] = 0x00;
    /* extended start at 0x0F = 0x2000 bytes; extended end at 0x17 = 0x3FFF */
    rec[0x0F] = 0x00; rec[0x10] = 0x20;
    rec[0x17] = 0xFF; rec[0x18] = 0x3F;
    uint64_t start = 0, end = 0; uint16_t handle = 0;
    int ok = smbios_type19_decode(rec, rec[1], &start, &end, &handle);
    TEST_ASSERT_EQ((uint32_t)ok, 1u, "decode succeeds");
    TEST_ASSERT_EQ((uint32_t)start, 0x2000u, "extended start at 0x0F");
    TEST_ASSERT_EQ((uint32_t)end, 0x3FFFu, "extended end at 0x17");
}

static void test_type19_sentinel_short_record_rejected(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 19; rec[1] = 0x1E;  /* one short of extended end field at 0x17..0x1E */
    rec[4] = 0xFF; rec[5] = 0xFF; rec[6] = 0xFF; rec[7] = 0xFF;  /* sentinel */
    uint64_t start = 9, end = 9; uint16_t handle = 0;
    int ok = smbios_type19_decode(rec, rec[1], &start, &end, &handle);
    TEST_ASSERT_EQ((uint32_t)ok, 0u, "sentinel without extended fields rejected");
}

static void test_type19_reversed_range_rejected(void)
{
    uint8_t rec[0x20];
    uint32_t i;
    for (i = 0; i < sizeof(rec); i++) rec[i] = 0;
    rec[0] = 19; rec[1] = 0x0F;
    /* start = 0x10 KB, end = 0x01 KB (reversed) */
    rec[4] = 0x10;
    rec[8] = 0x01;
    uint64_t start = 9, end = 9; uint16_t handle = 0;
    int ok = smbios_type19_decode(rec, rec[1], &start, &end, &handle);
    TEST_ASSERT_EQ((uint32_t)ok, 0u, "end < start rejected");
}

void test_register_uefi_advanced(void);
void test_register_uefi_advanced(void)
{
    test_suite_register_cat("uefi_advanced: chassis mobile codes",
        test_chassis_mobile_true, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: chassis non-mobile codes",
        test_chassis_mobile_false, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: KB->byte basic",
        test_kb_to_bytes_basic, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: KB->byte no wrap above 4 GiB",
        test_kb_to_bytes_no_wrap_above_4gib, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type16 capacity normal KB",
        test_type16_capacity_normal_kb, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type16 capacity zero unknown",
        test_type16_capacity_zero_unknown, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type16 extended sentinel",
        test_type16_capacity_extended_sentinel, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type16 extended short record",
        test_type16_extended_sentinel_short_record, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type19 KB inclusive end",
        test_type19_kb_inclusive_end, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type19 extended sentinel",
        test_type19_extended_sentinel, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type19 sentinel short reject",
        test_type19_sentinel_short_record_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("uefi_advanced: Type19 reversed range reject",
        test_type19_reversed_range_rejected, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
