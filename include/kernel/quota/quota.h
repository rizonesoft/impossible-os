/* ============================================================================
 * quota.h -- Kernel resource-accounting type registry
 *
 * The single authority for the taxonomy of chargeable kernel resources. Every
 * resource that a quota block can charge (section 2) has exactly one entry
 * here with an accounting unit, a default limit, the privilege that may exceed
 * it, and a human-readable name. Later sections (charge API, object/handle,
 * pool, registry/ALPC/notification, CPU/IO, syscalls) consume this table; none
 * of them may invent a resource type outside it.
 *
 * Design invariants:
 *   - The descriptor table is `static const`, so every field is available at
 *     link time: before any consumer can charge. `quota_register_types()` only
 *     VALIDATES the table and marks the registry ready; it does not build it.
 *     Validation is separate from the dump (`quota_types_dump`).
 *   - `quota_register_types()` returns `boot_result_t`; a malformed table
 *     (missing name, bad unit, count mismatch) returns BOOT_FATAL so boot halts
 *     rather than running with an inconsistent taxonomy.
 *   - Storage/volume-quota bytes are intentionally NOT a type here: they are
 *     provider-owned (IXFS per-volume, keyed by (volume, owner)), not a scalar
 *     central type. See the IXFS volume-quota provider for storage quota.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/atomic.h"           /* atomic_t, atomic64_t */
#include "kernel/boot_init.h"        /* boot_result_t */
#include "kernel/nt/ntstatus.h"      /* NTSTATUS */
#include "kernel/sched/spinlock.h"   /* spinlock_t */
#include "kernel/security/luid.h"    /* LUID */
#include "kernel/security/sid.h"     /* SID, SID_MAX_SIZE */

/* The chain-charge API is expressed in terms of a task without pulling in the
 * scheduler's headers: task.h reaches into most of the kernel, and every
 * charging subsystem would inherit that. */
struct task;
struct access_token;

/* --- Resource types ------------------------------------------------------ *
 * Index into the descriptor table. APPEND new types immediately before
 * QUOTA_RESOURCE_TYPE_COUNT; never renumber (later sections key off the
 * enum value). */
typedef enum {
    QUOTA_RES_HANDLE = 0,        /* open handle-table entries        */
    QUOTA_RES_OBJECT_BODY,       /* Object Manager object bodies     */
    QUOTA_RES_NAMESPACE_ENTRY,   /* named object-directory entries   */
    QUOTA_RES_PAGED_POOL,        /* paged pool bytes                 */
    QUOTA_RES_NONPAGED_POOL,     /* nonpaged pool bytes              */
    QUOTA_RES_REGISTRY_BYTES,    /* registry key/value/data bytes    */
    QUOTA_RES_ALPC_MESSAGE,      /* queued ALPC messages             */
    QUOTA_RES_NOTIFICATION_STATE,/* notification state objects       */
    QUOTA_RES_TIMER,             /* timer objects                    */
    QUOTA_RES_THREAD,            /* threads                          */
    QUOTA_RES_PROCESS,           /* processes                        */
    QUOTA_RES_SECTION,           /* section (shared-memory) objects  */
    QUOTA_RES_MAPPED_VIEW,       /* mapped views of sections         */
    QUOTA_RES_CRASH_BUFFER,      /* retained crash-dump buffers      */
    QUOTA_RESOURCE_TYPE_COUNT
} quota_resource_type_t;

/* --- Accounting unit ----------------------------------------------------- */
typedef enum {
    QUOTA_UNIT_COUNT = 0,  /* discrete objects (handles, threads, timers, ...) */
    QUOTA_UNIT_BYTES       /* byte-denominated (pool, registry, payloads)      */
} quota_unit_t;

/* A default_limit of 0 means "no cap" (unlimited).
 *
 * What the charge API enforces TODAY is exactly one value: the per-block limit,
 * seeded from this default_limit at create time and thereafter changed only by
 * quota_set_limit. There is no kernel-config override layer yet, so a caller
 * that never calls quota_set_limit gets an UNLIMITED block -- do not rely on
 * this table to impose a cap. Concrete numeric caps remain a kernel-config
 * policy decision; when that layer ships, the intended precedence is
 * per-block explicit > kernel-config override > default_limit, and this
 * comment must move from "intended" to "enforced" in the same change. */
#define QUOTA_LIMIT_UNLIMITED  0ULL

/* --- Descriptor ---------------------------------------------------------- *
 * Immutable per-type metadata. `name` is never NULL. */
typedef struct {
    const char   *name;               /* human-readable, never NULL           */
    LUID          override_privilege; /* privilege that may exceed the cap     */
    uint64_t      default_limit;      /* QUOTA_LIMIT_UNLIMITED (0) = no cap     */
    quota_unit_t  unit;
} quota_resource_desc_t;

/* --- Registry API -------------------------------------------------------- */

/* Validate the static descriptor table and mark the registry ready. Returns
 * BOOT_FATAL on a malformed table so boot halts. Call in Phase 2 BEFORE the
 * first quota consumer (Object Manager). Idempotent: a second call re-validates
 * and returns the same result. */
boot_result_t quota_register_types(void);

/* Descriptor for a type, or NULL if `type` is out of range. The table is const,
 * so reads are lock-free and SMP-safe once quota_register_types has run. */
const quota_resource_desc_t *quota_resource_desc(quota_resource_type_t type);

/* Human-readable name for a type, or "?" if out of range. */
const char *quota_resource_type_name(quota_resource_type_t type);

/* Number of registered resource types (== QUOTA_RESOURCE_TYPE_COUNT). */
uint32_t quota_resource_type_count(void);

/* Non-zero once quota_register_types has validated the table successfully. */
int quota_registry_ready(void);

/* Dump the registered type table (name / unit / limit) to the serial log.
 * Distinct from the per-block charge-state dump quota_dump() added by the
 * quota dashboard. */
void quota_types_dump(void);

/* ========================================================================== *
 * Quota blocks and the charge API
 *
 * A quota block is one accounting principal: the unit that owns usage, peaks,
 * limits, and failure counts across every resource type. Tokens, processes,
 * and jobs point at blocks; the ownership model that wires them is section 3.
 *
 * Counter domain (HARD invariant):
 *   Every stored counter is in 0 .. QUOTA_AMOUNT_MAX. The underlying storage
 *   is atomic64_t (signed int64_t), so the API refuses any amount or limit
 *   above QUOTA_AMOUNT_MAX and guards every add against overflow BEFORE it is
 *   performed -- a signed overflow here would be undefined behavior and could
 *   wrap a counter below its limit. Charges fail closed if a counter is ever
 *   observed negative (corruption), rather than continuing to account against
 *   a nonsense value.
 *
 * Concurrency:
 *   Every MUTATION (charge, return, transfer, set_limit) runs under the
 *   block's `lock`, taken with spin_lock_irqsave so the API is safe at
 *   DISPATCH_LEVEL and from interrupt context. Holding the lock is what makes
 *   an operation's several counter updates one indivisible step -- limit check
 *   and commit, or debit and credit -- which per-counter atomics cannot do:
 *   a lock-free version of this API admitted charges against a limit that had
 *   already been lowered and let a transfer racing a return manufacture usage.
 *   The critical sections are a handful of arithmetic operations with no
 *   allocation, logging on the normal path, or nested calls.
 *
 *   QUERIES (usage/peak/failures/limit) take no lock: each counter is an
 *   atomic load, so a reader never sees a torn value, though two counters read
 *   in succession may straddle a concurrent update. Diagnostics and dashboards
 *   must not assume a cross-counter or cross-block snapshot is consistent.
 *
 *   High-frequency charging (pool allocation) should batch per-CPU rather than
 *   contend one block's lock; that batching belongs to the pool integration
 *   section, not here.
 *
 * Failure semantics (uniform across every mutation):
 *   Every refusal bumps that type's saturating failure counter -- an invalid
 *   amount, a debit underflow, a limit rejection, an
 *   arithmetic overflow, or a corrupted counter. Statuses are uniform too:
 *   STATUS_QUOTA_EXCEEDED means "would exceed a cap / not enough charged to
 *   move", STATUS_INTEGER_OVERFLOW means "the counter domain or its integrity
 *   was violated", STATUS_INVALID_PARAMETER means "the arguments were wrong".
 *   The same logical condition never reports two different codes.
 *
 * Limit-lowering contract:
 *   Lowering a limit never revokes an accepted charge -- usage may sit above
 *   the limit right after a lowering, and such a block simply refuses the next
 *   charge until usage drops back under it. Because set_limit and charge share
 *   the block lock, there is no window in which a charge is admitted against a
 *   limit that a caller had already lowered: a charge is judged against either
 *   the old limit or the new one, never a stale copy of a limit it should have
 *   observed.
 *
 * Lifetime (HARD contract):
 *   The caller MUST hold a live reference to the block across the whole of
 *   EVERY function here that takes a quota_block_t pointer -- not just the
 *   mutations. quota_set_limit takes the lock, and even the lock-free queries
 *   and quota_block_owner dereference the block, so a concurrent last-deref
 *   would free it underneath any of them. quota_try_transfer needs a live
 *   reference to BOTH blocks for its whole duration.
 *
 *   The API deliberately does NOT take a reference internally: an increment
 *   inside the call cannot save a pointer that teardown already freed. Acquire
 *   a reference while the owning token/process/job still publishes the pointer
 *   under its own lock (that safe publication is section 3's ownership model),
 *   then call in.
 *
 * No nested calls (HARD contract):
 *   Never enter any function here while a quota block's lock is held, and do
 *   not call in while holding a plain (non-irqsave) spinlock: spin_lock() masks
 *   interrupts without raising the tracked IRQL, so the PASSIVE_LEVEL gate that
 *   keeps diagnostics off the blocking log path cannot see that context.
 *   Only
 *   quota.c takes those locks, and quota_try_transfer is the only path that
 *   holds two -- which it orders by address. A caller that could hold one
 *   block's lock and then charge another would defeat that ordering and
 *   deadlock two CPUs with interrupts disabled. The struct is opaque
 *   specifically so this cannot be done from outside quota.c.
 * ========================================================================== */

/* Largest amount, usage, peak, or explicit limit the API accepts or stores. */
#define QUOTA_AMOUNT_MAX  0x7FFFFFFFFFFFFFFFLL   /* INT64_MAX */

/* One accounting principal. OPAQUE BY CONSTRUCTION: the definition lives in
 * quota.c, so no other translation unit can read a counter directly or, more
 * importantly, acquire the block's lock. That is an enforcement mechanism
 * rather than a style preference -- the no-nested-call rule below cannot be
 * enforced at all if callers can reach the lock themselves. */
typedef struct quota_block quota_block_t;

/* Which KIND of principal a block accounts for. A single charge is recorded
 * against one block of EACH kind along the owner chain (the process, its user
 * block, then its job), so the same resource legitimately appears in several
 * blocks at once. Any aggregate that sums blocks must therefore select ONE
 * kind, or it multiplies every charge by the chain depth -- which is exactly
 * why quota_rollup_by_sid reads USER blocks only. */
typedef enum quota_principal {
    QUOTA_PRINCIPAL_PROCESS = 0,   /* one per live process (task slot)        */
    QUOTA_PRINCIPAL_USER    = 1,   /* one per owner SID, shared by its tokens */
    QUOTA_PRINCIPAL_JOB     = 2,   /* one per Job Object                      */
    QUOTA_PRINCIPAL_COUNT
} quota_principal_t;

/* Allocate a block with every counter zeroed and every limit seeded from the
 * type registry's default_limit. `owner` may be NULL (no owner SID recorded);
 * a non-NULL owner is COPIED into the block, so the caller keeps ownership of
 * its own SID storage.
 *
 * `principal` records which accounting layer the block belongs to. It is fixed
 * for the block's life and is what lets an aggregate pick a single layer. A
 * USER block should be obtained from quota_user_block_acquire rather than
 * created here: minting a second USER block for a SID that already has one
 * splits that user's budget into two independently enforced halves, which is
 * not an aggregate at all.
 *
 * `owner_len` is the number of bytes the caller GUARANTEES are readable at
 * `owner` (ignored when owner is NULL). It is required rather than assumed:
 * validating against SID_MAX_SIZE would accept a 15-subauthority header
 * sitting at the end of a mapping and then read 68 bytes across the boundary.
 *
 * The caller must also keep the SID STABLE for the duration of the call. Only
 * `owner_len` bytes are ever read and the capture is re-validated afterwards,
 * so a mutation cannot cause an out-of-bounds read and a structural change is
 * rejected -- but a same-length change to the authority or a sub-authority
 * cannot be detected, and would be captured as the block's owner. Callers hold
 * the token (or its lock) across this call; do not pass a shared mutable SID.
 *
 * Returns NULL on allocation failure, or when the SID is malformed, truncated
 * by owner_len, or structurally inconsistent after capture. The returned block
 * carries one reference, owned by the caller. */
quota_block_t *quota_block_create(quota_principal_t principal,
                                  const SID *owner, uint32_t owner_len);

/* Acquire an additional reference. The caller must already hold one. */
void quota_block_ref(quota_block_t *block);

/* Acquire a reference ONLY if the block is still live (refcount > 0), for a
 * finder that reached the block through the global registry rather than
 * through an owner that holds a reference. Returns 1 on success, 0 if the
 * block is already being torn down.
 *
 * quota_block_ref cannot serve that case: it increments unconditionally, so a
 * finder racing the final deref would lift the count 0 -> 1 on a block whose
 * teardown is already committed, and the teardown would then free memory the
 * finder still holds. The registry lock alone does not close that window
 * either -- the count reaching zero, not the unlink, is what decides the
 * block's fate. Callers must hold the registry lock across this call (which
 * quota.c's own lookups do); the deref path unlinks under the same lock before
 * freeing, so a block observed in the registry is never freed underneath a
 * try-ref that succeeded. */
int quota_block_try_ref(quota_block_t *block);

/* Release a reference; frees the block when the last one goes away. */
void quota_block_deref(quota_block_t *block);

/* The block's owner SID, or NULL if none was recorded. Valid while the caller
 * holds a reference; the storage belongs to the block. */
const SID *quota_block_owner(const quota_block_t *block);

/* Which accounting layer this block belongs to. QUOTA_PRINCIPAL_COUNT for a
 * NULL block, so a caller filtering by kind never needs to pre-validate. */
quota_principal_t quota_block_principal(const quota_block_t *block);

/* The ONE canonical USER block for `owner`, creating it on first use.
 *
 * Every token for a SID must reach the SAME block or the "per-user budget" is
 * not enforced at all: a second, independently created token lineage would get
 * its own full budget. Lookup and create are atomic with respect to each other,
 * so two CPUs racing on a first-ever SID still end up sharing one block.
 *
 * Returns a block carrying one reference owned by the caller (balance with
 * quota_block_deref), or NULL when the SID is malformed/truncated by owner_len
 * or allocation fails. `owner` must be non-NULL: a user block is defined by its
 * SID, so an ownerless one could not be found again and would silently become
 * a private budget. Same SID-stability requirement as quota_block_create. */
quota_block_t *quota_user_block_acquire(const SID *owner, uint32_t owner_len);

/* Charge `amount` of `type` against `block`.
 *   STATUS_SUCCESS            charge committed (amount 0 is a no-op success)
 *   STATUS_INVALID_PARAMETER  NULL block, bad type, or amount > QUOTA_AMOUNT_MAX
 *   STATUS_INTEGER_OVERFLOW   usage + amount would leave the counter domain
 *   STATUS_QUOTA_EXCEEDED     would exceed the limit (usage left unchanged)
 * On any failure the usage counter is exactly as it was, and the per-type
 * failure counter is bumped (saturating) for QUOTA_EXCEEDED/OVERFLOW. */
NTSTATUS quota_charge(quota_block_t *block, quota_resource_type_t type, uint64_t amount);

/* Return `amount` of `type` previously charged.
 *
 * Returning MORE than is currently charged fails closed: usage is left exactly
 * as it was and STATUS_INTEGER_OVERFLOW is returned. It does not clamp to zero.
 * A caller returning more than it holds is working from stale bookkeeping, and
 * zeroing would erase OTHER live charges -- a duplicate cleanup for a freed
 * resource would silently wipe the accounting for everything allocated since.
 * Refusing keeps the counter honest and makes the double-return visible.
 *
 * Returns STATUS_INVALID_PARAMETER on a bad argument, STATUS_INTEGER_OVERFLOW
 * on an over-return or a corrupted counter, STATUS_SUCCESS otherwise. */
NTSTATUS quota_return(quota_block_t *block, quota_resource_type_t type, uint64_t amount);

/* Move `amount` of `type` from `src` to `dst`.
 *
 * Both block locks are held for the whole move, acquired in block-address
 * order so opposite-direction transfers between the same pair cannot deadlock.
 * The transfer is therefore all-or-nothing and conserving WITH RESPECT TO
 * OTHER MUTATIONS: no concurrent charge, return, or transfer can observe or
 * consume a half-completed state, so usage is never duplicated or lost.
 *
 * That guarantee does NOT extend to the lock-free queries. A reader calling
 * quota_usage on dst and then on src can straddle the update and count the
 * amount twice (or, in the other order, not at all). Cross-block totals must
 * therefore not drive an irreversible decision; take the mutation path, or
 * tolerate the skew, if an exact aggregate matters.
 *
 * On failure NO usage moves and neither block's usage, peak, or limit changes;
 * the failing block's failure COUNTER does advance (that is the point of the
 * telemetry), so "unchanged" means the accounting state, not every byte.
 * Fails with STATUS_INVALID_PARAMETER on a bad argument, STATUS_QUOTA_EXCEEDED
 * if src holds less than `amount` or dst is at its cap, or
 * STATUS_INTEGER_OVERFLOW if dst cannot represent the total or either block's
 * counter is corrupt. `src` == `dst` is a no-op success. */
NTSTATUS quota_try_transfer(quota_block_t *src, quota_block_t *dst,
                            quota_resource_type_t type, uint64_t amount);

/* --- Queries and limit control ------------------------------------------- *
 * All four return 0 for a NULL block or an out-of-range type, so a diagnostic
 * caller never needs to pre-validate. */
/* Count of diagnostics that were recorded but NOT emitted because the calling
 * context was above PASSIVE_LEVEL (klog's live-debug path does blocking disk
 * I/O, which is unsafe there). Non-zero means accounting anomalies occurred
 * whose log lines were suppressed. */
uint64_t quota_diag_deferred_count(void);

/* Count of job un-absorb returns that were REFUSED, i.e. the job's counter no
 * longer held what a membership's absorb record folded in. Non-zero means the
 * job accounting has drifted. Counted rather than logged because the un-absorb
 * runs from the log-free, elevated-IRQL process-death teardown path. */
void     quota_note_unabsorb_refused(void);
uint64_t quota_unabsorb_refused_count(void);

uint64_t quota_usage(const quota_block_t *block, quota_resource_type_t type);
uint64_t quota_peak(const quota_block_t *block, quota_resource_type_t type);
uint64_t quota_failures(const quota_block_t *block, quota_resource_type_t type);
uint64_t quota_limit(const quota_block_t *block, quota_resource_type_t type);

/* Set an explicit limit (QUOTA_LIMIT_UNLIMITED removes the cap). Rejects a
 * limit above QUOTA_AMOUNT_MAX with STATUS_INVALID_PARAMETER. Lowering below
 * current usage is allowed and does not revoke committed charges; see the
 * limit-lowering contract above. */
NTSTATUS quota_set_limit(quota_block_t *block, quota_resource_type_t type, uint64_t limit);

/* Sum the usage and peak recorded for `type` across every live block owned by
 * `owner`, counting the USER layer only so a chain charge is not multiplied by
 * its depth. With one canonical block per SID this reads that block; the loop
 * exists so a SID whose canonical block was replaced mid-flight still reports a
 * total rather than a gap. Either output pointer may be NULL.
 *
 * This is a DIAGNOSTIC aggregate, not an enforcement input: the counters are
 * read without any block lock, so a total can straddle a concurrent charge.
 * Enforcement happens per block, inside the charge path.
 *
 * Returns STATUS_INVALID_PARAMETER for a NULL/malformed owner or bad type. */
NTSTATUS quota_rollup_by_sid(const SID *owner, uint32_t owner_len,
                             quota_resource_type_t type,
                             uint64_t *usage_out, uint64_t *peak_out);

/* ========================================================================== *
 * Chain charging
 *
 * A resource is not owned by one principal: it is owned by the process, by the
 * user behind the process's token, and by every job the process belongs to. A
 * charge must therefore be admitted by ALL of them or by none, and the matching
 * return must credit back EXACTLY the blocks that were charged.
 *
 * Recomputing the chain at return time cannot do that. The chain is mutable
 * while a charge is outstanding -- a process is assigned to a job, or dies and
 * detaches from one (task_death_teardown drops job membership while other
 * resources are released later at reap) -- so a recomputed chain can be missing
 * a block that was charged, permanently stranding usage in it. The receipt
 * below is the fix: it records the exact block set, holds a reference on each,
 * and is the ONLY thing consulted on the way back.
 *
 * The receipt is opaque in practice (callers only pass it back), but its
 * storage is public so it can live on the caller's stack or be embedded next
 * to the resource it accounts for -- charge paths must not have to allocate.
 * ========================================================================== */

/* Chain depth ceiling: process + user + the job chain. Sized with headroom for
 * nested jobs, which are not implemented yet (see quota_charge_chain). A chain
 * that would exceed this is refused rather than silently truncated -- an
 * untracked link means an unenforced limit. */
#define QUOTA_CHAIN_MAX  8u

/* Proof of a completed chain charge. Treat every field as private: build it
 * only with quota_charge_chain, consume it only with quota_return_chain.
 * Zero-initialized (`= {0}`) is a valid empty receipt that returns nothing.
 *
 * `state` makes ownership EXCLUSIVE rather than merely documented. A receipt
 * embedded next to the resource it accounts for is reachable from more than
 * one CPU, so two cleanup paths can race on it: without the state word both
 * would see a non-empty receipt, both would credit the amount back, and the
 * second credit would erase charges made since -- while dereferencing the same
 * references twice. Charging into a receipt that already holds a live charge
 * is refused for the same reason: it would silently drop the references to the
 * blocks the first charge is still holding and strand that usage forever. */
typedef struct quota_charge_receipt {
    quota_block_t        *blocks[QUOTA_CHAIN_MAX]; /* charged set, one ref each */
    uint32_t              count;                   /* live entries in blocks[]  */
    quota_resource_type_t type;                    /* what was charged          */
    uint64_t              amount;                  /* how much, per block       */
    atomic_t              state;                   /* QUOTA_RECEIPT_* below     */
} quota_charge_receipt_t;

/* Receipt states. IDLE is 0 so a zero-initialized receipt is valid. */
#define QUOTA_RECEIPT_IDLE       0   /* holds nothing; may be charged into  */
#define QUOTA_RECEIPT_ACTIVE     1   /* holds a live charge; may be returned */
#define QUOTA_RECEIPT_BUSY       2   /* a charge or return owns it right now */

/* SUBSTITUTE the client (thread impersonation) user block for the process's own
 * user block, for the rare API whose contract bills the client rather than the
 * server. The process and job blocks would still be charged -- the resource is
 * held by the server process either way -- but the per-user aggregate would
 * land on the client.
 *
 * CURRENTLY REFUSED: quota_charge_chain returns STATUS_NOT_SUPPORTED for this
 * flag. Billing the client means reading the executing thread's impersonation
 * token, and neither prerequisite is SMP-safe yet: thread_current() resolves
 * through process-global scheduler cursors, so it can sample a SIBLING thread
 * of the same task, and the token slot has no teardown-safe pin, so a
 * concurrent RevertToSelf can free the token between the load and the
 * reference. Either defect bills the WRONG user, which is worse than refusing
 * -- so the flag fails closed instead of approximating.
 *
 * The flag and this contract stay defined so callers can be written against the
 * final shape. Owned by the security reference monitor's token pin and the
 * per-CPU current-thread cursor.
 *
 * Default (flag clear) is correct for essentially every caller anyway: a server
 * impersonating a client uses the client's identity for ACCESS checks, but the
 * memory and handles it allocates belong to the SERVER, so billing the client
 * by default would let any client drain a stranger's budget for resources it
 * cannot even reach. */
#define QUOTA_CHARGE_CLIENT  0x1u

/* Charge `amount` of `type` against every principal owning `task`, all or
 * nothing, and record the result in `receipt`.
 *
 * The chain is snapshotted first (taking a reference on each block under the
 * owner's own lock), and only then charged -- the no-nested-call contract above
 * forbids charging while holding job_lock or a job's lock. Blocks are charged
 * one at a time, each under its own lock, so no two block locks are ever held
 * at once and no ordering hazard is introduced. If any block refuses, the
 * already-charged prefix is returned exactly (quota_return ignores limits, so a
 * concurrently lowered limit cannot block the unwind) and the failing status is
 * reported with no net usage anywhere.
 *
 * A charge admitted here is judged against the chain as it existed at snapshot
 * time. A job assigned to the process immediately afterwards does not
 * retroactively capture that charge; making live assignment migrate outstanding
 * usage needs a subsystem that actually charges, and is owned by section 4.
 *
 * Nested jobs do not exist yet (a task has at most one job), so the chain today
 * is process + user + at most one job. The receipt and this signature are
 * already chain-shaped, so adding ancestor jobs later changes only the snapshot
 * walk inside quota.c -- no caller changes.
 *
 * The task's own process block MUST be present or the charge is refused with
 * STATUS_PROCESS_IS_TERMINATING. An absent block means the task is dead (its
 * teardown already cleared it) or not yet fully created; admitting an empty
 * chain there would return success having charged nothing, letting a dying
 * process allocate entirely unaccounted.
 *
 * `flags` is 0 or QUOTA_CHARGE_CLIENT. On failure THIS call leaves the receipt
 * holding nothing of its own, so an unconditional quota_return_chain on the
 * error path is safe. The one exception is STATUS_INVALID_PARAMETER raised
 * because the receipt was ALREADY holding a live charge: that charge is
 * untouched and still belongs to whoever made it, so a caller must not treat
 * that status as licence to return it.
 * Returns STATUS_INVALID_PARAMETER (bad argument, a chain deeper than
 * QUOTA_CHAIN_MAX, or a receipt that already holds a live charge),
 * STATUS_NOT_SUPPORTED (QUOTA_CHARGE_CLIENT -- see the flag),
 * STATUS_PROCESS_IS_TERMINATING (no process block), or whatever the refusing
 * block returned. */
NTSTATUS quota_charge_chain(struct task *task, quota_resource_type_t type,
                            uint64_t amount, uint32_t flags,
                            quota_charge_receipt_t *receipt);

/* Return a charge recorded by quota_charge_chain and release the receipt's
 * references. Exactly one caller performs the return even if several race; the
 * losers are no-ops, so a double return cannot erase a newer live charge. Safe
 * on an empty or zero-initialized receipt.
 *
 * Returning a receipt while the charge that fills it is still in flight on
 * another CPU is a CALLER ORDERING ERROR, not a race this can resolve: the
 * return finds the receipt still being built, does nothing, and the charge
 * stands. There is no correct alternative -- crediting back a charge that has
 * not finished would corrupt the counters. Complete the charge first.
 *
 * SCOPE OF THAT GUARANTEE: it covers repeated returns of the SAME charge. It
 * does NOT make receipt STORAGE reuse safe -- if a caller returns a charge,
 * reuses the same receipt for a new charge, and only then a stale returner from
 * the first charge arrives, that returner wins the state claim and returns the
 * SECOND charge. Distinguishing them needs a caller-held generation token, and
 * no charging subsystem exists yet to hold one. Until then the rule for callers
 * is the simple one: a receipt belongs to exactly one resource for its lifetime
 * and is not recycled while any cleanup path for the old charge can still run.
 * -> the generation-token upgrade is owned by section 4 (first real consumer).
 */
void quota_return_chain(quota_charge_receipt_t *receipt);

/* --- Owner wiring (quota_owner.c) ---------------------------------------- *
 * Attach and detach the per-process block. Both are idempotent; teardown is
 * called from every process-death path. */

/* Create this task's QUOTA_PRINCIPAL_PROCESS block and attach the canonical
 * USER block for `token`'s owner SID, publishing both under the task's quota
 * lock.
 *
 * `token` is passed in rather than read from the task because creation wires
 * quota before the token slot is published (it belongs in the same early
 * unwind window as job inheritance, where a failure is still cheap to roll
 * back). It may be NULL -- pre-SRM kernel threads have no token yet and get an
 * ownerless process block, which is fully chargeable; only the per-user
 * aggregate needs a SID.
 *
 * The caller must keep `token` alive across the call (creation paths hold the
 * duplicate's reference). Returns STATUS_SUCCESS, or
 * STATUS_INSUFFICIENT_RESOURCES if allocation failed; a task that already has
 * a block is left alone and reports success. */
NTSTATUS quota_task_init(struct task *task, struct access_token *token);

/* Clear and release this task's process block. Log-free and lock-safe for the
 * death-teardown path. */
void quota_task_teardown(struct task *task);

/* Exactly what an absorb folded into a job, so it can be undone byte-for-byte
 * if the assignment it was preparing for is then refused. Re-reading the
 * process block at unwind time would NOT do: its usage may have moved since,
 * and returning the new value would credit back an amount that was never
 * charged. Zero-initialize before use. */
typedef struct quota_absorb_record {
    uint64_t taken[QUOTA_RESOURCE_TYPE_COUNT];
    uint32_t active;                  /* non-zero once something was folded in */
} quota_absorb_record_t;

/* Fold a joining task's CURRENT usage into `job_block` before its membership
 * is published, so the job's aggregate limit covers what the process already
 * holds. A process that allocated first and joined a capped job afterwards
 * would otherwise carry that usage past the cap entirely uncounted.
 *
 * All-or-nothing: if any resource type would exceed the job's limit, everything
 * folded in so far is returned and the status is reported so the CALLER can
 * refuse the assignment. On success `rec` records what was folded in; the
 * caller MUST pass it to quota_job_unabsorb if the assignment then fails for
 * any other reason. Must be called with NO quota block lock, job lock, or task
 * job_lock held (the no-nested-call contract above).
 *
 * The absorb deliberately precedes publication: a charge landing in the window
 * between them misses the job (a bounded under-count that unwinds symmetrically
 * because its receipt has no job block), whereas absorbing after publication
 * would count such a charge TWICE and permanently inflate the job. Closing the
 * window entirely is owned by section 4's live-assignment item. Returns
 * STATUS_SUCCESS when there is nothing to absorb.
 *
 * KNOWN LIMITATION -- absorbed usage is billed to the job until the member
 * DEPARTS, not until the underlying resource is freed. The receipts for
 * pre-join resources name only the process and user blocks, so returning one
 * while still a member reduces those two and leaves the job's copy standing;
 * the absorb record only unwinds at detach. A long-lived member can therefore
 * hold job headroom for resources it has already released. Fixing this needs
 * outstanding receipts to be MIGRATED into the job at assignment, which needs a
 * per-task registry of live receipts -- there is no charging subsystem yet to
 * have any. Owned by section 4 (receipt-obligation migration); today no
 * subsystem charges, so no member can hold a pre-join receipt at all. */
NTSTATUS quota_job_absorb_task(struct quota_block *job_block, struct task *task,
                               quota_absorb_record_t *rec);

/* Give back exactly what an absorb folded in. Used for BOTH halves of the
 * membership lifetime: to undo a refused assignment, and -- with the record
 * that was stored alongside the membership -- to withdraw the absorbed amount
 * when the member departs. No-op on an empty record.
 *
 * ONLY the absorbed amount may be withdrawn this way. A member's post-join
 * charges reached the job through chain receipts and belong to those receipts;
 * withdrawing the member's CURRENT usage instead would return those charges a
 * second time, and once another member's usage covers the difference the
 * receipt's own later return would subtract from THAT member's live charge. */
void quota_job_unabsorb(struct quota_block *job_block, quota_absorb_record_t *rec);

#ifdef KERNEL_TESTS
/* --- Test-only raw counter access ---------------------------------------- *
 * Corrupted-state and saturation fixtures cannot be built through the public
 * API (it exists precisely to keep counters in their domain). These helpers
 * write the counters directly, bypassing every guard on purpose. Same
 * test-only pattern as kmalloc_fail_next(); compiled out in production. */

/* Raw counter writes for corrupted-state fixtures (negative usage, saturated
 * failure counters). */
void quota_test_poke_usage(quota_block_t *block, quota_resource_type_t type, int64_t value);
void quota_test_poke_failures(quota_block_t *block, quota_resource_type_t type, int64_t value);

/* Raw counter read that does NOT clamp negatives, so a test can prove a
 * corrupted counter was left untouched rather than quietly normalized. */
int64_t quota_test_raw_usage(const quota_block_t *block, quota_resource_type_t type);
#endif /* KERNEL_TESTS */
