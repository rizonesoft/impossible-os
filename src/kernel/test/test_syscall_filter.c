/* ============================================================================
 * test_syscall_filter.c -- Per-process SSDT syscall filter unit tests
 *
 * Exercises the pure filter helpers in syscall_filter.c (set-policy build,
 * LOCKED tighten-only enforcement, clone isolation, retire-chain teardown,
 * generation cap, and the bitmap allow/block decision) against a static task
 * fixture -- no live boot calls, no dispatch path.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/syscall_filter.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/task.h"
#include "kernel/mm/heap.h"

/* Static task fixture: only the syscall_filter field + the global count are
 * touched by the code under test, so a zero-init static task is sufficient. */
static struct task s_task;

static void sf_reset(void)
{
    /* Drop any filter chain from a prior test and restore the global count. */
    syscall_filter_task_teardown(&s_task);
    s_task.syscall_filter = (struct syscall_filter *)0;
}

static void policy_init(PROCESS_SYSCALL_FILTER_POLICY *p, uint32_t op, uint32_t flags)
{
    uint32_t i;
    p->operation = op;
    p->flags = flags;
    for (i = 0; i < SYSCALL_FILTER_MAIN_WORDS; i++)
        p->allow_main[i] = ~0ULL;
    for (i = 0; i < SYSCALL_FILTER_SHADOW_WORDS; i++)
        p->allow_shadow[i] = ~0ULL;
}

/* CUSTOM bitmap install: blocked index reads blocked, others allowed. */
static void test_sf_custom_bitmap(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    sf_reset();
    TEST_ASSERT_EQ(syscall_filter_active(), 0, "no filters active initially");

    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, 0);
    /* Block main index 0x10 (word 0, bit 16). */
    p.allow_main[0] &= ~(1ULL << 16);

    NTSTATUS st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "custom filter installs");
    TEST_ASSERT(s_task.syscall_filter != (struct syscall_filter *)0, "filter attached");
    TEST_ASSERT_EQ(syscall_filter_active(), 1, "active count is 1");

    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_MAIN, 0x10),
                   0, "blocked index denied");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_MAIN, 0x11),
                   1, "neighbor index allowed");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_SHADOW, 0x10),
                   1, "shadow index allowed (only main was cleared)");
    sf_reset();
    TEST_ASSERT_EQ(syscall_filter_active(), 0, "teardown restores count");
}

/* DisallowWin32k clears the whole shadow table, leaves main allowed. */
static void test_sf_disallow_win32k(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    sf_reset();
    policy_init(&p, SYSCALL_FILTER_OP_DISALLOW_WIN32K, 0);
    NTSTATUS st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "disallow-win32k installs");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_SHADOW, 0),
                   0, "shadow index 0 blocked");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_SHADOW, 1000),
                   0, "high shadow index blocked");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_MAIN, 5),
                   1, "main table still allowed");
    sf_reset();
}

/* DisallowFsctl clears exactly the NtFsControlFile main index. */
static void test_sf_disallow_fsctl(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    uint32_t fsctl = SSDT_NtFsControlFile & SSDT_INDEX_MASK;
    sf_reset();
    policy_init(&p, SYSCALL_FILTER_OP_DISALLOW_FSCTL, 0);
    NTSTATUS st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "disallow-fsctl installs");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_MAIN, fsctl),
                   0, "NtFsControlFile blocked");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_MAIN, fsctl + 1),
                   1, "adjacent syscall allowed");
    sf_reset();
}

/* LOCKED: relaxation rejected, tightening accepted. */
static void test_sf_locked_tighten_only(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    sf_reset();

    /* Install a LOCKED filter blocking main index 0x20. */
    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, SYSCALL_FILTER_LOCKED);
    p.allow_main[0] &= ~(1ULL << 0);   /* block index 0 */
    NTSTATUS st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "locked filter installs");

    /* Attempt to RELAX (re-allow index 0 via a full-allow custom map). */
    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, SYSCALL_FILTER_LOCKED);
    st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_ACCESS_DENIED, "locked filter rejects relaxation");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_MAIN, 0),
                   0, "index 0 stays blocked after rejected relax");

    /* Attempt to TIGHTEN further (also block index 1) -- must succeed. */
    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, SYSCALL_FILTER_LOCKED);
    p.allow_main[0] &= ~(1ULL << 0);
    p.allow_main[0] &= ~(1ULL << 1);
    st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "locked filter accepts tightening");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(s_task.syscall_filter, SSDT_TABLE_MAIN, 1),
                   0, "newly tightened index blocked");
    sf_reset();
}

/* clone produces an independent, single-generation copy. */
static void test_sf_clone_isolation(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    sf_reset();
    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, SYSCALL_FILTER_INHERIT);
    p.allow_main[0] &= ~(1ULL << 3);
    NTSTATUS st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "parent filter installs");

    SYSCALL_FILTER *c = syscall_filter_clone(s_task.syscall_filter);
    TEST_ASSERT(c != (SYSCALL_FILTER *)0, "clone allocated");
    TEST_ASSERT_EQ(c->retired_prev == (SYSCALL_FILTER *)0, 1, "clone is single-generation");
    TEST_ASSERT_EQ(syscall_filter_index_allowed(c, SSDT_TABLE_MAIN, 3), 0,
                   "clone carries blocked bit");
    TEST_ASSERT_EQ(c->flags & SYSCALL_FILTER_INHERIT, SYSCALL_FILTER_INHERIT,
                   "clone carries flags");
    kfree(c);
    sf_reset();
}

/* Bad inputs rejected. */
static void test_sf_bad_input(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    sf_reset();
    policy_init(&p, 999u, 0);   /* invalid operation */
    TEST_ASSERT_EQ(syscall_filter_set_policy(&s_task, &p), STATUS_INVALID_PARAMETER,
                   "invalid operation rejected");
    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, 0x80u);   /* invalid flag bit */
    TEST_ASSERT_EQ(syscall_filter_set_policy(&s_task, &p), STATUS_INVALID_PARAMETER,
                   "out-of-range flag rejected");
    TEST_ASSERT_EQ(syscall_filter_set_policy((struct task *)0, &p), STATUS_INVALID_PARAMETER,
                   "NULL target rejected");
    TEST_ASSERT_EQ(syscall_filter_index_allowed((SYSCALL_FILTER *)0, SSDT_TABLE_MAIN, 0), 1,
                   "NULL filter allows all");
    sf_reset();
}

/* The retire chain is bounded: past the generation cap, tightening refuses. */
static void test_sf_generation_cap(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    uint32_t i;
    NTSTATUS st = STATUS_SUCCESS;
    sf_reset();

    /* Each unlocked CUSTOM install retires the prior snapshot. Fill to the cap. */
    for (i = 0; i < SYSCALL_FILTER_MAX_GENERATIONS; i++) {
        policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, 0);
        st = syscall_filter_set_policy(&s_task, &p);
        if (st != STATUS_SUCCESS)
            break;
    }
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "installs succeed up to the cap");

    /* The next install exceeds the retire-chain cap. */
    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, 0);
    st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_INSUFFICIENT_RESOURCES, "cap+1 install refused");
    sf_reset();
    TEST_ASSERT_EQ(syscall_filter_active(), 0, "teardown frees whole chain");
}

/* Death/teardown split: syscall_filter_task_dead drops the count but keeps the
 * memory; syscall_filter_task_teardown frees it. Neither double-counts. */
static void test_sf_dead_then_teardown(void)
{
    PROCESS_SYSCALL_FILTER_POLICY p;
    sf_reset();
    policy_init(&p, SYSCALL_FILTER_OP_CUSTOM_BITMAP, 0);
    NTSTATUS st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "filter installs");
    TEST_ASSERT_EQ(syscall_filter_active(), 1, "count is 1");

    /* TASK_DEAD transition: count drops, snapshot still present. */
    syscall_filter_task_dead(&s_task);
    TEST_ASSERT_EQ(syscall_filter_active(), 0, "count drops to 0 at death");
    TEST_ASSERT_EQ(s_task.syscall_filter != (struct syscall_filter *)0, 1,
                   "snapshot memory retained until reap barrier");

    /* Double death is idempotent (no underflow). */
    syscall_filter_task_dead(&s_task);
    TEST_ASSERT_EQ(syscall_filter_active(), 0, "double-dead does not underflow");

    /* Reap barrier: frees + clears the pointer, does not decrement again. */
    syscall_filter_task_teardown(&s_task);
    TEST_ASSERT_EQ(syscall_filter_active(), 0, "count stays 0 after teardown");
    TEST_ASSERT_EQ(s_task.syscall_filter == (struct syscall_filter *)0, 1,
                   "pointer cleared at teardown");

    /* Backstop teardown (task never passed through dead) also holds. */
    st = syscall_filter_set_policy(&s_task, &p);
    TEST_ASSERT_EQ(st, STATUS_SUCCESS, "reinstall after teardown");
    TEST_ASSERT_EQ(syscall_filter_active(), 1, "count is 1 again");
    syscall_filter_task_teardown(&s_task);   /* teardown without prior dead */
    TEST_ASSERT_EQ(syscall_filter_active(), 0, "teardown-only path decrements once");
}

void test_register_syscall_filter(void)
{
    test_suite_register_cat("NT: sfilter custom bitmap", test_sf_custom_bitmap, TEST_CAT_ABI);
    test_suite_register_cat("NT: sfilter disallow win32k", test_sf_disallow_win32k, TEST_CAT_ABI);
    test_suite_register_cat("NT: sfilter disallow fsctl", test_sf_disallow_fsctl, TEST_CAT_ABI);
    test_suite_register_cat("NT: sfilter locked tighten-only", test_sf_locked_tighten_only, TEST_CAT_ABI);
    test_suite_register_cat("NT: sfilter clone isolation", test_sf_clone_isolation, TEST_CAT_ABI);
    test_suite_register_cat("NT: sfilter bad input", test_sf_bad_input, TEST_CAT_ABI);
    test_suite_register_cat("NT: sfilter generation cap", test_sf_generation_cap, TEST_CAT_ABI);
    test_suite_register_cat("NT: sfilter dead-then-teardown", test_sf_dead_then_teardown, TEST_CAT_ABI);
}
