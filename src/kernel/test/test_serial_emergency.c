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
 *   - the wedged-transmitter budget: N timeouts saturate, a drain returns only
 *     the caller's own reservation, and the count is monotonic in timeouts
 *   - rerouted-write routing: the arming CPU is kept, others dropped, and both
 *     unknown cases fail OPEN
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/serial_emergency.h"
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


/* --- wedged-transmitter budget arithmetic ---------------------------------
 * This arithmetic already regressed once: a revision charged each timeout
 * twice, so the budget saturated after four full waits instead of eight, and
 * an earlier one zeroed the counter on every drained byte, which made the
 * "whole panic" bound meaningless for an intermittently-draining UART. Both
 * were found by review rather than by a test. These pin the contract. */

/* Reserving without returning is what a timeout does. Exactly
 * SERIAL_EMERG_STUCK_BYTES of them must fit, and the next must be refused. */
static void test_serial_emergency_budget_saturates(void)
{
    uint32_t granted = 0;
    uint32_t i;

    serial_emerg_reset_for_test();
    TEST_ASSERT_EQ(serial_emerg_waits(), 0, "budget starts unspent");

    /* 64 attempts is far past any sane ceiling; count how many are granted. */
    for (i = 0; i < 64u; i++) {
        if (!serial_emerg_reserve())
            break;
        granted++;
    }

    TEST_ASSERT_EQ(granted, 8, "exactly 8 full waits are granted before saturation");
    TEST_ASSERT_EQ(serial_emerg_waits(), 8, "charge equals the waits granted");
    TEST_ASSERT_EQ(serial_emerg_reserve(), 0, "a saturated budget refuses");

    serial_emerg_reset_for_test();
}

/* A byte that DRAINS returns its own reservation, so a healthy transmitter
 * never accumulates charge no matter how many bytes it sends. */
static void test_serial_emergency_budget_drain_returns_reservation(void)
{
    uint32_t i;

    serial_emerg_reset_for_test();

    for (i = 0; i < 32u; i++) {
        TEST_ASSERT_EQ(serial_emerg_reserve(), 1, "healthy byte gets an allowance");
        serial_emerg_return();
    }

    TEST_ASSERT_EQ(serial_emerg_waits(), 0,
                   "32 drained bytes leave the budget unspent");

    serial_emerg_reset_for_test();
}

/* The mixed case the reset used to break: a success must return only its OWN
 * reservation, never erase a timeout already charged. */
static void test_serial_emergency_budget_drain_does_not_erase_timeout(void)
{
    serial_emerg_reset_for_test();

    TEST_ASSERT_EQ(serial_emerg_reserve(), 1, "first byte reserves");
    /* ...and times out: no return. */
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "timeout leaves its charge");

    TEST_ASSERT_EQ(serial_emerg_reserve(), 1, "second byte reserves");
    serial_emerg_return();                      /* ...and drains */

    TEST_ASSERT_EQ(serial_emerg_waits(), 1,
                   "a drained byte did not erase the earlier timeout's charge");

    serial_emerg_reset_for_test();
}


/* --- rerouted-write routing (owner vs non-owner vs unknown) ---------------
 * Once the latch is armed, an ordinary serial_write from the panic owner is the
 * crash evidence and must survive; one from a CPU still running user code is
 * stdout and must be discarded before it can take the lock or spend the
 * terminal budget. Getting the unknown cases wrong in the SAFE direction
 * matters most: a dropped panic record is worse than an interleaved one. */
static void test_serial_emergency_routing_owner_vs_other(void)
{
    TEST_ASSERT_EQ(serial_emerg_should_drop_for(3u, 3u), 0,
                   "the arming CPU's own rerouted write is kept");
    TEST_ASSERT_EQ(serial_emerg_should_drop_for(4u, 3u), 1,
                   "another CPU's rerouted write is dropped");
    TEST_ASSERT_EQ(serial_emerg_should_drop_for(0u, 1u), 1,
                   "CPU 0 is not special -- it drops when it is not the owner");
}

/* FAIL OPEN on either unknown: an unidentifiable writer, or no owner recorded
 * (arming happened on a CPU with no per-CPU data), must preserve output. */
static void test_serial_emergency_routing_fails_open(void)
{
    TEST_ASSERT_EQ(serial_emerg_should_drop_for(SERIAL_EMERG_NO_OWNER, 2u), 0,
                   "unidentifiable writer keeps its output");
    TEST_ASSERT_EQ(serial_emerg_should_drop_for(2u, SERIAL_EMERG_NO_OWNER), 0,
                   "no recorded owner keeps every writer's output");
    TEST_ASSERT_EQ(serial_emerg_should_drop_for(SERIAL_EMERG_NO_OWNER,
                                                SERIAL_EMERG_NO_OWNER), 0,
                   "both unknown still fails open");
}


/* The epoch reset can land BETWEEN a reserve and its return: a direct writer
 * reserves before arming, the arming CPU clears the budget, and the byte then
 * drains. An unconditional fetch_sub wrapped to UINT32_MAX there -- permanently
 * saturated, the worst state this budget has. The return must saturate. */
static void test_serial_emergency_budget_return_does_not_underflow(void)
{
    serial_emerg_reset_for_test();

    TEST_ASSERT_EQ(serial_emerg_reserve(), 1, "reserved before the epoch reset");
    serial_emerg_reset_for_test();          /* stands in for arming's clear */
    TEST_ASSERT_EQ(serial_emerg_waits(), 0, "reset cleared the charge");

    serial_emerg_return();                  /* the stale return arrives */
    TEST_ASSERT_EQ(serial_emerg_waits(), 0,
                   "a return with nothing to give back did not wrap");

    /* And the budget must still be fully usable afterwards. */
    TEST_ASSERT_EQ(serial_emerg_reserve(), 1,
                   "the new epoch still grants allowances");
    serial_emerg_return();

    serial_emerg_reset_for_test();
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
    test_suite_register_cat("serial_emergency: budget saturates after 8 waits",
                            test_serial_emergency_budget_saturates,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: drained byte returns its reservation",
                            test_serial_emergency_budget_drain_returns_reservation,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: drain does not erase a timeout charge",
                            test_serial_emergency_budget_drain_does_not_erase_timeout,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: rerouted write routing by owner",
                            test_serial_emergency_routing_owner_vs_other,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: routing fails open when unknown",
                            test_serial_emergency_routing_fails_open,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: return does not underflow past a reset",
                            test_serial_emergency_budget_return_does_not_underflow,
                            TEST_CAT_BOOT);
}
