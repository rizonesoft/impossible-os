---
schema_version: 1
id: win32-compat-matrix
domain: 12-user-platform-sdk
status: active
title: "TODO-07 -- Win32 Compatibility Matrix & Bring-Up Ladder"
---

# TODO-07 -- Win32 Compatibility Matrix & Bring-Up Ladder

> **Goal:** Create the compatibility tracking system and progressive test-program
> bring-up ladder that validates Win32 compatibility systematically -- from the first
> `ExitProcess` call to running real unmodified Windows programs. The north star:
> **when a Win32 PE program calls any standard function, it just works.**

> [!IMPORTANT]
> This TODO owns **tracking and testing infrastructure only** -- it does not implement
> any Win32 API functions. Implementation of each API lives in its owning TODO
> (`10-platform-services/TODO-07` through `TODO-08`, `12-user-platform-sdk/TODO-04`
> through `TODO-05`). Gate conditions reference those TODOs.
>
> **`win32_unimpl_stub`** (in-kernel call-count table + `win32log` shell command) is
> already specced in `10-platform-services/TODO-08 §9`. This TODO extends it with a
> shared-memory counter map for user-mode access and the `win32compat.exe` report tool --
> do not re-specify the base stub infrastructure.
>
> **Tier gate rule**: a tier is declared ✅ complete **only** when all native test
> programs in that tier produce the expected serial output **and** the specified real
> Windows binaries run correctly.

---

## Inputs

- `10-platform-services/TODO-07-win32-pe-loader.md` (→ XREF) -- gate TODO for Tiers 1–3
- `10-platform-services/TODO-08-win32-api-surface.md §9` (→ XREF) -- `win32_unimpl_stub` call-count table; extend here with shmem map
- `10-platform-services/TODO-08-win32-api-surface.md §5 §4 §3 §6 §7 §10` (→ XREF) -- gate TODOs for Tiers 2–7
- `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §4 §3` (→ XREF) -- gate TODO for Tiers 5–7
- `12-user-platform-sdk/TODO-05-win32-subsystem.md §1–4` (→ XREF) -- gate TODO for Tiers 6–7
- `02-kernel-core/TODO-14-registry-completion.md` (→ XREF) -- gate TODO for Tier 4 (RegOpenKey etc.)
- `include/kernel/ipc/` -- `SYS_SHMEM_CREATE=35`, `SYS_SHMEM_MAP=36` for stub counter shared memory
- `user/` -- `hello.c`, `cmd.c` as reference user-mode program patterns
- `scripts/build.sh` -- QEMU headless run pattern (`-serial stdio`); `build/serial.log`
- `18-future-research/TODO-06-android-app-compatibility.md` (→ XREF) -- TODO-06 sections 5 and 8 launcher UX and lifecycle for Android guest beside Win32 tiers (future epic)
- `sdk/docs/` -- `win32-compat.md` lives here (§1)
- `../10-platform-services/TODO-A-user32-export-master-table.md` (XREF) authoritative user32.dll export rows + Done bits (this TODO keeps tier gates + sample apps)
- `../10-platform-services/TODO-B-comctl32-export-master-table.md` (XREF) authoritative comctl32.dll export rows
- `../10-platform-services/TODO-C-shell32-export-master-table.md` (XREF) authoritative shell32.dll export rows

---

## Outcome

`sdk/docs/win32-compat.md` tracks aggregate Win32 function status by DLL and tier. Per-DLL export rows and owners live in D10 TODO-A, TODO-B, and TODO-C (see Inputs). Each tier 1 through 8 has passing native test programs and green real-binary milestones. `make compat-check` runs all tier 1 through 7 programs under QEMU headless and reports a compat score. When PuTTY runs, Win32 compatibility is excellent.

---

## Implementation Order

| Step | Section                                        | 💎/⭐ | Gate / Dependency                                                                                                     |
| ---- | ---------------------------------------------- | ----- | --------------------------------------------------------------------------------------------------------------------- |
| 1    | API coverage tracker + `compat_stub` extension | ⭐    | `D10T08 §9` base stub; `SYS_SHMEM_CREATE/MAP`                                                                         |
| 2    | Bring-up ladder framework                      | ⭐    | §1; `sdk/compat/` test program structure                                                                              |
| 3    | Tier 1 -- Process exit                         | 💎    | Gate: `TODO-07 §1`                                                                                                    |
| 4    | Tier 2 -- Console I/O                          | 💎    | Gate: `D10T08 §5 §4`                                                                                                  |
| 5    | Tier 3 -- File I/O                             | 💎    | Gate: `TODO-07 §7`, `D10T08 §1` partial                                                                               |
| 6    | Tier 4 -- Process & Registry                   | 💎    | Gate: `D10T08 §4`, `D02T14`                                                                                           |
| 7    | Tier 5 -- Memory & Sync                        | 💎    | Gate: `D10T08 §3 §6`                                                                                                  |
| 8    | Tier 6 -- MessageBox & Basic GUI               | 💎    | Gate: `TODO-05 §1–3`, `D10T08 §10`                                                                                    |
| 9    | Tier 7 -- Full Win32 Window + Controls         | 💎    | Gate: `TODO-05 §4–4`, `D10T08 §7 §10 §11`                                                                             |
| 10   | Tier 8 -- Extended Win32 surface               | 💎    | Gate: all Tiers 1–7; `advapi32`/`shell32`                                                                             |
| 11   | Stub call log analysis (`win32compat.exe log`) | ⭐    | §1 shmem counters; `win32_unimpl_stub`                                                                                |
| 12   | CI compat gate (`scripts/compat-check.sh`)     | ⭐    | §3–9 native programs; QEMU headless                                                                                   |
| 13   | Tier 9 -- WinRT + DirectX + WinUI 3 apps       | 💎    | Gate: Tier 8; `TODO-05 §9`; COM/WinRT, MSVC CRT, Direct3D 11 + Direct2D + DirectWrite, DirectComposition (owners TBD) |

---

## 1. API Coverage Tracker `[Sonnet]`

**Output:** `sdk/docs/win32-compat.md` (hand-maintained + `tools/compat-scan.sh` regeneration)

- [ ] **`sdk/docs/win32-compat.md`** structure:
  - Header: date regenerated, total tracked, summary counts per status
  - One summary table per DLL in `win32-compat.md` (kernel32.dll, user32.dll, gdi32.dll, ntdll.dll, advapi32.dll, shell32.dll, msvcrt.dll, comdlg32.dll); detailed export rows for user32, comctl32, and shell32 stay in D10 TODO-A / TODO-B / TODO-C
  - Columns: `Function | Status | Tier | Notes`
  - Status values: `✅ implemented`, `⚠️ stub (returns error)`, `🔄 partial`, `❌ missing`
  - Tier: 1–8 from bring-up ladder (§2)
- [ ] **`compat_stub(dll, name)` macro extension** (extends `D10T08 §9` base):
  ```c
  /* In sdk/compat/compat_stub.h -- shared between kernel and compat tools */
  #define COMPAT_STUB_COUNT 512

  typedef struct {
      char     name[64];      /* "kernel32!CreateFiber" */
      uint32_t hit_count;
      uint32_t last_errno;    /* last error returned */
  } compat_stub_entry_t;

  /* Shared memory segment name: "Win32CompatCounters" */
  extern compat_stub_entry_t *g_compat_stubs; /* shmem-mapped in user mode */
  ```
  - Extend `win32_unimpl_stub(dll, fn)` from `D10T08 §9`: additionally write to shmem counter map (if `g_compat_stubs != NULL` -- kernel mode sets to NULL, user-mode `win32compat.exe` maps the shmem)
  - Shmem key: `SYS_SHMEM_CREATE("Win32CompatCounters", sizeof(compat_stub_entry_t) * 512)`; created once at process start; `win32compat.exe` maps it via `SYS_SHMEM_MAP`
- [ ] **`win32compat.exe` tool** (`src/tools/win32compat.c`):
  - `win32compat report`: map shmem → sort by `hit_count` desc → print top 40 entries
  - `win32compat status`: read `HKLM\SYSTEM\Win32Compat\Score` → print current compat %; print tier completion status
  - `win32compat reset`: zero all counters in shmem
- [ ] **`tools/compat-scan.sh`** (host-side script):
  - Scan `src/win32/*.c` for `EXPORT_STUB` / `COMPAT_STUB` / real implementations
  - Update `sdk/docs/win32-compat.md` status rows from code scan results
  - Print diff from previous run (new implementations since last scan)

---

## 2. Bring-Up Ladder Framework `[Sonnet]`

**Directory:** `sdk/compat/` -- one subdirectory per tier

- [ ] **Directory structure**:
  ```
  sdk/compat/
  ├── Makefile          ← builds all tiers with TCC; run: make TIER=1..8
  ├── tier1/            ← § 3
  ├── tier2/            ← § 4
  ├── tier3/            ← § 5
  ├── tier4/            ← § 6
  ├── tier5/            ← § 7
  ├── tier6/            ← § 8
  ├── tier7/            ← § 9
  ├── tier8/            ← § 10
  └── run_tier.sh       ← QEMU headless runner; checks expected serial output
  ```
- [ ] **`sdk/compat/run_tier.sh <tier> <program.exe>`**:
  - Launch QEMU with `bash scripts/build.sh run` headless (`-display none -serial stdio`)
  - Inject `program.exe` into disk image (via loop mount or `mcopy`)
  - Wait for serial output to match expected string (timeout 30 s)
  - Print `[PASS] tier{N}/{program}` or `[FAIL] tier{N}/{program}: expected "{X}" got "{Y}"`
- [ ] **Per-tier `README.md`**: gate condition, function list (~count), native programs, real Windows binary targets, pass criteria
- [ ] **Milestone badges** in `sdk/docs/win32-compat.md`: add a `## Milestones` section; each real Windows binary that runs earns a badge row: `| {binary} | ✅ Runs | {tier} | {date} | {notes} |`

---

## 3. Tier 1 -- Process Exit Only `[Sonnet]`

> Gate: `10-platform-services/TODO-07 §1` (ring-3 fix + `SYS_EXITPROCESS`)

**~2 functions:** `ExitProcess`, `GetLastError`

- [ ] **`tier1/tier1_exit.c`**: `#include <windows.h>` -- `void WinMainCRTStartup(void) { ExitProcess(0); }` -- hand-assembled minimal PE; no CRT; no imports except `kernel32.dll!ExitProcess`
  - Expected serial output: `[sched] task {pid} exited with code 0`
  - Expected shell output: `Exit code: 0`
- [ ] **Tier 1 pass criteria**: process terminates cleanly; shell returns to prompt; ring-3 transition and `SYS_EXITPROCESS` confirmed working
- [ ] **Real binary target**: none for Tier 1 (this is the bare minimum)
- [ ] Mark tier gate in `win32-compat.md` with date + serial log excerpt when passing

---

## 4. Tier 2 -- Console I/O `[Sonnet]`

> Gate: `10-platform-services/TODO-08 §5 §8`

**~6 functions:** `GetStdHandle`, `WriteConsoleA`, `ReadConsoleA`, `ExitProcess`, `GetLastError`, `SetLastError`

- [ ] **`tier2/tier2_hello.c`**: `WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), "Hello, World!\n", 14, NULL, NULL); ExitProcess(0);`
  - Expected: `Hello, World!` on console + serial
- [ ] **`tier2/tier2_echo.c`**: `ReadConsoleA(stdin, buf, 256, &read, NULL); WriteConsoleA(stdout, buf, read, NULL, NULL);`
  - Expected: echoes typed line back; exit code 0
- [ ] **`tier2/tier2_getlasterr.c`**: `WriteFile(INVALID_HANDLE_VALUE, "x", 1, NULL, NULL); assert(GetLastError() == ERROR_INVALID_HANDLE); ExitProcess(0);`
  - Expected: exit 0 (assert passes)
- [ ] **Real Windows binary target**: `cmd.exe` from **Windows XP** (~50 Win32 functions, console-only)
  - Run `cmd.exe /C echo hello`; expected: `hello` printed + exit 0
  - Pass criteria: prompt appears, basic built-in commands work (`echo`, `exit`)

---

## 5. Tier 3 -- File I/O `[Sonnet]`

> Gate: `10-platform-services/TODO-07 §7`, `D10T08 §1` partial

**~12 functions:** `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle`, `GetFileSize`, `SetFilePointer`, `DeleteFile`, `MoveFile`, `CreateDirectory`, `RemoveDirectory`, `FindFirstFile`, `FindNextFile`, `FindClose`

- [ ] **`tier3/tier3_cat.c`**: open file from `argv[1]`; read 4096 bytes at a time; write to stdout; exit
  - Expected: file contents printed to console
- [ ] **`tier3/tier3_dir.c`**: `FindFirstFile("C:\\*", &fd)` loop; print `fd.cFileName` + `fd.nFileSizeLow`; `FindClose`
  - Expected: lists root directory; at least 3 entries
- [ ] **`tier3/tier3_copy.c`**: `CreateFile(src, GENERIC_READ)`; `CreateFile(dst, GENERIC_WRITE, CREATE_NEW)`; copy loop; verify byte counts match; `CloseHandle` both
  - Expected: `Copied N bytes` + file exists at destination
- [ ] **Real Windows binary target**: **Busybox for Windows** (`busybox32.exe`)
  - `busybox cat X:\Logs\boot.log` → prints log
  - `busybox ls C:\` → prints directory listing
  - Pass criteria: at least `cat` and `ls` commands produce correct output

---

## 6. Tier 4 -- Process & Registry `[Sonnet]`

> Gate: `10-platform-services/TODO-08 §4`, `02-kernel-core/TODO-14-registry-completion.md`

**~20 functions** adds: `CreateProcess`, `WaitForSingleObject`, `GetExitCodeProcess`, `TerminateProcess`, `RegOpenKeyExA`, `RegQueryValueExA`, `RegSetValueExA`, `RegCreateKeyExA`, `RegCloseKey`, `GetEnvironmentVariableA`, `SetEnvironmentVariableA`, `GetCommandLineA`, `OpenProcess`, `GetCurrentProcessId`, `GetCurrentThreadId`

- [ ] **`tier4/tier4_spawn.c`**: `CreateProcess(NULL, "notepad.exe", ...)` (or simpler child); `WaitForSingleObject(hProc, INFINITE)`; `GetExitCodeProcess` → print; exit
  - Expected: child spawns + exits; parent prints child exit code
- [ ] **`tier4/tier4_regread.c`**: `RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Impossible\\Version", ...)` → `RegQueryValueExA` → print string; `RegCloseKey`
  - Expected: prints OS version string (e.g., `1.0.0`)
- [ ] **`tier4/tier4_env.c`**: `SetEnvironmentVariableA("MYVAR", "test123")`; `GetEnvironmentVariableA("MYVAR", buf, 64)`; `WriteConsoleA(stdout, buf, ...)` → verify round-trip
  - Expected: prints `test123`
- [ ] **Real Windows binary targets**:
  - **GNU Make for Windows** (`make.exe` static): `make.exe --version` → prints `GNU Make N.N`
  - **TCC self-hosting milestone**: `tcc.exe hello.c -o hello.exe` on-OS → `hello.exe` runs → prints `Hello`
  - Pass criteria: at minimum `make.exe --version` exits cleanly; TCC self-hosting is the milestone

---

## 7. Tier 5 -- Memory & Sync `[Sonnet]`

> Gate: `10-platform-services/TODO-08 §3 §6`, `12-user-platform-sdk/TODO-04 §4`

**~15 additional functions** adds: `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`, `HeapCreate`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `GetProcessHeap`, `CreateMutex`, `CreateEvent`, `SetEvent`, `ResetEvent`, `WaitForMultipleObjects`, `InitializeCriticalSection`, `EnterCriticalSection`, `LeaveCriticalSection`, `DeleteCriticalSection`, `InterlockedCompareExchange`, `InterlockedIncrement`, `InterlockedDecrement`

- [ ] **`tier5/tier5_heap.c`**: `HeapCreate(0, 0, 0)` → alloc 1 MB → write 0xAB pattern → verify → `HeapFree` → `HeapDestroy`; use `GetProcessHeap()` for a second allocation; verify both work
  - Expected: `Heap test passed` + exit 0
- [ ] **`tier5/tier5_mutex.c`**: create 2 threads; each increments shared counter 10000× inside `EnterCriticalSection`; join; verify final count == 20000
  - Expected: `Counter: 20000` + exit 0
- [ ] **`tier5/tier5_virtual.c`**: `VirtualAlloc(NULL, 65536, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)` → write → `VirtualProtect(PAGE_READONLY)` → attempt write → catch access violation → `VirtualFree`
  - Expected: `VirtualAlloc/Protect test passed`
- [ ] **Real Windows binary targets**:
  - **SQLite3 shell for Windows** (`sqlite3.exe` static): `.version` command prints version string; `CREATE TABLE; INSERT; SELECT` -- all execute correctly
  - **Lua interpreter** (`lua54.exe` static): `lua -e "print('hello from Lua')"` → prints output
  - Pass criteria: SQLite `.version` + basic SQL works; Lua `print` works

---

## 8. Tier 6 -- MessageBox & Basic GUI `[Sonnet]`

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog)

> Gate: `12-user-platform-sdk/TODO-05 §1–3`, `10-platform-services/TODO-08 §10`

**~5 additional functions** adds: `MessageBoxA`, `MessageBoxW`, `LoadIconA`, `LoadCursorA`, `GetSystemMetrics`

- [ ] **`tier6/tier6_msgbox.c`**: `int r = MessageBoxA(NULL, "Hello from Tier 6", "Test", MB_OK); ExitProcess(r == IDOK ? 0 : 1);`
  - Expected: dialog appears; click OK → exit 0
- [ ] **`tier6/tier6_yesno.c`**: `MessageBoxA(NULL, "Continue?", "Confirm", MB_YESNO)` → exit 0 if Yes, 1 if No
  - Expected: dialog appears with Yes/No buttons; both clicks produce correct exit code
- [ ] **Real Windows binary targets**:
  - **Winfile.exe** (Windows 3.x file manager, open-source reimplementation): launches window; navigates directory tree; lists files
  - **Classic Minesweeper** (WinXP era open-source reimplementation, ~30 Win32 functions including `WM_PAINT`): game window opens; mine grid renders; left-click reveals cell
  - Pass criteria: Winfile main window opens without crash; Minesweeper renders and accepts one click

---

## 9. Tier 7 -- Full Win32 Window + Controls `[Sonnet]`

> Gate: `12-user-platform-sdk/TODO-05 §4–4`, `10-platform-services/TODO-08 §7 §10 §11`

**~40 additional functions** adds: `RegisterClassExA`, `CreateWindowExA`, `ShowWindow`, `UpdateWindow`, `DestroyWindow`, `GetMessage`, `DispatchMessage`, `DefWindowProcA`, `TranslateMessage`, `PostQuitMessage`, `BeginPaint`, `EndPaint`, `InvalidateRect`, `TextOutA`, `DrawTextA`, `SetBkColor`, `SetTextColor`, `GetDC`, `ReleaseDC`, `CreatePen`, `CreateSolidBrush`, `SelectObject`, `DeleteObject`, `MoveWindow`, `SetWindowTextA`, `GetWindowTextA`, `GetClientRect`, `SetWindowLongPtrA`, `GetWindowLongPtrA`, standard control message set (`WM_COMMAND`, `BM_SETSTATE`, `EM_GETTEXT`, `LB_ADDSTRING`, etc.)

- [ ] **`tier7/tier7_window.c`**: full WndProc loop; `WM_PAINT` draws `"Hello, Window!"` via `TextOutA`; `WM_DESTROY` → `PostQuitMessage(0)`; `WM_KEYDOWN VK_ESCAPE` → `DestroyWindow`
  - Expected: window appears with text; Escape closes it; exit 0
- [ ] **`tier7/tier7_controls.c`**: `CreateWindowEx` with a Button (`BS_PUSHBUTTON`), Edit (`ES_AUTOHSCROLL`), ListBox, and Static label; button click appends edit text to listbox; `WM_COMMAND` routing
  - Expected: all 4 controls render; button click works; listbox accumulates entries
- [ ] **Real Windows binary targets**:
  - **Our own `notepad.exe`** compiled with TCC: opens; edits text; `File→Save` writes file; `File→Open` loads it
  - **PuTTY** (`putty.exe` open-source): main window renders; `Session` config dialog opens; SSH connection attempt (even if server unavailable) shows connection dialog correctly
  - Pass criteria: `notepad.exe` edit + save/load round-trip; PuTTY main window + Session dialog render without crash

---

## 10. Tier 8 -- Extended Win32 Surface `[Sonnet]`

> Gate: all Tiers 1–7; `advapi32.dll` security functions; `shell32.dll`; GDI+ subset

**~100+ additional functions** adds: `advapi32.dll` (security descriptors, `CryptAcquireContext`, `CryptGenRandom`, `OpenSCManager`, `CreateService`, `QueryServiceStatus`), `shell32.dll` (`SHGetFolderPath`, `SHBrowseForFolder`, `ShellExecuteA`, `ExtractIconEx`, `SHGetFileInfo`), extended GDI (`BitBlt`, `StretchBlt`, `CreateCompatibleBitmap`, `GetPixel`, `SetPixel`, `Polygon`, `Arc`, `CreateFont`, `GetTextMetrics`), `comctl32.dll` stub (`InitCommonControlsEx`)

- [ ] **`tier8/tier8_shell.c`**: `ShellExecuteA(NULL, "open", "notepad.exe", NULL, NULL, SW_SHOW)` → note-pad opens; `SHGetFolderPath(NULL, CSIDL_DESKTOP, ...)` → prints desktop path
  - Expected: notepad opens; path printed as `C:\Users\Default\Desktop`
- [ ] **`tier8/tier8_gdi.c`**: create compatible DC; `BitBlt` from window DC to off-screen; `GetPixel` reads expected color; `SelectObject` font → `TextOut`; verify metrics via `GetTextMetrics`
  - Expected: `GDI advanced test passed` + exit 0
- [ ] **Real Windows binary targets**:
  - **Our own `calculator.exe`**: opens; arithmetic works; keyboard input works; result displayed
  - **Notepad.exe from Windows XP** (extracted, ~120 Win32 functions): opens; text input; save/load; Find dialog; Help→About
  - **Far Manager** (open-source Windows console file manager, ~80 Win32 functions): panel view renders; directory navigation works
  - **`curl.exe` for Windows** (static build, ~50 Win32 functions): `curl --version` prints; `curl http://10.0.2.2/` succeeds (QEMU NAT host)
  - Pass criteria: each binary assigned a milestone badge row in `sdk/docs/win32-compat.md`

---

## 11. Stub Call Log Analysis `[Sonnet]`

**Source:** extends `src/tools/win32compat.c` (§1)

- [ ] **`win32compat log <program.exe>`**:
  1. `win32compat reset` -- zero shmem counters
  2. Launch `program.exe` (via `CreateProcess`)
  3. `WaitForSingleObject(hProc, 30000)` -- 30 s timeout
  4. Read shmem counter map; filter to `hit_count > 0` + `last_errno == ERROR_CALL_NOT_IMPLEMENTED`
  5. Sort by `hit_count` desc; print top 10:
     ```
     Stub call log: C:\putty.exe (1842 calls to unimplemented functions)
     
     Rank  Calls  DLL          Function
        1    412  user32.dll   LoadAccelerators
        2    308  gdi32.dll    CreateFontW
        3    201  advapi32.dll CryptGenRandom
        ...
     
     → These 10 functions likely needed to make putty.exe work.
     ```
  6. Write full log to `C:\Temp\compat_{program}_{timestamp}.log`
- [ ] **`tools/compat-scan.sh` auto-update**: parse `C:\Temp\compat_*.log` files; for each stub with `hit_count > 0` in `win32-compat.md`: change status to `⚠️` if currently `❌`; note call count in Notes column
- [ ] **`win32compat diff <before.log> <after.log>`**: compare two compat logs; show which stubs were eliminated (newly implemented) and which are new (regression)

---

## 12. CI Compatibility Gate `[Sonnet]`

**Source:** `scripts/compat-check.sh`

- [ ] **`scripts/compat-check.sh`**:
  1. Build OS: `bash scripts/build.sh` → verify `=== BUILD OK ===`
  2. For each tier 1–7 native test program: run `sdk/compat/run_tier.sh <tier> <program.exe>` under QEMU headless; collect pass/fail
  3. Compute compat score: query `win32compat report` after running all programs; score = `(✅ count + 🔄 count × 0.5) / total_tracked × 100`
  4. Write score to `HKLM\SYSTEM\Win32Compat\Score` (integer 0–100) via Registry init on next OS boot
  5. Print report:
     ```
     Win32 Compat Check Results
     ==========================
     Tier 1 (Process exit):    [PASS] 1/1
     Tier 2 (Console I/O):     [PASS] 3/3
     Tier 3 (File I/O):        [PASS] 3/3
     Tier 4 (Process/Reg):     [FAIL] 2/3 -- tier4_regread: timeout
     Tier 5 (Memory/Sync):     [SKIP] gate TODO-08 §6 not done
     ...
     
     Compat Score: 47%  (target: Tier 4 → 25%, Tier 6 → 50%, Tier 8 → 80%)
     ```
  6. Exit 0 if all enabled tiers pass; exit 1 if any enabled tier fails
- [ ] **Score milestones** (recorded in `HKLM\SYSTEM\Win32Compat\Score`; shown in System Information app):
  - 25%: Tier 4 complete (process + registry + TCC self-hosting)
  - 50%: Tier 6 complete (MessageBox + basic GUI)
  - 80%: Tier 8 complete (extended surface + curl + Far Manager)
  - 100%: all tracked functions implemented (long-term target)
- [ ] **`make compat-check`** target in root `Makefile`: calls `scripts/compat-check.sh`; used in CI
- [ ] **`sysinfo.exe` integration**: System Information app reads `HKLM\SYSTEM\Win32Compat\Score`; shows `Win32 Compat Score: {N}%` in its table (→ XREF `11-apps/TODO-13 §3`)

---

## 13. Tier 9 -- WinRT + DirectX + WinUI 3 / Windows App SDK Apps `[Opus]`

> **Spawned-by:** root

> Gate: Tier 8; `12-user-platform-sdk/TODO-05 §9` (→ XREF) Win11 opt-in surface; the four platform prerequisites below, each with an owning section

Modern Windows 11 apps (WinUI 3, Windows App SDK, and the .NET / C++ apps built on them) are Win32 PEs that load `Microsoft.UI.Xaml.dll`, `Microsoft.UI.Composition`, `CoreMessaging.dll` and friends from the Windows App SDK framework package, or from their own directory when self-contained. The framework DLLs are MIT-licensed source (`microsoft/microsoft-ui-xaml`, LICENSE verified 2026-08-29) but everything under them is platform, and this tier is that platform. It is deliberately NOT the native UI framework (`08-graphics-ui/TODO-03 §9` takes Fluent as data instead); it is the compatibility milestone that makes those apps run unmodified. Wine does not reach this tier either, for the same reasons listed below.

- [ ] **Prerequisite inventory**: each needs an owning section before this tier is scheduled; file it in the domain-correct TODO with a reciprocal XREF here, never a `-part-2` file
  - COM apartments + WinRT activation: `combase.dll` (`CoInitializeEx`, `CoCreateInstance`, `RoInitialize`, `RoGetActivationFactory`, `RoActivateInstance`), `.winmd` metadata lookup, `HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId` -- owner candidate `10-platform-services/TODO-08-win32-api-surface.md`
  - `ucrtbase.dll` + `vcruntime140.dll` + `msvcp140.dll` semantics for MSVC-built binaries, including MSVC C++ EH (`__CxxFrameHandler4`) over the SEH machinery -- owner candidate `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md`
  - D3D11 device + swapchain, Direct2D, DirectWrite on a software rasterizer (WARP-equivalent) -- owner candidate `18-future-research/TODO-03-gpu-compositor.md` (software path first; text via `08-graphics-ui/TODO-02 §7` FreeType/HarfBuzz)
  - DirectComposition / `Windows.UI.Composition` visual tree over the compositor (`dcomp.dll`, `DCompositionCreateDevice`, visuals, surfaces, animations) -- owner candidate `18-future-research/TODO-03-gpu-compositor.md`
  - Windows App SDK runtime services: `Microsoft.UI.Windowing` over HWNDs, `DispatcherQueue`, UI Automation (`UIAutomationCore.dll`, `08-graphics-ui/TODO-07`), TSF (`msctf.dll`) for IME
- [ ] **`tier9/tier9_winrt.c`**: `RoInitialize` + `RoGetActivationFactory(L"Windows.Foundation.Uri")` + `IUriRuntimeClassFactory::CreateUri` round-trip
  - Expected: `WinRT activation test passed` + exit 0
- [ ] **`tier9/tier9_d2d.c`**: `D3D11CreateDevice(WARP)` + `D2D1CreateFactory` + `DrawRectangle` into an HWND render target + DirectWrite `DrawText`
  - Expected: a rendered window and `D2D test passed`
- [ ] **Real binary targets**: WinUI 3 Gallery (self-contained build) main window renders and navigates; a `Microsoft.WindowsAppSDK` "Hello World" self-contained app opens
  - Pass criteria as Tier 8: badge rows in `sdk/docs/win32-compat.md`
- [ ] Extend the §2 ladder, §11 stub log and §12 `compat-check.sh` with Tier 9; the score denominator grows only once Tier 9 programs exist

---

## OS Comparison


| ⭐  | Feature                                        | 🪟 Win11                                            | 🐧 Linux                                     | 🚀 Impossible OS                                                                |
| --- | ---------------------------------------------- | --------------------------------------------------- | -------------------------------------------- | ------------------------------------------------------------------------------- |
| ⭐  | Win32 API coverage tracker                     | ✅ MSDN + Windows App Compat                        | ✅ Wine AppDB; ReactOS compat table          | ⬜ §1 -- `sdk/docs/win32-compat.md` with per-function status; `compat-scan.sh`  |
| ⭐  | `compat_stub` shmem counter + live report tool | ✅ ETW provider; `win32u!NtUser*` logging; AppVerif | ✅ Wine `WINEDEBUG=+relay`; strace           | ⬜ §1 -- shmem counter map; `win32compat report`                                |
| ⭐  | 8-tier progressive bring-up ladder             | ❌ No public bring-up ladder                        | ✅ ReactOS internal bring-up milestones      | ⬜ §2 -- –10; explicit gate conditions +                                        |
| 💎  | Runs XP `cmd.exe`                              | ✅ Native                                           | ✅ Wine runs XP cmd.exe                      | ⬜ §4                                                                           |
| 💎  | Runs Busybox, SQLite, Lua                      | ✅ Native                                           | ✅ Native Linux; Wine also runs              | ⬜ §5 -- §6 §7                                                                  |
| 💎  | Runs PuTTY                                     | ✅ Native                                           | ✅ Wine runs PuTTY                           | ⬜ §9                                                                           |
| ⭐  | Stub call log analysis                         | ❌ No equivalent (Windows is the                    | ✅ `wine --log-file` + `winetricks diagnose` | ⬜ §11 -- pinpoints exactly which stubs blocked                                 |
| ⭐  | CI compat gate with % score in sysinfo         | ❌ Not applicable                                   | ✅ ReactOS TestBot; Wine CI                  | ⬜ §12 -- `make compat-check`; score in `HKLM\SYSTEM\Win32Compat\Score`         |
| 💎  | Runs WinUI 3 / Windows App SDK apps            | ✅ Native                                           | ❌ Wine: no WinRT / DComp                    | ⬜ §13 -- Tier 9; four platform prerequisites inventoried with owner candidates |

Impossible OS's `⭐` advantage over Wine/ReactOS: the compat tracking is **kernel-native**
(stub counters in shmem, score in Registry, visible in System Information), the bring-up
ladder is **prescriptive** (explicit gate conditions tied to TODO numbers), and the stub
log analysis **automatically updates the compat matrix** -- closing the loop between
running a binary and knowing exactly what to implement next.

---

## Verification

Run `bash scripts/build.sh` then each tier's test in order.

- [ ] **Tracker**: `win32-compat.md` has ≥ 100 rows across 4 DLLs; `compat-scan.sh` runs without error; at least 10 functions have `✅` status
- [ ] **Tier 1**: `run_tier.sh 1 tier1_exit.exe` → `[PASS]`; serial shows clean task exit
- [ ] **Tier 2**: `tier2_hello.exe` prints `Hello, World!`; `tier2_getlasterr.exe` exits 0; XP `cmd.exe /C echo hi` prints `hi`
- [ ] **Tier 3**: `tier3_dir.exe` lists `C:\` directory; `tier3_copy.exe` copies a file correctly; `busybox cat` prints file contents
- [ ] **Tier 4**: `tier4_regread.exe` reads and prints `HKLM\SOFTWARE\Impossible\Version`; TCC self-hosting: `tcc.exe hello.c -o hello.exe` on-OS → `hello.exe` prints `Hello`
- [ ] **Tier 5**: `tier5_heap.exe` exits 0; `tier5_mutex.exe` prints `Counter: 20000`; SQLite `.version` works
- [ ] **Tier 6**: `tier6_msgbox.exe` dialog appears; OK → exit 0; Winfile main window renders
- [ ] **Tier 7**: `tier7_window.exe` window with text appears; ESC closes; PuTTY Session dialog opens
- [ ] **Stub log**: `win32compat log putty.exe` → prints top-10 stubs + writes `compat_putty_*.log`; `win32-compat.md` updated with `⚠️` status for at least 5 functions
- [ ] **CI gate**: `make compat-check` runs Tier 1–3 tests; prints score ≥ 25% after Tier 4 complete
- [ ] Commit: `"sdk: Win32 compat matrix, bring-up ladder Tier 1–8, compat_stub shmem, win32compat.exe, compat-check CI"`

---

## History

| Date | Action | Summary |
|------|--------|---------|
| 2026-04-14 | validate | Linked Inputs to D10 TODO-A/B/C as authoritative export tables; clarified Outcome + tracker checklist vs per-DLL rows. |
| 2026-04-14 | gap-analysis | Cross-TODO ownership: compat matrix stays tier gates; export inventories owned by TODO-A/B/C only. |
| 2026-08-29 | gap-filing | Added §13 Tier 9 (WinRT + DirectX + WinUI 3 apps) after evaluating `microsoft-ui-xaml` (MIT) as a UI framework: rejected as native framework, accepted as a compat tier; prerequisites inventoried with owner candidates. |
