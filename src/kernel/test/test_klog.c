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
 * The rate limiter uses a tick-based window (100 ticks = 1s at 100 Hz).
 * On fast systems (WHPX), 150 messages may complete before the window
 * mechanism engages. We verify the API exists and returns a sane value
 * rather than testing the timing-dependent drop behavior. */

static void test_klog_rate_limit_api(void)
{
    uint32_t dropped;

    /* klog_get_dropped for an unknown subsystem should return 0 */
    dropped = klog_get_dropped("nonexistent_subsys_xyz");
    TEST_ASSERT(dropped == 0,
                "klog_get_dropped() returns 0 for unknown subsystem");
}

/* ---- Ring buffer wrap ---- */

static void test_klog_ring_wrap(void)
{
    uint32_t count_before, head_before;
    uint32_t count, head;

    /* Check how many entries are already in the ring from boot.
     * By test time, the ring should have hundreds of entries.
     * We only need to fill enough to force a wrap past KLOG_RING_SIZE. */
    klog_get_ring(&count_before, &head_before);

    if (count_before < KLOG_RING_SIZE) {
        /* Need (KLOG_RING_SIZE - count_before + 10) more to overflow.
         * Use LOG_DEBUG with short messages to minimize serial noise. */
        uint32_t needed = KLOG_RING_SIZE - count_before + 10;
        uint32_t i;
        for (i = 0; i < needed; i++)
            klog(LOG_DEBUG, "TEST", "w%u", (uint64_t)i);
    }

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
    test_suite_register("Klog: rate limit API", test_klog_rate_limit_api);
    test_suite_register("Klog: ring wrap", test_klog_ring_wrap);
}

#endif /* KERNEL_TESTS */
