---
schema_version: 1
id: desktop-icons
domain: 06-desktop-foundation
status: active
title: "TODO-06 -- Desktop Icon System"
---

# TODO-06 -- Desktop Icon System

> **Goal:** A complete desktop icon system: special folder icons, user-created shortcuts, file type icons, grid layout, drag-to-reposition, rename, delete, and right-click context menu per icon. The current desktop has 3 hardcoded icons (Computer, Recycle Bin, Control Panel) -- this makes icons a real, dynamic system.

## Inputs

- [`src/desktop/desktop.c`](../../src/desktop/desktop.c) -- existing 3 hardcoded icons with alpha-blended rendering
- [`include/desktop/desktop.h`](../../include/desktop/desktop.h) -- icon constants, draw functions
- → XREF: `06-desktop-foundation/TODO-04-control-library.md §5` -- context menu engine (right-click on icon)
- → XREF: `06-desktop-foundation/TODO-05-desktop-shell.md §1` -- icon click actions (launch apps)

## Outcome

- Desktop icons loaded dynamically from `C:\Users\Default\Desktop\` directory
- Special folder icons: Computer, Recycle Bin, Network, User's Files (always present)
- Shortcut files (`.lnk`): icon + target path, double-click launches target
- File type icons: `.exe` shows embedded icon, `.txt` shows text icon, etc.
- Grid snap: icons align to invisible grid (80x96 cells)
- Drag-to-reposition: drag icon to new grid position
- Right-click icon: context menu with Open, Rename, Delete, Properties
- Auto-arrange and sort options (name, size, date, type)

## Implementation Order

| ⭐  | Order | Deliverable                             | Depends On         | Status |
| --- | :---: | --------------------------------------- | ------------------ | :----: |
| 💎  |   1   | Icon data model and grid layout         | --                 |  [ ]   |
| 💎  |   2   | Special folder icons (always present)   | §1                 |  [ ]   |
| 💎  |   3   | Dynamic icons from Desktop directory    | §1                 |  [ ]   |
| 💎  |   4   | Icon interaction (select, drag, rename) | §1                 |  [ ]   |
| 💎  |   5   | Shortcut (.lnk) file support            | §3                 |  [ ]   |
| 💎  |   6   | File type icon association              | §3                 |  [ ]   |
| 💎  |   7   | Icon right-click context menu           | §4, D06/TODO-04 §5 |  [ ]   |

---

## 1. Icon Data Model and Grid Layout
Replace hardcoded icons with a dynamic icon list and grid-based positioning.

**Files:** `src/desktop/desktop.c`, `include/desktop/desktop.h`

- [ ] `struct desktop_icon { char name[64]; char target[256]; int grid_x, grid_y; uint8_t type; uint8_t selected; icon_handle_t icon; }`
- [ ] Icon types: `ICON_SPECIAL_FOLDER`, `ICON_SHORTCUT`, `ICON_FILE`, `ICON_DIRECTORY`
- [ ] `desktop_icons[64]` -- max 64 icons on desktop
- [ ] Grid: 80px wide x 96px tall cells, column-first layout (top to bottom, left to right)
- [ ] `desktop_add_icon(name, target, type, icon)` -- add icon at next free grid position
- [ ] `desktop_remove_icon(index)` -- remove icon, compact grid
- [ ] Draw all icons from the dynamic list instead of hardcoded positions
- [ ] Commit

**Test checkpoint:** Desktop shows icons from dynamic list. Same visual as before but data-driven.

## 2. Special Folder Icons (Always Present)
Computer, Recycle Bin, Network, User's Files -- pinned to top of icon grid, can't be deleted.

**Files:** `src/desktop/desktop.c`

- [ ] `desktop_init_special_icons()` -- create the 4 special icons at boot
- [ ] Computer: opens file manager at `C:\` (or placeholder)
- [ ] Recycle Bin: opens recycle bin window (or placeholder)
- [ ] Network: opens network browser (or placeholder)
- [ ] User's Files: opens file manager at `C:\Users\Default\`
- [ ] Special icons have `ICON_SPECIAL_FOLDER` type -- cannot be renamed or deleted
- [ ] Icons use IRES icon store (48px, already loaded)
- [ ] Commit
- [ ] Special folders use the original icon set and grid in `docs/design/shell.md#desktop`: This PC, Recycle Bin (empty/full), user folder, Network, Control Panel
  - 76x86 cells, 48 px icons, caption labels with the soft shadow; hover and selection fills from the tokens

**Test checkpoint:** Desktop shows 4 special folder icons at top-left. Same look as current 3 icons.

## 3. Dynamic Icons from Desktop Directory
Scan `C:\Users\Default\Desktop\` and create icons for each file/folder found.

**Files:** `src/desktop/desktop.c`

- [ ] `desktop_scan_directory()` -- scan Desktop directory via VFS
- [ ] For each file: create icon with filename as label
- [ ] For each subdirectory: create folder icon
- [ ] `.lnk` files: parse shortcut (§5), show target's icon
- [ ] Place after special icons in grid order
- [ ] Re-scan on refresh (context menu "Refresh" or F5)
- [ ] Commit

**Test checkpoint:** Create `C:\Users\Default\Desktop\test.txt` -- icon appears on desktop after refresh.

## 4. Icon Interaction (Select, Drag, Rename)
Click to select, drag to reposition, F2 or slow double-click to rename.

**Files:** `src/desktop/desktop.c`

- [ ] Single-click: select icon (blue highlight rectangle, deselect others)
- [ ] Ctrl+click: toggle selection (multi-select)
- [ ] Drag selected icon: ghost follows cursor, snap to grid on release
- [ ] Double-click: launch (handled by TODO-05 §1)
- [ ] F2 key or slow double-click on selected icon: inline rename (textbox overlay)
- [ ] Delete key on selected icon: move to Recycle Bin (or delete if Shift held)
- [ ] Rubber-band selection: drag on empty desktop area draws selection rectangle
- [ ] Commit

**Test checkpoint:** Click icon -- highlights. Drag icon -- repositions. F2 -- rename inline. Delete -- icon removed.

## 5. Shortcut (.lnk) File Support
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
Show correct icon based on file extension.

**Files:** `src/desktop/desktop.c`

- [ ] Default icons by extension: `.exe` = application, `.txt` = text, `.jpg/.png` = image, `.conf` = config
- [ ] Folder icon for directories
- [ ] Unknown extension: generic file icon
- [ ] Icon lookup: `desktop_icon_for_extension(ext)` -- returns IRES icon handle
- [ ] Future: read icon from `.exe` PE resources (deferred)
- [ ] Commit

**Test checkpoint:** Desktop shows different icons for `.txt`, `.exe`, `.jpg` files.

## 7. Icon Right-Click Context Menu
Per-icon context menu with standard actions.

**Files:** `src/desktop/desktop.c`

- [ ] Right-click on icon: show context menu via `menu_show()` (from TODO-04 §5)
- [ ] Menu items: Open, Open With..., Rename, Delete, Properties
- [ ] "Open" = double-click action
- [ ] "Rename" = enter inline rename mode
- [ ] "Delete" = move to Recycle Bin
- [ ] "Properties" = show Properties dialog (placeholder)
- [ ] Special folder icons: no Delete or Rename options
- [ ] Right-click on empty desktop: different menu (handled by TODO-05 §4)
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
