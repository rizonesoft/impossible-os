---
schema_version: 1
id: executive-support-runtime
domain: 02-kernel-core
status: active
title: "TODO-06 -- Executive Support Runtime"
---

# TODO-06 -- Executive Support Runtime

> **Validated:** 2026-06-25 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-06-25 | gap-audit + codex-gap-audit; 4 new sections (SLIST, push locks, fast/guarded mutexes, run-once) + 6 inline expansions filed

> **Goal:** Complete the NT Executive-style support layer that sits above raw locks and scheduler mechanics: callback objects, rundown protection, lookaside lists, fast references, generic tables, resource objects, worker items, guarded regions, executive timers glue, bugcheck reason callbacks, and reusable verifier hooks. These primitives are small individually, but they are the connective tissue required by drivers, registry, SRM, ALPC, power, object callbacks, and file I/O.

> [!IMPORTANT]
> **Current state:** The tree has individual spinlocks, events, mutexes, condition variables, semaphores, workqueues, and DPCs. There is no coherent `Ex*` support API surface, no callback object type, no rundown protection, no NPaged lookaside lists, no fast-reference helper, no generic table implementation, and no central verifier hooks.

## Inputs

- [`src/kernel/sched`](../../src/kernel/sched/)
- [`src/kernel/ob`](../../src/kernel/ob/)
- [`include/kernel`](../../include/kernel/)
- → XREF: [`TODO-05-object-manager.md`](./TODO-05-object-manager.md) -- object callbacks and object type registration
- → XREF: [`TODO-07-irql-model-dpcs.md`](./TODO-07-irql-model-dpcs.md) -- IRQL/APC/DPC rules; KeEnterCriticalRegion/KeEnterGuardedRegion for push-lock and guarded-mutex acquire (§8, §9)
- → XREF: [`03-memory-concurrency/TODO-08-advanced-sync.md`](../03-memory-concurrency/TODO-08-advanced-sync.md) -- low-level lock algorithms; user-facing call_once complements kernel RTL_RUN_ONCE (§11)
- → XREF: [`03-memory-concurrency/TODO-03-advanced-allocator.md`](../03-memory-concurrency/TODO-03-advanced-allocator.md) -- `kmalloc_tag`/`ExAllocatePoolWithTag`/`ExAllocatePool2` pool-tag allocation that lookaside refill and callback/AVL alloc paths consume (§5)
- → XREF: [`TODO-31-kernel-bulletproofing.md`](./TODO-31-kernel-bulletproofing.md) -- invariants for fast refs and callback lists

## Outcome

- Kernel code can use stable `Ex*` primitives instead of inventing one-off lists and callbacks.
- Callback registration/unregistration is reference-safe and survives concurrent dispatch.
- Rundown protection safely tears down objects with active users.
- Interlocked SLIST gives a lock-free LIFO spine for lookaside free-lists and ad-hoc queues.
- Lookaside lists (NPaged, Paged, and the unified `LOOKASIDE_LIST_EX`) reduce hot-path allocation churn for registry, ALPC, objects, and I/O.
- Generic AVL tables, dynamic hash tables, and bitmaps replace ad-hoc sorted arrays and hand-rolled bit vectors.
- A spectrum of locks (ERESOURCE shared/exclusive, push locks, fast/guarded mutexes) lets callers pick the lightest correct primitive.
- Run-once initialization gives drivers a safe one-time-init primitive instead of ad-hoc flags.
- Verifier instruments Executive primitives with low runtime cost (scoped instrumentation, not a Driver Verifier / KASAN replacement).

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | Executive headers and namespace | -- | [x] |
| 💎 | 2 | Interlocked SLIST | §1, atomics | [ ] |
| 💎 | 3 | Rundown protection | §1, atomics, events | [ ] |
| 💎 | 4 | Callback objects | §1, §3, T05, T07 | [ ] |
| 💎 | 5 | Lookaside lists (NPaged/Paged/Ex) | §2, D03 T03 | [ ] |
| 💎 | 6 | Fast references | §1, T05 | [ ] |
| 💎 | 7 | Generic tables and bitmaps | §1 | [ ] |
| 💎 | 8 | Push locks | §1, T07 | [ ] |
| 💎 | 9 | Fast and guarded mutexes | §1, T07 | [ ] |
| 💎 | 10 | Executive resource wrapper | §1, T07, D03 T08 | [ ] |
| 💎 | 11 | Run-once initialization | §1 | [ ] |
| ⭐ | 12 | Worker items, delayed work, and Ex timers | §1, T07, DPC/workqueue | [ ] |
| 💎 | 13 | Bugcheck reason callbacks | §3, T27 | [ ] |
| ⭐ | 14 | Executive verifier hooks | §2..§13 | [ ] |

## 1. Executive Headers and Namespace

- [x] Add `include/kernel/ex.h` and `src/kernel/ex/` (`ex.c`). `ex.h` carries the layer doc + `ex_init`/`ex_ready` decls.
- [x] Prefix exported APIs with `Ex`, internal helpers with `Exp` -- documented as the namespace convention in `ex.h`.
- [x] Document the IRQL contract convention for every API (PASSIVE/APC/DISPATCH ceiling) in `ex.h`.
- [x] Add `ex_init()` boot hook in `boot_phase2()` (`boot_storage.c`) after `ob_init()` and before `registry_init()`; `SUBSYS_EX`=27 + `POST16_EX`/`POST16_EX_OK` 0x20C0/0x20C1 + `s_subsys_names` "EX" row.
- [x] Record the RTL/Ex utility include/exclude boundary in the `ex.h` doc-comment so the layer is not falsely "complete":
  - Owned here: SLIST (§2), lookaside (§5), generic tables/dynamic hash/RTL_BITMAP (§7), push locks (§8), fast/guarded mutexes (§9), ERESOURCE (§10), run-once (§11).
  - Owned elsewhere: pool/tag allocation -> XREF D03 T03 (item: "`ExAllocatePool2(pool_type, size, tag)` Win32 wrapper"); system-time helpers (`ExSystemTimeToLocalTime`) -> XREF T08; status/exception raise (`ExRaiseStatus`) -> XREF T23.
  - Deferred: UUID generation (`ExUuidCreate`) until an RPC/ALPC consumer needs it.
- [x] Commit: `"kernel: ex -- executive headers and namespace"`

> [!WARNING]
> **No hidden dynamic allocation on hot/SMP paths.** The kernel `pmm`/`kmalloc` allocators are unsynchronized (same constraint that shaped D02 T03 kernel-libraries). Every `Ex*` primitive that needs backing memory must use caller-provided storage, a pre-reserved pool, or the tagged pool API once synchronized -- never a silent `kmalloc` in an acquire/dispatch/refill path. Document each primitive's allocation policy in its header contract.

**Test checkpoint:** Build links the new `src/kernel/ex/` objects; `ex_init()` logs `"ex: initialized"` on serial at Phase 2 (after `ob_init`, before registry/security); POST16 entry/exit codes appear in order.

> **Test runner:** N/A (boot-path init; `ex_init` is a live-boot call, `ex_ready` reflects the subsystem oracle) | validation: smoke test serial `"ex: Executive support runtime initialized"` at 1.190s + `[PHASE2] EX (0x20C1)`; primitive suites land with `test_ex.c` in §2+.
> **Notes:**
> - What shipped: `include/kernel/ex.h` (layer doctrine: Ex*/Exp* namespace, per-API IRQL convention, no-hidden-allocation contract, ownership boundary) + `src/kernel/ex/ex.c` (`ex_init`, `ex_ready`).
> - How it runs / integrates: `ex_init()` called once on BSP in `boot_phase2()` between `ob_init()` and `registry_init()`; `SUBSYS_EX`=27, `POST16_EX` 0x20C0; build auto-discovers `src/kernel/ex/*.c`.
> - Downstream effects: establishes the boot point + ownership boundary all of §2-§14 build on; `ex_ready()` delegates to the atomic subsystem oracle (Codex adversarial fix -- no duplicate flag/race).
> - Canonical doc: [`include/kernel/ex.h`](../../include/kernel/ex.h).
> - Scope boundary: §1 owns only the scaffold + init hook; the primitives (SLIST..verifier) are §2-§14; pool/time/exception utilities owned by D03 T03 / T08 / T23.
> **Verified:** 2026-06-25 | commit `7b6f5b00` | 5/5 items | build OK | smoke PASS (TCG 2.5s)
> **Quality reviewed:** 2026-06-25 | Codex 6x (design, adversarial x2, re-adversarial, consistency, perf) | 1H+2M fixed | scope: kernel-code-quality

---

## 2. Interlocked SLIST

Lock-free LIFO singly-linked list (`SLIST_HEADER`); the free-list spine that the lookaside lists in §5 sit on, and a standalone interlocked queue for drivers.

- [ ] Define `SLIST_HEADER` (16-byte aligned on x64) and `SLIST_ENTRY` in `include/kernel/ex.h` per the Windows SLIST layout.
- [ ] Implement `ExInitializeSListHead`/`InitializeSListHead`, `ExInterlockedPushEntrySList`, `ExInterlockedPopEntrySList`, `ExInterlockedFlushSList`, `ExQueryDepthSList`.
- [ ] Use a CAS loop (128-bit `cmpxchg16b` on x64, or aligned 64-bit pack) so push/pop are correct under SMP without a lock; assert 16-byte alignment of the header.
- [ ] No hidden allocation: callers own the `SLIST_ENTRY` storage.
- [ ] Commit: `"kernel: ex -- interlocked SLIST"`

**Test checkpoint:** Concurrent push/pop from multiple CPUs preserves every entry (no lost/duplicated nodes across N iterations); `ExQueryDepthSList` matches the net push-minus-pop count; flush returns the whole chain and resets depth to 0. Verify on bare metal -- `cmpxchg16b` ABA behavior and alignment faults differ from VMs.

---

## 3. Rundown Protection

- [ ] Implement `EX_RUNDOWN_REF` with acquire, release, wait, reinitialize, and completed query.
- [ ] Guarantee acquire fails after rundown begins.
- [ ] Wait path must block only at PASSIVE_LEVEL.
- [ ] Convert ALPC ports, section views, process objects, and object callbacks to use rundown where teardown races exist.
- [ ] Commit: `"kernel: ex -- rundown protection"`

**Test checkpoint:** Acquire after rundown begins returns FALSE; `ExWaitForRundownProtectionRelease` unblocks only after every outstanding ref releases; reinitialize re-arms a completed ref. Verify on bare metal -- SMP acquire/release race timing differs from VMs.

---

## 4. Callback Objects

- [ ] Add `ExCreateCallback`, `ExRegisterCallback`, `ExUnregisterCallback`, `ExNotifyCallback`.
- [ ] Back callback objects with Object Manager type `Callback`.
- [ ] Support named callbacks under `\Callback\`.
- [ ] Built-in callbacks: process create, thread create, image load, registry change, power setting, code integrity decision.
- [ ] Dispatch callbacks under rundown protection (§3) so unregister can wait safely; document the IRQL ceiling and forbidden reentrancy (-> XREF T07 for APC/DISPATCH rules).
- [ ] Add `ExEnumerateCallback`-style introspection so the verifier (§14) and debugging can list registered routines without racing dispatch.
- [ ] Commit: `"kernel: ex -- callback objects"`

**Test checkpoint:** Named callback resolvable under `\Callback\`; `ExNotifyCallback` invokes every registered routine in order; concurrent `ExNotifyCallback` + `ExUnregisterCallback` from different CPUs proves unregister blocks until in-flight dispatch drains (no use-after-free, no missed routine); enumeration during churn returns a consistent snapshot. Verify on bare metal -- dispatch/unregister race window differs from VMs.

---

## 5. Lookaside Lists (NPaged, Paged, and Ex)

- [ ] Implement `NPAGED_LOOKASIDE_LIST` with depth, allocate/free counters, hit/miss stats, and tag; back the free-list with the §2 interlocked SLIST.
- [ ] Add `ExInitializeNPagedLookasideList`, `ExAllocateFromNPagedLookasideList`, `ExFreeToNPagedLookasideList`, `ExDeleteNPagedLookasideList`.
- [ ] Add the paged variant (`PAGED_LOOKASIDE_LIST` + `ExInitialize/Allocate/Free/DeletePagedLookasideList`), constrained to <= APC_LEVEL callers.
- [ ] Add the modern unified `LOOKASIDE_LIST_EX` (`ExInitializeLookasideListEx`, paged-or-nonpaged, custom alloc/free with private context) -- MS-recommended over the older pair for new code.
- [ ] Refill/drain uses the tagged pool API, never a hidden hot-path `kmalloc` (-> XREF D03 T03 pool-tag); enforce fixed-size allocations and poison freed entries in verifier mode.
- [ ] Consumers: registry notification records, ALPC messages, object namespace entries, klog v2 drain buffers.
- [ ] Commit: `"kernel: ex -- lookaside lists (npaged/paged/ex)"`

**Test checkpoint:** Free-then-allocate returns a cached block (hit counter increments); depth cap honored (excess frees go to backing pool, miss counter increments); paged variant rejects DISPATCH_LEVEL callers; `LOOKASIDE_LIST_EX` invokes the custom alloc/free with the private context; verifier mode detects use-after-free on a poisoned entry.

---

## 6. Fast References

- [ ] Implement `EX_FAST_REF`: pointer plus low-bit refcount packing with alignment asserts.
- [ ] Provide acquire, release, exchange, and get-object helpers.
- [ ] Use only for object pointers with guaranteed alignment and Object Manager reference semantics.
- [ ] Add tests for saturation fallback to full Ob reference.
- [ ] Commit: `"kernel: ex -- fast references"`

**Test checkpoint:** Pack/unpack round-trips an aligned object pointer with cached count intact; count saturation falls back to a full `ObReferenceObject`; alignment `_Static_assert`/runtime assert fires on a misaligned pointer.

---

## 7. Generic Tables and Bitmaps

- [ ] Implement AVL-backed `RTL_AVL_TABLE`-style generic table for kernel-core users.
- [ ] AVL APIs: initialize, insert, lookup, delete, enumerate, enumerate without splaying; pluggable compare/allocate/free callbacks.
- [ ] Add `RTL_DYNAMIC_HASH_TABLE` (create/insert/lookup/remove/enumerate, dynamic resize) for O(1)-average name/flow lookups where AVL O(log n) is too slow.
- [ ] Add `RTL_BITMAP` (`RtlInitializeBitMap`, `RtlSetBits`/`RtlClearBits`, `RtlFindClearBits`/`RtlFindSetBits`, `RtlAreBitsSet`) -- the general bit-vector utility (ad-hoc TLS bitmaps in T11 are not it).
- [ ] Caller-owned backing storage for all three (no hidden allocation); consumers: atom tables, loaded-image registry, tunable registry, named notification states, handle/PFN bit vectors.
- [ ] Commit: `"kernel: ex -- generic tables, dynamic hash, bitmaps"`

**Test checkpoint:** AVL insert/lookup/delete of 1000 keys returns correct elements and enumerate yields sorted order with height within log2(n)+1; dynamic hash table resizes and keeps lookups correct across grow/shrink; `RtlFindClearBits` returns the first run of N clear bits and `RtlSetBits` marks them.

---

## 8. Push Locks

Lighter-weight shared/exclusive lock than ERESOURCE (`EX_PUSH_LOCK`), used by Windows callback, file-object, and registry code on read-heavy paths.

- [ ] Define `EX_PUSH_LOCK` (pointer-sized) and implement `ExInitializePushLock`, `ExAcquirePushLockShared`, `ExAcquirePushLockExclusive`, `ExReleasePushLockShared`, `ExReleasePushLockExclusive`.
- [ ] Enforce the contract: caller must be in a critical region (`KeEnterCriticalRegion`, normal-APC-disabled) before acquire; not recursive; document the IRQL ceiling (-> XREF T07 for critical-region primitives).
- [ ] Paged-or-nonpaged storage; no owner-query API (matches Windows); pointer-sized so it fits inline in objects.
- [ ] Commit: `"kernel: ex -- push locks"`

**Test checkpoint:** N concurrent shared acquires proceed in parallel; an exclusive acquire waits until all shared holders release and blocks new shared acquires; acquiring outside a critical region asserts in verifier mode; release ordering is FIFO-fair enough to avoid writer starvation under steady read load. Verify on bare metal -- SMP contention and APC delivery differ from VMs.

---

## 9. Fast and Guarded Mutexes

Exclusive-only fast mutexes (`FAST_MUTEX`, `KGUARDED_MUTEX`) for the common single-owner case; not subsumed by ERESOURCE (no shared mode, no owner tracking, cheaper).

- [ ] Implement `ExInitializeFastMutex`, `ExAcquireFastMutex`, `ExReleaseFastMutex`, `ExTryToAcquireFastMutex`; acquire raises IRQL to APC_LEVEL while held; not recursive.
- [ ] Implement the guarded-mutex set (`KeInitializeGuardedMutex`/`KeAcquireGuardedMutex`/`KeReleaseGuardedMutex`/`KeTryToAcquireGuardedMutex`); acquire enters a guarded region disabling all kernel APCs (-> XREF T07 `KeEnterGuardedRegion`).
- [ ] Document wait legality: holders run at APC_LEVEL/guarded and must not perform alertable or PASSIVE-only waits.
- [ ] Commit: `"kernel: ex -- fast and guarded mutexes"`

**Test checkpoint:** Exclusive acquire blocks a second acquirer until release; `ExTryToAcquireFastMutex` returns FALSE without blocking when held; fast-mutex acquire observes IRQL == APC_LEVEL while held; guarded-mutex hold blocks APC delivery (a queued normal APC does not fire until release). Verify on bare metal -- IRQL/APC behavior differs from VMs.

---

## 10. Executive Resource Wrapper

- [ ] Provide `ERESOURCE`-style shared/exclusive resource wrapper over the owning synchronization primitives.
- [ ] Support recursive exclusive acquisition only when explicitly initialized with that flag.
- [ ] Add owner tracking in verifier mode; document the `ExInitializeResourceLite` vs `ExReinitializeResource` vs `ExDeleteResourceLite` lifecycle.
- [ ] Add a writer-preference / anti-starvation policy so a continuous read stream (e.g. registry hive lock) cannot starve an exclusive acquirer indefinitely (the documented ERESOURCE/rwsem hazard); record the policy in the header.
- [ ] Consumers: registry hive locks, NLS table reload locks, image registry lock.
- [ ] Commit: `"kernel: ex -- executive resource wrapper"`

**Test checkpoint:** N concurrent shared acquires succeed; an exclusive acquire blocks until all readers release; recursive exclusive succeeds only when initialized with the recursion flag; under a continuous reader stream a pending exclusive acquirer is granted within a bounded number of subsequent shared acquisitions (no indefinite starvation); verifier mode records the owning thread. Verify on bare metal -- SMP reader/writer contention differs.

---

## 11. Run-Once Initialization

One-time lazy-init primitive (`RTL_RUN_ONCE`) so drivers stop inventing unsafe ad-hoc "initialized" flags; distinct from the user-space `call_once` owned by D03 T08.

- [ ] Define `RTL_RUN_ONCE` and implement `RtlRunOnceInitialize`, `RtlRunOnceExecuteOnce` (synchronous), `RtlRunOnceBeginInitialize`/`RtlRunOnceComplete` (asynchronous), IRQL <= APC_LEVEL.
- [ ] Guarantee the init routine runs exactly once even under concurrent first-callers on multiple CPUs; losers wait for the winner's completion.
- [ ] Support the failure path: a failed init lets the next caller retry (state returns to uninitialized).
- [ ] Commit: `"kernel: ex -- run-once initialization"`

**Test checkpoint:** Concurrent `RtlRunOnceExecuteOnce` from N CPUs invokes the init routine exactly once and all callers observe the same context; a failing init routine leaves the once-block retryable (next caller re-runs it); async begin/complete serializes a single initializer. Verify on bare metal -- SMP first-caller race differs from VMs.

---

## 12. Worker Items, Delayed Work, and Ex Timers

- [ ] Add `ExInitializeWorkItem`, `ExQueueWorkItem`, `ExQueueDelayedWorkItem`, `ExCancelWorkItem`; work items run at PASSIVE_LEVEL.
- [ ] Route immediate work to the existing workqueue; route delayed work through timer/DPC handoff (-> XREF T07 for DPC).
- [ ] Expose the `EX_TIMER` object API drivers call directly: `ExAllocateTimer`, `ExSetTimer`, `ExCancelTimer`, `ExDeleteTimer` (object-manager-backed, distinct from the raw KTIMER/DPC path).
- [ ] Honor the cancel-vs-fire contract: `ExCancelWorkItem`/`ExCancelTimer` may fail if the item already dequeued/fired; document and test that window rather than assuming cancel always wins.
- [ ] Use in config tunable callbacks, notification fanout, and health probes.
- [ ] Commit: `"kernel: ex -- worker items, delayed work, ex timers"`

**Test checkpoint:** Immediate work runs on the workqueue at PASSIVE_LEVEL; delayed work and `ExSetTimer` fire after the elapsed interval; `ExCancelWorkItem`/`ExCancelTimer` before dispatch prevents the routine from running AND the in-flight case (cancel after dequeue) returns "already running" without double-free; `ExDeleteTimer` after cancel is safe.

---

## 13. Bugcheck Reason Callbacks

- [ ] Add `KeRegisterBugCheckReasonCallback` and `KeDeregisterBugCheckReasonCallback` (deregister on driver unload -- omitting it leaves a nonpaged record pointing at unloaded code).
- [ ] Callback classes: add-pages, secondary dump data, log snapshot, blackbox data.
- [ ] Integrate with TODO-27 dump writer and TODO-28 panic UI.
- [ ] Ensure callbacks cannot allocate or take locks in the panic path unless marked panic-safe (-> XREF T07 for IRQL/panic context).
- [ ] Commit: `"kernel: ex -- bugcheck reason callbacks"`

**Test checkpoint:** A registered reason callback is invoked during panic with the correct class; the panic-safe gate rejects a callback that attempts allocation or a non-panic-safe lock; `KeDeregisterBugCheckReasonCallback` removes the record so an unloaded-driver callback is never invoked; the dump writer receives secondary data. Verify on bare metal -- panic path differs from VMs.

---

## 14. Executive Verifier Hooks

> [!NOTE]
> Scoped Executive instrumentation, NOT a Driver Verifier / KASAN / lockdep replacement. Non-goals: full allocation poisoning, IRQL-misuse sweeps, lockdep-style global lock-order graphs, memory-lifetime/race detection. It checks the specific misuse modes of THIS layer's primitives at low cost; broader tooling is owned by D02 T31 (bulletproofing).

- [ ] Add `EX_VERIFIER=1` boot/config flag.
- [ ] Track callback leaks, rundown misuse, lookaside double-free, fast-ref alignment, push-lock/guarded-mutex acquire-outside-region, resource lock order, run-once misuse.
- [ ] Emit violations through TODO-27 and optionally bugcheck on fatal corruption.
- [ ] Unit tests cover every primitive (§2-§13) under normal and verifier mode.
- [ ] Commit: `"kernel: ex -- verifier hooks"`

**Test checkpoint:** With `EX_VERIFIER=1`, injected rundown misuse, lookaside double-free, fast-ref misalignment, and push-lock-outside-critical-region each emit a distinct violation record; a clean run emits none; fatal corruption optionally raises bugcheck.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                            | 🐧 Linux               | 🚀 Impossible OS          |
| -- | -------------------------- | ----------------------------------- | ---------------------- | ------------------------- |
| 💎 | Interlocked SLIST          | ✅ SLIST_HEADER                     | ✅ llist_head          | ⬜ Planned -- §2          |
| 💎 | Rundown protection         | ✅ EX_RUNDOWN_REF                   | ⚠️ percpu-ref/RCU      | ⬜ Planned -- §3          |
| 💎 | Callback objects           | ✅ ExCreateCallback                 | ⚠️ notifier chains     | ⬜ Planned -- §4          |
| 💎 | Lookaside lists            | ✅ NPaged/Paged/Ex                  | ✅ slab/kmem_cache     | ⬜ Planned -- §5          |
| 💎 | Fast references            | ✅ EX_FAST_REF                      | ❌ none                | ⬜ Planned -- §6          |
| 💎 | Ordered tables + bitmaps   | ✅ RTL_AVL_TABLE/RTL_BITMAP         | ✅ rbtree/bitmap       | ⬜ Planned -- §7          |
| 💎 | Push locks                 | ✅ EX_PUSH_LOCK                     | ⚠️ rwsem/RCU           | ⬜ Planned -- §8          |
| 💎 | Fast/guarded mutexes       | ✅ FAST_MUTEX/KGUARDED_MUTEX        | ✅ mutex               | ⬜ Planned -- §9          |
| 💎 | Shared/exclusive resource  | ✅ ERESOURCE                        | ✅ rw_semaphore        | ⬜ Planned -- §10         |
| 💎 | Run-once init              | ✅ RTL_RUN_ONCE                     | ⚠️ ad-hoc/call_once    | ⬜ Planned -- §11         |
| 💎 | Worker items + Ex timers   | ✅ ExQueueWorkItem/ExTimer          | ✅ workqueue/hrtimer   | ⬜ Planned -- §12         |
| 💎 | Bugcheck reason callbacks  | ✅ KeRegisterBugCheckReasonCallback | ⚠️ panic notifiers     | ⬜ Planned -- §13         |
| ⭐ | Scoped executive verifier  | ⚠️ Driver Verifier (heavy)         | ⚠️ KASAN/lockdep       | ⬜ Planned -- §14 scoped  |

> **After §1-§13:** Impossible OS matches Windows 11 and Linux on Executive support primitives.
> **After §14:** a low-cost verifier checks THIS layer's primitive-misuse modes inline, complementing (not replacing) the heavier Driver Verifier / KASAN / lockdep-class tooling owned by D02 T31.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_ex()` -- register in `src/kernel/test/test_runner.c`.
> Tests run with `debug=1` or `test=1` in boot.conf. Needs a new `TEST_CAT_EX` enum value (executive support) added to `include/kernel/test/test.h` + its `"ex"` string mapping in `test_category_from_string()`.

- [ ] Create `src/kernel/test/test_ex.c` with:
  - SLIST: concurrent push/pop preserves all entries; depth matches net count; flush resets to 0
  - Rundown: acquire-after-rundown returns FALSE; wait unblocks after last release
  - Callback: `ExNotifyCallback` invokes all registered; concurrent notify+unregister drains in-flight (no UAF); enumerate snapshot consistent
  - Lookaside: free-then-allocate hits cache; depth cap routes excess to pool; paged variant rejects DISPATCH_LEVEL; `LOOKASIDE_LIST_EX` calls custom alloc/free with private context
  - Fast-ref: pack/unpack round-trips pointer; saturation falls back to full Ob ref
  - Generic table: 1000-key AVL insert/lookup/delete correct + sorted enumerate; dynamic hash resizes; `RtlFindClearBits`/`RtlSetBits` correct
  - Push lock: N shared parallel; exclusive waits for drain; acquire-outside-critical-region asserts (verifier)
  - Fast/guarded mutex: exclusive blocks 2nd acquirer; try returns FALSE when held; fast-mutex holds at APC_LEVEL; guarded-mutex blocks APC delivery
  - ERESOURCE: shared allows N readers; exclusive blocks until drain; recursive only when flagged; bounded-starvation under reader stream
  - Run-once: concurrent first-callers run init exactly once; failed init leaves retryable
  - Work/timer: immediate runs at PASSIVE_LEVEL; `ExSetTimer` fires; cancel-before-dispatch prevents run; in-flight cancel returns "already running" without double-free
  - Bugcheck callback: panic-safe gate rejects allocating callback; deregister removes record
  - Verifier: injected double-free / misalignment / acquire-outside-region each emit a violation
- [ ] Add `TEST_CAT_EX` enum + `"ex"` string mapping
- [ ] Register in `test_runner_init()`: `test_register_ex()`
- [ ] Author `scripts/debug/kernel/run-ex-tests.bat` (SUITE=ex)
- [ ] Commit: `"test: add executive support runtime test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] `ex_init()` logs `"ex: initialized"` on serial during Phase 2
- [ ] Unit tests pass: `make test-ex` (or `bash scripts/test.sh SUITE=ex`) shows all PASS
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: ex -- executive support runtime complete"`

**Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 1 suite, 0 failures

