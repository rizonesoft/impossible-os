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
static uint64_t g_ledger_abandons;

uint64_t quota_gate_retry_count(void)
{
    return __atomic_load_n(&g_gate_retry_refusals, __ATOMIC_RELAXED);
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
#define QUOTA_LEDGER_CHUNK_SLOTS    24u

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
 * 64 chunks is ~1544 slots and ~151 KiB, so a maxed ledger takes single-digit
 * percent of the heap. It is deliberately a placeholder: the real answer is
 * page-backed storage or a compact common-depth receipt, and choosing between
 * them needs to know what the Object Manager charge points actually charge --
 * per handle or per table. Tracked as concrete follow-up work. */
#define QUOTA_LEDGER_MAX_CHUNKS     64u

typedef struct quota_ledger_slot {
    quota_charge_receipt_t receipt;
    atomic64_t             owner;   /* 0 = free, else this allocation's epoch */
} quota_ledger_slot_t;

typedef struct quota_ledger_chunk {
    struct quota_ledger_chunk *next;
    quota_ledger_slot_t        slots[QUOTA_LEDGER_CHUNK_SLOTS];
} quota_ledger_chunk_t;

/* kmalloc is documented for allocations up to 4 KB (CLAUDE.md); anything larger
 * must use pmm_alloc_contiguous. Asserting the chunk size here is what keeps a
 * future slot-count or receipt-width change from silently crossing that line. */
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
    quota_ledger_slot_t   inline_slots[QUOTA_LEDGER_INLINE_SLOTS];
};

_Static_assert(sizeof(struct quota_ledger) <= 4096,
               "a ledger must stay within the kmalloc ceiling");

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
        if (QUOTA_RECEIPT_TAG_STATE(tag) != QUOTA_RECEIPT_ACTIVE) {
            /* Allocated but holding nothing: a charge that failed after the claim.
             * Just release the slot. */
            atomic64_set(&slot->owner, 0);
            continue;
        }

        /* We are the only agent that can still name this charge, so its own
         * generation is the token. */
        quota_return_chain(&slot->receipt, QUOTA_RECEIPT_TAG_GEN(tag));
        atomic64_set(&slot->owner, 0);
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

void quota_ledger_deref(quota_ledger_t *ledger)
{
    if (!ledger)
        return;
    if (!atomic_dec_and_test(&ledger->refcount))
        return;
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
    if (!out)
        return STATUS_INVALID_PARAMETER;

    out->ledger = (quota_ledger_t *)0;
    out->slot   = 0;
    out->token  = 0;
    out->epoch  = 0;      /* zeroed WITH the others: the epoch is the field the
                           * stale-duplicate defence rests on, so it must never
                           * be left holding a caller's garbage on a failure or
                           * zero-amount path. */

    if (!task)
        return STATUS_INVALID_PARAMETER;

    quota_ledger_t *ledger = quota_ledger_acquire(task);
    if (!ledger) {
        /* Distinguish "dying" from "out of memory": a caller can retry the
         * second and must not retry the first. */
        return (quota_gate_state_of(task) == QUOTA_GATE_SEALED)
                   ? STATUS_PROCESS_IS_TERMINATING
                   : STATUS_INSUFFICIENT_RESOURCES;
    }

    uint64_t epoch = 0;
    int32_t  index = quota_ledger_claim_slot(ledger, &epoch);
    if (index < 0) {
        quota_ledger_deref(ledger);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    quota_ledger_slot_t *slot = quota_ledger_slot_at(ledger, (uint32_t)index);
    if (!slot) {
        /* Cannot happen: the index came from a resolved slot. Handled anyway so
         * the claim is never stranded if it ever does. */
        quota_ledger_deref(ledger);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    uint64_t token = 0;
    /* The gate is entered INSIDE quota_charge_chain, not here: that is the one
     * funnel every chain charge passes through, so gating there covers the
     * charges whose receipts live in their caller's own storage too. */
    NTSTATUS status = quota_charge_chain(task, type, amount, 0,
                                        &slot->receipt, &token);
    if (status != STATUS_SUCCESS) {
        atomic64_set(&slot->owner, 0);
        quota_ledger_deref(ledger);
        return status;
    }

    if (token == 0) {
        /* A zero-amount charge succeeds while owing nothing (quota.h calls 0
         * the canonical no-obligation token). There is no obligation to record,
         * so release the slot and hand back an EMPTY handle -- returning it
         * later must be a no-op, not a return of whatever reuses this slot. */
        atomic64_set(&slot->owner, 0);
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

int quota_ledger_return(quota_obligation_t *ob)
{
    if (!ob || !ob->ledger || ob->token == 0)
        return 0;

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
        return 0;
    }

    /* Retry the WHOLE return, not merely the tag CAS inside it. A return can fail
     * to land for exactly one reason -- colliding with an in-progress adjust on
     * this same charge -- and that collision clears the moment the adjust
     * republishes, so re-reading and trying again keeps the failure off the caller
     * in every case where the adjust is making progress. And it always is: an
     * adjust masks interrupts across its whole claim-to-republish window, so it
     * can only be observed BUSY from ANOTHER CPU, where it is running. */
    int completed = 0;
    int still_live = 0;

    for (uint32_t attempt = 0; attempt < QUOTA_LEDGER_RETURN_TRIES; attempt++) {
        quota_return_chain(&slot->receipt, ob->token);

        int64_t  tag   = atomic64_read(&slot->receipt.tag);
        uint64_t gen   = QUOTA_RECEIPT_TAG_GEN(tag);
        uint32_t state = QUOTA_RECEIPT_TAG_STATE(tag);

        /* ONLY IDLE at our own generation proves the return completed. */
        if (gen == ob->token && state == QUOTA_RECEIPT_IDLE) {
            completed = 1;
            break;
        }
        /* Our generation in ANY other state means the charge is still LIVE: BUSY
         * while an adjust owns it, or ACTIVE again because the adjust just
         * republished it. Both are retryable, and treating ACTIVE as anything
         * else would be the worst possible mistake here -- it would classify a
         * live charge as stale and destroy the only handle able to return it. */
        if (gen == ob->token) {
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
            /* The charge is live but an adjust outlasted every attempt. ABANDON it:
             * drop this handle's reference and empty the handle, WITHOUT releasing
             * the slot. That is what makes the reap-time sweep a real backstop
             * rather than a promise -- retaining the reference instead would hold
             * the refcount above the task's own claim forever, so the drain would
             * never run and the charge, its block references, and its slot would
             * outlive the boot. Dropping it means the ledger's own drain (at the
             * task release, or at destruction) reclaims the charge and counts it.
             *
             * The adjust that owns the slot holds its own obligation reference, so
             * this deref can never destroy the ledger underneath it.
             *
             * COUNTED, because the contract in quota_ledger.h claims this is rare
             * (it needs a cross-CPU collision with an owner queued behind a
             * contended block lock) and a claim like that should be measurable
             * rather than merely asserted. A nonzero count here with a matching
             * rise in the leak count is the signature to investigate. */
            __atomic_fetch_add(&g_ledger_abandons, 1, __ATOMIC_RELAXED);
            ob->ledger = (quota_ledger_t *)0;
            ob->slot   = 0;
            ob->token  = 0;
            ob->epoch  = 0;
            quota_ledger_deref(ledger);
            return 0;
        }

        /* Otherwise this handle is STALE -- its charge was returned already and the
         * slot has moved on. It owes nothing, so it is emptied and its reference
         * dropped, exactly like the documented no-op double return. The slot is
         * deliberately NOT released: this handle does not own it. */
        ob->ledger = (quota_ledger_t *)0;
        ob->slot   = 0;
        ob->token  = 0;
        ob->epoch  = 0;
        quota_ledger_deref(ledger);
        return 0;
    }

    /* Release the slot against the EXACT epoch that claimed it. A stale duplicate
     * of this handle reaches this line with a superseded epoch, matches nothing,
     * and therefore cannot free a slot another charger has since claimed and is
     * about to fill -- which a bare "clear the flag" would do. */
    (void)atomic64_cmpxchg(&slot->owner, (int64_t)ob->epoch, 0);

    /* Empty the handle BEFORE dropping the reference: after the deref the ledger
     * may be gone, and a handle still naming it is a dangling pointer waiting for
     * a double return. */
    ob->ledger = (quota_ledger_t *)0;
    ob->slot   = 0;
    ob->token  = 0;
    ob->epoch  = 0;
    quota_ledger_deref(ledger);
    return 1;
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
