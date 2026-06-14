/* ============================================================================
 * test_tpm_attest.c -- TPM2 attestation (EK/AK provisioning, quote) tests
 *
 * Pure marshaling/parse fixtures (no MMIO). The live EK/AK provision + quote +
 * verify cycle is QEMU-swtpm / bare-metal validation (scripts/test-swtpm.sh).
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_seal.h"
#include "kernel/tpm_attest.h"
#include "kernel/tpm_transport.h"
#include "libc/string.h"

/* ---- EK CreatePrimary template byte-layout ---- */

static void test_attest_build_ek(void)
{
    uint8_t buf[256];
    uint32_t n;

    n = tpm2_build_create_primary_ek(buf, sizeof buf);
    TEST_ASSERT_EQ(n, 163u, "CreatePrimary EK (ECC-P256 template) is 163 bytes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 0), TPM2_ST_SESSIONS, "EK tag ST_SESSIONS");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 2), 163u, "EK header size == length");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_CREATE_PRIMARY, "EK CC CreatePrimary");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM_RH_ENDORSEMENT, "primaryHandle ENDORSEMENT");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), 9u, "EK authorizationSize 9 (PW session)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 18), TPM_RS_PW, "EK endorsement password session");
    TEST_ASSERT_EQ((uint32_t)buf[24], 0u, "EK TPM_RS_PW attrs byte is 0 (no continueSession)");
    /* inPublic inner at 10+4+13+6 = 33. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 33), 122u, "EK TPMT_PUBLIC inner size 122");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 35), TPM_ALG_ECC, "EK type ECC");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 37), TPM_ALG_SHA256, "EK nameAlg SHA256");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 39), (uint32_t)TPM_EK_OBJECT_ATTRS, "EK objectAttributes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 43), 32u, "EK authPolicy size 32");
    TEST_ASSERT_EQ((uint32_t)buf[45], 0x83u, "EK authPolicy is the well-known PolicyA");
    TEST_ASSERT(buf[45 + 31] == 0xAAu, "EK authPolicy last byte (PolicyA)");
    /* ECC params after the 32-byte policy: 45 + 32 = 77. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 77), TPM_ALG_AES, "EK symmetric AES");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 79), 128u, "EK AES keyBits 128");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 81), TPM_ALG_CFB, "EK symmetric mode CFB");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 85), TPM_ECC_NIST_P256, "EK curve NIST P256");
    /* unique x at 89: size 32 (NOT empty), then 32 zero bytes; y size at 123. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 89), 32u, "EK unique.x size 32 (zero point)");
    TEST_ASSERT_EQ((uint32_t)buf[91], 0u, "EK unique.x first byte zero");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 123), 32u, "EK unique.y size 32 (zero point)");

    TEST_ASSERT_EQ(tpm2_build_create_primary_ek(buf, 162u), 0u,
                   "CreatePrimary EK refuses too-small buffer");
}

void test_register_tpm_attest(void)
{
    test_suite_register_cat("tpm: EK CreatePrimary marshal", test_attest_build_ek, TEST_CAT_SECURITY);
}
