---
schema_version: 1
id: taskbar
domain: 08-graphics-ui
status: active
title: "TODO-10 -- Taskbar"
---

# TODO-10 -- Taskbar

> **Goal:** Build the full taskbar -- window list with active/flash/progress, right-click context menus, Aero Peek window preview, progress badges with a user-mode syscall, pinned app launchers, jump lists, auto-hide, and taskbar customization (alignment, search and Task view visibility; the bar stays 48 px and bottom-docked per `docs/design/shell.md#taskbar`). This makes the taskbar the primary desktop chrome it must be, matching Windows 11 feature-for-feature.

> [!IMPORTANT]
> **Already exists**: `TASKBAR_HEIGHT=48` + `desktop_draw_taskbar()` + `desktop_in_taskbar()` in `desktop.h` -- a basic bar in `src/desktop/desktop.c` that draws the background, a left-anchored Start button, one 100 px text button per window (raise + focus on click) and the clock. `gfx_blit_alpha(dst, dx, dy, src, alpha)` (whole surface, global alpha) for Aero Peek opacity. `task_exec(data, size)` and `task_create_user(entry, name)` for launching pinned apps. Context menu engine `context_menu_show()` (`08-graphics-ui/TODO-09` §1) must exist before §2 and §6. `WM_DPI_CHANGED` + `DPI_SCALE()` (`TODO-09` §4) for §8 sizing. Animation engine `anim_mgr_add()` / `GFX_EASE_IN/OUT_CUBIC` (`TODO-04`) for §7 auto-hide. None of these, nor `wm_minimize()`, exists yet. **Missing**: window list struct, progress badge, peek, pins, jump lists, auto-hide, customization. Complete sections in order: window list → pinned apps → context menu → progress badges → Aero Peek → jump lists → auto-hide → customization.

## Inputs

- `include/desktop/desktop.h` -- `TASKBAR_HEIGHT`, `desktop_draw_taskbar()`, `desktop_in_taskbar()` -- extended throughout
- `include/desktop/wm.h` -- `wm_window`, `wm_focus_window()`; planned `wm_minimize()`, `wm_restore()`, `wm_maximize()` (`TODO-08` §1) and `wm_post_message_all()` (`TODO-03` §8) -- used by §1 window list and §3 Aero Peek
- `include/gfx.h` -- `gfx_blit_alpha()`, `gfx_fill_rounded_rect()`, `gfx_drop_shadow()` -- used by §1, §3 peek, §6 jump list popup
- `include/kernel/sched/syscall.h` -- legacy `SYS_*` numbers end at 48 today; the progress and jump-list calls (drafted as 52 and 53) take the next free numbers when §4 and §6 add them
- `include/kernel/sched/task.h` -- `task_exec(data, size)` -- used by §5 pinned app launch
- `include/registry.h` -- `RegGetValue/SetValueEx` -- used by §5 pins, §6 jump lists, §7 auto-hide, §8 customization
- `include/kernel/gfx/anim_mgr.h` (TODO-04) -- `anim_mgr_add()`, `GFX_EASE_IN_CUBIC/OUT_CUBIC` -- used by §7 auto-hide slide tween
- `include/desktop/context_menu.h` (TODO-09 §1) -- `context_menu_show()` -- used by §2 and §7
- `include/desktop/dpi.h` (TODO-09 §4) -- `DPI_SCALE()`, `WM_DPI_CHANGED` -- used by §8 sizing
- → XREF: `08-graphics-ui/TODO-08-window-manager.md` -- §1 window list wires to `wm_minimize/maximize/restore`; §3 Aero Peek temporarily overrides compositor window opacity
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §1` -- context menu engine must exist before §2 right-click and §7 jump list popup; §9 vdesk present for "Move to Desktop ►"
- Related (no stable XREF target): `08-graphics-ui/TODO-11-startmenu-tray-notifications.md` (notifications) -- §5 progress badge toast is a forward ref; use serial log stub until TODO-11 §5 is live

## Outcome

- `struct taskbar_entry` drives the centred, icon-only 40 px app buttons with the idle/focused indicator pill, flash and progress bar (`docs/design/shell.md#taskbar`).
- Right-click any window button → context menu with Close / Restore / Minimize / Move to Desktop.
- Hover 500 ms over window button → Aero Peek (all other windows dimmed to 10%); "Show Desktop" strip.
- `taskbar_set_progress(win, pct, state)` + `SYS_TASKBAR_SET_PROGRESS=52` syscall for user-mode apps.
- Pinned apps lead the app block after Start, search and Task view; running+pinned → combined; Registry-persisted.
- Jump list popup above right-click menu; `SYS_JUMPLIST_NOTIFY=53` syscall.
- Auto-hide: taskbar slides off-screen when unused; slides back on mouse-at-edge.
- Taskbar alignment (centre or left), search and Task view visibility, auto-hide and pin manager in Settings; the taskbar stays 48 px and bottom-docked.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                            | Depends On                                                                | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Window list -- `taskbar_entry`, add/remove/active wired from WM events, flash, draw                 | `wm_window`, `wm_focus/minimize` (TODO-08 §1) must exist                  |  [ ]   |
| 💎  |   5   | §5 Pinned apps -- load/save Registry pins, icon-only buttons, combined when running                    | §1 window list (combined button logic)                                    |  [ ]   |
| 💎  |   2   | §2 Context menu -- right-click window button → Close/Restore/Minimize/Move to Desktop                  | §1 window list + §5 pins; `context_menu_show()` (TODO-09 §1)              |  [ ]   |
| 💎  |   4   | §4 Progress badges -- `taskbar_set_progress()`, thin bar at icon bottom, `SYS_TASKBAR_SET_PROGRESS=52` | §1 window list (drawn on window buttons)                                  |  [ ]   |
| ⭐  |   3   | §3 Aero Peek -- 500 ms hover, compositor opacity override, "Show Desktop" strip                        | §1 window list; `gfx_blit_alpha`; `wm_composite` modification (TODO-08)   |  [ ]   |
| 💎  |   6   | §6 Jump lists -- recent files popup above context menu, `SYS_JUMPLIST_NOTIFY=53`                       | §2 context menu (jump list is shown above it); §5 pins (per-app Registry) |  [ ]   |
| 💎  |   7   | §7 Auto-hide -- slide off/on with anim_mgr tween, mouse proximity trigger                              | §1–§6 stable; TODO-04 `anim_mgr_add()` must exist                         |  [ ]   |
| 💎  |   8   | §8 Customization -- alignment, search/Task view visibility, pins, Settings page                        | §7 (auto-hide is a customization option too); TODO-09 §4 DPI              |  [ ]   |

---

## 1. Taskbar Window List `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)

**Owner of:** the work planned in `06-desktop-foundation/TODO-05 §3`, which is superseded there so the shell has one implementation.

`struct taskbar_entry` (window ptr, title, icon_id, active flag, flash flag, progress_pct, progress_state). `taskbar_add_window(win)` / `taskbar_remove_window(win)` / `taskbar_set_active(win)` wired from WM events. Draw the app buttons in the centred group after Start, the search box and Task view (`docs/design/shell.md#taskbar`). Click → focus/raise; click active → minimize. `taskbar_flash(win)` blinks 3×.

**Files:** `src/desktop/taskbar_winlist.c` (new), `include/desktop/taskbar.h` (new), `src/desktop/desktop.c` (extend)

> [!NOTE]
> Layout per `docs/design/shell.md#taskbar`: the taskbar is `THEME_SIZE_TASKBAR_HEIGHT` (48) tall, full width, bottom-docked, with a 1 px `stroke_divider` top border and taskbar acrylic (`mat.taskbar`). The centre group is centred on the screen: Start, the search box, Task view, then pinned apps (§5) and running windows as one block, `THEME_SIZE_TASKBAR_GAP` (4) apart. Each app button is `THEME_SIZE_TASKBAR_BUTTON` (40) square with `THEME_RADIUS_TASKBAR_BUTTON` (4) corners and a `THEME_SIZE_TASKBAR_ICON` (24) icon, no title text. **Running indicator**: a 3 px (`THEME_SIZE_TASKBAR_INDICATOR_HEIGHT`) pill 2 px above the bottom edge: running but not focused = 6 px wide in `taskbar_indicator_idle`; focused = 16 px wide in `taskbar_indicator` plus the hover fill on the button; the width animates over `THEME_MOTION_NORMAL_MS` with `gfx_ease_decelerate`. **Hover**: `subtle_fill_hover` with a 1 px `stroke_control` outline. **Attention**: `taskbar_flash(win)` sets the button fill `taskbar_attention_fill` and a full-width `taskbar_attention_indicator` pill until the window is focused, no blinking (`docs/design/shell.md#taskbar`). **WM wiring**: call `taskbar_add_window(win)` from `wm_create_window()`; `taskbar_remove_window(win)` from `wm_destroy_window()`; `taskbar_set_active(win)` from `wm_focus_window()`. Minimized windows keep their button with the idle indicator.

- [ ] `typedef struct { wm_window_t *win; char title[128]; uint32_t icon_id; uint8_t active; uint8_t flash_count; uint8_t flash_phase; uint8_t progress_state; uint8_t progress_pct; } taskbar_entry_t;` in `taskbar.h`
- [ ] `#define TASKBAR_MAX_ENTRIES 64` -- static array; no dynamic alloc for window list
- [ ] `void taskbar_add_window(wm_window_t *win)` -- find free slot; copy title from `win->title`; copy `win->icon_id`
- [ ] `void taskbar_remove_window(wm_window_t *win)` -- find entry by `win` ptr; zero slot
- [ ] `void taskbar_set_active(wm_window_t *win)` -- clear `active` on all entries; set `active=1` on matching entry
- [ ] `void taskbar_flash(wm_window_t *win)` -- set `flash_count=6` (3 blink pairs); `flash_phase=0`; periodic timer drives toggle
- [ ] `void taskbar_tick(void)` -- called from `desktop_tick()`: advance flash timers; rebuild draw list
- [ ] `void taskbar_draw_winlist(gfx_surface_t *s, int32_t x, int32_t w)` -- draw all window buttons in region [x, x+w]
- [ ] Click handler: find entry by mouse x; if `entry->active` → `wm_minimize(win)`; else → `wm_restore(win)` + `wm_focus_window(win)`
- [ ] Draw the taskbar to the design spec (`docs/design/shell.md#taskbar`): centred group (Start, search box, Task view, apps), 40 px buttons, 24 px icons, idle/focused indicator pill
  - Start button shows the `start` icon (the logo mark); pressing it scales the icon to 86% for the press duration
  - Search box: 196 x 36 pill, `THEME_RADIUS_SEARCH_BOX` (18), `control_fill` with a 1 px `stroke_control` outline, a search glyph and "Search" in `text_secondary`; below 1024 px screen width it collapses to a 40 px search button
  - Task view button: a 40 px subtle button with the Task view glyph, opening Task View (`08-graphics-ui/TODO-09 §9`)
  - Every size and colour comes from `include/desktop/theme_tokens.h` (`THEME_SIZE_TASKBAR_*`, `THEME_MAT_*_TASKBAR_*`); no hex literal survives in the taskbar draw path
  - Acrylic from `gfx_acrylic()` is cached per surface and recomputed only when the region behind the taskbar changes
- [ ] Unify the taskbar window-button geometry: drawing and clicks use 100 px with no flag filter (`desktop.c:515`, `:1108`) but the hand-cursor test uses 120 px and `flags & 0x01` (`desktop.c:1209`)
  - The click loop is also not bounded by the drawn list's right edge (`desktop.c:1112` versus `:518`), so clicks can land on buttons that were never drawn
  - One shared geometry helper for draw, click and cursor
  - The Start button has the same split: it is drawn at x 4-51 but hit-tested at x 2-49 (`desktop.c:460` versus `:992`); found in §17 of the same docs roadmap
  - Found while writing the docs pages (`00-infrastructure/TODO-10-documentation-site.md` §16); verified at source, not reproduced at runtime
- [ ] Commit: `"taskbar: window list -- taskbar_entry, add/remove/active/flash wired from WM events"`

## 2. Taskbar Button Context Menu `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar), [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)

Right-click window button → `context_menu_show()`: Close, Restore/Maximize (toggle), Minimize, *(stretch)* "Move to Desktop ►" submenu (virtual desktops), *(stretch)* "Pin to taskbar".

**Files:** `src/desktop/taskbar_winlist.c` (extend), `src/desktop/taskbar_ctxmenu.c` (new)

> [!NOTE]
> Detect right-click on window button in a new `desktop_handle_mouse()` (today `desktop_handle_click()` handles the left button only). Menu items are dynamic: if window is maximized → show "Restore"; else → show "Maximize". If already minimized → hide "Minimize". "Move to Desktop ►" submenu: list `vdesk_name(i)` for each active virtual desktop (TODO-09 §9); callback → `vdesk_move_window(win, i)`. "Pin to taskbar" toggles `entry->pinned`; persist to Registry. All items wire to `wm_*` functions.

- [ ] `void taskbar_show_win_menu(taskbar_entry_t *entry, int32_t mx, int32_t my)` -- build `menu_item[]` array based on window state; call `context_menu_show(mx, my, items, count)`
- [ ] Menu items: `Close` → `wm_destroy_window(entry->win)`; `Restore/Maximize` → toggle; `Minimize` → `wm_minimize(entry->win)` (hide if already minimized)
- [ ] "Move to Desktop ►": submenu of desktop names; callback `vdesk_move_window(entry->win, idx)` (stub if §7 vdesks not live)
- [ ] "Pin to taskbar": toggle; persists pin path to `HKCU\Software\Impossible\Shell\TaskbarPins`
- [ ] "Unpin from taskbar" shown instead of "Pin" if entry is pinned
- [ ] Right-click detection in `desktop_handle_mouse()`: if right-button and hit-tests a window button → `taskbar_show_win_menu(entry, mx, my)`
- [ ] Commit: `"taskbar: window button context menu -- close/restore/maximize/minimize/pin/move-to-desktop"`

## 3. Aero Peek `[Opus]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)

Hover a window button for `THEME_MOTION_TASKBAR_PREVIEW_DELAY_MS` (400) → the thumbnail preview flyout opens above it (one 200 x 120 thumbnail per window, title, close glyph); hover a thumbnail for `THEME_MOTION_PEEK_DELAY_MS` (500) → all other windows blitted at alpha=25 (10%), the previewed window stays at 255; leave → restore all (`docs/design/shell.md#taskbar`, Hover preview). "Show Desktop" strip (8 px at the far right, after the clock and bell, marked by a 1 px `stroke_divider` line): hover = peek all, click = toggle Win+D. Registry `EnablePeek` (default 1).

**Files:** `src/desktop/taskbar_peek.c` (new), `include/desktop/wm.h` (extend), `src/desktop/wm.c` (extend compositor)

> [!NOTE]
> This is `[Opus]` -- Aero Peek requires a novel per-window temporary opacity field in the compositor. Currently `wm_composite()` blits all visible windows at full opacity. The change: add `uint8_t peek_alpha` to `struct wm_window` (default 255); compositor: use `gfx_blit_alpha(dst, win->x, win->y, &win_surf, win->peek_alpha)` instead of full-opacity blit. On peek start: iterate all entries except the hovered window; set `win->peek_alpha = 25`; `wm_mark_dirty()`. On peek end: restore all `win->peek_alpha = 255`. **500 ms hover timer**: in `taskbar_tick()` per entry: increment `hover_ms` by tick delta; if `hover_ms >= 500` and not yet in peek: `taskbar_peek_start(entry)`. On mouse-leave: `hover_ms = 0`; `taskbar_peek_end()`. **Show Desktop strip**: `THEME_SIZE_SHOW_DESKTOP_WIDTH` (8) px at the far right; hover → peek all windows (no hovered exception); click → `wm_toggle_show_desktop()` (minimizes all, toggle restores). The compositor `gfx_blit_alpha` call replaces the unconditional `fb_blit` per window -- this is the core architectural change.

- [ ] `uint8_t peek_alpha` field in `struct wm_window` (default 255); `uint16_t hover_ms` in `taskbar_entry_t`
- [ ] `void taskbar_peek_start(taskbar_entry_t *peeked_entry)`: set `win->peek_alpha = 25` for all windows except `peeked_entry->win`; `wm_mark_dirty()`
- [ ] `void taskbar_peek_end(void)`: restore all windows to `peek_alpha = 255`; `wm_mark_dirty()`
- [ ] `wm_composite()`: per window blit: `if (win->peek_alpha < 255) gfx_blit_alpha(..., win->peek_alpha); else fb_blit_direct(...)`
- [ ] `taskbar_tick()` hover timer: accumulate `hover_ms` while mouse over button; at 500 ms → `taskbar_peek_start()`; mouse leave → reset + `taskbar_peek_end()`
- [ ] "Show Desktop" strip: 8 px at the far right of the taskbar with a 1 px `stroke_divider` line at its left; hover → full-peek all windows; click → `wm_toggle_show_desktop()`
- [ ] `void wm_toggle_show_desktop(void)`: minimize all non-minimized windows (save state); second call restores them; `g_show_desktop_active` toggle flag
- [ ] `int g_peek_enabled` from `HKCU\Software\Impossible\Shell\EnablePeek` (default 1); if 0: skip hover timer + draw "Show Desktop" strip without peek behavior
- [ ] Commit: `"taskbar: Aero Peek -- per-window peek_alpha in compositor, 500ms hover, Show Desktop strip"`

## 4. Taskbar Progress Badges `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)

`taskbar_set_progress(win, pct, state)` draws a thin 3 px bar at the bottom of the window button icon. States: `TASKBAR_PROGRESS_NORMAL` (green), `TASKBAR_PROGRESS_PAUSED` (yellow), `TASKBAR_PROGRESS_ERROR` (red), `TASKBAR_PROGRESS_NONE` (hidden). `SYS_TASKBAR_SET_PROGRESS=52` syscall for user-mode apps.

**Files:** `src/desktop/taskbar_winlist.c` (extend), `include/desktop/taskbar.h` (extend), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> The progress bar is a 3 px tall strip at the very bottom of the window button, spanning `(pct * button_w / 100)` pixels. Colour map (`docs/design/shell.md#taskbar` Badges): NORMAL = `accent`, PAUSED = `status_caution`, ERROR = `status_critical`; theme tokens, no literals. State is stored in `entry->progress_pct` (0–100) and `entry->progress_state`. `taskbar_draw_winlist()` checks `entry->progress_state != TASKBAR_PROGRESS_NONE` and draws the strip after the button fill. **Syscall**: `sys_taskbar_set_progress(pid, pct, state)` -- kernel side looks up the window belonging to `pid` via a new `task_get_main_window(pid)` (no such helper exists yet), then calls `taskbar_set_progress(win, pct, state)`.

- [ ] `#define TASKBAR_PROGRESS_NONE    0`, `NORMAL=1`, `PAUSED=2`, `ERROR=3` in `taskbar.h`
- [ ] `void taskbar_set_progress(wm_window_t *win, uint8_t pct, uint8_t state)` -- find entry by win; update `progress_pct/state`; `wm_mark_dirty()`
- [ ] Progress bar draw in `taskbar_draw_winlist()`: after button fill; `gfx_fill_rect(s, btn_x, btn_bottom-3, pct*btn_w/100, 3, color_for_state)`
- [ ] `#define SYS_TASKBAR_SET_PROGRESS 52` in `syscall.h`
- [ ] `sys_taskbar_set_progress(int pid, int pct, int state)` kernel handler: `task_get_main_window(pid)` → `taskbar_set_progress(win, pct, state)`
- [ ] Wire `SYS_TASKBAR_SET_PROGRESS` into syscall dispatch table in `src/kernel/sched/syscall.c`
- [ ] Commit: `"taskbar: progress badges -- NORMAL/PAUSED/ERROR states, 3px bar, SYS_TASKBAR_SET_PROGRESS=52"`

## 5. Pinned Apps `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)

Load from `HKCU\Software\Impossible\Shell\TaskbarPins` (comma-separated paths) at boot. Pins come first in the app block of the centred group (after Task view), running windows follow. Running + pinned = combined button. Not-running = icon-only → click → `task_exec()`. Right-click pinned → "Unpin from taskbar".

**Files:** `src/desktop/taskbar_pins.c` (new), `include/desktop/taskbar.h` (extend)

> [!NOTE]
> Pinned entries are a separate `taskbar_pin_t[]` array (max 16 pins). At startup, `taskbar_pins_load()` reads the Registry CSV; for each path, load the app's icon (try `C:\\path_dir\\icon.ires` → `ires_get_icon()`; fallback to generic app icon). When a pinned app's window appears (matched by `win->title` vs pin path basename), `entry->pinned = 1` and the window button draws at the pin's position. Not-running pin: icon only, no indicator (Windows 11 shows the indicator pill only for running apps). Running pin: draws the window button normally at the pinned slot position -- no separate icon entry.

- [ ] `typedef struct { char path[256]; uint32_t icon_id; wm_window_t *running_win; } taskbar_pin_t;` in `taskbar.h`
- [ ] `#define TASKBAR_MAX_PINS 16` -- static array
- [ ] `void taskbar_pins_load(void)`: `RegGetValue("HKCU\\...\\TaskbarPins")` → parse CSV into `g_pins[]`; load icon per entry
- [ ] `void taskbar_pins_save(void)`: serialize `g_pins[].path` to CSV → `RegSetValueEx()`
- [ ] `void taskbar_pin_add(const char *path)`: find free slot; load icon; save
- [ ] `void taskbar_pin_remove(const char *path)`: zero slot; save
- [ ] Pin draw: if `pin->running_win == NULL`: icon only, no indicator; click → `task_exec(load_file(pin->path), size)`; right-click → "Unpin from taskbar" + "Open" items
- [ ] Pin+running merge: when `taskbar_add_window(win)` finds a matching pin path → link `pin->running_win = win`; skip creating a separate window button slot; draw merged button at pin position
- [ ] Match heuristic: compare `basename(pin->path)` vs `win->title` (case-insensitive); secondary: `win->exe_path` if struct supports it
- [ ] Commit: `"taskbar: pinned apps -- Registry CSV pins, icon-only launch, combined running+pin button"`

## 6. Jump Lists `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)

Right-click pinned or window button → jump list popup above context menu showing recent files / frequent tasks. Read from `HKCU\Software\Impossible\Shell\JumpLists\{app_name}\Recent[]`. `SYS_JUMPLIST_NOTIFY=53` syscall for apps to register recent files.

**Files:** `src/desktop/taskbar_jumplist.c` (new), `include/desktop/taskbar.h` (extend), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> Jump list popup is a separate overlay window at z_order=26000 (above context menu at 25000 from TODO-09 §1). Layout: the context menu geometry and menu acrylic of `docs/design/shell.md#context-menus` (256 px wide, 32 px items, radius 8); sections: "Recent" (up to 10 items, each with icon + filename); "Pinned" (manually pinned items -- stub); "Tasks" (app-defined, stub). Popup appears immediately above the context menu if a jump list exists for the app; if empty, skip popup and show only the context menu. **Syscall**: `sys_jumplist_notify(const char *path, uint32_t len)` -- kernel side: `current_task->name` as key; write path to `HKCU\Software\Impossible\Shell\JumpLists\{name}\Recent[]` (ring buffer of 10 entries).

- [ ] `#define SYS_JUMPLIST_NOTIFY 53` in `syscall.h`
- [ ] `sys_jumplist_notify(const char *path, uint32_t len)` kernel handler: validate path; append to Registry ring buffer (max 10 entries); `RegSetValueEx("RecentN", path)` + `RegSetValueEx("RecentCount", count)`
- [ ] `void jumplist_load(const char *app_name, char paths[][256], int *count)`: read up to 10 entries from Registry
- [ ] `void jumplist_show(const char *app_name, int32_t btn_x, int32_t btn_y)`: load entries; if count == 0 return (no popup); create overlay window at z_order=26000 above context menu position; draw acrylic panel + items
- [ ] Item click: `task_exec(load_file(path), size)` to open the file in its app; close jump list
- [ ] `taskbar_show_win_menu()` (§2): call `jumplist_show(app_name, ...)` before `context_menu_show()` if app_name has entries; position jump list directly above the context menu top
- [ ] Wire `SYS_JUMPLIST_NOTIFY` into syscall dispatch table
- [ ] Commit: `"taskbar: jump lists -- SYS_JUMPLIST_NOTIFY=53, Registry ring buffer, popup above context menu"`

## 7. Taskbar Auto-Hide `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)

`HKCU\Software\Impossible\Shell\TaskbarAutoHide`: slide off-screen when the mouse is not near the bottom edge (`THEME_MOTION_NORMAL_MS`, `gfx_ease_accelerate`, via `anim_mgr`). Slide back when the mouse reaches the bottom row (`THEME_MOTION_NORMAL_MS`, `gfx_ease_decelerate`).

**Files:** `src/desktop/taskbar_autohide.c` (new), `include/desktop/taskbar.h` (extend)

> [!NOTE]
> Auto-hide uses a `gfx_tween_t g_taskbar_y_tween` that animates `g_taskbar_offset` (0 = fully visible, `TASKBAR_HEIGHT` = fully hidden). The compositor and `desktop_in_taskbar()` check `g_taskbar_offset` to position the bar correctly. **Trigger**: in `desktop_handle_mouse()`: if mouse y >= `fb_get_height() - 4` (bottom 4 px proximity strip): if `g_taskbar_hidden`: start slide-in tween. If mouse y < `fb_get_height() - TASKBAR_HEIGHT - 16` and no open window over taskbar: start slide-out tween after 500 ms idle. **Desktop height**: when auto-hide is enabled, `desktop_get_usable_height()` returns `fb_get_height()` (full screen available) so windows can use the full area.

- [ ] `int g_taskbar_autohide` (0=off, 1=on) + `int g_taskbar_hidden` + `gfx_tween_t g_taskbar_y_tween` in `taskbar_autohide.c`
- [ ] `void taskbar_autohide_init(void)`: read `HKCU\Software\Impossible\Shell\TaskbarAutoHide` DWORD
- [ ] `void taskbar_autohide_show(void)`: `anim_mgr_add(&g_taskbar_y_tween, TASKBAR_HEIGHT, 0, THEME_MOTION_NORMAL_MS, gfx_ease_decelerate, NULL)`
- [ ] `void taskbar_autohide_hide(void)`: `anim_mgr_add(&g_taskbar_y_tween, 0, TASKBAR_HEIGHT, THEME_MOTION_NORMAL_MS, gfx_ease_accelerate, NULL)`
- [ ] Mouse proximity: in `desktop_handle_mouse()`: if autohide enabled and `my >= fb_h - 4` → `taskbar_autohide_show()`; else if `my < fb_h - tb_h - 16` and `g_taskbar_hidden` → start 500 ms idle countdown → `taskbar_autohide_hide()`
- [ ] Compositor: `taskbar_y = fb_h - TASKBAR_HEIGHT + g_taskbar_y_tween.current`; draw taskbar at offset y
- [ ] `desktop_get_usable_height()`: if autohide enabled → return `fb_get_height()`; else → `fb_get_height() - TASKBAR_HEIGHT`
- [ ] Context menus / jump lists open above taskbar: auto-show taskbar while any popup is open
- [ ] Commit: `"taskbar: auto-hide -- anim_mgr slide in/out, proximity trigger, full usable height when hidden"`

## 8. Taskbar Customization `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar)

The taskbar is always `THEME_SIZE_TASKBAR_HEIGHT` (48) tall and docked to the bottom (`docs/design/shell.md#taskbar`); Windows 11 has no top/left/right positions or size presets, and neither does Impossible OS. Customization covers what Windows 11 offers: alignment of the centre group (Centre, the default, or Left), auto-hide (§7), which of search / Task view show, and the pin manager. `DPI_SCALE()` applied to all sizing. Settings "Personalization > Taskbar" page.

**Files:** `src/desktop/taskbar_config.c` (new), `include/desktop/taskbar.h` (extend)

> [!NOTE]
> `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\TaskbarAl` (0 = left, 1 = centre, default 1) mirrors the Windows 11 key; `ShowTaskViewButton`, `SearchboxTaskbarMode` (0 hidden, 1 icon, 2 box) likewise. Left alignment places the group 12 px from the left edge instead of centring it; nothing else moves. `TASKBAR_HEIGHT = DPI_SCALE(THEME_SIZE_TASKBAR_HEIGHT)`. On setting change: re-layout the taskbar only.

- [ ] `typedef enum { TB_ALIGN_LEFT=0, TB_ALIGN_CENTER=1 } taskbar_align_t;` + `g_taskbar_align` (default centre)
- [ ] `void taskbar_config_init(void)`: read `TaskbarAl`, `ShowTaskViewButton`, `SearchboxTaskbarMode`; set globals
- [ ] `void taskbar_config_set(taskbar_align_t align, int show_taskview, int search_mode)`: update globals; persist; re-layout the taskbar
- [ ] Settings "Taskbar" page (settings rows per `docs/design/controls.md#cards-and-settings-rows`): alignment combo box (Left / Centre)
  - search mode (Hidden / Icon / Search box)
  - Task view toggle
  - auto-hide toggle
  - pin manager list with Remove buttons
- [ ] Commit: `"taskbar: customization -- alignment, search/task view visibility, auto-hide, pins, Settings page"`

---

## OS Comparison


| ⭐  | Feature             | 🪟 Win11                                                                | 🐧 Linux                                                | 🚀 Impossible OS                                            |
| --- | ------------------- | ----------------------------------------------------------------------- | ------------------------------------------------------- | ----------------------------------------------------------- |
| 💎  | Window list         | ✅ Centred icon buttons, running indicator pills,                       | ✅ GNOME dash-to-panel, KDE task manager;               | ⬜ §1 -- `taskbar_entry_t` 64-slot array; indicator pill    |
| 💎  | Button context menu | ✅ Right-click taskbar button → window                                  | ✅ KDE right-click task; GNOME extension;               | ⬜ §2 -- uses `context_menu_show()` (TODO-09 §1); virtual   |
| ⭐  | Aero Peek           | ✅ DWM Aero Peek (GPU composited);                                      | ⚠️ KDE Peek effect (GPU shader);                        | ⬜ §3 -- `⭐` software `gfx_blit_alpha` per-window alpha    |
| 💎  | Progress badges     | ✅ `ITaskbarList3::SetProgressValue/State`; used by Explorer, Edge,     | ✅ Unity `libunity`; KDE `KStatusNotifierItem`; taskbar | ⬜ §4 -- 3 px bar at icon                                   |
| 💎  | Pinned apps         | ✅ Pin to taskbar; combined pin+window;                                 | ✅ GNOME Favorites (`gsettings`); KDE pinned            | ⬜ §5 -- max 16 pins; CSV in                                |
| 💎  | Jump lists          | ✅ `ICustomDestinationList`; Shell infrastructure; Explorer integration | ⚠️ KDE recent documents in taskbar;                     | ⬜ §6 -- `SYS_JUMPLIST_NOTIFY=53`; Registry ring buffer max |
| 💎  | Auto-hide           | ✅ Taskbar settings → Auto-hide; DWM                                    | ✅ GNOME auto-hide dock; KDE auto-hide                  | ⬜ §7 -- `gfx_tween_t` via `anim_mgr`; motion tokens        |
| 💎  | Customization       | ✅ Alignment left/centre; bottom only (Win11)                           | ✅ GNOME extension position; KDE panel                  | ⬜ §8 -- alignment, visibility, pins; fixed 48 px bottom    |

> **After §1–§8:** Impossible OS taskbar matches the Windows 11 taskbar (`docs/design/shell.md#taskbar`): centred icon buttons with running pills, fixed 48 px bottom dock, left/centre alignment, plus software Aero Peek (via `gfx_blit_alpha` per-window alpha pass) without any GPU dependency. Jump list integration with the `SYS_JUMPLIST_NOTIFY` syscall gives user-mode apps a clean path to register recent files from day one.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Open 3 windows → 3 icon buttons appear in the centred group after Start, search and Task view; focus window 2 → its indicator widens to the 16 px accent pill; close window 1 → button removed
- [ ] `taskbar_flash(win)`: button fill pulses 3× then stops
- [ ] Right-click window button → context menu with Close/Minimize/Restore; click Close → window destroyed
- [ ] Hover window button 500 ms → all other windows dim to ~10% opacity; move mouse away → opacity restored
- [ ] Click "Show Desktop" strip → all windows minimize; click again → restored
- [ ] `SYS_TASKBAR_SET_PROGRESS(win, 60, NORMAL)` → green 3 px bar covering 60% of button icon; state ERROR → red bar
- [ ] Pin `cmd.exe` to taskbar via "Pin to taskbar" → appears as icon-only button at left; click → `task_exec()` launches cmd.exe; running state → button fills and merges with pin
- [ ] Right-click pinned cmd.exe → jump list popup appears above context menu with recent entries (after `SYS_JUMPLIST_NOTIFY` calls from cmd.exe)
- [ ] Enable auto-hide via Settings → taskbar slides off bottom; move mouse to bottom 4 px → slides back; 500 ms idle → slides away again
- [ ] Set alignment to "Left" → the app group moves to 12 px from the left edge; Start stays first; back to "Centre" restores
- [ ] Shell matches the design reference: a framebuffer capture of the running shell compared with the matching `impossibleos.co/design/?shot` render (dark and light) passes the perceptual diff of `00-infrastructure/TODO-05 §9` at default threshold
- [ ] Commit: `"taskbar: full feature set -- window list, peek, pins, jump lists, auto-hide, customization"`
