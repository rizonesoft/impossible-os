/* ============================================================================
 * tpm_nv.c -- TPM2 NV index storage + PCR-policy sessions (measured boot)
 *
 * Implements the NV storage MECHANISM for measured-boot attestation baselines.
 * Pure marshaling helpers (MMIO-free, fixture-tested) plus the tpm_nv_* wrappers
 * that drive the Phase-1 transport.
 *
 * Session lifetime discipline (single cleanup path): every wrapper that calls
 * TPM2_StartAuthSession routes ALL subsequent exits through one nv_flush(), so
 * the teardown is unconditional. It is bounded rather than guaranteed: the
 * flush retries and requires proof (SUCCESS, or "no such handle"), and where
 * proof never arrives -- or where a creation response never arrived and the
 * handle is therefore unknowable -- the leak is LOGGED and the transport is
 * left usable. See tpm_nv.h for why that beats disabling it. The trial/real
 * PolicyPCR flow is hand-rolled here, not at the transport seam.
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
#include "kernel/klog.h"
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
        /* TPM_RC_ATTRIBUTES on the nvIndex handle is how a TPM reports "this
         * operation does not suit this index TYPE" -- an NV_Increment against
         * an ordinary index, or an NV_Write against a counter. Without its own
         * status it fell into TPMERR and read as a generic device fault. */
        case TPM2_RC_F1_ATTRIBUTES:  return TPM_NV_ATTRS;
        default:                     return TPM_NV_TPMERR;
    }
}

/* ---- Attribute validation (pure; no transport) ---- */

tpm_nv_status_t tpm_nv_attrs_valid(uint32_t attrs, uint16_t data_size,
                                   uint16_t name_alg)
{
    uint32_t nt = TPMA_NV_GET_TYPE(attrs);
    uint32_t digest_len;

    if (attrs & TPMA_NV_RESERVED_MASK)
        return TPM_NV_ATTRS;
    /* The TPM maintains these; a define cannot assert them. */
    if (attrs & TPMA_NV_STATUS_MASK)
        return TPM_NV_ATTRS;
    if ((attrs & TPMA_NV_READ_AUTH_MASK) == 0u ||
        (attrs & TPMA_NV_WRITE_AUTH_MASK) == 0u)
        return TPM_NV_ATTRS;
    /* Every define this module builds is authorized with TPM_RH_OWNER, and both
     * of these attributes require the PLATFORM hierarchy: POLICY_DELETE is
     * refused with TPM_RC_ATTRIBUTES under owner auth, and PLATFORMCREATE
     * describes an index the platform created, which by construction this is
     * not. Accepting them locally would mean the "validated" request still
     * fails, on real firmware only. */
    if (attrs & (TPMA_NV_POLICY_DELETE | TPMA_NV_PLATFORMCREATE))
        return TPM_NV_ATTRS;

    switch (nt) {
        case TPM_NT_PIN_FAIL:
        case TPM_NT_PIN_PASS:
            /* NOT SUPPORTED by this module, and refused rather than
             * half-validated. A PIN index carries authorization rules beyond
             * dataSize (which write-auth bits are legal, whether NO_DA is
             * required), and no consumer here defines one. Accepting it after
             * checking only the size would be the module claiming a validation
             * it does not perform -- the caller would pass locally and fail on
             * real firmware. A future consumer adds the rules with its own spec
             * verification and its own tests. */
            return TPM_NV_ATTRS;
        case TPM_NT_COUNTER:
        case TPM_NT_BITS:
            /* Both hold exactly one 8-byte value (a UINT64 counter, or a bit
             * field). */
            if (data_size != 8u)
                return TPM_NV_ATTRS;
            /* WRITEALL means "a partial write is refused", which is meaningless
             * for an index written by Increment or SetBits rather than by
             * NV_Write. Refused rather than passed through: no consumer here
             * needs it, and the alternative is a define that validates locally
             * and is then refused by firmware -- the exact failure this
             * validator exists to prevent. */
            if (attrs & TPMA_NV_WRITEALL)
                return TPM_NV_ATTRS;
            /* CLEAR_STCLEAR would let a TPM Reset return the index to its
             * pre-written state, which is the one property an anti-rollback
             * counter must not have. */
            if (nt == TPM_NT_COUNTER && (attrs & TPMA_NV_CLEAR_STCLEAR))
                return TPM_NV_ATTRS;
            return TPM_NV_OK;
        case TPM_NT_EXTEND:
            /* An EXTEND index holds one digest in its OWN nameAlg. */
            digest_len = tpm_alg_digest_len_pub(name_alg);
            if (digest_len == 0u || (uint32_t)data_size != digest_len)
                return TPM_NV_ATTRS;
            return TPM_NV_OK;
        case TPM_NT_ORDINARY:
            /* Bounded by the DEFINITION limit, not the per-transfer one:
             * tpm_nv_read/write chunk by offset past TPM_NV_MAX_DATA, so an
             * index larger than one transfer is legitimate and must stay
             * definable. Conflating the two made a chunk-readable index
             * unprovisionable. */
            if (data_size == 0u || (uint32_t)data_size > TPM_NV_MAX_INDEX_SIZE)
                return TPM_NV_ATTRS;
            return TPM_NV_OK;
        default:
            /* An undefined TPM_NT value: bits 4-7 are a field, not spare flags. */
            return TPM_NV_ATTRS;
    }
}

/* ---- Marshaling helpers ---- */

/* Append a one-session authorization area at off. Layout:
 *   authorizationSize(4) + sessionHandle(4) + nonce TPM2B(2 + nonce_len)
 *   + sessionAttributes(1) + hmac/password TPM2B(2 + auth_len).
 * continueSession (attrs bit 0) is set ONLY for a real HMAC/POLICY session (high
 * byte 0x02/0x03), keeping it alive for our explicit FlushContext. For the
 * permanent password handle TPM_RS_PW the attribute is 0x00: a password
 * authorization is not a savable session, so continueSession is meaningless and
 * a strict TPM can reject it as an attributes error (a bare-metal-only failure).
 * Returns the new offset. The caller guarantees buffer room. */
static uint32_t put_auth_area(uint8_t *buf, uint32_t off, uint32_t session,
                              const uint8_t *nonce, uint16_t nonce_len,
                              const uint8_t *auth, uint16_t auth_len)
{
    uint16_t i;
    uint8_t ht = (uint8_t)(session >> 24);
    uint32_t area = 4u + 2u + (uint32_t)nonce_len + 1u + 2u + (uint32_t)auth_len;
    tpm2_be32_put(buf + off, area); off += 4u;       /* authorizationSize */
    tpm2_be32_put(buf + off, session); off += 4u;    /* sessionHandle */
    tpm2_be16_put(buf + off, nonce_len); off += 2u;  /* nonce size */
    for (i = 0; i < nonce_len; i++) buf[off + i] = nonce ? nonce[i] : 0u;
    off += nonce_len;
    buf[off] = (ht == 0x02u || ht == 0x03u) ? 0x01u : 0u; off += 1u; /* attrs */
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
    /* Defense in depth: the wrappers validate first so they can report
     * TPM_NV_ATTRS by name, but no caller gets to marshal an illegal shape --
     * including a DIRECT caller of this builder, which is an exported symbol
     * and not only the wrappers' private back end. */
    if (tpm_nv_attrs_valid(attrs, data_size, name_alg) != TPM_NV_OK)
        return 0;
    /* An authPolicy is absent or exactly one nameAlg digest. Enforced here as
     * well as in tpm_nv_define_ex so the request path cannot be bypassed by
     * building the command directly. */
    if (auth_policy_len != 0u &&
        (uint32_t)auth_policy_len != tpm_alg_digest_len_pub(name_alg))
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

/* NV_Increment and NV_WriteLock share one wire shape: two handles (authHandle
 * carrying the session, then nvIndex) plus a one-session auth area and no
 * parameters. One builder, two command codes -- the alternative is two
 * near-identical functions whose auth areas drift apart. */
static uint32_t nv_build_handles_only(uint8_t *buf, uint32_t cap, uint32_t cc,
                                      uint32_t auth_handle, uint32_t nv_index,
                                      uint32_t session_handle)
{
    /* header(10) + authHandle(4) + nvIndex(4) + authArea(13). */
    uint32_t total = 10u + 4u + 4u + 13u;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, cc);
    tpm2_be32_put(buf + 10, auth_handle);
    tpm2_be32_put(buf + 14, nv_index);
    (void)put_auth_area(buf, 18u, session_handle, 0, 0, 0, 0);
    return total;
}

uint32_t tpm2_build_nv_increment(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                                 uint32_t nv_index, uint32_t session_handle)
{
    return nv_build_handles_only(buf, cap, TPM2_CC_NV_INCREMENT,
                                 auth_handle, nv_index, session_handle);
}

uint32_t tpm2_build_nv_write_lock(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                                  uint32_t nv_index, uint32_t session_handle)
{
    return nv_build_handles_only(buf, cap, TPM2_CC_NV_WRITE_LOCK,
                                 auth_handle, nv_index, session_handle);
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
     * TPM2B(2 + N) + dataSize(2). The structure is FIXED at 14 + policy, so
     * require an exact size rather than a minimum: a larger pubsize means
     * unaccounted bytes sit between dataSize and the trailing TPM2B_NAME, and
     * because nothing reads them they would sail through both parsers and be
     * accepted by the definition-identity check as an exact match. */
    if (pubsize < 14u)
        return -1;
    if ((uint32_t)pubsize != 14u + (uint32_t)tpm2_be16_get(rsp + poff + 2u + 10u))
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

int tpm2_parse_nv_public_full(const uint8_t *rsp, uint32_t len, uint32_t nv_index,
                              struct tpm_nv_public *out)
{
    uint32_t poff, plen, base;
    uint16_t pubsize, policy_len, i;

    /* TRANSACTIONAL: everything lands in a local first and is copied out only
     * once every check has passed. Filling `out` as we go left a rejected parse
     * having already written dataSize, attrs and nameAlg, so "the output is
     * untouched on failure" was false -- and a caller that trusted a partially
     * written struct after a -1 would be comparing against attacker-influenced
     * fields. */
    struct tpm_nv_public tmp;

    if (!out)
        return -1;
    /* Re-derive the offsets rather than duplicating the validation: the strict
     * parser has already rejected every malformed shape by the time it returns,
     * so a success here is over a bounds-checked public area. */
    if (tpm2_parse_nv_read_public(rsp, len, nv_index, &tmp.data_size,
                                  &tmp.attrs) != 0)
        return -1;
    if (tpm2_rsp_params(rsp, len, &poff, &plen) != 0 || plen < 2u)
        return -1;
    pubsize = tpm2_be16_get(rsp + poff);
    base = poff + 2u;
    tmp.name_alg = tpm2_be16_get(rsp + base + 4u);
    policy_len = tpm2_be16_get(rsp + base + 10u);
    /* Bounded inside pubsize by the strict parser, and bounded against our own
     * buffer so an oversized authPolicy is a refusal rather than a truncation
     * we would then compare as equal. */
    if ((uint32_t)policy_len > sizeof tmp.auth_policy ||
        (uint32_t)policy_len > (uint32_t)pubsize - 12u - 2u)
        return -1;
    /* An authPolicy is either ABSENT or exactly one nameAlg digest -- it is a
     * policy digest, so no other length is meaningful. Accepting an odd length
     * would let a fixture or a desynchronized TPM present a shape real hardware
     * never produces, and every comparison downstream would treat it as real. */
    if (policy_len != 0u &&
        (uint32_t)policy_len != tpm_alg_digest_len_pub(tmp.name_alg))
        return -1;
    tmp.policy_len = policy_len;
    for (i = 0; i < sizeof tmp.auth_policy; i++)
        tmp.auth_policy[i] = (i < policy_len) ? rsp[base + 12u + i] : 0u;
    *out = tmp;
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
    return tpm_session_cmd_exec_seq(0, cmd, n, rsp, cap, out_rlen, out_st, out_rc);
}

static int nv_cmd_exec_common(tpm2_seq_t seq, int teardown,
                              const uint8_t *cmd, uint32_t n, uint8_t *rsp,
                              uint32_t cap, uint32_t *out_rlen,
                              tpm_nv_status_t *out_st, uint32_t *out_rc);

int tpm_session_cmd_exec_seq(tpm2_seq_t seq,
                             const uint8_t *cmd, uint32_t n, uint8_t *rsp,
                             uint32_t cap, uint32_t *out_rlen,
                             tpm_nv_status_t *out_st, uint32_t *out_rc)
{
    return nv_cmd_exec_common(seq, 0, cmd, n, rsp, cap, out_rlen, out_st, out_rc);
}

/* Same, but submitted on the sequence's teardown allowance. */
static int tpm_session_cmd_exec_teardown(tpm2_seq_t seq,
                                         const uint8_t *cmd, uint32_t n,
                                         uint8_t *rsp, uint32_t cap,
                                         uint32_t *out_rlen,
                                         tpm_nv_status_t *out_st, uint32_t *out_rc)
{
    return nv_cmd_exec_common(seq, 1, cmd, n, rsp, cap, out_rlen, out_st, out_rc);
}

static int nv_cmd_exec_common(tpm2_seq_t seq, int teardown,
                              const uint8_t *cmd, uint32_t n, uint8_t *rsp,
                              uint32_t cap, uint32_t *out_rlen,
                              tpm_nv_status_t *out_st, uint32_t *out_rc)
{
    int r;
    uint16_t tag;
    uint32_t rc, size;
    if (!seq)
        r = tpm2_submit(cmd, n, rsp, cap);
    else if (teardown)
        r = tpm2_submit_seq_teardown(seq, cmd, n, rsp, cap);
    else
        r = tpm2_submit_seq(seq, cmd, n, rsp, cap);
    if (r < 0) {
        switch (r) {
            case TPM_T_ERR_BUSY:   *out_st = TPM_NV_BUSY;   break;
            /* The device answered within its own timeouts; the OPERATION ran out
             * of the boot's patience. Reporting TRANSPORT here would tell the
             * caller the TPM misbehaved, which is a different remediation. */
            case TPM_T_ERR_BUDGET: *out_st = TPM_NV_BUDGET; break;
            default:               *out_st = TPM_NV_TRANSPORT; break;
        }
        return -1;
    }
    if (tpm2_rsp_parse(rsp, (uint32_t)r, &tag, &size, &rc) != 0) {
        *out_st = TPM_NV_TRANSPORT;
        return -1;
    }
    /* Report BOTH facts learned from a parsed response, not just the code: a
     * caller that has to judge the response ENVELOPE (nv_flush does, because
     * FlushContext carries no auth area for the generic check to validate)
     * needs the received length on the error path too, and previously got 0. */
    if (out_rc) *out_rc = rc;
    if (out_rlen) *out_rlen = (uint32_t)r;
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

/* Sentinel for "tpm_session_cmd_exec_seq never parsed a response header", which
 * it signals by leaving *out_rc untouched. No real TPM response code can be
 * 0xFFFFFFFF (the format-1 encoding bounds it well below that). */
#define NV_RC_UNSET 0xFFFFFFFFu

/* 1 when a failed command definitely allocated nothing, so the transport is
 * safe to keep: the TPM returned a parsed response REFUSING it. Anything else
 * (no header parsed, or a structurally invalid rc-success) means the command
 * may have executed and its result is unknown. */
static int nv_outcome_is_definite_refusal(uint32_t raw_rc)
{
    return (raw_rc != NV_RC_UNSET && raw_rc != TPM2_RC_SUCCESS) ? 1 : 0;
}

/* The single teardown for every started session, and it VERIFIES itself.
 *
 * An unflushed handle erodes the TPM's small session pool until reboot, so
 * "best effort, result ignored" stopped being good enough once budget expiry
 * became recoverable: a flush abandoned mid-flight leaves the session allocated
 * while the transport stays usable. So the flush requires PROOF -- success, or
 * "no such handle", which is a definite answer meaning the session was already
 * released -- and retries within its budget before giving up.
 *
 * Contention is no longer a hazard here: the flush runs inside the sequence
 * that already owns the transport, so there is nothing to wait for and no BUSY
 * to lose the handle to. */
/* Every FlushContext response -- success OR error -- must look EXACTLY like
 * one: ST_NO_SESSIONS, a header, and nothing else. The generic executor cannot
 * check this, because it validates a response auth area only for commands
 * tagged ST_SESSIONS and FlushContext is not one.
 *
 * This gates BOTH proof paths, and that symmetry is the point. Guarding only
 * rc-SUCCESS leaves the "no such handle" answer just as forgeable: a
 * desynchronized ST_SESSIONS reply carrying TPM_RC_HANDLE parses fine,
 * classifies as NOTFOUND, and would end the teardown after one attempt with the
 * session still allocated. Both answers claim the session is gone, so both have
 * to be shaped like a real answer.
 *
 * Returns 1 and sets *out_rc when the envelope is exact; 0 otherwise. */
static int nv_flush_envelope_ok(const uint8_t *rsp, uint32_t rlen,
                                uint32_t *out_rc)
{
    uint16_t tag = 0;
    uint32_t size = 0, rc = 0;
    if (tpm2_rsp_parse(rsp, rlen, &tag, &size, &rc) != 0)
        return 0;
    if (tag != TPM2_ST_NO_SESSIONS || size != TPM2_RSP_HEADER_SIZE ||
        rlen != TPM2_RSP_HEADER_SIZE)
        return 0;
    if (out_rc) *out_rc = rc;
    return 1;
}

static void nv_flush(tpm2_seq_t seq, uint32_t handle)
{
    uint8_t cmd[16], rsp[16];
    uint32_t n = tpm2_build_flush_context(cmd, sizeof cmd, handle);
    uint32_t attempt;
    if (n == 0u)
        return;
    /* A teardown needs PROOF the session is gone, and only two answers give it:
     * SUCCESS, or "no such handle" (already released, which is the end state we
     * wanted). Every other parsed response is NOT proof -- TPM_RC_RETRY,
     * YIELDED and TESTING are transient warnings that leave the session exactly
     * where it was, so accepting the first non-success as "flushed" would leak
     * one session per operation.
     *
     * Rather than enumerate the transient warning codes (and risk getting one
     * wrong), retry a bounded number of times on anything unproven: a transient
     * error clears, and a permanent one costs two extra commands before
     * reaching the same conclusion. An unproven teardown is then REPORTED, not
     * escalated -- see the note at the end of this function. */
    for (attempt = 0; attempt < TPM_NV_FLUSH_RETRIES; attempt++) {
        uint32_t rlen = 0, raw_rc = NV_RC_UNSET;
        tpm_nv_status_t st;
        int exec_ok = (tpm_session_cmd_exec_teardown(seq, cmd, n, rsp, sizeof rsp,
                                                     &rlen, &st, &raw_rc) == 0);
        uint32_t env_rc = 0;
        int env_ok = nv_flush_envelope_ok(rsp, rlen, &env_rc);
        if (exec_ok && env_ok && env_rc == TPM2_RC_SUCCESS)
            return;                                   /* proven: session gone */
        if (!exec_ok && env_ok && st == TPM_NV_NOTFOUND)
            return;              /* proven already gone: the same end state */
        (void)raw_rc;
        /* Keep retrying while retrying can still ACHIEVE something. A malformed
         * or unparseable reply is exactly the transient case worth another
         * attempt -- FlushContext is idempotent, so a second try costs one
         * command and may well succeed. Only two conditions make further
         * attempts pointless: the ALLOWANCE is gone (reported as
         * TPM_NV_BUDGET), or the transport went sticky-failed and will refuse
         * every command from here.
         *
         * Deliberately NOT tpm2_seq_expired(): that reports the WORK budget,
         * which the teardown saves and restores around itself, so consulting it
         * would break the loop after one attempt in precisely the situation the
         * allowance exists for -- a spent work budget with a session still to
         * release. Each attempt re-arms the allowance, so exhaustion surfaces
         * as TPM_NV_BUDGET on its own. */
        if (st == TPM_NV_BUDGET || !tpm_transport_available())
            break;
    }

    /* Unproven: the session may still hold one of the TPM's few slots. REPORT
     * it; do not disable the transport.
     *
     * Disabling was tried and is the wrong trade. The leak degrades on its own
     * into a DEFINITE, classified error -- once the pool is exhausted,
     * StartAuthSession fails with a real response code that every caller
     * already handles -- so the failure is visible and terminal without any
     * help from us. Poisoning, by contrast, takes out the whole transport
     * immediately, including PCR reads and attestation that never open a
     * session at all, and it does not recover the leaked slot either. A louder
     * log and a narrower blast radius beats a self-inflicted outage. */
    klog(LOG_WARN, "TPM",
         "session 0x%x teardown unproven; a session slot may be held until reset",
         handle);
}

/* Decide what a FAILED TPM2_StartAuthSession means for the TPM's session pool.
 *
 * StartAuthSession is the one command in these flows whose abandonment can
 * leave a resource we cannot name: if the TPM executed it, a session occupies
 * one of its few slots and the handle we would need for FlushContext is in a
 * response we never successfully read.
 *
 * The distinction that matters is not WHICH error it was but whether the TPM
 * definitely allocated nothing:
 *   - a parsed response with rc != SUCCESS is a definite refusal. No session
 *     exists, so the transport stays usable.
 *   - anything else -- no header parsed at all, or a structurally invalid
 *     rc-success -- leaves the outcome unknown, and unknown is the case that
 *     must not be retried. */
static void nv_session_failure_guard(tpm2_seq_t seq, tpm_nv_status_t st,
                                     uint32_t raw_rc)
{
    (void)seq; (void)st;
    if (nv_outcome_is_definite_refusal(raw_rc))
        return;                       /* the TPM refused it; nothing allocated */
    /* Same trade as nv_flush(): an unnameable session is a leak that reports
     * itself once the pool runs out, and disabling the transport over it would
     * break unrelated TPM use without recovering anything. */
    klog(LOG_WARN, "TPM",
         "session creation outcome unknown; a session slot may be held until reset");
}

/* The budget one NV operation runs under. CONSTANT in production -- nothing
 * outside the KERNEL_TESTS seam below ever writes these, so a release build's
 * only concurrency question is a read of a never-changing value. The seam
 * itself assumes a single-threaded test context (the suite runs on the BSP);
 * it is not safe to call while another CPU has an operation in flight, and
 * nothing does. Mirrors the transport's own tpm_t_test_budget_iters seam. */
static uint32_t s_nv_work_ms    = TPM_NV_OP_BUDGET_MS;
static uint32_t s_nv_cleanup_ms = TPM_NV_OP_CLEANUP_BUDGET_MS;

#ifdef KERNEL_TESTS
void tpm_nv_test_set_op_budget(uint32_t work_ms, uint32_t cleanup_ms)
{
    s_nv_work_ms = work_ms;
    s_nv_cleanup_ms = cleanup_ms;
}

void tpm_nv_test_reset_op_budget(void)
{
    s_nv_work_ms = TPM_NV_OP_BUDGET_MS;
    s_nv_cleanup_ms = TPM_NV_OP_CLEANUP_BUDGET_MS;
}
#endif

/* Map a sequence that never STARTED onto a caller-facing status. */
static tpm_nv_status_t nv_seq_start_status(int rc)
{
    switch (rc) {
        case TPM_T_ERR_BUSY: return TPM_NV_BUSY;
        case TPM_T_ERR_ARG:  return TPM_NV_BADARG;
        default:             return TPM_NV_TRANSPORT;   /* NODEV / FAILED */
    }
}

/* ---- Bounded single-command execution ----
 * Every exported wrapper runs inside a sequence, even a one-command one, so the
 * section's cumulative bound actually covers the boot-path primitives rather
 * than only the multi-command policy flows. A single-command sequence costs one
 * gate acquire (which the unsequenced path took anyway) and buys the budget. */
struct nv_cmd_ctx {
    const uint8_t  *cmd;
    uint32_t        n;
    uint8_t        *rsp;
    uint32_t        cap;
    uint32_t        rlen;
    tpm_nv_status_t st;
};

static int nv_one_cmd_seq(tpm2_seq_t seq, void *vctx)
{
    struct nv_cmd_ctx *c = (struct nv_cmd_ctx *)vctx;
    (void)tpm_session_cmd_exec_seq(seq, c->cmd, c->n, c->rsp, c->cap,
                                   &c->rlen, &c->st, 0);
    return 0;
}

/* Run one command under the operation budget. *out_rlen (optional) receives the
 * response length on success. */
static tpm_nv_status_t nv_exec_bounded(const uint8_t *cmd, uint32_t n,
                                       uint8_t *rsp, uint32_t cap,
                                       uint32_t *out_rlen)
{
    struct nv_cmd_ctx c;
    int r;
    c.cmd = cmd; c.n = n; c.rsp = rsp; c.cap = cap;
    c.rlen = 0; c.st = TPM_NV_TRANSPORT;
    r = tpm2_seq_run(s_nv_work_ms, s_nv_cleanup_ms, nv_one_cmd_seq, &c);
    if (r != 0)
        return nv_seq_start_status(r);
    if (out_rlen) *out_rlen = c.rlen;
    return c.st;
}

/* ---- Shared policy-session seam (single-cleanup; reused by NV + sealed secrets) ----
 * Each flow runs as ONE bounded sequence: the transport gate is taken once and
 * every command inside shares a single cumulative budget, so a slow TPM cannot
 * spend a multi-second per-command timeout four times over in one logical
 * operation. tpm2_seq_run releases the gate on every path out, which is what
 * makes the goto-out cleanup style below safe to keep. */

struct nv_digest_ctx {
    uint16_t       alg;
    const uint8_t *sel;
    uint8_t       *out;
    uint32_t       cap;
    tpm_nv_status_t st;
};

/* The trial-session digest flow, running inside a sequence the CALLER owns.
 * Exposed separately so a larger operation (a baseline define) can compute the
 * policy and bind it without releasing the transport in between. */
static tpm_nv_status_t nv_policy_pcr_digest_in_seq(tpm2_seq_t seq, uint16_t alg,
                                                   const uint8_t sel[3],
                                                   uint8_t *out, uint32_t cap);

static int nv_policy_pcr_digest_seq(tpm2_seq_t seq, void *vctx)
{
    struct nv_digest_ctx *c = (struct nv_digest_ctx *)vctx;
    uint8_t cmd[64], rsp[128], nonce[16];
    uint32_t session, n, rlen = 0, sas_rc;
    tpm_nv_status_t st;

    /* nonceCaller is not security-relevant for a trial policy digest. */
    memset(nonce, 0xA5, sizeof nonce);
    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_TRIAL,
                                      TPM_ALG_SHA256, nonce, sizeof nonce);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    sas_rc = NV_RC_UNSET;
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st,
                                 &sas_rc) != 0) {
        nv_session_failure_guard(seq, st, sas_rc);
        c->st = st;
        return 0;
    }
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        /* rc was SUCCESS, so the TPM EXECUTED StartAuthSession and a session
         * almost certainly exists -- but the response is malformed, so the
         * strict parser will not hand us a handle to authorize with. Recover
         * the raw handle and flush it. If NO handle can be recovered the
         * session is unreachable, which is the second of the two leak
         * categories the header names; it is logged, like the first. */
        uint32_t raw = tpm2_rsp_session_handle(rsp, rlen);
        if (raw != 0u)
            nv_flush(seq, raw);
        else
            klog(LOG_WARN, "TPM",
                 "session created but its handle is unrecoverable; slot may be held");
        c->st = TPM_NV_TRANSPORT;
        return 0;
    }
    /* From here ALL exits flush `session`. */
    n = tpm2_build_policy_pcr(cmd, sizeof cmd, session, c->alg, c->sel);
    if (n == 0u) { st = TPM_NV_BADARG; goto out; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        goto out;
    n = tpm2_build_policy_get_digest(cmd, sizeof cmd, session);
    if (n == 0u) { st = TPM_NV_BADARG; goto out; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        goto out;
    st = (tpm2_parse_policy_get_digest(rsp, rlen, c->out, c->cap) == 32)
             ? TPM_NV_OK : TPM_NV_TRANSPORT;
out:
    nv_flush(seq, session);
    c->st = st;
    return 0;
}

static tpm_nv_status_t nv_policy_pcr_digest_in_seq(tpm2_seq_t seq, uint16_t alg,
                                                   const uint8_t sel[3],
                                                   uint8_t *out, uint32_t cap)
{
    struct nv_digest_ctx c;
    if (!out || !sel || cap < 32u)
        return TPM_NV_BADARG;
    c.alg = alg; c.sel = sel; c.out = out; c.cap = cap; c.st = TPM_NV_TRANSPORT;
    (void)nv_policy_pcr_digest_seq(seq, &c);
    return c.st;
}

tpm_nv_status_t tpm_policy_pcr_digest(uint16_t alg, const uint8_t sel[3],
                                      uint8_t *out, uint32_t cap)
{
    struct nv_digest_ctx c;
    int r;

    if (!out || !sel || cap < 32u)
        return TPM_NV_BADARG;
    c.alg = alg; c.sel = sel; c.out = out; c.cap = cap; c.st = TPM_NV_TRANSPORT;
    r = tpm2_seq_run(s_nv_work_ms, s_nv_cleanup_ms,
                     nv_policy_pcr_digest_seq, &c);
    return (r != 0) ? nv_seq_start_status(r) : c.st;
}

struct nv_session_run_ctx {
    uint16_t          alg;
    const uint8_t    *sel;
    tpm_policy_op_fn  op;
    void             *ctx;
    tpm_nv_status_t   st;
};

static int nv_policy_session_run_seq(tpm2_seq_t seq, void *vctx)
{
    struct nv_session_run_ctx *c = (struct nv_session_run_ctx *)vctx;
    uint8_t cmd[64], rsp[160], nonce[16];
    uint32_t session, n, rlen = 0, sas_rc;
    tpm_nv_status_t st;

    memset(nonce, 0xA5, sizeof nonce);
    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_POLICY,
                                      TPM_ALG_SHA256, nonce, sizeof nonce);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    sas_rc = NV_RC_UNSET;
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st,
                                 &sas_rc) != 0) {
        nv_session_failure_guard(seq, st, sas_rc);
        c->st = st;
        return 0;
    }
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        /* rc was SUCCESS, so the TPM EXECUTED StartAuthSession and a session
         * almost certainly exists -- but the response is malformed, so the
         * strict parser will not hand us a handle to authorize with. Recover
         * the raw handle and flush it. If NO handle can be recovered the
         * session is unreachable, which is the second of the two leak
         * categories the header names; it is logged, like the first. */
        uint32_t raw = tpm2_rsp_session_handle(rsp, rlen);
        if (raw != 0u)
            nv_flush(seq, raw);
        else
            klog(LOG_WARN, "TPM",
                 "session created but its handle is unrecoverable; slot may be held");
        c->st = TPM_NV_TRANSPORT;
        return 0;
    }
    /* From here ALL exits flush `session` (including op errors). */
    n = tpm2_build_policy_pcr(cmd, sizeof cmd, session, c->alg, c->sel);
    if (n == 0u) { st = TPM_NV_BADARG; goto out; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        goto out;
    st = c->op(seq, session, c->ctx);
out:
    nv_flush(seq, session);
    c->st = st;
    return 0;
}

tpm_nv_status_t tpm_policy_session_run(uint16_t alg, const uint8_t sel[3],
                                       tpm_policy_op_fn op, void *ctx)
{
    struct nv_session_run_ctx c;
    int r;

    if (!sel || !op)
        return TPM_NV_BADARG;
    c.alg = alg; c.sel = sel; c.op = op; c.ctx = ctx; c.st = TPM_NV_TRANSPORT;
    r = tpm2_seq_run(s_nv_work_ms, s_nv_cleanup_ms,
                     nv_policy_session_run_seq, &c);
    return (r != 0) ? nv_seq_start_status(r) : c.st;
}

/* ---- NV policy ops over the shared seam ---- */

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
static tpm_nv_status_t nv_policy_op_cb(tpm2_seq_t seq, uint32_t session, void *vctx)
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
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
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

/* An index the TPM reports as already DEFINED is only reusable when its public
 * area is the one we asked for -- ALL of it. Size and attributes are not an
 * identity: for a policy-protected index the authPolicy is the access-control
 * rule itself, so an index with our size and our attribute bits but somebody
 * else's policy digest would be accepted as ours and then trusted under PCR
 * state we never chose. nameAlg is definition-bearing for the same reason (it
 * determines the index Name).
 *
 * The TPM-maintained status bits are excluded from the attribute comparison:
 * WRITTEN flips on first write and the lock bits flip on WriteLock, so an index
 * that is in USE must still match the definition it was created with.
 * Returns OK for an identical definition, MISMATCH otherwise. */
static tpm_nv_status_t nv_definition_matches(tpm2_seq_t seq, uint32_t nv_index,
                                             uint32_t attrs,
                                             uint16_t data_size, uint16_t name_alg,
                                             const uint8_t *auth_policy,
                                             uint16_t policy_len)
{
    uint8_t cmd[16], rsp[TPM_NV_MAX_DATA + 128u];
    struct tpm_nv_public have;
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    uint16_t i;

    n = tpm2_build_nv_read_public(cmd, sizeof cmd, nv_index);
    if (n == 0u)
        return TPM_NV_BADARG;
    /* Submits inside the CALLER's sequence: the define and this comparison are
     * one logical operation and share one budget. */
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0)
        return st;
    if (tpm2_parse_nv_public_full(rsp, rlen, nv_index, &have) != 0)
        return TPM_NV_TRANSPORT;

    if (have.data_size != data_size)
        return TPM_NV_MISMATCH;
    if ((have.attrs & ~TPMA_NV_STATUS_MASK) != (attrs & ~TPMA_NV_STATUS_MASK))
        return TPM_NV_MISMATCH;
    if (have.name_alg != name_alg)
        return TPM_NV_MISMATCH;
    if (have.policy_len != policy_len)
        return TPM_NV_MISMATCH;
    for (i = 0; i < policy_len; i++)
        if (have.auth_policy[i] != auth_policy[i])
            return TPM_NV_MISMATCH;
    return TPM_NV_OK;
}

struct nv_define_ctx {
    uint32_t        nv_index;
    uint32_t        attrs;
    uint16_t        data_size;
    const uint8_t  *auth_policy;
    uint16_t        policy_len;
    /* When set, the authPolicy is COMPUTED inside the sequence (a trial
     * PolicyPCR digest over the baseline mask) rather than supplied. Keeping it
     * inside is what makes the define one atomic operation: computing it in a
     * separate sequence would release the gate in between, let another
     * transaction interleave, and spend two full budgets on one logical act. */
    int             compute_baseline_policy;
    uint8_t         policy_buf[32];
    tpm_nv_status_t st;
};

/* Define, and -- when the index already exists -- verify it, as ONE bounded
 * operation. The two commands belong to the same logical act, so they share one
 * budget rather than each getting a fresh one. */
static int nv_define_seq(tpm2_seq_t seq, void *vctx)
{
    struct nv_define_ctx *c = (struct nv_define_ctx *)vctx;
    uint8_t cmd[96], rsp[32];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;

    if (c->compute_baseline_policy) {
        uint8_t sel[3];
        tpm_nv_baseline_pcr_select(sel);
        st = nv_policy_pcr_digest_in_seq(seq, TPM_ALG_SHA256, sel,
                                         c->policy_buf, sizeof c->policy_buf);
        if (st != TPM_NV_OK) { c->st = st; return 0; }
        c->auth_policy = c->policy_buf;
        c->policy_len = (uint16_t)sizeof c->policy_buf;
    }
    n = tpm2_build_nv_define(cmd, sizeof cmd, c->nv_index, c->attrs,
                             TPM_ALG_SHA256, c->auth_policy, c->policy_len,
                             c->data_size);
    if (n == 0u) { c->st = TPM_NV_BADARG; return 0; }
    (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0);
    if (st == TPM_NV_DEFINED) {
        /* Fail closed on a DIFFERENT definition instead of proceeding against
         * an index that does not have the properties this caller depends on --
         * the shape a machine enrolled under the old, wrong read attributes is
         * in. Redefining it needs undefine + redefine under an authorization
         * this layer deliberately does not hold. */
        c->st = nv_definition_matches(seq, c->nv_index, c->attrs, c->data_size,
                                      TPM_ALG_SHA256, c->auth_policy,
                                      c->policy_len);
        return 0;
    }
    c->st = st;
    return 0;
}

tpm_nv_status_t tpm_nv_define_ex(uint32_t nv_index, uint32_t attrs,
                                 uint16_t data_size,
                                 const uint8_t *auth_policy, uint16_t policy_len)
{
    struct nv_define_ctx c;
    tpm_nv_status_t st;
    int r;

    if (policy_len != 0u && !auth_policy)
        return TPM_NV_BADARG;
    if ((uint32_t)policy_len > TPM_NV_POLICY_MAX)
        return TPM_NV_BADARG;
    /* Same rule on the request side as on the response side: an authPolicy is
     * absent or exactly one nameAlg digest. */
    if (policy_len != 0u &&
        (uint32_t)policy_len != tpm_alg_digest_len_pub(TPM_ALG_SHA256))
        return TPM_NV_BADARG;
    st = tpm_nv_attrs_valid(attrs, data_size, TPM_ALG_SHA256);
    if (st != TPM_NV_OK)
        return st;
    c.nv_index = nv_index; c.attrs = attrs; c.data_size = data_size;
    c.auth_policy = auth_policy; c.policy_len = policy_len;
    c.compute_baseline_policy = 0;
    c.st = TPM_NV_TRANSPORT;
    r = tpm2_seq_run(s_nv_work_ms, s_nv_cleanup_ms, nv_define_seq, &c);
    return (r != 0) ? nv_seq_start_status(r) : c.st;
}

tpm_nv_status_t tpm_nv_define_counter(uint32_t nv_index, uint32_t access_attrs)
{
    /* The type, size and NO_DA are fixed here so a "counter" cannot accidentally
     * be defined as an ordinary index; type bits in access_attrs are rejected
     * rather than silently overridden.
     *
     * OWNERREAD|OWNERWRITE are REQUIRED on top of whatever else the caller
     * asks for, because they are the only authorization the wrappers in this
     * module can actually use: tpm_nv_increment and tpm_nv_read_counter both
     * authorize with TPM_RH_OWNER and a password session. Accepting a
     * policy-only counter would provision a persistent index that this
     * module's own read and increment then fail to authorize on real hardware
     * -- unusable until an authorized undefine/redefine, which is exactly the
     * lifecycle operation this layer deliberately does not perform. A
     * policy-authorized counter needs policy-authorized wrappers, which belong
     * with the authorization construction rather than here. */
    if (access_attrs & (TPMA_NV_TPM_NT_MASK | TPMA_NV_RESERVED_MASK))
        return TPM_NV_ATTRS;
    if ((access_attrs & (TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE)) !=
        (TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE))
        return TPM_NV_ATTRS;
    return tpm_nv_define_ex(nv_index,
                            access_attrs | TPMA_NV_TYPE(TPM_NT_COUNTER) |
                            TPMA_NV_NO_DA,
                            (uint16_t)TPM_NV_COUNTER_SIZE, 0, 0);
}

tpm_nv_status_t tpm_nv_define_data(uint32_t nv_index, uint16_t data_size)
{
    uint32_t attrs = TPMA_NV_OWNERREAD | TPMA_NV_OWNERWRITE | TPMA_NV_NO_DA;
    if (data_size == 0u || data_size > TPM_NV_MAX_INDEX_SIZE)
        return TPM_NV_BADARG;
    return tpm_nv_define_ex(nv_index, attrs, data_size, 0, 0);
}

tpm_nv_status_t tpm_nv_define_baseline(uint32_t nv_index, uint16_t data_size)
{
    struct nv_define_ctx c;
    uint32_t attrs = TPMA_NV_POLICYREAD | TPMA_NV_POLICYWRITE | TPMA_NV_NO_DA;
    tpm_nv_status_t st;
    int r;

    if (data_size == 0u || data_size > TPM_NV_MAX_INDEX_SIZE)
        return TPM_NV_BADARG;
    st = tpm_nv_attrs_valid(attrs, data_size, TPM_ALG_SHA256);
    if (st != TPM_NV_OK)
        return st;
    /* Policy computation AND definition in ONE sequence. Splitting them (the
     * shape this replaced) released the transport between the trial session
     * that computes the digest and the define that binds it, so the operation
     * spent two full budgets and another transaction could interleave between
     * deciding the policy and committing to it. */
    c.nv_index = nv_index; c.attrs = attrs; c.data_size = data_size;
    c.auth_policy = 0; c.policy_len = 0;
    c.compute_baseline_policy = 1;
    c.st = TPM_NV_TRANSPORT;
    r = tpm2_seq_run(s_nv_work_ms, s_nv_cleanup_ms, nv_define_seq, &c);
    return (r != 0) ? nv_seq_start_status(r) : c.st;
}

tpm_nv_status_t tpm_nv_undefine(uint32_t nv_index)
{
    uint8_t cmd[40], rsp[32];
    uint32_t n, rlen = 0;
    n = tpm2_build_nv_undefine(cmd, sizeof cmd, nv_index);
    if (n == 0u)
        return TPM_NV_BADARG;
    return nv_exec_bounded(cmd, n, rsp, sizeof rsp, &rlen);
}

tpm_nv_status_t tpm_nv_write(uint32_t nv_index, uint16_t offset,
                             const uint8_t *data, uint16_t len)
{
    uint8_t cmd[TPM_NV_MAX_DATA + 64u], rsp[32];
    uint32_t n, rlen = 0;
    if (!data || len == 0u || len > TPM_NV_MAX_DATA)
        return TPM_NV_BADARG;
    n = tpm2_build_nv_write(cmd, sizeof cmd, TPM_RH_OWNER, nv_index, TPM_RS_PW,
                            offset, data, len);
    if (n == 0u)
        return TPM_NV_BADARG;
    return nv_exec_bounded(cmd, n, rsp, sizeof rsp, &rlen);
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
    st = nv_exec_bounded(cmd, n, rsp, sizeof rsp, &rlen);
    if (st != TPM_NV_OK)
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

/* NV_Increment and NV_WriteLock differ only in command code: same owner-auth
 * password session, same two handles, no parameters, no response parameters. */
static tpm_nv_status_t nv_handles_only_op(uint32_t cc, uint32_t nv_index)
{
    uint8_t cmd[40], rsp[32];
    uint32_t n, rlen = 0;
    n = nv_build_handles_only(cmd, sizeof cmd, cc, TPM_RH_OWNER, nv_index,
                              TPM_RS_PW);
    if (n == 0u)
        return TPM_NV_BADARG;
    /* Exactly one submit, under the operation budget. An abandoned increment's
     * completion is unknown, so a retry here could advance the counter twice --
     * the caller decides what to do with a failure, this layer never guesses. */
    return nv_exec_bounded(cmd, n, rsp, sizeof rsp, &rlen);
}

tpm_nv_status_t tpm_nv_increment(uint32_t nv_index)
{
    return nv_handles_only_op(TPM2_CC_NV_INCREMENT, nv_index);
}

tpm_nv_status_t tpm_nv_write_lock(uint32_t nv_index)
{
    return nv_handles_only_op(TPM2_CC_NV_WRITE_LOCK, nv_index);
}

tpm_nv_status_t tpm_nv_read_counter(uint32_t nv_index, uint64_t *out)
{
    uint8_t buf[TPM_NV_COUNTER_SIZE];
    uint16_t got = 0;
    tpm_nv_status_t st;

    if (!out)
        return TPM_NV_BADARG;
    st = tpm_nv_read(nv_index, 0u, buf, (uint16_t)sizeof buf, &got);
    if (st != TPM_NV_OK)
        return st;              /* UNINIT stays UNINIT; *out untouched */
    /* A counter is exactly 8 bytes. A short read is a malformed response, not a
     * small number -- decoding it would invent a value from partial bytes. */
    if (got != (uint16_t)TPM_NV_COUNTER_SIZE)
        return TPM_NV_TRANSPORT;
    *out = ((uint64_t)tpm2_be32_get(buf) << 32) |
           (uint64_t)tpm2_be32_get(buf + 4);
    return TPM_NV_OK;
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
    st = nv_exec_bounded(cmd, n, rsp, sizeof rsp, &rlen);
    if (st != TPM_NV_OK)
        return st;
    if (tpm2_parse_nv_read_public(rsp, rlen, nv_index, out_size, out_attrs) != 0)
        return TPM_NV_TRANSPORT;
    return TPM_NV_OK;
}
