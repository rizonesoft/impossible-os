/* test_tpm_replay.c -- unit tests for tpm_pcr_extend (PCR replay primitive).
 * Self-consistency: tpm_pcr_extend(alg, pcr, digest) must equal the direct
 * H_alg(pcr || digest), proving the concat + dispatch are correct for every
 * bank. No live boot infrastructure (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "PCR Replay
 * Engine".
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/tpm_replay.h"
#include "kernel/tpm.h"
#include "kernel/klog.h"   /* klog_entry_t.message cap -- the scope lines must fit */
#include "kernel/crypto/sha1.h"
#include "kernel/crypto/sha256.h"
#include "kernel/crypto/sha384.h"
#include "libc/string.h"

/* ---- fixture event-log builder (TPM 2.0 crypto-agile) ---- */
static uint8_t s_log[256];
static uint32_t s_log_len;

static void log_put32(uint32_t off, uint32_t v)
{
    s_log[off] = (uint8_t)v;        s_log[off + 1u] = (uint8_t)(v >> 8);
    s_log[off + 2u] = (uint8_t)(v >> 16); s_log[off + 3u] = (uint8_t)(v >> 24);
}
static void log_put16(uint32_t off, uint16_t v)
{
    s_log[off] = (uint8_t)v; s_log[off + 1u] = (uint8_t)(v >> 8);
}

/* Build: a leading TCG_PCR_EVENT (PCR 0, SHA-1, no data) + two EVENT2 records on
 * PCR 4 each carrying one SHA-256 digest (d1 then d2). */
static void build_fixture(const uint8_t *d1, const uint8_t *d2)
{
    uint32_t o;
    memset(s_log, 0, sizeof(s_log));
    /* TCG_PCR_EVENT: pcr(4)=0 type(4)=3 digest[20]=0 dsize(4)=0 -> 32 bytes. */
    log_put32(0, 0u); log_put32(4, 3u); log_put32(28, 0u);
    o = 32u;
    /* EVENT2 #1 @32: pcr(4)=4 type(4) count(4)=1 alg(2)=SHA256 digest[32] dsize(4)=0. */
    log_put32(o, 4u); log_put32(o + 4u, 0x80000003u); log_put32(o + 8u, 1u);
    log_put16(o + 12u, (uint16_t)TPM_ALG_SHA256);
    for (uint32_t i = 0; i < 32u; i++) s_log[o + 14u + i] = d1[i];
    log_put32(o + 46u, 0u);
    o += 50u;
    /* EVENT2 #2 @82: same shape, digest d2. */
    log_put32(o, 4u); log_put32(o + 4u, 0x80000003u); log_put32(o + 8u, 1u);
    log_put16(o + 12u, (uint16_t)TPM_ALG_SHA256);
    for (uint32_t i = 0; i < 32u; i++) s_log[o + 14u + i] = d2[i];
    log_put32(o + 46u, 0u);
    o += 50u;
    s_log_len = o;
}

/* Extend a zero PCR with `digest` in `alg`, and assert it equals the directly
 * computed H_alg(zero_pcr || digest). `dl` is the bank digest length. */
static void check_bank(uint16_t alg, uint32_t dl, uint8_t fill, const char *name)
{
    uint8_t pcr[64];
    uint8_t digest[64];
    uint8_t input[128];
    uint8_t expect[64];
    uint32_t i;

    for (i = 0; i < dl; i++) { pcr[i] = 0u; digest[i] = fill; }
    /* expected = H(zero_pcr || digest) */
    for (i = 0; i < dl; i++) { input[i] = 0u; input[dl + i] = fill; }
    if (alg == TPM_ALG_SHA1)        sha1(input, dl * 2u, expect);
    else if (alg == TPM_ALG_SHA256) sha256(input, dl * 2u, expect);
    else                            sha384(input, dl * 2u, expect);

    TEST_ASSERT_EQ(tpm_pcr_extend(alg, pcr, dl, digest, dl), TPM_REPLAY_OK,
                   "extend returns OK");
    TEST_ASSERT(memcmp(pcr, expect, dl) == 0, name);
}

static void test_pcr_extend_banks(void)
{
    check_bank(TPM_ALG_SHA1,   20u, 0xA1u, "SHA-1 extend == H(pcr||digest)");
    check_bank(TPM_ALG_SHA256, 32u, 0x5Au, "SHA-256 extend == H(pcr||digest)");
    check_bank(TPM_ALG_SHA384, 48u, 0x84u, "SHA-384 extend == H(pcr||digest)");
}

static void test_pcr_extend_chain(void)
{
    /* Two extends in sequence: PCR1 = H(0 || d), PCR2 = H(PCR1 || d). Verify the
     * second equals a freshly computed H(PCR1 || d), proving in-place update. */
    uint8_t pcr[32], d[32], snap[32], input[64], expect[32];
    uint32_t i;
    for (i = 0; i < 32u; i++) { pcr[i] = 0u; d[i] = (uint8_t)(i + 1u); }

    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, d, 32u), TPM_REPLAY_OK, "extend 1");
    for (i = 0; i < 32u; i++) snap[i] = pcr[i];      /* PCR after first extend */
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, d, 32u), TPM_REPLAY_OK, "extend 2");
    for (i = 0; i < 32u; i++) { input[i] = snap[i]; input[32 + i] = d[i]; }
    sha256(input, 64u, expect);
    TEST_ASSERT(memcmp(pcr, expect, 32u) == 0, "chained extend == H(prev||digest)");
}

static void test_pcr_extend_badargs(void)
{
    uint8_t pcr[32] = {0}, d[32] = {0};
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, (uint8_t *)0, 32u, d, 32u),
                   TPM_REPLAY_BADARG, "NULL pcr rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, (const uint8_t *)0, 32u),
                   TPM_REPLAY_BADARG, "NULL digest rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 20u, d, 32u),
                   TPM_REPLAY_BADARG, "wrong pcr_len for bank rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(TPM_ALG_SHA256, pcr, 32u, d, 20u),
                   TPM_REPLAY_BADARG, "wrong digest_len for bank rejected");
    TEST_ASSERT_EQ(tpm_pcr_extend(0x0099u, pcr, 32u, d, 32u),
                   TPM_REPLAY_BADARG, "unsupported bank alg rejected");
}

static void test_replay_known_vector(void)
{
    uint8_t d1[32], d2[32], input[64], pcr1[32], expect[32], out[32];
    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0, i;
    tpm_evlog_status_t st;

    for (i = 0; i < 32u; i++) { d1[i] = 0x11u; d2[i] = 0x22u; }
    build_fixture(d1, d2);

    /* Expected PCR 4 (SHA-256) computed INDEPENDENTLY of the replay path:
     * H(H(zero||d1)||d2). */
    for (i = 0; i < 32u; i++) input[i] = 0u;
    for (i = 0; i < 32u; i++) input[32u + i] = d1[i];
    sha256(input, 64u, pcr1);
    for (i = 0; i < 32u; i++) input[i] = pcr1[i];
    for (i = 0; i < 32u; i++) input[32u + i] = d2[i];
    sha256(input, 64u, expect);

    st = tpm_evlog_parse(s_log, s_log_len, 2, ev, 8, &count, &overflow, &fail);
    TEST_ASSERT_EQ(st, TPM_EVLOG_OK, "fixture log parses clean");
    TEST_ASSERT_EQ(count, 3u, "3 events (spec-ID + 2 EVENT2)");

    TEST_ASSERT_EQ(tpm_replay_pcr_from(s_log, s_log_len, ev, count, TPM_ALG_SHA256, 4u,
                                       out, 32u), TPM_REPLAY_OK, "replay PCR4 SHA-256 OK");
    TEST_ASSERT(memcmp(out, expect, 32u) == 0, "replayed PCR4 == H(H(0||d1)||d2)");

    /* PCR 4 has no SHA-384 digests in the log -> replays to the zero PCR. */
    {
        uint8_t out384[48], zero384[48];
        for (i = 0; i < 48u; i++) zero384[i] = 0u;
        TEST_ASSERT_EQ(tpm_replay_pcr_from(s_log, s_log_len, ev, count, TPM_ALG_SHA384, 4u,
                                           out384, 48u), TPM_REPLAY_OK, "replay PCR4 SHA-384 OK");
        TEST_ASSERT(memcmp(out384, zero384, 48u) == 0, "absent bank -> zero PCR");
    }
    /* PCR 0 has only the SHA-1 spec-ID event, no SHA-256 -> SHA-256 PCR0 is zero. */
    {
        uint8_t out0[32], zero0[32];
        for (i = 0; i < 32u; i++) zero0[i] = 0u;
        TEST_ASSERT_EQ(tpm_replay_pcr_from(s_log, s_log_len, ev, count, TPM_ALG_SHA256, 0u,
                                           out0, 32u), TPM_REPLAY_OK, "replay PCR0 SHA-256 OK");
        TEST_ASSERT(memcmp(out0, zero0, 32u) == 0, "PCR0 has no SHA-256 events -> zero");
    }
    /* The leading PCR0 event is EV_NO_ACTION (type 3, the spec-ID event); it must
     * NOT extend even the SHA-1 bank, so SHA-1 PCR0 stays the zero reset value. */
    {
        uint8_t out0[20], zero0[20];
        for (i = 0; i < 20u; i++) zero0[i] = 0u;
        TEST_ASSERT_EQ(tpm_replay_pcr_from(s_log, s_log_len, ev, count, TPM_ALG_SHA1, 0u,
                                           out0, 20u), TPM_REPLAY_OK, "replay PCR0 SHA-1 OK");
        TEST_ASSERT(memcmp(out0, zero0, 20u) == 0, "EV_NO_ACTION not extended -> SHA-1 PCR0 zero");
    }
}

/* Build a TPM 1.2 (legacy) log: two TCG_PCR_EVENT entries on PCR 4, each a raw
 * 20-byte SHA-1 digest (NO {alg} prefix). */
static void build_legacy_fixture(const uint8_t *d1, const uint8_t *d2)
{
    uint32_t o;
    memset(s_log, 0, sizeof(s_log));
    /* Event 0 @0: pcr(4)=4 type(4) digest[20]=d1 dsize(4)=0 -> 32 bytes. */
    log_put32(0, 4u); log_put32(4, 8u);
    for (uint32_t i = 0; i < 20u; i++) s_log[8u + i] = d1[i];
    log_put32(28, 0u);
    o = 32u;
    /* Event 1 @32: same shape, d2. */
    log_put32(o, 4u); log_put32(o + 4u, 9u);
    for (uint32_t i = 0; i < 20u; i++) s_log[o + 8u + i] = d2[i];
    log_put32(o + 28u, 0u);
    s_log_len = o + 32u;
}

static void test_replay_legacy_sha1(void)
{
    /* Legacy SHA-1 digests that do NOT start with 0x0004 -- the pre-fix extractor
     * would mis-read the leading bytes as an alg id and skip these events,
     * underextending the SHA-1 PCR. */
    uint8_t d1[20], d2[20], input[40], pcr1[20], expect[20], out[20];
    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0, i;

    for (i = 0; i < 20u; i++) { d1[i] = 0xAAu; d2[i] = 0xBBu; }
    build_legacy_fixture(d1, d2);

    /* Independent expected: H_sha1(H_sha1(zero||d1)||d2). */
    for (i = 0; i < 20u; i++) input[i] = 0u;
    for (i = 0; i < 20u; i++) input[20u + i] = d1[i];
    sha1(input, 40u, pcr1);
    for (i = 0; i < 20u; i++) input[i] = pcr1[i];
    for (i = 0; i < 20u; i++) input[20u + i] = d2[i];
    sha1(input, 40u, expect);

    TEST_ASSERT_EQ(tpm_evlog_parse(s_log, s_log_len, 1, ev, 8, &count, &overflow, &fail),
                   TPM_EVLOG_OK, "legacy log parses clean");
    TEST_ASSERT_EQ(count, 2u, "two legacy events");
    TEST_ASSERT_EQ(ev[0].legacy, 1u, "event 0 tagged legacy");
    TEST_ASSERT_EQ(tpm_replay_pcr_from(s_log, s_log_len, ev, count, TPM_ALG_SHA1, 4u,
                                       out, 20u), TPM_REPLAY_OK, "legacy SHA-1 replay OK");
    TEST_ASSERT(memcmp(out, expect, 20u) == 0,
                "legacy SHA-1 replay == H(H(0||d1)||d2) (not underextended)");
}

static void test_replay_bank_digest_extract(void)
{
    uint8_t d1[32], d2[32];
    struct tpm_event ev[8];
    uint32_t count = 0, overflow = 0, fail = 0, i;
    const uint8_t *dig = (const uint8_t *)0;

    for (i = 0; i < 32u; i++) { d1[i] = 0xABu; d2[i] = 0xCDu; }
    build_fixture(d1, d2);
    tpm_evlog_parse(s_log, s_log_len, 2, ev, 8, &count, &overflow, &fail);

    /* ev[1] is EVENT2 #1: its SHA-256 digest must be extracted == d1. */
    TEST_ASSERT_EQ(tpm_event_bank_digest(s_log, s_log_len, &ev[1], TPM_ALG_SHA256, &dig), 32u,
                   "SHA-256 digest extracted, len 32");
    TEST_ASSERT(dig && memcmp(dig, d1, 32u) == 0, "extracted digest == d1");
    /* The event has no SHA-1 digest in its list -> not found. */
    TEST_ASSERT_EQ(tpm_event_bank_digest(s_log, s_log_len, &ev[1], TPM_ALG_SHA1, &dig), 0u,
                   "absent bank -> 0");
}

static void test_replay_verify_report(void)
{
    /* Tamper case: PCR 0 matches, PCR 4 + 7 mismatch -> TAMPER, first = 4. */
    struct tpm_replay_report r = { TPM_ALG_SHA256, TPM_REPLAY_VERIFIED, 0u, 0u, -1 };
    tpm_replay_report_pcr(&r, 0u, 1);
    tpm_replay_report_pcr(&r, 4u, 0);
    tpm_replay_report_pcr(&r, 7u, 0);
    TEST_ASSERT_EQ(r.pcr_checked, 3u, "3 PCRs checked");
    TEST_ASSERT_EQ(r.mismatch_count, 2u, "2 mismatches");
    TEST_ASSERT_EQ(r.first_mismatch_pcr, 4, "first mismatch = lowest mismatching PCR (4)");
    TEST_ASSERT_EQ(r.verdict, TPM_REPLAY_TAMPER, "any replay!=hardware -> TAMPER");

    /* All-match case -> VERIFIED, no mismatch. */
    {
        struct tpm_replay_report ok = { TPM_ALG_SHA256, TPM_REPLAY_VERIFIED, 0u, 0u, -1 };
        tpm_replay_report_pcr(&ok, 0u, 1);
        tpm_replay_report_pcr(&ok, 7u, 1);
        TEST_ASSERT_EQ(ok.verdict, TPM_REPLAY_VERIFIED, "all match -> VERIFIED");
        TEST_ASSERT_EQ(ok.first_mismatch_pcr, -1, "no first mismatch");
        TEST_ASSERT_EQ(ok.mismatch_count, 0u, "0 mismatches");
    }
}

static void test_replay_finalize(void)
{
    struct tpm_replay_report r;

    /* Degraded/absent log -> UNVERIFIABLE, never tamper (even if a comparison had
     * flagged a mismatch from the all-zero replay). */
    r = (struct tpm_replay_report){ TPM_ALG_SHA256, TPM_REPLAY_TAMPER, 3u, 3u, 0 };
    tpm_replay_finalize(&r, 0 /*log_clean*/, 0);
    TEST_ASSERT_EQ(r.verdict, TPM_REPLAY_UNVERIFIABLE, "degraded log -> UNVERIFIABLE not tamper");

    /* Clean log, complete coverage, no mismatch -> VERIFIED. */
    r = (struct tpm_replay_report){ TPM_ALG_SHA256, TPM_REPLAY_VERIFIED, 9u, 0u, -1 };
    tpm_replay_finalize(&r, 1, 1 /*complete*/);
    TEST_ASSERT_EQ(r.verdict, TPM_REPLAY_VERIFIED, "clean + complete + match -> VERIFIED");

    /* Clean log, INCOMPLETE coverage, no mismatch -> UNVERIFIABLE (no false pass). */
    r = (struct tpm_replay_report){ TPM_ALG_SHA256, TPM_REPLAY_VERIFIED, 1u, 0u, -1 };
    tpm_replay_finalize(&r, 1, 0 /*incomplete*/);
    TEST_ASSERT_EQ(r.verdict, TPM_REPLAY_UNVERIFIABLE, "incomplete coverage -> UNVERIFIABLE");

    /* A confirmed mismatch is actionable and preserved even if coverage was
     * incomplete. */
    r = (struct tpm_replay_report){ TPM_ALG_SHA256, TPM_REPLAY_TAMPER, 2u, 1u, 4 };
    tpm_replay_finalize(&r, 1, 0 /*incomplete*/);
    TEST_ASSERT_EQ(r.verdict, TPM_REPLAY_TAMPER, "confirmed mismatch -> TAMPER kept");
}

static void test_integrity_status_label(void)
{
    struct boot_integrity_report r;
    memset(&r, 0, sizeof(r));

    /* A zero-initialized / not-yet-evaluated report (overall_status ==
     * BOOT_INTEGRITY_UNKNOWN) must read "unknown", NOT "no-TPM" -- version 0
     * alone cannot distinguish "not checked" from "no hardware present". */
    TEST_ASSERT(memcmp(tpm_integrity_status_label(&r), "unknown", 8) == 0,
                "zeroed/unchecked report -> unknown");

    /* An initialized report that probed and found no TPM. */
    r.overall_status = BOOT_INTEGRITY_NO_TPM;
    TEST_ASSERT(memcmp(tpm_integrity_status_label(&r), "no-TPM", 7) == 0, "no TPM -> no-TPM");

    /* TAMPER escalates overall_status to MISMATCH; the label must still report
     * tamper, NOT a generic baseline mismatch (tamper checked first). */
    r.tpm_version = 2u;
    r.replay_verdict = (uint8_t)TPM_REPLAY_TAMPER;
    r.overall_status = BOOT_INTEGRITY_MISMATCH;
    TEST_ASSERT(memcmp(tpm_integrity_status_label(&r), "event-log-tamper", 17) == 0,
                "replay tamper -> event-log-tamper (not baseline-mismatch)");

    /* A real baseline mismatch WITHOUT replay tamper. */
    r.replay_verdict = (uint8_t)TPM_REPLAY_VERIFIED;
    r.overall_status = BOOT_INTEGRITY_MISMATCH;
    TEST_ASSERT(memcmp(tpm_integrity_status_label(&r), "baseline-mismatch", 18) == 0,
                "mismatch w/o tamper -> baseline-mismatch");

    /* "baseline-verified", never a bare "verified". The subject is the enrolled
     * baseline; nothing in it measures the kernel image, and a bare word is
     * exactly what a reader upgrades into a claim that it does. */
    r.overall_status = BOOT_INTEGRITY_VERIFIED;
    TEST_ASSERT(memcmp(tpm_integrity_status_label(&r), "baseline-verified", 18) == 0,
                "verified -> baseline-verified (scoped, never a bare verified)");

    TEST_ASSERT(memcmp(tpm_integrity_status_label((const struct boot_integrity_report *)0),
                       "unknown", 8) == 0, "NULL report -> unknown");
}

/* The SCOPE sentence beside the label. The label alone cannot carry scope, and
 * the scope is the part a reader gets wrong -- so each status gets its own
 * sentence and the VERIFIED one must say out loud that the kernel image was not
 * measured. */
static void test_integrity_status_scope(void)
{
    struct boot_integrity_report r;
    const char *scopes[6];
    uint32_t i, j;
    memset(&r, 0, sizeof(r));

    TEST_ASSERT(tpm_integrity_status_scope((const struct boot_integrity_report *)0)
                != (const char *)0, "NULL report yields a scope sentence, not NULL");
    TEST_ASSERT(strcmp(tpm_integrity_status_scope(&r),
                       tpm_integrity_status_scope(
                           (const struct boot_integrity_report *)0)) == 0,
                "an unevaluated report reads the same as no report at all");

    /* The VERIFIED scope must name the kernel IMAGE exclusion explicitly. A
     * scope sentence that merely lists what IS covered would let the reader
     * keep the assumption this section exists to remove. */
    r.tpm_version = 2u;
    r.overall_status = BOOT_INTEGRITY_VERIFIED;
    TEST_ASSERT(strstr(tpm_integrity_status_scope(&r), "IMAGE was not measured")
                != (char *)0,
                "the verified scope states that the kernel image was not measured");

    /* The MISMATCH scope must NOT promise a differing field. That status is
     * also published for a corrupt or unauthenticated stored baseline, where no
     * comparison ran and there is no field cause to look up, so text sending
     * every reader after one would send half of them after evidence that does
     * not exist. */
    r.overall_status = BOOT_INTEGRITY_MISMATCH;
    TEST_ASSERT(strstr(tpm_integrity_status_scope(&r), "field") == (char *)0,
                "the mismatch scope promises no differing field");
    TEST_ASSERT(strstr(tpm_integrity_status_scope(&r), "authenticity") != (char *)0,
                "the mismatch scope admits the corrupt/unauthenticated case");
    /* The THIRD kind is the one that must never be missed. A SELF_CORRUPT
     * verdict also publishes MISMATCH, and its recovery path is not the stored
     * baseline's: the party that would produce a new golden is itself the thing
     * that failed validation. A scope covering only the first two kinds is what
     * sends a reader down the wrong repair. */
    TEST_ASSERT(strstr(tpm_integrity_status_scope(&r), "kernel's own identity")
                != (char *)0,
                "the mismatch scope covers a corrupt kernel identity as well");

    /* The NO_TPM scope must not claim nothing was measured: the event count is
     * filled before the no-TPM branch is taken, so a NO_TPM report can carry a
     * nonzero one and that claim would contradict the report in hand. */
    r.overall_status = BOOT_INTEGRITY_NO_TPM;
    r.event_count = 7u;
    TEST_ASSERT(strstr(tpm_integrity_status_scope(&r), "nothing was measured")
                == (char *)0,
                "a NO_TPM report carrying events is not described as unmeasured");
    TEST_ASSERT(strstr(tpm_integrity_status_scope(&r), "could not be compared")
                != (char *)0,
                "the no-TPM scope says the baseline could not be compared");
    r.event_count = 0u;

    /* Tamper is checked before the baseline status in the scope, exactly as in
     * the label, so the two can never describe different statuses. */
    r.overall_status = BOOT_INTEGRITY_MISMATCH;
    r.replay_verdict = (uint8_t)TPM_REPLAY_TAMPER;
    TEST_ASSERT(strstr(tpm_integrity_status_scope(&r), "event log") != (char *)0,
                "a tamper verdict scopes to the event log, not to the baseline");
    r.replay_verdict = (uint8_t)TPM_REPLAY_VERIFIED;

    /* Every status gets its OWN sentence: identical text would make the scope
     * decorative. UNKNOWN and the default share one on purpose and are counted
     * once. */
    {
        const uint8_t statuses[6] = {
            BOOT_INTEGRITY_UNKNOWN, BOOT_INTEGRITY_VERIFIED, BOOT_INTEGRITY_MISMATCH,
            BOOT_INTEGRITY_NO_TPM, BOOT_INTEGRITY_NO_BASELINE, BOOT_INTEGRITY_NO_CRYPTO
        };
        for (i = 0; i < 6u; i++) {
            r.overall_status = statuses[i];
            scopes[i] = tpm_integrity_status_scope(&r);
            TEST_ASSERT(scopes[i] != (const char *)0, "every status has a scope sentence");
        }
        for (i = 0; i < 6u; i++)
            for (j = i + 1u; j < 6u; j++)
                TEST_ASSERT(strcmp(scopes[i], scopes[j]) != 0,
                            "no two statuses share a scope sentence");

        /* The boot log renders "Boot integrity status: <label> (<scope>)" into
         * a klog entry that TRUNCATES at message[256]. The longest pair fits
         * today with little room, and a truncated scope sentence would fail
         * silently and quietly reintroduce the overclaim these sentences exist
         * to remove -- a half-printed "or this kernel's own identity is" is
         * worse than no scope at all. Assert the whole rendered shape. */
        for (i = 0; i < 6u; i++) {
            uint32_t rendered;
            /* Re-select the status so the LABEL measured is the one that
             * actually accompanies scopes[i]; leaving r on the loop's last
             * value would size every line against one label. */
            r.overall_status = statuses[i];
            rendered =
                (uint32_t)strlen("Boot integrity status: ") +
                (uint32_t)strlen(tpm_integrity_status_label(&r)) +
                (uint32_t)strlen(" ()") + (uint32_t)strlen(scopes[i]);
            TEST_ASSERT(rendered < (uint32_t)sizeof(((klog_entry_t *)0)->message),
                        "every rendered status+scope line fits the klog entry");
        }
    }
}

void test_register_tpm_replay(void)
{
    test_suite_register_cat("tpm: PCR extend banks", test_pcr_extend_banks, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR extend chain", test_pcr_extend_chain, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR extend bad args", test_pcr_extend_badargs, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR replay known vector", test_replay_known_vector, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: PCR replay legacy SHA-1", test_replay_legacy_sha1, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: replay bank-digest extract", test_replay_bank_digest_extract, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: replay verify report", test_replay_verify_report, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: replay verdict finalize", test_replay_finalize, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: integrity status label", test_integrity_status_label, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: integrity status scope", test_integrity_status_scope, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
