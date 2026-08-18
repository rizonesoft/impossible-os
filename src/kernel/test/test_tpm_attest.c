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

/* pcrDigest length build_quote_response emits (default = the SHA-256 size 32);
 * a test sets this to forge a consistent but wrong-length digest. */
static uint16_t bq_digest_len = 32u;

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
    tpm2_be16_put(rsp + a, bq_digest_len); a += 2u;          /* pcrDigest size */
    for (i = 0; i < bq_digest_len; i++) rsp[a + i] = (uint8_t)(0xC0u + i);
    a += bq_digest_len;
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
    /* attest_raw captures the EXACT signed TPMS_ATTEST bytes (the verifier's
     * signature input -- not the parsed fields). */
    TEST_ASSERT_EQ((uint32_t)att.attest_raw_len, (uint32_t)tpm2_be16_get(rsp + 14),
                   "attest_raw_len == TPM2B_ATTEST size");
    TEST_ASSERT_EQ(tpm2_be32_get(att.attest_raw), TPM2_GENERATED_VALUE,
                   "attest_raw[0..3] == TPMS_ATTEST magic");

    /* Wrong magic / wrong type are rejected (not a TPM-generated quote). */
    tpm2_be32_put(rsp + 16, 0xDEADBEEFu);
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "bad attest magic rejected");
    n = build_quote_response(rsp);
    tpm2_be16_put(rsp + 20, 0x8017u);   /* ATTEST_CERTIFY, not QUOTE */
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "non-quote attest type rejected");
}

/* The signature-structure validation rejects a crafted "successful" quote whose
 * TPMT_SIGNATURE is malformed -- otherwise tpm2_parse_quote would hand the verifier
 * an unverifiable signature reported as OK. The signature begins at offset
 * 16 + TPM2B_ATTEST size (header 14 + size(2) -> attest -> signature). */
static void test_attest_parse_quote_sig_reject(void)
{
    uint8_t rsp[256], sig[128];
    struct tpm_quote_attest att;
    uint32_t n, sig_len = 0, soff;

    /* Unsupported signing scheme (neither ECDSA nor RSASSA). */
    memset(rsp, 0, sizeof rsp);
    n = build_quote_response(rsp);
    soff = 16u + (uint32_t)tpm2_be16_get(rsp + 14);
    tpm2_be16_put(rsp + soff, TPM_ALG_SHA256);       /* not a signature alg */
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "unsupported sig scheme rejected");

    /* Zero-length sigR -- a degenerate ECDSA signature. */
    n = build_quote_response(rsp);
    soff = 16u + (uint32_t)tpm2_be16_get(rsp + 14);
    tpm2_be16_put(rsp + soff + 4u, 0u);              /* sigR TPM2B size = 0 */
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "zero-length sigR rejected");

    /* sigR length overruns the remaining parameter area. */
    n = build_quote_response(rsp);
    soff = 16u + (uint32_t)tpm2_be16_get(rsp + 14);
    tpm2_be16_put(rsp + soff + 4u, 0xFFFFu);         /* sigR size huge */
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "overlong sigR rejected");

    /* Non-exact consumption: declared sigS shorter than the bytes present, so the
     * signature is not consumed to its exact end. */
    n = build_quote_response(rsp);
    soff = 16u + (uint32_t)tpm2_be16_get(rsp + 14);
    tpm2_be16_put(rsp + soff + 38u, 16u);            /* sigS size 16 (32 bytes present) */
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "non-exact signature consumption rejected");

    /* sigR longer than the P-256 curve parameter (32 bytes) -- a wrong-curve or
     * malformed ECDSA component, even though it would otherwise fit the buffer. */
    n = build_quote_response(rsp);
    soff = 16u + (uint32_t)tpm2_be16_get(rsp + 14);
    tpm2_be16_put(rsp + soff + 4u, 40u);             /* sigR size 40 > 32 */
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "overlong-for-P256 sigR rejected");

    /* Wrong pcrDigest length: a SHA-256 quote's digest is exactly 32 bytes. A
     * consistent-but-wrong-length digest (64 or 0) must be rejected, not reported
     * as a valid quote. bq_digest_len keeps the rest of the response self-consistent
     * so it is the digest-length check -- not exact-consumption -- that fires. */
    bq_digest_len = 64u;
    n = build_quote_response(rsp);
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "64-byte pcrDigest (not SHA-256) rejected");
    bq_digest_len = 0u;
    n = build_quote_response(rsp);
    TEST_ASSERT_EQ(tpm2_parse_quote(rsp, n, &att, sig, sizeof sig, &sig_len), -1,
                   "empty pcrDigest rejected");
    bq_digest_len = 32u;                             /* restore default for later tests */
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

/* ---- Wrapper no-TPM degrade (never gate boot on attestation) ---- */

static void test_attest_no_tpm(void)
{
    struct tpm_t_test_state prev;
    struct tpm_quote_attest att;
    uint8_t nonce[16], sig[64], pub[64], cert[64];
    uint16_t got = 0;
    uint32_t slen = 0, i;
    for (i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)i;

    /* Force the transport unavailable: every attestation entry point must report
     * a degraded status, never hang or fault (attestation NEVER gates boot). */
    prev = tpm_t_test_install(0, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)tpm2_quote(0xFFu, nonce, sizeof nonce, &att, sig, sizeof sig, &slen),
                   (int)TPM_ATTEST_NO_TPM, "quote with no TPM -> NO_TPM");
    TEST_ASSERT_EQ((int)tpm_ek_cert_read(TPM_ALG_ECC, cert, sizeof cert, &got),
                   (int)TPM_ATTEST_NO_TPM, "EK cert read with no TPM -> NO_TPM");
    tpm_t_test_restore(prev);

    /* Bad args are rejected before any transaction. */
    TEST_ASSERT_EQ((int)tpm2_quote(0xFFu, nonce, 4u, &att, sig, sizeof sig, &slen),
                   (int)TPM_ATTEST_BADARG, "quote rejects too-short nonce");
    TEST_ASSERT_EQ((int)tpm2_quote(0xFFu, 0, 0u, &att, sig, sizeof sig, &slen),
                   (int)TPM_ATTEST_BADARG, "quote rejects null nonce");
    /* A PCR bit >= 24 is not representable by the single 3-octet SHA-256 selection
     * this path quotes; it must be rejected, never silently dropped + attested. */
    TEST_ASSERT_EQ((int)tpm2_quote(0x01000000u, nonce, sizeof nonce, &att, sig, sizeof sig, &slen),
                   (int)TPM_ATTEST_BADARG, "quote rejects out-of-range PCR bit (>= 24)");
    TEST_ASSERT_EQ((int)tpm_ak_public_get(0, sizeof pub, &got),
                   (int)TPM_ATTEST_BADARG, "ak_public_get rejects null out");
}

/* ---- CC-dispatch fake-TIS: the full provision + quote lifecycle ---- */

#define AF_CAP 512u
static uint8_t  af_cmd[AF_CAP];
static uint32_t af_cmd_len, af_cmd_expect;
static uint8_t  af_rsp[AF_CAP];
static uint32_t af_rsp_len, af_rsp_pos;
static int      af_ready, af_executed;
static uint32_t af_seen[32], af_seen_n, af_flush_n;
static uint32_t af_policy_secret_n;   /* count PolicySecret calls (must be 2) */
static uint32_t af_load_auth;         /* the auth handle the Load command carried */
static uint8_t  af_quote_pcr0 = 0xFFu;             /* fake CC_QUOTE pcrSelect byte 0 (corrupt to test the bind check) */
static uint16_t af_quote_hashalg = TPM_ALG_SHA256; /* fake CC_QUOTE sig hashAlg (corrupt to test the bind check) */
static uint16_t af_quote_bankalg = TPM_ALG_SHA256; /* fake CC_QUOTE PCR-bank hashAlg (corrupt to test the bank bind) */
static uint32_t af_stall_reads;   /* withhold commandReady for N status polls: a
                                   * slow-but-responsive TPM, exactly the shape a
                                   * cumulative sequence budget exists to bound. */
static uint32_t af_stall_after_go; /* withhold dataAvail for N status polls AFTER
                                    * the command was dispatched (mirrors
                                    * test_tpm_nv.c's nvf_stall_after_go): the
                                    * abandoned-mid-flight case. */
static uint32_t af_fail_cc;       /* command code to fail with af_fail_rc */
static uint32_t af_fail_rc;
static int      af_fail_times;    /* remaining failures; <0 = forever */

#define AF_REG_STS  0x018u
#define AF_REG_FIFO 0x024u
#define AF_STS_EXPECT 0x08u
#define AF_STS_DATA_AVAIL 0x10u
#define AF_STS_GO 0x20u
#define AF_STS_CMD_READY 0x40u
#define AF_STS_VALID 0x80u

static uint32_t af_put_auth(uint32_t off)
{
    tpm2_be16_put(af_rsp + off, 0u); off += 2u;   /* nonceTPM */
    af_rsp[off] = 0u; off += 1u;                  /* attributes */
    tpm2_be16_put(af_rsp + off, 0u); off += 2u;   /* hmac */
    return off;
}

static void af_build_response(void)
{
    uint32_t cc = tpm2_be32_get(af_cmd + 6);
    uint32_t off, i;
    uint16_t nlen;
    if (af_seen_n < 32u) af_seen[af_seen_n++] = cc;
    if (cc == TPM2_CC_FLUSH_CONTEXT) af_flush_n++;
    if (cc == TPM2_CC_POLICY_SECRET) af_policy_secret_n++;
    /* Load auth handle is at cmd offset 18 (header10 + parent4 + authSize4). */
    if (cc == TPM2_CC_LOAD) af_load_auth = tpm2_be32_get(af_cmd + 18);
    memset(af_rsp, 0, AF_CAP);

    if (af_fail_cc != 0u && cc == af_fail_cc && af_fail_times != 0) {
        if (af_fail_times > 0) af_fail_times--;
        /* A TRANSIENT-WARNING response: a bare ST_NO_SESSIONS header carrying
         * the injected rc, which is exactly the shape a real FlushContext
         * warning takes (it carries no auth area either way). */
        tpm2_be16_put(af_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(af_rsp + 2, 10u);
        tpm2_be32_put(af_rsp + 6, af_fail_rc);
        af_rsp_len = 10u;
        return;
    }

    if (cc == TPM2_CC_CREATE_PRIMARY || cc == TPM2_CC_LOAD) {
        /* leading objectHandle(4) + parameterSize(4) + auth(5). CreatePrimary
         * carries a real TPM2B_PUBLIC in its parameters (a plausible ECC-P256
         * EK size) so tpm2_parse_create_primary_public -- reached only from EK
         * capture, not from at_provision's handle-only path -- has a well-formed
         * outPublic to parse; Load has no such payload (psize stays 0). */
        uint16_t pub_len = (cc == TPM2_CC_CREATE_PRIMARY) ? 122u : 0u;
        uint32_t psize = (pub_len != 0u) ? (uint32_t)pub_len + 2u : 0u;
        tpm2_be16_put(af_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(af_rsp + 6, TPM2_RC_SUCCESS);
        tpm2_be32_put(af_rsp + 10, (cc == TPM2_CC_CREATE_PRIMARY) ? 0x80000001u : 0x80000002u);
        tpm2_be32_put(af_rsp + 14, psize);
        if (pub_len != 0u) {
            tpm2_be16_put(af_rsp + 18, pub_len);
            for (i = 0; i < (uint32_t)pub_len; i++)
                af_rsp[20u + i] = (uint8_t)(0xC0u + i);
        }
        off = af_put_auth(18u + psize);
        tpm2_be32_put(af_rsp + 2, off);
        af_rsp_len = off;
    } else if (cc == TPM2_CC_START_AUTH_SESSION) {
        tpm2_be16_put(af_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(af_rsp + 6, TPM2_RC_SUCCESS);
        tpm2_be32_put(af_rsp + 2, 32u);
        tpm2_be32_put(af_rsp + 10, 0x03000000u);   /* policy session */
        tpm2_be16_put(af_rsp + 14, 16u);           /* nonceTPM (16 zero) */
        af_rsp_len = 32u;
    } else if (cc == TPM2_CC_POLICY_SECRET) {
        /* params: timeout TPM2B(2,0) + ticket{tag(2)+hierarchy(4)+digest(2,0)}=10. */
        tpm2_be16_put(af_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(af_rsp + 6, TPM2_RC_SUCCESS);
        tpm2_be32_put(af_rsp + 10, 10u);
        /* timeout@14=0, ticket@16 (8 bytes zero) -> end @24. */
        off = af_put_auth(24u);
        tpm2_be32_put(af_rsp + 2, off);
        af_rsp_len = off;
    } else if (cc == TPM2_CC_CREATE) {
        /* Near-realistic AK blob: outPriv(2+200) + outPub(2+100) + creationData/
         * Hash(2,0 each) + ticket(8) -> exercises the Load command buffer. */
        tpm2_be16_put(af_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(af_rsp + 6, TPM2_RC_SUCCESS);
        tpm2_be16_put(af_rsp + 14, 200u);
        for (i = 0; i < 200u; i++) af_rsp[16 + i] = (uint8_t)(0x70u + (i & 0x3Fu));
        tpm2_be16_put(af_rsp + 216, 100u);                       /* AK pub @ 16+200 */
        for (i = 0; i < 100u; i++) af_rsp[218 + i] = (uint8_t)(0x80u + (i & 0x3Fu));
        /* creationData@318/creationHash@320/ticket@322..329 zero -> params end @330. */
        tpm2_be32_put(af_rsp + 10, 330u - 14u);                  /* parameterSize 316 */
        off = af_put_auth(330u);
        tpm2_be32_put(af_rsp + 2, off);
        af_rsp_len = off;
    } else if (cc == TPM2_CC_QUOTE) {
        /* Echo the command's qualifyingData nonce (at cmd offset 27) into the
         * attest extraData, then a minimal ECDSA signature. */
        uint32_t a, attest_off, sig_n;
        nlen = tpm2_be16_get(af_cmd + 27);
        tpm2_be16_put(af_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(af_rsp + 6, TPM2_RC_SUCCESS);
        attest_off = 16u;                          /* params@14 -> size(2) -> attest@16 */
        a = attest_off;
        tpm2_be32_put(af_rsp + a, TPM2_GENERATED_VALUE); a += 4u;
        tpm2_be16_put(af_rsp + a, TPM2_ST_ATTEST_QUOTE); a += 2u;
        tpm2_be16_put(af_rsp + a, 0u); a += 2u;    /* qualifiedSigner empty */
        tpm2_be16_put(af_rsp + a, nlen); a += 2u;  /* extraData = echoed nonce */
        for (i = 0; i < nlen; i++) af_rsp[a + i] = af_cmd[29 + i];
        a += nlen;
        for (i = 0; i < 17u; i++) af_rsp[a + i] = 0u; a += 17u; /* clockInfo */
        for (i = 0; i < 8u; i++) af_rsp[a + i] = 0u; a += 8u;   /* fwVersion */
        tpm2_be32_put(af_rsp + a, 1u); a += 4u;    /* TPML count */
        tpm2_be16_put(af_rsp + a, af_quote_bankalg); a += 2u;
        af_rsp[a] = 3u; a += 1u; af_rsp[a] = af_quote_pcr0; af_rsp[a+1u] = 0u; af_rsp[a+2u] = 0u; a += 3u;
        tpm2_be16_put(af_rsp + a, 32u); a += 2u;
        for (i = 0; i < 32u; i++) af_rsp[a + i] = (uint8_t)(0xC0u + i); a += 32u;
        tpm2_be16_put(af_rsp + 14, (uint16_t)(a - attest_off));  /* TPM2B_ATTEST size */
        /* TPMT_SIGNATURE (ECDSA): sigAlg + hash + R(2+32) + S(2+32). */
        tpm2_be16_put(af_rsp + a, TPM_ALG_ECDSA); a += 2u;
        tpm2_be16_put(af_rsp + a, af_quote_hashalg); a += 2u;
        tpm2_be16_put(af_rsp + a, 32u); a += 2u;
        for (i = 0; i < 32u; i++) af_rsp[a + i] = (uint8_t)(0x10u + i); a += 32u;
        tpm2_be16_put(af_rsp + a, 32u); a += 2u;
        for (i = 0; i < 32u; i++) af_rsp[a + i] = (uint8_t)(0x90u + i); a += 32u;
        sig_n = a - 14u;
        tpm2_be32_put(af_rsp + 10, sig_n);         /* parameterSize */
        off = af_put_auth(a);
        tpm2_be32_put(af_rsp + 2, off);
        af_rsp_len = off;
    } else if (cc == TPM2_CC_NV_READ_PUBLIC) {
        /* Report a 2048-byte EK cert for the ECC EK index (oversize test). */
        tpm2_be16_put(af_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(af_rsp + 6, TPM2_RC_SUCCESS);
        tpm2_be16_put(af_rsp + 10, 14u);                  /* nvPublic inner size */
        tpm2_be32_put(af_rsp + 12, TPM_NV_INDEX_EK_CERT_ECC);
        tpm2_be16_put(af_rsp + 16, TPM_ALG_SHA256);
        tpm2_be32_put(af_rsp + 18, 0u);                   /* attributes */
        tpm2_be16_put(af_rsp + 22, 0u);                   /* authPolicy size */
        tpm2_be16_put(af_rsp + 24, 2048u);                /* dataSize */
        tpm2_be16_put(af_rsp + 26, 0u);                   /* TPM2B_NAME size */
        tpm2_be32_put(af_rsp + 2, 28u);
        af_rsp_len = 28u;
    } else {
        tpm2_be16_put(af_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(af_rsp + 2, 10u);
        tpm2_be32_put(af_rsp + 6, TPM2_RC_SUCCESS);
        af_rsp_len = 10u;
    }
}

static uint8_t af_r8(uint32_t o)
{
    if (o == AF_REG_FIFO && af_executed && af_rsp_pos < af_rsp_len) return af_rsp[af_rsp_pos++];
    return 0;
}
static void af_w8(uint32_t o, uint8_t v)
{
    if (o != AF_REG_FIFO || af_executed) return;
    if (af_cmd_len < AF_CAP) af_cmd[af_cmd_len] = v;
    af_cmd_len++;
    if (af_cmd_len == 6u) af_cmd_expect = tpm2_be32_get(af_cmd + 2);
}
static uint32_t af_r32(uint32_t o)
{
    uint8_t s;
    if (o != AF_REG_STS) return 0;
    s = AF_STS_VALID;
    if (af_stall_reads > 0u) { af_stall_reads--; return (uint32_t)s | (32u << 8); }
    if (af_executed && af_stall_after_go > 0u) {
        af_stall_after_go--;
        return (uint32_t)s | (32u << 8);
    }
    if (af_ready && !af_executed && af_cmd_len == 0) s |= AF_STS_CMD_READY;
    if (!af_executed && af_cmd_len > 0 && af_cmd_len < af_cmd_expect) s |= AF_STS_EXPECT;
    if (af_executed && af_rsp_pos < af_rsp_len) s |= AF_STS_DATA_AVAIL;
    return (uint32_t)s | (32u << 8);
}
static void af_w32(uint32_t o, uint32_t v)
{
    if (o != AF_REG_STS) return;
    if (v & AF_STS_CMD_READY) { af_ready = 1; af_executed = 0; af_cmd_len = 0; af_cmd_expect = 0; af_rsp_pos = 0; }
    if ((v & AF_STS_GO) && af_cmd_len >= af_cmd_expect) { af_executed = 1; af_build_response(); }
}
static const struct tpm_t_io af_io = { af_r8, af_w8, af_r32, af_w32 };

static int af_saw(uint32_t cc)
{
    uint32_t i;
    for (i = 0; i < af_seen_n; i++) if (af_seen[i] == cc) return 1;
    return 0;
}

/* Reset the fake-TIS observers + corrupt-flag globals to defaults AND drop the
 * production AK cache, so each attestation test runs the full provisioning path
 * regardless of order (no reliance on a sibling test's cached AK). */
static void af_reset(void)
{
    af_cmd_len = 0; af_rsp_len = 0; af_rsp_pos = 0; af_ready = 0; af_executed = 0;
    af_seen_n = 0; af_flush_n = 0; af_policy_secret_n = 0; af_load_auth = 0;
    af_quote_pcr0 = 0xFFu; af_quote_hashalg = TPM_ALG_SHA256; af_quote_bankalg = TPM_ALG_SHA256;
    bq_digest_len = 32u;
    af_stall_reads = 0u;
    af_stall_after_go = 0u;
    af_fail_cc = 0u; af_fail_rc = 0u; af_fail_times = -1;
    tpm_attest_test_reset();
}

static void test_attest_quote_lifecycle(void)
{
    struct tpm_t_test_state prev;
    struct tpm_quote_attest att;
    uint8_t nonce[20], sig[128], pub[128];
    uint16_t pub_len = 0;
    uint32_t slen = 0, i;
    tpm_attest_status_t r;
    for (i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)(0x55u + i);

    af_reset();                          /* fresh provisioning, independent of test order */
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    memset(&att, 0, sizeof att);
    r = tpm2_quote(0xFFu, nonce, sizeof nonce, &att, sig, sizeof sig, &slen);
    /* The same provisioned AK serves a second query (no re-provision). */
    {
        tpm_attest_status_t r2 = tpm_ak_public_get(pub, sizeof pub, &pub_len);
        tpm_t_test_restore(prev);
        TEST_ASSERT_EQ((int)r2, (int)TPM_ATTEST_OK, "ak_public_get returns cached AK pub");
        TEST_ASSERT_EQ((uint32_t)pub_len, 100u, "cached AK pub is the Create outPublic (100 bytes)");
    }
    TEST_ASSERT_EQ((int)r, (int)TPM_ATTEST_OK, "provision + quote succeeds");
    TEST_ASSERT(af_saw(TPM2_CC_CREATE_PRIMARY), "EK CreatePrimary issued");
    TEST_ASSERT(af_saw(TPM2_CC_START_AUTH_SESSION), "policy session opened");
    TEST_ASSERT_EQ(af_policy_secret_n, 2u, "PolicySecret run TWICE (re-satisfied before Load)");
    TEST_ASSERT(af_saw(TPM2_CC_CREATE), "AK Create issued");
    TEST_ASSERT(af_saw(TPM2_CC_LOAD), "AK Load issued");
    /* F-A1: Load MUST be authorized by the policy session (the EK is policy-auth,
     * not password) -- a TPM_RS_PW Load would fail on a real EK. */
    TEST_ASSERT_EQ(af_load_auth, 0x03000000u, "AK Load authorized by the policy session");
    TEST_ASSERT(af_saw(TPM2_CC_QUOTE), "Quote issued");
    TEST_ASSERT(af_flush_n >= 2u, "EK + session flushed (AK kept loaded)");
    /* Anti-replay: the parsed attest echoes our exact nonce. */
    TEST_ASSERT_EQ((uint32_t)att.nonce_len, 20u, "attest echoes 20-byte nonce");
    TEST_ASSERT(att.nonce[0] == 0x55u && att.nonce[19] == 0x68u, "attest nonce == supplied nonce");
    TEST_ASSERT_EQ(att.pcr_select, 0xFFu, "attest pcrSelect PCRs 0-7");
    TEST_ASSERT(slen == 72u, "ECDSA signature blob returned");
}

/* The wrapper binds a reported-OK quote to the REQUEST: a response that parses but
 * covers the wrong PCR selection, or carries the wrong signature hash, must be
 * rejected -- never published as a valid attestation (adversarial-review hardening). */
static void test_attest_quote_bind_reject(void)
{
    struct tpm_t_test_state prev;
    struct tpm_quote_attest att;
    uint8_t nonce[20], sig[128];
    uint32_t slen = 0, i;
    for (i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)(0x33u + i);

    /* Wrong PCR selection: the TPM echoes PCRs 0-3 but the caller requested 0-7.
     * af_reset() drops any cached AK so this test provisions on its own. */
    af_reset();
    af_quote_pcr0 = 0x0Fu;
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    memset(&att, 0, sizeof att);
    TEST_ASSERT_EQ((int)tpm2_quote(0xFFu, nonce, sizeof nonce, &att, sig, sizeof sig, &slen),
                   (int)TPM_ATTEST_QUOTE_FAIL, "quote over wrong PCR set rejected");

    /* Wrong signature hash: SHA-384 where SHA-256 was requested (parses, fails bind). */
    af_cmd_len = 0; af_rsp_len = 0; af_rsp_pos = 0; af_ready = 0; af_executed = 0;
    af_quote_pcr0 = 0xFFu; af_quote_hashalg = 0x000Cu;   /* TPM_ALG_SHA384 */
    memset(&att, 0, sizeof att);
    TEST_ASSERT_EQ((int)tpm2_quote(0xFFu, nonce, sizeof nonce, &att, sig, sizeof sig, &slen),
                   (int)TPM_ATTEST_QUOTE_FAIL, "quote with wrong sig hash rejected");

    /* Wrong PCR bank: the TPM quotes a SHA-384 bank, not the requested SHA-256 -- a
     * matching bitmap over the wrong bank must not be accepted. */
    af_cmd_len = 0; af_rsp_len = 0; af_rsp_pos = 0; af_ready = 0; af_executed = 0;
    af_quote_hashalg = TPM_ALG_SHA256; af_quote_bankalg = 0x000Cu;   /* TPM_ALG_SHA384 bank */
    memset(&att, 0, sizeof att);
    TEST_ASSERT_EQ((int)tpm2_quote(0xFFu, nonce, sizeof nonce, &att, sig, sizeof sig, &slen),
                   (int)TPM_ATTEST_QUOTE_FAIL, "quote over wrong PCR bank rejected");

    tpm_t_test_restore(prev);
    af_quote_pcr0 = 0xFFu; af_quote_hashalg = TPM_ALG_SHA256; af_quote_bankalg = TPM_ALG_SHA256;
}

static void test_attest_ek_cert_oversize(void)
{
    struct tpm_t_test_state prev;
    uint8_t cert[64];
    uint16_t got = 0;
    af_cmd_len = 0; af_rsp_len = 0; af_rsp_pos = 0; af_ready = 0; af_executed = 0; af_seen_n = 0;
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    /* The ECC EK index reports 2048 bytes; with a 64-byte buffer the read MUST
     * NOT return OK with a truncated cert -- a verifier could trust a partial DER. */
    TEST_ASSERT_EQ((int)tpm_ek_cert_read(TPM_ALG_ECC, cert, sizeof cert, &got),
                   (int)TPM_ATTEST_BADARG, "oversized EK cert -> BADARG (no truncated OK)");
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((uint32_t)got, 2048u, "oversized EK cert reports the required size");
}


/* ---- CreatePrimary outPublic parser -----------------------------------------
 *
 * This parser reads a FIRMWARE response with hand-rolled bounds and feeds the
 * device identity the headless enrollment authorization is bound to, so a
 * wrong bound here would surface only on real hardware. Every case below is a
 * malformed shape asserted to leave `out` untouched and `out_len` zero, beside
 * a valid control -- without the control the whole table would pass against a
 * parser that rejected everything.
 */

#define CPP_HANDLE 0x80000001u

/* Build a CreatePrimary success response carrying `pub_len` public bytes.
 * `psize` and `handle` are overridable so a case can corrupt exactly one
 * field. Returns the total response length. */
static uint32_t cpp_build(uint8_t *rsp, uint32_t cap, uint16_t pub_len,
                          uint32_t psize, uint32_t handle, uint16_t tag,
                          uint32_t rc)
{
    uint32_t size = 18u + psize + 5u;   /* + minimal one-session auth area */
    uint32_t i;
    if (cap < size)
        return 0u;
    for (i = 0; i < size; i++)
        rsp[i] = 0u;
    tpm2_be16_put(rsp + 0, tag);
    tpm2_be32_put(rsp + 2, size);
    tpm2_be32_put(rsp + 6, rc);
    tpm2_be32_put(rsp + 10, handle);
    tpm2_be32_put(rsp + 14, psize);
    tpm2_be16_put(rsp + 18, pub_len);
    for (i = 0; i < (uint32_t)pub_len; i++)
        rsp[20u + i] = (uint8_t)(0xC0u + i);      /* recognizable public bytes */
    /* Minimal TPMS_AUTH_RESPONSE at 18+psize: nonce(2,0) attrs(1) hmac(2,0). */
    return size;
}

/* A well-formed response with a plausible ECC EK public area. */
static uint32_t cpp_valid(uint8_t *rsp, uint32_t cap, uint16_t pub_len)
{
    return cpp_build(rsp, cap, pub_len, (uint32_t)pub_len + 2u, CPP_HANDLE,
                     TPM2_ST_SESSIONS, TPM2_RC_SUCCESS);
}

static void test_attest_cpp_valid(void)
{
    uint8_t rsp[256], out[TPM_EK_PUB_MAX];
    uint16_t out_len = 0xFFFFu;
    uint32_t n, i;

    n = cpp_valid(rsp, sizeof rsp, 122u);       /* the real ECC-P256 EK size */
    TEST_ASSERT(n != 0u, "fixture builds a response");
    TEST_ASSERT_EQ(tpm2_parse_create_primary_public(rsp, n, out,
                                                    (uint16_t)sizeof out, &out_len),
                   0, "a well-formed CreatePrimary response parses");
    TEST_ASSERT_EQ(out_len, 122u, "the full public area length is reported");
    {
        uint32_t bad = 0;
        for (i = 0; i < 122u; i++) {
            if (out[i] != (uint8_t)(0xC0u + i))
                bad++;
        }
        TEST_ASSERT_EQ(bad, 0u, "the public bytes are copied verbatim");
    }

    /* Exactly at capacity is legal; one byte over is refused, never clamped. */
    n = cpp_valid(rsp, sizeof rsp, (uint16_t)sizeof out);
    TEST_ASSERT_EQ(tpm2_parse_create_primary_public(rsp, n, out,
                                                    (uint16_t)sizeof out, &out_len),
                   0, "a public area exactly at capacity is accepted");
    n = cpp_valid(rsp, sizeof rsp, (uint16_t)(sizeof out + 1u));
    out_len = 0xFFFFu;
    TEST_ASSERT_EQ(tpm2_parse_create_primary_public(rsp, n, out,
                                                    (uint16_t)sizeof out, &out_len),
                   -1, "one byte over capacity is refused, not truncated");
    TEST_ASSERT_EQ(out_len, 0u, "a refusal reports zero length");
}

static void test_attest_cpp_malformed(void)
{
    uint8_t rsp[256], out[TPM_EK_PUB_MAX];
    uint16_t out_len;
    uint32_t n;

    /* Every case sets out_len to a sentinel first, so "left at zero" is a real
     * assertion rather than a variable that was never written. */
#define CPP_REFUSED(desc)                                                     \
    do {                                                                      \
        out_len = 0xFFFFu;                                                    \
        TEST_ASSERT_EQ(tpm2_parse_create_primary_public(rsp, n, out,           \
                           (uint16_t)sizeof out, &out_len), -1, desc);        \
        TEST_ASSERT_EQ(out_len, 0u, "a refusal leaves out_len zero");          \
    } while (0)

    n = cpp_valid(rsp, sizeof rsp, 122u);
    out_len = 0xFFFFu;
    TEST_ASSERT_EQ(tpm2_parse_create_primary_public(0, n, out,
                                                    (uint16_t)sizeof out, &out_len),
                   -1, "a NULL response is refused");
    TEST_ASSERT_EQ(tpm2_parse_create_primary_public(rsp, n, 0,
                                                    (uint16_t)sizeof out, &out_len),
                   -1, "a NULL output buffer is refused");

    /* A non-success rc must never yield a public area, however well-formed the
     * rest of the response looks. */
    n = cpp_build(rsp, sizeof rsp, 122u, 124u, CPP_HANDLE, TPM2_ST_SESSIONS, 0x101u);
    CPP_REFUSED("a non-success response code is refused");

    n = cpp_build(rsp, sizeof rsp, 122u, 124u, CPP_HANDLE, TPM2_ST_NO_SESSIONS,
                  TPM2_RC_SUCCESS);
    CPP_REFUSED("a NO_SESSIONS tag is refused (CreatePrimary is session-authorized)");

    /* THE HANDLE CHECK. A success naming a persistent or NULL handle is a
     * malformed response, not an object this parser may describe. */
    n = cpp_build(rsp, sizeof rsp, 122u, 124u, 0x81000001u, TPM2_ST_SESSIONS,
                  TPM2_RC_SUCCESS);
    CPP_REFUSED("a persistent (non-transient) object handle is refused");
    n = cpp_build(rsp, sizeof rsp, 122u, 124u, 0u, TPM2_ST_SESSIONS,
                  TPM2_RC_SUCCESS);
    CPP_REFUSED("a zero object handle is refused");

    /* parameterSize too small to hold even the TPM2B size prefix. */
    n = cpp_build(rsp, sizeof rsp, 122u, 1u, CPP_HANDLE, TPM2_ST_SESSIONS,
                  TPM2_RC_SUCCESS);
    CPP_REFUSED("a parameterSize below the TPM2B prefix is refused");

    /* The declared public area runs past the parameter area it lives in. */
    n = cpp_build(rsp, sizeof rsp, 122u, 60u, CPP_HANDLE, TPM2_ST_SESSIONS,
                  TPM2_RC_SUCCESS);
    CPP_REFUSED("a public area crossing parameterSize is refused");

    /* Shorter than any TPMT_PUBLIC can be: not a truncated key, a response
     * that never carried one. */
    n = cpp_valid(rsp, sizeof rsp, (uint16_t)(TPM_PUBLIC_MIN_LEN - 1u));
    CPP_REFUSED("a public area below the minimum TPMT_PUBLIC is refused");
    n = cpp_valid(rsp, sizeof rsp, 0u);
    CPP_REFUSED("a zero-length public area is refused");

    /* A response truncated below the fixed header + handle + psize + auth
     * minimum cannot be parsed at all. */
    n = cpp_valid(rsp, sizeof rsp, 122u);
    (void)n;
    out_len = 0xFFFFu;
    TEST_ASSERT_EQ(tpm2_parse_create_primary_public(rsp, 22u, out,
                                                    (uint16_t)sizeof out, &out_len),
                   -1, "a response below the structural minimum is refused");

    /* Control LAST: the same fixture still parses, so none of the refusals
     * above came from a parser that simply stopped working. */
    n = cpp_valid(rsp, sizeof rsp, 122u);
    out_len = 0u;
    TEST_ASSERT_EQ(tpm2_parse_create_primary_public(rsp, n, out,
                                                    (uint16_t)sizeof out, &out_len),
                   0, "control: the valid response still parses");
    TEST_ASSERT_EQ(out_len, 122u, "control: and still reports its length");
#undef CPP_REFUSED
}


/* ---- Section 24: bounded sequence + verified teardown on attestation ----
 *
 * The same three properties asserted in test_tpm_seal.c, for the six-command
 * AK provisioning flow (at_provision) and the EK-capture path
 * (at_capture_ek_public). Reuses the fake TIS harness's existing af_flush_n /
 * af_policy_secret_n counters -- the teardown properties are checked by
 * COUNTING commands, never by the return value alone.
 */

/* ONE budget across all six provisioning commands (CreatePrimary EK,
 * StartAuthSession, PolicySecret x2, Create AK, Load), not one per command. */
static void test_attest_provision_one_budget(void)
{
    struct tpm_t_test_state prev;
    tpm_attest_status_t r;
    uint8_t nonce[20], sig[128], att_out[8];
    struct tpm_quote_attest att;
    uint32_t slen = 0, i;
    for (i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)(0x11u + i);
    (void)att_out;

    /* CONTROL: the same stalling TPM still completes provisioning under a
     * generous budget. */
    af_reset();
    af_stall_reads = 4u;
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(60000u, 1000u);
    memset(&att, 0, sizeof att);
    r = tpm2_quote(0xFFu, nonce, sizeof nonce, &att, sig, sizeof sig, &slen);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)r, (int)TPM_ATTEST_OK,
                   "CONTROL: a slow-but-responsive TPM still provisions+quotes "
                   "inside a generous budget");

    /* Pre-dispatch: budget already spent before CreatePrimary EK (the first
     * provisioning command) ever touches the interface. Nothing was
     * submitted, so this is exactly as safe to retry as ordinary contention. */
    af_reset();
    af_stall_reads = 4u;
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(0u, 1000u);
    memset(&att, 0, sizeof att);
    r = tpm2_quote(0xFFu, nonce, sizeof nonce, &att, sig, sizeof sig, &slen);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)r, (int)TPM_ATTEST_BUSY,
                   "a pre-dispatch budget refusal reports the retryable "
                   "class: nothing was ever submitted");

    /* The genuinely-in-flight case (dispatched, then abandoned) is proven at
     * the transport layer instead of here: test_seq_last_submit_dispatched
     * (test_tpm_nv.c) deterministically tests tpm2_seq_last_submit_dispatched()
     * itself -- the primitive at_provision_err_handle/at_exec_handle gate on --
     * because reaching that exact real-time race through this fixture has no
     * reliable bound (see test_seal_one_budget_per_operation for the full
     * rationale; identical here). */
}

/* EK capture (tpm_ek_public_get, reached before any AK exists) is its own
 * bounded sequence and its own verified teardown -- it is NOT part of
 * at_provision, so a missed conversion here would leave every EK-capture call
 * leaking a transient handle on success while the return value read OK. */
static void test_attest_ek_capture_one_budget_and_teardown(void)
{
    struct tpm_t_test_state prev;
    tpm_attest_status_t r;
    uint8_t pub[256];
    uint16_t pub_len = 0;

    af_reset();
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    r = tpm_ek_public_get(pub, sizeof pub, &pub_len);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)r, (int)TPM_ATTEST_OK, "CONTROL: EK capture succeeds");
    TEST_ASSERT_EQ((int)af_flush_n, 1,
                   "a clean EK capture proves exactly one teardown -- the EK "
                   "transient it created");

    /* Pre-dispatch: budget already spent before CreatePrimary ever touches the
     * interface. Nothing was submitted, so this is exactly as safe to retry
     * as ordinary contention -- not TRANSPORT either (a device-fault reading
     * would send a caller into hard-failure recovery). */
    af_reset();
    af_stall_reads = 4u;
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(0u, 1000u);
    r = tpm_ek_public_get(pub, sizeof pub, &pub_len);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)r, (int)TPM_ATTEST_BUSY,
                   "a pre-dispatch budget refusal reports the retryable "
                   "class: nothing was ever submitted");

    /* The genuinely-in-flight case is proven at the transport layer
     * (test_seq_last_submit_dispatched, test_tpm_nv.c) -- see
     * test_attest_provision_one_budget for the full rationale; identical
     * here. */
}

/* A TEARDOWN REQUIRES PROOF on the attestation side too: the old at_flush
 * submitted FlushContext and discarded the result, so a TPM_RC_RETRY left the
 * EK transient allocated while the code proceeded as though it were released.
 * Verified via the SAME transient-then-clears shape as the seal test, checking
 * the FLUSH COUNT rather than the operation's return value. */
static void test_attest_teardown_requires_proof(void)
{
    struct tpm_t_test_state prev;
    tpm_attest_status_t r;
    uint8_t pub[256];
    uint16_t pub_len = 0;
    uint32_t flushes_clean, flushes_retried;

    /* BASELINE: a TPM that proves the flush first time. */
    af_reset();
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    r = tpm_ek_public_get(pub, sizeof pub, &pub_len);
    flushes_clean = af_flush_n;
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)r, (int)TPM_ATTEST_OK, "CONTROL: a clean capture succeeds");
    TEST_ASSERT_EQ((int)flushes_clean, 1,
                   "CONTROL: a proven EK teardown costs exactly one FlushContext");

    /* TRANSIENT WARNING, once: FlushContext answers TPM2_RC_RETRY on its first
     * attempt then succeeds. More than one FlushContext is the observable that
     * separates "retried" from "accepted the first answer as proof". */
    af_reset();
    af_fail_cc = TPM2_CC_FLUSH_CONTEXT; af_fail_rc = TPM2_RC_RETRY; af_fail_times = 1;
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    r = tpm_ek_public_get(pub, sizeof pub, &pub_len);
    flushes_retried = af_flush_n;
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)r, (int)TPM_ATTEST_OK,
                   "a transient teardown warning does not fail the capture itself");
    TEST_ASSERT(flushes_retried > flushes_clean,
                "a transient TPM_RC_RETRY is RETRIED, not accepted as proof");

    /* NEVER PROVEN: bounded retry, then REPORT rather than escalate -- the
     * capture still succeeds (the public area was already read) and the
     * transport stays usable, because poisoning it would take out PCR reads
     * that never opened a handle at all. */
    af_reset();
    af_fail_cc = TPM2_CC_FLUSH_CONTEXT; af_fail_rc = TPM2_RC_RETRY; af_fail_times = -1;
    prev = tpm_t_test_install(&af_io, TPM_T_IFACE_TIS, 1);
    r = tpm_ek_public_get(pub, sizeof pub, &pub_len);
    TEST_ASSERT(af_flush_n > 1u,
                "an unproven EK teardown retries rather than accepting one answer");
    TEST_ASSERT(af_flush_n <= TPM_NV_FLUSH_RETRIES,
                "and the retry is BOUNDED, never an unbounded loop");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "an unproven teardown is REPORTED, not escalated into a "
                   "transport-wide outage");
    tpm_t_test_restore(prev);
}

void test_register_tpm_attest(void)
{
    test_suite_register_cat("tpm: attestation provision one budget",
                            test_attest_provision_one_budget, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: EK capture one budget and teardown",
                            test_attest_ek_capture_one_budget_and_teardown,
                            TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attestation teardown requires proof",
                            test_attest_teardown_requires_proof, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: EK CreatePrimary marshal", test_attest_build_ek, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PolicySecret marshal", test_attest_build_policy_secret, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: AK signing-key marshal", test_attest_build_ak, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: Quote marshal", test_attest_build_quote, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: quote-response (TPMS_ATTEST) parse", test_attest_parse_quote, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: quote signature-structure rejection",
                            test_attest_parse_quote_sig_reject, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: EvictControl marshal", test_attest_build_evict, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attestation no-TPM degrade", test_attest_no_tpm, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attestation provision+quote lifecycle",
                            test_attest_quote_lifecycle, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: quote request/response bind rejection",
                            test_attest_quote_bind_reject, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: EK cert oversize rejected", test_attest_ek_cert_oversize, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: CreatePrimary outPublic parses",
                            test_attest_cpp_valid, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: CreatePrimary outPublic malformed shapes",
                            test_attest_cpp_malformed, TEST_CAT_SECURITY);
}
