/* ============================================================================
 * test_harness.c -- Unit tests for the test framework itself.
 *
 * The action/cleanup registry (see kernel-test-harness roadmap) fires callbacks AFTER the
 * suite body returns. That means we cannot verify drain order from
 * inside the same suite: actions haven't run yet at the point of the
 * last TEST_ASSERT. Instead, suite N registers actions that write
 * observations to file-scope statics; suite N+1 reads those statics
 * and asserts the expected shape. The test_runner runs suites in
 * registration order, so N+1 always runs after N's drain completes.
 *
 * Coverage:
 *   1. LIFO drain: 3 actions recorded in reverse registration order.
 *   2. Over-registration: 33rd test_add_action returns -1; existing 32
 *      still drain normally.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/scratch.h" /* TEST_SCRATCH_KBUF for scratch-buffer tests */
#include "kernel/types.h"
#include "kernel/sched/irql.h"  /* KeGetCurrentIrql / KeRaiseIrql for IRQL recovery test */
#include "kernel/mm/heap.h"     /* heap_get_used for delta checks */
#include "kernel/mm/pmm.h"      /* pmm_get_used_frames for pmm delta checks */

/* ---------------------------------------------------------------------------
 * LIFO drain observation -- suite A registers, suite B verifies.
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_lifo_drain_count;
static volatile uint64_t g_lifo_drain_ctxs[4];

static void harness_action_record(void *ctx)
{
    uint32_t idx = g_lifo_drain_count;
    if (idx < 4) {
        g_lifo_drain_ctxs[idx] = (uint64_t)(uintptr_t)ctx;
        g_lifo_drain_count = idx + 1;
    }
}

static void test_harness_register_lifo_actions(void)
{
    g_lifo_drain_count = 0;
    g_lifo_drain_ctxs[0] = 0;
    g_lifo_drain_ctxs[1] = 0;
    g_lifo_drain_ctxs[2] = 0;
    g_lifo_drain_ctxs[3] = 0;

    int r1 = test_add_action(harness_action_record, (void *)(uintptr_t)0x1111);
    int r2 = test_add_action(harness_action_record, (void *)(uintptr_t)0x2222);
    int r3 = test_add_action(harness_action_record, (void *)(uintptr_t)0x3333);

    TEST_ASSERT_EQ(r1, 0, "first action registered");
    TEST_ASSERT_EQ(r2, 0, "second action registered");
    TEST_ASSERT_EQ(r3, 0, "third action registered");
    /* Suite body returns here; runner drains actions in LIFO order. */
}

static void test_harness_verify_lifo_drain(void)
{
    TEST_ASSERT_EQ(g_lifo_drain_count, 3, "all 3 actions fired after suite exit");
    TEST_ASSERT_EQ(g_lifo_drain_ctxs[0], 0x3333,
                   "LIFO: last-registered (0x3333) fires first");
    TEST_ASSERT_EQ(g_lifo_drain_ctxs[1], 0x2222,
                   "LIFO: middle (0x2222) fires second");
    TEST_ASSERT_EQ(g_lifo_drain_ctxs[2], 0x1111,
                   "LIFO: first-registered (0x1111) fires last");
}

/* ---------------------------------------------------------------------------
 * Over-registration -- 33rd add returns -1; first 32 drain normally.
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_overflow_fire_count;
static volatile int g_overflow_33rd_return;

static void harness_action_bump(void *ctx)
{
    (void)ctx;
    g_overflow_fire_count++;
}

static void test_harness_register_overflow(void)
{
    g_overflow_fire_count = 0;
    g_overflow_33rd_return = 0;

    int i;
    int accepted = 0;
    for (i = 0; i < 33; i++) {
        int r = test_add_action(harness_action_bump, (void *)(uintptr_t)i);
        if (i < 32) {
            if (r == 0) accepted++;
        } else {
            g_overflow_33rd_return = r;
        }
    }

    TEST_ASSERT_EQ(accepted, 32, "first 32 actions accepted");
    TEST_ASSERT_EQ(g_overflow_33rd_return, -1,
                   "33rd action rejected with -1");
}

static void test_harness_verify_overflow_drain(void)
{
    TEST_ASSERT_EQ(g_overflow_fire_count, 32,
                   "exactly 32 actions drained (33rd never registered)");
}

/* ---------------------------------------------------------------------------
 * NULL fn rejection -- test_add_action(NULL, ctx) returns -1.
 * ------------------------------------------------------------------------- */

static void test_harness_null_fn_rejected(void)
{
    int r = test_add_action(NULL, (void *)(uintptr_t)0xDEAD);
    TEST_ASSERT_EQ(r, -1, "test_add_action(NULL, ctx) rejected with -1");
    /* No cleanup needed -- nothing got registered. */
}

/* ---------------------------------------------------------------------------
 * Re-entrant registration rejected during drain.
 *
 * An action that tries to register another action must fail; otherwise
 * the drain loop could run forever. Suite 1 registers a single action
 * whose body calls test_add_action again and records the return code.
 * Suite 2 asserts the re-entrant call was rejected with -1 and the
 * drain completed exactly once.
 * ------------------------------------------------------------------------- */

static volatile int g_reentrant_fire_count;
static volatile int g_reentrant_add_return;

static void harness_action_reentrant(void *ctx)
{
    (void)ctx;
    g_reentrant_fire_count++;
    /* Try to re-register ourselves. Must be rejected during drain. */
    g_reentrant_add_return = test_add_action(harness_action_reentrant, NULL);
}

static void test_harness_register_reentrant(void)
{
    g_reentrant_fire_count = 0;
    g_reentrant_add_return = 0;
    int r = test_add_action(harness_action_reentrant, NULL);
    TEST_ASSERT_EQ(r, 0, "initial action registered");
}

static void test_harness_verify_reentrant_rejected(void)
{
    TEST_ASSERT_EQ(g_reentrant_fire_count, 1,
                   "re-entrant action fired exactly once (no drain loop hang)");
    TEST_ASSERT_EQ(g_reentrant_add_return, -1,
                   "test_add_action during drain returned -1");
}

/* ---------------------------------------------------------------------------
 * IRQL recovery -- runner force-lowers IRQL if a suite leaks elevation.
 *
 * Suite 1 registers an action that records KeGetCurrentIrql(), then
 * KeRaiseIrql to DISPATCH_LEVEL without lowering. The runner's drain
 * code SHOULD observe the elevated IRQL, warn, and force-lower to
 * PASSIVE_LEVEL before invoking the action. Suite 2 asserts the
 * action saw PASSIVE_LEVEL and that the test_runner itself resumed
 * at PASSIVE_LEVEL for the next suite.
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_irql_action_observed;
static volatile uint32_t g_irql_next_suite_observed;

static void harness_action_record_irql(void *ctx)
{
    (void)ctx;
    g_irql_action_observed = (uint32_t)KeGetCurrentIrql();
}

static void test_harness_leak_irql(void)
{
    g_irql_action_observed = 0xFFu;         /* sentinel: no observation yet */
    g_irql_next_suite_observed = 0xFFu;
    int r = test_add_action(harness_action_record_irql, NULL);
    TEST_ASSERT_EQ(r, 0, "IRQL-probe action registered");
    /* Deliberately leak DISPATCH_LEVEL -- runner must recover. */
    KIRQL saved;
    KeRaiseIrql(DISPATCH_LEVEL, &saved);
    /* Return to the runner WITHOUT lowering. The [WARN] message
     * 'left IRQL elevated' is the observable runner behaviour; the
     * action should then run at PASSIVE_LEVEL. */
}

static void test_harness_verify_irql_recovery(void)
{
    g_irql_next_suite_observed = (uint32_t)KeGetCurrentIrql();
    TEST_ASSERT_EQ(g_irql_action_observed, (uint32_t)PASSIVE_LEVEL,
                   "action ran at PASSIVE_LEVEL after runner forced IRQL down");
    TEST_ASSERT_EQ(g_irql_next_suite_observed, (uint32_t)PASSIVE_LEVEL,
                   "next suite starts at PASSIVE_LEVEL (runner recovered)");
}

/* ---------------------------------------------------------------------------
 * Action-level IRQL leak -- one cleanup action raises DISPATCH_LEVEL and
 * returns without lowering; the next action (drained AFTER it in LIFO
 * order) and the following suite must still start at PASSIVE_LEVEL.
 * Proves the per-action IRQL recovery path in test_actions_drain().
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_aleak_first_action_irql;
static volatile int g_aleak_second_ran;

static void harness_action_second(void *ctx)
{
    (void)ctx;
    /* Second in LIFO order -- fires AFTER the leaking action. If the
     * runner's per-action recovery works, this sees PASSIVE_LEVEL. */
    g_aleak_first_action_irql = (uint32_t)KeGetCurrentIrql();
    g_aleak_second_ran = 1;
}

static void harness_action_leak_irql(void *ctx)
{
    (void)ctx;
    /* First in LIFO order -- raise IRQL and return without lowering. */
    KIRQL saved;
    KeRaiseIrql(DISPATCH_LEVEL, &saved);
}

static void test_harness_register_action_leak(void)
{
    g_aleak_first_action_irql = 0xFFu;
    g_aleak_second_ran = 0;
    /* Register the observer SECOND so it fires LAST in LIFO order;
     * register the leaker FIRST so it fires FIRST (and its IRQL leak
     * must be recovered before the observer runs). */
    int r1 = test_add_action(harness_action_second, NULL);
    int r2 = test_add_action(harness_action_leak_irql, NULL);
    TEST_ASSERT_EQ(r1, 0, "observer action registered");
    TEST_ASSERT_EQ(r2, 0, "IRQL-leaking action registered");
}

static void test_harness_verify_action_leak_recovery(void)
{
    TEST_ASSERT_EQ(g_aleak_second_ran, 1,
                   "second action ran (not blocked by first action's leak)");
    TEST_ASSERT_EQ(g_aleak_first_action_irql, (uint32_t)PASSIVE_LEVEL,
                   "second action saw PASSIVE_LEVEL after runner recovered from leak");
    TEST_ASSERT_EQ((uint32_t)KeGetCurrentIrql(), (uint32_t)PASSIVE_LEVEL,
                   "next suite after action-leak chain starts at PASSIVE_LEVEL");
}

/* ---------------------------------------------------------------------------
 * TEST_SCRATCH_KBUF -- kmalloc route + PMM route + rollback.
 *
 * Each register-suite snapshots heap + PMM counters, allocates via
 * TEST_SCRATCH_KBUF, and samples mid-suite counters to prove the
 * correct allocator was hit. The follow-up verify-suite asserts
 * heap + PMM returned to the pre-snapshot after the §7 action drain
 * fires between suites. The rollback suite at the end exercises
 * the test_scratch_free path manually when the action stack is full.
 * ------------------------------------------------------------------------- */

static volatile uint64_t g_scratch_heap_pre;
static volatile uint64_t g_scratch_pmm_pre;
/* Sample heap/pmm usage mid-suite to confirm the alloc actually
 * landed on its intended allocator (otherwise a test that claims
 * 'PMM path' might silently hit kmalloc and pass for the wrong
 * reason). */
static volatile uint64_t g_scratch_heap_during_small;
static volatile uint64_t g_scratch_pmm_during_big;

/* Drain-after-failure coverage note: TEST_ASSERT is NOT longjmp-style
 * in this runner -- suite bodies always return normally and the §7
 * action drain fires identically on pass and on fail (same code path
 * in test_runner_run). A dedicated "forced-fail" suite that would
 * increment the global failed counter breaks scripts/test.sh's
 * exit-code gate (the passed/failed summary regex only matches the
 * zero-failure shape, so the first real failure would fail the gate
 * for every subsequent commit). Drain-on-pass coverage below
 * therefore also proves drain-on-fail by construction.
 *
 * Case A: 512 bytes -- routes to kmalloc. Mid-suite heap_get_used
 * should be > pre-snapshot (proving kmalloc was hit); PMM counter
 * must NOT change (proving PMM was NOT hit). Post-drain both return
 * to pre-snapshot. */
static void test_harness_scratch_kmalloc_route(void)
{
    g_scratch_heap_pre = heap_get_used();
    g_scratch_pmm_pre  = pmm_get_used_frames();

    TEST_SCRATCH_KBUF(small, 512);
    (void)small;

    /* Sample mid-suite so the verify step can prove the heap actually
     * grew (kmalloc path was taken). */
    g_scratch_heap_during_small = heap_get_used();

    TEST_ASSERT(g_scratch_heap_during_small > g_scratch_heap_pre,
                "512-byte scratch grew heap_get_used (kmalloc route)");
    TEST_ASSERT_EQ(pmm_get_used_frames(), g_scratch_pmm_pre,
                   "512-byte scratch did NOT touch PMM (kmalloc route)");
}

static void test_harness_scratch_kmalloc_verify(void)
{
    TEST_ASSERT_EQ(heap_get_used(), g_scratch_heap_pre,
                   "heap_get_used returned to pre-snapshot after kmalloc scratch drain");
    TEST_ASSERT_EQ(pmm_get_used_frames(), g_scratch_pmm_pre,
                   "pmm frames unchanged across kmalloc scratch suite");
}

/* Case C: 64 KiB -- routes to PMM. Mid-suite pmm_get_used_frames
 * should be > pre-snapshot by 16 (65536 / 4096); heap must NOT
 * change. Post-drain both return. */
static void test_harness_scratch_pmm_route(void)
{
    g_scratch_heap_pre = heap_get_used();
    g_scratch_pmm_pre  = pmm_get_used_frames();

    TEST_SCRATCH_KBUF(big, 65536);
    (void)big;

    g_scratch_pmm_during_big = pmm_get_used_frames();

    TEST_ASSERT_EQ(g_scratch_pmm_during_big, g_scratch_pmm_pre + 16,
                   "64 KiB scratch grew pmm_get_used_frames by exactly 16 pages");
    TEST_ASSERT_EQ(heap_get_used(), g_scratch_heap_pre,
                   "64 KiB scratch did NOT touch heap (PMM route)");
}

static void test_harness_scratch_pmm_verify(void)
{
    TEST_ASSERT_EQ(heap_get_used(), g_scratch_heap_pre,
                   "heap unchanged across PMM scratch suite");
    TEST_ASSERT_EQ(pmm_get_used_frames(), g_scratch_pmm_pre,
                   "pmm_get_used_frames returned to pre-snapshot after PMM scratch drain");
}

/* Case D: rollback path -- when test_add_action rejects registration
 * (action stack full), the TEST_SCRATCH_KBUF macro must free the
 * allocation before returning. We can't exercise the macro's rollback
 * directly from a test body because the macro calls `return`; instead
 * we exercise the PRIMITIVE (test_scratch_free called manually after
 * test_scratch_alloc + a failed test_add_action) which is what the
 * macro's rollback path invokes. This proves the unwind path leaves
 * heap_get_used at its pre-snapshot value. */
static void scratch_rollback_noop_action(void *ctx)
{
    (void)ctx;
}

static void test_harness_scratch_rollback(void)
{
    uint64_t heap_pre = heap_get_used();

    /* Fill the 32-slot action stack so the next test_add_action returns -1. */
    int filler_ok = 0;
    for (int i = 0; i < 32; i++) {
        if (test_add_action(scratch_rollback_noop_action, NULL) == 0)
            filler_ok++;
    }
    TEST_ASSERT_EQ(filler_ok, 32, "all 32 filler actions registered");

    /* Attempt a scratch allocation + registration path (not via macro). */
    void *buf = test_scratch_alloc(512);
    TEST_ASSERT_NOT_NULL(buf, "test_scratch_alloc succeeded despite full action stack");
    if (!buf)
        return;

    int r = test_add_action(test_scratch_free, buf);
    TEST_ASSERT_EQ(r, -1, "test_add_action rejects with -1 when stack is full");

    /* Manual rollback -- exactly what the macro's rollback branch does. */
    test_scratch_free(buf);

    TEST_ASSERT_EQ(heap_get_used(), heap_pre,
                   "heap returned to pre-snapshot after manual scratch rollback");
    /* The 32 filler actions still drain normally on suite exit (LIFO);
     * the action registry is not corrupted by the rejected insertion. */
}

/* Registration */
void test_register_harness(void)
{
    /* The order here MATTERS: each verify-suite depends on the preceding
     * register-suite's actions having drained. Do not reorder. */
    test_suite_register_cat("Harness: register 3 actions",
                            test_harness_register_lifo_actions, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: 3 actions fired LIFO after suite exit",
                            test_harness_verify_lifo_drain, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: action list full returns -1",
                            test_harness_register_overflow, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: 32 actions drained after overflow",
                            test_harness_verify_overflow_drain, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: NULL fn rejected",
                            test_harness_null_fn_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: register re-entrant action",
                            test_harness_register_reentrant, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: re-entrant add rejected during drain",
                            test_harness_verify_reentrant_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: register IRQL-leaking action",
                            test_harness_leak_irql, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: runner force-lowered IRQL for drain",
                            test_harness_verify_irql_recovery, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: register action-pair with IRQL leak",
                            test_harness_register_action_leak, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: per-action IRQL recovery between callbacks",
                            test_harness_verify_action_leak_recovery, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: TEST_SCRATCH_KBUF kmalloc route (512 B)",
                            test_harness_scratch_kmalloc_route, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: scratch kmalloc route returns to pre-snapshot",
                            test_harness_scratch_kmalloc_verify, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: TEST_SCRATCH_KBUF PMM route (64 KiB)",
                            test_harness_scratch_pmm_route, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: scratch PMM route returns to pre-snapshot",
                            test_harness_scratch_pmm_verify, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: scratch rollback on full action stack",
                            test_harness_scratch_rollback, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
