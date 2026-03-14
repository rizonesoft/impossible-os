# P0203 — Clock & Time System

> **Goal:** Wall-clock time tracking, formatting, timezone support, NTP sync,
> and taskbar clock enhancement.

> **Note:** CMOS RTC driver already exists (`rtc.c`). This TODO extends it with
> proper time tracking, formatting, timezone, and NTP sync.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. Kernel Time API

**Prompt:** The time system combines the CMOS RTC (wall-clock calendar time) with the PIT tick counter (monotonic uptime). On boot, `time_init()` reads the RTC, converts to Unix epoch, and records the PIT tick count. `time_now()` returns `boot_time + elapsed_ticks + ntp_offset`. `time_to_datetime` converts a Unix timestamp to a broken-down struct (year/month/day/hour/minute/second). After completing all items, create `docs/architecture/time.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: wall-clock time system"`.


- [ ] Define `time_t` (int64_t, seconds since Unix epoch)
- [ ] Define `struct datetime` (year, month, day, hour, minute, second, tz_offset_min)
- [ ] Create `include/time.h` and `src/kernel/time.c`
- [ ] Implement `time_init()` — read RTC, convert to Unix timestamp, record boot ticks
- [ ] Implement `time_now()` — return current Unix timestamp (boot_time + elapsed PIT ticks + NTP offset)
- [ ] Implement `time_now_local()` — `time_now()` + timezone offset
- [ ] Implement `time_to_datetime(ts, tz_offset)` — Unix timestamp → broken-down struct
- [ ] Implement `datetime_to_time(dt)` — broken-down → Unix timestamp
- [ ] Implement `time_set(new_time)` — called by NTP to adjust clock
- [ ] Implement `time_set_timezone(offset_minutes)` — from Registry
- [ ] Add `SYS_TIME` syscall (number 17) — return Unix timestamp
- [ ] Commit: `"kernel: wall-clock time system"`

---

## 2. Time Formatting

**Prompt:** `time_format` implements strftime-style formatting with specifiers: `%H` (24h), `%I` (12h), `%M` (min), `%S` (sec), `%p` (AM/PM), `%Y` (year), `%m` (month), `%d` (day). Read user preferences from Registry: `HKLM\SYSTEM\DateTime\Use24Hour`, `HKLM\SYSTEM\DateTime\DateFormat`. After completing all items, update `docs/architecture/time.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: time formatting"`.


- [ ] Implement `time_format(dt, buf, size, fmt)` with format specifiers:
  - [ ] `%H` — 24-hour hour (00–23)
  - [ ] `%I` — 12-hour hour (01–12)
  - [ ] `%M` — minutes (00–59)
  - [ ] `%S` — seconds (00–59)
  - [ ] `%p` — AM/PM
  - [ ] `%Y` — 4-digit year
  - [ ] `%m` — month (01–12)
  - [ ] `%d` — day (01–31)
- [ ] Read format preferences from Registry: `HKLM\SYSTEM\DateTime\Use24Hour`, `HKLM\SYSTEM\DateTime\DateFormat`
- [ ] Commit: `"kernel: time formatting"`

---

## 3. Taskbar Clock Enhancement

- [ ] Draw two lines on right side of taskbar: time (large) + date (small)
- [ ] Display in configured format (12h/24h, date format from Registry)
- [ ] Update every second (compare PIT ticks)
- [ ] *(Stretch)* Click clock → open calendar popup or Date & Time settings
- [ ] Commit: `"desktop: enhanced taskbar clock"`

---

## 4. NTP Client

**Prompt:** NTP synchronizes the system clock with internet time servers. Send SNTPv4 packet (48 bytes) to a time server via UDP port 123. Use hardcoded Google NTP IPs (216.239.35.0, .4, .8, .12) since DNS may not be available yet. Auto-sync after DHCP completes at boot. Store config in Registry: `HKLM\SYSTEM\DateTime\NTPEnabled`, `HKLM\SYSTEM\DateTime\NTPServer`. After completing all items, update `docs/architecture/time.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"net: NTP time sync client"`.


- [ ] Create `src/kernel/net/ntp.c`
- [ ] Define NTP packet struct (48 bytes, SNTPv4)
- [ ] Implement `ntp_sync()` — send request to hardcoded Google NTP IPs (216.239.35.0/4/8)
- [ ] Implement `ntp_handle_response()` — extract tx_timestamp, convert NTP→Unix epoch (-2208988800)
- [ ] Hook into UDP receive: route port 123 to `ntp_handle_response()`
- [ ] Auto-sync after DHCP completes at boot
- [ ] Periodic re-sync every 60 minutes (via scheduled task)
- [ ] Store NTP config in Registry: `HKLM\SYSTEM\DateTime\NTPEnabled`, `HKLM\SYSTEM\DateTime\NTPServer`
- [ ] Commit: `"net: NTP time sync client"`

---

## 5. Timezone

- [ ] Store timezone in Registry: `HKLM\SYSTEM\DateTime\TimezoneOffset` (minutes from UTC)
- [ ] Store timezone name: `HKLM\SYSTEM\DateTime\TimezoneName` (e.g., "SAST")
- [ ] Default: detect from locale or set UTC+0
- [ ] *(Stretch)* Timezone selector in Settings Panel
- [ ] Commit: `"kernel: timezone support"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | §1 Kernel Time API | Foundation — timestamps for logs, files, scheduler |
| 🔴 P0 | §2 Time Formatting | Display time throughout the UI |
| 🟠 P1 | §3 Taskbar Clock | Essential desktop UX |
| 🟡 P2 | §4 NTP Client | Accurate time sync |
| 🟡 P2 | §5 Timezone | Correct local time display |

---

## Key Files

| File | Purpose |
|------|---------|
| `src/kernel/time.c` | [NEW] Time tracking and conversion |
| `include/time.h` | [NEW] Time API header |
| `src/kernel/net/ntp.c` | [NEW] NTP client |
| `src/kernel/drivers/rtc.c` | [EXISTS] CMOS RTC driver |
| `docs/architecture/time.md` | [NEW] Time system documentation |
