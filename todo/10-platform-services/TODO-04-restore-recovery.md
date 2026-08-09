---
schema_version: 1
id: restore-recovery
domain: 10-platform-services
status: active
title: "TODO-04 -- System Restore, Recovery & Observability"
---

# TODO-04 -- System Restore, Recovery & Observability

> **Goal:** Make Impossible OS survivable when things go wrong -- restore points, system rollback, the F8 recovery environment, factory reset, startup repair, first-boot setup wizard, event log, crash dump viewer, and disk cleanup. This is the full OS safety net.

> [!IMPORTANT]
> **Already exists**: `klog(LOG_INFO/WARN/ERROR, source, fmt)` + `klog_entry_t` + `klog_get_ring()` in `klog.h`. `zip_create/add_file/extract/close()` from TODO-04-recycle-zip-scheduler (forward dep). `cng_sha256()` from TODO-07-cng (forward dep). `registry_backup/restore()` from TODO-02-kernel-core (forward dep -- Registry hive backup). `kernel_panic()` exists; `TODO-27-crash-dump-generation.md` adds full dump; §9 here adds on-boot prompt + viewer on top. `CTRL_LISTVIEW`, `CTRL_TABSTRIP`, `dialog_confirm()`, `CTRL_PROGRESSBAR` from TODO-05. `CTRL_MENUBAR` + `ttf_draw_string(FONT_UI)`. `sched_task_add()`. `notify_send()`. **Missing**: restore point CRUD, event log (`kevent_log()`), recovery environment, first-boot wizard, all CPL applets here. **`LOG_SECURITY`**: add new log level to `klog.h` (between `LOG_ERROR` and a new `LOG_SECURITY=4`). **Note on overlap**: §9 crash dump viewer builds on `02-kernel-core/TODO-27`; §8 event log adds a separate persistent structured event store on top of klog's ring buffer.

## Inputs

- `include/kernel/klog.h` -- `klog()`, `klog_entry_t`, `klog_get_ring()`; extend with `LOG_SECURITY=4` -- §8 event log
- `include/kernel/zip.h` (TODO-04 §6) -- `zip_create/add_file/close/open/extract()` -- §2 restore ZIP, §4 rollback extract
- `include/cng.h` (TODO-07 §1) -- `cng_sha256()` -- §2 rollback integrity verify, §7 kernel ELF verify
- `include/registry.h` -- `registry_backup()`, `registry_restore()`, `HKLM\SYSTEM\Restore\*`, `HKLM\SYSTEM\FirstBoot` -- §1, §2, §7
- `include/kernel/fs/vfs.h` -- `vfs_mkdir/rename/unlink/readdir/stat()` -- §1 restore dir, §2 file replace, §5 factory reset
- `include/kernel/fs/gpt.h` -- `gpt_write_header()`, CRC recompute -- §6 startup repair
- `include/kernel/uefi_runtime.h` -- `EFI_ResetSystem()`, NVRAM boot entry `SetVariable` -- §6 UEFI boot entry repair, §5 restart
- `include/kernel/sched/task.h` -- `sched_task_add()` -- §8 log rotate task, §10 weekly cleanup task
- `include/kernel/timer.h` -- `uptime()`, `system_get_ticks()` -- §1 restore point timestamps
- `include/desktop/controls.h` (TODO-05) -- `CTRL_LISTVIEW`, `CTRL_TABSTRIP`, `dialog_confirm()`, `CTRL_PROGRESSBAR` -- §2 rstrui.cpl, §8 event viewer, §10 cleanup UI
- `include/cpl.h` (TODO-11) -- `CPlApplet_t`, `NEWCPLINFO` -- §5 `rstrui.cpl`, §4 Event Viewer applet
- `include/desktop/notification.h` (TODO-09) -- `notify_send()` -- §10 on-boot crash report prompt
- `include/desktop/wm.h` -- `wm_create_window()` -- §7 first-boot wizard, §9 crash report viewer
- → XREF: `02-kernel-core/TODO-27` -- crash dump generation (registers, stack trace); §9 here adds on-boot prompt + formatted viewer on top
- → XREF: `08-graphics-ui/TODO-03 §3` -- F8 boot-time keyboard intercept lives there; §6 here implements the recovery menu content behind that intercept
- → XREF: `10-platform-services/TODO-03 §5` -- `update_apply()` and `installer_open()` call `restore_create()` from §1 here
- → XREF: `09-desktop-shell/TODO-04 §1` -- built-in scheduler tasks include `klog_rotate`; §7 event log rotate uses same scheduler slot
- → XREF: `09-desktop-shell/TODO-06 §5` -- user account creation used in §7 first-boot wizard

## Outcome

- `restore_create/list/delete/rollback()` kernel API; auto-create hooks in update/install pipelines.
- `rstrui.cpl` in Control Panel: calendar timeline, [Create], [Restore], [Delete].
- F8 recovery menu: text-mode (Normal/Safe/Shell/Restore/Factory/Repair); recovery shell with 15 commands.
- Factory reset: typed-"YES" confirmation; recreate default dir tree + first-boot flag.
- Startup repair: GPT CRC recompute, UEFI boot entry rewrite, kernel ELF SHA-256 verify, registry hive repair.
- First-boot wizard: timezone, region, keyboard, user account, wallpaper, update check, [Finish].
- `kevent_log()` structured event log: rolling 1 MiB file; `LOG_SECURITY=4`; Event Viewer applet.
- Crash report: on-boot prompt + formatted dump viewer (from TODO-16 base).
- Disk cleanup: `C:\Temp`, Recycle Bin, old restore points, cached packages; weekly scheduled.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                          | Depends On                                                                              | Status |
| --- | :---: | ---------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §8 Event log -- `kevent_log()`, `LOG_SECURITY=4`, rolling file, `CTRL_TABSTRIP` Event Viewer applet  | `klog_get_ring()` (exists); `vfs_write()`; `sched_task_add()` rotate task             |  [ ]   |
| 💎  |   2   | §1 Restore point creation -- `restore_create/list/delete()`, ZIP archive, Registry CRUD, max-evict   | `zip_create/add_file()` (TODO-04); `registry_backup()` (02-kernel-core/TODO-14-registry-completion.md); `vfs_mkdir()`  |  [ ]   |
| 💎  |   3   | §2 System rollback -- `restore_rollback()`, SHA-256 manifest verify, ZIP extract, hive restore       | §2; `cng_sha256()` (TODO-07); `zip_extract()` (TODO-04); `registry_restore()`         |  [ ]   |
| 💎  |   4   | §3 `rstrui.cpl` -- calendar timeline, [Create], [Restore], [Delete]                                  | §2 + §3; `CTRL_LISTVIEW`; `include/cpl.h` (TODO-11)                                   |  [ ]   |
| 💎  |   5   | §7 First-boot wizard -- 6-page OOBE; timezone/keyboard/user/wallpaper; `HKLM\SYSTEM\FirstBoot`       | `auth_create_user()` (TODO-06); `wallpaper_set()` (TODO-07); `update_check()` (TODO-03) |  [ ]   |
| 💎  |   6   | §9 Crash dump viewer -- on-boot `notify_send()` prompt + formatted dump viewer window                 | D02T27 crash dump file; `vfs_stat()`; `wm_create_window()`; `notify_send()` (TODO-09)|  [ ]   |
| ⭐  |   7   | §4 F8 recovery environment -- text-mode boot menu; 15-command recovery shell                         | `08-graphics-ui/TODO-03 §3` F8 intercept; VGA text mode or serial output              |  [ ]   |
| 💎  |   8   | §5 Factory reset -- typed-YES confirm; wipe user data + apps; recreate dir tree + FirstBoot flag     | §4 recovery env; `vfs_unlink/rmdir()`; `HKLM\SYSTEM\FirstBoot`                        |  [ ]   |
| ⭐  |   9   | §6 Startup repair -- GPT CRC, UEFI boot entry rewrite, kernel ELF SHA-256, registry hive repair      | `gpt_write_header()`; `uefi_runtime.h`; `cng_sha256()`; §4 recovery env              |  [ ]   |
| 💎  |  10   | §10 Disk cleanup -- scan Temp/Recycle/old-restore/cached-pkgs, space per category, weekly schedule   | §1 restore list; `trash_count/size()` (TODO-04); `sched_task_add()`                   |  [ ]   |

---

## 1. Event Log `[Sonnet]`

`struct kernel_event` (type INFO/WARN/ERROR/SECURITY, source[32], message[256], unix_ts). Add `LOG_SECURITY=4` to `klog.h`. `kevent_log()` appends to rolling `X:\Logs\events.log` (max 1 MiB, rotate on overflow). Event Viewer applet: `CTRL_TABSTRIP` filter by type/source, date range, export.

**Files:** `src/kernel/event_log.c` (new), `include/kernel/event_log.h` (new), `src/apps/control/applets/eventvwr.c` (new)

> [!NOTE]
> Extend `klog.h`: add `LOG_SECURITY = 4` constant. `kevent_log(type, source, msg)`: format as `"{unix_ts}|{type}|{source}|{msg}\n"` → append to VFS file `X:\Logs\events.log`. If file > 1 MiB: rotate: `vfs_rename("events.log", "events.1.log")`; optionally compress via `zip_create("events.1.log.zip")`. Log events at call sites: app install/uninstall → `kevent_log(LOG_INFO, "installer", "Installed %s %s")`. Login → `kevent_log(LOG_SECURITY, "auth", "Login: %s")`. Logout → `kevent_log(LOG_SECURITY, "auth", "Logout: %s")`. Permission denied → `kevent_log(LOG_SECURITY, "vfs", "Access denied: %s by uid %u")`. Crash/panic → `kevent_log(LOG_ERROR, "kernel", "Panic: %s at %p")`. Service start/stop → `kevent_log(LOG_INFO, "service", "%s started/stopped")`. **Rotate task**: `sched_task_add("event_rotate", event_rotate_task, 3600, 1)` (hourly check). **Event Viewer**: `eventvwr.cpl` `CTRL_TABSTRIP` tabs: All Events / Errors / Security / Info. `CTRL_LISTVIEW` (Time, Type, Source, Message). [Filter by date]: `dialog_input()` date range → filter in-memory. [Export]: `dialog_file_save("*.log")` → `vfs_write()` raw log lines. `CTRL_TEXTBOX` source filter.

- [ ] `include/kernel/event_log.h`: `struct kernel_event { log_level_t type; char source[32]; char msg[256]; uint64_t ts; }`, `kevent_log()` prototype
- [ ] Add `LOG_SECURITY = 4` to `klog.h` enum; update klog level string table
- [ ] `src/kernel/event_log.c`: `kevent_log()` -- format + VFS append; 1 MiB size check + rotate
- [ ] `sched_task_add("event_rotate", event_rotate_check, 3600, 1)` in kernel init
- [ ] Wire `kevent_log()` at: auth login/logout; UAC elevation; permission denied (VFS); service start/stop; app install/uninstall; kernel panic
- [ ] `eventvwr.cpl`: `CTRL_TABSTRIP` (All/Errors/Security/Info); `CTRL_LISTVIEW` Time+Type+Source+Message; source filter textbox; date range filter; [Export] button
- [ ] Commit: `"kernel: event log -- kevent_log, LOG_SECURITY, rolling file, rotate; eventvwr.cpl viewer"`

## 2. Restore Point Creation `[Sonnet]`

`restore_create(description)`: ZIP system files + registry backup → `C:\Impossible\System\Restore\{unix_ts}\`. `restore_list()`, `restore_delete(ts)`. Auto-evict oldest when > `HKLM\SYSTEM\Restore\MaxPoints` (default 10).

**Files:** `src/kernel/restore.c` (new), `include/kernel/restore.h` (new)

> [!NOTE]
> Restore point directory: `C:\Impossible\System\Restore\{unix_ts}\` containing: `manifest.ini` (description, timestamp, OS version, list of backed-up files), `registry_backup\` (hive dumps via `registry_backup(path)`), `system_files.zip` (ZIP of `C:\Impossible\System\*.exe`, `*.dll`, `*.sys`, `drivers\*` -- skip logs and restore dirs to avoid recursion). `restore_create(const char *desc)`: (1) `vfs_mkdir("C:\\Impossible\\System\\Restore\\{ts}")` (2) `registry_backup("C:\\Impossible\\System\\Restore\\{ts}\\registry_backup\\")` (3) `zip_create("system_files.zip")` → add each file matching the pattern list → `zip_close()` (4) write `manifest.ini` with `Description`, `Timestamp`, `FileCount`, `SHA256` of ZIP. **Max points**: read `HKLM\SYSTEM\Restore\MaxPoints` (default 10); call `restore_list()` → if `count > max_points`: `restore_delete(oldest_ts)`. `restore_list(restore_point_t *out, int max)`: scan `C:\Impossible\System\Restore\*` dirs; parse each `manifest.ini` → fill array; sort by timestamp. `restore_delete(uint64_t ts)`: `vfs_unlink_tree("C:\\Impossible\\System\\Restore\\{ts}")`. Log: `kevent_log(LOG_INFO, "restore", "Created restore point: %s", desc)`.

- [ ] `include/kernel/restore.h`: `struct restore_point_t { uint64_t ts; char desc[128]; uint64_t file_count; char sha256[65]; }`, `restore_create/list/delete/rollback()` prototypes
- [ ] `restore_create(desc)` -- mkdir; `registry_backup()`; `zip_create()` + add system files; `manifest.ini` write
- [ ] File inclusion list: `C:\Impossible\System\*.exe`, `System32\*.exe`, `System\Drivers\*`, skip logs + restore subdirs
- [ ] `restore_list(out, max)` -- VFS readdir `Restore\*` + parse `manifest.ini`; sort ascending by ts
- [ ] `restore_delete(ts)` -- `vfs_unlink_tree()` of ts directory
- [ ] Max-evict: after each `restore_create()`, if `list_count > MaxPoints`: delete oldest
- [ ] `kevent_log(LOG_INFO, "restore", ...)` on create + delete
- [ ] Commit: `"kernel: restore_create/list/delete -- ZIP system files, registry backup, max-points eviction"`

## 3. System Rollback `[Sonnet]`

`restore_rollback(ts)`: SHA-256 manifest integrity check, confirmation dialog, ZIP extract back to `C:\Impossible\System\`, registry hive restore from `registry_backup\`, bump "LastRestore" in Registry, restart.

**Files:** extend `src/kernel/restore.c`

> [!NOTE]
> `restore_rollback(uint64_t ts)`: (1) `restore_list()` → find entry → open `manifest.ini` → read stored SHA-256; (2) `cng_sha256("system_files.zip")` → compare with `crypto_verify32()` -- abort with `klog_err()` if mismatch ("restore point corrupt"); (3) `dialog_confirm("Restore to {desc} ({date})? All changes since then will be lost. The system will restart.", YES/NO)` → if NO: return; (4) `zip_extract("system_files.zip", "C:\\Impossible\\System\\")` -- overwrite in-place (files opened by running system may fail → log but continue); (5) `registry_restore("C:\\Impossible\\System\\Restore\\{ts}\\registry_backup\\")` -- copy hive files, mark dirty, reload at next boot; (6) `registry_set("HKLM\\SYSTEM\\Restore\\LastRestore", ts_str)` + `kevent_log(LOG_INFO, "restore", "Rollback to %llu (%s)", ts, desc)`; (7) display "Restarting to complete restore..." overlay 3 s → `EFI_ResetSystem(RESET_WARM)`.

- [ ] `restore_rollback(uint64_t ts)` -- find entry; `cng_sha256()` integrity verify; `dialog_confirm()`
- [ ] On hash mismatch: `klog_err()` + `kevent_log(LOG_ERROR, "restore", "corrupt restore point: %llu", ts)` + return error
- [ ] `zip_extract("system_files.zip", "C:\\Impossible\\System\\")` -- file-by-file; log VFS errors but continue
- [ ] `registry_restore()` -- copy hive files to active location; mark pending reload
- [ ] `registry_set("HKLM\\SYSTEM\\Restore\\LastRestore", ...)` + `kevent_log()`
- [ ] 3-s restart overlay → `EFI_ResetSystem(RESET_WARM)`
- [ ] Commit: `"kernel: restore_rollback -- SHA-256 verify, zip extract, registry restore, restart"`

## 4. `rstrui.cpl` -- System Restore UI `[Sonnet]`

Control Panel System Restore applet: calendar timeline of restore points (description + created-at + estimated size). [Create] button with description input. [Restore] button with confirmation. [Delete] button.

**Files:** `src/apps/control/applets/rstrui.c` (new)

> [!NOTE]
> `CPlApplet()` inner surface (480×380 px). **Top**: title "System Restore" + subtitle "Undo system changes by reverting to a saved restore point." **Restore point list**: `CTRL_LISTVIEW` 3 columns (Description 220 / Created 140 / Size 80); populated via `restore_list()`; size = `vfs_stat("system_files.zip").size / 1024` in KB. **[Create]**: `dialog_input("Restore point name:", "Manual restore point")` → `restore_create(desc)` → refresh list. **[Restore]**: selected row → `restore_rollback(ts)` (includes confirmation + restart). **[Delete]**: `dialog_confirm("Delete this restore point? This cannot be undone.", YES/NO)` → `restore_delete(ts)` → refresh. **Info bar**: "Last restore: {date}" from `HKLM\SYSTEM\Restore\LastRestore`; "Space used: {N} MB" (sum of all restore point ZIP sizes). [Configure] stub: `dialog_input("Max restore points:", "10")` → `registry_set("HKLM\\SYSTEM\\Restore\\MaxPoints", ...)`.

- [ ] `rstrui.c` implementing `CPlApplet()` messages
- [ ] `CTRL_LISTVIEW` populated from `restore_list()`; refresh on create/delete
- [ ] [Create] → `dialog_input()` → `restore_create()` → list refresh
- [ ] [Restore] → `restore_rollback(ts)` (includes confirmation + restart flow)
- [ ] [Delete] → `dialog_confirm()` → `restore_delete(ts)` → list refresh
- [ ] Info bar: last restore date + total space used
- [ ] [Configure] → `dialog_input()` max points → `registry_set()`
- [ ] Commit: `"rstrui.cpl: system restore UI -- CTRL_LISTVIEW timeline, create/restore/delete buttons"`

## 5. First-Boot Setup Wizard `[Sonnet]`

Runs when `HKLM\SYSTEM\FirstBoot=1`. 6 pages: Welcome, Timezone/Region, Keyboard, Create User Account, Wallpaper, Update Check. Clears flag on [Finish] → boots to desktop.

**Files:** `src/apps/oobe/oobe.c` (new), `include/apps/oobe.h` (new)

> [!NOTE]
> **Trigger**: in kernel init (after full desktop init): `registry_get("HKLM\\SYSTEM\\FirstBoot")` == "1" → `oobe_start()`. Window: full-screen (1024×768 base), non-resizable, z_order=29000 (above taskbar). Branding image top-left. **Page 1 -- Welcome**: "Welcome to Impossible OS" large heading; "Let's get started." subtitle; [Next]. **Page 2 -- Timezone/Region**: `CTRL_DROPDOWN` timezone (UTC offsets, TZ names); `CTRL_DROPDOWN` region (locale formats); writes `HKLM\SYSTEM\TimeZone` + `HKLM\SYSTEM\Region`. **Page 3 -- Keyboard**: `CTRL_DROPDOWN` keyboard layout; test textbox below; writes `HKLM\SYSTEM\KeyboardLayout`. **Page 4 -- Create User**: username textbox; password + confirm password (masked); `auth_create_user(username, password, PRIV_ADMIN)` (TODO-06); writes `HKLM\SYSTEM\DefaultUser`. **Page 5 -- Wallpaper**: 4-6 wallpaper thumbnails (scan `C:\Impossible\Web\Wallpaper\`); click selects; `wallpaper_set(path)` preview. **Page 6 -- Updates**: "Check for updates automatically?" `CTRL_CHECKBOX` (default on); `update_check()` background if checked; [Finish]. On finish: `registry_set("HKLM\\SYSTEM\\FirstBoot", "0")`; `kevent_log(LOG_INFO, "oobe", "First-boot setup completed: user=%s", username)`.

- [ ] `void oobe_start(void)` -- check `HKLM\SYSTEM\FirstBoot`; create full-screen wizard window
- [ ] Page navigation: [Next]/[Back] buttons; page index 0–5; render page by index
- [ ] Page 2: `CTRL_DROPDOWN` timezone (30+ TZ entries) + region → Registry
- [ ] Page 3: keyboard layout dropdown + `CTRL_TEXTBOX` test area
- [ ] Page 4: `CTRL_TEXTBOX` username + password + confirm; `auth_create_user()` on [Next]; show error if mismatch
- [ ] Page 5: wallpaper thumbnails from `vfs_readdir("C:\\Impossible\\Web\\Wallpaper\\")`; `image_scale()` for 120×80 px previews; click → `wallpaper_set()`
- [ ] Page 6: auto-update checkbox; background `update_check()` task; [Finish]
- [ ] Finish: `registry_set("HKLM\\SYSTEM\\FirstBoot", "0")`; `kevent_log()`; close OOBE; show desktop
- [ ] Commit: `"oobe: first-boot setup wizard -- timezone, keyboard, user create, wallpaper, update check"`

## 6. Crash Dump Viewer `[Sonnet]`

On next boot after crash (dump file present in `CrashDumps\`): `notify_send()` prompt "System shut down unexpectedly. [View Report] [Dismiss]". Crash report viewer window: formatted dump (registers, stack, loaded drivers, PMM stats, last serial lines).

**Files:** `src/apps/crashview/crashview.c` (new)

> [!NOTE]
> -> XREF: `02-kernel-core/TODO-27` -- crash dump generation (raw `.dmp` format, registers + stack + PMM stats + serial buffer); §6 here only adds the on-boot trigger + viewer UI. **On-boot check** (in kernel init, before desktop starts): `vfs_readdir("X:\\Crash\\")` (BlackBox, -> XREF: TODO-17 §8) -> if any `.dmp` newer than `HKLM\SYSTEM\LastBoot`: `notify_send("Unexpected Shutdown", ...)`. **[View] opens crash viewer**: window 700x520 px; `CTRL_TABSTRIP` tabs: Summary / Registers / Stack Trace / Drivers / Memory. On [Dismiss]: move `.dmp` to `X:\Crash\Archived\` (keep for 30 days).

- [ ] On-boot check: `vfs_readdir("X:\\Crash\\")` -> find unread `.dmp` files (-> XREF: TODO-17 §8)
- [ ] "Unread" detection: `.dmp` mtime > `HKLM\SYSTEM\LastBoot` value; update `LastBoot` on each clean boot
- [ ] `notify_send("Unexpected Shutdown", ..., NOTIFY_PERSISTENT)` with [View] + [Dismiss] action buttons
- [ ] `crashview_open(const char *dmp_path)` -- `wm_create_window()`; `CTRL_TABSTRIP` 5 tabs
- [ ] Summary tab: crash reason + timestamp + uptime; parse from dump header (TODO-16 format)
- [ ] Registers tab: 2-column hex layout; `RAX`–`R15`, `RIP`, `RFLAGS`, `CR0/CR2/CR3`
- [ ] Drivers tab: `CTRL_LISTVIEW` driver name + load address from dump
- [ ] Memory tab: PMM stats (total/free/used pages) from dump
- [ ] [Export] → `dialog_file_save("*.txt")` → formatted plain-text dump
- [ ] [Dismiss] → `vfs_rename(dmp, "Archived\\{dmp}")` + update seen flag in Registry
- [ ] Commit: `"crashview: on-boot crash prompt + report viewer -- CTRL_TABSTRIP, registers, stack, drivers, export"`

## 7. F8 Recovery Environment `[Opus]`

Text-mode boot menu (intercept F8 before scheduler): Normal / Safe / Recovery Shell / System Restore / Factory Reset / Startup Repair. Recovery Shell: 15 commands (ls/cd/cat/cp/mv/rm/pwd/fsck/fdisk/reg-reset/reg-query/reg-set/backup/restore/reboot).

**Files:** `src/kernel/recovery.c` (new), `include/kernel/recovery.h` (new)

> [!NOTE]
> → XREF: `08-graphics-ui/TODO-03 §3` -- F8 keyboard polling at early boot (before APIC/scheduler up); that section wires the key detection; §8 here implements the menu + shell loop. **Text mode**: use direct VGA text mode (`0xB8000`) or serial output (`COM1`) for recovery UI -- no framebuffer dependency, no WM. Menu rendered via `vga_puts_at(row, col, str, attr)` or equivalent. **Recovery shell**: small read-eval loop: `recovery_readline(buf, 256)` (PS/2 poll via `inb(0x60)` + PS/2 scancode decode); parse first token → dispatch table. **Commands**: `ls [path]` → `vfs_readdir()`; `cd path` → update cwd; `cat path` → `vfs_open/read/close()`; `cp src dst`; `mv src dst`; `rm path`; `pwd`; `fsck [drive:]` → `ixfs_fsck()` (TODO-fs); `fdisk` → print GPT partition table via `gpt_read()`; `reg-reset` → overwrite `SYSTEM` hive with `SYSTEM.bak` fallback; `reg-query key` → `registry_get()` + print; `reg-set key val` → `registry_set()`; `backup src dst` → `vfs_copy()` (chunked via kmalloc 4 KiB); `restore ts` → `restore_rollback(ts)`; `reboot` → `EFI_ResetSystem(RESET_COLD)`. **Safe mode** option: set `HKLM\SYSTEM\Boot\SafeMode=1` → `EFI_ResetSystem(RESET_WARM)` → kernel init reads flag → skip non-critical drivers.

- [ ] `void recovery_menu_show(void)` -- VGA text mode menu render; read key; dispatch to option
- [ ] **Normal Boot**: return immediately; kernel continues normal init
- [ ] **Safe Mode**: `registry_set("HKLM\\SYSTEM\\Boot\\SafeMode", "1")` + `EFI_ResetSystem(RESET_WARM)`
- [ ] **Recovery Shell**: `recovery_shell_loop()` -- `recovery_readline()` PS/2 poll + scancode → ASCII; parse + dispatch
- [ ] Implement all 15 shell commands with VGA/serial output
- [ ] `reg-reset`: `vfs_rename("SYSTEM", "SYSTEM.corrupt")` + `vfs_rename("SYSTEM.bak", "SYSTEM")`
- [ ] **System Restore**: call `restore_list()` → display numbered list → select → `restore_rollback(ts)`
- [ ] **Factory Reset**: route to §8
- [ ] **Startup Repair**: route to §9
- [ ] Commit: `"kernel: recovery environment -- F8 text-mode menu, recovery shell 15 cmds, safe mode boot"`

## 8. Factory Reset `[Sonnet]`

Wipe all user data + app installs, restore system files from recovery partition or embedded fallback, recreate default directory tree + set `HKLM\SYSTEM\FirstBoot=1`. Typed "YES" confirmation required.

**Files:** extend `src/kernel/recovery.c`

> [!NOTE]
> Called from recovery menu §7 or from Settings (future). **Typed confirmation**: display "WARNING: This will erase ALL user data, applications, and settings. Type YES to confirm: " → `recovery_readline()` → if `strcmp(input, "YES") != 0`: print "Cancelled." → return. **Steps**: (1) `vfs_unlink_tree("C:\\Users\\")` (all user home dirs); (2) `vfs_unlink_tree("C:\\Program Files\\")` (installed apps); (3) delete all `HKLM\SOFTWARE\*` app keys (scan + delete); (4) `vfs_unlink_tree("C:\\Impossible\\System\\Restore\\")` (clear restore points); (5) restore system files: if recovery partition mounted (scan GPT for recovery partition type `DE94BBA4...`): `zip_extract(recovery_partition_files)` else use `C:\Impossible\System\recovery.zip` (embedded fallback built at install time); (6) `registry_factory_reset()` -- overwrite `SYSTEM` + `SOFTWARE` hives with factory defaults (from `recovery.zip`); (7) recreate dir tree: `C:\Users\Default\*`, `C:\Program Files\`, `C:\Temp\`, `C:\Recycle\`; (8) `registry_set("HKLM\\SYSTEM\\FirstBoot", "1")`; (9) `kevent_log(LOG_WARN, "recovery", "Factory reset performed")`; (10) `EFI_ResetSystem(RESET_WARM)`.

- [ ] `void factory_reset(void)` -- typed "YES" confirmation loop
- [ ] `vfs_unlink_tree(path)` helper: recursively delete a directory + all contents
- [ ] Delete user dirs, Program Files, restore points, user registry keys
- [ ] System file restore: probe recovery partition → `zip_extract()`; fallback to `recovery.zip`
- [ ] `registry_factory_reset()` -- overwrite hives with factory defaults from recovery archive
- [ ] Recreate default dir tree: `C:\Users\Default\{Desktop,Documents,Downloads,Pictures}`, `C:\Program Files\`, `C:\Temp\`
- [ ] `registry_set("HKLM\\SYSTEM\\FirstBoot", "1")` + `kevent_log()` + reboot
- [ ] Commit: `"kernel: factory_reset -- typed-YES guard, user data wipe, hive restore, dir tree rebuild"`

## 9. Startup Repair `[Sonnet]`

Rewrite UEFI boot entry, recompute GPT header CRCs, verify kernel ELF SHA-256 vs manifest, repair `HKLM.default` hive if corrupt. Called from recovery menu §7.

**Files:** extend `src/kernel/recovery.c`

> [!NOTE]
> `void startup_repair(void)` prints progress to VGA/serial as each step completes. **Step 1 -- UEFI boot entry**: `EFI_SetVariable(L"BootXXXX", ...)` with correct `\EFI\BOOT\BOOTX64.EFI` device path; rewrite `BootOrder` to place our entry first. Print "UEFI boot entry: OK/FIXED". **Step 2 -- GPT CRCs**: `gpt_read_raw()` → `gpt_recompute_crcs()` → `gpt_write_header()` → `gpt_write_backup_header()`. Print "GPT partition table: OK/FIXED". **Step 3 -- kernel ELF**: read `C:\Impossible\System\kernel.exe` (or from EFI partition `\boot\kernel.exe`); `cng_sha256(data, size, hash)` → compare with `HKLM\SYSTEM\KernelSHA256` (written at update-apply time); if mismatch → restore from recovery partition. Print "Kernel ELF: OK/CORRUPT (restoring)". **Step 4 -- Registry**: try `registry_open("HKLM")` → if fails: `vfs_rename("SOFTWARE", "SOFTWARE.corrupt")` + `vfs_rename("SOFTWARE.bak", "SOFTWARE")` (same for `SYSTEM`). Print "Registry: OK/REPAIRED". Print "Startup repair complete. Press any key to restart." → `EFI_ResetSystem(RESET_WARM)`.

- [ ] `void startup_repair(void)` -- 4-step sequential repair with per-step status output
- [ ] Step 1: `EFI_SetVariable()` UEFI boot entry rewrite; `BootOrder` fixup
- [ ] Step 2: `gpt_read_raw()` → `gpt_recompute_crcs()` → `gpt_write_header()` + backup header
- [ ] Step 3: `cng_sha256(kernel_data, size)` → compare `HKLM\SYSTEM\KernelSHA256`; restore from recovery if mismatch
- [ ] Step 4: `registry_open()` smoke-test; on failure: rename `.corrupt` + rename `.bak` → active
- [ ] Print status per step: `recovery_puts("Kernel ELF: CORRUPT -- restoring from backup...")`
- [ ] Wait for keypress → `EFI_ResetSystem(RESET_WARM)`
- [ ] Commit: `"kernel: startup_repair -- UEFI boot entry, GPT CRC, kernel SHA-256, registry hive repair"`

## 10. Disk Cleanup Utility `[Sonnet]`

`src/apps/cleanup/cleanup.c`: scan `C:\Temp\`, Recycle Bin, old restore points (keep last 3), cached update packages, app crash dumps. Display space savings per category. [Clean up system files] deletes selected. Weekly scheduled task.

**Files:** `src/apps/cleanup/cleanup.c` (new), `include/apps/cleanup.h` (new)

> [!NOTE]
> Window: 480×400 px. **Categories** (checkbox + label + size):
> - **Temp files** (`C:\Temp\*`): `vfs_readdir()` sum sizes
> - **Recycle Bin** (`C:\Recycle\*`): `trash_size()` (TODO-04)
> - **Old restore points** (keep 3 newest): `restore_list()` → sum sizes of entries beyond 3
> - **Cached update packages** (`C:\Temp\*.ipkg`): `vfs_readdir()` filter by `.ipkg`
> - **Crash dumps** (`X:\Crash\Archived\*`): sum sizes (-> XREF: TODO-17 §8)
> - **App logs** (`X:\Logs\events.*.log`): sum rotated log files (not current)
> Total space at bottom: "Total: {N} MB will be freed". [Clean up system files] button: for each checked category → delete files; show `CTRL_PROGRESSBAR` during deletion; refresh sizes. **Weekly task**: `sched_task_add("disk_cleanup", disk_cleanup_auto, 7*86400, 1)` -- runs silently, clears Temp + Archived CrashDumps + old logs (> 7 days); `kevent_log(LOG_INFO, "cleanup", "Auto disk cleanup: freed %llu KB")`.

- [ ] `void cleanup_open(void)` -- `wm_create_window()`; scan all categories; display checkboxes + sizes
- [ ] Category scan: `cleanup_scan_temp()`, `cleanup_scan_recycle()`, `cleanup_scan_restore(keep_n=3)`, `cleanup_scan_ipkg()`, `cleanup_scan_crashdumps()`, `cleanup_scan_logs()`
- [ ] Each returns `uint64_t bytes` -- sum for "Total" display
- [ ] [Clean up system files] button: for each checked category → `cleanup_delete_category(cat)` → `CTRL_PROGRESSBAR`
- [ ] `cleanup_delete_category(CLEANUP_TEMP)` → `vfs_unlink_tree("C:\\Temp\\")` + `vfs_mkdir("C:\\Temp\\")`
- [ ] `cleanup_delete_category(CLEANUP_RECYCLE)` → `trash_empty()` (TODO-04)
- [ ] `cleanup_delete_category(CLEANUP_RESTORE)` → `restore_list()` → delete all but 3 newest
- [ ] Weekly `sched_task_add("disk_cleanup_auto", ...)` in kernel init
- [ ] Commit: `"cleanup: disk cleanup utility -- Temp/Recycle/restore/pkg/crashdumps categories, weekly task"`

---

## OS Comparison


| ⭐  | Feature                                   | 🪟 Win11                                        | 🐧 Linux                                                       | 🚀 Impossible OS                                                             |
| --- | ----------------------------------------- | ----------------------------------------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| 💎  | Event log -- structured persistent events | ✅ Windows Event Log: XML structured;           | ✅ systemd journal (`journalctl`); syslog; `/var/log/*`;       | ⬜ §1 -- `kevent_log()` + `LOG_SECURITY=4`; rolling 1                        |
| 💎  | Restore points                            | ✅ System Restore: VSS shadow copies;           | ⚠️ No built-in; Btrfs/ZFS snapshots; `snapper`;                | ⬜ §2 -- `⭐` explicit ZIP + registry                                        |
| 💎  | System rollback                           | ✅ System Restore: VSS revert; registry         | ⚠️ No built-in; filesystem snapshots only;                     | ⬜ §3 -- `cng_sha256()` manifest check before any                            |
| 💎  | Recovery environment                      | ✅ WinRE: graphical; Startup Repair; Command    | ✅ Recovery mode; GRUB single-user; `init=/bin/bash`;          | ⬜ §7 -- `[Opus]` text-mode F8 menu; 15-command                              |
| 💎  | Factory reset                             | ✅ Reset this PC: Keep/Remove files;            | ✅ Reinstall distro; `reinstall-os` on some;                   | ⬜ §8 -- typed-"YES" guard; GPT recovery partition                           |
| ⭐  | Startup repair                            | ✅ WinRE Startup Repair: BCD fixup;             | ⚠️ `grub-install`; `fsck`; manual; no unified                  | ⬜ §9 -- `⭐` kernel ELF SHA-256 verify                                      |
| 💎  | First-boot OOBE wizard                    | ✅ OOBE: account (MSA/local), region, keyboard, | ✅ Most distros: Anaconda/Ubiquity/Calamares first-boot wizard | ⬜ §5 -- `HKLM\SYSTEM\FirstBoot=1` flag; 6-page wizard; `auth_create_user()` |
| 💎  | Crash dump viewer                         | ✅ Windows Error Reporting; minidump viewer     | ✅ `apport`; `kdump`/`kexec`; `abrt`; crash files              | ⬜ §6 -- on-boot `notify_send()` + `CTRL_TABSTRIP` viewer                    |
| 💎  | Disk cleanup                              | ✅ Disk Cleanup + Storage Sense:                | ✅ `bleachbit`; `journalctl --vacuum`; `apt clean`;            | ⬜ §10 -- 6 categories with size preview                                     |

> **After §1–§10:** Impossible OS has the full OS safety net. The `⭐` advantages: restore points are stored as human-inspectable ZIP archives + INI manifests (no VSS opaque shadow copy format); startup repair automatically SHA-256 verifies the kernel ELF and restores from recovery partition on mismatch (no equivalent in WinRE); the recovery shell runs from VGA direct output with zero framebuffer/WM dependencies, making it available even when the compositor is corrupt.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `restore_create("test")` → `C:\Impossible\System\Restore\{ts}\` exists with `manifest.ini` + `system_files.zip` + `registry_backup\`
- [ ] `restore_list()` returns the created entry; `restore_delete(ts)` removes it
- [ ] `kevent_log(LOG_SECURITY, "auth", "Login: testuser")` → line appears in `events.log` on VFS
- [ ] `eventvwr.cpl` opens → Security tab shows the login event with correct timestamp
- [ ] F8 at boot → text-mode menu appears; select "Recovery Shell" → `ls C:\` prints directory; `pwd` returns current path
- [ ] First-boot (`HKLM\SYSTEM\FirstBoot=1`): boot → OOBE wizard appears; complete all pages; reboot → OOBE does not appear again
- [ ] Crash dump present in `CrashDumps\`: on boot → `notify_send()` prompt shown; [View] → crash viewer window with register tab
- [ ] Disk cleanup: open → all 6 categories show sizes; check Temp + Recycle → [Clean] → `C:\Temp\` emptied; Recycle Bin empty
- [ ] `restore_rollback(ts)` called (from rstrui.cpl): SHA-256 verify passes; confirmation dialog shown; on Yes → 3-s overlay + reboot
- [ ] Commit: `"recovery: restore points, rollback, OOBE wizard, event log, crash viewer, F8 recovery, cleanup -- all complete"`
