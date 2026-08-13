/* ============================================================================
 * test_serial_emergency.c -- unit tests for the abort-safe serial acquire /
 * release policy (bare-metal-hardening: abort-safe panic serial).
 *
 * The property under test is the one the emergency path exists to provide:
 * acquisition NEVER blocks. `serial_write_emergency` itself cannot be unit
 * tested (it drives real UART ports and masks interrupts), so the lock policy
 * is split into `serial_emergency_acquire` / `serial_emergency_release`, which
 * operate on a CALLER-SUPPLIED spinlock. Every test below therefore runs
 * against a local fixture lock -- no UART, no g_serial_lock, no boot
 * infrastructure, and no way for a test to arm the one-way emergency latch and
 * degrade serial output for the rest of the boot.
 *
 * Covers:
 *   - free lock       -> acquire returns 1 and the lock is held afterwards
 *   - release(held=1) -> lock is released
 *   - HELD lock       -> acquire RETURNS (does not spin) and reports 0
 *   - release(held=0) -> does NOT release a lock this caller never acquired
 *   - NULL lock       -> acquire reports 0, release is a no-op (no deref)
 *   - the emergency latch is NOT armed during a normal boot
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/drivers/serial.h"
#include "kernel/sched/spinlock.h"

/* A free lock is acquirable, and acquisition actually takes it. */
static void test_serial_emergency_acquire_free_lock(void)
{
    spinlock_t lock = SPINLOCK_INIT;
    int acquired;

    TEST_ASSERT_EQ(spin_is_locked(&lock), 0, "fixture lock starts free");

    acquired = serial_emergency_acquire(&lock);
    TEST_ASSERT_EQ(acquired, 1, "acquire on a free lock reports success");
    TEST_ASSERT_EQ(spin_is_locked(&lock), 1, "acquire actually took the lock");

    serial_emergency_release(&lock, acquired);
    TEST_ASSERT_EQ(spin_is_locked(&lock), 0, "release(acquired=1) frees the lock");
}

/* THE load-bearing test: acquiring a lock somebody else holds must RETURN,
 * reporting failure, rather than spinning. Reaching the assertion at all is
 * half the proof -- a blocking implementation hangs the suite here, which is
 * exactly the panic-path deadlock this section removes. */
static void test_serial_emergency_acquire_held_lock_does_not_block(void)
{
    spinlock_t lock = SPINLOCK_INIT;
    int acquired;

    /* Simulate the interrupted code holding the lock. spin_trylock rather than
     * spin_lock_irqsave: this must not touch IRQL or the interrupt flag inside
     * a test. */
    TEST_ASSERT_EQ(spin_trylock(&lock), 1, "fixture holder took the lock");

    acquired = serial_emergency_acquire(&lock);
    TEST_ASSERT_EQ(acquired, 0, "acquire on a held lock reports failure");
    TEST_ASSERT_EQ(spin_is_locked(&lock), 1, "the original holder still owns it");

    spin_tryunlock(&lock);
    TEST_ASSERT_EQ(spin_is_locked(&lock), 0, "fixture holder released cleanly");
}

/* A caller that did NOT acquire must not release. Getting this wrong would let
 * the emergency writer free a lock the interrupted code still holds, corrupting
 * that code's critical section instead of merely interleaving bytes. */
static void test_serial_emergency_release_without_acquire_is_noop(void)
{
    spinlock_t lock = SPINLOCK_INIT;

    TEST_ASSERT_EQ(spin_trylock(&lock), 1, "fixture holder took the lock");

    serial_emergency_release(&lock, 0);
    TEST_ASSERT_EQ(spin_is_locked(&lock), 1,
                   "release(acquired=0) left the other owner's lock held");

    spin_tryunlock(&lock);
    TEST_ASSERT_EQ(spin_is_locked(&lock), 0, "fixture holder released cleanly");
}

/* NULL must be inert on both halves -- the panic path is the worst possible
 * place to take a #PF out of a diagnostic helper. */
static void test_serial_emergency_null_lock_is_inert(void)
{
    /* A held witness lock stands in for unrelated kernel state: if a NULL
     * release performed a wild write instead of returning, the most likely
     * casualty is a neighbouring lock word. Asserting the witness is untouched
     * proves more than "the call returned" does. */
    spinlock_t witness = SPINLOCK_INIT;

    TEST_ASSERT_EQ(serial_emergency_acquire((spinlock_t *)0), 0,
                   "acquire(NULL) reports failure rather than dereferencing");

    TEST_ASSERT_EQ(spin_trylock(&witness), 1, "witness lock held");

    /* acquired=1 is the hostile combination: a caller claiming it holds a lock
     * that does not exist. */
    serial_emergency_release((spinlock_t *)0, 1);
    serial_emergency_release((spinlock_t *)0, 0);

    TEST_ASSERT_EQ(spin_is_locked(&witness), 1,
                   "release(NULL, ...) left unrelated lock state untouched");

    spin_tryunlock(&witness);
    TEST_ASSERT_EQ(spin_is_locked(&witness), 0, "witness released cleanly");
}

/* The latch is one-way and only a terminal path arms it. If this fails, some
 * boot path called serial_enter_emergency() and every subsequent serial write
 * in the system silently dropped to the unlocked, byte-interleaving path. */
static void test_serial_emergency_latch_not_armed_during_boot(void)
{
    TEST_ASSERT_EQ(serial_in_emergency(), 0,
                   "emergency mode is not armed on a healthy boot");
}

void test_register_serial_emergency(void)
{
    test_suite_register_cat("serial_emergency: free lock is acquired",
                            test_serial_emergency_acquire_free_lock,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: held lock does not block",
                            test_serial_emergency_acquire_held_lock_does_not_block,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: release without acquire is a no-op",
                            test_serial_emergency_release_without_acquire_is_noop,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: NULL lock is inert",
                            test_serial_emergency_null_lock_is_inert,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: latch not armed during boot",
                            test_serial_emergency_latch_not_armed_during_boot,
                            TEST_CAT_BOOT);
}
