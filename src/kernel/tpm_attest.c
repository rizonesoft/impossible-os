/* ============================================================================
 * tpm_attest.c -- TPM2 attestation: EK cert, AK provisioning, TPM2_Quote
 *
 * Pure marshaling builders/parsers (MMIO-free, fixture-tested) plus the live
 * query wrappers that drive the Phase-1 transport over the shared tpm_nv.c
 * policy seam. Provisions an Attestation Key bound to the Endorsement Key and
 * produces TPM-signed quotes a remote verifier can check.
 *
 * Auth model: CreatePrimary of the EK is authorized by the endorsement-hierarchy
 * auth (empty password by default -> TPM_RS_PW). The EK's own authPolicy
 * (PolicyA = TPM2_PolicySecret(TPM_RH_ENDORSEMENT)) governs USE of the EK as a
 * parent, so Create of the AK under the EK authorizes via a real policy session
 * that runs PolicySecret(endorsement) -- reusing the single-cleanup session
 * teardown discipline from tpm_nv.c / tpm_seal.c.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_seal.h"
#include "kernel/tpm_attest.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm_pcr_alloc.h"
#include "kernel/klog.h"
#include "libc/string.h"

/* The well-known EK authPolicy (PolicyA), SHA-256, from the TCG EK Credential
 * Profile: the digest of TPM2_PolicySecret(TPM_RH_ENDORSEMENT). Load-bearing --
 * the EK template MUST carry exactly this or the EK name (and every verifier's
 * expectation) diverges. */
const uint8_t TPM_EK_POLICY_A_SHA256[TPM_EK_POLICY_A_LEN] = {
    0x83, 0x71, 0x97, 0x67, 0x44, 0x84, 0xB3, 0xF8,
    0x1A, 0x90, 0xCC, 0x8D, 0x46, 0xA5, 0xD7, 0x24,
    0xFD, 0x52, 0xD7, 0x6E, 0x06, 0x52, 0x0B, 0x64,
    0xF2, 0xA1, 0xDA, 0x1B, 0x33, 0x14, 0x69, 0xAA,
};

/* ---- Auth-area writer (one session) ----
 * authorizationSize(4) + sessionHandle(4) + nonce TPM2B(2,0) +
 * sessionAttributes(1) + hmac TPM2B(2,0) = 13 bytes. continueSession (0x01) is
 * set ONLY for a real HMAC/POLICY session (high byte 0x02/0x03); for TPM_RS_PW
 * it is 0x00 (a password auth is not a savable session -- a strict TPM can
 * reject continueSession on it, a bare-metal-only failure). Mirrors the
 * tpm_nv.c / tpm_seal.c writers. */
static uint32_t at_put_auth(uint8_t *buf, uint32_t off, uint32_t session)
{
    uint8_t ht = (uint8_t)(session >> 24);
    tpm2_be32_put(buf + off, 9u); off += 4u;
    tpm2_be32_put(buf + off, session); off += 4u;
    tpm2_be16_put(buf + off, 0u); off += 2u;        /* nonce size 0 */
    buf[off] = (ht == 0x02u || ht == 0x03u) ? 0x01u : 0x00u; off += 1u;
    tpm2_be16_put(buf + off, 0u); off += 2u;        /* hmac size 0 */
    return off;
}

uint32_t tpm2_build_create_primary_ek(uint8_t *buf, uint32_t cap)
{
    /* TPMT_PUBLIC (ECC P-256 EK, TCG low-range template):
     *   type(2)+nameAlg(2)+attrs(4)
     *   + authPolicy TPM2B(2 + 32)
     *   + TPMS_ECC_PARMS{ sym alg(2)+keyBits(2)+mode(2) + scheme(2) + curve(2)
     *     + kdf(2) }
     *   + unique TPM2B_ECC_POINT{ x(2 + 32 zero) + y(2 + 32 zero) }
     * = 8 + 34 + 12 + 68 = 122. */
    const uint32_t pub_inner = 8u + (2u + 32u) + 12u + (2u + 32u + 2u + 32u);
    uint32_t total = 10u + 4u + 13u + 6u + (2u + pub_inner) + 2u + 4u;
    uint32_t off, i;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_CREATE_PRIMARY);
    tpm2_be32_put(buf + 10, TPM_RH_ENDORSEMENT);      /* primaryHandle */
    off = at_put_auth(buf, 14u, TPM_RS_PW);           /* endorsement pw (empty) */
    /* inSensitive TPM2B_SENSITIVE_CREATE: size(2) + userAuth(2,0) + data(2,0). */
    tpm2_be16_put(buf + off, 4u); off += 2u;
    tpm2_be16_put(buf + off, 0u); off += 2u;
    tpm2_be16_put(buf + off, 0u); off += 2u;
    /* inPublic TPM2B_PUBLIC. */
    tpm2_be16_put(buf + off, (uint16_t)pub_inner); off += 2u;
    tpm2_be16_put(buf + off, TPM_ALG_ECC); off += 2u;        /* type */
    tpm2_be16_put(buf + off, TPM_ALG_SHA256); off += 2u;     /* nameAlg */
    tpm2_be32_put(buf + off, TPM_EK_OBJECT_ATTRS); off += 4u;
    tpm2_be16_put(buf + off, TPM_EK_POLICY_A_LEN); off += 2u;/* authPolicy size */
    for (i = 0; i < TPM_EK_POLICY_A_LEN; i++) buf[off + i] = TPM_EK_POLICY_A_SHA256[i];
    off += TPM_EK_POLICY_A_LEN;
    tpm2_be16_put(buf + off, TPM_ALG_AES); off += 2u;       /* symmetric.algorithm */
    tpm2_be16_put(buf + off, 128u); off += 2u;              /* symmetric.keyBits */
    tpm2_be16_put(buf + off, TPM_ALG_CFB); off += 2u;       /* symmetric.mode */
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;      /* scheme */
    tpm2_be16_put(buf + off, TPM_ECC_NIST_P256); off += 2u; /* curveID */
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;      /* kdf */
    /* unique TPM2B_ECC_POINT: x{32 zero} + y{32 zero} (NOT empty -- the standard
     * EK template fixes the unique buffers to 32 zero bytes per coordinate). */
    tpm2_be16_put(buf + off, 32u); off += 2u;
    for (i = 0; i < 32u; i++) buf[off + i] = 0u;
    off += 32u;
    tpm2_be16_put(buf + off, 32u); off += 2u;
    for (i = 0; i < 32u; i++) buf[off + i] = 0u;
    off += 32u;
    /* outsideInfo TPM2B_DATA(2,0) + creationPCR TPML_PCR_SELECTION count=0. */
    tpm2_be16_put(buf + off, 0u); off += 2u;
    tpm2_be32_put(buf + off, 0u); off += 4u;
    return off;
}

uint32_t tpm2_build_policy_secret(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                                  uint32_t policy_session)
{
    /* TPM2_PolicySecret(authHandle{auth}, policySession): two handles (authHandle
     * carries the auth area, policySession does not), then the parameters:
     *   header(10) + authHandle(4) + policySession(4) + authArea(13: PW, empty
     *   endorsement auth) + nonceTPM TPM2B(2,0) + cpHashA TPM2B(2,0) + policyRef
     *   TPM2B(2,0) + expiration INT32(4) = 41. */
    uint32_t total = 10u + 4u + 4u + 13u + 2u + 2u + 2u + 4u;
    uint32_t off;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_POLICY_SECRET);
    tpm2_be32_put(buf + 10, auth_handle);             /* authHandle (entity) */
    tpm2_be32_put(buf + 14, policy_session);          /* policySession (no auth) */
    off = at_put_auth(buf, 18u, TPM_RS_PW);           /* authHandle auth (empty) */
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* nonceTPM empty */
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* cpHashA empty */
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* policyRef empty */
    tpm2_be32_put(buf + off, 0u); off += 4u;          /* expiration 0 (no timeout) */
    return off;
}

uint32_t tpm2_build_create_ak_signing(uint8_t *buf, uint32_t cap, uint32_t parent,
                                      uint32_t auth_session)
{
    /* TPMT_PUBLIC (ECC P-256 restricted signing key):
     *   type(2)+nameAlg(2)+attrs(4) + authPolicy(2,0)
     *   + TPMS_ECC_PARMS{ symmetric=NULL(2) + scheme TPMT_ECC_SCHEME{ECDSA(2)
     *     + hashAlg(2)} + curve(2) + kdf=NULL(2) }
     *   + unique TPM2B_ECC_POINT{ x(2,0)+y(2,0) }
     * = 8 + 2 + 10 + 4 = 24. A signing key has NO symmetric (alg NULL, 2 bytes),
     * unlike the storage EK/SRK (AES, 6 bytes). */
    const uint32_t pub_inner = 8u + 2u + 10u + 4u;
    uint32_t insens = 2u + 2u;                         /* userAuth(2,0) + data(2,0) */
    uint32_t total = 10u + 4u + 13u + (2u + insens) + (2u + pub_inner) + 2u + 4u;
    uint32_t off;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_CREATE);
    tpm2_be32_put(buf + 10, parent);                  /* parentHandle (loaded EK) */
    off = at_put_auth(buf, 14u, auth_session);        /* EK auth = policy session */
    /* inSensitive TPM2B_SENSITIVE_CREATE. */
    tpm2_be16_put(buf + off, (uint16_t)insens); off += 2u;
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* userAuth empty */
    tpm2_be16_put(buf + off, 0u); off += 2u;          /* data empty (TPM-generated) */
    /* inPublic TPM2B_PUBLIC. */
    tpm2_be16_put(buf + off, (uint16_t)pub_inner); off += 2u;
    tpm2_be16_put(buf + off, TPM_ALG_ECC); off += 2u;       /* type */
    tpm2_be16_put(buf + off, TPM_ALG_SHA256); off += 2u;    /* nameAlg */
    tpm2_be32_put(buf + off, TPM_AK_OBJECT_ATTRS); off += 4u;
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* authPolicy empty */
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;     /* symmetric = NULL (sign key) */
    tpm2_be16_put(buf + off, TPM_ALG_ECDSA); off += 2u;    /* scheme ECDSA */
    tpm2_be16_put(buf + off, TPM_ALG_SHA256); off += 2u;   /* scheme hashAlg */
    tpm2_be16_put(buf + off, TPM_ECC_NIST_P256); off += 2u;/* curveID */
    tpm2_be16_put(buf + off, TPM_ALG_NULL); off += 2u;     /* kdf */
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* unique.x empty */
    tpm2_be16_put(buf + off, 0u); off += 2u;               /* unique.y empty */
    /* outsideInfo TPM2B_DATA(2,0) + creationPCR count=0. */
    tpm2_be16_put(buf + off, 0u); off += 2u;
    tpm2_be32_put(buf + off, 0u); off += 4u;
    return off;
}

uint32_t tpm2_build_quote(uint8_t *buf, uint32_t cap, uint32_t ak_handle,
                          uint32_t auth_session, uint16_t sig_alg,
                          const uint8_t *nonce, uint16_t nonce_len, uint32_t pcr_mask)
{
    /* TPM2_Quote(signHandle{auth}, qualifyingData=nonce, inScheme, PCRselect):
     *   header(10) + signHandle(4) + authArea(13) + qualifyingData TPM2B(2 + N)
     *   + inScheme TPMT_SIG_SCHEME{ scheme(2) + hashAlg(2) }
     *   + TPML_PCR_SELECTION{ count(4)=1 + hashAlg(2) + sizeofSelect(1)=3
     *     + pcrSelect[3] } = 43 + nonce_len. The verifier nonce in qualifyingData
     * is what binds the quote to a fresh challenge (anti-replay). */
    uint32_t total = 10u + 4u + 13u + (2u + (uint32_t)nonce_len) + 4u + (4u + 2u + 1u + 3u);
    uint32_t off, i;
    uint8_t sel[3];
    if (!buf || cap < total || !nonce ||
        nonce_len < TPM_QUOTE_NONCE_MIN || nonce_len > TPM_QUOTE_NONCE_MAX ||
        (sig_alg != TPM_ALG_ECDSA && sig_alg != TPM_ALG_RSASSA))
        return 0;
    tpm_pcr_mask_to_select(pcr_mask, sel);
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_QUOTE);
    tpm2_be32_put(buf + 10, ak_handle);               /* signHandle (AK) */
    off = at_put_auth(buf, 14u, auth_session);        /* AK auth (TPM_RS_PW empty) */
    tpm2_be16_put(buf + off, nonce_len); off += 2u;   /* qualifyingData = nonce */
    for (i = 0; i < nonce_len; i++) buf[off + i] = nonce[i];
    off += nonce_len;
    tpm2_be16_put(buf + off, sig_alg); off += 2u;     /* inScheme scheme */
    tpm2_be16_put(buf + off, TPM_ALG_SHA256); off += 2u; /* inScheme hashAlg */
    tpm2_be32_put(buf + off, 1u); off += 4u;          /* TPML count = 1 */
    tpm2_be16_put(buf + off, TPM_ALG_SHA256); off += 2u; /* bank */
    buf[off] = 3u; off += 1u;                          /* sizeofSelect (PCR 0..23) */
    buf[off] = sel[0]; buf[off + 1u] = sel[1]; buf[off + 2u] = sel[2];
    off += 3u;
    return off;
}

/* Read a big-endian UINT64 (the TPM2 wire has no inline 64-bit helper). */
static uint64_t at_be64(const uint8_t *p)
{
    return ((uint64_t)tpm2_be32_get(p) << 32) | (uint64_t)tpm2_be32_get(p + 4u);
}

int tpm2_parse_quote(const uint8_t *rsp, uint32_t len, struct tpm_quote_attest *out,
                     uint8_t *sig_out, uint32_t sig_cap, uint32_t *sig_len)
{
    uint32_t poff, plen, pend, a, end_a, attest_size, count, i, sig_off, slen;
    uint16_t qn, en, pd, sos;
    if (!out || !sig_out || tpm2_rsp_params(rsp, len, &poff, &plen) != 0 || plen < 2u)
        return -1;
    pend = poff + plen;
    /* quoted: TPM2B_ATTEST{ size(2) + TPMS_ATTEST }. */
    attest_size = tpm2_be16_get(rsp + poff);
    a = poff + 2u;
    if (attest_size < 6u || attest_size > pend - a)   /* must hold >= magic+type */
        return -1;
    end_a = a + attest_size;
    /* Capture the EXACT signed bytes: the TPM signs the TPMS_ATTEST, so a verifier
     * needs this raw blob (not the parsed fields) as its signature input. */
    if (attest_size > sizeof out->attest_raw)         /* TPM_ATTEST_MAX bound */
        return -1;
    for (i = 0; i < attest_size; i++)
        out->attest_raw[i] = rsp[a + i];
    out->attest_raw_len = (uint16_t)attest_size;
    /* TPMS_ATTEST: magic + type pin this as a TPM-generated quote. */
    if (tpm2_be32_get(rsp + a) != TPM2_GENERATED_VALUE) return -1;
    a += 4u;
    if (tpm2_be16_get(rsp + a) != TPM2_ST_ATTEST_QUOTE) return -1;
    a += 2u;
    /* qualifiedSigner TPM2B_NAME (skip -- binds to the AK name; verifier checks). */
    if (a + 2u > end_a) return -1;
    qn = tpm2_be16_get(rsp + a); a += 2u;
    if ((uint32_t)qn > end_a - a) return -1;
    a += qn;
    /* extraData = the echoed verifier nonce (anti-replay). */
    if (a + 2u > end_a) return -1;
    en = tpm2_be16_get(rsp + a); a += 2u;
    if ((uint32_t)en > end_a - a || (uint32_t)en > TPM_QUOTE_NONCE_MAX) return -1;
    for (i = 0; i < en; i++) out->nonce[i] = rsp[a + i];
    out->nonce_len = en;
    a += en;
    /* clockInfo: clock(8) + resetCount(4) + restartCount(4) + safe(1) = 17. */
    if (a + 17u > end_a) return -1;
    out->clock = at_be64(rsp + a); a += 8u;
    out->reset_count = tpm2_be32_get(rsp + a); a += 4u;
    out->restart_count = tpm2_be32_get(rsp + a); a += 4u;
    out->safe = rsp[a]; a += 1u;
    /* firmwareVersion(8). */
    if (a + 8u > end_a) return -1;
    out->firmware_version = at_be64(rsp + a); a += 8u;
    /* attested TPMS_QUOTE_INFO: TPML_PCR_SELECTION + pcrDigest. */
    if (a + 4u > end_a) return -1;
    count = tpm2_be32_get(rsp + a); a += 4u;
    /* This subsystem requests exactly one SHA-256 bank with a 3-octet selection
     * (tpm2_build_quote). Bind the response shape to that: a desynchronized quote
     * over a different bank (SHA-1/SHA-384), with extra banks, or a wider selection
     * must NOT be accepted on a coincidentally matching bitmap. */
    if (count != 1u) return -1;
    out->pcr_select = 0u;
    for (i = 0; i < count; i++) {
        uint32_t k;
        if (a + 3u > end_a) return -1;
        if (tpm2_be16_get(rsp + a) != TPM_ALG_SHA256) return -1;  /* requested bank */
        a += 2u;                                       /* hashAlg */
        sos = rsp[a]; a += 1u;
        if (sos != 3u) return -1;                      /* PCRs 0-23 selection width */
        if ((uint32_t)sos > end_a - a) return -1;
        for (k = 0; k < sos; k++)
            out->pcr_select |= ((uint32_t)rsp[a + k]) << (8u * k);
        a += sos;
    }
    if (a + 2u > end_a) return -1;
    pd = tpm2_be16_get(rsp + a); a += 2u;
    /* SHA-256 bank (enforced above) -> pcrDigest is exactly a 32-byte SHA-256 digest.
     * A 0/31/64-byte digest is malformed and must not be reported as a valid quote. */
    if (pd != 32u || (uint32_t)pd > end_a - a) return -1;
    for (i = 0; i < pd; i++) out->pcr_digest[i] = rsp[a + i];
    out->pcr_digest_len = pd;
    a += pd;
    if (a != end_a)                                    /* exact attest consumption */
        return -1;
    /* TPMT_SIGNATURE = the rest of the parameter area. Validate its STRUCTURE for
     * the signing scheme (full-structure exact-consumption, like the sealed-object
     * response parsers): a crafted "successful" quote carrying only a 4-byte
     * sigAlg+hashAlg header and no R/S must NOT pass -- that would hand the
     * verifier an unverifiable signature reported as OK. */
    sig_off = end_a;                                   /* == poff + 2 + attest_size */
    slen = pend - sig_off;
    if (slen < 4u || slen > sig_cap)                   /* >= sigAlg(2) + hashAlg(2) */
        return -1;
    {
        const uint8_t *s = rsp + sig_off;
        uint16_t salg = tpm2_be16_get(s);
        uint32_t so = 4u;                              /* past sigAlg(2) + hashAlg(2) */
        uint16_t rl, sl;
        if (salg == TPM_ALG_ECDSA) {                   /* sigR TPM2B + sigS TPM2B */
            /* P-256 r and s are each <= 32 bytes (the AK is a restricted ECDSA-P256
             * signing key); an overlong component is a malformed/wrong-curve sig. */
            if (so + 2u > slen) return -1;
            rl = tpm2_be16_get(s + so); so += 2u;
            if (rl == 0u || rl > 32u || (uint32_t)rl > slen - so) return -1;
            so += rl;
            if (so + 2u > slen) return -1;
            sl = tpm2_be16_get(s + so); so += 2u;
            if (sl == 0u || sl > 32u || (uint32_t)sl > slen - so) return -1;
            so += sl;
        } else if (salg == TPM_ALG_RSASSA) {           /* single sig TPM2B */
            if (so + 2u > slen) return -1;
            rl = tpm2_be16_get(s + so); so += 2u;
            if (rl == 0u || (uint32_t)rl > slen - so) return -1;
            so += rl;
        } else {
            return -1;                                 /* unsupported signing scheme */
        }
        if (so != slen)                                /* exact signature consumption */
            return -1;
    }
    for (i = 0; i < slen; i++) sig_out[i] = rsp[sig_off + i];
    if (sig_len) *sig_len = slen;
    return 0;
}

uint32_t tpm2_build_evict_control(uint8_t *buf, uint32_t cap, uint32_t object,
                                  uint32_t persistent)
{
    /* TPM2_EvictControl(auth=TPM_RH_OWNER{auth}, objectHandle, persistentHandle):
     *   header(10) + authHandle(4) + objectHandle(4) + authArea(13) +
     *   persistentHandle(4) = 35. Persists a loaded transient AK to a stable
     *   handle so it survives across the boot the report is exported in. */
    uint32_t total = 10u + 4u + 4u + 13u + 4u;
    uint32_t off;
    if (!buf || cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_EVICT_CONTROL);
    tpm2_be32_put(buf + 10, TPM_RH_OWNER);            /* authHandle (owner) */
    tpm2_be32_put(buf + 14, object);                  /* objectHandle (transient AK) */
    off = at_put_auth(buf, 18u, TPM_RS_PW);           /* owner pw (empty) */
    tpm2_be32_put(buf + off, persistent); off += 4u;  /* persistentHandle */
    return off;
}

/* ---- Live transport wrappers (Phase-1; not ISR-safe) ---- */

/* The AK is provisioned once per boot and cached. s_ak_ready gates the cache;
 * its release/acquire orders the handle/pub writes (no lock needed for reads).
 * s_ak_busy serializes provisioning across CPUs WITHOUT a lock held over the
 * ms-scale TPM transactions (kernel-code-quality Gate 2: never hold a lock over
 * blocking I/O) -- a second concurrent caller gets BUSY and retries. */
static volatile int s_ak_ready;
static volatile int s_ak_busy;
static uint32_t s_ak_handle;
static uint8_t  s_ak_pub[TPM_AK_PUB_MAX];
static uint16_t s_ak_pub_len;

/* The EK PRIMARY public, cached for the boot. Separate from the AK cache and
 * separately ready-flagged, because a caller that wants the machine's stable
 * IDENTITY should not have to provision a signing key to get it. Published
 * under the same s_ak_busy gate so only one attestation conversation with the
 * TPM is ever in flight. */
static uint8_t  s_ek_pub[TPM_EK_PUB_MAX];
static uint16_t s_ek_pub_len;
static volatile int s_ek_ready;

static tpm_attest_status_t map_attest(tpm_nv_status_t s)
{
    switch (s) {
        case TPM_NV_OK:        return TPM_ATTEST_OK;
        case TPM_NV_BADARG:    return TPM_ATTEST_BADARG;
        case TPM_NV_BUSY:      return TPM_ATTEST_BUSY;
        case TPM_NV_NOTFOUND:  return TPM_ATTEST_NO_EK_CERT;
        case TPM_NV_TRANSPORT: return TPM_ATTEST_TRANSPORT;
        /* Same reasoning as tpm_seal.c's map_nv_status: a cumulative-budget
         * expiry means the operation ran out of the boot's patience, not that
         * the TPM failed. TPMERR would misreport a usable device. */
        case TPM_NV_BUDGET:    return TPM_ATTEST_BUSY;
        /* Same reasoning as tpm_baseline.c and tpm_seal.c: both are hard
         * integrity failures and are named so a future status cannot inherit
         * this bucket silently. */
        case TPM_NV_RECREATED:
        case TPM_NV_CONTRACT:  return TPM_ATTEST_TPMERR;
        /* Added with the authorized-record work: an operation needing an update
         * authority when none is installed. Named explicitly rather than left
         * to the default arm, which this switch promises is unreachable for
         * every value the enum defines. Not reachable from this module today;
         * the mapping exists so the promise stays true. */
        case TPM_NV_UNAVAIL:   return TPM_ATTEST_TPMERR;
        default:               return TPM_ATTEST_TPMERR;
    }
}

/* Map a provisioning-stage TPM failure: a retryable BUSY/TRANSPORT is preserved
 * so a caller can retry rather than mark attestation permanently unavailable;
 * any other TPM-level failure is a genuine provisioning failure. */
static tpm_attest_status_t at_provision_err(tpm_nv_status_t s)
{
    if (s == TPM_NV_BUSY)      return TPM_ATTEST_BUSY;
    if (s == TPM_NV_TRANSPORT) return TPM_ATTEST_TRANSPORT;
    /* BUDGET means the TPM answered, just slower than this boot will wait -- the
     * same transient class as BUSY, and the caller may retry. Reporting it as
     * PROVISION_FAIL would tell a caller the device cannot provision at all, so
     * a momentarily slow TPM would permanently disable attestation. Named here
     * because the sequence conversion made it REACHABLE: before it, no
     * provisioning command could ever return a cumulative-budget expiry. */
    if (s == TPM_NV_BUDGET)    return TPM_ATTEST_BUSY;
    return TPM_ATTEST_PROVISION_FAIL;
}

/* Like at_provision_err, for a stage that ALLOCATES a handle the caller cannot
 * name on failure (CreatePrimary EK, StartAuthSession, Load AK). BUDGET here
 * means the command's completion is UNKNOWN -- unlike Create (which returns
 * encrypted blobs, not a handle) or PolicySecret (which authorizes an EXISTING
 * session), these three commands can leave a session or transient object
 * allocated with no handle to flush. Reporting BUSY would tell a caller
 * (tpm2_quote's own retry loop, or a userspace attestation client) to retry
 * immediately, which for THESE stages risks allocating a SECOND unnameable
 * handle on top of the first. Same TPMERR trade as tpm_seal.c's
 * map_nv_status_handle: not a literal device fault, but "do not blind-retry",
 * and the small object/session pool turns repeated indeterminate outcomes into
 * a definite, visible refusal within a few attempts on its own. */
static tpm_attest_status_t at_provision_err_handle(tpm_nv_status_t s)
{
    if (s == TPM_NV_BUDGET) {
        /* Same pre-dispatch-vs-in-flight distinction as at_exec_handle. The
         * StartAuthSession call site (via tpm_session_cmd_exec_seq /
         * nv_cmd_exec_common, tpm_nv.c) does NOT disambiguate BUDGET before
         * returning here, unlike at_exec_handle's own CreatePrimary/Load
         * callers -- so the check belongs here too, and checking it
         * unconditionally is safe for every caller: nothing submits another
         * command between the failing submit and this call, so the flag still
         * reflects THAT submit either way. */
        if (!tpm2_seq_last_submit_dispatched())
            return TPM_ATTEST_BUSY;
        return TPM_ATTEST_TPMERR;
    }
    return at_provision_err(s);
}

/* Teardown, delegated to the ONE proof-requiring implementation. The previous
 * body submitted FlushContext and DISCARDED the result, so a transient
 * TPM_RC_RETRY / YIELDED / TESTING -- or a wrong-envelope reply -- left the
 * session or transient object ALLOCATED while this returned as though it had
 * been released. That is the exact defect the bounded-sequence NV work fixed in
 * nv_flush (tpm_nv.c), and it was
 * still live here: every attestation attempt could lose one of the TPM's few
 * slots, until StartAuthSession began refusing outright.
 *
 * tpm_nv_flush_handle spends the sequence's SEPARATE cleanup allowance, so it
 * still runs after the work budget is spent (a single hard deadline would leave
 * a started session unflushable), retries within that allowance, and accepts
 * only proof: SUCCESS, or "no such handle". Factored rather than reimplemented
 * -- a second copy of the proof rules is a second copy to get wrong. */
static void at_flush(tpm2_seq_t seq, uint32_t handle)
{
    tpm_nv_flush_handle(seq, handle);
}

/* Flush an object the TPM may have CREATED even though we could not parse the
 * response describing it.
 *
 * Any post-submit failure can hide a live object: the command reached the TPM,
 * the header says SUCCESS, and only our parse of the envelope failed. If the
 * handle is never recovered a run of malformed responses exhausts the TPM's
 * small transient-object pool and disables attestation for the rest of the
 * boot. Best-effort by construction -- the bytes are already known suspect, so
 * this type-checks the handle and flushes, and a wrong guess costs one refused
 * FlushContext rather than a leak.
 *
 * Every path out of a submit that does not itself return a usable handle goes
 * through here; it exists as one function because there are four such paths and
 * three of them originally forgot. */
static void at_flush_raw_if_created(tpm2_seq_t seq, const uint8_t *rsp, uint32_t len)
{
    uint32_t raw;
    if (!rsp || len < 14u)
        return;                              /* no handle field was received */
    if (tpm2_be32_get(rsp + 6) != TPM2_RC_SUCCESS)
        return;                              /* the TPM reported a failure: nothing was created */
    raw = tpm2_be32_get(rsp + 10);
    if ((uint8_t)(raw >> 24) == 0x80u)       /* TPM_HT_TRANSIENT */
        at_flush(seq, raw);
}

/* Submit a leading-object-handle command (CreatePrimary/Load) -> transient
 * handle, 0 on failure (*out_st classified). Mirrors tpm_seal.c seal_exec_handle
 * incl. the malformed-handle recovery flush. */
static uint32_t at_exec_handle(tpm2_seq_t seq, const uint8_t *cmd, uint32_t n,
                              tpm_nv_status_t *out_st)
{
    /* Static (off-stack): at_exec_handle is reached ONLY from at_provision, which
     * runs under the s_ak_busy mutual-exclusion gate (one CPU at a time), so a
     * shared response buffer is race-free -- and it keeps this 768B off the deep
     * tpm2_quote -> at_provision -> here chain on the 8 KiB kernel stack. */
    static uint8_t rsp[768];
    uint16_t tag;
    uint32_t size, rc, h;
    int r;
    if (n == 0u) { *out_st = TPM_NV_BADARG; return 0; }
    r = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);
    if (r < 0) {
        /* BUDGET must NOT collapse into TRANSPORT. at_provision_err maps
         * TPM_NV_BUDGET to a retryable TPM_ATTEST_BUSY, while TRANSPORT reads as
         * a device fault; without this arm a slow TPM would get a different,
         * NON-retryable public status depending on WHICH provisioning command
         * happened to cross the same sequence deadline.
         *
         * A budget expiry on a handle-PRODUCING command also leaves the outcome
         * unknown -- the TPM may have created the object and we abandoned the
         * response, so no handle exists to flush. Reported rather than silently
         * accepted: the leak degrades on its own into a definite, classified
         * error, whereas poisoning the transport would take out PCR reads too. */
        if (r == TPM_T_ERR_BUDGET) {
            /* Only an actually-DISPATCHED command can have created a handle
             * this caller cannot name. A pre-dispatch refusal (the cumulative
             * budget was already spent before this call touched the
             * interface) submitted nothing and is exactly as safe to retry as
             * ordinary contention -- report it as BUSY, not BUDGET, so every
             * caller of this helper (which always maps through
             * at_provision_err_handle) gets the correct classification. Only a
             * genuinely in-flight abandonment keeps the BUDGET status that
             * routes to the non-retryable class. */
            if (!tpm2_seq_last_submit_dispatched()) {
                *out_st = TPM_NV_BUSY;
                return 0;
            }
            *out_st = TPM_NV_BUDGET;
            klog(LOG_WARN, "TPM",
                 "attest: object-creating command abandoned on budget; "
                 "a transient slot may be held until reset");
            return 0;
        }
        *out_st = (r == TPM_T_ERR_BUSY) ? TPM_NV_BUSY : TPM_NV_TRANSPORT;
        return 0;
    }
    if (tpm2_rsp_parse(rsp, (uint32_t)r, &tag, &size, &rc) != 0) {
        /* An unparsable ENVELOPE can still carry a SUCCESS code and a live
         * handle -- a bad tag alone reaches here -- so recovery runs before the
         * return, not only on the handle-parse failure below. */
        at_flush_raw_if_created(seq, rsp, (uint32_t)r);
        *out_st = TPM_NV_TRANSPORT;
        return 0;
    }
    if (rc != TPM2_RC_SUCCESS) { *out_st = tpm_nv_classify_rc(rc); return 0; }
    h = tpm2_parse_object_handle(rsp, (uint32_t)r);
    if (h == 0u) {
        at_flush_raw_if_created(seq, rsp, (uint32_t)r);
        *out_st = TPM_NV_TRANSPORT;
        return 0;
    }
    *out_st = TPM_NV_OK;
    return h;
}

/* (Re)satisfy the EK authPolicy on `session` via PolicySecret(endorsement). A
 * policy session's digest is CONSUMED on each authorized use, so this MUST run
 * before EVERY EK-authorized command (Create AND Load of the AK). */
static tpm_nv_status_t at_policy_secret(tpm2_seq_t seq, uint32_t session)
{
    uint8_t cmd[48], rsp[64];
    uint32_t n, rlen = 0;
    tpm_nv_status_t st;
    n = tpm2_build_policy_secret(cmd, sizeof cmd, TPM_RH_ENDORSEMENT, session);
    if (n == 0u)
        return TPM_NV_BADARG;
    /* In-sequence: an unsequenced submit here would bounce off the caller's own
     * transport gate and report a spurious TPM_NV_BUSY, which reads as TPM
     * contention rather than a caller bug. Both PolicySecret calls in
     * at_provision reach this. */
    (void)tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0);
    return st;
}

/* TPM2_Load with the parent authorized by a SESSION. The EK is policy-authorized
 * (adminWithPolicy, no userWithAuth), so Load under it MUST carry the policy
 * session that just re-satisfied PolicySecret -- NOT TPM_RS_PW (the generic
 * tpm2_build_load hardcodes TPM_RS_PW and would leave Load unauthorized). */
static uint32_t at_build_load(uint8_t *buf, uint32_t cap, uint32_t parent,
                              uint32_t session, const struct tpm_sealed_blob *blob)
{
    uint32_t total, off, i;
    if (!buf || !blob || blob->priv_len == 0u || blob->pub_len == 0u ||
        blob->priv_len > TPM_SEAL_PRIV_MAX || blob->pub_len > TPM_SEAL_PUB_MAX)
        return 0;
    total = 10u + 4u + 13u + (2u + (uint32_t)blob->priv_len) + (2u + (uint32_t)blob->pub_len);
    if (cap < total)
        return 0;
    tpm2_be16_put(buf + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(buf + 2, total);
    tpm2_be32_put(buf + 6, TPM2_CC_LOAD);
    tpm2_be32_put(buf + 10, parent);
    off = at_put_auth(buf, 14u, session);             /* policy session, not TPM_RS_PW */
    tpm2_be16_put(buf + off, blob->priv_len); off += 2u;
    for (i = 0; i < blob->priv_len; i++) buf[off + i] = blob->priv[i];
    off += blob->priv_len;
    tpm2_be16_put(buf + off, blob->pub_len); off += 2u;
    for (i = 0; i < blob->pub_len; i++) buf[off + i] = blob->pub[i];
    off += blob->pub_len;
    return off;
}

/* Provision the EK + AK once; cache the AK handle + public. Single-cleanup:
 * EK + session are flushed on EVERY path; the AK stays loaded on success. */
static tpm_attest_status_t at_provision(tpm2_seq_t seq)
{
    /* rsp is static (off-stack): at_provision runs only under the s_ak_busy gate
     * (single CPU), so the shared response buffer is race-free and the deep
     * tpm2_quote -> at_provision call chain stays well within the 8 KiB kernel
     * stack (the Load step also stacks a blob-sized lcmd just below). */
    static uint8_t rsp[768];
    uint8_t cmd[256], nonce[16];
    uint32_t ek, session = 0, ak, n, rlen = 0, i;
    tpm_nv_status_t st;
    struct tpm_sealed_blob blob;
    tpm_attest_status_t r;

    if (!tpm_transport_available())
        return TPM_ATTEST_NO_TPM;
    /* 1. CreatePrimary the EK under the endorsement hierarchy. */
    n = tpm2_build_create_primary_ek(cmd, sizeof cmd);
    ek = at_exec_handle(seq, cmd, n, &st);
    if (ek == 0u)
        return at_provision_err_handle(st);   /* CreatePrimary allocates a handle */
    /* 2. Open a real POLICY session. */
    memset(nonce, 0xA5, sizeof nonce);
    n = tpm2_build_start_auth_session(cmd, sizeof cmd, TPM2_SE_POLICY, TPM_ALG_SHA256,
                                      nonce, sizeof nonce);
    if (n == 0u) { r = TPM_ATTEST_PROVISION_FAIL; goto out_ek; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0) {
        r = at_provision_err_handle(st);   /* StartAuthSession allocates a session */
        goto out_ek;
    }
    session = tpm2_parse_start_auth_session(rsp, rlen);
    if (session == 0u) {
        uint32_t raw = tpm2_rsp_session_handle(rsp, rlen);
        if (raw != 0u) at_flush(seq, raw);
        r = TPM_ATTEST_TRANSPORT;
        goto out_ek;
    }
    /* From here flush session + ek on every path. Every stage preserves a
     * retryable BUSY/TRANSPORT (at_provision_err) rather than collapsing it to a
     * permanent PROVISION_FAIL. */
    /* 3. Satisfy the EK policy for Create. */
    st = at_policy_secret(seq, session);
    if (st != TPM_NV_OK) { r = at_provision_err(st); goto out_session; }
    /* 4. Create the AK under the EK (parent auth = the policy session). */
    n = tpm2_build_create_ak_signing(cmd, sizeof cmd, ek, session);
    if (n == 0u) { r = TPM_ATTEST_PROVISION_FAIL; goto out_session; }
    if (tpm_session_cmd_exec_seq(seq, cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0) {
        r = at_provision_err(st); goto out_session;
    }
    if (tpm2_parse_create_sealed(rsp, rlen, &blob) != 0 || blob.pub_len > TPM_AK_PUB_MAX) {
        r = TPM_ATTEST_TRANSPORT; goto out_session;
    }
    /* 5. RE-satisfy the EK policy -- the session was consumed by Create. */
    st = at_policy_secret(seq, session);
    if (st != TPM_NV_OK) { r = at_provision_err(st); goto out_session; }
    /* 6. Load the AK under the EK, authorized by the policy session. A Load of a
     * near-max AK blob (priv+pub up to the parser caps) overflows the 256-byte
     * cmd buffer, so Load gets its own blob-sized buffer. */
    {
        /* Static for the same gate-shaped reason as this file's response
         * buffers: provisioning runs inside ONE bounded sequence (and under
         * s_ak_busy besides), so there is a single live user. At 480 bytes on
         * the deepest attestation chain that is worth keeping off an 8 KiB
         * stack. */
        static uint8_t lcmd[TPM_SEAL_PRIV_MAX + TPM_SEAL_PUB_MAX + 64u];
        n = at_build_load(lcmd, sizeof lcmd, ek, session, &blob);
        ak = at_exec_handle(seq, lcmd, n, &st);
    }
    if (ak == 0u) { r = at_provision_err_handle(st); goto out_session; }  /* Load allocates a handle */
    /* Cache the AK handle + public (the Create outPublic IS the AK TPMT_PUBLIC). */
    for (i = 0; i < blob.pub_len; i++) s_ak_pub[i] = blob.pub[i];
    s_ak_pub_len = blob.pub_len;
    s_ak_handle = ak;
    r = TPM_ATTEST_OK;
out_session:
    at_flush(seq, session);
out_ek:
    /* The EK transient is released here; the AK deliberately stays LOADED past
     * the end of the sequence (s_ak_handle). A sequence bounds TIMING and gate
     * ownership, not handle lifetime -- re-provisioning the AK per quote would
     * cost six commands each time. */
    at_flush(seq, ek);
    return r;
}

/* Run the six-command provisioning flow as ONE bounded sequence.
 *
 * Before this, each of CreatePrimary, StartAuthSession, PolicySecret, Create,
 * PolicySecret and Load took the transport gate separately and armed its own
 * per-command PTP timeout, so a slow-but-responsive TPM could spend that timeout
 * six times over inside what a caller sees as one operation -- and every
 * released gate was a window for another CPU's transaction to interleave
 * mid-provision. One sequence caps the SUM of every wait and makes the flow
 * atomic against other TPM users.
 *
 * The transport gate IS held for the whole flow, which is what makes it atomic.
 * That is affordable here because provisioning happens ONCE per boot (guarded by
 * s_ak_ready) and the sequence is bounded: a concurrent PCR read gets a prompt
 * TPM_T_ERR_BUSY rather than an unbounded block, which is the same answer it
 * would have got from any single command in the old unsequenced flow. */
static int at_provision_seq(tpm2_seq_t seq, void *vctx)
{
    tpm_attest_status_t *out = (tpm_attest_status_t *)vctx;
    *out = at_provision(seq);
    return 0;
}

static tpm_attest_status_t at_provision_run(void)
{
    tpm_attest_status_t r = TPM_ATTEST_TRANSPORT;
    int rc = tpm2_seq_run(tpm_nv_op_budget_ms(), tpm_nv_op_cleanup_ms(),
                          at_provision_seq, &r);
    if (rc != 0)
        return at_provision_err(tpm_nv_seq_start_status(rc));
    return r;
}

/* Ensure the AK is provisioned (once). One CPU provisions; concurrent callers
 * get BUSY (no lock held across the TPM transactions). */
static tpm_attest_status_t at_ensure_ak(void)
{
    tpm_attest_status_t r;
    if (__atomic_load_n(&s_ak_ready, __ATOMIC_ACQUIRE))
        return TPM_ATTEST_OK;
    if (__atomic_exchange_n(&s_ak_busy, 1, __ATOMIC_ACQ_REL))
        return TPM_ATTEST_BUSY;            /* another CPU is provisioning */
    /* Double-check ready UNDER the busy gate: a CPU that read ready==0 above can
     * stall while another CPU fully provisions, publishes the AK (ready=1), and
     * clears busy -- then this CPU wins the exchange and would re-enter
     * at_provision(), overwriting the live cache (a torn handle/pub read for a
     * third CPU + a leaked old AK transient handle). Re-checking closes that. */
    if (__atomic_load_n(&s_ak_ready, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_ak_busy, 0, __ATOMIC_RELEASE);
        return TPM_ATTEST_OK;
    }
    r = at_provision_run();
    if (r == TPM_ATTEST_OK)
        __atomic_store_n(&s_ak_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ak_busy, 0, __ATOMIC_RELEASE);
    return r;
}

tpm_attest_status_t tpm2_quote(uint32_t pcr_mask, const uint8_t *nonce,
                               uint16_t nonce_len, struct tpm_quote_attest *out,
                               uint8_t *sig_out, uint32_t sig_cap, uint32_t *sig_len)
{
    uint8_t cmd[128], rsp[512];
    uint32_t n, rlen = 0, i;
    tpm_nv_status_t st;
    tpm_attest_status_t pr;
    if (!nonce || nonce_len < TPM_QUOTE_NONCE_MIN || nonce_len > TPM_QUOTE_NONCE_MAX ||
        !out || !sig_out)
        return TPM_ATTEST_BADARG;
    /* This path quotes a single SHA-256 bank (PCRs 0-23, a 3-octet selection); a
     * pcr_mask bit >= 24 is not representable and would be silently dropped, then
     * accepted by the bind check below over a quote that never covered it. Reject it
     * up front as a caller error rather than attesting a different PCR set. */
    if (pcr_mask & ~0xFFFFFFu)
        return TPM_ATTEST_BADARG;
    if (!tpm_transport_available())
        return TPM_ATTEST_NO_TPM;
    /* Preflight output capacity BEFORE the TPM side effect (provisioning + issuing
     * the quote): this path always builds an ECDSA-P256/SHA256 quote, whose
     * TPMT_SIGNATURE is at most TPM_SIG_ECDSA_P256_MAX bytes -- NOT the RSA-sized
     * TPM_SIG_MAX. Requiring the RSA max rejected legitimate ECDSA callers (and the
     * 64/128-byte test buffers). An undersized sig_out is a caller BADARG, never a
     * post-quote QUOTE_FAIL that would make a buffer bug look like a TPM failure.
     * NO_TPM still precedes this: probing attestation availability must not require
     * a full-size signature buffer. */
    if (sig_cap < TPM_SIG_ECDSA_P256_MAX)
        return TPM_ATTEST_BADARG;
    pr = at_ensure_ak();
    if (pr != TPM_ATTEST_OK)
        return pr;
    /* AK uses userWithAuth -> empty password (TPM_RS_PW). */
    n = tpm2_build_quote(cmd, sizeof cmd, s_ak_handle, TPM_RS_PW, TPM_ALG_ECDSA,
                         nonce, nonce_len, pcr_mask);
    if (n == 0u)
        return TPM_ATTEST_BADARG;
    if (tpm_session_cmd_exec(cmd, n, rsp, sizeof rsp, &rlen, &st, 0) != 0) {
        if (st == TPM_NV_BUSY) return TPM_ATTEST_BUSY;
        if (st == TPM_NV_TRANSPORT) return TPM_ATTEST_TRANSPORT;
        return TPM_ATTEST_QUOTE_FAIL;
    }
    if (tpm2_parse_quote(rsp, rlen, out, sig_out, sig_cap, sig_len) != 0)
        return TPM_ATTEST_QUOTE_FAIL;
    /* Bind the response to the request: a quote reported OK MUST cover exactly the
     * requested PCR selection and carry the requested ECDSA/SHA256 scheme. The
     * parser only validates structure -- it accepts a structurally valid RSASSA or
     * wrong-PCR quote. A desynchronized or malformed TPM response that parses but
     * quotes the wrong PCRs / wrong alg must NOT be published as a valid attestation
     * (the attestation-report exporter would otherwise claim PCRs the TPM never
     * signed). The parser guarantees sig_out holds >= 4 bytes (sigAlg+hashAlg). */
    if (out->pcr_select != (pcr_mask & 0xFFFFFFu))
        return TPM_ATTEST_QUOTE_FAIL;
    if (tpm2_be16_get(sig_out) != TPM_ALG_ECDSA ||
        tpm2_be16_get(sig_out + 2u) != TPM_ALG_SHA256)
        return TPM_ATTEST_QUOTE_FAIL;
    /* Anti-replay: the attest MUST echo the supplied nonce exactly. */
    if (out->nonce_len != nonce_len)
        return TPM_ATTEST_NONCE_STALE;
    for (i = 0; i < nonce_len; i++)
        if (out->nonce[i] != nonce[i])
            return TPM_ATTEST_NONCE_STALE;
    return TPM_ATTEST_OK;
}

tpm_attest_status_t tpm_ak_public_get(uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    tpm_attest_status_t pr;
    uint16_t i;
    if (out_len) *out_len = 0;
    if (!out)
        return TPM_ATTEST_BADARG;
    pr = at_ensure_ak();
    if (pr != TPM_ATTEST_OK)
        return pr;
    if (s_ak_pub_len > cap)
        return TPM_ATTEST_BADARG;
    for (i = 0; i < s_ak_pub_len; i++) out[i] = s_ak_pub[i];
    if (out_len) *out_len = s_ak_pub_len;
    return TPM_ATTEST_OK;
}

int tpm2_parse_create_primary_public(const uint8_t *rsp, uint32_t len,
                                     uint8_t *out, uint16_t cap,
                                     uint16_t *out_len)
{
    uint16_t tag, pub_len;
    uint32_t size, rc, psize, auth_off, i;

    if (out_len) *out_len = 0;
    if (!rsp || !out)
        return -1;
    /* The envelope is validated here in full rather than trusted from a prior
     * tpm2_parse_object_handle call: this function is reachable on its own, and
     * a parser that assumes someone else already bounded the response is one
     * refactor away from reading past it. */
    if (tpm2_rsp_parse(rsp, len, &tag, &size, &rc) != 0 || rc != TPM2_RC_SUCCESS)
        return -1;
    if (tag != TPM2_ST_SESSIONS)
        return -1;
    if (size < 10u + 4u + 4u + 5u)          /* header + handle + psize + auth */
        return -1;
    psize = tpm2_be32_get(rsp + 14);        /* parameterSize, after objectHandle@10 */
    if (psize > size - 18u)
        return -1;
    auth_off = 18u + psize;
    if (size - auth_off < 5u)
        return -1;
    if (!tpm_session_auth_response_ok(rsp, size, auth_off))
        return -1;
    /* The objectHandle must be a TRANSIENT object (high byte 0x80). A success
     * naming anything else is a malformed response, and this function's banner
     * claims to validate the envelope in full -- so it checks the handle here
     * rather than leaving that to whoever happens to call it next. */
    if ((uint8_t)(tpm2_be32_get(rsp + 10) >> 24) != 0x80u)
        return -1;
    /* outPublic is the FIRST parameter: TPM2B_PUBLIC = size(2) + TPMT_PUBLIC. */
    if (psize < 2u)
        return -1;
    pub_len = tpm2_be16_get(rsp + 18);
    if ((uint32_t)pub_len + 2u > psize)     /* must fit inside the parameter area */
        return -1;
    /* A TPMT_PUBLIC cannot be shorter than type + nameAlg + attrs + an empty
     * authPolicy. Below that it is not a truncated key, it is a response that
     * never carried one, and hashing it would cache a device identity derived
     * from a couple of arbitrary bytes. The full template is deliberately NOT
     * pinned here: the identity is THIS TPM's EK, so demanding one particular
     * type or curve would refuse legitimate firmware on the correct machine. */
    if (pub_len < TPM_PUBLIC_MIN_LEN || pub_len > cap)
        return -1;
    for (i = 0; i < (uint32_t)pub_len; i++)
        out[i] = rsp[20u + i];
    if (out_len) *out_len = pub_len;
    return 0;
}

/* CreatePrimary the EK, capture its public area, and flush the transient handle
 * on EVERY path. The handle is not retained: the caller wants the identity, and
 * a leaked transient exhausts the TPM's small object pool within a few boots of
 * a failing path. Runs under the s_ak_busy gate, so the static response buffer
 * is race-free and stays off the stack. */
/* Capture the EK public. CreatePrimary plus its teardown run inside their OWN
 * bounded sequence: this path is reached from tpm_ek_public_get, NOT from
 * at_provision, so it owns no sequence of its own -- and the verified teardown
 * submits on the sequence's cleanup allowance, which does not exist outside one.
 * Passing 0 here would leave every EK transient un-flushed on the success path,
 * exhausting the TPM's small object pool after a handful of calls. */
static int at_capture_ek_public_seq(tpm2_seq_t seq, void *vctx)
{
    tpm_attest_status_t *out = (tpm_attest_status_t *)vctx;
    static uint8_t rsp[768];
    uint8_t cmd[256];
    uint16_t tag, plen = 0;
    uint32_t n, size, rc, ek;
    int r;

    n = tpm2_build_create_primary_ek(cmd, sizeof cmd);
    if (n == 0u) { *out = TPM_ATTEST_PROVISION_FAIL; return 0; }
    r = tpm2_submit_seq(seq, cmd, n, rsp, sizeof rsp);
    if (r < 0) {
        /* Same BUDGET distinction as at_exec_handle: a cumulative expiry is the
         * retryable class, and on this object-CREATING command its completion is
         * unknown, so the possible allocation is reported rather than dropped. */
        if (r == TPM_T_ERR_BUDGET) {
            /* Same pre-dispatch-vs-in-flight distinction as at_exec_handle:
             * only a DISPATCHED CreatePrimary can have allocated an EK
             * transient this caller cannot name. */
            if (!tpm2_seq_last_submit_dispatched()) {
                *out = TPM_ATTEST_BUSY;
                return 0;
            }
            klog(LOG_WARN, "TPM",
                 "attest: EK capture abandoned on budget; "
                 "a transient slot may be held until reset");
            /* TPMERR, not BUSY: CreatePrimary DISPATCHED and allocates a
             * handle, and this budget expiry's completion is unknown -- see
             * at_provision_err_handle for the full rationale. */
            *out = TPM_ATTEST_TPMERR;
            return 0;
        }
        *out = (r == TPM_T_ERR_BUSY) ? TPM_ATTEST_BUSY : TPM_ATTEST_TRANSPORT;
        return 0;
    }
    if (tpm2_rsp_parse(rsp, (uint32_t)r, &tag, &size, &rc) != 0) {
        /* A bad TAG lands here with a SUCCESS code and a live handle still in
         * the response, so this path needs the same recovery as the one below.
         * The re-adversarial round caught it missing. */
        at_flush_raw_if_created(seq, rsp, (uint32_t)r);
        *out = TPM_ATTEST_TRANSPORT;
        return 0;
    }
    if (rc != TPM2_RC_SUCCESS) {
        *out = at_provision_err(tpm_nv_classify_rc(rc));
        return 0;
    }
    ek = tpm2_parse_object_handle(rsp, (uint32_t)r);
    if (ek == 0u) {
        at_flush_raw_if_created(seq, rsp, (uint32_t)r);
        *out = TPM_ATTEST_TRANSPORT;
        return 0;
    }
    if (tpm2_parse_create_primary_public(rsp, (uint32_t)r, s_ek_pub,
                                         (uint16_t)sizeof s_ek_pub, &plen) != 0) {
        at_flush(seq, ek);
        *out = TPM_ATTEST_TRANSPORT;
        return 0;
    }
    at_flush(seq, ek);
    s_ek_pub_len = plen;
    *out = TPM_ATTEST_OK;
    return 0;
}

static tpm_attest_status_t at_capture_ek_public(void)
{
    tpm_attest_status_t r = TPM_ATTEST_TRANSPORT;
    int rc;

    s_ek_pub_len = 0u;
    if (!tpm_transport_available())
        return TPM_ATTEST_NO_TPM;
    rc = tpm2_seq_run(tpm_nv_op_budget_ms(), tpm_nv_op_cleanup_ms(),
                      at_capture_ek_public_seq, &r);
    if (rc != 0)
        return at_provision_err(tpm_nv_seq_start_status(rc));
    return r;
}

tpm_attest_status_t tpm_ek_public_get(uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    tpm_attest_status_t r;
    uint16_t i;

    if (out_len) *out_len = 0;
    if (!out)
        return TPM_ATTEST_BADARG;
    if (!__atomic_load_n(&s_ek_ready, __ATOMIC_ACQUIRE)) {
        if (__atomic_exchange_n(&s_ak_busy, 1, __ATOMIC_ACQ_REL))
            return TPM_ATTEST_BUSY;         /* another CPU holds the conversation */
        /* Re-check UNDER the gate, exactly as at_ensure_ak does: a CPU that read
         * ready==0 above can stall while another publishes and clears busy. */
        if (__atomic_load_n(&s_ek_ready, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s_ak_busy, 0, __ATOMIC_RELEASE);
        } else {
            r = at_capture_ek_public();
            if (r == TPM_ATTEST_OK)
                __atomic_store_n(&s_ek_ready, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&s_ak_busy, 0, __ATOMIC_RELEASE);
            if (r != TPM_ATTEST_OK)
                return r;
        }
    }
    if (s_ek_pub_len == 0u)
        return TPM_ATTEST_TRANSPORT;
    if (s_ek_pub_len > cap)
        return TPM_ATTEST_BADARG;
    for (i = 0; i < s_ek_pub_len; i++)
        out[i] = s_ek_pub[i];
    if (out_len) *out_len = s_ek_pub_len;
    return TPM_ATTEST_OK;
}

tpm_attest_status_t tpm_ek_cert_read(uint16_t alg, uint8_t *out, uint16_t cap,
                                     uint16_t *out_len)
{
    uint32_t idx;
    uint16_t size = 0, off = 0;
    uint32_t attrs = 0;
    tpm_nv_status_t st;
    if (out_len) *out_len = 0;
    /* Only the two real EK key types select an index; mapping every other alg to
     * the RSA index would silently return the wrong trust chain (e.g. a caller
     * passing TPM_ALG_SHA256 must not get the RSA EK cert reported as OK). */
    if (!out || cap == 0u || (alg != TPM_ALG_ECC && alg != TPM_ALG_RSA))
        return TPM_ATTEST_BADARG;
    if (!tpm_transport_available())
        return TPM_ATTEST_NO_TPM;
    idx = (alg == TPM_ALG_ECC) ? TPM_NV_INDEX_EK_CERT_ECC : TPM_NV_INDEX_EK_CERT_RSA;
    /* Size the index first; absent on many vTPM/fTPM -> degrade. */
    st = tpm_nv_read_public(idx, &size, &attrs);
    if (st == TPM_NV_NOTFOUND)
        return TPM_ATTEST_NO_EK_CERT;
    if (st != TPM_NV_OK)
        return map_attest(st);
    if (size == 0u)
        return TPM_ATTEST_NO_EK_CERT;
    if (size > cap) {
        /* Never return a TRUNCATED DER as OK -- a caller could publish an
         * unverifiable trust chain. Report the required size so the caller can
         * retry with a big-enough buffer. */
        if (out_len) *out_len = size;
        return TPM_ATTEST_BADARG;
    }
    /* Chunked read (an EK cert is ~1 KiB, over the per-op TPM_NV_MAX_DATA cap). */
    while (off < size) {
        uint16_t chunk = (uint16_t)((size - off > TPM_NV_MAX_DATA)
                                        ? TPM_NV_MAX_DATA : (size - off));
        uint16_t rd = 0;
        st = tpm_nv_read(idx, off, out + off, chunk, &rd);
        if (st != TPM_NV_OK)
            return map_attest(st);
        if (rd == 0u || rd > chunk)                    /* no progress / overlong -> truncation */
            return TPM_ATTEST_TRANSPORT;
        off += rd;
    }
    if (out_len) *out_len = off;
    /* OK only on a FULLY-read advertised-size cert -- never a truncated DER (a
     * verifier must not be fed a partial EK chain reported as success). */
    return (off == size) ? TPM_ATTEST_OK : TPM_ATTEST_TRANSPORT;
}

/* ---- Test seam ---- */

/* Drop the cached AK so the next attestation entry point re-provisions from
 * scratch. Lets each unit test exercise the full EK->AK provisioning path in any
 * order instead of inheriting a sibling test's cache (mirrors tpm_t_test_install
 * in the transport layer). Not on any production path.
 *
 * CONTRACT: quiescent, single-threaded unit-test setup ONLY -- the caller
 * guarantees no attestation is in flight on any CPU (tests run sequentially on
 * one CPU). It is NOT a concurrency-safe runtime reset: cache fields are cleared
 * first and the s_ak_busy gate is RELEASEd last so no later store can publish a
 * provisioned handle behind a freed gate, but a truly concurrent provisioner
 * would still race -- which the quiescent contract forbids. */
#ifdef KERNEL_TESTS
void tpm_attest_test_reset(void)
{
    s_ak_handle = 0u;
    s_ak_pub_len = 0u;
    __atomic_store_n(&s_ak_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ak_busy, 0, __ATOMIC_RELEASE);
    s_ek_pub_len = 0u;
    __atomic_store_n(&s_ek_ready, 0, __ATOMIC_RELEASE);
}
#endif
