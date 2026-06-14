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

/* ---- PolicySecret(endorsement) byte-layout ---- */

static void test_attest_build_policy_secret(void)
{
    uint8_t buf[64];
    uint32_t n;

    n = tpm2_build_policy_secret(buf, sizeof buf, TPM_RH_ENDORSEMENT, 0x03000000u);
    TEST_ASSERT_EQ(n, 41u, "PolicySecret is 41 bytes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 0), TPM2_ST_SESSIONS, "PolicySecret tag ST_SESSIONS");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_POLICY_SECRET, "PolicySecret CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM_RH_ENDORSEMENT, "PolicySecret authHandle ENDORSEMENT");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), 0x03000000u, "PolicySecret policySession handle");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 18), 9u, "PolicySecret authorizationSize 9");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 22), TPM_RS_PW, "PolicySecret authHandle auth is PW");
    /* params after the 13-byte auth area at 18+13 = 31: nonceTPM/cpHashA/policyRef
     * empty TPM2Bs (2 each) + expiration(4). */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 31), 0u, "PolicySecret nonceTPM empty");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 33), 0u, "PolicySecret cpHashA empty");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 35), 0u, "PolicySecret policyRef empty");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 37), 0u, "PolicySecret expiration 0");

    TEST_ASSERT_EQ(tpm2_build_policy_secret(buf, 40u, TPM_RH_ENDORSEMENT, 0x03000000u),
                   0u, "PolicySecret refuses too-small buffer");
}

/* ---- AK signing-key Create byte-layout ---- */

static void test_attest_build_ak(void)
{
    uint8_t buf[128];
    uint32_t n;

    n = tpm2_build_create_ak_signing(buf, sizeof buf, 0x80000001u, 0x03000000u);
    TEST_ASSERT_EQ(n, 65u, "Create AK signing key is 65 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_CREATE, "AK CC Create");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), 0x80000001u, "AK parentHandle (loaded EK)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 18), 0x03000000u, "AK parent auth = policy session");
    TEST_ASSERT_EQ((uint32_t)buf[24], 1u, "AK policy-session attrs byte = continueSession");
    /* inPublic inner at 10+4+13+6 = 33. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 33), 24u, "AK TPMT_PUBLIC inner size 24");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 35), TPM_ALG_ECC, "AK type ECC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 39), (uint32_t)TPM_AK_OBJECT_ATTRS,
                   "AK attrs restricted|sign|userWithAuth|noDA");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 43), 0u, "AK authPolicy empty");
    /* params after empty authPolicy at 45: symmetric NULL, scheme ECDSA+SHA256. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 45), TPM_ALG_NULL, "AK symmetric NULL (sign key)");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 47), TPM_ALG_ECDSA, "AK scheme ECDSA");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 49), TPM_ALG_SHA256, "AK scheme hashAlg SHA256");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 51), TPM_ECC_NIST_P256, "AK curve P256");
}

/* ---- TPM2_Quote byte-layout ---- */

static void test_attest_build_quote(void)
{
    uint8_t buf[96];
    uint8_t nonce[20];
    uint32_t n, i;
    for (i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)(0x40u + i);

    /* total = 43 + nonce_len(20) = 63; PCR mask 0xFF (PCRs 0-7). */
    n = tpm2_build_quote(buf, sizeof buf, 0x80000002u, TPM_RS_PW, TPM_ALG_ECDSA,
                         nonce, 20u, 0xFFu);
    TEST_ASSERT_EQ(n, 63u, "Quote (20-byte nonce, ECDSA) is 63 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_QUOTE, "Quote CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), 0x80000002u, "Quote signHandle (AK)");
    /* qualifyingData at 10+4+13 = 27. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 27), 20u, "Quote qualifyingData (nonce) size 20");
    TEST_ASSERT_EQ((uint32_t)buf[29], 0x40u, "Quote nonce first byte");
    /* inScheme after the 20-byte nonce at 29+20 = 49. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 49), TPM_ALG_ECDSA, "Quote inScheme ECDSA");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 51), TPM_ALG_SHA256, "Quote inScheme hashAlg SHA256");
    /* TPML_PCR_SELECTION at 53. */
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 53), 1u, "Quote PCR selection count 1");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 57), TPM_ALG_SHA256, "Quote PCR bank SHA256");
    TEST_ASSERT_EQ((uint32_t)buf[59], 3u, "Quote sizeofSelect 3");
    TEST_ASSERT_EQ((uint32_t)buf[60], 0xFFu, "Quote pcrSelect byte0 == mask 0xFF");

    /* Reject too-short / absent nonce + an unsupported scheme. */
    TEST_ASSERT_EQ(tpm2_build_quote(buf, sizeof buf, 0x80000002u, TPM_RS_PW,
                                    TPM_ALG_ECDSA, nonce, 4u, 0xFFu),
                   0u, "Quote rejects too-short nonce");
    TEST_ASSERT_EQ(tpm2_build_quote(buf, sizeof buf, 0x80000002u, TPM_RS_PW,
                                    TPM_ALG_NULL, nonce, 20u, 0xFFu),
                   0u, "Quote rejects unsupported sig scheme");
}

/* ---- TPMS_ATTEST quote-response parse ---- */

/* Build a well-formed TPM2_Quote response into rsp; returns its total length. */
static uint32_t build_quote_response(uint8_t *rsp)
{
    uint32_t i, attest_off, a, attest_size, sig_off, plen, total;
    /* header */
    tpm2_be16_put(rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    /* TPM2B_ATTEST starts at params offset 14 (after parameterSize). */
    attest_off = 16u;                 /* params@14 -> size(2) -> TPMS_ATTEST@16 */
    a = attest_off;
    tpm2_be32_put(rsp + a, TPM2_GENERATED_VALUE); a += 4u;   /* magic */
    tpm2_be16_put(rsp + a, TPM2_ST_ATTEST_QUOTE); a += 2u;   /* type */
    tpm2_be16_put(rsp + a, 4u); a += 2u;                     /* qualifiedSigner name(4) */
    for (i = 0; i < 4u; i++) rsp[a + i] = (uint8_t)(0x11u + i);
    a += 4u;
    tpm2_be16_put(rsp + a, 20u); a += 2u;                    /* extraData = nonce(20) */
    for (i = 0; i < 20u; i++) rsp[a + i] = (uint8_t)(0x40u + i);
    a += 20u;
    tpm2_be32_put(rsp + a, 0u); tpm2_be32_put(rsp + a + 4u, 0x0000ABCDu); a += 8u; /* clock */
    tpm2_be32_put(rsp + a, 0x11223344u); a += 4u;            /* resetCount */
    tpm2_be32_put(rsp + a, 0x55667788u); a += 4u;            /* restartCount */
    rsp[a] = 1u; a += 1u;                                    /* safe */
    tpm2_be32_put(rsp + a, 0u); tpm2_be32_put(rsp + a + 4u, 0x20u); a += 8u; /* fwVersion */
    /* TPMS_QUOTE_INFO: TPML_PCR_SELECTION (1 bank, PCRs 0-7) + pcrDigest. */
    tpm2_be32_put(rsp + a, 1u); a += 4u;                     /* count */
    tpm2_be16_put(rsp + a, TPM_ALG_SHA256); a += 2u;
    rsp[a] = 3u; a += 1u;                                    /* sizeofSelect */
    rsp[a] = 0xFFu; rsp[a + 1u] = 0u; rsp[a + 2u] = 0u; a += 3u; /* PCRs 0-7 */
    tpm2_be16_put(rsp + a, 32u); a += 2u;                    /* pcrDigest size */
    for (i = 0; i < 32u; i++) rsp[a + i] = (uint8_t)(0xC0u + i);
    a += 32u;
    attest_size = a - attest_off;
    tpm2_be16_put(rsp + 14, (uint16_t)attest_size);          /* TPM2B_ATTEST size */
    /* TPMT_SIGNATURE (ECDSA): sigAlg + hashAlg + R(2+32) + S(2+32). */
    sig_off = a;
    tpm2_be16_put(rsp + a, TPM_ALG_ECDSA); a += 2u;
    tpm2_be16_put(rsp + a, TPM_ALG_SHA256); a += 2u;
    tpm2_be16_put(rsp + a, 32u); a += 2u;
    for (i = 0; i < 32u; i++) rsp[a + i] = (uint8_t)(0x10u + i);
    a += 32u;
    tpm2_be16_put(rsp + a, 32u); a += 2u;
    for (i = 0; i < 32u; i++) rsp[a + i] = (uint8_t)(0x90u + i);
    a += 32u;
    (void)sig_off;
    plen = a - 14u;
    tpm2_be32_put(rsp + 10, plen);                           /* parameterSize */
    /* one-session auth area (5 bytes: nonceTPM(2,0)+attrs(1)+hmac(2,0)). */
    rsp[a] = 0; rsp[a + 1u] = 0; rsp[a + 2u] = 0; rsp[a + 3u] = 0; rsp[a + 4u] = 0;
    a += 5u;
    total = a;
    tpm2_be32_put(rsp + 2, total);
    return total;
}

static void test_attest_parse_quote(void)
{
    uint8_t rsp[256], sig[128];
    struct tpm_quote_attest att;
    uint32_t n, sig_len = 0;

    memset(rsp, 0, sizeof rsp);
    n = build_quote_response(rsp);
    memset(&att, 0, sizeof att);
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), 0,
                   "valid quote response parses");
    TEST_ASSERT_EQ((uint32_t)att.nonce_len, 20u, "attest nonce len 20");
    TEST_ASSERT(att.nonce[0] == 0x40u && att.nonce[19] == 0x53u, "attest nonce bytes");
    TEST_ASSERT_EQ(att.reset_count, 0x11223344u, "attest resetCount");
    TEST_ASSERT_EQ(att.restart_count, 0x55667788u, "attest restartCount");
    TEST_ASSERT_EQ((uint32_t)att.safe, 1u, "attest clock safe bit");
    TEST_ASSERT_EQ(att.pcr_select, 0xFFu, "attest pcrSelect (PCRs 0-7)");
    TEST_ASSERT_EQ((uint32_t)att.pcr_digest_len, 32u, "attest pcrDigest len 32");
    TEST_ASSERT(att.pcr_digest[0] == 0xC0u, "attest pcrDigest bytes");
    TEST_ASSERT_EQ(sig_len, 72u, "ECDSA signature blob 72 bytes (sigAlg+hash+R+S)");
    TEST_ASSERT_EQ(tpm2_be16_get(sig + 0), TPM_ALG_ECDSA, "signature alg ECDSA");

    /* Wrong magic / wrong type are rejected (not a TPM-generated quote). */
    tpm2_be32_put(rsp + 16, 0xDEADBEEFu);
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "bad attest magic rejected");
    n = build_quote_response(rsp);
    tpm2_be16_put(rsp + 20, 0x8017u);   /* ATTEST_CERTIFY, not QUOTE */
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "non-quote attest type rejected");
}

/* ---- EvictControl byte-layout ---- */

static void test_attest_build_evict(void)
{
    uint8_t buf[48];
    uint32_t n;
    n = tpm2_build_evict_control(buf, sizeof buf, 0x80000003u, 0x81010001u);
    TEST_ASSERT_EQ(n, 35u, "EvictControl is 35 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_EVICT_CONTROL, "EvictControl CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM_RH_OWNER, "EvictControl authHandle owner");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), 0x80000003u, "EvictControl objectHandle (AK)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 31), 0x81010001u, "EvictControl persistentHandle");
}

void test_register_tpm_attest(void)
{
    test_suite_register_cat("tpm: EK CreatePrimary marshal", test_attest_build_ek, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PolicySecret marshal", test_attest_build_policy_secret, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: AK signing-key marshal", test_attest_build_ak, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: Quote marshal", test_attest_build_quote, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: quote-response (TPMS_ATTEST) parse", test_attest_parse_quote, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: EvictControl marshal", test_attest_build_evict, TEST_CAT_SECURITY);
}
