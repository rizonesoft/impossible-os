---
schema_version: 1
id: service-manager
domain: 09-desktop-shell
status: active
title: "TODO-03 -- Service Manager & Core Daemons"
---

# TODO-03 -- Service Manager & Core Daemons

> **Goal:** Build the kernel service lifecycle manager -- a 32-slot static service table with Registry-backed definitions, start/stop/restart/status operations, PIT-ticked crash monitor with exponential backoff, critical-service BSOD gate, built-in daemons (netd/ntpd/registryd/indexd), a kernel notification queue, autostart program scanning, Win32 system-info stubs, and a minimal user account stub.

> [!IMPORTANT]
> **Already exists**: `task_create(entry, name)`, `task_create_user(entry, name)`, `task_exec(data, size)`, `task_fork()`, `task_waitpid(pid)` in `task.h`. `signal_send(pid, SIGTERM/SIGKILL)` in `signal.h`. `smp_cpu_count()` in `smp.h` + `acpi_get_cpu_count()` in `acpi.h`. `RegOpenKeyEx/RegSetValueEx/RegGetValue` for `HKLM\SYSTEM\Services` + per-user Registry. `vfs_readdir/vfs_finddir/vfs_mkdir/vfs_create` for home dir + startup scan. `shortcut_execute()` (TODO-02 §5) for autostart `.lnk` files. `notify_send()` (TODO-09 §6) for service failure toasts. `system_get_ticks()` for PIT-ticked monitor. **Missing**: all service manager infrastructure, `knotify_send`, `GetSystemInfo`, user account struct. Complete sections in order: user account stub → service manager core → auto-restart monitor → graceful degradation → `sc` shell command → built-in services → notification service → autostart programs → Win32 system info stubs.

## Inputs

- `include/kernel/sched/task.h` -- `task_create()`, `task_exec()`, `task_waitpid()` -- used by §1 `svc_start()` to fork/exec service processes
- `include/kernel/ipc/signal.h` -- `signal_send(pid, SIGTERM/SIGKILL)` -- used by §1 `svc_stop()` sequence
- `include/kernel/drivers/pit.h` -- `system_get_ticks()`, `PIT_TARGET_FREQ` -- used by §3 `svc_monitor_tick()` timing and backoff
- `include/registry.h` -- `RegGetValue/RegSetValueEx/RegOpenKeyEx` -- used by §1 service Registry definitions, §9 user Registry hive, §7 RunOnce/Run
- `include/kernel/fs/vfs.h` -- `vfs_readdir`, `vfs_mkdir`, `vfs_create` -- used by §9 home dir creation, §7 startup scan
- `include/kernel/smp.h` -- `smp_cpu_count()` -- used by §8 `GetSystemInfo.dwNumberOfProcessors`
- `include/desktop/notify.h` (TODO-09 §6) -- `notify_send()` -- used by §4 service failure toast and §7 `knotify_send()` drain
- `include/desktop/shortcut.h` (TODO-02 §5) -- `shortcut_execute()` -- used by §7 autostart `.lnk` execution
- `include/kernel/klog.h` -- `klog()` -- used throughout for `[svc]` serial log lines
- → XREF: `02-kernel-core/TODO-02-kernel-configuration-policy.md §5, §10` -- Safe Mode and boot acceptance policy decide which services and autostarts may run
- → XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §6` -- `knotify_send()` (§7) is drained by the toast queue; `notify_send()` is the user-facing path; §7 bridges the two
- Related (no stable XREF target): `07-networking/TODO-01-*` (networking) -- `netd` (§2) wraps the existing DHCP/ARP polling loop; must co-exist with existing `net_init()` call in `kernel_main`
- → XREF: `08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md` -- §4 Win32 system info stubs go into the `kernel32.dll` stub table alongside GDI/USER32

## Outcome

- `svc_start/stop/restart/status/list` manage 32 registered services with Registry-backed definitions.
- PIT-ticked `svc_monitor_tick()` detects crashes; exponential backoff restart (1→60 s cap); 5 failures → FAILED.
- Critical services: unrecoverable failure → BSOD via `kernel_panic()`; normal services toast + continue.
- Built-in daemons `netd/ntpd/registryd/indexd` auto-start after filesystem mount; dependency order enforced.
- `sc list/start/stop/restart/status` shell commands.
- `knotify_send(source, msg, severity)` kernel queue; compositor drains per frame → toasts.
- Autostart: `C:\Users\Default\AppData\Startup\` `.lnk` scan + `HKLM\SYSTEM\Boot\Run[Once]`.
- `GetSystemInfo/GetVersionExA/GetComputerNameA/GetTempPathA/GetSystemDirectoryA/GetUserNameA` in `kernel32` stubs.
- Default "Default" user with per-user Registry hive and home directory tree.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                    | Depends On                                                                       | Status |
| --- | :---: | ---------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §9 User account stub -- `user_account_t`, default user, home dirs, `HKU\{user}` hive           | `vfs_mkdir`, `RegSetValueEx` (both exist)                                       |  [ ]   |
| ⭐  |   2   | §1 Service manager core -- `struct service`, 32-slot table, `svc_start/stop/restart/status`   | §9 user (service Registry paths use HKLM); `signal_send`, `task_exec` (exist)  |  [ ]   |
| ⭐  |   3   | §3 Auto-restart & crash recovery -- `svc_monitor_tick()`, exponential backoff, FAILED state    | §1 service table must exist before monitor can iterate it                       |  [ ]   |
| 💎  |   4   | §4 Graceful degradation -- `critical` BSOD gate, `sc list` state display                      | §3 monitor (FAILED state set by monitor; degradation acts on it)                |  [ ]   |
| 💎  |   5   | §5 `sc` shell command -- `sc list/start/stop/restart/status`                                   | §4 (displays all states including FAILED)                                       |  [ ]   |
| 💎  |   6   | §2 Built-in services -- `netd/ntpd/registryd/indexd` definitions, dependency order            | §5 `sc` stable (so services are observable); networking + NTP exist             |  [ ]   |
| 💎  |   7   | §6 Notification service -- `kernel_notification` queue, `knotify_send()`, compositor drain    | §6 built-in services use `knotify_send()` for network up/down events            |  [ ]   |
| 💎  |   8   | §7 Autostart programs -- Startup `.lnk` scan, `Run`/`RunOnce` Registry execution              | §7 `knotify_send()` (autostart failures notify via knotify); §9 user home path  |  [ ]   |
| 💎  |   9   | §8 Win32 system info stubs -- `GetSystemInfo/GetVersionExA/GetComputerNameA/GetTempPathA/...`  | §9 user (GetUserNameA reads current user name); `kernel32` stub table (TODO-11) |  [ ]   |

---

## 1. User Account Stub `[Sonnet]`

Minimal `struct user_account` (uid, username, home_dir, privilege_level). Create default "Default" user on first boot. Per-user `HKU\{username}\` Registry hive. Home directories: `C:\Users\{username}\{Desktop,Documents,Downloads,Pictures,AppData}`. `%USERPROFILE%` env var. Current user tracked in kernel state.

**Files:** `src/kernel/user.c` (new), `include/kernel/user.h` (new)

> [!NOTE]
> Single-user stub only (full multi-user → future TODO-06 user management). `struct user_account`: `uid=1000`, `username="Default"`, `home_dir="C:\\Users\\Default"`, `privilege_level=1` (admin). `g_current_user` global pointer. `user_init()`: check `HKLM\SYSTEM\FirstBoot\UserInit` DWORD; if absent: call `user_create_default()`; set flag. `user_create_default()`: create `C:\Users\Default\` + subdirs via `vfs_mkdir()`; create `HKU\Default` Registry key via `RegCreateKeyEx(HKEY_USERS, "Default", ...)`; create `HKCU` redirect (HKCU = `HKU\Default`). `%USERPROFILE%` env var: stored in `g_env_userprofile[MAX_PATH]`; accessible via `env_get("USERPROFILE")`.

- [ ] `typedef struct { uint32_t uid; char username[32]; char home_dir[256]; uint8_t privilege_level; } user_account_t;` in `user.h`
- [ ] `extern user_account_t *g_current_user;` -- single global (multi-user: future)
- [ ] `void user_init(void)` -- `FirstBoot\UserInit` check; call `user_create_default()` if needed; set `g_current_user`
- [ ] `void user_create_default(void)` -- `vfs_mkdir("C:\\Users\\Default\\")` + 5 subdirs; `RegCreateKeyEx(HKEY_USERS, "Default", ...)`; set `HKCU` redirect
- [ ] `const char *user_get_home(void)` → `g_current_user->home_dir`
- [ ] `const char *user_get_name(void)` → `g_current_user->username`
- [ ] `char g_env_userprofile[MAX_PATH]` -- set in `user_init()`; queried by `GetTempPathA` and `GetUserNameA`
- [ ] `user_init()` called from `kernel_main()` after Registry and VFS are live
- [ ] Commit: `"user: account stub -- default user, home dirs, HKU\\Default Registry hive, USERPROFILE env"`

## 2. Service Manager Core `[Opus]`

`struct service` (name[32], exe_path[256], state: STOPPED/STARTING/RUNNING/FAILED, pid, auto_start, critical, restart_count, last_exit_time). Static table of 32 services. `svc_start/stop/restart/status/list`. Service definitions in `HKLM\SYSTEM\Services\{name}`.

**Files:** `src/kernel/svc.c` (new), `include/kernel/svc.h` (new)

> [!NOTE]
> This is `[Opus]` -- the service manager introduces a novel per-service lifecycle state machine that has no precedent in Impossible OS. Key architectural decisions: (1) services can be kernel threads (`task_create`) or user-mode processes (`task_exec`); the `svc_start()` path must distinguish these. (2) `svc_stop()` sends `SIGTERM` then waits up to 5 s (`system_get_ticks()` polling) before `SIGKILL` -- this is a timed multi-step operation that must not block the calling context. (3) The 32-slot static table avoids dynamic allocation in the service monitor path. (4) Service state transitions must be atomic (spinlock-protected) because `svc_monitor_tick()` runs in PIT interrupt context while shell commands run in task context. **State machine**: `STOPPED → STARTING → RUNNING → (crash) → STOPPED/FAILED`; explicit stop: `RUNNING → STOPPING → STOPPED`. **Registry layout**: `HKLM\SYSTEM\Services\{name}\AutoStart` (DWORD), `ExePath` (SZ), `Critical` (DWORD), `RestartPolicy` (SZ: "always"|"on-failure"|"never"), `Description` (SZ).

- [ ] `typedef enum { SVC_STOPPED, SVC_STARTING, SVC_RUNNING, SVC_STOPPING, SVC_FAILED } svc_state_t;`
- [ ] `typedef struct { char name[32]; char exe_path[256]; svc_state_t state; uint32_t pid; uint8_t auto_start; uint8_t critical; uint8_t restart_policy; uint8_t restart_count; uint64_t last_exit_ticks; uint64_t backoff_ticks; } service_t;`
- [ ] `#define SVC_MAX 32`, `#define SVC_RESTART_NEVER 0`, `SVC_RESTART_ON_FAILURE=1`, `SVC_RESTART_ALWAYS=2`
- [ ] `service_t g_services[SVC_MAX]` + `spinlock_t g_svc_lock`
- [ ] `void svc_init(void)` -- scan `HKLM\SYSTEM\Services` subkeys; populate `g_services[]`; start auto_start services in dependency order
- [ ] `int svc_start(const char *name)` -- find service; if `exe_path` empty → `task_create(entry_fn, name)` (kernel thread); else → load ELF via VFS + `task_exec()`; set state=STARTING; update pid
- [ ] `int svc_stop(const char *name)` -- set state=STOPPING; `signal_send(pid, SIGTERM)`; schedule `svc_force_kill_check()` at +5 s
- [ ] `void svc_force_kill_check(const char *name)` -- called 5 s after SIGTERM; if still STOPPING: `signal_send(pid, SIGKILL)`; state=STOPPED
- [ ] `int svc_restart(const char *name)` -- `svc_stop()`; deferred `svc_start()` after process confirmed dead
- [ ] `int svc_status(const char *name, service_t *out)` -- copy entry; return 0 or -1
- [ ] `int svc_list(service_t *out, int max)` -- copy all non-empty slots; return count
- [ ] `int svc_register(const char *name, const char *exe_path, uint8_t auto_start, uint8_t critical, uint8_t policy)` -- find free slot; write to Registry; populate slot
- [ ] Commit: `"svc: service manager core -- 32-slot table, svc_start/stop/restart/status, Registry definitions"`

## 3. Auto-Restart & Crash Recovery `[Opus]`

`svc_monitor_tick()` called from PIT once per second: check each RUNNING service's process state; on unexpected exit: schedule restart with exponential backoff (1→2→4→8→16→32→60 s). After 5 failures: FAILED state + toast. Reset counter after 60 s successful run.

**Files:** `src/kernel/svc.c` (extend)

> [!NOTE]
> This is `[Opus]` -- the crash monitor runs in PIT interrupt context (or a workqueue callback) and must check process liveness, manage per-service backoff state, and schedule deferred restarts -- all without blocking or allocating. The backoff sequence must be stable against races: if `svc_stop()` is called explicitly while a backoff is pending, the pending restart must be cancelled. **Liveness check**: `task_is_alive(pid)` -- a new function that checks if `pid` exists in the task table with non-ZOMBIE state; add to `task.h`. **Backoff calculation**: `backoff_ticks = min(SVC_BACKOFF_CAPS[restart_count], 60 * PIT_TARGET_FREQ)` where `SVC_BACKOFF_CAPS[] = {1,2,4,8,16,32,60} * PIT_TARGET_FREQ`. **Restart window**: if `state==RUNNING && !task_is_alive(pid) && last_exit_ticks==0`: mark exit time, schedule backoff. Backoff pending: `svc.last_exit_ticks > 0 && current_ticks >= last_exit_ticks + backoff_ticks`. **Failure cap**: `restart_count >= 5` → `state = SVC_FAILED`; `notify_send("Service Failed", name, ICON_ERROR, 0)` (persistent).

- [ ] `int task_is_alive(uint32_t pid)` -- add to `task.h`; scan task table; return 1 if pid exists and not ZOMBIE/DEAD
- [ ] `uint32_t g_svc_backoff_ticks[7]` -- `{1,2,4,8,16,32,60} * PIT_TARGET_FREQ` -- pre-computed constants
- [ ] `void svc_monitor_tick(void)` -- called from workqueue every `PIT_TARGET_FREQ` ticks (1 s); iterate `g_services[]`; take spinlock; for each RUNNING: `task_is_alive(pid)` → if dead: record `last_exit_ticks`; for each with pending backoff: if elapsed ≥ backoff: `svc_restart_deferred()`
- [ ] `void svc_restart_deferred(service_t *svc)` -- check `restart_policy`; if NEVER: `state=STOPPED`; return; increment `restart_count`; if ≥ 5: `state=SVC_FAILED`; `notify_send(...)`; return; compute next backoff; `svc_start(svc->name)`
- [ ] 60 s success reset: in `svc_monitor_tick()`: for each RUNNING where `system_get_ticks() - start_ticks >= 60 * PIT_TARGET_FREQ` and `restart_count > 0`: reset `restart_count = 0`
- [ ] `svc_monitor_tick()` registered as a workqueue item scheduled every `PIT_TARGET_FREQ` ticks from the PIT interrupt handler
- [ ] Explicit stop race: `svc_stop()` sets `svc->last_exit_ticks = 0` AND `svc->restart_policy = SVC_RESTART_NEVER` temporarily; `svc_monitor_tick()` skips services in STOPPING state
- [ ] Commit: `"svc: crash monitor -- svc_monitor_tick, exponential backoff 1-60s, 5-failure FAILED state, task_is_alive"`

## 4. Graceful Degradation `[Sonnet]`

`critical` flag: if critical service fails unrecoverably → `kernel_panic()` BSOD. Normal services: log + auto-restart + toast. Desktop remains usable. `sc list` shows ✅ RUNNING / ⚠ RESTARTING / ❌ FAILED display.

**Files:** `src/kernel/svc.c` (extend), `src/desktop/svc_ui.c` (new)

> [!NOTE]
> Critical gate in `svc_restart_deferred()`: after setting `state = SVC_FAILED`: check `svc->critical`; if 1: `klog(LOG_CRITICAL, "[svc] critical service %s failed -- panic", name)`; `kernel_panic("Critical service failure")`. Normal failure path: `klog(LOG_ERROR, "[svc] service %s FAILED after 5 restarts", name)`; `notify_send("Service Failed", msg, ICON_ERROR, 0)` with `timeout_ms=0` (persistent toast, no auto-dismiss). The desktop remains fully usable -- non-critical service failure does not affect the compositor or WM.

- [ ] Critical path in `svc_restart_deferred()`: `if (svc->critical && restart_count >= 5) kernel_panic("Critical service %s unrecoverable", svc->name)`
- [ ] Non-critical FAILED: persistent `notify_send()` with `timeout_ms=0`; serial log
- [ ] State display helpers: `svc_state_emoji(state)` → "✅" / "⚠" / "❌" / "⏸" for RUNNING/RESTARTING/FAILED/STOPPED
- [ ] `svc_format_status(svc, buf, size)` -- format: `"{emoji} {name:16} PID:{pid:5} Restarts:{n} Backoff:{s}s"`
- [ ] Commit: `"svc: degradation -- critical BSOD gate, persistent FAILED toast, svc_state_emoji display"`

## 5. `sc` Shell Command `[Sonnet]`

`sc list`, `sc start <name>`, `sc stop <name>`, `sc restart <name>`, `sc status <name>` (shows state + PID + restart count + backoff delay).

**Files:** `src/shell/cmds.c` (extend)

> [!NOTE]
> Shell `sc` command registered in `src/shell/cmds.c` command dispatch table. `sc list`: call `svc_list(buf, 32)`; iterate; `kprintf(svc_format_status(...))` per line. `sc status <name>`: `svc_status(name, &entry)`; print full status block including backoff countdown. `sc start/stop/restart <name>`: call `svc_start/stop/restart(name)`; print confirmation or error. Colour coding with ANSI sequences if terminal supports it (stub: always emit plain text for serial).

- [ ] `void cmd_sc(int argc, char **argv)` in `cmds.c` -- dispatch on `argv[1]`: list/start/stop/restart/status
- [ ] `sc` with no subcommand: print usage string
- [ ] `sc list`: iterate + print emoji + name + state line; align columns with fixed-width format
- [ ] `sc status <name>`: print multi-line: state, PID, restart count, backoff ticks remaining, exe_path, critical flag
- [ ] Error: unknown service → `"Unknown service: {name}\n"`
- [ ] Register `sc` in command table with `{ "sc", cmd_sc, "Service control: list/start/stop/restart/status" }`
- [ ] Commit: `"shell: sc command -- list/start/stop/restart/status with emoji state display"`

## 6. Built-In Services `[Sonnet]`

`netd` (DHCP renewal + ARP cache + network polling -- auto-start), `ntpd` (NTP sync every 60 min -- auto-start after netd), `registryd` (Registry dirty flush every 2 s -- auto-start, critical=true), `indexd` (file search rebuild every 30 min -- auto-start). Dependency order enforced.

**Files:** `src/kernel/svc_builtins.c` (new)

> [!NOTE]
> Each built-in service is a kernel thread (not a user-mode process) -- `svc_register(name, "", auto_start=1, critical, policy)` with empty `exe_path`; `svc_start()` calls `task_create(entry_fn, name)`. **Dependency order**: `svc_init()` starts services in priority order: registryd first (critical) → then `vfs_mount_all()` completes → then netd → then ntpd. `indexd` starts last (lowest priority; non-critical). Dependency check: each service has a `depends_on[4]` name array; `svc_start()` verifies all deps are RUNNING before proceeding. **registryd**: kernel thread runs `reg_flush_dirty()` every `2 * PIT_TARGET_FREQ` ticks; `critical=1` (if Registry can't flush, data loss risk). **netd**: wraps `net_dhcp_tick()` + `net_arp_tick()` in a loop; `knotify_send("netd", "Network connected", KNOTIFY_INFO)` on IP change. **ntpd**: runs `ntp_sync_now()` every 60 min; `knotify_send("ntpd", "Time synced", KNOTIFY_INFO)` on success. **indexd**: periodic VFS scan to build a search index (stub: just logs "index rebuild done").

- [ ] `void svc_builtins_register(void)` -- calls `svc_register()` for registryd, netd, ntpd, indexd with correct flags
- [ ] `registryd_thread(void)` -- `reg_flush_dirty()` every 2 s; `klog()` on flush errors
- [ ] `netd_thread(void)` -- `net_dhcp_tick()` + `net_arp_tick()` every 1 s; `knotify_send()` on IP change
- [ ] `ntpd_thread(void)` -- sleep 60 min; `ntp_sync_now()` (→ XREF `07-networking/TODO-06`); `knotify_send("ntpd", "Time synced", KNOTIFY_INFO)`
- [ ] `indexd_thread(void)` -- sleep 30 min; `index_rebuild()` stub; `knotify_send("indexd", "Index rebuilt", KNOTIFY_INFO)`
- [ ] Honor `kernel_config_get()->safe_mode`: always start `registryd`; allow `netd` only in normal or Safe Mode network; suppress `ntpd`, `indexd`, and other nonessential services in minimal/recovery profiles
- [ ] Dependency array: `registryd.depends_on = {}`, `netd.depends_on = {"registryd"}`, `ntpd.depends_on = {"netd"}`, `indexd.depends_on = {"registryd"}`
- [ ] `svc_builtins_register()` called from `svc_init()`; auto-start ordering enforced by dependency walk
- [ ] Commit: `"svc: built-in daemons -- registryd(critical)/netd/ntpd/indexd with dependency order"`

## 7. Notification Service `[Sonnet]`

Kernel-side `struct kernel_notification` queue (source[32], message[256], severity: INFO/WARN/ERROR, timestamp). `knotify_send(source, msg, severity)` enqueues. Compositor drains queue per frame → calls `notify_send()`. Used by: NTP sync, disk mount/unmount, network up/down.

**Files:** `src/kernel/knotify.c` (new), `include/kernel/knotify.h` (new)

> [!NOTE]
> Separate from the desktop `notify_send()` (user-visible toasts in TODO-09): `knotify_send()` is safe to call from any kernel context (interrupt, workqueue, kernel thread) -- it only enqueues into a lock-protected ring buffer. The compositor (or a dedicated desktop poll) drains this ring buffer per frame and calls `notify_send()` for each entry. This decouples the kernel from the desktop notification system. **Queue**: `#define KNOTIFY_QUEUE_SIZE 32`; ring buffer `g_knotify_queue[32]`; `spinlock_t g_knotify_lock`. Severity → icon mapping: `KNOTIFY_INFO` → `ICON_INFO`, `KNOTIFY_WARN` → `ICON_WARNING`, `KNOTIFY_ERROR` → `ICON_ERROR`. Drain: called from `wm_composite()` pre-draw tick: `knotify_drain()` iterates pending entries; calls `notify_send(source, message, icon, 4000)`.

- [ ] `typedef enum { KNOTIFY_INFO=0, KNOTIFY_WARN=1, KNOTIFY_ERROR=2 } knotify_severity_t;`
- [ ] `typedef struct { char source[32]; char message[256]; knotify_severity_t severity; int64_t timestamp; } kernel_notification_t;`
- [ ] `void knotify_send(const char *source, const char *msg, knotify_severity_t severity)` -- spinlock; enqueue; if full: overwrite oldest; release lock
- [ ] `int knotify_drain(void)` -- take lock; copy pending entries to local array; release lock; call `notify_send()` for each; return count drained
- [ ] `knotify_drain()` called from `desktop_tick()` (called each compositor frame)
- [ ] Severity → icon: `KNOTIFY_INFO → ICON_INFO`, `KNOTIFY_WARN → ICON_WARNING`, `KNOTIFY_ERROR → ICON_ERROR`
- [ ] Commit: `"knotify: kernel notification queue -- knotify_send() ring buffer, knotify_drain() to notify_send()"`

## 8. Autostart Programs `[Sonnet]`

Scan `C:\Users\Default\AppData\Startup\` for `.lnk` files after desktop fully initialized; execute via `shortcut_execute()`. `HKLM\SYSTEM\Boot\RunOnce` (run once, then delete). `HKLM\SYSTEM\Boot\Run` (run every boot).

**Files:** `src/desktop/autostart.c` (new), `include/desktop/autostart.h` (new)

> [!NOTE]
> Autostart runs after `desktop_init()` completes and all built-in services are RUNNING -- ensures that autostart apps have the full desktop available. **Startup folder scan**: `vfs_finddir(vfs_root, "C:\\Users\\Default\\AppData\\Startup\\")` → `vfs_readdir()` loop; for each `.lnk` file: `shortcut_execute(path)`. Failure: if `shortcut_execute()` returns -1: `knotify_send("autostart", "Failed: {name}", KNOTIFY_WARN)`. **RunOnce**: `RegGetValue(HKLM, "SYSTEM\\Boot\\RunOnce\\", NULL)` → enumerate subkeys; for each: execute command string; delete key after execution. **Run**: same but do not delete key. **Order**: Run first, then RunOnce (matches Windows behavior), then Startup folder.

- [ ] `void autostart_run(void)` -- called from `desktop_init()` after services are RUNNING
- [ ] `autostart_run_startup_folder()`: `vfs_readdir("C:\\Users\\Default\\AppData\\Startup\\")` loop; filter `.lnk`; `shortcut_execute()`; `knotify_send()` on error
- [ ] `autostart_run_registry()`: enumerate `HKLM\SYSTEM\Boot\Run` subkeys; execute each value as a command string via `file_assoc_open()` or `shortcut_execute()`
- [ ] `autostart_run_once()`: enumerate `HKLM\SYSTEM\Boot\RunOnce`; execute; `RegDeleteKey()` after each
- [ ] Execution order: `autostart_run_registry()` → `autostart_run_once()` → `autostart_run_startup_folder()`
- [ ] Suppress `Run`, `RunOnce`, and Startup-folder execution when Safe Mode or recovery policy forbids autostart; allow only explicitly tagged recovery tools
- [ ] Log each autostart execution: `[autostart] run: %s`
- [ ] Commit: `"autostart: Startup folder scan, Run/RunOnce Registry execution, error knotify"`

## 9. Win32 System Info Stubs `[Sonnet]`

`GetSystemInfo()` (CPU count, page size, architecture). `GetVersionExA()` (Win11 compat: 11.0 build 22000, "Impossible OS 1.0"). `GetComputerNameA/GetUserNameA/GetTempPathA/GetSystemDirectoryA/GetWindowsDirectoryA`. All in `kernel32.dll` stub table.

**Files:** `src/desktop/win32/kernel32_sysinfo.c` (new), `include/desktop/win32/kernel32.h` (new)

> [!NOTE]
> Win32 `SYSTEM_INFO` struct layout: `wProcessorArchitecture=9` (PROCESSOR_ARCHITECTURE_AMD64), `dwPageSize=4096`, `dwNumberOfProcessors = smp_cpu_count()`, `wProcessorLevel=6`, `wProcessorRevision=0`. `OSVERSIONINFOA` struct: `dwMajorVersion=11`, `dwMinorVersion=0`, `dwBuildNumber=22000`, `dwPlatformId=2` (VER_PLATFORM_WIN32_NT), `szCSDVersion="Impossible OS 1.0"`. `GetComputerNameA`: `RegGetValue(HKLM, "SYSTEM\\Network\\Hostname", ...)` → copy into buffer; default "ImpossiblePC" if key absent. `GetUserNameA`: `user_get_name()` from §9. `GetTempPathA`: return `"C:\\Temp\\"` (always). `GetSystemDirectoryA`: return `"C:\\Impossible\\System32\\"`. `GetWindowsDirectoryA`: return `"C:\\Impossible\\"`.

- [ ] `typedef struct { uint16_t wProcessorArch; uint16_t reserved; uint32_t dwPageSize; void *lpMinAppAddr; void *lpMaxAppAddr; uintptr_t dwActiveProcMask; uint32_t dwNumOfProcs; uint32_t dwProcType; uint32_t dwAllocGranularity; uint16_t wProcLevel; uint16_t wProcRevision; } SYSTEM_INFO;`
- [ ] `void GetSystemInfo(SYSTEM_INFO *si)` -- fill from `smp_cpu_count()` + constants
- [ ] `typedef struct { uint32_t dwOSVersionInfoSize; uint32_t dwMajor; uint32_t dwMinor; uint32_t dwBuildNum; uint32_t dwPlatformId; char szCSDVersion[128]; } OSVERSIONINFOA;`
- [ ] `int GetVersionExA(OSVERSIONINFOA *osvi)` -- fill; return 1
- [ ] `int GetComputerNameA(char *buf, uint32_t *size)` -- Registry read + fallback
- [ ] `int GetUserNameA(char *buf, uint32_t *size)` → `user_get_name()` from §9
- [ ] `uint32_t GetTempPathA(uint32_t size, char *buf)` → `kstrncpy(buf, "C:\\Temp\\", size)`; return length
- [ ] `uint32_t GetSystemDirectoryA(char *buf, uint32_t size)` → `"C:\\Impossible\\System32\\"`
- [ ] `uint32_t GetWindowsDirectoryA(char *buf, uint32_t size)` → `"C:\\Impossible\\"`
- [ ] Register all in `kernel32.dll` export table (add `kernel32.h` stub alongside `gdi.h` / `user32.h`)
- [ ] Commit: `"kernel32: GetSystemInfo/GetVersionExA/GetComputerNameA/GetTempPathA/GetSystemDirectoryA stubs"`

---

## OS Comparison


| ⭐  | Feature                               | 🪟 Win11                                                                     | 🐧 Linux                                                                   | 🚀 Impossible OS                                              |
| --- | ------------------------------------- | ---------------------------------------------------------------------------- | -------------------------------------------------------------------------- | ------------------------------------------------------------- |
| ⭐  | Service manager                       | ✅ SCM (Services Control Manager); full                                      | ✅ systemd / OpenRC / runit;                                               | ⬜ §1 -- `⭐` zero-dependency static table; no                |
| ⭐  | Auto-restart with exponential backoff | ✅ SCM restart policy (delay, max                                            | ✅ systemd `Restart=on-failure`; `RestartSec`; `StartLimitBurst`; journald | ⬜ §2 -- `⭐` PIT-ticked in-kernel monitor; no                |
| 💎  | Graceful degradation                  | ✅ Critical services → BSoD; non-critical                                    | ✅ systemd critical unit → emergency.target;                               | ⬜ §3 -- `critical=1` → `kernel_panic()`; else persistent     |
| 💎  | Built-in daemons                      | ✅ Dozens of system services; LSASS,                                         | ✅ systemd system daemons; NetworkManager, chrony,                         | ⬜ §5 -- 4 kernel threads; dependency `depends_on[4]`         |
| 💎  | Kernel notification queue             | ✅ Event Log + WNF (Windows                                                  | ✅ `printk` → `journald`; `netlink` event                                  | ⬜ §6 -- `⭐` no IPC or daemon                                |
| 💎  | Autostart                             | ✅ `HKLM/HKCU\Software\Microsoft\Windows\CurrentVersion\Run`; Startup folder | ✅ XDG autostart (`~/.config/autostart/`); systemd user                    | ⬜ §7 -- identical Win32 `Run`/`RunOnce` Registry layout      |
| 💎  | Win32 system info                     | ✅ Full Win32 APIs; accurate CPU                                             | ✅ `uname(2)`; `/proc/cpuinfo`; `getpwuid()`; Wine kernel32                | ⬜ §8 -- Win11 compat version (11.0.22000); `smp_cpu_count()` |
| 💎  | User account stub                     | ✅ Full SAM; NTLM/Kerberos auth; per-user                                    | ✅ `/etc/passwd`; `PAM`; per-user `~/.config`; UID/GID                     | ⬜ §9 -- (single stub user); full multi-user                  |

> **After §1–§9:** Impossible OS has a production-grade service lifecycle layer with zero external dependencies -- no D-Bus, no XML unit files, no daemon processes managing daemons. The `⭐` differentiators are the PIT-ticked in-kernel crash monitor (backoff computed from a pre-built constant array, running in workqueue context) and the kernel notification queue (a simple lock-protected ring buffer drained by the compositor, eliminating any kernel-to-user IPC machinery).

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot: serial log shows `[svc] starting registryd`, `[svc] registryd RUNNING pid=N` before other services
- [ ] `sc list` in shell → shows 4 built-in services with ✅ RUNNING state
- [ ] `sc stop netd` → netd STOPPED; `sc start netd` → RUNNING; `sc restart netd` → restart logged
- [ ] Kill `netd` externally (`sc stop` + SIGKILL) → monitor detects exit within 1 s; restarts after 1 s backoff; serial log `[svc] netd exited unexpectedly, restart in 1s`; 5 consecutive kills → state=FAILED; toast "Service netd failed"
- [ ] `registryd` killed → state=FAILED after 5 attempts → `kernel_panic()` BSOD (test in QEMU only)
- [ ] `knotify_send("test", "Hello kernel notify", KNOTIFY_INFO)` → toast appears within one compositor frame
- [ ] `C:\Users\Default\AppData\Startup\` contains `test.lnk` → after `desktop_init()`: shortcut executes
- [ ] `HKLM\SYSTEM\Boot\RunOnce\0001 = "notepad.exe"` → notepad launches on boot; key deleted after
- [ ] `GetSystemInfo()` → `dwNumberOfProcessors == smp_cpu_count()`; `dwPageSize == 4096`
- [ ] `GetVersionExA()` → `dwMajorVersion=11`, `dwBuildNumber=22000`, `szCSDVersion="Impossible OS 1.0"`
- [ ] `GetUserNameA(buf, &size)` → "Default"; `GetSystemDirectoryA()` → "C:\\Impossible\\System32\\"
- [ ] `user_get_home()` → "C:\\Users\\Default"; `vfs_finddir(root, "C:\\Users\\Default\\Documents\\")` → non-NULL
- [ ] Commit: `"svc: service manager, daemons, knotify, autostart, Win32 sysinfo stubs -- complete"`
