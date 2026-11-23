---
schema_version: 1
id: compiler-sdk
domain: 10-platform-services
status: active
title: "TODO-09 -- C/C++ Compiler & SDK"
---

# TODO-09 -- C/C++ Compiler & SDK

**Domain:** `10-platform-services`
**Goal:** Port TCC to run natively on Impossible OS so developers can compile and run native PE apps on the OS itself, and publish a complete developer SDK -- the moment the OS can develop its own software.

> [!IMPORTANT]
> **Depends on:** `TODO-07 §1–9` -- ring-3 PE execution; `TODO-08 §5–15` -- Win32 API surface (console, process, memory, file I/O, IxUI) must be working before TCC can run natively.
> **Continues from:** `todo-old/510-Long-Term-Stretch/TODO-530-Compiler.md` and `TODO-535-SDK.md` (migrated and consolidated).

---

## Important Notes

- `SYS_FORK=5`, `SYS_EXEC=6`, `SYS_MMAP=37` already defined in `include/kernel/sched/syscall.h`. `mmap_region_t` exists in `include/kernel/mm/mmap.h`. `thread_create()` exists in `include/kernel/sched/task.h`. TCC only requires `CreateFile`/`ReadFile`/`WriteFile` + `VirtualAlloc` for its core operation -- far lighter than GCC.
- `ImpossibleSDK/` paths are in the separate Impossible SDK repository, which replaced this repository's `sdk/` folder on 2026-11-23 (`todo/answers.md` Q55). The header, example and API reference that `sdk/` held were removed with it; nothing here builds on them.
- TCC already supports PE output natively with `-m64` -- minimal patching needed (include paths + temp dir).
- GCC/Clang (§11) is a **long-term** goal requiring `fork()`/`exec()` process spawning, large virtual memory, and `libgmp`/`libmpfr`/`libmpc` prerequisites -- tracked here but not a blocker for TCC milestones.
- SDK headers (`§1–2`) overlap with `TODO-08 §5` (Win32 types) and `TODO-08 §15` (SDK); this TODO focuses on the **on-OS** headers installed at `C:\Impossible\Include\` and the TCC build pipeline; `TODO-08 §15` covers the host cross-compile SDK (`impossible-cc`).
- Import libraries (`§3`) are COFF `.lib` stub files for the PE linker -- distinct from `libixui.a` (static library produced in `TODO-08 §13`).

---

## Inputs

| Path | Purpose |
|------|---------|
| `ImpossibleSDK/include/impossible/` | Impossible SDK headers (`base.h` and `window.h` today) |
| `ImpossibleSDK/samples/` | Impossible SDK samples (console and window programs today) |
| `include/kernel/sched/syscall.h` | `SYS_FORK`, `SYS_EXEC`, `SYS_MMAP`, `SYS_SLEEP` numbers |
| `include/kernel/sched/task.h` | `thread_create()` |
| `include/kernel/mm/mmap.h` | `mmap_region_t`, `mmap()` kernel API |
| `include/kernel/timer.h` | `timer_sleep_ms()` for `Sleep()` backing |
| → XREF: `TODO-07 §2` | `SYS_*` Win32 syscall ABI, `handle_table` in task |
| → XREF: `TODO-08 §5–5,13,15` | Win32 types, process API, memory API, IxUI, `impossible-cc` host SDK |
| Related | `11-apps/TODO-08-notepad.md` | Native IDE integration (`cc` build action, error parse) |
| `D11 T05 §11` | The C++23 user-mode runtime (libc++, libc++abi, libunwind) for bundled apps is built there; §11 here keeps on-device compilers |

---

## Outcome

- TCC runs natively on Impossible OS: `tcc hello.c -o hello.exe && hello.exe` prints output.
- `tcc -run hello.c` JIT-compiles and runs without an intermediate file.
- TCC is self-hosting: `tcc tcc.c -o tcc2.exe` succeeds.
- `#include <impossible.h>` and `#include <windows.h>` resolve from `C:\Impossible\Include\`.
- `tcc gui_hello.c -lixui -o gui_hello.exe` compiles and runs a windowed app.
- `cc` shell alias and `run <file.c>` shortcut work in `cmd.exe`.
- Minimal `make` utility parses Makefiles with variables and dependency tracking.
- SDK installer packages headers + import libs for host cross-compile.
- GCC/Clang long-term: `gcc.exe` on OS with C++ support.

---

## Implementation Order

| #   | Section                                                   | Tag        | Dep               | Mark |
| --- | --------------------------------------------------------- | ---------- | ----------------- | ---- |
| 1   | SDK C headers (`impossible.h` + subsystem headers)        | `[Sonnet]` | TODO-08 §5        | ⭐   |
| 2   | SDK GUI headers (`window.h`, `gdi.h`, `controls.h`, etc.) | `[Sonnet]` | TODO-08 §10–11,13 | 💎   |
| 3   | SDK import libraries (`kernel32.lib`, `user32.lib`, etc.) | `[Sonnet]` | §1–2              | 💎   |
| 4   | Cross-compile TCC for Impossible OS                       | `[Opus]`   | §1–3, TODO-08 §4  | 💎   |
| 5   | Install TCC on OS disk image                              | `[Sonnet]` | §4                | 💎   |
| 6   | TCC self-hosting tests                                    | `[Sonnet]` | §5                | 💎   |
| 7   | TCC IxUI integration                                      | `[Sonnet]` | §6, TODO-08 §13   | ⭐   |
| 8   | Shell compiler integration (`cc`, `run`, `make`)          | `[Sonnet]` | §6                | ⭐   |
| 9   | SDK installer + on-OS pre-install                         | `[Sonnet]` | §1–3              | ⭐   |
| 10  | SDK documentation                                         | `[Sonnet]` | §1–9              | 💎   |
| 11  | GCC/Clang (long-term C++ support)                         | `[Opus]`   | §6, TODO-08 §3    | 💎   |

---

## 1. SDK C Headers

Create the canonical on-OS C header set at `ImpossibleSDK/include/` (installed to `C:\Impossible\Include\` on disk). Compatible with TCC and MinGW cross-compiler. No stdlib -- freestanding only.

- [ ] Create `ImpossibleSDK/include/impossible.h` -- master umbrella include: pulls in all subsystem headers below; guarded with `#pragma once`; defines `IOS_SDK_VERSION_MAJOR/MINOR/PATCH`
- [ ] `ImpossibleSDK/include/impossible/types.h` -- `uint8_t`…`uint64_t`, `size_t`, `ptrdiff_t`, `bool`, `true`/`false`, `NULL`; `HANDLE`, `DWORD`, `BOOL` (if not already from `windows.h`)
- [ ] `ImpossibleSDK/include/impossible/errors.h` -- `IOS_OK=0`, `IOS_ERR_NOT_FOUND`, `IOS_ERR_ACCESS_DENIED`, `IOS_ERR_INVALID_HANDLE`, `IOS_ERR_OUT_OF_MEMORY`, `IOS_ERR_INVALID_PARAM`, `IOS_ERR_NOT_SUPPORTED`
  - `GetLastError()` / `SetLastError()` declarations
- [ ] `ImpossibleSDK/include/impossible/memory.h` -- `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `HeapAlloc`, `HeapFree`, `GetProcessHeap`; `MEM_COMMIT`, `MEM_RESERVE`, `MEM_RELEASE`, `PAGE_*` constants
- [ ] `ImpossibleSDK/include/impossible/process.h` -- `CreateProcessA/W`, `ExitProcess`, `GetCurrentProcessId`, `WaitForSingleObject`, `TerminateProcess`, `OpenProcess`, `GetExitCodeProcess`; `PROCESS_INFORMATION`, `STARTUPINFO` structs
- [ ] `ImpossibleSDK/include/impossible/thread.h` -- `CreateThread(attr, stackSize, startAddr, param, flags, threadId)` → `thread_create()` wrapper; `ExitThread(code)`, `GetCurrentThreadId()`, `Sleep(ms)` → `SYS_SLEEP`; `THREAD_PRIORITY_*` constants
- [ ] `ImpossibleSDK/include/impossible/sync.h` -- `CreateMutex`, `CreateSemaphore`, `CreateEvent`, `WaitForSingleObject`, `WaitForMultipleObjects`, `ReleaseMutex`, `ReleaseSemaphore`, `SetEvent`, `ResetEvent`
  - `InitializeCriticalSection`, `EnterCriticalSection`, `LeaveCriticalSection`; `Interlocked*` intrinsics
- [ ] `ImpossibleSDK/include/impossible/file.h` -- `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle`, `SetFilePointer`, `GetFileSize`, `FlushFileBuffers`; `GENERIC_READ`, `GENERIC_WRITE`, `FILE_SHARE_*`, `CREATE_*`, `OPEN_*` constants
- [ ] `ImpossibleSDK/include/impossible/filesystem.h` -- `CreateDirectoryA`, `RemoveDirectoryA`, `DeleteFileA`, `MoveFileA`, `CopyFileA`, `GetFileAttributesA`, `FindFirstFileA`, `FindNextFileA`, `FindClose`
  - `WIN32_FIND_DATA` struct; `FILE_ATTRIBUTE_*` constants
- [ ] `ImpossibleSDK/include/impossible/string.h` -- `strlen`, `strcpy`, `strncpy`, `strcmp`, `strncmp`, `strcat`, `strchr`, `strstr`, `memcpy`, `memmove`, `memset`, `memcmp`; `sprintf`, `snprintf`, `vsprintf`, `vsnprintf`
  - `atoi`, `atol`, `strtol`, `strtoul`
- [ ] `ImpossibleSDK/include/impossible/io.h` -- `printf`, `fprintf`, `puts`, `fputs`, `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `feof`; `stdin`/`stdout`/`stderr` as `HANDLE` aliases; `SEEK_SET`, `SEEK_CUR`, `SEEK_END`
- [ ] `ImpossibleSDK/include/impossible/time.h` -- `GetSystemTime(lpst)` fills `SYSTEMTIME` struct; `GetLocalTime(lpst)`; `GetTickCount()` → milliseconds since boot
  - `QueryPerformanceCounter(lpFrequency)` / `QueryPerformanceFrequency(lpFrequency)`; `SYSTEMTIME` struct (`wYear`, `wMonth`, `wDay`, `wHour`, `wMinute`, `wSecond`, `wMilliseconds`)
- [ ] Public UI ABI is Win32, full stop: the SDK exposes `user32` / `gdi32` / `comctl32` / `uxtheme` / `dwmapi` declarations only, never a parallel native widget API
  - The `Ix*` shorthands in §2 `controls.h` are macros over `CreateWindowExA` built-in classes (`12-user-platform-sdk/TODO-05 §2` → XREF); the kernel `CTRL_*` API stays shell-internal and is not installed under `ImpossibleSDK/include/`
  - Consequence, and the reason: a program built against this SDK is a Windows 11 program and vice versa, so the Win11 look (`08-graphics-ui/TODO-03 §9`) and Win11 app compatibility (`12-user-platform-sdk/TODO-07`) are one implementation, not two
- [ ] Create `ImpossibleSDK/include/impossible/windows.h` → include all subsystem headers above so `#include <windows.h>` is a full Win32-compat header
- [ ] Commit: `"sdk: C header set (types, process, thread, file, sync, io, time)"`

---

## 2. SDK GUI Headers

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

GUI-facing headers for Win32 window/GDI programs and IxUI native apps. These complement the C headers and are pulled in when `<windows.h>` is included.

- [ ] `ImpossibleSDK/include/impossible/window.h` -- `CreateWindowExA/W`, `DestroyWindow`, `ShowWindow`, `SetWindowTextA/W`, `GetClientRect`, `MoveWindow`; `SW_*`, `WS_*`, `WS_EX_*` style constants; `WNDCLASSEX` struct
  - `RegisterClassExA/W`, `UnregisterClassA/W`
- [ ] `ImpossibleSDK/include/impossible/message.h` -- `MSG` struct; `GetMessage`, `PeekMessage`, `TranslateMessage`, `DispatchMessage`, `PostQuitMessage`, `PostMessage`, `SendMessage`, `DefWindowProc`
  - `WM_CREATE`, `WM_DESTROY`, `WM_PAINT`, `WM_CLOSE`, `WM_QUIT`, `WM_COMMAND`, `WM_KEYDOWN`, `WM_KEYUP`, `WM_CHAR`, `WM_LBUTTONDOWN`, `WM_MOUSEMOVE`, `WM_SIZE` constants
- [ ] `ImpossibleSDK/include/impossible/paint.h` -- `PAINTSTRUCT` struct; `BeginPaint`, `EndPaint`, `InvalidateRect`, `UpdateWindow`, `GetUpdateRect`
- [ ] `ImpossibleSDK/include/impossible/gdi.h` -- `HDC`; `GetDC`, `ReleaseDC`, `CreateCompatibleDC`, `DeleteDC`; `BitBlt`, `TextOutA/W`, `DrawTextA/W`, `FillRect`, `SetPixel`, `GetPixel`, `MoveToEx`, `LineTo`
  - `SetTextColor`, `SetBkColor`, `SetBkMode`; `CreateSolidBrush`, `DeleteObject`, `SelectObject`, `GetStockObject`; `TRANSPARENT`, `OPAQUE` bk-mode constants
- [ ] `ImpossibleSDK/include/impossible/input.h` -- `GetAsyncKeyState(vKey)`, `GetKeyState(vKey)`, `GetCursorPos(lpPoint)`, `SetCursorPos(x, y)`, `POINT` struct
  - `VK_*` virtual key constants (at minimum: `VK_RETURN`, `VK_ESCAPE`, `VK_BACK`, `VK_TAB`, arrow keys, F1–F12)
- [ ] `ImpossibleSDK/include/impossible/dialog.h` -- `MessageBoxA/W`, `MessageBoxExA/W`; `MB_OK`, `MB_OKCANCEL`, `MB_YESNO`, `MB_ICONERROR`, `MB_ICONWARNING`, `MB_ICONINFORMATION` constants
  - `IDOK`, `IDCANCEL`, `IDYES`, `IDNO` return values; `OPENFILENAME` struct + `GetOpenFileNameA`, `GetSaveFileNameA` stubs
- [ ] `ImpossibleSDK/include/impossible/menu.h` -- `CreateMenu`, `CreatePopupMenu`, `AppendMenuA/W`, `DestroyMenu`, `TrackPopupMenu`; `SetMenu(hWnd, hMenu)`; `MF_STRING`, `MF_SEPARATOR`, `MF_POPUP`, `MF_GRAYED` constants
- [ ] `ImpossibleSDK/include/impossible/controls.h` (SDK-facing) -- `CreateWindowExA` shorthand macros for built-in classes: `IxCreateButton(parent, id, label, x, y, w, h)`, `IxCreateEdit(parent, id, text, x, y, w, h)`
  - `IxCreateStatic(parent, id, label, x, y, w, h)`, `IxCreateListBox(parent, id, x, y, w, h)`, `IxCreateComboBox(parent, id, x, y, w, h)`, `IxCreateScrollBar(parent, id, x, y, w, h, horiz)`
- [ ] `ImpossibleSDK/include/impossible/font.h` -- `LOGFONT` struct; `CreateFontA/W(h, w, esc, orient, weight, italic, ...)`, `DeleteObject`; `GetTextExtentPoint32A/W(hDC, str, len, lpSize)` → measure text; `SIZE` struct
- [ ] `ImpossibleSDK/include/impossible/bitmap.h` -- `BITMAP`, `BITMAPINFO`, `BITMAPINFOHEADER` structs; `LoadBitmapA`, `CreateBitmap`, `CreateCompatibleBitmap`, `DeleteObject`; `HBITMAP`
- [ ] Commit: `"sdk: GUI header set (window, message, paint, GDI, input, dialog, menu, controls, font, bitmap)"`

---

## 3. SDK Import Libraries

Build COFF `.lib` import stub libraries for the PE linker. These let MinGW-compiled and TCC-compiled programs link against Impossible OS DLL stubs.

- [ ] Create `tools/mkimportlib.c` -- host tool that generates a COFF `.lib` from a symbol list file (`dll_name: symbol1 symbol2 ...`); emits `__imp__*` indirection stubs compatible with `ld.lld` and TCC's linker
- [ ] Generate `ImpossibleSDK/lib/kernel32.lib` from `kernel32` symbol list (all exports from `TODO-08 §4,5,6,8`)
- [ ] Generate `ImpossibleSDK/lib/user32.lib` from `user32` symbol list (all exports from `TODO-08 §10`)
- [ ] Generate `ImpossibleSDK/lib/gdi32.lib` from `gdi32` symbol list (all exports from `TODO-08 §11`)
- [ ] Generate `ImpossibleSDK/lib/ntdll.lib` from `ntdll` symbol list (all exports from `TODO-08 §2`)
- [ ] Generate `ImpossibleSDK/lib/shell32.lib` from `shell32` symbol list (all exports from `TODO-08 §12,14`)
- [ ] Generate `ImpossibleSDK/lib/msvcrt.lib` from `msvcrt` symbol list (all exports from `TODO-08 §1`)
- [ ] Add `make install-sdk` Makefile target: copies all headers to `C:\Impossible\Include\` and all libs to `C:\Impossible\Lib\` on the disk image
- [ ] Commit: `"sdk: COFF import libraries (kernel32, user32, gdi32, ntdll, shell32, msvcrt)"`

---

## 4. Cross-Compile TCC for Impossible OS

TCC already supports PE/x86-64 output (`-m64`). Patch and configure for Impossible OS paths. This section requires careful integration of TCC's allocator and I/O routines against the SDK.

- [ ] Download TCC source: `repo.or.cz/tinycc.git` (latest stable)
- [ ] Configure for x86-64 target with PE output:
  - `./configure --cpu=x86_64 --enable-static --cross-prefix=x86_64-w64-mingw32-`
  - Output format: PE (`-DTCC_TARGET_PE`)
- [ ] Patch include search paths:
  - Default system include: `C:\Impossible\Include\`
  - TCC own include: `C:\Impossible\Include\tcc\`
  - Library search: `C:\Impossible\Lib\`
- [ ] Patch temp directory: replace all `"/tmp/"` occurrences with `"C:\\Temp\\"`
- [ ] Set PE as default output format (remove ELF default)
- [ ] Replace POSIX file I/O in TCC's host layer (`tcc_open`, `tcc_close`) with `CreateFileA`/`ReadFile`/`WriteFile` from `kernel32.lib`
- [ ] Replace `malloc`/`free` in TCC with `HeapAlloc`/`HeapFree` from process default heap
- [ ] Link TCC statically against `ImpossibleSDK/lib/kernel32.lib` (no POSIX libc)
- [ ] Build `tcc.exe` as static PE binary: `x86_64-w64-mingw32-gcc -static -o build/devtools/tcc.exe tcc.c -I"$IMPOSSIBLE_SDK_DIR/include" -L"$IMPOSSIBLE_SDK_DIR/lib" -lkernel32`
- [ ] Verify: `tcc.exe` passes PE validation (correct DOS header, PE signature, x86-64 machine type)
- [ ] Commit: `"userland: cross-compile TCC for Impossible OS"`

---

## 5. Install TCC on OS Disk Image

- [ ] Copy `tcc.exe` → `C:\Impossible\Bin\tcc.exe` on the IXFS partition
- [ ] Copy `libtcc1.a` → `C:\Impossible\Lib\libtcc1.a`
- [ ] Copy TCC's own include files → `C:\Impossible\Include\tcc\` (tcc's `include/` subdirectory)
- [ ] Copy all SDK headers (§1–2) → `C:\Impossible\Include\`
- [ ] Copy all SDK import libs (§3) → `C:\Impossible\Lib\`
- [ ] Copy `ImpossibleSDK/lib/libixui.a` → `C:\Impossible\Lib\libixui.a` (from `TODO-08 §13`)
- [ ] Copy `ImpossibleSDK/include/ixui.h` → `C:\Impossible\Include\ixui.h` (from `TODO-08 §13`)
- [ ] Add `make install-tcc` Makefile target that automates all of the above during disk image build
- [ ] Verify: `dir C:\Impossible\Bin\` shows `tcc.exe`; `dir C:\Impossible\Include\` shows `impossible.h`, `windows.h`
- [ ] Commit: `"userland: install TCC and SDK on disk image"`

---

## 6. TCC Self-Hosting Tests

Run all TCC tests natively inside QEMU after §5. Each test validates a different compiler capability.

- [ ] `tcc hello.c -o hello.exe` → compiles `printf("Hello from TCC!\n")` to PE; serial shows compile success
- [ ] `hello.exe` → prints "Hello from TCC on Impossible OS!" to console (via `WriteConsoleA`)
- [ ] `tcc -run hello.c` → JIT-compile and execute without writing a file
- [ ] Separate compilation: `tcc -c math.c -o math.o && tcc main.c math.o -o app.exe` → links .o files
- [ ] `#include <impossible.h>` resolves from `C:\Impossible\Include\` -- no path error
- [ ] `#include <windows.h>` resolves from `C:\Impossible\Include\` -- no path error
- [ ] `tcc tcc.c -o tcc2.exe` → TCC compiles itself (self-hosting milestone)
- [ ] Run `tcc2.exe hello.c -o hello2.exe && hello2.exe` → second-generation compiler works
- [ ] Commit: `"userland: TCC self-test + self-hosting milestone"`

---

## 7. TCC IxUI Integration

Enables on-OS GUI app development with TCC and the IxUI toolkit.

- [ ] Verify `ixui.h` installed at `C:\Impossible\Include\` and `libixui.a` at `C:\Impossible\Lib\` (§5)
- [ ] `ImpossibleSDK/samples/gui_hello.c` -- `#include <ixui.h>` + `IxCreateWindow` + `WM_PAINT` draws "Hello, IxUI!" label
- [ ] Test: `tcc gui_hello.c -lixui -o gui_hello.exe` → compiles without errors
- [ ] Test: `gui_hello.exe` → window appears on the desktop with label text
- [ ] `ImpossibleSDK/samples/button_app.c` -- window with a BUTTON that counts clicks; `WM_COMMAND` handler updates `STATIC` label
- [ ] Test: `tcc button_app.c -lixui -o button_app.exe && button_app.exe` → button click increments counter
- [ ] Commit: `"userland: TCC + IxUI GUI integration"`

---

## 8. Shell Compiler Integration

Integrates TCC into `cmd.exe` for a first-class developer experience on Impossible OS.

- [ ] `cc` alias in `cmd.exe` built-in alias table → `C:\Impossible\Bin\tcc.exe`
- [ ] `run <file.c>` shell built-in → shortcut for `tcc -run <file.c>`; no intermediate file
- [ ] Create `user/make/make.c` -- minimal `make` utility:
  - Parse `Makefile` line-by-line: variable assignment (`VAR = value`), rules (`target: deps`), recipes (tab-indented commands)
  - Variable substitution (`$(VAR)` in rules and recipes)
  - Dependency tracking: compare file modification timestamps via `GetFileAttributesEx`
  - Only rebuild targets whose deps are newer
  - Implicit rules: `%.exe: %.c` → `tcc $< -o $@`
- [ ] `make.exe` installed to `C:\Impossible\Bin\make.exe`
- [ ] Test: `make` in a directory with a `Makefile` → builds only out-of-date targets
- [ ] Shell command `sdk-info` → prints TCC version, SDK version, include path, lib path
- [ ] Commit: `"shell: cc alias, run shortcut, and make utility"`

---

## 9. SDK Installer + On-OS Pre-Install

Packages the SDK for host cross-compilation and pre-installs it on the OS disk image.

- [ ] Create `ImpossibleSDK/package.sh` -- assembles `impossible-os-sdk-vX.Y.Z.tar.gz` containing: `include/`, `lib/`, `examples/`, `docs/`, `cmake/ImpossibleOS.cmake` toolchain file
- [ ] `cmake/ImpossibleOS.cmake` -- CMake toolchain: sets `CMAKE_C_COMPILER` to `x86_64-w64-mingw32-gcc`, adds `-I$(SDK)/include -L$(SDK)/lib`, targets PE subsystem
- [ ] Create `ImpossibleSDK/setup.sh` -- installs SDK to `~/.impossible-os-sdk/`, adds `impossible-cc` wrapper to PATH
- [ ] Pre-install on disk image: `make install-sdk` (from §3) copies all headers + libs into the IXFS partition during build
- [ ] `C:\Impossible\SDK\Examples\` -- sample projects pre-loaded: `hello_console/`, `hello_window/`, `notepad_clone/` (each with `Makefile` + source)
- [ ] GitHub Actions step: build `impossible-os-sdk-*.tar.gz` and attach to release tags
- [ ] Commit: `"sdk: installer package + on-OS pre-install"`

---

## 10. SDK Documentation

- [ ] Create `ImpossibleSDK/docs/api-reference.md` -- full API reference for all headers (§1–2): function signatures, parameter descriptions, return values, error codes
- [ ] Create `ImpossibleSDK/docs/getting-started.md` -- "Hello World" tutorial: install SDK, cross-compile, copy to QEMU, run; covers both console and GUI app paths
- [ ] Create `ImpossibleSDK/docs/porting-guide.md` -- how to port an existing Win32 program to Impossible OS: include path changes, unsupported APIs, `WINAPI` ABI note
- [ ] Update `ImpossibleSDK/README.md` -- quick-start snippet: three commands from zero to running `hello.exe`
- [ ] Create `ImpossibleSDK/docs/contributing.md` -- guidelines for contributing to the public `impossible-os-sdk` GitHub repo
- [ ] Commit: `"docs: SDK documentation (getting started, API reference, porting guide)"`

---

## 11. GCC/Clang (Long-Term C++ Support)

> Long-term goal. Requires a mature userland environment. TCC milestones (§4–8) are the prerequisite -- do not start §11 until TCC is self-hosting.

**Additional prerequisites beyond TCC:**

| Prerequisite                           | Status                           | Why Needed                                           |
| -------------------------------------- | -------------------------------- | ---------------------------------------------------- |
| `fork()` + `exec()`                    | `SYS_FORK=5`, `SYS_EXEC=6` exist | GCC spawns cc1, as, ld as child processes            |
| `pipe()` for IPC                       | ⬜ Planned                       | Pipeline between preprocessor → compiler → assembler |
| Large virtual memory (256+ MB/process) | ⬜ Planned (TODO-08 §3)          | GCC uses 100+ MB during C++ compilation              |
| `libgmp`, `libmpfr`, `libmpc`          | ⬜ Planned                       | GCC internal math library dependencies               |
| Writable `C:\Temp\` with ≥ 512 MiB     | ⬜ Planned                       | Intermediate compilation files                       |
| Working `make` utility                 | ⬜ §8 above                      | GCC configure + build system                         |

- [ ] Verify `CreateProcess()` (`SYS_CREATEPROCESS`) reliably spawns child processes with correct handle inheritance
- [ ] Verify `pipe()` (IPC between parent/child) works for preprocessor → compiler pipeline
- [ ] Cross-compile `libgmp`, `libmpfr`, `libmpc` for `x86_64-w64-mingw32` target
- [ ] Cross-compile Stage 1 GCC on Linux targeting `x86_64-w64-mingw32` (`--enable-languages=c --disable-shared`)
- [ ] Cross-compile binutils (`as.exe`, `ld.exe`) for Impossible OS; install to `C:\Impossible\Bin\`
- [ ] Cross-compile Stage 2 GCC targeting Impossible OS; link against SDK; install `gcc.exe` to `C:\Impossible\Bin\`
- [ ] Test: `gcc hello.c -o hello.exe` on Impossible OS → compiles and runs
- [ ] Add C++ support: `--enable-languages=c,c++`; cross-compile `libstdc++`
- [ ] Test: `g++ hello.cpp -o hello.exe` with classes + templates → compiles and runs
- [ ] Commit: `"userland: GCC/G++ natively on Impossible OS"`

---

## OS Comparison


| ⭐  | Feature                                              | 🪟 Win11                         | 🐧 Linux                    | 🚀 Impossible OS                              |
| --- | ---------------------------------------------------- | -------------------------------- | --------------------------- | --------------------------------------------- |
| 💎  | Native C compiler on OS                              | ✅ MSVC                          | ✅ GCC/Clang                | ⬜ TCC natively, self-hosting                 |
| 💎  | GCC/Clang C++ compiler                               | ✅ MSVC C++                      | ✅ GCC/Clang                | ⬜ §11 -- (long-term, )                       |
| 💎  | SDK headers                                          | ✅ Windows SDK                   | ✅ glibc headers            | ⬜ `impossible.h` + subsystem headers         |
| 💎  | Import libraries for linker                          | ✅ Windows SDK                   | ✅ `.so` stubs              | ⬜ COFF `.lib` from `mkimportlib`             |
| 💎  | Cross-compilation toolchain                          | ✅ VS Build Tools                | ✅ `gcc`/`clang`            | ⬜ §15 -- MinGW wrapper (`TODO-08 `)          |
| 💎  | `make` build utility                                 | ✅ nmake/MSBuild                 | ✅ GNU make                 | ⬜ lightweight Makefile parser                |
| ⭐  | Self-hosting TCC on OS                               | ❌ Can't run TCC on Windows      | ❌ TCC runs but targets ELF | ⬜ TCC outputs PE on Impossible               |
| ⭐  | SDK pre-installed out-of-the-box on OS image         | ❌ Separate SDK install required | ❌ distro-specific headers  | ⬜ headers + libs at `C:\Impossible\Include\` |
| ⭐  | `cc`/`run` shell built-ins for instant C compilation | ❌ No equivalent                 | ❌ No equivalent            | ⬜ `run hello.c` compiles and executes        |
| ⭐  | IxUI GUI framework compilable natively with TCC      | ❌ Requires full Win32 SDK       | ❌ No native Win32          | ⬜ `tcc gui.c -lixui -o app.exe`              |

**Impossible OS advantage:** The SDK ships pre-installed on the OS -- a developer can boot Impossible OS, type `run hello.c`, and their program runs, with zero additional setup. TCC self-hosting on a custom OS is a milestone that neither Windows nor Linux achieve with their native formats. The `run` shell built-in makes C feel like a scripting language.

---

## Verification

**§1–3: Headers resolve**
- `#include <impossible.h>` in a TCC or MinGW compile → no errors; all type and function declarations visible
- Import lib link: `x86_64-w64-mingw32-gcc hello.c ImpossibleSDK/lib/kernel32.lib -o hello.exe` → valid PE

**§4–5: TCC installed**
- `dir C:\Impossible\Bin\tcc.exe` → file present; `tcc --version` → prints version string to serial

**§6: Self-hosting**
- `tcc hello.c -o hello.exe && hello.exe` → prints "Hello from TCC!"
- `tcc -run hello.c` → same output, no intermediate file
- `tcc tcc.c -o tcc2.exe` → no compile errors; `tcc2.exe --version` matches original

**§7: IxUI**
- `tcc gui_hello.c -lixui -o gui_hello.exe && gui_hello.exe` → window appears on QEMU desktop

**§8: Shell integration**
- `cc hello.c -o hello.exe` in `cmd.exe` → compiles (alias works)
- `run hello.c` → executes immediately without `-o`
- `make` in a directory with a Makefile → builds targets; re-run does nothing (up-to-date)

**§11: GCC (long-term)**
- `gcc hello.c -o hello.exe` on Impossible OS → compiles and runs
- `g++ hello.cpp -o hello.exe` with `std::vector<int>` → compiles and runs
