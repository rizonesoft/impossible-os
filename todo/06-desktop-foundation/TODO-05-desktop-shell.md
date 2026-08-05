---
schema_version: 1
id: desktop-shell
domain: 06-desktop-foundation
status: active
title: "TODO-05 -- Desktop Shell Completion"
---

# TODO-05 -- Desktop Shell Completion

> **Goal:** Make the desktop shell functional: desktop icon clicks launch apps, right-click menu works, taskbar reflects window state (minimize/restore on click), start menu items launch programs, and power button triggers shutdown. The visual elements exist -- this wires them to actions.

## Inputs

- [`src/desktop/desktop.c`](../../src/desktop/desktop.c) -- taskbar, start menu, desktop icons (visual only)
- [`src/desktop/wm.c`](../../src/desktop/wm.c) -- window lifecycle, minimize/maximize (from TODO-01)
- → XREF: `06-desktop-foundation/TODO-01-wm-completion.md` -- minimize/maximize must be implemented first
- → XREF: `06-desktop-foundation/TODO-04-control-library.md §5` -- context menu engine needed for right-click

## Outcome

- Desktop icon double-click: opens associated window (Terminal, File Manager placeholder, etc.)
- Right-click desktop: context menu with Refresh, Display Settings, New Folder
- Taskbar: clicking a window entry minimizes/restores it, shows active window highlighted
- Start menu: clicking "Terminal" opens terminal, "About" shows about dialog
- Power button: triggers `ResetSystem()` or shutdown sequence
- Settings button: opens Control Panel placeholder

## Implementation Order

| ⭐  | Order | Deliverable                      | Depends On     | Status |
| --- | :---: | -------------------------------- | -------------- | :----: |
| 💎  |   1   | Desktop icon click actions       | --             |  [ ]   |
| 💎  |   2   | Desktop right-click context menu | D06/TODO-04 §5 |  [ ]   |
| 💎  |   3   | Taskbar window state sync        | D06/TODO-01 §1 |  [ ]   |
| 💎  |   4   | Start menu program launch        | --             |  [ ]   |
| 💎  |   5   | Power and settings buttons       | --             |  [ ]   |

---

## 1. Desktop Icon Click Actions
Wire desktop icon clicks to launch associated windows/apps.

**Files:** `src/desktop/desktop.c`

- [ ] Double-click detection: track last click time + position, trigger on second click within 500ms and 4px
- [ ] "Computer" icon → open File Manager (placeholder: show "Coming soon" dialog)
- [ ] "Recycle Bin" icon → open Recycle Bin window (placeholder)
- [ ] "Control Panel" icon → open Control Panel (placeholder)
- [ ] Single-click: select icon (highlight with selection rect)
- [ ] Commit

**Test checkpoint:** Double-click "Computer" icon -- placeholder dialog opens. Single-click -- icon highlights.

## 2. Desktop Right-Click Context Menu
Show a context menu when right-clicking on the desktop background.

**Files:** `src/desktop/desktop.c`

- [ ] Detect right-click on desktop (not on window, not on taskbar)
- [ ] Show context menu via `menu_show()` (from TODO-04 §5) with items:
  - "View" → submenu: Large Icons, Medium Icons, Small Icons (placeholder)
  - separator
  - "Refresh" → force compositor redraw
  - "New" → submenu: Folder, Text Document (placeholder)
  - separator
  - "Display Settings" → open Display Settings dialog (placeholder)
  - "Personalize" → open Personalization dialog (placeholder)
- [ ] Commit

**Test checkpoint:** Right-click desktop → menu appears. Click "Refresh" → screen redraws. Click outside → menu closes.

## 3. Taskbar Window State Sync
Taskbar entries reflect actual window state and allow minimize/restore.

**Files:** `src/desktop/desktop.c`

- [ ] Taskbar entries track window handles from WM
- [ ] Active (focused) window: highlighted entry
- [ ] Click entry of focused window → minimize
- [ ] Click entry of minimized window → restore + focus
- [ ] Click entry of unfocused visible window → focus + raise
- [ ] Window destroy → remove entry from taskbar
- [ ] Window create → add entry to taskbar
- [ ] Commit

**Test checkpoint:** Open terminal + gallery. Click terminal taskbar entry -- minimizes. Click again -- restores. Click gallery entry -- focuses gallery.

## 4. Start Menu Program Launch
Start menu items launch actual programs/windows.

**Files:** `src/desktop/desktop.c`

- [ ] "Terminal" → `terminal_open()` (already exists)
- [ ] "About" → open About dialog (WM dialog with version info)
- [ ] "All Programs" → expand to show all .exe files in `C:\` (placeholder: show list)
- [ ] Search bar → filter program list by typed text (future, basic substring match)
- [ ] Right column: "Computer" → file manager, "Documents" → file manager at path
- [ ] Close start menu after launching a program
- [ ] Commit

**Test checkpoint:** Click Start → click "Terminal" → terminal opens, start menu closes. Click "About" → dialog shows OS version.

## 5. Power and Settings Buttons
Wire the bottom buttons in the start menu.

**Files:** `src/desktop/desktop.c`

- [ ] Settings button → open Settings/Control Panel placeholder window
- [ ] Power button → show submenu: "Shutdown", "Restart", "Sleep"
- [ ] Shutdown → call UEFI `ResetSystem(EfiResetShutdown, ...)`
- [ ] Restart → call UEFI `ResetSystem(EfiResetCold, ...)`
- [ ] Sleep → placeholder (log "sleep not implemented")
- [ ] Commit

**Test checkpoint:** Click Power → Shutdown submenu. Click "Restart" → machine reboots. Click "Settings" → placeholder window.

---

## OS Comparison

| ⭐  | Feature             | 🪟 Win11    | 🐧 Linux (GNOME) | 🚀 Impossible OS |
| --- | ------------------- | ----------- | ---------------- | ---------------- |
| 💎  | Desktop icon launch | ✅ Built-in | ✅ Nautilus      | ⬜ §1            |
| 💎  | Desktop right-click | ✅ Built-in | ✅ Built-in      | ⬜ §2            |
| 💎  | Taskbar state sync  | ✅ Built-in | ✅ Dash/Panel    | ⬜ §3            |
| 💎  | Start menu launch   | ✅ Built-in | ✅ Activities    | ⬜ §4            |
| 💎  | Power/restart       | ✅ Built-in | ✅ Built-in      | ⬜ §5            |
