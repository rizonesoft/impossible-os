---
schema_version: 1
id: control-panel
domain: 09-desktop-shell
status: active
title: "TODO-11 -- Control Panel & Settings"
---

# TODO-11 -- Control Panel & Settings

> **Goal:** Build the Control Panel -- a Windows CPL-compatible settings hub that ties all underlying APIs together (theme, DPI, network, time, user accounts, audio, power) into one discoverable place, with a Win32-compatible `cpl.h` that lets Win32 `.cpl` applets run natively.

> [!IMPORTANT]
> **Already exists**: `cpuid_get()->brand` (49-char brand string) + `cpuid_get()->vendor` in `cpuid.h`. `pmm_get_total_frames()` in `pmm.h` (total RAM = frames × 4096). `acpi_get_cpu_count()` + `smp_cpu_count()`. `uptime()` → uint64 seconds in `timer.h`. `vfs_readdir` for `.cpl` file scanning. `registry_get/set` for all per-applet settings. `icon_draw_scaled()` for sidebar icons. **Forward deps (must exist before respective applet)**: `wallpaper_set()` (TODO-07 §2), `g_dpi_pct`/`DPI_SCALE` (TODO-07 §5), `CTRL_SLIDER` (TODO-04 §2), `CTRL_CHECKBOX` (TODO-04 §2), `ntp_sync()` (TODO-04 §1 scheduled task), `auth_create/delete_user/change_password()` (TODO-06 §1), `firewall_enable/rule_add()` (06-networking/TODO-05), `audio_set_volume()` (future audio TODO), `shortcut_execute()` (TODO-02 §5). **Missing**: `cpl.h` entirely, `src/apps/control/`, all applet `.c` files. Complete sections in order: CPL framework → host app → core applets → additional applets → settings search.

## Inputs

- `include/kernel/cpuid.h` -- `cpuid_get()->brand/vendor/model`, `cpuid_get()->core_count` -- `sysdm.cpl` CPU info
- `include/kernel/mm/pmm.h` -- `pmm_get_total_frames()` -- `sysdm.cpl` installed RAM
- `include/kernel/acpi.h` -- `acpi_get_cpu_count()` -- `sysdm.cpl` CPU count
- `include/kernel/timer.h` -- `uptime()` seconds -- `sysdm.cpl` system uptime
- `include/registry.h` -- all Registry read/write for applet settings persistence
- `include/kernel/fs/vfs.h` -- `vfs_readdir()` for `.cpl` file scanning in `C:\Impossible\System32\`
- `include/icon_store.h` -- `icon_draw_scaled()` -- sidebar applet icons
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_draw_circle()`, accent color wheel in `desk.cpl`
- `include/font_mgr.h` -- `ttf_draw_string()`, `ttf_measure_width()` -- all applet labels
- `include/desktop/controls.h` (TODO-04 §2) -- `CTRL_SLIDER`, `CTRL_CHECKBOX`, `CTRL_DROPDOWN` -- all applets
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_set_title()` -- §2 host window + applet sub-windows
- `include/desktop/desktop.h` -- `wallpaper_set()` (TODO-07 §2 fwd) -- `desk.cpl`
- `include/kernel/auth.h` (TODO-06) -- `auth_create/delete_user/change_password/list_users()` -- `nusrmgr.cpl`
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §2` -- `wallpaper_set()` needed for `desk.cpl`
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §5` -- `g_dpi_pct` + DPI hot-change for `desk.cpl`
- → XREF: `08-graphics-ui/TODO-05-widget-library-core.md §3` -- `CTRL_SLIDER` needed for volume, DPI, cursor applets
- → XREF: `08-graphics-ui/TODO-12-clock-time.md §1` -- `time_now/set/set_timezone()` + `SYS_TIME` for `timedate.cpl`
- → XREF: `06-networking/TODO-05-firewall.md` -- `firewall_enable/rule_add/remove()` for `firewall.cpl`
- → XREF: `06-networking/TODO-06-ntp-status-winsock.md` -- `ntp_sync()` for `timedate.cpl` sync button

## Outcome

- Win32-compatible `include/cpl.h` with `CPL_*` messages, `NEWCPLINFO`, `CPlApplet` function pointer.
- Control Panel host app (`src/apps/control/`) with category sidebar + applet grid; scans `C:\Impossible\System32\` for `.cpl` files; Win+I shortcut.
- 9 core applets: `sysdm`, `desk`, `ncpa`, `mmsys`, `timedate`, `powercfg`, `main` (mouse), `intl`, `taskbar.cpl`.
- 4 additional applets: `nusrmgr`, `appwiz`, `firewall`, `datetime` alias.
- Settings search bar: in-process name+description match, Enter opens first result.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                      | Depends On                                                                              | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 CPL framework -- `cpl.h`, `CPL_*` messages, `NEWCPLINFO`, `CPlApplet` fn ptr                  | no deps (pure interface definition)                                                     |  [ ]   |
| 💎  |   2   | §2 Host app -- `src/apps/control/`, category sidebar, applet grid, `.cpl` scan, Win+I            | §1 CPL framework; `vfs_readdir`, `icon_draw_scaled`, `wm_create_window` (exist)        |  [ ]   |
| 💎  |   3   | §3 Core applets -- `sysdm/desk/ncpa/mmsys/timedate/powercfg/main/intl/taskbar.cpl`               | §1 CPL; §2 host; all forward dep APIs (see notes per applet)                           |  [ ]   |
| 💎  |   4   | §4 Additional applets -- `nusrmgr/appwiz/firewall/datetime.cpl`                                  | §1 CPL; §2 host; TODO-06 auth (nusrmgr); TODO-05-firewall.md (firewall.cpl)           |  [ ]   |
| ⭐  |   5   | §5 Settings search -- search bar, in-process name+desc filter, Enter opens first match           | §2 host (applet registry in memory); all applets registered                             |  [ ]   |

---

## 1. CPL Framework `[Sonnet]`

`include/cpl.h` matching Windows `cpl.h`: `CPL_INIT/GETCOUNT/INQUIRE/NEWINQUIRE/DBLCLK/STOP/EXIT` messages; `NEWCPLINFO` struct; `CPlApplet` function pointer; Win32-compatible flag values.

**Files:** `include/cpl.h` (new), `include/apps/control.h` (new)

> [!NOTE]
> Exact Windows CPL message values: `CPL_INIT=1`, `CPL_GETCOUNT=2`, `CPL_INQUIRE=3`, `CPL_DBLCLK=5`, `CPL_STOP=6`, `CPL_EXIT=7`, `CPL_NEWINQUIRE=8`. `NEWCPLINFO` struct: `{ DWORD dwSize; DWORD dwFlags; DWORD dwHelpContext; LONG lData; HICON hIcon; char szName[32]; char szInfo[64]; char szHelpFile[128]; }` -- use Impossible OS types (`uint32_t` etc.) but keep layout compatible. `CPlApplet` type: `LONG (CALLBACK *APPLET_PROC)(HWND hwndCPl, UINT uMsg, LPARAM lParam1, LPARAM lParam2)` -- in Impossible OS: `typedef int (*CPlApplet_t)(uint32_t hwnd, uint32_t msg, uintptr_t lParam1, uintptr_t lParam2)`. Win32 apps calling `ShellExecute("desk.cpl")` will route through `shell_execute()` (TODO-02 §5) → `cpl_load_and_run(path)`. CPL lifecycle: `CPlApplet(hwnd, CPL_INIT, 0, 0)` → non-zero = success; `CPlApplet(hwnd, CPL_GETCOUNT, 0, 0)` → number of applets in this DLL; `CPlApplet(hwnd, CPL_NEWINQUIRE, idx, &info)` → fills `NEWCPLINFO`; `CPlApplet(hwnd, CPL_DBLCLK, idx, lData)` → open applet UI; `CPlApplet(hwnd, CPL_STOP, idx, lData)` → cleanup; `CPlApplet(hwnd, CPL_EXIT, 0, 0)` → unload.

- [ ] `include/cpl.h` -- `#define CPL_INIT 1`, ..., `CPL_NEWINQUIRE 8`; `NEWCPLINFO` struct; `typedef int (*CPlApplet_t)(...)`;
- [ ] `#define CPL_OK 0`, `CPL_ERR -1` return values
- [ ] `include/apps/control.h` -- `cpl_entry_t { char name[32]; char desc[64]; int icon_id; char category[32]; CPlApplet_t fn; }` for compiled-in applets
- [ ] `int cpl_load_and_run(const char *cpl_path)` -- future: load `.cpl` as dynamic lib; for now: scan compiled-in table by filename match
- [ ] Commit: `"cpl: framework -- cpl.h Win32-compatible messages, NEWCPLINFO, CPlApplet_t typedef"`

## 2. Control Panel Host App `[Sonnet]`

`src/apps/control/control.c`; two-panel layout (category sidebar + applet grid). Scan `C:\Impossible\System32\` for `.cpl` files + compiled-in applets. Group by category. `CPL_DBLCLK` → render UI. Win+I opens.

**Files:** `src/apps/control/control.c` (new), `include/apps/control.h` (extend)

> [!NOTE]
> Window: 900×600 px. Left panel: 200 px category sidebar with category name + icon; click → filter applet grid to that category. Right panel: applet grid (icon + name + description); each applet = 160×100 px cell with `icon_draw_scaled(ICON_SIZE_48)` + `ttf_draw_string()` name + small description. **Categories**: "System", "Personalization", "Network and Internet", "Hardware and Sound", "User Accounts", "Date and Time", "Programs". **Back button**: when an applet is open (showing its UI full-right-panel): click Back → close applet panel; return to grid. **CPL dispatch**: `control_open_applet(idx)`: `applet->fn(hwnd, CPL_DBLCLK, 0, applet->lData)` -- applet renders its own UI into the right panel sub-surface. Each applet gets a `gfx_surface_t *sub` passed as hwnd (cast); it renders to this surface. **Win+I hotkey**: global hotkey table entry (TODO-06 §8) → `control_open()`. **Compiled-in applets**: `g_cpl_entries[]` static array populated at init; `control_scan_cpl_dir()` adds any `.cpl` files found in `C:\Impossible\System32\` by calling `cpl_load_and_run()`. **Keyboard nav**: arrow keys move focus between applet cells; Enter = open; Escape = Back.

- [ ] `cpl_entry_t g_cpl_entries[32]` + `g_cpl_count` -- compiled-in + scanned
- [ ] `void control_init(void)` -- populate `g_cpl_entries` with compiled-in applets + scan dir
- [ ] `void control_open(void)` -- `wm_create_window(80, 50, 900, 600, "Control Panel", ...)`
- [ ] `void control_render_sidebar(s, x, y, w, h)` -- category list; active highlight
- [ ] `void control_render_grid(s, x, y, w, h, category_filter)` -- 160×100 px applet cells; icon + name + desc
- [ ] `void control_open_applet(int idx)` -- `CPL_DBLCLK`; render applet's UI sub-surface in right panel
- [ ] Back button: `g_active_applet = -1`; re-render grid
- [ ] Win+I hotkey: registered in global hotkey table → `control_open()`
- [ ] Keyboard nav: arrow keys cycle cells; Enter → `control_open_applet(focused)`; Escape → Back
- [ ] Commit: `"control: host app -- category sidebar, applet grid, CPL_DBLCLK dispatch, Win+I hotkey, keyboard nav"`

## 3. Core Applets `[Sonnet]`

Nine applets: `sysdm.cpl` (System), `desk.cpl` (Display), `ncpa.cpl` (Network), `mmsys.cpl` (Sound), `timedate.cpl` (Date/Time), `powercfg.cpl` (Power), `main.cpl` (Mouse), `intl.cpl` (Region), `taskbar.cpl` (Taskbar).

**Files:** `src/apps/control/applets/` (new directory, one `.c` per applet)

> [!NOTE]
> Each applet is a `.c` file exposing a `CPlApplet_t` function registered in `g_cpl_entries[]`. All use `CTRL_SLIDER`/`CTRL_CHECKBOX`/`CTRL_DROPDOWN` (TODO-04 forward deps) and `registry_get/set` for persistence. Details per applet:
>
> **`sysdm.cpl` -- System Properties**: render OS version (`"Impossible OS 1.0"`), CPU brand (`cpuid_get()->brand`), CPU count (`acpi_get_cpu_count()`), installed RAM (`pmm_get_total_frames() * 4096 / (1024*1024)` MiB), system uptime (`uptime()` → `"Xd Xh Xm"`), hostname (Registry `HKLM\SYSTEM\ComputerName`); "Computer Name" editable text field + Apply.
>
> **`desk.cpl` -- Display**: resolution picker (`CTRL_DROPDOWN` listing available GOP modes via `gop_get_mode_list()`); DPI scale dropdown (100/125/150/200% → `g_dpi_pct`; calls `DPI_CHANGED` broadcast); wallpaper picker (thumbnail strip + Browse button → `dialog_file_open()` for image → `wallpaper_set(path, fit_mode)`); accent color picker (8 preset swatches + "Custom" → hue/saturation wheel via `gfx_draw_hue_ring()`); Dark/Light mode toggle → `HKCU\...\Theme\Mode` + `WM_THEME_CHANGED` broadcast.
>
> **`ncpa.cpl` -- Network**: DHCP/Static toggle (`CTRL_CHECKBOX`); IP/Mask/Gateway/DNS text fields (disabled when DHCP); hostname field; Apply → write Registry `HKLM\SYSTEM\Network\*`; status display: MAC, current IP, link state, Rx/Tx bytes via `net_get_status()`.
>
> **`mmsys.cpl` -- Sound**: master volume `CTRL_SLIDER` (0–100) → `audio_set_volume(val)` (forward ref to audio TODO); mute `CTRL_CHECKBOX`; output device dropdown; "Test Sound" button → `audio_play_sound(SOUND_TEST)`.
>
> **`timedate.cpl` -- Date and Time**: analog clock (draw hour/minute/second hands via `gfx_draw_line()`); digital clock (HH:MM:SS updating 1/s); timezone `CTRL_DROPDOWN`; 12h/24h `CTRL_CHECKBOX`; "Sync with NTP" button → `ntp_sync()`; manual date/time fields; Apply → `time_set(new_ts)` + `time_set_timezone(tz_offset_min)`.
>
> **`powercfg.cpl` -- Power Options**: screen timeout `CTRL_SLIDER` (1–60 min); sleep timeout `CTRL_SLIDER`; Shutdown/Restart/Sleep action buttons wired to `sys_shutdown()/sys_reboot()/sys_sleep()` (TODO-15 power management); battery indicator if `acpi_battery_present()`.
>
> **`main.cpl` -- Mouse Properties**: cursor theme picker (scan `C:\Impossible\System\Cursors\` for themes); cursor size `CTRL_SLIDER` (16–64 px); pointer speed sensitivity `CTRL_SLIDER` (1–10); left/right hand swap `CTRL_CHECKBOX`; cursor preview area.
>
> **`intl.cpl` -- Region**: date format dropdown (MM/DD/YYYY, DD/MM/YYYY, YYYY/MM/DD); time format dropdown (12h/24h); decimal separator text field (`.` or `,`); thousands separator; currency symbol field; all persist to `HKCU\Control Panel\International\*`.
>
> **`taskbar.cpl` -- Taskbar**: height preset dropdown (Small=28/Medium=40/Large=52 px); position dropdown (Bottom/Top/Left/Right); auto-hide `CTRL_CHECKBOX`; all write to `HKCU\...\Taskbar\*` + broadcast `WM_TASKBAR_CHANGED`.

- [ ] `src/apps/control/applets/sysdm.c` -- OS/CPU/RAM/uptime/hostname; Computer Name field + Apply
- [ ] `src/apps/control/applets/desk.c` -- GOP mode list; DPI dropdown; wallpaper picker; accent color swatches; Dark/Light toggle
- [ ] `gop_get_mode_list(modes_out, max)` helper (read existing GOP mode table from bootloader hand-off)
- [ ] `src/apps/control/applets/ncpa.c` -- DHCP/Static; IP/Mask/Gateway/DNS fields; hostname; `net_get_status()`
- [ ] `src/apps/control/applets/mmsys.c` -- volume `CTRL_SLIDER`; mute; test sound (forward ref to audio TODO)
- [ ] `src/apps/control/applets/timedate.c` -- analog clock render; timezone dropdown; 12/24h; NTP sync; manual fields
- [ ] `void draw_analog_clock(gfx_surface_t *s, int cx, int cy, int r, struct datetime *dt)` -- hands via `gfx_draw_line()`
- [ ] `src/apps/control/applets/powercfg.c` -- screen/sleep sliders; Shutdown/Restart/Sleep buttons; battery stub
- [ ] `src/apps/control/applets/main.c` -- cursor theme scan + picker; size/speed sliders; hand-swap toggle
- [ ] `src/apps/control/applets/intl.c` -- date/time/decimal/thousands/currency fields; Registry write
- [ ] `src/apps/control/applets/taskbar.c` -- height/position/auto-hide; `WM_TASKBAR_CHANGED` broadcast
- [ ] Register all 9 in `g_cpl_entries[]` with name, description, icon_id, and category
- [ ] Commit: `"control: core applets -- sysdm/desk/ncpa/mmsys/timedate/powercfg/main/intl/taskbar.cpl"`

## 4. Additional Applets `[Sonnet]`

`nusrmgr.cpl` (User Accounts), `appwiz.cpl` (Programs), `firewall.cpl` (Firewall), `datetime.cpl` (alias for `timedate.cpl`).

**Files:** `src/apps/control/applets/` (extend)

> [!NOTE]
> **`nusrmgr.cpl` -- User Accounts**: `auth_list_users()` → `CTRL_LISTVIEW` table (Username, Display Name, Privilege); Add User button → sub-form (name/password/privilege); Remove → `auth_delete_user()` (admin-only prompt); Change Password → `auth_change_password()`; Avatar picker: scan `C:\Impossible\System\Cursors\..` → no, scan `C:\Users\{name}\AppData\Avatar.*` for image file or show solid-color circle; privilege dropdown (`PRIV_ADMIN/USER/GUEST`). All admin operations: check `auth_is_admin()`; if not: `privilege_request("Manage user accounts")`.
>
> **`appwiz.cpl` -- Programs and Features**: scan `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*` subkeys for `DisplayName`, `DisplayVersion`, `Publisher`, `UninstallString`; also scan `C:\Users\Default\AppData\Startup\*.lnk`; render `CTRL_LISTVIEW` table; "Uninstall" button → `shortcut_execute(uninstall_string)` or `file_assoc_open(path)`; "Repair" stub (if `RepairString` key exists).
>
> **`firewall.cpl` -- Firewall**: enabled/disabled `CTRL_CHECKBOX` → `firewall_enable(bool)`; rule `CTRL_LISTVIEW` (Name, Protocol, Port, Action: Allow/Block); Add Rule button → sub-form (name/protocol/port/direction/action); Remove Rule → `firewall_remove_rule(name)`. Forward dep on `06-networking/TODO-05-firewall.md`.
>
> **`datetime.cpl` alias**: `g_cpl_entries` entry with name "datetime.cpl" pointing to same `CPlApplet_t` as `timedate.cpl`; `cpl_load_and_run("datetime.cpl")` routes to same function.

- [ ] `src/apps/control/applets/nusrmgr.c` -- `auth_list_users()` table; Add/Remove/ChangePassword; avatar picker; privilege check
- [ ] `src/apps/control/applets/appwiz.c` -- scan `HKLM\...\Uninstall\*`; `CTRL_LISTVIEW`; Uninstall/Repair
- [ ] `src/apps/control/applets/firewall.c` -- enable/disable; rule list; Add/Remove Rule form; forward dep firewall API
- [ ] `datetime.cpl` alias: `g_cpl_entries` entry routing to `timedate_applet` function
- [ ] Register `nusrmgr/appwiz/firewall/datetime` in `g_cpl_entries[]`
- [ ] Commit: `"control: additional applets -- nusrmgr/appwiz/firewall/datetime.cpl"`

## 5. Settings Search `[Sonnet]`

Search bar at top of Control Panel filters all applet names + descriptions live. In-process substring match (no search index). Enter opens first result.

**Files:** `src/apps/control/control.c` (extend)

> [!NOTE]
> Search bar: `CTRL_TEXTBOX` spanning top of right panel (full width, 28 px); always visible. `control_search(query)` -- iterate `g_cpl_entries[]`; for each: `kstrcasestr(entry->name, query) || kstrcasestr(entry->desc, query)` → include in results; set `g_search_results[]` filtered array. Render: when search active (`query` non-empty): show filtered applet grid instead of category grid; sidebar categories greyed out; clear button (×) → reset. Enter key: open first result (`g_search_results[0]`). No debounce needed (in-process match is instant, < 32 entries). Highlight matched portion in applet name label using accent color (split `ttf_draw_string()`).

- [ ] `CTRL_TEXTBOX` search bar at top of right panel; × clear button
- [ ] `void control_search(const char *query)` -- iterate `g_cpl_entries`; substring match on name + desc; fill `g_search_results[]`
- [ ] Render: if query non-empty → render `g_search_results[]` grid; else → category-filtered grid
- [ ] Enter key → `control_open_applet(g_search_results[0].idx)`
- [ ] Accent highlight: split name render at match position; draw match span in `theme_get(THEME_ACCENT)`
- [ ] × button clears query; restores normal grid
- [ ] Commit: `"control: settings search -- inline name+desc filter, accent highlight, Enter opens first match"`

---

## OS Comparison


| ⭐  | Feature                                          | 🪟 Win11                                              | 🐧 Linux                                        | 🚀 Impossible OS                                                              |
| --- | ------------------------------------------------ | ----------------------------------------------------- | ----------------------------------------------- | ----------------------------------------------------------------------------- |
| 💎  | CPL framework                                    | ✅ Full Win32 CPL ABI; `.cpl`                         | ❌ No CPL equivalent; GNOME uses                | ⬜ §1 -- `⭐` Win32-identical message IDs and                                 |
| 💎  | Host app                                         | ✅ Control Panel + Settings app                       | ✅ GNOME Control Center; KDE System             | ⬜ §2 -- two-panel 900×600 layout; `.cpl` scan                                |
| 💎  | System applet                                    | ✅ System Properties (`sysdm.cpl`); full hardware     | ✅ GNOME About; `lshw`; `neofetch`; no          | ⬜ §3 `sysdm.cpl`; `cpuid_get()->brand`, `pmm_get_total_frames()`, `uptime()` |
| 💎  | Display applet                                   | ✅ Display Settings; resolution; DPI 100-500%;        | ✅ GNOME Display; KDE Display; Night            | ⬜ §3 -- `desk.cpl`; GOP mode list; DPI                                       |
| 💎  | Date/Time applet                                 | ✅ `timedate.cpl`; analog clock; auto-sync; timezone; | ✅ GNOME Date&Time; `timedatectl`; NTP via      | ⬜ §3 -- `timedate.cpl`; analog clock via `gfx_draw_line()`                   |
| 💎  | User Accounts applet                             | ✅ `nusrmgr.cpl` (legacy); Settings Accounts; change  | ✅ GNOME Users; `useradd/passwd`; privilege via | ⬜ §4 -- `nusrmgr.cpl`; `auth_*` API (TODO-06); privilege                     |
| 💎  | Programs applet -- installed app list, Uninstall | ✅ `appwiz.cpl`; full uninstaller list; size;         | ✅ GNOME Software; `apt`/`dnf`; no single       | ⬜ §4 `appwiz.cpl`; `HKLM\...\Uninstall\*` scan; `shortcut_execute`           |
| ⭐  | Settings search                                  | ✅ Settings search: full indexed search               | ✅ GNOME Control Center: search bar             | ⬜ §5 -- `⭐` zero-overhead in-process substring scan                         |

> **After §1–§5:** Impossible OS has a full Control Panel with Win32 CPL compatibility. The `⭐` differentiators: `ShellExecute("desk.cpl")` routes to the same compiled-in applet that the host app uses (no fake stub); settings search has zero latency because it's an in-process substring match over < 32 applet entries -- no indexer, no delay; and the CPL ABI is identical to Windows so future Win32 `.cpl` files can be loaded directly.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Win+I → Control Panel opens; category sidebar visible; applet grid shows named icons
- [ ] Click "System" category → only System applets shown; click `sysdm.cpl` → CPU brand string, RAM in MiB, uptime displayed
- [ ] `desk.cpl` → wallpaper picker shows thumbnail; select image → desktop wallpaper updates; Dark/Light toggle → theme changes live
- [ ] `timedate.cpl` → analog clock shows correct time updating each second; timezone dropdown populated; "Sync with NTP" button triggers NTP sync
- [ ] `ncpa.cpl` → IP/Mask/DNS fields editable; Apply → Registry updated; DHCP checkbox disables fields
- [ ] `powercfg.cpl` → screen timeout slider works; Shutdown button → shutdown prompt → `sys_shutdown()`
- [ ] `nusrmgr.cpl` → shows Admin + Guest accounts; "Add User" → create new account; "Remove" → confirm → deleted
- [ ] `appwiz.cpl` → lists apps from `HKLM\...\Uninstall\*`; Uninstall button triggers uninstall command
- [ ] Search bar: type "time" → only `timedate.cpl` + `intl.cpl` shown; Enter → `timedate.cpl` opens
- [ ] Search bar: type "xyz_nonexistent" → empty grid; type matches "desk" → "Display" applet highlighted; clear × → full grid restored
- [ ] `ShellExecute("desk.cpl")` from shell → same `desk.cpl` applet opens inside Control Panel
- [ ] Commit: `"control: Control Panel and Settings -- all CPL applets complete"`
