---
schema_version: 1
id: startmenu-tray-notifications
domain: 08-graphics-ui
status: active
title: "TODO-11 -- Start Menu, System Tray & Notifications"
---

# TODO-11 -- Start Menu, System Tray & Notifications

> **Goal:** Complete the Start Menu (live data loading, app launching, search), build the system tray (overflow chevron and the network/volume/battery cluster that opens quick settings), deliver the full toast notification pipeline with user-mode syscall, and add the notification center with history and per-app settings. Every surface follows `docs/design/shell.md`.

> [!IMPORTANT]
> **Already exists**: `desktop_draw_start_menu()` in `desktop.h` -- a static 450x344 two-column Acrylic panel in `src/desktop/desktop.c` (hard-coded entries; only Terminal, About and Power act, Power calls `acpi_shutdown()` with no confirmation). `icon_get(system_icon_t id, uint32_t size)` in `icon_store.h` for icon rendering. `vfs_readdir(dir_node, index)` + `vfs_finddir(dir_node, name)` for app directory scanning. `task_exec(data, size)` for launching apps. `anim_mgr_add()` + `GFX_EASE_OUT_CUBIC` (`TODO-04`, not built) for all slide animations. `context_menu_show()` (`TODO-09` §1, not built) for the Power button flyout. `gfx_acrylic()` for panel backgrounds. `CTRL_SLIDER` (`TODO-05` §3, not built) for the volume flyout slider. `g_focus_mode` + `focus_mode_allows_toast()` (`TODO-09` §7, not built) for DND suppression. **Missing**: Start Menu data loading, app launch wiring, search bar, system tray struct + icons, toast queue + animation, notification center, per-app settings. **Syscalls**: legacy `SYS_*` numbers end at 48 today, and the message box, progress and jump-list calls are planned but unallocated; `SYS_NOTIFY_SEND` (drafted as 54) takes the next free number when §5 adds it. Complete sections in order: data loading → interaction → search → system tray → toasts → notification center → notification settings.

## Inputs

- `include/desktop/desktop.h` -- `desktop_draw_start_menu()` (stub to extend), `TASKBAR_HEIGHT`, `desktop_in_taskbar()` -- §1 extends the static Start Menu
- `include/icon_store.h` -- `icon_get(system_icon_t id, uint32_t size)`, `icon_get_by_name()` -- used by §1 app icons and §4 tray icons
- `include/kernel/fs/vfs.h` -- `vfs_readdir()`, `vfs_finddir()` -- used by §1 to scan `C:\Impossible\Bin\` + `C:\Program Files\`
- `include/kernel/sched/task.h` -- `task_exec(data, size)` -- used by §2 to launch pinned and All apps entries
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_composite()`, `wm_mark_dirty()`, `z_order` -- used by §4 tray flyouts, §5 toast overlay, §6 notification center
- `include/gfx.h` -- `gfx_acrylic()`, `gfx_fill_rounded_rect()`, `gfx_drop_shadow()` -- Start Menu panel + tray flyouts + toast + notification center
- `include/kernel/gfx/anim_mgr.h` (`TODO-04`, not built) -- `anim_mgr_add()`, `GFX_EASE_OUT_CUBIC/IN_CUBIC` -- §2 slide open, §3 toast slide-in/out, §6 panel slide-in
- `include/desktop/context_menu.h` (`TODO-09` §1, not built) -- `context_menu_show()` -- used by §2 Power button flyout (Shut Down / Restart / Sleep / Lock)
- `include/desktop/theme.h` (`TODO-03` §2, not built) -- `theme_get()` -- all panel and toast colors use theme tokens
- `include/registry.h` -- `RegGetValue/SetValueEx` -- §1 pinned apps, §7 per-app notification settings
- `include/kernel/sched/syscall.h` -- `SYS_NOTIFY_SEND` added in §5 at the next free number; `SYS_EXEC=6` exists for app launch
- `include/desktop/focus_mode.h` (`TODO-09` §7, not built) -- `focus_mode_allows_toast()` -- §5 toast send checks DND mode
- → XREF: `08-graphics-ui/TODO-05-widget-library-core.md §3` -- `CTRL_SLIDER` used in §6 volume flyout slider
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §7` -- Focus/DND mode must exist before §5 toast suppression
- → XREF: `08-graphics-ui/TODO-10-taskbar.md §1` -- system tray icons (§4) are drawn at the right of the taskbar window list; taskbar draw loop must be stable first

## Outcome

- Start Menu populates from Registry pins + VFS app scan at boot; click launches app; search filters live.
- The Windows 11 Start layout: search on top, Pinned 6 x 3 grid with an All apps view, Recommended list, user and power footer; opens over 250 ms rising 48 px.
- Power menu: Sleep, Shut down, Restart; user menu: Lock, Sign out (context menus).
- Taskbar right side: overflow chevron, network/volume/battery cluster (opens quick settings), clock and bell.
- `notify_send(title, body, icon_id, timeout_ms)` queues toasts; `SYS_NOTIFY_SEND=54` for user-mode.
- Toasts are 364 px wide, slide in over 250 ms, stay 5 s, stack up to three; respect Focus/DND mode.
- Notification center in the shared 360 px flyout above the calendar: history grouped by app; dismiss / clear all; 100-entry Registry log.
- Per-app notification toggle + sound in Settings page.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                   | Depends On                                                             | Status |
| --- | :---: | --------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Start Menu data + Windows 11 layout -- pins grid, all apps, recommended                    | `vfs_readdir`, `icon_get`, `task_exec` (all exist)                     |  [ ]   |
| 💎  |   2   | §2 Start Menu interaction -- toggle, launch, All apps view, power + user menus                | §1 data (must exist before wiring clicks); TODO-04 `anim_mgr`          |  [ ]   |
| 💎  |   3   | §3 Search filtering -- substring match, accent highlight, arrow-nav, no-results state         | §2 (search bar is part of the open menu interaction)                   |  [ ]   |
| 💎  |   4   | §4 System tray -- overflow chevron, system cluster → quick settings, glyphs                   | §3 (Start Menu stable); TODO-10 §1 taskbar draw loop stable            |  [ ]   |
| ⭐  |   5   | §5 Toast notifications -- `notify_send()` queue, slide-in/out, stacking, `SYS_NOTIFY_SEND=54` | §4 (tray bell icon; DND mode from TODO-09 §7)                          |  [ ]   |
| 💎  |   6   | §6 Notification center -- shared flyout, history, dismiss/clear, 100-entry log                | §5 (history comes from toast queue); §4 (bell icon is the entry point) |  [ ]   |
| 💎  |   7   | §7 Notification settings -- per-app Registry toggle + sound, Settings page                    | §6 (notification center lists registered apps; settings wires to that) |  [ ]   |

---

## 1. Start Menu Data Loading `[Sonnet]`

**Design:** [`shell.md#start-menu`](../../docs/design/shell.md#start-menu), [`shell.md#materials`](../../docs/design/shell.md#materials)

**Owner of:** the work planned in `06-desktop-foundation/TODO-05 §4`, which is superseded there so the shell has one implementation.

Replace the current two-column Start menu in `src/desktop/desktop.c` with the Windows 11 layout: a single column with the search box on top, a Pinned grid of `THEME_SIZE_START_PINNED_COLS` x `THEME_SIZE_START_PINNED_ROWS` (6 x 3) tiles with an "All" pill, a Recommended list of up to six recent items in two columns, and a footer with the user and the power button. This section loads the data those regions show. `HKCU\Software\Impossible\Shell\PinnedApps` (CSV paths) → Pinned tiles; a VFS scan of `C:\Impossible\Bin\` + `C:\Program Files\` → the alphabetical "All apps" list behind the All pill; the recent-items store → Recommended.

**Files:** `src/desktop/startmenu_data.c` (new), `include/desktop/startmenu.h` (new), `src/desktop/desktop.c` (remove the two-column `SM_*` layout)

> [!NOTE]
> `struct app_entry { char name[64]; char path[256]; uint32_t icon_id; uint8_t is_heading; }`. Pins: max 18 visible (6 x 3), more page vertically; Registry CSV via `RegGetValue("HKCU\\...\\PinnedApps")` → `strsplit(csv, ',', ...)`; default pins when the key is absent: File Explorer, Settings, Terminal, Notepad, Photos, Control Panel. All apps: scan both roots for `.exe`, sort case-insensitively (`kstrcmpi`), insert a letter heading entry when the first letter changes; max 256 entries (`pmm_alloc_contiguous`). Recommended: the six most recent entries of `HKCU\Software\Impossible\Shell\RecentDocs` (name, path, timestamp), shown with a relative time. Icons: `icon_get_by_name(basename_no_ext)` → fall back to `icon_get(ICON_APP_GENERIC, 32)`; tiles draw 32 px icons (`THEME_SIZE_START_TILE_ICON`).

- [ ] `typedef struct { char name[64]; char path[256]; uint32_t icon_id; uint8_t is_heading; } app_entry_t;` and `typedef struct { char name[64]; char path[256]; uint32_t icon_id; uint64_t when_s; } recent_entry_t;` in `startmenu.h`
- [ ] `#define STARTMENU_MAX_PINS 18`, `#define STARTMENU_MAX_ALLAPPS 256`, `#define STARTMENU_MAX_RECENT 6`
- [ ] `void startmenu_data_init(void)`: load pins (defaults when absent), scan apps, load recent items
- [ ] `void startmenu_scan_apps(void)`: scan both roots; filter `.exe`; sort; insert letter headings; store in `g_allapps[]`
- [ ] `void startmenu_pins_load(void)` / `void startmenu_pins_save(void)`: Registry CSV round trip; `icon_get()` per entry
- [ ] `void startmenu_recent_load(void)`: newest six from `RecentDocs`; `void startmenu_recent_add(const char *path)` called when a file is opened through `ShellExecute`
- [ ] `void startmenu_refresh(void)`: re-run scan + pins + recent; called after app install/uninstall
- [ ] Replace the two-column `SM_*` layout in `src/desktop/desktop.c` with the single-column Windows 11 layout
  - 640 x 720 panel (`THEME_SIZE_START_WIDTH/HEIGHT`, clamped above the taskbar), centred, 12 px above the taskbar, start acrylic (`THEME_MAT_*_START_*`), radius 8, `THEME_ELEV_START_*`
  - Search box on top (36 px pill, accent underline when focused), "Pinned" heading + All pill, 6 x 3 grid of 96 x 84 tiles, "Recommended" heading + More pill, two-column recent list (32 px icon, name, relative time), 64 px footer band with avatar + user name left and power button right
- [ ] Commit: `"startmenu: Windows 11 layout -- pinned grid, recommended list, footer; Registry pins, app scan, recent items"`

## 2. Start Menu Interaction `[Sonnet]`

**Design:** [`shell.md#start-menu`](../../docs/design/shell.md#start-menu)

**Owner of:** the work planned in `06-desktop-foundation/TODO-05 §4, §5`, which is superseded there so the shell has one implementation.

Win key / Start button → toggle open/close. Click a pinned tile → `task_exec(path)` + close. The "All" pill switches the Pinned grid to the alphabetical All apps list (letter headings, scrollable) with a "Back" pill; clicking a letter heading shows the letter jump grid. Recommended item → open with its associated app; "More" → the full recent list. Footer: the user button opens account options; the power button opens the power menu (Sleep, Shut down, Restart) as a context menu. Open: fade in over `THEME_MOTION_NORMAL_MS` while rising `THEME_SIZE_START_SLIDE` (48 px) over `THEME_MOTION_START_OPEN_MS` (250) on the decelerate curve; close reverses over `THEME_MOTION_START_CLOSE_MS` (167) accelerating. Close on outside click or Escape.

**Files:** `src/desktop/startmenu.c` (extend), `include/desktop/startmenu.h` (extend)

> [!NOTE]
> State: `g_startmenu_open`, `g_startmenu_view` (`SM_VIEW_PINNED`, `SM_VIEW_ALLAPPS`, `SM_VIEW_RECENT`). Open animation: tween `g_sm_y_offset` from `THEME_SIZE_START_SLIDE` to 0 and `g_sm_alpha` from 0 to 255 with the `motion.ease_decelerate` curve; menu z_order=35000 (above flyouts at 30000-32000). View switch (Pinned ↔ All apps): cross-fade the content region over `THEME_MOTION_NORMAL_MS`; the search box and footer stay fixed. Click routing by view: tiles → `g_pins[]`, rows → `g_allapps[]` / `g_recent[]`. Launch: `vfs_open(path)` → `task_exec(data, size)` → close. Power menu: `context_menu_show(btn_x, btn_y, power_items, 3)` -- Sleep → stub, Shut down → `sys_shutdown()`, Restart → `sys_reboot()`; Lock and Sign out live in the user button's menu. Outside click in `desktop_handle_mouse()` → `startmenu_close()`.

- [ ] `void startmenu_open(void)`: set `g_startmenu_open=1`; start the slide + fade tweens; `wm_mark_dirty()`
- [ ] `void startmenu_close(void)`: reverse tweens with the accelerate curve over 167 ms; `on_complete` → `g_startmenu_open=0`
- [ ] `void startmenu_toggle(void)` + Win key intercept in `desktop_handle_key()` (`KEY_LWIN || KEY_RWIN` on key-up)
- [ ] Pinned tile click: `startmenu_launch(pin->path)`; close menu; `startmenu_launch(path)` logs `[startmenu] launched: %s`
- [ ] "All" pill → `SM_VIEW_ALLAPPS` (cross-fade); "Back" pill → `SM_VIEW_PINNED`; letter heading click → 5 x 6 letter jump grid; letter → scroll to its group
- [ ] Recommended item → open via the file association; "More" pill → `SM_VIEW_RECENT`
- [ ] Footer: user button → context menu (Change account settings, Lock, Sign out, Switch user; account actions owned by `09-desktop-shell/TODO-06 §9`); power button → context menu (Sleep, Shut down, Restart)
- [ ] Commit: `"startmenu: interaction -- toggle, launch, All apps view, recommended, power and user menus"`

## 3. Start Menu Search `[Sonnet]`

**Design:** [`shell.md#start-menu`](../../docs/design/shell.md#start-menu)

**Owner of:** the work planned in `06-desktop-foundation/TODO-05 §4`, which is superseded there so the shell has one implementation.

Typing with the menu open (or clicking the search box) filters apps, settings pages and recent files by case-insensitive substring. Results replace the Pinned and Recommended regions below the search box: a "Best match" card on the left (large icon, name, type, Open action) and the remaining matches grouped by type (Apps, Settings, Documents). Down arrow moves into the results; Enter launches the selection; no results shows "No results for "<query>"".

**Files:** `src/desktop/startmenu.c` (extend), `include/desktop/startmenu.h` (extend)

> [!NOTE]
> `g_sm_search_buf[64]`; on each change `startmenu_filter()` rebuilds `g_sm_filtered[]` (up to 20) from `g_allapps[]`, `g_pins[]`, `g_recent[]` and the settings page index using `kstrcasestr()`. The matched substring renders in the accent colour, the rest in `text_primary`. The first result is the best match. Backspace to empty returns to the Pinned view. Escape clears an active search first, then closes the menu.

- [ ] `char g_sm_search_buf[64]`, `int g_sm_search_len`, `app_entry_t g_sm_filtered[20]`, `int g_sm_filtered_count`, `int g_sm_selected_idx`
- [ ] `void startmenu_filter(const char *query)`: iterate all sources; collect matches; cap at 20; best match first
- [ ] Key handling while open: printable → append + filter; Backspace → trim; Up/Down → move selection; Enter → launch; Escape → clear search, else close
- [ ] Draw results below the search box: best-match card left, grouped list right; accent highlight on the matched substring; empty state text
- [ ] Commit: `"startmenu: search -- live filter across apps, settings and files, best match, keyboard navigation"`

## 4. System Tray Icons `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar), [`shell.md#quick-settings`](../../docs/design/shell.md#quick-settings)

`src/desktop/systray.c`. The right side of the taskbar is, left to right: an overflow chevron (28 x 40) that opens hidden tray icons, the system cluster (network, volume and battery glyphs in ONE 40 px tall button), then the clock and notification bell (`08-graphics-ui/TODO-12 §3`). Clicking the system cluster opens the single quick settings flyout owned by `08-graphics-ui/TODO-09 §8`, which holds Wi-Fi, Bluetooth, volume and brightness; there are no separate volume or network popups. App tray icons (`tray_register`) live in the overflow flyout unless the user promotes them.

**Files:** `src/desktop/systray.c` (new), `include/desktop/systray.h` (new)

> [!NOTE]
> `struct tray_icon` (icon_id, tooltip_text, click_cb, menu_cb). `SYSTRAY_MAX_ICONS=16`. Glyphs are 16 px monochrome Fluent icons tinted `text_primary`. System cluster glyph states: network (disconnected / wired / Wi-Fi strength 1-4), volume (muted / low / mid / high from `volume_get()`), battery (from `HKLM\HARDWARE\Battery\Percentage`, hidden when no battery). Hover on the cluster or chevron: `subtle_fill_hover` with a 1 px `stroke_control` outline, `radius.taskbar_button` (4). Keyboard layout: a text button ("ENG") left of the cluster when more than one layout is installed; click → context menu of layouts.

- [ ] `typedef struct { uint32_t icon_id; char tooltip[64]; void (*click_cb)(void); void (*menu_cb)(int x, int y); } tray_icon_t;` in `systray.h`
- [ ] `void tray_register(const tray_icon_t *icon)` + `void tray_unregister(uint32_t icon_id)` -- static 16-slot array; registered icons appear in the overflow flyout
- [ ] `void systray_init(void)`: build the system cluster (network, volume, battery) and the overflow chevron
- [ ] `void systray_draw(gfx_surface_t *s, int32_t x, int32_t y, int32_t h)`: chevron, optional layout button, system cluster; tooltips via the control library tooltip
- [ ] System cluster click → `quick_settings_toggle()` (`08-graphics-ui/TODO-09 §8`); chevron click → overflow flyout (menu acrylic, 3-wide grid of 32 px cells)
- [ ] Glyph updates: `systray_update_volume_glyph()` from `volume_set()`; `systray_update_net_glyph()` from `net_on_state_change()`; battery glyph from the PM composite
- [ ] Keyboard layout button: shown when more than one layout exists; click → `context_menu_show()`; callback `kbd_set_layout()`
- [ ] **Secure Boot padlock in the overflow flyout when `g_system_state.secure_boot` is 1**
  - Filed 2026-05-01 from [`01-boot-platform/TODO-02-uefi-hardening-secureboot.md §5`](../01-boot-platform/TODO-02-uefi-hardening-secureboot.md#5-secure-boot-state-detection).
  - Register a Fluent padlock glyph `tray_icon_t` with tooltip "Secure Boot: ENABLED" at `systray_init()` and on each state change.
  - Click shows a small flyout with the live status fields (`uefi_secureboot_enabled` / `uefi_secureboot_setup_mode` / `uefi_secureboot_pk_present` / `uefi_secureboot_kek_present` accessors).
  - When `uefi_secureboot_drift_detected()` returns 1 the glyph adds the `status_critical` badge and the tooltip reads "Secure Boot: DRIFT DETECTED -- firmware tampering signal" (glyph plus text, never colour alone).
- [ ] **Battery glyph, tooltip and quick-settings footer** -- kernel PM publishes the composite state, the tray renders it
  - Filed 2026-09-04 from [`02-kernel-core/TODO-26-power-management.md §29`](../02-kernel-core/TODO-26-power-management.md#29-composite-battery-state-model-power-policy-and-registry-publication).
  - Glyph variant from `HKLM\HARDWARE\Battery\Percentage` (REG_DWORD, published on every `bat_update()`): charging, discharging level, or on AC with no battery present.
  - Tooltip `"Battery: 73% -- 2h 14m remaining"` on discharge, `"Plugged in, charging"` on AC; the minutes come from the composite time estimate and read "estimating" while it is `BAT_UNKNOWN`.
  - Detail (charge, present rate in W, last-full vs design capacity) lives in the quick settings footer's battery entry; the packed composite is `HKLM\SYSTEM\Battery\Status` (REG_BINARY) so it needs one read, not one per field.
  - The composite is the ONLY source: never read a per-battery ACPI object directly, or a dual-battery machine shows one cell's percentage as the system's.
- [ ] Commit: `"systray: overflow chevron, system cluster opening quick settings, glyph states, layout button"`

## 5. Toast Notifications `[Opus]`

**Design:** [`shell.md#toast-notifications`](../../docs/design/shell.md#toast-notifications)

`notify_send(title, body, icon_id, timeout_ms)` enqueues notification. Queue: one shown at a time, stack vertically if multiple pending. Render per `docs/design/shell.md#toast-notifications`: `THEME_SIZE_TOAST_WIDTH` (364) wide flyout-acrylic card, radius 8, `THEME_ELEV_FLYOUT_*`, 12 px from the right edge and 12 px above the taskbar; top row app icon (16) + app name (caption, `text_secondary`) + close glyph on hover; title (body strong); body up to three lines; optional equal-width action buttons. Slides in from the right over `THEME_MOTION_TOAST_IN_MS` (250, decelerate), stays `THEME_MOTION_TOAST_DWELL_MS` (5 s, paused while hovered), then fades out and moves into the notification center. `SYS_NOTIFY_SEND=54` syscall.

**Files:** `src/desktop/notify.c` (new), `include/desktop/notify.h` (new), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> This is `[Opus]` -- toast notifications require a novel async queue architecture: (1) notifications from both kernel and user-mode (via syscall) must be enqueued safely; (2) one notification is shown at a time on screen; if new toasts arrive while one is showing, they stack above it with their own slide-in animations; (3) each toast has independent animation state; (4) on timeout, slide-out animation runs, then the slot is freed and the next queued toast is dequeued. **Queue**: `notify_entry_t g_notify_queue[16]` ring buffer; head/tail indices; `g_notify_active_count` (visible on screen, max 3 stacked). Each active toast: `gfx_tween_t slide_x_tween` (from `fb_w` to `fb_w - 364 - 12`, decelerate); `uint64_t show_until_ms` (start tick + timeout_ms); `gfx_tween_t hide_x_tween`. **Vertical stack**: toasts stack upward from 12 px above the taskbar, `THEME_SIZE_TOAST_GAP` (12) apart, at most three visible; each toast's height follows its content. Slide-out: when `system_get_ticks() >= show_until_ms`: start slide-out tween; `on_complete` → free slot → dequeue next. **DND check**: `focus_mode_allows_toast(PRIORITY_NORMAL)` before enqueue; if suppressed: still push to notification history (§6) even if not shown. **Syscall**: `sys_notify_send(const char *title, const char *body, uint32_t title_len, uint32_t body_len, uint32_t icon_id, uint32_t timeout_ms)` -- validates lengths; calls `notify_send()` from kernel side.

- [ ] `typedef struct { char title[64]; char body[128]; uint32_t icon_id; uint32_t timeout_ms; void (*action_cb)(void); char action_label[32]; } notify_entry_t;` in `notify.h`
- [ ] `#define NOTIFY_QUEUE_SIZE 16`, `#define NOTIFY_MAX_VISIBLE 3`
- [ ] `void notify_send(const char *title, const char *body, uint32_t icon_id, uint32_t timeout_ms)`: DND check; push to ring buffer; if `active_count < MAX_VISIBLE` → dequeue and start slide-in immediately
- [ ] `void notify_send_action(const char *title, const char *body, uint32_t icon_id, uint32_t timeout_ms, const char *action_label, void (*action_cb)(void))`: same + stores action
- [ ] `typedef struct { notify_entry_t data; gfx_tween_t slide_x; uint64_t show_until_ms; gfx_tween_t hide_x; uint8_t hiding; } active_toast_t;` -- per visible toast
- [ ] `void notify_tick(void)`: called from `wm_composite()` before draw; advance slide tweens; check `show_until_ms`; start hide tween; free finished slots; dequeue next if room
- [ ] `void notify_draw(gfx_surface_t *s)`: for each active toast: `gfx_acrylic()`; rounded-rect; icon 24 px; bold title; body; action button if set; `x = slide_x.current`
- [ ] `#define SYS_NOTIFY_SEND 54` in `syscall.h`; wire in dispatch table
- [ ] System notifications: `net_on_connect()` → `notify_send("Network", "Connected to network", ICON_NETWORK, 4000)`; `usb_on_attach()` → toast; `disk_on_low_space()` → toast
- [ ] Commit: `"notify: toast queue -- slide-in/out stacking, ring buffer, SYS_NOTIFY_SEND=54, DND-aware"`

## 6. Notification Center `[Sonnet]`

**Design:** [`shell.md#notifications-and-calendar`](../../docs/design/shell.md#notifications-and-calendar), [`shell.md#toast-notifications`](../../docs/design/shell.md#toast-notifications)

The bell beside the clock (and a clock click) opens the shared notifications-and-calendar flyout: 360 px wide (`THEME_SIZE_FLYOUT_WIDTH`), anchored 12 px from the right edge and 12 px above the taskbar, flyout acrylic, radius 8; it fades and rises 12 px over `THEME_MOTION_NORMAL_MS`. The notification list sits above the calendar owned by `08-graphics-ui/TODO-12 §3`. Notification history grouped by source app (icon + app name header, entries with title + body + relative timestamp). Dismiss individual (× button) or "Clear all". Store last 100 in `HKLM\SYSTEM\Shell\NotifyHistory`.

**Files:** `src/desktop/notify_center.c` (new), `include/desktop/notify_center.h` (new)

> [!NOTE]
> Flyout window: `wm_create_window(NULL, fb_w - 360 - 12, <above taskbar>, 360, content_h, WM_FLAG_VISIBLE)` at z_order=31000; flyout acrylic; opens with the 12 px rise + fade, closes by fading. **History storage**: `HKLM\SYSTEM\Shell\NotifyHistory\Count` DWORD + `\Entry_N\{Title, Body, AppName, Timestamp}` values; circular overwrite at 100. Each `notify_send()` call also appends to history (regardless of DND). **Relative timestamp**: `rtc_read()` at notification arrival; at display time compute `elapsed = now - timestamp`; render "Just now" / "2 min ago" / "1 hr ago" / date string. **Dismiss**: × button per entry → `RegDeleteKey("...\\Entry_N")` + rebuild visible list. "Clear all": delete all `Entry_N` keys.

- [ ] `void notify_center_open(void)`: create panel window; slide-in tween; load history from Registry; render entries
- [ ] `void notify_center_close(void)`: slide-out tween; `on_complete` → `wm_destroy_window(panel_wh)`
- [ ] `void notify_center_toggle(void)`: if open → close; else → open; called from bell icon click (§4)
- [ ] History load: `RegGetValue("...\\Count")` → iterate `Entry_0..Entry_{N-1}`; build `notify_history_entry_t[]`; sort newest first
- [ ] `void notify_history_append(const notify_entry_t *e, const char *app_name)`: write to Registry; increment/wrap count; called from `notify_send()`
- [ ] Group by app: sort/group `notify_history_entry_t[]` by `app_name`; render section header per group: icon + bold app name + entry count
- [ ] Per-entry dismiss: × hit-test at `(entry_right - 16, entry_y)`; on click → `notify_history_remove(idx)`; re-render
- [ ] "Clear all" in the "Notifications" header: `notify_history_clear_all()` → delete all Registry entries; re-render the "No new notifications" empty state
- [ ] Relative timestamp: `uint64_t elapsed_s = now_s - entry->timestamp_s`; format: `< 60s` → "Just now"; `< 3600` → "N min ago"; else → "HH:MM"
- [ ] Notification center to `docs/design/shell.md#notifications-and-calendar`: "Notifications" header with Clear all, the list or a "No new notifications" empty state, above the calendar owned by `08-graphics-ui/TODO-12 §3`
  - Flyout: 360 px wide, 12 px from the right edge and above the taskbar, `THEME_MAT_*_FLYOUT_*`, `THEME_ELEV_FLYOUT_*`
- [ ] Commit: `"notify_center: notification list in the shared calendar flyout -- 100-entry log, group-by-app, dismiss/clear"`

## 7. Notification Settings `[Sonnet]`

**Design:** [`shell.md#notifications-and-calendar`](../../docs/design/shell.md#notifications-and-calendar), [`shell.md#toast-notifications`](../../docs/design/shell.md#toast-notifications)

`HKCU\Software\Impossible\Shell\Notifications\Enabled` (global on/off). Per-app `\{app_name}\Enabled` and `\{app_name}\Sound`. Settings → Notifications page: lists all registered apps with toggle switches and sound toggles.

**Files:** `src/desktop/notify_settings.c` (new), `include/desktop/notify_settings.h` (new)

> [!NOTE]
> Per-app opt-in: when `notify_send_with_app(app_name, ...)` is called (new variant of `notify_send` with an app_name parameter), the kernel checks `HKCU\Software\Impossible\Shell\Notifications\{app_name}\Enabled` (default 1 if key absent = allow). `app_name` is `current_task->name` for syscall-originated toasts. **Settings page**: `CTRL_LISTVIEW` of all apps that have ever sent a notification (enumerate Registry keys under `Notifications\`); per row: app icon + name + enabled `CTRL_CHECKBOX` + sound `CTRL_CHECKBOX`. Global enabled toggle at top: if 0, all toasts are suppressed regardless of per-app settings. Sound: `notify_play_sound(enabled)` stub (logs to serial until audio TODO is live).

- [ ] `int notify_is_allowed(const char *app_name)`: check global `Enabled` DWORD; then per-app `\{app_name}\Enabled`; return 1 if allowed
- [ ] `void notify_send_from_app(const char *app_name, const char *title, const char *body, uint32_t icon_id, uint32_t timeout_ms)`: check `notify_is_allowed(app_name)`; if 0: push to history only, do not show; else: normal `notify_send()` flow
- [ ] Update `sys_notify_send()` (§5): use `current_task->name` as `app_name` parameter
- [ ] Settings page `notify_settings_open()`: enumerate `HKCU\...\Notifications\` subkeys → build app list; `CTRL_LISTVIEW` with per-row checkboxes; global toggle at top
- [ ] Checkbox callbacks: `notify_set_app_enabled(app_name, 0/1)` → `RegSetValueEx()`; `notify_set_sound(app_name, 0/1)` → `RegSetValueEx()`
- [ ] `void notify_play_sound(int enabled)`: if `enabled` and audio system live: play `C:\Impossible\Media\notify.wav`; else: serial log
- [ ] Global toggle: `RegSetValueEx("HKCU\\...\\Notifications\\Enabled", 0/1)`; `notify_is_allowed()` always returns 0 when global disabled
- [ ] Commit: `"notify_settings: per-app Registry toggle + sound, Settings page CTRL_LISTVIEW, global suppress"`

---

## OS Comparison


| ⭐  | Feature                 | 🪟 Win11                                                          | 🐧 Linux                                               | 🚀 Impossible OS                                                              |
| --- | ----------------------- | ----------------------------------------------------------------- | ------------------------------------------------------ | ----------------------------------------------------------------------------- |
| 💎  | Start Menu data loading | ✅ Start Menu from Start layout                                   | ✅ GNOME App Grid from `.desktop`                      | ⬜ §1 -- VFS scan of `C:\Impossible\Bin\` +                                   |
| 💎  | Start Menu interaction  | ✅ Win key toggle; click launch;                                  | ✅ GNOME Activities overlay; KDE Kickoff;              | ⬜ §2 -- `task_exec()` launch; dual `anim_mgr` tween                          |
| 💎  | Start Menu search       | ✅ Start Menu search bar; instant                                 | ✅ GNOME search (Activities); KDE Runner               | ⬜ §3 -- `kstrcasestr` substring match; accent-color highlight                |
| 💎  | System tray             | ✅ System Tray with volume/network/battery/keyboard/bell; fly-out | ✅ GNOME system indicators; KDE system                 | ⬜ §4 -- `tray_register/unregister`; volume `CTRL_SLIDER` flyout; conditional |
| ⭐  | Toast notifications     | ✅ WinRT `ToastNotification`; stacking; action buttons;           | ✅ `libnotify` / `notify-send`; GNOME/KDE notification | ⬜ §5 -- `⭐` kernel-native queue without a                                   |
| 💎  | Notification center     | ✅ Win+N notification center; grouped by                          | ✅ GNOME notification list (since 3.18);               | ⬜ §6 -- 100-entry Registry ring; "N min                                      |
| 💎  | Notification settings   | ✅ Settings → System → Notifications;                             | ✅ GNOME per-app notification settings; KDE            | ⬜ §7 `Notifications\{app_name}\Enabled+Sound`; `CTRL_LISTVIEW` settings page |

> **After §1–§7:** Impossible OS delivers the full Start Menu + tray + notification stack. The `⭐` differentiator is the toast notification system: a kernel-native ring-buffer queue with no separate daemon process, where both kernel subsystems and user-mode apps use the same code path via the `SYS_NOTIFY_SEND=54` syscall -- simpler, lower-latency, and zero IPC overhead compared to DBus-mediated Linux notification daemons.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Win key → Start opens rising 48 px over 250 ms; search box on top, Pinned 6 x 3 grid populated, Recommended list, user + power footer (matches `impossibleos.co/design/?shot#start`)
- [ ] Click pinned app → `task_exec()` launches it; serial log `[startmenu] launched: C:\...`; menu closes
- [ ] "All" pill → All apps list (letter headings) cross-fades in; "Back" returns to Pinned; letter heading → jump grid
- [ ] Type "cmd" in Start Menu → filter shows cmd.exe; matched "cmd" highlighted in accent color; Enter → launches
- [ ] Taskbar right shows the overflow chevron, the network/volume/battery cluster, the clock and the bell; hover → tooltip
- [ ] Click the system cluster → the quick settings flyout opens (`08-graphics-ui/TODO-09 §8`); changing volume there updates the cluster glyph
- [ ] Overflow chevron → flyout of registered app tray icons; the Secure Boot padlock appears there when enabled
- [ ] `notify_send("Test", "Hello world", ICON_INFO, 4000)` → 364 px toast slides in from the right over 250 ms; stays 5 s; fades into the notification center; a second toast stacks 12 px above
- [ ] Set `FOCUS_ALARMS` → `notify_send(…, PRIORITY_NORMAL)` → no toast shown; check serial log confirms history entry still written
- [ ] Click the bell → the 360 px flyout shows notifications grouped by app above the calendar; × dismisses one; "Clear all" → "No new notifications"
- [ ] Settings page → Notifications: per-app toggle off for one app; send toast from that app → no toast shown
- [ ] Shell matches the design reference: a framebuffer capture of the running shell compared with the matching `impossibleos.co/design/?shot` render (dark and light) passes the perceptual diff of `00-infrastructure/TODO-05 §9` at default threshold
- [ ] Commit: `"startmenu+tray+notify: full Start Menu data, search, system tray, toast queue, notification center"`
