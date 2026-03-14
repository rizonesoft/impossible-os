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

> **Moved to [TODO-P0008-Search.md](TODO-P0008-Search.md)** — Search index,
> query API, Start Menu / File Manager / shell integration.

---

## 7. Shortcut Files (.lnk)

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §2** — Shortcut format, API, icon integration, desktop & Start Menu rendering.

---

## 8. Scheduled Tasks

> **Moved to [TODO-P0009-Scheduler.md](TODO-P0009-Scheduler.md)** — Task scheduler,
> built-in tasks (NTP sync, Registry flush, search index, log rotate).

---

## 9. Recycle Bin

> **Moved to [TODO-P0010-Recycle-Bin.md](TODO-P0010-Recycle-Bin.md)** — Trash core,
> metadata files, desktop integration (via P0302).

---

## 10. ZIP Compression

> **Moved to [TODO-P0011-ZIP.md](TODO-P0011-ZIP.md)** — miniz integration,
> ZIP API, shell commands (zip/unzip).

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
| 🟡 P2 | **P0010** Recycle Bin | Safe deletion — see `TODO-P0010-Recycle-Bin.md` |
| 🟡 P2 | **P0011** ZIP | Archive support — see `TODO-P0011-ZIP.md` |
| 🟡 P2 | §11.4 Autostart | Startup programs |
| 🟢 P3 | **P0007** §3–4 Clipboard History + Win32 | Polish features |
| 🟢 P3 | **P0008** Search & Indexing | File discovery — see `TODO-P0008-Search.md` |
| 🟢 P3 | **P0009** Scheduled Tasks | Automation — see `TODO-P0009-Scheduler.md` |
| 🟢 P3 | §11.1 User Accounts | Multi-user (see P2301) |
| 🔵 P4 | §11.2 Win32 System Info | Compatibility APIs |
| 🔵 P4 | §11.3 Notification Service | Desktop integration |

