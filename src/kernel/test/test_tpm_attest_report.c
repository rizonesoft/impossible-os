/* Unit tests for the TPM-rooted boot attestation report export: the immutable
 * bootloader-to-kernel handoff snapshot and the report builder. */

#include "kernel/test/test.h"
#include "kernel/test/boot_info_fixture.h" /* TEST_BOOT_INFO: the shared fixture */
#include "kernel/tpm_attest_report.h"
#include "kernel/tpm.h"                   /* TPM_ALG_SHA256 */
#include "kernel/boot_info.h"
#include "kernel/boot_proto_descriptor.h" /* kernel_boot_proto identity check */
#include "kernel/tpm_replay.h"            /* TPM_REPLAY_* verdicts */
#include "kernel/tpm_baseline.h"          /* TPM_BASELINE_* status and cause */
#include "kernel/crypto/sha256.h"
#include "boot/pcr_manifest.h"            /* the loader's PCR-11 manifest event */
#include "libc/string.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/ssdt.h"               /* ssdt_dispatch */
#include "kernel/nt/zw.h"                 /* ssdt_set_previous_mode, SSDT_USER_MODE */
#include "kernel/klog.h"
#include "kernel/nt/service_numbers.h"    /* SSDT_NtQuerySystemInformation */
#include "kernel/nt/attest_syscall_info.h"
#include "kernel/security/token.h"        /* SeCreateUserToken */
#include "kernel/ob/ob.h"
#include "kernel/sched/task.h"

extern const struct boot_proto_descriptor kernel_boot_proto;
extern NTSTATUS nt_query_attestation_information_from(int allowed, const uint8_t *snap,
                                                      uint32_t snap_len, void *buffer,
                                                      uint32_t buf_size,
                                                      uint32_t *return_length);

/* Static fixtures off the test stack: boot_info is large and the report is ~2.3 KB. */
static struct boot_attestation_report s_report;

/* In-memory JSON sink so the serializer is tested without a live filesystem. */
static uint8_t  s_json_buf[8192];
static uint32_t s_json_len;

static int json_mem_writer(void *ctx, uint32_t off, const uint8_t *data, uint32_t n)
{
    (void)ctx;
    if (off > sizeof s_json_buf || n > (uint32_t)sizeof s_json_buf - off)
        return -1;
    memcpy(s_json_buf + off, data, n);
    if (off + n > s_json_len)
        s_json_len = off + n;
    return 0;
}

static int buf_contains(const uint8_t *hay, uint32_t hlen, const char *needle)
{
    uint32_t nl = 0, i;
    while (needle[nl]) nl++;
    if (nl == 0u || nl > hlen)
        return 0;
    for (i = 0; i + nl <= hlen; i++)
        if (memcmp(hay + i, needle, nl) == 0)
            return 1;
    return 0;
}

static void test_attest_handoff_capture(void)
{
    struct boot_attest_handoff h;

    memset(&TEST_BOOT_INFO, 0, sizeof TEST_BOOT_INFO);
    TEST_BOOT_INFO.caps_required       = 0x1234u;
    TEST_BOOT_INFO.caps_present        = 0x5678u;
    TEST_BOOT_INFO.caps_degraded       = 0x9ABCu;
    TEST_BOOT_INFO.boot_path           = 3u;
    TEST_BOOT_INFO.boot_reason         = 7u;
    TEST_BOOT_INFO.boot_source_flags   = 0xF0u;
    TEST_BOOT_INFO.boot_fallback_depth = 2u;

    tpm_attest_handoff_capture(&TEST_BOOT_INFO, &h);
    TEST_ASSERT_EQ((uint32_t)h.valid, 1u, "capture marks the snapshot valid");
    TEST_ASSERT_EQ((uint32_t)h.caps_required, 0x1234u, "caps_required copied");
    TEST_ASSERT_EQ((uint32_t)h.caps_present, 0x5678u, "caps_present copied");
    TEST_ASSERT_EQ((uint32_t)h.caps_degraded, 0x9ABCu, "caps_degraded copied");
    TEST_ASSERT_EQ(h.boot_path, 3u, "boot_path copied");
    TEST_ASSERT_EQ(h.boot_reason, 7u, "boot_reason copied");
    TEST_ASSERT_EQ(h.boot_source_flags, 0xF0u, "boot_source_flags copied");
    TEST_ASSERT_EQ(h.boot_fallback_depth, 2u, "boot_fallback_depth copied");
    TEST_ASSERT_EQ((uint32_t)h.loader_identity_valid, 0u,
                   "all-zero git_sha is the loader-did-not-populate sentinel");

    /* The loader build identity is part of the handoff triple and is copied
     * verbatim, so the report never re-reads the live struct for it. */
    TEST_BOOT_INFO.loader_identity.git_sha[19]     = 0x5Au;
    TEST_BOOT_INFO.loader_identity.build_unix_time = 1789000000ull;
    memcpy(TEST_BOOT_INFO.loader_identity.build_label, "v1.2-dirty", 11);
    tpm_attest_handoff_capture(&TEST_BOOT_INFO, &h);
    TEST_ASSERT_EQ((uint32_t)h.loader_identity_valid, 1u, "populated identity is valid");
    TEST_ASSERT_EQ((uint32_t)h.loader_identity.git_sha[19], 0x5Au, "git_sha copied");
    TEST_ASSERT(h.loader_identity.build_unix_time == 1789000000ull, "build time copied");
    TEST_ASSERT_EQ(memcmp(h.loader_identity.build_label, "v1.2-dirty", 11), 0,
                   "build label copied");

    /* The load-bearing property: the snapshot is a COPY. A later kernel refinement
     * of the live boot_info (e.g. boot_caps_mark_present) must NOT change what the
     * attestation report attributes to the loader handoff. */
    TEST_BOOT_INFO.caps_present = 0xFFFFu;
    TEST_ASSERT_EQ((uint32_t)h.caps_present, 0x5678u,
                   "snapshot immutable to a later boot_info caps change");

    /* NULL boot_info -> invalid (fail closed), never a stale/garbage snapshot. */
    memset(&h, 0xAA, sizeof h);
    tpm_attest_handoff_capture(0, &h);
    TEST_ASSERT_EQ((uint32_t)h.valid, 0u, "NULL boot_info -> invalid snapshot");

    /* snapshot_init is WRITE-ONCE, enforced by a latch (no public reset). boot_phase0
     * already captured the real Phase-0 snapshot, so a later init with a fixture MUST
     * no-op -- the getter keeps the loader-time boot snapshot, never kernel-refined
     * caps. This proves a later accidental call cannot replace loader evidence. */
    {
        const struct boot_attest_handoff *g = tpm_attest_handoff_get();
        uint64_t boot_caps;
        TEST_ASSERT(g != 0, "handoff getter is never NULL");
        TEST_ASSERT_EQ((uint32_t)g->valid, 1u, "boot_phase0 captured a valid snapshot");
        boot_caps = g->caps_present;
        memset(&TEST_BOOT_INFO, 0, sizeof TEST_BOOT_INFO);
        TEST_BOOT_INFO.caps_present = ~boot_caps;        /* a value the boot snapshot is not */
        tpm_attest_handoff_snapshot_init(&TEST_BOOT_INFO);   /* latched -> no-op */
        TEST_ASSERT_EQ((uint32_t)g->caps_present, (uint32_t)boot_caps,
                       "write-once: a later init does not overwrite the boot snapshot");
    }
}

/* The report builder. TPM-presence-independent: asserts the structural and
 * invariant properties that hold whether or not the test host has a live TPM
 * (the harness usually has none, so the quote/AK/EK paths return NO_TPM). */
static void test_attest_report_build(void)
{
    const struct boot_attest_handoff *h;
    uint8_t nonce[TPM_QUOTE_NONCE_MAX + 1];
    uint8_t i;
    tpm_attest_status_t st;

    for (i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)(0xA0u + i);

    /* NULL out -> BADARG, nothing else. */
    st = tpm_attest_report_build(nonce, TPM_QUOTE_NONCE_MIN, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_BADARG, "NULL out -> BADARG");

    /* Malformed verifier challenges are caller errors (BADARG), never silently
     * absorbed into a valid degraded report. */
    st = tpm_attest_report_build(nonce, TPM_QUOTE_NONCE_MIN - 1u, &s_report);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_BADARG, "too-short nonce -> BADARG");
    st = tpm_attest_report_build(nonce, TPM_QUOTE_NONCE_MAX + 1u, &s_report);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_BADARG, "over-max nonce -> BADARG");
    st = tpm_attest_report_build(0, TPM_QUOTE_NONCE_MIN, &s_report);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_BADARG, "NULL nonce + nonzero len -> BADARG");

    st = tpm_attest_report_build(nonce, TPM_QUOTE_NONCE_MIN, &s_report);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_OK, "build returns OK");
    TEST_ASSERT_EQ((uint32_t)s_report.valid, 1u, "report assembled valid");
    TEST_ASSERT_EQ(s_report.schema_version, ATTEST_REPORT_SCHEMA_VERSION, "schema version stamped");
    TEST_ASSERT_EQ((uint32_t)s_report.quoted_bank_alg, (uint32_t)TPM_ALG_SHA256,
                   "only the SHA-256 bank is TPM-quoted");

    /* (1) Kernel-image identity sourced from the immutable .bootproto const. */
    TEST_ASSERT_EQ(memcmp(s_report.manifest_sha256, kernel_boot_proto.sha256, 32), 0,
                   "manifest sha256 == kernel_boot_proto.sha256");
    TEST_ASSERT_EQ(s_report.manifest_version, kernel_boot_proto.version, "manifest version copied");
    TEST_ASSERT_EQ(s_report.manifest_struct_size, kernel_boot_proto.struct_size,
                   "manifest struct_size copied");

    /* (2) Handoff provenance mirrors the immutable Phase-0 snapshot. */
    h = tpm_attest_handoff_get();
    TEST_ASSERT_EQ((uint32_t)s_report.handoff_valid, (uint32_t)h->valid, "handoff_valid mirrored");
    TEST_ASSERT_EQ((uint32_t)s_report.caps_present, (uint32_t)h->caps_present, "caps_present mirrored");
    TEST_ASSERT_EQ((uint32_t)s_report.boot_path, h->boot_path, "boot_path mirrored");

    /* SHA-256 PCR bank: the full quote set {0-7,11} is enumerated in order. */
    TEST_ASSERT_EQ((uint32_t)s_report.pcr_count, (uint32_t)ATTEST_REPORT_PCR_MAX,
                   "all 9 quote-mask PCRs enumerated");
    TEST_ASSERT_EQ((uint32_t)s_report.pcrs[0].pcr_index, 0u, "first quoted PCR is 0");
    TEST_ASSERT_EQ((uint32_t)s_report.pcrs[8].pcr_index, 11u, "last quoted PCR is 11");

    /* Nonce echoed into the report before the quote (exact, no truncation). */
    TEST_ASSERT_EQ((uint32_t)s_report.nonce_len, (uint32_t)TPM_QUOTE_NONCE_MIN, "nonce length echoed");
    TEST_ASSERT_EQ(memcmp(s_report.nonce, nonce, TPM_QUOTE_NONCE_MIN), 0, "nonce bytes echoed");

    /* Quote/coherence invariants (hold regardless of TPM presence). */
    TEST_ASSERT_EQ((uint32_t)s_report.quote_present,
                   (uint32_t)(s_report.quote_status == (uint8_t)TPM_ATTEST_OK ? 1u : 0u),
                   "quote_present iff quote_status OK");
    if (!s_report.quote_present)
        TEST_ASSERT_EQ((uint32_t)s_report.pcr_coherence, (uint32_t)ATTEST_COHERENCE_NA,
                       "no quote -> coherence NA");

    /* Trust model: AK<->EK certification is never auto-trusted by this build;
     * the quote signer is proven exactly when a quote is present (tpm2_quote
     * returns OK only on a qualifiedSigner match). */
    TEST_ASSERT_EQ((uint32_t)s_report.ak_credential_status, (uint32_t)ATTEST_TRUST_UNVERIFIED,
                   "AK credential binding stays UNVERIFIED");
    TEST_ASSERT_EQ((uint32_t)s_report.qualified_signer_status,
                   s_report.quote_present ? (uint32_t)ATTEST_TRUST_VERIFIED
                                          : (uint32_t)ATTEST_TRUST_UNVERIFIED,
                   "qualified signer VERIFIED iff a quote is present");
    /* EK cert trust: UNVERIFIED if present, ABSENT only for a clean no-cert,
     * UNKNOWN for a read/transport failure (must not look like benign absence). */
    {
        uint32_t want_ek = (s_report.ek_status == (uint8_t)TPM_ATTEST_OK)
                               ? ATTEST_TRUST_UNVERIFIED
                               : (s_report.ek_status == (uint8_t)TPM_ATTEST_NO_EK_CERT)
                                     ? ATTEST_TRUST_ABSENT
                                     : ATTEST_TRUST_UNKNOWN;
        TEST_ASSERT_EQ((uint32_t)s_report.ek_cert_status, want_ek,
                       "EK cert: UNVERIFIED present / ABSENT no-cert / UNKNOWN on read error");
    }

    /* Forward-compat DRTM slots zeroed (no SENTER/SKINIT today). */
    TEST_ASSERT_EQ((uint32_t)s_report.drtm_entry_pcr, 0xFFu, "DRTM entry PCR = none");
    TEST_ASSERT_EQ((uint32_t)s_report.drtm_measurement_type, 0u, "DRTM measurement type = none");

    /* NULL/0 is the explicit unsigned self-test mode: builds OK, no nonce, no
     * quote attempted (quote_present 0). */
    st = tpm_attest_report_build(0, 0, &s_report);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_OK, "self-test (NULL nonce) builds");
    TEST_ASSERT_EQ((uint32_t)s_report.valid, 1u, "self-test report valid");
    TEST_ASSERT_EQ((uint32_t)s_report.nonce_len, 0u, "self-test -> zero nonce length");
    TEST_ASSERT_EQ((uint32_t)s_report.quote_present, 0u, "self-test attempts no quote");
    TEST_ASSERT_EQ((uint32_t)s_report.pcr_coherence, (uint32_t)ATTEST_COHERENCE_NA,
                   "self-test -> coherence NA");
}

/* The JSON serializer, exercised through an in-memory write sink. */
static void test_attest_report_json(void)
{
    uint8_t nonce[TPM_QUOTE_NONCE_MIN];
    uint8_t i;
    int rc;

    for (i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)(0x10u + i);

    /* NULL guards. */
    TEST_ASSERT_EQ(tpm_attest_report_to_json(0, json_mem_writer, 0), -1, "NULL report -> -1");
    rc = (int)tpm_attest_report_build(nonce, sizeof nonce, &s_report);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)TPM_ATTEST_OK, "report built for serialize");
    TEST_ASSERT_EQ(tpm_attest_report_to_json(&s_report, 0, 0), -1, "NULL writer -> -1");

    s_json_len = 0u;
    memset(s_json_buf, 0, sizeof s_json_buf);
    rc = tpm_attest_report_to_json(&s_report, json_mem_writer, 0);
    TEST_ASSERT_EQ(rc, 0, "serialize ok");
    TEST_ASSERT(s_json_len > 0u, "json non-empty");
    TEST_ASSERT(s_json_len < sizeof s_json_buf, "json within buffer");

    /* Well-formed envelope. */
    TEST_ASSERT_EQ(memcmp(s_json_buf, "{\"schemaVersion\":3,", 19), 0, "json header prefix");
    TEST_ASSERT_EQ((uint32_t)s_json_buf[s_json_len - 1u], (uint32_t)'\n', "trailing newline");
    TEST_ASSERT_EQ((uint32_t)s_json_buf[s_json_len - 2u], (uint32_t)'}', "closes top object");

    /* Required sections (all values are numbers or hex strings -- no escaping). */
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"manifest\":{\"sha256\":\""), "manifest section");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"quotedBank\":{"), "quoted-bank section");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"drtm\":{"), "drtm section");
    /* Quote freshness fields must be in the JSON schema, not only in attestRaw. */
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"echoedNonce\":\""), "quote echoed nonce");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"clock\":"), "quote clockInfo");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"resetCount\":"), "quote reset count");
    /* The 8-byte nonce 0x10..0x17 must round-trip as lowercase hex. */
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"nonce\":\"1011121314151617\""),
                "nonce hex echoed");

    /* A malformed embedded length must be refused with ZERO bytes emitted, so a
     * corrupt report can never drive an OOB read into the output file. */
    rc = (int)tpm_attest_report_build(nonce, sizeof nonce, &s_report);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)TPM_ATTEST_OK, "rebuild clean report");
    s_report.ek_cert_len = (uint16_t)(sizeof s_report.ek_cert + 1u);
    s_json_len = 0u;
    rc = tpm_attest_report_to_json(&s_report, json_mem_writer, 0);
    TEST_ASSERT_EQ(rc, -1, "overlong ek_cert_len -> -1");
    TEST_ASSERT_EQ((uint32_t)s_json_len, 0u, "malformed report emits no bytes");
}

/* One synthetic EVENT2 record: a {alg, digest} list at `dig_off` and the event
 * data at `pay_off`, both inside s_log. */
static uint8_t s_log[256];

static void mk_event(struct tpm_event *e, uint32_t pcr, uint32_t type,
                     uint32_t dig_off, uint16_t alg, const uint8_t *digest,
                     uint32_t pay_off, const char *payload, uint32_t pay_len)
{
    uint16_t dl = tpm_alg_digest_len_pub(alg);
    memset(e, 0, sizeof *e);
    e->pcr_index    = pcr;
    e->event_type   = type;
    e->digest_count = 1u;
    e->digests_off  = dig_off;
    e->payload_off  = pay_off;
    e->payload_size = pay_len;
    s_log[dig_off]      = (uint8_t)(alg & 0xFFu);
    s_log[dig_off + 1u] = (uint8_t)(alg >> 8);
    memcpy(s_log + dig_off + 2u, digest, dl);
    memcpy(s_log + pay_off, payload, pay_len);
}

static void test_attest_manifest_binding(void)
{
    static const char name[] = TPM_PCR_MANIFEST_EVENT;
    const uint32_t nl = (uint32_t)sizeof name - 1u;
    uint8_t m[32], good[32], bad[32];
    struct tpm_event ev[3];
    struct boot_attest_handoff h;
    uint8_t i;

    for (i = 0; i < 32u; i++) m[i] = (uint8_t)(0x30u + i);
    sha256(m, 32u, good);
    memcpy(bad, good, 32);
    bad[0] ^= 0x01u;

    /* The real shape: an unrelated event first, then the manifest record. */
    memset(s_log, 0, sizeof s_log);
    mk_event(&ev[0], 4u, 0x80000003u, 0u, TPM_ALG_SHA256, bad, 34u, "other", 5u);
    mk_event(&ev[1], TPM_PCR_MANIFEST_INDEX, TPM_PCR_MANIFEST_EVENT_TYPE,
             40u, TPM_ALG_SHA256, good, 74u, name, nl);
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 2u, m), 1,
                   "logged SHA-256(manifest) on PCR 11 binds");
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 1u, m), 0,
                   "no manifest event -> not bound");
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, 74u + nl - 1u, ev, 2u, m), 0,
                   "event data past the log end -> not bound");

    /* A different manifest than this kernel's. */
    m[5] ^= 0x80u;
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 2u, m), 0,
                   "digest of another manifest -> not bound");
    m[5] ^= 0x80u;

    /* Wrong PCR, wrong type, the name with its NUL: none is the manifest event. */
    ev[1].pcr_index = 12u;
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 2u, m), 0,
                   "manifest event on another PCR -> not bound");
    ev[1].pcr_index = TPM_PCR_MANIFEST_INDEX;
    ev[1].event_type = 0x0Eu;
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 2u, m), 0,
                   "manifest name under another event type -> not bound");
    ev[1].event_type = TPM_PCR_MANIFEST_EVENT_TYPE;
    ev[1].payload_size = nl + 1u;
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 2u, m), 0,
                   "name plus a trailing byte is a different name");
    ev[1].payload_size = nl;

    /* A second manifest record with another digest voids the first. */
    mk_event(&ev[2], TPM_PCR_MANIFEST_INDEX, TPM_PCR_MANIFEST_EVENT_TYPE,
             120u, TPM_ALG_SHA256, bad, 154u, name, nl);
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 3u, m), 0,
                   "two disagreeing manifest records -> not bound");

    /* A SHA-1-only record has no SHA-256 digest to check. */
    mk_event(&ev[2], TPM_PCR_MANIFEST_INDEX, TPM_PCR_MANIFEST_EVENT_TYPE,
             120u, TPM_ALG_SHA1, good, 154u, name, nl);
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 3u, m), 0,
                   "manifest record without a SHA-256 digest -> not bound");
    ev[1].legacy = 1u;
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(s_log, sizeof s_log, ev, 2u, m), 0,
                   "legacy SHA-1 record -> not bound");
    TEST_ASSERT_EQ(tpm_attest_manifest_event_ok(0, sizeof s_log, ev, 2u, m), 0, "NULL log");

    /* The decision: the capability, the record and the replay, all three. */
    memset(&h, 0, sizeof h);
    h.valid = 1u;
    h.caps_present = BOOT_CAP_MANIFEST_PCR_BOUND;
    TEST_ASSERT_EQ((uint32_t)tpm_attest_pcr_bound_decide(&h, 1, (uint8_t)TPM_REPLAY_VERIFIED), 1u,
                   "claimed + recorded + replayed -> bound");
    TEST_ASSERT_EQ((uint32_t)tpm_attest_pcr_bound_decide(&h, 1, (uint8_t)TPM_REPLAY_TAMPER), 0u,
                   "a record in a tampered log proves nothing");
    TEST_ASSERT_EQ((uint32_t)tpm_attest_pcr_bound_decide(&h, 1, (uint8_t)TPM_REPLAY_UNVERIFIABLE), 0u,
                   "an unreplayed log proves nothing");
    TEST_ASSERT_EQ((uint32_t)tpm_attest_pcr_bound_decide(&h, 0, (uint8_t)TPM_REPLAY_VERIFIED), 0u,
                   "claim without the record -> not bound");
    h.caps_present = 0u;
    h.caps_degraded = BOOT_CAP_MANIFEST_PCR_BOUND;
    TEST_ASSERT_EQ((uint32_t)tpm_attest_pcr_bound_decide(&h, 1, (uint8_t)TPM_REPLAY_VERIFIED), 0u,
                   "record without the loader's claim -> not bound");
    h.caps_present = BOOT_CAP_MANIFEST_PCR_BOUND;
    h.valid = 0u;
    TEST_ASSERT_EQ((uint32_t)tpm_attest_pcr_bound_decide(&h, 1, (uint8_t)TPM_REPLAY_VERIFIED), 0u,
                   "invalid handoff -> not bound");
    TEST_ASSERT_EQ((uint32_t)tpm_attest_pcr_bound_decide(0, 1, (uint8_t)TPM_REPLAY_VERIFIED), 0u,
                   "NULL handoff -> not bound");
}

static void json_render(void)
{
    s_json_len = 0u;
    memset(s_json_buf, 0, sizeof s_json_buf);
    TEST_ASSERT_EQ(tpm_attest_report_to_json(&s_report, json_mem_writer, 0), 0, "serialize ok");
}

/* The scoped verdict as a relying party reads it, one case per class. */
static void test_attest_report_scoped_verdict(void)
{
    struct boot_proto_descriptor bad;
    tpm_attest_status_t st;

    st = tpm_attest_report_build(0, 0, &s_report);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_OK, "self-test report builds");
    TEST_ASSERT_EQ((uint32_t)s_report.manifest_valid, 1u, "own identity validates");
    TEST_ASSERT_EQ((uint32_t)s_report.kernel_image_measured, 0u,
                   "no kernel image digest is claimed");
    TEST_ASSERT(s_report.integrity_label != 0 && s_report.integrity_scope != 0,
                "label and scope are filled from the published verdict");

    /* Baseline-verified. */
    s_report.overall_status = (uint8_t)BOOT_INTEGRITY_VERIFIED;
    s_report.failure_class = (uint8_t)BOOT_INTEGRITY_CLASS_NONE;
    s_report.baseline_status = (uint8_t)TPM_BASELINE_OK;
    s_report.integrity_label = "baseline-verified";
    json_render();
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"label\":\"baseline-verified\""),
                "verified label");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len,
                             "\"failureClass\":\"none\",\"baselineStatus\":0,\"cause\":null,\"causePcr\":null"),
                "verified carries no cause");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"kernelImageMeasured\":false"),
                "image-measured flag is explicit");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"manifest\":{\"sha256\":\""),
                "manifest identity present");

    /* Field mismatch with its PCR. */
    s_report.overall_status = (uint8_t)BOOT_INTEGRITY_MISMATCH;
    s_report.failure_class = (uint8_t)BOOT_INTEGRITY_CLASS_BASELINE_FIELD;
    s_report.mismatch_cause = (uint8_t)TPM_BASELINE_CAUSE_PCR_DIGEST;
    s_report.mismatch_pcr = 7u;
    s_report.mismatch_pcr_valid = 1u;
    json_render();
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len,
                             "\"failureClass\":\"baseline-field\",\"baselineStatus\":0,"
                             "\"cause\":\"pcr-digest\",\"causePcr\":7"),
                "field mismatch names the field and the PCR");

    /* Stored-baseline failure: the class, no field cause. */
    s_report.failure_class = (uint8_t)BOOT_INTEGRITY_CLASS_STORED_BASELINE;
    s_report.baseline_status = (uint8_t)TPM_BASELINE_TORN;
    json_render();
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len,
                             "\"failureClass\":\"stored-baseline\",\"baselineStatus\":8,"
                             "\"cause\":null,\"causePcr\":null"),
                "blob failure carries the class and no field cause");

    /* Replay tamper. */
    s_report.failure_class = (uint8_t)BOOT_INTEGRITY_CLASS_REPLAY_TAMPER;
    json_render();
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"failureClass\":\"event-log-tamper\""),
                "tamper class");

    /* Nothing published yet. */
    s_report.overall_status = (uint8_t)BOOT_INTEGRITY_NO_BASELINE;
    s_report.failure_class = (uint8_t)BOOT_INTEGRITY_CLASS_NONE;
    s_report.baseline_status = (uint8_t)BOOT_INTEGRITY_BS_UNPUBLISHED;
    json_render();
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len,
                             "\"baselineStatus\":null,\"cause\":null,\"causePcr\":null"),
                "unpublished status renders null, not 0 (OK)");

    /* Loader text is escaped: a quote, a backslash and a control byte. */
    s_report.loader_identity_valid = 1u;
    memset(s_report.loader_identity.build_label, 0, sizeof s_report.loader_identity.build_label);
    memcpy(s_report.loader_identity.build_label, "a\"b\\c\x01", 6);
    json_render();
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"buildLabel\":\"a\\\"b\\\\c\\u0001\""),
                "build label JSON-escaped");
    /* An unterminated 24-byte label stops at the field, never past it. */
    memset(s_report.loader_identity.build_label, 'x', sizeof s_report.loader_identity.build_label);
    json_render();
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len,
                             "\"buildLabel\":\"xxxxxxxxxxxxxxxxxxxxxxxx\"}"),
                "unterminated label bounded at 24 bytes");

    /* Class 3 through the real builder: a corrupt own descriptor yields a
     * diagnostic report, never an empty one and never one with trust fields. */
    memcpy(&bad, &kernel_boot_proto, sizeof bad);
    bad.magic ^= 0xFFFFFFFFu;
    st = tpm_attest_report_build_from(&bad, 0, 0, &s_report);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)TPM_ATTEST_SELF_CORRUPT, "corrupt identity -> SELF_CORRUPT");
    TEST_ASSERT_EQ((uint32_t)s_report.valid, 0u, "diagnostic report is not valid evidence");
    TEST_ASSERT_EQ((uint32_t)s_report.manifest_valid, 0u, "identity marked invalid");
    TEST_ASSERT_EQ((uint32_t)s_report.failure_class, (uint32_t)BOOT_INTEGRITY_CLASS_SELF_IDENTITY,
                   "class is self-identity");
    TEST_ASSERT_EQ((uint32_t)s_report.pcr_bound, 0u, "no binding claimed");
    json_render();
    TEST_ASSERT_EQ(memcmp(s_json_buf, "{\"schemaVersion\":3,\"valid\":0,\"diagnostic\":true,", 47), 0,
                   "diagnostic envelope");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"failureClass\":\"self-identity\""),
                "diagnostic names the class");
    TEST_ASSERT(buf_contains(s_json_buf, s_json_len, "\"handoff\":{"), "diagnostic keeps the handoff");
    TEST_ASSERT(!buf_contains(s_json_buf, s_json_len, "\"quote\""), "diagnostic emits no quote");
    TEST_ASSERT(!buf_contains(s_json_buf, s_json_len, "\"ak\""), "diagnostic emits no AK");
    TEST_ASSERT(!buf_contains(s_json_buf, s_json_len, "\"sha256\""), "diagnostic emits no identity digest");
    TEST_ASSERT_EQ(tpm_attest_report_build_from(0, 0, 0, &s_report), TPM_ATTEST_SELF_CORRUPT,
                   "NULL descriptor -> SELF_CORRUPT");
}

/* The snapshot buffer must hold the largest report the serializer accepts:
 * every length at its cap and every text field escaped at six bytes a byte. */
static uint8_t s_big[ATTEST_REPORT_JSON_MAX];
static char    s_label[65];
static char    s_scope[513];

static void test_attest_report_size_bound(void)
{
    uint8_t nonce[TPM_QUOTE_NONCE_MIN];
    struct attest_json_mem m;
    uint32_t i;
    int rc;

    memset(nonce, 0x5Au, sizeof nonce);
    rc = (int)tpm_attest_report_build(nonce, sizeof nonce, &s_report);
    TEST_ASSERT_EQ((uint32_t)rc, (uint32_t)TPM_ATTEST_OK, "base report built");
    TEST_ASSERT_EQ((uint32_t)s_report.manifest_valid, 1u, "full (non-diagnostic) rendering");

    s_report.pcr_count = ATTEST_REPORT_PCR_MAX;
    for (i = 0; i < ATTEST_REPORT_PCR_MAX; i++) {
        s_report.pcrs[i].digest_len = (uint16_t)sizeof s_report.pcrs[i].digest;
        memset(s_report.pcrs[i].digest, 0xFF, sizeof s_report.pcrs[i].digest);
    }
    s_report.nonce_len          = (uint16_t)sizeof s_report.nonce;
    s_report.quote.nonce_len    = (uint16_t)sizeof s_report.quote.nonce;
    s_report.quote_sig_len      = (uint32_t)sizeof s_report.quote_sig;
    s_report.quote.attest_raw_len = (uint16_t)sizeof s_report.quote.attest_raw;
    s_report.quote.pcr_digest_len = (uint16_t)sizeof s_report.quote.pcr_digest;
    s_report.ak_pub_len         = (uint16_t)sizeof s_report.ak_pub;
    s_report.ek_cert_len        = (uint16_t)sizeof s_report.ek_cert;
    s_report.quote.clock        = ~0ull;
    s_report.quote.firmware_version = ~0ull;
    s_report.quote.reset_count  = ~0u;
    s_report.quote.restart_count = ~0u;
    memset(s_label, 0x01, sizeof s_label - 1u);
    memset(s_scope, 0x01, sizeof s_scope - 1u);
    s_label[sizeof s_label - 1u] = 0;
    s_scope[sizeof s_scope - 1u] = 0;
    s_report.integrity_label = s_label;
    s_report.integrity_scope = s_scope;
    memset(s_report.loader_identity.build_label, 0x01,
           sizeof s_report.loader_identity.build_label);

    m.buf = s_big;
    m.cap = (uint32_t)sizeof s_big;
    m.len = 0u;
    rc = tpm_attest_report_to_json(&s_report, tpm_attest_json_mem_write, &m);
    TEST_ASSERT_EQ(rc, 0, "worst-case report fits ATTEST_REPORT_JSON_MAX");
    TEST_ASSERT(m.len + 1024u <= ATTEST_REPORT_JSON_MAX, "with at least 1 KiB headroom");
    klog(LOG_INFO, "test", "attest worst-case report: %u of %u bytes",
         (uint64_t)m.len, (uint64_t)ATTEST_REPORT_JSON_MAX);

    /* The memory sink refuses what does not fit, and an out-of-order write. */
    m.cap = 16u;
    m.len = 0u;
    TEST_ASSERT(tpm_attest_report_to_json(&s_report, tpm_attest_json_mem_write, &m) != 0,
                "a short sink fails the serializer");
    m.cap = (uint32_t)sizeof s_big;
    m.len = 4u;
    TEST_ASSERT(tpm_attest_json_mem_write(&m, 0u, nonce, 1u) != 0, "out-of-order write refused");
    TEST_ASSERT(tpm_attest_json_mem_write(&m, 4u, nonce, 1u) == 0, "in-order write accepted");
    TEST_ASSERT(tpm_attest_json_mem_write((void *)0, 0u, nonce, 1u) != 0, "NULL sink refused");
}

/* NtQuerySystemInformation(SystemAttestationReportInformation): refusal first,
 * then not-found, then the size query, then the bytes. */
static void test_attest_query_class(void)
{
    static const uint8_t doc[] = "{\"schemaVersion\":3}\n";
    const uint32_t dl = (uint32_t)(sizeof doc - 1u);
    uint8_t  out[64];
    uint32_t rl;
    NTSTATUS st;

    rl = 0xDEADu;
    memset(out, 0xAA, sizeof out);
    st = nt_query_attestation_information_from(0, doc, dl, out, sizeof out, &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_ACCESS_DENIED, "non-administrator refused");
    TEST_ASSERT_EQ(rl, 0xDEADu, "a refusal reveals no size");
    TEST_ASSERT_EQ((uint32_t)out[0], 0xAAu, "a refusal writes nothing");

    st = nt_query_attestation_information_from(1, (const uint8_t *)0, 0u, out, sizeof out, &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_NOT_FOUND, "no published report -> NOT_FOUND");
    TEST_ASSERT_EQ(rl, 0xDEADu, "NOT_FOUND reports no size");

    st = nt_query_attestation_information_from(1, doc, dl, (void *)0, 0u, &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INFO_LENGTH_MISMATCH, "size query");
    TEST_ASSERT_EQ(rl, dl, "size query returns the document length");

    rl = 0u;
    st = nt_query_attestation_information_from(1, doc, dl, out, dl - 1u, &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INFO_LENGTH_MISMATCH, "one byte short");
    TEST_ASSERT_EQ(rl, dl, "short buffer still learns the length");
    TEST_ASSERT_EQ((uint32_t)out[0], 0xAAu, "short buffer is not written");

    rl = 0u;
    st = nt_query_attestation_information_from(1, doc, dl, out, sizeof out, &rl);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "exact document returned");
    TEST_ASSERT_EQ(rl, dl, "return length is the document length");
    TEST_ASSERT_EQ(memcmp(out, doc, dl), 0, "bytes match");
    TEST_ASSERT_EQ((uint32_t)out[dl], 0xAAu, "nothing past the document");

    st = nt_query_attestation_information_from(1, doc, dl, out, sizeof out, (uint32_t *)0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "ReturnLength is optional");
}

/* The class is wired into the real dispatcher and gated on the CALLER: a
 * user-mode standard user is refused; kernel mode reaches the snapshot. */
static void test_attest_query_dispatch(void)
{
    struct task  *cur = task_current();
    ACCESS_TOKEN *usr = SeCreateUserToken(SeBuiltinUsersSid, 0);
    void         *saved;
    const uint8_t *snap;
    uint32_t snap_len = 0u, rl = 0xDEADu;
    NTSTATUS st;

    TEST_ASSERT(cur && usr, "standard-user token created");
    if (!cur || !usr) {
        if (usr) ObDereferenceObject(usr);
        return;
    }
    saved = cur->token;
    cur->token = usr;
    ssdt_set_previous_mode(SSDT_USER_MODE);
    st = ssdt_dispatch(SSDT_NtQuerySystemInformation, SystemAttestationReportInformation,
                       0, 0, (uint64_t)(uintptr_t)&rl, 0, 0);
    ssdt_set_previous_mode(SSDT_KERNEL_MODE);
    cur->token = saved;
    ObDereferenceObject(usr);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_ACCESS_DENIED,
                   "user-mode standard user refused through the dispatcher");
    TEST_ASSERT_EQ(rl, 0xDEADu, "dispatcher refusal reveals no size");

    st = ssdt_dispatch(SSDT_NtQuerySystemInformation, SystemAttestationReportInformation,
                       0, 0, (uint64_t)(uintptr_t)&rl, 0, 0);
    if (tpm_attest_report_snapshot(&snap, &snap_len)) {
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INFO_LENGTH_MISMATCH,
                       "kernel-mode size query of the published report");
        TEST_ASSERT_EQ(rl, snap_len, "size is the published length");
    } else {
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_NOT_FOUND,
                       "no report published yet -> NOT_FOUND");
    }
}

void test_register_tpm_attest_report(void)
{
    test_suite_register_cat("tpm: attest handoff snapshot capture",
                            test_attest_handoff_capture, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest report build",
                            test_attest_report_build, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest report json export",
                            test_attest_report_json, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest manifest PCR binding",
                            test_attest_manifest_binding, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest report scoped verdict",
                            test_attest_report_scoped_verdict, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest report size bound",
                            test_attest_report_size_bound, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest query class marshaller",
                            test_attest_query_class, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest query class dispatch",
                            test_attest_query_dispatch, TEST_CAT_SECURITY);
}
