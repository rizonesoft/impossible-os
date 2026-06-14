/* Unit tests for the TPM-rooted boot attestation report export: the immutable
 * bootloader-to-kernel handoff snapshot and the report builder. */

#include "kernel/test/test.h"
#include "kernel/tpm_attest_report.h"
#include "kernel/tpm.h"                   /* TPM_ALG_SHA256 */
#include "kernel/boot_info.h"
#include "kernel/boot_proto_descriptor.h" /* kernel_boot_proto identity check */
#include "libc/string.h"

extern const struct boot_proto_descriptor kernel_boot_proto;

/* Static fixtures off the test stack: boot_info is large and the report is ~2.3 KB. */
static struct boot_info s_fixture_bi;
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

    memset(&s_fixture_bi, 0, sizeof s_fixture_bi);
    s_fixture_bi.caps_required       = 0x1234u;
    s_fixture_bi.caps_present        = 0x5678u;
    s_fixture_bi.caps_degraded       = 0x9ABCu;
    s_fixture_bi.boot_path           = 3u;
    s_fixture_bi.boot_reason         = 7u;
    s_fixture_bi.boot_source_flags   = 0xF0u;
    s_fixture_bi.boot_fallback_depth = 2u;

    tpm_attest_handoff_capture(&s_fixture_bi, &h);
    TEST_ASSERT_EQ((uint32_t)h.valid, 1u, "capture marks the snapshot valid");
    TEST_ASSERT_EQ((uint32_t)h.caps_required, 0x1234u, "caps_required copied");
    TEST_ASSERT_EQ((uint32_t)h.caps_present, 0x5678u, "caps_present copied");
    TEST_ASSERT_EQ((uint32_t)h.caps_degraded, 0x9ABCu, "caps_degraded copied");
    TEST_ASSERT_EQ(h.boot_path, 3u, "boot_path copied");
    TEST_ASSERT_EQ(h.boot_reason, 7u, "boot_reason copied");
    TEST_ASSERT_EQ(h.boot_source_flags, 0xF0u, "boot_source_flags copied");
    TEST_ASSERT_EQ(h.boot_fallback_depth, 2u, "boot_fallback_depth copied");

    /* The load-bearing property: the snapshot is a COPY. A later kernel refinement
     * of the live boot_info (e.g. boot_caps_mark_present) must NOT change what the
     * attestation report attributes to the loader handoff. */
    s_fixture_bi.caps_present = 0xFFFFu;
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
        memset(&s_fixture_bi, 0, sizeof s_fixture_bi);
        s_fixture_bi.caps_present = ~boot_caps;        /* a value the boot snapshot is not */
        tpm_attest_handoff_snapshot_init(&s_fixture_bi);   /* latched -> no-op */
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

    /* Trust model: AK<->EK binding is never auto-trusted by this build. */
    TEST_ASSERT_EQ((uint32_t)s_report.ak_credential_status, (uint32_t)ATTEST_TRUST_UNVERIFIED,
                   "AK credential binding stays UNVERIFIED");
    TEST_ASSERT_EQ((uint32_t)s_report.qualified_signer_status, (uint32_t)ATTEST_TRUST_UNVERIFIED,
                   "qualified-signer binding stays UNVERIFIED");
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
    TEST_ASSERT_EQ(memcmp(s_json_buf, "{\"schemaVersion\":2,", 19), 0, "json header prefix");
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

void test_register_tpm_attest_report(void)
{
    test_suite_register_cat("tpm: attest handoff snapshot capture",
                            test_attest_handoff_capture, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest report build",
                            test_attest_report_build, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: attest report json export",
                            test_attest_report_json, TEST_CAT_SECURITY);
}
