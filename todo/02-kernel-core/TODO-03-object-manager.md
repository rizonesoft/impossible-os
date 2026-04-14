# TODO-03 -- Object Manager

> **Goal:** Implement the kernel Object Manager (ObXxx layer) -- the unified substrate that gives every kernel resource (files, processes, threads, events, mutexes, semaphores, registry keys, sections) a typed header, reference-counted lifetime, named namespace entry, security descriptor, and per-process handle-table slot. Without this, Win32 `HANDLE` semantics are impossible and resource leaks are unavoidable. This is the single most foundational Win32 prerequisite in the kernel.

> [!IMPORTANT]
> **Current state:** Core Object Manager is complete (§1–§11 done). OBJECT_HEADER, OBJECT_TYPE, reference counting, handle tables, namespace, all major object types, security descriptors, NtClose/NtDuplicateObject/NtQueryObject, handle inheritance, and namespace browser API are all implemented and verified on QEMU WHPX + TCG. Remaining work: §12 (per-type statistics), §13 (object callbacks), §14 (handle quota), §15 (handle tracing).

## Inputs

- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h)
- [`include/kernel/fs/vfs.h`](../../include/kernel/fs/vfs.h)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- process/thread struct
- [`src/kernel/sched/event.c`](../../src/kernel/sched/event.c)
- [`src/kernel/sched/mutex.c`](../../src/kernel/sched/mutex.c)
- [`src/kernel/sched/semaphore.c`](../../src/kernel/sched/semaphore.c)
- [`src/kernel/ipc/`](../../src/kernel/ipc/) -- pipes, shared memory
- → XREF: `TODO-01-kernel-init-sequencing.md §4` -- ObInit (`object_manager_init()`) is a Phase 2 gate, before registry
- → XREF: `TODO-13-registry-completion.md §4` -- register HKEY handles in per-process handle table (DuplicateHandle parity); registry engine in `registry.c` until TODO-13 wires Ob
- → XREF: `TODO-04-peb-teb-user-abi.md` -- TODO-04 §10 exposes PEB/TEB in the Ob namespace (depends on §4 Object Namespace and §11 Namespace Browser)
- → XREF: `TODO-05-native-api-ssdt.md` -- NtCreateFile / NtOpenFile / NtClose and sync Nt syscalls are all Ob-routed; SSDT entries depend on §3, §5, §6
- → XREF: `TODO-08-binary-system.md §1` -- `exec_load()` uses §5 process object registration/lifetime semantics when wiring executable process objects into the Ob namespace
- → XREF: `TODO-11-security-reference-monitor.md` -- SRM enforces DACLs registered here; §13 callbacks enable PPL-style protection
- → XREF: `TODO-05-native-api-ssdt.md §10` -- `NtQuerySystemInformation(SystemHandleInformation)` consumes §12 per-type statistics
- → XREF: `TODO-02-system-logging.md` -- §15 handle tracing integrates with klog subsystem

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

| ⭐  | Order | Deliverable                                                   | Depends On     | Status |
| --- | :---: | ------------------------------------------------------------- | -------------- | :----: |
| 💎  |   1   | OBJECT_HEADER and OBJECT_TYPE infrastructure                  | --             |  [x]   |
| 💎  |   2   | Reference counting and object lifetime                        | §1             |  [x]   |
| 💎  |   3   | Per-process handle table                                      | §2             |  [x]   |
| 💎  |   4   | Object namespace (directory + symbolic link)                  | §2             |  [x]   |
| 💎  |   5   | File, Process, Thread object types                            | §3, §4         |  [x]   |
| 💎  |   6   | Synchronisation object types (Event, Mutex, Semaphore, Timer) | §3, §4         |  [x]   |
| 💎  |   7   | Section (shared memory) object type                           | §3, §4         |  [x]   |
| 💎  |   8   | Security descriptor integration                               | §1, T11 §1,§3  |  [x]   |
| 💎  |   9   | NtClose / NtDuplicateObject / NtQueryObject                   | §3, T11 §4     |  [x]   |
| 💎  |  10   | Handle inheritance across CreateProcess                       | §3, §5         |  [x]   |
| ⭐  |  11   | Unified kernel–user namespace browser API                     | §4             |  [x]   |
| 💎  |  12   | Per-type object and handle statistics                         | §1, §2, §3     |  [x]   |
| 💎  |  13   | Object callbacks -- handle operation filtering                | §3, §9         |  [x]   |
| 💎  |  14   | Per-process handle quota                                      | §3             |  [x]   |
| ⭐  |  15   | Handle tracing and leak detection                             | §2, §12        |  [x]   |

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

## 2. Reference Counting and Object Lifetime
- [x] Implement `ObReferenceObject(void *body)` -- atomic increment of `header->ref_count`
- [x] Implement `ObDereferenceObject(void *body)` -- atomic decrement; when count reaches 0, call `type->on_delete(body)` and free the combined allocation
- [x] Implement `ObReferenceObjectByPointer(void *body, uint32_t access)` -- validates type before ref
- [x] Permanent objects (`OB_FLAG_PERMANENT`) are never deleted when refcount hits 0; must be explicitly made temporary with `ObMakeTemporaryObject()` first
- [x] All existing code that stores raw `vfs_node *` pointers must be audited and wrapped in ObRef/ObDeref pairs -- track this as a follow-up checklist in §5
- [x] Commit: `"kernel: ob -- reference counting and object lifetime"`

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
- [x] Handle table is freed at process exit: iterate all slots and call `ObpFreeHandle` for each
- [x] Special kernel pseudo-handles: `INVALID_HANDLE_VALUE (-1)`, `CURRENT_PROCESS (-2)`, `CURRENT_THREAD (-3)` -- resolve in `ObpLookupHandle` without a table entry
- [x] Commit: `"kernel: ob -- per-process handle table"`
- [ ] **Wire HandleAttributes through `ObpAllocateHandle` for `NtXxxEx` syscalls**: the `attrs` parameter is already plumbed through `ObpAllocateHandle` and stored in `HANDLE_TABLE_ENTRY.attributes`, but the `NtXxxEx` syscalls (`NtOpenProcessTokenEx`, `NtOpenThreadTokenEx`, `NtOpenKeyEx`, `NtCreateFileEx`, etc.) currently discard their `HandleAttributes` argument (see `src/kernel/nt/nt_token.c::NtOpenProcessTokenEx_handler` which explicitly `(void)a3`). Update every `NtXxxEx` handler to pass validated `HandleAttributes` into `ObpAllocateHandle` (via library function or directly). Validate against `OBJ_VALID_ATTRIBUTES = 0x000007F2`; reject with `STATUS_INVALID_PARAMETER` for unsupported bits. This auto-closes TODO-05 §16 Accepted #3 (NtOpenProcessTokenEx HandleAttributes discarded).
- [ ] **Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive** that atomically (under a per-handle-table lock or via RCU-style load with refcount barrier): (1) validates index bounds and `entry->object != NULL`; (2) checks `entry->object`'s type matches `required_type` if non-NULL; (3) calls `ObReferenceObject(entry->object)` BEFORE releasing any concurrency guarantee so the body cannot be freed by a concurrent `ObpFreeHandle`; (4) stores `entry->granted_access` for the caller to enforce required access. Callers release with `ObDereferenceObject(body)` when done. Retrofit every `src/kernel/nt/nt_*.c` handler that currently does `entry = ObpLookupHandle(...); so = (T *)entry->object;` -- including the whole of `nt_section.c` (NtMapViewOfSection, NtUnmapViewOfSection, NtExtendSection, NtQuerySection, NtAreMappedFilesTheSame via `section_copy_backing_path`), `nt_timer.c` (`resolve_timer_handle` used by NtSetTimer/NtCancelTimer/NtQueryTimer/NtSetTimerEx), `nt_registry.c`, `nt_namespace.c`, `nt_token.c`, `nt_process.c`, and `nt_alpc.c` via `src/kernel/ipc/alpc_port.c`'s `AlpcAcceptConnectPort` + `AlpcDisconnectPort` (both currently use `ObpLookupHandle` + explicit `ObReferenceObject` which has a close-race window). Also retrofit `ObUnmapViewOfSectionByBase` in `src/kernel/ob/ob_section.c:414` which walks the handle table without any lock. This auto-closes TODO-05 §18 Accepted (initial-handle-lookup UAF race), TODO-05 §19 Accepted (timer handle UAF race), TODO-12 §3 Accepted (AlpcAcceptConnectPort/Disconnect handle-lookup race), and is a prerequisite for the `ObpReferenceObjectByHandle` SeAccessCheck integration already referenced in TODO-11 §5.
- [ ] **Add per-task view-base index** to eliminate full handle-table walks on unmap and mapped-file-compare. `ObUnmapViewOfSectionByBase` (`src/kernel/ob/ob_section.c:414`) and `ObAreMappedFilesTheSame` (`ob_section.c:604,652`) both iterate every handle-table entry and then every SECTION_MAX_VIEWS slot per section -- O(handle_capacity × SECTION_MAX_VIEWS) per call. Add a small hash or sorted array keyed by `(task_pid, base_addr)` -> `{SECTION_OBJECT *, view_index}` attached to `struct task` (or installed at task creation beside `handle_table`). Update on `ObMapViewOfSectionFull` (insert) and `ObUnmapViewOfSection`/`ObUnmapViewOfSectionByBase` (remove). Both callers become near-O(1). This auto-closes TODO-05 §18 Accepted (handle-table scan scalability).

## 4. Object Namespace
A hierarchical in-memory namespace rooted at `\`. Directories hold named object entries. Symbolic links redirect name lookups.

- [x] Define `OBJECT_DIRECTORY_ENTRY` -- linked list node: `name[64]`, `void *object`
- [x] Implement `OBJECT_DIRECTORY` body type -- list of entries, lock, parent pointer
- [x] Implement `ObpLookupDirectory(path, &remaining)` -- walks `\Foo\Bar` splitting on `\`; follows symlinks; stops at the deepest found directory
- [x] Implement `ObInsertObject(object, name, directory)` -- adds a named entry; fails if name exists and object is not a permanent replacement
- [x] Implement `ObLookupObjectByName(path, type, access, &result)` -- full parse walk calling `type->on_parse` at each node; calls `ObReferenceObject` on success
- [x] Implement `OBJECT_SYMBOLIC_LINK` body type -- target string; `on_parse` redirects the walk
- [x] Create the root namespace at ObInit: `\`, `\Device`, `\KernelObjects`, `\DosDevices`, `\BaseNamedObjects`, `\Sessions\0\BaseNamedObjects` (alias for user-mode named objects)
- [x] `C:` → `\Device\HardDisk0\Partition0` via symlink in `\DosDevices`
- [x] Commit: `"kernel: ob -- object namespace directory and symlinks"`

## 5. File, Process, and Thread Object Types
Register VFS nodes, tasks, and threads as first-class Ob-managed objects.

- [x] Register `ObpFileType` -- body wraps a `vfs_node *`; `on_close` calls `vfs_close`; `on_delete` frees the VFS node wrapper; `on_parse` defers remaining path to VFS
- [x] Replace all direct `vfs_open` / `vfs_close` call sites in syscall.c with ObRef/ObDeref wrappers routed through `NtCreateFile` / `NtOpenFile` stubs (→ XREF: TODO-05 §6)
- [x] Register `ObpProcessType` -- body is `task_t *`; permanent while process is alive; `on_delete` releases task memory; inserted into `\KernelObjects\Process<PID>` at creation
- [x] Register `ObpThreadType` -- body is `thread_t *` (or task sub-struct); same lifetime semantics
- [x] Audit all existing raw `task_t *` / `vfs_node *` storage in the syscall table and IPC code; replace with handle-table lookups or `ObReferenceObjectByPointer` calls
- [x] Commit: `"kernel: ob -- file, process, thread object types"`

## 6. Synchronisation Object Types
Re-register the existing event, mutex, semaphore, and timer primitives as Ob-managed objects so they can be named in `\BaseNamedObjects`, duplicated, and inherited.

- [x] Register `ObpEventType` -- body wraps existing `event_t`; `on_delete` frees event state
- [x] Register `ObpMutexType` -- body wraps existing `mutex_t`; `on_delete` frees mutex state; if thread holding the mutex is deleted, signal `MUTEX_ABANDONED`
- [x] Register `ObpSemaphoreType` -- body wraps existing `semaphore_t`
- [x] Register `ObpTimerType` -- body wraps timer state; `on_close` cancels the timer
- [x] Named variants: inserting into `\BaseNamedObjects\<name>` makes the object findable by name
- [x] `NtCreateEvent`, `NtOpenEvent`, `NtCreateMutex`, `NtOpenMutex`, `NtCreateSemaphore`, `NtCreateTimer` stubs -- resolve name via Ob namespace, allocate or open, return HANDLE (→ XREF: TODO-05 §8)
- [x] Update `SYS_PIPE` to wrap the pipe read/write ends as two File objects in the handle table so pipes are closeable with `NtClose` like any other handle
- [x] Commit: `"kernel: ob -- event, mutex, semaphore, timer object types"`
- [ ] **Replace the flat NT timer armed list with an ordered structure (min-heap or timing wheel)**: `src/kernel/nt/nt_timer.c` currently keeps `s_armed_head` as an unsorted singly-linked list; `nt_timer_tick()` walks every entry on every ISR call and NtSetTimer/NtCancelTimer do linear search. This is fine for tens of armed timers but becomes O(N * ticks/sec) work in ISR context at scale. Swap the list for either (a) a min-heap keyed by `due_ns` stored in an array attached to `static struct nt_timer_queue g_tq[MAX_CPUS]` (per-CPU to eliminate the global lock), or (b) a classic timing wheel with O(1) arm/cancel/tick for common cases. Preserve the two-phase ISR signal-outside-lock pattern. Add a stress test in `test_ob.c` that arms 1024 one-shot timers with staggered `DueTime` and asserts wake fan-out under 1ms of slack. This auto-closes TODO-05 §19 Accepted (linear armed-list scan at scale).

## 7. Section (Shared Memory) Object Type
Sections represent mappable memory objects; the foundation for `MapViewOfFile` and shared memory.

- [x] Register `ObpSectionType` -- body: physical base address, size, page count, flags, refcount
- [x] `ObCreateSection(size, protect, name)` -- allocates contiguous physical pages via `pmm_alloc_contiguous`; inserts into Ob namespace if named
- [x] `ObMapViewOfSection(section, process, address, size, offset, protect)` -- maps the physical pages into the target process address space via VMM page-table entries
- [x] `ObUnmapViewOfSection(process, base_address)` -- removes VMM mappings; does not free physical pages until refcount drops to 0
- [x] Re-implement `SYS_SHMEM_CREATE` / `SYS_SHMEM_MAP` as thin wrappers over the Ob section API
- [x] Commit: `"kernel: ob -- section object type and view mapping"`

## 8. Security Descriptor Integration
Attach DACL/SACL/Owner/Group to named objects so the Security Reference Monitor can enforce access rights at open time. Depends on TODO-11 (SRM) for full enforcement; this section wires the storage and basic check hook.

- [x] Define `SECURITY_DESCRIPTOR` in `include/kernel/security/acl.h` (full struct, not minimal); matches ob.h forward declaration
- [x] `ObSetSecurityDescriptor(object, sd)` -- attaches an SD; stored in `header->security`
- [x] `ObGetSecurityDescriptor(object, &sd)` -- returns current SD
- [x] In `ObpAllocateHandle`: call `type->on_open(object, access)` if `header->security` is non-NULL -- deny handle creation on access denied (full SeAccessCheck wired in TODO-11 §5)
- [x] Default SD for kernel-created objects: DACL granting `GENERIC_ALL` to SYSTEM SID (via `SeCreateDefaultSD(SE_SD_TYPE_DEFAULT)` in `ob_alloc_object`)
- [x] Default SD for user-created named objects: DACL granting `GENERIC_ALL` to creator SID (via `SeCreateCreatorSD` using `task->token->UserSid`)
- [x] Commit: `"kernel: ob -- security descriptor storage and access check hook"`

## 9. NtClose / NtDuplicateObject / NtQueryObject
Core Win32 handle management syscalls routed through the Ob layer.

- [x] `NtClose(HANDLE)` -- look up handle in calling process's table, call `ObpFreeHandle`
- [x] `NtDuplicateObject(src_process, src_handle, dst_process, &dst_handle, access, attrs, options)`:
  - Look up `src_handle` in `src_process` handle table
  - Allocate new entry in `dst_process` handle table pointing to the same object
  - Call `ObReferenceObject` for the new reference
  - If `DUPLICATE_CLOSE_SOURCE` is set, free the source handle
- [x] `NtQueryObject(handle, info_class, buffer, size, &return_length)`:
  - `ObjectNameInformation` -- returns the Ob namespace path of the object
  - `ObjectTypeInformation` -- returns type name, total handles, total references
  - `ObjectBasicInformation` -- returns refcount, handle count, attributes
- [x] Expose `NtClose` via `SYS_CLOSEHANDLE` replacing inline `ObpFreeHandle` in syscall.c
- [x] Commit: `"kernel: ob -- NtClose, NtDuplicateObject, NtQueryObject"`
- [ ] **Migrate PID/TID-encoded process/thread handles to OB-allocated handles**: current `src/kernel/nt/nt_process.c::task_from_handle` and `src/kernel/nt/nt_token.c::resolve_process_handle` cast a raw PID/TID integer to a `HANDLE` and look up the task directly. This bypasses the per-process handle table entirely, so `granted_access`, `OBJ_INHERIT`, `DuplicateHandle`, and `NtClose` all fail to apply to process/thread handles. Change `NtOpenProcess`/`NtOpenThread` (TODO-05 §7) to call `ObpAllocateHandle(&task_current()->handle_table, task, desired_access, attrs)` using `ObpProcessType`/`ObpThreadType` (registered in §5). Then change every handler that resolves process/thread handles (`nt_process.c`, `nt_token.c`, `nt_sync.c` wait handlers that accept thread/process) to use `ObpLookupHandle` + type-check + read `granted_access`. Remove the PID/TID-cast fallback except for explicit pseudo-handles (`CURRENT_PROCESS = -2`, `CURRENT_THREAD = -3`). This auto-closes TODO-05 §16 Accepted #1 (PID/TID-encoded handles) and enables per-handle access enforcement.

## 10. Handle Inheritance Across CreateProcess
Win32 `CreateProcess` with `bInheritHandles=TRUE` copies inheritable handles into the child.

- [x] At process creation: if inherit flag is set, call `ob_handle_table_inherit(parent, child)`
- [x] For each entry with `OBJ_INHERIT` attribute: copy to child table at the same slot index with the same access rights
- [x] Call `ObReferenceObject` for each inherited handle -- child holds independent references
- [x] Inherited handle indices in the child match the parent (Win32 contract -- same HANDLE value)
- [x] On child `NtClose`, child's references are released independently of the parent's
- [x] Commit: `"kernel: ob -- handle inheritance across CreateProcess"`

## 11. Unified Kernel–User Namespace Browser API
Expose the Ob namespace as a queryable tree to user-mode via a dedicated syscall. Neither Windows nor Linux expose this publicly -- Windows `NtQueryDirectoryObject` is internal / undocumented; Linux has no equivalent.

- [x] `NtOpenDirectoryObject(name, access, &handle)` -- opens a directory by path; returns HANDLE
- [x] `NtQueryDirectoryObject(handle, buffer, count, &context, &return_count)`:
  - Enumerates entries in an `OBJECT_DIRECTORY`
  - Each entry: name string + type name string (OBJECT_DIRECTORY_INFORMATION)
- [x] Syscalls: SYS_OPENDIROBJ (42), SYS_QUERYDIROBJ (43) -- user-mode can walk `\` and enumerate all named objects
- [x] Commit: `"kernel: ob -- NtOpenDirectoryObject and NtQueryDirectoryObject (public API)"`

## 12. Per-Type Object and Handle Statistics
Track per-type creation counts, live object counts, live handle counts, and peak (high-water) values so `NtQueryObject(ObjectTypeInformation)` can return the full `OBJECT_TYPE_INFORMATION` structure that Win11 provides. Currently `NtQueryObject` only returns the type name string.

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
- [x] Add `NtQueryObject(ObjectTypesInformation)` info class -- enumerates all registered types via `ob_get_types()` into `OBJECT_TYPES_INFORMATION` struct (→ XREF: `TODO-05 §10` `NtQuerySystemInformation`)
- [x] 15 test assertions in `test_ob.c`: alloc increments, free decrements, peak preserved, handle stats, NtQueryObject struct
- [x] Commit: `"kernel: ob -- per-type object and handle statistics"`

**Test checkpoint:** `ob_alloc_object(ObpEventType)` increments `ObpEventType->total_objects`; `ObDereferenceObject` decrements it. After creating 100 events and freeing 50, `total_objects == 50` and `peak_objects == 100`. `NtQueryObject(ObjectTypeInformation)` returns correct counters. Verify on QEMU WHPX + TCG + VirtualBox. Bare metal follow-up (no hardware interaction, low risk). `POST16(0xD900)`–`POST16(0xD903)` (range `0xD9xx` confirmed free -- `0xDBxx` used by `TODO-11-usb-boot-hardening.md`).

## 13. Object Callbacks -- Handle Operation Filtering
Allow kernel-mode drivers to register pre- and post-operation callbacks on handle create and duplicate operations. Win11's `ObRegisterCallbacks` (Vista SP1+) is used by anti-malware, EDR agents, and Protected Process Light enforcement to intercept and filter access to process/thread handles.

> [!WARNING]
> Modifies `ObpAllocateHandle()` and `NtDuplicateObject()` hot paths. An incorrectly registered callback that always denies access will break all handle creation. Test with a no-op callback first, then progressively add filtering logic. Rollback: remove callback list traversal from `ObpAllocateHandle()` to restore pre-§13 behavior.

> [!NOTE]
> Process access mask constants (`PROCESS_TERMINATE`, `PROCESS_QUERY_INFORMATION`, etc.) are not yet defined -- they are deliverables of `TODO-05 §7`. For §13 testing, define local `#define PROCESS_TERMINATE 0x0001` matching the Win32 spec. Replace with the canonical header once TODO-05 §7 is complete.

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
  - Returns a registration handle (array index) for later unregistration
- [x] Implement `ObUnRegisterCallbacks(handle)` -- removes callback node, shifts array down, frees slot
- [x] In `ObpAllocateHandle()`: `ob_invoke_pre_callbacks(HANDLE_CREATE)` before security check; denies if access zeroed
- [x] In `NtDuplicateObject()`: `ob_invoke_pre_callbacks(HANDLE_DUPLICATE)` before `ObpAllocateHandle`
- [x] After successful handle creation/duplication: `ob_invoke_post_callbacks()` with granted access
- [x] 10 test assertions: register, strip TERMINATE, post-callback invoked, unregister restores full access
- [x] Commit: `"kernel: ob -- ObRegisterCallbacks handle operation filtering"`

> [!NOTE]
> Linux uses LSM (Linux Security Modules) hooks at a different layer. The Ob callback approach matches Win32 driver compatibility requirements and enables anti-tamper protection for critical processes (→ XREF: `TODO-11-security-reference-monitor.md` for PPL integration).

**Test checkpoint:** Register a callback for `ObpProcessType` that strips `PROCESS_TERMINATE` from `desired_access`. Open a handle to a process -- verify `granted_access` lacks `PROCESS_TERMINATE`. Unregister callback -- verify full access is restored. Multiple callbacks at different altitudes invoked in order. Verify on QEMU WHPX + TCG + VirtualBox. `POST16(0xD910)`–`POST16(0xD913)`.

## 14. Per-Process Handle Quota
Enforce a configurable per-process handle limit to prevent resource exhaustion from buggy or malicious processes. Win11 enforces pool quota charges per handle with a theoretical 16M limit. Linux enforces `RLIMIT_NOFILE` per process. Currently `HANDLE_TABLE_MAX_CAP` is a hard compile-time 4096 with no per-process configurability.

> [!NOTE]
> The `NtSetInformationProcess(ProcessHandleQuota)` SSDT wiring depends on `TODO-05 §10` which is not yet implemented. Core quota enforcement in `ObpAllocateHandle()` works independently -- the SSDT entry is a user-mode convenience, not a blocker.

- [x] Add `uint32_t handle_limit` to `HANDLE_TABLE` -- default `HANDLE_TABLE_DEFAULT_LIMIT` (16384); set in `ob_handle_table_init()`
- [x] In `ObpAllocateHandle()`: if `table->count >= table->handle_limit`, return `INVALID_HANDLE_VALUE` (quota exceeded) before slot search
- [x] Add `ob_handle_table_set_limit(table, new_limit)` -- clamps to `HANDLE_TABLE_ABSOLUTE_MAX` (1 << 20 = 1M handles)
- [ ] Wire into `NtSetInformationProcess(ProcessHandleQuota)` (→ XREF: `TODO-05 §10`) -- requires `SeIncreaseQuotaPrivilege` to raise above default
- [x] Track cumulative handle allocations per process in `task_t.total_handles_created` -- incremented in `ObpAllocateHandle()`
- [x] On quota exhaustion: `klog(LOG_WARN, "ob", "PID %u handle quota exhausted (%u/%u)")` with PID, count, limit
- [x] 8 test assertions: set limit, 3 allocs succeed, 4th denied, free+retry succeeds, clamp to ABSOLUTE_MAX
- [x] Commit: `"kernel: ob -- per-process handle quota enforcement"`

**Test checkpoint:** Set handle limit to 100 for a test process. Allocate 100 handles successfully. 101st allocation returns `INVALID_HANDLE_VALUE`. Free 1 handle, allocate again succeeds. `klog` warning emitted on exhaustion. Default limit (16384) works for normal boot. Verify on QEMU WHPX + TCG + VirtualBox. `POST16(0xD920)`–`POST16(0xD923)`.

## 15. Handle Tracing and Leak Detection
Provide tagged reference tracking and optional per-handle event recording for diagnosing object leaks and under-references. Win11 has `ObReferenceObjectWithTag` / `ObDereferenceObjectWithTag` (Win7+) and ETW handle tracing, but ETW is complex and buffer-limited. Linux has no equivalent. Impossible OS integrates tracing directly with klog for a better developer experience.

> [!TIP]
> Neither Windows nor Linux provides built-in, always-available handle leak detection at the kernel level without external tooling (ETW requires WPR setup; Linux requires strace/lsof). Impossible OS's klog-integrated tracing means `debug=1` in `boot.conf` automatically captures handle leaks -- zero setup required.

- [x] Implement `ObReferenceObjectWithTag(body, tag)` -- calls `ObReferenceObject` and records `{tag, +1, caller_rip}` in trace log via `__builtin_return_address(0)`
- [x] Implement `ObDereferenceObjectWithTag(body, tag)` -- records `{tag, -1, caller_rip}`, calls `ob_dump_trace` when refcount will hit 0, then `ObDereferenceObject`
- [x] Define `OB_REF_TRACE_ENTRY` -- `{uint32_t tag, int8_t delta, uintptr_t caller, uint64_t timestamp}` in `ob_trace.h`
- [x] Per-object trace log: circular buffer of 64 `OB_REF_TRACE_ENTRY` in `OB_TRACE_INFO` (`struct ob_trace_info`); allocated by `ob_trace_alloc()` when type tracing enabled; attached to `OBJECT_HEADER.trace` pointer
- [x] `ob_enable_type_tracing(type)` / `ob_disable_type_tracing(type)` -- sets `OBJECT_TYPE.tracing_enabled` flag; `ob_alloc_object()` checks it and allocates trace info
- [x] `ob_dump_trace(body)` -- emits per-tag ref/deref summary to klog; `LOG_WARN` for imbalanced tags, `LOG_DEBUG` for balanced
- [x] In `ObDereferenceObjectWithTag`: when refcount == 1 (about to hit 0) and tracing enabled, calls `ob_dump_trace` automatically before final deref
- [x] Handle event tracing: `ObpAllocateHandle` and `ObpFreeHandle` emit `klog(LOG_DEBUG, "ob", "HANDLE CREATE/FREE PID=%u H=0x%x ...")` when `g_ob_handle_trace` is set
- [x] `g_ob_handle_trace` flag: set from `boot.conf` `ob_handle_trace=1`; wired in Phase 2 after OB init
- [x] 7 test assertions: enable tracing, alloc gets trace info, tagged ref/deref, trace log entries, disable tracing
- [x] Commit: `"kernel: ob -- tagged reference tracing and handle leak detection"`

**Test checkpoint:** Enable tracing for `ObpEventType`. Create event, ref with tag `"Lk01"`, ref with tag `"Lk02"`, deref with tag `"Lk01"`, deref (untagged). On final deref (refcount 0), `ob_dump_trace` reports tag `"Lk02"` has 1 ref / 0 deref = over-reference by 1. `ob_handle_trace=1`: every `ObpAllocateHandle` / `ObpFreeHandle` emits a klog entry with PID, handle value, object pointer, and type name. Verify on QEMU WHPX + TCG. `POST16(0xD930)`–`POST16(0xD933)`.

---

## OS Comparison

| ⭐ | Feature               | 🪟 Win11               | 🐧 Linux             | 🚀 Impossible OS              |
|----|-----------------------|-------------------------|----------------------|--------------------------------|
| 💎 | Typed object header   | ✅ OBJECT_HEADER       | ✅ kobject + kref    | ✅ §1                         |
| 💎 | Type descriptors      | ✅ OBJECT_TYPE hooks   | ✅ kobj_type         | ✅ §1                         |
| 💎 | Auto-delete on 0 ref  | ✅ ObDereferenceObject | ✅ kref_put          | ✅ §2                         |
| 💎 | Per-process handles   | ✅ HANDLE_TABLE        | ✅ fd table          | ✅ §3                         |
| 💎 | Named namespace       | ✅ \BaseNamedObjects   | ✅ /proc, /sys       | ✅ §4                         |
| 💎 | File objects          | ✅ FILE_OBJECT         | ✅ struct file       | ✅ §5                         |
| 💎 | Process/thread objs   | ✅ EPROCESS/ETHREAD    | ✅ task_struct       | ✅ §5                         |
| 💎 | Named sync objects    | ✅ Named events/mutex  | ✅ POSIX named sem   | ✅ §6                         |
| 💎 | Section objects       | ✅ SECTION_OBJECT      | ✅ anonymous mmap    | ✅ §7                         |
| 💎 | Security descriptors  | ✅ DACL/SACL           | ✅ inode perms/ACLs  | ✅ §8                         |
| 💎 | Duplicate/inherit     | ✅ Full semantics      | ✅ dup/O_CLOEXEC     | ✅ §9, §10                    |
| ⭐ | Public namespace API  | ❌ Internal only       | ❌ No equivalent     | ✅ §11 -- public, documented  |
| ⭐ | Unified type system   | ⚠️ Partial ObXxx       | ❌ Split fd/kobject  | ✅ §1–§7 -- one header        |
| 💎 | Per-type statistics   | ✅ OBJECT_TYPE_INFO    | ✅ /proc/slabinfo    | ✅ §12 -- atomic counters     |
| 💎 | Handle op callbacks   | ✅ ObRegisterCallbacks | ⚠️ LSM hooks         | ✅ §13 -- pre/post filtering  |
| 💎 | Handle quota          | ✅ 16M + pool quota    | ✅ RLIMIT_NOFILE     | ✅ §14 -- 16K default, 1M max |
| ⭐ | Handle leak detection | ⚠️ ETW (complex)       | ❌ No built-in       | ✅ §15 -- klog-integrated     |

> All foundational parity items (§1–§11) complete -- Impossible OS matches Windows NT object management.
> Three exclusive features (⭐): public namespace browser API (§11), unified single-header type system (§1–§7), and built-in klog-integrated handle leak detection (§15).
> Four new parity gaps identified: per-type statistics (§12), handle operation callbacks (§13), handle quota (§14) require implementation for full NT driver compatibility.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_ob()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
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
- [x] §13 tests: `ObRegisterCallbacks` pre-callback strips TERMINATE; post-callback invoked; `ObUnRegisterCallbacks` restores full access (10 assertions in `test_ob.c`)
- [x] §14 tests: handle limit set to 3, allocs 0-2 succeed, 3rd denied, free+retry succeeds, clamp to ABSOLUTE_MAX (8 assertions in `test_ob.c`)
- [x] §15 tests: enable/disable tracing, trace info allocated, tagged ref/deref, trace log count, refcount verified (7 assertions in `test_ob.c`)

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
- [x] All §1–§11 committed individually (12 commits across OB §5–§11, security §1–§4, OB §8)
