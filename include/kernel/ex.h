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

/* ex_ready -- true once the Executive support runtime is marked ready.
 *
 * Delegates to the subsystem readiness oracle (kernel_subsystem_ready(
 * SUBSYS_EX)), which uses an acquire load over a release store and is
 * SMP-safe: APs are already online at this boot stage, so a duplicate
 * plain-bool flag would be a data race. Consumers that come up after Phase 2
 * can assert on this; it is NOT a hot-path gate.
 */
bool ex_ready(void);
