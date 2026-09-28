<!-- docs: covers=todo/02-kernel-core/TODO-06-executive-support-runtime.md sources=include/kernel/ex.h,src/kernel/ex/ex.c,src/kernel/ex/ex_slist.c,src/kernel/ex/ex_rundown.c,src/kernel/ex/ex_callback.c,src/kernel/ex/ex_lookaside.c,src/kernel/ex/ex_fastref.c,src/kernel/ex/ex_avltable.c,src/kernel/ex/ex_hashtable.c,src/kernel/ex/ex_bitmap.c,src/kernel/ex/ex_workitem.c,src/kernel/test/test_ex.c,src/kernel/main/boot_storage.c reviewed=2026-09-28 order=6 -->
# Executive Support Runtime

## What is it?

The executive support runtime is the `Ex*` and `Rtl*` primitive layer between raw spinlocks and events and the subsystems that use them: the Registry, the Object Manager, ALPC, power and file I/O. It is the equivalent of the Windows NT executive support API: callback objects, rundown protection, lock-free lists, lookaside allocators, fast references and generic containers, so kernel code does not invent one-off lists and flags.

Sections 1 to 7 of the roadmap have shipped: the executive boot hook, the interlocked SLIST, rundown protection, callback objects, lookaside lists, fast references, and the AVL, hash and bitmap containers. Immediate work items have also shipped. Push locks, fast and guarded mutexes, the ERESOURCE wrapper, run-once initialization, delayed work and Ex timers, bugcheck reason callbacks and the executive verifier are designed but blocked on lower-level primitives that do not exist yet.

## How does it work?

`ex_init()` runs once on the BSP in `boot_phase2()`, after `ob_init()` and before `registry_init()`, and logs `ex: Executive support runtime initialized`. It then calls `ex_callback_init()`, which creates the `\Callback\` directory and six built-in callback objects (`ProcessCreate`, `ThreadCreate`, `ImageLoad`, `RegistryChange`, `PowerState`, `CodeIntegrity`) that nothing fires yet. [`boot_storage.c`](../../src/kernel/main/boot_storage.c) then marks `SUBSYS_EX` ready, so `ex_ready()` is safe to poll from any CPU after Phase 2.

**The SLIST** is the free-list spine everything else builds on. `SLIST_HEADER` is a 16-byte pair swung atomically by one `cmpxchg16b`, so the pointer is a full 64-bit value and the depth changes with it in the same instruction. A sequence field defeats ABA, and the depth saturates at 65,535 (`SLIST_DEPTH_MAX`) instead of wrapping. The CPU must support `CMPXCHG16B`, which boot requires on every CPU.

**Rundown protection** (`EX_RUNDOWN_REF`) packs an active flag and a reference count into one atomic word. Acquire and release are lock-free; the drain waits at PASSIVE_LEVEL by yielding in a loop, because the event primitive has a lost-wakeup race. That loop assumes holders run at or above the waiter's priority, a precondition the roadmap tracks.

**Callback objects** build on rundown protection. Each of up to 8 registrations (`EX_CALLBACK_MAX_SLOTS`) has its own rundown reference: `ExNotifyCallback` holds it while calling the routine, and `ExUnregisterCallback` drains every outstanding reference, on every CPU, before returning, so the caller can free its context. A registration cookie carries a slot generation, so a stale cookie for a reused slot is rejected. A routine must never unregister itself from inside its own callback, which would wait on its own reference.

**Lookaside lists** cache fixed-size blocks. Allocation and free are lock-free SLIST operations; nonpaged lists are safe up to DISPATCH_LEVEL, while every operation on a paged list, even a cache hit, requires APC_LEVEL or below and is refused above it. Refilling on a miss and trimming an over-full list call `kmalloc` and `kfree` and run only at APC_LEVEL or below; a nonpaged miss above that returns `NULL` instead of touching the heap. The default depth is 256 (`EX_LOOKASIDE_DEFAULT_DEPTH`) and the cap is 4,096 (`EX_LOOKASIDE_MAX_DEPTH`).

**Fast references** (`EX_FAST_REF`) cache a few object references inside one pointer-sized word, so a hot lookup can hand out a reference without touching the object's shared count. `kmalloc` guarantees only 8-byte alignment, so 3 low bits are free and at most 7 references are cached (`EX_FAST_REF_MAX`), as on 32-bit Windows.

**The containers** follow the Windows `Rtl` contract that the caller serializes access: `RTL_BITMAP` over a caller buffer, `RTL_AVL_TABLE` with caller-supplied allocation, and `RTL_DYNAMIC_HASH_TABLE`, which only grows, so a remove-during-walk cursor stays valid across a resize.

**Immediate work items** (`EX_WORK_ITEM`) run on the system work queue with an atomic state machine: idle, queued, running, then done or cancelled. The work queue cannot dequeue, so `ExCancelWorkItem` succeeds only while the item is still queued.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `ex_init()`, `ex_ready()` | Boot hook and readiness check ([`ex.c`](../../src/kernel/ex/ex.c), [`ex.h`](../../include/kernel/ex.h)) |
| `ExInterlockedPushEntrySList`, `ExInterlockedPopEntrySList`, `ExInterlockedFlushSList`, `ExQueryDepthSList` | Lock-free LIFO list ([`ex_slist.c`](../../src/kernel/ex/ex_slist.c)) |
| `ExAcquireRundownProtection`, `ExReleaseRundownProtection`, `ExWaitForRundownProtectionRelease` | Safe teardown barrier ([`ex_rundown.c`](../../src/kernel/ex/ex_rundown.c)) |
| `ExCreateCallback`, `ExRegisterCallback`, `ExUnregisterCallback`, `ExNotifyCallback` | Named notification points ([`ex_callback.c`](../../src/kernel/ex/ex_callback.c)) |
| `ExInitializeNPagedLookasideList`, `ExAllocateFromNPagedLookasideList`, `ExFreeToNPagedLookasideList` and the paged and `Ex` variants | Fixed-size block caches ([`ex_lookaside.c`](../../src/kernel/ex/ex_lookaside.c)) |
| `ExAcquireFastReference`, `ExReleaseFastReference`, `ExCompareSwapFastReference` | Cached object references ([`ex_fastref.c`](../../src/kernel/ex/ex_fastref.c)) |
| `RtlInsertElementGenericTableAvl` and related | Ordered AVL table ([`ex_avltable.c`](../../src/kernel/ex/ex_avltable.c)) |
| `RtlInsertEntryHashTable`, `RtlLookupEntryHashTable`, `RtlRemoveEntryHashTable` | Growing hash table ([`ex_hashtable.c`](../../src/kernel/ex/ex_hashtable.c)) |
| `RtlSetBits`, `RtlClearBits`, `RtlFindClearBitsAndSet` and related | Bitmaps over caller buffers ([`ex_bitmap.c`](../../src/kernel/ex/ex_bitmap.c)) |
| `ExInitializeWorkItem`, `ExQueueWorkItem`, `ExCancelWorkItem` | Immediate work items ([`ex_workitem.c`](../../src/kernel/ex/ex_workitem.c)) |

## How do I use it?

`ex_init()` runs on every boot; there is no setting. The test suite covers every shipped primitive:

```bash
bash scripts/test.sh SUITE=ex
```

From kernel code, allocate the control structure (most are caller-owned), call its initialize function, and respect the IRQL ceiling documented on each declaration in [`ex.h`](../../include/kernel/ex.h). The tests in [`test_ex.c`](../../src/kernel/test/test_ex.c) show each primitive in use.

## What is not implemented yet?

- **Push locks** need an address-keyed park and wake primitive, because spinning with yields on a contended lock can deadlock the strict-priority scheduler ([Push Locks](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md#8-push-locks)).
- **Fast and guarded mutexes** need a per-thread APC disable that survives a context switch ([Fast and Guarded Mutexes](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md#9-fast-and-guarded-mutexes)).
- **The ERESOURCE wrapper** waits for a reader/writer lock that is correct on SMP; the current `rwlock_t` can admit readers and a writer together ([Executive Resource Wrapper](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md#10-executive-resource-wrapper)).
- **Run-once initialization** needs the same address-keyed wait object as push locks ([Run-Once Initialization](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md#11-run-once-initialization)).
- **Delayed work items and Ex timers** need a cancellable multi-deadline timer queue ([Worker Items, Delayed Work, and Ex Timers](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md#12-worker-items-delayed-work-and-ex-timers)).
- **Bugcheck reason callbacks** need every CPU frozen at panic time and a dump writer to consume the data ([Bugcheck Reason Callbacks](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md#13-bugcheck-reason-callbacks)).
- **The executive verifier** mostly checks misuse of the primitives above ([Executive Verifier Hooks](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md#14-executive-verifier-hooks)).

## How does it compare with Windows 11 and Linux?

For what has shipped, the surface matches Windows closely: the SLIST uses the same `cmpxchg16b` design as Windows `SLIST_HEADER`; rundown protection mirrors `EX_RUNDOWN_REF` (Linux uses `percpu-ref` and RCU); callback objects mirror `ExCreateCallback` (Linux uses notifier chains); lookaside lists match the Windows split (Linux uses `kmem_cache`); and the containers match `RTL_AVL_TABLE` and `RTL_BITMAP` (Linux has `rbtree` and bitmaps). Fast references have no Linux counterpart and cache 3 bits here against 4 on 64-bit Windows.

Push locks, fast and guarded mutexes, ERESOURCE, run-once, bugcheck reason callbacks, delayed work and a verifier are gaps against both systems until their prerequisites land.

## See also

- [Executive Support Runtime roadmap](../../todo/02-kernel-core/TODO-06-executive-support-runtime.md)
- [Object Manager](object-manager.md)
- [IRQL, DPCs and APCs](irql-dpc.md)
