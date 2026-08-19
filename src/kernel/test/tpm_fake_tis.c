/* ============================================================================
 * tpm_fake_tis.c -- shared fake-TIS transport for the TPM unit suites.
 *
 * Contract, rationale and the reason this exists alongside test_tpm_nv.c's own
 * fake: include/kernel/test/tpm_fake_tis.h.
 *
 * The TIS handshake modelled here is the minimum tpm_transport.c drives:
 * COMMAND_READY -> FIFO writes -> GO -> DATA_AVAIL -> FIFO reads. Response
 * ENCODINGS follow the shapes the parsers in tpm_transport.c / tpm_nv.c accept
 * (ST_SESSIONS success carries parameterSize plus a minimal response auth area;
 * a TPM error response is always a bare ST_NO_SESSIONS header).
 * ========================================================================== */

#include "kernel/test/tpm_fake_tis.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "libc/string.h"

/* NV_Write of the largest stored object is the biggest command:
 * header(10) + authHandle(4) + nvIndex(4) + authArea(13) + TPM2B(2 + 512)
 * + offset(2) = 547. NV_Read of the same object is the biggest response:
 * header(10) + parameterSize(4) + TPM2B(2 + 512) + auth area(5) = 533. */
#define FT_CMD_CAP 576u
#define FT_RSP_CAP 576u

/* Format-1 TPM_RC_HANDLE on handle 1: what a real TPM answers for an NV command
 * against an index it has never defined. tpm_nv_classify_rc maps it to
 * TPM_NV_NOTFOUND, which the baseline layer reports as NO_BASELINE. */
#define FT_RC_HANDLE 0x0000018Bu

/* TPM_RC_COMMAND_CODE: what a device answers for a command it does not
 * implement. The fake models five commands and refuses everything else. */
#define FT_RC_COMMAND_CODE 0x00000143u

/* ---- device state ---- */

static uint8_t  ft_cmd[FT_CMD_CAP];
static uint32_t ft_cmd_len;      /* LOGICAL length: bytes the writer pushed */
static uint32_t ft_cmd_expect;
/* Set when the writer pushed more than FT_CMD_CAP. ft_cmd_len keeps counting
 * (the GO gate compares it against the declared commandSize), so it is NOT a
 * bound on the bytes actually captured -- everything past the cap is stale.
 * Every field read below is validated against ft_cmd_has() rather than against
 * the capacity, because reading a fixed offset that happens to land inside the
 * array is still reading a byte this command never wrote. */
static int      ft_cmd_overflow;
static uint8_t  ft_rsp[FT_RSP_CAP];
static uint32_t ft_rsp_len;
static uint32_t ft_rsp_pos;
static int      ft_ready;
static int      ft_executed;

/* ---- content ---- */

static int      ft_nv_defined;
static uint32_t ft_nv_index;
static uint8_t  ft_nv_data[TPM_FAKE_TIS_NV_MAX];
static uint16_t ft_nv_len;        /* bytes WRITTEN so far (the content extent) */
/* The DEFINED size, which a real TPM fixes at NV_DefineSpace and never changes:
 * it is what ReadPublic reports and what a write is bounded by. Kept apart from
 * the written extent because collapsing the two let a write grow the index's
 * public dataSize, which no TPM does -- and dataSize feeds the index Name, so a
 * fixture that drifts it would quietly invalidate every identity check run
 * against it. */
static uint16_t ft_nv_cap;
static int      ft_nv_written;    /* TPMA_NV_WRITTEN: set by a successful write */
static uint32_t ft_nv_attrs;      /* attributes the definition was created with */
static uint16_t ft_nv_name_alg;   /* nameAlg of the definition (SHA-256) */

static uint8_t  ft_pcr[24][TPM_FAKE_TIS_DIGEST];
static uint8_t  ft_pcr_present[24];

/* ---- failure injection ---- */

static uint32_t ft_fail_cc;
static uint32_t ft_fail_rc;
static int      ft_fail_times;

/* ---- transcript ---- */

static struct tpm_fake_tis_req ft_log[TPM_FAKE_TIS_LOG_MAX];
static uint32_t ft_log_n;
static int      ft_log_overflow;

/* ---- TIS registers (the subset tpm_transport.c touches) ---- */

#define FT_REG_STS   0x018u
#define FT_REG_FIFO  0x024u
#define FT_STS_EXPECT        0x08u
#define FT_STS_DATA_AVAIL    0x10u
#define FT_STS_GO            0x20u
#define FT_STS_COMMAND_READY 0x40u
#define FT_STS_VALID         0x80u

void tpm_fake_tis_reset(void)
{
    ft_cmd_len = 0u; ft_cmd_expect = 0u; ft_cmd_overflow = 0;
    ft_rsp_len = 0u; ft_rsp_pos = 0u;
    ft_ready = 0; ft_executed = 0;
    ft_nv_defined = 0; ft_nv_index = 0u; ft_nv_len = 0u;
    ft_nv_cap = 0u; ft_nv_written = 0;
    ft_nv_attrs = 0u; ft_nv_name_alg = TPM_ALG_SHA256;
    memset(ft_nv_data, 0, sizeof ft_nv_data);
    memset(ft_pcr, 0, sizeof ft_pcr);
    memset(ft_pcr_present, 0, sizeof ft_pcr_present);
    ft_fail_cc = 0u; ft_fail_rc = 0u; ft_fail_times = 0;
    memset(ft_log, 0, sizeof ft_log);
    ft_log_n = 0u; ft_log_overflow = 0;
}

void tpm_fake_tis_nv_set(uint32_t nv_index, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    if (!data || len == 0u || (uint32_t)len > TPM_FAKE_TIS_NV_MAX)
        return;
    ft_nv_defined = 1;
    ft_nv_index = nv_index;
    ft_nv_len = len;
    ft_nv_cap = len;
    ft_nv_written = 1;
    /* The attributes tpm_nv_define_data() asks for, so a re-define of a
     * fixture-seeded index takes the same already-defined path a real machine
     * takes on rotation rather than a made-up mismatch. TPMA_NV_WRITTEN is part
     * of that state: an index carrying content HAS been written, and the bit
     * lives inside the attributes the index Name covers. */
    ft_nv_attrs = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA |
                  TPMA_NV_WRITTEN;
    ft_nv_name_alg = TPM_ALG_SHA256;
    for (i = 0; i < len; i++)
        ft_nv_data[i] = data[i];
}

void tpm_fake_tis_nv_clear(void)
{
    ft_nv_defined = 0;
    ft_nv_index = 0u;
    ft_nv_len = 0u;
    ft_nv_cap = 0u;
    ft_nv_written = 0;
    ft_nv_attrs = 0u;
    memset(ft_nv_data, 0, sizeof ft_nv_data);
}

uint16_t tpm_fake_tis_nv_content(uint8_t *out, uint16_t cap)
{
    uint16_t n, i;
    if (!ft_nv_defined || !ft_nv_written)
        return 0u;
    n = (cap < ft_nv_len) ? cap : ft_nv_len;
    for (i = 0; out && i < n; i++)
        out[i] = ft_nv_data[i];
    return ft_nv_len;
}

void tpm_fake_tis_pcr_set(uint32_t pcr_index, const uint8_t *digest)
{
    uint32_t i;
    if (pcr_index >= 24u || !digest)
        return;
    for (i = 0; i < TPM_FAKE_TIS_DIGEST; i++)
        ft_pcr[pcr_index][i] = digest[i];
    ft_pcr_present[pcr_index] = 1u;
}

void tpm_fake_tis_pcr_clear(uint32_t pcr_index)
{
    if (pcr_index >= 24u)
        return;
    ft_pcr_present[pcr_index] = 0u;
    memset(ft_pcr[pcr_index], 0, TPM_FAKE_TIS_DIGEST);
}

void tpm_fake_tis_fail_cc(uint32_t cc, uint32_t rc, int times)
{
    /* rc 0 DISABLES, and clearing the whole record is what makes that true. The
     * dispatcher's injection branch keys off (cc, times) and then emits
     * ft_bare(rc), so keeping the command with rc 0 would intercept it and
     * fabricate a bare SUCCESS -- the wrapper above would see its write or
     * define succeed while the fake never executed it and NV never moved. A
     * disable that silently becomes "succeed without doing anything" is worse
     * than no disable at all in shared fixture code. */
    if (rc == TPM2_RC_SUCCESS) {
        ft_fail_cc = 0u;
        ft_fail_rc = 0u;
        ft_fail_times = 0;
        return;
    }
    ft_fail_cc = cc;
    ft_fail_rc = rc;
    ft_fail_times = times;
}

uint32_t tpm_fake_tis_log_count(void) { return ft_log_n; }
int      tpm_fake_tis_log_overflow(void) { return ft_log_overflow; }

const struct tpm_fake_tis_req *tpm_fake_tis_log(uint32_t i)
{
    return (i < ft_log_n) ? &ft_log[i] : (const struct tpm_fake_tis_req *)0;
}

uint32_t tpm_fake_tis_cc_count(uint32_t cc)
{
    uint32_t i, n = 0u;
    for (i = 0; i < ft_log_n; i++)
        if (ft_log[i].cc == cc)
            n++;
    return n;
}

/* Record the request BEFORE serving it, so a command the fake refuses is still
 * visible in the transcript -- a wrapper that asked for the wrong index must be
 * diagnosable from the log, not only from the status it got back. */
static struct tpm_fake_tis_req *ft_record(uint32_t cc)
{
    struct tpm_fake_tis_req *r;
    if (ft_log_n >= TPM_FAKE_TIS_LOG_MAX) {
        ft_log_overflow = 1;
        return (struct tpm_fake_tis_req *)0;
    }
    r = &ft_log[ft_log_n++];
    memset(r, 0, sizeof *r);
    r->cc = cc;
    return r;
}

/* 1 when the command actually captured `need` bytes. A command that overran the
 * fake's buffer captured NOTHING reliable past the cap, so it fails this for
 * every offset rather than only for the ones past FT_CMD_CAP. */
static int ft_cmd_has(uint32_t need)
{
    return (!ft_cmd_overflow && ft_cmd_len >= need) ? 1 : 0;
}

/* Bare ST_NO_SESSIONS header: every TPM error response, and every success for a
 * command this fake does not model in detail (FlushContext and friends). */
static void ft_bare(uint32_t rc)
{
    tpm2_be16_put(ft_rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(ft_rsp + 2, 10u);
    tpm2_be32_put(ft_rsp + 6, rc);
    ft_rsp_len = 10u;
}

/* ST_SESSIONS success with no return parameters: parameterSize(4) = 0 plus a
 * minimal response auth area (nonceTPM(2,0) + attributes(1) + hmac(2,0)). */
static void ft_sessions_empty(void)
{
    tpm2_be16_put(ft_rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(ft_rsp + 2, 19u);
    tpm2_be32_put(ft_rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(ft_rsp + 10, 0u);
    ft_rsp_len = 19u;
}

static void ft_do_nv_read(void)
{
    struct tpm_fake_tis_req *r = ft_record(TPM2_CC_NV_READ);
    uint32_t idx;
    uint16_t size, offset;
    uint32_t i;

    /* header(10) + authHandle(4) + nvIndex(4) + authArea(13) + size(2)
     * + offset(2). A short or over-cap command is refused BEFORE any field is
     * read: a fixed offset that lands inside the array is still a byte this
     * command never wrote. */
    if (!ft_cmd_has(35u)) {
        ft_bare(TPM2_RC_F1_SIZE);
        return;
    }
    idx = tpm2_be32_get(ft_cmd + 14);
    size = tpm2_be16_get(ft_cmd + 31);
    offset = tpm2_be16_get(ft_cmd + 33);
    if (r) { r->index = idx; r->size = size; r->offset = offset; }

    /* The response is built in ft_rsp, so the requested size is bounded by what
     * that buffer can carry as well as by the stored content. */
    if ((uint32_t)size + 21u > FT_RSP_CAP) {
        ft_bare(TPM2_RC_NV_SIZE);
        return;
    }

    /* An undefined index -- or a DIFFERENT index than the one this fixture
     * defined -- is TPM_RC_HANDLE, exactly as a real TPM answers it. This is
     * what makes a wrapper that reads the wrong index observable instead of
     * being handed the right bytes by a CC-only dispatcher. */
    if (!ft_nv_defined || idx != ft_nv_index) {
        ft_bare(FT_RC_HANDLE);
        return;
    }
    /* Defined but never written is TPM_RC_NV_UNINITIALIZED, a state distinct
     * from absent, and the baseline layer maps the two differently. */
    if (!ft_nv_written) {
        ft_bare(TPM2_RC_NV_UNINITIALIZED);
        return;
    }
    /* Reading past the DEFINED size is TPM_RC_NV_RANGE, so a wrapper that asks
     * for the wrong length or offset fails rather than being clamped into
     * looking correct. */
    if ((uint32_t)offset + (uint32_t)size > (uint32_t)ft_nv_cap) {
        ft_bare(TPM2_RC_NV_RANGE);
        return;
    }
    tpm2_be16_put(ft_rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(ft_rsp + 2, 21u + (uint32_t)size);
    tpm2_be32_put(ft_rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(ft_rsp + 10, 2u + (uint32_t)size);   /* parameterSize */
    tpm2_be16_put(ft_rsp + 14, size);                  /* TPM2B_MAX_NV_BUFFER */
    for (i = 0; i < size; i++)
        ft_rsp[16 + i] = ft_nv_data[offset + i];
    /* Trailing 5-byte response auth area, already zeroed. */
    ft_rsp_len = 21u + (uint32_t)size;
}

static void ft_do_nv_write(void)
{
    struct tpm_fake_tis_req *r = ft_record(TPM2_CC_NV_WRITE);
    uint32_t idx;
    uint16_t len, offset;
    uint32_t i;

    /* header(10) + authHandle(4) + nvIndex(4) + authArea(13) + TPM2B(2 + len)
     * + offset(2). The TPM2B length is read first, then re-validated: `len` is
     * command data, so the offset field it positions must be proven captured
     * before it is read. */
    if (!ft_cmd_has(33u)) {
        ft_bare(TPM2_RC_F1_SIZE);
        return;
    }
    idx = tpm2_be32_get(ft_cmd + 14);
    len = tpm2_be16_get(ft_cmd + 31);
    if (!ft_cmd_has(33u + (uint32_t)len + 2u)) {
        if (r) { r->index = idx; r->size = len; }
        ft_bare(TPM2_RC_NV_SIZE);
        return;
    }
    offset = tpm2_be16_get(ft_cmd + 33 + len);
    if (r) { r->index = idx; r->size = len; r->offset = offset; }

    if (!ft_nv_defined || idx != ft_nv_index) {
        ft_bare(FT_RC_HANDLE);
        return;
    }
    /* A write past the DEFINED size is refused rather than allowed to grow the
     * index: dataSize is fixed at define on a real TPM, and letting a write
     * move it would drift both the public area and the Name the fake reports. */
    if ((uint32_t)offset + (uint32_t)len > (uint32_t)ft_nv_cap) {
        ft_bare(TPM2_RC_NV_RANGE);
        return;
    }
    for (i = 0; i < len; i++)
        ft_nv_data[offset + i] = ft_cmd[33 + i];
    if ((uint32_t)offset + (uint32_t)len > (uint32_t)ft_nv_len)
        ft_nv_len = (uint16_t)(offset + len);
    ft_nv_written = 1;
    ft_nv_attrs |= TPMA_NV_WRITTEN;
    ft_sessions_empty();
}

static void ft_do_nv_define(void)
{
    struct tpm_fake_tis_req *r = ft_record(TPM2_CC_NV_DEFINE_SPACE);
    uint32_t idx, attrs;
    uint16_t policy_len, data_size;

    /* header(10) + authHandle(4) + authArea(13) + auth TPM2B(2) + nvPublic
     * size(2) + nvIndex(4) + nameAlg(2) + attrs(4) + policy TPM2B(2 + N)
     * + dataSize(2). dataSize is positioned by the POLICY LENGTH, which is
     * command data -- reading it without re-validating is the out-of-bounds
     * read this guard closes. */
    if (!ft_cmd_has(43u)) {
        ft_bare(TPM2_RC_F1_SIZE);
        return;
    }
    idx = tpm2_be32_get(ft_cmd + 31);
    attrs = tpm2_be32_get(ft_cmd + 37);
    policy_len = tpm2_be16_get(ft_cmd + 41);
    if (!ft_cmd_has(43u + (uint32_t)policy_len + 2u)) {
        if (r) { r->index = idx; r->attrs = attrs; }
        ft_bare(TPM2_RC_NV_SIZE);
        return;
    }
    data_size = tpm2_be16_get(ft_cmd + 43 + policy_len);
    if (r) { r->index = idx; r->attrs = attrs; r->size = data_size; }

    /* Already defined at this index -> TPM_RC_NV_DEFINED, the idempotent-define
     * case tpm_baseline_enroll_unauthenticated deliberately tolerates. */
    if (ft_nv_defined && idx == ft_nv_index) {
        ft_bare(TPM2_RC_NV_DEFINED);
        return;
    }
    if (data_size == 0u || (uint32_t)data_size > TPM_FAKE_TIS_NV_MAX) {
        ft_bare(TPM2_RC_NV_SIZE);
        return;
    }
    ft_nv_defined = 1;
    ft_nv_index = idx;
    ft_nv_len = 0u;              /* freshly defined: nothing written yet */
    ft_nv_cap = data_size;
    ft_nv_written = 0;
    /* TPMA_NV_WRITTEN is a read-only STATUS bit the TPM owns, so a define never
     * takes it from the request. */
    ft_nv_attrs = attrs & ~(uint32_t)TPMA_NV_WRITTEN;
    ft_nv_name_alg = tpm2_be16_get(ft_cmd + 35);   /* proven captured above */
    memset(ft_nv_data, 0, sizeof ft_nv_data);
    ft_sessions_empty();
}

/* TPM2B_NV_PUBLIC{ nvIndex(4) nameAlg(2) attrs(4) authPolicy TPM2B dataSize(2) }
 * followed by the index's TPM2B_NAME. ST_NO_SESSIONS: ReadPublic takes no auth.
 * Only the empty-authPolicy shape a data index has is modelled -- the baseline
 * blob is the owner-auth index, and the policy-protected indexes are covered by
 * test_tpm_nv.c's own fake. */
static void ft_do_nv_read_public(void)
{
    struct tpm_fake_tis_req *r = ft_record(TPM2_CC_NV_READ_PUBLIC);
    uint32_t idx;
    struct tpm_nv_public np;
    uint8_t nm[TPM_NV_NAME_MAX];
    uint32_t pubsize = 14u;
    uint32_t k;

    if (!ft_cmd_has(14u)) {          /* header(10) + nvIndex(4) */
        ft_bare(TPM2_RC_F1_SIZE);
        return;
    }
    idx = tpm2_be32_get(ft_cmd + 10);
    if (r) { r->index = idx; }
    if (!ft_nv_defined || idx != ft_nv_index) {
        ft_bare(FT_RC_HANDLE);
        return;
    }

    memset(&np, 0, sizeof np);
    np.data_size = ft_nv_cap;    /* the DEFINED size, stable across writes */
    np.attrs = ft_nv_written ? (ft_nv_attrs | (uint32_t)TPMA_NV_WRITTEN)
                             : (ft_nv_attrs & ~(uint32_t)TPMA_NV_WRITTEN);
    np.name_alg = ft_nv_name_alg;
    np.policy_len = 0u;

    tpm2_be16_put(ft_rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(ft_rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be16_put(ft_rsp + 10, (uint16_t)pubsize);
    tpm2_be32_put(ft_rsp + 12, idx);
    tpm2_be16_put(ft_rsp + 16, np.name_alg);
    tpm2_be32_put(ft_rsp + 18, np.attrs);
    tpm2_be16_put(ft_rsp + 22, 0u);            /* authPolicy: empty */
    tpm2_be16_put(ft_rsp + 24, np.data_size);
    if (tpm2_nv_name_compute(idx, &np, nm, sizeof nm) != (int)TPM_NV_NAME_MAX) {
        ft_bare(TPM2_RC_F1_VALUE);
        return;
    }
    tpm2_be16_put(ft_rsp + 26, (uint16_t)TPM_NV_NAME_MAX);
    for (k = 0; k < TPM_NV_NAME_MAX; k++)
        ft_rsp[28 + k] = nm[k];
    ft_rsp_len = 28u + TPM_NV_NAME_MAX;
    tpm2_be32_put(ft_rsp + 2, ft_rsp_len);
}

static void ft_do_pcr_read(void)
{
    struct tpm_fake_tis_req *r = ft_record(TPM2_CC_PCR_READ);
    uint32_t sel_count;
    uint16_t alg;
    uint8_t  sos;
    uint8_t  sel[3];
    uint32_t idx = 24u;   /* 24 = "no bit set", which is not a valid PCR */
    uint32_t b, bit;

    /* header(10) + count(4) + hashAlg(2) + sizeofSelect(1) + pcrSelect(3). */
    if (!ft_cmd_has(20u)) {
        ft_bare(TPM2_RC_F1_SIZE);
        return;
    }
    sel_count = tpm2_be32_get(ft_cmd + 10);
    alg = tpm2_be16_get(ft_cmd + 14);
    sos = ft_cmd[16];
    sel[0] = ft_cmd[17]; sel[1] = ft_cmd[18]; sel[2] = ft_cmd[19];
    for (b = 0; b < 3u; b++)
        for (bit = 0; bit < 8u; bit++)
            if (sel[b] & (uint8_t)(1u << bit)) {
                if (idx != 24u) { idx = 25u; break; }   /* >1 bit: not modelled */
                idx = b * 8u + bit;
            }
    if (r) {
        r->index = idx;
        r->alg = alg;
        r->sel_count = (uint8_t)sel_count;
        r->sel[0] = sel[0]; r->sel[1] = sel[1]; r->sel[2] = sel[2];
    }

    /* Only the single-PCR SHA-256 read the measured-boot path issues is
     * modelled. Anything else is TPM_RC_VALUE rather than a plausible-looking
     * answer, so an unexpected request fails visibly. */
    if (sel_count != 1u || sos != 3u || alg != TPM_ALG_SHA256 || idx >= 24u) {
        ft_bare(TPM2_RC_F1_VALUE);
        return;
    }

    tpm2_be16_put(ft_rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(ft_rsp + 6, TPM2_RC_SUCCESS);
    tpm2_be32_put(ft_rsp + 10, 1u);            /* pcrUpdateCounter */
    tpm2_be32_put(ft_rsp + 14, 1u);            /* pcrSelectionOut count */
    tpm2_be16_put(ft_rsp + 18, alg);
    ft_rsp[20] = 3u;                            /* sizeofSelect */
    if (ft_pcr_present[idx]) {
        uint32_t i;
        ft_rsp[21] = sel[0]; ft_rsp[22] = sel[1]; ft_rsp[23] = sel[2];
        tpm2_be32_put(ft_rsp + 24, 1u);         /* TPML_DIGEST count */
        tpm2_be16_put(ft_rsp + 28, (uint16_t)TPM_FAKE_TIS_DIGEST);
        for (i = 0; i < TPM_FAKE_TIS_DIGEST; i++)
            ft_rsp[30 + i] = ft_pcr[idx][i];
        ft_rsp_len = 30u + TPM_FAKE_TIS_DIGEST;
    } else {
        /* Bank inactive for this PCR: the selection bit comes back CLEAR and no
         * digest follows, which is how a TPM says "I did not read that one". */
        ft_rsp[21] = 0u; ft_rsp[22] = 0u; ft_rsp[23] = 0u;
        tpm2_be32_put(ft_rsp + 24, 0u);
        ft_rsp_len = 28u;
    }
    tpm2_be32_put(ft_rsp + 2, ft_rsp_len);
}

static void ft_build_response(void)
{
    uint32_t cc;

    memset(ft_rsp, 0, FT_RSP_CAP);

    /* The command code itself is a field like any other: a command shorter than
     * a header, or one that overran the buffer, gets a size error and is never
     * dispatched. Recorded with cc 0 so the transcript still shows that
     * something was submitted. */
    if (!ft_cmd_has(10u)) {
        (void)ft_record(0u);
        ft_bare(TPM2_RC_F1_SIZE);
        return;
    }
    cc = tpm2_be32_get(ft_cmd + 6);

    if (ft_fail_cc != 0u && cc == ft_fail_cc && ft_fail_times != 0) {
        if (ft_fail_times > 0)
            ft_fail_times--;
        (void)ft_record(cc);
        ft_bare(ft_fail_rc);
        return;
    }

    switch (cc) {
    case TPM2_CC_NV_READ:         ft_do_nv_read();   break;
    case TPM2_CC_NV_WRITE:        ft_do_nv_write();  break;
    case TPM2_CC_NV_DEFINE_SPACE: ft_do_nv_define(); break;
    case TPM2_CC_NV_READ_PUBLIC:  ft_do_nv_read_public(); break;
    case TPM2_CC_PCR_READ:        ft_do_pcr_read();  break;
    default:
        /* FAIL CLOSED on anything unmodelled. Answering an unknown command with
         * success let a wrapper issue extra TPM traffic and still pass every
         * test that did not assert the whole transcript -- the fake would be
         * hiding exactly the side effect it exists to expose.
         * TPM_RC_COMMAND_CODE is what a device answers for a command it does
         * not implement, so the refusal is also the honest one. */
        (void)ft_record(cc);
        ft_bare(FT_RC_COMMAND_CODE);
        break;
    }
}

static uint8_t ft_r8(uint32_t off)
{
    if (off == FT_REG_FIFO && ft_executed && ft_rsp_pos < ft_rsp_len)
        return ft_rsp[ft_rsp_pos++];
    return 0;
}

static void ft_w8(uint32_t off, uint8_t v)
{
    if (off != FT_REG_FIFO || ft_executed)
        return;
    if (ft_cmd_len < FT_CMD_CAP)
        ft_cmd[ft_cmd_len] = v;
    else
        ft_cmd_overflow = 1;
    ft_cmd_len++;
    if (ft_cmd_len == 6u)
        ft_cmd_expect = tpm2_be32_get(ft_cmd + 2);
}

static uint32_t ft_r32(uint32_t off)
{
    uint8_t sts;
    if (off != FT_REG_STS)
        return 0;
    sts = FT_STS_VALID;
    if (ft_ready && !ft_executed && ft_cmd_len == 0u)
        sts |= FT_STS_COMMAND_READY;
    if (!ft_executed && ft_cmd_len > 0u && ft_cmd_len < ft_cmd_expect)
        sts |= FT_STS_EXPECT;
    if (ft_executed && ft_rsp_pos < ft_rsp_len)
        sts |= FT_STS_DATA_AVAIL;
    return (uint32_t)sts | (32u << 8);   /* burstCount = 32 */
}

static void ft_w32(uint32_t off, uint32_t v)
{
    if (off != FT_REG_STS)
        return;
    if (v & FT_STS_COMMAND_READY) {
        ft_ready = 1;
        ft_executed = 0;
        ft_cmd_len = 0u;
        ft_cmd_expect = 0u;
        ft_cmd_overflow = 0;
        ft_rsp_pos = 0u;
    }
    if ((v & FT_STS_GO) && ft_cmd_len >= ft_cmd_expect) {
        ft_executed = 1;
        ft_build_response();
    }
}

static const struct tpm_t_io ft_io = { ft_r8, ft_w8, ft_r32, ft_w32 };

const struct tpm_t_io *tpm_fake_tis_io(void) { return &ft_io; }
