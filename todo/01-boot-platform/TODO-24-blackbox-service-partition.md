---
schema_version: 1
id: blackbox-service-partition
domain: 01-boot-platform
status: active
title: "TODO-24 -- BlackBox Service Partition"
---

# TODO-24 -- BlackBox Service Partition

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Add a 128 MiB FAT32 "BlackBox" partition to the GPT disk layout, mounted as `X:\`. All kernel logs, crash dumps, boot timelines, diagnostic snapshots, and portable tools live here -- separate from the IXFS system volume. FAT32 gives crash resilience (survives IXFS corruption), cross-platform readability (Windows/Linux/macOS mount it natively), and clean separation of OS files from diagnostic data.

> [!IMPORTANT]
> **Current state:** 3-partition GPT (64 MiB EFI + 128 MiB BlackBox FAT32 + ~316 MiB IXFS). All logs, crash reports, boot timelines, and diagnostics write to `X:\` (BlackBox). Falls back to `C:\Impossible\System\Logs\` if BlackBox is absent. A/B dual-slot layout supported via `--ab` flag (6 partitions: EFI + BlackBox + ABMeta + IXFS A + IXFS B + Recovery).

## Inputs

- [`tools/make-system-disk.c`](../../tools/make-system-disk.c) -- GPT disk image builder (3-partition default, 4-partition A/B)
- [`scripts/build.sh`](../../scripts/build.sh) -- build pipeline, disk formatting
- [`src/kernel/fs/partition.c`](../../src/kernel/fs/partition.c) -- GPT partition discovery, BlackBox X:\ mount by name
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c) -- log file creation and flush paths
- [`include/kernel/klog.h`](../../include/kernel/klog.h) -- runtime `klog_dir` global (resolved to `X:\Logs\` or C:\ fallback)
- -> XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` §6 -- FAT32 LFN write (prerequisite for lowercase filenames)
- -> XREF: `01-boot-platform/TODO-05-boot-device-discovery.md` -- partition GUID validation
- -> XREF: `02-kernel-core/TODO-04-system-logging.md` -- klog output paths, log rotation policy
- -> XREF: `02-kernel-core/TODO-27-crash-dump-generation.md` -- MEMORY.DMP + minidump writer targets X:\Crash\
- -> XREF: `14-host-tools/TODO-08-blackbox-log-extractor.md` -- host-side CLI to extract/view logs from disk images
- -> XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` §8 -- FAT32 fsck validates BlackBox integrity
- -> XREF: `01-boot-platform/TODO-21-ab-boot-rollback.md` -- A/B dual-slot layout adds 2 more partitions; BlackBox must coexist

## Outcome

- 3-partition GPT: EFI (64 MiB) + BlackBox (128 MiB FAT32) + IXFS (~316 MiB).
- BlackBox partition mounts as `X:\` by GPT name match, not by partition index.
- All kernel logs, crash dumps, boot timelines, and diagnostics write to `X:\` instead of `C:\`.
- Directory structure created on first boot: `Logs\`, `Logs\Serial\`, `Boot\`, `Crash\`, `Crash\WER\`, `Perf\`, `Diag\`, `Tools\`.
- Host-side tools (SDK) can mount and read BlackBox from the raw disk image.
- Windows auto-mounts BlackBox when the disk image or real disk is attached (Microsoft Basic Data GUID).
- Log space managed automatically: boot session pruning, rotation cleanup, configurable quota.
- FAT32 dirty-bit checked on mount; optional fsck for crash recovery integrity.
- WER-style crash reports staged in `X:\Crash\WER\` as cross-platform-readable JSON.
- A/B dual-slot boot layout compatible via `--ab` build flag.

## Implementation Order

| ⭐  | Order | Deliverable                                                    | Depends On | Status |
| --- | :---: | -------------------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Disk tooling -- add BlackBox partition to GPT layout           | --         |  [x]   |
| 💎  |   2   | Build pipeline -- format and populate BlackBox                 | §1         |  [x]   |
| 💎  |   3   | Kernel mount -- discover "BlackBox" GPT name, mount X:         | §1         |  [x]   |
| 💎  |   4   | Directory skeleton -- create dirs on first boot                | §3         |  [x]   |
| 💎  |   5   | Klog migration -- move all log output to X:\Logs\              | §3, §4     |  [/]   |
| 💎  |   6   | Boot logs -- per-boot session files to X:\Boot\                | §3, §4     |  [x]   |
| 💎  |   7   | Crash dump path -- crash_recovery.log to X:\Crash\             | §3, §4     |  [x]   |
| 💎  |   8   | Perf and diag -- boot-profile, hwdump to X:\Perf\ and X:\Diag\ | §3, §4     |  [x]   |
| 💎  |   9   | Host tools -- SDK reads BlackBox from disk image               | §1, §2     |  [x]   |
| 💎  |  10   | Disk space management -- log aging, quota, cleanup             | §5         |  [/]   |
| 💎  |  11   | FAT32 volume label -- set "BLACKBOX" at format time            | §2         |  [x]   |
| 💎  |  12   | Partition health -- fsck on mount, dirty-bit check             | §3, T09 §6 |  [/]   |
| ⭐  |  13   | WER staging area -- error reports in X:\Crash\WER\             | §4, §7     |  [/]   |
| 💎  |  14   | A/B layout compatibility -- 4+ partition coexistence           | §1         |  [x]   |
| 💎  |  15   | Boot platform TODO updates -- XREFs and domain sync            | §1-§14     |  [/]   |
| 💎  |  16   | Crash/WER report retention in partition cleanup                | §10, §13   |  [x]   |

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

> **Test runner:** N/A (host GPT image builder; no kernel test surface) | validation: clean build emits a valid GPT, `mdir` confirms BLACKBOX FAT32 + 6 dirs at the `.info` offset
> **Notes:**
> - `make-system-disk.c` builds the 3-partition (EFI + BlackBox + IXFS) and 4-partition `--ab` GPT; BlackBox is a 128 MiB Microsoft Basic Data partition named "BlackBox", 1 MiB-aligned after EFI.
> - Realized partition offsets/sizes are emitted to `<image>.info`; the Makefile mkfs/mmd/mcopy steps source the BlackBox offset+size from there (no hardcoded LBA), matching the EFI/IXFS pattern.
> - Backup GPT header + entry array written at `total_sectors-1` / `total_sectors-33` with recomputed CRC32; `GPT_NUM_ENTRIES=128`.
> - Kernel-side mount reads the "BlackBox" GPT name in `partition.c` (§3); host tools read the partition via the `.info` offset (§9).
> - Scope: GPT layout + `.info` contract only; FAT32 format/dir-skeleton is §2/§4, kernel mount is §3.
> **Verified:** 2026-06-16 | commit `4252b021` | 9/9 items | build OK | host mdir: BLACKBOX + 6 dirs at .info offset
> **Quality reviewed:** 2026-06-16 | Codex 3x (adversarial, consistency, perf; re-adv skipped: host-tool config, no locking/ISR/lifecycle) | 1H+2M fixed | scope: N/A (host build tool, no kernel domain skill)

---

## 2. Build Pipeline -- Format and Populate BlackBox

Update `scripts/build.sh` and Makefile to format the BlackBox partition as FAT32 and create the initial directory structure.

- [x] `. $@.info && mkfs.fat -F 32 -n "BLACKBOX" -s 1` formats BlackBox from the generated `.info` offset/size (`-s 1` keeps the 128 MiB volume above the FAT32 65525-cluster floor, matching the ESP/recovery recipes)
- [x] BlackBox offset/size sourced from `<image>.info` (`BB_OFFSET`/`BB_SIZE`), not a hardcoded Makefile constant (see §1)
- [x] Two `mmd` calls create 8 directories: `::Logs ::Boot ::Crash ::Perf ::Diag ::Tools` then nested `::Crash/WER ::Logs/Serial`
- [x] Verified: `mdir -i build/system-disk.img@@<.info BB_OFFSET> ::/` shows all directories with volume label BLACKBOX; FAT32 cluster-count warning cleared
- [x] Commit: implemented as part of `"build: add BlackBox 128 MiB FAT32 partition to GPT layout"` (TODO-24 §1)

**Test checkpoint:** `bash scripts/build.sh clean` produces a disk with BlackBox formatted as spec-valid FAT32 (no cluster-count warning). `mdir -i build/system-disk.img@@<.info BB_OFFSET> ::/` lists `Logs/`, `Logs/Serial/`, `Boot/`, `Crash/`, `Crash/WER/`, `Perf/`, `Diag/`, `Tools/`.

> **Test runner:** `scripts\debug\kernel\run-blackbox-tests.bat` (SUITE=fs) | marker round-trip (X:\Diag\blackbox-marker.txt) + FAT32 mount validate; clean build emits spec-valid FAT32 (cluster warning cleared)
> **Notes:**
> - Formats the BlackBox FAT32 volume + populates the directory skeleton in the system-disk Makefile recipe, sourcing the realized offset+size from the generated `.info`.
> - `-s 1` (512-byte clusters) keeps the 128 MiB volume above the FAT32 65525-cluster floor, so strict readers (firmware, other OSes) accept `X:`.
> - Two `mmd` calls create 8 dirs (6 top-level + `Crash/WER` + `Logs/Serial`); a `blackbox-marker.txt` is `mcopy`'d for the user-mode round-trip test.
> - `.DELETE_ON_ERROR` makes the multi-step in-place disk build atomic: a partial image is deleted on any recipe failure so `make` cannot reuse a half-built disk.
> - Scope: host-side format/populate only; kernel first-boot skeleton is §4, mount is §3.
> **Verified:** 2026-06-16 | commit `64280239` | 4/4 items | build OK | smoke PASS 2.45s; FAT32 cluster warning cleared
> **Quality reviewed:** 2026-06-16 | Codex 3x (adversarial, consistency, perf; re-adv skipped: static-const dir-list + Makefile config, no locking/ISR/lifecycle) | 2H+1M fixed | scope: kernel-code-quality (boot_storage skeleton)

---

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

> **Notes:**
> - `part_streqi()` case-insensitive GPT-name match mounts the BlackBox FAT32 volume as `X:\`, the reserved diagnostics letter.
> - X: reservation now honored by BOTH the FAT32 and NTFS sequential allocators -- the NTFS path previously could claim X: and shadow BlackBox.
> - BlackBox `vfs_mount('X')` return is checked; a taken letter logs LOG_WARN instead of silently reporting success.
> - X: stays reserved even on disks without a BlackBox partition, keeping the `X:\` == diagnostics contract stable.
> - Dirty-bit fsck on mount + mark-dirty; cleared on clean shutdown.
> **Verified:** 2026-06-17 | commit `ec307531` | 5/5 items | build OK | smoke PASS (TCG 2.63s)
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf; re-adversarial skipped -- 15-line fix, no lock/ISR/lifecycle) | 2H+1L fixed, 1M rejected (X: reserved by design) | scope: kernel-code-quality

---

## 4. Directory Skeleton -- Create Dirs on First Boot

Ensure the BlackBox directory structure exists on first boot and after format.

- [x] After `partition_mount_filesystems()`, (re)create the 8-dir skeleton unconditionally every boot -- no probe-gate, so a partial skeleton (`Logs` survived but a sibling was lost) self-heals
- [x] If missing, create all 8 directories: `Logs`, `Logs\Serial`, `Boot`, `Crash`, `Crash\WER`, `Perf`, `Diag`, `Tools` (`Logs\Serial` matches the host-built skeleton so kernel-formatted/repaired BlackBox keeps per-boot serial logs)
- [x] Uses `vfs_create(path, VFS_DIRECTORY)` -- works on FAT32
- [x] Logs each creation: `"BlackBox: created X:\Logs"` etc.
- [x] Idempotent: `fat32_create_dir_vol` refuses to recreate an existing dir (returns != 0, allocates nothing) via a read-only tri-state full-chain scan, so re-running every boot leaks no clusters (`test_bb_mkdir_idempotent`)
- [x] Commit: `"kernel: create BlackBox directory skeleton on first boot"`

**Test checkpoint:** On a BlackBox whose skeleton is incomplete, serial shows `"BlackBox: created X:\..."` for each of the 8 missing directories (Logs, Logs\Serial, Boot, Crash, Crash\WER, Perf, Diag, Tools); on a fully-populated BlackBox no creation messages appear (vfs_create is a no-op on existing dirs). Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-blackbox-tests.bat` (SUITE=fs) | BlackBox suite incl. `test_bb_mkdir_idempotent` (re-create refused); kernel FS 114 + user-mode 16 PASS on TCG
> **Notes:**
> - The 8-dir BlackBox skeleton (Logs, Logs\Serial, Boot, Crash, Crash\WER, Perf, Diag, Tools) is (re)created unconditionally every boot in `boot_storage.c`, so a partial skeleton self-heals.
> - `fat32_create_dir_vol` is now idempotent: a read-only tri-state full-chain scan (`fat32_dir_find_child`) refuses to recreate an existing dir, closing an every-boot cluster/dirent leak on X: that hid behind false "created" logs.
> - The existence scan is fail-closed: I/O error, malformed FAT chain, or out-of-range parent return -1 (refuse) so a degraded read can never let a duplicate through.
> - `read-blackbox.sh` now extracts `Crash\WER` (the non-recursive `Crash` copy dropped it), matching what `wer.c` writes there.
> - Regression: `test_bb_mkdir_idempotent` asserts re-creating an existing `X:\Logs` is refused.
> **Verified:** 2026-06-17 | commit `cb127f41` | 5/5 items | build OK | smoke PASS (TCG 2.65s)
> **Quality reviewed:** 2026-06-17 | Codex 6x (adversarial, consistency, perf, re-adversarial x3) | 3H+1M fixed | scope: kernel-code-quality

---

## 5. Klog Migration -- Move All Log Output to X:\Logs\

Redirect all kernel log output from `C:\Impossible\System\Logs\` to `X:\Logs\`.

- [x] Runtime `klog_dir` global replaces compile-time `KLOG_DIR`; resolved by `klog_resolve_dir()` at `klog_disk_init()` time
- [x] `KLOG_DIR_BLACKBOX = "X:\\Logs\\"` (primary), `KLOG_DIR_FALLBACK = "C:\\Impossible\\System\\Logs\\"` (fallback)
- [x] All log paths (`kernel.log`, subsystem logs, `events.jsonl`, `crash_recovery.log`, `boot-profile.log`, `boot-timeline.json`) built from `klog_dir` at runtime
- [x] Fallback: if X:\ not mounted, falls back to C:\ with warning `"BlackBox not mounted, using C:\\ for logs"`
- [x] `ensure_log_dirs()` skipped when using BlackBox (X:\Logs\ created by boot skeleton §4)
- [x] `KLOG_SERIAL_DIR` built dynamically from `klog_dir + "Serial\\"`
- [/] `klog_disk_flush` re-entrancy guard when the C:\ fallback is active -> XREF: 02-kernel-core/TODO-04 §15 (item: "klog_disk_flush re-entrancy guard on the C: fallback path")
  - `klog()` calls `klog_disk_append()` + `klog_disk_flush()` after the serial emission, and `klog_disk_flush()` calls `vfs_open()` / `vfs_write()` against `klog_dir`.
  - With `klog_using_blackbox=0` and `klog_dir` fallen back to `C:\Impossible\System\Logs\` (IXFS), a `klog()` from inside an IXFS read/write path (e.g. `ixfs_checksum_verify()`) re-enters VFS/IXFS synchronously.
  - Work: a per-CPU `klog_in_disk_flush` flag (or an existing vfs/ixfs-in-flight marker) in `src/kernel/klog_disk.c` `klog_disk_flush()`, or a `klog_no_flush()` variant used from fs-layer emitters.
  - Blocker: do not land a fix until the sole-C:\ boot scenario is reproduced. Flagged by a diagnose-serial-log Codex review 2026-04-18; X:\ was mounted in the captured log so the path was never exercised, and the hazard stays latent until a BlackBox mount fails.
  - The reentrancy regression test lands with the guard, built on `00-infrastructure/TODO-03` §1 (`test_add_fault`) + §6 fault-injection hardening to force the BlackBox-absent path.
- [/] **Durable-write + write-success honesty retrofit for X:\ diagnostic writers.** Three writers ack success without a durability / exact-length boundary, so a failed or non-durable write is silently treated as done:
  - `append_health_record` return is discarded (`src/kernel/main/boot_health_check.c:629`), then `mark_entry_successful_from_ctr()` writes `ImpossibleOS-MarkGood` on PASS (`boot_health_check.c:640-645`); the helper does `vfs_write()` + `vfs_close()` with no `vfs_flush()`, so a non-durable health record can still let the next boot consume MarkGood and DELETE the boot counter (irreversible boot-state). Safety-relevant, not cosmetic.
  - `audit_write_sequence` / `audit_append_file` (`src/kernel/main/boot_audit.c`) ack NVRAM triggers after write+close alone (same gap; durable-write predicate prototyped in a held git-stash).
  - `wer_write_crash_report` (`src/kernel/wer.c:175-181`, the §13 WER producer -- XREF §13) ignores the `vfs_write`/`vfs_close` returns, has no `vfs_flush`, and logs `"Crash report: ..."` success whenever `vfs_open` succeeded, so a truncated/empty report reads as staged.
  - Work: add a shared `boot_durable_write_ok(wr, expected, flush, close)` predicate; gate each writer on exact-length `vfs_write` + `vfs_flush()` + clean `vfs_close` (boundary from `01-boot/TODO-12` §6) and log an explicit failure path instead of false success.
  - Parked, and the MarkGood-gating half is `operator-gated`: whether a non-durable health record may still let the next boot consume MarkGood and delete the boot counter is an irreversible boot-state policy call, not an implementation choice. The retrofit itself is TODO-24-owned with no external blocker.
  - **DEFER (operator-reserved decision; NOT a run-stop):** whether MarkGood must be gated on the durable health-record write (is `X:\Boot\health.jsonl` authoritative for the mark-good decision?) is a deliberate design decision reserved for the operator. The unattended runner must DEFER this sub-item (`[/]` + Deferred stamp + XREF to this line) and ADVANCE -- it must NOT stop, disarm, or decide it autonomously (per `todo/TODO-Claude-Overnight-Runner.md` session-exit policy). The decoupled-choice WIP is held in a git stash pending operator confirm. The write-honesty fix for the audit + WER writers is NOT blocked by this and may proceed.
- [x] Commit: `"kernel: migrate klog output from C:\\ to X:\\Logs\\"`

**Test checkpoint:** Serial shows `"klog: writing to X:\Logs\Serial_YYMMDDN.log"`. All subsystem logs appear under `X:\Logs\`. On a disk without BlackBox, fallback message: `"klog: BlackBox not mounted, using C:\\"`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

**Regression risk:** If klog_disk_enable() runs before X:\ is mounted, log writes fail silently. The fallback check must happen at enable time, not at init time.

> **Notes:**
> - klog output migrated from `C:\Impossible\System\Logs\` to `X:\Logs\` via the runtime `klog_dir` global (`KLOG_DIR_BLACKBOX` primary, `KLOG_DIR_FALLBACK` C:\), resolved at `klog_disk_init()`.
> - All log paths (`kernel.log`, subsystem logs, `events.jsonl`, `crash_recovery.log`, `boot-profile.log`, `Serial\`) build from `klog_dir` at runtime; C:\ fallback with a warning when X: is unmounted.
> - Two hardening items remain open and are DEFERRED below (latent reentrancy guard + durable-write honesty); the core migration is shipped and exercised (smoke shows `X:\Logs` active).
> **Deferred:** [M] klog_disk_flush C:\-fallback re-entrancy guard is latent until the BlackBox-mount-fail (sole-C:\) path is reproduced with fault injection -> XREF: 01-boot-platform/TODO-24 §5 (item: "klog_disk_flush re-entrancy guard when C:\ fallback is active" at line 187); depends on 00-infrastructure/TODO-03 §1 (test_add_fault) + §6
> **Deferred:** [H] durable-write/write-success honesty retrofit for X:\ diagnostic writers (health/audit/WER ack without vfs_flush); MarkGood-gating is an operator-reserved decision (line 190), retrofit is substantial follow-up -> XREF: 01-boot-platform/TODO-24 §5 (item: "Durable-write + write-success honesty retrofit for X:\ diagnostic writers" at line 193)

---

## 6. Boot Logs -- Per-Boot Session Files to X:\Boot\

Move the per-boot numbered session logs to `X:\Boot\`. (`boot-timeline.json` originally landed here as well; TODO-04 FPDT and Boot Timing Normalization moved it to `X:\Perf\` when FPDT entries joined the timeline.)

- [x] `boot-timeline.json` → `X:\Perf\boot-timeline.json` when BlackBox mounted, C:\ fallback (path moved from `X:\Boot\` by TODO-04 FPDT and Boot Timing Normalization when FPDT entries joined the timeline)
- [x] `boot-profile.log` → `X:\Perf\boot-profile.log` when BlackBox mounted, C:\ fallback
- [x] `klog_using_blackbox` flag exposed in `klog.h` for boot subsystems to select correct subdirectory
- [x] Serial session logs remain at `X:\Logs\Serial\Serial_YYMMDDNN.log` (handled by §5)
- [x] Stale `BOOT_NNN.LOG` comments updated
- [x] Commit: `"kernel: move per-boot session logs to X:\\Boot\\"`

**Test checkpoint:** After boot, `X:\Boot\` contains the per-boot session files `26040501.LOG` and `26040501.json`; serial session logs stay at `X:\Logs\Serial\` (owned by §5, built from `klog_dir + "Serial\\"`). Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Notes:**
> - Per-boot diagnostic artifacts route to `X:\Perf\` when BlackBox is mounted, C:\ `klog_dir` fallback otherwise: `boot-profile.log` (`boot_timing.c`), `boot-timeline.json` (`boot_progress.c`).
> - `klog_using_blackbox` (extern in `klog.h`, defined in `klog_disk.c`) is the single runtime selector boot subsystems use to pick `X:\Perf\` vs the C:\ fallback dir.
> - `boot-timeline.json` now warns (not silently drops) when its target dir is missing, matching `boot-profile.log`'s open-failure behavior.
> - Serial session logs stay at `X:\Logs\Serial\` (owned by §5). The two boot-trend follow-ups moved to TODO-29 §20 on 2026-09-03: the O(N^2) linearization SHIPPED, and the C:\ fallback is parked on the kernel image ceiling behind an explicit BlackBox-absent refusal.
> **Verified:** 2026-06-17 | commit `994e9794` | 5/5 items | build OK | smoke PASS (TCG 2.66s)
> **Accepted:** [M] `boot-trend.json` has no C:\ fallback (always `X:\Perf` even when `klog_using_blackbox`=0) -> XREF: 01-boot-platform/TODO-29 §20 (item: "C:\ fallback for `boot-trend.json`" at line 557). Moved from §3 to §20 on 2026-09-03; the writer now REFUSES up front when BlackBox is absent instead of failing at `vfs_open`, and the runtime path construction stays parked on the kernel image ceiling.
> **Accepted:** [M] `boot_trend_publish_json` O(N^2) `json_array_get` traversal on the boot path -> RESOLVED 2026-09-03 in 01-boot-platform/TODO-29 §20 (item: "Linearize `boot_trend_publish_json()` traversal" at line 513, now `[x]`): the loop walks `json_array_first`/`json_array_next` and bounds the scan at `BOOT_TREND_MAX_SCAN`.
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf; re-adversarial skipped -- 9-line warn+comment fix) | 1M+1L fixed, 2M accepted-XREF | scope: kernel-code-quality

---

## 7. Crash Dump Path -- crash_recovery.log to X:\Crash\

Move crash-persistent log recovery output to `X:\Crash\`.

- [x] `klog_crash_write_to_disk()` writes to `X:\Crash\crash_recovery.log` when BlackBox mounted, C:\ fallback (producer: reserved-RAM ring-buffer survival, owned by `02-kernel-core/TODO-04` §8)
- [x] Binary `MEMORY.DMP` / minidump writers also target `X:\Crash\` -- owned by `02-kernel-core/TODO-27` §5-§8 (this section owns only the X:\Crash\ destination)
- [x] Fallback path uses `klog_dir` (C:\ logs directory)
- [x] Commit: `"kernel: move crash recovery log to X:\\Crash\\"`

**Test checkpoint:** Force panic with `crash_test=1`; on next boot, `crash_recovery.log` appears in `X:\Crash\`, not `C:\Impossible\System\Logs\`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> [!NOTE]
> Handoff: `TODO-14-boot-diagnostics.md` §5 writes `X:\Crash\last-panic.txt` (shipped) -- same `X:\Crash\` mount and BlackBox path rules as this section. Retention of the unbounded `X:\Crash\WER\` reports is owned by §16.

> **Notes:**
> - `crash_recovery.log` routes to `X:\Crash\` when BlackBox is mounted, C:\ `klog_dir` fallback otherwise (`klog_crash_write_to_disk`); producer (reserved-RAM ring survival) owned by TODO-04 §8, binary dumps by TODO-27.
> - Fixed flag-drift: `klog.c` locally defined `VFS_O_TRUNC` as `0x08` (= canonical `VFS_O_APPEND`), so the crash log opened APPEND not TRUNC and left stale bytes; now includes `kernel/fs/vfs.h` for canonical flags + signatures.
> - Write path is now durable-honest: every `vfs_write` is length-checked, `vfs_flush` + `vfs_close` are checked, and `s_recovered_count` is preserved for retry on any failure instead of reporting a truncated log as complete.
> - The previous-crash splash/klog hint (`boot_desktop.c`) now derives the `last-panic.txt` path from `klog_using_blackbox` to match `panic.c`'s writer (was a hardcoded `X:\` hint misdirecting on C:\ fallback).
> - Test gap: `klog_crash_write_to_disk` failure paths (short write / flush-fail) need fault injection -> deferred to 00-infrastructure/TODO-03 §1/§6 (same dependency as §5's reentrancy test).
> **Verified:** 2026-06-17 | commit `a6e06895` | 3/3 items | build OK | smoke PASS (TCG 2.88s)
> **Quality reviewed:** 2026-06-17 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 3H+1M fixed | scope: kernel-code-quality

---

## 8. Perf and Diag -- boot-profile, hwdump to X:\Perf\ and X:\Diag\

Move performance and diagnostic outputs to their BlackBox directories.

- [x] `hw_dump_write_file()`: writes structured hardware inventory to `X:\Diag\hwdump.txt` (CPU, memory, display, storage)
- [x] `boot_postcode_write_log()`: writes POST code history to `X:\Diag\postcode.log` with timestamps
- [x] `boot-profile.log` already at `X:\Perf\` (done in §6)
- [x] Both wired into Phase 3 boot after `boot_timing_write_report()`
- [x] All paths fall back to `klog_dir` (C:\ logs) when BlackBox not mounted
- [x] Commit: `"kernel: move diagnostic and perf output to X:\\Diag\\ and X:\\Perf\\"`

**Test checkpoint:** After boot, `X:\Diag\hwdump.txt` and `X:\Perf\boot-profile.log` exist with valid content. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Notes:**
> - `hwdump.txt` (`hw_dump_write_file`) + `postcode.log` (`boot_postcode_write_log`) write to `X:\Diag\` when BlackBox is mounted, C:\ `klog_dir` fallback otherwise; both wired into Phase 3.
> - `hwdump.txt` now opens with `VFS_O_CREATE|VFS_O_TRUNC` (was bare `VFS_O_WRITE`, leaving stale tail bytes on a shorter dump); `hw_dump.c` includes `kernel/fs/vfs.h` for canonical flags.
> - Both writers are now write-honest: `vfs_write` length-checked, `vfs_flush` before declaring success, `LOG_WARN` on open/write/flush failure instead of false success.
> - Fixed a parent-directory handle leak in the hwdump create-via-dir path (the `dir` node was never `vfs_close`'d).
> - `firmware-tables.json` C:\ fallback gap accepted to TODO-29 §11 (below).
> **Verified:** 2026-06-17 | commit `fb846f65` | 5/5 items | build OK | smoke PASS (TCG 2.61s)
> **Accepted:** [M] `firmware-tables.json` hardcodes `X:\Diag` with no C:\ fallback -> XREF: 01-boot-platform/TODO-29 §11 (item: "`firmware-tables.json` C:\ fallback" at line 358)
> **Quality reviewed:** 2026-06-17 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 3H+2M fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 9. Host Tools -- SDK Reads BlackBox from Disk Image

Update host-side tools to locate and read the BlackBox partition from raw disk images.

- [x] `scripts/tools/read-blackbox.sh`: extracts all BlackBox directories from raw disk image using mtools
- [x] Uses `BB_OFFSET=68157440` (LBA 133120 * 512) matching Makefile
- [x] Extracts Logs/, Logs/Serial/, Boot/, Crash/, Perf/, Diag/, Tools/ to `build/blackbox-extract/`
- [x] Documents Linux mount: `sudo mount -o loop,offset=68157440,ro build/system-disk.img /mnt/blackbox`
- [x] Commit: `"tools: SDK reads BlackBox partition from disk images"`

**Test checkpoint:** `bash scripts/tools/read-blackbox.sh build/system-disk.img` extracts logs to a local directory. `mount` on Linux at the correct offset shows FAT32 with the directory structure.

> **Notes:**
> - `scripts/tools/read-blackbox.sh` extracts the BlackBox FAT32 partition from a raw disk image via mtools into `build/blackbox-extract/` (host SDK tool).
> - `BB_OFFSET` is now resolved from the build-emitted `<disk>.info` sidecar (data-only `sed` parse, never sourced) instead of a hardcoded value -- matches the Makefile's offset source of truth and survives layout changes.
> - Validates the FAT volume label is `BLACKBOX` (via `mlabel`) before extracting, so a stale offset cannot silently extract the ESP/recovery/IXFS volume as diagnostics.
> - Single recursive `mcopy -s` (under `set -e`, no `|| true`) extracts the whole tree incl. `Logs/Serial` + `Crash/WER` and propagates real mtools errors instead of reporting a clean zero-file extract.
> **Verified:** 2026-06-17 | commit `e33accea` | 4/4 items | build OK | manual (extracts BLACKBOX, label-validated, rc=0)
> **Quality reviewed:** 2026-06-17 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 2H+2M fixed | scope: N/A (host shell tool)

---

## 10. Disk Space Management -- Log Aging, Quota, Cleanup

128 MiB is generous but finite. Without space management, logs eventually fill the partition and writes fail silently. Windows limits CBS.log to ~20 MB with rotation; Linux logrotate enforces per-file and total quotas. BlackBox needs the same discipline.

- [x] `fat32_get_free_bytes(vol)` / `fat32_volume_from_root(root)` added to `fat32.h` / `fat32_ops.c`
- [x] After BlackBox mount: logs free space (`"BlackBox: N MiB free (N%)"`)
- [x] If free < 10%: cleanup runs -- deletes oldest Boot\ files beyond 10 sessions, then rotated `.N` logs in Logs\
- [x] MaxBootSessions = 10, MinFreeMiB = 16 (hardcoded defaults, TODO: wire to registry)
- [x] Cleanup log: `"BlackBox: cleanup freed N KiB (N files removed)"`
- [/] If critically low after cleanup: LOG_ERROR fires, but the C:\ redirect (`klog_using_blackbox=0`) is defeated by `klog_resolve_dir()` at `boot_storage.c:607` re-setting the flag -- durable-fallback fix deferred below
- [/] Durable low-space C:\ fallback: sticky `klog_blackbox_forced_off` honored by `klog_resolve_dir` -> XREF: 02-kernel-core/TODO-04 §15 (item: "Durable low-space C: fallback for the critical-low redirect")
  - Fixes the `[/]` item above: the critical-low redirect must survive the later `klog_resolve_dir` call that re-sets the flag. (`klog_disk.c`)
- [/] Boot\ retention precision: delete oldest by the `YYMMDDNN` filename, not `vfs_readdir` slot order (FAT32 slot reuse breaks monotonic age) -- collect into a bounded array, sort, prune the surplus. (`boot_storage.c` ~371)
- [/] Harden cleanup path builders: `path[64]` silently truncates long names (wrong `vfs_unlink` target) -- use `VFS_MAX_PATH` + fail-closed skip on overflow for both the Boot\ and Logs\ builders.
- [/] Budget + batch the unbounded Logs\ delete loop (`boot_storage.c` ~414): bounded victim list, cap per-boot deletes/bytes/time, pet `boot_progress()` between batches -- a full volume can stall boot past the WDAT watchdog.
- [/] Wire `MaxBootSessions`/`MinFreeMiB` from registry `HKLM\SYSTEM\BlackBox` (hardcoded 10/16 at `boot_storage.c:357,438`); closes the item-310 deferral.
  - The four `boot_storage.c` items above are parked, not blocked: they are TODO-24-owned follow-ups with no external prerequisite. Re-opening this file is what they wait on, so they carry no cross-TODO owner and the stranded-deferral sweep will not surface them.
- [x] Commit: `"kernel: BlackBox disk space management -- log aging and quota enforcement"`

**Test checkpoint:** Fill BlackBox with dummy files until < 10% free. Boot -> serial shows cleanup message with freed space. Boot sessions beyond 10 are pruned. Verify on QEMU WHPX, TCG, VirtualBox.

> **Notes:**
> - Disk-space cleanup runs when X:\ FAT32 is <10% free: prunes Boot\ sessions beyond MaxBootSessions(10) + rotated `*.log.N`/`*.jsonl.N` in Logs\, rechecks free, then aims to fall back to C:\ if still critical.
> - Review hardened two hazards: the rotated-log matcher now requires `VFS_FILE` + a `.log`/`.jsonl` infix (was deleting any `.<digit>` name), and the freed-bytes display is clamped against uint64 underflow.
> - Four refinements remain open (`[/]` -- items above): durable low-space C:\ fallback (redirect currently defeated by `klog_resolve_dir`), oldest-by-filename retention, path-builder hardening, registry-backed quota config.
> **Deferred:** [M] §10 critical-low C:\ redirect is defeated by `klog_resolve_dir()` re-setting the flag -> XREF: 01-boot-platform/TODO-24 §10 (item: "Durable low-space C:\ fallback" at line 322)
> **Deferred:** [M] Boot\ retention deletes `vfs_readdir` slot order, not oldest-by-filename -> XREF: 01-boot-platform/TODO-24 §10 (item: "Boot\ retention precision" at line 324)
> **Deferred:** [M] cleanup path builders silently truncate long names (`path[64]`) -> XREF: 01-boot-platform/TODO-24 §10 (item: "Harden cleanup path builders" at line 325)
> **Deferred:** [M] `MaxBootSessions`/`MinFreeMiB` hardcoded, not registry-backed -> XREF: 01-boot-platform/TODO-24 §10 (item: "Wire `MaxBootSessions`/`MinFreeMiB`" at line 327)

---

## 11. FAT32 Volume Label -- Set "BLACKBOX" at Format Time

Windows and Linux identify FAT32 volumes by their 11-character volume label (stored in BPB and root directory). Setting it at format time ensures `vol` command and Disk Management show "BLACKBOX" instead of "NO NAME".

- [x] `-n BLACKBOX` already passed to `mkfs.fat` (done in §1)
- [x] Verified: `mlabel` shows `BLACKBOX`
- [x] `fat32_init()` parses BS_VolLab (BPB offset 71, 11 bytes) into `vol->label`, space-trimmed
- [x] Log at mount: `FAT32: "BLACKBOX" 128 MiB, 8 sectors/cluster, root cluster 2`
- [x] Commit: `"build: set FAT32 volume label BLACKBOX at format time"`

**Test checkpoint:** `mlabel` shows BLACKBOX. Windows Disk Management shows "BLACKBOX (X:)" when disk is attached. Serial shows volume label on mount.

> **Notes:**
> - The BlackBox FAT32 volume label `BLACKBOX` is set at format time via `mkfs.fat -n` (Makefile, in §1), written to both the BPB label field and the root volume-label dirent.
> - `fat32_init` parses BS_VolLab (BPB offset 71, 11 bytes) into `vol->label` (space-trimmed) and logs it at mount (`fat32_ops.c:847`).
> - The FAT label is an identifier independent of the GPT partition name `BlackBox` (mixed case) that `partition.c` keys X: mounting on; `read-blackbox.sh` (§9) + `test_blackbox.c` validate the FAT label, the kernel mount uses the GPT name.
> **Verified:** 2026-06-17 | commit `afba47cd` | 4/4 items | build OK | manual (mlabel shows BLACKBOX)
> **Accepted:** [M] bootloader `media_role_locate_blackbox_fs()` finds BlackBox by FAT label, not GPT identity (split-brain risk vs the kernel's GPT-name mount) -> XREF: 01-boot-platform/TODO-06 §6 (item: "Harden `media_role_locate_blackbox_fs()`" at line 206)
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf; re-adversarial skipped -- no code fixes) | 1M accepted-XREF | scope: kernel-code-quality

---

## 12. Partition Health -- fsck on Mount, Dirty-Bit Check

FAT32 has a "dirty" bit (byte 0x41 in BPB, bit 0 of the word). If the OS crashed mid-write, the dirty bit is set. Check it on mount and optionally run fsck. (-> XREF: `05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` §8 -- FAT32 fsck implementation)

- [x] At BlackBox mount: `fat32_is_dirty()` checks FAT[1] bit 27
- [x] If dirty: logs warning + runs `fat32_run_fsck(vol, 1)` in repair mode
- [x] `fat32_mark_dirty()` sets dirty on mount (new `fat32_set_dirty_marker()`)
- [x] `fat32_mark_clean()` clears dirty in `acpi_shutdown()` before power-off
- [x] Public API: `fat32_is_dirty/mark_dirty/mark_clean/run_fsck` in `fat32.h`
- [/] FAT[1] high-nibble preservation in the dirty/clean markers -> XREF: 05-storage-filesystems/TODO-04 §6 (item: "FAT[1] high-nibble preservation")
  - `fat32_core.c:224` reads FAT[1] 28-bit-masked and writes it back, zeroing reserved bits 28-31. Read and write the raw 32-bit value and toggle only bit 27.
- [/] FAT marker write error propagation -> XREF: 05-storage-filesystems/TODO-04 §6 (item: "FAT marker write error propagation")
  - `mark_dirty`/`mark_clean` are void and bail mid-mirror, so a torn clean-marker reads false-clean on the next mount and skips fsck. Return status and fail not-clean.
- [/] fsck cycle guard for `walk_directory` -> XREF: 05-storage-filesystems/TODO-04 §6 (item: "fsck cycle guard")
  - `fat32_fsck.c:154` recurses into a directory's first cluster with no visited-check, so a cycle drives unbounded recursion and exhausts the stack.
- [/] fsck BPB geometry validation before repair -> XREF: 05-storage-filesystems/TODO-04 §6 (item: "fsck BPB geometry validation")
  - `fat32_fsck.c:227` never checks that `fat_size_sectors` covers the cluster range, so a forged tiny-FAT BPB makes repair write data sectors as FAT.
- [x] Commit: `"kernel: BlackBox FAT32 dirty-bit check and optional fsck on mount"`

**Test checkpoint:** Force unclean shutdown (kill QEMU mid-write). Next boot: serial shows "partition dirty" warning. After clean shutdown: no warning. Verify on QEMU WHPX, TCG.

**Regression risk:** fsck on mount adds boot latency. Bound to max 2 seconds; skip if partition is clean. Rollback: disable fsck call, keep dirty-bit logging only.

> **Notes:**
> - At BlackBox mount `fat32_is_dirty()` reads the FAT[1] clean-shutdown bit (27); a dirty volume runs `fat32_run_fsck(vol,1)`; `mark_dirty` on mount, `mark_clean` at `acpi_shutdown` (acpi.c:940).
> - Consistency-verified happy path: the constant has one definition, shutdown reaches `mark_clean` for X:, and `is_dirty` is populated at init before §3's BlackBox dirty check.
> - Four FAT32-robustness hardenings (open `[/]` items) are DEFERRED -- malformed/external-FAT defenses on shared `fat32_core.c`/`fat32_fsck.c`; the kernel-formatted BlackBox volume is trusted.
> **Deferred:** [H] FAT[1] reserved high bits (28-31) zeroed by both marker writers -> XREF: 01-boot-platform/TODO-24 §12 (item: "FAT[1] high-nibble preservation" at line 375)
> **Deferred:** [H] torn FAT-mirror marker write -> false-clean -> skips fsck (no error propagation) -> XREF: 01-boot-platform/TODO-24 §12 (item: "FAT marker write error propagation" at line 377)
> **Deferred:** [H] fsck `walk_directory` recurses on directory cycles -> stack exhaustion -> XREF: 01-boot-platform/TODO-24 §12 (item: "fsck cycle guard" at line 379)
> **Deferred:** [H] fsck repair writes outside FAT on forged BPB geometry -> XREF: 01-boot-platform/TODO-24 §12 (item: "fsck BPB geometry validation" at line 381)

---

## 13. WER Staging Area -- Error Reports in X:\Crash\WER\

Windows Error Reporting (WER) stages error reports in `C:\ProgramData\Microsoft\Windows\WER\` before upload. Impossible OS can stage structured error reports for crashed processes in `X:\Crash\WER\` -- enabling post-mortem analysis without a network connection.

> [!TIP]
> Neither Win11 WER nor Linux apport stages crash reports on a separate cross-platform-readable partition. BlackBox WER reports are FAT32-readable by any OS -- plug the disk into any machine and read the crash context.

- [x] `X:\Crash\WER\` already in boot skeleton (§4)
- [x] `wer_write_crash_report()` in `wer.c`: writes a JSON report for an unhandled general user fault routed through `except.c` (not vmm.c #PF, which is serial-only, nor the idt.c panic-only fallback)
- [x] Report: `{"pid","name","exception","status","fault_addr","rip","rsp","error_code","registers","cs","stack","user_trace_available"}` (status/fault_addr are `0xN` or `null`; TODO-23 §12 added status/fault_addr/stack/user_trace_available)
- [x] Wired into the user-fault exception terminal (`except.c`, before `panic_screen`); the idt.c unhandled-vector fallback is `panic_screen`-only (TODO-23 §12)
- [x] Filename: `PID_YYYYMMDDHHMMSS.json` (timestamp from wall clock)
- [x] Falls back to C:\ when BlackBox not mounted
- [/] WER in exception path: `wer_write_crash_report` (`except.c` user-fault terminal) does vfs I/O in the fault handler -- reentrancy/deadlock if the fault was in FS code; move off the exception path or make it lock-free + preallocated. [§13]
- [/] WER open-failure observability: on `vfs_open` NULL the writer returns silently -- emit a panic-safe serial-only diagnostic (normal `klog` may re-enter VFS here). [§13]
  - Both WER items are parked without a cross-TODO owner ON PURPOSE: the code lives in `except.c` / `wer.c`, which `02-kernel-core/TODO-23` owns, but none of its three open sections (18 unwind fixtures, 19 telemetry flavor, 20 kernel-SEH arming) covers WER persistence, and filing into a stamped one is a black hole. Naming an owner would need a new section there.
- [x] Commit: `"kernel: WER-style crash report staging in X:\\Crash\\WER\\"`

**Test checkpoint:** Trigger a user-mode general fault that routes through `except.c` (e.g. `#UD` from an illegal instruction). `X:\Crash\WER\` contains a JSON report with PID, exception code, and the crash frames. Note: a NULL dereference is a `#PF` handled by `vmm.c`, whose terminal emits only the serial WER line (`WerpReportFault`), not a JSON report -- a safe JSON path for #PF is deferred with the exception-path VFS-safety work (TODO-23 §12 design decision). Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Notes:**
> - `wer_write_crash_report` (wer.c) writes a JSON crash report (`{pid,name,exception,status,fault_addr,rip,rsp,error_code,registers,cs,stack,user_trace_available}`, filename `PID_YYYYMMDDHHMMSS.json`) to `X:\Crash\WER\` (C:\ fallback) from the `except.c` user-fault exception terminal (TODO-23 §12 enriched it; the idt.c fallback is panic-only).
> - Fixed a JSON-injection bug: `t->name` is now escaped (`"`,`\`,control -> `\u00XX`) with a `pos<820` bound, so a crafted/long name can't break the report or overflow `buf[1536]` (re-adversarial-confirmed margin).
> - Two robustness items remain open (`[/]`): WER persistence runs in the exception path (FS-reentrancy/deadlock risk) and open-failure is silent; the write-honesty (flush/return-check) is owned by §5's durable-write retrofit.
> **Accepted:** [M] WER writer ignores `vfs_write`/`vfs_close` returns + no `vfs_flush` (false success) -> XREF: 01-boot-platform/TODO-24 §5 (item: "Durable-write + write-success honesty retrofit for X:\ diagnostic writers" at line 193)
> **Deferred:** [H] WER persistence does VFS I/O in the exception path (reentrancy/deadlock if fault was in FS) -> XREF: 01-boot-platform/TODO-24 §13 (item: "WER in exception path" at line 413)
> **Deferred:** [M] WER open-failure is a silent drop with no panic-safe diagnostic -> XREF: 01-boot-platform/TODO-24 §13 (item: "WER open-failure observability" at line 414)

---

## 14. A/B Layout Compatibility -- 4+ Partition Coexistence

TODO-21 (A/B dual-slot boot) defines: EFI + Slot A IXFS + Slot B IXFS. With BlackBox the realized A/B layout is EFI + BlackBox + ABMeta + Slot A + Slot B + Recovery (the ABMeta + Recovery partitions were added by the TODO-21 dual-slot + TODO-22 recovery work after this section first shipped). Ensure `make-system-disk.c` supports both layouts via a build flag.

- [x] `--ab` flag: produces the A/B multi-partition layout (EFI + BlackBox + ABMeta + IXFS A + IXFS B + Recovery), emitting `META_*`/`RECOVERY_*` sidecar keys
- [x] Default (no flag): 3-partition layout (EFI + BlackBox + IXFS)
- [x] BlackBox 128 MiB in both layouts; IXFS slots split remaining space evenly (1 MiB aligned)
- [x] Kernel mounts BlackBox as X:\ by GPT name -- works regardless of partition count
- [x] GPT names: "Impossible OS A" / "Impossible OS B" in A/B mode
- [x] Commit: `"tools: make-system-disk A/B layout with BlackBox partition"`

**Test checkpoint:** `make-system-disk --ab` produces the A/B image; `fdisk -l` shows EFI + BlackBox + ABMeta + IXFS A + IXFS B + Recovery. Default (no `--ab`) shows EFI + BlackBox + IXFS. Kernel boots and mounts X:\ from either layout. Verify on QEMU WHPX.

> **Notes:**
> - `tools/make-system-disk.c` builds the GPT layout: default 3-partition (EFI + BlackBox + IXFS), `--ab` the A/B layout (EFI + BlackBox + ABMeta + IXFS A + IXFS B + Recovery); BlackBox is 128 MiB and layout-independent.
> - A realized-layout validation fails closed on bad ordering/overlap/wrap, under-min slots (<96 MiB), or recovery below the FAT32 floor; the kernel mounts X: by GPT name regardless of partition count.
> - This review corrected stale "4-partition" docs to the real layout (ABMeta + Recovery were added by the TODO-21 dual-slot + TODO-22 recovery work after §14 first shipped).
> **Verified:** 2026-06-17 | commit `1b507d85` | 5/5 items | build OK | manual (host disk builder; realized layout fail-closed validated)
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf; re-adversarial skipped -- docs-only fix) | 1M fixed | scope: N/A (host disk-build tool)

---

## 15. Boot Platform TODO Updates -- XREFs and Domain Sync

Update cross-references across affected TODOs.

- [x] `TODO-04-system-logging.md`: XREF to TODO-24 already present (line 32, after the removed legacy-logger row)
- [x] `TODO-10-bare-metal-hardening.md`: updated log path refs from C:\ to X:\, KLOG_DIR -> klog_dir
- [x] `TODO-10-bare-metal-hardening.md`: follow-up ownership cleanup landed; TODO-10 now treats log, crash, perf, and diagnostic file paths as prerequisites owned here
- [x] `TODO-14-boot-diagnostics.md`: crash dump paths updated to `X:\Crash\`
- [x] `TODO-27-crash-dump-generation.md`: all `C:\Impossible\System\CrashDumps\` -> `X:\Crash\`
- [/] `TODO-04-restore-recovery.md` (domain 10): crash dump paths PARTLY updated -- lines 157/286 still describe dumps in `CrashDumps\` rather than `X:\Crash\` (deferred below)
- [/] `TODO-04-release-qa.md` (domain 15): crash dump collection PARTLY updated -- line 356 still references `CrashDumps\` (deferred below)
- [x] `TODO-24` current state block: updated from 2-partition to 3-partition
- [x] CLAUDE.md: no stale references (does not mention partition layout)
- [/] Finish cross-domain crash-dump sync: `CrashDumps\` -> `X:\Crash\` in TODO-04-restore-recovery + TODO-04-release-qa; add reciprocal TODO-24 owner XREFs to TODO-27-crash-dump-generation (§7) + TODO-27-uefi-advanced (§8). [§15]
  - Parked: TODO-24-owned bookkeeping spanning four other TODO files, no external blocker. Left for a pass that re-opens this file rather than done piecemeal from a drain.
- [x] Commit: `"docs: update XREFs for BlackBox partition migration"`

**Test checkpoint:** All referenced TODO files have correct XREFs. No stale `C:\Impossible\System\Logs\` references remain in active TODO files.

> **Notes:**
> - Propagated the C:\ -> X:\ log/crash/perf/diag path migration across boot-platform + cross-domain TODOs (TODO-04 logging, TODO-10, TODO-14, TODO-27); fixed the current-state callout + a wrong evidence line-ref this review.
> - Corrected the stale current-state A/B count (4 -> 6 partitions) to match §14's realized layout.
> - Incomplete (open `[/]`/`[ ]`): two cross-domain TODOs still describe dumps in `CrashDumps\` and two TODO-27 consumers lack reciprocal TODO-24 ownership XREFs -- deferred below.
> **Deferred:** [M] cross-domain crash-dump path sync incomplete + missing reciprocal TODO-24 owner XREFs -> XREF: 01-boot-platform/TODO-24 §15 (item: "Finish cross-domain crash-dump sync" at line 465)

---

## 16. Crash/WER Report Retention in Partition Cleanup

§10's low-space cleanup prunes `Boot\` sessions and rotated `Logs\` files but ignores `X:\Crash\WER\*.json`. §13 stages one unique `PID_YYYYMMDDHHMMSS.json` per unhandled exception with no cap, so a user-mode crash loop can fill the 128 MiB partition until klog falls back to C:\ and crash diagnostics split across volumes. WER reports need the same retention discipline as logs (Win11 WER age-out + size cap; Linux apport / `coredumpctl` MaxUse).

- [x] `wer_prune_reports()` (`wer.c`) prunes `X:\Crash\WER\*.json` oldest-first beyond `MaxWerReports`, called from `boot_storage.c boot_phase2` (mount + the §10 `MinFreeMiB` low-space path)
- [x] `WER_MAX_REPORTS`=64 in `wer.h`; registry-wiring to `HKLM\SYSTEM\BlackBox` shares the §10 `MaxBootSessions`/`MinFreeMiB` deferred follow-up
- [x] Best-effort cap (batched oldest-first delete) once per boot at X: mount (Win11 `MaxArchiveCount` style) + the §10 low-space path; NOT after each WER write (ISR exception path -- VFS enum/delete compounds the §13 reentrancy risk, Codex design finding)
- [x] `crash_recovery.log` is already bounded -- `klog_crash_write_to_disk` rewrites it `VFS_O_TRUNC` from the capped `s_recovered` ring each boot; `.dmp` rotation stays `02-kernel-core/TODO-27` §5-§8
- [x] `wer_prune_reports` self-logs `"WER retention: pruned N old report(s) (cap M)"` when it prunes (the cleanup-log WER report)
- [x] Commit: `"kernel: WER/crash-report retention in BlackBox partition cleanup"` (commit `9dfb2307`)

**Test checkpoint:** Stage more than `MaxWerReports` dummy JSON files in `X:\Crash\WER\`; boot -> serial shows `"WER retention: pruned N"`; count drops to the cap; partition free stays above `MinFreeMiB`. Verify on QEMU WHPX, TCG, VirtualBox.

> **Test runner:** N/A (`wer_prune_reports` needs live `X:\` + staged reports; helpers are static) | validation: serial `"WER retention: pruned N"` on WHPX

> **Notes:**
> - WER retention: `wer_prune_reports()` (`wer.c`) prunes `X:\Crash\WER\*.json` toward `WER_MAX_REPORTS`=64 oldest-first by timestamp (malformed names first), deleting bounded batches per enumeration to amortize FAT32 dir-cache rebuilds; self-logs the count.
> - Best-effort under FAT32's 128-entry readdir window (no truncation signal); exact for the WER dir since `wer_write_crash_report` is its sole writer (pure `PID_*.json`).
> - Runs once per boot at X: mount + the §10 low-space cleanup (`boot_storage.c boot_phase2`); deliberately NOT from the ISR exception path (Codex design finding -- §13 reentrancy risk).
> - `crash_recovery.log` needs no separate age-out (§7's `VFS_O_TRUNC` rewrite from the capped ring already bounds it); `.dmp` rotation owned by TODO-27 §5-§8.

> **Verified:** 2026-06-17 | commit `92a05b47` | 5/5 items | build OK
> **Deferred:** [M] >32 undeletable WER reports halt the prune (bounded FNV-1a failed-set, hash-collision skip); corruption-only -- fsck-on-mount repairs the directory before prune runs -> XREF: 01-boot-platform/TODO-24 §12 (item: "fsck cycle guard" at line 379)
> **Quality reviewed:** 2026-06-17 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 4M fixed, 1M deferred | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                    | 🪟 Win11                   | 🐧 Linux                 | 🚀 Impossible OS                    |
| --- | -------------------------- | -------------------------- | ------------------------ | ----------------------------------- |
| 💎  | Separate log partition     | ✅ Recovery + WinRE        | ⚠️ /var/log on root      | ✅ §1 -- 3-part GPT + X:\           |
| 💎  | Cross-platform readable    | ✅ NTFS (with drivers)     | ✅ ext4 (with drivers)   | ✅ §1 -- FAT32 universal            |
| 💎  | Crash dump isolation       | ✅ C:\Windows\MEMORY.DMP   | ✅ /var/crash            | ✅ §7 -- X:\Crash\                  |
| ⭐  | Per-boot log files         | ❌ Not built-in            | ❌ Not built-in          | ✅ §6 -- X:\Boot\ + X:\Perf\        |
| ⭐  | Boot timeline JSON         | ❌ Not built-in            | ❌ Not built-in          | ✅ §6 -- X:\Perf\boot-timeline.json |
| 💎  | Auto-mount on host         | ✅ Windows assigns letter  | ✅ udisks2 auto-mount    | ✅ §1 -- Basic Data GUID            |
| 💎  | Volume label               | ✅ NTFS volume label       | ✅ e2label / fatlabel    | ✅ §11 -- BPB label parsed          |
| 💎  | Named partition discovery  | ✅ Volume label match      | ✅ LABEL= in fstab       | ✅ §3 -- GPT name match X:\         |
| 💎  | Structured diagnostics dir | ⚠️ Scattered in C:\Windows | ⚠️ /var/log + /sys       | ✅ §4+§8 -- Logs/Boot/Crash/Diag    |
| 💎  | Log space management       | ✅ CBS.log 20 MB cap       | ✅ logrotate + journald  | ✅ §10 -- aging + quota + cleanup   |
| 💎  | Crash-report retention     | ✅ WER age-out + size cap  | ✅ apport / coredumpctl  | ✅ §16 -- WER JSON + log prune      |
| 💎  | Dirty-bit / fsck on mount  | ✅ chkdsk on dirty FAT32   | ✅ fsck.fat on mount     | ✅ §12 -- dirty+fsck on mount       |
| ⭐  | Cross-platform WER staging | ⚠️ WER on NTFS only        | ⚠️ apport on ext4 only   | ✅ §13 -- FAT32 WER JSON reports    |
| 💎  | A/B + diagnostic coexist   | ❌ Recovery only           | ⚠️ A/B without diag part | ✅ §14 -- 4-partition --ab layout   |

> After S1-S9, Impossible OS has a dedicated diagnostic partition more organized than both Windows (scattered C:\Windows files) and Linux (everything in /var/log). FAT32 universality means any OS can read the flight recorder.
> S10-S12 add production-grade space management and partition health -- matching Win11 chkdsk and Linux logrotate.
> S13 (WER staging on FAT32) is a competitive edge -- neither Win11 nor Linux stages crash reports on a universally readable partition.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_blackbox()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).

- [x] `test_blackbox.c`: 11 tests -- X:\ mounted, 7 directories exist, klog_dir resolved, volume label BLACKBOX, free space > 0
- [x] All tests skip gracefully if BlackBox partition not present
- [x] `fat32_get_label()` public API added for volume label access
- [x] Registered in `test_runner_init()`: `test_register_blackbox()`
- [x] Commit: `"test: add BlackBox partition tests"`

---

## Verification

- [x] `bash scripts/build.sh clean` -> `=== BUILD OK ===` with 3-partition disk (verified 2026-04-05)
- [x] QEMU WHPX: serial shows `"BlackBox partition mounted as X:\"` and `"klog: writing to X:\Logs\Serial_26040501.log"` (verified 2026-04-05, 3 boot runs)
- [ ] QEMU TCG: same as WHPX
- [ ] VirtualBox: boot completes, X:\ accessible
- [ ] Bare metal: BlackBox partition visible in Windows Disk Management with drive letter
- [x] Host Linux: `mdir -i build/system-disk.img@@68157440 ::/` shows FAT32 with Logs/Boot/Crash/Perf/Diag/Tools + Crash/WER + Logs/Serial (verified 2026-04-05)
- [ ] Crash test: `crash_test=1` -> next boot recovery log appears in `X:\Crash\`
- [ ] No-BlackBox fallback: remove BlackBox partition from disk -> logs go to `C:\` with warning
- [ ] Commit: `"kernel: BlackBox service partition verified -- 3-partition GPT, X:\\ mount, log migration"`

> **Test runner:** `scripts\debug\kernel\run-blackbox-tests.bat` (SUITE=fs) | 11 blackbox tests (X:\ mount, 7 dirs, klog_dir, volume label, free space), 0 failures
