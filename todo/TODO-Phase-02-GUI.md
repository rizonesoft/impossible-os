# Phase GUI — Complete Desktop & GUI System

> **Goal:** Build a complete, Windows 11-quality desktop shell with a full widget
> toolkit, modern visuals, and rich interactivity. After completing every item
> in this file, the user should be able to drag, resize, snap, and tile windows;
> launch apps from a polished Start menu; manage files via context menus; and
> interact with buttons, text fields, checkboxes, sliders, tree views, and all
> standard GUI controls — rivalling Windows 11 in appearance and functionality.

> **Note:** Items marked `[x]` were completed in earlier phases. Items marked
> `[ ]` are pending. Sections are ordered by **implementation priority** —
> foundations first, then shell, then polish.

---

## 1. Theme System *(from Phase 02 §9.1)*

> Foundation: every visual element references the theme instead of hardcoded hex
> colors. Must be done first so all subsequent code uses themed colors.

**Prompt:** Centralize all UI colors into a `theme_t` struct with named fields: background, foreground, accent, border, shadow, titlebar_active, titlebar_inactive, button_bg, button_hover, selection, error, warning. Load theme colors from Codex under `System\Theme\*`. Provide two built-in presets: Dark (dark backgrounds, light text, blue accent) and Light (light backgrounds, dark text). Every drawing function in `desktop.c`, `wm.c`, and `controls.c` must reference `theme_get()->field` instead of hardcoded hex colors. The accent color should be applied to focused controls, active title bars, and selection highlights. After completing all items, create `docs/architecture/theme-system.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: theme system"`.


- [ ] Define `theme_t` struct with all UI colors (bg, fg, accent, border, shadow, titlebar, button states, etc.)
- [ ] Create `include/desktop/theme.h` and `src/desktop/theme.c`
- [ ] Load theme from Codex (`System\Theme\*`)
- [ ] Built-in Dark mode preset (default)
- [ ] Built-in Light mode preset
- [ ] All drawing functions in `desktop.c`, `wm.c`, `controls.c` reference `theme_get()->field`
- [ ] Apply accent color to focused controls, active title bars, selection highlights
- [ ] Codex: `System\Theme\DarkMode` (BOOL), `System\Theme\AccentColor` (UINT32)
- [ ] Commit: `"desktop: theme system"`

---

## 2. Extended Widget Toolkit *(NEW — missing from all TODOs)*

> The existing controls library (`controls.h`) only has Button, Label, TextBox,
> and ScrollBar. A Windows 11-quality desktop needs ~15 additional widgets. This
> is the most critical missing piece for building real applications.

### 2.1 Checkbox & Radio Button

**Prompt:** Add `CTRL_CHECKBOX` and `CTRL_RADIO` to the controls library. A checkbox is a 16×16 square with a checkmark glyph when checked, plus a text label to the right. A radio button is a 16×16 circle with a filled inner circle when selected. Radio buttons within the same group (identified by `group_id`) are mutually exclusive — selecting one deselects the others. Both support checked/unchecked/disabled states. Use `gfx_fill_rounded_rect` for the checkbox background and `gfx_fill_circle` for the radio. Both need hover highlights and focus indicators. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: checkbox and radio button"`.


- [ ] Add `CTRL_CHECKBOX` type to `enum ctrl_type`
- [ ] `ctrl_create_checkbox(handle, x, y, text, checked, callback)` — returns ctrl ID
- [ ] Draw: square box + checkmark (✓) glyph when checked
- [ ] Hover/focused highlight, disabled grayed-out state
- [ ] Add `CTRL_RADIO` type to `enum ctrl_type`
- [ ] `ctrl_create_radio(handle, x, y, text, group_id, callback)` — returns ctrl ID
- [ ] Draw: circle + filled inner dot when selected
- [ ] Mutual exclusion within the same `group_id`
- [ ] Commit: `"controls: checkbox and radio button"`

### 2.2 Dropdown / ComboBox

**Prompt:** Add `CTRL_DROPDOWN` (combo box). It looks like a text field with a down-arrow button on the right. Clicking it opens a popup list of items (rendered as a small floating window above all others). Use the WM or a special overlay to draw the popup so it can extend beyond the parent window's bounds. Support up to 32 items. Keyboard: up/down to navigate, Enter to select, Escape to close. The currently selected item's text shows in the control. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: dropdown combobox"`.


- [ ] Add `CTRL_DROPDOWN` type
- [ ] `ctrl_create_dropdown(handle, x, y, w, items[], count, callback)` — returns ctrl ID
- [ ] Draw: text + ▼ button
- [ ] Click → open popup list (floating above all windows)
- [ ] Arrow key navigation, Enter to select, Escape to close
- [ ] `ctrl_add_dropdown_item(handle, ctrl_id, text)` — add item dynamically
- [ ] `ctrl_get_dropdown_selection(handle, ctrl_id)` — get selected index
- [ ] Commit: `"controls: dropdown combobox"`

### 2.3 Slider

**Prompt:** Add `CTRL_SLIDER` (track bar). A horizontal or vertical track with a draggable thumb. The track is a thin rounded rect, the thumb is a filled circle. Value ranges from `min_value` to `max_value`. Dragging the thumb updates the value and calls the `on_change` callback in real-time. Click on the track (away from thumb) to jump the value. Support both horizontal and vertical orientations. Used for volume, brightness, and settings. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: slider"`.


- [ ] Add `CTRL_SLIDER` type
- [ ] `ctrl_create_slider(handle, x, y, w, h, min, max, value, orientation, callback)`
- [ ] Draw: thin rounded rect track + circle thumb at current position
- [ ] Drag thumb to change value (real-time callback)
- [ ] Click track → jump value to clicked position
- [ ] Horizontal and vertical orientations
- [ ] Accent color for filled portion of track
- [ ] Commit: `"controls: slider"`

### 2.4 Progress Bar

**Prompt:** Add `CTRL_PROGRESSBAR`. A rounded rectangle that fills from left to right like Windows 11's progress bars. Supports determinate mode (0–100% with a set value) and indeterminate mode (an animated marquee block that slides back and forth). The filled portion uses the theme's accent color with a subtle gradient. Use `gfx_fill_rounded_rect` for both the track and the fill. The indeterminate animation uses the tween engine (§3.1). After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: progress bar"`.


- [ ] Add `CTRL_PROGRESSBAR` type
- [ ] `ctrl_create_progressbar(handle, x, y, w, h)`
- [ ] `ctrl_set_progress(handle, ctrl_id, value)` — 0–100
- [ ] Determinate mode: fill from left, accent color + gradient
- [ ] Indeterminate mode: animated sliding highlight bar
- [ ] `ctrl_set_progress_mode(handle, ctrl_id, mode)` — DETERMINATE or INDETERMINATE
- [ ] Commit: `"controls: progress bar"`

### 2.5 Tab Control

**Prompt:** Add `CTRL_TABSTRIP`. A row of clickable tab headers at the top of a pane. Each tab has a label and optional icon. Clicking a tab switches the visible content area. The active tab has a thick accent-color underline. Tabs can be added/removed dynamically. The tab strip sends a callback when the selection changes so the host can swap what's drawn in the content area below. Used by: terminal (future tabs), Settings Panel, File Manager properties. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: tab strip"`.


- [ ] Add `CTRL_TABSTRIP` type
- [ ] `ctrl_create_tabstrip(handle, x, y, w, h, callback)`
- [ ] `ctrl_add_tab(handle, ctrl_id, label, icon)` — add a tab
- [ ] `ctrl_remove_tab(handle, ctrl_id, index)` — remove a tab
- [ ] Active tab: accent-color underline + slightly different background
- [ ] Click tab → callback with new tab index
- [ ] Commit: `"controls: tab strip"`

### 2.6 ListView

**Prompt:** Add `CTRL_LISTVIEW`. Displays a scrollable list of items. Two view modes: details (columns with header row — icon, name, size, date) and icon grid (large icons with label below, used for desktops and icon views). Each item has: icon ID, primary text, secondary text, and a user pointer. Selection: single-click selects (highlight row), double-click activates (opens/runs). Multi-select with Ctrl+click and Shift+click. Column headers are clickable for sorting (ascending/descending toggle). Scrolls vertically with a scrollbar. Used by: File Manager, Settings app list, Start menu app list. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: list view"`.


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

### 2.7 TreeView

**Prompt:** Add `CTRL_TREEVIEW`. A hierarchical tree with expand/collapse triangles. Each node has: icon, label, parent pointer, children list, expanded flag, selected flag, and user data pointer. Clicking the triangle (▸/▾) toggles expand/collapse. Clicking the label selects the node. Arrow keys: up/down move selection, right expands, left collapses. Render with indentation (16px per level). Used by: File Manager navigation pane, Registry/Codex editor, Settings category tree. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: tree view"`.


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

### 2.8 Toolbar

**Prompt:** Add `CTRL_TOOLBAR`. A horizontal strip of icon buttons, usually placed immediately below the menu bar or title bar. Each toolbar button has: icon, optional tooltip text, click callback, toggle flag (stays pressed until clicked again), separator flag (draws a thin vertical line). Buttons are drawn as flat icons that gain a rounded highlight on hover and a pressed state on click. Toolbars can overflow with a `>>` chevron that opens a dropdown of hidden buttons. Used by: File Manager (back/forward/up/copy/paste/delete), Notepad (new/open/save), any app. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: toolbar"`.


- [ ] Add `CTRL_TOOLBAR` type
- [ ] `ctrl_create_toolbar(handle, x, y, w, h)`
- [ ] `ctrl_toolbar_add_button(handle, ctrl_id, icon, tooltip, callback, is_toggle)`
- [ ] `ctrl_toolbar_add_separator(handle, ctrl_id)`
- [ ] Draw: flat icon buttons with hover highlight + pressed state
- [ ] Toggle buttons: stay active until clicked again
- [ ] Overflow: `>>` chevron dropdown for hidden buttons
- [ ] Commit: `"controls: toolbar"`

### 2.9 Menu Bar

**Prompt:** Add `CTRL_MENUBAR`. A horizontal bar at the top of a window with top-level menu items (File, Edit, View, Help). Clicking a menu item opens a dropdown context menu (reusing the context menu system from §5). Keyboard: Alt activates the menu bar, arrow keys navigate between menus, accelerator keys (underlined letter) jump to a menu. Each menu item has a label, optional keyboard accelerator text (e.g., "Ctrl+S"), optional checkmark, optional submenu. Used by: Notepad, File Manager, Calculator, any standard app. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: menu bar"`.


- [ ] Add `CTRL_MENUBAR` type
- [ ] `ctrl_create_menubar(handle)`
- [ ] `ctrl_menubar_add_menu(handle, ctrl_id, label)` — add top-level menu (File, Edit...)
- [ ] `ctrl_menubar_add_item(handle, ctrl_id, menu_id, label, accel, callback)`
- [ ] Click menu label → open dropdown (reuse context menu renderer)
- [ ] Keyboard: Alt activates, arrows navigate, letter keys jump
- [ ] Accelerator key text shown right-aligned (e.g., "Ctrl+S")
- [ ] Separator, disabled, and checked item support
- [ ] Commit: `"controls: menu bar"`

### 2.10 Status Bar

**Prompt:** Add `CTRL_STATUSBAR`. A thin bar at the bottom of a window, divided into sections (panes). Each pane shows text and/or an icon. The first pane is the default help/status text. Other panes show things like cursor position (ln/col), file encoding, zoom level, etc. Panes have configurable widths (fixed pixels or stretchy). Used by: Notepad (line/col, encoding), File Manager (item count, selection size), Terminal (connection status). After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: status bar"`.


- [ ] Add `CTRL_STATUSBAR` type
- [ ] `ctrl_create_statusbar(handle)`
- [ ] `ctrl_statusbar_add_pane(handle, ctrl_id, width, text)` — add a pane
- [ ] `ctrl_statusbar_set_text(handle, ctrl_id, pane_id, text)` — update pane text
- [ ] Draw: thin bar at window bottom, divided into sections
- [ ] Stretchy first pane, fixed-width other panes
- [ ] Commit: `"controls: status bar"`

### 2.11 GroupBox & Separator

**Prompt:** Add `CTRL_GROUPBOX` — a labeled rectangle that visually groups related controls. The label sits in the top-left corner with the border line interrupted around it. Also add `CTRL_SEPARATOR` — a simple horizontal or vertical dividing line with themed color. Both are purely visual, no interaction. Used by: Settings applets (group settings into sections), dialogs. After completing all items, update `docs/architecture/controls.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"controls: groupbox and separator"`.


- [ ] Add `CTRL_GROUPBOX` type with label text
- [ ] Draw: rounded rect border with label cutout in top-left
- [ ] Add `CTRL_SEPARATOR` type (horizontal/vertical line)
- [ ] Commit: `"controls: groupbox and separator"`

### 2.12 Tooltip Support *(from Phase 02 §9.5)*

**Prompt:** Tooltips appear after hovering over a UI element for 500ms and display helpful text near the cursor. Implement a simple tooltip manager: controls register tooltip text via `tooltip_set(ctrl_id, text)`. A global timer tracks hover duration — when it exceeds 500ms without mouse movement, render a small rounded-rect popup (semi-transparent dark background, white text, drop shadow) near the cursor position. Auto-dismiss when the mouse moves. Add tooltips to all window buttons ("Close", "Minimize", "Maximize"), taskbar buttons (window title), toolbar buttons, and system tray icons. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: tooltip support"`.


- [ ] Hover delay (500ms) → show tooltip near cursor
- [ ] Tooltip struct: text, position, timer
- [ ] Rounded rect with shadow, semi-transparent background
- [ ] Auto-dismiss on mouse move
- [ ] Add to all buttons (close, minimize, maximize, start, taskbar items, toolbar buttons)
- [ ] Commit: `"desktop: tooltip support"`

### 2.13 Dialog System *(NEW — missing from all TODOs)*

**Prompt:** Implement a common dialog system for standard OS dialogs: Message Box (info/warning/error/question with OK/Cancel/Yes/No buttons), Input Dialog (prompt + text field + OK/Cancel), File Open/Save dialog (directory tree + file list + filename text field + filters), and Color Picker (hue wheel + saturation/value square + hex input). `dialog_msgbox(title, msg, type)` blocks the caller and returns the button pressed. `dialog_file_open(filter)` returns a file path. Dialogs are modal — they disable the parent window while open. Render with the standard WM chrome, centered on screen. Used everywhere: save prompts, error messages, file selection. After completing all items, create `docs/architecture/dialogs.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: common dialog system"`.


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

## 3. Animation Engine *(from Phase 02 §6)*

> Required by window transitions, menu popups, notification slides, and more.

### 3.1 Tween Engine *(from Phase 02 §6.1)*

**Prompt:** The animation engine provides time-based interpolation (tweening) for smooth UI transitions. A `gfx_tween_t` stores: start value, end value, current value, duration in ms, elapsed time, and an easing function pointer. `gfx_tween_update(delta_ms)` advances the tween by the frame delta time and recomputes the current value using the easing function. Easing functions take `t` (0.0→1.0) and return a shaped `t`: linear is identity, ease-out-cubic is `1 - (1-t)^3` (starts fast, decelerates), ease-in-quad is `t^2` (starts slow, accelerates). The compositor calls `gfx_tween_update` each frame with the frame delta. After completing all items, create `docs/architecture/animations.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"gfx: animation engine with easing"`.


- [ ] Create `src/kernel/gfx/gfx_animate.c`
- [ ] Define `gfx_tween_t` struct (from, to, current, duration_ms, elapsed_ms, easing, active)
- [ ] Implement `gfx_tween_start(tw, from, to, duration_ms, easing)`
- [ ] Implement `gfx_tween_update(tw, delta_ms)` — advance by delta time
- [ ] Implement `gfx_tween_value(tw)` — get interpolated current value
- [ ] Implement easing functions:
  - [ ] `GFX_EASE_LINEAR`
  - [ ] `GFX_EASE_IN_QUAD` / `GFX_EASE_OUT_QUAD` / `GFX_EASE_IN_OUT_QUAD`
  - [ ] `GFX_EASE_IN_CUBIC` / `GFX_EASE_OUT_CUBIC` / `GFX_EASE_IN_OUT_CUBIC`
  - [ ] `GFX_EASE_BOUNCE`
- [ ] Commit: `"gfx: animation engine with easing"`

### 3.2 Window Transition Animations *(from Phase 02 §6.2)*

**Prompt:** Each window state change should have a smooth animation: open (scale 90%→100% + fade in, 200ms ease-out-cubic), close (scale 100%→90% + fade out, 150ms), minimize (shrink toward the window's taskbar button position, 250ms), restore (reverse of minimize), maximize (expand to fill screen, 200ms). Use the tween engine from §3.1 — each animation creates tweens for the window's x, y, width, height, and opacity. The compositor must render animating windows at their interpolated position/size each frame. Add a Codex setting `System\Theme\EnableAnimations` (default: true) and `System\Theme\AnimationSpeed` (multiplier, default: 1.0). After completing all items, update `docs/architecture/animations.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: window transition animations"`.


- [ ] Create `src/kernel/wm_anim.c`
- [ ] Window open: scale 90%→100% + fade in (200ms, ease-out-cubic)
- [ ] Window close: scale 100%→90% + fade out (150ms)
- [ ] Minimize: shrink toward taskbar button position (250ms)
- [ ] Restore: expand from taskbar button (250ms)
- [ ] Maximize: expand to fill screen (200ms)
- [ ] *(Stretch)* Snap left/right: slide + resize to half (200ms)
- [ ] *(Stretch)* Focus switch: subtle scale pulse (100ms)
- [ ] Menu popup: scale Y 0→100% from top (150ms)
- [ ] Codex: `System\Theme\EnableAnimations`, `System\Theme\AnimationSpeed`
- [ ] "Reduce motion" option disables all animations
- [ ] Commit: `"desktop: window transition animations"`

---

## 4. Window Manager Enhancements *(from Phase 04 §5, §15.5)*

### 4.1 Window Minimize & Maximize *(NEW — partially missing)*

**Prompt:** Add full minimize/maximize/restore support to the window manager. `wm_minimize(handle)` hides the window, `wm_maximize(handle)` saves the pre-max position and resizes to fill the usable desktop area (screen minus taskbar), `wm_restore(handle)` returns to the saved position. The maximize button in the title bar should toggle between maximize and restore. Double-clicking the title bar also toggles maximize. Add minimize and maximize buttons to the window title bar alongside the close button. After completing all items, update `docs/architecture/window-manager.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: window minimize and maximize"`.


- [ ] Add `wm_minimize(handle)` — hide window, mark as minimized
- [ ] Add `wm_maximize(handle)` — save position, resize to fill usable area
- [ ] Add `wm_restore(handle)` — return to saved pre-max position
- [ ] Minimize button (━) in title bar
- [ ] Maximize/Restore button (☐/❐) in title bar
- [ ] Double-click title bar → toggle maximize
- [ ] Commit: `"desktop: window minimize and maximize"`

### 4.2 Keyboard Window Snapping *(from Phase 04 §5.1)*

**Prompt:** Window snapping allows quick tiling of windows. Win+Left snaps the focused window to the left half of the screen, Win+Right to the right half, Win+Up maximizes, Win+Down restores or minimizes. Store the window's pre-snap position so restoring returns it to its original size. Use the animation engine (§3.1) to smoothly tween the snap transition. After completing all items, create `docs/architecture/window-snapping.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: keyboard window snapping"`.


- [ ] Create `src/kernel/wm_snap.c`
- [ ] Win+Left → snap to left half of screen
- [ ] Win+Right → snap to right half
- [ ] Win+Up → maximize
- [ ] Win+Down → restore (if maximized) / minimize (if restored)
- [ ] Animate snap transitions (200ms slide + resize)
- [ ] Commit: `"desktop: keyboard window snapping"`

### 4.3 Edge Snapping (Mouse) *(from Phase 04 §5.2)*

**Prompt:** When dragging a window, detect if the cursor hits a screen edge and show a snap preview. After completing all items, update `docs/architecture/window-snapping.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: edge snap with preview"`.


- [ ] Drag to top edge → maximize preview overlay
- [ ] Drag to left/right edge → half-screen preview
- [ ] Drag to corner → quarter-screen preview
- [ ] Show snap preview zone (semi-transparent overlay) before drop
- [ ] Commit: `"desktop: edge snap with preview"`

### 4.4 Snap Layouts *(from Phase 04 §5.3)*

**Prompt:** Hovering over a window's maximize button shows a popup with visual layout options: 50/50 left-right, 50/50 top-bottom, 66/33 wide-narrow, and 33/33/33 three columns. This mimics Windows 11's Snap Layouts feature. After completing all items, update `docs/architecture/window-snapping.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: snap layouts on maximize hover"`.


- [ ] Hover maximize button → show snap layout popup
- [ ] Layout options: 50/50 LR, 50/50 TB, 66/33 wide-narrow, 33/33/33
- [ ] Click zone → snap current window, prompt to fill remaining zones
- [ ] Commit: `"desktop: snap layouts on maximize hover"`

### 4.5 Window Minimize/Restore All *(from Phase 04 §15.5)*

**Prompt:** Win+M minimizes all windows. Win+Shift+M restores all previously-minimized windows. Win+D toggles between minimize-all and restore-all (show desktop toggle). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: minimize/restore all windows"`.


- [ ] Win+M → minimize all windows
- [ ] Win+Shift+M → restore all minimized windows
- [ ] Win+D → toggle show desktop
- [ ] Commit: `"desktop: minimize/restore all windows"`

---

## 5. Context Menu System *(from Phase 02 §9.2 + Phase 04 §4)*

> Required by desktop right-click, file right-click, taskbar right-click,
> and the menu bar widget.

### 5.1 Generic Context Menu Engine *(from Phase 04 §4.1)*

**Prompt:** Build a generic reusable context menu system: `context_menu_show(x, y, items, count)` renders a floating menu with Acrylic blur background, rounded corners, and drop shadow. Each `menu_item` has: label, optional icon, callback, optional submenu pointer, separator flag, disabled flag, and checked flag. Handle keyboard navigation, submenu open on hover, and auto-close when clicking outside. After completing all items, create `docs/architecture/context-menus.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: context menu system"`.


- [ ] Define `struct menu_item` (label, icon, callback, submenu, separator, disabled, checked)
- [ ] Create `src/desktop/context_menu.c`
- [ ] `context_menu_show(x, y, items, count)` — display at position
- [ ] Render: rounded rect + drop shadow + Acrylic blur background
- [ ] Keyboard: up/down arrows, Enter to select, Escape to close
- [ ] Submenu support: `►` arrow, hover to open (300ms delay)
- [ ] Separator lines, disabled (grayed), checked (✓) items
- [ ] Auto-close on click outside
- [ ] Commit: `"desktop: context menu system"`

### 5.2 Desktop Context Menu *(from Phase 04 §4.2)*

**Prompt:** Right-clicking the desktop wallpaper shows a context menu with: View submenu, Sort By submenu, Refresh, New submenu (Folder, Text Document, Shortcut), Paste, Display Settings, Personalize. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: desktop right-click menu"`.


- [ ] Right-click desktop → context menu:
  - [ ] View ► → Large icons, Medium, Small, List
  - [ ] Sort by ► → Name, Date, Size, Type
  - [ ] Refresh
  - [ ] New ► → Folder, Text Document, Shortcut
  - [ ] Paste (if clipboard has files)
  - [ ] Display settings / Personalize
- [ ] Commit: `"desktop: desktop right-click menu"`

### 5.3 File Context Menu *(from Phase 04 §4.3)*

**Prompt:** Right-clicking a file icon shows: Open, Open With, Cut/Copy/Paste, Delete, Rename, Properties. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: file context menu"`.


- [ ] Right-click file → context menu:
  - [ ] Open / Open with... ►
  - [ ] Cut / Copy / Paste
  - [ ] Delete (recycle bin) / Rename
  - [ ] Properties (size, type, path, timestamps)
- [ ] Commit: `"desktop: file context menu"`

---

## 6. Taskbar *(from Phase 04 §1)*

### 6.1 Taskbar Window List *(from Phase 04 §1.1)*

**Prompt:** The taskbar shows a button for each open window. Clicking a window button focuses/raises it. Clicking the active window's button minimizes it (toggle). The active button gets an accent underline. After completing all items, create `docs/architecture/taskbar.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar window list"`.


- [ ] Define `struct taskbar_entry` (window ptr, title, icon, active, flashing)
- [ ] Create `src/desktop/taskbar_winlist.c`
- [ ] `taskbar_add_window(win)` / `taskbar_remove_window(win)` / `taskbar_set_active(win)`
- [ ] Draw window buttons between start button and system tray
- [ ] Active button: accent underline highlight
- [ ] Click button → focus/raise; click active → minimize
- [ ] `taskbar_flash(win)` — blink button to attract attention
- [ ] Commit: `"desktop: taskbar window list"`

### 6.2 Taskbar Button Context Menu *(from Phase 04 §1.2)*

**Prompt:** Right-clicking a taskbar button shows Close, Maximize/Restore, Minimize. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar button context menu"`.


- [ ] Right-click button → Close, Maximize/Restore, Minimize
- [ ] *(Stretch)* "Move to Desktop ►" submenu
- [ ] Commit: `"desktop: taskbar button context menu"`

### 6.3 Window Peek (Aero Peek) *(from Phase 04 §1.3)*

**Prompt:** Hovering a taskbar button for 500ms makes all other windows 10% opacity. "Show Desktop" button at far-right corner. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: window peek (Aero Peek)"`.


- [ ] Hover button 500ms → all other windows 10% opacity
- [ ] Mouse leaves → restore all to 100%
- [ ] Far-right corner: hover = peek all, click = toggle minimize all
- [ ] Codex: `System\Shell\EnablePeek`
- [ ] Commit: `"desktop: window peek (Aero Peek)"`

---

## 7. Start Menu *(from Phase 04 §2)*

### 7.1 Start Menu Layout *(from Phase 04 §2.1)*

**Prompt:** The Start Menu is a centered Windows 11-style popup with Acrylic blur, rounded corners, and drop shadow. Layout: search bar, pinned apps grid (4×2), recommended/recent section, footer (user + power). After completing all items, create `docs/architecture/start-menu.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: start menu layout"`.


- [ ] Create `src/desktop/start_menu.c`
- [ ] Windows 11 centered layout:
  - [ ] Search bar at top
  - [ ] Pinned apps grid (4×2 icons)
  - [ ] Recommended / Recent section
  - [ ] Footer: user avatar + name (left), power button (right)
- [ ] Acrylic blur background + rounded corners + drop shadow
- [ ] Commit: `"desktop: start menu layout"`

### 7.2 Start Menu Data *(from Phase 04 §2.2)*

**Prompt:** Load pinned apps from Codex, scan installed apps from filesystem. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: start menu data loading"`.


- [ ] Load pinned apps from Codex `User\{name}\Shell\PinnedApps`
- [ ] Load recent files from Codex `User\{name}\Shell\RecentFiles`
- [ ] Scan installed apps from `C:\Impossible\Bin\` and `C:\Programs\`
- [ ] Display app icons from icon store
- [ ] Commit: `"desktop: start menu data loading"`

### 7.3 Start Menu Interaction *(from Phase 04 §2.3)*

**Prompt:** Toggle open/close on Start click or Win key. Launch apps, open files, power submenu, search filtering. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: start menu interaction"`.


- [ ] Start button / Win key → toggle menu
- [ ] Click pinned app → launch, close menu
- [ ] Click recent file → open with associated app
- [ ] Power button → Shut down, Restart, Sleep, Lock
- [ ] Search: filter apps + files by typed query
- [ ] Slide-up animation (200ms, `GFX_EASE_OUT_CUBIC`)
- [ ] Click outside / Escape → close
- [ ] Commit: `"desktop: start menu interaction"`

---

## 8. System Tray & Notifications *(from Phase 04 §3)*

### 8.1 System Tray Icons *(from Phase 04 §3.1)*

**Prompt:** System tray: volume, network, notification bell icons. Each clickable with popups. After completing all items, create `docs/architecture/system-tray.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: system tray icons"`.


- [ ] Create `src/desktop/systray.c`
- [ ] 🔊 Volume icon → volume slider popup
- [ ] 🌐 Network icon → status popup (IP, connected/disconnected)
- [ ] 🔔 Notification bell → open notification center
- [ ] Dynamic icon updates (e.g., muted = different icon)
- [ ] Commit: `"desktop: system tray icons"`

### 8.2 Notification Toasts *(from Phase 02 §9.3 + Phase 04 §3.2)*

**Prompt:** Toast notifications slide in from bottom-right. Auto-dismiss after timeout, stack vertically. After completing all items, create `docs/architecture/notifications.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: notification toasts"`.


- [ ] Create `src/desktop/notify.c`
- [ ] `notify_send(title, msg, icon)` → display toast
- [ ] Slide-in animation from right edge (300ms, ease-out-cubic)
- [ ] Auto-dismiss after 5 seconds (configurable)
- [ ] Stack multiple toasts vertically
- [ ] Icon + bold title + message body + [Dismiss] button
- [ ] Commit: `"desktop: notification toasts"`

### 8.3 Notification Center *(from Phase 04 §7)*

**Prompt:** Slide-in panel from right edge with notification history. After completing all items, create `docs/architecture/notification-center.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: notification center"`.


- [ ] Create `src/desktop/notify_center.c`
- [ ] Slide-in panel from right edge
- [ ] Past notifications: grouped by source, icon + title + msg + timestamp
- [ ] Dismiss individual (X) or "Clear all"
- [ ] Store last 100 in Codex `System\Shell\NotifyHistory`
- [ ] Open: click 🔔 / Close: click outside or Escape
- [ ] Commit: `"desktop: notification center"`

---

## 9. Desktop Icons & Shortcuts *(from Phase 04 §15.4)*

**Prompt:** Desktop icons rendered in a grid layout. Default: "This PC", "Recycle Bin", user shortcuts. Single-click selects, double-click opens. Labels with text shadow for readability. After completing all items, create `docs/architecture/desktop-icons.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: desktop icon grid"`.


- [ ] Render icons on desktop surface (grid-aligned)
- [ ] Default icons: This PC, Recycle Bin
- [ ] User shortcuts from `C:\Users\{name}\Desktop\` (.lnk files)
- [ ] Single-click selects (highlight rect), double-click opens
- [ ] Drag to reorder, auto-arrange option
- [ ] Label text below icon with text shadow
- [ ] Commit: `"desktop: desktop icon grid"`

---

## 10. Keyboard Shortcuts & Task Switching *(from Phase 04 §15)*

### 10.1 Keyboard Shortcut Manager *(from Phase 04 §15.1)*

**Prompt:** Centralize all system-wide keyboard shortcuts in a hotkey table. After completing all items, create `docs/architecture/keyboard-shortcuts.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: keyboard shortcut manager"`.


- [ ] Define hotkey table (key combo → action callback)
- [ ] Register shortcuts: Win (Start), Win+E (Files), Win+L (Lock), Win+D (Desktop), Win+R (Run), Alt+Tab (Switch), Alt+F4 (Close), PrtSc (Screenshot)
- [ ] Allow user-defined shortcuts via Codex `System\Shell\Hotkeys\`
- [ ] Commit: `"desktop: keyboard shortcut manager"`

### 10.2 Alt+Tab Task Switcher *(from Phase 04 §15.2)*

**Prompt:** Alt+Tab overlay with window thumbnails. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: Alt+Tab task switcher"`.


- [ ] Alt+Tab → centered overlay with window thumbnails
- [ ] Tab cycles selection while Alt held
- [ ] Release Alt → focus selected window
- [ ] Show title + icon below each thumbnail
- [ ] Acrylic background panel
- [ ] Commit: `"desktop: Alt+Tab task switcher"`

### 10.3 Run Dialog (Win+R) *(from Phase 04 §15.3)*

**Prompt:** Small dialog with "Open:" text field, execute command/path. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: Win+R run dialog"`.


- [ ] Win+R → small dialog with text field
- [ ] Type command/path → execute
- [ ] History (last 20, Codex `User\Default\Shell\RunHistory`)
- [ ] Auto-complete from PATH
- [ ] Commit: `"desktop: Win+R run dialog"`

---

## 11. Drag and Drop *(from Phase 04 §8)*

### 11.1 Core System *(from Phase 04 §8.1)*

**Prompt:** Drag-and-drop with state tracking, visual feedback, and drop delivery. After completing all items, create `docs/architecture/drag-drop.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: drag-and-drop system"`.


- [ ] Define `drag_state_t` (active, format, data, cursor, drag_icon, source_window)
- [ ] Create `src/desktop/drag.c`
- [ ] `drag_begin(fmt, data, size, icon)` / `drag_update(mx, my)` / `drag_drop(target)`
- [ ] `drag_cancel()` — Escape to cancel
- [ ] Semi-transparent drag icon follows cursor
- [ ] Highlight valid drop targets, forbidden cursor on invalid
- [ ] File drag (move/copy), text drag, *(Stretch)* taskbar reorder
- [ ] Commit: `"desktop: drag-and-drop system"`

---

## 12. Quick Settings Panel *(from Phase 04 §9)*

**Prompt:** Popup panel with toggle grid + volume/brightness sliders. After completing all items, create `docs/architecture/quick-settings.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: quick settings panel"`.


- [ ] Create `src/desktop/quick_settings.c`
- [ ] Open: click system tray / Win+A
- [ ] 3×2 toggle grid: WiFi, Bluetooth, Airplane Mode, Night Light, DND, Cast
- [ ] Volume slider + Brightness slider
- [ ] [Edit ⚙] → open Settings app
- [ ] Acrylic background, rounded corners, drop shadow
- [ ] Commit: `"desktop: quick settings panel"`

---

## 13. Screenshot Capture *(from Phase 02 §9.4)*

**Prompt:** Capture framebuffer to PNG on Print Screen key. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: screenshot capture"`.


- [ ] PrtSc key → save framebuffer to `C:\Users\Default\Screenshots\screenshot_{timestamp}.png`
- [ ] Use `image_save_png()` from image API
- [ ] Notification toast: "Screenshot saved"
- [ ] Commit: `"kernel: screenshot capture"`

---

## 14. DPI Scaling *(from Phase 02 §8)*

### 14.1 DPI System *(from Phase 02 §8.1)*

**Prompt:** DPI scaling multiplies all UI dimensions by a scale factor. After completing all items, create `docs/architecture/dpi-scaling.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"display: DPI scaling system"`.


- [ ] Create `include/dpi.h` with `DPI(px)` macro: `(pixels * scale / 100)`
- [ ] `dpi_get_scale()`, `dpi_auto_detect()` (≥3840→200%, ≥2560→150%, else 100%)
- [ ] Store in Codex: `System\Display\Scale`, `System\Display\AutoScale`
- [ ] Commit: `"display: DPI scaling system"`

### 14.2 DPI-Aware UI *(from Phase 02 §8.2)*

**Prompt:** Replace all hardcoded pixel sizes with `DPI()` calls. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: DPI-aware layout"`.


- [ ] `desktop.c`: taskbar height `DPI(48)`, button padding, menu sizes
- [ ] `wm.c`: title bar `DPI(32)`, borders, corner radius, button sizes
- [ ] `controls.c`: scrollbar width `DPI(16)`, minimum click target `DPI(32)`
- [ ] Font sizes: `font_get(FONT_UI, DPI(14))`
- [ ] Icons: pick 32/48/64 based on DPI
- [ ] Commit: `"desktop: DPI-aware layout"`

---

## 15. Virtual Desktops *(from Phase 04 §6)*

**Prompt:** Virtual desktops with up to 8 workspaces. After completing all items, create `docs/architecture/virtual-desktops.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: virtual desktop manager"`.


- [ ] Create `src/kernel/wm_vdesktop.c`
- [ ] `struct virtual_desktop` (windows list, name), up to 8 desktops
- [ ] `vdesktop_create()` / `vdesktop_remove(id)` / `vdesktop_switch(id)`
- [ ] Default "Desktop 1" at boot
- [ ] Taskbar shows only active desktop's windows
- [ ] Ctrl+Win+Left/Right → switch desktops
- [ ] Ctrl+Win+D → create new / Ctrl+Win+F4 → close current
- [ ] *(Stretch)* Win+Tab overview with thumbnails
- [ ] Commit: `"desktop: virtual desktop manager"`

---

## 16. Night Light *(from Phase 04 §13)*

**Prompt:** Blue light filter reduces blue channel for eye comfort at night. After completing all items, create `docs/architecture/night-light.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"display: night light"`.


- [ ] Create `src/kernel/display/nightlight.c`
- [ ] `nightlight_apply(surface, intensity)` — reduce blue 0–50%
- [ ] Apply in compositor final blit step
- [ ] Gradual transition over 30 minutes
- [ ] Schedule: auto on/off by time (e.g., 9PM → 7AM)
- [ ] Codex: `System\Display\NightLight`, `NightLightIntensity`, `NightLightStart/End`
- [ ] Commit: `"display: night light""`

---

## 17. Focus / Do Not Disturb *(from Phase 04 §14)*

**Prompt:** Suppress notifications during focused work. After completing all items, create `docs/architecture/focus-mode.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: focus / do not disturb"`.


- [ ] Create `src/desktop/focus_mode.c`
- [ ] Modes: Off, Priority Only, Do Not Disturb
- [ ] Toggle via Quick Settings / Win+N
- [ ] Auto-activate during fullscreen apps or scheduled hours
- [ ] Badge on tray bell with suppressed count
- [ ] When DND ends: summary "You missed N notifications"
- [ ] Codex: `System\Shell\FocusMode`, `FocusScheduleStart/End`
- [ ] Commit: `"desktop: focus / do not disturb"`

---

## 18. Boot Splash Screen *(from Phase 04 §10)*

**Prompt:** Graphical boot splash with logo and progress bar. After completing all items, create `docs/architecture/boot-splash.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: graphical boot splash"`.


- [ ] Create `src/kernel/boot_splash.c`
- [ ] `boot_splash_init()`, `boot_splash_progress(pct)`, `boot_splash_status(msg)`, `boot_splash_finish()`
- [ ] Progress milestones: 10% PMM → 20% drivers → 40% FS → 60% network → 80% desktop → 100%
- [ ] Centered logo, gradient background, smooth progress bar
- [ ] *(Stretch)* F8 boot menu: Normal, Safe mode, Recovery, Last known good
- [ ] Commit: `"kernel: graphical boot splash"`

---

## 19. Screensaver & Lock Screen *(from Phase 04 §11)*

### 19.1 Screensaver System *(from Phase 04 §11.1–11.2)*

**Prompt:** Idle detection + screensaver API + 5 built-in screensavers. After completing all items, create `docs/architecture/screensaver.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: screensaver system"`.


- [ ] Create `src/desktop/screensaver.c`
- [ ] Screensaver API: `scr_entry_fn(msg, surface)` — SCR_INIT/FRAME/CLOSE
- [ ] Idle detection, configurable timeout
- [ ] Dismiss on any input
- [ ] Built-in: Blank, Starfield, Matrix, Bouncing Logo, Clock
- [ ] Codex: `System\Screensaver\IdleTimeout`, `System\Screensaver\Type`
- [ ] Commit: `"desktop: screensaver system"`

### 19.2 Lock Screen *(from Phase 04 §11.3)*

**Prompt:** Full-screen lock with blurred wallpaper, clock, password input. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: lock screen"`.


- [ ] Create `src/desktop/lockscreen.c`
- [ ] Blurred wallpaper background
- [ ] Large clock + date, user avatar + name
- [ ] Password input field + [Unlock →] button
- [ ] Win+L shortcut, auto-lock after screensaver
- [ ] Codex: `System\Screensaver\RequirePassword`
- [ ] Commit: `"desktop: lock screen"`

---

## 20. Desktop Widgets *(from Phase 04 §12)*

**Prompt:** Desktop widgets are floating panels above wallpaper, below windows. After completing all items, create `docs/architecture/widgets.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: widget framework"`.


- [ ] Create `src/desktop/widgets.c`
- [ ] Widget API: `widget_fn(msg, surface, ctx)` — WGT_INIT/RENDER/TICK/CLOSE
- [ ] Widget manager: load, position, update
- [ ] Draggable positioning, semi-transparent background
- [ ] Built-in: Clock, CPU Meter, RAM Monitor, Calendar, Quick Notes
- [ ] Commit: `"desktop: widget framework + built-in widgets"`

---

## 21. Display & Resolution *(from Phase 02 §8.3–8.4)*

### 21.1 Dynamic Resolution *(from Phase 02 §8.3)*

**Prompt:** VESA/VBE mode enumeration and resolution switching. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"display: resolution management"`.


- [ ] `display_enum_modes()` — query VESA/VBE modes
- [ ] `display_get_mode()` — return current resolution
- [ ] Configurable resolution in Multiboot2 header
- [ ] *(Stretch)* `display_set_mode(w, h)` — runtime change (requires virtio-gpu)
- [ ] Commit: `"display: resolution management"`

### 21.2 Multi-Monitor *(from Phase 02 §8.4 — Stretch)*

- [ ] *(Stretch)* `struct monitor` (id, resolution, position, DPI, framebuffer)
- [ ] *(Stretch)* Virtual desktop coordinate space
- [ ] *(Stretch)* Per-monitor DPI

---

## 22. Software OpenGL *(from Phase 02 §7)*

**Prompt:** TinyGL (software OpenGL 1.1) for 3D rendering. After completing all items, create `docs/architecture/opengl.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"gfx: TinyGL software OpenGL 1.1"`.


- [ ] Port TinyGL (~5000 lines, Zlib license) to Impossible OS framebuffer
- [ ] Basic OpenGL 1.1: `glBegin/glEnd`, vertices, colors, textures, z-buffer
- [ ] Test: rotating cube
- [ ] Commit: `"gfx: TinyGL software OpenGL 1.1"`

---

## Already Completed ✅

> These sections were completed in previous phases. They are included here for
> completeness and as a cross-reference.

- [x] **2D Compositing Library** — surfaces, primitives, alpha blending, gradients, blur, Mica, Acrylic, shadows, reveal highlight, SIMD optimization *(Phase 02 §1)*
- [x] **TrueType Font System** — stb_truetype, font manager, glyph caching, replaced bitmap font *(Phase 02 §2)*
- [x] **Runtime Image Decoding** — stb_image, JPEG/PNG wallpaper, image scaling *(Phase 02 §3)*
- [x] **System Icon Store** — IRES format, Fluent UI font icons, ICO loader, file type mapping *(Phase 02 §4)*
- [x] **Cursor Manager** — Adwaita X11 cursors, 11 shapes, embedded fallbacks *(Phase 02 §5.1–5.2)*
- [x] **Context-Aware Cursor Switching** — wm/desktop context, resize/move/hand cursors *(Phase 02 §5.3)*
- [x] **Dirty Rectangle Compositor** — partial redraws, fb_swap_rect *(implemented)*
- [x] **Basic Window Manager** — create, move, resize, close, title bar, focus *(Phase 01)*
- [x] **Basic Taskbar** — start button, clock, window buttons *(Phase 01)*
- [x] **Basic Controls** — Button, Label, TextBox, ScrollBar *(implemented)*

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. Theme System | Foundation — every other item depends on themed colors |
| 🔴 P0 | 2.1–2.4 Checkbox/Radio/Dropdown/Slider | Core widgets needed by every app |
| 🔴 P0 | 3.1 Tween Engine | Foundation for all animations |
| 🔴 P0 | 4.1 Minimize & Maximize | Core window management |
| 🔴 P0 | 5.1 Context Menu Engine | Foundation for all right-click menus |
| 🟠 P1 | 2.5–2.10 Tab/ListView/TreeView/Toolbar/MenuBar/StatusBar | Required for File Manager, Settings, apps |
| 🟠 P1 | 6.1 Taskbar Window List | Core UX — switch between apps |
| 🟠 P1 | 7. Start Menu | App launcher — most used element |
| 🟠 P1 | 9. Desktop Icons | Visual desktop experience |
| 🟠 P1 | 10. Keyboard Shortcuts + Alt+Tab | Essential navigation |
| 🟡 P2 | 2.11–2.13 GroupBox/Tooltip/Dialog | Polish controls |
| 🟡 P2 | 3.2 Window Animations | Fluid transitions |
| 🟡 P2 | 4.2–4.4 Window Snapping | Productivity tiling |
| 🟡 P2 | 5.2–5.3 Desktop/File Context Menu | Complete right-click UX |
| 🟡 P2 | 8. System Tray & Notifications | Status feedback |
| 🟡 P2 | 18. Boot Splash | First thing users see |
| 🟢 P3 | 11. Drag and Drop | File management UX |
| 🟢 P3 | 12. Quick Settings | System toggles |
| 🟢 P3 | 14. DPI Scaling | HiDPI display support |
| 🟢 P3 | 15. Virtual Desktops | Multi-workspace |
| 🟢 P3 | 16. Night Light | Eye comfort |
| 🔵 P4 | 6.2–6.3 Taskbar Context Menu / Peek | Polish |
| 🔵 P4 | 13. Screenshot | Utility |
| 🔵 P4 | 17. Focus / DND Mode | Notification control |
| 🔵 P4 | 19. Screensaver & Lock Screen | Security + flair |
| 🔵 P4 | 20. Desktop Widgets | Optional enhancements |
| 🔵 P4 | 21. Display/Resolution | Multi-monitor future |
| 🔵 P4 | 22. Software OpenGL | 3D rendering future |
