/* ============================================================================
 * test_quota_config.c -- Configurable per-user quota defaults + the shared
 * subsystem charge entry point (kernel resource accounting: registry, ALPC,
 * and notification quotas).
 *
 * Two surfaces, both introduced by section 6:
 *
 *   1. The kernel-config override layer that turns quota.h's documented
 *      precedence (per-block explicit > kernel-config > taxonomy default) from
 *      "intended" into enforced: the per-type tunable name table, the effective
 *      default lookup, and the live-USER-block re-limit walk.
 *   2. quota_charge_current, the ONE entry point every charging subsystem uses
 *      so the boot exemption lives in one place instead of in each consumer.
 *
 * The re-limit tests use quota_user_block_acquire with synthetic S-1-5-<rid>
 * SIDs so they exercise the real published registry (the walk only ever sees
 * registered USER blocks) without disturbing the live SYSTEM block that every
 * process shares. Every acquired block is dereferenced, which unlinks it.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/tunables.h"
#include "kernel/security/sid.h"
#include "kernel/boot_init.h"   /* kernel_subsystem_ready for the exemption test */
#include "kernel/sched/task.h"  /* task_current for the per-task usage deltas   */
#include "kernel/knf/knf.h"     /* KNF charge-symmetry coverage                 */
#include "kernel/ob/ob.h"       /* ObDereferenceObject                          */

/* RID base for this file's synthetic SIDs. Chosen well clear of the well-known
 * RIDs (18 = LOCAL SYSTEM) so an acquired block can never alias a live one. */
#define TQC_RID_BASE  0x25060000u

static SID *tqc_make_sid(uint8_t *buf, uint32_t rid)
{
    SID *sid = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    RtlInitializeSid(sid, nt_authority, 1);
    uint32_t *sub = RtlSubAuthoritySid(sid, 0);
    if (sub)
        *sub = rid;
    return sid;
}

/* --- The tunable name table --------------------------------------------- */

/* Every type must have a name and it must be unique, for the same reason the
 * taxonomy asserts unique type names: two types sharing a tunable would let a
 * change to one silently re-limit the other. */
static void test_quota_config_names_complete_and_unique(void)
{
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const char *ni = quota_config_tunable_name((quota_resource_type_t)i);
        TEST_ASSERT_NOT_NULL((void *)ni, "every resource type has a tunable name");
        if (!ni)
            continue;
        TEST_ASSERT(ni[0] != '\0', "tunable name is not the empty string");

        for (uint32_t j = 0; j < i; j++) {
            const char *nj = quota_config_tunable_name((quota_resource_type_t)j);
            int same = 1;
            if (!nj)
                continue;
            for (uint32_t k = 0;; k++) {
                if (ni[k] != nj[k]) { same = 0; break; }
                if (ni[k] == '\0') break;
            }
            TEST_ASSERT(same == 0, "tunable names are unique across types");
        }
    }
}

/* Out-of-range is NULL rather than a sentinel string: a caller that indexed
 * past the table must not get a name it can then register or set. */
static void test_quota_config_name_out_of_range(void)
{
    TEST_ASSERT_NULL((void *)quota_config_tunable_name(
                         (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT),
                     "one past the last type has no tunable name");
    TEST_ASSERT_NULL((void *)quota_config_tunable_name(
                         (quota_resource_type_t)(QUOTA_RESOURCE_TYPE_COUNT + 7)),
                     "far out-of-range type has no tunable name");
}

/* Every tunable must actually EXIST. Comparing quota_config_user_default with
 * kernel_tunable_get_u64 alone would be a tautology: both fall back to the
 * taxonomy default, so the comparison stays green even if registration failed
 * outright and the administrative surface does not exist. kernel_tunable_get
 * distinguishes the two -- it returns STATUS_NOT_FOUND for an absent name. */
static void test_quota_config_tunables_are_registered(void)
{
    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
        const char *name = quota_config_tunable_name((quota_resource_type_t)i);
        int64_t value = -1;
        if (!name)
            continue;
        TEST_ASSERT_EQ((uint64_t)kernel_tunable_get(name, &value),
                       (uint64_t)STATUS_SUCCESS,
                       "every per-type quota tunable is registered");
        TEST_ASSERT(value >= 0, "a registered quota tunable holds a sane value");
    }

    TEST_ASSERT_EQ(quota_config_user_default(
                       (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT),
                   (uint64_t)QUOTA_LIMIT_UNLIMITED,
                   "out-of-range type reports unlimited, not garbage");
}

/* End-to-end through the REAL administrative path: a kernel_tunable_set must
 * drive the change callback, which re-limits live USER blocks and re-publishes
 * the effective default for blocks created afterwards. Without this the wiring
 * between the tunable registry and the quota walk is never exercised -- every
 * other test calls quota_user_default_relimit directly. */
static void test_quota_config_tunable_set_drives_relimit(void)
{
    const quota_resource_type_t T = QUOTA_RES_NOTIFICATION_SUB;
    const char *name = quota_config_tunable_name(T);
    uint8_t buf_live[SID_MAX_SIZE] = { 0 };
    uint8_t buf_new[SID_MAX_SIZE] = { 0 };
    SID *s_live = tqc_make_sid(buf_live, TQC_RID_BASE + 7);
    quota_block_t *live = quota_user_block_acquire(s_live, RtlLengthSid(s_live));
    int64_t restore = 0;

    TEST_ASSERT_NOT_NULL((void *)name, "tunable name resolved");
    TEST_ASSERT_NOT_NULL((void *)live, "live user block acquired before the set");
    if (!name || !live)
        return;
    TEST_ASSERT_EQ((uint64_t)kernel_tunable_get(name, &restore),
                   (uint64_t)STATUS_SUCCESS, "original tunable value read");

    /* TUNABLE_SET_PRIVILEGED: this is the kernel applying its own policy, not
     * an unprivileged user-mode request. */
    TEST_ASSERT_EQ((uint64_t)kernel_tunable_set(name, 321,
                                                TUNABLE_SET_PRIVILEGED),
                   (uint64_t)STATUS_SUCCESS, "quota tunable set accepted");

    TEST_ASSERT_EQ(quota_user_default_current(T), 321ULL,
                   "the set re-published the effective default");
    TEST_ASSERT_EQ(quota_limit(live, T), 321ULL,
                   "the set re-limited a block that already existed");

    /* A block acquired AFTER the change must be seeded with the new value --
     * this is the seed/publish ordering the re-limit walk depends on when it
     * skips blocks linked at the head behind it. */
    {
        SID *s_new = tqc_make_sid(buf_new, TQC_RID_BASE + 8);
        quota_block_t *fresh = quota_user_block_acquire(s_new, RtlLengthSid(s_new));
        TEST_ASSERT_NOT_NULL((void *)fresh, "post-change user block acquired");
        if (fresh) {
            TEST_ASSERT_EQ(quota_limit(fresh, T), 321ULL,
                           "a block created after the change carries the new default");
            quota_block_deref(fresh);
        }
    }

    (void)kernel_tunable_set(name, restore, TUNABLE_SET_PRIVILEGED);
    quota_block_deref(live);
}

/* --- Principal scoping --------------------------------------------------- */

/* The override is a PER-USER cap, so it must not seed PROCESS or JOB blocks.
 * Applying it to those would cap a single process at the whole user's budget
 * and then cap its job at it a second time. */
static void test_quota_config_scoped_to_user_principal(void)
{
    const quota_resource_type_t T = QUOTA_RES_NOTIFICATION_SUB;
    const quota_resource_desc_t *d = quota_resource_desc(T);
    quota_block_t *p = quota_block_create(QUOTA_PRINCIPAL_PROCESS, NULL, 0);
    quota_block_t *j = quota_block_create(QUOTA_PRINCIPAL_JOB, NULL, 0);

    TEST_ASSERT_NOT_NULL((void *)p, "process block allocated");
    TEST_ASSERT_NOT_NULL((void *)j, "job block allocated");
    if (!p || !j) {
        if (p) quota_block_deref(p);
        if (j) quota_block_deref(j);
        return;
    }

    TEST_ASSERT_EQ(quota_limit(p, T), d->default_limit,
                   "process block seeds from the taxonomy, not the user config");
    TEST_ASSERT_EQ(quota_limit(j, T), d->default_limit,
                   "job block seeds from the taxonomy, not the user config");

    quota_block_deref(p);
    quota_block_deref(j);
}

/* A freshly acquired USER block carries the configured default. */
static void test_quota_config_seeds_user_block(void)
{
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *sid = tqc_make_sid(buf, TQC_RID_BASE + 1);
    quota_block_t *u = quota_user_block_acquire(sid, RtlLengthSid(sid));

    TEST_ASSERT_NOT_NULL((void *)u, "user block acquired");
    if (!u)
        return;

    for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++)
        TEST_ASSERT_EQ(quota_limit(u, (quota_resource_type_t)i),
                       quota_config_user_default((quota_resource_type_t)i),
                       "user block seeded from the configured default");

    quota_block_deref(u);
}

/* --- The live re-limit walk ---------------------------------------------- */

/* The property that makes the config a real administrative control: changing
 * the default reaches a USER block that ALREADY EXISTS. A create-time-only
 * default would never affect a logged-in user, whose canonical block is
 * created once and lives for the session. */
static void test_quota_config_relimit_reaches_live_blocks(void)
{
    const quota_resource_type_t T = QUOTA_RES_NOTIFICATION_STATE;
    uint8_t buf_a[SID_MAX_SIZE] = { 0 };
    uint8_t buf_b[SID_MAX_SIZE] = { 0 };
    SID *a = tqc_make_sid(buf_a, TQC_RID_BASE + 2);
    SID *b = tqc_make_sid(buf_b, TQC_RID_BASE + 3);
    quota_block_t *ua = quota_user_block_acquire(a, RtlLengthSid(a));
    quota_block_t *ub = quota_user_block_acquire(b, RtlLengthSid(b));
    uint64_t restore;

    TEST_ASSERT_NOT_NULL((void *)ua, "first user block acquired");
    TEST_ASSERT_NOT_NULL((void *)ub, "second user block acquired");
    if (!ua || !ub) {
        if (ua) quota_block_deref(ua);
        if (ub) quota_block_deref(ub);
        return;
    }
    restore = quota_limit(ua, T);

    quota_user_default_relimit(T, 4242);
    TEST_ASSERT_EQ(quota_limit(ua, T), 4242ULL,
                   "re-limit reached the first live user block");
    TEST_ASSERT_EQ(quota_limit(ub, T), 4242ULL,
                   "re-limit reached the second live user block (walk continues)");

    /* Lowering below current usage is allowed and must NOT rewrite usage --
     * the contract is that later charges are refused, not that accounting is
     * retroactively falsified. */
    TEST_ASSERT_EQ(quota_charge(ua, T, 100), (uint64_t)STATUS_SUCCESS,
                   "charge admitted under the raised limit");
    quota_user_default_relimit(T, 10);
    TEST_ASSERT_EQ(quota_limit(ua, T), 10ULL, "limit lowered below usage");
    TEST_ASSERT_EQ(quota_usage(ua, T), 100ULL,
                   "usage left intact when the limit drops below it");
    TEST_ASSERT_EQ(quota_charge(ua, T, 1), (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "further charges refused against the lowered limit");

    quota_return(ua, T, 100);
    quota_user_default_relimit(T, restore);
    quota_block_deref(ua);
    quota_block_deref(ub);
}

/* An explicitly set limit is the top of the precedence order: a later change
 * to the config default must not raise or lower it back. Without this the
 * config layer would trample every deliberate per-user cap. */
static void test_quota_config_relimit_respects_explicit(void)
{
    const quota_resource_type_t T = QUOTA_RES_NOTIFICATION_BYTES;
    uint8_t buf_x[SID_MAX_SIZE] = { 0 };
    uint8_t buf_y[SID_MAX_SIZE] = { 0 };
    SID *x = tqc_make_sid(buf_x, TQC_RID_BASE + 4);
    SID *y = tqc_make_sid(buf_y, TQC_RID_BASE + 5);
    quota_block_t *ux = quota_user_block_acquire(x, RtlLengthSid(x));
    quota_block_t *uy = quota_user_block_acquire(y, RtlLengthSid(y));
    uint64_t restore;

    TEST_ASSERT_NOT_NULL((void *)ux, "explicit-limit user block acquired");
    TEST_ASSERT_NOT_NULL((void *)uy, "default-limit user block acquired");
    if (!ux || !uy) {
        if (ux) quota_block_deref(ux);
        if (uy) quota_block_deref(uy);
        return;
    }
    restore = quota_limit(uy, T);

    TEST_ASSERT_EQ(quota_set_limit(ux, T, 777), (uint64_t)STATUS_SUCCESS,
                   "explicit limit set on the first block");

    quota_user_default_relimit(T, 5555);
    TEST_ASSERT_EQ(quota_limit(ux, T), 777ULL,
                   "explicit limit survives a config default change");
    TEST_ASSERT_EQ(quota_limit(uy, T), 5555ULL,
                   "non-explicit block follows the config default");

    quota_user_default_relimit(T, restore);
    quota_block_deref(ux);
    quota_block_deref(uy);
}

/* A re-limit walk must reject a bad type rather than index past the counter
 * array, and must reject a limit outside the counter domain rather than store
 * a value a charge could never be judged against. */
static void test_quota_config_relimit_bad_args(void)
{
    const quota_resource_type_t T = QUOTA_RES_NOTIFICATION_SUB;
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    SID *sid = tqc_make_sid(buf, TQC_RID_BASE + 6);
    quota_block_t *u = quota_user_block_acquire(sid, RtlLengthSid(sid));
    uint64_t before;

    TEST_ASSERT_NOT_NULL((void *)u, "user block acquired");
    if (!u)
        return;
    before = quota_limit(u, T);

    /* Neither call may change anything; both are no-ops by contract. */
    quota_user_default_relimit((quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT, 9);
    quota_user_default_relimit(T, (uint64_t)QUOTA_AMOUNT_MAX + 1ULL);

    TEST_ASSERT_EQ(quota_limit(u, T), before,
                   "out-of-range type and over-domain limit are both no-ops");

    quota_block_deref(u);
}

/* The cursor-retention and resume logic only RUNS when a batch fills, so a
 * two-block test always takes the short-batch exit and can never catch an
 * off-by-one, a bad cursor dereference, or a skipped second batch. Acquire
 * more than two full batches, and put explicit limits on the blocks sitting
 * either side of a batch boundary so a resume defect shows up as a block that
 * kept the default when it should have been skipped, or vice versa. */
#define TQC_BATCH_SPAN  40u   /* > 2 * QUOTA_RELIMIT_BATCH (16) */

static void test_quota_config_relimit_crosses_batches(void)
{
    const quota_resource_type_t T = QUOTA_RES_TIMER;
    static quota_block_t *blocks[TQC_BATCH_SPAN];
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    uint32_t n = 0;
    uint64_t restore = quota_user_default_current(T);

    for (uint32_t i = 0; i < TQC_BATCH_SPAN; i++) {
        SID *sid = tqc_make_sid(buf, TQC_RID_BASE + 0x100 + i);
        blocks[n] = quota_user_block_acquire(sid, RtlLengthSid(sid));
        if (blocks[n])
            n++;
    }
    TEST_ASSERT_EQ((uint64_t)n, (uint64_t)TQC_BATCH_SPAN,
                   "acquired more than two full re-limit batches of user blocks");

    /* Explicit limits at indices that straddle both batch boundaries. */
    for (uint32_t i = 0; i < n; i++) {
        if (i == 15 || i == 16 || i == 31 || i == 32)
            TEST_ASSERT_EQ((uint64_t)quota_set_limit(blocks[i], T, 900 + i),
                           (uint64_t)STATUS_SUCCESS,
                           "explicit limit set on a batch-boundary block");
    }

    quota_user_default_relimit(T, 6161);

    for (uint32_t i = 0; i < n; i++) {
        if (i == 15 || i == 16 || i == 31 || i == 32)
            TEST_ASSERT_EQ(quota_limit(blocks[i], T), (uint64_t)(900 + i),
                           "explicit block at a batch boundary was not trampled");
        else
            TEST_ASSERT_EQ(quota_limit(blocks[i], T), 6161ULL,
                           "every non-explicit block across all batches was re-limited");
    }

    quota_user_default_relimit(T, restore);
    for (uint32_t i = 0; i < n; i++)
        quota_block_deref(blocks[i]);
}

/* The walk bounds VISITED nodes per registry-lock acquisition, not just
 * successful pins, so a pass can now end on the visit budget with more list
 * left. TQC_BATCH_SPAN blocks already exceed one visit budget's worth of pin
 * batches; this asserts the every-block outcome still holds when the tunable
 * path (rather than a direct call) drives the same walk, which is the shape a
 * resume-logic off-by-one would break. */
static void test_quota_config_relimit_completes_via_tunable(void)
{
    const quota_resource_type_t T = QUOTA_RES_MAPPED_VIEW;
    const char *name = quota_config_tunable_name(T);
    static quota_block_t *blocks[TQC_BATCH_SPAN];
    uint8_t buf[SID_MAX_SIZE] = { 0 };
    uint32_t n = 0;
    int64_t restore = 0;

    TEST_ASSERT_NOT_NULL((void *)name, "tunable name resolved");
    if (!name || kernel_tunable_get(name, &restore) != STATUS_SUCCESS) {
        TEST_ASSERT(0, "quota tunable readable before the walk test");
        return;
    }

    for (uint32_t i = 0; i < TQC_BATCH_SPAN; i++) {
        SID *sid = tqc_make_sid(buf, TQC_RID_BASE + 0x200 + i);
        blocks[n] = quota_user_block_acquire(sid, RtlLengthSid(sid));
        if (blocks[n])
            n++;
    }
    TEST_ASSERT_EQ((uint64_t)n, (uint64_t)TQC_BATCH_SPAN,
                   "acquired the full span of user blocks");

    TEST_ASSERT_EQ((uint64_t)kernel_tunable_set(name, 7373,
                                                TUNABLE_SET_PRIVILEGED),
                   (uint64_t)STATUS_SUCCESS, "tunable set accepted");

    for (uint32_t i = 0; i < n; i++)
        TEST_ASSERT_EQ(quota_limit(blocks[i], T), 7373ULL,
                       "every block re-limited, including past the visit budget");

    (void)kernel_tunable_set(name, restore, TUNABLE_SET_PRIVILEGED);
    for (uint32_t i = 0; i < n; i++)
        quota_block_deref(blocks[i]);
}

/* The tunables are GLOBAL policy, so an unprivileged setter must be refused --
 * otherwise any future user-mode set path silently rewrites every user's cap. */
static void test_quota_config_tunable_requires_privilege(void)
{
    const quota_resource_type_t T = QUOTA_RES_NOTIFICATION_STATE;
    const char *name = quota_config_tunable_name(T);
    int64_t before = 0, after = 0;

    TEST_ASSERT_NOT_NULL((void *)name, "tunable name resolved");
    if (!name || kernel_tunable_get(name, &before) != STATUS_SUCCESS)
        return;

    TEST_ASSERT_EQ((uint64_t)kernel_tunable_set(name, 4321, 0),
                   (uint64_t)STATUS_ACCESS_DENIED,
                   "unprivileged set of a global quota default is refused");
    (void)kernel_tunable_get(name, &after);
    TEST_ASSERT_EQ((uint64_t)after, (uint64_t)before,
                   "a refused set leaves the configured value unchanged");
}

/* --- The shared charge entry point --------------------------------------- */

/* The exemption is bound to a boot MILESTONE, not to the absence of a block.
 * By the time the test sweep runs the scheduler is ready, so a charge here
 * must take the real path and produce a returnable receipt -- if it silently
 * took the exemption instead, every subsystem charge would be a no-op and no
 * quota would ever be enforced. */
static void test_quota_charge_current_charges_after_boot(void)
{
    quota_charge_receipt_t receipt = { 0 };
    uint64_t token = 0;

    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_SCHED) != 0,
                "scheduler is ready by the time the test sweep runs");
    TEST_ASSERT(quota_registry_ready() != 0, "quota taxonomy is validated");

    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB, 3,
                                                  &receipt, &token),
                   (uint64_t)STATUS_SUCCESS, "charge against the current task admitted");
    TEST_ASSERT(token != 0,
                "a real charge yields a non-zero token, not the no-obligation one");

    quota_return_chain(&receipt, token);

    /* Returning again with the same token is the documented no-op, which is
     * what makes a double cleanup path safe. */
    quota_return_chain(&receipt, token);
}

/* A zero-amount charge is a success that owes nothing: token 0 is the one
 * value quota_return_chain always refuses, so it cannot return a later charge
 * that reuses the same receipt storage. */
static void test_quota_charge_current_zero_amount(void)
{
    quota_charge_receipt_t receipt = { 0 };
    uint64_t token = 0xDEADBEEFULL;

    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB, 0,
                                                  &receipt, &token),
                   (uint64_t)STATUS_SUCCESS, "zero-amount charge succeeds");
    TEST_ASSERT_EQ(token, 0ULL,
                   "zero-amount charge writes the canonical no-obligation token");
}

/* Both out-parameters are mandatory: a charge with nowhere to record its
 * receipt or token would strand usage no caller could ever return. */
static void test_quota_charge_current_bad_args(void)
{
    quota_charge_receipt_t receipt = { 0 };
    uint64_t token = 0;

    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB, 1,
                                                  NULL, &token),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL receipt refused");
    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB, 1,
                                                  &receipt, NULL),
                   (uint64_t)STATUS_INVALID_PARAMETER, "NULL token out refused");
}

/* The wrapper must FORWARD failures, not flatten them: KNF and ALPC both
 * decide what status a caller sees based on what this returns, so a regression
 * that admitted an invalid type or collapsed STATUS_QUOTA_EXCEEDED into a
 * generic error would silently change their contracts. */
static void test_quota_charge_current_forwards_failures(void)
{
    quota_charge_receipt_t receipt = { 0 };
    quota_charge_receipt_t busy = { 0 };
    uint64_t token = 0, busy_token = 0;
    struct task *t = task_current();

    TEST_ASSERT_EQ((uint64_t)quota_charge_current(
                       (quota_resource_type_t)QUOTA_RESOURCE_TYPE_COUNT, 1,
                       &receipt, &token),
                   (uint64_t)STATUS_INVALID_PARAMETER, "invalid type refused");
    TEST_ASSERT_EQ(token, 0ULL, "a refused charge leaves the token untouched");

    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB,
                                                  (uint64_t)QUOTA_AMOUNT_MAX + 1ULL,
                                                  &receipt, &token),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "amount outside the counter domain refused");

    /* Charging into a receipt that already holds a live charge must be refused
     * rather than silently dropping the references the first charge holds. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB, 1,
                                                  &busy, &busy_token),
                   (uint64_t)STATUS_SUCCESS, "first charge into the receipt admitted");
    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB, 1,
                                                  &busy, &token),
                   (uint64_t)STATUS_INVALID_PARAMETER,
                   "charging into a receipt holding a live charge refused");
    quota_return_chain(&busy, busy_token);

    /* A real refusal: cap the current task's PROCESS block, then charge past
     * it. The status must arrive as STATUS_QUOTA_EXCEEDED and usage must be
     * exactly as it was. */
    if (t && t->quota) {
        const quota_resource_type_t T = QUOTA_RES_CRASH_BUFFER;
        uint64_t restore_limit = quota_limit(t->quota, T);
        uint64_t before = quota_usage(t->quota, T);
        quota_charge_receipt_t r2 = { 0 };
        uint64_t tok2 = 0;

        TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota, T, before + 4),
                       (uint64_t)STATUS_SUCCESS, "process block capped for the test");
        TEST_ASSERT_EQ((uint64_t)quota_charge_current(T, 5, &r2, &tok2),
                       (uint64_t)STATUS_QUOTA_EXCEEDED,
                       "over-limit charge surfaces STATUS_QUOTA_EXCEEDED");
        TEST_ASSERT_EQ(quota_usage(t->quota, T), before,
                       "a refused charge leaves usage exactly as it was");
        TEST_ASSERT_EQ(tok2, 0ULL, "a refused charge hands out no token");

        (void)quota_set_limit(t->quota, T, restore_limit);
    }
}

/* --- Charge symmetry across the KNF lifecycle ---------------------------- */

/* Creating a state charges the current task for the state AND its retention
 * budget; deleting it must return BOTH. Without a delta assertion a stranded
 * charge -- the exact failure a two-charge create is prone to -- is invisible.
 * Usage is read from the current task's PROCESS block, which every chain
 * charge bills. */
static void test_quota_knf_state_charge_symmetry(void)
{
    struct task *t = task_current();
    KNF_STATE *st;
    uint64_t states_before, bytes_before;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }
    states_before = quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE);
    bytes_before  = quota_usage(t->quota, QUOTA_RES_NOTIFICATION_BYTES);

    st = knf_create_state("Kernel", "QuotaSymState", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_MACHINE, NULL, KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL((void *)st, "state created for the charge-symmetry test");
    if (!st)
        return;

    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE),
                   states_before + 1,
                   "creating a state charges exactly one notification state");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_BYTES),
                   bytes_before + KNF_MAX_PAYLOAD,
                   "creating a state charges its full retention budget");

    /* Dropping the CREATOR reference is not enough: ObInsertObject gave the
     * category directory its own reference, so the body survives until the
     * namespace entry goes too. Both must go before on_delete runs. */
    ObDereferenceObject(st);
    TEST_ASSERT_EQ(knf_delete_state("Kernel", "QuotaSymState"), 0,
                   "state removed from the namespace");

    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE),
                   states_before, "state teardown returns the state charge");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_BYTES),
                   bytes_before, "state teardown returns the retention charge");
}

/* Subscribe charges one subscription; unsubscribe returns it. A leak here
 * would let a user pin subscription budget forever by churning subscriptions. */
static void test_quota_knf_subscription_charge_symmetry(void)
{
    struct task *t = task_current();
    KNF_STATE *st;
    struct knf_subscriber *sub = NULL;
    uint64_t before;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }

    st = knf_create_state("Kernel", "QuotaSymSub", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_MACHINE, NULL, KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL((void *)st, "state created for the subscription test");
    if (!st)
        return;

    before = quota_usage(t->quota, QUOTA_RES_NOTIFICATION_SUB);

    TEST_ASSERT_EQ((uint64_t)knf_subscribe(st, &sub), (uint64_t)STATUS_SUCCESS,
                   "subscribe succeeds");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_SUB), before + 1,
                   "subscribe charges exactly one subscription");

    TEST_ASSERT_EQ((uint64_t)knf_unsubscribe(&sub), (uint64_t)STATUS_SUCCESS,
                   "unsubscribe succeeds");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_SUB), before,
                   "unsubscribe returns the subscription charge");
    TEST_ASSERT_NULL((void *)sub, "unsubscribe consumed the handle");

    ObDereferenceObject(st);
    (void)knf_delete_state("Kernel", "QuotaSymSub");
}

/* A refused subscription charge must leave NOTHING behind: no node, no charge,
 * and a status that says quota rather than memory. */
static void test_quota_knf_subscription_refused_at_cap(void)
{
    struct task *t = task_current();
    KNF_STATE *st;
    struct knf_subscriber *sub = NULL;
    quota_charge_receipt_t hold = { 0 };
    uint64_t hold_token = 0;
    uint64_t before, restore;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }

    st = knf_create_state("Kernel", "QuotaSubCap", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_MACHINE, NULL, KNF_KERNEL_MODE);
    TEST_ASSERT_NOT_NULL((void *)st, "state created for the cap test");
    if (!st)
        return;

    restore = quota_limit(t->quota, QUOTA_RES_NOTIFICATION_SUB);

    /* Hold one subscription unit FIRST so usage is provably non-zero. Capping
     * "at current usage" when usage is 0 would set the limit to 0, and 0 is
     * QUOTA_LIMIT_UNLIMITED -- the cap would silently become no cap at all and
     * this test would assert nothing. The held charge is returned at the end. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_SUB, 1,
                                                  &hold, &hold_token),
                   (uint64_t)STATUS_SUCCESS, "held one subscription unit for the cap");
    before = quota_usage(t->quota, QUOTA_RES_NOTIFICATION_SUB);
    TEST_ASSERT(before > 0, "subscription usage is non-zero before capping");

    /* Cap exactly at current usage so the next subscription cannot fit. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota,
                                             QUOTA_RES_NOTIFICATION_SUB, before),
                   (uint64_t)STATUS_SUCCESS, "subscription budget capped at usage");

    TEST_ASSERT_EQ((uint64_t)knf_subscribe(st, &sub),
                   (uint64_t)STATUS_QUOTA_EXCEEDED,
                   "subscribe past the cap reports quota, not memory");
    TEST_ASSERT_NULL((void *)sub, "a refused subscribe hands back no node");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_SUB), before,
                   "a refused subscribe leaves usage unchanged");

    (void)quota_set_limit(t->quota, QUOTA_RES_NOTIFICATION_SUB, restore);
    quota_return_chain(&hold, hold_token);
    ObDereferenceObject(st);
    (void)knf_delete_state("Kernel", "QuotaSubCap");
}

/* A state whose SECOND charge is refused must not strand its first one. This
 * is the specific hazard of taking two independent charges in one create. */
static void test_quota_knf_create_refusal_strands_nothing(void)
{
    struct task *t = task_current();
    KNF_STATE *st;
    quota_charge_receipt_t hold = { 0 };
    uint64_t hold_token = 0;
    uint64_t states_before, bytes_now, restore;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }
    restore = quota_limit(t->quota, QUOTA_RES_NOTIFICATION_BYTES);

    /* Hold retention bytes so the cap below is non-zero: a limit of 0 is
     * QUOTA_LIMIT_UNLIMITED, so capping at a zero usage would not refuse
     * anything and the test would silently prove nothing. */
    TEST_ASSERT_EQ((uint64_t)quota_charge_current(QUOTA_RES_NOTIFICATION_BYTES,
                                                  KNF_MAX_PAYLOAD,
                                                  &hold, &hold_token),
                   (uint64_t)STATUS_SUCCESS, "held retention bytes for the cap");

    states_before = quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE);
    bytes_now     = quota_usage(t->quota, QUOTA_RES_NOTIFICATION_BYTES);
    TEST_ASSERT(bytes_now > 0, "retention usage is non-zero before capping");

    /* Cap the BYTES budget only, leaving the state budget open: the state
     * charge is taken first and succeeds, the retention charge is then
     * refused, and the create must unwind the first one. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota,
                                             QUOTA_RES_NOTIFICATION_BYTES,
                                             bytes_now),
                   (uint64_t)STATUS_SUCCESS, "retention budget capped at usage");

    st = knf_create_state("Kernel", "QuotaPartial", KNF_LIFETIME_TEMPORARY,
                          KNF_SCOPE_MACHINE, NULL, KNF_KERNEL_MODE);
    TEST_ASSERT_NULL((void *)st, "create refused when the retention charge fails");
    /* TEST_ASSERT_NULL records a failure but does NOT return, so on the very
     * regression this test exists to catch the state would stay live and keep
     * both charges -- and the leak detector would then blame the next test.
     * Clean up explicitly before asserting the rollback. */
    if (st) {
        ObDereferenceObject(st);
        (void)knf_delete_state("Kernel", "QuotaPartial");
    }
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_STATE),
                   states_before,
                   "the first charge is returned when the second is refused");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_NOTIFICATION_BYTES), bytes_now,
                   "the refused retention charge left usage unchanged");

    (void)quota_set_limit(t->quota, QUOTA_RES_NOTIFICATION_BYTES, restore);
    quota_return_chain(&hold, hold_token);
}

void test_register_quota_config(void)
{
    test_suite_register_cat("Quota config: tunable names complete + unique",
                            test_quota_config_names_complete_and_unique, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: tunable name out of range",
                            test_quota_config_name_out_of_range, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: every tunable is registered",
                            test_quota_config_tunables_are_registered, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: tunable set drives the re-limit",
                            test_quota_config_tunable_set_drives_relimit, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: re-limit crosses batch boundaries",
                            test_quota_config_relimit_crosses_batches, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: re-limit completes via the tunable",
                            test_quota_config_relimit_completes_via_tunable, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: global default needs privilege",
                            test_quota_config_tunable_requires_privilege, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: scoped to USER principal",
                            test_quota_config_scoped_to_user_principal, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: seeds a new user block",
                            test_quota_config_seeds_user_block, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: re-limit reaches live blocks",
                            test_quota_config_relimit_reaches_live_blocks, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: re-limit respects explicit limits",
                            test_quota_config_relimit_respects_explicit, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota config: re-limit bad args are no-ops",
                            test_quota_config_relimit_bad_args, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota charge: current task charged after boot",
                            test_quota_charge_current_charges_after_boot, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota charge: zero amount owes nothing",
                            test_quota_charge_current_zero_amount, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota charge: bad args refused",
                            test_quota_charge_current_bad_args, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota charge: failures forwarded not flattened",
                            test_quota_charge_current_forwards_failures, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota KNF: state charge symmetry",
                            test_quota_knf_state_charge_symmetry, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota KNF: subscription charge symmetry",
                            test_quota_knf_subscription_charge_symmetry, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota KNF: subscription refused at cap",
                            test_quota_knf_subscription_refused_at_cap, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota KNF: partial create strands nothing",
                            test_quota_knf_create_refusal_strands_nothing, TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
