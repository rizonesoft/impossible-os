# P0202 — Theme, Desktop Polish & UI Shell Features

> **Goal:** Centralize all UI colors, animations, DPI scaling, notifications,
> screenshot system, wallpaper engine, context menus, and tooltips into a
> cohesive, Windows 11-quality visual experience.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!NOTE]
> **Widget & Control Library** (Button, Label, TextBox, ScrollBar, Checkbox, Radio,
> Dropdown, Slider, ProgressBar, Tabs, ListView, TreeView, Toolbar, MenuBar,
> StatusBar, GroupBox, Tooltip, Dialog system) → **see [TODO-130-Controls.md](TODO-130-Controls.md)**.

---

## 1. Theme System

> **Foundation:** every visual element references the theme instead of hardcoded hex colors.

**Prompt:** Centralize all UI colors into a `theme_t` struct with named fields: `background`, `foreground`, `accent`, `border`, `shadow`, `titlebar_active`, `titlebar_inactive`, `button_bg`, `button_hover`, `button_pressed`, `selection`, `error`, `warning`, `success`. Load theme colors from the Registry under `HKCU\Software\Impossible\Theme\*`. Provide two built-in presets: Dark (dark backgrounds, light text, `#0078D4` blue accent) and Light (light backgrounds, dark text). Every drawing function in `desktop.c`, `wm.c`, and `controls.c` must reference `theme_get()->field` instead of hardcoded hex colors. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: theme system"`. Add notes directly in this TODO section covering the theme struct layout, Registry key names, and hot-reload mechanism.

> **Beats:** Linux GTK themes require restarting apps. Windows theme changes apply live to all windows. Impossible OS can match Windows — single `theme_changed()` broadcast to all windows forces immediate redraw.

- [ ] Define `theme_t` struct with all UI colors (bg, fg, accent, border, shadow, titlebar states, button states, selection, error, warning, success)
- [ ] Create `include/desktop/theme.h` and `src/desktop/theme.c`
- [ ] `theme_get()` — return pointer to current theme (thread-safe singleton)
- [ ] Load theme from Registry (`HKCU\Software\Impossible\Theme\AccentColor`, `Mode`, etc.)
- [ ] Built-in Dark mode preset (default): `#1C1C1C` bg, `#FFFFFF` fg, `#0078D4` accent
- [ ] Built-in Light mode preset: `#F3F3F3` bg, `#000000` fg, `#0078D4` accent
- [ ] All drawing functions in `desktop.c`, `wm.c`, `controls.c` reference `theme_get()->field`
- [ ] Apply accent color to: focused controls, active title bars, selection highlights, progress bars
- [ ] Hot-reload: `theme_reload()` → broadcast `WM_THEME_CHANGED` → all windows redraw
- [ ] Commit: `"desktop: theme system"`

---

## 2. Start Menu

**Prompt:** The Start Menu opens on Win key or Start button click, slides up from the taskbar with an Acrylic blur background. Top section: search box. Middle: pinned app grid (3 columns, app icon + name, click to launch). Bottom: All Apps button, Power button (fly-out: Shut Down, Restart, Sleep, Lock). The menu closes on outside click or Escape. Pinned apps are stored in Registry `HKCU\Software\Impossible\Shell\PinnedApps` as a comma-separated list of app names. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: Start Menu"`. Add notes directly in this TODO section covering menu layout, animation, Registry storage, and app launch mechanism.

> **Beats:** Windows 11 Start Menu has controversial recommended items and ads. Impossible OS Start Menu is clean — only pinned apps, no telemetry, no suggestions.

- [ ] Create `src/desktop/startmenu.c` and `include/desktop/startmenu.h`
- [ ] Start button in taskbar: Win key or click → open/close Start Menu
- [ ] Acrylic blur background (uses `gfx_acrylic()` from §1.4)
- [ ] Slide-up animation from taskbar (24px/frame, ease-out)
- [ ] Search box at top (accepts keyboard input, filters pinned apps)
- [ ] Pinned app grid: 3 columns, 48px icon + app name below
- [ ] Launch app on click: `exec_process(app_path)`
- [ ] All Apps button → list all executables in `C:\Programs\`
- [ ] Power button → fly-out submenu: Shut Down, Restart, Sleep, Lock (calls TODO-100 §4)
- [ ] Close: outside click, Escape key, Win key again
- [ ] Pinned apps stored in Registry: `HKCU\Software\Impossible\Shell\PinnedApps`
- [ ] Commit: `"desktop: Start Menu"`

---

## 3. Animations & Transitions

**Prompt:** Fluid animations make the desktop feel responsive and alive. The animation system drives property changes over time (position, opacity, size) using an easing function. `anim_create(target_ptr, from, to, duration_ms, easing)` registers a running animation. The compositor's main loop calls `anim_tick(delta_ms)` every frame to advance all running animations. Animations are used for: window open/close (scale+fade), Start Menu slide-up, notification slide-in/out, tooltip fade. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: animation system"`. Add notes directly in this TODO section covering the animation list, easing functions, and compositor integration.

> **Beats:** Linux desktop animations require Clutter/libmutter/wlroots. Windows uses DWM animations in the compositor. Impossible OS: in-kernel animation tick — no separate compositor process needed.

- [ ] Create `src/desktop/anim.c` and `include/desktop/anim.h`
- [ ] Define `anim_t` struct: target pointer, from, to, current, duration_ms, elapsed_ms, easing type
- [ ] Implement `anim_create(target_ptr, from, to, duration_ms, easing)` — add to animation list
- [ ] Implement `anim_tick(delta_ms)` — advance all animations, write to target pointer, remove completed
- [ ] Easing functions: Linear, EaseIn, EaseOut, EaseInOut (integer cubic approximation)
- [ ] Wire to compositor loop: call `anim_tick()` every frame with VSync delta
- [ ] Apply animations:
  - [ ] Window open: scale from 80% + fade in (150ms, EaseOut)
  - [ ] Window close: scale to 90% + fade out (100ms, EaseIn)
  - [ ] Start Menu open: slide up 48px (200ms, EaseOut)
  - [ ] Start Menu close: slide down (150ms, EaseIn)
  - [ ] Notification slide-in from right (250ms, EaseOut)
  - [ ] Tooltip fade-in (100ms, EaseOut)
- [ ] Commit: `"desktop: animation system"`

---

## 4. DPI Scaling

**Prompt:** Modern monitors support high DPI (2×, 1.25×, 1.5×). All UI sizes (fonts, icons, controls, window chrome) must scale by the active DPI factor. The DPI scale factor is stored in Registry `HKCU\Software\Impossible\Display\ScaleFactor` as an integer percentage (100, 125, 150, 200). All drawing code uses `DPI_SCALE(x)` macro (`x * dpi_pct / 100`). TrueType font pixel sizes scale automatically. Icons request the nearest-available size from the IRES store. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: DPI scaling"`. Add notes directly in this TODO section.

> **Beats:** Linux DPI scaling is fractional and inconsistent across toolkits (GTK vs Qt vs X11). Windows DPI scaling is per-monitor, per-process. Impossible OS: simple global DPI factor — consistent across all UI elements.

- [ ] Read DPI scale factor from Registry `HKCU\Software\Impossible\Display\ScaleFactor` (default: 100%)
- [ ] Implement `DPI_SCALE(x)` macro: `((x) * g_dpi_pct / 100)`
- [ ] Apply DPI scaling to: font pixel sizes, icon request sizes, window chrome sizes, control heights
- [ ] VBE resolution detection: if resolution ≥ 2560×1440 → auto-set to 150%; ≥ 3840×2160 → 200%
- [ ] Display Control Panel applet: scale factor dropdown (100%, 125%, 150%, 200%)
- [ ] Live change: update Registry + broadcast `WM_DPI_CHANGED` → all windows re-layout
- [ ] Commit: `"desktop: DPI scaling"`

---

## 5. Notification System (Toast)

**Prompt:** Toast notifications slide in from the bottom-right corner, display for 4 seconds, then fade out. The kernel notification API: `notify_send(title, body, icon, timeout_ms)`. Apps use a syscall wrapper. Notifications stack — if one is shown, the next queues. The compositor renders the notification overlay above all windows. Style: Acrylic background, rounded corners, app icon, bold title, body text, optional action button (e.g., "Open"). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: notification system (toast)"`. Add notes directly in this TODO section covering the notification queue, rendering, and syscall.

> **Beats:** Linux libnotify uses a separate notification daemon process. Windows WinRT toast notifications require COM. Impossible OS: in-kernel notification queue, no daemon needed.

- [ ] Create `src/desktop/notify.c` and `include/desktop/notify.h`
- [ ] Define `notification_t` struct: title, body, icon_id, timeout_ms, state (queued/visible/fading)
- [ ] Implement `notify_send(title, body, icon, timeout_ms)` — add to queue
- [ ] Notification queue: show one at a time, stack if multiple pending
- [ ] Render: bottom-right 320×72px panel, Acrylic background, rounded corners, icon + text
- [ ] Animation: slide in from right (250ms), hold, slide out to right (150ms)
- [ ] Action button support (optional — click calls registered callback)
- [ ] System notifications: new USB device, low memory, network connected, disk full
- [ ] Syscall: `SYS_NOTIFY_SEND` for usermode apps
- [ ] Commit: `"desktop: notification system (toast)"`

---

## 6. Screenshot System

**Prompt:** Win+PrintScreen saves a full-screen screenshot as `C:\Users\Default\Pictures\Screenshot_%Y%m%d_%H%M%S.png`. PrintScreen alone copies to clipboard. Alt+PrintScreen captures the active window only. The screenshot captures the compositor back buffer (post-flip), so it includes all composited effects. Use `image_save_png()` from §3.4 of TODO-110. Show a notification toast on save: "Screenshot saved to Pictures". After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: screenshot feature"`. Add notes directly in this TODO section.

> **Beats:** Linux screenshot on Wayland requires compositor protocol extensions. Windows screenshot is built into Win32. Impossible OS: in-compositor capture of back buffer — simpler than Wayland approach.

- [ ] Intercept PrintScreen key in keyboard handler → callback to screenshot module
- [ ] `screenshot_capture_full()` — copy compositor back buffer → `image_save_png(path)`
- [ ] `screenshot_capture_window(wnd)` — capture active window rect from back buffer
- [ ] Clipboard: PrintScreen alone copies image to kernel clipboard (bitmap format)
- [ ] File naming: `C:\Users\Default\Pictures\Screenshot_%04d%02d%02d_%02d%02d%02d.png`
- [ ] Notification: "Screenshot saved to Pictures" (uses notify_send from §5)
- [ ] Hotkeys: PrintScreen → clipboard, Win+PrintScreen → save to file, Alt+PrintScreen → active window
- [ ] Commit: `"desktop: screenshot feature"`

---

## 7. Wallpaper Engine

**Prompt:** The wallpaper loads from path in Registry `HKCU\Software\Impossible\Theme\Wallpaper` and fit mode in `WallpaperMode` (`fill`, `fit`, `stretch`, `center`, `tile`). Change detection: watch the Registry key — on change, reload and redraw. The scaled wallpaper is cached so the desktop compositor doesn't re-decode every frame. Provide a wallpaper picker in the Display Control Panel applet. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: wallpaper engine"`. Add notes directly in this TODO section covering the cache invalidation logic and fit modes.

> **Note:** The image loading and scaling infrastructure (stb_image, image_scale) is already complete in TODO-110 §3. This section wires those into a full wallpaper change system with Registry change detection.

- [ ] `wallpaper_set(path, mode)` — load, scale, cache, redraw desktop
- [ ] Registry watch: `HKCU\Software\Impossible\Theme\Wallpaper` change → auto-reload
- [ ] Support fit modes: fill, fit, stretch, center, tile (maps to `image_fit_t` from image.h)
- [ ] Cache scaled wallpaper — do not re-decode every compositor frame
- [ ] Display Control Panel applet: wallpaper picker, fit mode dropdown, thumbnail preview
- [ ] Solid color fallback if path invalid or file missing
- [ ] `background_color` Registry key: fallback solid color (e.g., `#1C1C1C`)
- [ ] Commit: `"desktop: wallpaper engine"`

---

## 8. Context Menus

**Prompt:** Right-click anywhere on the desktop opens a context menu: View (icon size: small/medium/large), Sort By (name/size/type/date), New Folder, New File, Display Settings, Personalize. Right-click on a window title bar: Restore, Move, Size, Minimize, Maximize, Close. Right-click on a desktop icon: Open, Rename, Delete, Properties. Context menus are positioned near the click point, constrained to screen bounds, and close on outside click or Escape. Style: Acrylic background, rounded corners, separator lines. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: context menus"`. Add notes directly in this TODO section.

- [ ] Create `src/desktop/ctxmenu.c` and `include/desktop/ctxmenu.h`
- [ ] `ctxmenu_show(items[], count, x, y)` — show menu at position, constrained to screen
- [ ] `ctxmenu_hide()` — close active menu
- [ ] Style: Acrylic background, 8px rounded corners, 24px item height, separator support
- [ ] Keyboard navigation: Up/Down arrows, Enter to select, Escape to close
- [ ] Desktop right-click menu: View, Sort By, New Folder, New File, Display Settings, Personalize
- [ ] Window title bar right-click: Restore, Move, Size, Minimize, Maximize, Close
- [ ] Desktop icon right-click: Open, Rename, Delete, Properties
- [ ] Commit: `"desktop: context menus"`

---

## 9. Taskbar System Tray

**Prompt:** The system tray (notification area) on the right side of the taskbar shows: clock (HH:MM), battery icon (if battery found — uses TODO-080 §9.3), Wi-Fi/network icon, speaker/volume icon, keyboard layout indicator (if non-US layout active). Each icon can show a tooltip on hover and a flyout on click (e.g., volume slider on speaker click, clock detail on clock click). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: system tray"`. Add notes directly in this TODO section.

- [ ] Define `tray_icon_t` struct: icon_id, tooltip string, click callback, flyout callback
- [ ] `tray_register(icon)` / `tray_unregister(icon)` — dynamic registration
- [ ] Clock: renders `HH:MM` using FONT_UI, right-aligned in tray
- [ ] Battery icon: shown only if `HKLM\HARDWARE\Battery\Percentage` exists
- [ ] Network icon: shown based on NIC link state
- [ ] Volume icon: speaker icon, click → volume flyout with slider
- [ ] Keyboard layout indicator: "EN" / "FR" / "DE" text, click → layout picker (TODO-060 §3)
- [ ] Hover tooltip: shows full status text (e.g., "Battery: 78% — Plugged in")
- [ ] Commit: `"desktop: system tray"`

---

## 10. Tooltips

**Prompt:** Tooltips appear when the mouse hovers over a control for 500ms. They display a short description string near the mouse cursor, constrained to screen bounds. Style: solid dark background, white text, rounded corners, 1px border. Tooltips auto-hide after 5 seconds or when the mouse moves away. Register a tooltip on any control: `tooltip_set(widget, text)`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: tooltips"`. Add notes directly in this TODO section.

- [ ] `tooltip_set(widget_id, text)` — register tooltip for widget
- [ ] Start 500ms hover timer in WM on mouse-enter
- [ ] On timer expiry: show tooltip near cursor (offset +16, +16 from mouse, constrained to screen)
- [ ] Auto-hide: on mouse-leave or after 5 seconds
- [ ] Style: 8px rounded corners, dark bg from theme, white text, FONT_UI at 12px
- [ ] Fade-in animation (100ms, EaseOut — uses §3 animation system)
- [ ] Commit: `"desktop: tooltips"`

---

## Priority Order

| Priority | Section                        | Reason                                                    |
|----------|--------------------------------|-----------------------------------------------------------|
| 🔴 P0    | §1 Theme System                | Foundation — all controls/windows need themed colors      |
| 🟠 P1    | §2 Start Menu                  | Core desktop feature — Windows 11 parity                  |
| 🟠 P1    | §8 Context Menus               | Right-click is fundamental desktop interaction            |
| 🟠 P1    | §7 Wallpaper Engine            | Registry change detection + fit modes                     |
| 🟡 P2    | §5 Notification System         | Apps need to surface status/events to user                |
| 🟡 P2    | §9 System Tray                 | Clock + battery + network — essential desktop chrome      |
| 🟡 P2    | §3 Animations                  | Fluid motion — polishes the desktop significantly         |
| 🟡 P2    | §10 Tooltips                   | Discoverability — users need control hints                |
| 🟢 P3    | §4 DPI Scaling                 | High-DPI monitor support                                  |
| 🟢 P3    | §6 Screenshot                  | PrintScreen → png in Pictures                             |

---

## Key Files

| File                              | Purpose                              |
|-----------------------------------|--------------------------------------|
| `src/desktop/theme.c`             | [NEW] Theme struct + Registry load   |
| `include/desktop/theme.h`         | [NEW] Theme API header               |
| `src/desktop/startmenu.c`         | [NEW] Start Menu                     |
| `src/desktop/anim.c`              | [NEW] Animation system               |
| `src/desktop/notify.c`            | [NEW] Toast notification queue       |
| `src/desktop/ctxmenu.c`           | [NEW] Context menus                  |
| `src/desktop/tray.c`              | [NEW] System tray icons              |

---

## OS Comparison

| Feature                         | Windows 11                            | Linux (GNOME/KDE)                   | Impossible OS                              |
|---------------------------------|---------------------------------------|-------------------------------------|--------------------------------------------|
| Theme system (color tokens)     | ✅ DWM / UWP resource dictionaries    | ✅ GTK CSS / KDE theme engine         | ⬜ §1 P0                                   |
| Dark / Light mode               | ✅ Registry `AppsUseLightTheme`       | ✅ GTK `prefer-dark-theme`            | ⬜ §1 P0 — Dark preset default            |
| Live theme hot-reload           | ✅ WM_THEMECHANGED broadcast          | ✅ GSettings notify                   | ⬜ §1 P0 — WM_THEME_CHANGED broadcast     |
| Start Menu                      | ✅ (controversial design in Win11)    | ✅ GNOME Dash / KDE Application Menu  | ⬜ §2 P1 — clean, no telemetry           |
| Context menus                   | ✅ ShellExecuteEx / Explorer shell    | ✅ Nautilus / Dolphin                 | ⬜ §8 P1                                  |
| Toast notifications             | ✅ WinRT ToastNotification (COM)     | ✅ libnotify + notification daemon    | ⬜ §5 P2 — **in-kernel, no daemon**       |
| DPI scaling                     | ✅ Per-monitor DPI awareness          | ✅ Fractional scaling (GDK / KWin)   | ⬜ §4 P3 — global scale factor            |
| Screenshot (PrintScreen)        | ✅ Snipping Tool / Win+PrtSc          | ✅ gnome-screenshot / Flameshot       | ⬜ §6 P3                                  |
| Wallpaper with fit modes        | ✅ Fill/Fit/Stretch/Center/Tile       | ✅ gnome-settings → various           | ✅ Done (§3.3 TODO-110) + ⬜ §7 change-detect |
| Animations                      | ✅ DWM compositor                     | ✅ Mutter (GNOME) / KWin              | ⬜ §3 P2 — in-kernel tick                 |
| System tray (notification area) | ✅ Shell_NotifyIcon / NOTIFYICONDATA  | ✅ SystemTray / AppIndicator          | ⬜ §9 P2                                  |
| Tooltips                        | ✅ TOOLTIPTEXT / Window tooltip       | ✅ GtkTooltip                         | ⬜ §10 P2                                 |
| **Clean Start Menu (no ads)**   | ❌ Win11 has recommendations + ads   | ✅ Clean                              | ✅ **§2 — pinned apps only, no telemetry** |
| **In-kernel toast (no daemon)** | ❌ Requires COM + WinRT              | ❌ Requires separate notify daemon    | ⬜ **§5 — single in-kernel queue**        |
| **In-kernel animation tick**    | ❌ DWM separate process               | ❌ Mutter/KWin separate process       | ⬜ **§3 — compositor loop, no IPC**       |
