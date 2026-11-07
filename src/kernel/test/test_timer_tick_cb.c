/* ============================================================================
 * test_timer_tick_cb.c -- TEST_CAT_BOOT coverage for the timer tick callback
 *
 * Tests the singleton tick-callback slot semantics in src/kernel/timer.c:
 * divisor counting, unregister retraction, and the divisor==0 guard added
 * by the boot-splash-spinner hardening review (a zero divisor must never
 * fire; the pre-fix code fired on EVERY tick: counter++ >= 0 always true).
 *
 * timer_tick_callback_fire() is a pure counter/dispatch helper -- calling it
 * directly does not touch hardware. The live timer ISR shares the slot, so
 * each test runs its counting under an IRQ-save spinlock to keep the fire
 * count deterministic, and unregisters before restoring interrupts.
 *
 * TEST-SIDE-EFFECT-ALLOWED: spinner_stop() before the slot tests mirrors
 * test_desktop.c -- it clears s_active and unregisters the spinner callback,
 * both pure-state writes. The boot splash spinner is the slot's production
 * owner and would otherwise race these registrations; each test saves
 * spinner_is_active() on entry and re-issues spinner_start() on exit so a
 * test=1 boot keeps its spinner animating after the Boot category runs.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/timer.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"
#include "kernel/sched/dpc.h"
#include "kernel/smp.h"

extern void spinner_stop(void);
extern void spinner_start(void);
extern int spinner_is_active(void);

static volatile uint32_t s_fire_count;

static void counting_cb(void)
{
    s_fire_count++;
}

static spinlock_t s_tick_test_lock = {0};

static void test_tick_cb_divisor_counting(void)
{
    uint64_t irq_flags;
    int spinner_was_active = spinner_is_active();
    uint32_t after_8_fires, after_unregister;

    spinner_stop();

    /* Critical section covers ONLY register/fire/unregister + snapshots --
     * no klog under the lock (TEST_ASSERT logs; spinlock contract forbids
     * printk/blocking calls while held). Assertions run after unlock. */
    spin_lock_irqsave(&s_tick_test_lock, &irq_flags);

    s_fire_count = 0;
    timer_register_tick_callback(counting_cb, 4);
    for (int i = 0; i < 8; i++)
        timer_tick_callback_fire();
    after_8_fires = s_fire_count;

    timer_unregister_tick_callback();
    for (int i = 0; i < 4; i++)
        timer_tick_callback_fire();
    after_unregister = s_fire_count;

    spin_unlock_irqrestore(&s_tick_test_lock, irq_flags);

    TEST_ASSERT_EQ(after_8_fires, 2, "divisor 4: 8 fires -> 2 callback calls");
    TEST_ASSERT_EQ(after_unregister, 2, "unregistered slot never fires");

    /* Restore the production owner: spinner_start() re-registers
     * spinner_advance at 10 ticks if the splash was animating on entry. */
    if (spinner_was_active)
        spinner_start();
}

static void test_tick_cb_divisor_zero_guard(void)
{
    uint64_t irq_flags;
    int spinner_was_active = spinner_is_active();
    uint32_t after_5_fires;

    spinner_stop();

    spin_lock_irqsave(&s_tick_test_lock, &irq_flags);

    s_fire_count = 0;
    timer_register_tick_callback(counting_cb, 0);
    for (int i = 0; i < 5; i++)
        timer_tick_callback_fire();
    after_5_fires = s_fire_count;

    timer_unregister_tick_callback();

    spin_unlock_irqrestore(&s_tick_test_lock, irq_flags);

    TEST_ASSERT_EQ(after_5_fires, 0, "divisor 0 never fires (guard, not every-tick)");

    if (spinner_was_active)
        spinner_start();
}

/* A real tick: the callback must see CLOCK_LEVEL and the DPC it queues must run
 * at DISPATCH_LEVEL, because the handler lowers between the two. A zero IRQL
 * means "not recorded", which no assertion below accepts. */
#define TICK_IRQL_WAIT_TICKS 100u

static KDPC              s_tick_dpc;
static volatile uint32_t s_tick_cb_irql;
static volatile uint32_t s_tick_dpc_irql;
static volatile uint32_t s_tick_dpc_interrupted;
static volatile int      s_tick_dpc_queued;

static void tick_irql_dpc(struct _KDPC *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
    s_tick_dpc_irql = (uint32_t)KeGetCurrentIrql();
}

static void tick_irql_cb(void)
{
    /* The slot fires on every CPU's tick, so only the first one queues. */
    if (__atomic_exchange_n(&s_tick_dpc_queued, 1, __ATOMIC_ACQ_REL))
        return;
    s_tick_cb_irql = (uint32_t)KeGetCurrentIrql();
    s_tick_dpc_interrupted = (uint32_t)smp_this_cpu()->clock_interrupted_irql;
    KeInsertQueueDpc(&s_tick_dpc, (void *)0, (void *)0);
}

static void test_tick_irql_clock_then_dispatch(void)
{
    int spinner_was_active = spinner_is_active();
    uint64_t start;

    spinner_stop();

    s_tick_cb_irql = 0;
    s_tick_dpc_irql = 0;
    s_tick_dpc_interrupted = 0xFFu;
    s_tick_dpc_queued = 0;
    KeInitializeDpc(&s_tick_dpc, tick_irql_dpc, (void *)0);

    timer_register_tick_callback(tick_irql_cb, 1);
    start = system_get_ticks();
    while (!__atomic_load_n(&s_tick_dpc_irql, __ATOMIC_ACQUIRE) &&
           system_get_ticks() - start < TICK_IRQL_WAIT_TICKS)
        __asm__ volatile("pause");
    timer_unregister_tick_callback();

    TEST_ASSERT_EQ(s_tick_cb_irql, (uint32_t)CLOCK_LEVEL, "tick work runs at CLOCK_LEVEL");
    TEST_ASSERT_EQ(s_tick_dpc_interrupted, (uint32_t)PASSIVE_LEVEL,
                   "this test's tick interrupted PASSIVE_LEVEL");
    TEST_ASSERT_EQ(s_tick_dpc_irql, (uint32_t)DISPATCH_LEVEL,
                   "DPC queued by the tick runs at DISPATCH_LEVEL");

    if (spinner_was_active)
        spinner_start();
}

/* The lower must never go below what the tick interrupted, and must refuse the
 * drain above DISPATCH_LEVEL. IRQs off so a real tick cannot share the fields. */
static void test_tick_irql_lower_targets(void)
{
    static const struct {
        KIRQL current, interrupted, expect;
        int   drain;
    } cases[] = {
        { CLOCK_LEVEL, PASSIVE_LEVEL,  DISPATCH_LEVEL, 1 },
        { CLOCK_LEVEL, APC_LEVEL,      DISPATCH_LEVEL, 1 },
        { CLOCK_LEVEL, DISPATCH_LEVEL, DISPATCH_LEVEL, 1 },
        { CLOCK_LEVEL, DIRQL_MIN,      DIRQL_MIN,      0 },
        { CLOCK_LEVEL, CLOCK_LEVEL,    CLOCK_LEVEL,    0 },
        { HIGH_LEVEL,  HIGH_LEVEL,     HIGH_LEVEL,     0 },
    };
    enum { CASE_COUNT = sizeof(cases) / sizeof(cases[0]) };
    KIRQL got[CASE_COUNT];
    int   drain[CASE_COUNT];
    uint64_t irq_flags;
    struct per_cpu_data *pcpu;
    KIRQL saved_irql, saved_interrupted;

    spin_lock_irqsave(&s_tick_test_lock, &irq_flags);
    pcpu = smp_this_cpu();
    saved_irql = pcpu->current_irql;
    saved_interrupted = pcpu->clock_interrupted_irql;
    for (int i = 0; i < CASE_COUNT; i++) {
        pcpu->current_irql = cases[i].current;
        pcpu->clock_interrupted_irql = cases[i].interrupted;
        drain[i] = irql_tick_lower_to_dispatch();
        got[i] = pcpu->current_irql;
    }
    pcpu->clock_interrupted_irql = saved_interrupted;
    pcpu->current_irql = saved_irql;
    spin_unlock_irqrestore(&s_tick_test_lock, irq_flags);

    for (int i = 0; i < CASE_COUNT; i++) {
        TEST_ASSERT_EQ((uint32_t)got[i], (uint32_t)cases[i].expect, "lower target");
        TEST_ASSERT_EQ(drain[i], cases[i].drain, "drain permission");
    }
}

void test_register_timer_tick_cb(void)
{
    test_suite_register_cat("Timer tick: callback at CLOCK_LEVEL, DPC at DISPATCH_LEVEL",
        test_tick_irql_clock_then_dispatch, TEST_CAT_BOOT);
    test_suite_register_cat("Timer tick: lower targets and drain permission",
        test_tick_irql_lower_targets, TEST_CAT_BOOT);
    test_suite_register_cat("Timer tick callback: divisor counting + unregister",
        test_tick_cb_divisor_counting, TEST_CAT_BOOT);
    test_suite_register_cat("Timer tick callback: divisor 0 guard",
        test_tick_cb_divisor_zero_guard, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
