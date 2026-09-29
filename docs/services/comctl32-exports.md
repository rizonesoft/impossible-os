<!-- docs: covers=todo/10-platform-services/TODO-B-comctl32-export-master-table.md sources=src/kernel/pe.c,include/desktop/controls.h reviewed=2026-09-29 order=14 -->
# comctl32 Export Master Table

## What is it?

The comctl32 master table is the checklist of `comctl32.dll` exports: the Windows common controls library behind list views, tree views, image lists, progress bars, property sheets and the version entry points installers probe. Each row names an export, its category, its owning roadmap, whether it is done, and notes. It holds 164 rows and none is done: there is no `comctl32.dll` today, and the native control library has only four control types.

## How does it work?

| Tier | Rows | What it covers |
| --- | ---: | --- |
| Tier 1 | 3 | `InitCommonControls`, `InitCommonControlsEx` and `DllGetVersion`, the calls a program makes before using common controls |
| Tier 2 | none | Three families to be split into real exports: image lists, list views and tree views, each mapped to a native control |
| Tier 3 | 161 | Every other named export, parsed from Wine's `comctl32.spec` as a scaffold |

**What the columns mean.** Done is `[x]` only when a PE program can call the export and it behaves as documented, `[/]` when partial and `[ ]` when missing. Owner names the roadmap that will implement it, or `NO_OWNING_TODO` when none has claimed it.

**The native side.** A comctl32 export will usually be a thin wrapper over a native control. Today [`controls.h`](../../include/desktop/controls.h) defines buttons, labels, text boxes and scroll bars only; the list view, tree view and tab controls the Tier 2 families map to are planned in the [widget dialogs roadmap](../../todo/08-graphics-ui/TODO-06-widget-dialogs.md). The PE loader's built-in import tables cover only `kernel32.dll` and `ntdll.dll` ([`pe.c`](../../src/kernel/pe.c)), so a `comctl32` import resolves to 0.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `C:\Windows\System32\comctl32.dll` | Planned |
| `InitCommonControls`, `InitCommonControlsEx`, `DllGetVersion` | Planned; 0 of 3 done |
| `ImageList_*`, `ListView_*`, `TreeView_*` families | Planned |
| Tier 3 exports | Planned; 0 of 161 done |

## How do I use it?

Before adding a comctl32 export, find its row, confirm the native control it wraps exists, and give the row a real owner. Refresh the table against a pinned Windows 11 `comctl32.dll` export dump; note that Windows ships two side-by-side versions (5.x and 6.x, selected by an application manifest).

## What is not implemented yet?

- [Tier 1](../../todo/10-platform-services/TODO-B-comctl32-export-master-table.md#tier-1----init-and-version), owned by the [Win32 API Surface](../../todo/10-platform-services/TODO-08-win32-api-surface.md) roadmap and the PE loader's built-in tables
- [Tier 2](../../todo/10-platform-services/TODO-B-comctl32-export-master-table.md#tier-2----control-class-roadmap-ixui-mapping-not-pe-names), which waits on the [ListView](../../todo/08-graphics-ui/TODO-06-widget-dialogs.md#1-listview-opus) and [TreeView](../../todo/08-graphics-ui/TODO-06-widget-dialogs.md#2-treeview-opus) controls
- [Tier 3](../../todo/10-platform-services/TODO-B-comctl32-export-master-table.md#tier-3----full-named-export-roster-alphabetical-wine-scaffold)

## How does it compare with Windows 11 and Linux?

Windows 11 ships comctl32 version 6 with visual styles, activated through an application manifest, alongside the older version 5. Linux programs use GTK or Qt widget sets instead, and Wine reimplements comctl32 for Windows programs. The Impossible OS plan maps common controls onto its own `CTRL_*` widgets rather than a second control implementation.

## See also

- [comctl32.dll Export Master Table](../../todo/10-platform-services/TODO-B-comctl32-export-master-table.md)
- [Complex Controls and Dialogs](../graphics/complex-controls-dialogs.md)
- [Control Library](../desktop/control-library.md)
- [Win32 API Surface](win32-api-surface.md)
- [user32](user32-exports.md) and [shell32](shell32-exports.md) master tables
