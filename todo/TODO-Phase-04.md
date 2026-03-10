# Phase 04 — Desktop Shell

> **All items from this file have been consolidated into
> [TODO-Phase-02-GUI.md](TODO-Phase-02-GUI.md) — the master GUI TODO.**
>
> This includes: Taskbar Window List (§1), Start Menu (§2), System Tray &
> Notifications (§3), Context Menus (§4), Window Snapping (§5), Virtual
> Desktops (§6), Notification Center (§7), Drag and Drop (§8), Quick Settings
> (§9), Boot Splash (§10), Screensaver & Lock Screen (§11), Desktop Widgets
> (§12), Night Light (§13), Focus/DND (§14), Keyboard Shortcuts, Alt+Tab,
> Run Dialog, Desktop Icons, Minimize/Restore All (§15).


---

## 1. Taskbar Window List
> *Research: [01_taskbar_window_list.md](research/phase_04_desktop_shell/01_taskbar_window_list.md)*

### 1.1 Window Buttons on Taskbar

**Prompt:** The taskbar window list shows a button for each open window between the Start button and system tray. Study the existing taskbar rendering in `desktop.c` to understand the current layout. Each `taskbar_entry` tracks a window pointer, its title, icon, and active/flashing state. When the user clicks a window button, focus/raise that window. Clicking the already-active window's button should minimize it (toggle behavior like Windows). The active button gets an accent-colored underline. Use the WM's window list (from `wm.c`) as the data source. After completing all items, create `docs/architecture/taskbar.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: taskbar window list"`.


- [ ] Define `struct taskbar_entry` (window ptr, title, icon, active flag, flashing flag)
- [ ] Create `src/desktop/taskbar_winlist.c`
- [ ] Implement `taskbar_add_window(win)` — add entry when window is created
- [ ] Implement `taskbar_remove_window(win)` — remove entry when window is closed
- [ ] Implement `taskbar_set_active(win)` — highlight the focused window's button
- [ ] Implement `taskbar_flash(win)` — blink button to get user attention
- [ ] Draw window buttons between start button and system tray
- [ ] Active window button: highlighted with accent color + underline
- [ ] Click a window button → focus/raise that window
- [ ] Click the active window button → minimize it
- [ ] Commit: `"desktop: taskbar window list"`

### 1.2 Taskbar Button Context Menu

**Prompt:** Right-clicking a taskbar window button should show a context menu (using the context menu system from §4.1) with Close, Maximize/Restore (toggle text based on current state), and Minimize. Stretch goal: "Move to Desktop ►" submenu for virtual desktops (§6). The context menu callbacks directly call the WM functions (`wm_close_window`, `wm_maximize_window`, `wm_minimize_window`). After completing all items, update `docs/architecture/taskbar.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: taskbar button context menu"`.


- [ ] Right-click window button → context menu:
  - [ ] "Close" → close window
  - [ ] "Maximize" / "Restore" → toggle maximize
  - [ ] "Minimize"
  - [ ] *(Stretch)* "Move to Desktop ►" → move to virtual desktop
- [ ] Commit: `"desktop: taskbar button context menu"`

### 1.3 Window Peek (Aero Peek)

**Prompt:** Aero Peek provides a quick preview of a window by making all other windows transparent when hovering over its taskbar button (500ms delay). Set every window's alpha to 10% except the hovered one, using the compositor's per-window alpha multiplier. When the mouse leaves the taskbar, restore all windows to 100% alpha. The "Show Desktop" button is a thin clickable region at the far-right end of the taskbar: hovering peeks all windows, clicking toggles minimize-all/restore-all. Codex setting `System\Shell\EnablePeek` controls this feature. After completing all items, update `docs/architecture/taskbar.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: window peek (Aero Peek)"`.

> *Research: [13_window_peek.md](research/phase_04_desktop_shell/13_window_peek.md)*

- [ ] Hover taskbar button for 500ms → make all other windows 10% opacity
- [ ] Mouse leaves taskbar → restore all window opacity to 100%
- [ ] "Show Desktop" peek → hover far-right corner of taskbar → all windows transparent
- [ ] Click far-right corner → minimize all windows (show desktop toggle)
- [ ] Codex: `System\Shell\EnablePeek = 1`
- [ ] Commit: `"desktop: window peek (Aero Peek)"`

---

## 2. Start Menu
> *Research: [02_start_menu.md](research/phase_04_desktop_shell/02_start_menu.md)*

### 2.1 Start Menu Layout

**Prompt:** The Start Menu is a centered Windows 11-style popup anchored to the taskbar. It opens above the Start button with an Acrylic blur background (from Phase 02 §1.4), rounded corners, and drop shadow. Layout from top to bottom: search bar ("Search apps and files"), pinned apps grid (4×2 icons), recommended/recent section (list of recent files with timestamps and icons), and footer (user avatar + name on left, power button on right). The menu is a special WM window rendered above all others. After completing all items, create `docs/architecture/start-menu.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: start menu layout"`.


- [ ] Create `src/desktop/start_menu.c`
- [ ] Centered Windows 11 style layout:
  - [ ] Search bar at top: "Search apps and files"
  - [ ] **Pinned** section: grid of app icons (4 columns × 2 rows)
  - [ ] **Recommended** section: recent files with timestamps
  - [ ] **Footer**: user avatar + name (left), power button (right)
- [ ] Acrylic blur background via `gfx_acrylic()`
- [ ] Rounded corners + drop shadow
- [ ] Commit: `"desktop: start menu layout"`

### 2.2 Start Menu Data

**Prompt:** Pinned apps are loaded from Codex `User\{name}\Shell\PinnedApps` (list of app names/paths). Recent files come from `User\{name}\Shell\RecentFiles` (populated when any app opens a file). Installed apps are discovered by scanning `C:\Impossible\Bin\` and `C:\Programs\` for executables. Each app entry shows its icon from the icon store (Phase 02 §4.1) and its display name. After completing all items, update `docs/architecture/start-menu.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: start menu data loading"`.


- [ ] Load pinned apps from Codex `User\{name}\Shell\PinnedApps`
- [ ] Load recent files from Codex `User\{name}\Shell\RecentFiles`
- [ ] Scan installed apps from `C:\Impossible\Bin\` and `C:\Programs\`
- [ ] Display app icons from icon store
- [ ] Commit: `"desktop: start menu data loading"`

### 2.3 Start Menu Interaction

**Prompt:** The Start Menu toggles open/close on Start button click or Win key press. It should appear with a slide-up animation (200ms, ease-out-cubic from Phase 02 §6.1). Clicking a pinned app launches it via `task_exec`. Clicking a recent file calls `file_assoc_open` (Phase 03 §5.1). The power button shows a submenu: Shut Down (Phase 01 §8.1), Restart, Sleep, Lock (§11.3). The search bar filters apps + files using the search API (Phase 03 §6.2). Clicking outside or pressing Escape closes the menu. After completing all items, update `docs/architecture/start-menu.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: start menu interaction"`.


- [ ] Click start button (or Win key) → toggle start menu open/close
- [ ] Click pinned app → launch app, close menu
- [ ] Click recent file → open with associated app (`file_assoc_open`)
- [ ] Power button → submenu: Shut down, Restart, Sleep, Lock
- [ ] Search: filter apps + files by typed query
- [ ] Slide-up animation from taskbar (200ms, `GFX_EASE_OUT_CUBIC`)
- [ ] Click outside / press Escape → close menu
- [ ] Commit: `"desktop: start menu interaction"`

---

## 3. System Tray & Notifications
> *Research: [03_systray_notifications.md](research/phase_04_desktop_shell/03_systray_notifications.md)*

### 3.1 System Tray Icons

**Prompt:** The system tray sits between the notification center button and the clock on the right side of the taskbar. Draw small icons (16×16 or DPI-scaled) for volume, network, and notification bell. Each icon is clickable: volume opens a vertical slider popup, network shows connection status (IP address, connected/disconnected), and the bell opens the notification center (§7.1). Icons should update dynamically — e.g., network disconnected shows an 'X' overlay, muted volume shows a cross. After completing all items, create `docs/architecture/system-tray.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: system tray icons"`.


- [ ] Create `src/desktop/systray.c`
- [ ] Draw tray icon area to the left of the clock:
  - [ ] 🔊 Volume icon — click → volume slider popup
  - [ ] 🌐 Network icon — click → network status popup (IP, connected/disconnected)
  - [ ] 🔔 Notification bell — click → open notification center
- [ ] Update icons dynamically (e.g., network disconnected → different icon)
- [ ] Commit: `"desktop: system tray icons"`

### 3.2 Notification Toasts

**Prompt:** Check if notification toasts are already implemented from Phase 02 §9.3 or Phase 03 §11.3 — if so, wire them into the desktop and extend. If not, implement `notify_send(title, msg, icon)` which creates a toast window that slides in from the bottom-right, auto-dismisses after 5 seconds, stacks vertically with other active toasts, and shows an icon + bold title + message body + dismiss button. The notification service (Phase 03 §11.3) feeds events to this system. After completing all items, update `docs/architecture/system-tray.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: notification toasts"`.


- [ ] Create `src/desktop/notify.c`
- [ ] Implement `notify_send(title, msg, icon)` — display a toast notification
- [ ] Implement `notify_dismiss(id)` — dismiss a specific notification
- [ ] Toast slides in from bottom-right corner
- [ ] Auto-dismiss after 5 seconds (configurable)
- [ ] Show icon + title (bold) + message body + [Dismiss] button
- [ ] Stack multiple toasts vertically
- [ ] Commit: `"desktop: notification toasts"`

---

## 4. Context Menus
> *Research: [04_context_menus.md](research/phase_04_desktop_shell/04_context_menus.md)*

### 4.1 Context Menu System

**Prompt:** Check if the context menu system already exists from Phase 02 §9.2 — if it does, reuse it here. Otherwise, build a generic reusable system: `context_menu_show(x, y, items, count)` renders a floating menu with Acrylic blur background, rounded corners, and drop shadow. Each `menu_item` has: label, optional icon, callback, optional submenu pointer, separator flag, disabled flag, and checked flag. Handle keyboard navigation (up/down/Enter/Escape), submenu open on hover (300ms delay), and auto-close when clicking outside. This system is used everywhere: desktop, files, taskbar, Start menu power button. After completing all items, create `docs/architecture/context-menus.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: context menu system"`.


- [ ] Define `struct menu_item` (label, icon, callback, submenu ptr, separator, disabled, checked)
- [ ] Create `src/desktop/context_menu.c`
- [ ] Implement `context_menu_show(x, y, items, count)` — display menu at position
- [ ] Render with rounded rect + drop shadow + Acrylic blur
- [ ] Keyboard navigation: up/down arrows, Enter to select, Escape to close
- [ ] Submenu support: `►` arrow, hover to open child menu
- [ ] Separator lines between groups
- [ ] Disabled items rendered grayed out
- [ ] Checked items show checkmark
- [ ] Auto-close when clicking outside
- [ ] Commit: `"desktop: context menu system"`

### 4.2 Desktop Context Menu

**Prompt:** Right-clicking the desktop wallpaper shows a context menu with: View submenu (Large/Medium/Small icons, List view), Sort By submenu (Name, Date, Size, Type), Refresh, separator, New submenu (Folder, Text Document, Shortcut), separator, Paste (if clipboard has files), separator, Display Settings (opens display settings applet), Personalize (opens theme settings). The "New → Folder" action creates a new folder on the desktop. "New → Text Document" creates an empty .txt file. After completing all items, update `docs/architecture/context-menus.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: desktop right-click menu"`.


- [ ] Right-click desktop → context menu:
  - [ ] View ► → Large icons, Medium, Small, List
  - [ ] Sort by ► → Name, Date, Size, Type
  - [ ] Refresh
  - [ ] ─────
  - [ ] New ► → Folder, Text Document, Shortcut
  - [ ] ─────
  - [ ] Paste
  - [ ] ─────
  - [ ] Display settings
  - [ ] Personalize
- [ ] Commit: `"desktop: desktop right-click menu"`

### 4.3 File Context Menu

**Prompt:** Right-clicking a file icon (on desktop or in File Manager) shows: Open (launch associated app via `file_assoc_open`), Open With submenu (list of apps), separator, Cut/Copy/Paste (clipboard integration from Phase 03 §3), Delete (sends to recycle bin via Phase 03 §9), Rename (inline text editing on the icon label), separator, Properties (show file size, type, path, timestamps in a dialog). The context menu items should be context-sensitive — e.g., Paste only shown when clipboard has files. After completing all items, update `docs/architecture/context-menus.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: file context menu"`.


- [ ] Right-click file/icon → context menu:
  - [ ] Open
  - [ ] Open with... ►
  - [ ] ─────
  - [ ] Cut / Copy / Paste
  - [ ] Delete (sends to recycle bin)
  - [ ] Rename
  - [ ] ─────
  - [ ] Properties
- [ ] Commit: `"desktop: file context menu"`

---

## 5. Window Snapping
> *Research: [05_window_snapping_vdesktops.md](research/phase_04_desktop_shell/05_window_snapping_vdesktops.md)*

### 5.1 Keyboard Snapping

**Prompt:** Window snapping allows quick tiling of windows. Win+Left snaps the focused window to the left half of the screen, Win+Right to the right half, Win+Up maximizes, Win+Down restores (if maximized) or minimizes (if restored). Store the window's pre-snap position so restoring returns it to its original size/position. Use the animation engine (Phase 02 §6.1) to smoothly tween the window to its new position over 200ms. After completing all items, create `docs/architecture/window-snapping.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: keyboard window snapping"`.


- [ ] Create `src/kernel/wm_snap.c`
- [ ] Win+Left → snap to left half of screen
- [ ] Win+Right → snap to right half
- [ ] Win+Up → maximize
- [ ] Win+Down → restore (if maximized) / minimize (if restored)
- [ ] Animate snap transitions (200ms slide + resize)
- [ ] Commit: `"desktop: keyboard window snapping"`

### 5.2 Edge Snapping (Mouse)

**Prompt:** When dragging a window, detect if the cursor hits a screen edge: top edge → maximize preview, left/right edge → half-screen preview, corners → quarter-screen preview. Show a semi-transparent overlay rectangle indicating the snap zone before the user releases the mouse button. On release, snap the window to the previewed zone with an animation. If the user drags away from the edge, cancel the preview. This requires tracking the drag state in `wm.c` and checking cursor position against screen edges with a small threshold (e.g., 5px). After completing all items, update `docs/architecture/window-snapping.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: edge snap with preview"`.


- [ ] Drag window to top edge → maximize
- [ ] Drag window to left/right edge → snap to half
- [ ] Drag window to corner → snap to quarter
- [ ] Show snap preview zone (semi-transparent outline) before drop
- [ ] Commit: `"desktop: edge snap with preview"`

### 5.3 Snap Layouts

**Prompt:** Hovering over a window's maximize button shows a popup with visual layout options: 50/50 left-right, 50/50 top-bottom, 66/33 wide-narrow, and 33/33/33 three columns. Each layout is drawn as a small grid preview. Clicking a zone in the layout snaps the current window to that zone, then highlights the remaining zones — the user can click other open windows to fill them. This mimics Windows 11's Snap Layouts feature. The popup is a small floating window positioned near the maximize button. After completing all items, update `docs/architecture/window-snapping.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: snap layouts on maximize hover"`.

> *Research: [06_snap_layouts.md](research/phase_04_desktop_shell/06_snap_layouts.md)*

- [ ] Hover over maximize button → show snap layout popup
- [ ] Layout options:
  - [ ] 50/50 (left/right)
  - [ ] 50/50 (top/bottom)
  - [ ] 66/33 (left wide, right narrow)
  - [ ] 33/33/33 (three columns)
- [ ] Click a zone → snap current window to that zone
- [ ] After snapping, prompt user to fill remaining zones with other windows
- [ ] Commit: `"desktop: snap layouts on maximize hover"`

---

## 6. Virtual Desktops
> *Research: [05_window_snapping_vdesktops.md](research/phase_04_desktop_shell/05_window_snapping_vdesktops.md)*

### 6.1 Virtual Desktop Manager

**Prompt:** Virtual desktops let users organize windows across separate workspaces. Each `struct virtual_desktop` has its own list of windows and a name. `vdesktop_switch(id)` hides all windows on the current desktop (set invisible in WM) and shows all windows on the target desktop. Support up to 8 desktops. Windows are assigned to the desktop they were created on. The taskbar only shows buttons for windows on the active desktop. Keep one desktop always available (can't delete the last one). After completing all items, create `docs/architecture/virtual-desktops.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: virtual desktop manager"`.


- [ ] Create `src/kernel/wm_vdesktop.c`
- [ ] Define `struct virtual_desktop` (windows list, name)
- [ ] Support up to `MAX_VIRTUAL_DESKTOPS` (8)
- [ ] Create default "Desktop 1" at boot
- [ ] Implement `vdesktop_create()` — add new desktop
- [ ] Implement `vdesktop_remove(id)` — close desktop, move windows to previous
- [ ] Implement `vdesktop_switch(id)` — change active desktop (hide/show windows)
- [ ] Commit: `"desktop: virtual desktop manager"`

### 6.2 Keyboard Shortcuts

**Prompt:** Ctrl+Win+Left/Right switches to the previous/next virtual desktop with a smooth slide animation (the current desktop slides out and the new one slides in). Ctrl+Win+D creates a new desktop and switches to it. Ctrl+Win+F4 closes the current desktop and moves its windows to the adjacent one. Register these in the keyboard shortcut manager (§15.1). After completing all items, update `docs/architecture/virtual-desktops.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: virtual desktop keyboard shortcuts"`.


- [ ] Ctrl+Win+Left/Right → switch between desktops
- [ ] Ctrl+Win+D → create new desktop
- [ ] Ctrl+Win+F4 → close current desktop
- [ ] Commit: `"desktop: virtual desktop keyboard shortcuts"`

### 6.3 Win+Tab Overview (Future)

**Prompt:** Win+Tab opens a full-screen overview showing all virtual desktops as horizontal rows with window thumbnails. Users can click to switch desktops, drag windows between desktops, and click "New Desktop" to create one. This is a stretch goal requiring thumbnail rendering (scale down each window's back buffer) and a full-screen overlay mode. After completing all items, update `docs/architecture/virtual-desktops.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: Win+Tab overview"`.


- [ ] *(Stretch)* Win+Tab → overview mode: show all desktops + thumbnails
- [ ] *(Stretch)* Click desktop to switch, drag windows between desktops
- [ ] *(Stretch)* "New Desktop" button at bottom

---

## 7. Notification Center
> *Research: [07_notification_center.md](research/phase_04_desktop_shell/07_notification_center.md)*

### 7.1 Notification History Panel

**Prompt:** The notification center is a panel that slides in from the right edge of the screen when clicking the bell icon in the system tray. It displays past notifications grouped by source, with icon + title + message + timestamp for each entry. Each notification has an X button to dismiss it individually, and a "Clear All" button at the bottom. Store the last 100 notifications in Codex `System\Shell\NotifyHistory`. The panel should have an Acrylic background and respect the "Do Not Disturb" mode from §14.1. After completing all items, create `docs/architecture/notification-center.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: notification center"`.


- [ ] Create `src/desktop/notify_center.c`
- [ ] Slide-in panel from right edge of screen
- [ ] Display past notifications:
  - [ ] Grouped by app/source
  - [ ] Each entry: icon + title + message + timestamp
  - [ ] Dismiss individual notifications (X button)
  - [ ] "Clear all" button at bottom
- [ ] Store last 100 notifications in Codex `System\Shell\NotifyHistory`
- [ ] Open: click 🔔 in system tray
- [ ] Close: click outside or press Escape
- [ ] Commit: `"desktop: notification center"`

---

## 8. Drag and Drop
> *Research: [08_drag_and_drop.md](research/phase_04_desktop_shell/08_drag_and_drop.md)*

### 8.1 Drag-and-Drop System

**Prompt:** Drag-and-drop enables moving files between locations and inserting content between apps. The drag state tracks: active flag, data format (FILE, TEXT), data payload, cursor position, drag icon (semi-transparent thumbnail), and source window. `drag_begin` is triggered when a mouse move exceeds a threshold while a button is held. `drag_update` moves the drag icon each frame. `drag_drop` delivers the data to whatever window/control is under the cursor on release. The WM routes drop events to the target window's event handler. After completing all items, create `docs/architecture/drag-drop.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: drag-and-drop system"`.


- [ ] Define `drag_state_t` struct (active, format, data, size, cursor_x/y, drag_icon, source_window)
- [ ] Create `src/desktop/drag.c`
- [ ] Implement `drag_begin(fmt, data, size, icon)` — start drag operation
- [ ] Implement `drag_update(mx, my)` — called on mouse move, update icon position
- [ ] Implement `drag_drop(target)` — called on mouse release, deliver data to target
- [ ] Implement `drag_cancel()` — Escape to cancel
- [ ] Commit: `"desktop: drag-and-drop system"`

### 8.2 Visual Feedback

**Prompt:** During a drag operation, render a semi-transparent icon (the file icon or a text selection preview) following the cursor at a slight offset. When hovering over a valid drop target (a window that accepts the drag format), highlight its border with the accent color. When hovering over an invalid target, show the `CURSOR_FORBIDDEN` cursor (from Phase 02 §5.1). This visual feedback is essential for usability. After completing all items, update `docs/architecture/drag-drop.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: drag-and-drop visual feedback"`.


- [ ] Draw semi-transparent drag icon following cursor
- [ ] Highlight valid drop targets when hovering
- [ ] Show "forbidden" cursor over invalid drop targets
- [ ] Commit: `"desktop: drag-and-drop visual feedback"`

### 8.3 Drag Types

**Prompt:** File drag: start from a File Manager icon or desktop icon, drop onto another folder (move), onto Notepad (open), or onto the desktop (move). Hold Ctrl during drag to copy instead of move. Text drag: select text in a text control, drag the selection to another text field to insert it. Stretch goal: drag taskbar buttons to reorder pinned app positions. Each drag type uses a different format tag so drop targets can check compatibility. After completing all items, update `docs/architecture/drag-drop.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: file and text drag support"`.


- [ ] Files: File Manager → Desktop → Notepad (move/copy/open)
- [ ] Text: select text, drag to another text field
- [ ] *(Stretch)* Taskbar: reorder pinned apps via drag
- [ ] Commit: `"desktop: file and text drag support"`

---

## 9. Quick Settings Panel
> *Research: [09_quick_settings.md](research/phase_04_desktop_shell/09_quick_settings.md)*

### 9.1 Quick Settings Popup

**Prompt:** Quick Settings is a popup panel opened by clicking the system tray area or pressing Win+A. Layout: a 3×2 grid of toggle buttons (WiFi/Network, Bluetooth placeholder, Airplane Mode, Night Light from §13, Do Not Disturb from §14, Cast placeholder) + vertical sliders for volume and brightness. Each toggle reads/writes a Codex value and triggers the corresponding service. The panel has rounded corners, Acrylic background, and a drop shadow. An "Edit" gear icon opens the full Settings app. Volume slider modifies `System\Sound\Volume`, brightness modifies `System\Display\Brightness`. After completing all items, create `docs/architecture/quick-settings.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: quick settings panel"`.


- [ ] Create `src/desktop/quick_settings.c`
- [ ] Open: click system tray area or Win+A
- [ ] Layout: 3×2 grid of toggle buttons + volume/brightness sliders
- [ ] Toggle buttons (each flips a Codex value + triggers a service):
  - [ ] WiFi / Network (enable/disable network)
  - [ ] Bluetooth (placeholder — future)
  - [ ] Airplane Mode (disable all radios)
  - [ ] Night Light (toggle blue light filter)
  - [ ] Do Not Disturb (suppress notifications)
  - [ ] *(Stretch)* Cast / Screen share
- [ ] Volume slider → reads/writes Codex `System\Sound\Volume`
- [ ] Brightness slider → reads/writes Codex `System\Display\Brightness`
- [ ] [Edit ⚙] link → open full Settings app
- [ ] Rounded corners, Acrylic background, drop shadow
- [ ] Commit: `"desktop: quick settings panel"`

---

## 10. Boot Splash Screen
> *Research: [10_boot_splash.md](research/phase_04_desktop_shell/10_boot_splash.md)*

### 10.1 Graphical Boot Splash

**Prompt:** The boot splash replaces the text-mode boot log with a graphical screen showing the OS logo and a smooth progress bar. `boot_splash_init` renders the logo (from an embedded bitmap or decoded from initrd) centered on a gradient background. Call `boot_splash_progress(pct)` at key milestones during kernel init: 10% after PMM/VMM, 20% after drivers, 40% after filesystem mount, 60% after network init, 80% after desktop surface creation, 100% ready. `boot_splash_finish` fades out to the desktop. The logo and progress bar render directly to the framebuffer without needing the full compositor. After completing all items, create `docs/architecture/boot-splash.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: graphical boot splash screen"`.


- [ ] Create `src/kernel/boot_splash.c`
- [ ] Implement `boot_splash_init()` — draw logo + progress bar on framebuffer
- [ ] Implement `boot_splash_progress(pct)` — update progress bar (0–100%)
- [ ] Implement `boot_splash_status(msg)` — display status text ("Loading drivers...")
- [ ] Implement `boot_splash_finish()` — fade out, transition to desktop
- [ ] Call progress updates during kernel init:
  - [ ] 10% — Memory manager
  - [ ] 20% — Drivers
  - [ ] 40% — Filesystem
  - [ ] 60% — Network
  - [ ] 80% — Desktop
  - [ ] 100% — Ready
- [ ] Design: centered OS logo, gradient background, smooth progress bar
- [ ] Commit: `"kernel: graphical boot splash screen"`

### 10.2 Boot Menu (F8)

**Prompt:** This stretch goal adds a boot menu accessible by holding F8 during the boot splash. Display options: Normal Boot (default), Safe Mode (load minimal drivers, no network, no desktop effects), Recovery Console (text-mode shell only, no GUI), Last Known Good (restore previous Codex configuration from a backup). Check for F8 keypress via keyboard polling during `boot_splash_init`. After completing all items, update `docs/architecture/boot-splash.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: boot menu"`.


- [ ] *(Stretch)* Hold F8 during boot → show boot options:
  - [ ] Normal boot (default)
  - [ ] Safe mode (minimal drivers, no network)
  - [ ] Recovery console (text-mode shell only)
  - [ ] Last known good (restore previous Codex)
- [ ] Commit: `"kernel: boot menu"`

---

## 11. Screensaver & Lock Screen
> *Research: [11_screensaver_lockscreen.md](research/phase_04_desktop_shell/11_screensaver_lockscreen.md)*

### 11.1 Screensaver System

**Prompt:** The screensaver activates after a configurable idle timeout by tracking the last mouse/keyboard event timestamp. The screensaver manager calls a registered screensaver function with messages: SCR_INIT (allocate resources), SCR_FRAME (render one frame to the provided surface), SCR_CLOSE (free resources). Any mouse move or keypress dismisses the screensaver. Codex settings: `System\Screensaver\IdleTimeout` (seconds), `System\Screensaver\Type` (enum: blank, starfield, matrix, logo, clock). Multiple built-in screensavers share this API. After completing all items, create `docs/architecture/screensaver.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: screensaver system"`.


- [ ] Create `src/desktop/screensaver.c`
- [ ] Define screensaver API: `scr_entry_fn(msg, surface)` with SCR_INIT/SCR_FRAME/SCR_CLOSE
- [ ] Implement idle detection (track last mouse/keyboard input time)
- [ ] Trigger screensaver after idle timeout (configurable: 1, 2, 5, 10, 15, 30 min, never)
- [ ] Dismiss on any mouse move or keypress
- [ ] Codex: `System\Screensaver\IdleTimeout`, `System\Screensaver\Type`
- [ ] Commit: `"desktop: screensaver system"`

### 11.2 Built-In Screensavers

**Prompt:** Implement 5 screensavers using the API from §11.1: Blank (solid black — simplest), Starfield (random white dots moving toward the viewer with 3D perspective using basic z-divide math), Matrix (columns of falling green characters using the mono font), Bouncing Logo (the OS logo bitmap bouncing off screen edges, changing direction on collision), and Clock (a large digital clock with seconds, floating slowly across the screen). Start with Blank and Starfield as they're simplest. After completing all items, update `docs/architecture/screensaver.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: built-in screensavers"`.


- [ ] **Blank** — solid black screen (power saving)
- [ ] **Starfield** — stars moving toward viewer (3D perspective)
- [ ] **Matrix** — falling green characters
- [ ] **Bouncing Logo** — OS logo bouncing off screen edges
- [ ] **Clock** — large floating digital clock
- [ ] Commit: `"desktop: built-in screensavers"`

### 11.3 Lock Screen

**Prompt:** The lock screen overlays the entire display with a blurred wallpaper background (using `gfx_blur` from Phase 02 §1.4). Show: large clock and date (centered, using FONT_UI at 48px), user avatar (or default silhouette icon), username, and password input field. The Unlock button (or Enter key) checks the entered password against the user account hash (Phase 03 §11.1, using Argon2 from Phase 01 §11.3). Win+L triggers lock immediately. The Codex setting `System\Screensaver\RequirePassword` controls whether the screensaver auto-locks. After completing all items, update `docs/architecture/screensaver.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: lock screen"`.


- [ ] Create `src/desktop/lockscreen.c`
- [ ] Blurred wallpaper background via `gfx_blur()`
- [ ] Display: large clock, date, user avatar + name
- [ ] Password input field
- [ ] [Unlock →] button → verify password, return to desktop
- [ ] Option: "Require password on wake" (Codex `System\Screensaver\RequirePassword`)
- [ ] Lock manually: Win+L shortcut or Start menu → Lock
- [ ] Lock automatically: after screensaver activates (if password required)
- [ ] Commit: `"desktop: lock screen"`

---

## 12. Desktop Widgets
> *Research: [12_desktop_widgets.md](research/phase_04_desktop_shell/12_desktop_widgets.md)*

### 12.1 Widget Framework

**Prompt:** Desktop widgets are small floating panels rendered on the desktop surface (above wallpaper, below windows). The widget API: `widget_fn(msg, surface, ctx)` with messages WGT_INIT (create), WGT_RENDER (draw to 200×N surface), WGT_TICK (update data), WGT_CLOSE (cleanup). The widget manager maintains a list of active widgets, their positions on screen, and updates them periodically. Widgets are draggable for repositioning. They render with a semi-transparent background. After completing all items, create `docs/architecture/widgets.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: widget framework"`.


- [ ] Create `src/desktop/widgets.c`
- [ ] Define widget API: `widget_fn(msg, surface, ctx)` with WGT_INIT/WGT_RENDER/WGT_TICK/WGT_CLOSE
- [ ] Widget manager: load, position, update widgets on desktop
- [ ] Draggable widget positioning
- [ ] Transparent/floating widget rendering
- [ ] Commit: `"desktop: widget framework"`

### 12.2 Built-In Widgets

**Prompt:** Implement 5 widgets using the framework from §12.1: Clock (analog or digital, updates every second using Phase 03 §4.1 time API), CPU Meter (bar chart showing scheduler busy percentage, read from a `sched_cpu_usage()` function), RAM Monitor (used/free/total bars from PMM stats), Calendar (month grid with today highlighted, using time API), Quick Notes (editable sticky note text that persists to Codex `User\Default\Widgets\Notes`). Start with Clock and RAM Monitor as they're simplest. After completing all items, update `docs/architecture/widgets.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: built-in widgets (clock, CPU, RAM, calendar, notes)"`.


- [ ] **Clock** (200×100) — analog or digital clock, updates 1/sec
- [ ] **CPU Meter** (200×120) — usage bar graph, updates 1/sec
- [ ] **RAM Monitor** (200×100) — used/free memory bars, updates 5/sec
- [ ] **Calendar** (200×200) — month view, today highlighted
- [ ] **Quick Notes** (200×150) — sticky note text, persist to Codex
- [ ] Commit: `"desktop: built-in widgets (clock, CPU, RAM, calendar, notes)"`

---

## 13. Night Light
> *Research: [14_night_light.md](research/phase_04_desktop_shell/14_night_light.md)*

### 13.1 Blue Light Filter

**Prompt:** Night light reduces the blue channel of the framebuffer to reduce eye strain at night. `nightlight_apply(surface, intensity)` multiplies each pixel's blue channel by `(100 - intensity) / 100` where intensity ranges 0-100. Apply this transform in the compositor's final blit step, after window compositing but before framebuffer swap. The gradual transition ramps intensity over 30 minutes for a natural feel. Schedule: auto-enable at `NightLightStart` time and disable at `NightLightEnd` using the time system (Phase 03 §4.1). Manual toggle via Quick Settings (§9.1). Codex keys: `System\Display\NightLight` (BOOL), `NightLightIntensity` (INT32), `NightLightStart`/`NightLightEnd` (strings, e.g., "21:00"/"07:00"). After completing all items, create `docs/architecture/night-light.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"display: night light / blue light filter"`.


- [ ] Create `src/kernel/display/nightlight.c`
- [ ] Implement `nightlight_apply(surface, intensity)` — reduce blue channel up to 50%
- [ ] Apply per-pixel color transform in compositor's final blit
- [ ] Gradual transition over 30 minutes (sunset → full warm)
- [ ] Schedule: auto on/off based on time (e.g., 9 PM → 7 AM)
- [ ] Manual toggle via Quick Settings or Codex
- [ ] Codex: `System\Display\NightLight`, `NightLightIntensity` (0–100), `NightLightStart`, `NightLightEnd`
- [ ] Commit: `"display: night light / blue light filter"`

---

## 14. Focus / Do Not Disturb Mode
> *Research: [15_focus_mode.md](research/phase_04_desktop_shell/15_focus_mode.md)*

### 14.1 Focus Assist

**Prompt:** Focus Assist suppresses notifications during focused work. Three modes: Off (all notifications show), Priority Only (only alarms/reminders from a priority list), Do Not Disturb (all notifications silenced, saved to history for later review). Toggle via Quick Settings (§9.1) or Win+N. Auto-activate when a fullscreen app is running or during scheduled quiet hours. The system tray notification bell shows a badge with the count of suppressed notifications. When DND ends, show a summary toast: "You missed N notifications". Codex keys: `System\Shell\FocusMode` (enum), `FocusScheduleStart`/`FocusScheduleEnd` (time strings). After completing all items, create `docs/architecture/focus-mode.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: focus / do not disturb mode"`.


- [ ] Create `src/desktop/focus_mode.c`
- [ ] Define modes: Off, Priority Only, Do Not Disturb
- [ ] **Off** — all notifications shown normally
- [ ] **Priority only** — only alarms/reminders shown
- [ ] **Do Not Disturb** — all notifications silenced, saved to history
- [ ] Toggle via Quick Settings toggle or Win+N
- [ ] Auto-activate during: full-screen apps, scheduled hours
- [ ] Notification badge on system tray shows suppressed count
- [ ] When DND ends, show summary: "You missed N notifications"
- [ ] Codex: `System\Shell\FocusMode`, `FocusScheduleStart`, `FocusScheduleEnd`
- [ ] Commit: `"desktop: focus / do not disturb mode"`

---

## 15. Agent-Recommended Additions

> Items not in the research files but important for a complete desktop shell.

### 15.1 Keyboard Shortcut Manager

**Prompt:** Centralize all system-wide keyboard shortcuts in a single hotkey table. Each entry maps a key combination (modifier flags + scancode) to an action callback. Register all known shortcuts here: Win (Start Menu), Win+E (File Manager), Win+L (Lock), Win+D (Show Desktop), Win+R (Run Dialog), Alt+Tab (Task Switcher), Alt+F4 (Close Window), Print Screen (Screenshot). The keyboard driver checks this table before dispatching key events to windows. Allow user-defined shortcuts via Codex `System\Shell\Hotkeys\`. After completing all items, create `docs/architecture/keyboard-shortcuts.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: system-wide keyboard shortcut manager"`.


- [ ] Define system-wide hotkey table (key combo → action callback)
- [ ] Register all shortcuts in one place:
  - [ ] Win → toggle Start Menu
  - [ ] Win+E → open File Manager
  - [ ] Win+L → lock screen
  - [ ] Win+D → show desktop (minimize all)
  - [ ] Win+R → run dialog
  - [ ] Alt+Tab → task switcher
  - [ ] Alt+F4 → close focused window
  - [ ] Print Screen → screenshot
- [ ] Allow user-defined shortcuts via Codex `System\Shell\Hotkeys\`
- [ ] Commit: `"desktop: system-wide keyboard shortcut manager"`

### 15.2 Alt+Tab Task Switcher

**Prompt:** Alt+Tab opens a centered overlay showing thumbnails of all open windows. Continue pressing Tab while holding Alt to cycle the selection highlight through windows. Release Alt to focus the selected window. Each thumbnail shows a scaled-down rendering of the window's back buffer (use `image_scale` from Phase 02 §3.2), its title, and its icon. The overlay has an Acrylic background panel. This is one of the most-used shortcuts in any desktop OS, so it must feel responsive and look polished. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: Alt+Tab task switcher"`.


- [ ] Alt+Tab → overlay showing all window thumbnails
- [ ] Keep pressing Tab while holding Alt → cycle through windows
- [ ] Release Alt → focus selected window
- [ ] Show window title + icon below each thumbnail
- [ ] Acrylic background panel
- [ ] Commit: `"desktop: Alt+Tab task switcher"`

### 15.3 Run Dialog (Win+R)

**Prompt:** Win+R opens a small dialog box with a single text input field labeled "Open:". The user types a command or path and presses Enter to execute it. The dialog searches PATH directories (Phase 01 §7.1 environment variables) for the command. Keep a history of recent entries in Codex `User\Default\Shell\RunHistory` (last 20 entries) accessible via up/down arrows. Auto-complete suggestions from PATH directories as the user types. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: Win+R run dialog"`.


- [ ] Win+R → small dialog box with text field
- [ ] Type command/path → execute
- [ ] History of recent entries (stored in Codex)
- [ ] Auto-complete from PATH directories
- [ ] Commit: `"desktop: Win+R run dialog"`

### 15.4 Desktop Icons

**Prompt:** Desktop icons are rendered on the desktop surface in a grid layout. Default icons: "This PC" (opens File Manager at C:\), "Recycle Bin" (shows trash contents, uses empty/full icon from Phase 03 §9.3), and user-created shortcuts (.lnk files from Phase 03 §7). Single-click selects (highlight with semi-transparent rect), double-click opens. Icons auto-arrange on a grid with configurable spacing. Each icon has a label below it rendered with a text shadow (dark outline over bright wallpaper) for readability. After completing all items, create `docs/architecture/desktop-icons.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: desktop icon grid"`.


- [ ] Render icons on desktop surface (grid-aligned)
- [ ] Default icons: This PC, Recycle Bin, user's shortcuts
- [ ] Single-click selects, double-click opens
- [ ] Drag to reorder, auto-arrange option
- [ ] Label text below icon (with text shadow for readability over wallpaper)
- [ ] Commit: `"desktop: desktop icon grid"`

### 15.5 Window Minimize/Restore All

**Prompt:** Win+M minimizes all windows. Win+Shift+M restores all previously-minimized windows to their pre-minimize positions. Win+D toggles between minimize-all and restore-all (show desktop toggle). These require tracking which windows were visible before the minimize-all operation. Store a snapshot of visible window IDs in a global array when minimizing all, then restore from that array when toggling back. Register these shortcuts in the hotkey manager (§15.1). After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: minimize/restore all windows"`.


- [ ] Win+M → minimize all windows
- [ ] Win+Shift+M → restore all minimized windows
- [ ] Win+D → toggle show desktop (minimize all / restore all)
- [ ] Commit: `"desktop: minimize/restore all windows"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1.1 Taskbar Window List | Core UX — switch between open apps |
| 🔴 P0 | 2. Start Menu | App launcher — most used UI element |
| 🔴 P0 | 4. Context Menus | Essential interaction — right-click everywhere |
| 🔴 P0 | 15.4 Desktop Icons | Visual desktop experience |
| 🟠 P1 | 3. System Tray & Notifications | Status feedback, toast notifications |
| 🟠 P1 | 5.1–5.2 Window Snapping | Productivity — snap to halves/quarters |
| 🟠 P1 | 15.1 Keyboard Shortcuts | Essential OS navigation |
| 🟠 P1 | 15.2 Alt+Tab | Task switching |
| 🟡 P2 | 10.1 Boot Splash | Visual polish — first thing users see |
| 🟡 P2 | 8. Drag and Drop | File management UX |
| 🟡 P2 | 9. Quick Settings | System toggles |
| 🟡 P2 | 13. Night Light | Eye comfort |
| 🟢 P3 | 5.3 Snap Layouts | Advanced window management |
| 🟢 P3 | 6. Virtual Desktops | Multi-workspace |
| 🟢 P3 | 7. Notification Center | History panel |
| 🟢 P3 | 14. Focus/DND Mode | Notification control |
| 🟢 P3 | 1.3 Window Peek | Polish |
| 🔵 P4 | 11. Screensaver & Lock Screen | Security + visual flair |
| 🔵 P4 | 12. Desktop Widgets | Optional enhancements |
| 🔵 P4 | 15.3 Run Dialog | Power user feature |
| 🔵 P4 | 10.2 Boot Menu | Recovery/safe mode |
