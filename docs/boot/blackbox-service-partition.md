<!-- docs: covers=todo/01-boot-platform/TODO-24-blackbox-service-partition.md sources=tools/make-system-disk.c,src/kernel/fs/partition.c,src/kernel/klog_disk.c,include/kernel/klog.h,src/kernel/wer.c,src/kernel/main/boot_storage.c,src/kernel/fs/fat32/fat32_ops.c,src/kernel/fs/fat32/fat32_fsck.c,scripts/tools/read-blackbox.sh,Makefile reviewed=2026-09-28 order=24 -->
# BlackBox Service Partition

## What is it?

BlackBox is a dedicated 128 MiB FAT32 partition, mounted as `X:\`, that holds the kernel logs, crash reports, boot timelines and diagnostic files the system produces. It exists so that a damaged or unbootable IXFS system volume never takes the diagnostic evidence with it, and so that evidence can be read on any computer: Windows, Linux and macOS all read FAT32 without extra drivers.

The partition, its mount, its directory skeleton, low-space cleanup and crash-report retention all ship today. Several hardening items around that core are still open: a latent log reentrancy hazard, diagnostic writers that report success without checking the write, some FAT32 checker robustness gaps, and a crash-report writer that still does file I/O from inside the exception path.

## How does it work?

`tools/make-system-disk.c` writes BlackBox as the second GPT entry, after the EFI System Partition, 1 MiB aligned, with the Microsoft Basic Data type GUID `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` and the partition name `BlackBox`. It is 128 MiB in every layout. Without `--ab` the tool builds three partitions (EFI, BlackBox, IXFS); the default `system-disk` Makefile target passes `--ab`, so a normal build produces six (EFI, BlackBox, Slot A, Slot B, A/B metadata, Recovery). The Makefile formats BlackBox with `mkfs.fat -F 32 -n "BLACKBOX" -s 1`, reading its offset and size from the image's `.info` sidecar, and creates the eight top-level directories: `Logs`, `Logs\Serial`, `Boot`, `Crash`, `Crash\WER`, `Perf`, `Diag` and `Tools`.

The kernel finds the partition by its GPT name, compared case-insensitively, rather than by drive-letter order, and mounts it as `X:\`. The letter `X` stays reserved even on a disk without BlackBox, so paths remain stable. At mount the kernel checks the FAT32 dirty bit and runs `fat32_run_fsck()` in repair mode if it is set, then marks the volume dirty until a clean shutdown clears it. The boot path recreates the directory skeleton every boot, so a partial skeleton heals itself.

Log output follows a runtime `klog_dir` rather than a fixed path. `klog_resolve_dir()` points it at `X:\Logs\` when BlackBox is mounted and at `C:\Impossible\System\Logs\` otherwise. Not every writer follows it: `boot-health.json` uses a fixed `X:\Diag\` path and `boot-trend.json` is skipped outright when BlackBox is absent, so without BlackBox those two files are simply not produced. The file-by-file inventory of what lands under `X:\Diag\` is in [BlackBox Diagnostic Artifacts](black-box-artifacts.md).

Two subsystems keep the partition usable over time. Low-space cleanup runs at mount and logs the free space; below 10% free it prunes old `Boot\` sessions and rotated `Logs\` files, then checks again. Crash-report retention prunes `X:\Crash\WER\*.json` oldest first once the count exceeds `WER_MAX_REPORTS` (64). It runs from the mount and cleanup paths, deliberately not from the exception path that writes a report.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| GPT name `BlackBox`, type `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` | The partition's identity on disk ([`make-system-disk.c`](../../tools/make-system-disk.c)) |
| `partition_mount_filesystems()` | Mounts the partition named `BlackBox` as `X:\`, with the dirty-bit check ([`partition.c`](../../src/kernel/fs/partition.c)) |
| `klog_dir`, `klog_using_blackbox`, `klog_resolve_dir()` | Choose `X:\Logs\` or the `C:\` fallback at runtime ([`klog.h`](../../include/kernel/klog.h), [`klog_disk.c`](../../src/kernel/klog_disk.c)) |
| `fat32_is_dirty()`, `fat32_mark_dirty()`, `fat32_mark_clean()`, `fat32_run_fsck()` | Dirty-bit tracking and repair ([`fat32_ops.c`](../../src/kernel/fs/fat32/fat32_ops.c), [`fat32_fsck.c`](../../src/kernel/fs/fat32/fat32_fsck.c)) |
| `wer_write_crash_report()`, `wer_prune_reports()` | Write a JSON crash report to `X:\Crash\WER\` and prune old ones ([`wer.c`](../../src/kernel/wer.c)) |
| Skeleton, cleanup and retention wiring | Directory recreation, low-space cleanup and retention calls during boot ([`boot_storage.c`](../../src/kernel/main/boot_storage.c)) |
| `read-blackbox.sh` | Extracts the BlackBox tree from a disk image on the host ([`read-blackbox.sh`](../../scripts/tools/read-blackbox.sh)) |

## How do I use it?

```bash
bash scripts/build.sh
bash scripts/tools/read-blackbox.sh build/system-disk.img
```

The second command copies the partition's contents out of the image with mtools. The script's header also shows the equivalent read-only loop mount, using the offset from the `.info` file.

A successful mount logs `BlackBox partition mounted as X:\`. A volume left dirty by a crash logs `BlackBox: partition dirty -- possible corruption` before the repair. Without BlackBox, the kernel logs `BlackBox not mounted, using C:\ for logs` and the kernel logs move to `C:\`; the boot-health and boot-trend files are not written at all. Each directory the skeleton step recreates logs `BlackBox: created <dir>`. Free space is reported as `BlackBox: <n> MiB free (<pct>%)`; a cleanup run logs `BlackBox: cleanup freed <n> KiB (<n> files removed)`, and pruning crash reports logs `WER retention: pruned <n> old report(s) (cap 64)`.

## What is not implemented yet?

- Two log-path gaps remain: a reentrancy hazard in `klog_disk_flush()` on the `C:\` fallback path, and boot-health, audit and crash-report writers that report success without checking the write or close result: [Klog Migration](../../todo/01-boot-platform/TODO-24-blackbox-service-partition.md#5-klog-migration----move-all-log-output-to-xlogs).
- Cleanup has four open refinements: a later `klog_resolve_dir()` call undoes the critically-low redirect, `Boot\` sessions are pruned in directory order rather than oldest first, path buffers are a fixed 64 bytes, and the limits are hardcoded rather than read from the Registry: [Disk Space Management](../../todo/01-boot-platform/TODO-24-blackbox-service-partition.md#10-disk-space-management----log-aging-quota-cleanup).
- The FAT32 health path has four open items, among them dirty-bit writes that do not report a torn mirror write and a directory walk in the checker with no cycle guard: [Partition Health](../../todo/01-boot-platform/TODO-24-blackbox-service-partition.md#12-partition-health----fsck-on-mount-dirty-bit-check).
- `wer_write_crash_report()` still performs file I/O from the exception path, which can deadlock if the fault happened inside filesystem code: [WER Staging Area](../../todo/01-boot-platform/TODO-24-blackbox-service-partition.md#13-wer-staging-area----error-reports-in-xcrashwer).
- Two other roadmap files still describe crash dumps under `CrashDumps\` instead of `X:\Crash\`: [Boot Platform TODO Updates](../../todo/01-boot-platform/TODO-24-blackbox-service-partition.md#15-boot-platform-todo-updates----xrefs-and-domain-sync).

## How does it compare with Windows 11 and Linux?

Windows 11 keeps a separate recovery partition, but its logs and crash dumps live on the system volume under `C:\Windows`. Linux keeps `/var/log` and `/var/crash` on the root filesystem. Impossible OS gives logs, crash reports, boot timelines and diagnostics their own partition, found by GPT name and readable on any host, with log aging and a quota comparable to Windows' CBS log cap and Linux's logrotate, and a dirty-bit check at mount comparable to `chkdsk` and `fsck.fat`. Per-boot log folders and a boot timeline file have no built-in equivalent on either system.

## See also

- [BlackBox Service Partition roadmap](../../todo/01-boot-platform/TODO-24-blackbox-service-partition.md)
- [BlackBox Diagnostic Artifacts](black-box-artifacts.md), the file-by-file deep dive
- [Boot Diagnostics](boot-diagnostics.md)
- [Boot Health Wire Format](boot-health-schema.md)
- [Boot Trend Wire Format](boot-trend-schema.md)
