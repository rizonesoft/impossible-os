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

## Outcome

- A program whose tier is complete here runs.
- Nothing a program needs is known only to the program.

## Implementation Order

| ⭐  | Order | Deliverable                    | Depends On                                                     | Status |
| --- | :---: | ------------------------------ | -------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Tier 0 functions: console   | `TODO-07` §7 (PE programs run), `TODO-08` §2, §5, §6, §7       |  [ ]   |
| 💎  |   2   | §2 Tier 1 functions: window    | §1, `TODO-08` §10, §11, `08-graphics-ui/TODO-14` §5, §6        |  [ ]   |
| 💎  |   3   | §3 Tier 2 functions: documents | §2, `TODO-08` §12, `09-desktop-shell/TODO-01` §3               |  [ ]   |
| 💎  |   4   | §4 Tier 3 functions: media     | §1, `TODO-01` §1, §2                                           |  [ ]   |
| 💎  |   5   | §5 Tier 4 functions: network   | §1, `07-networking/TODO-01` §3, `07-networking/TODO-02` §5, §8 |  [ ]   |
| 💎  |   6   | §6 Tier 5 functions: desktop   | §2, `08-graphics-ui/TODO-09` §5, `08-graphics-ui/TODO-08` §8   |  [ ]   |

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

---

## OS Comparison

| ⭐  | Feature                      | 🪟 Win11  | 🐧 Linux | 🚀 Impossible OS    |
| --- | ---------------------------- | --------- | -------- | ------------------- |
| 💎  | Win32 functions for programs | ✅ Native | ⚠️ Wine  | ⬜ Planned -- §1-§6 |
| ⭐  | Every program's needs listed | ❌        | ❌       | ⬜ Planned -- §1-§6 |

## Verification

- [ ] `python3 scripts/imports.py --needs` prints nothing
- [ ] The samples of Impossible SDK for each tier run on this OS
