# P0204 — Widget & Control Library

> **Goal:** A complete set of UI controls — from basic buttons to complex tree views
> and dialog systems — forming the building blocks for all GUI applications.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## Already Completed ✅

<details>
<summary>✅ Already Completed — completed</summary>


- [x] **Button** — click handler, hover/press states, themed colors
- [x] **Label** — text display with font rendering
- [x] **TextBox** — single-line text input with cursor
- [x] **ScrollBar** — vertical/horizontal scroll with thumb drag

> These basic controls are implemented in `controls.c` / `controls.h`.


</details>

---
## 1. Extended Widget Toolkit *(NEW — missing from all TODOs)*

> The existing controls library (`controls.h`) only has Button, Label, TextBox,
> and ScrollBar. A Windows 11-quality desktop needs ~15 additional widgets. This
> is the most critical missing piece for building real applications.

### 1.1 Checkbox & Radio Button

**Prompt:** Add `CTRL_CHECKBOX` and `CTRL_RADIO` to the controls library. A checkbox is a 16×16 square with a checkmark glyph when checked, plus a text label to the right. A radio button is a 16×16 circle with a filled inner circle when selected. Radio buttons within the same group (identified by `group_id`) are mutually exclusive — selecting one deselects the others. Both support checked/unchecked/disabled states. Use `gfx_fill_rounded_rect` for the checkbox background and `gfx_fill_circle` for the radio. Both need hover highlights and focus indicators. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: checkbox and radio button"`. Add notes, gotchas, and design decisions directly in this TODO section covering the checkbox and radio button API, group exclusion, and rendering.


- [ ] Add `CTRL_CHECKBOX` type to `enum ctrl_type`
- [ ] `ctrl_create_checkbox(handle, x, y, text, checked, callback)` — returns ctrl ID
- [ ] Draw: square box + checkmark (✓) glyph when checked
- [ ] Hover/focused highlight, disabled grayed-out state
- [ ] Add `CTRL_RADIO` type to `enum ctrl_type`
- [ ] `ctrl_create_radio(handle, x, y, text, group_id, callback)` — returns ctrl ID
- [ ] Draw: circle + filled inner dot when selected
- [ ] Mutual exclusion within the same `group_id`
- [ ] Commit: `"controls: checkbox and radio button"`

### 1.2 Dropdown / ComboBox

**Prompt:** Add `CTRL_DROPDOWN` (combo box). It looks like a text field with a down-arrow button on the right. Clicking it opens a popup list of items (rendered as a small floating window above all others). Use the WM or a special overlay to draw the popup so it can extend beyond the parent window's bounds. Support up to 32 items. Keyboard: up/down to navigate, Enter to select, Escape to close. The currently selected item's text shows in the control. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: dropdown combobox"`. Add notes, gotchas, and design decisions directly in this TODO section covering the dropdown API, popup rendering, and keyboard navigation.


- [ ] Add `CTRL_DROPDOWN` type
- [ ] `ctrl_create_dropdown(handle, x, y, w, items[], count, callback)` — returns ctrl ID
- [ ] Draw: text + ▼ button
- [ ] Click → open popup list (floating above all windows)
- [ ] Arrow key navigation, Enter to select, Escape to close
- [ ] `ctrl_add_dropdown_item(handle, ctrl_id, text)` — add item dynamically
- [ ] `ctrl_get_dropdown_selection(handle, ctrl_id)` — get selected index
- [ ] Commit: `"controls: dropdown combobox"`

### 1.3 Slider

**Prompt:** Add `CTRL_SLIDER` (track bar). A horizontal or vertical track with a draggable thumb. The track is a thin rounded rect, the thumb is a filled circle. Value ranges from `min_value` to `max_value`. Dragging the thumb updates the value and calls the `on_change` callback in real-time. Click on the track (away from thumb) to jump the value. Support both horizontal and vertical orientations. Used for volume, brightness, and settings. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: slider"`. Add notes, gotchas, and design decisions directly in this TODO section covering the slider API, orientation, and value change callback.


- [ ] Add `CTRL_SLIDER` type
- [ ] `ctrl_create_slider(handle, x, y, w, h, min, max, value, orientation, callback)`
- [ ] Draw: thin rounded rect track + circle thumb at current position
- [ ] Drag thumb to change value (real-time callback)
- [ ] Click track → jump value to clicked position
- [ ] Horizontal and vertical orientations
- [ ] Accent color for filled portion of track
- [ ] Commit: `"controls: slider"`

### 1.4 Progress Bar

**Prompt:** Add `CTRL_PROGRESSBAR`. A rounded rectangle that fills from left to right like Windows 11's progress bars. Supports determinate mode (0–100% with a set value) and indeterminate mode (an animated marquee block that slides back and forth). The filled portion uses the theme's accent color with a subtle gradient. Use `gfx_fill_rounded_rect` for both the track and the fill. The indeterminate animation uses the tween engine (§3.1). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: progress bar"`. Add notes, gotchas, and design decisions directly in this TODO section covering the progress bar API, determinate/indeterminate modes, and tween integration.


- [ ] Add `CTRL_PROGRESSBAR` type
- [ ] `ctrl_create_progressbar(handle, x, y, w, h)`
- [ ] `ctrl_set_progress(handle, ctrl_id, value)` — 0–100
- [ ] Determinate mode: fill from left, accent color + gradient
- [ ] Indeterminate mode: animated sliding highlight bar
- [ ] `ctrl_set_progress_mode(handle, ctrl_id, mode)` — DETERMINATE or INDETERMINATE
- [ ] Commit: `"controls: progress bar"`

### 1.5 Tab Control

**Prompt:** Add `CTRL_TABSTRIP`. A row of clickable tab headers at the top of a pane. Each tab has a label and optional icon. Clicking a tab switches the visible content area. The active tab has a thick accent-color underline. Tabs can be added/removed dynamically. The tab strip sends a callback when the selection changes so the host can swap what's drawn in the content area below. Used by: terminal (future tabs), Control Panel, File Manager properties. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: tab strip"`. Add notes, gotchas, and design decisions directly in this TODO section covering the tab strip API, dynamic tab management, and selection callbacks.


- [ ] Add `CTRL_TABSTRIP` type
- [ ] `ctrl_create_tabstrip(handle, x, y, w, h, callback)`
- [ ] `ctrl_add_tab(handle, ctrl_id, label, icon)` — add a tab
- [ ] `ctrl_remove_tab(handle, ctrl_id, index)` — remove a tab
- [ ] Active tab: accent-color underline + slightly different background
- [ ] Click tab → callback with new tab index
- [ ] Commit: `"controls: tab strip"`

### 1.6 ListView

**Prompt:** Add `CTRL_LISTVIEW`. Displays a scrollable list of items. Two view modes: details (columns with header row — icon, name, size, date) and icon grid (large icons with label below, used for desktops and icon views). Each item has: icon ID, primary text, secondary text, and a user pointer. Selection: single-click selects (highlight row), double-click activates (opens/runs). Multi-select with Ctrl+click and Shift+click. Column headers are clickable for sorting (ascending/descending toggle). Scrolls vertically with a scrollbar. Used by: File Manager, Control Panel app list, Start menu app list. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: list view"`. Add notes, gotchas, and design decisions directly in this TODO section covering the list view API, view modes, sorting, and multi-select.


- [ ] Add `CTRL_LISTVIEW` type
- [ ] `ctrl_create_listview(handle, x, y, w, h, mode, callback)` — DETAILS or ICON mode
- [ ] `ctrl_listview_add_column(handle, ctrl_id, title, width)` — detail mode columns
- [ ] `ctrl_listview_add_item(handle, ctrl_id, icon, text, sub_text, user_ptr)`
- [ ] Detail mode: column headers + sortable rows
- [ ] Icon mode: grid of large icons with labels
- [ ] Single-click select, double-click activate
- [ ] Multi-select: Ctrl+click, Shift+click
- [ ] Integrated vertical scrollbar
- [ ] Commit: `"controls: list view"`

### 1.7 TreeView

**Prompt:** Add `CTRL_TREEVIEW`. A hierarchical tree with expand/collapse triangles. Each node has: icon, label, parent pointer, children list, expanded flag, selected flag, and user data pointer. Clicking the triangle (▸/▾) toggles expand/collapse. Clicking the label selects the node. Arrow keys: up/down move selection, right expands, left collapses. Render with indentation (16px per level). Used by: File Manager navigation pane, Registry editor, Settings category tree. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: tree view"`. Add notes, gotchas, and design decisions directly in this TODO section covering the tree view API, node management, expand/collapse, and keyboard navigation.


- [ ] Add `CTRL_TREEVIEW` type
- [ ] `ctrl_create_treeview(handle, x, y, w, h, callback)`
- [ ] `ctrl_treeview_add_node(handle, ctrl_id, parent, icon, label, user_ptr)` — returns node ID
- [ ] `ctrl_treeview_remove_node(handle, ctrl_id, node_id)`
- [ ] Draw: indented rows with ▸/▾ expand triangles + icon + label
- [ ] Click triangle → toggle expand/collapse
- [ ] Click label → select node (callback)
- [ ] Arrow key navigation: up/down/left (collapse)/right (expand)
- [ ] Integrated vertical scrollbar
- [ ] Commit: `"controls: tree view"`

### 1.8 Toolbar

**Prompt:** Add `CTRL_TOOLBAR`. A horizontal strip of icon buttons, usually placed immediately below the menu bar or title bar. Each toolbar button has: icon, optional tooltip text, click callback, toggle flag (stays pressed until clicked again), separator flag (draws a thin vertical line). Buttons are drawn as flat icons that gain a rounded highlight on hover and a pressed state on click. Toolbars can overflow with a `>>` chevron that opens a dropdown of hidden buttons. Used by: File Manager (back/forward/up/copy/paste/delete), Notepad (new/open/save), any app. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: toolbar"`. Add notes, gotchas, and design decisions directly in this TODO section covering the toolbar API, button types, toggle state, and overflow chevron.


- [ ] Add `CTRL_TOOLBAR` type
- [ ] `ctrl_create_toolbar(handle, x, y, w, h)`
- [ ] `ctrl_toolbar_add_button(handle, ctrl_id, icon, tooltip, callback, is_toggle)`
- [ ] `ctrl_toolbar_add_separator(handle, ctrl_id)`
- [ ] Draw: flat icon buttons with hover highlight + pressed state
- [ ] Toggle buttons: stay active until clicked again
- [ ] Overflow: `>>` chevron dropdown for hidden buttons
- [ ] Commit: `"controls: toolbar"`

### 1.9 Menu Bar

**Prompt:** Add `CTRL_MENUBAR`. A horizontal bar at the top of a window with top-level menu items (File, Edit, View, Help). Clicking a menu item opens a dropdown context menu (reusing the context menu system from §5). Keyboard: Alt activates the menu bar, arrow keys navigate between menus, accelerator keys (underlined letter) jump to a menu. Each menu item has a label, optional keyboard accelerator text (e.g., "Ctrl+S"), optional checkmark, optional submenu. Used by: Notepad, File Manager, Calculator, any standard app. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: menu bar"`. Add notes, gotchas, and design decisions directly in this TODO section covering the menu bar API, keyboard activation, and accelerator display.


- [ ] Add `CTRL_MENUBAR` type
- [ ] `ctrl_create_menubar(handle)`
- [ ] `ctrl_menubar_add_menu(handle, ctrl_id, label)` — add top-level menu (File, Edit...)
- [ ] `ctrl_menubar_add_item(handle, ctrl_id, menu_id, label, accel, callback)`
- [ ] Click menu label → open dropdown (reuse context menu renderer)
- [ ] Keyboard: Alt activates, arrows navigate, letter keys jump
- [ ] Accelerator key text shown right-aligned (e.g., "Ctrl+S")
- [ ] Separator, disabled, and checked item support
- [ ] Commit: `"controls: menu bar"`

### 1.10 Status Bar

**Prompt:** Add `CTRL_STATUSBAR`. A thin bar at the bottom of a window, divided into sections (panes). Each pane shows text and/or an icon. The first pane is the default help/status text. Other panes show things like cursor position (ln/col), file encoding, zoom level, etc. Panes have configurable widths (fixed pixels or stretchy). Used by: Notepad (line/col, encoding), File Manager (item count, selection size), Terminal (connection status). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: status bar"`. Add notes, gotchas, and design decisions directly in this TODO section covering the status bar API, pane layout, and text updates.


- [ ] Add `CTRL_STATUSBAR` type
- [ ] `ctrl_create_statusbar(handle)`
- [ ] `ctrl_statusbar_add_pane(handle, ctrl_id, width, text)` — add a pane
- [ ] `ctrl_statusbar_set_text(handle, ctrl_id, pane_id, text)` — update pane text
- [ ] Draw: thin bar at window bottom, divided into sections
- [ ] Stretchy first pane, fixed-width other panes
- [ ] Commit: `"controls: status bar"`

### 1.11 GroupBox & Separator

**Prompt:** Add `CTRL_GROUPBOX` — a labeled rectangle that visually groups related controls. The label sits in the top-left corner with the border line interrupted around it. Also add `CTRL_SEPARATOR` — a simple horizontal or vertical dividing line with themed color. Both are purely visual, no interaction. Used by: Control Panel applets (group settings into sections), dialogs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"controls: groupbox and separator"`. Add notes, gotchas, and design decisions directly in this TODO section covering the groupbox and separator rendering.


- [ ] Add `CTRL_GROUPBOX` type with label text
- [ ] Draw: rounded rect border with label cutout in top-left
- [ ] Add `CTRL_SEPARATOR` type (horizontal/vertical line)
- [ ] Commit: `"controls: groupbox and separator"`

### 1.12 Tooltip Support *(from Phase 02 §9.5)*

**Prompt:** Tooltips appear after hovering over a UI element for 500ms and display helpful text near the cursor. Implement a simple tooltip manager: controls register tooltip text via `tooltip_set(ctrl_id, text)`. A global timer tracks hover duration — when it exceeds 500ms without mouse movement, render a small rounded-rect popup (semi-transparent dark background, white text, drop shadow) near the cursor position. Auto-dismiss when the mouse moves. Add tooltips to all window buttons ("Close", "Minimize", "Maximize"), taskbar buttons (window title), toolbar buttons, and system tray icons. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: tooltip support"`. Add notes, gotchas, and design decisions directly in this TODO section covering the tooltip manager, hover timing, and rendering.


- [ ] Hover delay (500ms) → show tooltip near cursor
- [ ] Tooltip struct: text, position, timer
- [ ] Rounded rect with shadow, semi-transparent background
- [ ] Auto-dismiss on mouse move
- [ ] Add to all buttons (close, minimize, maximize, start, taskbar items, toolbar buttons)
- [ ] Commit: `"desktop: tooltip support"`

### 1.13 Dialog System *(NEW — missing from all TODOs)*

**Prompt:** Implement a common dialog system for standard OS dialogs: Message Box (info/warning/error/question with OK/Cancel/Yes/No buttons), Input Dialog (prompt + text field + OK/Cancel), File Open/Save dialog (directory tree + file list + filename text field + filters), and Color Picker (hue wheel + saturation/value square + hex input). `dialog_msgbox(title, msg, type)` blocks the caller and returns the button pressed. `dialog_file_open(filter)` returns a file path. Dialogs are modal — they disable the parent window while open. Render with the standard WM chrome, centered on screen. Used everywhere: save prompts, error messages, file selection. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: common dialog system"`. Add notes, gotchas, and design decisions directly in this TODO section covering the dialog API, modal behavior, and each dialog type.


- [ ] Create `src/desktop/dialogs.c` and `include/desktop/dialogs.h`
- [ ] Message Box: `dialog_msgbox(title, msg, type)` — types: INFO, WARNING, ERROR, QUESTION
  - [ ] Button combos: OK, OK/Cancel, Yes/No, Yes/No/Cancel
  - [ ] Returns which button was pressed
- [ ] Input Dialog: `dialog_input(title, prompt, default_text)` — returns user text
- [ ] File Open dialog: `dialog_file_open(filter, default_dir)` — returns file path
  - [ ] Directory tree on left, file list on right (uses TreeView + ListView)
  - [ ] Filename text field + filter dropdown at bottom
- [ ] File Save dialog: `dialog_file_save(filter, default_name)`
- [ ] Color Picker dialog: `dialog_color(initial_color)` — returns selected color
- [ ] Modal: disable parent window while dialog is open
- [ ] All dialogs centered on screen, standard WM window chrome
- [ ] Commit: `"desktop: common dialog system"`

---

## Priority Order

| Priority | Section                   | Reason                                            |
|----------|---------------------------|---------------------------------------------------|
| ✅ Done   | Button, Label, TextBox, ScrollBar | Basic controls already implemented       |
| 🔴 P0    | §1.13 Dialog System       | Open/Save dialogs used by every app               |
| 🟠 P1    | §1.1 Checkbox + Radio     | Control Panel applets need these immediately      |
| 🟠 P1    | §1.2 Dropdown             | Settings profile pickers, configuration dropdowns |
| 🟠 P1    | §1.6 ListView             | File Manager, Settings — most visible control     |
| 🟠 P1    | §1.9 Menu Bar             | Notepad, File Manager need menu bars              |
| 🟡 P2    | §1.3 Slider               | Volume, brightness, power settings                |
| 🟡 P2    | §1.4 ProgressBar          | Download progress, disk operations                |
| 🟡 P2    | §1.5 TabStrip             | Control Panel category tabs                       |
| 🟡 P2    | §1.7 TreeView             | File Manager nav pane, Registry editor            |
| 🟡 P2    | §1.8 Toolbar              | File Manager, Notepad toolbars                    |
| 🟡 P2    | §1.12 Tooltip             | Discoverability for all controls                  |
| 🟢 P3    | §1.10 StatusBar           | File Manager (item count), Notepad (ln/col)       |
| 🟢 P3    | §1.11 GroupBox+Separator  | Control Panel applet visual grouping              |

---

## Key Files

| File                            | Purpose                               |
|---------------------------------|---------------------------------------|
| `src/desktop/controls.c`        | [MODIFY] Add all new widget types     |
| `include/desktop/controls.h`    | [MODIFY] New CTRL_* types + APIs      |
| `src/desktop/dialogs.c`         | [NEW] Common dialog system            |
| `include/desktop/dialogs.h`     | [NEW] Dialog API header               |

---

## OS Comparison

| Feature                        | 🪟 Windows 11 (Win32/WinUI3)     | 🐧 Linux (GTK4 / Qt6)             | 🚀 Impossible OS                         |
| ------------------------------ | ------------------------------- | -------------------------------- | --------------------------------------- |
| Button, Label, TextBox         | ✅ Win32 BUTTON, STATIC, EDIT    | ✅ GtkButton, GtkLabel, GtkEntry  | ✅ Done — `controls.c`                   |
| ScrollBar                      | ✅ Win32 SCROLLBAR               | ✅ GtkScrollbar                   | ✅ Done — `controls.c`                   |
| Checkbox + Radio               | ✅ BS_CHECKBOX, BS_RADIOBUTTON   | ✅ GtkCheckButton, GtkRadioButton | ⬜ §1.1 P1                               |
| Dropdown / ComboBox            | ✅ ComboBox (CBS_DROPDOWN)       | ✅ GtkDropDown, QComboBox         | ⬜ §1.2 P1                               |
| Slider / TrackBar              | ✅ TRACKBAR_CLASS                | ✅ GtkScale, QSlider              | ⬜ §1.3 P2                               |
| ProgressBar                    | ✅ PROGRESS_CLASS                | ✅ GtkProgressBar, QProgressBar   | ⬜ §1.4 P2                               |
| Tab Strip                      | ✅ WC_TABCONTROL                 | ✅ GtkNotebook, QTabWidget        | ⬜ §1.5 P2                               |
| ListView (details + icons)     | ✅ WC_LISTVIEW / SysListView32   | ✅ GtkListView + GtkColumnView    | ⬜ §1.6 P1                               |
| TreeView                       | ✅ WC_TREEVIEW / SysTreeView32   | ✅ GtkTreeView, QTreeView         | ⬜ §1.7 P2                               |
| Toolbar                        | ✅ TOOLBARCLASSNAME              | ✅ GtkToolbar, QToolBar           | ⬜ §1.8 P2                               |
| Menu Bar                       | ✅ HMENU / AppendMenu            | ✅ GtkMenuBar, QMenuBar           | ⬜ §1.9 P1                               |
| StatusBar                      | ✅ STATUSCLASSNAME               | ✅ GtkStatusbar, QStatusBar       | ⬜ §1.10 P3                              |
| GroupBox                       | ✅ BS_GROUPBOX                   | ✅ GtkFrame, QGroupBox            | ⬜ §1.11 P3                              |
| Tooltips                       | ✅ TOOLTIPS_CLASS                | ✅ GtkTooltip, QToolTip           | ⬜ §1.12 P2                              |
| File Open/Save dialog          | ✅ GetOpenFileName / IFileDialog | ✅ GtkFileChooserDialog           | ⬜ §1.13 P0                              |
| Color Picker dialog            | ✅ ChooseColor                   | ✅ GtkColorChooserDialog          | ⬜ §1.13 P0 (stretch)                    |
| **All-in-kernel (no toolkit)** | ❌ Requires Win32 DLLs + GDI     | ❌ Requires GTK/Qt runtime        | ✅ **Pure kernel C — zero dependencies** |
