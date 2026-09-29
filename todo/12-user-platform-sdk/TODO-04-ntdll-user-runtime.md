---
schema_version: 1
id: ntdll-user-runtime
domain: 12-user-platform-sdk
status: active
title: "TODO-04 -- NTDLL & User-Mode Runtime"
---

# TODO-04 -- NTDLL & User-Mode Runtime

> **Goal:** Build `ntdll.dll` -- the user-mode runtime sitting between every Win32 program
> and the kernel. Provides the process heap (`RtlHeap`), PE DLL loader (`LdrLoadDll`), TLS,
> vectored exception handling, process CRT startup, and the fiber API. The stubs already
> placed in `D10T08 §4` become real implementations here; `HeapAlloc`/`LoadLibrary` in
> `kernel32.dll` forward to these Rtl/Ldr functions.

> [!IMPORTANT]
> **Prerequisites complete before starting:**
> - `10-platform-services/TODO-07 §7` -- `pe_exec()` with minimal TEB (`stack_base/limit/self`,
>   `FS_BASE` MSR) and minimal PEB (`ImageBaseAddress`, `ProcessParameters.CommandLine`).
>   This TODO **extends** TEB with `TlsSlots[64]`, `ExceptionList`, and `Tib.FiberData`; and
>   extends PEB with `ProcessHeap`, `Ldr` (module list), `TlsBitmap`. (The VEH/VCH list heads are
>   ntdll process-global state, NOT a TEB field -- see §5.)
> - `10-platform-services/TODO-08 §4` -- `ntdll.dll` minimal stubs
>   (`RtlInitUnicodeString`, `NtCurrentTeb`, `RtlGetVersion`, `NtAllocateVirtualMemory`).
>   Stubs are replaced/completed here without breaking their export table entries.
> - `10-platform-services/TODO-08 §5` -- `VirtualAlloc`/`VirtualFree` already wired;
>   `HeapAlloc`/`HeapFree`/`GetProcessHeap` call through to `RtlAllocateHeap` (§2 here).
> - `10-platform-services/TODO-08 §7` -- `LoadLibrary`/`GetProcAddress`/`FreeLibrary`
>   already wired; they forward to `LdrLoadDll`/`LdrGetProcedureAddress` (§3 here).
>
> **`ntdll.dll` is always mapped at `0x7FF00000000`** (fixed VA, set in PE optional header
> `ImageBase`). It has **no import dependencies** -- it is the lowest user-mode layer.
> All Nt* functions issue raw `SYSCALL`; all Rtl*/Ldr* functions are pure user-mode logic.
>
> **VEH (§5) interacts with SEH frames** -- after exhausting the VEH list, dispatch falls back to
> table-based x64 SEH (`RtlLookupFunctionEntry` + `RtlVirtualUnwind` per frame), NOT the legacy
> x86-32 `TEB.ExceptionList` chain (→ XREF `02-kernel-core/TODO-23 §6`, `§8`).

---

## Inputs

- `include/kernel/sched/task.h` -- `struct task`, `TEB`, `PEB` fields; extend in this TODO
- `include/win32/types.h` -- `HANDLE`, `DWORD`, `BOOL`, `NTSTATUS` -- `D10T08 §1`
- `include/pe.h` -- `IMAGE_TLS_DIRECTORY`, `IMAGE_DATA_DIRECTORY` -- `TODO-07 §3`
- `include/kernel/mm/vmm.h` -- `vmm_alloc_user()`, `VirtualAlloc`/`VirtualFree` -- `D10T08 §5`
- `include/kernel/syscall.h` -- syscall numbers for `SYS_VIRTUALALLOC`, `SYS_VIRTUALFREE`
- `08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md` (XREF) -- `NtGdi*` / `NtUser*` syscall stub indices in `ntdll_syscalls.asm` must match Table 1; router contract in `08-graphics-ui/TODO-16-win32k-shadow-native-api.md`
- `src/win32/ntdll.c` -- existing minimal stubs from `D10T08 §4` (extend, do not duplicate)
- `10-platform-services/TODO-07-win32-pe-loader.md §9` (→ XREF) -- TEB/PEB minimal setup
- `10-platform-services/TODO-08-win32-api-surface.md §2 §3 §7` (→ XREF) -- stubs + VirtualAlloc + LoadLibrary forwards
- `02-kernel-core/TODO-23-exception-dispatch-seh.md §8` (→ XREF) -- SEH frame walk; VEH fallback
- `user/lib/crt0_pe.asm` -- existing from `TODO-07 §9`; extend for static initializers (§7)

---

## Outcome

Every Win32 process has a fully-functional user-mode runtime on startup: a process heap
(`GetProcessHeap()` returns a live allocator), DLLs load recursively with `LdrLoadDll`,
TLS indexes work, exceptions route through VEH handlers before the SEH fallback, and
`main`/`WinMain` is called after all static C++ constructors run. Fiber-based coroutines
are available via `CreateFiber`/`SwitchToFiber`.

---

## Implementation Order

| Step | Section                                                   | 💎/⭐ | Dependency                                                            |
| ---- | --------------------------------------------------------- | ----- | --------------------------------------------------------------------- |
| 1    | ntdll.dll structure + TEB/PEB extensions                  | 💎    | `TODO-07 §7`; `D10T08 §4` stubs                                       |
| 2    | RtlHeap process heap allocator                            | 💎    | §1; `VirtualAlloc` (`D10T08 §5`)                                      |
| 3    | LdrLoadDll PE DLL loader                                  | 💎    | §1; `TODO-07 §4 §5 §7`; `D10T08 §7`                                   |
| 4    | Thread-local storage (TLS + PE TLS callbacks)             | 💎    | §1 TEB extensions; `TODO-07 §3` TLS data dir                          |
| 5    | Vectored exception handling (VEH)                         | 💎    | §1 TEB VEH chain; `D10T08 §5` VirtualAlloc; `D10T10 §3` SEH fallback  |
| 6    | Process startup (CRT0)                                    | 💎    | §2 heap init; §3 Ldr init; §4 TLS init; §5 VEH                        |
| 7    | User-mode libc shims (`user/lib/libc.lib`)                | 💎    | §6 CRT0; `kernel32.dll` stubs                                         |
| 8    | Fiber API                                                 | ⭐    | §1 TEB FiberData; §6 thread-to-fiber conversion                       |
| 9    | Local atom tables (RtlAtomTable + kernel32 AddAtom)       | 💎    | §2 RtlHeap; `D02T13 §3` (kernel global `NtAddAtom` stays global-only) |
| 10   | WNF user runtime (RtlPublish/Subscribe + dispatch worker) | 💎    | §1 TEB; §2 RtlHeap; `D02 T16 §3 §8` (kernel KNF/WNF SSDT surface)     |

---

## 1. ntdll.dll Structure + TEB / PEB Extensions `[Sonnet]`

**Source layout:** `src/user/ntdll/` split into:
- `ntdll_syscalls.asm` -- thin `SYSCALL` wrappers for all `Nt*` functions
- `ntdll_heap.c` -- `RtlHeap` (§2)
- `ntdll_loader.c` -- `LdrLoadDll` (§3)
- `ntdll_tls.c` -- TLS (§4)
- `ntdll_except.c` -- VEH (§5)
- `ntdll_sync.c` -- critical sections (`InitializeCriticalSection`, `Enter/LeaveCriticalSection`)
- `ntdll_crt.c` -- process startup helpers (§6)
- `ntdll_fiber.c` -- fiber API (§8)

**Header:** `include/win32/ntdll.h`

- [ ] **Extend `TEB` struct** in `include/kernel/sched/task.h` (or `include/win32/teb.h`):
  ```c
  typedef struct _TEB {
      /* NT_TIB (must be at offset 0) */
      void     *ExceptionList;        /* +0x000  SEH chain head */
      void     *StackBase;            /* +0x008 */
      void     *StackLimit;           /* +0x010 */
      void     *SubSystemTib;         /* +0x018 */
      void     *FiberData;            /* +0x020  NULL if thread; FIBER* if fiber */
      void     *ArbitraryUserPointer; /* +0x028 */
      void     *Self;                 /* +0x030  GS:[0x30] → TEB* self-pointer */
      /* PEB pointer */
      void     *Peb;                  /* +0x060  GS:[0x60] */
      /* Last error */
      uint32_t  LastErrorValue;       /* +0x068  GetLastError() */
      uint32_t  _pad0;
      /* TLS */
      void     *TlsSlots[64];         /* +0x1480  TlsSetValue/GetValue */
      /* NOTE: no VehListHead here -- the VEH/VCH lists are ntdll PROCESS-GLOBAL
       * state (see §5), not per-thread TEB fields. */
  } TEB;
  ```
- [ ] **Extend `PEB` struct** in `include/win32/peb.h`:
  ```c
  typedef struct _PEB {
      uint8_t   InheritedAddressSpace;    /* +0x000 */
      uint8_t   ReadImageFileExecOptions; /* +0x001 */
      uint8_t   BeingDebugged;            /* +0x002 */
      uint8_t   _pad0;
      uint32_t  _pad1;
      void     *Mutant;                   /* +0x008 */
      void     *ImageBaseAddress;         /* +0x010 */
      void     *Ldr;                      /* +0x018  PEB_LDR_DATA* */
      void     *ProcessParameters;        /* +0x020  RTL_USER_PROCESS_PARAMETERS* */
      void     *ProcessHeap;             /* +0x030  RtlHeap handle */
      void     *TlsBitmap;               /* +0x070  RTL_BITMAP for TLS index alloc */
      uint32_t  TlsBitmapBits[2];        /* +0x074  64-bit bitmap (TLS index 0–63) */
  } PEB;

  typedef struct _PEB_LDR_DATA {
      uint32_t  Length;
      uint8_t   Initialized;
      void     *SsHandle;
      LIST_ENTRY InLoadOrderModuleList;
      LIST_ENTRY InMemoryOrderModuleList;
      LIST_ENTRY InInitializationOrderModuleList;
  } PEB_LDR_DATA;

  typedef struct _LDR_DATA_TABLE_ENTRY {
      LIST_ENTRY InLoadOrderLinks;
      LIST_ENTRY InMemoryOrderLinks;
      void      *DllBase;
      void      *EntryPoint;        /* DllMain address */
      uint32_t   SizeOfImage;
      UNICODE_STRING FullDllName;
      UNICODE_STRING BaseDllName;
  } LDR_DATA_TABLE_ENTRY;
  ```
- [ ] **`pe_exec()` extension** (in `TODO-07 §7` code): allocate full TEB; populate `TlsSlots[]` all-NULL; allocate `PEB_LDR_DATA`; set `PEB.ProcessHeap` after `RtlCreateHeap` (§2) from CRT0 (§7). VEH/VCH heads init in `VehInit()` (§5), not here
- [ ] **Fixed VA mapping**: `ntdll.dll` PE optional header `ImageBase = 0x7FF000000000`; `pe_resolve_imports()` maps it there; `GS:[0x30]` → TEB self-pointer already set by `TODO-07 §7`
- [ ] **`ntdll_syscalls.asm`**: one stub per Nt* function:
  ```nasm
  global NtWriteFile
  NtWriteFile:
      mov rax, SYS_WRITEFILE
      syscall
      ret
  ```
- [ ] **Kernel-callback user side** (ntdll dep of TODO-12 §26): `KernelCallbackTable` at PEB +0x058 + a `KiUserCallbackDispatcher` in `ntdll_except.c` (index by ApiNumber, call the callback, return via `NtCallbackReturn` 0x0300).

---

## 2. RtlHeap Process Heap Allocator `[Opus]`

> Novel user-mode heap: free-list + best-fit with coalescing. No prior Impossible OS
> user-mode allocator exists; `kmalloc` is ring-0 only.

**Source file:** `src/user/ntdll/ntdll_heap.c`

- [ ] **`HEAP_HANDLE RtlCreateHeap(flags, base, reserve_size, commit_size, lock, params)`**:
  - If `base == NULL`: `VirtualAlloc(NULL, reserve_size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)`
  - Initialize `struct heap_header` at base: magic `0x50414548` ("HEAP"), total size, free list head, lock (§ `ntdll_sync.c` critical section)
  - Insert one free block covering the entire committed range (minus header)
  - `PEB.ProcessHeap` = return value (set by CRT0 §6 before `main`)
- [ ] **`void *RtlAllocateHeap(heap, flags, size)`**:
  - Round `size` up to 8-byte alignment; add `sizeof(block_header_t)`
  - Walk free list (best-fit: smallest block that fits); if found: split block if remainder ≥ `MIN_BLOCK_SIZE` (32 bytes); unlink from free list; mark `HEAP_BLOCK_USED`
  - If no block fits: `VirtualAlloc` another page (round up to PAGE_SIZE); insert as free block; retry
  - If `HEAP_ZERO_MEMORY` flag: `memset(ptr, 0, size)` before returning
  - Return `ptr` (past block header); on failure: return `NULL` + `SetLastError(ERROR_NOT_ENOUGH_MEMORY)`
- [ ] **`BOOL RtlFreeHeap(heap, flags, ptr)`**:
  - Validate `ptr` in heap range; recover `block_header_t` from `ptr - sizeof(block_header_t)`
  - Mark block free; coalesce with adjacent free blocks (both forward and backward links)
  - Return TRUE; `SetLastError(ERROR_INVALID_PARAMETER)` + return FALSE if `ptr == NULL` or bad header magic
- [ ] **`SIZE_T RtlSizeHeap(heap, flags, ptr)`**: return `block_header_t.user_size` for block at `ptr`
- [ ] **`HEAP_HANDLE RtlDestroyHeap(heap)`**: `VirtualFree(heap_base, 0, MEM_RELEASE)`; do not call for default process heap
- [ ] **`GetProcessHeap()`**: `NtCurrentTeb()->Peb->ProcessHeap`
- [ ] **`HeapAlloc(heap, flags, size)`** / **`HeapFree`** / **`HeapCreate`** / **`HeapDestroy`** in `kernel32.dll`: all forward directly to Rtl equivalents (one-line wrappers)

---

## 3. LdrLoadDll (PE DLL Loader) `[Opus]`

> Novel: ntdll-side DLL loading pipeline. No prior Impossible OS in-ntdll loader. Manages
> the PEB loaded-modules list and drives recursive import resolution.

**Source file:** `src/user/ntdll/ntdll_loader.c`

- [ ] **DLL search order**: `LdrLoadDll` searches:
  1. Process executable directory
  2. `C:\Impossible\System32\`
  3. `C:\Impossible\Bin\`
  4. Each dir in `PATH` env var
- [ ] **`NTSTATUS LdrLoadDll(path, flags, basename, HMODULE *out)`**:
  1. Scan `PEB.Ldr->InLoadOrderModuleList` by `BaseDllName` (case-insensitive); if found: increment `LDR_DATA_TABLE_ENTRY.LoadCount`; `*out = DllBase`; return `STATUS_SUCCESS`
  2. Resolve full path via search order above; `NtOpenFile` + `NtReadFile` into `VirtualAlloc` buffer
  3. Call `pe_load(data, size, &image_base)` (existing); `pe_apply_relocations()`; `pe_resolve_imports()` -- `pe_resolve_imports` calls back into `LdrLoadDll` recursively for each import DLL
  4. Allocate `LDR_DATA_TABLE_ENTRY`; populate `DllBase`, `EntryPoint`, `SizeOfImage`, `BaseDllName`, `FullDllName`; insert at head of all three `PEB.Ldr` lists (`InLoadOrder`, `InMemoryOrder`, `InInitializationOrder`)
  5. If PE has `DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS]`: call `LdrpHandleTlsData(entry)` (§4)
  6. Call `DllMain(hmod, DLL_PROCESS_ATTACH, NULL)` if `EntryPoint != NULL`
  7. `*out = DllBase`; return `STATUS_SUCCESS`
- [ ] **`NTSTATUS LdrGetProcedureAddress(hmod, name, ordinal, addr_out)`**:
  - Walk PE export directory (`IMAGE_DIRECTORY_ENTRY_EXPORT`):
    - If `name != NULL`: binary search `AddressOfNames[]` (names are sorted); get ordinal via `AddressOfNameOrdinals[]`; get address via `AddressOfFunctions[]`
    - If `ordinal != 0`: use ordinal directly (bias by `Base` field)
  - Return address; `STATUS_PROCEDURE_NOT_FOUND` on miss
- [ ] **`NTSTATUS LdrUnloadDll(hmod)`**:
  - Find `LDR_DATA_TABLE_ENTRY` by `DllBase`; decrement `LoadCount`
  - If `LoadCount == 0`: call `DllMain(hmod, DLL_PROCESS_DETACH, NULL)`; unlink from all 3 PEB lists; `VirtualFree(DllBase, 0, MEM_RELEASE)`; free entry
- [ ] **`LoadLibraryA/W`** → `LdrLoadDll`; **`GetProcAddress`** → `LdrGetProcedureAddress`; **`FreeLibrary`** → `LdrUnloadDll` (one-line wrappers in `kernel32.dll`)
- [ ] **Built-in DLL stubs** (kernel32/user32/gdi32/msvcrt): pre-populate PEB LDR list with entries pointing to their stub export tables; `LdrLoadDll` short-circuits to `STATUS_SUCCESS` before doing file I/O

---

## 4. Thread-Local Storage (TLS) `[Sonnet]`

**Source file:** `src/user/ntdll/ntdll_tls.c`

- [ ] **TLS index allocation**:
  - `TEB.Peb->TlsBitmap` + `TlsBitmapBits[2]` (64-bit bitmap, one bit per slot 0–63)
  - `DWORD TlsAlloc()`: find first zero bit in `TlsBitmapBits`; set it; return index; if all 64 used: `SetLastError(ERROR_NO_MORE_ITEMS)`; return `TLS_OUT_OF_INDEXES`
  - `BOOL TlsFree(index)`: clear bit in bitmap; zero `TEB.TlsSlots[index]` for all threads (current process only for MVP)
- [ ] **TLS value access** (inline-able via `GS` register):
  - `BOOL TlsSetValue(index, value)`: `TEB.TlsSlots[index] = value` via `GS`-relative write; return TRUE
  - `PVOID TlsGetValue(index)`: return `TEB.TlsSlots[index]` via `GS`-relative read; clear `LastError` to 0 on success
- [ ] **PE TLS callbacks** (`IMAGE_TLS_DIRECTORY` at `DataDirectory[9]`):
  - `LdrpHandleTlsData(LDR_DATA_TABLE_ENTRY *entry)`: read `IMAGE_TLS_DIRECTORY` from PE; for each `AddressOfCallBacks[]` entry (NULL-terminated): call `callback(DllBase, DLL_PROCESS_ATTACH, NULL)`
  - Called by `LdrLoadDll` (§3) and on thread create/exit (thread-create path: TEB init sets `TlsSlots[]` to `TlsDirectory.AddressOfIndex` values)
- [ ] **`FlsAlloc(callback)`** / **`FlsSetValue`** / **`FlsGetValue`** / **`FlsFree`**: fiber-local storage; uses same `TlsSlots` mechanism; `callback` called at fiber/thread exit with final value (store callback in a per-index table `g_fls_callbacks[64]`)
- [ ] **C++ thread-local** (`__thread` / `thread_local`): LLVM emits PE TLS sections for `thread_local` variables; the `LdrpHandleTlsData` path covers this automatically

---

## 5. Vectored Exception Handling (VEH) `[Opus]`

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog), [`controls.md#status-colours`](../../docs/design/controls.md#status-colours)

> Novel exception dispatch chain. No prior Impossible OS VEH exists. Kernel delivers
> exceptions via `NtRaiseException`; ntdll dispatches VEH → SEH → VCH (Vectored Continue
> Handlers, walked when dispatch continues execution -- via VEH or a frame handler -- before
> resume) → UnhandledExceptionFilter.

> [!IMPORTANT]
> **Ownership boundary with `02-kernel-core/TODO-23` (binding both ways).** THIS section owns the
> ring-3 half: the VEH/VCH lists, `RtlDispatchException`, `__C_specific_handler`, `RtlRestoreContext`,
> and the top-level filter + crash dialog. TODO-23 owns the ring-0 half: fault capture, debugger
> mediation, trap-frame delivery to `KiUserExceptionDispatcher`, `NtContinue`/`NtRaiseException`
> validation, and terminal termination. The kernel never calls a user handler.
> `SetUnhandledExceptionFilter` state lives HERE as a process-global (Windows uses the kernel32
> global `BasepCurrentTopLevelFilter`, `EncodePointer`-obfuscated) -- it is **not** a PEB field.
> When ring-3 dispatch declines everything, re-enter the kernel via `NtRaiseException(first_chance=FALSE)`
> so TODO-23 §4 can run second-chance and terminate.

**Source file:** `src/user/ntdll/ntdll_except.c`

- [ ] **VEH list**: PROCESS-GLOBAL lock-guarded doubly-linked list of `VECTORED_HANDLER_ENTRY` nodes in ntdll data (a `LdrpVectorHandlerList`-analog) -- NOT a TEB/PEB anchor (per-process semantics). Node ABI pinned by `02-kernel-core/TODO-23 §10`
- [ ] **`PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)`**:
  - Allocate `VECTORED_HANDLER_ENTRY` via `RtlAllocateHeap`
  - If `first != 0`: insert at list head; else append at tail
  - Return opaque handle (pointer to entry)
- [ ] **`ULONG RemoveVectoredExceptionHandler(PVOID handle)`**:
  - Unlink entry from the process-global VEH list (under the ntdll lock); free via `RtlFreeHeap`; return 1 on success, 0 if not found
- [ ] **`KiUserExceptionDispatcher`** (called by kernel on exception, via `NtRaiseException`):
  - Walk the process-global VEH list (lock-safe per the re-entrancy protocol below, NOT one lock held across the callbacks): call each `handler(EXCEPTION_POINTERS *)`:
    - Returns `EXCEPTION_CONTINUE_EXECUTION` → go to the **VCH walk** convergence step, then resume
    - Returns `EXCEPTION_CONTINUE_SEARCH` → continue to next handler
  - If VEH list exhausted: walk SEH via **table-based x64 dispatch** -- `RtlLookupFunctionEntry` + `RtlVirtualUnwind` per frame, invoking each frame's language handler (`__C_specific_handler`). x64 has NO frame-linked chain: do NOT walk `TEB.ExceptionList` (that is the legacy x86-32 mechanism; the kernel sets it to -1 "no SEH" at `task.c:2319`) (→ XREF `02-kernel-core/TODO-23 §6`, `§8`). A frame handler that elects to continue execution → go to the **VCH walk** convergence step, then resume
  - **VCH walk** (convergence for BOTH the VEH-continue and SEH-continue paths, before context restore): walk the VCH list per the VCH items below, then restore context and resume
  - If SEH also exhausted and no handler continued: call `UnhandledExceptionFilter(EXCEPTION_POINTERS *)` (VCH is NOT walked on this terminal path)
    - Default: `MessageBox`-style crash dialog with exception code + module name + offset + stack trace (10 frames via `RtlCaptureStackBackTrace`)
    - Call `ExitProcess(1)` after dialog dismissed
- [ ] **`VOID RaiseException(code, flags, nargs, args)`**: build `EXCEPTION_RECORD`; call `NtRaiseException(record, context, TRUE)` → kernel delivers back via `KiUserExceptionDispatcher`
- [ ] **`RtlCaptureStackBackTrace(skip, count, buffer, hash)`**: walk `RBP` chain; store return addresses; return actual count captured
- [ ] **Re-entrant dispatch + removal safety** (DESIGN OPEN): a handler may call `RemoveVectoredExceptionHandler` or fault into nested dispatch, so one lock across the walk is insufficient; define+test a re-entrancy protocol (see NOTE)
- [ ] **VCH list**: SECOND process-global lock-guarded list of `VECTORED_HANDLER_ENTRY` nodes, separate head from the VEH list; reuses §10 node ABI (no new type); NOT a TEB/PEB anchor. ABI: `TODO-23 §10`; boundary: `§11`
- [ ] **Ring-3 dispatch telemetry**: `KiUserExceptionDispatcher` emits an `exception_dispatch` event per handler (type veh/seh/vch/filter, disposition, unwound frames) → XREF: `02-kernel-core/TODO-23 §16` (schema owner)
- [ ] **`PVOID AddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)`**: allocate node via `RtlAllocateHeap`; `first != 0` head-insert else tail-append on the VCH list; return opaque handle
- [ ] **`ULONG RemoveVectoredContinueHandler(PVOID handle)`**: unlink from the VCH list under the ntdll lock; free via `RtlFreeHeap`; return 1 on success, 0 if not found
- [ ] **VCH walk**: on either continue path (VEH `CONTINUE_EXECUTION` or a frame handler continuing), before context restore, walk the VCH list lock-safe per the shared re-entrancy protocol; validate exact triggers vs a Windows/ReactOS trace
- [ ] **VCH return semantics**: each `handler(EXCEPTION_POINTERS *)` returns a disposition -- `EXCEPTION_CONTINUE_EXECUTION` stops the VCH walk, `EXCEPTION_CONTINUE_SEARCH` proceeds to the next; shares the VEH re-entrancy protocol
- [ ] **VCH ordering + re-entrancy tests**: assert VCH fires on both continue paths (VEH-continue and SEH-continue) and stops on `CONTINUE_EXECUTION`, in registration order, and removal-during-walk is safe under the shared protocol

> [!NOTE]
> **Re-entrancy is unresolved.** A VEH handler is arbitrary code: it may call `RemoveVectoredExceptionHandler` (self or another node) or fault into nested exception dispatch. Holding one lock across the whole walk does NOT make this safe -- a non-recursive lock deadlocks inside recovery, and a recursive one lets a node be freed while an outer walk still references it (use-after-free). The design must cover self-removal, next-handler removal, and nested exceptions via deferred reclamation / dispatch-depth tracking, or per-node lifetime state (Microsoft's ntdll uses a ~0x28 refcounted + `EncodePointer`'d node for exactly this). The VEH/VCH node is currently the minimal `{ LIST_ENTRY List; PVECTORED_EXCEPTION_HANDLER Handler; }` (24 bytes) -- PROVISIONAL: if the protocol needs per-node lifetime state the node grows and both sides update together. Node ABI owner: `02-kernel-core/TODO-23 §10`.

---

## 6. Process Startup (CRT0) `[Sonnet]`

**Source file:** `user/lib/crt0_pe.asm` (extend existing); `src/user/ntdll/ntdll_crt.c`

- [ ] **EXE startup sequence** (extend `_mainCRTStartup` / `_start` in `crt0_pe.asm`):
  1. Receive `PEB *` in `RCX` (Windows PE convention) at process entry point
  2. Call `RtlCreateHeap(0, NULL, 0x100000, 0x10000, NULL, NULL)` → store in `PEB.ProcessHeap`
  3. Call `LdrpInitialize()`: populate `PEB.Ldr`, load all static imports recursively via `LdrLoadDll`
  4. Call `LdrpRunTlsCallbacks(DLL_PROCESS_ATTACH)` for all loaded modules
  5. Call `VehInit()`: zero-initialize the process-global VEH/VCH list heads + their lock (ntdll data, not the TEB)
  6. Walk `.ctors` section (LLVM emits static C++ constructor pointers there): call each in order
  7. Parse `ProcessParameters.CommandLine` → `argc`/`argv` via `crt_parse_cmdline()`
  8. Call `WinMain(hInstance, NULL, cmdline, SW_SHOW)` or `main(argc, argv)`
  9. Call `ExitProcess(return_value)`
- [ ] **DLL startup** (`_DllMainCRTStartup` in `crt0_pe.asm`):
  - On `DLL_PROCESS_ATTACH`: call `.ctors` static initializers; then call `DllMain(hmod, reason, reserved)`
  - On `DLL_PROCESS_DETACH`: call `.dtors` destructors in reverse; then call `DllMain`
- [ ] **Static C++ initializers**: LLVM/Clang emits function pointers in `.ctors` section (NULL-terminated); `crt0_pe.asm` walks from `__CTOR_LIST__` to `NULL` sentinel calling each; same for `.dtors` at exit
- [ ] **`crt_parse_cmdline(wchar_t *cmdline, int *argc_out, char ***argv_out)`**: Win32 command-line tokenization (quote-aware); allocate `argv[]` on heap; `argv[0]` = executable path from `ProcessParameters.ImagePathName`
- [ ] **`ExitProcess(exitcode)`**: call `.dtors` destructors; call `DLL_PROCESS_DETACH` on all loaded DLLs in reverse load order; `NtTerminateProcess(NtCurrentProcess(), exitcode)`
- [ ] **`TerminateProcess(hproc, exitcode)`**: `NtTerminateProcess(hproc, exitcode)` -- skips cleanup; for external kill

---

## 7. User-Mode libc Shims `[Sonnet]`

**Output:** `user/lib/libc.lib` (static; linked by default by `impossible-cc` wrapper)

- [ ] **`printf(fmt, ...)`** → `WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), buf, len, NULL, NULL)` using a stack-local vsnprintf buffer (4096 bytes)
- [ ] **`fprintf(stderr, fmt, ...)`** → `WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), ...)`
- [ ] **`malloc(size)`** → `HeapAlloc(GetProcessHeap(), 0, size)`
- [ ] **`calloc(n, size)`** → `HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n*size)`
- [ ] **`realloc(ptr, size)`** → `HeapReAlloc(GetProcessHeap(), 0, ptr, size)`
- [ ] **`free(ptr)`** → `HeapFree(GetProcessHeap(), 0, ptr)`
- [ ] **`fopen(path, mode)`** → `CreateFileA(path, access_from_mode, 0, NULL, disposition, 0, NULL)`; return `FILE *` = `(FILE *)(uint64_t)handle`
- [ ] **`fread(buf, sz, n, fp)`** → `ReadFile((HANDLE)fp, buf, sz*n, &read, NULL)`; return `read/sz`
- [ ] **`fwrite(buf, sz, n, fp)`** → `WriteFile((HANDLE)fp, buf, sz*n, &written, NULL)`; return `written/sz`
- [ ] **`fclose(fp)`** → `CloseHandle((HANDLE)fp)`
- [ ] **`fseek/ftell`** → `SetFilePointerEx` / `GetFileSizeEx`
- [ ] **`exit(n)`** → `ExitProcess(n)`
- [ ] **`abort()`** → `RaiseException(STATUS_FAIL_FAST_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, NULL)`
- [ ] **`getenv(name)`** → `GetEnvironmentVariableA(name, buf, 256)`; return static buffer or NULL
- [ ] **kernel32 conversion + last-error shims**: `MultiByteToWideChar`/`WideCharToMultiByte`, `SetLastError`/`GetLastError` (TEB +0x68), `LocalAlloc`/`LocalFree` over §2 RtlHeap -- prereq for `02-kernel-core/TODO-22 §6`
- [ ] **`memset/memcpy/strlen/strcmp/strcpy/strcat/sprintf/snprintf`**: link against `src/libs/libc/string.c` (→ XREF `12-user-platform-sdk/TODO-01 §1`) -- do not re-implement
- [ ] **`user/lib/libc.lib`** build rule in Makefile: compile all shims; `llvm-ar-19 rcs libc.lib *.o`

---

## 8. Fiber API `[Opus]`

> Novel user-mode context switching. No prior Impossible OS fiber/coroutine mechanism.
> Requires explicit register save/restore at user level without OS involvement.

**Source file:** `src/user/ntdll/ntdll_fiber.c`

- [ ] **`struct FIBER`** in `include/win32/fiber.h`:
  ```c
  typedef struct {
      void     *rsp;          /* saved RSP when fiber is suspended */
      void     *stack_base;   /* top of allocated stack */
      uint64_t  stack_size;   /* allocation size */
      void     (*fn)(void *); /* fiber function */
      void     *param;        /* parameter passed to fn */
      uint8_t   is_thread;    /* set by ConvertThreadToFiber */
      /* saved registers: RBX, RBP, RDI, RSI, R12-R15, XMM6-XMM15 (Windows ABI non-volatile) */
      uint64_t  rbx, rbp, rdi, rsi, r12, r13, r14, r15;
  } FIBER;
  ```
- [ ] **`PVOID CreateFiber(stack_size, fn, param)`**:
  - `VirtualAlloc(NULL, stack_size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)`
  - Allocate `FIBER` struct via `RtlAllocateHeap`; set `stack_base`, `fn`, `param`
  - Set up initial stack frame: push `fiber_start_trampoline` address as initial RSP; `FIBER.rsp = stack_top - 8`
  - `fiber_start_trampoline`: calls `fiber->fn(fiber->param)`; then `ExitFiber()` (terminates fiber, switches back to calling fiber)
  - Return `FIBER *` cast to `PVOID`
- [ ] **`VOID SwitchToFiber(fiber)`** (implemented in NASM -- cannot use C calling convention):
  1. Save all non-volatile registers (RBX, RBP, RDI, RSI, R12–R15) + RSP into `TEB.FiberData`'s `FIBER` struct (current fiber)
  2. Restore registers from target `FIBER`
  3. Load target `FIBER.rsp` into RSP
  4. `ret` -- returns to where target fiber was last suspended (or `fiber_start_trampoline` on first call)
  - `TEB.FiberData` tracks the currently running fiber on this thread
- [ ] **`PVOID ConvertThreadToFiber(param)`**: allocate `FIBER` for the current thread; `FIBER.is_thread = 1`; `TEB.FiberData = fiber`; return fiber handle; subsequent `SwitchToFiber` calls return to this fiber
- [ ] **`VOID DeleteFiber(fiber)`**: `VirtualFree(stack_base, 0, MEM_RELEASE)`; `RtlFreeHeap(fiber)`; if `fiber == TEB.FiberData`: call `ExitThread(0)` (current fiber cannot delete itself except via exit)

---

## 9. Local Atom Tables (RtlAtomTable) `[Sonnet]`

> Win32 LOCAL atom tables (`AddAtom`/`FindAtom`/`DeleteAtom`) are USER-MODE, per Windows parity: they live in the process heap and are destroyed at process exit, unlike the kernel-owned GLOBAL atom table (`GlobalAddAtom` -> `NtAddAtom`, kept global-only in the kernel per `D02T13 §3`). This section owns the user-mode local-table implementation the kernel deliberately does not host. **Source file:** `src/user/ntdll/ntdll_atom.c`.

- [ ] **`RtlCreateAtomTable(NumberOfBuckets, AtomTable)`** in `include/win32/ntdll.h`: allocate a per-process `RTL_ATOM_TABLE` from the process heap (`RtlAllocateHeap`) with a hash-bucket index; lazily created on first `AddAtom`.
- [ ] **`RtlAddAtomToAtomTable` / `RtlLookupAtomInAtomTable` / `RtlDeleteAtomFromAtomTable` / `RtlDestroyAtomTable`**: refcounted string-atom interning with the `0xC000`-base integer/string split and case-insensitive invariant fold.
- [ ] **`kernel32` wrappers `ATOM AddAtomW/FindAtomW/DeleteAtom`** over the process-default local table; `GlobalAddAtomW` etc. route to `NtAddAtom` (kernel global table).
- [ ] Destroy the process-default local atom table on process exit (CRT0 teardown / `ExitProcess`), matching Windows local-table lifetime.
- [ ] Commit: `"ntdll: user-mode local atom tables (RtlAtomTable) + kernel32 Add/Find/DeleteAtom"`

**Test checkpoint:** A local atom added in one process is not visible in another (separate `RTL_ATOM_TABLE`); a global atom (`GlobalAddAtom`) is visible across processes. Case-insensitive `FindAtom("Foo")` locates an atom added as `"foo"`. The local table is freed at process exit with no leak.

---

## 10. WNF User Runtime (RtlPublish / RtlSubscribe) `[Sonnet]`

> The user-mode Rtl surface over the kernel notification facility (WNF-compatible SSDT surface owned by `D02 T16 §8`). Windows keeps the WNF `Rtl*` layer in `ntdll`: it owns the per-process subscription table + the single delivery worker that turns a kernel wake into user callback dispatch, while the kernel owns state storage, coalescing, and the syscall boundary. This section owns ONLY the user-mode half; it must not duplicate kernel state. **Source file:** `src/user/ntdll/ntdll_wnf.c`. -> XREF: D02 T16 §3 (waitable user subscriptions), D02 T16 §8 (WNF-compatible SSDT surface).

- [ ] **`RtlPublishWnfStateData`**: thin wrapper over `NtUpdateWnfStateData`; validates `Length` against the state's max size and returns the syscall status unchanged.
- [ ] **`RtlSubscribeWnfStateChangeNotification`**: allocate a per-process subscription node from `RtlHeap`, register it in the process subscription table, and arm the kernel wait via `NtSubscribeWnfStateChange`.
- [ ] **`RtlUnsubscribeWnfStateChangeNotification`**: detach from the process table, call `NtUnsubscribeWnfStateChange`, and retire-then-free the node (free only once the dispatch worker is not inside its callback -- no UAF on a concurrent publish).
- [ ] **Single delivery worker**: one per-process thread waits on the kernel wake, queries the payload via `NtQueryWnfStateData`, and fans out to each matching subscription's callback; callbacks serialized per Windows WNF semantics.
- [ ] **`RtlQueryWnfStateData` / `RtlWnfDllUnloadCallback`**: synchronous one-shot read; teardown hook that unsubscribes every live node at DLL unload / `ExitProcess` so no kernel subscription outlives the process.
- [ ] Process-exit cleanup: CRT0 teardown (§6) drains the subscription table so a crashing/exiting process leaves no dangling kernel subscription (mirrors kernel-side teardown-on-exit in `D02 T16 §3`).
- [ ] Commit: `"ntdll: WNF user runtime (RtlPublish/Subscribe + single dispatch worker + process-exit cleanup)"`

**Test checkpoint:** A process subscribes to a state, another publishes, and the subscriber's callback fires once with the new `ChangeStamp` + payload. `RtlUnsubscribe` during a concurrent publish does not use-after-free (retire-then-free). Process exit with a live subscription leaves no kernel subscription (verified via the KNF diagnostics counter, `D02 T16 §9`). Test on: QEMU WHPX + TCG.

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                                     | 🐧 Linux                                                    | 🚀 Impossible OS                                                     |
| --- | ---------------------------------------- | ------------------------------------------------------------ | ----------------------------------------------------------- | -------------------------------------------------------------------- |
| 💎  | User-mode heap                           | ✅ `ntdll!RtlAllocateHeap`; LFH + segment heap               | ✅ glibc `malloc` (ptmalloc)                                | ⬜ §2 -- free-list best-fit with coalescing; `HEAP_ZERO_MEMORY`      |
| 💎  | PE DLL loader in user-mode ntdll         | ✅ `ntdll!LdrLoadDll`; full PEB LDR chain                    | ❌ Not applicable (ELF native)                              | ⬜ §3 -- full PEB LDR list; recursive                                |
| 💎  | Thread-local storage                     | ✅ Full TLS + `__declspec(thread)`                           | ✅ `pthread_key_create` + `__thread`                        | ⬜ §4 -- `TEB.TlsSlots[64]`, `TlsBitmapBits`, PE TLS callbacks,      |
| 💎  | Vectored Exception Handling              | ✅ `AddVectoredExceptionHandler`; KiUserExceptionDispatcher  | ✅ POSIX signals (`sigaction`)                              | ⬜ §5 -- VEH list → SEH fallback                                     |
| 💎  | Process CRT startup                      | ✅ `ntdll!LdrpInitialize`; `.ctors`/`atexit`; Win32 entry    | ✅ glibc `__libc_start_main`                                | ⬜ §6 -- heap+Ldr+TLS init → `.ctors` walk                           |
| 💎  | User-mode libc shims                     | ✅ `msvcrt.dll` / `ucrt.dll`                                 | ✅ glibc                                                    | ⬜ §7 -- `libc.lib` thin wrappers over Win32                         |
| ⭐  | Fiber API                                | ✅ Windows fibers                                            | ⚠️ `makecontext`/`swapcontext` (POSIX; deprecated in glibc) | ⬜ §8 -- `SwitchToFiber` NASM context switch; `ConvertThreadToFiber` |
| 💎  | WNF user runtime (Rtl publish/subscribe) | ✅ `ntdll!RtlSubscribeWnfStateChangeNotification` + dispatch | ✅ inotify/`sd-bus`/kdbus signals (different model)         | ⬜ §10 -- Rtl wrappers over kernel KNF; per-process table + 1 worker |

Impossible OS `ntdll.dll` maps at a fixed VA (`0x7FF000000000`) with **zero import dependencies**
and issues raw `SYSCALL` instructions for all `Nt*` functions -- identical to Windows NT's design.
The heap and Ldr are built on `VirtualAlloc` (no kernel-heap backing), giving the same
performance profile as Windows. The `⭐` edge: the fiber API will serve as the foundation
for a future async I/O runtime (completion-port + fiber-yield model) with no POSIX
compatibility burden.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **TEB/PEB**: `NtCurrentTeb()` returns non-null; `TEB.Self == TEB*`; `TEB.Peb->ImageBaseAddress` matches PE `ImageBase`; `GS:[0x30]` reads TEB self-pointer correctly
- [ ] **RtlHeap**: `HeapAlloc(GetProcessHeap(), 0, 128)` → non-null pointer, 8-byte aligned; `HeapFree` → no crash; `HeapAlloc(heap, HEAP_ZERO_MEMORY, 64)` → zeroed bytes; allocate + free 1000× without leak
- [ ] **LdrLoadDll**: `LoadLibraryA("kernel32.dll")` → returns non-null `HMODULE`; `GetProcAddress(hmod, "CreateFileA")` → non-null address; second `LoadLibraryA` call → same `HMODULE` (cached); `FreeLibrary` → `DLL_PROCESS_DETACH` logged
- [ ] **TLS**: `TlsAlloc()` returns 0; `TlsSetValue(0, (PVOID)42)`; `TlsGetValue(0) == 42`; `TlsFree(0)` → bit cleared; 64 indexes allocate without error; 65th → `TLS_OUT_OF_INDEXES`
- [ ] **VEH**: `AddVectoredExceptionHandler(1, myHandler)`; `RaiseException(0xE0000001, 0, 0, NULL)` → `myHandler` called with correct `ExceptionCode`; return `EXCEPTION_CONTINUE_SEARCH` → crash dialog shown; `RemoveVectoredExceptionHandler` → handler no longer called
- [ ] **CRT0**: `hello.exe` with `int main(int argc, char **argv) { printf("Hello\n"); }` compiles and runs; `argc >= 1`; `argv[0]` = executable path; static constructor (file-scope `struct Foo { Foo() { klog(...); } }`) runs before `main`
- [ ] **libc shims**: `printf("test %d\n", 42)` prints to console; `malloc(100)` → non-null; `free` → no crash; `fopen`/`fwrite`/`fclose` writes file to `C:\Temp\test.txt`; `fopen` on missing file → NULL
- [ ] **Fiber**: `CreateFiber(65536, fn, param)` → non-null; `ConvertThreadToFiber(NULL)` → returns current thread as fiber; `SwitchToFiber(fiber)` → `fn` executes; `SwitchToFiber(original)` → returns to caller; `DeleteFiber` → no crash
- [ ] **WNF runtime**: A subscribes, B `RtlPublish` → A's callback fires once with the new `ChangeStamp` + payload; unsubscribe during a concurrent publish → no crash; process exit with a live subscription → no leaked kernel subscription
- [ ] Commit: `"win32: ntdll RtlHeap, LdrLoadDll, TLS, VEH, CRT0, libc shims, fiber API"`
