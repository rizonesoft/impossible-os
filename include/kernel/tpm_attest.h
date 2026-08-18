/* ============================================================================
 * tpm_attest.h -- TPM2 attestation: EK cert, AK provisioning, TPM2_Quote
 *
 * The TPM-rooted signing mechanism a remote-verifiable attestation report needs.
 * A software-signed report cannot prove the PCRs came from THIS machine's TPM;
 * Win11 Device Health Attestation and Linux Keylime both root trust in a
 * TPM-resident Attestation Key (AK) bound to the Endorsement Key (EK), and
 * TPM2_Quote (the TPM signs the selected-PCR digest + a verifier nonce).
 *
 * Flow:
 *   - EK cert: read from the standard NV indices (RSA 0x01c00002, ECC
 *     0x01c0000a); absent on many vTPM/fTPM -> degrade, never gate boot.
 *   - AK provision: CreatePrimary the EK under the ENDORSEMENT hierarchy (the EK
 *     uses POLICY auth -- adminWithPolicy, NOT a password -- so authorizing it
 *     requires a real policy session that satisfies TPM2_PolicySecret over
 *     TPM_RH_ENDORSEMENT), then Create + Load a restricted SIGNING AK under it.
 *   - Quote: TPM2_Quote(AK, pcrSelect, nonce) -> TPMS_ATTEST + signature. The
 *     attest embeds the verifier nonce (anti-replay) and the TPM clockInfo
 *     (resetCount/restartCount) so a verifier can detect stale/replayed quotes.
 *
 * The pure marshaling builders/parsers are MMIO-free and fixture-tested; the
 * live wrappers drive the Phase-1 transport over the shared tpm_nv.c policy seam
 * and must NOT run in ISR context. Live quote+verify is swtpm/bare-metal.
 *
 * Reuses the tpm_seal.c / tpm_nv.c seam: tpm_session_cmd_exec,
 * tpm_session_auth_response_ok, tpm2_parse_object_handle, tpm_policy_session_run,
 * the BE marshaling helpers, and the single-cleanup handle-teardown discipline.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Hierarchies + command codes (TPM 2.0 Part 2) not already in tpm_seal.h --- */
#define TPM_RH_ENDORSEMENT        0x4000000Bu  /* endorsement hierarchy (EK parent) */
#define TPM2_CC_EVICT_CONTROL     0x00000120u
#define TPM2_CC_POLICY_SECRET     0x00000151u
#define TPM2_CC_QUOTE             0x00000158u
#define TPM2_CC_MAKE_CREDENTIAL   0x00000168u
#define TPM2_CC_ACTIVATE_CREDENTIAL 0x00000147u

/* ---- Attestation structure tags (TPM 2.0 Part 2) ---- */
#define TPM2_GENERATED_VALUE      0xFF544347u  /* "TCG" magic on a TPM-generated attest */
#define TPM2_ST_ATTEST_QUOTE      0x8018u

/* ---- Signature schemes (TPMI_ALG_SIG_SCHEME) ---- */
#define TPM_ALG_RSASSA            0x0014u
#define TPM_ALG_ECDSA             0x0018u

/* ---- Standard EK NV-index handles (TCG EK Credential Profile) ---- */
#define TPM_NV_INDEX_EK_CERT_RSA  0x01C00002u
#define TPM_NV_INDEX_EK_CERT_ECC  0x01C0000Au

/* ---- Standard ECC-P256 low-range EK template (TCG EK Credential Profile) ----
 * objectAttributes: fixedTPM | fixedParent | sensitiveDataOrigin | adminWithPolicy
 * | restricted | decrypt. Note: NO userWithAuth (the EK is policy-authorized),
 * and the unique field is 32 zero bytes per coordinate (NOT empty) -- both are
 * load-bearing: a different attrs/unique yields a different EK than every
 * verifier expects. */
#define TPM_EK_OBJECT_ATTRS       0x000300B2u

/* Restricted ECDSA-P256 signing-AK attributes: fixedTPM | fixedParent |
 * sensitiveDataOrigin | userWithAuth | noDA | restricted | sign. userWithAuth
 * (not a policy) so the AK signs under an empty password; restricted + sign + a
 * fixed ECDSA scheme is what makes it a valid quote-signing key. */
#define TPM_AK_OBJECT_ATTRS       0x00050472u

/* The well-known EK authPolicy (PolicyA) = TPM2_PolicySecret(TPM_RH_ENDORSEMENT)
 * digest under SHA-256, published in the TCG EK Credential Profile. The EK's
 * authorization is satisfied only by a policy session carrying this digest. */
#define TPM_EK_POLICY_A_LEN 32u
extern const uint8_t TPM_EK_POLICY_A_SHA256[TPM_EK_POLICY_A_LEN];

/* ---- Bounds ---- */
#define TPM_QUOTE_NONCE_MIN   8u    /* reject quote requests with a too-short/absent nonce */
#define TPM_QUOTE_NONCE_MAX   64u   /* TPM2B_DATA / digest-sized */
#define TPM_ATTEST_MAX        320u  /* a SHA-256 single-bank quote attest is well under this */
#define TPM_SIG_MAX           288u  /* RSA-2048 sig (256) or ECDSA-P256 (r||s, 64) + tags */
#define TPM_SIG_ECDSA_P256_MAX 72u  /* sigAlg(2)+hashAlg(2)+sigR(2+32)+sigS(2+32); only scheme tpm2_quote makes */
#define TPM_AK_PUB_MAX        160u  /* marshaled AK TPMT_PUBLIC */
#define TPM_EK_PUB_MAX        160u  /* marshaled EK TPMT_PUBLIC (ECC P-256 template is 122) */
#define TPM_PUBLIC_MIN_LEN     10u  /* type(2)+nameAlg(2)+attrs(4)+empty authPolicy(2) */
#define TPM_EK_CERT_MAX       1024u /* DER EK cert chunk we read from NV */

/* ---- Classified outcome (degraded states never gate boot) ---- */
typedef enum {
    TPM_ATTEST_OK            = 0,
    TPM_ATTEST_BADARG        = 1,
    TPM_ATTEST_NO_TPM        = 2,  /* transport unavailable */
    TPM_ATTEST_NO_EK_CERT    = 3,  /* EK cert NV index absent (vTPM/fTPM) */
    TPM_ATTEST_PROVISION_FAIL = 4, /* EK/AK provisioning failed */
    TPM_ATTEST_QUOTE_FAIL    = 5,  /* TPM2_Quote failed / malformed attest */
    TPM_ATTEST_NONCE_STALE   = 6,  /* attest extraData != supplied nonce (replay) */
    TPM_ATTEST_TRANSPORT     = 7,  /* transport failure / malformed response */
    TPM_ATTEST_BUSY          = 8,
    TPM_ATTEST_TPMERR        = 9,
    /* THIS KERNEL's build-time `.bootproto` ABI identity failed validation, so
     * no honest manifest identity can be exported. Distinct from every status
     * above: those describe the TPM or the caller, this one describes corrupt
     * read-only kernel data, and a verifier must not read the resulting report
     * as merely degraded evidence. */
    TPM_ATTEST_SELF_CORRUPT  = 10,
} tpm_attest_status_t;

/* Parsed TPMS_ATTEST quote fields a verifier / report consumer needs. */
struct tpm_quote_attest {
    uint8_t  nonce[TPM_QUOTE_NONCE_MAX]; /* extraData (echoed verifier nonce) */
    uint16_t nonce_len;
    uint8_t  pcr_digest[64];             /* TPMS_QUOTE_INFO.pcrDigest */
    uint16_t pcr_digest_len;
    uint32_t pcr_select;                 /* echoed PCR selection bitmap (bits 0..23) */
    uint64_t clock;                      /* clockInfo.clock (ms) */
    uint32_t reset_count;                /* clockInfo.resetCount */
    uint32_t restart_count;              /* clockInfo.restartCount */
    uint8_t  safe;                       /* clockInfo.safe */
    uint64_t firmware_version;
    /* The EXACT TPMS_ATTEST bytes the TPM signed (the verifier's signature
     * input -- the signature is over these bytes, NOT over the parsed fields
     * above). A remote verifier needs this raw blob + the AK public + the
     * signature to verify the quote; it also carries qualifiedSigner (AK name). */
    uint8_t  attest_raw[TPM_ATTEST_MAX];
    uint16_t attest_raw_len;
};

/* ---- Pure marshaling seam (MMIO-free, fixture-tested) ----
 * Each builder returns the marshaled command length, or 0 on bad arg / too-small
 * buffer. Each parser returns 0 on success, -1 on malformed/failed. */

/* TPM2_CreatePrimary of the standard ECC-P256 EK under the endorsement hierarchy.
 * Authorized by the ENDORSEMENT-HIERARCHY auth (empty password by default ->
 * TPM_RS_PW); the EK's own authPolicy (PolicyA) governs later USE of the EK as a
 * parent, not its creation. */
uint32_t tpm2_build_create_primary_ek(uint8_t *buf, uint32_t cap);

/* TPM2_PolicySecret(authHandle=TPM_RH_ENDORSEMENT, policySession). Satisfies the
 * EK authPolicy so the policy session can authorize EK use. nonceTPM/cpHashA/
 * policyRef/expiration are empty (endorsement has no auth value by default). */
uint32_t tpm2_build_policy_secret(uint8_t *buf, uint32_t cap, uint32_t auth_handle,
                                  uint32_t policy_session);

/* TPM2_Create of a restricted SIGNING AK (ECDSA P256) under `parent` (the loaded
 * EK), authorized by a policy session (the EK requires policy auth for Create). */
uint32_t tpm2_build_create_ak_signing(uint8_t *buf, uint32_t cap, uint32_t parent,
                                      uint32_t auth_session);

/* TPM2_Quote(signHandle=AK, qualifyingData=nonce, inScheme=ECDSA/SHA256,
 * pcrSelect from `pcr_mask`). The AK auth area uses the supplied session. */
uint32_t tpm2_build_quote(uint8_t *buf, uint32_t cap, uint32_t ak_handle,
                          uint32_t auth_session, uint16_t sig_alg,
                          const uint8_t *nonce, uint16_t nonce_len, uint32_t pcr_mask);

/* Parse a TPM2_Quote response: validates the TPM2B_ATTEST (magic TPM2_GENERATED,
 * type ST_ATTEST_QUOTE), extracts extraData/clockInfo/firmwareVersion and the
 * TPMS_QUOTE_INFO pcrSelect+pcrDigest into `out`, and copies the raw TPMT_
 * SIGNATURE to sig_out. Returns 0 on success, -1 on malformed/failed. */
int tpm2_parse_quote(const uint8_t *rsp, uint32_t len, struct tpm_quote_attest *out,
                     uint8_t *sig_out, uint32_t sig_cap, uint32_t *sig_len);

/* TPM2_EvictControl(auth=TPM_RH_OWNER, object=transient, persistent=handle). */
uint32_t tpm2_build_evict_control(uint8_t *buf, uint32_t cap, uint32_t object,
                                  uint32_t persistent);

/* ---- High-level query APIs (Phase-1 transport; not ISR-safe) ----
 * These are what the attestation-report export layer calls to obtain the
 * TPM-rooted quote, AK public, and EK-cert chain. */

/* Read the EK certificate (DER) from the standard NV index for `alg` (RSA/ECC).
 * Returns NO_EK_CERT when the index is absent (common on vTPM/fTPM). */
tpm_attest_status_t tpm_ek_cert_read(uint16_t alg, uint8_t *out, uint16_t cap,
                                     uint16_t *out_len);

/* Produce a TPM2_Quote over `pcr_mask` bound to `nonce` (>= TPM_QUOTE_NONCE_MIN).
 * Provisions the EK+AK on demand. Fills `out` (the parsed attest) + the raw
 * signature; verifies the echoed nonce matches (anti-replay) before returning OK. */
tpm_attest_status_t tpm2_quote(uint32_t pcr_mask, const uint8_t *nonce,
                               uint16_t nonce_len, struct tpm_quote_attest *out,
                               uint8_t *sig_out, uint32_t sig_cap, uint32_t *sig_len);

/* Marshaled AK public (TPMT_PUBLIC) captured at provisioning, for the verifier. */
tpm_attest_status_t tpm_ak_public_get(uint8_t *out, uint16_t cap, uint16_t *out_len);

/* Marshaled EK PRIMARY public (TPMT_PUBLIC), cached for the boot.
 *
 * This is the STABLE device identity, and the AK deliberately is not: the AK is
 * TPM2_Created fresh under the EK on every boot (at_provision), so a digest over
 * it differs each time. The EK primary is derived from the endorsement seed, so
 * it is reproducible across reboots and changes on TPM2_Clear.
 *
 * Creates the EK primary on first use and flushes the transient handle
 * immediately; it does NOT provision the AK, because a caller that only needs
 * the machine's identity should not pay a Create plus a Load for it.
 *
 * Returns TPM_ATTEST_OK, NO_TPM when the transport is unavailable, BADARG on a
 * NULL buffer or one smaller than the public area, and TRANSPORT on a malformed
 * response. */
tpm_attest_status_t tpm_ek_public_get(uint8_t *out, uint16_t cap, uint16_t *out_len);

/* Parse the outPublic parameter of a TPM2_CreatePrimary success response.
 * Exposed for unit tests: a wrong bound here would surface only on real
 * firmware. Returns 0 on success, -1 on any malformed or over-long response. */
int tpm2_parse_create_primary_public(const uint8_t *rsp, uint32_t len,
                                     uint8_t *out, uint16_t cap,
                                     uint16_t *out_len);

/* Test seam: drop the cached AK so the next call re-provisions. Unit-test only.
 * Guarded out of release builds (release test-surface exclusion). */
#ifdef KERNEL_TESTS
void tpm_attest_test_reset(void);
#endif
