---
schema_version: 1
id: screensaver-widgets-display
domain: 10-platform-services
status: active
title: "TODO-05 -- Screensaver, Widgets & Display"
---

# TODO-05 -- Screensaver, Widgets & Display

> **Goal:** Add the idle/lock experience, desktop widget layer, and display management that round out the desktop shell. The compositor, GFX library, and GOP mode list are already available -- this TODO builds the user-facing idle, decoration, and display features directly on top.

> [!IMPORTANT]
> **Already exists**: `gfx_acrylic()`, `gfx_draw_line()`, `gfx_fill_circle()`, `gfx_fill_rect()` in `gfx.h`. `FONT_MONO`, `FONT_UI` in `font_mgr.h`. `system_get_ticks()`, `uptime()` in `timer.h`. `boot_info->gop_modes[]` + `gop_mode_count` + `gop_mode_selected` in `boot_info.h` -- full GOP mode list available at boot. `fb_lock_compositor()` / `fb_unlock_compositor()`. `wallpaper_set()` (TODO-07 forward dep). Lock screen (TODO-06 §7 forward dep). `sched_get_task_list()` / `cpu_ticks` (TODO-12 §1 forward dep for CPU meter widget). `pmm_get_total_frames()` / `pmm_get_free_frames()`. `image_load()` for bouncing logo PNG. `ttf_draw_string()`. **Missing**: `g_last_input_ticks` idle tracking in WM (add 1 line); screensaver `scr_entry_fn` API; widget manager + lifecycle; GOP runtime `SetMode` (not available post-ExitBootServices -- `display_set_mode()` is a stretch requiring VirtIO-GPU MMIO). **`LOG_SECURITY` + `kevent_log()`** already done in TODO-04 §1.

## Inputs

- `include/gfx.h` -- `gfx_acrylic/fill_rect/fill_circle/draw_line/put_pixel/blit()` -- §1 screensaver rendering, §4 widget drawing
- `include/font_mgr.h` -- `FONT_MONO`, `FONT_UI`; `ttf_draw_string()` -- §1 Matrix screensaver, §4 clock/notes widgets
- `include/kernel/timer.h` -- `system_get_ticks()`, `uptime()` -- §1 idle detection, §4 clock widget tick
- `include/kernel/boot_info.h` -- `boot_info->gop_modes[BOOT_GOP_MODE_MAX]`, `gop_mode_count`, `gop_mode_selected` -- §5 mode enum
- `include/kernel/drivers/framebuffer.h` -- `fb_lock_compositor()`, `fb_unlock_compositor()` -- §1 screensaver exclusive surface
- `include/kernel/mm/pmm.h` -- `pmm_get_total_frames/free_frames()` -- §4 RAM monitor widget
- `include/kernel/image.h` -- `image_load()` -- §1 bouncing logo screensaver, §4 calendar widget
- `include/registry.h` -- `HKCU\Software\Impossible\Screensaver\*`, `HKCU\Software\Impossible\Widgets\{id}\*`, `HKLM\HARDWARE\Display\*` -- §1 settings, §3 widget positions, §5 current mode
- `include/desktop/wm.h` -- WM input event loop (add `g_last_input_ticks`); `wm_create_window()` -- §1 idle hook, §4 widget overlay
- `include/desktop/controls.h` (TODO-05) -- `CTRL_DROPDOWN`, `CTRL_SLIDER`, `CTRL_CHECKBOX` -- §5 desk.cpl
- `include/cpl.h` (TODO-11) -- `CPlApplet_t` -- §9 desk.cpl mode picker update
- → XREF: `09-desktop-shell/TODO-06 §7` -- Win+L lock screen; §2 calls lock screen on screensaver dismiss when `RequirePassword=1`
- → XREF: `09-desktop-shell/TODO-07 §2` -- `wallpaper_set()`; §1 bouncing logo screensaver reads wallpaper path; §3 widget manager sits above wallpaper layer
- → XREF: `09-desktop-shell/TODO-11 §7` -- `desk.cpl` resolution dropdown; §9 here populates it with GOP mode list
- → XREF: `10-platform-services/TODO-12 §1` -- `sched_get_task_list()` + `cpu_ticks`; §5 CPU meter widget depends on that

## Outcome

- `scr_entry_fn` screensaver API; 5 built-ins (Blank, Starfield, Matrix, Bouncing Logo, Clock); idle timeout from Registry; any input dismisses.
- Screensaver → lock screen bridge (`RequirePassword=1` default).
- Widget manager: `WGT_INIT/RENDER/TICK/CLICK/CLOSE` lifecycle; drawn above wallpaper below windows; draggable; `gfx_acrylic` bg; Registry positions.
- 5 built-in widgets: Analog Clock, CPU Meter, RAM Monitor, Mini Calendar, Quick Notes.
- `display_enum_modes()` from `boot_info->gop_modes[]`; current mode in Registry; `desk.cpl` dropdown updated.
- Multi-monitor `struct monitor` stubs (stretch).

## Implementation Order

| ⭐  | Order | Deliverable                                                                                             | Depends On                                                                                   | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Screensaver system -- idle tracking, `scr_entry_fn` API, 5 built-ins, dismiss                        | add `g_last_input_ticks` to WM; `system_get_ticks()`; `image_load()` for logo              |  [ ]   |
| 💎  |   2   | §2 Screensaver → lock bridge -- `RequirePassword` Registry; dismiss → lock screen                       | §1; TODO-06 §7 lock screen `lock_screen_show()`                                             |  [ ]   |
| ⭐  |   3   | §3 Widget framework -- `struct widget`, `WGT_*` lifecycle, compositor layer, drag, Registry positions   | compositor tick (exists); `gfx_acrylic()`; `registry_set/get()` for positions               |  [ ]   |
| ⭐  |   4   | §4 Built-in widgets -- Analog Clock, CPU Meter, RAM Monitor, Mini Calendar, Quick Notes                  | §3 framework; `system_get_ticks()` clock; `sched_get_task_list()` (TODO-12 §1)             |  [ ]   |
| 💎  |   5   | §5 Display management -- `display_enum_modes()`, current mode Registry, `desk.cpl` dropdown update      | `boot_info->gop_modes[]` (exists); `desk.cpl` stub (TODO-11 §5)                            |  [ ]   |
| 💎  |   6   | §6 Multi-monitor stubs -- `struct monitor`, `monitor_enum()`, virtual coordinate space stubs             | §5; no hardware dep (stubs only)                                                             |  [ ]   |

---

## 1. Screensaver System `[Sonnet]`

Idle detection: track `g_last_input_ticks` in WM input handler. PIT tick comparison: `system_get_ticks() - g_last_input_ticks > idle_timeout_ticks`. On timeout → `scr_launch(type)`. API: `scr_entry_fn(msg, surface)` with `SCR_INIT / SCR_FRAME / SCR_CLOSE`. Any input → `scr_dismiss()`.

**Files:** `src/desktop/screensaver.c` (new), `include/desktop/screensaver.h` (new)

> [!NOTE]
> **Idle tracking**: add `uint64_t g_last_input_ticks` global to `wm.c`; update to `system_get_ticks()` on any `WM_MOUSE_MOVE`, `WM_MOUSE_DOWN`, or `WM_KEYDOWN` event in the WM event dispatcher. **Idle check**: in compositor tick (called ~60 Hz): `if (system_get_ticks() - g_last_input_ticks > idle_ticks)` → `scr_launch()`. `idle_ticks = registry_get("HKCU\\...\\IdleTimeout") * 60 * TICKS_PER_SEC`. **Launch**: `fb_lock_compositor()` → allocate full-screen `gfx_surface_t` (PMM backed) → call `scr_fn(SCR_INIT, surf)` → enter screensaver loop (blocking compositor main loop or separate task). **Frame loop**: call `scr_fn(SCR_FRAME, surf)` → `fb_blit(surf)` → `sleep_ms(16)` (≈60 fps). **Dismiss**: on any input event in screensaver mode → `scr_fn(SCR_CLOSE, surf)` → `pmm_free()` surface → `fb_unlock_compositor()` → resume normal compositor. **Built-ins**: `typedef void (*scr_entry_fn_t)(int msg, gfx_surface_t *s)`. Five implementations:
> - **Blank**: `SCR_FRAME` → `gfx_clear(s, 0xFF000000)`.
> - **Starfield**: 200 star structs `{x, y, z}` init on `SCR_INIT`; each frame: advance `z`; project to screen; `gfx_put_pixel()` with brightness by depth.
> - **Matrix**: 40 column structs `{x, y, speed}`; each frame: draw green FONT_MONO chars scrolling down; fade trail with alpha blend.
> - **Bouncing Logo**: load `image_t` ImpossibleOS logo on `SCR_INIT`; each frame: advance position by velocity; flip velocity on edge collision; change accent color on corner hit; `gfx_blit()`.
> - **Clock**: `SCR_FRAME` → black fill → compute H/M/S hands from `system_get_ticks()`; `gfx_draw_line()` for each hand; hour numbers via `ttf_draw_string(FONT_UI, 14px)`.

- [ ] Add `uint64_t g_last_input_ticks` to `wm.c`; update in mouse/keyboard event handlers
- [ ] `void scr_check_idle(void)` -- compare ticks to timeout threshold; call `scr_launch()` on trip
- [ ] `int scr_launch(int type)` -- `fb_lock_compositor()`; PMM surface alloc; `scr_fn(SCR_INIT)`; enter frame loop
- [ ] `void scr_dismiss(void)` -- `scr_fn(SCR_CLOSE)`; PMM free; `fb_unlock_compositor()`
- [ ] Blank: `gfx_clear(s, 0xFF000000)` on every SCR_FRAME
- [ ] Starfield: 200-star `{float x,y,z}` array; depth projection; brightness from z
- [ ] Matrix: 40-column `{int x,y; int speed; char chars[20]}` array; FONT_MONO green cascade
- [ ] Bouncing Logo: `image_load()` logo PNG on SCR_INIT; velocity bounce off screen edges; accent color on corner
- [ ] Clock: hand angles from `uptime() % 43200` etc.; `gfx_draw_line()` h/m/s hands; `ttf_draw_string()` numerals
- [ ] Registry: `HKCU\Software\Impossible\Screensaver\Type` (0–4); `IdleTimeout` (minutes, default 5)
- [ ] `scr_check_idle()` called from compositor tick loop
- [ ] Commit: `"desktop: screensaver -- idle tracking, scr_entry_fn API, Blank/Starfield/Matrix/Logo/Clock"`

## 2. Screensaver → Lock Screen `[Sonnet]`

After dismiss: if `HKCU\Software\Impossible\Screensaver\RequirePassword=1` (default) → call `lock_screen_show()` (TODO-06 §7) instead of resuming desktop directly. Screensaver runs over locked desktop.

**Files:** extend `src/desktop/screensaver.c`

> [!NOTE]
> `scr_dismiss()` extended: `if (registry_get("HKCU\\...\\RequirePassword") == "1")` → do NOT call `fb_unlock_compositor()` immediately; instead call `lock_screen_show()` from TODO-06 §7. Lock screen takes over the full-screen context; on correct password → `fb_unlock_compositor()` + resume desktop. **Screensaver while locked**: if Win+L pressed while screensaver is already running → no state change needed (lock screen will trigger on dismiss anyway). **Registry default**: set `RequirePassword=1` during first-boot or `auth_create_user()`. **Note**: if no user account exists (early boot) → skip password requirement (single-user mode).

- [ ] Extend `scr_dismiss()`: check `HKCU\...\RequirePassword` → route to `lock_screen_show()` vs direct resume
- [ ] `lock_screen_show()` from TODO-06 §7 called with `fb_unlock_compositor` as on-success callback
- [ ] Registry: `HKCU\Software\Impossible\Screensaver\RequirePassword` (default 1, set at `auth_create_user()`)
- [ ] Screensaver Settings UI (in §5 desk.cpl or separate): `RequirePassword` checkbox; `IdleTimeout` slider; type selector
- [ ] Commit: `"desktop: screensaver lock bridge -- RequirePassword Registry, lock_screen_show() on dismiss"`

## 3. Desktop Widget Framework `[Sonnet]`

`struct widget` (id, `wgt_fn` fn, rect, z_order, visible). Lifecycle: `WGT_INIT / WGT_RENDER / WGT_TICK / WGT_CLICK / WGT_CLOSE`. `widget_manager_tick()` from compositor loop. Drawn above wallpaper, below all windows. Draggable. `gfx_acrylic` background. Registry positions.

**Files:** `src/desktop/widget_manager.c` (new), `include/desktop/widget_manager.h` (new)

> [!NOTE]
> `typedef void (*wgt_fn_t)(int msg, struct widget *w, gfx_surface_t *s, void *event_data)`. `struct widget { int id; wgt_fn_t fn; int x, y, w, h; int z_order; int visible; int dragging; int drag_ox, drag_oy; }`. Global `g_widgets[16]` table; `g_widget_count`. **Rendering order**: compositor draws wallpaper → calls `widget_manager_render(compositor_surface)` → widgets are composited in z_order ascending → then windows are drawn on top. **Widget render**: for each visible widget: `gfx_acrylic(s, w->x, w->y, w->w, w->h, radius=8, alpha=180)` as background; then `w->fn(WGT_RENDER, w, sub_surface, NULL)`. **Tick**: every compositor frame: `w->fn(WGT_TICK, w, NULL, NULL)` for each widget needing animation updates. **Mouse events**: if click lands in widget rect and NOT in any window rect → `w->fn(WGT_CLICK, w, NULL, &mouse_event)`. **Drag**: `WM_MOUSE_DOWN` on widget title area (top 20 px): enter drag mode; `WM_MOUSE_MOVE`: `w->x += delta_x; w->y += delta_y`; `WM_MOUSE_UP`: save to Registry `HKCU\Software\Impossible\Widgets\{id}\{x,y}`. **Registry load**: on `widget_manager_init()`: for each registered widget: read `HKCU\...\{id}\x` + `y` + `visible`; restore positions. **Add/remove**: right-click desktop → context menu → "Widgets" submenu → check/uncheck each widget type.

- [ ] `include/desktop/widget_manager.h`: `struct widget`, `wgt_fn_t`, `WGT_*` message constants, `widget_manager_init/tick/render/add/remove()` prototypes
- [ ] `g_widgets[16]` table; `widget_manager_init()` -- register built-in widgets, restore Registry positions
- [ ] `widget_manager_render(gfx_surface_t *compositor_surface)` -- z_order sort + `gfx_acrylic` bg + per-widget render
- [ ] `widget_manager_tick()` -- per-widget `WGT_TICK` dispatch; called from compositor loop
- [ ] Mouse hit-test: check if click inside widget rect AND not inside any window; dispatch `WGT_CLICK`
- [ ] Drag: `WM_MOUSE_DOWN` on widget → set `dragging=1`; `MOUSE_MOVE` → update `x/y`; `MOUSE_UP` → `registry_set()` positions
- [ ] Right-click desktop "Widgets" submenu toggle via `context_menu_show()` (TODO-07)
- [ ] Call `widget_manager_render()` + `widget_manager_tick()` from compositor main loop
- [ ] Commit: `"desktop: widget framework -- struct widget, WGT lifecycle, acrylic bg, drag, Registry positions"`

## 4. Built-in Widgets `[Sonnet]`

Five widgets: **Analog Clock** (150×150, `gfx_draw_line` hands), **CPU Meter** (200×100, rolling 60-s bar), **RAM Monitor** (200×80, PMM filled bar), **Mini Calendar** (200×180, month grid), **Quick Notes** (200×150, editable text).

**Files:** `src/desktop/widgets/` (new directory: `clock_widget.c`, `cpu_widget.c`, `ram_widget.c`, `cal_widget.c`, `notes_widget.c`)

> [!NOTE]
> **Analog Clock** (`WGT_INIT`: nothing; `WGT_TICK`: invalidate; `WGT_RENDER`): center `cx=75, cy=75`. Ticks = `system_get_ticks()`; seconds = `(ticks / TICKS_PER_SEC) % 60`; minutes = `uptime() / 60 % 60`; hours = `uptime() / 3600 % 12`. Hand angles in radians: `s_angle = seconds * 2π/60`; m_angle, h_angle. `gfx_draw_line(s, cx, cy, cx + sin(angle)*r, cy - cos(angle)*r, color)` for each hand (kmath_sin/cos from TODO-12 §4 stretch, or manual `sin_table[60]`). Draw 60-tick marks around rim. **CPU Meter** (`WGT_TICK`: `sched_get_task_list()` → sum CPU% → push to `g_cpu_history[60]` ring; `WGT_RENDER`): rolling bar chart: for each of 60 samples, `gfx_fill_rect(s, i*3, 100 - cpu[i], 2, cpu[i], accent_color)`. Label: `"CPU {N}%"`. **RAM Monitor** (`WGT_RENDER`): `total = pmm_get_total_frames() * 4096`; `used = total - pmm_get_free_frames()*4096`; filled bar `(used/total) * bar_w`. Label: `"{used MB} / {total MB}"`. **Mini Calendar** (`WGT_RENDER`): reuse calendar rendering logic from TODO-12 §8 -- 7-col grid, current day accent. Click day → `calendar_open_at_date()` (TODO-12). **Quick Notes** (`WGT_TICK`: nothing; `WGT_CLICK`: focus → enable `g_notes_editing` → `WM_KEYDOWN` appends to `g_notes_buf[1024]`; `WGT_RENDER`: `ttf_draw_string(FONT_UI, 12px)` wrapped; `WGT_CLOSE`: save to `HKCU\...\Notes\Content`).

- [ ] `clock_widget_fn(msg, w, s, data)` -- `sin_table[60]` + `cos_table[60]` (precomputed) for hands; 60-tick rim marks
- [ ] `cpu_widget_fn(msg, w, s, data)` -- `g_cpu_history[60]` ring; `sched_get_task_list()` on WGT_TICK; bar chart render
- [ ] `ram_widget_fn(msg, w, s, data)` -- `pmm_get_total/free_frames()` bar + label
- [ ] `cal_widget_fn(msg, w, s, data)` -- month grid; today highlight; click → `calendar_open_at_date()`
- [ ] `notes_widget_fn(msg, w, s, data)` -- `CTRL_TEXTBOX`-style: key input → `g_notes_buf`; Registry save on close/blur; word-wrap render
- [ ] Register all 5 in `widget_manager_init()` with default positions + `visible=1`
- [ ] Precomputed `sin_table[60]` + `cos_table[60]` at degree steps to avoid floating-point in ISR context
- [ ] Commit: `"desktop: built-in widgets -- analog clock, CPU meter, RAM monitor, mini calendar, quick notes"`

## 5. Display Management `[Sonnet]`

`display_enum_modes()` returns `boot_info->gop_modes[]` array. `display_get_current_mode()` from `gop_mode_selected`. Store in `HKLM\HARDWARE\Display\CurrentMode`. Update `desk.cpl` resolution `CTRL_DROPDOWN`. Stretch: `display_set_mode()` via VirtIO-GPU.

**Files:** `src/kernel/display.c` (new), `include/kernel/display.h` (new)

> [!NOTE]
> `display_enum_modes(display_mode_t *out, int max)`: copies `boot_info->gop_modes[0..gop_mode_count-1]` into `out`; returns count. `display_mode_t { uint32_t width, height, refresh_hz; }` -- `refresh_hz` stub = 60 (GOP doesn't expose refresh rate). `display_get_current_mode()`: return `boot_info->gop_modes[boot_info->gop_mode_selected]`. On boot: write `HKLM\HARDWARE\Display\Width` + `Height` + `BitsPerPixel` (always 32) to Registry. **`desk.cpl` integration**: in `desk.cpl` `CPL_DBLCLK` init, call `display_enum_modes()` → populate `CTRL_DROPDOWN` with `"{W}×{H}"` entries; current mode pre-selected. **`display_set_mode()` stretch**: requires VirtIO-GPU MMIO virtqueue command or UEFI GOP `SetMode()` (only valid in Boot Services -- unavailable after `ExitBootServices()`). If VirtIO-GPU: send `VIRTIO_GPU_CMD_SET_SCANOUT` with new resolution. Otherwise: note "Resolution changes require restart." and write `HKLM\HARDWARE\Display\PendingWidth/Height`; on next boot, bootloader applies via `gop->SetMode()`. **DPI scaling**: `HKLM\HARDWARE\Display\DPI` (default 96); `desk.cpl` DPI slider → `registry_set()` + `notify_send("Restart to apply DPI change.")`.

- [ ] `include/kernel/display.h`: `struct display_mode_t { uint32_t width, height, refresh_hz; }`, prototypes
- [ ] `src/kernel/display.c`: `display_enum_modes()` -- copy from `boot_info->gop_modes[]`; deduplicate W×H pairs
- [ ] `display_get_current_mode()` -- return `boot_info->gop_modes[boot_info->gop_mode_selected]`
- [ ] Boot write: `HKLM\HARDWARE\Display\{Width,Height,BitsPerPixel,DPI}` from current GOP mode
- [ ] `desk.cpl` update: call `display_enum_modes()` → populate `CTRL_DROPDOWN`; current mode pre-selected
- [ ] `desk.cpl` DPI slider (50–200 DPI range): `registry_set("HKLM\\HARDWARE\\Display\\DPI")`; toast to restart
- [ ] `display_set_mode()` stub: write pending W×H to Registry; show "Restart to apply" notification
- [ ] Stretch: VirtIO-GPU `VIRTIO_GPU_CMD_SET_SCANOUT` for live resolution switch
- [ ] Commit: `"kernel: display_enum_modes -- GOP mode list, boot Registry write, desk.cpl dropdown, DPI setting"`

## 6. Multi-Monitor Stubs `[Sonnet]`

`struct monitor` (id, resolution, position, DPI, framebuffer_addr). `monitor_enum()`. Virtual coordinate space stub. Taskbar on primary. Window draggable across monitors.

**Files:** extend `src/kernel/display.c`

> [!NOTE]
> Stub-only section: defines the data structures and API contracts for future multi-monitor support without wiring hardware. `struct monitor { int id; uint32_t width, height; int32_t virt_x, virt_y; uint32_t dpi; uint32_t *framebuffer_addr; int is_primary; }`. `monitor_enum(struct monitor *out, int *count)`: currently always returns 1 entry from `display_get_current_mode()` + `boot_info->framebuffer_base`. **Virtual coordinate space**: `wm_to_monitor(int virt_x, int virt_y, struct monitor **mon)` -- given a virtual desktop coordinate, return which monitor contains it + local offset. Currently trivial: always returns monitor 0. **Taskbar**: always spans `g_monitors[0]`. **Window drag across monitors**: on `WM_MOUSE_MOVE` with drag: `wm_to_monitor(new_x, new_y)` -- if result is a different monitor than window's current monitor → update window's `monitor_id`. Currently no-op since only 1 monitor. **`HKLM\HARDWARE\Display\MonitorCount`**: set to `monitor_count` at boot.

- [ ] `include/kernel/display.h`: extend with `struct monitor`, `monitor_enum()`, `wm_to_monitor()` prototypes
- [ ] `monitor_enum()` -- return single-entry array from current GOP mode + `boot_info->framebuffer_base`
- [ ] `wm_to_monitor(virt_x, virt_y)` -- trivial: always returns monitor 0 (single-monitor stub)
- [ ] `HKLM\HARDWARE\Display\MonitorCount` set to `1` at boot (foundation for future multi-monitor)
- [ ] WM window struct: add `monitor_id` field (currently always 0)
- [ ] Commit: `"kernel: multi-monitor stubs -- struct monitor, monitor_enum, wm_to_monitor, MonitorCount registry"`

---

## OS Comparison


| ⭐  | Feature                   | 🪟 Win11                                        | 🐧 Linux                                                   | 🚀 Impossible OS                                                            |
| --- | ------------------------- | ----------------------------------------------- | ---------------------------------------------------------- | --------------------------------------------------------------------------- |
| 💎  | Screensaver               | ✅ Built-in screensavers; idle timeout; lock    | ✅ GNOME/KDE screensavers; `xscreensaver`; idle timeout    | ⬜ §1 -- `⭐` `scr_entry_fn` pluggable API; 5                               |
| 💎  | Screensaver → lock bridge | ✅ Windows lock screen shown on                 | ✅ `gnome-screensaver` + PAM; `xscreensaver-auth`; `slock` | ⬜ §2 -- direct `lock_screen_show()` callback from `scr_dismiss()`          |
| ⭐  | Desktop widget framework  | ✅ Win11 Widgets panel (web-based); legacy      | ✅ KDE Plasma: QML plasmoids (separate                     | ⬜ §3 -- `⭐` in-kernel widget layer (above                                 |
| ⭐  | Built-in widgets          | ✅ Win11 Widgets: news/weather/stocks (web); no | ✅ KDE Plasma: analog clock widget,                        | ⬜ §4 -- `⭐` precomputed trig table in                                     |
| 💎  | Display management        | ✅ Display Settings: all resolutions; refresh   | ✅ `xrandr`; `arandr`; GNOME/KDE display settings;         | ⬜ §5 -- `display_enum_modes()` from `boot_info->gop_modes[]`; DPI Registry |
| 💎  | Multi-monitor stubs       | ✅ Full multi-monitor: per-monitor taskbar; DPI | ✅ `xrandr --output`; wayland `wl_output`; virtual         | ⬜ §6 -- (stubs) -- ; `monitor_enum()`; `wm_to_monitor()`                   |

> **After §1–§6:** The Impossible OS desktop shell is feature-complete for single-monitor use. The `⭐` differentiators: the widget framework runs as a compositor layer with zero separate processes (unlike KDE's QML plasmoids or Windows' Electron-based widgets panel); the screensaver uses a pluggable `scr_entry_fn` callback that runs in the compositor context with no process spawn overhead; and the CPU meter widget uses a precomputed sin/cos table (16 bytes) to avoid any floating-point dependency in the compositor rendering path.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Leave system idle for `IdleTimeout` minutes → screensaver launches (Starfield animates on screen)
- [ ] Press any key during screensaver → `RequirePassword=1` → lock screen appears; correct password → desktop resumes
- [ ] Matrix screensaver: green FONT_MONO columns falling; Bouncing Logo: logo bounces off edges; Clock: hands rotate correctly
- [ ] Right-click desktop → Widgets → enable Clock widget → analog clock appears on desktop above wallpaper but behind windows
- [ ] CPU meter widget shows CPU% changing as tasks run
- [ ] Drag clock widget to new position → persists after restart (Registry stores position)
- [ ] Quick Notes widget: click → type text → close and reopen → text preserved (Registry)
- [ ] Control Panel → Display (`desk.cpl`) → Resolution dropdown shows all GOP modes from boot_info
- [ ] DPI slider change → "Restart to apply" notification shown; `HKLM\HARDWARE\Display\DPI` updated
- [ ] Commit: `"desktop: screensaver, widget framework+5 built-ins, display management, multi-monitor stubs -- complete"`
