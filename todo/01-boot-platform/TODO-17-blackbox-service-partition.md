# TODO-17 -- BlackBox Service Partition

> **Goal:** Add a 128 MiB FAT32 "BlackBox" partition to the GPT disk layout, mounted as `X:\`. All kernel logs, crash dumps, boot timelines, diagnostic snapshots, and portable tools live here -- separate from the IXFS system volume. FAT32 gives crash resilience (survives IXFS corruption), cross-platform readability (Windows/Linux/macOS mount it natively), and clean separation of OS files from diagnostic data.

> [!IMPORTANT]
> **Current state:** 2-partition GPT (64 MiB EFI + ~444 MiB IXFS). All logs on `C:\Impossible\System\Logs\` (IXFS). Old `X:\` FAT32 log partition was removed when IXFS write support landed. This TODO restores a service partition with a proper name, structure, and expanded scope.

## Inputs

- [`tools/make-system-disk.c`](../../tools/make-system-disk.c) -- GPT disk image builder (2-partition layout)
- [`scripts/build.sh`](../../scripts/build.sh) -- build pipeline, disk formatting
- [`src/kernel/fs/partition.c`](../../src/kernel/fs/partition.c) -- GPT partition discovery, drive letter assignment
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c) -- log file creation and flush paths
- [`include/kernel/klog.h`](../../include/kernel/klog.h) -- `KLOG_DIR` macro (currently `C:\Impossible\System\Logs\`)
- -> XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` S4 -- FAT32 LFN write (prerequisite for lowercase filenames)
- -> XREF: `01-boot-platform/TODO-03-boot-device-discovery.md` -- partition GUID validation
- -> XREF: `02-kernel-core/TODO-02-system-logging.md` -- klog output paths, log rotation policy
- -> XREF: `02-kernel-core/TODO-16-crash-dump-generation.md` -- MEMORY.DMP + minidump writer targets X:\Crash\
- -> XREF: `14-host-tools/TODO-08-blackbox-log-extractor.md` -- host-side CLI to extract/view logs from disk images
- -> XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` S6 -- FAT32 fsck validates BlackBox integrity
- -> XREF: `01-boot-platform/TODO-14-ab-boot-rollback.md` -- A/B dual-slot layout adds 2 more partitions; BlackBox must coexist

## Outcome

- 3-partition GPT: EFI (64 MiB) + BlackBox (128 MiB FAT32) + IXFS (~316 MiB).
- BlackBox partition mounts as `X:\` by GPT name match, not by partition index.
- All kernel logs, crash dumps, boot timelines, and diagnostics write to `X:\` instead of `C:\`.
- Directory structure created on first boot: `Logs\`, `Boot\`, `Crash\`, `Perf\`, `Diag\`, `Tools\`.
- Host-side tools (SDK) can mount and read BlackBox from the raw disk image.
- Windows auto-mounts BlackBox when the disk image or real disk is attached (Microsoft Basic Data GUID).
- Log space managed automatically: boot session pruning, rotation cleanup, configurable quota.
- FAT32 dirty-bit checked on mount; optional fsck for crash recovery integrity.
- WER-style crash reports staged in `X:\Crash\WER\` as cross-platform-readable JSON.
- A/B dual-slot boot layout compatible via `--ab` build flag.

## Implementation Order

| S  | Order | Deliverable                                          | Depends On        | Status |
|----|:-----:|------------------------------------------------------|-------------------|:------:|
| 💎 |   1   | Disk tooling -- add BlackBox partition to GPT layout  | --                |  [ ]   |
| 💎 |   2   | Build pipeline -- format and populate BlackBox        | S1                |  [ ]   |
| 💎 |   3   | Kernel mount -- discover "BlackBox" GPT name, mount X:| S1                |  [ ]   |
| 💎 |   4   | Directory skeleton -- create dirs on first boot       | S3                |  [ ]   |
| 💎 |   5   | Klog migration -- move all log output to X:\Logs\     | S3, S4            |  [ ]   |
| 💎 |   6   | Boot logs -- per-boot session files to X:\Boot\       | S3, S4            |  [ ]   |
| 💎 |   7   | Crash dump path -- crash_recovery.log to X:\Crash\    | S3, S4            |  [ ]   |
| 💎 |   8   | Perf and diag -- boot-profile, hwdump to X:\Perf\Diag\| S3, S4           |  [ ]   |
| 💎 |   9   | Host tools -- SDK reads BlackBox from disk image      | S1, S2            |  [ ]   |
| 💎 |  10   | Disk space management -- log aging, quota, cleanup    | S5                |  [ ]   |
| 💎 |  11   | FAT32 volume label -- set "BLACKBOX" at format time   | S2                |  [ ]   |
| 💎 |  12   | Partition health -- fsck on mount, dirty-bit check    | S3, T04 S6        |  [ ]   |
| ⭐ |  13   | WER staging area -- error reports in X:\Crash\WER\    | S4, S7            |  [ ]   |
| S  |  14   | A/B layout compatibility -- 4+ partition coexistence  | S1                |  [ ]   |
| S  |  15   | Boot platform TODO updates -- XREFs and domain sync   | S1-S14            |  [ ]   |

> 💎 = parity -- Windows has a recovery/diagnostic partition; Linux has /var/log separation.
> S = scope -- internal project hygiene.

---

## 1. Disk Tooling -- Add BlackBox Partition to GPT Layout

Modify `make-system-disk.c` to create a 3-partition GPT: EFI + BlackBox + IXFS.

- [ ] Add BlackBox partition entry between EFI and IXFS in the GPT partition array
- [ ] GPT type GUID: Microsoft Basic Data (`EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`) -- Windows auto-mounts
- [ ] GPT partition name: `"BlackBox"` (UTF-16LE, stored in partition entry bytes 56-127)
- [ ] Size: 128 MiB (262144 sectors at 512 B/sec), starting at next 1 MiB-aligned LBA after EFI
- [ ] IXFS partition shifts to start after BlackBox (reduced to ~316 MiB on a 512 MiB disk)
- [ ] Update `GPT_NUM_ENTRIES` if needed (currently 128, sufficient)
- [ ] Verify: backup GPT header and entry array at end of disk still correct
- [ ] Commit: `"tools: add BlackBox 128 MiB FAT32 partition to GPT layout"`

**Test checkpoint:** `make-system-disk` produces a 3-partition disk image. `fdisk -l build/system-disk.img` shows EFI (64 MiB) + BlackBox (128 MiB) + IXFS (~316 MiB). BlackBox partition type is Microsoft Basic Data. GPT partition name is "BlackBox".

## 2. Build Pipeline -- Format and Populate BlackBox

Update `scripts/build.sh` and Makefile to format the BlackBox partition as FAT32 and create the initial directory structure.

- [ ] After `make-system-disk` creates the image, format the BlackBox partition with `mkfs.fat -F 32`
- [ ] Calculate correct byte offset for `mkfs.fat --offset` based on BlackBox start LBA
- [ ] Create initial directory skeleton on the formatted partition: `Logs/`, `Boot/`, `Crash/`, `Perf/`, `Diag/`, `Tools/`
- [ ] Use `mmd` (mtools) or a custom script to create directories on the FAT32 image
- [ ] Verify: mount the partition in Linux and confirm all 6 directories exist
- [ ] Commit: `"build: format BlackBox FAT32 partition and create directory skeleton"`

**Test checkpoint:** `bash scripts/build.sh clean` produces a disk with BlackBox formatted as FAT32. `mcopy -i build/system-disk.img@@<offset> ::/ /tmp/bb-test` lists `Logs/`, `Boot/`, `Crash/`, `Perf/`, `Diag/`, `Tools/`.

## 3. Kernel Mount -- Discover "BlackBox" GPT Name, Mount as X:\

Update `partition.c` to recognize the "BlackBox" GPT partition name and mount it as `X:\`.

- [ ] In `partition_mount_filesystems()`: check `pi->gpt_name` for `"BlackBox"` (case-insensitive)
- [ ] If found and `fs_type == PART_FS_FAT32`: mount as drive letter `'X'` instead of the next sequential letter
- [ ] Skip BlackBox from the sequential `D:`, `E:`, ... letter assignment
- [ ] Log: `klog(LOG_INFO, "blk", "BlackBox partition mounted as X:\\")`
- [ ] Graceful fallback: if no "BlackBox" partition found, logs continue to `C:\Impossible\System\Logs\` (existing behavior)
- [ ] Commit: `"kernel: mount BlackBox partition as X:\\ by GPT name"`

**Test checkpoint:** Serial log shows `"BlackBox partition mounted as X:\"`. `X:\` is accessible via VFS. Other FAT32 partitions still get `D:`, `E:`, etc. On disks without BlackBox, logs go to `C:\` as before. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

**Regression risk:** Partition mount order change could break C:\ IXFS mount. Rollback: revert the `gpt_name` check in `partition_mount_filesystems()`.

## 4. Directory Skeleton -- Create Dirs on First Boot

Ensure the BlackBox directory structure exists on first boot and after format.

- [ ] After `X:\` mount, check for `X:\Logs\` existence via `vfs_open("X:\\Logs", ...)`
- [ ] If missing, create all 7 directories: `Logs`, `Boot`, `Crash`, `Crash\WER`, `Perf`, `Diag`, `Tools`
- [ ] Use existing `vfs_create(parent, name, VFS_DIRECTORY)` -- already works on FAT32
- [ ] Log each directory creation: `klog(LOG_INFO, "boot", "BlackBox: created X:\\%s", dir_name)`
- [ ] Idempotent: directories already existing is not an error
- [ ] Commit: `"kernel: create BlackBox directory skeleton on first boot"`

**Test checkpoint:** First boot after clean build: serial shows 6 `"BlackBox: created X:\..."` messages. Second boot: no creation messages (directories already exist). Verify on QEMU WHPX, TCG.

## 5. Klog Migration -- Move All Log Output to X:\Logs\

Redirect all kernel log output from `C:\Impossible\System\Logs\` to `X:\Logs\`.

- [ ] Change `KLOG_DIR` in `klog.h` from `"C:\\Impossible\\System\\Logs\\"` to `"X:\\Logs\\"`
- [ ] Update `klog_disk.c` `ensure_log_dirs()`: use new `KLOG_DIR` path
- [ ] Update all hardcoded log paths in `klog_disk.c`: `kernel.log`, `boot.log`, `network.log`, `fs.log`, `mm.log`, `drivers.log`, `security.log`, `events.jsonl`
- [ ] Fallback: if `X:\` not mounted (no BlackBox partition), fall back to `C:\Impossible\System\Logs\` -- check `vfs_stat("X:\\")` at `klog_disk_enable()` time
- [ ] Update log rotation paths to use `KLOG_DIR`
- [ ] Commit: `"kernel: migrate klog output from C:\\ to X:\\Logs\\"`

**Test checkpoint:** Serial shows `"klog: writing to X:\Logs\Serial_YYMMDDN.log"`. All subsystem logs appear under `X:\Logs\`. On a disk without BlackBox, fallback message: `"klog: BlackBox not mounted, using C:\\"`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

**Regression risk:** If klog_disk_enable() runs before X:\ is mounted, log writes fail silently. The fallback check must happen at enable time, not at init time.

## 6. Boot Logs -- Per-Boot Session Files to X:\Boot\

Move the per-boot numbered session logs and boot timeline JSON to `X:\Boot\`.

- [ ] Update `klog_disk.c` FAT32 boot log path from the old `X:\BOOT_NNN.LOG` pattern to `X:\Boot\YYMMDDN.LOG`
- [ ] Update `boot_timing_write_report()` to write `X:\Boot\YYMMDDN.json` (boot timeline)
- [ ] Update `boot_profile_write()` to write `X:\Perf\boot-profile.log`
- [ ] Keep the same date-stamped naming scheme: `YYMMDDN` where N increments per boot that day
- [ ] Commit: `"kernel: move per-boot session logs to X:\\Boot\\"`

**Test checkpoint:** After boot, `X:\Boot\` contains `26040501.LOG` and `26040501.json`. Serial log path shows `X:\Boot\` prefix. Verify on QEMU WHPX, TCG.

## 7. Crash Dump Path -- crash_recovery.log to X:\Crash\

Move crash-persistent log recovery output to `X:\Crash\`.

- [ ] Update `klog_crash_write_to_disk()` in `klog.c` to write `X:\Crash\crash_recovery.log` instead of `C:\Impossible\System\Logs\crash_recovery.log`
- [ ] Future: `MEMORY.DMP` crash dumps (TODO-16) will also target `X:\Crash\`
- [ ] Fallback: if `X:\` not mounted, write to `C:\` as before
- [ ] Commit: `"kernel: move crash recovery log to X:\\Crash\\"`

**Test checkpoint:** Force panic with `crash_test=1`; on next boot, `crash_recovery.log` appears in `X:\Crash\`, not `C:\Impossible\System\Logs\`. Verify on QEMU WHPX, TCG.

## 8. Perf and Diag -- boot-profile, hwdump to X:\Perf\ and X:\Diag\

Move performance and diagnostic outputs to their BlackBox directories.

- [ ] Update `hw_dump.c` output path to `X:\Diag\hwdump.txt`
- [ ] Update POST code history log to `X:\Diag\postcode.log`
- [ ] Update boot performance comparison to `X:\Perf\boot-profile.log`
- [ ] Future: ETW trace session exports will go to `X:\Perf\traces\`
- [ ] Commit: `"kernel: move diagnostic and perf output to X:\\Diag\\ and X:\\Perf\\"`

**Test checkpoint:** After boot, `X:\Diag\hwdump.txt` and `X:\Perf\boot-profile.log` exist with valid content. Verify on QEMU WHPX, TCG.

## 9. Host Tools -- SDK Reads BlackBox from Disk Image

Update host-side tools to locate and read the BlackBox partition from raw disk images.

- [ ] Update `tools/ixfs-mount/` (or equivalent SDK tools) to recognize the 3-partition layout
- [ ] Add a `blackbox-read` host utility or script: extracts `X:\Logs\`, `X:\Crash\`, etc. from the raw disk image
- [ ] Calculate BlackBox byte offset from GPT entries (same approach as `mkfs-ixfs --offset`)
- [ ] Document: how to mount BlackBox from a disk image on Linux (`mount -o loop,offset=<N>`)
- [ ] Commit: `"tools: SDK reads BlackBox partition from disk images"`

**Test checkpoint:** `bash scripts/tools/read-blackbox.sh build/system-disk.img` extracts logs to a local directory. `mount` on Linux at the correct offset shows FAT32 with the directory structure.

## 10. Disk Space Management -- Log Aging, Quota, Cleanup

128 MiB is generous but finite. Without space management, logs eventually fill the partition and writes fail silently. Windows limits CBS.log to ~20 MB with rotation; Linux logrotate enforces per-file and total quotas. BlackBox needs the same discipline.

- [ ] At `klog_disk_enable()` time, check BlackBox free space via `fat32_get_free_clusters(vol)`
- [ ] If free space < 10% of partition size: run cleanup before first flush
- [ ] Cleanup policy: delete oldest files in `Boot\` (keep last 10 boot sessions), then oldest rotated logs in `Logs\` (`.1`, `.2`, `.3`)
- [ ] Registry-configurable: `HKLM\SYSTEM\BlackBox\MaxBootSessions` (default: 10), `HKLM\SYSTEM\BlackBox\MinFreeMiB` (default: 16)
- [ ] Log when cleanup runs: `klog(LOG_WARN, "boot", "BlackBox: cleanup freed %u KiB (%u files removed)")`
- [ ] If cleanup cannot free enough space: log `LOG_ERROR` and fall back to C:\ for this boot session
- [ ] Commit: `"kernel: BlackBox disk space management -- log aging and quota enforcement"`

**Test checkpoint:** Fill BlackBox with dummy files until < 10% free. Boot -> serial shows cleanup message with freed space. Boot sessions beyond 10 are pruned. Verify on QEMU WHPX, TCG.

## 11. FAT32 Volume Label -- Set "BLACKBOX" at Format Time

Windows and Linux identify FAT32 volumes by their 11-character volume label (stored in BPB and root directory). Setting it at format time ensures `vol` command and Disk Management show "BLACKBOX" instead of "NO NAME".

- [ ] Pass `-n BLACKBOX` to `mkfs.fat` in `scripts/build.sh` when formatting the BlackBox partition
- [ ] Verify: `mlabel -i build/system-disk.img@@<offset> -s ::` shows `BLACKBOX`
- [ ] In kernel: `fat32_init()` reads volume label from BPB -- log it: `klog(LOG_INFO, "blk", "BlackBox: volume label \"%s\"", vol->label)`
- [ ] Commit: `"build: set FAT32 volume label BLACKBOX at format time"`

**Test checkpoint:** `mlabel` shows BLACKBOX. Windows Disk Management shows "BLACKBOX (X:)" when disk is attached. Serial shows volume label on mount.

## 12. Partition Health -- fsck on Mount, Dirty-Bit Check

FAT32 has a "dirty" bit (byte 0x41 in BPB, bit 0 of the word). If the OS crashed mid-write, the dirty bit is set. Check it on mount and optionally run fsck. (-> XREF: TODO-04 S6 -- FAT32 fsck implementation)

- [ ] At BlackBox mount time: read FAT32 dirty bit from BPB
- [ ] If dirty: `klog(LOG_WARN, "blk", "BlackBox: partition dirty -- possible corruption from previous crash")`
- [ ] If dirty and `fat32_fsck()` is available (TODO-04 S6): run read-only scan, log results
- [ ] Clear dirty bit after successful mount (standard FAT32 behavior)
- [ ] Set dirty bit on mount, clear on clean unmount/shutdown
- [ ] Commit: `"kernel: BlackBox FAT32 dirty-bit check and optional fsck on mount"`

**Test checkpoint:** Force unclean shutdown (kill QEMU mid-write). Next boot: serial shows "partition dirty" warning. After clean shutdown: no warning. Verify on QEMU WHPX, TCG.

**Regression risk:** fsck on mount adds boot latency. Bound to max 2 seconds; skip if partition is clean. Rollback: disable fsck call, keep dirty-bit logging only.

## 13. WER Staging Area -- Error Reports in X:\Crash\WER\

Windows Error Reporting (WER) stages error reports in `C:\ProgramData\Microsoft\Windows\WER\` before upload. Impossible OS can stage structured error reports for crashed processes in `X:\Crash\WER\` -- enabling post-mortem analysis without a network connection.

> [!TIP]
> Neither Win11 WER nor Linux apport stages crash reports on a separate cross-platform-readable partition. BlackBox WER reports are FAT32-readable by any OS -- plug the disk into any machine and read the crash context.

- [ ] Create `X:\Crash\WER\` directory in the skeleton (S4)
- [ ] When a user process crashes (unhandled exception): write a structured report to `X:\Crash\WER\PID_YYYYMMDD_HHMMSS.json`
- [ ] Report format: `{"pid":N,"name":"app.exe","exception":N,"rip":"0xNNN","stack":["0xN",...],"timestamp":N}`
- [ ] Limit: keep last 50 reports, delete oldest when exceeded
- [ ] Future: integrate with TODO-16 minidump writer for full crash context
- [ ] Commit: `"kernel: WER-style crash report staging in X:\\Crash\\WER\\"`

**Test checkpoint:** Trigger a user-mode crash (NULL deref in cmd.exe test). `X:\Crash\WER\` contains a JSON report with PID, exception code, and stack trace. Verify on QEMU WHPX, TCG.

## 14. A/B Layout Compatibility -- 4+ Partition Coexistence

TODO-14 (A/B dual-slot boot) defines: EFI + Slot A IXFS + Slot B IXFS. With BlackBox, the layout becomes 4 partitions: EFI + BlackBox + Slot A + Slot B. Ensure `make-system-disk.c` supports both layouts via a build flag.

- [ ] Add `--ab` flag to `make-system-disk`: produces 4-partition layout (EFI + BlackBox + Slot A + Slot B)
- [ ] Default (no flag): 3-partition layout (EFI + BlackBox + IXFS)
- [ ] BlackBox size stays 128 MiB in both layouts; IXFS slots split remaining space
- [ ] Kernel partition scanner: mount BlackBox as X:\ regardless of partition count
- [ ] Document: partition layout variants in CLAUDE.md
- [ ] Commit: `"tools: make-system-disk A/B layout with BlackBox partition"`

**Test checkpoint:** `make-system-disk --ab` produces 4-partition image. `fdisk -l` shows EFI + BlackBox + 2x IXFS. Kernel boots and mounts X:\ from either layout. Verify on QEMU WHPX.

## 15. Boot Platform TODO Updates -- XREFs and Domain Sync

Update cross-references across affected TODOs.

- [ ] Update `TODO-02-system-logging.md`: change `KLOG_DIR` references, add XREF to this TODO
- [ ] Update `TODO-03-boot-device-discovery.md`: note 3-partition layout
- [ ] Update `TODO-16-crash-dump-generation.md`: crash dump target is now `X:\Crash\`
- [ ] Update `CLAUDE.md` if partition layout is mentioned
- [ ] Update domain `INDEX.md` files as needed
- [ ] Commit: `"docs: update XREFs for BlackBox partition migration"`

**Test checkpoint:** All referenced TODO files have correct XREFs. No stale `C:\Impossible\System\Logs\` references remain in active TODO files.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_blackbox()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).

- [ ] Create `src/kernel/test/test_blackbox.c` with:
  - `X:\` is mounted (vfs_stat returns success) -- or skip if no BlackBox partition
  - `X:\Logs\` directory exists and is openable
  - `X:\Boot\` directory exists
  - `X:\Crash\` directory exists
  - `X:\Crash\WER\` directory exists
  - `X:\Perf\` directory exists
  - `X:\Diag\` directory exists
  - `X:\Tools\` directory exists
  - `KLOG_DIR` starts with `"X:\\Logs\\"` (or `"C:\\"` fallback)
  - FAT32 volume label is "BLACKBOX" (read from BPB)
  - BlackBox free space > 0 (partition is not full)
- [ ] Register in `test_runner_init()`: `test_register_blackbox()`
- [ ] Commit: `"test: add BlackBox partition tests"`

---

## OS Comparison

| S  | Feature                    | Win11                      | Linux                      | Impossible OS                       |
|----|----------------------------|----------------------------|----------------------------|-------------------------------------|
| 💎 | Separate log partition     | ✅ Recovery + WinRE        | ⚠️ /var/log on root        | ⬜ S1-S3 -- X:\ BlackBox FAT32      |
| 💎 | Cross-platform readable    | ✅ NTFS (with drivers)     | ✅ ext4 (with drivers)     | ⬜ S1 -- FAT32 universal             |
| 💎 | Crash dump isolation       | ✅ C:\Windows\MEMORY.DMP   | ✅ /var/crash              | ⬜ S7 -- X:\Crash\                   |
| ⭐ | Per-boot log files         | ❌ Not built-in            | ❌ Not built-in            | ⬜ S6 -- X:\Boot\YYMMDDN.LOG        |
| ⭐ | Boot timeline JSON         | ❌ Not built-in            | ❌ Not built-in            | ⬜ S6 -- X:\Boot\YYMMDDN.json       |
| 💎 | Auto-mount on host         | ✅ Windows assigns letter  | ✅ udisks2 auto-mount      | ⬜ S1 -- Basic Data GUID            |
| 💎 | Volume label               | ✅ NTFS volume label       | ✅ e2label / fatlabel      | ⬜ S11 -- "BLACKBOX" FAT32 label    |
| S  | Named partition discovery  | ✅ Volume label match      | ✅ LABEL= in fstab         | ⬜ S3 -- GPT name "BlackBox"        |
| 💎 | Structured diagnostics dir | ⚠️ Scattered in C:\Windows | ⚠️ /var/log + /sys         | ⬜ S4 -- organized Logs/Boot/Crash/  |
| 💎 | Log space management       | ✅ CBS.log 20 MB cap       | ✅ logrotate + journald    | ⬜ S10 -- aging + quota + cleanup   |
| 💎 | Dirty-bit / fsck on mount  | ✅ chkdsk on dirty FAT32   | ✅ fsck.fat on mount       | ⬜ S12 -- dirty-bit + optional fsck |
| ⭐ | Cross-platform WER staging | ⚠️ WER on NTFS only        | ⚠️ apport on ext4 only    | ⬜ S13 -- FAT32 WER JSON reports    |
| 💎 | A/B + diagnostic coexist   | ❌ Recovery only           | ⚠️ A/B without diag part  | ⬜ S14 -- 4-partition layout        |

> After S1-S9, Impossible OS has a dedicated diagnostic partition more organized than both Windows (scattered C:\Windows files) and Linux (everything in /var/log). FAT32 universality means any OS can read the flight recorder.
> S10-S12 add production-grade space management and partition health -- matching Win11 chkdsk and Linux logrotate.
> S13 (WER staging on FAT32) is a competitive edge -- neither Win11 nor Linux stages crash reports on a universally readable partition.

## Verification

- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===` with 3-partition disk
- [ ] QEMU WHPX: serial shows `"BlackBox partition mounted as X:\"` and `"klog: writing to X:\Logs\..."`
- [ ] QEMU TCG: same as WHPX
- [ ] VirtualBox: boot completes, X:\ accessible
- [ ] Bare metal: BlackBox partition visible in Windows Disk Management with drive letter
- [ ] Host Linux: `mount -o loop,offset=<N> build/system-disk.img /mnt` shows FAT32 with Logs/Boot/Crash/Perf/Diag/Tools
- [ ] Crash test: `crash_test=1` -> next boot recovery log appears in `X:\Crash\`
- [ ] No-BlackBox fallback: remove BlackBox partition from disk -> logs go to `C:\` with warning
- [ ] Commit: `"kernel: BlackBox service partition verified -- 3-partition GPT, X:\\ mount, log migration"`
