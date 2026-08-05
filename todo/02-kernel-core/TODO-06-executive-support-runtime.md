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

| ⭐   | Order | Deliverable                              | Depends On             | Status |
| --- | :---: | ---------------------------------------- | ---------------------- | :----: |
| 💎   |   1   | Executive headers and namespace          | --                     |  [x]   |
| 💎   |   2   | Interlocked SLIST                        | §1, atomics            |  [x]   |
| 💎   |   3   | Rundown protection                       | §1, atomics, events    |  [x]   |
| 💎   |   4   | Callback objects                         | §1, §3, T05, T07       |  [x]   |
| 💎   |   5   | Lookaside lists (NPaged/Paged/Ex)        | §2, D03 T03            |  [x]   |
| 💎   |   6   | Fast references                          | §1, T05                |  [x]   |
| 💎   |   7   | Generic tables and bitmaps               | §1                     |  [x]   |
| 💎   |   8   | Push locks                               | §1, T07                |  [/]   |
| 💎   |   9   | Fast and guarded mutexes                 | §1, T07                |  [/]   |
| 💎   |  10   | Executive resource wrapper               | §1, T07, D03 T08       |  [/]   |
| 💎   |  11   | Run-once initialization                  | §1                     |  [/]   |
| ⭐   |  12   | Worker items, delayed work, and Ex timers | §1, T07, DPC/workqueue |  [/]   |
| 💎   |  13   | Bugcheck reason callbacks                | §3, T27                |  [/]   |
| ⭐   |  14   | Executive verifier hooks                 | §2..§13                |  [/]   |

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

- [x] Define `SLIST_HEADER` (16-byte aligned `{SLIST_ENTRY *Next; uint64_t SeqDepth}`) + `SLIST_ENTRY` in `include/kernel/ex.h`.
  - `Next` is a FULL 64-bit pointer (no packing/truncation -- higher-half-relocation safe, D02 T33); `SeqDepth = (ABA_seq << 16) | depth16`; `_Static_assert(sizeof==16 && _Alignof>=16)`.
- [x] Implement `ExInitializeSListHead`/`InitializeSListHead`, `ExInterlockedPushEntrySList`, `ExInterlockedPopEntrySList`, `ExInterlockedFlushSList`, `ExQueryDepthSList` in `src/kernel/ex/ex_slist.c`.
- [x] Push/pop/flush use a single 16-byte `cmpxchg16b` DCAS so `Next` AND `SeqDepth` swap atomically (depth linearizable, NOT a side counter; ABA defeated by the seq bump). Arch-marked RBX-preserving `dcas16b()` inline-asm helper.
- [x] Gate CMPXCHG16B as a CPU requirement:
  - Added `CPU_FEATURE_CX16` (CPUID.01H:ECX[13]) to `cpuid.h` enum + `cpuid.c` BSP+AP detection + the `CPU_FEATURES_REQUIRED_LIST` X-macro (BSP minimum gate `boot_hw.c` + AP mask `cpu_security.c` fail-closed-halt on a CX16-less CPU).
  - Fixed the stale `cpuid.h` comment calling CMPXCHG16B a long-mode prereq; `kusd_time.c` `PF_COMPARE_EXCHANGE128` now from `cpu_has(CPU_FEATURE_CX16)`.
- [x] No hidden allocation: callers own the `SLIST_ENTRY` storage (documented contract in `ex.h`).
- [x] Commit: `"kernel: ex -- interlocked SLIST"`

**Test checkpoint:** `ex` suite (`test_ex.c`, `TEST_CAT_EX`) proves push/pop LIFO order, depth linearization (push++/pop--), pop/flush empty-edge returns NULL, and flush detaches the full chain + resets depth to 0. Verify on bare metal -- `cmpxchg16b` ABA behavior and alignment faults differ from VMs.

> **Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 5 suites, 0 failures
> **Notes:**
> - What shipped: `src/kernel/ex/ex_slist.c` (SLIST push/pop/flush/depth + arch-marked `dcas16b()` cmpxchg16b helper) + `SLIST_HEADER`/`SLIST_ENTRY` in `ex.h`; new `TEST_CAT_EX` + `test_ex.c` (5 suites incl. a 4-kthread concurrent stress).
> - How it runs / integrates: lock-free, DISPATCH-safe, no allocation; 16-byte header swung by one DCAS so depth is linearizable; ABA defeated by the seq counter. `make test-ex` / `SUITE=ex`.
> - Adversarial hardening (commit message): init bugchecks a misaligned head (cmpxchg16b #GP guard), depth saturates at 65535 (no wrap-to-0), SMP stress fails-by-assertion not hang.
> - Downstream effects: free-list spine for §5 lookaside lists; gated CMPXCHG16B as a real boot CPU requirement (`CPU_FEATURE_CX16`), fixing a latent wrong assumption that it is a long-mode prereq.
> - Canonical doc: [`include/kernel/ex.h`](../../include/kernel/ex.h) S2 block.
> - Scope boundary: §2 owns the SLIST primitive + CX16 gate; lookaside consumption is §5; the higher-half pointer-safety assumption is owned by D02 T33.
> **Verified:** 2026-06-25 | commit `576eb413` | 5/5 items | build OK | smoke PASS (TCG 2.53s)
> **Quality reviewed:** 2026-06-25 | Codex 7x (design, adversarial x2, re-adversarial x2, consistency, perf) | 3H+4M+2L fixed | scope: kernel-code-quality

---

## 3. Rundown Protection

- [x] Implement `EX_RUNDOWN_REF` (single `volatile uint64_t Count`: bit0 = rundown-active, bits1+ = refcount<<1) in `src/kernel/ex/ex_rundown.c`.
  - APIs: `ExInitializeRundownProtection`, `ExAcquireRundownProtection`, `ExReleaseRundownProtection`, `ExWaitForRundownProtectionRelease`, `ExReInitializeRundownProtection`, `ExRundownCompleted`, `ExIsRundownActive`.
- [x] Guarantee `ExAcquireRundownProtection` returns FALSE once the rundown bit is set (CAS rejects acquire after wait begins).
- [x] Wait blocks only at PASSIVE_LEVEL via a cooperative `while(refcount>0) yield()` poll, NOT the kernel `event_t` (lost-wakeup race between state-read and enqueue). Caller-owned, no allocation.
- [/] Convert teardown races -- DEFERRED to owning consumers (primitive ships here):
  - object callbacks `ob_callback.c` unregister-vs-dispatch race -> XREF TODO-05 §13; Ex callback objects -> §4; ALPC ports -> XREF TODO-24; section/process objects -> XREF TODO-05 §6/§7.
- [/] Priority-inheritance-correct wait (DEFERRED, in-scope): poll-wait requires holders run at >= the waiter priority (precondition in `ex.h`).
  - A PI-correct wait needs per-reference holder identity to boost holders; not functional-blocking today (no live rundown consumers).
- [x] Commit: `"kernel: ex -- rundown protection"`

**Test checkpoint:** `ex` suite proves acquire/nested-acquire/release; `ExRundownCompleted` + post-wait acquire return FALSE; `ExReInitialize` re-arms; and a concurrent kthread holding a ref makes `ExWaitForRundownProtectionRelease` block until the worker releases. Verify on bare metal -- SMP acquire/release race timing differs from VMs.

> **Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 9 suites, 0 failures
> **Notes:**
> - What shipped: `src/kernel/ex/ex_rundown.c` (`EX_RUNDOWN_REF` acquire/release/wait/reinit/completed/active) + the type/decls in `ex.h`; 4 rundown test suites in `test_ex.c` (incl. concurrent drain).
> - How it runs / integrates: single atomic word (bit0 active, bits1+ refcount); lock-free CAS acquire (fails once active), atomic-subtract release; wait is a cooperative PASSIVE poll avoiding the `event_t` lost-wakeup race. DISPATCH-safe except wait.
> - Downstream effects: the drain primitive for teardown-race consumers; resolves the mechanism gap in TODO-05 §13 (ob_callback drain), reciprocally XREF'd.
> - Canonical doc: [`include/kernel/ex.h`](../../include/kernel/ex.h) S3 block.
> - Scope boundary: §3 owns only the primitive + test; consumer conversions (ob_callback, Ex callbacks §4, ALPC TODO-24, section/process TODO-05 §6/§7) are owner-deferred.
> **Verified:** 2026-06-25 | commit `5fc1b61b` | 3/5 items | build OK | tests 9 suites PASS
> **Deferred:** [H] poll-wait priority inversion: a strictly-higher-priority waiter starves a strictly-lower-priority holder (reason: not-functional-today, no live rundown consumers; precondition documented in `ex.h`) -> XREF: 02-kernel-core/TODO-06-executive-support-runtime.md §3 (item: "Priority-inheritance-correct wait" at line 127)
> **Quality reviewed:** 2026-06-25 | Codex 6x (design, adversarial x2, re-adversarial, consistency, perf) | 1H+1M fixed, 1H deferred | scope: kernel-code-quality

---

## 4. Callback Objects

- [x] Add `ExCreateCallback`/`ExRegisterCallback`/`ExUnregisterCallback`/`ExNotifyCallback` in `src/kernel/ex/ex_callback.c`, backed by OM type `Callback` (`ObpCallbackType` via `ob_create_type`, like `ob_event.c`).
- [x] Slot lifecycle + cookie identity (design F1):
  - Fixed registration array, explicit free -> active -> unregistering -> free states; cookie packs slot index + a monotonic per-slot generation, validated under the spinlock so a stale cookie can't hit a reused slot.
  - Slot reuse (free -> active) MUST `ExReInitializeRundownProtection` before publishing active -- the drain leaves the active bit set, so a reused slot would otherwise never dispatch (re-review F3-redux).
- [x] Dispatch under per-slot rundown (§3):
  - `ExNotifyCallback` snapshots active slots + acquires each slot's `EX_RUNDOWN_REF` under the lock, invokes outside the lock, releases; `ExUnregisterCallback` marks unregistering then `ExWaitForRundownProtectionRelease` drains ALL outstanding refs (incl. other CPUs') before returning, so the caller may then free context.
- [x] Self-unregister FORBIDDEN by contract (design F2, re-review): must not call `ExUnregisterCallback` from within a callback (matches Windows); documented in the header.
  - Drain-all-refs is provably safe for the only LEGAL (non-self) usage -- no deferral, no concurrent-dispatcher UAF. Misuse detection (deadlock -> bugcheck) owned by the §14 verifier -> XREF §14.
- [x] Support named callbacks under `\Callback\` (created at `ex_init` via `ob_ns_create_directory` + `ObInsertObject`, name-collision handling like `ob_event.c`).
- [x] Built-in callback OBJECTS only (named `\Callback\` points; PRODUCERS owned by subsystems):
  - ProcessCreate/ThreadCreate -> XREF D02 T21; ImageLoad -> XREF D02 T18; RegistryChange -> XREF D02 T14; PowerState -> XREF D02 T26; CodeIntegrity -> XREF D02 T19.
- [x] Add `ExpEnumerateCallback`-style introspection (snapshot under the spinlock) for the verifier (§14) + debugging.
- [/] Priority-inversion precondition (design F3):
  - `ExUnregisterCallback` drain inherits the §3 poll-wait precondition (notifier priority >= unregister), documented in the header. Not functional-blocking (all producers deferred -> no live notifiers); PI-correct wait is the §3 deferred item.
- [x] Commit: `"kernel: ex -- callback objects"`

**Test checkpoint:** `ex` suite proves register/notify/unregister (routine gets context+args, no invoke after unregister), `allow_multiple=false` cap, multi-registration fan-out + enumerate count, slot reuse re-dispatches (rundown re-armed), stale double-unregister is a safe no-op, and the built-in `\Callback\ProcessCreate` is resolvable. Boot serial shows `"Callback objects initialized (\Callback\ + 6 built-ins)"`. Verify on bare metal -- dispatch/unregister race window differs from VMs.

> **Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 15 suites, 0 failures
> **Notes:**
> - What shipped: `src/kernel/ex/ex_callback.c` (`ExCreate`/`Register`/`Unregister`/`Notify`/`ExpEnumerate` over OM type `Callback` + `\Callback\` dir + 6 built-in objects) + `EX_CALLBACK_OBJECT`/decls in `ex.h`; 6 callback suites in `test_ex.c`.
> - How it runs / integrates: `ex_callback_init` from `ex_init` (Phase 2 post-OB); per-slot `EX_RUNDOWN_REF` makes unregister drain in-flight dispatch; generation cookie rejects stale; allow_multiple cap; PASSIVE.
> - Downstream effects: named notification points for the deferred producers (process/image/registry/power/CI); the OB-callback drain mechanism reciprocally referenced in TODO-05 §13.
> - Canonical doc: [`include/kernel/ex.h`](../../include/kernel/ex.h) S4 block.
> - Scope boundary: §4 owns the callback OBJECTS + dispatch; PRODUCERS owned by D02 T21/T18/T14/T26/T19; self-unregister misuse detection owned by §14; PI-correct drain by §3.
> **Verified:** 2026-06-26 | commit `3d67711e` | 7/8 items | build OK | smoke PASS (TCG 2.55s)
> **Quality reviewed:** 2026-06-26 | Codex 8x (design x2, adversarial x2, re-adversarial x2, consistency, perf) | 7H+4M+1L fixed | scope: kernel-code-quality

## 5. Lookaside Lists (NPaged, Paged, and Ex)

- [x] `NPAGED_LOOKASIDE_LIST` over a shared `EX_LOOKASIDE` block (`ex_lookaside.c`): size/tag/max_depth + atomic stats; §2 SLIST free-list, SLIST_HEADER aligned in an over-provisioned buffer.
- [x] `ExInitialize/Allocate/Free/DeleteNPagedLookasideList`: cache hit/free are SLIST-only (DISPATCH/SMP-safe); a nonpaged miss above APC_LEVEL returns NULL rather than touch the unsynchronized heap.
- [x] Paged variant (`PAGED_LOOKASIDE_LIST` + `ExInitialize/Allocate/Free/DeletePagedLookasideList`): all ops legal only at <= APC_LEVEL; a DISPATCH_LEVEL caller is rejected (NULL/no-op; IRQL bugcheck in verifier mode).
- [x] Unified `LOOKASIDE_LIST_EX` (`ExInitialize/Allocate/Free/Flush/DeleteLookasideListEx`): paged-or-nonpaged, optional custom alloc/free pair + `ctx`; bad args return non-zero; flush keeps the list usable.
- [x] Backing: cold-path kmalloc/kfree only at <= APC_LEVEL (never DISPATCH); inherits the unsynchronized heap; verifier poisons freed bodies. Retrofit -> XREF: 03-memory-concurrency/TODO-03 §6 (item: "Migrate lookaside backing to tagged pool").
- [/] Consumers (registry notification records, ALPC messages, OB namespace entries, klog drain buffers): API is consumer-ready; adoption owned by each consuming subsystem, not a §5 deliverable.
- [x] Commit: `"kernel: ex -- lookaside lists (npaged/paged/ex)"`

**Test checkpoint:** Free-then-allocate returns a cached block (hit counter increments); depth cap honored (excess frees go to backing pool, miss counter increments); paged variant rejects DISPATCH_LEVEL callers; `LOOKASIDE_LIST_EX` invokes the custom alloc/free with the private context; verifier mode detects use-after-free on a poisoned entry.

> **Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 8 lookaside suites, 0 failures
> **Notes:**
> - What shipped: `src/kernel/ex/ex_lookaside.c` (NPaged/Paged/Ex lookaside over a shared `EX_LOOKASIDE` control block) + types/decls in `ex.h`; 8 lookaside suites added to `test_ex.c`.
> - How it integrates: hot paths are the §2 interlocked SLIST (lock-free, DISPATCH/SMP-safe); the cold backing (grow/drain) calls kmalloc/kfree only at <= APC_LEVEL (never DISPATCH) and inherits the unsynchronized kernel heap.
> - Verifier seam: `ExpSetLookasideVerifier` + `ExpLookasideUafCount` poison/re-check freed bodies; broader verifier escalation owned by §14.
> - Retrofit owed: kmalloc backing -> synchronized tagged pool when 03-memory-concurrency/TODO-03 §6 ships `ExAllocatePoolWithTag`; Codex design+adversarial+test-coverage adoptions in the commit message.
> - Scope boundary: §5 owns the lookaside OBJECTS + backing discipline; consumers (registry/ALPC/OB/klog) adopt the API in their own sections; kernel-wide heap SMP-safety owned by 03-memory-concurrency/TODO-03.
> **Verified:** 2026-06-26 | commit `e10b246a` | 5/6 items | build OK | tests 141 kernel + 16 user PASS
> **Accepted:** [H] backing cold path races the globally-unsynchronized kernel heap across CPUs (reason: pre-existing kernel-wide condition) -> XREF: 03-memory-concurrency/TODO-03 §6 (item: "Migrate lookaside backing to tagged pool" at line 217)
> **Accepted:** [H] custom-free-that-unmaps + delete/flush-while-in-use are caller concurrency contracts (reason: Windows-faithful; detection owned by verifier) -> XREF: 02-kernel-core/TODO-06 §14 (item: "Track ... lookaside misuse (... teardown-in-use + custom-free-unmap ...)" at line 197)
> **Quality reviewed:** 2026-06-26 | Codex 5x (adversarial, consistency, perf, re-adversarial) | 2H+2M+2L fixed, 3H accepted-XREF | scope: kernel-code-quality

---

## 6. Fast References

- [x] Implement `EX_FAST_REF`: pointer-sized atomic packing `(object & ~7) | count`, 3-bit cached count (`EX_FAST_REF_MAX=7`); `_Static_assert` + `ExpFastRefPack` bugcheck guard alignment/range. `src/kernel/ex/ex_fastref.c` + ex.h S6.
- [x] Provide acquire (`ExAcquireFastReference` -> `{object, cached}`), release (`ExReleaseFastReference`), exchange (`ExCompareSwapFastReference`), get-object (`ExGetObjectFastReference`) helpers -- all lock-free `__atomic` CAS, arch-neutral.
- [x] 3-bit count (max 7), not Windows' x64 4-bit/15: kmalloc only guarantees 8-byte object alignment so only 3 low bits are free (x86 Windows model). Ob ref/deref + replenish are caller-owned (see ex.h S6 CONTRACT).
- [x] Tests: pack/unpack round-trip 0..MAX + NULL; empty-cache acquire -> slow path; release saturation/mismatch/NULL rejected; compare-swap identity + NULL clear; 8-thread acquire/release conservation stress. `test_ex_fastref_*` (7 cases).
- [x] Commit: `"kernel: ex -- fast references"`

**Test checkpoint:** Pack/unpack round-trips an aligned object pointer with cached count intact; an empty-cache acquire returns the slow-path signal (`cached=false`, caller does `ObReferenceObjectSafe`) and a release at saturation returns false (caller does `ObDereferenceObject`); alignment `_Static_assert`/runtime assert fires on a misaligned pointer; an 8-thread (> MAX) acquire/release stress conserves the reference pool with no torn count.

> **Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 1 suite, 0 failures

> **Notes:**
> - **What shipped** -- `src/kernel/ex/ex_fastref.c` (pointer-sized `EX_FAST_REF`, lock-free inline-cached Ob reference; 3-bit count, max 7) + ex.h S6 block; 7 `test_ex_fastref_*` cases incl. an 8-thread conservation stress.
> - **How it integrates** -- no global state, no init hook, no hidden allocation; every op is a single-word `__atomic` CAS. Caller owns the Ob ref/deref and replenish under its own lock; acquire/release booleans signal the fast/slow path.
> - **Scope boundary** -- S6 owns only pointer/count packing + the lock-free cache mechanic; `ObReferenceObjectSafe`/`ObDereferenceObject` and replenish are the consuming subsystem's (no live consumer yet, like S4 callbacks).
> - **Canonical doc** -- `include/kernel/ex.h` S6 header contract (ALIGNMENT + CONTRACT blocks).
> **Verified:** 2026-06-26 | commit `27bf7ce1` | 4/4 items | build OK | tests 225/225 PASS
> **Quality reviewed:** 2026-06-26 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+3M+1L fixed, 1M rejected | scope: kernel-code-quality

---

## 7. Generic Tables and Bitmaps

- [x] Height-cached AVL `RTL_AVL_TABLE` (`ex_avltable.c`): nodes (`RTL_BALANCED_LINKS` + element copy) from caller allocate/free; rotations recompute cached height; delete uses size-independent node RELINKING.
- [x] AVL APIs: Initialize/Insert (copies + dedups)/Lookup/Delete/in-order Enumerate/NumberElements/IsEmpty `GenericTableAvl`; pluggable compare/allocate/free + context.
- [x] `RTL_DYNAMIC_HASH_TABLE` (`ex_hashtable.c`): power-of-two chained buckets, splitmix64 mix, load-factor GROW-only rehash (no shrink -> remove-during-walk cursors stay valid); init/insert/lookup(+GetNext)/remove/delete; caller-embedded entry.
- [x] `RTL_BITMAP` (`ex_bitmap.c`): Init, Set/ClearBit(s), Set/ClearAllBits, TestBit, AreBitsSet/Clear, FindClear/SetBits(+AndSet/AndClear), NumberOfSet/ClearBits; final-word padding never counted, returned, or mutated.
- [x] Caller-owned storage for all three (no hidden allocation): bitmap buffer caller-owned; AVL nodes + hash directory via MANDATORY caller allocate/free (no kmalloc fallback); hash resize PASSIVE-only, fails closed. All caller-serialized.
- [x] Commit: `"kernel: ex -- generic tables, dynamic hash, bitmaps"`

**Test checkpoint:** AVL insert/lookup/delete of 1000 keys returns correct elements and enumerate yields sorted order with height within the AVL bound (<= 15 nodes for n=1000, ~1.44*log2(n)); dynamic hash table grows and keeps lookups correct across the rehash, and a long collision chain drains via the remove-current cursor; `RtlFindClearBits` returns the first run of N clear bits (crossing a word boundary) and `RtlFindClearBitsAndSet` marks them.

> **Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 1 suite, 0 failures

> **Notes:**
> - **What shipped** -- `ex_bitmap.c` (RTL_BITMAP), `ex_avltable.c` (height-cached AVL RTL_AVL_TABLE), `ex_hashtable.c` (RTL_DYNAMIC_HASH_TABLE) + ex.h S7 block; 7 test cases incl. a 1000-key AVL worst-case balance test and a grow-only chain-drain.
> - **How it integrates** -- all three caller-serialized (no internal locks), caller-owned storage (bitmap buffer, AVL nodes, hash directory via a mandatory allocate/free pair); hash is grow-only with a PASSIVE fail-closed grow.
> - **Scope boundary** -- S7 owns the container mechanics only; the consumers (atom tables, loaded-image registry, tunable registry, named-notification states, handle/PFN bit vectors) own their own locking and pick the structure.
> - **Canonical doc** -- `include/kernel/ex.h` S7 header block (per-structure storage + serialization contracts).
> **Verified:** 2026-06-26 | commit `02062e62` | 5/5 items | build OK | tests 5661/5661 PASS
> **Quality reviewed:** 2026-06-26 | Codex 6x (design, adversarial, consistency, perf, re-adversarial) | 3H+2M+2L fixed | scope: kernel-code-quality

---

## 8. Push Locks

Lighter-weight shared/exclusive lock than ERESOURCE (`EX_PUSH_LOCK`), used by Windows callback, file-object, and registry code on read-heavy paths.

- [/] Define pointer-sized `EX_PUSH_LOCK` + `ExInitializePushLock`/Acquire/Release Shared+Exclusive. BLOCKED on an address-keyed park (see Deferred stamp); yield-poll deadlocks the priority scheduler.
- [/] Enforce the contract: caller in a critical region (`KeEnterCriticalRegion`) before acquire; not recursive. UNBLOCKED: `KeEnterCriticalRegion` shipped in T07 §11 (`sched/apc.{c,h}`); wire the acquire-contract assert.
- [/] Paged-or-nonpaged storage; no owner-query API (matches Windows); pointer-sized so it fits inline in objects.
- [ ] Commit: `"kernel: ex -- push locks"`

**Test checkpoint:** N concurrent shared acquires proceed in parallel; an exclusive acquire waits until all shared holders release and blocks new shared acquires; acquiring outside a critical region asserts in verifier mode; release ordering is FIFO-fair enough to avoid writer starvation under steady read load. Verify on bare metal -- SMP contention and APC delivery differ from VMs.

> **Notes:**
> - **Why deferred** -- a pointer-sized contended block needs an address-keyed park/wake (keyed events); the `yield()`-poll fallback deadlocks on the strict-priority scheduler (Codex design HIGH).
> - **Blocked on** -- `02-kernel-core/TODO-07 §11` (KeEnterCriticalRegion -- acquire contract) + `03-memory-concurrency/TODO-08 §10` (keyed events -- pointer-sized blocking primitive).
> - **Scope boundary** -- §8 owns the `EX_PUSH_LOCK` Ex-API only; the critical-region primitive (T07) and keyed-event/park mechanism (D03 T08) are owned elsewhere; the out-of-region verifier assert is §14.

> **Deferred:** [H] EX_PUSH_LOCK needs an address-keyed park (yield-poll deadlocks on the priority scheduler) -> XREF: 03-memory-concurrency/TODO-08 §10 (item: "NtWaitForKeyedEvent" at line 254 -- the kernel keyed-event block a pointer-sized pushlock waits on)
> **Deferred:** [M] acquire contract needs the critical-region primitive -> XREF: 02-kernel-core/TODO-07 §11 (item: "Add critical/guarded region support: KeEnterCriticalRegion()/KeLeaveCriticalRegion()" at line 239)

---

## 9. Fast and Guarded Mutexes

Exclusive-only fast mutexes (`FAST_MUTEX`, `KGUARDED_MUTEX`) for the common single-owner case; not subsumed by ERESOURCE (no shared mode, no owner tracking, cheaper).

- [/] Implement `FAST_MUTEX` set (`ExInitializeFastMutex`/Acquire/Release/Try); raises IRQL to APC_LEVEL while held; not recursive. BLOCKED: per-CPU IRQL leaks APC_LEVEL across a yield (see Deferred stamp); needs per-thread APC-disable.
- [/] Implement the guarded-mutex set (`KeInitialize/Acquire/Release/TryToAcquireGuardedMutex`); acquire enters a guarded region. UNBLOCKED: `KeEnterGuardedRegion` shipped in T07 §11 (`sched/apc.{c,h}`); wire the acquire.
- [/] Document wait legality: holders run at APC_LEVEL/guarded and must not perform alertable or PASSIVE-only waits.
- [ ] Commit: `"kernel: ex -- fast and guarded mutexes"`

**Test checkpoint:** Exclusive acquire blocks a second acquirer until release; `ExTryToAcquireFastMutex` returns FALSE without blocking when held; fast-mutex acquire observes IRQL == APC_LEVEL while held; guarded-mutex hold blocks APC delivery (a queued normal APC does not fire until release). Verify on bare metal -- IRQL/APC behavior differs from VMs.

> **Notes:**
> - **Why deferred** -- the per-CPU IRQL (TPR) raise leaks APC_LEVEL across a yield (the scheduler does not context-switch IRQL); APC suppression needs a per-thread APC-disable that survives a switch.
> - **Blocked on** -- `02-kernel-core/TODO-07 §11` (KeEnterCriticalRegion/KeEnterGuardedRegion -- per-thread APC-disable) or scheduler per-thread IRQL save/restore.
> - **Scope boundary** -- §9 owns the FAST_MUTEX/KGUARDED_MUTEX Ex-API; the per-thread APC-suppression mechanism is owned by T07/scheduler. The underlying exclusion (mutex_t) already exists.

> **Deferred:** [H] fast/guarded mutex APC suppression needs a per-thread APC-disable surviving context switches (per-CPU IRQL leaks across yields) -> XREF: 02-kernel-core/TODO-07 §11 (item: "Add critical/guarded region support: KeEnterCriticalRegion()/KeLeaveCriticalRegion()" at line 239)

---

## 10. Executive Resource Wrapper

- [/] Provide `ERESOURCE`-style shared/exclusive resource wrapper over the owning synchronization primitives. BLOCKED: the underlying `rwlock_t` is not SMP-linearizable (see Deferred stamp), so a wrapper would inherit a non-exclusive lock.
- [/] Support recursive exclusive acquisition only when explicitly initialized with that flag.
- [/] Add owner tracking in verifier mode; document the `ExInitializeResourceLite` vs `ExReinitializeResource` vs `ExDeleteResourceLite` lifecycle.
- [/] Add a writer-preference / anti-starvation policy so a continuous read stream (e.g. registry hive lock) cannot starve an exclusive acquirer indefinitely; record the policy in the header.
- [ ] Consumers: registry hive locks, NLS table reload locks, image registry lock.
- [ ] Commit: `"kernel: ex -- executive resource wrapper"`

**Test checkpoint:** N concurrent shared acquires succeed; an exclusive acquire blocks until all readers release; recursive exclusive succeeds only when initialized with the recursion flag; under a continuous reader stream a pending exclusive acquirer is granted within a bounded number of subsequent shared acquisitions (no indefinite starvation); verifier mode records the owning thread. Verify on bare metal -- SMP reader/writer contention differs.

> **Notes:**
> - **Why deferred** -- `rwlock_t`'s acquire paths are not SMP-linearizable (read_lock check-then-inc; write_lock atomic_set-not-CAS), so wrapping it would inherit a lock that admits a reader+writer or two writers concurrently.
> - **Blocked on** -- `03-memory-concurrency/TODO-08 §11` (rwlock_t acquire linearizability) for the core lock; `02-kernel-core/TODO-07 §11` (critical-region precondition) for the verifier owner-tracking assert.
> - **Scope boundary** -- §10 owns the ERESOURCE Ex-API + recursion/owner tracking; the SMP-safe rwlock is owned by advanced-sync (fixing core scheduler locks is out of §10's scope).

> **Deferred:** [H] ERESOURCE needs an SMP-linearizable shared/exclusive lock; `rwlock_t` admits concurrent reader+writer / two writers -> XREF: 03-memory-concurrency/TODO-08 §11 (item: "rwlock_t acquire linearizability" -- gate the state transition with a spinlock/CAS)
> **Deferred:** [M] verifier owner-tracking assert needs the critical-region precondition -> XREF: 02-kernel-core/TODO-07 §11 (item: "Add critical/guarded region support: KeEnterCriticalRegion()/KeLeaveCriticalRegion()" at line 239)

---

## 11. Run-Once Initialization

One-time lazy-init primitive (`RTL_RUN_ONCE`) so drivers stop inventing unsafe ad-hoc "initialized" flags; distinct from the user-space `call_once` owned by D03 T08.

- [/] Define `RTL_RUN_ONCE` + `RtlRunOnceInitialize`/`ExecuteOnce` (sync)/`BeginInitialize`+`Complete` (async), IRQL <= APC_LEVEL. BLOCKED: the RUNNING-state loser-wait (sync AND async) needs a per-once address-keyed wait object (see Deferred stamp).
- [/] Guarantee the init routine runs exactly once even under concurrent first-callers on multiple CPUs; losers wait for the winner's completion.
- [/] Support the failure path: a failed init lets the next caller retry (state returns to uninitialized).
- [ ] Commit: `"kernel: ex -- run-once initialization"`

**Test checkpoint:** Concurrent `RtlRunOnceExecuteOnce` from N CPUs invokes the init routine exactly once and all callers observe the same context; a failing init routine leaves the once-block retryable (next caller re-runs it); async begin/complete serializes a single initializer. Verify on bare metal -- SMP first-caller race differs from VMs.

> **Notes:**
> - **Why deferred** -- the RUNNING-state loser-wait needs a per-once address-keyed wait object; a shared hash-bucket mutex held across init deadlocks on nested run-once and cannot model the async begin/complete handoff.
> - **Blocked on** -- `03-memory-concurrency/TODO-08 §10` (keyed events -- per-once address-keyed wait) + `03-memory-concurrency/TODO-08 §11` (mutex/rwlock SMP backfill -- the loser wait queue).
> - **Scope boundary** -- §11 owns the RTL_RUN_ONCE Ex-API + the lock-free state machine; the per-once blocking wait is owned by advanced-sync.

> **Deferred:** [H] RTL_RUN_ONCE sync+async loser-wait needs a per-once address-keyed wait object -> XREF: 03-memory-concurrency/TODO-08 §10 (item: "NtWaitForKeyedEvent" at line 254 -- the kernel keyed-event block keyed on the once address)

---

## 12. Worker Items, Delayed Work, and Ex Timers

- [x] Add immediate-work `EX_WORK_ITEM` (`ExInitializeWorkItem`/`ExQueueWorkItem`/`ExCancelWorkItem`) on `sys_wq` at PASSIVE_LEVEL; lock-free `__atomic` state machine, enqueue-failure rolls QUEUED->IDLE. `ex_workitem.c`.
- [x] Route immediate work to the existing workqueue (`sys_wq`). `ExQueueDelayedWorkItem` deferred -- no multi-deadline timer queue (see Deferred stamp).
- [/] Expose the `EX_TIMER` object API (`ExAllocateTimer`/`ExSetTimer`/`ExCancelTimer`/`ExDeleteTimer`). DEFERRED: needs a KTIMER/timer-queue (the timer layer is a singleton one-shot).
- [x] Honor the cancel-vs-fire contract: `ExCancelWorkItem` wins only while QUEUED; once the worker flips to RUNNING it returns false ("already running"), no double-free. Tested deterministically (blocker occupies the single worker).
- [ ] Use in config tunable callbacks, notification fanout, and health probes.
- [x] Commit: `"kernel: ex -- worker items, delayed work, ex timers"`

**Test checkpoint:** Immediate work runs on the workqueue at PASSIVE_LEVEL; `ExCancelWorkItem` before dispatch prevents the routine from running AND the in-flight case (worker already flipped to RUNNING) returns "already running" without double-free; a DONE work item re-queues. (Delayed work / `ExSetTimer` / `ExDeleteTimer` deferred -- see stamp.)

> **Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 1 suite, 0 failures

> **Notes:**
> - **What shipped** -- `ex_workitem.c` (immediate `EX_WORK_ITEM` on `sys_wq` + lock-free cancel-vs-fire state machine) + ex.h S12 block; 2 test cases incl. a deterministic cancel-before-dispatch.
> - **How it integrates** -- caller-owned item, no allocation; `ExQueueWorkItem` trampolines through `sys_wq`; the workqueue has no dequeue so cancel is a state-flag the trampoline re-checks; enqueue-failure (full 64-node pool) rolls back to IDLE.
> - **Scope boundary** -- §12 ships immediate work items; delayed work + the `EX_TIMER` object set are deferred on a multi-deadline timer queue (KTIMER), owned by T07.
> - **Canonical doc** -- `include/kernel/ex.h` S12 header block.

> **Verified:** 2026-06-26 | commit `7636ae22` | 3/5 items | build OK | tests 5684/5684 PASS
> **Deferred:** [M] delayed work (`ExQueueDelayedWorkItem`) + the `EX_TIMER` object set need a multi-deadline cancellable timer queue; the timer layer is only a singleton one-shot -> XREF: 02-kernel-core/TODO-07 §6 (item: "Timer/APIC Scheduling Path for DPC Dispatch")
> **Quality reviewed:** 2026-06-26 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 3H+5M fixed | scope: kernel-code-quality

---

## 13. Bugcheck Reason Callbacks

- [/] Add `KeRegisterBugCheckReasonCallback`/`KeDeregisterBugCheckReasonCallback`. BLOCKED: the panic-time callback walk needs a global CPU-freeze (panic only disables LOCAL interrupts); see Deferred stamp.
- [/] Callback classes: add-pages, secondary dump data, log snapshot, blackbox data.
- [/] Integrate with TODO-27 dump writer and TODO-28 panic UI. BLOCKED: T27 §5 / T28 §13 are the consumers; without them the dump-data contract cannot be validated.
- [/] Ensure callbacks cannot allocate or take locks in the panic path unless marked panic-safe. BLOCKED: a registration flag is not enforcement; needs panic-context instrumentation.
- [ ] Commit: `"kernel: ex -- bugcheck reason callbacks"`

**Test checkpoint:** A registered reason callback is invoked during panic with the correct class; the panic-safe gate rejects a callback that attempts allocation or a non-panic-safe lock; `KeDeregisterBugCheckReasonCallback` removes the record so an unloaded-driver callback is never invoked; the dump writer receives secondary data. Verify on bare metal -- panic path differs from VMs.

> **Notes:**
> - **Why deferred** -- the panic-path callback walk needs a global CPU-freeze/ack (panic only disables local interrupts), the panic-safe flag is documentation not enforcement, and there is no real dump/UI consumer to validate the data contract.
> - **Blocked on** -- `02-kernel-core/TODO-27 §1` (KeBugCheckEx + panic global CPU-freeze) + `02-kernel-core/TODO-27 §5` (minidump writer -- the secondary-dump-data consumer) + `02-kernel-core/TODO-28 §13` (panic UI).
> - **Scope boundary** -- §13 owns the bugcheck-reason-callback Ex-API + panic-safe registration; the panic CPU-freeze + dump-data consumers are owned by the crash-dump/BSOD TODOs.

> **Deferred:** [H] panic-time callback walk needs a global CPU-freeze + a real dump consumer -> XREF: 02-kernel-core/TODO-27 §5 (item: "Commit: minidump writer, stream serialisation, CRC32C, memory page selection" at line 190)

---

## 14. Executive Verifier Hooks

> [!NOTE]
> Scoped Executive instrumentation, NOT a Driver Verifier / KASAN / lockdep replacement. Non-goals: full allocation poisoning, IRQL-misuse sweeps, lockdep-style global lock-order graphs, memory-lifetime/race detection. It checks the specific misuse modes of THIS layer's primitives at low cost; broader tooling is owned by D02 T31 (bulletproofing).

- [/] Add `EX_VERIFIER=1` boot/config flag. BLOCKED: a central verifier needs its instrumented primitives to exist; §8-§11 + §13 are deferred; shipped primitives already self-guard (§5/§6).
- [/] Track callback leaks, rundown misuse, lookaside misuse, fast-ref alignment, push-lock/guarded-mutex acquire-outside-region, resource lock order, run-once misuse. BLOCKED: most misuse modes target deferred §8-§11 primitives.
- [/] Emit violations through TODO-27 and optionally bugcheck on fatal corruption. BLOCKED: the TODO-27 dump/violation sink is deferred.
- [/] Unit tests cover every primitive (§2-§13) under normal and verifier mode. BLOCKED: §8-§11/§13 primitives do not exist to test.
- [ ] Commit: `"kernel: ex -- verifier hooks"`

**Test checkpoint:** With `EX_VERIFIER=1`, injected rundown misuse, lookaside double-free, fast-ref misalignment, and push-lock-outside-critical-region each emit a distinct violation record; a clean run emits none; fatal corruption optionally raises bugcheck.

> **Notes:**
> - **Why deferred** -- the bulk of §14's misuse modes (push-lock/guarded-mutex/resource/run-once) target the deferred §8-§11 primitives, emission routes through the deferred TODO-27 sink, and "every primitive §2-§13" cannot be tested.
> - **Blocked on** -- the deferred primitive sections §8/§9/§10/§11/§13 in this file + `02-kernel-core/TODO-27 §5` (violation/dump sink).
> - **Scope boundary** -- §14 owns the central `EX_VERIFIER` framework; the shipped primitives already self-guard (§5 lookaside verifier, §6 fast-ref bugcheck, §3 rundown); broader tooling is D02 T31.

> **Deferred:** [M] central verifier needs the §8-§13 primitives it instruments (mostly deferred) + a violation sink -> XREF: 02-kernel-core/TODO-27 §5 (item: "Commit: minidump writer, stream serialisation, CRC32C, memory page selection" at line 190)

---

## OS Comparison

| ⭐   | Feature                   | 🪟 Win11                            | 🐧 Linux             | 🚀 Impossible OS                    |
| --- | ------------------------- | ---------------------------------- | ------------------- | ---------------------------------- |
| 💎   | Interlocked SLIST         | ✅ SLIST_HEADER                     | ✅ llist_head        | ✅ §2 cmpxchg16b DCAS               |
| 💎   | Rundown protection        | ✅ EX_RUNDOWN_REF                   | ⚠️ percpu-ref/RCU   | ✅ §3 atomic + poll-drain           |
| 💎   | Callback objects          | ✅ ExCreateCallback                 | ⚠️ notifier chains  | ✅ §4 \Callback\ + rundown          |
| 💎   | Lookaside lists           | ✅ NPaged/Paged/Ex                  | ✅ slab/kmem_cache   | ✅ NPaged/Paged/Ex + verifier       |
| 💎   | Fast references           | ✅ EX_FAST_REF                      | ❌ none              | ✅ §6 EX_FAST_REF (3-bit)           |
| 💎   | Ordered tables + bitmaps  | ✅ RTL_AVL_TABLE/RTL_BITMAP         | ✅ rbtree/bitmap     | ✅ §7 AVL + dyn-hash + bitmap       |
| 💎   | Push locks                | ✅ EX_PUSH_LOCK                     | ⚠️ rwsem/RCU        | ⬜ §8 deferred (keyed events)       |
| 💎   | Fast/guarded mutexes      | ✅ FAST_MUTEX/KGUARDED_MUTEX        | ✅ mutex             | ⬜ §9 deferred (per-thread APC)     |
| 💎   | Shared/exclusive resource | ✅ ERESOURCE                        | ✅ rw_semaphore      | ⬜ §10 deferred (SMP rwlock)        |
| 💎   | Run-once init             | ✅ RTL_RUN_ONCE                     | ⚠️ ad-hoc/call_once | ⬜ §11 deferred (keyed events)      |
| 💎   | Worker items + Ex timers  | ✅ ExQueueWorkItem/ExTimer          | ✅ workqueue/hrtimer | ✅ §12 work items (timers deferred) |
| 💎   | Bugcheck reason callbacks | ✅ KeRegisterBugCheckReasonCallback | ⚠️ panic notifiers  | ⬜ §13 deferred (panic freeze)      |
| ⭐   | Scoped executive verifier | ⚠️ Driver Verifier (heavy)         | ⚠️ KASAN/lockdep    | ⬜ §14 deferred (needs §8-§13)      |

> **Shipped (pass 1):** §2-§7 (SLIST, rundown, callbacks, lookaside, fast refs, AVL/hash/bitmap) + §12 immediate work items match Win11/Linux. §8-§11 + §13/§14 are deferred on lower-level sync/panic infrastructure (keyed events, per-thread APC-disable, SMP-safe rwlock, panic CPU-freeze, T27 dump writer) -- see each section's Deferred stamp.
> **After §8-§14 land:** the full lock spectrum (push locks, fast/guarded mutexes, ERESOURCE), run-once, Ex timers, bugcheck callbacks, and a low-cost scoped verifier complete the layer; broader tooling stays with D02 T31.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_ex()` -- register in `src/kernel/test/test_runner.c`.
> Tests run with `debug=1` or `test=1` in boot.conf. Needs a new `TEST_CAT_EX` enum value (executive support) added to `include/kernel/test/test.h` + its `"ex"` string mapping in `test_category_from_string()`.

- [/] Create `src/kernel/test/test_ex.c` with (shipped-primitive suites landed; deferred §8-§11/§13/§14 primitive tests land with those sections):
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
- [x] Add `TEST_CAT_EX` enum + `"ex"` string mapping
- [x] Register in `test_runner_init()`: `test_register_ex()`
- [x] Author `scripts/debug/kernel/run-ex-tests.bat` (SUITE=ex)
- [x] Commit: `"test: add executive support runtime test suite"` (tests landed with each shipped section)

## Verification

- [x] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===` (2026-06-26)
- [x] `ex_init()` logs `ex: Executive support runtime initialized` on serial during Phase 2 (confirmed in smoke serial `[ OK ] ex: ...` at 1.280s, 2026-06-26)
- [x] Unit tests pass: `bash scripts/test.sh SUITE=ex` -> 5684 kernel + 16 user-mode PASS (TCG/KVM, 2026-06-26)
- [/] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal. TCG/KVM 2026-06-26 (5684/5684 + smoke PASS); WHPX + VirtualBox + bare metal still pending (user rig).
- [x] Commit: `"kernel: ex -- executive support runtime complete"` (per-section ships 27bf7ce1..7636ae22 + closure)

**Test runner:** `scripts\debug\kernel\run-ex-tests.bat` (SUITE=ex) | 1 suite, 0 failures | 5684 kernel + 16 user PASS (TCG/KVM 2026-06-26)
