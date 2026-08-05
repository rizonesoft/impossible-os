---
schema_version: 1
id: win32-api-surface
domain: 10-platform-services
status: active
title: "TODO-08 -- Win32 API Surface Completion"
---

# TODO-08 -- Win32 API Surface Completion

**Domain:** `10-platform-services`
**Goal:** Complete the Win32 API surface so unmodified Win32 programs compiled with MinGW run on Impossible OS -- console apps, GUI apps, process/memory/sync APIs, DLL loading, the IxUI toolkit, and the developer SDK.

> [!IMPORTANT]
> **Depends on:** `TODO-07 §1–9` -- ring-3 execution, `SYSCALL`/`SYSRET`, and the PE loader must be working before this TODO begins.
> **Continues from:** `todo-old/510-Long-Term-Stretch/TODO-510-Native-Win32.md` §5–9 (migrated and expanded).
> [!NOTE]
> **DLL export master tables (authoritative rows):** [`TODO-A-user32-export-master-table.md`](TODO-A-user32-export-master-table.md), [`TODO-B-comctl32-export-master-table.md`](TODO-B-comctl32-export-master-table.md), [`TODO-C-shell32-export-master-table.md`](TODO-C-shell32-export-master-table.md). Sections 10 through 12 below describe **files, wiring, and acceptance**; do **not** duplicate full export inventories here (maintain rows in TODO-A/B/C).


---

## Important Notes

- Registry API (`RegOpenKeyExA/W`, `RegQueryValueExA/W`, `RegSetValueExA/W`, `RegCloseKey`, `RegCreateKeyExA/W`, `RegEnumKeyExA/W`) is **already implemented** via the native Registry API -- expose as pass-throughs only.
- Kernel already has `mutex_t` (`include/kernel/sched/mutex.h`), `semaphore_t` (`include/kernel/sched/semaphore.h`), `event_t` (`include/kernel/sched/event.h`) -- Win32 sync primitives wrap these directly.
- `wm_create_window()` and `wm_destroy_window()` exist in `include/desktop/wm.h`; `CTRL_BUTTON`, `CTRL_LABEL`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR` exist in `include/desktop/controls.h`. Win32 window/control classes map to these.
- `kmalloc`/`kfree` in `include/kernel/mm/heap.h` are the backing store for `HeapAlloc`/`HeapFree`. User-mode `VirtualAlloc` maps to `vmm_alloc_user()` (defined in `TODO-07`).
- File I/O exports (`CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle`, etc.) are **native** Impossible OS API -- `kernel32.dll` simply re-exports the same function pointers.
- `GetLastError`/`SetLastError` store the error code in `TEB.LastErrorValue` (per-thread, set up in `TODO-07 §9`).
- `CTRL_LISTVIEW`, `CTRL_TREEVIEW`, `CTRL_TABSTRIP`, `CTRL_CHECKBOX`, `CTRL_RADIO` widgets are defined in `TODO-04` (07-graphics-ui domain); this TODO may reference but not implement them.
- The IxUI toolkit (`sdk/include/ixui.h`) is the **native** Impossible OS GUI framework -- it wraps `wm_create_window()` and the compositor directly, not a Win32 emulation layer.

---

## Inputs

| Path                                     | Purpose                                  |
| ---------------------------------------- | ---------------------------------------- |
| `include/kernel/sched/mutex.h`           | `mutex_t`, `mutex_init/lock/unlock`      |
| `include/kernel/sched/semaphore.h`       | `semaphore_t`, `sem_init/wait/signal`    |
| `include/kernel/sched/event.h`           | `event_t`, `event_init/set/reset/wait`   |
| `include/kernel/mm/heap.h`               | `kmalloc`, `kfree`                       |
| `include/desktop/wm.h`                   | `wm_create_window`, `wm_destroy_window`  |
| `include/desktop/controls.h`             | `CTRL_*` widget types                    |
| `include/kernel/sched/task.h`            | `task_t`, `handle_table[]`, TEB base     |
| `include/kernel/sched/abi.h`             | Win32 `SYS_*` constants (`TODO-07 §2`)   |
| `include/pe.h`                           | PE32+ structs, export directory types (`TODO-07 §3`) |
| `src/win32/pe_loader.c`                  | `pe_exec()`, `pe_load()` (`TODO-07 §4–7`) |
| `todo-old/510-Long-Term-Stretch/TODO-510-Native-Win32.md` | §5–9 migration source                    |
| → XREF: `TODO-07 §1–9`                   | Ring-3, SYSCALL ABI, PE loader, TEB/PEB, `exec_load()` |
| → XREF: `08-graphics-ui/TODO-04`         | `CTRL_LISTVIEW`, `CTRL_TREEVIEW`, `CTRL_TABSTRIP` |

---

## Outcome

- Unmodified MinGW-compiled Win32 console apps (`WriteConsoleA`, `ExitProcess`) run.
- Process, memory, sync, and DLL-loading APIs (`CreateProcess`, `VirtualAlloc`, `CreateMutex`, `LoadLibrary`, `GetProcAddress`) function correctly.
- `user32.dll` and `gdi32.dll` route to the native WM/GFX stack.
- IxUI toolkit (`ixui.h`) provides a native Win32-compatible GUI API for native apps.
- SDK (`windows.h`, `impossible.h`, `kernel32.lib`, `impossible-cc`) lets developers cross-compile PE apps.
- Unimplemented functions log to serial and return safe stubs instead of crashing.

---

## Implementation Order

| #   | Section                                  | Tag        | Dep            | Mark |
| --- | ---------------------------------------- | ---------- | -------------- | ---- |
| 1   | Win32 Type Definitions                   | `[Sonnet]` | TODO-07 §2     | 💎   |
| 2   | Console & Process API (`kernel32.dll` tier 1) | `[Sonnet]` | §1             | 💎   |
| 3   | C Runtime (`msvcrt.dll`)                 | `[Sonnet]` | §1             | 💎   |
| 4   | NT Runtime stubs (`ntdll.dll`)           | `[Sonnet]` | §1             | 💎   |
| 5   | Memory Management API                    | `[Sonnet]` | §1, TODO-07 §9 | 💎   |
| 6   | Synchronization API                      | `[Sonnet]` | §1             | 💎   |
| 7   | DLL Loading API (`LoadLibrary`/`GetProcAddress`) | `[Opus]`   | §2, TODO-07 §7 | 💎   |
| 8   | Error API (`GetLastError`, `FormatMessage`, debug output) | `[Sonnet]` | §1             | 💎   |
| 9   | Unimplemented Function Logger            | `[Sonnet]` | §2–8           | ⭐   |
| 10  | Window Management (`user32.dll`)         | `[Sonnet]` | §1, TODO-07 §9 | 💎   |
| 11  | GDI Rendering (`gdi32.dll`)              | `[Sonnet]` | §10            | 💎   |
| 12  | Shell & Icon API (`shell32.dll`)         | `[Sonnet]` | §10            | 💎   |
| 13  | IxUI Native Toolkit (`sdk/include/ixui.h`) | `[Opus]`   | §10–11         | ⭐   |
| 14  | Win32 Shell Integration (`ShellExecute`, `SHGetFolderPath`) | `[Sonnet]` | §12            | 💎   |
| 15  | Developer SDK (`windows.h`, `impossible-cc`) | `[Sonnet]` | §1–14          | ⭐   |

---

## 1. Win32 Type Definitions `[Sonnet]`

Create `include/win32/types.h` (included by `include/win32.h`). All Windows types map to Impossible OS primitives -- no stdlib required.

- [ ] Create `include/win32/types.h`:
  - `HANDLE = void*`, `DWORD = uint32_t`, `BOOL = int`, `WORD = uint16_t`, `BYTE = uint8_t`
  - `LPVOID = void*`, `LPCVOID = const void*`, `LPCSTR = const char*`, `LPSTR = char*`
  - `LPCWSTR = const uint16_t*`, `LPWSTR = uint16_t*`
  - `SIZE_T = size_t`, `UINT = unsigned int`, `LONG = int32_t`, `ULONG = uint32_t`
  - `LPARAM = int64_t`, `WPARAM = uint64_t`, `LRESULT = int64_t`
  - `HMODULE = void*`, `HINSTANCE = void*`, `HICON = void*`, `HCURSOR = void*`
  - `HWND = int` (WM window handle), `HDC = void*`, `HBRUSH = void*`, `HFONT = void*`
  - `INVALID_HANDLE_VALUE = (HANDLE)(uintptr_t)-1`, `TRUE = 1`, `FALSE = 0`
  - `STD_INPUT_HANDLE = -10`, `STD_OUTPUT_HANDLE = -11`, `STD_ERROR_HANDLE = -12`
  - `WINAPI`, `CALLBACK`, `APIENTRY` → all `__attribute__((ms_abi))`
- [ ] Create `include/win32.h` -- umbrella header (`#include "win32/types.h"` + forward-declares all DLL APIs)
- [ ] Commit: `"win32: type definitions and umbrella header"`

---

## 2. Console & Process API (`kernel32.dll` tier 1) `[Sonnet]`

Create `src/win32/kernel32.c`. File I/O functions are **re-exports** of the native API from `TODO-07 §2`.

- [ ] `GetStdHandle(nStdHandle)` → look up `STD_INPUT/OUTPUT/ERROR_HANDLE` in `task->handle_table`; return HANDLE
- [ ] `AllocConsole()` / `FreeConsole()` / `AttachConsole(pid)` → attach/detach terminal emulator session
- [ ] `WriteConsoleA/W(hConsole, buf, len, written, reserved)` → `WriteFile(hConsole, buf, len, written, NULL)`
- [ ] `ReadConsoleA/W(hConsole, buf, len, read, reserved)` → `ReadFile(hConsole, buf, len, read, NULL)`
- [ ] `SetConsoleTitleA/W(title)` → update the terminal emulator window title via WM
- [ ] `SetConsoleTextAttribute(hConsole, attrs)` → map Win32 color attribute bits to ANSI escape codes
- [ ] `GetConsoleWindow()` → return the HWND of the associated console window (or NULL)
- [ ] File I/O re-exports (same function pointers as native kernel): `CreateFileA/W`, `ReadFile`, `WriteFile`, `CloseHandle`, `SetFilePointer`, `GetFileSize`, `FindFirstFileA/W`, `FindNextFileA/W`, `FindClose`, `DeleteFileA`, `CreateDirectoryA`, `RemoveDirectoryA`, `MoveFileA`, `CopyFileA`, `GetFileAttributesA`, `GetCurrentDirectoryA`, `SetCurrentDirectoryA`
- [ ] `ExitProcess(code)` → `SYS_EXIT`
- [ ] `GetCommandLineA/W()` → read from `PEB.CommandLine` (set in `TODO-07 §9`)
- [ ] `GetModuleHandleA/W(name)` → NULL → return current `ImageBase`; named → walk PEB loaded-modules list
- [ ] `GetCurrentProcessId()` → `SYS_GETCURRENTPID`
- [ ] `GetCurrentThreadId()` → current thread ID from scheduler
- [ ] `OpenProcess(access, inherit, pid)` → allocate a kernel handle referencing the target `task_t`
- [ ] `TerminateProcess(hProcess, exitCode)` → send kill signal to target task
- [ ] `GetEnvironmentVariableA/W(name, buf, size)` / `SetEnvironmentVariableA/W(name, value)` → TEB env-block read/write
- [ ] `GetExitCodeProcess(hProcess, lpExitCode)` → read exit code from completed task
- [ ] Registry re-exports (already implemented): `RegOpenKeyExA/W`, `RegQueryValueExA/W`, `RegSetValueExA/W`, `RegCloseKey`, `RegCreateKeyExA/W`, `RegEnumKeyExA/W`
- [ ] Firmware variable trampolines for kernel32 exports reserved in [`src/kernel/pe.c`](../../src/kernel/pe.c) `s_kernel32_exports[]` (TODO-02 firmware variable + table surface XREF). Each implements ANSI/Wide name conversion, EFI_GUID string parsing (`{8be4df61-93ca-11d2-aa0d-00e098032b8c}` form), and dispatches the existing `SSDT_NtQuerySystemEnvironmentValueEx` / `SSDT_NtSetSystemEnvironmentValueEx` / `SSDT_NtQuerySystemInformation` slots. Privilege gating (`SE_SYSTEM_ENVIRONMENT_NAME` for the Set path) defers to the privilege table TODO. Trampolines: `GetFirmwareEnvironmentVariableA/W`, `SetFirmwareEnvironmentVariableA/W`, `GetSystemFirmwareTable`, `EnumSystemFirmwareTables`. Without these the kernel32 imports resolve but first calls decode arguments wrong (Codex design F1 2026-04-29 against the kernel-side reservation).
- [ ] Commit: `"win32: kernel32.dll console + process API"`

---

## 3. C Runtime (`msvcrt.dll`) `[Sonnet]`

Create `src/win32/msvcrt.c`. All functions map directly to the kernel's freestanding implementations.

- [ ] `printf(fmt, ...)` / `fprintf(stream, fmt, ...)` → kernel `kprintf` engine → `WriteFile(stdout, ...)`
- [ ] `puts(str)` / `fputs(str, stream)` → write string + newline
- [ ] `exit(code)` → `ExitProcess(code)`
- [ ] `malloc(size)` → user-heap `HeapAlloc(GetProcessHeap(), 0, size)` (§5)
- [ ] `free(ptr)` → `HeapFree(GetProcessHeap(), 0, ptr)`
- [ ] `calloc(n, size)` → `malloc(n*size)` + `memset`
- [ ] `realloc(ptr, size)` → allocate + copy + free
- [ ] `memcpy`, `memmove`, `memset`, `memcmp` → kernel implementations
- [ ] `strlen`, `strcpy`, `strncpy`, `strcmp`, `strncmp`, `strcat`, `strchr`, `strstr`
- [ ] `sprintf`, `snprintf`, `vsprintf`, `vsnprintf` → kernel printf engine
- [ ] `atoi`, `atol`, `strtol`, `strtoul`
- [ ] `rand`, `srand` → simple LCG
- [ ] Commit: `"win32: msvcrt.dll C runtime"`

---

## 4. NT Runtime Stubs (`ntdll.dll`) `[Sonnet]`

Create `src/win32/ntdll.c`. These are minimal stubs that satisfy the import resolver for programs that link against `ntdll`.

- [ ] `RtlInitUnicodeString(dest, src)` → populate `Length` and `Buffer` fields; no-op if `src == NULL`
- [ ] `RtlFreeUnicodeString(str)` → `HeapFree` the Buffer if non-NULL
- [ ] `NtCurrentTeb()` → return `FS_BASE` MSR value (current thread's TEB pointer)
- [ ] `RtlGetVersion(lpVersionInfo)` → populate `OSVERSIONINFOEXW` with Impossible OS version info
- [ ] `RtlAllocateHeap(heap, flags, size)` / `RtlFreeHeap(heap, flags, ptr)` → forward to `HeapAlloc`/`HeapFree`
- [ ] `NtAllocateVirtualMemory` / `NtFreeVirtualMemory` → thin wrappers over `vmm_alloc_user`/`vmm_free_user`
- [ ] `NtQuerySystemInformation(class, buf, len, retlen)` → partial: `SystemBasicInformation`, `SystemProcessorInformation`
- [ ] Commit: `"win32: ntdll.dll minimal stubs"`

---

## 5. Memory Management API `[Sonnet]`

Extend `src/win32/kernel32.c` with virtual memory and heap management. Backed by `vmm_alloc_user()` and `vmm_free_user()` from `TODO-07 §4`.

- [ ] `VirtualAlloc(addr, size, type, protect)` → `vmm_alloc_user(size)` at `addr` (or any if NULL); `MEM_COMMIT|MEM_RESERVE` → allocate + zero; return base address
- [ ] `VirtualFree(addr, size, type)` → `vmm_free_user(addr, size)` on `MEM_RELEASE`
- [ ] `VirtualProtect(addr, size, newProtect, oldProtect)` → `NtProtectVirtualMemory()` (TODO-07 §2 syscall)
- [ ] `VirtualQuery(addr, mbi, size)` → fill `MEMORY_BASIC_INFORMATION`: `BaseAddress`, `RegionSize`, `State` (`MEM_COMMIT`/`MEM_FREE`), `Protect`, `Type`
- [ ] `HeapCreate(options, initialSize, maxSize)` → allocate a user-mode heap region; return opaque `HANDLE`
- [ ] `HeapDestroy(heap)` → release the heap region
- [ ] `HeapAlloc(heap, flags, size)` → sub-allocate from heap region; `HEAP_ZERO_MEMORY` flag → zero fill
- [ ] `HeapFree(heap, flags, ptr)` → return allocation to heap region
- [ ] `GetProcessHeap()` → return handle to default process heap (created at process init in `pe_exec()`)
- [ ] `GlobalAlloc(flags, size)` / `GlobalFree(hmem)` → wrap `HeapAlloc`/`HeapFree` on default heap
- [ ] `LocalAlloc(flags, size)` / `LocalFree(hmem)` → same as `GlobalAlloc`/`GlobalFree`
- [ ] Add `SYS_VIRTUALALLOC`, `SYS_VIRTUALFREE`, `SYS_VIRTUALPROTECT` syscall dispatch in `syscall.c` (TODO-07 §2 defines numbers 68–70)
- [ ] Commit: `"win32: kernel32.dll memory management"`

---

## 6. Synchronization API `[Sonnet]`

Add to `src/win32/kernel32.c`. All Win32 sync objects are thin wrappers over kernel primitives (`mutex_t`, `semaphore_t`, `event_t`) stored in the task handle table.

- [ ] **Mutex:** `CreateMutexA/W(attr, bInitialOwner, name)` → allocate `mutex_t`, optionally `mutex_lock()`; return HANDLE
- [ ] `ReleaseMutex(hMutex)` → `mutex_unlock()`
- [ ] **Semaphore:** `CreateSemaphoreA/W(attr, initial, max, name)` → allocate `semaphore_t`; return HANDLE
- [ ] `ReleaseSemaphore(hSem, count, prevCount)` → `sem_signal()` × count
- [ ] **Event:** `CreateEventA/W(attr, bManualReset, bInitialState, name)` → allocate `event_t` with `EVENT_MANUAL_RESET` or `EVENT_AUTO_RESET`
- [ ] `SetEvent(hEvent)` → `event_set()`
- [ ] `ResetEvent(hEvent)` → `event_reset()`
- [ ] `WaitForSingleObject(hObject, dwMilliseconds)` → dispatch on handle type: mutex → `mutex_lock`; semaphore → `sem_wait`; event → `event_wait_timeout(ev, ms)`; process → wait for task exit; return `WAIT_OBJECT_0` or `WAIT_TIMEOUT`
- [ ] `WaitForMultipleObjects(count, handles, bWaitAll, ms)` → iterate and wait; `bWaitAll` blocks until all are signaled
- [ ] **Critical Section:** `InitializeCriticalSection(cs)` → init embedded `mutex_t`; `EnterCriticalSection(cs)` → `mutex_lock()`; `LeaveCriticalSection(cs)` → `mutex_unlock()`; `DeleteCriticalSection(cs)` → no-op
- [ ] **Interlocked:** `InterlockedIncrement(ptr)` → `lock xadd [ptr], 1`; `InterlockedDecrement` → `lock xadd [ptr], -1`; `InterlockedCompareExchange(ptr, exch, cmp)` → `lock cmpxchg [ptr], exch`; all inline assembly or compiler intrinsics
- [ ] Commit: `"win32: kernel32.dll synchronization API"`

---

## 7. DLL Loading API `[Opus]`

Add to `src/win32/kernel32.c` and extend `src/win32/pe_loader.c`. Enables loading PE DLLs (same loader as EXEs but `IMAGE_FILE_DLL` flag) and resolving exports at runtime.

- [ ] `LoadLibraryA/W(path)` → detect if already loaded (walk PEB loaded-modules list by `BaseDllName`); if not: `pe_load(path)`, validate `IMAGE_FILE_DLL`, `pe_apply_relocations()`, `pe_resolve_imports()`, call `DllMain(hModule, DLL_PROCESS_ATTACH, NULL)` if present; add to PEB loaded-modules list; return `HMODULE` = `ImageBase`
- [ ] `GetProcAddress(hModule, lpProcName)` → walk PE export directory (`IMAGE_DIRECTORY_ENTRY_EXPORT`): if `lpProcName` is ordinal (high bit set) → look up by ordinal; else → binary search `AddressOfNames[]`; return function pointer; return NULL + `SetLastError(ERROR_PROC_NOT_FOUND)` on miss
- [ ] `FreeLibrary(hModule)` → decrement reference count in loaded-modules list; if ref == 0: call `DllMain(DLL_PROCESS_DETACH)`, unmap image pages, remove from list
- [ ] Per-process loaded-modules list in PEB: `struct ldr_module { void *base; char name[64]; uint32_t ref_count; }` array (max 64 entries)
- [ ] Built-in DLLs (`kernel32.dll`, `user32.dll`, `gdi32.dll`, `ntdll.dll`, `msvcrt.dll`) registered at process init with their stub export tables -- `GetProcAddress` hits these without disk I/O
- [ ] Commit: `"win32: LoadLibrary + GetProcAddress DLL loading"`

---

## 8. Error API `[Sonnet]`

- [ ] `GetLastError()` → read `TEB.LastErrorValue` (set during all Win32 API failures)
- [ ] `SetLastError(code)` → write `TEB.LastErrorValue`
- [ ] `FormatMessageA(flags, source, msgId, langId, buf, size, args)` → `FORMAT_MESSAGE_FROM_SYSTEM`: look up `msgId` in a built-in table of Win32 error strings (at minimum: `ERROR_SUCCESS`, `ERROR_FILE_NOT_FOUND`, `ERROR_ACCESS_DENIED`, `ERROR_INVALID_HANDLE`, `ERROR_NOT_ENOUGH_MEMORY`, `ERROR_INVALID_PARAMETER`, `ERROR_PROC_NOT_FOUND`, `ERROR_MOD_NOT_FOUND`); copy to `buf`
- [ ] `OutputDebugStringA/W(str)` → write to serial output (via `klog_debug` or serial port)
- [ ] Commit: `"win32: error API and debug output"`

---

## 9. Unimplemented Function Logger `[Sonnet]`

Prevents crashes when a PE calls a function not yet implemented. Maps to Impossible OS's superior diagnostics story.

- [ ] Create `src/win32/unimpl.c`: `win32_unimpl_stub(const char *dll, const char *fn)` → `klog_warn("UNIMPL: %s!%s", dll, fn)`; increment call count in a static table (max 256 entries: `dll!fn` → count)
- [ ] All stub entries in export tables (Section 2, Sections 10 through 12) route to `win32_unimpl_stub(dll, fn)` when the real implementation is missing
- [ ] Return safe defaults: `(HANDLE)0` / `0` / `FALSE` / `NULL` as appropriate per function signature
- [ ] Shell command `win32log` (in `cmd.exe`) → dump the unimpl table sorted by call count descending to serial + screen
- [ ] Commit: `"win32: unimplemented function logger"`

---

## 10. Window Management (`user32.dll`) `[Sonnet]`

Create `src/win32/user32.c`. Maps Win32 window and message API to the native WM (`wm_create_window`, `wm_destroy_window`).

- [ ] Per-export tracking and Done bits: authoritative table in [`TODO-A-user32-export-master-table.md`](TODO-A-user32-export-master-table.md) (Tier 1 core pump; Tier 2 TODO-11 bridge). Mark each row `[x]` only when callable from a PE and tested per [`../12-user-platform-sdk/TODO-07-win32-compat-matrix.md`](../12-user-platform-sdk/TODO-07-win32-compat-matrix.md) tiers.
- [ ] Implement Tier 1 behaviors: class registration, `CreateWindowEx`, message loop (`GetMessage` / `TranslateMessage` / `DispatchMessage`), `DefWindowProc` integration with [`../12-user-platform-sdk/TODO-05-win32-subsystem.md`](../12-user-platform-sdk/TODO-05-win32-subsystem.md), `PostQuitMessage` / `PostMessage` / `SendMessage`, `MessageBox` syscall path, `SetWindowText`, `GetClientRect`.
- [ ] Wire exports into the built-in DLL table per [`TODO-07-win32-pe-loader.md`](TODO-07-win32-pe-loader.md) Section 6; ship with the on-disk name contract in TODO-A (`C:\Windows\System32\user32.dll`).
- [ ] Commit: `"win32: user32.dll window management"`

---

## 11. GDI Rendering (`gdi32.dll`) `[Sonnet]`

Create `src/win32/gdi32.c`. Maps GDI primitives to the kernel `gfx_*` and `ttf_*` APIs.

- [ ] Optional future: add `TODO-D-gdi32-export-master-table.md` in this folder for export-by-export parity with TODO-A style. Until then, use [`../08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md`](../08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md) plus [`../08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md`](../08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md) for NtGdi mapping.
- [ ] Device context model, `GetDC` / `ReleaseDC`, `BeginPaint` / `EndPaint`, `CreateCompatibleDC`, `DeleteDC`, `BitBlt`, `TextOut` / `DrawText`, `FillRect`, `SetTextColor` / `SetBkColor`, `SetPixel` / `GetPixel`, `MoveToEx` / `LineTo`, `CreateSolidBrush`, `SelectObject`, PE smoke test (window draws text and rectangle).
- [ ] Commit: `"win32: gdi32.dll rendering"`

---

## 12. Shell & Icon API (`shell32.dll`) `[Sonnet]`

Create `src/win32/shell32.c`.

- [ ] Per-export rows: [`TODO-C-shell32-export-master-table.md`](TODO-C-shell32-export-master-table.md) (Tier 1 icons and paths; Tier 1b `ShellExecute` and path helpers). Mark rows `[x]` with the same Done gate as Section 10.
- [ ] Shell32 / imageres **icon index** tables stay in [`../08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md`](../08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md) Section 1; TODO-C rows point there with Notes (no second index source).
- [ ] Commit: `"win32: shell32.dll shell + icon API"
---

## 13. IxUI Native Toolkit `[Opus]`

IxUI is Impossible OS's **native** Win32-compatible GUI framework. Unlike Wine's user32 emulation, IxUI is a first-class, kernel-backed GUI toolkit for native app development. Apps link `sdk/lib/libixui.a` or use Win32 `#include <windows.h>`.

- [ ] Create `sdk/include/ixui.h`:
  - `IxCreateWindow(className, title, style, x, y, w, h, parent)` → `SYS_CREATEWINDOW` → `wm_create()`
  - `IxRegisterWindowClass(className, wndProc)` → register WndProc callback in per-process class table
  - `IxGetMessage(lpMsg)` → `SYS_GETMESSAGE` → block on WM event queue
  - `IxDispatchMessage(lpMsg)` → dispatch to registered WndProc
  - `IxPostMessage(hWnd, msg, wParam, lParam)` → `SYS_POSTMESSAGE`
  - `IxDestroyWindow(hWnd)` → `SYS_DESTROYWINDOW`
- [ ] Built-in window class implementations (backed by `CTRL_*` widgets from 08-graphics-ui/TODO-04):
  - `"BUTTON"` → `ctrl_create_button()`; `BN_CLICKED` → `WM_COMMAND(BN_CLICKED)`
  - `"EDIT"` → `ctrl_create_textbox()`; `EN_CHANGE` notifications
  - `"STATIC"` → `ctrl_create_label()`
  - `"LISTBOX"` → `ctrl_create_listview()` (single-column); `LBN_SELCHANGE`
  - `"COMBOBOX"` → `ctrl_create_listview()` + dropdown overlay
  - `"SCROLLBAR"` → `ctrl_create_scrollbar()`
- [ ] Add `SYS_CREATEWINDOW`, `SYS_DESTROYWINDOW`, `SYS_GETMESSAGE`, `SYS_POSTMESSAGE`, `SYS_SHOWWINDOW` syscall numbers (extend `include/kernel/sched/abi.h`)
- [ ] Build `sdk/lib/libixui.a` (static library for userland programs)
- [ ] Test: native PE app with window + BUTTON + EDIT + message loop → renders on desktop, button click fires WM_COMMAND
- [ ] Commit: `"sdk: IxUI native GUI toolkit"`

---

## 14. Win32 Shell Integration `[Sonnet]`

Add to `src/win32/shell32.c`. Enables `ShellExecute` and file-association-aware launching from both shell and desktop.

- [ ] `ShellExecuteA/W(hwnd, verb, file, params, dir, show)`:
  - `"open"` → `file_assoc_open(file)` → `exec_load()` for executables, or open in registered app
  - `"edit"` → `file_assoc_get_app(file, "edit")` → launch editor with file as argument
  - `"explore"` → launch file manager at directory
  - Returns `HINSTANCE > 32` on success; Win32 error codes (2 = file not found, 5 = access denied) on failure
- [ ] `ShellExecuteExA/W(lpExecInfo)` → extended form; populate `hProcess` in `SHELLEXECUTEINFO`
- [ ] Windows path utilities:
  - `PathIsRelativeA/W(path)` → true if no drive letter or `\\` prefix
  - `PathGetDriveNumberA/W(path)` → 0 for `C:`, 1 for `D:`, etc.
  - `PathStripPathA/W(path)` → in-place filename extraction
  - `GetFullPathNameA/W(rel, size, buf, filePart)` → prepend CWD for relative paths
- [ ] Commit: `"win32: shell32.dll ShellExecute + path utilities"`

---

## 15. Developer SDK `[Sonnet]`

Provides the cross-compilation toolchain for targeting Impossible OS from a host Linux system.

- [ ] Create `sdk/include/windows.h` -- aggregates all Win32 headers:
  - File I/O, process, memory, sync, console, registry declarations
  - GUI: `CreateWindowEx`, `RegisterClassEx`, `GetMessage`, `DispatchMessage`, `MessageBox`
  - Wraps IxUI: `#include "ixui.h"` for native `IxCreate*` extensions
- [ ] Create `sdk/include/impossible.h` -- Impossible OS extensions: `GetImpossibleVersion()`, `IxUI types`, `IRES icon handles`, OS feature flags
- [ ] Create `sdk/lib/kernel32.lib` -- import library (`__imp__*` symbols) for PE linker
- [ ] Create `sdk/lib/user32.lib`, `sdk/lib/gdi32.lib`, `sdk/lib/shell32.lib`, `sdk/lib/msvcrt.lib` -- import libraries
- [ ] Create `sdk/lib/libixui.a` -- static IxUI library (from §13)
- [ ] Create `tools/impossible-cc` wrapper script:
  - `x86_64-w64-mingw32-gcc -I$SDK/include -L$SDK/lib -lkernel32 -luser32 "$@"`
  - Auto-selects CRT (`-lmsvcrt`) and sets correct subsystem (`-Wl,--subsystem,console` or `windows`)
- [ ] Test: `impossible-cc hello.c -o hello.exe` → produces valid PE; runs on Impossible OS and prints "Hello, Win32!"
- [ ] Test: `impossible-cc gui_app.c -o app.exe -Wl,--subsystem,windows` → window app runs on desktop
- [ ] Commit: `"sdk: Impossible OS developer SDK"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                 | 🐧 Linux                      | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ------------------------ | ----------------------------- | ---------------------------------------- |
| 💎  | Console API                              | ✅ Win32                 | ✅ POSIX tty                  | ⬜ routes to terminal emulator           |
| 💎  | Process management                       | ✅ Win32                 | ✅ `fork`/`waitpid`           | ⬜ wraps native task scheduler           |
| 💎  | Memory management                        | ✅ Win32                 | ✅ `mmap`/`malloc`            | ⬜ wraps `vmm_alloc_user`                |
| 💎  | Synchronization                          | ✅ Win32                 | ✅ `pthread_mutex`, `eventfd` | ⬜ thin wrappers on kernel `mutex_t`/`event_t` |
| 💎  | DLL loading                              | ✅ Win32                 | ✅ `dlopen`/`dlsym`           | ⬜ native PE export directory walk       |
| 💎  | Error API                                | ✅ Win32                 | ✅ `errno`/`strerror`         | ⬜ TEB-backed per-thread error           |
| 💎  | GUI window management                    | ✅ Win32                 | ✅ X11/Wayland                | ⬜ wraps native WM                       |
| 💎  | GDI rendering                            | ✅ GDI32                 | ✅ Cairo/Skia                 | ⬜ wraps `gfx_*`/`ttf_*`                 |
| 💎  | C runtime                                | ✅ MSVCRT                | ✅ glibc/musl                 | ⬜ kernel freestanding implementations   |
| 💎  | Shell API                                | ✅ shell32               | ✅ `xdg-open`                 | ⬜ `file_assoc_open()` + VFS paths       |
| ⭐  | Native IxUI toolkit                      | ❌ User32 emulation only | ❌ No native Win32            | ⬜ first-class kernel-backed Win32 GUI   |
| ⭐  | Zero-layer Win32 ABI                     | ❌ Requires Windows      | ❌ Requires Wine              | ⬜ native kernel implements Win32        |
| ⭐  | Unimplemented function logger with call-count telemetry | ❌ Crashes or silent     | ❌ Crashes                    | ⬜ safe stubs + serial diagnostics       |
| ⭐  | Developer SDK                            | ❌ Windows only          | ❌ No Win32 SDK               | ⬜ MinGW cross-compiler + native headers |

**Impossible OS advantage:** Win32 is implemented natively in the kernel -- no translation layer, no Wine, no DLL emulation. IxUI is a first-class toolkit that gives Win32 programs a native compositor-backed window system with zero overhead. The unimplemented-function logger gives a unique observability story not available on any other platform.


## History

| Date | Action | Summary |
|------|--------|---------|
| 2026-04-14 | Added DLL export master tables TODO-A/B/C; condensed Sections 10 through 12 to XREF those tables | Hub links TODO-A/B/C as authoritative export rows. |
| 2026-04-14 | gap-analysis | Confirmed TODO-A/B/C own export rows; TODO-08 hub + narrative only; Win32k syscall map stays 08-graphics-ui TODO-A shadow SSDT. |
| 2026-04-14 | validate | Domain label set to 10-services-security; History schema aligned. |
| 2026-04-14 | retarget | Renamed domain folder to `10-platform-services`; refreshed all `todo/` XREF paths. |

---

## Verification

**Sections 1 to 3: Console Hello World**
- Cross-compile: `x86_64-w64-mingw32-gcc -o hello.exe hello.c` (uses `WriteConsoleA`, `ExitProcess`)
- QEMU serial: `hello.exe` runs, prints "Hello, Win32!" to console

**§5: Memory**
- `VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE)` → returns valid user-mode address; write + read back succeeds
- `VirtualFree(ptr, 0, MEM_RELEASE)` → no crash; accessing freed pages causes page fault

**§6: Synchronization**
- Create mutex, two threads: thread A holds, thread B blocks in `WaitForSingleObject`; `ReleaseMutex` unblocks B
- `WaitForSingleObject(hProcess, INFINITE)` waits until spawned process exits

**§7: DLL Loading**
- `LoadLibraryA("kernel32.dll")` → returns non-NULL HMODULE; `GetProcAddress(hmod, "ExitProcess")` → returns valid function pointer
- Load a simple PE DLL: `DllMain(DLL_PROCESS_ATTACH)` fires; `GetProcAddress` resolves exported function

**Sections 10 and 11: GUI App**
- Cross-compile GUI app: `CreateWindowEx`, `RegisterClassEx`, `WndProc` with `WM_PAINT` drawing "Hello, GDI!"
- QEMU: window appears on desktop, text renders, close button destroys window

**§13: IxUI**
- Native PE with `#include <ixui.h>`: `IxCreateWindow("BUTTON", ...)` → button appears in window; click fires `WM_COMMAND`

**§15: SDK Round-Trip**
- `impossible-cc hello.c -o hello.exe` completes without errors
- Resulting PE runs on Impossible OS and on Windows (dual compatibility test)
