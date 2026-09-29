<!-- docs: covers=todo/10-platform-services/TODO-A-user32-export-master-table.md sources=src/kernel/pe.c,include/kernel/nt/ssdt.h,src/kernel/nt/ssdt.c reviewed=2026-09-29 order=13 -->
# user32 Export Master Table

## What is it?

The user32 master table is the single checklist of every `user32.dll` export Impossible OS has to provide for Windows programs that make windows, pump messages and draw controls. Each row names an export, its category, the roadmap that owns it, whether it is done, and notes. It holds 1,114 rows and none is done: there is no `user32.dll` today. The table plans names and owners only; the NtUser system call numbers behind them live in the [Win32k Shadow SSDT Master Table](../graphics/win32k-shadow-master-table.md).

## How does it work?

The table is split into tiers so the work can be sequenced without losing sight of completeness:

| Tier | Rows | What it covers |
| --- | ---: | --- |
| Tier 1 | 23 | The core window class and message pump: `RegisterClassExA/W`, `CreateWindowExA/W`, `ShowWindow`, `DestroyWindow`, `GetMessageA/W`, `TranslateMessage`, `DispatchMessageA/W`, `PostMessage`, `SendMessage`, `PostQuitMessage` and similar |
| Tier 2 | 11 | The stub suite from the Win32 GDI and USER32 roadmap: `GetDC` and `ReleaseDC`, cursor and icon loading, `GetSystemMetrics`, `InvalidateRect`, and the open, save and colour dialogs |
| Tier 3 | 1,080 | Every other named export, parsed from Wine's `user32.spec` as a scaffold |

A further 54 exports in the Wine spec have no name, only an ordinal. They are counted in the methodology section as a bucket, not as rows, to be listed when a real Windows 11 `user32.dll` is dumped.

**What the columns mean.** Done is `[x]` only when a PE program can call the export and it behaves as documented, `[/]` when partial and `[ ]` when missing. Owner is the roadmap section that will implement it, or `NO_OWNING_TODO` when no section has claimed it yet.

**Where the code stands.** The kernel's PE loader knows only `kernel32.dll` and `ntdll.dll` ([`pe.c`](../../src/kernel/pe.c)), so a program that imports `user32.dll` gets every thunk set to 0. The shadow SSDT that `user32` calls will enter exists as a 1,024-slot table at indices `0x1000` to `0x13FF` ([`ssdt.h`](../../include/kernel/nt/ssdt.h), [`ssdt.c`](../../src/kernel/nt/ssdt.c)), with no services behind it yet.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `C:\Windows\System32\user32.dll` (the published import name) | Planned |
| Tier 1 and Tier 2 exports | Planned; 0 of 34 done |
| Tier 3 exports | Planned; 0 of 1,080 done |
| Shadow SSDT, `0x1000` to `0x13FF` | Table present, empty |

## How do I use it?

Before implementing a `user32` export, find its row, make sure its Owner names a real open section (open one in the owning roadmap if it says `NO_OWNING_TODO`), and flip Done only when a PE test calls it. When refreshing the table, dump the export directory of a pinned Windows 11 build with `llvm-readobj --coff-exports` or `dumpbin /exports` and record the new, renamed and ordinal-only exports against the Wine scaffold.

## What is not implemented yet?

Every row. The tiers are worked through their owners:

- [Tier 1](../../todo/10-platform-services/TODO-A-user32-export-master-table.md#tier-1----core-window-class-and-message-pump-from-todo-08-section-10), owned by [Window Management (`user32.dll`)](../../todo/10-platform-services/TODO-08-win32-api-surface.md#10-window-management-user32dll-sonnet) and the [Win32 GDI and USER32 roadmap](../../todo/08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md)
- [Tier 2](../../todo/10-platform-services/TODO-A-user32-export-master-table.md#tier-2----todo-14-stub-suite-extend-rows-as-sections-2-to-8-land), owned by sections 2 to 8 of the Win32 GDI and USER32 roadmap
- [Tier 3](../../todo/10-platform-services/TODO-A-user32-export-master-table.md#tier-3----full-named-export-roster-alphabetical-wine-scaffold), mostly `NO_OWNING_TODO`
- A dump of a pinned Windows 11 `user32.dll` to reconcile against the Wine scaffold ([Implementation Order](../../todo/10-platform-services/TODO-A-user32-export-master-table.md#implementation-order) step 3)

## How does it compare with Windows 11 and Linux?

Windows 11 documents `user32` through the SDK headers and Microsoft Learn, with no single list of exports; the export directory of the DLL is the only complete inventory. Linux has no `user32`; Wine reimplements it for Windows programs, and its spec file is the scaffold here. Impossible OS keeps an explicit, owned row for every export so progress can be counted.

## See also

- [user32.dll Export Master Table](../../todo/10-platform-services/TODO-A-user32-export-master-table.md)
- [Win32 API Surface](win32-api-surface.md)
- [Win32 GDI and USER32 Desktop API](../graphics/win32-gdi-user32.md)
- [Win32k Shadow SSDT Master Table](../graphics/win32k-shadow-master-table.md)
- [comctl32](comctl32-exports.md) and [shell32](shell32-exports.md) master tables
