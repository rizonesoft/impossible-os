/* ============================================================================
 * test_tpm_baseline.c -- measured-boot baseline pure-core tests
 *
 * Format / validate / compare / generation logic (MMIO-free, no live TPM). The
 * live snapshot / enroll / verify wrappers are validated against QEMU swtpm.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "kernel/tpm_baseline.h"
#include "libc/string.h"

/* The measured PCR set the baseline canonically pins (matches tpm_baseline.c). */
static const uint8_t k_baseline_pcrs[TPM_BASELINE_MAX_PCRS] =
    { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 11u };

/* Build a CANONICAL well-formed golden baseline: the full measured PCR set in
 * the expected index order, all present, plus SB state + firmware hash. */
static void make_golden(struct tpm_baseline *b)
{
    uint32_t i;
    memset(b, 0, sizeof(*b));
    b->alg = TPM_ALG_SHA256;
    b->generation = 1u;
    b->pcr_count = TPM_BASELINE_MAX_PCRS;
    for (i = 0; i < TPM_BASELINE_MAX_PCRS; i++) {
        b->pcrs[i].index = k_baseline_pcrs[i];
        b->pcrs[i].present = 1u;
        memset(b->pcrs[i].digest, (int)(0xA0u + i), TPM_BASELINE_DIGEST);
    }
    b->secure_boot = 1u;
    b->secure_boot_valid = 1u;
    b->fw_hash_present = 1u;
    memset(b->fw_hash, 0xCC, TPM_BASELINE_DIGEST);
    tpm_baseline_finalize(b);
}

static void test_baseline_finalize_validate(void)
{
    struct tpm_baseline b;
    struct tpm_baseline out;

    make_golden(&b);
    TEST_ASSERT_EQ(tpm_baseline_finalize(&b), (uint32_t)sizeof(struct tpm_baseline),
                   "finalize returns blob size");
    TEST_ASSERT_EQ(b.magic, TPM_BASELINE_MAGIC, "finalize stamps magic");
    TEST_ASSERT_EQ((uint32_t)b.version, (uint32_t)TPM_BASELINE_VERSION, "finalize stamps version");
    TEST_ASSERT_EQ((uint32_t)b.size, (uint32_t)sizeof(struct tpm_baseline), "finalize stamps size");

    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, sizeof(b), &out), 1,
                   "well-formed canonical baseline validates");
    TEST_ASSERT_EQ((uint32_t)out.generation, 1u, "validate copies fields into the aligned out");

    /* NULL out / truncated blob -> reject. */
    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, sizeof(b), 0), 0,
                   "NULL out rejected");
    TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&b, sizeof(b) - 1u, &out), 0,
                   "truncated blob rejected");
    /* Corrupt magic -> reject. */
    {
        struct tpm_baseline c = b;
        c.magic ^= 0xFFu;
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "bad magic rejected");
    }
    /* Wrong version -> reject. */
    {
        struct tpm_baseline c = b;
        c.version = (uint16_t)(TPM_BASELINE_VERSION + 1u);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "bad version rejected");
    }
    /* Tampered body without recomputing crc -> reject (crc covers the body). */
    {
        struct tpm_baseline c = b;
        c.pcrs[0].digest[0] ^= 0xFFu;
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "body tamper without crc fixup rejected");
    }
    /* CANONICAL-STRUCTURE attacks (CRC-valid but hollow) -- all rejected: */
    /* pcr_count != full measured set. */
    {
        struct tpm_baseline c = b;
        c.pcr_count = 2u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "non-full pcr_count rejected");
    }
    /* Every PCR present=0 -> pins nothing -> reject (F-B1). */
    {
        struct tpm_baseline c = b;
        uint32_t i;
        for (i = 0; i < TPM_BASELINE_MAX_PCRS; i++) c.pcrs[i].present = 0u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "all-absent PCR set rejected (pins nothing)");
    }
    /* PARTIAL present-set (8 of 9) -> reject: a verifiable baseline must pin the
     * FULL measured set, else the unpinned PCR's changes go unverified (F-RV3). */
    {
        struct tpm_baseline c = b;
        c.pcrs[4].present = 0u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "partial present-set rejected (must pin all measured PCRs)");
    }
    /* Wrong PCR index order -> reject. */
    {
        struct tpm_baseline c = b;
        c.pcrs[0].index = 9u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "wrong PCR index order rejected");
    }
    /* Out-of-range boolean flag -> reject. */
    {
        struct tpm_baseline c = b;
        c.secure_boot = 2u;
        tpm_baseline_finalize(&c);
        TEST_ASSERT_EQ(tpm_baseline_validate((const uint8_t *)&c, sizeof(c), &out), 0,
                       "non-boolean flag rejected");
    }
}

static void test_baseline_compare(void)
{
    struct tpm_baseline g, c;
    make_golden(&g);

    /* Identical current snapshot -> MATCH. */
    c = g;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MATCH,
                   "identical snapshot matches");

    /* A changed PCR digest -> MISMATCH. */
    c = g; c.pcrs[0].digest[5] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "changed PCR digest -> mismatch");

    /* A golden PCR no longer present in current -> MISMATCH. */
    c = g; c.pcrs[1].present = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "golden PCR absent in current -> mismatch");

    /* Secure Boot state change -> MISMATCH. */
    c = g; c.secure_boot = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "SB state change -> mismatch");
    /* SB readability change (valid->unknown) -> MISMATCH (never a silent match). */
    c = g; c.secure_boot_valid = 0u;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "SB readability change -> mismatch");

    /* Firmware-version hash change -> MISMATCH. */
    c = g; c.fw_hash[0] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "firmware-version hash change -> mismatch");

    /* Different hash bank -> MISMATCH. */
    c = g; c.alg = TPM_ALG_SHA384;
    TEST_ASSERT_EQ((int)tpm_baseline_compare(&g, &c), (int)TPM_BASELINE_MISMATCH,
                   "different bank -> mismatch");

    /* NULL args -> CMP_BADARG. */
    TEST_ASSERT_EQ((int)tpm_baseline_compare(0, &c), (int)TPM_BASELINE_CMP_BADARG,
                   "NULL golden -> badarg");
}

static void test_baseline_rotation(void)
{
    TEST_ASSERT_EQ(tpm_baseline_rotation_ok(1u, 2u), 1, "higher generation accepted");
    TEST_ASSERT_EQ(tpm_baseline_rotation_ok(2u, 2u), 0, "equal generation rejected (no rollback)");
    TEST_ASSERT_EQ(tpm_baseline_rotation_ok(5u, 3u), 0, "lower generation rejected (rollback)");
}

void test_register_tpm_baseline(void)
{
    test_suite_register_cat("tpm: baseline finalize/validate", test_baseline_finalize_validate, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline compare", test_baseline_compare, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline rotation guard", test_baseline_rotation, TEST_CAT_SECURITY);
}
