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
    /* One scalar per principal covering bodies of EVERY object type: the enum
     * value is the accounting identity, so this space has no OBJECT_TYPE
     * dimension and cannot answer "how many Events are live". That question is
     * owned by OBJECT_TYPE.total_objects (include/kernel/ob/ob_type.h), which
     * carries no owner dimension in return. Both halves of that boundary are
     * deliberate; see the counter-authority contract in ob_type.h. */
    QUOTA_RES_OBJECT_BODY,       /* Object Manager object bodies     */
    QUOTA_RES_NAMESPACE_ENTRY,   /* named object-directory entries   */
    QUOTA_RES_PAGED_POOL,        /* paged pool bytes                 */
    QUOTA_RES_NONPAGED_POOL,     /* nonpaged pool bytes              */
    QUOTA_RES_REGISTRY_BYTES,    /* registry key/value/data bytes    */
    QUOTA_RES_ALPC_MESSAGE,      /* queued ALPC messages             */
    QUOTA_RES_NOTIFICATION_STATE = 7, /* notification state objects  */
    QUOTA_RES_TIMER              = 8, /* timer objects               */
    QUOTA_RES_THREAD             = 9, /* threads                     */
    QUOTA_RES_PROCESS            = 10,/* processes                   */
    QUOTA_RES_SECTION            = 11,/* section (shared-memory) objects */
    QUOTA_RES_MAPPED_VIEW        = 12,/* mapped views of sections    */
    QUOTA_RES_CRASH_BUFFER       = 13,/* retained crash-dump buffers */
    /* Appended by section 6. New types go HERE, at the END. Adding one beside
     * a related type instead shifts every value after it, and the enum value
     * IS the identity (counter index, dump rows, the later query ABI). */
    QUOTA_RES_NOTIFICATION_SUB   = 14,/* notification subscriptions  */
    QUOTA_RES_NOTIFICATION_BYTES = 15,/* retained notification payload */
    /* Appending past 16 is a HARD build failure, not a silent widening: the
     * job-quota wire class freezes its row array at JOB_QUOTA_V1_RESOURCE_COUNT
     * (include/kernel/ob/ob_job.h) and static-asserts this count fits. A 17th
     * resource type needs a V2 information class first. */
    QUOTA_RESOURCE_TYPE_COUNT    = 16
} quota_resource_type_t;

/* Every stable value is pinned explicitly above AND re-asserted here, so a
 * future insertion in the middle is a COMPILE error rather than a silent
 * renumber that makes an existing record address a different counter. This
 * assert is the mechanism the "never renumber" rule needed to actually hold:
 * the prose alone did not stop section 6's first draft from inserting two
 * types in the middle of the enum. */
_Static_assert(QUOTA_RES_HANDLE == 0 && QUOTA_RES_OBJECT_BODY == 1 &&
               QUOTA_RES_NAMESPACE_ENTRY == 2 && QUOTA_RES_PAGED_POOL == 3 &&
               QUOTA_RES_NONPAGED_POOL == 4 && QUOTA_RES_REGISTRY_BYTES == 5 &&
               QUOTA_RES_ALPC_MESSAGE == 6 && QUOTA_RES_NOTIFICATION_STATE == 7 &&
               QUOTA_RES_TIMER == 8 && QUOTA_RES_THREAD == 9 &&
               QUOTA_RES_PROCESS == 10 && QUOTA_RES_SECTION == 11 &&
               QUOTA_RES_MAPPED_VIEW == 12 && QUOTA_RES_CRASH_BUFFER == 13 &&
               QUOTA_RES_NOTIFICATION_SUB == 14 &&
               QUOTA_RES_NOTIFICATION_BYTES == 15,
    "quota resource type IDs are ABI: append new types, never renumber");

/* --- Accounting unit ----------------------------------------------------- */
typedef enum {
    QUOTA_UNIT_COUNT = 0,  /* discrete objects (handles, threads, timers, ...) */
    QUOTA_UNIT_BYTES       /* byte-denominated (pool, registry, payloads)      */
} quota_unit_t;

/* A default_limit of 0 means "no cap" (unlimited).
 *
 * PRECEDENCE (ENFORCED, section 6). A block's limit for a type resolves as:
 *
 *   per-block explicit  >  kernel-config override  >  this default_limit
 *
 *   - "per-block explicit" is any limit set by quota_set_limit. It records a
 *     provenance flag, so a later config change never overwrites it.
 *   - "kernel-config override" applies to USER blocks ONLY (the tunables are
 *     a PER-USER policy; PROCESS and JOB blocks continue to seed straight from
 *     default_limit). It seeds a new USER block and is re-applied to live ones
 *     -- see quota_config_user_default / quota_user_default_relimit.
 *
 * WHAT THIS STILL DOES NOT GUARANTEE: every default_limit in the table is
 * currently UNLIMITED and no production code sets a quota.user.<type> tunable,
 * so a caller that never calls quota_set_limit still gets an UNLIMITED block.
 * The override MECHANISM is enforced; the POLICY that would make it bite is
 * not wired yet. Do not read this table as imposing a cap. */
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
 * Distinct from the per-block charge-state dump quota_dump() below. */
void quota_types_dump(void);

/* ========================================================================== *
 * Dashboards and the leak sweep
 *
 * WHAT IS ENUMERABLE. Only QUOTA_PRINCIPAL_USER blocks are reachable from a
 * global list (see the registry rationale in quota.c). A PROCESS block hangs
 * off task->quota and a JOB block off JOB_OBJECT.quota, and neither a
 * lifetime-safe task iterator nor a job registry exists today -- so every
 * facility here reports USER blocks and SAYS so, rather than pretending to a
 * whole-system view it cannot take. Reaching process blocks is owned by the
 * lifetime-safe task enumeration item in section 11.
 *
 * That restriction costs the leak sweep less than it looks: a chain charge
 * rolls its amount up into the owning USER block, so a process or job charge
 * that is never returned still surfaces here as USER-block usage.
 * ========================================================================== */

/* Monotonic count of registry LINK and UNLINK events, bumped under the
 * registry lock. A reader that samples this before and after a walk learns
 * whether list MEMBERSHIP moved underneath it; equal samples mean the walk saw
 * one generation. It deliberately says nothing about counter mutation, which
 * needs no such gate because every counter is read atomically. */
uint64_t quota_registry_generation(void);

/* Render live USER blocks (id, owner-SID digest, and every type whose usage,
 * peak, or failure count is non-zero) to the serial log.
 *
 * CALLING CONTRACT (HARD). PASSIVE_LEVEL, thread context, interrupts enabled,
 * holding NO lock -- not a quota block lock, not the registry lock, not any
 * other non-reentrant debug lock. Three reasons, each independently fatal:
 * the registry lock may be taken only while holding nothing, so entering with
 * a block lock held inverts the one absolute lock order in this module; the
 * serial output this emits must not run beneath a spinlock; and releasing the
 * walk's final pin can enter registry removal, which takes the registry lock
 * again. From panic, NMI, interrupt context, or inside a charge/return
 * critical section call quota_dump_crash() instead -- it is the bounded,
 * non-blocking form built for exactly those callers. */
void quota_dump(void);

/* Panic-path form. Attempts the registry lock ONCE without blocking: on
 * success it copies a hard-bounded set of rows into preallocated storage,
 * releases the lock, and only then emits them; on failure it emits a single
 * "registry unavailable" line and returns. Allocates nothing, and never waits
 * on a CPU that died holding the REGISTRY lock.
 *
 * It never traverses the list unlocked. A lock-free walk looks appealing on a
 * dead machine, but quota_block_deref unlinks and then immediately frees, so
 * an unlocked walker can follow a perfectly aligned pointer into freed or
 * reused memory -- and a second fault inside the panic path costs the whole
 * crash report, which is the one thing this function exists to produce.
 *
 * WHAT IT DOES NOT GUARANTEE, precisely. Output goes through klog, and
 * klog_emit takes the blocking s_klog_lock (src/kernel/klog.c), rasterizes to
 * the framebuffer when the level passes the screen filter, and appends AND
 * FLUSHES to disk when live disk logging is on -- blocking storage I/O from a
 * panicking CPU whose storage stack may be exactly what died. So this is
 * non-blocking with respect to QUOTA state, not with respect to the logger: a
 * panic that interrupted logging can still stall here, exactly as
 * kernel_subsystem_dump and transition_ring_dump_to_serial already can on the
 * same path. It emits up to one header plus a row per charged type per
 * reported block, all before the BSOD is painted. That hazard is repo-wide and predates this function; the
 * panic-safe raw emitter that fixes it for every panic-path dumper is owned by
 * TODO-27 crash-dump-generation section 7 ("dump_emit_raw"). Do NOT read this
 * contract as "safe to call with the logger lock held." */
void quota_dump_crash(void);

#ifdef KERNEL_TESTS
/* Times quota_dump_crash took its try-lock FAILURE branch. The fallback path
 * is the whole reason the function exists, and a test cannot otherwise tell a
 * successful dump from a skipped one -- both simply return. */
uint64_t quota_test_crash_fallbacks(void);

/* The dump/leak pin-batch bound. Exposed so a boundary test DERIVES its
 * fixture count from the implementation instead of hardcoding it: a test that
 * hardcodes "batch is 8, so use 9 blocks" stays green while silently no longer
 * crossing a boundary if the bound ever grows. */
uint32_t quota_test_dump_batch(void);

/* Hold / release the registry lock, so a single-CPU test can make the lock
 * genuinely unavailable to quota_dump_crash instead of asserting against an
 * uncontended path that would stay green through a blocking regression.
 * Strictly paired; the flags are the caller's, as with spin_lock_irqsave.
 * Nothing between the two may take a quota block lock -- that is the one
 * ordering this module forbids. */
void quota_test_registry_hold(uint64_t *flags);
void quota_test_registry_release(uint64_t flags);

/* Counter-mutation epoch: advances on every block-lock release, so it moves
 * for a charge, a return, and a transfer -- the mutations the registry
 * generation cannot see. Exposed so a test can assert the leak snapshot's
 * coherence gate actually reacts to them. */
uint64_t quota_test_mutation_epoch(void);

/* Writers currently inside a block critical section. Zero means no mutation is
 * in progress; the leak snapshot requires zero at BOTH ends of its walks,
 * because the epoch only witnesses mutations that already COMPLETED. */
uint32_t quota_test_writers_active(void);
#endif

/* One reading of outstanding USER-block quota, for the boot leak sweep. Both
 * signals are carried because neither implies the other: replacing a
 * zero-usage block with a charged one leaves `user_blocks` flat while `usage`
 * rises, and retaining a fresh canonical block moves `user_blocks` with no
 * usage change at all.
 *
 * `coherent` is 1 only when the reading is QUIESCENT, which is stronger than
 * "membership did not change" and deliberately so. Three independent checks
 * must all hold, because each catches what the others cannot:
 *
 *   1. The registry GENERATION catches a link or unlink.
 *   2. The counter-mutation EPOCH (KERNEL_TESTS builds) catches a charge,
 *      return, or transfer that COMPLETED. It is load-bearing and cannot be
 *      dropped in favour of check 4: a transfer whose source is read before
 *      the move and whose destination is read after it produces the SAME wrong
 *      total on both walks, so agreeing walks alone would certify it.
 *   3. The ACTIVE-WRITER count must be zero at both ends. The epoch witnesses
 *      only completed mutations, and quota_try_transfer credits the
 *      destination before it debits the source -- so a writer stalled between
 *      those two stores leaves the amount visible in BOTH blocks with the
 *      epoch unmoved on either side of the walk. Requiring no mutation to be
 *      IN PROGRESS is what rejects that total, which never existed.
 *   4. TWO independent walks must agree exactly.
 *
 * Checks 2 AND 3 are BOTH KERNEL_TESTS-only and BOTH read as constants
 * elsewhere, because the accounting behind them is test instrumentation and
 * the sweep it serves is test infrastructure. On a KERNEL_TESTS-off build only
 * checks 1 and 4 apply, so `coherent` there means "membership did not move and
 * two walks agreed" and NOT quiescence -- a transfer parked between its two
 * stores can be certified. Nothing in the production tree consumes this API;
 * do not add a consumer without first making the writer accounting real for
 * that build.
 *
 * `coherent` is also 0 when the aggregate would overflow the counter domain
 * (each block may legally hold up to QUOTA_AMOUNT_MAX == INT64_MAX, so two
 * blocks can exceed it) or when a node could not be pinned. A comparison
 * against an incoherent snapshot is INDETERMINATE and must never be reported
 * as a leak. */
typedef struct {
    uint64_t generation;                        /* registry gen at capture   */
    uint32_t user_blocks;                       /* live USER blocks seen     */
    uint8_t  coherent;                          /* 0 = mixed generations     */
    int64_t  usage[QUOTA_RESOURCE_TYPE_COUNT];  /* summed outstanding usage  */
} quota_leak_snapshot_t;

/* Capture into `out`. Returns non-zero when the snapshot is coherent (the
 * same value left in out->coherent). A NULL `out` is a no-op returning 0.
 * Same calling contract as quota_dump: PASSIVE_LEVEL, no lock held. */
int quota_leak_snapshot(quota_leak_snapshot_t *out);

/* --- Configurable per-user default limits (quota_config.c) ---------------- *
 * The kernel-config override layer named in the precedence rule above. One
 * runtime tunable per resource type, "quota.user.<type-name>", consulted when
 * a USER block is seeded and re-applied to live USER blocks when changed. */

/* Register one tunable per resource type. Call once, in Phase 2, immediately
 * after quota_register_types(): the taxonomy supplies each tunable's default
 * and its name, so it must be validated first. Idempotent -- a second call
 * finds every name already taken and registers nothing. */
void quota_config_register_tunables(void);

/* The effective default limit for `type` on a USER block: the configured
 * override when one is registered, otherwise the taxonomy's default_limit.
 * Returns 0 (QUOTA_LIMIT_UNLIMITED) for an out-of-range type. Safe to call
 * before quota_config_register_tunables -- it falls back to the taxonomy. */
uint64_t quota_config_user_default(quota_resource_type_t type);

/* The tunable name for `type` ("quota.user.handles", ...), or NULL if the type
 * is out of range. Exposed so callers and tests name a tunable through the one
 * table rather than rebuilding the string. */
const char *quota_config_tunable_name(quota_resource_type_t type);

/* Apply `limit` to every LIVE USER block whose limit for `type` was never set
 * explicitly by quota_set_limit. Called by the tunable change callback; also
 * the tested seam for the walk. Lowering below current usage is allowed and
 * does not rewrite usage -- later charges are simply refused. */
void quota_user_default_relimit(quota_resource_type_t type, uint64_t limit);

/* Publish the effective USER default for `type` WITHOUT walking live blocks.
 * Used to seed the cache from the taxonomy at validation and to prime it from
 * the tunables at registration -- both points where no live USER block can yet
 * disagree. To change a default at runtime use quota_user_default_relimit,
 * which publishes AND re-limits; publishing alone would leave existing users
 * on the old value. Limits above QUOTA_AMOUNT_MAX are clamped into the counter
 * domain; an out-of-range type is a no-op. */
void quota_user_default_publish(quota_resource_type_t type, uint64_t limit);

/* The currently published effective USER default for `type`, or 0 (unlimited)
 * for an out-of-range type. This is the value a new USER block is seeded with,
 * which is NOT necessarily what the tunable registry reports: it is re-read
 * from the tunables only at registration and on a change callback. */
uint64_t quota_user_default_current(quota_resource_type_t type);

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
 * block's fate. Two admissible caller forms, and ONLY these two:
 *   (a) hold the REGISTRY lock across the call (what quota.c's own lookups
 *       do) -- the deref path unlinks under that same lock before freeing, so
 *       a block observed in the registry is never freed under a successful
 *       try-ref; or
 *   (b) for an UNREGISTERED process/job block, hold the OWNER's lock across
 *       the call AND have the owner's teardown clear its pointer under that
 *       same lock BEFORE dereferencing (quota_task_teardown and job_on_delete
 *       are the reference implementations). A teardown that derefs first
 *       leaves this CAS running on freed memory. */
int quota_block_try_ref(quota_block_t *block);

/* Release a reference; frees the block when the last one goes away. */
void quota_block_deref(quota_block_t *block);

/* The block's owner SID, or NULL if none was recorded. Valid while the caller
 * holds a reference; the storage belongs to the block. */
const SID *quota_block_owner(const quota_block_t *block);

/* Which accounting layer this block belongs to. QUOTA_PRINCIPAL_COUNT for a
 * NULL block, so a caller filtering by kind never needs to pre-validate. */
quota_principal_t quota_block_principal(const quota_block_t *block);

/* Stable, never-reused identity for this block; 0 for NULL. Assigned at create
 * and immutable, so it is readable without the block lock. Diagnostic records
 * carry it because a block ADDRESS is recycled by the allocator: a consumer
 * correlating two events by address could attribute a later block's refusal to
 * an earlier block's owner. */
uint64_t quota_block_id(const quota_block_t *block);

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

/* Count of quota_return_chain calls that EXHAUSTED their bounded wait on a
 * receipt still BUSY at their own generation. The event, not the conclusion: for
 * a direct caller it is a leaked charge (the usage is still counted, the chain's
 * block references are still held, and that caller held the only token able to
 * return them), while a ledger return wraps this call in its own retry and may
 * still recover -- correlate with the ledger's own abandon counter before
 * calling it a leak. Stale and duplicate returns are documented no-ops and are
 * NOT counted. Counted rather than logged because the path runs with interrupts
 * masked. */
void     quota_note_return_wait_exhausted(void);
uint64_t quota_return_wait_exhausted_count(void);

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
 * `tag` makes ownership EXCLUSIVE and IDENTIFIED rather than merely documented.
 * A receipt embedded next to the resource it accounts for is reachable from
 * more than one CPU, so two cleanup paths can race on it: without the state
 * both would see a non-empty receipt, both would credit the amount back, and
 * the second credit would erase charges made since -- while dereferencing the
 * same references twice. Charging into a receipt that already holds a live
 * charge is refused for the same reason: it would silently drop the references
 * to the blocks the first charge is still holding and strand that usage.
 *
 * The state alone is NOT enough once receipt STORAGE is reused. A returner that
 * stalls after its charge was already returned, and resumes after the same
 * storage was charged again, finds ACTIVE and returns the SECOND charge -- an
 * ABA on the state word. So the state does not stand alone: it is packed with a
 * monotonic GENERATION into one 64-bit word, mutated only as a PAIR. Every
 * transition that must not race another agent -- claiming an IDLE receipt to
 * charge it, and claiming an ACTIVE one to return or adjust it -- is a single
 * atomic64_cmpxchg on the whole word. The transitions made by the agent that
 * already holds the receipt BUSY, namely publishing ACTIVE and releasing back
 * to IDLE, are release stores (atomic64_set): the BUSY claim is what makes them
 * exclusive, so there is no other writer for a CAS to guard against, and the
 * release order is what publishes the receipt's fields with the tag. A caller
 * receives its generation as a token
 * from quota_charge_chain and must present it to quota_return_chain, which
 * CASes the exact {generation, ACTIVE} it was handed. A stale returner presents
 * a superseded generation, matches nothing, and is a no-op.
 *
 * Checking the generation NEXT TO the state CAS rather than inside it does not
 * work in either order: check-then-CAS lets the stale returner resume between
 * the two and claim the newer charge, and CAS-then-check makes the legitimate
 * returner observe BUSY and give up while the stale caller restores ACTIVE,
 * leaking the charge permanently. One word, one CAS. */
typedef struct quota_charge_receipt {
    quota_block_t        *blocks[QUOTA_CHAIN_MAX]; /* charged set, one ref each */
    uint32_t              count;                   /* live entries in blocks[]  */
    quota_resource_type_t type;                    /* what was charged          */
    uint64_t              amount;                  /* how much, per block       */
    atomic64_t            tag;                     /* {generation, state}       */
} quota_charge_receipt_t;

/* Receipt states. IDLE is 0 so a zero-initialized receipt is valid (generation
 * 0, IDLE), which is exactly the "empty receipt returns nothing" contract. */
#define QUOTA_RECEIPT_IDLE       0   /* holds nothing; may be charged into  */
#define QUOTA_RECEIPT_ACTIVE     1   /* holds a live charge; may be returned */
#define QUOTA_RECEIPT_BUSY       2   /* a charge or return owns it right now */

/* Tag encoding: the low QUOTA_RECEIPT_STATE_BITS carry the state, everything
 * above carries the generation. The generation is 62 bits, so exhausting it
 * needs 2^62 charges on ONE receipt -- unreachable at any real charge rate --
 * but the charge path still fails closed at the ceiling rather than wrapping a
 * token back onto a live one (a wrapped generation is the same stale-token
 * identity the tag exists to prevent). */
/* Bound on how long a return waits for an in-progress adjust on the SAME charge
 * to republish. Bounded rather than unbounded because this path is reachable
 * with interrupts disabled, where an unbounded retry is a hang and not merely a
 * slowdown -- the same reasoning the rate-limit reader's retry bound uses. An
 * adjust holds its claim only across a bounded, non-blocking multi-block
 * transaction, so this is generous by orders of magnitude. */
#define QUOTA_RETURN_BUSY_TRIES   64u

#define QUOTA_RECEIPT_STATE_BITS  2u
#define QUOTA_RECEIPT_STATE_MASK  ((1u << QUOTA_RECEIPT_STATE_BITS) - 1u)
#define QUOTA_RECEIPT_GEN_MAX     (0xFFFFFFFFFFFFFFFFull >> QUOTA_RECEIPT_STATE_BITS)
#define QUOTA_RECEIPT_TAG(gen, state) \
    ((int64_t)(((uint64_t)(gen) << QUOTA_RECEIPT_STATE_BITS) | (uint64_t)(state)))
#define QUOTA_RECEIPT_TAG_GEN(tag)   ((uint64_t)(tag) >> QUOTA_RECEIPT_STATE_BITS)
#define QUOTA_RECEIPT_TAG_STATE(tag) ((uint32_t)((uint64_t)(tag) & QUOTA_RECEIPT_STATE_MASK))

/* Every state value must fit under the state mask, or a state would bleed into
 * the generation field and two different tags would compare equal. */
_Static_assert(QUOTA_RECEIPT_BUSY <= QUOTA_RECEIPT_STATE_MASK,
               "receipt state values must fit in QUOTA_RECEIPT_STATE_BITS");
/* The encode/decode round trip must hold at the CEILING, not just near zero.
 * QUOTA_RECEIPT_TAG casts into int64_t, and at QUOTA_RECEIPT_GEN_MAX the shifted
 * value exceeds INT64_MAX, so the conversion is the one place in this encoding
 * where signedness is load-bearing. Pinning both halves of the round trip here
 * makes a toolchain that does not wrap two's-complement a BUILD failure rather
 * than a token that silently decodes to the wrong charge. */
_Static_assert(QUOTA_RECEIPT_TAG_GEN(QUOTA_RECEIPT_TAG(QUOTA_RECEIPT_GEN_MAX,
                                                       QUOTA_RECEIPT_ACTIVE))
                   == QUOTA_RECEIPT_GEN_MAX,
               "receipt generation must survive encode/decode at the ceiling");
_Static_assert(QUOTA_RECEIPT_TAG_STATE(QUOTA_RECEIPT_TAG(QUOTA_RECEIPT_GEN_MAX,
                                                         QUOTA_RECEIPT_ACTIVE))
                   == QUOTA_RECEIPT_ACTIVE,
               "receipt state must survive encode/decode at the ceiling");
_Static_assert(QUOTA_RECEIPT_TAG(0, QUOTA_RECEIPT_IDLE) == 0,
               "a zero-initialized receipt must decode as generation 0 / IDLE");

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
 * holding nothing of its own, so an unconditional
 * quota_return_chain(receipt, token) on the error path is safe. The one
 * exception is STATUS_INVALID_PARAMETER raised because the receipt was ALREADY
 * holding a live charge: that charge is untouched and still belongs to whoever
 * made it, so a caller must not treat that status as licence to return it.
 *
 * `out_token` is MANDATORY (NULL is STATUS_INVALID_PARAMETER): on success it
 * receives the generation identifying THIS charge. It is the caller's proof of
 * ownership and the only key that will return the charge, so a charge with
 * nowhere to record it would strand its usage; store it beside the receipt and
 * pass it to quota_return_chain.
 *
 * A zero-amount charge succeeds with token 0, the canonical NO-OBLIGATION
 * token: it owes nothing, and 0 is the one value quota_return_chain always
 * refuses. It is written rather than left alone precisely because a zero charge
 * does not advance the generation -- a value left in the caller's variable
 * could otherwise collide with the token of the NEXT real charge on the same
 * receipt.
 *
 * `*out_token` is written on success, and CLEARED to 0 by any failure raised
 * after the receipt has been claimed. Only the refusals raised BEFORE the claim
 * leave it untouched -- a bad argument, QUOTA_CHARGE_CLIENT, an exhausted
 * generation, or a receipt that already holds a live charge. That last one is
 * the case the exemption exists for: the earlier charge is still owned by
 * whoever made it, and clearing its token would strand the usage with no key
 * able to return it. Everywhere else the caller's variable is stale by
 * definition (the claim proved the receipt held nothing), and leaving a stale
 * value is NOT harmless: one equal to the next generation this receipt will
 * publish would let an error-path return take a charge it never made.
 *
 * Returns STATUS_INVALID_PARAMETER (bad argument, a chain deeper than
 * QUOTA_CHAIN_MAX, or a receipt that already holds a live charge),
 * STATUS_NOT_SUPPORTED (QUOTA_CHARGE_CLIENT -- see the flag),
 * STATUS_PROCESS_IS_TERMINATING (no process block, or the task's charge gate is
 * SEALED because it is dying), STATUS_INTEGER_OVERFLOW (this receipt's
 * generation space is exhausted), STATUS_RETRY, or whatever the refusing block
 * returned.
 *
 * STATUS_RETRY IS TRANSIENT AND MUST NOT BE TREATED AS A QUOTA REFUSAL. It means
 * a job-membership transition currently holds this task's charge gate closed (see
 * quota_gate_quiesce in quota_ledger.h). The charge was not attempted, nothing
 * was charged, and the same call will succeed once the transition completes.
 *
 * It is reported on the FIRST refusal rather than retried internally, and
 * deliberately so: the gate stays closed for the whole absorb-and-migrate
 * transition, so immediate re-probing cannot span it, and each probe costs a
 * locked read-modify-write on a shared counter on the hot charge path. A caller
 * that maps this to STATUS_QUOTA_EXCEEDED tells user mode a process is out of
 * quota when it merely raced an assignment; retry it at a point where waiting is
 * acceptable, or propagate it to a layer that can. */
NTSTATUS quota_charge_chain(struct task *task, quota_resource_type_t type,
                            uint64_t amount, uint32_t flags,
                            quota_charge_receipt_t *receipt,
                            uint64_t *out_token);

/* Return a charge recorded by quota_charge_chain and release the receipt's
 * references. Exactly one caller performs the return even if several race; the
 * losers are no-ops, so a double return cannot erase a newer live charge. Safe
 * on an empty or zero-initialized receipt.
 *
 * ALL-OR-NOTHING: this returns the whole charge and ends it. There is no
 * partial-return form of this call; to give back part of a charge and KEEP the
 * rest (the charge-then-trim-the-remainder pattern), call quota_charge_adjust
 * with the smaller amount -- it lowers every block in the chain and leaves the
 * charge ACTIVE under the same token.
 *
 * Returning a receipt while the charge that fills it is still in flight on
 * another CPU is a CALLER ORDERING ERROR, not a race this can resolve: the
 * return finds the receipt still being built, does nothing, and the charge
 * stands. There is no correct alternative -- crediting back a charge that has
 * not finished would corrupt the counters. Complete the charge first.
 *
 * ONE EXCEPTION, and it is a RETRY rather than a give-up: a receipt found BUSY
 * at the caller's OWN generation is a charge being resized by quota_charge_adjust
 * on another CPU, not a charge still being built. Treating that as a no-op would
 * silently discard the only return this charge will ever get, so the return
 * spins (bounded, and only while the generation still matches) until the adjust
 * republishes. A generation that has MOVED means the charge is already gone, and
 * the return correctly stops.
 *
 * SCOPE OF THAT GUARANTEE: `token` is what extends it from "the same charge"
 * to "the same charge in reused storage". Only the exact token handed out by
 * the quota_charge_chain that filled this receipt returns that charge; a stale
 * returner holding a superseded token matches nothing and does nothing, even if
 * the storage has since been recharged. Passing token 0, a non-canonical token
 * (above QUOTA_RECEIPT_GEN_MAX), or a token from a different charge is a silent
 * no-op by design -- that is the mechanism, not a swallowed error.
 *
 * LIFETIME INVARIANT the guarantee rests on: every potential returner must
 * CAPTURE the token BY VALUE before the receipt's storage can be reused. The
 * token identifies the charge, so a cleanup path that instead keeps a POINTER
 * into the recycled object and loads the token only when it finally runs would
 * read the token of whatever charge occupies that storage NOW, match it, and
 * return a resource it never charged. Recycling is safe for the RECEIPT,
 * because its generation moves; it is NOT safe for a token still being read
 * through shared storage. Copy the token out beside the resource whose lifetime
 * it tracks, or keep the receipt and token alive until every path that could
 * return that charge has run. */
void quota_return_chain(quota_charge_receipt_t *receipt, uint64_t token);

/* Change an outstanding charge's amount across EVERY block that holds it, all
 * or nothing.
 *
 * WHY THIS IS NOT A RETURN FOLLOWED BY A CHARGE, nor a charge-the-delta with a
 * prefix unwind. Both of those leave observable damage behind on refusal:
 * committing a charge lifts the block's PEAK, and no return lowers a peak
 * again, so a per-block walk that gets three blocks in and is then refused has
 * permanently inflated three peaks for a transaction that never happened. The
 * decrease direction is no safer in the other order -- a return can itself be
 * refused when the counter does not hold what the caller thinks it does, and by
 * then an already-returned prefix cannot be safely recharged, because the
 * recharge is exactly the thing that can be refused.
 *
 * So this takes EVERY block's lock at once, in the same ascending-address order
 * quota_try_transfer uses (so two adjusts over overlapping chains cannot
 * deadlock), PREVALIDATES the whole delta under those locks, and only then
 * commits every counter and the receipt's own amount. On refusal not one usage
 * value, peak, or receipt field has moved: the failure telemetry of the block
 * that came up short is the only thing that changes, which is deliberate and
 * matches every other refusal in this module.
 *
 * `token` must be the exact token that the charge was issued with -- the same
 * proof-of-ownership quota_return_chain requires, for the same reason: without
 * it, a stale holder could resize a charge that reused the storage.
 *
 * The receipt is BUSY for the duration, which is a state a token-holding
 * returner can now legitimately encounter (before this existed, an ACTIVE
 * charge was never briefly BUSY). quota_return_chain therefore RETRIES on a
 * BUSY tag at its own generation instead of treating it as a no-op -- see its
 * contract above. Callers need do nothing about this.
 *
 * A new amount equal to the current one is a success that touches nothing. A
 * new amount of 0 is NOT a return: the obligation survives holding zero, and
 * quota_return_chain remains the only way to end it.
 *
 * Returns STATUS_INVALID_PARAMETER (bad argument, a non-canonical token, or a
 * token that does not name a live charge on this receipt), the refusing block's
 * status (STATUS_QUOTA_EXCEEDED, STATUS_INTEGER_OVERFLOW) on an increase that
 * does not fit, or STATUS_INTEGER_OVERFLOW on a decrease larger than what a
 * block actually holds (an accounting-integrity failure, not a routine
 * refusal). */
NTSTATUS quota_charge_adjust(quota_charge_receipt_t *receipt, uint64_t token,
                             uint64_t new_amount);

/* THE charge entry point for a charging subsystem: bill the CURRENT task's
 * chain, or take the boot exemption. Consumers call this and NEVER inspect
 * task->quota themselves -- see the contract in quota_owner.c.
 *
 * Before the scheduler is ready (and before the taxonomy is validated) there
 * is no principal to bill: the call succeeds with token 0, having charged
 * nothing. AFTER that milestone a task with no process block is dying or
 * half-built and the charge is REFUSED with STATUS_PROCESS_IS_TERMINATING --
 * the exemption is bound to boot, never to the absence of a block, so a
 * request racing its own process teardown cannot inherit it.
 *
 * Otherwise this is quota_charge_chain with flags 0: same receipt/token
 * contract, same statuses, and quota_return_chain returns the charge. */
NTSTATUS quota_charge_current(quota_resource_type_t type, uint64_t amount,
                              quota_charge_receipt_t *receipt,
                              uint64_t *out_token);

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
 * window needs more than a membership generation: revalidating a generation
 * after the charge turns the under-count into a DOUBLE count, because the
 * absorb has already folded the in-flight charge in and the retry then charges
 * the job again. It needs a transition protocol that drains in-flight chargers
 * before absorbing and publishes membership before reopening charging.
 *
 * THAT PROTOCOL NOW EXISTS: quota_gate_quiesce in quota_ledger.h, which
 * ob_job_assign holds across the absorb and the publication. Every chain charge
 * enters the gate, so no charge can land in this window at all -- the window is
 * closed for charges rather than merely bounded. A RETURN still may land here
 * (it is handed a receipt and a token and never learns whose task they are), and
 * that remains benign for the reason the KNOWN LIMITATION below describes:
 * conservation holds, the job merely over-holds headroom until detach.
 *
 * Returns STATUS_SUCCESS when there is nothing to absorb.
 *
 * REMAINING LIMITATION, now narrowed to receipts the ledger cannot see.
 * Absorbed usage is billed to the job until the member DEPARTS rather than until
 * the underlying resource is freed, because a pre-join receipt names only the
 * process and user blocks: returning one while still a member reduces those two
 * and leaves the job's copy standing, and the absorb record only unwinds at
 * detach.
 *
 * quota_ledger_migrate_to_job closes that for every obligation held in a LEDGER:
 * it appends the job block to the outstanding receipt and subtracts the same
 * amount from this record, so the return reaches the job and the record no longer
 * owes it. What it cannot reach is a receipt embedded in its caller's own
 * structure (an ALPC message, a notification state), because those are not
 * enumerable from the task. Those amounts stay with the absorb record and unwind
 * at detach exactly as described above -- conservation holds either way, and the
 * cost is bounded over-held job headroom, never a lost or duplicated charge.
 * Converting the embedded-receipt consumers to ledger obligations is owned as
 * concrete follow-up work by the charge-path cost and lifetime section. */
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

/* ========================================================================== *
 * Charge-path cost contract
 *
 * The pool-integration section had to answer "what does a charge cost?" before
 * any hot path could adopt this API. The answer is expressed STRUCTURALLY --
 * how many block critical sections an operation enters -- rather than in time.
 *
 * Why not a wall-clock budget: a nanosecond threshold asserted in a test would
 * have to pass on QEMU TCG, KVM, WHPX, VirtualBox, and bare metal, whose timing
 * differs by more than an order of magnitude. Such a test either fails on the
 * slow platform or is widened until it accepts every answer and verifies
 * nothing (CLAUDE.md forbids exactly that widening). A critical-section count
 * is exact, identical on every platform, and is what actually changed when a
 * charge got more expensive -- the one number a refactor can regress silently.
 *
 * What the count does NOT capture, and must not be read as capturing: lock HOLD
 * time, contention under concurrent chargers, and cache-miss cost. Those are
 * genuine latency, and they are measured advisorily (TSC, reported and never
 * asserted) by the quota performance suite. A real contended-latency budget
 * needs cross-CPU run queues that the scheduler does not have yet, so it is
 * owned by the quota contention-test and leak-sweep work.
 *
 * The constants below are the enforced structural guard: they are asserted
 * exactly (not as upper bounds) so that both an added lock and a REMOVED one
 * are caught -- a charge that stopped taking its lock would otherwise look like
 * an improvement.
 * ========================================================================== */

/* Critical sections entered by one successful single-block operation. Charge
 * and return each take the block's lock exactly once, for the whole of the
 * check-and-commit; a transfer holds both blocks (address-ordered). */
#define QUOTA_BUDGET_CHARGE_LOCKS     1u
#define QUOTA_BUDGET_RETURN_LOCKS     1u
#define QUOTA_BUDGET_TRANSFER_LOCKS   2u
#define QUOTA_BUDGET_SET_LIMIT_LOCKS  1u

/* A chain operation enters one BLOCK critical section per layer it charges
 * (process, user, job): the chain is charged one block at a time, never two at
 * once, so a depth-N charge costs N and its return costs N. */
#define QUOTA_BUDGET_CHAIN_LOCKS_PER_LAYER  1u

/* ...plus the OWNER-side sections a chain charge needs to discover that chain
 * safely, which the budget counts as well. Excluding them would be the whole
 * mistake this contract exists to prevent: they are irqsave sections on the
 * same code path, they dominate a shallow chain, and a budget blind to them
 * would let owner-side locking grow without a single test noticing.
 *
 * Three, one per snapshot step: the process/user pointer pair under
 * task->quota_lock, the liveness re-check of the process pointer, and the job
 * pin under task->job_lock. A chain RETURN adds none -- it walks the receipt,
 * which already holds a reference to every block it will touch.
 *
 * So a depth-2 charge+return lifetime is 3 + 2 + 2 = 7 sections, which is the
 * measured cost the pool-integration section had to establish before any hot
 * path could adopt this API. */
#define QUOTA_BUDGET_CHAIN_OWNER_LOCKS  3u

/* A zero-amount chain charge is a no-op for the COUNTERS but not for the
 * owner: it still takes task->quota_lock once to confirm the task could have
 * been charged at all, so a charge against a dying task is refused rather than
 * silently succeeding. It is therefore NOT a zero-lock operation, unlike the
 * single-block no-ops and the stale-token return. */
#define QUOTA_BUDGET_CHAIN_ZERO_OWNER_LOCKS  1u

/* An adjust enters one block critical section per block the receipt names -- but
 * unlike a chain charge it holds them ALL AT ONCE (ascending address order),
 * because prevalidate-then-commit across several blocks is only atomic if none
 * of them can move in between. The COUNT is what this contract measures, so it
 * is the same per-block 1; the simultaneous hold is a lock-ORDER fact, recorded
 * in the quota_charge_adjust contract rather than here. */
#define QUOTA_BUDGET_ADJUST_LOCKS_PER_BLOCK  1u

/* The per-task charge gate costs ZERO block critical sections: it is two atomic
 * read-modify-writes on one word in the task (enter, exit), with no lock and no
 * allocation. That is why every chain charge can afford to pass through it, and
 * why adding it did not change any constant above. Stated here because a future
 * reader comparing the measured section count against this contract needs to
 * know the gate is deliberately outside it, not accidentally missing from it. */
#define QUOTA_BUDGET_GATE_LOCKS  0u

/* ========================================================================== *
 * Rate-limit policy records
 *
 * A quota block carries TWO different kinds of state, and conflating them is
 * the mistake this separation exists to prevent:
 *   - CUMULATIVE USAGE (everything above): how much has been consumed, charged
 *     and returned, enforced per charge.
 *   - RATE POLICY (here): how fast consumption is ALLOWED to proceed. It is
 *     never charged, never returned, and has no usage of its own.
 *
 * This section OWNS the record. It does not enforce it: enforcement belongs to
 * the subsystem that schedules the resource, and both consumers (a CPU
 * bandwidth controller and a block-I/O QoS layer) are tracked as concrete
 * unowned work rather than implied here. A record with no enforcer is inert --
 * setting one changes nothing today, which is why it is stored and reported
 * rather than treated as a cap.
 *
 * WHY THE RECORD IS TYPED AND VERSIONED rather than five bare scalars: the two
 * consumers do not share units. A CPU cap is "runtime nanoseconds per period",
 * an I/O cap is "operations per period" or "bytes per period", and read, write,
 * and control traffic are independently limited in practice. A record that did
 * not carry its own key, unit, and period could not be interpreted correctly by
 * either consumer, and could not gain a field later without breaking the other.
 * ========================================================================== */

/* Which stream a record governs. The value is ABI (it indexes the per-block
 * array and will appear in the later query syscall): APPEND only. */
typedef enum quota_rate_class {
    QUOTA_RATE_CLASS_CPU        = 0,  /* processor time                     */
    QUOTA_RATE_CLASS_IO_READ    = 1,  /* block-device read traffic          */
    QUOTA_RATE_CLASS_IO_WRITE   = 2,  /* block-device write traffic         */
    QUOTA_RATE_CLASS_IO_CONTROL = 3,  /* device-control traffic             */
    QUOTA_RATE_CLASS_COUNT      = 4
} quota_rate_class_t;

_Static_assert(QUOTA_RATE_CLASS_CPU == 0 && QUOTA_RATE_CLASS_IO_READ == 1 &&
               QUOTA_RATE_CLASS_IO_WRITE == 2 && QUOTA_RATE_CLASS_IO_CONTROL == 3,
    "quota rate-class IDs are ABI: append new classes, never renumber");

/* How an envelope's amounts are denominated. This is DERIVED from the class and
 * which envelope the amounts sit in (see quota_rate_envelope_unit), not stored
 * as a free field: a stored unit could disagree with its class, which is a
 * policy no consumer can act on. */
typedef enum quota_rate_unit {
    QUOTA_RATE_UNIT_NS    = 0,   /* nanoseconds of runtime per period */
    QUOTA_RATE_UNIT_OPS   = 1,   /* operations per period            */
    QUOTA_RATE_UNIT_BYTES = 2    /* bytes per period                 */
} quota_rate_unit_t;

/* Which fields carry meaning. A zero is a legitimate value for every amount
 * (reservation 0, hard cap 0 = "allow nothing"), so "set" cannot be inferred
 * from the value; an unflagged field is UNSET and a consumer must ignore it
 * rather than read it as zero. */
#define QUOTA_RATE_F_WEIGHT       0x1u  /* proportional share is meaningful   */
#define QUOTA_RATE_F_RESERVATION  0x2u  /* guaranteed floor is meaningful     */
#define QUOTA_RATE_F_MAX          0x4u  /* soft ceiling is meaningful         */
#define QUOTA_RATE_F_HARD_CAP     0x8u  /* hard ceiling is meaningful         */
#define QUOTA_RATE_F_ALL \
    (QUOTA_RATE_F_WEIGHT | QUOTA_RATE_F_RESERVATION | \
     QUOTA_RATE_F_MAX | QUOTA_RATE_F_HARD_CAP)

/* Record layout version. A consumer that does not recognize the version must
 * refuse the record instead of interpreting unknown fields. */
#define QUOTA_RATE_VERSION  1u

/* Amounts share the counter domain so a policy value can never exceed what the
 * accounting side can represent. */
#define QUOTA_RATE_AMOUNT_MAX  QUOTA_AMOUNT_MAX

/* One set of amounts in ONE denomination, plus the flags saying which of them
 * are meaningful. A record carries two of these because a single stream is
 * legitimately limited in two denominations at once. */
typedef struct quota_rate_envelope {
    uint32_t flags;        /* QUOTA_RATE_F_* -- which amounts below apply */
    uint32_t reserved;     /* MUST be 0 -- see the no-implicit-padding rule */
    uint64_t weight;       /* proportional share vs. other blocks         */
    uint64_t reservation;  /* guaranteed floor per period                 */
    uint64_t max;          /* soft ceiling per period                     */
    uint64_t hard_cap;     /* enforced ceiling per period                 */
} quota_rate_envelope_t;

/* One rate policy for one class. Read and written only as a WHOLE (see set/get
 * below): the fields are one logical decision, and a scheduler that observed a
 * new hard cap beside an old reservation could admit work no consistent policy
 * ever allowed.
 *
 * TWO ENVELOPES, because one is not enough for a real I/O policy. A storage QoS
 * layer caps a stream by BOTH operations per second and bytes per second -- a
 * pure-IOPS cap lets one huge request saturate the device, and a pure-bandwidth
 * cap lets a flood of tiny requests exhaust the queue. With a single
 * denomination per record, publishing a bytes policy would silently REPLACE the
 * ops policy for that stream, so the consumer could only ever enforce one of
 * the two protections. The denomination of each envelope is implied by the
 * class (quota_rate_envelope_unit), so a record cannot disagree with itself:
 *   CPU:      `primary` is nanoseconds of runtime; `bytes` is unused and MUST
 *             be left unflagged (CPU time has no byte dimension).
 *   IO_READ / IO_WRITE / IO_CONTROL:
 *             `primary` is operations, `bytes` is bytes. Either, both, or
 *             neither may be flagged.
 *
 * `generation` is assigned by the publisher and increments on every successful
 * set. A consumer caches it to detect that the policy changed without having to
 * compare every field. */
typedef struct quota_rate_limit {
    uint32_t              version;    /* QUOTA_RATE_VERSION                     */
    uint32_t              reserved0;  /* pad to a stable layout; must be 0      */
    uint64_t              period_ns;  /* accounting window, shared by both      */
    uint64_t              generation; /* bumped by each successful set          */
    quota_rate_envelope_t primary;    /* CPU: runtime ns. I/O: operations.      */
    quota_rate_envelope_t bytes;      /* I/O only: bytes. Unused for CPU.       */
} quota_rate_limit_t;

/* NO IMPLICIT PADDING, and the layout is PINNED. This record is a versioned ABI
 * that a later query syscall will copy outward by sizeof(), so a byte the
 * publisher never defines is a byte of kernel stack residue leaving the kernel.
 * `uint32_t flags` beside a `uint64_t` would leave a 4-byte hole that no
 * assignment is required to initialize, so the hole is spelled out as
 * `reserved` (required to be 0, stored and loaded explicitly like any other
 * field). The asserts below are what keep that true: a future field added in
 * the wrong place changes a size or an offset and breaks the build instead of
 * silently reintroducing a hole or renumbering the ABI. */
_Static_assert(sizeof(quota_rate_envelope_t) == 40,
    "quota_rate_envelope_t is ABI: 5 named slots, no implicit padding");
_Static_assert(__builtin_offsetof(quota_rate_envelope_t, flags) == 0 &&
               __builtin_offsetof(quota_rate_envelope_t, reserved) == 4 &&
               __builtin_offsetof(quota_rate_envelope_t, weight) == 8 &&
               __builtin_offsetof(quota_rate_envelope_t, reservation) == 16 &&
               __builtin_offsetof(quota_rate_envelope_t, max) == 24 &&
               __builtin_offsetof(quota_rate_envelope_t, hard_cap) == 32,
    "quota_rate_envelope_t field offsets are ABI");
_Static_assert(sizeof(quota_rate_limit_t) == 104,
    "quota_rate_limit_t is ABI: header + two envelopes, no implicit padding");
_Static_assert(__builtin_offsetof(quota_rate_limit_t, version) == 0 &&
               __builtin_offsetof(quota_rate_limit_t, reserved0) == 4 &&
               __builtin_offsetof(quota_rate_limit_t, period_ns) == 8 &&
               __builtin_offsetof(quota_rate_limit_t, generation) == 16 &&
               __builtin_offsetof(quota_rate_limit_t, primary) == 24 &&
               __builtin_offsetof(quota_rate_limit_t, bytes) == 64,
    "quota_rate_limit_t field offsets are ABI");

/* The denomination of one envelope of a class's record, so a consumer derives
 * the unit rather than trusting a stored field. Returns QUOTA_RATE_UNIT_NS for
 * the CPU primary envelope, OPS for an I/O primary, and BYTES for an I/O bytes
 * envelope. `want_bytes` selects the envelope. The CPU bytes envelope has no
 * valid unit and is never published, so it reports NS and carries no flags. */
quota_rate_unit_t quota_rate_envelope_unit(quota_rate_class_t cls, int want_bytes);

/* Publish a rate policy for `class` on `block`, COHERENTLY: a concurrent getter
 * observes either the whole previous record or the whole new one, never a mix.
 * `rec->generation` is ignored on input and assigned by this call.
 *
 * CANONICALIZED on publication: an amount whose flag is clear is stored as 0,
 * and the whole `bytes` envelope of a CPU policy is stored as 0, regardless of
 * what the caller passed. The contract says an unflagged amount is meaningless,
 * so persisting whatever happened to be in the caller's struct would store
 * indeterminate values, make two logically identical policies compare unequal,
 * and hand uninitialized stack bytes to a future query syscall. What comes back
 * from a get is therefore always the canonical form of what was set.
 *
 * Validated before publication, so an unreadable policy can never be stored:
 *   - version must be QUOTA_RATE_VERSION, and reserved0 / each envelope's
 *     `reserved` must be 0
 *   - neither envelope's flags may contain unknown bits
 *   - the `bytes` envelope must be unflagged for QUOTA_RATE_CLASS_CPU: CPU time
 *     has no byte dimension, and accepting one would store something the
 *     consumer must later ignore -- a silent configuration failure
 *   - every FLAGGED amount must be <= QUOTA_RATE_AMOUNT_MAX
 *   - WITHIN each envelope, a flagged reservation must not exceed a flagged max
 *     or hard cap, and a flagged max must not exceed a flagged hard cap (an
 *     unsatisfiable policy is a configuration error, not something for a
 *     consumer to resolve). The two envelopes are independent dimensions and
 *     are NOT compared against each other
 *   - period_ns must be non-zero whenever any per-period amount is flagged in
 *     either envelope
 *
 * Returns STATUS_INVALID_PARAMETER on a NULL argument, a bad class, or any
 * validation failure, and STATUS_SUCCESS otherwise. */
NTSTATUS quota_rate_limit_set(quota_block_t *block, quota_rate_class_t cls,
                              const quota_rate_limit_t *rec);

/* Read the current policy for `class` into `out`, as a coherent snapshot.
 *
 * NEVER takes the block lock and NEVER spins unboundedly, because the intended
 * consumer is a scheduler or I/O dispatcher holding its own lock with
 * interrupts disabled -- a context where the no-nested-call contract above
 * forbids the lock and an unbounded retry is a hang, not a slowdown. The read
 * is a per-class seqlock bounded to a fixed number of attempts.
 *
 * A block with no policy set for `class` reports a zeroed record with flags 0
 * and generation 0 -- "no policy", distinct from a policy whose amounts happen
 * to be zero, which carries flags.
 *
 * Returns STATUS_INVALID_PARAMETER on a NULL argument or a bad class, and
 * STATUS_RETRY if a publisher held this class in flux for every attempt. On
 * STATUS_RETRY `*out` is UNTOUCHED: the caller keeps whatever snapshot it had,
 * which is the correct behavior for a policy consumer -- acting on a policy one
 * update stale is safe, acting on a torn one is not. */
NTSTATUS quota_rate_limit_get(const quota_block_t *block, quota_rate_class_t cls,
                              quota_rate_limit_t *out);

#ifdef KERNEL_TESTS
/* --- Test-only raw counter access ---------------------------------------- *
 * Corrupted-state and saturation fixtures cannot be built through the public
 * API (it exists precisely to keep counters in their domain). These helpers
 * write the counters directly, bypassing every guard on purpose. Same
 * test-only pattern as kmalloc_fail_next(). Present whenever KERNEL_TESTS is
 * defined -- which is the Makefile DEFAULT, so these seams are in the image
 * that boots on real hardware; only a KERNEL_TESTS=off build prunes them. */

/* Raw counter writes for corrupted-state fixtures (negative usage, saturated
 * failure counters). */
void quota_test_poke_usage(quota_block_t *block, quota_resource_type_t type, int64_t value);
void quota_test_poke_failures(quota_block_t *block, quota_resource_type_t type, int64_t value);

/* Raw counter read that does NOT clamp negatives, so a test can prove a
 * corrupted counter was left untouched rather than quietly normalized. */
int64_t quota_test_raw_usage(const quota_block_t *block, quota_resource_type_t type);

/* Force a rate class's publication sequence odd ("write in flight") or even
 * ("stable"). The bounded-retry path of quota_rate_limit_get is otherwise
 * unreachable from a single-threaded test -- it needs a publisher held mid-
 * update on another CPU -- yet it carries a real contract (STATUS_RETRY with
 * the caller's buffer untouched) that a regression could silently break. Same
 * test-only-seam pattern as the raw counter pokes above. */
void quota_test_poke_rate_seq(quota_block_t *block, quota_rate_class_t cls, int odd);

/* --- Test-only charge-cost instrumentation -------------------------------- *
 * Counts critical sections COMPLETED while counting is armed -- both the block
 * locks in quota.c and the owner-side locks in quota_owner.c -- so a test can
 * assert the structural budgets above exactly. Both files funnel their
 * releases through one helper, so the count cannot drift from the real lock
 * discipline.
 *
 * WHERE it counts: on the UNLOCK side, after the lock is released, never
 * between acquire and release. The nested two-lock transfer path releases both
 * locks first and records its two sections afterwards, because releasing the
 * inner lock restores flags captured while the outer acquisition already held
 * interrupts masked.
 *
 * That places the work outside the quota lock, but NOT necessarily outside an
 * IRQ-disabled window: spin_unlock_irqrestore restores the CALLER's saved IF,
 * so a caller that was already in interrupt context (which this API explicitly
 * supports) or beneath an outer irqsave lock still has interrupts masked when
 * the count runs. The guarantee is "never inside a quota critical section",
 * not "always with interrupts enabled".
 *
 * WHAT it costs: disarmed, a block unlock adds an inlined relaxed load of the
 * enable byte plus a not-taken branch; an owner unlock additionally pays an
 * out-of-line call, because the helper lives in quota.c. Armed, it adds an
 * IRQL read, a thread-cursor read, and a relaxed read-modify-write. Counting is explicitly gated
 * rather than always-on precisely so the advisory TSC loops time the
 * production path instead of the instrument -- arming it measurably moved the
 * numbers when it was always-on.
 *
 * SCOPE -- read this before trusting a count. Attribution is INVOCATION-scoped:
 * a section counts only when it is released by the same THREAD that armed the
 * window, at PASSIVE_LEVEL. Both halves are load-bearing and each replaced a
 * specific defect of the earlier CPU-scoped rule:
 *
 *   - Thread identity, not CPU identity, is what "this invocation" means. CPU
 *     matching dropped a section whenever the measured thread migrated between
 *     release and attribution, and counted a section that some unrelated task
 *     happened to complete on the armed CPU. Task creation and process death
 *     take owner locks constantly, and every budget assertion is an exact
 *     equality, so either direction is a hard test failure.
 *   - The PASSIVE_LEVEL gate excludes interrupt and DPC context, so a timer or
 *     device interrupt landing mid-window on the measured thread's stack
 *     cannot add a section that the operation under test never performed.
 *
 * A nested quota operation issued BY the measured thread at PASSIVE still
 * counts, and should: it is genuinely part of the invocation being budgeted.
 *
 * NOT SMP-SOUND, and the limit is in the resolver rather than the rule.
 * thread_current() reads the GLOBAL current_task / current_thread cursors, not
 * per-CPU state, so on a real multi-CPU scheduler another CPU can move those
 * cursors between the measured operation and this comparison -- letting an
 * unrelated thread match, or the arming thread be rejected. Thread scoping is
 * strictly better than the CPU scoping it replaced (it survives migration and
 * excludes sibling tasks, both asserted by tests), but genuine invocation
 * scoping needs a CPU-local current-thread identity that does not exist yet.
 * Until then these exact-equality budgets are valid under the single-cursor
 * scheduler the test runner uses, and no further. Owner: per-CPU run queues in
 * 03-memory-concurrency/TODO-07-smp-phase2.md section 3.
 *
 * What remains outside scope is unchanged -- one global gate means two
 * overlapping measurement windows still interfere, and this is a structural
 * lock-count guard, never a latency budget (it sees no hold time, contention,
 * or cache misses).
 *
 * Usage: begin() zeroes, records the arming thread, and arms; end() disarms
 * and returns the total. */
void     quota_test_lock_count_begin(void);
uint64_t quota_test_lock_count_end(void);

/* Instrumentation hook shared with quota_owner.c so owner-side sections land
 * in the same total. Not for test use directly. */
void     quota_test_count_lock_section(void);

/* Byte size of one per-type counter record, so a locality test can report how
 * many cache lines a charge actually touches at the block's runtime address
 * without exposing the private struct. */
uint32_t quota_test_counter_record_bytes(void);

/* Byte offset of the counter array within a block, paired with the record size
 * so a test can compute the real line span from a block's runtime address
 * without the private struct definition. */
uint32_t quota_test_counter_base_offset(void);

/* Cache-line size the implementation's own record-fits-a-line assert uses, so
 * a locality test computes spans against the same bound instead of re-defining
 * 64 and silently drifting if that bound ever changes. */
uint32_t quota_test_counter_line_bytes(void);
#endif /* KERNEL_TESTS */
