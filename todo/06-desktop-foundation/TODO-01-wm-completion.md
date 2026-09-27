---
schema_version: 1
id: wm-completion
domain: 06-desktop-foundation
status: active
title: "TODO-01 -- Window Manager Completion"
---

# TODO-01 -- Window Manager Completion

> **Goal:** Superseded. The window manager completion work planned here is owned by `08-graphics-ui/TODO-08-window-manager.md`, which implements it to `docs/design/shell.md` (window chrome, snap layouts, Alt+Tab). Each section below points at its owner so no second implementation is built.

> [!IMPORTANT]
> The existing `wm.c` (1143 lines) has window create/destroy/move/focus/z-order and title bar decorations with Mica effect. What's missing: minimize, maximize, restore, resize-by-edge, snap layouts, and system hotkeys. The `TODO: wm_minimize()` and `TODO: wm_maximize()` stubs are already in the code.

## Inputs

- [`src/desktop/wm.c`](../../src/desktop/wm.c) -- existing WM with drag, focus, decorations
- [`include/desktop/wm.h`](../../include/desktop/wm.h) -- window struct, flags, API

## Outcome

- Every capability of this file is owned by `08-graphics-ui/TODO-08-window-manager.md` (§1 min/max/restore, §2 decorations and 5 px edge resize, §3 snap layouts, §5 hotkeys, §6 Alt+Tab), which follows `docs/design/shell.md`. This file is kept as a superseded pointer so no second implementation is built.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎  |   1   | Minimize, maximize, restore (superseded) | --         |  [/]   |
| 💎  |   2   | Resize by dragging window edges          | --         |  [/]   |
| 💎  |   3   | Snap to left/right half, top=maximize    | §1, §2     |  [/]   |
| 💎  |   4   | Global hotkeys (Alt+Tab, Alt+F4, Win+D)  | §1         |  [/]   |
---

## 1. Minimize, Maximize, Restore

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome)
Minimize, maximize and restore are owned by the canonical window manager roadmap, `08-graphics-ui/TODO-08` §1, including caption-button wiring and taskbar restore.

**Files:** `src/desktop/wm.c`, `include/desktop/wm.h`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §1`, which follows `docs/design/shell.md#window-chrome`; do not build a second minimize/maximize/restore path here

**Test checkpoint:** Click minimize -- window disappears. Click taskbar entry -- window restores. Click maximize -- fills screen. Click maximize again -- restores to original size.

## 2. Resize by Dragging Window Edges

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome)
Edge and corner resize is owned by `08-graphics-ui/TODO-08` §2: an invisible grab zone of `THEME_SIZE_RESIZE_MARGIN` (5 px) outside the visible edge, all 8 directions, with resize cursors.

**Files:** `src/desktop/wm.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §2`, which follows `docs/design/shell.md#window-chrome`; do not build a second edge-resize path here

**Test checkpoint:** Drag bottom-right corner of terminal window -- it resizes. Cursor changes to resize arrows when hovering edges.

## 3. Snap to Left/Right Half

**Design:** [`shell.md#snap-layouts`](../../docs/design/shell.md#snap-layouts)
Snapping is owned by `08-graphics-ui/TODO-08` §3: the six-layout snap flyout (maximize-button hover or Win+Z) and edge-drag preview, per `docs/design/shell.md#snap-layouts`.

**Files:** `src/desktop/wm.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §3`, which follows `docs/design/shell.md#snap-layouts`; do not build a second snap implementation here

**Test checkpoint:** Drag window to left edge -- snaps to left half. Drag to right -- right half. Drag to top -- maximizes. Drag title bar away -- restores.

## 4. Global Hotkeys

**Design:** n/a -- keyboard dispatch only; it draws nothing
The global hotkey table (Alt+F4, Win+D, Win+L, Win+arrows) is owned by `08-graphics-ui/TODO-08` §5 and the Alt+Tab switcher by §6.

**Files:** `src/desktop/wm.c`, `src/desktop/desktop.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-08-window-manager.md §5 and §6`, which follows `docs/design/shell.md#alttab`; do not build a second hotkey table or Alt+Tab switcher here

**Test checkpoint:** Alt+Tab cycles between terminal and gallery. Alt+F4 closes focused window. Win+D shows desktop.

---

## OS Comparison

| ⭐  | Feature         | 🪟 Win11    | 🐧 Linux (GNOME) | 🚀 Impossible OS |
| --- | --------------- | ----------- | ---------------- | ---------------- |
| 💎  | Min/Max/Restore | ✅ Built-in | ✅ Built-in      | ⬜ §1            |
| 💎  | Edge resize     | ✅ Built-in | ✅ Built-in      | ⬜ §2            |
| ⭐  | Snap layouts    | ✅ 6-zone   | ❌ Manual tiling | ⬜ 08 TODO-08 §3 |
| 💎  | Alt+Tab         | ✅ Built-in | ✅ Built-in      | ⬜ §4            |
