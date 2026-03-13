# Phase 03 — System Services

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

**Prompt:** The service manager provides start/stop/restart lifecycle for background daemons. Each service is tracked by a `struct service` with state + PID. `svc_start` forks and execs the service binary (or creates a kernel thread for kernel-level services). `svc_stop` sends SIGTERM (from Phase 01 §2.2) and waits, then SIGKILL if necessary. Service definitions are stored in the Registry under `HKLM\SYSTEM\Services\{name}\`. Consider whether services should be kernel threads (simpler, but runs in kernel space) or separate processes (more isolated, but requires user-mode exec). Start with kernel threads since the process model may not be fully mature. After completing all items, create `docs/architecture/services.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: service manager"`.


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

**Prompt:** Register core OS services that start automatically at boot. `netd` manages the network stack (DHCP renewal, ARP cache), `ntpd` runs NTP time sync, `registryd` handles periodic Registry dirty-flag flushes to disk, `indexd` rebuilds the file search index. Each service's config lives in the Registry: `HKLM\SYSTEM\Services\{name}\AutoStart` (REG_DWORD), `HKLM\SYSTEM\Services\{name}\ExePath` (REG_SZ). Services start after kernel init is complete and the filesystem is mounted. Dependency ordering matters: netd must start before ntpd. After completing all items, update `docs/architecture/services.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: built-in services"`.


- [ ] Register `netd` — network stack (DHCP, ARP) — auto-start
- [ ] Register `ntpd` — NTP time sync — auto-start after network
- [ ] Register `registryd` — Registry dirty-flag flush — auto-start
- [ ] Register `indexd` — file search indexer — auto-start
- [ ] Store service config in Registry: `HKLM\SYSTEM\Services\{name}\AutoStart`, `HKLM\SYSTEM\Services\{name}\ExePath`
- [ ] Auto-start services at boot (after kernel init)
- [ ] Commit: `"kernel: built-in services"`

### 2.3 Service Shell Command

**Prompt:** The `sc` shell command provides service management from the terminal: `sc list` shows all services with their current state (RUNNING/STOPPED), `sc start netd` starts a service, `sc stop netd` stops it, `sc status netd` shows detailed info (PID, uptime, auto-start setting). Model this after Windows' `sc` command. After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"shell: sc service control command"`.


- [ ] `sc list` — show all services with state
- [ ] `sc start <name>` / `sc stop <name>` / `sc restart <name>`
- [ ] `sc status <name>` — show detailed service info
- [ ] Commit: `"shell: sc service control command"`

---

## 3. Clipboard

### 3.1 System Clipboard

**Prompt:** The clipboard is a single kernel-resident buffer shared between all processes. `clipboard_set(fmt, data, size)` copies data into the buffer with a format tag (TEXT, IMAGE, FILES). `clipboard_get(fmt, buf, max)` retrieves it. The data must be deep-copied on set (not just pointer storage) since the source process may free its buffer. Syscalls SYS_CLIPBOARD_SET/GET let user-mode apps access the clipboard — the kernel copies data across address space boundaries. Keep it simple: one clipboard entry at a time, with history handled separately in §3.4. After completing all items, create `docs/architecture/clipboard.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: system clipboard"`.


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

**Prompt:** Ctrl+C/X/V must be wired through the keyboard event pipeline to reach the focused control. The flow: keyboard driver → WM key event → check for global shortcuts (Win+key) → dispatch to focused window → focused control handles Ctrl+C by reading its selection and calling `clipboard_set`. This requires that text controls (textbox, terminal) track a selection range. Ctrl+X does copy + delete selection. Ctrl+V calls `clipboard_get` and inserts at cursor. Must not conflict with existing Ctrl+C (SIGINT) in the terminal — SIGINT should only fire when no text is selected. After completing all items, update `docs/architecture/clipboard.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: clipboard keyboard shortcuts"`.


- [ ] Ctrl+C in focused control → copy selection to clipboard
- [ ] Ctrl+X in focused control → cut selection to clipboard
- [ ] Ctrl+V in focused control → paste from clipboard
- [ ] Wire shortcuts through WM → focused window → focused control
- [ ] Commit: `"desktop: clipboard keyboard shortcuts"`

### 3.3 Win32 Clipboard Mapping

**Prompt:** Windows apps use `OpenClipboard`/`SetClipboardData`/`GetClipboardData`/`CloseClipboard` for clipboard access. `OpenClipboard` and `CloseClipboard` are no-ops (Impossible OS clipboard doesn't need exclusive locking). Map `CF_TEXT` (format 1) to `CLIP_TEXT`, `CF_BITMAP` to `CLIP_IMAGE`. Add these stubs to the `user32.dll` builtin table in the Win32 compatibility layer. After completing all items, update `docs/architecture/clipboard.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: clipboard API stubs"`.


- [ ] `OpenClipboard()` / `CloseClipboard()` → no-op
- [ ] `SetClipboardData(CF_TEXT, data)` → `clipboard_set(CLIP_TEXT, ...)`
- [ ] `GetClipboardData(CF_TEXT)` → `clipboard_get(CLIP_TEXT, ...)`
- [ ] `EmptyClipboard()` → `clipboard_clear()`
- [ ] Add to `user32.dll` builtin stub table
- [ ] Commit: `"win32: clipboard API stubs"`

### 3.4 Clipboard History

**Prompt:** Clipboard history keeps the last 25 entries in a ring buffer — each entry is a deep copy of the clipboard data plus metadata (format, timestamp, source app name). Win+V opens a popup that lists recent entries with previews (first line of text, or "Image" for non-text). Clicking an entry sets it as the current clipboard content and sends a paste event to the focused control. The popup is a WM window with a vertical scrollable list. Configurable via Registry: `HKLM\SYSTEM\Clipboard\HistoryEnabled` (REG_DWORD), `HKLM\SYSTEM\Clipboard\MaxItems` (REG_DWORD, default 25). After completing all items, update `docs/architecture/clipboard.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: clipboard history (Win+V)"`.


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

**Prompt:** The time system combines the CMOS RTC (wall-clock calendar time) with the PIT tick counter (monotonic nanosecond-precision uptime). On boot, `time_init()` reads the RTC to get the calendar time, converts to Unix epoch (seconds since 1970-01-01), and records the corresponding PIT tick count. `time_now()` returns `boot_time + (current_ticks - boot_ticks) / ticks_per_second + ntp_offset`. The NTP offset is applied by `time_set()` when NTP §4.4 completes sync. `time_to_datetime` converts a Unix timestamp to a broken-down struct (year/month/day/hour/minute/second) using standard calendar math (handle leap years). After completing all items, create `docs/architecture/time.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: wall-clock time system"`.


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

**Prompt:** `time_format` implements strftime-style formatting with specifiers: `%H` (24h hour), `%I` (12h hour), `%M` (minutes), `%S` (seconds), `%p` (AM/PM), `%Y` (year), `%m` (month), `%d` (day). Walk the format string character by character, replacing `%X` tokens with formatted values from the datetime struct. Read user preferences from Registry: `HKLM\SYSTEM\DateTime\Use24Hour` (REG_DWORD), `HKLM\SYSTEM\DateTime\DateFormat` (REG_SZ, e.g., "YYYY-MM-DD" or "MM/DD/YYYY"). These preferences drive the taskbar clock (§4.3) and file timestamps throughout the UI. After completing all items, update `docs/architecture/time.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: time formatting"`.


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

**Prompt:** The taskbar clock is currently a simple time display — enhance it with two lines: time (larger font, e.g., FONT_UI at 14px) and date below (smaller, 11px). Use `time_format` from §4.2 with the user's configured format. Update the display every second by comparing the PIT tick count. The clock occupies the right side of the taskbar. Clicking it could eventually open a calendar popup or Date & Time settings applet. After completing all items, update `docs/architecture/time.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: enhanced taskbar clock"`.


- [ ] Draw two lines on right side of taskbar: time (large) + date (small)
- [ ] Display in configured format (12h/24h, date format from Registry)
- [ ] Update every second (compare PIT ticks)
- [ ] *(Stretch)* Click clock → open calendar popup or Date & Time settings
- [ ] Commit: `"desktop: enhanced taskbar clock"`

### 4.4 NTP Client

**Prompt:** NTP synchronizes the system clock with internet time servers. Send an SNTPv4 packet (48 bytes) to a time server via UDP port 123. The response contains a transmit timestamp in NTP format (seconds since 1900-01-01 — subtract 2208988800 to convert to Unix epoch). Use hardcoded Google NTP IPs (216.239.35.0, .4, .8, .12) since DNS may not be available yet. Hook into the UDP receive path to route port-123 responses to `ntp_handle_response()`. Auto-sync after DHCP completes at boot. The NTP offset adjusts `time_now()` from §4.1 without modifying the RTC. Store config in Registry: `HKLM\SYSTEM\DateTime\NTPEnabled`, `HKLM\SYSTEM\DateTime\NTPServer`. After completing all items, update `docs/architecture/time.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"net: NTP time sync client"`.


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

**Prompt:** Timezones are stored as a signed integer offset in minutes from UTC (e.g., SAST is UTC+2 = +120 minutes, EST is UTC-5 = -300 minutes). Store in Registry: `HKLM\SYSTEM\DateTime\TimezoneOffset` (REG_DWORD) and `HKLM\SYSTEM\DateTime\TimezoneName` (REG_SZ). `time_now_local()` from §4.1 adds the offset to UTC time. The default should be UTC+0 until configured. A stretch goal is a timezone selector dropdown in the Settings Panel showing common timezone names and their offsets. After completing all items, update `docs/architecture/time.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: timezone support"`.


- [ ] Store timezone in Registry: `HKLM\SYSTEM\DateTime\TimezoneOffset` (minutes from UTC)
- [ ] Store timezone name: `HKLM\SYSTEM\DateTime\TimezoneName` (e.g., "SAST")
- [ ] Default: detect from locale or set UTC+0
- [ ] *(Stretch)* Timezone selector in Settings Panel
- [ ] Commit: `"kernel: timezone support"`

---

## 5. File Associations

> **Moved to [TODO-P0301-Resources.md](TODO-P0301-Resources.md) §1** — File type icon mapping, extension-to-app mapping, default associations, "Open With" dialog.

---

## 6. Search & File Indexing

### 6.1 Search Index

**Prompt:** The search index provides fast filename lookup without scanning the entire filesystem on each query. Walk the VFS tree (starting from `C:\`) recursively, recording each file/folder name and full path in a flat index file (`C:\Impossible\System\Cache\search.idx`). The index is a sorted array of `{name, path}` entries enabling binary-search-based prefix matching. Rebuild the index in a background thread at boot and periodically (every 30 minutes). Keep the index in memory for fast queries, flush to disk for persistence across reboots. After completing all items, create `docs/architecture/search.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: file search indexer"`.


- [ ] Define `struct search_result` (path, match, type [FILE/FOLDER/APP/SETTING], modified)
- [ ] Create `src/kernel/search.c`
- [ ] Implement `search_index_rebuild()` — walk VFS tree, index file/folder names
- [ ] Store index in `C:\Impossible\System\Cache\search.idx` (flat file: path + name)
- [ ] Run index rebuild in background thread on boot + periodically (30 min)
- [ ] Commit: `"kernel: file search indexer"`

### 6.2 Search Query

**Prompt:** `search_query(query, results, max)` performs substring matching against the in-memory index. Search multiple sources: filenames from the index, app names by scanning `C:\Impossible\Bin\` and `C:\Programs\`, and optionally settings panel names. Results are ranked: exact match > prefix match > substring match. The SYS_SEARCH syscall lets user-mode apps (Start menu search bar, file manager) access this API. Keep the query fast (<50ms for 10K indexed items) by using the sorted index with binary search for prefix matches. After completing all items, update `docs/architecture/search.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: search query API"`.


- [ ] Implement `search_query(query, results, max)` — substring match on index
- [ ] Search sources:
  - [ ] File names (from index)
  - [ ] App names (scan `C:\Impossible\Bin\` + `C:\Programs\`)
  - [ ] *(Stretch)* File contents (scan text files — slow path)
  - [ ] *(Stretch)* Settings panel names
- [ ] Add `SYS_SEARCH` syscall
- [ ] Commit: `"kernel: search query API"`

### 6.3 Integration

**Prompt:** Wire the search API into the desktop UI: typing in the Start menu search bar filters apps and files, the File Manager search bar filters the current directory or searches globally with a `?` prefix, and a `find` shell command searches from the command line. These are stretch goals that depend on the Start menu (Phase 04 §2) and File Manager (Phase 05 §2) being implemented. After completing all items, update `docs/architecture/search.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: search integration"`.


- [ ] *(Stretch)* Start menu search bar → type to search apps + files
- [ ] *(Stretch)* File manager search bar → filter current directory
- [ ] *(Stretch)* Shell `find <query>` command
- [ ] Commit: `"desktop: search integration"`

---

## 7. Shortcut Files (.lnk)

> **Moved to [TODO-P0301-Resources.md](TODO-P0301-Resources.md) §2** — Shortcut format, API, icon integration, desktop & Start Menu rendering.

---

## 8. Scheduled Tasks

### 8.1 Task Scheduler

**Prompt:** The task scheduler runs registered commands at specified intervals. Each `struct sched_task` has a name, command string, interval in seconds, next_run timestamp, and enabled flag. `sched_task_tick()` is called by the PIT timer handler every second — it checks each task's next_run against the current time and executes due tasks by spawning a new thread. After execution, `next_run` is updated to `current_time + interval`. This is a simpler alternative to the full Service Manager (§2.1) for periodic tasks. After completing all items, create `docs/architecture/scheduler.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: task scheduler"`.


- [ ] Define `struct sched_task` (name, command, arguments, interval_seconds, next_run, enabled)
- [ ] Create `src/kernel/scheduler_tasks.c`
- [ ] Implement `sched_task_add(task)` — register a scheduled task
- [ ] Implement `sched_task_remove(name)` — unregister
- [ ] Implement `sched_task_tick()` — called by timer, check if any tasks are due
- [ ] Hook `sched_task_tick()` into PIT timer (check every second)
- [ ] Commit: `"kernel: task scheduler"`

### 8.2 Built-In Scheduled Tasks

**Prompt:** Register default system tasks: NTP sync every 60 minutes (depends on §4.4), Registry dirty-flag flush every 2 seconds (already done via compositor loop, consolidate here), search index rebuild every 30 minutes (§6.1), and log rotation every 24 hours. Store task definitions in Registry: `HKLM\SYSTEM\Scheduler\Tasks\{name}\Interval` (REG_DWORD, seconds), `HKLM\SYSTEM\Scheduler\Tasks\{name}\Enabled` (REG_DWORD). This allows users to enable/disable or adjust intervals through the regedit shell command. After completing all items, update `docs/architecture/scheduler.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: built-in scheduled tasks"`.


- [ ] NTP sync — every 60 minutes
- [ ] Registry flush — every 2 seconds (dirty-flag)
- [ ] Search index rebuild — every 30 minutes
- [ ] Log rotate — every 24 hours (trim old entries)
- [ ] Store tasks in Registry: `HKLM\SYSTEM\Scheduler\Tasks\{name}\*`
- [ ] Commit: `"kernel: built-in scheduled tasks"`

---

## 9. Recycle Bin

### 9.1 Recycle Bin Core

**Prompt:** The recycle bin makes file deletion recoverable. `trash_delete(path)` moves the file to `C:\Recycle\` (renaming to a unique name to prevent collisions) and saves metadata about where it came from. `trash_restore(trash_name)` reads the metadata and moves the file back. `trash_empty()` permanently deletes everything. The VFS `delete` operation should be redirected through `trash_delete` so all user-initiated deletions go to the recycle bin. Only explicit "Delete permanently" or trash flushing should use VFS permanent delete. After completing all items, create `docs/architecture/recycle-bin.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: recycle bin"`.


- [ ] Create `src/kernel/trash.c`
- [ ] Implement `trash_delete(path)` — move file to `C:\Recycle\`, save metadata
- [ ] Implement `trash_restore(trash_name)` — move back to original path (from metadata)
- [ ] Implement `trash_empty()` — permanently delete all files in recycle
- [ ] Implement `trash_count()` — number of items
- [ ] Implement `trash_size()` — total bytes used
- [ ] Commit: `"kernel: recycle bin"`

### 9.2 Metadata Files

**Prompt:** Each trashed file gets a `.meta` sidecar file in `C:\Recycle\_meta\` containing INI-formatted metadata: `OriginalPath` (full path before deletion), `DeletedAt` (Unix timestamp from §4.1), `Size` (bytes). The file is named to match the trashed file's renamed entry. `trash_restore` reads this metadata to know where to move the file back. `trash_count` and `trash_size` iterate the metadata directory. After completing all items, update `docs/architecture/recycle-bin.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: recycle bin metadata"`.


- [ ] Create `.meta` file for each trashed item in `C:\Recycle\_meta\`
- [ ] Store: OriginalPath, DeletedAt (Unix timestamp), Size
- [ ] INI format for easy parsing
- [ ] Commit: `"kernel: recycle bin metadata"`

### 9.3 Desktop Integration

> **Moved to [TODO-P0301-Resources.md](TODO-P0301-Resources.md) §3** — Recycle Bin desktop icon states (`ICON_TRASH_EMPTY`/`ICON_TRASH_FULL`), context menu, auto-purge.

---

## 10. ZIP Compression

### 10.1 miniz Integration

**Prompt:** miniz is a single-file MIT-licensed zlib-compatible compression library. Check if it's already been ported to `src/libs/miniz/` from Phase 01 §11.2 — if so, verify the build and skip. Otherwise, copy miniz.c/miniz.h into `src/libs/miniz/`, redirect `mz_malloc`/`mz_free` to `kmalloc`/`kfree`, and compile with `-ffreestanding`. This provides the deflate/inflate backend for the ZIP API in §10.2. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: miniz compression library"`.


- [ ] Port **miniz** (MIT, single file, ~4000 lines) into `src/libs/miniz/`
- [ ] Redirect memory: `mz_malloc → kmalloc`, `mz_free → kfree`
- [ ] Commit: `"libs: miniz compression library"`

### 10.2 ZIP API

**Prompt:** The ZIP API wraps miniz's `mz_zip_reader_*` and `mz_zip_writer_*` functions into a simpler kernel API. `zip_create(path, files, count)` creates a new ZIP archive from a list of file paths. `zip_extract(path, dest)` extracts all files to a destination directory. `zip_list(path, names, max)` lists the archive contents. `zip_add_file(path, file)` appends a file to an existing archive. These functions use VFS to read/write the archive and its contents. Test by creating a ZIP, listing its contents, and extracting it. After completing all items, create `docs/architecture/zip.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: ZIP archive support"`.


- [ ] Create `include/zip.h` and `src/kernel/zip.c` (wrapper around miniz)
- [ ] Implement `zip_create(zip_path, files[], count)` — create ZIP from files
- [ ] Implement `zip_extract(zip_path, dest_dir)` — extract all files
- [ ] Implement `zip_list(zip_path, names, max)` — list archive contents
- [ ] Implement `zip_add_file(zip_path, file_path)` — add file to existing ZIP
- [ ] Commit: `"kernel: ZIP archive support"`

### 10.3 Shell & UI Integration

**Prompt:** Shell commands: `zip archive.zip file1 file2` creates a ZIP from files, `unzip archive.zip [dest]` extracts. These are thin wrappers around the ZIP API from §10.2. Stretch goals include right-click context menu entries (Phase 04 §4.3): "Compress to ZIP" on selected files, "Extract Here" and "Extract to..." on .zip files. After completing all items, add to `docs/user/shell-commands.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"shell: zip/unzip commands"`.


- [ ] Shell commands: `zip archive.zip file1 file2`, `unzip archive.zip`
- [ ] *(Stretch)* Right-click → "Compress to ZIP"
- [ ] *(Stretch)* Right-click `.zip` → "Extract here" / "Extract to..."
- [ ] Commit: `"shell: zip/unzip commands"`

---

## 11. Agent-Recommended Additions

> Items not in the research files but important for system services.

### 11.1 User Account System

**Prompt:** A basic user account system tracks the current user with a username, UID, home directory, and password hash. Create a default "Default" user on first boot with home directory `C:\Users\Default\`. Per-user Registry subtrees (HKU\{username}\) allow different settings per user. This is foundational for Phase 09 (full security) but a minimal version is needed now for file permissions and HOME environment variable. Password verification uses monocypher's Argon2 hash (Phase 01 §11.3) but can be deferred until Phase 09. After completing all items, create `docs/architecture/user-accounts.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: user account system"`.


- [ ] Define `struct user_account` (username, uid, home_dir, password_hash)
- [ ] Create default user "Default" on first boot
- [ ] Per-user Registry trees (`HKU\{username}\`)
- [ ] Per-user home directories (`C:\Users\{username}\`)
- [ ] Current user tracked in kernel (for file permissions, environment)
- [ ] *(Stretch)* Login screen at boot
- [ ] *(Stretch)* Password verification (hash with monocypher Argon2)
- [ ] Commit: `"kernel: user account system"`

### 11.2 Win32 System Info APIs

**Prompt:** Many Windows programs call `GetSystemInfo`, `GetVersionEx`, `GetComputerName`, `GetUserName`, `GetTempPath`, `GetSystemDirectory`, and `GetWindowsDirectory` during startup. These are simple stubs that return values from the Registry or hardcoded paths. `GetSystemInfo` returns CPU count (1), page size (4096), architecture (x86-64). `GetVersionEx` returns an OS version mimicking Windows 10 (major=10, minor=0) so programs don't refuse to run. Add to the `kernel32.dll` builtin stub table. After completing all items, update `docs/architecture/win32-stubs.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: system info API stubs"`.


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

**Prompt:** The kernel-side notification service provides a queue that drivers and kernel subsystems can push messages to. `notify_send(source, message, severity)` adds an entry with a timestamp. The desktop notification popup (Phase 02 §9.3) periodically checks this queue and displays new entries. Sources include: NTP sync success, DHCP IP assignment, disk mount events, network state changes, and error conditions. This decouples the kernel from the UI — drivers don't need to know about the desktop to send notifications. After completing all items, create `docs/architecture/notifications.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: notification service"`.


- [ ] Kernel-side notification queue (source, message, severity, timestamp)
- [ ] `notify_send(source, message, severity)` — kernel/driver API
- [ ] Desktop notification popup from Phase 02 reads from this queue
- [ ] Used by: NTP sync success, disk mount, network changes, errors
- [ ] Commit: `"kernel: notification service"`

### 11.4 Autostart / Startup Programs

**Prompt:** After the desktop is fully initialized, scan `C:\Users\{name}\AppData\Startup\` for .lnk files (§7.1) and execute each shortcut's target. Also check Registry: `HKLM\SYSTEM\Boot\Run` for commands to run every boot, and `HKLM\SYSTEM\Boot\RunOnce` for commands to run once then auto-delete the entry. RunOnce is useful for post-install setup or one-time migrations. The Task Manager (Phase 05 §8.1) should have a "Startup" tab listing autostart entries with enable/disable toggles. After completing all items, create `docs/architecture/autostart.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: startup program execution"`.


- [ ] Scan `C:\Users\{name}\AppData\Startup\` for `.lnk` files at boot
- [ ] Execute each shortcut after desktop is fully initialized
- [ ] Registry: `HKLM\SYSTEM\Boot\RunOnce` — list of commands to run once then remove
- [ ] Registry: `HKLM\SYSTEM\Boot\Run` — list of commands to run every boot
- [ ] Commit: `"kernel: startup program execution"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. Registry System | Foundation — all other services store config here |
| 🔴 P0 | 4.1–4.2 Time System | Wall-clock time for timestamps, logs, scheduler |
| 🔴 P0 | 8. Scheduled Tasks | Powers Registry flush, NTP sync, log rotate |
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
