/* ============================================================================
 * tpm_attest_report.c -- TPM-rooted boot attestation report export.
 *
 * Currently: the immutable bootloader-to-kernel handoff snapshot the report
 * builder consumes instead of live `g_boot_info` (whose capability words are
 * refined by the kernel after handoff). Pure value copy, MMIO-free, unit-tested.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm_attest_report.h"
#include "kernel/boot_info.h"
#include "kernel/tpm.h"                  /* tpm_integrity_report, tpm_pcr_get, TPM_ALG_SHA256 */
#include "kernel/tpm_seal.h"             /* TPM_ALG_RSA (EK-cert NV-index selector) */
#include "kernel/tpm_pcr_alloc.h"        /* tpm_pcr_quote_mask */
#include "kernel/boot_proto_descriptor.h"/* kernel_boot_proto (.bootproto manifest const) */
#include "kernel/crypto/sha256.h"        /* recompute the quoted pcrDigest */
#include "libc/string.h"

/* The kernel-image ABI manifest descriptor, emitted as a compile-time const in
 * the `.bootproto` ELF section (src/kernel/main/boot_proto.c). Immutable by
 * construction -- a more stable identity source than the Phase-0 snapshot. */
extern const struct boot_proto_descriptor kernel_boot_proto;

/* ABI pins: the report is serialized (JSON export + native query API) by a later
 * slice, so its layout is load-bearing. */
_Static_assert(sizeof(struct attest_pcr_slot) == 36,
    "attest_pcr_slot layout pinned (4-byte header + 32-byte SHA-256 digest)");
_Static_assert(ATTEST_REPORT_PCR_MAX == 9u,
    "quoted PCR set is {0-7,11} == 9 slots; matches tpm_pcr_quote_mask()");
_Static_assert(sizeof(struct boot_attestation_report) <= 4096,
    "report must fit one kmalloc page so callers can allocate it off-stack");
_Static_assert(__builtin_offsetof(struct boot_attestation_report, manifest_sha256) % 4 == 0,
    "manifest_sha256 stays naturally aligned for the serializer");

void tpm_attest_handoff_capture(const struct boot_info *bi,
                                struct boot_attest_handoff *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!bi)
        return;   /* valid stays 0 -> the report builder fails closed */
    out->caps_required       = bi->caps_required;
    out->caps_present        = bi->caps_present;
    out->caps_degraded       = bi->caps_degraded;
    out->boot_path           = bi->boot_path;
    out->boot_reason         = bi->boot_reason;
    out->boot_source_flags   = bi->boot_source_flags;
    out->boot_fallback_depth = bi->boot_fallback_depth;
    out->valid               = 1u;
}

/* The one canonical Phase-0 handoff snapshot. Written ONCE on the BSP at Phase 0
 * (before APs run and before any caps refinement), then read-only -- the same
 * write-once-at-boot, lock-free-read discipline as the PCR cache and the integrity
 * report, so no lock is needed. The latch ENFORCES write-once: a later accidental
 * call with already-refined g_boot_info no-ops instead of replacing loader evidence
 * (and a lock-free reader can never race a second write because there isn't one). */
static struct boot_attest_handoff s_handoff;
static uint8_t s_handoff_latched;

void tpm_attest_handoff_snapshot_init(const struct boot_info *bi)
{
    if (s_handoff_latched)
        return;                       /* write-once: the first Phase-0 capture wins */
    tpm_attest_handoff_capture(bi, &s_handoff);
    if (s_handoff.valid)
        s_handoff_latched = 1u;       /* latch only a SUCCESSFUL capture */
}

const struct boot_attest_handoff *tpm_attest_handoff_get(void)
{
    return &s_handoff;
}

/* Recompute the TPM2 quote's pcrDigest from the separately-listed SHA-256 PCRs
 * and compare to what the TPM signed. TPMS_QUOTE_INFO.pcrDigest is
 * H_nameAlg(PCR[i0] || PCR[i1] || ...) over the selected PCRs in ascending
 * index order; for our single SHA-256 selection that is SHA256 of the
 * concatenated 32-byte PCR values. A MATCH binds the listed PCRs to the signed
 * quote; a MISMATCH means cache staleness, tamper, or a TPM fault; INDETERMINATE
 * means a selected PCR could not be read back to recompute. */
static uint8_t report_pcr_coherence(const struct boot_attestation_report *out)
{
    struct sha256_ctx ctx;
    uint8_t expected[SHA256_DIGEST_LEN];
    uint32_t sel = out->quote.pcr_select;
    uint8_t pcr;

    if (out->quote.pcr_digest_len != SHA256_DIGEST_LEN)
        return ATTEST_COHERENCE_INDETERMINATE;

    sha256_init(&ctx);
    for (pcr = 0; pcr < 24u; pcr++) {
        const struct attest_pcr_slot *slot = 0;
        uint8_t i;
        if (!(sel & (1u << pcr)))
            continue;
        for (i = 0; i < out->pcr_count; i++) {
            if (out->pcrs[i].pcr_index == pcr) { slot = &out->pcrs[i]; break; }
        }
        if (!slot || slot->status != (uint8_t)TPM_PCR_OK ||
            slot->digest_len != SHA256_DIGEST_LEN)
            return ATTEST_COHERENCE_INDETERMINATE;
        sha256_update(&ctx, slot->digest, SHA256_DIGEST_LEN);
    }
    sha256_final(&ctx, expected);

    return (memcmp(expected, out->quote.pcr_digest, SHA256_DIGEST_LEN) == 0)
               ? ATTEST_COHERENCE_MATCH
               : ATTEST_COHERENCE_MISMATCH;
}

tpm_attest_status_t tpm_attest_report_build(const uint8_t *nonce, uint16_t nonce_len,
                                            struct boot_attestation_report *out)
{
    const struct boot_attest_handoff *h;
    const struct boot_integrity_report *ir;
    uint32_t qmask;
    uint8_t pcr;
    int self_test;

    if (!out)
        return TPM_ATTEST_BADARG;

    /* Nonce contract: NULL/0 is an explicit unsigned self-test report; any other
     * call must carry a valid verifier challenge of TPM_QUOTE_NONCE_MIN..MAX
     * bytes. Reject a malformed challenge as a caller error -- never silently
     * truncate it or route it through the quote-failure path, which would make a
     * bad nonce masquerade as ordinary degraded-TPM evidence in a valid report. */
    self_test = (nonce == 0 && nonce_len == 0u);
    if (!self_test &&
        (nonce == 0 || nonce_len < TPM_QUOTE_NONCE_MIN || nonce_len > TPM_QUOTE_NONCE_MAX))
        return TPM_ATTEST_BADARG;

    memset(out, 0, sizeof *out);
    out->schema_version          = ATTEST_REPORT_SCHEMA_VERSION;
    out->quoted_bank_alg         = TPM_ALG_SHA256;
    out->pcr_coherence           = ATTEST_COHERENCE_NA;
    out->pcr_bound               = 0u;   /* gates on BOOT_CAP_MANIFEST_PCR_BOUND once that cap-bit ships */
    out->drtm_entry_pcr          = 0xFFu;/* none: native BOOTX64 issues no SENTER/SKINIT */
    out->ak_credential_status    = ATTEST_TRUST_UNVERIFIED;
    out->qualified_signer_status = ATTEST_TRUST_UNVERIFIED;
    out->ek_cert_status          = ATTEST_TRUST_ABSENT;

    /* (1) Kernel-image identity: the immutable .bootproto manifest const. */
    memcpy(out->manifest_sha256, kernel_boot_proto.sha256, sizeof out->manifest_sha256);
    out->manifest_version     = kernel_boot_proto.version;
    out->manifest_struct_size = kernel_boot_proto.struct_size;

    /* (2) Handoff provenance: the immutable Phase-0 snapshot, never live g_boot_info. */
    h = tpm_attest_handoff_get();
    out->handoff_valid       = h->valid;
    out->caps_required       = h->caps_required;
    out->caps_present        = h->caps_present;
    out->caps_degraded       = h->caps_degraded;
    out->boot_path           = h->boot_path;
    out->boot_reason         = h->boot_reason;
    out->boot_source_flags   = h->boot_source_flags;
    out->boot_fallback_depth = h->boot_fallback_depth;

    /* (3) Integrity verdict. */
    ir = tpm_integrity_report();
    if (ir) {
        out->overall_status    = ir->overall_status;
        out->replay_verdict    = ir->replay_verdict;
        out->tpm_version       = ir->tpm_version;
        out->secure_boot       = ir->secure_boot;
        out->secure_boot_valid = ir->secure_boot_valid;
        out->event_count       = ir->event_count;
    }

    /* (4) Verifier nonce: exact copy (validated MIN..MAX above; no truncation). */
    if (!self_test) {
        memcpy(out->nonce, nonce, nonce_len);
        out->nonce_len = nonce_len;
    }

    /* (5) SHA-256 PCR bank: read each quote-mask PCR from the lock-free cache. */
    qmask = tpm_pcr_quote_mask();
    for (pcr = 0; pcr < 24u && out->pcr_count < ATTEST_REPORT_PCR_MAX; pcr++) {
        struct attest_pcr_slot *slot;
        uint32_t dlen = 0;
        tpm_pcr_status_t ps;
        if (!(qmask & (1u << pcr)))
            continue;
        slot = &out->pcrs[out->pcr_count++];
        ps = tpm_pcr_get(pcr, TPM_ALG_SHA256, slot->digest, sizeof slot->digest, &dlen);
        slot->pcr_index  = pcr;
        slot->status     = (uint8_t)ps;
        if (ps == TPM_PCR_OK) {
            slot->digest_len = (uint8_t)dlen;
        } else {
            slot->digest_len = 0u;
            memset(slot->digest, 0, sizeof slot->digest);
        }
    }

    /* (6) Live TPM2_Quote over the SHA-256 quote-mask selection. A self-test
     * report carries no nonce, so no signed quote is attempted (quote_status
     * BADARG records "no challenge"; a real malformed nonce was already rejected
     * with BADARG before the report was filled, so a populated report with this
     * status can only be the self-test mode). */
    if (self_test) {
        out->quote_status  = (uint8_t)TPM_ATTEST_BADARG;
        out->quote_present = 0u;
    } else {
        out->quote_status = (uint8_t)tpm2_quote(qmask, out->nonce, out->nonce_len,
                                                &out->quote, out->quote_sig,
                                                sizeof out->quote_sig, &out->quote_sig_len);
        out->quote_present = (out->quote_status == (uint8_t)TPM_ATTEST_OK) ? 1u : 0u;
    }

    /* (7) Coherence: bind the listed PCRs to the signed pcrDigest. */
    if (out->quote_present)
        out->pcr_coherence = report_pcr_coherence(out);

    /* (8) AK public (verifier checks the quote sig against this). */
    out->ak_status = (uint8_t)tpm_ak_public_get(out->ak_pub, sizeof out->ak_pub,
                                                &out->ak_pub_len);

    /* (9) EK certificate (RSA EK-cert NV index). NO_EK_CERT is the common
     * fTPM/vTPM case -- never a hard failure. The AK<->EK binding stays
     * UNVERIFIED regardless: present cert != credential-activated. */
    {
        uint16_t eklen = 0;
        tpm_attest_status_t eks = tpm_ek_cert_read(TPM_ALG_RSA, out->ek_cert,
                                                   sizeof out->ek_cert, &eklen);
        out->ek_status = (uint8_t)eks;
        if (eks == TPM_ATTEST_OK) {
            out->ek_cert_len    = eklen;
            out->ek_cert_status = ATTEST_TRUST_UNVERIFIED;  /* present, not yet credential-activated */
        } else {
            out->ek_cert_len    = 0u;
            out->ek_cert_status = ATTEST_TRUST_ABSENT;
        }
    }

    out->valid = 1u;
    return TPM_ATTEST_OK;
}
