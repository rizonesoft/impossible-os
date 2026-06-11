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

void test_register_timer_tick_cb(void)
{
    test_suite_register_cat("Timer tick callback: divisor counting + unregister",
        test_tick_cb_divisor_counting, TEST_CAT_BOOT);
    test_suite_register_cat("Timer tick callback: divisor 0 guard",
        test_tick_cb_divisor_zero_guard, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
