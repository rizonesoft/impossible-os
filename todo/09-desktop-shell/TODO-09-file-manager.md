---
schema_version: 1
id: file-manager
domain: 09-desktop-shell
status: active
title: "TODO-09 -- File Manager"
---

# TODO-09 -- File Manager

> **Goal:** Build the primary file browsing application -- a production-quality File Manager with four-zone window layout, sidebar quick access + drives, icon and detail views with column sort, full file operations (copy/cut/paste/delete/rename/new folder), context menus, inline search, drag-and-drop, and stretch advanced features (preview pane, tabs, breadcrumb) that rival Windows Explorer.

> [!IMPORTANT]
> **Already exists**: `icon_for_extension(ext)` + `icon_draw()` + `icon_draw_scaled()` in `icon_store.h`. `vfs_readdir(dir_node, idx)`, `vfs_finddir(dir_node, name)`, `vfs_stat(path, stat)`, `vfs_rename(old, new)`, `vfs_create(path, type)`, `vfs_open/read/write`, `vfs_unlink` in `vfs.h`. `vfs_mkdir()` for new folder. **No `src/apps/` directory** -- create `src/apps/filemgr/filemgr.c`. **No `vfs_copy()`** -- implement `filemgr_copy_file(src, dst)` as a read+write loop with a PMM 64 KiB copy buffer. **Forward dependencies** (must be completed before the stated sections): `context_menu_show()` (08-graphics-ui/TODO-09 §1), `CTRL_LISTVIEW` (08-graphics-ui/TODO-06 §1) / `CTRL_TABSTRIP` (08-graphics-ui/TODO-05 §6), `clipboard_set/get(CLIP_FILES)` (TODO-01 §1), `file_assoc_open()` (TODO-02 §1), `trash_delete/restore()` (TODO-04 §2), `search_query_scoped()` (TODO-05 §4), `wm_drag_start()` (08-graphics-ui/TODO-08 §7 drag-drop). Complete sections in order: core layout → sidebar → view modes → file operations → context menus → file search → drag-and-drop → advanced features.

## Inputs

- [`TODO-13-explorer-shell-host.md`](TODO-13-explorer-shell-host.md) (XREF) shell host process model + `ShellExecute` wiring; this file stays the deep four-zone file manager UX
- `include/kernel/fs/vfs.h` -- `vfs_readdir`, `vfs_stat`, `vfs_rename`, `vfs_create`, `vfs_unlink`, `vfs_open/read/write`, `vfs_mkdir` -- all file operations and directory listing
- `include/icon_store.h` -- `icon_for_extension()`, `icon_draw()`, `icon_draw_scaled()` -- icon grid rendering
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_surface_t`, `ttf_draw_string()` -- cell and label rendering
- `include/font_mgr.h` -- `ttf_get(FONT_UI, px)`, `ttf_draw_string()` -- filename labels, status bar
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()` -- 64 KiB copy buffer, large grid allocations
- `include/registry.h` -- `HKCU\Software\Impossible\FileManager\ViewMode/SortColumn/SidebarWidth` -- §3 view persist
- `include/kernel/time.h` (TODO-10) -- `time_to_datetime()`, `time_format()` -- §3 Date Modified column
- `include/desktop/controls.h` (08-graphics-ui/TODO-06) -- `CTRL_LISTVIEW`, `CTRL_TABSTRIP` -- §2 detail view, §8 tabs
- `include/desktop/context_menu.h` (08-graphics-ui/TODO-09 §1) -- `context_menu_show()` -- §5 right-click menus
- `include/kernel/clipboard.h` (TODO-01 §1) -- `clipboard_set/get(CLIP_FILES, ...)` -- §4 cut/copy/paste
- `include/desktop/shortcut.h` (TODO-02 §1) -- `file_assoc_open()` -- §1 double-click open
- `include/kernel/trash.h` (TODO-04 §2) -- `trash_delete()`, `trash_restore()` -- §6 Delete + Ctrl+Z
- `include/kernel/search.h` (TODO-05 §4) -- `search_query_scoped()` -- §6 toolbar search
- `include/desktop/wm.h` (08-graphics-ui/TODO-08 §7) -- `wm_drag_start()` -- §7 drag-and-drop ghost
- `include/stb_image.h` -- `stbi_load_from_memory()` -- §8 preview pane image rendering
- → XREF: `09-desktop-shell/TODO-05-file-search.md §5` -- §6 here IS the file manager search integration described there
- → XREF: `09-desktop-shell/TODO-04-recycle-zip-scheduler.md §2` -- §6 Delete uses `trash_delete()` from there
- → XREF: `09-desktop-shell/TODO-02-file-associations-resources.md §1` -- `file_assoc_open()` for double-click; §3 Open With dialog
- → XREF: `09-desktop-shell/TODO-06-security-accounts.md §9` -- §5 Properties dialog shows `i_uid/i_mode` permissions

## Outcome

- Four-zone window (toolbar + address bar, sidebar, file area, status bar) in `src/apps/filemgr/`.
- Navigation pane (220 px): Desktop, Downloads, Documents, Pictures, Music, Videos, This PC, drives, Network; drive free space in This PC tiles.
- Icon view (48 px grid) + Detail view (sortable Name/Size/Type/Date/Attrs table).
- File operations: copy/cut/paste/delete (trash)/permanent delete/rename/new folder; progress dialog > 1 MB; Ctrl+Z undo.
- Context menus: file, folder, empty-area; Properties dialog.
- Inline search bar scoped to current directory via `search_query_scoped()`.
- Drag-and-drop between windows, to desktop, to trash; semi-transparent ghost with count badge.
- Stretch: preview pane, directory tabs via `CTRL_TABSTRIP`, breadcrumb address bar, FTP/UNC path.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                              | Depends On                                                                                    | Status |
| --- | :---: | -------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 File Explorer frame -- title bar, address row, command bar, nav history, `vfs_readdir` listing        | `vfs_readdir`, `icon_for_extension`, `file_assoc_open` (TODO-02), `wm_create_window` (exist) |  [ ]   |
| 💎  |   2   | §2 Navigation pane -- design-order folders, This PC, drives, Network, accent selection bar               | §1 core layout; `vfs_stat` (exists); drive letter enumeration via VFS                        |  [ ]   |
| 💎  |   3   | §3 View modes -- Icon grid (48 px), Detail table (sortable columns), multi-select, Registry persist       | §1+§2; `CTRL_LISTVIEW` (D08 T06 §1); `time_format()` (TODO-10); `icon_draw_scaled()` (exists)  |  [ ]   |
| 💎  |   4   | §4 File operations -- copy/cut/paste, delete (trash), rename (F2 inline), new folder, progress, Ctrl+Z  | §3 selection; `clipboard_set(CLIP_FILES)` (TODO-01); `trash_delete()` (TODO-04)             |  [ ]   |
| 💎  |   5   | §5 Context menus -- file/folder/empty-area menus; Properties dialog with permissions                     | §3 selection; §4 ops; `context_menu_show()` (D08 T09 §1); `file_assoc_open()` (TODO-02)        |  [ ]   |
| 💎  |   6   | §6 File search -- toolbar search bar, `search_query_scoped()`, 150 ms debounce, clear-to-restore          | §1 toolbar; `search_query_scoped()` (TODO-05 §4); §2 file area                              |  [ ]   |
| 💎  |   7   | §7 Drag and drop -- file→desktop, file→trash, file→other window, drop-onto; drag ghost + count badge     | §3 selection; §4 file ops; `wm_drag_start()` (08-graphics-ui/TODO-08 §7; §8)                                   |  [ ]   |
| ⭐  |   8   | §8 Advanced -- preview pane, `CTRL_TABSTRIP` tabs, breadcrumb address bar, FTP/UNC path stub             | §1–§7; `CTRL_TABSTRIP` (D08 T05 §6); `stb_image` (exists)                                      |  [ ]   |

---

## 1. Core Layout `[Sonnet]`

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

The Windows 11 File Explorer frame of `docs/design/shell.md#file-explorer`: a 40 px tabbed title bar, a 48 px address row (Back/Forward/Up/Refresh, breadcrumb, search), a 48 px command bar, a 220 px navigation pane beside the file area, and a 24 px status bar. `vfs_readdir()` to populate file area. `icon_for_extension()` + `icon_draw()` per entry. Double-click file → `file_assoc_open()`; folder → navigate. Navigation history stack (16 entries).

**Files:** `src/apps/filemgr/filemgr.c` (new), `include/apps/filemgr.h` (new), `Makefile` (extend)

> [!NOTE]
> Window dimensions: default 980×560 px (the mockup size). Bands, top to bottom: title bar with tabs = 40 px (`THEME_SIZE_TAB_HEIGHT` tabs, mica when active); address row = 48 px; command bar = 48 px with a 1 px `stroke_divider` below; body = navigation pane `220` px + file area; status bar = 24 px. **Address row buttons** (36×32 subtle buttons): Back disabled at history start; Forward disabled at end (`text_disabled`); Up disabled at drive root; Refresh; then the breadcrumb field (location icon + path segments with chevrons; click-to-edit as a text box; Enter → `filemgr_navigate(typed_path)`) and a 260 px search box. **Navigation history**: `char g_history[16][256]`; `g_hist_pos` index; `filemgr_navigate(path)` pushes to history; Back = `g_hist_pos--`; Forward = `g_hist_pos++`. **Directory listing**: `filemgr_refresh()` -- open dir node via `vfs_finddir_path(path)` (helper to walk path components); iterate `vfs_readdir(node, i)` until NULL; `vfs_stat(entry_path, &st)` per entry; fill `g_entries[]` (name, path, size, type, mtime, icon_id). **Render file area**: initially Icon view (§3). **Status bar**: "N items" or "N items selected, total X KB". `mkdir("src/apps/filemgr/")` needed in `Makefile`.

- [ ] `mkdir -p src/apps/filemgr/` + `include/apps/` + add to Makefile compile rules
- [ ] `typedef struct { char name[64]; char full_path[256]; uint8_t is_dir; uint64_t size; int64_t mtime; int icon_id; } fm_entry_t;`
- [ ] `fm_entry_t g_entries[4096]` + `int g_entry_count` as module globals
- [ ] `void filemgr_navigate(const char *path)` -- push to history; `filemgr_refresh()`
- [ ] `void filemgr_refresh(void)` -- `vfs_readdir` loop; `vfs_stat` each; fill `g_entries`; sort by name; redraw
- [ ] `void filemgr_open(void)` -- `wm_create_window(..., 980, 560, "File Explorer", ...)`; render the five bands
- [ ] Address row: Back/Forward/Up (check history/root); Refresh; breadcrumb field; search box. Command bar: accent New, then cut/copy/paste/rename/share/delete, Sort, View, overflow
- [ ] Status bar: render "N items" or "N items selected, total X KB" via `ttf_draw_string()`
- [ ] Double-click: if `is_dir` → `filemgr_navigate(entry.full_path)`; else → `file_assoc_open(full_path)`
- [ ] Up button: strip last path component from `current_path`; `filemgr_navigate(parent_path)`
- [ ] Lay out File Explorer to `docs/design/shell.md#file-explorer`: tabbed 40 px title bar, 48 px address row, 48 px command bar, 220 px navigation pane, drive tiles with usage bars, 24 px status bar
  - Title bar: active tab 220 px minimum on `layer_bg` with top radius 8, then a new-tab button and the 46 px caption buttons; mica when active
  - Address row: back, forward, up, refresh (36x32), breadcrumb with the location icon, 260 px search field; command bar: accent New, cut/copy/paste/rename/share/delete, Sort, View, overflow
  - Nav pane: Desktop, Downloads, Documents, Pictures, Music, Videos, separator, This PC, drives, Network; the selected item gets a 3 px accent bar
  - Content: "Devices and drives" with 48 px drive icons and 6 px accent usage bars, then "Folders" as 48 px folder tiles
- [ ] Commit: `"filemgr: File Explorer frame -- tabbed title bar, address row, command bar, nav pane, status bar, vfs_readdir listing, history"`

## 2. Sidebar `[Sonnet]`

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

The navigation pane of `docs/design/shell.md#file-explorer`: one flat list -- Desktop, Downloads, Documents, Pictures, Music, Videos, a separator, then This PC, each mounted drive, and Network. Items are 32 px with 16 px icons from the original set; the selected item gets `subtle_fill_hover` and a 3 px accent bar at its left edge. Free space per drive shows in the This PC content view ("Devices and drives" tiles with 6 px accent usage bars), not in the pane.

**Files:** `src/apps/filemgr/filemgr.c` (extend)

> [!NOTE]
> Pane width: 220 px, 8 px inner padding. **Folders**: `C:\Users\Default\{Desktop,Downloads,Documents,Pictures,Music,Videos}` with the matching `folder_*` icons; substitute `auth_get_userprofile()` (`09-desktop-shell/TODO-06`) when auth is live. **Drives section**: enumerate drive letters `C:` through `Z:` using `vfs_find_mount(letter)` to check if mounted; for each: `vfs_stat(drive_root, &st)` for free/total bytes; display "C:\ -- IXFS -- 12.4 GB free". **Free space**: `vfs_get_free_space(path)` helper (add to VFS) or read from IXFS superblock. **Highlight**: draw accent left-border (3 px `THEME_ACCENT`) on active path row. Click on any row → `filemgr_navigate(path)`. **Collapse**: `g_sidebar_quick_collapsed` + `g_sidebar_drives_collapsed` bool toggles.

- [ ] `void filemgr_render_sidebar(gfx_surface_t *s, int x, int y, int w, int h)` -- draw sections + rows
- [ ] Folder entries in design order (Desktop, Downloads, Documents, Pictures, Music, Videos) with `folder_*` icons; separator; This PC; drives; Network
  - Network lists discovered machines and their shares -> XREF: `07-networking/TODO-08-ssh-ftp-clients.md` §10 (Network Browsing and SMB Client)
- [ ] Drive enumeration: `for c in 'A'..'Z'`: `vfs_find_mount(c)` → if mounted: add drive entry with free space
- [ ] `vfs_get_free_space(path, free_bytes_out)` new VFS helper function (reads IXFS/FAT32 metadata)
- [ ] Active item highlight: compare row path to `g_current_path`; draw 3 px accent left bar
- [ ] Commit: `"filemgr: navigation pane -- design-order folders, This PC, drives, Network, accent selection bar"`

## 3. View Modes `[Sonnet]`

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

Icon view and details view per `docs/design/controls.md#list-tree-and-grid-views`: icon view tiles use 48 px icons and a two-line caption; details view is a `CTRL_LISTVIEW` table (Name, Date modified, Type, Size; sortable columns) with 32 px rows. The command bar "View" menu switches between them. Single/multi-select (Ctrl+click, Shift+click). Persist in Registry.

**Files:** `src/apps/filemgr/filemgr.c` (extend)

> [!NOTE]
> **Icon view**: tile = 96 px wide (the mockup `.folder` tile: 10 px top padding, 48 px icon, 6 px gap, two-line caption); grid columns = `(file_area_w) / 96`; `icon_draw_scaled(s, entry.icon_id, cx+16, cy, 48, 48)`; filename below (truncate to 10 chars + "…" if longer; 2 lines allowed; `ttf_draw_string` centered). Hover: `subtle_fill_hover`; selected: `selection_fill` with a 1 px `selection_stroke`, radius 4 (`controls.md#list-tree-and-grid-views`). **Detail view**: forward dep on `CTRL_LISTVIEW` (D08 T06 §1); before it's ready: hand-draw the table with a 32 px header and 32 px rows, no alternating fills. Columns per `docs/design/shell.md#file-explorer`: Name (240 px), Date modified (160 px), Type (120 px), Size (80 px right-aligned), all resizable. **Sorting**: `g_sort_column` + `g_sort_ascending`; click header → toggle; `qsort(g_entries, g_entry_count, sizeof(fm_entry_t), compare_fn)`. Compare fns: `fm_cmp_name`, `fm_cmp_size`, `fm_cmp_type`, `fm_cmp_mtime`. **Multi-select**: `uint8_t g_selected[4096]` bool array (index matches `g_entries[]`); Ctrl+click = toggle; Shift+click = range; single click = exclusive select. `g_selection_count` + `g_selection_size`. **Date**: `time_to_datetime(mtime, g_tz_offset_min, &dt)` → `time_format(&dt, buf, 32, "%d/%m/%Y %H:%M")`. **View persist**: `RegSetValueEx(HKCU\\...\\ViewMode, icon=0/detail=1)`.

- [ ] `#define FM_VIEW_ICON 0`, `FM_VIEW_DETAIL 1` + `g_view_mode` global
- [ ] `void filemgr_render_icon_view(s, x, y, w, h)` -- grid layout; `icon_draw_scaled()` + filename label; selected border
- [ ] `void filemgr_render_detail_view(s, x, y, w, h)` -- column headers + 32 px rows (no alternating backgrounds); hover/selection fills + 3 x 16 accent pill; use `CTRL_LISTVIEW` when available
- [ ] Column sort: `g_sort_column` + `g_sort_ascending`; header click handler; `qsort`
- [ ] `uint8_t g_selected[4096]`; click handler: single / Ctrl+click / Shift+range select
- [ ] `g_selection_count`, `g_selection_size` updated on every selection change; status bar refreshed
- [ ] Folders: icon `folder_closed` (or the matching `folder_*` icon for known folders); size blank; type "File folder"
- [ ] Command bar "View" menu (context menu engine): Large icons / Details; choice sets `g_view_mode`; Registry save
- [ ] Commit: `"filemgr: icon view, detail view, column sort, multi-select Ctrl+Shift+click, view mode persist"`

## 4. File Operations `[Sonnet]`

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

Ctrl+C → `clipboard_set(CLIP_FILES, path_list)`. Ctrl+X → cut mark. Ctrl+V → paste (copy or move). Delete → `trash_delete()`. Shift+Delete → `vfs_unlink()`. F2 → inline rename. Ctrl+Shift+N → new folder. Progress dialog > 1 MB. Ctrl+Z → `trash_restore()`.

**Files:** `src/apps/filemgr/filemgr.c` (extend), `src/apps/filemgr/filemgr_ops.c` (new)

> [!NOTE]
> **Copy**: `filemgr_copy_file(src, dst)` -- `pmm_alloc_contiguous(16)` for 64 KiB copy buffer; `vfs_open(src)` → loop `vfs_read()` + `vfs_write(dst_node, ...)` in 64 KiB chunks; free PMM buffer. **Cut/paste**: `g_clipboard_is_cut` flag; on Ctrl+V: if cut → `vfs_rename(src, dst_dir\\src_name)` (within-fs fast path) or copy+unlink (cross-fs); clear cut mark and grey overlay. **Multi-file paste**: if `clipboard_get(CLIP_FILES)` returns a list (`|`-separated paths): iterate each. **Progress dialog**: `filemgr_progress_show(filename, total_size)` -- non-modal window (300×120 px); updated via `filemgr_progress_update(bytes_done, total)`: progress bar + "Copying X.Y KB/s" + "X of N files"; Cancel button → `g_op_cancelled = 1`. **F2 inline rename**: replace filename label/cell with `CTRL_TEXTBOX` pre-filled with current name; Enter → `vfs_rename(old_path, new_path)`; Escape → cancel; invalid chars `\/:*?"<>|` blocked. **Ctrl+Z undo**: `g_last_trash_name[32]`; `trash_restore(g_last_trash_name)` → `filemgr_refresh()`. **New folder**: `vfs_mkdir(current_path\\New Folder)` → F2 inline rename immediately.

- [ ] `int filemgr_copy_file(const char *src, const char *dst)` -- PMM 64 KiB buffer; chunked read/write; return 0 on success
- [ ] `void filemgr_copy_selected(void)` -- iterate `g_selected[]`; build path list; `clipboard_set(CLIP_FILES, list, len)`
- [ ] `void filemgr_cut_selected(void)` -- same as copy + set `g_clipboard_is_cut=1`; grey overlay on cut items
- [ ] `void filemgr_paste(void)` -- `clipboard_get(CLIP_FILES)`; for each path: copy or `vfs_rename`; refresh
- [ ] `void filemgr_delete_selected(int permanent)` -- permanent: `vfs_unlink()`; else: `trash_delete()` + store in `g_last_trash_name`
- [ ] `void filemgr_rename_inline(int entry_idx)` -- replace cell label with `CTRL_TEXTBOX`; Enter=confirm; Escape=cancel; char filter
- [ ] `void filemgr_new_folder(void)` -- `vfs_mkdir(new_path)` + auto-trigger rename inline
- [ ] `void filemgr_progress_show/update/hide(...)` -- non-modal 300×120 window; progress bar + speed label + Cancel
- [ ] Keyboard handler: `Ctrl+C` → copy; `Ctrl+X` → cut; `Ctrl+V` → paste; `Del` → trash; `Shift+Del` → permanent; `F2` → rename; `Ctrl+Shift+N` → new folder; `Ctrl+Z` → undo
- [ ] Name-conflict dialog on copy and move: Replace, Skip, or Keep both (renames to `name (2).ext`), with "Do this for the next N conflicts" and a side-by-side size and date comparison
  - Uses the standard dialog (`docs/design/controls.md#dialog`); the safe choice (Skip) is the default button; applies equally to paste, drag and drop, and restore from the Recycle Bin
- [ ] Commit: `"filemgr: file ops -- copy/cut/paste, trash delete, inline F2 rename, new folder, progress, Ctrl+Z undo"`

## 5. Context Menus `[Sonnet]`

**Design:** [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)

Right-click file → Open/Open With/Cut/Copy/Delete/Rename/Properties. Right-click folder → same + Open in New Window. Right-click empty → New (Folder/Text File/Shortcut)/View/Sort By/Paste/Select All. Properties dialog: name/path/size/type/dates/permissions.

**Files:** `src/apps/filemgr/filemgr.c` (extend)

> [!NOTE]
> Context menus via `context_menu_show()` (08-graphics-ui/TODO-09 §1 forward dep). **File menu**: `{ "Open", filemgr_open_selected }, { "Open With...", filemgr_openwith }, { "---" }, { "Cut", filemgr_cut_selected }, { "Copy", filemgr_copy_selected }, { "---" }, { "Delete", filemgr_delete_selected }, { "Rename", filemgr_rename_inline }, { "---" }, { "Properties", filemgr_show_properties }`. **Empty area menu**: `{ "New Folder", filemgr_new_folder }, { "New Text File", filemgr_new_text_file }, { "New Shortcut", filemgr_new_shortcut }, { "---" }, { "View", submenu: Icon/Detail }, { "Sort By", submenu: Name/Size/Type/Date }, { "---" }, { "Paste", filemgr_paste }, { "Select All", filemgr_select_all }`. **Properties dialog**: 380×300 px modal: icon (48 px) + name top; table rows: Full Path, Size (bytes + human), Type (extension or "Folder"), Created, Modified; Permissions section: `security_check_access()` result + `i_uid/i_mode` from `vfs_stat()` (IXFS only). **Open With**: `dialog_open_with(ext)` from TODO-02 §4 -- launches "Open With" dialog listing registered apps.

- [ ] `void filemgr_show_file_context_menu(int entry_idx, int mx, int my)` -- `context_menu_show()` with file entries
- [ ] `void filemgr_show_folder_context_menu(int entry_idx, int mx, int my)` -- file entries + "Open in New Window"
- [ ] `void filemgr_show_empty_context_menu(int mx, int my)` -- New/View/Sort/Paste/Select All
- [ ] `void filemgr_show_properties(int entry_idx)` -- dialog per `docs/design/controls.md#dialog` (card, radius 8, smoke scrim, 80 px footer with OK/Cancel/Apply); icon + name + stats table + permissions
- [ ] Menus use the `shell.md#context-menus` geometry (256 px, 32 px items, 16 px glyphs, shortcut hints) and end with "Show more options" (Shift+F10)
  - the Windows 11 compact icon row (Cut, Copy, Rename, Share, Delete; Paste when applicable) sits at the top of file and folder menus per `docs/design/shell.md#context-menus` (40 px row of 32 px icon-only subtle buttons with tooltips)
- [ ] Properties: size = `st.size` formatted + bytes; dates via `time_format()`; perms from `vfs_stat()`
- [ ] `void filemgr_new_text_file(void)` -- `vfs_create("New Text File.txt")`; inline rename
- [ ] `void filemgr_new_shortcut(void)` -- `shortcut_create(path, ...)` (TODO-02 §5); inline rename
- [ ] `void filemgr_select_all(void)` -- `memset(g_selected, 1, g_entry_count)`; refresh status bar
- [ ] WM right-click: hit-test → file/folder/empty → dispatch to correct menu
- [ ] Commit: `"filemgr: context menus -- file/folder/empty, Properties dialog with size/dates/perms, Open With"`

## 6. File Search `[Sonnet]`

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

The 260 px search box at the right of the address row ("Search <location>") → `search_query_scoped()` constrained to current directory. Filter as user types (150 ms debounce). Results shown in file area. Clear button → normal directory view.

**Files:** `src/apps/filemgr/filemgr.c` (extend)

> [!NOTE]
> → XREF: `09-desktop-shell/TODO-05-file-search.md §5` -- this section is the implementation of the File Manager integration described there. Search bar: `CTRL_TEXTBOX` rightmost in toolbar; `×` clear button beside it. Debounce: same pattern as Start Menu search (§3 of TODO-05): `g_search_debounce_tick`; set on keystroke; `filemgr_tick()` fires `filemgr_do_search()` when debounce elapsed. `filemgr_do_search(query)`: `search_query_scoped(query, g_current_path, g_search_results, 512)` → convert results to `fm_entry_t[]` + set `g_in_search_mode=1`; render as current view mode. `g_in_search_mode` shows a banner "Search results for '{query}' in {current_path}" above file area. Click × or clear box → `g_in_search_mode=0`; `filemgr_refresh()`. In search mode: file area shows results from any depth of subdirectory; path column shows full path in detail view.

- [ ] `g_search_debounce_tick` + `g_in_search_mode` + `g_search_query[64]` globals
- [ ] Search textbox `WM_KEYDOWN` handler → set dirty; update debounce tick
- [ ] `filemgr_tick()` debounce check → `filemgr_do_search(query)` when elapsed
- [ ] `filemgr_do_search(query)` → `search_query_scoped()` → convert to `fm_entry_t[]`; set `g_in_search_mode`
- [ ] Search banner: accent-bg label "Search results for '...' in C:\..." + × dismiss button
- [ ] Clear / × button → `g_in_search_mode=0`; `filemgr_refresh()`; address bar restored
- [ ] In detail view search results: add full-path column (or tooltip on hover)
- [ ] Commit: `"filemgr: search -- toolbar search bar, search_query_scoped, 150ms debounce, in-search banner, clear"`

## 7. Drag and Drop `[Sonnet]`

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome)

Drag files from file area to desktop (create shortcut or copy). Drag between two File Manager windows. Drag to trash icon → `trash_delete()`. Drop files onto File Manager window (copy to that directory). Semi-transparent icon stack ghost with item-count badge.

**Files:** `src/apps/filemgr/filemgr.c` (extend)

> [!NOTE]
> Forward dep on `wm_drag_start()` (`08-graphics-ui/TODO-08` §7 drag-drop engine). **Drag start**: `WM_MOUSE_DOWN` + move > 4 px threshold on a selected entry → `wm_drag_start(drag_data)` where `drag_data` contains: path list, source window id, operation type (COPY/MOVE). **Ghost image**: semi-transparent surface (60 px wide): stack of first 3 item icons (each offset by 3 px); badge circle top-right with item count (if > 1). Ghost follows mouse via compositor float overlay. **Drop targets**: file area of another File Manager window → copy/move; desktop area → `shortcut_create(path, desktop_path)` (shortcut) or `filemgr_copy_file()` (copy); trash icon → `trash_delete(path)` for each dragged file. **Drop feedback**: highlight drop target area with accent border while dragging over. **Intra-window drag**: drag from file area to sidebar Quick Access folder → copy to that dir. **Source formatting**: `wm_drag_get_paths(drop_event)` returns null-separated path list from drag payload.

- [ ] `void filemgr_drag_start(void)` -- called when mouse moved > 4 px threshold; build drag payload; `wm_drag_start()`
- [ ] `filemgr_build_ghost_surface()` -- PMM surface; stack first 3 item icons; count badge circle if > 1
- [ ] `WM_DROP` handler in File Manager window: receive dropped paths; `filemgr_copy_file()` each to `g_current_path`
- [ ] Desktop `WM_DROP` handler (forward ref to desktop.c): if path is file → offer "Copy here / Create shortcut"
- [ ] Trash icon `WM_DROP` handler: `trash_delete(path)` for each dropped file; update trash icon state
- [ ] Drop highlight: `gfx_draw_rect_outline(s, drop_target_rect, THEME_ACCENT, 2)` while drag over
- [ ] Sidebar drop: drop on a navigation pane folder row → `filemgr_copy_file()` to that row's path
- [ ] Commit: `"filemgr: drag and drop -- ghost with count badge, drop to desktop/trash/other window, sidebar drop target"`

## 8. Advanced Features `[Sonnet]`

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

Preview pane (toggle right panel: text via `ttf_draw_string`, images via `stb_image`, thumbnails). Directory tabs in the 40 px title bar per `docs/design/shell.md#file-explorer` and `controls.md#tabs` (not a strip below the command bar). Breadcrumb address bar (`This PC › Local Disk (C:) › Users`). FTP/UNC path stub.

**Files:** `src/apps/filemgr/filemgr.c` (extend), `src/apps/filemgr/filemgr_preview.c` (new)

> [!NOTE]
> **Preview pane**: toggle button in toolbar (View → Preview Pane); splits right side of file area (300 px wide divider); single-click file → render preview. Text files (< 64 KB): `vfs_read()` → `ttf_draw_string()` wrapped at panel width. Images: `stbi_load_from_memory()` → scale to fit panel. Other: show icon (128 px) + metadata (name, size, type, dates). **Tabs**: `CTRL_TABSTRIP` hosted IN the 40 px title bar (selected tab on `layer_bg` with radius 8 top corners joining the address row, new-tab button after the last tab, caption buttons at the right); each tab = `fm_tab_t { char path[256]; fm_entry_t entries[4096]; sel_t selection; }`. New tab: Ctrl+T. Close tab: Ctrl+W or × on tab. Switch: click. Each tab maintains independent navigation state. **Breadcrumb**: replace address-bar textbox with clickable path segments: "C:\" is a button → navigate; "›" separator; each component is a button → navigate to that level; last component = grayed. Click textbox area (not a segment) → switch back to plain textbox mode for typing. **FTP stub**: `filemgr_navigate("ftp://host/path")` → `klog(LOG_WARN, "filemgr", "FTP not yet supported")` + show "FTP not supported yet" status bar; UNC `\\server\share` → same stub. Forward ref to `07-networking/TODO-08-ssh-ftp.md`.

- [ ] `void filemgr_render_preview(gfx_surface_t *s, int x, int y, int w, int h, int entry_idx)` -- dispatch by type
- [ ] Text preview: `vfs_read()` + `ttf_draw_string()` wrapped at panel width; scroll if tall
- [ ] Image preview: `stbi_load_from_memory()` + scale-to-fit via `gfx_blit_scaled()` or `icon_draw_scaled()`
- [ ] Preview pane resize handle: drag to adjust 150–400 px; persist width in Registry
- [ ] `fm_tab_t g_tabs[8]`; `CTRL_TABSTRIP` widget (08-graphics-ui/TODO-05 §6 forward dep); Ctrl+T/W; per-tab state
- [ ] Breadcrumb: parse `g_current_path` into segments; draw as clickable button chain
- [ ] Breadcrumb: click on area outside segments → switch to textbox mode; focus textbox
- [ ] FTP/UNC: `if (strncmp(path, "ftp://", 6) == 0 || ...)` → status bar warning stub
- [ ] Commit: `"filemgr: advanced -- preview pane text/image, CTRL_TABSTRIP tabs, breadcrumb address bar, FTP stub"`

---

## OS Comparison


| ⭐  | Feature         | 🪟 Win11                                                       | 🐧 Linux                                                                   | 🚀 Impossible OS                                                               |
| --- | --------------- | -------------------------------------------------------------- | -------------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| 💎  | Core layout     | ✅ File Explorer: Ribbon (simplified in                        | ✅ Nautilus/Dolphin: toolbar; sidebar; main area;                          | ⬜ §1 -- 4-zone layout; 16-entry history stack                                 |
| 💎  | Sidebar         | ✅ File Explorer nav pane: Quick                               | ✅ Nautilus bookmarks + Drives (GIO                                        | ⬜ §2 -- nav pane: folders + drives hardcoded + `auth_get_userprofile()`       |
| 💎  | View modes      | ✅ Extra-large/large/medium/small icons; List; Details; Tiles; | ✅ Nautilus: icons/list/compact; Dolphin: icons/list/compact/details; sort | ⬜ §3 -- Icon (48 px) + Detail                                                 |
| 💎  | File operations | ✅ Full copy engine with speed                                 | ✅ Nautilus/Dolphin: copy/move/delete/rename/undo; Trash; progress dialog  | ⬜ §4 -- `filemgr_copy_file` PMM chunked; progress non-modal                   |
| 💎  | Context menus   | ✅ Full context menu; shell extensions;                        | ✅ Nautilus/Dolphin: context menus; Properties with                        | ⬜ §5 -- `context_menu_show()` menus; Properties shows `i_uid/i_mode`          |
| ⭐  | File search     | ✅ File Explorer search bar uses                               | ✅ Nautilus/Dolphin search bars; Tracker/Baloo indexed;                    | ⬜ §6 -- `search_query_scoped()` (TODO-05) instantly returns; accent-highlight |
| 💎  | Drag and drop   | ✅ Full OLE drag-drop; ghost thumbnail                         | ✅ Nautilus/Dolphin: drag to sidebar/desktop/other windows;                | ⬜ §7 -- PMM ghost surface stack of                                            |
| 💎  | Advanced        | ✅ Preview pane; Details pane; tabbed                          | ✅ Dolphin: preview pane; tabs (Ctrl+T);                                   | ⬜ §8 -- preview pane text+image via `stb_image`                               |

> **After §1–§8:** Impossible OS has a full-featured File Manager. The `⭐` differentiator: file search is instant at any depth of the current directory tree because it routes through the kernel PMM index from TODO-05 -- no waiting for an indexer to catch up, no full VFS walk on demand.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] File Explorer opens at This PC: 40 px tabbed title bar, 48 px address row, 48 px command bar, 220 px navigation pane, "Devices and drives" + "Folders", 24 px status bar "N items" (matches `impossibleos.co/design/`)
- [ ] Double-click `C:\Users\Default\Documents\` → navigates; Back (←) → returns to C:\
- [ ] Navigation pane: "Documents" → navigates with the accent bar on it; This PC shows every mounted drive as a tile with a usage bar and free space
- [ ] Icon view: icons drawn at 48 px; switch to Details view → Name, Date modified, Type, Size columns visible; click Name header → alphabetical sort; click again → reverse
- [ ] Multi-select: Ctrl+click 3 files → status bar shows "3 items selected"; Shift+click range → range highlighted
- [ ] Ctrl+C selected files → `clipboard_get(CLIP_FILES)` returns path list; Ctrl+V in different dir → files copied
- [ ] Delete key → file moved to trash; `trash_count()` increments; Ctrl+Z → file restored; `trash_count()` decrements
- [ ] F2 on file → inline rename textbox; type new name + Enter → file renamed; Escape → unchanged
- [ ] Right-click file → context menu; click Properties → modal with size, dates, permissions shown
- [ ] Type "Inter" in search bar → file area shows matching results from current subtree after 150 ms; click × → normal dir listing restored
- [ ] Drag file from file area → ghost appears; drop onto desktop → shortcut created; drop on the Recycle Bin → file moved to the Recycle Bin
- [ ] A framebuffer capture of File Explorer at This PC matches the `impossibleos.co/design/?shot` window in dark and light within the perceptual diff threshold of `00-infrastructure/TODO-05 §9`
- [ ] Commit: `"filemgr: file manager -- all sections complete"`
