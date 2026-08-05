---
schema_version: 1
id: wm-completion
domain: 06-desktop-foundation
status: active
title: "TODO-01 -- Window Manager Completion"
---

# TODO-01 -- Window Manager Completion

> **Goal:** Complete the window manager so windows can be minimized, maximized, restored, snapped to edges, and resized by dragging edges. Add Alt+Tab task switcher and global hotkeys. The WM prototype works -- this hardens it into a production desktop.

> [!IMPORTANT]
> The existing `wm.c` (1143 lines) has window create/destroy/move/focus/z-order and title bar decorations with Mica effect. What's missing: minimize, maximize, restore, resize-by-edge, snap layouts, and system hotkeys. The `TODO: wm_minimize()` and `TODO: wm_maximize()` stubs are already in the code.

## Inputs

- [`src/desktop/wm.c`](../../src/desktop/wm.c) -- existing WM with drag, focus, decorations
- [`include/desktop/wm.h`](../../include/desktop/wm.h) -- window struct, flags, API

## Outcome

- Windows minimize to taskbar, maximize to full screen, restore to saved position
- Drag window edges to resize (8 directions)
- Snap windows to left/right half, or top for maximize
- Alt+Tab cycles focus between windows with thumbnail preview
- Alt+F4 closes focused window, Win+D shows desktop

## Implementation Order

| ⭐  | Order | Deliverable                             | Depends On | Status |
| --- | :---: | --------------------------------------- | ---------- | :----: |
| 💎  |   1   | Minimize, maximize, restore             | --         |  [ ]   |
| 💎  |   2   | Resize by dragging window edges         | --         |  [ ]   |
| 💎  |   3   | Snap to left/right half, top=maximize   | §1, §2     |  [ ]   |
| 💎  |   4   | Global hotkeys (Alt+Tab, Alt+F4, Win+D) | §1         |  [ ]   |

---

## 1. Minimize, Maximize, Restore
Wire the existing caption button hit detection to actual window state changes.

**Files:** `src/desktop/wm.c`, `include/desktop/wm.h`

- [ ] Add `WM_FLAG_MINIMIZED` and `WM_FLAG_MAXIMIZED` to window flags
- [ ] Add `saved_rect` (x, y, w, h) to window struct for restore geometry
- [ ] `wm_minimize(handle)` -- hide window, clear VISIBLE, set MINIMIZED
- [ ] `wm_maximize(handle)` -- save rect, resize to screen minus taskbar, set MAXIMIZED
- [ ] `wm_restore(handle)` -- restore saved_rect, clear MINIMIZED/MAXIMIZED
- [ ] Caption button close → `wm_destroy_window()`
- [ ] Caption button maximize → toggle maximize/restore
- [ ] Caption button minimize → `wm_minimize()`
- [ ] Taskbar click on minimized window → `wm_restore()` + focus
- [ ] Commit

**Test checkpoint:** Click minimize -- window disappears. Click taskbar entry -- window restores. Click maximize -- fills screen. Click maximize again -- restores to original size.

## 2. Resize by Dragging Window Edges
Detect mouse near window edges (8px border zone) and allow resize dragging.

**Files:** `src/desktop/wm.c`

- [ ] Edge hit detection: top, bottom, left, right, and 4 corners (8px threshold)
- [ ] Set cursor shape based on edge: `↔` `↕` `⤢` `⤡`
- [ ] On drag: resize window, reallocate framebuffer if needed
- [ ] Minimum window size: 200x100
- [ ] Respect `WM_FLAG_RESIZABLE` -- only allow resize if flag set
- [ ] Commit

**Test checkpoint:** Drag bottom-right corner of terminal window -- it resizes. Cursor changes to resize arrows when hovering edges.

## 3. Snap to Left/Right Half
Drag window to screen edge to snap it to half-screen or full-screen.

**Files:** `src/desktop/wm.c`

- [ ] Detect drag reaching screen left edge (x <= 0): snap to left half
- [ ] Detect drag reaching screen right edge (x >= width-1): snap to right half
- [ ] Detect drag reaching screen top (y <= 0): maximize
- [ ] Save pre-snap rect for restore on un-snap (drag away from edge)
- [ ] Visual indicator: translucent overlay showing snap target zone
- [ ] Commit

**Test checkpoint:** Drag window to left edge -- snaps to left half. Drag to right -- right half. Drag to top -- maximizes. Drag title bar away -- restores.

## 4. Global Hotkeys
System-wide keyboard shortcuts that work regardless of focused window.

**Files:** `src/desktop/wm.c`, `src/desktop/desktop.c`

- [ ] Hotkey dispatch table: key + modifiers → action
- [ ] Alt+Tab: cycle focus to next window (reverse z-order)
- [ ] Alt+F4: close focused window (`wm_destroy_window`)
- [ ] Win+D: minimize all windows (show desktop) / restore all
- [ ] Win+L: lock screen (placeholder -- just shows message)
- [ ] Requires modifier key tracking (see TODO-03 Input System)
- [ ] Commit

**Test checkpoint:** Alt+Tab cycles between terminal and gallery. Alt+F4 closes focused window. Win+D shows desktop.

---

## OS Comparison

| ⭐  | Feature         | 🪟 Win11    | 🐧 Linux (GNOME) | 🚀 Impossible OS     |
| --- | --------------- | ----------- | ---------------- | -------------------- |
| 💎  | Min/Max/Restore | ✅ Built-in | ✅ Built-in      | ⬜ §1                |
| 💎  | Edge resize     | ✅ Built-in | ✅ Built-in      | ⬜ §2                |
| ⭐  | Snap layouts    | ✅ 6-zone   | ❌ Manual tiling | ⬜ §3 left/right/max |
| 💎  | Alt+Tab         | ✅ Built-in | ✅ Built-in      | ⬜ §4                |
