# Phase 03 — System Services

> **Goal:** Build the system-level infrastructure that transforms a bare kernel into a
> usable operating system: configuration management (Codex registry), background
> services, clipboard, file associations, time management, and essential utilities.

---

## 1. Codex Registry System
> *Research: [01_codex_registry.md](research/phase_03_system_services/01_codex_registry.md)*

### 1.1 Core In-Memory Tree

- [ ] Define `codex_type_t` enum: STRING, INT32, INT64, BINARY, BOOL
- [ ] Define `codex_value_t` struct (name, type, data union, data_size)
- [ ] Define `codex_key_t` struct (name, parent, children list, sibling list, values list)
- [ ] Create `include/codex.h` and `src/kernel/codex.c`
- [ ] Implement `codex_init()` — initialize empty tree with root keys
- [ ] Implement `codex_open(path)` — navigate tree by backslash-separated path
- [ ] Implement `codex_create(path)` — create key if it doesn't exist
- [ ] Implement `codex_delete_key(path)` — remove key and all children
- [ ] Commit: `"kernel: Codex registry tree structure"`

### 1.2 Value Accessors

- [ ] Implement `codex_get_string(key, name, buf, buf_size)`
- [ ] Implement `codex_get_int32(key, name, out)`
- [ ] Implement `codex_get_int64(key, name, out)`
- [ ] Implement `codex_get_bool(key, name, out)`
- [ ] Implement `codex_set_string(key, name, value)`
- [ ] Implement `codex_set_int32(key, name, value)`
- [ ] Implement `codex_set_int64(key, name, value)`
- [ ] Implement `codex_set_bool(key, name, value)`
- [ ] Implement `codex_delete_value(key, name)`
- [ ] Implement `codex_enum_keys(key, index, name, size)` — iterate subkeys
- [ ] Implement `codex_enum_values(key, index, out)` — iterate values
- [ ] Commit: `"kernel: Codex get/set/enum operations"`

### 1.3 Pre-Populated Defaults

- [ ] Populate `System\Display\` — Width, Height, DPI from framebuffer
- [ ] Populate `System\Theme\` — AccentColor (#0078D4), DarkMode (1), Font (Selawik), CornerRadius (8)
- [ ] Populate `System\Shell\` — TaskbarHeight (48), TaskbarPosition (bottom)
- [ ] Populate `System\Network\` — Hostname, DHCP, DNS
- [ ] Populate `Hardware\CPU\` — Vendor, Model (from CPUID)
- [ ] Populate `Hardware\Memory\` — TotalMB (from PMM)
- [ ] Populate `User\Default\` — Desktop, Shell defaults
- [ ] Call `codex_populate_defaults()` from `kernel_main()` on first boot
- [ ] Commit: `"kernel: Codex default values"`

### 1.4 Disk Persistence (.codex files)

- [ ] Define `.codex` file format (INI-style: [Section] + Key = Value)
- [ ] Implement `.codex` file parser (read sections, key-value pairs, handle quoting)
- [ ] Implement `codex_save(tree)` — serialize to `C:\Impossible\System\Config\Codex\`
  - [ ] `system.codex` for `System\`
  - [ ] `hardware.codex` for `Hardware\`
  - [ ] `user.codex` for `User\`
  - [ ] `apps.codex` for `Apps\`
- [ ] Implement `codex_load(tree)` — load from disk at boot
- [ ] Dirty-flag tracking — only save changed trees
- [ ] Periodic flush (every 2 seconds) via scheduled task or timer
- [ ] Force flush on shutdown
- [ ] Commit: `"kernel: Codex disk persistence"`

### 1.5 Win32 Registry Mapping

- [ ] Map Windows hives to Codex paths:
  - [ ] `HKEY_LOCAL_MACHINE\SOFTWARE` → `System\`
  - [ ] `HKEY_LOCAL_MACHINE\HARDWARE` → `Hardware\`
  - [ ] `HKEY_CURRENT_USER` → `User\Default\`
  - [ ] `HKEY_CURRENT_USER\Software\{App}` → `Apps\{App}\`
  - [ ] `HKEY_CLASSES_ROOT` → `System\FileTypes\`
- [ ] Implement `RegOpenKeyExA/W` → `codex_open(mapped_path)`
- [ ] Implement `RegCreateKeyExA/W` → `codex_create(mapped_path)`
- [ ] Implement `RegQueryValueExA/W` → `codex_get_*()` with type mapping (REG_SZ→STRING, REG_DWORD→INT32)
- [ ] Implement `RegSetValueExA/W` → `codex_set_*()`
- [ ] Implement `RegDeleteKeyA/W`, `RegDeleteValueA/W`
- [ ] Implement `RegEnumKeyExA/W`, `RegEnumValueA/W`
- [ ] `RegCloseKey` → no-op
- [ ] Add to `advapi32.dll` builtin stub table
- [ ] Commit: `"win32: registry API stubs mapping to Codex"`

### 1.6 Codex Shell Command

- [ ] `codex list System\Theme` — show all values in a key
- [ ] `codex get System\Theme\DarkMode` — show a single value
- [ ] `codex set System\Theme\DarkMode 0` — set a value
- [ ] `codex tree System` — print key tree
- [ ] Add to shell built-in commands
- [ ] Commit: `"shell: codex command"`

---

## 2. Background Services / Daemons
> *Research: [02_services_daemons.md](research/phase_03_system_services/02_services_daemons.md)*

### 2.1 Service Manager

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

- [ ] Register `netd` — network stack (DHCP, ARP) — auto-start
- [ ] Register `ntpd` — NTP time sync — auto-start after network
- [ ] Register `codexd` — Codex dirty-flag flush — auto-start
- [ ] Register `indexd` — file search indexer — auto-start
- [ ] Store service config in Codex: `System\Services\{name}\AutoStart`, `System\Services\{name}\ExePath`
- [ ] Auto-start services at boot (after kernel init)
- [ ] Commit: `"kernel: built-in services"`

### 2.3 Service Shell Command

- [ ] `sc list` — show all services with state
- [ ] `sc start <name>` / `sc stop <name>` / `sc restart <name>`
- [ ] `sc status <name>` — show detailed service info
- [ ] Commit: `"shell: sc service control command"`

---

## 3. Clipboard
> *Research: [03_clipboard.md](research/phase_03_system_services/03_clipboard.md), [08_clipboard_history.md](research/phase_03_system_services/08_clipboard_history.md)*

### 3.1 System Clipboard

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

- [ ] Create `src/desktop/clip_history.c`
- [ ] Maintain ring buffer of last 25 clipboard entries
- [ ] Each entry: format, data copy, timestamp, source app name
- [ ] Win+V keyboard shortcut → show clipboard history popup
- [ ] Click an entry → paste it (set as current clipboard, send paste to focused control)
- [ ] "Clear all" button → empty history
- [ ] Codex: `System\Clipboard\HistoryEnabled`, `System\Clipboard\MaxItems`
- [ ] Commit: `"desktop: clipboard history (Win+V)"`

---

## 4. Clock & Time System
> *Research: [05_clock_time_sync.md](research/phase_03_system_services/05_clock_time_sync.md)*

> **Note:** CMOS RTC driver already exists (`rtc.c`). This section extends it with
> proper time tracking, formatting, timezone, and NTP sync.

### 4.1 Kernel Time API

- [ ] Define `time_t` (int64_t, seconds since Unix epoch)
- [ ] Define `struct datetime` (year, month, day, hour, minute, second, tz_offset_min)
- [ ] Create `include/time.h` and `src/kernel/time.c`
- [ ] Implement `time_init()` — read RTC, convert to Unix timestamp, record boot ticks
- [ ] Implement `time_now()` — return current Unix timestamp (boot_time + elapsed PIT ticks + NTP offset)
- [ ] Implement `time_now_local()` — `time_now()` + timezone offset
- [ ] Implement `time_to_datetime(ts, tz_offset)` — Unix timestamp → broken-down struct
- [ ] Implement `datetime_to_time(dt)` — broken-down → Unix timestamp
- [ ] Implement `time_set(new_time)` — called by NTP to adjust clock
- [ ] Implement `time_set_timezone(offset_minutes)` — from Codex
- [ ] Add `SYS_TIME` syscall (number 17) — return Unix timestamp
- [ ] Commit: `"kernel: wall-clock time system"`

### 4.2 Time Formatting

- [ ] Implement `time_format(dt, buf, size, fmt)` with format specifiers:
  - [ ] `%H` — 24-hour hour (00–23)
  - [ ] `%I` — 12-hour hour (01–12)
  - [ ] `%M` — minutes (00–59)
  - [ ] `%S` — seconds (00–59)
  - [ ] `%p` — AM/PM
  - [ ] `%Y` — 4-digit year
  - [ ] `%m` — month (01–12)
  - [ ] `%d` — day (01–31)
- [ ] Read format preferences from Codex: `System\DateTime\Use24Hour`, `System\DateTime\DateFormat`
- [ ] Commit: `"kernel: time formatting"`

### 4.3 Taskbar Clock Enhancement

- [ ] Draw two lines on right side of taskbar: time (large) + date (small)
- [ ] Display in configured format (12h/24h, date format from Codex)
- [ ] Update every second (compare PIT ticks)
- [ ] *(Stretch)* Click clock → open calendar popup or Date & Time settings
- [ ] Commit: `"desktop: enhanced taskbar clock"`

### 4.4 NTP Client

- [ ] Create `src/kernel/net/ntp.c`
- [ ] Define NTP packet struct (48 bytes, SNTPv4)
- [ ] Implement `ntp_sync()` — send request to hardcoded Google NTP IPs (216.239.35.0/4/8)
- [ ] Implement `ntp_handle_response()` — extract tx_timestamp, convert NTP→Unix epoch (-2208988800)
- [ ] Hook into UDP receive: route port 123 to `ntp_handle_response()`
- [ ] Auto-sync after DHCP completes at boot
- [ ] Periodic re-sync every 60 minutes (via scheduled task)
- [ ] Store NTP config in Codex: `System\DateTime\NTPEnabled`, `System\DateTime\NTPServer`
- [ ] Commit: `"net: NTP time sync client"`

### 4.5 Timezone

- [ ] Store timezone in Codex: `System\DateTime\TimezoneOffset` (minutes from UTC)
- [ ] Store timezone name: `System\DateTime\TimezoneName` (e.g., "SAST")
- [ ] Default: detect from locale or set UTC+0
- [ ] *(Stretch)* Timezone selector in Settings Panel
- [ ] Commit: `"kernel: timezone support"`

---

## 5. File Associations
> *Research: [04_file_associations.md](research/phase_03_system_services/04_file_associations.md)*

### 5.1 Extension-to-App Mapping

- [ ] Create `include/file_assoc.h` and `src/kernel/file_assoc.c`
- [ ] Implement `file_assoc_get_app(ext)` — look up Codex `System\FileTypes\.{ext}\App`
- [ ] Implement `file_assoc_get_icon(ext)` — look up icon name for extension
- [ ] Implement `file_assoc_set(ext, app_path)` — set/change default app
- [ ] Implement `file_assoc_open(filepath)` — extract extension, find app, exec with filepath as argument
- [ ] Commit: `"kernel: file associations"`

### 5.2 Default Associations

- [ ] Register defaults in Codex on first boot:
  - [ ] `.txt`, `.md`, `.log` → `notepad.exe`, icon `file_text`
  - [ ] `.c`, `.h`, `.py`, `.js` → `notepad.exe`, icon `file_code`
  - [ ] `.jpg`, `.png`, `.bmp` → `imgview.exe`, icon `file_image`
  - [ ] `.exe` → (self), icon `file_exe`
  - [ ] `.zip` → (archive handler), icon `file_archive`
- [ ] Commit: `"kernel: default file associations"`

### 5.3 "Open With..." Dialog (Future)

- [ ] *(Stretch)* Show list of installed apps for any file type
- [ ] *(Stretch)* "Always use this app" checkbox → updates Codex
- [ ] *(Stretch)* Right-click context menu entry

---

## 6. Search & File Indexing
> *Research: [06_search_indexing.md](research/phase_03_system_services/06_search_indexing.md)*

### 6.1 Search Index

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
> *Research: [07_shortcut_files.md](research/phase_03_system_services/07_shortcut_files.md)*

### 7.1 Shortcut Format & API

- [ ] Define `struct shortcut` (target, arguments, icon_path, working_dir, description)
- [ ] Create `include/shortcut.h` and `src/kernel/shortcut.c`
- [ ] Define `.lnk` file format (INI-style: [Shortcut] section with Target, Arguments, Icon, WorkingDir, Description)
- [ ] Implement `shortcut_create(lnk_path, shortcut)` — write .lnk file
- [ ] Implement `shortcut_read(lnk_path, shortcut)` — parse .lnk file
- [ ] Implement `shortcut_execute(lnk_path)` — read target + exec with arguments
- [ ] Commit: `"kernel: shortcut file (.lnk) support"`

### 7.2 Desktop & Start Menu Integration

- [ ] Desktop renders `.lnk` files with their custom icon + description as label
- [ ] Double-click `.lnk` on desktop → `shortcut_execute()`
- [ ] Start Menu reads `.lnk` files from `C:\Users\{name}\AppData\StartMenu\`
- [ ] Create default shortcuts on first boot (Terminal, Notepad, Settings)
- [ ] Commit: `"desktop: shortcut integration"`

---

## 8. Scheduled Tasks
> *Research: [09_scheduled_tasks.md](research/phase_03_system_services/09_scheduled_tasks.md)*

### 8.1 Task Scheduler

- [ ] Define `struct sched_task` (name, command, arguments, interval_seconds, next_run, enabled)
- [ ] Create `src/kernel/scheduler_tasks.c`
- [ ] Implement `sched_task_add(task)` — register a scheduled task
- [ ] Implement `sched_task_remove(name)` — unregister
- [ ] Implement `sched_task_tick()` — called by timer, check if any tasks are due
- [ ] Hook `sched_task_tick()` into PIT timer (check every second)
- [ ] Commit: `"kernel: task scheduler"`

### 8.2 Built-In Scheduled Tasks

- [ ] NTP sync — every 60 minutes
- [ ] Codex flush — every 2 seconds (dirty-flag)
- [ ] Search index rebuild — every 30 minutes
- [ ] Log rotate — every 24 hours (trim old entries)
- [ ] Store tasks in Codex: `System\Scheduler\Tasks\{name}\*`
- [ ] Commit: `"kernel: built-in scheduled tasks"`

---

## 9. Recycle Bin
> *Research: [10_recycle_bin.md](research/phase_03_system_services/10_recycle_bin.md)*

### 9.1 Recycle Bin Core

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

- [ ] Desktop icon: `ICON_TRASH_EMPTY` when bin is empty, `ICON_TRASH_FULL` when items present
- [ ] Right-click trash icon → "Open Recycle Bin", "Empty Recycle Bin"
- [ ] File manager "Delete" action → `trash_delete()` instead of permanent delete
- [ ] Codex: `System\Recycle\MaxSize` — auto-purge oldest when limit reached (default 1 GB)
- [ ] Commit: `"desktop: recycle bin integration"`

---

## 10. ZIP Compression
> *Research: [11_zip_compression.md](research/phase_03_system_services/11_zip_compression.md)*

### 10.1 miniz Integration

- [ ] Port **miniz** (MIT, single file, ~4000 lines) into `src/libs/miniz/`
- [ ] Redirect memory: `mz_malloc → kmalloc`, `mz_free → kfree`
- [ ] Commit: `"libs: miniz compression library"`

### 10.2 ZIP API

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

## 11. Agent-Recommended Additions

> Items not in the research files but important for system services.

### 11.1 User Account System

- [ ] Define `struct user_account` (username, uid, home_dir, password_hash)
- [ ] Create default user "Default" on first boot
- [ ] Per-user Codex trees (`User\{username}\`)
- [ ] Per-user home directories (`C:\Users\{username}\`)
- [ ] Current user tracked in kernel (for file permissions, environment)
- [ ] *(Stretch)* Login screen at boot
- [ ] *(Stretch)* Password verification (hash with monocypher Argon2)
- [ ] Commit: `"kernel: user account system"`

### 11.2 Win32 System Info APIs

- [ ] `GetSystemInfo()` — CPU count, page size, architecture
- [ ] `GetVersionExA/W()` — OS version from Codex/VERSION file
- [ ] `GetComputerNameA()` — from Codex `System\Network\Hostname`
- [ ] `GetUserNameA()` — from current user
- [ ] `GetTempPathA()` — return `C:\Temp\`
- [ ] `GetSystemDirectoryA()` — return `C:\Impossible\System\`
- [ ] `GetWindowsDirectoryA()` — return `C:\Impossible\`
- [ ] Add to `kernel32.dll` builtin stub table
- [ ] Commit: `"win32: system info API stubs"`

### 11.3 Notification Service

- [ ] Kernel-side notification queue (source, message, severity, timestamp)
- [ ] `notify_send(source, message, severity)` — kernel/driver API
- [ ] Desktop notification popup from Phase 02 reads from this queue
- [ ] Used by: NTP sync success, disk mount, network changes, errors
- [ ] Commit: `"kernel: notification service"`

### 11.4 Autostart / Startup Programs

- [ ] Scan `C:\Users\{name}\AppData\Startup\` for `.lnk` files at boot
- [ ] Execute each shortcut after desktop is fully initialized
- [ ] Codex: `System\Boot\RunOnce` — list of commands to run once then remove
- [ ] Codex: `System\Boot\Run` — list of commands to run every boot
- [ ] Commit: `"kernel: startup program execution"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. Codex Registry | Foundation — all other services store config here |
| 🔴 P0 | 4.1–4.2 Time System | Wall-clock time for timestamps, logs, scheduler |
| 🔴 P0 | 8. Scheduled Tasks | Powers Codex flush, NTP sync, log rotate |
| 🟠 P1 | 2. Services/Daemons | Background processes infrastructure |
| 🟠 P1 | 3.1–3.2 Clipboard | Essential UX (copy/paste) |
| 🟠 P1 | 5. File Associations | Double-click opens correct app |
| 🟠 P1 | 7. Shortcut Files | Desktop/Start Menu proper UX |
| 🟡 P2 | 4.4 NTP Client | Accurate time |
| 🟡 P2 | 9. Recycle Bin | Safe deletion |
| 🟡 P2 | 10. ZIP Compression | Archive support |
| 🟡 P2 | 11.4 Autostart | Startup programs |
| 🟢 P3 | 3.3–3.4 Clipboard History + Win32 | Polish features |
| 🟢 P3 | 6. Search & Indexing | File discovery |
| 🟢 P3 | 1.5 Win32 Registry Mapping | Windows compatibility |
| 🟢 P3 | 11.1 User Accounts | Multi-user (future) |
| 🔵 P4 | 11.2 Win32 System Info | Compatibility APIs |
| 🔵 P4 | 11.3 Notification Service | Desktop integration |
