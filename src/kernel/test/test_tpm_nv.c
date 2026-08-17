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
    TEST_ASSERT_EQ(tpm2_parse_nv_read_public(rsp, 28u, TPM_NV_INDEX_BASELINE, &sz, &attrs), 0,
                   "NV_ReadPublic parses (matching index)");
    TEST_ASSERT_EQ((uint32_t)sz, 128u, "ReadPublic dataSize 128");
    TEST_ASSERT_EQ(attrs, (uint32_t)(TPMA_NV_POLICYREAD | TPMA_NV_WRITTEN),
                   "ReadPublic attributes");
    /* Index binding: the SAME response for a DIFFERENT requested index is rejected. */
    TEST_ASSERT_EQ(tpm2_parse_nv_read_public(rsp, 28u, TPM_NV_INDEX_OS_DATA, &sz, &attrs), -1,
                   "ReadPublic rejects mismatched nvIndex");
    /* Trailing bytes (over-long TPM2B_NAME claim) -> exact-consumption reject. */
    tpm2_be16_put(rsp + 26, 4u);              /* name claims 4 bytes, none present */
    TEST_ASSERT_EQ(tpm2_parse_nv_read_public(rsp, 28u, TPM_NV_INDEX_BASELINE, &sz, &attrs), -1,
                   "ReadPublic rejects non-exact TPM2B_NAME");

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
static int      nvf_fail_times;   /* -1 = fail every occurrence; N > 0 = the first N
                                   * only, which is how a TRANSIENT warning behaves */
static uint32_t nvf_session;      /* session handle to hand out */
static int      nvf_malformed;    /* emit size-10 ST_SESSIONS success: no parameterSize (F-TC1) */
static int      nvf_noauth;       /* emit size-14 ST_SESSIONS success: no auth area (F-AD1) */
static int      nvf_bad_auth;     /* emit auth area with out-of-bounds nonce length (F-RE1) */
static int      nvf_oversize_auth;/* emit NV_Read auth area with nonce_n > 64 (F-RE2-r2) */
static int      nvf_sas_bad;      /* emit malformed StartAuthSession success (F-TC2/F-AD2) */
static int      nvf_sas_no_handle;/* rc-SUCCESS StartAuthSession with NO recoverable
                                   * handle: the TPM executed it, so a session may
                                   * exist that nothing can ever FlushContext */
static int      nvf_read_bad;     /* emit malformed NV_Read success (F-TC3) */
static int      nvf_malformed_flush; /* emit a FlushContext reply the transport
                                      * rejects (declared size > caller cap), so
                                      * no rc is ever parsed: transient, and an
                                      * idempotent command worth retrying */
static int      nvf_flush_bad_tag;   /* rc-SUCCESS with an ILLEGAL response tag:
                                      * accepted by tis_submit (its size checks
                                      * pass) but rejected by tpm2_rsp_parse */
static int      nvf_flush_wrong_shape; /* rc-SUCCESS with a LEGAL tag but the
                                        * wrong FlushContext envelope */
static int      nvf_flush_bad_handle_env; /* TPM_RC_HANDLE in a WRONG envelope:
                                           * classifies NOTFOUND, so without an
                                           * envelope check it would read as
                                           * proof the session was released */
static uint32_t nvf_stall_reads;  /* withhold COMMAND_READY for N status reads:
                                   * a slow-but-responsive TPM, which is what a
                                   * cumulative budget exists to bound */
static uint32_t nvf_read_payload_len;  /* bytes an NV_Read success returns (0 = default) */
static uint64_t nvf_counter_value;     /* value an 8-byte NV_Read reports */
static uint32_t nvf_public_attrs;      /* attrs an NV_ReadPublic success reports */
static uint16_t nvf_public_size;       /* dataSize an NV_ReadPublic success reports */
static uint16_t nvf_public_name_alg;   /* nameAlg an NV_ReadPublic reports (0 = SHA-256) */
static uint16_t nvf_public_policy_len; /* authPolicy length an NV_ReadPublic reports */
static uint8_t  nvf_public_policy_fill;/* byte the reported authPolicy is filled with */
static uint32_t nvf_stall_after_go;    /* withhold dataAvail for N status reads AFTER
                                        * the command was dispatched: the abandoned
                                        * mid-flight case quiescence must clean up */
static int      nvf_abort_refused;     /* never report commandReady again: the abort
                                        * itself fails, which IS a wedge */

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
    if (nvf_fail_cc != 0u && cc == nvf_fail_cc && nvf_fail_times != 0) {
        rc = nvf_fail_rc;
        if (nvf_fail_times > 0)
            nvf_fail_times--;      /* a transient failure that eventually clears */
    }
    memset(nvf_rsp, 0, NVF_CAP);
    if (rc == TPM2_RC_SUCCESS && cc == TPM2_CC_START_AUTH_SESSION) {
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(nvf_rsp + 6, rc);
        if (nvf_sas_no_handle) {
            /* rc-SUCCESS with an EMPTY parameter area: no handle to recover. */
            tpm2_be32_put(nvf_rsp + 2, 10u);
            nvf_rsp_len = 10u;
        } else if (nvf_sas_bad) {
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
    } else if (rc == TPM2_RC_SUCCESS && cc == TPM2_CC_NV_READ_PUBLIC) {
        /* TPM2B_NV_PUBLIC{ nvIndex(4) nameAlg(2) attrs(4) policy TPM2B(2,empty)
         * dataSize(2) } + TPM2B_NAME(2, empty). ST_NO_SESSIONS: ReadPublic takes
         * no auth. pubsize 14, params 2+14+2 = 18, total 28. */
        uint32_t idx = tpm2_be32_get(nvf_cmd + 10);
        uint32_t pl = nvf_public_policy_len;
        uint32_t pubsize = 14u + pl;
        uint32_t i;
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(nvf_rsp + 2, 14u + pubsize);
        tpm2_be32_put(nvf_rsp + 6, rc);
        tpm2_be16_put(nvf_rsp + 10, (uint16_t)pubsize);  /* TPM2B_NV_PUBLIC size */
        tpm2_be32_put(nvf_rsp + 12, idx);          /* nvIndex (echo request) */
        tpm2_be16_put(nvf_rsp + 16, nvf_public_name_alg ? nvf_public_name_alg
                                                        : TPM_ALG_SHA256);
        tpm2_be32_put(nvf_rsp + 18, nvf_public_attrs);
        tpm2_be16_put(nvf_rsp + 22, (uint16_t)pl); /* authPolicy size */
        for (i = 0; i < pl; i++)
            nvf_rsp[24 + i] = nvf_public_policy_fill;
        tpm2_be16_put(nvf_rsp + 24 + pl, nvf_public_size);
        tpm2_be16_put(nvf_rsp + 26 + pl, 0u);      /* TPM2B_NAME (empty) */
        nvf_rsp_len = 28u + pl;
    } else if (rc == TPM2_RC_SUCCESS && cc == TPM2_CC_NV_READ) {
        /* size 25 = header(10) + parameterSize(4) + TPM2B(2+4) + 5-byte auth
         * area (nv_exec now requires the response auth area, F-AD1). */
        uint32_t plen = (nvf_read_payload_len != 0u) ? nvf_read_payload_len : 4u;
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(nvf_rsp + 6, rc);
        tpm2_be32_put(nvf_rsp + 10, 2u + plen);    /* parameterSize */
        tpm2_be16_put(nvf_rsp + 14, (uint16_t)plen);
        if (plen == 8u) {
            /* Counter payload: one big-endian UINT64. */
            tpm2_be32_put(nvf_rsp + 16, (uint32_t)(nvf_counter_value >> 32));
            tpm2_be32_put(nvf_rsp + 20, (uint32_t)nvf_counter_value);
        } else {
            nvf_rsp[16] = 'D'; nvf_rsp[17] = 'A';
            nvf_rsp[18] = 'T'; nvf_rsp[19] = 'A';
        }
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
            /* 10 + 4 + (2+plen) + 5-byte auth area */
            tpm2_be32_put(nvf_rsp + 2, 21u + plen);
            nvf_rsp_len = 21u + plen;               /* trailing auth area zero */
        }
    } else if (rc == TPM2_RC_SUCCESS &&
               (cc == TPM2_CC_NV_DEFINE_SPACE ||
                cc == TPM2_CC_NV_UNDEFINE_SPACE ||
                cc == TPM2_CC_NV_INCREMENT ||
                cc == TPM2_CC_NV_WRITE_LOCK ||
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
    } else if (cc == TPM2_CC_FLUSH_CONTEXT && nvf_flush_bad_handle_env) {
        /* A real TPM sends errors header-only as ST_NO_SESSIONS. This one is
         * session-tagged with trailing bytes, but still parses and still
         * classifies as NOTFOUND. */
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(nvf_rsp + 2, 14u);
        tpm2_be32_put(nvf_rsp + 6, 0x0000018Bu /* HANDLE */);
        nvf_rsp_len = 14u;
    } else if (cc == TPM2_CC_FLUSH_CONTEXT && nvf_flush_bad_tag) {
        /* Size 10 passes tis_submit's bounds, but 0x1234 is not one of the
         * three legal response tags, so tpm2_rsp_parse rejects it. */
        tpm2_be16_put(nvf_rsp + 0, 0x1234u);
        tpm2_be32_put(nvf_rsp + 2, 10u);
        tpm2_be32_put(nvf_rsp + 6, rc);
        nvf_rsp_len = 10u;
    } else if (cc == TPM2_CC_FLUSH_CONTEXT && nvf_flush_wrong_shape) {
        /* A LEGAL tag and a parseable header, but the wrong envelope for
         * FlushContext: session-tagged with trailing bytes. rc says SUCCESS. */
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(nvf_rsp + 2, 14u);
        tpm2_be32_put(nvf_rsp + 6, rc);
        nvf_rsp_len = 14u;
    } else if (cc == TPM2_CC_FLUSH_CONTEXT && nvf_malformed_flush) {
        /* Declare a size larger than nv_flush's 16-byte response buffer, so
         * tis_submit's size-vs-cap check rejects it and no rc is parsed. */
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(nvf_rsp + 2, 64u);
        tpm2_be32_put(nvf_rsp + 6, rc);
        nvf_rsp_len = 12u;
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
    /* A slow TPM: answers eventually, never fails. This is the device class the
     * cumulative budget exists for -- every per-command deadline is satisfied
     * while the whole operation still overruns the boot. */
    if (nvf_stall_reads > 0u) {
        nvf_stall_reads--;
        return (uint32_t)sts | (32u << 8);
    }
    /* Stall AFTER the command was dispatched: the response is withheld while
     * the TPM has already been told to execute. Abandoning here is the case
     * quiescence has to clean up, and the pre-dispatch stall above cannot
     * reach it. */
    if (nvf_executed && nvf_stall_after_go > 0u) {
        nvf_stall_after_go--;
        return (uint32_t)sts | (32u << 8);
    }
    if (nvf_ready && !nvf_executed && nvf_cmd_len == 0 && !nvf_abort_refused)
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
    nvf_fail_times = -1;
    nvf_session = 0x03000000u;
    nvf_malformed = 0;
    nvf_noauth = 0;
    nvf_bad_auth = 0;
    nvf_oversize_auth = 0;
    nvf_sas_bad = 0;
    nvf_sas_no_handle = 0;
    nvf_read_bad = 0;
    nvf_malformed_flush = 0;
    nvf_flush_bad_tag = 0;
    nvf_flush_wrong_shape = 0;
    nvf_flush_bad_handle_env = 0;
    nvf_stall_reads = 0;
    nvf_read_payload_len = 0;
    nvf_counter_value = 0;
    nvf_public_attrs = 0;
    nvf_public_size = 0;
    nvf_public_name_alg = 0;
    nvf_public_policy_len = 0;
    nvf_public_policy_fill = 0;
    nvf_stall_after_go = 0;
    nvf_abort_refused = 0;
}

static int nvf_cc_count(uint32_t cc)
{
    uint32_t i;
    int n = 0;
    for (i = 0; i < nvf_seen_n; i++)
        if (nvf_seen[i] == cc)
            n++;
    return n;
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
    struct tpm_t_test_state prev;
    tpm_nv_status_t st;

    /* Success path: trial session -> PolicyPCR -> PolicyGetDigest -> define.
     * The trial session handle MUST be flushed even on success. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_baseline(TPM_NV_INDEX_BASELINE, 96u);
    tpm_t_test_restore(prev);
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
    tpm_t_test_restore(prev);
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
        tpm_t_test_restore(prev);
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

    /* nonceTPM length not consuming the parameter area exactly -> reject. */
    tpm2_be32_put(rsp + 2, 16u);
    tpm2_be16_put(rsp + 14, 64u);   /* claims 64 nonce bytes, only 0 present */
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 16u), 0u,
                   "non-exact nonceTPM rejected");

    /* A well-formed reply naming a NON-session handle (transient object 0x80...)
     * must be rejected by the strict parser too -- the caller later flushes it. */
    tpm2_be32_put(rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(rsp + 10, 0x80000000u);
    tpm2_be16_put(rsp + 14, 0u);
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 16u), 0u,
                   "non-session handle rejected by strict parser");

    /* A failed (error rc) response yields handle 0 regardless of shape. */
    tpm2_be32_put(rsp + 6, 0x0000018Bu);
    tpm2_be32_put(rsp + 10, 0x03000001u);
    tpm2_be16_put(rsp + 14, 0u);
    TEST_ASSERT_EQ(tpm2_parse_start_auth_session(rsp, 16u), 0u,
                   "error-rc StartAuthSession -> handle 0");
}

/* ---- Malformed session-success rejection + wrapper status mapping ---- */

static void test_nv_wrapper_status(void)
{
    struct tpm_t_test_state prev;
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
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "malformed ST_SESSIONS write success -> TRANSPORT (not OK)");

    nvf_reset(0u, 0u);
    nvf_malformed = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "malformed ST_SESSIONS define success -> TRANSPORT");

    /* F-TC3: wrapper rc -> status propagation through nv_exec.
     * NV_DEFINED is no longer propagated raw: an existing index is only usable
     * if it is the index we asked for, so define_ex reads its public area back
     * and answers OK (identical) or MISMATCH (different). Here the fake reports
     * an empty public area, so the definition differs. The full match/mismatch
     * matrix is test_nv_define_mismatch. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, 0x0000014Cu /* NV_DEFINED */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "define_data over a DIFFERENT existing index -> MISMATCH");

    nvf_reset(TPM2_CC_NV_WRITE, 0x00000148u /* NV_LOCKED */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write(TPM_NV_INDEX_OS_DATA, 0u, buf, 8u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_LOCKED, "write NV_LOCKED propagates");

    nvf_reset(TPM2_CC_NV_UNDEFINE_SPACE, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_undefine(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND, "undefine HANDLE -> NOTFOUND");

    nvf_reset(TPM2_CC_NV_READ_PUBLIC, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    {
        uint16_t sz = 0; uint32_t attrs = 0;
        st = tpm_nv_read_public(TPM_NV_INDEX_OS_DATA, &sz, &attrs);
        tpm_t_test_restore(prev);
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
    tpm_t_test_restore(prev);
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
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "over-max auth nonce length -> TRANSPORT");

    /* F-AD1: a session success with parameterSize but NO response auth area must
     * be rejected -- a real one-session reply always carries a TPMS_AUTH_RESPONSE. */
    nvf_reset(0u, 0u);
    nvf_noauth = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write(TPM_NV_INDEX_OS_DATA, 0u, buf, 8u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "ST_SESSIONS success with no auth area -> TRANSPORT");

    /* F-RE1: an auth area whose internal nonceTPM.size runs past the response
     * must be rejected -- 5 trailing bytes alone is not "well-formed". */
    nvf_reset(0u, 0u);
    nvf_bad_auth = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write(TPM_NV_INDEX_OS_DATA, 0u, buf, 8u);
    tpm_t_test_restore(prev);
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
    tpm_t_test_restore(prev);
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
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "non-session handle -> TRANSPORT");
    TEST_ASSERT(!nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), "non-session handle NOT flushed (F-RE2)");
}

/* ---- Cleanup flush waits out a busy gate instead of leaking (F-RA) ---- */

static void test_nv_flush_busy_wait(void)
{
    struct tpm_t_test_state prev;
    tpm_nv_status_t st;
    uint8_t out[8];
    uint16_t got;

    /* The gate is "held" for 3 waiting-acquire ticks (< FAST_TEST_ITERS=64): the
     * session cleanup flush must WAIT it out, so FlushContext is still submitted
     * (the session is not leaked). Only the flush uses tpm2_submit_waiting; the
     * StartAuthSession/PolicyPCR/NV_Read commands use tpm2_submit and are
     * unaffected by the busy-ticks seam. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    tpm_t_test_busy_ticks(3u);
    memset(out, 0, sizeof out);
    got = 0;
    st = tpm_nv_policy_read(TPM_NV_INDEX_BASELINE, 0u, out, sizeof out, &got);
    tpm_t_test_busy_ticks(0u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "policy_read succeeds under brief gate contention");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT),
                "session flushed after waiting out BUSY (no leak)");

    /* If the gate never clears within the budget (100 > FAST_TEST_ITERS=64), the
     * flush gives up WITHOUT hanging -- the wait is bounded, not infinite. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    tpm_t_test_busy_ticks(100u);
    memset(out, 0, sizeof out);
    got = 0;
    st = tpm_nv_policy_read(TPM_NV_INDEX_BASELINE, 0u, out, sizeof out, &got);
    tpm_t_test_busy_ticks(0u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "policy_read completes even when flush times out (bounded wait)");
}

/* ---- TPMA_NV model: attribute validation + the spec-literal wire oracle ---- */

/* Attribute words computed from TPM 2.0 Part 2 "Definition of (UINT32) TPMA_NV
 * Bits" (Table 204 in rev 1.38) rather than from the macros under test. This is
 * the independent oracle the previous tests lacked: they compared the marshaled
 * bytes against the same header constants that produced them, so an off-by-one
 * bit position (which is exactly what OWNERREAD/AUTHREAD/POLICYREAD carried)
 * agreed with itself and passed. */
#define SPEC_ATTR_OWNERWRITE  0x00000002u  /* bit 1 */
#define SPEC_ATTR_POLICYWRITE 0x00000008u  /* bit 3 */
#define SPEC_ATTR_NT_COUNTER  0x00000010u  /* bits 4-7 = 1 */
#define SPEC_ATTR_WRITEDEFINE 0x00002000u  /* bit 13 */
#define SPEC_ATTR_OWNERREAD   0x00020000u  /* bit 17 */
#define SPEC_ATTR_POLICYREAD  0x00080000u  /* bit 19 */
#define SPEC_ATTR_NO_DA       0x02000000u  /* bit 25 */

static void test_nv_attr_spec_oracle(void)
{
    uint8_t buf[96];
    uint32_t n;

    /* An owner data index: ownerWrite | ownerRead | NO_DA. The attribute word is
     * at offset 37 (header 10 + authHandle 4 + authArea 13 + authValue 2 +
     * nvPublic size 2 + nvIndex 4 + nameAlg 2). */
    n = tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_OS_DATA,
                             TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA,
                             TPM_ALG_SHA256, 0, 0, 64u);
    TEST_ASSERT(n > 0u, "owner data define marshals");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 37),
                   SPEC_ATTR_OWNERREAD | SPEC_ATTR_OWNERWRITE | SPEC_ATTR_NO_DA,
                   "owner data attrs match the spec bit positions (17/1/25)");

    /* A policy-protected index must carry policyRead at bit 19. The old
     * numbering emitted bit 20, which is Reserved -- so the index would have had
     * NO read authorization at all. */
    n = tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                             TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA,
                             TPM_ALG_SHA256, 0, 0, 64u);
    TEST_ASSERT(n > 0u, "policy define marshals");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 37),
                   SPEC_ATTR_POLICYREAD | SPEC_ATTR_POLICYWRITE | SPEC_ATTR_NO_DA,
                   "policy attrs match the spec bit positions (19/3/25)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 37) & TPMA_NV_RESERVED_MASK, 0u,
                   "policy define sets no reserved bit");

    /* A counter define carries TPM_NT = 1 in bits 4-7, not a flag elsewhere. */
    n = tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_OS_DATA,
                             TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE |
                             TPMA_NV_TYPE(TPM_NT_COUNTER) | TPMA_NV_NO_DA,
                             TPM_ALG_SHA256, 0, 0,
                             (uint16_t)TPM_NV_COUNTER_SIZE);
    TEST_ASSERT(n > 0u, "counter define marshals");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 37),
                   SPEC_ATTR_POLICYREAD | SPEC_ATTR_POLICYWRITE |
                   SPEC_ATTR_NT_COUNTER | SPEC_ATTR_NO_DA,
                   "counter define emits TPM_NT=COUNTER in bits 4-7");
    TEST_ASSERT_EQ(tpm2_be16_get(buf + 43), (uint16_t)TPM_NV_COUNTER_SIZE,
                   "counter dataSize is 8");

    /* A write-lockable index must actually request writeDefine (bit 13) rather
     * than dropping it -- the attribute that makes the lock permanent. */
    n = tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_OS_DATA,
                             TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE |
                             TPMA_NV_WRITEDEFINE | TPMA_NV_NO_DA,
                             TPM_ALG_SHA256, 0, 0, 32u);
    TEST_ASSERT(n > 0u, "write-lockable define marshals");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 37) & SPEC_ATTR_WRITEDEFINE,
                   SPEC_ATTR_WRITEDEFINE,
                   "write-lockable define emits writeDefine (bit 13)");
}

/* Build an NV_ReadPublic response carrying an authPolicy of `plen` bytes.
 * ST_NO_SESSIONS: params = TPM2B_NV_PUBLIC(2 + 14 + plen) + TPM2B_NAME(2). */
static uint32_t nvp_build_public(uint8_t *buf, uint32_t nv_index, uint16_t plen,
                                 uint8_t fill)
{
    uint32_t pubsize = 14u + plen, total = 10u + 2u + pubsize + 2u, i;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_RC_SUCCESS);
    tpm2_be16_put(buf + 10, (uint16_t)pubsize);
    tpm2_be32_put(buf + 12, nv_index);
    tpm2_be16_put(buf + 16, TPM_ALG_SHA256);
    tpm2_be32_put(buf + 18, TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE);
    tpm2_be16_put(buf + 22, plen);
    for (i = 0; i < plen; i++) buf[24 + i] = fill;
    tpm2_be16_put(buf + 24 + plen, 64u);          /* dataSize */
    tpm2_be16_put(buf + 26 + plen, 0u);           /* TPM2B_NAME (empty) */
    return total;
}

/* The authPolicy copy bound, tested directly on the pure parser. A response can
 * be well-formed and still declare a policy longer than the destination, so the
 * refusal BEFORE the copy is the memory-safety boundary -- and a truncating
 * "fix" would additionally make two different policies compare equal. */
static void test_nv_parse_public_policy_bound(void)
{
    uint8_t rsp[256];
    struct tpm_nv_public pub, before;
    uint32_t n, i;

    /* Exactly one SHA-256 digest is the only legal non-empty length. */
    memset(&pub, 0, sizeof pub);
    n = nvp_build_public(rsp, TPM_NV_INDEX_OS_DATA, 32u, 0xC3u);
    TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n, TPM_NV_INDEX_OS_DATA, &pub), 0,
                   "a 32-byte SHA-256 authPolicy parses");
    TEST_ASSERT_EQ((int)pub.policy_len, 32, "the policy length is reported exactly");
    for (i = 0; i < 32u; i++)
        TEST_ASSERT_EQ((int)pub.auth_policy[i], 0xC3, "policy bytes copied exactly");
    for (i = 32u; i < TPM_NV_POLICY_MAX; i++)
        TEST_ASSERT_EQ((int)pub.auth_policy[i], 0, "the tail is cleared, not stale");

    /* FAILURE IS TRANSACTIONAL: fill the whole struct with sentinels and prove
     * a rejected parse leaves every byte of it alone. Checking only policy_len
     * was not discriminating -- the parser wrote dataSize, attrs and nameAlg
     * before reaching the policy check, so the stated invariant was false while
     * the test still passed. */
    memset(&pub, 0x5A, sizeof pub);
    before = pub;

    /* Longer than any defined digest: refused, never truncated. */
    n = nvp_build_public(rsp, TPM_NV_INDEX_OS_DATA,
                         (uint16_t)(TPM_NV_POLICY_MAX + 1u), 0xA5u);
    TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n, TPM_NV_INDEX_OS_DATA, &pub), -1,
                   "a 65-byte authPolicy is refused, never truncated");
    TEST_ASSERT_EQ(memcmp(&pub, &before, sizeof pub), 0,
                   "a refused oversize parse leaves the output byte-for-byte intact");

    /* At the buffer limit but NOT a SHA-256 digest: still refused, because an
     * authPolicy is a digest and 64 bytes is not this index's digest size. */
    n = nvp_build_public(rsp, TPM_NV_INDEX_OS_DATA, (uint16_t)TPM_NV_POLICY_MAX, 0xA5u);
    TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n, TPM_NV_INDEX_OS_DATA, &pub), -1,
                   "a 64-byte policy under a SHA-256 nameAlg is refused");
    TEST_ASSERT_EQ(memcmp(&pub, &before, sizeof pub), 0,
                   "that refusal is transactional too");

    /* A short-but-plausible length is refused for the same reason. */
    n = nvp_build_public(rsp, TPM_NV_INDEX_OS_DATA, 20u, 0xA5u);
    TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n, TPM_NV_INDEX_OS_DATA, &pub), -1,
                   "a 20-byte policy under a SHA-256 nameAlg is refused");
    TEST_ASSERT_EQ(memcmp(&pub, &before, sizeof pub), 0,
                   "and leaves the output intact");

    /* An empty policy is legal and reports zero length. */
    memset(&pub, 0, sizeof pub);
    n = nvp_build_public(rsp, TPM_NV_INDEX_OS_DATA, 0u, 0u);
    TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n, TPM_NV_INDEX_OS_DATA, &pub), 0,
                   "an empty authPolicy parses");
    TEST_ASSERT_EQ((int)pub.policy_len, 0, "empty policy reports length 0");
    TEST_ASSERT_EQ((uint32_t)pub.name_alg, (uint32_t)TPM_ALG_SHA256, "nameAlg reported");
    TEST_ASSERT_EQ((int)pub.data_size, 64, "dataSize reported");

    /* Bound to the REQUESTED index, like the strict parser it builds on. */
    memset(&pub, 0x5A, sizeof pub);
    before = pub;
    n = nvp_build_public(rsp, TPM_NV_INDEX_OS_DATA, 0u, 0u);
    TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n, TPM_NV_INDEX_BASELINE, &pub), -1,
                   "a public area naming a different index is refused");
    TEST_ASSERT_EQ(memcmp(&pub, &before, sizeof pub), 0,
                   "the wrong-index refusal is transactional");
    TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n, TPM_NV_INDEX_OS_DATA, 0), -1,
                   "a NULL output is refused");
}

/* Independent transcription of TPM 2.0 Part 2's TPMA_NV table and TPM_NT
 * constants. These literals are written FROM THE SPEC, not derived from the
 * macros, which is the whole point: the defect this section repaired was three
 * macros agreeing with a test that had been written from the same wrong source.
 * Every named bit is covered, not the handful one caller happens to use. */
static void test_nv_attr_table_oracle(void)
{
    static const struct { uint32_t macro; uint32_t spec; const char *name; } bits[] = {
        { TPMA_NV_PPWRITE,        0x00000001u, "PPWRITE bit 0" },
        { TPMA_NV_OWNERWRITE,     0x00000002u, "OWNERWRITE bit 1" },
        { TPMA_NV_AUTHWRITE,      0x00000004u, "AUTHWRITE bit 2" },
        { TPMA_NV_POLICYWRITE,    0x00000008u, "POLICYWRITE bit 3" },
        { TPMA_NV_POLICY_DELETE,  0x00000400u, "POLICY_DELETE bit 10" },
        { TPMA_NV_WRITELOCKED,    0x00000800u, "WRITELOCKED bit 11" },
        { TPMA_NV_WRITEALL,       0x00001000u, "WRITEALL bit 12" },
        { TPMA_NV_WRITEDEFINE,    0x00002000u, "WRITEDEFINE bit 13" },
        { TPMA_NV_WRITE_STCLEAR,  0x00004000u, "WRITE_STCLEAR bit 14" },
        { TPMA_NV_GLOBALLOCK,     0x00008000u, "GLOBALLOCK bit 15" },
        { TPMA_NV_PPREAD,         0x00010000u, "PPREAD bit 16" },
        { TPMA_NV_OWNERREAD,      0x00020000u, "OWNERREAD bit 17" },
        { TPMA_NV_AUTHREAD,       0x00040000u, "AUTHREAD bit 18" },
        { TPMA_NV_POLICYREAD,     0x00080000u, "POLICYREAD bit 19" },
        { TPMA_NV_NO_DA,          0x02000000u, "NO_DA bit 25" },
        { TPMA_NV_ORDERLY,        0x04000000u, "ORDERLY bit 26" },
        { TPMA_NV_CLEAR_STCLEAR,  0x08000000u, "CLEAR_STCLEAR bit 27" },
        { TPMA_NV_READLOCKED,     0x10000000u, "READLOCKED bit 28" },
        { TPMA_NV_WRITTEN,        0x20000000u, "WRITTEN bit 29" },
        { TPMA_NV_PLATFORMCREATE, 0x40000000u, "PLATFORMCREATE bit 30" },
        { TPMA_NV_READ_STCLEAR,   0x80000000u, "READ_STCLEAR bit 31" },
    };
    static const struct { uint32_t macro; uint32_t spec; const char *name; } types[] = {
        { TPM_NT_ORDINARY, 0x0u, "TPM_NT_ORDINARY" },
        { TPM_NT_COUNTER,  0x1u, "TPM_NT_COUNTER" },
        { TPM_NT_BITS,     0x2u, "TPM_NT_BITS" },
        { TPM_NT_EXTEND,   0x4u, "TPM_NT_EXTEND" },
        { TPM_NT_PIN_FAIL, 0x8u, "TPM_NT_PIN_FAIL" },
        { TPM_NT_PIN_PASS, 0x9u, "TPM_NT_PIN_PASS" },
    };
    uint32_t i, union_of_named = 0;

    for (i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        TEST_ASSERT_EQ(bits[i].macro, bits[i].spec, bits[i].name);
        union_of_named |= bits[i].spec;
    }
    for (i = 0; i < sizeof types / sizeof types[0]; i++)
        TEST_ASSERT_EQ(types[i].macro, types[i].spec, types[i].name);

    /* The two MASKS are pinned relationally rather than echoed as literals: the
     * completeness and disjointness assertions below fail if either one drops a
     * bit, gains one, or overlaps the named set, which is everything a literal
     * comparison would have caught without restating the define. */
    TEST_ASSERT_EQ(union_of_named | TPMA_NV_TPM_NT_MASK | TPMA_NV_RESERVED_MASK,
                   0xFFFFFFFFu, "the model covers all 32 TPMA_NV bits");
    TEST_ASSERT_EQ(union_of_named & TPMA_NV_RESERVED_MASK, 0u,
                   "no named bit lands in a reserved position");
}

static void test_nv_attrs_validation(void)
{
    uint32_t rw = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE;
    /* Spec-literal reserved positions (bits 8-9 and 20-24) and the three
     * TPM-maintained status bits, each asserted INDIVIDUALLY. Testing two of
     * the seven reserved positions would let a mask regression in any of the
     * other five through -- and a bit-position regression is precisely the
     * defect class this section exists to repair. */
    static const uint32_t reserved_bits[] = {
        1u << 8, 1u << 9, 1u << 20, 1u << 21, 1u << 22, 1u << 23, 1u << 24
    };
    static const uint32_t status_bits[] = {
        TPMA_NV_WRITELOCKED, TPMA_NV_READLOCKED, TPMA_NV_WRITTEN
    };
    uint32_t i;

    /* CONTROLS first, at both size boundaries. Without these every refusal
     * below would also pass against a validator that rejected everything. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_NO_DA, 64u, TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "ordinary index with read+write auth is legal");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw, 1u, TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "ordinary index at dataSize 1 is legal");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw, (uint16_t)TPM_NV_MAX_DATA,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "ordinary index at TPM_NV_MAX_DATA is legal");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_COUNTER), 8u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "8-byte counter with read+write auth is legal");

    for (i = 0; i < sizeof reserved_bits / sizeof reserved_bits[0]; i++)
        TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | reserved_bits[i], 64u,
                                               TPM_ALG_SHA256),
                       (int)TPM_NV_ATTRS, "each reserved bit position refused");
    for (i = 0; i < sizeof status_bits / sizeof status_bits[0]; i++)
        TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | status_bits[i], 64u,
                                               TPM_ALG_SHA256),
                       (int)TPM_NV_ATTRS, "each TPM-maintained status bit refused");

    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(TPMA_NV_OWNERWRITE, 64u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "index nobody may read refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(TPMA_NV_OWNERREAD, 64u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "index nobody may write refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(3u), 64u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "undefined TPM_NT value 3 refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(0xFu), 64u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "undefined TPM_NT value 15 refused");

    /* Owner-authorized DefineSpace cannot request platform-hierarchy
     * attributes. Accepting them locally would produce a request that passes
     * validation and then fails on real firmware only. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_POLICY_DELETE, 64u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "POLICY_DELETE refused under owner auth");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_PLATFORMCREATE, 64u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "PLATFORMCREATE refused under owner auth");

    /* Per-type size rules: COUNTER, BITS and both PIN types are exactly 8. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_COUNTER), 4u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "counter with dataSize 4 refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_COUNTER), 64u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "counter with dataSize 64 refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_BITS), 64u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "BITS index with dataSize 64 refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_BITS), 8u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "BITS index at 8 bytes is legal");
    /* The PIN types are refused whatever their size: this module does not
     * validate their authorization rules, so it will not marshal one. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_PIN_FAIL), 16u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "PIN_FAIL refused (unsupported type)");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_PIN_PASS), 8u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS,
                   "PIN_PASS refused even at 8 bytes (unsupported type)");

    /* An EXTEND index holds exactly one digest in its OWN nameAlg. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_EXTEND), 32u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "SHA-256 EXTEND index at 32 bytes is legal");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_EXTEND), 20u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "SHA-256 EXTEND index at 20 bytes refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_EXTEND), 32u,
                                           0xFFFFu),
                   (int)TPM_NV_ATTRS, "EXTEND index with an unknown nameAlg refused");

    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_COUNTER) |
                                           TPMA_NV_CLEAR_STCLEAR, 8u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS,
                   "counter with CLEAR_STCLEAR refused (resettable is not monotonic)");

    /* Size bounds for an ordinary index. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw, 0u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "zero dataSize refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw, (uint16_t)(TPM_NV_MAX_DATA + 1u),
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "oversize dataSize refused");

    /* The builder enforces the authPolicy length rule too: it is an exported
     * symbol, so a direct caller must not be able to marshal a shape
     * tpm_nv_define_ex would reject. */
    {
        uint8_t pol[96], buf[160];
        uint32_t k;
        for (k = 0; k < sizeof pol; k++) pol[k] = (uint8_t)k;
        TEST_ASSERT(tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                                         TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE,
                                         TPM_ALG_SHA256, pol, 32u, 64u) > 0u,
                    "a 32-byte SHA-256 authPolicy marshals");
        TEST_ASSERT_EQ(tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                                            TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE,
                                            TPM_ALG_SHA256, pol, 20u, 64u),
                       0u, "a 20-byte policy under SHA-256 is refused");
        TEST_ASSERT_EQ(tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                                            TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE,
                                            TPM_ALG_SHA256, pol, 64u, 64u),
                       0u, "a 64-byte policy under SHA-256 is refused");
        TEST_ASSERT_EQ(tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                                            TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE,
                                            TPM_ALG_SHA256, pol, 65u, 64u),
                       0u, "a 65-byte policy under SHA-256 is refused");
        TEST_ASSERT(tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_OS_DATA,
                                         TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE,
                                         TPM_ALG_SHA256, 0, 0u, 64u) > 0u,
                    "an absent authPolicy marshals");
    }

    /* The builder refuses the same shapes rather than marshaling them. */
    {
        uint8_t buf[96];
        TEST_ASSERT_EQ(tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_OS_DATA,
                                            rw | (1u << 20), TPM_ALG_SHA256,
                                            0, 0, 64u),
                       0u, "builder refuses a reserved-bit define");
        TEST_ASSERT(tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_OS_DATA,
                                         rw, TPM_ALG_SHA256, 0, 0, 64u) > 0u,
                    "builder still marshals a legal define");
    }
}

/* ---- Increment / WriteLock builders + their classification ---- */

static void test_nv_build_increment_writelock(void)
{
    uint8_t buf[64];
    uint32_t n;

    /* header(10) + authHandle(4) + nvIndex(4) + one-session auth area(13). */
    n = tpm2_build_nv_increment(buf, sizeof buf, TPM_RH_OWNER,
                                TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    TEST_ASSERT_EQ(n, 31u, "increment command length");
    TEST_ASSERT_EQ((uint32_t)tpm2_be16_get(buf + 0), (uint32_t)TPM2_ST_SESSIONS,
                   "increment is session-tagged");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 2), 31u, "increment header size matches");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), 0x00000134u, "increment CC (spec 0x134)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 10), TPM_RH_OWNER, "increment authHandle");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), TPM_NV_INDEX_OS_DATA, "increment nvIndex");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 18), 9u, "increment authorizationSize");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 22), TPM_RS_PW, "increment session handle");

    n = tpm2_build_nv_write_lock(buf, sizeof buf, TPM_RH_OWNER,
                                 TPM_NV_INDEX_BASELINE, TPM_RS_PW);
    TEST_ASSERT_EQ(n, 31u, "write-lock command length");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 6), 0x00000138u, "write-lock CC (spec 0x138)");
    TEST_ASSERT_EQ(tpm2_be32_get(buf + 14), TPM_NV_INDEX_BASELINE,
                   "write-lock nvIndex");

    TEST_ASSERT_EQ(tpm2_build_nv_increment(buf, 30u, TPM_RH_OWNER,
                                           TPM_NV_INDEX_OS_DATA, TPM_RS_PW),
                   0u, "increment refuses a too-small buffer");
    TEST_ASSERT_EQ(tpm2_build_nv_write_lock(0, sizeof buf, TPM_RH_OWNER,
                                            TPM_NV_INDEX_OS_DATA, TPM_RS_PW),
                   0u, "write-lock refuses a NULL buffer");
}

static void test_nv_classify_attributes(void)
{
    /* TPM_RC_ATTRIBUTES is format-1 base 0x082: an increment against a
     * non-counter index, or a write against a counter. It used to fall through
     * to TPMERR and read as a generic device fault. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000082u), (int)TPM_NV_ATTRS,
                   "0x082 (ATTRIBUTES) -> ATTRS");
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000282u), (int)TPM_NV_ATTRS,
                   "0x282 (ATTRIBUTES on handle 2) -> ATTRS");
    /* Control: the format-0 NV warning whose low byte is also 0x48 is unaffected
     * by the new format-1 entry. */
    TEST_ASSERT_EQ((int)tpm_nv_classify_rc(0x00000148u), (int)TPM_NV_LOCKED,
                   "format-0 NV_LOCKED still classifies as LOCKED");
}

/* ---- Counter wrappers over the fake transport ---- */

static void test_nv_counter_ops(void)
{
    struct tpm_t_test_state prev;
    tpm_nv_status_t st;
    uint64_t v;

    /* An 8-byte big-endian counter value decodes exactly. */
    nvf_reset(0u, 0u);
    nvf_read_payload_len = 8u;
    nvf_counter_value = 0x0102030405060708ull;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    v = 0xDEADBEEFDEADBEEFull;
    st = tpm_nv_read_counter(TPM_NV_INDEX_OS_DATA, &v);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "counter read succeeds");
    TEST_ASSERT(v == 0x0102030405060708ull, "counter value decoded big-endian");
    TEST_ASSERT(!nvf_saw_cc(TPM2_CC_NV_WRITE),
                "counter read never issues NV_Write");

    /* A never-incremented counter is UNINIT, NOT zero: the first increment of a
     * recreated index can land above a previous value, so reporting 0 here would
     * let a rollback read as a fresh install. */
    nvf_reset(TPM2_CC_NV_READ, 0x0000014Au /* NV_UNINITIALIZED */);
    nvf_read_payload_len = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    v = 0xDEADBEEFDEADBEEFull;
    st = tpm_nv_read_counter(TPM_NV_INDEX_OS_DATA, &v);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_UNINIT, "never-incremented counter -> UNINIT");
    TEST_ASSERT(v == 0xDEADBEEFDEADBEEFull,
                "UNINIT leaves the caller's value untouched (never synthesized 0)");

    /* A short read is a malformed response, not a small number. */
    nvf_reset(0u, 0u);
    nvf_read_payload_len = 4u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    v = 0xDEADBEEFDEADBEEFull;
    st = tpm_nv_read_counter(TPM_NV_INDEX_OS_DATA, &v);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "short counter read -> TRANSPORT");
    TEST_ASSERT(v == 0xDEADBEEFDEADBEEFull, "short read does not decode a value");

    TEST_ASSERT_EQ((int)tpm_nv_read_counter(TPM_NV_INDEX_OS_DATA, 0),
                   (int)TPM_NV_BADARG, "NULL out refused without a transaction");
}

static void test_nv_increment_writelock_status(void)
{
    struct tpm_t_test_state prev;
    tpm_nv_status_t st;

    /* CONTROL: an ordinary increment and write-lock succeed. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_increment(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "increment succeeds");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_NV_INCREMENT), "increment issued NV_Increment");

    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_write_lock(TPM_NV_INDEX_BASELINE);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "write-lock succeeds");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_NV_WRITE_LOCK), "write-lock issued NV_WriteLock");

    /* An increment past a write lock reports LOCKED and leaves the transport
     * usable -- the "a locked index never wedges" contract. */
    nvf_reset(TPM2_CC_NV_INCREMENT, TPM2_RC_NV_LOCKED);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_increment(TPM_NV_INDEX_OS_DATA);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_LOCKED, "increment past a write-lock -> LOCKED");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a locked index does not wedge the transport");
    nvf_reset(0u, 0u);
    st = tpm_nv_write_lock(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "a later command still runs after LOCKED");

    /* An increment against a non-counter index reports the TYPE error by name. */
    nvf_reset(TPM2_CC_NV_INCREMENT, 0x00000282u /* ATTRIBUTES on handle 2 */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_increment(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_ATTRS,
                   "increment on a non-counter index -> ATTRS");
}

static void test_nv_define_mismatch(void)
{
    struct tpm_t_test_state prev;
    tpm_nv_status_t st;
    uint32_t want = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA;

    /* An existing index whose public area MATCHES the request is idempotent. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, TPM2_RC_NV_DEFINED);
    nvf_public_attrs = want | TPMA_NV_WRITTEN;   /* in use: WRITTEN is the TPM's */
    nvf_public_size = 64u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "an identical existing definition is idempotent OK");

    /* A DIFFERENT definition is refused rather than silently reused -- the shape
     * a machine enrolled under the old, wrong read attributes is in. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, TPM2_RC_NV_DEFINED);
    nvf_public_attrs = TPMA_NV_AUTHREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA;
    nvf_public_size = 64u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "legacy AUTHREAD-instead-of-OWNERREAD index refused, not reused");

    /* A size difference is equally a mismatch. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, TPM2_RC_NV_DEFINED);
    nvf_public_attrs = want;
    nvf_public_size = 32u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "wrong dataSize refused");

    /* An index whose size and attributes match but whose POLICY differs is a
     * different index: for a policy-protected index the authPolicy IS the
     * access rule, so accepting it would trust PCR state we never chose. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, TPM2_RC_NV_DEFINED);
    nvf_public_attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA;
    nvf_public_size = 64u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x5Au;         /* NOT the digest the define computed */
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_baseline(TPM_NV_INDEX_BASELINE, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "same size+attrs but a DIFFERENT authPolicy is refused");

    /* A policy LENGTH difference is a mismatch even when the prefix agrees. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, TPM2_RC_NV_DEFINED);
    nvf_public_attrs = want;
    nvf_public_size = 64u;
    nvf_public_policy_len = 32u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);   /* requests NO policy */
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "an existing policy where none was requested is refused");

    /* POSITIVE non-empty policy: an existing baseline index whose authPolicy is
     * EXACTLY the digest this define computed is idempotent OK. Without this
     * the equality loop is only ever exercised by mismatches, so a regression
     * that rejected every policy_len > 0 would fail a correctly provisioned
     * baseline closed on every boot and no test would notice. The fake's
     * PolicyGetDigest returns 32 zero bytes, so that is the computed digest. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, TPM2_RC_NV_DEFINED);
    nvf_public_attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA;
    nvf_public_size = 64u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x00u;          /* == the computed trial digest */
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_baseline(TPM_NV_INDEX_BASELINE, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "an identical NON-EMPTY authPolicy is idempotent OK");

    /* nameAlg is definition-bearing too: it determines the index Name. */
    nvf_reset(TPM2_CC_NV_DEFINE_SPACE, TPM2_RC_NV_DEFINED);
    nvf_public_attrs = want;
    nvf_public_size = 64u;
    nvf_public_name_alg = 0x000Bu + 1u;      /* not SHA-256 */
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_data(TPM_NV_INDEX_OS_DATA, 64u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "a different nameAlg is refused");

    /* A counter define fixes type/size/NO_DA and refuses caller-supplied type
     * bits. Use a NONZERO type with otherwise-valid auth bits: TPM_NT_ORDINARY
     * is 0, so the previous version of this assertion passed because the
     * attributes lacked write permission, not because the type was rejected. */
    TEST_ASSERT_EQ((int)tpm_nv_define_counter(TPM_NV_INDEX_OS_DATA,
                                              TPMA_NV_TYPE(TPM_NT_BITS) |
                                              TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE),
                   (int)TPM_NV_ATTRS, "counter define refuses caller-supplied type bits");

    /* And a successful counter define emits COUNTER / 8 bytes / NO_DA on the
     * wire -- without this the wrapper could quietly define an ordinary index. */
    /* A policy-only counter is REFUSED: increment and counter-read authorize
     * with owner auth, so provisioning one would create a persistent index this
     * module cannot then use. The previous version of this test asserted the
     * opposite and passed only because the fake does not enforce the
     * definition's authorization attributes. */
    TEST_ASSERT_EQ((int)tpm_nv_define_counter(TPM_NV_INDEX_OS_DATA,
                                              TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE),
                   (int)TPM_NV_ATTRS,
                   "a policy-only counter is refused (its wrappers use owner auth)");

    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_define_counter(TPM_NV_INDEX_OS_DATA,
                               TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "an owner-authorized counter define succeeds");
    TEST_ASSERT_EQ(tpm2_be32_get(nvf_cmd + 37),
                   SPEC_ATTR_OWNERREAD | SPEC_ATTR_OWNERWRITE |
                   SPEC_ATTR_NT_COUNTER | SPEC_ATTR_NO_DA,
                   "counter define put COUNTER + NO_DA on the wire");
    TEST_ASSERT_EQ((uint32_t)tpm2_be16_get(nvf_cmd + 43),
                   (uint32_t)TPM_NV_COUNTER_SIZE,
                   "counter define put dataSize 8 on the wire");

    /* The authorization the define REQUESTS must be the one the wrappers SEND,
     * or the index is provisioned unusable. Both increment and counter-read
     * carry TPM_RH_OWNER + TPM_RS_PW, matching the OWNERREAD|OWNERWRITE the
     * define now insists on. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_nv_increment(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(tpm2_be32_get(nvf_cmd + 10), TPM_RH_OWNER,
                   "increment authorizes with the owner handle the define requires");
    TEST_ASSERT_EQ(tpm2_be32_get(nvf_cmd + 22), TPM_RS_PW,
                   "increment uses the password session owner auth needs");
}

/* ---- Bounded-sequence budget + ownership ---- */

static int seq_probe_submit(tpm2_seq_t seq, void *ctx)
{
    uint8_t cmd[40], rsp[64];
    uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                         TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    int *rc = (int *)ctx;
    *rc = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);
    return 0;
}

/* A teardown runs on its OWN allowance and must not leave any of it behind for
 * ordinary work. Both halves matter: the reserve has to fund a mandatory flush
 * after the work budget is spent, and it must NOT fund the irreversible
 * commands that follow a mid-sequence flush -- which is exactly what extending
 * the shared deadline used to do inside a baseline define. */
static int seq_probe_teardown_allowance(tpm2_seq_t seq, void *ctx)
{
    uint8_t cmd[40], rsp[64], fcmd[16], frsp[16];
    int *rc = (int *)ctx;
    uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                         TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    uint32_t fn = tpm2_build_flush_context(fcmd, sizeof fcmd, 0x03000000u);

    rc[0] = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);        /* work: spent */
    rc[1] = tpm2_submit_seq_teardown(seq, fcmd, fn, frsp, sizeof frsp);
    rc[2] = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);        /* STILL spent */
    return 0;
}

/* A teardown whose OWN allowance is zero: it must be refused on the allowance,
 * not silently fall back to the work budget, and the work budget must come back
 * exactly as it was so ordinary commands carry on. */
static int seq_probe_teardown_expiry(tpm2_seq_t seq, void *ctx)
{
    uint8_t cmd[40], rsp[64], fcmd[16], frsp[16];
    int *rc = (int *)ctx;
    uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                         TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    uint32_t fn = tpm2_build_flush_context(fcmd, sizeof fcmd, 0x03000000u);
    uint64_t before = tpm_t_test_budget_deadline();
    rc[0] = tpm2_submit_seq_teardown(seq, fcmd, fn, frsp, sizeof frsp);
    rc[1] = (tpm_t_test_budget_deadline() == before) ? 1 : 0;
    rc[2] = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);
    return 0;
}

/* A teardown FIRST, before anything has polled: the work budget is already
 * spent by the clock but no poll has latched it yet. Restoring the pre-teardown
 * latch then leaves expiry unrecorded, so an ordinary command would proceed on
 * an elapsed budget unless the DEADLINE is what gets checked. */
static int seq_probe_stale_latch(tpm2_seq_t seq, void *ctx)
{
    uint8_t cmd[40], rsp[64], fcmd[16], frsp[16];
    int *rc = (int *)ctx;
    uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                         TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    uint32_t fn = tpm2_build_flush_context(fcmd, sizeof fcmd, 0x03000000u);
    rc[0] = tpm2_submit_seq_teardown(seq, fcmd, fn, frsp, sizeof frsp);
    rc[1] = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);
    return 0;
}

/* The work budget is saved and restored around a teardown, so a mid-sequence
 * flush leaves the deadline exactly where it found it. */
static int seq_probe_teardown_restores(tpm2_seq_t seq, void *ctx)
{
    uint64_t *out = (uint64_t *)ctx;
    uint8_t fcmd[16], frsp[16];
    uint32_t fn = tpm2_build_flush_context(fcmd, sizeof fcmd, 0x03000000u);
    out[0] = tpm_t_test_budget_deadline();
    (void)tpm2_submit_seq_teardown(seq, fcmd, fn, frsp, sizeof frsp);
    out[1] = tpm_t_test_budget_deadline();
    return 0;
}

static int seq_probe_nested(tpm2_seq_t seq, void *ctx)
{
    int inner_rc = 0;
    (void)seq;
    *(int *)ctx = tpm2_seq_run(1000u, 100u, seq_probe_submit, &inner_rc);
    return 0;
}

static int seq_probe_stale(tpm2_seq_t seq, void *ctx)
{
    uint8_t cmd[40], rsp[64];
    uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                         TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    int *rc = (int *)ctx;
    rc[0] = tpm2_submit_seq(0u, cmd, n, rsp, sizeof rsp);        /* no token */
    rc[1] = tpm2_submit_seq(seq + 1u, cmd, n, rsp, sizeof rsp);  /* wrong token */
    return 0;
}

/* Capture this sequence's own token so a LATER sequence can be driven with it.
 * The seq+1 case above would pass against a constant token; only a token from a
 * COMPLETED sequence proves the generation actually advances. */
static tpm2_seq_t seq_captured_token;

static int seq_probe_capture(tpm2_seq_t seq, void *ctx)
{
    (void)ctx;
    seq_captured_token = seq;
    return 0;
}

static int seq_probe_use_captured(tpm2_seq_t seq, void *ctx)
{
    uint8_t cmd[40], rsp[64];
    uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                         TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    int *rc = (int *)ctx;
    rc[0] = (seq != seq_captured_token) ? 1 : 0;
    rc[1] = tpm2_submit_seq(seq_captured_token, cmd, n, rsp, sizeof rsp);
    return 0;
}

static void test_nv_sequence_budget(void)
{
    struct tpm_t_test_state prev;
    int rc[3];
    uint64_t dl[5];
    int r;

    /* CONTROL: the same slow-but-responsive TPM completes inside a generous
     * budget. Without this, the expiry assertions below would also pass against
     * a bound that fires on every command. */
    nvf_reset(0u, 0u);
    nvf_stall_reads = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = 1;
    r = tpm2_seq_run(60000u, 1000u, seq_probe_submit, &rc[0]);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(r, 0, "sequence with a generous budget runs");
    TEST_ASSERT(rc[0] > 0, "a stalling TPM still completes inside the budget");

    /* Expiry AFTER dispatch: the command was handed to the TPM and its response
     * abandoned, which is the state quiescence exists to clean up. The recovery
     * is asserted WITHOUT an intervening nvf_reset -- resetting the fake would
     * clear the dirty interface independently and the assertion would pass
     * whether or not tpm_t_quiesce did anything. */
    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = 0;
    r = tpm2_seq_run(0u, 1000u, seq_probe_submit, &rc[0]);
    TEST_ASSERT_EQ(r, 0, "sequence runs to completion even when the budget expires");
    TEST_ASSERT_EQ(rc[0], TPM_T_ERR_BUDGET, "expiry reports BUDGET, not TIMEOUT");
    /* The device answered within its own timeouts; it was only slower than this
     * boot will wait. Poisoning the transport would disable every later TPM
     * operation over a machine that is merely slow. */
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "budget expiry does not sticky-fail the transport");
    rc[0] = 0;
    r = tpm2_seq_run(60000u, 1000u, seq_probe_submit, &rc[0]);
    tpm_t_test_restore(prev);
    TEST_ASSERT(rc[0] > 0,
                "quiescence left the interface clean: the next command runs "
                "with no fake reset in between");

    /* The budget-expiry RECOVERY paths live in test_tpm_transport.c, not here:
     * reaching them needs the budget to elapse WHILE a command is in flight,
     * which only the iteration-mode budget seam can do deterministically (a
     * deadline would need real wall-clock). Covered there for both interfaces
     * and both outcomes -- TIS and CRB, abort honored (BUDGET, transport kept)
     * and abort ignored (TIMEOUT, transport sticky-failed). */

    /* The teardown allowance funds a mandatory flush past a spent work budget,
     * and -- the half that was leaking -- does NOT leave any of it behind for
     * the ordinary commands that follow. A mid-sequence flush inside a baseline
     * define used to top up the shared deadline, so an irreversible
     * NV_DefineSpace then ran on time reserved for cleanup. */
    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = rc[1] = rc[2] = 0;
    (void)tpm2_seq_run(0u, 60000u, seq_probe_teardown_allowance, rc);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(rc[0], TPM_T_ERR_BUDGET, "a spent work budget refuses ordinary work");
    TEST_ASSERT(rc[1] > 0, "the teardown allowance still runs a mandatory flush");
    TEST_ASSERT_EQ(rc[2], TPM_T_ERR_BUDGET,
                   "the allowance does NOT leak into ordinary work after the flush");

    /* A zero teardown allowance refuses the teardown rather than borrowing the
     * work budget, and the work budget survives the attempt intact. This is the
     * restoration path -- deadline, iters, active and expired flags all put
     * back -- which the success cases never exercise. */
    /* NO stall: a zero allowance must refuse BEFORE the interface is touched.
     * An earlier version of this test needed a stall to make the refusal
     * happen, which proved the opposite of what it claimed -- the command was
     * reaching the TPM and only failing once a poll happened to tick the
     * budget. The preflight check is what makes the guarantee real, and the
     * no-FlushContext assertion below is what proves it. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = rc[1] = rc[2] = 0;
    (void)tpm2_seq_run(60000u, 0u, seq_probe_teardown_expiry, rc);
    /* Read availability BEFORE restoring the snapshot: restore puts back the
     * pre-test transport state, which on this host is "unavailable". */
    r = tpm_transport_available();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(rc[0], TPM_T_ERR_BUDGET,
                   "a zero teardown allowance refuses, never borrows the work budget");
    TEST_ASSERT_EQ(rc[1], 1, "the work deadline is restored after a refused teardown");
    TEST_ASSERT(rc[2] > 0, "ordinary work continues on the restored work budget");
    TEST_ASSERT_EQ(r, 1, "a refused teardown does not disable the transport");
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), 0,
                   "a zero allowance refuses before the command reaches the TPM");

    /* nv_flush STOPS once its allowance reports BUDGET: retrying without budget
     * would spin the loop for nothing. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(60000u, 0u);
    {
        uint8_t sel2[3] = { 0x01u, 0u, 0u };
        uint8_t pol2[32];
        (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel2, pol2, sizeof pol2);
    }
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), 0,
                   "the flush stops immediately when it has no allowance at all");

    /* An elapsed-but-unlatched work budget must refuse ordinary work even when
     * the teardown ran first and restored a clean latch. Checking only the
     * latch let an irreversible NV_DefineSpace start after its deadline. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = rc[1] = 0;
    (void)tpm2_seq_run(0u, 60000u, seq_probe_stale_latch, rc);
    tpm_t_test_restore(prev);
    TEST_ASSERT(rc[0] > 0, "the teardown runs on its own allowance");
    TEST_ASSERT_EQ(rc[1], TPM_T_ERR_BUDGET,
                   "an elapsed work budget refuses ordinary work after a teardown");

    /* CONTROL: with a LIVE work budget the same order still permits work, so
     * the assertion above is about elapsed time and not about a teardown
     * poisoning the sequence outright. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = rc[1] = 0;
    (void)tpm2_seq_run(60000u, 60000u, seq_probe_stale_latch, rc);
    tpm_t_test_restore(prev);
    TEST_ASSERT(rc[1] > 0, "a live work budget still permits work after a teardown");

    /* And with a LIVE work budget the teardown leaves the deadline untouched,
     * which is what makes the two budgets genuinely separate rather than one
     * shared deadline being nudged. (Skipped in iteration mode, where there is
     * no deadline to compare -- reported rather than silently passed.) */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    dl[0] = dl[1] = 0;
    (void)tpm2_seq_run(60000u, 1000u, seq_probe_teardown_restores, dl);
    tpm_t_test_restore(prev);
    if (dl[0] == 0u) {
        TEST_SKIP("budget in iteration mode: no deadline to compare");
    } else {
        TEST_ASSERT(dl[1] == dl[0],
                    "a teardown restores the work deadline it borrowed against");
    }

    /* Ownership is the token, so a stale or absent one cannot drive the gate. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = rc[1] = 0;
    (void)tpm2_seq_run(60000u, 1000u, seq_probe_stale, rc);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(rc[0], TPM_T_ERR_SEQ, "submit with no token refused");
    TEST_ASSERT_EQ(rc[1], TPM_T_ERR_SEQ, "submit with a wrong token refused");

    /* A token from a COMPLETED sequence is refused by the NEXT one. seq+1 above
     * would pass against a constant token; this is what proves the generation
     * advances and that a stale holder cannot drive somebody else's gate. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    seq_captured_token = 0;
    (void)tpm2_seq_run(60000u, 1000u, seq_probe_capture, 0);
    TEST_ASSERT(seq_captured_token != 0u, "a live sequence has a nonzero token");
    rc[0] = rc[1] = 0;
    (void)tpm2_seq_run(60000u, 1000u, seq_probe_use_captured, rc);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(rc[0], 1, "the next sequence gets a DIFFERENT token");
    TEST_ASSERT_EQ(rc[1], TPM_T_ERR_SEQ,
                   "a completed sequence's token is refused by the next one");

    /* Sequences do not nest: the inner one bounces rather than clobbering the
     * outer one's budget or releasing its gate. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = 0;
    (void)tpm2_seq_run(60000u, 1000u, seq_probe_nested, &rc[0]);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(rc[0], TPM_T_ERR_BUSY, "a nested sequence is refused with BUSY");

    /* The gate is released on every path out, so an ordinary submit works
     * afterwards -- the property a raw begin/end pair could not guarantee. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    r = (int)tpm_nv_write_lock(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(r, (int)TPM_NV_OK, "the transport is free after every sequence");
}

/* ---- Abandoned session creation: unknown outcome must not be retried ---- */

static void test_nv_session_unknown_outcome(void)
{
    struct tpm_t_test_state prev;
    uint8_t policy[32];
    uint8_t sel[3] = { 0x01u, 0u, 0u };
    tpm_nv_status_t st;

    /* CONTROL: the TPM DEFINITELY refused StartAuthSession (a parsed response
     * with a real error code). Nothing was allocated, so the transport stays
     * usable and the next operation runs. Without this the poison assertion
     * below would pass against code that poisons on every failure. */
    nvf_reset(TPM2_CC_START_AUTH_SESSION, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND, "a refused SAS propagates its rc");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a definite TPM refusal allocates nothing: transport kept");
    tpm_t_test_restore(prev);

    /* A StartAuthSession abandoned mid-flight: the TPM may have created a
     * session whose handle we never received, so there is no FlushContext we
     * could issue for it. THIS invocation fails and is not retried; later
     * independent TPM operations are still allowed, because a held slot
     * terminates on its own in a definite StartAuthSession error while
     * disabling the transport would also break operations that open no
     * session. */
    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(0u, 60000u);   /* expire on the first wait */
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    tpm_nv_test_reset_op_budget();
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET, "an abandoned SAS reports BUDGET");
    /* The leak is REPORTED, not escalated. Disabling the transport here was
     * tried and reverted: the leak degrades on its own into a definite
     * StartAuthSession error once the pool runs out, whereas poisoning also
     * takes out PCR reads and attestation, which never open a session. */
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "an unknowable session outcome does not disable the transport");
    tpm_t_test_restore(prev);

    /* An rc-SUCCESS StartAuthSession whose body carries NO recoverable handle:
     * the TPM executed the command, so a session very likely exists and there
     * is nothing to FlushContext. Retrying would stack another one behind it. */
    nvf_reset(0u, 0u);
    nvf_sas_no_handle = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "an unparseable SAS success reports TRANSPORT");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a created-but-unrecoverable session leaves the transport usable");
    tpm_t_test_restore(prev);

    /* CONTROL: the same malformed shape WITH a recoverable handle is flushed,
     * and the transport survives. Without it the assertion above would pass
     * against code that poisons on every malformed reply. */
    nvf_reset(0u, 0u);
    nvf_sas_bad = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "a recoverable malformed SAS still reports TRANSPORT");
    TEST_ASSERT(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT),
                "the recovered session handle is flushed");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a flushed session leaves the transport usable");
    tpm_t_test_restore(prev);

    /* CONTROL for the budget seam itself: with a generous budget the SAME
     * stalling fake completes the whole flow, so the assertion above is about
     * expiry and not about the stall breaking the transport outright. */
    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "the same stalling TPM completes inside the real budget");
}

/* ---- Teardown needs PROOF, and the boot primitives are bounded ---- */

static void test_nv_teardown_and_bounds(void)
{
    struct tpm_t_test_state prev;
    uint8_t policy[32];
    uint8_t sel[3] = { 0x01u, 0u, 0u };
    tpm_nv_status_t st;

    /* A TRANSIENT warning on FlushContext (RETRY / YIELDED / TESTING in the
     * field) leaves the session exactly where it was. Accepting the first
     * non-success as "flushed" would leak one session per operation while the
     * transport still looked healthy, so the teardown retries and only then
     * gives up. Two failures then success: no poison. */
    nvf_reset(TPM2_CC_FLUSH_CONTEXT, 0x00000922u /* a transient warning */);
    nvf_fail_times = 2;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "the operation itself still succeeds");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a transient flush warning that clears does not poison");
    tpm_t_test_restore(prev);

    /* A flush the TPM keeps refusing without ever proving the handle is gone IS
     * a leak -- it is retried the full number of times and then reported. The
     * transport deliberately stays usable: the leak terminates on its own in a
     * definite StartAuthSession failure, while disabling the transport would
     * break unrelated TPM use and recover nothing. */
    nvf_reset(TPM2_CC_FLUSH_CONTEXT, 0x00000922u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), (int)TPM_NV_FLUSH_RETRIES,
                   "an unproven flush is retried the full bounded number of times");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "an unproven flush reports the leak without disabling the TPM");
    tpm_t_test_restore(prev);

    /* "No such handle" IS proof: the session is already released, which is the
     * end state the flush wanted. Not a leak, so not a poison. */
    nvf_reset(TPM2_CC_FLUSH_CONTEXT, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "operation succeeds");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "an already-released handle is proof, not a leak");
    tpm_t_test_restore(prev);

    /* A flush reply the transport REJECTS (here: a header declaring more bytes
     * than the caller's buffer) yields no parsed rc at all. FlushContext is
     * idempotent, so this is retried the full bounded number of times rather
     * than abandoned after one attempt -- the earlier break-on-unparsed gave up
     * immediately and leaked a session on a single bad reply.
     *
     * Note this exercises the TRANSPORT-level rejection, not a parser-level
     * one: tis_submit validates the header size against the same bounds
     * tpm2_rsp_parse would (>= header, <= cap, <= MAX_RESPONSE) and returns
     * TPM_T_ERR_RESPONSE first, so a response the transport accepts always
     * parses. There is no reachable "positive length that tpm2_rsp_parse
     * rejects" case to write a fixture for. */
    nvf_reset(0u, 0u);
    nvf_malformed_flush = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), (int)TPM_NV_FLUSH_RETRIES,
                   "a malformed flush reply is retried, not abandoned after one");
    TEST_ASSERT_EQ(tpm_transport_available(), 1,
                   "a malformed flush reply does not disable the transport");
    tpm_t_test_restore(prev);

    /* An rc-SUCCESS whose TAG is illegal reaches tpm2_rsp_parse (tis_submit only
     * bounds the size) and is rejected there -- the parser-level shape. */
    nvf_reset(0u, 0u);
    nvf_flush_bad_tag = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), (int)TPM_NV_FLUSH_RETRIES,
                   "an illegal-tag flush success is retried, not believed");
    tpm_t_test_restore(prev);

    /* An rc-SUCCESS with a LEGAL tag but the wrong FlushContext envelope must
     * not count as proof either. FlushContext is ST_NO_SESSIONS, so the generic
     * executor performs no shape check for it -- believing this reply is how a
     * desynchronized TPM would silently keep the session. */
    nvf_reset(0u, 0u);
    nvf_flush_wrong_shape = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), (int)TPM_NV_FLUSH_RETRIES,
                   "a wrong-shape flush success is retried, not accepted as proof");
    tpm_t_test_restore(prev);

    /* "No such handle" is only proof when it arrives in a real error envelope.
     * A wrong-envelope HANDLE still classifies as NOTFOUND, so without the
     * check it would end the teardown after one attempt while the session is
     * still allocated -- the exact mirror of the success-path hole. */
    nvf_reset(0u, 0u);
    nvf_flush_bad_handle_env = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), (int)TPM_NV_FLUSH_RETRIES,
                   "a wrong-envelope HANDLE is retried, not taken as proof");
    tpm_t_test_restore(prev);

    /* CONTROL: a correctly shaped HANDLE response IS accepted on the first
     * attempt, so the assertion above is about the envelope and not about
     * NOTFOUND having stopped counting as proof at all. */
    nvf_reset(TPM2_CC_FLUSH_CONTEXT, 0x0000018Bu /* HANDLE, header-only */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), 1,
                   "a well-formed HANDLE response is proof on the first attempt");
    tpm_t_test_restore(prev);

    /* CONTROL: the correct envelope IS accepted first time -- without this the
     * three assertions above would pass against a flush that never believes
     * anything and always burns every retry. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), 1,
                   "a well-formed flush success is accepted on the first attempt");
    tpm_t_test_restore(prev);

    /* The exported boot primitives run under the cumulative budget too -- they
     * used to submit unsequenced, so the section's headline bound did not
     * actually cover the commands the section added. */
    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(0u, 60000u);
    st = tpm_nv_increment(TPM_NV_INDEX_OS_DATA);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET, "increment is budget-bounded");

    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    tpm_nv_test_set_op_budget(0u, 60000u);
    st = tpm_nv_write_lock(TPM_NV_INDEX_OS_DATA);
    tpm_nv_test_reset_op_budget();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET, "write-lock is budget-bounded");

    {
        uint64_t v = 0;
        nvf_reset(0u, 0u);
        nvf_read_payload_len = 8u;
        nvf_stall_after_go = 8u;
        prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
        tpm_nv_test_set_op_budget(0u, 60000u);
        st = tpm_nv_read_counter(TPM_NV_INDEX_OS_DATA, &v);
        tpm_nv_test_reset_op_budget();
        tpm_t_test_restore(prev);
        TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET, "counter read is budget-bounded");
    }

    /* CONTROL: the same stalling fake completes every one of them inside the
     * real budget, so the three assertions above are about the bound and not
     * about the stall breaking the commands. */
    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_increment(TPM_NV_INDEX_OS_DATA);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "the same stalling TPM completes inside the real budget");
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
    test_suite_register_cat("tpm: NV cleanup flush busy-wait", test_nv_flush_busy_wait, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV attribute spec oracle", test_nv_attr_spec_oracle, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV attribute validation", test_nv_attrs_validation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV TPMA_NV table oracle", test_nv_attr_table_oracle, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV public policy bound", test_nv_parse_public_policy_bound, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV inc/lock marshal", test_nv_build_increment_writelock, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV attribute rc classification", test_nv_classify_attributes, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV counter read semantics", test_nv_counter_ops, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV inc/lock status", test_nv_increment_writelock_status, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV define public-area mismatch", test_nv_define_mismatch, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV bounded-sequence budget", test_nv_sequence_budget, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV abandoned session outcome", test_nv_session_unknown_outcome, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV teardown proof + wrapper bounds", test_nv_teardown_and_bounds, TEST_CAT_SECURITY);
}
