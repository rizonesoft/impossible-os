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

> **Moved to [TODO-P0007-Clipboard.md](TODO-P0007-Clipboard.md)** — System clipboard,
> Ctrl+C/X/V shortcuts, Win32 API mapping, clipboard history (Win+V).

---

## 4. Clock & Time System

> **Moved to [TODO-P0203-Clock.md](TODO-P0203-Clock.md)** — Kernel time API,
> time formatting, taskbar clock, NTP client, timezone support.

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
| 🔴 P0 | **P0203** §1–2 Time System | Wall-clock time for timestamps, logs, scheduler |
| 🔴 P0 | §8 Scheduled Tasks | Powers Registry flush, NTP sync, log rotate |
| 🟠 P1 | §2 Services/Daemons | Background processes infrastructure |
| 🟠 P1 | **P0007** §1–2 Clipboard | Essential UX — see `TODO-P0007-Clipboard.md` |
| 🟠 P1 | §5 File Associations | Double-click opens correct app |
| 🟠 P1 | §7 Shortcut Files | Desktop/Start Menu proper UX |
| 🟡 P2 | **P0203** §4 NTP Client | Accurate time |
| 🟡 P2 | §9 Recycle Bin | Safe deletion |
| 🟡 P2 | §10 ZIP Compression | Archive support |
| 🟡 P2 | §11.4 Autostart | Startup programs |
| 🟢 P3 | **P0007** §3–4 Clipboard History + Win32 | Polish features |
| 🟢 P3 | §6 Search & Indexing | File discovery |
| 🟢 P3 | §11.1 User Accounts | Multi-user (see P2301) |
| 🔵 P4 | §11.2 Win32 System Info | Compatibility APIs |
| 🔵 P4 | §11.3 Notification Service | Desktop integration |
