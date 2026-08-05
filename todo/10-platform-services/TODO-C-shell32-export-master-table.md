---
schema_version: 1
id: shell32-export-master-table
domain: 10-platform-services
status: active
title: "TODO-C -- shell32.dll Export Master Table"
---

# TODO-C -- shell32.dll Export Master Table

> **Goal:** Authoritative checklist of **shell32.dll** exports used for paths, icons, folders, and **ShellExecute**. Implementation files stay in `TODO-08-win32-api-surface.md` Sections 12 and 14. **Shell icon index tables** (hardcoded `shell32.dll` / `imageres.dll` resource indices) remain implemented per `../08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md` Section 1; this file references them with Notes `index-map only` to avoid two competing inventories of **indices**.

> [!IMPORTANT]
> **Tiers:** Tier 1 = paths + icons + folder CSIDL subset (installer / Explorer). Tier 1b = Shell execute helpers. Tier 2 = **roadmap** (browser APIs, property UI, shell extensions), not a literal PE roster. **Completeness** = Tier 1 + Tier 1b + Tier 3 + ordinal notes. **`CommandLineToArgvW`** is tracked here and implemented per `../02-kernel-core/TODO-22-environment-variables.md` Section 15 (cross-link Owner). **Done column:** `[x]` only when callable from a PE and behavior matches Windows; `[/]` partial; `[ ]` missing. **Owner `NO_OWNING_TODO`:** no leaf TODO owns this symbol yet.

## On-disk and naming contract

- **Windows canonical path:** `C:\Windows\System32\shell32.dll` (same **BaseDllName** as Windows).
- **Also used:** `explorer.exe` host plan in `../09-desktop-shell/TODO-13-explorer-shell-host.md` depends on Tier 1 rows marked done for boot shell.

## Inputs

| Path / TODO                                                                              | Purpose                                                              |
| ------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------- |
| `TODO-08-win32-api-surface.md`                                                           | `shell32.c` APIs + ShellExecute (Sections 12 and 14)                 |
| `../08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md`                                    | Icon index maps (`shell32_icon_map`, `imageres_icon_map`); Section 1 |
| `../02-kernel-core/TODO-22-environment-variables.md`                                     | `CommandLineToArgvW` kernel / env surface                            |
| `TODO-A-user32-export-master-table.md`                                                   | `LoadIcon` / window integration                                      |
| `../09-desktop-shell/TODO-13-explorer-shell-host.md`                                     | Explorer host bring-up depends on Tier 1 done rows                   |
| `https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/shell32/shell32.spec`    | Scaffold: named exports (Wine `master`)                              |
| `dumpbin /exports` or `llvm-readobj --coff-exports` on `C:\Windows\System32\shell32.dll` | Win11 ground truth on a pinned build; diff against Tier 3            |

## Outcome

Every **named** `shell32.dll` export that appears on a shipping Windows 11 machine is either listed in Tier 1, Tier 1b, the Tier 2 roadmap bullets, or the Tier 3 roster, or is an **ordinal-only** export captured in the methodology section. `TODO-08` stays high-level; detailed acceptance is row-level here.

## Implementation Order

| Order | Deliverable                                                                                                  | Status |
| ------- | -------------------------------------------------------------------------------------------------------------- | -------- |
| 1     | Tier 1 rows kept in sync with PE loader + `TODO-08` when `shell32` is wired                                  | [ ]    |
| 2     | Tier 2 roadmap bullets updated when Explorer / compat matrix expands                                         | [ ]    |
| 3     | Dump exports from pinned Win11 `shell32.dll`; merge deltas into Tier 3 Notes (new names, forwards, ordinals) | [ ]    |
| 4     | Replace `NO_OWNING_TODO` in Tier 3 as owning TODO sections are opened                                        | [ ]    |

## Tier 1 -- Paths, icons, folder resolution (from TODO-08 Section 12)

| Export               | Category     | Owner                     | Done | Notes                                    |
| -------------------- | ------------ | ------------------------- | ---- | ---------------------------------------- |
| `ExtractIconExA`     | Icon         | T08 Section 12            | [ ]  | PE / ico / system                        |
| `ExtractIconExW`     | Icon         | T08 Section 12            | [ ]  |                                          |
| `SHGetFileInfoA`     | File info    | T08 Section 12            | [ ]  |                                          |
| `SHGetFileInfoW`     | File info    | T08 Section 12            | [ ]  |                                          |
| `SHGetStockIconInfo` | Icon         | T08 Section 12            | [ ]  | SIID_* mapping; index maps stay `TODO-11` Section 1 |
| `LoadIconA`          | Icon         | T08 Section 12 / `TODO-A` | [ ]  | Overlap with user32                      |
| `LoadIconW`          | Icon         | T08 Section 12 / `TODO-A` | [ ]  |                                          |
| `LoadImageA`         | Icon         | T08 Section 12            | [ ]  |                                          |
| `LoadImageW`         | Icon         | T08 Section 12            | [ ]  |                                          |
| `DestroyIcon`        | Icon         | T08 Section 12            | [ ]  |                                          |
| `SHGetFolderPathA`   | Known folder | T08 Section 12            | [ ]  | CSIDL mapping in `TODO-08`               |
| `SHGetFolderPathW`   | Known folder | T08 Section 12            | [ ]  |                                          |
| `PathCombineA`       | Path         | T08 Section 12            | [ ]  |                                          |
| `PathCombineW`       | Path         | T08 Section 12            | [ ]  |                                          |
| `PathAppendA`        | Path         | T08 Section 12            | [ ]  |                                          |
| `PathAppendW`        | Path         | T08 Section 12            | [ ]  |                                          |
| `PathFileExistsA`    | Path         | T08 Section 12            | [ ]  | VFS                                      |
| `PathFileExistsW`    | Path         | T08 Section 12            | [ ]  |                                          |
| `CommandLineToArgvW` | Process      | `TODO-14` Section 15      | [ ]  | **Owner `TODO-14`**; row here for shell32 discoverability |

## Tier 1b -- Shell execute (from TODO-08 Section 14)

| Export                | Category | Owner          | Done | Notes             |
| --------------------- | -------- | -------------- | ---- | ----------------- |
| `ShellExecuteA`       | Execute  | T08 Section 14 | [ ]  | `file_assoc_open` |
| `ShellExecuteW`       | Execute  | T08 Section 14 | [ ]  |                   |
| `ShellExecuteExA`     | Execute  | T08 Section 14 | [ ]  |                   |
| `ShellExecuteExW`     | Execute  | T08 Section 14 | [ ]  |                   |
| `PathIsRelativeA`     | Path     | T08 Section 14 | [ ]  |                   |
| `PathIsRelativeW`     | Path     | T08 Section 14 | [ ]  |                   |
| `PathGetDriveNumberA` | Path     | T08 Section 14 | [ ]  |                   |
| `PathGetDriveNumberW` | Path     | T08 Section 14 | [ ]  |                   |
| `PathStripPathA`      | Path     | T08 Section 14 | [ ]  |                   |
| `PathStripPathW`      | Path     | T08 Section 14 | [ ]  |                   |
| `GetFullPathNameA`    | Path     | T08 Section 14 | [ ]  |                   |
| `GetFullPathNameW`    | Path     | T08 Section 14 | [ ]  |                   |

## Tier 2 -- Shell roadmap (not PE names)

These are **families** to split into real `shell32` exports in Tier 3 over time. They are not alternate spellings of the Tier 3 rows.

- **Folder browser:** `SHBrowseForFolder` / `SHBrowseForFolderA` / `SHBrowseForFolderW` style APIs; Owner splits between `TODO-08` and `../09-desktop-shell/TODO-13-explorer-shell-host.md` when Explorer hosts the dialog.
- **Known folders v2:** `SHGetKnownFolderPath`, `SHSetKnownFolderPath`, FOLDERID mapping; align with CSIDL work in `TODO-08` Section 12.
- **Drag and drop:** `DAD_*` helpers and OLE drag sources already listed in Tier 3; wire when desktop shell gains DnD (`TODO-13` plus compositor input).
- **Property sheets / associations:** property UI verbs, `Assoc*` helpers; many installers depend on a small subset first.

## Export inventory methodology (Win11 completeness)

Microsoft does not publish one MSDN page per `shell32.dll` export. Use two layers:

1. **Machine ground truth (Windows 11):** Export directory of `%SystemRoot%\System32\shell32.dll` from a **pinned build**. Tools: `dumpbin /exports`, `llvm-readobj --coff-exports`, or `link /dump /exports`. Background: `https://learn.microsoft.com/en-us/dotnet/framework/interop/identifying-functions-in-dlls`
2. **Scaffold list (this repo):** Tier 3 merges **451** unique named symbols parsed from Wine `dlls/shell32/shell32.spec` on branch `master` (re-parse when refreshing). Wine models compatibility; **Win11 servicing builds can differ**, so step 3 in `Implementation Order` is mandatory.

**Ordinal-only exports:** Wine lists some entries as **numeric ordinals** with names or as **`-noname`** exports (name omitted in the PE export table). This checklist still keys off the **C symbol** Wine uses in the spec. Win11 can add forwarded exports or private-by-ordinal entries that never appear in Wine: paste `ordinal -> name/rva` from your dump under `Ordinal-only (Win11 build XXXXX)` when you have hardware truth. **No dedicated TODO** owns that bucket yet; track it here until `TODO-08` adds a subsection.

**Symbols in Tier 1 and Tier 1b** (plus Wine `SHGetFileInfo`, `PathAppend`, `PathCombine`, `PathFileExists`, `PathIsRelative`, `PathStripPath`, `PathGetDriveNumber`, `ShellExecuteEx` where those are the underlying export names) are omitted from Tier 3 to avoid duplicate rows.

## Tier 3 -- Full named export roster (alphabetical, Wine scaffold)

| Export                                   | Category   | Owner          | Done | Notes                                    |
| ---------------------------------------- | ---------- | -------------- | ---- | ---------------------------------------- |
| `AddCommasW`                             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ArrangeWindows`                         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `CDefFolderMenu_Create2`                 | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `CIDLData_CreateFromIDArray`             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `CallCPLEntry16`                         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `CheckEscapesA`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `CheckEscapesW`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Control_FillCache_RunDLL`               | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Control_FillCache_RunDLLA`              | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Control_FillCache_RunDLLW`              | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Control_RunDLL`                         | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Control_RunDLLA`                        | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Control_RunDLLAsUserW`                  | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Control_RunDLLW`                        | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_AutoScroll`                         | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_DragEnter`                          | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_DragEnterEx`                        | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_DragLeave`                          | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_DragMove`                           | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_SetDragImage`                       | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_SetDragImageFromListView`           | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DAD_ShowDragImage`                      | DragDrop   | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Desktop_UpdateBriefcaseOnEvent`         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DllCanUnloadNow`                        | COM/Dll    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DllGetClassObject`                      | COM/Dll    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DllGetVersion`                          | COM/Dll    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DllInstall`                             | COM/Dll    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DllRegisterServer`                      | COM/Dll    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DllUnregisterServer`                    | COM/Dll    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DoEnvironmentSubst`                     | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DoEnvironmentSubstA`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DoEnvironmentSubstW`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragAcceptFiles`                        | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragFinish`                             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragQueryFile`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragQueryFileA`                         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragQueryFileAorW`                      | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragQueryFileW`                         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragQueryInfo`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DragQueryPoint`                         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DriveType`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `DuplicateIcon`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExitWindowsDialog`                      | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractAssociatedIconA`                 | Assoc      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractAssociatedIconExA`               | Assoc      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractAssociatedIconExW`               | Assoc      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractAssociatedIconW`                 | Assoc      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractIconA`                           | Icon       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractIconEx`                          | Icon       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractIconResInfoA`                    | Icon       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractIconResInfoW`                    | Icon       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractIconW`                           | Icon       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ExtractVersionResource16W`              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FOOBAR1217`                             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileIconInit`                           | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_AbortInitMenu`                 | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_AddFilesForPidl`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_AppendFilesForPidl`            | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_AppendItem`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_Create`                        | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_DeleteAllItems`                | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_DeleteItemByCmd`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_DeleteItemByFirstID`           | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_DeleteItemByIndex`             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_DeleteSeparator`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_Destroy`                       | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_DrawItem`                      | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_EnableItemByCmd`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_FindSubMenuByPidl`             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_GetItemExtent`                 | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_GetLastSelectedItemPidls`      | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_HandleMenuChar`                | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_InitMenuPopup`                 | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_InsertUsingPidl`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_Invalidate`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_MeasureItem`                   | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_ReplaceUsingPidl`              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FileMenu_TrackPopupMenuEx`              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FindExeDlgProc`                         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FindExecutableA`                        | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FindExecutableW`                        | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FixupOptionalComponents`                | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `FreeIconList`                           | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `GUIDFromStringW`                        | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `GetCurrentProcessExplicitAppUserModelID` | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `GetFileNameFromBrowse`                  | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILAppendID`                             | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILClone`                                | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILCloneFirst`                           | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILCombine`                              | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILCreateFromPath`                       | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILCreateFromPathA`                      | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILCreateFromPathW`                      | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILFindChild`                            | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILFindLastID`                           | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILFree`                                 | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILGetDisplayName`                       | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILGetDisplayNameEx`                     | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILGetNext`                              | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILGetPseudoNameW`                       | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILGetSize`                              | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILGlobalClone`                          | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILGlobalFree`                           | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILIsEqual`                              | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILIsParent`                             | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILLoadFromStream`                       | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILRemoveLastID`                         | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ILSaveToStream`                         | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `InitNetworkAddressControl`              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Int64ToString`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `InternalExtractIconListA`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `InternalExtractIconListW`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `InvalidateDriveType`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `IsLFNDrive`                             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `IsLFNDriveA`                            | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `IsLFNDriveW`                            | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `IsNetDrive`                             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `IsUserAnAdmin`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `LargeIntegerToString`                   | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `LinkWindow_RegisterClass`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `LinkWindow_UnregisterClass`             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Link_AddExtraDataSection`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Link_ReadExtraDataSection`              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Link_RemoveExtraDataSection`            | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `LogoffWindowsDialog`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `NTSHChangeNotifyDeregister`             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `NTSHChangeNotifyRegister`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `OCInstall`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `OleStrToStrN`                           | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `OpenAs_RunDLL`                          | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `OpenAs_RunDLLA`                         | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `OpenAs_RunDLLW`                         | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `OpenRegStream`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ParseField`                             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathAddBackslash`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathBuildRoot`                          | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathCleanupSpec`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathFindExtension`                      | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathFindFileName`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathFindOnPath`                         | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathGetArgs`                            | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathGetExtension`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathGetShortPath`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathIsDirectory`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathIsExe`                              | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathIsRoot`                             | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathIsSameRoot`                         | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathIsTemporaryW`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathIsUNC`                              | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathMakeUniqueName`                     | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathMatchSpec`                          | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathParseIconLocation`                  | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathProcessCommand`                     | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathQualify`                            | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathQuoteSpaces`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathRemoveArgs`                         | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathRemoveBlanks`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathRemoveExtension`                    | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathRemoveFileSpec`                     | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathResolve`                            | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathSetDlgItemPath`                     | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathStripToRoot`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathUnquoteSpaces`                      | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PathYetAnotherMakeUniqueName`           | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PickIconDlg`                            | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PifMgr_CloseProperties`                 | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PifMgr_GetProperties`                   | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PifMgr_OpenProperties`                  | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PifMgr_SetProperties`                   | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Printer_LoadIconsW`                     | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PrintersGetCommand_RunDLL`              | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PrintersGetCommand_RunDLLA`             | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `PrintersGetCommand_RunDLLW`             | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Printers_AddPrinterPropPages`           | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Printers_GetPidl`                       | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Printers_RegisterWindowW`               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Printers_UnregisterWindow`              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RLBuildListOfPaths`                     | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ReadCabinetState`                       | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RealDriveType`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RealDriveTypeFlags`                     | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RealShellExecuteA`                      | Execute    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RealShellExecuteExA`                    | Execute    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RealShellExecuteExW`                    | Execute    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RealShellExecuteW`                      | Execute    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ReceiveAddToRecentDocs`                 | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RegenerateUserEnvironment`              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RegisterShellHook`                      | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RestartDialog`                          | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RestartDialogEx`                        | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RunDLL_CallEntry16`                     | CPL/Rundll | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `RunFileDlg`                             | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHAbortInvokeCommand`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHAddFromPropSheetExtArray`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHAddToRecentDocs`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHAlloc`                                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHAppBarMessage`                        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHAssocEnumHandlers`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHAssocEnumHandlersForProtocolByApplication` | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHBindToFolderIDListParent`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHBindToObject`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHBindToParent`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHBrowseForFolder`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHBrowseForFolderA`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHBrowseForFolderW`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCLSIDFromString`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotification_Lock`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotification_Unlock`            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotify`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotifyDeregister`               | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotifyReceive`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotifyRegister`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotifySuspendResume`            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeNotifyUpdateEntryList`          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHChangeRegistrationReceive`            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCloneSpecialIDList`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCoCreateInstance`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateAssociationRegistration`        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateDataObject`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateDefClassObject`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateDefaultContextMenu`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateDirectory`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateDirectoryExA`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateDirectoryExW`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateFileExtractIconW`               | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateItemFromIDList`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateItemFromParsingName`            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateItemFromRelativeName`           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateItemInKnownFolder`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateItemWithParent`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateLinks`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateProcessAsUserW`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreatePropSheetExtArray`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreatePropSheetExtArrayEx`            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateQueryCancelAutoPlayMoniker`     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateSessionKey`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateShellFolderView`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateShellFolderViewEx`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateShellItem`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateShellItemArray`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateShellItemArrayFromDataObject`   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateShellItemArrayFromIDLists`      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateShellItemArrayFromShellItem`    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHCreateStdEnumFmtEtc`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHDefExtractIconA`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHDefExtractIconW`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHDestroyPropSheetExtArray`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHDllGetClassObject`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHDoDragDrop`                           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHEmptyRecycleBinA`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHEmptyRecycleBinW`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHEnumerateUnreadMailAccountsW`         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHEvaluateSystemCommandTemplate`        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHExtractIconsW`                        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFileOperation`                        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFileOperationA`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFileOperationW`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFindComputer`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFindFiles`                            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFind_InitMenuPopup`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFlushClipboard`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFlushSFCache`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFormatDrive`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFree`                                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFreeNameMappings`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHFreeUnusedLibraries`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetDataFromIDListA`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetDataFromIDListW`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetDesktopFolder`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetDiskFreeSpaceA`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetDiskFreeSpaceExA`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetDiskFreeSpaceExW`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetFileIcon`                          | File info  | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetFolderLocation`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetFolderPathAndSubDirA`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetFolderPathAndSubDirW`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetFolderPathEx`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetFreeDiskSpace`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetIDListFromObject`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetIconOverlayIndexA`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetIconOverlayIndexW`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetImageList`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetInstanceExplorer`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetItemFromDataObject`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetItemFromObject`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetKnownFolderIDList`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetKnownFolderItem`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetKnownFolderPath`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetLocalizedName`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetMalloc`                            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetNameFromIDList`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetNetResource`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetNewLinkInfo`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetNewLinkInfoA`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetNewLinkInfoW`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetPathFromIDList`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetPathFromIDListA`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetPathFromIDListEx`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetPathFromIDListW`                   | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetPropertyStoreForWindow`            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetPropertyStoreFromParsingName`      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetRealIDL`                           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetSetFolderCustomSettings`           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetSetSettings`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetSettings`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetSpecialFolderLocation`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetSpecialFolderPath`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetSpecialFolderPathA`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGetSpecialFolderPathW`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHGlobalDefect`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHHandleDiskFull`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHHandleUpdateImage`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHHelpShortcuts_RunDLL`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHHelpShortcuts_RunDLLA`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHHelpShortcuts_RunDLLW`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHILCreateFromPath`                     | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHInitRestricted`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHInvokePrinterCommandA`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHInvokePrinterCommandW`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHIsBadInterfacePtr`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHIsFileAvailableOffline`               | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLimitInputEdit`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLoadInProc`                           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLoadNonloadedIconOverlayIdentifiers`  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLoadOLE`                              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLocalAlloc`                           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLocalFree`                            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLocalReAlloc`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHLogILFromFSIL`                        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHMapIDListToImageListIndexAsync`       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHMapPIDLToSystemImageListIndex`        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHMultiFileProperties`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHNetConnectionDialog`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHObjectProperties`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHOpenFolderAndSelectItems`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHOpenWithDialog`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHOutOfMemoryMessageBox`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHParseDisplayName`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHPathPrepareForWriteA`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHPathPrepareForWriteW`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHPropStgCreate`                        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHPropStgReadMultiple`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHPropStgWriteMultiple`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHQueryRecycleBinA`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHQueryRecycleBinW`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHQueryUserNotificationState`           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegCloseKey`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegDeleteKeyW`                        | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegOpenKeyA`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegOpenKeyW`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegQueryValueA`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegQueryValueExA`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegQueryValueExW`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegQueryValueW`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRegisterDragDrop`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRemoveLocalizedName`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHReplaceFromPropSheetExtArray`         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRestricted`                           | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRevokeDragDrop`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHRunControlPanel`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHSetInstanceExplorer`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHSetLocalizedName`                     | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHSetTemporaryPropertyForItem`          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHSetUnreadMailCountW`                  | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHShellFolderView_Message`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHSimpleIDListFromPath`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHStartNetConnectionDialog`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHUpdateImageA`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHUpdateImageW`                         | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHUpdateRecycleBinIcon`                 | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHValidateUNC`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHWaitForFileToOpen`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHWaitOp_Operate`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SHWinHelp`                              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SetAppStartingCursor`                   | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SetCurrentProcessExplicitAppUserModelID` | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheChangeDirA`                          | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheChangeDirExA`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheChangeDirExW`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheChangeDirW`                          | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheConvertPathW`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheFullPathA`                           | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheFullPathW`                           | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheGetCurDrive`                         | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheGetDirA`                             | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheGetDirExW`                           | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheGetDirW`                             | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheGetPathOffsetW`                      | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheRemoveQuotesA`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheRemoveQuotesW`                       | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheSetCurDrive`                         | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheShortenPathA`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SheShortenPathW`                        | Path       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellAboutA`                            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellAboutW`                            | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellDDEInit`                           | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellExec_RunDLL`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellExec_RunDLLA`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellExec_RunDLLW`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellHookProc`                          | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellMessageBoxA`                       | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShellMessageBoxW`                       | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_GetCachedImageIndex`              | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_GetCachedImageIndexA`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_GetCachedImageIndexW`             | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_GetImageLists`                    | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_MergeMenus`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_NotifyIcon`                       | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_NotifyIconA`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_NotifyIconGetRect`                | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shell_NotifyIconW`                      | Shell      | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shl1632_ThunkData32`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Shl3216_ThunkData32`                    | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `ShortSizeFormatW`                       | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `SignalFileOpen`                         | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrChrA`                                | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrChrIA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrChrIW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrChrW`                                | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrCmpNA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrCmpNIA`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrCmpNIW`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrCmpNW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrCpyNA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrCpyNW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrNCmpA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrNCmpIA`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrNCmpIW`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrNCmpW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrNCpyA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrNCpyW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRChrA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRChrIA`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRChrIW`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRChrW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRStrA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRStrIA`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRStrIW`                              | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRStrW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrRetToStrN`                           | PIDL       | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrStrA`                                | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrStrIA`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrStrIW`                               | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrStrW`                                | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrToOleStr`                            | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `StrToOleStrN`                           | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `WOWShellExecute`                        | Execute    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Win32CreateDirectory`                   | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Win32DeleteFile`                        | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `Win32RemoveDirectory`                   | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |
| `WriteCabinetState`                      | General    | NO_OWNING_TODO | [ ]  | Wine `shell32.spec`; reconcile with Win11 dump |

## OS Comparison

| ⭐  | Feature                             | 🪟 Win11                             | 🐧 Linux                       | 🚀 Impossible OS                         |
| --- | ----------------------------------- | ------------------------------------ | ------------------------------ | ---------------------------------------- |
| 💎  | Shell paths + icons (`shell32.dll`) | shell32 `SH*` APIs + icon index maps | `xdg-user-dir` + desktop files | Tier 1 rows; icon index kernel maps stay per `TODO-11` Notes |

## Unit Tests

**Note:** Doc-only table. Proof via compat tiers and `../09-desktop-shell/TODO-13-explorer-shell-host.md` Explorer bring-up tests when added.

## History

| Date       | Action                                     | Summary                                                                                                                                          |
| ------------ | -------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2026-04-14 | Created TODO-C shell32 export master table | Initial Tier 1 shell paths + icon API seed rows.                                                                                                 |
| 2026-04-14 | validate                                   | OS Comparison 5-column template; structural pass after master-table landing.                                                                     |
| 2026-04-14 | gap-analysis                               | Master table owns per-export rows; TODO-08 / TODO-11 reference here only.                                                                        |
| 2026-04-14 | research                                   | Aligned tables; removed inline URL markdown; Tier 3 roster (451 rows) from Wine named set (471 names, 1 ordinal `@` slot); Win11 dump checklist. |
