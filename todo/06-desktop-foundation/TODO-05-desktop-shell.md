---
schema_version: 1
id: desktop-shell
domain: 06-desktop-foundation
status: active
title: "TODO-05 -- Desktop Shell Completion"
---

# TODO-05 -- Desktop Shell Completion

> **Goal:** Superseded. Every behaviour planned here is owned in `08-graphics-ui/` and built to `docs/design/shell.md`: desktop icons (`TODO-08` §4 plus `06-desktop-foundation/TODO-06` §2), the desktop menu (`TODO-09` §2), the taskbar (`TODO-10` §1) and Start (`TODO-11` §1-§3). Sections below are pointers so no second implementation is built.

## Inputs

- [`src/desktop/desktop.c`](../../src/desktop/desktop.c) -- taskbar, start menu, desktop icons (visual only)
- [`src/desktop/wm.c`](../../src/desktop/wm.c) -- window lifecycle, minimize/maximize (from TODO-01)
- `08-graphics-ui/TODO-08-window-manager.md` §4, `TODO-09-desktop-shell-features.md` §1-§2, `TODO-10-taskbar.md` §1, `TODO-11-startmenu-tray-notifications.md` §1-§3 -- the canonical owners of everything planned here

## Outcome

- No second implementation: each section points at its canonical `08-graphics-ui/` owner, which follows `docs/design/shell.md`.
- The power menu's shutdown and restart reach UEFI `ResetSystem()`.

## Implementation Order

| ⭐  | Order | Deliverable                      | Depends On | Status |
| --- | :---: | -------------------------------- | ---------- | :----: |
| 💎  |   1   | Desktop icon click actions       | --         |  [/]   |
| 💎  |   2   | Desktop right-click context menu | D08 T09 §2 |  [/]   |
| 💎  |   3   | Taskbar window state sync        | D08 T10 §1 |  [/]   |
| 💎  |   4   | Start menu program launch        | --         |  [/]   |
| 💎  |   5   | Power and settings buttons       | --         |  [/]   |
---

## 1. Desktop Icon Click Actions

**Design:** [`shell.md#desktop`](../../docs/design/shell.md#desktop), [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)
Desktop icon selection and double-click launch are owned by `08-graphics-ui/TODO-08` §4; the five special icons and their targets by `06-desktop-foundation/TODO-06` §2.

**Files:** `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §4`, which follows `docs/design/shell.md#desktop`; do not build a second double-click/select path here
- [ ] Special-icon targets are defined in `06-desktop-foundation/TODO-06` §2 (This PC, Recycle Bin, user folder, Network, Control Panel); this section adds nothing beyond them

**Test checkpoint:** Double-click "This PC" opens File Explorer at This PC; single-click shows the selection fill.

## 2. Desktop Right-Click Context Menu

**Design:** [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)
The desktop right-click menu is owned by `08-graphics-ui/TODO-09` §2 on the engine of §1, in the design order: View, Sort by, Refresh | New | Display settings, Personalize | Open in Terminal, Show more options (Shift+F10).

**Files:** `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-09-desktop-shell-features.md §2`, which follows `docs/design/shell.md#context-menus`; do not build a second desktop right-click menu here

**Test checkpoint:** Right-click desktop → menu appears. Click "Refresh" → screen redraws. Click outside → menu closes.

## 3. Taskbar Window State Sync

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)
Taskbar entries, the running and focused indicator pill, and click-to-minimize/restore are owned by `08-graphics-ui/TODO-10` §1.

**Files:** `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-10-taskbar.md §1`, which follows `docs/design/shell.md#taskbar`; do not build a second taskbar window list here

**Test checkpoint:** Open terminal + gallery. Click terminal taskbar entry -- minimizes. Click again -- restores. Click gallery entry -- focuses gallery.

## 4. Start Menu Program Launch

**Design:** [`shell.md#start-menu`](../../docs/design/shell.md#start-menu)
Start is the Windows 11 layout owned by `08-graphics-ui/TODO-11` §1-§3: search on top, a 6 x 3 Pinned grid with an All apps view, Recommended, and a user and power footer. There is no All Programs list and no right column.

**Files:** `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-11-startmenu-tray-notifications.md §1 and §2`, which follows `docs/design/shell.md#start-menu`; do not build a second Start menu here

**Test checkpoint:** Click Start → click the Terminal tile → terminal opens and Start closes (08 TODO-11 §2).

## 5. Power and Settings Buttons

**Design:** [`shell.md#start-menu`](../../docs/design/shell.md#start-menu)
The Start footer (user button left, power button right) and its menus are owned by `08-graphics-ui/TODO-11` §2; Settings is reached from its Pinned tile and the user menu.

**Files:** `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-11-startmenu-tray-notifications.md §2`, which follows `docs/design/shell.md#start-menu`; do not build a second power or user menu here
- [ ] `sys_shutdown()` / `sys_reboot()` used by the power menu reach UEFI `ResetSystem(EfiResetShutdown / EfiResetCold, ...)`; Sleep logs "sleep not implemented" until S3 exists

**Test checkpoint:** Power button → context menu Sleep, Shut down, Restart; Restart reboots the machine.

---

## OS Comparison

| ⭐  | Feature             | 🪟 Win11    | 🐧 Linux (GNOME) | 🚀 Impossible OS |
| --- | ------------------- | ----------- | ---------------- | ---------------- |
| 💎  | Desktop icon launch | ✅ Built-in | ✅ Nautilus      | ⬜ §1            |
| 💎  | Desktop right-click | ✅ Built-in | ✅ Built-in      | ⬜ §2            |
| 💎  | Taskbar state sync  | ✅ Built-in | ✅ Dash/Panel    | ⬜ §3            |
| 💎  | Start menu launch   | ✅ Built-in | ✅ Activities    | ⬜ §4            |
| 💎  | Power/restart       | ✅ Built-in | ✅ Built-in      | ⬜ §5            |
