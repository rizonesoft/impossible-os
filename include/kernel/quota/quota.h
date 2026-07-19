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

/* A default_limit of 0 means "no cap" (unlimited). Concrete numeric caps are a
 * policy decision owned by the kernel configuration layer; the charge-time
 * limit precedence (documented contract, enforced by the charge API) is:
 *   per-block explicit limit  >  kernel-config override  >  default_limit. */
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
 *   Never enter any function here while a quota block's lock is held. Only
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

/* Allocate a block with every counter zeroed and every limit seeded from the
 * type registry's default_limit. `owner` may be NULL (no owner SID recorded);
 * a non-NULL owner is COPIED into the block, so the caller keeps ownership of
 * its own SID storage.
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
quota_block_t *quota_block_create(const SID *owner, uint32_t owner_len);

/* Acquire an additional reference. The caller must already hold one. */
void quota_block_ref(quota_block_t *block);

/* Release a reference; frees the block when the last one goes away. */
void quota_block_deref(quota_block_t *block);

/* The block's owner SID, or NULL if none was recorded. Valid while the caller
 * holds a reference; the storage belongs to the block. */
const SID *quota_block_owner(const quota_block_t *block);

/* Charge `amount` of `type` against `block`.
 *   STATUS_SUCCESS            charge committed (amount 0 is a no-op success)
 *   STATUS_INVALID_PARAMETER  NULL block, bad type, or amount > QUOTA_AMOUNT_MAX
 *   STATUS_INTEGER_OVERFLOW   usage + amount would leave the counter domain
 *   STATUS_QUOTA_EXCEEDED     would exceed the limit (usage left unchanged)
 * On any failure the usage counter is exactly as it was, and the per-type
 * failure counter is bumped (saturating) for QUOTA_EXCEEDED/OVERFLOW. */
NTSTATUS quota_charge(quota_block_t *block, quota_resource_type_t type, uint64_t amount);

/* Return `amount` of `type` previously charged. Returning more than is charged
 * is an accounting bug: the counter clamps at 0 and a warning is logged rather
 * than wrapping negative. Returns STATUS_INVALID_PARAMETER on a bad argument,
 * STATUS_SUCCESS otherwise (including the clamped case). */
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
 * Fails (leaving BOTH blocks as it found them) with STATUS_INVALID_PARAMETER on
 * a bad argument, STATUS_QUOTA_EXCEEDED if src holds less than `amount` or dst
 * is at its cap, or STATUS_INTEGER_OVERFLOW if dst cannot represent the total.
 * `src` == `dst` is a no-op success. */
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

uint64_t quota_usage(const quota_block_t *block, quota_resource_type_t type);
uint64_t quota_peak(const quota_block_t *block, quota_resource_type_t type);
uint64_t quota_failures(const quota_block_t *block, quota_resource_type_t type);
uint64_t quota_limit(const quota_block_t *block, quota_resource_type_t type);

/* Set an explicit limit (QUOTA_LIMIT_UNLIMITED removes the cap). Rejects a
 * limit above QUOTA_AMOUNT_MAX with STATUS_INVALID_PARAMETER. Lowering below
 * current usage is allowed and does not revoke committed charges; see the
 * limit-lowering contract above. */
NTSTATUS quota_set_limit(quota_block_t *block, quota_resource_type_t type, uint64_t limit);

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
