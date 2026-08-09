---
schema_version: 1
id: win32-file-io-api
domain: 05-storage-filesystems
status: active
title: "TODO-05 -- Win32 File I/O API & IRP Layer"
---

# TODO-05 -- Win32 File I/O API & IRP Layer

> **Goal:** Build the complete Win32 I/O subsystem from scratch -- IRP engine, MDL, file handle table, `CreateFile`/`ReadFile`/`WriteFile`, directory APIs, file management, metadata, async I/O + APC, filter manager stub, file change notifications, and `GetDiskFreeSpaceEx` -- then migrate all kernel and shell internal files from raw `vfs_open`/`vfs_read`/`vfs_write` calls to the new Win32 layer. This is the **P0 blocker for all user-mode programs**.

> [!IMPORTANT]
> No Win32 file API exists yet. The current syscall layer (`SYS_READFILE`, `SYS_READDIR`) is a primitive stub. Raw `vfs_open`/`vfs_read`/`vfs_write` calls are scattered across ~15 kernel and desktop source files that §11 migrates. Execution order is strict: §1 (IRP) → §2 (MDL) → §3 (handle table) → §4 (CreateFile) → §5 (ReadFile/WriteFile) → §6 (directories) → §7 (file management) → §8 (metadata) → §9 (async) → §10 (filter manager) → §11 (migration) → §12 (change notifications) → §13 (disk free space). Do not start §4 before §3; do not start §11 before §5. The handle table in §3 **extends** the Object Manager (→ XREF: `10-platform-services` Object Manager TODO) -- coordinate on the `HANDLE` type, reference counting, and `ObReferenceObjectByHandle`.

## Inputs

- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- `vfs_open`/`vfs_read`/`vfs_write`/`vfs_close`; the IRP dispatch in §1 calls into this layer; NOT replaced -- VFS remains the kernel-internal API; Win32 wraps it
- `src/kernel/sched/syscall.c` -- `SYS_READFILE`/`SYS_READDIR` stubs; §3 + §4 + §5 replace them with `NtCreateFile`/`NtReadFile`/`NtWriteFile` syscall numbers
- `src/kernel/sched/` -- process task struct; §3 adds per-process handle table; §6 adds per-process CWD
- `src/desktop/`, `src/kernel/gfx/gfx_text.c`, `src/kernel/panic.c`, `src/kernel/klog_disk.c`, `src/kernel/registry.c`, `src/kernel/image.c`, `src/kernel/mm/swap.c`, `src/kernel/mm/mmap.c`, `src/kernel/symtab.c`, `src/kernel/ico.c`, `src/kernel/icon_store.c`, `src/kernel/image_save.c`, `src/kernel/drivers/cursor.c` -- raw `vfs_open` callers migrated in §11
- → XREF: `10-platform-services` Object Manager TODO -- `HANDLE`, reference counting, `ObReferenceObjectByHandle`, `OBJ_INHERIT` flag; handle table in §3 must use the same type definitions
- → XREF: `10-platform-services` Process Object TODO -- per-process handle table lives in the process object; coordinate on layout
- → XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md §7–10` -- share-mode, delete-on-close, byte-range locks are VFS-layer; `NtCreateFile` in §6 passes these flags down to `vfs_open()`
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §4` -- `GetDiskFreeSpaceEx` and `GetVolumeInformation` (§13) depend on Registry volume entries from `vfs_probe()`
- → XREF: `12-user-platform-sdk` Win32 API TODO -- `CreateFile`/`ReadFile`/`WriteFile` defined here are the kernel implementations; the SDK exposes them as user-mode wrappers
- → XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md §11,§12` -- KAPC object and KiDeliverApc; §6 async I/O completion queues a user-mode APC to the issuing thread via KeInsertQueueApc
- → XREF: `02-kernel-core/TODO-15-security-reference-monitor.md §8` -- `SeAccessCheck` enforcement; `NtCreateFile` in §6 must call `SeAccessCheck` with `FILE_GENERIC_READ`/`WRITE` desired access against the file object's DACL

## Outcome

- `IRP` engine dispatches `IRP_MJ_CREATE`/`READ`/`WRITE`/`CLOSE`/`QUERY_INFORMATION`/`SET_INFORMATION`/`DIRECTORY_CONTROL` synchronously and asynchronously.
- `MmCreateMdl` / `MmProbeAndLockPages` / `MmGetSystemAddressForMdl` safe-copies user buffers into kernel space.
- Per-process `HANDLE` table with `NtCreateFile` (path → `FILE_OBJECT`, alloc handle) and `NtClose`.
- `CreateFile`/`CloseHandle` Win32 wrappers functional with all creation dispositions and flags.
- `ReadFile`/`WriteFile`/`SetFilePointerEx` functional; overlapped I/O queues IOCP completion packets.
- `FindFirstFileW`/`FindNextFileW`/`FindClose`, `CreateDirectoryW`/`RemoveDirectoryW`, per-process CWD.
- `DeleteFileW`, `MoveFileExW` (within-volume rename; cross-volume copy+delete), `CopyFileW`.
- File attribute + size + time metadata APIs all functional.
- `OVERLAPPED` + `GetOverlappedResult` + user-mode APC on completion.
- `FltMgr` registration stub: `FltRegisterFilter`/`FltUnregisterFilter` pass-through by default.
- All ~15 internal callers migrated from `vfs_open` to `CreateFile`/`ReadFile`/`WriteFile`.
- `FindFirstChangeNotification`/`ReadDirectoryChangesW` post change events to watchers.
- `GetDiskFreeSpaceEx`/`GetVolumeInformation` query the VFS layer.

## Implementation Order

| ⭐  | Order | Deliverable                                                                              | Depends On                                                     | Status |
| --- | :---: | ---------------------------------------------------------------------------------------- | -------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 IRP engine -- `struct IRP`, allocate/complete, synchronous dispatch, 7 major functions  | `vfs.c` (IRP calls into it)                                    |  [ ]   |
| 💎  |   2   | §2 MDL -- `MmCreateMdl`, probe+lock, `MmGetSystemAddressForMdl`, unlock+free             | §1 (IRP carries MDL pointer)                                   |  [ ]   |
| 💎  |   3   | §3 File handle table -- per-process `HANDLE`→`FILE_OBJECT*`, `NtCreateFile`, `NtClose`  | §1 + §2, Object Manager handle type                            |  [ ]   |
| 💎  |   4   | §4 `CreateFile`/`CloseHandle` -- dispositions, flags, Win32 wrapper over `NtCreateFile`  | §3 (handle table)                                              |  [ ]   |
| 💎  |   5   | §5 `ReadFile`/`WriteFile`/`SetFilePointerEx` -- sync + overlapped via IRP               | §4 (`FILE_OBJECT.CurrentByteOffset`)                           |  [ ]   |
| 💎  |   6   | §6 Directory APIs -- `FindFirstFileW`/`FindNextFileW`/`FindClose`, `CreateDirectoryW`, CWD | §5 (file path open proven)                                   |  [ ]   |
| 💎  |   7   | §7 File management -- `DeleteFileW`, `MoveFileExW`, `CopyFileW`                          | §6 (directory context for rename), §5 (copy uses read/write)   |  [ ]   |
| 💎  |   8   | §8 File metadata -- `GetFileAttributesW`, `GetFileSizeEx`, `GetFileTime`/`SetFileTime`   | §3 (handle → file object), §5 (offset for size)                |  [ ]   |
| 💎  |   9   | §9 Async I/O + APC -- `OVERLAPPED`, `GetOverlappedResult`, completion APC to caller      | §5 (async IRP dispatch path)                                   |  [ ]   |
| 💎  |  10   | §10 Filter manager stub -- `FltRegisterFilter`/`FltUnregisterFilter`, pass-through       | §1 (IRP pre/post hook points)                                  |  [ ]   |
| ⭐  |  11   | §11 Internal migration -- all `vfs_open` callers → `CreateFile`/`ReadFile`/`WriteFile`  | §5 + §8 (full read/write + metadata available)                  |  [ ]   |
| 💎  |  12   | §12 File change notifications -- `FindFirstChangeNotification`/`ReadDirectoryChangesW`   | §6 (directory watch list), §3 (handle for notification object) |  [ ]   |
| 💎  |  13   | §13 `GetDiskFreeSpaceEx` / `GetVolumeInformation` -- VFS query + Win32 surface            | TODO-03 §4 (Registry volume entries exist)                     |  [ ]   |
| 💎  |  14   | §14 `DeviceIoControl` + FSCTL dispatch + oplock ioctls + volume lock                     | §3 + §4 (handle table), TODO-04 §14 (vfs_request_oplock)        |  [ ]   |

> §11 (internal migration) is `⭐` exclusive in scope: Windows was built Win32-first and never needed a migration pass; Linux never migrates anything to Win32. Impossible OS performs a clean architectural cut -- kernel-internal code uses `vfs_*` (fast, no handle overhead), while everything visible to user-mode programs uses `CreateFile`/`ReadFile`/`WriteFile` (full Win32 semantics). This gives user-mode programs an unmodified Win32 file API while keeping the kernel core free of handle-table overhead for internal operations.

---

## 1. IRP Engine `[Opus]`

Define `struct IRP` and implement the core dispatch lifecycle: `IoAllocateIrp`, `IoCompleteRequest`, synchronous dispatch, and the seven `IRP_MJ_*` major function codes used by filesystem drivers.

**Files:** `src/kernel/io/irp.c` (new), `include/kernel/io/irp.h` (new)

> [!NOTE]
> IRP layout: `UCHAR MajorFunction`; `UCHAR MinorFunction`; `NTSTATUS IoStatus.Status`; `ULONG_PTR IoStatus.Information`; `void *DeviceObject`; `void *FileObject`; `struct MDL *MdlAddress`; `IO_COMPLETION_ROUTINE CompletionRoutine`; `union Parameters { CREATE, READ, WRITE, QUERY_INFO, SET_INFO, DIR_CTRL } p`. Major function codes: `IRP_MJ_CREATE (0x00)`, `IRP_MJ_READ (0x03)`, `IRP_MJ_WRITE (0x04)`, `IRP_MJ_CLOSE (0x02)`, `IRP_MJ_QUERY_INFORMATION (0x05)`, `IRP_MJ_SET_INFORMATION (0x06)`, `IRP_MJ_DIRECTORY_CONTROL (0x0C)`. Dispatch: the IRP passes through a per-device `DRIVER_OBJECT.MajorFunction[]` function pointer table; for file operations, the FS driver's handler calls `vfs_*` and calls `IoCompleteRequest()`.

- [ ] `struct IRP { uint8_t MajorFunction; uint8_t MinorFunction; NTSTATUS status; ULONG_PTR info; void *DeviceObject; FILE_OBJECT *FileObject; struct MDL *MdlAddress; IO_COMPLETION_ROUTINE *CompletionRoutine; void *CompletionContext; union { struct { ACCESS_MASK access; ULONG share; ULONG disposition; ULONG options; } Create; struct { LARGE_INTEGER offset; ULONG len; } ReadWrite; struct { FILE_INFORMATION_CLASS cls; ULONG len; void *buf; } QueryInfo; struct { uint32_t index; uint32_t restart; UNICODE_STRING mask; } DirCtrl; } p; }`
- [ ] `IoAllocateIrp(MajorFunction)` → allocate from heap; zero-init; set `MajorFunction`; return pointer
- [ ] `IoFreeIrp(irp)` → `kfree(irp)`
- [ ] `IoCompleteRequest(irp, priority_boost)`: call `irp->CompletionRoutine(irp, irp->CompletionContext)` if non-NULL; set completion event if synchronous wait pending
- [ ] `IoCallDriver(device_obj, irp)`: look up `device_obj->DriverObject->MajorFunction[irp->MajorFunction]`; call it; return NTSTATUS
- [ ] FS driver adapter: `vfs_irp_dispatch(irp)` -- switch on `MajorFunction`; `CREATE` → `vfs_open()`; `READ` → `vfs_read()`; `WRITE` → `vfs_write()`; `CLOSE` → `vfs_close()`; `QUERY_INFORMATION` → `vfs_stat()`; `DIR_CONTROL` → `vfs_readdir()`; call `IoCompleteRequest()` in each
- [ ] Log: `[IRP] %s MajorFn=0x%02x → NTSTATUS 0x%x` (debug only, gated by `klog_level >= LOG_DEBUG`)
- [ ] Commit: `"io: IRP engine -- struct IRP, IoAllocateIrp/Complete/CallDriver, vfs_irp_dispatch"`

## 2. MDL -- Memory Descriptor List `[Opus]`

Implement MDL to safely copy user buffers into kernel space for IRP READ/WRITE operations. Required before any user-mode read/write path can pass buffers through the IRP.

**Files:** `src/kernel/io/mdl.c` (new), `include/kernel/io/mdl.h` (new)

> [!NOTE]
> MDL layout: `void *StartVa` (user virtual address); `ULONG ByteCount`; `ULONG ByteOffset` (offset within first page); `ULONG MdlFlags`; `void *MappedSystemVa` (kernel mapping of pages). `MmProbeAndLockPages`: validate user VA range is mapped and readable/writable (check page table entries); record physical pages. `MmGetSystemAddressForMdl`: map the physical pages into the kernel's address space (use `vmm_map_pages()` at a kernel VA); return `MappedSystemVa`. For the initial implementation in a single-address-space kernel, this can be a direct pointer -- but the structure and API must be in place for future ring-3 isolation.

- [ ] `struct MDL { void *StartVa; uint32_t ByteCount; uint32_t ByteOffset; uint32_t Flags; void *MappedSystemVa; uint64_t Pages[MDL_MAX_PAGES]; uint32_t PageCount; }`
- [ ] `MmCreateMdl(vaddr, length)` → allocate MDL; set `StartVa`, `ByteCount`, `ByteOffset = vaddr & 0xFFF`; return pointer
- [ ] `MmProbeAndLockPages(mdl, read_write)`: walk virtual address range; populate `mdl->Pages[]` with physical page addresses via `vmm_virt_to_phys()`; set `MDL_PAGES_LOCKED`; if any page is not present: set `IoStatus.Status = STATUS_ACCESS_VIOLATION`
- [ ] `MmGetSystemAddressForMdl(mdl)`: if `MappedSystemVa` already set: return it; else map pages at a kernel VA via `vmm_alloc_kernel_va()`; set `MDL_MAPPED_TO_SYSTEM_VA`; return `MappedSystemVa`
- [ ] `MmUnlockPages(mdl)`: release page locks; clear `MDL_PAGES_LOCKED`
- [ ] `MmFreeMdl(mdl)`: if `MDL_MAPPED_TO_SYSTEM_VA`: unmap kernel VA; `kfree(mdl)`
- [ ] Commit: `"io: MDL -- MmCreateMdl/ProbeAndLock/GetSystemAddress/Unlock/Free"`

## 3. File Handle Table `[Opus]`

Add a per-process `HANDLE`→`FILE_OBJECT*` table to the process struct. Implement `NtCreateFile` (path → `FILE_OBJECT`, allocate handle), `NtClose`, and `ObReferenceObjectByHandle`.

**Files:** `src/kernel/io/file_object.c` (new), `include/kernel/io/file_object.h` (new), `src/kernel/sched/task.c` (extend), `src/kernel/sched/syscall.c` (extend)

> [!NOTE]
> `FILE_OBJECT` fields: `UNICODE_STRING FileName`; `LARGE_INTEGER CurrentByteOffset`; `ACCESS_MASK DesiredAccess`; `ULONG ShareAccess`; `ULONG Flags` (delete-on-close, overlapped, etc.); `uint32_t ReferenceCount`; `vfs_node_t *VfsNode`; `void *FsContext` (filesystem-private). Handle table: `task_t.handle_table[256]` (static array for now); slot 0 reserved; slots 1–255 for file/event/other objects. `HANDLE` is an index into this table. `ObReferenceObjectByHandle(handle, access, &obj)`: look up in current process's table; check access mask; increment refcount; set `*obj`.

- [ ] `FILE_OBJECT` struct definition; `file_object_create(vfs_node, access, share, flags)` → kmalloc + init; `file_object_release(fo)` → decrement refcount, `vfs_close` + `kfree` on zero
- [ ] Per-process handle table: add `void *handle_table[256]; uint32_t handle_count` to `task_t`; `handle_alloc(task, obj)` → find first NULL slot, store `obj`, return index+1 as HANDLE; `handle_free(task, handle)` → zero slot
- [ ] `ObReferenceObjectByHandle(HANDLE, access_mask, &obj_ptr)`: get current task; bounds-check handle; fetch slot; check access; `++refcount`; set `*obj_ptr`; return `STATUS_SUCCESS` or `STATUS_INVALID_HANDLE`
- [ ] `NtCreateFile(handle_out, access, obj_attrs, io_status, disposition, options, share)`: parse `ObjectName` from `obj_attrs`; call `vfs_open(path, flags)` → `vfs_node_t`; if share/delete-on-close conflict → `STATUS_SHARING_VIOLATION`; create `FILE_OBJECT`; alloc handle; write to `*handle_out`; set `IoStatusBlock`
- [ ] `NtClose(handle)`: `ObReferenceObjectByHandle` to get `FILE_OBJECT`; call `file_object_release()`; `handle_free()`
- [ ] Inherit handles: `handle_duplicate(src_task, dst_task, handle)` for `OBJ_INHERIT` during `CreateProcess`
- [ ] Register `NtCreateFile`/`NtClose` in `syscall.c` dispatch table
- [ ] Commit: `"io: file handle table -- FILE_OBJECT, per-process HANDLE table, NtCreateFile, NtClose, ObRefByHandle"`

## 4. `CreateFile` / `CloseHandle` `[Sonnet]`

Implement Win32 `CreateFile`/`CreateFileW` as wrappers over `NtCreateFile`. Handle all five creation dispositions, common flags, and access masks. Implement `CloseHandle` over `NtClose`.

**Files:** `src/kernel/win32/createfile.c` (new), `include/kernel/win32/file.h` (new)

> [!NOTE]
> Creation disposition mapping: `CREATE_NEW (1)` → `FILE_CREATE`; `CREATE_ALWAYS (2)` → `FILE_SUPERSEDE`; `OPEN_EXISTING (3)` → `FILE_OPEN`; `OPEN_ALWAYS (4)` → `FILE_OPEN_IF`; `TRUNCATE_EXISTING (5)` → `FILE_OVERWRITE`. Flag translations: `FILE_FLAG_OVERLAPPED (0x40000000)` → `FILE_OBJECT.Flags |= FO_OVERLAPPED`; `FILE_FLAG_DELETE_ON_CLOSE (0x04000000)` → `vfs_open` with `VFS_DELETE_ON_CLOSE`; `FILE_FLAG_WRITE_THROUGH (0x80000000)` → `BLKDEV_FLAG_WRITE_THROUGH`; `FILE_FLAG_NO_BUFFERING (0x20000000)` → bypass block cache. `dwShareMode` maps directly to `NtCreateFile` `ShareAccess`.

- [ ] `CreateFileW(lpFileName, dwAccess, dwShare, lpSecAttr, dwDisposition, dwFlags, hTemplate)` → convert `dwDisposition` to NT disposition; build `OBJECT_ATTRIBUTES`; call `NtCreateFile(...)`; on `STATUS_SUCCESS` return handle; on error: `SetLastError(RtlNtStatusToDosError(status))`; return `INVALID_HANDLE_VALUE`
- [ ] `CreateFileA(lpFileName, ...)` → convert ASCII to `UNICODE_STRING`; call `CreateFileW`
- [ ] `CloseHandle(hObject)` → `NtClose(hObject)`; return TRUE/FALSE; `SetLastError` on failure
- [ ] Verify path prefix: strip `\\?\` and `\\.\` prefixes if present; map drive letter to VFS root
- [ ] Commit: `"win32: CreateFile/CloseHandle -- all dispositions, FILE_FLAG_*, share mode, NtCreateFile wrapper"`

## 5. `ReadFile` / `WriteFile` / `SetFilePointerEx` `[Opus]`

Implement `NtReadFile`/`NtWriteFile` via synchronous IRP dispatch. Add `SetFilePointerEx` to update `FILE_OBJECT.CurrentByteOffset`. Wire overlapped I/O IOCP completion packet path.

**Files:** `src/kernel/win32/readwrite.c` (new), `src/kernel/sched/syscall.c` (extend)

> [!NOTE]
> Synchronous read: `NtReadFile(handle, event, apc, apc_ctx, io_status, buffer, length, byte_offset, key)`: `ObReferenceObjectByHandle` → `FILE_OBJECT`; allocate IRP (`IRP_MJ_READ`); if `byte_offset` given use it, else use `fo->CurrentByteOffset`; `MmCreateMdl(buffer, length)`; `MmProbeAndLockPages(mdl, READ)`; `IoCallDriver()`; if synchronous: wait for completion; update `fo->CurrentByteOffset += info`; return status. Overlapped: if `fo->Flags & FO_OVERLAPPED`: do NOT wait; queue completion packet to IOCP/event (§9).

- [ ] `NtReadFile(handle, event, apc, apc_ctx, io_status, buf, len, byte_offset, key)`: reference handle; allocate+fill IRP; MDL for buffer; call `IoCallDriver`; synchronous → `KeWaitForSingleObject(completion_event)`; update offset; free MDL + IRP; set `io_status`
- [ ] `NtWriteFile(...)`: same pattern for `IRP_MJ_WRITE`; MDL probe for write; update offset on completion
- [ ] `SetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod)`: reference handle; compute new offset from `FILE_BEGIN`/`CURRENT`/`END`; validate ≥ 0; set `fo->CurrentByteOffset`; write result to `lpNewFilePointer`
- [ ] `ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped)` → `NtReadFile`; set `*lpNumberOfBytesRead = io_status.Information`; return TRUE/FALSE
- [ ] `WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped)` → `NtWriteFile`
- [ ] Register `NtReadFile`/`NtWriteFile`/`NtSetInformationFile` in syscall dispatch; retire `SYS_READFILE` primitive
- [ ] Closing `SYS_READFILE` must close its hazards: it hands the ring-3 `buf` to `vfs_read` for kernel-mode writing, narrows a 64-bit length to `uint32_t`, and reports partial reads as success. -> XREF: `02-kernel-core/TODO-25 §7`
- [ ] Route the replacement `NtReadFile` path through `task_acct_note_read_io()` so retiring the primitive also closes its accounting gap. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7`
- [ ] Publish each completion's (ops, bytes) pair coherently so a job membership snapshot cannot split one request. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7` (item: "I/O split by read/write/control")
- [ ] Commit: `"win32: ReadFile/WriteFile/SetFilePointerEx -- IRP dispatch, MDL, offset tracking, sync path"`

## 6. Directory APIs + Per-Process CWD `[Sonnet]`

Implement `FindFirstFileW`/`FindNextFileW`/`FindClose`, `CreateDirectoryW`/`RemoveDirectoryW`, and per-process `GetCurrentDirectoryW`/`SetCurrentDirectoryW`.

**Files:** `src/kernel/win32/findfile.c` (new), `src/kernel/win32/directory.c` (new), `src/kernel/sched/task.c` (extend)

> [!NOTE]
> `FindFirstFileW` opens the parent directory, issues `IRP_MJ_DIRECTORY_CONTROL` (sub-function `IRP_MN_QUERY_DIRECTORY`) with a wildcard mask, fills a `WIN32_FIND_DATAW` struct, and returns a find handle (stored in process handle table as a `FIND_OBJECT`). `FindNextFileW` continues from the saved directory position. `FIND_OBJECT` fields: `HANDLE dir_handle`; `uint32_t entry_index`; `UNICODE_STRING mask`. `RemoveDirectoryW` requires the directory to be empty -- if not, return `ERROR_DIR_NOT_EMPTY`.

- [ ] `FindFirstFileW(lpFileName, lpFindFileData)`: split path into directory + mask; `CreateFileW(directory, LIST_DIR)`; allocate `FIND_OBJECT`; call `IRP_MJ_DIRECTORY_CONTROL` for first entry matching mask; fill `WIN32_FIND_DATAW`; alloc handle; return handle
- [ ] `FindNextFileW(hFindFile, lpFindFileData)`: `ObReferenceObjectByHandle` → `FIND_OBJECT`; continue directory query from saved index; fill data; return TRUE or FALSE + `ERROR_NO_MORE_FILES`
- [ ] `FindClose(hFindFile)`: close directory handle; free `FIND_OBJECT`; `handle_free()`
- [ ] `WIN32_FIND_DATAW`: `dwFileAttributes`, `ftCreationTime`, `ftLastWriteTime`, `nFileSizeHigh`/`Low`, `cFileName[260]` (WCHAR), `cAlternateFileName[14]` (8.3)
- [ ] `CreateDirectoryW(lpPathName, lpSecAttr)` → `vfs_mkdir(path)`; return TRUE/FALSE
- [ ] `RemoveDirectoryW(lpPathName)` → `vfs_rmdir(path)`; if empty check fails return `ERROR_DIR_NOT_EMPTY`
- [ ] Per-process CWD wrappers over the shipped kernel syscalls: `GetCurrentDirectoryW` → `NtQueryCurrentDirectory`; `SetCurrentDirectoryW` → `NtSetCurrentDirectory` (→ XREF: `02-kernel-core/TODO-21-process-model-extensions.md §1`)
- [ ] `SetCurrentDirectoryW` must also sync `PEB->ProcessParameters->CurrentDirectoryDosPath` (RtlSetCurrentDirectory_U): kernel keeps `task->cwd` authoritative and does NOT write the user PEB on set (→ XREF: TODO-21 §1 F2)
- [ ] **VFS dir-enum cursor** (`NtQueryDirectoryFile`/FindNextFileW): O(1) `FILE_OBJECT` cursor + metadata in `vfs_dirent`, replacing O(n^2) `vfs_readdir`+`vfs_finddir`; serialize vs concurrent shared-handle enum (`dir_enum_index` raced).
- [ ] **Retire the `NtQueryDirectoryFile` fh==0 C:\ shim** (`nt_syscall.c`): once `CreateFile(LIST_DIR)` yields a real dir handle + the ABI test opens `C:\`, require a valid handle (fh==0 -> `STATUS_INVALID_HANDLE`, parity).
- [ ] **Per-handle granted-access enum gate** (`nt_syscall.c`): gate dir/read/write on the handle's `granted_access` (`ObReferenceObjectByHandle`) not object-wide `FILE_OBJECT.access`; a read-stripped duplicate handle still passes.
- [ ] **`disposition_to_vfs` FILE_LIST_DIRECTORY mapping** (`nt_syscall.c`): map `FILE_LIST_DIRECTORY`/`FILE_READ_DATA` to `VFS_O_READ` independent of `GENERIC_WRITE`, so a list+write dir open is not stored write-only and wrongly denied.
- [ ] **`NtQueryDirectoryFile` IOSB on early exits** (`nt_syscall.c`): probe the `IO_STATUS_BLOCK` first and set it on the null/zero-buffer and probe-failure returns too, so a reused IOSB never shows a stale completion.
- [ ] Commit: `"win32: FindFirstFile/FindNextFile/FindClose, CreateDirectory/RemoveDirectory, per-process CWD"`

## 7. File Management -- Delete, Move, Copy `[Sonnet]`

Implement `DeleteFileW`, `MoveFileExW` (within-volume rename and cross-volume copy+delete), and `CopyFileW` (chunked read/write + timestamp preserve).

**Files:** `src/kernel/win32/fileops.c` (new)

> [!NOTE]
> `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`: if same volume (same drive letter): call `vfs_rename(src, dst)` directly (in-filesystem atomic rename). Cross-volume: copy then delete (use `CopyFileW` logic then `DeleteFileW`). `CopyFileW` chunk size: 64 KiB per read/write loop. After copy: call `SetFileTime(dst, creation, access, write)` using source file times (preserve timestamps).

- [ ] `DeleteFileW(lpFileName)` → `CreateFileW(path, DELETE, 0, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE)`; `CloseHandle()` triggers the deferred delete; return TRUE/FALSE
- [ ] `MoveFileExW(lpExisting, lpNew, dwFlags)`: parse drive letters; if same letter: `vfs_rename(existing_path, new_path)`; if different letters: `CopyFileW(existing, new, !REPLACE)`; then `DeleteFileW(existing)`
- [ ] `CopyFileW(lpExisting, lpNew, bFailIfExists)`: open src `OPEN_EXISTING`; open dst `CREATE_ALWAYS` (or `CREATE_NEW` if `bFailIfExists`); allocate 64 KiB heap buffer; loop `ReadFile`+`WriteFile` until EOF; `GetFileTime(src, ...)` → `SetFileTime(dst, ...)`; close both handles; return TRUE/FALSE
- [ ] `MoveFileW(lpExisting, lpNew)` → `MoveFileExW(lpExisting, lpNew, 0)`
- [ ] Commit: `"win32: DeleteFileW, MoveFileExW (rename + cross-volume copy+delete), CopyFileW"`

## 8. File Metadata APIs `[Sonnet]`

Implement `GetFileAttributesW`/`SetFileAttributesW`, `GetFileSizeEx`, `GetFileTime`/`SetFileTime`. Encode/decode `FILETIME` (100 ns ticks since 1601-01-01 UTC).

**Files:** `src/kernel/win32/filemeta.c` (new), `include/kernel/win32/filemeta.h` (new)

> [!NOTE]
> `FILETIME` = 100-nanosecond intervals since 1601-01-01 00:00:00 UTC, stored as a 64-bit little-endian value. Conversion from FAT32 time: `fat32_filetime_to_kernel()` built in TODO-04 §5. Conversion from NTFS time: NTFS uses the same 100 ns tick epoch as `FILETIME`. `GetFileSizeEx`: call `vfs_stat()` for inode size; write to `LARGE_INTEGER`. `GetFileInformationByHandle`: fill `BY_HANDLE_FILE_INFORMATION` (attributes, creation/access/write times, volume serial, file index high/low, nLinks).

- [ ] `GetFileAttributesW(lpFileName)` → `CreateFile(OPEN_EXISTING, READ_ATTRIBUTES)` + `NtQueryInformationFile(FileBasicInformation)` → return `dwFileAttributes`; `INVALID_FILE_ATTRIBUTES` on error
- [ ] `SetFileAttributesW(lpFileName, dwAttr)` → open + `NtSetInformationFile(FileBasicInformation, {.FileAttributes = dwAttr})`
- [ ] `GetFileSizeEx(hFile, lpFileSize)` → `NtQueryInformationFile(FileStandardInformation)` → `AllocationSize`/`EndOfFile`; set `lpFileSize->QuadPart = EndOfFile`
- [ ] `GetFileTime(hFile, lpCreation, lpLastAccess, lpLastWrite)` → `NtQueryInformationFile(FileBasicInformation)` → convert NTFS/FAT32 times to `FILETIME` fields
- [ ] `SetFileTime(hFile, lpCreation, lpLastAccess, lpLastWrite)` → `NtSetInformationFile(FileBasicInformation)` with encoded times
- [ ] `GetFileInformationByHandle(hFile, lpFileInfo)` → assemble `BY_HANDLE_FILE_INFORMATION` from `vfs_stat()` + volume serial from Registry
- [ ] Commit: `"win32: file metadata -- GetFileAttributes, GetFileSizeEx, GetFileTime/SetFileTime, FILETIME encode"`

## 9. Async I/O + APC `[Opus]`

Implement `OVERLAPPED` struct, `GetOverlappedResult`, and user-mode APC queued to the calling thread on IRP completion for overlapped file handles.

**Files:** `src/kernel/win32/async_io.c` (new), `include/kernel/win32/overlapped.h` (new)

> [!NOTE]
> `OVERLAPPED`: `ULONG_PTR Internal` (NTSTATUS); `ULONG_PTR InternalHigh` (bytes transferred); `DWORD Offset`; `DWORD OffsetHigh`; `HANDLE hEvent` (signalled on completion). Overlapped IRP completion: when `IoCompleteRequest()` fires and `fo->Flags & FO_OVERLAPPED`, set `overlapped->Internal = status`, `overlapped->InternalHigh = info`, then `SetEvent(overlapped->hEvent)` if provided, then queue a user-mode APC to the initiating thread if an APC function was supplied.

- [ ] `OVERLAPPED` struct definition; `overlapped->Internal = STATUS_PENDING` at submit time
- [ ] IRP completion with overlapped: in `IoCompleteRequest()`, if `irp->FileObject->Flags & FO_OVERLAPPED && irp->Overlapped`: fill `Internal`/`InternalHigh`; if `hEvent` provided: `NtSetEvent(hEvent)`; if APC routine provided: `KeInsertQueueApc(thread, apc_routine, apc_ctx, status)`
- [ ] `GetOverlappedResult(hFile, lpOverlapped, lpBytesTransferred, bWait)`: check `Internal != STATUS_PENDING`; if still pending and `bWait`: `WaitForSingleObject(hEvent, INFINITE)`; set `*lpBytesTransferred = InternalHigh`; return TRUE/FALSE
- [ ] `KeInsertQueueApc(thread, apc, ctx, status)`: add to thread's APC queue; when thread enters alertable wait (`WaitForSingleObjectEx(INFINITE, bAlertable=TRUE)`): dequeue and call `apc(ctx, 0, status)`
- [ ] `GetOverlappedResultEx(hFile, lpOverlapped, lpBytes, dwMs, bAlertable)`: waits up to `dwMs` ms; if `bAlertable` drains APC queue
- [ ] Commit: `"win32: OVERLAPPED async I/O -- IRP overlapped completion, hEvent signal, user-mode APC queue"`

## 10. Filter Manager Stub `[Sonnet]`

Register and unregister mini-filters via `FltRegisterFilter`/`FltUnregisterFilter`. Wire pre/post operation callbacks at IRP dispatch. Pass-through by default.

**Files:** `src/kernel/io/fltmgr.c` (new), `include/kernel/io/fltmgr.h` (new)

> [!NOTE]
> `FLT_REGISTRATION` fields: `Size`, `Version`, `Flags`, `ContextRegistration`, `OperationRegistration[]` (each entry: `MajorFunction`, `Flags`, `PreOperation`, `PostOperation`), `FilterUnloadCallback`. A mini-filter's `PreOperation` returns `FLT_PREOP_SUCCESS_WITH_CALLBACK` (continue), `FLT_PREOP_COMPLETE` (short-circuit), or `FLT_PREOP_PENDING` (async). IRP dispatch in `IoCallDriver()` must call each registered filter's pre-op in ascending altitude order, and post-ops in reverse.

- [ ] `FLT_FILTER` struct: `uint32_t altitude`; `FLT_OPERATION_REGISTRATION *ops`; `PFLT_FILTER_UNLOAD_CALLBACK unload`; linked into `g_filter_list` sorted by altitude
- [ ] `FltRegisterFilter(DriverObject, Registration, &FilterHandle)`: allocate `FLT_FILTER`; insert into altitude-sorted list; return handle
- [ ] `FltUnregisterFilter(FilterHandle)`: remove from list; call `unload` callback
- [ ] `FltStartFiltering(FilterHandle)`: no-op initially (pass-through); mark filter as active
- [ ] Pre-op dispatch in `IoCallDriver`: before calling FS driver, iterate `g_filter_list` ascending; call `PreOperation(cbd, CompletionContext)`; if returns `FLT_PREOP_COMPLETE`: skip FS driver, skip post-ops
- [ ] Post-op dispatch: after FS driver returns, iterate list descending; call `PostOperation`
- [ ] Commit: `"io: FltMgr stub -- FltRegisterFilter/Unregister/StartFiltering, pre/post op dispatch hooks"`

## 11. Internal Migration -- `vfs_open` → `CreateFile` `[Sonnet]`

Migrate all ~15 kernel and desktop source files that currently call `vfs_open`/`vfs_read`/`vfs_write`/`vfs_close` directly to use `CreateFile`/`ReadFile`/`WriteFile`/`CloseHandle`.

**Files:** `src/kernel/gfx/gfx_text.c`, `src/kernel/panic.c`, `src/kernel/klog_disk.c`, `src/kernel/registry.c`, `src/kernel/image.c`, `src/kernel/mm/swap.c`, `src/kernel/mm/mmap.c`, `src/kernel/symtab.c`, `src/kernel/ico.c`, `src/kernel/icon_store.c`, `src/kernel/image_save.c`, `src/kernel/drivers/cursor.c`, `src/kernel/sched/syscall.c`, `src/desktop/desktop.c`, `src/desktop/terminal.c`

> [!NOTE]
> Migration pattern: `vfs_open(path, VFS_O_READ)` → `CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)`; `vfs_read(node, buf, size)` → `ReadFile(handle, buf, size, &read, NULL)`; `vfs_write(node, buf, size)` → `WriteFile(...)`; `vfs_close(node)` → `CloseHandle(handle)`. Kernel-internal modules (like `klog_disk.c`, `registry.c`) may keep using `vfs_*` directly if they run before the handle table is initialised -- document these with a `/* kernel-internal: pre-handle-table path */` comment. Do NOT migrate `src/kernel/fs/` itself -- the VFS layer stays on `vfs_*`.
> → XREF: `D03 T04 §1,§4` -- `src/kernel/mm/swap.c` and `src/kernel/mm/mmap.c` are pager-owned files. Keep the migration backend narrow so reclaim, pagefile, and mapped-file fault code do not depend on scattered raw `vfs_*` calls.

- [ ] For each file in the list: search for `vfs_open`/`vfs_read`/`vfs_write`/`vfs_close` calls; replace with Win32 equivalents; verify compile; test read/write correctness
- [ ] `syscall.c`: retire `SYS_READFILE` / `SYS_READDIR`; replace with `NtCreateFile` / `NtReadFile` / `NtWriteFile` / `NtClose` syscall numbers
- [ ] Files that run pre-handle-table (`klog_disk.c`, `registry.c`, `symtab.c`): annotate as kernel-internal; keep `vfs_*`; document in header
- [ ] Commit: `"kernel: migrate vfs_open→CreateFile in gfx, panic, image, mm, desktop, syscalls"`

## 12. File Change Notifications `[Sonnet]`

Implement `FindFirstChangeNotificationW`/`FindNextChangeNotification`/`FindCloseChangeNotification` and `ReadDirectoryChangesW`. Post change events from VFS inode mutation paths.

**Files:** `src/kernel/win32/notify.c` (new), `include/kernel/win32/notify.h` (new), `src/kernel/fs/vfs.c` (extend)

> [!NOTE]
> `FindFirstChangeNotification` filter flags: `FILE_NOTIFY_CHANGE_FILE_NAME (0x1)`, `FILE_NOTIFY_CHANGE_DIR_NAME (0x2)`, `FILE_NOTIFY_CHANGE_SIZE (0x8)`, `FILE_NOTIFY_CHANGE_LAST_WRITE (0x10)`. The notification object is a manual-reset event handle signalled when a matching change occurs in the watched directory (or subtree if `bWatchSubtree`). `ReadDirectoryChangesW` returns a buffer of `FILE_NOTIFY_INFORMATION` records. VFS integration: `vfs_create`, `vfs_delete`, `vfs_rename`, `vfs_write` must call `vfs_notify_change(dir_node, action, filename)` after mutating the directory.

- [ ] `NOTIFY_WATCHER { HANDLE event; char path[256]; DWORD filter; BOOL subtree; FILE_NOTIFY_INFORMATION *buf; DWORD buf_size; }` -- linked list of active watchers
- [ ] `FindFirstChangeNotificationW(lpPath, bSubtree, dwFilter)`: create watcher; create event; return event handle (watchers indexed by path)
- [ ] `FindNextChangeNotification(hChange)`: reset the event (re-arm); return TRUE
- [ ] `FindCloseChangeNotification(hChange)`: free watcher entry
- [ ] `ReadDirectoryChangesW(hDir, lpBuf, nBufLen, bSubtree, dwFilter, lpBytesReturned, lpOverlapped, lpApc)`: store watcher; fill `FILE_NOTIFY_INFORMATION` records on next change or wait synchronously
- [ ] `vfs_notify_change(vfs_node_t *dir, DWORD action, const char *filename)`: iterate watcher list; if path matches: append `FILE_NOTIFY_INFORMATION` record; signal event; if overlapped: complete IRP
- [ ] Hook `vfs_notify_change()` into `vfs_create`, `vfs_delete`, `vfs_rename`, `vfs_write_complete`
- [ ] Commit: `"win32: file change notifications -- FindFirstChangeNotification, ReadDirectoryChangesW, vfs hooks"`

## 13. `GetDiskFreeSpaceEx` / `GetVolumeInformation` `[Sonnet]`

Implement `GetDiskFreeSpaceExW`/`GetDiskFreeSpaceW` and the remaining `GetVolumeInformationW` fields not covered in TODO-03 §4 (free bytes, cluster geometry).

**Files:** `src/kernel/win32/volume_info.c` (new or extend createfile.c)

> [!NOTE]
> `GetDiskFreeSpaceExW(lpDirectory, lpFreeBytesAvailable, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes)`: extract drive letter; call `vfs_get_free_space(letter, &free_clusters, &total_clusters, &bytes_per_cluster)`; compute bytes fields. `GetDiskFreeSpaceW` (legacy): returns sectors-per-cluster, bytes-per-sector, free clusters, total clusters. `GetVolumeInformationW` is partially implemented in TODO-03 §4; this section adds `lpVolumeSerialNumber` (hash of device path), `lpMaximumComponentLength = 255`, and `lpFileSystemFlags` per filesystem type.

- [ ] `vfs_get_free_space(char letter, uint64_t *free_clusters, uint64_t *total_clusters, uint64_t *bytes_per_cluster)`: read Registry `FreeBytes`/`TotalBytes` (populated by probe); also query filesystem live: FAT32 `vol->fsinfo_free_count`, NTFS `$Bitmap` free count, IXFS equivalent
- [ ] `GetDiskFreeSpaceExW(lpDir, lpFreeAvail, lpTotal, lpTotalFree)`: drive letter from `lpDir`; compute from `vfs_get_free_space()`; all three outputs use `ULARGE_INTEGER`
- [ ] `GetDiskFreeSpaceW(lpRoot, &spc, &bps, &free_c, &total_c)`: return cluster geometry from filesystem BPB + free count
- [ ] Verify `GetVolumeInformationW` (TODO-03 §4) includes volume serial (CRC32 of device path), max component = 255, correct `FileSystemFlags` per type; extend here if missing
- [ ] Commit: `"win32: GetDiskFreeSpaceEx/GetDiskFreeSpace -- VFS free-space query, cluster geometry"`

---

## 14. `DeviceIoControl` + FSCTL Dispatch + Oplock Ioctls

Win32 `DeviceIoControl(hDev, dwIoControlCode, ...)` is the single entry point for FSCTL_* / IOCTL_* / METHOD_* operations. Without a routing layer, programs that issue `FSCTL_REQUEST_OPLOCK`, `FSCTL_LOCK_VOLUME`, `FSCTL_DISMOUNT_VOLUME`, `FSCTL_GET_VOLUME_INFORMATION`, etc. get `ERROR_INVALID_FUNCTION` even when the underlying capability exists. This section adds the dispatcher + the oplock ioctls (which §14 of the FAT32+VFS-semantics TODO depends on -- see [`05-storage-filesystems/TODO-04 §14`](TODO-04-fat32-hardening-vfs-semantics.md#14-vfs-opportunistic-locks-oplocks)).

**Files:** `src/kernel/win32/deviceio.c` (new), `src/kernel/win32/fsctl_oplock.c` (new), `include/kernel/win32/fsctl.h` (new)

> [!NOTE]
> `DeviceIoControl` decodes the IOCTL code's `DEVICE_TYPE` (high 16 bits) and routes to the right handler family (FSCTL = `FILE_DEVICE_FILE_SYSTEM`, disk = `FILE_DEVICE_DISK`, etc.). The `METHOD_*` field selects buffering: `METHOD_BUFFERED` copies user buffers via SystemBuffer, `METHOD_NEITHER` leaves them un-probed (driver responsibility). `FSCTL_REQUEST_OPLOCK` (since Windows 7) accepts a `REQUEST_OPLOCK_INPUT_BUFFER` selecting which lease type (Read, Read-Handle, Read-Write, Read-Write-Handle); `FSCTL_OPLOCK_BREAK_ACKNOWLEDGE` re-arms the oplock from a break-pending state. The TODO-04 §14 oplock subsystem already implements `vfs_request_oplock` / `vfs_break_oplock`; this section is the user-mode plumbing layer.

- [ ] `NTSTATUS NtDeviceIoControlFile(HANDLE, ..., ULONG IoControlCode, PVOID InBuf, ULONG InLen, PVOID OutBuf, ULONG OutLen)` -- the SSDT entry point; wired through the existing handle table (TODO-05 §3) to find the underlying object (file/volume/device)
- [ ] FSCTL routing table: `static const struct fsctl_handler s_fsctl[] = { { FSCTL_REQUEST_OPLOCK, ... }, ... }` indexed by IOCTL code; unknown codes return `STATUS_INVALID_DEVICE_REQUEST`
- [ ] `FSCTL_REQUEST_OPLOCK` (0x90240): decode `REQUEST_OPLOCK_INPUT_BUFFER`, map flags to `vfs_request_oplock(node, owner_id, level)` from TODO-04 §14, return `STATUS_PENDING` for async or `STATUS_SUCCESS` on grant
- [ ] `FSCTL_OPLOCK_BREAK_ACKNOWLEDGE` (0x90238): mark the break-pending oplock as acknowledged so subsequent waiters can proceed; pairs with the §14 `vfs_break_oplock` notification path
- [ ] `FSCTL_LOCK_VOLUME` (0x90018) + `FSCTL_UNLOCK_VOLUME` (0x9001C) + `FSCTL_DISMOUNT_VOLUME` (0x90020): exclusive volume access for chkdsk/format; rejects new opens while locked; integrates with TODO-13 §4 chkdsk's exclusive-mount need
- [ ] `FSCTL_GET_VOLUME_INFORMATION` (0x900C4): mirror of the §13 path through the FSCTL surface (some legacy code uses this instead of `GetVolumeInformationW`)
- [ ] Win32 `DeviceIoControl(...)` user-mode wrapper that calls `NtDeviceIoControlFile` with the right buffer layout
- [ ] Unit tests: `test_fsctl_oplock_request_release` (Level 1 grant + acknowledge), `test_fsctl_lock_volume_blocks_new_opens`, `test_fsctl_invalid_returns_invalid_device_request`
- [ ] On each COMPLETED control request call `task_acct_note_control_io(task_current(), bytes)` so the control-I/O counters and `IO_COUNTERS.Other*` stop reading zero. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7`
- [ ] Commit: `"win32: NtDeviceIoControlFile + FSCTL dispatch + oplock + volume-lock ioctls"`

**Test checkpoint:** A user-mode test program calls `DeviceIoControl(hFile, FSCTL_REQUEST_OPLOCK, ...)` with a Level 1 lease request; receives `STATUS_SUCCESS` on initial grant. A second open of the same file triggers a break notification consumable via overlapped completion. `FSCTL_LOCK_VOLUME` blocks a subsequent `CreateFile` on the same volume with `ERROR_ACCESS_DENIED`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

---

## OS Comparison


| ⭐  | Feature                                                                | 🪟 Win11                                                                       | 🐧 Linux                                                      | 🚀 Impossible OS                                                                |
| --- | ---------------------------------------------------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| 💎  | IRP engine                                                             | ✅ `ntoskrnl.exe`; full IRP + stack                                            | ❌ VFS `file_operations` vtable; no IRP                       | ⬜ §1 -- `IoAllocateIrp/Complete/CallDriver`, 7 major functions, VFS            |
| 💎  | MDL -- safe user↔kernel buffer mapping                                 | ✅ `ntoskrnl.exe`; full MDL with physical                                      | ❌ `copy_to/from_user()`; no MDL                              | ⬜ §2 -- `MmCreateMdl/ProbeAndLock/GetSystemAddressForMdl`                      |
| 💎  | Per-process `HANDLE` table                                             | ✅ `ntoskrnl.exe`; `HANDLE` table in `EPROCESS`;                               | ❌ fd table (integer index, not                               | ⬜ §3 -- `handle_table[256]` in `task_t`, `FILE_OBJECT`, `NtCreateFile/NtClose` |
| 💎  | `CreateFile`/`CloseHandle`                                             | ✅ Full Win32; `CREATE_NEW/ALWAYS/OPEN_EXISTING/OPEN_ALWAYS/TRUNCATE_EXISTING` | ❌ `open()`/`close()` POSIX; no create-disposition concept    | ⬜ §4 full Win32 dispositions, `FILE_FLAG_OVERLAPPED/DELETE_ON_CLOSE`           |
| 💎  | `ReadFile`/`WriteFile`/`SetFilePointerEx`                              | ✅ Win32 sync + async; IRP                                                     | ❌ `read()`/`write()`/`lseek()` POSIX; `io_uring` for async   | ⬜ §5 -- IRP-based sync read/write, `CurrentByteOffset`, overlapped             |
| 💎  | `FindFirstFileW`/`FindNextFileW` + `CreateDirectory` + per-process CWD | ✅ Full Win32 directory API; per-process                                       | ❌ `opendir()`/`readdir()`/`mkdir()` POSIX                    | ⬜ §6 -- `FIND_OBJECT`, `WIN32_FIND_DATAW`, `cwd` in `task_t`                   |
| 💎  | `DeleteFileW`, `MoveFileExW`, `CopyFileW`                              | ✅ Full Win32 file management; atomic                                          | ❌ `unlink()`/`rename()`/no `CopyFile` syscall; shell `cp`    | ⬜ §7 -- `vfs_rename` for same-volume, copy+delete for                          |
| 💎  | File metadata                                                          | ✅ `GetFileAttributesW`, `GetFileTime`, etc.; NTFS `$STANDARD_INFORMATION`     | ❌ `stat()`/`utimes()`/`chmod()` POSIX; no `FILETIME`         | ⬜ §8 -- `NtQueryInformationFile`, `FILETIME` encode, full attribute            |
| 💎  | Async I/O + `OVERLAPPED` + user-mode APC                               | ✅ Win32 `OVERLAPPED`; IOCP; `ReadFileEx` APC;                                 | ❌ `aio_read()`/`io_uring`; POSIX signal-based completion; no | ⬜ §9 -- `OVERLAPPED`, `hEvent` signal, APC queue                               |
| 💎  | Filter manager                                                         | ✅ `fltmgr.sys`; full mini-filter model; antivirus/backup                      | ❌ `inotify`/`fanotify`; no filter manager abstraction        | ⬜ §10 -- `FltRegisterFilter`, altitude-sorted list, pre/post op                |
| ⭐  | Internal migration                                                     | ✅ Always was Win32-based (no migration                                        | ❌ Not applicable (no Win32)                                  | ⬜ §11 -- clean cut: kernel-internal stays `vfs_*`                              |
| 💎  | File change notifications                                              | ✅ `ReadDirectoryChangesW`; `IRPStack` `IRP_MJ_DIRECTORY_CONTROL`              | ❌ `inotify`; different model (`IN_CREATE`/`IN_DELETE`/etc.)  | ⬜ §12 -- `vfs_notify_change()` hooked into VFS mutations,                      |
| 💎  | `GetDiskFreeSpaceEx` / `GetVolumeInformation`                          | ✅ Full Win32; backed by filesystem                                            | ❌ `statvfs()` POSIX                                          | ⬜ §13 -- VFS free-space query, cluster geometry,                               |

> **After §1–13:** Impossible OS presents an unmodified Win32 file I/O surface to user-mode programs: a binary compiled against `kernel32.dll` `CreateFile`/`ReadFile`/`WriteFile` will work without modification. The `⭐` architectural insight is the clean layering in §11: kernel-internal code (klog, registry, mmap) retains lightweight `vfs_*` calls with zero handle overhead, while every user-visible API path uses the full IRP+MDL+handle Win32 model. This mirrors Windows' own layered architecture -- `ntoskrnl` internal paths bypass the object manager for performance, while all user-mode-facing paths go through `FILE_OBJECT` + `HANDLE`.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] IRP: `IoAllocateIrp(IRP_MJ_READ)` → `IoCallDriver()` → `vfs_irp_dispatch` returns `STATUS_SUCCESS`; IRP freed; no leak
- [ ] MDL: `MmCreateMdl(user_buf, 4096)` → `MmProbeAndLockPages` → `MmGetSystemAddressForMdl` returns non-NULL; `MmUnlockPages` + `MmFreeMdl` complete without crash
- [ ] `CreateFile("C:\\hello.txt", GENERIC_READ, ...)` → valid HANDLE (not `INVALID_HANDLE_VALUE`); `CloseHandle` → handle slot freed
- [ ] `ReadFile(handle, buf, len, &read, NULL)` → `read == len`; content matches file on disk; `SetFilePointerEx(SEEK_SET, 0)` → `ReadFile` again gets same content
- [ ] `FindFirstFileW("C:\\*.*", &data)` → returns valid handle; `FindNextFileW` iterates all entries; `FindClose` succeeds
- [ ] `CreateDirectoryW("C:\\TestDir")` → directory visible via `FindFirstFileW`; `RemoveDirectoryW` → gone
- [ ] `CopyFileW(src, dst)` → dst exists, content identical; timestamps match source
- [ ] `GetFileAttributesW("C:\\Impossible\\System32\\cmd.exe")` → `FILE_ATTRIBUTE_ARCHIVE` (non-INVALID)
- [ ] `GetFileSizeEx(hFile, &size)` → `size.QuadPart` matches actual file size
- [ ] Async: `ReadFile(overlapped_handle, buf, len, NULL, &ov)` → returns immediately; `GetOverlappedResult(..., TRUE)` → correct byte count; APC fires on alertable wait
- [ ] Filter manager: `FltRegisterFilter` → filter in list; IRP pre-op called before VFS; post-op called after; `FltUnregisterFilter` removes it
- [ ] `ReadDirectoryChangesW("C:\\TestDir", ...)` → create file in dir → notification fires with `FILE_ACTION_ADDED`
- [ ] `GetDiskFreeSpaceExW("C:\\", &avail, &total, &totalFree)` → non-zero values; `total >= avail`
- [ ] Migration: `gfx_text.c` opens fonts via `CreateFile`; `registry.c` still uses `vfs_open` (annotated); build passes without `vfs_open` in migrated files
- [ ] Commit: `"win32: complete file I/O API -- IRP, MDL, handles, CreateFile/ReadFile/WriteFile, dirs, async, notifications"`
