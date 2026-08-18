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
#include "kernel/crypto/sha256.h"
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
static uint32_t nvf_special_sessions;  /* TPMS_AUTH_RESPONSE count an
                                        * UndefineSpaceSpecial success returns */
static int      nvf_public_name_mode;  /* TPM2B_NAME an NV_ReadPublic reports:
                                        * 0 = empty (what the fake served before
                                        * anything consumed the field), 1 = the
                                        * real Name of the reported public area,
                                        * 2 = a well-formed but WRONG Name */
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
    } else if (rc == TPM2_RC_SUCCESS &&
               cc == TPM2_CC_NV_UNDEFINE_SPACE_SPECIAL) {
        /* TWO authorizations in, TWO TPMS_AUTH_RESPONSE structures out. A
         * conforming TPM answers with exactly as many sessions as the command
         * carried, so this is the shape a real successful special-delete has --
         * and the shape the one-session validator rejects. nvf_special_sessions
         * lets a test serve the WRONG count to prove the check is live. */
        uint32_t nsess = nvf_special_sessions;
        uint32_t area = 5u * nsess;   /* nonceTPM(2,0) + attrs(1) + hmac(2,0) */
        uint32_t k;
        tpm2_be16_put(nvf_rsp + 0, TPM2_ST_SESSIONS);
        tpm2_be32_put(nvf_rsp + 2, 10u + 4u + area);
        tpm2_be32_put(nvf_rsp + 6, rc);
        tpm2_be32_put(nvf_rsp + 10, 0u);   /* parameterSize: no parameters */
        for (k = 0; k < area; k++)
            nvf_rsp[14 + k] = 0u;
        nvf_rsp_len = 14u + area;
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
        if (nvf_public_name_mode == 0) {
            tpm2_be16_put(nvf_rsp + 26 + pl, 0u);  /* TPM2B_NAME (empty) */
            nvf_rsp_len = 28u + pl;
        } else {
            /* Serve the REAL Name for the public area just reported, so the
             * identity path has something to consume. Mode 2 corrupts one byte
             * AFTER computing it, which is the shape a substituted index
             * presents: a well-formed name that is not this index's. */
            struct tpm_nv_public np;
            uint8_t nm[TPM_NV_NAME_MAX];
            uint32_t k;
            np.data_size = nvf_public_size;
            np.attrs = nvf_public_attrs;
            np.name_alg = nvf_public_name_alg ? nvf_public_name_alg : TPM_ALG_SHA256;
            np.policy_len = (uint16_t)pl;
            for (k = 0; k < sizeof np.auth_policy; k++)
                np.auth_policy[k] = (k < pl) ? nvf_public_policy_fill : 0u;
            (void)tpm2_nv_name_compute(idx, &np, nm, sizeof nm);
            if (nvf_public_name_mode == 2)
                nm[TPM_NV_NAME_MAX - 1u] ^= 0xFFu;
            tpm2_be16_put(nvf_rsp + 26 + pl, (uint16_t)TPM_NV_NAME_MAX);
            for (k = 0; k < TPM_NV_NAME_MAX; k++)
                nvf_rsp[28 + pl + k] = nm[k];
            tpm2_be32_put(nvf_rsp + 2, 14u + pubsize + TPM_NV_NAME_MAX);
            nvf_rsp_len = 28u + pl + TPM_NV_NAME_MAX;
        }
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
    nvf_public_name_mode = 0;
    nvf_special_sessions = 2u;
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

    /* Unaccounted bytes inside TPM2B_NV_PUBLIC are refused. Nothing READS the
     * space between dataSize and the trailing TPM2B_NAME, so a size-only check
     * would let a padded public area pass both parsers and then be accepted by
     * the definition-identity comparison as an exact match. */
    memset(&pub, 0x5A, sizeof pub);
    before = pub;
    n = nvp_build_public(rsp, TPM_NV_INDEX_OS_DATA, 0u, 0u);
    {
        uint16_t sz = 0;
        uint32_t at = 0;
        /* Grow pubsize (and the response) by one byte the fixed TPMS_NV_PUBLIC
         * layout does not account for, leaving policy_len at 0. */
        tpm2_be16_put(rsp + 10, (uint16_t)(tpm2_be16_get(rsp + 10) + 1u));
        tpm2_be32_put(rsp + 2, n + 1u);
        rsp[n] = 0x00u;
        TEST_ASSERT_EQ(tpm2_parse_nv_read_public(rsp, n + 1u, TPM_NV_INDEX_OS_DATA,
                                                 &sz, &at), -1,
                       "the strict parser refuses a padded TPMS_NV_PUBLIC");
        TEST_ASSERT_EQ(tpm2_parse_nv_public_full(rsp, n + 1u, TPM_NV_INDEX_OS_DATA,
                                                 &pub), -1,
                       "the full parser refuses a padded TPMS_NV_PUBLIC");
        TEST_ASSERT_EQ(memcmp(&pub, &before, sizeof pub), 0,
                       "and that refusal is transactional too");
    }

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
    /* WRITEALL is meaningless on an index written by Increment/SetBits, and a
     * conforming TPM refuses the combination -- so the validator does too
     * rather than letting it pass locally and fail on firmware. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_COUNTER) |
                                           TPMA_NV_WRITEALL, 8u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "WRITEALL on a counter refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_TYPE(TPM_NT_BITS) |
                                           TPMA_NV_WRITEALL, 8u, TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "WRITEALL on a BITS index refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw | TPMA_NV_WRITEALL, 64u, TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "WRITEALL on an ORDINARY index is still legal");
    TEST_ASSERT_EQ((int)tpm_nv_define_counter(TPM_NV_INDEX_OS_DATA,
                                              TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE |
                                              TPMA_NV_WRITEALL),
                   (int)TPM_NV_ATTRS, "a counter define carrying WRITEALL is refused");

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
    /* An index LARGER than one transfer is still definable: the read/write
     * wrappers chunk by offset, so capping the definition at TPM_NV_MAX_DATA
     * would make a chunk-readable index impossible to provision. The two limits
     * are separate on purpose. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw, (uint16_t)(TPM_NV_MAX_DATA + 1u),
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "an index bigger than one transfer is definable");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw, (uint16_t)TPM_NV_MAX_INDEX_SIZE,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_OK, "an index at the definition limit is legal");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(rw, (uint16_t)(TPM_NV_MAX_INDEX_SIZE + 1u),
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "past the definition limit is refused");

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

/* Repeated teardowns under a SPENT work budget must all run: the allowance is
 * independent of the work phase, which is the whole point of separating them.
 * This is asserted at the sequence seam rather than through tpm_nv_read/flush,
 * because a spent work budget refuses the StartAuthSession too, so the wrapper
 * path can never reach a flush in that state -- the property is real on
 * hardware, where budgets elapse mid-operation, and only reachable here. */
static int seq_probe_teardown_repeats(tpm2_seq_t seq, void *ctx)
{
    uint8_t fcmd[16], frsp[16];
    int *rc = (int *)ctx;
    uint32_t fn = tpm2_build_flush_context(fcmd, sizeof fcmd, 0x03000000u);
    int i;
    for (i = 0; i < 3; i++)
        rc[i] = tpm2_submit_seq_teardown(seq, fcmd, fn, frsp, sizeof frsp);
    return 0;
}

/* Once the callback returns the sequence stops accepting work: a holder that
 * kept its token past the callback is refused rather than driving a gate that
 * is being torn down. Asserted from INSIDE by leaving the closing flag set --
 * the drain wait in tpm2_seq_run is what makes the shutdown safe, and this is
 * the observable half of it. */
static tpm2_seq_t seq_escaped_token;

static int seq_probe_capture_only(tpm2_seq_t seq, void *ctx)
{
    (void)ctx;
    seq_escaped_token = seq;
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

struct seq_dispatch_probe_ctx {
    int rc;
    int dispatched;
};

/* Same NV_Increment shape as seq_probe_submit above, plus a dispatch-flag
 * capture immediately after the submit -- while it is still valid, since
 * nothing else calls tpm2_submit_seq in between. */
static int seq_probe_submit_dispatch(tpm2_seq_t seq, void *ctx)
{
    uint8_t cmd[40], rsp[64];
    uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                         TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
    struct seq_dispatch_probe_ctx *out = (struct seq_dispatch_probe_ctx *)ctx;
    out->rc = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);
    out->dispatched = tpm2_seq_last_submit_dispatched();
    return 0;
}

/* tpm2_seq_last_submit_dispatched() is the primitive section 24 built to
 * distinguish "the cumulative budget was already spent before this command
 * touched the interface" (genuinely safe to retry) from "the command was
 * written and its response abandoned" (completion unknown). Both cases return
 * the SAME TPM_T_ERR_BUDGET from tpm2_submit_seq, so a caller cannot tell them
 * apart without this. Tested here, once, at the transport layer, rather than
 * timing-dependently through every handle-allocating call site above it. */
static void test_seq_last_submit_dispatched(void)
{
    struct tpm_t_test_state prev;
    struct seq_dispatch_probe_ctx out;
    int r;

    /* Pre-dispatch refusal: work_ms=0 means the cumulative budget is already
     * spent before the FIRST submit in the sequence ever touches the
     * interface -- tpm_t_budget_spent() refuses inside tpm2_submit_seq before
     * tpm_t_submit_txn is ever called. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    out.rc = 0; out.dispatched = -1;
    r = tpm2_seq_run(0u, 1000u, seq_probe_submit_dispatch, &out);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(r, 0,
                   "the sequence itself runs; the CALLBACK reports the "
                   "refusal, not tpm2_seq_run's own return");
    TEST_ASSERT_EQ(out.rc, TPM_T_ERR_BUDGET,
                   "submit refuses with BUDGET before touching the interface");
    TEST_ASSERT_EQ(out.dispatched, 0,
                   "a pre-dispatch refusal reports dispatched=0: nothing was "
                   "ever submitted");

    /* CONTROL: a normal, generously-budgeted submit that actually reaches the
     * fake interface must report dispatched=1. Without this, dispatched=0
     * would trivially "pass" the assertion above for the wrong reason (a
     * getter that always returns 0 would satisfy it too). */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    out.rc = -1; out.dispatched = -1;
    r = tpm2_seq_run(60000u, 1000u, seq_probe_submit_dispatch, &out);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(r, 0, "the sequence runs to completion");
    TEST_ASSERT(out.rc >= 0,
                "CONTROL: a generous budget lets the command actually submit "
                "and succeed");
    TEST_ASSERT_EQ(out.dispatched, 1,
                   "CONTROL: a command that reaches the interface reports "
                   "dispatched=1");

    /* A THIRD case -- the cumulative budget expiring WHILE WAITING for the
     * Ready handshake, before GO is ever written -- is what the fix in this
     * round actually targets (moving the dispatch write from "before
     * tpm_t_submit_txn" to "immediately before the GO/START register write"),
     * but it cannot be reproduced deterministically through this fixture: the
     * KERNEL_TESTS fast-timeout seam (tpm_t_test_install) forces EVERY local
     * wait to exactly TPM_T_FAST_TEST_ITERS (64) ticks regardless of test
     * budget, while the smallest whole-millisecond cumulative budget
     * (work_ms=1) already arms >= 50,000 ticks (TPM_T_NOFREQ_ITERS_PER_MS) --
     * so the local 64-tick cap always wins first and the wait ends in a plain
     * TIMEOUT before the cumulative check ever gets a chance to fire. work_ms=0
     * reaches only the pre-check case already covered above. The fix is
     * instead verified by INSPECTION: the write is the immediate next
     * statement before the unconditional MMIO/port register write in both
     * tis_submit and crb_submit (src/kernel/tpm_transport.c), with no
     * intervening statement that could itself fail -- so "flag set" and "GO/
     * START written" are the same instant by construction, not merely by
     * proximity. */
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

    /* Every teardown gets the allowance, however spent the work budget is --
     * and they SHARE it rather than each taking a fresh reserve. Sharing is what
     * keeps the advertised bound true: re-arming per attempt turned a 2s reserve
     * into 3 x 2s plus an abort each, ~17s for one operation. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = rc[1] = rc[2] = 0;
    (void)tpm2_seq_run(0u, 60000u, seq_probe_teardown_repeats, rc);
    tpm_t_test_restore(prev);
    TEST_ASSERT(rc[0] > 0 && rc[1] > 0 && rc[2] > 0,
                "a spent work budget does not cut the teardown's own retries");
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), 3,
                   "all three teardowns reached the TPM");

    /* Once that ONE allowance is spent, further teardowns are refused: the
     * retries debit a shared deadline instead of resetting it. */
    nvf_reset(0u, 0u);
    nvf_stall_after_go = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    rc[0] = rc[1] = rc[2] = 0;
    (void)tpm2_seq_run(0u, 0u, seq_probe_teardown_repeats, rc);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(rc[0], TPM_T_ERR_BUDGET, "a spent allowance refuses the first teardown");
    TEST_ASSERT_EQ(rc[1], TPM_T_ERR_BUDGET, "and does not renew for the second");
    TEST_ASSERT_EQ(rc[2], TPM_T_ERR_BUDGET, "or the third");
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_FLUSH_CONTEXT), 0,
                   "no teardown reached the TPM on a spent allowance");

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

    /* A token that ESCAPED its sequence is refused once that sequence has
     * ended -- the shutdown path clears the token, so a late holder cannot
     * submit against a released gate. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    seq_escaped_token = 0;
    (void)tpm2_seq_run(60000u, 1000u, seq_probe_capture_only, 0);
    {
        uint8_t cmd[40], rsp[64];
        uint32_t n = tpm2_build_nv_increment(cmd, sizeof cmd, TPM_RH_OWNER,
                                             TPM_NV_INDEX_OS_DATA, TPM_RS_PW);
        r = tpm2_submit_seq(seq_escaped_token, cmd, n, rsp, sizeof rsp);
    }
    tpm_t_test_restore(prev);
    TEST_ASSERT(seq_escaped_token != 0u, "the probe captured a live token");
    TEST_ASSERT_EQ(r, TPM_T_ERR_SEQ,
                   "a token used after its sequence ended cannot drive the gate");

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

/* ---- Index identity: TPM2 Name computation ----
 *
 * The oracle is INDEPENDENT of the module's marshaller: the test hand-writes
 * the TPMS_NV_PUBLIC byte layout and hashes it with sha256() directly, so a
 * mistake in tpm2_nv_name_compute's field order or widths shows up as a
 * mismatch rather than being echoed back. sha256() itself is pinned by the
 * FIPS 180-4 vectors in test_sha256.c. */

static void nvid_fill_public(struct tpm_nv_public *p, uint16_t policy_len,
                             uint8_t fill)
{
    uint32_t i;
    p->data_size = 412u;
    p->attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA;
    p->name_alg = TPM_ALG_SHA256;
    p->policy_len = policy_len;
    for (i = 0; i < sizeof p->auth_policy; i++)
        p->auth_policy[i] = (i < policy_len) ? fill : 0u;
}

/* Arm the fake to report a public area matching nvid_fill_public, with a REAL
 * Name, so the live identity paths have something consistent to consume. */
static void nvid_arm_public(uint32_t attrs)
{
    nvf_reset(0u, 0u);
    nvf_public_attrs = attrs;
    nvf_public_size = 412u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x5Au;
    nvf_public_name_mode = 1;
}


static void test_nv_name_compute(void)
{
    struct tpm_nv_public pub, other;
    uint8_t name[TPM_NV_NAME_MAX], name2[TPM_NV_NAME_MAX];
    uint8_t oracle[64], expect[TPM_NV_NAME_MAX];
    uint32_t off = 0, i;
    int n;

    nvid_fill_public(&pub, 32u, 0x5Au);

    n = tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &pub, name, sizeof name);
    TEST_ASSERT_EQ(n, (int)TPM_NV_NAME_MAX, "Name is nameAlg(2) + one SHA-256 digest");
    TEST_ASSERT_EQ((int)tpm2_be16_get(name), (int)TPM_ALG_SHA256,
                   "Name is prefixed with the literal nameAlg");

    /* Independent oracle: hand-marshal TPMS_NV_PUBLIC and hash it here.
     * nvIndex(4) nameAlg(2) attributes(4) authPolicy TPM2B(2+N) dataSize(2),
     * and deliberately NOT the TPM2B_NV_PUBLIC size prefix. */
    tpm2_be32_put(oracle + off, TPM_NV_INDEX_BASELINE); off += 4u;
    tpm2_be16_put(oracle + off, TPM_ALG_SHA256); off += 2u;
    tpm2_be32_put(oracle + off, pub.attrs); off += 4u;
    tpm2_be16_put(oracle + off, 32u); off += 2u;
    for (i = 0; i < 32u; i++) oracle[off + i] = 0x5Au;
    off += 32u;
    tpm2_be16_put(oracle + off, 412u); off += 2u;
    TEST_ASSERT_EQ((int)off, 46, "hand-marshalled TPMS_NV_PUBLIC is 46 bytes here");
    tpm2_be16_put(expect, TPM_ALG_SHA256);
    sha256(oracle, off, expect + 2u);
    TEST_ASSERT_EQ(memcmp(name, expect, TPM_NV_NAME_MAX), 0,
                   "computed Name matches the independently marshalled oracle");

    /* Differential: the Name must move when ANY definition field moves. A Name
     * that ignored a field would still pass the oracle above if the test and
     * the module happened to share the same omission, so each field is
     * perturbed separately. */
    n = tpm2_nv_name_compute(TPM_NV_INDEX_OS_DATA, &pub, name2, sizeof name2);
    TEST_ASSERT_EQ(n, (int)TPM_NV_NAME_MAX, "second Name computes");
    TEST_ASSERT(memcmp(name, name2, TPM_NV_NAME_MAX) != 0,
                "a different nvIndex yields a different Name");

    other = pub; other.attrs |= TPMA_NV_WRITEDEFINE;
    (void)tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &other, name2, sizeof name2);
    TEST_ASSERT(memcmp(name, name2, TPM_NV_NAME_MAX) != 0,
                "different attributes yield a different Name");

    other = pub; other.data_size = 413u;
    (void)tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &other, name2, sizeof name2);
    TEST_ASSERT(memcmp(name, name2, TPM_NV_NAME_MAX) != 0,
                "a different dataSize yields a different Name");

    other = pub; other.auth_policy[0] ^= 0xFFu;
    (void)tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &other, name2, sizeof name2);
    TEST_ASSERT(memcmp(name, name2, TPM_NV_NAME_MAX) != 0,
                "a different authPolicy yields a different Name");

    other = pub; other.policy_len = 0u;
    (void)tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &other, name2, sizeof name2);
    TEST_ASSERT(memcmp(name, name2, TPM_NV_NAME_MAX) != 0,
                "an absent authPolicy yields a different Name");

    /* TPMA_NV_WRITTEN is INSIDE the hashed attributes, so the Name changes the
     * first time the index is written. This is the fact the enrolled contract
     * is normalized against; if it ever stopped being true the identity design
     * would be over-strict rather than wrong, so it is pinned here. */
    other = pub; other.attrs |= TPMA_NV_WRITTEN;
    (void)tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &other, name2, sizeof name2);
    TEST_ASSERT(memcmp(name, name2, TPM_NV_NAME_MAX) != 0,
                "TPMA_NV_WRITTEN is part of the Name: it changes on first write");

    /* Refusals, each a control against the helper simply always succeeding. */
    other = pub; other.name_alg = 0x000Cu;   /* SHA-384: not computed here */
    TEST_ASSERT_EQ(tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &other, name2,
                                        sizeof name2), -1,
                   "a nameAlg this module cannot hash is refused, not mis-hashed");
    TEST_ASSERT_EQ(tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &pub, name2,
                                        TPM_NV_NAME_MAX - 1u), -1,
                   "a short output buffer is refused");
    TEST_ASSERT_EQ(tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, 0, name2,
                                        sizeof name2), -1,
                   "a NULL public area is refused");
}

/* ---- Enrolled identity contract ---- */

static void test_nv_identity_contract(void)
{
    struct tpm_nv_public pub, live;
    struct tpm_nv_identity id;

    nvid_fill_public(&pub, 32u, 0x5Au);
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "identity builds from a public area");
    TEST_ASSERT_EQ((int)(id.attrs & TPMA_NV_STATUS_MASK), 0,
                   "enrolled attrs are normalized: no TPM-maintained status bits");
    TEST_ASSERT_EQ((int)id.expect_written, 1, "expect_written is recorded explicitly");
    TEST_ASSERT_EQ((int)id.nv_index, (int)TPM_NV_INDEX_BASELINE, "handle recorded");

    /* The whole point of normalizing: the contract must survive the index being
     * written and locked, because those bits are not part of the definition. */
    live = pub;
    live.attrs |= TPMA_NV_WRITTEN | TPMA_NV_WRITELOCKED | TPMA_NV_READLOCKED;
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &live), (int)TPM_NV_OK,
                   "a written and locked index still matches its enrolled contract");

    /* Enrolling FROM a written index must produce the same normalized contract,
     * so enrollment time does not silently change the rule. */
    {
        struct tpm_nv_identity id2;
        TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE,
                                                        &live, 1, &id2),
                       (int)TPM_NV_OK, "identity builds from a written index");
        TEST_ASSERT_EQ((int)id2.attrs, (int)id.attrs,
                       "normalization makes enrollment order irrelevant");
    }

    /* Every definition field is load-bearing: each disagreement is a MISMATCH. */
    live = pub; live.attrs |= TPMA_NV_WRITEDEFINE;
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &live), (int)TPM_NV_MISMATCH,
                   "a changed definition attribute is a mismatch");
    live = pub; live.data_size = 413u;
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &live), (int)TPM_NV_MISMATCH,
                   "a changed dataSize is a mismatch");
    live = pub; live.name_alg = 0x000Cu;
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &live), (int)TPM_NV_MISMATCH,
                   "a changed nameAlg is a mismatch");
    live = pub; live.auth_policy[7] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &live), (int)TPM_NV_MISMATCH,
                   "a changed authPolicy is a mismatch");
    live = pub; live.policy_len = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &live), (int)TPM_NV_MISMATCH,
                   "a removed authPolicy is a mismatch");

    /* Control: without this the mismatch assertions above would all pass
     * against a comparator that refuses everything. */
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &pub), (int)TPM_NV_OK,
                   "control: the unchanged public area MATCHES");
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(0, &pub), (int)TPM_NV_BADARG,
                   "NULL contract is BADARG, not a silent match");
}

/* ---- Lifecycle classification ---- */

static void nvlc_good(struct tpm_nv_lifecycle_obs *o)
{
    /* Zero FIRST, so a field added to the observation struct later cannot be
     * read indeterminate by a test that clears its companion flag -- which is
     * exactly what happened when counter_required was added and this fixture
     * was not. */
    memset(o, 0, sizeof *o);
    o->enrolled = 1;
    o->index_present = 1;
    o->identity_ok = 1;
    o->name_ok = 1;
    o->written = 1;
    o->expect_written = 1;
    o->counter_required = 1;
    o->counter_known = 1;
    o->counter_value = 42u;
    o->enrolled_counter = 42u;
}

static void test_nv_lifecycle_classify(void)
{
    struct tpm_nv_lifecycle_obs o;

    /* Control FIRST: every refusal below is worthless without proof that the
     * healthy anchor is ACCEPTED. */
    nvlc_good(&o);
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o), (int)TPM_NV_LIFECYCLE_ACCEPT,
                   "control: a healthy enrolled anchor is ACCEPTED");
    nvlc_good(&o); o.counter_value = 43u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o), (int)TPM_NV_LIFECYCLE_ACCEPT,
                   "control: an ADVANCED counter is accepted, not read as tampering");

    /* The undefine/redefine attack: identity and Name both still match, because
     * a byte-identical public area produces a byte-identical Name. Only the
     * cleared WRITTEN bit betrays it. */
    nvlc_good(&o); o.written = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_RECREATED,
                   "an enrolled-written anchor reading back UNWRITTEN was recreated");

    nvlc_good(&o); o.identity_ok = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_IDENTITY,
                   "a public area that left the contract is an identity refusal");
    nvlc_good(&o); o.name_ok = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_IDENTITY,
                   "a TPM contradicting its own Name is an identity refusal");

    nvlc_good(&o); o.counter_value = 41u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_ROLLBACK,
                   "a counter below the enrolled value is a rollback refusal");

    /* The ambiguous case is reported ambiguous. It must NOT resolve to ACCEPT
     * (which would let a deletion launder into a fresh install) and must not
     * silently claim a benign TPM clear. */
    nvlc_good(&o); o.index_present = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_RECOVERY_REQUIRED,
                   "an enrolled anchor whose index is GONE requires authorized recovery");

    /* Ordering: identity is judged before the recreation signal, so a
     * substituted index is not mislabelled as a recreation of the enrolled one. */
    nvlc_good(&o); o.identity_ok = 0; o.written = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_IDENTITY,
                   "identity outranks the recreation signal when both fire");
    /* And absence outranks everything: nothing else can be observed. */
    nvlc_good(&o); o.index_present = 0; o.identity_ok = 0; o.counter_value = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_RECOVERY_REQUIRED,
                   "an absent index cannot be judged on identity it does not have");

    /* Never-enrolled is a provisioning state, not a verdict about an anchor. */
    nvlc_good(&o); o.enrolled = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_UNENROLLED,
                   "nothing enrolled is UNENROLLED, not ACCEPT");
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(0),
                   (int)TPM_NV_LIFECYCLE_UNENROLLED,
                   "a NULL observation asserts nothing about an anchor");

    /* An anchor enrolled as NOT-yet-written is legitimately unwritten. */
    nvlc_good(&o); o.expect_written = 0; o.written = 0;
    o.counter_required = 0; o.counter_known = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o), (int)TPM_NV_LIFECYCLE_ACCEPT,
                   "an anchor enrolled unwritten is accepted while still unwritten");
}

/* ---- Two indexes, separate contracts ---- */

static void test_nv_two_index_separation(void)
{
    uint8_t a[96], b[96];
    uint32_t na, nb;

    /* The invariant is that the two anchors are DISTINCT indexes on the wire.
     * A later change collapsing them into one shared counter fails here rather
     * than silently making each consumer's invariant unenforceable. */
    TEST_ASSERT(TPM_NV_INDEX_BASELINE_GEN != TPM_NV_INDEX_AB_SEQ,
                "baseline generation and A/B update sequence are separate handles");
    TEST_ASSERT(TPM_NV_INDEX_AB_FLOOR != TPM_NV_INDEX_AB_SEQ,
                "the A/B floor RECORD is a separate index from its sequence counter");

    na = tpm2_build_nv_define(a, sizeof a, TPM_NV_INDEX_BASELINE_GEN,
                              TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE |
                              TPMA_NV_NO_DA | TPMA_NV_TYPE(TPM_NT_COUNTER),
                              TPM_ALG_SHA256, 0, 0, (uint16_t)TPM_NV_COUNTER_SIZE);
    nb = tpm2_build_nv_define(b, sizeof b, TPM_NV_INDEX_AB_SEQ,
                              TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE |
                              TPMA_NV_NO_DA | TPMA_NV_TYPE(TPM_NT_COUNTER),
                              TPM_ALG_SHA256, 0, 0, (uint16_t)TPM_NV_COUNTER_SIZE);
    TEST_ASSERT(na > 0u && nb > 0u, "both counter defines marshal");
    TEST_ASSERT_EQ((int)na, (int)nb, "the two counter defines are the same shape");
    /* nvIndex sits in the TPMS_NV_PUBLIC: header(10) + authHandle(4)
     * + authArea(13) + authValue(2) + nvPublic size(2) = offset 31. */
    TEST_ASSERT(tpm2_be32_get(a + 31) != tpm2_be32_get(b + 31),
                "the marshalled commands name DIFFERENT indexes");
    TEST_ASSERT_EQ((int)tpm2_be32_get(a + 31), (int)TPM_NV_INDEX_BASELINE_GEN,
                   "the generation counter define names the generation handle");

    /* The floor RECORD is an ordinary data index, never a counter: a TPM
     * counter moves by exactly one per NV_Increment, so it cannot represent a
     * security version that jumps. */
    nb = tpm2_build_nv_define(b, sizeof b, TPM_NV_INDEX_AB_FLOOR,
                              TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA,
                              TPM_ALG_SHA256, 0, 0, (uint16_t)TPM_NV_AB_FLOOR_SIZE);
    TEST_ASSERT(nb > 0u, "the floor record define marshals as an ordinary index");
    TEST_ASSERT_EQ((int)TPMA_NV_GET_TYPE(tpm2_be32_get(b + 31 + 6u)),
                   (int)TPM_NT_ORDINARY,
                   "the floor record is TPM_NT_ORDINARY, not a counter");
    TEST_ASSERT_EQ((int)tpm2_be16_get(b + 31 + 12u), (int)TPM_NV_AB_FLOOR_SIZE,
                   "the floor record is defined at its declared size");
}

/* ---- Platform-authorized define and POLICY_DELETE admission ---- */

static void test_nv_platform_define_attrs(void)
{
    uint32_t base = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA;
    uint8_t buf[96], policy[32];
    uint32_t n, i;

    for (i = 0; i < 32u; i++) policy[i] = (uint8_t)i;

    /* The owner path must stay unable to reach either attribute. That is the
     * boundary: a shared entry point with a mode flag is one wrong argument
     * away from admitting them under owner auth, where firmware refuses them. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(base | TPMA_NV_POLICY_DELETE, 96u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "owner validator still refuses POLICY_DELETE");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid(base | TPMA_NV_PLATFORMCREATE, 96u,
                                           TPM_ALG_SHA256),
                   (int)TPM_NV_ATTRS, "owner validator still refuses PLATFORMCREATE");
    TEST_ASSERT_EQ((int)tpm2_build_nv_define(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                                             base | TPMA_NV_POLICY_DELETE,
                                             TPM_ALG_SHA256, policy, 32u, 96u),
                   0, "the owner define BUILDER refuses POLICY_DELETE outright");

    /* Platform path: admitted, but only in a shape that can actually be used. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(base | TPMA_NV_PLATFORMCREATE |
                                                    TPMA_NV_POLICY_DELETE, 96u,
                                                    TPM_ALG_SHA256, 1),
                   (int)TPM_NV_OK,
                   "POLICY_DELETE is admitted under platform auth WITH a policy");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(base | TPMA_NV_PLATFORMCREATE |
                                                    TPMA_NV_POLICY_DELETE, 96u,
                                                    TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS,
                   "POLICY_DELETE with NO authPolicy would be undeletable: refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(base | TPMA_NV_POLICY_DELETE,
                                                    96u, TPM_ALG_SHA256, 1),
                   (int)TPM_NV_ATTRS,
                   "a platform define must state PLATFORMCREATE");
    /* Control: the platform validator is not simply permissive. It inherits
     * every shared rule, so a reserved bit and a bad counter size still fail. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(base | TPMA_NV_PLATFORMCREATE |
                                                    TPMA_NV_RESERVED_MASK, 96u,
                                                    TPM_ALG_SHA256, 1),
                   (int)TPM_NV_ATTRS,
                   "the platform validator still refuses reserved bits");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(base | TPMA_NV_PLATFORMCREATE |
                                                    TPMA_NV_TYPE(TPM_NT_COUNTER),
                                                    7u, TPM_ALG_SHA256, 1),
                   (int)TPM_NV_ATTRS,
                   "the platform validator still enforces the counter size");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(base | TPMA_NV_PLATFORMCREATE,
                                                    96u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_OK,
                   "control: a plain platform define with no POLICY_DELETE is legal");

    /* And the wire: the ONLY difference from the owner define is the
     * authHandle, which is what makes the platform hierarchy the authorizer. */
    n = tpm2_build_nv_define_platform(buf, sizeof buf, TPM_NV_INDEX_BASELINE,
                                      base | TPMA_NV_PLATFORMCREATE |
                                      TPMA_NV_POLICY_DELETE,
                                      TPM_ALG_SHA256, policy, 32u, 96u);
    TEST_ASSERT(n > 0u, "the platform define marshals");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6), (int)TPM2_CC_NV_DEFINE_SPACE,
                   "platform define is still NV_DefineSpace");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 10), (int)TPM_RH_PLATFORM,
                   "platform define authorizes with TPM_RH_PLATFORM");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 31), (int)TPM_NV_INDEX_BASELINE,
                   "platform define names the requested index");
}

/* ---- NV_UndefineSpaceSpecial and PolicyCommandCode marshalling ---- */

static void test_nv_undefine_special_marshal(void)
{
    uint8_t buf[96];
    uint32_t n, area1;
    const uint32_t sess = 0x03000000u;

    n = tpm2_build_nv_undefine_special(buf, sizeof buf, TPM_NV_INDEX_BASELINE, sess);
    TEST_ASSERT(n > 0u, "UndefineSpaceSpecial marshals");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 0), (int)TPM2_ST_SESSIONS,
                   "session-tagged");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 2), (int)n,
                   "declared size equals the marshalled length");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6),
                   (int)TPM2_CC_NV_UNDEFINE_SPACE_SPECIAL, "command code");
    /* Handle ORDER is normative: nvIndex (authorized by its own policy) then
     * the platform hierarchy. Swapped, each secret authorizes the wrong
     * object -- and the TPM would refuse, so this cannot be caught late. */
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 10), (int)TPM_NV_INDEX_BASELINE,
                   "handle 1 is the index being deleted");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 14), (int)TPM_RH_PLATFORM,
                   "handle 2 is the platform hierarchy");
    area1 = 4u + 2u + 1u + 2u;
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 18), (int)(area1 * 2u),
                   "authorizationSize covers BOTH authorization areas");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 22), (int)sess,
                   "the first authorization is the index policy session");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 22 + area1), (int)TPM_RS_PW,
                   "the second authorization is the platform password session");
    TEST_ASSERT_EQ((int)n, (int)(22u + area1 * 2u),
                   "no parameters follow the two authorization areas");

    /* Refusals: a shape that could only ever be rejected by the TPM is
     * rejected locally instead. */
    TEST_ASSERT_EQ((int)tpm2_build_nv_undefine_special(buf, sizeof buf,
                                                       TPM_NV_INDEX_BASELINE,
                                                       TPM_RS_PW),
                   0, "a password handle cannot satisfy an index authPolicy");
    TEST_ASSERT_EQ((int)tpm2_build_nv_undefine_special(buf, sizeof buf,
                                                       TPM_NV_INDEX_BASELINE, 0u),
                   0, "a zero policy session is refused");
    TEST_ASSERT_EQ((int)tpm2_build_nv_undefine_special(buf, 8u,
                                                       TPM_NV_INDEX_BASELINE, sess),
                   0, "a too-small buffer is refused");
}

static void test_nv_policy_command_code_marshal(void)
{
    uint8_t buf[32];
    uint32_t n;
    const uint32_t sess = 0x03000001u;

    n = tpm2_build_policy_command_code(buf, sizeof buf, sess,
                                       TPM2_CC_NV_UNDEFINE_SPACE_SPECIAL);
    TEST_ASSERT_EQ((int)n, 18, "PolicyCommandCode is header(10) + session(4) + code(4)");
    /* A policy ASSERTION authorizes nothing, it only updates the session's
     * policyDigest, so it carries no authorization area. */
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 0), (int)TPM2_ST_NO_SESSIONS,
                   "PolicyCommandCode carries no authorization area");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 2), (int)n, "declared size");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6), (int)TPM2_CC_POLICY_COMMAND_CODE,
                   "command code");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 10), (int)sess, "policySession handle");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 14),
                   (int)TPM2_CC_NV_UNDEFINE_SPACE_SPECIAL,
                   "the restricted command code is the delete command");
    TEST_ASSERT_EQ((int)tpm2_build_policy_command_code(buf, sizeof buf, 0u,
                                                       TPM2_CC_NV_READ),
                   0, "a zero policy session is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_command_code(buf, 4u, sess,
                                                       TPM2_CC_NV_READ),
                   0, "a too-small buffer is refused");
}

/* ---- Multi-session response validation ---- */

static void test_nv_auth_response_sessions(void)
{
    uint8_t rsp[64];
    uint32_t off = 0, one;

    /* Two minimal TPMS_AUTH_RESPONSE structures: nonceTPM(2,0) +
     * sessionAttributes(1) + hmac(2,0) = 5 bytes each. */
    memset(rsp, 0, sizeof rsp);
    one = 5u;
    off = 2u * one;

    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, off, 0u, 2u), 1,
                   "two well-formed sessions validate at n_sessions 2");
    /* The regression this exists for: the one-session validator rejects a
     * legitimate two-authorization success as malformed. */
    TEST_ASSERT_EQ(tpm_session_auth_response_ok(rsp, off, 0u), 0,
                   "the one-session validator rejects a two-session response");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, one, 0u, 2u), 0,
                   "one session where two were authorized is rejected");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, off + 1u, 0u, 2u), 0,
                   "a trailing byte after two sessions is rejected");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, off, 0u, 3u), 0,
                   "three sessions where two exist is rejected");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, off, 0u, 0u), 0,
                   "n_sessions 0 is a caller error, not a vacuous pass");
    /* Control: the single-session path is unchanged. */
    TEST_ASSERT_EQ(tpm_session_auth_response_ok(rsp, one, 0u), 1,
                   "control: one well-formed session still validates");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, one, 0u, 1u), 1,
                   "control: n_ok at 1 agrees with the one-session validator");
}

/* ---- Live identity path through the fake transport ---- */

static void test_nv_live_identity(void)
{
    struct tpm_t_test_state prev;
    struct tpm_nv_public pub;
    struct tpm_nv_identity id;
    tpm_nv_status_t st;
    int name_ok = -1;

    /* The fake now serves the REAL Name of the public area it reports, which is
     * what makes the identity path testable at all: it served an EMPTY name for
     * as long as nothing consumed the field. */
    nvf_reset(0u, 0u);
    nvf_public_attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                       TPMA_NV_WRITTEN;
    nvf_public_size = 412u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x5Au;
    nvf_public_name_mode = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_read_identity(TPM_NV_INDEX_BASELINE, &pub, &name_ok);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "read_identity succeeds");
    TEST_ASSERT_EQ(name_ok, 1, "the reported Name is consistent with the public area");
    TEST_ASSERT_EQ((int)pub.data_size, 412, "the public area came back");

    /* A well-formed but WRONG Name: the shape a substituted index presents. */
    nvf_reset(0u, 0u);
    nvf_public_attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                       TPMA_NV_WRITTEN;
    nvf_public_size = 412u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x5Au;
    nvf_public_name_mode = 2;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_read_identity(TPM_NV_INDEX_BASELINE, &pub, &name_ok);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "the read itself still succeeds");
    TEST_ASSERT_EQ(name_ok, 0, "a Name that does not match the public area FAILS");

    /* An EMPTY name is a failed identity check, never a pass -- exactly the
     * value the fake served before this section, so a regression to ignoring
     * the field shows up here. */
    nvf_reset(0u, 0u);
    nvf_public_attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA;
    nvf_public_size = 412u;
    nvf_public_name_mode = 0;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_read_identity(TPM_NV_INDEX_BASELINE, &pub, &name_ok);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "the read succeeds with an empty name");
    TEST_ASSERT_EQ(name_ok, 0, "an EMPTY reported Name is not a pass");

    /* The gate: verify against an enrolled contract WITHOUT reading contents. */
    nvid_fill_public(&pub, 32u, 0x5Au);
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "enroll the contract");

    nvf_reset(0u, 0u);
    nvf_public_attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                       TPMA_NV_WRITTEN;
    nvf_public_size = 412u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x5Au;
    nvf_public_name_mode = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_identity(&id, 0);
    tpm_t_test_restore(prev);
    /* Control: without this every refusal below passes against a gate that
     * refuses everything. The live index is WRITTEN and the enrolled contract
     * was taken from an unwritten one, which must NOT matter. */
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "control: the enrolled index verifies even once written");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_READ), 0,
                   "the identity gate reads NO contents: ordering is the point");

    /* A different definition at the same handle is a MISMATCH, and the handle
     * number was never the thing being trusted. */
    nvf_reset(0u, 0u);
    nvf_public_attrs = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA;
    nvf_public_size = 412u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x5Au;
    nvf_public_name_mode = 1;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_identity(&id, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "a redefined index answering the same handle is refused");

    /* A self-inconsistent TPM is a mismatch even when the definition agrees. */
    nvf_reset(0u, 0u);
    nvf_public_attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA;
    nvf_public_size = 412u;
    nvf_public_policy_len = 32u;
    nvf_public_policy_fill = 0x5Au;
    nvf_public_name_mode = 2;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_identity(&id, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "a TPM whose Name contradicts its public area is refused");

    /* A missing index surfaces as NOTFOUND so the caller can reach the
     * ambiguous recovery state rather than reading it as a definition change. */
    nvf_reset(TPM2_CC_NV_READ_PUBLIC, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_identity(&id, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND,
                   "an absent index is NOTFOUND, not MISMATCH");
    /* The standalone gate enforces the SAME lifecycle contract as the atomic
     * path. If it did not, a caller would get OK here and a refusal there for
     * the same index, and this gate's documented meaning of OK would be false. */
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_identity(&id, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "the standalone gate also refuses a recreated unwritten anchor");

    TEST_ASSERT_EQ((int)tpm_nv_verify_identity(0, 0), (int)TPM_NV_BADARG,
                   "a NULL contract is BADARG");
}

/* ---- Delete-policy digest through the fake ---- */

static void test_nv_delete_policy_digest(void)
{
    struct tpm_t_test_state prev;
    uint8_t digest[32];
    tpm_nv_status_t st;

    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_delete_policy_digest(digest, sizeof digest);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "the delete policy digest computes");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_POLICY_COMMAND_CODE), 1,
                   "the trial session asserts PolicyCommandCode");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_POLICY_PCR), 0,
                   "a delete policy is NOT a PCR policy");
    /* The session-leak discipline is the same single cleanup as the PCR flow,
     * and a trial session must be flushed even on the success path. */
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), 1,
                   "the trial session is flushed on the success path");

    /* A failure AFTER the session exists must still flush it. */
    nvf_reset(TPM2_CC_POLICY_COMMAND_CODE, 0x0000099Du /* POLICY_FAIL */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_delete_policy_digest(digest, sizeof digest);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_AUTH, "the policy failure is classified");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_FLUSH_CONTEXT), 1,
                   "the session is flushed on the failure path too");

    /* What the policy IS: command-scoped. PolicyCommandCode is the only
     * assertion in it, so the session restricts the authorization to the delete
     * and nothing else. */
    nvf_reset(0u, 0u);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    (void)tpm_nv_delete_policy_digest(digest, sizeof digest);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(nvf_cc_count(TPM2_CC_POLICY_COMMAND_CODE), 1,
                   "the policy asserts the command-code restriction exactly once");

    /* What the policy is NOT: authentication. PolicyCommandCode carries no
     * secret, so the digest is reproducible by anyone -- two independent
     * computations agree, which is the property an authenticated policy would
     * NOT have. This assertion exists so nobody reads the delete policy as a
     * trust boundary; making deletion unreachable from the OS path needs a
     * signed assertion, tracked as open work in the section. */
    {
        uint8_t again[32];
        uint32_t i;
        int same = 1;
        nvf_reset(0u, 0u);
        prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
        (void)tpm_nv_delete_policy_digest(again, sizeof again);
        tpm_t_test_restore(prev);
        for (i = 0; i < sizeof again; i++)
            if (again[i] != digest[i]) { same = 0; break; }
        TEST_ASSERT_EQ(same, 1,
                       "the delete policy is reproducible: it scopes, it does NOT authenticate");
    }

    TEST_ASSERT_EQ((int)tpm_nv_delete_policy_digest(digest, 31u),
                   (int)TPM_NV_BADARG, "a short output buffer is refused");
    TEST_ASSERT_EQ((int)tpm_nv_delete_policy_digest(0, 32u),
                   (int)TPM_NV_BADARG, "a NULL output is refused");
}

/* ---- End-to-end: the two-session delete actually EXECUTES ----
 *
 * The builder and the response validator were tested separately, which left the
 * one thing that matters untested: whether the command survives the executor.
 * A wrong session count keeps every isolated test green while every real
 * firmware success is rejected as TPM_NV_TRANSPORT. */

static void test_nv_undefine_special_exec(void)
{
    struct tpm_t_test_state prev;
    uint8_t cmd[96], rsp[128];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st = TPM_NV_TRANSPORT;
    int r;

    n = tpm2_build_nv_undefine_special(cmd, sizeof cmd, TPM_NV_INDEX_BASELINE,
                                       0x03000000u);
    TEST_ASSERT(n > 0u, "the special delete marshals");

    /* The real shape: two authorizations in, two response sessions out. */
    nvf_reset(0u, 0u);
    nvf_special_sessions = 2u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    r = tpm_session_cmd_exec_seq_n(0, cmd, n, rsp, sizeof rsp, &rlen, &st, 0, 2u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(r, 0, "a two-session success EXECUTES through the n-session path");
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "and classifies OK, not TRANSPORT");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_UNDEFINE_SPACE_SPECIAL), 1,
                   "the special delete reached the transport");

    /* The regression this whole seam exists for: the SAME response through the
     * one-session executor must be refused, proving the count is load-bearing
     * rather than decorative. */
    nvf_reset(0u, 0u);
    nvf_special_sessions = 2u;
    st = TPM_NV_OK;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    r = tpm_session_cmd_exec_seq(0, cmd, n, rsp, sizeof rsp, &rlen, &st, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(r, -1, "the same response through the ONE-session path is refused");
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "a session-count mismatch is a malformed envelope");

    /* And the converse: a TPM that answers with only ONE session where two were
     * authorized is refused, so the check is not merely counting upward. */
    nvf_reset(0u, 0u);
    nvf_special_sessions = 1u;
    st = TPM_NV_OK;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    r = tpm_session_cmd_exec_seq_n(0, cmd, n, rsp, sizeof rsp, &rlen, &st, 0, 2u);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ(r, -1, "one session where two were authorized is refused");
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT, "and classified as malformed");

    /* n_sessions 0 is a caller error at the executor, not a vacuous success. */
    st = TPM_NV_OK;
    r = tpm_session_cmd_exec_seq_n(0, cmd, n, rsp, sizeof rsp, &rlen, &st, 0, 0u);
    TEST_ASSERT_EQ(r, -1, "n_sessions 0 is refused by the executor");
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BADARG, "and reported as BADARG");
}

/* ---- Verified-operation atomicity ----
 *
 * tpm_nv_verify_identity's answer expires when its sequence closes, so a read
 * issued afterwards is a check-then-use race. tpm_nv_verify_then exists to hold
 * ONE sequence across the verification and the operation; these assertions pin
 * that the op runs only after a passing verification, and never after a failing
 * one. */

static int vt_calls;
static int vt_saw_read_public_first;

static uint32_t vt_seen_index;

static tpm_nv_status_t vt_op(tpm2_seq_t seq, uint32_t nv_index,
                             const struct tpm_nv_public *pub, void *ctx)
{
    uint8_t cmd[64], rsp[128];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    (void)ctx;
    vt_calls++;
    vt_seen_index = nv_index;
    /* The verification must already have happened, in this same sequence. */
    vt_saw_read_public_first = nvf_saw_cc(TPM2_CC_NV_READ_PUBLIC);
    if (!pub)
        return TPM_NV_BADARG;
    /* Operate on the handle the API VERIFIED, never one of our own -- an op
     * naming its own index is operating on something this call never checked. */
    n = tpm2_build_nv_read(cmd, sizeof cmd, TPM_RH_OWNER, nv_index,
                           TPM_RS_PW, 4u, 0u);
    if (n == 0u)
        return TPM_NV_BADARG;
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        return st;
    return TPM_NV_OK;
}

static void test_nv_verify_then_atomicity(void)
{
    struct tpm_t_test_state prev;
    struct tpm_nv_public pub;
    struct tpm_nv_identity id;
    tpm_nv_status_t st;

    nvid_fill_public(&pub, 32u, 0x5Au);
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "enroll the contract");

    /* Control FIRST: the op RUNS on a verified index, and runs after the
     * ReadPublic rather than before it. */
    vt_calls = 0; vt_saw_read_public_first = 0;
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                    TPMA_NV_WRITTEN);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_then(&id, vt_op, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "control: the verified op runs and succeeds");
    TEST_ASSERT_EQ(vt_calls, 1, "control: the op was invoked exactly once");
    TEST_ASSERT_EQ(vt_saw_read_public_first, 1,
                   "the identity was established BEFORE the op ran");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_READ), 1,
                   "the op's content read happened in the same sequence");
    TEST_ASSERT_EQ((int)vt_seen_index, (int)TPM_NV_INDEX_BASELINE,
                   "the op is handed the VERIFIED handle, so it need not name one");

    /* A definition that no longer matches: the op must NEVER run. */
    vt_calls = 0;
    nvid_arm_public(TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_then(&id, vt_op, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "a redefined index is refused");
    TEST_ASSERT_EQ(vt_calls, 0, "the op did NOT run on a failed identity");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_READ), 0, "and no content was read");

    /* A self-inconsistent Name: same refusal, op still never runs. */
    vt_calls = 0;
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA);
    nvf_public_name_mode = 2;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_then(&id, vt_op, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "an inconsistent Name is refused");
    TEST_ASSERT_EQ(vt_calls, 0, "the op did NOT run on a failed Name check");

    /* An absent index: refused before the op, and reported as NOTFOUND so the
     * caller can reach the ambiguous recovery state. */
    vt_calls = 0;
    nvf_reset(TPM2_CC_NV_READ_PUBLIC, 0x0000018Bu /* HANDLE */);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_then(&id, vt_op, 0);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND, "an absent index is NOTFOUND");
    TEST_ASSERT_EQ(vt_calls, 0, "the op did NOT run on an absent index");

    TEST_ASSERT_EQ((int)tpm_nv_verify_then(0, vt_op, 0), (int)TPM_NV_BADARG,
                   "a NULL contract is BADARG");
    TEST_ASSERT_EQ((int)tpm_nv_verify_then(&id, 0, 0), (int)TPM_NV_BADARG,
                   "a NULL op is BADARG");
}

/* ---- Lifecycle precedence: every pair of simultaneously-true conditions ----
 *
 * Each refusal was pinned on its own, which leaves the ORDER between them free
 * to change silently. Order is not cosmetic here: it decides which remediation
 * the caller reaches, and the wrong one can route an attack to the recovery
 * path. */

static void test_nv_lifecycle_precedence(void)
{
    struct tpm_nv_lifecycle_obs o;

    /* Absence outranks everything: nothing else can even be observed. */
    nvlc_good(&o); o.index_present = 0; o.name_ok = 0; o.identity_ok = 0;
    o.written = 0; o.counter_value = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_RECOVERY_REQUIRED,
                   "absence outranks identity, recreation and rollback together");
    nvlc_good(&o); o.index_present = 0; o.written = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_RECOVERY_REQUIRED,
                   "absence outranks recreation");
    nvlc_good(&o); o.index_present = 0; o.counter_value = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_RECOVERY_REQUIRED,
                   "absence outranks rollback");

    /* Identity outranks recreation and rollback: a substituted index must not
     * be reported as a recreation of the enrolled one, which would name the
     * wrong anchor in the refusal. */
    nvlc_good(&o); o.identity_ok = 0; o.counter_value = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_IDENTITY,
                   "identity outranks rollback");
    nvlc_good(&o); o.name_ok = 0; o.written = 0; o.counter_value = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_IDENTITY,
                   "a failed Name outranks recreation and rollback");

    /* Recreation outranks rollback: a recreated counter has no comparable
     * value, so reporting a rollback would be describing arithmetic on a
     * number that does not mean what the comparison assumes. */
    nvlc_good(&o); o.written = 0; o.counter_value = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_RECREATED,
                   "recreation outranks rollback");
    nvlc_good(&o); o.written = 0; o.counter_required = 1; o.counter_known = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_RECREATED,
                   "recreation outranks an incomplete counter read");
    /* Control for the line above: with the recreation signal removed, the SAME
     * observation must reach REFUSE_INCOMPLETE. Without it the precedence
     * assertion would pass even if the incomplete rule did not exist. */
    nvlc_good(&o); o.counter_required = 1; o.counter_known = 0;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_INCOMPLETE,
                   "control: the incomplete condition really is present");

    /* Unenrolled outranks every refusal: with no contract there is nothing to
     * refuse against, and inventing one would refuse a first provisioning. */
    nvlc_good(&o); o.enrolled = 0; o.identity_ok = 0; o.written = 0;
    o.index_present = 0; o.counter_value = 0u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_UNENROLLED,
                   "unenrolled outranks every refusal");
}

/* ---- Fail-closed on missing anti-rollback evidence ---- */

static void test_nv_lifecycle_incomplete_counter(void)
{
    struct tpm_nv_lifecycle_obs o;

    /* The fail-open shape: a counter-backed anchor whose counter could not be
     * read used to fall through to ACCEPT, so an attacker who merely makes the
     * read FAIL got the same verdict as a healthy machine. */
    nvlc_good(&o); o.counter_required = 1; o.counter_known = 0;
    o.enrolled_counter = 42u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_INCOMPLETE,
                   "a required counter that could not be read is REFUSED");

    /* Control: the same anchor WITH the counter read is accepted, so the
     * refusal is about the missing evidence and not about the flag existing. */
    nvlc_good(&o); o.counter_required = 1; o.counter_known = 1;
    o.counter_value = 42u; o.enrolled_counter = 42u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o), (int)TPM_NV_LIFECYCLE_ACCEPT,
                   "control: a required counter that WAS read is accepted");
    nvlc_good(&o); o.counter_required = 1; o.counter_known = 1;
    o.counter_value = 41u; o.enrolled_counter = 42u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o),
                   (int)TPM_NV_LIFECYCLE_REFUSE_ROLLBACK,
                   "a required counter below enrollment is still a rollback");

    /* An anchor that is genuinely NOT counter-backed is unaffected: the two
     * flags are separate facts precisely so this case stays ACCEPT. */
    nvlc_good(&o); o.counter_required = 0; o.counter_known = 0;
    o.enrolled_counter = 42u;
    TEST_ASSERT_EQ((int)tpm_nv_lifecycle_classify(&o), (int)TPM_NV_LIFECYCLE_ACCEPT,
                   "an anchor with no counter is not refused for lacking one");
}

/* ---- Platform validator inherits the WHOLE shared rule set ---- */

static void test_nv_platform_validator_inheritance(void)
{
    const uint32_t pc = TPMA_NV_PLATFORMCREATE;
    const uint32_t rw = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE;

    /* Each of these is refused by the shared validator, and the platform
     * validator must not become a way around any of them. Proving only the
     * reserved-bit and counter-size rules left the rest free to drift. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc | TPMA_NV_WRITTEN, 96u,
                                                    TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: TPM-maintained status bits refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(TPMA_NV_OWNERREAD | pc, 96u,
                                                    TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: a write-authorization bit is required");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(TPMA_NV_OWNERWRITE | pc, 96u,
                                                    TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: a read-authorization bit is required");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc |
                                                    TPMA_NV_TYPE(TPM_NT_PIN_PASS),
                                                    8u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: PIN index types refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc | TPMA_NV_WRITEALL |
                                                    TPMA_NV_TYPE(TPM_NT_COUNTER),
                                                    8u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: WRITEALL on a counter refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc | TPMA_NV_CLEAR_STCLEAR |
                                                    TPMA_NV_TYPE(TPM_NT_COUNTER),
                                                    8u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS,
                   "inherits: CLEAR_STCLEAR on a counter refused (resettable anchor)");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc |
                                                    TPMA_NV_TYPE(TPM_NT_EXTEND),
                                                    31u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: an EXTEND index is one digest");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc, 0u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: a zero-size ordinary index refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc, 4097u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: an oversized ordinary index refused");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc | 0x00000060u, 96u,
                                                    TPM_ALG_SHA256, 0),
                   (int)TPM_NV_ATTRS, "inherits: an undefined TPM_NT value refused");

    /* Controls at each boundary, so the refusals above are about the rule and
     * not about the validator rejecting everything. */
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc, 1u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_OK, "control: the smallest ordinary index is legal");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc, 4096u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_OK, "control: the largest ordinary index is legal");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc |
                                                    TPMA_NV_TYPE(TPM_NT_COUNTER),
                                                    8u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_OK, "control: a legal platform counter is accepted");
    TEST_ASSERT_EQ((int)tpm_nv_attrs_valid_platform(rw | pc |
                                                    TPMA_NV_TYPE(TPM_NT_EXTEND),
                                                    32u, TPM_ALG_SHA256, 0),
                   (int)TPM_NV_OK, "control: a correctly sized EXTEND index is legal");
}

/* ---- Name and parse boundaries ---- */

static void test_nv_name_boundaries(void)
{
    struct tpm_nv_public pub;
    uint8_t name[TPM_NV_NAME_MAX];

    /* An absent authPolicy is a legal public area, not a degenerate one. */
    nvid_fill_public(&pub, 0u, 0u);
    TEST_ASSERT_EQ(tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &pub, name,
                                        sizeof name),
                   (int)TPM_NV_NAME_MAX, "policy_len 0 computes a Name");

    /* The largest authPolicy the contract can hold must still fit the
     * marshalling buffer -- this is the boundary that would overflow it. */
    nvid_fill_public(&pub, (uint16_t)TPM_NV_POLICY_MAX, 0xC3u);
    TEST_ASSERT_EQ(tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &pub, name,
                                        sizeof name),
                   (int)TPM_NV_NAME_MAX, "policy_len at TPM_NV_POLICY_MAX computes");

    /* One byte past it is refused rather than truncated: a truncated policy
     * would compare equal to a different policy sharing its prefix. */
    pub.policy_len = (uint16_t)(TPM_NV_POLICY_MAX + 1u);
    TEST_ASSERT_EQ(tpm2_nv_name_compute(TPM_NV_INDEX_BASELINE, &pub, name,
                                        sizeof name),
                   -1, "policy_len past TPM_NV_POLICY_MAX is refused");
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    0),
                   (int)TPM_NV_BADARG, "a NULL identity output is BADARG");
    {
        struct tpm_nv_identity id;
        TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub,
                                                        1, &id),
                       (int)TPM_NV_BADARG,
                       "an oversized authPolicy cannot be enrolled");
        TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, 0, 1,
                                                        &id),
                       (int)TPM_NV_BADARG, "a NULL public area is BADARG");
    }
}

/* ---- The handle-owning verified read ----
 *
 * tpm_nv_verify_then cannot stop an op from submitting against a DIFFERENT
 * index than the one verified, which is why the read form owns the handle
 * outright: there is no argument for a caller to get wrong. */

static void test_nv_verify_and_read(void)
{
    struct tpm_t_test_state prev;
    struct tpm_nv_public pub;
    struct tpm_nv_identity id;
    uint8_t buf[32];
    uint16_t got = 0xFFFFu;
    tpm_nv_status_t st;

    nvid_fill_public(&pub, 32u, 0x5Au);
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "enroll the contract");

    /* Control: a verified index reads, and the read names the ENROLLED handle
     * -- taken from the contract, never from a caller argument. The transfer is
     * EXACT, so cap is the fake's payload length (4) rather than the buffer
     * size; asking for more than the TPM returns is a refusal, asserted below. */
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                    TPMA_NV_WRITTEN);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_and_read(&id, 0u, buf, 4u, &got);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "control: a verified index reads");
    TEST_ASSERT_EQ((int)got, 4, "an exact transfer reports exactly what was asked");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_READ), 1, "exactly one content read");
    /* NV_Read handle area: header(10) + authHandle(4) + nvIndex(4). */
    TEST_ASSERT_EQ((int)tpm2_be32_get(nvf_cmd + 14), (int)TPM_NV_INDEX_BASELINE,
                   "the content read targets the ENROLLED handle, not another anchor");

    /* A different anchor's contract reads THAT anchor, proving the handle
     * follows the contract rather than being fixed in the code. */
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_AB_FLOOR, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "enroll a second anchor");
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                    TPMA_NV_WRITTEN);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_and_read(&id, 0u, buf, 4u, &got);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "the second anchor reads");
    TEST_ASSERT_EQ((int)tpm2_be32_get(nvf_cmd + 14), (int)TPM_NV_INDEX_AB_FLOOR,
                   "the read follows the contract's handle");

    /* A failed verification must read NOTHING and must not report a length. */
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "re-enroll the first anchor");
    got = 0xFFFFu;
    nvid_arm_public(TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_and_read(&id, 0u, buf, 4u, &got);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "a redefined index is refused");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_READ), 0, "and NO contents were read");
    TEST_ASSERT_EQ((int)got, 0xFFFF,
                   "out_len is untouched on failure, never a stale length");

    /* A SHORT response is a refusal, not a quiet partial success. Asking for 8
     * bytes from a fake that returns 4 is exactly the shape that used to be
     * reported OK, leaving a record consumer with truncated bytes it had no way
     * to notice. */
    got = 0xFFFFu;
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                    TPMA_NV_WRITTEN);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_and_read(&id, 0u, buf, 8u, &got);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "a response shorter than requested is a malformed response");
    TEST_ASSERT_EQ((int)got, 0xFFFF, "and out_len stays untouched");

    /* An OVER-LENGTH response is refused for the same reason: the requested
     * size and the output bound are one number, so extra bytes are never
     * copied. The fake serves 8 where 4 were asked for. */
    got = 0xFFFFu;
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                    TPMA_NV_WRITTEN);
    nvf_read_payload_len = 8u;
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_and_read(&id, 0u, buf, 4u, &got);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_TRANSPORT,
                   "a response longer than requested is refused, not truncated");
    TEST_ASSERT_EQ((int)got, 0xFFFF, "and out_len stays untouched");

    /* THE attack this section exists to refuse, through the consumer API.
     * The recreated index has a BYTE-IDENTICAL public area, so its Name and its
     * definition both match; only TPMA_NV_WRITTEN is clear. It must be refused
     * BEFORE any NV_Read -- and refused as RECREATED, not left to surface as
     * TPM_NV_UNINIT, which the baseline layer maps to NO_BASELINE and would
     * therefore launder a destroyed anchor into a first enrollment. */
    got = 0xFFFFu;
    nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA);
    prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
    st = tpm_nv_verify_and_read(&id, 0u, buf, 4u, &got);
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "a same-definition UNWRITTEN index is refused as RECREATED");
    TEST_ASSERT(st != TPM_NV_UNINIT,
                "and never as UNINIT, which downstream reads as first enrollment");
    TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_READ), 0,
                   "the recreated index is refused BEFORE any content read");
    TEST_ASSERT_EQ((int)got, 0xFFFF, "and out_len stays untouched");

    /* Control: an anchor enrolled as NOT written is legitimately unwritten, so
     * the same live public area must be ACCEPTED against that contract. Without
     * this the refusal above would pass against a gate that refuses every
     * unwritten index regardless of what enrollment recorded. */
    {
        struct tpm_nv_identity unwritten;
        TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE,
                                                        &pub, 0, &unwritten),
                       (int)TPM_NV_OK, "enroll an anchor as not-yet-written");
        nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA);
        prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
        st = tpm_nv_verify_and_read(&unwritten, 0u, buf, 4u, &got);
        tpm_t_test_restore(prev);
        TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                       "control: an anchor enrolled unwritten reads while unwritten");
    }

    TEST_ASSERT_EQ((int)tpm_nv_verify_and_read(0, 0u, buf, 4u, &got),
                   (int)TPM_NV_BADARG, "a NULL contract is BADARG");
    TEST_ASSERT_EQ((int)tpm_nv_verify_and_read(&id, 0u, 0, 4u, &got),
                   (int)TPM_NV_BADARG, "a NULL output is BADARG");
    TEST_ASSERT_EQ((int)tpm_nv_verify_and_read(&id, 0u, buf, 0u, &got),
                   (int)TPM_NV_BADARG, "a zero capacity is BADARG");
    /* A cap past the single-transfer maximum is a REFUSAL rather than a silent
     * clamp: clamping is what let the request and the output bound disagree. */
    TEST_ASSERT_EQ((int)tpm_nv_verify_and_read(&id, 0u, buf,
                                               (uint16_t)(TPM_NV_MAX_DATA + 1u),
                                               &got),
                   (int)TPM_NV_BADARG, "a cap past TPM_NV_MAX_DATA is refused");
}

/* ---- A persisted contract is untrusted input ---- */

static void test_nv_identity_persisted_bounds(void)
{
    struct tpm_nv_public pub;
    struct tpm_nv_identity id;

    nvid_fill_public(&pub, 32u, 0x5Au);
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "enroll a well-formed contract");

    /* struct tpm_nv_identity is EXPORTED so a record layer can persist it, so a
     * contract read back from storage is untrusted like any other input. An
     * oversized policy_len must be a refusal, not a walk past auth_policy[64]
     * on both sides of the comparison. */
    id.policy_len = (uint16_t)(TPM_NV_POLICY_MAX + 1u);
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &pub), (int)TPM_NV_CONTRACT,
                   "an oversized ENROLLED policy_len is refused before comparing");

    id.policy_len = 32u;
    pub.policy_len = (uint16_t)(TPM_NV_POLICY_MAX + 1u);
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &pub), (int)TPM_NV_CONTRACT,
                   "an oversized OBSERVED policy_len is refused before comparing");

    /* CONTRACT is deliberately NOT BADARG: corrupt persisted state is an
     * authorized-recovery question, while BADARG is documented as a caller
     * error with no transaction issued. A consumer choosing between halting on
     * an invariant violation and entering recovery needs them distinguishable,
     * and NULL is still the caller's mistake. */
    TEST_ASSERT(TPM_NV_CONTRACT != TPM_NV_BADARG,
                "a corrupt persisted contract is not a caller argument error");
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(0, &pub), (int)TPM_NV_BADARG,
                   "a NULL argument IS still BADARG");

    /* Controls at the boundary: exactly TPM_NV_POLICY_MAX is legal on both
     * sides, so the refusals above are about the overrun and not about the
     * comparator rejecting large policies. */
    nvid_fill_public(&pub, (uint16_t)TPM_NV_POLICY_MAX, 0xC3u);
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK, "control: a max-length policy enrolls");
    TEST_ASSERT_EQ((int)tpm_nv_identity_match(&id, &pub), (int)TPM_NV_OK,
                   "control: a max-length policy compares equal to itself");
}

/* ---- Review-round hardening: fixes from the post-commit review wave ---- */

static void test_nv_review_hardening(void)
{
    struct tpm_nv_public pub;
    struct tpm_nv_identity id;
    uint8_t rsp[64];
    uint32_t one = 5u;

    /* The exported N-session validator takes caller-controlled arguments, so a
     * NULL response and an out-of-range auth_off must be refusals rather than a
     * crash or an overread. auth_off near UINT32_MAX is the wrap case: a
     * `size < off + 2` guard is FALSE once off + 2 overflows, which would let
     * the read land far outside the buffer. */
    memset(rsp, 0, sizeof rsp);
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(0, one, 0u, 1u), 0,
                   "a NULL response is refused, not dereferenced");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, one, one + 1u, 1u), 0,
                   "an auth_off past the response is refused");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, one, 0xFFFFFFFCu, 1u), 0,
                   "an auth_off that would WRAP the bound check is refused");
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, one, 0u, 0xFFFFFFFFu), 0,
                   "a session count that cannot fit is refused before the loop");
    /* Control: the valid shape still validates, so the guards above reject the
     * malformed cases rather than everything. */
    TEST_ASSERT_EQ(tpm_session_auth_response_n_ok(rsp, one, 0u, 1u), 1,
                   "control: a well-formed one-session area still validates");

    /* An index whose WRITTEN bit is not durable cannot back an expect_written
     * contract: TPM Reset clears WRITTEN on a TPMA_NV_CLEAR_STCLEAR index, so
     * enrolling one that way would report RECREATED after every ordinary
     * reboot -- an attack verdict for a normal event. */
    nvid_fill_public(&pub, 32u, 0x5Au);
    pub.attrs |= TPMA_NV_CLEAR_STCLEAR;
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_AB_FLOOR, &pub, 1,
                                                    &id),
                   (int)TPM_NV_ATTRS,
                   "a CLEAR_STCLEAR index cannot be enrolled as written");
    /* Controls: the same index enrolls fine when NOT claimed written, and a
     * durable index enrolls fine when claimed written. */
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_AB_FLOOR, &pub, 0,
                                                    &id),
                   (int)TPM_NV_OK,
                   "control: the same index enrolls when not claimed written");
    nvid_fill_public(&pub, 32u, 0x5Au);
    TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_AB_FLOOR, &pub, 1,
                                                    &id),
                   (int)TPM_NV_OK,
                   "control: a durable index still enrolls as written");

    /* The Name verdict may not be discarded: tpm_nv_read_public already serves
     * the public-area-only case, so a NULL out_name_ok could only let a caller
     * read OK as though the identity had been checked. */
    TEST_ASSERT_EQ((int)tpm_nv_read_identity(TPM_NV_INDEX_BASELINE, &pub, 0),
                   (int)TPM_NV_BADARG,
                   "the Name verdict cannot be discarded");

    /* A malformed PERSISTED contract must propagate as TPM_NV_CONTRACT through
     * BOTH verification gates, not collapse into a transport error or a default
     * arm -- the two statuses lead to opposite remediations (authorized
     * recovery versus distrusting the device). */
    {
        struct tpm_t_test_state prev;
        struct tpm_nv_identity bad;
        tpm_nv_status_t st;
        nvid_fill_public(&pub, 32u, 0x5Au);
        TEST_ASSERT_EQ((int)tpm_nv_identity_from_public(TPM_NV_INDEX_BASELINE,
                                                        &pub, 1, &bad),
                       (int)TPM_NV_OK, "enroll a good contract first");
        bad.policy_len = (uint16_t)(TPM_NV_POLICY_MAX + 1u);

        nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                        TPMA_NV_WRITTEN);
        prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
        st = tpm_nv_verify_identity(&bad, 0);
        tpm_t_test_restore(prev);
        TEST_ASSERT_EQ((int)st, (int)TPM_NV_CONTRACT,
                       "the standalone gate propagates a corrupt contract");

        nvid_arm_public(TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA |
                        TPMA_NV_WRITTEN);
        prev = tpm_t_test_install(&nvf_io, TPM_T_IFACE_TIS, 1);
        st = tpm_nv_verify_and_read(&bad, 0u, (uint8_t *)&pub, 4u, 0);
        tpm_t_test_restore(prev);
        TEST_ASSERT_EQ((int)st, (int)TPM_NV_CONTRACT,
                       "the atomic gate propagates a corrupt contract");
        TEST_ASSERT_EQ(nvf_saw_cc(TPM2_CC_NV_READ), 0,
                       "and reads no contents on a corrupt contract");
    }
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
    test_suite_register_cat("tpm: sequence submit dispatch flag", test_seq_last_submit_dispatched, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV teardown proof + wrapper bounds", test_nv_teardown_and_bounds, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV index Name computation", test_nv_name_compute, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV enrolled identity contract", test_nv_identity_contract, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV lifecycle classification", test_nv_lifecycle_classify, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV two-index separation", test_nv_two_index_separation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV platform define + POLICY_DELETE",
                            test_nv_platform_define_attrs, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV UndefineSpaceSpecial marshal",
                            test_nv_undefine_special_marshal, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV PolicyCommandCode marshal",
                            test_nv_policy_command_code_marshal, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV multi-session response validation",
                            test_nv_auth_response_sessions, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV live identity gate", test_nv_live_identity, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV delete-policy digest", test_nv_delete_policy_digest, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV special-delete execution", test_nv_undefine_special_exec, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV verified-operation atomicity", test_nv_verify_then_atomicity, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV lifecycle precedence", test_nv_lifecycle_precedence, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV incomplete counter evidence",
                            test_nv_lifecycle_incomplete_counter, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV platform validator inheritance",
                            test_nv_platform_validator_inheritance, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV Name boundaries", test_nv_name_boundaries, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV verified read owns the handle", test_nv_verify_and_read, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV persisted contract bounds", test_nv_identity_persisted_bounds, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: NV review-round hardening",
                            test_nv_review_hardening, TEST_CAT_SECURITY);
}
