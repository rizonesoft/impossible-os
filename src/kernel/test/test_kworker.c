/* ============================================================================
 * test_kworker.c -- system worker thread pool (kworker) unit tests
 *
 * Deterministic tests of the register/unregister/rundown logic. Each entry is
 * registered with a huge period (1 hour) so the live boot-started kworker thread
 * never fires it during the brief test window -- these tests exercise the slot
 * table, token validity, and generation-rundown protocol, NOT the firing
 * cadence (that is timing-sensitive and validated at runtime on WHPX / bare
 * metal per the section's test checkpoint).
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/sched/kworker.h"

#define KW_TEST_PERIOD_MS  (3600u * 1000u)   /* 1 hour -- never fires in-test */

static void kw_noop(void *ctx) { (void)ctx; }

/* register validation: NULL fn, period 0, and the happy path. */
static void test_kworker_register_validation(void)
{
    int bad_fn  = kworker_register((kworker_callback_t)0, (void *)0, KW_TEST_PERIOD_MS);
    int bad_per = kworker_register(kw_noop, (void *)0, 0);
    int ok      = kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS);

    TEST_ASSERT(bad_fn < 0,  "register with NULL fn rejected");
    TEST_ASSERT(bad_per < 0, "register with period 0 rejected");
    TEST_ASSERT(ok >= 0,     "register with valid args returns a token");
    TEST_ASSERT_EQ((uint64_t)kworker_unregister(ok), 0u, "unregister of a live token succeeds");
}

/* unregistering a token twice: the second call sees an inactive slot. */
static void test_kworker_stale_token(void)
{
    int t = kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS);
    TEST_ASSERT(t >= 0, "registered");
    TEST_ASSERT_EQ((uint64_t)kworker_unregister(t), 0u, "first unregister succeeds");
    TEST_ASSERT(kworker_unregister(t) < 0, "second unregister of the same token fails (inactive)");
}

/* generation rundown: a slot reused by a new registration rejects the OLD
 * token (generation mismatch), so a stale token can never cancel the new entry. */
static void test_kworker_generation(void)
{
    int t1 = kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS);
    TEST_ASSERT(t1 >= 0, "first register");
    TEST_ASSERT_EQ((uint64_t)kworker_unregister(t1), 0u, "unregister t1");

    int t2 = kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS);
    TEST_ASSERT(t2 >= 0, "re-register (reuses the freed slot)");
    TEST_ASSERT(t2 != t1, "reused slot yields a DIFFERENT token (generation bumped)");
    TEST_ASSERT(kworker_unregister(t1) < 0, "the stale t1 token cannot cancel the new entry");
    TEST_ASSERT_EQ((uint64_t)kworker_unregister(t2), 0u, "t2 unregisters cleanly");
}

/* slot exhaustion: fill every FREE slot until the table is full (other slots
 * may already be taken by boot-registered monitors, e.g. the SecureBoot drift
 * monitor), then the next register fails, and freeing one slot lets a
 * registration succeed again. */
static void test_kworker_slot_exhaustion(void)
{
    int tokens[KWORKER_MAX_ENTRIES];
    uint32_t n = 0, i;
    int tok;

    while (n < KWORKER_MAX_ENTRIES) {
        tok = kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS);
        if (tok < 0)
            break;
        tokens[n++] = tok;
    }
    TEST_ASSERT(n >= 1, "at least one slot was free to fill");
    TEST_ASSERT(kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS) < 0,
                "registration on a full table fails");

    TEST_ASSERT_EQ((uint64_t)kworker_unregister(tokens[0]), 0u, "free one slot");
    tokens[0] = kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS);
    TEST_ASSERT(tokens[0] >= 0, "registration succeeds after a slot is freed");

    for (i = 0; i < n; i++)
        kworker_unregister(tokens[i]);
}

/* last-fire accessor on a stale/invalid token returns 0. */
static void test_kworker_last_fire_invalid(void)
{
    int t = kworker_register(kw_noop, (void *)0, KW_TEST_PERIOD_MS);
    TEST_ASSERT(t >= 0, "registered");
    kworker_unregister(t);
    TEST_ASSERT_EQ(kworker_last_fire_ns(t), 0u, "last_fire_ns on a stale token is 0");
    TEST_ASSERT_EQ(kworker_last_fire_ns(-1), 0u, "last_fire_ns on a negative token is 0");
}

void test_register_kworker(void)
{
    test_suite_register_cat("kworker: register validation",
                            test_kworker_register_validation, TEST_CAT_SCHED);
    test_suite_register_cat("kworker: stale token rejected",
                            test_kworker_stale_token, TEST_CAT_SCHED);
    test_suite_register_cat("kworker: generation rundown",
                            test_kworker_generation, TEST_CAT_SCHED);
    test_suite_register_cat("kworker: slot exhaustion",
                            test_kworker_slot_exhaustion, TEST_CAT_SCHED);
    test_suite_register_cat("kworker: last_fire_ns invalid token",
                            test_kworker_last_fire_invalid, TEST_CAT_SCHED);
}
