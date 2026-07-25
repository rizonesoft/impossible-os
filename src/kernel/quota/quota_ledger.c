/* quota_ledger.c -- the per-task charge gate and the refcounted charge ledger.
 *
 * Contract, rationale, and the reasoning behind every design choice here live in
 * include/kernel/quota/quota_ledger.h. This file is the mechanism.
 *
 * ------------------------------------------------------------------------- */
#include "kernel/quota/quota.h"
#include "kernel/quota/quota_ledger.h"
#include "kernel/sched/task.h"
#include "kernel/sched/irql.h"   /* KeGetCurrentIrql for the diagnostic gate */
#include "kernel/sched/dpc.h"    /* threaded DPC for deferred completion */
#include "kernel/mm/heap.h"      /* kmalloc_zeroed / kfree */
#include "kernel/klog.h"

/* ========================================================================== *
 * Diagnostics
 *
 * All four are plain 64-bit counters mutated with relaxed atomics: they are
 * reported, never branched on, and every path that bumps one is reachable at
 * elevated IRQL where a klog would be unsafe (the same counted-not-logged
 * discipline quota_note_unabsorb_refused established).
 * ========================================================================== */
static uint64_t g_gate_retry_refusals;
static uint64_t g_gate_sealed_refusals;
static uint64_t g_gate_drain_timeouts;
static uint64_t g_ledger_leaks;

/* Charges refused by the per-task obligation ceiling, saturating.
 *
 * THE CEILING NEEDS ITS OWN INSTRUMENT because it refuses BEFORE
 * quota_charge_chain runs, so none of the existing quota telemetry can see it:
 * no block declined this charge, no block failure counter moves, and no quota
 * failure event is emitted. Once a consumer is converted, a task could sit at
 * its cap failing every request while every existing dashboard showed a clean
 * subsystem -- an actionable-looking status with nothing to act on.
 *
 * Bumping a BLOCK's failure counter instead would be the wrong repair and not
 * merely a lazy one: those counters mean "this principal's limit for this
 * resource was reached", and this refusal is about ledger obligations, not
 * about any block's limit. A separate counter tells the truth; a borrowed one
 * would corrupt the meaning of an existing instrument. The per-task, per-type
 * counts remain queryable through quota_ledger_task_obligations_of, so this
 * answers "is the ceiling biting at all" and that answers "where". */
static uint64_t g_ceiling_refusals;

/* The same refusals, split by resource type, so the counter says WHICH class is
 * being refused rather than only that something was.
 *
 * SYSTEM-WIDE RATHER THAN PER-LEDGER, for the reason the attribution table in
 * quota.h gives for its own totals: a per-ledger array would be paid for by
 * every task that ever takes one obligation, and it would DIE WITH THE LEDGER --
 * the evidence disappearing exactly when the offending task exits, which is
 * when an operator starts looking. Sixteen global saturating cells have no
 * lifetime at all.
 *
 * WHAT THIS STILL DOES NOT ANSWER is which PRINCIPAL was refused. That needs a
 * durable per-principal record, which means either the failure-event ring or
 * the lifetime-safe task enumeration this roadmap has not built yet; it is
 * filed rather than approximated, because a per-task counter that vanishes at
 * reap would look like attribution while providing none. */
static uint64_t g_ceiling_refusals_by_type[QUOTA_RESOURCE_TYPE_COUNT];
static uint64_t g_ledger_abandons;

uint64_t quota_gate_retry_count(void)
{
    return __atomic_load_n(&g_gate_retry_refusals, __ATOMIC_RELAXED);
}

uint64_t quota_ledger_ceiling_refusal_count(void)
{
    return __atomic_load_n(&g_ceiling_refusals, __ATOMIC_RELAXED);
}

uint64_t quota_ledger_ceiling_refusals_of(quota_resource_type_t type)
{
    if ((uint32_t)type >= (uint32_t)QUOTA_RESOURCE_TYPE_COUNT)
        return 0;
    return __atomic_load_n(&g_ceiling_refusals_by_type[(uint32_t)type],
                           __ATOMIC_RELAXED);
}

uint64_t quota_gate_sealed_refusal_count(void)
{
    return __atomic_load_n(&g_gate_sealed_refusals, __ATOMIC_RELAXED);
}

uint64_t quota_gate_drain_timeout_count(void)
{
    return __atomic_load_n(&g_gate_drain_timeouts, __ATOMIC_RELAXED);
}

uint64_t quota_ledger_leak_count(void)
{
    return __atomic_load_n(&g_ledger_leaks, __ATOMIC_RELAXED);
}

uint64_t quota_ledger_abandon_count(void)
{
    return __atomic_load_n(&g_ledger_abandons, __ATOMIC_RELAXED);
}

/* ========================================================================== *
 * The gate
 * ========================================================================== */

int quota_gate_enter(struct task *task, uint32_t *out_state)
{
    if (!task) {
        if (out_state)
            *out_state = QUOTA_GATE_SEALED;
        return 0;
    }

    for (;;) {
        int64_t  word  = atomic64_read(&task->quota_gate);
        uint32_t state = QUOTA_GATE_STATE(word);
        uint64_t count = QUOTA_GATE_COUNT(word);

        if (state != QUOTA_GATE_OPEN) {
            if (out_state)
                *out_state = state;
            if (state == QUOTA_GATE_SEALED)
                __atomic_fetch_add(&g_gate_sealed_refusals, 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&g_gate_retry_refusals, 1, __ATOMIC_RELAXED);
            return 0;
        }
        /* Refuse rather than wrap. A count that carried into the state field
         * would reinterpret a busy gate as a sealed one, which is a far worse
         * outcome than refusing a charge at a concurrency level no real
         * workload reaches. */
        if (count >= QUOTA_GATE_COUNT_MAX) {
            if (out_state)
                *out_state = QUOTA_GATE_CLOSED;
            __atomic_fetch_add(&g_gate_retry_refusals, 1, __ATOMIC_RELAXED);
            return 0;
        }

        /* The state observation and the increment are ONE step. That is the
         * whole point: a separate "check OPEN then increment" lets a closer
         * publish CLOSED and observe zero in between, and the charge then lands
         * inside the window the quiesce exists to empty. */
        if (atomic64_cmpxchg(&task->quota_gate, word,
                             QUOTA_GATE_PACK(QUOTA_GATE_OPEN, count + 1)) == word)
            return 1;
        /* Lost the CAS: another charger or a closer moved the word. Retry
         * against what it says now. */
    }
}

void quota_gate_exit(struct task *task)
{
    if (!task)
        return;

    for (;;) {
        int64_t  word  = atomic64_read(&task->quota_gate);
        uint64_t count = QUOTA_GATE_COUNT(word);

        if (count == 0)
            return;    /* unbalanced exit: refuse to underflow the count */

        /* Preserve the STATE. A close or a seal may have landed while this
         * charger was in flight, and an exit that wrote OPEN back would reopen
         * a gate whose owner is still waiting for this very charger to leave. */
        if (atomic64_cmpxchg(&task->quota_gate, word,
                             QUOTA_GATE_PACK(QUOTA_GATE_STATE(word),
                                             count - 1)) == word)
            return;
    }
}

uint32_t quota_gate_state_of(struct task *task)
{
    if (!task)
        return QUOTA_GATE_SEALED;
    return QUOTA_GATE_STATE(atomic64_read(&task->quota_gate));
}

uint64_t quota_gate_inflight(struct task *task)
{
    if (!task)
        return 0;
    return QUOTA_GATE_COUNT(atomic64_read(&task->quota_gate));
}

void quota_gate_seal(struct task *task)
{
    if (!task)
        return;

    /* Unbounded, like quota_block_try_ref's CAS loop: failing to seal is worse
     * than looping, because an unsealed dead task would keep admitting charges
     * against blocks its teardown is releasing. The loop only spins while other
     * CPUs are actively changing the in-flight count, which is bounded work. */
    for (;;) {
        int64_t word = atomic64_read(&task->quota_gate);

        if (QUOTA_GATE_STATE(word) == QUOTA_GATE_SEALED)
            return;    /* idempotent: a doubled death path must not re-seal */

        if (atomic64_cmpxchg(&task->quota_gate, word,
                             QUOTA_GATE_PACK(QUOTA_GATE_SEALED,
                                             QUOTA_GATE_COUNT(word))) == word)
            return;
    }
}

void quota_gate_reopen(struct task *task)
{
    if (!task)
        return;

    for (;;) {
        int64_t word = atomic64_read(&task->quota_gate);

        /* SEALED outranks CLOSED: if death landed while this caller held the
         * quiesce, the gate stays shut forever and this is a no-op. Anything
         * other than CLOSED means this caller no longer owns a close. */
        if (QUOTA_GATE_STATE(word) != QUOTA_GATE_CLOSED)
            return;

        if (atomic64_cmpxchg(&task->quota_gate, word,
                             QUOTA_GATE_PACK(QUOTA_GATE_OPEN,
                                             QUOTA_GATE_COUNT(word))) == word)
            return;
    }
}

/* Wait for the in-flight charger count to reach zero. Returns 1 when drained, 0
 * on timeout. The caller must already have shut the gate (CLOSED or SEALED), or
 * this waits for a moving target. */
static int quota_gate_wait_idle(struct task *task)
{
    for (uint32_t spin = 0; spin < QUOTA_GATE_DRAIN_SPINS; spin++) {
        /* ACQUIRE, not relaxed: observing zero must also make the departing
         * charger's writes to its receipt and to the blocks visible here,
         * otherwise the absorb that follows could read stale usage. */
        int64_t word = atomic64_read(&task->quota_gate);
        if (QUOTA_GATE_COUNT(word) == 0)
            return 1;
    }
    return 0;
}

NTSTATUS quota_gate_quiesce(struct task *task)
{
    if (!task)
        return STATUS_INVALID_PARAMETER;

    /* Claim the close. The CAS preserves the in-flight count, so a charger
     * entering or leaving merely costs a retry rather than a lost transition. */
    int claimed = 0;
    for (uint32_t spin = 0; spin < QUOTA_GATE_CLAIM_TRIES && !claimed; spin++) {
        int64_t  word  = atomic64_read(&task->quota_gate);
        uint32_t state = QUOTA_GATE_STATE(word);

        if (state == QUOTA_GATE_SEALED)
            return STATUS_PROCESS_IS_TERMINATING;
        if (state == QUOTA_GATE_CLOSED)
            return STATUS_RETRY;    /* another quiesce owns the gate */

        if (atomic64_cmpxchg(&task->quota_gate, word,
                             QUOTA_GATE_PACK(QUOTA_GATE_CLOSED,
                                             QUOTA_GATE_COUNT(word))) == word)
            claimed = 1;
    }
    if (!claimed)
        return STATUS_RETRY;

    /* Drain. A seal landing here means the task died mid-transition: the gate is
     * already shut for good, this caller owns no close any more, and it owes no
     * reopen -- which is why the header requires reopen ONLY after success. */
    for (uint32_t spin = 0; spin < QUOTA_GATE_DRAIN_SPINS; spin++) {
        int64_t word = atomic64_read(&task->quota_gate);

        if (QUOTA_GATE_STATE(word) == QUOTA_GATE_SEALED)
            return STATUS_PROCESS_IS_TERMINATING;
        if (QUOTA_GATE_COUNT(word) == 0)
            return STATUS_SUCCESS;
    }

    /* Timed out with a charger still in flight. Restore OPEN rather than leave
     * a live task permanently refusing charges: turning transient contention
     * into a permanent quota refusal is a far worse failure than refusing this
     * one transition. The reopen is itself a CAS that loses to a seal. */
    __atomic_fetch_add(&g_gate_drain_timeouts, 1, __ATOMIC_RELAXED);
    quota_gate_reopen(task);
    return STATUS_RETRY;
}

/* ========================================================================== *
 * The ledger
 *
 * STORAGE SHAPE. Slots live in an APPEND-ONLY chain: a fixed inline run in the
 * ledger itself, then chunks added on demand. Append-only is what makes a slot
 * INDEX a durable name -- resolve it once and the pointer stays valid for the
 * ledger's whole life, so an obligation handle can carry an index instead of a
 * pointer and no reader ever races a reallocation. Nothing is ever removed; a
 * returned slot is reused in place.
 *
 * Each slot embeds an ordinary quota_charge_receipt_t rather than a second
 * identity scheme, so the generation-tagged token protocol that section 4
 * established (and proved) is reused verbatim: the ledger adds STORAGE and
 * OWNERSHIP, not a new notion of charge identity.
 *
 * The slot's `owner` word is the ledger's own allocation identity, deliberately
 * separate from the receipt's tag. It could not be the tag: the tag's IDLE state
 * means "holds no charge", which is true both of a free slot and of a slot whose
 * obligation is allocated but whose charge is still being built. Conflating them
 * would let two chargers pick the same slot.
 *
 * And it is an EPOCH rather than a flag, because a flag cannot be released
 * safely. Release has to answer "is this slot still MINE", and with a 0/1 flag the
 * only available evidence is the receipt tag -- which reads IDLE at a returner's
 * own generation both when that returner just completed AND when a stale
 * duplicate of the same handle arrives long afterwards. The duplicate would then
 * clear a flag belonging to a charger that had already claimed the slot and was
 * about to fill it. A unique epoch, released only by CAS against the exact value
 * that claimed it, makes that impossible: the stale duplicate matches nothing.
 * ========================================================================== */

#define QUOTA_LEDGER_INLINE_SLOTS   8u

/* Slots per chunk. A slot is one receipt plus three words: when the chain ceiling
 * dropped from 8 to 4 the receipt narrowed and the slot fell from 104 to 72
 * bytes, leaving the old count of 24 using most of a chunk on nothing, and 34
 * was chosen then to spend the same chunk on more slots. The obligation-ceiling
 * word added since takes a slot to 80 bytes, so 34 slots is now 2728 bytes per
 * chunk rather than the 2456 that count was originally sized against.
 *
 * THE FOOTPRINT DID GROW, and pretending otherwise is how the next change gets
 * made blind: a fully grown ledger is ~175 KiB rather than ~150 KiB. That is
 * still single-digit percent of the fixed 2 MiB heap and still far below the
 * hundreds of chunks an unbounded ledger would need, which is what the ceiling
 * exists to prevent -- but it is a real cost, paid deliberately for an
 * attribution word the completion path can read without racing the receipt. This
 * heap is exact-size first-fit, NOT size-classed, so no size-class boundary is
 * being preserved either. Keep this in step with the receipt width and the slot
 * layout; the EXACT size asserts below are the backstop. */
#define QUOTA_LEDGER_CHUNK_SLOTS    34u

/* Bounded growth attempts per charge, so a pathological allocator cannot turn
 * one charge into an unbounded loop. Three attempts covers "another CPU grew it
 * first, twice" and then gives up with a real status. */
#define QUOTA_LEDGER_GROW_ATTEMPTS  3u

/* HARD CEILING on chunks per ledger, and it is a memory-safety bound rather than
 * a policy preference. The kernel heap is a fixed 2 MiB (src/kernel/mm/heap.c),
 * and a slot embeds a whole QUOTA_CHAIN_MAX-wide receipt, so an unbounded ledger
 * scales badly against it: one obligation per handle at the default handle
 * ceiling would need hundreds of chunks and consume the MAJORITY of the global
 * heap for a SINGLE process. That is a denial of service on every other
 * subsystem, reached without exceeding any quota the process was actually given.
 *
 * Capping turns that into a bounded, attributable refusal
 * (STATUS_INSUFFICIENT_RESOURCES for the charge) instead of global exhaustion --
 * fail-closed, with the failure landing on the process responsible.
 *
 * 64 chunks is 2184 slots and ~175 KiB, so a maxed ledger takes single-digit
 * percent of the heap. The slot count rose from 1544 without touching this number:
 * the compact receipt (QUOTA_CHAIN_MAX sized to the reachable chain depth) cut
 * each slot from 104 bytes to 72, and QUOTA_LEDGER_CHUNK_SLOTS was raised to
 * spend the same chunk on more slots. The per-slot obligation-type word added
 * with the ceiling then took a slot back to 80 bytes (2728 per chunk), which is
 * where the ~150 KiB figure this comment used to quote became ~175 KiB -- both
 * sizes are now pinned by exact asserts beside the chunk declaration so the next
 * such change is deliberate.
 *
 * IT IS STILL A CEILING, and a deliberate one. Raising it far enough to cover a
 * consumer with no finite per-task bound of its own is NOT a matter of picking a
 * bigger number, and two specific things block it:
 *   - Page-backed storage would take this off the fixed 2 MiB heap, but the PMM
 *     bitmap is not SMP-locked and pmm.h forbids lazy allocation on a live call
 *     path outright, which is exactly what ledger growth is. Owned by the PMM
 *     bitmap SMP-locking work in the advanced-allocator roadmap.
 *   - A per-task TOTAL-obligation policy has to exist first. KNF's 4096 bounds ONE
 *     state's subscriber list, not a task's aggregate, so no finite ceiling here
 *     can be derived from it; a task subscribing across many states needs an
 *     explicit total limit with a defined status and defined cross-resource
 *     semantics, so that one resource class cannot starve another's obligations.
 * Both are tracked in the charge-path cost-reduction work; until they land,
 * converting a consumer that has no aggregate bound of its own would impose this
 * ceiling on it. */
#define QUOTA_LEDGER_MAX_CHUNKS     64u

/* Attempts the obligation-ceiling reservation makes before reporting contention
 * rather than exhaustion. Sized like QUOTA_GATE_CLAIM_TRIES and for the same
 * reason: every failed attempt means another CPU committed a reservation on
 * this exact counter, so reaching the bound needs sixty-four consecutive losses
 * on one resource type of one task. */
#define QUOTA_LEDGER_RESERVE_TRIES  64u

/* Where the refusal counters stop instead of wrapping. Named rather than
 * spelled inline because the saturation is a published contract, not an
 * implementation detail of one increment. */
#define QUOTA_LEDGER_REFUSALS_MAX   ((uint64_t)~(uint64_t)0)

/* Set in a slot's `owner` word to mark the slot DEFERRED: its charge is owed to
 * the drain and the slot may not be reclaimed until the drain has completed it.
 *
 * IT IS PART OF THE OWNER WORD ON PURPOSE, because validating the epoch and
 * claiming the deferral have to be ONE transition. Reading the epoch and then
 * CAS-ing a separate token word leaves an ABA window: between the two, another
 * CPU can complete the old obligation, release the slot, and let a fresh charge
 * claim it -- and the stale return then stamps its dead token onto a stranger's
 * charge, whose own return later finds the slot "already deferred", coalesces,
 * and throws away the only handle that could have credited it. A compare-and
 * -swap of `owner` from the exact epoch to epoch|DEFERRED does both at once, and
 * a nonzero owner is precisely what quota_ledger_claim_slot refuses, so the slot
 * cannot be reclaimed while the mark stands.
 *
 * Bit 63 is free: epochs come from a monotonic counter starting at 1, so no
 * reachable epoch has the top bit set. */
#define QUOTA_SLOT_DEFERRED          ((int64_t)((uint64_t)1 << 63))

/* Largest epoch a slot claim may store. The DEFERRED mark owns bit 63, so an
 * epoch that reached it would make `epoch|DEFERRED` equal the epoch itself --
 * the marking CAS would succeed as a no-op, several duplicate returns could all
 * "win" it, and a wrapped counter would mark a fresh slot deferred or store a
 * zero owner that reads as free. 2^63 claims is not reachable by any real
 * workload, but this file's own standard for a packed word is a named ceiling
 * that fails closed (QUOTA_RECEIPT_GEN_MAX in quota.h does exactly this), not a
 * prose claim that the value cannot get there. */
#define QUOTA_SLOT_EPOCH_MAX         ((uint64_t)0x7FFFFFFFFFFFFFFFULL)

_Static_assert((QUOTA_SLOT_EPOCH_MAX & (uint64_t)QUOTA_SLOT_DEFERRED) == 0,
               "the deferred mark must not overlap any storable slot epoch");

/* THE POLICY CEILING MUST STAY BELOW THE STORAGE CEILING, or the two statuses it
 * exists to separate collapse: a task would reach the last slot before it
 * reached its policy limit and be told STATUS_INSUFFICIENT_RESOURCES for what is
 * really a policy decision. Strictly below, not equal -- at equality the last
 * admitted obligation races the last free slot and the reported status depends
 * on which check ran first. */
_Static_assert(QUOTA_LEDGER_TASK_MAX
                   < (QUOTA_LEDGER_INLINE_SLOTS
                      + (QUOTA_LEDGER_MAX_CHUNKS * QUOTA_LEDGER_CHUNK_SLOTS)),
               "the per-task obligation ceiling must sit strictly below the "
               "ledger's storage ceiling, or a policy refusal is reported as "
               "heap exhaustion");

/* THE caps, one row per resource type, in taxonomy order. The admission rule
 * reads this table and quota_ledger_type_cap publishes it, so a caller and the
 * ceiling cannot disagree about what the limit is.
 *
 * Indexed by quota_resource_type_t, so it is a parallel array over an enum that
 * is also a storage index -- the assert below is what makes appending a
 * seventeenth resource type a build failure here rather than a read past the
 * end on that type's first charge. */
static const uint32_t s_type_obligation_cap[] = {
    [QUOTA_RES_HANDLE]              = QUOTA_LEDGER_CAP_HANDLE,
    [QUOTA_RES_OBJECT_BODY]         = QUOTA_LEDGER_CAP_OBJECT_BODY,
    [QUOTA_RES_NAMESPACE_ENTRY]     = QUOTA_LEDGER_CAP_NAMESPACE_ENTRY,
    [QUOTA_RES_PAGED_POOL]          = QUOTA_LEDGER_CAP_PAGED_POOL,
    [QUOTA_RES_NONPAGED_POOL]       = QUOTA_LEDGER_CAP_NONPAGED_POOL,
    [QUOTA_RES_REGISTRY_BYTES]      = QUOTA_LEDGER_CAP_REGISTRY_BYTES,
    [QUOTA_RES_ALPC_MESSAGE]        = QUOTA_LEDGER_CAP_ALPC_MESSAGE,
    [QUOTA_RES_NOTIFICATION_STATE]  = QUOTA_LEDGER_CAP_NOTIFICATION_STATE,
    [QUOTA_RES_TIMER]               = QUOTA_LEDGER_CAP_TIMER,
    [QUOTA_RES_THREAD]              = QUOTA_LEDGER_CAP_THREAD,
    [QUOTA_RES_PROCESS]             = QUOTA_LEDGER_CAP_PROCESS,
    [QUOTA_RES_SECTION]             = QUOTA_LEDGER_CAP_SECTION,
    [QUOTA_RES_MAPPED_VIEW]         = QUOTA_LEDGER_CAP_MAPPED_VIEW,
    [QUOTA_RES_CRASH_BUFFER]        = QUOTA_LEDGER_CAP_CRASH_BUFFER,
    [QUOTA_RES_NOTIFICATION_SUB]    = QUOTA_LEDGER_CAP_NOTIFICATION_SUB,
    [QUOTA_RES_NOTIFICATION_BYTES]  = QUOTA_LEDGER_CAP_NOTIFICATION_BYTES,
};

/* UNSIZED ON PURPOSE, so this assert has something to say. Declaring the array
 * with an explicit [QUOTA_RESOURCE_TYPE_COUNT] bound would make the comparison
 * below trivially true and the guard worthless: a resource type appended to the
 * taxonomy without a cap row here would compile clean and silently get a cap of
 * zero, refusing every charge of a perfectly valid type with
 * STATUS_QUOTA_EXCEEDED. Letting the initializer list decide the size means the
 * count only reaches the taxonomy when a row was actually written -- and the
 * enum's own rule is that new types append at the END, which is exactly the
 * case this catches. A zero left in a MIDDLE row is caught by the unit test
 * that walks every type's cap. */
_Static_assert((sizeof(s_type_obligation_cap)
                / sizeof(s_type_obligation_cap[0])) == QUOTA_RESOURCE_TYPE_COUNT,
               "one obligation cap per resource type; appending a type without "
               "a cap row would give it a silent cap of zero");

uint32_t quota_ledger_type_cap(quota_resource_type_t type)
{
    if ((uint32_t)type >= (uint32_t)QUOTA_RESOURCE_TYPE_COUNT)
        return 0;
    return s_type_obligation_cap[(uint32_t)type];
}

typedef struct quota_ledger_slot {
    quota_charge_receipt_t receipt;
    atomic64_t             owner;   /* 0 = free, else this allocation's epoch */
    /* The resource type this slot's obligation was reserved against, published
     * by the charger that claimed the slot and read by whoever completes it.
     *
     * DUPLICATED FROM THE RECEIPT ON PURPOSE, and atomically. The completion
     * needs the type to release the ceiling budget, and it must read it BEFORE
     * validating its epoch -- after the release CAS the slot is free and a new
     * charger may already be filling it. Reading receipt.type there is a plain
     * load that a stale duplicate can perform while a new owner is writing that
     * same field under its BUSY claim: the VALUE is never used (only an epoch
     * -winning caller releases, and a won CAS proves the slot was continuously
     * ours, epochs being monotonic and never reused), but the race itself is
     * undefined behaviour and the compiler is entitled to act on it. An atomic
     * word costs four bytes per slot and removes the question. */
    atomic_t               rtype;
    /* Nonzero when a raised-IRQL return handed this slot to the drain worker,
     * and it holds the token that return must present. This is the ENTIRE
     * deferral record, which is why the deferral path allocates nothing: the
     * slot the obligation already owns is the storage. The matching epoch is not
     * duplicated here -- `owner` already is that epoch, and the deferring
     * returner is by definition the agent that still owns the slot. */
    atomic64_t             deferred_token;
} quota_ledger_slot_t;

typedef struct quota_ledger_chunk {
    struct quota_ledger_chunk *next;
    quota_ledger_slot_t        slots[QUOTA_LEDGER_CHUNK_SLOTS];
} quota_ledger_chunk_t;

/* kmalloc is documented for allocations up to 4 KB (CLAUDE.md); anything larger
 * must use pmm_alloc_contiguous. Asserting the chunk size here is what keeps a
 * future slot-count or receipt-width change from silently crossing that line. */
/* PIN BOTH SIZES EXACTLY. The <= 4096 assert below only protects the kmalloc
 * ceiling; it says nothing about the fixed 2 MiB heap budget this ledger is
 * sized against, and a field added between two aligned members can grow every
 * slot through padding without tripping it. Adding the per-slot `rtype` word did
 * exactly that -- 72 bytes to 80, and the chunk from 2456 to 2728 -- so the cost
 * is recorded here rather than discovered later. A change that moves either
 * number must confront it and update the arithmetic in the ceiling commentary
 * above, not merely stay under 4096. */
_Static_assert(sizeof(quota_ledger_slot_t) == 80,
               "ledger slot size is a system-wide memory multiplier; update the "
               "heap arithmetic deliberately, with the cost in hand");
_Static_assert(sizeof(quota_ledger_chunk_t) == 2728,
               "ledger chunk size follows the slot size; update it deliberately");
_Static_assert(sizeof(quota_ledger_chunk_t) <= 4096,
               "a ledger chunk must stay within the kmalloc ceiling; reduce "
               "QUOTA_LEDGER_CHUNK_SLOTS or switch to pmm_alloc_contiguous");

struct quota_ledger {
    atomic_t              refcount;   /* task's own claim + one per obligation */
    /* Where the next claim scan STARTS. Without it every charge rescans from slot
     * zero, so filling N slots costs O(N squared) owner reads -- on the order of
     * 10^8 by the time a ledger holds thousands of obligations. The hint makes
     * sequential filling linear; a scan that finds nothing from the hint wraps and
     * checks the front, so correctness never depends on its value. Advisory, hence
     * a plain relaxed atomic: a stale hint costs a few extra reads, never a missed
     * or double-claimed slot (the claim itself is still a CAS). */
    uint32_t              claim_hint;
    uint64_t              id;         /* never reused; diagnostics only        */
    /* The task this ledger was created for, so an adjust can find the gate it
     * must enter. Safe to hold raw and unreferenced for exactly one reason:
     * `tasks[]` is a static array (src/kernel/sched/task.c), so the pointer is
     * always valid MEMORY -- it can only be reused by a later process, never
     * freed. Reuse is detected by checking that the task still points BACK at
     * this ledger, which a recycled slot never does (task creation clears the
     * pointer, and the reap that preceded it cleared it too). */
    struct task          *owner;
    spinlock_t            lock;       /* chunk-list APPEND only                */
    quota_ledger_chunk_t *chunks;     /* append-only; acquire-loaded           */
    uint32_t              capacity;   /* slots addressable; grows only         */
    /* Intrusive link for the global pending-completion list, and the flag that
     * keeps this ledger on it at most once. Both are guarded by g_defer_lock,
     * never by `lock` above: this list is walked from an interrupt, and mixing
     * it with the chunk-append lock would put an allocation-adjacent lock in
     * that path. `defer_destroy` records that the final reference was dropped at
     * raised IRQL, so the drain must destroy rather than dereference. */
    /* --- The per-task obligation ceiling (quota_ledger.h) ------------------ *
     *
     * Counted here rather than on the task because the LEDGER is what an
     * obligation can still reach: three of the four completion paths (the inline
     * return, the drain's credit, the destroy-time orphan reclaim) resolve only
     * a ledger and a slot, and the ledger's `owner` back-pointer is explicitly
     * not trustworthy without the quota_lock-guarded liveness re-check. A
     * counter on the task would be unreachable from exactly the paths that must
     * decrement it. One ledger belongs to one task for its whole life, so
     * per-ledger IS per-task.
     *
     * The count tracks SLOT OCCUPANCY, not handle possession, and the two
     * genuinely differ: a return whose retries are exhausted ABANDONS -- it
     * empties the caller's handle but leaves the charge live and the slot
     * claimed until an orphan drain reclaims it. That charge is still
     * outstanding against the block chain, so it must still be counted; keying
     * on the handle would let an abandoning task charge past its ceiling with
     * unreturned obligations still standing. Occupancy also makes the accounting
     * structural rather than remembered: `owner` moves 0 -> epoch in exactly one
     * place and epoch -> 0 in exactly three, so every transition is paired.
     *
     * ONE COUNTER PER TYPE, and that is what makes the accounting safe without a
     * lock: a charge moves exactly one atomic, so there is no pair that could be
     * observed half-updated and no release that has to work out which of two
     * pools its unit came from. Relaxed is enough -- the counter is its own
     * admission decision and is ordered against nothing else, since the slot
     * claim that follows is its own CAS and the receipt publication is ordered
     * by the tag. */
    atomic_t              obligations[QUOTA_RESOURCE_TYPE_COUNT];
    struct quota_ledger  *defer_next;
    uint8_t               defer_queued;
    /* Written under g_defer_lock at push time, but read and cleared by the drain
     * WITHOUT it. That is deliberate and rests on an invariant rather than the
     * lock: a destroy push only happens at refcount zero, so no other agent can
     * reach the ledger, and the popping pass owns it outright once it is off the
     * list. Stated because the field's neighbours ARE lock-guarded, and the
     * difference would otherwise read as an oversight. */
    uint8_t               defer_destroy;
    /* Where the NEXT drain pass resumes. Without it a ledger holding more
     * pending slots than one pass's budget rescans the same prefix forever:
     * the pass would examine slots 0..budget-1, find them already completed,
     * hit the bound, requeue, and never reach the deferral at a higher index --
     * a live-lock that also spins the worker. Guarded by g_defer_lock. */
    uint32_t              defer_cursor;
    /* Deferrals stamped on this ledger and not yet completed. It is the REQUEUE
     * PREDICATE, and it has to be a count rather than "did I run out of budget":
     * a pass that stops mid-array leaves work behind it, but so does one that
     * finishes the array having started past zero, because a deferral can land
     * on an already-queued ledger at a slot the cursor has gone past. Requeuing
     * on the budget alone misses that one; requeuing whenever the cursor is
     * nonzero never terminates. Requeuing while work remains does both. */
    uint32_t              defer_pending;
    /* Consecutive drain callbacks that completed nothing on this ledger because
     * an adjust held its receipts BUSY. Bounded so a persistently BUSY owner
     * cannot re-arm the shared worker forever; reset by any real progress. */
    uint32_t              defer_retries;
    quota_ledger_slot_t   inline_slots[QUOTA_LEDGER_INLINE_SLOTS];
};

_Static_assert(sizeof(struct quota_ledger) <= 4096,
               "a ledger must stay within the kmalloc ceiling");

/* The per-type counters are a PARALLEL ARRAY over the resource taxonomy: a type
 * appended to the enum without widening this array would index past its end on
 * the very first charge of the new type. */
_Static_assert((sizeof(((struct quota_ledger *)0)->obligations)
                / sizeof(((struct quota_ledger *)0)->obligations[0]))
                   == QUOTA_RESOURCE_TYPE_COUNT,
               "one obligation counter per resource type, or a charge indexes "
               "off the end of the array");

/* Bump a refusal counter, SATURATING at the ceiling rather than wrapping.
 *
 * The header promises saturating values, and a plain fetch_add does not deliver
 * that: at the ceiling it wraps to zero, so a counter whose entire job is to be
 * durable evidence would erase itself and then read as though the refusals had
 * never happened -- a monotonic instrument that goes DOWN is worse than no
 * instrument, because it is believed.
 *
 * Bounded, like every other retry on a path the charge can reach: each failed
 * exchange means another CPU counted a refusal on the same cell, and dropping a
 * count under contention that severe is strictly better than spinning there.
 * The dropped count is a diagnostic imprecision; a wrap is a lie. */
static void quota_ledger_count_refusal(uint64_t *cell)
{
    for (uint32_t attempt = 0; attempt < QUOTA_LEDGER_RESERVE_TRIES; attempt++) {
        uint64_t cur = __atomic_load_n(cell, __ATOMIC_RELAXED);

        if (cur == QUOTA_LEDGER_REFUSALS_MAX)
            return;

        if (__atomic_compare_exchange_n(cell, &cur, cur + 1, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

/* Take one unit of `type`'s obligation budget, or refuse.
 *
 * LINEARIZABLE, AND BOUNDED. The counter is only ever incremented from a value
 * this caller observed to be below the cap, so it never exceeds the cap even
 * transiently -- which matters because the alternative is not merely untidy.
 * An unconditional add-then-roll-back overshoots to cap+1 while it is in
 * flight, and a CONCURRENT charger reading that inflated value is refused with
 * STATUS_QUOTA_EXCEEDED: a permanent, administrator-actionable status handed
 * out for a transient race, at a moment when the budget genuinely had room. A
 * ceiling that fails closed is right; a ceiling that fails PERMANENTLY closed
 * on contention is a bug that looks like policy.
 *
 * The retry bound is what keeps this off the forbidden shape: an unbounded
 * for(;;) on a path reachable with interrupts masked is the defect class the
 * charge-gate loops are already tracked for. Exhausting the bound means real
 * contention, not exhaustion, so it reports STATUS_RETRY -- the status that
 * says "ask again", which is exactly what a caller should do. Every CAS failure
 * means another CPU made progress, so the bound is generous rather than tight. */
static NTSTATUS quota_ledger_reserve_obligation(quota_ledger_t *ledger,
                                                quota_resource_type_t type)
{
    atomic_t     *counter = &ledger->obligations[(uint32_t)type];
    const int32_t cap     = (int32_t)s_type_obligation_cap[(uint32_t)type];

    /* A RESERVATION IS COUNTED FROM THE MOMENT IT IS TAKEN, including the brief
     * window before its charge commits, and a charge that then fails rolls it
     * back. So a charger arriving exactly at the cap during another charger's
     * in-flight reservation is refused for an obligation that may never commit.
     * That is deliberate, and it is the same bargain the layer below already
     * makes: quota_charge_chain charges each block in turn and returns the
     * prefix if a later one refuses (quota.h), so a concurrent reader of those
     * counters likewise sees usage that is transiently high and can be refused
     * on it. Splitting committed from provisional here would make this policy
     * counter STRICTER than the authoritative charge it sits on top of, and it
     * would need two counters moved per charge with the outcome recomputed at
     * release -- precisely the cross-counter protocol whose drift made it
     * possible to admit PAST the ceiling, which is the worse failure by far.
     * A ceiling that is momentarily pessimistic under contention is a bargain
     * worth keeping; one that is momentarily permissive is not. */
    for (uint32_t attempt = 0; attempt < QUOTA_LEDGER_RESERVE_TRIES; attempt++) {
        int32_t cur = atomic_read(counter);

        if (cur >= cap)
            return STATUS_QUOTA_EXCEEDED;   /* a TRUE observation, not an
                                             * in-flight overshoot. NOT counted
                                             * here: the caller may still turn
                                             * this into a gate refusal, and the
                                             * counter must record what the
                                             * CALLER was actually told. */

        if (atomic_cmpxchg(counter, cur, cur + 1) == cur)
            return STATUS_SUCCESS;
    }

    return STATUS_RETRY;
}

/* Give one unit of `type`'s obligation budget back. Callers must invoke this
 * once per successful reserve and never otherwise -- every call site is paired
 * with a slot-occupancy transition, so "once" is a property of the transition
 * rather than of caller discipline. */
static void quota_ledger_release_obligation(quota_ledger_t *ledger,
                                            quota_resource_type_t type)
{
    if ((uint32_t)type >= (uint32_t)QUOTA_RESOURCE_TYPE_COUNT)
        return;     /* never reserved: a charge with this type was refused */

    (void)atomic_sub_fetch_relaxed(&ledger->obligations[(uint32_t)type], 1);
}

/* Source of the never-reused ledger identity. Mutated with a relaxed atomic:
 * uniqueness needs atomicity, not ordering. */
static uint64_t g_ledger_id_next;

/* Source of slot-allocation epochs. GLOBAL and 64-bit rather than per-slot and
 * 32-bit: uniqueness only has to hold against the handles that might still name a
 * slot, and a single monotonic 64-bit counter gives that for the whole system
 * without any wrap a real workload could reach. */
static uint64_t g_slot_epoch_next;

static quota_ledger_t *quota_ledger_peek(struct task *task);

uint64_t quota_ledger_id(quota_ledger_t *ledger)
{
    return ledger ? ledger->id : 0;
}

uint32_t quota_ledger_capacity(quota_ledger_t *ledger)
{
    if (!ledger)
        return 0;
    return __atomic_load_n(&ledger->capacity, __ATOMIC_ACQUIRE);
}

/* Resolve a slot index. NULL when the index is beyond what has been allocated,
 * which a caller treats as "not a valid obligation" rather than an error. */
static quota_ledger_slot_t *quota_ledger_slot_at(quota_ledger_t *ledger,
                                                 uint32_t index)
{
    if (!ledger)
        return (quota_ledger_slot_t *)0;

    if (index < QUOTA_LEDGER_INLINE_SLOTS)
        return &ledger->inline_slots[index];

    uint32_t remaining = index - QUOTA_LEDGER_INLINE_SLOTS;
    /* ACQUIRE on every link: the chunk's zeroed slots must be visible to this
     * CPU before the pointer that reaches them is. */
    quota_ledger_chunk_t *chunk =
        __atomic_load_n(&ledger->chunks, __ATOMIC_ACQUIRE);

    while (chunk) {
        if (remaining < QUOTA_LEDGER_CHUNK_SLOTS)
            return &chunk->slots[remaining];
        remaining -= QUOTA_LEDGER_CHUNK_SLOTS;
        chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE);
    }
    return (quota_ledger_slot_t *)0;
}

/* Single-pass slot iterator.
 *
 * WHY THIS EXISTS rather than a `for (i = 0; i < capacity; i++) slot_at(i)` loop:
 * quota_ledger_slot_at resolves an index by walking the chunk chain from the
 * HEAD, so indexing every slot in turn re-walks the whole chain every time and
 * turns any full walk into O(capacity squared). That is invisible at a handful of
 * obligations and brutal at realistic ones -- a table near the default handle
 * ceiling spans hundreds of chunks, so one walk would perform millions of link
 * loads, and the job-assign path performs it with charging quiesced.
 *
 * The iterator holds its position instead, so a full walk is O(capacity). Index
 * resolution (slot_at) stays for the handle path, where it happens once per
 * return and the caller has a specific index rather than a sweep. */
typedef struct quota_ledger_iter {
    quota_ledger_t       *ledger;
    quota_ledger_chunk_t *chunk;    /* NULL while still in the inline run */
    uint32_t              within;   /* position inside the current run   */
    uint32_t              index;    /* global slot index of the NEXT slot */
    int                   inline_done;
} quota_ledger_iter_t;

static void quota_ledger_iter_init(quota_ledger_iter_t *it, quota_ledger_t *ledger)
{
    it->ledger      = ledger;
    it->chunk       = (quota_ledger_chunk_t *)0;
    it->within      = 0;
    it->index       = 0;
    it->inline_done = 0;
}

/* Next allocated-or-free slot, or NULL at the end. `out_index` receives the
 * slot's global index (the value an obligation handle carries). */
static quota_ledger_slot_t *quota_ledger_iter_next(quota_ledger_iter_t *it,
                                                   uint32_t *out_index)
{
    if (!it->inline_done) {
        if (it->within < QUOTA_LEDGER_INLINE_SLOTS) {
            quota_ledger_slot_t *slot = &it->ledger->inline_slots[it->within];
            if (out_index)
                *out_index = it->index;
            it->within++;
            it->index++;
            return slot;
        }
        /* Inline run exhausted: step onto the first chunk. ACQUIRE so the
         * chunk's zeroed slots are visible before the pointer that reaches
         * them, matching the release-publish in quota_ledger_grow. */
        it->inline_done = 1;
        it->chunk  = __atomic_load_n(&it->ledger->chunks, __ATOMIC_ACQUIRE);
        it->within = 0;
    }

    while (it->chunk) {
        if (it->within < QUOTA_LEDGER_CHUNK_SLOTS) {
            quota_ledger_slot_t *slot = &it->chunk->slots[it->within];
            if (out_index)
                *out_index = it->index;
            it->within++;
            it->index++;
            return slot;
        }
        it->chunk  = __atomic_load_n(&it->chunk->next, __ATOMIC_ACQUIRE);
        it->within = 0;
    }
    return (quota_ledger_slot_t *)0;
}

/* Append one chunk. Allocation happens OUTSIDE the lock (kmalloc under an
 * irqsave spinlock is exactly the pattern the handle-table lock contract warns
 * against), and the link + capacity publication happen under it. */
static int quota_ledger_grow(quota_ledger_t *ledger)
{
    /* Refuse at the ceiling BEFORE allocating: see QUOTA_LEDGER_MAX_CHUNKS. This
     * is what stops one process's obligations from consuming the majority of a
     * fixed 2 MiB kernel heap. */
    const uint32_t cap_max = QUOTA_LEDGER_INLINE_SLOTS
                             + (QUOTA_LEDGER_MAX_CHUNKS * QUOTA_LEDGER_CHUNK_SLOTS);
    if (__atomic_load_n(&ledger->capacity, __ATOMIC_ACQUIRE) >= cap_max)
        return 0;

    quota_ledger_chunk_t *chunk =
        (quota_ledger_chunk_t *)kmalloc_zeroed(sizeof(quota_ledger_chunk_t));
    if (!chunk)
        return 0;

    uint64_t flags;
    spin_lock_irqsave(&ledger->lock, &flags);

    /* Re-check under the lock so concurrent growers cannot stampede past the
     * ceiling: each read the pre-lock check separately and both could pass. */
    if (ledger->capacity >= cap_max) {
        spin_unlock_irqrestore(&ledger->lock, flags);
        kfree(chunk);
        return 0;
    }

    quota_ledger_chunk_t **tail = &ledger->chunks;
    while (*tail)
        tail = &(*tail)->next;

    /* RELEASE-publish the link so a concurrent resolver that observes the
     * pointer also observes the zeroed slots behind it. */
    __atomic_store_n(tail, chunk, __ATOMIC_RELEASE);
    __atomic_store_n(&ledger->capacity,
                     ledger->capacity + QUOTA_LEDGER_CHUNK_SLOTS,
                     __ATOMIC_RELEASE);

    spin_unlock_irqrestore(&ledger->lock, flags);
    return 1;
}

/* Claim a free slot, growing if every existing slot is taken. Returns the index,
 * or -1 when no slot could be obtained. */
static int32_t quota_ledger_claim_slot(quota_ledger_t *ledger,
                                      uint64_t *out_epoch)
{
    *out_epoch = 0;

    /* Scan, and only THEN decide whether to grow -- and always scan AGAIN after a
     * successful growth. The earlier shape grew on its final attempt and returned
     * failure without ever looking at the slots it had just added, reporting
     * STATUS_INSUFFICIENT_RESOURCES while the ledger had free space and had been
     * enlarged for nothing. */
    for (uint32_t attempt = 0; ; attempt++) {
        uint32_t hint = __atomic_load_n(&ledger->claim_hint, __ATOMIC_RELAXED);
        uint32_t cap  = __atomic_load_n(&ledger->capacity, __ATOMIC_ACQUIRE);

        if (hint > cap)
            hint = 0;

        /* Two passes: hint..end, then front..hint. A filling ledger claims the
         * very next slot on its first probe instead of rescanning everything it
         * has already handed out; a ledger with holes near the front still finds
         * them on the wrap. */
        for (uint32_t pass = 0; pass < 2; pass++) {
            uint32_t from = (pass == 0) ? hint : 0;
            uint32_t upto = (pass == 0) ? cap  : hint;

            if (from >= upto)
                continue;

            quota_ledger_iter_t  it;
            quota_ledger_slot_t *slot;
            uint32_t             i = 0;

            quota_ledger_iter_init(&it, ledger);
            while ((slot = quota_ledger_iter_next(&it, &i))
                       != (quota_ledger_slot_t *)0) {
                if (i < from)
                    continue;          /* still walking up to the start point */
                if (i >= upto)
                    break;
                if (atomic64_read(&slot->owner) != 0)
                    continue;

                /* The CAS is the claim. A plain "if free then take" would hand
                 * the same slot to two chargers on two CPUs. */
                uint64_t epoch =
                    __atomic_add_fetch(&g_slot_epoch_next, 1, __ATOMIC_RELAXED);
                /* Fail CLOSED at the ceiling rather than storing an epoch that
                 * collides with the DEFERRED mark or wraps to the free value.
                 * Refusing the claim surfaces as STATUS_INSUFFICIENT_RESOURCES
                 * for the charge, which is a bounded, attributable refusal --
                 * the same shape the chunk ceiling already uses. */
                if (epoch == 0 || epoch > QUOTA_SLOT_EPOCH_MAX)
                    return -1;
                if (atomic64_cmpxchg(&slot->owner, 0, (int64_t)epoch) == 0) {
                    __atomic_store_n(&ledger->claim_hint, i + 1,
                                     __ATOMIC_RELAXED);
                    *out_epoch = epoch;
                    return (int32_t)i;
                }
            }
        }

        if (attempt >= QUOTA_LEDGER_GROW_ATTEMPTS)
            return -1;
        if (!quota_ledger_grow(ledger))
            return -1;
    }
}

void quota_ledger_ref(quota_ledger_t *ledger)
{
    if (!ledger)
        return;
    atomic_inc(&ledger->refcount);
}

/* Reference a ledger that may already be committed to teardown. Returns 0 when
 * the count has reached zero, exactly like quota_block_try_ref: lifting a dead
 * count back to one would hand out a reference to storage the destroy path is
 * about to free.
 *
 * SCOPE OF THE PROTECTION, stated honestly because it is partial: this closes the
 * window where the count has hit zero but the memory is not yet freed. It cannot
 * make a FREED pointer safe -- nothing can, since the load itself would fault --
 * so it does not remove the caller's obligation to own a reference before
 * handing a ledger pointer to another agent (see quota_obligation_t in
 * quota_ledger.h). It is the same defence, and the same limit, as the block-level
 * try-ref this mirrors. */
static int quota_ledger_try_ref(quota_ledger_t *ledger)
{
    if (!ledger)
        return 0;
    for (;;) {
        int32_t cur = atomic_read(&ledger->refcount);
        if (cur <= 0)
            return 0;
        if (atomic_cmpxchg(&ledger->refcount, cur, cur + 1) == cur)
            return 1;
    }
}

/* ========================================================================== *
 * Deferred completion above PASSIVE_LEVEL
 *
 * The contract, and the precise statement of what is unsafe at raised IRQL,
 * live in quota_ledger.h. This is the mechanism: one global list of ledgers
 * with work pending, and one threaded DPC that completes it at PASSIVE_LEVEL.
 * ========================================================================== */

/* Which CPU's THREADED-DPC list this drain joins.
 *
 * It is queue ownership, NOT affinity, and the distinction matters because the
 * obvious rationale is wrong for this DPC class: dpc.h:57-65 states that a
 * threaded DPC has no CPU-affinity guarantee -- a single all-CPU worker drains
 * every CPU's threaded list and runs the callback on whatever CPU the scheduler
 * gives it. So the "a callback queued at an idle AP strands until that AP lowers
 * IRQL" hazard, which is real for a NORMAL DPC and is why quota_pressure.c pins
 * its publisher, does not apply here. A fixed list is still chosen over
 * DPC_TARGET_CURRENT so that every producer contends on ONE queue in a
 * predictable order rather than scattering entries across per-CPU lists.
 *
 * COROLLARY, and the reason this is spelled out: quota_ledger_deferred_drain
 * must never read smp_this_cpu() or any per-CPU state. The single-drainer
 * property comes from g_defer_draining, not from affinity. */
#define QUOTA_LEDGER_SERVICE_CPU     0u

/* Work bound for ONE drain callback. The worker is shared (pressure publication
 * runs on it too), and a ledger can hold QUOTA_LEDGER_MAX_CHUNKS chunks worth of
 * slots, so an unbounded callback could monopolise it for an arbitrary walk. The
 * callback stops at whichever bound it reaches first and re-arms itself if work
 * remains, which turns a burst into several bounded visits instead of one long
 * one. */
#define QUOTA_LEDGER_DRAIN_LEDGERS   16u
#define QUOTA_LEDGER_DRAIN_SLOTS     512u

/* What one deferred DESTROY costs against the callback budget. A destroy scans
 * every slot and frees up to QUOTA_LEDGER_MAX_CHUNKS chunks, so it is charged as
 * a full slot allowance rather than as one item. */
#define QUOTA_LEDGER_DESTROY_COST    QUOTA_LEDGER_DRAIN_SLOTS

/* Drain callbacks that may complete NOTHING on one ledger before its remaining
 * deferrals are given up to the orphan drain. Without a cross-callback bound the
 * requeue-and-rearm cycle is self-sustaining: a receipt held same-generation
 * BUSY makes every pass restore the stamp, keep the pending count up, and re-arm
 * immediately. */
#define QUOTA_LEDGER_DEFER_RETRY_MAX 8u


#ifdef KERNEL_TESTS
/* Bounded YIELD count while waiting for an in-flight drain pass to finish when a
 * test takes the hold. An iteration count rather than a duration, for the same
 * reason the gate drain wait is: it must behave the same on TCG, KVM, WHPX and
 * bare metal. Each iteration yields the CPU, so this is 1000 scheduling turns,
 * not 1000 spins -- a drain pass is bounded by its own budgets, so exhausting it
 * means something is wrong and the code says so rather than failing silently. */
#define QUOTA_LEDGER_HOLD_WAIT_ITERS 1000u
#endif

static spinlock_t      g_defer_lock = SPINLOCK_INIT;
static quota_ledger_t *g_defer_head;
static quota_ledger_t *g_defer_tail;

/* Exactly one CPU may hand g_defer_dpc to the DPC queue at a time; dpc.h
 * documents concurrent same-DPC inserts as caller misuse. The atomic exchange is
 * what makes that single ownership real -- a read-then-set would let two CPUs
 * both observe 0. Cleared only by the callback. */
static uint32_t        g_defer_armed;

/* Set by quota_ledger_init once a worker exists to defer TO. Until then a
 * raised-IRQL return completes in place: correctness is preserved, the bounded
 * -work property is not, and g_defer_forced counts exactly how often that
 * happened rather than leaving it to assumption. */
static uint32_t        g_defer_ready;

/* Single-ownership claim for quota_ledger_init itself; see the note there. */
static uint32_t        g_defer_initialising;

static KDPC            g_defer_dpc;

/* ONE drain pass runs at a time, system-wide. Two concurrent walkers over the
 * same ledger cannot be made safe cheaply: the pop takes a ledger OFF the list,
 * so a producer can immediately requeue it and a second walker can then be
 * inside the same slot array while the first is completing obligations in it.
 * A single claim removes that entire class, and costs nothing real -- the DPC
 * is pinned to one CPU, so the only contender is the synchronous drain. A
 * refused pass is not lost work: the running pass rechecks and re-arms. */
static uint32_t        g_defer_draining;

static uint32_t        g_defer_pending;
static uint64_t        g_defer_forced;
static uint64_t        g_defer_completed;
static uint64_t        g_defer_destroyed;

static int  quota_ledger_defer_nonempty(void);
static void quota_ledger_defer_arm(void);

#ifdef KERNEL_TESTS
/* Drain hold. In a booted test kernel the threaded-DPC worker is already
 * running, so it would race a test for the very obligations that test just
 * deferred -- and an assertion like "this drain completed N" would then be
 * counting against a worker that may have taken some of them first. While the
 * hold is set the worker neither arms nor drains, and quota_ledger_drain_now
 * (the tests' own entry point) owns the queue outright. Same mechanism, and the
 * same reason, as quota_pressure.c's publication hold. */
static uint32_t g_defer_test_hold;

void quota_ledger_test_hold(int hold)
{
    if (hold) {
        __atomic_store_n(&g_defer_test_hold, 1u, __ATOMIC_RELEASE);
        /* Setting the flag stops the worker from STARTING a pass; it does not
         * evict one already running, which would go on racing the very
         * assertions the hold exists to make deterministic (and would hold the
         * single-drainer claim, so the test's own quota_ledger_drain_now would
         * return zero having done nothing). Wait it out.
         *
         * YIELDING, not spinning. The agent being waited for is a scheduled
         * PASSIVE_LEVEL worker, so a tight loop here can consume the very CPU it
         * needs to clear the claim -- on a single-CPU boot that never resolves.
         * This runs at PASSIVE_LEVEL in a test, so yielding is legal and is the
         * only shape that can actually converge. */
        for (uint32_t spin = 0; spin < QUOTA_LEDGER_HOLD_WAIT_ITERS; spin++) {
            if (__atomic_load_n(&g_defer_draining, __ATOMIC_ACQUIRE) == 0u)
                return;
            yield();
        }
        klog(LOG_WARN, "quota",
             "ledger drain hold: worker still draining after %u yields; "
             "deferral assertions may race",
             (uint64_t)QUOTA_LEDGER_HOLD_WAIT_ITERS);
        return;
    }

    __atomic_store_n(&g_defer_test_hold, 0u, __ATOMIC_RELEASE);

    /* Releasing must RE-ARM. A callback dispatched before the hold was taken
     * disarms on entry and then returns without draining, so the queue can be
     * non-empty with nothing armed to come back for it -- deferred work would
     * then sit until some unrelated producer happened to arrive, which in a test
     * boot may be never. */
    if (quota_ledger_defer_nonempty())
        quota_ledger_defer_arm();
}
#endif

static void quota_ledger_defer_arm(void)
{
    if (!__atomic_load_n(&g_defer_ready, __ATOMIC_ACQUIRE))
        return;
#ifdef KERNEL_TESTS
    if (__atomic_load_n(&g_defer_test_hold, __ATOMIC_ACQUIRE))
        return;
#endif
    if (__atomic_exchange_n(&g_defer_armed, 1u, __ATOMIC_ACQ_REL) != 0u)
        return;
    if (!KeInsertQueueDpcOnCpu(&g_defer_dpc, QUOTA_LEDGER_SERVICE_CPU,
                               (void *)0, (void *)0, (int *)0)) {
        /* Already queued, or the queue refused it. Releasing ownership is what
         * keeps a refusal from being permanent: leaving the flag set would mean
         * no later producer could ever arm the drain again. */
        __atomic_store_n(&g_defer_armed, 0u, __ATOMIC_RELEASE);
    }
}

/* Put `ledger` on the pending list if it is not already there, and arm the
 * worker. FIFO, so a ledger that has waited longest is completed first.
 *
 * Callable at any IRQL: it allocates nothing and the only lock it takes is this
 * list's own, held across a couple of pointer stores. */
/* Put a ledger back at the FRONT of the queue.
 *
 * Only used for a destroy the current callback could not afford. Tail-requeueing
 * it starves it outright: an ordinary ledger ahead of it that keeps receiving
 * deferrals is re-queued behind it on every pass, so the rotation
 * [A,D] -> [D,A] -> [A,D] repeats forever and D's refcount-zero storage and its
 * orphaned charges are never reclaimed. Front-requeueing makes it the first item
 * the next callback pops, where the full budget is available. */
static void quota_ledger_defer_push_front(quota_ledger_t *ledger)
{
    uint64_t flags;

    spin_lock_irqsave(&g_defer_lock, &flags);
    ledger->defer_destroy = 1u;
    if (!ledger->defer_queued) {
        ledger->defer_queued = 1u;
        ledger->defer_next   = g_defer_head;
        g_defer_head         = ledger;
        if (!g_defer_tail)
            g_defer_tail = ledger;
    }
    spin_unlock_irqrestore(&g_defer_lock, flags);

    quota_ledger_defer_arm();
}

static void quota_ledger_defer_push(quota_ledger_t *ledger, int destroy)
{
    uint64_t flags;
    int      enqueued = 0;

    /* TAKE THE MEMBERSHIP REFERENCE BEFORE THE NODE IS VISIBLE. Taking it after
     * unlocking is a use-after-free: a drain already running can pop and finish
     * the ledger in that gap, drop the transferred reference AND the membership
     * reference it believes it holds, reach zero, and free the storage this
     * function is about to touch. The caller's own reference keeps the ledger
     * alive up to here, so the ref is always legal to take; if it turns out the
     * ledger was already queued, the speculative reference is dropped after the
     * lock is released.
     *
     * A DESTROY push takes none: its refcount is already zero, which is exactly
     * what makes it unreachable by anyone else and its queue node safe to
     * write. */
    if (!destroy)
        quota_ledger_ref(ledger);

    spin_lock_irqsave(&g_defer_lock, &flags);
    if (destroy)
        ledger->defer_destroy = 1u;
    if (!ledger->defer_queued) {
        ledger->defer_queued = 1u;
        ledger->defer_next   = (quota_ledger_t *)0;
        if (g_defer_tail)
            g_defer_tail->defer_next = ledger;
        else
            g_defer_head = ledger;
        g_defer_tail = ledger;
        enqueued = 1;
    }
    spin_unlock_irqrestore(&g_defer_lock, flags);

    /* Already queued: the membership reference is held by whoever enqueued it,
     * so give back the speculative one. Safe after the unlock because the
     * caller still holds its own. */
    if (!enqueued && !destroy)
        quota_ledger_deref(ledger);

    quota_ledger_defer_arm();
}

static quota_ledger_t *quota_ledger_defer_pop(void)
{
    quota_ledger_t *ledger;
    uint64_t        flags;

    spin_lock_irqsave(&g_defer_lock, &flags);
    ledger = g_defer_head;
    if (ledger) {
        g_defer_head = ledger->defer_next;
        if (!g_defer_head)
            g_defer_tail = (quota_ledger_t *)0;
        ledger->defer_next   = (quota_ledger_t *)0;
        ledger->defer_queued = 0u;
    }
    spin_unlock_irqrestore(&g_defer_lock, flags);
    return ledger;
}

static int quota_ledger_defer_nonempty(void)
{
    int      any;
    uint64_t flags;

    spin_lock_irqsave(&g_defer_lock, &flags);
    any = (g_defer_head != (quota_ledger_t *)0);
    spin_unlock_irqrestore(&g_defer_lock, flags);
    return any;
}

/* Return every still-live obligation in a ledger nobody can reach any more, and
 * report how many there were. An ACTIVE slot at this point is unreturnable by
 * anyone else -- no holder exists to present its token -- so leaving it charged
 * would strand that usage on the blocks for the rest of the boot. */
static uint32_t quota_ledger_drain_orphans(quota_ledger_t *ledger,
                                           uint32_t *out_skipped)
{
    if (out_skipped)
        *out_skipped = 0;
    uint32_t             reclaimed = 0;
    quota_ledger_iter_t  it;
    quota_ledger_slot_t *slot;

    quota_ledger_iter_init(&it, ledger);
    while ((slot = quota_ledger_iter_next(&it, (uint32_t *)0)) != (quota_ledger_slot_t *)0) {
        if (atomic64_read(&slot->owner) == 0)
            continue;

        int64_t tag = atomic64_read(&slot->receipt.tag);
        if (QUOTA_RECEIPT_TAG_STATE(tag) == QUOTA_RECEIPT_BUSY) {
            /* An operation OWNS this slot right now (a charge being built, an
             * adjust, or a migration). Leave it entirely alone: releasing the slot
             * or returning the charge here would race that operation's writes to
             * the very receipt it is mutating. Its own completion, or a later
             * drain, deals with it.
             *
             * REPORTED FROM THIS OBSERVATION, not from a second scan afterwards: a
             * later re-read could find the owner had republished ACTIVE and would
             * then neither reclaim nor count the slot, which is precisely the
             * silent loss the count exists to prevent. */
            if (out_skipped)
                (*out_skipped)++;
            continue;
        }
        /* Captured before either release below, for the same reason the inline
         * completion captures it: the ceiling release is keyed by resource type,
         * and a credited receipt is no longer a reliable place to read it.
         *
         * THE CEILING RELEASES HERE ARE FOR SYMMETRY, and saying so is more
         * honest than implying they are load-bearing. Both callers of this walk
         * are tearing the ledger down -- quota_ledger_destroy runs at refcount
         * zero, and quota_ledger_task_release drains only when the task's own
         * claim is the last reference and then drops it -- so the counters this
         * releases are freed within a few instructions and no admission
         * decision can ever observe them. They are kept because the pairing is
         * what makes the accounting structural: EVERY owner transition from an
         * epoch back to zero releases, so a future path that drains orphans
         * WITHOUT destroying is correct by construction rather than by having
         * remembered this exception. For the same reason no test asserts the
         * decrement here -- a seam existing only to observe state that is about
         * to be freed would test the seam, not the property. */
        const quota_resource_type_t type =
            (quota_resource_type_t)atomic_read(&slot->rtype);

        if (QUOTA_RECEIPT_TAG_STATE(tag) != QUOTA_RECEIPT_ACTIVE) {
            /* Allocated but holding nothing: a charge that failed after the claim.
             * Just release the slot. */
            atomic64_set(&slot->owner, 0);
            quota_ledger_release_obligation(ledger, type);
            continue;
        }

        /* We are the only agent that can still name this charge, so its own
         * generation is the token. */
        quota_return_chain(&slot->receipt, QUOTA_RECEIPT_TAG_GEN(tag));
        atomic64_set(&slot->owner, 0);
        quota_ledger_release_obligation(ledger, type);
        reclaimed++;
    }
    return reclaimed;
}

static void quota_ledger_destroy(quota_ledger_t *ledger)
{
    /* Refcount is zero: no other agent can reach this ledger, so a live
     * obligation here is one whose holder went away without returning it. That
     * is a genuine leak by that holder -- reclaim it and count it. */
    /* The drain deliberately SKIPS a BUSY slot, because releasing one an operation
     * is still writing would race it. That is right for the task-release caller,
     * whose ledger survives -- but here the storage is about to be freed, so a
     * skipped slot is a charge nobody will ever return, and a silent skip would be
     * one the leak counter never shows. Both outcomes are therefore counted from
     * the drain's own observation. (Reaching zero references while an operation
     * still owns a slot should be impossible now that every walker pins the
     * ledger; this counts it rather than trusting that.) */
    uint32_t skipped = 0;
    uint32_t orphans = quota_ledger_drain_orphans(ledger, &skipped);
    if (orphans || skipped)
        __atomic_fetch_add(&g_ledger_leaks, (uint64_t)orphans + (uint64_t)skipped,
                           __ATOMIC_RELAXED);

    quota_ledger_chunk_t *chunk = ledger->chunks;
    while (chunk) {
        quota_ledger_chunk_t *next = chunk->next;
        kfree(chunk);
        chunk = next;
    }
    kfree(ledger);
}

/* Drop a reference from the DRAIN that may turn out to be the last, and make the
 * destroy pay for itself out of the callback budget.
 *
 * The decision has to ride the decrement itself. Sampling the refcount first and
 * dropping afterwards is a race: between the two, another holder -- the task
 * claim, or a concurrent obligation return -- can let go, so a drop the drain
 * believed was not last becomes last, and the ordinary deref then destroys
 * INLINE at PASSIVE_LEVEL with no budget charged. That is precisely the
 * unbudgeted teardown this accounting exists to prevent.
 *
 * Returns 1 when it DEFERRED a destroy for want of budget, so the caller can end
 * the pass; 0 otherwise. */
static int quota_ledger_deref_budgeted(quota_ledger_t *ledger, uint32_t *budget)
{
    if (!ledger)
        return 0;
    if (!atomic_dec_and_test(&ledger->refcount))
        return 0;

    if (*budget >= QUOTA_LEDGER_DESTROY_COST) {
        *budget -= QUOTA_LEDGER_DESTROY_COST;
        quota_ledger_destroy(ledger);
        return 0;
    }

    /* No allowance left. Hand the teardown to the next callback, at the FRONT so
     * it leads with a full budget rather than queueing behind ledgers that keep
     * being requeued. Nothing can observe the ledger between the decrement and
     * the push: the count is already zero, which is what makes it unreachable. */
    quota_ledger_defer_push_front(ledger);
    return 1;
}

void quota_ledger_deref(quota_ledger_t *ledger)
{
    if (!ledger)
        return;
    if (!atomic_dec_and_test(&ledger->refcount))
        return;

    /* The refcount reached zero, so nothing else can reach this ledger and the
     * intrusive list node inside it is now exclusively ours to write. That
     * ordering is the whole safety argument for reusing the dying object's own
     * storage as the queue node, and atomic_dec_and_test is what establishes it:
     * the push below is strictly after the acquire-release transition to zero,
     * never concurrent with another walker. */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        if (__atomic_load_n(&g_defer_ready, __ATOMIC_ACQUIRE)) {
            quota_ledger_defer_push(ledger, 1);
            return;
        }
        /* No worker yet, so there is nowhere to defer to and the destroy runs
         * here. Counted rather than silent: this is the one window where the
         * bounded-work property does not hold. */
        __atomic_fetch_add(&g_defer_forced, 1, __ATOMIC_RELAXED);
    }

    quota_ledger_destroy(ledger);
}

quota_ledger_t *quota_ledger_acquire(struct task *task)
{
    if (!task)
        return (quota_ledger_t *)0;

    uint64_t         flags;
    quota_ledger_t  *existing;

    /* Fast path: already there. Same lock that guards the process/user block
     * pair, because the ledger pointer has the same lifetime discipline -- with
     * one deliberate difference documented in task.h: it is cleared at REAP,
     * not at death, because obligations must survive the death teardown. */
    spin_lock_irqsave(&task->quota_lock, &flags);
    existing = task->quota_ledger;
    if (existing)
        quota_ledger_ref(existing);
    spin_unlock_irqrestore(&task->quota_lock, flags);
    if (existing)
        return existing;

    /* A ledger created for a task that has already been sealed would never be
     * reaped, so it could never be released. Refuse instead of leaking. Checked
     * here to avoid the allocation, and AGAIN under the lock below, because the
     * seal can land in between. */
    if (quota_gate_state_of(task) == QUOTA_GATE_SEALED)
        return (quota_ledger_t *)0;

    quota_ledger_t *fresh =
        (quota_ledger_t *)kmalloc_zeroed(sizeof(quota_ledger_t));
    if (!fresh)
        return (quota_ledger_t *)0;

    fresh->id = __atomic_add_fetch(&g_ledger_id_next, 1, __ATOMIC_RELAXED);
    fresh->owner = task;
    fresh->lock = (spinlock_t)SPINLOCK_INIT;
    fresh->capacity = QUOTA_LEDGER_INLINE_SLOTS;
    /* Two references on the winner: the TASK's own claim, plus the one this
     * call returns to its caller. */
    atomic_set(&fresh->refcount, 2);

    spin_lock_irqsave(&task->quota_lock, &flags);
    existing = task->quota_ledger;
    if (!existing && quota_gate_state_of(task) != QUOTA_GATE_SEALED) {
        task->quota_ledger = fresh;
        existing = fresh;
        fresh = (quota_ledger_t *)0;      /* published; not ours to free */
    } else if (existing) {
        quota_ledger_ref(existing);
    }
    spin_unlock_irqrestore(&task->quota_lock, flags);

    if (fresh) {
        /* Either another CPU published first, or death sealed the gate while we
         * allocated. Nothing ever saw this one, so free it directly rather than
         * through deref (which would run the orphan drain over empty slots). */
        kfree(fresh);
    }
    return existing;
}

/* Both queries below go through quota_ledger_peek, which resolves the task's
 * ledger under quota_lock AND takes a reference before releasing it -- never a
 * bare read of task->quota_ledger.
 *
 * The bare read is a use-after-free, not a style preference: a reap on another
 * CPU can clear the pointer and drop the task's last reference the instant the
 * lock is released, so a query that returned the raw pointer would then read
 * counters out of freed storage. It does not even need a live obligation to
 * happen -- a ledger holding nothing but the task's own claim is enough. The
 * migrate and unmigrate walkers already pin the same way, for the same reason.
 *
 * A task with no ledger holds no obligations, so NULL is the honest answer
 * rather than a reason to create one: these are diagnostics, and a diagnostic
 * that allocates is a diagnostic that changes what it measures. */

uint32_t quota_ledger_task_obligations(struct task *task)
{
    /* NULL is answered, not dereferenced: quota_ledger_peek takes
     * task->quota_lock unconditionally, so passing NULL through would fault
     * inside a DIAGNOSTIC -- the one class of call that must never be able to
     * take the machine down. Every neighbouring public quota query treats a
     * NULL principal as "holds nothing". */
    if (!task)
        return 0;

    quota_ledger_t *ledger = quota_ledger_peek(task);
    if (!ledger)
        return 0;

    /* Summed over the per-type counters rather than kept as a third counter the
     * charge path would also have to move: the total is a diagnostic, and one
     * more contended cache line on the hot path to save sixteen relaxed reads
     * off it is the wrong trade. The sum is not a snapshot -- concurrent charges
     * of different types land at different moments -- which is all a diagnostic
     * needs, and matches what quota_source_usage already promises. */
    int32_t total = 0;
    for (uint32_t t = 0; t < (uint32_t)QUOTA_RESOURCE_TYPE_COUNT; t++)
        total += atomic_read(&ledger->obligations[t]);

    quota_ledger_deref(ledger);
    return (total > 0) ? (uint32_t)total : 0u;
}

uint32_t quota_ledger_task_obligations_of(struct task *task,
                                          quota_resource_type_t type)
{
    if (!task || (uint32_t)type >= (uint32_t)QUOTA_RESOURCE_TYPE_COUNT)
        return 0;

    quota_ledger_t *ledger = quota_ledger_peek(task);
    if (!ledger)
        return 0;

    int32_t held = atomic_read(&ledger->obligations[(uint32_t)type]);

    quota_ledger_deref(ledger);
    return (held > 0) ? (uint32_t)held : 0u;
}

uint32_t quota_ledger_outstanding(quota_ledger_t *ledger)
{
    if (!ledger)
        return 0;

    /* Counted by walking rather than tracked in a counter, deliberately: a
     * counter would need to be adjusted on every charge, return, orphan drain,
     * and failed-charge release, and a single missed adjustment silently
     * corrupts both the leak assertion and the tests that read it. The walk is
     * bounded by the ledger's own capacity and is only used for diagnostics,
     * tests, and the reap decision. */
    uint32_t             live = 0;
    quota_ledger_iter_t  it;
    quota_ledger_slot_t *slot;

    quota_ledger_iter_init(&it, ledger);
    while ((slot = quota_ledger_iter_next(&it, (uint32_t *)0)) != (quota_ledger_slot_t *)0) {
        if (atomic64_read(&slot->owner) == 0)
            continue;
        if (QUOTA_RECEIPT_TAG_STATE(atomic64_read(&slot->receipt.tag))
                == QUOTA_RECEIPT_ACTIVE)
            live++;
    }
    return live;
}

NTSTATUS quota_ledger_charge(struct task *task, quota_resource_type_t type,
                             uint64_t amount, quota_obligation_t *out)
{
    return quota_ledger_charge_from(task, type, amount, QUOTA_SOURCE_UNKNOWN,
                                    out);
}

NTSTATUS quota_ledger_charge_from(struct task *task, quota_resource_type_t type,
                                  uint64_t amount, quota_charge_source_t source,
                                  quota_obligation_t *out)
{
    if (!out)
        return STATUS_INVALID_PARAMETER;

    out->ledger = (quota_ledger_t *)0;
    out->slot   = 0;
    out->token  = 0;
    out->epoch  = 0;      /* zeroed WITH the others: the epoch is the field the
                           * stale-duplicate defence rests on, so it must never
                           * be left holding a caller's garbage on a failure or
                           * zero-amount path. */

    if (!task || !quota_source_valid(source))
        return STATUS_INVALID_PARAMETER;

    /* RANGE BEFORE STORAGE, and here it is also range before STATE: `type`
     * indexes the per-type obligation counters, so an out-of-range value would
     * read and write past the end of that array before quota_charge_chain ever
     * saw it. The chain validates the type too; this validation exists because
     * the ceiling now touches the type FIRST. */
    if ((uint32_t)type >= (uint32_t)QUOTA_RESOURCE_TYPE_COUNT)
        return STATUS_INVALID_PARAMETER;

    /* AMOUNT SEMANTICS BEFORE THE CEILING, because the ceiling is not entitled
     * to an opinion about a charge that takes no obligation. Both of these were
     * previously handled only in the current-task wrapper, which meant the two
     * public entry points disagreed the moment a type sat at its cap: the same
     * malformed amount reported STATUS_INVALID_PARAMETER through one and
     * STATUS_QUOTA_EXCEEDED through the other, and a zero charge -- which
     * quota.h defines as a SUCCESS owing nothing -- was refused outright. Which
     * wrapper a caller happened to use is not allowed to change what a charge
     * means. */
    if (amount > (uint64_t)QUOTA_AMOUNT_MAX)
        return STATUS_INVALID_PARAMETER;

    /* ZERO OWES NOTHING, but it is NOT simply STATUS_SUCCESS: quota_charge_chain
     * still checks liveness on a zero charge, so a sealed task must still be
     * told it is terminating. Delegating reproduces that contract exactly while
     * touching no ledger storage and reserving no budget. */
    if (amount == 0) {
        quota_charge_receipt_t scratch = { 0 };
        uint64_t               scratch_token = 0;
        return quota_charge_chain(task, type, 0, QUOTA_CHARGE_SOURCE(source),
                                  &scratch, &scratch_token);
    }

    quota_ledger_t *ledger = quota_ledger_acquire(task);
    if (!ledger) {
        /* Distinguish "dying" from "out of memory": a caller can retry the
         * second and must not retry the first. */
        return (quota_gate_state_of(task) == QUOTA_GATE_SEALED)
                   ? STATUS_PROCESS_IS_TERMINATING
                   : STATUS_INSUFFICIENT_RESOURCES;
    }

    /* RESERVE BEFORE CLAIMING A SLOT. A task at its ceiling must not reach
     * quota_ledger_grow at all: the claim path allocates a chunk when the
     * existing slots are full, so checking afterwards would let a refused charge
     * still grow the heap footprint the ceiling exists to bound.
     *
     * A REFUSAL IS DISAMBIGUATED AGAINST THE GATE, because the authoritative
     * liveness answer lives inside quota_charge_chain and this check now runs
     * before it. Without this, a task that was sealed or is mid-quiesce would be
     * told STATUS_QUOTA_EXCEEDED -- a permanent, actionable-looking refusal --
     * for what is really "you are dying" or "retry in a moment". The mapping is
     * quota_charge_chain's own (SEALED -> terminating, CLOSED -> retry), so the
     * status a caller sees does not depend on which check happened to run
     * first. */
    NTSTATUS reserved = quota_ledger_reserve_obligation(ledger, type);
    if (reserved != STATUS_SUCCESS) {
        /* ASK THE GATE, do not imitate it. Reading quota_gate_state_of and
         * mapping the answer by hand reproduced the classification ALMOST
         * exactly, and "almost" is the bug: the gate also refuses an OPEN gate
         * whose in-flight count is at its ceiling, calling that CLOSED and
         * reporting STATUS_RETRY, and it keeps the refusal telemetry every other
         * refusal lands in. A hand-rolled copy silently answered
         * STATUS_QUOTA_EXCEEDED for that case and recorded nothing.
         *
         * Entering and immediately leaving costs one CAS pair on the REFUSAL
         * path only, and it does not move the gate off its funnel: the charge
         * itself still enters inside quota_charge_chain. This asks the
         * authority a question; it does not take over the authority's job. */
        uint32_t gate_state = QUOTA_GATE_OPEN;

        if (!quota_gate_enter(task, &gate_state)) {
            quota_ledger_deref(ledger);
            return (gate_state == QUOTA_GATE_SEALED)
                       ? STATUS_PROCESS_IS_TERMINATING
                       : STATUS_RETRY;
        }
        quota_gate_exit(task);

        /* COUNTED HERE, not where the budget was found full, because only now is
         * STATUS_QUOTA_EXCEEDED what the caller actually receives. The gate
         * outranks the ceiling, so a charge arriving at a full cap on a CLOSED
         * or SEALED task is told RETRY or TERMINATING and is a gate refusal --
         * counting it above would let a teardown storm or a burst of membership
         * transitions read as a quota-policy problem on the dashboards, which is
         * precisely the misattribution this counter exists to prevent.
         *
         * And ONLY for the policy status. The reservation can also give up with
         * STATUS_RETRY after exhausting its CAS bound, which is contention, not
         * a budget decision -- counting that would make a contended boundary
         * look like an administrative quota problem, the same misreading in a
         * different disguise. */
        if (reserved == STATUS_QUOTA_EXCEEDED) {
            quota_ledger_count_refusal(&g_ceiling_refusals);
            quota_ledger_count_refusal(&g_ceiling_refusals_by_type[(uint32_t)type]);
        }

        quota_ledger_deref(ledger);
        return reserved;
    }

    uint64_t epoch = 0;
    int32_t  index = quota_ledger_claim_slot(ledger, &epoch);
    if (index < 0) {
        quota_ledger_release_obligation(ledger, type);
        quota_ledger_deref(ledger);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    quota_ledger_slot_t *slot = quota_ledger_slot_at(ledger, (uint32_t)index);
    if (!slot) {
        /* Cannot happen: the index came from a resolved slot. Handled anyway so
         * the claim is never stranded if it ever does. */
        quota_ledger_release_obligation(ledger, type);
        quota_ledger_deref(ledger);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Publish the reserved type while this slot is exclusively ours -- the claim
     * above is what makes that true -- so every completion path can read it
     * without touching the receipt. */
    atomic_set(&slot->rtype, (int32_t)type);

    uint64_t token = 0;
    /* The gate is entered INSIDE quota_charge_chain, not here: that is the one
     * funnel every chain charge passes through, so gating there covers the
     * charges whose receipts live in their caller's own storage too. */
    NTSTATUS status = quota_charge_chain(task, type, amount,
                                         QUOTA_CHARGE_SOURCE(source),
                                         &slot->receipt, &token);
    if (status != STATUS_SUCCESS) {
        atomic64_set(&slot->owner, 0);
        quota_ledger_release_obligation(ledger, type);
        quota_ledger_deref(ledger);
        return status;
    }

    if (token == 0) {
        /* A zero-amount charge succeeds while owing nothing (quota.h calls 0
         * the canonical no-obligation token). There is no obligation to record,
         * so release the slot and hand back an EMPTY handle -- returning it
         * later must be a no-op, not a return of whatever reuses this slot. */
        atomic64_set(&slot->owner, 0);
        quota_ledger_release_obligation(ledger, type);
        quota_ledger_deref(ledger);
        return STATUS_SUCCESS;
    }

    /* Transfer this call's ledger reference into the obligation: the handle is
     * what keeps the slot storage alive from here on, including past the death
     * of the task that charged. */
    out->ledger = ledger;
    out->slot   = (uint32_t)index;
    out->token  = token;
    out->epoch  = epoch;
    return STATUS_SUCCESS;
}

/* Complete one obligation against `slot`: credit the charge back, release the
 * block references, and release the slot against the epoch that claimed it.
 *
 * Shared verbatim by the inline return and the drain worker, so a deferred
 * completion is the SAME completion -- not a second implementation that could
 * drift from it.
 *
 * TRI-STATE, because the drain needs to tell two very different failures apart.
 * QUOTA_COMPLETE_CREDITED: credited. QUOTA_COMPLETE_STALE: this handle owed
 * nothing (its charge was returned already and the slot moved on).
 * QUOTA_COMPLETE_RETRY: the charge is STILL LIVE but an adjust on another CPU
 * outlasted the bounded retries. Collapsing the last two into one "not credited"
 * answer is what let the drain drop a live obligation on the floor: it cleared
 * the pending stamp, dropped the transferred reference and reported success,
 * leaving the charge outstanding with the slot occupied until reap.
 *
 * The slot release is deliberately inside this helper: doing it in the caller
 * would mean the drain had to duplicate the exact-epoch CAS that makes a stale
 * duplicate harmless. */
#define QUOTA_COMPLETE_STALE     0
#define QUOTA_COMPLETE_CREDITED  1
#define QUOTA_COMPLETE_RETRY    (-1)

static int quota_ledger_complete_return(quota_ledger_t *ledger,
                                        quota_ledger_slot_t *slot,
                                        uint64_t token, uint64_t epoch)
{
    /* Read the reserved type from the slot's own atomic word, not from the
     * receipt: this read necessarily happens before the epoch is validated, so a
     * stale duplicate performs it while a new owner may be writing the receipt. */
    const quota_resource_type_t type =
        (quota_resource_type_t)atomic_read(&slot->rtype);

    /* Retry the WHOLE return, not merely the tag CAS inside it. A return can fail
     * to land for exactly one reason -- colliding with an in-progress adjust on
     * this same charge -- and that collision clears the moment the adjust
     * republishes, so re-reading and trying again keeps the failure off the caller
     * in every case where the adjust is making progress. And it always is: an
     * adjust masks interrupts across its whole claim-to-republish window, so it
     * can only be observed BUSY from ANOTHER CPU, where it is running. */
    int completed  = 0;
    int still_live = 0;

    for (uint32_t attempt = 0; attempt < QUOTA_LEDGER_RETURN_TRIES; attempt++) {
        quota_return_chain(&slot->receipt, token);

        int64_t  tag   = atomic64_read(&slot->receipt.tag);
        uint64_t gen   = QUOTA_RECEIPT_TAG_GEN(tag);
        uint32_t state = QUOTA_RECEIPT_TAG_STATE(tag);

        /* ONLY IDLE at our own generation proves the return completed. */
        if (gen == token && state == QUOTA_RECEIPT_IDLE) {
            completed = 1;
            break;
        }
        /* Our generation in ANY other state means the charge is still LIVE: BUSY
         * while an adjust owns it, or ACTIVE again because the adjust just
         * republished it. Both are retryable, and treating ACTIVE as anything
         * else would be the worst possible mistake here -- it would classify a
         * live charge as stale and destroy the only handle able to return it. */
        if (gen == token) {
            still_live = 1;
            continue;
        }
        /* A MOVED generation is the only proof of staleness: this charge is long
         * gone and no further attempt could ever match it. */
        still_live = 0;
        break;
    }

    if (!completed) {
        if (still_live) {
            /* The charge is live but an adjust outlasted every attempt. ABANDON
             * it WITHOUT releasing the slot. That is what makes the reap-time
             * sweep a real backstop rather than a promise -- retaining the
             * obligation instead would hold the refcount above the task's own
             * claim forever, so the drain would never run and the charge, its
             * block references, and its slot would outlive the boot. Letting go
             * means the ledger's own drain (at the task release, or at
             * destruction) reclaims the charge and counts it.
             *
             * NOT COUNTED HERE. The abandon counter means "an obligation was
             * given up on", and this function no longer decides that -- the
             * drain retries a RETRY across callbacks and usually wins. Counting
             * here would bill every intermediate retry as an abandonment and
             * destroy the diagnostic. The two TERMINAL sites bump it instead:
             * quota_ledger_return when it empties the handle, and the drain when
             * its retry budget is exhausted. */
            return QUOTA_COMPLETE_RETRY;
        }
        /* Otherwise this handle is STALE -- its charge was returned already and
         * the slot has moved on. It owes nothing. Either way the slot is
         * deliberately NOT released: this handle does not own it. */
        return QUOTA_COMPLETE_STALE;
    }

    /* Release the slot against the EXACT epoch that claimed it. A stale duplicate
     * of this handle reaches this line with a superseded epoch, matches nothing,
     * and therefore cannot free a slot another charger has since claimed and is
     * about to fill -- which a bare "clear the flag" would do.
     *
     * THE CEILING RELEASE IS CONDITIONAL ON WINNING THAT CAS, and the CAS result
     * is the only proof available. "IDLE at my own generation" is true both for
     * the returner that just completed AND for a properly referenced DUPLICATE of
     * the same handle arriving afterwards -- the comment above says exactly that
     * -- so both reach this line reporting CREDITED. Releasing on the report
     * would decrement twice for one obligation, drive the counter below the
     * obligations that exist, and admit charges past the ceiling. Only the caller
     * whose epoch matched actually freed the slot, so only it gives the budget
     * back. */
    if (atomic64_cmpxchg(&slot->owner, (int64_t)epoch, 0) == (int64_t)epoch)
        quota_ledger_release_obligation(ledger, type);
    return QUOTA_COMPLETE_CREDITED;
}

int quota_ledger_return(quota_obligation_t *ob)
{
    if (!ob || !ob->ledger || ob->token == 0)
        return QUOTA_LEDGER_RETURN_NONE;

    quota_ledger_t      *ledger = ob->ledger;
    quota_ledger_slot_t *slot   = quota_ledger_slot_at(ledger, ob->slot);

    if (!slot) {
        /* The handle names a slot this ledger does not have. Nothing to return and
         * nothing to release; drop the reference and empty it. */
        ob->ledger = (quota_ledger_t *)0;
        ob->slot   = 0;
        ob->token  = 0;
        ob->epoch  = 0;
        quota_ledger_deref(ledger);
        return QUOTA_LEDGER_RETURN_NONE;
    }

    /* ABOVE PASSIVE_LEVEL the completion is handed to the drain worker instead of
     * performed here, because completing it means releasing every block reference
     * the receipt holds -- and a last block reference unlinks and frees that block
     * -- which is unbounded work to do inside an interrupt. See the deferral
     * contract in quota_ledger.h.
     *
     * The slot is the record: stamping the token onto the slot this obligation
     * already owns is the whole handoff, so nothing is allocated. The CAS from 0
     * is what makes a double return harmless -- the second one finds a token
     * already stamped, owes nothing more, and must NOT hand the reference over a
     * second time. */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        if (__atomic_load_n(&g_defer_ready, __ATOMIC_ACQUIRE)) {
            /* ONE transition claims the deferral and proves the handle is still
             * the slot's owner. It fails for a stale handle (the slot moved on)
             * and for a slot already marked DEFERRED (someone got there first);
             * both mean this handle owes nothing further, and neither may fall
             * through to the inline completion -- doing the block teardown at
             * raised IRQL is the entire thing this path exists to prevent. */
            if (atomic64_cmpxchg(&slot->owner, (int64_t)ob->epoch,
                                 (int64_t)ob->epoch | QUOTA_SLOT_DEFERRED)
                == (int64_t)ob->epoch) {
                /* PIN THE LEDGER ACROSS THE HANDOFF. The moment the token
                 * becomes visible, a drain that has ALREADY popped this ledger
                 * can settle the obligation and drop the very reference this
                 * handle was about to transfer -- together with its own
                 * membership reference. For a ledger whose task claim is gone
                 * (the outliving-task case this whole mechanism exists for)
                 * those can be the last two, so the producer would then run
                 * quota_ledger_defer_push on freed storage. A temporary pin held
                 * across the publication makes that impossible.
                 *
                 * THE COUNTERS GO UP BEFORE THE TOKEN IS VISIBLE, for the same
                 * reason and one more: a drain that settles the obligation
                 * decrements them, and doing that before the producer's
                 * increment underflows an unsigned count. */
                quota_ledger_ref(ledger);
                __atomic_fetch_add(&g_defer_pending, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&ledger->defer_pending, 1, __ATOMIC_RELAXED);

                /* The slot is now ours and unreclaimable, so the token store
                 * needs no CAS.
                 *
                 * THE ORDERING INVARIANT IS THE REQUEUE, not the queue lock, and
                 * naming the wrong one would mislead the next refactor. When the
                 * ledger is not yet queued, the push below publishes it and its
                 * spin_unlock/pop acquire pair orders this store ahead of any
                 * drain. When a drain has ALREADY popped the ledger and is
                 * mid-walk -- possibly past this slot -- no lock separates us
                 * from it at all. What saves that case is that the pop cleared
                 * defer_queued, so the push below RE-ENQUEUES the ledger with a
                 * fresh membership reference and the next pass sees this slot.
                 * The relaxed defer_pending bump is a requeue hint, never the
                 * guarantee. */
                atomic64_set(&slot->deferred_token, (int64_t)ob->token);

                /* The handle's ledger reference TRANSFERS to the pending slot:
                 * the drain drops it once it has completed the obligation.
                 * Emptying the handle without a deref is not a leak, it is the
                 * handoff. */
                ob->ledger = (quota_ledger_t *)0;
                ob->slot   = 0;
                ob->token  = 0;
                ob->epoch  = 0;
                quota_ledger_defer_push(ledger, 0);
                /* Handoff complete; release the producer pin. If this is now the
                 * last reference the deref defers the destroy, exactly as any
                 * other raised-IRQL final drop does. */
                quota_ledger_deref(ledger);
                return QUOTA_LEDGER_RETURN_DEFERRED;
            }

            ob->ledger = (quota_ledger_t *)0;
            ob->slot   = 0;
            ob->token  = 0;
            ob->epoch  = 0;
            quota_ledger_deref(ledger);
            return QUOTA_LEDGER_RETURN_NONE;
        }
        /* No worker: there is nowhere to defer to, so the return completes here.
         * The bounded-work property is what is lost, never correctness, and the
         * count is what keeps that visible rather than assumed. */
        __atomic_fetch_add(&g_defer_forced, 1, __ATOMIC_RELAXED);
    }

    int outcome = quota_ledger_complete_return(ledger, slot, ob->token, ob->epoch);

    /* TERMINAL: this handle is emptied and its reference dropped below whatever
     * the outcome, so a RETRY here really is an abandonment. */
    if (outcome == QUOTA_COMPLETE_RETRY)
        __atomic_fetch_add(&g_ledger_abandons, 1, __ATOMIC_RELAXED);

    /* Empty the handle BEFORE dropping the reference: after the deref the ledger
     * may be gone, and a handle still naming it is a dangling pointer waiting for
     * a double return. */
    ob->ledger = (quota_ledger_t *)0;
    ob->slot   = 0;
    ob->token  = 0;
    ob->epoch  = 0;
    quota_ledger_deref(ledger);
    return (outcome == QUOTA_COMPLETE_CREDITED) ? QUOTA_LEDGER_RETURN_CREDITED
                                                : QUOTA_LEDGER_RETURN_NONE;
}

/* Complete every deferred return stamped on `ledger`, and report how many.
 *
 * Resumes at the ledger's saved cursor and stops once `budget` slots have been
 * EXAMINED, so one enormous ledger cannot hold the shared worker for an
 * unbounded walk; the caller requeues it and the saved cursor is what makes the
 * next visit make progress instead of rescanning the same prefix.
 *
 * The caller must hold a reference for the duration (the queue membership one).
 * That is what lets this function drop the obligations' transferred references
 * SAFELY -- they are dropped after the iteration is finished, never inside it,
 * because the last of them can destroy the ledger. */
/* quota_ledger_iter_init positioned at `start`, advancing by whole CHUNKS
 * instead of returning every skipped slot.
 *
 * The drain resumes from a saved cursor, and initialising at zero and discarding
 * the prefix made every visit re-walk it: at the per-task slot ceiling with a
 * 512-slot budget, covering the array cost several times more iterator returns
 * than there are slots, and a lone deferral at the last slot paid that on every
 * visit. Chunk arithmetic turns the skip into at most QUOTA_LEDGER_MAX_CHUNKS
 * pointer hops, which is why raising the ceiling does not reintroduce the cost. */
static void quota_ledger_iter_init_at(quota_ledger_iter_t *it,
                                      quota_ledger_t *ledger, uint32_t start)
{
    quota_ledger_iter_init(it, ledger);
    if (start == 0)
        return;

    if (start < QUOTA_LEDGER_INLINE_SLOTS) {
        it->within = start;
        it->index  = start;
        return;
    }

    /* Past the inline run: step onto the chunk that holds `start`. The ACQUIRE
     * matches quota_ledger_grow's release-publish, exactly as iter_next does. */
    it->inline_done = 1;
    it->chunk  = __atomic_load_n(&ledger->chunks, __ATOMIC_ACQUIRE);
    it->index  = QUOTA_LEDGER_INLINE_SLOTS;
    it->within = 0;

    while (it->chunk && (start - it->index) >= QUOTA_LEDGER_CHUNK_SLOTS) {
        it->index += QUOTA_LEDGER_CHUNK_SLOTS;
        it->chunk  = it->chunk->next;
    }
    if (it->chunk) {
        it->within = start - it->index;
        it->index  = start;
    }
}

static uint32_t quota_ledger_drain_ledger(quota_ledger_t *ledger, uint32_t *budget,
                                          int *out_more, uint32_t *out_credited)
{
    quota_ledger_iter_t  it;
    quota_ledger_slot_t *slot;
    uint32_t             index    = 0;
    uint32_t             done     = 0;   /* credited or proven stale */
    uint32_t             settled  = 0;   /* references this pass must drop */
    uint32_t             retried  = 0;   /* slots left BUSY on this visit */
    uint32_t             retries_at_entry;
    uint32_t             start;
    uint32_t             last     = 0;
    int                  wrapped  = 0;
    uint64_t             flags;

    *out_more = 0;

    spin_lock_irqsave(&g_defer_lock, &flags);
    start = ledger->defer_cursor;
    spin_unlock_irqrestore(&g_defer_lock, flags);

    /* Read the retry budget ONCE, and judge every slot on this visit against
     * that value. Consulting a counter mutated inside the loop would make the
     * bound count BUSY SLOTS instead of no-progress CALLBACKS: nine
     * simultaneously-BUSY slots would push the ninth over an 8-callback cap
     * within a single visit, abandoning a charge that had not been retried even
     * once. */
    retries_at_entry = __atomic_load_n(&ledger->defer_retries, __ATOMIC_RELAXED);

    quota_ledger_iter_init_at(&it, ledger, start);
    while ((slot = quota_ledger_iter_next(&it, &index)) != (quota_ledger_slot_t *)0) {

        if (*budget == 0u) {
            *out_more = 1;
            break;
        }
        (*budget)--;
        last = index;

        int64_t token = atomic64_read(&slot->deferred_token);
        if (token == 0)
            continue;

        /* Take the deferral before completing it. The claim is what stops a
         * later pass from completing the same obligation twice and dropping its
         * transferred reference twice. */
        if (atomic64_cmpxchg(&slot->deferred_token, token, 0) != token)
            continue;

        /* Complete against the owner word AS IT STANDS -- epoch|DEFERRED. That
         * is the value the release CAS inside the completion must match, and
         * matching it is what finally clears the mark and frees the slot for
         * reuse. Nobody can have changed it: the mark is what makes the slot
         * unclaimable, and this pass owns the mark. */
        int64_t owner   = atomic64_read(&slot->owner);
        int     outcome = quota_ledger_complete_return(ledger, slot,
                                                       (uint64_t)token,
                                                       (uint64_t)owner);

        if (outcome == QUOTA_COMPLETE_RETRY &&
            retries_at_entry < QUOTA_LEDGER_DEFER_RETRY_MAX) {
            /* The charge is STILL LIVE -- an adjust on another CPU outlasted the
             * bounded retries. Put the stamp back and leave everything else
             * alone: the pending count stays up (so this ledger is requeued),
             * the transferred reference is NOT dropped, and nothing is counted
             * completed. Dropping it here instead would strand a live charge
             * with its slot occupied until reap, and report success doing it.
             *
             * Restoring is safe without a CAS: the slot still carries our
             * DEFERRED mark, so no producer can have stamped it in between. */
            atomic64_set(&slot->deferred_token, token);
            retried++;
            *out_more = 1;
            continue;
        }

        if (outcome == QUOTA_COMPLETE_RETRY) {
            /* Retry budget exhausted. Give up on this obligation the way the
             * pre-deferral code always did -- leave the charge for the ledger's
             * own orphan drain to reclaim and count -- rather than letting a
             * receipt that stays BUSY re-arm this worker forever. The bound is
             * what stops a persistent BUSY owner from turning the shared drain
             * into a hot loop; making the handoff lossless instead needs the
             * owner to CONSUME a pending return, which needs cross-CPU
             * contention to validate and is owned by the infrastructure-gated
             * charge-path bounding work in the resource-accounting roadmap. */
            __atomic_fetch_add(&g_ledger_abandons, 1, __ATOMIC_RELAXED);
            if (__atomic_fetch_sub(&ledger->defer_pending, 1, __ATOMIC_RELAXED) == 1u)
                __atomic_store_n(&ledger->defer_retries, 0u, __ATOMIC_RELAXED);
            settled++;
            /* END THE VISIT. retries_at_entry was snapshotted before the loop,
             * so continuing would judge a deferral a producer published DURING
             * this visit against a budget that belongs to the episode just
             * abandoned -- and abandon it on its first collision. */
            break;
        }

        /* CREDITED or STALE: the obligation is settled either way, so the
         * transferred reference is the caller's to drop and the counters move.
         * An abandoned one is settled too -- its reference must still be dropped
         * -- but it is deliberately NOT counted completed, because
         * quota_ledger_deferrals_completed means the drain credited it. */
        /* The 1-to-0 transition IS the end of the deferral episode, and taking
         * it from the decrement itself is what makes it reliable: a separate
         * "is it zero now" load afterwards can be beaten by a producer that
         * publishes the next obligation in between, which would leave the
         * exhausted budget in place for an episode that never used it. */
        if (__atomic_fetch_sub(&ledger->defer_pending, 1, __ATOMIC_RELAXED) == 1u)
            __atomic_store_n(&ledger->defer_retries, 0u, __ATOMIC_RELAXED);
        settled++;
        done++;
    }
    if (!*out_more)
        wrapped = 1;   /* the sweep reached the end of the slot array */

    spin_lock_irqsave(&g_defer_lock, &flags);
    ledger->defer_cursor = wrapped ? 0u : (last + 1u);
    spin_unlock_irqrestore(&g_defer_lock, flags);

    /* Come back if and only if this ledger still owes something. Terminating,
     * because every visit either completes at least one obligation or finds the
     * count already zero -- and complete, because it does not depend on where in
     * the array the remaining work happens to sit. */
    *out_more = (__atomic_load_n(&ledger->defer_pending, __ATOMIC_RELAXED) != 0u);

    /* ONE retry-budget update per VISIT, from whether the visit made progress.
     * Progress clears the budget -- the bound exists to stop a STUCK ledger from
     * spinning the worker, not to cap a busy one that is steadily draining. */
    if (done)
        __atomic_store_n(&ledger->defer_retries, 0u, __ATOMIC_RELAXED);
    else if (retried)
        __atomic_fetch_add(&ledger->defer_retries, 1, __ATOMIC_RELAXED);


    if (done)
        __atomic_fetch_add(&g_defer_completed, done, __ATOMIC_RELAXED);
    if (settled)
        __atomic_fetch_sub(&g_defer_pending, settled, __ATOMIC_RELAXED);
    if (out_credited)
        *out_credited = done;
    return settled;
}

/* One bounded pass over the pending list. PASSIVE_LEVEL, one at a time. */
static uint32_t quota_ledger_drain_pass(int is_worker)
{
#ifndef KERNEL_TESTS
    (void)is_worker;
#endif
    uint32_t completed = 0;
    uint32_t visited   = 0;
    /* ONE budget for the whole callback, not one per ledger. Per-ledger budgets
     * multiplied: 16 dequeues x 512 slots is 8192 completions, each of which can
     * take up to QUOTA_CHAIN_MAX block lock sections, on the SHARED threaded-DPC
     * worker that also runs pressure publication. A destroy is charged against
     * the same allowance for the same reason -- its orphan scan and its chunk
     * frees are real work, and leaving them unbudgeted let 16 maxed ledgers add
     * thousands of slot checks and up to 1024 frees to a callback that had
     * already spent its slot allowance. */
    uint32_t budget    = QUOTA_LEDGER_DRAIN_SLOTS;

    /* Claim the drain. A refused pass returns immediately rather than walking a
     * ledger another pass is already inside; the pass that holds the claim
     * rechecks the list before it finishes, so nothing is stranded. */
    if (__atomic_exchange_n(&g_defer_draining, 1u, __ATOMIC_ACQ_REL) != 0u)
        return 0;

#ifdef KERNEL_TESTS
    /* Re-check the hold AFTER claiming, for the WORKER only. A worker that
     * passed the entry check before a test took the hold would otherwise still
     * be admitted here -- the hold's wait watches only this claim, so such a
     * worker is invisible to it and would go on completing the very obligations
     * the test is asserting about. The test's own synchronous drain is exempt:
     * the hold exists to hand it the queue, not to lock it out. */
    if (is_worker && __atomic_load_n(&g_defer_test_hold, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_defer_draining, 0u, __ATOMIC_RELEASE);
        return 0;
    }
#endif

    while (visited++ < QUOTA_LEDGER_DRAIN_LEDGERS && budget > 0u) {
        quota_ledger_t *ledger = quota_ledger_defer_pop();
        if (!ledger)
            break;

        int destroy = ledger->defer_destroy;

        if (destroy && budget < QUOTA_LEDGER_DESTROY_COST) {
            /* Not enough allowance left to pay for a destroy, and a destroy
             * cannot be done by halves -- it scans every slot and frees every
             * chunk. Admitting it on a nearly-spent budget was how a callback
             * could run 511 slot examinations AND a full destroy, roughly
             * doubling the bound it is supposed to enforce. Put it back and end
             * the pass; the re-arm below picks it up with a full allowance. */
            quota_ledger_defer_push_front(ledger);
            break;
        }

        if (destroy) {
            /* The final reference was dropped at raised IRQL, so nothing else
             * can reach this ledger and the destroy is ours alone to run --
             * here, at PASSIVE_LEVEL, where its orphan drain and its chunk frees
             * cost an interrupt nothing. A destroy entry carries no membership
             * reference (its refcount is already zero), so there is none to
             * drop afterwards. */
            ledger->defer_destroy = 0u;
            /* Charge the destroy its worst case up front: it walks every slot
             * and can free up to QUOTA_LEDGER_MAX_CHUNKS chunks, so it is at
             * least as expensive as a full slot budget. */
            budget -= QUOTA_LEDGER_DESTROY_COST;
            quota_ledger_destroy(ledger);
            __atomic_fetch_add(&g_defer_destroyed, 1, __ATOMIC_RELAXED);
            continue;
        }

        int      more    = 0;
        /* The count returned is the number of transferred references this pass
         * must give back -- credited, stale AND abandoned alike. It is
         * deliberately not the "completed" telemetry, which counts only credits. */
        uint32_t credited = 0;
        uint32_t settled   = quota_ledger_drain_ledger(ledger, &budget, &more,
                                                       &credited);
        /* Report only what was CREDITED. `settled` also covers abandoned
         * obligations, whose charge is still outstanding -- reporting those as
         * completed would contradict this function's own contract. */
        completed += credited;

        /* Requeue BEFORE releasing anything: the decision reads the ledger, and
         * the requeue takes its own membership reference. Tail, so a large
         * ledger cannot starve the ones behind it. */
        if (more)
            quota_ledger_defer_push(ledger, 0);

        /* NOW drop what the completions transferred, and the membership
         * reference last. Every ledger access above is finished, so the deref
         * that finally destroys it has nothing left to race.
         *
         * EVERY ONE OF THESE DROPS IS BUDGETED, because any of them can be the
         * last: we are at PASSIVE_LEVEL, where a plain final deref destroys
         * INLINE -- an orphan scan over every slot plus up to
         * QUOTA_LEDGER_MAX_CHUNKS frees -- on top of a pass that may already
         * have spent its whole allowance. */
        int deferred_destroy = 0;
        for (uint32_t i = 0; i < settled; i++)
            deferred_destroy |= quota_ledger_deref_budgeted(ledger, &budget);
        deferred_destroy |= quota_ledger_deref_budgeted(ledger, &budget);

        if (deferred_destroy)
            break;   /* the teardown is queued; let the next callback pay */
    }

    __atomic_store_n(&g_defer_draining, 0u, __ATOMIC_RELEASE);
    return completed;
}

/* Threaded DPC routine: PASSIVE_LEVEL, so the block teardown and the heap frees
 * a completion performs are all legal here.
 *
 * The disarm-then-drain-then-recheck shape closes the lost-wakeup window, and it
 * is the same shape quota_pressure_drain uses: a producer that pushes between
 * the last pop and the disarm would otherwise find the DPC still armed, skip its
 * insert, and leave its obligation pending until some unrelated event. Clearing
 * the flag FIRST means such a producer either arms us again (its exchange sees
 * 0) or we see its ledger in the recheck below. */
static void quota_ledger_deferred_drain(KDPC *dpc, void *context, void *arg1,
                                        void *arg2)
{
    (void)dpc; (void)context; (void)arg1; (void)arg2;

    __atomic_store_n(&g_defer_armed, 0u, __ATOMIC_RELEASE);

#ifdef KERNEL_TESTS
    /* A callback queued before the hold was taken can still be dispatched after
     * it. Checking here as well as in the arm is what makes the hold actually
     * hold; the disarm above already ran, so a later producer can re-arm. */
    if (__atomic_load_n(&g_defer_test_hold, __ATOMIC_ACQUIRE))
        return;
#endif

    (void)quota_ledger_drain_pass(1);

    /* Work left over, either because a producer raced the disarm or because this
     * pass hit its budget. Re-arm so the remainder is another bounded visit. */
    if (quota_ledger_defer_nonempty())
        quota_ledger_defer_arm();
}

void quota_ledger_init(void)
{
    if (__atomic_load_n(&g_defer_ready, __ATOMIC_ACQUIRE))
        return;

    /* Claim the initialisation with an exchange, the same single-ownership shape
     * g_defer_armed and g_defer_draining use. A load-then-store pair would let
     * two callers both pass the guard and both re-initialise the static KDPC --
     * which resets its queue linkage, potentially while it is queued. One caller
     * today (boot_desktop, Phase 3, BSP), but the header promises idempotence
     * without qualifying it to sequential calls, so it should hold. */
    if (__atomic_exchange_n(&g_defer_initialising, 1u, __ATOMIC_ACQ_REL) != 0u)
        return;

    /* PUBLISH READY ONLY IF A WORKER ACTUALLY EXISTS. dpc_start_threads rolls
     * its started flag back and lets boot continue when the worker task cannot
     * be created, and in that degraded state a deferral would be strictly worse
     * than no deferral at all: the return transfers its only reference to a
     * queue nothing will ever drain, so the charge, its block references and the
     * ledger stay outstanding for the rest of the boot. Staying un-ready keeps
     * the forced-inline behaviour, which is unbounded but correct, and the
     * counter says how often it happened. This is idempotent, so a later call
     * once the worker is up promotes the subsystem cleanly. */
    if (!dpc_worker_started()) {
        klog(LOG_ERROR, "quota",
             "no threaded-DPC worker: ledger returns above PASSIVE_LEVEL will "
             "complete inline (unbounded); see quota_ledger_deferrals_forced");
        /* Release the claim: this call achieved nothing, and a later one once
         * the worker exists must be able to try again. */
        __atomic_store_n(&g_defer_initialising, 0u, __ATOMIC_RELEASE);
        return;
    }

    KeInitializeThreadedDpc(&g_defer_dpc, quota_ledger_deferred_drain, (void *)0);

    /* Publish LAST. A producer that sees the ready flag must find a fully
     * prepared KDPC behind it, so the release store is what orders the two. */
    __atomic_store_n(&g_defer_ready, 1u, __ATOMIC_RELEASE);

    klog(LOG_INFO, "quota", "ledger deferred-completion worker armed (CPU %u)",
         (uint64_t)QUOTA_LEDGER_SERVICE_CPU);

    /* Anything deferred during the pre-worker window, or pushed by a destroy that
     * found no worker, is drained now rather than waiting for the next producer. */
    if (quota_ledger_defer_nonempty())
        quota_ledger_defer_arm();
}

uint32_t quota_ledger_drain_now(void)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return 0;
    return quota_ledger_drain_pass(0);
}

uint32_t quota_ledger_deferrals_pending(void)
{
    return __atomic_load_n(&g_defer_pending, __ATOMIC_RELAXED);
}

uint64_t quota_ledger_deferrals_forced(void)
{
    return __atomic_load_n(&g_defer_forced, __ATOMIC_RELAXED);
}

uint64_t quota_ledger_deferrals_completed(void)
{
    return __atomic_load_n(&g_defer_completed, __ATOMIC_RELAXED);
}

uint64_t quota_ledger_deferrals_destroyed(void)
{
    return __atomic_load_n(&g_defer_destroyed, __ATOMIC_RELAXED);
}

NTSTATUS quota_ledger_charge_current(quota_resource_type_t type, uint64_t amount,
                                     quota_obligation_t *out)
{
    return quota_ledger_charge_current_from(type, amount, QUOTA_SOURCE_UNKNOWN,
                                            out);
}

NTSTATUS quota_ledger_charge_current_from(quota_resource_type_t type,
                                          uint64_t amount,
                                          quota_charge_source_t source,
                                          quota_obligation_t *out)
{
    struct task *task;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    /* EMPTY THE HANDLE BEFORE ANY OTHER REFUSAL, source validation included.
     * Every failure of this API leaves an empty obligation -- that is what lets
     * a caller run one unconditional cleanup path instead of branching on the
     * status -- and returning early on a bad source would hand back whatever the
     * caller's storage happened to contain, which a cleanup then treats as a
     * live handle. The explicit-task form clears first for the same reason, and
     * the two entry points must not differ on it. */
    out->ledger = (quota_ledger_t *)0;
    out->slot   = 0;
    out->token  = 0;
    out->epoch  = 0;

    if (!quota_source_valid(source))
        return STATUS_INVALID_PARAMETER;

    /* Validate the TYPE before the boot exemption, for the same reason
     * quota_charge_current does: an exempted charge that skipped this would
     * report SUCCESS for an out-of-range type, so a subsystem wired to the wrong
     * quota_resource_type_t would go unnoticed until the first charge made after
     * SUBSYS_SCHED came up. A pure range check against the static taxonomy is
     * valid before the registry is populated. */
    if (!quota_resource_desc(type))
        return STATUS_INVALID_PARAMETER;

    /* The ledger allocates, so this entry point is PASSIVE_LEVEL and says so with
     * a status. quota_charge_current needs no such guard because it never
     * allocates; a consumer moved from one to the other does. */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_UNSUCCESSFUL;

    /* Boot exemption, reproduced from quota_charge_current so that a converted
     * consumer keeps the behaviour it was written against. Both conditions are
     * milestones, not per-caller state: the taxonomy must be validated before a
     * limit means anything, and the scheduler must be publishing tasks before one
     * can be billed. The empty obligation left in `out` owes nothing, which
     * quota_ledger_return already handles as a no-op. */
    if (!quota_registry_ready() || !kernel_subsystem_ready(SUBSYS_SCHED))
        return STATUS_SUCCESS;

    task = task_current();
    if (!task) {
        /* PARITY, and it is fail-CLOSED on purpose. quota_charge_current returns
         * STATUS_PROCESS_IS_TERMINATING here (quota_owner.c:771). Reporting
         * SUCCESS instead would hand the caller an empty obligation and let it
         * create the resource with nothing owed -- a missing current-task cursor
         * turned into unaccounted usage. Not reachable while task_current()
         * always resolves, but it becomes reachable with the per-CPU cursor. */
        return STATUS_PROCESS_IS_TERMINATING;
    }

    /* RANGE AND ZERO ARE HANDLED BY quota_ledger_charge_from, not duplicated
     * here. They used to live in this wrapper alone, which is exactly how the
     * two entry points came to disagree at a full ceiling; one implementation
     * cannot drift from itself. */
    return quota_ledger_charge_from(task, type, amount, source, out);
}

NTSTATUS quota_ledger_adjust(quota_obligation_t *ob, uint64_t new_amount)
{
    if (!ob || !ob->ledger || ob->token == 0)
        return STATUS_INVALID_PARAMETER;

    /* PIN THE LEDGER for the whole transaction. This is not defensive tidiness:
     * a concurrent quota_ledger_return on the same obligation that exhausts its
     * retries ABANDONS -- it drops the handle's reference -- and if that was the
     * last reference the ledger is destroyed and its chunks freed while this
     * adjust is still writing receipt->amount and republishing the tag. Both
     * sibling walkers (migrate, unmigrate) already pin via quota_ledger_peek;
     * this one relied on the caller's handle reference, which the abandon path is
     * explicitly designed to drop out from under it. */
    quota_ledger_t *ledger = ob->ledger;
    if (!quota_ledger_try_ref(ledger))
        return STATUS_INVALID_PARAMETER;   /* already committed to teardown */

    quota_ledger_slot_t *slot = quota_ledger_slot_at(ledger, ob->slot);
    if (!slot) {
        quota_ledger_deref(ledger);
        return STATUS_INVALID_PARAMETER;
    }

    /* An adjust RAISES charged usage, so it must be serialized against a
     * membership transition exactly as a charge is. Without this an obligation
     * could grow between the absorb and the publication: the absorb folded in the
     * old amount, the migration can only adopt what the absorb record covers, and
     * the job ends up under-charged by the difference while the process carries
     * all of it -- a job limit bypassed by however much the adjust added.
     *
     * The owner back-pointer is trusted ONLY while the task still points at THIS
     * ledger. That check is what distinguishes a live owner from a recycled task
     * slot, and it is read under the same lock that publishes and clears the
     * pointer. When the owner is gone the adjust proceeds ungated, which is
     * correct rather than a gap: a task that no longer owns this ledger cannot be
     * joining a job with it, so there is no transition to serialize against. */
    struct task *owner = ledger->owner;
    int          gated = 0;

    if (owner) {
        uint64_t flags;
        int      still_owner;
        spin_lock_irqsave(&owner->quota_lock, &flags);
        still_owner = (owner->quota_ledger == ledger);
        spin_unlock_irqrestore(&owner->quota_lock, flags);

        if (still_owner) {
            uint32_t gate_state = QUOTA_GATE_OPEN;
            if (!quota_gate_enter(owner, &gate_state)) {
                quota_ledger_deref(ledger);
                return (gate_state == QUOTA_GATE_SEALED)
                           ? STATUS_PROCESS_IS_TERMINATING
                           : STATUS_RETRY;
            }
            /* RE-CHECK ownership now that we are inside the gate. The check above
             * released quota_lock before entering, so a reap could have cleared
             * the pointer in between and this would be gating a task that no
             * longer owns this ledger. Re-reading under the gate closes that
             * window; if it moved, drop the gate and proceed ungated, which is
             * the correct answer for an orphaned ledger (no task can be joining a
             * job with it, so there is no transition to serialize against). */
            spin_lock_irqsave(&owner->quota_lock, &flags);
            still_owner = (owner->quota_ledger == ledger);
            spin_unlock_irqrestore(&owner->quota_lock, flags);
            if (still_owner) {
                gated = 1;
            } else {
                quota_gate_exit(owner);
            }
        }
    }

    NTSTATUS status = quota_charge_adjust(&slot->receipt, ob->token, new_amount);

    if (gated)
        quota_gate_exit(owner);
    quota_ledger_deref(ledger);
    return status;
}

uint32_t quota_ledger_migrate_to_job(struct task *task,
                                     struct quota_block *job_block,
                                     quota_absorb_record_t *rec)
{
    if (!task || !job_block || !rec || !rec->active)
        return 0;

    /* Do NOT create one: a task with no ledger has no obligations to migrate,
     * and allocating here would put an allocation on the job-assign path for no
     * benefit. */
    quota_ledger_t *ledger = quota_ledger_peek(task);
    if (!ledger)
        return 0;

    uint32_t             migrated = 0;
    quota_ledger_iter_t  it;
    quota_ledger_slot_t *slot;

    quota_ledger_iter_init(&it, ledger);
    while ((slot = quota_ledger_iter_next(&it, (uint32_t *)0)) != (quota_ledger_slot_t *)0) {
        if (atomic64_read(&slot->owner) == 0)
            continue;

        int64_t tag = atomic64_read(&slot->receipt.tag);
        if (QUOTA_RECEIPT_TAG_STATE(tag) != QUOTA_RECEIPT_ACTIVE)
            continue;

        uint64_t generation = QUOTA_RECEIPT_TAG_GEN(tag);

        /* CLAIM the obligation for the duration of its adoption, exactly as a
         * charge or a return would. Without this a returner could walk blocks[]
         * while the job block is being appended and see a count that does not
         * match the array. Losing the CAS means a returner got here first: skip
         * it, and its amount stays with the absorb record.
         *
         * INTERRUPTS ARE MASKED across the claim-to-republish window for the same
         * reason quota_charge_adjust masks its own: a return running in a same-CPU
         * interrupt would find the receipt BUSY with no way to make progress,
         * because the operation it must wait for cannot resume until the interrupt
         * returns. Masking makes this window invisible to that CPU, leaving only
         * cross-CPU collisions where the owner is actually running. The window is
         * a handful of field writes, so the cost is negligible. */
        uint64_t slot_irq = local_irq_save();
        if (atomic64_cmpxchg(&slot->receipt.tag, tag,
                             QUOTA_RECEIPT_TAG(generation,
                                               QUOTA_RECEIPT_BUSY)) != tag) {
            local_irq_restore(slot_irq);
            continue;
        }

        quota_charge_receipt_t *receipt = &slot->receipt;
        quota_resource_type_t   type    = receipt->type;
        uint64_t                amount  = receipt->amount;
        int                     adopted = 0;

        if (receipt->count < QUOTA_CHAIN_MAX && amount != 0 &&
            (uint32_t)type < QUOTA_RESOURCE_TYPE_COUNT &&
            rec->taken[type] >= amount) {

            int already = 0;
            for (uint32_t j = 0; j < receipt->count; j++) {
                if (receipt->blocks[j] == (quota_block_t *)job_block) {
                    already = 1;
                    break;
                }
            }

            if (!already) {
                /* The receipt owns one reference per block it names, so the
                 * appended job block needs one too -- its return will drop it. */
                quota_block_ref((quota_block_t *)job_block);
                receipt->blocks[receipt->count++] = (quota_block_t *)job_block;
                /* ADOPT rather than charge: the absorb already billed the job
                 * for this amount, so ownership of the eventual return moves
                 * from the absorb record to this receipt and the job's total
                 * usage does not change by a single byte. */
                rec->taken[type] -= amount;
                adopted = 1;
            }
        }

        /* Republish at the SAME generation. Advancing it would invalidate the
         * holder's token and strand the charge. */
        atomic64_set(&receipt->tag,
                     QUOTA_RECEIPT_TAG(generation, QUOTA_RECEIPT_ACTIVE));
        local_irq_restore(slot_irq);
        if (adopted)
            migrated++;
    }

    if (migrated) {
        /* A record whose every amount was adopted owns nothing, so mark it
         * inactive: an unabsorb at detach must not look like it has work. */
        int any = 0;
        for (uint32_t i = 0; i < QUOTA_RESOURCE_TYPE_COUNT; i++) {
            if (rec->taken[i]) {
                any = 1;
                break;
            }
        }
        if (!any)
            rec->active = 0;
    }

    quota_ledger_deref(ledger);
    return migrated;
}

/* Resolve the task's ledger WITHOUT creating one, taking a reference. NULL when
 * the task never charged through a ledger, which both walkers below treat as
 * "nothing to do" rather than an error. */
static quota_ledger_t *quota_ledger_peek(struct task *task)
{
    uint64_t        flags;
    quota_ledger_t *ledger;

    spin_lock_irqsave(&task->quota_lock, &flags);
    ledger = task->quota_ledger;
    /* try-ref, not ref: the pointer is published and cleared under this lock, but
     * the COUNT can still be racing a final deref from an obligation holder. */
    if (ledger && !quota_ledger_try_ref(ledger))
        ledger = (quota_ledger_t *)0;
    spin_unlock_irqrestore(&task->quota_lock, flags);
    return ledger;
}

uint32_t quota_ledger_unmigrate_from_job(struct task *task,
                                         struct quota_block *job_block,
                                         quota_absorb_record_t *rec)
{
    if (!task || !job_block || !rec)
        return 0;

    quota_ledger_t *ledger = quota_ledger_peek(task);
    if (!ledger)
        return 0;

    uint32_t             reverted = 0;
    quota_ledger_iter_t  it;
    quota_ledger_slot_t *slot;

    quota_ledger_iter_init(&it, ledger);
    while ((slot = quota_ledger_iter_next(&it, (uint32_t *)0)) != (quota_ledger_slot_t *)0) {
        if (atomic64_read(&slot->owner) == 0)
            continue;

        int64_t tag = atomic64_read(&slot->receipt.tag);
        if (QUOTA_RECEIPT_TAG_STATE(tag) != QUOTA_RECEIPT_ACTIVE)
            continue;

        uint64_t generation = QUOTA_RECEIPT_TAG_GEN(tag);

        /* Claim it exactly as the migration did, interrupts masked for the same
         * reason (see quota_ledger_migrate_to_job). Losing means a returner is
         * mid-return: skip it, and see the header for why that still conserves
         * (the return credits the job through the appended block). */
        uint64_t slot_irq = local_irq_save();
        if (atomic64_cmpxchg(&slot->receipt.tag, tag,
                             QUOTA_RECEIPT_TAG(generation,
                                               QUOTA_RECEIPT_BUSY)) != tag) {
            local_irq_restore(slot_irq);
            continue;
        }

        quota_charge_receipt_t *receipt = &slot->receipt;
        int                     removed = 0;

        for (uint32_t j = 0; j < receipt->count; j++) {
            if (receipt->blocks[j] != (quota_block_t *)job_block)
                continue;

            /* Swap-remove: the charged set is unordered (a return walks all of
             * it, and the adjust sorts by address), so compacting in place is
             * correct and cheaper than shifting. */
            receipt->blocks[j] = receipt->blocks[receipt->count - 1];
            receipt->blocks[receipt->count - 1] = (quota_block_t *)0;
            receipt->count--;
            /* Drop the reference the migration took for this block. */
            quota_block_deref((quota_block_t *)job_block);
            removed = 1;
            break;
        }

        if (removed && (uint32_t)receipt->type < QUOTA_RESOURCE_TYPE_COUNT) {
            /* Give the amount back to the absorb record, so the caller's
             * unabsorb withdraws it from the job like it never moved. */
            rec->taken[receipt->type] += receipt->amount;
            if (receipt->amount)
                rec->active = 1;
            reverted++;
        }

        atomic64_set(&receipt->tag,
                     QUOTA_RECEIPT_TAG(generation, QUOTA_RECEIPT_ACTIVE));
        local_irq_restore(slot_irq);
    }

    quota_ledger_deref(ledger);
    return reverted;
}

void quota_ledger_task_release(struct task *task)
{
    if (!task)
        return;

    /* Wait for anything already inside the charge path to finish. The gate was
     * sealed at death, so nothing new can start; this only waits out an
     * operation that had already been admitted. Bounded -- a stuck charger must
     * not wedge a reap -- and the refcount check below stays correct either way,
     * because an in-flight charger holds its own ledger reference. */
    (void)quota_gate_wait_idle(task);

    uint64_t        flags;
    quota_ledger_t *ledger;

    spin_lock_irqsave(&task->quota_lock, &flags);
    ledger = task->quota_ledger;
    task->quota_ledger = (quota_ledger_t *)0;
    spin_unlock_irqrestore(&task->quota_lock, flags);

    if (!ledger)
        return;

    /* THE classification that keeps this assertion honest. A remaining
     * obligation is only a leak if nobody can still return it. If any other
     * holder exists, the obligation belongs to a resource that legitimately
     * outlives this task -- an object body or a namespace entry -- which is the
     * entire reason the ledger is refcounted and not task-embedded. Reporting
     * that as a leak would make the sweep cry wolf on correct behavior. */
    if (atomic_read(&ledger->refcount) == 1) {
        uint32_t orphans = quota_ledger_drain_orphans(ledger, (uint32_t *)0);
        if (orphans) {
            __atomic_fetch_add(&g_ledger_leaks, (uint64_t)orphans,
                               __ATOMIC_RELAXED);
            /* IRQL-gated like every other diagnostic on a teardown path: klog's
             * live-disk flush re-enters the VFS and can stall a reap. The
             * counter is always bumped, so the anomaly is never lost even when
             * the message is suppressed. */
            if (KeGetCurrentIrql() == PASSIVE_LEVEL)
                klog(LOG_ERROR, "quota",
                     "ledger %llu: %u orphaned obligation(s) reclaimed at reap",
                     (uint64_t)ledger->id, (uint64_t)orphans);
        }
    }

    quota_ledger_deref(ledger);
}
