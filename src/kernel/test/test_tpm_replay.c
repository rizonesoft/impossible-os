/* test_tpm_replay.c -- unit tests for tpm_pcr_extend (PCR replay primitive).
 * Self-consistency: tpm_pcr_extend(alg, pcr, digest) must equal the direct
 * H_alg(pcr || digest), proving the concat + dispatch are correct for every
 * bank. No live boot infrastructure (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "PCR Replay
 * Engine".
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/tpm_replay.h"
#include "kernel/tpm.h"
#include "kernel/crypto/sha1.h"
#include "kernel/crypto/sha256.h"
#include "kernel/crypto/sha384.h"
#include "libc/string.h"

/* Extend a zero PCR with `digest` in `alg`, and assert it equals the directly
 * computed H_alg(zero_pcr || digest). `dl` is the bank digest length. */
static void check_bank(uint16_t alg, uint32_t dl, uint8_t fill, const char *name)
{
    uint8_t pcr[64];
    uint8_t digest[64];
    uint8_t input[128];
    uint8_t expect[64];
    uint32_t i;

    for (i = 0; i < dl; i++) { pcr[i] = 0u; digest[i] = fill; }
    /* expected = H(zero_pcr || digest) */
    for (i = 0; i < dl; i++) { input[i] = 0u; input[dl + i] = fill; }
    if (alg == TPM_ALG_SHA1)        sha1(input, dl * 2u, expect);
    else if (alg == TPM_ALG_SHA256) sha256(input, dl * 2u, expect);
    else                            sha384(input, dl * 2u, expect);

    TEST_ASSERT_EQ(tpm_pcr_extend(alg, pcr, dl, digest, dl), TPM_REPLAY_OK,
                   "extend returns OK");
    TEST_ASSERT(memcmp(pcr, expect, dl) == 0, name);
}

static void test_pcr_extend_banks(void)
{
    check_bank(TPM_ALG_SHA1,   20u, 0xA1u, "SHA-1 extend == H(pcr||digest)");
    check_bank(TPM_ALG_SHA256, 32u, 0x5Au, "SHA-256 extend == H(pcr||digest)");
    check_bank(TPM_ALG_SHA384, 48u, 0x84u, "SHA-384 extend == H(pcr||digest)");
}

static void test_pcr_extend_chain(void)
{
    /* Two extends in sequence: PCR1 = H(0 || d), PCR2 = H(PCR1 || d). Verify the
     * second equals a freshly computed H(PCR1 || d), proving in-place update. */
    uint8_t pcr[32], d[32], snap[32], input[64], expect[32];
    uint32_t i;
    for (i = 0; i < 32u; i++) { pcr[i] = 0u; d[i] = (uint8_t)(i + 1u); }

    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, d, 32u), TPM_REPLAY_OK, "extend 1");
    for (i = 0; i < 32u; i++) snap[i] = pcr[i];      /* PCR after first extend */
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, d, 32u), TPM_REPLAY_OK, "extend 2");
    for (i = 0; i < 32u; i++) { input[i] = snap[i]; input[32 + i] = d[i]; }
    sha256(input, 64u, expect);
    TEST_ASSERT(memcmp(pcr, expect, 32u) == 0, "chained extend == H(prev||digest)");
}

static void test_pcr_extend_badargs(void)
{
    uint8_t pcr[32] = {0}, d[32] = {0};
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, (uint8_t *)0, 32u, d, 32u),
                   TPM_REPLAY_BADARG, "NULL pcr rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, (const uint8_t *)0, 32u),
                   TPM_REPLAY_BADARG, "NULL digest rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 20u, d, 32u),
                   TPM_REPLAY_BADARG, "wrong pcr_len for bank rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, d, 20u),
                   TPM_REPLAY_BADARG, "wrong digest_len for bank rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(0x0099u, pcr, 32u, d, 32u),
                   TPM_REPLAY_BADARG, "unsupported bank alg rejected");
}

void test_register_tpm_replay(void)
{
    test_suite_register_cat("tpm: PCR extend banks", test_pcr_extend_banks, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR extend chain", test_pcr_extend_chain, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR extend bad args", test_pcr_extend_badargs, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
