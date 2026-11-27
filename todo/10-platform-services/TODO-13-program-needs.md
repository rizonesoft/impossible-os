---
schema_version: 1
id: program-needs
domain: 10-platform-services
status: active
title: "TODO-13 -- What Programs Need of This OS"
---

# TODO-13 -- What Programs Need of This OS

> **Goal:** Whatever a program built beside this repository needs of Impossible OS, a Windows function, an engine or a service, is a section here or in the file that owns it, from the day it is declared.

> [!NOTE]
> **Grows by itself.** `python3 scripts/imports.py --needs` names what no section answers, and `--file-needs` writes a section here for it: its items, its row below, its place in the plan and its row in `todo/imports.json`. Lint Check 42 refuses a commit while something is unanswered. The program never waits for it. A section may be answered in the file that owns the subject instead: the line `**Need:** <id>` and the ledger row then point there.

## Inputs

- `abi/tiers.json` of Impossible SDK: every Windows function a program may call, by tier. It grows as programs are built
- `app.json` of each program: what else it needs
- [`TODO-08-win32-api-surface.md`](TODO-08-win32-api-surface.md), [`TODO-07-win32-pe-loader.md`](TODO-07-win32-pe-loader.md): where most functions are built
- `impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md`: the GPU tier, its `resolved` and `methods` records (§7 to §9)
- -> XREF: `11-apps/TODO-14-inkeys.md` §3, §5 and `12-user-platform-sdk/TODO-07-win32-compat-matrix.md` §13: Inkeys' Direct3D 11 needs are answered by §7 and §8, never by a second list
- -> XREF: `11-apps/TODO-16-office.md`, `11-apps/TODO-17-isotone.md` §5: the programs that call the tier
- `impossible-sdk` `todo/09-platform/TODO-03-gpu.md` -- the SDK's GPU tier, the owner of the list that §7 to §9 implement

## Outcome

- A program whose tier is complete here runs.
- Nothing a program needs is known only to the program.

## Implementation Order

| ⭐  | Order | Deliverable                                              | Depends On                                                     | Status |
| --- | :---: | -------------------------------------------------------- | -------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Tier 0 functions: console                             | `TODO-07` §7 (PE programs run), `TODO-08` §2, §5, §6, §7       |  [ ]   |
| 💎  |   2   | §2 Tier 1 functions: window                              | §1, `TODO-08` §10, §11, `08-graphics-ui/TODO-14` §5, §6        |  [ ]   |
| 💎  |   3   | §3 Tier 2 functions: documents                           | §2, `TODO-08` §12, `09-desktop-shell/TODO-01` §3               |  [ ]   |
| 💎  |   4   | §4 Tier 3 functions: media                               | §1, `TODO-01` §1, §2                                           |  [ ]   |
| 💎  |   5   | §5 Tier 4 functions: network                             | §1, `07-networking/TODO-01` §3, `07-networking/TODO-02` §5, §8 |  [ ]   |
| 💎  |   6   | §6 Tier 5 functions: desktop                             | §2, `08-graphics-ui/TODO-09` §5, `08-graphics-ui/TODO-08` §8   |  [ ]   |
| 💎  |   7   | §7 GPU tier functions: Direct3D 11 and DXGI              | §1, §2, `TODO-07` §7 (PE programs run)                         |  [ ]   |
| 💎  |   8   | §8 GPU tier methods: Direct3D 11 by interface            | §7                                                             |  [ ]   |
| ⭐  |   9   | §9 The GPU tier in `--needs`: resolved names and methods | --                                                             |  [ ]   |

---

## 1. Tier 0 Functions: Console

> **Spawned-by:** root

**Design:** n/a -- functions, they draw nothing

**Need:** impossible-sdk-tier-0

- [ ] Name what each stands on (a driver, a protocol, an engine of `08-graphics-ui/TODO-18`, a file system), and the section that builds it; add it to this row's dependencies, and file a section where there is none
- [ ] `kernel32.dll`: `AcquireSRWLockExclusive`, `CloseHandle`, `CreateDirectoryW`, `CreateEventW`, `CreateFileW`, `CreateThread`, `DeleteFileW`, `ExitProcess` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FlushFileBuffers`, `GetCommandLineW`, `GetConsoleMode`, `GetEnvironmentVariableW`, `GetFileAttributesW` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `GetFileSizeEx`, `GetLastError`, `GetModuleFileNameW`, `GetModuleHandleW`, `GetProcAddress`, `GetProcessHeap`, `GetStdHandle`, `GetSystemInfo` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `GetSystemTimeAsFileTime`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `LoadLibraryW`, `MoveFileExW`, `QueryPerformanceCounter`, `QueryPerformanceFrequency` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `ReadConsoleW`, `ReadFile`, `ReleaseSRWLockExclusive`, `SetEndOfFile`, `SetEvent`, `SetFilePointerEx`, `Sleep`, `VirtualAlloc` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `VirtualFree`, `WaitForSingleObject`, `WriteConsoleW`, `WriteFile`, `GetCurrentDirectoryW` act as on Windows, whichever section builds each
- [ ] Run Impossible SDK's test of each on this OS and on Windows, and compare the results and the errors
- [ ] Commit: `"win32: tier 0 of Impossible SDK"`

## 2. Tier 1 Functions: Window

> **Spawned-by:** root

**Design:** n/a -- functions, they draw nothing

**Need:** impossible-sdk-tier-1

- [ ] Name what each stands on (a driver, a protocol, an engine of `08-graphics-ui/TODO-18`, a file system), and the section that builds it; add it to this row's dependencies, and file a section where there is none
- [ ] `user32.dll`: `BeginPaint`, `CreateWindowExW`, `DefWindowProcW`, `DestroyWindow`, `DispatchMessageW`, `EndPaint`, `GetClientRect`, `GetCursorPos` act as on Windows, whichever section builds each
- [ ] `user32.dll`: `GetDpiForWindow`, `GetKeyState`, `GetMessageW`, `GetMonitorInfoW`, `GetSystemMetrics`, `GetWindowLongPtrW`, `GetWindowRect`, `InvalidateRect` act as on Windows, whichever section builds each
- [ ] `user32.dll`: `KillTimer`, `LoadCursorW`, `MonitorFromWindow`, `PeekMessageW`, `PostMessageW`, `PostQuitMessage`, `RegisterClassExW`, `ReleaseCapture` act as on Windows, whichever section builds each
- [ ] `user32.dll`: `ScreenToClient`, `SetCapture`, `SetCursor`, `SetTimer`, `SetWindowLongPtrW`, `SetWindowPos`, `SetWindowTextW`, `ShowWindow` act as on Windows, whichever section builds each
- [ ] `user32.dll`: `TranslateMessage` act as on Windows, whichever section builds each
- [ ] `gdi32.dll`: `SetDIBitsToDevice` act as on Windows, whichever section builds each
- [ ] Run Impossible SDK's test of each on this OS and on Windows, and compare the results and the errors
- [ ] Commit: `"win32: tier 1 of Impossible SDK"`

## 3. Tier 2 Functions: Documents

> **Spawned-by:** root

**Design:** n/a -- functions, they draw nothing

**Need:** impossible-sdk-tier-2

- [ ] Name what each stands on (a driver, a protocol, an engine of `08-graphics-ui/TODO-18`, a file system), and the section that builds it; add it to this row's dependencies, and file a section where there is none
- [ ] `user32.dll`: `CloseClipboard`, `EmptyClipboard`, `GetClipboardData`, `OpenClipboard`, `SetClipboardData` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `CreateProcessW`, `GlobalAlloc`, `GlobalLock`, `GlobalUnlock`, `ReadDirectoryChangesW` act as on Windows, whichever section builds each
- [ ] `shell32.dll`: `DragAcceptFiles`, `DragFinish`, `DragQueryFileW`, `ShellExecuteW` act as on Windows, whichever section builds each
- [ ] Run Impossible SDK's test of each on this OS and on Windows, and compare the results and the errors
- [ ] Commit: `"win32: tier 2 of Impossible SDK"`

## 4. Tier 3 Functions: Media

> **Spawned-by:** root

**Design:** n/a -- functions, they draw nothing

**Need:** impossible-sdk-tier-3

- [ ] Name what each stands on (a driver, a protocol, an engine of `08-graphics-ui/TODO-18`, a file system), and the section that builds it; add it to this row's dependencies, and file a section where there is none
- [ ] `winmm.dll`: `waveInAddBuffer`, `waveInClose`, `waveInOpen`, `waveInPrepareHeader`, `waveInReset`, `waveInStart`, `waveInUnprepareHeader`, `waveOutClose` act as on Windows, whichever section builds each
- [ ] `winmm.dll`: `waveOutOpen`, `waveOutPrepareHeader`, `waveOutReset`, `waveOutUnprepareHeader`, `waveOutWrite` act as on Windows, whichever section builds each
- [ ] Run Impossible SDK's test of each on this OS and on Windows, and compare the results and the errors
- [ ] Commit: `"win32: tier 3 of Impossible SDK"`

## 5. Tier 4 Functions: Network

> **Spawned-by:** root

**Design:** n/a -- functions, they draw nothing

**Need:** impossible-sdk-tier-4

- [ ] Name what each stands on (a driver, a protocol, an engine of `08-graphics-ui/TODO-18`, a file system), and the section that builds it; add it to this row's dependencies, and file a section where there is none
- [ ] `ws2_32.dll`: `WSACleanup`, `WSAGetLastError`, `WSAStartup`, `accept`, `bind`, `closesocket`, `connect`, `freeaddrinfo` act as on Windows, whichever section builds each
- [ ] `ws2_32.dll`: `getaddrinfo`, `ioctlsocket`, `listen`, `recv`, `recvfrom`, `select`, `send`, `sendto` act as on Windows, whichever section builds each
- [ ] `ws2_32.dll`: `setsockopt`, `shutdown`, `socket` act as on Windows, whichever section builds each
- [ ] `bcrypt.dll`: `BCryptGenRandom` act as on Windows, whichever section builds each
- [ ] `crypt32.dll`: `CryptProtectData`, `CryptUnprotectData` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `LocalFree` act as on Windows, whichever section builds each
- [ ] Run Impossible SDK's test of each on this OS and on Windows, and compare the results and the errors
- [ ] Commit: `"win32: tier 4 of Impossible SDK"`

## 6. Tier 5 Functions: Desktop

> **Spawned-by:** root

**Design:** n/a -- functions, they draw nothing

**Need:** impossible-sdk-tier-5

- [ ] Name what each stands on (a driver, a protocol, an engine of `08-graphics-ui/TODO-18`, a file system), and the section that builds it; add it to this row's dependencies, and file a section where there is none
- [ ] `user32.dll`: `EnumDisplayMonitors`, `EnumWindows`, `GetDC`, `GetForegroundWindow`, `GetKeyboardLayout`, `GetWindowTextW`, `GetWindowThreadProcessId`, `IsWindowVisible` act as on Windows, whichever section builds each
- [ ] `user32.dll`: `MapVirtualKeyExW`, `PrintWindow`, `RegisterHotKey`, `ReleaseDC`, `SendInput`, `SetLayeredWindowAttributes`, `SetWindowDisplayAffinity`, `ToUnicodeEx` act as on Windows, whichever section builds each
- [ ] `user32.dll`: `UnregisterHotKey` act as on Windows, whichever section builds each
- [ ] `gdi32.dll`: `BitBlt`, `CreateCompatibleDC`, `CreateDIBSection`, `DeleteDC`, `DeleteObject`, `SelectObject` act as on Windows, whichever section builds each
- [ ] `dwmapi.dll`: `DwmGetWindowAttribute` act as on Windows, whichever section builds each
- [ ] `kernel32.dll`: `DeviceIoControl` act as on Windows, whichever section builds each
- [ ] Run Impossible SDK's test of each on this OS and on Windows, and compare the results and the errors
- [ ] Commit: `"win32: tier 5 of Impossible SDK"`

## 7. GPU Tier Functions: Direct3D 11 and DXGI

> **Spawned-by:** root

**Design:** n/a -- functions, they draw nothing

Impossible SDK gets a GPU tier after Desktop (the approved design `impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md`). Its `abi/tiers.json` lists the Direct3D 11 and DXGI functions, the names looked up at run time (`resolved`) and the COM methods by interface (`methods`) that a program may use. `abi/tiers.json` does not list the tier yet; its number and exact lists are set by two workloads, Recto's page tiles and a layered canvas sample shaped like Isotone's, and they are shared with what Inkeys needs (`11-apps/TODO-14-inkeys.md` §3 and §5, `12-user-platform-sdk/TODO-07-win32-compat-matrix.md` §13). Programs never touch `d3d11.dll` or `dxgi.dll` directly: the SDK's `gpu` part loads them at run time with `LoadLibraryW` and `GetProcAddress`, so a program still starts on a system that lacks them. This section makes Impossible OS provide the listed functions and names, and nothing the list does not hold. Impossible OS implements the listed calls and never edits the list.

- [ ] Name what each stands on (the software rasterizer below, the window surface of `08-graphics-ui/TODO-14`, a GPU driver once one exists), and the section that builds it; add it to this row's dependencies, and file a section where there is none
- [ ] Each listed function of `d3d11.dll` and `dxgi.dll` acts as on Windows, whichever section builds it, so `LoadLibraryW` finds both and `GetProcAddress` resolves every name of the tier's `resolved` record
- [ ] Failure at the start-up boundary is clean and never a crash, in the three cases the SDK tests
  - `LoadLibraryW` of either DLL fails with its error when the OS has none
  - `GetProcAddress` of a missing export returns NULL with `ERROR_PROC_NOT_FOUND`
  - Device creation fails with Windows' status when no adapter offers the required feature level
- [ ] A software rasterizer device (a WARP-equivalent) answers device and swap chain creation until a GPU driver exists, and reports the feature level it really has
- [ ] The Direct3D 11 entry points that Inkeys' `d3d11_canvas.c` probe needs are answered here, from this one list: the §2 inventory of `11-apps/TODO-14-inkeys.md` points at this section instead of keeping its own
- [ ] Run Impossible SDK's GPU sample on this OS and on Windows through its three start-up failure cases (DLL cannot be loaded, export missing, no adapter) and compare the results and the errors
- [ ] When `abi/tiers.json` lists the tier, give this section the line `**Need:** impossible-sdk-tier-<n>` and its row under `needs` in `todo/imports.json` in the same commit, so `--needs` finds its answers here and files no second section
- [ ] Commit: `"win32: GPU tier functions of Impossible SDK"`

## 8. GPU Tier Methods: Direct3D 11 by Interface

> **Spawned-by:** root

**Design:** n/a -- COM methods, they draw nothing

The `methods` record of the GPU tier lists COM interface methods by interface: device and device context, swap chain, buffers, textures, views, compiled shaders and the draw calls. A COM method is an indirect call that names nothing in an import table, so the list is the only place Impossible OS can learn what to implement. The SDK's `gpu` part reaches Direct3D only through a generated header that declares the listed methods and no others.

- [ ] Each listed method of each listed interface acts as on Windows through its COM virtual table, whichever section builds it; an interface method that is not listed is not built for this tier
- [ ] Shaders arrive compiled at build time in the bytecode format the tier fixes; the software rasterizer of §7 runs that bytecode, and a program never needs a shader compiler on this OS
- [ ] Present puts a swap chain's back buffer on the window's surface through the compositor, and resizing the swap chain keeps its resources valid
- [ ] A lost device is reported on the calling thread by any call that can see it (presenting, resizing the swap chain, creating a resource); the device and everything made on it are then invalid, and creating a new device works
  - Test by removing the device while a canvas is open, once at presentation and once during a resize: the canvas must keep drawing, on a new device or on its CPU path
- [ ] The Direct3D 11 methods that Inkeys' `d3d11_canvas.c` probe calls are among these, and it passes on exactly them (`11-apps/TODO-14-inkeys.md` §3)
  - Direct2D and DirectComposition stay owned by `12-user-platform-sdk/TODO-07-win32-compat-matrix.md` §13
- [ ] Run Impossible SDK's layered canvas sample on this OS and on Windows, compare the pictures within the sample's tolerance, and keep the timings separate
- [ ] When `abi/tiers.json` lists the tier, give this section the line `**Need:** impossible-sdk-tier-<n>-methods` and its row under `needs` in `todo/imports.json` in the same commit
- [ ] Commit: `"win32: GPU tier methods of Impossible SDK"`

## 9. The GPU Tier in `--needs`: Resolved Names and Methods

> **Spawned-by:** root

**Design:** n/a -- a script change, it draws nothing

`python3 scripts/imports.py --needs` reads only the `functions` of each tier of `abi/tiers.json` (`unanswered()`), so a name that is looked up at run time or a COM method is invisible to it. The approved design makes this the change that belongs to the OS import section of the GPU tier: read the two new kinds, and report a method nobody provides under its interface.

- [ ] Read `resolved` and `methods` from each tier in the shape the SDK's `abi/tiers.json` gives them at the pinned commit, and adopt that shape without defining one; a tier that has neither (every tier today) reads as none
- [ ] Teach `functions_of()` the item form for a method: an item whose first backticked name is an interface (`ID3D11Device`) followed by method names answers each (interface, method) pair, as `` `kernel32.dll`: `f` `` answers a function
- [ ] `--needs` reports a `resolved` name that no item answers like an unanswered function, and a method that no item answers under its interface, one line per interface: `impossible-sdk needs: ID3D11Device: CreateBuffer, CreateTexture2D`
- [ ] `--file-needs` writes a section for them with the same layout as for functions, one item per interface
- [ ] Add tests to `scripts/tests/test_imports.py`, each failing before the change, over a fixture `tiers.json` with `resolved` and `methods`
  - An unprovided method is reported under its interface; a provided method is not reported
  - An unanswered `resolved` name is reported; a tier without the records reports as before
- [ ] Describe the two new kinds in `docs/services/program-needs.md`
- [ ] Commit: `"imports: --needs reads resolved names and methods of a tier"`

**Test checkpoint:** `python3 scripts/tests/test_imports.py` passes with the new cases; with a fixture tier listing `ID3D11Device` `CreateBuffer` and no item answering it, `python3 scripts/imports.py --needs` prints `impossible-sdk needs: ID3D11Device: CreateBuffer` and exits 1; adding the item makes it print nothing. Test on: WSL2 dev host.

---

## OS Comparison

| ⭐  | Feature                      | 🪟 Win11  | 🐧 Linux | 🚀 Impossible OS    |
| --- | ---------------------------- | --------- | -------- | ------------------- |
| 💎  | Win32 functions for programs | ✅ Native | ⚠️ Wine  | ⬜ Planned -- §1-§9 |
| ⭐  | Every program's needs listed | ❌        | ❌       | ⬜ Planned -- §1-§9 |

## Verification

- [ ] `python3 scripts/imports.py --needs` prints nothing
- [ ] The samples of Impossible SDK for each tier run on this OS
