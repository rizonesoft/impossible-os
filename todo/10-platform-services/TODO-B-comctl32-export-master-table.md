---
schema_version: 1
id: comctl32-export-master-table
domain: 10-platform-services
status: active
title: "TODO-B -- comctl32.dll Export Master Table"
---

# TODO-B -- comctl32.dll Export Master Table

> **Goal:** Authoritative checklist of **comctl32.dll** exports and version entry points. Most real binaries expect `comctl32` for common controls v6 and `InitCommonControlsEx`; the full Common Controls surface is large and often manifest-gated, so Tier 1 is the bring-up slice and Tier 3 is the exhaustive **named** export roster (Wine scaffold) plus ordinal-only accounting.

> [!IMPORTANT]
> **Tiers:** Tier 1 = load + init entry points. Tier 2 = **roadmap** (control families that map to `CTRL_*` widgets, not literal PE names). **Completeness** = Tier 1 + Tier 3 + ordinal-only notes. **Done column:** `[x]` only when callable from a PE and behavior matches Windows; `[/]` partial; `[ ]` missing. **Owner `NO_OWNING_TODO`:** no leaf TODO owns this symbol yet (split work across `TODO-08`, `TODO-11`, `TODO-04`, or PE loader notes).

## On-disk and naming contract

- **Windows canonical path:** `C:\Windows\System32\comctl32.dll` (same **BaseDllName** as Windows; v6 activation still requires manifest / activation context work in `TODO-08`).
- **Implementation:** typically thin re-exports or stubs under `src/win32/` (see `TODO-08-win32-api-surface.md` when a section is added) plus `../08-graphics-ui/TODO-05-widget-library-core.md` where behavior is native IxUI, not a full DLL clone.

## Inputs

| Path / TODO                                                                                   | Purpose                                                                   |
| --------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| `TODO-08-win32-api-surface.md`                                                                | Win32 surface owner; add a section when `comctl32.c` lands                |
| `../12-user-platform-sdk/TODO-07-win32-compat-matrix.md`                                      | Tier 8 `InitCommonControlsEx` gate text                                   |
| `../08-graphics-ui/TODO-05-widget-library-core.md`                                            | `CTRL_LISTVIEW`, `CTRL_TREEVIEW`, tab strip, etc.                         |
| `TODO-A-user32-export-master-table.md`                                                        | Parent HWND APIs for hosted controls                                      |
| `https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/comctl32/comctl32.spec`       | Scaffold: legacy `comctl32` ordinals + exports (Wine `master`)            |
| `https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/comctl32_v6/comctl32_v6.spec` | Scaffold: v6 forwarder surface merged into same checklist (Wine `master`) |
| `dumpbin /exports` or `llvm-readobj --coff-exports` on `C:\Windows\System32\comctl32.dll`     | Win11 ground truth on a pinned build; diff against Tier 3                 |

## Outcome

Every **named** `comctl32.dll` export that appears on a shipping Windows 11 machine is either listed in Tier 1, the Tier 2 roadmap bullets, or the Tier 3 roster, or is an **ordinal-only** export captured in the methodology section. Tier 8 compat text points here instead of duplicating long export lists.

## Implementation Order

| Order | Deliverable                                                                                                   | Status |
| ----- | ------------------------------------------------------------------------------------------------------------- | ------ |
| 1     | Tier 1 rows kept in sync with PE loader + `TODO-08` when `comctl32` is wired                                  | [ ]    |
| 2     | Tier 2 roadmap bullets updated when `TODO-04` / `TODO-11` gain matching control APIs                          | [ ]    |
| 3     | Dump exports from pinned Win11 `comctl32.dll`; merge deltas into Tier 3 Notes (new names, forwards, ordinals) | [ ]    |
| 4     | Replace `NO_OWNING_TODO` in Tier 3 as owning TODO sections are opened                                         | [ ]    |

## Tier 1 -- Init and version

| Export                 | Category | Owner                       | Done | Notes                                    |
| ---------------------- | -------- | --------------------------- | ---- | ---------------------------------------- |
| `InitCommonControls`   | Init     | T08 / T07 PE built-in table | [ ]  | Ordinal-friendly legacy entry; pairs with `InitCommonControlsEx` |
| `InitCommonControlsEx` | Init     | T08 / T07 PE built-in table | [ ]  | Required for v6 common controls awareness in many installers |
| `DllGetVersion`        | Version  | T08 / PE loader             | [ ]  | Optional stub returning common controls major/minor |

## Tier 2 -- Control class roadmap (IxUI mapping, not PE names)

These are **families** to split into real `comctl32` exports in Tier 3 over time. They are not alternate spellings of the Tier 3 rows.

- **Image list APIs:** implement as `ImageList_*` exports from Tier 3; Owner splits between `TODO-11` (GDI-backed lists) and `TODO-08` (DLL export table).
- **List view:** `ListView_*` exports line up with `CTRL_LISTVIEW` in `../08-graphics-ui/TODO-05-widget-library-core.md` plus `../08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md` when Win32 shims wrap the widget.
- **Tree view:** `TreeView_*` exports line up with `CTRL_TREEVIEW` in the same TODO-04 file.

## Export inventory methodology (Win11 completeness)

Microsoft does not publish one MSDN page per `comctl32.dll` export. Use two layers:

1. **Machine ground truth (Windows 11):** Export directory of `%SystemRoot%\System32\comctl32.dll` from a **pinned build**. Tools: `dumpbin /exports`, `llvm-readobj --coff-exports`, or `link /dump /exports`. Background: `https://learn.microsoft.com/en-us/dotnet/framework/interop/identifying-functions-in-dlls`
2. **Scaffold list (this repo):** Tier 3 merges **161** unique named symbols parsed from Wine `dlls/comctl32/comctl32.spec` **plus** `dlls/comctl32_v6/comctl32_v6.spec` on branch `master` (re-parse when refreshing). Wine models compatibility; **Win11 servicing builds can differ**, so step 3 in `Implementation Order` is mandatory.

**Ordinal-only exports:** Wine lists many entries as **numeric ordinals** with names (`2 stdcall MenuHelp(...)`) or as **`-noname`** exports (name omitted in the spec). This is different from `user32.dll` where Wine also uses bare `stub @` slots. Win11 can still add forwarded exports or private-by-ordinal entries that never appear in Wine: paste `ordinal -> name/rva` from your dump under `Ordinal-only (Win11 build XXXXX)` when you have hardware truth. **No dedicated TODO** owns that bucket yet; track it here until `TODO-08` adds a subsection.

**Symbols in Tier 1** are omitted from Tier 3 to avoid duplicate rows.

## Tier 3 -- Full named export roster (alphabetical, Wine scaffold)

| Export                         | Category       | Owner          | Done | Notes                                    |
| ------------------------------ | -------------- | -------------- | ---- | ---------------------------------------- |
| `AddMRUData`                   | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `AddMRUStringA`                | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `AddMRUStringW`                | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `Alloc`                        | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `Cctl1632_ThunkData32`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateMRUListA`               | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateMRUListLazyA`           | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateMRUListLazyW`           | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateMRUListW`               | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateMappedBitmap`           | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreatePage`                   | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreatePropertySheetPage`      | Property sheet | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreatePropertySheetPageA`     | Property sheet | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreatePropertySheetPageW`     | Property sheet | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateProxyPage`              | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateStatusWindow`           | Status bar     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateStatusWindowA`          | Status bar     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateStatusWindowW`          | Status bar     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateToolbar`                | Toolbar        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateToolbarEx`              | Toolbar        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `CreateUpDownControl`          | UpDown         | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_Clone`                    | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_Create`                   | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_CreateEx`                 | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_DeleteAllPtrs`            | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_DeletePtr`                | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_Destroy`                  | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_DestroyCallback`          | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_EnumCallback`             | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_GetPtr`                   | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_GetPtrIndex`              | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_Grow`                     | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_InsertPtr`                | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_LoadStream`               | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_Merge`                    | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_SaveStream`               | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_Search`                   | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_SetPtr`                   | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DPA_Sort`                     | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_Clone`                    | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_Create`                   | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_DeleteAllItems`           | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_DeleteItem`               | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_Destroy`                  | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_DestroyCallback`          | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_EnumCallback`             | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_GetItem`                  | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_GetItemPtr`               | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_InsertItem`               | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DSA_SetItem`                  | DPA/DSA        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DefSubclassProc`              | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DelMRUString`                 | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DestroyPropertySheetPage`     | Property sheet | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DllInstall`                   | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DoReaderMode`                 | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DrawInsert`                   | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DrawShadowText`               | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DrawStatusText`               | Status bar     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DrawStatusTextA`              | Status bar     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DrawStatusTextW`              | Status bar     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DrawTextExPrivWrap`           | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `DrawTextWrap`                 | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `EnumMRUListA`                 | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `EnumMRUListW`                 | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ExtTextOutWrap`               | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FindMRUData`                  | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FindMRUStringA`               | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FindMRUStringW`               | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_EnableScrollBar`       | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_GetScrollInfo`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_GetScrollPos`          | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_GetScrollProp`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_GetScrollRange`        | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_SetScrollInfo`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_SetScrollPos`          | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_SetScrollProp`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_SetScrollRange`        | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FlatSB_ShowScrollBar`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `Free`                         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `FreeMRUList`                  | MRU            | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `GetCharWidthWrap`             | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `GetEffectiveClientRect`       | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `GetMUILanguage`               | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `GetSize`                      | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `GetTextExtentPoint32Wrap`     | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `GetTextExtentPointWrap`       | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `GetWindowSubclass`            | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `HIMAGELIST_QueryInterface`    | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Add`                | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_AddIcon`            | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_AddMasked`          | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_BeginDrag`          | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_CoCreateInstance`   | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Copy`               | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Create`             | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Destroy`            | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_DragEnter`          | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_DragLeave`          | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_DragMove`           | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_DragShowNolock`     | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Draw`               | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_DrawEx`             | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_DrawIndirect`       | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Duplicate`          | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_EndDrag`            | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetBkColor`         | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetDragImage`       | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetFlags`           | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetIcon`            | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetIconSize`        | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetImageCount`      | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetImageInfo`       | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_GetImageRect`       | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_LoadImage`          | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_LoadImageA`         | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_LoadImageW`         | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Merge`              | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Read`               | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Remove`             | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Replace`            | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_ReplaceIcon`        | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetBkColor`         | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetColorTable`      | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetDragCursorImage` | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetFilter`          | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetFlags`           | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetIconSize`        | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetImageCount`      | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_SetOverlayImage`    | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_Write`              | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ImageList_WriteEx`            | Image list     | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `InitMUILanguage`              | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `InitializeFlatSB`             | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `LBItemFromPt`                 | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `LoadIconMetric`               | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `LoadIconWithScaleDown`        | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `MakeDragList`                 | Drag list      | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `MenuHelp`                     | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `MirrorIcon`                   | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `PropertySheet`                | Property sheet | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `PropertySheetA`               | Property sheet | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `PropertySheetW`               | Property sheet | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ReAlloc`                      | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `RegisterClassNameW`           | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `RemoveWindowSubclass`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `SHGetProcessDword`            | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `SendNotify`                   | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `SendNotifyEx`                 | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `SetPathWordBreakProc`         | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `SetWindowSubclass`            | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `ShowHideMenuCtl`              | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `SmoothScrollWindow`           | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `Str_GetPtrA`                  | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `Str_GetPtrW`                  | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `Str_SetPtrA`                  | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `Str_SetPtrW`                  | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `TaskDialog`                   | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `TaskDialogIndirect`           | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `TextOutWrap`                  | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `UninitializeFlatSB`           | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |
| `_TrackMouseEvent`             | General        | NO_OWNING_TODO | [ ]  | Wine `comctl32*.spec`; reconcile with Win11 dump |

## OS Comparison

| ⭐  | Feature                          | 🪟 Win11                          | 🐧 Linux             | 🚀 Impossible OS                         |
| --- | -------------------------------- | --------------------------------- | -------------------- | ---------------------------------------- |
| 💎  | Common Controls (`comctl32.dll`) | comctl32 v6 + manifest activation | GTK/Qt widget stacks | Tier 1 init + Tier 3 rows; native `CTRL_*` where applicable |

## Unit Tests

**Note:** Doc-only table. Proof lives in compat tier tests in `../12-user-platform-sdk/TODO-07-win32-compat-matrix.md` and PE load of a binary that links `comctl32`.

## History

| Date       | Action                                      | Summary                                                                                                                                    |
| ---------- | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| 2026-04-14 | Created TODO-B comctl32 export master table | Initial InitCommonControlsEx + control-class seed rows.                                                                                    |
| 2026-04-14 | validate                                    | OS Comparison 5-column template; structural pass after master-table landing.                                                               |
| 2026-04-14 | gap-analysis                                | Master table owns per-export rows; TODO-08/TODO-11 reference here only.                                                                    |
| 2026-04-14 | research                                    | Aligned tables; removed inline URL markdown; added Tier 3 roster (161 names) from Wine comctl32 + comctl32_v6 specs; Win11 dump checklist. |
