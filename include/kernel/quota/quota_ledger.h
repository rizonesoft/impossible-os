/* quota_ledger.h -- refcounted charge ledger and the per-task charge gate.
 *
 * Section 11 of the kernel resource-accounting roadmap. Two mechanisms live
 * here, and they are separate on purpose:
 *
 *   THE GATE (per task, stored in struct task) serializes CHARGERS against a
 *   membership transition. It is a single packed atomic word, so it costs no
 *   lock section and needs no allocation -- which is why EVERY chain charge can
 *   pass through it, including the ones whose receipt lives in the caller's own
 *   storage (ALPC messages, notification state) rather than in a ledger.
 *
 *   THE LEDGER (allocated on demand, refcounted) owns receipt STORAGE whose
 *   lifetime is independent of the task slot. A charge recorded here can be
 *   returned after its creating task has died and its PID slot has been
 *   recycled, because the returner holds a reference to the ledger rather than
 *   a pointer into the task array.
 *
 * WHY THE SPLIT. The obvious design puts the gate inside the ledger and
 * allocates the ledger at task init. Both halves of that are wrong. Gating on a
 * structure that may not exist yet forces the charge path to ALLOCATE --
 * introducing an out-of-memory failure mode into a path documented as callable
 * at DISPATCH_LEVEL, and one the caller cannot do anything useful about. And
 * allocating a ledger for every task spends memory on the overwhelming majority
 * of tasks that never hold a ledger obligation at all. Splitting them makes the
 * gate free and unconditional, and the ledger lazy and optional.
 *
 * It also makes the barrier COMPLETE on the charge side, which a ledger-hosted
 * gate could not be: the two charging subsystems that exist today keep their
 * receipts in their own structures, so a gate reachable only through a ledger
 * would never see them. Every chain charge funnels through quota_charge_chain,
 * so that is where the gate is entered, and no charger can route around it.
 *
 * WHAT THE LEDGER IS FOR (and why it is not merely a list). A charge is
 * identified by a {receipt, token} pair whose whole purpose is to stay valid
 * across storage reuse (see the receipt-identity contract in quota.h). A task
 * cannot own that pair for a resource that outlives it: an object body or a
 * namespace entry can outlive its creator, so a registry embedded in the
 * reusable task slot would hand out identities that a recycled PID silently
 * inherits. The ledger is that registry, lifted out of the task and refcounted,
 * so the OBLIGATION keeps its own storage alive.
 *
 * WHAT IS NOT COVERED. The barrier covers CHARGES. A return does not pass
 * through it, because quota_return_chain is handed a receipt and a token and
 * never learns which task owns them -- and giving the receipt a back-pointer to
 * find out would grow every embedded receipt, which is the cost the ALPC
 * receipt-compaction work exists to reduce. That is sound rather than merely
 * accepted: a return landing beside a membership transition reduces the
 * process and user blocks and leaves the job's absorbed copy standing until
 * detach, which is a bounded over-hold of job headroom, not a lost or
 * duplicated charge. Closing it is owned as concrete follow-up work.
 *
 * ------------------------------------------------------------------------- */
#ifndef KERNEL_QUOTA_LEDGER_H
#define KERNEL_QUOTA_LEDGER_H

#include "kernel/types.h"
#include "kernel/atomic.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/quota/quota.h"

struct task;
struct quota_block;

/* ========================================================================== *
 * The per-task charge gate
 *
 * ONE 64-bit word carries both the gate STATE and the count of chargers
 * currently in flight, and every transition is a single compare-and-swap on the
 * pair. Splitting them into a state flag beside a counter does not work, in
 * either order, for the same reason the receipt tag is one word: a closer can
 * publish CLOSED, observe a zero count, and begin absorbing while a charger
 * that already read OPEN increments afterwards -- the charge then lands inside
 * the very window the quiesce exists to empty. Entry increments ONLY while the
 * state is OPEN, in the same atomic step that observes it, so a charger that
 * gets in is counted before any closer can see zero.
 *
 * STATES
 *   OPEN    -- charges admitted. The steady state, and the zero value, so a
 *              zero-initialized task slot is a task whose gate is open.
 *   CLOSED  -- transient. A membership transition is being prepared: new
 *              chargers are refused with STATUS_RETRY and the closer waits for
 *              the in-flight count to drain to zero.
 *   SEALED  -- permanent, published at task death. New charges are refused for
 *              good. RETURNS are unaffected -- a sealed task's outstanding
 *              obligations must still be returnable, or every resource it
 *              charged for would leak.
 *
 * SEALED OUTRANKS CLOSED. A quiesce that times out, or one whose transition is
 * then refused, restores OPEN by CASing away its own CLOSED -- and that CAS
 * fails, harmlessly, if death advanced the state to SEALED in the meantime. A
 * reopen must never resurrect a dead task's gate.
 * ========================================================================== */

#define QUOTA_GATE_STATE_BITS   2u
#define QUOTA_GATE_STATE_MASK   ((1u << QUOTA_GATE_STATE_BITS) - 1u)

#define QUOTA_GATE_OPEN         0u
#define QUOTA_GATE_CLOSED       1u
#define QUOTA_GATE_SEALED       2u

#define QUOTA_GATE_PACK(state, count) \
    ((int64_t)(((uint64_t)(count) << QUOTA_GATE_STATE_BITS) | (uint64_t)(state)))
#define QUOTA_GATE_STATE(w)     ((uint32_t)((uint64_t)(w) & QUOTA_GATE_STATE_MASK))
#define QUOTA_GATE_COUNT(w)     ((uint64_t)(w) >> QUOTA_GATE_STATE_BITS)

/* An in-flight count this large is not reachable by any real workload (it would
 * need that many CPUs inside one task's charge path at once); the ceiling exists
 * so the count can never carry into the state field, which would silently
 * reinterpret a busy gate as a sealed one. */
#define QUOTA_GATE_COUNT_MAX    (0x3FFFFFFFFFFFFFFFull)

/* Bounded drain wait. Expressed as an iteration count rather than a duration
 * because the wait must behave identically on TCG, KVM, WHPX, and bare metal,
 * whose timing differs by more than an order of magnitude -- the same reasoning
 * the charge-path cost contract in quota.h uses for lock sections. Each
 * iteration is one acquire load of the gate word.
 *
 * It bounds a window that is already short: an in-flight charger is inside
 * quota_charge_chain, which enters a fixed number of block critical sections and
 * never blocks. The bound exists for the pathological case (a charger preempted
 * by higher-IRQL work), where giving up and refusing the transition is strictly
 * better than spinning on a CPU the charger needs to make progress.
 *
 * CALIBRATED FOR THE SLOWEST GATE-ADMITTED OPERATION, WHICH IS NOT A PLAIN
 * CHARGE. The tempting justification -- "an admitted charger never blocks" -- is
 * FALSE now that quota_ledger_adjust is gate-admitted too: it waits while
 * acquiring up to QUOTA_CHAIN_MAX block spinlocks, so under cross-CPU contention
 * an in-flight operation can legitimately outlast a tight bound. Sizing for the
 * charge alone would make live job assignment fail with STATUS_RETRY whenever
 * quota traffic is heavy, which is a functional regression, not a slow path.
 *
 * So this sits deliberately between the two failure modes. An earlier
 * 1-million-iteration bound was pure CPU burn (a fraction of a millisecond at
 * best, many milliseconds with the gate word bouncing between caches or under
 * emulation, spent only to give up); 16K was too tight for a lock-contended
 * adjust. 128K dependent loads is tens of microseconds on real silicon.
 *
 * WHAT THIS BUDGET IS NOT: a bound on the operation it waits for. It cannot be.
 * quota_charge_adjust acquires its block locks through spin_lock_irqsave, which
 * has no deadline, so a lock holder descheduled by a hypervisor makes the
 * admitted operation arbitrarily long and NO polling budget can cover it. Raising
 * this constant moves a probability, it does not establish a guarantee, and the
 * comment should not pretend otherwise.
 *
 * The consequence when the budget does expire is deliberately the mild one: the
 * quiesce restores OPEN and reports STATUS_RETRY, so a live job assignment fails
 * transiently and retryably rather than proceeding over a chain it did not
 * actually drain. Nothing is mis-charged and nothing leaks.
 *
 * A real bound needs the admitted operation itself bounded -- ordered try-lock
 * with backoff instead of IRQ-off blocking acquisition -- and validating either
 * needs cross-CPU contention the scheduler cannot express yet. Both are owned by
 * the concrete follow-up item that carries this exact scope; see the
 * charge-path-cost section of the resource-accounting roadmap. */
#define QUOTA_GATE_DRAIN_SPINS  (1u << 17)

/* Attempts to publish CLOSED before giving up. Separate from the drain bound
 * because it counts something completely different: this CAS only loses when
 * another CPU changed the in-flight count in the same instant, so a handful of
 * attempts is plenty and a large bound would be a million locked read-modify-
 * writes on a contended line. */
#define QUOTA_GATE_CLAIM_TRIES  64u

_Static_assert(QUOTA_GATE_SEALED <= QUOTA_GATE_STATE_MASK,
               "gate state values must fit under QUOTA_GATE_STATE_MASK, or a "
               "state would bleed into the in-flight count");
_Static_assert(QUOTA_GATE_PACK(QUOTA_GATE_OPEN, 0) == 0,
               "a zero-initialized task slot must decode as an OPEN, empty gate");
_Static_assert(QUOTA_GATE_COUNT(QUOTA_GATE_PACK(QUOTA_GATE_SEALED, 5)) == 5,
               "packing must round-trip an in-flight count beside a state");
_Static_assert(QUOTA_GATE_STATE(QUOTA_GATE_PACK(QUOTA_GATE_SEALED, 5))
                   == QUOTA_GATE_SEALED,
               "packing must round-trip a state beside an in-flight count");

/* Admit one charger. Returns 1 when the caller is counted in the gate and MUST
 * pair the call with quota_gate_exit, 0 when the charge must be refused.
 *
 * `out_state` (optional) receives the state observed on refusal, so the caller
 * can distinguish a transient CLOSED (retry) from a permanent SEALED (the task
 * is dying). Callable from any context: one CAS loop, no lock. */
int  quota_gate_enter(struct task *task, uint32_t *out_state);

/* Release one charger. Preserves whatever state the word carries, because a
 * close or a seal may have landed while this charger was in flight. */
void quota_gate_exit(struct task *task);

/* Close the gate and wait for in-flight chargers to drain.
 *
 * On STATUS_SUCCESS the caller owns the quiesce and MUST reach
 * quota_gate_reopen on every path, including every failure path. The caller is
 * then the only agent that can change this task's charged usage through the
 * chain API, which is what makes an absorb-then-publish sequence atomic with
 * respect to charging.
 *
 * PASSIVE_LEVEL ONLY. The wait is a bounded spin, and a charger on another CPU
 * may be preempted by higher-IRQL work while inside its charge; spinning for it
 * from an elevated IRQL could deny the very CPU time the drain is waiting on.
 *
 * Returns STATUS_RETRY when another quiesce already owns the gate or when the
 * drain did not complete within the bound (OPEN is restored in that case and
 * nothing is owed), STATUS_PROCESS_IS_TERMINATING when the gate is SEALED, and
 * STATUS_INVALID_PARAMETER on a NULL task. */
NTSTATUS quota_gate_quiesce(struct task *task);

/* End a quiesce this caller owns. No-op if death has since SEALED the gate. */
void quota_gate_reopen(struct task *task);

/* Publish SEALED at task death: no further charges, forever. Idempotent, and
 * safe on the log-free death-teardown path (one CAS loop, no lock, no klog).
 * Returns without waiting -- an operation that already owns a slot completes on
 * its own, and the reap-time release is what waits for it. */
void quota_gate_seal(struct task *task);

/* Observed gate state, for diagnostics and tests. QUOTA_GATE_SEALED for NULL,
 * which is the fail-closed answer (a task that does not exist cannot charge). */
uint32_t quota_gate_state_of(struct task *task);

/* In-flight chargers, for diagnostics and tests. */
uint64_t quota_gate_inflight(struct task *task);

/* ========================================================================== *
 * The ledger
 * ========================================================================== */

typedef struct quota_ledger quota_ledger_t;

/* A caller's proof of one outstanding obligation.
 *
 * It is a VALUE, deliberately: `ledger` carries a counted reference, so the slot
 * storage cannot be freed while this handle exists, and `token` identifies the
 * charge WITHIN that storage, so a stale handle whose charge was already
 * returned matches nothing rather than returning a later charge that reused the
 * slot. Copy it beside the resource whose lifetime it tracks; never keep a
 * pointer to someone else's obligation and read the token later, which is the
 * exact lifetime error the receipt-identity contract in quota.h describes. */
typedef struct quota_obligation {
    quota_ledger_t *ledger;   /* holds one reference; NULL when empty      */
    uint32_t        slot;     /* slot index, stable for the ledger's life  */
    uint64_t        token;    /* the charge's generation; 0 when empty     */
    uint64_t        epoch;    /* THIS allocation of that slot; 0 when empty */
} quota_obligation_t;

/* WHY THE HANDLE CARRIES AN EPOCH AS WELL AS A TOKEN. The token identifies the
 * CHARGE; the epoch identifies the ALLOCATION OF THE SLOT that holds it, and the
 * two are not the same fact. A slot returns to IDLE at the generation the return
 * completed, so "the tag reads IDLE at my token" is true both for the returner
 * that just finished AND for a stale duplicate of that same handle arriving
 * later. Without the epoch, that stale duplicate would conclude it had returned
 * the charge and release a slot a DIFFERENT charger had meanwhile claimed and was
 * about to fill -- two live obligations in one slot. The slot is therefore
 * released only by a compare-and-swap against the exact epoch that claimed it,
 * which a stale handle can never match. */

/* Get this task's ledger, creating it on first use, and return it with ONE
 * reference the caller must release with quota_ledger_deref.
 *
 * Refuses (NULL) when the task's gate is SEALED: a ledger created for a task
 * that will never be reaped again could never be released, so the request is
 * refused rather than leaked. Also NULL on allocation failure. PASSIVE_LEVEL (it
 * allocates); callers that merely need to CHARGE use quota_ledger_charge, which
 * acquires internally. */
quota_ledger_t *quota_ledger_acquire(struct task *task);

void quota_ledger_ref(quota_ledger_t *ledger);
void quota_ledger_deref(quota_ledger_t *ledger);

/* Charge `amount` of `type` against `task`'s whole principal chain and record
 * the obligation in a ledger slot.
 *
 * This is quota_charge_chain plus ledger-owned storage: the charge is
 * all-or-nothing across the chain exactly as before, but the receipt lives in
 * the ledger, so the resource that was charged for may outlive the task. On
 * success `*out` holds the obligation (and one ledger reference); on failure it
 * is left empty and nothing is charged.
 *
 * Returns whatever quota_charge_chain returns, plus STATUS_RETRY (a membership
 * transition is in progress -- the caller may retry),
 * STATUS_PROCESS_IS_TERMINATING (the task is dying), or
 * STATUS_INSUFFICIENT_RESOURCES (no free slot and no memory to grow). */
NTSTATUS quota_ledger_charge(struct task *task, quota_resource_type_t type,
                             uint64_t amount, quota_obligation_t *out);

/* Return an obligation and empty the handle. Safe on an empty handle, safe to
 * call twice (the second is a no-op), and safe after the charging task has died
 * and its slot has been recycled -- which is the whole point.
 *
 * Returns 1 when the charge was credited back and the handle emptied, 0 when it
 * was NOT. A return fails to complete for exactly one reason: colliding with an
 * in-progress quota_charge_adjust on this same charge, for longer than
 * QUOTA_LEDGER_RETURN_TRIES attempts. In that case the handle is emptied and its
 * reference dropped WITHOUT the charge being credited -- the obligation is
 * ABANDONED to the ledger, whose own drain reclaims the charge and counts it as a
 * leak at the task release or at destruction.
 *
 * Abandoning rather than retaining the handle is deliberate, and it is what makes
 * that backstop real: a retained reference would hold the ledger's refcount above
 * the task's own claim forever, and the drain only runs when nothing else holds
 * the ledger -- so "keep the handle safe" would in fact mean "this charge, its
 * block references, and its slot live until reboot". A caller that CAN retry
 * should prefer to, because a reclaimed charge is reported as a leak.
 *
 * HOW NARROW THAT CASE IS, precisely, because the honest bound matters more than
 * a reassuring one. Every operation that publishes BUSY on a receipt -- the
 * adjust, the job migration, and its revert -- masks interrupts across its whole
 * claim-to-republish window, so a return can NEVER observe BUSY from an interrupt
 * on the owner's own CPU. That was the one unrecoverable shape: the returner could
 * not progress and neither could the owner it was waiting for. What remains is a
 * cross-CPU collision, where the owner is genuinely running and the retry
 * normally wins. It can still lose if the owner is itself queued behind a
 * contended block spinlock for longer than the retry bound, and THAT is the case
 * abandonment exists to make safe rather than silent.
 *
 * Closing it completely needs a pending-return handoff every BUSY owner consumes
 * before republishing, and validating such a protocol needs cross-CPU contention
 * that the scheduler cannot express yet -- so it is tracked as concrete follow-up
 * work rather than half-built here.
 *
 * 0 is also returned for an empty handle, which owes nothing. */
int quota_ledger_return(quota_obligation_t *ob);

/* Attempts quota_ledger_return makes internally before reporting non-completion.
 * Each attempt re-enters the bounded same-generation BUSY retry, with the tag
 * re-read in between, so a collision with an adjust that is making progress is
 * resolved here rather than pushed onto the caller. */
#define QUOTA_LEDGER_RETURN_TRIES  4u

/* Change an outstanding obligation's amount as one all-or-nothing transaction.
 * See the quota_charge_adjust contract in quota.h for the transaction semantics.
 *
 * This is the GATED entry point, and that is the reason to prefer it over calling
 * quota_charge_adjust directly: an adjust RAISES charged usage, so it must be
 * serialized against a membership transition exactly as a charge is. Without the
 * gate an obligation could grow from 100 to 200 between the absorb and the
 * publication -- the absorb folded in 100, the migration can only adopt the 100
 * the absorb record covers, and the job ends up under-charged by the difference
 * while the process carries all of it. Refused with STATUS_RETRY during a
 * quiesce, and with STATUS_PROCESS_IS_TERMINATING once the owner is sealed.
 *
 * The owner is found through the ledger, so an obligation whose ledger has
 * outlived its task adjusts ungated -- correctly, because a task that no longer
 * owns this ledger can no longer be joining a job with it. */
NTSTATUS quota_ledger_adjust(quota_obligation_t *ob, uint64_t new_amount);

/* Outstanding (live) obligations in this ledger. Diagnostics and tests. */
uint32_t quota_ledger_outstanding(quota_ledger_t *ledger);

/* Slots this ledger has storage for. Grows as obligations demand it; the tests
 * use it to prove the overflow chunks are reached and then reused. */
uint32_t quota_ledger_capacity(quota_ledger_t *ledger);

/* Never-reused identity, for diagnostics. 0 for NULL. */
uint64_t quota_ledger_id(quota_ledger_t *ledger);

/* Migrate this task's outstanding obligations into `job_block`, ADOPTING the
 * amount `rec` already folded in rather than charging the job a second time.
 *
 * The caller MUST hold a successful quota_gate_quiesce on `task` and must not
 * yet have published the membership. For each obligation the migration can
 * claim, the job block is appended to that obligation's charged set and the SAME
 * amount is subtracted from `rec` -- so the job's total usage does not change at
 * all, only WHO is obliged to give it back. That is the point: the absorb has
 * already charged the job for the joiner's whole current usage, which includes
 * these obligations, so issuing a second charge here would double-count, and a
 * second charge is also the only thing that could FAIL -- removing it makes the
 * migration unable to fail partway.
 *
 * Each slot is claimed with its own exact generation (ACTIVE to BUSY) for the
 * duration of its adoption, so a returner can never walk a half-rewritten
 * charged set. Obligations that cannot be migrated stay with the absorb record
 * and are withdrawn at detach exactly as before: one whose charged set is
 * already QUOTA_CHAIN_MAX deep, one whose amount the absorb record does not
 * cover, one already naming this job, and one a returner is claiming right now.
 * Conservation holds in every case -- an obligation is owed to the job either
 * through its own receipt or through the absorb record, never both and never
 * neither.
 *
 * Returns the number of obligations migrated. Never fails. */
uint32_t quota_ledger_migrate_to_job(struct task *task,
                                     struct quota_block *job_block,
                                     quota_absorb_record_t *rec);

/* Undo a migration whose assignment was then refused, restoring `rec` so the
 * caller's quota_job_unabsorb gives the job back everything the absorb took.
 *
 * Must be called inside the SAME quiesce as the migration it reverses, before
 * the membership is published. It is unambiguous about what to undo because of
 * one invariant: a migration only runs for a task that is not yet a member of
 * this job, so no obligation could legitimately name this job block already --
 * every receipt naming it was written by that migration.
 *
 * An obligation a returner is claiming right now is SKIPPED, and that is
 * conserving rather than lossy: the return credits the job through the block the
 * migration appended, which is exactly the amount `rec` no longer carries. So
 * the job ends up back where it started whether the undo or the return gets
 * there first.
 *
 * Returns the number of obligations reverted. Never fails. */
uint32_t quota_ledger_unmigrate_from_job(struct task *task,
                                         struct quota_block *job_block,
                                         quota_absorb_record_t *rec);

/* Release the TASK's claim on its ledger, at reap.
 *
 * Called after the handle sweep, when every charge the task itself owned has
 * been returned. It waits (bounded, PASSIVE_LEVEL) for in-flight gate work to
 * finish, then decides what any REMAINING obligation means -- and that decision
 * is what keeps the leak assertion honest:
 *
 *   Other holders exist (refcount above the task's own claim): the remaining
 *   obligations belong to resources that legitimately outlive the task, which is
 *   the case this whole file exists to support. Nothing is drained, nothing is
 *   reported; the ledger dies with the last holder.
 *
 *   No other holder exists: any live obligation is ORPHANED -- nobody holds a
 *   token able to return it, so its usage would sit on the blocks forever. Those
 *   are returned here and counted as leaks, because an unreturnable charge is
 *   exactly the bug a sweep is meant to surface.
 *
 * Idempotent; no-op for a task that never allocated a ledger. */
void quota_ledger_task_release(struct task *task);

/* Total orphaned obligations reclaimed since boot. Counted rather than logged on
 * the reap path, which is reachable at elevated IRQL; the boot leak sweep and
 * the quota dashboard read this. */
uint64_t quota_ledger_leak_count(void);

/* Obligations ABANDONED by a return that could not complete (see
 * quota_ledger_return). The contract above claims this needs a cross-CPU
 * collision with a BUSY owner queued behind a contended block lock, which makes
 * it rare -- this counter is what makes that claim checkable instead of merely
 * asserted. A nonzero value paired with a rising leak count is the signature
 * worth investigating. */
uint64_t quota_ledger_abandon_count(void);

/* Gate refusals observed by chargers since boot, split by cause. Diagnostics: a
 * nonzero retry count means membership transitions are contending with
 * charging, which is expected under load and a bug only if it never settles. */
uint64_t quota_gate_retry_count(void);
uint64_t quota_gate_sealed_refusal_count(void);

/* Drain waits that hit QUOTA_GATE_DRAIN_SPINS and refused their transition. A
 * nonzero value means a charger stayed in flight past the bound, which is the
 * one case where a job assignment fails for a reason its caller did nothing to
 * cause -- worth surfacing rather than hiding. */
uint64_t quota_gate_drain_timeout_count(void);

#endif /* KERNEL_QUOTA_LEDGER_H */
