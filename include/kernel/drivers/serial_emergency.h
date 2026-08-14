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

/* ---- serial_lock_t: the UART lock, with ownership INSIDE the lock word ----
 *
 * Not a spinlock_t. A spinlock_t is a bare 0/1 flag with no owner field, so
 * recording WHO holds it needs a second word and therefore a second store -- and
 * an abort landing between the flag CAS and the owner store leaves the lock held
 * with owner 0, which the force-release below then correctly refuses to touch.
 * That is the hang this type exists to REMOVE rather than narrow: `cli` masks
 * maskable interrupts only, while NMI and #MC pierce it, and those are exactly
 * the panic entries the serial path serves.
 *
 * One word, one transition per state change:
 *   free -> held   acquire, a single CAS 0 -> SERIAL_LOCK_OWNER_OF(id)
 *   held -> free   release, a single release-store of 0
 *   free -> held   try-acquire, the same CAS, non-blocking
 *   held -> free   force-release, a single CAS owner -> 0, exact owner only
 * There is no instant at which the lock is held by nobody, so an abort at ANY
 * instruction boundary leaves it either free or attributably owned.
 *
 * A DISTINCT type rather than a re-encoded spinlock_t on purpose: a spinlock_t
 * whose flag secretly held an APIC id would still compile against every generic
 * spin_lock/spin_trylock in the tree, each of which stores a bare 1 and would
 * silently record "owned by APIC id 0". The type is the enforcement.
 *
 * 0 is never a valid owner -- the encoding is id + 1 -- so a free word and a word
 * owned by APIC id 0 stay distinguishable. ONE definition of that encoding,
 * because open-coding `id + 1` at the record and release sites is how the two
 * silently stop matching: the compare-exchange would simply never fire and the
 * handback would degrade back to the hang it was added to remove, with no
 * runtime signal at all. */
#define SERIAL_LOCK_FREE          0u
#define SERIAL_LOCK_ID_MASK       0xFFu
#define SERIAL_LOCK_OWNER_OF(id)  (((id) & SERIAL_LOCK_ID_MASK) + 1u)

typedef struct {
    volatile uint32_t owner;
} serial_lock_t;

#define SERIAL_LOCK_INIT { .owner = SERIAL_LOCK_FREE }

/* Acquire/release policy for the emergency path, exposed so the never-block
 * contract is unit-testable against a caller-supplied lock with no UART and no
 * boot infrastructure involved.
 *   serial_emergency_acquire() -- 1 = acquired (caller must release), 0 = the
 *                                 lock was held elsewhere.
 *   serial_emergency_release() -- releases only when `acquired` is non-zero.
 * Neither touches the interrupt flag or IRQL; that is the caller's business,
 * exactly as it is for spin_trylock/spin_tryunlock.
 *
 * IDENTITY REPRESENTATION, and it has exactly one exception:
 *   ENCODED (SERIAL_LOCK_OWNER_OF(id)) -- serial_emergency_acquire,
 *     serial_lock_try_acquire_owned, serial_lock_try_release_owned.
 *   RAW APIC id -- serial_lock_release_if_owner_for, and ONLY that one, because
 *     it exists to BE the raw-to-encoded step and encodes internally.
 * Both forms are uint32_t, so a mix-up compiles silently, which is why the split
 * is enumerated here rather than left to each prototype. Passing a raw id to an
 * encoded parameter is the bug the encoding exists to prevent: raw id 0 reads as
 * SERIAL_LOCK_FREE and is refused outright, while raw id N records N, after
 * which the park-time release compares against N + 1, never matches, and the
 * panic-path hang returns with no runtime signal. Passing an ENCODED owner to
 * the raw seam is the same bug mirrored -- it encodes twice, so the exact-owner
 * CAS misses and the parking CPU strands the UART.
 *
 * The owner is passed in rather than derived so this records the same identity
 * the force-release later compares against, and so a test can drive both sides
 * of a contended lock from a single CPU.
 *
 * FAILURE AND INVALID INPUT, depended on by the panic path. In EVERY
 * zero-returning case below the lock word is left byte-identical, and none of
 * these dereferences a NULL lock -- the panic path is the worst possible place
 * to take a fault out of a diagnostic helper.
 *   The ENCODED-owner helpers (serial_emergency_acquire,
 *     serial_lock_try_acquire_owned, serial_lock_try_release_owned) return 0 for
 *     a NULL lock, for an `owner` of SERIAL_LOCK_FREE, and for genuine
 *     contention or non-ownership.
 *   serial_lock_release_if_owner_for returns 0 for a NULL lock and for
 *     non-ownership. It does NOT reject a raw id of 0: 0 is a VALID APIC id that
 *     encodes to owner 1, so this call legitimately succeeds when CPU 0 holds
 *     the lock. Rejecting it would strand exactly the CPU the encoding exists to
 *     keep distinguishable from a free word.
 *   serial_emergency_release is inert on a NULL lock and on `acquired` == 0. */
int  serial_emergency_acquire(serial_lock_t *lock, uint32_t owner);
void serial_emergency_release(serial_lock_t *lock, int acquired);

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

/* The raw-id-to-encoded step of the above, over a caller-supplied lock, so the
 * ONE place that conversion happens for the real global is testable without
 * seizing it. Takes a RAW APIC id (unlike everything else on this surface) and
 * encodes it internally -- that asymmetry is the entire point: if the wrapper
 * ever passed a raw id straight through, or used a different mask or a different
 * identity source, every encoded-owner fixture would stay green while a parking
 * CPU silently failed to hand back the UART. Returns 1 if it released. */
int serial_lock_release_if_owner_for(serial_lock_t *lock, uint32_t raw_id);

/* The ownership policy itself, over a CALLER-SUPPLIED lock, so both the
 * successful handoff and the successful acquisition can be unit tested without
 * seizing the machine's real serial lock.
 *
 * serial_lock_try_acquire_owned() -- 1 if `owner` took the lock, 0 if it was
 *   already held. The CAS both takes the lock and records the owner, so there is
 *   no window in which a holder is unattributable.
 * serial_lock_try_release_owned() -- 1 if `owner` held the lock and it was
 *   released, 0 if it held nothing, in which case the word is left exactly as it
 *   was. The exact-owner compare IS the safety property: this can never take a
 *   lock away from a live holder.
 * Neither touches the interrupt flag or IRQL. `owner` is the ENCODED value in
 * both, for the reason spelled out above serial_emergency_acquire. */
int serial_lock_try_acquire_owned(serial_lock_t *lock, uint32_t owner);
int serial_lock_try_release_owned(serial_lock_t *lock, uint32_t owner);

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
 * ADVANCE it, which is what starts the new accounting epoch -- every outstanding
 * allowance carries a generation, so the bump invalidates all of them in the
 * same compare-exchange that publishes ARMED, with no second pass and therefore
 * no window in which the two epochs coexist. */
uint32_t serial_emerg_claim_word(uint32_t cur, uint32_t owner);
uint32_t serial_emerg_publish_word(uint32_t cur);

/* THE ALLOWANCE IS ONE WORD, AND TAKING IT IS ONE INSTRUCTION.
 *
 * Each of the SERIAL_EMERG_STUCK_BYTES full-length waits is a composite
 * {generation, owner} claim word of its own, and the single compare-exchange
 * that takes the allowance is the same one that records who owns it and which
 * epoch it belongs to. There is no separate publication step and no separate
 * owner ledger, so there is no instant at which a charge exists without an
 * owner -- the failure that made a charge unattributable, and therefore
 * unrefundable, when the allowance was a bit in the latch and the owner a write
 * to a second word that an NMI or #MC could land in front of.
 *
 * Two consequences the callers below depend on. A charge exists if and only if a
 * live claim exists, so serial_emerg_waits and serial_emerg_charges_self read
 * the claims directly instead of reconstructing a count from two sources that
 * can disagree. And publishing an epoch releases every allowance by advancing
 * one generation field, so no reader ever sees a half-cleared budget.
 *
 * Reservation token. Bit 31 marks a real reservation so that a token minted in
 * generation 0 is still distinguishable from "no allowance"; the generation the
 * charge landed in sits above the low byte, and the low byte carries the slot
 * index that names the charge itself. Opaque to callers -- pass it back to
 * serial_emerg_return unmodified, exactly once, ON THE CPU THAT RESERVED IT: the
 * claim records an owner and the return compares against it, so a token that has
 * crossed CPUs releases nothing rather than releasing somebody else's charge. */
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

/* The live accounting epoch, exposed so a test can assert that a granted token
 * names it. The generation and the claim are separate words, so a publication
 * can land between reserve reading one and writing the other; reserve releases
 * such a claim rather than granting an allowance nothing counts. A single CPU
 * cannot schedule that interleaving, so the post-condition is what is checked. */
uint32_t serial_emerg_gen_for_test(void);

/* The emergency string walk with its OUTPUT AS A PARAMETER, so the chunking is
 * testable without a UART.
 *
 * serial_write_emergency cuts a record into SERIAL_EMERG_CHUNK-sized pieces,
 * stops at SERIAL_EMERG_MAX_CHARS, continues past a capacity stop but not past a
 * fault, and appends an unreadable marker AFTER whatever it managed to read.
 * None of that was observable while every byte left through the UART, so none of
 * it was tested -- a walk that dropped the final partial chunk, stopped early on
 * a capacity result, or emitted the marker instead of the text would have looked
 * identical from outside.
 *
 * A PARAMETER rather than an installable sink, deliberately: a writable function
 * pointer consulted on the panic path is mutable control-flow state, which can
 * be swapped between the calls comprising one report and which turns the
 * diagnostic into a second fault if it is ever corrupted. Here the production
 * path passes a constant callee and the test passes a collector; nothing global
 * decides where a panic's output goes.
 *
 * `emit` receives a byte SPAN, not a C string -- `len` is authoritative and the
 * buffer is not terminated. A NULL `str` or `emit` walks nothing. */
typedef void (*serial_emerg_emit_fn)(void *sink, const char *buf, uint32_t len);

void serial_emerg_walk_for_test(const char *str, uint32_t ctx,
                                serial_emerg_emit_fn emit, void *sink);

/* The two numbers that walk divides a record by: the total character ceiling for
 * one emergency record, and how many characters are captured per locked
 * emission. Here rather than private to serial.c so a test asserts against the
 * values the walk actually uses -- a fixture carrying its own copy of 128 would
 * keep passing after the chunk size changed, which is the one thing a chunk-edge
 * test exists to catch.
 *
 * The chunk size is what keeps the capture buffer a small frame on a #DF/#MC IST
 * stack while still emitting records in large atomic pieces; the ceiling leaves
 * 3x headroom over the longest string that reaches this path in-tree. */
#define SERIAL_EMERG_MAX_CHARS    1024u
#define SERIAL_EMERG_CHUNK        128u

/* The advertised ceiling: how many full-length waits one accounting epoch may
 * spend on a wedged transmitter. Also the extent of the claim array, since there
 * is exactly one claim word per allowance -- so a test that asserts the whole
 * budget is reachable, or that a held allowance costs exactly one, must use this
 * number and not a copy of it. */
#define SERIAL_EMERG_STUCK_BYTES  8u

/* THE SINGLE TRANSITION, and its exact inverse.
 *
 * serial_emerg_claim_slot() takes allowance `idx` for {gen, owner} -- but only
 * while it still reads exactly `expect`, which the caller observed to be either
 * free or a claim from an epoch that has since been replaced. Taking the
 * allowance and recording who owns it are the SAME compare-exchange; there is no
 * second step, which is the whole reason an allowance is a word of its own
 * rather than a bit in the latch beside an owner written separately.
 * serial_emerg_release_slot() frees it, again only while it reads exactly
 * `expect`, so a release can never touch a claim the caller does not hold.
 * Both return 1 on success and leave the word byte-identical on failure.
 *
 * These carry external linkage deliberately. A static helper is inlined into its
 * caller, and the single-lock-prefixed-transition property then has no symbol to
 * check -- tools/atomic-claim-check disassembles this function in the built
 * object precisely because no post-state a test can assert distinguishes one
 * atomic transition from two. `idx` must be below SERIAL_EMERG_STUCK_BYTES;
 * callers validate it, and the token path validates it against a malformed
 * token before it reaches here. */
int serial_emerg_claim_slot(uint32_t idx, uint32_t expect,
                            uint32_t gen, uint32_t owner);
int serial_emerg_release_slot(uint32_t idx, uint32_t expect);
