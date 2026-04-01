/* ============================================================================
 * test_klog.c — Kernel logging unit tests
 *
 * Tests ring buffer, per-subsystem filtering, rate limiting, and drop counts.
 *
 * XREF: 02-kernel-core/TODO-02-system-logging.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/klog.h"

/* ---- Ring buffer: klog writes to ring and head advances ---- */

static void test_klog_ring_write(void)
{
    uint32_t count_before, head_before;
    uint32_t count_after, head_after;

    klog_get_ring(&count_before, &head_before);
    klog(LOG_INFO, "TEST", "klog_ring_write test entry");
    klog_get_ring(&count_after, &head_after);

    TEST_ASSERT(head_after != head_before,
                "klog() advances ring head");
    TEST_ASSERT(count_after >= count_before,
                "klog() increments ring count");
}

/* ---- Per-subsystem level filtering: dropped below threshold ---- */

static void test_klog_level_drop(void)
{
    uint32_t count_before, head_before;
    uint32_t count_after, head_after;

    /* Set "mm" subsystem to WARN — DEBUG entries should be dropped */
    klog_set_level("mm", LOG_WARN);

    klog_get_ring(&count_before, &head_before);
    klog(LOG_DEBUG, "mm", "this should be dropped");
    klog_get_ring(&count_after, &head_after);

    TEST_ASSERT(head_after == head_before,
                "LOG_DEBUG dropped after klog_set_level(mm, LOG_WARN)");

    /* Restore default */
    klog_set_level("mm", LOG_DEBUG);
}

static void test_klog_level_pass(void)
{
    uint32_t count_before, head_before;
    uint32_t count_after, head_after;

    /* Set "mm" to WARN — WARN entries should pass through */
    klog_set_level("mm", LOG_WARN);

    klog_get_ring(&count_before, &head_before);
    klog(LOG_WARN, "mm", "this should pass");
    klog_get_ring(&count_after, &head_after);

    TEST_ASSERT(head_after != head_before,
                "LOG_WARN not dropped after klog_set_level(mm, LOG_WARN)");

    /* Restore default */
    klog_set_level("mm", LOG_DEBUG);
}

/* ---- Global level override ---- */

static void test_klog_global_level(void)
{
    uint32_t head_before, head_after, dummy;

    /* Set global to ERROR — INFO and WARN should be dropped */
    klog_set_level((const char *)0, LOG_ERROR);

    klog_get_ring(&dummy, &head_before);
    klog(LOG_INFO, "test_global", "should be dropped by global");
    klog_get_ring(&dummy, &head_after);

    TEST_ASSERT(head_after == head_before,
                "LOG_INFO suppressed by global LOG_ERROR override");

    klog_get_ring(&dummy, &head_before);
    klog(LOG_WARN, "test_global", "should also be dropped");
    klog_get_ring(&dummy, &head_after);

    TEST_ASSERT(head_after == head_before,
                "LOG_WARN suppressed by global LOG_ERROR override");

    /* Restore global default */
    klog_set_level((const char *)0, LOG_DEBUG);
}

/* ---- Rate limiting ----
 * Threshold is 100 msgs per 1-second window (KLOG_RATE_DEFAULT=100).
 * We can't easily trigger the window in a unit test (timer-dependent),
 * but we can verify that after a burst, klog_get_dropped() returns > 0. */

static void test_klog_rate_limit(void)
{
    uint32_t i;
    uint32_t dropped;

    /* Restore global to allow all levels */
    klog_set_level((const char *)0, LOG_DEBUG);

    /* Fire 150 messages from "rate_test" subsystem — should exceed 100 limit */
    for (i = 0; i < 150; i++)
        klog(LOG_DEBUG, "rate_test", "burst msg %u", (uint64_t)i);

    dropped = klog_get_dropped("rate_test");
    TEST_ASSERT(dropped > 0,
                "klog_get_dropped() > 0 after rate-limited burst");
}

/* ---- Ring buffer wrap ---- */

static void test_klog_ring_wrap(void)
{
    uint32_t count, head;
    uint32_t i;

    /* Fill the ring past KLOG_RING_SIZE to force wrap.
     * The ring is already partially filled from boot, so we just need
     * enough entries to push past 1000 total. */
    for (i = 0; i < KLOG_RING_SIZE + 10; i++)
        klog(LOG_DEBUG, "wrap_test", "fill %u", (uint64_t)i);

    klog_get_ring(&count, &head);

    TEST_ASSERT(count == KLOG_RING_SIZE,
                "ring count caps at KLOG_RING_SIZE after overflow");
    TEST_ASSERT(head < KLOG_RING_SIZE,
                "ring head wraps within bounds");
}

/* ---- Registration ---- */

void test_register_klog(void)
{
    test_suite_register("Klog: ring write", test_klog_ring_write);
    test_suite_register("Klog: level drop", test_klog_level_drop);
    test_suite_register("Klog: level pass", test_klog_level_pass);
    test_suite_register("Klog: global level", test_klog_global_level);
    test_suite_register("Klog: rate limit", test_klog_rate_limit);
    test_suite_register("Klog: ring wrap", test_klog_ring_wrap);
}

#endif /* KERNEL_TESTS */
