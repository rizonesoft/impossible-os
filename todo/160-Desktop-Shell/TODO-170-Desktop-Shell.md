# P0301 — Desktop Shell

> **Goal:** Window management enhancements, context menus, desktop icons,
> keyboard shortcuts, drag-and-drop, virtual desktops, and display features.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Window Manager Enhancements *(from Phase 04 §5, §15.5)*

### 1.1 Window Minimize & Maximize *(NEW — partially missing)*

**Prompt:** Add full minimize/maximize/restore support to the window manager. `wm_minimize(handle)` hides the window, `wm_maximize(handle)` saves the pre-max position and resizes to fill the usable desktop area (screen minus taskbar), `wm_restore(handle)` returns to the saved position. The maximize button in the title bar should toggle between maximize and restore. Double-clicking the title bar also toggles maximize. Add minimize and maximize buttons to the window title bar alongside the close button. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: window minimize and maximize"`. Add notes, gotchas, and design decisions directly in this TODO section covering minimize/maximize/restore API, title bar buttons, and double-click toggle.


- [ ] Add `wm_minimize(handle)` — hide window, mark as minimized
- [ ] Add `wm_maximize(handle)` — save position, resize to fill usable area
- [ ] Add `wm_restore(handle)` — return to saved pre-max position
- [ ] Minimize button (━) in title bar
- [ ] Maximize/Restore button (☐/❐) in title bar
- [ ] Double-click title bar → toggle maximize
- [ ] Commit: `"desktop: window minimize and maximize"`

### 1.2 Keyboard Window Snapping *(from Phase 04 §5.1)*

**Prompt:** Window snapping allows quick tiling of windows. Win+Left snaps the focused window to the left half of the screen, Win+Right to the right half, Win+Up maximizes, Win+Down restores or minimizes. Store the window's pre-snap position so restoring returns it to its original size. Use the animation engine (§3.1) to smoothly tween the snap transition. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: keyboard window snapping"`. Add notes, gotchas, and design decisions directly in this TODO section covering snap keyboard shortcuts, pre-snap position storage, and animation.


- [ ] Create `src/kernel/wm_snap.c`
- [ ] Win+Left → snap to left half of screen
- [ ] Win+Right → snap to right half
- [ ] Win+Up → maximize
- [ ] Win+Down → restore (if maximized) / minimize (if restored)
- [ ] Animate snap transitions (200ms slide + resize)
- [ ] Commit: `"desktop: keyboard window snapping"`

### 1.3 Edge Snapping (Mouse) *(from Phase 04 §5.2)*

**Prompt:** When dragging a window, detect if the cursor hits a screen edge and show a snap preview. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: edge snap with preview"`. Add notes, gotchas, and design decisions directly in this TODO section covering edge detection, snap preview overlay rendering, and quarter-screen zones.


- [ ] Drag to top edge → maximize preview overlay
- [ ] Drag to left/right edge → half-screen preview
- [ ] Drag to corner → quarter-screen preview
- [ ] Show snap preview zone (semi-transparent overlay) before drop
- [ ] Commit: `"desktop: edge snap with preview"`

### 1.4 Snap Layouts *(from Phase 04 §5.3)*

**Prompt:** Hovering over a window's maximize button shows a popup with visual layout options: 50/50 left-right, 50/50 top-bottom, 66/33 wide-narrow, and 33/33/33 three columns. This mimics Windows 11's Snap Layouts feature. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: snap layouts on maximize hover"`. Add notes, gotchas, and design decisions directly in this TODO section covering snap layout options, maximize button hover, and zone filling.


- [ ] Hover maximize button → show snap layout popup
- [ ] Layout options: 50/50 LR, 50/50 TB, 66/33 wide-narrow, 33/33/33
- [ ] Click zone → snap current window, prompt to fill remaining zones
- [ ] Commit: `"desktop: snap layouts on maximize hover"`

### 1.5 Window Minimize/Restore All *(from Phase 04 §15.5)*

**Prompt:** Win+M minimizes all windows. Win+Shift+M restores all previously-minimized windows. Win+D toggles between minimize-all and restore-all (show desktop toggle). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: minimize/restore all windows"`. Add notes, gotchas, and design decisions directly in this TODO section covering Win+M, Win+Shift+M, and Win+D behavior.


- [ ] Win+M → minimize all windows
- [ ] Win+Shift+M → restore all minimized windows
- [ ] Win+D → toggle show desktop
- [ ] Commit: `"desktop: minimize/restore all windows"`


## 2. Context Menu System *(from Phase 02 §9.2 + Phase 04 §4)*

> Required by desktop right-click, file right-click, taskbar right-click,
> and the menu bar widget.

### 2.1 Generic Context Menu Engine *(from Phase 04 §4.1)*

**Prompt:** Build a generic reusable context menu system: `context_menu_show(x, y, items, count)` renders a floating menu with Acrylic blur background, rounded corners, and drop shadow. Each `menu_item` has: label, optional icon, callback, optional submenu pointer, separator flag, disabled flag, and checked flag. Handle keyboard navigation, submenu open on hover, and auto-close when clicking outside. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: context menu system"`. Add notes, gotchas, and design decisions directly in this TODO section covering the context menu API, submenu handling, and keyboard navigation.


- [ ] Define `struct menu_item` (label, icon, callback, submenu, separator, disabled, checked)
- [ ] Create `src/desktop/context_menu.c`
- [ ] `context_menu_show(x, y, items, count)` — display at position
- [ ] Render: rounded rect + drop shadow + Acrylic blur background
- [ ] Keyboard: up/down arrows, Enter to select, Escape to close
- [ ] Submenu support: `►` arrow, hover to open (300ms delay)
- [ ] Separator lines, disabled (grayed), checked (✓) items
- [ ] Auto-close on click outside
- [ ] Commit: `"desktop: context menu system"`

### 2.2 Desktop Context Menu *(from Phase 04 §4.2)*

**Prompt:** Right-clicking the desktop wallpaper shows a context menu with: View submenu, Sort By submenu, Refresh, New submenu (Folder, Text Document, Shortcut), Paste, Display Settings, Personalize. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: desktop right-click menu"`. Add notes, gotchas, and design decisions directly in this TODO section covering the desktop context menu items and submenus.


- [ ] Right-click desktop → context menu:
  - [ ] View ► → Large icons, Medium, Small, List
  - [ ] Sort by ► → Name, Date, Size, Type
  - [ ] Refresh
  - [ ] New ► → Folder, Text Document, Shortcut
  - [ ] Paste (if clipboard has files)
  - [ ] Display settings / Personalize
- [ ] Commit: `"desktop: desktop right-click menu"`

### 2.3 File Context Menu *(from Phase 04 §4.3)*

**Prompt:** Right-clicking a file icon shows: Open, Open With, Cut/Copy/Paste, Delete, Rename, Properties. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: file context menu"`. Add notes, gotchas, and design decisions directly in this TODO section covering the file context menu items and action dispatch.


- [ ] Right-click file → context menu:
  - [ ] Open / Open with... ►
  - [ ] Cut / Copy / Paste
  - [ ] Delete (recycle bin) / Rename
  - [ ] Properties (size, type, path, timestamps)
- [ ] Commit: `"desktop: file context menu"`


## 3. Desktop Icons & Shortcuts *(from Phase 04 §15.4)*

**Prompt:** Desktop icons rendered in a grid layout. Default: "This PC", "Recycle Bin", user shortcuts. Single-click selects, double-click opens. Labels with text shadow for readability. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: desktop icon grid"`. Add notes, gotchas, and design decisions directly in this TODO section covering the icon grid layout, default icons, shortcut handling, and drag reorder.


- [ ] Render icons on desktop surface (grid-aligned)
- [ ] Default icons: This PC, Recycle Bin
- [ ] User shortcuts from `C:\Users\{name}\Desktop\` (.lnk files)
- [ ] Single-click selects (highlight rect), double-click opens
- [ ] Drag to reorder, auto-arrange option
- [ ] Label text below icon with text shadow
- [ ] Commit: `"desktop: desktop icon grid"`


## 4. Keyboard Shortcuts & Task Switching *(from Phase 04 §15)*

### 4.1 Keyboard Shortcut Manager *(from Phase 04 §15.1)*

**Prompt:** Centralize all system-wide keyboard shortcuts in a hotkey table. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: keyboard shortcut manager"`. Add notes, gotchas, and design decisions directly in this TODO section covering the hotkey table, registration API, and user-defined shortcuts.


- [ ] Define hotkey table (key combo → action callback)
- [ ] Register shortcuts: Win (Start), Win+E (Files), Win+L (Lock), Win+D (Desktop), Win+R (Run), Alt+Tab (Switch), Alt+F4 (Close), PrtSc (Screenshot)
- [ ] Allow user-defined shortcuts via Registry `HKCU\Software\Impossible\Shell\Hotkeys\`
- [ ] Commit: `"desktop: keyboard shortcut manager"`

### 4.2 Alt+Tab Task Switcher *(from Phase 04 §15.2)*

**Prompt:** Alt+Tab overlay with window thumbnails. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: Alt+Tab task switcher"`. Add notes, gotchas, and design decisions directly in this TODO section covering the Alt+Tab overlay, thumbnail rendering, and focus handling.


- [ ] Alt+Tab → centered overlay with window thumbnails
- [ ] Tab cycles selection while Alt held
- [ ] Release Alt → focus selected window
- [ ] Show title + icon below each thumbnail
- [ ] Acrylic background panel
- [ ] Commit: `"desktop: Alt+Tab task switcher"`

### 4.3 Run Dialog (Win+R) *(from Phase 04 §15.3)*

**Prompt:** Small dialog with "Open:" text field, execute command/path. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: Win+R run dialog"`. Add notes, gotchas, and design decisions directly in this TODO section covering the Run dialog, command execution, history, and auto-complete.


- [ ] Win+R → small dialog with text field
- [ ] Type command/path → execute
- [ ] History (last 20, Registry `HKU\Default\Software\Impossible\Shell\RunHistory`)
- [ ] Auto-complete from PATH
- [ ] Commit: `"desktop: Win+R run dialog"`


## 5. Drag and Drop *(from Phase 04 §8)*

### 5.1 Core System *(from Phase 04 §8.1)*

**Prompt:** Drag-and-drop with state tracking, visual feedback, and drop delivery. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: drag-and-drop system"`. Add notes, gotchas, and design decisions directly in this TODO section covering the drag state machine, visual feedback, drop targets, and data formats.


- [ ] Define `drag_state_t` (active, format, data, cursor, drag_icon, source_window)
- [ ] Create `src/desktop/drag.c`
- [ ] `drag_begin(fmt, data, size, icon)` / `drag_update(mx, my)` / `drag_drop(target)`
- [ ] `drag_cancel()` — Escape to cancel
- [ ] Semi-transparent drag icon follows cursor
- [ ] Highlight valid drop targets, forbidden cursor on invalid
- [ ] File drag (move/copy), text drag, *(Stretch)* taskbar reorder
- [ ] Commit: `"desktop: drag-and-drop system"`


## 6. Quick Settings Panel *(from Phase 04 §9)*

**Prompt:** Popup panel with toggle grid + volume/brightness sliders. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: quick settings panel"`. Add notes, gotchas, and design decisions directly in this TODO section covering the quick settings panel, toggle grid, and slider controls.


- [ ] Create `src/desktop/quick_settings.c`
- [ ] Open: click system tray / Win+A
- [ ] 3×2 toggle grid: WiFi, Bluetooth, Airplane Mode, Night Light, DND, Cast
- [ ] Volume slider + Brightness slider
- [ ] [Edit ⚙] → open Control Panel
- [ ] Acrylic background, rounded corners, drop shadow
- [ ] Commit: `"desktop: quick settings panel"`


## 7. Virtual Desktops *(from Phase 04 §6)*

**Prompt:** Virtual desktops with up to 8 workspaces. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: virtual desktop manager"`. Add notes, gotchas, and design decisions directly in this TODO section covering the virtual desktop API, keyboard shortcuts, and taskbar integration.


- [ ] Create `src/kernel/wm_vdesktop.c`
- [ ] `struct virtual_desktop` (windows list, name), up to 8 desktops
- [ ] `vdesktop_create()` / `vdesktop_remove(id)` / `vdesktop_switch(id)`
- [ ] Default "Desktop 1" at boot
- [ ] Taskbar shows only active desktop's windows
- [ ] Ctrl+Win+Left/Right → switch desktops
- [ ] Ctrl+Win+D → create new / Ctrl+Win+F4 → close current
- [ ] *(Stretch)* Win+Tab overview with thumbnails
- [ ] Commit: `"desktop: virtual desktop manager"`


## 8. Night Light *(from Phase 04 §13)*

**Prompt:** Blue light filter reduces blue channel for eye comfort at night. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"display: night light"`. Add notes, gotchas, and design decisions directly in this TODO section covering the night light algorithm, scheduling, and Registry settings.


- [ ] Create `src/kernel/display/nightlight.c`
- [ ] `nightlight_apply(surface, intensity)` — reduce blue 0–50%
- [ ] Apply in compositor final blit step
- [ ] Gradual transition over 30 minutes
- [ ] Schedule: auto on/off by time (e.g., 9PM → 7AM)
- [ ] Registry: `HKLM\SYSTEM\Display\NightLight`, `NightLightIntensity`, `NightLightStart/End`
- [ ] Commit: `"display: night light""`


## 9. Focus / Do Not Disturb *(from Phase 04 §14)*

**Prompt:** Suppress notifications during focused work. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: focus / do not disturb"`. Add notes, gotchas, and design decisions directly in this TODO section covering the focus mode API, DND levels, and scheduled activation.


- [ ] Create `src/desktop/focus_mode.c`
- [ ] Modes: Off, Priority Only, Do Not Disturb
- [ ] Toggle via Quick Settings / Win+N
- [ ] Auto-activate during fullscreen apps or scheduled hours
- [ ] Badge on tray bell with suppressed count
- [ ] When DND ends: summary "You missed N notifications"
- [ ] Registry: `HKCU\Software\Impossible\Shell\FocusMode`, `FocusScheduleStart/End`
- [ ] Commit: `"desktop: focus / do not disturb"`



---

## 10. Compositor Performance

> *Incorporated from parking-lot P12–P16*

### 10.1 Cached Acrylic for Taskbar & Start Menu

- **Taskbar** (fixed position, always visible):
  - [ ] On wallpaper load/change: pre-blur the taskbar strip region → store as PMM-allocated cached texture
  - [ ] Each frame: fast-blit cached texture instead of recomputing `gfx_acrylic()`
  - [ ] Invalidate cache only when wallpaper changes or screen resolution changes
- **Start menu** (fixed position when open):
  - [ ] On menu open: snapshot + blur the menu region once → store as cached texture
  - [ ] Each frame while open: fast-blit cached texture
  - [ ] Invalidate on close (re-snapshot + re-blur on next open)
- **Invalidation triggers:**
  - [ ] Wallpaper change → invalidate both caches
  - [ ] Resolution change → invalidate + reallocate

### ~~10.2 Dirty-rectangle compositor during drag~~ ✅

> Done. `wm_is_dragging()` + `wm_get_drag_dirty_rect()` API.
> `fb_swap_rect()` on drag dirty region (~200 KB vs 3.6 MB).
> Terminal render gated behind `!wm_is_dragging()`.

### ~~10.3 Batch mouse events~~ ✅

> Done (commit `8f9ec05`). Up to 4 coalesced reads per frame.
> Replaced `yield()` with `sti; hlt` (instant IRQ wake).

---

## Priority Order

| Priority | Section                        | Reason                                            |
|----------|--------------------------------|---------------------------------------------------|
| 🔴 P0    | §1.1 Minimize + Maximize       | Core WM feature — every app needs it             |
| 🔴 P0    | §2.1 Context Menu Engine       | Foundation for all right-click menus             |
| 🔴 P0    | §4.1 Keyboard Shortcut Manager | Win+E, Win+L, Alt+Tab — essential shortcuts      |
| 🟠 P1    | §2.2 Desktop Context Menu      | Right-click desktop — core desktop interaction   |
| 🟠 P1    | §3 Desktop Icons + Shortcuts   | Icons on desktop — This PC, Recycle Bin          |
| 🟠 P1    | §4.2 Alt+Tab Task Switcher     | App switching — essential desktop UX             |
| 🟡 P2    | §1.2 Keyboard Window Snapping  | Win+Left/Right/Up/Down — productivity feature    |
| 🟡 P2    | §2.3 File Context Menu         | Right-click file — Open, Copy, Delete, Rename    |
| 🟡 P2    | §4.3 Win+R Run Dialog          | Run commands / open files                        |
| 🟡 P2    | §5 Drag and Drop               | File operations in File Manager                  |
| 🟡 P2    | §6 Quick Settings Panel        | Win+A — toggles + sliders                        |
| 🟡 P2    | §10.1 Cached Acrylic           | Compositor perf: pre-blur taskbar/start menu     |
| 🟢 P3    | §1.3 Edge Snapping             | Drag to edge → snap preview                      |
| 🟢 P3    | §7 Virtual Desktops            | Ctrl+Win+Left/Right workspaces                   |
| 🟢 P3    | §8 Night Light                 | Blue filter → eye comfort                        |
| 🟢 P3    | §9 Focus / DND Mode            | Suppress notifications during work               |
| 🔵 P4    | §1.4 Snap Layouts              | Maximize hover → layout picker                   |
| 🔵 P4    | §1.5 Minimize All (Win+M)      | Show desktop toggle                              |

---

## Key Files

| File                              | Purpose                                    |
|-----------------------------------|--------------------------------------------|
| `src/kernel/wm.c`                 | [MODIFY] Add minimize/maximize/restore     |
| `src/kernel/wm_snap.c`            | [NEW] Keyboard + edge snapping             |
| `src/kernel/wm_vdesktop.c`        | [NEW] Virtual desktop manager              |
| `src/kernel/wm_anim.c`            | [NEW] Window animation state (see TODO-140)|
| `src/desktop/context_menu.c`      | [NEW] Generic context menu engine          |
| `src/desktop/drag.c`              | [NEW] Drag-and-drop system                 |
| `src/desktop/quick_settings.c`    | [NEW] Quick settings panel                 |
| `src/desktop/focus_mode.c`        | [NEW] Focus / DND mode                     |
| `src/kernel/display/nightlight.c` | [NEW] Night light blue filter              |

---

## OS Comparison

| Feature                         | Windows 11                            | Linux (GNOME/KDE)                    | Impossible OS                          |
|---------------------------------|---------------------------------------|--------------------------------------|----------------------------------------|
| Minimize + Maximize             | ✅ Always present                     | ✅ GNOME (toggleable) / KDE           | ⬜ §1.1 P0                            |
| Keyboard window snapping        | ✅ Win+Arrow keys                     | ✅ KWin / GNOME (with extension)      | ⬜ §1.2 P2                            |
| Edge snap with preview          | ✅ Snap assist zones                  | ✅ KWin edge snap                     | ⬜ §1.3 P3                            |
| Snap Layouts (maximize hover)   | ✅ Win11 exclusive                    | ⚠️ KWin layout switcher (extension)  | ⬜ §1.4 P4                            |
| Win+M minimize all              | ✅ Win+M / Win+D                      | ✅ Super+H (GNOME)                    | ⬜ §1.5 P4                            |
| Context menus (desktop/file)    | ✅ Explorer shell right-click         | ✅ Nautilus / Dolphin                 | ⬜ §2 P0-P1                           |
| Desktop icons (This PC, Bin)    | ✅ Desktop.ini controlled             | ✅ GNOME/Nautilus desktop icons       | ⬜ §3 P1                              |
| Alt+Tab task switcher           | ✅ Windows Task Switcher              | ✅ GNOME Overview / KWin              | ⬜ §4.2 P1                            |
| Win+R Run dialog                | ✅ Run dialog                         | ✅ GNOME Run (Alt+F2)                 | ⬜ §4.3 P2                            |
| Drag and drop                   | ✅ OLE Drag-and-Drop                  | ✅ GDK DnD / XDnD                     | ⬜ §5 P2                              |
| Quick Settings panel            | ✅ Win+A action center                | ✅ GNOME quick settings               | ⬜ §6 P2                              |
| Virtual desktops                | ✅ Win+Ctrl+D                         | ✅ GNOME / KWin workspaces            | ⬜ §7 P3                              |
| Night Light / Blue filter       | ✅ Settings → Display                 | ✅ Redshift / GNOME Night Light       | ⬜ §8 P3                              |
| Focus / Do Not Disturb          | ✅ Focus Assist                       | ✅ GNOME DND                          | ⬜ §9 P3                              |
| **Snap Layouts on hover**       | ✅ Win11 unique feature               | ❌ (extension required)              | ⬜ §1.4 — **matches Win11 parity**    |
| **Dirty-rect drag compositor**  | ✅ DWM                                | ✅ Mutter                             | ✅ Done — §10.2                        |
