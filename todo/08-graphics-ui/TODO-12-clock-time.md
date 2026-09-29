---
schema_version: 1
id: clock-time
domain: 08-graphics-ui
status: active
title: "TODO-12 -- Kernel Time & Taskbar Clock"
---

# TODO-12 -- Kernel Time & Taskbar Clock

> **Goal:** Build the full kernel wall-clock API on top of the existing CMOS RTC driver -- `time_now()`, timezone handling, `time_to_datetime()`, NTP hook, `strftime`-style formatting, a minimal embedded timezone table with DST rules, the taskbar clock with a calendar flyout, and a Date/Time Control Panel applet.

> [!IMPORTANT]
> **Already exists**: `struct rtc_time { second, minute, hour, day, month, year, weekday }` + `rtc_read(struct rtc_time *t)` + `rtc_init()` in `include/kernel/drivers/rtc.h`. `system_get_ticks()` + `PIT_TARGET_FREQ=100` Hz in `include/kernel/drivers/pit.h` / `include/kernel/timer.h`. `uptime()` (returns seconds) already declared in `timer.h`. `SYS_UPTIME=12` in `syscall.h`. `EFI_TIME.timezone` (int16_t minutes-from-UTC) in `uefi_runtime.h` for initial timezone seed. **Conflict**: prompt cites `SYS_TIME=17`, but `SYS_LOG=17` is already assigned; use the next free legacy number for `SYS_TIME` (drafted as 55; legacy `SYS_*` numbers end at 48 today). **Missing**: `time_now()`, `time_to_datetime()`, `time_set()`, timezone system, `time_format()`, `uptime_ms()`, taskbar clock flyout + calendar, Control Panel applet, timezone table with DST. FILETIME epoch (100 ns ticks since 1601) and HPET/TSC calibration belong to `02-kernel-core/TODO-08-time-filetime-management.md` -- this TODO handles the civil calendar layer only.

> [!WARNING]
> **Written before the kernel clock shipped (corrected 2026-09-29).** A FILETIME wall clock already exists: `wall_clock_init()` seeds from UEFI GetTime then `rtc_try_read()`, `KeQuerySystemTime()` / `KeSetSystemTime()` read and set it, NTP disciplines it through `ke_ntp_adjtime()`, and `timezone_set()` / `filetime_to_local()` apply a bias (`include/kernel/time/wall_clock.h`, `include/kernel/time/timezone.h`). `uptime_ns()` exists in `timer.h`. A basic taskbar clock is already drawn in `src/desktop/desktop.c` (12 h time over `YYYY/MM/DD`, from `KeQuerySystemTime()`). §1 must wrap that clock, not add a second one; see its first item. Complete sections in order: kernel time API → monotonic uptime extension → formatting → timezone database → taskbar clock → Control Panel applet.

## Inputs

- `include/kernel/drivers/rtc.h` -- `struct rtc_time`, `rtc_read()`, `rtc_init()` -- §1 reads initial wall-clock from CMOS
- `include/kernel/drivers/pit.h` -- `PIT_TARGET_FREQ=100`, `system_get_ticks()` -- §1 `time_now()` adds elapsed PIT ticks to boot timestamp; §6 `uptime_ms()` uses the same counter
- `include/kernel/timer.h` -- `uptime()` (seconds, uint64) -- §6 extends this with `uptime_ms()`
- `include/kernel/sched/syscall.h` -- `SYS_UPTIME=12` exists; `SYS_TIME=55` added in §1
- `include/registry.h` -- `RegGetValue/SetValueEx` -- §2 reads `Use24Hour` + `DateFormat`; §4 persists timezone + NTP flag; §5 timezone selection
- `include/kernel/uefi_runtime.h` -- `EFI_TIME.timezone` (int16 minutes from UTC) -- §1 `time_init()` seeds initial timezone offset
- `include/desktop/systray.h` (`08-graphics-ui/TODO-11` §4, not built) -- system tray draw area -- §4 taskbar clock draws inside the tray right zone
- `include/kernel/gfx/anim_mgr.h` (`08-graphics-ui/TODO-04`, not built) -- `anim_mgr_add()` -- §3 flyout fade + 12 px rise (`THEME_MOTION_NORMAL_MS`)
- `include/gfx.h` -- `gfx_fill_rounded_rect()`, `gfx_acrylic()` -- §3 flyout panel, §4 Control Panel clock face
- `include/desktop/wm.h` -- `wm_create_window()`, `z_order` -- §3 the shared notifications-and-calendar flyout
- → XREF: `07-networking/TODO-06-ntp-status-winsock.md` -- NTP sync calls `time_set(new_unix)` from this TODO; §1 `time_set()` must be the single authority for wall-clock updates
- → XREF: `02-kernel-core/TODO-08-time-filetime-management.md` -- the shipped FILETIME wall clock, NTP discipline and high-resolution monotonic clock live there; this TODO only handles the civil calendar (seconds resolution)
- → XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4` -- system tray provides the right-edge draw area where the clock lives (§3 draws inside it)

## Outcome

- `time_now()` returns Unix timestamp (seconds since 1970) derived from RTC + PIT elapsed ticks + NTP offset.
- `time_now_local()` adds timezone offset; `time_to_datetime()` converts to broken-down `struct datetime`.
- `time_format(dt, buf, fmt)` with `%H/%I/%M/%S/%p/%Y/%m/%d/%A/%a/%B/%b` and Registry 24h/date-format control.
- Embedded 30+ entry timezone table with DST rules; `time_set_timezone(offset_min)`.
- Taskbar clock draws the time over the date beside the bell; click → the shared 360 px notifications-and-calendar flyout (`docs/design/shell.md#notifications-and-calendar`).
- Date & time settings page (`timedate.cpl`, alias `datetime.cpl`): settings rows for time, date, timezone and NTP sync; no analog clock face.
- `uptime_ms()` (uint64) + `uptime` shell command. `SYS_TIME=55` syscall.

## Implementation Order

| ⭐  | Order | Deliverable                                                                              | Depends On                                                             | Status |
| --- | :---: | ---------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Kernel time API -- `time_init/now/now_local/to_datetime/datetime_to_time/set/set_tz` | `rtc_read()`, `system_get_ticks()` (both exist); `SYS_TIME=55`        |  [ ]   |
| 💎  |   6   | §6 Monotonic uptime -- `uptime_ms()` uint64, `uptime` shell command                     | §1 (`time_init()` must run at boot before `uptime_ms()` callers)      |  [ ]   |
| 💎  |   2   | §2 Time formatting -- `time_format()` with strftime specifiers, Registry 24h/date-fmt   | §1 `struct datetime` must exist                                        |  [ ]   |
| 💎  |   5   | §5 Timezone database -- embedded `tz_entry[]` table, DST rules, `tz_find_by_name()`    | §2 formatting (DST applies before formatting local time)               |  [ ]   |
| 💎  |   3   | §3 Taskbar clock -- time over date, 1 s update, calendar in the shared 360 px flyout | §2 formatting (clock uses `time_format()`); §5 tz table; TODO-11 tray |  [ ]   |
| 💎  |   4   | §4 Date/time settings page -- time, date, tz dropdown, NTP sync rows (no clock face)   | §3 clock flyout (calendar widget reused in the page); §5 tz table      |  [ ]   |

---

## 1. Kernel Time API `[Sonnet]`

`time_init()`: read CMOS RTC, convert to Unix timestamp, record `g_boot_unix` + `g_boot_ticks`. `time_now()`: `g_boot_unix + (system_get_ticks() - g_boot_ticks) / PIT_TARGET_FREQ + g_ntp_offset_secs`. `time_now_local()`: `time_now() + g_tz_offset_sec`. `time_to_datetime()` and `datetime_to_time()` with full leap-year arithmetic. `time_set()` for NTP. `SYS_TIME=55`.

**Files:** `src/kernel/time.c` (new), `include/kernel/time.h` (new)

> [!NOTE]
> **Superseded by the warning in the preamble for the clock source; the calendar arithmetic below still applies to `time_to_datetime()`.** Unix epoch conversion from `struct rtc_time`: use a portable days-since-epoch calculation: `days = days_from_epoch(year, month, day)` where `days_from_epoch()` counts days from 1970-01-01 using a standard Gregorian algorithm (no floats). `g_boot_unix = days * 86400 + hour*3600 + minute*60 + second`. `time_now()` adds `(system_get_ticks() - g_boot_ticks) / PIT_TARGET_FREQ` -- integer division; resolution = 1 s (fine for civil time). `time_to_datetime(ts, tz_offset_min)`: add `tz_offset_min * 60` to ts; compute year (subtract 365/366-day epochs); month (subtract month lengths accounting for leap year); day, hour, minute, second. Weekday: Tomohiko Sakamoto's algorithm or Zeller's congruence (integer only). **Leap year**: `(y%4==0 && y%100!=0) || y%400==0`. `g_ntp_offset_secs` is `int32_t` (signed -- NTP can correct backward); set atomically by `time_set()` which is called from `07-networking/TODO-06`'s NTP client. **SYS_TIME conflict**: prompt cites number 17 but `SYS_LOG=17` is taken; use `SYS_TIME=55`.

- [ ] `typedef int64_t time_t;` in `include/kernel/time.h`
- [ ] `struct datetime { uint16_t year; uint8_t month, day, hour, minute, second, weekday; int16_t tz_offset_min; };`
- [ ] No private clock state in `time.c`: every read goes through `KeQuerySystemTime()` and `timezone_get()`, so `wall_clock.c` (NTP, `NtSetSystemTime()`) stays the only writer
- [ ] `void time_init(void)`: load the saved zone from `HKLM\SYSTEM\DateTime\TimezoneOffsetMin` into `timezone_set()` (fallback: `EFI_TIME.timezone`); no clock seeding, `wall_clock_init()` already did it
- [ ] `int64_t time_now(void)`: `return filetime_to_unix_seconds(KeQuerySystemTime());`
- [ ] `int64_t time_now_local(void)`: `return filetime_to_unix_seconds(filetime_to_local(KeQuerySystemTime()));`
- [ ] `void time_to_datetime(int64_t ts, int16_t tz_offset_min, struct datetime *out)` -- full Gregorian decomposition with leap-year arithmetic; weekday via Zeller's or Tomohiko algorithm
- [ ] `int64_t datetime_to_time(const struct datetime *dt)` -- inverse: days-from-epoch × 86400 + time-of-day; subtract `tz_offset_min * 60`
- [ ] `void time_set(int64_t new_unix)`: convert to FILETIME and call `KeSetSystemTime()` for privileged callers (settings page); NTP keeps using `ke_ntp_adjtime()`, not this
- [ ] `void time_set_timezone(int16_t offset_min)`: `timezone_set()` with the new bias; `RegSetValueEx("HKLM\\SYSTEM\\DateTime\\TimezoneOffsetMin", offset_min)`
- [ ] `#define SYS_TIME 55` in `syscall.h`; `sys_time()` handler returns `time_now()`; wire into dispatch table
- [ ] `time_init()` called after `wall_clock_init()` and `timezone_init()` in `src/kernel/main/boot_storage.c`
- [ ] Commit: `"time: kernel time API -- time_init/now/local/to_datetime/set/set_tz, SYS_TIME=55"`

## 2. Time Formatting `[Sonnet]`

`time_format(dt, buf, size, fmt)` with strftime-style specifiers: `%H` (00–23), `%I` (01–12), `%M`, `%S`, `%p` (AM/PM), `%Y`, `%m`, `%d`, `%A` (full weekday), `%a` (abbr), `%B` (full month), `%b` (abbr). Registry `HKLM\SYSTEM\DateTime\Use24Hour` + `DateFormat` ("MM/DD/YYYY" etc.).

**Files:** `src/kernel/time.c` (extend), `include/kernel/time.h` (extend)

> [!NOTE]
> `time_format()` scans `fmt` char-by-char; on `%`: consume next char; look up in a switch; emit formatted segment into `buf` with `snprintf`-style length tracking (no libc; use `ksnprintf` or hand-coded decimal formatter since `-nostdinc`). Weekday names: `static const char *g_weekday_abbr[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}` and full equivalents (0-indexed, Sunday=0). Month names: same pattern. `%p`: if hour < 12 → "AM"; else "PM". `%I`: `h = hour % 12; if (h==0) h=12;`. **Registry-driven defaults**: `time_format_clock(dt, buf, size)` reads `Use24Hour` and emits `%H:%M` or `%I:%M %p`. `time_format_date(dt, buf, size)` reads `DateFormat` string from Registry and maps it to a `time_format()` call.

- [ ] `int time_format(const struct datetime *dt, char *buf, uint32_t size, const char *fmt)` -- returns bytes written; no libc; uses hand-coded decimal/string helpers
- [ ] `static const char *g_weekday_full[7]` + `g_weekday_abbr[7]` + `g_month_full[12]` + `g_month_abbr[12]` -- static string tables in `time.c`
- [ ] Specifiers: `%H`, `%I`, `%M`, `%S`, `%p`, `%Y`, `%y` (2-digit year), `%m`, `%d`, `%A`, `%a`, `%B`, `%b` -- 13 specifiers
- [ ] `void time_format_clock(const struct datetime *dt, char *buf, uint32_t size)`: read `HKLM\SYSTEM\DateTime\Use24Hour`; call `time_format(dt, buf, size, "%H:%M")` or `"%I:%M %p"`
- [ ] `void time_format_date(const struct datetime *dt, char *buf, uint32_t size)`: read `HKLM\SYSTEM\DateTime\DateFormat` ("MM/DD/YYYY" | "DD/MM/YYYY" | "YYYY-MM-DD"); map to `time_format()` call
- [ ] Commit: `"time: time_format() -- 13 strftime specifiers, Registry 24h/date-format, no libc dependency"`

## 3. Taskbar Clock `[Sonnet]`

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar), [`shell.md#notifications-and-calendar`](../../docs/design/shell.md#notifications-and-calendar)

Render the time over the date in the caption style (12/16), right-aligned in a `THEME_SIZE_TRAY_CLOCK_WIDTH` (76) wide, 40 px tall button beside the notification bell (`docs/design/shell.md#taskbar`). Update every 1 s (PIT tick counter). Click clock → the shared 360 px notifications-and-calendar flyout (`docs/design/shell.md#notifications-and-calendar`): the notification list of `08-graphics-ui/TODO-11 §6` above a month calendar (6 rows x 7 columns of 40 px cells), today as an accent circle.

**Files:** `src/desktop/clock_tray.c` (new), `include/desktop/clock_tray.h` (new)

> [!NOTE]
> 1 s check, not 1 s repaint: once a second, outside the paint path (not inside `systray_draw()`), format `time_now_local()` into a scratch buffer and call `wm_mark_dirty()` only when it differs from `g_clock_str`. The clock shows minutes, so it still repaints once a minute; every `wm_mark_dirty()` is a full composite today (`wm_composite()` redraws the wallpaper and every window). **Calendar**: drawn in the lower part of the shared 360 px notifications-and-calendar flyout (`docs/design/shell.md#notifications-and-calendar`), below the notification list of `08-graphics-ui/TODO-11 §6`; there is no separate clock popup and no large-time header. Layout: a month title row ("September 2026" in body strong) with previous/next chevron buttons on the right; 7-column day-of-week headers in the caption style, `text_secondary`; 6 rows x 7 columns of 40 px day cells. Today: `gfx_fill_circle(s, cx, cy, 20, theme_get()->colors.accent)` with the number in `text_on_accent`; other-month days in `text_tertiary`. Close on outside click. Month navigation: local `g_cal_year/month` state, not tied to system clock.

- [ ] Replace the clock in `desktop_draw_taskbar()` (not a new one); its `rtc_read()` fallback formats a zero date without CMOS, so check `rtc_try_read()` / `rtc_time_plausible()` first
- [ ] `void clock_tray_draw(gfx_surface_t *s, int32_t x, int32_t y, int32_t w)`: call `time_format_clock()` every `PIT_TARGET_FREQ` ticks; draw `g_clock_str` right-aligned in tray zone; date string below if taskbar height permits
- [ ] `void clock_flyout_open(void)`: open the shared 360 px flyout (flyout acrylic, radius 8, `THEME_ELEV_FLYOUT_*`) anchored 12 px from the right edge and 12 px above the taskbar; draw the notification list then the calendar
- [ ] `void clock_flyout_close(void)`: `wm_destroy_window(flyout_wh)`; called on outside click
- [ ] Calendar grid draw: `clock_draw_calendar(s, year, month)`: day-of-week header row; compute first weekday of month (`datetime_to_time()` then `time_to_datetime()`); fill 6×7 grid; today = accent circle; pad with prev/next month days in muted color
- [ ] Previous/next chevron click: `g_cal_month--/++` with year rollover; `clock_draw_calendar()` re-renders
- [ ] Clock click in `systray_mouse_handler()`: if hit-test in clock area → `clock_flyout_open()`
- [ ] Clock and calendar to `docs/design/shell.md#taskbar` and `docs/design/shell.md#notifications-and-calendar`: time over date in the caption style, right-aligned, beside the notification bell
  - Calendar: month title with previous/next buttons, 7-column grid of 40 px day cells, today as an accent circle, other-month days in `text_tertiary`, below the notification list of `08-graphics-ui/TODO-11 §6`
- [ ] Commit: `"clock: taskbar clock -- time over date, 1 s update, calendar in the shared notifications flyout"`

## 4. Date/Time Control Panel `[Sonnet]`

**Design:** [`shell.md#settings-and-control-panel-frame`](../../docs/design/shell.md#settings-and-control-panel-frame), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

The Date & time page of the Settings / Control Panel host (`09-desktop-shell/TODO-11`), reached as `timedate.cpl` (and its `datetime.cpl` alias): settings rows per `docs/design/controls.md#cards-and-settings-rows`, no standalone window and no analog clock face, as in Windows 11.

**Files:** `src/desktop/datetime_cpl.c` (new), `include/desktop/datetime_cpl.h` (new)

> [!NOTE]
> **Superseded by the lead above (no standalone window, no analog face); kept only for the calendar, timezone and Set details.** Open from Control Panel or right-click clock flyout "Adjust date/time". Window: 400×480 px. **Analog clock**: 120 px diameter circle; hour hand length 35 px, minute 50 px, second 60 px; angles computed from `%H/%M/%S` via integer trigonometry (`sin/cos` approximation using a 64-entry lookup table in `kmath.h` if it exists, else integer line drawing via Bresenham). Redraw every 1 s. **Digital**: `time_format()` "%H:%M:%S" below the dial. **Date picker**: reuse `clock_draw_calendar()` from §3 as a shared helper; user can click a day to select it. **Timezone dropdown**: `CTRL_DROPDOWN` populated from `tz_table[]` (§5); current selection from `g_tz_offset_min`; on select → `time_set_timezone()`. **"Set" button**: read time-field text inputs + selected date from calendar; call `datetime_to_time()` → `time_set()`; persist `HKLM\SYSTEM\DateTime\*` fields. **"Sync NTP Now" button**: call `ntp_sync_now()` stub (real implementation in `07-networking/TODO-06`); show "Syncing…" / "Synced" status label.

- [ ] `void datetime_page_build(settings_page_t *pg)`: registers as the `timedate.cpl` page with the host (`09-desktop-shell/TODO-11 §3`); page title "Date & time"
- [ ] Rows: "Set time automatically" (toggle, NTP), "Time zone" (combo box from `tz_get_all()` of §5), "Adjust for daylight saving time automatically" (toggle), "Use 24-hour clock" (toggle, `HKLM\SYSTEM\DateTime\Use24Hour`)
- [ ] "Set the date and time manually": "Change" button, disabled while automatic time is on, opens a dialog (`controls.md#dialog`) with a date picker (`controls.md#date-picker`) and hour/minute text boxes; OK → `datetime_to_time()` → `time_set()`
- [ ] "Sync now" row: button + caption "Last successful sync: HH:MM" (`text_secondary`); `ntp_sync()` (`07-networking/TODO-06`); shows a progress ring while syncing
- [ ] Commit: `"datetime: Date & time settings page -- automatic time, time zone, DST, 24-hour, manual change dialog, sync now"`

## 5. Timezone Database `[Sonnet]`

Embedded minimal timezone table: UTC, UTC±1 through ±14, plus named entries (US/Eastern, US/Central, US/Pacific, Europe/London, Europe/Paris, Asia/Tokyo, Australia/Sydney, etc.). `struct tz_entry { name, offset_min, dst_offset_min, dst_start_rule, dst_end_rule }`. DST adjustment computed from rules on `time_now_local()`.

**Files:** `src/kernel/tz_table.c` (new), `include/kernel/tz_table.h` (new)

> [!NOTE]
> DST rules: `struct tz_dst_rule { uint8_t month; uint8_t week; uint8_t weekday; uint8_t hour; }` -- "Nth weekday of month M at HH:00" (covers US, EU, AU rules). Example: US Eastern DST start = 2nd Sunday of March at 02:00 (`{ 3, 2, 0, 2 }`); end = 1st Sunday of November at 02:00 (`{ 11, 1, 0, 2 }`). `tz_is_dst(ts, rule_start, rule_end)`: compute the exact Unix timestamp for that year's DST start/end boundary; return 1 if `ts` is between them. `time_now_local()`: `offset_sec = g_tz_entry->offset_min * 60`; if DST enabled and `tz_is_dst(time_now(), ...)`: `offset_sec += g_tz_entry->dst_offset_min * 60`; return `time_now() + offset_sec`. Table size: ~30 entries at ~40 bytes each = ~1.2 KB static data.

- [ ] `typedef struct { char name[32]; int16_t offset_min; int16_t dst_offset_min; struct tz_dst_rule dst_start; struct tz_dst_rule dst_end; } tz_entry_t;` in `tz_table.h`
- [ ] `static const tz_entry_t tz_table[]` in `tz_table.c` -- ~30 entries: UTC, UTC-12 through UTC+14, US/Eastern, US/Central, US/Mountain, US/Pacific, Europe/London, Europe/Paris, Europe/Berlin, Asia/Tokyo, Asia/Shanghai, Asia/Kolkata, Australia/Sydney, Pacific/Auckland
- [ ] `const tz_entry_t *tz_find_by_name(const char *name)` -- linear search `tz_table`; NULL if not found
- [ ] `const tz_entry_t *tz_find_by_offset(int16_t offset_min)` -- find first match; fallback for unnamed offsets
- [ ] `int tz_is_dst(int64_t ts, const tz_entry_t *tz)` -- compute DST boundary timestamps for current year; return 1 if in DST window; return 0 if `dst_offset_min == 0` (no DST)
- [ ] `void tz_get_all(const tz_entry_t **out, int *count)` -- expose full table to `CTRL_DROPDOWN` in §4
- [ ] Update `time_now_local()` to check `tz_is_dst()` when `g_tz_entry->dst_offset_min != 0`
- [ ] Commit: `"tz_table: embedded 30-entry timezone database with DST rules (Nth-weekday-of-month)"`

## 6. Monotonic Uptime Extension `[Sonnet]`

`uptime_ms()` returns uint64 milliseconds since boot (wraps at ~584M years, not 49 days). `SYS_UPTIME=12` already dispatches to seconds; extend `sys_uptime()` to return ms. `uptime` shell command: "Xd Xh Xm Xs". Used by: ping RTT, disk latency histogram.

**Files:** `src/kernel/timer.c` (extend), `include/kernel/timer.h` (extend)

> [!NOTE]
> `uptime_ms()`: `return (system_get_ticks() - g_boot_ticks) * 1000 / PIT_TARGET_FREQ`. With `PIT_TARGET_FREQ=100` and a uint64 counter this is safe for ~5.8 × 10¹⁵ ms. Existing `uptime()` returns seconds -- keep it; add `uptime_ms()` beside it. `sys_uptime()` (SYS_UPTIME=12) currently returns seconds; add an optional arg: if called with arg `1` → return ms (ABI-compatible: existing callers pass no arg / 0 and get seconds). Shell `uptime` command: `elapsed_s = uptime()`; `d = elapsed_s/86400; h = (elapsed_s%86400)/3600; m = (elapsed_s%3600)/60; s = elapsed_s%60`; `kprintf("Up %ud %uh %um %us\n", d, h, m, s)`.

- [ ] `uint64_t uptime_ms(void)` in `timer.c` / `timer.h`: `(system_get_ticks() * 1000) / PIT_TARGET_FREQ`
- [ ] Verify `system_get_ticks()` returns `uint64_t` (not uint32 -- avoids 49-day wrap); fix return type in `pit.h` / `timer.h` if it's currently uint32_t
- [ ] `sys_uptime()` extended: argument 0 or absent → seconds; argument 1 → ms; update dispatch handler
- [ ] `uptime` shell command in `src/shell/cmds.c`: parse `uptime()`; format "Xd Xh Xm Xs"; `kprintf()`
- [ ] Callers: confirm `net_ping_rtt` and disk latency histogram use `uptime_ms()` where ms precision is needed
- [ ] Commit: `"timer: uptime_ms() uint64, sys_uptime ms arg, uptime shell command"`

---

## OS Comparison


| ⭐  | Feature                 | 🪟 Win11                                                               | 🐧 Linux                                                                    | 🚀 Impossible OS                                                            |
| --- | ----------------------- | ---------------------------------------------------------------------- | --------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| 💎  | Kernel time API         | ✅ `GetSystemTime`, `SystemTimeToTzSpecificLocalTime`, `SetSystemTime` | ✅ `clock_gettime(CLOCK_REALTIME)`, `mktime`, `localtime_r`, `settimeofday` | ⬜ §1 -- `time_now()` = boot CMOS +                                         |
| 💎  | Time formatting         | ✅ `strftime`, `GetTimeFormat`, locale-aware                           | ✅ `strftime()` + locale; `date` utility                                    | ⬜ §2 -- 13 specifiers; hand-coded (no `-nostdinc`                          |
| 💎  | Taskbar clock           | ✅ System tray clock; click →                                          | ✅ GNOME clock indicator; KDE clock                                         | ⬜ §3 -- shared 360 px flyout with calendar                                 |
| 💎  | Date/Time Control Panel | ✅ Settings → Time & Language                                          | ✅ GNOME Settings date-time; `timedatectl`; KDE                             | ⬜ §4 -- settings rows (no clock face); was: analog clock face with integer |
| 💎  | Timezone database       | ✅ Bundled `tzdata` in Windows; DST                                    | ✅ IANA `tzdata` package; `zoneinfo` files;                                 | ⬜ §5 -- 30-entry embedded table; `tz_is_dst()` Nth-weekday-of-month        |
| 💎  | Monotonic uptime        | ✅ `GetTickCount64()` (ms); `QueryPerformanceCounter()` (ns)           | ✅ `clock_gettime(CLOCK_MONOTONIC)`; `uptime` command; `/proc/uptime`       | ⬜ §6 -- `uptime_ms()` uint64; `uptime` shell cmd                           |

> **After §1–§6:** Impossible OS has a complete civil-time stack. No libc, no `tzdata` package -- the timezone table and `time_format()` are hand-coded directly in the kernel, making the total footprint under 2 KB of static data. The 30-entry timezone table covers all major world regions with proper DST rules.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log on boot shows `[time] boot UTC: 2026-MM-DD HH:MM:SS` from `time_init()`
- [ ] `time_now()` at boot ≈ CMOS RTC value; after 10 s, `time_now()` advanced by 10 (PIT-driven)
- [ ] `time_format(&dt, buf, sizeof(buf), "%A, %B %d %Y %I:%M %p")` → e.g. `"Thursday, March 26 2026 10:30 AM"`
- [ ] `time_set_timezone(-300)` (US Eastern UTC-5) → `time_now_local()` = `time_now() - 18000`
- [ ] Taskbar clock shows `HH:MM`; updates every second; click → the shared 360 px flyout opens above the taskbar with notifications and the calendar; today is an accent circle; the chevrons navigate months
- [ ] Date & time page: time and date rows update every second; timezone dropdown populated with all 30 entries; select "US/Pacific" → clock shows PDT/PST offset; "Sync NTP Now" → serial log `[ntp] sync requested`
- [ ] DST test: set timezone "US/Eastern" + set date to second Sunday of March → `tz_is_dst()` returns 1 at 02:00 → clock shows UTC-4 instead of UTC-5
- [ ] `uptime` shell command → "Xd Xh Xm Xs" format; matches `SYS_UPTIME` syscall value
- [ ] `uptime_ms()` > 0 after boot; `system_get_ticks()` is uint64 (no overflow warning from compiler)
- [ ] Commit: `"time: full time stack -- kernel API, format, tz table, taskbar clock, Control Panel, uptime_ms"`
