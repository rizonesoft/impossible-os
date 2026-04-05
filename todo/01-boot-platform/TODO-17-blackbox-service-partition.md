# TODO-17 -- BlackBox Service Partition

> **Goal:** Add a 128 MiB FAT32 "BlackBox" partition to the GPT disk layout, mounted as `X:\`. All kernel logs, crash dumps, boot timelines, diagnostic snapshots, and portable tools live here -- separate from the IXFS system volume. FAT32 gives crash resilience (survives IXFS corruption), cross-platform readability (Windows/Linux/macOS mount it natively), and clean separation of OS files from diagnostic data.

> [!IMPORTANT]
> **Current state:** 3-partition GPT (64 MiB EFI + 128 MiB BlackBox FAT32 + ~316 MiB IXFS). All logs, crash reports, boot timelines, and diagnostics write to `X:\` (BlackBox). Falls back to `C:\Impossible\System\Logs\` if BlackBox is absent. A/B dual-slot layout supported via `--ab` flag (4 partitions).

## Inputs

- [`tools/make-system-disk.c`](../../tools/make-system-disk.c) -- GPT disk image builder (3-partition default, 4-partition A/B)
- [`scripts/build.sh`](../../scripts/build.sh) -- build pipeline, disk formatting
- [`src/kernel/fs/partition.c`](../../src/kernel/fs/partition.c) -- GPT partition discovery, BlackBox X:\ mount by name
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c) -- log file creation and flush paths
- [`include/kernel/klog.h`](../../include/kernel/klog.h) -- runtime `klog_dir` global (resolved to `X:\Logs\` or C:\ fallback)
- -> XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` §4 -- FAT32 LFN write (prerequisite for lowercase filenames)
- -> XREF: `01-boot-platform/TODO-03-boot-device-discovery.md` -- partition GUID validation
- -> XREF: `02-kernel-core/TODO-02-system-logging.md` -- klog output paths, log rotation policy
- -> XREF: `02-kernel-core/TODO-16-crash-dump-generation.md` -- MEMORY.DMP + minidump writer targets X:\Crash\
- -> XREF: `14-host-tools/TODO-08-blackbox-log-extractor.md` -- host-side CLI to extract/view logs from disk images
- -> XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` §6 -- FAT32 fsck validates BlackBox integrity
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

| ⭐ | Order | Deliverable                                            | Depends On       | Status |
|----|:-----:|-------------------------------------------------------|-------------------|:------:|
| 💎 |   1   | Disk tooling -- add BlackBox partition to GPT layout  | --                |  [x]   |
| 💎 |   2   | Build pipeline -- format and populate BlackBox        | §1                |  [x]   |
| 💎 |   3   | Kernel mount -- discover "BlackBox" GPT name, mount X:| §1                |  [x]   |
| 💎 |   4   | Directory skeleton -- create dirs on first boot       | §3                |  [x]   |
| 💎 |   5   | Klog migration -- move all log output to X:\Logs\     | §3, §4            |  [x]   |
| 💎 |   6   | Boot logs -- per-boot session files to X:\Boot\       | §3, §4            |  [x]   |
| 💎 |   7   | Crash dump path -- crash_recovery.log to X:\Crash\    | §3, §4            |  [x]   |
| 💎 |   8   | Perf and diag -- boot-profile, hwdump to X:\Perf\Diag\| §3, §4            |  [x]   |
| 💎 |   9   | Host tools -- SDK reads BlackBox from disk image      | §1, §2            |  [x]   |
| 💎 |  10   | Disk space management -- log aging, quota, cleanup    | §5                |  [x]   |
| 💎 |  11   | FAT32 volume label -- set "BLACKBOX" at format time   | §2                |  [x]   |
| 💎 |  12   | Partition health -- fsck on mount, dirty-bit check    | §3, T04 §6        |  [x]   |
| ⭐ |  13   | WER staging area -- error reports in X:\Crash\WER\    | §4, §7            |  [x]   |
| 💎 |  14   | A/B layout compatibility -- 4+ partition coexistence  | §1                |  [x]   |
| 💎 |  15   | Boot platform TODO updates -- XREFs and domain sync   | §1-§14            |  [x]   |

> 💎 = parity -- Windows has a recovery/diagnostic partition; Linux has /var/log separation.
> S = scope -- internal project hygiene.

---

## 1. Disk Tooling -- Add BlackBox Partition to GPT Layout

Modify `make-system-disk.c` to create a 3-partition GPT: EFI + BlackBox + IXFS.

- [x] Add BlackBox partition entry between EFI and IXFS in `make-system-disk.c` GPT partition array (entry 1, shifting IXFS to entry 2)
- [x] GPT type GUID: Microsoft Basic Data (`EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`) -- Windows auto-mounts
- [x] GPT partition name: `"BlackBox"` (UTF-16LE)
- [x] Size: 128 MiB (262144 sectors), LBA 133120-395263, 1 MiB-aligned after EFI
- [x] IXFS shifts to LBA 395264 (~319 MiB on 512 MiB disk)
- [x] `GPT_NUM_ENTRIES` = 128 (sufficient, unchanged)
- [x] Backup GPT header and entry array correct (verified via fdisk)
- [x] Makefile updated: `BB_OFFSET`, `BB_SIZE`, `mkfs.fat -n BLACKBOX`, `mmd` creates 6 directories
- [x] `.info` file includes `BB_OFFSET` and `BB_SIZE` for downstream tools
- [x] Commit: `"tools: add BlackBox 128 MiB FAT32 partition to GPT layout"`

**Test checkpoint:** `make-system-disk` produces a 3-partition disk image. `fdisk -l build/system-disk.img` shows EFI (64 MiB) + BlackBox (128 MiB) + IXFS (~316 MiB). BlackBox partition type is Microsoft Basic Data. GPT partition name is "BlackBox".

## 2. Build Pipeline -- Format and Populate BlackBox

Update `scripts/build.sh` and Makefile to format the BlackBox partition as FAT32 and create the initial directory structure.

- [x] `mkfs.fat -F 32 -n "BLACKBOX" --offset $(BB_OFFSET/512)` formats BlackBox after GPT creation (Makefile line 376)
- [x] `BB_OFFSET := 68157440` (LBA 133120 * 512) calculated from make-system-disk GPT layout
- [x] `mmd -i $@@@$(BB_OFFSET) ::Logs ::Boot ::Crash ::Perf ::Diag ::Tools` creates all 6 directories (line 377)
- [x] Verified: `mdir -i build/system-disk.img@@68157440 ::/` shows all 6 directories with volume label BLACKBOX
- [x] Commit: implemented as part of `"build: add BlackBox 128 MiB FAT32 partition to GPT layout"` (TODO-17 §1)

**Test checkpoint:** `bash scripts/build.sh clean` produces a disk with BlackBox formatted as FAT32. `mcopy -i build/system-disk.img@@<offset> ::/ /tmp/bb-test` lists `Logs/`, `Boot/`, `Crash/`, `Perf/`, `Diag/`, `Tools/`.

## 3. Kernel Mount -- Discover "BlackBox" GPT Name, Mount as X:\

Update `partition.c` to recognize the "BlackBox" GPT partition name and mount it as `X:\`.

- [x] `part_streqi()` case-insensitive compare in `partition.c`; checks `pi->gpt_name` for `"BlackBox"`
- [x] BlackBox FAT32 partition mounts as `X:\` instead of next sequential letter
- [x] Sequential letter assignment skips `X` (reserved for BlackBox)
- [x] Logs: `"BlackBox partition mounted as X:\"`
- [x] Graceful fallback: if no "BlackBox" partition found, sequential assignment continues as before
- [x] Commit: `"kernel: mount BlackBox partition as X:\\ by GPT name"`

**Test checkpoint:** Serial log shows `"BlackBox partition mounted as X:\"`. `X:\` is accessible via VFS. Other FAT32 partitions still get `D:`, `E:`, etc. On disks without BlackBox, logs go to `C:\` as before. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

**Regression risk:** Partition mount order change could break C:\ IXFS mount. Rollback: revert the `gpt_name` check in `partition_mount_filesystems()`.

## 4. Directory Skeleton -- Create Dirs on First Boot

Ensure the BlackBox directory structure exists on first boot and after format.

- [x] After `partition_mount_filesystems()`, probe `X:\Logs` via `vfs_open()` to detect first boot
- [x] If missing, create all 7 directories: `Logs`, `Boot`, `Crash`, `Crash\WER`, `Perf`, `Diag`, `Tools`
- [x] Uses `vfs_create(path, VFS_DIRECTORY)` -- works on FAT32
- [x] Logs each creation: `"BlackBox: created X:\Logs"` etc.
- [x] Idempotent: second boot skips creation (probe finds `X:\Logs`)
- [x] Commit: `"kernel: create BlackBox directory skeleton on first boot"`

**Test checkpoint:** First boot after clean build: serial shows 7 `"BlackBox: created X:\..."` messages (Logs, Boot, Crash, Crash\WER, Perf, Diag, Tools). Second boot: no creation messages (directories already exist). Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

## 5. Klog Migration -- Move All Log Output to X:\Logs\

Redirect all kernel log output from `C:\Impossible\System\Logs\` to `X:\Logs\`.

- [x] Runtime `klog_dir` global replaces compile-time `KLOG_DIR`; resolved by `klog_resolve_dir()` at `klog_disk_init()` time
- [x] `KLOG_DIR_BLACKBOX = "X:\\Logs\\"` (primary), `KLOG_DIR_FALLBACK = "C:\\Impossible\\System\\Logs\\"` (fallback)
- [x] All log paths (`kernel.log`, subsystem logs, `events.jsonl`, `crash_recovery.log`, `boot-profile.log`, `boot-timeline.json`) built from `klog_dir` at runtime
- [x] Fallback: if X:\ not mounted, falls back to C:\ with warning `"BlackBox not mounted, using C:\\ for logs"`
- [x] `ensure_log_dirs()` skipped when using BlackBox (X:\Logs\ created by boot skeleton §4)
- [x] `KLOG_SERIAL_DIR` built dynamically from `klog_dir + "Serial\\"`
- [x] Commit: `"kernel: migrate klog output from C:\\ to X:\\Logs\\"`

**Test checkpoint:** Serial shows `"klog: writing to X:\Logs\Serial_YYMMDDN.log"`. All subsystem logs appear under `X:\Logs\`. On a disk without BlackBox, fallback message: `"klog: BlackBox not mounted, using C:\\"`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

**Regression risk:** If klog_disk_enable() runs before X:\ is mounted, log writes fail silently. The fallback check must happen at enable time, not at init time.

## 6. Boot Logs -- Per-Boot Session Files to X:\Boot\

Move the per-boot numbered session logs and boot timeline JSON to `X:\Boot\`.

- [x] `boot-timeline.json` → `X:\Boot\boot-timeline.json` when BlackBox mounted, C:\ fallback
- [x] `boot-profile.log` → `X:\Perf\boot-profile.log` when BlackBox mounted, C:\ fallback
- [x] `klog_using_blackbox` flag exposed in `klog.h` for boot subsystems to select correct subdirectory
- [x] Serial session logs remain at `X:\Logs\Serial\Serial_YYMMDDNN.log` (handled by §5)
- [x] Stale `BOOT_NNN.LOG` comments updated
- [x] Commit: `"kernel: move per-boot session logs to X:\\Boot\\"`

**Test checkpoint:** After boot, `X:\Boot\` contains `26040501.LOG` and `26040501.json`. Serial log path shows `X:\Boot\` prefix. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

## 7. Crash Dump Path -- crash_recovery.log to X:\Crash\

Move crash-persistent log recovery output to `X:\Crash\`.

- [x] `klog_crash_write_to_disk()` writes to `X:\Crash\crash_recovery.log` when BlackBox mounted, C:\ fallback
- [x] Future: `MEMORY.DMP` crash dumps (TODO-16) will also target `X:\Crash\`
- [x] Fallback path uses `klog_dir` (C:\ logs directory)
- [x] Commit: `"kernel: move crash recovery log to X:\\Crash\\"`

**Test checkpoint:** Force panic with `crash_test=1`; on next boot, `crash_recovery.log` appears in `X:\Crash\`, not `C:\Impossible\System\Logs\`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

## 8. Perf and Diag -- boot-profile, hwdump to X:\Perf\ and X:\Diag\

Move performance and diagnostic outputs to their BlackBox directories.

- [x] `hw_dump_write_file()`: writes structured hardware inventory to `X:\Diag\hwdump.txt` (CPU, memory, display, storage)
- [x] `boot_postcode_write_log()`: writes POST code history to `X:\Diag\postcode.log` with timestamps
- [x] `boot-profile.log` already at `X:\Perf\` (done in §6)
- [x] Both wired into Phase 3 boot after `boot_timing_write_report()`
- [x] All paths fall back to `klog_dir` (C:\ logs) when BlackBox not mounted
- [x] Commit: `"kernel: move diagnostic and perf output to X:\\Diag\\ and X:\\Perf\\"`

**Test checkpoint:** After boot, `X:\Diag\hwdump.txt` and `X:\Perf\boot-profile.log` exist with valid content. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

## 9. Host Tools -- SDK Reads BlackBox from Disk Image

Update host-side tools to locate and read the BlackBox partition from raw disk images.

- [x] `scripts/tools/read-blackbox.sh`: extracts all BlackBox directories from raw disk image using mtools
- [x] Uses `BB_OFFSET=68157440` (LBA 133120 * 512) matching Makefile
- [x] Extracts Logs/, Logs/Serial/, Boot/, Crash/, Perf/, Diag/, Tools/ to `build/blackbox-extract/`
- [x] Documents Linux mount: `sudo mount -o loop,offset=68157440,ro build/system-disk.img /mnt/blackbox`
- [x] Commit: `"tools: SDK reads BlackBox partition from disk images"`

**Test checkpoint:** `bash scripts/tools/read-blackbox.sh build/system-disk.img` extracts logs to a local directory. `mount` on Linux at the correct offset shows FAT32 with the directory structure.

## 10. Disk Space Management -- Log Aging, Quota, Cleanup

128 MiB is generous but finite. Without space management, logs eventually fill the partition and writes fail silently. Windows limits CBS.log to ~20 MB with rotation; Linux logrotate enforces per-file and total quotas. BlackBox needs the same discipline.

- [x] `fat32_get_free_bytes(vol)` / `fat32_volume_from_root(root)` added to `fat32.h` / `fat32_ops.c`
- [x] After BlackBox mount: logs free space (`"BlackBox: N MiB free (N%)"`)
- [x] If free < 10%: cleanup runs -- deletes oldest Boot\ files beyond 10 sessions, then rotated `.N` logs in Logs\
- [x] MaxBootSessions = 10, MinFreeMiB = 16 (hardcoded defaults, TODO: wire to registry)
- [x] Cleanup log: `"BlackBox: cleanup freed N KiB (N files removed)"`
- [x] If critically low after cleanup: falls back to C:\ with LOG_ERROR
- [x] Commit: `"kernel: BlackBox disk space management -- log aging and quota enforcement"`

**Test checkpoint:** Fill BlackBox with dummy files until < 10% free. Boot -> serial shows cleanup message with freed space. Boot sessions beyond 10 are pruned. Verify on QEMU WHPX, TCG, VirtualBox.

## 11. FAT32 Volume Label -- Set "BLACKBOX" at Format Time

Windows and Linux identify FAT32 volumes by their 11-character volume label (stored in BPB and root directory). Setting it at format time ensures `vol` command and Disk Management show "BLACKBOX" instead of "NO NAME".

- [x] `-n BLACKBOX` already passed to `mkfs.fat` (done in §1)
- [x] Verified: `mlabel` shows `BLACKBOX`
- [x] `fat32_init()` parses BS_VolLab (BPB offset 71, 11 bytes) into `vol->label`, space-trimmed
- [x] Log at mount: `FAT32: "BLACKBOX" 128 MiB, 8 sectors/cluster, root cluster 2`
- [x] Commit: `"build: set FAT32 volume label BLACKBOX at format time"`

**Test checkpoint:** `mlabel` shows BLACKBOX. Windows Disk Management shows "BLACKBOX (X:)" when disk is attached. Serial shows volume label on mount.

## 12. Partition Health -- fsck on Mount, Dirty-Bit Check

FAT32 has a "dirty" bit (byte 0x41 in BPB, bit 0 of the word). If the OS crashed mid-write, the dirty bit is set. Check it on mount and optionally run fsck. (-> XREF: TODO-04 §6 -- FAT32 fsck implementation)

- [x] At BlackBox mount: `fat32_is_dirty()` checks FAT[1] bit 27
- [x] If dirty: logs warning + runs `fat32_run_fsck(vol, 1)` in repair mode
- [x] `fat32_mark_dirty()` sets dirty on mount (new `fat32_set_dirty_marker()`)
- [x] `fat32_mark_clean()` clears dirty in `acpi_shutdown()` before power-off
- [x] Public API: `fat32_is_dirty/mark_dirty/mark_clean/run_fsck` in `fat32.h`
- [x] Commit: `"kernel: BlackBox FAT32 dirty-bit check and optional fsck on mount"`

**Test checkpoint:** Force unclean shutdown (kill QEMU mid-write). Next boot: serial shows "partition dirty" warning. After clean shutdown: no warning. Verify on QEMU WHPX, TCG.

**Regression risk:** fsck on mount adds boot latency. Bound to max 2 seconds; skip if partition is clean. Rollback: disable fsck call, keep dirty-bit logging only.

## 13. WER Staging Area -- Error Reports in X:\Crash\WER\

Windows Error Reporting (WER) stages error reports in `C:\ProgramData\Microsoft\Windows\WER\` before upload. Impossible OS can stage structured error reports for crashed processes in `X:\Crash\WER\` -- enabling post-mortem analysis without a network connection.

> [!TIP]
> Neither Win11 WER nor Linux apport stages crash reports on a separate cross-platform-readable partition. BlackBox WER reports are FAT32-readable by any OS -- plug the disk into any machine and read the crash context.

- [x] `X:\Crash\WER\` already in boot skeleton (§4)
- [x] `wer_write_crash_report()` in `wer.c`: writes JSON report on unhandled CPU exception
- [x] Report: `{"pid":N,"name":"app","exception":N,"rip":"0xN","rsp":"0xN","error_code":N,"registers":{...},"cs":N}`
- [x] Wired into `isr_handler()` default exception path (before `panic_screen`)
- [x] Filename: `PID_YYYYMMDDHHMMSS.json` (timestamp from wall clock)
- [x] Falls back to C:\ when BlackBox not mounted
- [x] Commit: `"kernel: WER-style crash report staging in X:\\Crash\\WER\\"`

**Test checkpoint:** Trigger a user-mode crash (NULL deref in cmd.exe test). `X:\Crash\WER\` contains a JSON report with PID, exception code, and stack trace. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

## 14. A/B Layout Compatibility -- 4+ Partition Coexistence

TODO-14 (A/B dual-slot boot) defines: EFI + Slot A IXFS + Slot B IXFS. With BlackBox, the layout becomes 4 partitions: EFI + BlackBox + Slot A + Slot B. Ensure `make-system-disk.c` supports both layouts via a build flag.

- [x] `--ab` flag: produces 4-partition layout (EFI + BlackBox + IXFS A + IXFS B)
- [x] Default (no flag): 3-partition layout (EFI + BlackBox + IXFS)
- [x] BlackBox 128 MiB in both layouts; IXFS slots split remaining space evenly (1 MiB aligned)
- [x] Kernel mounts BlackBox as X:\ by GPT name -- works regardless of partition count
- [x] GPT names: "Impossible OS A" / "Impossible OS B" in A/B mode
- [x] Commit: `"tools: make-system-disk A/B layout with BlackBox partition"`

**Test checkpoint:** `make-system-disk --ab` produces 4-partition image. `fdisk -l` shows EFI + BlackBox + 2x IXFS. Kernel boots and mounts X:\ from either layout. Verify on QEMU WHPX.

## 15. Boot Platform TODO Updates -- XREFs and Domain Sync

Update cross-references across affected TODOs.

- [x] `TODO-02-system-logging.md`: XREF to TODO-17 already present (line 23)
- [x] `TODO-05-bare-metal-hardening.md`: updated log path refs from C:\ to X:\, KLOG_DIR -> klog_dir
- [x] `TODO-07-boot-diagnostics.md`: crash dump paths updated to `X:\Crash\`
- [x] `TODO-16-crash-dump-generation.md`: all `C:\Impossible\System\CrashDumps\` -> `X:\Crash\`
- [x] `TODO-04-restore-recovery.md` (domain 10): crash dump paths updated to `X:\Crash\`
- [x] `TODO-04-release-qa.md` (domain 15): crash dump collection path updated
- [x] `TODO-17` current state block: updated from 2-partition to 3-partition
- [x] CLAUDE.md: no stale references (does not mention partition layout)
- [x] Commit: `"docs: update XREFs for BlackBox partition migration"`

**Test checkpoint:** All referenced TODO files have correct XREFs. No stale `C:\Impossible\System\Logs\` references remain in active TODO files.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_blackbox()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).

- [x] `test_blackbox.c`: 11 tests -- X:\ mounted, 7 directories exist, klog_dir resolved, volume label BLACKBOX, free space > 0
- [x] All tests skip gracefully if BlackBox partition not present
- [x] `fat32_get_label()` public API added for volume label access
- [x] Registered in `test_runner_init()`: `test_register_blackbox()`
- [x] Commit: `"test: add BlackBox partition tests"`

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                   | 🐧 Linux                   | 🚀 Impossible OS                    |
|----|----------------------------|-----------------------------|----------------------------|--------------------------------------|
| 💎 | Separate log partition     | ✅ Recovery + WinRE        | ⚠️ /var/log on root        | ✅ §1 -- 3-part GPT + X:\           |
| 💎 | Cross-platform readable    | ✅ NTFS (with drivers)     | ✅ ext4 (with drivers)     | ✅ §1 -- FAT32 universal            |
| 💎 | Crash dump isolation       | ✅ C:\Windows\MEMORY.DMP   | ✅ /var/crash              | ✅ §7 -- X:\Crash\                  |
| ⭐ | Per-boot log files         | ❌ Not built-in            | ❌ Not built-in            | ✅ §6 -- X:\Boot\ + X:\Perf\        |
| ⭐ | Boot timeline JSON         | ❌ Not built-in            | ❌ Not built-in            | ✅ §6 -- X:\Boot\timeline.json      |
| 💎 | Auto-mount on host         | ✅ Windows assigns letter  | ✅ udisks2 auto-mount      | ✅ §1 -- Basic Data GUID            |
| 💎 | Volume label               | ✅ NTFS volume label       | ✅ e2label / fatlabel      | ✅ §11 -- BPB label parsed          |
| 💎 | Named partition discovery  | ✅ Volume label match      | ✅ LABEL= in fstab         | ✅ §3 -- GPT name match X:\         |
| 💎 | Structured diagnostics dir | ⚠️ Scattered in C:\Windows | ⚠️ /var/log + /sys         | ✅ §4+§8 -- Logs/Boot/Crash/Diag    |
| 💎 | Log space management       | ✅ CBS.log 20 MB cap       | ✅ logrotate + journald    | ✅ §10 -- aging + quota + cleanup   |
| 💎 | Dirty-bit / fsck on mount  | ✅ chkdsk on dirty FAT32   | ✅ fsck.fat on mount       | ✅ §12 -- dirty+fsck on mount       |
| ⭐ | Cross-platform WER staging | ⚠️ WER on NTFS only        | ⚠️ apport on ext4 only     | ✅ §13 -- FAT32 WER JSON reports    |
| 💎 | A/B + diagnostic coexist   | ❌ Recovery only           | ⚠️ A/B without diag part   | ✅ §14 -- 4-partition --ab layout   |

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
