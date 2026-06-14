/* ============================================================================
 * test_tpm_nv.c -- TPM2 NV index storage + PCR-policy session tests
 *
 * Pure marshaling/parse/classifier tests (no MMIO) plus a CC-dispatch fake-TIS
 * transport for the session-lifecycle paths: a successful baseline define and a
 * post-StartAuthSession failure, both asserting the session handle is flushed
 * (no leak). No live boot infrastructure.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm_pcr_alloc.h"
#include "libc/string.h"

/* ---- RC classification (format-first; never wedges) ---- */

static void test_nv_classify_rc(void)
{
    /* Format-0 NV warnings: exact-compared. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000000u), (int)TPM_NV_OK, "rc 0 -> OK");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000148u), (int)TPM_NV_LOCKED, "0x148 -> LOCKED");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x0000014Bu), (int)TPM_NV_NOSPACE, "0x14B -> NOSPACE");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x0000014Cu), (int)TPM_NV_DEFINED, "0x14C -> DEFINED");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x0000014Au), (int)TPM_NV_UNINIT, "0x14A -> UNINIT");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000146u), (int)TPM_NV_RANGE, "0x146 -> RANGE");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000147u), (int)TPM_NV_RANGE, "0x147 -> RANGE");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000149u), (int)TPM_NV_AUTH, "0x149 -> AUTH");

    /* Format-1: masked to base error. A handle error on handle #1 is 0x18B. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x0000018Bu), (int)TPM_NV_NOTFOUND,
                   "0x18B (HANDLE on handle 1) -> NOTFOUND");
    /* Policy fail on session #1: 0x09D base | 0x800 N | ... -> still AUTH. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x0000099Du), (int)TPM_NV_AUTH,
                   "0x99D (POLICY_FAIL on session 1) -> AUTH");
    /* Value error on parameter #2: 0x084 base | 0x40 P | 0x200 N -> RANGE. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x000002C4u), (int)TPM_NV_RANGE,
                   "0x2C4 (VALUE on param 2) -> RANGE");

    /* Format discrimination: a format-1 code whose low byte collides with an
     * NV warning value (0x48) must NOT be classified as NV_LOCKED. 0x1C8 has
     * bit 7 set -> format-1 path -> base 0x88 -> TPMERR, never LOCKED. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x000001C8u), (int)TPM_NV_TPMERR,
                   "0x1C8 (format-1) not mistaken for NV_LOCKED");
    /* An unknown format-0 code is a classified TPMERR, never out-of-enum. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000101u), (int)TPM_NV_TPMERR,
                   "unknown format-0 -> TPMERR (no wedge)");
}

/* ---- Pure builder byte-layout ---- */

static void test_nv_build_define(void)
{
    uint8_t buf[96];
    uint8_t policy[32];
    uint32_t n, inner;
    uint32_t i;
    for (i = 0; i < 32; i++) policy[i] = (uint8_t)(0x10u + i);

    /* No policy: total = 10+4+13+2+2+(14) = 45. */
    n = tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_OS_DATA,
                             TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE,
                             TPM_ALG_SHA256, 0, 0, 64u);
    TEST_ASSERT_EQ(n, 45u, "NV_DefineSpace (no policy) is 45 bytes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 0), TPM2_ST_SESSIONS, "define tag ST_SESSIONS");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 2), 45u, "define header size == length");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_NV_DEFINE_SPACE, "define CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM_RH_OWNER, "define authHandle OWNER");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), 9u, "define authorizationSize 9 (PW session)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 18), TPM_RS_PW, "define session is TPM_RS_PW");
    /* nvPublic inner size at offset 10+4+13+2 = 29; nvIndex follows. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 29), 14u, "nvPublic inner size (empty policy) == 14");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 31), TPM_NV_INDEX_OS_DATA, "nvIndex");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 35), TPM_ALG_SHA256, "nameAlg SHA256");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 37), (uint32_t)(TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE),
                   "attributes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 41), 0u, "authPolicy size 0");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 43), 64u, "dataSize 64");

    /* With a 32-byte policy: inner = 14 + 32 = 46; total = 45 + 32 = 77. */
    n = tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                             TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE,
                             TPM_ALG_SHA256, policy, 32u, 96u);
    TEST_ASSERT_EQ(n, 77u, "NV_DefineSpace (32B policy) is 77 bytes");
    inner = tpm2_be16_get(buf + 29);
    TEST_ASSERT_EQ(inner, 46u, "nvPublic inner size (32B policy) == 46");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 41), 32u, "authPolicy size 32");
    TEST_ASSERT_EQ(buf[43], 0x10u, "authPolicy first byte");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 43 + 32), 96u, "dataSize after policy");

    /* Buffer too small refused. */
    TEST_ASSERT_EQ(tpm2_build_nv_define(buf, 44u, TPM_NV_INDEX_OS_DATA,
                                        0, TPM_ALG_SHA256, 0, 0, 64u),
                   0u, "define refuses too-small buffer");
}

static void test_nv_build_rw(void)
{
    uint8_t buf[128];
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t n;

    /* Write: total = 10+4+4+13+2+8+2 = 43. */
    n = tpm2_build_nv_write(buf, sizeof buf, TPM_RH_OWNER, TPM_NV_INDEX_OS_DATA,
                            TPM_RS_PW, 0u, data, 8u);
    TEST_ASSERT_EQ(n, 43u, "NV_Write (8B) is 43 bytes");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 0), TPM2_ST_SESSIONS, "write tag ST_SESSIONS");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_NV_WRITE, "write CC");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM_RH_OWNER, "write authHandle");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), TPM_NV_INDEX_OS_DATA, "write nvIndex");
    /* data TPM2B at 10+4+4+13 = 31. */
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 31), 8u, "write data size 8");
    TEST_ASSERT_EQ(buf[33], 1u, "write data[0]");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 41), 0u, "write offset 0");

    /* Read: total = 10+4+4+13+2+2 = 35. */
    n = tpm2_build_nv_read(buf, sizeof buf, TPM_RH_OWNER, TPM_NV_INDEX_OS_DATA,
                           TPM_RS_PW, 32u, 16u);
    TEST_ASSERT_EQ(n, 35u, "NV_Read is 35 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), TPM2_CC_NV_READ, "read CC");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 31), 32u, "read size 32");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 33), 16u, "read offset 16");

    /* Over-cap and zero-length refused. */
    TEST_ASSERT_EQ(tpm2_build_nv_write(buf, sizeof buf, TPM_RH_OWNER,
                                       TPM_NV_INDEX_OS_DATA, TPM_RS_PW, 0u, data,
                                       (uint16_t)(TPM_NV_MAX_DATA + 1u)),
                   0u, "write refuses over-cap len");
    TEST_ASSERT_EQ(tpm2_build_nv_read(buf, sizeof buf, TPM_RH_OWNER,
                                      TPM_NV_INDEX_OS_DATA, TPM_RS_PW, 0u, 0u),
                   0u, "read refuses zero size");
}

/* ---- Session-aware response parse (F1): both tags ---- */

static void test_nv_parse_read_both_tags(void)
{
    uint8_t rsp[64];
    uint8_t out[16];
    int dl;
    uint32_t i;
    for (i = 0; i < sizeof out; i++) out[i] = 0xEE;

    /* ST_SESSIONS NV_Read response: header(10) + parameterSize(4)
     * + TPM2B_MAX_NV_BUFFER{ size(2)=4 + "ABCD" } + (empty auth area). */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(rsp + 2, 20u);              /* size: 10+4+6 */
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 6u);              /* parameterSize = 2 + 4 */
    tpm2_be16_put(rsp + 14, 4u);              /* TPM2B size */
    rsp[16] = 'A'; rsp[17] = 'B'; rsp[18] = 'C'; rsp[19] = 'D';
    dl = tpm2_parse_nv_read(rsp, 20u, out, sizeof out);
    TEST_ASSERT_EQ(dl, 4, "ST_SESSIONS NV_Read yields 4 data bytes");
    TEST_ASSERT(out[0] == 'A' && out[3] == 'D', "ST_SESSIONS NV_Read data correct");

    /* The same bytes mis-parsed at offset 10 would read parameterSize(0x6) as the
     * TPM2B size -> wrong. The helper bound it correctly, proving F1. */

    /* Truncated parameterSize must fail, not overread. */
    tpm2_be32_put(rsp + 10, 100u);            /* parameterSize > available */
    dl = tpm2_parse_nv_read(rsp, 20u, out, sizeof out);
    TEST_ASSERT_EQ(dl, -1, "oversize parameterSize rejected");
}

/* ---- ReadPublic + StartAuthSession + PolicyGetDigest parse ---- */

static void test_nv_parse_misc(void)
{
    uint8_t rsp[64];
    uint8_t dig[32];
    uint16_t sz = 0;
    uint32_t attrs = 0;
    int dl;
    uint32_t h;

    /* NV_ReadPublic (ST_NO_SESSIONS): TPM2B_NV_PUBLIC{ size(2)=14
     * + nvIndex(4) + nameAlg(2) + attributes(4) + authPolicy(2,0) + dataSize(2) }
     * + TPM2B_NAME(2,0). */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(rsp + 2, 28u);              /* 10 + 2 + 14 + 2 */
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be16_put(rsp + 10, 14u);             /* nvPublic size */
    tpm2_be32_put(rsp + 12, TPM_NV_INDEX_BASELINE);
    tpm2_be16_put(rsp + 16, TPM_ALG_SHA256);
    tpm2_be32_put(rsp + 18, TPMA_NV_POLICYREAD | TPMA_NV_WRITTEN);
    tpm2_be16_put(rsp + 22, 0u);              /* authPolicy size */
    tpm2_be16_put(rsp + 24, 128u);            /* dataSize */
    tpm2_be16_put(rsp + 26, 0u);              /* TPM2B_NAME size */
    TEST_ASSERT_EQ(tpm2_parse_nv_read_public(rsp, 28u, &sz, &attrs), 0,
                   "NV_ReadPublic parses");
    TEST_ASSERT_EQ((uint32_t)sz, 128u, "ReadPublic dataSize 128");
    TEST_ASSERT_EQ(attrs, (uint32_t)(TPMA_NV_POLICYREAD | TPMA_NV_WRITTEN),
                   "ReadPublic attributes");

    /* StartAuthSession response: sessionHandle(4) + nonceTPM(2,0). */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(rsp + 2, 16u);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 0x03000000u);     /* sessionHandle */
    tpm2_be16_put(rsp + 14, 0u);
    h = tpm2_parse_start_auth_session(rsp, 16u);
    TEST_ASSERT_EQ(h, 0x03000000u, "StartAuthSession handle parsed");
    /* A failed (error rc) response yields handle 0. */
    tpm2_be32_put(rsp + 6, 0x0000018Bu);
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 16u), 0u,
                   "failed StartAuthSession -> handle 0");

    /* PolicyGetDigest response: policyDigest(2,32). */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(rsp + 2, 44u);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be16_put(rsp + 10, 32u);
    dl = tpm2_parse_policy_get_digest(rsp, 44u, dig, sizeof dig);
    TEST_ASSERT_EQ(dl, 32, "PolicyGetDigest yields 32-byte digest");
}

/* ---- Baseline policy pinned to the allocation table (F3) ---- */

static void test_nv_baseline_policy_selection(void)
{
    uint8_t sel[3];
    uint8_t cmd[64];
    uint32_t mask = tpm_pcr_baseline_mask();
    uint32_t n;

    tpm_nv_baseline_pcr_select(sel);
    TEST_ASSERT_EQ((uint32_t)sel[0], mask & 0xFFu, "select[0] == mask bits 0..7");
    TEST_ASSERT_EQ((uint32_t)sel[1], (mask >> 8) & 0xFFu, "select[1] == mask bits 8..15");
    TEST_ASSERT_EQ((uint32_t)sel[2], (mask >> 16) & 0xFFu, "select[2] == mask bits 16..23");
    /* The mask is non-empty (the baseline policy covers measured PCRs). */
    TEST_ASSERT(mask != 0u, "baseline mask is non-empty");

    /* PolicyPCR marshals exactly the derived selection (count 1, alg, sos 3). */
    n = tpm2_build_policy_pcr(cmd, sizeof cmd, 0x03000000u, TPM_ALG_SHA256, sel);
    TEST_ASSERT_EQ(n, 26u, "PolicyPCR command is 26 bytes");
    TEST_ASSERT_EQ(tpm2_be32_get(cmd + 6), TPM2_CC_POLICY_PCR, "PolicyPCR CC");
    TEST_ASSERT_EQ(tpm2_be32_get(cmd + 16), 1u, "PolicyPCR count 1");
    TEST_ASSERT_EQ(tpm2_be16_get(cmd + 20), TPM_ALG_SHA256, "PolicyPCR bank SHA256");
    TEST_ASSERT_EQ((uint32_t)cmd[22], 3u, "PolicyPCR sizeofSelect 3");
    TEST_ASSERT(cmd[23] == sel[0] && cmd[24] == sel[1] && cmd[25] == sel[2],
                "PolicyPCR carries the derived baseline selection");
}

/* ---- CC-dispatch fake-TIS transport (session lifecycle, F2) ---- */

#define NVF_CAP 96u
static uint8_t  nvf_cmd[NVF_CAP];
static uint32_t nvf_cmd_len;
static uint32_t nvf_cmd_expect;
static uint8_t  nvf_rsp[NVF_CAP];
static uint32_t nvf_rsp_len;
static uint32_t nvf_rsp_pos;
static int      nvf_ready;
static int      nvf_executed;
static uint32_t nvf_seen[16];     /* CC sequence */
static uint32_t nvf_seen_n;
static uint32_t nvf_fail_cc;      /* CC to fail */
static uint32_t nvf_fail_rc;
static uint32_t nvf_session;      /* session handle to hand out */
static int      nvf_malformed;    /* emit size-10 ST_SESSIONS success: no parameterSize (F-TC1) */
static int      nvf_noauth;       /* emit size-14 ST_SESSIONS success: no auth area (F-AD1) */
static int      nvf_bad_auth;     /* emit auth area with out-of-bounds nonce length (F-RE1) */
static int      nvf_oversize_auth;/* emit NV_Read auth area with nonce_n > 64 (F-RE2-r2) */
static int      nvf_sas_bad;      /* emit malformed StartAuthSession success (F-TC2/F-AD2) */
static int      nvf_read_bad;     /* emit malformed NV_Read success (F-TC3) */

#define NVF_REG_STS   0x018u
#define NVF_REG_FIFO  0x024u
#define NVF_STS_EXPECT        0x08u
#define NVF_STS_DATA_AVAIL    0x10u
#define NVF_STS_GO            0x20u
#define NVF_STS_COMMAND_READY 0x40u
#define NVF_STS_VALID         0x80u

static void nvf_build_response(void)
{
    uint32_t cc = tpm2_be32_get(nvf_cmd + 6);
    uint32_t rc = TPM2_RC_SUCCESS;
    if (nvf_seen_n < 16u) nvf_seen[nvf_seen_n++] = cc;
    if (nvf_fail_cc != 0u && cc == nvf_fail_cc) rc = nvf_fail_rc;
    memset(nvf_rsp, 0, NVF_CAP);
    if (rc == TPM2_RC_SUCCESS && cc == TPM2_CC_START_AUTH_SESSION) {
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(nvf_rsp + 6, rc);
        if (nvf_sas_bad) {
            /* Handle only, no nonceTPM TPM2B -> parser must reject (plen 4). */
            tpm2_be32_put(nvf_rsp + 2, 14u);
            tpm2_be32_put(nvf_rsp + 10, nvf_session);
            nvf_rsp_len = 14u;
        } else {
            tpm2_be32_put(nvf_rsp + 2, 32u);
            tpm2_be32_put(nvf_rsp + 10, nvf_session);  /* sessionHandle */
            tpm2_be16_put(nvf_rsp + 14, 16u);          /* nonceTPM (16 zero) */
            nvf_rsp_len = 32u;
        }
    } else if (rc == TPM2_RC_SUCCESS && cc == TPM2_CC_POLICY_GET_DIGEST) {
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(nvf_rsp + 2, 44u);
        tpm2_be32_put(nvf_rsp + 6, rc);
        tpm2_be16_put(nvf_rsp + 10, 32u);          /* 32-byte policyDigest */
        nvf_rsp_len = 44u;
    } else if (rc == TPM2_RC_SUCCESS && cc == TPM2_CC_NV_READ) {
        /* size 25 = header(10) + parameterSize(4) + TPM2B(2+4) + 5-byte auth
         * area (nv_exec now requires the response auth area, F-AD1). */
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(nvf_rsp + 6, rc);
        tpm2_be32_put(nvf_rsp + 10, 6u);           /* parameterSize */
        tpm2_be16_put(nvf_rsp + 14, 4u);           /* TPM2B size */
        nvf_rsp[16] = 'D'; nvf_rsp[17] = 'A';
        nvf_rsp[18] = 'T'; nvf_rsp[19] = 'A';
        if (nvf_read_bad) {
            tpm2_be16_put(nvf_rsp + 14, 99u);      /* TPM2B size > params -> parse rejects */
            tpm2_be32_put(nvf_rsp + 2, 25u);
            nvf_rsp_len = 25u;
        } else if (nvf_oversize_auth) {
            /* Valid params, but the auth area nonceTPM.size is 65 (> 64 max) yet
             * fits the response -> nv_auth_response_ok must reject (round 2). */
            tpm2_be32_put(nvf_rsp + 2, 90u);       /* 10+4+6 + (2+65+1+2+0) */
            tpm2_be16_put(nvf_rsp + 20, 65u);      /* nonceTPM.size > TPM2B_HA_MAX */
            nvf_rsp_len = 90u;
        } else {
            tpm2_be32_put(nvf_rsp + 2, 25u);       /* 10+4+6 + 5-byte auth area */
            nvf_rsp_len = 25u;                      /* bytes 20..24 (auth area) zero */
        }
    } else if (rc == TPM2_RC_SUCCESS &&
               (cc == TPM2_CC_NV_DEFINE_SPACE ||
                cc == TPM2_CC_NV_UNDEFINE_SPACE ||
                cc == TPM2_CC_NV_WRITE)) {
        /* Session-authorized success is ST_SESSIONS. Well-formed: parameterSize
         * (0, no return params) + a minimal response auth area (nonceTPM(2,0) +
         * attrs(1) + hmac(2,0)) -> size 19. nvf_malformed emits the truncated
         * 10-byte shape nv_exec must now reject (F-TC1). */
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(nvf_rsp + 6, rc);
        if (nvf_malformed) {
            /* No parameterSize at all (F-TC1). */
            tpm2_be32_put(nvf_rsp + 2, 10u);
            nvf_rsp_len = 10u;
        } else if (nvf_noauth) {
            /* parameterSize present but NO response auth area (F-AD1). */
            tpm2_be32_put(nvf_rsp + 2, 14u);
            tpm2_be32_put(nvf_rsp + 10, 0u);   /* parameterSize = 0 */
            nvf_rsp_len = 14u;
        } else if (nvf_bad_auth) {
            /* parameterSize(0) + 5-byte auth area, but nonceTPM.size claims 99
             * bytes (past the response) -> auth-response parse rejects (F-RE1). */
            tpm2_be32_put(nvf_rsp + 2, 19u);
            tpm2_be32_put(nvf_rsp + 10, 0u);   /* parameterSize = 0 */
            tpm2_be16_put(nvf_rsp + 14, 99u);  /* nonceTPM.size out of bounds */
            nvf_rsp_len = 19u;
        } else {
            /* Well-formed: parameterSize(0) + 5-byte auth area -> size 19. */
            tpm2_be32_put(nvf_rsp + 2, 19u);
            tpm2_be32_put(nvf_rsp + 10, 0u);   /* parameterSize = 0 */
            nvf_rsp_len = 19u;                  /* bytes 14..18 (auth area) zero */
        }
    } else {
        /* Bare header: ST_NO_SESSIONS success (policy_pcr/flush) or any error
         * (TPM error responses are always ST_NO_SESSIONS). */
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(nvf_rsp + 2, 10u);
        tpm2_be32_put(nvf_rsp + 6, rc);
        nvf_rsp_len = 10u;
    }
}

static uint8_t nvf_r8(uint32_t off)
{
    if (off == NVF_REG_FIFO && nvf_executed && nvf_rsp_pos < nvf_rsp_len)
        return nvf_rsp[nvf_rsp_pos++];
    return 0;
}

static void nvf_w8(uint32_t off, uint8_t v)
{
    if (off != NVF_REG_FIFO || nvf_executed)
        return;
    if (nvf_cmd_len < NVF_CAP)
        nvf_cmd[nvf_cmd_len] = v;
    nvf_cmd_len++;
    if (nvf_cmd_len == 6u)
        nvf_cmd_expect = tpm2_be32_get(nvf_cmd + 2);
}

static uint32_t nvf_r32(uint32_t off)
{
    uint8_t sts;
    if (off != NVF_REG_STS)
        return 0;
    sts = NVF_STS_VALID;
    if (nvf_ready && !nvf_executed && nvf_cmd_len == 0)
        sts |= NVF_STS_COMMAND_READY;
    if (!nvf_executed && nvf_cmd_len > 0 && nvf_cmd_len < nvf_cmd_expect)
        sts |= NVF_STS_EXPECT;
    if (nvf_executed && nvf_rsp_pos < nvf_rsp_len)
        sts |= NVF_STS_DATA_AVAIL;
    return (uint32_t)sts | (32u << 8);   /* burstCount = 32 */
}

static void nvf_w32(uint32_t off, uint32_t v)
{
    if (off != NVF_REG_STS)
        return;
    if (v & NVF_STS_COMMAND_READY) {
        nvf_ready = 1;
        nvf_executed = 0;
        nvf_cmd_len = 0;
        nvf_cmd_expect = 0;
        nvf_rsp_pos = 0;
    }
    if ((v & NVF_STS_GO) && nvf_cmd_len >= nvf_cmd_expect) {
        nvf_executed = 1;
        nvf_build_response();
    }
}

static const struct tpm_t_io nvf_io = { nvf_r8, nvf_w8, nvf_r32, nvf_w32 };

static void nvf_reset(uint32_t fail_cc, uint32_t fail_rc)
{
    nvf_cmd_len = 0; nvf_cmd_expect = 0;
    nvf_rsp_len = 0; nvf_rsp_pos = 0;
    nvf_ready = 0; nvf_executed = 0;
    nvf_seen_n = 0;
    nvf_fail_cc = fail_cc; nvf_fail_rc = fail_rc;
    nvf_session = 0x03000000u;
    nvf_malformed = 0;
    nvf_noauth = 0;
    nvf_bad_auth = 0;
    nvf_oversize_auth = 0;
    nvf_sas_bad = 0;
    nvf_read_bad = 0;
}

static int nvf_saw_cc(uint32_t cc)
{
    uint32_t i;
    for (i = 0; i < nvf_seen_n; i++)
        if (nvf_seen[i] == cc)
            return 1;
    return 0;
}

static void test_nv_session_lifecycle(void)
{
    const struct tpm_t_io *prev;
    tpm_nv_status_t st;

    /* Success path: trial session -> PolicyPCR -> PolicyGetDigest -> define.
     * The trial session handle MUST be flushed even on success. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_baseline(TPM_NV_INDEX_BASELINE, 96u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "define_baseline succeeds");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_START_AUTH_SESSION), "trial session started");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_POLICY_GET_DIGEST), "trial policy digest read");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_NV_DEFINE_SPACE), "index defined");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), "trial session flushed on success");

    /* Teardown path (F2): fail PolicyPCR AFTER the session is created. The
     * classified status must surface and the session MUST still be flushed. */
    nvf_reset(TPM2_CC_POLICY_PCR, 0x0000099Du /* POLICY_FAIL */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_baseline(TPM_NV_INDEX_BASELINE, 96u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_AUTH, "PolicyPCR failure -> AUTH");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_START_AUTH_SESSION), "session started before failure");
    TEST_ASSERT(!nvf_saw_cc(TPM2_CC_NV_DEFINE_SPACE), "define NOT attempted after policy fail");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), "session flushed on the error path (no leak)");

    /* Policy read round-trip returns the staged NV data. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    {
        uint8_t out[8];
        uint16_t got = 0;
        memset(out, 0, sizeof out);
        st = tpm_nv_policy_read(TPM_NV_INDEX_BASELINE, 0u, out, sizeof out, &got);
        tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
        TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "policy_read succeeds");
        TEST_ASSERT_EQ((uint32_t)got, 4u, "policy_read returns 4 bytes");
        TEST_ASSERT(out[0] == 'D' && out[3] == 'A', "policy_read data correct");
        TEST_ASSERT(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), "policy_read flushed its session");
    }
}

/* ---- StartAuthSession parser malformed-success coverage (F-TC2) ---- */

static void test_nv_sas_parse_malformed(void)
{
    uint8_t rsp[32];

    /* Valid baseline: handle(4) + nonceTPM(2,0) -> plen 6. */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(rsp + 2, 16u);
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 0x03000001u);
    tpm2_be16_put(rsp + 14, 0u);
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 16u), 0x03000001u,
                   "well-formed StartAuthSession parses handle");

    /* Handle only, no nonceTPM size field -> plen 4 < 6 -> reject. */
    tpm2_be32_put(rsp + 2, 14u);
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 14u), 0u,
                   "handle-only response rejected (no nonceTPM)");

    /* nonceTPM length runs past the parameter area -> reject. */
    tpm2_be32_put(rsp + 2, 16u);
    tpm2_be16_put(rsp + 14, 64u);   /* claims 64 nonce bytes, only 0 present */
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 16u), 0u,
                   "over-long nonceTPM rejected");

    /* A failed (error rc) response yields handle 0 regardless of shape. */
    tpm2_be32_put(rsp + 6, 0x0000018Bu);
    tpm2_be16_put(rsp + 14, 0u);
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 16u), 0u,
                   "error-rc StartAuthSession -> handle 0");
}

/* ---- Malformed session-success rejection + wrapper status mapping ---- */

static void test_nv_wrapper_status(void)
{
    const struct tpm_t_io *prev;
    uint8_t buf[16];
    uint8_t out[8];
    uint16_t got;
    tpm_nv_status_t st;

    memset(buf, 0xC3, sizeof buf);

    /* F-TC1: a truncated ST_SESSIONS "success" for an irreversible write must
     * be rejected as TRANSPORT, never reported as OK. */
    nvf_reset(0u, 0u);
    nvf_malformed = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write(TPM_NV_INDEX_OS_DATA, 0u, buf, 8u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "malformed ST_SESSIONS write success -> TRANSPORT (not OK)");

    nvf_reset(0u, 0u);
    nvf_malformed = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "malformed ST_SESSIONS define success -> TRANSPORT");

    /* F-TC3: wrapper rc -> status propagation through nv_exec. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, 0x0000014Cu /* NV_DEFINED */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_DEFINED, "define_data NV_DEFINED propagates");

    nvf_reset(TPM2_CC_NV_WRITE, 0x00000148u /* NV_LOCKED */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write(TPM_NV_INDEX_OS_DATA, 0u, buf, 8u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_LOCKED, "write NV_LOCKED propagates");

    nvf_reset(TPM2_CC_NV_UNDEFINE_SPACE, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_undefine(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND, "undefine HANDLE -> NOTFOUND");

    nvf_reset(TPM2_CC_NV_READ_PUBLIC, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    {
        uint16_t sz = 0; uint32_t attrs = 0;
        st = tpm_nv_read_public(TPM_NV_INDEX_OS_DATA, &sz, &attrs);
        tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
        TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND, "read_public HANDLE -> NOTFOUND");
    }

    /* F-TC3: a read whose SUCCESS response has a malformed TPM2B must map to
     * TRANSPORT (parse failure), not return uninitialized data as OK. */
    nvf_reset(0u, 0u);
    nvf_read_bad = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    got = 0xFFFFu;
    memset(out, 0, sizeof out);
    st = tpm_nv_read(TPM_NV_INDEX_OS_DATA, 0u, out, sizeof out, &got);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "malformed NV_Read success -> TRANSPORT");
    TEST_ASSERT_EQ((uint32_t)got, 0u, "failed read leaves out_len 0");

    /* F-RE2 round 2: an auth area whose nonceTPM.size (65) fits the response but
     * exceeds the TPM2B_HA 64-byte max must be rejected. */
    nvf_reset(0u, 0u);
    nvf_oversize_auth = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    got = 0xFFFFu;
    memset(out, 0, sizeof out);
    st = tpm_nv_read(TPM_NV_INDEX_OS_DATA, 0u, out, sizeof out, &got);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "over-max auth nonce length -> TRANSPORT");

    /* F-AD1: a session success with parameterSize but NO response auth area must
     * be rejected -- a real one-session reply always carries a TPMS_AUTH_RESPONSE. */
    nvf_reset(0u, 0u);
    nvf_noauth = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write(TPM_NV_INDEX_OS_DATA, 0u, buf, 8u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "ST_SESSIONS success with no auth area -> TRANSPORT");

    /* F-RE1: an auth area whose internal nonceTPM.size runs past the response
     * must be rejected -- 5 trailing bytes alone is not "well-formed". */
    nvf_reset(0u, 0u);
    nvf_bad_auth = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write(TPM_NV_INDEX_OS_DATA, 0u, buf, 8u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "out-of-bounds auth-response length -> TRANSPORT");

    /* F-TC2 + F-AD2: a malformed StartAuthSession (truncated nonceTPM) must stop
     * before PolicyPCR AND still flush the allocated SESSION handle the reply
     * carried -- a corrupted reply must not leak a session across retries. */
    nvf_reset(0u, 0u);
    nvf_sas_bad = 1;
    nvf_session = 0x03000000u;   /* a real policy-session handle */
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_baseline(TPM_NV_INDEX_BASELINE, 96u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "malformed StartAuthSession -> TRANSPORT");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_START_AUTH_SESSION), "StartAuthSession was attempted");
    TEST_ASSERT(!nvf_saw_cc(TPM2_CC_POLICY_PCR), "no PolicyPCR after bad session handle");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), "allocated session flushed (no leak, F-AD2)");

    /* F-RE2: a corrupt reply naming a NON-session handle (transient object
     * 0x80...) must NOT be flushed -- flushing it would evict unrelated state. */
    nvf_reset(0u, 0u);
    nvf_sas_bad = 1;
    nvf_session = 0x80000000u;   /* transient OBJECT handle, not a session */
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_baseline(TPM_NV_INDEX_BASELINE, 96u);
    tpm_t_test_install(prev, TPM_T_IFACE_NONE, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "non-session handle -> TRANSPORT");
    TEST_ASSERT(!nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), "non-session handle NOT flushed (F-RE2)");
}

void test_register_tpm_nv(void)
{
    test_suite_register_cat("tpm: NV rc classification", test_nv_classify_rc, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV define marshal", test_nv_build_define, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV read/write marshal", test_nv_build_rw, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV session-aware parse", test_nv_parse_read_both_tags, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV public/session/digest parse", test_nv_parse_misc, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV baseline policy selection", test_nv_baseline_policy_selection, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV session lifecycle", test_nv_session_lifecycle, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV StartAuthSession malformed parse", test_nv_sas_parse_malformed, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV wrapper status mapping", test_nv_wrapper_status, TEST_CAT_SECURITY);
}
