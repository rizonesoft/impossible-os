/* ============================================================================
 * tpm_nv.c -- TPM2 NV index storage + PCR-policy sessions (measured boot)
 *
 * Implements the NV storage MECHANISM for measured-boot attestation baselines.
 * Pure marshaling helpers (MMIO-free, fixture-tested) plus the tpm_nv_* wrappers
 * that drive the Phase-1 transport.
 *
 * Session lifetime discipline (single cleanup path): every wrapper that calls
 * TPM2_StartAuthSession routes ALL subsequent exits through one nv_flush() so a
 * failure or BUSY after the session is created can never leak a handle into the
 * TPM's small session pool (which would make baseline storage fail until
 * reboot). The trial/real PolicyPCR flow is hand-rolled here, not at the
 * transport seam, but the teardown is unconditional.
 *
 * Auth model: define/undefine always use owner auth (TPM_RH_OWNER + the
 * TPM_RS_PW password session) -- owner auth bypasses any PCR policy, so it is
 * restricted to lifecycle. A policy-protected index's read/write authorize via
 * a real TPM2_PolicyPCR session over tpm_pcr_baseline_mask() (the PCR allocation
 * table), never owner auth.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm_pcr_alloc.h"
#include "libc/string.h"

/* ---- Response-code classification (format-first; never wedges) ---- */

tpm_nv_status_t tpm_nv_classify_rc(uint32_t rc)
{
    if (rc == TPM2_RC_SUCCESS)
        return TPM_NV_OK;
    if ((rc & TPM2_RC_FMT1) == 0u) {
        /* Format-0 (RC_VER1 / RC_WARN): the whole low value IS the error.
         * Exact-compare -- masking these would corrupt the NV warning codes. */
        switch (rc) {
            case TPM2_RC_NV_LOCKED:        return TPM_NV_LOCKED;
            case TPM2_RC_NV_SPACE:         return TPM_NV_NOSPACE;
            case TPM2_RC_NV_DEFINED:       return TPM_NV_DEFINED;
            case TPM2_RC_NV_UNINITIALIZED: return TPM_NV_UNINIT;
            case TPM2_RC_NV_RANGE:
            case TPM2_RC_NV_SIZE:          return TPM_NV_RANGE;
            case TPM2_RC_NV_AUTHORIZATION: return TPM_NV_AUTH;
            default:                       return TPM_NV_TPMERR;
        }
    }
    /* Format-1: bits 0-5 are the error, bit 6 (P) selects parameter-vs-handle,
     * bits 8-11 carry the handle/parameter/session number. Mask to the base
     * error (bits 0-5 + the format bit 7) before comparing. */
    switch (rc & 0xBFu) {
        case TPM2_RC_F1_AUTH_FAIL:
        case TPM2_RC_F1_POLICY_FAIL: return TPM_NV_AUTH;
        case TPM2_RC_F1_HANDLE:      return TPM_NV_NOTFOUND;
        case TPM2_RC_F1_VALUE:
        case TPM2_RC_F1_SIZE:        return TPM_NV_RANGE;
        default:                     return TPM_NV_TPMERR;
    }
}

/* ---- Marshaling helpers ---- */

/* Append a one-session authorization area at off. Layout:
 *   authorizationSize(4) + sessionHandle(4) + nonce TPM2B(2 + nonce_len)
 *   + sessionAttributes(1) + hmac/password TPM2B(2 + auth_len).
 * continueSession (attrs bit 0) is set so the TPM keeps a real session alive
 * for our explicit FlushContext; harmless for the permanent TPM_RS_PW handle.
 * Returns the new offset. The caller guarantees buffer room. */
static uint32_t put_auth_area(uint8_t *buf, uint32_t off, uint32_t session,
                              const uint8_t *nonce, uint16_t nonce_len,
                              const uint8_t *auth, uint16_t auth_len)
{
    uint16_t i;
    uint32_t area = 4u + 2u + (uint32_t)nonce_len + 1u + 2u + (uint32_t)auth_len;
    tpm2_be32_put(buf + off, area); off += 4u;       /* authorizationSize */
    tpm2_be32_put(buf + off, session); off += 4u;    /* sessionHandle */
    tpm2_be16_put(buf + off, nonce_len); off += 2u;  /* nonce size */
    for (i = 0; i < nonce_len; i++) buf[off + i] = nonce ? nonce[i] : 0u;
    off += nonce_len;
    buf[off] = 0x01u; off += 1u;                     /* sessionAttributes: continue */
    tpm2_be16_put(buf + off, auth_len); off += 2u;   /* hmac/password size */
    for (i = 0; i < auth_len; i++) buf[off + i] = auth ? auth[i] : 0u;
    off += auth_len;
    return off;
}

uint32_t tpm2_build_nv_define(uint8_t *buf, uint32_t cap, uint32_t nv_index,
                              uint32_t attrs, uint16_t name_alg,
                              const uint8_t *auth_policy, uint16_t auth_policy_len,
                              uint16_t data_size)
{
    /* header(10) + authHandle(4) + authArea(13: PW session, empty owner pw)
     * + authValue TPM2B(2, empty) + TPM2B_NV_PUBLIC(2 + inner). */
    uint32_t inner = 4u + 2u + 4u + (2u + (uint32_t)auth_policy_len) + 2u;
    uint32_t total = 10u + 4u + 13u + 2u + 2u + inner;
    uint32_t off, i;
    if (!buf || cap < total || data_size == 0u ||
        (auth_policy_len != 0u && !auth_policy))
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_NV_DEFINE_SPACE);
    tpm2_be32_put(buf + 10, TPM_RH_OWNER);                 /* authHandle */
    off = put_auth_area(buf, 14u, TPM_RS_PW, 0, 0, 0, 0);  /* owner pw (empty) */
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* auth (index authValue) empty */
    tpm2_be16_put(buf + off, (uint16_t)inner); off += 2u;  /* nvPublic size */
    tpm2_be32_put(buf + off, nv_index); off += 4u;         /* nvIndex */
    tpm2_be16_put(buf + off, name_alg); off += 2u;         /* nameAlg */
    tpm2_be32_put(buf + off, attrs); off += 4u;            /* attributes */
    tpm2_be16_put(buf + off, auth_policy_len); off += 2u;  /* authPolicy size */
    for (i = 0; i < auth_policy_len; i++) buf[off + i] = auth_policy[i];
    off += auth_policy_len;
    tpm2_be16_put(buf + off, data_size); off += 2u;        /* dataSize */
    return off;
}

uint32_t tpm2_build_nv_undefine(uint8_t *buf, uint32_t cap, uint32_t nv_index)
{
    /* header(10) + authHandle(4) + nvIndex(4) + authArea(13). */
    uint32_t total = 10u + 4u + 4u + 13u;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_NV_UNDEFINE_SPACE);
    tpm2_be32_put(buf + 10, TPM_RH_OWNER);  /* authHandle */
    tpm2_be32_put(buf + 14, nv_index);      /* nvIndex */
    (void)put_auth_area(buf, 18u, TPM_RS_PW, 0, 0, 0, 0);
    return total;
}

uint32_t tpm2_build_nv_write(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                             uint32_t nv_index, uint32_t session_handle,
                             uint16_t offset, const uint8_t *data, uint16_t len)
{
    /* header(10) + authHandle(4) + nvIndex(4) + authArea(13)
     * + data TPM2B(2 + len) + offset(2). */
    uint32_t total = 10u + 4u + 4u + 13u + 2u + (uint32_t)len + 2u;
    uint32_t off, i;
    if (!buf || cap < total || len == 0u || !data || len > TPM_NV_MAX_DATA)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_NV_WRITE);
    tpm2_be32_put(buf + 10, auth_handle);
    tpm2_be32_put(buf + 14, nv_index);
    off = put_auth_area(buf, 18u, session_handle, 0, 0, 0, 0);
    tpm2_be16_put(buf + off, len); off += 2u;
    for (i = 0; i < len; i++) buf[off + i] = data[i];
    off += len;
    tpm2_be16_put(buf + off, offset); off += 2u;
    return off;
}

uint32_t tpm2_build_nv_read(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                            uint32_t nv_index, uint32_t session_handle,
                            uint16_t size, uint16_t offset)
{
    /* header(10) + authHandle(4) + nvIndex(4) + authArea(13) + size(2) + offset(2). */
    uint32_t total = 10u + 4u + 4u + 13u + 2u + 2u;
    uint32_t off;
    if (!buf || cap < total || size == 0u || size > TPM_NV_MAX_DATA)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_NV_READ);
    tpm2_be32_put(buf + 10, auth_handle);
    tpm2_be32_put(buf + 14, nv_index);
    off = put_auth_area(buf, 18u, session_handle, 0, 0, 0, 0);
    tpm2_be16_put(buf + off, size); off += 2u;
    tpm2_be16_put(buf + off, offset); off += 2u;
    return off;
}

int tpm2_parse_nv_read(const uint8_t *rsp, uint32_t len,
                       uint8_t *out, uint32_t out_cap)
{
    uint32_t poff, plen, i;
    uint16_t dsz;
    if (!out || tpm2_rsp_params(rsp, len, &poff, &plen) != 0)
        return -1;
    /* parameters: TPM2B_MAX_NV_BUFFER = size(2) + data, and NOTHING else.
     * Require exact consumption (2 + dsz == plen) so a malformed rc-success
     * cannot hide extra parameter bytes and still be reported as OK. */
    if (plen < 2u)
        return -1;
    dsz = tpm2_be16_get(rsp + poff);
    if ((uint32_t)dsz != plen - 2u || (uint32_t)dsz > out_cap)
        return -1;
    for (i = 0; i < dsz; i++)
        out[i] = rsp[poff + 2u + i];
    return (int)dsz;
}

uint32_t tpm2_build_nv_read_public(uint8_t *buf, uint32_t cap, uint32_t nv_index)
{
    /* header(10, ST_NO_SESSIONS) + nvIndex(4). */
    if (!buf || cap < 14u)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, 14u);
    tpm2_be32_put(buf + 6, TPM2_CC_NV_READ_PUBLIC);
    tpm2_be32_put(buf + 10, nv_index);
    return 14u;
}

int tpm2_parse_nv_read_public(const uint8_t *rsp, uint32_t len, uint32_t nv_index,
                              uint16_t *out_size, uint32_t *out_attrs)
{
    uint32_t poff, plen, base;
    uint16_t pubsize, policy_len, dsz, name_len;
    /* params: TPM2B_NV_PUBLIC{ size(2) + TPMS_NV_PUBLIC } + TPM2B_NAME{ size(2)
     * + name }, and NOTHING else. */
    if (tpm2_rsp_params(rsp, len, &poff, &plen) != 0 || plen < 2u)
        return -1;
    pubsize = tpm2_be16_get(rsp + poff);
    if ((uint32_t)pubsize > plen - 2u)
        return -1;
    /* TPMS_NV_PUBLIC: nvIndex(4) + nameAlg(2) + attributes(4) + authPolicy
     * TPM2B(2 + N) + dataSize(2). Minimum is 14 (empty policy). */
    if (pubsize < 14u)
        return -1;
    base = poff + 2u;                         /* start of TPMS_NV_PUBLIC */
    /* Bind the public area to the REQUESTED index: a desynchronized/malicious
     * TPM that echoes well-formed metadata for a DIFFERENT nvIndex must not be
     * reported as success for the index the caller asked about. */
    if (tpm2_be32_get(rsp + base) != nv_index)
        return -1;
    policy_len = tpm2_be16_get(rsp + base + 10u);
    /* authPolicy + dataSize must fit inside pubsize. */
    if ((uint32_t)policy_len > (uint32_t)pubsize - 12u - 2u)
        return -1;
    dsz = tpm2_be16_get(rsp + base + 12u + policy_len);
    /* Trailing TPM2B_NAME and exact parameter consumption: a malformed success
     * with extra bytes or a missing name must be rejected. */
    if (plen < 2u + (uint32_t)pubsize + 2u)
        return -1;
    name_len = tpm2_be16_get(rsp + poff + 2u + pubsize);
    if (2u + (uint32_t)pubsize + 2u + (uint32_t)name_len != plen)
        return -1;
    if (out_attrs) *out_attrs = tpm2_be32_get(rsp + base + 6u);
    if (out_size)  *out_size = dsz;
    return 0;
}

uint32_t tpm2_build_start_auth_session(uint8_t *buf, uint32_t cap,
                                       uint8_t session_type, uint16_t auth_hash,
                                       const uint8_t *nonce_caller, uint16_t nonce_len)
{
    /* header(10) + tpmKey(4) + bind(4) + nonceCaller TPM2B(2 + nonce_len)
     * + encryptedSalt TPM2B(2, 0) + sessionType(1) + symmetric(2: TPM_ALG_NULL)
     * + authHash(2). */
    uint32_t total = 10u + 4u + 4u + (2u + (uint32_t)nonce_len) + 2u + 1u + 2u + 2u;
    uint32_t off, i;
    if (!buf || cap < total || nonce_len == 0u || !nonce_caller)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_START_AUTH_SESSION);
    tpm2_be32_put(buf + 10, TPM_RH_NULL);   /* tpmKey (no salt) */
    tpm2_be32_put(buf + 14, TPM_RH_NULL);   /* bind */
    off = 18u;
    tpm2_be16_put(buf + off, nonce_len); off += 2u;
    for (i = 0; i < nonce_len; i++) buf[off + i] = nonce_caller[i];
    off += nonce_len;
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* encryptedSalt empty */
    buf[off] = session_type; off += 1u;               /* sessionType */
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;/* symmetric.algorithm */
    tpm2_be16_put(buf + off, auth_hash); off += 2u;   /* authHash */
    return off;
}

uint32_t tpm2_parse_start_auth_session(const uint8_t *rsp, uint32_t len)
{
    uint32_t poff, plen, handle;
    uint16_t nonce_len;
    uint8_t ht;
    /* params: sessionHandle(4) + nonceTPM TPM2B(2 + N), and NOTHING else.
     * Require EXACT consumption (no trailing bytes) and a real SESSION-type
     * handle -- a corrupt rc-success reply naming a transient object handle
     * (e.g. 0x80...) with a well-formed nonce must NOT be accepted, since the
     * caller later FlushContext's whatever handle this returns (flushing a
     * non-session object would evict unrelated TPM state). */
    if (tpm2_rsp_params(rsp, len, &poff, &plen) != 0 || plen < 6u)
        return 0;
    nonce_len = tpm2_be16_get(rsp + poff + 4u);
    if ((uint32_t)nonce_len != plen - 6u)   /* exact: 6 + nonce_len == plen */
        return 0;
    handle = tpm2_be32_get(rsp + poff);
    ht = (uint8_t)(handle >> 24);
    if (ht != 0x02u && ht != 0x03u)   /* HMAC / POLICY session types only */
        return 0;
    return handle;
}

uint32_t tpm2_rsp_session_handle(const uint8_t *rsp, uint32_t len)
{
    uint32_t poff, plen, handle;
    uint8_t ht;
    /* Raw handle only -- no nonceTPM validation (cleanup path). */
    if (tpm2_rsp_params(rsp, len, &poff, &plen) != 0 || plen < 4u)
        return 0;
    handle = tpm2_be32_get(rsp + poff);
    /* Only flush a SESSION-type handle (TPM_HT high byte 0x02 HMAC / 0x03
     * POLICY -- the only types StartAuthSession allocates). A corrupt reply
     * naming a transient OBJECT handle (0x80) must NOT be flushed, since
     * FlushContext is valid for transient objects and would evict unrelated
     * TPM state rather than clean up this session. */
    ht = (uint8_t)(handle >> 24);
    if (ht != 0x02u && ht != 0x03u)
        return 0;
    return handle;
}

uint32_t tpm2_build_policy_pcr(uint8_t *buf, uint32_t cap, uint32_t policy_session,
                               uint16_t alg, const uint8_t pcr_select[3])
{
    /* header(10) + policySession(4) + pcrDigest TPM2B(2, 0)
     * + TPML_PCR_SELECTION{ count(4)=1 + hashAlg(2) + sizeofSelect(1)=3
     *   + pcrSelect[3] }. */
    uint32_t total = 10u + 4u + 2u + 4u + 2u + 1u + 3u;
    uint32_t off;
    if (!buf || cap < total || !pcr_select)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_POLICY_PCR);
    tpm2_be32_put(buf + 10, policy_session);
    off = 14u;
    tpm2_be16_put(buf + off, 0u); off += 2u;   /* pcrDigest empty -> TPM uses current */
    tpm2_be32_put(buf + off, 1u); off += 4u;   /* count = 1 */
    tpm2_be16_put(buf + off, alg); off += 2u;  /* hashAlg */
    buf[off] = 3u; off += 1u;                  /* sizeofSelect (PCR 0..23) */
    buf[off] = pcr_select[0];
    buf[off + 1u] = pcr_select[1];
    buf[off + 2u] = pcr_select[2];
    off += 3u;
    return off;
}

uint32_t tpm2_build_policy_get_digest(uint8_t *buf, uint32_t cap,
                                      uint32_t policy_session)
{
    if (!buf || cap < 14u)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, 14u);
    tpm2_be32_put(buf + 6, TPM2_CC_POLICY_GET_DIGEST);
    tpm2_be32_put(buf + 10, policy_session);
    return 14u;
}

int tpm2_parse_policy_get_digest(const uint8_t *rsp, uint32_t len,
                                 uint8_t *out, uint32_t out_cap)
{
    uint32_t poff, plen, i;
    uint16_t dsz;
    if (!out || tpm2_rsp_params(rsp, len, &poff, &plen) != 0 || plen < 2u)
        return -1;
    dsz = tpm2_be16_get(rsp + poff);
    /* policyDigest TPM2B is the only parameter: exact consumption. */
    if (dsz == 0u || (uint32_t)dsz != plen - 2u || (uint32_t)dsz > out_cap)
        return -1;
    for (i = 0; i < dsz; i++)
        out[i] = rsp[poff + 2u + i];
    return (int)dsz;
}

uint32_t tpm2_build_flush_context(uint8_t *buf, uint32_t cap, uint32_t handle)
{
    /* TPM2_FlushContext: tag ST_NO_SESSIONS, NO handle area; flushHandle is a
     * parameter (it can name a transient OR session handle). */
    if (!buf || cap < 14u)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(buf + 2, 14u);
    tpm2_be32_put(buf + 6, TPM2_CC_FLUSH_CONTEXT);
    tpm2_be32_put(buf + 10, handle);
    return 14u;
}

void tpm_pcr_mask_to_select(uint32_t mask, uint8_t out_sel[3])
{
    /* bit i of the mask == PCR i; the 3-byte select covers PCRs 0..23. */
    out_sel[0] = (uint8_t)(mask & 0xFFu);
    out_sel[1] = (uint8_t)((mask >> 8) & 0xFFu);
    out_sel[2] = (uint8_t)((mask >> 16) & 0xFFu);
}

void tpm_nv_baseline_pcr_select(uint8_t out_sel[3])
{
    /* Derive the PCR selection from the allocation table's baseline mask -- never
     * a hard-coded set, so the policy tracks the table. */
    tpm_pcr_mask_to_select(tpm_pcr_baseline_mask(), out_sel);
}

/* ---- Transport drivers (Phase-1; not ISR-safe) ---- */

/* Validate the trailing one-session TPMS_AUTH_RESPONSE of a session-tagged
 * response: nonceTPM TPM2B(2 + N) + sessionAttributes(1) + hmac TPM2B(2 + M),
 * starting at auth_off, consuming the response EXACTLY to `size` (an NV command
 * carries exactly one session). Each TPM2B size is bounds-checked against the
 * declared response size, so a crafted auth area that claims bytes past the
 * response is rejected. Returns 1 if well-formed, 0 otherwise. */
/* TPM2B_NONCE and TPM2B_AUTH both wrap TPMU_HA (a hash digest); the largest
 * defined hash is SHA-512, so a spec-valid nonce or hmac is at most 64 bytes.
 * A length that fits inside the response but exceeds this is malformed. */
#define TPM2B_HA_MAX 64u

int tpm_session_auth_response_ok(const uint8_t *rsp, uint32_t size, uint32_t auth_off)
{
    uint32_t off = auth_off;
    uint16_t nonce_n, hmac_n;
    if (size < off + 2u) return 0;
    nonce_n = tpm2_be16_get(rsp + off); off += 2u;
    if ((uint32_t)nonce_n > TPM2B_HA_MAX || (uint32_t)nonce_n > size - off) return 0;
    off += nonce_n;
    if (size < off + 1u) return 0;          /* sessionAttributes */
    off += 1u;
    if (size < off + 2u) return 0;
    hmac_n = tpm2_be16_get(rsp + off); off += 2u;
    if ((uint32_t)hmac_n > TPM2B_HA_MAX || (uint32_t)hmac_n > size - off) return 0;
    off += hmac_n;
    return (off == size) ? 1 : 0;           /* exactly one session, no trailing bytes */
}

/* Submit one command and classify the response code. On TPM_NV_OK leaves rsp
 * intact (and sets *out_rlen) for the caller to parse. */
int tpm_session_cmd_exec(const uint8_t *cmd, uint32_t n, uint8_t *rsp, uint32_t cap,
                   uint32_t *out_rlen, tpm_nv_status_t *out_st, uint32_t *out_rc)
{
    int r;
    uint16_t tag;
    uint32_t rc, size;
    r = tpm2_submit(cmd, n, rsp, cap);
    if (r < 0) {
        *out_st = (r == TPM_T_ERR_BUSY) ? TPM_NV_BUSY : TPM_NV_TRANSPORT;
        return -1;
    }
    if (tpm2_rsp_parse(rsp, (uint32_t)r, &tag, &size, &rc) != 0) {
        *out_st = TPM_NV_TRANSPORT;
        return -1;
    }
    if (out_rc) *out_rc = rc;
    if (rc != TPM2_RC_SUCCESS) {
        *out_st = tpm_nv_classify_rc(rc);
        return -1;
    }
    /* A SUCCESS response to a session-authorized command (NV define/write/
     * undefine/read, Create, Unseal -- any command tagged ST_SESSIONS) MUST be
     * ST_SESSIONS and carry a well-formed parameterSize AND a one-session
     * response auth area (minimal TPMS_AUTH_RESPONSE = nonceTPM(2,0) +
     * sessionAttributes(1) + hmac(2,0) = 5 bytes). Key the requirement off the
     * COMMAND tag, not the response tag: a forged/degraded ST_NO_SESSIONS
     * rc-success would otherwise skip auth validation entirely and let the
     * caller's parser read attacker-controlled parameters from offset 10
     * (tpm2_rsp_params accepts ST_NO_SESSIONS params at the fixed offset) --
     * catastrophic after an irreversible NV write or when returning a sealed
     * secret. tpm2_rsp_parse only validates the header.
     *
     * SCOPE: this validates response STRUCTURE (tag, parameterSize bound, a
     * well-formed one-session TPMS_AUTH_RESPONSE), which rejects malformed /
     * truncated / wrong-shape responses (the realistic local failure: bus
     * glitch, firmware desync). It does NOT cryptographically AUTHENTICATE the
     * response: these are unsalted password/policy sessions with no HMAC over
     * the response, so a physical bus interposer that forges a well-formed
     * ST_SESSIONS success is NOT defended here -- that needs salted/bound HMAC
     * sessions + parameter encryption (BitLocker-style bus protection), a
     * separate transport-wide feature tracked as a TODO-13 section-8 follow-up. */
    if (tpm2_be16_get(cmd) == TPM2_ST_SESSIONS) {
        uint32_t poff, plen;
        if (tag != TPM2_ST_SESSIONS ||
            tpm2_rsp_params(rsp, (uint32_t)r, &poff, &plen) != 0 ||
            !tpm_session_auth_response_ok(rsp, size, poff + plen)) {
            *out_st = TPM_NV_TRANSPORT;
            return -1;
        }
    }
    if (out_rlen) *out_rlen = (uint32_t)r;
    *out_st = TPM_NV_OK;
    return 0;
}

/* The single teardown for every started session. FlushContext must not leak the
 * session on transient contention: if another transaction holds the transport,
 * an immediate-BUSY submit would abandon the handle (eroding the TPM's small
 * session pool until reboot). tpm2_submit_waiting WAITS up to a bounded budget
 * for the gate to clear -- waiting out a real ms-scale transaction that a tight
 * retry could not. A permanently wedged transport is unrecoverable either way;
 * the result is otherwise ignored (best-effort cleanup). */
static void nv_flush(uint32_t handle)
{
    uint8_t cmd[16], rsp[16];
    uint32_t n = tpm2_build_flush_context(cmd, sizeof cmd, handle);
    if (n == 0u)
        return;
    (void)tpm2_submit_waiting(cmd, n, rsp, sizeof rsp, TPM_NV_FLUSH_BUDGET_MS);
}

/* ---- Shared policy-session seam (single-cleanup; reused by NV + sealed secrets) ---- */

tpm_nv_status_t tpm_policy_pcr_digest(uint16_t alg, const uint8_t sel[3],
                                      uint8_t *out, uint32_t cap)
{
    uint8_t cmd[64], rsp[128], nonce[16];
    uint32_t session, n, rlen = 0;
    tpm_nv_status_t st;

    if (!out || !sel || cap < 32u)
        return TPM_NV_BADARG;
    /* nonceCaller is not security-relevant for a trial policy digest. */
    memset(nonce, 0xA5, sizeof nonce);
    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_TRIAL,
                                      TPM_ALG_SHA256, nonce, sizeof nonce);
    if (n == 0u)
        return TPM_NV_BADARG;
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        return st;
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        /* rc was SUCCESS but the response structure is malformed. The TPM may
         * still have allocated a session handle -- recover + best-effort flush. */
        uint32_t raw = tpm2_rsp_session_handle(rsp, rlen);
        if (raw != 0u)
            nv_flush(raw);
        return TPM_NV_TRANSPORT;
    }
    /* From here ALL exits flush `session`. */
    n = tpm2_build_policy_pcr(cmd, sizeof cmd, session, alg, sel);
    if (n == 0u) { st = TPM_NV_BADARG; goto out; }
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        goto out;
    n = tpm2_build_policy_get_digest(cmd, sizeof cmd, session);
    if (n == 0u) { st = TPM_NV_BADARG; goto out; }
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        goto out;
    st = (tpm2_parse_policy_get_digest(rsp, rlen, out, cap) == 32)
             ? TPM_NV_OK : TPM_NV_TRANSPORT;
out:
    nv_flush(session);
    return st;
}

tpm_nv_status_t tpm_policy_session_run(uint16_t alg, const uint8_t sel[3],
                                       tpm_policy_op_fn op, void *ctx)
{
    uint8_t cmd[64], rsp[160], nonce[16];
    uint32_t session, n, rlen = 0;
    tpm_nv_status_t st;

    if (!sel || !op)
        return TPM_NV_BADARG;
    memset(nonce, 0xA5, sizeof nonce);
    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_POLICY,
                                      TPM_ALG_SHA256, nonce, sizeof nonce);
    if (n == 0u)
        return TPM_NV_BADARG;
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        return st;
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        uint32_t raw = tpm2_rsp_session_handle(rsp, rlen);
        if (raw != 0u)
            nv_flush(raw);
        return TPM_NV_TRANSPORT;
    }
    /* From here ALL exits flush `session` (including op errors). */
    n = tpm2_build_policy_pcr(cmd, sizeof cmd, session, alg, sel);
    if (n == 0u) { st = TPM_NV_BADARG; goto out; }
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        goto out;
    st = op(session, ctx);
out:
    nv_flush(session);
    return st;
}

/* ---- NV policy ops over the shared seam ---- */

/* Baseline authPolicy: a trial PolicyPCR digest over the baseline mask. */
static tpm_nv_status_t nv_compute_baseline_policy(uint8_t *out, uint32_t cap)
{
    uint8_t sel[3];
    tpm_nv_baseline_pcr_select(sel);
    return tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, out, cap);
}

struct nv_policy_ctx {
    uint32_t nv_index;
    int      is_write;
    uint16_t offset;
    const uint8_t *data;
    uint16_t in_len;
    uint8_t *out;
    uint16_t cap;
    uint16_t *out_len;
};

/* Run one NV read/write authorized by the supplied policy session. */
static tpm_nv_status_t nv_policy_op_cb(uint32_t session, void *vctx)
{
    struct nv_policy_ctx *c = (struct nv_policy_ctx *)vctx;
    /* rsp sized for a max-length NV_Read: header(10) + parameterSize(4) +
     * TPM2B_MAX_NV_BUFFER(2 + TPM_NV_MAX_DATA) + a full one-session auth area
     * (~69). +128 covers it; a larger response is rejected by tpm2_submit. */
    uint8_t cmd[TPM_NV_MAX_DATA + 64u];
    uint8_t rsp[TPM_NV_MAX_DATA + 128u];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;

    if (c->is_write)
        n = tpm2_build_nv_write(cmd, sizeof cmd, c->nv_index, c->nv_index, session,
                                c->offset, c->data, c->in_len);
    else
        n = tpm2_build_nv_read(cmd, sizeof cmd, c->nv_index, c->nv_index, session,
                               c->cap, c->offset);
    if (n == 0u)
        return TPM_NV_BADARG;
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        return st;
    if (!c->is_write) {
        int dl = tpm2_parse_nv_read(rsp, rlen, c->out, c->cap);
        if (dl < 0)
            return TPM_NV_TRANSPORT;
        if (c->out_len) *c->out_len = (uint16_t)dl;
    }
    return TPM_NV_OK;
}

static tpm_nv_status_t nv_policy_op(uint32_t nv_index, int is_write,
                                    uint16_t offset, const uint8_t *data,
                                    uint16_t in_len, uint8_t *out,
                                    uint16_t cap, uint16_t *out_len)
{
    uint8_t sel[3];
    struct nv_policy_ctx c = { nv_index, is_write, offset, data, in_len,
                               out, cap, out_len };
    tpm_nv_baseline_pcr_select(sel);
    return tpm_policy_session_run(TPM_ALG_SHA256, sel, nv_policy_op_cb, &c);
}

tpm_nv_status_t tpm_nv_define_data(uint32_t nv_index, uint16_t data_size)
{
    uint8_t cmd[64], rsp[32];
    uint32_t attrs = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA;
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    if (data_size == 0u || data_size > TPM_NV_MAX_DATA)
        return TPM_NV_BADARG;
    n = tpm2_build_nv_define(cmd, sizeof cmd, nv_index, attrs, TPM_ALG_SHA256,
                             0, 0, data_size);
    if (n == 0u)
        return TPM_NV_BADARG;
    (void)tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0);
    return st;
}

tpm_nv_status_t tpm_nv_define_baseline(uint32_t nv_index, uint16_t data_size)
{
    uint8_t policy[32];
    uint8_t cmd[96], rsp[32];
    uint32_t attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA;
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    if (data_size == 0u || data_size > TPM_NV_MAX_DATA)
        return TPM_NV_BADARG;
    st = nv_compute_baseline_policy(policy, sizeof policy);
    if (st != TPM_NV_OK)
        return st;
    n = tpm2_build_nv_define(cmd, sizeof cmd, nv_index, attrs, TPM_ALG_SHA256,
                             policy, (uint16_t)sizeof policy, data_size);
    if (n == 0u)
        return TPM_NV_BADARG;
    (void)tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0);
    return st;
}

tpm_nv_status_t tpm_nv_undefine(uint32_t nv_index)
{
    uint8_t cmd[40], rsp[32];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    n = tpm2_build_nv_undefine(cmd, sizeof cmd, nv_index);
    if (n == 0u)
        return TPM_NV_BADARG;
    (void)tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0);
    return st;
}

tpm_nv_status_t tpm_nv_write(uint32_t nv_index, uint16_t offset,
                             const uint8_t *data, uint16_t len)
{
    uint8_t cmd[TPM_NV_MAX_DATA + 64u], rsp[32];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    if (!data || len == 0u || len > TPM_NV_MAX_DATA)
        return TPM_NV_BADARG;
    n = tpm2_build_nv_write(cmd, sizeof cmd, TPM_RH_OWNER, nv_index, TPM_RS_PW,
                            offset, data, len);
    if (n == 0u)
        return TPM_NV_BADARG;
    (void)tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0);
    return st;
}

tpm_nv_status_t tpm_nv_read(uint32_t nv_index, uint16_t offset,
                            uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    uint8_t cmd[40], rsp[TPM_NV_MAX_DATA + 128u];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    int dl;
    if (out_len) *out_len = 0;
    if (!out || cap == 0u || cap > TPM_NV_MAX_DATA)
        return TPM_NV_BADARG;
    n = tpm2_build_nv_read(cmd, sizeof cmd, TPM_RH_OWNER, nv_index, TPM_RS_PW,
                           cap, offset);
    if (n == 0u)
        return TPM_NV_BADARG;
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        return st;
    dl = tpm2_parse_nv_read(rsp, rlen, out, cap);
    if (dl < 0)
        return TPM_NV_TRANSPORT;
    if (out_len) *out_len = (uint16_t)dl;
    return TPM_NV_OK;
}

tpm_nv_status_t tpm_nv_policy_write(uint32_t nv_index, uint16_t offset,
                                    const uint8_t *data, uint16_t len)
{
    if (!data || len == 0u || len > TPM_NV_MAX_DATA)
        return TPM_NV_BADARG;
    return nv_policy_op(nv_index, 1, offset, data, len, 0, 0, 0);
}

tpm_nv_status_t tpm_nv_policy_read(uint32_t nv_index, uint16_t offset,
                                   uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!out || cap == 0u || cap > TPM_NV_MAX_DATA)
        return TPM_NV_BADARG;
    return nv_policy_op(nv_index, 0, offset, 0, 0, out, cap, out_len);
}

tpm_nv_status_t tpm_nv_read_public(uint32_t nv_index, uint16_t *out_size,
                                   uint32_t *out_attrs)
{
    uint8_t cmd[16], rsp[128];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    n = tpm2_build_nv_read_public(cmd, sizeof cmd, nv_index);
    if (n == 0u)
        return TPM_NV_BADARG;
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        return st;
    if (tpm2_parse_nv_read_public(rsp, rlen, nv_index, out_size, out_attrs) != 0)
        return TPM_NV_TRANSPORT;
    return TPM_NV_OK;
}
