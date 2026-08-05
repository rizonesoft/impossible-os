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
>   `09-desktop-shell/TODO-12-utilities.md §7`; this TODO adds **recurring events** and
>   **`.ics` export** only.
> - Font Manager app (preview, install/remove, `ttf_mgr_reload`) →
>   `09-desktop-shell/TODO-02-file-associations-resources.md §8`; this TODO adds **OS/2
>   table Unicode coverage** display only.
> - System Info as `msinfo32.cpl` (3-tab tabbed CPL) →
>   `09-desktop-shell/TODO-12-utilities.md §9`; this TODO specifies the standalone
>   **`sysinfo.exe`** app that shares the data-aggregation layer with `sysdm.cpl`.

---

## Inputs

- `include/registry.h` -- `reg_get/set_string`, `reg_delete_key`, `reg_enum_subkeys` -- §1 events, §2 sticky persist, §5 color history
- `include/kernel/time.h` -- `time_now()`, `time_to_datetime()`, `time_format()` -- §1 ICS timestamps, §2 auto-save
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_set_flag(WM_FLAG_NO_FOCUS)`, `wm_set_flag(WM_FLAG_ALWAYS_ON_TOP)`, `wm_set_fullscreen()` -- §2 §4 §5
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR_VERT`, `CTRL_STATUSBAR` -- §2 §3 §4 §6 §7
- `include/gfx.h` -- `gfx_fill_rect()`, `gfx_draw_line()`, `gfx_blit()` -- §1 grid, §5 magnifier loupe
- `include/kernel/drivers/keyboard.h` -- `keyboard_inject_scancode()` -- §4 OSK keypress injection
- `include/kernel/drivers/framebuffer.h` -- `fb_get_backbuffer()`, `fb_get_width/height()` -- §5 color picker pixel read
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set(CLIP_TEXT, ...)` -- §5 HEX copy
- `include/kernel/cpuid.h` -- `cpuid_get()` → `brand`, `vendor`, `model`, `cores`, `threads` -- §3
- `include/kernel/acpi.h` -- `acpi_get_cpu_count()`, `acpi_get_cpu_info()` -- §3
- `include/kernel/smbios.h` -- `struct smbios_system_info`, `smbios_get_system_info()` -- §3
- `include/kernel/mm/pmm.h` -- `pmm_get_total_frames()`, `pmm_get_free_frames()` -- §3
- `include/kernel/drivers/blkdev.h` -- `blkdev_count()`, `blkdev_list()` -- §3 disk list
- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_measure_width()` -- §6 font preview
- `include/kernel/fs/vfs.h` -- `vfs_readdir`, `vfs_open`, `vfs_read`, `vfs_write`, `vfs_stat` -- §1 ICS save, §3 export, §6 font listing
- `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §6` (→ XREF) -- `notify_send()` -- §7 install toast
- `09-desktop-shell/TODO-03-service-manager.md §8` (→ XREF) -- `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` autostart -- §9 sticky notes startup
- `08-graphics-ui/TODO-08-window-manager.md §5` (→ XREF) -- `hotkey_table[]`, `MOD_WIN|MOD_SHIFT`, `MOD_WIN|MOD_CTRL` -- §4 OSK hotkey, §5 color picker hotkey
- `09-desktop-shell/TODO-12-utilities.md §7` (→ XREF) -- `calendar_open_at_date()`, Zeller formula, add-event dialog -- §1 base
- `09-desktop-shell/TODO-02-file-associations-resources.md §8` (→ XREF) -- `fontmgr.exe`, `ttf_mgr_reload()` -- §6 base

---

## Outcome

Calendar opens from the taskbar clock flyout, persists events in Registry, supports daily/weekly/monthly recurring events, and exports `.ics` files compatible with any calendar application. Sticky Notes restore across reboots via autostart. `sysinfo.exe` shows a read-only hardware/OS summary table. The On-Screen Keyboard injects key events without stealing focus. The Color Picker (Win+Shift+C) reads any pixel from the screen and copies the HEX color. Font Manager shows TTF Unicode coverage from the OS/2 table. Every app has a shared Help→About dialog.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                               |
| ---- | ---------------------------------------- | --- | ---------------------------------------- |
| 1    | Calendar App (Recurring Events + ICS Export) | 💎   | `09-desktop-shell/TODO-12 §7` base calendar complete |
| 2    | Sticky Notes                             | 💎   | `wm_create_window(WM_FLAG_ALWAYS_ON_TOP)`, Registry persist |
| 3    | System Information (`sysinfo.exe`)       | 💎   | `cpuid_get`, `smbios_get_system_info`, `pmm_*`, `blkdev_count` |
| 4    | On-Screen Keyboard                       | 💎   | `keyboard_inject_scancode`, `WM_FLAG_NO_FOCUS`, `hotkey_table` |
| 5    | Color Picker                             | ⭐   | `fb_get_backbuffer`, `hotkey_table`, `clipboard_set(CLIP_TEXT)` |
| 6    | Font Manager (OS/2 + Unicode Coverage)   | 💎   | `09-desktop-shell/TODO-02 §8` base fontmgr complete |
| 7    | Shared Help / About Dialog               | 💎   | `wm_create_window`, icon rendering       |

---

## 1. Calendar App (Recurring Events + ICS Export) `[Sonnet]`

> Base calendar grid, add-event dialog, and taskbar flyout are specified in
> `09-desktop-shell/TODO-12-utilities.md §7` -- implement those first.
> This section adds **recurring events** and **`.ics` export** only.

**Source file:** `src/apps/calendar/calendar_recur.c` (extension of `calendar.c`)

- [ ] **Recurring event model**:
  - [ ] Add `recurrence` field to event Registry: `HKCU\Software\Impossible\Calendar\Events\YYYY-MM-DD\{uid}\Recur` = `"none"` | `"daily"` | `"weekly"` | `"monthly"` | `"yearly"`
  - [ ] Add `recur_end` field: `YYYY-MM-DD` (empty = infinite) and `recur_count` (max occurrences, 0 = unlimited)
  - [ ] **Add Event dialog** extension: add "Repeat" dropdown (`None / Daily / Weekly / Monthly / Yearly`) and "Ends" row (Never / On date / After N occurrences)
  - [ ] **Event enumeration for month grid**: for each day in view, scan all `Events\*` subkeys; if `recur != none`, evaluate whether `view_date` is a recurrence of the base date:
    - daily: `(view_date - base_date).days % recur_interval == 0`
    - weekly: same weekday, `weeks_elapsed % 1 == 0`
    - monthly: same day-of-month
    - yearly: same month + day
  - [ ] Check `recur_end` and `recur_count` before generating occurrences
  - [ ] **"Edit series" vs "Edit occurrence"** choice dialog when editing a recurring event; "this event only" → write exception subkey `Events\YYYY-MM-DD-exc\{uid}`; "all events" → update base event
- [ ] **`.ics` export** (`File→Export → ICS`):
  - [ ] `vfs_create("C:\\Users\\{name}\\Documents\\calendar_export.ics")`
  - [ ] Write iCalendar RFC 5545 header: `BEGIN:VCALENDAR`, `VERSION:2.0`, `PRODID:-//ImpossibleOS//Calendar//EN`
  - [ ] For each event across all dates: write `BEGIN:VEVENT`, `UID:{uid}@impossible-os`, `DTSTART:YYYYMMDDTHHMMSSz`, `SUMMARY:{title}`, `DESCRIPTION:`, `END:VEVENT`
  - [ ] For recurring events: add `RRULE:FREQ=DAILY|WEEKLY|MONTHLY|YEARLY;UNTIL=YYYYMMDDTHHMMSSz` (or `COUNT=N`)
  - [ ] Write `END:VCALENDAR`; toast `"Calendar exported to Documents\calendar_export.ics"`
- [ ] **`.ics` import** (stretch): parse `VCALENDAR` → extract `VEVENT` blocks; read `DTSTART`/`SUMMARY`/`RRULE`; write to Registry event store

---

## 2. Sticky Notes `[Sonnet]`

**Source file:** `src/apps/sticky/sticky.c`; binary: `sticky.exe`

- [ ] **Note window**: `wm_create_window("Note_{id}", x, y, w, h, WM_FLAG_ALWAYS_ON_TOP | WM_FLAG_NO_TITLEBAR | WM_FLAG_RESIZABLE)`
  - [ ] Colored header strip (24 px) with `[+ New]` and `[✕]` buttons; click+drag header to move
  - [ ] Below header: full-window multiline `CTRL_TEXTBOX` on same background color
  - [ ] 6 color presets (header strip swatch row): yellow `0xFFFACD`, pink `0xFFB6C1`, blue `0xADD8E6`, green `0x90EE90`, purple `0xE6E6FA`, gray `0xD3D3D3`
  - [ ] Color selection → update header strip + textbox background; save `Color` to Registry immediately
- [ ] **Auto-save**: on every keystroke with 500 ms debounce (suppress rapid writes): `reg_set_string("HKCU\\Software\\Impossible\\StickyNotes\\{id}\\Text", text)`
- [ ] **Registry schema** per note: `HKCU\Software\Impossible\StickyNotes\{id}\{Text, Color, X, Y, W, H}` (id = monotonically incrementing counter, stored in `HKCU\..\NextId`)
- [ ] **On window move/resize**: save `X`, `Y`, `W`, `H` to Registry
- [ ] **Create new note**: `[+ New]` button → increment `NextId`; create new note window at offset (+30,+30) from last; write defaults to Registry
- [ ] **Delete note**: `[✕]` on note header → `MessageBox("Delete this note?", MB_YESNO)` → on Yes: `reg_delete_key("HKCU\\Software\\Impossible\\StickyNotes\\{id}")` → destroy window
- [ ] **Restore on startup**: enumerate `HKCU\Software\Impossible\StickyNotes\*` subkeys; create a window for each with saved `Color/X/Y/W/H/Text`; register in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\StickyNotes` = `"C:\\Impossible\\System32\\sticky.exe"`

---

## 3. System Information (`sysinfo.exe`) `[Sonnet]`

> → XREF: `09-desktop-shell/TODO-12-utilities.md §9` -- `msinfo32.cpl` tabbed CPL; share the
> data-aggregation layer (`sysinfo_gather()`) between `sysinfo.exe` and `sysdm.cpl`.

**Source file:** `src/apps/sysinfo/sysinfo.c`; header `include/apps/sysinfo/sysinfo.h`

- [ ] **Data gathering** (`sysinfo_gather(struct sys_summary *out)` -- shared with `sysdm.cpl`):
  - [ ] CPU: `cpuid_get()->brand` (model name), `cpuid_get()->vendor`, physical cores = `acpi_get_cpu_count()`, logical threads = `cpuid_get()->threads_per_core * cores`
  - [ ] RAM: `pmm_get_total_frames() * 4096 / (1024*1024)` → total MiB; `pmm_get_free_frames()` → free MiB
  - [ ] Framebuffer: `fb_get_width()` × `fb_get_height()` (GOP resolution); `fb_get_bpp()` if available
  - [ ] Disks: `blkdev_count()` + `blkdev_list()` (print to serial; for UI: iterate `g_blkdevs[]` array); format `"{name}: {size_gb} GB"`
  - [ ] OS version: `reg_get_string("HKLM\\SOFTWARE\\Impossible\\CurrentVersion", "Version")` + `"BuildNumber"`
  - [ ] Uptime: `system_get_ticks() / 1000` → seconds → format `"{d}d {h}h {m}m {s}s"`
  - [ ] Hostname: `reg_get_string("HKLM\\SYSTEM\\ComputerName\\ActiveComputerName", "ComputerName")`
  - [ ] IP: from `net_get_ip()` or `reg_get_string` cached DHCP lease
  - [ ] SMBIOS: `smbios_get_system_info()` → `struct smbios_system_info` → `manufacturer`, `product_name`, `serial_number`
- [ ] **Main window** `wm_create_window("System Information", 600, 500, WM_RESIZABLE)`:
  - [ ] Two-column read-only table: bold label (left 180 px) + value (remaining); rows: OS Version, Build, Computer Name, Manufacturer, Product, CPU, Cores/Threads, RAM Total, RAM Free, Display, Disk(s), IP Address, Uptime, SMBIOS Serial
  - [ ] `CTRL_SCROLLBAR_VERT` if rows exceed window height
  - [ ] `[📋 Copy All]` button → format all rows as `"Label: Value\n"` plain text → `clipboard_set(CLIP_TEXT, ...)`
  - [ ] `File→Save As Text` → `dialog_file_save("Text|*.txt")` → write same format to file
  - [ ] `CTRL_STATUSBAR`: `"Data collected at {HH:MM:SS}"`

---

## 4. On-Screen Keyboard `[Sonnet]`

**Source file:** `src/apps/osk/osk.c`; binary: `osk.exe`

- [ ] **Non-focusable window**: `wm_create_window("On-Screen Keyboard", x, y, 780, 200, WM_FLAG_NO_FOCUS | WM_FLAG_ALWAYS_ON_TOP | WM_FLAG_NO_TASKBAR)` -- clicking any key does not steal focus from target window
- [ ] **QWERTY button grid** (3 rows + function + number rows):
  - [ ] Row 0 (Esc + F1–F12): 14 buttons, 42 px wide × 36 px tall
  - [ ] Row 1 (`` ` ``1234567890-= Backspace): 14 buttons
  - [ ] Row 2 (Tab QWERTYUIOP[]\\): 14 buttons
  - [ ] Row 3 (CapsLock ASDFGHJKL;' Enter): 13 buttons
  - [ ] Row 4 (Shift ZXCVBNM,./ Shift): 11 buttons
  - [ ] Row 5 (Ctrl Win Alt Space Alt Ctrl ←↑↓→): 9 buttons
  - [ ] Each key button: `gfx_fill_rounded_rect` background; hover → lighter; press → darker; `ttf_draw_string` label (uppercase in Shift/CapsLock state)
- [ ] **Key injection**: button click → `keyboard_inject_scancode(scancode)` (press); button release → `keyboard_inject_scancode(scancode | 0x80)` (release with break code)
- [ ] **Modifier state**:
  - [ ] Shift: one-shot (auto-release after next character key); or sticky if double-clicked
  - [ ] CapsLock: toggle sticky; update key label case
  - [ ] Ctrl, Alt: sticky toggle; modifier scancode injected before character key and released after
- [ ] **Special keys**: Backspace (scancode `0x0E`), Enter (`0x1C`), Tab (`0x0F`), Esc (`0x01`), Space (`0x39`)
- [ ] **Win+Ctrl+O hotkey**: `hotkey_table[N] = { MOD_WIN|MOD_CTRL, KEY_O, osk_toggle }` -- toggle show/hide
- [ ] **Auto-show in touch mode** (stretch): `wm_set_touch_mode_osk_callback(osk_show)` -- called when any `CTRL_TEXTBOX` gains focus in touch mode (requires WM touch-mode flag)
- [ ] **Position persistence**: `HKCU\Software\Impossible\OSK\{X,Y}` saved on window move; restored on launch

---

## 5. Color Picker `[Sonnet]`

**Source file:** `src/apps/colorpick/colorpick.c`; binary: `colorpick.exe`

- [ ] **Win+Shift+C hotkey**: `hotkey_table[N] = { MOD_WIN|MOD_SHIFT, KEY_C, colorpick_start }` → enter eyedropper mode
- [ ] **Eyedropper mode**: `wm_set_cursor(CURSOR_CROSSHAIR)`; full-screen transparent input-capture window (`WM_FLAG_NO_DRAW | WM_FLAG_INPUT_CAPTURE`, z_order=32766)
- [ ] **Pixel magnifier loupe** (9×9 pixel grid, each pixel scaled to 12×12 px = 108×108 px box):
  - [ ] Each frame: read 9×9 block centered on cursor from `fb_get_backbuffer()`; scale up 12× into `gfx_surface_t`; show floating panel near cursor (offset so not obscured by crosshair)
  - [ ] Center pixel outlined with 2 px white border
- [ ] **On click** (mouse button down):
  - [ ] Read `uint32_t pixel = fb[cursor_y * fb_w + cursor_x]` from back-buffer
  - [ ] Extract BGRA: `b = pixel & 0xFF`, `g = (pixel>>8) & 0xFF`, `r = (pixel>>16) & 0xFF`
  - [ ] Convert to HSL: standard RGB→HSL formulae (`H = 0–360`, `S = 0–100`, `L = 0–100`)
  - [ ] Convert to HEX string: `"#RRGGBB"` snprintf
  - [ ] `clipboard_set(CLIP_TEXT, hex_string)` -- auto-copy HEX
  - [ ] Exit eyedropper mode; restore cursor
- [ ] **Result popup** (`wm_create_window("Color Picker", 240, 220, WM_FIXED)`):
  - [ ] 60×60 solid color swatch at top center
  - [ ] Three rows: `RGB: r, g, b` | `HSL: h°, s%, l%` | `HEX: #RRGGBB` with `[📋]` copy button on each row
  - [ ] History strip (last 10 colors): 10 × 20×20 swatches; click re-shows that color in result popup; stored in `HKCU\Software\Impossible\ColorPicker\History\{0..9}` as `"#RRGGBB"` strings
- [ ] **"Pick again"** button → re-enter eyedropper mode without closing popup

---

## 6. Font Manager (OS/2 + Unicode Coverage) `[Sonnet]`

> Base font manager UI (list, 4-size preview, install/remove, `ttf_mgr_reload()`) is specified
> in `09-desktop-shell/TODO-02-file-associations-resources.md §8` -- implement that first.
> This section adds **OS/2 table parsing** for Unicode coverage and detailed font metadata.

**Source file:** `src/apps/fontmgr/fontmgr_os2.c` (extension of `fontmgr.c`)

- [ ] **OS/2 table read** from TTF file:
  - [ ] TTF files are opened via `vfs_open(path)` → `vfs_read` into buffer; scan `sfntVersion` + table directory for tag `"OS/2"` at `buf + offset`
  - [ ] OS/2 table fields to extract: `fsType` (embedding license bits), `fsSelection` (bold/italic/regular flags), `ulUnicodeRange1–4` (128 bits of Unicode block coverage), `sTypoAscender`, `sTypoDescender`, `usWeightClass` (100=Thin…900=Black), `usWidthClass`
  - [ ] `name` table: scan name records for `nameID=1` (Family), `nameID=4` (Full Name), `nameID=0` (Copyright), `nameID=13` (License)
- [ ] **Font details panel** (right side of `fontmgr.exe` window, shows when a font is selected):
  - [ ] Rows: Full Name, Family, Weight Class (`{n} ({name})`), Style (Regular/Bold/Italic from `fsSelection`), Copyright, License description
  - [ ] **Unicode coverage**: iterate `ulUnicodeRange1–4` bits; for each set bit display the corresponding Unicode block name (e.g., bit 0 = "Basic Latin", bit 1 = "Latin-1 Supplement", bit 2 = "Latin Extended-A"…); show as a wrapped tag list (gray rounded pills)
  - [ ] **Embedding flags** from `fsType`: show `"Installable Embedding"` / `"Preview & Print"` / `"Editable Embedding"` / `"No embedding"` label in orange if restricted
- [ ] **`.ttf` double-click** integration: `fontmgr.exe {path}` → open app with that font pre-selected + details panel shown + large 48 pt "Aa Bb Cc 0123" preview + `[Install]` button prominent at top
- [ ] **Set default body font** (from TODO-02 §8): button `[Set as Default Body Font]` → writes to `HKCU\Software\Impossible\Fonts\BodyFont` → calls `ttf_mgr_reload()` → toast `"Restart apps to apply new font"`

---

## 7. Shared Help / About Dialog `[Sonnet]`

**Source file:** `src/apps/common/ui_dialogs.c`; header `include/desktop/ui_dialogs.h`

- [ ] **`ui_dialog_about(const char *name, const char *version, int icon_id, const char *copyright, const char *website)`**:
  - [ ] `wm_create_window("About {name}", 360, 220, WM_FIXED | WM_MODAL)` with `[OK]` button
  - [ ] Layout: 48×48 app icon (left, `icon_draw_scaled(icon_id, 48)`); bold large app name; version below; horizontal rule; copyright text (word-wrapped); website as underlined text (`gfx_draw_string` in accent color; click → no-op for now, stretch: `browser.exe {url}`)
  - [ ] `[OK]` or Escape → close
- [ ] **Every app wires `Help→About`**: `notes.exe`, `calc.exe`, `wordpad.exe`, `photos.exe`, `player.exe`, `archiver.exe`, `sniptool.exe`, `sysinfo.exe`, `osk.exe`, `colorpick.exe`, `fontmgr.exe`, `calendar.exe`, `sticky.exe` -- each passes its own name, version, icon, and copyright string
- [ ] **`include/desktop/ui_dialogs.h`** also declares:
  - [ ] `void ui_dialog_progress(const char *title, const char *message, int percent)` -- reusable modal progress dialog (used by Archive Manager §4, installer, etc.)
  - [ ] `int ui_dialog_confirm(const char *message)` → returns 1 (OK) / 0 (Cancel) -- thin wrapper around `MessageBox(MB_OKCANCEL)`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                            | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------- | ---------------------------------------- |
| 💎   | Calendar with recurring events + ICS export | ✅ Outlook/Calendar (RRULE, ICS import/export) | ✅ GNOME Calendar / KOrganizer      | ⬜ §1 -- daily/weekly/monthly/yearly RRULE, RFC 5545 ICS |
| 💎   | Sticky Notes with always-on-top restore  | ✅ Sticky Notes (syncs OneDrive)          | ✅ KNotes / GNOME Notes             | ⬜ §2 -- colored WM_FLAG_ALWAYS_ON_TOP windows, Registry persist, |
| 💎   | System Information read-only summary table | ✅ `msinfo32.exe` (full detail)           | ✅ `inxi` / GNOME System Info       | ⬜ §3 -- `sysinfo_gather()` shared with `sysdm.cpl`; Copy |
| 💎   | On-Screen Keyboard injecting hardware scancodes | ✅ `osk.exe` (Win+Ctrl+O)                 | ✅ Onboard / GNOME OSK              | ⬜ §4 -- `keyboard_inject_scancode`, WM_FLAG_NO_FOCUS, Win+Ctrl+O |
| ⭐   | System-wide color picker with loupe + RGB/HSL/HEX | ✅ PowerToys Color Picker (not inbox)     | ✅ gpick / KColorChooser            | ⬜ §5 -- inbox Win+Shift+C, 9×9 loupe, 10-color |
| ⭐   | Font Manager with OS/2 Unicode coverage tag pills | ✅ Font Settings (basic list)             | ✅ Font Manager / GNOME Fonts       | ⬜ §6 -- OS/2 table `ulUnicodeRange1–4` block-name display, |
| 💎   | Shared Help→About dialog across all apps | ✅ Each app has own About                 | ✅ gtk_about_dialog() shared widget | ⬜ §7 -- `ui_dialog_about()` single impl called by |

Impossible OS ships the Color Picker as an **inbox OS feature** (Win+Shift+C hotkey baked into
the global hotkey table) -- unlike Windows where it requires PowerToys installation. The loupe
reads directly from the compositor's back-buffer rather than going through a screen-capture
round-trip. The Font Manager's Unicode coverage display (OS/2 `ulUnicodeRange` bits decoded
into named block pills) gives users richer font introspection than either platform provides by
default.

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
