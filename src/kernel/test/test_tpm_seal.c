/* ============================================================================
 * test_tpm_seal.c -- TPM2 sealed-secret boot policy hook tests
 *
 * Pure marshaling/parse tests (no MMIO) for CreatePrimary/Create/Load/Unseal,
 * plus a CC-dispatch fake-TIS transport for the full seal + unseal lifecycle:
 * round-trip seal, unseal success, a PCR-drift POLICY_FAIL (with the recovery
 * handler firing + every handle flushed), and the seal-mask excluding PCR 11.
 * No live boot infrastructure. The live seal -> PCR-change -> deny cycle is
 * swtpm/bare-metal validation.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_seal.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm_pcr_alloc.h"
#include "libc/string.h"

/* ---- Pure builder byte-layout ---- */

static void test_seal_build_create_primary(void)
{
    uint8_t buf[128];
    uint32_t n;

    n = tpm2_build_create_primary_srk(buf, sizeof buf);
    TEST_ASSERT_EQ(n, 67u, "CreatePrimary SRK template is 67 bytes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 0), TPM2_ST_SESSIONS, "CreatePrimary tag ST_SESSIONS");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 2), 67u, "CreatePrimary header size == length");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_CREATE_PRIMARY, "CreatePrimary CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM_RH_OWNER, "primaryHandle OWNER");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), 9u, "authorizationSize 9 (PW session)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 18), TPM_RS_PW, "owner password session");
    /* sessionAttributes (offset 24, after authSize+handle+nonce-size) MUST be 0
     * for TPM_RS_PW -- continueSession on a password auth is non-portable. */
    TEST_ASSERT_EQ((uint32_t)buf[24], 0u, "TPM_RS_PW attrs byte is 0 (no continueSession)");
    /* inPublic inner at 10+4+13+6 = 33. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 33), 26u, "TPMT_PUBLIC inner size 26");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 35), TPM_ALG_ECC, "SRK type ECC");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 37), TPM_ALG_SHA256, "SRK nameAlg SHA256");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 39), (uint32_t)TPM_SRK_OBJECT_ATTRS, "SRK objectAttributes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 45), TPM_ALG_AES, "SRK symmetric AES");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 47), 128u, "SRK AES keyBits 128");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 49), TPM_ALG_CFB, "SRK symmetric mode CFB");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 53), TPM_ECC_NIST_P256, "SRK curve NIST P256");

    TEST_ASSERT_EQ(tpm2_build_create_primary_srk(buf, 66u), 0u,
                   "CreatePrimary refuses too-small buffer");
}

static void test_seal_build_create_sealed(void)
{
    uint8_t buf[256];
    uint8_t policy[32], secret[16];
    uint32_t n, i;
    for (i = 0; i < 32; i++) policy[i] = (uint8_t)(0x40u + i);
    for (i = 0; i < 16; i++) secret[i] = (uint8_t)(0x90u + i);

    /* insens = 2 + (2+16) = 20; pub_inner = 8 + (2+32) + 2 + 2 = 46;
     * total = 10+4+13 + (2+20) + (2+46) + 2 + 4 = 103. */
    n = tpm2_build_create_sealed(buf, sizeof buf, 0x80000001u, policy, 32u, secret, 16u);
    TEST_ASSERT_EQ(n, 103u, "Create sealed (32B policy, 16B secret) is 103 bytes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 0), TPM2_ST_SESSIONS, "Create tag ST_SESSIONS");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_CREATE, "Create CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), 0x80000001u, "Create parentHandle");
    /* inSensitive at 10+4+13 = 27. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 27), 20u, "TPM2B_SENSITIVE_CREATE size 20");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 29), 0u, "userAuth empty");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 31), 16u, "sealed data size 16");
    TEST_ASSERT_EQ(buf[33], 0x90u, "sealed data first byte");
    /* inPublic at 27 + 2 + 20 = 49. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 49), 46u, "TPMT_PUBLIC inner size 46");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 51), TPM_ALG_KEYEDHASH, "sealed type KEYEDHASH");
    {
        uint32_t attrs = tpm2_be32_get(buf + 55);
        TEST_ASSERT_EQ(attrs, (uint32_t)TPM_SEAL_OBJECT_ATTRS, "sealed attrs == macro");
        TEST_ASSERT((attrs & TPMA_OBJ_FIXED_TPM) != 0u, "sealed attrs include fixedTPM");
        TEST_ASSERT((attrs & TPMA_OBJ_FIXED_PARENT) != 0u, "sealed attrs include fixedParent");
        TEST_ASSERT((attrs & TPMA_OBJ_NO_DA) != 0u,
                    "sealed attrs include noDA (PCR drift must not DA-lock recovery)");
        TEST_ASSERT((attrs & TPMA_OBJ_USER_WITH_AUTH) == 0u,
                    "sealed attrs exclude userWithAuth (Unseal must satisfy policy)");
        TEST_ASSERT((attrs & TPMA_OBJ_SENSITIVE_DATA_ORIGIN) == 0u,
                    "sealed attrs exclude sensitiveDataOrigin (data is caller-supplied)");
    }
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 59), 32u, "authPolicy size 32");
    TEST_ASSERT_EQ(buf[61], 0x40u, "authPolicy first byte");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 93), TPM_ALG_NULL, "keyedHash scheme NULL");

    /* Over-cap secret + zero-length refused. */
    TEST_ASSERT_EQ(tpm2_build_create_sealed(buf, sizeof buf, 0x80000001u, policy, 32u,
                                            secret, (uint16_t)(TPM_SEAL_SECRET_MAX + 1u)),
                   0u, "Create refuses over-cap secret");
    TEST_ASSERT_EQ(tpm2_build_create_sealed(buf, sizeof buf, 0x80000001u, policy, 32u,
                                            secret, 0u),
                   0u, "Create refuses zero-length secret");
}

static void test_seal_build_load_unseal(void)
{
    uint8_t buf[512];
    struct tpm_sealed_blob blob;
    uint32_t n, i;
    memset(&blob, 0, sizeof blob);
    blob.priv_len = 16; blob.pub_len = 20;
    for (i = 0; i < 16; i++) blob.priv[i] = (uint8_t)(0x10u + i);
    for (i = 0; i < 20; i++) blob.pub[i] = (uint8_t)(0x30u + i);

    /* Load total = 10+4+13 + (2+16) + (2+20) = 67. */
    n = tpm2_build_load(buf, sizeof buf, 0x80000002u, &blob);
    TEST_ASSERT_EQ(n, 67u, "Load (16B priv, 20B pub) is 67 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_LOAD, "Load CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), 0x80000002u, "Load parentHandle");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 27), 16u, "inPrivate size 16");
    TEST_ASSERT_EQ(buf[29], 0x10u, "inPrivate first byte");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 45), 20u, "inPublic size 20");
    TEST_ASSERT_EQ(buf[47], 0x30u, "inPublic first byte");

    /* Zero-length blob refused. */
    blob.priv_len = 0;
    TEST_ASSERT_EQ(tpm2_build_load(buf, sizeof buf, 0x80000002u, &blob), 0u,
                   "Load refuses empty private blob");

    /* Unseal total = 10+4+13 = 27. */
    n = tpm2_build_unseal(buf, sizeof buf, 0x80000003u, 0x03000000u);
    TEST_ASSERT_EQ(n, 27u, "Unseal is 27 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_UNSEAL, "Unseal CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), 0x80000003u, "Unseal itemHandle");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), 9u, "Unseal authorizationSize 9");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 18), 0x03000000u, "Unseal policy session handle");
    /* A real POLICY session (high byte 0x03) MUST carry continueSession=0x01 so
     * the session survives for our explicit FlushContext. */
    TEST_ASSERT_EQ((uint32_t)buf[24], 1u, "policy session attrs byte is 1 (continueSession)");
}

/* ---- Pure parse coverage ---- */

static void test_seal_parse(void)
{
    uint8_t rsp[128];
    struct tpm_sealed_blob blob;
    uint8_t out[64];
    int dl;
    uint32_t h, i;

    /* parse_object_handle: header(10) + objectHandle(4) + parameterSize(4) +
     * params(0) + one-session auth area(5) -> size 23. */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(rsp + 2, 23u);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 0x80000001u);     /* transient object handle */
    tpm2_be32_put(rsp + 14, 0u);              /* parameterSize 0 */
    /* auth area at 18: nonceTPM(2,0) + attrs(1) + hmac(2,0) -- already zeroed. */
    h = tpm2_parse_object_handle(rsp, 23u);
    TEST_ASSERT_EQ(h, 0x80000001u, "object handle parsed (full ST_SESSIONS envelope)");
    /* Non-object handle (a session 0x03..) rejected -- never flush-able as object. */
    tpm2_be32_put(rsp + 10, 0x03000000u);
    TEST_ASSERT_EQ(tpm2_parse_object_handle(rsp, 23u), 0u, "non-object handle rejected");
    /* Error rc -> 0. */
    tpm2_be32_put(rsp + 10, 0x80000001u);
    tpm2_be32_put(rsp + 6, 0x0000018Bu);
    TEST_ASSERT_EQ(tpm2_parse_object_handle(rsp, 23u), 0u, "error-rc handle rejected");
    /* ST_NO_SESSIONS success (no auth area) for a session-authorized command -> reject. */
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    TEST_ASSERT_EQ(tpm2_parse_object_handle(rsp, 23u), 0u,
                   "ST_NO_SESSIONS handle response rejected (no auth area)");
    /* Truncated: ST_SESSIONS but no room for the auth area -> reject. */
    tpm2_be16_put(rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(rsp + 2, 18u);
    TEST_ASSERT_EQ(tpm2_parse_object_handle(rsp, 18u), 0u,
                   "handle response with no auth area rejected");
    /* parameterSize claiming past the response -> reject (no overread). */
    tpm2_be32_put(rsp + 2, 23u);
    tpm2_be32_put(rsp + 14, 99u);
    TEST_ASSERT_EQ(tpm2_parse_object_handle(rsp, 23u), 0u,
                   "oversize parameterSize in handle response rejected");

    /* parse_create_sealed: params = outPrivate(2+8) + outPublic(2+12) +
     * creationData(2,0) + creationHash(2,0) + creationTicket(8) -> 36. */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 36u);             /* parameterSize */
    tpm2_be16_put(rsp + 14, 8u);              /* outPrivate size */
    for (i = 0; i < 8; i++) rsp[16 + i] = (uint8_t)(0xA0u + i);
    tpm2_be16_put(rsp + 24, 12u);             /* outPublic size */
    for (i = 0; i < 12; i++) rsp[26 + i] = (uint8_t)(0xB0u + i);
    /* creationData(38)=0, creationHash(40)=0, creationTicket(42..49) zero. */
    tpm2_be32_put(rsp + 2, 55u);              /* 10+4+36 + 5-byte auth area */
    memset(&blob, 0, sizeof blob);
    TEST_ASSERT_EQ(tpm2_parse_create_sealed(rsp, 55u, &blob), 0, "Create response parsed");
    TEST_ASSERT_EQ((uint32_t)blob.priv_len, 8u, "outPrivate length 8");
    TEST_ASSERT_EQ((uint32_t)blob.pub_len, 12u, "outPublic length 12");
    TEST_ASSERT(blob.priv[0] == 0xA0u && blob.pub[0] == 0xB0u, "blob bytes copied");
    /* An outPrivate claiming more than the param area must be rejected. */
    tpm2_be16_put(rsp + 14, 200u);
    TEST_ASSERT_EQ(tpm2_parse_create_sealed(rsp, 55u, &blob), -1,
                   "oversize outPrivate rejected");
    tpm2_be16_put(rsp + 14, 8u);              /* restore */
    /* A truncated Create whose parameterSize ends right after outPublic (missing
     * the required creation fields) must be rejected -- exact consumption. */
    tpm2_be32_put(rsp + 10, 24u);             /* parameterSize = only outPriv+outPub */
    tpm2_be32_put(rsp + 2, 43u);              /* 10+4+24 + 5 auth */
    TEST_ASSERT_EQ(tpm2_parse_create_sealed(rsp, 43u, &blob), -1,
                   "Create missing creation fields rejected (exact consumption)");

    /* parse_unseal: outData TPM2B (exact consumption). */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 12u);             /* parameterSize = 2 + 10 */
    tpm2_be16_put(rsp + 14, 10u);             /* outData size */
    for (i = 0; i < 10; i++) rsp[16 + i] = (uint8_t)(0xC0u + i);
    tpm2_be32_put(rsp + 2, 31u);              /* 10+4+12 + 5-byte auth area */
    memset(out, 0, sizeof out);
    dl = tpm2_parse_unseal(rsp, 31u, out, sizeof out);
    TEST_ASSERT_EQ(dl, 10, "Unseal yields 10 data bytes");
    TEST_ASSERT(out[0] == 0xC0u && out[9] == 0xC9u, "Unseal data correct");
    /* Non-exact outData (claims 99 in a 10-byte param area) rejected. */
    tpm2_be16_put(rsp + 14, 99u);
    TEST_ASSERT_EQ(tpm2_parse_unseal(rsp, 31u, out, sizeof out), -1,
                   "non-exact outData rejected");
    /* Zero-length outData rejected -- an empty unseal is never a valid unlock. */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 2u);              /* parameterSize = 2 (size only) */
    tpm2_be16_put(rsp + 14, 0u);              /* outData size 0 */
    tpm2_be32_put(rsp + 2, 21u);              /* 10+4+2 + 5-byte auth area */
    TEST_ASSERT_EQ(tpm2_parse_unseal(rsp, 21u, out, sizeof out), -1,
                   "zero-length outData rejected");
}

/* ---- Seal mask excludes PCR 11 (FDE must survive a kernel rebuild) ---- */

static void test_seal_mask_excludes_pcr11(void)
{
    uint32_t mask = tpm_pcr_seal_mask();
    TEST_ASSERT((mask & (1u << 7)) != 0u, "seal mask includes PCR 7 (Secure Boot policy)");
    TEST_ASSERT((mask & (1u << 11)) == 0u,
                "seal mask EXCLUDES PCR 11 (kernel-ABI manifest must not brick FDE)");
    TEST_ASSERT(mask != 0u, "seal mask is non-empty");
}

/* ---- CC-dispatch fake-TIS transport (seal/unseal lifecycle) ---- */

#define SF_CAP 256u
static uint8_t  sf_cmd[SF_CAP];
static uint32_t sf_cmd_len;
static uint32_t sf_cmd_expect;
static uint8_t  sf_rsp[SF_CAP];
static uint32_t sf_rsp_len;
static uint32_t sf_rsp_pos;
static int      sf_ready;
static int      sf_executed;
static uint32_t sf_seen[24];
static uint32_t sf_seen_n;
static uint32_t sf_flush_n;        /* count of FlushContext commands */
static uint32_t sf_flushed[16];    /* flushHandle of each FlushContext */
static uint32_t sf_flushed_n;
static uint32_t sf_fail_cc;
static uint32_t sf_fail_rc;
static uint8_t  sf_secret[32];     /* staged unseal payload */
static uint16_t sf_secret_len;
static int      sf_unseal_noauth;    /* emit Unseal success with NO auth area (F1) */
static int      sf_unseal_nosession; /* emit Unseal success as ST_NO_SESSIONS (F-A1) */
static int      sf_handle_malformed; /* emit CreatePrimary/Load success: handle present, bad envelope (F-A2) */
static int      sf_fail_times;     /* how many times sf_fail_cc still fails; <0 = forever.
                                    * A TRANSIENT failure that eventually clears is what
                                    * separates "retried and succeeded" from "gave up". */
static uint32_t sf_stall_reads;    /* withhold COMMAND_READY for N status reads: a
                                    * slow-but-responsive TPM, which is exactly what a
                                    * cumulative budget exists to bound (and what a
                                    * per-command timeout does NOT). */
static uint32_t sf_stall_after_go;  /* withhold DATA_AVAIL for N status reads
                                    * AFTER the command was dispatched (mirrors
                                    * test_tpm_nv.c's nvf_stall_after_go): the
                                    * abandoned-mid-flight case, distinct from
                                    * sf_stall_reads' before-dispatch stall. */

#define SF_REG_STS   0x018u
#define SF_REG_FIFO  0x024u
#define SF_STS_EXPECT        0x08u
#define SF_STS_DATA_AVAIL    0x10u
#define SF_STS_GO            0x20u
#define SF_STS_COMMAND_READY 0x40u
#define SF_STS_VALID         0x80u

/* Append a minimal 5-byte response auth area (nonceTPM(2,0) + attrs(1) +
 * hmac(2,0)) at off; returns the new length. */
static uint32_t sf_put_auth(uint32_t off)
{
    tpm2_be16_put(sf_rsp + off, 0u); off += 2u;   /* nonceTPM */
    sf_rsp[off] = 0u; off += 1u;                  /* attributes */
    tpm2_be16_put(sf_rsp + off, 0u); off += 2u;   /* hmac */
    return off;
}

static void sf_build_response(void)
{
    uint32_t cc = tpm2_be32_get(sf_cmd + 6);
    uint32_t rc = TPM2_RC_SUCCESS;
    uint32_t off;
    uint16_t i;
    if (sf_seen_n < 24u) sf_seen[sf_seen_n++] = cc;
    if (cc == TPM2_CC_FLUSH_CONTEXT) {
        sf_flush_n++;
        if (sf_flushed_n < 16u) sf_flushed[sf_flushed_n++] = tpm2_be32_get(sf_cmd + 10);
    }
    if (sf_fail_cc != 0u && cc == sf_fail_cc && sf_fail_times != 0) {
        rc = sf_fail_rc;
        if (sf_fail_times > 0)
            sf_fail_times--;       /* transient: clears after this many attempts */
    }
    memset(sf_rsp, 0, SF_CAP);

    if (rc != TPM2_RC_SUCCESS) {
        /* TPM error responses are always ST_NO_SESSIONS bare headers. */
        tpm2_be16_put(sf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(sf_rsp + 2, 10u);
        tpm2_be32_put(sf_rsp + 6, rc);
        sf_rsp_len = 10u;
        return;
    }

    if (cc == TPM2_CC_START_AUTH_SESSION) {
        tpm2_be16_put(sf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(sf_rsp + 2, 32u);
        tpm2_be32_put(sf_rsp + 6, rc);
        tpm2_be32_put(sf_rsp + 10, 0x03000000u);  /* policy session handle */
        tpm2_be16_put(sf_rsp + 14, 16u);          /* nonceTPM (16 zero) */
        sf_rsp_len = 32u;
    } else if (cc == TPM2_CC_POLICY_GET_DIGEST) {
        tpm2_be16_put(sf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(sf_rsp + 2, 44u);
        tpm2_be32_put(sf_rsp + 6, rc);
        tpm2_be16_put(sf_rsp + 10, 32u);          /* 32-byte policyDigest */
        sf_rsp_len = 44u;
    } else if (cc == TPM2_CC_CREATE_PRIMARY || cc == TPM2_CC_LOAD) {
        uint32_t handle = (cc == TPM2_CC_CREATE_PRIMARY) ? 0x80000001u : 0x80000002u;
        if (sf_handle_malformed) {
            /* rc=SUCCESS with the transient handle present at offset 10 but a
             * malformed envelope (wrong tag) -> tpm2_parse_object_handle rejects,
             * and seal_exec_handle must best-effort flush the leaked handle. */
            tpm2_be16_put(sf_rsp + 0, TPM2_ST_NO_SESSIONS);
            tpm2_be32_put(sf_rsp + 6, rc);
            tpm2_be32_put(sf_rsp + 10, handle);
            tpm2_be32_put(sf_rsp + 2, 14u);
            sf_rsp_len = 14u;
        } else {
            /* Leading object-handle(4) before parameterSize(4), then params
             * (empty) + a one-session response auth area (5 bytes). */
            tpm2_be16_put(sf_rsp + 0, TPM2_ST_SESSIONS);
            tpm2_be32_put(sf_rsp + 6, rc);
            tpm2_be32_put(sf_rsp + 10, handle);
            tpm2_be32_put(sf_rsp + 14, 0u);       /* parameterSize 0 */
            off = sf_put_auth(18u);               /* auth area at 18 -> 23 */
            tpm2_be32_put(sf_rsp + 2, off);
            sf_rsp_len = off;
        }
    } else if (cc == TPM2_CC_CREATE) {
        /* params: outPrivate(2+16) + outPublic(2+20) + creationData(2,0)
         * + creationHash(2,0) + creationTicket(tag(2)+hierarchy(4)+digest(2,0))
         * -> parameterSize 52. The trailing creation fields are zeroed (memset). */
        tpm2_be16_put(sf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(sf_rsp + 6, rc);
        tpm2_be32_put(sf_rsp + 10, 52u);
        tpm2_be16_put(sf_rsp + 14, 16u);          /* outPrivate */
        for (i = 0; i < 16; i++) sf_rsp[16 + i] = (uint8_t)(0x70u + i);
        tpm2_be16_put(sf_rsp + 32, 20u);          /* outPublic */
        for (i = 0; i < 20; i++) sf_rsp[34 + i] = (uint8_t)(0x80u + i);
        /* creationData(54)=0, creationHash(56)=0, creationTicket(58..65) zero. */
        off = sf_put_auth(66u);
        tpm2_be32_put(sf_rsp + 2, off);
        sf_rsp_len = off;
    } else if (cc == TPM2_CC_UNSEAL && sf_unseal_nosession) {
        /* Forged ST_NO_SESSIONS rc-success with a valid-looking outData TPM2B at
         * offset 10: a session-authorized command must NOT accept this. */
        tpm2_be16_put(sf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(sf_rsp + 6, rc);
        tpm2_be16_put(sf_rsp + 10, sf_secret_len);
        for (i = 0; i < sf_secret_len; i++) sf_rsp[12 + i] = sf_secret[i];
        tpm2_be32_put(sf_rsp + 2, 12u + (uint32_t)sf_secret_len);
        sf_rsp_len = 12u + (uint32_t)sf_secret_len;
    } else if (cc == TPM2_CC_UNSEAL) {
        /* params: outData(2 + sf_secret_len). */
        tpm2_be16_put(sf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(sf_rsp + 6, rc);
        tpm2_be16_put(sf_rsp + 14, sf_secret_len);
        for (i = 0; i < sf_secret_len; i++) sf_rsp[16 + i] = sf_secret[i];
        if (sf_unseal_noauth) {
            /* parameterSize consumes the whole declared response: NO auth area.
             * tpm_session_cmd_exec must reject this rather than return outData. */
            tpm2_be32_put(sf_rsp + 10, 2u + (uint32_t)sf_secret_len);
            tpm2_be32_put(sf_rsp + 2, 14u + 2u + (uint32_t)sf_secret_len);
            sf_rsp_len = 14u + 2u + (uint32_t)sf_secret_len;
        } else {
            tpm2_be32_put(sf_rsp + 10, 2u + (uint32_t)sf_secret_len);
            off = sf_put_auth(16u + sf_secret_len);
            tpm2_be32_put(sf_rsp + 2, off);
            sf_rsp_len = off;
        }
    } else {
        /* PolicyPCR / FlushContext: bare ST_NO_SESSIONS success. */
        tpm2_be16_put(sf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(sf_rsp + 2, 10u);
        tpm2_be32_put(sf_rsp + 6, rc);
        sf_rsp_len = 10u;
    }
}

static uint8_t sf_r8(uint32_t off)
{
    if (off == SF_REG_FIFO && sf_executed && sf_rsp_pos < sf_rsp_len)
        return sf_rsp[sf_rsp_pos++];
    return 0;
}

static void sf_w8(uint32_t off, uint8_t v)
{
    if (off != SF_REG_FIFO || sf_executed)
        return;
    if (sf_cmd_len < SF_CAP)
        sf_cmd[sf_cmd_len] = v;
    sf_cmd_len++;
    if (sf_cmd_len == 6u)
        sf_cmd_expect = tpm2_be32_get(sf_cmd + 2);
}

static uint32_t sf_r32(uint32_t off)
{
    uint8_t sts;
    if (off != SF_REG_STS)
        return 0;
    sts = SF_STS_VALID;
    /* A slow-but-RESPONSIVE TPM: withhold commandReady for N status polls, then
     * behave normally. This is the shape a cumulative budget exists to bound --
     * every individual command still completes well inside its own PTP timeout,
     * so a per-command bound never fires, while the SUM across a multi-command
     * flow runs away. Modelling it as an outright timeout instead would prove
     * nothing about the sequence conversion. */
    if (sf_stall_reads > 0u) {
        sf_stall_reads--;
        return (uint32_t)sts | (32u << 8);
    }
    if (sf_executed && sf_stall_after_go > 0u) {
        sf_stall_after_go--;
        return (uint32_t)sts | (32u << 8);
    }
    if (sf_ready && !sf_executed && sf_cmd_len == 0)
        sts |= SF_STS_COMMAND_READY;
    if (!sf_executed && sf_cmd_len > 0 && sf_cmd_len < sf_cmd_expect)
        sts |= SF_STS_EXPECT;
    if (sf_executed && sf_rsp_pos < sf_rsp_len)
        sts |= SF_STS_DATA_AVAIL;
    return (uint32_t)sts | (32u << 8);   /* burstCount = 32 */
}

static void sf_w32(uint32_t off, uint32_t v)
{
    if (off != SF_REG_STS)
        return;
    if (v & SF_STS_COMMAND_READY) {
        sf_ready = 1;
        sf_executed = 0;
        sf_cmd_len = 0;
        sf_cmd_expect = 0;
        sf_rsp_pos = 0;
    }
    if ((v & SF_STS_GO) && sf_cmd_len >= sf_cmd_expect) {
        sf_executed = 1;
        sf_build_response();
    }
}

static const struct tpm_t_io sf_io = { sf_r8, sf_w8, sf_r32, sf_w32 };

static void sf_reset(uint32_t fail_cc, uint32_t fail_rc)
{
    uint16_t i;
    sf_cmd_len = 0; sf_cmd_expect = 0;
    sf_rsp_len = 0; sf_rsp_pos = 0;
    sf_ready = 0; sf_executed = 0;
    sf_seen_n = 0; sf_flush_n = 0; sf_flushed_n = 0;
    sf_fail_cc = fail_cc; sf_fail_rc = fail_rc;
    sf_fail_times = -1;            /* default: fails forever, the prior behaviour */
    sf_stall_reads = 0;
    sf_stall_after_go = 0;
    sf_secret_len = 16;
    sf_unseal_noauth = 0;
    sf_unseal_nosession = 0;
    sf_handle_malformed = 0;
    for (i = 0; i < 16; i++) sf_secret[i] = (uint8_t)(0xE0u + i);
}

static int sf_saw_cc(uint32_t cc)
{
    uint32_t i;
    for (i = 0; i < sf_seen_n; i++)
        if (sf_seen[i] == cc)
            return 1;
    return 0;
}

static int sf_was_flushed(uint32_t handle)
{
    uint32_t i;
    for (i = 0; i < sf_flushed_n; i++)
        if (sf_flushed[i] == handle)
            return 1;
    return 0;
}

/* Recovery handler capture. */
static struct tpm_unseal_result g_last_recovery;
static int g_recovery_calls;
static void test_recovery_handler(const struct tpm_unseal_result *r)
{
    g_recovery_calls++;
    g_last_recovery = *r;
}

static void test_seal_lifecycle(void)
{
    struct tpm_t_test_state prev;
    struct tpm_sealed_blob blob;
    struct tpm_unseal_result res;
    uint8_t out[32];
    uint16_t got;
    tpm_seal_status_t st;
    uint32_t i;

    /* ---- Seal round-trip: trial policy -> CreatePrimary -> Create -> blob. ---- */
    sf_reset(0u, 0u);
    memset(&blob, 0, sizeof blob);
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    {
        uint8_t secret[16];
        for (i = 0; i < 16; i++) secret[i] = (uint8_t)(0x11u + i);
        st = tpm_seal_secret(secret, 16u, &blob);
    }
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_OK, "seal_secret succeeds");
    TEST_ASSERT(sf_saw_cc(TPM2_CC_START_AUTH_SESSION), "seal computed trial policy");
    TEST_ASSERT(sf_saw_cc(TPM2_CC_CREATE_PRIMARY), "seal created SRK parent");
    TEST_ASSERT(sf_saw_cc(TPM2_CC_CREATE), "seal created sealed object");
    TEST_ASSERT(sf_flush_n >= 2u, "seal flushed trial session + primary (no leak)");
    TEST_ASSERT_EQ((uint32_t)blob.priv_len, 16u, "blob carries outPrivate");
    TEST_ASSERT_EQ((uint32_t)blob.pub_len, 20u, "blob carries outPublic");

    /* ---- Unseal success: CreatePrimary -> Load -> policy session -> Unseal. ---- */
    sf_reset(0u, 0u);
    /* Reuse the blob shape produced above (priv 16 / pub 20). */
    blob.priv_len = 16; blob.pub_len = 20;
    for (i = 0; i < 16; i++) blob.priv[i] = (uint8_t)(0x70u + i);
    for (i = 0; i < 20; i++) blob.pub[i] = (uint8_t)(0x80u + i);
    g_recovery_calls = 0;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    memset(out, 0, sizeof out);
    got = 0;
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_GENERIC, out, sizeof out, &got, &res);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_OK, "unseal_secret succeeds while PCRs match");
    TEST_ASSERT_EQ((uint32_t)got, 16u, "unseal returns staged 16-byte secret");
    TEST_ASSERT(out[0] == 0xE0u && out[15] == 0xEFu, "unsealed data correct");
    TEST_ASSERT(sf_saw_cc(TPM2_CC_LOAD), "unseal loaded the object");
    TEST_ASSERT(sf_saw_cc(TPM2_CC_UNSEAL), "unseal issued TPM2_Unseal");
    TEST_ASSERT(sf_flush_n >= 3u, "unseal flushed primary + session + object");
    TEST_ASSERT_EQ(g_recovery_calls, 0, "no recovery handler on success");
    TEST_ASSERT_EQ((int)res.status, (int)TPM_SEAL_OK, "result status OK");

    /* ---- Unseal POLICY_FAIL: PCR drift -> TPM_RC_POLICY_FAIL on Unseal. ---- */
    sf_reset(TPM2_CC_UNSEAL, 0x0000099Du /* POLICY_FAIL on session 1 */);
    blob.priv_len = 16; blob.pub_len = 20;
    g_recovery_calls = 0;
    tpm_seal_set_recovery_handler(test_recovery_handler);
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    memset(out, 0xCC, sizeof out);
    got = 0xFFFFu;
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_FDE, out, sizeof out, &got, &res);
    tpm_t_test_restore(prev);
    tpm_seal_set_recovery_handler(0);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_POLICY_FAIL, "PCR drift -> POLICY_FAIL");
    TEST_ASSERT_EQ((uint32_t)got, 0u, "failed unseal returns no data");
    TEST_ASSERT(sf_saw_cc(TPM2_CC_UNSEAL), "unseal was attempted");
    TEST_ASSERT(sf_flush_n >= 3u, "object + session flushed even on policy failure");
    TEST_ASSERT_EQ(g_recovery_calls, 1, "recovery handler fired once on failure");
    TEST_ASSERT_EQ((int)g_last_recovery.status, (int)TPM_SEAL_POLICY_FAIL,
                   "recovery report carries POLICY_FAIL");
    TEST_ASSERT_EQ((int)g_last_recovery.domain, (int)TPM_SEAL_DOMAIN_FDE,
                   "recovery report carries the FDE domain");
    TEST_ASSERT_EQ(g_last_recovery.tpm_rc, 0x0000099Du, "recovery report carries raw rc");

    /* ---- F1 regression: an Unseal ST_SESSIONS "success" with NO auth area must
     * NOT return its outData as OK -- the session-response validation rejects it. ---- */
    sf_reset(0u, 0u);
    sf_unseal_noauth = 1;
    blob.priv_len = 16; blob.pub_len = 20;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    memset(out, 0xCC, sizeof out);
    got = 0xFFFFu;
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_GENERIC, out, sizeof out, &got, &res);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_TRANSPORT,
                   "Unseal success with no auth area -> TRANSPORT (never leaks outData)");
    TEST_ASSERT_EQ((uint32_t)got, 0u, "rejected unseal returns no data");
}

/* ---- No-TPM degrades cleanly (never wedges) ---- */

static void test_seal_no_tpm(void)
{
    struct tpm_t_test_state prev;
    struct tpm_sealed_blob blob;
    struct tpm_unseal_result res;
    uint8_t out[32];
    uint16_t got = 0;
    uint8_t secret[16];
    tpm_seal_status_t st;
    uint32_t i;
    for (i = 0; i < 16; i++) secret[i] = (uint8_t)i;

    /* Force the transport UNAVAILABLE (io == NULL restores pre-init state) so the
     * no-TPM path is deterministic -- otherwise a boot-initialized transport
     * would be probed and time out (TRANSPORT) instead of short-circuiting. */
    prev = tpm_t_test_install(0, TPM_T_IFACE_NONE, 0);

    memset(&blob, 0, sizeof blob);
    st = tpm_seal_secret(secret, 16u, &blob);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_NO_TPM, "seal with no TPM -> NO_TPM");

    blob.priv_len = 16; blob.pub_len = 20;
    g_recovery_calls = 0;
    tpm_seal_set_recovery_handler(test_recovery_handler);
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_CI, out, sizeof out, &got, &res);
    tpm_seal_set_recovery_handler(0);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_NO_TPM, "unseal with no TPM -> NO_TPM");
    TEST_ASSERT_EQ(g_recovery_calls, 1, "recovery handler fired on NO_TPM");
    TEST_ASSERT_EQ((int)g_last_recovery.domain, (int)TPM_SEAL_DOMAIN_CI,
                   "NO_TPM report carries the CI domain");

    /* BADARG: a null/empty secret issues no transaction (checked before transport). */
    TEST_ASSERT_EQ((int)tpm_seal_secret(0, 0u, &blob), (int)TPM_SEAL_BADARG,
                   "seal of null secret -> BADARG");
    TEST_ASSERT_EQ((int)tpm_unseal_fde_key(0, out, sizeof out, &got, 0),
                   (int)TPM_SEAL_BADARG, "fde unseal of null blob -> BADARG");

    tpm_t_test_restore(prev);
}

/* ---- Forged/degraded session responses (F-A1 + F-A2 hardening) ---- */

static void test_seal_forged_responses(void)
{
    struct tpm_t_test_state prev;
    struct tpm_sealed_blob blob;
    struct tpm_unseal_result res;
    uint8_t out[32];
    uint16_t got;
    tpm_seal_status_t st;

    memset(&blob, 0, sizeof blob);
    blob.priv_len = 16; blob.pub_len = 20;

    /* F-A1: an Unseal rc-success forged as ST_NO_SESSIONS (no auth area) with a
     * valid-looking outData must be rejected -- a session-authorized command may
     * only accept a session-tagged success. Otherwise attacker-controlled bytes
     * would be returned as the unsealed secret. */
    sf_reset(0u, 0u);
    sf_unseal_nosession = 1;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    memset(out, 0xCC, sizeof out);
    got = 0xFFFFu;
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_GENERIC, out, sizeof out, &got, &res);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_TRANSPORT,
                   "Unseal ST_NO_SESSIONS forged success -> TRANSPORT (never returns secret)");
    TEST_ASSERT_EQ((uint32_t)got, 0u, "forged unseal returns no data");

    /* F-A2: a malformed CreatePrimary success that still names a transient handle
     * must have that handle best-effort flushed so the object pool is not leaked. */
    sf_reset(0u, 0u);
    sf_handle_malformed = 1;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    {
        uint8_t secret[16];
        uint32_t i;
        for (i = 0; i < 16; i++) secret[i] = (uint8_t)i;
        st = tpm_seal_secret(secret, 16u, &blob);
    }
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_TRANSPORT, "malformed CreatePrimary success -> TRANSPORT");
    TEST_ASSERT(sf_was_flushed(0x80000001u),
                "leaked transient handle from malformed CreatePrimary is flushed (no pool leak)");
    TEST_ASSERT(!sf_saw_cc(TPM2_CC_CREATE), "no Create attempted after a bad parent handle");

    /* A well-formed ST_SESSIONS Unseal success carrying a ZERO-length payload
     * must NOT be reported as a successful unlock (seal never seals empty data). */
    sf_reset(0u, 0u);
    sf_secret_len = 0;
    blob.priv_len = 16; blob.pub_len = 20;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    memset(out, 0xCC, sizeof out);
    got = 0xFFFFu;
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_GENERIC, out, sizeof out, &got, &res);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_TRANSPORT,
                   "zero-length Unseal payload -> TRANSPORT (not a successful unlock)");
    TEST_ASSERT_EQ((uint32_t)got, 0u, "empty unseal returns no data");
}


/* ---- Section 24: bounded sequence + verified teardown on the seal path ----
 *
 * The three properties section 17 proved for the NV path, asserted here for the
 * two flows this section converted. Each is written so that reverting the
 * conversion fails it: a CONTROL run proves the fixture itself is not simply
 * refusing everything, and the teardown assertions count FlushContext COMMANDS
 * rather than reading the operation's return value, because the defect being
 * guarded against is precisely one that leaves the return value looking fine.
 */

/* ONE budget across the WHOLE operation, not one per command.
 *
 * Before the conversion, seal ran CreatePrimary, Create and two flushes as
 * separate transactions, each arming its own per-command PTP timeout. A TPM slow
 * enough to burn most of a timeout per command stayed inside every individual
 * bound while the operation as a whole ran unboundedly long. The fixture stalls
 * a fixed number of status polls per command, so the only thing that can
 * terminate the flow early is a CUMULATIVE bound. */
static void test_seal_one_budget_per_operation(void)
{
    struct tpm_t_test_state prev;
    struct tpm_sealed_blob blob;
    tpm_seal_status_t st;
    uint8_t secret[16];
    uint16_t i;

    for (i = 0; i < 16u; i++) secret[i] = (uint8_t)(0x40u + i);

    /* CONTROL FIRST. The same stalling TPM must SUCCEED under a generous
     * budget. Without this, a seal that refused everything for an unrelated
     * reason would satisfy the expiry assertion below and prove nothing. */
    sf_reset(0u, 0u);
    sf_stall_reads = 6u;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(60000u, 1000u);
    st = tpm_seal_secret(secret, (uint16_t)sizeof secret, &blob);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_OK,
                   "CONTROL: a slow-but-responsive TPM still seals inside a "
                   "generous budget");

    /* Budget already spent BEFORE the trial session's own StartAuthSession
     * ever touches the interface (sf_stall_reads only delays commandReady,
     * which the pre-dispatch check never reaches): nothing was submitted, so
     * this is exactly as safe to retry as ordinary gate contention. */
    sf_reset(0u, 0u);
    sf_stall_reads = 6u;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(0u, 1000u);
    st = tpm_seal_secret(secret, (uint16_t)sizeof secret, &blob);
    tpm_nv_test_reset_op_budget();
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_BUSY,
                   "a pre-dispatch budget refusal reports the retryable class: "
                   "nothing was ever submitted");
    /* Checked BEFORE restore: tpm_transport_available() reflects the
     * INSTALLED fake here, not whatever real (or absent) device the test
     * harness restores to afterward. */
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a budget expiry never sticky-fails the transport");
    tpm_t_test_restore(prev);

    /* The genuinely-in-flight (dispatched, then abandoned) case for a
     * handle-allocating command is proven at the transport layer instead of
     * here: test_seq_last_submit_dispatched (test_tpm_nv.c) is a deterministic,
     * timing-independent test of tpm2_seq_last_submit_dispatched() itself --
     * the primitive both TPMERR arms above are gated on -- because reaching
     * "dispatched, then the cumulative budget expires mid-wait" through this
     * fixture requires an unbounded stall count with no reliable bound (a
     * work_ms budget large enough to survive the pre-dispatch check makes the
     * exact expiry point a real-time race no fixed iteration count can pin,
     * and an unbounded one risks a hung test). The classification wiring
     * itself -- map_nv_status_handle special-cases exactly TPM_NV_BUDGET, and
     * the pre-dispatch case above proves the code path that must NOT take
     * that branch (seal_exec_handle reports BUSY, not BUDGET, when nothing
     * dispatched) -- is verified by code inspection plus the transport-layer
     * test proving the underlying signal is correct in both directions;
     * end-to-end reproduction of the in-flight case is what fixture timing
     * cannot pin reliably. */
}

/* The same bound covers UNSEAL, which is the FDE-unlock path and the one a user
 * actually feels. It is a separate assertion rather than a loop because unseal
 * runs a DIFFERENT command set (CreatePrimary, Load, StartAuthSession, PolicyPCR,
 * Unseal, and two flushes) through a nested policy-session helper, and that
 * helper is exactly where a missed _seq conversion would strand the flow. */
static void test_unseal_one_budget_per_operation(void)
{
    struct tpm_t_test_state prev;
    struct tpm_sealed_blob blob;
    struct tpm_unseal_result res;
    uint8_t out[32];
    uint16_t got = 0;
    tpm_seal_status_t st;

    /* A blob the fixture will accept; contents are irrelevant to the bound. */
    memset(&blob, 0, sizeof blob);
    blob.priv_len = 32u; blob.pub_len = 32u;

    sf_reset(0u, 0u);
    sf_stall_reads = 6u;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(60000u, 1000u);
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_FDE, out,
                           (uint16_t)sizeof out, &got, &res);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_OK,
                   "CONTROL: a slow-but-responsive TPM still unseals inside a "
                   "generous budget");

    /* Pre-dispatch: budget already spent before CreatePrimary (the first
     * command) ever touches the interface. Nothing was submitted, so this is
     * exactly as safe to retry as ordinary gate contention. */
    sf_reset(0u, 0u);
    sf_stall_reads = 6u;
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(0u, 1000u);
    st = tpm_unseal_secret(&blob, TPM_SEAL_DOMAIN_FDE, out,
                           (uint16_t)sizeof out, &got, &res);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_BUSY,
                   "a pre-dispatch budget refusal reports the retryable "
                   "class: nothing was ever submitted");
    TEST_ASSERT_EQ((int)res.status, (int)TPM_SEAL_BUSY,
                   "the structured recovery result carries the same verdict");

    /* The genuinely-in-flight case is proven at the transport layer
     * (test_seq_last_submit_dispatched, test_tpm_nv.c) rather than here -- see
     * the identical rationale in test_seal_one_budget_per_operation above. */
}

/* A TEARDOWN REQUIRES PROOF, and a transient warning is retried.
 *
 * The old seal_flush submitted FlushContext and discarded the result, so a
 * TPM_RC_RETRY left the object allocated while the code proceeded as though it
 * were released. Asserted by COUNTING FlushContext commands rather than by the
 * operation's return value, because the whole defect is that the return value
 * looked correct while a slot leaked. */
static void test_seal_teardown_requires_proof(void)
{
    struct tpm_t_test_state prev;
    struct tpm_sealed_blob blob;
    tpm_seal_status_t st;
    uint8_t secret[16];
    uint32_t flushes_clean, flushes_retried;
    uint16_t i;

    for (i = 0; i < 16u; i++) secret[i] = (uint8_t)(0x40u + i);

    /* BASELINE: a TPM that proves the flush first time. */
    sf_reset(0u, 0u);
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_seal_secret(secret, (uint16_t)sizeof secret, &blob);
    flushes_clean = sf_flush_n;
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_OK, "CONTROL: a clean seal succeeds");
    /* TWO, and naming both is the point of the control: seal releases the SRK
     * primary it created AND the trial policy session that computed the
     * authPolicy digest. Asserting the exact number pins the handle inventory,
     * so a future change that leaks one (or opens a third without releasing it)
     * fails here rather than silently eroding the TPM's object pool. */
    TEST_ASSERT_EQ((int)flushes_clean, 2,
                   "CONTROL: a clean seal proves exactly two teardowns -- the "
                   "trial policy session and the SRK primary");

    /* TRANSIENT WARNING: the first FlushContext answers TPM_RC_RETRY, which is
     * NOT proof -- the session is exactly where it was. The teardown must try
     * again, and the second attempt succeeds. More than one FlushContext is the
     * observable that separates "retried" from "accepted the first answer". */
    sf_reset(TPM2_CC_FLUSH_CONTEXT, TPM2_RC_RETRY);
    sf_fail_times = 1;                 /* transient: clears after one attempt */
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_seal_secret(secret, (uint16_t)sizeof secret, &blob);
    flushes_retried = sf_flush_n;
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_SEAL_OK,
                   "a transient teardown warning does not fail the seal itself");
    TEST_ASSERT(flushes_retried > flushes_clean,
                "a transient TPM_RC_RETRY is RETRIED, not accepted as proof");

    /* NEVER PROVEN: FlushContext answers RETRY forever. The teardown must give
     * up after a BOUNDED number of attempts -- it must neither loop without end
     * nor stop after one -- and it must REPORT rather than escalate: the seal
     * still succeeds and the transport stays usable, because poisoning it would
     * take out PCR reads that never opened a handle at all. */
    sf_reset(TPM2_CC_FLUSH_CONTEXT, TPM2_RC_RETRY);
    sf_fail_times = -1;                /* never clears */
    prev = tpm_t_test_install(&sf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_seal_secret(secret, (uint16_t)sizeof secret, &blob);
    TEST_ASSERT(sf_flush_n > flushes_clean,
                "an unproven teardown retries rather than accepting one answer");
    /* Bounded RELATIVE to the clean handle inventory rather than against a bare
     * constant: each handle gets its own bounded retry allowance, so the ceiling
     * is per-teardown, not per-operation. Hardcoding TPM_NV_FLUSH_RETRIES here
     * would assert that the whole operation shares one allowance, which is not
     * the contract and fails the moment a flow releases a second handle. */
    TEST_ASSERT(sf_flush_n <= flushes_clean * TPM_NV_FLUSH_RETRIES,
                "and the retry is BOUNDED per handle, never an unbounded loop");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "an unproven teardown is REPORTED, not escalated into a "
                   "transport-wide outage");
    tpm_t_test_restore(prev);
}

void test_register_tpm_seal(void)
{
    test_suite_register_cat("tpm: seal one budget per operation",
                            test_seal_one_budget_per_operation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: unseal one budget per operation",
                            test_unseal_one_budget_per_operation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal teardown requires proof",
                            test_seal_teardown_requires_proof, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal CreatePrimary marshal", test_seal_build_create_primary, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal Create marshal", test_seal_build_create_sealed, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal Load/Unseal marshal", test_seal_build_load_unseal, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal response parse", test_seal_parse, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal mask excludes PCR 11", test_seal_mask_excludes_pcr11, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal/unseal lifecycle", test_seal_lifecycle, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal forged-response rejection", test_seal_forged_responses, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: seal no-TPM degrade", test_seal_no_tpm, TEST_CAT_SECURITY);
}
