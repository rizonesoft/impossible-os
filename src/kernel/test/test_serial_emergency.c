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
 *   - epoch-tokened reservations: a token from a dead epoch cannot spend the
 *     live epoch's charge, a refusal token is inert, and a same-epoch double
 *     return saturates instead of wrapping
 *   - per-CPU charge attribution: charges are recorded against the reserving
 *     CPU, voided across an epoch, and the refund is bounded by what that CPU
 *     actually holds
 *   - __kread_u8: the guarded caller-string read succeeds on valid kernel
 *     memory and rejects the operands that would #GP rather than #PF
 *   - g_serial_lock ownership: the owner word is paired with the lock across
 *     every acquisition path, and the park-time force-release is owner-scoped
 *   - NMI nesting depth: the signal the panic context predicate needs in order
 *     to see a fault taken INSIDE an NMI handler, which the vector cannot show
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/idt.h"            /* NMI nesting depth under test */
#include "kernel/panic.h"          /* panic_declared_ctx truth table */
#include "kernel/vectors.h"        /* VECTOR_NMI */
#include "kernel/drivers/serial.h"
#include "kernel/drivers/serial_emergency.h"
#include "kernel/sched/spinlock.h"
#include "kernel/cpu_security.h"   /* __kread_u8 + __kstr_read_guarded (under test) */
#include "libc/string.h"           /* memset -- fixture buffer prefill */

/* Two distinct fixture identities. Concrete APIC ids rather than 0/1 so a test
 * that accidentally compared the RAW id against the ENCODED owner would fail
 * instead of coincidentally passing. */
#define TEST_SERIAL_ID_A   7u
#define TEST_SERIAL_ID_B   9u
#define TEST_SERIAL_OWNER_A  SERIAL_LOCK_OWNER_OF(TEST_SERIAL_ID_A)
#define TEST_SERIAL_OWNER_B  SERIAL_LOCK_OWNER_OF(TEST_SERIAL_ID_B)

static int test_serial_lock_held(const serial_lock_t *lock)
{
    return lock->owner != SERIAL_LOCK_FREE;
}

/* A free lock is acquirable, and acquisition takes it AND records the owner in
 * the same word -- there is no second store to observe. */
static void test_serial_emergency_acquire_free_lock(void)
{
    serial_lock_t lock = SERIAL_LOCK_INIT;
    int acquired;

    TEST_ASSERT_EQ(test_serial_lock_held(&lock), 0, "fixture lock starts free");

    acquired = serial_emergency_acquire(&lock, TEST_SERIAL_OWNER_A);
    TEST_ASSERT_EQ(acquired, 1, "acquire on a free lock reports success");
    TEST_ASSERT_EQ(lock.owner, TEST_SERIAL_OWNER_A,
                   "acquire took the lock AND recorded the owner in one word");

    serial_emergency_release(&lock, acquired);
    TEST_ASSERT_EQ(lock.owner, SERIAL_LOCK_FREE,
                   "release(acquired=1) frees the lock and clears the owner");
}

/* THE load-bearing test: acquiring a lock somebody else holds must RETURN,
 * reporting failure, rather than spinning. Reaching the assertion at all is
 * half the proof -- a blocking implementation hangs the suite here, which is
 * exactly the panic-path deadlock this section removes. */
static void test_serial_emergency_acquire_held_lock_does_not_block(void)
{
    serial_lock_t lock = SERIAL_LOCK_INIT;
    int acquired;

    /* Simulate the interrupted code holding the lock, as a DIFFERENT owner. */
    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&lock, TEST_SERIAL_OWNER_B), 1,
                   "fixture holder took the lock");

    acquired = serial_emergency_acquire(&lock, TEST_SERIAL_OWNER_A);
    TEST_ASSERT_EQ(acquired, 0, "acquire on a held lock reports failure");
    TEST_ASSERT_EQ(lock.owner, TEST_SERIAL_OWNER_B,
                   "the original holder still owns it, unchanged");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&lock, TEST_SERIAL_OWNER_B), 1,
                   "fixture holder released cleanly");
}

/* A caller that did NOT acquire must not release. Getting this wrong would let
 * the emergency writer free a lock the interrupted code still holds, corrupting
 * that code's critical section instead of merely interleaving bytes. */
static void test_serial_emergency_release_without_acquire_is_noop(void)
{
    serial_lock_t lock = SERIAL_LOCK_INIT;

    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&lock, TEST_SERIAL_OWNER_B), 1,
                   "fixture holder took the lock");

    serial_emergency_release(&lock, 0);
    TEST_ASSERT_EQ(lock.owner, TEST_SERIAL_OWNER_B,
                   "release(acquired=0) left the other owner's lock held");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&lock, TEST_SERIAL_OWNER_B), 1,
                   "fixture holder released cleanly");
}

/* The force-release path is owner-scoped: a CPU that owns nothing must leave a
 * live holder's lock ALONE. This is the branch that stops a parking CPU from
 * stealing the UART out from under a CPU that is still using it. */
static void test_serial_lock_release_wrong_owner_leaves_holder(void)
{
    serial_lock_t lock = SERIAL_LOCK_INIT;

    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&lock, TEST_SERIAL_OWNER_B), 1,
                   "holder B took the lock");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&lock, TEST_SERIAL_OWNER_A), 0,
                   "A does not own the lock, so its release reports failure");
    TEST_ASSERT_EQ(lock.owner, TEST_SERIAL_OWNER_B,
                   "B's live lock is left exactly as it was");

    /* And a force-release against a FREE lock must not claim it. */
    TEST_ASSERT_EQ(serial_lock_try_release_owned(&lock, TEST_SERIAL_OWNER_B), 1,
                   "B released its own lock");
    TEST_ASSERT_EQ(serial_lock_try_release_owned(&lock, TEST_SERIAL_OWNER_A), 0,
                   "releasing an already-free lock reports failure");
    TEST_ASSERT_EQ(lock.owner, SERIAL_LOCK_FREE, "the lock stayed free");
}

/* Same-owner re-entry must FAIL. This is the panic/NMI shape: an abort taken on
 * a CPU that already holds the lock re-enters the emergency writer and presents
 * the SAME encoded owner. Treating a matching owner as re-entrant success would
 * be a disaster -- the inner release would then free a lock the outer critical
 * section is still inside, permitting concurrent UART access with no signal.
 * The CAS gets this right for free by expecting FREE rather than comparing
 * owners, but nothing proved it until this test. */
static void test_serial_lock_same_owner_reentry_is_refused(void)
{
    serial_lock_t lock = SERIAL_LOCK_INIT;
    int outer, inner;

    outer = serial_emergency_acquire(&lock, TEST_SERIAL_OWNER_A);
    TEST_ASSERT_EQ(outer, 1, "the outer acquisition took the lock");

    inner = serial_emergency_acquire(&lock, TEST_SERIAL_OWNER_A);
    TEST_ASSERT_EQ(inner, 0, "the SAME owner is refused a second acquisition");
    TEST_ASSERT_EQ(lock.owner, TEST_SERIAL_OWNER_A,
                   "the outer ownership is left byte-identical");

    /* The nested caller must honour its own failed result: releasing on a failed
     * acquire is exactly the bug this shape would cause. */
    serial_emergency_release(&lock, inner);
    TEST_ASSERT_EQ(lock.owner, TEST_SERIAL_OWNER_A,
                   "a failed nested acquire releases nothing");

    serial_emergency_release(&lock, outer);
    TEST_ASSERT_EQ(lock.owner, SERIAL_LOCK_FREE,
                   "the original acquisition still releases cleanly");
}

/* The raw-id-to-encoded conversion the real handoff wrapper performs. Every
 * other fixture passes an already-encoded owner, so none of them would notice
 * serial_lock_release_if_owner() being changed to pass a raw id, a different
 * mask, or a different identity source -- and the visible symptom would only be
 * a parking CPU silently failing to hand back the UART. */
static void test_serial_lock_release_if_owner_for_encodes_raw_id(void)
{
    serial_lock_t lock = SERIAL_LOCK_INIT;

    /* Raw id 0 is the case a raw/encoded mix-up breaks first: encoded it is 1,
     * but passed through raw it would read as SERIAL_LOCK_FREE and be refused. */
    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&lock,
                                                 SERIAL_LOCK_OWNER_OF(0u)), 1,
                   "CPU 0 holds the lock");
    TEST_ASSERT_EQ(serial_lock_release_if_owner_for(&lock, 0u), 1,
                   "the wrapper encodes raw id 0 and releases it");
    TEST_ASSERT_EQ(lock.owner, SERIAL_LOCK_FREE, "the lock is free again");

    /* Top of the mask, the other end a wrong mask would break. */
    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(
                       &lock, SERIAL_LOCK_OWNER_OF(SERIAL_LOCK_ID_MASK)), 1,
                   "the highest addressable CPU holds the lock");
    TEST_ASSERT_EQ(serial_lock_release_if_owner_for(&lock, SERIAL_LOCK_ID_MASK),
                   1, "the wrapper encodes the top of the mask and releases it");
    TEST_ASSERT_EQ(lock.owner, SERIAL_LOCK_FREE, "the lock is free again");

    /* A foreign id must still be refused THROUGH the wrapper, not just through
     * the policy call underneath it. */
    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&lock,
                                                 SERIAL_LOCK_OWNER_OF(3u)), 1,
                   "CPU 3 holds the lock");
    TEST_ASSERT_EQ(serial_lock_release_if_owner_for(&lock, 5u), 0,
                   "CPU 5 releases nothing it does not own");
    TEST_ASSERT_EQ(lock.owner, SERIAL_LOCK_OWNER_OF(3u),
                   "CPU 3's live lock is untouched");

    TEST_ASSERT_EQ(serial_lock_release_if_owner_for((serial_lock_t *)0, 3u), 0,
                   "a NULL lock is rejected without dereference");
    TEST_ASSERT_EQ(serial_lock_release_if_owner_for(&lock, 3u), 1,
                   "the real owner released cleanly through the wrapper");
}

/* SERIAL_LOCK_FREE is not a usable identity. It cannot come out of
 * SERIAL_LOCK_OWNER_OF, so it only reaches the policy from a caller that built
 * an owner some other way -- and accepting it would let an "acquire" store the
 * free value, taking the lock and leaving it unlocked at the same time. */
static void test_serial_lock_free_is_not_an_identity(void)
{
    serial_lock_t lock = SERIAL_LOCK_INIT;

    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&lock, SERIAL_LOCK_FREE), 0,
                   "acquiring as the free value is refused");
    TEST_ASSERT_EQ(lock.owner, SERIAL_LOCK_FREE, "the lock was not taken");

    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&lock, TEST_SERIAL_OWNER_A), 1,
                   "a real identity still acquires");
    TEST_ASSERT_EQ(serial_lock_try_release_owned(&lock, SERIAL_LOCK_FREE), 0,
                   "releasing as the free value is refused");
    TEST_ASSERT_EQ(lock.owner, TEST_SERIAL_OWNER_A, "the live holder is intact");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&lock, TEST_SERIAL_OWNER_A), 1,
                   "the real owner released cleanly");
}

/* Every APIC id the mask admits must round-trip to a DISTINCT owner value that
 * is never the free value. An encoding that aliased two ids would let one CPU
 * force-release another's lock; one that produced FREE would make a held lock
 * look free. */
static void test_serial_lock_owner_encoding_is_injective(void)
{
    uint32_t id;

    for (id = 0; id <= SERIAL_LOCK_ID_MASK; id++) {
        TEST_ASSERT_EQ(SERIAL_LOCK_OWNER_OF(id) != SERIAL_LOCK_FREE, 1,
                       "no APIC id encodes to the free value");
        TEST_ASSERT_EQ(SERIAL_LOCK_OWNER_OF(id), id + 1u,
                       "owner encoding is exactly id + 1 across the mask");
    }
    /* The mask is what makes it total: an id above it folds back in range
     * rather than producing a value the lock word could not hold. */
    TEST_ASSERT_EQ(SERIAL_LOCK_OWNER_OF(SERIAL_LOCK_ID_MASK + 1u),
                   SERIAL_LOCK_OWNER_OF(0u),
                   "ids above the mask fold, they do not escape it");
}

/* NULL must be inert on both halves -- the panic path is the worst possible
 * place to take a #PF out of a diagnostic helper. */
static void test_serial_emergency_null_lock_is_inert(void)
{
    /* A held witness lock stands in for unrelated kernel state: if a NULL
     * release performed a wild write instead of returning, the most likely
     * casualty is a neighbouring lock word. Asserting the witness is untouched
     * proves more than "the call returned" does. */
    serial_lock_t witness = SERIAL_LOCK_INIT;

    TEST_ASSERT_EQ(serial_emergency_acquire((serial_lock_t *)0,
                                            TEST_SERIAL_OWNER_A), 0,
                   "acquire(NULL) reports failure rather than dereferencing");
    TEST_ASSERT_EQ(serial_lock_try_release_owned((serial_lock_t *)0,
                                                 TEST_SERIAL_OWNER_A), 0,
                   "release(NULL) reports failure rather than dereferencing");

    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&witness, TEST_SERIAL_OWNER_B),
                   1, "witness lock held");

    /* acquired=1 is the hostile combination: a caller claiming it holds a lock
     * that does not exist. */
    serial_emergency_release((serial_lock_t *)0, 1);
    serial_emergency_release((serial_lock_t *)0, 0);

    TEST_ASSERT_EQ(witness.owner, TEST_SERIAL_OWNER_B,
                   "release(NULL, ...) left unrelated lock state untouched");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&witness, TEST_SERIAL_OWNER_B),
                   1, "witness released cleanly");
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
        if (serial_emerg_reserve() == SERIAL_EMERG_NO_TOKEN)
            break;
        granted++;
    }

    TEST_ASSERT_EQ(granted, 8, "exactly 8 full waits are granted before saturation");
    TEST_ASSERT_EQ(serial_emerg_waits(), 8, "charge equals the waits granted");
    TEST_ASSERT_EQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN,
                   "a saturated budget refuses");

    serial_emerg_reset_for_test();
}

/* A byte that DRAINS returns its own reservation, so a healthy transmitter
 * never accumulates charge no matter how many bytes it sends. */
static void test_serial_emergency_budget_drain_returns_reservation(void)
{
    uint32_t i;

    serial_emerg_reset_for_test();

    for (i = 0; i < 32u; i++) {
        uint32_t token = serial_emerg_reserve();
        TEST_ASSERT_NEQ(token, SERIAL_EMERG_NO_TOKEN,
                       "healthy byte gets an allowance");
        serial_emerg_return(token);
    }

    TEST_ASSERT_EQ(serial_emerg_waits(), 0,
                   "32 drained bytes leave the budget unspent");

    serial_emerg_reset_for_test();
}

/* The mixed case the reset used to break: a success must return only its OWN
 * reservation, never erase a timeout already charged. */
static void test_serial_emergency_budget_drain_does_not_erase_timeout(void)
{
    uint32_t second;

    serial_emerg_reset_for_test();

    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN,
                   "first byte reserves");
    /* ...and times out: no return. */
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "timeout leaves its charge");

    second = serial_emerg_reserve();
    TEST_ASSERT_NEQ(second, SERIAL_EMERG_NO_TOKEN, "second byte reserves");
    serial_emerg_return(second);                /* ...and drains */

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


/* Returning a token whose slot is ALREADY free must not corrupt the field --
 * the accounting must stay usable rather than wrapping to a saturated state.
 *
 * This pins the defensive behavior only. It deliberately does NOT assert that a
 * token is idempotent, because it is not: a slot index is handed straight back
 * out by the next reserve, so returning a stale token AFTER its slot has been
 * reallocated would release the new holder's charge. Tokens are single-use by
 * contract (see serial_emerg_return); this test covers the no-reallocation case
 * a defensive check can actually catch. */
static void test_serial_emergency_stale_return_does_not_corrupt_budget(void)
{
    uint32_t token;

    serial_emerg_reset_for_test();

    token = serial_emerg_reserve();
    TEST_ASSERT_NEQ(token, SERIAL_EMERG_NO_TOKEN, "reserved one allowance");
    serial_emerg_return(token);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0, "the return gave the charge back");

    serial_emerg_return(token);             /* slot already free, none reissued */
    TEST_ASSERT_EQ(serial_emerg_waits(), 0,
                   "a return with nothing to give back left the field at zero");

    /* And the budget must still be fully usable afterwards -- a wrap would show
     * up here as a refusal, since a saturated field grants nothing. */
    token = serial_emerg_reserve();
    TEST_ASSERT_NEQ(token, SERIAL_EMERG_NO_TOKEN,
                   "the budget still grants allowances");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "and charges exactly one for it");
    serial_emerg_return(token);

    serial_emerg_reset_for_test();
}

/* THE SECTION 17 REGRESSION TEST for the wedged-UART reservation.
 *
 * The hole: a reservation taken before an epoch is published, returned after it,
 * decremented the NEW epoch's charge -- so a panic could be granted up to a full
 * extra ceiling of full-length UART waits beyond the two-phase bound. The token
 * names the epoch its charge landed in, and a return validates it, so a stale
 * return now finds a bumped generation and does nothing. */
static void test_serial_emergency_stale_token_cannot_spend_new_epoch(void)
{
    uint32_t stale, fresh;

    serial_emerg_reset_for_test();

    stale = serial_emerg_reserve();             /* epoch A */
    TEST_ASSERT_NEQ(stale, SERIAL_EMERG_NO_TOKEN, "epoch A granted an allowance");

    serial_emerg_reset_for_test();              /* stands in for arming's clear */
    TEST_ASSERT_EQ(serial_emerg_waits(), 0, "the new epoch starts unspent");

    fresh = serial_emerg_reserve();             /* epoch B charges 1 */
    TEST_ASSERT_NEQ(fresh, SERIAL_EMERG_NO_TOKEN, "epoch B granted an allowance");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "epoch B holds its own charge");

    serial_emerg_return(stale);                 /* the stale return arrives */
    TEST_ASSERT_EQ(serial_emerg_waits(), 1,
                   "a token from a dead epoch does not spend the live one");

    serial_emerg_return(fresh);                 /* the live one still works */
    TEST_ASSERT_EQ(serial_emerg_waits(), 0,
                   "a live token still returns its own charge");

    serial_emerg_reset_for_test();
}

/* An invalid token is inert. Reserve returns SERIAL_EMERG_NO_TOKEN when it
 * refuses, and that value must never decrement anything -- otherwise a caller
 * that was denied an allowance could still credit one back. */
static void test_serial_emergency_no_token_return_is_inert(void)
{
    uint32_t token;

    serial_emerg_reset_for_test();

    token = serial_emerg_reserve();
    TEST_ASSERT_NEQ(token, SERIAL_EMERG_NO_TOKEN, "reserved one allowance");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "the charge is held");

    serial_emerg_return(SERIAL_EMERG_NO_TOKEN);
    TEST_ASSERT_EQ(serial_emerg_waits(), 1,
                   "returning a refusal token changes nothing");

    serial_emerg_reset_for_test();
}

/* Per-CPU charge attribution. The async-isolation refund needs to know what ITS
 * dump spent, and the global charge cannot say: a delta across two reads of a
 * shared counter credits another CPU's concurrent charge to whoever measured
 * last. These counters are per writer, so the difference is exact. */
static void test_serial_emergency_charges_are_attributed_per_cpu(void)
{
    uint32_t t1, t2;

    serial_emerg_reset_for_test();
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0,
                   "a fresh epoch holds no charges for this CPU");

    t1 = serial_emerg_reserve();
    t2 = serial_emerg_reserve();
    TEST_ASSERT_NEQ(t1, SERIAL_EMERG_NO_TOKEN, "first reserve granted");
    TEST_ASSERT_NEQ(t2, SERIAL_EMERG_NO_TOKEN, "second reserve granted");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 2,
                   "both reserves are recorded against this CPU");

    serial_emerg_return(t1);
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1,
                   "a return drops this CPU's held count");
    serial_emerg_return(t2);
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0,
                   "returning both leaves this CPU holding nothing");

    serial_emerg_reset_for_test();
}

/* A new epoch voids this CPU's recorded charges, because the publishing clear
 * erased them from the global budget too. Without this the refund would hand
 * back charges that no longer exist, replenishing a live panic's allowance. */
static void test_serial_emergency_charges_reset_across_epoch(void)
{
    serial_emerg_reset_for_test();

    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN, "charge taken");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1, "this CPU holds it");

    serial_emerg_reset_for_test();
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0,
                   "a new epoch voids this CPU's old charges");
}

/* The refund is BOUNDED by what this CPU actually holds, so asking for more than
 * it took cannot drain a concurrent writer's charge out of the shared budget. */
static void test_serial_emergency_refund_self_is_bounded(void)
{
    serial_emerg_reset_for_test();

    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN, "one charge taken");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "budget shows the one charge");

    serial_emerg_refund_self(8u);           /* ask for far more than held */
    TEST_ASSERT_EQ(serial_emerg_waits(), 0,
                   "the refund gave back exactly the charge this CPU held");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0, "and holds nothing after");

    /* Refunding again must not go negative or credit the budget. */
    serial_emerg_refund_self(8u);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0, "a second refund credits nothing");

    serial_emerg_reset_for_test();
}

/* --- guarded caller-string read ------------------------------------------
 * __kread_u8 is what lets the emergency writer and panic_collect_evidence walk
 * a caller string whose pointer may itself be the corruption being reported.
 * The live unmapped-page recovery runs through page_fault_handler and is
 * serial-validated (it needs a real #PF); what is unit-testable is the
 * success path and the up-front rejections that must NOT reach the load,
 * because a non-canonical operand raises #GP, which a #PF fixup cannot catch. */
static void test_serial_emergency_kread_u8_reads_valid_bytes(void)
{
    const char src[4] = { 'O', 'K', '\0', 'x' };
    uint8_t     b     = 0xFFu;

    TEST_ASSERT_EQ((uint32_t)__kread_u8(&b, &src[0]), 0u,
                   "__kread_u8 on a valid kernel address succeeds");
    TEST_ASSERT_EQ((uint32_t)b, (uint32_t)'O', "and returns the byte at it");

    TEST_ASSERT_EQ((uint32_t)__kread_u8(&b, &src[2]), 0u,
                   "reading the terminator succeeds");
    TEST_ASSERT_EQ((uint32_t)b, 0u, "and reports it as zero, ending a walk");
}

/* --- bounded guarded string read (section 24) ----------------------------
 * __kstr_read_guarded is the same guarantee over a whole string in ONE
 * protected loop: the panic collector copied ~2,900 bytes through a call per
 * byte before its record was durable. As with __kread_u8 the live #PF recovery
 * is serial-validated; what is unit-testable is the copy semantics, the
 * terminator discipline, and the four stop reasons -- and the stop reason is
 * load-bearing, because a walk that clipped at the canonical boundary and
 * reported a clean terminator would render a corrupt pointer as complete text. */
static void test_kstr_read_guarded_copies_and_terminates(void)
{
    char dst[8];
    uint32_t stop = 0xFFu;

    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, "abc", sizeof dst, &stop), 3u,
                   "copies the payload length, excluding the terminator");
    TEST_ASSERT_EQ(stop, KSTR_STOP_NUL, "and reports the source terminator");
    TEST_ASSERT_EQ((uint32_t)dst[0], (uint32_t)'a', "first byte copied");
    TEST_ASSERT_EQ((uint32_t)dst[2], (uint32_t)'c', "last byte copied");
    TEST_ASSERT_EQ((uint32_t)dst[3], 0u, "destination is NUL-terminated");
    TEST_ASSERT_EQ((uint32_t)dst[4], (uint32_t)'Z',
                   "and nothing past the terminator is touched");

    /* An empty source stores nothing but must still terminate: the panic path
     * distinguishes this from unreadable by the stop reason, not by length. */
    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, "", sizeof dst, &stop), 0u,
                   "an empty source copies nothing");
    TEST_ASSERT_EQ(stop, KSTR_STOP_NUL, "and is reported as a clean terminator");
    TEST_ASSERT_EQ((uint32_t)dst[0], 0u, "with the destination terminated");
}

static void test_kstr_read_guarded_bounds_the_copy(void)
{
    char dst[4];
    uint32_t stop = 0xFFu;

    /* cap is the FULL destination size and the terminator is reserved inside
     * it, so a longer source yields cap-1 payload bytes and never overruns. */
    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, "abcdef", sizeof dst, &stop), 3u,
                   "a longer source is truncated to cap-1 payload bytes");
    TEST_ASSERT_EQ(stop, KSTR_STOP_CAP, "and reports budget exhaustion");
    TEST_ASSERT_EQ((uint32_t)dst[3], 0u, "the last byte is the terminator");

    /* cap 1 leaves room for the terminator only. */
    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, "abc", 1u, &stop), 0u,
                   "cap 1 copies no payload");
    TEST_ASSERT_EQ((uint32_t)dst[0], 0u, "but still terminates");
    TEST_ASSERT_EQ((uint32_t)dst[1], (uint32_t)'Z', "without touching dst[1]");

    /* cap 0 has nowhere to put a terminator, so it must write NOTHING. */
    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, "abc", 0u, &stop), 0u,
                   "cap 0 copies nothing");
    TEST_ASSERT_EQ((uint32_t)dst[0], (uint32_t)'Z',
                   "and writes no terminator it has no room for");
}

/* The stop reason must separate UNREADABLE from TERMINATED. A non-canonical
 * source is rejected before the load (a #GP there is not recoverable by a #PF
 * fixup), exactly as __kread_u8 rejects it -- and it must surface as NONCANON,
 * not as a clean empty string, or the panic path prints "no reason given" for a
 * pointer that was part of the corruption. */
static void test_kstr_read_guarded_reports_unreadable(void)
{
    char dst[8];
    uint32_t stop = 0xFFu;

    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, (const char *)0, sizeof dst, &stop),
                   0u, "a NULL source copies nothing");
    TEST_ASSERT_EQ(stop, KSTR_STOP_NONCANON,
                   "and is reported unreadable, not as a terminator");
    TEST_ASSERT_EQ((uint32_t)dst[0], 0u, "with the destination still terminated");

    /* Non-canonical: the middle of the 4-level hole. Same address family the
     * __kread_u8 rejection test uses. */
    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, (const char *)0x0000800000000000ull,
                                       sizeof dst, &stop), 0u,
                   "a non-canonical source copies nothing");
    TEST_ASSERT_EQ(stop, KSTR_STOP_NONCANON, "and reports NONCANON");

    /* stop is optional -- the collector passes NULL because a truncated field
     * in a forensic record IS the report. That must not fault. */
    memset(dst, 'Z', sizeof dst);
    TEST_ASSERT_EQ(__kstr_read_guarded(dst, "hi", sizeof dst, (uint32_t *)0), 2u,
                   "a NULL stop pointer is accepted");
}

/* The canonical-half CLIP, which is the half of the primitive a mapped-address
 * test cannot reach: a source whose RANGE runs off the end of its half has a
 * canonical base and a non-canonical interior, and the walk must stop at the
 * boundary and SAY so -- a silent clip would render a corrupt pointer as a
 * complete string, the one outcome the per-byte walk never produced. Exercising
 * it through the loop would need an address that is both on a half edge and
 * mapped, which no test can arrange, so the arithmetic is tested directly (same
 * reasoning as the fixup-lookup case below). */
static void test_kstr_read_budget_clips_at_the_canonical_half(void)
{
    char probe[8];
    int  clipped = 0xFF;

    /* An ordinary kernel address: cap binds, nothing is clipped. */
    TEST_ASSERT_EQ(kstr_read_budget(probe, sizeof probe, &clipped), 7u,
                   "an ordinary source gets the full cap-1 payload budget");
    TEST_ASSERT_EQ((uint32_t)clipped, 0u, "and is not reported as clipped");

    /* The LAST address of the low half: exactly one byte remains before the
     * canonical hole, so the budget must be 1 and the clip must be reported. */
    TEST_ASSERT_EQ(kstr_read_budget((const void *)0x00007FFFFFFFFFFFull, 8u,
                                    &clipped), 1u,
                   "the last low-half address leaves a one-byte budget");
    TEST_ASSERT_EQ((uint32_t)clipped, 1u, "and reports the clip");

    /* Four bytes short of the hole: the clip still binds, at 5. */
    TEST_ASSERT_EQ(kstr_read_budget((const void *)0x00007FFFFFFFFFFBull, 8u,
                                    &clipped), 5u,
                   "a source near the hole is clipped to what remains in-half");
    TEST_ASSERT_EQ((uint32_t)clipped, 1u, "and reports the clip");

    /* Five bytes short with cap 6 (budget 5): the half boundary and the cap
     * bind at the same value, and that is NOT a clip -- reporting one would
     * turn an ordinary truncation into an "unreadable" verdict. */
    TEST_ASSERT_EQ(kstr_read_budget((const void *)0x00007FFFFFFFFFFBull, 6u,
                                    &clipped), 5u,
                   "cap and boundary agreeing yields that budget");
    TEST_ASSERT_EQ((uint32_t)clipped, 0u, "and is not a clip");

    /* THE TOP OF THE ADDRESS SPACE. The high half ends at UINT64_MAX, so a
     * budget computed as (end - a) with an exclusive end would wrap to a huge
     * value here and hand the loop an unbounded walk. */
    TEST_ASSERT_EQ(kstr_read_budget((const void *)~(uint64_t)0, 8u, &clipped),
                   1u, "the top address yields one byte, not a wrapped budget");
    TEST_ASSERT_EQ((uint32_t)clipped, 1u, "and reports the clip");

    /* The BASE of the high half has the whole upper half ahead of it, so cap
     * binds -- the same arithmetic must not mistake a half start for an end. */
    TEST_ASSERT_EQ(kstr_read_budget((const void *)0xFFFF800000000000ull, 8u,
                                    &clipped), 7u,
                   "the high-half base gets the full budget");
    TEST_ASSERT_EQ((uint32_t)clipped, 0u, "and is not clipped");

    /* cap 0 has no payload and no terminator slot. */
    TEST_ASSERT_EQ(kstr_read_budget(probe, 0u, &clipped), 0u, "cap 0 -> 0");
    TEST_ASSERT_EQ((uint32_t)clipped, 0u, "and is not a clip");
}

/* The exception-table routing decision, testable without provoking a real #PF
 * (same reasoning as the __kread_u8 lookup case): a mistyped label or a dropped
 * direction check would still boot, and the loop would either stop recovering
 * or start swallowing unrelated kernel WRITE faults. */
static void test_kstr_read_fixup_lookup_routes_reads_only(void)
{
    extern char __kstr_read_fault[], __kstr_read_fixup[];
    uint64_t fault = (uint64_t)(uintptr_t)__kstr_read_fault;
    uint64_t out   = 0u;

    TEST_ASSERT_EQ((uint32_t)kstr_read_fixup_lookup(fault, 0, &out), 1u,
                   "a read fault at the guarded load is ours");
    TEST_ASSERT_EQ(out, (uint64_t)(uintptr_t)__kstr_read_fixup,
                   "and routes to this loop's own fixup");

    out = 0u;
    TEST_ASSERT_EQ((uint32_t)kstr_read_fixup_lookup(fault, 1, &out), 0u,
                   "a WRITE fault at that RIP is not ours (dst is trusted)");
    TEST_ASSERT_EQ(out, 0u, "and leaves the caller's fixup untouched");

    TEST_ASSERT_EQ((uint32_t)kstr_read_fixup_lookup(fault + 1u, 0, &out), 0u,
                   "a read fault at any other RIP is not ours");
    TEST_ASSERT_EQ((uint32_t)kstr_read_fixup_lookup(fault, 0, (uint64_t *)0), 0u,
                   "a NULL fixup out-parameter is rejected");

    /* The two guarded loads must not share a label: page_fault_handler matches
     * the EXACT faulting instruction, so an overlap would route one primitive's
     * fault into the other's fixup. */
    TEST_ASSERT_EQ((uint32_t)kread_u8_fixup_lookup(fault, 0, &out), 0u,
                   "the byte-load lookup does not claim the string-loop label");
}

/* The refund must leave charges it was NOT asked for standing. Production passes
 * only the delta its own dump spent, so a refund that discarded everything this
 * CPU held would silently replenish the terminal wait allowance on a survivable
 * panic -- and the "ask for more than held" test alone cannot see that, because
 * it would pass for a function that simply dropped all charges. */
static void test_serial_emergency_refund_self_preserves_unrequested(void)
{
    serial_emerg_reset_for_test();

    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN, "charge 1");
    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN, "charge 2");
    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN, "charge 3");
    TEST_ASSERT_EQ(serial_emerg_waits(), 3, "three charges outstanding");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 3, "all three held by this CPU");

    serial_emerg_refund_self(0u);
    TEST_ASSERT_EQ(serial_emerg_waits(), 3, "refunding zero gives nothing back");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 3, "and holds all three still");

    serial_emerg_refund_self(1u);
    TEST_ASSERT_EQ(serial_emerg_waits(), 2, "a partial refund returns exactly one");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 2,
                   "the other two charges survive the partial refund");

    serial_emerg_reset_for_test();
}

/* THE per-CPU isolation property, proved on one CPU via the ledger-identity
 * seam. Without this every reserve/refund in the suite runs under one identity,
 * so a global counter would pass every other test in this file. */
static void test_serial_emergency_refund_isolated_between_cpu_ids(void)
{
    serial_emerg_reset_for_test();

    serial_emerg_set_ledger_id_for_test(1u);
    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN, "CPU 1 charges");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1, "CPU 1 holds its charge");

    serial_emerg_set_ledger_id_for_test(2u);
    TEST_ASSERT_NEQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN, "CPU 2 charges");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1,
                   "CPU 2 holds only its own charge, not CPU 1's");
    TEST_ASSERT_EQ(serial_emerg_waits(), 2, "the global budget shows both");

    /* CPU 2 refunds generously. CPU 1's charge must not be touched. */
    serial_emerg_refund_self(8u);
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0, "CPU 2 gave back its own charge");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1,
                   "CPU 1's charge survived another CPU's refund");

    serial_emerg_set_ledger_id_for_test(1u);
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1,
                   "CPU 1 still holds exactly what it reserved");

    serial_emerg_set_ledger_id_for_test(SERIAL_EMERG_NO_OWNER);
    serial_emerg_reset_for_test();
}

/* The guarded read is OPT-IN. Written as a mutation test: flipping the
 * implementation to `ctx != PANIC_CTX_NMI` -- the natural-looking form -- would
 * route UNKNOWN callers through the IRETQ-based fixup and reintroduce the
 * nested-NMI IST2 hazard, and this is the assertion that catches it. */
static void test_serial_emergency_guarded_read_context_is_opt_in(void)
{
    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(PANIC_CTX_NORMAL), 1,
                   "the ordinary panic / #DF / #MC context may use the guarded read");
    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(PANIC_CTX_NMI), 0,
                   "NMI context must not: the fixup IRETQ re-arms NMI delivery");
    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(PANIC_CTX_UNKNOWN), 0,
                   "an undeclared context takes the restrictive answer");
    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(3u), 0,
                   "an unrecognized context value degrades safely");
    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(0xFFFFFFFFu), 0,
                   "and so does a wild one");
}

/* The exception-table wiring for the guarded read. The live fault-and-resume
 * path needs a real #PF and stays serial-validated; what is pinned here is the
 * routing DECISION, which would otherwise rot silently -- a mistyped label stops
 * recovery, and a dropped direction check starts swallowing unrelated kernel
 * write faults as if they were ours. */
static void test_serial_emergency_kread_u8_fixup_routing(void)
{
    extern char __kread_u8_fault[], __kread_u8_fixup[];
    uint64_t fault_rip = (uint64_t)(uintptr_t)__kread_u8_fault;
    uint64_t out       = 0u;

    TEST_ASSERT_EQ((uint32_t)kread_u8_fixup_lookup(fault_rip, 0, &out), 1u,
                   "a READ fault at the guarded load is ours");
    TEST_ASSERT_EQ(out, (uint64_t)(uintptr_t)__kread_u8_fixup,
                   "and it routes to that load's own fixup label");

    out = 0u;
    TEST_ASSERT_EQ((uint32_t)kread_u8_fixup_lookup(fault_rip, 1, &out), 0u,
                   "a WRITE fault at the same RIP is NOT ours");
    TEST_ASSERT_EQ(out, 0u, "and leaves the fixup output untouched");

    TEST_ASSERT_EQ((uint32_t)kread_u8_fixup_lookup(fault_rip + 1u, 0, &out), 0u,
                   "a read fault at any other RIP is not ours");
    TEST_ASSERT_EQ((uint32_t)kread_u8_fixup_lookup(fault_rip, 0, (uint64_t *)0), 0u,
                   "a NULL output pointer is rejected rather than dereferenced");
}

/* Slot IDENTITY, not just slot COUNT. Every other accounting test here would
 * still pass if reserve handed out the wrong slot or return cleared a
 * neighbour's, because popcounts do not change. This creates a HOLE among
 * occupied slots and pins which token comes back. */
static void test_serial_emergency_slot_reuse_picks_the_freed_slot(void)
{
    uint32_t t0, t1, t2, again;

    serial_emerg_reset_for_test();

    t0 = serial_emerg_reserve();
    t1 = serial_emerg_reserve();
    t2 = serial_emerg_reserve();
    TEST_ASSERT_EQ(serial_emerg_waits(), 3, "three slots occupied");
    TEST_ASSERT_NEQ(t0, t1, "each reservation gets a distinct token");
    TEST_ASSERT_NEQ(t1, t2, "and they keep being distinct");

    /* Free the MIDDLE one, leaving a hole between two live charges. */
    serial_emerg_return(t1);
    TEST_ASSERT_EQ(serial_emerg_waits(), 2, "exactly one slot was freed");

    again = serial_emerg_reserve();
    TEST_ASSERT_EQ(again, t1, "the freed slot is the one reissued");
    TEST_ASSERT_EQ(serial_emerg_waits(), 3, "and it is charged again");

    /* The neighbours must be untouched and still individually returnable. */
    serial_emerg_return(t0);
    TEST_ASSERT_EQ(serial_emerg_waits(), 2, "returning t0 released exactly one");
    serial_emerg_return(t2);
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "returning t2 released exactly one");
    serial_emerg_return(again);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0, "all slots accounted for");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0, "and the ledger agrees");

    serial_emerg_reset_for_test();
}

/* A malformed token must be rejected, not shifted by. The slot field is 8 bits
 * wide, so a corrupt token can name slot 32..255; `1u << 32` is undefined in C
 * and on x86 the count masks to 5 bits, which would turn slot 32 into bit 0 and
 * clear a LIVE charge while looking like a rejected token. */
static void test_serial_emergency_malformed_token_is_rejected(void)
{
    uint32_t held;
    uint32_t bad;

    serial_emerg_reset_for_test();

    held = serial_emerg_reserve();
    TEST_ASSERT_NEQ(held, SERIAL_EMERG_NO_TOKEN, "one real charge is held");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1, "and the budget shows it");

    /* Slot 8 is past the ceiling; 32 and 255 are the shift-aliasing cases.
     *
     * The bad token REUSES held's generation and replaces only the slot byte.
     * Building it as TOKEN_VALID | bad would encode generation 0, and since the
     * suite resets the epoch repeatedly, `held` carries a nonzero generation --
     * so the generation check would reject every bad token before the shift was
     * ever reached, and this test would pass just as happily against the
     * unfixed code. Keeping the live generation is what makes slots 32 and 255
     * actually target the held slot under the buggy shift. */
    for (bad = 8u; bad != 0u; bad = (bad == 8u) ? 31u :
                                    (bad == 31u) ? 32u :
                                    (bad == 32u) ? 255u : 0u) {
        serial_emerg_return((held & ~(uint32_t)0xFFu) | bad);
        TEST_ASSERT_EQ(serial_emerg_waits(), 1,
                       "a token naming a nonexistent slot released nothing");
        TEST_ASSERT_EQ(serial_emerg_charges_self(), 1,
                       "and left the ledger alone");
    }

    /* A NONZERO token with the VALID bit cleared. Every other refusal case
     * above keeps VALID set, and the "no allowance" token is zero, so a
     * regression from testing `token & SERIAL_EMERG_TOKEN_VALID` to testing
     * `token != 0` would pass all of them -- and would then let a token
     * carrying a live generation and a live slot release a real charge,
     * lengthening the stall budget by a full-length wait. */
    serial_emerg_return(held & ~SERIAL_EMERG_TOKEN_VALID);
    TEST_ASSERT_EQ(serial_emerg_waits(), 1,
                   "a nonzero token without the VALID bit releases nothing");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1,
                   "and the charge stays attributed to this CPU");

    serial_emerg_return(held);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0, "the real token still works");

    serial_emerg_reset_for_test();
}

/* The OFF-to-INIT-to-ARMED transitions, tested through the pure helpers that
 * serial_enter_emergency itself uses -- the live latch is one-way and arming it
 * in a test would degrade serial output for the rest of the boot.
 *
 * The generation-preservation assertion is the load-bearing one: an earlier
 * revision dropped the generation in the claim, which rewound epoch identity and
 * let a deliberately-invalidated token return a live panic's charge. No
 * reset-based test can reach that transition. */
static void test_serial_emergency_latch_transitions_preserve_generation(void)
{
    /* An OFF word carrying generation 5. The reserved span that once held the
     * slot bitmap is set too, so a transition that leaked it back into a live
     * field would be visible rather than silently harmless. */
    uint32_t off   = (5u << 12) | (3u << 20);
    uint32_t claim = serial_emerg_claim_word(off, 7u);
    uint32_t armed;

    TEST_ASSERT_EQ((claim >> 12) & 0xFFu, 5u,
                   "OFF->INIT PRESERVES the generation, never rewinds it");
    TEST_ASSERT_EQ(claim & 0xFFu, 7u, "and records the claiming owner");
    TEST_ASSERT_EQ(claim & 0xC0000000u, 0x40000000u, "and the state is INIT");
    TEST_ASSERT_EQ(claim & 0x0FF00000u, 0u,
                   "and leaves the reserved span clear rather than carrying it");

    armed = serial_emerg_publish_word(claim);
    TEST_ASSERT_EQ((armed >> 12) & 0xFFu, 6u,
                   "INIT->ARMED ADVANCES the generation exactly once");
    TEST_ASSERT_EQ(armed & 0xFFu, 7u, "while keeping the owner immutable");
    TEST_ASSERT_EQ(armed & 0xC0000000u, 0x80000000u, "and the state is ARMED");
    TEST_ASSERT_EQ(armed & 0x0FF00000u, 0u, "and the reserved span stays clear");
}

/* THE ALLOWANCE IS RELEASED BY THE GENERATION BUMP, NOT BY A SECOND PASS.
 *
 * This is the property that replaced the explicit slot-bitmap clear: every
 * outstanding claim carries the epoch it was taken in, so advancing the
 * generation invalidates all of them at once. Asserted through the reset hook,
 * which starts a new epoch exactly as the publishing compare-exchange does. */
static void test_serial_emergency_epoch_bump_releases_every_allowance(void)
{
    uint32_t held[SERIAL_EMERG_STUCK_BYTES];
    uint32_t i, n = 0;

    serial_emerg_reset_for_test();

    while (n < SERIAL_EMERG_STUCK_BYTES) {
        uint32_t t = serial_emerg_reserve();
        if (t == SERIAL_EMERG_NO_TOKEN) break;
        held[n++] = t;
    }
    TEST_ASSERT_EQ(n, SERIAL_EMERG_STUCK_BYTES,
                   "every advertised allowance can actually be reserved");
    TEST_ASSERT_EQ(serial_emerg_waits(), SERIAL_EMERG_STUCK_BYTES,
                   "and each one reads back as an outstanding charge");
    TEST_ASSERT_EQ(serial_emerg_reserve(), SERIAL_EMERG_NO_TOKEN,
                   "a saturated budget refuses rather than exceeding the ceiling");

    serial_emerg_reset_for_test();
    TEST_ASSERT_EQ(serial_emerg_waits(), 0u,
                   "one generation bump releases every allowance at once");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0u,
                   "and leaves nothing attributed to this CPU");

    /* Stale claims must be RECLAIMABLE, not merely uncounted: the words are
     * deliberately never cleared, so a reserve that could not take them over
     * would strand the whole budget after the first epoch. */
    for (i = 0; i < n; i++)
        serial_emerg_return(held[i]);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0u,
                   "and a token from the dead epoch releases nothing");

    n = 0;
    while (n < SERIAL_EMERG_STUCK_BYTES) {
        uint32_t t = serial_emerg_reserve();
        if (t == SERIAL_EMERG_NO_TOKEN) break;
        held[n++] = t;
    }
    TEST_ASSERT_EQ(n, SERIAL_EMERG_STUCK_BYTES,
                   "the full budget is reclaimable in the fresh epoch");

    for (i = 0; i < n; i++)
        serial_emerg_return(held[i]);
    serial_emerg_reset_for_test();
}

/* EVERY ALLOWANCE IS REACHABLE, not just the lowest free one.
 *
 * A reserve that stopped at the first slot it could not take would report the
 * budget exhausted while most of it sat idle. Driven here by holding a claim on
 * slot 0 and requiring the remaining allowances to still be issued. */
static void test_serial_emergency_reserve_sweeps_past_a_held_slot(void)
{
    uint32_t first, t, n = 0;
    uint32_t held[SERIAL_EMERG_STUCK_BYTES];

    serial_emerg_reset_for_test();

    first = serial_emerg_reserve();
    TEST_ASSERT_EQ(first & SERIAL_EMERG_TOKEN_VALID, SERIAL_EMERG_TOKEN_VALID,
                   "the first reservation succeeds");
    TEST_ASSERT_EQ(first & 0xFFu, 0u, "and takes the lowest allowance");

    while ((t = serial_emerg_reserve()) != SERIAL_EMERG_NO_TOKEN)
        held[n++] = t;

    TEST_ASSERT_EQ(n, SERIAL_EMERG_STUCK_BYTES - 1u,
                   "the held slot costs exactly one allowance, not the rest");
    TEST_ASSERT_EQ(serial_emerg_waits(), SERIAL_EMERG_STUCK_BYTES,
                   "and the budget reads fully spent, never short");

    serial_emerg_return(first);
    while (n--)
        serial_emerg_return(held[n]);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0u, "returning them all clears the budget");
    serial_emerg_reset_for_test();
}

/* A RETURN CANNOT CROSS CPUS. The claim records an owner, so a token handed to a
 * different CPU releases nothing -- otherwise one CPU could hand back a charge
 * attributed to another and put the ceiling back out by a full-length wait,
 * which is the failure the composite claim exists to make impossible. */
static void test_serial_emergency_return_rejects_a_foreign_owner(void)
{
    uint32_t token;

    serial_emerg_reset_for_test();
    serial_emerg_set_ledger_id_for_test(3u);

    token = serial_emerg_reserve();
    TEST_ASSERT_EQ(token & SERIAL_EMERG_TOKEN_VALID, SERIAL_EMERG_TOKEN_VALID,
                   "CPU 3 takes an allowance");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1u, "and is charged for it");

    serial_emerg_set_ledger_id_for_test(4u);
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0u,
                   "another CPU is charged nothing for it");
    serial_emerg_return(token);
    TEST_ASSERT_EQ(serial_emerg_waits(), 1u,
                   "and returning CPU 3's token releases nothing");
    serial_emerg_refund_self(SERIAL_EMERG_STUCK_BYTES);
    TEST_ASSERT_EQ(serial_emerg_waits(), 1u,
                   "nor can a refund absorb a charge it does not own");

    serial_emerg_set_ledger_id_for_test(3u);
    serial_emerg_return(token);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0u,
                   "while the owning CPU releases exactly its own charge");

    serial_emerg_set_ledger_id_for_test(SERIAL_EMERG_NO_OWNER);
    serial_emerg_reset_for_test();
}

/* THE OVERRIDE RESTORE IS PROVED, NOT PERFORMED.
 *
 * Every owner-isolation test above ends by storing SERIAL_EMERG_NO_OWNER, and
 * none of them exercises ledger identity afterwards -- so a restore path that
 * left a fake id selected would pass all of them, silently attribute later
 * suites' charges to a CPU that does not exist, and undermine exactly the
 * attribution evidence those tests are cited for.
 *
 * The fake id is derived from the real one rather than fixed, because a
 * hardcoded fake that happened to equal this machine's APIC id would make the
 * isolation assertion vacuous on that machine only. */
static void test_serial_emergency_restore_ledger_identity(void *unused)
{
    (void)unused;
    serial_emerg_set_ledger_id_for_test(SERIAL_EMERG_NO_OWNER);
}

static void test_serial_emergency_ledger_override_restores(void)
{
    uint32_t real = cpu_panic_safe_apic_id() & 0xFFu;
    uint32_t fake = real ^ 1u;
    uint32_t token;

    /* Registered BEFORE the overrides, so identity is restored even if an
     * assertion below ends the test early. */
    test_add_action(test_serial_emergency_restore_ledger_identity, 0);

    serial_emerg_reset_for_test();
    serial_emerg_set_ledger_id_for_test(SERIAL_EMERG_NO_OWNER);

    token = serial_emerg_reserve();
    TEST_ASSERT_NEQ(token, SERIAL_EMERG_NO_TOKEN, "the real identity reserves");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1u,
                   "and NO_OWNER selects the real CPUID identity, not a stub");

    serial_emerg_set_ledger_id_for_test(fake);
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 0u,
                   "a different id is charged nothing for it");

    serial_emerg_set_ledger_id_for_test(SERIAL_EMERG_NO_OWNER);
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1u,
                   "and restoring NO_OWNER selects the real identity again");

    serial_emerg_return(token);
    TEST_ASSERT_EQ(serial_emerg_waits(), 0u,
                   "so the restored identity can still return its own token");
    serial_emerg_reset_for_test();
}

/* THE CLAIM AND ITS EXACT INVERSE, driven directly.
 *
 * That the transition is ONE lock-prefixed instruction is not assertable from
 * here -- a two-step implementation reaches an identical post-state, which is
 * why tools/atomic-claim-check disassembles the built object instead. What IS
 * assertable is the exactness contract those instructions implement: a claim
 * lands only over the word the caller observed, and a release frees only the
 * claim the caller actually holds. */
static void test_serial_emergency_claim_is_exact(void)
{
    uint32_t gen;

    serial_emerg_reset_for_test();
    gen = 0u;

    /* Reserve slot 0 so the array holds a known live claim, then read the epoch
     * back out of the token rather than assuming it. */
    {
        uint32_t t = serial_emerg_reserve();
        TEST_ASSERT_EQ(t & SERIAL_EMERG_TOKEN_VALID, SERIAL_EMERG_TOKEN_VALID,
                       "a reservation succeeds on a fresh epoch");
        TEST_ASSERT_EQ(t & 0xFFu, 0u, "taking slot 0");
        gen = (t >> 8) & 0xFFu;
    }

    /* A TOKEN NEVER NAMES A DEAD EPOCH. The generation is read before the claim
     * compare-exchange and lives in a different word, so a publication can land
     * between them; reserve re-reads it afterwards and releases the claim rather
     * than handing back an allowance nothing counts while its holder still
     * spends a full-length wait. A single CPU cannot schedule that interleaving,
     * so what is asserted here is the post-condition it exists to guarantee. */
    TEST_ASSERT_EQ(gen, serial_emerg_gen_for_test(),
                   "a granted token always names the LIVE epoch");

    /* A claim over a stale expectation must FAIL and change nothing: this is
     * what stops a second CPU from overwriting a live charge. */
    TEST_ASSERT_EQ((uint32_t)serial_emerg_claim_slot(0u, 0u /*expect FREE*/,
                                                     gen, 9u), 0u,
                   "claiming a live slot with a FREE expectation is refused");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1u, "and the live charge still stands");
    TEST_ASSERT_EQ(serial_emerg_charges_self(), 1u,
                   "still attributed to the CPU that took it");

    /* A release over a wrong expectation must be equally inert. */
    TEST_ASSERT_EQ((uint32_t)serial_emerg_release_slot(0u, 0u), 0u,
                   "releasing with a stale expectation is refused");
    TEST_ASSERT_EQ(serial_emerg_waits(), 1u, "and frees nothing");

    /* An unheld slot is claimable, and the claim shows up as a charge owned by
     * the id the caller named. */
    TEST_ASSERT_EQ((uint32_t)serial_emerg_claim_slot(1u, 0u, gen, 9u), 1u,
                   "an unheld slot is claimable over a FREE expectation");
    TEST_ASSERT_EQ(serial_emerg_waits(), 2u, "which is a second outstanding charge");
    TEST_ASSERT_EQ((uint32_t)serial_emerg_release_slot(1u, 0u), 0u,
                   "and a mismatched release still cannot free it");

    serial_emerg_reset_for_test();
    TEST_ASSERT_EQ(serial_emerg_waits(), 0u, "the epoch bump clears both");
}

/* --- the emergency string walk (section 26) ------------------------------
 * The walk cuts a record into SERIAL_EMERG_CHUNK pieces, stops at
 * SERIAL_EMERG_MAX_CHARS, continues past a capacity stop but not past a fault,
 * and appends the unreadable marker after whatever it managed to read. None of
 * that was observable while every byte left through the UART, so a walk that
 * dropped the final partial chunk, stopped early on a capacity result, or
 * emitted the marker instead of the text would have looked identical.
 *
 * Static rather than a local: the collector is larger than a panic-path frame
 * should ever be, and the tests run one at a time. */
#define WALK_SINK_CAP  2048u
static struct {
    char     buf[WALK_SINK_CAP];
    uint32_t len;
    uint32_t calls;
    uint32_t first_len;
    uint32_t last_len;
    uint32_t overflow;
} g_walk;

static char g_walk_src[SERIAL_EMERG_MAX_CHARS + SERIAL_EMERG_CHUNK + 1u];

static void test_walk_collect(void *sink, const char *buf, uint32_t len)
{
    uint32_t i;

    (void)sink;
    if (len > WALK_SINK_CAP - g_walk.len) { g_walk.overflow++; return; }
    for (i = 0; i < len; i++)
        g_walk.buf[g_walk.len + i] = buf[i];
    g_walk.len += len;
    if (!g_walk.calls)
        g_walk.first_len = len;
    g_walk.last_len = len;
    g_walk.calls++;
}

static void test_walk_reset(void)
{
    memset(&g_walk, 0, sizeof g_walk);
}

/* Fill a source of `n` printable characters and terminate it. */
static const char *test_walk_source(uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        g_walk_src[i] = (char)('a' + (i % 26u));
    g_walk_src[n] = '\0';
    return g_walk_src;
}

static void test_serial_emergency_walk_cuts_records_at_the_chunk_edge(void)
{
    /* Shorter than one chunk: one emission of exactly the payload. */
    test_walk_reset();
    serial_emerg_walk_for_test("panic", PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.calls, 1u, "a short record is emitted in one piece");
    TEST_ASSERT_EQ(g_walk.len, 5u, "of exactly its own length");
    TEST_ASSERT_EQ((uint32_t)(memcmp(g_walk.buf, "panic", 5u) == 0), 1u,
                   "and the bytes are the caller's, unaltered");

    /* Exactly one chunk. The guarded read reports a CAPACITY stop here, and a
     * walk that treated that as the end would still look correct -- so the next
     * case is the one that actually pins it. */
    test_walk_reset();
    serial_emerg_walk_for_test(test_walk_source(SERIAL_EMERG_CHUNK),
                               PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.len, SERIAL_EMERG_CHUNK,
                   "a full-chunk record emits every byte and no more");
    TEST_ASSERT_EQ(g_walk.calls, 1u, "in a single emission");

    /* One byte past the edge: the capacity stop must CONTINUE the walk, and the
     * trailing partial chunk must not be dropped. */
    test_walk_reset();
    serial_emerg_walk_for_test(test_walk_source(SERIAL_EMERG_CHUNK + 1u),
                               PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.calls, 2u,
                   "a record past the chunk edge continues past the capacity stop");
    TEST_ASSERT_EQ(g_walk.first_len, SERIAL_EMERG_CHUNK, "a full first chunk");
    TEST_ASSERT_EQ(g_walk.last_len, 1u, "and the trailing byte is not dropped");
    TEST_ASSERT_EQ(g_walk.len, SERIAL_EMERG_CHUNK + 1u,
                   "so the whole record reaches the sink");
    TEST_ASSERT_EQ(g_walk.overflow, 0u, "and nothing overran the collector");
}

static void test_serial_emergency_walk_stops_at_the_character_ceiling(void)
{
    const char *src;

    /* EXACTLY the ceiling: every byte must survive, and the record must end
     * because the string ended, not because the walk clipped it. */
    test_walk_reset();
    src = test_walk_source(SERIAL_EMERG_MAX_CHARS);
    serial_emerg_walk_for_test(src, PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.len, SERIAL_EMERG_MAX_CHARS,
                   "a record of exactly the ceiling is emitted whole");
    TEST_ASSERT_EQ(g_walk.calls, SERIAL_EMERG_MAX_CHARS / SERIAL_EMERG_CHUNK,
                   "in whole chunks, with no short final emission");
    TEST_ASSERT_EQ((uint32_t)(memcmp(g_walk.buf, src, SERIAL_EMERG_MAX_CHARS) == 0),
                   1u, "byte for byte, in order");
    TEST_ASSERT_EQ(g_walk.overflow, 0u, "and nothing overran the collector");

    /* ONE byte over: the extra byte must be absent, not merely uncounted. The
     * adjacent boundaries are tested separately because an off-by-one that
     * clipped at MAX-1 or admitted MAX+1 still reports a plausible aggregate
     * length on a coarse over-long input. */
    test_walk_reset();
    src = test_walk_source(SERIAL_EMERG_MAX_CHARS + 1u);
    serial_emerg_walk_for_test(src, PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.len, SERIAL_EMERG_MAX_CHARS,
                   "one byte past the ceiling truncates to the ceiling exactly");
    TEST_ASSERT_EQ((uint32_t)(memcmp(g_walk.buf, src, SERIAL_EMERG_MAX_CHARS) == 0),
                   1u, "keeping every byte up to it");
    TEST_ASSERT_EQ(g_walk.last_len, SERIAL_EMERG_CHUNK,
                   "the final chunk is full, so the ceiling clipped it, not the walk");

    /* Well past the ceiling: the same answer, so the bound is the ceiling and
     * not some property of how far the source happened to run. */
    test_walk_reset();
    serial_emerg_walk_for_test(
        test_walk_source(SERIAL_EMERG_MAX_CHARS + SERIAL_EMERG_CHUNK),
        PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.len, SERIAL_EMERG_MAX_CHARS,
                   "and a far-over-long record stops at the same place");
    TEST_ASSERT_EQ(g_walk.overflow, 0u, "with nothing overrunning the collector");
}

/* An unreadable source must produce the marker rather than silence: an empty
 * tail is indistinguishable from a short string, and the difference is "that is
 * all it said" versus "the pointer describing the crash was itself corrupt".
 *
 * Driven with a NON-CANONICAL source, the same fault surface the guarded-read
 * tests above use: the live unmapped-page recovery needs a real #PF and is
 * serial-validated, while a non-canonical address is rejected before the load
 * and reaches the identical stop reason. */
static void test_serial_emergency_walk_marks_an_unreadable_source(void)
{
    static const char mark[] = "<truncated: unreadable>";

    test_walk_reset();
    serial_emerg_walk_for_test((const char *)0x0000800000000000ull,
                               PANIC_CTX_NORMAL, test_walk_collect, 0);

    TEST_ASSERT_EQ(g_walk.calls, 1u,
                   "an unreadable source still emits -- silence would be a lie");
    TEST_ASSERT_EQ(g_walk.len, (uint32_t)(sizeof mark - 1u),
                   "and what it emits is the marker, whole");
    TEST_ASSERT_EQ((uint32_t)(memcmp(g_walk.buf, mark, sizeof mark - 1u) == 0), 1u,
                   "with the exact text a serial log reader is looking for");

    /* A readable record must NOT carry it. */
    test_walk_reset();
    serial_emerg_walk_for_test("clean", PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.len, 5u, "a readable record ends without a marker");
}

/* NMI context takes the unguarded byte walk instead of the guarded loop, because
 * recovery through IRETQ would re-arm NMI delivery while the outer NMI still
 * owns IST2. It must divide the record identically -- the chunking is not a
 * property of which read primitive was used. */
static void test_serial_emergency_walk_chunks_alike_in_nmi_context(void)
{
    test_walk_reset();
    serial_emerg_walk_for_test(test_walk_source(SERIAL_EMERG_CHUNK + 1u),
                               PANIC_CTX_NMI, test_walk_collect, 0);

    TEST_ASSERT_EQ(g_walk.calls, 2u, "the unguarded walk chunks at the same edge");
    TEST_ASSERT_EQ(g_walk.first_len, SERIAL_EMERG_CHUNK, "with a full first chunk");
    TEST_ASSERT_EQ(g_walk.len, SERIAL_EMERG_CHUNK + 1u,
                   "and loses no byte of the record");

    test_walk_reset();
    serial_emerg_walk_for_test(
        test_walk_source(SERIAL_EMERG_MAX_CHARS + SERIAL_EMERG_CHUNK),
        PANIC_CTX_NMI, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.len, SERIAL_EMERG_MAX_CHARS,
                   "and honours the same character ceiling");
}

/* Neither a NULL record nor a NULL sink may walk anything: the panic path is the
 * worst possible place to take a fault out of a diagnostic helper. */
static void test_serial_emergency_walk_refuses_null_operands(void)
{
    test_walk_reset();
    serial_emerg_walk_for_test(0, PANIC_CTX_NORMAL, test_walk_collect, 0);
    TEST_ASSERT_EQ(g_walk.calls, 0u, "a NULL record emits nothing at all");

    serial_emerg_walk_for_test("text", PANIC_CTX_NORMAL, 0, 0);
    TEST_ASSERT_EQ(g_walk.calls, 0u, "and a NULL sink is inert rather than fatal");
}

static void test_serial_emergency_kread_u8_rejects_bad_operands(void)
{
    uint8_t b = 0xFFu;

    TEST_ASSERT_EQ((uint32_t)(int32_t)__kread_u8(&b, (const void *)0x0000800000000000ULL),
                   (uint32_t)(int32_t)-1,
                   "a non-canonical address is rejected before the load");
    TEST_ASSERT_EQ((uint32_t)(int32_t)__kread_u8((uint8_t *)0, (const void *)&b),
                   (uint32_t)(int32_t)-1,
                   "a NULL destination is rejected");
    TEST_ASSERT_EQ((uint32_t)b, 0xFFu,
                   "a rejected read leaves the destination untouched");
}

/* ---- g_serial_lock ownership handoff ----
 *
 * Against a FIXTURE lock and a FIXTURE owner word, never the real globals. An
 * earlier revision asserted on the live owner word directly; that was both racy
 * (on SMP another CPU may legitimately hold the serial lock at the instant of
 * the assertion, so the expected zero is nondeterministic under unrelated serial
 * traffic) and weak (it passed even with ownership recording removed entirely).
 * The fixture form proves the branch that actually prevents the hang: a matching
 * owner RELEASES the lock. */
static void test_serial_lock_handoff_releases_for_owner(void)
{
    serial_lock_t  fixture = SERIAL_LOCK_INIT;
    const uint32_t me      = SERIAL_LOCK_OWNER_OF(6u);

    /* The acquire IS the owner record now -- there is no separate word to seed,
     * which is precisely the property under test. */
    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&fixture, me), 1,
                   "acquiring a free lock records this owner in the same word");
    TEST_ASSERT_EQ(fixture.owner, me,
                   "the lock word names its holder with no second store");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&fixture, me), 1,
                   "the recorded owner is allowed to release the lock");
    TEST_ASSERT_EQ(fixture.owner, SERIAL_LOCK_FREE,
                   "a successful handoff clears the owner AND frees the lock -- "
                   "this is what stops the surviving CPU hanging on its next write");
}

/* The other half, and the one that must never fire: a CPU that does not own the
 * lock must leave a live holder completely untouched. Without this the park-time
 * release would be a way for one CPU to steal another CPU's serial lock. */
static void test_serial_lock_handoff_refuses_foreign_owner(void)
{
    serial_lock_t fixture = SERIAL_LOCK_INIT;

    TEST_ASSERT_EQ(serial_lock_try_acquire_owned(&fixture,
                                                 SERIAL_LOCK_OWNER_OF(6u)), 1,
                   "CPU 6 holds the fixture lock");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&fixture,
                                                 SERIAL_LOCK_OWNER_OF(8u)), 0,
                   "a CPU that does not own the lock releases nothing");
    TEST_ASSERT_EQ(fixture.owner, SERIAL_LOCK_OWNER_OF(6u),
                   "the real holder's record is left exactly as it was");

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&fixture,
                                                 SERIAL_LOCK_OWNER_OF(6u)), 1,
                   "the live holder released cleanly");
}

/* Safe to call unconditionally: the panic async-park path invokes it without
 * knowing whether the faulting step held the lock at all. */
static void test_serial_lock_handoff_inert_when_unowned(void)
{
    serial_lock_t fixture = SERIAL_LOCK_INIT;

    TEST_ASSERT_EQ(serial_lock_try_release_owned(&fixture,
                                                 SERIAL_LOCK_OWNER_OF(4u)), 0,
                   "releasing a free, unowned lock reports no handoff");
    TEST_ASSERT_EQ(fixture.owner, SERIAL_LOCK_FREE, "the free lock stays free");

    TEST_ASSERT_EQ(serial_lock_try_release_owned((serial_lock_t *)0,
                                                 SERIAL_LOCK_OWNER_OF(4u)), 0,
                   "a NULL lock is rejected without dereference");
    TEST_ASSERT_EQ(serial_lock_try_acquire_owned((serial_lock_t *)0,
                                                 SERIAL_LOCK_OWNER_OF(4u)), 0,
                   "a NULL lock is rejected on the acquire side too");
}

/* ---- NMI nesting depth ----
 *
 * Pure per-CPU counter arithmetic: no boot infrastructure, no interrupt is
 * raised, and the test restores the depth it started from. The counter is what
 * lets the panic context predicate distinguish a fault taken INSIDE an NMI
 * handler (which must NOT use the fault-suppressed read, because its fixup
 * IRETQs and re-arms NMI over the outer NMI's live IST2 frames) from an ordinary
 * fault, in the case where frame->int_no names only the inner vector. */
static void test_nmi_depth_nests_and_unwinds(void)
{
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 0u,
                   "no NMI is in flight while the test suite runs");
    TEST_ASSERT_EQ((uint32_t)idt_in_nmi(), 0u,
                   "in_nmi is false at depth zero");

    idt_nmi_enter();
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 1u, "one enter raises the depth to 1");
    TEST_ASSERT_EQ((uint32_t)idt_in_nmi(), 1u, "in_nmi is true at depth 1");

    idt_nmi_enter();
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 2u, "the depth NESTS rather than latching");
    TEST_ASSERT_EQ((uint32_t)idt_in_nmi(), 1u, "in_nmi stays true while nested");

    idt_nmi_exit();
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 1u, "one exit unwinds one level only");
    TEST_ASSERT_EQ((uint32_t)idt_in_nmi(), 1u,
                   "in_nmi is STILL true with an outer NMI live -- the whole point");

    idt_nmi_exit();
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 0u, "the balanced pair returns to zero");
    TEST_ASSERT_EQ((uint32_t)idt_in_nmi(), 0u, "in_nmi is false once unwound");
}

/* An unbalanced exit must SATURATE, never wrap. Underflow would set the depth to
 * 0xFFFFFFFF and pin this CPU in "inside NMI" for the rest of the boot, silently
 * disabling the guarded read on a CPU that is not in an NMI at all -- turning a
 * safety signal into a permanent degradation. */
static void test_nmi_depth_exit_saturates_at_zero(void)
{
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 0u, "precondition: depth starts at zero");

    idt_nmi_exit();
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 0u,
                   "an unbalanced exit saturates at zero instead of wrapping");
    TEST_ASSERT_EQ((uint32_t)idt_in_nmi(), 0u,
                   "a saturated depth does not report a phantom NMI context");

    idt_nmi_exit();
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 0u, "repeated unbalanced exits stay at zero");
}

/* ---- the panic context decision itself ----
 *
 * The counter tests above prove only that a number goes up and down. THIS is the
 * assertion that pins the fix: reverting panic_declared_ctx to the old
 * vector-only predicate, or dropping the depth term at any consumer, restores
 * the guarded-read/IST2 corruption path -- and would leave every other test in
 * this file green. Needs neither a real NMI nor a real fault: a stack frame with
 * the vector field set and explicit depth manipulation is the whole input space.
 *
 * Restores the depth it borrows, so it cannot leak state into a later suite. */
static void test_panic_declared_ctx_vector_and_depth(void)
{
    struct interrupt_frame nmi_frame = {0};
    struct interrupt_frame pf_frame  = {0};

    nmi_frame.int_no = VECTOR_NMI;
    pf_frame.int_no  = 14u;             /* #PF -- the live nested-abort route */

    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 0u, "precondition: depth starts at zero");

    TEST_ASSERT_EQ(panic_declared_ctx(&nmi_frame), (uint32_t)PANIC_CTX_NMI,
                   "a direct NMI entry is NMI context on the vector alone");
    TEST_ASSERT_EQ(panic_declared_ctx(&pf_frame), (uint32_t)PANIC_CTX_NORMAL,
                   "an ordinary page fault is NORMAL context at depth zero");
    TEST_ASSERT_EQ(panic_declared_ctx((struct interrupt_frame *)0),
                   (uint32_t)PANIC_CTX_NORMAL,
                   "a software panic outside any NMI is NORMAL context");

    idt_nmi_enter();

    /* THE CASE THE VECTOR CANNOT SEE: same page-fault frame, but taken INSIDE
     * an NMI handler. Vector says 14, depth says NMI, and NMI must win. */
    TEST_ASSERT_EQ(panic_declared_ctx(&pf_frame), (uint32_t)PANIC_CTX_NMI,
                   "a fault taken INSIDE an NMI is NMI context despite its vector");
    TEST_ASSERT_EQ(panic_declared_ctx((struct interrupt_frame *)0),
                   (uint32_t)PANIC_CTX_NMI,
                   "a NULL frame nested inside an NMI is still NMI context");
    TEST_ASSERT_EQ(panic_declared_ctx(&nmi_frame), (uint32_t)PANIC_CTX_NMI,
                   "an NMI frame at depth stays NMI context");

    idt_nmi_exit();

    TEST_ASSERT_EQ(panic_declared_ctx(&pf_frame), (uint32_t)PANIC_CTX_NORMAL,
                   "once the NMI unwinds, an ordinary fault is NORMAL again");
    TEST_ASSERT_EQ(idt_nmi_depth_raw(), 0u, "the test restores the depth it took");
}

/* The consequence, spelled out end to end: NMI context must DENY the guarded
 * read. Asserting the predicate and the gate separately would let a correct
 * classification feed a gate that ignores it. */
static void test_panic_nested_nmi_denies_guarded_read(void)
{
    struct interrupt_frame pf_frame = {0};

    pf_frame.int_no = 14u;

    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(panic_declared_ctx(&pf_frame)),
                   1, "an ordinary fault may use the fault-suppressed read");

    idt_nmi_enter();
    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(panic_declared_ctx(&pf_frame)),
                   0, "a fault nested inside an NMI must NOT use it -- its fixup "
                      "IRETQ would re-arm NMI over the outer NMI's IST2 frames");
    idt_nmi_exit();

    TEST_ASSERT_EQ(serial_emerg_ctx_allows_guarded_read(panic_declared_ctx(&pf_frame)),
                   1, "the guarded read is available again once unwound");
}

void test_register_serial_emergency(void)
{
    test_suite_register_cat("serial_lock: owner handoff releases for owner",
                            test_serial_lock_handoff_releases_for_owner,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_lock: handoff refuses a foreign owner",
                            test_serial_lock_handoff_refuses_foreign_owner,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_lock: handoff inert when unowned",
                            test_serial_lock_handoff_inert_when_unowned,
                            TEST_CAT_BOOT);
    test_suite_register_cat("panic_ctx: vector and NMI depth truth table",
                            test_panic_declared_ctx_vector_and_depth,
                            TEST_CAT_BOOT);
    test_suite_register_cat("panic_ctx: nested NMI denies the guarded read",
                            test_panic_nested_nmi_denies_guarded_read,
                            TEST_CAT_BOOT);
    test_suite_register_cat("nmi_depth: nests and unwinds one level per exit",
                            test_nmi_depth_nests_and_unwinds,
                            TEST_CAT_BOOT);
    test_suite_register_cat("nmi_depth: unbalanced exit saturates at zero",
                            test_nmi_depth_exit_saturates_at_zero,
                            TEST_CAT_BOOT);
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
    test_suite_register_cat("serial_lock: wrong owner leaves the live holder",
                            test_serial_lock_release_wrong_owner_leaves_holder,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_lock: the free value is not an identity",
                            test_serial_lock_free_is_not_an_identity,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_lock: owner encoding is injective",
                            test_serial_lock_owner_encoding_is_injective,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_lock: same-owner re-entry is refused",
                            test_serial_lock_same_owner_reentry_is_refused,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_lock: handoff wrapper encodes a raw id",
                            test_serial_lock_release_if_owner_for_encodes_raw_id,
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
    test_suite_register_cat("serial_emergency: stale return does not corrupt the budget",
                            test_serial_emergency_stale_return_does_not_corrupt_budget,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: stale token cannot spend a new epoch",
                            test_serial_emergency_stale_token_cannot_spend_new_epoch,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: returning a refusal token is inert",
                            test_serial_emergency_no_token_return_is_inert,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: charges are attributed per CPU",
                            test_serial_emergency_charges_are_attributed_per_cpu,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: a new epoch voids held charges",
                            test_serial_emergency_charges_reset_across_epoch,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: refund is bounded by charges held",
                            test_serial_emergency_refund_self_is_bounded,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: __kread_u8 reads valid kernel bytes",
                            test_serial_emergency_kread_u8_reads_valid_bytes,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: __kread_u8 rejects #GP operands",
                            test_serial_emergency_kread_u8_rejects_bad_operands,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: partial refund preserves the rest",
                            test_serial_emergency_refund_self_preserves_unrequested,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: refund is isolated between CPU ids",
                            test_serial_emergency_refund_isolated_between_cpu_ids,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: guarded read context is opt-in",
                            test_serial_emergency_guarded_read_context_is_opt_in,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: __kread_u8 fixup routing",
                            test_serial_emergency_kread_u8_fixup_routing,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: guarded string copy terminates",
                            test_kstr_read_guarded_copies_and_terminates,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: guarded string copy is bounded",
                            test_kstr_read_guarded_bounds_the_copy,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: guarded string copy reports unreadable",
                            test_kstr_read_guarded_reports_unreadable,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: guarded string clips at canonical half",
                            test_kstr_read_budget_clips_at_the_canonical_half,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: guarded string fixup routing",
                            test_kstr_read_fixup_lookup_routes_reads_only,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: slot reuse reissues the freed slot",
                            test_serial_emergency_slot_reuse_picks_the_freed_slot,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: malformed token is rejected",
                            test_serial_emergency_malformed_token_is_rejected,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: latch transitions preserve generation",
                            test_serial_emergency_latch_transitions_preserve_generation,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: ledger override restores identity",
                            test_serial_emergency_ledger_override_restores,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: claim and release are exact",
                            test_serial_emergency_claim_is_exact,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: epoch bump releases every allowance",
                            test_serial_emergency_epoch_bump_releases_every_allowance,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: reserve sweeps past a held slot",
                            test_serial_emergency_reserve_sweeps_past_a_held_slot,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: return rejects a foreign owner",
                            test_serial_emergency_return_rejects_a_foreign_owner,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: walk cuts records at the chunk edge",
                            test_serial_emergency_walk_cuts_records_at_the_chunk_edge,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: walk stops at the character ceiling",
                            test_serial_emergency_walk_stops_at_the_character_ceiling,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: walk marks an unreadable source",
                            test_serial_emergency_walk_marks_an_unreadable_source,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: walk chunks alike in NMI context",
                            test_serial_emergency_walk_chunks_alike_in_nmi_context,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: walk refuses NULL operands",
                            test_serial_emergency_walk_refuses_null_operands,
                            TEST_CAT_BOOT);
}
