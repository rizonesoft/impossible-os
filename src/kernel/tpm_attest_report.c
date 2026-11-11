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
#include "kernel/tpm.h"                  /* tpm_integrity_report_copy, tpm_pcr_get, TPM_ALG_SHA256 */
#include "kernel/tpm_seal.h"             /* TPM_ALG_RSA (EK-cert NV-index selector) */
#include "kernel/tpm_pcr_alloc.h"        /* tpm_pcr_quote_mask */
#include "kernel/boot_proto_descriptor.h"/* boot_proto_abi_digest (validated ABI identity) */
#include "kernel/crypto/sha256.h"        /* recompute the quoted pcrDigest */
#include "kernel/csprng.h"               /* fresh nonce for the exported quote */
#include "kernel/fs/vfs.h"               /* X:\Diag\attestation.json export */
#include "kernel/mm/heap.h"              /* kmalloc/kfree the ~2.3 KB report */
#include "kernel/klog.h"                 /* best-effort export logging */
#include "kernel/tpm_replay.h"           /* tpm_event_bank_digest, TPM_REPLAY_VERIFIED */
#include "kernel/tpm_baseline.h"         /* tpm_baseline_cause_label */
#include "boot/pcr_manifest.h"           /* the PCR-11 manifest event the loader extends */

/* The exported manifest field is filled by boot_proto_abi_digest(), which
 * writes exactly BOOT_PROTO_ABI_DIGEST_LEN bytes. Pinning the two together
 * closes the last unasserted link in the digest-length chain: a future widening
 * would otherwise have this write past the field, and a narrowing would leave a
 * silently truncated identity that still looked valid. */
_Static_assert(sizeof(((struct boot_attestation_report *)0)->manifest_sha256) ==
                   BOOT_PROTO_ABI_DIGEST_LEN,
    "attest report manifest_sha256 must match the ABI-manifest digest length");
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
    memcpy(&out->loader_identity, &bi->loader_identity, sizeof out->loader_identity);
    {
        uint32_t k;
        for (k = 0; k < sizeof out->loader_identity.git_sha; k++)
            if (out->loader_identity.git_sha[k] != 0u) {
                out->loader_identity_valid = 1u;
                break;
            }
    }
    out->valid               = 1u;
}

/* ----------------------------------------------------------------------------
 * Manifest PCR binding.
 * ------------------------------------------------------------------------- */

static int manifest_event_named(const uint8_t *log, uint32_t log_size,
                                const struct tpm_event *e)
{
    static const char name[] = TPM_PCR_MANIFEST_EVENT;
    const uint32_t nlen = (uint32_t)sizeof name - 1u;
    if (e->pcr_index != TPM_PCR_MANIFEST_INDEX ||
        e->event_type != TPM_PCR_MANIFEST_EVENT_TYPE ||
        e->payload_size != nlen)
        return 0;
    if (e->payload_off > log_size || log_size - e->payload_off < nlen)
        return 0;
    return memcmp(log + e->payload_off, name, nlen) == 0;
}

int tpm_attest_manifest_event_ok(const uint8_t *log, uint32_t log_size,
                                 const struct tpm_event *ev, uint32_t count,
                                 const uint8_t manifest_sha256[32])
{
    uint8_t want[SHA256_DIGEST_LEN];
    uint32_t i;
    int matched = 0;

    if (!log || !ev || !manifest_sha256 || count == 0u)
        return 0;
    /* The loader hands firmware the 32 manifest bytes as DataToHash, so the
     * logged SHA-256 digest is SHA-256 of those bytes. */
    sha256(manifest_sha256, 32u, want);
    for (i = 0; i < count; i++) {
        const uint8_t *d = 0;
        if (!manifest_event_named(log, log_size, &ev[i]))
            continue;
        if (tpm_event_bank_digest(log, log_size, &ev[i], TPM_ALG_SHA256, &d) !=
                SHA256_DIGEST_LEN || !d)
            return 0;   /* a manifest event without a SHA-256 digest proves nothing */
        if (memcmp(d, want, SHA256_DIGEST_LEN) != 0)
            return 0;   /* a second, different manifest measurement voids the claim */
        matched = 1;
    }
    return matched;
}

uint8_t tpm_attest_pcr_bound_decide(const struct boot_attest_handoff *h,
                                    int event_ok, uint8_t replay_verdict)
{
    if (!h || !h->valid)
        return 0u;
    if (!(h->caps_present & (uint64_t)BOOT_CAP_MANIFEST_PCR_BOUND))
        return 0u;
    /* A matching log record is evidence only when the log replays to the
     * hardware PCRs, which is what makes it the value PCR 11 actually holds. */
    return (event_ok && replay_verdict == (uint8_t)TPM_REPLAY_VERIFIED) ? 1u : 0u;
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
    return tpm_attest_report_build_from(&kernel_boot_proto, nonce, nonce_len, out);
}

tpm_attest_status_t tpm_attest_report_build_from(const struct boot_proto_descriptor *desc,
                                                 const uint8_t *nonce, uint16_t nonce_len,
                                                 struct boot_attestation_report *out)
{
    const struct boot_attest_handoff *h;
    struct boot_integrity_report ir;
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
    out->pcr_bound               = 0u;   /* proved below by tpm_attest_pcr_bound_decide */
    out->drtm_entry_pcr          = 0xFFu;/* none: native BOOTX64 issues no SENTER/SKINIT */
    out->ak_credential_status    = ATTEST_TRUST_UNVERIFIED;
    out->qualified_signer_status = ATTEST_TRUST_UNVERIFIED;
    out->ek_cert_status          = ATTEST_TRUST_ABSENT;

    /* (1) Kernel-ABI identity: the immutable .bootproto MANIFEST digest. This
     * is NOT a kernel-image hash -- it is the SHA-256 of the boot-info ABI
     * manifest, so it stays put when the kernel image changes underneath it.
     * The image digests are separately owned and not yet implemented.
     *
     * Routed through the validated accessor rather than copying the descriptor
     * field directly: the accessor is the single place that decides whether the
     * descriptor is intact, and a direct copy exported a corrupt (bad-magic or
     * all-zero) identity as a VALID manifest. On failure the report carries no
     * manifest identity and is marked invalid rather than shipping zeros that a
     * verifier could mistake for a real digest. */
    out->manifest_valid = (desc && boot_proto_abi_digest_from(desc, out->manifest_sha256)) ? 1u : 0u;
    if (out->manifest_valid) {
        out->manifest_version     = desc->version;
        out->manifest_struct_size = desc->struct_size;
    } else {
        memset(out->manifest_sha256, 0, sizeof out->manifest_sha256);
    }

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
    out->loader_identity_valid = h->loader_identity_valid;
    memcpy(&out->loader_identity, &h->loader_identity, sizeof out->loader_identity);

    /* (3) Integrity verdict. Copied out under the report's publication lock, so
     * these six fields all come from the SAME snapshot -- a field-by-field read
     * of a live struct could straddle a publication and pair a new verdict with
     * old provenance, which is precisely what an attestation report must not
     * do. */
    tpm_integrity_report_copy(&ir);
    out->overall_status    = ir.overall_status;
    out->replay_verdict    = ir.replay_verdict;
    out->tpm_version       = ir.tpm_version;
    out->secure_boot       = ir.secure_boot;
    out->secure_boot_valid = ir.secure_boot_valid;
    out->event_count       = ir.event_count;
    out->baseline_status    = ir.baseline_status;
    out->mismatch_cause     = ir.mismatch_cause;
    out->mismatch_pcr       = ir.mismatch_pcr;
    out->mismatch_pcr_valid = ir.mismatch_pcr_valid;
    out->failure_class      = tpm_integrity_failure_class(&ir);
    out->integrity_label    = tpm_integrity_status_label(&ir);
    out->integrity_scope    = tpm_integrity_status_scope(&ir);
    out->kernel_image_measured = 0u;   /* nothing measures the kernel IMAGE (tpm.h) */

    /* A corrupt own identity outranks whatever the published verdict says: the
     * report becomes diagnostic only. No nonce is echoed and no TPM evidence is
     * gathered, because a report that cannot name the kernel it describes must
     * not carry a signature a verifier could accept. */
    if (!out->manifest_valid) {
        out->failure_class = BOOT_INTEGRITY_CLASS_SELF_IDENTITY;
        return TPM_ATTEST_SELF_CORRUPT;
    }

    /* (3b) Manifest PCR binding: claimed by the loader, proved here. */
    {
        uint32_t ne = tpm_event_count();
        const struct tpm_event *ev = ne ? tpm_event_get(0) : 0;
        int ok = tpm_attest_manifest_event_ok((const uint8_t *)g_boot_info.tpm_event_log,
                                              g_boot_info.tpm_event_log_size,
                                              ev, ne, out->manifest_sha256);
        out->pcr_bound = tpm_attest_pcr_bound_decide(h, ok, ir.replay_verdict);
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
        /* tpm2_quote returns OK only once qualifiedSigner equals the Qualified
         * Name it computed for this boot's AK under the EK, so an OK quote has a
         * proven signer. Any failure leaves the status UNVERIFIED. */
        if (out->quote_present)
            out->qualified_signer_status = ATTEST_TRUST_VERIFIED;
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
        out->ek_cert_len = 0u;
        if (eks == TPM_ATTEST_OK) {
            out->ek_cert_len    = eklen;
            out->ek_cert_status = ATTEST_TRUST_UNVERIFIED;  /* present, not yet credential-activated */
        } else if (eks == TPM_ATTEST_NO_EK_CERT) {
            out->ek_cert_status = ATTEST_TRUST_ABSENT;       /* benign: no EK cert provisioned */
        } else {
            /* NO_TPM / TRANSPORT / BADARG / TPMERR: the read FAILED -- a verifier
             * must not treat this like a clean vTPM/fTPM absence. */
            out->ek_cert_status = ATTEST_TRUST_UNKNOWN;
        }
    }

    out->valid = 1u;
    return TPM_ATTEST_OK;
}

/* ----------------------------------------------------------------------------
 * JSON serialization (streaming, bounded; no full-document buffer).
 * ------------------------------------------------------------------------- */

#define RPT_CHUNK 384u   /* largest single snprintf group below stays well under this */

struct rpt_sink {
    attest_json_write_fn wr;
    void    *ctx;
    uint32_t woff;       /* file offset of the next flush */
    uint32_t alen;       /* bytes pending in acc[] */
    int      err;        /* sticky: a write failure or snprintf overflow */
    char     acc[512];   /* coalescing buffer -> one wr() per fill, not per token */
};

static void sink_raw(struct rpt_sink *s, const char *data, uint32_t n)
{
    while (!s->err && n > 0u) {
        uint32_t room = (uint32_t)sizeof s->acc - s->alen;
        uint32_t take = (n < room) ? n : room;
        memcpy(s->acc + s->alen, data, take);
        s->alen += take; data += take; n -= take;
        if (s->alen == sizeof s->acc) {
            if (s->wr(s->ctx, s->woff, (const uint8_t *)s->acc, s->alen) != 0) {
                s->err = 1; return;
            }
            s->woff += s->alen; s->alen = 0u;
        }
    }
}

static void sink_flush(struct rpt_sink *s)
{
    if (s->err || s->alen == 0u)
        return;
    if (s->wr(s->ctx, s->woff, (const uint8_t *)s->acc, s->alen) != 0)
        s->err = 1;
    else { s->woff += s->alen; s->alen = 0u; }
}

static void sink_lit(struct rpt_sink *s, const char *str)
{
    uint32_t n = 0;
    while (str[n]) n++;
    sink_raw(s, str, n);
}

/* Commit an snprintf result; a non-positive or truncated length is a bug -> err. */
static void sink_chunk(struct rpt_sink *s, const char *buf, int n)
{
    if (n <= 0 || (uint32_t)n >= RPT_CHUNK) { s->err = 1; return; }
    sink_raw(s, buf, (uint32_t)n);
}

static void sink_hex(struct rpt_sink *s, const uint8_t *d, uint32_t n)
{
    static const char hx[] = "0123456789abcdef";
    char tmp[64];
    uint32_t t = 0, i;
    for (i = 0; i < n; i++) {
        tmp[t++] = hx[(d[i] >> 4) & 0x0Fu];
        tmp[t++] = hx[d[i] & 0x0Fu];
        if (t >= sizeof tmp) { sink_raw(s, tmp, t); t = 0u; }
    }
    if (t)
        sink_raw(s, tmp, t);
}

/* A capability word as a quoted "0x...." 16-nibble hex string (big-endian). */
static void sink_u64hex(struct rpt_sink *s, uint64_t v)
{
    uint8_t be[8];
    int i;
    for (i = 7; i >= 0; i--) { be[i] = (uint8_t)(v & 0xFFu); v >>= 8; }
    sink_lit(s, "\"0x");
    sink_hex(s, be, 8u);
    sink_lit(s, "\"");
}

/* A JSON string literal, quotes included, from at most `max` bytes of `str`
 * (stopping at NUL). Escapes the quote, the backslash and every control or
 * non-ASCII byte as \u00XX, so firmware- or loader-supplied text cannot break
 * the document. NULL renders the JSON literal null. */
static void sink_jstr(struct rpt_sink *s, const char *str, uint32_t max)
{
    static const char hx[] = "0123456789abcdef";
    uint32_t i;
    if (!str) {
        sink_lit(s, "null");
        return;
    }
    sink_lit(s, "\"");
    for (i = 0; i < max && str[i]; i++) {
        uint8_t c = (uint8_t)str[i];
        if (c == '"' || c == '\\') {
            char e[2] = { '\\', (char)c };
            sink_raw(s, e, 2u);
        } else if (c < 0x20u || c >= 0x7Fu) {
            char e[6] = { '\\', 'u', '0', '0', hx[c >> 4], hx[c & 0x0Fu] };
            sink_raw(s, e, 6u);
        } else {
            sink_raw(s, (const char *)&str[i], 1u);
        }
    }
    sink_lit(s, "\"");
}

/* The scoped integrity verdict, the loader identity and the handoff: the part a
 * diagnostic report and a full report share. */
static void sink_shared_blocks(struct rpt_sink *s, const struct boot_attestation_report *r)
{
    char chunk[RPT_CHUNK];
    int n;

    sink_lit(s, "\"integrity\":{\"label\":");
    sink_jstr(s, r->integrity_label, 64u);
    sink_lit(s, ",\"scope\":");
    sink_jstr(s, r->integrity_scope, 512u);
    sink_lit(s, ",\"failureClass\":");
    sink_jstr(s, tpm_integrity_failure_class_label(r->failure_class), 32u);
    if (r->baseline_status == (uint8_t)BOOT_INTEGRITY_BS_UNPUBLISHED)
        sink_lit(s, ",\"baselineStatus\":null,\"cause\":null,\"causePcr\":null");
    else {
        n = snprintf(chunk, sizeof chunk, ",\"baselineStatus\":%u,\"cause\":",
                     r->baseline_status);
        sink_chunk(s, chunk, n);
        if (r->failure_class == BOOT_INTEGRITY_CLASS_BASELINE_FIELD)
            sink_jstr(s, tpm_baseline_cause_label(r->mismatch_cause), 32u);
        else
            sink_lit(s, "null");
        if (r->failure_class == BOOT_INTEGRITY_CLASS_BASELINE_FIELD && r->mismatch_pcr_valid) {
            n = snprintf(chunk, sizeof chunk, ",\"causePcr\":%u", r->mismatch_pcr);
            sink_chunk(s, chunk, n);
        } else {
            sink_lit(s, ",\"causePcr\":null");
        }
    }
    sink_lit(s, r->kernel_image_measured ? ",\"kernelImageMeasured\":true},"
                                         : ",\"kernelImageMeasured\":false},");

    /* Handoff provenance. */
    n = snprintf(chunk, sizeof chunk, "\"handoff\":{\"valid\":%u,\"capsRequired\":",
                 r->handoff_valid);
    sink_chunk(s, chunk, n);
    sink_u64hex(s, r->caps_required);
    sink_lit(s, ",\"capsPresent\":");
    sink_u64hex(s, r->caps_present);
    sink_lit(s, ",\"capsDegraded\":");
    sink_u64hex(s, r->caps_degraded);
    n = snprintf(chunk, sizeof chunk,
        ",\"bootPath\":%u,\"bootReason\":%u,\"bootSourceFlags\":%u,\"bootFallbackDepth\":%u,",
        r->boot_path, r->boot_reason, r->boot_source_flags, r->boot_fallback_depth);
    sink_chunk(s, chunk, n);
    n = snprintf(chunk, sizeof chunk, "\"loader\":{\"valid\":%u,\"gitSha\":\"",
                 r->loader_identity_valid);
    sink_chunk(s, chunk, n);
    if (r->loader_identity_valid)
        sink_hex(s, r->loader_identity.git_sha, (uint32_t)sizeof r->loader_identity.git_sha);
    n = snprintf(chunk, sizeof chunk, "\",\"buildUnixTime\":%llu,\"buildLabel\":",
                 (unsigned long long)(r->loader_identity_valid
                                          ? r->loader_identity.build_unix_time : 0u));
    sink_chunk(s, chunk, n);
    if (r->loader_identity_valid)
        sink_jstr(s, r->loader_identity.build_label,
                  (uint32_t)sizeof r->loader_identity.build_label);
    else
        sink_lit(s, "null");
    sink_lit(s, "}},");
}

int tpm_attest_report_to_json(const struct boot_attestation_report *r,
                              attest_json_write_fn write, void *ctx)
{
    struct rpt_sink s;
    char chunk[RPT_CHUNK];
    int n;
    uint8_t i;

    if (!r || !write)
        return -1;

    /* Defensive: this is a public serializer, so validate every embedded length
     * against its backing array BEFORE any hex streaming. A corrupted/malformed
     * report must not make sink_hex read past the struct into adjacent kernel
     * memory and leak it into the file. Refuse to serialize, emitting nothing. */
    if (r->pcr_count > ATTEST_REPORT_PCR_MAX ||
        r->nonce_len > sizeof r->nonce ||
        r->quote.nonce_len > sizeof r->quote.nonce ||
        r->quote_sig_len > sizeof r->quote_sig ||
        r->quote.attest_raw_len > sizeof r->quote.attest_raw ||
        r->quote.pcr_digest_len > sizeof r->quote.pcr_digest ||
        r->ak_pub_len > sizeof r->ak_pub ||
        r->ek_cert_len > sizeof r->ek_cert)
        return -1;
    for (i = 0; i < r->pcr_count; i++)
        if (r->pcrs[i].digest_len > sizeof r->pcrs[i].digest)
            return -1;

    s.wr = write; s.ctx = ctx; s.woff = 0u; s.alen = 0u; s.err = 0;

    /* Diagnostic report: this kernel could not validate its own identity, so
     * no TPM evidence was gathered and no trust field may be emitted (their
     * zero values are valid "OK" enums). */
    if (!r->manifest_valid) {
        n = snprintf(chunk, sizeof chunk,
            "{\"schemaVersion\":%u,\"valid\":0,\"diagnostic\":true,"
            "\"overallStatus\":%u,\"replayVerdict\":%u,\"manifest\":{\"valid\":0},",
            r->schema_version, r->overall_status, r->replay_verdict);
        sink_chunk(&s, chunk, n);
        sink_shared_blocks(&s, r);
        sink_lit(&s, "\"end\":true}\n");
        sink_flush(&s);
        return s.err ? -1 : 0;
    }

    n = snprintf(chunk, sizeof chunk,
        "{\"schemaVersion\":%u,\"valid\":%u,\"tpmVersion\":%u,\"overallStatus\":%u,"
        "\"replayVerdict\":%u,\"secureBoot\":%u,\"secureBootValid\":%u,"
        "\"pcrBound\":%u,\"quotePresent\":%u,\"eventCount\":%u,",
        r->schema_version, r->valid, r->tpm_version, r->overall_status,
        r->replay_verdict, r->secure_boot, r->secure_boot_valid,
        r->pcr_bound, r->quote_present, r->event_count);
    sink_chunk(&s, chunk, n);

    /* Kernel-image identity. */
    sink_lit(&s, "\"manifest\":{\"sha256\":\"");
    sink_hex(&s, r->manifest_sha256, (uint32_t)sizeof r->manifest_sha256);
    n = snprintf(chunk, sizeof chunk, "\",\"valid\":1,\"version\":%u,\"structSize\":%u},",
                 r->manifest_version, r->manifest_struct_size);
    sink_chunk(&s, chunk, n);

    sink_shared_blocks(&s, r);

    /* SHA-256 quoted PCR bank. */
    n = snprintf(chunk, sizeof chunk,
        "\"quotedBank\":{\"alg\":%u,\"pcrCoherence\":%u,\"pcrs\":[",
        r->quoted_bank_alg, r->pcr_coherence);
    sink_chunk(&s, chunk, n);
    for (i = 0; i < r->pcr_count && i < ATTEST_REPORT_PCR_MAX; i++) {
        n = snprintf(chunk, sizeof chunk, "%s{\"index\":%u,\"status\":%u,\"digest\":\"",
                     (i ? "," : ""), r->pcrs[i].pcr_index, r->pcrs[i].status);
        sink_chunk(&s, chunk, n);
        sink_hex(&s, r->pcrs[i].digest, r->pcrs[i].digest_len);
        sink_lit(&s, "\"}");
    }
    sink_lit(&s, "]},");

    /* Verifier nonce. */
    sink_lit(&s, "\"nonce\":\"");
    sink_hex(&s, r->nonce, r->nonce_len);
    sink_lit(&s, "\",");

    /* Signed quote. Emit the parsed freshness fields (echoed nonce + clockInfo +
     * firmwareVersion) explicitly so a verifier can do anti-replay/freshness
     * checks from the schema, without re-parsing the raw TPM attest bytes. */
    n = snprintf(chunk, sizeof chunk,
        "\"quote\":{\"present\":%u,\"status\":%u,\"pcrSelect\":%u,"
        "\"resetCount\":%u,\"restartCount\":%u,\"safe\":%u,\"sig\":\"",
        r->quote_present, r->quote_status, r->quote.pcr_select,
        r->quote.reset_count, r->quote.restart_count, r->quote.safe);
    sink_chunk(&s, chunk, n);
    sink_hex(&s, r->quote_sig, r->quote_sig_len);
    sink_lit(&s, "\",\"attestRaw\":\"");
    sink_hex(&s, r->quote.attest_raw, r->quote.attest_raw_len);
    sink_lit(&s, "\",\"pcrDigest\":\"");
    sink_hex(&s, r->quote.pcr_digest, r->quote.pcr_digest_len);
    sink_lit(&s, "\",\"echoedNonce\":\"");
    sink_hex(&s, r->quote.nonce, r->quote.nonce_len);
    sink_lit(&s, "\",\"clock\":");
    sink_u64hex(&s, r->quote.clock);
    sink_lit(&s, ",\"firmwareVersion\":");
    sink_u64hex(&s, r->quote.firmware_version);
    sink_lit(&s, "},");

    /* AK public + binding status. */
    n = snprintf(chunk, sizeof chunk,
        "\"ak\":{\"status\":%u,\"credentialStatus\":%u,\"qualifiedSignerStatus\":%u,\"pub\":\"",
        r->ak_status, r->ak_credential_status, r->qualified_signer_status);
    sink_chunk(&s, chunk, n);
    sink_hex(&s, r->ak_pub, r->ak_pub_len);
    sink_lit(&s, "\"},");

    /* EK certificate + status. */
    n = snprintf(chunk, sizeof chunk,
        "\"ek\":{\"status\":%u,\"certStatus\":%u,\"len\":%u,\"cert\":\"",
        r->ek_status, r->ek_cert_status, r->ek_cert_len);
    sink_chunk(&s, chunk, n);
    sink_hex(&s, r->ek_cert, r->ek_cert_len);
    sink_lit(&s, "\"},");

    /* Forward-compat DRTM slots. */
    n = snprintf(chunk, sizeof chunk,
        "\"drtm\":{\"entryPcr\":%u,\"acmStatus\":%u,\"measurementType\":%u}}\n",
        r->drtm_entry_pcr, r->drtm_acm_status, r->drtm_measurement_type);
    sink_chunk(&s, chunk, n);

    sink_flush(&s);
    return s.err ? -1 : 0;
}

/* The boot's report, serialized once and kept for the query class.
 *
 * WRITE-ONCE PUBLICATION. s_snap_state moves 0 -> 1 (one writer claimed it,
 * by compare-exchange) -> 2 (bytes complete, release store). A reader acquires
 * the state and copies only after seeing 2; the bytes never change after that,
 * so a copy needs no lock and can never observe a half-written document. A
 * second export call cannot claim the slot and leaves the published snapshot
 * alone. Static rather than allocated: the export runs after the scheduler is
 * live, where pmm_alloc_pages_hhdm must not be called. */
static uint8_t  s_snap[ATTEST_REPORT_JSON_MAX];
static uint32_t s_snap_len;
static volatile uint32_t s_snap_state;   /* 0 none, 1 writing, 2 published */

int tpm_attest_json_mem_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t n)
{
    struct attest_json_mem *m = (struct attest_json_mem *)ctx;
    if (!m || off != m->len || n > m->cap - m->len)
        return -1;
    memcpy(m->buf + off, data, n);
    m->len += n;
    return 0;
}

int tpm_attest_report_snapshot(const uint8_t **bytes, uint32_t *len)
{
    if (!bytes || !len)
        return 0;
    if (__atomic_load_n(&s_snap_state, __ATOMIC_ACQUIRE) != 2u)
        return 0;
    *bytes = s_snap;
    *len = s_snap_len;
    return 1;
}

/* Older kernels wrote this boot's evidence to X:\Diag\attestation.json. The
 * diagnostics volume is FAT32, which carries no security descriptor, and no
 * open path authorizes a caller (file-open authorization is owned by the
 * SeAccessCheck engine in the security reference monitor roadmap), so a
 * standard user could read the EK certificate there that the query class
 * refuses them. The evidence therefore lives only in the snapshot, and a copy
 * a previous boot left behind is deleted. */
#define ATTEST_LEGACY_FILE "X:\\Diag\\attestation.json"

int tpm_attest_retire_file(const char *path)
{
    struct vfs_stat st;

    if (!path || vfs_stat(path, &st) != 0)
        return 0;   /* absent, or the volume is not mounted: nothing exposed */
    if (vfs_unlink(path) == 0) {
        klog(LOG_INFO, "TPM", "attestation: removed unprotected %s "
             "(the report is the administrator query class)", path);
        return 1;
    }
    /* The entry could not be removed (the file is open, or the directory
     * update failed to flush). The evidence is the CONTENT, so empty it, and
     * claim that only once a fresh stat confirms zero bytes remain. */
    if (vfs_truncate(path, 0) == 0 && vfs_stat(path, &st) == 0 && st.size == 0) {
        klog(LOG_WARN, "TPM", "attestation: could not remove %s; emptied it instead", path);
        return 2;
    }
    klog(LOG_ERROR, "TPM", "attestation: could neither remove nor empty %s; a previous "
         "boot's evidence stays readable until X:\\Diag opens are authorized", path);
    return -1;
}

void tpm_attest_report_publish(void)
{
    struct boot_attestation_report *r;
    struct attest_json_mem m;
    uint8_t nonce[TPM_QUOTE_NONCE_MAX];
    const uint8_t *np = 0;
    uint16_t nl = 0;
    uint32_t expect = 0u;
    tpm_attest_status_t bs;
    int rc;

    /* Every outcome is logged inside; a -1 (neither removed nor emptied, so
     * the volume refuses writes) is closed only by authorizing X:\\Diag
     * opens, owned by the SeAccessCheck engine work. The query still ships. */
    (void)tpm_attest_retire_file(ATTEST_LEGACY_FILE);

    /* One boot, one report: a second call keeps the published snapshot. */
    if (!__atomic_compare_exchange_n(&s_snap_state, &expect, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        klog(LOG_WARN, "TPM", "attestation export: report already built this boot");
        return;
    }

    r = (struct boot_attestation_report *)kmalloc(sizeof *r);
    if (!r) {
        klog(LOG_WARN, "TPM", "attestation export: out of memory -> no report this boot");
        return;   /* state stays 1: never published, never retried */
    }

    /* A fresh CSPRNG nonce so the report carries a signed quote; fall back to an
     * unsigned self-test report when the CSPRNG is not yet crypto-ready. */
    if (csprng_crypto_ok()) {
        csprng_fill(nonce, sizeof nonce);
        np = nonce;
        nl = (uint16_t)sizeof nonce;
    }
    /* The builder's status is load-bearing, not advisory. SELF_CORRUPT (this
     * kernel's own ABI identity failed validation) still yields a report, a
     * DIAGNOSTIC one: manifest_valid 0, the integrity verdict and handoff, and
     * no TPM evidence, which the serializer renders without any trust field.
     * That is the one case a relying party most needs to see. Every other
     * non-OK status means the struct holds unevaluated zeros, and zero is a
     * VALID enum value for quote_status, ak_status and ek_status, so nothing is
     * published. */
    bs = tpm_attest_report_build(np, nl, r);
    if (bs != TPM_ATTEST_OK && bs != TPM_ATTEST_SELF_CORRUPT) {
        kfree(r);
        klog(LOG_ERROR, "TPM",
             "attestation export: report build failed (status %u) -> no report this boot",
             (uint64_t)bs);
        return;
    }
    if (bs == TPM_ATTEST_SELF_CORRUPT)
        klog(LOG_ERROR, "TPM",
             "attestation export: kernel identity invalid -> diagnostic report only");

    m.buf = s_snap;
    m.cap = (uint32_t)sizeof s_snap;
    m.len = 0u;
    rc = tpm_attest_report_to_json(r, tpm_attest_json_mem_write, &m);
    kfree(r);
    if (rc != 0) {
        klog(LOG_ERROR, "TPM", "attestation export: report exceeds %u bytes -> not published",
             (uint64_t)ATTEST_REPORT_JSON_MAX);
        return;
    }
    s_snap_len = m.len;
    __atomic_store_n(&s_snap_state, 2u, __ATOMIC_RELEASE);
    klog(LOG_INFO, "TPM", "attestation report published (%u bytes, administrator query class)",
         (uint64_t)m.len);
}
