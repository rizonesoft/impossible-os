# TODO-06 -- Executive Support Runtime

> **Goal:** Complete the NT Executive-style support layer that sits above raw locks and scheduler mechanics: callback objects, rundown protection, lookaside lists, fast references, generic tables, resource objects, worker items, guarded regions, executive timers glue, bugcheck reason callbacks, and reusable verifier hooks. These primitives are small individually, but they are the connective tissue required by drivers, registry, SRM, ALPC, power, object callbacks, and file I/O.

> [!IMPORTANT]
> **Current state:** The tree has individual spinlocks, events, mutexes, condition variables, semaphores, workqueues, and DPCs. There is no coherent `Ex*` support API surface, no callback object type, no rundown protection, no NPaged lookaside lists, no fast-reference helper, no generic table implementation, and no central verifier hooks.

## Inputs

- [`src/kernel/sched`](../../src/kernel/sched/)
- [`src/kernel/ob`](../../src/kernel/ob/)
- [`include/kernel`](../../include/kernel/)
- → XREF: [`TODO-05-object-manager.md`](./TODO-05-object-manager.md) -- object callbacks and object type registration
- → XREF: [`TODO-07-irql-model-dpcs.md`](./TODO-07-irql-model-dpcs.md) -- IRQL/APC/DPC rules
- → XREF: [`03-memory-concurrency/TODO-08-advanced-sync.md`](../03-memory-concurrency/TODO-08-advanced-sync.md) -- low-level lock algorithms
- → XREF: [`TODO-31-kernel-bulletproofing.md`](./TODO-31-kernel-bulletproofing.md) -- invariants for fast refs and callback lists

## Outcome

- Kernel code can use stable `Ex*` primitives instead of inventing one-off lists and callbacks.
- Callback registration/unregistration is reference-safe and survives concurrent dispatch.
- Rundown protection safely tears down objects with active users.
- Lookaside lists reduce hot-path allocation churn for registry, ALPC, objects, and I/O.
- Generic AVL/splay tables replace ad-hoc sorted arrays where kernel-core needs ordered maps.
- Verifier can instrument Executive primitives with low runtime cost.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | Executive headers and namespace | -- | [ ] |
| 💎 | 2 | Rundown protection | atomics, events | [ ] |
| 💎 | 3 | Callback objects | T05 | [ ] |
| 💎 | 4 | NPaged lookaside lists | heap | [ ] |
| 💎 | 5 | Fast references | T05 | [ ] |
| 💎 | 6 | Generic tables | §1 | [ ] |
| 💎 | 7 | Executive resource wrapper | T07, D03 | [ ] |
| ⭐ | 8 | Worker item and delayed work glue | DPC/workqueue | [ ] |
| 💎 | 9 | Bugcheck reason callbacks | T27 | [ ] |
| ⭐ | 10 | Executive verifier hooks | §2..§9 | [ ] |

## 1. Executive Headers and Namespace

- [ ] Add `include/kernel/ex.h` and `src/kernel/ex/`.
- [ ] Prefix exported APIs with `Ex`, internal helpers with `Exp`.
- [ ] Document IRQL contract for every API.
- [ ] Add `ex_init()` boot hook in Phase 2 after Object Manager and before registry/security consumers.

## 2. Rundown Protection

- [ ] Implement `EX_RUNDOWN_REF` with acquire, release, wait, reinitialize, and completed query.
- [ ] Guarantee acquire fails after rundown begins.
- [ ] Wait path must block only at PASSIVE_LEVEL.
- [ ] Convert ALPC ports, section views, process objects, and object callbacks to use rundown where teardown races exist.

## 3. Callback Objects

- [ ] Add `ExCreateCallback`, `ExRegisterCallback`, `ExUnregisterCallback`, `ExNotifyCallback`.
- [ ] Back callback objects with Object Manager type `Callback`.
- [ ] Support named callbacks under `\Callback\`.
- [ ] Built-in callbacks: process create, thread create, image load, registry change, power setting, code integrity decision.
- [ ] Dispatch callbacks under rundown protection so unregister can wait safely.

## 4. NPaged Lookaside Lists

- [ ] Implement `NPAGED_LOOKASIDE_LIST` with depth, allocate/free counters, hit/miss stats, and tag.
- [ ] Add `ExInitializeNPagedLookasideList`, `ExAllocateFromNPagedLookasideList`, `ExFreeToNPagedLookasideList`, `ExDeleteNPagedLookasideList`.
- [ ] Enforce fixed-size allocations and poison freed entries in verifier mode.
- [ ] Consumers: registry notification records, ALPC messages, object namespace entries, klog v2 drain buffers.

## 5. Fast References

- [ ] Implement `EX_FAST_REF`: pointer plus low-bit refcount packing with alignment asserts.
- [ ] Provide acquire, release, exchange, and get-object helpers.
- [ ] Use only for object pointers with guaranteed alignment and Object Manager reference semantics.
- [ ] Add tests for saturation fallback to full Ob reference.

## 6. Generic Tables

- [ ] Implement AVL-backed `RTL_AVL_TABLE`-style generic table for kernel-core users.
- [ ] APIs: initialize, insert, lookup, delete, enumerate, enumerate without splaying.
- [ ] Pluggable compare, allocate, and free callbacks.
- [ ] Consumers: atom tables, loaded-image registry, tunable registry, named notification states.

## 7. Executive Resource Wrapper

- [ ] Provide `ERESOURCE`-style shared/exclusive resource wrapper over the owning synchronization primitives.
- [ ] Support recursive exclusive acquisition only when explicitly initialized with that flag.
- [ ] Add owner tracking in verifier mode.
- [ ] Consumers: registry hive locks, NLS table reload locks, image registry lock.

## 8. Worker Item and Delayed Work Glue

- [ ] Add `ExInitializeWorkItem`, `ExQueueWorkItem`, `ExQueueDelayedWorkItem`, `ExCancelWorkItem`.
- [ ] Ensure work items run at PASSIVE_LEVEL.
- [ ] Route immediate work to existing workqueue; route delayed work through timer/DPC handoff.
- [ ] Use in config tunable callbacks, notification fanout, and health probes.

## 9. Bugcheck Reason Callbacks

- [ ] Add `KeRegisterBugCheckReasonCallback` and unregister equivalent.
- [ ] Callback classes: add-pages, secondary dump data, log snapshot, blackbox data.
- [ ] Integrate with TODO-27 dump writer and TODO-28 panic UI.
- [ ] Ensure callbacks cannot allocate in panic path unless marked panic-safe.

## 10. Executive Verifier Hooks

- [ ] Add `EX_VERIFIER=1` boot/config flag.
- [ ] Track callback leaks, rundown misuse, lookaside double-free, fast-ref alignment, resource lock order.
- [ ] Emit violations through TODO-27 and optionally bugcheck on fatal corruption.
- [ ] Unit tests cover every primitive under normal and verifier mode.

