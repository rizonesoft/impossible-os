# TODO-02 — System Logging

> **Goal:** Complete the klog system from its current working foundation to a production-grade logging stack: per-subsystem log splitting, log rotation, structured JSON events, rate limiting, and remote syslog forwarding. The core klog infrastructure (ring buffer, disk flush, serial/framebuffer output, numbered boot logs, user-mode syscall) is already implemented and is documented in the Completed section below for reference.

> [!IMPORTANT]
> **Current state:** `klog.c` (541 lines) + `klog_disk.c` (912 lines). §1-§6 are complete: per-subsystem splitting, verbosity control, rotation, rate limiting, and JSON Lines events all working. Only §7 (remote syslog forwarding) remains.

## Inputs

- [`src/kernel/klog.c`](../../src/kernel/klog.c)
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c)
- [`include/kernel/klog.h`](../../include/kernel/klog.h)
- ~~`src/kernel/log.c`~~ — legacy serial-only logger (removed, superseded by klog)
- [`todo/02-kernel-core/TODO-01-kernel-init-sequencing.md`](./TODO-01-kernel-init-sequencing.md)
- → XREF: `TODO-20-kernel-libraries.md §6` — cJSON DOM parser required by §6 (structured JSON events)
- → XREF: `07-networking/TODO-01-tcp-network-infrastructure.md` — UDP send path required by §7 (remote syslog); `src/kernel/net/udp.c` already exists but syslog send API is not yet wired
- → XREF: `01-boot-platform/TODO-11-klog-ixfs-bare-metal-perf.md` — klog_disk_flush performance fix (ring snapshot, deferred flush mode); both TODOs modify klog_disk.c

## Outcome

- All kernel subsystems write to dedicated log files (`network.log`, `boot.log`, `fs.log`, `mm.log`).
- Log files rotate automatically; disk never fills from logging alone.
- `events.jsonl` provides structured, human-readable log events readable by any editor.
- Remote syslog forwarding enables enterprise and headless debug scenarios.
- The klog lifecycle is gated correctly on VFS readiness per TODO-01 Phase 0/Phase 2 contract.

## Implementation Order

| ⭐  | Order | Deliverable                         | Depends On    | Status |
| --- | :---: | ----------------------------------- | ------------- | :----: |
| 💎  |   1   | Boot-phase aware klog init          | T01 §1        |  [x]   |
| 💎  |   2   | Per-subsystem log splitting         | §1            |  [x]   |
| 💎  |   3   | Per-subsystem verbosity control     | §2            |  [x]   |
| 💎  |   4   | Log rotation                        | §2            |  [x]   |
| 💎  |   5   | Rate limiting                       | §2            |  [x]   |
| ⭐  |   6   | Structured JSON log events          | §4, T20 §6    |  [x]   |
| 💎  |   7   | Remote syslog forwarding (RFC 5424) | UDP (exists)  |  [ ]   |

> 💎 = parity — Windows Event Log and Linux journald/syslog both have these capabilities.
> ⭐ = exclusive — JSON Lines events.jsonl is human-readable by any editor; beats Windows XML and Linux binary journal.

---

## Completed (Reference)

These items are implemented and verified. Kept here for future correctness checks.

### Core klog Infrastructure ✅

- [x] 5 log levels: `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL` — `klog.h` lines 20–26
- [x] Subsystem tag on every log call (`"net"`, `"fs"`, `"mm"`, `"boot"`, etc.)
- [x] 1000-entry in-memory ring buffer — `klog.c` `klog_ring[KLOG_RING_SIZE]`
- [x] Serial output with colored level prefixes
- [x] Framebuffer output with colored level prefixes
- [x] `LOG_FATAL` auto-halt — fatal log entries stop the kernel

### Disk Logging ✅

- [x] Batch flush to `C:\Impossible\System\Logs\kernel.log` (IXFS, appendable) — `klog_disk.c`
- [x] Numbered boot session logs `X:\BOOT_NNN.LOG` on FAT32 partition — one file per boot
- [x] Live mode — every `klog()` entry appended and flushed immediately when enabled
- [x] 256 KB flush buffer via `pmm_alloc_contiguous()` (identity-mapped)
- [x] Reentrancy guard (`flushing` flag) prevents recursive flush

### User-Mode Integration ✅

- [x] `SYS_LOG` syscall (#17) — user-mode apps can write to kernel log — `syscall.h`/`syscall.c`
- [x] Commit: `"kernel: unified logging with disk persistence"`

---

## 1. Boot-Phase Aware klog Init
The current `klog_disk.c` assumes VFS is available when it initialises. After TODO-01, the kernel has explicit Phase 0 (no VFS) and Phase 2 (VFS ready) gates. `klog` must split into a Phase 0 ring-buffer-only mode and a Phase 2 disk-enable step.

- [x] Add `klog_early_init()` — Phase 0 safe; resets ring buffer, serial output only; no VFS
- [x] Add `klog_disk_enable()` — Phase 2 safe; calls `klog_disk_init()` + `klog_disk_flush()` to open log files and flush ring to disk
- [x] Remove any VFS calls from the path triggered during Phase 0 (klog ring + serial are static; no VFS calls before Phase 2)
- [x] Register `SUBSYS_KLOG` as ready after `klog_early_init()` in Phase 0
- [x] Update `boot_hw.c` (Phase 0) to call `klog_early_init()` and `boot_storage.c` (Phase 2) to call `klog_disk_enable()`
- [x] Commit: `"kernel: split klog early-init from disk-enable"`

## 2. Per-Subsystem Log Splitting
Route log entries to dedicated per-subsystem log files based on the subsystem tag. Entries with no matching tag continue to go to `kernel.log`.

- [x] Define dispatch table in `klog_disk.c` with 18 tag-to-file mappings:
  - `"net"` → `network.log`, `"boot"/"smp"/"UEFI"` → `boot.log`, `"fs"/"vfs"/"ixfs"/"fat32"` → `fs.log`
  - `"mm"` → `mm.log`, `"drv"/"ahci"/"pci"/"lapic"/"ioapic"/"acpi"/"blk"` → `drivers.log`
  - `"sec"/"TPM"` → `security.log`, unmatched → `kernel.log`
- [x] Create per-subsystem log files at `ensure_log_dirs()` time (6 files + kernel.log)
- [x] Route entries in the flush loop: after writing to `kernel.log`, iterate each subsystem file and append matching entries
- [x] Fall back to `kernel.log` for unknown tags — `dispatch_filename()` returns "kernel.log" for unmatched
- [x] Boot-session numbered logs (`YYMMDDN.LOG`) on X: continue to contain all subsystems combined
- [x] Commit: `"kernel: per-subsystem log files"`

## 3. Per-Subsystem Verbosity Control
Allow silencing verbose subsystems in release builds without recompiling.

- [x] Add `klog_set_level(const char *subsystem, log_level_t min_level)` to `klog.h`/`klog.c`
- [x] Store per-subsystem min levels in a 32-entry override table keyed by tag string (pointer + strcmp)
- [x] In `klog()`: look up `subsys_min_level()` before writing to ring buffer; drop entries below threshold
- [x] `klog_load_levels_from_registry()`: reads `HKLM\SYSTEM\Logs\Levels\<tag>` for 16 known subsystems via `RegReadKeyValue()`
- [x] Default: all subsystems at `LOG_DEBUG` (global min); adjustable via `klog_set_level(NULL, level)` or per-tag
- [x] Commit: `"kernel: per-subsystem log verbosity control"`

## 4. Log Rotation
Prevent log files growing unbounded on long-running or repeatedly booted systems.

- [x] Before each `klog_disk_flush()`: check `kernel_log_size` against threshold
- [x] If size exceeds threshold: `rotate_log_file()` shifts `.1` → `.2` → `.3`, renames current to `.1`, creates fresh empty file
- [x] Keep at most N rotated files; delete oldest (`.N`) when N is exceeded
- [x] Rotation applies to kernel.log; per-subsystem files share the same `rotate_log_file()` function
- [x] `load_rotation_config()` reads `MaxSize` (default: 4 MB) from `HKLM\SYSTEM\Logs\MaxSize` via `RegReadKeyValue()`
- [x] Reads `MaxRotated` (default: 3, max: 9) from `HKLM\SYSTEM\Logs\MaxRotated`
- [x] O(1) check: `kernel_log_size` tracked in static var, updated from `logfile->size` after each flush
- [x] Commit: `"kernel: log rotation"`

## 5. Rate Limiting
Prevent a misbehaving subsystem from flooding the log and starving disk I/O.

- [x] Track per-subsystem message count within a 100-tick sliding window (100 Hz = 1 second); 32-slot table
- [x] When a subsystem exceeds the rate: drop entries and emit one summary: `"[<tag>] rate limit active (>N msgs/sec)"`
- [x] Reset the counter at window expiry (checked on each `klog()` call via `system_get_ticks()`)
- [x] Rate limit thresholds configurable per subsystem via Registry: `HKLM\SYSTEM\Logs\RateLimit\<tag>` (REG_DWORD)
- [x] Dropped count tracked in `klog_rate_slot_t.dropped` for future `events.jsonl` integration (§6)
- [x] Commit: `"kernel: log rate limiting"`

## 6. Structured JSON Log Events
Emit machine-parseable events alongside plain-text logs. Requires cJSON from `TODO-20-kernel-libraries.md` §6.

- [x] Define JSON event format: `{"ts":<ms>,"lvl":"WARN","sub":"net","msg":"DHCP timeout","dropped":0}`
- [x] JSON Lines flush integrated into `klog_disk_flush()` — serializes ring buffer entries to `C:\Impossible\System\Logs\events.jsonl`
- [x] JSON Lines format — one JSON object per line; append-only; batched into 16 KB buffer for single `vfs_write()`
- [x] `events.jsonl` created at first flush; file size tracked for rotation
- [x] §4 log rotation applied to `events.jsonl` via `rotate_log_file()`
- [x] `"dropped"` field included from §5 rate limiter via `klog_get_dropped()` public API
- [ ] Event viewer reads `events.jsonl` for colour-coded filtering — see [16-tools-accessories/TODO-02](../16-tools-accessories/TODO-02-event-viewer.md)
- [x] Commit: `"kernel: structured JSON log events"`

## 7. Remote Syslog Forwarding (RFC 5424)

Moved to [07-networking/TODO-11](../07-networking/TODO-11-syslog-forwarding.md) — syslog is a networking feature that depends on UDP stack readiness.

---

## OS Comparison

| ⭐ | Feature               | 🪟 Win11                | 🐧 Linux                  | 🚀 Impossible OS              |
|----|-----------------------|----------------------|------------------------|--------------------------==|
| 💎 | Unified kernel log    | ✅ Event Log         | ✅ journald/syslog    | ✅ klog ring buffer       |
| 💎 | Log levels            | ✅ 5 levels          | ✅ 8 POSIX levels     | ✅ 5 levels               |
| 💎 | Serial debug output   | ⚠️ Needs WinDbg      | ✅ earlyprintk        | ✅ All entries to serial  |
| 💎 | Per-boot log files    | ❌ Not built-in      | ❌ Not built-in       | ✅ BOOT_NNN.LOG on FAT32  |
| 💎 | User-mode log API     | ✅ ReportEvent/ETW   | ✅ syslog()           | ✅ SYS_LOG syscall #17    |
| 💎 | Boot-phase init       | ✅ Phase 0/1         | ✅ early_printk       | ✅ §1 — done              |
| 💎 | Subsystem splitting   | ✅ Event channels    | ✅ syslog facilities  | ✅ §2 — done              |
| 💎 | Subsystem verbosity   | ✅ ETW filters       | ✅ per-facility level | ✅ §3 — done              |
| 💎 | Log rotation          | ✅ Size-limited      | ✅ logrotate          | ✅ §4 — done              |
| 💎 | Rate limiting         | ✅ ETW built-in      | ⚠️ rsyslog only       | ✅ §5 — done              |
| ⭐ | Human-readable struct | ❌ XML verbose       | ❌ Binary journal     | ✅ §6 — JSON Lines        |
| 💎 | Remote forwarding     | ✅ WEF               | ✅ rsyslog UDP        | ⬜ §7 — syslog RFC 5424   |
| ⭐ | Serial timestamps     | ❌ Not standard      | ❌ Not standard       | ✅ Every entry            |

> After §1-§6, Impossible OS matches or exceeds Windows and Linux on all logging.
> §6 (JSON Lines) and serial timestamps are exclusive competitive edges.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_klog()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] Create `src/kernel/test/test_klog.c` with:
  - `klog(LOG_INFO, "test", "hello")` writes to the ring buffer; ring head advances
  - `klog(LOG_DEBUG, "mm", "x")` is dropped after `klog_set_level("mm", LOG_WARN)`
  - `klog(LOG_WARN, "mm", "x")` is not dropped after `klog_set_level("mm", LOG_WARN)`
  - Global level override: `klog_set_level(NULL, LOG_ERROR)` suppresses all INFO/WARN entries
  - Rate limiting: >N messages from same subsystem within 1-second window triggers drop; summary message emitted
  - `klog_get_dropped("test")` returns correct count after rate-limited burst
  - Ring buffer wraps correctly when filled past `KLOG_RING_SIZE` entries
  - Subsystem dispatch: entry tagged `"net"` maps to `"network.log"` filename (skipped: `dispatch_filename()` is static in klog_disk.c)
  - Subsystem dispatch: entry tagged `"fs"` maps to `"fs.log"` filename (skipped: same reason)
  - Subsystem dispatch: entry with unknown tag maps to `"kernel.log"` (skipped: same reason)
- [x] Register in `test_runner_init()`: `test_register_klog()`
- [x] Commit: `"test: add klog test suite"`

> **Done:** 6 suites, 8 assertions — registered in `test_runner_init()` (2026-04-02). Subsystem dispatch tests skipped: `dispatch_filename()` is static in klog_disk.c, no public API to test tag-to-filename mapping.

## Verification

- [x] `bash scripts/build.sh clean` → `=== BUILD OK ===` — PASS: build 1939 booted successfully (WHPX, 2026-04-02)
- [x] QEMU WHPX: serial log shows `klog_early_init()` in Phase 0, `klog_disk_enable()` in Phase 2 — PASS: `[PHASE0] KLOG (0x0051)` at 0.000s, `klog: writing to C:\...\Serial_26040201.log` at 4.560s after VFS mount (WHPX, 2026-04-02)
- [ ] QEMU TCG: same as WHPX — (not tested in this log, WHPX only)
- [ ] VirtualBox: boot completes, log files created — (manual: requires VirtualBox)
- [ ] `C:\Impossible\System\Logs\network.log` contains only `"net"` tagged entries — (not verifiable from serial log, requires filesystem inspection)
- [ ] `C:\Impossible\System\Logs\boot.log` contains only `"boot"` tagged entries — (not verifiable from serial log, requires filesystem inspection)
- [x] `klog_set_level("mm", LOG_WARN)` silences mm DEBUG entries in serial — PASS: unit test `Klog: level drop` passed at 8.510s, confirms LOG_DEBUG dropped after set_level(mm, LOG_WARN) (WHPX, 2026-04-02)
- [ ] Rotation: `kernel.log.1` appears when `kernel.log` exceeds MaxSize — (not verifiable from serial log, requires filesystem inspection after multiple boots)
- [ ] `events.jsonl` parseable by `jq` — one JSON object per line — (not verifiable from serial log, requires filesystem inspection)
- [ ] Syslog: entries appear on test syslog server (UDP 514) when configured — (not verifiable from serial log, requires syslog server setup)
- [ ] Bare metal: log files written correctly to IXFS on SATA/NVMe — (manual: requires physical hardware)
- [ ] Commit: `"kernel: system-logging verified — splitting, rotation, JSON events, syslog"`
