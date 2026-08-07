---
schema_version: 1
id: volume-management-automount
domain: 05-storage-filesystems
status: active
title: "TODO-03 -- Volume Management & Auto-mount"
---

# TODO-03 -- Volume Management & Auto-mount

> **Goal:** Replace the hardcoded `partition_mount_filesystems()` path with a proper volume manager: a priority-ordered filesystem probe chain, dynamic drive-letter assignment, USB hot-plug mount/unmount with desktop toasts, manual `mount`/`umount` shell commands, Win32 volume query APIs, optical drive support, and `FSCTL_*` volume control ioctls.

> [!IMPORTANT]
> The current `partition_mount_filesystems()` in `src/kernel/fs/partition.c` hardcodes IXFS→C:, FAT32→D:+, NTFS by type field -- no probe abstraction, no hot-plug, no drive-label Registry entries. §1 introduces `vfs_probe()` as the new canonical entry point; §2 rewires `boot_storage.c` to call it. §6 (Win32 APIs) depends on §1's Registry population. §3–4 depend on `04-drivers-hardware/TODO-10-usb-stack.md §8` (hot-plug interrupt events). The IXFS → NTFS volume migration path (`§7` of the old source) is **dropped** -- `C:\` stays IXFS permanently.

## Inputs

- `src/kernel/fs/partition.c` + `include/kernel/fs/partition.h` -- `partition_mount_filesystems()` is the current hardcoded mount path; §1–2 replace it with `vfs_probe()`
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- `vfs_mount()` / `vfs_unmount()` / `vfs_is_mounted()` exist; §1 adds `vfs_probe()` and drive Registry writes
- `src/kernel/main/boot_storage.c` -- calls `partition_mount_filesystems()`; §2 replaces it with the new probe+auto-assign flow
- → XREF: `04-drivers-hardware/TODO-10-usb-stack.md §8` -- hot-plug TRB events that trigger §3 (USB volume arrival) and §4 (safe removal)
- → XREF: `05-storage-filesystems/TODO-01-block-storage-hardening.md §7` -- `cache_flush(dev)` / `cache_invalidate(dev)` must be called during unmount (§4 safe removal and §5 `umount`)
- → XREF: `05-storage-filesystems/TODO-02-ntfs-readwrite.md §3` -- dirty NTFS volume recovery runs inside `ntfs_vfs_mount()`, called by `vfs_probe()` in §1
- → XREF: `08-desktop-shell` domain -- §3 desktop toast and §4 tray icon safe-remove are shell-facing components; coordinate with the notification/tray TODO
- → XREF: `10-apps` domain -- File Manager sidebar (§3 real-time update) and Task Manager disk section (§6 volume stats) consume `vfs_probe` Registry entries
- → XREF: `01-boot-platform/TODO-05-boot-device-discovery.md §2, §3, §8` -- boot_info.boot_device_type and boot_device_removable inform C: drive assignment and cache policy in §1
- → XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md §2` -- BPB validation fires inside `fat32_init()` which `vfs_probe()` calls

## Outcome

- `vfs_probe(blkdev)` detects filesystem type in priority order; all partitions auto-assigned drive letters on boot.
- `HKLM\SYSTEM\Storage\Drive\{letter}\` Registry populated with device, filesystem, label, size, free bytes.
- USB drives auto-mount on hot-plug with a desktop toast; safe-remove flushes and unmounts cleanly.
- `mount` / `umount` shell commands work; `mount` with no args prints a drive table.
- `GetLogicalDrives()`, `GetDriveTypeW()`, `GetVolumeInformationW()`, `SetVolumeLabelW()`, `QueryDosDeviceW()` implemented.
- Optical drives detected, mounted read-only as ISO 9660; autorun disabled by default.
- `FSCTL_IS_VOLUME_DIRTY`, `FSCTL_LOCK_VOLUME`, `FSCTL_UNLOCK_VOLUME`, `FSCTL_DISMOUNT_VOLUME` ioctls implemented.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                | Depends On                                     | Status |
| --- | :---: | ------------------------------------------------------------------------------------------ | ---------------------------------------------- | :----: |
| ⭐  |   1   | §1 Filesystem probe chain + drive-letter assignment + Registry population                  | Existing FS drivers (IXFS, NTFS, FAT32)        |  [ ]   |
| 💎  |   2   | §2 Boot mount sequence -- rewire `boot_storage.c` to use `vfs_probe()`                     | §1 (probe API exists)                          |  [ ]   |
| ⭐  |   3   | §3 USB hot-plug volume arrival -- auto-mount + desktop toast + File Manager sidebar        | §1, TODO-09 §8 hot-plug events                 |  [ ]   |
| 💎  |   4   | §4 USB safe removal -- tray right-click, flush+unmount, force-unmount after 5 s            | §3 (drive mounted), TODO-01 §7 `cache_flush()` |  [ ]   |
| 💎  |   5   | §5 Manual `mount` / `umount` shell commands                                                | §1 (probe), §4 (unmount path)                  |  [ ]   |
| 💎  |   6   | §6 Win32 volume query APIs -- `GetLogicalDrives`, `GetVolumeInformation`, `QueryDosDevice` | §1 (Registry populated)                        |  [ ]   |
| 💎  |   7   | §7 Optical drive -- ATAPI detect, ISO 9660 mount, tray-open, autorun stub                  | §1 (probe chain), ATAPI driver                 |  [ ]   |
| 💎  |   8   | §8 Volume control ioctls -- `FSCTL_IS_VOLUME_DIRTY`, `LOCK`, `UNLOCK`, `DISMOUNT`          | §1 (mounted volumes), §4 (unmount path)        |  [ ]   |

> §1 (probe chain + drive-letter assignment) and §3 (USB hot-plug with toast + sidebar) are `⭐` exclusive: Windows uses a static partition table enumeration with `mountmgr.sys`; Linux uses `udev` rules in user space. Impossible OS performs dynamic probe + assignment + Registry write + toast + sidebar update entirely inside the kernel on a single hot-plug event -- no user-space daemon required.

---

## 1. Filesystem Probe Chain + Drive-Letter Assignment `[Opus]`

Implement `vfs_probe(blkdev)` as a priority-ordered filesystem probe that auto-assigns drive letters and populates Registry entries. Replace the hardcoded type-field checks in `partition_mount_filesystems()`.

**Files:** `src/kernel/fs/vfs_probe.c` (new), `include/kernel/fs/vfs.h` (extend), `src/kernel/fs/partition.c` (demote)

> [!NOTE]
> Probe priority order and identification: (1) IXFS -- read sector 0, check magic `IXFS` at offset 0; (2) NTFS -- read sector 0 BPB OEM ID bytes 3–10 == `"NTFS    "`; (3) FAT32 -- BPB signature `0x28`/`0x29` at offset 66, `"FAT32   "` at offset 82; (4) exFAT -- OEM ID `"EXFAT   "` at offset 3; (5) ext4 -- read LBA 2 (superblock offset 1024), magic `0xEF53` at offset 56; (6) Btrfs -- read LBA 64 (superblock offset 65536), magic `_BHRfS_M` at offset 0x40; (7) ISO 9660 -- read LBA 16 (PVD offset 32768), bytes 1–5 == `"CD001"`. Drive-letter assignment rule: first IXFS partition → C:; remaining partitions assigned D:, E:, F:… in discovery order (skipping EFI and Logs partitions which keep special handling).

- [ ] `fs_identify_result_t { fs_type_t type; char label[64]; uint64_t total_bytes; uint64_t free_bytes; }` -- returned by each probe attempt
- [ ] `vfs_probe(blkdev_t *dev, char drive_letter)` → read sector 0 (and LBA 2, 16, 64 as needed); try each probe in priority order; on match: call appropriate `fs_init(dev)` + `vfs_mount(letter, driver, root)` + `vfs_probe_registry_write(letter, result)`; return fs_type or `FS_UNKNOWN`
- [ ] `vfs_probe_registry_write(char letter, fs_identify_result_t *r)`: write `HKLM\SYSTEM\Storage\Drive\{letter}\Device (REG_SZ)`, `Filesystem (REG_SZ)`, `Label (REG_SZ)`, `TotalBytes (REG_QWORD)`, `FreeBytes (REG_QWORD)`, `DriveType (REG_DWORD: 2=removable, 3=fixed, 5=cdrom)`
- [ ] `vfs_auto_assign_letters(void)`: enumerate all registered `blkdev` partitions (via `blkdev_iterate()`); assign C: to first IXFS; then assign D:, E:… to remaining non-EFI, non-Logs partitions in discovery order; call `vfs_probe(dev, letter)` for each
- [ ] exFAT probe: if detected, log `[VFS] %c: exFAT detected -- driver not yet loaded (TODO)` and skip (driver comes in a later TODO)
- [ ] ext4/Btrfs probe: same stub log -- detected but not mounted
- [ ] Log: `[VFS] Mounted %c: (%s, "%s", %llu GB)` for each successful mount
- [ ] Commit: `"fs: vfs_probe() -- priority probe chain, drive-letter assignment, Registry population"`

## 2. Boot Mount Sequence Rewrite `[Sonnet]`

Replace the hardcoded `partition_mount_filesystems()` call in `boot_storage.c` with `vfs_auto_assign_letters()`. Preserve existing X: (Logs) and debug-flag detection behaviour.

**Files:** `src/kernel/main/boot_storage.c` (extend), `src/kernel/fs/partition.c` (remove hardcoded mount loop)

> [!NOTE]
> Keep `partition_scan_all()` -- it populates the partition table that `vfs_auto_assign_letters()` iterates. Remove only the mount loop in `partition_mount_filesystems()`; replace its body with a call to `vfs_auto_assign_letters()`. The Logs partition (GPT name `"Logs"` → X:) and EFI partition skip logic must be preserved inside `vfs_auto_assign_letters()`, not removed.

- [ ] Replace `partition_mount_filesystems()` body: call `vfs_auto_assign_letters()` from §1; retire hardcoded IXFS/FAT32/NTFS chains
- [ ] Keep X: Logs partition: in `vfs_auto_assign_letters()`, detect GPT name `"Logs"` → mount as X: before general assignment loop
- [ ] Verify debug-flag detection in `boot_storage.c` (`vfs_is_mounted('X')` check) still works after rewrite
- [ ] Boot log order: C: first, then alphabetically; total mount count logged: `[VFS] Boot mount complete: %u drives`
- [ ] Commit: `"boot: rewire partition mount to vfs_auto_assign_letters()"`

## 3. USB Hot-Plug Volume Arrival `[Opus]`

On USB MSC hot-plug event from the xHCI driver, probe the new block device, auto-assign a drive letter, mount the filesystem, update the File Manager sidebar, and show a desktop toast.

**Files:** `src/kernel/fs/vfs_probe.c` (extend), `src/kernel/main/boot_storage.c` (add hotplug callback), `src/desktop/toast.c` (extend)

> [!NOTE]
> Hot-plug events arrive from `04-drivers-hardware/TODO-10-usb-stack.md §8` as `usb_hotplug_notify(dev, ATTACH)`. This callback must run in a deferred context (DPC or kernel thread), not the xHCI interrupt handler, since `vfs_probe()` does block I/O. After mounting, post a desktop notification via `toast_show("USB drive detected -- %c:\\ (%s, %s, '%s')", letter, size_str, fs_name, label)` with two buttons: `Open` (opens File Manager to that drive) and `Dismiss`.

- [ ] `vfs_hotplug_attach(blkdev_t *dev)`: find next free drive letter (D:–Z:); call `vfs_probe(dev, letter)`; on success: log `[VFS] Hot-plug: mounted %c: (%s)`; call `desktop_sidebar_add_drive(letter)`; show toast
- [ ] `vfs_hotplug_detach(blkdev_t *dev)`: find letter for dev; if mounted and all file handles closed: call `vfs_unmount(letter)`; log `[VFS] Hot-plug: unmounted %c: (device removed)`; call `desktop_sidebar_remove_drive(letter)`; show toast `"Drive removed: %c:\\"`; if handles open: log warning, force unmount after 5 s (timer callback)
- [ ] Register callback: `usb_msc_register_hotplug_cb(vfs_hotplug_attach, vfs_hotplug_detach)` at end of `boot_storage_init()`
- [ ] Toast format: `"USB drive detected -- E:\\ (16.0 GB, exFAT, 'MyDrive')"` with `Open` button → `explorer_open_path("E:\\")`
- [ ] File Manager sidebar: `desktop_sidebar_add_drive(letter)` posts a message to the File Manager's drive-list window; `desktop_sidebar_remove_drive(letter)` removes it
- [ ] Commit: `"fs: USB hot-plug mount -- vfs_hotplug_attach/detach, toast, sidebar, deferred probe"`

## 4. USB Safe Removal `[Sonnet]`

Implement safe-remove: tray icon right-click menu → flush + journal + unmount → USB stack port power-down → toast confirmation.

**Files:** `src/desktop/tray.c` (extend), `src/kernel/fs/vfs_probe.c` (extend)

> [!NOTE]
> Safe-remove sequence: (1) `cache_flush(dev)` (block cache §TODO-01); (2) `vfs_journal_flush(letter)` for NTFS/IXFS (clear dirty flag); (3) `vfs_unmount(letter)`; (4) `usb_msc_power_down(dev)` (port power off); (5) toast `"Safe to remove %c:\\"`. If any file handle is still open after step 1: wait up to 5 s, retrying every 500 ms; if still open at 5 s, force-close handles and proceed. C: (system drive) must be blocked from unmount with an error toast.

- [ ] Tray icon right-click: for each mounted removable drive (DriveType=2 in Registry): add `"Safely Remove %c:\\"` menu item; click → `vfs_safe_remove(letter)`
- [ ] `vfs_safe_remove(letter)`: if `letter == 'C'`: toast error `"Cannot remove the system drive"`; return. Else: flush cache; flush journal; check open handles; if open: start 5 s timeout, show progress toast; force close after timeout; `vfs_unmount(letter)`; `usb_msc_power_down(dev)`; `vfs_probe_registry_delete(letter)`; `desktop_sidebar_remove_drive(letter)`; toast `"Safe to remove %c:\\"` 
- [ ] Force-unmount: close all VFS nodes with `vfs_node_t.ref_count > 0` for the drive; log `[VFS] Force-unmount %c: after timeout -- %u handles closed`
- [ ] Commit: `"fs: USB safe removal -- cache/journal flush, handle drain, force-unmount, port power-off"`

## 5. Manual `mount` / `umount` Shell Commands `[Sonnet]`

Add `mount` and `umount` as built-in shell commands. `mount` with no args prints a drive table. `umount` flushes and unmounts gracefully.

**Files:** `src/shell/cmd_mount.c` (new), `src/shell/shell.c` (register)

> [!NOTE]
> `mount E: /dev/disk1p2 ntfs` syntax: drive letter, device path, optional fs hint. The device path must resolve to a registered `blkdev_t`. If the filesystem hint is given, skip the probe chain and call the named driver directly. `umount` must refuse C: with `STATUS_DEVICE_BUSY` semantics. `mount` with no args iterates all 26 drive slots and prints a table; skip unmounted slots.

- [ ] `mount` (no args): print table header `  Drive  Filesystem  Label            Total     Free    RO`; iterate A:–Z:; for each mounted slot: read Registry `HKLM\SYSTEM\Storage\Drive\{letter}\*`; print row; skip unmounted
- [ ] `mount <letter>: <device> [fs]`: look up `blkdev_get(device)`; if fs hint given call driver directly; else call `vfs_probe(dev, letter)`; print result
- [ ] `umount <letter>:`: refuse C: → print `Cannot unmount system drive`; else call `vfs_safe_remove(letter)` from §4; print `Unmounted %c:`
- [ ] Register in `src/shell/shell.c` dispatch table
- [ ] Commit: `"shell: mount/umount commands -- drive table, probe-mount, safe unmount"`

## 6. Win32 Volume Query APIs `[Sonnet]`

Implement `GetLogicalDrives()`, `GetDriveTypeW()`, `GetVolumeInformationW()`, `SetVolumeLabelW()`, and `QueryDosDeviceW()` backed by the Registry entries written in §1.

**Files:** `src/kernel/sched/syscalls.c` or `src/kernel/win32/volume.c` (new), `include/kernel/win32/volume.h` (new)

> [!NOTE]
> All five functions read from `HKLM\SYSTEM\Storage\Drive\{letter}\` populated by `vfs_probe_registry_write()`. Return values follow the Windows spec exactly. `GetVolumeInformationW`: `FileSystemFlags` should include `FILE_CASE_SENSITIVE_SEARCH (0x1)`, `FILE_UNICODE_ON_DISK (0x4)`, `FILE_PERSISTENT_ACLS (0x8)` for NTFS and IXFS; `MaximumComponentLength = 255`. `QueryDosDeviceW("C:")` returns `\\Device\\HardDisk0\\Partition1` (device path from Registry `Device` key).

- [ ] `GetLogicalDrives()` → `DWORD`: iterate A:–Z: slots; set bit N if `vfs_is_mounted(letter)`; return bitmask
- [ ] `GetDriveTypeW(lpRootPathName)` → `UINT`: extract letter; read `DriveType` from Registry; map: 2→`DRIVE_REMOVABLE`, 3→`DRIVE_FIXED`, 5→`DRIVE_CDROM`; unmounted → `DRIVE_NO_ROOT_DIR`
- [ ] `GetVolumeInformationW(lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize, lpVolumeSerialNumber, lpMaximumComponentLength, lpFileSystemFlags, lpFileSystemNameBuffer, nFileSystemNameSize)`: read label, serial (hash of device path), fs name from Registry; set filesystem flags per fs type
- [ ] `SetVolumeLabelW(lpRootPathName, lpVolumeName)`: call `vfs_set_label(letter, label)` (FS driver vtable); update Registry `Label` key; return `TRUE`/`FALSE`
- [ ] `QueryDosDeviceW(lpDeviceName, lpTargetPath, ucchMax)`: if `lpDeviceName` is `"C:"` etc., return Registry `Device` path; if NULL, enumerate all drive letters
- [ ] Commit: `"win32: volume APIs -- GetLogicalDrives, GetVolumeInformation, SetVolumeLabel, QueryDosDevice"`

## 7. Optical Drive Handling `[Sonnet]`

Auto-detect ATAPI/SCSI CD-ROM drives, assign next drive letter, mount as ISO 9660 (read-only). Implement tray-open command. Add autorun stub (disabled by default).

**Files:** `src/kernel/fs/iso9660.c` (new), `include/kernel/fs/iso9660.h` (new), `src/kernel/drivers/ahci/atapi.c` (extend)

> [!NOTE]
> ISO 9660 probe: read LBA 16, check bytes 1–5 == `"CD001"` and byte 0 == `0x01` (PVD type). Joliet extension: check supplementary PVD (type 0x02) escape sequence `%/E` or `%/@` for Joliet Level 1/2/3 -- use Joliet paths if present (UCS-2 → UTF-8 conversion). UDF detection: check LBA 256 for `"BEA01 "` followed by `"NSR02"` or `"NSR03"` -- stub: detect and log, full UDF deferred.

- [ ] Probe: add ISO 9660 to `vfs_probe()` as step 7; ATAPI drives get `DRIVE_CDROM` type; mount read-only
- [ ] `iso9660_init(blkdev_t *dev)`: read PVD at LBA 16; parse volume space size, logical block size, root directory record; build VFS tree lazily on `readdir()`
- [ ] `iso9660_readdir(node)`: parse ISO 9660 directory records (33 + file_identifier bytes each); prefer Joliet names if supplementary PVD present
- [ ] ATAPI tray: `atapi_eject(dev)` issues `START/STOP UNIT (LOEJ=1, START=0)` SCSI command via AHCI PIO; exposed as shell command `eject D:`
- [ ] Autorun: on disc insert, check `HKLM\SYSTEM\Settings\AutorunEnabled (REG_DWORD)` (default 0); if 1: read `autorun.inf` from root, parse `[autorun] open=` key, spawn process
- [ ] Log: `[VFS] Mounted %c: (ISO9660/Joliet, "%s", read-only)` or `[VFS] Disc inserted in %c: -- autorun disabled`
- [ ] Commit: `"fs: ISO9660/Joliet mount, ATAPI eject, autorun stub (disabled by default)"`

## 8. Volume Control Ioctls `[Sonnet]`

Implement `NtFsControlFile` FSCTL codes needed by backup software and system tools: `FSCTL_IS_VOLUME_DIRTY`, `FSCTL_LOCK_VOLUME`, `FSCTL_UNLOCK_VOLUME`, `FSCTL_DISMOUNT_VOLUME`. Add `ixfs_snapshot_create()` as a VSS shadow copy stub.

**Files:** `src/kernel/fs/vfs.c` (extend), `src/kernel/sched/syscalls.c` (extend)

> [!NOTE]
> `FSCTL_IS_VOLUME_DIRTY (0x90078)`: returns `IOCTL_VOLUME_DIRTY` if `vol->volume_dirty`; used by `chkdsk`. `FSCTL_LOCK_VOLUME (0x90018)`: marks volume locked (refuses new opens); used before backup image. `FSCTL_UNLOCK_VOLUME (0x9001C)`: clears lock. `FSCTL_DISMOUNT_VOLUME (0x90020)`: flushes + marks dismounted; next access re-mounts. Implement as a dispatch in `vfs_ioctl(letter, fsctl_code, in_buf, out_buf)`.

- [ ] `vfs_ioctl(char letter, uint32_t code, void *in, uint32_t in_len, void *out, uint32_t *out_len)`: dispatch on code
- [ ] `FSCTL_IS_VOLUME_DIRTY`: call `vfs_driver_is_dirty(letter)` (NTFS: `vol->volume_dirty`; IXFS: `ixfs_is_dirty()`); write `DWORD dirty_flags` to output buffer
- [ ] `FSCTL_LOCK_VOLUME`: set `vol->locked = 1`; refuse all new `vfs_open()` for that letter; return `STATUS_SUCCESS` if no handles open, `STATUS_ACCESS_DENIED` if handles open
- [ ] `FSCTL_UNLOCK_VOLUME`: clear `vol->locked`
- [ ] `FSCTL_DISMOUNT_VOLUME`: `cache_flush(dev)` + journal checkpoint + `vfs_unmount(letter)` + mark slot as `DISMOUNTED` (re-probe on next access)
- [ ] `ixfs_snapshot_create(vol, &snapshot_id)`: stub -- log `[IXFS] VSS shadow copy requested (not yet implemented)`, return `STATUS_NOT_IMPLEMENTED`; wire up when IXFS COW snapshotting is added
- [ ] Syscall: `NtFsControlFile(handle, ioctl, ...)` → resolve handle to drive letter → `vfs_ioctl()`
- [ ] Commit: `"fs: FSCTL_IS_VOLUME_DIRTY/LOCK/UNLOCK/DISMOUNT ioctls, ixfs_snapshot_create stub"`

---

## OS Comparison


| ⭐  | Feature                                                                      | 🪟 Win11                                                     | 🐧 Linux                                                     | 🚀 Impossible OS                                                             |
| --- | ---------------------------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------ | ---------------------------------------------------------------------------- |
| ⭐  | In-kernel probe chain + dynamic drive-letter assignment + Registry write     | ⚠️ `mountmgr.sys` assigns letters; probe in                  | ⚠️ `udev` rules in user space;                               | ⬜ §1 -- priority probe: IXFS→NTFS→FAT32→exFAT→ext4→Btrfs→ISO; auto-assign + |
| 💎  | Boot-time partition scan + auto-mount of all filesystems                     | ✅ `IoInitSystem`, `mountmgr.sys`; all partitions enumerated | ✅ `init` + `udev` + `/etc/fstab`;                           | ⚠️ §2 -- Partial -- ; replaces hardcoded                                     |
| ⭐  | USB hot-plug: in-kernel probe + auto-assign + desktop toast + sidebar update | ⚠️ `mountmgr.sys` mount + Explorer notification              | ⚠️ `udev` → `udisks2` daemon →                               | ⬜ §3 -- single kernel path: probe→mount→Registry→toast→sidebar, no          |
| 💎  | USB safe removal                                                             | ✅ `SafelyRemoveHardware` tray; Safely Remove finalizes      | ✅ `umount` + `udisksctl power-off`; requires                | ⬜ §4 -- tray right-click → cache_flush +                                    |
| 💎  | `mount` / `umount` shell commands                                            | ✅ `mountvol.exe`; no `mount` command natively               | ✅ `mount`/`umount`; standard POSIX tools                    | ⬜ §5 -- `mount` (table) + `mount E:                                         |
| 💎  | Win32 volume query APIs                                                      | ✅ Full Win32 API; backed by                                 | ❌ Not applicable (POSIX `statfs`/`statvfs` equivalents)     | ⬜ §6 -- Registry-backed implementations of all five                         |
| 💎  | Optical drive                                                                | ✅ `cdfs.sys` ISO 9660 + Joliet;                             | ✅ `isofs.ko`; ISO 9660/Joliet/Rock Ridge; `eject`           | ⬜ §7 -- `iso9660.c`, Joliet, ATAPI eject command,                           |
| 💎  | `FSCTL_IS_VOLUME_DIRTY` / `LOCK` / `UNLOCK` / `DISMOUNT` ioctls              | ✅ `NtFsControlFile`; all codes; used by                     | ✅ `ioctl(BLKFLSBUF)`, `FITRIM`, etc.; `LOCK`/`DISMOUNT` via | ⬜ §8 -- `vfs_ioctl()` dispatcher, four FSCTL codes,                         |

> **After §1–8:** Impossible OS has the most unified volume management of the three platforms. The `⭐` architectural differentiator is the in-kernel probe chain (§1) and the hot-plug handler (§3): Windows delegates mount decisions to `mountmgr.sys` + AutoPlay service (user space); Linux delegates to `udev` + `udisks2` (both user space). Impossible OS handles the entire flow -- identify filesystem, assign letter, write Registry, mount, notify desktop -- inside a single kernel path with no user-space daemon in the critical path.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot: serial log shows `[VFS] Mounted C: (IXFS, ...)`, `[VFS] Mounted D: (NTFS, ...)` for each partition; `[VFS] Boot mount complete: N drives`
- [ ] `mount` (no args) in shell: prints drive table with all mounted drives, filesystems, labels, sizes
- [ ] USB hot-plug (QEMU `usb-storage` device): serial shows `[VFS] Hot-plug: mounted E: (FAT32)`; desktop toast appears; File Manager sidebar updates
- [ ] USB safe removal: tray right-click → `Safely Remove E:` → serial shows flush + unmount + `[VFS] Hot-plug: unmounted E:`; toast `"Safe to remove E:\"`
- [ ] `umount E:` shell: unmounts; `umount C:` → prints error `Cannot unmount system drive`
- [ ] `GetLogicalDrives()` returns correct bitmask after mount/unmount sequence
- [ ] `GetVolumeInformationW(L"C:\\")` returns `"IXFS"` as filesystem name and correct label
- [ ] `SetVolumeLabelW(L"D:\\", L"NewLabel")` → `mount` command shows updated label; NTFS label persists across remount
- [ ] Optical: QEMU `ide-cd` device → serial shows `[VFS] Mounted F: (ISO9660/Joliet, ...)`; `eject F:` command opens tray
- [ ] `FSCTL_IS_VOLUME_DIRTY` on C: returns 0 (clean); after simulated dirty → returns 1
- [ ] `FSCTL_LOCK_VOLUME` on E: → subsequent `vfs_open("E:\\...")` returns error; `FSCTL_UNLOCK_VOLUME` → access restored
- [ ] Commit: `"fs: volume manager -- probe chain, auto-mount, hot-plug, safe remove, Win32 APIs, FSCTL ioctls"`
