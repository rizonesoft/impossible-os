/* ============================================================================
 * test_pm_idle.c -- C1 idle entry + per-CPU idle accounting unit tests
 *
 * Covers the race-safe entry contract of pm_idle_c1(): it halts only when it
 * has re-checked readiness with interrupts masked, it refuses to halt for a
 * caller who arrived with interrupts already disabled, and it accounts the
 * halted cycles to the calling CPU.
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md section 2
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/pm.h"
#include "kernel/smp.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/dpc.h"


/* Interrupts must be ON for the halting path to be reachable at all; a test
 * running with them masked would exercise the refusal branch instead and its
 * assertion would be about the wrong thing. */

/* Proving the HLT is actually emitted needs a DETERMINISTIC oracle, and three
 * plausible ones were tried and rejected before this one:
 *
 *   - elapsed TSC cycles: two increasing RDTSC reads produce a delta whether
 *     or not the CPU halted, so the assertion passes with the HLT deleted;
 *   - per_cpu_data.irq_count: that field is DEAD -- zeroed in four places in
 *     src/kernel/smp/smp.c and incremented nowhere -- so it reads a constant
 *     and its assertion can never fire (filed in todo/02-kernel-core/TODO-26-power-management.md section 25);
 *   - an advancing tick counter: non-causal in BOTH directions. A device IRQ
 *     can wake a correctly halted CPU before the next tick (false failure),
 *     and with the HLT deleted a tick landing between the samples passes;
 *   - a byte SEARCH over the function: pm_idle_c1 is ~0x97 bytes, so a
 *     512-byte window runs into neighbouring functions and can match their
 *     bytes after the real halt is removed.
 *
 * So the halt site names itself. pm_idle_c1() emits a global label
 * immediately before its `sti; hlt`, and this checks the two bytes AT that
 * address: 0xFB 0xF4. No window, no instruction-boundary guessing, and if the
 * halt is ever deleted the label disappears and this file fails to LINK. */
#define PM_OP_STI 0xFBu
#define PM_OP_HLT 0xF4u

extern const uint8_t pm_idle_hlt_site[];

/* ---- readiness predicate ---- */

static void test_pm_deep_idle_allowed_default(void)
{
    /* This CANNOT assert `allowed == 1`. The DPC queue is live and shared:
     * dpc_insert_core() lets any CPU target it, and local_irq_save() excludes
     * local delivery only, so a pre-existing or remote entry makes a REFUSAL
     * the correct answer and a fixed expectation would flake in the ship gate.
     *
     * So assert the CONTRACT against a SINGLE sample: with policy enabled, the
     * predicate may refuse only when the queue is non-empty. Two separate
     * observations would NOT do, and an earlier version of this test made that
     * mistake -- a cross-CPU KeRemoveQueueDpc between them (dpc.c:720-758)
     * gives allowed == 0 with depth == 0, a contradiction that is nobody's bug
     * and a false ship-gate failure. The only failing direction here is
     * refused-with-an-empty-queue, which is a genuine defect and nothing
     * else. */
    uint32_t depth = 0;
    int prev, allowed;

    prev = pm_idle_set_enable_test(1);

    /* ONE sample: the verdict and the depth it was computed from. */
    allowed = pm_deep_idle_probe_test(&depth);

    pm_idle_set_enable_test(prev > 0 ? 1 : (prev == 0 ? 0 : 1));

    TEST_ASSERT(allowed == 1 || depth != 0,
                "with policy enabled, deep idle is refused only for a queued DPC");
}

static void test_pm_deep_idle_allowed_policy_off(void)
{
    uint64_t f;
    int prev, allowed;

    prev = pm_idle_set_enable_test(0);
    f = local_irq_save();
    allowed = pm_deep_idle_allowed();
    local_irq_restore(f);
    pm_idle_set_enable_test(prev > 0 ? 1 : (prev == 0 ? 0 : 1));
    TEST_ASSERT_EQ(allowed, 0,
                   "PowerIdleEnable=0 refuses deep idle");
}

/* The pending-DPC branch of the predicate was untested through eight review
 * rounds, which is why it is here: it is the branch that decides what section
 * 16's governor will eventually be told, and it had no coverage at all. */
static void pm_test_dpc_noop(struct _KDPC *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
}

static KDPC s_pm_test_dpc;

static void test_pm_deep_idle_allowed_pending_dpc(void)
{
    uint64_t f;
    int prev, allowed_with_dpc;

    prev = pm_idle_set_enable_test(1);   /* isolate the DPC term from policy */

    KeInitializeDpc(&s_pm_test_dpc, pm_test_dpc_noop, (void *)0);

    f = local_irq_save();
    if (!KeInsertQueueDpc(&s_pm_test_dpc, (void *)0, (void *)0)) {
        local_irq_restore(f);
        pm_idle_set_enable_test(prev > 0 ? 1 : (prev == 0 ? 0 : 1));
        TEST_SKIP("DPC queue refused the insert -- branch not reachable here");
        return;
    }
    /* Sampled with the DPC still queued and interrupts masked, so nothing can
     * drain it between the insert and the read. */
    allowed_with_dpc = pm_deep_idle_allowed();
    local_irq_restore(f);

    /* Let the queued no-op drain normally; it is a static object with a static
     * routine, so it stays valid however late it runs. */
    pm_idle_set_enable_test(prev > 0 ? 1 : (prev == 0 ? 0 : 1));

    TEST_ASSERT_EQ(allowed_with_dpc, 0,
                   "a queued DPC makes deep idle not-allowed, with policy on");
}

/* ---- entry contract: the predicate must never become a spin ---- */

/* The regression this test exists to prevent: an idle AP has no DPC drain
 * trigger (`src/kernel/sched/ktimer.c:248-251` -- masked LAPIC timer, no DPC
 * IPI), so if a false predicate could refuse the halt, a stranded DPC or a
 * disabled policy would turn the AP park loop into a 100% busy-spin forever. */
static void test_pm_idle_c1_halts_even_when_not_ready(void)
{
    int prev, halted_ready;
    uint32_t me;
    uint64_t before, after, hits_before, hits_after;

    if (!irqs_enabled()) {
        TEST_SKIP("interrupts masked in this context -- halting path unreachable");
        return;
    }

    me     = smp_cpu_id();
    prev   = pm_idle_set_enable_test(0);      /* predicate forced false */
    before = pm_idle_cycles(me);
    hits_before = pm_idle_halt_hits_test();
    halted_ready = pm_idle_c1();
    hits_after = pm_idle_halt_hits_test();
    after  = pm_idle_cycles(me);
    pm_idle_set_enable_test(prev > 0 ? 1 : (prev == 0 ? 0 : 1));

    TEST_ASSERT_EQ(halted_ready, 0,
                   "a not-ready CPU reports the predicate as 0");
    /* THE never-spin assertion. Accounting alone would pass even if the halt
     * were skipped for ready == 0; only the site counter proves it was run. */
    TEST_ASSERT_EQ(hits_after, hits_before + 1ull,
                   "a not-ready CPU REACHED the halt site -- it must never spin");
    TEST_ASSERT(after > before,
                "the halt was accounted to this CPU");
    TEST_ASSERT(irqs_enabled(),
                "the caller's interrupt flag is restored");
}

static void test_pm_idle_c1_refuses_irqs_disabled(void)
{
    uint64_t flags, hits_before, hits_after;
    int halted, if_left_clear;

    hits_before = pm_idle_halt_hits_test();
    flags = local_irq_save();
    halted = pm_idle_c1();
    /* Sampled BEFORE the restore, which is the whole point: checking IF after
     * local_irq_restore() would only prove the restore works, and would pass
     * even if pm_idle_c1() had wrongly executed `sti` and run an ISR inside
     * this masked region. */
    if_left_clear = !irqs_enabled();
    local_irq_restore(flags);
    hits_after = pm_idle_halt_hits_test();

    TEST_ASSERT_EQ(hits_after, hits_before,
                   "a masked caller never reached the halt site -- no ISR ran "
                   "inside its critical region");
    TEST_ASSERT_EQ(halted, 0,
                   "pm_idle_c1 refuses to halt a caller that masked interrupts");
    TEST_ASSERT(if_left_clear,
                "pm_idle_c1 left interrupts masked for a masked caller");
}

/* ---- the halting path ---- */

static void test_pm_idle_c1_accounts_cycles(void)
{
    uint32_t me;
    uint64_t before, after, hits_before, hits_after;
    int ready, prev;

    if (!irqs_enabled()) {
        TEST_SKIP("interrupts masked in this context -- halting path unreachable");
        return;
    }

    me     = smp_cpu_id();
    prev   = pm_idle_set_enable_test(1);
    before = pm_idle_cycles(me);
    hits_before = pm_idle_halt_hits_test();
    ready  = pm_idle_c1();          /* returns on the next interrupt */
    hits_after = pm_idle_halt_hits_test();
    after  = pm_idle_cycles(me);
    pm_idle_set_enable_test(prev > 0 ? 1 : (prev == 0 ? 0 : 1));

    /* NOT `ready == 1`: the DPC queue is live, so a DPC landing in the window
     * makes 0 the correct answer and the assertion would be about timing. The
     * deterministic claim is that a halt happened and was accounted. */
    TEST_ASSERT(ready == 0 || ready == 1,
                "the reported predicate is a valid boolean");
    /* The ORDINARY path needs the same proof as the not-ready one: accounting
     * alone would pass with the halt bypassed here, leaving an idle CPU
     * spinning on exactly the path a machine spends its life in. */
    TEST_ASSERT_EQ(hits_after, hits_before + 1ull,
                   "the ready path REACHED the halt site exactly once");
    TEST_ASSERT(after > before,
                "idle_tsc_cycles advanced across the halt");
    TEST_ASSERT(irqs_enabled(),
                "the caller's interrupt flag is restored after the halt");
}

/* ---- the halt itself ---- */

static void test_pm_idle_c1_emits_sti_hlt(void)
{
    /* The never-spin property is only worth anything if the halt is genuinely
     * emitted, and no runtime assertion can see that: the return value, IF
     * handling and accounting all behave identically with the HLT deleted. */
    TEST_ASSERT_EQ((uint64_t)pm_idle_hlt_site[0], (uint64_t)PM_OP_STI,
                   "the labelled halt site begins with STI");
    TEST_ASSERT_EQ((uint64_t)pm_idle_hlt_site[1], (uint64_t)PM_OP_HLT,
                   "STI is immediately followed by HLT -- the shadow pair");
}

/* ---- the backwards-TSC guard ---- */

static void test_pm_idle_delta_rejects_backwards_tsc(void)
{
    /* The failure this guard exists to stop: an unsigned wrap turns ONE
     * backwards sample into a ~2^64 increment that never washes out, whereas
     * rejecting it costs a single idle episode. */
    TEST_ASSERT_EQ(pm_idle_delta(1000ull, 900ull), 0ull,
                   "a backwards TSC sample contributes nothing");
    TEST_ASSERT_EQ(pm_idle_delta(1000ull, 1000ull), 0ull,
                   "an unchanged TSC contributes nothing");
    TEST_ASSERT_EQ(pm_idle_delta(1000ull, 1500ull), 500ull,
                   "a normal interval contributes its exact delta");
    TEST_ASSERT_EQ(pm_idle_delta(0xFFFFFFFFFFFFFFFFull, 0ull), 0ull,
                   "a TSC wrap is rejected, not accumulated as ~2^64");
}

/* ---- accounting accessor ---- */

static void test_pm_idle_cycles_offline_slot_zero(void)
{
    /* MAX_CPUS is a slot bound, and a slot that is not online has no owning
     * CPU to have written a counter -- it must read 0 rather than stale data
     * or an out-of-bounds access. */
    TEST_ASSERT_EQ(pm_idle_cycles(MAX_CPUS), 0ull,
                   "an out-of-range CPU slot reports zero idle cycles");
}

/* ---- Registration ---- */

/* ---- MWAIT capability layer (section 10) --------------------------------
 *
 * These are pure functions over supplied CPUID words, so every case below is
 * deterministic on any host -- including this one, whose emulated CPU reports
 * no leaf 5 at all. That is the reason the layer takes arguments instead of
 * executing CPUID: the platforms the logic exists for are not the platforms
 * the suite runs on. */

static void test_pm_mwait_hint_encode_sdm(void)
{
    /* SDM Table 4-11: bits 7:4 are (class - 1), so C1 encodes as 0. */
    TEST_ASSERT_EQ(pm_mwait_hint_encode(1, 0), 0x00u, "C1s0");
    TEST_ASSERT_EQ(pm_mwait_hint_encode(2, 0), 0x10u, "C2s0");
    TEST_ASSERT_EQ(pm_mwait_hint_encode(7, 0), 0x60u, "C7s0");
    TEST_ASSERT_EQ(pm_mwait_hint_encode(7, 15), 0x6Fu, "C7s15");
    TEST_ASSERT_EQ(pm_mwait_hint_encode(3, 2), 0x22u, "C3s2");
}

static void test_pm_mwait_hint_encode_rejects_out_of_range(void)
{
    /* Class 0 is C0, the running state, and is not an idle target. Class 8
     * is past what leaf 5 can enumerate. Both must refuse rather than wrap
     * into a neighbouring class's encoding. */
    TEST_ASSERT_EQ(pm_mwait_hint_encode(0, 0), PM_MWAIT_HINT_INVALID,
                   "cls0");
    TEST_ASSERT_EQ(pm_mwait_hint_encode(8, 0), PM_MWAIT_HINT_INVALID,
                   "cls8");
    TEST_ASSERT_EQ(pm_mwait_hint_encode(1, 16), PM_MWAIT_HINT_INVALID,
                   "sub16");
}

static void test_pm_mwait_deepest_picks_deepest_class(void)
{
    uint32_t hint = 0xDEADu;

    /* One sub-state in C1 (bits 7:4) and two in C3 (bits 15:12). C3 is
     * deeper, and its deepest sub-state is count-1 = 1, so 0x21. */
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0x2010u, &hint), 1, "ok");
    TEST_ASSERT_EQ(hint, 0x21u, "C3s1 wins");

    /* C1 alone must still WIN, which is the loop's last iteration. Without
     * this, tightening the bound to cclass > PM_MWAIT_CLASS_MIN would pass
     * every other case while rejecting a CPU that implements only C1.
     * Note the hint is a legitimate 0x00: success is the return value, and a
     * caller treating a zero hint as "none" would be wrong. */
    hint = 0xDEADu;
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0x10u, &hint), 1, "C1 ok");
    TEST_ASSERT_EQ(hint, 0x00u, "C1s0 zero");
}

static void test_pm_mwait_deepest_skips_empty_classes(void)
{
    uint32_t hint = 0xDEADu;

    /* C7 nibble (bits 31:28) is zero, so C7 is unimplemented and must be
     * skipped rather than encoded as C7 sub0. C2 (bits 11:8) has one. */
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0x0100u, &hint), 1, "ok");
    TEST_ASSERT_EQ(hint, 0x10u, "C2s0");
}

static void test_pm_mwait_deepest_ignores_c0(void)
{
    uint32_t hint = 0xDEADu;

    /* Only the C0 nibble is set. C0 is the running state, never an idle
     * target, so this must refuse -- not encode class 0. */
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0x000Fu, &hint), 0, "C0 no");
    TEST_ASSERT_EQ(hint, 0xDEADu, "untouched");
}

static void test_pm_mwait_deepest_refuses_all_zero(void)
{
    uint32_t hint = 0xDEADu;

    /* What a CPU with no MWAIT idle classes reports, and what this emulated
     * host reports. A fabricated hint here would name a state the CPU does
     * not implement, which is how a monitored wait becomes unbounded. */
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0u, &hint), 0, "zero no");
    TEST_ASSERT_EQ(hint, 0xDEADu, "untouched");

    /* The NULL-output probe deliberately supplies an EDX that DOES name a
     * class (C7, count 1). An all-zero EDX would return before reaching the
     * store, so it would pass even with the NULL guard deleted -- proving
     * nothing. This input reaches the store, so only the guard stops a
     * kernel NULL write. */
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0x10000000u, 0), 0, "null no");
}

static void test_pm_mwait_deepest_substate_within_count(void)
{
    uint32_t hint = 0xDEADu;

    /* Max a 4-bit count field can report is 15 sub-states, whose deepest
     * INDEX is 14 -- not 15. Requesting index 15 off a count of 15 would name
     * a sub-state the class does not implement, so this asserts the count-1
     * conversion rather than the field width. */
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0xF0000000u, &hint), 1, "ok");
    TEST_ASSERT_EQ(hint, 0x6Eu, "C7 n15");

    /* A count of 1 is the boundary in the other direction: index 0. */
    TEST_ASSERT_EQ(pm_mwait_deepest_hint(0x10000000u, &hint), 1, "ok");
    TEST_ASSERT_EQ(hint, 0x60u, "C7 n1");
}

static void test_pm_mwait_idle_allowed_rules(void)
{
    /* Interrupts on: an arriving interrupt always breaks the wait. */
    TEST_ASSERT_EQ(pm_mwait_idle_allowed(1, 0), 1, "if1");
    TEST_ASSERT_EQ(pm_mwait_idle_allowed(1, 1), 1, "if1 brk");
    /* Interrupts masked: only a masked-interrupt break event can wake the
     * CPU, so without CPUID.05H:ECX[1] this must refuse outright. */
    TEST_ASSERT_EQ(pm_mwait_idle_allowed(0, 0), 0, "if0 no");
    TEST_ASSERT_EQ(pm_mwait_idle_allowed(0, 1), 1, "if0 brk");
}


void test_register_pm_idle(void)
{
    test_suite_register_cat("PM: deep idle allowed by default",
                            test_pm_deep_idle_allowed_default, TEST_CAT_BOOT);
    test_suite_register_cat("PM: PowerIdleEnable=0 refuses deep idle",
                            test_pm_deep_idle_allowed_policy_off, TEST_CAT_BOOT);
    test_suite_register_cat("PM: C1 halts even when not ready (never spins)",
                            test_pm_idle_c1_halts_even_when_not_ready, TEST_CAT_BOOT);
    test_suite_register_cat("PM: queued DPC blocks the readiness predicate",
                            test_pm_deep_idle_allowed_pending_dpc, TEST_CAT_BOOT);
    test_suite_register_cat("PM: C1 entry refuses irqs-disabled caller",
                            test_pm_idle_c1_refuses_irqs_disabled, TEST_CAT_BOOT);
    test_suite_register_cat("PM: C1 halt accounts idle cycles",
                            test_pm_idle_c1_accounts_cycles, TEST_CAT_BOOT);
    test_suite_register_cat("PM: C1 entry really emits STI;HLT",
                            test_pm_idle_c1_emits_sti_hlt, TEST_CAT_BOOT);
    test_suite_register_cat("PM: backwards TSC sample is rejected",
                            test_pm_idle_delta_rejects_backwards_tsc, TEST_CAT_BOOT);
    test_suite_register_cat("PM: offline CPU slot reports zero idle cycles",
                            test_pm_idle_cycles_offline_slot_zero, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT hint encoding",
                            test_pm_mwait_hint_encode_sdm, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT hint range",
                            test_pm_mwait_hint_encode_rejects_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT deepest class",
                            test_pm_mwait_deepest_picks_deepest_class, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT skips empty",
                            test_pm_mwait_deepest_skips_empty_classes, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT skips C0",
                            test_pm_mwait_deepest_ignores_c0, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT no class",
                            test_pm_mwait_deepest_refuses_all_zero, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT substate bound",
                            test_pm_mwait_deepest_substate_within_count, TEST_CAT_BOOT);
    test_suite_register_cat("PM: MWAIT IF=0 rule",
                            test_pm_mwait_idle_allowed_rules, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
