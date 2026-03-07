# Phase 12 — System Maintenance & Recovery

> **Goal:** Make Impossible OS self-maintaining and resilient: automatic system
> updates with verification, a package manager for installing/uninstalling apps,
> restore points for rollback, and a recovery environment for repairing broken
> installs — providing the reliability infrastructure expected of a real OS.

---

## 1. System Updates
> *Research: [01_system_updates.md](research/phase_12_system_maintenance/01_system_updates.md)*

### 1.1 Update Check API

- [ ] Create `src/apps/updater/updater.c` and `include/update.h`
- [ ] Define `struct update_info` (version, url, hash, size, type, description)
- [ ] Implement `update_check(info)`:
  - [ ] HTTP GET to update server: `https://impossible-os.dev/api/version`
  - [ ] Parse JSON/INI response: latest version, download URL, SHA-256 hash
  - [ ] Compare with current version (Codex: `System\Version`)
  - [ ] Return: update available (yes/no) + info
- [ ] Commit: `"apps: update check API"`

### 1.2 Update Download & Verification

- [ ] Implement `update_download(info, path)`:
  - [ ] HTTP GET download URL → save to `C:\Temp\update.ipkg`
  - [ ] Show progress: bytes downloaded / total size
- [ ] Implement `update_verify(path, expected_hash)`:
  - [ ] Compute SHA-256 hash of downloaded file (via monocypher)
  - [ ] Compare with expected hash from server
  - [ ] Reject if hash mismatch (corrupted or tampered)
- [ ] Commit: `"apps: update download + SHA-256 verification"`

### 1.3 Update Application

- [ ] Implement `update_apply(path)`:
  - [ ] **Create restore point first** (via `restore_create()`)
  - [ ] Extract `.ipkg` update package (ZIP format)
  - [ ] Replace system files in `C:\Impossible\System\`
  - [ ] Update Codex: `System\Version` → new version
  - [ ] Update Codex: `System\Update\LastUpdate` → timestamp
- [ ] Handle update types:
  - [ ] **Hotfix**: single file replacement (<100 KB)
  - [ ] **Minor update**: bug fixes, small features (1–5 MB)
  - [ ] **Major update**: kernel changes, full image (~10+ MB)
- [ ] Prompt user: "Update installed. Restart now? [Restart] [Later]"
- [ ] Commit: `"apps: update apply + auto-restore-point"`

### 1.4 Update Settings

- [ ] `update.spl` settings applet:
  - [ ] "Check for updates" button (manual check)
  - [ ] Auto-check toggle (Codex: `System\Update\AutoCheck`)
  - [ ] Check frequency: daily / weekly / never
  - [ ] Update channel: stable / beta (Codex: `System\Update\Channel`)
  - [ ] Last check timestamp display
  - [ ] Update history (list of installed updates)
- [ ] Auto-check at boot (if enabled): background task checks for updates silently
- [ ] Notification toast: "A system update is available (v0.3.0)" with [Install] action
- [ ] Commit: `"apps: update settings applet"`

---

## 2. App Installer & Package Manager
> *Research: [04_app_installer.md](research/phase_12_system_maintenance/04_app_installer.md)*

### 2.1 IPKG Package Format

- [ ] Define `.ipkg` format (ZIP archive containing):
  - [ ] `manifest.ini` — app metadata (Name, Version, Author, Icon, Description, InstallPath, StartMenu, Desktop)
  - [ ] `install.ini` — file destinations, Codex entries, shortcuts
  - [ ] `files/` — app executable + libraries + data
- [ ] Commit: `"apps: IPKG package format specification"`

### 2.2 App Installer

- [ ] Create `src/apps/installer/installer.c`
- [ ] Parse `.ipkg` file: extract ZIP (via miniz), read `manifest.ini` and `install.ini`
- [ ] Installer UI:
  - [ ] Welcome screen: app name, version, author, description, icon
  - [ ] Install path selection (default from `manifest.ini`)
  - [ ] [Install] button → progress
- [ ] Install process:
  - [ ] Create install directory (e.g., `C:\Programs\MyApp\`)
  - [ ] Extract files from `files/` → install directory
  - [ ] Write Codex entries from `[Registry]` section
  - [ ] Create Start Menu shortcut (if `StartMenu = 1`)
  - [ ] Create Desktop shortcut (if `Desktop = 1`)
  - [ ] Register in Codex: `System\Apps\{name}\Version`, `System\Apps\{name}\InstallPath`
- [ ] **Create restore point before install**
- [ ] Commit: `"apps: app installer"`

### 2.3 App Uninstaller

- [ ] Read `System\Apps\{name}\InstallPath` from Codex
- [ ] Reverse install: delete files, remove Codex entries, remove shortcuts
- [ ] Confirmation dialog: "Uninstall {App Name}? This will remove the application."
- [ ] Clean up empty directories after file removal
- [ ] Commit: `"apps: app uninstaller"`

### 2.4 Add/Remove Programs UI

- [ ] `apps.spl` settings applet (or integrated in Settings Panel)
- [ ] List all installed apps: name, version, size, install date
- [ ] [Uninstall] button per app
- [ ] Search/filter installed apps
- [ ] Commit: `"apps: Add/Remove Programs"`

### 2.5 IPKG Build Tool (Host-Side)

- [ ] Create `tools/ipkg_create.c` (runs on build host, not on OS)
- [ ] Pack a directory into `.ipkg` (ZIP with manifest + install instructions)
- [ ] Usage: `./ipkg_create --name "My App" --version "1.0.0" --dir ./myapp/ --output myapp.ipkg`
- [ ] Validate manifest before packaging
- [ ] Commit: `"tools: IPKG package build tool"`

### 2.6 File Associations from Packages

- [ ] `install.ini` `[Associations]` section: `.txt = myapp.exe`, etc.
- [ ] Installer registers file associations in Codex: `System\FileAssoc\{ext}\Program`
- [ ] Uninstaller removes associations
- [ ] Commit: `"apps: package file associations"`

---

## 3. System Restore
> *Research: [03_system_restore.md](research/phase_12_system_maintenance/03_system_restore.md)*

### 3.1 Restore Point Creation

- [ ] Create `src/kernel/restore.c` and `include/restore.h`
- [ ] Implement `restore_create(description)`:
  - [ ] Create directory: `C:\Impossible\System\Restore\{timestamp}\`
  - [ ] Write `manifest.ini`: timestamp, description, OS version
  - [ ] Back up all Codex files → `codex_backup/`
  - [ ] Snapshot changed system files → `system_files.tar` (simple tar-like archive)
- [ ] Define `struct restore_point` (id, timestamp, description, size)
- [ ] Commit: `"kernel: restore point creation"`

### 3.2 Restore Point Management

- [ ] Implement `restore_list(out, max)` — list all restore points (sorted by date)
- [ ] Implement `restore_cleanup(keep_count)` — delete oldest, keep last N (default 5)
- [ ] Auto-cleanup when disk space is low
- [ ] Commit: `"kernel: restore point management"`

### 3.3 System Rollback

- [ ] Implement `restore_apply(restore_id)`:
  - [ ] Restore Codex files from `codex_backup/`
  - [ ] Restore system files from `system_files.tar`
  - [ ] Update Codex: `System\Restore\LastRestore` → timestamp
  - [ ] Prompt restart
- [ ] Commit: `"kernel: system rollback"`

### 3.4 Automatic Restore Points

- [ ] Auto-create before OS update (from updater)
- [ ] Auto-create before app install (from installer)
- [ ] Manual: Settings → System → "Create restore point" button
- [ ] Commit: `"kernel: automatic restore points"`

### 3.5 Restore UI

- [ ] Settings → System → "System Restore" panel:
  - [ ] List of restore points (date, description, size)
  - [ ] [Restore] button → confirmation → rollback
  - [ ] [Create] button → manual restore point
  - [ ] [Delete] button → remove specific point
- [ ] Also accessible from Recovery Environment
- [ ] Commit: `"apps: system restore UI"`

---

## 4. Recovery Environment
> *Research: [02_recovery_environment.md](research/phase_12_system_maintenance/02_recovery_environment.md)*

### 4.1 Recovery Boot Menu

- [ ] Create `src/recovery/recovery.c`
- [ ] Hold F8 at boot → enter Recovery Environment (intercept in bootloader/early kernel)
- [ ] Text-mode menu:
  ```
  Impossible OS Recovery
  ──────────────────────
  1. Reset to factory defaults
  2. System Restore (rollback to restore point)
  3. Command Prompt (recovery shell)
  4. Startup Repair (fix boot issues)
  5. Reinstall OS (keep user files)
  6. Boot from USB
  ```
- [ ] Commit: `"recovery: boot menu"`

### 4.2 Recovery Shell

- [ ] Minimal text-mode shell (no GUI, serial-capable)
- [ ] Basic commands: `ls`, `cd`, `cat`, `cp`, `mv`, `rm`, `pwd`
- [ ] Disk tools:
  - [ ] `fsck` — check and repair filesystem
  - [ ] `fdisk` — view partition table
- [ ] Codex tools:
  - [ ] `codex-reset` — restore Codex to factory defaults
  - [ ] `codex-get` / `codex-set` — read/write Codex values
- [ ] File backup:
  - [ ] `backup C:\Users\ D:\` — copy user files to USB drive
- [ ] Commit: `"recovery: text-mode shell with disk/codex tools"`

### 4.3 Factory Reset

- [ ] Wipe all user data + app installs
- [ ] Restore system files to original state (from recovery partition or ISO)
- [ ] Recreate default directories + first-boot setup
- [ ] Confirmation: "This will ERASE ALL DATA. Are you sure? Type YES to confirm"
- [ ] Commit: `"recovery: factory reset"`

### 4.4 Startup Repair

- [ ] Re-install bootloader (GRUB config regeneration)
- [ ] Repair MBR/GPT (rewrite partition table header)
- [ ] Verify kernel binary integrity (checksum)
- [ ] Commit: `"recovery: startup repair"`

### 4.5 Recovery Partition

- [ ] *(Stretch)* Separate minimal kernel in recovery partition (text-mode only, ~50 KB)
- [ ] *(Stretch)* Recovery partition created during OS install
- [ ] *(Stretch)* Boot from recovery partition even if main partition is corrupted

---

## 5. Agent-Recommended Additions

> Items not in the research files but critical for system maintenance.

### 5.1 Disk Cleanup

- [ ] Create `src/apps/cleanup/cleanup.c`
- [ ] Scan for deletable files:
  - [ ] `C:\Temp\` — temporary files
  - [ ] `C:\Recycle\` — recycle bin contents
  - [ ] Old restore points (keep last N)
  - [ ] Cached update packages
  - [ ] Application logs
- [ ] Display space savings per category
- [ ] [Clean up] button → delete selected categories
- [ ] Commit: `"apps: disk cleanup utility"`

### 5.2 Scheduled Tasks

- [ ] Simple task scheduler: run actions at specified times
- [ ] Schedule: auto-check for updates, disk cleanup, restore point creation
- [ ] Define `struct scheduled_task` (name, interval, last_run, callback)
- [ ] `scheduler_add(name, interval, callback)` — register task
- [ ] Run matching tasks at boot and periodically
- [ ] Codex: `System\Scheduler\{name}\Interval`, `System\Scheduler\{name}\LastRun`
- [ ] Commit: `"kernel: scheduled task system"`

### 5.3 Event Log

- [ ] System-wide event logging (beyond serial debug log)
- [ ] Event types: INFO, WARNING, ERROR, SECURITY
- [ ] Events: app install/uninstall, update applied, login/logout, crash, permission denied
- [ ] Store in `C:\Impossible\System\Logs\events.log` (rolling, max 1 MB)
- [ ] Event Viewer: settings applet showing event log with filters
- [ ] Commit: `"kernel: system event log"`

### 5.4 Crash Dump & Bug Reporter

- [ ] On kernel panic: save crash dump to `C:\Impossible\System\CrashDumps\`
- [ ] Include: registers, stack trace, last 100 serial lines, loaded drivers
- [ ] On next boot: "The system shut down unexpectedly. View crash report?"
- [ ] *(Stretch)* Send crash report to server (opt-in)
- [ ] Commit: `"kernel: crash dump and reporting"`

### 5.5 First-Boot Setup Wizard

- [ ] Runs on very first boot (fresh install) or after factory reset:
  - [ ] Welcome screen: "Welcome to Impossible OS"
  - [ ] Set timezone / region
  - [ ] Set keyboard layout
  - [ ] Create first user account (name + password)
  - [ ] Choose wallpaper
  - [ ] Optional: check for updates
  - [ ] [Finish] → boot to desktop
- [ ] Flag in Codex: `System\FirstBoot = 0` (skip wizard on subsequent boots)
- [ ] Commit: `"desktop: first-boot setup wizard"`

### 5.6 Safe Mode Boot

- [ ] Boot with minimal drivers (no network, no audio, no USB)
- [ ] Load basic VGA-resolution desktop
- [ ] Skip auto-start applications
- [ ] Can fix driver issues, uninstall problematic apps
- [ ] Select from Recovery menu or hold Shift at boot
- [ ] Commit: `"kernel: safe mode boot"`

### 5.7 OS Installer (Fresh Install)

- [ ] *(Stretch)* Boot from ISO → partition disk → format IXFS → copy system files
- [ ] *(Stretch)* Install GRUB bootloader
- [ ] *(Stretch)* Create recovery partition
- [ ] *(Stretch)* Run first-boot setup wizard
- [ ] Commit: `"installer: OS installer from ISO"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 2.1–2.2 App Installer (IPKG) | Install/manage applications |
| 🔴 P0 | 2.3 App Uninstaller | Clean removal of applications |
| 🔴 P0 | 3.1 Restore Point Creation | Safety net before changes |
| 🟠 P1 | 1.1–1.2 Update Check + Download | Keep OS current |
| 🟠 P1 | 2.5 IPKG Build Tool | Create packages for distribution |
| 🟠 P1 | 3.3 System Rollback | Recovery from bad updates |
| 🟠 P1 | 5.5 First-Boot Wizard | Clean initial setup |
| 🟡 P2 | 1.3 Update Application | Apply downloaded updates |
| 🟡 P2 | 2.4 Add/Remove Programs | GUI for managing apps |
| 🟡 P2 | 3.4–3.5 Auto Restore + UI | Transparent backup |
| 🟡 P2 | 5.3 Event Log | System diagnostics |
| 🟡 P2 | 4.1 Recovery Boot Menu | Emergency repair |
| 🟢 P3 | 4.2 Recovery Shell | Hands-on repair tools |
| 🟢 P3 | 1.4 Update Settings | Auto-update configuration |
| 🟢 P3 | 5.1 Disk Cleanup | Free disk space |
| 🟢 P3 | 5.4 Crash Dump | Debugging aid |
| 🟢 P3 | 5.2 Scheduled Tasks | Automated maintenance |
| 🟢 P3 | 5.6 Safe Mode | Driver troubleshooting |
| 🔵 P4 | 4.3–4.4 Factory Reset + Startup Repair | Full system recovery |
| 🔵 P4 | 4.5 Recovery Partition | Separate boot environment |
| 🔵 P4 | 5.7 OS Installer | Fresh install from ISO |
