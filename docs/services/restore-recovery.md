<!-- docs: covers=todo/10-platform-services/TODO-04-restore-recovery.md sources=include/kernel/klog.h,src/kernel/klog_disk.c,src/kernel/wer.c,src/kernel/panic.c,include/kernel/boot_recovery.h,src/kernel/main/boot_desktop.c,src/kernel/main/boot_storage.c,src/kernel/registry.c,include/kernel/fs/gpt.h,include/kernel/uefi_runtime.h reviewed=2026-09-29 order=4 -->
# System Restore, Recovery and Observability

## What is it?

This roadmap is the OS safety net: restore points and rollback, the Restore UI (`rstrui.cpl`), a first-boot setup wizard, an event log, a crash report viewer, an F8 recovery environment, factory reset, startup repair and disk cleanup. None of its ten sections has shipped. Several lower layers it builds on already run, notably the on-disk event log, crash reports and a degraded-boot recovery screen.

## How does it work?

**Today.** These pieces ship, owned by other roadmaps:

- **Event log on disk.** The kernel log keeps a 1,000-entry ring and `klog_disk.c` flushes it as JSON Lines to `events.jsonl`, one object per line with `ts`, `lvl`, `sub`, `cpu`, `pid`, `tid`, `msg` and `dropped`, rotating old files into LZ4 archives ([`klog_disk.c`](../../src/kernel/klog_disk.c)). The folder is `X:\Logs\` on the BlackBox partition, or `C:\Impossible\System\Logs\` without one ([`klog.h`](../../include/kernel/klog.h)). The log levels are Debug, Info, Warn, Error and Fatal (0 to 4). See [System Logging](../kernel/system-logging.md).
- **Crash records.** A panic writes `X:\Crash\last-panic.txt` ([`panic.c`](../../src/kernel/panic.c)), and Windows Error Reporting style reports go to `X:\Crash\WER\` ([`wer.c`](../../src/kernel/wer.c)). On the next boot the splash shows "system shut down unexpectedly" with the report path ([`boot_desktop.c`](../../src/kernel/main/boot_desktop.c)). No minidump (`.dmp`) writer exists yet; see [Crash Dump Generation](../kernel/crash-dump-generation.md).
- **Degraded-boot screen.** When boot fails in Phase 2 or 3, `boot_recovery_show()` offers Retry, Console (which today halts to the serial log) and Power off ([`boot_recovery.h`](../../include/kernel/boot_recovery.h)).
- **Primitives for repair.** GPT recovery partition types, `gpt_sync_backup()` and `gpt_crc32()` ([`gpt.h`](../../include/kernel/fs/gpt.h)), and `uefi_reset()` with cold, warm and shutdown modes ([`uefi_runtime.h`](../../include/kernel/uefi_runtime.h)).

**Registry backup is not available.** `RegSaveKey()` and `RegRestoreKey()` are declared but return `ERROR_NOT_SUPPORTED` after the privilege check ([`registry.c`](../../src/kernel/registry.c)). Restore points and rollback depend on them.

**Planned design.**

1. **Event log.** Security events and a type field added to the existing `events.jsonl` stream, and an Event Viewer; the viewer itself moved to the [Event Viewer roadmap](../../todo/13-tools-accessories/TODO-02-event-viewer.md).
2. **Restore points.** `restore_create()` zips system files and a Registry backup with a manifest, evicting the oldest past a limit, and `restore_rollback()` verifies SHA-256 before extracting and restoring hives, then reboots.
3. **`rstrui.cpl`.** A timeline of restore points with Create, Restore and Delete.
4. **First-boot wizard.** Six pages (time zone, keyboard, user account, wallpaper, updates), triggered by `HKLM\SYSTEM\FirstBoot`.
5. **Crash report viewer.** A boot-time prompt and a tabbed report window with Export.
6. **F8 recovery environment.** A menu drawn on the boot framebuffer (UEFI has no VGA text mode) with a 15-command recovery shell; the menu itself is owned by [Boot Splash and F8 Recovery](../graphics/boot-splash-recovery.md).
7. **Factory reset, startup repair and disk cleanup.** A typed "YES" guard; GPT, boot entry, kernel hash and hive repair; six cleanup categories with a size preview and a weekly task.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `klog()`, `klog_get_ring()`, `events.jsonl` | Shipped |
| `last-panic.txt`, `X:\Crash\WER\` reports, unexpected-shutdown notice | Shipped |
| `boot_recovery_show()` | Shipped (console option pending) |
| `RegSaveKey()`, `RegRestoreKey()` | Declared, return `ERROR_NOT_SUPPORTED` |
| `kevent_log()`, `restore_create()`, `restore_list()`, `restore_rollback()` | Planned |
| `rstrui.cpl`, first-boot wizard, recovery shell, cleanup | Planned |

## How do I use it?

After a crash, reboot and read the notice on the splash, then open `X:\Crash\last-panic.txt` or the newest report under `X:\Crash\WER\`. The event history is in `events.jsonl` in the log folder above. The degraded-boot screen appears on its own when boot fails; press R, C or P.

## What is not implemented yet?

- [Event Log](../../todo/10-platform-services/TODO-04-restore-recovery.md#1-event-log-sonnet)
- [Restore Point Creation](../../todo/10-platform-services/TODO-04-restore-recovery.md#2-restore-point-creation-sonnet) and [System Rollback](../../todo/10-platform-services/TODO-04-restore-recovery.md#3-system-rollback-sonnet), which need Registry hive save and restore from the [Registry roadmap](../../todo/02-kernel-core/TODO-14-registry-completion.md) and the ZIP API
- [`rstrui.cpl`](../../todo/10-platform-services/TODO-04-restore-recovery.md#4-rstruicpl----system-restore-ui-sonnet)
- [First-Boot Setup Wizard](../../todo/10-platform-services/TODO-04-restore-recovery.md#5-first-boot-setup-wizard-sonnet)
- [Crash Dump Viewer](../../todo/10-platform-services/TODO-04-restore-recovery.md#6-crash-dump-viewer-sonnet)
- [F8 Recovery Environment](../../todo/10-platform-services/TODO-04-restore-recovery.md#7-f8-recovery-environment-opus)
- [Factory Reset](../../todo/10-platform-services/TODO-04-restore-recovery.md#8-factory-reset-sonnet), [Startup Repair](../../todo/10-platform-services/TODO-04-restore-recovery.md#9-startup-repair-sonnet) and [Disk Cleanup](../../todo/10-platform-services/TODO-04-restore-recovery.md#10-disk-cleanup-utility-sonnet)

## How does it compare with Windows 11 and Linux?

Windows 11 has System Restore on Volume Shadow Copy, the graphical Windows Recovery Environment with Startup Repair and Reset this PC, an XML event log and minidump-based error reports. Linux has journald or syslog, GRUB recovery mode, kdump and apport, and snapshots only through Btrfs, ZFS or snapper. Impossible OS already records events, panics and crash reports on a dedicated BlackBox partition; the plan adds ZIP-based restore points checked by SHA-256 before extraction, and startup repair that also verifies the kernel image's hash.

## See also

- [System Restore, Recovery and Observability roadmap](../../todo/10-platform-services/TODO-04-restore-recovery.md)
- [System Logging](../kernel/system-logging.md)
- [Crash Dump Generation](../kernel/crash-dump-generation.md)
- [Boot Splash and F8 Recovery](../graphics/boot-splash-recovery.md)
- [Recovery Partition](../boot/recovery-partition.md)
- [System Updates and IPKG Packages](updates-packages.md)
