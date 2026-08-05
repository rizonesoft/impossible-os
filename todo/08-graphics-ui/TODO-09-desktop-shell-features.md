---
schema_version: 1
id: desktop-shell-features
domain: 08-graphics-ui
status: active
title: "TODO-09 -- Desktop Shell Features"
---

# TODO-09 -- Desktop Shell Features

> **Goal:** Complete the full suite of desktop-level shell features -- context menu engine, desktop right-click menu, wallpaper engine with fit modes, DPI scaling, PrintScreen screenshot, night light compositor LUT, Focus/DND mode, quick settings slide-in panel, and virtual desktops. This brings the desktop to Windows 11 feature parity on the shell layer.

> [!IMPORTANT]
> **Already exists**: `image_fit_t` (STRETCH/FILL/FIT/CENTER/TILE), `image_scale(dst, src, w, h, mode)`, `image_save_png(img, path)`, `image_load(img, path)`, `desktop_draw_wallpaper()`, `desktop_get_wallpaper_surface()`, `desktop_copy_wallpaper_rect()` -- wallpaper plumbing is partially done; §3 completes `wallpaper_set(path, mode)` and Registry watch. `fb_get_width/height()` for screen dims. `rtc_read(struct rtc_time *t)` in `include/kernel/drivers/rtc.h` for screenshot timestamp. `wm_post_message_all()` (TODO-01) for broadcasting `WM_DPI_CHANGED`. `gfx_acrylic()` for context menu background. **Missing**: context menu engine, `g_dpi_pct`/`DPI_SCALE`, screenshot API, night light LUT, quick settings panel, virtual desktops. TODO-09 notifications (toast) is a forward dependency for screenshot toast and DND mode -- use a `desktop_toast(msg, icon)` stub that logs to serial if TODO-09 is not yet live. Complete sections in order: wallpaper → DPI → context menu engine → desktop right-click → screenshot → night light → focus/DND → quick settings → virtual desktops.

## Inputs

- `include/kernel/image.h` -- `image_fit_t`, `image_scale()`, `image_save_png()`, `image_load()` -- used by §3 wallpaper and §5 screenshot
- `include/desktop/desktop.h` -- `desktop_draw_wallpaper()`, `desktop_get_wallpaper_surface()` -- extended in §3
- `include/kernel/drivers/framebuffer.h` -- `fb_get_width()`, `fb_get_height()` -- used by §4 DPI auto-detect and §5 screenshot
- `include/kernel/drivers/rtc.h` -- `rtc_read(struct rtc_time *)` -- used by §5 screenshot filename
- `include/desktop/wm.h` -- `wm_composite()`, `wm_mark_dirty()`, `wm_create_window()`, `z_order` -- used by §1 context menu overlay and §7 virtual desktops
- `include/gfx.h` -- `gfx_acrylic()`, `gfx_fill_rounded_rect()`, `gfx_drop_shadow()` -- used by §1 context menu and §6 quick settings
- `include/desktop/theme.h` (TODO-01) -- `theme_get()`, `WM_THEME_CHANGED` -- §3 wallpaper reload triggered here; `WM_DPI_CHANGED` added in §4
- `include/kernel/gfx/anim_mgr.h` (TODO-02) -- `anim_mgr_add()`, `GFX_EASE_OUT_CUBIC` -- §6 quick settings slide-in + §7 vdesk fade
- `include/registry.h` -- `RegGetValue/SetValueEx` -- used by §3 wallpaper, §4 DPI, §5 screenshot path, §8 night light, §9 focus mode
- → XREF: `08-graphics-ui/TODO-08-window-manager.md` -- desktop right-click (§2) and quick settings (§6) depend on WM overlay being live; `wm_post_message_all()` used for `WM_DPI_CHANGED`
- Related (no stable XREF target): `09-desktop-shell/TODO-01-*` (taskbar) -- §7 virtual desktops needs per-desktop taskbar button group
- Related (no stable XREF target): `09-notifications/TODO-01-*` (TODO-09 notifications) -- §6 screenshot toast and §10 DND mode depend on toast API; use stub until TODO-09 is live

## Outcome

- `wallpaper_set(path, mode)` loads, scales, caches wallpaper; Registry-watched; Mica stays in sync.
- `g_dpi_pct` global + `DPI_SCALE(x)` macro applied to all font sizes, icons, chrome heights; `WM_DPI_CHANGED` broadcast.
- `context_menu_show(x, y, items[], count)` renders Acrylic popup with submenus; used throughout the shell.
- PrintScreen → full/window capture → `image_save_png` to Pictures; clipboard copy; toast.
- Night light LUT applied in `wm_composite()` at strength 0–100.
- Quick settings panel slides in from right (Win+A); Night Light / Focus / Volume / Brightness tiles.
- Up to 8 virtual desktops; Win+Ctrl+D/F4/←/→; Task View overlay (Win+Tab).

## Implementation Order

| ⭐  | Order | Deliverable                                                                                          | Depends On                                                                  | Status |
| --- | :---: | ---------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §3 Wallpaper engine -- `wallpaper_set()`, fit modes, cache, Registry watch + reload                 | `image_load/scale`, `desktop_draw_wallpaper()` (already exist)             |  [ ]   |
| 💎  |   2   | §4 DPI scaling -- `g_dpi_pct`, `DPI_SCALE()`, auto-detect, `WM_DPI_CHANGED` broadcast              | §3 (wallpaper re-scale needed on DPI change); TODO-01 `wm_post_message_all` |  [ ]   |
| 💎  |   3   | §1 Context menu engine -- `context_menu_show/hide`, Acrylic popup, submenus, keyboard nav           | §2 DPI (context menu item heights use `DPI_SCALE`)                          |  [ ]   |
| 💎  |   4   | §2 Desktop right-click menu -- view/sort/refresh/new/paste/settings items using §1 engine           | §3 context menu engine (§1 must exist before wiring desktop menus)          |  [ ]   |
| 💎  |   5   | §5 Screenshot -- PrintScreen hook, full + window capture, `image_save_png`, clipboard, toast stub  | §3 wallpaper done (compositor back buffer is clean wallpaper+windows frame) |  [ ]   |
| 💎  |   6   | §8 Night light -- compositor LUT (warm RGB shift), Registry schedule, kernel timer                  | §5 (compositor is confirmed stable before adding LUT pass)                  |  [ ]   |
| 💎  |   7   | §9 Focus / DND -- Registry-backed mode enum, toast suppression filter, quick-settings tile hook     | §6 night light (both are quick-settings tiles; wire together)               |  [ ]   |
| 💎  |   8   | §6 Quick settings panel -- slide-in from right, tiles grid, Wi-Fi/BT stubs, night light/DND/volume | §6+§7 night light + focus (tiles must be wired); §3 context menu (for panel) |  [ ]   |
| ⭐  |   9   | §7 Virtual desktops -- 8 desktops, per-desktop Z-order, Win+Ctrl keys, Task View overlay           | §8 quick settings (task view is another overlay; patterns established)      |  [ ]   |

---

## 1. Context Menu Engine `[Opus]`

`context_menu_show(x, y, items[], count)`: Acrylic blur background, 8 px rounded corners, 24 px item height, drop shadow. `struct menu_item` (label, icon_id, callback, submenu_ptr, separator/disabled/checked). Keyboard: up/down, Enter, Escape, submenu on hover. Auto-close on outside click.

**Files:** `src/desktop/context_menu.c` (new), `include/desktop/context_menu.h` (new)

> [!NOTE]
> This is `[Opus]` -- the context menu engine is a novel overlay architecture: it must (1) render above all application windows, (2) auto-close on any click outside its bounds, (3) support cascading submenus (a child overlay spawned at the ▶ item's right edge on hover after 300 ms), and (4) capture keyboard input globally while open -- none of which exist in Impossible OS. **Popup window**: `wm_create_window(NULL, cx, cy, popup_w, popup_h, WM_FLAG_VISIBLE)` at z_order=25000; no decoration. **Acrylic**: `gfx_acrylic(s, 0, 0, w, h, theme_get()->surface, 200, 12)` as background. **Item height**: `DPI_SCALE(24)`. **Outside-click close**: in `wm_handle_mouse()`, if any context menu open and click is not in any context menu window: `context_menu_hide()`. **Submenu**: `struct menu_item.submenu_ptr` → pointer to another `context_menu_t`; on hover > 300 ms: `context_menu_show_sub(parent, item_idx)`; child spawns at right edge of parent + item's y. **Checked items**: draw Fluent checkmark icon at left if `item.checked`. Max nesting depth: 3.

- [ ] `struct menu_item { char label[128]; uint32_t icon_id; void (*callback)(void); void *submenu_ptr; uint8_t flags; }` -- `flags`: `MENU_SEPARATOR=1`, `MENU_DISABLED=2`, `MENU_CHECKED=4`, `MENU_SUBMENU=8`
- [ ] `typedef struct { struct menu_item items[32]; int count; int popup_wh; int hovered_idx; } context_menu_t;` in `context_menu.h`
- [ ] `void context_menu_show(int32_t x, int32_t y, struct menu_item items[], int count)` → create popup window; acrylic bg; render items
- [ ] `void context_menu_hide(void)` -- destroy popup + any open submenus
- [ ] `int context_menu_is_open(void)` → non-zero if any menu popup visible
- [ ] Draw: per item: icon (16 px); label `FONT_UI 13 px`; checkmark if checked; ▶ if submenu; separator as 1 px `theme_get()->border`; disabled items in `theme_get()->foreground_muted`; hover: `theme_get()->button_hover` background fill
- [ ] Mouse: hover sets `hovered_idx`; 300 ms hover timer → open submenu; click → `item.callback()`; `context_menu_hide()`; outside click → `context_menu_hide()`
- [ ] Key: Up/Down move `hovered_idx`; Enter → callback + hide; Escape → hide; Right → open submenu; Left → close submenu (return to parent)
- [ ] `context_menu_tick()` called from `wm_composite()` before drawing; checks hover timer for submenu open
- [ ] Commit: `"desktop: context_menu engine -- Acrylic popup, submenus, keyboard nav, outside-click auto-close"`

## 2. Desktop Right-Click Menu `[Sonnet]`

Right-click on wallpaper → context menu: View submenu (icon size), Sort By submenu, Refresh, New submenu (Folder/Text/Shortcut), Paste, Display Settings, Personalize.

**Files:** `src/desktop/desktop.c` (extend), `src/desktop/desktop_rightclick.c` (new)

> [!NOTE]
> Wire into `desktop_handle_mouse()`: if right-click at position not covered by any window (`wm_window_at(mx, my) == -1`): call `context_menu_show(mx, my, desktop_menu_items, count)`. Submenus use `MENU_SUBMENU` flag and a pointer to child `context_menu_t` arrays defined as static tables. Item callbacks: View submenu → `desktop_icons_set_size(SMALL/MEDIUM/LARGE)` stub (icon size persisted to Registry); Sort By → `desktop_icons_sort(by)`; Refresh → `desktop_icons_init()` re-scan; New → `dialog_input("File name:", ...)` + `vfs_create()`; Paste → clipboard paste stub; Display Settings → `desktop_open_display_settings()` (opens Control Panel applet); Personalize → same with Personalization tab.

- [ ] `static struct menu_item desktop_menu[]` + view/sort/new submenu arrays in `desktop_rightclick.c`
- [ ] `void desktop_show_context_menu(int32_t x, int32_t y)` -- calls `context_menu_show()` with desktop menu
- [ ] Right-click handler in `desktop_handle_mouse()`: if no window at cursor → `desktop_show_context_menu()`
- [ ] View submenu callbacks: set icon size (small=32, medium=48, large=64); persist to `HKCU\Software\Impossible\Shell\Desktop\IconSize`
- [ ] Sort By callbacks: `desktop_icons_sort(SORT_NAME|SORT_SIZE|SORT_TYPE|SORT_DATE)` stub
- [ ] Refresh: `desktop_icons_init()` re-scans `C:\Users\Default\Desktop\`
- [ ] New Folder: `dialog_input("New folder name:", "", "New Folder")` → `vfs_mkdir(path)`
- [ ] New Text Document: `vfs_create("C:\\Users\\Default\\Desktop\\New Text Document.txt", "")` → `desktop_icons_init()`
- [ ] Commit: `"desktop: right-click menu -- view/sort/new/refresh/settings using context_menu engine"`

## 3. Wallpaper Engine `[Sonnet]`

`wallpaper_set(path, mode)`: load via `image_load()`, scale via `image_scale(mode)`, cache scaled bitmap. Registry watch: `HKCU\Software\Impossible\Theme\Wallpaper` + `WallpaperMode` → auto-reload on change. `background_color` fallback. Display Control Panel: thumbnail + fit-mode dropdown.

**Files:** `src/desktop/wallpaper.c` (new), `include/desktop/wallpaper.h` (new), `src/desktop/desktop.c` (extend)

> [!NOTE]
> Existing `desktop_draw_wallpaper()` draws a pre-loaded wallpaper; `wallpaper_set()` is the new entrypoint that replaces the hardcoded wallpaper load. **Cache strategy**: scale to exact screen resolution `(fb_get_width(), fb_get_height())` once; store as `uint32_t *g_wallpaper_scaled` (allocated via `pmm_alloc_contiguous(w*h*4)`). On every `desktop_draw_wallpaper()` call: `memcpy(screen, g_wallpaper_scaled, w*h*4)` (O(n) blit; no per-frame decode). Registry watch: poll `HKCU\Software\Impossible\Theme\Wallpaper` every 5 s in a background tick (called from `desktop_tick()`); if value changes: free old buffer, reload, re-scale. `background_color`: if `image_load()` fails: fill screen with `RegGetValue("BackgroundColor", DWORD)`. Acrylic/Mica consumers (`gfx_mica`, acrylic_cache) are automatically updated because they read `desktop_get_wallpaper_surface()` each time.

- [ ] `void wallpaper_set(const char *path, image_fit_t mode)` in `wallpaper.c`: `image_load()`; `image_scale()` to screen dims; store in `g_wallpaper_scaled`; `wm_mark_dirty()`
- [ ] `void wallpaper_init(void)`: read `HKCU\Software\Impossible\Theme\Wallpaper` + `WallpaperMode`; call `wallpaper_set()`; fallback to `background_color` DWORD on error
- [ ] `void wallpaper_tick(void)`: called from `desktop_tick()` every 5 s; `RegGetValue()` and compare to last loaded path; reload if changed
- [ ] Update `desktop_draw_wallpaper()` to use `g_wallpaper_scaled` via `memcpy`; update `desktop_get_wallpaper_surface()` to wrap the new scaled buffer
- [ ] `background_color` fallback: `gfx_fill_rect(screen, 0, 0, fb_w, fb_h, bg_color)` if wallpaper load fails
- [ ] `void wallpaper_reload_on_dpi(void)`: `image_scale()` re-scale to new screen dims; called from `WM_DPI_CHANGED` handler
- [ ] Commit: `"desktop: wallpaper_set() -- image_load+scale, cache, Registry watch, background_color fallback"`

## 4. DPI Scaling `[Sonnet]`

`g_dpi_pct` global (default 100). `DPI_SCALE(x)` macro. Auto-detect: ≥2560×1440→150%, ≥3840×2160→200%. Read from `HKCU\Software\Impossible\Display\ScaleFactor`. Apply to font sizes, icon sizes, chrome heights, control heights. `WM_DPI_CHANGED` broadcast. Display Control Panel dropdown.

**Files:** `include/desktop/dpi.h` (new), `src/desktop/dpi.c` (new), `include/desktop/wm.h` (extend)

> [!NOTE]
> `DPI_SCALE(x)` is `(int)((x) * g_dpi_pct / 100)`. All sizing uses this macro: `WM_TITLEBAR_HEIGHT = DPI_SCALE(32)`, `TASKBAR_H = DPI_SCALE(48)`, `font_size = DPI_SCALE(base_size)`. `WM_DPI_CHANGED` message constant: add to `wm.h` after `WM_THEME_CHANGED`; `wm_post_message_all(WM_DPI_CHANGED, g_dpi_pct, 0)`. Auto-detect: in `dpi_init()`: if `fb_get_width() >= 3840` → 200; else if >= 2560 → 150; else 100; override with Registry if present. Live change: `dpi_set(pct)` -- update `g_dpi_pct`; call `wm_post_message_all(WM_DPI_CHANGED, pct, 0)`; each window re-measures its controls; `wallpaper_reload_on_dpi()`. Font sizes: `ttf_get(FONT_UI, DPI_SCALE(13))` -- pass DPI-scaled size to font manager.

- [ ] `extern int g_dpi_pct;` + `#define DPI_SCALE(x) ((int)((x) * g_dpi_pct / 100))` in `include/desktop/dpi.h`
- [ ] `void dpi_init(void)`: auto-detect from framebuffer dims; override from Registry; set `g_dpi_pct`
- [ ] `void dpi_set(int pct)`: clamp to [75, 300]; update `g_dpi_pct`; persist to Registry; `wm_post_message_all(WM_DPI_CHANGED, pct, 0)`; `wallpaper_reload_on_dpi()`
- [ ] `#define WM_DPI_CHANGED 0x0021` in `wm.h` (after `WM_THEME_CHANGED = 0x0020`)
- [ ] Apply `DPI_SCALE()` to: `WM_TITLEBAR_HEIGHT`, `WM_BTN_WIDTH`, `WM_BTN_HEIGHT`, `TASKBAR_H` (all currently hardcoded); font pixel sizes passed to `ttf_get()`; icon request sizes; control ROW_H and BTN_W
- [ ] `dpi_init()` called from `desktop_init()` before any window creation
- [ ] Display Control Panel hook: `ctrl_create_dropdown(…, ["100%","125%","150%","175%","200%"], 5, on_scale_change)` where `on_scale_change` calls `dpi_set()`
- [ ] Commit: `"desktop: DPI scaling -- g_dpi_pct, DPI_SCALE macro, auto-detect, WM_DPI_CHANGED broadcast"`

## 5. Screenshot `[Sonnet]`

Intercept PrintScreen in keyboard handler. `screenshot_capture_full()`: copy compositor back buffer → `image_save_png()` to `C:\Users\Default\Pictures\Screenshot_%Y%m%d_%H%M%S.png`. Alt+PrintScreen → crop to active window rect. Win+PrintScreen → save to file + toast. PrintScreen alone → kernel clipboard (bitmap).

**Files:** `src/desktop/screenshot.c` (new), `include/desktop/screenshot.h` (new)

> [!NOTE]
> PrintScreen scancode: `0xE037` (extended) or `0x54` (regular PS/2 make code). Detect in the desktop keyboard handler. Back buffer: `wm_composite()` renders to a screen-sized `uint32_t *` buffer; `screenshot_capture_full()` wraps it as `image_t { .pixels = back_buffer, .width = fb_get_width(), .height = fb_get_height() }` then calls `image_save_png()`. Filename: `rtc_read(&t)`; format as `Screenshot_%04u%02u%02u_%02u%02u%02u.png`. Path: `C:\\Users\\Default\\Pictures\\` -- create dir if not exists via `vfs_mkdir()`. Clipboard: for PrintScreen-only (no modifier), call `clipboard_set_bitmap(back_buffer, fb_w, fb_h)` (stub until clipboard TODO); log to serial if clipboard not ready. Toast: `desktop_toast("Screenshot saved to Pictures", ICON_INFO)` stub -- logs to serial if TODO-09 not live.

- [ ] `void screenshot_capture_full(const char *filepath)`: wrap back buffer as `image_t`; `image_save_png(img, filepath)` → 0 or -errno
- [ ] `void screenshot_capture_window(int handle, const char *filepath)`: get `win->x, y, width, height`; copy that rect from back buffer; scale to `image_t`; `image_save_png()`
- [ ] `void screenshot_auto_path(char *buf, uint32_t max)`: `rtc_read(&t)`; `snprintf(buf, max, "C:\\Users\\Default\\Pictures\\Screenshot_%04u%02u%02u_%02u%02u%02u.png", ...)`
- [ ] PrintScreen intercept in `desktop_handle_key()`: if `KEY_PRINTSCREEN` and no modifier → clipboard copy; if Alt held → `screenshot_capture_window(focused_handle, ...)`; if Win held → `screenshot_capture_full(path)` + `desktop_toast("Screenshot saved", ICON_INFO)`
- [ ] `vfs_mkdir("C:\\Users\\Default\\Pictures\\")` on first screenshot if dir absent
- [ ] Log: `[screenshot] saved: %s (%u bytes)` on success; `[screenshot] failed: %d` on error
- [ ] Commit: `"desktop: screenshot -- PrintScreen full/window capture, image_save_png, clipboard+toast stub"`

## 6. Night Light `[Sonnet]`

`night_light_set_active(enabled, strength_pct)`: apply warm RGB shift LUT in `wm_composite()`. `R_out=R, G_out=G*(1-0.3*s), B_out=B*(1-0.6*s)`. Registry: `HKCU\Software\Impossible\Display\NightLight\Enabled`, `Strength` (0–100), `ScheduleStart/End` (HH:MM). Kernel timer checks schedule every minute.

**Files:** `src/desktop/night_light.c` (new), `include/desktop/night_light.h` (new), `src/desktop/wm.c` (extend compositor)

> [!NOTE]
> LUT application: after `wm_composite()` completes the full frame but before `fb_blit()` to screen: iterate every pixel in the back buffer; apply `G = G * g_nl_g_scale >> 8`; `B = B * g_nl_b_scale >> 8` (pre-compute `g_nl_g_scale = 256 * (1 - 0.3 * strength/100)` and `g_nl_b_scale = 256 * (1 - 0.6 * strength/100)` as integers when strength changes -- avoid per-pixel floats). When disabled: skip LUT pass entirely (check `g_night_light_enabled` flag). **Schedule**: `night_light_tick()` called from a background kernel timer task every 60 s; `rtc_read(&t)`; compare `t.hour*60+t.min` against schedule start/end; if in range: `night_light_set_active(1, 60)` (default 60%); else: `night_light_set_active(0, 0)`. Performance: at 1920×1080 = 2M pixels × 2 multiply+shift ops ≈ 4M operations; should complete < 2 ms.

- [ ] `void night_light_init(void)` -- read Registry enabled/strength/schedule; compute `g_nl_g_scale/b_scale`; start schedule timer task
- [ ] `void night_light_set_active(int enabled, int strength_pct)` -- clamp strength 0–100; recompute integer scales; `g_night_light_enabled = enabled`; `wm_mark_dirty()`; persist to Registry
- [ ] `void night_light_apply(uint32_t *pixels, uint32_t count)` -- if disabled: return; iterate; `g = (ARGB>>8)&0xFF; g = g * g_nl_g_scale >> 8; b = ARGB&0xFF; b = b * g_nl_b_scale >> 8;` repack ARGB
- [ ] `wm_composite()`: after final blit assembly but before `fb_blit()`: `night_light_apply(back_buffer, fb_w * fb_h)`
- [ ] `night_light_tick()` scheduled task: `rtc_read()`; compare against schedule HHMM; toggle automatically
- [ ] `int night_light_enabled(void)` + `int night_light_strength(void)` -- read-only accessors for quick settings
- [ ] Commit: `"desktop: night light -- integer LUT warm shift, Registry schedule, compositor apply pass"`

## 7. Focus / Do Not Disturb `[Sonnet]`

`HKCU\Software\Impossible\Shell\FocusMode` (0=off, 1=priority, 2=alarms-only). Priority: suppress non-priority toasts. Alarms-only: suppress all toasts. Quick settings tile toggle. Auto-enable during fullscreen apps (`DetectFullscreen` flag).

**Files:** `src/desktop/focus_mode.c` (new), `include/desktop/focus_mode.h` (new)

> [!NOTE]
> `focus_mode_t` enum: `FOCUS_OFF=0`, `FOCUS_PRIORITY=1`, `FOCUS_ALARMS=2`. `focus_mode_set(mode)` writes to Registry + sets `g_focus_mode`. Toast filter: `desktop_toast(msg, icon, priority)` checks `g_focus_mode`; if `FOCUS_ALARMS` and `priority != PRIORITY_ALARM`: return without showing; if `FOCUS_PRIORITY` and `priority == PRIORITY_NORMAL`: return. Add `uint8_t priority` parameter to `desktop_toast()` stub. `DetectFullscreen`: in the compositor loop, check if the topmost window covers the full screen (`win->x == 0 && win->y == 0 && win->width == fb_w && win->height == fb_h`); if so and `DetectFullscreen` Registry flag is set: auto-set `FOCUS_PRIORITY`.

- [ ] `typedef enum { FOCUS_OFF=0, FOCUS_PRIORITY=1, FOCUS_ALARMS=2 } focus_mode_t;` in `focus_mode.h`
- [ ] `extern focus_mode_t g_focus_mode;`
- [ ] `void focus_mode_init(void)`: read `HKCU\Software\Impossible\Shell\FocusMode` + `DetectFullscreen` DWORDs
- [ ] `void focus_mode_set(focus_mode_t mode)`: set `g_focus_mode`; persist to Registry; `wm_mark_dirty()` to update tray icon
- [ ] `int focus_mode_allows_toast(uint8_t priority)` → 1 if toast should show: `FOCUS_OFF`=always; `FOCUS_PRIORITY`=only `PRIORITY_HIGH+ALARM`; `FOCUS_ALARMS`=only `PRIORITY_ALARM`
- [ ] `desktop_toast(msg, icon)` stub: call `focus_mode_allows_toast(PRIORITY_NORMAL)` before showing; if 0: log to serial only
- [ ] Fullscreen detect: in compositor, if `DetectFullscreen && topmost_covers_screen`: `focus_mode_set(FOCUS_PRIORITY)` silently
- [ ] Commit: `"desktop: focus/DND mode -- FOCUS_OFF/PRIORITY/ALARMS enum, toast filter, fullscreen auto-enable"`

## 8. Quick Settings Panel `[Sonnet]`

Win+A or click notification area → slide-in panel from right (200 ms ease-out-cubic). Tiles grid: Wi-Fi stub, Bluetooth stub, Night Light toggle, Volume slider, Brightness slider, Display Scale picker. Pinned quick actions row. Slide-out on outside click or Escape.

**Files:** `src/desktop/quick_settings.c` (new), `include/desktop/quick_settings.h` (new)

> [!NOTE]
> Panel window: `wm_create_window(NULL, fb_w - PANEL_W, 0, PANEL_W, fb_h - TASKBAR_H, WM_FLAG_VISIBLE)` at z_order=30000; `PANEL_W = DPI_SCALE(360)`. Acrylic background. Slide-in: `wm_anim_state_t.x` tween from `fb_w` to `fb_w - PANEL_W`; 200 ms `GFX_EASE_OUT_CUBIC`. Tile grid: 3 columns × 2 rows of toggle tiles (88×60 px each); tile: rounded-rect background (`theme_get()->surface_variant` off, `theme_get()->accent` on); icon centered (24 px); label below (10 px). Volume/Brightness: `ctrl_create_slider()` -- wire Volume to `volume_set()` stub, Brightness to `brightness_set()` stub. Night Light tile: toggles `night_light_set_active()`; Focus tile: cycles `focus_mode_set()`. Scale picker: inline dropdown showing "100%/125%/150%/200%" calling `dpi_set()`. Outside click: `qs_mouse_handler()` checks if click is outside panel bounds; if so → `quick_settings_close()`. Escape key closes.

- [ ] `void quick_settings_open(void)`: create panel window at z_order=30000; slide-in tween; build tile + slider controls
- [ ] `void quick_settings_close(void)`: slide-out tween (200 ms); `on_complete` → `wm_destroy_window(panel_wh)`
- [ ] `int quick_settings_is_open(void)` -- for hotkey toggle
- [ ] Tile implementation: `qs_tile_t { uint32_t icon_id; char label[32]; int (*get_state)(void); void (*toggle)(void); }` + array of 6 tiles
- [ ] Wi-Fi tile: state = `net_is_connected()` (from TODO-01 networking); toggle = stub log
- [ ] Night Light tile: state = `night_light_enabled()`; toggle = `night_light_set_active(!enabled, 60)`
- [ ] Focus tile: state = `g_focus_mode`; toggle = cycle `FOCUS_OFF → FOCUS_PRIORITY → FOCUS_ALARMS → FOCUS_OFF`
- [ ] Volume + Brightness sliders: `ctrl_create_slider(panel_wh, x, y, w, 20, 0, 100, vol, HORIZ, on_vol_change)` -- `on_vol_change` calls `volume_set(v)` stub
- [ ] Win+A hotkey in `hotkeys.c`: `quick_settings_open()` or `quick_settings_close()` toggle
- [ ] Commit: `"desktop: quick settings panel -- slide-in from right, tiles, night light/focus/volume/scale wired"`

## 9. Virtual Desktops `[Opus]`

Up to 8 desktops. `vdesk_create()`, `vdesk_destroy(idx)`, `vdesk_switch(idx)` (150 ms fade). Per-desktop window Z-order group. Win+Ctrl+D create, Win+Ctrl+F4 close, Win+Ctrl+←/→ switch. Task View overlay (Win+Tab): thumbnail grid.

**Files:** `src/desktop/vdesk.c` (new), `include/desktop/vdesk.h` (new), `src/desktop/wm.c` (extend)

> [!NOTE]
> This is `[Opus]` -- virtual desktops require a novel architecture for Impossible OS: each desktop is an independent Z-order group (windows on the inactive desktop are hidden from the compositor), and `vdesk_switch()` must fade out the current desktop and fade in the new one atomically. **Per-desktop window assignment**: add `uint8_t desktop_idx` to `struct wm_window`. Compositor: skip windows where `win->desktop_idx != g_current_desktop` (they are invisible). **Switch**: `vdesk_switch(idx)`: set `g_switch_from = g_current_desktop`; `gfx_tween_start(&vdesk_fade, 255, 0, 75, GFX_EASE_LINEAR)` (fade out in 75 ms); `on_complete`: set `g_current_desktop = idx`; `gfx_tween_start(&vdesk_fade, 0, 255, 75, GFX_EASE_LINEAR)` (fade in). During fade: compositor applies `vdesk_fade.current / 255.0` alpha to all windows. **Task View**: `wm_create_window(NULL, 0, 0, fb_w, fb_h, WM_FLAG_VISIBLE)` at z_order=40000; dark acrylic full-screen; render `g_desktop_count` rows of desktop thumbnails (each thumbnail is a scaled composite of that desktop's windows rendered offscreen); click thumbnail → `vdesk_switch(idx)`.

- [ ] `uint8_t desktop_idx` field in `struct wm_window` (default 0 = first desktop)
- [ ] `int g_current_desktop = 0`, `int g_desktop_count = 1`, `gfx_tween_t vdesk_fade` in `vdesk.c`
- [ ] `int vdesk_create(void)` → new_idx or -1 if at 8 limit; increments `g_desktop_count`
- [ ] `void vdesk_destroy(int idx)`: move all windows on `idx` to desktop 0; `g_desktop_count--`; if current was idx: `vdesk_switch(0)`
- [ ] `void vdesk_switch(int idx)`: bounds check; fade-out tween → change `g_current_desktop` → fade-in tween
- [ ] Compositor: skip `win->desktop_idx != g_current_desktop`; during fade: `gfx_fill_rect_alpha(screen, 0,0, fw, fh, 0xFF000000, 255 - vdesk_fade.current)` after composite
- [ ] `void taskview_open(void)`: create fullscreen dark overlay; render desktop thumbnails (each: render that desktop's windows at 1/4 scale offscreen; blit as thumbnail); click → `vdesk_switch()`; Win+Tab or Escape → `taskview_close()`
- [ ] Hotkeys in `hotkeys.c`: `MOD_WIN+MOD_CTRL+D` → `vdesk_create()` + `vdesk_switch(new)`; `MOD_WIN+MOD_CTRL+F4` → `vdesk_destroy(g_current)`; `MOD_WIN+MOD_CTRL+LEFT/RIGHT` → `vdesk_switch(current ± 1)`; `MOD_WIN+Tab` → `taskview_open()`
- [ ] New window: assigned to `g_current_desktop` by default; `wm_create_window()` sets `win->desktop_idx = g_current_desktop`
- [ ] Commit: `"desktop: virtual desktops -- 8 desktops, fade switch, per-desktop compositor, Task View overlay"`

---

## OS Comparison


| ⭐  | Feature              | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | -------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | Context menu engine  | ✅ Win32 `CreatePopupMenu`; WinUI3 `MenuFlyout`; Acrylic | ✅ GTK `GtkMenu`; Qt `QMenu`; cascading  | ⬜ §1 -- z_order=25000 overlay; 300 ms hover |
| 💎  | Wallpaper engine     | ✅ `SystemParametersInfo(SPI_SETDESKWALLPAPER)`; fill/fit/stretch/tile/center | ✅ GNOME `gsettings org.gnome.desktop.background`; feh/nitrogen for | ⬜ §3 -- `image_scale(mode)` + `pmm_alloc_contiguous` cache; 5 |
| 💎  | DPI scaling          | ✅ `SetProcessDpiAwareness`; `WM_DPICHANGED`; GDI/WinUI3 auto-scale | ✅ Wayland logical pixels; GDK `GDK_DPI_SCALE`; | ⬜ §4 -- single `g_dpi_pct` global; `DPI_SCALE(x)` applied |
| 💎  | Screenshot           | ✅ PrintScreen to clipboard; Win+PrintScreen → | ✅ `scrot`/`gnome-screenshot`; KDE Spectacle; PrtSc to | ⬜ §5 -- `rtc_read()` timestamp filename; `image_save_png()` to |
| 💎  | Night light          | ✅ Settings → System → Night             | ✅ `redshift`/`gammastep`; GNOME built-in night light; | ⬜ §6 -- integer `g_nl_g_scale/b_scale` per-pixel LUT in |
| 💎  | Focus / DND          | ✅ Windows Focus Assist; priority-only; alarms-only; | ✅ GNOME DND (`org.gnome.desktop.notifications`); KDE DND | ⬜ §7 -- `FOCUS_OFF/PRIORITY/ALARMS` enum; compositor fullscreen detect |
| 💎  | Quick settings panel | ✅ Win+A; Fluent slide-in panel; tiles   | ✅ GNOME quick settings (since 43);      | ⬜ §8 -- z_order=30000; acrylic; 6 tiles; sliders |
| ⭐  | Virtual desktops     | ✅ Win+Ctrl+D/F4/←/→; Task View (Win+Tab); per-desktop | ✅ GNOME workspaces; KDE virtual desktops; | ⬜ §9 -- `⭐` fade composited in kernel  |

> **After §1–§9:** Impossible OS matches Windows 11 on every desktop shell feature. The `⭐` virtual desktop fade is composited in the kernel's software renderer -- a single `gfx_fill_rect_alpha` pass over the already-composited frame buffer -- meaning the transition is frame-perfect with no GPU needed and no per-window alpha manipulation.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Context menu: right-click on empty desktop → menu appears with Acrylic background; click outside → closes; keyboard Up/Down/Enter/Escape; hover View → submenu appears at right edge after 300 ms
- [ ] Wallpaper: `wallpaper_set("C:\\Impossible\\Web\\Wallpaper\\default.jpg", IMAGE_FIT_FILL)` → wallpaper visible; change `HKCU\...\Wallpaper` Registry key → reloads within 5 s; invalid path → background color shown
- [ ] DPI: set `g_dpi_pct = 150` → fonts 50% larger, icons 50% larger, title bar 48 px instead of 32 px; `WM_DPI_CHANGED` in serial log; auto-detect on 1920×1080 → 100%
- [ ] PrintScreen → serial log `[screenshot] saved: C:\Users\Default\Pictures\Screenshot_...png`; file exists in VFS; Alt+PrintScreen → only focused window captured
- [ ] Night light: `night_light_set_active(1, 80)` → visible warm tint in QEMU; pixels have reduced blue/green; disable → full color restored
- [ ] Focus mode: set FOCUS_ALARMS; `desktop_toast(…, PRIORITY_NORMAL)` → no toast, serial log confirms suppressed
- [ ] Quick settings: Win+A → panel slides in from right over 200 ms; Night Light tile click → toggles; Focus tile cycles modes; Escape → slides out
- [ ] Virtual desktops: Win+Ctrl+D → new desktop created; Win+Ctrl+→ → fade transition to desktop 2; window on desktop 1 not visible; Win+Tab → Task View overlay shows both desktops as thumbnails; click desktop 1 thumbnail → switches back
- [ ] Commit: `"desktop: complete shell features -- context menu, wallpaper, DPI, screenshot, night light, quick settings, virtual desktops"`
