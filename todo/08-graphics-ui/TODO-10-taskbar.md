---
schema_version: 1
id: taskbar
domain: 08-graphics-ui
status: active
title: "TODO-10 -- Taskbar"
---

# TODO-10 -- Taskbar

> **Goal:** Build the full taskbar -- window list with active/flash/progress, right-click context menus, Aero Peek window preview, progress badges with a user-mode syscall, pinned app launchers, jump lists, auto-hide, and taskbar customization (position/size). This makes the taskbar the primary desktop chrome it must be, matching Windows 11 feature-for-feature.

> [!IMPORTANT]
> **Already exists**: `TASKBAR_HEIGHT=48` + `desktop_draw_taskbar()` + `desktop_in_taskbar()` in `desktop.h` -- a basic stub that draws the bar background and a Start button. `gfx_blit_alpha(dst, dx, dy, src, sx, sy, sw, sh, alpha)` for Aero Peek opacity. `task_exec(data, size)` and `task_create_user(entry, name)` for launching pinned apps. Context menu engine `context_menu_show()` (TODO-07 §1) must exist before §2 and §6. `WM_DPI_CHANGED` + `DPI_SCALE()` (TODO-07 §4) for §8 sizing. Animation engine `anim_mgr_add()` / `GFX_EASE_IN/OUT_CUBIC` (TODO-02) for §7 auto-hide. **Missing**: window list struct, progress badge, peek, pins, jump lists, auto-hide, customization. Complete sections in order: window list → pinned apps → context menu → progress badges → Aero Peek → jump lists → auto-hide → customization.

## Inputs

- `include/desktop/desktop.h` -- `TASKBAR_HEIGHT`, `desktop_draw_taskbar()`, `desktop_in_taskbar()` -- extended throughout
- `include/desktop/wm.h` -- `wm_window`, `wm_focus_window()`, `wm_minimize()`, `wm_restore()`, `wm_maximize()`, `wm_post_message_all()` -- used by §1 window list and §3 Aero Peek
- `include/gfx.h` -- `gfx_blit_alpha()`, `gfx_fill_rounded_rect()`, `gfx_drop_shadow()` -- used by §1, §3 peek, §6 jump list popup
- `include/kernel/sched/syscall.h` -- `SYS_MSGBOX=51`; `SYS_TASKBAR_SET_PROGRESS=52`, `SYS_JUMPLIST_NOTIFY=53` added in §4+§6
- `include/kernel/sched/task.h` -- `task_exec(data, size)` -- used by §5 pinned app launch
- `include/registry.h` -- `RegGetValue/SetValueEx` -- used by §5 pins, §6 jump lists, §7 auto-hide, §8 customization
- `include/kernel/gfx/anim_mgr.h` (TODO-02) -- `anim_mgr_add()`, `GFX_EASE_IN_CUBIC/OUT_CUBIC` -- used by §7 auto-hide slide tween
- `include/desktop/context_menu.h` (TODO-07 §1) -- `context_menu_show()` -- used by §2 and §7
- `include/desktop/dpi.h` (TODO-07 §4) -- `DPI_SCALE()`, `WM_DPI_CHANGED` -- used by §8 sizing
- → XREF: `08-graphics-ui/TODO-08-window-manager.md` -- §1 window list wires to `wm_minimize/maximize/restore`; §3 Aero Peek temporarily overrides compositor window opacity
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §1` -- context menu engine must exist before §2 right-click and §7 jump list popup; §9 vdesk present for "Move to Desktop ►"
- Related (no stable XREF target): `09-desktop-shell/TODO-01-*` (notifications/TODO-09) -- §5 progress badge toast is a forward ref; use serial log stub until TODO-09 live

## Outcome

- `struct taskbar_entry` drives window buttons (icon + title, active underline, flash animation, progress bar).
- Right-click any window button → context menu with Close / Restore / Minimize / Move to Desktop.
- Hover 500 ms over window button → Aero Peek (all other windows dimmed to 10%); "Show Desktop" strip.
- `taskbar_set_progress(win, pct, state)` + `SYS_TASKBAR_SET_PROGRESS=52` syscall for user-mode apps.
- Pinned app launchers at left of window list; running+pinned → combined; Registry-persisted.
- Jump list popup above right-click menu; `SYS_JUMPLIST_NOTIFY=53` syscall.
- Auto-hide: taskbar slides off-screen when unused; slides back on mouse-at-edge.
- Taskbar position (bottom/top/left/right), size (small/medium/large), and pin manager in Control Panel.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                   | Depends On                                                                 | Status |
| --- | :---: | --------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Window list -- `taskbar_entry`, add/remove/active wired from WM events, flash, draw        | `wm_window`, `wm_focus/minimize` (TODO-06) must exist                     |  [ ]   |
| 💎  |   2   | §5 Pinned apps -- load/save Registry pins, icon-only buttons, combined when running           | §1 window list (combined button logic)                                     |  [ ]   |
| 💎  |   3   | §2 Context menu -- right-click window button → Close/Restore/Minimize/Move to Desktop         | §1 window list + §5 pins; `context_menu_show()` (TODO-07 §1)              |  [ ]   |
| 💎  |   4   | §4 Progress badges -- `taskbar_set_progress()`, thin bar at icon bottom, `SYS_TASKBAR_SET_PROGRESS=52` | §1 window list (drawn on window buttons)                           |  [ ]   |
| ⭐  |   5   | §3 Aero Peek -- 500 ms hover, compositor opacity override, "Show Desktop" strip               | §1 window list; `gfx_blit_alpha`; `wm_composite` modification (TODO-06)   |  [ ]   |
| 💎  |   6   | §6 Jump lists -- recent files popup above context menu, `SYS_JUMPLIST_NOTIFY=53`             | §2 context menu (jump list is shown above it); §5 pins (per-app Registry) |  [ ]   |
| 💎  |   7   | §7 Auto-hide -- slide off/on with anim_mgr tween, mouse proximity trigger                     | §1–§6 stable; TODO-02 `anim_mgr_add()` must exist                        |  [ ]   |
| 💎  |   8   | §8 Customization -- position/size/pins Registry, `DPI_SCALE()` sizing, Control Panel tab     | §7 (auto-hide is a customization option too); TODO-07 §4 DPI              |  [ ]   |

---

## 1. Taskbar Window List `[Sonnet]`

`struct taskbar_entry` (window ptr, title, icon_id, active flag, flash flag, progress_pct, progress_state). `taskbar_add_window(win)` / `taskbar_remove_window(win)` / `taskbar_set_active(win)` wired from WM events. Draw window buttons between Start and system tray. Click → focus/raise; click active → minimize. `taskbar_flash(win)` blinks 3×.

**Files:** `src/desktop/taskbar_winlist.c` (new), `include/desktop/taskbar.h` (new), `src/desktop/desktop.c` (extend)

> [!NOTE]
> Window button layout: left of Start button = pinned apps (§5); right of Start = window buttons; right edge = system tray + clock. Each button: `DPI_SCALE(120)` px wide × full taskbar height; icon 16 px at left + title string, truncated with `…` if text overflows at 88 px remaining. **Active button**: 2 px accent-color underline at bottom of button (`theme_get()->accent`). **Hover**: `theme_get()->button_hover` fill. **Flash**: `taskbar_flash(win)`: toggle `entry->active` flag 3× at 500 ms interval using a 1 s periodic timer; blink accent underline + orange highlight (`0xFFE06C00`). **WM wiring**: call `taskbar_add_window(win)` from `wm_create_window()`; `taskbar_remove_window(win)` from `wm_destroy_window()`; `taskbar_set_active(win)` from `wm_focus_window()`. Windows with `WM_FLAG_MINIMIZED` should still appear in list but without active underline.

- [ ] `typedef struct { wm_window_t *win; char title[128]; uint32_t icon_id; uint8_t active; uint8_t flash_count; uint8_t flash_phase; uint8_t progress_state; uint8_t progress_pct; } taskbar_entry_t;` in `taskbar.h`
- [ ] `#define TASKBAR_MAX_ENTRIES 64` -- static array; no dynamic alloc for window list
- [ ] `void taskbar_add_window(wm_window_t *win)` -- find free slot; copy title from `win->title`; copy `win->icon_id`
- [ ] `void taskbar_remove_window(wm_window_t *win)` -- find entry by `win` ptr; zero slot
- [ ] `void taskbar_set_active(wm_window_t *win)` -- clear `active` on all entries; set `active=1` on matching entry
- [ ] `void taskbar_flash(wm_window_t *win)` -- set `flash_count=6` (3 blink pairs); `flash_phase=0`; periodic timer drives toggle
- [ ] `void taskbar_tick(void)` -- called from `desktop_tick()`: advance flash timers; rebuild draw list
- [ ] `void taskbar_draw_winlist(gfx_surface_t *s, int32_t x, int32_t w)` -- draw all window buttons in region [x, x+w]
- [ ] Click handler: find entry by mouse x; if `entry->active` → `wm_minimize(win)`; else → `wm_restore(win)` + `wm_focus_window(win)`
- [ ] Commit: `"taskbar: window list -- taskbar_entry, add/remove/active/flash wired from WM events"`

## 2. Taskbar Button Context Menu `[Sonnet]`

Right-click window button → `context_menu_show()`: Close, Restore/Maximize (toggle), Minimize, *(stretch)* "Move to Desktop ►" submenu (virtual desktops), *(stretch)* "Pin to taskbar".

**Files:** `src/desktop/taskbar_winlist.c` (extend), `src/desktop/taskbar_ctxmenu.c` (new)

> [!NOTE]
> Detect right-click on window button in `desktop_handle_mouse()`. Menu items are dynamic: if window is maximized → show "Restore"; else → show "Maximize". If already minimized → hide "Minimize". "Move to Desktop ►" submenu: list `vdesk_name(i)` for each active virtual desktop (TODO-07 §9); callback → `vdesk_move_window(win, i)`. "Pin to taskbar" toggles `entry->pinned`; persist to Registry. All items wire to `wm_*` functions.

- [ ] `void taskbar_show_win_menu(taskbar_entry_t *entry, int32_t mx, int32_t my)` -- build `menu_item[]` array based on window state; call `context_menu_show(mx, my, items, count)`
- [ ] Menu items: `Close` → `wm_destroy_window(entry->win)`; `Restore/Maximize` → toggle; `Minimize` → `wm_minimize(entry->win)` (hide if already minimized)
- [ ] "Move to Desktop ►": submenu of desktop names; callback `vdesk_move_window(entry->win, idx)` (stub if §7 vdesks not live)
- [ ] "Pin to taskbar": toggle; persists pin path to `HKCU\Software\Impossible\Shell\TaskbarPins`
- [ ] "Unpin from taskbar" shown instead of "Pin" if entry is pinned
- [ ] Right-click detection in `desktop_handle_mouse()`: if right-button and hit-tests a window button → `taskbar_show_win_menu(entry, mx, my)`
- [ ] Commit: `"taskbar: window button context menu -- close/restore/maximize/minimize/pin/move-to-desktop"`

## 3. Aero Peek `[Opus]`

Hover window button 500 ms → all other windows blitted at alpha=25 (10%) in compositor. Hovered window stays at 255. Mouse-leave → restore all. "Show Desktop" strip (12 px right edge): hover = peek all, click = toggle Win+D. Registry `EnablePeek` (default 1).

**Files:** `src/desktop/taskbar_peek.c` (new), `include/desktop/wm.h` (extend), `src/desktop/wm.c` (extend compositor)

> [!NOTE]
> This is `[Opus]` -- Aero Peek requires a novel per-window temporary opacity field in the compositor. Currently `wm_composite()` blits all visible windows at full opacity. The change: add `uint8_t peek_alpha` to `struct wm_window` (default 255); compositor: use `gfx_blit_alpha(dst, win->x, win->y, &win_surf, 0, 0, win->w, win->h, win->peek_alpha)` instead of full-opacity blit. On peek start: iterate all entries except the hovered window; set `win->peek_alpha = 25`; `wm_mark_dirty()`. On peek end: restore all `win->peek_alpha = 255`. **500 ms hover timer**: in `taskbar_tick()` per entry: increment `hover_ms` by tick delta; if `hover_ms >= 500` and not yet in peek: `taskbar_peek_start(entry)`. On mouse-leave: `hover_ms = 0`; `taskbar_peek_end()`. **Show Desktop strip**: 12 px strip at far-right; hover → peek all windows (no hovered exception); click → `wm_toggle_show_desktop()` (minimizes all, toggle restores). The compositor `gfx_blit_alpha` call replaces the unconditional `fb_blit` per window -- this is the core architectural change.

- [ ] `uint8_t peek_alpha` field in `struct wm_window` (default 255); `uint16_t hover_ms` in `taskbar_entry_t`
- [ ] `void taskbar_peek_start(taskbar_entry_t *peeked_entry)`: set `win->peek_alpha = 25` for all windows except `peeked_entry->win`; `wm_mark_dirty()`
- [ ] `void taskbar_peek_end(void)`: restore all windows to `peek_alpha = 255`; `wm_mark_dirty()`
- [ ] `wm_composite()`: per window blit: `if (win->peek_alpha < 255) gfx_blit_alpha(..., win->peek_alpha); else fb_blit_direct(...)`
- [ ] `taskbar_tick()` hover timer: accumulate `hover_ms` while mouse over button; at 500 ms → `taskbar_peek_start()`; mouse leave → reset + `taskbar_peek_end()`
- [ ] "Show Desktop" strip: 12 px at right edge of taskbar; draw with `theme_get()->border` fill; hover → full-peek all windows; click → `wm_toggle_show_desktop()`
- [ ] `void wm_toggle_show_desktop(void)`: minimize all non-minimized windows (save state); second call restores them; `g_show_desktop_active` toggle flag
- [ ] `int g_peek_enabled` from `HKCU\Software\Impossible\Shell\EnablePeek` (default 1); if 0: skip hover timer + draw "Show Desktop" strip without peek behavior
- [ ] Commit: `"taskbar: Aero Peek -- per-window peek_alpha in compositor, 500ms hover, Show Desktop strip"`

## 4. Taskbar Progress Badges `[Sonnet]`

`taskbar_set_progress(win, pct, state)` draws a thin 3 px bar at the bottom of the window button icon. States: `TASKBAR_PROGRESS_NORMAL` (green), `TASKBAR_PROGRESS_PAUSED` (yellow), `TASKBAR_PROGRESS_ERROR` (red), `TASKBAR_PROGRESS_NONE` (hidden). `SYS_TASKBAR_SET_PROGRESS=52` syscall for user-mode apps.

**Files:** `src/desktop/taskbar_winlist.c` (extend), `include/desktop/taskbar.h` (extend), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> The progress bar is a 3 px tall strip at the very bottom of the window button, spanning `(pct * button_w / 100)` pixels. Color map: `NORMAL=0xFF16C60C` (green), `PAUSED=0xFFF9F1A5` (yellow), `ERROR=0xFFE81123` (red). State is stored in `entry->progress_pct` (0–100) and `entry->progress_state`. `taskbar_draw_winlist()` checks `entry->progress_state != TASKBAR_PROGRESS_NONE` and draws the strip after the button fill. **Syscall**: `sys_taskbar_set_progress(pid, pct, state)` -- kernel side looks up the window belonging to `pid` via `task_get_window(pid)`, then calls `taskbar_set_progress(win, pct, state)`.

- [ ] `#define TASKBAR_PROGRESS_NONE    0`, `NORMAL=1`, `PAUSED=2`, `ERROR=3` in `taskbar.h`
- [ ] `void taskbar_set_progress(wm_window_t *win, uint8_t pct, uint8_t state)` -- find entry by win; update `progress_pct/state`; `wm_mark_dirty()`
- [ ] Progress bar draw in `taskbar_draw_winlist()`: after button fill; `gfx_fill_rect(s, btn_x, btn_bottom-3, pct*btn_w/100, 3, color_for_state)`
- [ ] `#define SYS_TASKBAR_SET_PROGRESS 52` in `syscall.h`
- [ ] `sys_taskbar_set_progress(int pid, int pct, int state)` kernel handler: `task_get_main_window(pid)` → `taskbar_set_progress(win, pct, state)`
- [ ] Wire `SYS_TASKBAR_SET_PROGRESS` into syscall dispatch table in `src/kernel/sched/syscall.c`
- [ ] Commit: `"taskbar: progress badges -- NORMAL/PAUSED/ERROR states, 3px bar, SYS_TASKBAR_SET_PROGRESS=52"`

## 5. Pinned Apps `[Sonnet]`

Load from `HKCU\Software\Impossible\Shell\TaskbarPins` (comma-separated paths) at boot. Render pin icons left of window list. Running + pinned = combined button. Not-running = icon-only → click → `task_exec()`. Right-click pinned → "Unpin from taskbar".

**Files:** `src/desktop/taskbar_pins.c` (new), `include/desktop/taskbar.h` (extend)

> [!NOTE]
> Pinned entries are a separate `taskbar_pin_t[]` array (max 16 pins). At startup, `taskbar_pins_load()` reads the Registry CSV; for each path, load the app's icon (try `C:\\path_dir\\icon.ires` → `ires_get_icon()`; fallback to generic app icon). When a pinned app's window appears (matched by `win->title` vs pin path basename), `entry->pinned = 1` and the window button draws at the pin's position. Not-running pin: draws icon + 1 px dot at bottom center (like Windows 11 pinned-not-running indicator). Running pin: draws the window button normally at the pinned slot position -- no separate icon entry.

- [ ] `typedef struct { char path[256]; uint32_t icon_id; wm_window_t *running_win; } taskbar_pin_t;` in `taskbar.h`
- [ ] `#define TASKBAR_MAX_PINS 16` -- static array
- [ ] `void taskbar_pins_load(void)`: `RegGetValue("HKCU\\...\\TaskbarPins")` → parse CSV into `g_pins[]`; load icon per entry
- [ ] `void taskbar_pins_save(void)`: serialize `g_pins[].path` to CSV → `RegSetValueEx()`
- [ ] `void taskbar_pin_add(const char *path)`: find free slot; load icon; save
- [ ] `void taskbar_pin_remove(const char *path)`: zero slot; save
- [ ] Pin draw: if `pin->running_win == NULL`: icon + small dot indicator; click → `task_exec(load_file(pin->path), size)`; right-click → "Unpin from taskbar" + "Open" items
- [ ] Pin+running merge: when `taskbar_add_window(win)` finds a matching pin path → link `pin->running_win = win`; skip creating a separate window button slot; draw merged button at pin position
- [ ] Match heuristic: compare `basename(pin->path)` vs `win->title` (case-insensitive); secondary: `win->exe_path` if struct supports it
- [ ] Commit: `"taskbar: pinned apps -- Registry CSV pins, icon-only launch, combined running+pin button"`

## 6. Jump Lists `[Sonnet]`

Right-click pinned or window button → jump list popup above context menu showing recent files / frequent tasks. Read from `HKCU\Software\Impossible\Shell\JumpLists\{app_name}\Recent[]`. `SYS_JUMPLIST_NOTIFY=53` syscall for apps to register recent files.

**Files:** `src/desktop/taskbar_jumplist.c` (new), `include/desktop/taskbar.h` (extend), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> Jump list popup is a separate overlay window at z_order=26000 (above context menu at 25000 from TODO-07 §1). Layout: dark acrylic panel; sections: "Recent" (up to 10 items, each with icon + filename); "Pinned" (manually pinned items -- stub); "Tasks" (app-defined, stub). Popup appears immediately above the context menu if a jump list exists for the app; if empty, skip popup and show only the context menu. **Syscall**: `sys_jumplist_notify(const char *path, uint32_t len)` -- kernel side: `current_task->name` as key; write path to `HKCU\Software\Impossible\Shell\JumpLists\{name}\Recent[]` (ring buffer of 10 entries).

- [ ] `#define SYS_JUMPLIST_NOTIFY 53` in `syscall.h`
- [ ] `sys_jumplist_notify(const char *path, uint32_t len)` kernel handler: validate path; append to Registry ring buffer (max 10 entries); `RegSetValueEx("RecentN", path)` + `RegSetValueEx("RecentCount", count)`
- [ ] `void jumplist_load(const char *app_name, char paths[][256], int *count)`: read up to 10 entries from Registry
- [ ] `void jumplist_show(const char *app_name, int32_t btn_x, int32_t btn_y)`: load entries; if count == 0 return (no popup); create overlay window at z_order=26000 above context menu position; draw acrylic panel + items
- [ ] Item click: `task_exec(load_file(path), size)` to open the file in its app; close jump list
- [ ] `taskbar_show_win_menu()` (§2): call `jumplist_show(app_name, ...)` before `context_menu_show()` if app_name has entries; position jump list directly above the context menu top
- [ ] Wire `SYS_JUMPLIST_NOTIFY` into syscall dispatch table
- [ ] Commit: `"taskbar: jump lists -- SYS_JUMPLIST_NOTIFY=53, Registry ring buffer, popup above context menu"`

## 7. Taskbar Auto-Hide `[Sonnet]`

`HKCU\Software\Impossible\Shell\TaskbarAutoHide`: slide off-screen when mouse is not near the bottom edge (150 ms ease-in via `anim_mgr`). Slide back when mouse moves to the screen bottom row (100 ms ease-out).

**Files:** `src/desktop/taskbar_autohide.c` (new), `include/desktop/taskbar.h` (extend)

> [!NOTE]
> Auto-hide uses a `gfx_tween_t g_taskbar_y_tween` that animates `g_taskbar_offset` (0 = fully visible, `TASKBAR_HEIGHT` = fully hidden). The compositor and `desktop_in_taskbar()` check `g_taskbar_offset` to position the bar correctly. **Trigger**: in `desktop_handle_mouse()`: if mouse y >= `fb_get_height() - 4` (bottom 4 px proximity strip): if `g_taskbar_hidden`: start slide-in tween. If mouse y < `fb_get_height() - TASKBAR_HEIGHT - 16` and no open window over taskbar: start slide-out tween after 500 ms idle. **Desktop height**: when auto-hide is enabled, `desktop_get_usable_height()` returns `fb_get_height()` (full screen available) so windows can use the full area.

- [ ] `int g_taskbar_autohide` (0=off, 1=on) + `int g_taskbar_hidden` + `gfx_tween_t g_taskbar_y_tween` in `taskbar_autohide.c`
- [ ] `void taskbar_autohide_init(void)`: read `HKCU\Software\Impossible\Shell\TaskbarAutoHide` DWORD
- [ ] `void taskbar_autohide_show(void)`: `anim_mgr_add(&g_taskbar_y_tween, TASKBAR_HEIGHT, 0, 100, GFX_EASE_OUT_CUBIC, NULL)`
- [ ] `void taskbar_autohide_hide(void)`: `anim_mgr_add(&g_taskbar_y_tween, 0, TASKBAR_HEIGHT, 150, GFX_EASE_IN_CUBIC, NULL)`
- [ ] Mouse proximity: in `desktop_handle_mouse()`: if autohide enabled and `my >= fb_h - 4` → `taskbar_autohide_show()`; else if `my < fb_h - tb_h - 16` and `g_taskbar_hidden` → start 500 ms idle countdown → `taskbar_autohide_hide()`
- [ ] Compositor: `taskbar_y = fb_h - TASKBAR_HEIGHT + g_taskbar_y_tween.current`; draw taskbar at offset y
- [ ] `desktop_get_usable_height()`: if autohide enabled → return `fb_get_height()`; else → `fb_get_height() - TASKBAR_HEIGHT`
- [ ] Context menus / jump lists open above taskbar: auto-show taskbar while any popup is open
- [ ] Commit: `"taskbar: auto-hide -- anim_mgr slide in/out, proximity trigger, full usable height when hidden"`

## 8. Taskbar Customization `[Sonnet]`

`HKCU\Software\Impossible\Shell\TaskbarPosition` (bottom/top/left/right), `TaskbarSize` (small=32/medium=40/large=48). `DPI_SCALE()` applied to all sizing. Display Control Panel "Taskbar" tab: position picker, size picker, pin manager.

**Files:** `src/desktop/taskbar_config.c` (new), `include/desktop/taskbar.h` (extend)

> [!NOTE]
> `taskbar_position_t` enum: `TB_BOTTOM=0`, `TB_TOP=1`, `TB_LEFT=2`, `TB_RIGHT=3`. `taskbar_size_t`: `TB_SMALL=0` (32 px), `TB_MEDIUM=1` (40 px), `TB_LARGE=2` (48 px). Position affects: where `desktop_in_taskbar()` tests; where the taskbar window is drawn; which edge auto-hide slides toward. For left/right positions: window buttons are stacked vertically; titles hidden (icon only) at TB_SMALL. `DPI_SCALE()` multiplies the base pixel sizes: `TASKBAR_HEIGHT = DPI_SCALE(tb_base_height)`. On setting change: `wm_post_message_all(WM_DPI_CHANGED, ...)` to trigger re-layout (reuses DPI changed path since both cause resize of all windows).

- [ ] `typedef enum { TB_BOTTOM=0, TB_TOP=1, TB_LEFT=2, TB_RIGHT=3 } taskbar_position_t;`
- [ ] `typedef enum { TB_SMALL=0, TB_MEDIUM=1, TB_LARGE=2 } taskbar_size_t;`
- [ ] `int g_taskbar_base_h` (32/40/48 per size) + `int g_taskbar_pos`; `TASKBAR_HEIGHT` becomes `DPI_SCALE(g_taskbar_base_h)`
- [ ] `void taskbar_config_init(void)`: read `TaskbarPosition` + `TaskbarSize` DWORDs from Registry; set globals
- [ ] `void taskbar_config_set(taskbar_position_t pos, taskbar_size_t size)`: update globals; persist to Registry; `wm_post_message_all(WM_DPI_CHANGED, g_dpi_pct, 0)` to trigger layout refresh
- [ ] Position routing: `desktop_in_taskbar(mx, my)` uses `g_taskbar_pos` to determine which edge to test
- [ ] Control Panel "Taskbar" tab: position dropdown (Bottom/Top/Left/Right); size dropdown (Small/Medium/Large); auto-hide checkbox; peek checkbox; pin manager list (`CTRL_LISTVIEW` of current pins with Remove buttons)
- [ ] Commit: `"taskbar: customization -- position/size/autohide/peek Registry, DPI_SCALE sizing, Control Panel tab"`

---

## OS Comparison


| ⭐  | Feature             | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | Window list         | ✅ Taskbar grouping, labels, accent underline, | ✅ GNOME dash-to-panel, KDE task manager; | ⬜ §1 -- `taskbar_entry_t` 64-slot static array; accent |
| 💎  | Button context menu | ✅ Right-click taskbar button → window   | ✅ KDE right-click task; GNOME extension; | ⬜ §2 -- uses `context_menu_show()` (TODO-07 §1); virtual |
| ⭐  | Aero Peek           | ✅ DWM Aero Peek (GPU composited);       | ⚠️ KDE Peek effect (GPU shader);          | ⬜ §3 -- `⭐` software `gfx_blit_alpha` per-window alpha |
| 💎  | Progress badges     | ✅ `ITaskbarList3::SetProgressValue/State`; used by Explorer, Edge, | ✅ Unity `libunity`; KDE `KStatusNotifierItem`; taskbar | ⬜ §4 -- 3 px bar at icon                |
| 💎  | Pinned apps         | ✅ Pin to taskbar; combined pin+window;  | ✅ GNOME Favorites (`gsettings`); KDE pinned | ⬜ §5 -- max 16 pins; CSV in             |
| 💎  | Jump lists          | ✅ `ICustomDestinationList`; Shell infrastructure; Explorer integration | ⚠️ KDE recent documents in taskbar;       | ⬜ §6 -- `SYS_JUMPLIST_NOTIFY=53`; Registry ring buffer max |
| 💎  | Auto-hide           | ✅ Taskbar settings → Auto-hide; DWM     | ✅ GNOME auto-hide dock; KDE auto-hide   | ⬜ §7 -- `gfx_tween_t` via `anim_mgr`; 150 ms |
| 💎  | Customization       | ✅ Taskbar position (all edges, Win10);  | ✅ GNOME extension position; KDE panel   | ⬜ §8 -- `⭐` all 4 edge positions       |

> **After §1–§8:** Impossible OS taskbar matches Windows 10 (not Win11's regression) by supporting all four edge positions while also delivering software Aero Peek (via `gfx_blit_alpha` per-window alpha pass) without any GPU dependency. Jump list integration with the `SYS_JUMPLIST_NOTIFY` syscall gives user-mode apps a clean path to register recent files from day one.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Open 3 windows → taskbar shows 3 buttons; focus window 2 → accent underline on button 2; close window 1 → button removed
- [ ] `taskbar_flash(win)`: button blinks orange 3× then stops
- [ ] Right-click window button → context menu with Close/Minimize/Restore; click Close → window destroyed
- [ ] Hover window button 500 ms → all other windows dim to ~10% opacity; move mouse away → opacity restored
- [ ] Click "Show Desktop" strip → all windows minimize; click again → restored
- [ ] `SYS_TASKBAR_SET_PROGRESS(win, 60, NORMAL)` → green 3 px bar covering 60% of button icon; state ERROR → red bar
- [ ] Pin `cmd.exe` to taskbar via "Pin to taskbar" → appears as icon-only button at left; click → `task_exec()` launches cmd.exe; running state → button fills and merges with pin
- [ ] Right-click pinned cmd.exe → jump list popup appears above context menu with recent entries (after `SYS_JUMPLIST_NOTIFY` calls from cmd.exe)
- [ ] Enable auto-hide via Control Panel → taskbar slides off bottom; move mouse to bottom 4 px → slides back; 500 ms idle → slides away again
- [ ] Set taskbar position to "Top" → taskbar moves to screen top; windows reflow below it
- [ ] Commit: `"taskbar: full feature set -- window list, peek, pins, jump lists, auto-hide, customization"`
