# TODO-02 — System Logging

> **Goal:** Complete the klog system from its current working foundation to a production-grade logging stack: per-subsystem log splitting, log rotation, structured JSON events, rate limiting, and remote syslog forwarding. The core klog infrastructure (ring buffer, disk flush, serial/framebuffer output, numbered boot logs, user-mode syscall) is already implemented and is documented in the Completed section below for reference.

> [!IMPORTANT]
> **Current state:** `klog.c` (380 lines) + `klog_disk.c` (578 lines). All entries go to a single `kernel.log` — per-subsystem splitting, rotation, structured events, and remote forwarding are not yet implemented.

## Inputs

- [`src/kernel/klog.c`](../../src/kernel/klog.c)
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c)
- [`include/kernel/klog.h`](../../include/kernel/klog.h)
- [`src/kernel/log.c`](../../src/kernel/log.c) — legacy serial-only logger (reference)
- [`todo/02-kernel-core/TODO-01-kernel-init-sequencing.md`](./TODO-01-kernel-init-sequencing.md)
- → XREF: `TODO-20-kernel-libraries.md §6` — cJSON DOM parser required by §6 (structured JSON events)
- → XREF: `06-networking/TODO-01-tcp-network-infrastructure.md` — UDP send path required by §7 (remote syslog); `src/kernel/net/udp.c` already exists but syslog send API is not yet wired

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
| ⭐  |   6   | Structured JSON log events          | §4, T20 §6    |  [ ]   |
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

## 1. Boot-Phase Aware klog Init `[Sonnet]`

The current `klog_disk.c` assumes VFS is available when it initialises. After TODO-01, the kernel has explicit Phase 0 (no VFS) and Phase 2 (VFS ready) gates. `klog` must split into a Phase 0 ring-buffer-only mode and a Phase 2 disk-enable step.

- [x] Add `klog_early_init()` — Phase 0 safe; resets ring buffer, serial output only; no VFS
- [x] Add `klog_disk_enable()` — Phase 2 safe; calls `klog_disk_init()` + `klog_disk_flush()` to open log files and flush ring to disk
- [x] Remove any VFS calls from the path triggered during Phase 0 (klog ring + serial are static; no VFS calls before Phase 2)
- [x] Register `SUBSYS_KLOG` as ready after `klog_early_init()` in Phase 0
- [x] Update `boot_hw.c` (Phase 0) to call `klog_early_init()` and `boot_storage.c` (Phase 2) to call `klog_disk_enable()`
- [x] Commit: `"kernel: split klog early-init from disk-enable"`

## 2. Per-Subsystem Log Splitting `[Sonnet]`

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

## 3. Per-Subsystem Verbosity Control `[Sonnet]`

Allow silencing verbose subsystems in release builds without recompiling.

- [x] Add `klog_set_level(const char *subsystem, log_level_t min_level)` to `klog.h`/`klog.c`
- [x] Store per-subsystem min levels in a 32-entry override table keyed by tag string (pointer + strcmp)
- [x] In `klog()`: look up `subsys_min_level()` before writing to ring buffer; drop entries below threshold
- [x] `klog_load_levels_from_registry()`: reads `HKLM\SYSTEM\Logs\Levels\<tag>` for 16 known subsystems via `RegReadKeyValue()`
- [x] Default: all subsystems at `LOG_DEBUG` (global min); adjustable via `klog_set_level(NULL, level)` or per-tag
- [x] Commit: `"kernel: per-subsystem log verbosity control"`

## 4. Log Rotation `[Sonnet]`

Prevent log files growing unbounded on long-running or repeatedly booted systems.

- [x] Before each `klog_disk_flush()`: check `kernel_log_size` against threshold
- [x] If size exceeds threshold: `rotate_log_file()` shifts `.1` → `.2` → `.3`, renames current to `.1`, creates fresh empty file
- [x] Keep at most N rotated files; delete oldest (`.N`) when N is exceeded
- [x] Rotation applies to kernel.log; per-subsystem files share the same `rotate_log_file()` function
- [x] `load_rotation_config()` reads `MaxSize` (default: 4 MB) from `HKLM\SYSTEM\Logs\MaxSize` via `RegReadKeyValue()`
- [x] Reads `MaxRotated` (default: 3, max: 9) from `HKLM\SYSTEM\Logs\MaxRotated`
- [x] O(1) check: `kernel_log_size` tracked in static var, updated from `logfile->size` after each flush
- [x] Commit: `"kernel: log rotation"`

## 5. Rate Limiting `[Sonnet]`

Prevent a misbehaving subsystem from flooding the log and starving disk I/O.

- [x] Track per-subsystem message count within a 100-tick sliding window (100 Hz = 1 second); 32-slot table
- [x] When a subsystem exceeds the rate: drop entries and emit one summary: `"[<tag>] rate limit active (>N msgs/sec)"`
- [x] Reset the counter at window expiry (checked on each `klog()` call via `system_get_ticks()`)
- [x] Rate limit thresholds configurable per subsystem via Registry: `HKLM\SYSTEM\Logs\RateLimit\<tag>` (REG_DWORD)
- [x] Dropped count tracked in `klog_rate_slot_t.dropped` for future `events.jsonl` integration (§6)
- [x] Commit: `"kernel: log rate limiting"`

## 6. Structured JSON Log Events `[Sonnet]`

Emit machine-parseable events alongside plain-text logs. Requires cJSON from `TODO-20-kernel-libraries.md` §6.

- [ ] Define JSON event format: `{"ts":<ms>,"lvl":"WARN","sub":"net","msg":"DHCP timeout","dropped":0}`
- [ ] Add `klog_json_flush()` — serialise ring buffer entries to `C:\Impossible\System\Logs\events.jsonl`
- [ ] JSON Lines format — one JSON object per line; append-only
- [ ] Open `events.jsonl` once at `klog_disk_enable()` time; keep handle cached
- [ ] Apply §4 log rotation to `events.jsonl`
- [ ] Include `"dropped"` field from §5 rate limiter in each affected event
- [ ] Task Manager log viewer panel reads `events.jsonl` for colour-coded filtering by level and subsystem
- [ ] Commit: `"kernel: structured JSON log events"`

## 7. Remote Syslog Forwarding (RFC 5424) `[Sonnet]`

Forward log entries to a remote syslog server for enterprise and headless debug use. `src/kernel/net/udp.c` already exists — this section wires syslog packet sending on top of it.

- [ ] Read syslog server IP from `HKLM\SYSTEM\Logs\SyslogServer` at `klog_disk_enable()`; skip if not set
- [ ] Map klog levels to RFC 5424 severity: `DEBUG→7`, `INFO→6`, `WARN→4`, `ERROR→3`, `FATAL→2`
- [ ] Facility: `LOG_KERN (0)`
- [ ] Format each packet: `<PRI>1 TIMESTAMP HOSTNAME APPNAME - - - MSG`
- [ ] In `klog_disk_flush()`: if syslog is configured, also send formatted packet via UDP port 514
- [ ] Queue entries in the ring buffer if network is not yet up; drain queue once network is ready
- [ ] Graceful no-op if network goes down mid-session — never block the flush path waiting for network
- [ ] Commit: `"kernel: syslog UDP forwarding"`

---

## OS Comparison

| ⭐  | Feature                       | 🪟 Windows 11                        | 🐧 Linux (journald / syslog)            | 🚀 Impossible OS                                    |
| --- | ----------------------------- | ------------------------------------- | --------------------------------------- | ---------------------------------------------------- |
| 💎  | Unified kernel log            | ✅ Windows Event Log                 | ✅ journald + syslog                    | ✅ Done — klog ring buffer + disk flush             |
| 💎  | Log levels (5+)               | ✅ 5 levels                          | ✅ 8 POSIX levels                       | ✅ Done — DEBUG / INFO / WARN / ERROR / FATAL       |
| 💎  | Serial debug output           | ⚠️ Requires WinDbg or DebugPrint     | ✅ `earlyprintk=serial`                 | ✅ Done — all entries to serial with level prefix   |
| 💎  | Numbered per-boot log files   | ❌ Not built-in                      | ❌ Not built-in                         | ✅ Done — `BOOT_NNN.LOG` on FAT32 per boot session  |
| 💎  | User-mode log syscall         | ✅ `ReportEvent()` / ETW             | ✅ `syslog()` / write to `/dev/log`     | ✅ Done — `SYS_LOG` syscall #17                     |
| 💎  | Boot-phase aware init         | ✅ Phase 0 / Phase 1 safe logging    | ✅ `early_printk` before VFS            | ⬜ Planned — §1                                     |
| 💎  | Per-subsystem log splitting   | ✅ Event channels per source         | ✅ syslog facilities                    | ⬜ Planned — §2                                     |
| 💎  | Per-subsystem verbosity       | ✅ ETW session level filters         | ✅ per-facility log level               | ⬜ Planned — §3                                     |
| 💎  | Log rotation                  | ✅ Automatic (Event Log size limit)  | ✅ logrotate                            | ⬜ Planned — §4                                     |
| 💎  | Rate limiting                 | ✅ ETW rate limiting built-in        | ⚠️ rsyslog rate-limit, not journald     | ⬜ Planned — §5                                     |
| ⭐  | Structured human-readable log | ❌ XML (verbose, hard to read raw)   | ❌ Binary journal (requires journalctl) | ⬜ **Planned — §6 JSON Lines, any editor**          |
| 💎  | Remote log forwarding         | ✅ Windows Event Forwarding (WEF)    | ✅ rsyslog / syslog UDP                 | ⬜ Planned — §7                                     |
| ⭐  | Serial timestamp every entry  | ❌ Not standard                      | ❌ Not standard                         | ✅ Done — **Impossible OS only**                    |

> **After §1–§5:** Impossible OS matches Windows and Linux on all core logging capabilities.
> **After §6:** `events.jsonl` beats both — Windows XML is verbose and Linux binary journal requires `journalctl`. Impossible OS logs are readable by any text editor, `grep`, `jq`, or custom tooling with zero dependencies.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log confirms `klog_early_init()` runs in Phase 0 before VFS
- [ ] Headless QEMU serial log confirms `klog_disk_enable()` runs in Phase 2 after VFS ready
- [ ] `C:\Impossible\System\Logs\network.log` exists and contains only `"net"` tagged entries
- [ ] `C:\Impossible\System\Logs\boot.log` exists and contains only `"boot"` tagged entries
- [ ] Forcing `klog_set_level("mm", LOG_WARN)` silences mm DEBUG entries in serial log
- [ ] Writing beyond `MaxSize` threshold causes rotation: `kernel.log.1` appears, new `kernel.log` starts fresh
- [ ] `events.jsonl` contains one valid JSON object per line; parseable by `jq`
- [ ] Enabling syslog in Registry causes entries to appear on a test syslog server (UDP 514)
- [ ] Commit: `"kernel: system-logging verified — splitting, rotation, JSON events, syslog"`
