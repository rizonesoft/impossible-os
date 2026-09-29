<!-- docs: covers=todo/10-platform-services/TODO-C-shell32-export-master-table.md sources=src/kernel/pe.c,include/kernel/env.h,src/kernel/env.c reviewed=2026-09-29 order=15 -->
# shell32 Export Master Table

## What is it?

The shell32 master table is the checklist of `shell32.dll` exports that Windows programs and installers use for paths, icons, known folders and launching files with `ShellExecute`. Each row names an export, its category, its owning roadmap, whether it is done, and notes. It holds 482 rows and none is done: there is no `shell32.dll` today. Explorer's own shell host has its gates in the [Explorer Shell Host](../desktop/explorer-shell-host.md) roadmap, which consumes this table.

## How does it work?

| Tier | Rows | What it covers |
| --- | ---: | --- |
| Tier 1 | 19 | Paths, icons and folders: `ExtractIconExA/W`, `SHGetFileInfoA/W`, `SHGetStockIconInfo`, `LoadIconA/W`, `LoadImageA/W`, `DestroyIcon`, `SHGetFolderPathA/W`, `PathCombine`, `PathAppend`, `PathFileExists` and `CommandLineToArgvW` |
| Tier 1b | 12 | `ShellExecuteA/W`, `ShellExecuteExA/W` and path helpers such as `PathIsRelative`, `PathStripPath` and `GetFullPathNameA/W` |
| Tier 2 | none | Families still to be split into exports: the folder browser, known folders v2, drag and drop, property sheets and file associations |
| Tier 3 | 451 | Every other named export, parsed from Wine's `shell32.spec` as a scaffold |

**What the columns mean.** Done is `[x]` only when a PE program can call the export and it behaves as documented, `[/]` when partial and `[ ]` when missing. Owner names the roadmap section that will implement it, or `NO_OWNING_TODO`.

**Where the code stands.**

- The PE loader's built-in import tables cover only `kernel32.dll` and `ntdll.dll` ([`pe.c`](../../src/kernel/pe.c)), so a `shell32` import resolves to 0.
- The `CommandLineToArgvW` quoting rules are already implemented in the kernel as `cmdline_to_argv()` in [`env.c`](../../src/kernel/env.c) (declared in [`env.h`](../../include/kernel/env.h)), owned by the [environment variables roadmap](../../todo/02-kernel-core/TODO-22-environment-variables.md). It is a kernel primitive, not a callable shell32 export, so its row stays open.
- The shell icon index maps (the fixed `shell32.dll` and `imageres.dll` icon numbers) are owned by section 1 of the [Win32 GDI and USER32 roadmap](../../todo/08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md).

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `C:\Windows\System32\shell32.dll` | Planned |
| Tier 1 and Tier 1b exports | Planned; 0 of 31 done |
| Tier 3 exports | Planned; 0 of 451 done |
| `cmdline_to_argv()`, the kernel decoder behind `CommandLineToArgvW` | Shipped in the kernel, not exported |

## How do I use it?

Before adding a shell32 export, find its row and give it a real owner. Two rows need care: `LoadIconA/W` also appear in the [user32 table](user32-exports.md), deliberately, and `GetFullPathNameA/W` are `kernel32.dll` exports on Windows, so a program will import them from `kernel32` rather than `shell32`. Refresh the table against a pinned Windows 11 `shell32.dll` export dump.

## What is not implemented yet?

- [Tier 1](../../todo/10-platform-services/TODO-C-shell32-export-master-table.md#tier-1----paths-icons-folder-resolution-from-todo-08-section-12), owned by [Shell and Icon API (`shell32.dll`)](../../todo/10-platform-services/TODO-08-win32-api-surface.md#12-shell--icon-api-shell32dll-sonnet)
- [Tier 1b](../../todo/10-platform-services/TODO-C-shell32-export-master-table.md#tier-1b----shell-execute-from-todo-08-section-14), owned by [Win32 Shell Integration](../../todo/10-platform-services/TODO-08-win32-api-surface.md#14-win32-shell-integration-sonnet)
- [Tier 2](../../todo/10-platform-services/TODO-C-shell32-export-master-table.md#tier-2----shell-roadmap-not-pe-names) and [Tier 3](../../todo/10-platform-services/TODO-C-shell32-export-master-table.md#tier-3----full-named-export-roster-alphabetical-wine-scaffold)

## How does it compare with Windows 11 and Linux?

On Windows 11, `shell32` provides the `SH*` path and folder APIs, `ShellExecute`, icon extraction and the shell's icon index maps, and Explorer is built on it. Linux desktops use XDG user directories, `.desktop` files and `xdg-open` for the same jobs, and Wine reimplements `shell32` for Windows programs. The Impossible OS plan keeps one owned row per export, with `ShellExecute` routed to its own file association engine.

## See also

- [shell32.dll Export Master Table](../../todo/10-platform-services/TODO-C-shell32-export-master-table.md)
- [Explorer Shell Host](../desktop/explorer-shell-host.md)
- [File Associations, Shortcuts and System Resources](../desktop/file-associations.md)
- [Environment Variables](../kernel/environment-variables.md)
- [Win32 API Surface](win32-api-surface.md)
- [user32](user32-exports.md) and [comctl32](comctl32-exports.md) master tables
