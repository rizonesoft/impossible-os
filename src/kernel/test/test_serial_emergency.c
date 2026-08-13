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
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/serial_emergency.h"
#include "kernel/sched/spinlock.h"
#include "kernel/cpu_security.h"   /* __kread_u8 (guarded read under test) */

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
    /* An OFF word carrying generation 5 and two occupied slots. */
    uint32_t off   = (5u << 12) | (3u << 20);
    uint32_t claim = serial_emerg_claim_word(off, 7u);
    uint32_t armed;

    TEST_ASSERT_EQ((claim >> 12) & 0xFFu, 5u,
                   "OFF->INIT PRESERVES the generation, never rewinds it");
    TEST_ASSERT_EQ((claim >> 20) & 0xFFu, 3u, "and preserves occupied slots");
    TEST_ASSERT_EQ(claim & 0xFFu, 7u, "and records the claiming owner");
    TEST_ASSERT_EQ(claim & 0xC0000000u, 0x40000000u, "and the state is INIT");

    armed = serial_emerg_publish_word(claim);
    TEST_ASSERT_EQ((armed >> 12) & 0xFFu, 6u,
                   "INIT->ARMED ADVANCES the generation exactly once");
    TEST_ASSERT_EQ((armed >> 20) & 0xFFu, 0u,
                   "and clears every slot as it publishes");
    TEST_ASSERT_EQ(armed & 0xFFu, 7u, "while keeping the owner immutable");
    TEST_ASSERT_EQ(armed & 0xC0000000u, 0x80000000u, "and the state is ARMED");
}

/* The refund subtraction, tested directly for the cases the CAS loop only
 * reaches under contention: it must clear exactly the named slots, leave every
 * other field alone, and change nothing when none of them are set. */
static void test_serial_emergency_refund_word_clears_only_named_slots(void)
{
    uint32_t cur = 0x80000000u | (9u << 12) | (0x0Fu << 20) | 2u;
    uint32_t out;

    out = serial_emerg_refund_word(cur, 0x05u);
    TEST_ASSERT_EQ((out >> 20) & 0xFFu, 0x0Au,
                   "exactly the named slots are cleared");
    TEST_ASSERT_EQ((out >> 12) & 0xFFu, 9u, "the generation is untouched");
    TEST_ASSERT_EQ(out & 0xFFu, 2u, "the owner is untouched");
    TEST_ASSERT_EQ(out & 0xC0000000u, 0x80000000u, "the state is untouched");

    out = serial_emerg_refund_word(cur, 0xF0u);
    TEST_ASSERT_EQ(out, cur,
                   "refunding slots that are not set changes nothing at all");
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
    test_suite_register_cat("serial_emergency: slot reuse reissues the freed slot",
                            test_serial_emergency_slot_reuse_picks_the_freed_slot,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: malformed token is rejected",
                            test_serial_emergency_malformed_token_is_rejected,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: latch transitions preserve generation",
                            test_serial_emergency_latch_transitions_preserve_generation,
                            TEST_CAT_BOOT);
    test_suite_register_cat("serial_emergency: refund word clears only named slots",
                            test_serial_emergency_refund_word_clears_only_named_slots,
                            TEST_CAT_BOOT);
}
