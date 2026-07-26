/* ============================================================================
 * test_quota_ledger.c -- charge gate, obligation ledger, transactional adjust
 *
 * Covers quota_ledger.c and the quota_charge_adjust transaction in quota.c: the
 * packed gate state machine that serializes chargers against a membership
 * transition, the refcounted ledger whose obligations outlive the task slot, the
 * orphan-versus-legitimate classification the reap-time release depends on, the
 * all-or-nothing multi-block adjust, and the adopt/revert migration that moves an
 * obligation into a job without charging it twice.
 *
 * Split from test_quota_owner.c (receipt identity) so the test file matches the
 * translation unit under test.
 *
 * GATE STATE IS GLOBAL to the running task, so every test that shuts the gate
 * SAVES the gate word and RESTORES it before returning. Leaving a CLOSED or
 * SEALED gate behind would refuse every later charge in the boot, including the
 * ones the rest of this suite makes.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/quota/quota_ledger.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob_job.h"       /* real JOB_OBJECT fixture for the unwind test */
#include "kernel/ob/handle_table.h"  /* ObpLookupHandle / ObpFreeHandle */
#include "kernel/mm/heap.h"          /* kmalloc_fail_next injection */
#include "kernel/sched/irql.h"        /* KeRaiseIrql/KeLowerIrql for the deferral tests */
#include "kernel/test/scratch.h"      /* TEST_SCRATCH_KBUF for the ceiling walk */

/* The resource type these tests charge. SECTION is used by the sibling receipt
 * tests for the same reason: nothing in a test boot charges it, so a delta
 * observed here was caused by this test and not by background activity. */
#define LEDGER_TEST_TYPE   QUOTA_RES_SECTION

/* Not in the freestanding kernel headers; the house convention is a local
 * declaration beside the test that formats a per-iteration assertion message. */
extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Gate ---------------------------------------------------------------- */

/* Entry and exit are the only things that move the in-flight count, and they
 * must leave the STATE untouched. */
static void test_quota_gate_enter_exit_balances(void)
{
    struct task *t = task_current();
    if (!t)
        return;

    uint32_t state_before = quota_gate_state_of(t);
    uint64_t in_before    = quota_gate_inflight(t);

    TEST_ASSERT_EQ((uint64_t)state_before, (uint64_t)QUOTA_GATE_OPEN,
                   "a live task's gate is OPEN");

    TEST_ASSERT_EQ((uint64_t)quota_gate_enter(t, NULL), 1,
                   "an OPEN gate admits a charger");
    TEST_ASSERT_EQ(quota_gate_inflight(t), in_before + 1,
                   "entry increments the in-flight count by exactly one");
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_OPEN,
                   "entry does not disturb the gate state");

    quota_gate_exit(t);
    TEST_ASSERT_EQ(quota_gate_inflight(t), in_before,
                   "exit restores the in-flight count exactly");
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_OPEN,
                   "exit does not disturb the gate state");
}

/* A quiesced gate must refuse a CHARGE with STATUS_RETRY and move no counter --
 * that refusal is the whole mechanism that makes absorb-then-publish atomic. */
static void test_quota_gate_closed_refuses_charge(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    int64_t  saved = atomic64_read(&t->quota_gate);
    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;

    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "an idle OPEN gate quiesces");
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_CLOSED,
                   "a successful quiesce leaves the gate CLOSED");

    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, LEDGER_TEST_TYPE, 11, 0, &r, &tok),
                   (uint64_t)STATUS_RETRY,
                   "a charge against a CLOSED gate is refused with STATUS_RETRY");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "a gate-refused charge moves no usage");
    TEST_ASSERT_EQ(tok, 0, "a gate-refused charge issues no token");

    /* A second quiesce must not be able to steal a close someone else owns. */
    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_RETRY,
                   "a second quiesce is refused while the first owns the gate");

    quota_gate_reopen(t);
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_OPEN,
                   "reopen restores OPEN");

    /* And charging works again afterwards, so the quiesce is transient. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, LEDGER_TEST_TYPE, 11, 0, &r, &tok),
                   (uint64_t)STATUS_SUCCESS, "a reopened gate admits charges");
    quota_return_chain(&r, tok);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "the post-reopen charge returns exactly");

    atomic64_set(&t->quota_gate, saved);
}

/* SEALED outranks CLOSED: a reopen must never resurrect a dead task's gate, and
 * a charge against a sealed gate is refused permanently rather than transiently
 * -- a caller must be able to tell "retry" from "never". */
static void test_quota_gate_seal_outranks_closed(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    int64_t  saved  = atomic64_read(&t->quota_gate);
    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_charge_receipt_t r = { 0 };
    uint64_t tok = 0;

    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "gate quiesces before the seal");
    quota_gate_seal(t);
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_SEALED,
                   "a seal overrides a CLOSED gate");

    quota_gate_reopen(t);
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_SEALED,
                   "reopen cannot resurrect a SEALED gate");

    TEST_ASSERT_EQ((uint64_t)quota_charge_chain(t, LEDGER_TEST_TYPE, 13, 0, &r, &tok),
                   (uint64_t)STATUS_PROCESS_IS_TERMINATING,
                   "a charge against a SEALED gate reports termination, not retry");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "a seal-refused charge moves no usage");

    /* Sealing twice is idempotent (a doubled death path must not misbehave). */
    quota_gate_seal(t);
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_SEALED,
                   "sealing an already-sealed gate is idempotent");

    atomic64_set(&t->quota_gate, saved);
}

/* A charger still in flight must make the quiesce FAIL rather than proceed, and
 * the failure must restore OPEN -- turning transient contention into a
 * permanently refusing gate would be far worse than refusing one transition. */
static void test_quota_gate_drain_timeout_restores_open(void)
{
    struct task *t = task_current();
    if (!t)
        return;

    int64_t  saved     = atomic64_read(&t->quota_gate);
    uint64_t timeouts0 = quota_gate_drain_timeout_count();

    /* Simulate a charger that is in flight for the whole drain window. */
    TEST_ASSERT_EQ((uint64_t)quota_gate_enter(t, NULL), 1, "charger enters");

    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_RETRY,
                   "a quiesce that cannot drain refuses with STATUS_RETRY");
    TEST_ASSERT_EQ(quota_gate_drain_timeout_count(), timeouts0 + 1,
                   "the refused drain is counted exactly once");
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_OPEN,
                   "a timed-out quiesce restores OPEN rather than stranding CLOSED");

    quota_gate_exit(t);
    atomic64_set(&t->quota_gate, saved);
}

/* --- Ledger -------------------------------------------------------------- */

/* The basic obligation lifetime: a ledger charge moves the same counters a chain
 * charge does, is counted as outstanding while live, and returns exactly. */
static void test_quota_ledger_charge_return_roundtrip(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 64, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge admitted");
    TEST_ASSERT_NOT_NULL((void *)ob.ledger, "a live obligation names its ledger");
    TEST_ASSERT(ob.token != 0, "a live obligation carries a nonzero token");
    TEST_ASSERT(quota_ledger_id(ob.ledger) != 0, "a ledger has a nonzero identity");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 64,
                   "the ledger charge moved the process block by exactly 64");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_outstanding(ob.ledger), 1,
                   "one obligation is outstanding");

    quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "returning the obligation restores usage exactly");
    TEST_ASSERT_NULL((void *)ob.ledger, "a returned handle is emptied");
    TEST_ASSERT_EQ(ob.token, 0, "a returned handle carries no token");
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* A doubled cleanup must not credit the amount twice. The second return sees an
 * emptied handle and does nothing at all. */
static void test_quota_ledger_double_return_is_noop(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 32, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge admitted");
    quota_ledger_return(&ob);
    quota_ledger_return(&ob);      /* must be inert */
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "a doubled return credits the amount exactly once");
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* THE reuse hazard: a stale handle kept past its return must not return the
 * NEXT charge that lands in the same recycled slot.
 *
 * The premise -- that some later charge really does reuse the freed slot -- is
 * ESTABLISHED here rather than assumed. An earlier version simply asserted that
 * the very next charge landed on the same slot, which was an assumption about
 * allocation ORDER, not about the safety property; it broke the moment the claim
 * scan gained a start hint, even though nothing unsafe had changed. Charging until
 * a claim actually lands on the freed slot is robust to any allocation policy: the
 * scan wraps within one pass over the capacity, so the freed slot is reached in a
 * bounded number of charges. */
static void test_quota_ledger_stale_handle_cannot_return_later_charge(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t first = { 0 }, stale;

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 16, &first),
                   (uint64_t)STATUS_SUCCESS, "first ledger charge admitted");
    /* Copy the handle BEFORE returning it, and take the reference the copy
     * implies, so the stale copy stays usable exactly as a buggy caller's would. */
    stale = first;
    quota_ledger_ref(stale.ledger);
    (void)quota_ledger_return(&first);

    /* Charge until one lands on the freed slot, holding every obligation so none
     * of them can be recycled underneath the search. */
    enum { REUSE_MAX = 48 };
    quota_obligation_t held[REUSE_MAX];
    uint32_t n = 0, reuse_at = REUSE_MAX;

    for (n = 0; n < REUSE_MAX; n++) {
        held[n].ledger = (quota_ledger_t *)0;
        held[n].slot   = 0;
        held[n].token  = 0;
        held[n].epoch  = 0;
        if (quota_ledger_charge(t, LEDGER_TEST_TYPE, 48, &held[n]) != STATUS_SUCCESS)
            break;
        if (held[n].slot == stale.slot) {
            reuse_at = n;
            n++;                 /* count this one as held */
            break;
        }
    }

    TEST_ASSERT(reuse_at < REUSE_MAX,
                "a later charge reused the freed slot (the hazard's premise)");

    if (reuse_at < REUSE_MAX) {
        uint64_t live_before = quota_usage(t->quota, LEDGER_TEST_TYPE);
        uint32_t out_before  = quota_ledger_outstanding(held[reuse_at].ledger);

        /* The stale handle's token names a charge that is already gone, and its
         * epoch names an allocation that has already been superseded. */
        TEST_ASSERT_EQ((uint64_t)quota_ledger_return(&stale), 0,
                       "a stale handle reports that it returned nothing");
        TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), live_before,
                       "a stale handle cannot return the charge that reused its slot");
        TEST_ASSERT_EQ((uint64_t)quota_ledger_outstanding(held[reuse_at].ledger),
                       (uint64_t)out_before,
                       "and cannot free the live obligation's slot");
    } else {
        /* Premise not reached: still drop the stale handle's reference so the
         * test leaks nothing. */
        (void)quota_ledger_return(&stale);
    }

    for (uint32_t i = 0; i < n; i++)
        (void)quota_ledger_return(&held[i]);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "every live obligation still returns exactly");
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. */
    quota_ledger_task_release(t);
}

/* A zero-amount charge succeeds owing nothing, so it must hand back an EMPTY
 * handle -- a handle naming a slot it does not own would later return whatever
 * charge reused that slot. */
static void test_quota_ledger_zero_charge_holds_nothing(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 0, &ob),
                   (uint64_t)STATUS_SUCCESS, "a zero-amount ledger charge succeeds");
    TEST_ASSERT_NULL((void *)ob.ledger, "a zero charge yields an empty handle");
    TEST_ASSERT_EQ(ob.token, 0, "a zero charge yields no token");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "a zero charge moves no usage");
    quota_ledger_return(&ob);     /* inert on an empty handle */
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* Storage grows past the inline run when demanded, and a returned slot is
 * REUSED rather than growing the ledger again. */
static void test_quota_ledger_capacity_grows_then_reuses(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    /* One more than the inline run, so at least one overflow chunk is required.
     * The inline count is an implementation constant, so the test derives the
     * target from the reported capacity instead of hard-coding it. */
    quota_obligation_t obs[12];
    uint64_t before   = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint32_t base_cap = 0;
    uint32_t held     = 0;

    for (uint32_t i = 0; i < 12; i++) {
        obs[i].ledger = (quota_ledger_t *)0;
        obs[i].slot   = 0;
        obs[i].token  = 0;
        if (quota_ledger_charge(t, LEDGER_TEST_TYPE, 4, &obs[i]) != STATUS_SUCCESS)
            break;
        if (i == 0)
            base_cap = quota_ledger_capacity(obs[0].ledger);
        held++;
    }

    TEST_ASSERT_EQ((uint64_t)held, 12, "twelve obligations were admitted");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + (12 * 4),
                   "every obligation charged its amount");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_outstanding(obs[0].ledger), 12,
                   "all twelve are outstanding at once");
    TEST_ASSERT(quota_ledger_capacity(obs[0].ledger) > base_cap,
                "storage grew past the inline run to hold twelve obligations");

    uint32_t grown_cap = quota_ledger_capacity(obs[0].ledger);
    quota_ledger_t *ledger = obs[0].ledger;
    quota_ledger_ref(ledger);      /* keep it alive across the returns */

    for (uint32_t i = 0; i < 12; i++)
        quota_ledger_return(&obs[i]);

    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "returning all twelve restores usage exactly");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_outstanding(ledger), 0,
                   "no obligation is outstanding after the returns");

    quota_obligation_t again = { 0 };
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 4, &again),
                   (uint64_t)STATUS_SUCCESS, "a further charge is admitted");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_capacity(ledger), (uint64_t)grown_cap,
                   "a freed slot is reused rather than growing the ledger again");
    quota_ledger_return(&again);
    quota_ledger_deref(ledger);
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* The claim path resumes AT its hint instead of walking the prefix it has
 * already handed out. Positioning the iterator has three distinct branches --
 * inside the inline run, exactly at the inline boundary, and inside a chunk --
 * and behind them sits a wrap pass that must still find a hole in front of the
 * hint. All four are asserted here against a FRESH ledger, where slot indices
 * are predictable.
 *
 * The task's own claim is released first so the ledger is recreated at its
 * inline capacity; a sibling test that already grew this task's ledger would
 * otherwise make the fill below unbounded. */
static void test_quota_ledger_claim_resumes_at_its_hint(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_ledger_task_release(t);

    quota_obligation_t obs[16] = { 0 };
    uint32_t           held    = 0;
    char               msg[64];

    NTSTATUS st = quota_ledger_charge(t, LEDGER_TEST_TYPE, 4, &obs[0]);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS,
                   "the first charge against a fresh ledger is admitted");
    if (st != STATUS_SUCCESS)
        return;
    held = 1;

    quota_ledger_t *ledger     = obs[0].ledger;
    uint32_t        inline_cap = quota_ledger_capacity(ledger);

    TEST_ASSERT_EQ((uint64_t)obs[0].slot, 0ULL,
                   "the first claim on a fresh ledger takes slot 0");
    /* Loud rather than silent: growing the inline run past this array is a
     * legitimate change, and it must come with an update to this test. */
    TEST_ASSERT(inline_cap >= 2 && inline_cap + 2 <= 16,
                "the fresh inline run fits this test's obligation array");

    if (inline_cap >= 2 && inline_cap + 2 <= 16) {
        /* Branch 1 -- resume INSIDE the inline run: each claim takes the very
         * next slot rather than re-probing the ones already handed out. */
        uint64_t probes_before = quota_ledger_claim_probe_count();

        for (uint32_t i = 1; i < inline_cap; i++) {
            if (quota_ledger_charge(t, LEDGER_TEST_TYPE, 4, &obs[i]) != STATUS_SUCCESS)
                break;
            held++;
            snprintf(msg, sizeof(msg), "claim %u resumes at the hint, taking slot %u",
                     (uint64_t)i, (uint64_t)i);
            TEST_ASSERT_EQ((uint64_t)obs[i].slot, (uint64_t)i, msg);
        }
        TEST_ASSERT_EQ((uint64_t)held, (uint64_t)inline_cap,
                       "the whole inline run is claimed");

        /* THE cost property, and the only assertion here that can tell the two
         * implementations apart: filling the rest of the run resumes at the
         * hint, so each claim examines its own slot and nothing else. Walking
         * the prefix instead would cost 1 + 2 + ... + (inline_cap - 1) probes,
         * which is already over this bound at the smallest legal inline run. */
        uint64_t probes = quota_ledger_claim_probe_count() - probes_before;
        snprintf(msg, sizeof(msg), "filling %u slots cost %u probes, not a prefix walk",
                 (uint64_t)(inline_cap - 1), (uint64_t)probes);
        TEST_ASSERT(probes <= (uint64_t)inline_cap, msg);

        /* The wrap pass -- free the FRONT slot with the hint parked at the end.
         * The hint-to-end pass has nothing left, so the claim must fall back to
         * front-to-hint and reuse slot 0 instead of growing the ledger. */
        if (held == inline_cap) {
            quota_ledger_return(&obs[0]);
            st = quota_ledger_charge(t, LEDGER_TEST_TYPE, 4, &obs[0]);
            TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS,
                           "a charge is admitted from the freed front slot");
            TEST_ASSERT_EQ((uint64_t)obs[0].slot, 0ULL,
                           "the wrap pass reuses the freed front slot");
            TEST_ASSERT_EQ((uint64_t)quota_ledger_capacity(ledger),
                           (uint64_t)inline_cap,
                           "the wrap pass reuses rather than growing the ledger");

            /* Branch 2 -- resume EXACTLY at the inline boundary. Every slot is
             * taken now, so this claim grows the ledger and then resumes at a
             * hint equal to the inline count: the first slot of the new chunk. */
            st = quota_ledger_charge(t, LEDGER_TEST_TYPE, 4, &obs[inline_cap]);
            TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS,
                           "a charge past a full inline run is admitted");
            if (st == STATUS_SUCCESS) {
                held++;
                TEST_ASSERT_EQ((uint64_t)obs[inline_cap].slot, (uint64_t)inline_cap,
                               "the claim at the inline boundary takes the first chunk slot");
                TEST_ASSERT(quota_ledger_capacity(ledger) > inline_cap,
                            "storage grew to hold the claim past the inline run");

                /* Branch 3 -- resume INSIDE a chunk. */
                st = quota_ledger_charge(t, LEDGER_TEST_TYPE, 4, &obs[inline_cap + 1]);
                TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS,
                               "a further chunk-backed charge is admitted");
                if (st == STATUS_SUCCESS) {
                    held++;
                    TEST_ASSERT_EQ((uint64_t)obs[inline_cap + 1].slot,
                                   (uint64_t)(inline_cap + 1),
                                   "the claim inside a chunk takes the next chunk slot");
                }
            }
        }
    }

    for (uint32_t i = 0; i < 16; i++)
        quota_ledger_return(&obs[i]);
    /* Drop the task's lazily-created ledger so the harness's per-test heap
     * accounting does not see it as a leak, exactly as the sibling tests do. */
    quota_ledger_task_release(t);
}

/* THE point of refcounting the ledger: an obligation stays returnable after the
 * task has released its own claim, which is what a reaped task slot looks like to
 * a resource that outlived its creator. And because a holder still exists, the
 * release must NOT report the obligation as a leak. */
static void test_quota_ledger_obligation_outlives_task_claim(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint64_t leaks0 = quota_ledger_leak_count();
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 24, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge admitted");

    /* Drop the TASK's claim, exactly as the reap path does. The obligation's own
     * reference is what must keep the storage alive. */
    quota_ledger_task_release(t);
    TEST_ASSERT_EQ(quota_ledger_leak_count(), leaks0,
                   "an obligation with a live holder is NOT reported as a leak");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_outstanding(ob.ledger), 1,
                   "the obligation survives the task's release");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 24,
                   "its charge is still standing");

    quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "the obligation still returns exactly after the task released");
}

/* The other half of that classification: an obligation whose holder vanished
 * WITHOUT returning it is unreturnable by anyone, so the release must reclaim it
 * and count it as a leak rather than strand the usage for the rest of the boot. */
static void test_quota_ledger_orphan_reclaimed_and_counted(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint64_t leaks0 = quota_ledger_leak_count();
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 40, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge admitted");

    /* Simulate a holder that was freed without returning its obligation: drop
     * the handle's reference and forget the handle, leaving the slot live with
     * only the task's own claim behind it. */
    quota_ledger_deref(ob.ledger);
    ob.ledger = (quota_ledger_t *)0;
    ob.token  = 0;

    quota_ledger_task_release(t);
    TEST_ASSERT_EQ(quota_ledger_leak_count(), leaks0 + 1,
                   "an orphaned obligation is counted as exactly one leak");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "the orphaned charge is reclaimed rather than stranded");
}

/* --- Transactional adjust ------------------------------------------------ */

/* An adjust moves every block the obligation names, in both directions, and the
 * obligation stays returnable at its NEW amount. */
static void test_quota_charge_adjust_both_directions(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 100, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge of 100 admitted");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 100,
                   "usage reflects the original amount");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 250), (uint64_t)STATUS_SUCCESS,
                   "an increase to 250 is admitted");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 250,
                   "an increase moves usage by exactly the delta");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 30), (uint64_t)STATUS_SUCCESS,
                   "a decrease to 30 is admitted");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 30,
                   "a decrease moves usage by exactly the delta");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 30), (uint64_t)STATUS_SUCCESS,
                   "adjusting to the current amount is a success that changes nothing");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 30,
                   "a no-op adjust moves no usage");

    /* The return must credit back the ADJUSTED amount, not the original. */
    quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "the return credits the adjusted amount exactly");
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* THE all-or-nothing property. A refused increase must leave usage AND PEAK
 * byte-identical: committing part of the walk would inflate a peak that no
 * return can ever lower again. */
static void test_quota_charge_adjust_refused_changes_nothing(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t usage0 = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint64_t limit0 = quota_limit(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 10, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge of 10 admitted");

    /* Cap the process block just above what is already charged, so an increase
     * cannot fit. Saved and restored: this is the live task's own block. */
    uint64_t usage_now = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint64_t peak_now;
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota, LEDGER_TEST_TYPE, usage_now + 1),
                   (uint64_t)STATUS_SUCCESS, "a tight limit is installed");
    peak_now = quota_peak(t->quota, LEDGER_TEST_TYPE);

    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 10 + 4096),
                   (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "an increase past the limit is refused");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), usage_now,
                   "a refused adjust moves no usage");
    TEST_ASSERT_EQ(quota_peak(t->quota, LEDGER_TEST_TYPE), peak_now,
                   "a refused adjust lifts no peak (the prefix-commit hazard)");

    /* Still a live, unmodified obligation: the refusal must not have consumed it. */
    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 5), (uint64_t)STATUS_SUCCESS,
                   "the obligation is still adjustable after a refusal");

    (void)quota_set_limit(t->quota, LEDGER_TEST_TYPE, limit0);
    quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), usage0,
                   "the obligation returns exactly after a refused adjust");
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* The token is proof of ownership for a resize exactly as it is for a return: a
 * caller without it must not be able to resize someone else's charge. */
static void test_quota_charge_adjust_requires_the_token(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 20, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge admitted");

    quota_obligation_t wrong = ob;
    wrong.token = ob.token + 1;
    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&wrong, 500),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "a wrong token cannot resize a charge");

    quota_obligation_t zero = ob;
    zero.token = 0;
    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&zero, 500),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "the no-obligation token cannot resize a charge");

    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 20,
                   "a token-refused resize moves no usage");
    quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "the obligation returns exactly");
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* --- Migration ----------------------------------------------------------- */

/* ADOPTION, not a second charge. The absorb has already billed the job for the
 * joiner's usage, so migrating an obligation must move only the OBLIGATION to
 * return it: the job's total is unchanged at migration time, and the eventual
 * return is what credits the job back. */
static void test_quota_ledger_migrate_adopts_absorb_record(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_block_t *job_block = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    if (!job_block)
        return;

    int64_t  saved  = atomic64_read(&t->quota_gate);
    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t    ob  = { 0 };
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 70, &ob),
                   (uint64_t)STATUS_SUCCESS, "pre-join obligation charged");

    /* Stand in for quota_job_absorb_task: the job is charged the joiner's
     * current usage, and the record remembers exactly what it folded in. */
    TEST_ASSERT_EQ((uint64_t)quota_charge(job_block, LEDGER_TEST_TYPE, 70),
                   (uint64_t)STATUS_SUCCESS, "the absorb charges the job block");
    rec.taken[LEDGER_TEST_TYPE] = 70;
    rec.active = 1;

    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "the joiner's gate quiesces for the transition");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_migrate_to_job(t, job_block, &rec), 1,
                   "the outstanding obligation is migrated");
    quota_gate_reopen(t);

    TEST_ASSERT_EQ(rec.taken[LEDGER_TEST_TYPE], 0,
                   "the adopted amount is removed from the absorb record");
    TEST_ASSERT_EQ((uint64_t)rec.active, 0,
                   "a fully adopted record is marked inactive");
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 70,
                   "migration does not change the job's usage by a single byte");

    /* The proof: the return now reaches the JOB too, which is exactly what the
     * absorb-until-detach limitation could not do. */
    quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 0,
                   "returning the migrated obligation credits the job");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "and still credits the process block exactly");

    atomic64_set(&t->quota_gate, saved);
    quota_block_deref(job_block);
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* A refused assignment must leave the job billed for nothing. Reverting the
 * migration restores the absorb record so the caller's unabsorb withdraws the
 * whole amount -- otherwise the job stays charged for a process that never
 * joined it. */
static void test_quota_ledger_unmigrate_restores_absorb_record(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_block_t *job_block = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    if (!job_block)
        return;

    int64_t  saved  = atomic64_read(&t->quota_gate);
    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t    ob  = { 0 };
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 55, &ob),
                   (uint64_t)STATUS_SUCCESS, "pre-join obligation charged");
    TEST_ASSERT_EQ((uint64_t)quota_charge(job_block, LEDGER_TEST_TYPE, 55),
                   (uint64_t)STATUS_SUCCESS, "the absorb charges the job block");
    rec.taken[LEDGER_TEST_TYPE] = 55;
    rec.active = 1;

    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "the joiner's gate quiesces");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_migrate_to_job(t, job_block, &rec), 1,
                   "the obligation is migrated");
    TEST_ASSERT_EQ(rec.taken[LEDGER_TEST_TYPE], 0, "the record was adopted from");

    /* The assignment is then refused, so everything unwinds. */
    TEST_ASSERT_EQ((uint64_t)quota_ledger_unmigrate_from_job(t, job_block, &rec), 1,
                   "the migration is reverted");
    quota_gate_reopen(t);

    TEST_ASSERT_EQ(rec.taken[LEDGER_TEST_TYPE], 55,
                   "the reverted amount is restored to the absorb record");
    TEST_ASSERT_EQ((uint64_t)rec.active, 1, "the restored record is active again");

    /* The obligation no longer names the job, so its return leaves the job's
     * absorbed copy standing for unabsorb to withdraw. */
    quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 55,
                   "a reverted obligation does not credit the job on return");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "the process block is still credited exactly");

    /* Which is exactly what the caller's unabsorb then gives back. */
    quota_job_unabsorb(job_block, &rec);
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 0,
                   "unabsorb withdraws the whole absorbed amount after a revert");

    atomic64_set(&t->quota_gate, saved);
    quota_block_deref(job_block);
    /* Drop the task's own claim so this test's ledger allocation is freed
     * inside the test, exactly as the reap path frees it. Without this the
     * harness's per-test heap accounting reports the lazily-created ledger
     * as a leak -- and it would be right to: nothing else releases it while
     * the charging task is still alive. */
    quota_ledger_task_release(t);
}

/* --- Negative and boundary cases (from the Codex coverage pass) ----------- */

/* The all-or-nothing guarantee must be proven across MORE THAN ONE block, with
 * the refusing block sorted AFTER one that already passed validation. A test that
 * watches only one block would stay green even if an earlier block had been
 * committed before a later one refused -- which is the exact defect the
 * prevalidate-then-commit structure exists to prevent. */
static void test_quota_charge_adjust_refusal_spans_every_block(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_block_t *job_block = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    if (!job_block)
        return;

    int64_t  saved = atomic64_read(&t->quota_gate);
    quota_obligation_t    ob  = { 0 };
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };

    if (quota_ledger_charge(t, LEDGER_TEST_TYPE, 60, &ob) != STATUS_SUCCESS) {
        quota_block_deref(job_block);
        return;
    }

    /* Migrate so the receipt names the process block AND this job block. */
    (void)quota_charge(job_block, LEDGER_TEST_TYPE, 60);
    rec.taken[LEDGER_TEST_TYPE] = 60;
    rec.active = 1;
    if (quota_gate_quiesce(t) == STATUS_SUCCESS) {
        (void)quota_ledger_migrate_to_job(t, job_block, &rec);
        quota_gate_reopen(t);
    }

    /* Tighten the HIGHEST-ADDRESS participating block, so the refusal happens
     * after at least one lower-address block has already prevalidated. The
     * adjust orders its locks by block address, so this is deterministic. */
    quota_block_t *high = ((uintptr_t)job_block > (uintptr_t)t->quota)
                              ? job_block : t->quota;
    quota_block_t *low  = (high == job_block) ? t->quota : job_block;

    uint64_t high_limit0 = quota_limit(high, LEDGER_TEST_TYPE);
    uint64_t high_usage0 = quota_usage(high, LEDGER_TEST_TYPE);
    uint64_t low_usage0  = quota_usage(low,  LEDGER_TEST_TYPE);
    uint64_t high_peak0, low_peak0;

    TEST_ASSERT_EQ((uint64_t)quota_set_limit(high, LEDGER_TEST_TYPE, high_usage0 + 1),
                   (uint64_t)STATUS_SUCCESS, "a tight limit is installed on the high block");
    high_peak0 = quota_peak(high, LEDGER_TEST_TYPE);
    low_peak0  = quota_peak(low,  LEDGER_TEST_TYPE);

    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 60 + 4096),
                   (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "an increase past the high block's limit is refused");

    /* THE assertion: the block that never got to refuse must be untouched too. */
    TEST_ASSERT_EQ(quota_usage(low, LEDGER_TEST_TYPE), low_usage0,
                   "the lower-address block's usage is unchanged by the refusal");
    TEST_ASSERT_EQ(quota_peak(low, LEDGER_TEST_TYPE), low_peak0,
                   "the lower-address block's peak is unchanged by the refusal");
    TEST_ASSERT_EQ(quota_usage(high, LEDGER_TEST_TYPE), high_usage0,
                   "the refusing block's usage is unchanged");
    TEST_ASSERT_EQ(quota_peak(high, LEDGER_TEST_TYPE), high_peak0,
                   "the refusing block's peak is unchanged");

    (void)quota_set_limit(high, LEDGER_TEST_TYPE, high_limit0);
    (void)quota_ledger_return(&ob);
    quota_job_unabsorb(job_block, &rec);
    atomic64_set(&t->quota_gate, saved);
    quota_ledger_task_release(t);
    quota_block_deref(job_block);
}

/* An adjust RAISES charged usage, so it must be refused during a membership
 * transition exactly as a charge is -- otherwise an obligation could grow between
 * the absorb and the publication and the job would be under-charged by the
 * difference. */
static void test_quota_ledger_adjust_refused_during_quiesce(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    int64_t  saved  = atomic64_read(&t->quota_gate);
    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 50, &ob),
                   (uint64_t)STATUS_SUCCESS, "obligation charged");

    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "the owner's gate quiesces");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 500), (uint64_t)STATUS_RETRY,
                   "an adjust during a quiesce is refused with STATUS_RETRY");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 50,
                   "the refused adjust moved no usage");
    quota_gate_reopen(t);

    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 500), (uint64_t)STATUS_SUCCESS,
                   "the same adjust succeeds once the gate reopens");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before + 500,
                   "and then moves usage by the full delta");

    (void)quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "the obligation returns exactly");
    atomic64_set(&t->quota_gate, saved);
    quota_ledger_task_release(t);
}

/* The in-flight ceiling exists so an increment can never carry into the packed
 * state bits and silently reinterpret a busy OPEN gate as some other state. */
static void test_quota_gate_inflight_ceiling_refuses(void)
{
    struct task *t = task_current();
    if (!t)
        return;

    int64_t  saved    = atomic64_read(&t->quota_gate);
    uint64_t retries0 = quota_gate_retry_count();
    uint32_t cause    = QUOTA_GATE_OPEN;

    atomic64_set(&t->quota_gate,
                 QUOTA_GATE_PACK(QUOTA_GATE_OPEN, QUOTA_GATE_COUNT_MAX));

    TEST_ASSERT_EQ((uint64_t)quota_gate_enter(t, &cause), 0,
                   "entry at the in-flight ceiling is refused");
    TEST_ASSERT_EQ((uint64_t)cause, (uint64_t)QUOTA_GATE_CLOSED,
                   "the ceiling refusal is reported as a transient cause");
    TEST_ASSERT_EQ((uint64_t)atomic64_read(&t->quota_gate),
                   (uint64_t)QUOTA_GATE_PACK(QUOTA_GATE_OPEN, QUOTA_GATE_COUNT_MAX),
                   "a refused entry leaves the packed word byte-identical");
    TEST_ASSERT_EQ(quota_gate_retry_count(), retries0 + 1,
                   "the ceiling refusal is counted exactly once");

    atomic64_set(&t->quota_gate, saved);
}

/* Migration must SKIP rather than underflow when the absorb record does not cover
 * an obligation, and must not append the same job twice on a repeated call. Either
 * defect would make the job's usage credited twice on return. */
static void test_quota_ledger_migrate_skips_and_is_idempotent(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_block_t *job_block = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    if (!job_block)
        return;

    int64_t  saved = atomic64_read(&t->quota_gate);
    quota_obligation_t    ob  = { 0 };
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };

    if (quota_ledger_charge(t, LEDGER_TEST_TYPE, 90, &ob) != STATUS_SUCCESS) {
        quota_block_deref(job_block);
        return;
    }

    /* CASE 1: the record covers less than the obligation holds, so adopting it
     * would underflow the record. Migration must decline. */
    rec.taken[LEDGER_TEST_TYPE] = 10;      /* < 90 */
    rec.active = 1;
    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "gate quiesces for the under-covered attempt");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_migrate_to_job(t, job_block, &rec), 0,
                   "an obligation the record cannot cover is NOT migrated");
    quota_gate_reopen(t);
    TEST_ASSERT_EQ(rec.taken[LEDGER_TEST_TYPE], 10,
                   "the under-covered record is left exactly as it was");
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 0,
                   "and the job's usage is untouched");

    /* CASE 2: now cover it properly, migrate once, then migrate AGAIN with
     * residual credit still in the record. The second call must decline. */
    rec.taken[LEDGER_TEST_TYPE] = 90 + 25;         /* 25 of residual credit */
    (void)quota_charge(job_block, LEDGER_TEST_TYPE, 90 + 25);
    TEST_ASSERT_EQ((uint64_t)quota_gate_quiesce(t), (uint64_t)STATUS_SUCCESS,
                   "gate quiesces for the covered attempt");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_migrate_to_job(t, job_block, &rec), 1,
                   "a covered obligation is migrated once");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_migrate_to_job(t, job_block, &rec), 0,
                   "a repeated migration does not adopt the same obligation twice");
    quota_gate_reopen(t);
    TEST_ASSERT_EQ(rec.taken[LEDGER_TEST_TYPE], 25,
                   "exactly one adoption was subtracted from the record");

    /* And the job's books balance exactly once through both routes. */
    (void)quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 25,
                   "the return credits the job exactly once");
    quota_job_unabsorb(job_block, &rec);
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 0,
                   "unabsorb withdraws the residual credit exactly once");

    atomic64_set(&t->quota_gate, saved);
    quota_ledger_task_release(t);
    quota_block_deref(job_block);
}

/* A decrease larger than a block actually holds is an accounting-integrity
 * failure, not a routine refusal: it must fail CLOSED rather than clamp, because
 * clamping would erase some other live obligation's usage. */
static void test_quota_charge_adjust_decrease_underflow_fails_closed(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_block_t *job_block = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);
    if (!job_block)
        return;

    int64_t  saved = atomic64_read(&t->quota_gate);
    quota_obligation_t    ob  = { 0 };
    quota_absorb_record_t rec = { .taken = { 0 }, .active = 0 };

    if (quota_ledger_charge(t, LEDGER_TEST_TYPE, 80, &ob) != STATUS_SUCCESS) {
        quota_block_deref(job_block);
        return;
    }

    (void)quota_charge(job_block, LEDGER_TEST_TYPE, 80);
    rec.taken[LEDGER_TEST_TYPE] = 80;
    rec.active = 1;
    if (quota_gate_quiesce(t) == STATUS_SUCCESS) {
        (void)quota_ledger_migrate_to_job(t, job_block, &rec);
        quota_gate_reopen(t);
    }

    uint64_t proc_usage0 = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint64_t proc_peak0  = quota_peak(t->quota, LEDGER_TEST_TYPE);

    /* Corrupt ONLY the isolated job block, so it holds less than the receipt
     * claims. A decrease must then refuse rather than partially apply. */
    quota_test_poke_usage(job_block, LEDGER_TEST_TYPE, 1);

    TEST_ASSERT_EQ((uint64_t)quota_ledger_adjust(&ob, 5),
                   (uint64_t)STATUS_INTEGER_OVERFLOW,
                   "a decrease larger than a block holds fails closed");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), proc_usage0,
                   "the healthy block's usage is untouched by the refusal");
    TEST_ASSERT_EQ(quota_peak(t->quota, LEDGER_TEST_TYPE), proc_peak0,
                   "the healthy block's peak is untouched by the refusal");
    TEST_ASSERT_EQ(quota_usage(job_block, LEDGER_TEST_TYPE), 1,
                   "the corrupt block is left exactly as it was found");

    /* Restore the corrupted counter so the return balances, then unwind. */
    quota_test_poke_usage(job_block, LEDGER_TEST_TYPE, 80);
    (void)quota_ledger_return(&ob);
    atomic64_set(&t->quota_gate, saved);
    quota_ledger_task_release(t);
    quota_block_deref(job_block);
}

/* A first-ledger allocation failure must be reported, not papered over, and must
 * leave the obligation empty and nothing charged. */
static void test_quota_ledger_charge_handles_allocation_failure(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    /* Start from a task with no ledger, so the very next charge must allocate. */
    quota_ledger_task_release(t);

    uint64_t before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    quota_obligation_t ob = { 0 };

    kmalloc_fail_next();
    NTSTATUS st = quota_ledger_charge(t, LEDGER_TEST_TYPE, 12, &ob);
    kmalloc_fail_countdown_set(0);      /* disarm regardless of the outcome */

    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INSUFFICIENT_RESOURCES,
                   "a ledger charge whose allocation fails reports it");
    TEST_ASSERT_NULL((void *)ob.ledger, "the obligation is left empty");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "nothing is charged when the ledger cannot be allocated");

    /* And the failure strands nothing: the next charge works normally. */
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_TEST_TYPE, 12, &ob),
                   (uint64_t)STATUS_SUCCESS,
                   "a charge after the injected failure succeeds");
    (void)quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), before,
                   "and returns exactly");
    quota_ledger_task_release(t);
}

/* THE rollback branches that matter in production: ob_job_assign refusing AFTER
 * the absorb and the migration have run. A regression in the unwind would leave a
 * job permanently billed for a process that never joined it.
 *
 * The fixture is a REAL Object-Manager-allocated job, not a stack JOB_OBJECT, and
 * that is a safety requirement rather than a preference: JOB_OBJECT is only the
 * BODY of an Ob allocation, so ObReferenceObject computes an OBJECT_HEADER
 * immediately BEFORE it. On a stack body that write lands in unrelated stack
 * memory -- and it would happen precisely when a refusal branch REGRESSED into
 * success, which is the failure this test exists to catch. A test that corrupts
 * the kernel when it fails is worse than no test. (`ob_job_create(ht, NULL)`
 * allocates a real body with its own quota block and registers no name, so it
 * leaves nothing behind in the namespace.)
 *
 * Only refusal branches are exercised, so the live task never joins anything --
 * and the teardown detaches defensively anyway, because assertions record failure
 * and CONTINUE, so a regression must not leave this task a member. */
/* --- The per-task obligation ceiling (section 18) ------------------------- */

/* A second resource type, distinct from LEDGER_TEST_TYPE, so the non-starvation
 * assertion is about two genuinely different classes competing for one task's
 * obligation budget rather than one class competing with itself. */
#define LEDGER_CEILING_TYPE   QUOTA_RES_CRASH_BUFFER
#define LEDGER_OTHER_TYPE     QUOTA_RES_MAPPED_VIEW

/* Restore a gate's STATE without clobbering its in-flight count.
 *
 * A blind `atomic64_set(&t->quota_gate, saved)` is a lost update: the word packs
 * {state, in-flight charger count}, so writing back a snapshot erases any
 * increment another agent made inside the window, and that agent's later
 * gate_exit then decrements a count it no longer owns. The runner is sequential
 * today, which is why the existing tests get away with it; a CAS that preserves
 * the LIVE count costs nothing and does not depend on that staying true. */
static void ledger_gate_restore_state(struct task *t, int64_t saved)
{
    uint32_t want = QUOTA_GATE_STATE(saved);

    for (;;) {
        int64_t now = atomic64_read(&t->quota_gate);

        if (QUOTA_GATE_STATE(now) == want)
            return;
        if (atomic64_cmpxchg(&t->quota_gate, now,
                             QUOTA_GATE_PACK(want, QUOTA_GATE_COUNT(now))) == now)
            return;
    }
}

/* Give back an obligation a NEGATIVE probe was never supposed to receive.
 *
 * Every refusal asserted below is asserted with a recording assertion, which
 * does NOT return -- so when the behaviour under test regresses into success,
 * the test keeps running while holding a live charge and a ledger reference.
 * That strands the ledger (task release cannot orphan-drain one whose refcount
 * is still held), leaves quota usage on the process block, and contaminates
 * every suite that runs afterwards. Reclaiming immediately turns a regression
 * into a clean single failure instead of a cascade.
 *
 * Keyed on the HANDLE, not on the status: a zero-amount charge legitimately
 * reports success with an empty handle, and this must be a no-op for it. */
static void ledger_probe_reclaim(quota_obligation_t *probe)
{
    if (probe && probe->ledger)
        (void)quota_ledger_return(probe);
}

/* Obligations a type can hold before its budget is gone, read from the SAME
 * table the admission rule uses. Not a constant copied beside the header: caps
 * are per type and independently raisable, so a test that hardcoded one number
 * would start failing the moment a consumer's cap moved -- and would be
 * asserting its own copy rather than the policy in force. */
#define LEDGER_CEILING_CAP   quota_ledger_type_cap(LEDGER_CEILING_TYPE)
#define LEDGER_OTHER_CAP     quota_ledger_type_cap(LEDGER_OTHER_TYPE)

/* Charging one type until it is refused must report a POLICY refusal, and must
 * leave every other type its reserved floor.
 *
 * The two halves are one test because the second only means anything against a
 * budget the first has actually exhausted: asserting that a fresh type can
 * charge proves nothing unless the pool it would otherwise draw on is empty.
 *
 * STATUS_QUOTA_EXCEEDED, not STATUS_INSUFFICIENT_RESOURCES, is the whole point.
 * The refusal arrives while the ledger still has hundreds of free slots and
 * could still grow, so a caller can tell "this principal has been given all the
 * obligations policy allows" from "the kernel is out of storage". */
static void test_quota_ledger_ceiling_refuses_and_reserves_per_type(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    /* Start from a task with no ledger so the counts below start at zero. */
    quota_ledger_task_release(t);

    /* Room for the WHOLE task budget: this walk fills every resource type to its
     * cap, not just the two the non-starvation half needs. */
    TEST_SCRATCH_KBUF(raw, QUOTA_LEDGER_TASK_MAX * sizeof(quota_obligation_t));
    quota_obligation_t *obs = (quota_obligation_t *)raw;

    uint32_t held   = 0;
    NTSTATUS refuse = STATUS_SUCCESS;

    /* Walk one type up to its budget, writing EXACTLY cap entries. The
     * over-limit probe is a separate handle rather than one more slot in this
     * array, because the array is sized to the task ceiling: if the cap ever
     * regressed by one, an in-array probe would make the aggregate fill below
     * write one past the end and corrupt memory before any assertion could
     * report the defect. A test must fail loudly on a regression, not scribble. */
    for (uint32_t i = 0; i < LEDGER_CEILING_CAP; i++) {
        if (quota_ledger_charge(t, LEDGER_CEILING_TYPE, 1, &obs[held])
                != STATUS_SUCCESS)
            break;
        held++;
    }

    quota_obligation_t probe = { 0 };
    uint64_t refusals_before   = quota_ledger_ceiling_refusal_count();
    uint64_t by_type_before    = quota_ledger_ceiling_refusals_of(LEDGER_CEILING_TYPE);
    uint64_t other_type_before = quota_ledger_ceiling_refusals_of(LEDGER_OTHER_TYPE);
    refuse = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 1, &probe);
    ledger_probe_reclaim(&probe);

    /* The refusal must be VISIBLE. It happens before quota_charge_chain, so no
     * block failure counter records it -- without a counter of its own a
     * converted consumer could fail every request at the cap while every
     * existing instrument showed a clean subsystem. (The refusal ALSO emits a
     * failure record naming the principal; that half is asserted in
     * test_quota_pressure.c, which owns the ring's test protocol.) */
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusal_count(), refusals_before + 1,
                   "a ceiling refusal is counted exactly once");
    /* And the per-type instrument names the class that was refused, so an
     * operator learns WHICH resource is saturating rather than only that
     * something did. A second type must not have moved. */
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusals_of(LEDGER_CEILING_TYPE),
                   by_type_before + 1,
                   "and is attributed to the resource type that was refused");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusals_of(LEDGER_OTHER_TYPE),
                   other_type_before,
                   "while an unrelated type's refusal count is untouched");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusals_of(
                       (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT), 0,
                   "an undefined type reports no refusals");

    TEST_ASSERT_EQ((uint64_t)refuse, (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "the obligation ceiling refuses with a POLICY status, not "
                   "with heap exhaustion");
    TEST_ASSERT_EQ((uint64_t)held, (uint64_t)LEDGER_CEILING_CAP,
                   "one type may hold exactly its own cap, and not one more");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations_of(t,
                                                             LEDGER_CEILING_TYPE),
                   (uint64_t)LEDGER_CEILING_CAP,
                   "the per-type count matches the obligations admitted");

    /* The refusal is a policy decision taken with storage still available: the
     * ledger never reached the slot ceiling that reports the other status. */
    TEST_ASSERT(quota_ledger_task_obligations(t) < QUOTA_LEDGER_TASK_MAX,
                "the exhausted type has NOT consumed the whole task budget");

    /* NON-STARVATION. Every unit the first type could take is taken, and a
     * second type must still get its WHOLE cap -- that is the property a single
     * aggregate total cannot provide, and the reason the ALPC and KNF
     * conversions can be bounded at all. */
    uint32_t other = 0;
    while (other < LEDGER_OTHER_CAP
           && (held + other) < QUOTA_LEDGER_TASK_MAX) {
        if (quota_ledger_charge(t, LEDGER_OTHER_TYPE, 1,
                                &obs[held + other]) != STATUS_SUCCESS)
            break;
        other++;
    }
    TEST_ASSERT_EQ((uint64_t)other, (uint64_t)LEDGER_OTHER_CAP,
                   "a second type still reaches its FULL cap after another type "
                   "exhausted its own -- the budgets do not compete");

    /* THE AGGREGATE. Two types at their caps is not the task total, so fill
     * every remaining type as well and assert the summed budget IS
     * QUOTA_LEDGER_TASK_MAX -- the derivation the header claims and the static
     * assert pins, now demonstrated against the running admission rule rather
     * than against arithmetic. */
    uint32_t total_held = held + other;
    for (uint32_t ty = 0; ty < (uint32_t)QUOTA_RESOURCE_TYPE_COUNT; ty++) {
        quota_resource_type_t rt = (quota_resource_type_t)ty;
        if (rt == LEDGER_CEILING_TYPE || rt == LEDGER_OTHER_TYPE)
            continue;
        uint32_t of_type = 0;
        /* EVERY write is bounded by the array, not merely by the cap the code
         * is supposed to enforce -- the whole point of this test is that the
         * cap might not hold. */
        while (of_type < quota_ledger_type_cap(rt)
               && total_held < QUOTA_LEDGER_TASK_MAX) {
            if (quota_ledger_charge(t, rt, 1, &obs[total_held]) != STATUS_SUCCESS)
                break;
            total_held++;
            of_type++;
        }
    }

    TEST_ASSERT_EQ((uint64_t)total_held, (uint64_t)QUOTA_LEDGER_TASK_MAX,
                   "every type reaches its cap at the same time, and the sum IS "
                   "the task ceiling");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t),
                   (uint64_t)QUOTA_LEDGER_TASK_MAX,
                   "the task-wide count agrees with the obligations admitted");

    /* At the aggregate ceiling EVERY type is refused, and still by policy. */
    quota_obligation_t over = { 0 };
    NTSTATUS over_st = quota_ledger_charge(t, LEDGER_OTHER_TYPE, 1, &over);
    ledger_probe_reclaim(&over);
    TEST_ASSERT_EQ((uint64_t)over_st, (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "a task at its aggregate ceiling is refused by policy");

    for (uint32_t i = 0; i < total_held; i++)
        (void)quota_ledger_return(&obs[i]);

    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 0,
                   "returning every obligation restores the whole budget");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 1,
                                                 &obs[0]),
                   (uint64_t)STATUS_SUCCESS,
                   "the ceiling admits again once the budget is given back");
    (void)quota_ledger_return(&obs[0]);
    quota_ledger_task_release(t);
}

/* Every path that fails AFTER the budget is reserved must give it back, or a
 * task would be charged for obligations it never received. */
static void test_quota_ledger_ceiling_restored_on_failure_paths(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_ledger_task_release(t);

    /* THE SCRATCH BUFFER IS TAKEN FIRST, before any obligation exists.
     * TEST_SCRATCH_KBUF returns outright when the allocation or its cleanup
     * registration fails, so acquiring an obligation ahead of it would strand
     * that obligation on exactly the memory-pressure path where cleanup matters
     * most. Nothing is owed yet at this point, so that early return is free.
     *
     * The inline slot count is private to quota_ledger.c, so the fill below is
     * driven by the PUBLIC capacity query and bounded by the per-type cap -- the
     * most obligations of this type that could ever be admitted. */
    TEST_SCRATCH_KBUF(fill_raw, LEDGER_CEILING_CAP
                                * sizeof(quota_obligation_t));
    quota_obligation_t *fill = (quota_obligation_t *)fill_raw;

    quota_obligation_t ob = { 0 };

    /* Take one real obligation so the ledger exists and the counter has a
     * nonzero baseline that a mis-restore would visibly disturb. */
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8, &ob),
                   (uint64_t)STATUS_SUCCESS, "baseline obligation admitted");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 1,
                   "the baseline obligation is counted");

    /* ZERO AMOUNT: the charge succeeds owing nothing, and it RESERVES NOTHING --
     * it is answered before the ledger is acquired or any budget is taken. That
     * is what this asserts: an unchanged count and an empty handle. It is
     * deliberately NOT a rollback proof; the post-reservation token==0 branch is
     * unreachable while the chain issues token 0 only for a zero amount, and
     * claiming this exercised it would be evidence for something it never
     * touches. The reachable rollbacks are asserted below (claim failure and a
     * refusal inside the chain). */
    quota_obligation_t zero = { 0 };
    NTSTATUS zero_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 0, &zero);
    int zero_empty = (zero.ledger == (quota_ledger_t *)0);
    ledger_probe_reclaim(&zero);
    TEST_ASSERT_EQ((uint64_t)zero_st, (uint64_t)STATUS_SUCCESS,
                   "a zero charge is admitted");
    TEST_ASSERT(zero_empty, "a zero charge holds no obligation");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 1,
                   "a zero charge reserves no budget at all");

    /* CHAIN FAILURE, and it must fail INSIDE the chain -- after the budget was
     * reserved AND the slot claimed -- or it does not test the rollback it
     * claims to. An out-of-range amount no longer qualifies: it is rejected up
     * front, before the ledger is even acquired, so it would exercise nothing
     * and the unchanged-count assertion would hold even if the post-reservation
     * cleanup had stopped releasing anything.
     *
     * Closing the gate is a genuine one. The type is far below its cap, so the
     * reservation succeeds and the slot is claimed; quota_charge_chain then
     * enters the gate, finds it CLOSED, and refuses -- landing exactly on the
     * branch that must give back both the slot and the budget. */
    quota_obligation_t bad = { 0 };
    uint32_t cap_before = quota_ledger_capacity(ob.ledger);

    /* THE QUIESCE IS CHECKED BEFORE ANYTHING IS DONE WITH IT. A recording
     * assertion does not return, so an unconditional reopen after a FAILED
     * quiesce would either reopen a close this test never owned, or -- if the
     * task had been sealed -- do nothing while leaving it sealed for every test
     * that follows. Only a confirmed close is reopened. */
    NTSTATUS quiesced = quota_gate_quiesce(t);
    TEST_ASSERT_EQ((uint64_t)quiesced, (uint64_t)STATUS_SUCCESS,
                   "the gate closes so the chain refuses mid-charge");
    if (quiesced == STATUS_SUCCESS) {
        /* Enough forced failures that a LEAKED slot could not hide. Asserting
         * on quota_ledger_outstanding would not do it: that counts only slots
         * whose receipt reached ACTIVE, and this refusal lands before the charge
         * publishes, so a claimed-but-never-cleared slot is invisible to it. A
         * leak would instead consume real capacity, so drive more failures than
         * the ledger has slots and assert it never had to GROW -- which it would
         * have to if each attempt kept its slot. */
        for (uint32_t i = 0; i < cap_before + 2u; i++) {
            NTSTATUS st_i = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8, &bad);
            ledger_probe_reclaim(&bad);
            TEST_ASSERT_EQ((uint64_t)st_i, (uint64_t)STATUS_RETRY,
                           "a charge refused inside the chain reports the "
                           "chain's status");
        }
        quota_gate_reopen(t);
    }

    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 1,
                   "a charge refused INSIDE the chain gives its reserved budget "
                   "back, every time");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_capacity(ob.ledger),
                   (uint64_t)cap_before,
                   "and releases the slot it had already claimed -- the ledger "
                   "never had to grow to serve the retries");

    /* The up-front rejection is still worth asserting -- just as what it now is:
     * a malformed request refused before any state is touched. */
    quota_obligation_t huge = { 0 };
    NTSTATUS huge_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE,
                                           (uint64_t)QUOTA_AMOUNT_MAX + 1u,
                                           &huge);
    ledger_probe_reclaim(&huge);
    TEST_ASSERT_EQ((uint64_t)huge_st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "an out-of-range amount is a malformed request");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 1,
                   "and reserves nothing at all");

    /* CLAIM FAILURE, and it must ACTUALLY fail. Injecting an allocation failure
     * while free slots remain proves nothing: the claim takes an inline slot and
     * never allocates, so the injection is never consulted and the assertion
     * below would pass over a rollback that leaks budget. Fill every slot the
     * ledger currently has first, so the next claim is forced to GROW and the
     * injected failure is the thing it hits. */
    uint32_t filled = 0;
    while (filled < LEDGER_CEILING_CAP - 1u
           && quota_ledger_outstanding(ob.ledger) < quota_ledger_capacity(ob.ledger)) {
        if (quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8,
                                &fill[filled]) != STATUS_SUCCESS)
            break;
        filled++;
    }
    TEST_ASSERT_EQ((uint64_t)quota_ledger_outstanding(ob.ledger),
                   (uint64_t)quota_ledger_capacity(ob.ledger),
                   "every slot the ledger has is occupied, so the next claim "
                   "must grow");

    uint32_t counted_before = quota_ledger_task_obligations(t);
    quota_obligation_t starved = { 0 };
    kmalloc_fail_next();
    NTSTATUS st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8, &starved);
    kmalloc_fail_countdown_set(0);

    int starved_empty = (starved.ledger == (quota_ledger_t *)0);
    ledger_probe_reclaim(&starved);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INSUFFICIENT_RESOURCES,
                   "a charge whose slot claim cannot grow reports storage "
                   "exhaustion, NOT a policy refusal");
    TEST_ASSERT(starved_empty, "the failed claim leaves the obligation empty");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t),
                   (uint64_t)counted_before,
                   "a charge whose slot claim fails gives its budget back");

    for (uint32_t i = 0; i < filled; i++)
        (void)quota_ledger_return(&fill[i]);

    /* AN UNDEFINED TYPE never reserves at all: it is refused before the counter
     * array is indexed, which is also what keeps it from being indexed out of
     * bounds. */
    quota_obligation_t off = { 0 };
    NTSTATUS off_st = quota_ledger_charge(t,
                          (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT, 8,
                          &off);
    ledger_probe_reclaim(&off);
    TEST_ASSERT_EQ((uint64_t)off_st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "an undefined resource type is refused before it is indexed");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 1,
                   "a refused type moves no counter");

    (void)quota_ledger_return(&ob);
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 0,
                   "the baseline obligation is given back exactly once");
    quota_ledger_task_release(t);
}

/* A DUPLICATE handle of an already-returned obligation reports the same
 * "credited" outcome as the handle that really completed it -- the receipt reads
 * IDLE at that generation for both. Only the caller whose epoch matched actually
 * freed the slot, so only that one may give the budget back. A decrement keyed
 * on the outcome instead would run twice for one obligation and drive the count
 * below the obligations that exist, which is a ceiling BYPASS: the task would be
 * admitted past its limit for as long as the deficit lasted.
 *
 * The duplicate takes its OWN reference, exactly as the raised-IRQL duplicate
 * test does; a bare struct copy would be two handles over one reference, which
 * is a lifetime error rather than a case this contract covers. */
static void test_quota_ledger_ceiling_survives_duplicate_return(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_ledger_task_release(t);

    quota_obligation_t ob = { 0 };
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 16, &ob),
                   (uint64_t)STATUS_SUCCESS, "obligation admitted");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 1,
                   "one obligation is counted against the ceiling");

    quota_obligation_t dup = ob;
    quota_ledger_ref(dup.ledger);

    (void)quota_ledger_return(&ob);
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 0,
                   "the returning handle gives the budget back");

    (void)quota_ledger_return(&dup);
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 0,
                   "the duplicate does not give the same budget back twice");

    /* The count is not merely reported as zero -- it is actually zero, so the
     * full budget is still available. A double decrement would have left a
     * deficit that admits one charge too many. */
    quota_obligation_t after = { 0 };
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 16,
                                                 &after),
                   (uint64_t)STATUS_SUCCESS, "the budget is intact afterwards");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 1,
                   "and it counts exactly one obligation again");
    (void)quota_ledger_return(&after);
    quota_ledger_task_release(t);
}

/* A return above PASSIVE_LEVEL is completed later by the drain, and the budget
 * must come back when the obligation is actually settled -- not when the handle
 * was emptied. The completion is shared verbatim with the inline path, which is
 * exactly why this must be asserted rather than assumed. */
static void test_quota_ledger_ceiling_released_by_deferred_drain(void)
{
    quota_ledger_test_hold(1);
    struct task       *t  = task_current();
    quota_obligation_t ob = { 0 };
    KIRQL              old;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 4, &ob),
                   (uint64_t)STATUS_SUCCESS, "obligation admitted");
    uint32_t counted = quota_ledger_task_obligations_of(t, LEDGER_CEILING_TYPE);

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    (void)quota_ledger_return(&ob);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations_of(t,
                                                             LEDGER_CEILING_TYPE),
                   (uint64_t)counted,
                   "a deferred return still holds its budget: the obligation is "
                   "owed to the drain, not settled");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 1u,
                   "the drain completes the deferred obligation");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations_of(t,
                                                             LEDGER_CEILING_TYPE),
                   (uint64_t)(counted - 1u),
                   "and the drain is what gives the budget back");

    /* ZERO IS NOT PROOF OF EXACTLY-ONCE. The query clamps a negative raw counter
     * to zero, so a deferred completion that released the same budget twice
     * would read as zero here and satisfy the assertion above while leaving the
     * task one obligation richer than its cap allows. Charging again and
     * demanding EXACTLY one is what distinguishes a true zero from a masked
     * deficit: against a counter sitting at -1 this reads zero, not one. */
    quota_obligation_t after = { 0 };
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 4,
                                                 &after),
                   (uint64_t)STATUS_SUCCESS, "a further charge is admitted");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations_of(t,
                                                             LEDGER_CEILING_TYPE),
                   (uint64_t)counted,
                   "the counter is a TRUE zero, not a negative one clamped by "
                   "the query: a doubled release would read one short here");
    (void)quota_ledger_return(&after);

    quota_ledger_task_release(t);
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* At the ceiling, the GATE still outranks the policy. A task that is dying or
 * mid-membership-transition must not be told STATUS_QUOTA_EXCEEDED just because
 * its budget happens to be full: that is a permanent, administrator-actionable
 * refusal standing in for "you are dying" or "retry in a moment". The ceiling is
 * checked before quota_charge_chain -- which is where the authoritative liveness
 * answer lives -- so the precedence has to be reproduced explicitly, and that
 * makes it exactly the kind of thing a later refactor drops silently.
 *
 * The gate word is saved and restored: a seal is deliberately terminal (reopen
 * refuses to resurrect one), so leaving the current task sealed would break
 * every quota test that runs after this one. */
static void test_quota_ledger_ceiling_yields_to_the_gate(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_ledger_task_release(t);

    TEST_SCRATCH_KBUF(raw, LEDGER_CEILING_CAP * sizeof(quota_obligation_t));
    quota_obligation_t *obs = (quota_obligation_t *)raw;

    uint32_t held = 0;
    while (held < LEDGER_CEILING_CAP) {
        if (quota_ledger_charge(t, LEDGER_CEILING_TYPE, 1, &obs[held])
                != STATUS_SUCCESS)
            break;
        held++;
    }
    TEST_ASSERT_EQ((uint64_t)held, (uint64_t)LEDGER_CEILING_CAP,
                   "the type is at its cap, so the next charge is at the ceiling");

    /* A SEPARATE HANDLE PER PROBE, each reclaimed immediately if the charge is
     * wrongly admitted. Reusing one handle across the three probes is a leak
     * waiting for the first regression: quota_ledger_charge_from empties its
     * output before it decides anything, so an unexpectedly admitted obligation
     * would be erased by the NEXT probe, stranding a live charge and a ledger
     * reference that task release then cannot orphan-drain -- contaminating
     * every test that runs after this one. A failing assertion records and
     * continues, so "it cannot happen" is not a cleanup strategy. */
    quota_obligation_t open_probe = { 0 };
    uint64_t refusals = quota_ledger_ceiling_refusal_count();
    NTSTATUS open_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 1, &open_probe);
    ledger_probe_reclaim(&open_probe);
    TEST_ASSERT_EQ((uint64_t)open_st, (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "with the gate OPEN the ceiling reports a policy refusal");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusal_count(), refusals + 1,
                   "and THAT is what the ceiling counter records -- one policy "
                   "refusal, one count, and nothing counted for a status the "
                   "caller never received");
    refusals = quota_ledger_ceiling_refusal_count();

    int64_t saved = atomic64_read(&t->quota_gate);

    /* CLOSED: a membership transition is in progress, so the honest answer is
     * "retry", not "you are over quota". */
    quota_obligation_t closed_probe = { 0 };
    NTSTATUS closed_quiesce = quota_gate_quiesce(t);
    TEST_ASSERT_EQ((uint64_t)closed_quiesce, (uint64_t)STATUS_SUCCESS,
                   "the gate closes for the transition");
    /* PROBE AND REOPEN ONLY ON A CLOSE THIS TEST ACTUALLY OWNS. The assertion
     * above records and continues, so a quiesce refused because another
     * transition holds CLOSED would otherwise be followed by a reopen of THAT
     * transition's gate -- and a refusal because the task is sealed would leave
     * it sealed while the reopen quietly did nothing. */
    if (closed_quiesce == STATUS_SUCCESS) {
        NTSTATUS closed_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 1,
                                                 &closed_probe);
        ledger_probe_reclaim(&closed_probe);
        quota_gate_reopen(t);
        TEST_ASSERT_EQ((uint64_t)closed_st, (uint64_t)STATUS_RETRY,
                       "a CLOSED gate outranks the ceiling: retry, not "
                       "over-quota");
        /* A gate refusal is NOT a policy refusal, and the counter must not say
         * it was: a burst of membership transitions against a task sitting at
         * its cap would otherwise read as a quota problem. */
        TEST_ASSERT_EQ(quota_ledger_ceiling_refusal_count(), refusals,
                       "a CLOSED-gate refusal does not count as a ceiling "
                       "refusal");
    }

    /* SEALED: the task is dying, and no amount of returning obligations would
     * make this charge succeed -- so it must not look like a quota problem. The
     * gate word is restored BEFORE the assertion, because a seal is terminal and
     * an early return here would leave the task permanently unchargeable. */
    quota_obligation_t sealed_probe = { 0 };
    NTSTATUS sealed_st;
    quota_gate_seal(t);
    sealed_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 1, &sealed_probe);
    ledger_gate_restore_state(t, saved);
    ledger_probe_reclaim(&sealed_probe);
    TEST_ASSERT_EQ((uint64_t)sealed_st, (uint64_t)STATUS_PROCESS_IS_TERMINATING,
                   "a SEALED gate outranks the ceiling: terminating, not "
                   "over-quota");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusal_count(), refusals,
                   "and a teardown refusal does not count as a ceiling refusal "
                   "either");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations_of(t,
                                                             LEDGER_CEILING_TYPE),
                   (uint64_t)LEDGER_CEILING_CAP,
                   "none of the three refusals consumed or leaked budget");

    /* AMOUNT SEMANTICS ALSO OUTRANK THE CEILING, and the two public entry points
     * must agree about it. A zero charge owes nothing, so a full cap is none of
     * its business; a malformed amount is a malformed request, not a quota
     * problem. Both are asserted through BOTH wrappers, because the defect this
     * guards against was precisely that they disagreed once a type filled up. */
    quota_obligation_t zero_at_cap = { 0 };
    NTSTATUS zero_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 0,
                                           &zero_at_cap);
    ledger_probe_reclaim(&zero_at_cap);
    TEST_ASSERT_EQ((uint64_t)zero_st, (uint64_t)STATUS_SUCCESS,
                   "a zero charge succeeds even with the type at its cap");

    quota_obligation_t zero_cur = { 0 };
    NTSTATUS zero_cur_st = quota_ledger_charge_current(LEDGER_CEILING_TYPE, 0,
                                                       &zero_cur);
    ledger_probe_reclaim(&zero_cur);
    TEST_ASSERT_EQ((uint64_t)zero_cur_st, (uint64_t)zero_st,
                   "and the current-task form answers a zero charge identically");

    quota_obligation_t huge = { 0 };
    NTSTATUS huge_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE,
                                           (uint64_t)QUOTA_AMOUNT_MAX + 1u,
                                           &huge);
    ledger_probe_reclaim(&huge);
    TEST_ASSERT_EQ((uint64_t)huge_st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "a malformed amount at the cap is a malformed request, not "
                   "a quota refusal");

    quota_obligation_t huge_cur = { 0 };
    NTSTATUS huge_cur_st = quota_ledger_charge_current(LEDGER_CEILING_TYPE,
                               (uint64_t)QUOTA_AMOUNT_MAX + 1u, &huge_cur);
    ledger_probe_reclaim(&huge_cur);
    TEST_ASSERT_EQ((uint64_t)huge_cur_st, (uint64_t)huge_st,
                   "and both entry points agree on it");

    /* NULL is a diagnostic answer, never a fault. */
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations((struct task *)0), 0,
                   "the task-wide query answers NULL rather than faulting");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations_of((struct task *)0,
                                                              LEDGER_CEILING_TYPE),
                   0, "and so does the per-type query");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_type_cap(
                       (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT), 0,
                   "an undefined type has no cap");

    /* EVERY DEFINED type must have a NONZERO cap. The compile-time guard catches
     * a type appended without a cap row, because the taxonomy appends at the
     * end; it cannot catch a zero left in a middle row, and a zero cap would
     * refuse every charge of a perfectly valid resource type with
     * STATUS_QUOTA_EXCEEDED -- a whole subsystem silently unable to charge. */
    uint32_t cap_sum = 0;
    for (uint32_t ty = 0; ty < (uint32_t)QUOTA_RESOURCE_TYPE_COUNT; ty++) {
        uint32_t cap = quota_ledger_type_cap((quota_resource_type_t)ty);
        TEST_ASSERT(cap > 0, "every defined resource type has a nonzero "
                             "obligation cap");
        cap_sum += cap;
    }
    TEST_ASSERT_EQ((uint64_t)cap_sum, (uint64_t)QUOTA_LEDGER_TASK_MAX,
                   "the task ceiling is exactly the per-type caps summed");

    for (uint32_t i = 0; i < held; i++)
        (void)quota_ledger_return(&obs[i]);
    quota_ledger_task_release(t);
}

/* A NEGATIVE obligation count is corruption, and the charge path must fail
 * CLOSED on it rather than treating the deficit as free budget. That branch
 * guards a state no reachable path produces, which is exactly why it needs a
 * manufactured one: a net nobody ever fires is a net nobody knows is connected.
 *
 * The status matters as much as the refusal. Reporting corruption as
 * STATUS_QUOTA_EXCEEDED would bump the ceiling counters and tell an operator a
 * task holding (apparently) zero obligations is at its limit -- sending them to
 * raise a cap while the real failure stayed hidden. */
static void test_quota_ledger_negative_count_fails_closed(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_ledger_task_release(t);

    quota_obligation_t seed = { 0 };
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8, &seed),
                   (uint64_t)STATUS_SUCCESS, "a ledger exists to corrupt");

    uint64_t ceiling_before   = quota_ledger_ceiling_refusal_count();
    uint64_t integrity_before = quota_ledger_integrity_refusal_count();

    int32_t restored = quota_ledger_test_set_obligations(t, LEDGER_CEILING_TYPE, -1);

    quota_obligation_t probe = { 0 };
    NTSTATUS st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8, &probe);
    ledger_probe_reclaim(&probe);

    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_INTEGER_OVERFLOW,
                   "a negative obligation count fails closed as an integrity "
                   "failure, NOT as policy exhaustion");
    TEST_ASSERT_EQ(quota_ledger_integrity_refusal_count(), integrity_before + 1,
                   "and is recorded as an integrity refusal");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusal_count(), ceiling_before,
                   "while the policy-ceiling counter is untouched, so a cap "
                   "raise is not suggested for a corruption");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations_of(t,
                                                             LEDGER_CEILING_TYPE),
                   0, "the public query clamps the deficit -- which is why the "
                      "counter above is the only visible evidence");

    /* SYMMETRIC: a count ABOVE the cap is just as unreachable as one below zero
     * -- the reservation only ever increments from a value it observed strictly
     * below the cap -- so it is corruption too, and calling it exhaustion would
     * point an operator at the cap, the one number that is not the problem. */
    /* BOTH policy instruments are snapshotted, not just the global one.
     * Production records a policy refusal in the aggregate AND in the per-type
     * counter, so a regression that moved only the per-type cell for cap+1
     * would pass a global-only assertion while still pointing an operator at
     * that resource's cap -- the exact misclassification this guards. */
    integrity_before = quota_ledger_integrity_refusal_count();
    ceiling_before   = quota_ledger_ceiling_refusal_count();
    uint64_t by_type_before = quota_ledger_ceiling_refusals_of(LEDGER_CEILING_TYPE);
    uint64_t other_before   = quota_ledger_ceiling_refusals_of(LEDGER_OTHER_TYPE);
    (void)quota_ledger_test_set_obligations(t, LEDGER_CEILING_TYPE,
                                            (int32_t)LEDGER_CEILING_CAP + 1);

    quota_obligation_t over = { 0 };
    NTSTATUS over_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8, &over);
    ledger_probe_reclaim(&over);

    TEST_ASSERT_EQ((uint64_t)over_st, (uint64_t)STATUS_INTEGER_OVERFLOW,
                   "a count ABOVE the cap is corruption too, not exhaustion");
    TEST_ASSERT_EQ(quota_ledger_integrity_refusal_count(), integrity_before + 1,
                   "and lands on the integrity counter");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusal_count(), ceiling_before,
                   "leaving the aggregate policy counter untouched");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusals_of(LEDGER_CEILING_TYPE),
                   by_type_before,
                   "and the PER-TYPE policy counter untouched, so no cap is "
                   "implicated");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusals_of(LEDGER_OTHER_TYPE),
                   other_before, "and no unrelated type either");

    /* And EXACTLY at the cap is still the ordinary policy refusal -- on BOTH
     * policy instruments, and on neither integrity nor an unrelated type. */
    integrity_before = quota_ledger_integrity_refusal_count();
    ceiling_before   = quota_ledger_ceiling_refusal_count();
    by_type_before   = quota_ledger_ceiling_refusals_of(LEDGER_CEILING_TYPE);
    other_before     = quota_ledger_ceiling_refusals_of(LEDGER_OTHER_TYPE);
    (void)quota_ledger_test_set_obligations(t, LEDGER_CEILING_TYPE,
                                            (int32_t)LEDGER_CEILING_CAP);

    quota_obligation_t at_cap = { 0 };
    NTSTATUS at_cap_st = quota_ledger_charge(t, LEDGER_CEILING_TYPE, 8, &at_cap);
    ledger_probe_reclaim(&at_cap);

    TEST_ASSERT_EQ((uint64_t)at_cap_st, (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "exactly at the cap is the legitimate policy refusal");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusal_count(), ceiling_before + 1,
                   "counted as policy in the aggregate");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusals_of(LEDGER_CEILING_TYPE),
                   by_type_before + 1, "and against its own resource type");
    TEST_ASSERT_EQ(quota_ledger_ceiling_refusals_of(LEDGER_OTHER_TYPE),
                   other_before, "while an unrelated type is untouched");
    TEST_ASSERT_EQ(quota_ledger_integrity_refusal_count(), integrity_before,
                   "and not as corruption");

    (void)quota_ledger_test_set_obligations(t, LEDGER_CEILING_TYPE, restored);
    (void)quota_ledger_return(&seed);
    quota_ledger_task_release(t);
}

/* --- Charge-source pass-through (section 18) ------------------------------ */

/* A ledger charge that names its subsystem must land that source in the
 * system-wide attribution table, exactly as quota_charge_current_from does. The
 * ledger charged with flags 0 before this existed, so a consumer converted from
 * an embedded receipt to a ledger obligation lost its attribution entirely. */
static void test_quota_ledger_charge_from_attributes_the_source(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_ledger_task_release(t);

    int64_t before_usage = quota_source_usage(QUOTA_SOURCE_DIAG,
                                              LEDGER_CEILING_TYPE);
    int64_t before_count = quota_source_charges(QUOTA_SOURCE_DIAG,
                                                LEDGER_CEILING_TYPE);

    quota_obligation_t ob = { 0 };
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_from(t, LEDGER_CEILING_TYPE, 24,
                                                      QUOTA_SOURCE_DIAG, &ob),
                   (uint64_t)STATUS_SUCCESS, "an attributed ledger charge is admitted");
    TEST_ASSERT_EQ((uint64_t)quota_source_usage(QUOTA_SOURCE_DIAG,
                                                LEDGER_CEILING_TYPE),
                   (uint64_t)(before_usage + 24),
                   "the named source holds the ledger charge");
    TEST_ASSERT_EQ((uint64_t)quota_source_charges(QUOTA_SOURCE_DIAG,
                                                  LEDGER_CEILING_TYPE),
                   (uint64_t)(before_count + 1),
                   "and the charge is counted against that source");

    (void)quota_ledger_return(&ob);
    TEST_ASSERT_EQ((uint64_t)quota_source_usage(QUOTA_SOURCE_DIAG,
                                                LEDGER_CEILING_TYPE),
                   (uint64_t)before_usage,
                   "returning the obligation credits the source back");

    /* An UNATTRIBUTED charge stays unattributed: quota_ledger_charge is the
     * unnamed form, and UNKNOWN is deliberately never recorded in the table. */
    int64_t rows_before[QUOTA_SOURCE_COUNT];
    for (uint32_t src = 0; src < (uint32_t)QUOTA_SOURCE_COUNT; src++)
        rows_before[src] = quota_source_usage((quota_charge_source_t)src,
                                              LEDGER_CEILING_TYPE);

    quota_obligation_t plain = { 0 };
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge(t, LEDGER_CEILING_TYPE, 24,
                                                 &plain),
                   (uint64_t)STATUS_SUCCESS, "an unnamed ledger charge is admitted");
    /* EVERY row, not just DIAG. Checking one source cannot tell "attributed to
     * nobody" from "attributed to the wrong subsystem": a charge that encoded
     * IPC instead of nothing would leave DIAG untouched and UNKNOWN at zero, and
     * both assertions would pass while the receipt carried a wrong label. */
    for (uint32_t src = 0; src < (uint32_t)QUOTA_SOURCE_COUNT; src++)
        TEST_ASSERT_EQ((uint64_t)quota_source_usage((quota_charge_source_t)src,
                                                    LEDGER_CEILING_TYPE),
                       (uint64_t)rows_before[src],
                       "an unnamed charge moves no source row at all");
    (void)quota_ledger_return(&plain);

    /* THE CURRENT-TASK WRAPPER CARRIES A VALID SOURCE TOO. Proving only that it
     * REFUSES an undefined source leaves its actual pass-through contract
     * unproved: a regression that kept the validation but passed
     * QUOTA_SOURCE_UNKNOWN downstream would still refuse the bad source here,
     * while every consumer converted through this entry point silently lost its
     * attribution -- which is the exact defect this section exists to fix. */
    /* EVERY valid source, not one. A single probe naming DIAG and expecting DIAG
     * cannot tell pass-through from a hardcoded value: a wrapper that ignored
     * its `source` argument and always forwarded DIAG would satisfy it exactly,
     * while every caller naming IPC or MEMORY was silently misattributed.
     * Walking the whole taxonomy makes the assertion about the PARAMETER.
     *
     * UNKNOWN is skipped because it is deliberately never recorded (quota.h):
     * "unattributed" lives on the receipt, not in this table, and the unnamed
     * charge above already proves that direction. */
    for (uint32_t named = 1; named < (uint32_t)QUOTA_SOURCE_COUNT; named++) {
        quota_charge_source_t src_id = (quota_charge_source_t)named;
        int64_t cur_rows[QUOTA_SOURCE_COUNT];
        for (uint32_t src = 0; src < (uint32_t)QUOTA_SOURCE_COUNT; src++)
            cur_rows[src] = quota_source_usage((quota_charge_source_t)src,
                                               LEDGER_CEILING_TYPE);
        int64_t cur_charges = quota_source_charges(src_id, LEDGER_CEILING_TYPE);

        quota_obligation_t cur = { 0 };
        TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current_from(
                           LEDGER_CEILING_TYPE, 32, src_id, &cur),
                       (uint64_t)STATUS_SUCCESS,
                       "the current-task form admits an attributed charge");
        for (uint32_t src = 0; src < (uint32_t)QUOTA_SOURCE_COUNT; src++) {
            int64_t want = cur_rows[src] + ((src == named) ? 32 : 0);
            TEST_ASSERT_EQ((uint64_t)quota_source_usage(
                               (quota_charge_source_t)src, LEDGER_CEILING_TYPE),
                           (uint64_t)want,
                           "the current-task form moves ONLY the row of the "
                           "source it was GIVEN");
        }
        TEST_ASSERT_EQ((uint64_t)quota_source_charges(src_id,
                                                      LEDGER_CEILING_TYPE),
                       (uint64_t)(cur_charges + 1),
                       "and counts one charge against that same source");

        (void)quota_ledger_return(&cur);
        TEST_ASSERT_EQ((uint64_t)quota_source_usage(src_id, LEDGER_CEILING_TYPE),
                       (uint64_t)cur_rows[named],
                       "returning it credits that source back");
    }

    /* A source the taxonomy does not define is refused OUTRIGHT, and before any
     * ledger state is touched -- never encoded into flags and never laundered
     * into a success by an early return. */
    quota_obligation_t bogus = { 0 };
    NTSTATUS bogus_st = quota_ledger_charge_from(t, LEDGER_CEILING_TYPE, 24,
                            (quota_charge_source_t)QUOTA_SOURCE_COUNT, &bogus);
    int bogus_empty = (bogus.ledger == (quota_ledger_t *)0);
    ledger_probe_reclaim(&bogus);
    TEST_ASSERT_EQ((uint64_t)bogus_st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "an undefined charge source is refused");
    TEST_ASSERT(bogus_empty, "a refused source leaves the obligation empty");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_task_obligations(t), 0,
                   "and reserves no budget");

    /* Its OWN handle: sharing one with the probe above would let a regressed
     * first probe's live obligation be erased by this call's output clear. */
    /* SENTINEL-FILLED, because "the handle is emptied on every failure" is what
     * lets a caller run one unconditional cleanup path instead of branching on
     * the status. A refusal that returned before clearing would hand back these
     * sentinels, and that cleanup would treat them as a live obligation. */
    quota_obligation_t bogus_cur = {
        .ledger = (quota_ledger_t *)~(uintptr_t)0,
        .slot   = 0xDEADBEEFu,
        .token  = 0xFEEDFACEULL,
        .epoch  = 0xC0FFEEULL,
    };
    NTSTATUS bogus_cur_st = quota_ledger_charge_current_from(LEDGER_CEILING_TYPE,
                                24, (quota_charge_source_t)QUOTA_SOURCE_COUNT,
                                &bogus_cur);
    TEST_ASSERT_EQ((uint64_t)bogus_cur_st, (uint64_t)STATUS_INVALID_PARAMETER,
                   "the current-task form refuses it too");
    TEST_ASSERT_NULL((void *)bogus_cur.ledger,
                     "and empties the handle rather than returning early over "
                     "the caller's storage");
    TEST_ASSERT_EQ((uint64_t)bogus_cur.token, 0, "no token survives a refusal");
    TEST_ASSERT_EQ((uint64_t)bogus_cur.epoch, 0, "no epoch survives a refusal");
    TEST_ASSERT_EQ((uint64_t)bogus_cur.slot, 0, "no slot survives a refusal");

    quota_ledger_task_release(t);
}

static void test_ob_job_assign_refusal_unwinds_completely(void)
{
    struct task *t = task_current();
    if (!t || !t->quota || t->job)
        return;

    /* Save/restore the gate word like every other gate-touching test here. This
     * one drives quiesce and reopen on the LIVE task through ob_job_assign, and
     * assertions RECORD failure and continue -- so if an unwind ever regressed and
     * left the gate CLOSED, every remaining quota charge in the boot would be
     * refused. The restore is the net for that. */
    int64_t saved_gate = atomic64_read(&t->quota_gate);

    HANDLE h = ob_job_create(&t->handle_table, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    HANDLE_TABLE_ENTRY *ent = ObpLookupHandle(&t->handle_table, h);
    if (!ent || !ent->object) {
        (void)ObpFreeHandle(&t->handle_table, h);
        return;
    }
    JOB_OBJECT *job = (JOB_OBJECT *)ent->object;
    quota_block_t *jb = (quota_block_t *)job->quota;
    if (!jb) {
        (void)ObpFreeHandle(&t->handle_table, h);
        return;
    }

    quota_obligation_t ob = { 0 };
    if (quota_ledger_charge(t, LEDGER_TEST_TYPE, 45, &ob) != STATUS_SUCCESS) {
        (void)ObpFreeHandle(&t->handle_table, h);
        return;
    }
    uint64_t proc_before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint64_t job_before  = quota_usage(jb, LEDGER_TEST_TYPE);

    /* Branch 1: a terminated job refuses AFTER the absorb and the migration. */
    job->terminated = 1;
    TEST_ASSERT_EQ((uint64_t)ob_job_assign(job, t),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "assignment to a terminated job is refused");
    TEST_ASSERT_EQ(quota_usage(jb, LEDGER_TEST_TYPE), job_before,
                   "a refused assignment leaves the job billed for nothing");
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_OPEN,
                   "the unwind reopens the charge gate");
    TEST_ASSERT_NULL((void *)t->job, "no membership was published");
    TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), proc_before,
                   "the process block is unchanged by the refused assignment");

    /* Branch 2: the active-process limit, the other post-absorb refusal. */
    job->terminated = 0;
    job->limit_flags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    job->active_process_limit = 0;       /* any join exceeds it */

    TEST_ASSERT_EQ((uint64_t)ob_job_assign(job, t),
                   (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "assignment past the active-process limit is refused");
    TEST_ASSERT_EQ(quota_usage(jb, LEDGER_TEST_TYPE), job_before,
                   "that refusal also leaves the job billed for nothing");
    TEST_ASSERT_EQ((uint64_t)quota_gate_state_of(t), (uint64_t)QUOTA_GATE_OPEN,
                   "and reopens the charge gate");
    TEST_ASSERT_NULL((void *)t->job, "still no membership");

    /* The obligation must no longer name the job, so its return credits only the
     * process and user blocks -- otherwise the job's usage would go negative. */
    (void)quota_ledger_return(&ob);
    TEST_ASSERT_EQ(quota_usage(jb, LEDGER_TEST_TYPE), job_before,
                   "returning the reverted obligation does not touch the job");

    /* DEFENSIVE teardown: assertions continue after failure, so if either refusal
     * regressed into success this task is now a member of a job that is about to
     * be freed. Detach before dropping the handle. */
    if (t->job == job)
        ob_job_detach_task(t);
    quota_ledger_task_release(t);
    (void)ObpFreeHandle(&t->handle_table, h);
    atomic64_set(&t->quota_gate, saved_gate);
}

/* ==========================================================================
 * Deferred completion above PASSIVE_LEVEL (section 14)
 *
 * These raise IRQL with the ordinary KeRaiseIrql/KeLowerIrql pair rather than
 * calling any boot or interrupt machinery: the deferral branches on
 * KeGetCurrentIrql alone, so a software raise reproduces the ISR case exactly
 * and needs none of the live infrastructure the test policy forbids.
 * ========================================================================== */

/* A return taken above PASSIVE_LEVEL must not credit anything in place: it hands
 * the completion to the drain and says so, and the charge stays charged until the
 * drain runs. That "stays charged" half is the point -- it is what proves the
 * block teardown was actually postponed rather than merely reported as such. */
static void test_quota_ledger_return_at_raised_irql_defers(void)
{
    quota_ledger_test_hold(1);
    struct task       *t = task_current();
    quota_obligation_t ob = { 0 };
    uint64_t           before, pending_before;
    KIRQL              old;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    before         = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);
    pending_before = quota_ledger_deferrals_pending();

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                         1, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge succeeds");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 1,
                   "the charge landed on the block");

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    int rc = quota_ledger_return(&ob);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)QUOTA_LEDGER_RETURN_DEFERRED,
                   "a raised-IRQL return reports DEFERRED, not credited");
    TEST_ASSERT_NULL((void *)ob.ledger,
                     "the handle is emptied on the deferring path too");
    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before + 1,
                   "the obligation is counted as pending");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 1,
                   "the charge is STILL charged: the credit was postponed, not lost");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 1u,
                   "the drain completes exactly the one deferred obligation");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "the drain credits the charge back in full");
    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before,
                   "nothing is left pending");

    /* Release the task ledger this suite created, the same cleanup the other
     * ledger tests do: the ledger is allocated on a task's first obligation and
     * lives to reap, so leaving it behind would read as a heap leak. Every
     * obligation above is completed by now, so the release drains nothing. */
    quota_ledger_task_release(t);
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* Draining twice must credit once. The drain claims each slot's deferral with a
 * CAS before completing it, and without that claim a second pass would complete
 * the same obligation again and drop its ledger reference a second time. */
static void test_quota_ledger_deferred_drain_credits_once(void)
{
    quota_ledger_test_hold(1);
    struct task       *t = task_current();
    quota_obligation_t ob = { 0 };
    uint64_t           before;
    KIRQL              old;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    before = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                         1, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge succeeds");

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    (void)quota_ledger_return(&ob);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 1u,
                   "the first drain completes the obligation");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 0u,
                   "a second drain finds nothing to complete");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "usage returned to baseline exactly once, not below it");

    /* Release the task ledger this suite created, the same cleanup the other
     * ledger tests do: the ledger is allocated on a task's first obligation and
     * lives to reap, so leaving it behind would read as a heap leak. Every
     * obligation above is completed by now, so the release drains nothing. */
    quota_ledger_task_release(t);
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* A doubled return above PASSIVE_LEVEL must hand the obligation over ONCE, and
 * the duplicate must NOT be completed inline -- doing the block teardown at
 * raised IRQL on a charge already promised to the drain is exactly the work this
 * path exists to remove from an interrupt.
 *
 * The duplicate takes its OWN ledger reference before being used. A bare struct
 * copy would be two handles over one reference, which is the lifetime error the
 * obligation contract in quota_ledger.h describes, not a case the return owes
 * any guarantee about. */
static void test_quota_ledger_double_return_at_raised_irql(void)
{
    quota_ledger_test_hold(1);
    struct task       *t = task_current();
    quota_obligation_t ob = { 0 }, dup;
    uint64_t           before, pending_before, forced_before;
    KIRQL              old;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    before         = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);
    pending_before = quota_ledger_deferrals_pending();
    forced_before  = quota_ledger_deferrals_forced();

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                         1, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge succeeds");
    dup = ob;
    quota_ledger_ref(dup.ledger);   /* the duplicate owns its own reference */

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    int first  = quota_ledger_return(&ob);
    int second = quota_ledger_return(&dup);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)first, (uint64_t)QUOTA_LEDGER_RETURN_DEFERRED,
                   "the first raised-IRQL return defers");
    TEST_ASSERT_EQ((uint64_t)second, (uint64_t)QUOTA_LEDGER_RETURN_NONE,
                   "the duplicate coalesces into the pending deferral");
    TEST_ASSERT_NULL((void *)dup.ledger, "the duplicate handle is emptied");
    TEST_ASSERT_EQ(quota_ledger_deferrals_forced(), forced_before,
                   "and is NOT completed inline at raised IRQL");
    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before + 1,
                   "the obligation is pending exactly once, not twice");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 1,
                   "and is still charged until the drain runs");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 1u,
                   "the drain completes it once");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "the charge is credited exactly once across both returns");

    quota_ledger_task_release(t);
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* A deferral must survive the task release that follows it. The caller's handle
 * is already empty, so the pending slot is the only holder -- and the release
 * classifies an obligation as an orphan only when nothing else holds the ledger.
 * A regression that reclaimed the pending obligation would show up here as a
 * leak count, and one that dropped its reference early as a double credit. */
static void test_quota_ledger_deferred_survives_task_release(void)
{
    quota_ledger_test_hold(1);
    struct task       *t = task_current();
    quota_obligation_t ob = { 0 };
    uint64_t           before, pending_before, leaks_before;
    KIRQL              old;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    before         = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);
    pending_before = quota_ledger_deferrals_pending();
    leaks_before   = quota_ledger_leak_count();

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                         1, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge succeeds");

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    TEST_ASSERT_EQ((uint64_t)quota_ledger_return(&ob),
                   (uint64_t)QUOTA_LEDGER_RETURN_DEFERRED,
                   "the return defers");
    KeLowerIrql(old);

    /* Release the task's own claim BEFORE draining: the pending slot's
     * transferred reference is now the only one keeping the ledger alive. */
    quota_ledger_task_release(t);

    TEST_ASSERT_EQ(quota_ledger_leak_count(), leaks_before,
                   "a pending deferral is not reclaimed as an orphan");
    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before + 1,
                   "it is still pending after the release");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 1,
                   "and still charged");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 1u,
                   "the drain completes it after the task let go");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "credited in full");
    TEST_ASSERT_EQ(quota_ledger_leak_count(), leaks_before,
                   "and never counted as a leak");
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* Dropping the LAST ledger reference above PASSIVE_LEVEL must defer the
 * destruction, not run an orphan drain plus up to 64 chunk frees inside an
 * interrupt. An implementation that called quota_ledger_destroy directly here
 * would pass every other test in this file. */
static void test_quota_ledger_destroy_at_raised_irql_defers(void)
{
    quota_ledger_test_hold(1);
    struct task       *t = task_current();
    quota_obligation_t ob = { 0 };
    quota_ledger_t    *ledger;
    uint64_t           destroyed_before;
    KIRQL              old;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    destroyed_before = quota_ledger_deferrals_destroyed();

    /* Take a charge purely to materialise the ledger, then hold an explicit
     * reference and give up every other one, so the deref below is the last. */
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                         1, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge succeeds");
    ledger = ob.ledger;
    TEST_ASSERT_NOT_NULL((void *)ledger, "the charge named a ledger");
    quota_ledger_ref(ledger);
    TEST_ASSERT_EQ((uint64_t)quota_ledger_return(&ob),
                   (uint64_t)QUOTA_LEDGER_RETURN_CREDITED,
                   "the obligation is returned at PASSIVE_LEVEL");
    quota_ledger_task_release(t);   /* drops the task's claim; ours remains */

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    quota_ledger_deref(ledger);     /* the LAST reference, at raised IRQL */
    KeLowerIrql(old);

    TEST_ASSERT_EQ(quota_ledger_deferrals_destroyed(), destroyed_before,
                   "nothing was destroyed inside the raised-IRQL window");

    (void)quota_ledger_drain_now();
    TEST_ASSERT_EQ(quota_ledger_deferrals_destroyed(), destroyed_before + 1,
                   "the drain performed exactly one deferred destruction");
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* More pending obligations than one pass's slot budget must still all complete.
 * Without a saved cursor every pass rescans the first QUOTA_LEDGER_DRAIN_SLOTS
 * slots, finds them already done, and requeues forever -- the obligation past
 * the budget is never examined and the worker spins. 513 is the smallest count
 * that crosses the 512-slot bound.
 *
 * SPREAD ACROSS RESOURCE TYPES, because the per-task obligation ceiling caps
 * each type at its own cap and 513 of any single type is refused by
 * policy. The type is incidental to what this test proves -- the drain's cursor
 * across one LEDGER's slots -- so rotating types keeps all 513 obligations in
 * the same ledger, still past the slot budget and still past one chunk, without
 * weakening the assertion. */
static void test_quota_ledger_deferrals_past_the_slot_budget_complete(void)
{
    quota_ledger_test_hold(1);
    enum { N = 513 };
    static const quota_resource_type_t kinds[] = {
        QUOTA_RES_ALPC_MESSAGE, QUOTA_RES_TIMER, QUOTA_RES_SECTION,
        QUOTA_RES_MAPPED_VIEW, QUOTA_RES_CRASH_BUFFER,
    };
    enum { KINDS = (int)(sizeof(kinds) / sizeof(kinds[0])) };
    struct task       *t = task_current();
    static quota_obligation_t obs[N];   /* static: too large for a kernel stack */
    uint64_t           before[KINDS];
    uint64_t           pending_before;
    KIRQL              old;
    uint32_t           i, charged = 0;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    for (i = 0; i < (uint32_t)KINDS; i++)
        before[i] = quota_usage(t->quota, kinds[i]);
    pending_before = quota_ledger_deferrals_pending();

    for (i = 0; i < (uint32_t)N; i++) {
        obs[i] = (quota_obligation_t){ 0 };
        if (quota_ledger_charge_current(kinds[i % (uint32_t)KINDS], 1,
                                        &obs[i]) != STATUS_SUCCESS)
            break;
        charged++;
    }
    TEST_ASSERT_EQ((uint64_t)charged, (uint64_t)N,
                   "all 513 obligations were charged (the ledger grows past one chunk)");

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    for (i = 0; i < charged; i++)
        (void)quota_ledger_return(&obs[i]);
    KeLowerIrql(old);

    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before + charged,
                   "every one of them is pending");

    /* Bounded number of passes, so a live-lock fails the test instead of
     * hanging the suite. */
    uint32_t completed = 0, rounds = 0;
    while (completed < charged && rounds++ < 8u)
        completed += quota_ledger_drain_now();

    TEST_ASSERT_EQ((uint64_t)completed, (uint64_t)charged,
                   "every deferral past the slot budget is completed, not rescanned forever");
    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before,
                   "nothing left pending");
    for (i = 0; i < (uint32_t)KINDS; i++)
        TEST_ASSERT_EQ(quota_usage(t->quota, kinds[i]), before[i],
                       "and every charge is credited back");

    quota_ledger_task_release(t);
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* quota_ledger_charge_current allocates, so it refuses a raised-IRQL caller with
 * a status rather than entering the allocation path. */
static void test_quota_ledger_charge_current_refuses_raised_irql(void)
{
    quota_ledger_test_hold(1);
    quota_obligation_t ob = { 0 };
    NTSTATUS           st;
    KIRQL              old;

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    st = quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE, 1, &ob);
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_UNSUCCESSFUL,
                   "a raised-IRQL ledger charge is refused, not attempted");
    TEST_ASSERT_NULL((void *)ob.ledger,
                     "the refused charge leaves an empty obligation");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_return(&ob),
                   (uint64_t)QUOTA_LEDGER_RETURN_NONE,
                   "returning the empty obligation owes nothing");
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* quota_ledger_charge_current must answer a ZERO charge the way
 * quota_charge_current does -- success, owing nothing -- and must do it WITHOUT
 * touching ledger storage. Answering it inside quota_ledger_charge instead would
 * acquire a ledger and claim a slot first, so under allocation pressure or at the
 * slot ceiling a zero-cost operation would be refused for exhaustion. */
static void test_quota_ledger_charge_current_zero_owes_nothing(void)
{
    struct task       *t = task_current();
    quota_obligation_t ob = { 0 };
    uint64_t           before;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }

    before = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);

    /* Start from a task with NO ledger, so "no ledger was allocated" is an
     * observable fact rather than an inference. Without this the assertions
     * below all hold even for an implementation that acquires a ledger, claims a
     * slot and releases it again -- which is exactly the behaviour the zero
     * short-circuit exists to avoid. */
    quota_ledger_task_release(t);
    TEST_ASSERT_NULL((void *)t->quota_ledger,
                     "the task starts this check with no ledger");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                         0, &ob),
                   (uint64_t)STATUS_SUCCESS, "a zero charge succeeds");
    TEST_ASSERT_NULL((void *)t->quota_ledger,
                     "and allocated NO ledger: the shortcut ran before the storage");
    TEST_ASSERT_NULL((void *)ob.ledger, "so it produced no obligation");
    TEST_ASSERT_EQ(ob.token, 0ULL, "the empty obligation carries no token");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "and moves no usage");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_return(&ob),
                   (uint64_t)QUOTA_LEDGER_RETURN_NONE,
                   "returning it owes nothing");

    /* An out-of-range type is refused for EITHER amount -- including zero, so an
     * implementation that short-circuited the zero amount ahead of the type
     * check would be caught here rather than passing. */
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(
                       (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT, 1, &ob),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "an out-of-range resource type is refused");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(
                       (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT, 0, &ob),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "and is refused at amount zero too, before the shortcut");
    TEST_ASSERT_NULL((void *)ob.ledger, "and leaves the obligation empty");
}

/* The drain is only legal at PASSIVE_LEVEL, and says so by doing nothing rather
 * than by running the very work the deferral exists to keep out of an interrupt. */
static void test_quota_ledger_drain_now_refuses_raised_irql(void)
{
    quota_ledger_test_hold(1);
    struct task       *t = task_current();
    quota_obligation_t ob = { 0 };
    uint64_t           before;
    KIRQL              old;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    before = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);
    TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                         1, &ob),
                   (uint64_t)STATUS_SUCCESS, "ledger charge succeeds");

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    (void)quota_ledger_return(&ob);
    uint32_t drained_high = quota_ledger_drain_now();
    KeLowerIrql(old);

    TEST_ASSERT_EQ((uint64_t)drained_high, 0u,
                   "the drain does nothing above PASSIVE_LEVEL");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 1,
                   "so the charge is still outstanding");

    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 1u,
                   "and completes once the caller is back at PASSIVE_LEVEL");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "credited in full");

    /* Release the task ledger this suite created, the same cleanup the other
     * ledger tests do: the ledger is allocated on a task's first obligation and
     * lives to reap, so leaving it behind would read as a heap leak. Every
     * obligation above is completed by now, so the release drains nothing. */
    quota_ledger_task_release(t);
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* Several deferrals on the same ledger complete in ONE drain pass, and the
 * pending count tracks them exactly. */
static void test_quota_ledger_deferred_batch_completes(void)
{
    quota_ledger_test_hold(1);
    struct task       *t = task_current();
    quota_obligation_t obs[4];
    uint64_t           before, pending_before;
    KIRQL              old;
    uint32_t           i;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        quota_ledger_test_hold(0);
        return;
    }

    before         = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);
    pending_before = quota_ledger_deferrals_pending();

    for (i = 0; i < 4; i++) {
        obs[i] = (quota_obligation_t){ 0 };
        TEST_ASSERT_EQ((uint64_t)quota_ledger_charge_current(QUOTA_RES_ALPC_MESSAGE,
                                                             1, &obs[i]),
                       (uint64_t)STATUS_SUCCESS, "batch ledger charge succeeds");
    }
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 4,
                   "all four charges landed");

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    for (i = 0; i < 4; i++)
        (void)quota_ledger_return(&obs[i]);
    KeLowerIrql(old);

    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before + 4,
                   "all four are pending, so the ledger was queued once, not four times");
    TEST_ASSERT_EQ((uint64_t)quota_ledger_drain_now(), 4u,
                   "one pass completes the whole batch");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "every charge is credited back");
    TEST_ASSERT_EQ(quota_ledger_deferrals_pending(), pending_before,
                   "nothing left pending");

    /* Release the task ledger this suite created, the same cleanup the other
     * ledger tests do: the ledger is allocated on a task's first obligation and
     * lives to reap, so leaving it behind would read as a heap leak. Every
     * obligation above is completed by now, so the release drains nothing. */
    quota_ledger_task_release(t);
    /* Flush anything the drain deferred to a LATER callback -- a destroy that
     * did not fit the callback budget is requeued rather than run, and while the
     * hold is set no worker will come back for it. Draining to quiescence here
     * is what keeps that correct behaviour from reading as a heap leak. */
    for (uint32_t flush = 0; flush < 4u; flush++)
        (void)quota_ledger_drain_now();
    quota_ledger_test_hold(0);
}

/* --- Charge-path cost reduction ----------------------------------------- */

/* ob_job_collect_accounting's LIVE-MEMBER walk, which nothing else covers: the
 * existing accounting test passes out-of-range pids, so every member resolves to
 * NULL and the loop body never runs.
 *
 * What is asserted is the membership-interval contract -- a member's usage is
 * counted exactly once while it belongs to the job, and a task that has left
 * contributes nothing further -- so this holds regardless of how the collector
 * arranges its locking internally. A REAL Ob-allocated job is required because
 * ob_job_assign takes an Ob reference, which writes an OBJECT_HEADER immediately
 * before the body; a stack JOB_OBJECT would corrupt adjacent stack memory. */
static void test_quota_job_collect_member_accounting(void)
{
    struct task *t = task_current();
    if (!t || !t->quota || t->job) {
        TEST_SKIP("needs a jobless live task with a quota block");
        return;
    }

    int64_t saved_gate = atomic64_read(&t->quota_gate);

    HANDLE h = ob_job_create(&t->handle_table, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        TEST_SKIP("could not allocate a real job object");
        return;
    }
    HANDLE_TABLE_ENTRY *ent = ObpLookupHandle(&t->handle_table, h);
    if (!ent || !ent->object) {
        (void)ObpFreeHandle(&t->handle_table, h);
        TEST_SKIP("job handle did not resolve to a body");
        return;
    }
    JOB_OBJECT *job = (JOB_OBJECT *)ent->object;

    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    IO_COUNTERS                            io;

    /* No members yet: the walk runs zero times and reports only the persistent
     * departed-member totals. */
    ob_job_collect_accounting(job, &acct, &io);
    TEST_ASSERT_EQ((uint64_t)acct.ActiveProcesses, 0,
                   "a job with no members reports no active processes");

    if (ob_job_assign(job, t) != STATUS_SUCCESS) {
        atomic64_set(&t->quota_gate, saved_gate);
        (void)ObpFreeHandle(&t->handle_table, h);
        TEST_SKIP("could not assign the live task as a job member");
        return;
    }

    /* The live member is now reachable from member_pids[] AND satisfies
     * t->job == job, so the collector must count it. */
    ob_job_collect_accounting(job, &acct, &io);
    TEST_ASSERT_EQ((uint64_t)acct.ActiveProcesses, 1,
                   "the assigned task is counted as an active member");
    TEST_ASSERT_EQ((uint64_t)acct.TotalProcesses, 1,
                   "the assigned task is counted as ever-associated");

    /* Burn a little accountable I/O so the member has a nonzero delta to
     * contribute, then confirm the delta is reported rather than dropped by the
     * relaxed window. task_acct_note_control_io is a pure relaxed-atomic
     * counter bump (task.c), safe to call from a test. */
    task_acct_note_control_io(t, 4096);
    ob_job_collect_accounting(job, &acct, &io);
    TEST_ASSERT(io.OtherTransferCount >= 4096,
                "a live member's post-join I/O reaches the job aggregate "
                "while the membership stands");
    uint64_t joined_other = io.OtherTransferCount;

    /* Detach, which folds the member's delta into job->acc_* and clears
     * t->job. The next collection must count that usage exactly ONCE: from the
     * persistent totals now, never again from the live walk (t->job == job is
     * false, so the predicate excludes it). */
    ob_job_detach_task(t);
    ob_job_collect_accounting(job, &acct, &io);
    TEST_ASSERT_EQ((uint64_t)acct.ActiveProcesses, 0,
                   "a detached task is no longer an active member");
    /* Bounded rather than exact: this task's control-I/O counter is shared with
     * any real device activity the boot happens to attribute to it between the
     * two collections, so exact equality would be a flake. The band still
     * catches both regressions -- losing the fold drops BELOW the floor, and
     * counting the member twice adds its whole delta again, landing at or above
     * the ceiling. */
    TEST_ASSERT(io.OtherTransferCount >= joined_other,
                "a detached member's usage survives in the persistent totals "
                "rather than being lost with the membership");
    TEST_ASSERT(io.OtherTransferCount < joined_other + 4096,
                "and is folded in exactly once, not counted again by the "
                "membership predicate after the detach");

    /* POST-MEMBERSHIP ACTIVITY MUST NOT REACH THIS JOB: the detach has already
     * removed the pid, so the collector no longer resolves this task at all and
     * its later activity belongs to nobody's job. */
    uint64_t after_detach_base = io.OtherTransferCount;
    task_acct_note_control_io(t, 65536);
    ob_job_collect_accounting(job, &acct, &io);
    TEST_ASSERT(io.OtherTransferCount < after_detach_base + 65536,
                "activity accrued AFTER the membership ended is not billed to "
                "the job the task used to belong to");

    atomic64_set(&t->quota_gate, saved_gate);
    (void)ObpFreeHandle(&t->handle_table, h);
}

/* quota_charge_adjust used to run one writer-instrumentation RMW per block lock
 * INSIDE its IRQ-off window. It now marks itself in-flight once before masking
 * and settles the epoch and section counts after unmasking. The contract is that
 * the RECORDED TOTALS did not change -- only where the RMWs happen -- so that is
 * exactly what is asserted: the epoch advances by the block count, the lock
 * section budget is met per block, and the in-flight count is balanced. */
static void test_quota_adjust_writer_totals_unchanged(void)
{
    struct task *t = task_current();
    if (!t || !t->quota)
        return;

    quota_charge_receipt_t r     = { 0 };
    uint64_t               token = 0;
    if (quota_charge_current(LEDGER_TEST_TYPE, 8, &r, &token) != STATUS_SUCCESS) {
        TEST_SKIP("could not place a chain charge to adjust");
        return;
    }
    if (r.count == 0 || token == 0) {
        quota_return_chain(&r, token);
        TEST_SKIP("chain charge produced no returnable obligation");
        return;
    }
    uint32_t n = r.count;

    TEST_ASSERT_EQ((uint64_t)quota_test_writers_active(), 0,
                   "no writer is in flight before the adjust");

    uint64_t epoch_before = quota_test_mutation_epoch();
    quota_test_lock_count_begin();
    NTSTATUS st = quota_charge_adjust(&r, token, 9);
    uint64_t sections = quota_test_lock_count_end();
    uint64_t epoch_after = quota_test_mutation_epoch();

    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS,
                   "the adjust itself still succeeds");
    TEST_ASSERT_EQ(epoch_after - epoch_before, (uint64_t)n,
                   "the mutation epoch still advances once per block, settled "
                   "after the mask restore instead of inside the window");
    TEST_ASSERT_EQ(sections,
                   (uint64_t)n * (uint64_t)QUOTA_BUDGET_ADJUST_LOCKS_PER_BLOCK,
                   "the adjust still records its budgeted lock sections");
    TEST_ASSERT_EQ((uint64_t)quota_test_writers_active(), 0,
                   "the in-flight marker taken before the mask is released "
                   "after it, leaving the count balanced");

    quota_return_chain(&r, token);
}

/* QUOTA_CHAIN_MAX was narrowed from 8 to 4, which is the compaction that shrank
 * every embedded receipt. The risk of narrowing is that a chain a limit would
 * have admitted now gets REFUSED for want of a slot, so this asserts the
 * deepest chain any live path builds (process + user + job) still charges and
 * returns exactly, and that it fits with a slot to spare. A chain deeper than
 * the ceiling cannot be constructed through any public path today -- nested jobs
 * do not exist -- so the refuse-rather-than-truncate branch stays covered by the
 * ceiling check in quota_owner.c rather than by a fabricated receipt. */
static void test_quota_chain_fits_compacted_receipt(void)
{
    struct task *t = task_current();
    if (!t || !t->quota || t->job) {
        TEST_SKIP("needs a jobless live task with a quota block");
        return;
    }

    int64_t saved_gate = atomic64_read(&t->quota_gate);

    HANDLE h = ob_job_create(&t->handle_table, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        TEST_SKIP("could not allocate a real job object");
        return;
    }
    HANDLE_TABLE_ENTRY *ent = ObpLookupHandle(&t->handle_table, h);
    if (!ent || !ent->object) {
        (void)ObpFreeHandle(&t->handle_table, h);
        TEST_SKIP("job handle did not resolve to a body");
        return;
    }
    JOB_OBJECT *job = (JOB_OBJECT *)ent->object;
    quota_block_t *jb = (quota_block_t *)job->quota;
    if (!jb || ob_job_assign(job, t) != STATUS_SUCCESS) {
        atomic64_set(&t->quota_gate, saved_gate);
        (void)ObpFreeHandle(&t->handle_table, h);
        TEST_SKIP("could not make the live task a job member");
        return;
    }

    uint64_t proc_before = quota_usage(t->quota, LEDGER_TEST_TYPE);
    uint64_t job_before  = quota_usage(jb, LEDGER_TEST_TYPE);

    quota_charge_receipt_t r     = { 0 };
    uint64_t               token = 0;
    NTSTATUS st = quota_charge_current(LEDGER_TEST_TYPE, 7, &r, &token);
    TEST_ASSERT_EQ((uint64_t)st, (uint64_t)STATUS_SUCCESS,
                   "the deepest reachable chain is still admitted after the "
                   "receipt was narrowed to QUOTA_CHAIN_MAX slots");
    if (st == STATUS_SUCCESS) {
        /* Asserted against the REACHABLE depth (process + user + one job = 3),
         * not against the ceiling: a relative bound would silently disagree with
         * the floor assert in quota.h, which admits a ceiling of 3. */
        TEST_ASSERT(r.count >= 2 && r.count <= 3,
                    "a job member's chain names process, user and job blocks, "
                    "which is the deepest chain any path builds today");
        TEST_ASSERT(r.count <= QUOTA_CHAIN_MAX,
                    "the reachable chain fits the compacted receipt");
        TEST_ASSERT_EQ(quota_usage(jb, LEDGER_TEST_TYPE), job_before + 7,
                       "the job block in the chain was charged");
        quota_return_chain(&r, token);
        TEST_ASSERT_EQ(quota_usage(jb, LEDGER_TEST_TYPE), job_before,
                       "the whole chain returns exactly through the compacted "
                       "receipt, stranding nothing in the job block");
        TEST_ASSERT_EQ(quota_usage(t->quota, LEDGER_TEST_TYPE), proc_before,
                       "and nothing is stranded in the process block");
    }

    ob_job_detach_task(t);
    atomic64_set(&t->quota_gate, saved_gate);
    (void)ObpFreeHandle(&t->handle_table, h);
}

void test_register_quota_ledger(void)
{
    test_suite_register_cat("Quota: job accounting counts a live member for its "
                            "membership interval only",
                            test_quota_job_collect_member_accounting, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: an adjust records the same writer totals "
                            "with no RMW inside its IRQ-off window",
                            test_quota_adjust_writer_totals_unchanged, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: the deepest reachable chain fits the "
                            "compacted receipt and returns exactly",
                            test_quota_chain_fits_compacted_receipt, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a raised-IRQL return defers instead of crediting",
                            test_quota_ledger_return_at_raised_irql_defers, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a deferred obligation is credited exactly once",
                            test_quota_ledger_deferred_drain_credits_once, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a doubled raised-IRQL return defers once",
                            test_quota_ledger_double_return_at_raised_irql, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a deferral survives the task release",
                            test_quota_ledger_deferred_survives_task_release, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a last-reference drop at raised IRQL defers the destroy",
                            test_quota_ledger_destroy_at_raised_irql_defers, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: deferrals past the slot budget still complete",
                            test_quota_ledger_deferrals_past_the_slot_budget_complete, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a raised-IRQL charge is refused, not attempted",
                            test_quota_ledger_charge_current_refuses_raised_irql, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a zero charge owes nothing and needs no ledger",
                            test_quota_ledger_charge_current_zero_owes_nothing, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: the drain refuses to run above PASSIVE_LEVEL",
                            test_quota_ledger_drain_now_refuses_raised_irql, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota ledger: a batch of deferrals completes in one pass",
                            test_quota_ledger_deferred_batch_completes, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: gate entry and exit balance the in-flight count",
                            test_quota_gate_enter_exit_balances, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a CLOSED gate refuses a charge with STATUS_RETRY",
                            test_quota_gate_closed_refuses_charge, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a seal outranks a close and cannot be reopened",
                            test_quota_gate_seal_outranks_closed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a drain timeout restores OPEN rather than stranding CLOSED",
                            test_quota_gate_drain_timeout_restores_open, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: ledger charge and return round-trip exactly",
                            test_quota_ledger_charge_return_roundtrip, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a doubled ledger return credits once",
                            test_quota_ledger_double_return_is_noop, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a stale handle cannot return a recycled slot's charge",
                            test_quota_ledger_stale_handle_cannot_return_later_charge,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a zero-amount ledger charge holds nothing",
                            test_quota_ledger_zero_charge_holds_nothing, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: ledger storage grows then reuses freed slots",
                            test_quota_ledger_capacity_grows_then_reuses, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a slot claim resumes at its hint and wraps for a hole",
                            test_quota_ledger_claim_resumes_at_its_hint, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: an obligation outlives the task's own claim",
                            test_quota_ledger_obligation_outlives_task_claim, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: an orphaned obligation is reclaimed and counted",
                            test_quota_ledger_orphan_reclaimed_and_counted, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a charge adjust moves usage in both directions",
                            test_quota_charge_adjust_both_directions, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a refused adjust changes no usage and no peak",
                            test_quota_charge_adjust_refused_changes_nothing, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a charge adjust requires the exact token",
                            test_quota_charge_adjust_requires_the_token, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: migration adopts the absorb record, never recharges",
                            test_quota_ledger_migrate_adopts_absorb_record, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: unmigrate restores the absorb record for a refused join",
                            test_quota_ledger_unmigrate_restores_absorb_record, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a refused adjust changes NO participating block",
                            test_quota_charge_adjust_refusal_spans_every_block, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: an adjust during a quiesce is refused",
                            test_quota_ledger_adjust_refused_during_quiesce, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: gate entry at the in-flight ceiling is refused",
                            test_quota_gate_inflight_ceiling_refuses, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: migration skips an uncovered obligation and is idempotent",
                            test_quota_ledger_migrate_skips_and_is_idempotent, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a decrease past what a block holds fails closed",
                            test_quota_charge_adjust_decrease_underflow_fails_closed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a ledger allocation failure is reported and strands nothing",
                            test_quota_ledger_charge_handles_allocation_failure, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: the obligation ceiling refuses by policy and reserves per type",
                            test_quota_ledger_ceiling_refuses_and_reserves_per_type, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a reserved obligation budget is given back on every reachable failure",
                            test_quota_ledger_ceiling_restored_on_failure_paths, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a duplicate return cannot give the same budget back twice",
                            test_quota_ledger_ceiling_survives_duplicate_return, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a deferred return releases its budget at drain, not at handoff",
                            test_quota_ledger_ceiling_released_by_deferred_drain, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a negative obligation count fails closed as an integrity failure",
                            test_quota_ledger_negative_count_fails_closed, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: the charge gate outranks the obligation ceiling",
                            test_quota_ledger_ceiling_yields_to_the_gate, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a ledger charge carries its named source into the attribution table",
                            test_quota_ledger_charge_from_attributes_the_source, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: a refused ob_job_assign unwinds absorb, migration, and gate",
                            test_ob_job_assign_refusal_unwinds_completely, TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
