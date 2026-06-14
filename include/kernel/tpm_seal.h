/* ============================================================================
 * tpm_seal.h -- TPM2 sealed-secret boot policy hooks (measured boot)
 *
 * Seals a small secret (FDE unlock key, code-integrity policy hash, ...) to the
 * platform's CURRENT PCR state so it can only be unsealed while the machine
 * boots the same measured firmware/Secure-Boot configuration. The seal binds to
 * the DERIVED seal PCR mask (tpm_pcr_seal_mask(), PCR 7 only -- PCR 11 is
 * deliberately EXCLUDED so a kernel rebuild / .bootproto-manifest change does
 * NOT brick FDE unlock; see tpm_pcr_alloc.h).
 *
 * Flow (both seal and unseal regenerate the parent deterministically):
 *   seal   : CreatePrimary(SRK template) -> Create(KEYEDHASH sealed object,
 *            authPolicy = trial PolicyPCR(seal mask)) -> flush primary -> blob.
 *   unseal : CreatePrimary(SRK template) -> Load(blob) -> flush primary ->
 *            real PolicyPCR session -> Unseal -> flush object + session.
 * A fixed CreatePrimary template regenerates the same storage parent from the
 * TPM primary seed, so only the small sealed blob (outPrivate + outPublic) is
 * persisted by the OS; the parent is never stored. Every transport path routes
 * its primary/object/session handles through a single cleanup so an error or
 * BUSY cannot leak a handle into the TPM's small object/session pool.
 *
 * The pure marshaling builders/parsers are MMIO-free and fixture-tested; the
 * tpm_seal_* / tpm_unseal_* wrappers drive the Phase-1 transport and must NOT
 * run in ISR context (the transport serializes whole transactions and can block
 * for a TPM duration). The live seal -> PCR-change -> deny cycle is swtpm /
 * bare-metal validation.
 *
 * Threat-model scope: the seam validates response STRUCTURE (rejects malformed /
 * truncated / wrong-tag responses -- the realistic local failure mode). It does
 * NOT cryptographically authenticate or encrypt the TPM session, so a physical
 * bus interposer that forges a well-formed success or sniffs the unsealed key on
 * the wire is out of scope here; that needs salted/bound HMAC sessions +
 * parameter encryption (BitLocker-style bus protection), a separate transport-
 * wide feature tracked as a TODO-13 section-8 follow-up.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Object/sealing command codes (TPM 2.0 Part 2) ---- */
#define TPM2_CC_CREATE_PRIMARY 0x00000131u
#define TPM2_CC_CREATE         0x00000153u
#define TPM2_CC_LOAD           0x00000157u
#define TPM2_CC_UNSEAL         0x0000015Eu

/* ---- Algorithm identifiers used by the SRK template + sealed object ---- */
#define TPM_ALG_KEYEDHASH 0x0008u  /* sealed-data object type */
#define TPM_ALG_AES       0x0006u  /* SRK symmetric algorithm */
#define TPM_ALG_RSA       0x0001u  /* RSA key type (EK-cert NV-index selector) */
#define TPM_ALG_ECC       0x0023u  /* SRK asymmetric algorithm */
#define TPM_ALG_CFB       0x0043u  /* SRK symmetric mode */
#define TPM_ECC_NIST_P256 0x0003u  /* SRK curve */

/* ---- TPMA_OBJECT attribute bits (TPM 2.0 Part 2 Table 31) ---- */
#define TPMA_OBJ_FIXED_TPM            (1u << 1)
#define TPMA_OBJ_FIXED_PARENT         (1u << 4)
#define TPMA_OBJ_SENSITIVE_DATA_ORIGIN (1u << 5)
#define TPMA_OBJ_USER_WITH_AUTH       (1u << 6)
#define TPMA_OBJ_NO_DA                (1u << 10)
#define TPMA_OBJ_RESTRICTED           (1u << 16)
#define TPMA_OBJ_DECRYPT              (1u << 17)

/* Standard TCG storage-root-key template attributes: a fixed, non-duplicable,
 * password-authorized, restricted decryption (storage) parent. A CreatePrimary
 * with this template + an empty unique point regenerates the same parent from
 * the owner primary seed on every call. */
#define TPM_SRK_OBJECT_ATTRS                                                  \
    (TPMA_OBJ_FIXED_TPM | TPMA_OBJ_FIXED_PARENT |                             \
     TPMA_OBJ_SENSITIVE_DATA_ORIGIN | TPMA_OBJ_USER_WITH_AUTH |              \
     TPMA_OBJ_NO_DA | TPMA_OBJ_RESTRICTED | TPMA_OBJ_DECRYPT)

/* Sealed KEYEDHASH object attributes: fixed to this TPM + parent, NO
 * userWithAuth (so Unseal MUST satisfy the authPolicy), NO sensitiveDataOrigin
 * (the data is supplied by the caller, not TPM-generated). noDA is REQUIRED: a
 * PCR-sealed object is EXPECTED to fail authorization on legitimate PCR drift
 * (a Secure Boot policy change, firmware update, ...); without noDA those normal
 * TPM_RC_POLICY_FAIL events feed the TPM dictionary-attack counter and repeated
 * boots after a mismatch escalate a recoverable POLICY_FAIL into a DA lockout
 * that blocks unseal/recovery and other TPM auth. BitLocker/systemd seal objects
 * set noDA for the same reason. */
#define TPM_SEAL_OBJECT_ATTRS \
    (TPMA_OBJ_FIXED_TPM | TPMA_OBJ_FIXED_PARENT | TPMA_OBJ_NO_DA)

/* ---- Sealed-blob + secret bounds ----
 * A persisted sealed blob is the Create response's outPrivate + outPublic. For a
 * KEYEDHASH object sealing <= 128 bytes under SHA-256, outPrivate stays well
 * under 256 and outPublic under 128; the wrappers reject anything larger as
 * malformed. FDE keys (XTS-AES-256 = 64 bytes) and CI-policy digests (<= 64)
 * fit inside the 128-byte secret cap. */
#define TPM_SEAL_SECRET_MAX 128u
#define TPM_SEAL_PRIV_MAX   256u
#define TPM_SEAL_PUB_MAX    160u

/* A persisted sealed object: the opaque private/public blobs the OS stores and
 * later hands back to unseal. Self-describing (carries its own lengths) so a
 * consumer can serialize it without knowing the TPM wire format. */
struct tpm_sealed_blob {
    uint8_t  priv[TPM_SEAL_PRIV_MAX];
    uint16_t priv_len;
    uint8_t  pub[TPM_SEAL_PUB_MAX];
    uint16_t pub_len;
};

/* ---- Classified outcome (degraded states never wedge the caller) ---- */
typedef enum {
    TPM_SEAL_OK        = 0,  /* sealed / unsealed successfully */
    TPM_SEAL_BADARG    = 1,  /* caller argument error (no transaction issued) */
    TPM_SEAL_NO_TPM    = 2,  /* transport unavailable / no TPM */
    TPM_SEAL_POLICY_FAIL = 3,/* PCR state does not match the seal -- secret locked */
    TPM_SEAL_AUTH_FAIL = 4,  /* parent/owner authorization failed */
    TPM_SEAL_TRANSPORT = 5,  /* transport failure / malformed response */
    TPM_SEAL_BUSY      = 6,  /* another transaction in flight -- transient */
    TPM_SEAL_TPMERR    = 7,  /* other classified TPM rc */
} tpm_seal_status_t;

/* Structured unseal-failure report: the classified status plus the raw TPM rc
 * (0 when the failure is pre-transport) and which sealing domain failed, so a
 * recovery UX can name what is wrong without re-deriving it. */
typedef enum {
    TPM_SEAL_DOMAIN_GENERIC = 0,
    TPM_SEAL_DOMAIN_FDE     = 1,  /* full-disk-encryption unlock key */
    TPM_SEAL_DOMAIN_CI      = 2,  /* code-integrity policy */
} tpm_seal_domain_t;

struct tpm_unseal_result {
    tpm_seal_status_t status;
    uint32_t          tpm_rc;    /* raw response code (0 if pre-transport) */
    tpm_seal_domain_t domain;    /* which sealed secret failed */
};

/* Recovery handoff: a consumer (recovery-mode UX, FDE fallback) registers one
 * handler that the unseal path invokes on a POLICY_FAIL / hard failure. The
 * live-console recovery PROMPT is a separate follow-up that needs the Phase-1
 * console-input path; this callable seam exists now so the prompt can be wired
 * in without changing the unseal flow. NULL handler = no-op (default). */
typedef void (*tpm_seal_recovery_fn)(const struct tpm_unseal_result *result);
void tpm_seal_set_recovery_handler(tpm_seal_recovery_fn fn);

/* ---- Pure marshaling seam (MMIO-free, fixture-tested) ----
 * Each builder returns the marshaled command length, or 0 on bad argument /
 * too-small buffer. Each parser returns 0 / a byte count on success, or -1 /0
 * on malformed/failed response. */

/* TPM2_CreatePrimary(primaryHandle=TPM_RH_OWNER) of the fixed ECC-P256 storage
 * parent (SRK template), empty owner auth, no sealed-in data. */
uint32_t tpm2_build_create_primary_srk(uint8_t *buf, uint32_t cap);

/* Extract the objectHandle from a CreatePrimary/Load response. These responses
 * carry a 4-byte handle area BEFORE parameterSize, so the handle is at the fixed
 * post-header offset. Returns the handle (a transient-object handle, high byte
 * 0x80) or 0 on malformed/failed/non-object response. */
uint32_t tpm2_parse_object_handle(const uint8_t *rsp, uint32_t len);

/* TPM2_Create a KEYEDHASH sealed object under `parent` (password auth, empty pw)
 * carrying `secret` (1..TPM_SEAL_SECRET_MAX bytes) with the supplied 32-byte
 * authPolicy. */
uint32_t tpm2_build_create_sealed(uint8_t *buf, uint32_t cap, uint32_t parent,
                                  const uint8_t *auth_policy, uint16_t auth_policy_len,
                                  const uint8_t *secret, uint16_t secret_len);

/* Parse a TPM2_Create response: copies outPrivate + outPublic (the first two
 * TPM2B parameters) into the blob. Returns 0 on success, -1 on malformed/failed
 * or if either blob exceeds its cap. */
int tpm2_parse_create_sealed(const uint8_t *rsp, uint32_t len,
                             struct tpm_sealed_blob *out);

/* TPM2_Load(parent) of a previously-created sealed blob (password auth). */
uint32_t tpm2_build_load(uint8_t *buf, uint32_t cap, uint32_t parent,
                         const struct tpm_sealed_blob *blob);

/* TPM2_Unseal(itemHandle) under a real policy session. */
uint32_t tpm2_build_unseal(uint8_t *buf, uint32_t cap, uint32_t item,
                           uint32_t policy_session);

/* Parse a TPM2_Unseal response: copies outData (the TPM2B_SENSITIVE_DATA
 * parameter) to out. Returns the byte count (>= 0) or -1 on malformed/failed. */
int tpm2_parse_unseal(const uint8_t *rsp, uint32_t len,
                      uint8_t *out, uint32_t out_cap);

/* ---- High-level wrappers (Phase-1 transport; not ISR-safe) ---- */

/* Seal `secret` to the current seal-mask PCR state. On TPM_SEAL_OK fills `out`
 * with the persisted blob the caller stores. */
tpm_seal_status_t tpm_seal_secret(const uint8_t *secret, uint16_t secret_len,
                                  struct tpm_sealed_blob *out);

/* Unseal a previously-sealed blob. Succeeds only while the seal-mask PCRs match
 * the seal-time state; a mismatch returns TPM_SEAL_POLICY_FAIL. `result` (may be
 * NULL) receives the structured outcome; on failure the registered recovery
 * handler is invoked with it. `domain` tags the result for the recovery UX. */
tpm_seal_status_t tpm_unseal_secret(const struct tpm_sealed_blob *blob,
                                    tpm_seal_domain_t domain,
                                    uint8_t *out, uint16_t cap, uint16_t *out_len,
                                    struct tpm_unseal_result *result);

/* ---- Forward-API hooks (callable now; stub consumers until owners land) ----
 * Thin domain-tagged wrappers so the storage-encryption (FDE) and code-integrity
 * subsystems have a stable entry point the moment they exist, without re-deriving
 * the seal/unseal flow. The blob is supplied by the consumer (it owns where the
 * sealed key/policy is persisted). */

tpm_seal_status_t tpm_seal_fde_key(const uint8_t *key, uint16_t key_len,
                                   struct tpm_sealed_blob *out);
tpm_seal_status_t tpm_unseal_fde_key(const struct tpm_sealed_blob *blob,
                                     uint8_t *out_key, uint16_t cap,
                                     uint16_t *out_len,
                                     struct tpm_unseal_result *result);

tpm_seal_status_t tpm_seal_ci_policy(const uint8_t *policy, uint16_t policy_len,
                                     struct tpm_sealed_blob *out);
tpm_seal_status_t tpm_unseal_ci_policy(const struct tpm_sealed_blob *blob,
                                       uint8_t *out_policy, uint16_t cap,
                                       uint16_t *out_len,
                                       struct tpm_unseal_result *result);
