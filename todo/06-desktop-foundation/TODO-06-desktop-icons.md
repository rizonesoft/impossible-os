---
schema_version: 1
id: desktop-icons
domain: 06-desktop-foundation
status: active
title: "TODO-06 -- Desktop Icon System"
---

# TODO-06 -- Desktop Icon System

> **Goal:** A complete desktop icon system: special folder icons, user-created shortcuts, file type icons, grid layout, drag-to-reposition, rename, delete, and right-click context menu per icon. The current desktop has hardcoded icons -- this makes icons a real, dynamic system.

## Inputs

- [`src/desktop/desktop.c`](../../src/desktop/desktop.c) -- existing 3 hardcoded icons with alpha-blended rendering
- [`include/desktop/desktop.h`](../../include/desktop/desktop.h) -- icon constants, draw functions
- `08-graphics-ui/TODO-08-window-manager.md` §4 -- canonical desktop icon list, drawing, select, launch and drag (this file keeps special folders, rename/delete, `.lnk`, file-type icons and icon menu items)
- `08-graphics-ui/TODO-09-desktop-shell-features.md` §1 -- the context menu engine (`context_menu_show()`)

## Outcome

- Desktop icons loaded dynamically from `C:\Users\Default\Desktop\` directory
- Special folder icons: This PC, Recycle Bin, user folder, Network, Control Panel (always present, in that order)
- Shortcut files (`.lnk`): icon + target path, double-click launches target
- File type icons: `.exe` shows embedded icon, `.txt` shows text icon, etc.
- Grid: 76 x 86 cells with 48 px icons (`docs/design/shell.md#desktop`), owned by `08-graphics-ui/TODO-08` §4
- Drag-to-reposition: drag icon to new grid position
- Right-click icon: context menu with Open, Rename, Delete, Properties
- Auto-arrange and sort options (name, size, date, type)

## Implementation Order

| ⭐  | Order | Deliverable                             | Depends On     | Status |
| --- | :---: | --------------------------------------- | -------------- | :----: |
| 💎  |   1   | Icon data model and grid layout         | --             |  [/]   |
| 💎  |   2   | Special folder icons (always present)   | §1             |  [ ]   |
| 💎  |   3   | Dynamic icons from Desktop directory    | §1             |  [/]   |
| 💎  |   4   | Icon interaction (select, drag, rename) | §1             |  [/]   |
| 💎  |   5   | Shortcut (.lnk) file support            | §3             |  [ ]   |
| 💎  |   6   | File type icon association              | §3             |  [ ]   |
| 💎  |   7   | Icon right-click context menu           | §4, D08 T09 §1 |  [ ]   |

---

## 1. Icon Data Model and Grid Layout

**Design:** [`shell.md#desktop`](../../docs/design/shell.md#desktop), [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)
The desktop icon list, grid and drawing are owned by `08-graphics-ui/TODO-08` §4: column-major grid from the top-left, `THEME_SIZE_DESKTOP_MARGIN` (8) from the edges, `THEME_SIZE_DESKTOP_CELL_WIDTH` x `THEME_SIZE_DESKTOP_CELL_HEIGHT` (76 x 86) cells, 48 px icons 6 px from the cell top, two-line caption labels.

**Files:** `src/desktop/desktop.c`, `include/desktop/desktop.h`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §4`, which follows `docs/design/shell.md#desktop`; do not build a second desktop icon list or grid here

**Test checkpoint:** Desktop shows icons from dynamic list. Same visual as before but data-driven.

## 2. Special Folder Icons (Always Present)

**Design:** [`shell.md#desktop`](../../docs/design/shell.md#desktop), [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)

The five default desktop icons, in order: This PC, Recycle Bin, the user folder, Network and Control Panel (`docs/design/shell.md#desktop`). They are added to the canonical icon list of `08-graphics-ui/TODO-08` §4 at boot, sit at the top of the grid, and cannot be renamed or deleted.

**Files:** `src/desktop/desktop_icons.c` (the canonical file from 08 TODO-08 §4)

- [ ] `desktop_init_special_icons()` -- add the 5 special icons, in the design order, through the canonical `desktop_icons` API
- [ ] This PC (`computer` icon) → File Explorer at This PC (`09-desktop-shell/TODO-09`)
- [ ] Recycle Bin (`recycle_bin_empty` / `recycle_bin_full`, switched by `09-desktop-shell/TODO-02` §6) → Recycle Bin window (`09-desktop-shell/TODO-04` §2)
- [ ] User folder (`user_folder` icon, labelled with the user name) → File Explorer at `C:\Users\<user>\`
- [ ] Network (`network` icon) → File Explorer at Network
- [ ] Control Panel (`control_panel` icon) → Control Panel host (`09-desktop-shell/TODO-11`)
- [ ] Special icons have `ICON_SPECIAL_FOLDER` type: no Rename or Delete; icons come from the original set in `resources/icons/src/` at 48 px
- [ ] Commit: `"desktop: five special icons -- This PC, Recycle Bin, user folder, Network, Control Panel"`

**Test checkpoint:** Desktop shows This PC, Recycle Bin, the user folder, Network and Control Panel top-left in that order on the 76 x 86 grid, matching `impossibleos.co/design/`.

## 3. Dynamic Icons from Desktop Directory

**Design:** [`shell.md#desktop`](../../docs/design/shell.md#desktop), [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)
Scan `C:\Users\Default\Desktop\` and create icons for each file/folder found.

**Files:** `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §4`, which follows `docs/design/shell.md#desktop`; do not build a second Desktop-directory scan here
- [ ] Subdirectories get the `folder_closed` icon; `.lnk` entries show their target icon (§5)
- [ ] Re-scan on the desktop menu "Refresh" (08 TODO-09 §2) and on F5

**Test checkpoint:** Create `C:\Users\Default\Desktop\test.txt` -- icon appears on desktop after refresh.

## 4. Icon Interaction (Select, Drag, Rename)

**Design:** [`shell.md#desktop`](../../docs/design/shell.md#desktop), [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)
Click to select, drag to reposition, F2 or slow double-click to rename.

**Files:** `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §4 (select, double-click launch, drag)`, which follows `docs/design/shell.md#desktop`; do not build a second select/launch/drag path here
- [ ] Ctrl+click toggles selection; rubber-band selection on empty desktop draws a `selection_fill` / `selection_stroke` rectangle
- [ ] F2 or slow double-click on a selected icon: inline rename in a text box per `controls.md#text-box-password-box-and-search-box`
- [ ] Delete moves the selection to the Recycle Bin (Shift+Delete deletes)

**Test checkpoint:** Click icon -- highlights. Drag icon -- repositions. F2 -- rename inline. Delete -- icon removed.

## 5. Shortcut (.lnk) File Support

**Design:** [`shell.md#desktop`](../../docs/design/shell.md#desktop), [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)
Simple shortcut format: target path + optional icon override.

**Files:** `src/desktop/desktop.c`, new `include/desktop/shortcut.h`

- [ ] `.lnk` file format: plain text, line 1 = target path, line 2 = icon name (optional)
- [ ] `shortcut_parse(path, target_out, icon_out)` -- read .lnk file
- [ ] Desktop shows shortcut icon with small arrow overlay (bottom-left)
- [ ] Double-click shortcut: launch the target
- [ ] "Create Shortcut" in context menu: create .lnk pointing to selected file
- [ ] Commit

**Test checkpoint:** Create `test.lnk` with target `C:\cmd.exe` -- shows on desktop with arrow overlay. Double-click launches cmd.

## 6. File Type Icon Association

**Design:** [`shell.md#desktop`](../../docs/design/shell.md#desktop), [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)
Show correct icon based on file extension.

**Files:** `src/kernel/icon_store.c`, `include/icon_store.h`

- [ ] Icons by extension from the closed set in `docs/design/icons.md#system-icons`: `.exe` = `exe_default`, `.dll` = `dll_default`, `.txt`/`.md`/`.log`/`.cfg`/`.conf`/`.ini` = `text_file`, `.jpg`/`.png`/`.bmp` = `file_image`, `.zip` = `file_archive`
- [ ] Directories use `folder_closed`
- [ ] Unknown extension: `file_default`; never invent an icon outside the set (a new type needs a new icon in `resources/icons/src/` first)
- [ ] Extend the shipped `icon_for_extension(ext)` (`icon_store.c`, returns `system_icon_t`) rather than adding a second lookup
  - Add icon IDs for `file_image`, `file_archive`, `user_folder` and `network`, whose SVGs exist but have no `system_icon_t` entry
- [ ] Accept an extension without its leading dot and compare case-insensitively (`TXT`, `txt`); today both fall to `ICON_FILE_DEFAULT`, and the comment claiming the dot is skipped has no code
- [ ] Future: read icon from `.exe` PE resources (deferred)
- [ ] Commit

**Test checkpoint:** Desktop shows different icons for `.txt`, `.exe`, `.jpg` files.

## 7. Icon Right-Click Context Menu

**Design:** [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)
Per-icon context menu with standard actions.

**Files:** `src/desktop/desktop.c`

- [ ] Right-click on icon: `context_menu_show()` from `08-graphics-ui/TODO-09` §1 (the only menu engine), drawn per `docs/design/shell.md#context-menus`
- [ ] Menu items: Open, Open With..., Rename, Delete, Properties
- [ ] "Open" = double-click action
- [ ] "Rename" = enter inline rename mode
- [ ] "Delete" = move to Recycle Bin
- [ ] "Properties" = show Properties dialog (placeholder)
- [ ] Special folder icons: no Delete or Rename options
- [ ] Right-click on empty desktop: the desktop menu owned by `08-graphics-ui/TODO-09` §2
- [ ] Commit

**Test checkpoint:** Right-click icon -- context menu. Click "Rename" -- inline rename. Click "Delete" -- icon removed.

---

## OS Comparison

| ⭐  | Feature             | 🪟 Win11    | 🐧 Linux (GNOME) | 🚀 Impossible OS |
| --- | ------------------- | ----------- | ---------------- | ---------------- |
| 💎  | Desktop icons       | ✅ Built-in | ✅ Nautilus      | ⬜ §1-§2         |
| 💎  | Dynamic from folder | ✅ Built-in | ✅ ~/Desktop     | ⬜ §3            |
| 💎  | Drag to reposition  | ✅ Built-in | ✅ Built-in      | ⬜ §4            |
| 💎  | Shortcuts (.lnk)    | ✅ Built-in | ✅ .desktop      | ⬜ §5            |
| 💎  | File type icons     | ✅ Registry | ✅ MIME          | ⬜ §6            |
| 💎  | Icon context menu   | ✅ Built-in | ✅ Built-in      | ⬜ §7            |
