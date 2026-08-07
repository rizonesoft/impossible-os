---
schema_version: 1
id: startmenu-tray-notifications
domain: 08-graphics-ui
status: active
title: "TODO-11 -- Start Menu, System Tray & Notifications"
---

# TODO-11 -- Start Menu, System Tray & Notifications

> **Goal:** Complete the Start Menu (live data loading, app launching, search), build the system tray (volume, network, notification bell, keyboard layout icons), deliver the full toast notification pipeline with user-mode syscall, and add the notification center slide-in panel with history and per-app settings.

> [!IMPORTANT]
> **Already exists**: `desktop_draw_start_menu()` in `desktop.h` -- a drawn-but-static stub (layout/Acrylic panel is done). `icon_get(system_icon_t id, uint32_t size)` in `icon_store.h` for icon rendering. `vfs_readdir(dir_node, index)` + `vfs_finddir(dir_node, name)` for app directory scanning. `task_exec(data, size)` for launching apps. `anim_mgr_add()` + `GFX_EASE_OUT_CUBIC` (TODO-02) for all slide animations. `context_menu_show()` (TODO-07 §1) for the Power button flyout. `gfx_acrylic()` for panel backgrounds. `CTRL_SLIDER` (TODO-04) for the volume flyout slider. `g_focus_mode` + `focus_mode_allows_toast()` (TODO-07 §9) for DND suppression. **Missing**: Start Menu data loading, app launch wiring, search bar, system tray struct + icons, toast queue + animation, notification center, per-app settings. **Syscalls**: `SYS_MSGBOX=51`, `SYS_TASKBAR_SET_PROGRESS=52`, `SYS_JUMPLIST_NOTIFY=53` are taken; `SYS_NOTIFY_SEND=54`. Complete sections in order: data loading → interaction → search → system tray → toasts → notification center → notification settings.

## Inputs

- `include/desktop/desktop.h` -- `desktop_draw_start_menu()` (stub to extend), `TASKBAR_HEIGHT`, `desktop_in_taskbar()` -- §1 extends the static Start Menu
- `include/icon_store.h` -- `icon_get(system_icon_t id, uint32_t size)`, `icon_get_by_name()` -- used by §1 app icons and §4 tray icons
- `include/kernel/fs/vfs.h` -- `vfs_readdir()`, `vfs_finddir()` -- used by §1 to scan `C:\Impossible\Bin\` + `C:\Program Files\`
- `include/kernel/sched/task.h` -- `task_exec(data, size)` -- used by §2 to launch pinned + All Programs apps
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_composite()`, `wm_mark_dirty()`, `z_order` -- used by §4 tray flyouts, §5 toast overlay, §6 notification center
- `include/gfx.h` -- `gfx_acrylic()`, `gfx_fill_rounded_rect()`, `gfx_drop_shadow()` -- Start Menu panel + tray flyouts + toast + notification center
- `include/kernel/gfx/anim_mgr.h` (TODO-02) -- `anim_mgr_add()`, `GFX_EASE_OUT_CUBIC/IN_CUBIC` -- §2 slide open, §3 toast slide-in/out, §6 panel slide-in
- `include/desktop/context_menu.h` (TODO-07 §1) -- `context_menu_show()` -- used by §2 Power button flyout (Shut Down / Restart / Sleep / Lock)
- `include/desktop/theme.h` (TODO-01) -- `theme_get()` -- all panel and toast colors use theme tokens
- `include/registry.h` -- `RegGetValue/SetValueEx` -- §1 pinned apps, §7 per-app notification settings
- `include/kernel/sched/syscall.h` -- `SYS_NOTIFY_SEND=54` added in §5; `SYS_EXEC=6` exists for app launch
- `include/desktop/focus_mode.h` (TODO-07 §9) -- `focus_mode_allows_toast()` -- §5 toast send checks DND mode
- → XREF: `08-graphics-ui/TODO-05-widget-library-core.md §5` -- `CTRL_SLIDER` used in §6 volume flyout slider
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §9` -- Focus/DND mode must exist before §5 toast suppression
- → XREF: `08-graphics-ui/TODO-10-taskbar.md §5` -- system tray icons (§2) are drawn at the right of the taskbar window list; taskbar draw loop must be stable first

## Outcome

- Start Menu populates from Registry pins + VFS app scan at boot; click launches app; search filters live.
- "All Programs ►" slides the left column to the alphabetical app list with 200 ms ease-out-cubic.
- Power flyout: Shut Down, Restart, Sleep, Lock via `context_menu_show()`.
- System tray renders at taskbar right: volume, network, notification bell, keyboard layout icons.
- `notify_send(title, body, icon_id, timeout_ms)` queues toasts; `SYS_NOTIFY_SEND=54` for user-mode.
- Toast slides in bottom-right 320×80 px, stacks, slides out; respects Focus/DND mode.
- Notification center (bell click): history grouped by app; dismiss / clear all; 100-entry Registry log.
- Per-app notification toggle + sound in Settings page.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                   | Depends On                                                                 | Status |
| --- | :---: | --------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Start Menu data loading -- pins, app scan, icons, right-column links                       | `vfs_readdir`, `icon_get`, `task_exec` (all exist)                        |  [ ]   |
| 💎  |   2   | §2 Start Menu interaction -- Win toggle, launch, All Programs slide, Power flyout, open anim  | §1 data (must exist before wiring clicks); TODO-02 `anim_mgr`            |  [ ]   |
| 💎  |   3   | §3 Search filtering -- prefix match, accent highlight, arrow-nav, no-results state           | §2 (search bar is part of the open menu interaction)                       |  [ ]   |
| 💎  |   4   | §4 System tray icons -- `tray_icon` struct, register/unregister, volume/network/bell/layout  | §3 (Start Menu stable); TODO-08 §5 taskbar draw loop stable               |  [ ]   |
| ⭐  |   5   | §5 Toast notifications -- `notify_send()` queue, slide-in/out, stacking, `SYS_NOTIFY_SEND=54` | §4 (tray bell icon; DND mode from TODO-07 §6)                             |  [ ]   |
| 💎  |   6   | §6 Notification center -- bell click, history panel, dismiss/clear, 100-entry Registry log   | §5 (history comes from toast queue); §4 (bell icon is the entry point)    |  [ ]   |
| 💎  |   7   | §7 Notification settings -- per-app Registry toggle + sound, Settings page                   | §6 (notification center lists registered apps; settings wires to that)     |  [ ]   |

---

## 1. Start Menu Data Loading `[Sonnet]`

`HKCU\Software\Impossible\Shell\PinnedApps` (CSV paths) → populate left-column pinned list. Scan `C:\Impossible\Bin\` + `C:\Program Files\` → build alphabetical All Programs list with letter-group headings (A, B, C…). Right-column quick links map to fixed paths. App icons via `icon_get(system_icon_t, 32)` with extension-based fallback.

**Files:** `src/desktop/startmenu_data.c` (new), `include/desktop/startmenu.h` (new)

> [!NOTE]
> Static pin list: max 12 entries; `struct app_entry { char name[64]; char path[256]; uint32_t icon_id; }`. Registry CSV: `RegGetValue("HKCU\\...\\PinnedApps")` → `strsplit(csv, ',', ...)`. App scan: `vfs_finddir(vfs_root, "C:\\Impossible\\Bin\\")` → `vfs_readdir()` loop; repeat for `C:\\Program Files\\`; collect `.exe` files only; sort case-insensitive with `kstrcmpi`; build letter-heading group list (insert a heading entry `{ is_heading=1, name="A" }` when first letter changes). All Programs list: max 256 entries (static `pmm_alloc_contiguous`). Icon selection: try `icon_get_by_name(basename_no_ext)` → on failure fall back to `icon_get(ICON_APP_GENERIC, 32)`. Right-column quick links: static `{ "Computer", "C:\\", ICON_COMPUTER }`, `{ "Documents", "C:\\Users\\Default\\Documents\\", ICON_FOLDER_DOCUMENTS }`, etc. -- 8 fixed entries.

- [ ] `typedef struct { char name[64]; char path[256]; uint32_t icon_id; uint8_t is_heading; } app_entry_t;` in `startmenu.h`
- [ ] `#define STARTMENU_MAX_PINS 12`, `#define STARTMENU_MAX_ALLPROG 256`
- [ ] `void startmenu_data_init(void)`: load pins from Registry CSV; `startmenu_scan_apps()` for All Programs; populate right-column links
- [ ] `void startmenu_scan_apps(void)`: scan `C:\Impossible\Bin\` + `C:\Program Files\`; filter `.exe`; sort; insert letter headings; store in static `g_allprog[]`
- [ ] `void startmenu_pins_load(void)`: `RegGetValue` → parse CSV → fill `g_pins[]`; load `icon_get()` per entry
- [ ] `void startmenu_pins_save(void)`: serialize `g_pins[].path` → CSV → `RegSetValueEx()`
- [ ] `void startmenu_refresh(void)`: re-run `startmenu_scan_apps()` + `startmenu_pins_load()`; called after app install/uninstall
- [ ] Right-column quick links: static array of 8 `{ label, path, icon_id }` entries; initialized once in `startmenu_data_init()`
- [ ] Commit: `"startmenu: data loading -- Registry pins, VFS app scan, alphabetical groups, right-column links"`

## 2. Start Menu Interaction `[Sonnet]`

Win key / Start button → toggle menu open/close. Click pinned → `task_exec(path)` + close. "All Programs ►" → slide left-column to app list + "Back ◄" (200 ms ease-out-cubic). Right-column links → open path. Power button → context menu flyout (Shut Down, Restart, Sleep, Lock). Open: slide-up 200 ms ease-out-cubic. Close on outside click or Escape.

**Files:** `src/desktop/startmenu.c` (extend), `include/desktop/startmenu.h` (extend)

> [!NOTE]
> Menu open state: `g_startmenu_open` flag; `g_startmenu_showing_allprog` flag for the All Programs slide. **Open animation**: tween `g_sm_y_offset` from `+MENU_H` to 0 over 200 ms `GFX_EASE_OUT_CUBIC`; menu window is at z_order=35000 (above quick settings at 30000). **All Programs slide**: tween `g_sm_col_x` from 0 to `-COL_W` (left column slides out) + `g_sm_allprog_x` from `COL_W` to 0 (all-programs slides in) simultaneously over 200 ms. "Back ◄" resets with reverse tween. **Click routing**: if `g_startmenu_showing_allprog`: clicks map to `g_allprog[]`; else: map to `g_pins[]`. Click on `app_entry` where `!is_heading`: read file via `vfs_open(path)` → `task_exec(data, size)` → close menu. **Power button flyout**: call `context_menu_show(btn_x, btn_y, power_items, 4)` with items: Shut Down → `sys_shutdown()`; Restart → `sys_reboot()`; Sleep → stub; Lock → stub. **Outside click**: `desktop_handle_mouse()`: if menu open and click outside menu bounds → `startmenu_close()`.

- [ ] `void startmenu_open(void)`: set `g_startmenu_open=1`; tween `g_sm_y_offset`; `wm_mark_dirty()`
- [ ] `void startmenu_close(void)`: tween reverse; `on_complete` → `g_startmenu_open=0`
- [ ] `void startmenu_toggle(void)`: if open → `startmenu_close()`; else → `startmenu_open()`
- [ ] Win key intercept in `desktop_handle_key()`: if `KEY_LWIN || KEY_RWIN` on key-up → `startmenu_toggle()`
- [ ] Pinned app click: `startmenu_launch(pin->path)`; close menu
- [ ] `startmenu_launch(path)`: `vfs_open(path)` → `task_exec(data, size)`; log serial `[startmenu] launched: %s`
- [ ] "All Programs ►" click: `startmenu_show_allprog()`: set flag; dual tween for left-column slide-out + all-programs slide-in
- [ ] "Back ◄" click: `startmenu_show_pins()`: reverse dual tween
- [ ] Right-column link click: if path is a directory → open in File Manager (stub: `startmenu_launch("C:\\Impossible\\System32\\explorer.exe")` with path arg); else: `startmenu_launch(path)`
- [ ] Power button click: `context_menu_show(x, y, power_menu_items, 4)` -- Shut Down / Restart / Sleep / Lock
- [ ] Commit: `"startmenu: interaction -- toggle open/close, app launch, All Programs slide, Power flyout"`

## 3. Start Menu Search `[Sonnet]`

Start Menu search bar: typing filters pinned + All Programs + right-column links by prefix match (case-insensitive). Highlight matched portion in `theme_get()->accent` color. Down arrow from search box → focus first result. Enter → launch. No-results state shows "No results found".

**Files:** `src/desktop/startmenu.c` (extend), `include/desktop/startmenu.h` (extend)

> [!NOTE]
> Search input is a text field drawn at the top of the Start Menu panel. Key presses while menu open (non-navigation keys) append to `g_sm_search_buf[64]`. On each character change: `startmenu_filter(g_sm_search_buf)` rebuilds `g_sm_filtered[]` (up to 20 results) from `g_pins[] + g_allprog[] + quicklinks[]` using `kstrcasestr(entry->name, query)`. Results are rendered in the left column in place of pins. **Highlight**: when drawing an entry, scan for the query substring; render pre-match in `theme_get()->foreground`; match in `theme_get()->accent`; post-match in `theme_get()->foreground`. Backspace: remove last char; empty query → show pins view. No-results: if `g_sm_filtered_count == 0`: draw "No results found" in `theme_get()->foreground_muted` centered in left column.

- [ ] `char g_sm_search_buf[64]` + `int g_sm_search_len` + `app_entry_t g_sm_filtered[20]` + `int g_sm_filtered_count`
- [ ] `void startmenu_filter(const char *query)`: iterate all sources; `kstrcasestr(entry->name, query)` → collect matches; cap at 20
- [ ] `int g_sm_selected_idx` → arrow-key navigation in filtered results; `Enter` → `startmenu_launch(g_sm_filtered[g_sm_selected_idx].path)`
- [ ] Key handler while menu open: printable chars → append to search buf → `startmenu_filter()`; Backspace → trim; Down/Up → move `g_sm_selected_idx`; Enter → launch selected; Escape → if search active → clear search; else → `startmenu_close()`
- [ ] Draw search results: if `g_sm_search_len > 0` override left column with `g_sm_filtered[]`; highlight matched portion; no-results text if count == 0
- [ ] Commit: `"startmenu: search -- live prefix filter, accent highlight, arrow-nav, no-results state"`

## 4. System Tray Icons `[Sonnet]`

`src/desktop/systray.c`. `struct tray_icon` (icon_id, tooltip_text, click_cb, flyout_cb). `tray_register(icon)` / `tray_unregister(icon)`. Built-in icons: 🔊 volume (click → slider flyout), 🌐 network (click → status popup), 🔔 bell (click → notification center §6), "EN"/"FR" keyboard layout label. Battery icon conditional on Registry hardware key.

**Files:** `src/desktop/systray.c` (new), `include/desktop/systray.h` (new)

> [!NOTE]
> Tray area: right of taskbar window list; icons are 16 px each with 4 px padding; drawn right-to-left (clock → battery → bell → keyboard → network → volume). `SYSTRAY_MAX_ICONS=16` static array. **Volume flyout**: `wm_create_window(NULL, tray_x - 60, tray_y - 140, 60, 140, WM_FLAG_VISIBLE)` at z_order=32000; Acrylic background; `CTRL_SLIDER` vertical 0–100 for volume; current value from `volume_get()` stub; real-time `on_change` → `volume_set(v)` stub; speaker icon showing muted/low/mid/high variant via `icon_get(ICON_SPEAKER_*, 24)`. **Network flyout**: 200×100 px popup; IP address `net_get_ip_str()`, link state `net_is_connected()`, SSID stub; close on outside click. **Keyboard layout**: `kbd_get_layout_name()` stub returns "EN"; click → `context_menu_show()` with available layouts. **Battery**: check `HKLM\HARDWARE\Battery\Percentage` DWORD; if present: show battery icon with percentage text; charge % drives icon variant.

- [ ] `typedef struct { uint32_t icon_id; char tooltip[64]; void (*click_cb)(void); void (*flyout_cb)(int x, int y); } tray_icon_t;` in `systray.h`
- [ ] `void tray_register(const tray_icon_t *icon)` + `void tray_unregister(uint32_t icon_id)` -- static 16-slot array
- [ ] `void systray_init(void)`: register built-in icons (volume, network, bell, keyboard layout); conditional battery
- [ ] `void systray_draw(gfx_surface_t *s, int32_t x, int32_t y, int32_t w)`: draw icons right-to-left; clock text (`rtc_read()` → HH:MM); battery % if present; tooltip on hover (`context_menu_show()` stub for tooltip or `desktop_tooltip_show()` from TODO-05)
- [ ] Volume flyout: `systray_show_volume_flyout(x, y)`: create 60×140 px Acrylic popup at z_order=32000; vertical `CTRL_SLIDER`; `on_change` calls `volume_set()`; speaker icon updates on change
- [ ] Network flyout: `systray_show_net_flyout(x, y)`: 200×100 px; IP, link state, SSID; built from `gfx_fill_rounded_rect` + `gfx_text`
- [ ] Bell icon click: `notify_center_toggle()` (§6)
- [ ] Keyboard layout click: `context_menu_show()` with layouts list; callback `kbd_set_layout()` stub
- [ ] Dynamic icon updates: `systray_update_volume_icon()` called from `volume_set()`; `systray_update_net_icon()` from `net_on_state_change()` hook
- [ ] **Padlock icon when `g_system_state.secure_boot` is 1** (filed 2026-05-01 from [`01-boot-platform/TODO-02-uefi-hardening-secureboot.md §5`](../01-boot-platform/TODO-02-uefi-hardening-secureboot.md#5-secure-boot-state-detection)): conditional tray icon mirroring the battery-icon pattern -- check `g_system_state.secure_boot` at `systray_init()` and on each state change; if 1, register a padlock `tray_icon_t` with tooltip "Secure Boot: ENABLED". Click handler shows a small popup with the live SecureBoot status fields (`uefi_secureboot_enabled` / `uefi_secureboot_setup_mode` / `uefi_secureboot_pk_present` / `uefi_secureboot_kek_present` accessors). When `uefi_secureboot_drift_detected()` returns 1 the icon switches to a red padlock variant + tooltip "Secure Boot: DRIFT DETECTED -- firmware tampering signal".
- [ ] Commit: `"systray: tray icons -- volume/network/bell/keyboard flyouts, battery conditional, right-to-left draw"`

## 5. Toast Notifications `[Opus]`

`notify_send(title, body, icon_id, timeout_ms)` enqueues notification. Queue: one shown at a time, stack vertically if multiple pending. Render: bottom-right 320×80 px Acrylic panel, rounded corners, icon (24 px) + bold title + body + optional [Action] button. Slide-in from right (300 ms ease-out-cubic), hold, slide-out right (150 ms). `SYS_NOTIFY_SEND=54` syscall.

**Files:** `src/desktop/notify.c` (new), `include/desktop/notify.h` (new), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> This is `[Opus]` -- toast notifications require a novel async queue architecture: (1) notifications from both kernel and user-mode (via syscall) must be enqueued safely; (2) one notification is shown at a time on screen; if new toasts arrive while one is showing, they stack above it with their own slide-in animations; (3) each toast has independent animation state; (4) on timeout, slide-out animation runs, then the slot is freed and the next queued toast is dequeued. **Queue**: `notify_entry_t g_notify_queue[16]` ring buffer; head/tail indices; `g_notify_active_count` (visible on screen, max 4 stacked). Each active toast: `gfx_tween_t slide_x_tween` (from `fb_w` to `fb_w - 330`, ease-out); `uint64_t show_until_ms` (start tick + timeout_ms); `gfx_tween_t hide_x_tween`. **Vertical stack**: toast N renders at `fb_h - taskbar_h - (N * (80 + 8)) - 8` y. Slide-out: when `system_get_ticks() >= show_until_ms`: start slide-out tween; `on_complete` → free slot → dequeue next. **DND check**: `focus_mode_allows_toast(PRIORITY_NORMAL)` before enqueue; if suppressed: still push to notification history (§6) even if not shown. **Syscall**: `sys_notify_send(const char *title, const char *body, uint32_t title_len, uint32_t body_len, uint32_t icon_id, uint32_t timeout_ms)` -- validates lengths; calls `notify_send()` from kernel side.

- [ ] `typedef struct { char title[64]; char body[128]; uint32_t icon_id; uint32_t timeout_ms; void (*action_cb)(void); char action_label[32]; } notify_entry_t;` in `notify.h`
- [ ] `#define NOTIFY_QUEUE_SIZE 16`, `#define NOTIFY_MAX_VISIBLE 4`
- [ ] `void notify_send(const char *title, const char *body, uint32_t icon_id, uint32_t timeout_ms)`: DND check; push to ring buffer; if `active_count < MAX_VISIBLE` → dequeue and start slide-in immediately
- [ ] `void notify_send_action(const char *title, const char *body, uint32_t icon_id, uint32_t timeout_ms, const char *action_label, void (*action_cb)(void))`: same + stores action
- [ ] `typedef struct { notify_entry_t data; gfx_tween_t slide_x; uint64_t show_until_ms; gfx_tween_t hide_x; uint8_t hiding; } active_toast_t;` -- per visible toast
- [ ] `void notify_tick(void)`: called from `wm_composite()` before draw; advance slide tweens; check `show_until_ms`; start hide tween; free finished slots; dequeue next if room
- [ ] `void notify_draw(gfx_surface_t *s)`: for each active toast: `gfx_acrylic()`; rounded-rect; icon 24 px; bold title; body; action button if set; `x = slide_x.current`
- [ ] `#define SYS_NOTIFY_SEND 54` in `syscall.h`; wire in dispatch table
- [ ] System notifications: `net_on_connect()` → `notify_send("Network", "Connected to network", ICON_NETWORK, 4000)`; `usb_on_attach()` → toast; `disk_on_low_space()` → toast
- [ ] Commit: `"notify: toast queue -- slide-in/out stacking, ring buffer, SYS_NOTIFY_SEND=54, DND-aware"`

## 6. Notification Center `[Sonnet]`

🔔 notification bell in tray → slide-in right-edge panel (320 px wide, full height minus taskbar, 200 ms ease-out-cubic). Notification history grouped by source app (icon + app name header, entries with title + body + relative timestamp). Dismiss individual (× button) or "Clear all". Store last 100 in `HKLM\SYSTEM\Shell\NotifyHistory`.

**Files:** `src/desktop/notify_center.c` (new), `include/desktop/notify_center.h` (new)

> [!NOTE]
> Panel window: `wm_create_window(NULL, fb_w - 320, TASKBAR_H (top) or 0, 320, usable_h, WM_FLAG_VISIBLE)` at z_order=31000. Acrylic background. Slide-in: same pattern as quick settings (TODO-07 §7) -- tween x from `fb_w` to `fb_w - 320` over 200 ms `GFX_EASE_OUT_CUBIC`; slide-out on close. **History storage**: `HKLM\SYSTEM\Shell\NotifyHistory\Count` DWORD + `\Entry_N\{Title, Body, AppName, Timestamp}` values; circular overwrite at 100. Each `notify_send()` call also appends to history (regardless of DND). **Relative timestamp**: `rtc_read()` at notification arrival; at display time compute `elapsed = now - timestamp`; render "Just now" / "2 min ago" / "1 hr ago" / date string. **Dismiss**: × button per entry → `RegDeleteKey("...\\Entry_N")` + rebuild visible list. "Clear all": delete all `Entry_N` keys.

- [ ] `void notify_center_open(void)`: create panel window; slide-in tween; load history from Registry; render entries
- [ ] `void notify_center_close(void)`: slide-out tween; `on_complete` → `wm_destroy_window(panel_wh)`
- [ ] `void notify_center_toggle(void)`: if open → close; else → open; called from bell icon click (§4)
- [ ] History load: `RegGetValue("...\\Count")` → iterate `Entry_0..Entry_{N-1}`; build `notify_history_entry_t[]`; sort newest first
- [ ] `void notify_history_append(const notify_entry_t *e, const char *app_name)`: write to Registry; increment/wrap count; called from `notify_send()`
- [ ] Group by app: sort/group `notify_history_entry_t[]` by `app_name`; render section header per group: icon + bold app name + entry count
- [ ] Per-entry dismiss: × hit-test at `(entry_right - 16, entry_y)`; on click → `notify_history_remove(idx)`; re-render
- [ ] "Clear all" button at top of panel: `notify_history_clear_all()` → delete all Registry entries; `notify_center_open()` re-renders empty state ("All clear ✓")
- [ ] Relative timestamp: `uint64_t elapsed_s = now_s - entry->timestamp_s`; format: `< 60s` → "Just now"; `< 3600` → "N min ago"; else → "HH:MM"
- [ ] Commit: `"notify_center: history panel -- Registry 100-entry log, group-by-app, dismiss/clear, slide-in"`

## 7. Notification Settings `[Sonnet]`

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


| ⭐  | Feature                 | 🪟 Win11                                                          | 🐧 Linux                                               | 🚀 Impossible OS                                                                          |
| --- | ----------------------- | ----------------------------------------------------------------- | ------------------------------------------------------ | ----------------------------------------------------------------------------------------- |
| 💎  | Start Menu data loading | ✅ Start Menu from Start layout                                   | ✅ GNOME App Grid from `.desktop`                      | ⬜ §1 -- VFS scan of `C:\Impossible\Bin\` +                                               |
| 💎  | Start Menu interaction  | ✅ Win key toggle; click launch;                                  | ✅ GNOME Activities overlay; KDE Kickoff;              | ⬜ §2 -- `task_exec()` launch; dual `anim_mgr` tween                                      |
| 💎  | Start Menu search       | ✅ Start Menu search bar; instant                                 | ✅ GNOME search (Activities); KDE Runner               | ⬜ §3 -- `kstrcasestr` prefix match; accent-color highlight                               |
| 💎  | System tray             | ✅ System Tray with volume/network/battery/keyboard/bell; fly-out | ✅ GNOME system indicators; KDE system                 | ⬜ §4 -- `tray_register/unregister`; volume `CTRL_SLIDER` flyout; conditional             |
| ⭐  | Toast notifications     | ✅ WinRT `ToastNotification`; stacking; action buttons;           | ✅ `libnotify` / `notify-send`; GNOME/KDE notification | ⬜ §5 -- `⭐` kernel-native queue without a                                               |
| 💎  | Notification center     | ✅ Win+N notification center; grouped by                          | ✅ GNOME notification list (since 3.18);               | ⬜ §6 -- 100-entry Registry ring; "N min                                                  |
| 💎  | Notification settings   | ✅ Settings → System → Notifications;                             | ✅ GNOME per-app notification settings; KDE            | ⬜ §7 -- `HKCU\...\Notifications\{app_name}\Enabled+Sound`; `CTRL_LISTVIEW` settings page |

> **After §1–§7:** Impossible OS delivers the full Start Menu + tray + notification stack. The `⭐` differentiator is the toast notification system: a kernel-native ring-buffer queue with no separate daemon process, where both kernel subsystems and user-mode apps use the same code path via the `SYS_NOTIFY_SEND=54` syscall -- simpler, lower-latency, and zero IPC overhead compared to DBus-mediated Linux notification daemons.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Win key → Start Menu opens with slide-up animation; pins list populated; right column shows Computer/Documents/etc.
- [ ] Click pinned app → `task_exec()` launches it; serial log `[startmenu] launched: C:\...`; menu closes
- [ ] "All Programs ►" click → left column slides out, All Programs list slides in (A/B/C headings); "Back ◄" reverses
- [ ] Type "cmd" in Start Menu → filter shows cmd.exe; matched "cmd" highlighted in accent color; Enter → launches
- [ ] System tray shows volume, network, bell, keyboard icons; hover each → tooltip appears
- [ ] Click volume icon → slider flyout; drag slider → volume updates; click mute → speaker icon changes to muted variant
- [ ] Click network icon → network status popup with IP address and link state
- [ ] `notify_send("Test", "Hello world", ICON_INFO, 4000)` → toast slides in from right; holds 4 s; slides out; second toast arrives → stacks above first
- [ ] Set `FOCUS_ALARMS` → `notify_send(…, PRIORITY_NORMAL)` → no toast shown; check serial log confirms history entry still written
- [ ] Click bell icon → notification center slides in from right; toasts grouped by app with headers; × button dismisses individual; "Clear all" → panel shows "All clear"
- [ ] Settings page → Notifications: per-app toggle off for one app; send toast from that app → no toast shown
- [ ] Commit: `"startmenu+tray+notify: full Start Menu data, search, system tray, toast queue, notification center"`
