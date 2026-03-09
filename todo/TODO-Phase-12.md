# Phase 12 — System Maintenance & Recovery

> **Goal:** Make Impossible OS self-maintaining and resilient: automatic system
> updates with verification, a package manager for installing/uninstalling apps,
> restore points for rollback, and a recovery environment for repairing broken
> installs — providing the reliability infrastructure expected of a real OS.

---

## 1. System Updates
> *Research: [01_system_updates.md](research/phase_12_system_maintenance/01_system_updates.md)*

### 1.1 Update Check API

**Prompt:** The update system provides a mechanism for keeping Impossible OS current. `update_check()` does an HTTP GET to the update server URL, parses the JSON/INI response for version, download URL, and SHA-256 hash, then compares with the current version stored in Codex `System\Version`. If an update is available, populate `struct update_info` with version, URL, hash, size, type (hotfix/minor/major), and description. After completing all items, create `docs/architecture/updates.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: update check API"`.


- [ ] Create `src/apps/updater/updater.c` and `include/update.h`
- [ ] Define `struct update_info` (version, url, hash, size, type, description)
- [ ] Implement `update_check(info)`:
  - [ ] HTTP GET to update server: `https://impossible-os.dev/api/version`
  - [ ] Parse JSON/INI response: latest version, download URL, SHA-256 hash
  - [ ] Compare with current version (Codex: `System\Version`)
  - [ ] Return: update available (yes/no) + info
- [ ] Commit: `"apps: update check API"`

### 1.2 Update Download & Verification

**Prompt:** `update_download(info, path)` does HTTP GET to the download URL and saves to `C:\Temp\update.ipkg` with progress reporting (bytes/total). `update_verify(path, expected_hash)` computes SHA-256 via monocypher and compares with the expected hash, rejecting corrupted or tampered downloads. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: update download + SHA-256 verification"`.


- [ ] Implement `update_download(info, path)`:
  - [ ] HTTP GET download URL → save to `C:\Temp\update.ipkg`
  - [ ] Show progress: bytes downloaded / total size
- [ ] Implement `update_verify(path, expected_hash)`:
  - [ ] Compute SHA-256 hash of downloaded file (via monocypher)
  - [ ] Compare with expected hash from server
  - [ ] Reject if hash mismatch (corrupted or tampered)
- [ ] Commit: `"apps: update download + SHA-256 verification"`

### 1.3 Update Application

**Prompt:** `update_apply(path)` first creates a restore point (§3.1) for rollback safety, then extracts the .ipkg ZIP via miniz, replaces system files in `C:\Impossible\System\`, and updates Codex version/timestamp. Handle three update types: hotfix (<100 KB single file), minor (1-5 MB), major (10+ MB kernel changes). Prompt restart. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: update apply + auto-restore-point"`.


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

**Prompt:** `update.spl` settings applet: manual check button, auto-check toggle with frequency (daily/weekly/never, Codex `System\Update\AutoCheck`), update channel (stable/beta), last check timestamp, and update history. Auto-check at boot runs as a background task with a notification toast: "System update available (v0.3.0)" with [Install]. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: update settings applet"`.


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

**Prompt:** Define `.ipkg` as a ZIP archive containing: `manifest.ini` (Name, Version, Author, Icon, Description, InstallPath, StartMenu, Desktop), `install.ini` (file destinations, Codex entries, shortcuts, file associations), and `files/` directory with the app executable + libraries + data. After completing all items, create `docs/architecture/package-format.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: IPKG package format specification"`.


- [ ] Define `.ipkg` format (ZIP archive containing):
  - [ ] `manifest.ini` — app metadata (Name, Version, Author, Icon, Description, InstallPath, StartMenu, Desktop)
  - [ ] `install.ini` — file destinations, Codex entries, shortcuts
  - [ ] `files/` — app executable + libraries + data
- [ ] Commit: `"apps: IPKG package format specification"`

### 2.2 App Installer

**Prompt:** Parse `.ipkg` via miniz ZIP extraction, read manifest.ini and install.ini. Installer UI: welcome screen with app name/version/icon, install path selection, progress bar. Install process: create dir (e.g., `C:\Programs\MyApp\`), extract files, write Codex entries, create Start Menu/Desktop shortcuts, register in Codex `System\Apps\{name}\*`. Create a restore point before install. After completing all items, update `docs/architecture/package-format.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: app installer"`.


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

**Prompt:** Read `System\Apps\{name}\InstallPath` from Codex, reverse the install: delete files, remove Codex entries, remove shortcuts. Confirmation dialog: "Uninstall {App Name}?". Clean up empty directories. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: app uninstaller"`.


- [ ] Read `System\Apps\{name}\InstallPath` from Codex
- [ ] Reverse install: delete files, remove Codex entries, remove shortcuts
- [ ] Confirmation dialog: "Uninstall {App Name}? This will remove the application."
- [ ] Clean up empty directories after file removal
- [ ] Commit: `"apps: app uninstaller"`

### 2.4 Add/Remove Programs UI

**Prompt:** `apps.spl` settings applet listing all installed apps (name, version, size, install date) with [Uninstall] button per app and search/filter. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: Add/Remove Programs"`.


- [ ] `apps.spl` settings applet (or integrated in Settings Panel)
- [ ] List all installed apps: name, version, size, install date
- [ ] [Uninstall] button per app
- [ ] Search/filter installed apps
- [ ] Commit: `"apps: Add/Remove Programs"`

### 2.5 IPKG Build Tool (Host-Side)

**Prompt:** `tools/ipkg_create.c` runs on the build host, packing a directory into a `.ipkg` ZIP with manifest + install instructions. Usage: `./ipkg_create --name "My App" --version "1.0.0" --dir ./myapp/ --output myapp.ipkg`. Validates manifest first. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"tools: IPKG package build tool"`.


- [ ] Create `tools/ipkg_create.c` (runs on build host, not on OS)
- [ ] Pack a directory into `.ipkg` (ZIP with manifest + install instructions)
- [ ] Usage: `./ipkg_create --name "My App" --version "1.0.0" --dir ./myapp/ --output myapp.ipkg`
- [ ] Validate manifest before packaging
- [ ] Commit: `"tools: IPKG package build tool"`

### 2.6 File Associations from Packages

**Prompt:** `install.ini` `[Associations]` section maps extensions to executables. Installer registers associations in Codex `System\FileAssoc\{ext}\Program`. Uninstaller reverses them. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: package file associations"`.


- [ ] `install.ini` `[Associations]` section: `.txt = myapp.exe`, etc.
- [ ] Installer registers file associations in Codex: `System\FileAssoc\{ext}\Program`
- [ ] Uninstaller removes associations
- [ ] Commit: `"apps: package file associations"`

---

## 3. System Restore
> *Research: [03_system_restore.md](research/phase_12_system_maintenance/03_system_restore.md)*

### 3.1 Restore Point Creation

**Prompt:** `restore_create(description)` creates `C:\Impossible\System\Restore\{timestamp}\` with: `manifest.ini` (timestamp, description, OS version), `codex_backup/` (all Codex files), and `system_files.tar` (snapshot of changed system files). After completing all items, create `docs/architecture/system-restore.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: restore point creation"`.


- [ ] Create `src/kernel/restore.c` and `include/restore.h`
- [ ] Implement `restore_create(description)`:
  - [ ] Create directory: `C:\Impossible\System\Restore\{timestamp}\`
  - [ ] Write `manifest.ini`: timestamp, description, OS version
  - [ ] Back up all Codex files → `codex_backup/`
  - [ ] Snapshot changed system files → `system_files.tar` (simple tar-like archive)
- [ ] Define `struct restore_point` (id, timestamp, description, size)
- [ ] Commit: `"kernel: restore point creation"`

### 3.2 Restore Point Management

**Prompt:** `restore_list()` returns all restore points sorted by date. `restore_cleanup(keep_count)` deletes oldest, keeps last N (default 5). Auto-cleanup when disk space is low. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: restore point management"`.


- [ ] Implement `restore_list(out, max)` — list all restore points (sorted by date)
- [ ] Implement `restore_cleanup(keep_count)` — delete oldest, keep last N (default 5)
- [ ] Auto-cleanup when disk space is low
- [ ] Commit: `"kernel: restore point management"`

### 3.3 System Rollback

**Prompt:** `restore_apply(restore_id)` restores Codex files from backup and system files from tar archive, updates Codex timestamp, and prompts restart. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: system rollback"`.


- [ ] Implement `restore_apply(restore_id)`:
  - [ ] Restore Codex files from `codex_backup/`
  - [ ] Restore system files from `system_files.tar`
  - [ ] Update Codex: `System\Restore\LastRestore` → timestamp
  - [ ] Prompt restart
- [ ] Commit: `"kernel: system rollback"`

### 3.4 Automatic Restore Points

**Prompt:** Automatically create restore points before OS updates (from updater) and before app installs (from installer). Manual creation via Settings → System → "Create restore point" button. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: automatic restore points"`.


- [ ] Auto-create before OS update (from updater)
- [ ] Auto-create before app install (from installer)
- [ ] Manual: Settings → System → "Create restore point" button
- [ ] Commit: `"kernel: automatic restore points"`

### 3.5 Restore UI

**Prompt:** Settings → System → "System Restore" panel lists restore points with date, description, and size. [Restore] with confirmation, [Create] for manual, [Delete] to remove specific. Also accessible from Recovery Environment. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: system restore UI"`.


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

**Prompt:** Hold F8 at boot to enter Recovery Environment (intercept in bootloader/early kernel). Text-mode menu: Reset to factory, System Restore, Command Prompt, Startup Repair, Reinstall OS, Boot from USB. After completing all items, create `docs/architecture/recovery.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"recovery: boot menu"`.


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

**Prompt:** Minimal text-mode shell with basic commands (ls, cd, cat, cp, mv, rm), disk tools (fsck, fdisk), Codex tools (codex-reset, codex-get, codex-set), and file backup (backup src dst). No GUI, serial-capable. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"recovery: text-mode shell"`.


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

**Prompt:** Wipe all user data + app installs, restore system files from recovery partition or ISO, recreate default directories + first-boot setup. Requires typed "YES" confirmation. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"recovery: factory reset"`.


- [ ] Wipe all user data + app installs
- [ ] Restore system files to original state (from recovery partition or ISO)
- [ ] Recreate default directories + first-boot setup
- [ ] Confirmation: "This will ERASE ALL DATA. Are you sure? Type YES to confirm"
- [ ] Commit: `"recovery: factory reset"`

### 4.4 Startup Repair

**Prompt:** Re-install bootloader (GRUB config regen), repair MBR/GPT headers, verify kernel binary integrity via checksum. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"recovery: startup repair"`.


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

**Prompt:** Scan deletable files: C:\Temp\, C:\Recycle\, old restore points, cached update packages, app logs. Display space savings per category. [Clean up] button deletes selected categories. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: disk cleanup utility"`.


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

**Prompt:** Simple task scheduler: `struct scheduled_task` with name, interval, last_run, callback. `scheduler_add()` registers tasks. Run at boot + periodically. Schedule: auto-update check, disk cleanup, restore point creation. Codex under `System\Scheduler\*`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: scheduled task system"`.


- [ ] Simple task scheduler: run actions at specified times
- [ ] Schedule: auto-check for updates, disk cleanup, restore point creation
- [ ] Define `struct scheduled_task` (name, interval, last_run, callback)
- [ ] `scheduler_add(name, interval, callback)` — register task
- [ ] Run matching tasks at boot and periodically
- [ ] Codex: `System\Scheduler\{name}\Interval`, `System\Scheduler\{name}\LastRun`
- [ ] Commit: `"kernel: scheduled task system"`

### 5.3 Event Log

**Prompt:** System-wide event logging with types INFO/WARNING/ERROR/SECURITY. Log: install/uninstall, updates, login/logout, crashes, permission denied. Store in `C:\Impossible\System\Logs\events.log` (rolling, max 1 MB). Event Viewer settings applet with filters. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: system event log"`.


- [ ] System-wide event logging (beyond serial debug log)
- [ ] Event types: INFO, WARNING, ERROR, SECURITY
- [ ] Events: app install/uninstall, update applied, login/logout, crash, permission denied
- [ ] Store in `C:\Impossible\System\Logs\events.log` (rolling, max 1 MB)
- [ ] Event Viewer: settings applet showing event log with filters
- [ ] Commit: `"kernel: system event log"`

### 5.4 Crash Dump & Bug Reporter

**Prompt:** On kernel panic: save crash dump (registers, stack trace, last 100 serial lines, loaded drivers) to `C:\Impossible\System\CrashDumps\`. Next boot: "System shut down unexpectedly. View crash report?" Stretch: opt-in server submission. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: crash dump and reporting"`.


- [ ] On kernel panic: save crash dump to `C:\Impossible\System\CrashDumps\`
- [ ] Include: registers, stack trace, last 100 serial lines, loaded drivers
- [ ] On next boot: "The system shut down unexpectedly. View crash report?"
- [ ] *(Stretch)* Send crash report to server (opt-in)
- [ ] Commit: `"kernel: crash dump and reporting"`

### 5.5 First-Boot Setup Wizard

**Prompt:** Runs on first boot or after factory reset: Welcome screen, set timezone/region, set keyboard layout, create first user (name + password), choose wallpaper, optional update check, [Finish] boots to desktop. Codex `System\FirstBoot = 0` skips on subsequent boots. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: first-boot setup wizard"`.


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

**Prompt:** Boot with minimal drivers (no network, audio, USB), basic VGA resolution, skip auto-start apps. Used to fix driver issues or uninstall problematic apps. Select from Recovery menu or hold Shift at boot. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: safe mode boot"`.


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
