---
schema_version: 1
id: control-library
domain: 06-desktop-foundation
status: active
title: "TODO-04 -- Control Library Completion"
---

# TODO-04 -- Control Library Completion

> **Goal:** Superseded as an implementation plan. The controls planned here are owned by `08-graphics-ui/TODO-05-widget-library-core.md` (check box, radio, progress, combo box), `TODO-06-widget-dialogs.md` (lists) and `TODO-09-desktop-shell-features.md` §1 (the single context menu engine), all drawn to `docs/design/controls.md`. What stays here is adding each control to the control gallery.

## Inputs

- [`src/desktop/controls.c`](../../src/desktop/controls.c) -- existing 4 controls, per-window storage, hit-test, draw pipeline
- [`include/desktop/controls.h`](../../include/desktop/controls.h) -- control types, state flags, API

## Outcome

- The control gallery showcases check box, radio group, progress bar, list and combo box built by their canonical owners, matching `docs/design/controls.md`.
- No second control or context-menu implementation exists in this domain.

## Implementation Order

| ⭐  | Order | Deliverable               | Depends On | Status |
| --- | :---: | ------------------------- | ---------- | :----: |
| 💎  |   1   | Checkbox and radio button | --         |  [/]   |
| 💎  |   2   | Progress bar              | --         |  [/]   |
| 💎  |   3   | Listbox with scrollbar    | --         |  [/]   |
| 💎  |   4   | Combobox (dropdown)       | §3         |  [/]   |
| 💎  |   5   | Context menu (popup)      | --         |  [/]   |
---

## 1. Checkbox and Radio Button

**Design:** [`controls.md#check-box-and-radio-button`](../../docs/design/controls.md#check-box-and-radio-button)
Check boxes and radio buttons are owned by `08-graphics-ui/TODO-05` §1 and §2, drawn to `controls.md` (20 px box with radius 4, 20 px radio with a 12 px dot, accent when checked, two-ring keyboard focus).

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-05-widget-library-core.md §1 and §2`, which follows `docs/design/controls.md#check-box-and-radio-button`; do not build a second CTRL_CHECKBOX or CTRL_RADIO here
- [ ] Showcase: add a check box and a 3-button radio group to the control gallery (`src/desktop/gallery.c`) using the canonical `ctrl_create_checkbox()` / radio API

**Test checkpoint:** Gallery shows a 20 px check box and a radio group that match `controls.md`; click toggles, radio selection is exclusive.

## 2. Progress Bar

**Design:** [`controls.md#progress`](../../docs/design/controls.md#progress)
The progress bar is owned by `08-graphics-ui/TODO-05` §4, drawn to `controls.md#progress` (1 px track, 3 px accent indicator, indeterminate sweep).

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-05-widget-library-core.md §4`, which follows `docs/design/controls.md#progress`; do not build a second CTRL_PROGRESSBAR here
- [ ] Showcase: add a progress bar at 65% and an indeterminate bar to the control gallery using the canonical API

**Test checkpoint:** Add progress bar to gallery at 65%. Call `ctrl_set_progress` to update -- bar fills.

## 3. Listbox with Scrollbar

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)
Scrollable single-selection lists are the `CTRL_LISTVIEW` details mode owned by `08-graphics-ui/TODO-06` §1, drawn to `controls.md#list-tree-and-grid-views` (32 px rows, 3 x 16 accent selection pill).

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-06-widget-dialogs.md §1`, which follows `docs/design/controls.md#list-tree-and-grid-views`; do not build a second list control here
- [ ] Showcase: add a 20-item list (the canonical `CTRL_LISTVIEW` in details mode) to the control gallery

**Test checkpoint:** Add listbox with 20 items to gallery. Click items -- highlights. Scroll -- list scrolls.

## 4. Combobox (Dropdown)

**Design:** [`controls.md#combo-box-and-drop-down`](../../docs/design/controls.md#combo-box-and-drop-down)
The combo box is owned by `08-graphics-ui/TODO-05` §5, drawn to `controls.md#combo-box-and-drop-down` (32 px, chevron, menu-acrylic list with the selected item over the box).

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-05-widget-library-core.md §5`, which follows `docs/design/controls.md#combo-box-and-drop-down`; do not build a second CTRL_COMBOBOX here
- [ ] Showcase: add a 5-option combo box to the control gallery using the canonical API

**Test checkpoint:** Add combobox with 5 options to gallery. Click arrow -- dropdown opens. Select item -- closes and shows selection.

## 5. Context Menu (Popup)

**Design:** [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)
The one context menu engine is `context_menu_show()` owned by `08-graphics-ui/TODO-09` §1, drawn to `shell.md#context-menus` (256 px wide, 4 px padding, 32 px items, menu acrylic, radius 8). There is no second `menu_*` API.

**Files:** `src/desktop/controls.c` or new `src/desktop/menu.c`

- [/] Superseded: implemented by `todo/08-graphics-ui/TODO-09-desktop-shell-features.md §1`, which follows `docs/design/shell.md#context-menus`; do not build a second menu engine (no `menu_create`/`menu_show` API) here
- [ ] Migrate any existing popup code in `src/desktop/controls.c` / `desktop.c` onto `context_menu_show()` from 08 TODO-09 §1 and delete the duplicate path

**Test checkpoint:** No `menu_create` / `menu_show` symbols exist; right-click on the desktop opens the 08 TODO-09 §2 menu (View, Sort by, Refresh | New | Display settings, Personalize | Open in Terminal, Show more options).

---

## OS Comparison

| ⭐  | Feature        | 🪟 Win11    | 🐧 Linux (GTK) | 🚀 Impossible OS |
| --- | -------------- | ----------- | -------------- | ---------------- |
| 💎  | Checkbox/Radio | ✅ Built-in | ✅ Built-in    | ⬜ §1            |
| 💎  | Progress bar   | ✅ Built-in | ✅ Built-in    | ⬜ §2            |
| 💎  | Listbox        | ✅ Built-in | ✅ Built-in    | ⬜ §3            |
| 💎  | Combobox       | ✅ Built-in | ✅ Built-in    | ⬜ §4            |
| 💎  | Context menu   | ✅ Built-in | ✅ Built-in    | ⬜ §5            |
