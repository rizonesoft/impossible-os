/* test_tpm_sb_reconcile.c -- unit tests for the pure Secure Boot reconciliation
 * seam: uefi_var_data_parse() (TCG UEFI_VARIABLE_DATA decoder) and
 * sb_reconcile_classify() (impossible-combination logic). Both are
 * side-effect-free, so these tests build byte fixtures / call the classifier
 * directly. No live boot infrastructure is touched (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "Secure Boot
 * Variable Measurement Reconciliation".
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "libc/string.h"

/* ---- UEFI_VARIABLE_DATA fixture builder ---- */
static uint8_t  s_vd[256];
static uint32_t s_vd_len;

static void vd_put64(uint32_t off, uint64_t v)
{
    for (uint32_t i = 0; i < 8u; i++)
        s_vd[off + i] = (uint8_t)(v >> (8u * i));
}

/* Build a UEFI_VARIABLE_DATA blob: 16B guid (0x10..0x1F) + name(UCS-2) + data. */
static void vd_build(const char *name, uint32_t name_chars,
                     uint32_t data_len, uint8_t data_fill)
{
    uint32_t off;
    for (uint32_t i = 0; i < 16u; i++) s_vd[i] = (uint8_t)(0x10u + i);
    vd_put64(16, name_chars);
    vd_put64(24, data_len);
    off = 32u;
    for (uint32_t i = 0; i < name_chars; i++) {
        s_vd[off++] = (uint8_t)name[i];   /* low byte */
        s_vd[off++] = 0u;                 /* high byte (ASCII UCS-2) */
    }
    for (uint32_t i = 0; i < data_len; i++)
        s_vd[off++] = data_fill;
    s_vd_len = off;
}

static void test_uefi_var_data_parse(void)
{
    uint8_t guid[16];
    uint16_t name[16];
    uint32_t name_chars = 0, data_off = 0, data_len = 0;
    int r;

    /* Valid: name "db" (2 CHAR16), 5 data bytes. */
    vd_build("db", 2u, 5u, 0xAB);
    r = uefi_var_data_parse(s_vd, s_vd_len, guid, name, 16u, &name_chars,
                            &data_off, &data_len);
    TEST_ASSERT_EQ(r, 0, "valid VARIABLE_DATA parses");
    TEST_ASSERT_EQ(guid[0], 0x10u, "guid byte 0 extracted");
    TEST_ASSERT_EQ(guid[15], 0x1Fu, "guid byte 15 extracted");
    TEST_ASSERT_EQ(name_chars, 2u, "name length = 2 CHAR16");
    TEST_ASSERT_EQ(name[0], (uint16_t)'d', "name[0] = d");
    TEST_ASSERT_EQ(name[1], (uint16_t)'b', "name[1] = b");
    TEST_ASSERT_EQ(data_off, 36u, "data offset = 32 + 2*2");
    TEST_ASSERT_EQ(data_len, 5u, "data length = 5");

    /* Too short for the fixed header. */
    TEST_ASSERT_EQ(uefi_var_data_parse(s_vd, 31u, guid, name, 16u, &name_chars,
                                       &data_off, &data_len),
                   -1, "size < 32 rejected");

    /* Name runs past the buffer (claims 10 chars but buffer holds 2). */
    vd_build("db", 2u, 0u, 0);
    vd_put64(16, 10u);    /* overstate name length */
    TEST_ASSERT_EQ(uefi_var_data_parse(s_vd, s_vd_len, guid, name, 16u,
                                       &name_chars, &data_off, &data_len),
                   -1, "name length past buffer rejected");

    /* Data runs past the buffer. */
    vd_build("db", 2u, 5u, 0xAB);
    vd_put64(24, 100u);   /* overstate data length */
    TEST_ASSERT_EQ(uefi_var_data_parse(s_vd, s_vd_len, guid, name, 16u,
                                       &name_chars, &data_off, &data_len),
                   -1, "data length past buffer rejected");

    /* Absurd name length (> sanity cap) rejected before multiply overflow. */
    vd_build("db", 2u, 0u, 0);
    vd_put64(16, 0x20000ull);
    TEST_ASSERT_EQ(uefi_var_data_parse(s_vd, s_vd_len, guid, name, 16u,
                                       &name_chars, &data_off, &data_len),
                   -1, "absurd name length rejected");

    /* Wrap defense: a near-UINT64_MAX data_len must NOT wrap an additive bound
     * back under size (subtraction-form check). */
    vd_build("db", 2u, 5u, 0xAB);
    vd_put64(24, 0xFFFFFFFFFFFFFFF0ull);
    TEST_ASSERT_EQ(uefi_var_data_parse(s_vd, s_vd_len, guid, name, 16u,
                                       &name_chars, &data_off, &data_len),
                   -1, "wrapped (near-UINT64_MAX) data length rejected");

    /* data_len that exceeds UINT32_MAX is rejected (cannot fit out_data_len). */
    vd_build("db", 2u, 5u, 0xAB);
    vd_put64(24, 0x100000000ull);
    TEST_ASSERT_EQ(uefi_var_data_parse(s_vd, s_vd_len, guid, name, 16u,
                                       &name_chars, &data_off, &data_len),
                   -1, "data length > UINT32_MAX rejected");

    /* Zero-length data is VALID (e.g. empty dbx): parses, data_len = 0, the
     * data offset still points past the name. The reconciler treats a 0==0
     * length pair as EQUAL, not a missing measurement. */
    vd_build("dbx", 3u, 0u, 0);
    r = uefi_var_data_parse(s_vd, s_vd_len, guid, name, 16u, &name_chars,
                            &data_off, &data_len);
    TEST_ASSERT_EQ(r, 0, "zero-length VariableData parses");
    TEST_ASSERT_EQ(name_chars, 3u, "name length = 3 for empty dbx");
    TEST_ASSERT_EQ(data_off, 38u, "data offset = 32 + 3*2 for empty data");
    TEST_ASSERT_EQ(data_len, 0u, "data length = 0 accepted");

    /* name_cap truncation: only 1 code unit copied, full count still reported. */
    vd_build("KEK", 3u, 1u, 0x00);
    name[1] = 0xEEEE;
    r = uefi_var_data_parse(s_vd, s_vd_len, guid, name, 1u, &name_chars,
                            &data_off, &data_len);
    TEST_ASSERT_EQ(r, 0, "valid parse with small name_cap");
    TEST_ASSERT_EQ(name_chars, 3u, "full name length reported despite cap");
    TEST_ASSERT_EQ(name[0], (uint16_t)'K', "first code unit copied under cap");
    TEST_ASSERT_EQ(name[1], 0xEEEEu, "code unit past cap untouched");
}

static void test_sb_reconcile_classify(void)
{
    /* Unreadable live state -> only STATE_UNKNOWN, no enabled-claims. */
    TEST_ASSERT_EQ(sb_reconcile_classify(1, 0, /*sv*/0, /*en*/0, 0, 0),
                   SB_IMPOSSIBLE_STATE_UNKNOWN, "unknown state -> STATE_UNKNOWN");
    TEST_ASSERT_EQ(sb_reconcile_classify(1, 0, /*sv*/0, /*en*/1, 1, 0),
                   SB_IMPOSSIBLE_STATE_UNKNOWN, "unknown state masks enabled claims");

    /* Enabled, clean log, no PCR7 events, PK present -> ENABLED_NO_PCR7 only. */
    TEST_ASSERT_EQ(sb_reconcile_classify(/*clean*/1, /*pcr7*/0, 1, 1, 0, /*pk*/1),
                   SB_IMPOSSIBLE_ENABLED_NO_PCR7,
                   "enabled + clean log + no PCR7 events -> NO_PCR7");

    /* Same but log NOT clean -> NO_PCR7 must NOT fire (degraded, not impossible). */
    TEST_ASSERT_EQ(sb_reconcile_classify(/*clean*/0, /*pcr7*/0, 1, 1, 0, /*pk*/1),
                   SB_IMPOSSIBLE_NONE,
                   "degraded log never produces a false NO_PCR7 impossibility");

    /* Enabled while SetupMode active. */
    TEST_ASSERT_EQ(sb_reconcile_classify(1, 5, 1, 1, /*setup*/1, 1),
                   SB_IMPOSSIBLE_ENABLED_SETUPMODE,
                   "enabled + setup mode -> ENABLED_SETUPMODE");

    /* Enabled but PK absent. */
    TEST_ASSERT_EQ(sb_reconcile_classify(1, 5, 1, 1, 0, /*pk*/0),
                   SB_IMPOSSIBLE_ENABLED_NO_PK,
                   "enabled + no PK -> ENABLED_NO_PK");

    /* Healthy: enabled, clean log, PCR7 events present, PK present, no setup. */
    TEST_ASSERT_EQ(sb_reconcile_classify(1, 3, 1, 1, 0, 1),
                   SB_IMPOSSIBLE_NONE, "healthy active SB -> no impossibilities");

    /* Disabled SB makes no enabled-claims even with no PCR7 events. */
    TEST_ASSERT_EQ(sb_reconcile_classify(1, 0, 1, /*en*/0, 0, 0),
                   SB_IMPOSSIBLE_NONE, "disabled SB -> no enabled impossibilities");
}

void test_register_tpm_sb_reconcile(void)
{
    test_suite_register_cat("tpm: UEFI_VARIABLE_DATA parse",
        test_uefi_var_data_parse, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: SB reconcile classify",
        test_sb_reconcile_classify, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
