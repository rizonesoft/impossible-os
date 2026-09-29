<!-- docs: covers=todo/10-platform-services/TODO-08-win32-api-surface.md sources=src/kernel/pe.c,include/kernel/nt/service_numbers.h,include/registry.h,user/include/win32.h,user/lib/win32.c,user/test/test_win32.c,sdk/include/impossible/windows.h,include/kernel/ob/teb.h reviewed=2026-09-29 order=8 -->
# Win32 API Surface

## What is it?

The Win32 API surface is the set of Windows DLLs and functions (`kernel32`, `msvcrt`, `ntdll`, `user32`, `gdi32`, `shell32`) that a program built with MinGW expects to find, plus the native IxUI toolkit, shell integration and a developer SDK. The goal is that unmodified console and GUI programs run. None of its fifteen sections has shipped. Today the kernel holds two small import tables that bind names to system calls, and user mode has a five-function test shim; there is no DLL a program can call.

## How does it work?

**Today.**

- **Kernel import tables.** The PE loader knows two DLLs, and only by name ([`pe.c`](../../src/kernel/pe.c)). `kernel32.dll` lists 14 names (`CloseHandle`, `CreateFileA`, `CreateFileW`, `ExitProcess`, `GetLastError`, `SetLastError`, `ReadFile`, `WriteFile` and the six firmware-table and firmware-variable calls), each mapped to the native service it will eventually call, such as `CreateFileA` to `NtCreateFile`. `ntdll.dll` lists 96 `Nt*` names mapped one to one to their SSDT slots. The table is kept sorted because lookup is a binary search, and a boot-time test checks the order. These rows reserve the mapping only: the user-mode wrappers that would convert ANSI to Unicode, map error codes and call the service are not written, so an import resolves to a service number rather than to code. See [Win32 PE Loader](win32-pe-loader.md).
- **Native services.** The main SSDT defines 477 services ([`service_numbers.h`](../../include/kernel/nt/service_numbers.h)); see [Native API and the SSDT](../kernel/native-api-ssdt.md). The kernel Registry already uses Win32 names (`RegOpenKeyEx`, `RegQueryValueEx`, `RegSetValueEx` and others, [`registry.h`](../../include/registry.h)), so the planned `advapi32`-style calls are pass-throughs.
- **Per-thread error code.** The TEB holds `LastErrorValue` at offset `0x68`, where `GetLastError()` will read it ([`teb.h`](../../include/kernel/ob/teb.h)).
- **User-mode test shim.** [`win32.h`](../../user/include/win32.h) and [`win32.c`](../../user/lib/win32.c) implement `GetCurrentProcessId`, `GetTickCount`, `CreateFileA`, `ReadFile` and `CloseHandle` for ELF test programs. `GetCurrentProcessId` reads the process ID from the TEB at `gs:0x40`, `GetTickCount` reads the tick count from `KUSER_SHARED_DATA` at `0x7FFE0000` (falling back to `sys_uptime()`), and the three file calls go through `INT 0x80` to `sys_openfile`, `sys_readhandle` and `sys_closehandle`. It is deliberately not a `kernel32` and is exercised by [`test_win32.c`](../../user/test/test_win32.c).
- **SDK header.** [`windows.h`](../../sdk/include/impossible/windows.h) declares the file API (`CreateFile`, `ReadFile`, `WriteFile`, `FindFirstFile` and more) for the SDK, but `sdk/lib/` has no library behind it yet.

**Planned design.** Win32 types first (`HANDLE`, `DWORD`, `HWND`, `WINAPI` as the Microsoft x64 calling convention), then `kernel32` tier 1 (console, process, file, environment and Registry calls), `msvcrt`, `ntdll` stubs, the memory API (`VirtualAlloc`, `HeapAlloc`), the synchronisation API over the kernel's mutexes, semaphores and events, `LoadLibrary` and `GetProcAddress`, the error API, and a logger that records every call to an unimplemented export. On top of that: `user32` and `gdi32` routed to the window manager and graphics library, `shell32`, the IxUI toolkit, `ShellExecute` and the SDK. The export-by-export inventories for `user32`, `comctl32` and `shell32` live in their master tables.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `kernel32.dll` (14 names) and `ntdll.dll` (96 names) import tables | Shipped; resolve to service numbers |
| `RegOpenKeyEx`, `RegQueryValueEx`, `RegSetValueEx` and related kernel calls | Shipped in the kernel |
| Five-function user-mode shim in `user/lib/win32.c` | Shipped, for tests |
| `sdk/include/impossible/windows.h` file API | Declared, no library |
| `kernel32`, `msvcrt`, `ntdll`, `user32`, `gdi32`, `shell32` DLLs | Planned |
| IxUI (`sdk/include/ixui.h`, `libixui.a`), `impossible-cc`, import libraries | Planned |

## How do I use it?

Only the test shim can be used: link an ELF test program against `user/libc.a` and include `win32.h`. `make test-exec` and `make test-ob` check the kernel import tables.

## What is not implemented yet?

- [Win32 Type Definitions](../../todo/10-platform-services/TODO-08-win32-api-surface.md#1-win32-type-definitions-sonnet)
- [Console and Process API](../../todo/10-platform-services/TODO-08-win32-api-surface.md#2-console--process-api-kernel32dll-tier-1-sonnet), including the user-mode wrappers behind the firmware rows of the `kernel32` table
- [C Runtime](../../todo/10-platform-services/TODO-08-win32-api-surface.md#3-c-runtime-msvcrtdll-sonnet) and [NT Runtime Stubs](../../todo/10-platform-services/TODO-08-win32-api-surface.md#4-nt-runtime-stubs-ntdlldll-sonnet)
- [Memory Management API](../../todo/10-platform-services/TODO-08-win32-api-surface.md#5-memory-management-api-sonnet), [Synchronization API](../../todo/10-platform-services/TODO-08-win32-api-surface.md#6-synchronization-api-sonnet), [DLL Loading API](../../todo/10-platform-services/TODO-08-win32-api-surface.md#7-dll-loading-api-opus) and [Error API](../../todo/10-platform-services/TODO-08-win32-api-surface.md#8-error-api-sonnet)
- [Unimplemented Function Logger](../../todo/10-platform-services/TODO-08-win32-api-surface.md#9-unimplemented-function-logger-sonnet)
- [`user32`](../../todo/10-platform-services/TODO-08-win32-api-surface.md#10-window-management-user32dll-sonnet), [`gdi32`](../../todo/10-platform-services/TODO-08-win32-api-surface.md#11-gdi-rendering-gdi32dll-sonnet) and [`shell32`](../../todo/10-platform-services/TODO-08-win32-api-surface.md#12-shell--icon-api-shell32dll-sonnet)
- [IxUI Native Toolkit](../../todo/10-platform-services/TODO-08-win32-api-surface.md#13-ixui-native-toolkit-opus), [Win32 Shell Integration](../../todo/10-platform-services/TODO-08-win32-api-surface.md#14-win32-shell-integration-sonnet) and the [Developer SDK](../../todo/10-platform-services/TODO-08-win32-api-surface.md#15-developer-sdk-sonnet)

## How does it compare with Windows 11 and Linux?

Windows 11 implements these DLLs in user mode over `ntdll` and the kernel's system calls, and `user32` and `gdi32` over `win32k`. On Linux the same programs run through Wine, which reimplements the DLLs in user space on top of POSIX and X11 or Wayland. The Impossible OS plan maps the Win32 names onto its own native services with no translation layer, and adds a logger that counts calls to unimplemented functions. Today only the name-to-service tables exist.

## See also

- [Win32 API Surface Completion roadmap](../../todo/10-platform-services/TODO-08-win32-api-surface.md)
- [Win32 PE Loader](win32-pe-loader.md)
- [user32](user32-exports.md), [comctl32](comctl32-exports.md) and [shell32](shell32-exports.md) export master tables
- [Win32 GDI and USER32 Desktop API](../graphics/win32-gdi-user32.md)
- [Win32 File I/O](../storage/win32-file-io.md)
- [Compiler and SDK](compiler-sdk.md)
