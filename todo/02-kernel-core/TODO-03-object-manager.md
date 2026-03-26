# TODO-03 — Object Manager

> **Goal:** Implement the kernel Object Manager (ObXxx layer) — the unified substrate
> that gives every kernel resource (files, processes, threads, events, mutexes,
> semaphores, registry keys, sections) a typed header, reference-counted lifetime,
> named namespace entry, security descriptor, and per-process handle-table slot.
> Without this, Win32 `HANDLE` semantics are impossible and resource leaks are
> unavoidable. This is the single most foundational Win32 prerequisite in the kernel.

> [!IMPORTANT]
> **Current state:** No Object Manager exists. VFS returns raw `vfs_node *` pointers
> with no reference counting. Syscalls use raw fd integers (0, 1…) from a POSIX-style
> table. IPC primitives (events, mutexes, semaphores, pipes, shared memory) are
> unregistered in any unified namespace and cannot be named, duplicated, or inherited.

## Inputs

- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h)
- [`include/kernel/fs/vfs.h`](../../include/kernel/fs/vfs.h)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) — process/thread struct
- [`src/kernel/sched/event.c`](../../src/kernel/sched/event.c)
- [`src/kernel/sched/mutex.c`](../../src/kernel/sched/mutex.c)
- [`src/kernel/sched/semaphore.c`](../../src/kernel/sched/semaphore.c)
- [`src/kernel/ipc/`](../../src/kernel/ipc/) — pipes, shared memory
- → XREF: `TODO-01-kernel-init-sequencing.md` §4 — ObInit (`object_manager_init()`) is a Phase 2 gate, before registry
- → XREF: `TODO-04-peb-teb-user-abi.md` — TODO-04 §10 exposes PEB/TEB in the Ob namespace (depends on §4 Object Namespace and §11 Namespace Browser)
- → XREF: `TODO-05-native-api-layer.md` — NtCreateFile / NtOpenFile / NtClose and sync Nt syscalls are all Ob-routed; SSDT entries depend on §3, §5, §6
- → XREF: `TODO-11-security-reference-monitor.md` — SRM enforces DACLs registered here

## Outcome

- Every kernel object carries an `OBJECT_HEADER` with refcount, type pointer, name, and security descriptor.
- `ObCreateObject` / `ObReferenceObject` / `ObDereferenceObject` manage all object lifetimes.
- Every process owns a handle table mapping `HANDLE` integers to (object, access rights, attributes).
- Named objects live in a hierarchical namespace: `\`, `\Device\`, `\BaseNamedObjects\`, `\DosDevices\`.
- `NtCreateFile`, `NtOpenFile`, `NtClose`, `NtDuplicateObject` are routed through the Ob layer.
- Existing IPC primitives are re-registered as Ob-managed named object types.

## Implementation Order

| ⭐  | Order | Deliverable                                                   | Depends On | Status |
| --- | :---: | ------------------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | OBJECT_HEADER and OBJECT_TYPE infrastructure                  | —          |  [ ]   |
| 💎  |   2   | Reference counting and object lifetime                        | 1          |  [ ]   |
| 💎  |   3   | Per-process handle table                                      | 2          |  [ ]   |
| 💎  |   4   | Object namespace (directory + symbolic link)                  | 2          |  [ ]   |
| 💎  |   5   | File, Process, Thread object types                            | 3, 4       |  [ ]   |
| 💎  |   6   | Synchronisation object types (Event, Mutex, Semaphore, Timer) | 3, 4 | [ ] |
| 💎  |   7   | Section (shared memory) object type                           | 3, 4       |  [ ]   |
| 💎  |   8   | Security descriptor integration                               | 1, TODO-11 |  [ ]   |
| 💎  |   9   | NtClose / NtDuplicateObject / NtQueryObject                   | 3          |  [ ]   |
| 💎  |  10   | Handle inheritance across CreateProcess                       | 3, 5       |  [ ]   |
| ⭐  |  11   | Unified kernel–user namespace browser API                     | 4          |  [ ]   |

> 💎 = parity — Windows NT ObXxx and Linux kobject/fd_table both provide these capabilities.
> ⭐ = exclusive — a queryable namespace browser API exposed to user-mode that covers every
>     kernel object type in one unified tree; neither Windows nor Linux expose this publicly.

---

## 1. OBJECT_HEADER and OBJECT_TYPE Infrastructure `[Opus]`

Every kernel object body is preceded in memory by an `OBJECT_HEADER`. Types are described
by an `OBJECT_TYPE` singleton registered at boot.

- [ ] Define `OBJECT_HEADER` in `include/kernel/ob/ob.h`:
  - `uint32_t ref_count` — atomic reference count
  - `uint32_t handle_count` — number of open handles
  - `const OBJECT_TYPE *type` — pointer to the type descriptor
  - `const char *name` — pointer into the name buffer (NULL if unnamed)
  - `SECURITY_DESCRIPTOR *security` — NULL until §8
  - `uint32_t flags` — `OB_FLAG_PERMANENT`, `OB_FLAG_KERNEL_ONLY`, `OB_FLAG_NAMED`
- [ ] Define `OBJECT_TYPE` in `include/kernel/ob/ob_type.h`:
  - `const char *name` — type name string (e.g. `"File"`, `"Process"`)
  - `void (*on_close)(void *body, uint32_t handle_count)` — called when handle count drops to 0
  - `void (*on_delete)(void *body)` — called when ref count drops to 0; frees body memory
  - `int (*on_open)(void *body, uint32_t access)` — optional access check on open
  - `int (*on_parse)(void *body, const char *remaining, void **result)` — namespace parse
  - `size_t body_size` — size of the object body (for combined allocation)
- [ ] Implement `ob_create_type(name, callbacks, body_size)` — registers a type in a static global type table
- [ ] Implement `ob_alloc_object(type)` — allocates `sizeof(OBJECT_HEADER) + type->body_size` via `kmalloc` or
      `pmm_alloc_contiguous` based on body size; returns pointer to the body (header is at `body - sizeof(OBJECT_HEADER)`)
- [ ] Add `OB_HEADER_FROM_BODY(ptr)` macro — subtracts header size from a body pointer
- [ ] Register built-in type singletons at ObInit time: `ObpFileType`, `ObpProcessType`, `ObpThreadType`,
      `ObpDirectoryType`, `ObpSymlinkType`, `ObpEventType`, `ObpMutexType`, `ObpSemaphoreType`,
      `ObpSectionType`, `ObpTimerType`
- [ ] Commit: `"kernel: ob — OBJECT_HEADER and OBJECT_TYPE infrastructure"`

## 2. Reference Counting and Object Lifetime `[Opus]`

- [ ] Implement `ObReferenceObject(void *body)` — atomic increment of `header->ref_count`
- [ ] Implement `ObDereferenceObject(void *body)` — atomic decrement; when count reaches 0,
      call `type->on_delete(body)` and free the combined allocation
- [ ] Implement `ObReferenceObjectByPointer(void *body, uint32_t access)` — validates type before ref
- [ ] Permanent objects (`OB_FLAG_PERMANENT`) are never deleted when refcount hits 0; must be explicitly
      made temporary with `ObMakeTemporaryObject()` first
- [ ] All existing code that stores raw `vfs_node *` pointers must be audited and wrapped in ObRef/ObDeref
      pairs — track this as a follow-up checklist in §5
- [ ] Commit: `"kernel: ob — reference counting and object lifetime"`

## 3. Per-Process Handle Table `[Sonnet]`

The handle table maps opaque `HANDLE` integer values to (object pointer + granted access + flags) within a
single process. `HANDLE` values are always multiples of 4 (low bits reserved for inheritance flags).

- [ ] Define `HANDLE_TABLE_ENTRY` in `include/kernel/ob/handle_table.h`:
  - `void *object` — body pointer (NULL = free slot)
  - `uint32_t granted_access` — access rights granted at open time
  - `uint32_t attributes` — `OBJ_INHERIT`, `OBJ_PROTECT_CLOSE`
- [ ] Define `HANDLE_TABLE` — array of `HANDLE_TABLE_ENTRY` with grow-on-demand capacity
      (initial: 64 entries, grows by doubling; allocated via `kmalloc` for small tables,
      `pmm_alloc_contiguous` for tables exceeding 4 KB)
- [ ] Add `HANDLE_TABLE handle_table` to the `task_t` / process struct in `task.c`
- [ ] Implement `ObpAllocateHandle(table, object, access, attrs)` — finds a free slot, stores entry,
      calls `ObReferenceObject`, returns `HANDLE` value (slot index × 4)
- [ ] Implement `ObpFreeHandle(table, handle)` — validates handle, calls `type->on_close` if handle_count
      drops to 0, calls `ObDereferenceObject`, zeroes slot
- [ ] Implement `ObpLookupHandle(table, handle)` — validates index bounds and non-NULL; returns entry ptr
- [ ] Handle table is freed at process exit: iterate all slots and call `ObpFreeHandle` for each
- [ ] Special kernel pseudo-handles: `INVALID_HANDLE_VALUE (-1)`, `CURRENT_PROCESS (-2)`,
      `CURRENT_THREAD (-3)` — resolve in `ObpLookupHandle` without a table entry
- [ ] Commit: `"kernel: ob — per-process handle table"`

## 4. Object Namespace `[Opus]`

A hierarchical in-memory namespace rooted at `\`. Directories hold named object entries.
Symbolic links redirect name lookups.

- [ ] Define `OBJECT_DIRECTORY_ENTRY` — linked list node: `name[64]`, `void *object`
- [ ] Implement `OBJECT_DIRECTORY` body type — list of entries, lock, parent pointer
- [ ] Implement `ObpLookupDirectory(path, &remaining)` — walks `\Foo\Bar` splitting on `\`;
      follows symlinks; stops at the deepest found directory
- [ ] Implement `ObInsertObject(object, name, directory)` — adds a named entry; fails if name exists
      and object is not a permanent replacement
- [ ] Implement `ObLookupObjectByName(path, type, access, &result)` — full parse walk calling
      `type->on_parse` at each node; calls `ObReferenceObject` on success
- [ ] Implement `OBJECT_SYMBOLIC_LINK` body type — target string; `on_parse` redirects the walk
- [ ] Create the root namespace at ObInit: `\`, `\Device`, `\KernelObjects`, `\DosDevices`,
      `\BaseNamedObjects`, `\Sessions\0\BaseNamedObjects` (alias for user-mode named objects)
- [ ] `C:` → `\Device\HardDisk0\Partition0` via symlink in `\DosDevices`
- [ ] Commit: `"kernel: ob — object namespace directory and symlinks"`

## 5. File, Process, and Thread Object Types `[Sonnet]`

Register VFS nodes, tasks, and threads as first-class Ob-managed objects.

- [ ] Register `ObpFileType` — body wraps a `vfs_node *`; `on_close` calls `vfs_close`; `on_delete` frees
      the VFS node wrapper; `on_parse` defers remaining path to VFS
- [ ] Replace all direct `vfs_open` / `vfs_close` call sites in syscall.c with ObRef/ObDeref wrappers
      routed through `NtCreateFile` / `NtOpenFile` stubs (→ XREF: TODO-05 §6)
- [ ] Register `ObpProcessType` — body is `task_t *`; permanent while process is alive; `on_delete`
      releases task memory; inserted into `\KernelObjects\Process<PID>` at creation
- [ ] Register `ObpThreadType` — body is `thread_t *` (or task sub-struct); same lifetime semantics
- [ ] Audit all existing raw `task_t *` / `vfs_node *` storage in the syscall table and IPC code;
      replace with handle-table lookups or `ObReferenceObjectByPointer` calls
- [ ] Commit: `"kernel: ob — file, process, thread object types"`

## 6. Synchronisation Object Types `[Sonnet]`

Re-register the existing event, mutex, semaphore, and timer primitives as Ob-managed objects
so they can be named in `\BaseNamedObjects`, duplicated, and inherited.

- [ ] Register `ObpEventType` — body wraps existing `event_t`; `on_delete` frees event state
- [ ] Register `ObpMutexType` — body wraps existing `mutex_t`; `on_delete` frees mutex state;
      if thread holding the mutex is deleted, signal `MUTEX_ABANDONED`
- [ ] Register `ObpSemaphoreType` — body wraps existing `semaphore_t`
- [ ] Register `ObpTimerType` — body wraps timer state; `on_close` cancels the timer
- [ ] Named variants: inserting into `\BaseNamedObjects\<name>` makes the object findable by name
- [ ] `NtCreateEvent`, `NtOpenEvent`, `NtCreateMutex`, `NtOpenMutex`, `NtCreateSemaphore`,
      `NtCreateTimer` stubs — resolve name via Ob namespace, allocate or open, return HANDLE
      (→ XREF: TODO-05 §8)
- [ ] Update `SYS_PIPE` to wrap the pipe read/write ends as two File objects in the handle table
      so pipes are closeable with `NtClose` like any other handle
- [ ] Commit: `"kernel: ob — event, mutex, semaphore, timer object types"`

## 7. Section (Shared Memory) Object Type `[Sonnet]`

Sections represent mappable memory objects; the foundation for `MapViewOfFile` and shared memory.

- [ ] Register `ObpSectionType` — body: physical base address, size, page count, flags, refcount
- [ ] `ObCreateSection(size, protect, name)` — allocates contiguous physical pages via
      `pmm_alloc_contiguous`; inserts into Ob namespace if named
- [ ] `ObMapViewOfSection(section, process, address, size, offset, protect)` — maps the
      physical pages into the target process address space via VMM page-table entries
- [ ] `ObUnmapViewOfSection(process, base_address)` — removes VMM mappings; does not free
      physical pages until refcount drops to 0
- [ ] Re-implement `SYS_SHMEM_CREATE` / `SYS_SHMEM_MAP` as thin wrappers over the Ob section API
- [ ] Commit: `"kernel: ob — section object type and view mapping"`

## 8. Security Descriptor Integration `[Opus]`

Attach DACL/SACL/Owner/Group to named objects so the Security Reference Monitor can enforce
access rights at open time. Depends on TODO-11 (SRM) for full enforcement; this section
wires the storage and basic check hook.

- [ ] Define minimal `SECURITY_DESCRIPTOR` in `include/kernel/ob/security.h`:
      owner SID, group SID, DACL pointer, SACL pointer, flags
- [ ] `ObSetSecurityDescriptor(object, sd)` — attaches an SD; stored in `header->security`
- [ ] `ObGetSecurityDescriptor(object, &sd)` — returns current SD
- [ ] In `ObpAllocateHandle`: call `SrmAccessCheck(header->security, requested_access)` if
      `SUBSYS_SRM` is ready — deny handle creation on access denied
- [ ] Default SD for kernel-created objects: DACL granting `GENERIC_ALL` to SYSTEM SID only
- [ ] Default SD for user-created named objects: DACL granting `GENERIC_ALL` to creator SID
- [ ] Commit: `"kernel: ob — security descriptor storage and access check hook"`

## 9. NtClose / NtDuplicateObject / NtQueryObject `[Sonnet]`

Core Win32 handle management syscalls routed through the Ob layer.

- [ ] `NtClose(HANDLE)` — look up handle in calling process's table, call `ObpFreeHandle`
- [ ] `NtDuplicateObject(src_process, src_handle, dst_process, &dst_handle, access, attrs, options)`:
  - Look up `src_handle` in `src_process` handle table
  - Allocate new entry in `dst_process` handle table pointing to the same object
  - Call `ObReferenceObject` for the new reference
  - If `DUPLICATE_CLOSE_SOURCE` is set, free the source handle
- [ ] `NtQueryObject(handle, info_class, buffer, size, &return_length)`:
  - `ObjectNameInformation` — returns the Ob namespace path of the object
  - `ObjectTypeInformation` — returns type name, total handles, total references
  - `ObjectBasicInformation` — returns refcount, handle count, attributes
- [ ] Expose `NtClose` via `SYS_CLOSE` (add to `syscall.h`) replacing per-resource close syscalls
- [ ] Commit: `"kernel: ob — NtClose, NtDuplicateObject, NtQueryObject"`

## 10. Handle Inheritance Across CreateProcess `[Sonnet]`

Win32 `CreateProcess` with `bInheritHandles=TRUE` copies inheritable handles into the child.

- [ ] At process creation: if inherit flag is set, iterate parent handle table
- [ ] For each entry with `OBJ_INHERIT` attribute: call `ObpAllocateHandle` in child table at the
      same slot index with the same access rights
- [ ] Call `ObReferenceObject` for each inherited handle — child holds independent references
- [ ] Inherited handle indices in the child match the parent (Win32 contract)
- [ ] On child `NtClose`, child's references are released independently of the parent's
- [ ] Commit: `"kernel: ob — handle inheritance across CreateProcess"`

## 11. Unified Kernel–User Namespace Browser API `[Opus]`

Expose the Ob namespace as a queryable tree to user-mode via a dedicate syscall.
Neither Windows nor Linux expose this publicly — Windows `NtQueryDirectoryObject` is
internal / undocumented; Linux has no equivalent.

- [ ] `NtOpenDirectoryObject(name, access, &handle)` — opens a directory by path; returns HANDLE
- [ ] `NtQueryDirectoryObject(handle, buffer, size, single_entry, first_scan, &context, &return_length)`:
  - Enumerates entries in an `OBJECT_DIRECTORY`
  - Each entry: name string + type name string
- [ ] User-mode `ObBrowse.exe` or desktop shell namespace panel can walk `\` and enumerate all
      named objects — files, devices, named events, sections — in one unified tree
- [ ] Document the API as a public Impossible OS extension (not available on Windows or Linux)
- [ ] Commit: `"kernel: ob — NtOpenDirectoryObject and NtQueryDirectoryObject (public API)"`

---

## OS Comparison

| ⭐  | Feature                                  | 🪟 Windows NT / 11                          | 🐧 Linux                                      | 🚀 Impossible OS                              |
| --- | ---------------------------------------- | ---------------------------------------------| --------------------------------------------- | ---------------------------------------------- |
| 💎  | Typed object header with refcount        | ✅ OBJECT_HEADER (non-paged pool)           | ✅ `kobject` + `kref`                         | ⬜ Planned — §1                               |
| 💎  | Object type descriptors with hooks       | ✅ OBJECT_TYPE (CreateProc, DeleteProc)     | ✅ `kobj_type` (release, sysfs_ops)           | ⬜ Planned — §1                               |
| 💎  | Automatic deletion on zero refcount      | ✅ ObDereferenceObject → frees when 0       | ✅ `kref_put` → release callback              | ⬜ Planned — §2                               |
| 💎  | Per-process handle table                 | ✅ `HANDLE_TABLE` in kernel EPROCESS        | ✅ `struct files_struct` fd table             | ⬜ Planned — §3                               |
| 💎  | Named kernel object namespace            | ✅ `\BaseNamedObjects\`, `\Device\`         | ✅ `/proc`, `/sys`, tmpfs named objects       | ⬜ Planned — §4                               |
| 💎  | File objects in handle table             | ✅ FILE_OBJECT with ObXxx routing           | ✅ `struct file` in fd table                  | ⬜ Planned — §5                               |
| 💎  | Process/thread objects with handles      | ✅ EPROCESS / ETHREAD — ObXxx managed       | ✅ `struct task_struct` with `/proc/<pid>`    | ⬜ Planned — §5                               |
| 💎  | Named sync objects (events, mutexes)     | ✅ `\BaseNamedObjects\<name>`               | ✅ POSIX named semaphores in `/dev/shm`       | ⬜ Planned — §6                               |
| 💎  | Section / memory-mapped file objects     | ✅ SECTION_OBJECT via ObXxx                 | ✅ `struct file` backing anonymous mmap       | ⬜ Planned — §7                               |
| 💎  | Security descriptors on objects          | ✅ DACL/SACL on every named object          | ✅ inode permissions / POSIX ACLs             | ⬜ Planned — §8                               |
| 💎  | NtDuplicateObject / handle inherit       | ✅ Full inherit + duplicate semantics       | ✅ `dup()`/`dup2()` + `O_CLOEXEC`             | ⬜ Planned — §9, §10                          |
| ⭐  | Public namespace browser API             | ❌ Internal only (NtQueryDirectoryObject)   | ❌ No unified namespace API                   | ⬜ **Planned — §11 — public, documented**     |
| ⭐  | Unified type system across all resources | ⚠️ Not all resources use ObXxx (e.g. IRPs)  | ❌ fd table and kobject are separate systems  | ⬜ **Planned — §1–§7 — one header for all**   |

> **After §1–§10:** Impossible OS achieves full parity with Windows NT object management and
> exceeds Linux's split fd/kobject model with a single unified type system.
> **§11** makes the Ob namespace a public, documented API — something neither Windows nor Linux
> offer — enabling user-mode tools to browse every kernel resource in one place.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `ObCreateObject(ObpFileType)` returns a valid body pointer; `OB_HEADER_FROM_BODY` recovers the header
- [ ] `ObReferenceObject` + `ObDereferenceObject` pair drives refcount to 0 and calls `on_delete`
- [ ] Process handle table survives 1000 `ObpAllocateHandle` + `ObpFreeHandle` round trips with no leak
- [ ] Named event created in `\BaseNamedObjects\TestEvent` is findable via `ObLookupObjectByName`
- [ ] `NtDuplicateObject` produces an independent handle; closing source does not affect duplicate
- [ ] Child process inherits `OBJ_INHERIT` handles at correct indices
- [ ] `NtQueryDirectoryObject` on `\` enumerates at least `Device`, `KernelObjects`, `BaseNamedObjects`
- [ ] KASAN / heap checker shows zero use-after-free on object deletion under QEMU
- [ ] Commit: `"kernel: ob — object manager complete"`
