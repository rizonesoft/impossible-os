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
    out->pcr_select = 0u;
    for (i = 0; i < count; i++) {
        if (a + 3u > end_a) return -1;
        a += 2u;                                       /* hashAlg */
        sos = rsp[a]; a += 1u;
        if ((uint32_t)sos > end_a - a) return -1;
        if (i == 0u) {                                 /* fold the first bank's bitmap */
            uint32_t k;
            for (k = 0; k < sos && k < 3u; k++)
                out->pcr_select |= ((uint32_t)rsp[a + k]) << (8u * k);
        }
        a += sos;
    }
    if (a + 2u > end_a) return -1;
    pd = tpm2_be16_get(rsp + a); a += 2u;
    if ((uint32_t)pd > end_a - a || (uint32_t)pd > sizeof out->pcr_digest) return -1;
    for (i = 0; i < pd; i++) out->pcr_digest[i] = rsp[a + i];
    out->pcr_digest_len = pd;
    a += pd;
    if (a != end_a)                                    /* exact attest consumption */
        return -1;
    /* TPMT_SIGNATURE = the rest of the parameter area (raw, for the verifier). */
    sig_off = end_a;                                   /* == poff + 2 + attest_size */
    slen = pend - sig_off;
    if (slen < 4u || slen > sig_cap)                   /* >= sigAlg(2) + hashAlg(2) */
        return -1;
    for (i = 0; i < slen; i++) sig_out[i] = rsp[sig_off + i];
    if (sig_len) *sig_len = slen;
    return 0;
}
