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
 * release. PASSIVE_LEVEL only (cooperative yield poll). */
void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r);

/* Mark rundown complete immediately (no outstanding refs); subsequent acquires
 * fail. Use when the owner knows no references are live. */
void ExRundownCompleted(EX_RUNDOWN_REF *r);

/* True once rundown has begun (acquire would fail). */
bool ExIsRundownActive(EX_RUNDOWN_REF *r);

/* ex_ready -- true once the Executive support runtime is marked ready.
 *
 * Delegates to the subsystem readiness oracle (kernel_subsystem_ready(
 * SUBSYS_EX)), which uses an acquire load over a release store and is
 * SMP-safe: APs are already online at this boot stage, so a duplicate
 * plain-bool flag would be a data race. Consumers that come up after Phase 2
 * can assert on this; it is NOT a hot-path gate.
 */
bool ex_ready(void);
