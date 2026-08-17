/* ============================================================================
 * tpm_attest_report.h -- TPM-rooted boot attestation report export.
 *
 * The attestation report exports the TPM-signed boot evidence (PCRs + quote +
 * AK public + EK-cert chain) plus the bootloader-to-kernel handoff context. This
 * header currently defines the IMMUTABLE handoff snapshot the report builder must
 * consume instead of the live `g_boot_info`:
 *
 *   The capability words (`caps_present` / `caps_degraded`) are MUTATED after
 *   handoff -- the kernel refines them via `boot_caps_mark_present()` and
 *   runtime-services degradation. A report that read live `g_boot_info` would
 *   publish kernel-refined caps rather than the loader-to-kernel handoff, so a
 *   remote verifier could not distinguish loader evidence from later kernel
 *   policy. The snapshot is captured once, early (Phase 0, before any caps
 *   refinement), and the report consumes it.
 *
 * The report struct + builder + JSON export + native query API + the bootloader
 * PCR-11 manifest extend are the remaining section-9 work; see the section Design
 * note in todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/tpm_attest.h"   /* struct tpm_quote_attest, tpm_attest_status_t, TPM_*_MAX */

struct boot_info;   /* forward decl: the capture reads the handoff fields */

/* Immutable snapshot of the bootloader-to-kernel handoff triple, taken before any
 * kernel-side capability refinement. Plain value type (copied, never aliased to the
 * live struct), so a later kernel `boot_caps_mark_present()` cannot change what the
 * attestation report attributes to the loader. */
struct boot_attest_handoff {
    uint64_t caps_required;        /* loader-asserted required capability bits */
    uint64_t caps_present;         /* capabilities the loader actually populated */
    uint64_t caps_degraded;        /* known-but-not-provided bits (adapter degradation) */
    uint32_t boot_path;            /* enum boot_path_type: which flow ran */
    uint32_t boot_reason;          /* enum boot_reason_code: the policy reason */
    uint32_t boot_source_flags;    /* BOOT_SOURCE_FLAG_* bitmask: inputs consulted */
    uint32_t boot_fallback_depth;  /* 0 = primary, N = Nth fallback */
    uint8_t  valid;                /* 1 once captured from a non-NULL boot_info */
    uint8_t  _pad[7];
};

/* Copy the handoff triple out of `bi` into `out` (pure, MMIO-free). On a NULL `bi`
 * or `out`, leaves/marks `out` invalid (valid = 0) so a consumer fails closed. Call
 * this ONCE at Phase 0 from the kernel snapshot point, before caps refinement; the
 * attestation report builder then reads the snapshot, never live `g_boot_info`. */
void tpm_attest_handoff_capture(const struct boot_info *bi,
                                struct boot_attest_handoff *out);

/* Capture the handoff snapshot into the module's one canonical store. Call ONCE at
 * Phase 0 on the BSP, after boot_info validation and BEFORE any capability
 * refinement (boot_caps_mark_present / runtime-services degradation). */
void tpm_attest_handoff_snapshot_init(const struct boot_info *bi);

/* The stored Phase-0 handoff snapshot, for the attestation report builder. Never
 * NULL; `valid` is 0 until snapshot_init has run. Consumers MUST read this, never
 * the live g_boot_info, for handoff-attributed fields. snapshot_init is write-once:
 * the first successful capture latches and later calls no-op. */
const struct boot_attest_handoff *tpm_attest_handoff_get(void);

/* ----------------------------------------------------------------------------
 * Boot attestation report.
 *
 * The TPM-rooted report a remote verifier (or local settings UI) consumes: the
 * immutable kernel-image identity + handoff provenance + the SHA-256 PCR bank
 * the TPM signed via TPM2_Quote, plus the AK public and EK certificate. Built
 * once into a caller-allocated struct; the JSON export and native query API are
 * a later slice that serialize this struct.
 *
 * Trust model (design constraint 3): tpm2_quote signs ONLY the SHA-256 bank, so
 * that is the lone TPM-attested (trusted) bank here; any other bank a future
 * slice adds is diagnostic-only. The AK<->EK binding is NOT proven by this build
 * (MakeCredential/ActivateCredential and qualifiedSigner-to-AK-name binding are
 * tracked follow-ups), so ak_credential_status / qualified_signer_status stay
 * UNVERIFIED -- a consumer must not treat the AK as EK-certified yet.
 * ------------------------------------------------------------------------- */

/* Report schema version -- bump on any field add/reorder so a serialized
 * consumer can tell variants apart. v2 added the quote freshness fields
 * (echoedNonce, clock, resetCount, restartCount, safe, firmwareVersion). */
#define ATTEST_REPORT_SCHEMA_VERSION   2u

/* Quoted PCR set == tpm_pcr_quote_mask() == PCR {0-7, 11} == 9 slots. */
#define ATTEST_REPORT_PCR_MAX          9u

/* Trust status for the AK->EK binding and the quote signer identity. */
#define ATTEST_TRUST_UNVERIFIED        0u  /* not proven by this build (default) */
#define ATTEST_TRUST_VERIFIED          1u  /* proven (reserved; unreachable until ActivateCredential ships) */
#define ATTEST_TRUST_ABSENT            2u  /* source material cleanly absent (e.g. vTPM/fTPM with no EK cert) */
#define ATTEST_TRUST_UNKNOWN           3u  /* could not determine: a read/transport error, NOT a clean absence */

/* Coherence between the separately-listed SHA-256 PCRs and the signed quote:
 * the builder recomputes H(selected PCR digests) and compares it to the TPM's
 * signed pcrDigest, so a verifier learns whether the listed PCRs actually match
 * what was signed (vs cache staleness / tamper / a TPM fault). */
#define ATTEST_COHERENCE_NA            0u  /* no quote (no TPM / quote failed) */
#define ATTEST_COHERENCE_MATCH         1u  /* recomputed digest == signed pcrDigest */
#define ATTEST_COHERENCE_MISMATCH      2u  /* disagreement: cache staleness / tamper / TPM error */
#define ATTEST_COHERENCE_INDETERMINATE 3u  /* a quoted PCR could not be read back to recompute */

/* One SHA-256 PCR measurement in the quoted bank. */
struct attest_pcr_slot {
    uint8_t  pcr_index;    /* 0-23 */
    uint8_t  status;       /* tpm_pcr_status_t value, stored fixed-width */
    uint8_t  digest_len;   /* 32 on OK, else 0 */
    uint8_t  _pad;
    uint8_t  digest[32];   /* SHA-256 PCR value (zeroed unless status == OK) */
};

struct boot_attestation_report {
    uint32_t schema_version;        /* ATTEST_REPORT_SCHEMA_VERSION */
    uint8_t  valid;                 /* 1 = report assembled (TPM evidence may still be absent) */
    uint8_t  tpm_version;           /* 0=none, 1=1.2, 2=2.0 */
    uint8_t  overall_status;        /* BOOT_INTEGRITY_* */
    uint8_t  replay_verdict;        /* tpm_replay_verdict_t */
    uint8_t  secure_boot;           /* 1 if SB state readable AND active */
    uint8_t  secure_boot_valid;     /* 1 if the live SB state was readable */
    uint8_t  pcr_bound;             /* 1 once BOOT_CAP_MANIFEST_PCR_BOUND gates; 0 until that cap-bit ships */
    uint8_t  quote_present;         /* 1 if tpm2_quote succeeded */
    uint32_t event_count;           /* total measured events */

    /* Kernel-ABI identity: the immutable .bootproto MANIFEST digest (compile-
     * time const). NOT a kernel-image hash -- it tracks the boot-info ABI and
     * stays put when the image changes underneath it. */
    uint8_t  manifest_sha256[32];   /* == BOOT_PROTO_ABI_DIGEST_LEN; asserted in tpm_attest_report.c */
    uint32_t manifest_version;      /* kernel_boot_proto.version (== BOOT_INFO_VERSION) */
    uint32_t manifest_struct_size;  /* kernel_boot_proto.struct_size */

    /* Handoff provenance: from the immutable Phase-0 snapshot, never live g_boot_info. */
    uint8_t  handoff_valid;         /* boot_attest_handoff.valid */
    uint8_t  _pad0[3];
    uint64_t caps_required;
    uint64_t caps_present;
    uint64_t caps_degraded;
    uint32_t boot_path;
    uint32_t boot_reason;
    uint32_t boot_source_flags;
    uint32_t boot_fallback_depth;

    /* SHA-256 PCR bank: the ONLY TPM-quoted (trusted) bank. */
    uint16_t quoted_bank_alg;       /* TPM_ALG_SHA256 */
    uint8_t  pcr_count;             /* populated slot count */
    uint8_t  pcr_coherence;         /* ATTEST_COHERENCE_* */
    struct attest_pcr_slot pcrs[ATTEST_REPORT_PCR_MAX];

    /* Verifier nonce (anti-replay), echoed in the signed quote. */
    uint8_t  nonce[TPM_QUOTE_NONCE_MAX];
    uint16_t nonce_len;
    uint16_t _pad1;

    /* TPM2_Quote: signed attestation over the SHA-256 bank. */
    uint8_t  quote_status;          /* tpm_attest_status_t value, fixed-width */
    uint8_t  _pad2[3];
    struct tpm_quote_attest quote;
    uint8_t  quote_sig[TPM_SIG_ECDSA_P256_MAX];
    uint32_t quote_sig_len;

    /* AK public + binding status. The verifier checks the quote signature against
     * ak_pub; ak_credential_status / qualified_signer_status say whether the AK is
     * yet bound to the platform EK and to the quote signer (UNVERIFIED today). */
    uint8_t  ak_status;             /* tpm_attest_status_t value, fixed-width */
    uint8_t  ak_credential_status;  /* ATTEST_TRUST_* (UNVERIFIED until ActivateCredential ships) */
    uint8_t  qualified_signer_status; /* ATTEST_TRUST_* (UNVERIFIED until qualifiedSigner binding ships) */
    uint8_t  _pad3;
    uint16_t ak_pub_len;
    uint16_t _pad4;
    uint8_t  ak_pub[TPM_AK_PUB_MAX];

    /* EK certificate (binds the AK to the platform once credential-activated). */
    uint8_t  ek_status;             /* tpm_attest_status_t value, fixed-width */
    uint8_t  ek_cert_status;        /* ATTEST_TRUST_* (ABSENT when no EK cert provisioned) */
    uint16_t ek_cert_len;
    uint8_t  ek_cert[TPM_EK_CERT_MAX];

    /* Forward-compat DRTM slots: zeroed today (native BOOTX64 issues no
     * SENTER/SKINIT); a future Secure Launch adapter populates without a
     * schema break. */
    uint8_t  drtm_entry_pcr;        /* 0xFF = none (typically 17 when present) */
    uint8_t  drtm_acm_status;       /* 0 = none */
    uint8_t  drtm_measurement_type; /* 0=none, 1=TXT SENTER, 2=SKINIT */
    uint8_t  _pad5[5];
};

/* Build the attestation report into the caller-allocated `out` (the struct is
 * ~2.3 KB; allocate it off-stack). Reads the immutable handoff snapshot + the
 * .bootproto manifest + the integrity report + the SHA-256 PCR cache, then
 * issues a live TPM2_Quote over the quote-mask selection and reads AK pub + EK
 * cert. `nonce` is the verifier challenge echoed in the quote and MUST be
 * TPM_QUOTE_NONCE_MIN..TPM_QUOTE_NONCE_MAX bytes; pass NULL/0 for an unsigned
 * self-test report (no quote attempted). On no-TPM the report is still assembled
 * (valid=1, quote_present=0) -- a missing TPM is degraded, never a hard failure.
 * Returns TPM_ATTEST_BADARG on a NULL `out` OR a malformed non-NULL nonce (out of
 * the MIN..MAX range); a malformed nonce is a caller error, NOT a degraded report
 * (the report is left untouched so a bad challenge cannot masquerade as valid). */
tpm_attest_status_t tpm_attest_report_build(const uint8_t *nonce, uint16_t nonce_len,
                                            struct boot_attestation_report *out);

/* Sequential write sink for the JSON serializer: called with a monotonically
 * increasing `off` and the chunk to write there. Return 0 on success, non-zero
 * to abort. Mirrors the (offset, len) shape of vfs_write so the file export is a
 * thin adapter; a unit test can pass a memory-buffer writer instead. */
typedef int (*attest_json_write_fn)(void *ctx, uint32_t off, const uint8_t *data, uint32_t len);

/* Serialize a built report as a single JSON object via `write`. Every value is a
 * number or a hex string (no free-text), so no JSON string escaping is required.
 * Byte arrays (digests, signature, AK pub, EK cert) are lowercase hex. Streams in
 * bounded chunks (no full-document buffer). Returns 0 on success, -1 on a NULL
 * arg or a write-callback failure. */
int tpm_attest_report_to_json(const struct boot_attestation_report *r,
                              attest_json_write_fn write, void *ctx);

/* Build a fresh report (with a CSPRNG nonce when the CSPRNG is crypto-ready, else
 * an unsigned self-test report) and export it to X:\Diag\attestation.json. Best
 * effort: logs and returns on any failure, never halts. Call once X:\ is mounted. */
void tpm_attest_report_export(void);
