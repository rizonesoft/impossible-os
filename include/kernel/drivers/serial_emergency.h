/* ============================================================================
 * serial_emergency.h -- internal contract for the abort-safe serial path
 *
 * Split out of serial.h deliberately. These declarations need `spinlock_t`,
 * and serial.h is included by seventeen files -- four of them architecture-
 * neutral surfaces (klog.c, fs/partition.c, fs/gpt.c, nt/nt_syscall.c) that
 * would then transitively pull spinlock.h's inline x86-64 asm. A device-driver
 * header should not be the vector that widens the arch include closure, so the
 * lock-policy surface lives here and is included only by serial.c and its unit
 * test.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/spinlock.h"

/* Acquire/release policy for the emergency path, exposed so the never-block
 * contract is unit-testable against a caller-supplied lock with no UART and no
 * boot infrastructure involved.
 *   serial_emergency_acquire() -- 1 = acquired (caller must release), 0 = the
 *                                 lock was held elsewhere.
 *   serial_emergency_release() -- releases only when `acquired` is non-zero.
 * Neither touches the interrupt flag or IRQL; that is the caller's business,
 * exactly as it is for spin_trylock/spin_tryunlock. */
int  serial_emergency_acquire(spinlock_t *lock);
void serial_emergency_release(spinlock_t *lock, int acquired);

/* Hand the driver's own UART lock back if THIS CPU is the recorded holder.
 *
 * For a CPU that is about to park forever and therefore can never release it
 * itself -- today that is the panic async-isolation branch, which parks a
 * faulting async-init worker so the rest of the boot survives. Without this, an
 * AP that faulted while holding the lock leaves it set and every surviving CPU
 * blocks on its next ordinary serial write: a silent hang instead of a boot.
 *
 * Safe to call unconditionally. Ownership is compared before release, so a CPU
 * that does not hold the lock changes nothing and can never steal it from a live
 * holder. Owner identity is the CPUID-derived APIC id, so this does not depend on
 * a GS base the panic path cannot trust. */
void serial_lock_release_if_owner(void);

/* The ownership policy itself, over a CALLER-SUPPLIED lock and owner word, so
 * the successful handoff can be unit tested without seizing the machine's real
 * serial lock. Returns 1 if `me` owned the lock and it was released, 0 if `me`
 * owned nothing -- in which case BOTH words are left exactly as they were. */
int serial_lock_try_release_owned(spinlock_t *lock, volatile uint32_t *owner,
                                  uint32_t me);

/* Wedged-transmitter budget accounting, exposed for unit test. The arithmetic
 * here already regressed once (a revision charged each timeout twice, halving
 * the effective budget), which is why it is testable rather than private.
 *   serial_emerg_reserve()  -- nonzero TOKEN = this caller may perform a
 *                              full-length wait; SERIAL_EMERG_NO_TOKEN = it may
 *                              not. The token names the accounting epoch the
 *                              charge landed in, and serial_emerg_return
 *                              validates it, so a reservation taken before an
 *                              epoch was published can no longer be handed back
 *                              into the epoch that replaced it. A caller that is
 *                              saturated or lost the CAS race gets
 *                              SERIAL_EMERG_NO_TOKEN and probes once.
 *   serial_emerg_return(t)  -- give back a reservation that did NOT time out.
 *                              Ignores a token whose epoch has since ended.
 *                              SINGLE-USE: return each token exactly once. A
 *                              freed slot is reissued by the next reserve, so a
 *                              token returned twice can release the charge that
 *                              replaced it. Not idempotent, and cannot be made
 *                              so within the 32-bit latch word.
 *   serial_emerg_waits()    -- current charge; 0 when nothing has timed out.
 *   serial_emerg_charges_self() -- charges the CALLING CPU currently holds in
 *                              the live epoch. The global charge cannot answer
 *                              this: a delta across two reads of a shared
 *                              counter attributes another CPU's concurrent
 *                              charge to whoever measured last.
 *   serial_emerg_refund_self(n) -- hand back up to n charges this CPU is
 *                              recorded as holding. Bounded by that record, so
 *                              it can never absorb another CPU's charge or one
 *                              from a dead epoch. Used by the async-isolation
 *                              refund on a CPU that turns out to survive.
 *   serial_emerg_reset_for_test() -- restore the pristine state. Starts a NEW
 *                              accounting epoch (it clears the budget), so it
 *                              advances the generation and invalidates tokens.
 * The budget is MONOTONIC IN TIMEOUTS: a reservation that times out is never
 * handed back, and no ordinary success path lowers the count for another
 * caller's charge. It is NOT monotonic outright -- three things lower it, all
 * deliberate: a byte that drains returns its own reservation, the async refund
 * hands back the charges its own CPU made on a machine that turns out to
 * survive, and publishing an epoch clears every slot. See serial.c. */
/* Pure routing predicate: given the CPU attempting a REROUTED ordinary write
 * and the recorded arming CPU, must the write be discarded? Exposed so the
 * owner / non-owner / unknown cases are testable without arming the one-way
 * latch (which would degrade serial for the rest of the boot).
 *   `writer` or `owner` == SERIAL_EMERG_NO_OWNER means "cannot identify", and
 *   the answer is always 0 -- FAIL OPEN, preserve the evidence. */
#define SERIAL_EMERG_NO_OWNER  0xFFFFFFFFu
int serial_emerg_should_drop_for(uint32_t writer, uint32_t owner);

/* Pure predicate: may a caller in this panic context use the fault-suppressed
 * read (`__kread_u8`) to walk a caller-supplied string? True for
 * PANIC_CTX_NORMAL ONLY -- see the PANIC_CTX_* block in serial.h for why NMI
 * must not, and why unknown contexts take the restrictive answer. Exposed so
 * that opt-in property is testable without a UART or an armed latch; shared by
 * serial.c and panic.c so the two cannot drift apart. */
int serial_emerg_ctx_allows_guarded_read(uint32_t ctx);

/* Pure latch-transition arithmetic, exposed so the OFF->INIT->ARMED sequence is
 * testable without arming the one-way latch. serial_enter_emergency uses exactly
 * these, so testing them tests it. claim() must PRESERVE the generation (losing
 * it rewinds epoch identity and revalidates invalidated tokens); publish() must
 * advance the generation and clear every slot. refund() clears exactly the named
 * slots and no other field. */
uint32_t serial_emerg_claim_word(uint32_t cur, uint32_t owner);
uint32_t serial_emerg_publish_word(uint32_t cur);
uint32_t serial_emerg_refund_word(uint32_t cur, uint32_t give);

/* Reservation token. Bit 31 marks a real reservation so that a token minted in
 * generation 0 is still distinguishable from "no allowance"; the generation the
 * charge landed in sits above the low byte, and the low byte carries the slot
 * index that names the charge itself. Opaque to callers -- pass it back to
 * serial_emerg_return unmodified, exactly once. */
#define SERIAL_EMERG_NO_TOKEN     0x00000000u
#define SERIAL_EMERG_TOKEN_VALID  0x80000000u

uint32_t serial_emerg_reserve(void);
void     serial_emerg_return(uint32_t token);
uint32_t serial_emerg_waits(void);
uint32_t serial_emerg_charges_self(void);
void     serial_emerg_refund_self(uint32_t n);
void     serial_emerg_reset_for_test(void);

/* Test-only override of the ledger identity, so one CPU can prove that a refund
 * cannot consume another CPU's charge -- the defining property of the per-CPU
 * ledger, and one a same-CPU test cannot tell apart from a global counter. Pass
 * SERIAL_EMERG_NO_OWNER to restore the real CPUID identity; a test that sets it
 * MUST restore it. Affects ONLY the ledger: the panic owner claim and the
 * rerouted-write routing predicate keep reading real CPUID. */
void     serial_emerg_set_ledger_id_for_test(uint32_t id);
