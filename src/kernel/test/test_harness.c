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
#include "kernel/test/klog_suppress.h" /* TEST_KLOG_SUPPRESS for klog-demotion tests */
#include "kernel/types.h"
#include "kernel/sched/irql.h"  /* KeGetCurrentIrql / KeRaiseIrql for IRQL recovery test */
#include "kernel/mm/heap.h"     /* heap_get_used for delta checks */
#include "kernel/mm/pmm.h"      /* pmm_get_used_frames for pmm delta checks */
#include "kernel/klog.h"        /* klog_get_level / klog_set_level for suppress tests */
#include "libc/string.h"        /* strcmp for the [COUNT] record shape assertions */

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
 * heap + PMM returned to the pre-snapshot after the action drain
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
 * in this runner -- suite bodies always return normally and the
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

/* ---------------------------------------------------------------------------
 * TEST_KLOG_SUPPRESS -- block-scoped klog level demotion.
 *
 * Raises the subsystem's minimum level to LOG_FATAL for the remainder
 * of the current suite, then the action drain restores it after
 * suite exit. Register-suite snapshots the level BEFORE and AFTER
 * calling the macro; verify-suite asserts the level was restored on
 * drain (proving the action registration works end-to-end).
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_klog_level_before_begin;
static volatile uint32_t g_klog_level_after_begin;
static volatile uint32_t g_klog_level_after_drain;

static void test_harness_klog_suppress_begin(void)
{
    /* Pick a tag that is not used elsewhere in the test run so the
     * override-table lookup is deterministic. */
    const char *tag = "_test_suppress";

    g_klog_level_before_begin = (uint32_t)klog_get_level(tag);
    TEST_KLOG_SUPPRESS(tag);
    g_klog_level_after_begin = (uint32_t)klog_get_level(tag);

    TEST_ASSERT_EQ(g_klog_level_after_begin, (uint32_t)LOG_FATAL,
                   "klog_get_level returns LOG_FATAL after TEST_KLOG_SUPPRESS");
    TEST_ASSERT(g_klog_level_after_begin > g_klog_level_before_begin,
                "post-suppress level strictly higher than pre-suppress");
}

static void test_harness_klog_suppress_restored(void)
{
    const char *tag = "_test_suppress";
    g_klog_level_after_drain = (uint32_t)klog_get_level(tag);

    TEST_ASSERT_EQ(g_klog_level_after_drain, g_klog_level_before_begin,
                   "klog level restored to pre-suppress value after drain");
    TEST_ASSERT_EQ(klog_has_override(tag), 0,
                   "no lingering override: tag follows global default again "
                   "(would fail if suppress_begin blindly called klog_set_level on restore)");
}

/* Distinguish effective-level restore from true override-state restore:
 * the suppress on a tag with no pre-existing override must NOT create
 * a permanent override on restore. Otherwise a later klog_set_level("",
 * LOG_DEBUG) global change would not propagate to the tag. */
static volatile uint32_t g_klog_suppress_override_pre;
static volatile uint32_t g_klog_suppress_override_during;

static void test_harness_klog_suppress_no_override_leak(void)
{
    const char *tag = "_test_suppress_leak";
    g_klog_suppress_override_pre = (uint32_t)klog_has_override(tag);
    TEST_ASSERT_EQ(g_klog_suppress_override_pre, 0,
                   "test tag has no pre-existing override");

    TEST_KLOG_SUPPRESS(tag);
    g_klog_suppress_override_during = (uint32_t)klog_has_override(tag);
    TEST_ASSERT_EQ(g_klog_suppress_override_during, 1,
                   "TEST_KLOG_SUPPRESS creates a temporary override");
}

static void test_harness_klog_suppress_no_override_leak_verify(void)
{
    const char *tag = "_test_suppress_leak";
    TEST_ASSERT_EQ(klog_has_override(tag), 0,
                   "override removed on drain -- tag again follows global default");
}

/* ---------------------------------------------------------------------------
 * Per-test heap-leak detection.
 *
 * The detector snapshots heap_get_used() before the suite body and
 * re-reads it after the action drain. A non-zero delta absent
 * TEST_EXPECT_LEAK/IGNORE logs [LEAK] and increments the leaked
 * counter. Three tests:
 *   1. Deliberate kmalloc(64) without kfree + TEST_EXPECT_LEAK.
 *      The detector runs, observes the delta, writes it into the
 *      diagnostic getter, and classifies as [LEAK-OK] (no counter
 *      bump). Verify suite checks test_runner_last_leak_delta() > 0.
 *   2. TEST_SCRATCH_KBUF(buf, 256) -- drain frees, delta=0.
 *      Verify asserts last_leak_delta == 0.
 *   3. TEST_EXPECT_LEAK(64, ...) + kmalloc(64) unfreed. Same pattern
 *      as (1); proves the opt-out path is live.
 *
 * No test deliberately triggers a [LEAK] line in the summary, because
 * that would permanently polute L= in the boot-test report. The
 * diagnostic getter lets us prove the detection code ran without
 * using the classify-as-unexpected path. The [LEAK] branch is simple
 * enough (1 klog + 1 counter increment) that code review covers it.
 * ------------------------------------------------------------------------- */

/* g_leak_test_buf carries the leaked allocation across the
 * register/verify suite boundary so the verify suite can free it. The
 * delta values themselves are read-once-and-asserted-on, so they live
 * as locals in their respective verify functions. */
static uint8_t *g_leak_test_buf;
static uint8_t *g_leak_ignore_test_buf;

static void test_harness_leak_detect_kmalloc(void)
{
    /* Allocate but DO NOT free. TEST_EXPECT_LEAK prevents the
     * [LEAK] branch + counter increment, so the summary L stays
     * clean while still exercising the delta-computation path. */
    TEST_EXPECT_LEAK(64, "harness: kmalloc leak detection regression");
    g_leak_test_buf = (uint8_t *)kmalloc(64);
    TEST_ASSERT_NOT_NULL(g_leak_test_buf, "kmalloc(64) for leak test");
}

static void test_harness_leak_detect_kmalloc_verify(void)
{
    /* The previous suite ended with a leaked 64-byte allocation.
     * The detector observed it, wrote the delta into the diagnostic
     * getter, and classified as [LEAK-OK] per TEST_EXPECT_LEAK. */
    int64_t delta = test_runner_last_leak_delta();

    /* kmalloc adds a block-header so the observed delta is > 64;
     * we only assert it's strictly positive (proves detection
     * observed a real leak). */
    TEST_ASSERT(delta >= 64,
                "detector saw >= 64 bytes of heap growth from the unfreed kmalloc");

    /* Clean up the leak for subsequent suites. */
    if (g_leak_test_buf) {
        kfree(g_leak_test_buf);
        g_leak_test_buf = (void *)0;
    }
}

static void test_harness_leak_detect_scratch_clean(void)
{
    /* TEST_SCRATCH_KBUF auto-frees via action drain on suite exit.
     * The detector then observes zero delta -- no [LEAK] line, no
     * counter bump, last_leak_delta == 0. Proves the drain-path
     * cleanup is visible to the leak detector end-to-end. */
    TEST_SCRATCH_KBUF(buf, 256);
    (void)buf;
}

static void test_harness_leak_detect_scratch_verify(void)
{
    int64_t delta = test_runner_last_leak_delta();
    TEST_ASSERT_EQ(delta, 0,
                   "TEST_SCRATCH_KBUF + action drain -> zero heap delta");
}

/* TEST_LEAK_IGNORE regression -- prove the [LEAK-SKIP] branch fires
 * AND no counter bump occurs even with a positive heap delta. The
 * paired verify suite reads g_test_state.leaked before/after to
 * confirm the bypass actually held. Closes the M1 finding from the
 * quality review (TEST_LEAK_IGNORE was implemented but had no
 * caller exercising the bypass). */
static uint32_t g_leak_ignore_pre_count;

static void test_harness_leak_detect_ignore(void)
{
    g_leak_ignore_pre_count = g_test_state.leaked;
    TEST_LEAK_IGNORE("harness: bypass test for [LEAK-SKIP] branch");
    /* Force a real positive heap delta so the bypass path is the
     * thing keeping g_test_state.leaked unchanged. */
    g_leak_ignore_test_buf = (uint8_t *)kmalloc(64);
    TEST_ASSERT_NOT_NULL(g_leak_ignore_test_buf,
                         "kmalloc(64) for leak-ignore bypass test");
}

static void test_harness_leak_detect_ignore_verify(void)
{
    /* If TEST_LEAK_IGNORE worked, the leaked counter is unchanged
     * even though the previous suite had a >64-byte positive delta. */
    TEST_ASSERT_EQ(g_test_state.leaked, g_leak_ignore_pre_count,
                   "TEST_LEAK_IGNORE prevented [LEAK] counter bump despite real delta");

    /* Clean up the buffer for hygiene. */
    if (g_leak_ignore_test_buf) {
        kfree(g_leak_ignore_test_buf);
        g_leak_ignore_test_buf = (void *)0;
    }
}

/* ---------------------------------------------------------------------------
 * [COUNT] trace formatters.
 *
 * The records are how a moving headline assertion total gets attributed to a
 * SUITE, so their shape is a contract with the host-side comparison in
 * scripts/test-count-stability.sh, not a debug convenience. Both formatters
 * are pure, which is the whole reason they were split out of the runner loop:
 * the overflow branch is unreachable from a real run (the longest registered
 * suite name is 85 bytes against a 224-byte budget) and would otherwise never
 * be exercised until the day a long name made it load-bearing.
 * ------------------------------------------------------------------------ */

static void test_harness_count_record_format(void)
{
    char rec[TEST_COUNT_RECORD_MAX];

    TEST_ASSERT_EQ(test_count_record_format(rec, sizeof(rec), 1, "mm",
                                            "PMM: alloc", 12, 0, 3, 1), 1,
                   "record formats within budget");
    TEST_ASSERT_EQ(strcmp(rec, "[COUNT] #1 mm PMM: alloc p=12 f=0 s=3 P=1"), 0,
                   "record carries ordinal, category, suite and all four deltas");

    /* Zero is a real value, not an absent field: a suite that ran and asserted
     * nothing must be distinguishable from a suite that never emitted. */
    TEST_ASSERT_EQ(test_count_record_format(rec, sizeof(rec), 2703, "boot",
                                            "x", 0, 0, 0, 0), 1,
                   "all-zero record still formats");
    TEST_ASSERT_EQ(strcmp(rec, "[COUNT] #2703 boot x p=0 f=0 s=0 P=0"), 0,
                   "zero deltas render as 0 rather than being omitted");

    /* NULL category or suite must not fault -- the runner passes s->name
     * straight through and a registration bug should surface as a readable
     * record, not a page fault inside the test runner itself. */
    TEST_ASSERT_EQ(test_count_record_format(rec, sizeof(rec), 4, (void *)0,
                                            (void *)0, 1, 2, 3, 4), 1,
                   "NULL category and suite format without faulting");
    TEST_ASSERT_EQ(strcmp(rec, "[COUNT] #4 ? ? p=1 f=2 s=3 P=4"), 0,
                   "NULL fields render as ? placeholders");
}

static void test_harness_count_record_overflow(void)
{
    char     small[24];
    uint32_t len = 0;

    /* A record that does not fit returns 0 AND self-describes: the suite name
     * is the comparison KEY, so a quietly shortened one would let two distinct
     * suites compare equal and hide exactly the drift the trace exists to
     * expose. The consumer must be able to see that this key is unusable. */
    TEST_ASSERT_EQ(test_count_record_format(small, sizeof(small), 7, "sched",
                                            "a suite name far past the budget",
                                            1, 0, 0, 0), 0,
                   "over-budget record reports failure");
    while (small[len]) len++;
    TEST_ASSERT(len < sizeof(small),
                "over-budget record stays NUL-terminated inside the buffer");
    TEST_ASSERT_EQ(strcmp(small + (len - TEST_COUNT_TRUNC_MARK_LEN),
                          " trunc=1"), 0,
                   "over-budget record ends with the trunc=1 marker");

    /* Degenerate buffers are refused rather than written through. */
    TEST_ASSERT_EQ(test_count_record_format(small, 0, 1, "mm", "s", 0, 0, 0, 0), 0,
                   "zero-capacity buffer is refused");
    TEST_ASSERT_EQ(test_count_record_format((void *)0, sizeof(small), 1, "mm",
                                            "s", 0, 0, 0, 0), 0,
                   "NULL destination is refused");
}

static void test_harness_count_trailer_format(void)
{
    char trailer[TEST_COUNT_RECORD_MAX];
    char small[8];

    TEST_ASSERT_EQ(test_count_trailer_format(trailer, sizeof(trailer),
                                             2703, 28109, 0, 41, 7), 1,
                   "trailer formats within budget");
    TEST_ASSERT_EQ(strcmp(trailer,
                          "[COUNT-END] records=2703 p=28109 f=0 s=41 P=7"), 0,
                   "trailer announces the record count and the run totals");

    /* `records` is what makes the trace completeness-checkable: a consumer
     * that counted a different number is reading truncated input. It must
     * therefore survive as its own field even when everything else is zero. */
    TEST_ASSERT_EQ(test_count_trailer_format(trailer, sizeof(trailer),
                                             0, 0, 0, 0, 0), 1,
                   "empty-run trailer formats");
    TEST_ASSERT_EQ(strcmp(trailer, "[COUNT-END] records=0 p=0 f=0 s=0 P=0"), 0,
                   "empty run still announces records=0 rather than nothing");

    TEST_ASSERT_EQ(test_count_trailer_format(small, sizeof(small),
                                             1, 1, 0, 0, 0), 0,
                   "over-budget trailer reports failure");
}

static void test_harness_count_name_is_safe(void)
{
    TEST_ASSERT_EQ(test_count_name_is_safe("Harness: register 3 actions"), 1,
                   "an ordinary name with spaces and a colon is a usable key");
    TEST_ASSERT_EQ(test_count_name_is_safe(""), 1, "an empty name is usable");
    TEST_ASSERT_EQ(test_count_name_is_safe((void *)0), 0, "NULL is refused");

    /* The reserved token, which every consumer reads as "truncated record". */
    TEST_ASSERT_EQ(test_count_name_is_safe("Harness: trunc=1 behavior"), 0,
                   "' trunc=1' is refused -- a name carrying it would make "
                   "every sweep refuse its own trace");
    TEST_ASSERT_EQ(test_count_name_is_safe("ends with trunc=1"), 0,
                   "the reserved token is refused at the very end too");

    /* The shape no source grep can see: C concatenates these into ONE string,
     * so the resolved name carries the token even though neither literal does.
     * This case is the entire reason the check lives at registration. */
    TEST_ASSERT_EQ(test_count_name_is_safe("safe" " trunc=1"), 0,
                   "adjacent string literals concatenating into the reserved "
                   "token are refused, which no source-level grep can see");

    /* The four FIELD delimiters are explicitly NOT banned. The consumer parses
     * the numeric fields right-anchored, so a name containing them is
     * unambiguous -- verified against the host regex, which resolves even
     * `evil p=9 f=9 s=9 P=9 tail` to the correct name and numbers. Rejecting
     * these would LOG_ERROR on names that work, and a validator that cries
     * wolf gets switched off. */
    TEST_ASSERT_EQ(test_count_name_is_safe("suite with p=5 inside"), 1,
                   "a field delimiter in the name is fine: the parse is "
                   "right-anchored");
    TEST_ASSERT_EQ(test_count_name_is_safe("evil p=9 f=9 s=9 P=9 tail"), 1,
                   "even a complete fake field tail is fine, because only the "
                   "trailing group can satisfy the parser's anchor");

    /* Near-misses on the reserved token stay usable. */
    TEST_ASSERT_EQ(test_count_name_is_safe("trunc=1"), 1,
                   "the token without its leading space is not the token");
    TEST_ASSERT_EQ(test_count_name_is_safe("about trunc=2"), 1,
                   "a different value is not the reserved token");
    TEST_ASSERT_EQ(test_count_name_is_safe("about trunc="), 1,
                   "a prefix of the reserved token is not the token");
}

/* ---------------------------------------------------------------------------
 * Failing-assertion record formatter.
 *
 * Pure, for the same reason the [COUNT] formatters are: the truncation branch
 * is the one that matters and it is unreachable from an ordinary run, because
 * an ordinary run has no failing assertions at all. It is also the branch that
 * used to wedge the boot, so it gets exercised with synthetic inputs on every
 * green run rather than only on the day something fails loudly.
 * ------------------------------------------------------------------------ */

static uint32_t harness_str_len(const char *s)
{
    uint32_t n = 0;

    while (s[n]) n++;
    return n;
}

/* 1 when `hay` ends with `needle`. */
static int harness_ends_with(const char *hay, const char *needle)
{
    uint32_t h = harness_str_len(hay);
    uint32_t n = harness_str_len(needle);

    if (n > h) return 0;
    return strcmp(hay + (h - n), needle) == 0;
}

static void test_harness_fail_record_format(void)
{
    char rec[TEST_FAIL_RECORD_MAX];

    TEST_ASSERT_EQ(test_fail_record_format(rec, sizeof(rec), "MM: alloc",
                                           "frame count matches", (void *)0,
                                           "src/kernel/mm/pmm.c", 412), 1,
                   "a record within budget formats");
    TEST_ASSERT_EQ(strcmp(rec, "MM: alloc :: frame count matches  "
                               "(src/kernel/mm/pmm.c:412)"), 0,
                   "the record carries suite, message and the file:line suffix");

    /* The diagnostic field sits between the message and the suffix, so the
     * location stays last no matter what the emitter supplies. */
    TEST_ASSERT_EQ(test_fail_record_format(rec, sizeof(rec), "S", "m",
                                           "  (got 5, expected 6)", "f.c", 1), 1,
                   "a record with a diagnostic field formats");
    TEST_ASSERT_EQ(strcmp(rec, "S :: m  (got 5, expected 6)  (f.c:1)"), 0,
                   "the diagnostic precedes the suffix");

    /* NULL fields must not fault: a registration bug should surface as a
     * readable record, not a page fault inside the failure reporter. */
    TEST_ASSERT_EQ(test_fail_record_format(rec, sizeof(rec), (void *)0,
                                           (void *)0, (void *)0, (void *)0, 7), 1,
                   "NULL suite, message and file format without faulting");
    TEST_ASSERT_EQ(strcmp(rec, "? :: ?  (:7)"), 0,
                   "NULL fields render as ? placeholders and keep the line");

    TEST_ASSERT_EQ(test_fail_record_format((void *)0, sizeof(rec), "s", "m",
                                           (void *)0, "f", 1), 0,
                   "NULL destination is refused");
    TEST_ASSERT_EQ(test_fail_record_format(rec, 0, "s", "m", (void *)0, "f", 1), 0,
                   "zero-capacity buffer is refused");
}

static void test_harness_fail_record_overflow(void)
{
    /* 300 characters: past the 255 a klog entry can hold, which is the exact
     * case that used to wedge the boot. */
    static char big[301];
    char        rec[TEST_FAIL_RECORD_MAX];
    uint32_t    i, len;

    for (i = 0; i < 300u; i++)
        big[i] = (char)('a' + (i % 26u));
    big[300] = '\0';

    TEST_ASSERT_EQ(test_fail_record_format(rec, sizeof(rec), "Suite", big,
                                           (void *)0, "src/kernel/test/x.c",
                                           4242), 0,
                   "an over-budget record reports failure");

    len = harness_str_len(rec);
    TEST_ASSERT(len < sizeof(rec),
                "the over-budget record stays NUL-terminated inside the buffer");
    TEST_ASSERT_EQ((uint64_t)len, (uint64_t)(sizeof(rec) - 1u),
                   "the record spends its whole budget rather than stopping "
                   "short");
    TEST_ASSERT(harness_ends_with(rec, "  (src/kernel/test/x.c:4242)"),
                "the file:line suffix survives truncation -- it is what makes "
                "a failure locatable, so it is reserved before author text");
    TEST_ASSERT(harness_ends_with(rec, " trunc=1  (src/kernel/test/x.c:4242)"),
                "the cut record self-describes with the same marker the "
                "[COUNT] trace uses");
    TEST_ASSERT_EQ(strncmp(rec, "Suite :: ", 9), 0,
                   "the suite name and separator survive truncation");

    /* One byte past the exact fit must truncate, and the exact fit must not --
     * an off-by-one here would either lose a whole message or mark a record
     * that was never cut. */
    {
        static char exact[TEST_FAIL_RECORD_MAX];
        /* "S :: " (5) + msg + "  (f.c:1)" (9) == 255 usable -> msg == 241 */
        for (i = 0; i < 241u; i++)
            exact[i] = 'x';
        exact[241] = '\0';
        TEST_ASSERT_EQ(test_fail_record_format(rec, sizeof(rec), "S", exact,
                                               (void *)0, "f.c", 1), 1,
                       "a record that exactly fills the budget is not marked "
                       "truncated");
        TEST_ASSERT_EQ((uint64_t)harness_str_len(rec),
                       (uint64_t)(sizeof(rec) - 1u),
                       "the exact-fit record is exactly 255 characters");

        exact[241] = 'x';
        exact[242] = '\0';
        TEST_ASSERT_EQ(test_fail_record_format(rec, sizeof(rec), "S", exact,
                                               (void *)0, "f.c", 1), 0,
                       "one byte past the exact fit truncates");
        TEST_ASSERT(harness_ends_with(rec, " trunc=1  (f.c:1)"),
                    "the one-byte overflow is marked, not silently cut");
    }
}

static void test_harness_fail_record_suffix_ladder(void)
{
    char small[48];

    /* A path too long for the remaining budget sheds directory components
     * rather than surrendering the line number, which is the field that
     * actually locates the failure. */
    TEST_ASSERT_EQ(test_fail_record_format(small, sizeof(small), "S", "m",
                                           (void *)0,
                                           "src/kernel/subsystem/very/deep/"
                                           "path/module_name.c", 900), 0,
                   "a record whose path exceeds the budget reports failure");
    TEST_ASSERT(harness_ends_with(small, "  (module_name.c:900)"),
                "the suffix degrades to the basename rather than being cut");
    TEST_ASSERT(harness_str_len(small) < sizeof(small),
                "the degraded record stays inside its buffer");

    /* Backslash separators must shed the same way. `__FILE__` carries whatever
     * the build used, and if this branch regressed the full path and the
     * "basename" would be the same string -- so the ladder would skip straight
     * to the line-number-only form and drop the filename exactly when a
     * diagnostic needs it. */
    {
        char win[48];

        TEST_ASSERT_EQ(test_fail_record_format(win, sizeof(win), "S", "m",
                                               (void *)0,
                                               "src\\kernel\\subsystem\\very\\"
                                               "deep\\path\\module_name.c",
                                               900), 0,
                       "a backslash path over the budget reports failure");
        TEST_ASSERT(harness_ends_with(win, "  (module_name.c:900)"),
                    "a backslash path sheds components to its basename too");
    }

    /* The text-exhausted branch: the budget leaves NO room for any author
     * text, so the record degrades to marker plus location. It is the branch
     * most likely to be written casually -- it was, and shipped a bare
     * "trunc=1" without the token's leading space, which the consumer that
     * the token exists for would not have matched. */
    {
        char tiny[22];

        TEST_ASSERT_EQ(test_fail_record_format(tiny, sizeof(tiny), "SuiteName",
                                               "a message", (void *)0,
                                               "f.c", 7), 0,
                       "a record with no room for author text reports failure");
        TEST_ASSERT_EQ(strncmp(tiny, TEST_TRUNC_MARK, TEST_COUNT_TRUNC_MARK_LEN),
                       0,
                       "the text-exhausted record leads with the FULL shared "
                       "marker, leading space included");
        TEST_ASSERT(harness_ends_with(tiny, "(f.c:7)"),
                    "the text-exhausted record still carries its location");
        TEST_ASSERT(harness_str_len(tiny) < sizeof(tiny),
                    "the text-exhausted record stays inside its buffer");
    }

    /* When even the basename cannot fit, the line number alone survives. */
    {
        char tiny[24];

        TEST_ASSERT_EQ(test_fail_record_format(tiny, sizeof(tiny), "SuiteName",
                                               "some message", (void *)0,
                                               "a_rather_long_file_name.c",
                                               12345), 0,
                       "a record with no room for any path reports failure");
        TEST_ASSERT(harness_ends_with(tiny, "(:12345)"),
                    "the line number is the last field to go");
        TEST_ASSERT(harness_str_len(tiny) < sizeof(tiny),
                    "the minimal record stays inside its buffer");
    }
}

static void test_harness_fail_detail_format(void)
{
    char det[64];
    char small[8];

    TEST_ASSERT_EQ(test_fail_detail_format(det, sizeof(det), "got ", 12,
                                           ", expected ", 34), 1,
                   "a two-value diagnostic formats");
    TEST_ASSERT_EQ(strcmp(det, "  (got 12, expected 34)"), 0,
                   "both values render with their labels");

    TEST_ASSERT_EQ(test_fail_detail_format(det, sizeof(det), "both are ", 0,
                                           (void *)0, 0), 1,
                   "a one-value diagnostic omits the second value");
    TEST_ASSERT_EQ(strcmp(det, "  (both are 0)"), 0,
                   "zero is a real value, not an absent field");

    /* 64-bit values must survive whole: the assertion helpers take uint64_t
     * and a narrowed diagnostic would misreport the very mismatch it is
     * describing. */
    TEST_ASSERT_EQ(test_fail_detail_format(det, sizeof(det), "got ",
                                           18446744073709551615ULL,
                                           (void *)0, 0), 1,
                   "a full-width 64-bit value formats");
    TEST_ASSERT_EQ(strcmp(det, "  (got 18446744073709551615)"), 0,
                   "the 64-bit value is not narrowed");

    TEST_ASSERT_EQ(test_fail_detail_format(small, sizeof(small), "got ", 12,
                                           ", expected ", 34), 0,
                   "a diagnostic that does not fit reports failure");
    TEST_ASSERT_EQ(strcmp(small, ""), 0,
                   "a diagnostic that did not fit is left EMPTY -- a half "
                   "written one reads as a real one");

    TEST_ASSERT_EQ(test_fail_detail_format((void *)0, sizeof(det), "g", 1,
                                           (void *)0, 0), 0,
                   "NULL destination is refused");
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
    test_suite_register_cat("Harness: TEST_KLOG_SUPPRESS demotes level",
                            test_harness_klog_suppress_begin, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: klog level restored after action drain",
                            test_harness_klog_suppress_restored, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: TEST_KLOG_SUPPRESS no pre-existing override -> creates temp",
                            test_harness_klog_suppress_no_override_leak, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: klog override removed on drain when none pre-existed",
                            test_harness_klog_suppress_no_override_leak_verify, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: leak detector observes unfreed kmalloc",
                            test_harness_leak_detect_kmalloc, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: leak detector reported > 64 bytes for kmalloc route",
                            test_harness_leak_detect_kmalloc_verify, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: TEST_SCRATCH_KBUF -> zero heap delta after drain",
                            test_harness_leak_detect_scratch_clean, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: scratch drain leaves last_leak_delta == 0",
                            test_harness_leak_detect_scratch_verify, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: TEST_LEAK_IGNORE bypasses delta check",
                            test_harness_leak_detect_ignore, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: leaked counter unchanged after TEST_LEAK_IGNORE",
                            test_harness_leak_detect_ignore_verify, TEST_CAT_BOOT);

    /* [COUNT] trace formatters -- order-independent (pure functions). */
    test_suite_register_cat("Harness: count-trace record format",
                            test_harness_count_record_format, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: count-trace record overflow is marked, not silent",
                            test_harness_count_record_overflow, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: count-trace trailer format",
                            test_harness_count_trailer_format, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: count-trace suite-name key validation",
                            test_harness_count_name_is_safe, TEST_CAT_BOOT);

    /* Failing-assertion record formatters -- order-independent (pure). */
    test_suite_register_cat("Harness: failure-record format",
                            test_harness_fail_record_format, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: failure-record overflow keeps the file:line suffix",
                            test_harness_fail_record_overflow, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: failure-record suffix degrades by shedding path",
                            test_harness_fail_record_suffix_ladder, TEST_CAT_BOOT);
    test_suite_register_cat("Harness: failure-record diagnostic field format",
                            test_harness_fail_detail_format, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
