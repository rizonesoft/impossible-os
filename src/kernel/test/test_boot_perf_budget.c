/* SPDX-License-Identifier: MIT */
/* Unit tests for boot_perf_budget classifier + lookup.
 *
 * All tests are pure-helper calls -- no live boot infrastructure. The
 * Test Code Policy hard ban prevents tests from invoking boot_progress
 * / boot_perf_dump / vpd_* / etc.; this file only exercises the pure
 * classify(observed,target) and lookup(name) functions. */

#include "kernel/types.h"
#include "kernel/test/test.h"
#include "kernel/boot_perf_budget.h"

/* ---- Classifier boundary cases ----------------------------------------- */

static void test_budget_classify_zero_target(void)
{
    /* Target 0 always returns OK (caller has no budget). */
    TEST_ASSERT_EQ(boot_perf_budget_classify(0,    0), BUDGET_OK,
                   "(0,0) is OK");
    TEST_ASSERT_EQ(boot_perf_budget_classify(9999, 0), BUDGET_OK,
                   "(9999,0) is OK");
}

static void test_budget_classify_well_under(void)
{
    TEST_ASSERT_EQ(boot_perf_budget_classify(0,   100), BUDGET_OK,
                   "(0, 100) is OK");
    TEST_ASSERT_EQ(boot_perf_budget_classify(99,  100), BUDGET_OK,
                   "(99, 100) is OK");
}

static void test_budget_classify_at_target(void)
{
    TEST_ASSERT_EQ(boot_perf_budget_classify(100, 100), BUDGET_OK,
                   "(100, 100) at target is OK");
}

static void test_budget_classify_at_soft_boundary(void)
{
    /* 1.5x = soft cap. 150 == soft -> still OK (boundary inclusive). */
    TEST_ASSERT_EQ(boot_perf_budget_classify(150, 100), BUDGET_OK,
                   "(150, 100) at 1.5x is OK");
}

static void test_budget_classify_just_above_soft(void)
{
    /* 151 > 150 -> SOFT. */
    TEST_ASSERT_EQ(boot_perf_budget_classify(151, 100), BUDGET_SOFT,
                   "(151, 100) just above 1.5x is SOFT");
}

static void test_budget_classify_at_hard_boundary(void)
{
    /* 4x = hard cap. 400 == hard -> still SOFT (boundary inclusive). */
    TEST_ASSERT_EQ(boot_perf_budget_classify(400, 100), BUDGET_SOFT,
                   "(400, 100) at 4x is SOFT");
}

static void test_budget_classify_just_above_hard(void)
{
    /* 401 > 400 -> HARD. */
    TEST_ASSERT_EQ(boot_perf_budget_classify(401, 100), BUDGET_HARD,
                   "(401, 100) just above 4x is HARD");
}

static void test_budget_classify_extreme_overrun(void)
{
    /* 1342ms vs 100ms target = 13.4x = HARD (the SMBIOS observation that
     * motivated this section). */
    TEST_ASSERT_EQ(boot_perf_budget_classify(1342, 100), BUDGET_HARD,
                   "(1342, 100) extreme overrun is HARD");
}

/* ---- Lookup ------------------------------------------------------------ */

static void test_budget_lookup_known_step(void)
{
    const struct boot_phase_budget *b = boot_perf_budget_lookup("SMBIOS");
    TEST_ASSERT(b != (const struct boot_phase_budget *)0,
                "SMBIOS budget exists");
    TEST_ASSERT_EQ((uint32_t)b->target_ms, 100u,
                   "SMBIOS target is 100ms");
}

static void test_budget_lookup_unknown_step(void)
{
    TEST_ASSERT(boot_perf_budget_lookup("NOT_A_REAL_STEP")
                == (const struct boot_phase_budget *)0,
                "unknown step lookup returns NULL");
}

static void test_budget_lookup_null_step(void)
{
    TEST_ASSERT(boot_perf_budget_lookup((const char *)0)
                == (const struct boot_phase_budget *)0,
                "NULL step lookup returns NULL");
}

static void test_budget_lookup_desktop_ready_excluded(void)
{
    /* DESKTOP_READY is the last recorded step -- per-step delta is
     * always 0 there. The total-boot-time check covers it instead.
     * The per-step table must NOT carry a DESKTOP_READY entry. */
    TEST_ASSERT(boot_perf_budget_lookup("DESKTOP_READY")
                == (const struct boot_phase_budget *)0,
                "DESKTOP_READY excluded from per-step budgets");
}

/* ---- Table integrity -------------------------------------------------- */

static void test_budget_table_size(void)
{
    TEST_ASSERT_EQ(boot_perf_budget_count(), 18u,
                   "exactly 18 named per-step budgets");
}

static void test_budget_table_all_have_step_and_reason(void)
{
    uint32_t n = boot_perf_budget_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct boot_phase_budget *b = boot_perf_budget_get(i);
        TEST_ASSERT(b != (const struct boot_phase_budget *)0,
                    "budget entry non-NULL");
        TEST_ASSERT(b->step != (const char *)0 && b->step[0] != 0,
                    "budget step name non-empty");
        TEST_ASSERT(b->reason != (const char *)0 && b->reason[0] != 0,
                    "budget reason non-empty");
        TEST_ASSERT(b->target_ms > 0u,
                    "budget target_ms positive");
    }
}

static void test_budget_get_oob_returns_null(void)
{
    TEST_ASSERT(boot_perf_budget_get(boot_perf_budget_count())
                == (const struct boot_phase_budget *)0,
                "get(count) returns NULL");
    TEST_ASSERT(boot_perf_budget_get(999u)
                == (const struct boot_phase_budget *)0,
                "get(999) returns NULL");
}

void test_register_boot_perf_budget(void)
{
    test_suite_register_cat("boot_perf_budget: classify zero target",
        test_budget_classify_zero_target, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: classify well under target",
        test_budget_classify_well_under, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: classify at target",
        test_budget_classify_at_target, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: classify at 1.5x soft boundary",
        test_budget_classify_at_soft_boundary, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: classify just above soft",
        test_budget_classify_just_above_soft, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: classify at 4x hard boundary",
        test_budget_classify_at_hard_boundary, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: classify just above hard",
        test_budget_classify_just_above_hard, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: classify extreme overrun",
        test_budget_classify_extreme_overrun, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: lookup known step",
        test_budget_lookup_known_step, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: lookup unknown step returns NULL",
        test_budget_lookup_unknown_step, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: lookup NULL step returns NULL",
        test_budget_lookup_null_step, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: DESKTOP_READY excluded",
        test_budget_lookup_desktop_ready_excluded, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: table size 18",
        test_budget_table_size, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: every entry has step + reason + target",
        test_budget_table_all_have_step_and_reason, TEST_CAT_BOOT);
    test_suite_register_cat("boot_perf_budget: get(OOB) returns NULL",
        test_budget_get_oob_returns_null, TEST_CAT_BOOT);
}
