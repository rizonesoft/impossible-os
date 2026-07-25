/* ============================================================================
 * test_quota_dashboard.c -- quota dashboards, leak sweep, and charge-path
 * bulletproofing.
 *
 * Section 10 coverage: quota_dump / quota_dump_crash (the serial and crash
 * dashboards), quota_registry_generation + quota_leak_snapshot (the machinery
 * the per-category boot leak sweep is built on), the invocation-scoped
 * lock-section counter, and the bulletproofing rule that every charge names a
 * resource type AND an owning block.
 *
 * The taxonomy half of bulletproofing (every type has a unique name, a valid
 * unit, a descriptor) already lives in test_quota.c and is not duplicated
 * here; this file owns the CHARGE-PATH half.
 *
 * XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/quota/quota.h"
#include "kernel/security/sid.h"         /* RtlInitializeSid for the USER block */
#include "kernel/sched/task.h"           /* kthread_create / thread_yield        */
#include "kernel/sched/irql.h"           /* KeGetCurrentIrql assertion           */
#include "kernel/sched/spinlock.h"       /* spin_trylock / spin_tryunlock        */
#include "kernel/klog.h"                 /* klog_get_ring for the render check   */

/* Bounded substring search. The kernel libc has no strstr and this needs no
 * more than "does this rendered line mention X". */
static int dash_str_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0])
        return 0;
    for (uint32_t i = 0; hay[i]; i++) {
        uint32_t j = 0;
        while (needle[j] && hay[i + j] == needle[j])
            j++;
        if (!needle[j])
            return 1;
    }
    return 0;
}

/* Local SID fixture. Deliberately a private copy rather than an export from
 * test_quota.c: the RIDs below must not collide with that file's blocks, and a
 * shared builder invites exactly that coupling. */
static SID *dash_make_sid(uint8_t *buf, uint32_t rid)
{
    SID *sid = (SID *)buf;
    const uint8_t nt_authority[6] = { 0, 0, 0, 0, 0, 5 };

    RtlInitializeSid(sid, nt_authority, 1);
    uint32_t *sub = RtlSubAuthoritySid(sid, 0);
    if (sub)
        *sub = rid;
    return sid;
}

/* ---- Registry generation ------------------------------------------------- */

/* The generation is the leak sweep's coherence gate, so its ONE promise is
 * that a link or unlink is observable. Monotonic, never decreasing, and it
 * moves when a USER block enters the registry. */
static void test_quota_generation_moves_on_link(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7301);

    uint64_t before = quota_registry_generation();

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the generation test");

    uint64_t linked = quota_registry_generation();
    TEST_ASSERT(linked > before,
                "linking a new USER block advances the registry generation");

    quota_block_deref(b);

    uint64_t unlinked = quota_registry_generation();
    TEST_ASSERT(unlinked > linked,
                "unlinking that block advances the generation again");
}

/* A block that is merely LOOKED UP (the canonical-user path returning an
 * existing block) must not move the generation: the sweep would then read
 * every repeat charge by a known principal as a membership change and mark
 * otherwise perfectly coherent snapshots indeterminate. */
static void test_quota_generation_stable_on_reacquire(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7302);

    quota_block_t *first = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(first != NULL, "first acquire created the canonical block");

    uint64_t after_create = quota_registry_generation();

    quota_block_t *second = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(second == first,
                "re-acquiring the same SID returns the canonical block");
    TEST_ASSERT_EQ(quota_registry_generation(), after_create,
                   "a lookup that links nothing leaves the generation alone");

    quota_block_deref(second);
    quota_block_deref(first);
}

/* ---- Leak snapshot ------------------------------------------------------- */

/* The sweep's core claim: an outstanding charge is VISIBLE as a positive
 * per-type usage delta, and returning it puts the total back exactly. This is
 * the whole basis for gating a category on usage rather than block count. */
static void test_quota_snapshot_tracks_outstanding_usage(void)
{
    quota_leak_snapshot_t before, charged, returned;
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7303);

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the snapshot test");

    TEST_ASSERT(quota_leak_snapshot(&before) != 0,
                "baseline snapshot is coherent on a quiescent registry");

    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_HANDLE, 5), STATUS_SUCCESS,
                   "charge of 5 handles accepted");

    TEST_ASSERT(quota_leak_snapshot(&charged) != 0,
                "post-charge snapshot is coherent");
    TEST_ASSERT_EQ((uint64_t)(charged.usage[QUOTA_RES_HANDLE] -
                              before.usage[QUOTA_RES_HANDLE]), 5,
                   "the snapshot sees exactly the 5 outstanding handles");

    TEST_ASSERT_EQ(quota_return(b, QUOTA_RES_HANDLE, 5), STATUS_SUCCESS,
                   "return of 5 handles accepted");

    TEST_ASSERT(quota_leak_snapshot(&returned) != 0,
                "post-return snapshot is coherent");
    TEST_ASSERT_EQ((uint64_t)returned.usage[QUOTA_RES_HANDLE],
                   (uint64_t)before.usage[QUOTA_RES_HANDLE],
                   "returning the charge restores the baseline exactly");

    quota_block_deref(b);
}

/* A charge on ONE type must not perturb any other type's total. Without this
 * the sweep could attribute a leak to the wrong resource, which is worse than
 * no attribution -- an operator would go looking in the wrong subsystem. */
static void test_quota_snapshot_isolates_types(void)
{
    quota_leak_snapshot_t before, after;
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7304);

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the isolation test");

    TEST_ASSERT(quota_leak_snapshot(&before) != 0, "baseline coherent");
    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_TIMER, 3), STATUS_SUCCESS,
                   "charge of 3 timers accepted");
    TEST_ASSERT(quota_leak_snapshot(&after) != 0, "post-charge coherent");

    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
        int64_t delta = after.usage[t] - before.usage[t];
        if (t == (uint32_t)QUOTA_RES_TIMER)
            TEST_ASSERT_EQ((uint64_t)delta, 3, "timer total moved by 3");
        else
            TEST_ASSERT_EQ((uint64_t)delta, 0,
                           "no other resource type's total moved");
    }

    TEST_ASSERT_EQ(quota_return(b, QUOTA_RES_TIMER, 3), STATUS_SUCCESS,
                   "timers returned");
    quota_block_deref(b);
}

/* A coherent snapshot reports the generation it was taken at, and a NULL
 * output is refused rather than faulting -- the sweep calls this from the test
 * runner, where a fault would take the whole run down. */
static void test_quota_snapshot_contract(void)
{
    quota_leak_snapshot_t snap;

    TEST_ASSERT_EQ((uint64_t)quota_leak_snapshot(NULL), 0,
                   "a NULL snapshot target is refused, not dereferenced");

    TEST_ASSERT(quota_leak_snapshot(&snap) != 0, "snapshot coherent");
    TEST_ASSERT_EQ(snap.coherent, 1,
                   "the return value and the coherent field agree");
    TEST_ASSERT_EQ(snap.generation, quota_registry_generation(),
                   "a coherent snapshot reports the generation it saw");
}

/* ---- Dashboards ---------------------------------------------------------- */

/* quota_dump walks the registry, pins blocks, and releases them. The property
 * worth asserting is that it is NON-MUTATING: a dashboard that perturbed the
 * membership it reports (by ref-ing a dying block back to life, say) would
 * corrupt the very sweep that runs beside it. */
static void test_quota_dump_does_not_mutate(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7305);
    quota_leak_snapshot_t before, after;

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the dump test");
    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_THREAD, 2), STATUS_SUCCESS,
                   "charge of 2 threads accepted so the dump has a row");

    uint64_t gen_before = quota_registry_generation();
    TEST_ASSERT(quota_leak_snapshot(&before) != 0, "pre-dump snapshot coherent");

    quota_dump();

    TEST_ASSERT_EQ(quota_registry_generation(), gen_before,
                   "quota_dump links and unlinks nothing");
    TEST_ASSERT(quota_leak_snapshot(&after) != 0, "post-dump snapshot coherent");
    TEST_ASSERT_EQ((uint64_t)after.usage[QUOTA_RES_THREAD],
                   (uint64_t)before.usage[QUOTA_RES_THREAD],
                   "quota_dump charges and returns nothing");
    TEST_ASSERT_EQ(after.user_blocks, before.user_blocks,
                   "quota_dump leaves the live block count unchanged");

    TEST_ASSERT_EQ(quota_return(b, QUOTA_RES_THREAD, 2), STATUS_SUCCESS,
                   "threads returned");
    quota_block_deref(b);
}

/* The crash dashboard on the UNCONTENDED path: it must report rows without
 * mutating anything. */
static void test_quota_dump_crash_uncontended(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7306);
    uint64_t gen_before = quota_registry_generation();
    uint64_t fb_before  = quota_test_crash_fallbacks();

    quota_dump_crash();

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired around the crash dump");

    quota_dump_crash();

    TEST_ASSERT_EQ(quota_registry_generation(), gen_before + 1,
                   "only the acquire moved the generation, never the dump");

    quota_block_deref(b);
    quota_dump_crash();

    TEST_ASSERT_EQ(quota_registry_generation(), gen_before + 2,
                   "the deref unlinked; the three dumps mutated nothing");
    TEST_ASSERT_EQ(quota_test_crash_fallbacks(), fb_before,
                   "an available registry lock never takes the fallback");
}

/* The crash dashboard on the CONTENDED path -- the branch the function exists
 * for. The registry lock is genuinely held across the call through the
 * KERNEL_TESTS seam, so the try-lock must FAIL, the fallback must fire, and
 * the call must RETURN. A blocking regression cannot pass this test: it would
 * never come back, which is precisely the failure the panic path must not
 * have. Nothing between hold and release touches a quota block lock -- that is
 * the one ordering the module forbids under the registry lock. */
static void test_quota_dump_crash_falls_back_when_locked(void)
{
    uint64_t fb_before = quota_test_crash_fallbacks();
    uint64_t gen_before = quota_registry_generation();
    uint64_t flags;

    quota_test_registry_hold(&flags);
    quota_dump_crash();
    quota_test_registry_release(flags);

    TEST_ASSERT_EQ(quota_test_crash_fallbacks(), fb_before + 1,
                   "a held registry lock drives the header-only fallback once");
    TEST_ASSERT_EQ(quota_registry_generation(), gen_before,
                   "the fallback path mutates no registry state");
    TEST_ASSERT_EQ((uint64_t)KeGetCurrentIrql(), (uint64_t)PASSIVE_LEVEL,
                   "IRQL is back at PASSIVE after the held-lock dump");
}

/* spin_tryunlock must release the flag and NOTHING else -- that is the whole
 * reason it exists rather than reusing spin_unlock (which would sti). Verified
 * on a LOCAL spinlock so the assertion cannot disturb any live subsystem. */
static void test_quota_spin_tryunlock_releases_only_flag(void)
{
    spinlock_t local = SPINLOCK_INIT;

    TEST_ASSERT_EQ((uint64_t)spin_is_locked(&local), 0,
                   "a fresh spinlock starts unlocked");
    TEST_ASSERT_EQ((uint64_t)spin_trylock(&local), 1, "try-lock acquires it");
    TEST_ASSERT_EQ((uint64_t)spin_is_locked(&local), 1, "it now reads locked");
    TEST_ASSERT_EQ((uint64_t)spin_trylock(&local), 0,
                   "a second try-lock on a held lock fails instead of spinning");

    spin_tryunlock(&local);

    TEST_ASSERT_EQ((uint64_t)spin_is_locked(&local), 0,
                   "try-unlock released the flag");
    TEST_ASSERT_EQ((uint64_t)KeGetCurrentIrql(), (uint64_t)PASSIVE_LEVEL,
                   "try-unlock raised or lowered no IRQL");
    TEST_ASSERT_EQ((uint64_t)spin_trylock(&local), 1,
                   "the lock is re-acquirable after try-unlock");
    spin_tryunlock(&local);
}

/* The dashboard's RENDERED output, not merely its side-effect-freeness. A dump
 * that omitted the usage row, or mislabelled the type, would pass every
 * mutation assertion above; the operator-facing acceptance criterion is that
 * the numbers actually appear. Read back through the klog ring (a read-only
 * query, permitted in tests). */
static void test_quota_dump_renders_usage_row(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7310);
    uint32_t count = 0, head = 0;
    int found_type = 0, found_header = 0;

    /* Pin the subsystem's log level for the duration. Without this the test
     * asserts against AMBIENT logging configuration it does not own: a
     * quota-only run drops these INFO records (the whole run is quota-tagged)
     * while a full run lets them through, so the same code passed one way and
     * failed the other. Saved and restored exactly -- including the
     * had-no-override case, so this cannot leave a permanent override behind. */
    int         had_override = klog_has_override("quota");
    log_level_t prev_level   = klog_get_level("quota");
    klog_set_level("quota", LOG_DEBUG);

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the render test");
    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_SECTION, 7), STATUS_SUCCESS,
                   "charge of 7 sections accepted");

    uint64_t seq_before = klog_get_seq();
    quota_dump();
    TEST_ASSERT(klog_get_seq() > seq_before,
                "quota_dump emitted at least one klog record");

    const klog_entry_t *ring = klog_get_ring(&count, &head);
    TEST_ASSERT(ring != NULL, "klog ring readable");

    const char *type_name = quota_resource_type_name(QUOTA_RES_SECTION);
    for (uint32_t i = 0; i < count; i++) {
        const char *msg = ring[i].message;
        if (dash_str_contains(msg, "quota dashboard"))
            found_header = 1;
        if (dash_str_contains(msg, type_name) && dash_str_contains(msg, "usage"))
            found_type = 1;
    }

    TEST_ASSERT(found_header != 0, "the dashboard header was rendered");
    TEST_ASSERT(found_type != 0,
                "the charged type's usage row was rendered by name");

    TEST_ASSERT_EQ(quota_return(b, QUOTA_RES_SECTION, 7), STATUS_SUCCESS,
                   "sections returned");
    quota_block_deref(b);

    if (had_override)
        klog_set_level("quota", prev_level);
    else
        klog_remove_override("quota");
}

/* The batched pin-walk resumes across a QUOTA_DUMP_BATCH boundary via a pinned
 * cursor. With a single fixture block every test stays inside the first batch,
 * so a skipped or double-counted block at the boundary would never show. Nine
 * distinct principals force at least two batches, derived from the accessor. */
static void test_quota_snapshot_crosses_batch_boundary(void)
{
    /* DERIVED from the implementation bound, never hardcoded: a fixture count
     * frozen to a literal would stay green while silently no longer crossing a
     * boundary if the batch ever grew. */
    const uint32_t batch = quota_test_dump_batch();
    const uint32_t fixtures = batch + 1;
    enum { FIXTURES_MAX = 33 };
    quota_block_t *blocks[FIXTURES_MAX];
    uint8_t buf[SID_MAX_SIZE];
    quota_leak_snapshot_t before, after;
    uint32_t created = 0;
    uint64_t expect_sum = 0;

    TEST_ASSERT(batch >= 1, "the implementation reports a usable batch bound");
    /* RETURN, do not merely assert. TEST_ASSERT records a failure and CONTINUES,
     * so a batch grown past the fixture array would run on and write past
     * blocks[] -- a buffer overrun in the test harness itself. */
    if (fixtures > FIXTURES_MAX) {
        TEST_ASSERT(fixtures <= FIXTURES_MAX,
                    "fixture array covers batch+1 (widen FIXTURES_MAX)");
        return;
    }

    TEST_ASSERT(quota_leak_snapshot(&before) != 0, "baseline coherent");

    for (uint32_t i = 0; i < fixtures; i++) {
        SID *sid = dash_make_sid(buf, 7400 + i);
        blocks[i] = quota_user_block_acquire(sid, RtlLengthSid(sid));
        if (!blocks[i])
            break;
        created++;
        /* Distinguishable amounts: a double-count or a skip changes the sum,
         * which an all-ones fixture set could hide. */
        TEST_ASSERT_EQ(quota_charge(blocks[i], QUOTA_RES_TIMER, i + 1),
                       STATUS_SUCCESS, "per-block timer charge accepted");
        expect_sum += (uint64_t)(i + 1);
    }

    TEST_ASSERT_EQ((uint64_t)created, (uint64_t)fixtures,
                   "batch+1 USER blocks created, spanning two pin batches");
    if (created != fixtures)
        return;                 /* a short fixture set proves nothing here */

    TEST_ASSERT(quota_leak_snapshot(&after) != 0,
                "multi-batch snapshot is coherent");
    TEST_ASSERT_EQ((uint64_t)(after.user_blocks - before.user_blocks),
                   (uint64_t)fixtures,
                   "every block across the batch boundary counted exactly once");
    TEST_ASSERT_EQ((uint64_t)(after.usage[QUOTA_RES_TIMER] -
                              before.usage[QUOTA_RES_TIMER]), expect_sum,
                   "usage summed exactly across the batch boundary");

    for (uint32_t i = 0; i < created; i++) {
        TEST_ASSERT_EQ(quota_return(blocks[i], QUOTA_RES_TIMER, i + 1),
                       STATUS_SUCCESS, "per-block timer return accepted");
        quota_block_deref(blocks[i]);
    }
}

/* The aggregate can legally overflow: one block may hold QUOTA_AMOUNT_MAX,
 * which IS INT64_MAX, so a second charged block exceeds the domain. The
 * snapshot must refuse to classify rather than wrap into a negative total --
 * a negative total would make the sweep read a real leak as "no positive
 * delta" and pass. */
static void test_quota_snapshot_overflow_is_indeterminate(void)
{
    uint8_t buf_a[SID_MAX_SIZE], buf_b[SID_MAX_SIZE];
    SID *sid_a = dash_make_sid(buf_a, 7411);
    SID *sid_b = dash_make_sid(buf_b, 7412);
    quota_leak_snapshot_t snap;

    quota_block_t *a = quota_user_block_acquire(sid_a, RtlLengthSid(sid_a));
    quota_block_t *b = quota_user_block_acquire(sid_b, RtlLengthSid(sid_b));
    TEST_ASSERT(a != NULL && b != NULL, "two USER blocks acquired");

    TEST_ASSERT_EQ(quota_charge(a, QUOTA_RES_CRASH_BUFFER,
                                (uint64_t)QUOTA_AMOUNT_MAX),
                   STATUS_SUCCESS, "one block may hold the whole domain");
    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_CRASH_BUFFER, 1),
                   STATUS_SUCCESS, "a second block charges 1 more");

    TEST_ASSERT_EQ((uint64_t)quota_leak_snapshot(&snap), 0,
                   "an aggregate past the counter domain is indeterminate");
    TEST_ASSERT_EQ(snap.coherent, 0,
                   "the coherent flag agrees with the return value");
    TEST_ASSERT(snap.usage[QUOTA_RES_CRASH_BUFFER] >= 0,
                "the refused aggregate never wrapped negative");

    TEST_ASSERT_EQ(quota_return(a, QUOTA_RES_CRASH_BUFFER,
                                (uint64_t)QUOTA_AMOUNT_MAX),
                   STATUS_SUCCESS, "domain-wide charge returned");
    TEST_ASSERT_EQ(quota_return(b, QUOTA_RES_CRASH_BUFFER, 1),
                   STATUS_SUCCESS, "the extra unit returned");
    quota_block_deref(a);
    quota_block_deref(b);
}

/* The counter-mutation epoch is what lets the snapshot see a charge, a
 * return, and above all a TRANSFER -- the mutation that moves usage between
 * two blocks without touching registry membership or the grand total, and
 * which two agreeing walks would therefore certify as coherent. */
static void test_quota_mutation_epoch_sees_all_mutations(void)
{
    uint8_t buf_a[SID_MAX_SIZE], buf_b[SID_MAX_SIZE];
    SID *sid_a = dash_make_sid(buf_a, 7421);
    SID *sid_b = dash_make_sid(buf_b, 7422);

    quota_block_t *a = quota_user_block_acquire(sid_a, RtlLengthSid(sid_a));
    quota_block_t *b = quota_user_block_acquire(sid_b, RtlLengthSid(sid_b));
    TEST_ASSERT(a != NULL && b != NULL, "two USER blocks acquired");

    uint64_t e0 = quota_test_mutation_epoch();
    TEST_ASSERT_EQ(quota_charge(a, QUOTA_RES_TIMER, 4), STATUS_SUCCESS,
                   "charge accepted");
    uint64_t e1 = quota_test_mutation_epoch();
    TEST_ASSERT(e1 > e0, "a charge advances the mutation epoch");

    TEST_ASSERT_EQ(quota_try_transfer(a, b, QUOTA_RES_TIMER, 4),
                   STATUS_SUCCESS, "transfer accepted");
    uint64_t e2 = quota_test_mutation_epoch();
    TEST_ASSERT(e2 > e1,
                "a transfer advances the epoch, though it moves no total");

    TEST_ASSERT_EQ(quota_return(b, QUOTA_RES_TIMER, 4), STATUS_SUCCESS,
                   "return accepted");
    TEST_ASSERT(quota_test_mutation_epoch() > e2,
                "a return advances the epoch");

    /* Outside any quota critical section, nothing is in progress. The in-flight
     * case (a writer parked between a transfer's two stores) cannot be forced
     * on a sequential single-CPU runner without an operation-level checkpoint;
     * it is the same blocker as the cross-CPU contention proof. */
    TEST_ASSERT_EQ((uint64_t)quota_test_writers_active(), 0,
                   "no mutation is in progress between quota operations");

    quota_block_deref(a);
    quota_block_deref(b);
}

/* ---- Sweep classification ------------------------------------------------ */

/* Every branch of the gate the host parser folds into FAILED, driven with
 * SYNTHETIC snapshots. Driving a real category into each state would require
 * planting a leak in production code, so without this the gate's decision
 * logic ships untested and an inverted comparison reads as a plausible zero. */
static void test_quota_sweep_classification(void)
{
    quota_leak_snapshot_t open, close;
    uint32_t types = 99;

    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++) {
        open.usage[t]  = 100;
        close.usage[t] = 100;
    }
    open.user_blocks = close.user_blocks = 4;
    open.coherent    = close.coherent    = 1;
    open.generation  = close.generation  = 7;

    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(&open, &close, &types),
                   (uint64_t)QUOTA_SWEEP_CLEAN, "equal snapshots read clean");
    TEST_ASSERT_EQ((uint64_t)types, 0, "a clean sweep names no leaked types");

    /* One type grows. */
    close.usage[QUOTA_RES_HANDLE] = 101;
    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(&open, &close, &types),
                   (uint64_t)QUOTA_SWEEP_LEAKED, "a positive delta leaks");
    TEST_ASSERT_EQ((uint64_t)types, 1, "exactly one type reported");

    /* A second type grows: still ONE leaking category, two types. */
    close.usage[QUOTA_RES_TIMER] = 250;
    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(&open, &close, &types),
                   (uint64_t)QUOTA_SWEEP_LEAKED, "two positive deltas leak");
    TEST_ASSERT_EQ((uint64_t)types, 2, "both types reported");

    /* Negative deltas are returns, never leaks. */
    close.usage[QUOTA_RES_HANDLE] = 1;
    close.usage[QUOTA_RES_TIMER]  = 0;
    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(&open, &close, &types),
                   (uint64_t)QUOTA_SWEEP_CLEAN,
                   "a category that ends holding LESS did not leak");

    /* Block count moved with no usage delta: reported, not gated. */
    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++)
        close.usage[t] = open.usage[t];
    close.user_blocks = 5;
    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(&open, &close, &types),
                   (uint64_t)QUOTA_SWEEP_COUNT_ONLY,
                   "a retained canonical block is count-only, not a leak");
    TEST_ASSERT_EQ((uint64_t)types, 0, "count-only names no leaked types");

    /* Incoherence outranks everything, including a positive delta. */
    close.usage[QUOTA_RES_HANDLE] = 9999;
    close.coherent = 0;
    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(&open, &close, &types),
                   (uint64_t)QUOTA_SWEEP_INDETERMINATE,
                   "an incoherent CLOSING snapshot is never a leak verdict");
    close.coherent = 1;
    open.coherent  = 0;
    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(&open, &close, &types),
                   (uint64_t)QUOTA_SWEEP_INDETERMINATE,
                   "an incoherent OPENING snapshot is never a leak verdict");

    TEST_ASSERT_EQ((uint64_t)quota_sweep_classify(NULL, &close, &types),
                   (uint64_t)QUOTA_SWEEP_INDETERMINATE,
                   "a NULL snapshot is refused, not dereferenced");

    /* The gate FAILS CLOSED. An indeterminate verdict must not be a synonym
     * for clean: a gate that could not be established is a failed gate, and
     * treating it as a pass would disarm the sweep precisely when concurrent
     * activity makes measurement hard. Asserted here as a distinct verdict so
     * the runner cannot quietly downgrade it to CLEAN. */
    TEST_ASSERT(QUOTA_SWEEP_INDETERMINATE != QUOTA_SWEEP_CLEAN,
                "INDETERMINATE is never the same verdict as CLEAN");
}

/* ---- Charge-path bulletproofing ------------------------------------------ */

/* "Every charge path names a resource type and an owner." The enforcement is
 * that a charge missing EITHER is refused, and refused with the documented
 * status -- not silently attributed to some default block or type. */
static void test_quota_charge_requires_type_and_owner(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7307);

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the bulletproofing test");

    /* No owner named. */
    TEST_ASSERT_EQ(quota_charge(NULL, QUOTA_RES_HANDLE, 1),
                   STATUS_INVALID_PARAMETER,
                   "a charge with no owning block is refused");
    TEST_ASSERT_EQ(quota_return(NULL, QUOTA_RES_HANDLE, 1),
                   STATUS_INVALID_PARAMETER,
                   "a return with no owning block is refused");

    /* No valid type named. */
    TEST_ASSERT_EQ(quota_charge(b, (quota_resource_type_t)
                                QUOTA_RESOURCE_TYPE_COUNT, 1),
                   STATUS_INVALID_PARAMETER,
                   "a charge naming no valid resource type is refused");
    TEST_ASSERT_EQ(quota_return(b, (quota_resource_type_t)
                                QUOTA_RESOURCE_TYPE_COUNT, 1),
                   STATUS_INVALID_PARAMETER,
                   "a return naming no valid resource type is refused");

    quota_block_deref(b);
}

/* A refused charge -- and the documented zero-amount no-op -- must leave
 * NOTHING charged. If a rejected call moved a counter, the leak sweep would
 * report a leak for an operation that never succeeded, and every QLEAK line
 * would become suspect. */
static void test_quota_refused_charge_moves_nothing(void)
{
    quota_leak_snapshot_t before, after;
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7308);

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the refusal test");

    TEST_ASSERT(quota_leak_snapshot(&before) != 0, "baseline coherent");

    TEST_ASSERT_EQ(quota_charge(b, (quota_resource_type_t)
                                QUOTA_RESOURCE_TYPE_COUNT, 9),
                   STATUS_INVALID_PARAMETER, "invalid-type charge refused");
    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_HANDLE, 0), STATUS_SUCCESS,
                   "zero-amount charge is the documented no-op success");
    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_HANDLE, QUOTA_AMOUNT_MAX + 1ull),
                   STATUS_INVALID_PARAMETER,
                   "an amount outside the counter domain is refused");

    TEST_ASSERT(quota_leak_snapshot(&after) != 0, "post-refusal coherent");
    for (uint32_t t = 0; t < QUOTA_RESOURCE_TYPE_COUNT; t++)
        TEST_ASSERT_EQ((uint64_t)(after.usage[t] - before.usage[t]), 0,
                       "a refused charge left every total untouched");

    quota_block_deref(b);
}

/* ---- Invocation-scoped lock accounting ----------------------------------- */

/* The counter now attributes by ARMING THREAD at PASSIVE_LEVEL rather than by
 * CPU. The observable contract is unchanged for the budgeted operations, so
 * assert the budget exactly -- a regression in the scoping rule shows up here
 * as an off-by-N rather than as a silent drift. */
static void test_quota_lock_count_invocation_scoped(void)
{
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7309);

    TEST_ASSERT_EQ((uint64_t)KeGetCurrentIrql(), (uint64_t)PASSIVE_LEVEL,
                   "the suite body runs at PASSIVE, where sections count");

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the lock-count test");

    quota_test_lock_count_begin();
    TEST_ASSERT_EQ(quota_charge(b, QUOTA_RES_HANDLE, 1), STATUS_SUCCESS,
                   "charge accepted inside the counted window");
    uint64_t charge_sections = quota_test_lock_count_end();

    TEST_ASSERT_EQ(charge_sections, (uint64_t)QUOTA_BUDGET_CHARGE_LOCKS,
                   "one charge completes exactly its budgeted lock sections");

    quota_test_lock_count_begin();
    TEST_ASSERT_EQ(quota_return(b, QUOTA_RES_HANDLE, 1), STATUS_SUCCESS,
                   "return accepted inside the counted window");
    uint64_t return_sections = quota_test_lock_count_end();

    TEST_ASSERT_EQ(return_sections, (uint64_t)QUOTA_BUDGET_RETURN_LOCKS,
                   "one return completes exactly its budgeted lock sections");

    quota_block_deref(b);
}

/* A window that is armed and immediately closed must read zero: nothing ran
 * inside it. */
static void test_quota_lock_count_empty_window_is_zero(void)
{
    quota_test_lock_count_begin();
    uint64_t sections = quota_test_lock_count_end();

    TEST_ASSERT_EQ(sections, 0,
                   "an empty counted window attributes no lock sections");
}

/* THE regression this scoping change exists to prevent: a FOREIGN thread's
 * lock sections contaminating the armed window. Both the old CPU-scoped rule
 * and the new thread-scoped rule pass a test that only ever charges on the
 * arming thread, so neither of the two tests above can tell them apart. Here a
 * worker kthread runs a full charge/return on the SAME CPU (the runner is
 * single-CPU) inside the window; under CPU scoping its two sections would land
 * in the total, under thread scoping they must not.
 *
 * The window is closed on the worker's own DONE signal rather than after
 * thread_join, so the worker's teardown -- which takes owner locks and may run
 * on the joining thread -- lands outside the measurement. */
static volatile int s_dash_worker_done;

/* The block arrives as the worker's ARGUMENT carrying its OWN reference, not
 * through a global the driver can clear. On the timeout path the driver stops
 * waiting and drops its reference while the worker may still be running; if
 * the worker were reading a global and holding no reference, that drop could
 * free the block underneath a live charge. Owning a reference makes the
 * worker's access safe no matter when it actually runs, and its release is the
 * last thing it does. */
static void dash_lock_scope_worker(void *arg)
{
    quota_block_t *b = (quota_block_t *)arg;

    if (b) {
        quota_charge(b, QUOTA_RES_HANDLE, 1);
        quota_return(b, QUOTA_RES_HANDLE, 1);
    }
    __atomic_store_n(&s_dash_worker_done, 1, __ATOMIC_RELEASE);
    if (b)
        quota_block_deref(b);   /* the worker's own reference */
}

static void test_quota_lock_count_excludes_foreign_thread(void)
{
    /* Bounded wait: the scheduler is flat-cyclic over ALL runnable threads, so
     * a created thread is always eventually picked; the budget only stops a
     * broken scheduler from hanging the whole sweep. */
    enum { SPIN_BUDGET = 100000 };
    uint8_t buf[SID_MAX_SIZE];
    SID    *sid = dash_make_sid(buf, 7320);

    quota_block_t *b = quota_user_block_acquire(sid, RtlLengthSid(sid));
    TEST_ASSERT(b != NULL, "USER block acquired for the foreign-thread test");

    __atomic_store_n(&s_dash_worker_done, 0, __ATOMIC_RELEASE);

    /* Hand the worker its own reference BEFORE it can run. Taken outside the
     * counted window: quota_block_ref is not a quota critical section, but
     * ordering it first keeps the measurement about the worker's charge. */
    quota_block_ref(b);

    quota_test_lock_count_begin();

    int tid = kthread_create(dash_lock_scope_worker, (void *)b, 0);
    if (tid < 0) {
        /* Nothing will consume the worker's reference; release it here or it
         * leaks the block for the rest of the boot. */
        quota_test_lock_count_end();
        quota_block_deref(b);
        quota_block_deref(b);
        TEST_ASSERT(tid >= 0, "worker kthread spawned");
        return;
    }

    int ran = 0;
    for (uint32_t spin = 0; spin < SPIN_BUDGET; spin++) {
        if (__atomic_load_n(&s_dash_worker_done, __ATOMIC_ACQUIRE)) {
            ran = 1;
            break;
        }
        thread_yield();
    }

    uint64_t sections = quota_test_lock_count_end();

    TEST_ASSERT(ran != 0, "the worker completed its charge/return in-window");
    TEST_ASSERT_EQ(sections, 0,
                   "a foreign thread's lock sections stay out of the window");

    /* Join ONLY once the worker signalled DONE. thread_join has no timeout, so
     * joining after the budget expired would convert a failed assertion into a
     * hang of the entire sweep -- the opposite of the bounded wait above. On
     * that path the thread is left unreaped, which is the cheaper failure: the
     * assertion has already recorded the problem, and the worker's own block
     * reference means a late run cannot touch freed memory. */
    if (ran) {
        thread_join((uint32_t)tid);
        TEST_ASSERT_EQ((uint64_t)quota_test_raw_usage(b, QUOTA_RES_HANDLE), 0,
                       "the worker's charge was returned, leaving nothing behind");
    }

    quota_block_deref(b);       /* the driver's reference only */
}

/* ---- Registration -------------------------------------------------------- */

void test_register_quota_dashboard(void)
{
    test_suite_register_cat("Quota: generation moves on link/unlink",
                            test_quota_generation_moves_on_link,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: generation stable on re-acquire",
                            test_quota_generation_stable_on_reacquire,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: snapshot tracks outstanding usage",
                            test_quota_snapshot_tracks_outstanding_usage,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: snapshot isolates resource types",
                            test_quota_snapshot_isolates_types,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: snapshot contract",
                            test_quota_snapshot_contract, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: dump does not mutate",
                            test_quota_dump_does_not_mutate, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: crash dump uncontended",
                            test_quota_dump_crash_uncontended, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: crash dump falls back when locked",
                            test_quota_dump_crash_falls_back_when_locked,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: spin_tryunlock releases only the flag",
                            test_quota_spin_tryunlock_releases_only_flag,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: dump renders the usage row",
                            test_quota_dump_renders_usage_row, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: snapshot crosses batch boundary",
                            test_quota_snapshot_crosses_batch_boundary,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: snapshot overflow is indeterminate",
                            test_quota_snapshot_overflow_is_indeterminate,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: sweep classification branches",
                            test_quota_sweep_classification, TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: mutation epoch sees all mutations",
                            test_quota_mutation_epoch_sees_all_mutations,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: lock count excludes foreign thread",
                            test_quota_lock_count_excludes_foreign_thread,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: charge requires type and owner",
                            test_quota_charge_requires_type_and_owner,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: refused charge moves nothing",
                            test_quota_refused_charge_moves_nothing,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: lock count is invocation-scoped",
                            test_quota_lock_count_invocation_scoped,
                            TEST_CAT_QUOTA);
    test_suite_register_cat("Quota: empty lock-count window is zero",
                            test_quota_lock_count_empty_window_is_zero,
                            TEST_CAT_QUOTA);
}

#endif /* KERNEL_TESTS */
