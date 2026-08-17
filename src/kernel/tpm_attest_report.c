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
    out->pcr_bound               = 0u;   /* gates on BOOT_CAP_MANIFEST_PCR_BOUND once that cap-bit ships */
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
    if (!boot_proto_abi_digest(out->manifest_sha256))
        return TPM_ATTEST_SELF_CORRUPT;
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
    n = snprintf(chunk, sizeof chunk, "\",\"version\":%u,\"structSize\":%u},",
                 r->manifest_version, r->manifest_struct_size);
    sink_chunk(&s, chunk, n);

    /* Handoff provenance. */
    n = snprintf(chunk, sizeof chunk, "\"handoff\":{\"valid\":%u,\"capsRequired\":",
                 r->handoff_valid);
    sink_chunk(&s, chunk, n);
    sink_u64hex(&s, r->caps_required);
    sink_lit(&s, ",\"capsPresent\":");
    sink_u64hex(&s, r->caps_present);
    sink_lit(&s, ",\"capsDegraded\":");
    sink_u64hex(&s, r->caps_degraded);
    n = snprintf(chunk, sizeof chunk,
        ",\"bootPath\":%u,\"bootReason\":%u,\"bootSourceFlags\":%u,\"bootFallbackDepth\":%u},",
        r->boot_path, r->boot_reason, r->boot_source_flags, r->boot_fallback_depth);
    sink_chunk(&s, chunk, n);

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

static int rpt_vfs_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t n)
{
    struct vfs_node *f = (struct vfs_node *)ctx;
    return (vfs_write(f, off, n, data) == (int)n) ? 0 : -1;
}

void tpm_attest_report_export(void)
{
    struct boot_attestation_report *r;
    struct vfs_node *dir, *f;
    uint8_t nonce[TPM_QUOTE_NONCE_MAX];
    const uint8_t *np = 0;
    uint16_t nl = 0;
    int rc;
    tpm_attest_status_t bs;

    /* Open the destination FIRST, before any live TPM work: on a boot where
     * diagnostics storage is unavailable we must not pay the quote + AK/EK
     * transaction latency only to discard the result. Create via the parent dir
     * (FAT32 dir-cache re-walk). O_TRUNC: a shorter report must not leave stale
     * previous-boot tail bytes. */
    dir = vfs_open("X:\\Diag\\", VFS_O_READ);
    if (dir && dir->ops && dir->ops->create)
        dir->ops->create(dir, "attestation.json", VFS_FILE);
    if (dir)
        vfs_close(dir);
    f = vfs_open("X:\\Diag\\attestation.json", VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        klog(LOG_WARN, "TPM", "attestation export: cannot open X:\\Diag\\attestation.json");
        return;   /* no TPM work paid on unwritable storage */
    }

    r = (struct boot_attestation_report *)kmalloc(sizeof *r);
    if (!r) {
        klog(LOG_WARN, "TPM", "attestation export: out of memory");
        vfs_close(f);
        return;
    }

    /* A fresh CSPRNG challenge so the file carries a signed quote; fall back to an
     * unsigned self-test report when the CSPRNG is not yet crypto-ready. */
    if (csprng_crypto_ok()) {
        csprng_fill(nonce, sizeof nonce);
        np = nonce;
        nl = (uint16_t)sizeof nonce;
    }
    /* The builder's status is load-bearing, not advisory. It can now abandon the
     * report early (SELF_CORRUPT: this kernel's own ABI identity failed
     * validation), at which point the struct holds only the memset zeros plus a
     * few defaults -- and zero is a VALID enum value for quote_status,
     * ak_status and ek_status, so serializing it would publish TPM_ATTEST_OK
     * for three trust fields that were never evaluated, under a success log.
     * Write nothing instead: an absent artifact is honest, a confidently-wrong
     * one is not. The file is truncated on open, so closing without writing
     * already leaves it empty; the explicit re-truncate keeps that true even if
     * a future edit writes a prefix before this point. */
    bs = tpm_attest_report_build(np, nl, r);
    if (bs != TPM_ATTEST_OK) {
        struct vfs_node *z;
        vfs_close(f);
        kfree(r);
        z = vfs_open("X:\\Diag\\attestation.json", VFS_O_WRITE | VFS_O_TRUNC);
        if (z)
            vfs_close(z);
        klog(LOG_ERROR, "TPM",
             "attestation export: report build failed (status %u) -> no artifact written",
             (uint64_t)bs);
        return;
    }

    rc = tpm_attest_report_to_json(r, rpt_vfs_write, f);
    vfs_close(f);
    kfree(r);

    if (rc != 0) {
        /* A mid-stream write failure may have left a partial JSON prefix on disk.
         * Re-truncate to zero so a consumer sees a clear empty (absent) artifact,
         * never a corrupt partial report. The cleanup reopen can ITSELF fail under
         * the same storage fault -- only claim "zeroed" when it actually happened,
         * else warn loudly that a partial file may remain. */
        struct vfs_node *z = vfs_open("X:\\Diag\\attestation.json",
                                      VFS_O_WRITE | VFS_O_TRUNC);
        if (z) {
            vfs_close(z);
            klog(LOG_WARN, "TPM",
                 "attestation export: write error -> zeroed partial X:\\Diag\\attestation.json");
        } else {
            klog(LOG_ERROR, "TPM",
                 "attestation export: write error AND could not zero -> partial "
                 "X:\\Diag\\attestation.json may remain");
        }
    } else {
        klog(LOG_INFO, "TPM", "attestation report exported: X:\\Diag\\attestation.json");
    }
}
