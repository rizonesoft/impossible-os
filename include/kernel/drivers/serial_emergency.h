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
 *   serial_emerg_mark_timeout(t) -- record that this reservation's wait was
 *                              never answered, so a reclamation leaves it.
 *   serial_emerg_reclaim_self() -- hand back every allowance this CPU holds
 *                              that carries no such mark, and report how many.
 *                              Bounded by this CPU's own claims, so it can
 *                              never absorb another CPU's charge or one from a
 *                              dead epoch. Used by the async-isolation branch
 *                              on a CPU that is about to park forever.
 *   serial_emerg_reset_for_test() -- restore the pristine state. Starts a NEW
 *                              accounting epoch (it clears the budget), so it
 *                              advances the generation and invalidates tokens.
 * The budget is MONOTONIC IN TIMEOUTS: a reservation that times out is never
 * handed back -- not by a return, which refuses a marked claim, and not by a
 * reclamation, which skips one. It is NOT monotonic outright: a byte that
 * drains returns its own reservation, a parking CPU reclaims what it took and
 * never spent, and publishing an epoch clears every slot.
 *
 * WHERE THAT LINE FALLS, stated because it is a policy choice rather than an
 * oversight. A wait that was INTERRUPTED -- an abort landing while the spin
 * loop is still running, before the timeout is established -- counts as taken
 * and never spent, so a parking CPU reclaims it. Committing the charge before
 * the wait begins was considered and rejected: it makes an interrupted wait a
 * PERMANENT charge on a machine that SURVIVES, and the two errors are not
 * symmetric. Over-refunding costs a later panic at most one extra bounded wait;
 * under-refunding walks the budget down to nothing, and a panic with no
 * allowance emits its reason through single status probes on a wedged UART and
 * may emit nothing at all -- the exact evidence loss this path exists to
 * prevent. See serial.c. */
/* Pure port-selection policy: which I/O base serial_init adopts, given what the
 * bootloader reported and the default already in place. Exposed here so the
 * three rows are unit-testable without a UART.
 *
 *   source == NONE           -> current. The probe found nothing and therefore
 *     names no base. It does NOT return 0: zeroing the port from boot_info is
 *     the 2026-04-11 incident (boot-code-quality Gate 8), where a machine whose
 *     COM1 worked lost all kernel serial output because the scratch-register
 *     probe false-negatived on its firmware. The no-UART finding is carried by
 *     serial_uart_probed_absent() instead, which gates the emergency WAIT
 *     BUDGET rather than the output.
 *   PROBE, port COM1 or COM2 -> port. The scratch-register probe tries exactly
 *     those two and can report nothing else, so a PROBE source naming any other
 *     base did not come from that probe whatever it claims -> current.
 *   SPCR, port one of the four legacy COM bases -> port. Firmware-authoritative
 *     and the only source permitted to name COM3 or COM4. A vendor-custom base
 *     is REFUSED: these two fields are outside the header validation, so a
 *     numeric range would let corrupted provenance name 0xCF8 (PCI
 *     CONFIG_ADDRESS) and have Phase 0 program it -> current.
 *   any other source byte -> current. An ALLOWLIST rather than an
 *     anything-but-NONE test: this value decides which I/O port Phase 0
 *     programs and the panic path writes to forever after, and boot_info's
 *     header validation covers the envelope, not this field.
 *
 * NEVER RETURNS 0 FOR A NON-ZERO `current`, which is what keeps `s_serial_port`
 * provably non-zero for the whole boot: it is initialised to COM1 and this is
 * its only other writer. That invariant is why the driver carries no zero-port
 * guards -- an unreachable test the optimiser deletes is a comment claiming a
 * protection the object does not have.
 *
 * `current` is the caller's existing setting, passed in rather than read from
 * the file's static so the policy is a pure function of its inputs. */
/* The four legacy 16550 bases. COM1 and COM2 are the only two the bootloader's
 * scratch-register probe tries; all four are what an ACPI SPCR entry is allowed
 * to name and be adopted. SERIAL_PORT_BASE_MAX is the arithmetic ceiling the
 * register block imposes (base..base+7 must stay inside 16-bit I/O space); it
 * is a sanity bound, NOT the acceptance test -- see serial_select_port. */
#define SERIAL_PORT_COM1      0x3F8u
#define SERIAL_PORT_COM2      0x2F8u
#define SERIAL_PORT_COM3      0x3E8u
#define SERIAL_PORT_COM4      0x2E8u
#define SERIAL_PORT_BASE_MAX  0xFFF8u
_Static_assert(SERIAL_PORT_COM1 <= SERIAL_PORT_BASE_MAX &&
               SERIAL_PORT_COM2 <= SERIAL_PORT_BASE_MAX &&
               SERIAL_PORT_COM3 <= SERIAL_PORT_BASE_MAX &&
               SERIAL_PORT_COM4 <= SERIAL_PORT_BASE_MAX,
               "every adoptable base must leave its register block in I/O space");
uint16_t serial_select_port(uint16_t boot_port, uint8_t boot_source,
                            uint16_t current);

/* Pure first-probe policy for the bounded emergency writer: what a byte does
 * having read the transmitter-ready bit once, given whether the bootloader
 * found a UART at all. Exposed for the same reason the routing predicate below
 * is -- the decision is the interesting part and it sits between two raw port
 * reads, where no fixture can reach it.
 *
 *   thre_ready              -> SEND_NOW. Checked FIRST, and that order is the
 *     whole safety argument for believing a negative probe at all: on a machine
 *     whose scratch-register probe false-negatived, the transmitter answers
 *     here and the byte goes out before `probed_absent` is ever consulted, so
 *     the no-UART finding costs such a machine nothing.
 *   !thre_ready, probed_absent -> SKIP_WAIT. Drop the byte WITHOUT reserving.
 *     Reserving and then declining to wait would still walk the contended claim
 *     word once per byte of the record, which is the cost this removes; and
 *     waiting would spend the SERIAL_EMERG_STUCK_BYTES ceiling on hardware that
 *     was never going to drain a byte.
 *   !thre_ready, !probed_absent -> MAY_WAIT. The ordinary path: this byte is
 *     about to wait, which is exactly what the budget bounds. */
#define SERIAL_PROBE_SEND_NOW   0u
#define SERIAL_PROBE_SKIP_WAIT  1u
#define SERIAL_PROBE_MAY_WAIT   2u
uint32_t serial_emerg_first_probe(int thre_ready, int probed_absent);

/* The probed-absent state AFTER observing this byte's readiness: 1 = still
 * believed absent, 0 = a transmitter answered, so the boot-time verdict is
 * retired for the rest of the boot.
 *
 * WITHOUT THIS, THE FIRST-PROBE POLICY IS ONLY SAFE FOR THE FIRST BYTE, which
 * is a trap the ordering argument walks straight into. On a machine whose
 * scratch probe false-negatived, byte 1 finds THRE set and goes out -- and byte
 * 2, issued immediately while the shift register is still draining byte 1,
 * finds THRE CLEAR and takes SKIP_WAIT. The record would emit roughly one
 * character and drop the rest, which is a worse silencing than the one the
 * refusal to zero s_serial_port was protecting against.
 *
 * A transmitter that asserts THRE exists, whatever the boot probe concluded, so
 * one observation is enough to retire the verdict permanently. A genuinely
 * absent UART never asserts it and never clears the flag, so it keeps the
 * budget saving this was added for. Monotonic in the safe direction: the state
 * only ever moves from absent to present. */
int serial_emerg_probe_absent_next(int thre_ready, int probed_absent);

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
void     serial_emerg_reset_for_test(void);

/* A RESERVATION THAT WAS SPENT, versus one that was merely TAKEN.
 *
 * serial_emerg_mark_timeout records that the wait this token paid for was never
 * answered by the transmitter, which is what keeps a later reclamation from
 * handing the charge back. Returns 1 when the claim is (or already was) marked,
 * and 0 when the token authorises nothing. It authorises nothing when ANY of
 * these hold, and the shared validator rejects them in this order:
 *   - the VALID bit is clear;
 *   - any bit outside the minted token fields is set (most plausibly a claim
 *     word passed where a token belongs, since the two layouts disagree about
 *     bit 16);
 *   - the token's generation is not the LIVE latch generation;
 *   - its slot index is not below SERIAL_EMERG_STUCK_BYTES;
 *   - the claim there is not live in that generation;
 *   - the claim is owned by a different CPU.
 * serial_emerg_return rejects exactly the same six, plus a claim already marked.
 *
 * THE TWO ARE NOT THE SAME CONTRACT, and the difference matters:
 *   - mark is IDEMPOTENT while the reservation is held: it is a transition to a
 *     state, so marking twice reports the same result and changes nothing.
 *   - return is SINGLE-USE and never repeatable: it consumes the reservation,
 *     and a second return releases whatever now occupies that slot.
 * What they share is only the outer bound: a token is {generation, slot} and
 * nothing more, so once a reservation is returned or reclaimed, the next reserve
 * reissues a BYTE-IDENTICAL token for a different one. Past that point neither
 * call is safe -- a late mark charges the replacement, making an untouched
 * allowance permanently unreclaimable, and a late return frees it. Telling
 * incarnations apart needs a per-slot counter, and the latch word has six free
 * bits against the sixteen that would take, so this is a caller contract rather
 * than an enforced invariant. Every in-tree caller satisfies it by holding one
 * local token across one interrupt-disabled region.
 *
 * serial_emerg_reclaim_self hands back every allowance this CPU still holds that
 * carries no such mark, and returns how many it freed. It is the refund a CPU
 * about to PARK FOREVER owes the rest of the machine: every panic invocation
 * recorded against it -- an outer dump and any nested abort inside it -- left
 * reservations that no surviving frame will ever return, and an unreturned
 * reservation is a full-length wait subtracted from the next real panic's
 * budget. Genuine timeouts are left standing, so the ceiling stays monotonic in
 * the waits the machine actually paid.
 *
 * CALL RECLAIM ONLY WHERE THE CALLER NEVER RETURNS. An outer writer frame
 * interrupted mid-wait still holds a token, and freeing its slot lets the next
 * reservation take the same one -- so a frame that resumed would return a charge
 * it no longer owns.
 *
 * WHAT MAKES IT SOUND IN panic.c IS NOT PROXIMITY TO THE PARK. The
 * async-isolation branch calls this well before it parks, and in between it
 * stops identifying itself as async, publishes completion, emits a diagnostic
 * and hands back the UART lock -- so across most of that tail a nested abort
 * does NOT re-enter the branch, it takes the terminal path. The invariant is
 * that EVERY disposition reachable from the call is non-returning, so none
 * unwinds to the interrupted writer. A future caller must satisfy THAT, not
 * merely sit close to a halt. */
int      serial_emerg_mark_timeout(uint32_t token);
uint32_t serial_emerg_reclaim_self(void);

/* The timeout transition as ONE named function: mark the reservation, THEN push
 * the byte. Internal to the driver, but external linkage deliberately -- the
 * ordering is the property, no post-state distinguishes it from the reverse, and
 * tools/atomic-claim-check asserts against the disassembly of this symbol that
 * the locked compare-exchange precedes the port write. */
void     serial_emerg_timeout_byte(uint32_t token, uint32_t ledger, uint8_t byte);

/* Test-only override of the ledger identity, so one CPU can prove that a parking
 * CPU's reclamation cannot consume another CPU's charge -- the defining property
 * of the per-CPU ledger, and one a same-CPU test cannot tell apart from a global
 * counter. Pass SERIAL_EMERG_NO_OWNER to restore the real CPUID identity; a test
 * that sets it MUST restore it. Affects ONLY the ledger: the panic owner claim
 * and the rerouted-write routing predicate keep reading real CPUID.
 *
 * DECLARED ONLY IN A TEST BUILD. The override is a single global, so while it is
 * set every CPU resolves to the same ledger identity and a panic on any AP could
 * reclaim another CPU's allowances -- the exact isolation this ledger promises.
 * It is therefore compiled out under KERNEL_TESTS=off rather than merely left
 * unused, and the declaration is gated so a production caller fails to build
 * instead of linking against a seam that is not there. */
#ifdef KERNEL_TESTS
void     serial_emerg_set_ledger_id_for_test(uint32_t id);
#endif

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
