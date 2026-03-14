# P1701 — System Services

> **Goal:** Build the system-level infrastructure that transforms a bare kernel into a
> usable operating system: configuration management (Registry), background
> services, clipboard, file associations, time management, and essential utilities.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. Registry System

> **See [TODO-P0102-Registry.md](TODO-P0102-Registry.md)** — Full Registry implementation spec
> (Codex→Registry migration, Win32 API, data structures, hive files, persistence,
> change notifications, syscalls, Win32 stubs, regedit CLI).

---

## 2. Background Services / Daemons

### 2.1 Service Manager

**Prompt:** The service manager provides start/stop/restart lifecycle for background daemons. Each service is tracked by a `struct service` with state + PID. `svc_start` forks and execs the service binary (or creates a kernel thread for kernel-level services). `svc_stop` sends SIGTERM and waits, then SIGKILL if necessary. Service definitions are stored in the Registry under `HKLM\SYSTEM\Services\{name}\`. Start with kernel threads since the process model may not be fully mature. After completing all items, create `docs/architecture/services.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: service manager"`.


- [ ] Define `svc_state_t` enum: STOPPED, RUNNING, STARTING
- [ ] Define `struct service` (name, exe_path, state, pid, auto_start)
- [ ] Create `include/service.h` and `src/kernel/service.c`
- [ ] Implement `svc_start(name)` — fork + exec the service executable
- [ ] Implement `svc_stop(name)` — send SIGTERM → kill process
- [ ] Implement `svc_restart(name)` — stop + start
- [ ] Implement `svc_status(name)` — return current state
- [ ] Implement `svc_list(out, max)` — enumerate all services
- [ ] Commit: `"kernel: service manager"`

### 2.2 Built-In Services

**Prompt:** Register core OS services that start automatically at boot. `netd` manages the network stack (DHCP renewal, ARP cache), `ntpd` runs NTP time sync, `registryd` handles periodic Registry dirty-flag flushes to disk, `indexd` rebuilds the file search index. Each service's config lives in the Registry. Services start after kernel init is complete and the filesystem is mounted. Dependency ordering matters: netd must start before ntpd. After completing all items, update `docs/architecture/services.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: built-in services"`.


- [ ] Register `netd` — network stack (DHCP, ARP) — auto-start
- [ ] Register `ntpd` — NTP time sync — auto-start after network
- [ ] Register `registryd` — Registry dirty-flag flush — auto-start
- [ ] Register `indexd` — file search indexer — auto-start
- [ ] Store service config in Registry: `HKLM\SYSTEM\Services\{name}\AutoStart`, `HKLM\SYSTEM\Services\{name}\ExePath`
- [ ] Auto-start services at boot (after kernel init)
- [ ] Commit: `"kernel: built-in services"`

### 2.3 Service Shell Command

**Prompt:** The `sc` shell command provides service management from the terminal: `sc list` shows all services with their current state, `sc start netd` starts a service, `sc stop netd` stops it, `sc status netd` shows detailed info. Model this after Windows' `sc` command. After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: sc service control command"`.


- [ ] `sc list` — show all services with state
- [ ] `sc start <name>` / `sc stop <name>` / `sc restart <name>`
- [ ] `sc status <name>` — show detailed service info
- [ ] Commit: `"shell: sc service control command"`

---

## 3. Clipboard

### 3.1 System Clipboard

**Prompt:** The clipboard is a single kernel-resident buffer shared between all processes. `clipboard_set(fmt, data, size)` copies data into the buffer with a format tag (TEXT, IMAGE, FILES). `clipboard_get(fmt, buf, max)` retrieves it. Data must be deep-copied on set. Syscalls SYS_CLIPBOARD_SET/GET let user-mode apps access it. Keep it simple: one clipboard entry at a time. After completing all items, create `docs/architecture/clipboard.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: system clipboard"`.


- [ ] Define `clip_format_t` enum: TEXT, IMAGE, FILES
- [ ] Define `clipboard_t` struct (format, data pointer, size)
- [ ] Create `include/clipboard.h` and `src/kernel/clipboard.c`
- [ ] Implement `clipboard_set(fmt, data, size)` — copy data to clipboard buffer
- [ ] Implement `clipboard_get(fmt, buf, max_size)` — read clipboard data
- [ ] Implement `clipboard_has(fmt)` — check if format available
- [ ] Implement `clipboard_clear()` — clear all clipboard data
- [ ] Add `SYS_CLIPBOARD_SET` and `SYS_CLIPBOARD_GET` syscalls
- [ ] Commit: `"kernel: system clipboard"`

### 3.2 Keyboard Shortcuts

**Prompt:** Ctrl+C/X/V must be wired through the keyboard event pipeline to reach the focused control. The flow: keyboard driver → WM key event → check for global shortcuts → dispatch to focused window → focused control handles Ctrl+C by reading its selection and calling `clipboard_set`. Ctrl+X does copy + delete selection. Ctrl+V calls `clipboard_get` and inserts at cursor. Must not conflict with SIGINT in the terminal — SIGINT should only fire when no text is selected. After completing all items, update `docs/architecture/clipboard.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: clipboard keyboard shortcuts"`.


- [ ] Ctrl+C in focused control → copy selection to clipboard
- [ ] Ctrl+X in focused control → cut selection to clipboard
- [ ] Ctrl+V in focused control → paste from clipboard
- [ ] Wire shortcuts through WM → focused window → focused control
- [ ] Commit: `"desktop: clipboard keyboard shortcuts"`

### 3.3 Win32 Clipboard Mapping

- [ ] `OpenClipboard()` / `CloseClipboard()` → no-op
- [ ] `SetClipboardData(CF_TEXT, data)` → `clipboard_set(CLIP_TEXT, ...)`
- [ ] `GetClipboardData(CF_TEXT)` → `clipboard_get(CLIP_TEXT, ...)`
- [ ] `EmptyClipboard()` → `clipboard_clear()`
- [ ] Add to `user32.dll` builtin stub table
- [ ] Commit: `"win32: clipboard API stubs"`

### 3.4 Clipboard History

**Prompt:** Clipboard history keeps the last 25 entries in a ring buffer — each entry is a deep copy plus metadata (format, timestamp, source app name). Win+V opens a popup listing recent entries. Clicking an entry sets it as current and pastes. Configurable via Registry: `HKLM\SYSTEM\Clipboard\HistoryEnabled`, `HKLM\SYSTEM\Clipboard\MaxItems`. After completing all items, update `docs/architecture/clipboard.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: clipboard history (Win+V)"`.


- [ ] Create `src/desktop/clip_history.c`
- [ ] Maintain ring buffer of last 25 clipboard entries
- [ ] Each entry: format, data copy, timestamp, source app name
- [ ] Win+V keyboard shortcut → show clipboard history popup
- [ ] Click an entry → paste it (set as current clipboard, send paste to focused control)
- [ ] "Clear all" button → empty history
- [ ] Registry: `HKLM\SYSTEM\Clipboard\HistoryEnabled`, `HKLM\SYSTEM\Clipboard\MaxItems`
- [ ] Commit: `"desktop: clipboard history (Win+V)"`

---

## 4. Clock & Time System

> **Note:** CMOS RTC driver already exists (`rtc.c`). This section extends it with
> proper time tracking, formatting, timezone, and NTP sync.

### 4.1 Kernel Time API

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

### 4.2 Time Formatting

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

### 4.3 Taskbar Clock Enhancement

- [ ] Draw two lines on right side of taskbar: time (large) + date (small)
- [ ] Display in configured format (12h/24h, date format from Registry)
- [ ] Update every second (compare PIT ticks)
- [ ] *(Stretch)* Click clock → open calendar popup or Date & Time settings
- [ ] Commit: `"desktop: enhanced taskbar clock"`

### 4.4 NTP Client

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

### 4.5 Timezone

- [ ] Store timezone in Registry: `HKLM\SYSTEM\DateTime\TimezoneOffset` (minutes from UTC)
- [ ] Store timezone name: `HKLM\SYSTEM\DateTime\TimezoneName` (e.g., "SAST")
- [ ] Default: detect from locale or set UTC+0
- [ ] *(Stretch)* Timezone selector in Settings Panel
- [ ] Commit: `"kernel: timezone support"`

---

## 5. File Associations

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §1** — File type icon mapping, extension-to-app mapping, default associations, "Open With" dialog.

---

## 6. Search & File Indexing

### 6.1 Search Index

**Prompt:** The search index provides fast filename lookup without scanning the entire filesystem on each query. Walk the VFS tree recursively, recording each file/folder name and full path in a sorted index. Rebuild in a background thread at boot and periodically (every 30 minutes). Keep the index in memory, flush to disk for persistence. After completing all items, create `docs/architecture/search.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: file search indexer"`.


- [ ] Define `struct search_result` (path, match, type [FILE/FOLDER/APP/SETTING], modified)
- [ ] Create `src/kernel/search.c`
- [ ] Implement `search_index_rebuild()` — walk VFS tree, index file/folder names
- [ ] Store index in `C:\Impossible\System\Cache\search.idx` (flat file: path + name)
- [ ] Run index rebuild in background thread on boot + periodically (30 min)
- [ ] Commit: `"kernel: file search indexer"`

### 6.2 Search Query

- [ ] Implement `search_query(query, results, max)` — substring match on index
- [ ] Search sources:
  - [ ] File names (from index)
  - [ ] App names (scan `C:\Impossible\Bin\` + `C:\Programs\`)
  - [ ] *(Stretch)* File contents (scan text files — slow path)
  - [ ] *(Stretch)* Settings panel names
- [ ] Add `SYS_SEARCH` syscall
- [ ] Commit: `"kernel: search query API"`

### 6.3 Integration

- [ ] *(Stretch)* Start menu search bar → type to search apps + files
- [ ] *(Stretch)* File manager search bar → filter current directory
- [ ] *(Stretch)* Shell `find <query>` command
- [ ] Commit: `"desktop: search integration"`

---

## 7. Shortcut Files (.lnk)

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §2** — Shortcut format, API, icon integration, desktop & Start Menu rendering.

---

## 8. Scheduled Tasks

### 8.1 Task Scheduler

**Prompt:** The task scheduler runs registered commands at specified intervals. Each `struct sched_task` has a name, command string, interval in seconds, next_run timestamp, and enabled flag. `sched_task_tick()` is called by the PIT timer handler every second — it checks each task's next_run against current time and executes due tasks. After completing all items, create `docs/architecture/scheduler.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: task scheduler"`.


- [ ] Define `struct sched_task` (name, command, arguments, interval_seconds, next_run, enabled)
- [ ] Create `src/kernel/scheduler_tasks.c`
- [ ] Implement `sched_task_add(task)` — register a scheduled task
- [ ] Implement `sched_task_remove(name)` — unregister
- [ ] Implement `sched_task_tick()` — called by timer, check if any tasks are due
- [ ] Hook `sched_task_tick()` into PIT timer (check every second)
- [ ] Commit: `"kernel: task scheduler"`

### 8.2 Built-In Scheduled Tasks

- [ ] NTP sync — every 60 minutes
- [ ] Registry flush — every 2 seconds (dirty-flag)
- [ ] Search index rebuild — every 30 minutes
- [ ] Log rotate — every 24 hours (trim old entries)
- [ ] Store tasks in Registry: `HKLM\SYSTEM\Scheduler\Tasks\{name}\*`
- [ ] Commit: `"kernel: built-in scheduled tasks"`

---

## 9. Recycle Bin

### 9.1 Recycle Bin Core

**Prompt:** The recycle bin makes file deletion recoverable. `trash_delete(path)` moves the file to `C:\Recycle\` and saves metadata. `trash_restore(trash_name)` moves it back. `trash_empty()` permanently deletes everything. User-initiated deletions go to the recycle bin; only explicit "Delete permanently" uses VFS permanent delete. After completing all items, create `docs/architecture/recycle-bin.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: recycle bin"`.


- [ ] Create `src/kernel/trash.c`
- [ ] Implement `trash_delete(path)` — move file to `C:\Recycle\`, save metadata
- [ ] Implement `trash_restore(trash_name)` — move back to original path (from metadata)
- [ ] Implement `trash_empty()` — permanently delete all files in recycle
- [ ] Implement `trash_count()` — number of items
- [ ] Implement `trash_size()` — total bytes used
- [ ] Commit: `"kernel: recycle bin"`

### 9.2 Metadata Files

- [ ] Create `.meta` file for each trashed item in `C:\Recycle\_meta\`
- [ ] Store: OriginalPath, DeletedAt (Unix timestamp), Size
- [ ] INI format for easy parsing
- [ ] Commit: `"kernel: recycle bin metadata"`

### 9.3 Desktop Integration

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §3** — Recycle Bin desktop icon states (`ICON_TRASH_EMPTY`/`ICON_TRASH_FULL`), context menu, auto-purge.

---

## 10. ZIP Compression

### 10.1 miniz Integration

- [ ] Port **miniz** (MIT, single file, ~4000 lines) into `src/libs/miniz/`
- [ ] Redirect memory: `mz_malloc → kmalloc`, `mz_free → kfree`
- [ ] Commit: `"libs: miniz compression library"`

### 10.2 ZIP API

**Prompt:** The ZIP API wraps miniz into a simpler kernel API. `zip_create(path, files, count)` creates a new ZIP archive. `zip_extract(path, dest)` extracts all files to a destination. `zip_list(path, names, max)` lists contents. `zip_add_file(path, file)` appends a file. After completing all items, create `docs/architecture/zip.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ZIP archive support"`.


- [ ] Create `include/zip.h` and `src/kernel/zip.c` (wrapper around miniz)
- [ ] Implement `zip_create(zip_path, files[], count)` — create ZIP from files
- [ ] Implement `zip_extract(zip_path, dest_dir)` — extract all files
- [ ] Implement `zip_list(zip_path, names, max)` — list archive contents
- [ ] Implement `zip_add_file(zip_path, file_path)` — add file to existing ZIP
- [ ] Commit: `"kernel: ZIP archive support"`

### 10.3 Shell & UI Integration

- [ ] Shell commands: `zip archive.zip file1 file2`, `unzip archive.zip`
- [ ] *(Stretch)* Right-click → "Compress to ZIP"
- [ ] *(Stretch)* Right-click `.zip` → "Extract here" / "Extract to..."
- [ ] Commit: `"shell: zip/unzip commands"`

---

## 11. Additional System Services

### 11.1 User Account System

> **See [TODO-P2301-Security-Accounts.md](TODO-P2301-Security-Accounts.md)** — Full multi-user
> security with login, permissions, UAC, and encryption.

A minimal version is needed here for file permissions and HOME environment variable:

- [ ] Define `struct user_account` (username, uid, home_dir, password_hash)
- [ ] Create default user "Default" on first boot
- [ ] Per-user Registry trees (`HKU\{username}\`)
- [ ] Per-user home directories (`C:\Users\{username}\`)
- [ ] Current user tracked in kernel (for file permissions, environment)
- [ ] Commit: `"kernel: user account system"`

### 11.2 Win32 System Info APIs

- [ ] `GetSystemInfo()` — CPU count, page size, architecture
- [ ] `GetVersionExA/W()` — OS version from Registry/VERSION file
- [ ] `GetComputerNameA()` — from Registry `HKLM\SYSTEM\Network\Hostname`
- [ ] `GetUserNameA()` — from current user
- [ ] `GetTempPathA()` — return `C:\Temp\`
- [ ] `GetSystemDirectoryA()` — return `C:\Impossible\System\`
- [ ] `GetWindowsDirectoryA()` — return `C:\Impossible\`
- [ ] Add to `kernel32.dll` builtin stub table
- [ ] Commit: `"win32: system info API stubs"`

### 11.3 Notification Service

- [ ] Kernel-side notification queue (source, message, severity, timestamp)
- [ ] `notify_send(source, message, severity)` — kernel/driver API
- [ ] Desktop notification popup reads from this queue
- [ ] Used by: NTP sync success, disk mount, network changes, errors
- [ ] Commit: `"kernel: notification service"`

### 11.4 Autostart / Startup Programs

- [ ] Scan `C:\Users\{name}\AppData\Startup\` for `.lnk` files at boot
- [ ] Execute each shortcut after desktop is fully initialized
- [ ] Registry: `HKLM\SYSTEM\Boot\RunOnce` — list of commands to run once then remove
- [ ] Registry: `HKLM\SYSTEM\Boot\Run` — list of commands to run every boot
- [ ] Commit: `"kernel: startup program execution"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | §1 Registry System | Foundation — all services store config here |
| 🔴 P0 | §4.1–4.2 Time System | Wall-clock time for timestamps, logs, scheduler |
| 🔴 P0 | §8 Scheduled Tasks | Powers Registry flush, NTP sync, log rotate |
| 🟠 P1 | §2 Services/Daemons | Background processes infrastructure |
| 🟠 P1 | §3.1–3.2 Clipboard | Essential UX (copy/paste) |
| 🟠 P1 | §5 File Associations | Double-click opens correct app |
| 🟠 P1 | §7 Shortcut Files | Desktop/Start Menu proper UX |
| 🟡 P2 | §4.4 NTP Client | Accurate time |
| 🟡 P2 | §9 Recycle Bin | Safe deletion |
| 🟡 P2 | §10 ZIP Compression | Archive support |
| 🟡 P2 | §11.4 Autostart | Startup programs |
| 🟢 P3 | §3.3–3.4 Clipboard History + Win32 | Polish features |
| 🟢 P3 | §6 Search & Indexing | File discovery |
| 🟢 P3 | §11.1 User Accounts | Multi-user (see P2301) |
| 🔵 P4 | §11.2 Win32 System Info | Compatibility APIs |
| 🔵 P4 | §11.3 Notification Service | Desktop integration |
