---
schema_version: 1
id: object-manager
domain: 02-kernel-core
status: active
title: "TODO-05 -- Object Manager"
---

# TODO-05 -- Object Manager

> **Validated:** 2026-06-21 | validate-todo-file clean (structure / IO table / XREF / test wiring); sections §1-§15 implemented + unstamped (review pending in SECTIONS)
> **Gap-audited:** 2026-06-21 | mature TODO, confirmatory parity pass (Win11 24H2/25H2 + Linux 6.x) -- no new baseline gaps; code-truth audit confirms §1-§15 implemented + wired; codex-gap-audit skipped (zero new sections/ownership/parity claims)

> **Goal:** Implement the kernel Object Manager (ObXxx layer) -- the unified substrate that gives every kernel resource (files, processes, threads, events, mutexes, semaphores, registry keys, sections) a typed header, reference-counted lifetime, named namespace entry, security descriptor, and per-process handle-table slot. Without this, Win32 `HANDLE` semantics are impossible and resource leaks are unavoidable. This is the single most foundational Win32 prerequisite in the kernel.

> [!IMPORTANT]
> **Current state:** Object Manager is feature-complete (§1-§15 all implemented). OBJECT_HEADER, OBJECT_TYPE, reference counting, handle tables, namespace, all major object types, security descriptors, NtClose/NtDuplicateObject/NtQueryObject, handle inheritance, namespace browser API, per-type statistics (`ob.c`), object callbacks (`ob_callback.c`), per-process handle quota (`handle_table.c`), and handle tracing/leak detection (`ob_trace.c`) are implemented and verified on QEMU WHPX + TCG. The numbered sections are shipped old-system work pending the dual-stamp (Verified + Quality reviewed) review pass.

## Inputs

- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h)
- [`include/kernel/fs/vfs.h`](../../include/kernel/fs/vfs.h)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- process/thread struct
- [`src/kernel/sched/event.c`](../../src/kernel/sched/event.c)
- [`src/kernel/sched/mutex.c`](../../src/kernel/sched/mutex.c)
- [`src/kernel/sched/semaphore.c`](../../src/kernel/sched/semaphore.c)
- [`src/kernel/ipc/`](../../src/kernel/ipc/) -- pipes, shared memory
- → XREF: `TODO-01-kernel-init-sequencing.md §4` -- ObInit (`object_manager_init()`) is a Phase 2 gate, before registry
- → XREF: `TODO-14-registry-completion.md §5` -- register HKEY handles in per-process handle table (DuplicateHandle parity); registry engine in `registry.c` until TODO-14 wires Ob
- → XREF: `TODO-11-peb-teb-user-abi.md` -- TODO-11 §10 exposes PEB/TEB in the Ob namespace (depends on §8 Object Namespace and §11 Namespace Browser)
- → XREF: `TODO-12-native-api-ssdt.md` -- NtCreateFile / NtOpenFile / NtClose and sync Nt syscalls are all Ob-routed; SSDT entries depend on §4, §6, §8
- → XREF: `TODO-17-binary-system.md §5` -- `exec_load()` uses §2 process object registration/lifetime semantics when wiring executable process objects into the Ob namespace
- → XREF: `TODO-15-security-reference-monitor.md` -- SRM enforces DACLs registered here; §13 callbacks enable PPL-style protection
- → XREF: `TODO-12-native-api-ssdt.md §10` -- `NtQuerySystemInformation(SystemHandleInformation)` consumes §12 per-type statistics
- → XREF: `TODO-04-system-logging.md` -- §15 handle tracing integrates with klog subsystem

## Outcome

- Every kernel object carries an `OBJECT_HEADER` with refcount, type pointer, name, and security descriptor.
- `ObCreateObject` / `ObReferenceObject` / `ObDereferenceObject` manage all object lifetimes.
- Every process owns a handle table mapping `HANDLE` integers to (object, access rights, attributes).
- Named objects live in a hierarchical namespace: `\`, `\Device\`, `\BaseNamedObjects\`, `\DosDevices\`.
- `NtCreateFile`, `NtOpenFile`, `NtClose`, `NtDuplicateObject` are routed through the Ob layer.
- Existing IPC primitives are re-registered as Ob-managed named object types.
- Per-type object and handle statistics (current + peak) are tracked and queryable via `NtQueryObject(ObjectTypeInformation)`.
- Kernel drivers can register pre/post callbacks on handle create and duplicate operations via `ObRegisterCallbacks`.
- Per-process handle quota is configurable and enforced, preventing resource exhaustion.
- Tagged reference tracking and optional handle tracing enable kernel-level leak detection integrated with klog.

## Implementation Order

| ⭐  | Order | Deliverable                                                   | Depends On    | Status |
| --- | :---: | ------------------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | OBJECT_HEADER and OBJECT_TYPE infrastructure                  | --            |  [x]   |
| 💎  |   2   | Reference counting and object lifetime                        | §1            |  [x]   |
| 💎  |   3   | Per-process handle table                                      | §2            |  [/]   |
| 💎  |   4   | Object namespace (directory + symbolic link)                  | §2            |  [/]   |
| 💎  |   5   | File, Process, Thread object types                            | §3, §4        |  [/]   |
| 💎  |   6   | Synchronisation object types (Event, Mutex, Semaphore, Timer) | §3, §4        |  [/]   |
| 💎  |   7   | Section (shared memory) object type                           | §3, §4        |  [/]   |
| 💎  |   8   | Security descriptor integration                               | §1, T15 §1,§3 |  [x]   |
| 💎  |   9   | NtClose / NtDuplicateObject / NtQueryObject                   | §3, T15 §4    |  [/]   |
| 💎  |  10   | Handle inheritance across CreateProcess                       | §3, §5        |  [/]   |
| ⭐  |  11   | Unified kernel-user namespace browser API                     | §4            |  [/]   |
| 💎  |  12   | Per-type object and handle statistics                         | §1, §2, §3    |  [x]   |
| 💎  |  13   | Object callbacks -- handle operation filtering                | §3, §9        |  [x]   |
| 💎  |  14   | Per-process handle quota                                      | §3            |  [x]   |
| ⭐  |  15   | Handle tracing and leak detection                             | §2, §12       |  [x]   |

> 💎 = parity -- Windows NT ObXxx and Linux kobject/fd_table both provide these capabilities.
> ⭐ = exclusive -- built-in handle/reference leak detection integrated with klog, providing
>     first-class kernel-level diagnostics that neither Windows ETW nor Linux exposes natively.

---

## 1. OBJECT_HEADER and OBJECT_TYPE Infrastructure
Every kernel object body is preceded in memory by an `OBJECT_HEADER`. Types are described by an `OBJECT_TYPE` singleton registered at boot.

- [x] Define `OBJECT_HEADER` in `include/kernel/ob/ob.h`:
  - `atomic_t ref_count` -- atomic reference count (SMP-safe)
  - `uint32_t handle_count` -- number of open handles
  - `const OBJECT_TYPE *type` -- pointer to the type descriptor
  - `const char *name` -- pointer into the name buffer (NULL if unnamed)
  - `SECURITY_DESCRIPTOR *security` -- NULL until §8
  - `uint32_t flags` -- `OB_FLAG_PERMANENT`, `OB_FLAG_KERNEL_ONLY`, `OB_FLAG_NAMED`
- [x] Define `OBJECT_TYPE` in `include/kernel/ob/ob_type.h`:
  - `const char *name` -- type name string (e.g. `"File"`, `"Process"`)
  - `void (*on_close)(void *body, uint32_t handle_count)` -- called when handle count drops to 0
  - `void (*on_delete)(void *body)` -- called when ref count drops to 0; frees body memory
  - `int (*on_open)(void *body, uint32_t access)` -- optional access check on open
  - `int (*on_parse)(void *body, const char *remaining, void **result)` -- namespace parse
  - `size_t body_size` -- size of the object body (for combined allocation)
- [x] Implement `ob_create_type(name, callbacks, body_size)` -- registers a type in a static global type table
- [x] Implement `ob_alloc_object(type)` -- allocates `sizeof(OBJECT_HEADER) + type->body_size` via `kmalloc` or `pmm_alloc_contiguous` based on body size; returns pointer to the body (header is at `body - sizeof(OBJECT_HEADER)`)
- [x] Add `OB_HEADER_FROM_BODY(ptr)` macro -- subtracts header size from a body pointer
- [x] Register built-in type singletons at ObInit time: `ObpFileType`, `ObpProcessType`, `ObpThreadType`, `ObpDirectoryType`, `ObpSymlinkType`, `ObpEventType`, `ObpMutexType`, `ObpSemaphoreType`, `ObpSectionType`, `ObpTimerType`
- [x] Commit: `"kernel: ob -- OBJECT_HEADER and OBJECT_TYPE infrastructure"`
- [/] **Budget the `g_ob_types` table.** 64 slots, append-only, no release path; ~37/64 in one boot with test throwaways, and it tipped 32/32 once. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §13`
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - The cross-TODO owner is already named in the item text above and remains the primary dependency.

> **Notes:**
> - `OBJECT_HEADER` (`ob.h`) + `OBJECT_TYPE` (`ob_type.h`) are the typed-header substrate: `ob_create_type` registers type singletons, `ob_alloc_object` combined-allocates header+body, `OB_HEADER_FROM_BODY`/`OB_BODY_FROM_HEADER` convert.
> - `ob_alloc_object` tail-packs a per-object creator SD into the same allocation block for token tasks (one allocation, freed with the object); kernel-default objects share the immutable static default SD.
> - Type registration is serialised by `s_type_lock` and publishes the count via release/acquire so an unlocked `NtQueryObject(ObjectTypesInformation)` reader never observes a half-written row.
> - 13 built-in type singletons register at `ob_init` (File/Process/Thread/Directory/Symlink/Event/Mutex/Semaphore/Section/Timer/Peb/Teb + Info).
> - Validation: `test_ob.c` (`TEST_CAT_OB`), `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob).
> **Verified:** 2026-06-21 | ship `44db565a` + review fixes | 6/6 items | build OK | tests 323/323 PASS (SUITE=ob)
> **Deferred:** [M] `hdr->security` holds both absolute (creator) and self-relative (default) SDs with no discriminator -- the access checker must branch on `SE_SELF_RELATIVE` before dereferencing Owner/Dacl -> XREF: 02-kernel-core/TODO-15 §5 (item: "Implement `SeAccessCheck(...)`" sub-bullet 0 SD-format normalize at line 324)
> **Deferred:** [M] `SeCreateCreatorSD` stores `tok->UserSid` by pointer (no copy); latent dangle if a non-static SID-backed token is freed while the object lives -> XREF: 02-kernel-core/TODO-15 §5 (item: "Implement `SeAccessCheck(...)`" sub-bullet 0 owner-SID handling at line 324)
> **Quality reviewed:** 2026-06-21 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + auditor | 1Critical+2H+3M+2L fixed, 2M deferred | scope: kernel-code-quality

---

## 2. Reference Counting and Object Lifetime
- [x] Implement `ObReferenceObject(void *body)` -- atomic increment of `header->ref_count`
- [x] Implement `ObDereferenceObject(void *body)` -- atomic decrement; when count reaches 0, call `type->on_delete(body)` and free the combined allocation
- [x] Implement `ObReferenceObjectByPointer(void *body, uint32_t access)` -- validates type before ref
- [x] Permanent objects (`OB_FLAG_PERMANENT`) are never deleted when refcount hits 0; must be explicitly made temporary with `ObMakeTemporaryObject()` first
- [x] All existing code that stores raw `vfs_node *` pointers must be audited and wrapped in ObRef/ObDeref pairs -- track this as a follow-up checklist in §5
- [x] Commit: `"kernel: ob -- reference counting and object lifetime"`

> **Notes:**
> - `ObReferenceObject` is the blind atomic_inc fast path for callers that already hold a reference; `ObReferenceObjectSafe` (added in review) is the cmpxchg-if-nonzero primitive for lookup paths that may race a last-ref close.
> - `ObReferenceObjectByPointer` type-checks then safe-references; `-1` now means type-mismatch OR dead object.
> - `ObDereferenceObject` always decrements first (so it never leaks or under-decrements a reference), frees at 0 via `on_delete` + `ob_free_object`, and restores the standing reference for `OB_FLAG_PERMANENT` objects so they never underflow.
> - Permanent objects (namespace roots, info files, process/thread pseudo-objects) are torn down by `ObMakeTemporaryObject` + a final deref; the brief ref_count==0 restore window is benign (only at teardown).
> - Full lookup-then-reference atomicity (the memory-validity half) lands with the handle-table lock; validated by `test_ob.c` (`TEST_CAT_OB`).
> **Verified:** 2026-06-21 | ship `44db565a` + review fixes | 5/5 items | build OK | tests 323/323 PASS (SUITE=ob)
> **Deferred:** [Critical] handle lookup-then-reference is not atomic (ObpLookupHandle returns an unreferenced entry; a concurrent ObpFreeHandle can free the object before ObReferenceObject runs -- the safe-ref primitive closes count-0 resurrection but not the freed-memory window) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Quality reviewed:** 2026-06-21 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + auditor | 1H+2M fixed, 1Critical+2L deferred | scope: kernel-code-quality

---

## 3. Per-Process Handle Table
The handle table maps opaque `HANDLE` integer values to (object pointer + granted access + flags) within a single process. `HANDLE` values are always multiples of 4 (low bits reserved for inheritance flags).

- [x] Define `HANDLE_TABLE_ENTRY` in `include/kernel/ob/handle_table.h`:
  - `void *object` -- body pointer (NULL = free slot)
  - `uint32_t granted_access` -- access rights granted at open time
  - `uint32_t attributes` -- `OBJ_INHERIT`, `OBJ_PROTECT_CLOSE`
- [x] Define `HANDLE_TABLE` -- array of `HANDLE_TABLE_ENTRY` with grow-on-demand capacity (initial: 64 entries, grows by doubling; allocated via `kmalloc` for small tables, `pmm_alloc_contiguous` for tables exceeding 4 KB)
- [x] Add `HANDLE_TABLE handle_table` to the `task_t` / process struct in `task.c`
- [x] Implement `ObpAllocateHandle(table, object, access, attrs)` -- finds a free slot, stores entry, calls `ObReferenceObject`, returns `HANDLE` value (slot index × 4)
- [x] Implement `ObpFreeHandle(table, handle)` -- validates handle, calls `type->on_close` if handle_count drops to 0, calls `ObDereferenceObject`, zeroes slot
- [x] Implement `ObpLookupHandle(table, handle)` -- validates index bounds and non-NULL; returns entry ptr
- [x] Handle table freed at process exit: force-close every slot via `ObpFreeHandle`, clearing `OBJ_PROTECT_CLOSE` first so rundown ignores it (that bit only guards an explicit NtClose). Test: `test_ob_handle_table_destroy_forces_protected_close`
- [x] Special kernel pseudo-handles: `INVALID_HANDLE_VALUE (-1)`, `CURRENT_PROCESS (-2)`, `CURRENT_THREAD (-3)` -- resolve in `ObpLookupHandle` without a table entry
- [x] Commit: `"kernel: ob -- per-process handle table"`
- [/] **Wire HandleAttributes through `ObpAllocateHandle` for `NtXxxEx` syscalls**:
      - The `attrs` parameter is already plumbed through `ObpAllocateHandle` and stored in `HANDLE_TABLE_ENTRY.attributes`, but the `NtXxxEx` syscalls (`NtOpenProcessTokenEx`, `NtOpenThreadTokenEx`, `NtOpenKeyEx`, `NtCreateFileEx`, etc.) currently discard their `HandleAttributes` argument (see `src/kernel/nt/nt_token.c::NtOpenProcessTokenEx_handler` which explicitly `(void)a3`).
      - Update every `NtXxxEx` handler to pass validated `HandleAttributes` into `ObpAllocateHandle` (via library function or directly).
      - Validate against `OBJ_VALID_ATTRIBUTES = 0x000007F2`; reject with `STATUS_INVALID_PARAMETER` for unsupported bits.
      - This auto-closes TODO-12 §16 Accepted #3 (NtOpenProcessTokenEx HandleAttributes discarded).
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive**
      - That atomically (under a per-handle-table lock or via RCU-style load with refcount barrier): (1) validates index bounds and `entry->object != NULL`; (2) checks `entry->object`'s type matches `required_type` if non-NULL; (3) calls `ObReferenceObject(entry->object)` BEFORE releasing any concurrency guarantee so the body cannot be freed by a concurrent `ObpFreeHandle`; (4) stores `entry->granted_access` for the caller to enforce required access.
      - Callers release with `ObDereferenceObject(body)` when done.
      - Retrofit every `src/kernel/nt/nt_*.c` handler that currently does `entry = ObpLookupHandle(...); so = (T *)entry->object;` -- including `nt_sync.c` (all `sync_lookup` callers plus `wait_on_handle`/`NtWaitForMultipleObjects` Phase A/`NtSignalAndWait`, whose lookup-then-safe-ref pin still has the freed-header window), the whole of `nt_section.c` (NtMapViewOfSection, NtUnmapViewOfSection, NtExtendSection, NtQuerySection, NtAreMappedFilesTheSame via `section_copy_backing_path`), `nt_timer.c` (`resolve_timer_handle` used by NtSetTimer/NtCancelTimer/NtQueryTimer/NtSetTimerEx), `nt_registry.c`, `nt_namespace.c`, `nt_token.c`, `nt_process.c`, and `nt_alpc.c` via `src/kernel/ipc/alpc_port.c`'s `AlpcAcceptConnectPort` + `AlpcDisconnectPort` (both currently use `ObpLookupHandle` + explicit `ObReferenceObject` which has a close-race window).
      - Also retrofit `ObUnmapViewOfSectionByBase` in `src/kernel/ob/ob_section.c:414` which walks the handle table without any lock.
      - Also retrofit `NtQueryDirectoryObject` in `src/kernel/ob/ob.c:437` which Codex flagged 2026-04-22 during TODO-03 §9 review: the current code captures `entry->object` unlocked, then calls `ObReferenceObject(dir)` -- the window between capture and ref still races `NtClose` on another CPU.
      - Also retrofit `NtDuplicateObject` + `NtQueryObject` in `src/kernel/ob/ob.c` (the §9 review 2026-06-21 fixed the deterministic same-table-grow UAF and the access-escalation in `NtDuplicateObject`; the residual concurrent-close lookup-then-deref race in both stays owned here).
      - This auto-closes TODO-12 §18 Accepted (initial-handle-lookup UAF race), TODO-12 §19 Accepted (timer handle UAF race), TODO-05 §2 Accepted (AlpcAcceptConnectPort/Disconnect handle-lookup race), D00 T03 §9 Accepted (NtQueryDirectoryObject residual handle-close race), and is a prerequisite for the `ObpReferenceObjectByHandle` SeAccessCheck integration already referenced in TODO-15 §5.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Add per-task view-base index** to eliminate full handle-table walks on unmap and mapped-file-compare.
      - `ObUnmapViewOfSectionByBase` (`src/kernel/ob/ob_section.c:414`) and `ObAreMappedFilesTheSame` (`ob_section.c:604,652`) both iterate every handle-table entry and then every SECTION_MAX_VIEWS slot per section -- O(handle_capacity × SECTION_MAX_VIEWS) per call.
      - Add a small hash or sorted array keyed by `(task_pid, base_addr)` -> `{SECTION_OBJECT *, view_index}` attached to `struct task` (or installed at task creation beside `handle_table`).
      - Update on `ObMapViewOfSectionFull` (insert) and `ObUnmapViewOfSection`/`ObUnmapViewOfSectionByBase` (remove).
      - Both callers become near-O(1).
      - This auto-closes TODO-12 §18 Accepted (handle-table scan scalability).
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Walk section views on task_cleanup to drop leaked view pins.**
      - `task_cleanup` in [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) destroys the handle table but does NOT walk per-task section views, so any task that dies without calling `ObUnmapViewOfSectionByBase` (or `sys_unmapview` on the INT 0x80 path) leaks the `ObReferenceObject(so)` pin that `ObMapViewOfSectionFull` took, keeping the section backing PMM frames pinned until reboot.
      - Additionally, `ObUnmapViewOfSectionByBase` today finds the view by scanning the HANDLE TABLE for live section handles -- once the caller closes the section handle, the view becomes unreachable even though its pin still exists.
      - Add a per-task view index (either the one from the preceding bullet, or a separate array) that `task_cleanup` walks after `ob_handle_table_destroy`, calling `ObUnmapViewOfSectionByBase` (or an inner helper that takes the SECTION_OBJECT pointer directly) for each still-mapped view.
      - XREFs: `00-infrastructure/TODO-04-usermode-test-framework.md §11` introduced `SYS_UNMAPVIEW` (46) and explicitly documents the "must unmap before close" ordering as a workaround for this gap.
      - Codex quality 2026-04-21 M2.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

- [/] **Give `HANDLE_TABLE` an owning-task back-pointer** so `ObpAllocateHandle` charges `total_handles_created` to the table owner, not `task_current()` (cross-process `NtDuplicateObject` mis-attributes; §14). Lands with the item-17 lock pass.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Push `RLIMIT_NOFILE` into `handle_table.handle_limit` under the table lock** (encodings invert; `quota_policy_effective_handle_limit()` reconciles READ-only); item-17 lock pass. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §8`
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - The cross-TODO owner is already named in the item text above and remains the primary dependency.

> [!WARNING]
> **Handle-table lock contract (from the 2026-06-21 Codex design review -- implement `ObpReferenceObjectByHandle` to THIS spec, a naive "lock everything" deadlocks):**
> - **Never run `type->on_close` / `type->on_delete` / `ObDereferenceObject` under the table lock.** Those callbacks take VFS/pipe/timer/ALPC locks, `kfree`, and can re-enter the handle table. Under the lock: validate the handle, clear the slot, decrement the table count, update the per-type handle statistics (`OBJECT_TYPE.total_handles` / `peak_handles` via the `handle_stat_lift_peak` CAS-max), and CAPTURE `body` + a last-handle boolean. Release the lock, THEN call `on_close` and `ObDereferenceObject`. The per-type handle-stat increment/decrement MUST live inside the same serialized region as the slot claim/clear so each logical handle create/close accounts exactly once -- today the stats are atomic but the slot mutation is not, so concurrent same-table create/close can double-count or drive `total_handles` negative (§12 review 2026-06-22; export path floors at 0 via `ob_stat_export` as a stopgap).
> - **`OBJECT_HEADER.handle_count` must become atomic** (it lives in the shared header and is mutated by every table holding the object via duplicate/inherit/open). The last-handle transition must be a single atomic fetch-sub so exactly one closer observes old==1 and fires `on_close`; a per-table lock alone cannot serialise it.
> - **Do not allocate/copy/free in the grow path under the spinlock** (`handle_table_grow` calls `kmalloc`/`pmm_alloc_contiguous`). Use retry-growth: observe full under lock, drop lock, allocate the replacement, reacquire, revalidate capacity, swap, free the old storage after unlock.
> - **`NtDuplicateObject` must not hold two table locks** (ABBA deadlock on dup A->B vs B->A). Pin the source object via `ObpReferenceObjectByHandle` (releases the source lock), allocate the destination handle, then drop the temporary pin; handle `src_ht == dst_ht` specially.

> **Notes:**
> - Base per-process handle table is shipped + working: `HANDLE_TABLE` in the task struct, `ObpAllocateHandle`/`ObpFreeHandle`/`ObpLookupHandle`, grow-on-demand, inheritance, kernel pseudo-handles (`handle_table.c`).
> - SMP-safety hardening (per-handle-table lock + `ObpReferenceObjectByHandle` atomic lookup-then-ref, item 17) is DEFERRED to a focused pass; the corrected lock contract from the 2026-06-21 Codex design review is in the WARNING callout above.
> - Perf items (per-task view-base index, `task_cleanup` view-pin walk) and the `NtXxxEx` HandleAttributes wiring also remain open `[ ]` here.
> - This deferral is why `ObReferenceObjectSafe` (§2) closes only count-0 resurrection, not the freed-memory window; that window closes when item 17 lands.
> **Deferred:** [Critical] handle lookup-then-reference not atomic + SMP-hardening/perf items need a focused implementation pass (design contract in the §3 WARNING callout) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)

---

## 4. Object Namespace
A hierarchical in-memory namespace rooted at `\`. Directories hold named object entries. Symbolic links redirect name lookups.

- [x] Define `OBJECT_DIRECTORY_ENTRY` -- linked list node: `name[64]`, `void *object`
- [x] Implement `OBJECT_DIRECTORY` body type -- list of entries, lock, parent pointer
- [x] Implement `ObpLookupDirectory(path, &remaining)` -- walks `\Foo\Bar` splitting on `\`; follows symlinks; stops at the deepest found directory
- [x] Implement `ObInsertObject(object, name, directory)` -- adds a named entry; fails if name exists and object is not a permanent replacement
- [x] Implement `ObLookupObjectByName(path, type, access, &result)` -- full parse walk calling `type->on_parse` at each node; calls `ObReferenceObject` on success
- [x] Implement `OBJECT_SYMBOLIC_LINK` body type -- target string; `on_parse` redirects the walk
- [/] Named-object create must be transactional: roll back the `ObInsertObject` publication when handle allocation fails, so a failed create leaves no persistent named object. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7`
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - The cross-TODO owner is already named in the item text above and remains the primary dependency.
- [x] Create the root namespace at ObInit: `\`, `\Device`, `\KernelObjects`, `\DosDevices`, `\BaseNamedObjects`, `\Sessions\0\BaseNamedObjects` (alias for user-mode named objects)
- [x] `C:` → `\Device\HardDisk0\Partition0` via symlink in `\DosDevices`
- [x] Commit: `"kernel: ob -- object namespace directory and symlinks"`
- [/] **Give `Directory` an `on_delete` draining its entry list.** Registered bare at `ob.c:809`, so freeing a non-empty directory leaks every entry node and child ref. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §13`
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - The cross-TODO owner is already named in the item text above and remains the primary dependency.
- [/] **Status-bearing insertion primitive.** `ObInsertObject` returns 0/-1 (`ob_ns.h:65`), conflating collision, capacity, OOM and quota refusal. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §13`
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - The cross-TODO owner is already named in the item text above and remains the primary dependency.
- [/] **Cache parent-directory + entry linkage in `OBJECT_HEADER` so teardown avoids path relookup.**
      - Today `ob_thread_mark_dead()` ([`src/kernel/ob/ob_thread.c:90-113`](../../src/kernel/ob/ob_thread.c)) and `ob_process_mark_dead()` ([`src/kernel/ob/ob_process.c:89-108`](../../src/kernel/ob/ob_process.c)) do `ObLookupObjectByName()` (path walk over `\KernelObjects\Thread<pid>.<tid>`) plus `ObpLookupDirectory()` plus `ObpRemoveFromDirectory()` linear walk -- two O(n) namespace scans under IRQ-off spinlock on every thread exit.
      - `thread_exit()` calls this on the scheduler hot path; cost scales with namespace size and pushes IRQ-off latency up under kthread churn.
      - Fix: add two fields to `OBJECT_HEADER` -- `OBJECT_DIRECTORY *name_dir` and `OBJECT_DIRECTORY_ENTRY *name_entry` -- populated by `ObInsertObject()` and cleared by `ObpRemoveFromDirectory()`.
      - `mark_dead` paths then unlink directly in O(1) without any path/list walk.
      - Codex quality 2026-04-22 M finding from D00 T03 §9 review.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

> **Notes:**
> - In-memory hierarchical namespace (`ob_ns.c`): `OBJECT_DIRECTORY` (per-dir spinlock + entry list) + `OBJECT_SYMBOLIC_LINK`; insert/lookup take the directory ref under the lock before publishing and return a referenced object.
> - Symlink resolution is now cycle-safe: one shared `OB_SYMLINK_DEPTH` (8) budget threaded (in/out `*depth`) across the directory walk AND leaf recursion, so a symlink cycle fails instead of exhausting the kernel stack.
> - Root + standard dirs built at `ob_ns_init` (`\`, `\Device`, `\KernelObjects`, `\DosDevices`, `\BaseNamedObjects`, `\Sessions\0`); `C:` -> `\Device\HardDisk0\Partition0` symlink.
> - Validation: `test_ob.c` (`TEST_CAT_OB`) incl. new `test_ob_symlink_cycle_bounded`; `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob).
> - Open: O(1) teardown caching (deferred perf item below) still `[ ]`; namespace functionality is complete.
> **Verified:** 2026-06-21 | ship `44db565a` + review fixes | 8/9 items | build OK | tests 327/327 PASS (SUITE=ob)
> **Deferred:** [M] thread/process teardown does two O(n) namespace scans under an IRQ-off lock on every exit (scheduler hot path) -> XREF: 02-kernel-core/TODO-05 §4 (item: "Cache parent-directory + entry linkage in `OBJECT_HEADER` so teardown avoids path relookup" at line 228)
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 2H+1M+2L fixed | scope: kernel-code-quality

Register VFS nodes, tasks, and threads as first-class Ob-managed objects.

- [x] Register `ObpFileType` -- body wraps a `vfs_node *`; `on_close` calls `vfs_close`; `on_delete` frees the VFS node wrapper; `on_parse` defers remaining path to VFS
- [x] Replace all direct `vfs_open` / `vfs_close` call sites in syscall.c with ObRef/ObDeref wrappers routed through `NtCreateFile` / `NtOpenFile` stubs (→ XREF: TODO-12 §8)
- [x] Register `ObpProcessType` -- body is `task_t *`; permanent while process is alive; `on_delete` releases task memory; inserted into `\KernelObjects\Process<PID>` at creation
- [x] Register `ObpThreadType` -- body is `thread_t *` (or task sub-struct); same lifetime semantics
- [x] Audit all existing raw `task_t *` / `vfs_node *` storage in the syscall table and IPC code; replace with handle-table lookups or `ObReferenceObjectByPointer` calls
- [x] Commit: `"kernel: ob -- file, process, thread object types"`

---

## 5. File, Process, Thread object types

Implemented across the owning subsystems (VFS, `task.c`, `thread.c`), each registering its own `OBJECT_TYPE` entry via the §1 framework and `ObCreateObjectType`. This section documents the contract; the actual implementations live in the per-subsystem TODOs (handle-lookup paths, delete callbacks, and access-check integration). See §6 for synchronisation object types and §7 for Section.

- [x] `FileObject` type: owned by VFS; close callback releases `vnode` reference.
- [x] `ProcessObject` type: owned by `task.c`; delete callback tears down address space.
- [x] `ThreadObject` type: owned by `thread.c`; delete callback frees kernel + user stacks.
- [/] **Fix PEB/TEB dir name collision** (`task.c:1974`): the `Process<PID>` dir collides with `ob_process_create`'s process-object name, so PEB/TEB go unreachable + `proc_dir` leaks -- distinct dir name + check/free on insert failure
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Scale process/thread namespace beyond the flat 128-entry `\KernelObjects` cap**: per-type sub-dirs (or hashed dirs) so >128 live objects stay enumerable
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

> **Notes:**
> - `ObpFileType` (`ob_file.c`): wraps a VFS node OR a pipe end (mutually exclusive); `file_on_close` releases the resource, `file_on_delete` is an idempotent safety net (both null the handle so no double-close).
> - `ObpProcessType`/`ObpThreadType` (`ob_process.c`/`ob_thread.c`): wrap `task_t`/`thread`, registered permanent while alive, named under `\KernelObjects`, torn down by `*_mark_dead` (`ObMakeTemporaryObject` + namespace removal).
> - Review fix: `ob_process_create`/`ob_thread_create` now clear `OB_FLAG_PERMANENT` on namespace-insert failure before the creation-ref drop, so a failed insert frees the object instead of leaking an unreachable permanent one.
> - Validation: `test_ob.c` (`TEST_CAT_OB`); `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob).
> - Open: process handles still allocate from raw `task_t*` (Critical, owned by §9 migration) and the flat-namespace/PEB-TEB-collision items above; section is functional but not fully hardened.
> **Verified:** 2026-06-21 | ship `44db565a` + review fixes | 3/5 items | build OK | tests 327/327 PASS (SUITE=ob)
> **Accepted:** [Critical] process handles are allocated from raw `task_t*` not PROCESS_OBJECT bodies (ObpAllocateHandle mis-reads memory before the task as an OBJECT_HEADER on close) -> XREF: 02-kernel-core/TODO-05 §9 (item: "Migrate PID/TID-encoded process/thread handles to OB-allocated handles" at line 403)
> **Deferred:** [H] `task_exec` PEB/TEB `Process<PID>` dir collides with the process-object name (PEB/TEB unreachable + `proc_dir` leak) -> XREF: 02-kernel-core/TODO-05 §5 (item: "Fix PEB/TEB dir name collision" at line 265)
> **Deferred:** [M] flat `\KernelObjects` 128-entry cap drops process/thread objects under churn -> XREF: 02-kernel-core/TODO-05 §5 (item: "Scale process/thread namespace beyond the flat 128-entry" at line 268)
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 2H fixed, 1Critical accepted-XREF, 1H+1M deferred, 1M rejected (NT tagged-handle low bits) | scope: kernel-code-quality

---

## 6. Synchronisation Object Types
Re-register the existing event, mutex, semaphore, and timer primitives as Ob-managed objects so they can be named in `\BaseNamedObjects`, duplicated, and inherited.

- [x] Register `ObpEventType` -- body wraps existing `event_t`; `on_delete` frees event state
- [x] Register `ObpMutexType` -- body wraps existing `mutex_t`; `on_delete` frees mutex state; if thread holding the mutex is deleted, signal `MUTEX_ABANDONED`
- [x] Register `ObpSemaphoreType` -- body wraps existing `semaphore_t`
- [x] Register `ObpTimerType` -- body wraps timer state; `on_close` cancels the timer
- [x] Named variants: inserting into `\BaseNamedObjects\<name>` makes the object findable by name
- [x] `NtCreateEvent`, `NtOpenEvent`, `NtCreateMutex`, `NtOpenMutex`, `NtCreateSemaphore`, `NtCreateTimer` stubs -- resolve name via Ob namespace, allocate or open, return HANDLE (→ XREF: TODO-12 §7)
- [x] Update `SYS_PIPE` to wrap the pipe read/write ends as two File objects in the handle table so pipes are closeable with `NtClose` like any other handle
- [x] Commit: `"kernel: ob -- event, mutex, semaphore, timer object types"`
- [/] **Replace the flat NT timer armed list with an ordered structure (min-heap or timing wheel)**:
      - `src/kernel/nt/nt_timer.c` currently keeps `s_armed_head` as an unsorted singly-linked list; `nt_timer_tick()` walks every entry on every ISR call and NtSetTimer/NtCancelTimer do linear search.
      - This is fine for tens of armed timers but becomes O(N * ticks/sec) work in ISR context at scale.
      - Swap the list for either (a) a min-heap keyed by `due_ns` stored in an array attached to `static struct nt_timer_queue g_tq[MAX_CPUS]` (per-CPU to eliminate the global lock), or (b) a classic timing wheel with O(1) arm/cancel/tick for common cases.
      - Preserve the two-phase ISR signal-outside-lock pattern.
      - Add a stress test in `test_ob.c` that arms 1024 one-shot timers with staggered `DueTime` and asserts wake fan-out under 1ms of slack.
      - This auto-closes TODO-12 §19 Accepted (linear armed-list scan at scale).
      - Also move periodic `event_set` and the Phase-B final deref OUT of `nt_timer_tick` onto a deferred chain so no `event_set`/`kfree` runs under `s_armed_lock` or in the tick path.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Normalize `NtOpen{Event,Mutex,Semaphore}` names** through `\BaseNamedObjects\%s` (like create + `ObOpenTimer`) in `nt_sync.c`, so a named sync object is reopenable by leaf name (today only timer does this)
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Lock the event/mutex/semaphore waiter queues**: `sched/event.c`/`mutex.c`/`semaphore.c` mutate `num_waiters` + waiter arrays unlocked, raced between timer-ISR `event_set` and thread-context wait (torn array / lost wakeup on SMP)
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

> **Notes:**
> - Event/Mutex/Semaphore/Timer registered as OB types wrapping the existing `event_t`/`mutex_t`/`semaphore_t`/timer primitives; named in `\BaseNamedObjects`, handle-managed, `on_delete` frees the state (mutex signals `MUTEX_ABANDONED` on owner death).
> - Review fix (Critical): `nt_timer_tick` now `ObReferenceObject`s a one-shot timer under `s_armed_lock` before the deferred `event_set` and drops it after, closing a use-after-free where a concurrent last-handle close freed the timer mid-signal.
> - Review fix (High): named create (`ob_event`/`mutex`/`semaphore`) handles the `ObInsertObject` duplicate-race by redirecting to the namespace winner + freeing the loser (matches `ObCreateTimerEx`), so one name maps to one object.
> - Validation: `test_ob.c` (`TEST_CAT_OB`); `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob).
> - Open (deferred items above): NtOpen leaf-name normalization, sync-primitive waiter-queue locking, timer-tick latency rework; section is functional but the wait/timer paths are not fully SMP-hardened.
> **Verified:** 2026-06-21 | ship `d91153f5` + review fixes | 7/10 items | build OK | tests 327/327 PASS (SUITE=ob)
> **Accepted:** [H] `wait_on_handle` blocks on an embedded sync primitive without pinning the object, so a concurrent last-handle close can free it under the waiter -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Deferred:** [H] `NtOpen{Event,Mutex,Semaphore}` do not normalize leaf names to `\BaseNamedObjects\%s`, so a named object is not reopenable by leaf name -> XREF: 02-kernel-core/TODO-05 §6 (item: "Normalize `NtOpen{Event,Mutex,Semaphore}` names" at line 307)
> **Deferred:** [H] the event/mutex/semaphore waiter queues are mutated unlocked, raced between timer-ISR `event_set` and thread-context wait -> XREF: 02-kernel-core/TODO-05 §6 (item: "Lock the event/mutex/semaphore waiter queues" at line 310)
> **Deferred:** [M] periodic `event_set` runs under `s_armed_lock` and the Phase-B final deref can free in the tick path -> XREF: 02-kernel-core/TODO-05 §6 (item: "Replace the flat NT timer armed list" at line 297)
> **Quality reviewed:** 2026-06-21 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + auditor | 1Critical+1H fixed, 1H accepted-XREF, 2H+1M deferred | scope: kernel-code-quality

---

## 7. Section (Shared Memory) Object Type
Sections represent mappable memory objects; the foundation for `MapViewOfFile` and shared memory.

- [x] Register `ObpSectionType` -- body: physical base address, size, page count, flags, refcount
- [x] `ObCreateSection(size, protect, name)` -- allocates contiguous physical pages via `pmm_alloc_contiguous`; inserts into Ob namespace if named
- [x] `ObMapViewOfSection(section, process, address, size, offset, protect)` -- maps the physical pages into the target process address space via VMM page-table entries
- [x] `ObUnmapViewOfSection(process, base_address)` -- removes VMM mappings; does not free physical pages until refcount drops to 0
- [x] Re-implement `SYS_SHMEM_CREATE` / `SYS_SHMEM_MAP` as thin wrappers over the Ob section API
- [x] Commit: `"kernel: ob -- section object type and view mapping"`
- [/] **Move section PTE install/teardown outside `so->lk`**: the map loop holds the per-section spinlock across up to 64K `vmm_share_user_page` calls (alloc/split/flush) -- snapshot under lock, do PTE work after unlock, reacquire to commit
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **`ObUnmapViewOfSection` (handle path) must clear user PTEs** like `ObUnmapViewOfSectionByBase`: today it drops the section pin without `vmm_unshare_user_page`, so frames can be freed while still mapped (shared teardown helper)
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Resolve section views without a live handle**: a view outlives its handle (per-view pin), so map -> close-handle -> unmap-by-base leaks the view + frames; track views task-side by base (XREF: §4 view-base index)
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

> **Notes:**
> - `ObpSectionType` (`ob_section.c`): contiguous frames (`pmm_alloc_contiguous`), per-section `so->lk`, `SECTION_MAX_VIEWS` view table; map installs per-task user PTEs (`vmm_share_user_page`) at a bump VA; `section_on_delete` frees frames at last ref.
> - Review fix (Critical): a partial map failure now `vmm_unshare_user_page`s the PTEs already installed before returning, instead of stranding user mappings onto frames `section_on_delete` could free.
> - Review fix (Medium): `section_offset_bytes` rejected if not page-aligned, closing the unaligned-offset wrong-span map.
> - Validation: `test_ob.c` (`TEST_CAT_OB`); `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob).
> - Open (deferred above): map holds `so->lk` across PTE install, handle-path unmap lacks PTE teardown, base-unmap can't find a view after handle close -- focused mapping rework; create/map/unmap-by-base work.
> **Verified:** 2026-06-21 | ship `4bca949c` + review fixes | 5/8 items | build OK | tests 327/327 PASS (SUITE=ob)
> **Deferred:** [H] the map path holds the per-section `so->lk` across up to 64K `vmm_share_user_page` calls (long IRQ-off hold, lock-order risk) -> XREF: 02-kernel-core/TODO-05 §7 (item: "Move section PTE install/teardown outside `so->lk`" at line 338)
> **Deferred:** [H] `ObUnmapViewOfSection` (handle path) drops the section pin without clearing user PTEs, so frames can be freed while still mapped -> XREF: 02-kernel-core/TODO-05 §7 (item: "`ObUnmapViewOfSection` (handle path) must clear user PTEs" at line 341)
> **Deferred:** [M] a view outlives its section handle, so base-unmap (handle-table scan) can't find it -> XREF: 02-kernel-core/TODO-05 §7 (item: "Resolve section views without a live handle" at line 344)
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1Critical+1M fixed, 2H+1M deferred | scope: kernel-code-quality

---

## 8. Security Descriptor Integration
Attach DACL/SACL/Owner/Group to named objects so the Security Reference Monitor can enforce access rights at open time. Depends on TODO-15 (SRM) for full enforcement; this section wires the storage and basic check hook.

- [x] Define `SECURITY_DESCRIPTOR` in `include/kernel/security/acl.h` (full struct, not minimal); matches ob.h forward declaration
- [x] `ObSetSecurityDescriptor(object, sd)` -- attaches an SD; stored in `header->security`
- [x] `ObGetSecurityDescriptor(object, &sd)` -- returns current SD
- [x] In `ObpAllocateHandle`: call `type->on_open(object, access)` if `header->security` is non-NULL -- deny handle creation on access denied (full SeAccessCheck wired in TODO-15 §8)
- [x] Default SD for kernel-created objects: DACL granting `GENERIC_ALL` to SYSTEM SID (via `SeCreateDefaultSD(SE_SD_TYPE_DEFAULT)` in `ob_alloc_object`)
- [x] Default SD for user-created named objects: DACL granting `GENERIC_ALL` to creator SID (via `SeCreateCreatorSD` using `task->token->UserSid`)
- [x] Commit: `"kernel: ob -- security descriptor storage and access check hook"`

> **Notes:**
> - SD storage: `ObSetSecurityDescriptor`/`ObGetSecurityDescriptor` (`ob.c`) attach/return `header->security`; `ob_alloc_object` assigns a default (shared static, self-relative) SD or a per-object tail-packed creator SD (`SeCreateCreatorSD`).
> - `SeCreateCreatorSD` builds a 3-ACE absolute DACL (creator + SYSTEM `GENERIC_ALL`, World `READ_CONTROL`); `SeCreateDefaultSD` returns a cached static blob.
> - Review fix (Medium): `SeCreateCreatorSD` now sizes the DACL from the actual SID lengths, rejects undersized buffers, and checks every ACE-add return -- a 64-byte buffer previously dropped the World ACE silently while reporting success.
> - Validation: `test_ob.c` + `test_security.c` (982 SUITE=security + 327 SUITE=ob pass).
> - Scope: SD STORAGE + the `on_open` hook only; the hook is NULL for all types today, so DACLs are not yet ENFORCED at handle open -- that is `SeAccessCheck` (TODO-15 §5), accepted below.
> **Verified:** 2026-06-21 | ship `8845b6ba` + review fixes | 6/6 items | build OK | tests 982 security + 327 ob PASS
> **Accepted:** [H] the `on_open` access hook is NULL for every object type, so stored SD DACLs are not enforced at handle open (storage-complete, enforcement deferred) -> XREF: 02-kernel-core/TODO-15 §5 (item: "In `ObpReferenceObjectByHandle` ... call `SeAccessCheck(...)`; return `STATUS_ACCESS_DENIED` if check fails" at line 343)
> **Quality reviewed:** 2026-06-21 | Codex 3x (adversarial, consistency, perf) | 1M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 9. NtClose / NtDuplicateObject / NtQueryObject
Core Win32 handle management syscalls routed through the Ob layer.

- [x] `NtClose(HANDLE)` -- look up handle in calling process's table, call `ObpFreeHandle`
- [x] `NtDuplicateObject(src_process, src_handle, dst_process, &dst_handle, access, attrs, options)`:
  - Look up `src_handle` in `src_process` handle table
  - Allocate new entry in `dst_process` handle table pointing to the same object
  - Call `ObReferenceObject` for the new reference
  - If `DUPLICATE_CLOSE_SOURCE` is set, free the source handle
- [x] `NtQueryObject(handle, info_class, buffer, size, &return_length)`:
  - `ObjectNameInformation` -- returns the object's leaf component name (`hdr->name`); full namespace-path reconstruction is deferred (see the full-path item below)
  - `ObjectTypeInformation` -- returns type name, total handles, total references
  - `ObjectBasicInformation` -- returns refcount, handle count, attributes
- [x] Expose `NtClose` via `SYS_CLOSEHANDLE` replacing inline `ObpFreeHandle` in syscall.c
- [x] Commit: `"kernel: ob -- NtClose, NtDuplicateObject, NtQueryObject"`
- [/] **`ObjectNameInformation` full-path reconstruction**: `NtQueryObject` returns only `hdr->name` (leaf), so `\A\Foo`/`\B\Foo` collide. Walk `OBJECT_DIRECTORY.parent` to `\`, prepending each component into the caller buffer; test distinct paths.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.
- [/] **Migrate PID/TID-encoded process/thread handles to OB-allocated handles**:
      - Current `src/kernel/nt/nt_process.c::task_from_handle` and `src/kernel/nt/nt_token.c::resolve_process_handle` cast a raw PID/TID integer to a `HANDLE` and look up the task directly.
      - This bypasses the per-process handle table entirely, so `granted_access`, `OBJ_INHERIT`, `DuplicateHandle`, and `NtClose` all fail to apply to process/thread handles.
      - Change `NtOpenProcess`/`NtOpenThread` (TODO-12 §2) to call `ObpAllocateHandle(&task_current()->handle_table, task, desired_access, attrs)` using `ObpProcessType`/`ObpThreadType` (registered in §6).
      - Then change every handler that resolves process/thread handles (`nt_process.c`, `nt_token.c`, `nt_sync.c` wait handlers that accept thread/process) to use `ObpLookupHandle` + type-check + read `granted_access`.
      - Remove the PID/TID-cast fallback except for explicit pseudo-handles (`CURRENT_PROCESS = -2`, `CURRENT_THREAD = -3`).
      - This auto-closes TODO-12 §16 Accepted #1 (PID/TID-encoded handles) and enables per-handle access enforcement.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

- [/] **Migrate IO completion ports to OB handles**: `nt_file.c` IOCP `idx+0x10000` pseudo-handles are globally guessable (cross-task Set/Remove). Register `ObpIoCompletionType`, route through the per-process handle table. XREF: TODO-12 §13.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

**Test checkpoint:** `NtClose` frees a handle and drops one ref; `NtDuplicateObject` clones a handle into a target table (target stays valid after the source closes), caps the dup to the source's access mask, and closes the source when `DUPLICATE_CLOSE_SOURCE` is set; `NtQueryObject` returns Basic/Name/Type/Types info within the caller's buffer bound. `bash scripts/test.sh SUITE=ob` passes incl. `OB: duplicate handle` + `OB: duplicate access cap`.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 349 kernel suites, 0 failures

> **Notes:**
> - `NtClose` / `NtDuplicateObject` / `NtQueryObject` in `ob.c` route through the per-process handle table; `NtClose` is wired via `SYS_CLOSEHANDLE` (`syscall.c`) + the SSDT 0x0000 wrapper (`nt_syscall.c`).
> - `NtQueryObject` serves ObjectBasic/Name/Type/TypesInformation, each bounds-checked against the caller `size` before any write.
> - Review fix: `NtDuplicateObject` pins the source + caps the dup to an immutable request ceiling (callback-proof), allocates the dest BEFORE closing the source (so the last-handle `on_close` never fires mid-dup), re-validates the source after the pre-callback, and rolls the dest back if the atomic-transfer close is refused.
> - Review fix: the handle-table entry array is now freed with the allocator that produced it (`kfree` vs per-frame `pmm_free_frame`) -- a >256-handle table is PMM-backed and was being `kfree`'d, corrupting the heap.
> - Validation: `test_ob.c` (`TEST_CAT_OB`) incl. 4 new dup tests (access-cap, close-protected, callback-proof ceiling, no-premature-on_close) + handle-alias rejection; 349 OB + 982 security tests pass.
> - Scope: the concurrent-close lookup-then-deref race + non-atomic `handle_count` are the shared no-table-lock gap owned by §3; the two `[/]` items below are deferred.
> **Verified:** 2026-06-21 | core ship `bc41dc9d` + review fixes | 4/6 items | build OK | tests 349 ob + 982 security PASS
> **Accepted:** [Critical] concurrent `NtClose` vs duplicate/query uses an unpinned handle-table entry (no per-table lock) and `handle_count` is a non-atomic `uint32_t` -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Accepted:** [M] `NtDuplicateObject` caps the dup to the source handle's mask (safe pre-SeAccessCheck interim), stricter than Win32 DuplicateHandle which re-authorizes `desired_access` against the SD -> XREF: 02-kernel-core/TODO-15 §5 (item: "In `ObpReferenceObjectByHandle` ... call `SeAccessCheck(...)`; return `STATUS_ACCESS_DENIED` if check fails" at line 343)
> **Deferred:** [M] `ObjectNameInformation` returns the leaf component name, not the full namespace path -> XREF: 02-kernel-core/TODO-05 §9 (item: "`ObjectNameInformation` full-path reconstruction" at line 400)
> **Deferred:** [H] process/thread handles are PID/TID-encoded and bypass the handle table, so granted_access/inherit/dup/close do not apply to them -> XREF: 02-kernel-core/TODO-05 §9 (item: "Migrate PID/TID-encoded process/thread handles to OB-allocated handles" at line 403)
> **Deferred:** [H] the cross-CPU race window between the dup's source re-validation and the close/alloc (and the transient HANDLE_CREATE callback for a dest rolled back in that race) needs the transactional per-handle-table lock -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Quality reviewed:** 2026-06-21 | Codex 11x (adversarial, consistency, perf, re-adversarial x8) + auditor | 1Crit+8H+1M fixed, 1Crit+2H+2M accepted-XREF | scope: kernel-code-quality

---

## 10. Handle Inheritance Across CreateProcess
Win32 `CreateProcess` with `bInheritHandles=TRUE` copies inheritable handles into the child.

- [x] At process creation: if inherit flag is set, call `ob_handle_table_inherit(parent, child)`
- [x] For each entry with `OBJ_INHERIT` attribute: copy to child table at the same slot index with the same access rights
- [x] Call `ObReferenceObject` for each inherited handle -- child holds independent references
- [x] Inherited handle indices in the child match the parent (Win32 contract -- same HANDLE value)
- [x] On child `NtClose`, child's references are released independently of the parent's
- [x] Commit: `"kernel: ob -- handle inheritance across CreateProcess"`
- [/] **Atomic CreateProcess teardown on failure**: `NtCreateProcess_handler` leaks the child task on post-`task_create` failure (inherit/alloc); add a `task_destroy(pid)` for unstarted tasks, then make inheritance all-or-fail with teardown.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

**Test checkpoint:** `bash scripts/test.sh SUITE=ob` -- `OB: handle inherit` asserts an `OBJ_INHERIT` handle copies to the child at the same index with `total_handles` rising 1->2 then back to 0 on close; `OB: inherit none -> 0` asserts a non-inheritable parent yields 0 inherited (not OOM).

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 355 kernel suites, 0 failures

> **Notes:**
> - `ob_handle_table_inherit` (`handle_table.c`) copies every `OBJ_INHERIT` parent handle to the same child slot (HANDLE values match), takes an independent `ObReferenceObject` per handle; child closes are independent of the parent.
> - Runs from `NtCreateProcess_handler` when `bInheritHandles` is set.
> - Review fix: inherited handles now bump `type->total_handles`/`peak_handles` like `ObpAllocateHandle` (was undercounting -- `ObpFreeHandle` decrements them on close, driving the signed counter negative and corrupting `NtQueryObject` stats).
> - Review fix: the child grows only to cover the highest inheritable slot (avoids false-OOM from non-inheritable handles) and inherits best-effort with a `klog` warning if memory pressure drops some.
> - Validation: `test_ob.c` (`TEST_CAT_OB`) `test_ob_handle_inherit` (stats 1->2->0) + `test_ob_inherit_none`; 355 OB tests pass.
> - Scope: atomic all-or-fail process creation needs a child-task teardown helper (deferred above); the unlocked-handle-table race is the shared §3 gap.
> **Verified:** 2026-06-21 | core ship -- review fixes | 5/6 items | build OK | tests 355 ob PASS
> **Deferred:** [H] CreateProcess leaks the created child task on any post-`task_create` failure (inheritance OOM / handle alloc); needs a `task_destroy` helper for atomic teardown -> XREF: 02-kernel-core/TODO-05 §10 (item: "Atomic CreateProcess teardown on failure" at line 447)
> **Accepted:** [M] the parent handle table is read without a lock during inheritance (concurrent mutation race) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 2M fixed, 1H+1M deferred-XREF | scope: kernel-code-quality

---

## 11. Unified Kernel-User Namespace Browser API
Expose the Ob namespace as a queryable tree to user-mode via a dedicated syscall. Neither Windows nor Linux expose this publicly -- Windows `NtQueryDirectoryObject` is internal / undocumented; Linux has no equivalent.

- [x] `NtOpenDirectoryObject(name, access, &handle)` -- opens a directory by path; returns HANDLE
- [x] `NtQueryDirectoryObject(handle, buffer, count, &context, &return_count)`:
  - Enumerates entries in an `OBJECT_DIRECTORY`
  - Each entry: name string + type name string (OBJECT_DIRECTORY_INFORMATION)
- [x] Syscalls: SYS_OPENDIROBJ (42), SYS_QUERYDIROBJ (43) -- user-mode can walk `\` and enumerate all named objects
- [x] Commit: `"kernel: ob -- NtOpenDirectoryObject and NtQueryDirectoryObject (public API)"`
- [/] **`NtQueryDirectoryObject` `ReturnLength` in bytes**: the SSDT handler (`nt_namespace.c`) returns `*ret_len` as entry COUNT; Win32 wants bytes (count * 96). Multiply when writing the caller's ReturnLength.
      - Also PARKED on the kernel image ceiling, which is the blocker with a real re-open path: this adds kernel `.text` and the budget is 95 bytes -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT").
      - Beyond that ceiling there is NO external prerequisite: this section's Deferred stamp points back at this same item, so the work needs a focused implementation pass in this file rather than another TODO to land first. Recorded plainly so a later pass does not go looking for a dependency that does not exist.

**Test checkpoint:** `bash scripts/test.sh SUITE=ob` -- `OB: NtQueryDirectoryObject: enumerate root` opens `\` and enumerates its entries (each row = name + type name, 96-byte `OBJECT_DIRECTORY_INFORMATION`) without fault.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 355 kernel suites, 0 failures

> **Notes:**
> - `NtOpenDirectoryObject` / `NtQueryDirectoryObject` (`ob.c`) expose the Ob namespace tree to user-mode via `SYS_OPENDIROBJ`/`SYS_QUERYDIROBJ` (`syscall.c`) + SSDT handlers (`nt_namespace.c`).
> - Enumeration pins the directory and holds `dir->lock` across the list walk.
> - Review fix (Critical): rows are assembled into a zeroed kernel-local bounce buffer under `dir->lock`, then copied out via `ProbeForWriteIfUser` + `copy_to_user` AFTER unlock -- no `#PF` under the lock, no kernel-memory write, no stack-padding leak.
> - Review fix (High): the legacy `SYS_QUERYDIROBJ` (no byte-length, and the INT 0x80 path never sets UserMode so the in-function `IfUser` probe no-ops) is forced to single-entry and now `ProbeForWrite`s the one 96-byte row explicitly; multi-row reads use the SSDT handler (`buf_len`).
> - Validation: `test_ob.c` (`TEST_CAT_OB`) `test_ob_query_directory`; 355 OB tests pass.
> - Scope: `ReturnLength`-in-bytes is deferred above; the lookup-then-ref race is the shared §3 handle-table-lock gap.
> **Verified:** 2026-06-21 | core ship -- review fixes | 4/5 items | build OK | tests 355 ob PASS
> **Deferred:** [M] SSDT `NtQueryDirectoryObject` returns `ReturnLength` as an entry count, not bytes -> XREF: 02-kernel-core/TODO-05 §11 (item: "`NtQueryDirectoryObject` `ReturnLength` in bytes" at line 478)
> **Accepted:** [H] the directory handle is pinned AFTER an unlocked `ObpLookupHandle` (the lookup-then-ref window races a concurrent `NtClose`) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Accepted:** [H] `ProbeForWrite` is range-check-only (no page touch) and `copy_to_user` has no fault fixup, so a probe-passing-but-unmapped user page #PFs in the copy -- a kernel-wide user-access gap, not §11-specific -> XREF: 02-kernel-core/TODO-23 §13 (item: "`include/kernel/probe.h` -- `ProbeForRead`, `ProbeForWrite`, `try_copy_from_user`, `try_copy_to_user`" at line 52)
> **Quality reviewed:** 2026-06-21 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + auditor | 1Crit+2H fixed, 2H+1M accepted-XREF | scope: kernel-code-quality

---

## 12. Per-Type Object and Handle Statistics
Track per-type creation counts, live object counts, live handle counts, and peak (high-water) values, exposed through `NtQueryObject(ObjectTypeInformation|ObjectTypesInformation)`. This section owns the **counters** (the data source); the kernel returns them in an internal `OBJECT_TYPE_INFORMATION` representation. The Win11 NT-compatible ABI shape for the user-facing syscall (`UNICODE_STRING TypeName`, pool-usage fields, canonical field order) is owned by the SSDT exposure in TODO-12 §30 -- no NT-ABI consumer exists yet, so the internal struct is sufficient until that lands.

> [!WARNING]
> Modifies `ob_alloc_object()`, `ob_free_object()`, `ObpAllocateHandle()`, and `ObpFreeHandle()` -- all core Ob paths used by every kernel subsystem. Test incrementally: add counters to `ob_alloc_object` first, verify boot still works, then proceed to handle-side counters. Rollback: revert counter increments if boot regresses.

- [x] Add atomic counters to `OBJECT_TYPE` in `include/kernel/ob/ob_type.h`:
  - `atomic_t total_objects` -- current live objects of this type
  - `atomic_t total_handles` -- current open handles to objects of this type
  - `uint32_t peak_objects` -- high-water mark for `total_objects`
  - `uint32_t peak_handles` -- high-water mark for `total_handles`
- [x] In `ob_alloc_object()`: increment `type->total_objects`; update `peak_objects` if new value exceeds it
- [x] In `ob_free_object()`: decrement `type->total_objects`
- [x] In `ObpAllocateHandle()`: increment `type->total_handles`; update `peak_handles`
- [x] In `ObpFreeHandle()`: decrement `type->total_handles`
- [x] Update `NtQueryObject(ObjectTypeInformation)` in `ob.c` to return `OBJECT_TYPE_INFORMATION` struct: type name, `total_objects`, `total_handles`, `peak_objects`, `peak_handles`, `body_size`, valid access mask
- [x] Add `NtQueryObject(ObjectTypesInformation)` info class -- enumerates all registered types via `ob_get_types()` into `OBJECT_TYPES_INFORMATION` struct (→ XREF: `TODO-12 §10` `NtQuerySystemInformation`)
- [x] 15 test assertions in `test_ob.c`: alloc increments, free decrements, peak preserved, handle stats, NtQueryObject struct
- [x] Commit: `"kernel: ob -- per-type object and handle statistics"`

**Test checkpoint:** `ob_alloc_object(ObpEventType)` increments `ObpEventType->total_objects`; `ObDereferenceObject` decrements it. After creating 100 events and freeing 50, `total_objects == 50` and `peak_objects == 100`. `NtQueryObject(ObjectTypeInformation)` returns correct counters. Verify on QEMU WHPX + TCG + VirtualBox. Bare metal follow-up (no hardware interaction, low risk). `POST16(0xD900)`-`POST16(0xD903)` (range `0xD9xx` confirmed free -- `0xDBxx` used by `TODO-19-usb-boot-hardening.md`).

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 363 kernel suites, 0 failures

> **Notes:**
> - `OBJECT_TYPE` holds per-type live/peak object+handle counts (`ob_type.h`); bumped in alloc/handle/inherit, dropped in free/close, surfaced by `NtQueryObject(ObjectTypeInformation|ObjectTypesInformation)`.
> - Stat RMWs use relaxed atomics (pure diagnostics, never gate free/publish/lock); peak lift via the shared guarded `ob_stat_lift_peak` CAS-max (`cur<=0` ignored so a transient-negative never poisons a peak to ~UINT32_MAX).
> - Export floors `total_handles` silently (expected handle-table transient) and reports a negative `total_objects` loudly via `klog` (accounting bug); `ObjectTypesInformation` enumerates global types with no object handle.
> - Validation: `test_ob.c` (`TEST_CAT_OB`) `test_ob_type_stats` + `test_ob_stat_export_clamp`; 363 OB tests pass.
> - Scope: counters are SMP-atomic; exactly-once handle-stat accounting needs the §3 per-handle-table lock; Win11 NT-ABI struct shape owned by TODO-12 §30.
> **Verified:** 2026-06-22 | ship `44db565a` + review fixes | 8/8 items | build OK | tests 363 ob PASS
> **Accepted:** [H] handle-stat exactly-once accounting races the unserialized handle-table slot claim/free (counters atomic, slot mutation not) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Accepted:** [H] exported `OBJECT_TYPE_INFORMATION` is an internal `char[32]`+counters struct, not the Win11 NT ABI (no NT-ABI consumer exists yet) -> XREF: 02-kernel-core/TODO-12 §30 (item: "Expose `NtQueryObject` with the Win11 `OBJECT_TYPE_INFORMATION` NT ABI" at line 1089)
> **Quality reviewed:** 2026-06-22 | Codex 6x (adversarial, consistency, perf, re-adversarial) | 1H+4M fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 13. Object Callbacks -- Handle Operation Filtering
Allow kernel-mode drivers to register pre- and post-operation callbacks on handle create and duplicate operations. Win11's `ObRegisterCallbacks` (Vista SP1+) is used by anti-malware, EDR agents, and Protected Process Light enforcement to intercept and filter access to process/thread handles.

> [!WARNING]
> Modifies `ObpAllocateHandle()` and `NtDuplicateObject()` hot paths. An incorrectly registered callback that always denies access will break all handle creation. Test with a no-op callback first, then progressively add filtering logic. Rollback: remove callback list traversal from `ObpAllocateHandle()` to restore pre-§13 behavior.

> [!NOTE]
> Process access mask constants (`PROCESS_TERMINATE`, `PROCESS_QUERY_INFORMATION`, etc.) are not yet defined -- they are deliverables of `TODO-12 §2`. For §13 testing, define local `#define PROCESS_TERMINATE 0x0001` matching the Win32 spec. Replace with the canonical header once TODO-12 §2 is complete.

- [x] Define `OB_PRE_OPERATION_INFORMATION` in `include/kernel/ob/ob_callback.h`:
  - `OB_OPERATION operation` -- `OB_OPERATION_HANDLE_CREATE` or `OB_OPERATION_HANDLE_DUPLICATE`
  - `void *object` -- target object body pointer
  - `const OBJECT_TYPE *object_type` -- type of the target object
  - `uint32_t *desired_access` -- mutable pointer; pre-callback can strip access bits
  - `void *context` -- caller-supplied context from registration
- [x] Define `OB_POST_OPERATION_INFORMATION` -- same fields but `granted_access` is read-only
- [x] Define `OB_OPERATION_REGISTRATION` -- `{object_type, operations_mask, pre_callback, post_callback}`
- [x] Define `OB_CALLBACK_REGISTRATION` -- `{version, altitude, context, operation_count, operations[]}`
- [x] Implement `ObRegisterCallbacks(registration, &handle)`:
  - Validates all fields; inserts into 16-slot array sorted by altitude; irqsave spinlock
  - Returns a **stable monotonic `int32` id** (`g_next_cb_id` from 1) as the handle, decoupled from array position so altitude-sort shifts never invalidate it; refuses cleanly at `INT32_MAX`
- [x] Implement `ObUnRegisterCallbacks(handle)` -- finds node by **id-scan** (not array index; the altitude-sorted array shifts so a slot can later hold a different registration), shifts down, zeroes freed slot
- [x] In `ObpAllocateHandle()`: `ob_invoke_pre_callbacks(HANDLE_CREATE)` before security check; denies if access zeroed
- [x] In `NtDuplicateObject()`: `ob_invoke_pre_callbacks(HANDLE_DUPLICATE)` before `ObpAllocateHandle`
- [x] After successful handle creation/duplication: `ob_invoke_post_callbacks()` with granted access
- [x] **Snapshot-under-lock, invoke-outside-lock** (`ob_dispatch_pre`/`ob_dispatch_post`, `noinline`; relaxed-load `count==0` fast path) so a callback that allocates/logs/re-enters can't deadlock with `s_cb_lock` held + IRQs off
- [x] **Monotonically-decreasing access ceiling** (`*access &= prior` per pre-callback): a callback can only STRIP rights, never mint unrequested ones or re-add a right an earlier filter removed; deny if fully stripped
- [x] Tests: base callbacks (`test_ob_callbacks`); ceiling clamp + stable-id-across-shift + chain-monotonicity (`test_ob_callbacks_hardening`); CREATE-only cb not fired on a duplicate; zero-access request not denied by a no-op cb
- [/] **Drain in-flight invokes before `ObUnRegisterCallbacks` returns** so a freed `context` can't UAF a live invoke; harmless today (static callbacks).
  - Drain mechanism shipped: `EX_RUNDOWN_REF` -> XREF `02-kernel-core/TODO-06-executive-support-runtime.md §3`; needs per-registration stable rundown storage (array shifts) -> XREF `04-drivers-hardware/TODO-05-kernel-module-system.md §4`
- [x] Commit: `"kernel: ob -- ObRegisterCallbacks handle operation filtering"`

> [!NOTE]
> Linux uses LSM (Linux Security Modules) hooks at a different layer. The Ob callback approach matches Win32 driver compatibility requirements and enables anti-tamper protection for critical processes (→ XREF: `TODO-15-security-reference-monitor.md` for PPL integration).

**Test checkpoint:** Register a callback for `ObpProcessType` that strips `PROCESS_TERMINATE` from `desired_access`. Open a handle to a process -- verify `granted_access` lacks `PROCESS_TERMINATE`. Unregister callback -- verify full access is restored. Multiple callbacks at different altitudes invoked in order. Verify on QEMU WHPX + TCG + VirtualBox. `POST16(0xD910)`-`POST16(0xD913)`.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | OB suite 389 kernel PASS, 0 failures

> **Notes:**
> - **What shipped** -- `ObRegisterCallbacks`/`ObUnRegisterCallbacks` + pre/post invoke in `ob_callback.c`, hardened with stable monotonic `int32` ids, snapshot-under-lock/invoke-outside-lock dispatch, and a strip-only monotonic access ceiling.
> - **How it integrates** -- CREATE callbacks fire in `ObpAllocateHandle`; `NtDuplicateObject` fires DUPLICATE callbacks and allocs the dest via `ObpAllocateHandleNoCreateCb`, so a CREATE-only cb never sees a duplicate (operation mask authoritative).
> - **Tests** -- `test_ob_callbacks` + `test_ob_callbacks_hardening` (ceiling / stable-id / chain-monotonicity) + dup-create-only-not-fired + zero-access-noop-allowed.
> - **Scope boundary** -- §13 owns the registry + dispatch; in-flight-invoke drain on unregister is deferred to `04-drivers-hardware/TODO-05 §4`; PPL anti-tamper integration is `TODO-15-security-reference-monitor.md`.

> **Verified:** 2026-06-22 | ship `3e466f86` + review fixes | 12/13 items | build OK | tests 6878 kernel + 16 user PASS
> **Deferred:** [H] in-flight-invoke drain on `ObUnRegisterCallbacks` -- a driver freeing `context` post-unregister could UAF a snapshotted invoke (no dynamic registrants / free path today) -> XREF: 04-drivers-hardware/TODO-05-kernel-module-system.md §4 (item: "Before `pmm_free(base)`, unregister + drain any Ob handle-op callbacks the module registered" at line 139)
> **Quality reviewed:** 2026-06-22 | Codex 6x (design, adversarial, consistency, perf, re-adversarial x2) + auditor | 2H+1M+2L fixed, 1H deferred | scope: kernel-code-quality

---

## 14. Per-Process Handle Quota
Enforce a configurable per-process handle limit to prevent resource exhaustion from buggy or malicious processes. Win11 enforces pool quota charges per handle with a theoretical 16M limit. Linux enforces `RLIMIT_NOFILE` per process. This section replaced the old hard compile-time 4096 grow cap with the configurable per-process quota: the table now grows on demand up to the quota (default 16384, ceiling 1M), bounded by `HANDLE_TABLE_ABSOLUTE_MAX`.

> [!NOTE]
> The `NtSetInformationProcess(ProcessHandleQuota)` SSDT wiring (TODO-12 §7, the `NtSetInformationProcess` 0x0035 owner) needs the privilege-check primitive `SeSinglePrivilegeCheck`/`SeIncreaseQuotaPrivilege` to gate raising the quota above the default -- not yet implemented, so raising-without-privilege would be a security hole. Core quota enforcement in `ObpAllocateHandle()` works independently; the SSDT setter is a user-mode convenience, not a blocker.

- [x] Add `uint32_t handle_limit` to `HANDLE_TABLE` -- default from the `handle.quota_default` tunable (falls back to `HANDLE_TABLE_DEFAULT_LIMIT` 16384); set in `ob_handle_table_init()`
- [x] In `ObpAllocateHandle()`: if `table->count >= table->handle_limit`, return `INVALID_HANDLE_VALUE` (quota exceeded) before slot search; `handle_limit == 0` (`HANDLE_TABLE_LIMIT_UNLIMITED`) means no quota
- [x] `handle_table_grow()` honors the quota, not a fixed cap: grows up to `HANDLE_TABLE_ABSOLUTE_MAX` (the retired `HANDLE_TABLE_MAX_CAP` 4096 made the 16384 default unreachable)
- [x] Add `ob_handle_table_set_limit(table, new_limit)` -- clamps to `HANDLE_TABLE_ABSOLUTE_MAX` (1 << 20 = 1M handles)
- [/] Wire into `NtSetInformationProcess(ProcessHandleQuota)` -- needs the `SeSinglePrivilegeCheck`/`SeIncreaseQuotaPrivilege` gate for raise-above-default (security) -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §7`
- [x] Track cumulative handle allocations per process in `task_t.total_handles_created` (`uint64`) -- incremented in `ObpAllocateHandle()`
- [x] On quota exhaustion: `klog(LOG_WARN, ...)` once per episode via the `HANDLE_TABLE.quota_warned` one-shot guard (re-armed in `ObpFreeHandle` when count drops below limit) -- no per-denial log flood
- [x] Tests: set-limit + 3-allocs + 4th-denied + free+retry + ABSOLUTE_MAX clamp; one-shot warn set/clear; grow-past-old-4096-cap; zero-limit unlimited
- [x] Commit: `"kernel: ob -- per-process handle quota enforcement"`

**Test checkpoint:** Set handle limit to 100 for a test process. Allocate 100 handles successfully. 101st allocation returns `INVALID_HANDLE_VALUE`. Free 1 handle, allocate again succeeds. `klog` warning emitted once on exhaustion. Default limit (16384) works for normal boot and a process can hold >4096 handles. Verify on QEMU WHPX + TCG + VirtualBox. `POST16(0xD920)`-`POST16(0xD923)`.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | OB suite 399 kernel PASS, 0 failures

> **Notes:**
> - **What shipped** -- per-process handle quota in `handle_table.c`: tunable-default `handle_limit` (16384, 1M ceiling), enforcement in `ObpAllocateHandle`, `ob_handle_table_set_limit`; the table now grows up to the quota (retired the fixed 4096 cap).
> - **How it integrates** -- the quota check runs before the slot scan; exhaustion logs `LOG_WARN` once per episode via the `quota_warned` one-shot (re-armed on free-below-limit); `total_handles_created` (uint64) is bumped per alloc.
> - **Tests** -- `test_ob_handle_quota` (limit/deny/free/clamp + one-shot warn) + `test_ob_handle_quota_grows_past_old_cap` (>4096 handles + zero-limit unlimited).
> - **Scope boundary** -- §14 owns the per-table quota mechanism; the user-mode `NtSetInformationProcess(ProcessHandleQuota)` setter is `TODO-12 §7` (needs privilege-check infra); the unlocked `count` SMP race is owned by §3's per-handle-table lock work.

> **Verified:** 2026-06-22 | ship `e0361301` + review fixes | 7/8 items | build OK | tests 6892 kernel + 16 user PASS
> **Accepted:** [H] the quota check + `count++`/`count--` are unsynchronized, so same-process concurrent allocs can overshoot the limit (bounded, self-correcting) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Accepted:** [L] `total_handles_created` is charged to `task_current()`, so cross-process `NtDuplicateObject` mis-attributes the diagnostic counter -> XREF: 02-kernel-core/TODO-05 §3 (item: "Give `HANDLE_TABLE` an owning-task back-pointer" at line 184)
> **Deferred:** [M] `NtSetInformationProcess(ProcessHandleQuota)` user-mode setter not wired (needs `SeSinglePrivilegeCheck` for the privileged raise-above-default) -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §7 (item: "`NtSetInformationProcess(0x0035)` ProcessHandleQuota" at line 408)
> **Quality reviewed:** 2026-06-22 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + auditor | 2H+4M+1L fixed, 1H+1L accepted-XREF, 1M deferred | scope: kernel-code-quality

---

## 15. Handle Tracing and Leak Detection
Provide tagged reference tracking and optional per-handle event recording for diagnosing object leaks and under-references. Win11 has `ObReferenceObjectWithTag` / `ObDereferenceObjectWithTag` (Win7+) and ETW handle tracing, but ETW is complex and buffer-limited. Linux has no equivalent. Impossible OS integrates tracing directly with klog for a better developer experience.

> [!TIP]
> Neither Windows nor Linux provides built-in, always-available handle leak detection at the kernel level without external tooling (ETW requires WPR setup; Linux requires strace/lsof). Impossible OS's klog-integrated tracing means `debug=1` in `boot.conf` automatically captures handle leaks -- zero setup required.

- [x] Implement `ObReferenceObjectWithTag(body, tag)` -- calls `ObReferenceObject` and records `{tag, +1, caller_rip}` in trace log via `__builtin_return_address(0)`
- [x] Implement `ObDereferenceObjectWithTag(body, tag)` -- records `{tag, -1, caller_rip}` then `ObDereferenceObject`; leak dump fires from `ob_free_object` at true 0-refcount (race-free), not off a speculative refcount==1 read (TOCTOU)
- [x] Define `OB_REF_TRACE_ENTRY` -- `{uint32_t tag, int8_t delta, uintptr_t caller, uint64_t timestamp}` in `ob_trace.h`
- [x] `OB_TRACE_INFO` (`ob_trace_alloc()`, on `OBJECT_HEADER.trace`, IRQ-safe `spinlock_t`): 64-entry recent-caller ring + net `total_refs`/`total_derefs` + per-tag ledger `life_tags[32]`+`life_overflow` (attribution survives ring wrap)
- [x] `ob_enable_type_tracing(type)` / `ob_disable_type_tracing(type)` -- sets `OBJECT_TYPE.tracing_enabled` flag; `ob_alloc_object()` checks it and allocates trace info
- [x] `ob_dump_trace(body)` (+ `ob_dump_trace_hdr`) -- snapshots under lock, klogs outside (Gate 2): net LIFETIME verdict + ring-truncation note + ledger-overflow warning + per-tag `IMBALANCE` lines that catch a net-balanced mis-tag leak
- [x] Auto-dump on teardown: `ob_free_object` calls `ob_dump_trace_hdr` before freeing the ring -- 0 refcount, sole owner, emitted exactly once
- [x] Handle event tracing: CREATE/FREE/INHERIT klog from all three handle alloc/inherit/close paths when `g_ob_handle_trace` set (INHERIT closes the free-only gap); subject to klog's per-subsystem rate limit + dropped-count summary
- [/] No-drop handle-trace path: opt-in loss-accounted ring (or rate-limit bypass) so handle-heavy leak reconstruction keeps every event under churn instead of relying on klog's per-subsystem 1s budget
- [x] `g_ob_handle_trace` flag: set from `boot.conf` `ob_handle_trace=1`; wired in Phase 2 after OB init
- [x] Tests: `test_ob_trace` (tagged ref/deref + imbalance dump), `test_ob_trace_wrap_and_tags` (ring wrap past 64 + lifetime net survives wrap), `test_ob_trace_mistag_lifetime` (net-balanced mis-tag leak caught by per-tag ledger after wrap)
- [x] Commit: `"kernel: ob -- tagged reference tracing and handle leak detection"`

**Test checkpoint:** Enable tracing for `ObpEventType`. Create event, ref with tag `"Lk01"`, ref with tag `"Lk02"`, deref with tag `"Lk01"`, deref (untagged). On final deref (refcount 0), `ob_dump_trace` reports tag `"Lk02"` has 1 ref / 0 deref = over-reference by 1. `ob_handle_trace=1`: every `ObpAllocateHandle` / `ObpFreeHandle` emits a klog entry with PID, handle value, object pointer, and type name. Verify on QEMU WHPX + TCG. `POST16(0xD930)`-`POST16(0xD933)`.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | OB suite PASS, 0 failures (6913 kernel + 16 user TCG)

> **Notes:**
> - **What shipped** -- `ob_trace.c`/`.h`: tagged ref/deref API, per-object `OB_TRACE_INFO` (64-entry ring + net lifetime counters + 32-slot per-tag ledger), `ob_dump_trace`, plus `g_ob_handle_trace` handle CREATE/FREE event tracing from `boot.conf`.
> - **How it integrates** -- opt-in (`ob_enable_type_tracing`, off by default); `ob_alloc_object` attaches the log, `ob_free_object` dumps once at true 0-refcount; ring SMP-serialized by an IRQ-safe spinlock, klog emitted outside the lock.
> - **Leak detection is wrap-proof** -- ring-independent net counters give the authoritative "any leak?" verdict; the per-tag ledger pins a net-balanced mis-tag leak after the ring wraps; `life_overflow` flags partial attribution past 32 distinct tags.
> - **Canonical doc** -- TODO-05 §15; klog-integrated, no external tooling unlike Win11 ETW / Linux strace.
> - **Scope boundary** -- §15 owns object-ref + handle-event tracing; the handle-table `count` SMP race + owning-task back-pointer stay §3's; `NtSetInformationProcess(ProcessHandleQuota)` stays TODO-12 §7.

> **Verified:** 2026-06-25 | commit `bc16df2d` + review fixes | 10/11 items | build OK | tests 6913 kernel + 16 user PASS, smoke PASS (TCG 2.43s)
> **Accepted:** [M] `ob_handle_table_inherit` reads the parent slot without a table lock, so the broader close-vs-inherit race can still inherit a stale/NULL object (the INHERIT trace itself now snapshots `obj` so it cannot diverge) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 157)
> **Deferred:** [M] handle-event tracing rides normal klog, so the per-subsystem rate limit can drop individual CREATE/FREE/INHERIT lines under churn (klog dropped-count summary accounts the loss) -> XREF: 02-kernel-core/TODO-05 §15 (item: "No-drop handle-trace path: opt-in loss-accounted ring" at line 637)
> **Quality reviewed:** 2026-06-25 | Codex 8x (adversarial, consistency, perf, re-adversarial) + kernel-quality-auditor | 6M+1L fixed, 1M accepted-XREF, 1M deferred | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature               | 🪟 Win11               | 🐧 Linux            | 🚀 Impossible OS              |
| --- | --------------------- | ---------------------- | ------------------- | ----------------------------- |
| 💎  | Typed object header   | ✅ OBJECT_HEADER       | ✅ kobject + kref   | ✅ §1                         |
| 💎  | Type descriptors      | ✅ OBJECT_TYPE hooks   | ✅ kobj_type        | ✅ §1                         |
| 💎  | Auto-delete on 0 ref  | ✅ ObDereferenceObject | ✅ kref_put         | ✅ §2                         |
| 💎  | Per-process handles   | ✅ HANDLE_TABLE        | ✅ fd table         | ✅ §3                         |
| 💎  | Named namespace       | ✅ \BaseNamedObjects   | ✅ /proc, /sys      | ✅ §4                         |
| 💎  | File objects          | ✅ FILE_OBJECT         | ✅ struct file      | ✅ §5                         |
| 💎  | Process/thread objs   | ✅ EPROCESS/ETHREAD    | ✅ task_struct      | ✅ §5                         |
| 💎  | Named sync objects    | ✅ Named events/mutex  | ✅ POSIX named sem  | ✅ §6                         |
| 💎  | Section objects       | ✅ SECTION_OBJECT      | ✅ anonymous mmap   | ✅ §7                         |
| 💎  | Security descriptors  | ✅ DACL/SACL           | ✅ inode perms/ACLs | ✅ §8                         |
| 💎  | Duplicate/inherit     | ✅ Full semantics      | ✅ dup/O_CLOEXEC    | ✅ §9, §10                    |
| ⭐  | Public namespace API  | ❌ Internal only       | ❌ No equivalent    | ✅ §11 -- public, documented  |
| ⭐  | Unified type system   | ⚠️ Partial ObXxx       | ❌ Split fd/kobject | ✅ §1-§7 -- one header        |
| 💎  | Per-type statistics   | ✅ OBJECT_TYPE_INFO    | ✅ /proc/slabinfo   | ✅ §12 -- atomic counters     |
| 💎  | Handle op callbacks   | ✅ ObRegisterCallbacks | ⚠️ LSM hooks        | ✅ §13 -- pre/post filtering  |
| 💎  | Handle quota          | ✅ 16M + pool quota    | ✅ RLIMIT_NOFILE    | ✅ §14 -- 16K default, 1M max |
| ⭐  | Handle leak detection | ⚠️ ETW (complex)       | ❌ No built-in      | ✅ §15 -- klog-integrated     |

> All parity items (§1-§15) complete -- Impossible OS matches Windows NT object management, including per-type statistics (§12), handle-operation callbacks (§13), and handle quota (§14) for full NT driver compatibility.
> Three exclusive features (⭐): public namespace browser API (§11), unified single-header type system (§1-§7), and built-in klog-integrated handle leak detection (§15).

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_ob()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] `src/kernel/test/test_ob.c` created with 7 core tests (§1-§4, §9-§11):
  - `ob_alloc_object` returns non-NULL; `OB_HEADER_FROM_BODY` type matches
  - `ObReferenceObject` increments refcount; `ObDereferenceObject` calls `on_delete` at 0
  - `ObpAllocateHandle` returns valid HANDLE; `ObpLookupHandle` returns correct entry; `ObpFreeHandle` releases
  - `ObLookupObjectByName(\BaseNamedObjects)` finds the directory
  - `NtDuplicateObject` produces independent handle; closing source doesn't affect duplicate
  - Handle with `OBJ_INHERIT` is copied to child at same index
  - `NtQueryDirectoryObject(\)` enumerates `Device`, `KernelObjects`, `BaseNamedObjects`
- [x] Registered in `test_runner_init()`: `test_register_ob()`
- [x] Commit: `"test: add object manager test suite"`
- [ ] §12 tests: `ObpEventType->total_objects` tracks create/delete; `peak_objects` high-water; `NtQueryObject(ObjectTypeInformation)` returns counters; `NtQueryObject(ObjectTypesInformation)` enumerates all types
- [x] §13 tests: pre-callback strips TERMINATE, post-callback invoked, unregister restores access; hardening (ceiling/stable-id/chain-monotonicity), dup-create-only-not-fired, zero-access-noop-allowed (`test_ob.c`)
- [x] §14 tests: limit/allocs/deny/free-retry/ABSOLUTE_MAX clamp + one-shot exhaustion warn; grow-past-old-4096-cap + zero-limit unlimited (`test_ob_handle_quota`, `test_ob_handle_quota_grows_past_old_cap`)
- [x] §15 tests (`test_ob.c`): `test_ob_trace` (tracing + tagged ref/deref + imbalance dump), `test_ob_trace_wrap_and_tags` (ring wrap + lifetime net survives wrap), `test_ob_trace_mistag_lifetime` (mis-tag leak caught by per-tag ledger after wrap)

---

## Verification

- [x] `bash scripts/build.sh clean` -> `=== BUILD OK ===` (verified every commit)
- [x] QEMU WHPX: `ob: Registered 13 built-in types` + namespace created, 2 CPUs, desktop boots (verified 2026-04-01)
- [x] QEMU TCG: same -- verified via NVMe test (NVMe + OB + SMP all working)
- [x] `ObReferenceObject` + `ObDereferenceObject` drives refcount to 0, calls `on_delete` -- verified by `test_ob.c` (7 tests PASS)
- [x] Handle table: alloc/free with `ObpAllocateHandle`/`ObpFreeHandle` -- verified by unit test
- [x] `ObLookupObjectByName(\BaseNamedObjects)` finds directory -- verified by unit test
- [x] `NtDuplicateObject` produces independent handle; closing source doesn't affect duplicate -- verified by unit test
- [x] Child process inherits `OBJ_INHERIT` handles at correct indices -- verified by unit test
- [x] `NtQueryDirectoryObject(\)` enumerates `Device`, `KernelObjects`, `BaseNamedObjects` -- verified by unit test (>= 3 entries)
- [ ] VirtualBox: boot completes with Ob init, no regression
- [ ] Bare metal: boot completes with Ob init, handles work end-to-end
- [ ] §12-§15: per-type stats, callbacks, quota, tracing verified after implementation
- [x] All §1-§11 committed individually (12 commits across OB §5-§11, security §1-§4, OB §8)

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | OB suite 424 kernel PASS, 0 failures (TCG 2026-06-25)
