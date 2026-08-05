---
schema_version: 1
id: control-library
domain: 06-desktop-foundation
status: active
title: "TODO-04 -- Control Library Completion"
---

# TODO-04 -- Control Library Completion

> **Goal:** Add the missing essential controls: checkbox, radio button, combobox (dropdown), listbox, and progress bar. Also add a reusable context menu (popup menu) control. The existing 4 controls (button, label, textbox, scrollbar) are production-quality -- these extend the library.

## Inputs

- [`src/desktop/controls.c`](../../src/desktop/controls.c) -- existing 4 controls, per-window storage, hit-test, draw pipeline
- [`include/desktop/controls.h`](../../include/desktop/controls.h) -- control types, state flags, API

## Outcome

- Checkbox: square with checkmark, label, toggle on click
- Radio button: circle with dot, grouped (only one active per group)
- Combobox: text field + dropdown arrow → popup list
- Listbox: scrollable list of items with selection
- Progress bar: horizontal fill with percentage
- Context menu: popup at cursor with items, separators, hover highlight

## Implementation Order

| ⭐  | Order | Deliverable               | Depends On | Status |
| --- | :---: | ------------------------- | ---------- | :----: |
| 💎  |   1   | Checkbox and radio button | --         |  [ ]   |
| 💎  |   2   | Progress bar              | --         |  [ ]   |
| 💎  |   3   | Listbox with scrollbar    | --         |  [ ]   |
| 💎  |   4   | Combobox (dropdown)       | §3         |  [ ]   |
| 💎  |   5   | Context menu (popup)      | --         |  [ ]   |

---

## 1. Checkbox and Radio Button
Toggle controls for boolean and exclusive-choice inputs.

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [ ] `CTRL_CHECKBOX` type: 16x16 square + label, `checked` state
- [ ] Draw: unchecked = empty square, checked = filled square with checkmark glyph
- [ ] Click toggles `checked` state, fires `on_change` callback
- [ ] `CTRL_RADIO` type: 16x16 circle + label, `checked` state, `group_id`
- [ ] Click on radio: uncheck all others in same `group_id`, check this one
- [ ] `ctrl_is_checked(ctrl_id)` / `ctrl_set_checked(ctrl_id, bool)`
- [ ] Colors: accent blue fill when checked, subtle border when unchecked
- [ ] Commit

**Test checkpoint:** Add checkbox + 3 radio buttons to gallery. Click checkbox -- toggles. Click radio -- deselects others.

## 2. Progress Bar
Horizontal bar showing completion percentage.

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [ ] `CTRL_PROGRESSBAR` type: filled rect proportion of `value / max_value`
- [ ] `ctrl_set_progress(ctrl_id, value, max_value)` -- update fill
- [ ] Draw: track background (dark), fill (accent blue), rounded ends
- [ ] Optional: percentage text centered ("42%")
- [ ] Indeterminate mode: sliding highlight animation (future, after animation engine)
- [ ] Commit

**Test checkpoint:** Add progress bar to gallery at 65%. Call `ctrl_set_progress` to update -- bar fills.

## 3. Listbox with Scrollbar
Scrollable vertical list of text items with single-selection.

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [ ] `CTRL_LISTBOX` type: list of string items (max 256 items, 128 chars each)
- [ ] `ctrl_listbox_add_item(ctrl_id, text)` -- append item
- [ ] `ctrl_listbox_get_selected(ctrl_id)` -- return selected index (-1 if none)
- [ ] Draw: items rendered vertically, selected item highlighted (accent blue bg)
- [ ] Scroll: automatic vertical scrollbar when items exceed visible height
- [ ] Click on item → select, fire `on_change` callback
- [ ] Mouse wheel scrolls list
- [ ] Commit

**Test checkpoint:** Add listbox with 20 items to gallery. Click items -- highlights. Scroll -- list scrolls.

## 4. Combobox (Dropdown)
Text field with dropdown arrow that opens a listbox popup.

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [ ] `CTRL_COMBOBOX` type: text display area + dropdown arrow button
- [ ] Click arrow → open popup listbox below the combobox
- [ ] Select item in popup → close popup, update text display, fire `on_change`
- [ ] Click outside popup → close popup
- [ ] `ctrl_combobox_add_item(ctrl_id, text)` -- add option
- [ ] `ctrl_combobox_get_selected(ctrl_id)` -- get selected index
- [ ] Popup rendered on top of all controls (z-order above window content)
- [ ] Commit

**Test checkpoint:** Add combobox with 5 options to gallery. Click arrow -- dropdown opens. Select item -- closes and shows selection.

## 5. Context Menu (Popup)
Reusable popup menu that appears at cursor position on right-click.

**Files:** `src/desktop/controls.c` or new `src/desktop/menu.c`

- [ ] `menu_create()` -- create a menu with items
- [ ] `menu_add_item(menu, label, icon, callback)` -- add clickable item
- [ ] `menu_add_separator(menu)` -- add horizontal line
- [ ] `menu_show(menu, x, y)` -- display at position, grab input
- [ ] `menu_hide()` -- close menu
- [ ] Draw: acrylic background, rounded corners, hover highlight, item icons
- [ ] Click item → fire callback, close menu
- [ ] Click outside → close menu
- [ ] Escape → close menu
- [ ] Commit

**Test checkpoint:** Right-click desktop → context menu with "Refresh", "New Folder", "Display Settings". Click item → menu closes.

---

## OS Comparison

| ⭐  | Feature        | 🪟 Win11    | 🐧 Linux (GTK) | 🚀 Impossible OS |
| --- | -------------- | ----------- | -------------- | ---------------- |
| 💎  | Checkbox/Radio | ✅ Built-in | ✅ Built-in    | ⬜ §1            |
| 💎  | Progress bar   | ✅ Built-in | ✅ Built-in    | ⬜ §2            |
| 💎  | Listbox        | ✅ Built-in | ✅ Built-in    | ⬜ §3            |
| 💎  | Combobox       | ✅ Built-in | ✅ Built-in    | ⬜ §4            |
| 💎  | Context menu   | ✅ Built-in | ✅ Built-in    | ⬜ §5            |
