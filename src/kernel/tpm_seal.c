/* ============================================================================
 * tpm_seal.c -- TPM2 sealed-secret boot policy hooks (measured boot)
 *
 * Pure marshaling builders/parsers (MMIO-free, fixture-tested) plus the
 * tpm_seal_ / tpm_unseal_ wrappers that drive the Phase-1 transport. Seals a
 * small secret to the DERIVED seal PCR mask (tpm_pcr_seal_mask(), PCR 7 -- PCR
 * 11 excluded so a kernel rebuild does not brick FDE unlock).
 *
 * Session/handle lifetime discipline (single cleanup path): the parent storage
 * key is a deterministic CreatePrimary of a fixed SRK template (the TPM
 * regenerates the same key from its owner primary seed), so it is created on
 * demand and flushed once the object is created/loaded. Every wrapper routes
 * ALL exits after a handle is created through one seal_flush(), and the policy
 * session is owned by tpm_policy_session_run() (which flushes on every path).
 * An error or BUSY can therefore never leak a transient object or session handle
 * into the TPM's small pool (which would wedge sealing until reboot).
 *
 * The pure builders/parsers and tpm_nv.c's classifier/seam are reused for the
 * transport plumbing; this module owns the object-sealing wire formats only.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_seal.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm_pcr_alloc.h"
#include "libc/string.h"

/* ---- Auth-area writers (one session) ----
 * authorizationSize(4) + sessionHandle(4) + nonce TPM2B(2,0) +
 * sessionAttributes(1) + hmac TPM2B(2,0) = 13 bytes; authorizationSize value is 9
 * (the per-session bytes after the size field). continueSession (attrs bit 0) is
 * set ONLY for a real HMAC/POLICY session (high byte 0x02/0x03) -- it keeps that
 * session alive for our explicit FlushContext. For the permanent password handle
 * TPM_RS_PW the attribute is 0x00: a password authorization is not a savable
 * session, so continueSession is meaningless and a strict TPM can reject it as an
 * attributes error (a bare-metal-only failure the fakes would not catch). */
static uint32_t seal_put_auth(uint8_t *buf, uint32_t off, uint32_t session)
{
    uint8_t ht = (uint8_t)(session >> 24);
    tpm2_be32_put(buf + off, 9u); off += 4u;        /* authorizationSize */
    tpm2_be32_put(buf + off, session); off += 4u;   /* sessionHandle */
    tpm2_be16_put(buf + off, 0u); off += 2u;        /* nonce size 0 */
    buf[off] = (ht == 0x02u || ht == 0x03u) ? 0x01u : 0x00u; off += 1u; /* attrs */
    tpm2_be16_put(buf + off, 0u); off += 2u;        /* hmac size 0 */
    return off;
}

/* ---- Pure marshaling builders ---- */

uint32_t tpm2_build_create_primary_srk(uint8_t *buf, uint32_t cap)
{
    /* TPMT_PUBLIC (ECC P-256 SRK): type(2)+nameAlg(2)+attrs(4)+authPolicy(2,0)
     * + TPMS_ECC_PARMS{ sym alg(2)+keyBits(2)+mode(2) + scheme(2) + curve(2)
     *   + kdf(2) } + unique TPM2B_ECC_POINT{ x(2,0)+y(2,0) } = 26. */
    const uint32_t pub_inner = 26u;
    uint32_t total = 10u + 4u + 13u + 6u + (2u + pub_inner) + 2u + 4u;
    uint32_t off;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_CREATE_PRIMARY);
    tpm2_be32_put(buf + 10, TPM_RH_OWNER);            /* primaryHandle */
    off = seal_put_auth(buf, 14u, TPM_RS_PW);         /* owner pw (empty) */
    /* inSensitive TPM2B_SENSITIVE_CREATE: size(2) + userAuth(2,0) + data(2,0). */
    tpm2_be16_put(buf + off, 4u); off += 2u;
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* userAuth empty */
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* data empty */
    /* inPublic TPM2B_PUBLIC. */
    tpm2_be16_put(buf + off, (uint16_t)pub_inner); off += 2u;
    tpm2_be16_put(buf + off, TPM_ALG_ECC); off += 2u;       /* type */
    tpm2_be16_put(buf + off, TPM_ALG_SHA256); off += 2u;    /* nameAlg */
    tpm2_be32_put(buf + off, TPM_SRK_OBJECT_ATTRS); off += 4u;
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* authPolicy empty */
    tpm2_be16_put(buf + off, TPM_ALG_AES); off += 2u;      /* symmetric.algorithm */
    tpm2_be16_put(buf + off, 128u); off += 2u;             /* symmetric.keyBits */
    tpm2_be16_put(buf + off, TPM_ALG_CFB); off += 2u;      /* symmetric.mode */
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;     /* scheme */
    tpm2_be16_put(buf + off, TPM_ECC_NIST_P256); off += 2u;/* curveID */
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;     /* kdf */
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* unique.x empty */
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* unique.y empty */
    /* outsideInfo TPM2B_DATA(2,0) + creationPCR TPML_PCR_SELECTION count=0. */
    tpm2_be16_put(buf + off, 0u); off += 2u;
    tpm2_be32_put(buf + off, 0u); off += 4u;
    return off;
}

uint32_t tpm2_parse_object_handle(const uint8_t *rsp, uint32_t len)
{
    uint16_t tag;
    uint32_t size, rc, psize, auth_off, handle;
    /* CreatePrimary/Load are session-authorized: a SUCCESS is ST_SESSIONS with a
     * leading objectHandle(4) area BEFORE parameterSize(4), then parameters, then
     * a one-session response auth area. tpm2_rsp_params (which reads parameterSize
     * at the fixed offset 10) does NOT apply, so the whole envelope is validated
     * here: tag, size, the parameterSize bound, and the auth area. */
    if (tpm2_rsp_parse(rsp, len, &tag, &size, &rc) != 0 || rc != TPM2_RC_SUCCESS)
        return 0;
    if (tag != TPM2_ST_SESSIONS)
        return 0;
    /* header(10) + objectHandle(4) + parameterSize(4) + >= 5-byte auth area. */
    if (size < 10u + 4u + 4u + 5u)
        return 0;
    psize = tpm2_be32_get(rsp + 14);            /* parameterSize (after objectHandle@10) */
    if (psize > size - 18u)                      /* parameters must fit (size >= 23) */
        return 0;
    auth_off = 18u + psize;                      /* parameters end -> auth area start */
    if (size - auth_off < 5u)                    /* room for the minimal auth area */
        return 0;
    if (!tpm_session_auth_response_ok(rsp, size, auth_off))
        return 0;
    /* Only a transient OBJECT handle (TPM_HT_TRANSIENT, high byte 0x80) is a
     * valid CreatePrimary/Load result; a corrupt success naming anything else
     * must not be returned (the caller later FlushContext's whatever this is). */
    handle = tpm2_be32_get(rsp + 10);
    if ((uint8_t)(handle >> 24) != 0x80u)
        return 0;
    return handle;
}

uint32_t tpm2_build_create_sealed(uint8_t *buf, uint32_t cap, uint32_t parent,
                                  const uint8_t *auth_policy, uint16_t auth_policy_len,
                                  const uint8_t *secret, uint16_t secret_len)
{
    /* TPMS_SENSITIVE_CREATE contents: userAuth(2,0) + data TPM2B(2 + secret). */
    uint32_t insens = 2u + (2u + (uint32_t)secret_len);
    /* TPMT_PUBLIC (KEYEDHASH sealed): type(2)+nameAlg(2)+attrs(4)
     * + authPolicy TPM2B(2 + N) + TPMS_KEYEDHASH_PARMS{ scheme(2) }
     * + unique TPM2B_DIGEST(2,0). */
    uint32_t pub_inner = 2u + 2u + 4u + (2u + (uint32_t)auth_policy_len) + 2u + 2u;
    uint32_t total = 10u + 4u + 13u + (2u + insens) + (2u + pub_inner) + 2u + 4u;
    uint32_t off, i;
    if (!buf || cap < total || !secret || secret_len == 0u ||
        secret_len > TPM_SEAL_SECRET_MAX ||
        (auth_policy_len != 0u && !auth_policy))
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_CREATE);
    tpm2_be32_put(buf + 10, parent);                  /* parentHandle */
    off = seal_put_auth(buf, 14u, TPM_RS_PW);         /* parent pw (empty) */
    /* inSensitive TPM2B_SENSITIVE_CREATE. */
    tpm2_be16_put(buf + off, (uint16_t)insens); off += 2u;
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* userAuth empty */
    tpm2_be16_put(buf + off, secret_len); off += 2u;  /* data size */
    for (i = 0; i < secret_len; i++) buf[off + i] = secret[i];
    off += secret_len;
    /* inPublic TPM2B_PUBLIC. */
    tpm2_be16_put(buf + off, (uint16_t)pub_inner); off += 2u;
    tpm2_be16_put(buf + off, TPM_ALG_KEYEDHASH); off += 2u; /* type */
    tpm2_be16_put(buf + off, TPM_ALG_SHA256); off += 2u;    /* nameAlg */
    tpm2_be32_put(buf + off, TPM_SEAL_OBJECT_ATTRS); off += 4u;
    tpm2_be16_put(buf + off, auth_policy_len); off += 2u;   /* authPolicy size */
    for (i = 0; i < auth_policy_len; i++) buf[off + i] = auth_policy[i];
    off += auth_policy_len;
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;      /* scheme (no HMAC) */
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* unique empty */
    /* outsideInfo TPM2B_DATA(2,0) + creationPCR count=0. */
    tpm2_be16_put(buf + off, 0u); off += 2u;
    tpm2_be32_put(buf + off, 0u); off += 4u;
    return off;
}

int tpm2_parse_create_sealed(const uint8_t *rsp, uint32_t len,
                             struct tpm_sealed_blob *out)
{
    uint32_t poff, plen, end, p, priv_off, pub_off, i;
    uint16_t priv_n, pub_n, cd_n, ch_n, ct_n;
    if (!out || tpm2_rsp_params(rsp, len, &poff, &plen) != 0)
        return -1;
    /* A real TPM2_Create response carries ALL of: outPrivate TPM2B_PRIVATE +
     * outPublic TPM2B_PUBLIC + creationData TPM2B + creationHash TPM2B +
     * creationTicket TPMT_TK_CREATION{ tag(2)+hierarchy(4)+digest TPM2B }. Parse
     * and bound EVERY field and require EXACT consumption of the parameter area:
     * a truncated success that ends after outPublic must NOT be accepted as a
     * valid sealed blob (it would persist a blob from an invalid wire shape). */
    end = poff + plen;
    p = poff;
    if (p + 2u > end) return -1;
    priv_n = tpm2_be16_get(rsp + p); p += 2u;            /* outPrivate */
    if (priv_n == 0u || (uint32_t)priv_n > TPM_SEAL_PRIV_MAX ||
        (uint32_t)priv_n > end - p) return -1;
    priv_off = p; p += priv_n;
    if (p + 2u > end) return -1;
    pub_n = tpm2_be16_get(rsp + p); p += 2u;             /* outPublic */
    if (pub_n == 0u || (uint32_t)pub_n > TPM_SEAL_PUB_MAX ||
        (uint32_t)pub_n > end - p) return -1;
    pub_off = p; p += pub_n;
    if (p + 2u > end) return -1;
    cd_n = tpm2_be16_get(rsp + p); p += 2u;              /* creationData TPM2B */
    if ((uint32_t)cd_n > end - p) return -1;
    p += cd_n;
    if (p + 2u > end) return -1;
    ch_n = tpm2_be16_get(rsp + p); p += 2u;              /* creationHash TPM2B */
    if ((uint32_t)ch_n > end - p) return -1;
    p += ch_n;
    if (p + 6u > end) return -1;                         /* ticket tag(2)+hierarchy(4) */
    p += 6u;
    if (p + 2u > end) return -1;
    ct_n = tpm2_be16_get(rsp + p); p += 2u;              /* ticket digest TPM2B */
    if ((uint32_t)ct_n > end - p) return -1;
    p += ct_n;
    if (p != end)                                        /* exact consumption */
        return -1;
    for (i = 0; i < priv_n; i++) out->priv[i] = rsp[priv_off + i];
    for (i = 0; i < pub_n; i++)  out->pub[i]  = rsp[pub_off + i];
    out->priv_len = priv_n;
    out->pub_len  = pub_n;
    return 0;
}

uint32_t tpm2_build_load(uint8_t *buf, uint32_t cap, uint32_t parent,
                         const struct tpm_sealed_blob *blob)
{
    uint32_t total, off, i;
    if (!buf || !blob || blob->priv_len == 0u || blob->pub_len == 0u ||
        blob->priv_len > TPM_SEAL_PRIV_MAX || blob->pub_len > TPM_SEAL_PUB_MAX)
        return 0;
    total = 10u + 4u + 13u + (2u + (uint32_t)blob->priv_len) +
            (2u + (uint32_t)blob->pub_len);
    if (cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_LOAD);
    tpm2_be32_put(buf + 10, parent);                  /* parentHandle */
    off = seal_put_auth(buf, 14u, TPM_RS_PW);         /* parent pw (empty) */
    tpm2_be16_put(buf + off, blob->priv_len); off += 2u;   /* inPrivate */
    for (i = 0; i < blob->priv_len; i++) buf[off + i] = blob->priv[i];
    off += blob->priv_len;
    tpm2_be16_put(buf + off, blob->pub_len); off += 2u;    /* inPublic */
    for (i = 0; i < blob->pub_len; i++) buf[off + i] = blob->pub[i];
    off += blob->pub_len;
    return off;
}

uint32_t tpm2_build_unseal(uint8_t *buf, uint32_t cap, uint32_t item,
                           uint32_t policy_session)
{
    uint32_t total = 10u + 4u + 13u;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_UNSEAL);
    tpm2_be32_put(buf + 10, item);                    /* itemHandle */
    (void)seal_put_auth(buf, 14u, policy_session);    /* policy session auth */
    return total;
}

int tpm2_parse_unseal(const uint8_t *rsp, uint32_t len,
                      uint8_t *out, uint32_t out_cap)
{
    uint32_t poff, plen, i;
    uint16_t dsz;
    if (!out || tpm2_rsp_params(rsp, len, &poff, &plen) != 0 || plen < 2u)
        return -1;
    /* outData TPM2B_SENSITIVE_DATA is the only parameter: exact consumption. A
     * zero-length payload is rejected -- the seal path never seals an empty
     * secret, so an empty unseal is malformed and must not be reported as a
     * successful unlock (an FDE/CI caller keying off OK would mishandle an empty
     * key/policy). */
    dsz = tpm2_be16_get(rsp + poff);
    if (dsz == 0u || (uint32_t)dsz != plen - 2u || (uint32_t)dsz > out_cap)
        return -1;
    for (i = 0; i < dsz; i++)
        out[i] = rsp[poff + 2u + i];
    return (int)dsz;
}

/* ---- Transport plumbing ---- */

/* Best-effort single teardown for a transient object / session handle. Uses the
 * bounded-wait submit so transient transport contention cannot abandon the
 * handle (a leaked object/session erodes the TPM's small pool until reboot). */
static void seal_flush(uint32_t handle)
{
    uint8_t cmd[16], rsp[16];
    uint32_t n;
    if (handle == 0u)
        return;
    n = tpm2_build_flush_context(cmd, sizeof cmd, handle);
    if (n == 0u)
        return;
    (void)tpm2_submit_waiting(cmd, n, rsp, sizeof rsp, TPM_NV_FLUSH_BUDGET_MS);
}

/* Map the shared seam's NV status onto a seal status. AUTH is mapped by the
 * CALLER per stage (owner/parent auth vs policy mismatch), so it is treated as
 * the generic auth failure here. */
static tpm_seal_status_t map_nv_status(tpm_nv_status_t s)
{
    switch (s) {
        case TPM_NV_OK:        return TPM_SEAL_OK;
        case TPM_NV_BADARG:    return TPM_SEAL_BADARG;
        case TPM_NV_BUSY:      return TPM_SEAL_BUSY;
        case TPM_NV_TRANSPORT: return TPM_SEAL_TRANSPORT;
        case TPM_NV_AUTH:      return TPM_SEAL_AUTH_FAIL;
        /* BUDGET is "the TPM answered, just slower than this boot will wait",
         * not a device error. Letting it fall into TPMERR would tell a caller
         * the TPM misbehaved and send it into hard-failure recovery, when the
         * honest report is the same transient class as BUSY: worth retrying
         * outside the boot path, not a reason to distrust the device. */
        case TPM_NV_BUDGET:    return TPM_SEAL_BUSY;
        /* Enumerated rather than left to default: a destroyed-and-recreated
         * anchor and a corrupt persisted contract are both hard integrity
         * failures, and naming them stops a future status from inheriting this
         * bucket by accident. */
        case TPM_NV_RECREATED:
        case TPM_NV_CONTRACT:  return TPM_SEAL_TPMERR;
        default:               return TPM_SEAL_TPMERR;
    }
}

/* Recovery handoff -- one registered handler, invoked on unseal failure. Stored
 * atomically: it is set during single-threaded boot init but read by any later
 * consumer, so the load/store are atomic (SMP-safe). */
static tpm_seal_recovery_fn s_recovery_handler;

void tpm_seal_set_recovery_handler(tpm_seal_recovery_fn fn)
{
    __atomic_store_n(&s_recovery_handler, fn, __ATOMIC_RELEASE);
}

static void seal_report_failure(struct tpm_unseal_result *result,
                                tpm_seal_status_t status, uint32_t tpm_rc,
                                tpm_seal_domain_t domain)
{
    tpm_seal_recovery_fn h;
    struct tpm_unseal_result local;
    struct tpm_unseal_result *r = result ? result : &local;
    r->status = status;
    r->tpm_rc = tpm_rc;
    r->domain = domain;
    if (status == TPM_SEAL_OK)
        return;
    h = __atomic_load_n(&s_recovery_handler, __ATOMIC_ACQUIRE);
    if (h)
        h(r);
}

/* Submit a command whose SUCCESS response carries a leading object-handle area
 * (CreatePrimary / Load) and return that transient handle, 0 on failure with
 * *out_st classified. These responses put objectHandle BEFORE parameterSize, so
 * tpm_session_cmd_exec's parameter/auth validation (which assumes no handle
 * area) does NOT apply -- the fixed-offset handle parser is the validation, and
 * the rc is classified directly here. */
static uint32_t seal_exec_handle(const uint8_t *cmd, uint32_t n,
                                 tpm_nv_status_t *out_st, uint32_t *out_rc)
{
    uint8_t rsp[512];
    uint16_t tag;
    uint32_t size, rc, h;
    int r;
    if (n == 0u) { *out_st = TPM_NV_BADARG; return 0; }
    r = tpm2_submit(cmd, n, rsp, sizeof rsp);
    if (r < 0) {
        *out_st = (r == TPM_T_ERR_BUSY) ? TPM_NV_BUSY : TPM_NV_TRANSPORT;
        return 0;
    }
    if (tpm2_rsp_parse(rsp, (uint32_t)r, &tag, &size, &rc) != 0) {
        *out_st = TPM_NV_TRANSPORT;
        return 0;
    }
    if (out_rc) *out_rc = rc;                 /* raw rc for the structured report */
    if (rc != TPM2_RC_SUCCESS) {
        *out_st = tpm_nv_classify_rc(rc);
        return 0;
    }
    h = tpm2_parse_object_handle(rsp, (uint32_t)r);
    if (h == 0u) {
        /* rc was SUCCESS but the response is malformed (bad envelope/auth area).
         * The TPM may still have allocated the transient object named at the
         * fixed offset 10 -- best-effort flush it so repeated malformed successes
         * cannot exhaust the TPM's small object pool until reboot (mirrors the
         * session-handle recovery flush in tpm_policy_*). */
        if ((uint32_t)r >= 14u) {
            uint32_t raw = tpm2_be32_get(rsp + 10);
            if ((uint8_t)(raw >> 24) == 0x80u)
                seal_flush(raw);
        }
        *out_st = TPM_NV_TRANSPORT;
        return 0;
    }
    *out_st = TPM_NV_OK;
    return h;
}

/* Deterministically (re)create the SRK storage parent and return its handle. */
static uint32_t seal_create_primary(tpm_nv_status_t *out_st, uint32_t *out_rc)
{
    uint8_t cmd[96];
    uint32_t n = tpm2_build_create_primary_srk(cmd, sizeof cmd);
    return seal_exec_handle(cmd, n, out_st, out_rc);
}

tpm_seal_status_t tpm_seal_secret(const uint8_t *secret, uint16_t secret_len,
                                  struct tpm_sealed_blob *out)
{
    uint8_t cmd[TPM_SEAL_SECRET_MAX + 128u], rsp[768];
    uint8_t policy[32], sel[3];
    uint32_t mask, primary, n, rlen = 0;
    tpm_nv_status_t st;
    tpm_seal_status_t r;

    if (!secret || secret_len == 0u || secret_len > TPM_SEAL_SECRET_MAX || !out)
        return TPM_SEAL_BADARG;
    if (!tpm_transport_available())
        return TPM_SEAL_NO_TPM;
    mask = tpm_pcr_seal_mask();
    if (mask == 0u)                          /* misconfigured table: refuse a no-bind seal */
        return TPM_SEAL_BADARG;
    tpm_pcr_mask_to_select(mask, sel);

    /* authPolicy = trial PolicyPCR(seal mask) digest (self-cleaning session). */
    st = tpm_policy_pcr_digest(TPM_ALG_SHA256, sel, policy, sizeof policy);
    if (st != TPM_NV_OK)
        return map_nv_status(st);

    primary = seal_create_primary(&st, 0);   /* seal does not report; no rc needed */
    if (primary == 0u)
        return map_nv_status(st);
    /* From here ALL exits flush `primary`. */
    n = tpm2_build_create_sealed(cmd, sizeof cmd, primary, policy,
                                 (uint16_t)sizeof policy, secret, secret_len);
    if (n == 0u) { r = TPM_SEAL_BADARG; goto out; }
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0) {
        r = map_nv_status(st);
        goto out;
    }
    r = (tpm2_parse_create_sealed(rsp, rlen, out) == 0)
            ? TPM_SEAL_OK : TPM_SEAL_TRANSPORT;
out:
    seal_flush(primary);
    return r;
}

/* Unseal op run UNDER the open policy session (PolicyPCR already satisfied). */
struct seal_unseal_ctx {
    uint32_t  item;
    uint8_t  *out;
    uint16_t  cap;
    uint16_t *out_len;
    uint32_t  tpm_rc;   /* raw rc captured for the structured report */
};

static tpm_nv_status_t seal_unseal_op(tpm2_seq_t seq, uint32_t session, void *vctx)
{
    struct seal_unseal_ctx *c = (struct seal_unseal_ctx *)vctx;
    uint8_t cmd[64], rsp[TPM_SEAL_SECRET_MAX + 128u];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    int dl;
    n = tpm2_build_unseal(cmd, sizeof cmd, c->item, session);
    if (n == 0u)
        return TPM_NV_BADARG;
    /* tpm_session_cmd_exec validates the response is ST_SESSIONS with a bounded
     * parameterSize AND a well-formed one-session auth area BEFORE we trust
     * outData -- a truncated/forged success can never return arbitrary secret
     * bytes as OK. It also surfaces the raw rc (TPM_RC_POLICY_FAIL on PCR drift)
     * for the structured report. The policy session + object are flushed by the
     * callers, so no handle-leak hazard lives in this op. */
    /* Submits inside the caller's bounded sequence: that sequence already holds
     * the transport gate, so an unsequenced tpm2_submit here would bounce BUSY
     * against our own lock rather than run. */
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st,
                                 &c->tpm_rc) != 0)
        return st;
    dl = tpm2_parse_unseal(rsp, rlen, c->out, c->cap);
    if (dl < 0)
        return TPM_NV_TRANSPORT;
    if (c->out_len) *c->out_len = (uint16_t)dl;
    return TPM_NV_OK;
}

tpm_seal_status_t tpm_unseal_secret(const struct tpm_sealed_blob *blob,
                                    tpm_seal_domain_t domain,
                                    uint8_t *out, uint16_t cap, uint16_t *out_len,
                                    struct tpm_unseal_result *result)
{
    uint8_t cmd[TPM_SEAL_PRIV_MAX + TPM_SEAL_PUB_MAX + 64u];
    uint8_t sel[3];
    uint32_t mask, primary, object, n, stage_rc = 0u;
    tpm_nv_status_t st;
    tpm_seal_status_t r;
    struct seal_unseal_ctx ctx;

    if (out_len) *out_len = 0;
    if (!blob || !out || cap == 0u || cap > TPM_SEAL_SECRET_MAX ||
        blob->priv_len == 0u || blob->pub_len == 0u ||
        blob->priv_len > TPM_SEAL_PRIV_MAX || blob->pub_len > TPM_SEAL_PUB_MAX) {
        seal_report_failure(result, TPM_SEAL_BADARG, 0u, domain);
        return TPM_SEAL_BADARG;
    }
    if (!tpm_transport_available()) {
        seal_report_failure(result, TPM_SEAL_NO_TPM, 0u, domain);
        return TPM_SEAL_NO_TPM;
    }
    mask = tpm_pcr_seal_mask();
    if (mask == 0u) {
        seal_report_failure(result, TPM_SEAL_BADARG, 0u, domain);
        return TPM_SEAL_BADARG;
    }
    tpm_pcr_mask_to_select(mask, sel);

    primary = seal_create_primary(&st, &stage_rc);
    if (primary == 0u) {
        r = map_nv_status(st);
        seal_report_failure(result, r, stage_rc, domain);  /* raw rc for diagnosis */
        return r;
    }
    /* Load the object under the parent, then drop the parent immediately -- the
     * loaded object is self-standing for Unseal. Load's response carries a
     * leading object-handle area (same shape as CreatePrimary). */
    stage_rc = 0u;
    n = tpm2_build_load(cmd, sizeof cmd, primary, blob);
    object = seal_exec_handle(cmd, n, &st, &stage_rc);
    seal_flush(primary);                     /* parent no longer needed */
    if (object == 0u) {
        r = map_nv_status(st);
        seal_report_failure(result, r, stage_rc, domain);
        return r;
    }

    /* Unseal under a real PolicyPCR session over the seal mask. A PCR mismatch
     * surfaces as TPM_RC_POLICY_FAIL -> TPM_NV_AUTH from the op -> POLICY_FAIL. */
    ctx.item = object; ctx.out = out; ctx.cap = cap; ctx.out_len = out_len;
    ctx.tpm_rc = 0u;
    st = tpm_policy_session_run(TPM_ALG_SHA256, sel, seal_unseal_op, &ctx);
    seal_flush(object);

    if (st == TPM_NV_OK)
        r = TPM_SEAL_OK;
    else if (st == TPM_NV_AUTH)
        /* AUTH covers both TPM_RC_POLICY_FAIL and TPM_RC_AUTH_FAIL. Only a
         * POLICY_FAIL raised by the Unseal command itself means PCR drift (the
         * recoverable "boot a different config" case); an AUTH_FAIL, or an AUTH
         * from the PolicyPCR setup stage (ctx.tpm_rc stays 0 there), is a
         * different condition and must NOT be misdiagnosed as PCR drift to the
         * recovery UX. Discriminate on the raw rc captured from the Unseal stage. */
        r = ((ctx.tpm_rc & 0xBFu) == TPM2_RC_F1_POLICY_FAIL)
                ? TPM_SEAL_POLICY_FAIL : TPM_SEAL_AUTH_FAIL;
    else
        r = map_nv_status(st);
    seal_report_failure(result, r, ctx.tpm_rc, domain);
    return r;
}

/* ---- Forward-API hooks (domain-tagged wrappers; stub consumers) ---- */

tpm_seal_status_t tpm_seal_fde_key(const uint8_t *key, uint16_t key_len,
                                   struct tpm_sealed_blob *out)
{
    return tpm_seal_secret(key, key_len, out);
}

tpm_seal_status_t tpm_unseal_fde_key(const struct tpm_sealed_blob *blob,
                                     uint8_t *out_key, uint16_t cap,
                                     uint16_t *out_len,
                                     struct tpm_unseal_result *result)
{
    return tpm_unseal_secret(blob, TPM_SEAL_DOMAIN_FDE, out_key, cap, out_len, result);
}

tpm_seal_status_t tpm_seal_ci_policy(const uint8_t *policy, uint16_t policy_len,
                                     struct tpm_sealed_blob *out)
{
    return tpm_seal_secret(policy, policy_len, out);
}

tpm_seal_status_t tpm_unseal_ci_policy(const struct tpm_sealed_blob *blob,
                                       uint8_t *out_policy, uint16_t cap,
                                       uint16_t *out_len,
                                       struct tpm_unseal_result *result)
{
    return tpm_unseal_secret(blob, TPM_SEAL_DOMAIN_CI, out_policy, cap, out_len, result);
}
