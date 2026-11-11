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
#include "kernel/boot_info.h"    /* struct boot_loader_identity, BOOT_CAP_MANIFEST_PCR_BOUND */
#include "kernel/tpm.h"          /* struct tpm_event */

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
    uint8_t  loader_identity_valid;/* 1 when the loader populated loader_identity
                                    * (its git_sha is not the all-zero sentinel) */
    uint8_t  _pad[6];
    struct boot_loader_identity loader_identity;  /* bootloader build identity, copied verbatim */
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
 * slice adds is diagnostic-only. A quote is returned only when its qualifiedSigner
 * equals the AK's Qualified Name under the EK, so qualified_signer_status is
 * VERIFIED for a present quote. The AK<->EK CERTIFICATION (MakeCredential /
 * ActivateCredential) is a tracked follow-up, so ak_credential_status stays
 * UNVERIFIED -- a consumer must not treat the AK as EK-certified yet.
 * ------------------------------------------------------------------------- */

/* Report schema version -- bump on any field add/reorder so a serialized
 * consumer can tell variants apart. v2 added the quote freshness fields
 * (echoedNonce, clock, resetCount, restartCount, safe, firmwareVersion). */
#define ATTEST_REPORT_SCHEMA_VERSION   3u

/* Quoted PCR set == tpm_pcr_quote_mask() == PCR {0-7, 11} == 9 slots. */
#define ATTEST_REPORT_PCR_MAX          9u

/* Trust status for the AK->EK binding and the quote signer identity. */
#define ATTEST_TRUST_UNVERIFIED        0u  /* not proven by this build (default) */
#define ATTEST_TRUST_VERIFIED          1u  /* proven by this build (the quote signer today) */
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
    uint8_t  pcr_bound;             /* 1 only when tpm_attest_manifest_bound() proved it: the loader
                                     * advertised BOOT_CAP_MANIFEST_PCR_BOUND, the log holds the PCR-11
                                     * manifest event with the expected digest, and the log replays */
    uint8_t  quote_present;         /* 1 if tpm2_quote succeeded */
    uint32_t event_count;           /* total measured events */

    /* Kernel-ABI identity: the immutable .bootproto MANIFEST digest (compile-
     * time const). NOT a kernel-image hash -- it tracks the boot-info ABI and
     * stays put when the image changes underneath it. */
    uint8_t  manifest_sha256[32];   /* == BOOT_PROTO_ABI_DIGEST_LEN; asserted in tpm_attest_report.c */
    uint32_t manifest_version;      /* kernel_boot_proto.version (== BOOT_INFO_VERSION) */
    uint32_t manifest_struct_size;  /* kernel_boot_proto.struct_size */
    uint8_t  manifest_valid;        /* 1 when the descriptor validated; 0 = this kernel's own
                                     * identity is corrupt and the report is diagnostic only */

    /* Scoped integrity verdict (schema 3): what overall_status covers, which of
     * the four MISMATCH classes it is, and the first differing field. */
    uint8_t  failure_class;         /* BOOT_INTEGRITY_CLASS_* */
    uint8_t  baseline_status;       /* tpm_baseline_status_t, or BOOT_INTEGRITY_BS_UNPUBLISHED */
    uint8_t  mismatch_cause;        /* tpm_baseline_cause_t */
    uint8_t  mismatch_pcr;          /* meaningful only when mismatch_pcr_valid */
    uint8_t  mismatch_pcr_valid;
    uint8_t  kernel_image_measured; /* always 0: no kernel IMAGE digest is measured or verified */
    uint8_t  _pad_iv;
    const char *integrity_label;    /* tpm_integrity_status_label: static string, NULL = unset */
    const char *integrity_scope;    /* tpm_integrity_status_scope: static string, NULL = unset */

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
    uint8_t  loader_identity_valid;
    uint8_t  _pad_li[3];
    struct boot_loader_identity loader_identity;

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
     * certified by the platform EK (UNVERIFIED today) and whether the quote signer
     * is that AK (VERIFIED whenever a quote is present). */
    uint8_t  ak_status;             /* tpm_attest_status_t value, fixed-width */
    uint8_t  ak_credential_status;  /* ATTEST_TRUST_* (UNVERIFIED until ActivateCredential ships) */
    uint8_t  qualified_signer_status; /* ATTEST_TRUST_* (VERIFIED with a present quote) */
    uint8_t  _pad3;
    uint16_t ak_pub_len;
    uint16_t _pad4;
    uint8_t  ak_pub[TPM_AK_PUB_MAX];

    /* EK certificate (binds the AK to the platform once credential-activated). */
    uint8_t  ek_status;             /* tpm_attest_status_t value, fixed-width */
    uint8_t  ek_cert_status;        /* ATTEST_TRUST_* (ABSENT when no EK cert provisioned) */
    uint16_t ek_cert_len;
    uint8_t  ek_cert[TPM_EK_CERT_MAX];

    /* Forward-compat DRTM slots: no entry today (entry PCR 0xFF, the others 0;
     * native BOOTX64 issues no SENTER/SKINIT); a future Secure Launch adapter
     * populates them without a schema break. */
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
 * (the report is left untouched so a bad challenge cannot masquerade as valid).
 * Returns TPM_ATTEST_SELF_CORRUPT when this kernel's own .bootproto identity
 * fails validation: `out` then holds a DIAGNOSTIC report (valid 0,
 * manifest_valid 0, failure_class SELF_IDENTITY, handoff + integrity verdict
 * filled, no nonce and no TPM evidence), which the serializer renders without
 * any trust field. */
tpm_attest_status_t tpm_attest_report_build(const uint8_t *nonce, uint16_t nonce_len,
                                            struct boot_attestation_report *out);

struct boot_proto_descriptor;

/* tpm_attest_report_build over a CALLER-SUPPLIED descriptor: build() is exactly
 * this applied to kernel_boot_proto. The seam exists so the SELF_CORRUPT
 * diagnostic path is testable, since kernel_boot_proto is const and linked in. */
tpm_attest_status_t tpm_attest_report_build_from(const struct boot_proto_descriptor *desc,
                                                 const uint8_t *nonce, uint16_t nonce_len,
                                                 struct boot_attestation_report *out);

/* 1 when the parsed event log proves the loader's manifest measurement: at least
 * one PCR-11 event of TPM_PCR_MANIFEST_EVENT_TYPE whose payload is exactly
 * TPM_PCR_MANIFEST_EVENT, carrying a SHA-256 digest equal to
 * SHA-256(manifest_sha256), and NO such event with any other digest. Pure and
 * bounds-checked against `log_size` (the log is firmware-supplied). */
int tpm_attest_manifest_event_ok(const uint8_t *log, uint32_t log_size,
                                 const struct tpm_event *ev, uint32_t count,
                                 const uint8_t manifest_sha256[32]);

/* The report's pcr_bound: 1 only when the handoff snapshot is valid and carries
 * BOOT_CAP_MANIFEST_PCR_BOUND, `event_ok` (tpm_attest_manifest_event_ok) holds,
 * AND the SHA-256 replay verdict is VERIFIED, so the record is what PCR 11
 * really holds. Any other combination is 0. Pure. */
uint8_t tpm_attest_pcr_bound_decide(const struct boot_attest_handoff *h,
                                    int event_ok, uint8_t replay_verdict);

/* Sequential write sink for the JSON serializer: called with a monotonically
 * increasing `off` and the chunk to write there. Return 0 on success, non-zero
 * to abort. Mirrors the (offset, len) shape of vfs_write so the file export is a
 * thin adapter; a unit test can pass a memory-buffer writer instead. */
typedef int (*attest_json_write_fn)(void *ctx, uint32_t off, const uint8_t *data, uint32_t len);

/* Serialize a built report as a single JSON object via `write`. Text values (the
 * integrity label and scope, the loader build label) are JSON-escaped; the rest
 * are numbers or hex strings. A diagnostic report (manifest_valid 0) is rendered
 * with its identity, handoff and integrity blocks only.
 * Byte arrays (digests, signature, AK pub, EK cert) are lowercase hex. Streams in
 * bounded chunks (no full-document buffer). Returns 0 on success, -1 on a NULL
 * arg or a write-callback failure. */
int tpm_attest_report_to_json(const struct boot_attestation_report *r,
                              attest_json_write_fn write, void *ctx);

/* Upper bound of one serialized report. Every variable field is capped by its
 * array (9 PCRs, 64-byte nonce, 72-byte signature, 160-byte AK, 1024-byte EK
 * cert, each hex-doubled); a unit test serializes a report with every length
 * at its cap and checks it fits with headroom. */
#define ATTEST_REPORT_JSON_MAX  (12u * 1024u)

/* Build this boot's report once (with a CSPRNG nonce when the CSPRNG is
 * crypto-ready, else an unsigned self-test report), publish its JSON as the
 * snapshot tpm_attest_report_snapshot() returns, then write it to
 * X:\Diag\attestation.json. The snapshot is published whether or not that
 * volume is writable. Best effort: logs and returns on any failure, never
 * halts. A second call is refused and leaves the published snapshot alone. */
void tpm_attest_report_export(void);

/* The published report: 1 with `*bytes`/`*len` naming the immutable JSON
 * document (schema ATTEST_REPORT_SCHEMA_VERSION, no NUL), or 0 when none has
 * been published this boot. Safe from any CPU without a lock: the bytes never
 * change once visible (acquire on the publication state). */
int tpm_attest_report_snapshot(const uint8_t **bytes, uint32_t *len);

/* attest_json_write_fn over a memory sink (ctx is a struct attest_json_mem);
 * refuses a write that is out of order or past cap. */
struct attest_json_mem { uint8_t *buf; uint32_t cap; uint32_t len; };
int tpm_attest_json_mem_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t n);
