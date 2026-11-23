---
schema_version: 1
id: calendar-utilities
domain: 11-apps
status: active
title: "TODO-13 -- Calendar, Sticky Notes & Utility Apps"
---

# TODO-13 -- Calendar, Sticky Notes & Utility Apps

> **Goal:** Deliver the remaining accessory suite: Calendar app with recurring events and
> `.ics` export, Sticky Notes, standalone `sysinfo.exe`, On-Screen Keyboard, system-wide
> Color Picker, Font Manager app-layer detail (OS/2 Unicode coverage), and the shared
> Help→About dialog used by every app.

> [!IMPORTANT]
> **Source migration:** §5 (Calendar), §6 (System Info), §7 (On-Screen Keyboard),
> §8 (Font Manager, Color Picker, Sticky Notes), §9 (Help/About) from
> `todo-old/310-Core-Apps/TODO-370-Utility-Apps.md`.
> **Delete `todo-old/310-Core-Apps/TODO-370-Utility-Apps.md` after creating this TODO** --
> all its sections are now migrated (§1 → TODO-11, §5–§8 → TODO-12, §6–§9 → here).
>
> **Scope overlaps -- cross-reference, do not re-specify:**
> - Calendar base (Zeller grid, events pane, add-event dialog, taskbar flyout) →
>   `§8`; this TODO adds **recurring events** and
>   **`.ics` export** only.
> - Font Manager app (preview, install/remove, `ttf_mgr_reload`) →
>   `§10`; this TODO adds **OS/2
>   table Unicode coverage** display only.
> - System Info as `msinfo32.cpl` (3-tab tabbed CPL) →
>   `§9`; this TODO specifies the standalone
>   **`sysinfo.exe`** app that shares the data-aggregation layer with `sysdm.cpl`.

---

## Inputs

- `include/registry.h` -- `RegGetString`/`RegSetString`, `RegDeleteKey`, `RegEnumKeyEx` (the `reg_*` calls below are shorthand for these) -- §1 events, §2 sticky persist, §5 color history
- `include/kernel/time/wall_clock.h` -- wall-clock time (no `time_now()`, `time_to_datetime()` or `time_format()` exist yet) -- §1 ICS timestamps, §2 auto-save
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_set_flag(WM_FLAG_NO_FOCUS)`, `wm_set_flag(WM_FLAG_ALWAYS_ON_TOP)`, `wm_set_fullscreen()` -- §2 §4 §5
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR_VERT`, `CTRL_STATUSBAR` -- §2 §3 §4 §6 §7
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_draw_line()`, `gfx_blit()` -- §1 grid, §5 magnifier loupe
- `include/kernel/drivers/keyboard.h` -- `keyboard_inject_scancode()` -- §4 OSK keypress injection
- `include/kernel/drivers/framebuffer.h` -- `fb_get_backbuffer()`, `fb_get_width/height()` -- §5 color picker pixel read
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set(CLIP_TEXT, ...)` -- §5 HEX copy
- `include/kernel/cpuid.h` -- `cpuid_get()` → `brand`, `vendor`, `model`, `cores`, `threads` -- §3
- `include/kernel/acpi.h` -- `acpi_get_cpu_count()`, `acpi_get_cpu_info()` -- §3
- `include/kernel/smbios.h` -- `struct smbios_system_info`, `smbios_get_info()` -- §3
- `include/kernel/mm/pmm.h` -- `pmm_get_total_frames()`, `pmm_get_free_frames()` -- §3
- `include/kernel/drivers/blkdev.h` -- `blkdev_count()`, `blkdev_list()` -- §3 disk list
- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_measure_width()` -- §6 font preview
- `include/kernel/fs/vfs.h` -- `vfs_readdir`, `vfs_open`, `vfs_read`, `vfs_write`, `vfs_stat` -- §1 ICS save, §3 export, §6 font listing
- `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §5` (→ XREF) -- `notify_send()` -- §7 install toast
- `09-desktop-shell/TODO-03-service-manager.md §8` (→ XREF) -- `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` autostart -- §9 sticky notes startup
- `08-graphics-ui/TODO-08-window-manager.md §5` (→ XREF) -- `hotkey_table[]`, `MOD_WIN|MOD_SHIFT`, `MOD_WIN|MOD_CTRL` -- §4 OSK hotkey, §5 color picker hotkey
- `§8` (→ XREF) -- `calendar_open_at_date()`, Zeller formula, add-event dialog -- §1 base
- `§10` (→ XREF) -- `fontmgr.exe`, `ttf_mgr_reload()` -- §6 base

---

## Outcome

Calendar opens from the taskbar clock flyout, persists events in Registry, supports daily/weekly/monthly recurring events, and exports `.ics` files compatible with any calendar application. Sticky Notes restore across reboots via autostart. `sysinfo.exe` shows a read-only hardware/OS summary table. The On-Screen Keyboard injects key events without stealing focus. The Color Picker (Win+Shift+C) reads any pixel from the screen and copies the HEX color. Font Manager shows TTF Unicode coverage from the OS/2 table. Every app has a shared Help→About dialog.

---

## Implementation Order

| Step | Section                                      | 💎/⭐ | Dependency                                                                             | Status |
| ---- | -------------------------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | Calendar App (Recurring Events + ICS Export) | 💎    | §11, §8                                                                                |  [ ]   |
| 2    | Sticky Notes                                 | 💎    | §12                                                                                    |  [ ]   |
| 3    | System Information (`sysinfo.exe`)           | 💎    | `cpuid_get`, `smbios_get_info`, `pmm_*`, `blkdev_count`                                |  [ ]   |
| 4    | On-Screen Keyboard                           | 💎    | §13                                                                                    |  [ ]   |
| 5    | Color Picker                                 | ⭐    | §14                                                                                    |  [ ]   |
| 6    | Font Manager (OS/2 + Unicode Coverage)       | 💎    | §15, §10                                                                               |  [ ]   |
| 7    | Shared Help / About Dialog                   | 💎    | `wm_create_window`, icon rendering                                                     |  [ ]   |
| 8    | Calendar App                                 | 💎    | §11                                                                                    |  [ ]   |
| 9    | System Info                                  | 💎    | --                                                                                     |  [ ]   |
| 10   | Font Manager App                             | 💎    | §15                                                                                    |  [ ]   |
| 11   | Import Calendar                              | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §3 |  [ ]   |
| 12   | Import Sticky Notes                          | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §3 |  [ ]   |
| 13   | Import On-Screen Keyboard                    | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §6 |  [ ]   |
| 14   | Import Color Picker                          | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §2 |  [ ]   |
| 15   | Import Fonts                                 | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §3 |  [ ]   |

---

## 1. Calendar App (Recurring Events + ICS Export)

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** calendar

Built in Calendar, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §11 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §11 brings it
- [ ] Commit: `"imports: calendar brings Calendar App (Recurring Events + ICS Export)"`

---

## 2. Sticky Notes

**Moved:** sticky-notes

Built in StickyNotes, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §12 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §12 brings it
- [ ] Commit: `"imports: sticky-notes brings Sticky Notes"`

---

## 3. System Information (`sysinfo.exe`)

> → XREF: `§9` -- `msinfo32.cpl` tabbed CPL; share the
> data-aggregation layer (`sysinfo_gather()`) between `sysinfo.exe` and `sysdm.cpl`.

**Source file:** `src/apps/sysinfo/sysinfo.c`; header `include/apps/sysinfo/sysinfo.h`

- [ ] `sysinfo.exe` already ships as a CLI (`user/sysinfo/sysinfo.c`, subcommand `firmware-updates`, run by the shell's `sysinfo`); extend that binary or rename this app, never ship two of the same name
- [ ] **Data gathering** (`sysinfo_gather(struct sys_summary *out)` -- shared with `sysdm.cpl`):
  - [ ] CPU: `cpuid_get()->brand` (model name), `cpuid_get()->vendor`, physical cores = `acpi_get_cpu_count()`, logical threads = `cpuid_get()->threads_per_core * cores`
  - [ ] RAM: `pmm_get_total_frames() * 4096 / (1024*1024)` → total MiB; `pmm_get_free_frames()` → free MiB
  - [ ] Framebuffer: `fb_get_width()` × `fb_get_height()` (GOP resolution); `fb_get_bpp()` if available
  - [ ] Disks: `blkdev_count()` + `blkdev_list()` (print to serial; for UI: iterate `g_blkdevs[]` array); format `"{name}: {size_gb} GB"`
  - [ ] OS version: `reg_get_string("HKLM\\SOFTWARE\\Impossible\\CurrentVersion", "Version")` + `"BuildNumber"`
  - [ ] Uptime: `system_get_ticks() / 1000` → seconds → format `"{d}d {h}h {m}m {s}s"`
  - [ ] Hostname: `reg_get_string("HKLM\\SYSTEM\\ComputerName\\ActiveComputerName", "ComputerName")`
  - [ ] IP: from `net_get_ip()` or `reg_get_string` cached DHCP lease
  - [ ] SMBIOS: `smbios_get_info()` → `struct smbios_system_info` → `sys_manufacturer`, `sys_product`, `sys_serial`
- [ ] **Main window** `wm_create_window("System Information", 600, 500, WM_RESIZABLE)`:
  - [ ] Two-column read-only table: bold label (left 180 px) + value (remaining); rows: OS Version, Build, Computer Name, Manufacturer, Product, CPU, Cores/Threads, RAM Total, RAM Free, Display, Disk(s), IP Address, Uptime, SMBIOS Serial
  - [ ] `CTRL_SCROLLBAR_VERT` if rows exceed window height
  - [ ] `[📋 Copy All]` button → format all rows as `"Label: Value\n"` plain text → `clipboard_set(CLIP_TEXT, ...)`
  - [ ] `File→Save As Text` → `dialog_file_save("Text|*.txt")` → write same format to file
  - [ ] `CTRL_STATUSBAR`: `"Data collected at {HH:MM:SS}"`

---

## 4. On-Screen Keyboard

**Design:** [`controls.md#button`](../../docs/design/controls.md#button), [`shell.md#materials`](../../docs/design/shell.md#materials)

**Inventory:** `win11.taskbar.tray.touch-keyboard`

**Moved:** on-screen-keyboard

Built in OnScreenKeyboard, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §13 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **Win+Ctrl+O hotkey**: `hotkey_table[N] = { MOD_WIN|MOD_CTRL, KEY_O, osk_toggle }` -- toggle show/hide
- [ ] Shown to work here on the imported copy, as §13 brings it
- [ ] Commit: `"imports: on-screen-keyboard brings On-Screen Keyboard"`

---

## 5. Color Picker

**Moved:** color-picker

Built in ColorPicker, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §14 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] **Win+Shift+C hotkey**: `hotkey_table[N] = { MOD_WIN|MOD_SHIFT, KEY_C, colorpick_start }` → enter eyedropper mode
- [ ] Shown to work here on the imported copy, as §14 brings it
- [ ] Commit: `"imports: color-picker brings Color Picker"`

---

## 6. Font Manager (OS/2 + Unicode Coverage)

**Moved:** fonts

Built in Fonts, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §15 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §15 brings it
- [ ] Commit: `"imports: fonts brings Font Manager (OS/2 + Unicode Coverage)"`

---

## 7. Shared Help / About Dialog

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog)

**Source file:** `src/apps/common/ui_dialogs.c`; header `include/desktop/ui_dialogs.h`

- [ ] **`ui_dialog_about(const char *name, const char *version, int icon_id, const char *copyright, const char *website)`**:
  - [ ] `wm_create_window("About {name}", 360, 220, WM_FIXED | WM_MODAL)` with `[OK]` button
  - [ ] Layout: 48×48 app icon (left, `icon_draw_scaled(icon_id, 48)`); bold large app name; version below; horizontal rule; copyright text (word-wrapped); website as underlined text (`gfx_draw_string` in accent color; click → no-op for now, stretch: `browser.exe {url}`)
  - [ ] `[OK]` or Escape → close
- [ ] **Every app wires `Help→About`**: `notes.exe`, `calc.exe`, `wordpad.exe`, `photos.exe`, `player.exe`, `archiver.exe`, `sniptool.exe`, `sysinfo.exe`, `osk.exe`, `colorpick.exe`, `fontmgr.exe`, `calendar.exe`, `sticky.exe` -- each passes its own name, version, icon, and copyright string
- [ ] **`include/desktop/ui_dialogs.h`** also declares:
  - [ ] `void ui_dialog_progress(const char *title, const char *message, int percent)` -- reusable modal progress dialog (used by Archive Manager §4, installer, etc.)
  - [ ] `int ui_dialog_confirm(const char *message)` → returns 1 (OK) / 0 (Cancel) -- thin wrapper around `MessageBox(MB_OKCANCEL)`

## 8. Calendar App

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-12-utilities.md` §6** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** calendar

Built in Calendar, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §11 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] `void calendar_open_at_date(int y, int m, int d)` -- public API for taskbar clock
- [ ] Shown to work here on the imported copy, as §11 brings it
- [ ] Commit: `"imports: calendar brings Calendar App"`

## 9. System Info

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-12-utilities.md` §9** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

`msinfo32.cpl` -- `CTRL_TABSTRIP` tabs: System Summary (OS/CPU/RAM/disk), Components (PCI devices), Software Environment (drivers/processes/startup). Export to text file. From Control Panel.

**Files:** `src/apps/control/applets/msinfo32.c` (new)

> [!NOTE]
> Three tabs via `CTRL_TABSTRIP`. **System Summary**: OS Name "Impossible OS 1.0", Version "1.0", Build date, CPU (`cpuid_get()->brand`), Cores (`acpi_get_cpu_count()`), MHz (stub from `cpuid_get()->max_freq` if available), Total RAM (`pmm_get_total_frames() * 4 / 1024` MiB), Available RAM (`pmm_get_free_frames() * 4 / 1024`), BIOS (UEFI firmware info from `uefi_get_variable()`), Uptime (`uptime()` seconds). **Components**: reuse `pci_get_all_devices()` from `09-desktop-shell/TODO-12-utilities.md` §8; display as flat list: PCI addr, vendor/device IDs, class code, driver name. **Software Env**: loaded drivers = scan `HKLM\SYSTEM\Drivers\*`; Running processes = `sched_get_task_list()` count + names; Startup programs = scan `HKLM\SYSTEM\Boot\Run\*` + Startup folder `.lnk` count. **Export**: "Export…" button → `dialog_file_save("System Info*.txt")` → write all three tabs as plain text sections.

- [ ] `src/apps/control/applets/msinfo32.c` -- `CPlApplet_t` registered in control panel
- [ ] `CTRL_TABSTRIP` 3 tabs; tab switch renders appropriate content
- [ ] System Summary: info key-value rows (label + value); `ttf_draw_string()` two-column layout
- [ ] Components: `pci_get_all_devices()` → flat list with class code + driver name
- [ ] Software Env: driver count from Registry + `sched_get_task_list()` count + startup scan
- [ ] "Export…" → format all data as text → `vfs_write()`
- [ ] Register in `g_cpl_entries[]` as "System Information"; accessible from `sysdm.cpl` "More info" link
- [ ] Commit: `"msinfo32: system info -- CTRL_TABSTRIP tabs, CPU/RAM/PCI/drivers/processes, export to text"`

## 10. Font Manager App

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-02-file-associations-resources.md` §8** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Moved:** fonts

Built in Fonts, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §15 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] `.ttf` file association (`09-desktop-shell/TODO-02-file-associations-resources.md` §1/`09-desktop-shell/TODO-02-file-associations-resources.md` §2) → launches `fontmgr.exe` with the TTF path as arg → preview the passed font
- [ ] Shown to work here on the imported copy, as §15 brings it
- [ ] Commit: `"imports: fonts brings Font Manager App"`

## 11. Import Calendar

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** calendar

Calendar is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull calendar` into `src/apps/calendar/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Calendar on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose calendar`
- [ ] Commit: `"apps: Calendar imported"`

## 12. Import Sticky Notes

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** sticky-notes

Sticky Notes is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull sticky-notes` into `src/apps/sticky-notes/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Sticky Notes on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose sticky-notes`
- [ ] Commit: `"apps: Sticky Notes imported"`

## 13. Import On-Screen Keyboard

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** on-screen-keyboard

On-Screen Keyboard is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull on-screen-keyboard` into `src/apps/on-screen-keyboard/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open On-Screen Keyboard on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose on-screen-keyboard`
- [ ] Commit: `"apps: On-Screen Keyboard imported"`

## 14. Import Color Picker

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** color-picker

Color Picker is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull color-picker` into `src/apps/color-picker/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Color Picker on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose color-picker`
- [ ] Commit: `"apps: Color Picker imported"`

## 15. Import Fonts

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** fonts

Fonts is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull fonts` into `src/apps/fonts/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Fonts on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose fonts`
- [ ] Commit: `"apps: Fonts imported"`

---

## OS Comparison


| ⭐  | Feature                                           | 🪟 Win11                                       | 🐧 Linux                            | 🚀 Impossible OS                                                  |
| --- | ------------------------------------------------- | ---------------------------------------------- | ----------------------------------- | ----------------------------------------------------------------- |
| 💎  | Calendar with recurring events + ICS export       | ✅ Outlook/Calendar (RRULE, ICS import/export) | ✅ GNOME Calendar / KOrganizer      | ⬜ §1 -- daily/weekly/monthly/yearly RRULE, RFC 5545 ICS          |
| 💎  | Sticky Notes with always-on-top restore           | ✅ Sticky Notes (syncs OneDrive)               | ✅ KNotes / GNOME Notes             | ⬜ §2 -- colored WM_FLAG_ALWAYS_ON_TOP windows, Registry persist, |
| 💎  | System Information read-only summary table        | ✅ `msinfo32.exe` (full detail)                | ✅ `inxi` / GNOME System Info       | ⬜ §3 -- `sysinfo_gather()` shared with `sysdm.cpl`; Copy         |
| 💎  | On-Screen Keyboard injecting hardware scancodes   | ✅ `osk.exe` (Win+Ctrl+O)                      | ✅ Onboard / GNOME OSK              | ⬜ §4 -- `keyboard_inject_scancode`, WM_FLAG_NO_FOCUS, Win+Ctrl+O |
| ⭐  | System-wide color picker with loupe + RGB/HSL/HEX | ✅ PowerToys Color Picker (not inbox)          | ✅ gpick / KColorChooser            | ⬜ §5 -- inbox Win+Shift+C, 9×9 loupe, 10-color                   |
| ⭐  | Font Manager with OS/2 Unicode coverage tag pills | ✅ Font Settings (basic list)                  | ✅ Font Manager / GNOME Fonts       | ⬜ §6 -- OS/2 table `ulUnicodeRange1–4` block-name display,       |
| 💎  | Shared Help→About dialog across all apps          | ✅ Each app has own About                      | ✅ gtk_about_dialog() shared widget | ⬜ §7 -- `ui_dialog_about()` single impl called by                |

Impossible OS ships the Color Picker as an **inbox OS feature** (Win+Shift+C hotkey baked into the global hotkey table) -- unlike Windows where it requires PowerToys installation. The loupe reads directly from the compositor's back-buffer rather than going through a screen-capture round-trip. The Font Manager's Unicode coverage display (OS/2 `ulUnicodeRange` bits decoded into named block pills) gives users richer font introspection than either platform provides by default.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Recurring events**: add a "Weekly Team" event on Monday; navigate 4 weeks forward → event appears on same weekday each week; "Edit all" changes title everywhere; "Edit this" creates exception on that date only
- [ ] **ICS export**: File→Export → `.ics` file created; open in any text editor → valid `BEGIN:VCALENDAR … BEGIN:VEVENT … RRULE:FREQ=WEEKLY … END:VEVENT … END:VCALENDAR`
- [ ] **Sticky Notes**: create 3 notes with different colors; reboot → all 3 restore at saved positions with correct colors and text; move a note → position saved; delete → confirm → note gone
- [ ] **Sysinfo**: `sysinfo.exe` → CPU brand string matches serial log; RAM total matches `pmm_get_total_frames() × 4096`; disk list shows attached drives; [Copy All] → paste in Notepad shows all rows
- [ ] **OSK**: open Notepad; press Win+Ctrl+O → OSK appears at bottom; click keys → characters appear in Notepad; CapsLock toggle → labels change to uppercase; close Notepad focus → keys still inject into last window; OSK window click does not steal focus
- [ ] **Color Picker**: Win+Shift+C → crosshair cursor + magnifier loupe tracks mouse; click on a green pixel → popup shows correct RGB/HSL/HEX; clipboard contains `"#RRGGBB"`; history strip shows the color; "Pick again" re-enters eyedropper
- [ ] **Font Manager OS/2**: select a TTF font → details panel shows weight class, Unicode coverage pills (at minimum Basic Latin for any Latin font); select a CJK font → "CJK Unified Ideographs" pill appears; copyright and license rows populated
- [ ] **About dialog**: Help→About in any app → compact window with app icon + name + version + copyright; `[OK]` closes; all 13+ apps use the same dialog
- [ ] Commit: `"apps: Calendar recurring+ICS, StickyNotes, sysinfo.exe, OSK, ColorPicker, FontMgr OS/2, shared About"`
