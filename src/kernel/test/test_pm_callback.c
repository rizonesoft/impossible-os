/*
 * test_pm_callback.c -- Driver power callbacks and sleep/resume ordering
 *
 * Covers src/kernel/pm/power_callback.c.
 * XREF: 02-kernel-core/TODO-26-power-management.md section 9
 *
 * EVERY test drives its OWN pm_cb_table_t. Not one of them touches the
 * production singleton, and that is a correctness requirement rather than a
 * preference: driver registration happens in Phase 1/2 boot and this runner
 * does not execute until Phase 3, so the singleton already holds real AHCI,
 * xHCI and EC callbacks by the time a test runs. Calling pm_notify_sleep()
 * here would quiesce live hardware during an ordinary test boot, and filling
 * the singleton to capacity would permanently consume slots no unregister can
 * return. The table/singleton split in power_callback.h exists for this.
 *
 * The clock is injected (t.now_ns), so the overrun tests assert on a
 * deterministic elapsed time rather than on how long a real callback happened
 * to take on a loaded host.
 */

#include "kernel/types.h"
#include "kernel/test/test.h"
#include "kernel/pm/power_callback.h"

/* ---- Fixtures ------------------------------------------------------------
 * Order is recorded into a shared trace so a test can assert the exact
 * sequence of callbacks, which is the section's core claim. */

#define TRACE_MAX 32

static uint32_t s_trace[TRACE_MAX];
static uint32_t s_trace_len;
static uint64_t s_fake_ns;

/* Trace tokens: priority * 10 + phase (1 = sleep, 2 = wake). */
#define TRACE_SLEEP(pri) ((pri) * 10u + 1u)
#define TRACE_WAKE(pri)  ((pri) * 10u + 2u)

static void trace_reset(void)
{
    uint32_t i;

    for (i = 0; i < TRACE_MAX; i++)
        s_trace[i] = 0;
    s_trace_len = 0;
    s_fake_ns   = 0;
}

static void trace_put(uint32_t token)
{
    if (s_trace_len < TRACE_MAX)
        s_trace[s_trace_len++] = token;
}

static uint64_t fake_now_ns(void)
{
    return s_fake_ns;
}

/* Advance the injected clock by a fixed amount on every call, so a callback
 * can be made to "take" a chosen duration. */
static uint64_t s_advance_per_call_ns;

static uint64_t fake_now_ns_advancing(void)
{
    uint64_t v = s_fake_ns;
    s_fake_ns += s_advance_per_call_ns;
    return v;
}

/* Steps BACKWARD on every call, so the "after" sample is lower than the
 * "before" one. Halving rather than subtracting a constant keeps it monotone
 * decreasing without ever underflowing to a huge unsigned value, which would
 * test the opposite of what this fixture is for. */
static uint64_t fake_now_ns_backward(void)
{
    uint64_t v = s_fake_ns;
    s_fake_ns /= 2ull;
    return v;
}

static int cb_sleep_ok(uint32_t state, void *ctx)
{
    (void)state;
    trace_put(TRACE_SLEEP((uint32_t)(uint64_t)ctx));
    return 0;
}

static int cb_wake_ok(uint32_t state, void *ctx)
{
    (void)state;
    trace_put(TRACE_WAKE((uint32_t)(uint64_t)ctx));
    return 0;
}

static int cb_sleep_fail(uint32_t state, void *ctx)
{
    (void)state;
    trace_put(TRACE_SLEEP((uint32_t)(uint64_t)ctx));
    return -42;
}

static int cb_wake_fail(uint32_t state, void *ctx)
{
    (void)state;
    trace_put(TRACE_WAKE((uint32_t)(uint64_t)ctx));
    return -7;
}

/* Register one slot per priority level, ctx carrying the priority so the
 * callbacks can trace which one ran. */
static void seed_all_priorities(pm_cb_table_t *t)
{
    uint32_t pri;

    for (pri = 0; pri < (uint32_t)PM_PRI_COUNT; pri++)
        (void)pm_cb_table_register(t, (pm_priority_t)pri, cb_sleep_ok,
                                   cb_wake_ok, (void *)(uint64_t)pri, "seed");
}


/* ---- Tests ---------------------------------------------------------------
 * Grouped into suites rather than one function per assertion. The kernel image
 * sits against its 0x800000 user-base ceiling, and 26 separate frames cost
 * multiple KB of .text for no extra coverage: every assertion below survived
 * the consolidation, each with its own message. */

static void test_pm_registration(void)
{
    pm_cb_table_t t;
    uint32_t      i;

    pm_cb_table_init(&t);
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_STORAGE, cb_sleep_ok,
                                        cb_wake_ok, (void *)0, "disk"),
                   0, "first registration takes slot 0");
    TEST_ASSERT_EQ(pm_cb_table_count(&t), 1u, "count reflects registration");
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_IDLE, "table starts idle");

    /* Same triple twice must not double-notify; a different ctx is a
     * different device and must be accepted. */
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok,
                                        cb_wake_ok, (void *)0x1234, "usb"),
                   1, "distinct slot accepted");
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok,
                                        cb_wake_ok, (void *)0x1234, "usb"),
                   PM_CB_DUPLICATE, "duplicate triple refused");
    TEST_ASSERT_EQ(pm_cb_table_count(&t), 2u, "refusal consumed no slot");
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok,
                                        cb_wake_ok, (void *)0x5678, "usb2"),
                   2, "distinct ctx is a distinct device");

    TEST_ASSERT_EQ(pm_cb_table_register(&t, (pm_priority_t)PM_PRI_COUNT,
                                        cb_sleep_ok, cb_wake_ok, (void *)0,
                                        "bad"),
                   PM_CB_INVALID, "priority at the enum bound refused");
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_INPUT,
                                        (pm_power_callback_t)0,
                                        (pm_power_callback_t)0, (void *)0,
                                        "empty"),
                   PM_CB_INVALID, "slot with neither callback refused");
    /* A quiescible slot must have a way back, or an aborted sleep leaves it
     * down while reporting clean recovery. */
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_INPUT, cb_sleep_ok,
                                        (pm_power_callback_t)0, (void *)0,
                                        "sleeponly"),
                   PM_CB_INVALID, "on_sleep without on_wake refused");
    TEST_ASSERT_EQ(pm_cb_table_register((pm_cb_table_t *)0, PM_PRI_USER,
                                        cb_sleep_ok, cb_wake_ok, (void *)0,
                                        "null"),
                   PM_CB_INVALID, "NULL table refused not dereferenced");
    TEST_ASSERT_EQ(pm_cb_table_count((const pm_cb_table_t *)0), 0u,
                   "NULL table counts 0");

    /* Fill to the bound and prove slot 65 is refused rather than written
     * past the end. */
    pm_cb_table_init(&t);
    for (i = 0; i < PM_CB_MAX_SLOTS; i++) {
        if (pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok, cb_wake_ok,
                                 (void *)(uint64_t)(i + 1u), "fill") < 0)
            break;
    }
    TEST_ASSERT_EQ(pm_cb_table_count(&t), (uint32_t)PM_CB_MAX_SLOTS,
                   "exactly PM_CB_MAX_SLOTS accepted");
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok,
                                        cb_wake_ok, (void *)0xDEAD, "over"),
                   PM_CB_FULL, "slot 65 refused");
    TEST_ASSERT_EQ(pm_cb_table_count(&t), (uint32_t)PM_CB_MAX_SLOTS,
                   "refusal did not grow past the bound");
}

static void test_pm_walk_ordering(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    pm_cb_table_init(&t);
    trace_reset();
    seed_all_priorities(&t);

    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep), PM_CB_OK,
                   "sleep walk succeeds");
    TEST_ASSERT_EQ(rep.invoked, (uint32_t)PM_PRI_COUNT, "every slot invoked");
    TEST_ASSERT_EQ(rep.failed, 0u, "no failures");
    TEST_ASSERT_EQ(s_trace_len, (uint32_t)PM_PRI_COUNT, "one trace per slot");

    /* Sleep: user-space first, storage last. */
    TEST_ASSERT_EQ(s_trace[0], TRACE_SLEEP((uint32_t)PM_PRI_USER),
                   "sleep starts at user");
    TEST_ASSERT_EQ(s_trace[1], TRACE_SLEEP((uint32_t)PM_PRI_GRAPHICS),
                   "sleep 2nd graphics");
    TEST_ASSERT_EQ(s_trace[2], TRACE_SLEEP((uint32_t)PM_PRI_INPUT),
                   "sleep 3rd input");
    TEST_ASSERT_EQ(s_trace[3], TRACE_SLEEP((uint32_t)PM_PRI_USB),
                   "sleep 4th usb");
    TEST_ASSERT_EQ(s_trace[4], TRACE_SLEEP((uint32_t)PM_PRI_NETWORK),
                   "sleep 5th network");
    TEST_ASSERT_EQ(s_trace[5], TRACE_SLEEP((uint32_t)PM_PRI_STORAGE),
                   "sleep ends at storage");

    /* Resume: the exact inverse, storage first. */
    trace_reset();
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep), PM_CB_OK,
                   "resume walk succeeds");
    TEST_ASSERT_EQ(s_trace_len, (uint32_t)PM_PRI_COUNT, "one trace per slot");
    TEST_ASSERT_EQ(s_trace[0], TRACE_WAKE((uint32_t)PM_PRI_STORAGE),
                   "resume starts at storage");
    TEST_ASSERT_EQ(s_trace[1], TRACE_WAKE((uint32_t)PM_PRI_NETWORK),
                   "resume 2nd network");
    TEST_ASSERT_EQ(s_trace[2], TRACE_WAKE((uint32_t)PM_PRI_USB),
                   "resume 3rd usb");
    TEST_ASSERT_EQ(s_trace[3], TRACE_WAKE((uint32_t)PM_PRI_INPUT),
                   "resume 4th input");
    TEST_ASSERT_EQ(s_trace[4], TRACE_WAKE((uint32_t)PM_PRI_GRAPHICS),
                   "resume 5th graphics");
    TEST_ASSERT_EQ(s_trace[5], TRACE_WAKE((uint32_t)PM_PRI_USER),
                   "resume ends at user");

    /* Registration order decides within one priority. */
    pm_cb_table_init(&t);
    trace_reset();
    (void)pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok, cb_wake_ok,
                               (void *)1, "first");
    (void)pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok, cb_wake_ok,
                               (void *)2, "second");
    (void)pm_cb_table_notify_sleep(&t, 3u, &rep);
    TEST_ASSERT_EQ(s_trace_len, 2u, "both same-priority slots ran");
    TEST_ASSERT_EQ(s_trace[0], TRACE_SLEEP(1u), "registration order holds");
    TEST_ASSERT_EQ(s_trace[1], TRACE_SLEEP(2u), "second runs second");
}

static void test_pm_transaction_span(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    pm_cb_table_init(&t);
    trace_reset();
    seed_all_priorities(&t);

    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_IDLE, "starts idle");
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep), PM_CB_OK,
                   "sleep opens the transaction");
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_ASLEEP, "now asleep");

    /* The window the transaction exists to close: a driver admitted here
     * would be woken by the resume walk having never been quiesced. */
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok,
                                        cb_wake_ok, (void *)0x99, "late"),
                   PM_CB_BUSY, "registration refused mid-transaction");
    TEST_ASSERT_EQ(pm_cb_table_count(&t), (uint32_t)PM_PRI_COUNT,
                   "refused registration did not enter the table");
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep), PM_CB_BUSY,
                   "second sleep refused not interleaved");

    trace_reset();
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep), PM_CB_OK,
                   "resume closes the transaction");
    TEST_ASSERT_EQ(s_trace_len, (uint32_t)PM_PRI_COUNT,
                   "resume woke exactly what slept");
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_IDLE, "back to idle");
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep),
                   PM_CB_NO_TRANSACTION, "second resume refused");

    /* A resume with no preceding sleep is a caller bug, not a no-op. */
    pm_cb_table_init(&t);
    trace_reset();
    seed_all_priorities(&t);
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep),
                   PM_CB_NO_TRANSACTION, "resume without sleep refused");
    TEST_ASSERT_EQ(s_trace_len, 0u, "no callback ran");
}

static void test_pm_sleep_abort_unwinds(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    pm_cb_table_init(&t);
    trace_reset();

    /* USER and GRAPHICS quiesce; INPUT fails. Sleep runs USER, GRAPHICS,
     * INPUT, so exactly the first two must be woken back up -- and STORAGE,
     * which never slept, must not be. */
    (void)pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_USER, "user");
    (void)pm_cb_table_register(&t, PM_PRI_GRAPHICS, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_GRAPHICS, "gfx");
    (void)pm_cb_table_register(&t, PM_PRI_INPUT, cb_sleep_fail, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_INPUT, "input");
    (void)pm_cb_table_register(&t, PM_PRI_STORAGE, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_STORAGE, "disk");

    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep),
                   PM_CB_CALLBACK_FAILED, "failed quiesce fails the walk");
    TEST_ASSERT_EQ(rep.first_status, -42, "callback status is kept");
    TEST_ASSERT_EQ(rep.first_failed_priority, (uint32_t)PM_PRI_INPUT,
                   "failing priority reported");
    TEST_ASSERT_EQ(rep.unwound, 2u, "exactly the two quiesced slots unwound");
    TEST_ASSERT_EQ(s_trace_len, 5u, "3 sleep calls then 2 unwind calls");
    TEST_ASSERT_EQ(s_trace[2], TRACE_SLEEP((uint32_t)PM_PRI_INPUT),
                   "walk stopped at the failure");
    TEST_ASSERT_EQ(s_trace[3], TRACE_WAKE((uint32_t)PM_PRI_GRAPHICS),
                   "unwind in resume order: graphics first");
    TEST_ASSERT_EQ(s_trace[4], TRACE_WAKE((uint32_t)PM_PRI_USER),
                   "unwind ends at user");
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_IDLE,
                   "recovered abort returns to idle");

    /* A slot whose own sleep failed never quiesced, so it is never woken. */
    pm_cb_table_init(&t);
    trace_reset();
    (void)pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_fail, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_USER, "user");
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep),
                   PM_CB_CALLBACK_FAILED, "the walk fails");
    TEST_ASSERT_EQ(rep.unwound, 0u, "nothing quiesced nothing unwound");
    TEST_ASSERT_EQ(s_trace_len, 1u, "only the failing sleep ran");
}

static void test_pm_unwind_failure_degrades(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    pm_cb_table_init(&t);
    trace_reset();

    /* USER quiesces but CANNOT be woken; INPUT's sleep then fails, so the
     * abort tries to restore USER and cannot. The machine is in an unknown
     * power state and the table must say so rather than reporting idle. */
    (void)pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok, cb_wake_fail,
                               (void *)(uint64_t)PM_PRI_USER, "stuck");
    (void)pm_cb_table_register(&t, PM_PRI_INPUT, cb_sleep_fail, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_INPUT, "input");

    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep),
                   PM_CB_UNWIND_FAILED, "failed unwind has its own status");
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_DEGRADED,
                   "table does not return to idle");

    /* Terminal until an explicit re-init: nothing may stack on top of
     * hardware nobody can account for. */
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok,
                                        cb_wake_ok, (void *)0x1, "late"),
                   PM_CB_BUSY, "registration refused while degraded");
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep), PM_CB_BUSY,
                   "new sleep refused while degraded");
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep),
                   PM_CB_NO_TRANSACTION, "resume refused while degraded");
    pm_cb_table_init(&t);
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_IDLE,
                   "re-init is the only way out");
}

static void test_pm_resume_failure_policy(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    pm_cb_table_init(&t);
    trace_reset();

    /* NETWORK's wake fails. Resume must still wake USB and USER after it --
     * abandoning the walk would strand them powered down. */
    (void)pm_cb_table_register(&t, PM_PRI_NETWORK, cb_sleep_ok, cb_wake_fail,
                               (void *)(uint64_t)PM_PRI_NETWORK, "net");
    (void)pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_USB, "usb");
    (void)pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_USER, "user");
    (void)pm_cb_table_notify_sleep(&t, 3u, &rep);
    trace_reset();

    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep),
                   PM_CB_CALLBACK_FAILED, "failed wake fails the walk");
    TEST_ASSERT_EQ(rep.failed, 1u, "exactly one wake failed");
    TEST_ASSERT_EQ(rep.invoked, 3u, "all three wakes still attempted");
    TEST_ASSERT_EQ(s_trace_len, 3u, "walk did not stop at the failure");
    TEST_ASSERT_EQ(s_trace[2], TRACE_WAKE((uint32_t)PM_PRI_USER),
                   "last slot still woken");
    TEST_ASSERT_EQ(rep.storage_failed, 0u,
                   "network failure is not a storage failure");
    /* EVERY failed wake degrades, not just storage: the device is still down
     * whichever class it belongs to. Without this the table would reopen and
     * lose the failed slot's identity. */
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_DEGRADED,
                   "non-storage failure degrades too");
    /* Slot 0 is the network registration and is the ONLY one that failed, so
     * the retained masks must name exactly it, not "some bits set". */
    TEST_ASSERT_EQ(t.quiesced_mask, 1ull, "retains exactly the failed slot");
    TEST_ASSERT_EQ(t.wake_mask, 1ull, "wake mask retains the same slot");

    /* Storage is the one failure a caller may not treat as advisory. */
    pm_cb_table_init(&t);
    trace_reset();
    (void)pm_cb_table_register(&t, PM_PRI_STORAGE, cb_sleep_ok, cb_wake_fail,
                               (void *)(uint64_t)PM_PRI_STORAGE, "disk");
    (void)pm_cb_table_notify_sleep(&t, 3u, &rep);
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep),
                   PM_CB_STORAGE_FAILED, "storage gets its own status");
    TEST_ASSERT_EQ(rep.storage_failed, 1u,
                   "storage sets the do-not-unfreeze flag");
    /* The registry must still know WHICH controller is down: clearing it here
     * would erase the identity the caller was just told to act on. */
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_DEGRADED,
                   "failed resume degrades not reopens");
    TEST_ASSERT_EQ(t.quiesced_mask, 1ull, "retains the failed disk slot");
    TEST_ASSERT_EQ(t.wake_mask, 1ull, "wake mask retains the disk slot");
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USB, cb_sleep_ok,
                                        cb_wake_ok, (void *)0x2, "late"),
                   PM_CB_BUSY, "no new registration after a failed resume");

    /* A wake-only slot that FAILS its wake: never quiesced, so it holds a
     * wake_mask bit and no quiesced_mask bit going in. It must still be
     * retained as unrecovered. */
    pm_cb_table_init(&t);
    trace_reset();
    (void)pm_cb_table_register(&t, PM_PRI_INPUT, (pm_power_callback_t)0,
                               cb_wake_fail, (void *)(uint64_t)PM_PRI_INPUT,
                               "ec");
    (void)pm_cb_table_notify_sleep(&t, 3u, &rep);
    TEST_ASSERT_EQ(t.quiesced_mask, 0ull, "wake-only slot never quiesced");
    TEST_ASSERT_EQ(t.wake_mask, 1ull, "but is eligible for the wake");
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep),
                   PM_CB_CALLBACK_FAILED, "failed wake-only wake reported");
    TEST_ASSERT_EQ(pm_cb_table_txn_state(&t), PM_TXN_DEGRADED,
                   "failed wake-only wake degrades");
    TEST_ASSERT_EQ(t.wake_mask, 1ull, "and retains that slot");
}

static void test_pm_half_registered_slots(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    /* The shape the EC driver registers: no sleep half (it needs GPE control
     * this kernel does not have), but a resume half that re-proves the
     * controller idle. It must still be woken by a completed resume. */
    pm_cb_table_init(&t);
    trace_reset();
    (void)pm_cb_table_register(&t, PM_PRI_INPUT, (pm_power_callback_t)0,
                               cb_wake_ok, (void *)(uint64_t)PM_PRI_INPUT,
                               "ec");
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep), PM_CB_OK,
                   "wake-only slot does not fail the sleep");
    TEST_ASSERT_EQ(rep.invoked, 0u, "no sleep callback to invoke");
    TEST_ASSERT_EQ(s_trace_len, 0u, "nothing ran during sleep");
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep), PM_CB_OK,
                   "resume succeeds");
    TEST_ASSERT_EQ(s_trace_len, 1u, "wake-only slot was resumed");
    TEST_ASSERT_EQ(s_trace[0], TRACE_WAKE((uint32_t)PM_PRI_INPUT),
                   "the EC-shaped slot came back");

    /* But an ABORTED sleep must NOT wake it: it never slept, and waking it
     * would re-initialise hardware that was never taken down. */
    pm_cb_table_init(&t);
    trace_reset();
    (void)pm_cb_table_register(&t, PM_PRI_USER, (pm_power_callback_t)0,
                               cb_wake_ok, (void *)(uint64_t)PM_PRI_USER,
                               "wakeonly");
    (void)pm_cb_table_register(&t, PM_PRI_INPUT, cb_sleep_fail, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_INPUT, "input");
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep),
                   PM_CB_CALLBACK_FAILED, "the sleep aborts");
    TEST_ASSERT_EQ(rep.unwound, 0u, "nothing quiesced nothing unwound");
    TEST_ASSERT_EQ(s_trace_len, 1u, "wake-only slot stayed put");
    TEST_ASSERT_EQ(s_trace[0], TRACE_SLEEP((uint32_t)PM_PRI_INPUT),
                   "only the failing sleep ran");

    /* A sleep-only registration is refused outright, so there is no way to
     * reach a quiesced slot that nothing can wake. */
    pm_cb_table_init(&t);
    TEST_ASSERT_EQ(pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok,
                                        (pm_power_callback_t)0,
                                        (void *)0, "sleeponly"),
                   PM_CB_INVALID, "sleep-only registration refused");
    TEST_ASSERT_EQ(pm_cb_table_count(&t), 0u, "no slot consumed");
}

static void test_pm_overrun_accounting(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    /* Each now_ns() call advances by 1.5x the sleep budget, so the
     * before/after pair straddles an overrun deterministically. */
    pm_cb_table_init(&t);
    trace_reset();
    s_advance_per_call_ns = (uint64_t)PM_CB_SLEEP_OVERRUN_MS * 1500000ull;
    t.now_ns              = fake_now_ns_advancing;
    (void)pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_USER, "slow");
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep), PM_CB_OK,
                   "an overrun is a diagnostic not a failure");
    TEST_ASSERT_EQ(rep.overruns, 1u, "the overrun was counted");
    TEST_ASSERT_EQ(rep.failed, 0u, "overrun does not fail the callback");

    /* Frozen clock is the control: elapsed is 0, so nothing may be reported.
     * This is also the shape a tick-derived clock takes with interrupts
     * masked, which is why the production entry points refuse that context. */
    pm_cb_table_init(&t);
    trace_reset();
    t.now_ns = fake_now_ns;
    (void)pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_USER, "fast");
    (void)pm_cb_table_notify_sleep(&t, 3u, &rep);
    TEST_ASSERT_EQ(rep.overruns, 0u, "fast callback reports no overrun");

    /* A clock that steps BACKWARD must report 0, never an enormous unsigned
     * difference: the wrapped value would fabricate a multi-year overrun. */
    pm_cb_table_init(&t);
    trace_reset();
    s_fake_ns             = (uint64_t)PM_CB_SLEEP_OVERRUN_MS * 10000000ull;
    s_advance_per_call_ns = 0;
    t.now_ns              = fake_now_ns_backward;
    (void)pm_cb_table_register(&t, PM_PRI_USER, cb_sleep_ok, cb_wake_ok,
                               (void *)(uint64_t)PM_PRI_USER, "backward");
    (void)pm_cb_table_notify_sleep(&t, 3u, &rep);
    TEST_ASSERT_EQ(rep.overruns, 0u, "backward step reports 0 not a wrap");
}

static void test_pm_degenerate_inputs(void)
{
    pm_cb_table_t  t;
    pm_cb_report_t rep;

    pm_cb_table_init(&t);
    trace_reset();
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, &rep), PM_CB_OK,
                   "empty table sleeps successfully");
    TEST_ASSERT_EQ(rep.invoked, 0u, "nothing was invoked");
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, &rep), PM_CB_OK,
                   "and resumes successfully");

    pm_cb_table_init(&t);
    trace_reset();
    seed_all_priorities(&t);
    TEST_ASSERT_EQ(pm_cb_table_notify_sleep(&t, 3u, (pm_cb_report_t *)0),
                   PM_CB_OK, "NULL report accepted not dereferenced");
    TEST_ASSERT_EQ(pm_cb_table_notify_resume(&t, 3u, (pm_cb_report_t *)0),
                   PM_CB_OK, "same on the resume side");

    TEST_ASSERT_EQ(pm_cb_table_notify_sleep((pm_cb_table_t *)0, 3u, &rep),
                   PM_CB_INVALID, "NULL table sleep refused");
    TEST_ASSERT_EQ(pm_cb_table_notify_resume((pm_cb_table_t *)0, 3u, &rep),
                   PM_CB_INVALID, "NULL table resume refused");
    TEST_ASSERT_EQ(pm_cb_table_txn_state((const pm_cb_table_t *)0),
                   PM_TXN_IDLE, "NULL table reports idle");
}

/* ---- Registration ------------------------------------------------------- */

void test_register_pm_callback(void)
{
    test_suite_register_cat("PM: callback registration and bounds",
                            test_pm_registration, TEST_CAT_BOOT);
    test_suite_register_cat("PM: sleep/resume walk ordering",
                            test_pm_walk_ordering, TEST_CAT_BOOT);
    test_suite_register_cat("PM: transaction spans sleep to resume",
                            test_pm_transaction_span, TEST_CAT_BOOT);
    test_suite_register_cat("PM: sleep abort unwinds quiesced slots",
                            test_pm_sleep_abort_unwinds, TEST_CAT_BOOT);
    test_suite_register_cat("PM: failed unwind enters degraded",
                            test_pm_unwind_failure_degrades, TEST_CAT_BOOT);
    test_suite_register_cat("PM: resume failure policy",
                            test_pm_resume_failure_policy, TEST_CAT_BOOT);
    test_suite_register_cat("PM: wake-only and sleep-only slots",
                            test_pm_half_registered_slots, TEST_CAT_BOOT);
    test_suite_register_cat("PM: overrun accounting",
                            test_pm_overrun_accounting, TEST_CAT_BOOT);
    test_suite_register_cat("PM: degenerate inputs",
                            test_pm_degenerate_inputs, TEST_CAT_BOOT);
}
