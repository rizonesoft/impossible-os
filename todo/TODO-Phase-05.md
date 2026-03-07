# Phase 05 — Desktop Shell

> **Goal:** Build a complete, Windows 11-quality desktop shell experience with taskbar
> window management, start menu, context menus, notification system, window snapping,
> virtual desktops, drag-and-drop, and polished user-facing features like boot splash,
> screensavers, night light, and quick settings.

---

## 1. Taskbar Window List
> *Research: [01_taskbar_window_list.md](research/phase_04_desktop_shell/01_taskbar_window_list.md)*

### 1.1 Window Buttons on Taskbar

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

- [ ] Right-click window button → context menu:
  - [ ] "Close" → close window
  - [ ] "Maximize" / "Restore" → toggle maximize
  - [ ] "Minimize"
  - [ ] *(Stretch)* "Move to Desktop ►" → move to virtual desktop
- [ ] Commit: `"desktop: taskbar button context menu"`

### 1.3 Window Peek (Aero Peek)
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

- [ ] Load pinned apps from Codex `User\{name}\Shell\PinnedApps`
- [ ] Load recent files from Codex `User\{name}\Shell\RecentFiles`
- [ ] Scan installed apps from `C:\Impossible\Bin\` and `C:\Programs\`
- [ ] Display app icons from icon store
- [ ] Commit: `"desktop: start menu data loading"`

### 2.3 Start Menu Interaction

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

- [ ] Create `src/desktop/systray.c`
- [ ] Draw tray icon area to the left of the clock:
  - [ ] 🔊 Volume icon — click → volume slider popup
  - [ ] 🌐 Network icon — click → network status popup (IP, connected/disconnected)
  - [ ] 🔔 Notification bell — click → open notification center
- [ ] Update icons dynamically (e.g., network disconnected → different icon)
- [ ] Commit: `"desktop: system tray icons"`

### 3.2 Notification Toasts

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

- [ ] Create `src/kernel/wm_snap.c`
- [ ] Win+Left → snap to left half of screen
- [ ] Win+Right → snap to right half
- [ ] Win+Up → maximize
- [ ] Win+Down → restore (if maximized) / minimize (if restored)
- [ ] Animate snap transitions (200ms slide + resize)
- [ ] Commit: `"desktop: keyboard window snapping"`

### 5.2 Edge Snapping (Mouse)

- [ ] Drag window to top edge → maximize
- [ ] Drag window to left/right edge → snap to half
- [ ] Drag window to corner → snap to quarter
- [ ] Show snap preview zone (semi-transparent outline) before drop
- [ ] Commit: `"desktop: edge snap with preview"`

### 5.3 Snap Layouts
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

- [ ] Create `src/kernel/wm_vdesktop.c`
- [ ] Define `struct virtual_desktop` (windows list, name)
- [ ] Support up to `MAX_VIRTUAL_DESKTOPS` (8)
- [ ] Create default "Desktop 1" at boot
- [ ] Implement `vdesktop_create()` — add new desktop
- [ ] Implement `vdesktop_remove(id)` — close desktop, move windows to previous
- [ ] Implement `vdesktop_switch(id)` — change active desktop (hide/show windows)
- [ ] Commit: `"desktop: virtual desktop manager"`

### 6.2 Keyboard Shortcuts

- [ ] Ctrl+Win+Left/Right → switch between desktops
- [ ] Ctrl+Win+D → create new desktop
- [ ] Ctrl+Win+F4 → close current desktop
- [ ] Commit: `"desktop: virtual desktop keyboard shortcuts"`

### 6.3 Win+Tab Overview (Future)

- [ ] *(Stretch)* Win+Tab → overview mode: show all desktops + thumbnails
- [ ] *(Stretch)* Click desktop to switch, drag windows between desktops
- [ ] *(Stretch)* "New Desktop" button at bottom

---

## 7. Notification Center
> *Research: [07_notification_center.md](research/phase_04_desktop_shell/07_notification_center.md)*

### 7.1 Notification History Panel

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

- [ ] Define `drag_state_t` struct (active, format, data, size, cursor_x/y, drag_icon, source_window)
- [ ] Create `src/desktop/drag.c`
- [ ] Implement `drag_begin(fmt, data, size, icon)` — start drag operation
- [ ] Implement `drag_update(mx, my)` — called on mouse move, update icon position
- [ ] Implement `drag_drop(target)` — called on mouse release, deliver data to target
- [ ] Implement `drag_cancel()` — Escape to cancel
- [ ] Commit: `"desktop: drag-and-drop system"`

### 8.2 Visual Feedback

- [ ] Draw semi-transparent drag icon following cursor
- [ ] Highlight valid drop targets when hovering
- [ ] Show "forbidden" cursor over invalid drop targets
- [ ] Commit: `"desktop: drag-and-drop visual feedback"`

### 8.3 Drag Types

- [ ] Files: File Manager → Desktop → Notepad (move/copy/open)
- [ ] Text: select text, drag to another text field
- [ ] *(Stretch)* Taskbar: reorder pinned apps via drag
- [ ] Commit: `"desktop: file and text drag support"`

---

## 9. Quick Settings Panel
> *Research: [09_quick_settings.md](research/phase_04_desktop_shell/09_quick_settings.md)*

### 9.1 Quick Settings Popup

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

- [ ] Create `src/desktop/screensaver.c`
- [ ] Define screensaver API: `scr_entry_fn(msg, surface)` with SCR_INIT/SCR_FRAME/SCR_CLOSE
- [ ] Implement idle detection (track last mouse/keyboard input time)
- [ ] Trigger screensaver after idle timeout (configurable: 1, 2, 5, 10, 15, 30 min, never)
- [ ] Dismiss on any mouse move or keypress
- [ ] Codex: `System\Screensaver\IdleTimeout`, `System\Screensaver\Type`
- [ ] Commit: `"desktop: screensaver system"`

### 11.2 Built-In Screensavers

- [ ] **Blank** — solid black screen (power saving)
- [ ] **Starfield** — stars moving toward viewer (3D perspective)
- [ ] **Matrix** — falling green characters
- [ ] **Bouncing Logo** — OS logo bouncing off screen edges
- [ ] **Clock** — large floating digital clock
- [ ] Commit: `"desktop: built-in screensavers"`

### 11.3 Lock Screen

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

- [ ] Create `src/desktop/widgets.c`
- [ ] Define widget API: `widget_fn(msg, surface, ctx)` with WGT_INIT/WGT_RENDER/WGT_TICK/WGT_CLOSE
- [ ] Widget manager: load, position, update widgets on desktop
- [ ] Draggable widget positioning
- [ ] Transparent/floating widget rendering
- [ ] Commit: `"desktop: widget framework"`

### 12.2 Built-In Widgets

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

- [ ] Alt+Tab → overlay showing all window thumbnails
- [ ] Keep pressing Tab while holding Alt → cycle through windows
- [ ] Release Alt → focus selected window
- [ ] Show window title + icon below each thumbnail
- [ ] Acrylic background panel
- [ ] Commit: `"desktop: Alt+Tab task switcher"`

### 15.3 Run Dialog (Win+R)

- [ ] Win+R → small dialog box with text field
- [ ] Type command/path → execute
- [ ] History of recent entries (stored in Codex)
- [ ] Auto-complete from PATH directories
- [ ] Commit: `"desktop: Win+R run dialog"`

### 15.4 Desktop Icons

- [ ] Render icons on desktop surface (grid-aligned)
- [ ] Default icons: This PC, Recycle Bin, user's shortcuts
- [ ] Single-click selects, double-click opens
- [ ] Drag to reorder, auto-arrange option
- [ ] Label text below icon (with text shadow for readability over wallpaper)
- [ ] Commit: `"desktop: desktop icon grid"`

### 15.5 Window Minimize/Restore All

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
