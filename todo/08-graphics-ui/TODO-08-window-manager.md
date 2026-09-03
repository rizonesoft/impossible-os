---
schema_version: 1
id: window-manager
domain: 08-graphics-ui
status: active
title: "TODO-08 -- Window Manager Enhancements"
---

# TODO-08 -- Window Manager Enhancements

> **Goal:** Elevate the basic WM (create/move/resize/close) to a Windows 11-quality experience: proper Mica titlebar decorations with Fluent chrome buttons, minimize/maximize/restore with state persistence, snap layouts (hover-popup + keyboard), desktop icons (load/draw/launch/drag), global hotkeys + Win+D/M/Alt+F4, Alt+Tab thumbnail switcher, file drag-and-drop, and compositor dirty-rect optimization for 60 fps frame gating.

> [!IMPORTANT]
> **Existing WM**: `wm_move_window`, `wm_resize_window`, `wm_focus_window`, `wm_mark_dirty`, `wm_handle_mouse`, `wm_window_at`, `wm_get_cursor_context`, `wm_get_drag_dirty_rect` all exist. **Missing from `struct wm_window`**: no `saved_rect`, no `WM_FLAG_MINIMIZED/MAXIMIZED` -- these are added in §1. `FONT_UI_BOLD` in `include/font_mgr.h` for title text. `gfx_mica(s, x, y, w, h, wallpaper, tint)` exists for Mica titlebar. `desktop_get_wallpaper_surface()` provides the wallpaper. `task_create_user()` in `include/kernel/sched/task.h` launches user processes. TODO-02 `wm_anim_state_t` is embedded in `struct wm_window` for all animation. Complete sections in order: decorations → minimize/maximize/restore → snap layouts → compositor performance → desktop icons → keyboard shortcuts → Alt+Tab → drag and drop.

## Inputs

- `include/desktop/wm.h` -- extend `struct wm_window` with `saved_x/y/w/h`, `WM_FLAG_MINIMIZED/MAXIMIZED`, `WM_FLAG_SNAPPED`; add `wm_minimize/maximize/restore/snap` declarations
- `include/gfx.h` -- `gfx_mica()`, `gfx_drop_shadow()`, `gfx_fill_rounded_rect()` for decoration rendering
- `include/font_mgr.h` -- `FONT_UI_BOLD`, `FONT_UI` for title and status text
- `include/desktop/desktop.h` -- `desktop_get_wallpaper_surface()` for Mica titlebar tint source
- `include/desktop/theme.h` (TODO-01) -- `theme_get()->titlebar_active/inactive/accent/button_hover` for decoration colors
- `include/kernel/gfx/wm_anim.h` (TODO-02) -- `wm_anim_minimize/restore/maximize/snap` for animated transitions
- `include/kernel/sched/task.h` -- `task_create_user(entry, name)` for desktop icon double-click launch
- `include/registry.h` -- Registry persistence for window state + desktop icon positions
- → XREF: `08-graphics-ui/TODO-04-animation-engine.md` -- `wm_anim_state_t` fields in `struct wm_window` prerequisite for §1 minimize/maximize animations
- Related (no stable XREF target): `09-desktop-shell/TODO-01-*` (taskbar) -- minimize animation target rect requires taskbar button position; taskbar height defines usable area for maximize

## Outcome

- `wm_minimize/maximize/restore/snap()` implemented with animated transitions.
- Title bar: 32 px, Mica tint, `FONT_UI_BOLD` 13 px title, Fluent chrome buttons (×/─/⬜/❐), Windows 11 hover states.
- Snap layouts popup on maximize-button hover; Win+←/→ keyboard snap.
- Desktop icons load from `C:\Users\Default\Desktop\`; double-click launches; drag repositions.
- Global hotkeys: Win+D, Win+M, Win+L, Alt+F4 + Win+number taskbar launch.
- Alt+Tab overlay with 96×64 window thumbnails, fade in/out 150 ms.
- Compositor dirty-rect union + frame-skip when idle (> 16 ms frame warning to serial).

## Implementation Order

| ⭐  | Order | Deliverable                                                                                              | Depends On                                                                 | Status |
| --- | :---: | -------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §2 Window decorations -- Mica titlebar, Fluent chrome buttons, resize handles                           | `gfx_mica`, `FONT_UI_BOLD`, `theme_get()` (TODO-01) all must be live      |  [ ]   |
| 💎  |   2   | §1 Min/max/restore -- WM_FLAG_MINIMIZED/MAXIMIZED, saved_rect, `wm_minimize/maximize/restore`           | §1 (chrome buttons trigger these; animation from TODO-02)                  |  [ ]   |
| 💎  |   3   | §3 Snap layouts -- hover-popup (4 zones), Win+←/→/↑/↓ keyboard snap, edge-drag preview                | §2 (maximize button hover is the trigger; snap uses wm_maximize geometry)  |  [ ]   |
| 💎  |   4   | §8 Compositor performance -- dirty-rect union, frame-skip when idle, frame-time warning                | §2 (min/max/restore are the biggest dirty-rect drivers; optimize after they work) |  [ ]   |
| 💎  |   5   | §4 Desktop icons -- load from VFS, draw+select+launch, drag reposition, registry persistence           | §1 decorations done (desktop renders behind windows)                       |  [ ]   |
| 💎  |   6   | §5 Keyboard shortcuts -- global hotkey table, Win+D/M/Shift+M/L, Alt+F4, Win+number                   | §2 (wm_minimize/restore needed for Win+M/D); §5 desktop icons for Win+D   |  [ ]   |
| 💎  |   7   | §6 Alt+Tab switcher -- thumbnail capture, overlay panel, 150 ms fade, focus on release                 | §5 (hotkey table registers Alt+Tab); §2 (min state affects thumbnail list) |  [ ]   |
| 💎  |   8   | §7 Drag and drop -- desktop icon drag, file drag from FM to desktop, window move/resize extend         | §5 desktop icons (icon drag extends their existing mouse handling)          |  [ ]   |

---

## 1. Minimize / Maximize / Restore `[Sonnet]`

Add `WM_FLAG_MINIMIZED=0x40`, `WM_FLAG_MAXIMIZED=0x80`, `WM_FLAG_SNAPPED=0x100` to `wm.h`. Add `saved_x/y/w/h` fields to `struct wm_window`. Implement `wm_minimize/maximize/restore`. Persist window state in Registry. Double-click title bar toggles maximize.

**Files:** `src/desktop/wm.c` (extend), `include/desktop/wm.h` (extend)

> [!NOTE]
> `wm_minimize(handle)`: if already minimized, return. Set `WM_FLAG_MINIMIZED`; clear `WM_FLAG_VISIBLE`; `wm_anim_minimize(handle, tx, ty, tw, th)` where `(tx, ty, tw, th)` is the taskbar button rect (pass `0, screen_h - TASKBAR_H, 48, TASKBAR_H` as a placeholder until TODO-08 taskbar provides the exact rect). `wm_maximize(handle)`: save `win->saved_x/y/w/h`; compute usable area = `(0, 0, fb_w, fb_h - TASKBAR_H)`; `wm_move_window()` + `wm_resize_window()` to usable area; set `WM_FLAG_MAXIMIZED`; clear `WM_FLAG_SNAPPED`; `wm_anim_maximize(handle)`. `wm_restore(handle)`: if minimized: set `WM_FLAG_VISIBLE`; `wm_move_window/resize` to saved; `wm_anim_restore()`; clear `WM_FLAG_MINIMIZED`; if maximized: `wm_move_window/resize` to saved; clear `WM_FLAG_MAXIMIZED`; `wm_anim_restore()`. Registry: persist `HKCU\Software\Impossible\Shell\Windows\{title}\State` (MINIMIZED/MAXIMIZED/RESTORED DWORD) + `X/Y/W/H` on each state change.

- [ ] `#define WM_FLAG_MINIMIZED 0x40`, `WM_FLAG_MAXIMIZED 0x80`, `WM_FLAG_SNAPPED 0x100` in `wm.h`
- [ ] `int32_t saved_x, saved_y; uint32_t saved_w, saved_h;` fields in `struct wm_window`
- [ ] `void wm_minimize(int handle)` -- animate to taskbar; hide; set flag
- [ ] `void wm_maximize(int handle)` -- save rect; resize to usable area; animate; set flag
- [ ] `void wm_restore(int handle)` -- return to saved rect; animate; clear flag
- [ ] `wm_handle_mouse()`: double-click on titlebar (two clicks within 300 ms) → `wm_maximize/restore`
- [ ] `#define TASKBAR_H 48` in `wm.h` (placeholder; overridden by taskbar init in TODO-08)
- [ ] Registry persist: write state + rect on change; read on desktop restart
- [ ] Log: `[wm] window %d: minimize → maximized rect saved [%d %d %u %u]`
- [ ] Commit: `"wm: minimize/maximize/restore -- WM_FLAG_MINIMIZED/MAXIMIZED, saved_rect, Registry persist"`

## 2. Window Decorations `[Sonnet]`

Title bar: 32 px, Mica effect (from wallpaper tint), title text `FONT_UI_BOLD` 13 px left-aligned. Chrome buttons: ×/─/⬜(❐) via Fluent icon codepoints, 46×32 px, right-aligned. Hover: × = `#C42B1C` fill, others = `theme_get()->button_hover`. Resize handle: 4 px invisible edge margin mapped to `cursor_shape_t` via `wm_get_cursor_context`.

**Files:** `src/desktop/wm.c` (extend titlebar drawing)

> [!NOTE]
> Titlebar Mica: `gfx_mica(win_surface, 0, 0, win_w, 32, desktop_get_wallpaper_surface(), theme_get()->titlebar_active)` for focused window; use `theme_get()->titlebar_inactive` for unfocused. Title text: `gfx_draw_string(ttf_get(FONT_UI_BOLD, 13), title, 12, 10, theme_get()->foreground)`. Chrome button layout: `close_x = win_w - 46`, `max_x = win_w - 92`, `min_x = win_w - 138` (each 46 px wide). Button icon: render Fluent codepoints for ×=`\uE8BB`, ─=`\uE921`, ⬜=`\uE922`, ❐=`\uE923`. Hover detection: in `wm_handle_mouse()`, check if `(mx, my)` falls in each button rect. Close hover: `gfx_fill_rect(titlebar, close_x, 0, 46, 32, 0xFFC42B1C)`. Min/max hover: `gfx_fill_rect_alpha(titlebar, mx, 0, 46, 32, theme_get()->button_hover, 255)`. Resize handle: existing `wm_get_cursor_context()` already returns resize cursor shapes -- confirm it uses `WM_RESIZE_MARGIN = 5` and covers all 8 edge+corner directions.

- [ ] Mica titlebar: `gfx_mica()` called in `wm_composite()` per visible decorated window; active/inactive variants
- [ ] `FONT_UI_BOLD` 13 px title text left-aligned at `(12, 10)` within titlebar rect
- [ ] Chrome buttons: Fluent codepoints for ×/─/⬜/❐; maximized window: ⬜ → ❐ icon
- [ ] Hover fill: close=`#C42B1C`; min/max = `theme_get()->button_hover`; press = `theme_get()->button_pressed`
- [ ] Click routing: close button → `wm_destroy_window()`; min → `wm_minimize()`; max/restore → `wm_maximize()` or `wm_restore()`
- [ ] Resize cursor: confirm `wm_get_cursor_context()` handles all 8 directions at `WM_RESIZE_MARGIN` px
- [ ] Drop shadow: `gfx_drop_shadow(screen_surface, win_x-4, win_y-4, win_w+8, win_h+8, 8, WM_CORNER_RADIUS, 0, 4, theme_get()->shadow)` in compositor for focused window
- [ ] Commit: `"wm: decorations -- Mica titlebar, Fluent chrome buttons, hover states, drop shadow"`

## 3. Snap Layouts `[Sonnet]`

Hover maximize button → 4-zone layout popup (50/50 LR, 50/50 TB, 66/33 wide-narrow, 33/33/33). Click zone → snap. Win+← snap left half, Win+→ right half, Win+↑ maximize, Win+↓ restore/minimize. Edge-drag preview: semi-transparent zone overlay while dragging near screen edge.

**Files:** `src/desktop/wm_snap.c` (new), `include/desktop/wm.h` (extend)

> [!NOTE]
> Snap layout popup: when mouse enters the maximize button rect for > 300 ms (hover timer same as tooltip): create a borderless overlay window at z_order=9999 showing 4 zones as colored rectangles (120×80 px popup, 4 zone cells each 56×36 px with 4 px gap). Zone rects (usable area = screen minus taskbar): SNAP_LR_LEFT=`{0,0, uw/2, uh}`, SNAP_LR_RIGHT=`{uw/2,0, uw/2, uh}`, SNAP_WIDE=`{0,0, uw*2/3, uh}`, SNAP_NARROW=`{uw*2/3,0, uw/3, uh}`, SNAP_TOP=`{0,0, uw, uh/2}`, SNAP_BOTTOM=`{0,uh/2, uw, uh/2}`. `wm_snap(handle, zone_rect)`: save current rect; `wm_move/resize`; set `WM_FLAG_SNAPPED`; `wm_anim_snap(handle, side)`. Keyboard: register Win+←/→/↑/↓ in hotkey table (§5); dispatch to `wm_snap()` or `wm_maximize/restore`. Edge-drag preview: in `wm_handle_mouse()` while dragging, if cursor within 8 px of screen edge: render semi-transparent zone highlight; on drop → `wm_snap()`.

- [ ] `void wm_snap(int handle, int32_t zx, int32_t zy, uint32_t zw, uint32_t zh)` in `wm_snap.c`: save rect; move+resize; set `WM_FLAG_SNAPPED`; animate
- [ ] Snap layout popup: hover timer on maximize button; borderless overlay window; 4 zone cells; click zone → `wm_snap(handle, zone_rect)`; mouse-leave → close popup
- [ ] Edge-drag preview: in `wm_handle_mouse()` drag path: if `mx < 8` → highlight left-half zone; `mx > fb_w - 8` → right-half; `my < 8` → maximize zone; draw semi-transparent `gfx_fill_rect_alpha` overlay
- [ ] `wm_restore()` from snapped state: return to `saved_x/y/w/h` (same as from maximized)
- [ ] Keyboard snap: Win+← → left half; Win+→ → right half; Win+↑ → `wm_maximize`; Win+↓ → `wm_restore` (registered in §5 hotkey table)
- [ ] Commit: `"wm: snap layouts -- 4-zone hover popup, Win+arrow keyboard snap, edge-drag preview"`

## 4. Desktop Icons `[Sonnet]`

`struct desktop_icon` (name, path, icon_id, x, y). Load from `C:\Users\Default\Desktop\` at startup via `vfs_readdir()`. Draw icon (48 px) + label below. Single-click select, double-click launch via `task_create_user`. Drag to reposition with grid snap (16 px). Right-click → context menu (TODO-07). Registry persistence for icon positions.

**Files:** `src/desktop/desktop_icons.c` (new), `include/desktop/desktop_icons.h` (new), `src/desktop/desktop.c` (extend)

> [!NOTE]
> `struct desktop_icon { char name[128]; char path[256]; uint32_t icon_id; int32_t x, y; uint8_t selected; }`. Max 64 icons. Load: `vfs_readdir("C:\\Users\\Default\\Desktop\\", entries[], &count)`; for each entry: look up icon from `icon_store` by file extension (`.exe`→app icon, `.txt`→document icon, etc.); default position: grid layout 80×80 px starting from top-left. Draw: `icon_store_draw(icon_id, 48, icon_cx, icon_y)` then `gfx_draw_string(name, label_x, label_y, ...)` with `theme_get()->foreground`; selection: `gfx_fill_rect_alpha(sel_rect, theme_get()->selection, 100)`. Launch: `task_create_user(entry_point, name)` -- for now: `sys_exec(path)` wrapper. Registry: `HKCU\Software\Impossible\Shell\Desktop\Icons\{name}\X` + `Y` DWORD; read at load, write on drag-drop.

- [ ] `struct desktop_icon` + `desktop_icon_t` + max-64 array in `desktop_icons.c`
- [ ] `void desktop_icons_init(void)` -- `vfs_readdir("C:\\Users\\Default\\Desktop\\", ...)`; populate icon array; read Registry positions; called from `desktop_init()`
- [ ] `void desktop_icons_draw(gfx_surface_t *s)` -- draw all icons over wallpaper; called from `desktop_draw()` before `wm_composite()`
- [ ] Mouse: single-click → select (clear others); double-click → `desktop_icon_launch()`; click on empty → deselect all
- [ ] `desktop_icon_launch(icon)`: `task_create_user(NULL, icon->path)` -- spawns process with `icon->path` as argv[0]
- [ ] Drag: mouse-down on icon → set `dragging = 1`; mouse-move → update `icon->x/y` + redraw; mouse-up → grid snap `x = (x/16)*16`, `y = (y/16)*16`; save to Registry
- [ ] Grid snap on initial layout: columns from left `100 + col*80`; rows from top `100 + row*80`
- [ ] Commit: `"desktop: icons -- load from Desktop/, draw/select/launch, drag+16px grid snap, Registry"`

## 5. Keyboard Shortcuts & Task Switching `[Sonnet]`

Global hotkey dispatch table. Win+D (show-desktop toggle), Win+M (minimize all), Win+Shift+M (restore all), Win+L (lock screen stub), Alt+F4 (close focused), Win+←/→/↑/↓ (snap), Win+number (launch/focus pinned taskbar app by position).

**Files:** `src/desktop/hotkeys.c` (new), `include/desktop/hotkeys.h` (new), `src/desktop/desktop.c` (extend keyboard handler)

> [!NOTE]
> Hotkey dispatch: in the desktop keyboard handler (the main keyboard event loop), before passing keys to the focused window: check a `hotkey_table[]` for modifier+key combinations. `struct hotkey_entry { uint8_t modifiers; uint8_t scancode; void (*handler)(void); }`. Modifiers: `MOD_WIN=0x01`, `MOD_ALT=0x02`, `MOD_CTRL=0x04`, `MOD_SHIFT=0x08`. Win key detection: `scancode 0x5B` (left Win) sets `g_win_held = 1`; release clears it. Win+D: if any window is visible → minimize all + save "restored-set" list + `g_show_desktop=1`; if `g_show_desktop` → restore all from list; toggle. Win+M: iterate all windows; minimize non-minimized. Win+Shift+M: restore all minimized. Alt+F4: `wm_destroy_window(focused_handle)`. Win+number: look up taskbar pin at position N; if running → focus; else → launch.

- [ ] `struct hotkey_entry` + `hotkey_table[32]` + `hotkeys_init()` in `hotkeys.c`
- [ ] Win+D toggle: minimize-all + set `g_show_desktop`; second press restore from saved list
- [ ] Win+M / Win+Shift+M: iterate `wm_state.windows[]`; minimize/restore
- [ ] Win+L: `lock_screen()` stub -- clear framebuffer to black, draw "Press Enter to unlock" text
- [ ] Alt+F4: `wm_destroy_window(wm_get_focused())`
- [ ] Win+←/→/↑/↓: dispatch to `wm_snap()` / `wm_maximize()` / `wm_restore()` from §3
- [ ] Win+1..9: stub for taskbar pinned apps (no-op until TODO-08 taskbar provides the pin list)
- [ ] `int hotkeys_handle(uint8_t modifiers, uint8_t scancode)` → 1 if consumed, 0 if pass-through; called from desktop keyboard loop before focused window
- [ ] Commit: `"desktop: hotkeys -- Win+D/M/Shift+M/L, Alt+F4, Win+arrows, Win+number, dispatch table"`

## 6. Alt+Tab Task Switcher `[Opus]`

Centered overlay panel with 96×64 px window thumbnails from compositor back buffer. Focused thumbnail enlarged + drop shadow. Cycle with Alt+Tab / Alt+Shift+Tab. Release Alt → focus selected window. Animate overlay in/out 150 ms fade.

**Files:** `src/desktop/alttab.c` (new), `include/desktop/alttab.h` (new)

> [!NOTE]
> This is `[Opus]` -- the Alt+Tab switcher requires capturing per-window screenshots from the compositor back buffer, which has no prior implementation in Impossible OS. **Thumbnail capture**: in `wm_composite()`, after compositing each visible window: `memcpy(win->thumbnail, framebuffer_region, 96*64*4)` scaled from full window via `image_scale()`; store in `win->thumbnail[96*64]` buffer (allocated via `pmm_alloc_contiguous(96*64*4)` in `wm_create_window()`). Update only if window is not minimized and has been dirty since last thumbnail. **Overlay panel**: `wm_create_window(NULL, panel_x, panel_y, panel_w, 120, WM_FLAG_VISIBLE)` at z_order=20000; acrylic blur background; iterate visible non-minimized windows; draw thumbnails in a row; focused thumbnail scales to 108×72 with a drop shadow. Fade: `gfx_tween_start(&overlay_fade, 0, 255, 150, GFX_EASE_OUT_QUAD)`; `anim_mgr_add()`. Alt release detection: in keyboard handler, when `MOD_ALT` released while switcher open: `wm_focus_window(selected_handle)`; close overlay. Alt+Shift+Tab cycles backwards.

- [ ] Add `uint32_t *thumbnail; uint8_t thumb_dirty;` fields to `struct wm_window`; allocate `pmm_alloc_contiguous(96*64*4)` in `wm_create_window()`
- [ ] In `wm_composite()`: after compositing a dirty window, update its thumbnail via `image_scale(&thumb, &win_surface, 96, 64)`; set `thumb_dirty = 0`
- [ ] `void alttab_open(void)`: build visible window list; compute panel dimensions; create overlay window at z_order=20000; populate with thumbnails; `anim_mgr_add()` fade-in tween; `g_alttab_open = 1`
- [ ] `void alttab_cycle(int direction)`: `selected = (selected + direction + count) % count`; redraw overlay with enlarged selected thumbnail
- [ ] `void alttab_confirm(void)`: `wm_focus_window(windows[selected])`; `alttab_close()`
- [ ] `void alttab_close(void)`: `anim_mgr_add()` fade-out tween; `on_complete` → `wm_destroy_window(overlay_wh)` + free thumbnails; `g_alttab_open = 0`
- [ ] Hotkey wiring: in `hotkeys.c`: `MOD_ALT + Tab scancode (0x0F)` → `alttab_open()` or `alttab_cycle(+1)`; `MOD_ALT + MOD_SHIFT + Tab` → `alttab_cycle(-1)` ; Alt release → `alttab_confirm()`
- [ ] Overlay layout: thumbnails in a row, 112 px apart; focused: 108×72 + 4 px `gfx_drop_shadow`; panel width = `min(count * 112 + 32, screen_w - 64)`; centered horizontally + vertically
- [ ] Commit: `"desktop: Alt+Tab switcher -- compositor thumbnails, overlay panel, fade, focus on release"`

## 7. Drag and Drop `[Opus]`

Intra-desktop: drag desktop icon to reposition (§4 extends). File drag from File Manager to desktop (create shortcut/copy). Window title-bar drag → move (exists, verify). Window edge/corner drag → resize (exists, verify). Visual drag ghost: semi-transparent copy of the dragged element rendered at cursor position.

**Files:** `src/desktop/dragdrop.c` (new), `include/desktop/dragdrop.h` (new), `src/desktop/wm.c` (extend)

> [!NOTE]
> This is `[Opus]` -- drag-and-drop requires a novel cross-window drag protocol: the drag source (File Manager window) must communicate file path(s) to the drag target (desktop), and the compositor must render a floating drag ghost at the mouse position -- neither of which exist in Impossible OS. **Drag ghost**: a borderless window at z_order=15000 containing a scaled-down icon or file thumbnail; created at drag-start, follows mouse on every `wm_handle_mouse()` call. **Drag protocol**: `drag_start(source_wh, type, payload, payload_len)` registers a drag operation with type `DRAG_FILES`/`DRAG_ICON`/`DRAG_WINDOW`; `drag_drop(target_wh, x, y)` delivers the payload to `target_wh` via `drag_on_drop(target_wh, type, payload, x, y)` callback. **File drag from File Manager**: File Manager calls `drag_start(fm_wh, DRAG_FILES, paths[], count)` on mouse-down on selected items; if dropped on desktop: `desktop_icons_create_shortcut(path, drop_x, drop_y)`. Window move (title-bar drag) and resize (edge drag) already work via `wm_handle_mouse()`; verify they use the `wm_get_drag_dirty_rect()` path for optimization.

- [ ] `typedef struct { int type; int source_wh; uint8_t payload[512]; uint32_t payload_len; int ghost_wh; } drag_state_t;` in `dragdrop.h`
- [ ] `void drag_start(int source_wh, int type, const void *payload, uint32_t len)`: create ghost window at cursor; set `g_drag.active = 1`
- [ ] Ghost window: `wm_create_window(NULL, mx-24, my-24, 48, 48, WM_FLAG_VISIBLE)` at z_order=15000; draw icon at 50% opacity; updated on mouse-move
- [ ] `void drag_tick(int32_t mx, int32_t my)`: called from `wm_handle_mouse()` while drag active; move ghost window to `(mx-24, my-24)`; highlight drop target window (`wm_window_at(mx, my)`)
- [ ] `void drag_end(int32_t mx, int32_t my)`: destroy ghost; compute target = `wm_window_at(mx, my)`; if target is desktop → `desktop_drop_handler(type, payload, mx, my)`; else → `drag_on_drop(target_wh, type, payload, mx, my)`; clear `g_drag.active`
- [ ] Desktop drop handler: `DRAG_FILES` → for each path: `desktop_icons_create_shortcut(path, drop_x, drop_y)`
- [ ] Verify window title-bar drag and edge-resize use `wm_get_drag_dirty_rect()` optimization path; add if missing
- [ ] Commit: `"desktop: drag+drop -- ghost window, file drag to desktop, drag_start/end/tick protocol"`

## 8. Compositor Performance `[Opus]`

Dirty-rect union: only re-composite screen regions touched by changed windows. Frame skip: if no dirty rects and no active animations (`!anim_mgr_any_active()`), skip `wm_composite()` entirely. Measure frame time: warn if > 16 ms (60 fps threshold) to serial.

**Files:** `src/desktop/wm.c` (extend `wm_composite`)

> [!NOTE]
> This is `[Opus]` -- the dirty-rect union algorithm requires tracking a bounding box that grows as multiple windows change, then only re-compositing that region instead of the full framebuffer. Novel for Impossible OS. **Dirty rect tracking**: `static int32_t dirty_x, dirty_y; static uint32_t dirty_w, dirty_h; static uint8_t dirty_full;`. `wm_mark_dirty()`: sets `dirty_full = 1` (full redraw needed). Per-window: `wm_mark_window_dirty(handle)`: compute old + new window rect union; expand `(dirty_x, dirty_y, dirty_w, dirty_h)` to include it. **Composite gate**: `if (!dirty_full && dirty_w == 0 && !anim_mgr_any_active()) return;` at top of `wm_composite()`. **Partial composite**: if `!dirty_full && dirty_w > 0`: only re-composite windows that overlap `(dirty_x, dirty_y, dirty_w, dirty_h)`. **Frame time**: `uint64_t t0 = system_get_ticks()` before composite; `uint64_t elapsed_ms = (system_get_ticks() - t0) * 10` after; if `elapsed_ms > 16`: `klog(LOG_WARN, "wm", "compositor: frame %u ms > 16 ms threshold", elapsed_ms)`. Clear dirty state at end of each composite call.

- [ ] `static int32_t g_dirty_x, g_dirty_y; static uint32_t g_dirty_w, g_dirty_h; static uint8_t g_dirty_full;` in `wm.c`
- [ ] `void wm_mark_window_dirty(int handle)`: expand dirty rect to union of old+new window bounds; set `g_dirty_w > 0`
- [ ] `wm_mark_dirty()`: sets `g_dirty_full = 1` (full redraw; existing callers unchanged)
- [ ] `wm_composite()` gate: early return if `!g_dirty_full && g_dirty_w == 0 && !anim_mgr_any_active()`
- [ ] Partial composite path: if `!g_dirty_full`: clip compositor to `(g_dirty_x, g_dirty_y, g_dirty_w, g_dirty_h)` -- only blit windows that overlap; skip others
- [ ] Frame time: `system_get_ticks()` before/after composite; if `Δ × 10 > 16`: serial warn; log max observed frame time per 100 frames
- [ ] Per-frame `spinner_tick()` over the active-spinner list, so a multi-instance spinner pool has a compositor driver
  - -> XREF: `01-boot-platform/TODO-14-boot-diagnostics.md` §7 (item: "Compositor integration: WM maintains a `spinner_t *g_active_spinners[8]` list" at line 266)
- [ ] Reset: `g_dirty_w = g_dirty_h = 0; g_dirty_full = 0` at end of `wm_composite()`
- [ ] Commit: `"wm: compositor perf -- dirty-rect union, frame-skip when idle, 16 ms frame-time warning"`

---

## OS Comparison


| ⭐  | Feature                     | 🪟 Win11                                                              | 🐧 Linux                                                           | 🚀 Impossible OS                                                               |
| --- | --------------------------- | --------------------------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------------------ |
| 💎  | Min/max/restore             | ✅ DWM animated; `WM_SYSCOMMAND SC_MINIMIZE/MAXIMIZE/RESTORE`; window | ✅ Mutter/KWin animated; window state via                          | ⬜ §1 `WM_FLAG_MINIMIZED/MAXIMIZED`, `saved_x/y/w/h`, Registry `{title}\State` |
| 💎  | Mica titlebar               | ✅ DWM Mica material; `DWMWA_USE_IMMERSIVE_DARK_MODE`; Fluent         | ⚠️ KDE Breeze blur titlebar; GNOME                                 | ⬜ §2 -- `gfx_mica()` per window titlebar; Fluent                              |
| 💎  | Snap layouts                | ✅ Windows 11 Snap Layouts; PowerToys                                 | ✅ KWin tiling; GNOME extension snap;                              | ⬜ §3 -- hover-timer popup at maximize button                                  |
| 💎  | Desktop icons               | ✅ Windows desktop icons; drag reposition;                            | ✅ GNOME/KDE desktop icons; drag reposition;                       | ⬜ §4 -- `vfs_readdir("C:\\Users\\Default\\Desktop\\")`; 16 px grid snap       |
| 💎  | Global hotkeys              | ✅ All these hotkeys built into                                       | ✅ GNOME/KDE global hotkey service; `xbindkeys`/`ydotool`          | ⬜ §5 -- kernel-level `hotkey_table[]` dispatch before focused-window          |
| 💎  | Alt+Tab                     | ✅ DWM live thumbnails; Alt+Tab overlay;                              | ✅ Mutter/KWin Alt+Tab with live window                            | ⬜ §6 -- compositor thumbnail per window; 96×64                                |
| ⭐  | Drag-and-drop ghost         | ✅ Windows drag ghost (DragDrop COM                                   | ✅ GTK `GtkDragSource`; X11 `XdndEnter`; drag                      | ⬜ §7 -- `⭐` kernel-native: borderless z_order=15000 ghost                    |
| ⭐  | Compositor dirty-rect union | ✅ DWM dirty-region tracking (hardware-accelerated; GPU               | ✅ Mutter/KWin damage tracking; Wayland `wl_surface.damage_buffer` | ⬜ §8 -- `⭐` software dirty-rect union with                                   |

> **After §1–§8:** Impossible OS has a Windows 11-quality WM with zero GPU dependency. The `⭐` dirty-rect compositor skips entire frames when idle -- a parity with DWM's GPU damage tracking but implemented in pure software, meaning it works on any framebuffer hardware with no GPU driver required. The `⭐` drag ghost uses a real WM overlay window at z_order=15000, which means it correctly composites above all app windows without any special compositor path.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Minimize: click minimize button → window animates to bottom of screen, disappears; `WM_FLAG_MINIMIZED` set; window not composited
- [ ] Maximize: click maximize button → window fills screen minus 48 px taskbar area; saved rect stored; button changes to ❐ restore icon
- [ ] Restore: click restore button → window returns to saved size/position with animation
- [ ] Mica: titlebar shows wallpaper blur tint; focused window brighter than unfocused window
- [ ] Snap layouts: hover maximize button 300 ms → 4-zone popup appears; click left zone → window snaps to left half; Win+→ snaps to right half
- [ ] Desktop icons: `C:\Users\Default\Desktop\` with a `.txt` file → icon appears on desktop; double-click → launches process; drag icon 80 px right → snaps to grid; position saved in Registry
- [ ] Win+D: all windows minimize; second Win+D → all restore; serial log shows `[hotkeys] Win+D: show-desktop toggle`
- [ ] Alt+Tab: 3 windows open; Alt+Tab → overlay appears with 3 thumbnails; Tab cycles selection; release Alt → focus moves to selected window; overlay fades out
- [ ] File drag from File Manager (future): `drag_start()` with `DRAG_FILES`; ghost window follows cursor; drop on desktop → shortcut created
- [ ] Idle compositor: no windows moving, no animations → `wm_composite()` early-returns without redraw; CPU usage near 0%; serial log confirms `[wm] frame skip: no dirty`
- [ ] Frame time: `wm_composite()` completes in < 16 ms for a typical 5-window desktop; any slow frame logs warning to serial
- [ ] Commit: `"wm: complete WM enhancements -- decorations, min/max, snap, desktop icons, hotkeys, Alt+Tab, drag+drop, dirty-rect"`
