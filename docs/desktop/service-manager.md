<!-- docs: covers=todo/09-desktop-shell/TODO-03-service-manager.md sources=src/kernel/env.c,include/kernel/env.h,src/kernel/registry.c,include/registry.h,include/kernel/sched/workqueue.h,src/kernel/main/compositor.c reviewed=2026-09-29 order=9 -->
# Service Manager and Core Daemons

## What is it?

A service manager starts background programs at boot, watches them, restarts the ones that crash and lets an administrator start, stop and query them. This roadmap plans one for Impossible OS: a service table defined in the Registry, restart with backoff, a `sc` command, built-in daemons for the network, time, Registry flushing and the search index, a notification queue for the shell, autostart programs, the basic Win32 system information calls and a single default user account. Nothing in it has shipped. There is no service manager, no daemon and no `sc` command.

## How does it work?

**Today.** Background work runs as kernel code, started directly by the boot sequence, not as managed services. Some of what the roadmap would provide already exists in other forms:

- **Default user.** There is no account record, but the environment block already describes one user: `USERNAME` is `Default`, and `USERPROFILE`, `APPDATA` and `LOCALAPPDATA` point under `C:\Users\Default` ([`env.c`](../../src/kernel/env.c)). The Registry creates `HKU\Default`, and `HKEY_CURRENT_USER` resolves to it ([`registry.c`](../../src/kernel/registry.c)).
- **Machine facts.** `COMPUTERNAME` comes from `HKLM\SYSTEM\ComputerName\ActiveComputerName`, defaulting to `IMPOSSIBLE-PC`; `TEMP` is `C:\Temp`; the system folders are `C:\Impossible` and `C:\Impossible\System32` ([`env.h`](../../include/kernel/env.h)). A future `GetComputerNameA` or `GetTempPathA` should read these rather than define new values.
- **Registry flushing.** The compositor loop calls `registry_flush()` on every pass, which saves changed hives once `C:` is mounted ([`compositor.c`](../../src/kernel/main/compositor.c)). That is the job the planned `registryd` daemon would take over.
- **Deferred work.** Kernel work queues exist (`workqueue_create()`, `workqueue_enqueue()`, [`workqueue.h`](../../include/kernel/sched/workqueue.h)), and are the natural home for a service monitor.

The Kernel Notification Facility ([KNF](../kernel/kernel-notification-facility.md)) is a different mechanism from the toast queue this roadmap calls `knotify`; the two names should not be confused.

**Planned design.**

1. **User account stub.** One account, `Default`, user ID 1000, an administrator, with its home folder tree.
2. **Service table.** 32 slots, each defined under `HKLM\SYSTEM\Services\{name}` with `AutoStart`, `ExePath`, `Critical` and `RestartPolicy`. Stopping sends SIGTERM, then SIGKILL after 5 seconds.
3. **Restart.** Backoff of 1, 2, 4, 8, 16, 32 and 60 seconds, and the FAILED state after five failures.
4. **Degradation.** A failed critical service stops the system; any other failure shows a persistent notification.
5. **`sc` command.** `sc list`, `start`, `stop`, `restart` and `status`.
6. **Built-in services.** `registryd` (critical), `netd`, `ntpd` and `indexd`, with up to four dependencies each.
7. **Notification queue.** A 32-entry ring that the compositor drains into toasts.
8. **Autostart.** The Startup folder and `Run` and `RunOnce` keys.
9. **System information.** `GetSystemInfo`, `GetVersionExA` (reporting Windows 11), `GetComputerNameA`, `GetUserNameA`, `GetTempPathA` and `GetSystemDirectoryA`.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `USERNAME`, `USERPROFILE`, `COMPUTERNAME`, `TEMP` environment values | Shipped, synthesised by [`env.c`](../../src/kernel/env.c) |
| `HKU\Default` and the `HKEY_CURRENT_USER` redirect | Shipped ([`registry.h`](../../include/registry.h)) |
| `registry_flush()` from the compositor loop | Shipped |
| `svc_start()`, `svc_stop()`, `svc_restart()`, `svc_status()`, `sc` | Planned |
| `knotify_send()` | Planned |
| `GetSystemInfo`, `GetVersionExA` and the other kernel32 calls | Planned |

## How do I use it?

It cannot be used yet. `ps` and `kill` in the shell are the only process controls today.

## What is not implemented yet?

- [User Account Stub](../../todo/09-desktop-shell/TODO-03-service-manager.md#1-user-account-stub-sonnet); the full multi-user system is [Security and User Accounts](security-accounts.md)
- [Service Manager Core](../../todo/09-desktop-shell/TODO-03-service-manager.md#2-service-manager-core-opus), [Auto-Restart and Crash Recovery](../../todo/09-desktop-shell/TODO-03-service-manager.md#3-auto-restart--crash-recovery-opus) and [Graceful Degradation](../../todo/09-desktop-shell/TODO-03-service-manager.md#4-graceful-degradation-sonnet)
- [`sc` Shell Command](../../todo/09-desktop-shell/TODO-03-service-manager.md#5-sc-shell-command-sonnet)
- [Built-In Services](../../todo/09-desktop-shell/TODO-03-service-manager.md#6-built-in-services-sonnet)
- [Notification Service](../../todo/09-desktop-shell/TODO-03-service-manager.md#7-notification-service-sonnet), feeding the toasts in [Start Menu, System Tray and Notifications](../graphics/start-menu-tray-notifications.md)
- [Autostart Programs](../../todo/09-desktop-shell/TODO-03-service-manager.md#8-autostart-programs-sonnet)
- [Win32 System Info Stubs](../../todo/09-desktop-shell/TODO-03-service-manager.md#9-win32-system-info-stubs-sonnet)

## How does it compare with Windows 11 and Linux?

Windows 11 has the Service Control Manager with restart policies, critical services that stop the machine when they fail, dozens of system services, Run and RunOnce keys, the Startup folder and full user accounts. Linux distributions use systemd (or OpenRC or runit) with `Restart=on-failure` and start limits, XDG autostart and `/etc/passwd` accounts. Impossible OS has neither yet. The plan keeps the service table and monitor inside the kernel with no separate manager process, and keeps the Windows Registry layout for services and autostart.

## See also

- [Service Manager and Core Daemons roadmap](../../todo/09-desktop-shell/TODO-03-service-manager.md)
- [Environment Variables](../kernel/environment-variables.md)
- [Registry](../kernel/registry.md)
- [Kernel Notification Facility](../kernel/kernel-notification-facility.md)
- [Recycle Bin, ZIP and Task Scheduler](recycle-bin-zip-scheduler.md)
