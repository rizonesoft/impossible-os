/* ============================================================================
 * test_tpm_authz.c -- authorized NV record transitions
 *
 * Covers the record format and its transition rules, the five new TPM2 policy
 * command builders, the compiled enrollment manifest, and the fail-closed
 * behaviour of an unprovisioned authority. Pure tests only -- no MMIO, no live
 * boot infrastructure.
 *
 * The two acceptance shapes this section exists for are asserted directly:
 * a lower security version paired with a freshly incremented counter is
 * REFUSED, and an old baseline stamped with the current generation is REFUSED.
 * Every refusal has a passing CONTROL beside it, because a validator that
 * refuses everything would satisfy the refusals alone.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/tpm.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_record.h"
#include "kernel/tpm_authz.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm_budget.h"
#include "kernel/tpm_baseline.h"
#include "kernel/crypto/sha256.h"
#include "libc/string.h"

/* ---- helpers ---- */

static uint32_t az_build_floor(uint8_t *buf, uint32_t cap, uint64_t gen,
                               uint32_t version)
{
    struct tpm_ab_floor_payload p;
    memset(&p, 0, sizeof p);
    p.security_version = version;
    if (tpm_record_build(buf, cap, TPM_RECORD_KIND_AB_FLOOR, gen,
                         (const uint8_t *)&p, (uint32_t)sizeof p) != TPM_RECORD_OK)
        return 0u;
    return cap;
}

static uint32_t az_build_bind(uint8_t *buf, uint32_t cap, uint64_t gen,
                              const uint8_t *blob, uint32_t blob_len)
{
    struct tpm_baseline_bind_payload p;
    memset(&p, 0, sizeof p);
    sha256(blob, blob_len, p.blob_digest);
    p.blob_len = blob_len;
    if (tpm_record_build(buf, cap, TPM_RECORD_KIND_BASELINE, gen,
                         (const uint8_t *)&p, (uint32_t)sizeof p) != TPM_RECORD_OK)
        return 0u;
    return cap;
}

#define AZ_FLOOR_LEN ((uint32_t)TPM_AB_FLOOR_RECORD_LEN)
#define AZ_BIND_LEN  ((uint32_t)TPM_BASELINE_BIND_LEN)

/* ---- record format ---- */

static void test_authz_record_roundtrip(void)
{
    uint8_t rec[AZ_FLOOR_LEN];
    struct tpm_record_view v;
    uint32_t ver = 0;

    TEST_ASSERT_EQ((int)az_build_floor(rec, sizeof rec, 7u, 500u), (int)AZ_FLOOR_LEN,
                   "floor record builds");
    TEST_ASSERT_EQ((int)tpm_record_parse(rec, sizeof rec, TPM_RECORD_KIND_AB_FLOOR,
                                         (uint32_t)sizeof(struct tpm_ab_floor_payload),
                                         &v),
                   (int)TPM_RECORD_OK, "CONTROL: a well-formed record parses");
    TEST_ASSERT_EQ((int)v.generation, 7, "generation survives the round trip");
    TEST_ASSERT_EQ((int)tpm_record_ab_floor_version(&v, &ver), (int)TPM_RECORD_OK,
                   "version extracted");
    TEST_ASSERT_EQ((int)ver, 500, "security version survives the round trip");

    /* The header is a wire contract; its size is what a machine enrolled under
     * this layout will read back. */
    TEST_ASSERT_EQ((int)TPM_RECORD_HDR_LEN, 56, "record header is 56 bytes");
    TEST_ASSERT_EQ((int)AZ_FLOOR_LEN, 96, "floor record is 96 bytes");
    TEST_ASSERT_EQ((int)AZ_BIND_LEN, 104, "bind record is 104 bytes");
    /* EXACTLY fills its index, not merely fits. dataSize is part of the identity
     * contract and is compared for exact equality, so a record smaller than its
     * index is a contract the enrolled index can never satisfy. The old
     * assertion used `<=`, which is the relation that let a 72-vs-96
     * disagreement sit there looking checked. */
    TEST_ASSERT_EQ((int)AZ_FLOOR_LEN, (int)TPM_NV_AB_FLOOR_SIZE,
                   "floor record exactly fills its NV index");
}

static void test_authz_record_digest_covers_everything(void)
{
    uint8_t rec[AZ_FLOOR_LEN], alt[AZ_FLOOR_LEN];
    struct tpm_record_view v;
    uint32_t i;

    (void)az_build_floor(rec, sizeof rec, 3u, 42u);

    /* Every payload byte is covered. */
    memcpy(alt, rec, sizeof rec);
    alt[TPM_RECORD_HDR_LEN] ^= 0x01u;
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, sizeof alt, TPM_RECORD_KIND_AB_FLOOR,
                                         (uint32_t)sizeof(struct tpm_ab_floor_payload), &v),
                   (int)TPM_RECORD_DIGEST_BAD, "a flipped payload byte fails the digest");

    /* And every header byte before the digest field. */
    memcpy(alt, rec, sizeof rec);
    alt[8] ^= 0x01u;   /* generation */
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, sizeof alt, TPM_RECORD_KIND_AB_FLOOR,
                                         (uint32_t)sizeof(struct tpm_ab_floor_payload), &v),
                   (int)TPM_RECORD_DIGEST_BAD, "a flipped generation byte fails the digest");

    /* The digest field itself is not self-authenticating: rewriting it does not
     * make a modified record valid, which is the point of zeroing it inside the
     * covered span rather than skipping it. */
    memcpy(alt, rec, sizeof rec);
    for (i = 0; i < TPM_RECORD_DIGEST; i++)
        alt[24u + i] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, sizeof alt, TPM_RECORD_KIND_AB_FLOOR,
                                         (uint32_t)sizeof(struct tpm_ab_floor_payload), &v),
                   (int)TPM_RECORD_DIGEST_BAD, "a rewritten digest field does not validate");

    /* The digest computation itself is stable and depends on the content. */
    {
        uint8_t d1[TPM_RECORD_DIGEST], d2[TPM_RECORD_DIGEST];
        TEST_ASSERT_EQ((int)tpm_record_digest_compute(rec, sizeof rec, d1),
                       (int)TPM_RECORD_OK, "digest computes");
        TEST_ASSERT_EQ((int)tpm_record_digest_compute(rec, sizeof rec, d2),
                       (int)TPM_RECORD_OK, "digest recomputes");
        TEST_ASSERT_EQ(memcmp(d1, d2, TPM_RECORD_DIGEST), 0, "digest is stable");
        TEST_ASSERT_EQ((int)tpm_record_digest_compute(rec, TPM_RECORD_HDR_LEN - 1u, d1),
                       (int)TPM_RECORD_BADARG, "a buffer below the header is refused");
        TEST_ASSERT_EQ((int)tpm_record_digest_compute(0, sizeof rec, d1),
                       (int)TPM_RECORD_BADARG, "NULL buffer refused");
    }
}

static void test_authz_record_malformed(void)
{
    uint8_t rec[AZ_FLOOR_LEN], alt[AZ_FLOOR_LEN + 8u];
    struct tpm_record_view v;
    const uint32_t plen = (uint32_t)sizeof(struct tpm_ab_floor_payload);

    (void)az_build_floor(rec, sizeof rec, 5u, 9u);

    memcpy(alt, rec, sizeof rec);
    alt[0] ^= 0xFFu;
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED, "bad magic refused");

    memcpy(alt, rec, sizeof rec);
    alt[4] = 0x7Fu;   /* layout */
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED, "unknown layout refused");

    /* Reserved bytes are BOTH hashed and required to be zero. Rebuilding the
     * digest over dirty padding is exactly the shape a signed-but-hostile
     * producer would use, so the zero rule must hold on its own. */
    memcpy(alt, rec, sizeof rec);
    alt[20] = 0x01u;  /* header reserved */
    (void)tpm_record_digest_compute(alt, AZ_FLOOR_LEN, alt + 24u);
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED,
                   "non-zero header padding refused even with a valid digest");

    memcpy(alt, rec, sizeof rec);
    alt[TPM_RECORD_HDR_LEN + 4u] = 0x01u;  /* payload reserved[0] */
    (void)tpm_record_digest_compute(alt, AZ_FLOOR_LEN, alt + 24u);
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED,
                   "non-zero payload padding refused even with a valid digest");

    /* Exact length, not a minimum: trailing bytes would sit inside the digest
     * with no field describing them. */
    memcpy(alt, rec, sizeof rec);
    memset(alt + AZ_FLOOR_LEN, 0, 8u);
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, AZ_FLOOR_LEN + 8u, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED, "an over-long blob is refused");
    TEST_ASSERT_EQ((int)tpm_record_parse(rec, AZ_FLOOR_LEN - 1u, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED, "a short blob is refused");

    /* Generation 0 is the never-written sentinel and can never be stored. */
    TEST_ASSERT_EQ((int)az_build_floor(alt, AZ_FLOOR_LEN, 0u, 1u), 0,
                   "building generation 0 is refused at the builder");

    TEST_ASSERT_EQ((int)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_NONE,
                                         plen, &v),
                   (int)TPM_RECORD_BADARG, "KIND_NONE is not a query");
    TEST_ASSERT_EQ((int)tpm_record_parse(0, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_BADARG, "NULL buffer refused");
}

static void test_authz_record_kind_substitution(void)
{
    uint8_t floor[AZ_FLOOR_LEN], bind[AZ_BIND_LEN];
    static const uint8_t blob[16] = { 1, 2, 3, 4 };
    struct tpm_record_view v;

    (void)az_build_floor(floor, sizeof floor, 2u, 11u);
    (void)az_build_bind(bind, sizeof bind, 2u, blob, (uint32_t)sizeof blob);

    /* Both records carry a perfectly valid digest under the SAME authority, so
     * only the kind check separates one served from the other's index. */
    TEST_ASSERT_EQ((int)tpm_record_parse(bind, AZ_BIND_LEN, TPM_RECORD_KIND_BASELINE,
                                         (uint32_t)sizeof(struct tpm_baseline_bind_payload), &v),
                   (int)TPM_RECORD_OK, "CONTROL: the bind record parses as itself");
    TEST_ASSERT_EQ((int)tpm_record_parse(bind, AZ_BIND_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         (uint32_t)sizeof(struct tpm_baseline_bind_payload), &v),
                   (int)TPM_RECORD_KIND, "a bind record is refused where a floor is expected");
    TEST_ASSERT_EQ((int)tpm_record_parse(floor, AZ_FLOOR_LEN, TPM_RECORD_KIND_BASELINE,
                                         (uint32_t)sizeof(struct tpm_ab_floor_payload), &v),
                   (int)TPM_RECORD_KIND, "a floor record is refused where a bind is expected");
}

/* ---- transitions: the two acceptance refusals ---- */

static void test_authz_transition_rules(void)
{
    uint8_t a[AZ_FLOOR_LEN], b[AZ_FLOOR_LEN];
    struct tpm_record_view va, vb;
    const uint32_t plen = (uint32_t)sizeof(struct tpm_ab_floor_payload);

    (void)az_build_floor(a, sizeof a, 10u, 100u);
    (void)tpm_record_parse(a, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &va);

    /* CONTROL: a correctly authorized advance succeeds. Without this every
     * refusal below would pass against a validator that refuses everything. */
    (void)az_build_floor(b, sizeof b, 11u, 100u);
    (void)tpm_record_parse(b, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &vb);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&va, &vb), (int)TPM_RECORD_OK,
                   "CONTROL: +1 generation at the same version is accepted");

    /* CONTROL: the version may JUMP freely -- a fresh install at a high
     * ordinal, or an upgrade skipping releases, in ONE counter transaction. */
    (void)az_build_floor(b, sizeof b, 11u, 500u);
    (void)tpm_record_parse(b, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &vb);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&va, &vb), (int)TPM_RECORD_OK,
                   "CONTROL: a version jump of 400 needs only one counter step");

    /* THE ACCEPTANCE REFUSAL: a LOWER version paired with a freshly advanced
     * counter. Every cooperative check passes -- the counter moved, the record
     * matches it, nothing is skewed -- and it is still a rollback. */
    (void)az_build_floor(b, sizeof b, 11u, 99u);
    (void)tpm_record_parse(b, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &vb);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&va, &vb), (int)TPM_RECORD_ROLLBACK,
                   "a lower version under a freshly incremented counter is REFUSED");

    /* A jump is not an authorized step even in the safe direction: the grant
     * bound ONE counter transition. */
    (void)az_build_floor(b, sizeof b, 12u, 200u);
    (void)tpm_record_parse(b, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &vb);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&va, &vb), (int)TPM_RECORD_STEP,
                   "a two-step generation jump is refused");

    (void)az_build_floor(b, sizeof b, 10u, 200u);
    (void)tpm_record_parse(b, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &vb);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&va, &vb), (int)TPM_RECORD_STEP,
                   "a standing generation is refused");

    (void)az_build_floor(b, sizeof b, 9u, 200u);
    (void)tpm_record_parse(b, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &vb);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&va, &vb), (int)TPM_RECORD_STEP,
                   "a receding generation is refused");

    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&va, 0), (int)TPM_RECORD_BADARG,
                   "NULL successor refused");
}

static void test_authz_first_record_generation(void)
{
    uint8_t rec[AZ_FLOOR_LEN];
    struct tpm_record_view v;
    const uint32_t plen = (uint32_t)sizeof(struct tpm_ab_floor_payload);

    /* A fresh TPM_NT_COUNTER does NOT start at zero: TPM 2.0 Part 1 section
     * 37.2.6.3 initializes it on its first increment to the largest value any
     * NV counter has held over the TPM's lifetime. A first-record rule pinned
     * to generation 1 would therefore refuse every first enrollment on a
     * previously-used TPM, which is the bug this asserts against. */
    (void)az_build_floor(rec, sizeof rec, 1u, 1u);
    (void)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &v);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(0, &v), (int)TPM_RECORD_OK,
                   "a first record at generation 1 is accepted");

    (void)az_build_floor(rec, sizeof rec, 501u, 1u);
    (void)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &v);
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(0, &v), (int)TPM_RECORD_OK,
                   "a first record at generation 501 is accepted (used TPM)");
}

static void test_authz_counter_commit_window(void)
{
    uint8_t rec[AZ_FLOOR_LEN];
    struct tpm_record_view v;
    const uint32_t plen = (uint32_t)sizeof(struct tpm_ab_floor_payload);

    (void)az_build_floor(rec, sizeof rec, 20u, 7u);
    (void)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &v);

    TEST_ASSERT_EQ((int)tpm_record_counter_ok(&v, 20u), (int)TPM_RECORD_OK,
                   "CONTROL: a record matching the counter is current");
    /* The legitimate write-then-increment window: written, not yet committed.
     * Reported as skew so a half-finished update cannot move the floor. */
    TEST_ASSERT_EQ((int)tpm_record_counter_ok(&v, 19u), (int)TPM_RECORD_SKEW,
                   "a record one AHEAD of the counter is not yet current");
    TEST_ASSERT_EQ((int)tpm_record_counter_ok(&v, 21u), (int)TPM_RECORD_SKEW,
                   "a record BEHIND the counter is stale");
    TEST_ASSERT_EQ((int)tpm_record_counter_ok(&v, 99u), (int)TPM_RECORD_SKEW,
                   "a record far behind the counter is stale");
    TEST_ASSERT_EQ((int)tpm_record_counter_ok(0, 20u), (int)TPM_RECORD_BADARG,
                   "NULL record refused");
}

static void test_authz_boot_budget_single_deadline(void)
{
    struct tpm_boot_budget b;
    uint32_t grant = 0;
    uint32_t i;

    /* THE CLAIM UNDER TEST: N records cannot each claim the full per-call
     * budget. Three operations against a 6000 ms quota with a 3000 ms per-call
     * budget must not be granted 9000 ms between them. */
    tpm_boot_budget_init(&b, 6000u, 8u);
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_OK,
                   "the first operation is admitted");
    TEST_ASSERT_EQ((int)grant, 3000, "and gets the full per-call budget");
    tpm_boot_budget_charge(&b, 3000u);

    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_OK,
                   "the second operation is admitted");
    TEST_ASSERT_EQ((int)grant, 3000, "with the quota exactly consumed");
    tpm_boot_budget_charge(&b, 3000u);

    /* Exhaustion is REPORTED, not a silently shortened read set. */
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_BUDGET,
                   "the third operation is refused: the boot quota is spent");

    /* THE CLAMP, isolated: a partially-spent quota grants only the remainder,
     * never the full per-call budget. */
    tpm_boot_budget_init(&b, 6000u, 8u);
    tpm_boot_budget_charge(&b, 4500u);
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_OK,
                   "an operation is admitted against the remainder");
    TEST_ASSERT_EQ((int)grant, 1500, "and is clamped to what the BOOT has left");

    /* Charging the FULL observed elapsed, not the grant: an operation that
     * overran must not be undercounted, because those are exactly the ones the
     * deadline exists to bound. */
    tpm_boot_budget_init(&b, 6000u, 8u);
    (void)tpm_boot_budget_admit(&b, 3000u, &grant);
    tpm_boot_budget_charge(&b, 7000u);   /* work + cleanup + an abort envelope */
    TEST_ASSERT_EQ((int)b.overrun, 1, "an overrun is latched");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_BUDGET,
                   "and nothing is admitted after an overrun");

    /* The breadth bound holds when there is no wall clock to enforce a quota,
     * which is the honest degraded mode: unmeasurable is not unlimited. */
    tpm_boot_budget_init(&b, 0u, 3u);
    for (i = 0; i < 3u; i++)
        TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_OK,
                       "CONTROL: operations inside the breadth bound are admitted");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_BUDGET,
                   "the operation count bounds a clockless boot");

    /* An UNARMED ledger governs nothing, so a caller outside the boot path is
     * never refused by a deadline nobody set up for it. */
    memset(&b, 0, sizeof b);
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_OK,
                   "an unarmed ledger admits");
    TEST_ASSERT_EQ((int)grant, 3000, "and does not clamp the request");

    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(0, 3000u, &grant), (int)TPM_NV_BADARG,
                   "a NULL ledger is refused");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 0u, &grant), (int)TPM_NV_BADARG,
                   "a zero request is refused");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, 0), (int)TPM_NV_BADARG,
                   "a NULL grant pointer is refused");

    /* A charge must never WRAP and refund the budget. */
    tpm_boot_budget_init(&b, 6000u, 8u);
    tpm_boot_budget_charge(&b, 0xFFFFFFFFu);
    tpm_boot_budget_charge(&b, 0xFFFFFFFFu);
    TEST_ASSERT_EQ((int)(b.spent_ms == 0xFFFFFFFFu), 1, "spent saturates rather than wrapping");
    TEST_ASSERT_EQ((int)tpm_boot_budget_remaining_ms(&b), 0, "and the remainder stays zero");

    /* THE EQUALITY BOUNDARY, all three sides. Millisecond truncation makes an
     * exact hit an ordinary outcome, not a corner case, and `overrun` means
     * strictly-more-than-was-left -- so exact exhaustion must leave overrun
     * CLEAR while still leaving nothing to admit. Keying a diagnostic off
     * overrun alone would go silent at precisely this value. */
    tpm_boot_budget_init(&b, 6000u, 8u);
    tpm_boot_budget_charge(&b, 5999u);
    TEST_ASSERT_EQ((int)b.overrun, 0, "grant-1: under the remainder does not overrun");
    TEST_ASSERT_EQ((int)tpm_boot_budget_remaining_ms(&b), 1, "and leaves 1 ms");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_OK,
                   "CONTROL: 1 ms is still admissible");
    TEST_ASSERT_EQ((int)grant, 1, "clamped to the 1 ms that is left");

    tpm_boot_budget_init(&b, 6000u, 8u);
    tpm_boot_budget_charge(&b, 6000u);
    TEST_ASSERT_EQ((int)b.overrun, 0,
                   "grant: exact exhaustion is NOT an overrun");
    TEST_ASSERT_EQ((int)tpm_boot_budget_remaining_ms(&b), 0, "but nothing is left");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_BUDGET,
                   "and nothing further is admitted");

    tpm_boot_budget_init(&b, 6000u, 8u);
    tpm_boot_budget_charge(&b, 6001u);
    TEST_ASSERT_EQ((int)b.overrun, 1, "grant+1: one millisecond over IS an overrun");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 3000u, &grant), (int)TPM_NV_BUDGET,
                   "and nothing is admitted after it");
}

/* THE LIFECYCLE AND THE WALL CLOCK, which nothing else pins.
 *
 * A fake TPM answers in microseconds, so every fixture read charges 0 ms:
 * deleting every settle call, breaking exact-exhaustion reporting, or
 * corrupting the elapsed arithmetic would leave the whole suite green while a
 * slow real TPM silently got independent full allowances again. The injected
 * clock is what makes those regressions visible. */
static void test_boot_budget_lifecycle_and_clock(void)
{
    struct tpm_boot_budget_test_state prev;
    struct tpm_boot_grant g;
    uint32_t grant = 0;
    struct tpm_boot_budget b;

    /* DISARM. Nothing else calls it, and a no-op disarm would leave Phase 1's
     * spent quota applied to every runtime verified read -- a later mark-good
     * or attestation refused with TPM_NV_BUDGET, forever. */
    prev = tpm_boot_budget_test_install(TPM_BOOT_BUDGET_MS, 1u);
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_POLICY_SEQ_COST_MS, &g),
                   (int)TPM_NV_OK, "CONTROL: the one permitted operation is admitted");
    tpm_boot_budget_settle(&g);
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_POLICY_SEQ_COST_MS, &g),
                   (int)TPM_NV_BUDGET, "and the ledger then refuses on breadth");
    tpm_boot_budget_disarm();
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_POLICY_SEQ_COST_MS, &g),
                   (int)TPM_NV_OK, "a DISARMED ledger admits again");
    TEST_ASSERT_EQ((int)g.granted_ms, (int)TPM_NV_POLICY_SEQ_COST_MS,
                   "and does not clamp what it grants");
    tpm_boot_budget_test_restore(prev);

    /* THE WALL CLOCK. Reserve, advance the injected clock past the grant, and
     * settle: the excess must be charged, not discarded. */
    prev = tpm_boot_budget_test_install(TPM_BOOT_BUDGET_MS, TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 1000u);
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_POLICY_SEQ_COST_MS, &g),
                   (int)TPM_NV_OK, "an operation reserves its grant");
    TEST_ASSERT_EQ((int)g.mark_ms, 1000, "against the injected clock");
    /* Overrun the reservation by exactly the remaining quota, so the boot ends
     * up with nothing left and must SAY so. */
    tpm_boot_budget_test_set_clock(1, 1000u + TPM_BOOT_BUDGET_MS + 1u);
    tpm_boot_budget_settle(&g);
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), 1,
                   "an overrun settlement latches expiry");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_POLICY_SEQ_COST_MS, &g),
                   (int)TPM_NV_BUDGET, "and nothing is admitted afterwards");
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(prev);

    /* THE REFUND. An operation that comes in UNDER its reservation must return
     * the difference, or the first read would permanently consume its whole
     * grant and a healthy boot would run out after two fast reads. */
    prev = tpm_boot_budget_test_install(TPM_BOOT_BUDGET_MS, TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 500u);
    (void)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &g);
    tpm_boot_budget_test_set_clock(1, 500u + 5u);      /* 5 ms of a huge grant */
    tpm_boot_budget_settle(&g);
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), 0,
                   "a fast operation does not exhaust the boot");
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &g),
                   (int)TPM_NV_OK, "so a second read is still admitted");
    TEST_ASSERT_EQ((int)g.granted_ms, (int)TPM_NV_VERIFIED_READ_BUDGET_MS,
                   "at the FULL request, because only 5 ms was actually spent");
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(prev);

    /* EPOCH. A settlement for a ledger that has been replaced must be dropped,
     * not charged into its successor. */
    prev = tpm_boot_budget_test_install(TPM_BOOT_BUDGET_MS, TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 100u);
    (void)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &g);
    tpm_boot_budget_arm();                 /* a NEW generation underneath it */
    tpm_boot_budget_test_set_clock(1, 100u + TPM_BOOT_BUDGET_MS + 1u);
    tpm_boot_budget_settle(&g);            /* a huge overrun, wrong epoch */
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), 0,
                   "a settlement across a re-arm is DROPPED, not charged");
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(prev);

    /* THE RESERVATION ITSELF. Hold grants OUTSTANDING across further
     * admissions: without the reserve-at-admit charge, each would read the same
     * untouched remainder and all three would be granted, which on SMP is two
     * concurrent reads each promised the whole boot's budget. max_ops is 0 here
     * so the refusal can only come from the reservation, never from breadth. */
    prev = tpm_boot_budget_test_install(TPM_BOOT_BUDGET_MS, 0u);
    tpm_boot_budget_test_set_clock(1, 10u);
    {
        struct tpm_boot_grant a, b2, c, copy;
        TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &a),
                       (int)TPM_NV_OK, "CONTROL: the first reservation is admitted");
        TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &b2),
                       (int)TPM_NV_OK, "CONTROL: the second fits the quota too");
        TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &c),
                       (int)TPM_NV_BUDGET,
                       "the third is REFUSED: the first two are still holding the quota");
        TEST_ASSERT_EQ((int)(a.id != b2.id), 1, "each reservation has its own id");

        /* Settling A at 5 ms returns almost all of A's reservation, and only
         * A's -- B is still outstanding and must still be held. The copy is
         * taken BEFORE the settlement, which is what makes the replay below a
         * real replay rather than an unknown-id lookup. */
        tpm_boot_budget_test_set_clock(1, 15u);
        copy = a;
        tpm_boot_budget_settle(&a);
        TEST_ASSERT_EQ((int)a.granted_ms, 0, "a settled grant is spent");
        TEST_ASSERT_EQ((int)a.id, 0, "and its id is consumed");
        TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS, &c),
                       (int)TPM_NV_OK, "so a new reservation now fits");

        /* REPLAY OF A REAL, ISSUED GRANT. `copy` was taken before A settled, so
         * it carries A's genuine id and epoch -- exactly what a caller that
         * passed its grant around by value would still be holding. Settling it
         * again must refund nothing; otherwise the same milliseconds come back
         * twice and reopen budget that B and C are still holding. Using a
         * never-issued id here would only have tested the lookup miss, and
         * would stay green if settlement stopped clearing the real slot. */
        tpm_boot_budget_settle(&copy);
        TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS,
                                                      &c),
                       (int)TPM_NV_BUDGET,
                       "a replayed settlement of a REAL grant refunds nothing");
    }
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(prev);

    /* THE PER-SEQUENCE WORK CLAMP. The aggregate bounds a whole verified read;
     * it must never widen ONE sequence past the transport's per-operation
     * ceiling. Inlined at the call site nothing could observe the work budget
     * passed to tpm2_seq_run, so deleting the clamp left a fast fixture green
     * while the final read inherited the entire three-sequence grant. */
    TEST_ASSERT_EQ((int)tpm_boot_grant_work_ms(TPM_NV_VERIFIED_READ_BUDGET_MS),
                   (int)TPM_NV_OP_BUDGET_MS,
                   "a full grant is clamped to the per-operation ceiling");
    TEST_ASSERT_EQ((int)tpm_boot_grant_work_ms(TPM_NV_OP_BUDGET_MS),
                   (int)TPM_NV_OP_BUDGET_MS, "the ceiling itself passes through");
    TEST_ASSERT_EQ((int)tpm_boot_grant_work_ms(TPM_NV_OP_BUDGET_MS - 1u),
                   (int)(TPM_NV_OP_BUDGET_MS - 1u),
                   "a partial grant is NOT widened up to the ceiling");
    TEST_ASSERT_EQ((int)tpm_boot_grant_work_ms(0u), 0,
                   "and nothing left grants nothing");

    /* ELAPSED WRAP and OPS SATURATION, both documented and neither pinned. */
    tpm_boot_budget_test_set_clock(1, 5u);
    TEST_ASSERT_EQ((int)tpm_boot_elapsed_ms(0xFFFFFFFAu), 11,
                   "elapsed across the 32-bit wrap is the true delta");
    tpm_boot_budget_test_set_clock(0, 0u);

    tpm_boot_budget_init(&b, 6000u, 0u /* no breadth bound */);
    b.ops = 0xFFFFFFFFu;
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, 100u, &grant), (int)TPM_NV_OK,
                   "a saturated op counter still admits when no breadth bound applies");
    TEST_ASSERT_EQ((int)(b.ops == 0xFFFFFFFFu), 1, "and the counter saturates rather than wrapping to 0");
}

/* The recovery-routing map, every enum value.
 *
 * This is the decision that tells an operator to migrate, to complete a commit,
 * or to enter authorized recovery, and the three are not interchangeable: a
 * destroyed anchor routed to re-enrollment overwrites the evidence of a
 * rollback with a fresh golden and launders it. Inline in the verifier this was
 * reachable only through a full fake-TIS fixture, so most of these values had
 * no coverage at all. */
static void test_baseline_pairing_status_map(void)
{
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status(TPM_PAIRING_CURRENT),
                   (int)TPM_BASELINE_RELABELED,
                   "an intact pairing whose digest failed is the RELABEL attack, "
                   "never a legacy blob to migrate");
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status(TPM_PAIRING_TORN),
                   (int)TPM_BASELINE_TORN,
                   "a counter ahead of its record routes to authorized recovery");
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status(TPM_PAIRING_UNCOMMITTED),
                   (int)TPM_BASELINE_TORN,
                   "an uncommitted record is a broken pairing, not a usable value");
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status(TPM_PAIRING_IMPOSSIBLE),
                   (int)TPM_BASELINE_TORN,
                   "a record more than one step ahead is a broken pairing too");
    /* This branch is reached ONLY on a MISMATCH, and the only MISMATCH that
     * arrives before a pairing is computed is an index that failed its enrolled
     * contract. That is detected tamper, so it must be an authenticity failure
     * the boot PUBLISHES -- not TPMERR, which the boot excludes precisely
     * because TPMERR means no conclusion was reached. */
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status(TPM_PAIRING_BADARG),
                   (int)TPM_BASELINE_IDENTITY,
                   "a pre-pair identity failure is a publishable authenticity verdict");
    TEST_ASSERT_EQ((int)(TPM_BASELINE_IDENTITY != TPM_BASELINE_TPMERR), 1,
                   "and is NOT the no-conclusion status the boot drops");

    /* An out-of-range value must land on the same fail-closed answer rather
     * than falling through to something a caller would act on. */
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status((tpm_pairing_t)99),
                   (int)TPM_BASELINE_IDENTITY,
                   "an unenumerated pairing fails closed");

    /* THE VERDICT PREDICATE, which decides both whether tpm_baseline_verify
     * writes *out_overall and whether the boot publishes it. A status that
     * reached NO conclusion must never be treated as a failure: doing so turns
     * a slow, contended or absent TPM into a tamper report. */
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_NO_TPM), 0,
                   "could-not-measure is NOT an integrity failure");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_TPMERR), 0,
                   "nor is a device fault that reached no conclusion");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_BADARG), 0,
                   "nor is a caller error");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_OK), 0,
                   "and success is obviously not one");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_TORN), 1,
                   "CONTROL: a broken pairing IS one");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_RELABELED), 1,
                   "CONTROL: so is a relabelled blob");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_IDENTITY), 1,
                   "CONTROL: so is a wrong index");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_UNBOUND), 1,
                   "CONTROL: and so is an unauthenticated blob, on the VERIFY path");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_CORRUPT), 1,
                   "a corrupt STORED baseline is a publication-critical failure");
    TEST_ASSERT_EQ((int)tpm_baseline_status_is_failure(TPM_BASELINE_SELF_CORRUPT), 1,
                   "and so is this kernel's own corrupt ABI identity");

    /* The three authenticity failures must stay DISTINCT values: collapsing any
     * pair is what would tell a machine under active tamper to migrate or
     * re-enroll its blob. */
    TEST_ASSERT_EQ((int)(TPM_BASELINE_RELABELED != TPM_BASELINE_UNBOUND), 1,
                   "the relabel attack and a legacy blob are different statuses");
    TEST_ASSERT_EQ((int)(TPM_BASELINE_TORN != TPM_BASELINE_RELABELED), 1,
                   "a broken pairing and a relabelled blob are different statuses");
    TEST_ASSERT_EQ((int)(TPM_BASELINE_IDENTITY != TPM_BASELINE_UNBOUND), 1,
                   "a wrong index and a legacy blob are different statuses");
}

/* The NV-status map, on the arms that decide PUBLICATION.
 *
 * A detected index recreation is the rollback attack itself. Routing it to
 * TPMERR left it caught and then discarded, because boot_phase1 excludes
 * TPMERR from publication on the (correct) grounds that it means no conclusion
 * was reached. Every arm below is a claim about whether the boot will speak. */
static void test_baseline_nv_status_map(void)
{
    /* THE PRE-PAIR AUTHENTICITY CLASS: detected tamper, and PUBLISHED. */
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_RECREATED),
                   (int)TPM_BASELINE_IDENTITY,
                   "a destroyed-and-recreated anchor is a published authenticity failure");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_CONTRACT),
                   (int)TPM_BASELINE_IDENTITY,
                   "and so is a corrupt persisted identity");

    /* None of them may reach NO_BASELINE, which this file treats as a genuine
     * first enroll -- that is exactly the laundering the gate exists to stop. */
    TEST_ASSERT_EQ((int)(tpm_baseline_nv_status(TPM_NV_RECREATED) !=
                         TPM_BASELINE_NO_BASELINE), 1,
                   "a recreated anchor is NEVER read as a first enroll");

    /* COULD-NOT-MEASURE: deliberately unpublished, so a slow or contended TPM
     * never reports a false tamper. The CONTROL for the class above -- without
     * it, mapping everything to IDENTITY would satisfy those assertions. */
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_BUDGET),
                   (int)TPM_BASELINE_NO_TPM,
                   "CONTROL: a budget expiry stays unpublished, not a tamper claim");
    /* CONTENTION IS THE ONE RETRY-SAFE CAUSE, and it is no longer NO_TPM.
     * The gate refused before anything was submitted, so nothing executed and
     * another attempt cannot double-apply -- which is what lets the boot retry
     * instead of abandoning its integrity verdict over a condition that clears
     * in milliseconds. It still publishes nothing on THIS attempt. */
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_BUSY),
                   (int)TPM_BASELINE_BUSY,
                   "ordinary transport contention is retryable, not an absent TPM");
    /* The SPLIT is the property, so assert the two are distinguishable rather
     * than only that each maps somewhere. Collapsing them again would make a
     * momentarily busy TPM indistinguishable from a missing one, which is the
     * bug this mapping was changed to fix. */
    TEST_ASSERT_EQ((int)(TPM_BASELINE_BUSY != TPM_BASELINE_NO_TPM), 1,
                   "contention and could-not-measure are separately reportable");
    /* And the ASYMMETRY is deliberate, not an oversight: BUDGET looks equally
     * transient and is NOT retry-safe, because the command was abandoned in
     * flight and may have executed. If a later change routes BUDGET here too,
     * this fails -- which is the point. */
    TEST_ASSERT_EQ((int)(tpm_baseline_nv_status(TPM_NV_BUDGET) !=
                         TPM_BASELINE_BUSY), 1,
                   "a budget expiry is NEVER reported as blind-retryable");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_TRANSPORT),
                   (int)TPM_BASELINE_NO_TPM,
                   "CONTROL: and an absent transport");

    /* The remaining named arms, so a future status cannot inherit a bucket. */
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_OK), (int)TPM_BASELINE_OK,
                   "CONTROL: success maps to success");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_NOTFOUND),
                   (int)TPM_BASELINE_NO_BASELINE, "an undefined index has no baseline");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_BADARG),
                   (int)TPM_BASELINE_BADARG, "a caller error is not a TPM fault");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_UNAVAIL),
                   (int)TPM_BASELINE_UNBOUND, "no authority is a configuration state");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_ATTRS),
                   (int)TPM_BASELINE_TPMERR,
                   "an illegal attribute request stays a device-shaped fault");

    /* MISMATCH IS CONTEXT-DEPENDENT, and the two maps must disagree about it
     * ON PURPOSE. Globally it stays TPMERR because the WRITE path produces it
     * for an ordinary counter race on a healthy SMP box, and calling that
     * tamper is worse than the bug that tempted the widening. The VERIFY path
     * reaches the identity verdict through the pairing map instead, which only
     * ever sees a MISMATCH that arrived before a pairing could be computed. */
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(TPM_NV_MISMATCH),
                   (int)TPM_BASELINE_TPMERR,
                   "a write-path MISMATCH is NOT identity tamper: a counter race is benign");
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status(TPM_PAIRING_BADARG),
                   (int)TPM_BASELINE_IDENTITY,
                   "while the verify path still reports identity tamper for its own MISMATCH");
    TEST_ASSERT_EQ((int)(tpm_baseline_nv_status(TPM_NV_MISMATCH) !=
                         tpm_baseline_pairing_status(TPM_PAIRING_BADARG)), 1,
                   "the two maps disagree about MISMATCH deliberately, not by drift");
}

/* The aggregate deadline must bound LATENCY, not merely account for it.
 *
 * A verified read runs sequences that cannot be subdivided: tpm_authz_contract
 * arms fixed per-operation constants, so once a sequence starts it can spend
 * its whole work plus cleanup whatever the remainder was. Admission therefore
 * has to refuse a read whose remainder cannot pay for even one sequence. */
static void test_boot_budget_indivisible_sequence(void)
{
    struct tpm_boot_budget b;
    uint32_t grant = 0;

    /* CONTROL: a full quota grants at least one whole sequence. */
    tpm_boot_budget_init(&b, TPM_BOOT_BUDGET_MS, TPM_BOOT_BUDGET_OPS);
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, TPM_NV_VERIFIED_READ_BUDGET_MS,
                                              &grant), (int)TPM_NV_OK,
                   "CONTROL: a fresh boot quota admits a verified read");
    TEST_ASSERT_EQ((int)(grant >= TPM_NV_POLICY_SEQ_COST_MS), 1,
                   "CONTROL: and the grant covers at least one whole sequence");

    /* A remainder ONE MILLISECOND short of a sequence still admits -- the
     * ledger's job is the clamp, and refusing on the shortfall is the CALLER's
     * check, because only the caller knows a sequence is what it is about to
     * start. The assertion pins that division of labour so a future edit
     * cannot quietly move the refusal and leave both sides assuming the other
     * does it. */
    tpm_boot_budget_init(&b, TPM_BOOT_BUDGET_MS, TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_charge(&b, TPM_BOOT_BUDGET_MS - (TPM_NV_POLICY_SEQ_COST_MS - 1u));
    TEST_ASSERT_EQ((int)tpm_boot_budget_admit(&b, TPM_NV_VERIFIED_READ_BUDGET_MS,
                                              &grant), (int)TPM_NV_OK,
                   "the ledger still admits against a short remainder");
    TEST_ASSERT_EQ((int)grant, (int)(TPM_NV_POLICY_SEQ_COST_MS - 1u),
                   "clamped to exactly the short remainder");
    TEST_ASSERT_EQ((int)(grant < TPM_NV_POLICY_SEQ_COST_MS), 1,
                   "which is the shortfall the read path refuses on");

    /* The constants must keep the relation the design rests on: a boot quota
     * that could not fund a whole read would clamp every first admission. */
    TEST_ASSERT_EQ((int)(TPM_BOOT_BUDGET_MS >= TPM_NV_VERIFIED_READ_BUDGET_MS), 1,
                   "the boot quota funds at least one whole verified read");
    TEST_ASSERT_EQ((int)(TPM_NV_VERIFIED_READ_BUDGET_MS >= TPM_NV_POLICY_SEQ_COST_MS), 1,
                   "and a read's request funds at least one whole sequence");
}

static void test_authz_pairing_direction(void)
{
    uint8_t rec[AZ_FLOOR_LEN];
    struct tpm_record_view v;
    const uint32_t plen = (uint32_t)sizeof(struct tpm_ab_floor_payload);

    (void)az_build_floor(rec, sizeof rec, 20u, 7u);
    (void)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &v);

    /* The strict gate collapses every direction into SKEW; this classifier is
     * what recovery routing needs, because the two directions demand opposite
     * actions. The CONTROL is load-bearing: a classifier that answered TORN for
     * everything would satisfy the TORN assertion on its own. */
    TEST_ASSERT_EQ((int)tpm_record_pairing(&v, 20u), (int)TPM_PAIRING_CURRENT,
                   "CONTROL: a record matching the counter is CURRENT");
    TEST_ASSERT_EQ((int)tpm_record_pairing(&v, 19u), (int)TPM_PAIRING_UNCOMMITTED,
                   "one AHEAD of the counter is the write-then-increment window");
    TEST_ASSERT_EQ((int)tpm_record_pairing(&v, 21u), (int)TPM_PAIRING_TORN,
                   "the counter one ahead of the record is a TORN pairing");
    TEST_ASSERT_EQ((int)tpm_record_pairing(&v, 9999u), (int)TPM_PAIRING_TORN,
                   "a counter far ahead is TORN, not a fresh install");
    TEST_ASSERT_EQ((int)tpm_record_pairing(&v, 18u), (int)TPM_PAIRING_IMPOSSIBLE,
                   "two ahead of the counter cannot come from one grant");
    TEST_ASSERT_EQ((int)tpm_record_pairing(0, 20u), (int)TPM_PAIRING_BADARG,
                   "NULL record refused");

    /* A saturated counter must not wrap the +1 into a false UNCOMMITTED. */
    (void)az_build_floor(rec, sizeof rec, 0xFFFFFFFFFFFFFFFFull, 7u);
    (void)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &v);
    TEST_ASSERT_EQ((int)tpm_record_pairing(&v, 0xFFFFFFFFFFFFFFFFull),
                   (int)TPM_PAIRING_CURRENT, "CONTROL: saturated == saturated is CURRENT");
    TEST_ASSERT_EQ((int)tpm_record_pairing(&v, 0xFFFFFFFFFFFFFFFEull),
                   (int)TPM_PAIRING_UNCOMMITTED, "saturated record one ahead is the window");

    /* The strict gate's own contract over these same inputs is NOT re-asserted
     * here: test_authz_counter_commit_window already pins counter_ok to SKEW in
     * both directions, and duplicating it would only add a second place to
     * update when the contract changes. */
}

static void test_authz_baseline_relabel_refused(void)
{
    /* THE OTHER ACCEPTANCE REFUSAL. An attacker takes an OLD baseline blob,
     * stamps the CURRENT generation on the bind record and recomputes
     * everything they can reach. The bind record's digest is over the blob they
     * did not get to choose, so the substitution fails there. */
    static const uint8_t old_blob[32] = { 0xDE, 0xAD };
    static const uint8_t new_blob[32] = { 0xBE, 0xEF };
    uint8_t bind[AZ_BIND_LEN];
    struct tpm_record_view v;
    const struct tpm_baseline_bind_payload *p;
    uint8_t d[TPM_RECORD_DIGEST];

    /* The authority bound the CURRENT baseline at generation 4. */
    (void)az_build_bind(bind, sizeof bind, 4u, new_blob, (uint32_t)sizeof new_blob);
    TEST_ASSERT_EQ((int)tpm_record_parse(bind, AZ_BIND_LEN, TPM_RECORD_KIND_BASELINE,
                                         (uint32_t)sizeof(struct tpm_baseline_bind_payload), &v),
                   (int)TPM_RECORD_OK, "CONTROL: the authorized bind record parses");
    p = (const struct tpm_baseline_bind_payload *)v.payload;

    sha256(new_blob, sizeof new_blob, d);
    TEST_ASSERT_EQ(memcmp(d, p->blob_digest, TPM_RECORD_DIGEST), 0,
                   "CONTROL: the bound blob matches its record");

    sha256(old_blob, sizeof old_blob, d);
    TEST_ASSERT(memcmp(d, p->blob_digest, TPM_RECORD_DIGEST) != 0,
                "an OLD blob under the current generation does not match: REFUSED");
    TEST_ASSERT_EQ((int)p->blob_len, (int)sizeof new_blob, "bound length recorded");
}

/* ---- cpHash and the five new command builders ---- */

static void test_authz_cphash(void)
{
    static const uint8_t names[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint8_t params[4] = { 9, 9, 9, 9 };
    uint8_t a[32], b[32];

    TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_WRITE, names, sizeof names,
                                            params, sizeof params, a),
                   (int)TPM_NV_OK, "CONTROL: cpHash computes");
    TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_WRITE, names, sizeof names,
                                            params, sizeof params, b),
                   (int)TPM_NV_OK, "cpHash recomputes");
    TEST_ASSERT_EQ(memcmp(a, b, 32), 0, "cpHash is deterministic");

    /* The command code is inside the hash, which is what stops a grant for one
     * command satisfying another. */
    TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_INCREMENT, names, sizeof names,
                                            params, sizeof params, b),
                   (int)TPM_NV_OK, "cpHash for a different command computes");
    TEST_ASSERT(memcmp(a, b, 32) != 0, "a different command code changes the cpHash");

    /* So are the handle Names, which is what stops a grant for one index
     * authorizing a write to another. */
    {
        uint8_t other[8];
        memcpy(other, names, sizeof other);
        other[0] ^= 0xFFu;
        TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_WRITE, other, sizeof other,
                                                params, sizeof params, b),
                       (int)TPM_NV_OK, "cpHash over a different Name computes");
        TEST_ASSERT(memcmp(a, b, 32) != 0, "a different handle Name changes the cpHash");
    }

    /* And the parameters, which is what binds the exact record bytes. */
    {
        uint8_t other[4] = { 9, 9, 9, 8 };
        TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_WRITE, names, sizeof names,
                                                other, sizeof other, b),
                       (int)TPM_NV_OK, "cpHash over different parameters computes");
        TEST_ASSERT(memcmp(a, b, 32) != 0, "a different parameter changes the cpHash");
    }

    /* A NULL pointer with a nonzero length is caller misuse, not an empty
     * field: hashing zero bytes for it would silently produce a cpHash for a
     * DIFFERENT command than the one about to be submitted. */
    TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_WRITE, 0, 8u, params, 4u, a),
                   (int)TPM_NV_BADARG, "NULL names with a length is refused");
    TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_WRITE, names, 8u, 0, 4u, a),
                   (int)TPM_NV_BADARG, "NULL params with a length is refused");
    TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_WRITE, names, 8u, params, 4u, 0),
                   (int)TPM_NV_BADARG, "NULL output is refused");
    /* Genuinely empty fields ARE legal (NV_Increment carries no parameters). */
    TEST_ASSERT_EQ((int)tpm2_cphash_compute(TPM2_CC_NV_INCREMENT, names, 8u, 0, 0u, a),
                   (int)TPM_NV_OK, "CONTROL: an empty parameter area is legal");
}

static void test_authz_build_policy_cphash(void)
{
    uint8_t buf[64], cph[32];
    uint32_t n;

    memset(cph, 0x5Au, sizeof cph);
    n = tpm2_build_policy_cphash(buf, sizeof buf, 0x03000000u, cph);
    TEST_ASSERT_EQ((int)n, 48, "PolicyCpHash marshals to 48 bytes");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf), (int)TPM2_ST_NO_SESSIONS,
                   "a policy assertion carries no auth area");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 2), 48, "declared size matches");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6), (int)TPM2_CC_POLICY_CP_HASH,
                   "command code is PolicyCpHash");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 10), 0x03000000, "session handle placed");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 14), 32, "cpHashA TPM2B size");
    TEST_ASSERT_EQ((int)buf[16], 0x5A, "cpHashA body copied");

    TEST_ASSERT_EQ((int)tpm2_build_policy_cphash(buf, 47u, 0x03000000u, cph), 0,
                   "a short buffer is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_cphash(buf, sizeof buf, 0u, cph), 0,
                   "session handle 0 is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_cphash(buf, sizeof buf, 0x03000000u, 0), 0,
                   "NULL cpHash is refused");
}

static void test_authz_build_policy_nv(void)
{
    uint8_t buf[96], operand[8];
    uint32_t n;

    memset(operand, 0x11, sizeof operand);
    n = tpm2_build_policy_nv(buf, sizeof buf, TPM_NV_INDEX_AB_SEQ, 0x03000000u,
                             operand, (uint16_t)sizeof operand, 0u, TPM2_EO_EQ);
    TEST_ASSERT_EQ((int)n, 49, "PolicyNV marshals to 49 bytes");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf), (int)TPM2_ST_SESSIONS,
                   "PolicyNV authorizes its authHandle");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6), (int)TPM2_CC_POLICY_NV, "command code");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 10), (int)TPM_RH_OWNER, "authHandle is owner");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 14), (int)TPM_NV_INDEX_AB_SEQ, "nvIndex");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 18), 0x03000000, "policySession");
    /* authorizationSize(4) + one 13-byte auth entry lands the operand at 35. */
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 35), 8, "operandB TPM2B size");
    TEST_ASSERT_EQ((int)buf[37], 0x11, "operandB body copied");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 45), 0, "offset placed");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 47), (int)TPM2_EO_EQ, "operation placed");

    TEST_ASSERT_EQ((int)tpm2_build_policy_nv(buf, 48u, TPM_NV_INDEX_AB_SEQ, 0x03000000u,
                                             operand, 8u, 0u, TPM2_EO_EQ), 0,
                   "a short buffer is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_nv(buf, sizeof buf, TPM_NV_INDEX_AB_SEQ, 0u,
                                             operand, 8u, 0u, TPM2_EO_EQ), 0,
                   "session handle 0 is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_nv(buf, sizeof buf, TPM_NV_INDEX_AB_SEQ,
                                             0x03000000u, 0, 8u, 0u, TPM2_EO_EQ), 0,
                   "NULL operand with a length is refused");
}

static void test_authz_build_policy_authorize(void)
{
    uint8_t buf[256], approved[32], name[34], ticket[TPM2_TK_VERIFIED_NULL_LEN];
    static const uint8_t ref[4] = { 'I', 'P', 'O', 'S' };
    uint32_t n;

    memset(approved, 0xA1, sizeof approved);
    memset(name, 0xB2, sizeof name);
    memset(ticket, 0xC3, sizeof ticket);

    n = tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                    approved, (uint16_t)sizeof approved,
                                    ref, (uint16_t)sizeof ref,
                                    name, (uint16_t)sizeof name,
                                    ticket, (uint32_t)sizeof ticket);
    /* 10 + 4 + (2+32) + (2+4) + (2+34) + 8 */
    TEST_ASSERT_EQ((int)n, 98, "PolicyAuthorize marshals to 98 bytes");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6), (int)TPM2_CC_POLICY_AUTHORIZE,
                   "command code");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 14), 32, "approvedPolicy TPM2B size");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 48), 4, "policyRef TPM2B size");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 54), 34, "keySign TPM2B size");
    /* The Name BODY follows its single size prefix. A second prefix here is the
     * defect that made the TPM read 0x0022 as the name algorithm. */
    TEST_ASSERT_EQ((int)buf[56], 0xB2, "keySign body starts immediately after ONE prefix");

    /* An empty approved policy or an absent key Name would marshal cleanly and
     * assert nothing. */
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    approved, 0u, ref, 4u,
                                                    name, 34u, ticket, 8u), 0,
                   "an empty approved policy is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    approved, 32u, ref, 4u,
                                                    name, 0u, ticket, 8u), 0,
                   "an empty key Name is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    approved, 32u, ref, 4u,
                                                    name, 34u, ticket, 0u), 0,
                   "an absent ticket is refused");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0u,
                                                    approved, 32u, ref, 4u,
                                                    name, 34u, ticket, 8u), 0,
                   "session handle 0 is refused");
    /* An EMPTY policyRef is legal and must stay legal. */
    TEST_ASSERT(tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                            approved, 32u, 0, 0u,
                                            name, 34u, ticket, 8u) != 0u,
                "CONTROL: an empty policyRef is legal");
}

static void test_authz_build_load_external(void)
{
    uint8_t buf[128], pub[40];
    uint32_t n;

    memset(pub, 0x77, sizeof pub);
    n = tpm2_build_load_external(buf, sizeof buf, pub, (uint16_t)sizeof pub,
                                 TPM_RH_OWNER);
    TEST_ASSERT_EQ((int)n, 58, "LoadExternal marshals to 58 bytes");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6), (int)TPM2_CC_LOAD_EXTERNAL,
                   "command code");
    /* The EMPTY inPrivate is what selects a public-only load. */
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 10), 0, "inPrivate is empty");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 12), 40, "inPublic TPM2B size");
    TEST_ASSERT_EQ((int)buf[14], 0x77, "inPublic body copied");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 54), (int)TPM_RH_OWNER,
                   "hierarchy named so a ticket has a proof value");

    TEST_ASSERT_EQ((int)tpm2_build_load_external(buf, 57u, pub, 40u, TPM_RH_OWNER), 0,
                   "a short buffer is refused");
    TEST_ASSERT_EQ((int)tpm2_build_load_external(buf, sizeof buf, pub, 0u,
                                                 TPM_RH_OWNER), 0,
                   "an empty public area is refused");
    TEST_ASSERT_EQ((int)tpm2_build_load_external(buf, sizeof buf, 0, 40u,
                                                 TPM_RH_OWNER), 0,
                   "NULL public area is refused");
}

static void test_authz_build_verify_signature(void)
{
    uint8_t buf[128], digest[32], sig[20];
    uint32_t n;

    memset(digest, 0x31, sizeof digest);
    memset(sig, 0x42, sizeof sig);
    n = tpm2_build_verify_signature(buf, sizeof buf, 0x80000001u, digest,
                                    sig, (uint16_t)sizeof sig);
    TEST_ASSERT_EQ((int)n, 68, "VerifySignature marshals to 68 bytes");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 6), (int)TPM2_CC_VERIFY_SIGNATURE,
                   "command code");
    TEST_ASSERT_EQ((int)tpm2_be32_get(buf + 10), (int)0x80000001, "key handle placed");
    TEST_ASSERT_EQ((int)tpm2_be16_get(buf + 14), 32, "digest TPM2B size");
    TEST_ASSERT_EQ((int)buf[48], 0x42, "signature copied verbatim (already a TPMT)");

    TEST_ASSERT_EQ((int)tpm2_build_verify_signature(buf, 67u, 0x80000001u, digest,
                                                    sig, 20u), 0,
                   "a short buffer is refused");
    TEST_ASSERT_EQ((int)tpm2_build_verify_signature(buf, sizeof buf, 0u, digest,
                                                    sig, 20u), 0,
                   "key handle 0 is refused");
    TEST_ASSERT_EQ((int)tpm2_build_verify_signature(buf, sizeof buf, 0x80000001u,
                                                    digest, sig, 0u), 0,
                   "an empty signature is refused");
}

static void test_authz_parse_bounds(void)
{
    uint8_t rsp[128], out[64];
    uint32_t handle = 0, tlen = 0;
    uint16_t nlen = 0;

    /* A well-formed LoadExternal reply: header, handle, paramSize, TPM2B_NAME. */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(rsp + 2, 10u + 4u + 2u + 34u);
    tpm2_be32_put(rsp + 6, 0u);
    tpm2_be32_put(rsp + 10, 0x80000002u);
    tpm2_be16_put(rsp + 14, 34u);
    rsp[16] = 0xAB;
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 10u + 4u + 2u + 34u, &handle,
                                                 out, (uint16_t)sizeof out, &nlen),
                   (int)TPM_NV_OK, "CONTROL: a well-formed LoadExternal reply parses");
    TEST_ASSERT_EQ((int)handle, (int)0x80000002, "object handle recovered");
    TEST_ASSERT_EQ((int)nlen, 34, "Name length recovered");
    TEST_ASSERT_EQ((int)out[0], 0xAB, "Name body recovered without its size prefix");

    /* A Name that does not consume the parameter area exactly is malformed. */
    tpm2_be16_put(rsp + 14, 33u);
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 10u + 4u + 2u + 34u, &handle,
                                                 out, (uint16_t)sizeof out, &nlen),
                   (int)TPM_NV_TRANSPORT, "an inconsistent Name length is refused");

    /* A Name longer than the caller's buffer is a refusal, never a truncation. */
    tpm2_be16_put(rsp + 14, 34u);
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 10u + 4u + 2u + 34u, &handle,
                                                 out, 8u, &nlen),
                   (int)TPM_NV_TRANSPORT, "an over-long Name is refused, not truncated");

    /* A handle of zero is never valid. */
    tpm2_be32_put(rsp + 10, 0u);
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 10u + 4u + 2u + 34u, &handle,
                                                 out, (uint16_t)sizeof out, &nlen),
                   (int)TPM_NV_TRANSPORT, "a zero object handle is refused");

    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 8u, &handle, out, 64u, &nlen),
                   (int)TPM_NV_TRANSPORT, "a truncated reply is refused");

    /* VerifySignature: the whole parameter area IS the ticket. */
    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(rsp + 2, 10u + 8u);
    tpm2_be32_put(rsp + 6, 0u);
    rsp[10] = 0x80; rsp[11] = 0x22;
    TEST_ASSERT_EQ((int)tpm2_parse_verify_signature(rsp, 18u, out, 64u, &tlen),
                   (int)TPM_NV_OK, "CONTROL: a ticket parses");
    TEST_ASSERT_EQ((int)tlen, 8, "ticket length is the parameter area");
    TEST_ASSERT_EQ((int)out[0], 0x80, "ticket copied verbatim");
    /* A ticket that does not fit is refused: truncating one would hand
     * PolicyAuthorize a mangled authorization. */
    TEST_ASSERT_EQ((int)tpm2_parse_verify_signature(rsp, 18u, out, 4u, &tlen),
                   (int)TPM_NV_TRANSPORT, "an over-long ticket is refused");
    tpm2_be32_put(rsp + 2, 10u);
    TEST_ASSERT_EQ((int)tpm2_parse_verify_signature(rsp, 10u, out, 64u, &tlen),
                   (int)TPM_NV_TRANSPORT, "an empty ticket is refused");
}

/* ---- the authority: unprovisioned fails CLOSED ---- */

static void test_authz_unprovisioned_fails_closed(void)
{
    struct tpm_authz_transition tr;
    uint8_t name[64];
    uint16_t nlen = 0;
    uint32_t ver = 0;

    memset(&tr, 0, sizeof tr);
    tpm_authz_test_clear_authority();
    TEST_ASSERT_EQ(tpm_authz_provisioned(), 0, "no authority after clearing");

    /* Every authorized path refuses, and refuses with UNAVAIL rather than a
     * fault: a kernel with no key compiled in is CORRECTLY refusing, the way
     * Secure Boot with an empty db refuses every image. */
    TEST_ASSERT_EQ((int)tpm_authz_authority_name(name, (uint16_t)sizeof name, &nlen),
                   (int)TPM_NV_UNAVAIL, "no authority Name without an authority");
    TEST_ASSERT_EQ((int)tpm_ab_floor_advance(5u, &tr), (int)TPM_NV_UNAVAIL,
                   "the floor cannot be advanced without an authority");
    TEST_ASSERT_EQ((int)tpm_baseline_bind_write(name, 8u, &tr), (int)TPM_NV_UNAVAIL,
                   "a baseline cannot be bound without an authority");
    TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                               TPM_NV_INDEX_AB_SEQ,
                                               name, 8u, 0u, &tr),
                   (int)TPM_NV_BADARG,
                   "an empty grant is caller misuse before the authority is consulted");
    (void)ver;
}

static void test_authz_authority_name(void)
{
    /* A minimal TPMT_PUBLIC prefix: type(2) || nameAlg(2) || attributes(4). Only
     * the nameAlg field and the total byte string matter to the Name. */
    static uint8_t pub[16];
    struct tpm_authz_authority a;
    uint8_t name[64], want[32];
    uint16_t nlen = 0;

    memset(pub, 0x5C, sizeof pub);
    tpm2_be16_put(pub + 0, 0x0001u);            /* type */
    tpm2_be16_put(pub + 2, TPM_ALG_SHA256);     /* nameAlg */

    memset(&a, 0, sizeof a);
    a.public_area = pub;
    a.public_len = (uint16_t)sizeof pub;
    TEST_ASSERT_EQ((int)tpm_authz_set_authority(&a), (int)TPM_NV_OK,
                   "CONTROL: a well-formed authority installs");
    TEST_ASSERT_EQ(tpm_authz_provisioned(), 1, "authority reported provisioned");

    TEST_ASSERT_EQ((int)tpm_authz_authority_name(name, (uint16_t)sizeof name, &nlen),
                   (int)TPM_NV_OK, "authority Name computes");
    /* Name := nameAlg || H(publicArea), and the BODY only -- the builders add
     * the TPM2B prefix themselves. */
    TEST_ASSERT_EQ((int)nlen, 34, "Name body is 34 bytes, with no size prefix");
    TEST_ASSERT_EQ((int)tpm2_be16_get(name), (int)TPM_ALG_SHA256, "Name leads with nameAlg");
    sha256(pub, sizeof pub, want);
    TEST_ASSERT_EQ(memcmp(name + 2, want, 32), 0, "Name digest is H(publicArea)");

    TEST_ASSERT_EQ((int)tpm_authz_authority_name(name, 33u, &nlen), (int)TPM_NV_BADARG,
                   "a buffer one byte short is refused");

    /* A nameAlg the kernel cannot hash is refused rather than hashed with the
     * wrong algorithm, which would produce a plausible Name matching nothing. */
    tpm2_be16_put(pub + 2, 0x000Cu);   /* SHA-384 */
    TEST_ASSERT_EQ((int)tpm_authz_authority_name(name, (uint16_t)sizeof name, &nlen),
                   (int)TPM_NV_BADARG, "a non-SHA-256 nameAlg is refused");
    tpm2_be16_put(pub + 2, TPM_ALG_SHA256);

    /* ONE-WAY: with an authority installed, every later call is refused --
     * including a NULL one. A clearable authority would make the whole boundary
     * optional to any caller that could reach this setter, because both the
     * enroll refusal and the verifier's bind check ask only whether one is
     * provisioned. */
    TEST_ASSERT_EQ((int)tpm_authz_set_authority(&a), (int)TPM_NV_AUTH,
                   "re-installing an authority is refused");
    TEST_ASSERT_EQ((int)tpm_authz_set_authority(0), (int)TPM_NV_AUTH,
                   "CLEARING an installed authority is refused");
    TEST_ASSERT_EQ(tpm_authz_provisioned(), 1, "the authority survived both attempts");

    /* The misconfiguration rejections are reachable only from the
     * unprovisioned state, which is what the one-way rule means. */
    tpm_authz_test_clear_authority();
    {
        struct tpm_authz_authority bad;
        memset(&bad, 0, sizeof bad);
        TEST_ASSERT_EQ((int)tpm_authz_set_authority(&bad), (int)TPM_NV_BADARG,
                       "an empty authority is refused");
        bad.public_area = pub; bad.public_len = 4u;
        TEST_ASSERT_EQ((int)tpm_authz_set_authority(&bad), (int)TPM_NV_BADARG,
                       "a public area too short to carry nameAlg is refused");
        bad.public_len = (uint16_t)sizeof pub; bad.policy_ref = 0; bad.policy_ref_len = 3u;
        TEST_ASSERT_EQ((int)tpm_authz_set_authority(&bad), (int)TPM_NV_BADARG,
                       "a NULL policyRef with a length is refused");
        TEST_ASSERT_EQ(tpm_authz_provisioned(), 0,
                       "a refused misconfiguration installs nothing");
    }

    tpm_authz_test_clear_authority();
    TEST_ASSERT_EQ(tpm_authz_provisioned(), 0, "authority cleared at teardown");
}

static void test_authz_manifest(void)
{
    const struct tpm_enroll_entry *e;

    e = tpm_enroll_lookup(TPM_NV_INDEX_AB_FLOOR);
    TEST_ASSERT(e != 0, "the A/B floor is in the manifest");
    TEST_ASSERT_EQ((int)e->data_size, (int)AZ_FLOOR_LEN,
                   "the floor entry is sized for its record");
    TEST_ASSERT_EQ((int)e->policy_kind, (int)TPM_ENROLL_POLICY_AUTHORIZE,
                   "the floor carries the authorization boundary");
    TEST_ASSERT((e->attrs & TPMA_NV_POLICYWRITE) != 0u, "the floor is policy-written");
    TEST_ASSERT_EQ((int)(e->attrs & TPMA_NV_OWNERWRITE), 0,
                   "the floor is NOT owner-writable");

    e = tpm_enroll_lookup(TPM_NV_INDEX_BASELINE_BIND);
    TEST_ASSERT(e != 0, "the baseline bind index is in the manifest");
    TEST_ASSERT_EQ((int)e->data_size, (int)AZ_BIND_LEN,
                   "the bind entry is sized for its record");

    /* Both counters are anchors in their own right: each one's increment IS the
     * irreversible commit point, so neither may be owner-writable. */
    e = tpm_enroll_lookup(TPM_NV_INDEX_AB_SEQ);
    TEST_ASSERT(e != 0, "the A/B sequence counter is in the manifest");
    TEST_ASSERT_EQ((int)(e->attrs & TPMA_NV_OWNERWRITE), 0,
                   "the A/B counter is NOT owner-incrementable");
    TEST_ASSERT((e->attrs & TPMA_NV_OWNERREAD) != 0u,
                "the A/B counter stays readable on every boot");
    /* WRITTEN is CLEAR on a counter until its first increment, so demanding it
     * would refuse every machine between enrollment and its first transition. */
    TEST_ASSERT_EQ((int)e->expect_written, 0,
                   "a counter is not expected written before its first increment");

    e = tpm_enroll_lookup(TPM_NV_INDEX_BASELINE_GEN);
    TEST_ASSERT(e != 0, "the baseline generation counter is in the manifest");
    TEST_ASSERT_EQ((int)(e->attrs & TPMA_NV_OWNERWRITE), 0,
                   "the baseline counter is NOT owner-incrementable");

    /* No manifest row means no contract, which is itself the answer to "may I
     * verify against this handle". */
    TEST_ASSERT(tpm_enroll_lookup(TPM_NV_INDEX_OS_DATA) == 0,
                "an ordinary data index is not an enrolled anchor");
    TEST_ASSERT(tpm_enroll_lookup(0x01FFFFFFu) == 0, "an unknown handle has no entry");
}

static void test_authz_owner_increment_refused(void)
{
    /* The owner-auth increment wrapper refuses the anchors OUTRIGHT rather than
     * letting firmware reject them. Letting it through would hide the layering
     * mistake on every emulator whose policy enforcement is more forgiving than
     * a real TPM's, which is precisely the class of bug that only appears on
     * hardware. No transport is installed, so a wrapper that did NOT refuse
     * would fail with a transport status instead. */
    TEST_ASSERT_EQ((int)tpm_nv_increment(TPM_NV_INDEX_AB_SEQ), (int)TPM_NV_AUTH,
                   "owner-auth increment of the A/B counter is refused");
    TEST_ASSERT_EQ((int)tpm_nv_increment(TPM_NV_INDEX_BASELINE_GEN), (int)TPM_NV_AUTH,
                   "owner-auth increment of the baseline counter is refused");

    /* And the policy-counter define validates its policy argument. */
    TEST_ASSERT_EQ((int)tpm_nv_define_counter_policy(TPM_NV_INDEX_AB_SEQ, 0, 0u),
                   (int)TPM_NV_BADARG, "a policy counter needs an authPolicy");
    {
        uint8_t pol[32];
        memset(pol, 0x11, sizeof pol);
        TEST_ASSERT_EQ((int)tpm_nv_define_counter_policy(TPM_NV_INDEX_AB_SEQ, pol,
                                                         (uint16_t)(TPM_NV_POLICY_MAX + 1u)),
                       (int)TPM_NV_BADARG, "an over-long authPolicy is refused");
    }
}

/* ---- fake TIS: a CC-dispatch TPM for the authorized-transition path ----
 *
 * Serves the whole command set one transition drives: ReadPublic (with a REAL
 * Name, so the identity gate is actually exercised rather than waved through),
 * NV_Read for both the counter and the record, LoadExternal, VerifySignature,
 * StartAuthSession, the four policy assertions, NV_Write, NV_Increment and
 * FlushContext. No MMIO, no live boot infrastructure. */

/* Sized to the largest command and response this fixture actually carries: an
 * NV_Write of the 104-byte bind record is ~140 bytes and PolicyAuthorize is 98.
 * These are kernel BSS in a test build, and an over-generous fixture buffer is
 * not free -- the first version used 1 KiB apiece and pushed kernel BSS into
 * the user base at 0x800000, failing the build's collision check. */
#define AZF_CAP 384u
#define AZF_POLICY_FILL 0x9Eu       /* the byte PolicyGetDigest returns */
#define AZF_SESSION     0x03000000u
#define AZF_OBJECT      0x80000001u

static uint8_t  azf_cmd[AZF_CAP];
static uint32_t azf_cmd_len, azf_cmd_expect;
static uint8_t  azf_rsp[AZF_CAP];
static uint32_t azf_rsp_len, azf_rsp_pos;
static int      azf_ready, azf_executed;
static uint32_t azf_seen[48];
static uint32_t azf_seen_n;
static uint32_t azf_fail_cc, azf_fail_rc;
static uint64_t azf_counter;                 /* what an 8-byte NV_Read reports */
static uint8_t  azf_record[192];             /* what a record NV_Read reports */
static uint32_t azf_record_len;
static uint8_t  azf_key_name[34];            /* Name LoadExternal reports */
static uint32_t azf_writes;                  /* NV_Write commands executed */
static uint32_t azf_increments;              /* NV_Increment commands executed */
static uint32_t azf_flushes;                 /* FlushContext commands executed */
static uint32_t azf_loads;                   /* LoadExternal commands executed */
static int      azf_report_unwritten;        /* 1 = report WRITTEN clear (recreated) */
/* 1 = the COUNTER's NV_Read answers TPM_RC_NV_UNINITIALIZED while the RECORD
 * still reads normally. That is the shape a destroyed-and-recreated commit
 * counter has BEFORE the attacker increments it, and it cannot be produced with
 * the blanket azf_fail_cc control because both reads share one command code. */
static int      azf_counter_uninit;
/* Nonzero = NV_ReadPublic for THIS index answers NOTFOUND, i.e. the index is
 * DELETED rather than recreated. Distinct from azf_counter_uninit: a deleted
 * index fails the identity read, a recreated one passes it and fails the value
 * read, and they are different attacker moves against the same anchor. */
static uint32_t azf_absent_index;
static uint32_t azf_absent_index2;           /* a SECOND absent index */
/* Nonzero = report a public area for THIS index whose dataSize does not match
 * the compiled manifest, leaving every other index correct. The Name stays
 * self-consistent with the reported public area, so the Name check passes and
 * the CONTRACT comparison is what fails -- which is the only way to reach the
 * counter-identity match. Forcing NV_ReadPublic to fail outright (the absent
 * controls above) exits before that comparison and cannot pin it. */
static uint32_t azf_wrong_contract_index;
/* Nonzero = the RECORD's NV_Read answers this rc while its NV_ReadPublic still
 * succeeds. The two commands are separately failable on a real TPM (and on an
 * inconsistent one), and only this reaches the post-ReadPublic record-loss
 * path. */
static uint32_t azf_record_read_rc;
/* Advance the injected budget clock by this many ms at the START of every TPM
 * sequence (exactly one TPM2_CC_START_AUTH_SESSION each), so a fixture can make
 * a sequence "take" wall-clock time. A fake TPM answers in microseconds, so
 * without this the read path's between-sequence deadline guards can never fire
 * and deleting them stays green. */
static uint32_t azf_clock_step_ms;
/* PER-SEQUENCE steps, applied in order, used where a uniform step cannot reach
 * the state under test. Guard 2 protects `grant_ms -= spent_ms` from
 * underflowing, and a uniform step can only reach spent == grant exactly, where
 * the subtraction is 0 and the transport refuses anyway -- so the guard's
 * removal is invisible. Different steps per sequence reach spent > grant, where
 * the underflow hands the read a full budget it never earned. */
static uint32_t azf_clock_steps[4];
static uint32_t azf_clock_step_n;
static uint32_t azf_clock_ms;
/* A VALIDATING fixture, not a permissive one. A fake that answers SUCCESS to
 * every policy command cannot fail when the aHash construction, the key Name
 * framing or the counter-Name cpHash regress -- the control would still pass and
 * the test would be mirroring the implementation rather than constraining it.
 * These fields let the fake CHECK what it was told, and answer TPM_RC_POLICY_FAIL
 * when the binding does not hold. */
static uint8_t  azf_pinned_cphash[SHA256_DIGEST_LEN];
static int      azf_have_cphash;
static uint8_t  azf_verified_approved[SHA256_DIGEST_LEN];
static int      azf_have_ticket;
static uint8_t  azf_approved_write[SHA256_DIGEST_LEN];
static uint8_t  azf_approved_commit[SHA256_DIGEST_LEN];
static uint8_t  azf_policy_ref[4];
static uint32_t azf_rejects;                 /* bindings the fake refused */

#define AZF_RC_POLICY_FAIL 0x0000099Du

/* A MODELLED policy session: handle, type, and the running policyDigest.
 *
 * Without this the fake cannot tell a correct assertion sequence from an
 * omitted or reordered one -- PolicyAuthorize would accept whatever approved
 * policy it was handed, where a real TPM requires that value to EQUAL the
 * digest the session accumulated. The accumulation follows TPM 2.0 Part 1
 * section 19.7 ("policyDigest_new := H(policyDigest_old || PolicyAssertion)",
 * initialized to the zero digest) and section 19.7.5 for PolicyAuthorize's
 * reset-and-re-extend with the key Name.
 *
 * It is a MODEL and is labelled one on purpose: it constrains the ORDER,
 * PRESENCE and ARGUMENTS of the assertions, which is what this section's
 * correctness turns on. Byte-exact agreement with real firmware is a different
 * claim, and it is operator-gated -- there is no swtpm and no discrete TPM on
 * this host. */
#define AZF_MAX_SESSIONS 4u

struct azf_session {
    uint32_t handle;
    int      trial;
    uint8_t  digest[SHA256_DIGEST_LEN];
    int      live;
};

static struct azf_session azf_sess[AZF_MAX_SESSIONS];
static uint32_t azf_next_session;
/* Transient objects, tracked the same way. A leak assertion that counts total
 * flushes cannot tell a flushed authority key from a flushed trial session, so
 * the fake records LIVENESS per handle and the tests assert on that. */
static uint32_t azf_obj_handle;
static int      azf_obj_live;

static int azf_any_handle_live(void)
{
    uint32_t i;
    if (azf_obj_live) return 1;
    for (i = 0; i < AZF_MAX_SESSIONS; i++)
        if (azf_sess[i].live) return 1;
    return 0;
}

static struct azf_session *azf_find_session(uint32_t handle)
{
    uint32_t i;
    for (i = 0; i < AZF_MAX_SESSIONS; i++)
        if (azf_sess[i].live && azf_sess[i].handle == handle)
            return &azf_sess[i];
    return 0;
}

/* policyDigest_new := H(policyDigest_old || commandCode || args) */
static void azf_policy_extend(struct azf_session *s, uint32_t cc,
                              const uint8_t *args, uint32_t args_len)
{
    struct sha256_ctx c;
    uint8_t ccb[4];
    tpm2_be32_put(ccb, cc);
    sha256_init(&c);
    sha256_update(&c, s->digest, SHA256_DIGEST_LEN);
    sha256_update(&c, ccb, 4u);
    if (args_len)
        sha256_update(&c, args, args_len);
    sha256_final(&c, s->digest);
}

#define AZF_REG_STS   0x018u
#define AZF_REG_FIFO  0x024u
#define AZF_STS_EXPECT        0x08u
#define AZF_STS_DATA_AVAIL    0x10u
#define AZF_STS_GO            0x20u
#define AZF_STS_COMMAND_READY 0x40u
#define AZF_STS_VALID         0x80u

static int azf_saw(uint32_t cc)
{
    uint32_t i;
    for (i = 0; i < azf_seen_n; i++)
        if (azf_seen[i] == cc)
            return 1;
    return 0;
}

/* Header-only success for the assertion commands that return no parameters. */
static void azf_hdr_only(uint32_t rc)
{
    tpm2_be16_put(azf_rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(azf_rsp + 2, 10u);
    tpm2_be32_put(azf_rsp + 6, rc);
    azf_rsp_len = 10u;
}

/* A session-tagged success carrying `plen` parameter bytes already placed at
 * azf_rsp+14, plus the one TPMS_AUTH_RESPONSE a single-authorization command
 * answers with (nonceTPM TPM2B(2,0) + attrs(1) + hmac TPM2B(2,0)). */
static void azf_session_rsp(uint32_t rc, uint32_t plen)
{
    uint32_t k;
    tpm2_be16_put(azf_rsp + 0, TPM2_ST_SESSIONS);
    tpm2_be32_put(azf_rsp + 2, 10u + 4u + plen + 5u);
    tpm2_be32_put(azf_rsp + 6, rc);
    tpm2_be32_put(azf_rsp + 10, plen);
    for (k = 0; k < 5u; k++)
        azf_rsp[14 + plen + k] = 0u;
    azf_rsp_len = 10u + 4u + plen + 5u;
}

/* The public area the fake reports for an index, matching the compiled
 * manifest so the identity gate PASSES for a well-behaved TPM. TPMA_NV_WRITTEN
 * is added for the record indexes because their contracts are enrolled written;
 * a counter's WRITTEN stays clear until its first increment. */
static void azf_public_for(uint32_t idx, uint32_t *attrs, uint16_t *size)
{
    const struct tpm_enroll_entry *e = tpm_enroll_lookup(idx);
    if (!e) { *attrs = 0u; *size = 0u; return; }
    *attrs = e->attrs | ((e->expect_written && !azf_report_unwritten)
                         ? TPMA_NV_WRITTEN : 0u);
    *size = e->data_size;
    if (azf_wrong_contract_index != 0u && idx == azf_wrong_contract_index)
        *size = (uint16_t)(e->data_size + 8u);   /* not what the manifest enrolls */
}

/* The Name the fake REPORTS for an index. The independent cpHash check below
 * derives its expectation from this and from the command bytes actually
 * received, so a mismatch means the pinned cpHash does not describe the command
 * that ran -- which is exactly the counter-Name defect. */
static int azf_name_for(uint32_t idx, uint8_t *out, uint32_t cap)
{
    struct tpm_nv_public pub;
    uint32_t attrs, i;
    uint16_t dsz;
    azf_public_for(idx, &attrs, &dsz);
    memset(&pub, 0, sizeof pub);
    pub.name_alg = TPM_ALG_SHA256;
    pub.attrs = attrs;
    pub.data_size = dsz;
    pub.policy_len = (uint16_t)SHA256_DIGEST_LEN;
    for (i = 0; i < SHA256_DIGEST_LEN; i++)
        pub.auth_policy[i] = AZF_POLICY_FILL;
    return tpm2_nv_name_compute(idx, &pub, out, cap);
}

/* Recompute the cpHash of the command in hand and compare it with what
 * PolicyCpHash pinned. `params`/`params_len` come from the received command. */
static int azf_cphash_matches(uint32_t cc, uint32_t idx,
                              const uint8_t *params, uint32_t params_len)
{
    uint8_t names[2u * (2u + SHA256_DIGEST_LEN)], want[SHA256_DIGEST_LEN];
    int nl;
    uint32_t i, nlen;

    if (!azf_have_cphash)
        return 0;
    nl = azf_name_for(idx, names, (uint32_t)sizeof names / 2u);
    if (nl <= 0)
        return 0;
    nlen = (uint32_t)nl;
    for (i = 0; i < nlen; i++)
        names[nlen + i] = names[i];
    if (tpm2_cphash_compute(cc, names, nlen * 2u, params, params_len, want) != TPM_NV_OK)
        return 0;
    return memcmp(want, azf_pinned_cphash, SHA256_DIGEST_LEN) == 0;
}

static void azf_build_response(void)
{
    uint32_t cc = tpm2_be32_get(azf_cmd + 6);
    uint32_t rc = TPM2_RC_SUCCESS;
    if (azf_seen_n < 48u) azf_seen[azf_seen_n++] = cc;
    if (cc == TPM2_CC_START_AUTH_SESSION) {
        uint32_t step = azf_clock_step_ms;
        if (azf_clock_step_n < 4u && azf_clock_steps[azf_clock_step_n] != 0u)
            step = azf_clock_steps[azf_clock_step_n];
        if (azf_clock_step_n < 4u) azf_clock_step_n++;
        if (step != 0u) {
            azf_clock_ms += step;
            tpm_boot_budget_test_set_clock(1, azf_clock_ms);
        }
    }
    if (azf_fail_cc != 0u && cc == azf_fail_cc)
        rc = azf_fail_rc;
    memset(azf_rsp, 0, AZF_CAP);

    if (rc != TPM2_RC_SUCCESS) { azf_hdr_only(rc); return; }

    if (cc == TPM2_CC_NV_READ_PUBLIC) {
        /* TPM2B_NV_PUBLIC{ nvIndex nameAlg attrs authPolicy(TPM2B) dataSize }
         * followed by the REAL TPM2B_NAME of that public area, so the gate's
         * self-consistency check is genuinely exercised. */
        uint32_t idx = tpm2_be32_get(azf_cmd + 10);
        uint32_t attrs; uint16_t dsz;
        if ((azf_absent_index != 0u && idx == azf_absent_index) ||
            (azf_absent_index2 != 0u && idx == azf_absent_index2)) {
            azf_hdr_only(0x0000018Bu);   /* TPM_RC_HANDLE -> NOTFOUND */
            return;
        }
        uint32_t pl = SHA256_DIGEST_LEN, pubsize = 14u + pl, i;
        struct tpm_nv_public pub;
        uint8_t name[64];
        int nlen;

        azf_public_for(idx, &attrs, &dsz);
        tpm2_be16_put(azf_rsp + 10, (uint16_t)pubsize);
        tpm2_be32_put(azf_rsp + 12, idx);
        tpm2_be16_put(azf_rsp + 16, TPM_ALG_SHA256);
        tpm2_be32_put(azf_rsp + 18, attrs);
        tpm2_be16_put(azf_rsp + 22, (uint16_t)pl);
        for (i = 0; i < pl; i++)
            azf_rsp[24 + i] = AZF_POLICY_FILL;
        tpm2_be16_put(azf_rsp + 24 + pl, dsz);

        memset(&pub, 0, sizeof pub);
        pub.name_alg = TPM_ALG_SHA256;
        pub.attrs = attrs;
        pub.data_size = dsz;
        pub.policy_len = (uint16_t)pl;
        for (i = 0; i < pl; i++)
            pub.auth_policy[i] = AZF_POLICY_FILL;
        nlen = tpm2_nv_name_compute(idx, &pub, name, (uint32_t)sizeof name);
        if (nlen <= 0) nlen = 0;
        tpm2_be16_put(azf_rsp + 26 + pl, (uint16_t)nlen);
        for (i = 0; i < (uint32_t)nlen; i++)
            azf_rsp[28 + pl + i] = name[i];

        tpm2_be16_put(azf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(azf_rsp + 2, 10u + 2u + pubsize + 2u + (uint32_t)nlen);
        tpm2_be32_put(azf_rsp + 6, rc);
        azf_rsp_len = 10u + 2u + pubsize + 2u + (uint32_t)nlen;
        return;
    }
    if (cc == TPM2_CC_NV_READ) {
        /* NV_Read: header(10) + authHandle(4) + nvIndex(4) + authArea(13),
         * so the requested size lands at 31 and the offset at 33. Reading it
         * from 35 is one field past the end of the command, which made every
         * counter read look like a record read and fail its exact-length check
         * as a transport fault. */
        uint16_t want = tpm2_be16_get(azf_cmd + 31);
        uint32_t i;
        if (want == (uint16_t)TPM_NV_COUNTER_SIZE && azf_counter_uninit) {
            azf_hdr_only(TPM2_RC_NV_UNINITIALIZED);
            return;
        }
        if (want == (uint16_t)TPM_NV_COUNTER_SIZE) {
            tpm2_be16_put(azf_rsp + 14, 8u);
            for (i = 0; i < 8u; i++)
                azf_rsp[16 + i] = (uint8_t)((azf_counter >> (56u - 8u * i)) & 0xFFu);
            azf_session_rsp(rc, 10u);
        } else if (azf_record_read_rc != 0u) {
            azf_hdr_only(azf_record_read_rc);
            return;
        } else {
            uint32_t n = (want < azf_record_len) ? (uint32_t)want : azf_record_len;
            tpm2_be16_put(azf_rsp + 14, (uint16_t)n);
            for (i = 0; i < n; i++)
                azf_rsp[16 + i] = azf_record[i];
            azf_session_rsp(rc, 2u + n);
        }
        return;
    }
    if (cc == TPM2_CC_START_AUTH_SESSION) {
        /* sessionType sits after nonceCaller and the empty encryptedSalt:
         * header(10) + tpmKey(4) + bind(4) + TPM2B nonce(2+16) + TPM2B salt(2). */
        uint8_t stype = azf_cmd[38];
        uint32_t handle = AZF_SESSION + azf_next_session;
        struct azf_session *s = 0;
        uint32_t i;
        for (i = 0; i < AZF_MAX_SESSIONS; i++)
            if (!azf_sess[i].live) { s = &azf_sess[i]; break; }
        if (!s) { azf_hdr_only(AZF_RC_POLICY_FAIL); return; }
        memset(s, 0, sizeof *s);
        s->handle = handle;
        s->trial = (stype == TPM2_SE_TRIAL) ? 1 : 0;
        s->live = 1;
        azf_next_session++;
        tpm2_be16_put(azf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(azf_rsp + 2, 32u);
        tpm2_be32_put(azf_rsp + 6, rc);
        tpm2_be32_put(azf_rsp + 10, handle);
        tpm2_be16_put(azf_rsp + 14, 16u);
        azf_rsp_len = 32u;
        return;
    }
    if (cc == TPM2_CC_POLICY_COMMAND_CODE) {
        struct azf_session *s = azf_find_session(tpm2_be32_get(azf_cmd + 10));
        if (!s) { azf_rejects++; azf_hdr_only(AZF_RC_POLICY_FAIL); return; }
        azf_policy_extend(s, TPM2_CC_POLICY_COMMAND_CODE, azf_cmd + 14, 4u);
        azf_hdr_only(rc);
        return;
    }
    if (cc == TPM2_CC_FLUSH_CONTEXT) {
        uint32_t fh = tpm2_be32_get(azf_cmd + 10);
        struct azf_session *s = azf_find_session(fh);
        if (s) s->live = 0;
        if (azf_obj_live && fh == azf_obj_handle) azf_obj_live = 0;
        azf_flushes++;
        azf_hdr_only(rc);
        return;
    }
    if (cc == TPM2_CC_POLICY_GET_DIGEST) {
        uint32_t i;
        tpm2_be16_put(azf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(azf_rsp + 2, 44u);
        tpm2_be32_put(azf_rsp + 6, rc);
        tpm2_be16_put(azf_rsp + 10, 32u);
        for (i = 0; i < 32u; i++)
            azf_rsp[12 + i] = AZF_POLICY_FILL;
        azf_rsp_len = 44u;
        return;
    }
    if (cc == TPM2_CC_LOAD_EXTERNAL) {
        uint32_t i;
        azf_loads++;
        azf_obj_handle = AZF_OBJECT;
        azf_obj_live = 1;
        tpm2_be16_put(azf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(azf_rsp + 2, 10u + 4u + 2u + 34u);
        tpm2_be32_put(azf_rsp + 6, rc);
        tpm2_be32_put(azf_rsp + 10, AZF_OBJECT);
        tpm2_be16_put(azf_rsp + 14, 34u);
        for (i = 0; i < 34u; i++)
            azf_rsp[16 + i] = azf_key_name[i];
        azf_rsp_len = 10u + 4u + 2u + 34u;
        return;
    }
    if (cc == TPM2_CC_VERIFY_SIGNATURE) {
        /* The digest MUST be aHash = H(approvedPolicy || policyRef) for one of
         * the two grants. A fake that ticketed anything could not fail when the
         * aHash construction regressed, which is the whole reason the previous
         * round could not catch it. */
        struct sha256_ctx ah;
        uint8_t want_w[SHA256_DIGEST_LEN], want_c[SHA256_DIGEST_LEN];
        const uint8_t *got = azf_cmd + 16;
        sha256_init(&ah);
        sha256_update(&ah, azf_approved_write, SHA256_DIGEST_LEN);
        sha256_update(&ah, azf_policy_ref, sizeof azf_policy_ref);
        sha256_final(&ah, want_w);
        sha256_init(&ah);
        sha256_update(&ah, azf_approved_commit, SHA256_DIGEST_LEN);
        sha256_update(&ah, azf_policy_ref, sizeof azf_policy_ref);
        sha256_final(&ah, want_c);
        if (memcmp(got, want_w, SHA256_DIGEST_LEN) == 0) {
            memcpy(azf_verified_approved, azf_approved_write, SHA256_DIGEST_LEN);
            azf_have_ticket = 1;
        } else if (memcmp(got, want_c, SHA256_DIGEST_LEN) == 0) {
            memcpy(azf_verified_approved, azf_approved_commit, SHA256_DIGEST_LEN);
            azf_have_ticket = 1;
        } else {
            azf_rejects++;
            azf_hdr_only(AZF_RC_POLICY_FAIL);
            return;
        }
        /* TPMT_TK_VERIFIED: tag || hierarchy || empty digest. */
        tpm2_be16_put(azf_rsp + 0, TPM2_ST_NO_SESSIONS);
        tpm2_be32_put(azf_rsp + 2, 18u);
        tpm2_be32_put(azf_rsp + 6, rc);
        tpm2_be16_put(azf_rsp + 10, TPM2_ST_VERIFIED);
        tpm2_be32_put(azf_rsp + 12, TPM_RH_OWNER);
        tpm2_be16_put(azf_rsp + 16, 0u);
        azf_rsp_len = 18u;
        return;
    }
    if (cc == TPM2_CC_POLICY_CP_HASH) {
        struct azf_session *s = azf_find_session(tpm2_be32_get(azf_cmd + 10));
        if (!s) { azf_rejects++; azf_hdr_only(AZF_RC_POLICY_FAIL); return; }
        memcpy(azf_pinned_cphash, azf_cmd + 16, SHA256_DIGEST_LEN);
        azf_have_cphash = 1;
        azf_policy_extend(s, TPM2_CC_POLICY_CP_HASH, azf_cmd + 16,
                          SHA256_DIGEST_LEN);
        azf_hdr_only(rc);
        return;
    }
    if (cc == TPM2_CC_POLICY_NV) {
        /* The operand must be the counter value the grant was issued against,
         * big-endian. Accepting any operand would let a grant be replayed at a
         * different generation, which is the replay this assertion pins. */
        uint8_t want[8];
        uint32_t i;
        for (i = 0; i < 8u; i++)
            want[i] = (uint8_t)((azf_counter >> (56u - 8u * i)) & 0xFFu);
        if (memcmp(azf_cmd + 37, want, 8u) != 0) {
            azf_rejects++;
            azf_hdr_only(AZF_RC_POLICY_FAIL);
            return;
        }
        {
            struct azf_session *s = azf_find_session(tpm2_be32_get(azf_cmd + 18));
            uint8_t args[SHA256_DIGEST_LEN + 64], nm[64];
            struct sha256_ctx ac;
            int nl;
            if (!s) { azf_rejects++; azf_hdr_only(AZF_RC_POLICY_FAIL); return; }
            /* args := H(operandB || offset || operation) || nvIndex Name, so
             * changing the OPERATION or the index changes the digest. */
            sha256_init(&ac);
            sha256_update(&ac, azf_cmd + 37, 8u);
            sha256_update(&ac, azf_cmd + 45, 2u);
            sha256_update(&ac, azf_cmd + 47, 2u);
            sha256_final(&ac, args);
            nl = azf_name_for(tpm2_be32_get(azf_cmd + 14), nm, (uint32_t)sizeof nm);
            if (nl <= 0) { azf_rejects++; azf_hdr_only(AZF_RC_POLICY_FAIL); return; }
            memcpy(args + SHA256_DIGEST_LEN, nm, (uint32_t)nl);
            azf_policy_extend(s, TPM2_CC_POLICY_NV, args,
                              SHA256_DIGEST_LEN + (uint32_t)nl);
        }
        azf_session_rsp(rc, 0u);
        return;
    }
    if (cc == TPM2_CC_POLICY_AUTHORIZE) {
        /* A real TPM requires approvedPolicy to EQUAL the digest the session
         * has accumulated, and that equality is the only thing that makes an
         * omitted or reordered assertion detectable. A trial session is
         * identified by its recorded TYPE, not by the value it happens to
         * carry: inferring "trial" from a zero digest would let a real session
         * that asserted nothing pass as one. */
        struct azf_session *s = azf_find_session(tpm2_be32_get(azf_cmd + 10));
        uint16_t alen = tpm2_be16_get(azf_cmd + 14);
        uint16_t rlen2, klen;
        uint32_t koff;
        if (!s || alen != SHA256_DIGEST_LEN) {
            azf_rejects++; azf_hdr_only(AZF_RC_POLICY_FAIL); return;
        }
        if (!s->trial) {
            if (!azf_have_ticket ||
                memcmp(azf_cmd + 16, azf_verified_approved, SHA256_DIGEST_LEN) != 0 ||
                memcmp(azf_cmd + 16, s->digest, SHA256_DIGEST_LEN) != 0) {
                azf_rejects++; azf_hdr_only(AZF_RC_POLICY_FAIL); return;
            }
        }
        rlen2 = tpm2_be16_get(azf_cmd + 16 + alen);
        koff = 16u + (uint32_t)alen + 2u + (uint32_t)rlen2;
        klen = tpm2_be16_get(azf_cmd + koff);
        if (rlen2 != (uint16_t)sizeof azf_policy_ref ||
            memcmp(azf_cmd + 18 + alen, azf_policy_ref, rlen2) != 0 ||
            klen != 34u ||
            memcmp(azf_cmd + koff + 2u, azf_key_name, 34u) != 0) {
            azf_rejects++; azf_hdr_only(AZF_RC_POLICY_FAIL); return;
        }
        /* Satisfied: reset to zero and re-extend with the key Name, then the
         * policyRef (Part 1 section 19.7.5). */
        memset(s->digest, 0, SHA256_DIGEST_LEN);
        azf_policy_extend(s, TPM2_CC_POLICY_AUTHORIZE,
                          azf_cmd + koff + 2u, 34u);
        {
            struct sha256_ctx rc2;
            sha256_init(&rc2);
            sha256_update(&rc2, s->digest, SHA256_DIGEST_LEN);
            sha256_update(&rc2, azf_policy_ref, sizeof azf_policy_ref);
            sha256_final(&rc2, s->digest);
        }
        azf_hdr_only(rc);
        return;
    }
    if (cc == TPM2_CC_NV_WRITE) {
        /* The final command must be authorized by the policy session that was
         * just built: a real TPM checks the session's policyDigest against the
         * index authPolicy. A command authorized with TPM_RS_PW, with a trial
         * session, or with an unrelated handle would otherwise pass this suite
         * and fail on firmware. */
        {
            uint32_t sh = tpm2_be32_get(azf_cmd + 22);
            struct azf_session *as = azf_find_session(sh);
            if (!as || as->trial) {
                azf_rejects++;
                azf_hdr_only(AZF_RC_POLICY_FAIL);
                return;
            }
        }
        /* The pinned cpHash must describe THIS command. Derived from the bytes
         * actually received and from the Name this fake itself reported, so a
         * cpHash built over the wrong index's Name is caught here rather than
         * passing as a green control. */
        uint32_t widx = tpm2_be32_get(azf_cmd + 14);
        uint16_t dlen = tpm2_be16_get(azf_cmd + 31);
        uint8_t params[2u + TPM_NV_MAX_DATA + 2u];
        uint32_t plen2 = 0, q;
        if ((uint32_t)dlen + 4u > sizeof params) { azf_hdr_only(AZF_RC_POLICY_FAIL); return; }
        tpm2_be16_put(params, dlen); plen2 = 2u;
        for (q = 0; q < dlen; q++) params[plen2 + q] = azf_cmd[33 + q];
        plen2 += dlen;
        tpm2_be16_put(params + plen2, tpm2_be16_get(azf_cmd + 33 + dlen));
        plen2 += 2u;
        if (!azf_cphash_matches(TPM2_CC_NV_WRITE, widx, params, plen2)) {
            azf_rejects++;
            azf_hdr_only(AZF_RC_POLICY_FAIL);
            return;
        }
        /* STORE the bytes, the way a real device does. A fake that acknowledged
         * a write without keeping it would fail the implementation's readback
         * for a reason the implementation is right about, and the fixture would
         * be asserting its own defect. Layout matches NV_Read: the data TPM2B
         * sits at 31 after the 13-byte auth area. */
        uint16_t wlen = tpm2_be16_get(azf_cmd + 31);
        uint32_t i;
        if (wlen > sizeof azf_record) wlen = (uint16_t)sizeof azf_record;
        for (i = 0; i < wlen; i++)
            azf_record[i] = azf_cmd[33 + i];
        azf_record_len = wlen;
        azf_writes++;
        azf_session_rsp(rc, 0u);
        return;
    }
    if (cc == TPM2_CC_NV_INCREMENT) {
        /* The final command must be authorized by the policy session that was
         * just built: a real TPM checks the session's policyDigest against the
         * index authPolicy. A command authorized with TPM_RS_PW, with a trial
         * session, or with an unrelated handle would otherwise pass this suite
         * and fail on firmware. */
        {
            uint32_t sh = tpm2_be32_get(azf_cmd + 22);
            struct azf_session *as = azf_find_session(sh);
            if (!as || as->trial) {
                azf_rejects++;
                azf_hdr_only(AZF_RC_POLICY_FAIL);
                return;
            }
        }
        /* NV_Increment carries the counter handle twice and no parameters, so
         * its cpHash must be over the COUNTER's Name. Reusing the record
         * index's Name here was a real defect; this is the assertion that
         * would have caught it. */
        uint32_t cidx = tpm2_be32_get(azf_cmd + 14);
        if (!azf_cphash_matches(TPM2_CC_NV_INCREMENT, cidx, 0, 0u)) {
            azf_rejects++;
            azf_hdr_only(AZF_RC_POLICY_FAIL);
            return;
        }
        /* And ADVANCE the counter, so a read after the commit reports the
         * generation the record was bound to rather than the pre-commit one. */
        azf_counter++;
        azf_increments++;
        azf_session_rsp(rc, 0u);
        return;
    }
    /* PolicyCommandCode, PolicyCpHash, PolicyAuthorize: no parameters. */
    azf_hdr_only(rc);
}

static uint8_t azf_r8(uint32_t off)
{
    if (off == AZF_REG_FIFO && azf_executed && azf_rsp_pos < azf_rsp_len)
        return azf_rsp[azf_rsp_pos++];
    return 0;
}

static void azf_w8(uint32_t off, uint8_t v)
{
    if (off != AZF_REG_FIFO || azf_executed)
        return;
    if (azf_cmd_len < AZF_CAP)
        azf_cmd[azf_cmd_len] = v;
    azf_cmd_len++;
    if (azf_cmd_len == 6u)
        azf_cmd_expect = tpm2_be32_get(azf_cmd + 2);
}

static uint32_t azf_r32(uint32_t off)
{
    uint8_t sts;
    if (off != AZF_REG_STS)
        return 0;
    sts = AZF_STS_VALID;
    /* commandReady is reported only while the fake is genuinely idle. Leaving
     * it asserted after a command has been written is what makes the transport
     * conclude the device never accepted it, and every operation then reports a
     * transport fault that looks like a code defect rather than a fixture one. */
    if (azf_ready && !azf_executed && azf_cmd_len == 0u)
        sts |= AZF_STS_COMMAND_READY;
    if (!azf_executed && azf_cmd_len > 0u && azf_cmd_len < azf_cmd_expect)
        sts |= AZF_STS_EXPECT;
    if (azf_executed && azf_rsp_pos < azf_rsp_len)
        sts |= AZF_STS_DATA_AVAIL;
    return (uint32_t)sts | (32u << 8);   /* burstCount = 32 */
}

static void azf_w32(uint32_t off, uint32_t v)
{
    if (off != AZF_REG_STS)
        return;
    if (v & AZF_STS_COMMAND_READY) {
        azf_ready = 1;
        azf_executed = 0;
        azf_cmd_len = 0;
        azf_cmd_expect = 0;
        azf_rsp_pos = 0;
    }
    if ((v & AZF_STS_GO) && azf_cmd_len >= azf_cmd_expect) {
        azf_executed = 1;
        azf_build_response();
    }
}

static const struct tpm_t_io azf_io = { azf_r8, azf_w8, azf_r32, azf_w32 };

/* A minimal TPMT_PUBLIC for the authority, plus the Name the fake will echo. */
static uint8_t azf_pub[16];

/* How many TPM SEQUENCES ran: exactly one TPM2_CC_START_AUTH_SESSION each. It
 * is the only observable that separates "refused after the first derivation"
 * from "refused after the second", which both report TPM_NV_BUDGET -- so
 * without it a mutation removing either between-sequence guard hides behind the
 * other and the suite stays green. */
static uint32_t azf_sequences(void)
{
    uint32_t i, n = 0u;
    for (i = 0u; i < azf_seen_n; i++)
        if (azf_seen[i] == TPM2_CC_START_AUTH_SESSION) n++;
    return n;
}

static void azf_reset(uint32_t fail_cc, uint32_t fail_rc)
{
    uint8_t digest[SHA256_DIGEST_LEN];
    azf_cmd_len = 0; azf_cmd_expect = 0;
    azf_rsp_len = 0; azf_rsp_pos = 0;
    azf_ready = 0; azf_executed = 0;
    azf_seen_n = 0;
    azf_fail_cc = fail_cc; azf_fail_rc = fail_rc;
    azf_counter = 0u;
    azf_record_len = 0u;
    azf_writes = 0; azf_increments = 0; azf_flushes = 0; azf_loads = 0;
    azf_report_unwritten = 0;
    azf_counter_uninit = 0;
    azf_absent_index = 0u;
    azf_absent_index2 = 0u;
    azf_wrong_contract_index = 0u;
    azf_record_read_rc = 0u;
    azf_clock_step_ms = 0u;
    azf_clock_steps[0] = azf_clock_steps[1] = 0u;
    azf_clock_steps[2] = azf_clock_steps[3] = 0u;
    azf_clock_step_n = 0u;
    azf_clock_ms = 0u;
    azf_have_cphash = 0; azf_have_ticket = 0; azf_rejects = 0;
    memset(azf_sess, 0, sizeof azf_sess);
    azf_next_session = 0;
    azf_obj_handle = 0; azf_obj_live = 0;
    memset(azf_pinned_cphash, 0, sizeof azf_pinned_cphash);
    memset(azf_verified_approved, 0, sizeof azf_verified_approved);
    memset(azf_approved_write, 0xA1, sizeof azf_approved_write);
    memset(azf_approved_commit, 0xB2, sizeof azf_approved_commit);
    azf_policy_ref[0] = 'I'; azf_policy_ref[1] = 'P';
    azf_policy_ref[2] = 'O'; azf_policy_ref[3] = 'S';

    memset(azf_pub, 0x5C, sizeof azf_pub);
    tpm2_be16_put(azf_pub + 0, 0x0001u);
    tpm2_be16_put(azf_pub + 2, TPM_ALG_SHA256);
    sha256(azf_pub, sizeof azf_pub, digest);
    tpm2_be16_put(azf_key_name, TPM_ALG_SHA256);
    memcpy(azf_key_name + 2, digest, SHA256_DIGEST_LEN);
}

static void azf_fill_grant(struct tpm_authz_transition *tr,
                           uint8_t *approved_w, uint8_t *approved_c, uint8_t *sig);

/* Compute the approved policies an authority would have to sign for ONE
 * transition, by running the SAME accumulation the fake performs. This is what
 * makes the grants real: if the implementation omits an assertion, reorders
 * them, or builds a cpHash over the wrong index's Name, the digest it presents
 * to PolicyAuthorize will not equal what the fake accumulated and the
 * transition is refused. */
static void azf_expected_policies(uint32_t idx, uint32_t counter_idx,
                                  uint64_t counter,
                                  const uint8_t *record, uint16_t rlen,
                                  uint8_t *out_write, uint8_t *out_commit)
{
    struct azf_session s;
    uint8_t names[2u * (2u + SHA256_DIGEST_LEN)];
    uint8_t params[2u + TPM_NV_MAX_DATA + 2u];
    uint8_t cph[SHA256_DIGEST_LEN];
    uint8_t nvargs[SHA256_DIGEST_LEN + 64], nm[64], operand[8];
    uint8_t ccbuf[4];
    struct sha256_ctx ac;
    uint32_t i, nlen, plen;
    int nl;

    for (i = 0; i < 8u; i++)
        operand[i] = (uint8_t)((counter >> (56u - 8u * i)) & 0xFFu);

    /* PolicyNV's args are the same for both halves: the generation this
     * transition was authorized against. */
    sha256_init(&ac);
    sha256_update(&ac, operand, 8u);
    { uint8_t two[2] = { 0, 0 }; sha256_update(&ac, two, 2u); }
    { uint8_t op[2]; tpm2_be16_put(op, TPM2_EO_EQ); sha256_update(&ac, op, 2u); }
    sha256_final(&ac, nvargs);
    nl = azf_name_for(counter_idx, nm, (uint32_t)sizeof nm);
    memcpy(nvargs + SHA256_DIGEST_LEN, nm, (uint32_t)nl);

    /* --- the WRITE half --- */
    memset(&s, 0, sizeof s);
    tpm2_be32_put(ccbuf, TPM2_CC_NV_WRITE);
    azf_policy_extend(&s, TPM2_CC_POLICY_COMMAND_CODE, ccbuf, 4u);
    nlen = (uint32_t)azf_name_for(idx, names, (uint32_t)sizeof names / 2u);
    for (i = 0; i < nlen; i++)
        names[nlen + i] = names[i];
    tpm2_be16_put(params, rlen); plen = 2u;
    for (i = 0; i < rlen; i++) params[plen + i] = record[i];
    plen += rlen;
    tpm2_be16_put(params + plen, 0u); plen += 2u;   /* offset */
    (void)tpm2_cphash_compute(TPM2_CC_NV_WRITE, names, nlen * 2u, params, plen, cph);
    azf_policy_extend(&s, TPM2_CC_POLICY_CP_HASH, cph, SHA256_DIGEST_LEN);
    azf_policy_extend(&s, TPM2_CC_POLICY_NV, nvargs,
                      SHA256_DIGEST_LEN + (uint32_t)nl);
    memcpy(out_write, s.digest, SHA256_DIGEST_LEN);

    /* --- the COMMIT half: a different command over a different Name --- */
    memset(&s, 0, sizeof s);
    tpm2_be32_put(ccbuf, TPM2_CC_NV_INCREMENT);
    azf_policy_extend(&s, TPM2_CC_POLICY_COMMAND_CODE, ccbuf, 4u);
    nlen = (uint32_t)azf_name_for(counter_idx, names, (uint32_t)sizeof names / 2u);
    for (i = 0; i < nlen; i++)
        names[nlen + i] = names[i];
    (void)tpm2_cphash_compute(TPM2_CC_NV_INCREMENT, names, nlen * 2u, 0, 0u, cph);
    azf_policy_extend(&s, TPM2_CC_POLICY_CP_HASH, cph, SHA256_DIGEST_LEN);
    azf_policy_extend(&s, TPM2_CC_POLICY_NV, nvargs,
                      SHA256_DIGEST_LEN + (uint32_t)nl);
    memcpy(out_commit, s.digest, SHA256_DIGEST_LEN);
}

/* Arm the fake and the grant for an advance of the A/B floor from `counter`
 * to `new_version`, so the authority's approved policies describe exactly the
 * transition the implementation is about to attempt. */
static void azf_arm_floor_advance(struct tpm_authz_transition *tr,
                                  uint8_t *aw, uint8_t *ac_, uint8_t *sig,
                                  uint64_t counter, uint32_t new_version)
{
    uint8_t next[AZ_FLOOR_LEN];
    (void)az_build_floor(next, AZ_FLOOR_LEN, counter + 1u, new_version);
    azf_expected_policies(TPM_NV_INDEX_AB_FLOOR, TPM_NV_INDEX_AB_SEQ, counter,
                          next, (uint16_t)AZ_FLOOR_LEN,
                          azf_approved_write, azf_approved_commit);
    azf_fill_grant(tr, aw, ac_, sig);
}

static void azf_install_authority(void)
{
    struct tpm_authz_authority a;
    static const uint8_t ref[4] = { 'I', 'P', 'O', 'S' };
    memset(&a, 0, sizeof a);
    a.public_area = azf_pub;
    a.public_len = (uint16_t)sizeof azf_pub;
    a.policy_ref = ref;
    a.policy_ref_len = (uint16_t)sizeof ref;
    (void)tpm_authz_set_authority(&a);
}

/* DISTINCT grants. The write and the commit assert different command codes and
 * different cpHashes, so their approved policies necessarily differ; giving
 * both the same policy modelled a transition no authority could ever issue. */
static void azf_fill_grant(struct tpm_authz_transition *tr,
                           uint8_t *approved_w, uint8_t *approved_c, uint8_t *sig)
{
    memcpy(approved_w, azf_approved_write, SHA256_DIGEST_LEN);
    memcpy(approved_c, azf_approved_commit, SHA256_DIGEST_LEN);
    memset(sig, 0x77, 8u);
    tr->write.approved_policy = approved_w;
    tr->write.approved_len = (uint16_t)SHA256_DIGEST_LEN;
    tr->write.signature = sig;
    tr->write.sig_len = 8u;
    tr->commit.approved_policy = approved_c;
    tr->commit.approved_len = (uint16_t)SHA256_DIGEST_LEN;
    tr->commit.signature = sig;
    tr->commit.sig_len = 8u;
}

/* ---- end-to-end: the authorized transition, and the attacks ---- */

static void test_authz_e2e_contract_derivation(void)
{
    struct tpm_t_test_state prev;
    struct tpm_nv_identity id;
    tpm_nv_status_t st;

    /* The contract's authPolicy is computed BY THE TPM via a trial session
     * rather than by reproducing the policyDigest formula in kernel code: a
     * formula that is subtly wrong produces an index nobody can ever satisfy,
     * and the mistake only shows up on real firmware. */
    azf_reset(0u, 0u);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_authz_contract(TPM_NV_INDEX_AB_FLOOR, &id);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "the contract derives from the manifest");
    TEST_ASSERT_EQ((int)id.nv_index, (int)TPM_NV_INDEX_AB_FLOOR, "contract names the index");
    TEST_ASSERT_EQ((int)id.data_size, (int)AZ_FLOOR_LEN, "contract sizes the record");
    TEST_ASSERT_EQ((int)id.policy_len, 32, "an authPolicy was computed");
    TEST_ASSERT_EQ((int)id.auth_policy[0], (int)AZF_POLICY_FILL,
                   "the authPolicy is the digest the TPM reported");
    TEST_ASSERT(azf_saw(TPM2_CC_START_AUTH_SESSION), "a trial session was opened");
    TEST_ASSERT(azf_saw(TPM2_CC_POLICY_AUTHORIZE), "PolicyAuthorize shaped the digest");
    TEST_ASSERT(azf_saw(TPM2_CC_POLICY_GET_DIGEST), "the digest was read back");
    TEST_ASSERT(azf_saw(TPM2_CC_FLUSH_CONTEXT), "the trial session was flushed");
}

static void test_authz_e2e_authorized_advance(void)
{
    struct tpm_t_test_state prev;
    struct tpm_authz_transition tr;
    uint8_t approved_w[SHA256_DIGEST_LEN], approved_c[SHA256_DIGEST_LEN], sig[8];
    tpm_nv_status_t st;

    /* CONTROL, and the most important assertion in the file: a correctly
     * authorized advance SUCCEEDS. Without it every refusal below would pass
     * against an implementation that refuses everything. */
    azf_reset(0u, 0u);
    memset(&tr, 0, sizeof tr);
    azf_counter = 40u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 40u, 100u);
    azf_arm_floor_advance(&tr, approved_w, approved_c, sig, 40u, 200u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_advance(200u, &tr);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "CONTROL: an authorized advance succeeds");
    TEST_ASSERT(azf_saw(TPM2_CC_LOAD_EXTERNAL), "the authority key was loaded");
    TEST_ASSERT(azf_saw(TPM2_CC_VERIFY_SIGNATURE), "the signature became a ticket");
    TEST_ASSERT(azf_saw(TPM2_CC_POLICY_CP_HASH), "the write was pinned by cpHash");
    TEST_ASSERT(azf_saw(TPM2_CC_POLICY_NV), "the generation was pinned by PolicyNV");
    TEST_ASSERT(azf_saw(TPM2_CC_POLICY_AUTHORIZE), "the authority satisfied the policy");
    TEST_ASSERT_EQ((int)azf_writes, 1, "exactly one record write");
    TEST_ASSERT_EQ((int)azf_increments, 1, "exactly one counter increment");
    /* Write BEFORE commit: the irreversible half is last, over bytes already on
     * the device and already read back. */
    TEST_ASSERT(azf_flushes >= 3u, "every session and loaded key was flushed");
}

static void test_authz_e2e_write_then_increment_order(void)
{
    struct tpm_t_test_state prev;
    struct tpm_authz_transition tr;
    uint8_t approved_w[SHA256_DIGEST_LEN], approved_c[SHA256_DIGEST_LEN], sig[8];
    uint32_t i, write_at = 0xFFFFu, inc_at = 0xFFFFu;

    azf_reset(0u, 0u);
    memset(&tr, 0, sizeof tr);
    azf_counter = 7u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 7u, 1u);
    azf_arm_floor_advance(&tr, approved_w, approved_c, sig, 7u, 2u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    (void)tpm_ab_floor_advance(2u, &tr);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    for (i = 0; i < azf_seen_n; i++) {
        if (azf_seen[i] == TPM2_CC_NV_WRITE && write_at == 0xFFFFu) write_at = i;
        if (azf_seen[i] == TPM2_CC_NV_INCREMENT && inc_at == 0xFFFFu) inc_at = i;
    }
    TEST_ASSERT(write_at != 0xFFFFu, "the record was written");
    TEST_ASSERT(inc_at != 0xFFFFu, "the counter was incremented");
    /* The ordering IS the design: increment-then-write would advance
     * irreversible state before the authorized bytes exist. */
    TEST_ASSERT(write_at < inc_at, "the write precedes the commit increment");
    /* And the readback sits between them, so nothing is committed over bytes
     * the device did not actually store. */
    {
        uint32_t read_after_write = 0;
        for (i = write_at + 1u; i < inc_at; i++)
            if (azf_seen[i] == TPM2_CC_NV_READ) read_after_write = 1;
        TEST_ASSERT_EQ((int)read_after_write, 1,
                       "the record is read back before the commit");
    }
}

static void test_authz_e2e_stale_generation_refused(void)
{
    struct tpm_t_test_state prev;
    struct tpm_authz_transition tr;
    uint8_t approved_w[SHA256_DIGEST_LEN], approved_c[SHA256_DIGEST_LEN], sig[8];
    tpm_nv_status_t st;

    /* A grant is signed for ONE transition. If the counter moved since it was
     * issued, this is a different transition and the grant does not cover it --
     * a REFUSAL, never a retry at the new value. */
    azf_reset(0u, 0u);
    memset(&tr, 0, sizeof tr);
    azf_fill_grant(&tr, approved_w, approved_c, sig);
    azf_counter = 9u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 9u, 5u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR, TPM_NV_INDEX_AB_SEQ,
                                azf_record, (uint16_t)AZ_FLOOR_LEN,
                                4u /* the grant assumed generation 4 */, &tr);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "a grant for a different generation is refused");
    TEST_ASSERT_EQ((int)azf_writes, 0, "nothing was written");
    TEST_ASSERT_EQ((int)azf_increments, 0, "the counter did not move");
}

static void test_authz_e2e_uncommitted_record_not_current(void)
{
    struct tpm_t_test_state prev;
    uint32_t version = 0;
    uint64_t gen = 0;
    tpm_nv_status_t st;

    /* CONTROL: a record matching the counter reads as the floor. */
    azf_reset(0u, 0u);
    azf_counter = 12u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 12u, 300u);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_read(&version, &gen);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "CONTROL: a committed floor reads back");
    TEST_ASSERT_EQ((int)version, 300, "the committed version is reported");
    TEST_ASSERT_EQ((int)gen, 12, "the committed generation is reported");

    /* The write-then-increment window: the record is one AHEAD of the counter,
     * so the transition was authorized and written but never committed. The
     * committed value is still the old one, and a half-finished update must not
     * move the floor in either direction. */
    azf_reset(0u, 0u);
    azf_counter = 12u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 13u, 900u);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_read(&version, &gen);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "an uncommitted record is NOT the floor");
}

static void test_authz_e2e_torn_pairing_detected(void)
{
    struct tpm_t_test_state prev;
    struct tpm_ab_floor_view view;
    tpm_nv_status_t st;

    /* CONTROL FIRST. Without it a detector that fired on every state would pass
     * the torn assertion below on its own. */
    azf_reset(0u, 0u);
    azf_counter = 30u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 30u, 400u);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_read_view(&view);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "CONTROL: an intact pairing verifies cleanly");
    TEST_ASSERT_EQ((int)view.pairing, (int)TPM_PAIRING_CURRENT,
                   "CONTROL: the intact pairing is CURRENT");
    TEST_ASSERT_EQ((int)view.version_valid, 1, "CONTROL: the version is published");
    TEST_ASSERT_EQ((int)view.version, 400, "CONTROL: the committed version is reported");

    /* THE TORN PAIRING: the commit counter advanced with no matching record
     * behind it. Detected rather than trusted -- treating the advance as
     * evidence the record is current is exactly how a rollback is laundered. */
    azf_reset(0u, 0u);
    azf_counter = 31u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 30u, 400u);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_read_view(&view);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "a torn pairing is not usable");
    TEST_ASSERT_EQ((int)view.pairing, (int)TPM_PAIRING_TORN,
                   "the counter-ahead direction is reported as TORN");
    TEST_ASSERT_EQ((int)view.committed_generation, 31, "the counter value is reported");
    TEST_ASSERT_EQ((int)view.record_generation, 30, "the record generation is reported");
    /* NO usable value is published. The authorized write overwrote the sole
     * record in place, so there is no committed value left to fall back on --
     * publishing one here would hand a caller bytes no authority committed. */
    TEST_ASSERT_EQ((int)view.version_valid, 0,
                   "a torn pairing publishes NO version");
    TEST_ASSERT_EQ((int)view.version, 0, "and the version field stays zero");

    /* The opposite direction stays distinguishable from it. */
    azf_reset(0u, 0u);
    azf_counter = 30u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 31u, 900u);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_read_view(&view);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "an uncommitted record is not usable");
    TEST_ASSERT_EQ((int)view.pairing, (int)TPM_PAIRING_UNCOMMITTED,
                   "the record-ahead direction is UNCOMMITTED, not TORN");
    TEST_ASSERT_EQ((int)view.version_valid, 0,
                   "an uncommitted record publishes NO version either");

    /* And the STRICT wrapper's contract is unchanged: shipped consumers
     * classify on MISMATCH and must keep seeing exactly that. */
    {
        uint32_t version = 0xFFFFFFFFu;
        azf_reset(0u, 0u);
        azf_counter = 31u;
        azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 30u, 400u);
        prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
        azf_install_authority();
        st = tpm_ab_floor_read(&version, 0);
        tpm_authz_test_clear_authority();
        tpm_t_test_restore(prev);
        TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                       "strict reader still reports MISMATCH for a torn pairing");
    }
}

static void test_authz_floor_read_plan_matches_sequence(void)
{
    uint8_t plan[64], direct[64];
    uint32_t pn, dn;

    /* The loader has no tpm2_submit() and must reach these same bytes through
     * EFI_TCG2 SubmitCommand. Asserting the published plan against the builders
     * the in-sequence path uses is what stops the two from drifting once the
     * loader has its own executor -- a comment could not. */
    pn = tpm_ab_floor_read_plan(TPM_FLOOR_STEP_COUNTER_PUBLIC, plan, sizeof plan);
    dn = tpm2_build_nv_read_public(direct, sizeof direct, TPM_NV_INDEX_AB_SEQ);
    TEST_ASSERT_EQ((int)pn, (int)dn, "counter ReadPublic length matches the sequence path");
    TEST_ASSERT_EQ(memcmp(plan, direct, pn), 0, "counter ReadPublic bytes match");

    pn = tpm_ab_floor_read_plan(TPM_FLOOR_STEP_COUNTER_READ, plan, sizeof plan);
    dn = tpm2_build_nv_read(direct, sizeof direct, TPM_RH_OWNER, TPM_NV_INDEX_AB_SEQ,
                            TPM_RS_PW, (uint16_t)TPM_NV_COUNTER_SIZE, 0u);
    TEST_ASSERT_EQ((int)pn, (int)dn, "counter NV_Read length matches the sequence path");
    TEST_ASSERT_EQ(memcmp(plan, direct, pn), 0, "counter NV_Read bytes match");

    pn = tpm_ab_floor_read_plan(TPM_FLOOR_STEP_RECORD_PUBLIC, plan, sizeof plan);
    dn = tpm2_build_nv_read_public(direct, sizeof direct, TPM_NV_INDEX_AB_FLOOR);
    TEST_ASSERT_EQ((int)pn, (int)dn, "record ReadPublic length matches the sequence path");
    TEST_ASSERT_EQ(memcmp(plan, direct, pn), 0, "record ReadPublic bytes match");

    pn = tpm_ab_floor_read_plan(TPM_FLOOR_STEP_RECORD_READ, plan, sizeof plan);
    dn = tpm2_build_nv_read(direct, sizeof direct, TPM_RH_OWNER, TPM_NV_INDEX_AB_FLOOR,
                            TPM_RS_PW, (uint16_t)TPM_AB_FLOOR_RECORD_LEN, 0u);
    TEST_ASSERT_EQ((int)pn, (int)dn, "record NV_Read length matches the sequence path");
    TEST_ASSERT_EQ(memcmp(plan, direct, pn), 0, "record NV_Read bytes match");

    /* The two indices must not collapse to the same command: a plan that
     * marshalled one index for both steps would pass every equality above if
     * the direct builders were fed the same index by mistake. */
    pn = tpm_ab_floor_read_plan(TPM_FLOOR_STEP_COUNTER_PUBLIC, plan, sizeof plan);
    dn = tpm_ab_floor_read_plan(TPM_FLOOR_STEP_RECORD_PUBLIC, direct, sizeof direct);
    TEST_ASSERT_EQ((int)(pn == dn && memcmp(plan, direct, pn) == 0), 0,
                   "the counter and record ReadPublic commands are distinct");

    TEST_ASSERT_EQ((int)tpm_ab_floor_read_plan(TPM_FLOOR_STEP_COUNT, plan, sizeof plan),
                   0, "an out-of-range step marshals nothing");
    TEST_ASSERT_EQ((int)tpm_ab_floor_read_plan(TPM_FLOOR_STEP_COUNTER_READ, plan, 4u),
                   0, "a buffer too small marshals nothing");
    TEST_ASSERT_EQ((int)tpm_ab_floor_read_plan(TPM_FLOOR_STEP_COUNTER_READ, 0, 64u),
                   0, "a NULL buffer marshals nothing");
}

static void test_authz_e2e_forged_record_refused(void)
{
    struct tpm_t_test_state prev;
    uint32_t version = 0;
    tpm_nv_status_t st;

    /* An attacker with owner-write access rewrites the stored record's version
     * in place. They cannot recompute the digest without the authority, so the
     * read refuses rather than reporting their number. */
    azf_reset(0u, 0u);
    azf_counter = 20u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 20u, 500u);
    azf_record[TPM_RECORD_HDR_LEN] = 0x01u;   /* security_version -> 1 */

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_read(&version, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* Corrupt PERSISTED state, not caller misuse: an authorized-recovery
     * question, which is exactly what TPM_NV_CONTRACT carries. */
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_CONTRACT,
                   "a record edited in place is refused, not reported");
}

static void test_authz_e2e_wrong_index_identity_refused(void)
{
    struct tpm_t_test_state prev;
    uint32_t version = 0;
    tpm_nv_status_t st;

    /* The identity gate is live: make the TPM report a public area that does
     * not match the compiled manifest and the read must refuse BEFORE trusting
     * a single content byte. */
    azf_reset(TPM2_CC_NV_READ_PUBLIC, 0x0000018Bu /* HANDLE -> NOTFOUND */);
    azf_counter = 5u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 5u, 10u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_ab_floor_read(&version, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT(st != TPM_NV_OK, "an unreadable index identity refuses the read");
    /* The counter read legitimately precedes the identity check inside the same
     * sequence, so the assertion is on the RECORD read: exactly one NV_Read (the
     * counter) may appear, and no second one for contents. */
    {
        uint32_t i, reads = 0;
        for (i = 0; i < azf_seen_n; i++)
            if (azf_seen[i] == TPM2_CC_NV_READ) reads++;
        TEST_ASSERT(reads <= 1u, "no content was read after the identity failed");
    }
}

static void test_authz_e2e_recreated_anchor_refused(void)
{
    struct tpm_t_test_state prev;
    struct tpm_authz_transition tr;
    uint8_t approved_w[SHA256_DIGEST_LEN], approved_c[SHA256_DIGEST_LEN], sig[8];
    uint32_t version = 0;
    tpm_nv_status_t st;

    /* An attacker destroys the floor index and recreates it with a BYTE-
     * IDENTICAL definition. The public area matches the enrolled contract
     * exactly, because they copied it, so the identity comparison alone passes.
     * What gives it away is that a freshly defined index reads back UNWRITTEN.
     *
     * This must report RECREATED. If it fell through to the content read it
     * would report UNINIT, and UNINIT downstream means "no record yet" -- the
     * laundering of a destroyed anchor into a first enrollment. */
    azf_reset(0u, 0u);
    azf_counter = 30u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 30u, 400u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    azf_report_unwritten = 1;
    st = tpm_ab_floor_read(&version, 0);
    azf_report_unwritten = 0;
    azf_have_cphash = 0; azf_have_ticket = 0; azf_rejects = 0;
    memset(azf_sess, 0, sizeof azf_sess);
    azf_next_session = 0;
    azf_obj_handle = 0; azf_obj_live = 0;
    memset(azf_pinned_cphash, 0, sizeof azf_pinned_cphash);
    memset(azf_verified_approved, 0, sizeof azf_verified_approved);
    memset(azf_approved_write, 0xA1, sizeof azf_approved_write);
    memset(azf_approved_commit, 0xB2, sizeof azf_approved_commit);
    azf_policy_ref[0] = 'I'; azf_policy_ref[1] = 'P';
    azf_policy_ref[2] = 'O'; azf_policy_ref[3] = 'S';
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "a recreated anchor is REFUSED, not read as uninitialized");
    TEST_ASSERT_EQ((int)azf_writes, 0, "nothing was written to a recreated anchor");

    /* And an ADVANCE over it must not paper the gap over with a fresh floor:
     * that is precisely the laundering, one layer up. */
    azf_reset(0u, 0u);
    memset(&tr, 0, sizeof tr);
    azf_fill_grant(&tr, approved_w, approved_c, sig);
    azf_counter = 30u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 30u, 400u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    azf_report_unwritten = 1;
    st = tpm_ab_floor_advance(1u, &tr);
    azf_report_unwritten = 0;
    azf_have_cphash = 0; azf_have_ticket = 0; azf_rejects = 0;
    memset(azf_sess, 0, sizeof azf_sess);
    azf_next_session = 0;
    azf_obj_handle = 0; azf_obj_live = 0;
    memset(azf_pinned_cphash, 0, sizeof azf_pinned_cphash);
    memset(azf_verified_approved, 0, sizeof azf_verified_approved);
    memset(azf_approved_write, 0xA1, sizeof azf_approved_write);
    memset(azf_approved_commit, 0xB2, sizeof azf_approved_commit);
    azf_policy_ref[0] = 'I'; azf_policy_ref[1] = 'P';
    azf_policy_ref[2] = 'O'; azf_policy_ref[3] = 'S';
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "an advance over a recreated anchor is refused");
    TEST_ASSERT_EQ((int)azf_writes, 0, "no fresh floor was written over the gap");
    TEST_ASSERT_EQ((int)azf_increments, 0, "the counter was not advanced");
}

static void test_authz_e2e_no_handle_leak_on_failure(void)
{
    struct tpm_t_test_state prev;
    struct tpm_authz_transition tr;
    uint8_t approved_w[SHA256_DIGEST_LEN], approved_c[SHA256_DIGEST_LEN], sig[8];

    /* Fail the write AFTER the key is loaded and the session is open, then
     * assert on LIVENESS rather than on a flush COUNT. Counting flushes cannot
     * distinguish the authority object from the trial session the contract
     * derivation already flushed, so the old assertion could pass while both
     * the object and the policy session leaked. */
    azf_reset(TPM2_CC_NV_WRITE, AZF_RC_POLICY_FAIL);
    memset(&tr, 0, sizeof tr);
    azf_counter = 3u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 3u, 1u);
    azf_arm_floor_advance(&tr, approved_w, approved_c, sig, 3u, 2u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    (void)tpm_ab_floor_advance(2u, &tr);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)azf_writes, 0, "the write did not succeed");
    TEST_ASSERT_EQ((int)azf_increments, 0, "nothing was committed");
    TEST_ASSERT(azf_loads >= 1u, "the authority key was loaded");
    TEST_ASSERT_EQ(azf_obj_live, 0, "the authority object was flushed");
    TEST_ASSERT_EQ(azf_any_handle_live(), 0,
                   "no session or object handle survives the failure");

    /* Same assertion where the failure lands on the COMMIT half, which opens a
     * second key load and a second session. */
    azf_reset(TPM2_CC_NV_INCREMENT, AZF_RC_POLICY_FAIL);
    memset(&tr, 0, sizeof tr);
    azf_counter = 11u;
    azf_record_len = az_build_floor(azf_record, AZ_FLOOR_LEN, 11u, 4u);
    azf_arm_floor_advance(&tr, approved_w, approved_c, sig, 11u, 9u);

    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    (void)tpm_ab_floor_advance(9u, &tr);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    TEST_ASSERT_EQ((int)azf_writes, 1, "the record was written before the commit");
    TEST_ASSERT_EQ((int)azf_increments, 0, "the commit was refused");
    TEST_ASSERT_EQ(azf_any_handle_live(), 0,
                   "no handle survives a failure on the commit half");
}

static void test_authz_e2e_bind_verify(void)
{
    static const uint8_t blob[64] = { 0x11, 0x22, 0x33 };
    static const uint8_t other[64] = { 0x44, 0x55, 0x66 };
    struct tpm_t_test_state prev;
    uint64_t gen = 0;
    tpm_nv_status_t st;

    /* CONTROL: a blob that matches its committed bind record verifies. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, &gen);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "CONTROL: a bound baseline verifies");
    TEST_ASSERT_EQ((int)gen, 6, "the bound generation is reported");

    /* THE RELABEL ATTACK, through the REAL verifier: old content, current
     * generation, everything the attacker can recompute recomputed. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(other, (uint32_t)sizeof other, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "a different blob under the same bind record is REFUSED");

    /* A blob of the wrong LENGTH is refused before the digest is consulted. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, 32u, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH, "a wrong blob length is REFUSED");

    /* A LEGACY machine: no bind index at all. Reported as NOTFOUND so the
     * caller can refuse rather than silently accepting an unauthenticated
     * baseline -- and never auto-wrapped, since its content is owner-writable. */
    azf_reset(TPM2_CC_NV_READ_PUBLIC, 0x0000018Bu /* HANDLE -> NOTFOUND */);
    azf_counter = 6u;
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND,
                   "a legacy baseline with no bind record is reported, not accepted");
    TEST_ASSERT_EQ((int)azf_writes, 0, "the legacy blob was NOT auto-wrapped");

    TEST_ASSERT_EQ((int)tpm_baseline_bind_verify(0, 8u, 0), (int)TPM_NV_BADARG,
                   "NULL blob refused");
    TEST_ASSERT_EQ((int)tpm_baseline_bind_verify(blob, 0u, 0), (int)TPM_NV_BADARG,
                   "zero-length blob refused");
}

/* The aggregate ledger against the REAL read path.
 *
 * The pure fixture above proves the arithmetic. This proves the arithmetic is
 * actually WIRED: that a verified read consults the boot ledger at all, that a
 * refusal propagates out of tpm_baseline_bind_verify rather than being absorbed
 * into a shorter read set, and that the refusal is visible afterwards through
 * tpm_boot_budget_expired().
 *
 * The BREADTH bound is what the refusal is driven with, deliberately. A
 * wall-clock exhaustion against the fake TIS would depend on how long the
 * fixture's own commands happen to take, so it would assert a race rather than
 * a rule; the clock arithmetic is fully covered by the pure fixture, and the
 * breadth bound exercises the identical admission path. */
static void test_authz_e2e_boot_budget_bounds_verified_read(void)
{
    static const uint8_t blob[64] = { 0x11, 0x22, 0x33 };
    struct tpm_t_test_state prev;
    struct tpm_boot_budget_test_state bprev;
    tpm_nv_status_t st;
    /* Whatever the live ledger's expiry state is when the suite reaches here.
     * Asserting a LITERAL 0 at the end would assert a property of the ambient
     * boot rather than of the restore, and would flip the moment the boot
     * legitimately exhausted its own allowance. */
    const int ambient_expired = tpm_boot_budget_expired();

    /* CONTROL: the same read, under the real boot quota, succeeds. Without it
     * the refusal below would also pass against a read that never worked. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(TPM_BOOT_BUDGET_MS, TPM_BOOT_BUDGET_OPS);
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "CONTROL: a verified read inside the boot allowance succeeds");
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), 0,
                   "CONTROL: and reports no expiry");
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* Now the same read against a ledger that allows exactly ONE operation.
     * The first is admitted, the second is refused, and the refusal is the
     * READ's answer -- not a quietly truncated set of records. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(0u /* clockless */, 1u);
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "the first verified read is admitted");
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET,
                   "the second is REFUSED by the aggregate ledger");
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), 1,
                   "and the boot can report that it ran out, not that it was clean");
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* THE PER-SEQUENCE CLAMP, at the call site. The pure helper is asserted in
     * the ledger fixture; this proves the read path actually CALLS it. The read
     * runs under a full boot quota, so its remaining grant is far above the
     * per-operation ceiling: without the clamp the final sequence would inherit
     * the whole three-sequence allowance, and a fake TPM that answers instantly
     * would never notice. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(TPM_BOOT_BUDGET_MS, TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 0u);
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "CONTROL: the read completes");
    {
        uint32_t work = 0; uint64_t gen = 0, gen2 = 0;
        tpm_t_test_last_seq(&work, &gen);
        TEST_ASSERT_EQ((int)work, (int)TPM_NV_OP_BUDGET_MS,
                       "and its final sequence got the per-operation ceiling, "
                       "not the whole aggregate grant");
        TEST_ASSERT_EQ((int)(gen != 0u), 1,
                       "observed against a real admitted sequence generation");
        tpm_t_test_last_seq(0, &gen2);
        TEST_ASSERT_EQ((int)(gen == gen2), 1,
                       "and the observation is stable, not a racing write");
    }
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* THE SHORT-GRANT REFUSAL, through the REAL read path.
     *
     * The pure fixture proves admission CAN return a grant smaller than one
     * indivisible sequence; only this proves the read then refuses on it.
     * Without it, deleting the pre-sequence check or its expiry latch would
     * stay green while the boot started a full-price TPM sequence it could not
     * afford. Zero commands is the assertion that makes "refuses BEFORE
     * starting" mean something. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(TPM_NV_POLICY_SEQ_COST_MS - 1u,
                                         TPM_BOOT_BUDGET_OPS);
    azf_seen_n = 0u;
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET,
                   "a grant too small for one sequence REFUSES the read");
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), 1,
                   "and records that the boot ran out, not that it was clean");
    TEST_ASSERT_EQ((int)azf_seen_n, 0,
                   "refusing BEFORE starting: no TPM command was issued");
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* THE BETWEEN-SEQUENCE GUARDS, driven by pacing the injected clock one step
     * per TPM sequence. A verified read runs three indivisible sequences, and
     * the guards between them are what stop it starting one it can no longer
     * afford. Nothing else reaches them: the short-grant case above returns
     * before any command, and the pure ledger tests never enter the read path.
     *
     * Quota is two sequence-costs, so admission grants exactly that. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(2u * TPM_NV_POLICY_SEQ_COST_MS,
                                         TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 0u);
    /* One whole sequence-cost per sequence: after the FIRST contract the
     * remainder is exactly one cost, which is the PASSING boundary; after the
     * second it is zero and the read must refuse. */
    azf_clock_step_ms = TPM_NV_POLICY_SEQ_COST_MS;
    azf_seen_n = 0u;
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET,
                   "a read whose derivations consumed the grant refuses mid-way");
    TEST_ASSERT_EQ((int)azf_sequences(), 2,
                   "after EXACTLY TWO sequences -- guard 2, having passed guard 1 at its boundary");
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), 1,
                   "and records the exhaustion rather than reporting a clean boot");
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* THE FIRST GUARD SPECIFICALLY. The case above leaves exactly one sequence
     * cost after the first derivation, which is the PASSING boundary, so it
     * refuses at the second guard and proves nothing about the first -- a
     * mutation removing guard 1 survived it. Stepping slightly MORE than a
     * sequence cost leaves less than one behind the first derivation, which is
     * the only condition guard 1 answers. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(2u * TPM_NV_POLICY_SEQ_COST_MS,
                                         TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 0u);
    azf_clock_step_ms = TPM_NV_POLICY_SEQ_COST_MS + 1000u;
    azf_seen_n = 0u;
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET,
                   "a first derivation that overruns refuses before the second");
    TEST_ASSERT_EQ((int)azf_sequences(), 1,
                   "and refuses after EXACTLY ONE sequence -- guard 1, not guard 2");
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* GUARD 2 SPECIFICALLY, at the state a uniform step cannot reach. Guard 2
     * exists to stop `grant_ms -= spent_ms` underflowing when the derivations
     * overran the grant outright; at spent == grant the subtraction is 0 and
     * the transport refuses anyway, so only spent > grant exposes it. The steps
     * pass guard 1 at its boundary and then blow past the grant. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(2u * TPM_NV_POLICY_SEQ_COST_MS,
                                         TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 0u);
    azf_clock_steps[0] = TPM_NV_POLICY_SEQ_COST_MS;          /* guard 1 passes */
    azf_clock_steps[1] = TPM_NV_POLICY_SEQ_COST_MS + 1000u;  /* spent > grant */
    azf_seen_n = 0u;
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_BUDGET,
                   "derivations that overran the grant refuse rather than underflowing it");
    TEST_ASSERT_EQ((int)azf_sequences(), 2,
                   "and the read sequence is never started");
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* CONTROL for the pacing itself: identical fixture, clock NOT advanced, so
     * the same read completes. Without this the assertion above would pass for
     * a fixture that simply broke the read. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(2u * TPM_NV_POLICY_SEQ_COST_MS,
                                         TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 0u);
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "CONTROL: the same read with a still clock completes");
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* SETTLEMENT ON AN ERROR PATH. A read that fails AFTER admission must
     * return its reservation, or one TPM error permanently consumes a whole
     * three-sequence grant and every later verified read is refused a budget it
     * never spent. The clock is held still, so a correct settlement refunds
     * essentially all of it. */
    azf_reset(TPM2_CC_NV_READ, 0x0000018Bu /* HANDLE */);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    bprev = tpm_boot_budget_test_install(TPM_NV_VERIFIED_READ_BUDGET_MS,
                                         TPM_BOOT_BUDGET_OPS);
    tpm_boot_budget_test_set_clock(1, 0u);
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    TEST_ASSERT_EQ((int)(st != TPM_NV_OK), 1,
                   "CONTROL: the fixture really did fail the read");
    TEST_ASSERT_EQ((int)(st != TPM_NV_BUDGET), 1,
                   "and failed for its own reason, not by running out of budget");
    {
        struct tpm_boot_grant after;
        TEST_ASSERT_EQ((int)tpm_boot_budget_admit_one(TPM_NV_VERIFIED_READ_BUDGET_MS,
                                                      &after),
                       (int)TPM_NV_OK,
                       "a failed read RETURNS its reservation");
        TEST_ASSERT_EQ((int)after.granted_ms, (int)TPM_NV_VERIFIED_READ_BUDGET_MS,
                       "in full, because no wall-clock time was actually spent");
    }
    tpm_boot_budget_test_set_clock(0, 0u);
    tpm_boot_budget_test_restore(bprev);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);

    /* The ledger is RESTORED, so a suite run cannot leave the live boot's
     * allowance perturbed for whatever runs after it -- the fixture drove it to
     * expired and it must come back exactly as it was found. */
    TEST_ASSERT_EQ(tpm_boot_budget_expired(), ambient_expired,
                   "the live ledger's expiry state is restored after the fixture");
}

/* A LOST COMMIT ANCHOR IS NOT A LEGACY MACHINE.
 *
 * The rollback attack does not need the attacker to increment anything. Destroy
 * the commit counter and redefine it byte-identically: its manifest contract
 * has expect_written 0 (a TPM_NT_COUNTER legitimately reads unwritten until its
 * first increment), so the identity check passes and the value read answers
 * TPM_RC_NV_UNINITIALIZED. Collapsing that into the same no-record answer a
 * genuinely un-enrolled machine gives made verification recommend MIGRATION --
 * which authenticates the current owner-writable blob and launders the rollback
 * evidence. The RECORD is what tells the two apart. */
static void test_authz_e2e_lost_commit_anchor(void)
{
    static const uint8_t blob[64] = { 0x11, 0x22, 0x33 };
    struct tpm_t_test_state prev;
    tpm_nv_status_t st;

    /* CONTROL: intact anchors verify. Without it, a fixture that broke the read
     * outright would satisfy the refusal below on its own. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "CONTROL: an intact pairing verifies");

    /* THE ATTACK: the record survives, its commit counter reads uninitialized. */
    azf_reset(0u, 0u);
    azf_counter_uninit = 1;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "a record with no commit anchor behind it is a RECREATED anchor");
    TEST_ASSERT_EQ((int)(st != TPM_NV_NOTFOUND), 1,
                   "and is NEVER the legacy no-record answer that invites migration");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(st), (int)TPM_BASELINE_IDENTITY,
                   "so the boot publishes it as an authenticity failure");

    /* THE SIMPLER ATTACK: the counter index is DELETED outright, so it fails
     * the identity read rather than the value read. Deferring only the value
     * read left this taking the legacy path -- the easier move, misclassified. */
    azf_reset(0u, 0u);
    azf_absent_index = TPM_NV_INDEX_BASELINE_GEN;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "a DELETED commit counter behind a surviving record is tamper");
    TEST_ASSERT_EQ((int)(st != TPM_NV_NOTFOUND), 1,
                   "and is NEVER the legacy answer either");

    /* THE COUNTER'S IDENTITY MATCH, which nothing else reaches. Only the
     * COUNTER's definition is corrupted; the record's stays correct, and the
     * reported Name stays self-consistent with the corrupted public area, so
     * the Name check passes and the manifest-contract comparison is what
     * refuses. The absent controls above force NV_ReadPublic to fail outright
     * and exit before that comparison, so deleting the match stayed green. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_wrong_contract_index = TPM_NV_INDEX_BASELINE_GEN;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_MISMATCH,
                   "a counter whose definition is not the enrolled one is REFUSED");
    TEST_ASSERT_EQ((int)(st != TPM_NV_NOTFOUND), 1,
                   "and not mistaken for an absent anchor");
    /* The verdict is reached through the VERIFY-path map, not the global one:
     * tpm_baseline_verify intercepts a MISMATCH and routes it on the pairing,
     * which is BADARG here because the refusal landed before any pairing could
     * be computed. The global map deliberately leaves MISMATCH as TPMERR,
     * because the WRITE path produces it for a benign counter race. */
    TEST_ASSERT_EQ((int)tpm_baseline_pairing_status(TPM_PAIRING_BADARG),
                   (int)TPM_BASELINE_IDENTITY,
                   "so verification publishes it as an authenticity failure");

    /* CONTROL: the identical fixture with the counter definition CORRECT
     * verifies, so the refusal above is the contract check and not the harness. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK,
                   "CONTROL: the enrolled counter definition verifies");

    /* THE MIRROR CASE: the RECORD is gone and its commit counter survives with a
     * VALUE. A counter only reads a value once a transition committed, and a
     * transition writes the record BEFORE it increments, so a live counter
     * proves a record once existed. Reporting absence would invite the same
     * migration the counter-side deferral exists to prevent. */
    azf_reset(0u, 0u);
    azf_counter = 6u;                                 /* the counter is live */
    azf_absent_index = TPM_NV_INDEX_BASELINE_BIND;    /* the record is gone */
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "a DESTROYED record behind a live commit counter is tamper");
    TEST_ASSERT_EQ((int)(st != TPM_NV_NOTFOUND), 1,
                   "and never the legacy answer that invites migration");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(st), (int)TPM_BASELINE_IDENTITY,
                   "so the boot publishes it as an authenticity failure");

    /* RECORD LOSS AFTER ITS IDENTITY PASSED. ReadPublic proves the index
     * exists, matches the manifest and is WRITTEN; the NV_Read that follows
     * then says "not found". That is a record destroyed between two commands,
     * or a TPM answering inconsistently -- never a machine that was simply
     * never enrolled, which is what the legacy answer would invite. */
    azf_reset(0u, 0u);
    azf_counter = 6u;
    azf_record_len = az_build_bind(azf_record, AZ_BIND_LEN, 6u, blob,
                                   (uint32_t)sizeof blob);
    azf_record_read_rc = 0x0000018Bu;   /* HANDLE -> NOTFOUND, on NV_Read only */
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_RECREATED,
                   "a record that vanishes after its identity passed is tamper");
    TEST_ASSERT_EQ((int)tpm_baseline_nv_status(st), (int)TPM_BASELINE_IDENTITY,
                   "and is published, not routed to migration");

    /* BOTH anchors genuinely ABSENT is the un-enrolled machine, and it MUST
     * still reach the legacy answer -- the assertion that stops the fixes above
     * from being implemented as "always report tamper".
     *
     * Both indices are driven to NOTFOUND at NV_ReadPublic, and the assertion is
     * the EXACT status. An earlier version of this control left both indices
     * present and asserted only `!= RECREATED`, which a transport error, a
     * MISMATCH or a BADARG would all have satisfied while never once exercising
     * the two-NOTFOUND path it claimed to cover. */
    azf_reset(0u, 0u);
    azf_absent_index  = TPM_NV_INDEX_BASELINE_GEN;
    azf_absent_index2 = TPM_NV_INDEX_BASELINE_BIND;
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_baseline_bind_verify(blob, (uint32_t)sizeof blob, 0);
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_NOTFOUND,
                   "CONTROL: a machine missing BOTH anchors is the legacy answer, exactly");
}

static void test_authz_contract_cache(void)
{
    struct tpm_t_test_state prev;
    struct tpm_nv_identity id, bad;
    tpm_nv_status_t st;

    /* The cache guard is what stops persisted data replacing the compiled
     * manifest, and a swapped contract redirects a "verified" read to an index
     * the attacker defined to match it. Every field is load-bearing. */
    azf_reset(0u, 0u);
    prev = tpm_t_test_install(&azf_io, TPM_T_IFACE_TIS, 1);
    azf_install_authority();
    st = tpm_authz_contract(TPM_NV_INDEX_AB_FLOOR, &id);
    if (st == TPM_NV_OK) {
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&id), (int)TPM_NV_OK,
                       "CONTROL: the derived contract validates as its own cache");
        bad = id; bad.name_alg = 0x000Cu;
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_MISMATCH,
                       "a mutated name algorithm is refused");
        bad = id; bad.attrs ^= TPMA_NV_OWNERREAD;
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_MISMATCH,
                       "mutated attributes are refused");
        bad = id; bad.data_size = (uint16_t)(id.data_size + 1u);
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_MISMATCH,
                       "a mutated data size is refused");
        bad = id; bad.expect_written = (uint8_t)(!id.expect_written);
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_MISMATCH,
                       "a mutated written expectation is refused");
        bad = id; bad.policy_len = 0u;
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_MISMATCH,
                       "a dropped authPolicy is refused");
        bad = id; bad.auth_policy[0] ^= 0xFFu;
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_MISMATCH,
                       "a single mutated authPolicy byte is refused");
        bad = id; bad.policy_len = (uint16_t)(TPM_NV_POLICY_MAX + 1u);
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_MISMATCH,
                       "an over-long authPolicy is refused");
        /* A cache naming a handle with NO manifest row cannot be validated at
         * all, which is the answer rather than an omission. */
        bad = id; bad.nv_index = 0x01FFFFFFu;
        TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(&bad), (int)TPM_NV_BADARG,
                       "a cache naming an unenrolled handle has no contract");
    }
    tpm_authz_test_clear_authority();
    tpm_t_test_restore(prev);
    TEST_ASSERT_EQ((int)st, (int)TPM_NV_OK, "the contract derived for the cache test");
    TEST_ASSERT_EQ((int)tpm_authz_contract_cache_ok(0), (int)TPM_NV_BADARG,
                   "NULL cache refused");
}


static void test_authz_grant_wellformed(void)
{
    struct tpm_authz_transition tr;
    uint8_t approved[SHA256_DIGEST_LEN], sig[8];

    memset(approved, 0xA1, sizeof approved);
    memset(sig, 0x77, sizeof sig);
    memset(&tr, 0, sizeof tr);
    tr.write.approved_policy = approved;
    tr.write.approved_len = (uint16_t)SHA256_DIGEST_LEN;
    tr.write.signature = sig; tr.write.sig_len = 8u;
    tr.commit = tr.write;

    /* A consumer whose first act is irreversible checks this BEFORE mutating,
     * so both halves have to be judged and neither may be skipped. */
    TEST_ASSERT_EQ((int)tpm_authz_grant_wellformed(&tr), (int)TPM_NV_OK,
                   "CONTROL: a well-formed transition passes");
    TEST_ASSERT_EQ((int)tpm_authz_grant_wellformed(0), (int)TPM_NV_BADARG,
                   "a NULL transition is refused");
    { struct tpm_authz_transition b = tr; b.write.signature = 0;
      TEST_ASSERT_EQ((int)tpm_authz_grant_wellformed(&b), (int)TPM_NV_BADARG,
                     "a write half without a signature is refused"); }
    { struct tpm_authz_transition b = tr; b.commit.approved_policy = 0;
      TEST_ASSERT_EQ((int)tpm_authz_grant_wellformed(&b), (int)TPM_NV_BADARG,
                     "a commit half without an approved policy is refused"); }
    { struct tpm_authz_transition b = tr; b.commit.approved_len = 16u;
      TEST_ASSERT_EQ((int)tpm_authz_grant_wellformed(&b), (int)TPM_NV_BADARG,
                     "a wrong-sized commit policy is refused"); }
    { struct tpm_authz_transition b = tr; b.write.sig_len = 0u;
      TEST_ASSERT_EQ((int)tpm_authz_grant_wellformed(&b), (int)TPM_NV_BADARG,
                     "an empty signature is refused"); }
}

static void test_authz_builder_rejections(void)
{
    uint8_t buf[256], d32[32], sig[8], name[34], ticket[8];

    memset(d32, 0x5A, sizeof d32);
    memset(sig, 0x11, sizeof sig);
    memset(name, 0x22, sizeof name);
    memset(ticket, 0x33, sizeof ticket);

    /* A NULL destination on any builder must be a refusal, not a write through
     * a null pointer: these are reached from response-driven paths where a
     * caller mistake and a malformed reply look alike. */
    TEST_ASSERT_EQ((int)tpm2_build_policy_cphash(0, 64u, 0x03000000u, d32), 0,
                   "PolicyCpHash refuses a NULL buffer");
    TEST_ASSERT_EQ((int)tpm2_build_policy_nv(0, 96u, TPM_NV_INDEX_AB_SEQ,
                                             0x03000000u, d32, 8u, 0u, TPM2_EO_EQ), 0,
                   "PolicyNV refuses a NULL buffer");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(0, 256u, 0x03000000u, d32, 32u,
                                                    0, 0u, name, 34u, ticket, 8u), 0,
                   "PolicyAuthorize refuses a NULL buffer");
    TEST_ASSERT_EQ((int)tpm2_build_load_external(0, 128u, d32, 32u, TPM_RH_OWNER), 0,
                   "LoadExternal refuses a NULL buffer");
    TEST_ASSERT_EQ((int)tpm2_build_verify_signature(0, 128u, 0x80000001u, d32,
                                                    sig, 8u), 0,
                   "VerifySignature refuses a NULL buffer");

    /* Each remaining guard on PolicyAuthorize, one at a time. */
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    0, 32u, 0, 0u, name, 34u,
                                                    ticket, 8u), 0,
                   "PolicyAuthorize refuses a NULL approved policy");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    d32, 32u, 0, 4u, name, 34u,
                                                    ticket, 8u), 0,
                   "PolicyAuthorize refuses a NULL policyRef carrying a length");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    d32, 32u, 0, 0u, 0, 34u,
                                                    ticket, 8u), 0,
                   "PolicyAuthorize refuses a NULL key Name");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    d32, 32u, 0, 0u, name, 34u,
                                                    0, 8u), 0,
                   "PolicyAuthorize refuses a NULL ticket");
    /* Exact-capacity control beside the one-byte-short refusal, so the bound is
     * pinned rather than merely non-zero. 10+4+(2+32)+(2+0)+(2+34)+8 = 94. */
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, 94u, 0x03000000u, d32, 32u,
                                                    0, 0u, name, 34u, ticket, 8u), 94,
                   "CONTROL: PolicyAuthorize fits its exact capacity");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, 93u, 0x03000000u, d32, 32u,
                                                    0, 0u, name, 34u, ticket, 8u), 0,
                   "PolicyAuthorize refuses one byte short");

    TEST_ASSERT_EQ((int)tpm2_build_verify_signature(buf, sizeof buf, 0x80000001u,
                                                    0, sig, 8u), 0,
                   "VerifySignature refuses a NULL digest");
    TEST_ASSERT_EQ((int)tpm2_build_verify_signature(buf, sizeof buf, 0x80000001u,
                                                    d32, 0, 8u), 0,
                   "VerifySignature refuses a NULL signature");

    /* A ticket length near UINT32_MAX must be refused BEFORE it enters the
     * total, or the addition wraps below cap and the copy runs past the buffer.
     * The exported builder cannot rely on its usual caller's small tickets. */
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    d32, 32u, 0, 0u, name, 34u,
                                                    ticket, 0xFFFFFFF0u), 0,
                   "PolicyAuthorize refuses a wrapping ticket length");
    TEST_ASSERT_EQ((int)tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u,
                                                    d32, 32u, 0, 0u, name, 34u,
                                                    ticket, TPM2_TK_VERIFIED_MAX_LEN + 1u), 0,
                   "PolicyAuthorize refuses a ticket above the TPMT_TK_VERIFIED bound");
    TEST_ASSERT(tpm2_build_policy_authorize(buf, sizeof buf, 0x03000000u, d32, 32u,
                                            0, 0u, name, 34u, ticket,
                                            TPM2_TK_VERIFIED_NULL_LEN) != 0u,
                "CONTROL: a null-ticket length is still accepted");

    /* The corrected comparison constants: GT and GE are DIFFERENT operations,
     * and an earlier revision gave GE the GT value. */
    TEST_ASSERT_EQ((int)TPM2_EO_EQ, 0x0000, "TPM_EO_EQ wire value");
    TEST_ASSERT_EQ((int)TPM2_EO_UNSIGNED_GT, 0x0003, "TPM_EO_UNSIGNED_GT wire value");
    TEST_ASSERT_EQ((int)TPM2_EO_UNSIGNED_GE, 0x0007, "TPM_EO_UNSIGNED_GE wire value");
    TEST_ASSERT(TPM2_EO_UNSIGNED_GT != TPM2_EO_UNSIGNED_GE,
                "GT and GE are distinct operations");
}

static void test_authz_parser_bounds(void)
{
    uint8_t rsp[128], out[64];
    uint32_t handle = 0, tlen = 0;
    uint16_t nlen = 0;

    memset(rsp, 0, sizeof rsp);
    tpm2_be16_put(rsp + 0, TPM2_ST_NO_SESSIONS);
    tpm2_be32_put(rsp + 2, 10u + 4u + 2u + 34u);
    tpm2_be32_put(rsp + 6, 0u);
    tpm2_be32_put(rsp + 10, 0x80000002u);
    tpm2_be16_put(rsp + 14, 34u);

    /* NULL outputs are caller misuse and must be distinguished from a bad
     * response: BADARG means no transaction was implicated, TRANSPORT means the
     * device answered badly, and a caller acts differently on each. */
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 50u, 0, out, 64u, &nlen),
                   (int)TPM_NV_BADARG, "NULL handle output refused");
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 50u, &handle, 0, 64u, &nlen),
                   (int)TPM_NV_BADARG, "NULL Name output refused");
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 50u, &handle, out, 64u, 0),
                   (int)TPM_NV_BADARG, "NULL Name length output refused");
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 50u, &handle, out, 34u, &nlen),
                   (int)TPM_NV_OK, "CONTROL: an exact-capacity Name buffer succeeds");
    TEST_ASSERT_EQ((int)tpm2_parse_load_external(rsp, 50u, &handle, out, 33u, &nlen),
                   (int)TPM_NV_TRANSPORT, "one byte short of the Name is refused");

    /* A non-success response code must not be read as a handle. */
    tpm2_be32_put(rsp + 6, 0x0000018Bu);
    TEST_ASSERT(tpm2_parse_load_external(rsp, 50u, &handle, out, 64u, &nlen)
                != TPM_NV_OK, "an error response yields no object handle");
    tpm2_be32_put(rsp + 6, 0u);

    TEST_ASSERT_EQ((int)tpm2_parse_verify_signature(rsp, 50u, 0, 64u, &tlen),
                   (int)TPM_NV_BADARG, "NULL ticket output refused");
    TEST_ASSERT_EQ((int)tpm2_parse_verify_signature(rsp, 50u, out, 64u, 0),
                   (int)TPM_NV_BADARG, "NULL ticket length output refused");

    /* The transient-object handle extractor only ever hands back a TRANSIENT
     * handle: FlushContext accepts sessions too, so returning one from a
     * corrupt object reply would evict unrelated TPM state. */
    tpm2_be32_put(rsp + 10, 0x80000005u);
    TEST_ASSERT_EQ((int)tpm2_rsp_object_handle(rsp, 50u), (int)0x80000005,
                   "CONTROL: a transient handle is recovered for cleanup");
    tpm2_be32_put(rsp + 10, 0x03000001u);
    TEST_ASSERT_EQ((int)tpm2_rsp_object_handle(rsp, 50u), 0,
                   "a SESSION handle is never returned as an object");
}

static void test_authz_record_parser_bounds(void)
{
    uint8_t rec[AZ_FLOOR_LEN], alt[AZ_FLOOR_LEN];
    struct tpm_record_view v, a, b;
    const uint32_t plen = (uint32_t)sizeof(struct tpm_ab_floor_payload);

    (void)az_build_floor(rec, sizeof rec, 4u, 8u);

    TEST_ASSERT_EQ((int)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, 0),
                   (int)TPM_RECORD_BADARG, "NULL view output refused");
    /* A payload length that would overflow the header addition is refused
     * before any length arithmetic is trusted. */
    TEST_ASSERT_EQ((int)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         0xFFFFFFFFu, &v),
                   (int)TPM_RECORD_BADARG, "an overflowing payload length is refused");

    /* A header payload_len that disagrees with the total, under an otherwise
     * exact length, with the digest recomputed so the branch is isolated. */
    memcpy(alt, rec, sizeof alt);
    alt[16] ^= 0xFFu;   /* payload_len, so it no longer describes the total */
    (void)tpm_record_digest_compute(alt, AZ_FLOOR_LEN, alt + 24u);
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED,
                   "a header payload length disagreeing with the total is refused");

    /* A HOSTILE persisted record at the never-written generation, digest
     * recomputed. The builder refuses to mint one; the parser must refuse to
     * accept one, because persisted bytes do not come from the builder. */
    memcpy(alt, rec, sizeof alt);
    memset(alt + 8, 0, 8u);
    (void)tpm_record_digest_compute(alt, AZ_FLOOR_LEN, alt + 24u);
    TEST_ASSERT_EQ((int)tpm_record_parse(alt, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR,
                                         plen, &v),
                   (int)TPM_RECORD_MALFORMED,
                   "a stored generation-zero record is refused even with a valid digest");

    /* Saturation: a counter at UINT64_MAX cannot advance, and the check must
     * come BEFORE the +1 that would wrap it to the never-written sentinel. */
    (void)tpm_record_parse(rec, AZ_FLOOR_LEN, TPM_RECORD_KIND_AB_FLOOR, plen, &a);
    b = a;
    a.generation = 0xFFFFFFFFFFFFFFFFull;
    b.generation = 0u;
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&a, &b), (int)TPM_RECORD_STEP,
                   "a saturated generation cannot advance");
    /* A hand-built view too short to hold the ordering field is not judgeable
     * and must not be waved through. */
    b = a; b.generation = 0xFFFFFFFFFFFFFFFFull;
    a.generation = 0xFFFFFFFFFFFFFFFEull;
    b.payload_len = 4u;
    TEST_ASSERT_EQ((int)tpm_record_transition_ok(&a, &b), (int)TPM_RECORD_ROLLBACK,
                   "a truncated payload view is treated as a regression");
}

static void test_authz_write_record_input_guards(void)
{
    struct tpm_authz_transition tr;
    uint8_t rec[AZ_FLOOR_LEN], approved[SHA256_DIGEST_LEN], sig[8];

    (void)az_build_floor(rec, sizeof rec, 2u, 1u);
    memset(approved, 0xA1, sizeof approved);
    memset(sig, 0x77, sizeof sig);
    memset(&tr, 0, sizeof tr);
    tr.write.approved_policy = approved;
    tr.write.approved_len = (uint16_t)SHA256_DIGEST_LEN;
    tr.write.signature = sig; tr.write.sig_len = 8u;
    tr.commit = tr.write;
    tpm_authz_test_clear_authority();

    /* Input guards run BEFORE the authority is consulted, so each is reachable
     * without a transport and each returns BADARG rather than UNAVAIL. */
    TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                               TPM_NV_INDEX_AB_SEQ, 0,
                                               (uint16_t)AZ_FLOOR_LEN, 1u, &tr),
                   (int)TPM_NV_BADARG, "a NULL record is refused");
    TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                               TPM_NV_INDEX_AB_SEQ, rec, 0u, 1u, &tr),
                   (int)TPM_NV_BADARG, "a zero-length record is refused");
    TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                               TPM_NV_INDEX_AB_SEQ, rec,
                                               (uint16_t)(TPM_NV_MAX_DATA + 1u), 1u, &tr),
                   (int)TPM_NV_BADARG, "an oversized record is refused");
    TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                               TPM_NV_INDEX_AB_SEQ, rec,
                                               (uint16_t)AZ_FLOOR_LEN, 1u, 0),
                   (int)TPM_NV_BADARG, "a NULL transition is refused");
    /* The record index and its counter must be DIFFERENT indexes: the same
     * handle for both would let one authorization satisfy the other. */
    TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                               TPM_NV_INDEX_AB_FLOOR, rec,
                                               (uint16_t)AZ_FLOOR_LEN, 1u, &tr),
                   (int)TPM_NV_BADARG, "record and counter may not be one index");
    /* Each half of the grant is checked; neither may be empty. */
    {
        struct tpm_authz_transition half = tr;
        half.commit.signature = 0;
        TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                                   TPM_NV_INDEX_AB_SEQ, rec,
                                                   (uint16_t)AZ_FLOOR_LEN, 1u, &half),
                       (int)TPM_NV_BADARG, "a commit grant without a signature is refused");
        half = tr; half.write.approved_len = 8u;
        TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                                   TPM_NV_INDEX_AB_SEQ, rec,
                                                   (uint16_t)AZ_FLOOR_LEN, 1u, &half),
                       (int)TPM_NV_BADARG, "a wrong-sized approved policy is refused");
    }
    /* A fully valid grant with NO authority is UNAVAIL, not BADARG: the caller
     * did nothing wrong, the machine is simply not provisioned. */
    TEST_ASSERT_EQ((int)tpm_authz_write_record(TPM_NV_INDEX_AB_FLOOR,
                                               TPM_NV_INDEX_AB_SEQ, rec,
                                               (uint16_t)AZ_FLOOR_LEN, 1u, &tr),
                   (int)TPM_NV_UNAVAIL, "a valid grant without an authority is UNAVAIL");
}

void test_register_tpm_authz(void)
{
    test_suite_register_cat("tpm: authz record round trip",
                            test_authz_record_roundtrip, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz record digest coverage",
                            test_authz_record_digest_covers_everything, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz record malformed shapes",
                            test_authz_record_malformed, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz record kind substitution",
                            test_authz_record_kind_substitution, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz transition rules",
                            test_authz_transition_rules, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz first-record generation",
                            test_authz_first_record_generation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz counter commit window",
                            test_authz_counter_commit_window, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz pairing direction",
                            test_authz_pairing_direction, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline pairing status map",
                            test_baseline_pairing_status_map, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e lost commit anchor",
                            test_authz_e2e_lost_commit_anchor, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: baseline nv status map",
                            test_baseline_nv_status_map, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: boot budget lifecycle and clock",
                            test_boot_budget_lifecycle_and_clock, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: boot budget indivisible sequence",
                            test_boot_budget_indivisible_sequence, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz boot read budget accounting",
                            test_authz_boot_budget_single_deadline, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz torn pairing detected",
                            test_authz_e2e_torn_pairing_detected, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz floor read plan matches sequence",
                            test_authz_floor_read_plan_matches_sequence, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz baseline relabel refused",
                            test_authz_baseline_relabel_refused, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz cpHash binding",
                            test_authz_cphash, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz PolicyCpHash marshalling",
                            test_authz_build_policy_cphash, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz PolicyNV marshalling",
                            test_authz_build_policy_nv, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz PolicyAuthorize marshalling",
                            test_authz_build_policy_authorize, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz LoadExternal marshalling",
                            test_authz_build_load_external, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz VerifySignature marshalling",
                            test_authz_build_verify_signature, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz response parse bounds",
                            test_authz_parse_bounds, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz unprovisioned fails closed",
                            test_authz_unprovisioned_fails_closed, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz authority Name",
                            test_authz_authority_name, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz enrollment manifest",
                            test_authz_manifest, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz owner increment refused",
                            test_authz_owner_increment_refused, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e contract derivation",
                            test_authz_e2e_contract_derivation, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e authorized advance",
                            test_authz_e2e_authorized_advance, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e write-then-increment order",
                            test_authz_e2e_write_then_increment_order, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e stale generation refused",
                            test_authz_e2e_stale_generation_refused, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e uncommitted record not current",
                            test_authz_e2e_uncommitted_record_not_current, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e forged record refused",
                            test_authz_e2e_forged_record_refused, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e identity gate refuses first",
                            test_authz_e2e_wrong_index_identity_refused, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e recreated anchor refused",
                            test_authz_e2e_recreated_anchor_refused, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e no handle leak on failure",
                            test_authz_e2e_no_handle_leak_on_failure, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e bind verify",
                            test_authz_e2e_bind_verify, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz e2e boot budget bounds verified read",
                            test_authz_e2e_boot_budget_bounds_verified_read,
                            TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz contract cache guard",
                            test_authz_contract_cache, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz grant well-formedness",
                            test_authz_grant_wellformed, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz builder rejections",
                            test_authz_builder_rejections, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz parser bounds",
                            test_authz_parser_bounds, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz record parser bounds",
                            test_authz_record_parser_bounds, TEST_CAT_SECURITY);
    test_suite_register_cat("tpm: authz write-record input guards",
                            test_authz_write_record_input_guards, TEST_CAT_SECURITY);
}
