/* ============================================================================
 * test_quota_owner.c -- Charge-receipt identity tests (quota_owner.c)
 *
 * Covers the receipt IDENTITY contract that makes a chain charge safe to hold
 * next to a recycled resource: the packed {generation, state} tag, the
 * caller-held token that alone returns a charge, the no-obligation token a
 * zero-amount charge reports, and the generation ceiling that fails closed
 * rather than reissuing a live token.
 *
 * Split from test_quota.c (registry + block/charge API) so the test file
 * matches the translation unit under test.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/quota/quota_ledger.h"   /* the per-task charge gate */
#include "kernel/sched/task.h"           /* task_current, task->quota */

/* The token, not merely the state, is what identifies a charge. A returner
 * holding a token from a charge that was already returned must NOT be able to
 * return a LATER charge made through the same receipt storage -- that is the
 * ABA the generation exists to close, and it is the difference between "a
 * receipt is single-use" and "receipt storage is safe to recycle". */
static void test_quota_receipt_token_blocks_stale_return(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t stale_tok = 0, live_tok = 0, before, exhausted_before;

    if (!t || !t->quota) {
        TEST_SKIP("no quota block on the current task");
        return;
    }

    before           = quota_usage(t->quota, QUOTA_RES_SECTION);
    exhausted_before = quota_return_wait_exhausted_count();

    /* First charge, returned normally: stale_tok is now spent. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_SECTION, 7, 0, &r, &stale_tok),
                   (uint64_t)STATUS_SUCCESS, "first charge admitted");
    TEST_ASSERT(stale_tok != 0, "a successful charge issues a non-zero token");
    quota_return_chain(&r, stale_tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_SECTION), before,
                   "first charge returned exactly");

    /* The SAME storage is recharged -- exactly the recycle the old state-only
     * receipt could not distinguish. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_SECTION, 7, 0, &r, &live_tok),
                   (uint64_t)STATUS_SUCCESS, "receipt storage recharged");
    TEST_ASSERT(live_tok != stale_tok,
                "each charge is identified by a distinct token");

    /* The stale returner arrives late. Under the old state-only claim it would
     * have found ACTIVE and credited back a charge it never made. */
    quota_return_chain(&r, stale_tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_SECTION), before + 7,
                   "a stale token cannot return a later charge");

    /* Neither can a fabricated or zero token. */
    quota_return_chain(&r, 0);
    quota_return_chain(&r, live_tok + 1000u);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_SECTION), before + 7,
                   "zero and unissued tokens are no-ops");

    /* A token with bits above the generation field must NOT alias the live
     * token: the tag encoding shifts the token past the state bits, so an
     * unchecked high bit would be discarded and build an identical tag. */
    quota_return_chain(&r, live_tok | (1ULL << 62));
    quota_return_chain(&r, live_tok | (1ULL << 63));
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_SECTION), before + 7,
                   "a non-canonical token cannot alias the live token");

    /* The rightful holder still returns it. */
    quota_return_chain(&r, live_tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_SECTION), before,
                   "the live token still returns its own charge");

    /* None of the above is a leak: a stale, zero, unissued, or non-canonical
     * token is a documented no-op, and the duplicate return below loses
     * idempotently. The wait-exhausted counter means a charge may have been
     * stranded, so counting ordinary idempotence there would make it fire
     * constantly and be useless as a leak signal. */
    quota_return_chain(&r, live_tok);
    TEST_ASSERT_EQ(quota_return_wait_exhausted_count(), exhausted_before,
                   "stale and duplicate returns are not counted as stranded charges");
}

/* A zero-amount charge owes nothing, so the token it reports must be the
 * no-obligation token rather than whatever the caller's variable happened to
 * hold. A zero charge does not advance the generation, so a stale value left in
 * place could collide with the NEXT real charge's token and let a cleanup
 * return a charge it never made. */
static void test_quota_zero_charge_reports_no_obligation_token(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok, live_tok = 0, before;

    if (!t || !t->quota) {
        TEST_SKIP("no quota block on the current task");
        return;
    }

    before = quota_usage(t->quota, QUOTA_RES_OBJECT_BODY);

    /* Seed the caller's variable with the value the NEXT charge on this
     * receipt will publish -- the exact collision the contract must prevent. */
    tok = QUOTA_RECEIPT_TAG_GEN(atomic64_read(&r.tag)) + 1u;

    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_OBJECT_BODY, 0, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "zero-amount charge succeeds");
    TEST_ASSERT_EQ(tok, 0ULL,
                   "a zero-amount charge reports the no-obligation token, not a stale value");

    /* Now make a real charge through the same receipt and try to return it
     * with the token captured from the zero charge. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_OBJECT_BODY, 9, 0, &r, &live_tok),
                   (uint64_t)STATUS_SUCCESS, "the following real charge is admitted");
    quota_return_chain(&r, tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_OBJECT_BODY), before + 9,
                   "a token captured from a zero charge cannot return the live charge");

    quota_return_chain(&r, live_tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_OBJECT_BODY), before,
                   "the real token still returns its own charge");
}

/* The generation is 62 bits, so exhaustion is unreachable in practice -- but
 * the boundary must still fail CLOSED rather than wrap, because a wrapped
 * generation reissues a token a stalled returner may still hold. Seeding the
 * tag directly is the only way to reach the boundary: charging 2^62 times is
 * not a test. */
static void test_quota_receipt_generation_exhaustion(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0, before;

    if (!t || !t->quota) {
        TEST_SKIP("no quota block on the current task");
        return;
    }

    before = quota_usage(t->quota, QUOTA_RES_PROCESS);

    /* One below the ceiling: the charge must still be admitted, and the token
     * it issues is exactly the ceiling value. */
    atomic64_set(&r.tag, QUOTA_RECEIPT_TAG(QUOTA_RECEIPT_GEN_MAX - 1,
                                           QUOTA_RECEIPT_IDLE));
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_PROCESS, 3, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "the last available generation still charges");
    TEST_ASSERT_EQ(tok, QUOTA_RECEIPT_GEN_MAX,
                   "the final token is the ceiling generation itself");
    quota_return_chain(&r, tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_PROCESS), before,
                   "the boundary charge returned exactly");

    /* At the ceiling: refuse rather than wrap back onto a live token. */
    atomic64_set(&r.tag, QUOTA_RECEIPT_TAG(QUOTA_RECEIPT_GEN_MAX,
                                           QUOTA_RECEIPT_IDLE));
    tok = 0;
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_PROCESS, 3, 0, &r, &tok),
                   (uint64_t)STATUS_INTEGER_OVERFLOW,
                   "an exhausted generation fails closed instead of wrapping");
    TEST_ASSERT_EQ(tok, 0ULL, "a refused charge issues no token");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_PROCESS), before,
                   "the refused boundary charge added nothing");

    atomic64_set(&r.tag, QUOTA_RECEIPT_TAG(0, QUOTA_RECEIPT_IDLE));
}


/* A token the caller still holds must NOT survive a refusal raised after the
 * receipt was claimed. The claim proves the receipt held nothing, so whatever
 * sits in the caller's variable is stale -- and a stale value equal to the
 * generation the NEXT charge publishes would let the error-path return that the
 * header documents as safe take a charge it never made. */
static void test_quota_failed_charge_clears_predicted_token(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok, live_tok = 0, before;

    if (!t || !t->quota) {
        TEST_SKIP("no quota block on the current task");
        return;
    }

    before = quota_usage(t->quota, QUOTA_RES_SECTION);

    /* Seed EXACTLY the token the next successful charge on this receipt will
     * publish, then force a POST-CLAIM refusal. The type is validated after the
     * receipt is claimed, so an out-of-range type takes the claimed-then-
     * released path rather than failing at the door. */
    tok = QUOTA_RECEIPT_TAG_GEN(atomic64_read(&r.tag)) + 1u;
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(
                       t, (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT,
                       5, 0, &r, &tok),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "an out-of-range type is refused after the receipt is claimed");
    TEST_ASSERT_EQ(tok, 0ULL,
                   "a post-claim refusal retires the caller's stale token");

    /* The real charge that follows publishes the generation the stale value had
     * predicted -- so before the fix, that value would have returned it. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_SECTION, 5, 0, &r, &live_tok),
                   (uint64_t)STATUS_SUCCESS, "the following real charge is admitted");
    quota_return_chain(&r, tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_SECTION), before + 5,
                   "the retired token cannot return the live charge");

    quota_return_chain(&r, live_tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_SECTION), before,
                   "the live token still returns its own charge");
}

/* A zero-amount charge is still a charge against a specific task, so a SEALED
 * task must refuse it. task_death_teardown seals the gate BEFORE it clears the
 * quota block, leaving a window where a block is still attached to a task that
 * can no longer be charged; the zero path skips the gate, so it has to ask the
 * gate directly or it reports a dying task as chargeable. */
static void test_quota_zero_charge_refused_on_sealed_gate(void)
{
    struct task *t = task_current();
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 7;
    int64_t  saved;

    if (!t || !t->quota) {
        TEST_SKIP("no quota block on the current task");
        return;
    }

    saved = atomic64_read(&t->quota_gate);

    quota_gate_seal(t);
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_SEALED,
                   "the gate is sealed for the probe");
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_OBJECT_BODY, 0, 0, &r, &tok),
                   (uint64_t)STATUS_PROCESS_IS_TERMINATING,
                   "a zero-amount charge on a SEALED task reports termination");
    TEST_ASSERT_EQ(tok, 0ULL, "the refused zero charge issues no token");
    TEST_ASSERT_EQ((uint64_t)QUOTA_RECEIPT_TAG_STATE(atomic64_read(&r.tag)),
                   (uint64_t)QUOTA_RECEIPT_IDLE,
                   "the refused zero charge leaves the receipt reusable");

    atomic64_set(&t->quota_gate, saved);

    tok = 7;
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, QUOTA_RES_OBJECT_BODY, 0, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS,
                   "the same zero charge succeeds once the gate is restored");
    TEST_ASSERT_EQ(tok, 0ULL,
                   "a live zero-amount charge reports the no-obligation token");
}

void test_register_quota_owner(void)
{
    test_suite_register_cat("Quota: stale receipt token cannot return a later charge",
                            test_quota_receipt_token_blocks_stale_return, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: zero charge reports the no-obligation token",
                            test_quota_zero_charge_reports_no_obligation_token, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: receipt generation exhaustion fails closed",
                            test_quota_receipt_generation_exhaustion, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a post-claim refusal retires the stale token",
                            test_quota_failed_charge_clears_predicted_token,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: zero charge on a sealed gate reports termination",
                            test_quota_zero_charge_refused_on_sealed_gate,
                            TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
