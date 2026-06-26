/* ============================================================================
 * ex.h -- Executive Support Runtime: the Ex* primitive layer
 *
 * The Executive support layer sits ABOVE raw spinlocks/events/scheduler
 * mechanics and BELOW the subsystems that consume it (registry, SRM, ALPC,
 * power, object callbacks, file I/O). It provides the connective-tissue
 * primitives NT drivers expect: rundown protection, callback objects,
 * lookaside lists, fast references, generic tables, locks, work items,
 * and bugcheck reason callbacks.
 *
 * Namespace convention:
 *   Ex*  / Ke* / Rtl*   -- exported APIs (callable from any kernel module).
 *   Exp* / Exi*         -- internal helpers, not part of the stable surface.
 *
 * IRQL contract: every exported API documents its IRQL ceiling in its own
 * declaration comment (PASSIVE_LEVEL / APC_LEVEL / DISPATCH_LEVEL). Until the
 * IRQL/APC/DPC model is fully wired, "PASSIVE_LEVEL only" means "callable
 * from a normal thread context, may block"; "DISPATCH-safe" means "no blocking,
 * no allocation, callable from a DPC/timer/ISR-tail context".
 *
 * ----------------------------------------------------------------------------
 * NO HIDDEN DYNAMIC ALLOCATION ON HOT/SMP PATHS.
 * The kernel pmm/kmalloc allocators are unsynchronized (the same constraint
 * that shaped the kernel-libraries codecs: caller-provided workspace). Every Ex*
 * primitive that needs backing memory MUST use caller-provided storage, a
 * pre-reserved pool, or the tagged pool API once it is synchronized -- never a
 * silent kmalloc() in an acquire/dispatch/refill path. Each primitive states
 * its allocation policy in its own header contract; the executive verifier
 * (S14) checks the per-primitive misuse modes at runtime.
 * ----------------------------------------------------------------------------
 *
 * Ownership boundary (what this layer does and does NOT own):
 *   Owned here:  interlocked SLIST (S2), rundown (S3), callbacks (S4),
 *                lookaside (S5), fast references (S6), generic tables /
 *                dynamic hash / RTL_BITMAP (S7), push locks (S8),
 *                fast/guarded mutexes (S9), ERESOURCE (S10), run-once (S11),
 *                work items + Ex timers (S12), bugcheck reason callbacks (S13),
 *                executive verifier (S14).
 *   Owned elsewhere:
 *     - Pool/tag allocation (ExAllocatePoolWithTag / ExAllocatePool2)
 *         -> 03-memory-concurrency/TODO-03-advanced-allocator (kmalloc_tag).
 *     - System-time conversion helpers (ExSystemTimeToLocalTime)
 *         -> 02-kernel-core/TODO-08-time-filetime-management.
 *     - Status/exception raise (ExRaiseStatus / ExRaiseException)
 *         -> 02-kernel-core/TODO-23-exception-dispatch-seh.
 *     - Broad verifier tooling (KASAN / lockdep class)
 *         -> 02-kernel-core/TODO-31-kernel-bulletproofing.
 *   Deferred (no consumer yet):
 *     - UUID generation (ExUuidCreate) -- until an RPC/ALPC caller needs it.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"

/* ex_init -- bring up the Executive support runtime.
 *
 * Called once on the BSP in boot_phase2(), AFTER the Object Manager (callback
 * objects in S4 are backed by an OM type) and BEFORE registry/security
 * consumers (they acquire ERESOURCE/push locks owned here). Single-threaded at
 * this point (APs are not yet running consumer code), so no locking is needed
 * for the bring-up itself.
 *
 * IRQL: PASSIVE_LEVEL (boot context).
 * Returns BOOT_OK on success. The bring-up has no optional capability to
 * degrade and no fatal failure mode of its own today; downstream primitives
 * (S2-S14) carry their own build-time and boot-time invariant asserts.
 */
boot_result_t ex_init(void);

/* ===========================================================================
 * S2 -- Interlocked SLIST (lock-free LIFO singly-linked list)
 *
 * The free-list spine the lookaside lists (S5) sit on, plus a standalone
 * interlocked queue for drivers. Matches the Windows x64 SLIST surface.
 *
 * Storage contract (NO HIDDEN ALLOCATION): the caller owns every SLIST_ENTRY.
 * An entry must stay MAPPED while the list is live -- a popper may read
 * `entry->Next` from an entry another CPU just popped, so the storage must not
 * be returned to the pmm (freed/unmapped) until the whole list is drained. The
 * value-level ABA hazard is defeated by the sequence counter in SeqDepth; this
 * mapped-while-live rule covers the read-from-reused-memory hazard.
 *
 * Header is a full 16-byte {Next, SeqDepth} swung atomically by cmpxchg16b, so
 * Next is a FULL 64-bit pointer (no 48-bit packing -- higher-half-relocation
 * safe) and Depth is linearizable with the pointer swap (not a side counter).
 * SeqDepth = (seq << 16) | depth16. Requires CPU_FEATURE_CX16 (gated at boot).
 * IRQL: any (lock-free, no blocking, no allocation -- DISPATCH-safe).
 *
 * ALIGNMENT: the head MUST be 16-byte aligned (cmpxchg16b #GPs otherwise). The
 * type carries aligned(16), but kmalloc does NOT honor type alignment, so a
 * header in dynamic storage must be placed on a 16-byte boundary;
 * ExInitializeSListHead bugchecks a misaligned head.
 *
 * DEPTH: 16-bit, EXACT up to SLIST_DEPTH_MAX (65535) entries. Push saturates at
 * MAX so the 65536th push never wraps the count to 0 (which would corrupt the
 * seq field and depth consumers). Beyond 65535 entries the count is an
 * approximate, bounded [0..MAX] value (pops past the cap under-report), so a
 * consumer needing an exact count must keep the list under 65535 -- which every
 * real consumer (lookaside trims long before that) does. The list itself stays
 * valid at any depth; only the reported count degrades past the cap.
 * =========================================================================== */

typedef struct _SLIST_ENTRY {
    struct _SLIST_ENTRY *Next;
} SLIST_ENTRY;

typedef struct _SLIST_HEADER {
    SLIST_ENTRY *Next;       /* current head (full 64-bit pointer) */
    uint64_t     SeqDepth;   /* (ABA_seq << 16) | depth16 */
} __attribute__((aligned(16))) SLIST_HEADER;

_Static_assert(sizeof(SLIST_HEADER) == 16,
               "SLIST_HEADER must be 16 bytes for cmpxchg16b");
_Static_assert(_Alignof(SLIST_HEADER) >= 16,
               "SLIST_HEADER must be 16-byte aligned for cmpxchg16b");

#define SLIST_DEPTH_MAX 0xFFFFu  /* depth field is 16 bits */

/* Initialize an empty list head. */
void ExInitializeSListHead(SLIST_HEADER *head);
void InitializeSListHead(SLIST_HEADER *head);   /* alias */

/* Push `entry` on the head; returns the PREVIOUS head (NULL if list was empty). */
SLIST_ENTRY *ExInterlockedPushEntrySList(SLIST_HEADER *head, SLIST_ENTRY *entry);

/* Pop the head entry; returns it, or NULL if the list was empty. */
SLIST_ENTRY *ExInterlockedPopEntrySList(SLIST_HEADER *head);

/* Detach the whole chain and reset the list to empty; returns the old head
 * (NULL if already empty). The returned chain is walkable via ->Next. */
SLIST_ENTRY *ExInterlockedFlushSList(SLIST_HEADER *head);

/* Current depth (0..SLIST_DEPTH_MAX). Coherent single-snapshot read. */
uint16_t ExQueryDepthSList(SLIST_HEADER *head);

/* ===========================================================================
 * S3 -- Rundown Protection (EX_RUNDOWN_REF)
 *
 * A reference barrier for safe teardown: short-lived users take a rundown
 * reference around their access to a shared object; the owner calls
 * ExWaitForRundownProtectionRelease before freeing, which rejects new
 * references and blocks until all outstanding ones drain. Matches the Windows
 * EX_RUNDOWN_REF surface.
 *
 * Single pointer-sized atomic: bit 0 = rundown-active, bits 1+ = refcount (each
 * outstanding reference is +2). Lock-free acquire/release; the wait is a
 * cooperative PASSIVE_LEVEL poll (the kernel event_t has a lost-wakeup race
 * between its state-read and enqueue, so rundown polls Count instead). No
 * allocation -- the EX_RUNDOWN_REF is caller-owned.
 *
 * IRQL: acquire/release/query are DISPATCH-safe (lock-free, no block); WAIT is
 * PASSIVE_LEVEL only (it yields).
 * =========================================================================== */

typedef struct _EX_RUNDOWN_REF {
    volatile uint64_t Count;   /* bit0 = rundown-active; bits1+ = refcount << 1 */
} EX_RUNDOWN_REF;

/* Arm an empty rundown ref (no refs, not running down). */
void ExInitializeRundownProtection(EX_RUNDOWN_REF *r);

/* Re-arm a ref after a completed rundown so it can be reused. */
void ExReInitializeRundownProtection(EX_RUNDOWN_REF *r);

/* Take a rundown reference. Returns true if acquired; false if rundown has
 * already begun (caller must NOT touch the protected object). DISPATCH-safe. */
bool ExAcquireRundownProtection(EX_RUNDOWN_REF *r);

/* Drop a rundown reference taken by a successful acquire. DISPATCH-safe. */
void ExReleaseRundownProtection(EX_RUNDOWN_REF *r);

/* Begin rundown: reject new acquires, then block until all outstanding refs
 * release. PASSIVE_LEVEL only (cooperative yield poll).
 * PRECONDITION: reference holders must run at >= the waiter's scheduler
 * priority -- a strictly-higher-priority waiter would starve a strictly-lower-
 * priority holder (priority inversion). A priority-inheritance-correct wait is
 * a tracked enhancement; the typical PASSIVE-owner-waits-on-equal/higher-users
 * teardown pattern satisfies the precondition. */
void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r);

/* Mark rundown complete immediately (no outstanding refs); subsequent acquires
 * fail. Use when the owner knows no references are live. */
void ExRundownCompleted(EX_RUNDOWN_REF *r);

/* True once rundown has begun (acquire would fail). */
bool ExIsRundownActive(EX_RUNDOWN_REF *r);

/* ===========================================================================
 * S4 -- Callback Objects (Ex callback objects; \Callback\ namespace)
 *
 * A named notification point: a producer creates/opens an EX_CALLBACK_OBJECT
 * (optionally named under \Callback\), consumers register routines on it, and
 * the producer fires them all via ExNotifyCallback. Matches the Windows
 * ExCreateCallback surface. DISTINCT from the Object Manager operation
 * callbacks (ObRegisterCallbacks in ob_callback.c).
 *
 * Lifetime safety: each registration slot carries its own EX_RUNDOWN_REF (S3).
 * ExNotifyCallback acquires a slot's rundown before invoking it (outside the
 * lock); ExUnregisterCallback marks the slot unregistering and drains ALL
 * outstanding references (including other CPUs') before returning, so a
 * consumer can free its context with no UAF. A monotonic per-slot generation
 * in the cookie prevents a stale cookie from touching a reused slot.
 *
 * CONTRACT: ExUnregisterCallback MUST NOT be called from within a callback
 * routine of the same object (matches Windows) -- it would deadlock waiting on
 * its own in-flight rundown reference. The drain-all-refs path is provably safe
 * for the only legal (non-self) usage. Misuse detection (turning that deadlock
 * into a bugcheck) is owned by the executive verifier (S14).
 *
 * PRECONDITION (inherited from S3 rundown wait): a callback notifier must run
 * at >= the unregistering thread's scheduler priority. Not functional-blocking
 * today (no live producers fire ExNotifyCallback yet).
 *
 * IRQL: PASSIVE_LEVEL (registration, notify dispatch, unregister drain).
 * =========================================================================== */

/* Callback routine: invoked with the registration context + the two notify
 * arguments the producer passes to ExNotifyCallback. */
typedef void (*EX_CALLBACK_ROUTINE)(void *context, void *arg1, void *arg2);

#define EX_CALLBACK_MAX_SLOTS 8   /* registrations per callback object */

typedef struct _EX_CALLBACK_OBJECT EX_CALLBACK_OBJECT;   /* OM-allocated body */

/* Registration cookie = (generation << 8) | slot: an 8-bit slot index in the
 * low byte and a 56-bit generation above it. generation >= 1 so a valid cookie
 * is never 0. Treat as opaque (layout documented only for the verifier/debug). */
typedef uint64_t EX_CALLBACK_COOKIE;

/* Create or open a callback object. `name` non-NULL -> created/opened under
 * \Callback\<name>; `allow_multiple` false caps it at one active registration.
 * Returns the object body or NULL. PASSIVE. */
EX_CALLBACK_OBJECT *ExCreateCallback(const char *name, bool create,
                                     bool allow_multiple);

/* Register `routine`(`context`) on `cb`. Returns a non-zero cookie, or 0 if no
 * slot is free or allow_multiple is violated. PASSIVE. */
EX_CALLBACK_COOKIE ExRegisterCallback(EX_CALLBACK_OBJECT *cb,
                                      EX_CALLBACK_ROUTINE routine, void *context);

/* Unregister the routine identified by `cookie`, draining in-flight dispatch
 * before returning (caller may then free context). No-op on a stale/invalid
 * cookie. MUST NOT be called from within a callback routine (would deadlock; the
 * S14 verifier detects + bugchecks the misuse). PASSIVE. */
void ExUnregisterCallback(EX_CALLBACK_OBJECT *cb, EX_CALLBACK_COOKIE cookie);

/* Invoke every active registration on `cb` with (context, arg1, arg2), each
 * under its slot's rundown so a concurrent unregister drains safely. PASSIVE. */
void ExNotifyCallback(EX_CALLBACK_OBJECT *cb, void *arg1, void *arg2);

/* Snapshot active registrations into out[] (each = the registered context);
 * returns the count written (capped at max). For the verifier (S14)/debug. */
uint32_t ExpEnumerateCallback(EX_CALLBACK_OBJECT *cb, void **out, uint32_t max);

/* ============================================================================
 * S5 -- Lookaside Lists (NPaged, Paged, and unified Ex)
 *
 * Fixed-size block cache layered over a backing allocator. The fast paths
 * (alloc-from-cache, free-to-cache) are the S2 interlocked SLIST and are fully
 * lock-free and DISPATCH/SMP-safe. The cold backing paths (grow on a cache
 * miss, drain an over-cap free) call the backing allocator (kmalloc by default)
 * ONLY at <= APC_LEVEL -- never at DISPATCH_LEVEL, where kmalloc is illegal. The
 * kernel heap is itself globally unsynchronized today, so the backing path is
 * no more SMP-safe than any other kmalloc consumer (-> retrofit to the
 * synchronized tagged pool ExAllocatePoolWithTag, owned by
 * 03-memory-concurrency/TODO-03 advanced-allocator). A nonpaged miss at
 * > APC_LEVEL returns NULL (a legal "lookaside empty" outcome) rather than
 * touch the heap unsafely; a paged op at > APC_LEVEL is rejected (paged memory
 * may fault) -- NULL normally, bugcheck in verifier mode.
 *
 * SMP NOTE: the lock-free SLIST hot paths are fully SMP-safe. The kmalloc
 * backing is only as SMP-safe as the kernel heap, which is globally
 * unsynchronized today (no worse than any other kmalloc consumer) -- that gap
 * closes when the synchronized tagged pool lands (advanced-allocator). Backing
 * is never called above APC_LEVEL, so it never runs kmalloc at DISPATCH_LEVEL.
 *
 * CONCURRENCY CONTRACTS (match Windows interlocked-SLIST semantics):
 *   - Reclaim: an over-cap drain frees a popped entry back to the backing
 *     allocator while the list is live. A concurrent popper may still read that
 *     entry's link word; the SLIST pop tolerates a stale read (its DCAS retries)
 *     ONLY as long as the memory stays mapped. The default kmalloc backing
 *     never unmaps, so this is safe. A custom LOOKASIDE_LIST_EX free_fn MUST
 *     likewise keep freed storage mapped/stable while the list is concurrently
 *     used (do not unmap or repurpose it), or must not be drained concurrently.
 *   - Teardown: Delete/Flush are NOT serialized against in-flight allocate/free.
 *     The caller must quiesce all users of a list before deleting it (and before
 *     freeing the control block). Misuse detection is owned by the verifier.
 *
 * STORAGE: the embedded SLIST_HEADER needs 16-byte alignment for cmpxchg16b,
 * but kmalloc does not honor type alignment, so the control block carries an
 * over-provisioned raw buffer and init points free_list at the aligned slot
 * inside it. An initialized lookaside list therefore must NOT be relocated /
 * copied (free_list points into its own storage).
 * ============================================================================ */

#define EX_LOOKASIDE_DEFAULT_DEPTH  256u    /* default cache cap (entries) */
#define EX_LOOKASIDE_MAX_DEPTH      4096u   /* cap: MUST stay < SLIST_DEPTH_MAX so trim never stalls at saturation */
#define EX_LOOKASIDE_MIN_ALLOC      (sizeof(SLIST_ENTRY))  /* entry holds the link while cached */
#define EX_LOOKASIDE_MAX_ALLOC      4096u   /* per-entry cap: bounds kmalloc + verifier scan, no (size+15) wrap */
#define EX_LOOKASIDE_POISON_BYTE    0xA5u   /* verifier free-poison fill */
#define EX_LOOKASIDE_BUGCHECK_PAGED_IRQL  5u  /* KeBugCheckEx p4: paged op above APC_LEVEL */

_Static_assert(EX_LOOKASIDE_MAX_DEPTH < SLIST_DEPTH_MAX,
               "lookaside depth cap must be below the SLIST saturation point or trim stalls");

/* Custom backing allocator for LOOKASIDE_LIST_EX (NULL pair -> kmalloc/kfree).
 * The custom allocator owns its own SMP-safety; the kmalloc fallback inherits
 * the kernel heap's (currently unsynchronized) behavior. */
typedef void *(*EX_LOOKASIDE_ALLOC)(size_t size, uint32_t tag, void *ctx);
typedef void  (*EX_LOOKASIDE_FREE)(void *block, void *ctx);

/* Common control block embedded by all three public list types. */
typedef struct _EX_LOOKASIDE {
    uint8_t            _slist_raw[sizeof(SLIST_HEADER) + 16]; /* over-provision for 16-align */
    SLIST_HEADER      *free_list;     /* aligned into _slist_raw at init */
    size_t             size;          /* per-entry size (>= EX_LOOKASIDE_MIN_ALLOC) */
    uint32_t           tag;           /* 4-char pool tag (stats + future kmalloc_tag) */
    uint16_t           max_depth;     /* cache cap; over-cap frees drain to backing */
    bool               paged;         /* paged variant -> <= APC_LEVEL ceiling for ALL ops */
    bool               initialized;
    EX_LOOKASIDE_ALLOC alloc_fn;      /* EX custom alloc (NULL -> kmalloc) */
    EX_LOOKASIDE_FREE  free_fn;       /* EX custom free  (NULL -> kfree)  */
    void              *ctx;           /* private context for the custom pair */
    /* Atomic stat counters. Totals are derivable (allocs = alloc_hits +
     * alloc_misses, frees = free_hits + free_drains), so they are NOT stored --
     * one less RMW per hot-path op. */
    uint64_t           alloc_hits;    /* served from cache */
    uint64_t           alloc_misses;  /* served from backing */
    uint64_t           free_hits;     /* returned to cache */
    uint64_t           free_drains;   /* drained to backing (over cap) */
} __attribute__((aligned(16))) EX_LOOKASIDE;

/* The aligned SLIST_HEADER lives inside _slist_raw: aligning up costs <= 15
 * bytes, then the header needs sizeof(SLIST_HEADER). Guard the over-provision
 * so a future shrink of the buffer (or growth of SLIST_HEADER) fails the build
 * instead of silently running free_list off the end of _slist_raw. */
_Static_assert(sizeof(((EX_LOOKASIDE *)0)->_slist_raw) >= 15 + sizeof(SLIST_HEADER),
               "_slist_raw too small to hold a 16-byte-aligned SLIST_HEADER");

typedef struct _NPAGED_LOOKASIDE_LIST { EX_LOOKASIDE L; } NPAGED_LOOKASIDE_LIST;
typedef struct _PAGED_LOOKASIDE_LIST  { EX_LOOKASIDE L; } PAGED_LOOKASIDE_LIST;
typedef struct _LOOKASIDE_LIST_EX     { EX_LOOKASIDE L; } LOOKASIDE_LIST_EX;

/* Nonpaged: usable at <= DISPATCH_LEVEL on the cache fast path; a miss above
 * APC_LEVEL returns NULL (no heap growth at high IRQL). `depth` 0 -> default. */
void  ExInitializeNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l, size_t size,
                                      uint32_t tag, uint16_t depth);
void *ExAllocateFromNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l);
void  ExFreeToNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l, void *entry);
void  ExDeleteNPagedLookasideList(NPAGED_LOOKASIDE_LIST *l);

/* Paged: all operations legal at <= APC_LEVEL only; a DISPATCH_LEVEL caller is
 * rejected (NULL normally, IRQL bugcheck in verifier mode). */
void  ExInitializePagedLookasideList(PAGED_LOOKASIDE_LIST *l, size_t size,
                                     uint32_t tag, uint16_t depth);
void *ExAllocateFromPagedLookasideList(PAGED_LOOKASIDE_LIST *l);
void  ExFreeToPagedLookasideList(PAGED_LOOKASIDE_LIST *l, void *entry);
void  ExDeletePagedLookasideList(PAGED_LOOKASIDE_LIST *l);

/* Unified modern variant (MS-recommended for new code). `alloc_fn`/`free_fn`
 * NULL -> kmalloc/kfree backing; non-NULL -> caller backing with `ctx`. Returns
 * 0 on success, non-zero on bad args. */
int   ExInitializeLookasideListEx(LOOKASIDE_LIST_EX *l, EX_LOOKASIDE_ALLOC alloc_fn,
                                  EX_LOOKASIDE_FREE free_fn, void *ctx, bool paged,
                                  size_t size, uint32_t tag, uint16_t depth);
void *ExAllocateFromLookasideListEx(LOOKASIDE_LIST_EX *l);
void  ExFreeToLookasideListEx(LOOKASIDE_LIST_EX *l, void *entry);
void  ExFlushLookasideListEx(LOOKASIDE_LIST_EX *l);   /* drain cache, keep usable */
void  ExDeleteLookasideListEx(LOOKASIDE_LIST_EX *l);

/* Verifier seam (S14 owns the broader verifier). When enabled, freed entry
 * bodies are poisoned and re-checked on the next alloc; a detected
 * use-after-free increments the UAF counter (LOG_ERROR; S14 escalates). */
void     ExpSetLookasideVerifier(bool on);
uint64_t ExpLookasideUafCount(void);

/* ============================================================================
 * S6 -- Fast References (EX_FAST_REF)
 *
 * A pointer-sized atomic that caches a small batch of Object Manager references
 * inline so a hot lookup path can hand out a reference WITHOUT touching the
 * object's contended refcount on every access. Matches the Windows EX_FAST_REF
 * surface (ExfAcquireFastReference / ExfReleaseFastReference) but adapted to
 * this kernel's allocator alignment (see ALIGNMENT below).
 *
 * LAYOUT: the single Value word packs `(object & ~EX_FAST_REF_MASK) | count`,
 * where `count` is the number of cached references currently available to hand
 * out (0..EX_FAST_REF_MAX). The fast-ref structure logically owns `count`
 * references on the object PLUS one structural reference for storing the
 * pointer -- (count + 1) total, all held by the caller before init.
 *
 * ALIGNMENT (why 3 bits / max 7, not Windows' x64 4 bits / max 15): this
 * kernel's kmalloc guarantees only 8-byte alignment (page-aligned heap base +
 * 24-byte block header => 8-mod-16 payloads), and Object Manager bodies inherit
 * that floor (OBJECT_HEADER is 16-aligned, so body alignment == block
 * alignment == 8-byte worst case). Only the low 3 bits are reliably zero, so
 * the cached count is 3 bits (0..7) -- the x86 (32-bit) Windows model, not the
 * x64 4-bit model. Requiring 16-byte alignment would bugcheck on real objects.
 * Init/swap runtime-assert (object & EX_FAST_REF_MASK) == 0.
 *
 * CONTRACT (caller owns the Ob ref/deref; this layer owns only the packing):
 *   - ExAcquireFastReference returns {object, cached}. cached==true: a cached
 *     reference was handed out lock-free (fast path), caller owns it and must
 *     ObDereferenceObject when done. This fast path needs NO lock: the CAS
 *     re-checks the object bits, so a concurrent swap just forces a retry. The
 *     cached count handed out is a real reference the fast ref owned, and
 *     reference accounting stays balanced against a concurrent swap (the swap
 *     surfaces the exact count it swapped out). cached==false && object!=NULL:
 *     the cache was empty -- SLOW PATH (see the LOCK RULE below). object==NULL:
 *     the fast ref is empty.
 *   - ExReleaseFastReference returns true if the reference was absorbed back
 *     into the cache (caller must NOT deref). It returns false in two distinct
 *     cases: (a) for a valid NON-NULL `object` the cache is saturated
 *     (count==MAX) or holds a different object -- the caller MUST then
 *     ObDereferenceObject the reference it holds (this is the intended
 *     steady-state fallback, not an error); (b) `object` is NULL -- invalid
 *     input, NO reference was consumed and the caller must NOT dereference
 *     anything (a caller only ever releases an object it acquired, which is
 *     non-NULL; the NULL guard exists only to reject a degraded/error path).
 *     A correct caller therefore derefs on false only when it passed a non-NULL
 *     object: `if (obj && !ExReleaseFastReference(ref, obj)) ObDeref(obj);`.
 *
 * LOCK RULE (load-bearing -- the slow path is NOT lock-free):
 *   The cnt==0 slow path, ExCompareSwapFastReference, and object teardown MUST
 *   all be serialized by ONE per-object lock the consuming subsystem owns (the
 *   token lock, the handle-table lock, etc. -- the same way Windows wraps
 *   EX_FAST_REF slow paths). The fast path above is the only lock-free op.
 *   Rationale: the structural reference keeps the object alive ONLY while the
 *   pointer is stored, and ExCompareSwapFastReference is exactly what un-stores
 *   it (then the swap's caller balances the old references and may free the
 *   object). If a lock-free swap could race the slow path, the slow-path caller
 *   would hold a raw pointer to freed memory before its ObReferenceObjectSafe.
 *   Therefore the slow path MUST, under the lock: (1) re-read the stored object
 *   with ExGetObjectFastReference, (2) confirm it still equals the object it
 *   intends to reference, (3) only then ObReferenceObjectSafe(object) and
 *   optionally replenish the cache. Holding the lock excludes swap/teardown, so
 *   the structural reference provably still pins the object across the safe-ref.
 *
 * NO global state, NO init hook, NO hidden allocation: every op is a single-word
 * CAS over a caller-owned word. Replenish + Ob ref/deref are caller-owned
 * (matches Windows, which splits ExfAcquireFastReference from ObfReferenceObject
 * so only the consuming subsystem -- which owns the relevant lock -- replenishes).
 *
 * IRQL: all ops are DISPATCH-safe (lock-free, no block, no allocation). The
 * caller's slow-path Ob ref/deref carry their own IRQL rules.
 * =========================================================================== */

#define EX_FAST_REF_BITS  3u                              /* 8-byte align => 3 free low bits */
#define EX_FAST_REF_MASK  ((uintptr_t)((1u << EX_FAST_REF_BITS) - 1u))  /* 0x7 */
#define EX_FAST_REF_MAX   ((uintptr_t)EX_FAST_REF_MASK)   /* max cached count = 7 */

typedef struct _EX_FAST_REF {
    volatile uintptr_t Value;   /* (object & ~EX_FAST_REF_MASK) | cached_count */
} EX_FAST_REF;

_Static_assert(sizeof(EX_FAST_REF) == sizeof(void *),
               "EX_FAST_REF must be pointer-sized to fit inline in objects");
_Static_assert(EX_FAST_REF_MASK == 0x7u,
               "EX_FAST_REF count field is 3 bits (8-byte object alignment)");

/* Result of a fast acquire: object pointer plus whether a cached reference was
 * handed out (fast path) or the caller must take the slow Ob-reference path. */
typedef struct _EX_FAST_REF_RESULT {
    void *object;   /* stored object, or NULL if the fast ref was empty */
    bool  cached;   /* true: cached ref handed out; false + object!=NULL: slow path */
} EX_FAST_REF_RESULT;

/* Pure packing helpers (exposed for the verifier/tests; treat Value as opaque).
 * ExpFastRefPack bugchecks a misaligned object or an out-of-range count. */
uintptr_t ExpFastRefPack(void *object, uintptr_t count);
void     *ExpFastRefUnpackObject(uintptr_t value);
uintptr_t ExpFastRefUnpackCount(uintptr_t value);

/* Initialize a fast ref with `object` (or NULL for empty), cached count 0.
 * PRECONDITION: when object != NULL the caller already holds 1 Ob reference on
 * it (the structural reference). Runtime-asserts object alignment. */
void ExInitializeFastReference(EX_FAST_REF *ref, void *object);

/* Lock-free acquire. See CONTRACT above for the {object, cached} semantics. */
EX_FAST_REF_RESULT ExAcquireFastReference(EX_FAST_REF *ref);

/* Lock-free release of one reference on `object`. true: absorbed into the cache
 * (do NOT deref). false has two cases (see the full CONTRACT above): a non-NULL
 * `object` that is saturated or mismatched -> caller MUST ObDereferenceObject
 * it; a NULL `object` -> invalid input, no reference consumed, deref nothing.
 * Correct pattern: `if (obj && !ExReleaseFastReference(ref, obj)) ObDeref(obj);` */
bool ExReleaseFastReference(EX_FAST_REF *ref, void *object);

/* Atomically replace the stored object with `new_object` (cached count reset to
 * 0) iff the currently stored object == `old_object`. On success returns true
 * and writes the swapped-out object's cached count to *out_old_count (so the
 * caller can balance (count + 1) references on the old object). On mismatch
 * returns false and leaves the ref unchanged. new_object may be NULL (clears the
 * ref); a non-NULL new_object is runtime-asserted aligned and the caller must
 * already hold 1 structural reference on it. `out_old_count` is MANDATORY when
 * `old_object` is non-NULL (bugchecks if NULL) -- dropping the swapped-out count
 * would leak the old object's cached references; it is optional only for a NULL
 * `old_object` swap (comparing against an empty ref, nothing to balance). */
bool ExCompareSwapFastReference(EX_FAST_REF *ref, void *new_object,
                                void *old_object, uintptr_t *out_old_count);

/* Snapshot the currently stored object pointer WITHOUT consuming a cached
 * reference (single aligned load). The caller must already hold a reference or
 * a lock pinning the object to use the result safely. */
void *ExGetObjectFastReference(EX_FAST_REF *ref);

/* ============================================================================
 * S7 -- Generic Tables (AVL), Dynamic Hash Table, and Bitmaps (Rtl namespace)
 *
 * Three reusable container primitives that replace ad-hoc sorted arrays and
 * hand-rolled bit vectors across kernel-core (atom tables, loaded-image
 * registry, tunable registry, named notification states, handle/PFN bit
 * vectors). All three follow the Windows Rtl semantics: they are CALLER-
 * SERIALIZED (NOT internally locked -- the consumer owns synchronization, same
 * as Windows RtlAvl / RtlBitMap) and they NEVER hide an allocation:
 *   - RTL_BITMAP: the bit buffer is caller-owned; the struct stores a pointer
 *     to it, never allocates.
 *   - RTL_AVL_TABLE: per-element nodes come from a caller-provided allocate/free
 *     callback pair (the table copies the element into the node).
 *   - RTL_DYNAMIC_HASH_TABLE: entries are caller-owned (the caller embeds an
 *     RTL_DYNAMIC_HASH_TABLE_ENTRY link in its own struct); the bucket directory
 *     grows/shrinks via a MANDATORY caller-provided allocate/free pair (no
 *     kmalloc fallback -- "no hidden allocation" is literal), resize is
 *     PASSIVE_LEVEL only and fails closed (table unchanged, entries stable).
 * ============================================================================ */

/* --- RTL_BITMAP: general bit-vector over a caller-owned uint32_t buffer ----
 * Bit i lives in Buffer[i / 32] bit (i % 32) (little-endian within the word).
 * Bits at indices >= SizeOfBitMap are PADDING in the final word: every whole-
 * word read masks them off, and every range/find op is bounded by SizeOfBitMap,
 * so padding can never be counted, returned in a run, or mutated. Caller-
 * serialized; not thread-safe (matches Windows RtlBitMap). */

#define RTL_BITMAP_BITS_PER_WORD  32u
#define RTL_BITMAP_NOT_FOUND      0xFFFFFFFFu   /* Find* "no run" sentinel */

/* Words needed to hold `bits` bits (the caller sizes its Buffer to this).
 * Overflow-free: the naive (bits + 31) / 32 wraps for bits > UINT32_MAX - 31. */
#define RTL_BITMAP_WORDS(bits) \
    (((bits) / RTL_BITMAP_BITS_PER_WORD) + \
     (((bits) & (RTL_BITMAP_BITS_PER_WORD - 1u)) ? 1u : 0u))

/* Pin the overflow-freedom at compile time: the naive (bits+31)/32 wraps to 0
 * at UINT32_MAX, this form yields the correct 2^27 words. */
_Static_assert(RTL_BITMAP_WORDS(0xFFFFFFFFu) == 0x8000000u,
               "RTL_BITMAP_WORDS must not overflow near UINT32_MAX");

typedef struct _RTL_BITMAP {
    uint32_t  SizeOfBitMap;   /* number of valid bits */
    uint32_t *Buffer;         /* caller-owned, RTL_BITMAP_WORDS(SizeOfBitMap) words */
} RTL_BITMAP;

/* Bind the bitmap to a caller-owned buffer of RTL_BITMAP_WORDS(size_bits) words.
 * Does NOT zero the buffer (matches Windows) -- call RtlClearAllBits to start
 * empty. size_bits 0 is legal (an empty bitmap; all Find* return NOT_FOUND). */
void RtlInitializeBitMap(RTL_BITMAP *bm, uint32_t *buffer, uint32_t size_bits);

void RtlClearAllBits(RTL_BITMAP *bm);   /* all valid bits -> 0 (padding stays 0) */
void RtlSetAllBits(RTL_BITMAP *bm);     /* all valid bits -> 1 (padding stays 0) */

/* Single-bit ops. Out-of-range index is a no-op (set/clear) / false (test). */
void RtlSetBit(RTL_BITMAP *bm, uint32_t bit);
void RtlClearBit(RTL_BITMAP *bm, uint32_t bit);
bool RtlTestBit(const RTL_BITMAP *bm, uint32_t bit);

/* Range ops over [start, start+count). Out-of-range range is a no-op (set/clear)
 * / false (AreBits*). count 0 is a no-op / true. */
void RtlSetBits(RTL_BITMAP *bm, uint32_t start, uint32_t count);
void RtlClearBits(RTL_BITMAP *bm, uint32_t start, uint32_t count);
bool RtlAreBitsSet(const RTL_BITMAP *bm, uint32_t start, uint32_t count);
bool RtlAreBitsClear(const RTL_BITMAP *bm, uint32_t start, uint32_t count);

uint32_t RtlNumberOfSetBits(const RTL_BITMAP *bm);    /* popcount of valid bits */
uint32_t RtlNumberOfClearBits(const RTL_BITMAP *bm);

/* Find the first run of `count` contiguous clear/set bits at index >= the search
 * order (starting at `hint`, then wrapping to 0). Returns the run's start index,
 * or RTL_BITMAP_NOT_FOUND if no such run fits within [0, SizeOfBitMap). A run
 * never includes a padding bit. count 0 returns min(hint, SizeOfBitMap). */
uint32_t RtlFindClearBits(const RTL_BITMAP *bm, uint32_t count, uint32_t hint);
uint32_t RtlFindSetBits(const RTL_BITMAP *bm, uint32_t count, uint32_t hint);

/* find-a-clear-run-then-set-it (the index-allocator use); returns the start or
 * NOT_FOUND (and sets nothing on NOT_FOUND). Symmetric find-set-then-clear too. */
uint32_t RtlFindClearBitsAndSet(RTL_BITMAP *bm, uint32_t count, uint32_t hint);
uint32_t RtlFindSetBitsAndClear(RTL_BITMAP *bm, uint32_t count, uint32_t hint);

/* --- RTL_AVL_TABLE: balanced (AVL) generic ordered table ------------------
 * Stores caller elements keyed by a caller compare routine, balanced so height
 * stays <= 1.44*log2(n+2). Per-element nodes (RTL_BALANCED_LINKS header + a copy
 * of the element) come from the caller's allocate/free pair. Caller-serialized. */

typedef enum _RTL_GENERIC_COMPARE_RESULTS {
    RtlGenericLessThan = 0,
    RtlGenericGreaterThan = 1,
    RtlGenericEqual = 2,
} RTL_GENERIC_COMPARE_RESULTS;

struct _RTL_AVL_TABLE;

/* Compare two elements (`first`/`second` point at element bodies). */
typedef RTL_GENERIC_COMPARE_RESULTS (*RTL_AVL_COMPARE_ROUTINE)(
    struct _RTL_AVL_TABLE *table, void *first, void *second);
/* Allocate a node of `size` bytes (= header + element); return NULL on failure. */
typedef void *(*RTL_AVL_ALLOCATE_ROUTINE)(struct _RTL_AVL_TABLE *table, uint32_t size);
/* Free a node previously returned by the allocate routine. */
typedef void (*RTL_AVL_FREE_ROUTINE)(struct _RTL_AVL_TABLE *table, void *buffer);

/* AVL node header; the element body immediately follows (8-byte aligned). The
 * subtree Height is cached so rotations recompute balance from children in O(1)
 * without the error-prone running balance-factor arithmetic (leaf Height = 1,
 * empty subtree = 0; balance = Height(right) - Height(left), kept in [-1,+1]). */
typedef struct _RTL_BALANCED_LINKS {
    struct _RTL_BALANCED_LINKS *Parent;
    struct _RTL_BALANCED_LINKS *LeftChild;
    struct _RTL_BALANCED_LINKS *RightChild;
    int32_t  Height;         /* cached subtree height */
    int32_t  _pad;           /* pad so the element body is 8-byte aligned */
} RTL_BALANCED_LINKS;

/* The element body sits at (node + sizeof(RTL_BALANCED_LINKS)); that offset must
 * be 8-byte aligned or a caller element with 8-byte members faults on ARM64. */
_Static_assert((sizeof(RTL_BALANCED_LINKS) % 8u) == 0u,
               "AVL element body must be 8-byte aligned");

typedef struct _RTL_AVL_TABLE {
    RTL_BALANCED_LINKS      *Root;        /* NULL when empty */
    uint32_t                 NumberOfElements;
    uint32_t                 _pad;
    RTL_AVL_COMPARE_ROUTINE  CompareRoutine;
    RTL_AVL_ALLOCATE_ROUTINE AllocateRoutine;
    RTL_AVL_FREE_ROUTINE     FreeRoutine;
    void                    *TableContext; /* opaque caller cookie */
    /* In-order enumeration cursor (RtlEnumerateGenericTableAvl). */
    RTL_BALANCED_LINKS      *EnumNext;
} RTL_AVL_TABLE;

void RtlInitializeGenericTableAvl(RTL_AVL_TABLE *table,
                                  RTL_AVL_COMPARE_ROUTINE compare,
                                  RTL_AVL_ALLOCATE_ROUTINE allocate,
                                  RTL_AVL_FREE_ROUTINE free, void *context);

/* Insert a COPY of `buffer` (size bytes). Returns a pointer to the stored
 * element body (caller may mutate non-key fields in place). If an equal element
 * exists, returns the existing body and does NOT insert. *new_element (if non-
 * NULL) reports whether a new node was created. Returns NULL only on allocate
 * failure. */
void *RtlInsertElementGenericTableAvl(RTL_AVL_TABLE *table, void *buffer,
                                      uint32_t size, bool *new_element);
/* Remove the element equal to `buffer`. Returns true if found+removed. */
bool RtlDeleteElementGenericTableAvl(RTL_AVL_TABLE *table, void *buffer);
/* Return the stored element body equal to `buffer`, or NULL. */
void *RtlLookupElementGenericTableAvl(RTL_AVL_TABLE *table, void *buffer);
/* In-order enumeration: restart=true starts at the smallest element; each call
 * returns the next element body, NULL at the end. The table must not be mutated
 * mid-enumeration. */
void *RtlEnumerateGenericTableAvl(RTL_AVL_TABLE *table, bool restart);
uint32_t RtlNumberGenericTableElementsAvl(const RTL_AVL_TABLE *table);
bool RtlIsGenericTableEmptyAvl(const RTL_AVL_TABLE *table);

/* --- RTL_DYNAMIC_HASH_TABLE: caller-owned chained entries, resizable -------
 * The caller embeds an RTL_DYNAMIC_HASH_TABLE_ENTRY in its own struct and owns
 * the entry storage; the table owns only the bucket directory, GROWN (never
 * shrunk) via a MANDATORY caller allocate/free pair at PASSIVE_LEVEL. Grow-only
 * is deliberate: a shrink would rehash and reorder same-signature chains and
 * break an in-flight remove-current cursor walk (see RtlGetNextEntryHashTable);
 * the directory is reclaimed wholesale at RtlDeleteDynamicHashTable.
 * Caller-serialized. */

typedef struct _RTL_DYNAMIC_HASH_TABLE_ENTRY {
    struct _RTL_DYNAMIC_HASH_TABLE_ENTRY *Next;   /* bucket chain link */
    uint64_t Signature;                            /* caller's hash key */
} RTL_DYNAMIC_HASH_TABLE_ENTRY;

struct _RTL_DYNAMIC_HASH_TABLE;
/* Allocate `size` bytes for the bucket directory; NULL on failure. */
typedef void *(*RTL_HASH_ALLOCATE_ROUTINE)(struct _RTL_DYNAMIC_HASH_TABLE *t, uint32_t size);
typedef void  (*RTL_HASH_FREE_ROUTINE)(struct _RTL_DYNAMIC_HASH_TABLE *t, void *buffer);

/* Walk cursor for RtlLookupEntryHashTable / RtlGetNextEntryHashTable. It caches
 * the NEXT entry to examine (captured eagerly when the current match is
 * returned), so removing the JUST-RETURNED entry between calls is safe (the
 * cached pointer is its successor, which a removal does not move) -- the common
 * "look up all matches and remove each" pattern works. INVALIDATION: inserting
 * into the table during a walk (it may rehash and relocate every entry) or
 * removing/freeing an entry OTHER than the just-returned one invalidates the
 * cursor; complete the walk first or restart it. */
typedef struct _RTL_HASH_TABLE_CONTEXT {
    RTL_DYNAMIC_HASH_TABLE_ENTRY *NextEntry;  /* next chain entry to examine */
    uint64_t                      Signature;  /* signature being matched */
} RTL_HASH_TABLE_CONTEXT;

typedef struct _RTL_DYNAMIC_HASH_TABLE {
    RTL_DYNAMIC_HASH_TABLE_ENTRY **Directory; /* bucket array, BucketCount slots */
    uint32_t                       BucketCount;   /* power of two */
    uint32_t                       NumEntries;
    RTL_HASH_ALLOCATE_ROUTINE      Allocate;
    RTL_HASH_FREE_ROUTINE          Free;
    void                          *Context;
} RTL_DYNAMIC_HASH_TABLE;

/* Initialize with a MANDATORY allocate/free pair and an initial bucket count
 * (rounded up to a power of two, min 1). Allocates the initial directory via
 * `allocate`. Returns 0 on success, non-zero on bad args / allocate failure. */
int RtlInitializeDynamicHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                                  RTL_HASH_ALLOCATE_ROUTINE allocate,
                                  RTL_HASH_FREE_ROUTINE free, void *context,
                                  uint32_t initial_buckets);
/* Insert a caller-owned entry under `signature`. Returns 0 on success. May grow
 * the directory (PASSIVE); a grow-allocate failure still inserts (table stays at
 * the old size, just denser) so insert never fails for a valid entry.
 * PRECONDITION: `entry` must NOT already be in the table -- re-inserting a live
 * entry would form a self-cycle in the bucket chain (the caller-serialized Rtl
 * contract; a per-insert chain scan to defend it would cost the hot path, so the
 * caller owns this, exactly as Windows does). Remove before re-inserting. */
int RtlInsertEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                            RTL_DYNAMIC_HASH_TABLE_ENTRY *entry, uint64_t signature);
/* Remove a previously-inserted entry. Returns true if found+removed. Never
 * resizes (grow-only table), so removing the just-returned entry during a
 * Lookup/GetNext walk is safe. */
bool RtlRemoveEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                             RTL_DYNAMIC_HASH_TABLE_ENTRY *entry);
/* First entry whose Signature == signature; NULL if none. `ctx` (non-NULL) is
 * seeded for RtlGetNextEntryHashTable to walk the rest of the collision chain. */
RTL_DYNAMIC_HASH_TABLE_ENTRY *RtlLookupEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                                                      uint64_t signature,
                                                      RTL_HASH_TABLE_CONTEXT *ctx);
/* Next entry in the same-signature chain seeded by RtlLookupEntryHashTable. */
RTL_DYNAMIC_HASH_TABLE_ENTRY *RtlGetNextEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                                                       RTL_HASH_TABLE_CONTEXT *ctx);
uint32_t RtlNumberOfEntriesHashTable(const RTL_DYNAMIC_HASH_TABLE *t);
/* Release the bucket directory via the free routine (entries are caller-owned
 * and untouched). The table is empty/unusable afterward. */
void RtlDeleteDynamicHashTable(RTL_DYNAMIC_HASH_TABLE *t);

/* ex_ready -- true once the Executive support runtime is marked ready.
 *
 * Delegates to the subsystem readiness oracle (kernel_subsystem_ready(
 * SUBSYS_EX)), which uses an acquire load over a release store and is
 * SMP-safe: APs are already online at this boot stage, so a duplicate
 * plain-bool flag would be a data race. Consumers that come up after Phase 2
 * can assert on this; it is NOT a hot-path gate.
 */
bool ex_ready(void);
