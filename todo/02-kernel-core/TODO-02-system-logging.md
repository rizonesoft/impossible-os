# TODO-02 — System Logging

> **Goal:** Complete the klog system from its current working foundation to a
> production-grade logging stack: per-subsystem log splitting, log rotation,
> structured JSON events, rate limiting, and remote syslog forwarding.
> The core klog infrastructure (ring buffer, disk flush, serial/framebuffer
> output, numbered boot logs, user-mode syscall) is already implemented and
> is documented in the Completed section below for reference.

> [!IMPORTANT]
> **Current state:** `klog.c` (380 lines) + `klog_disk.c` (578 lines).
> All entries go to a single `kernel.log` — per-subsystem splitting, rotation,
> structured events, and remote forwarding are not yet implemented.

## Inputs

- [`src/kernel/klog.c`](../../src/kernel/klog.c)
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c)
- [`include/kernel/klog.h`](../../include/kernel/klog.h)
- [`src/kernel/log.c`](../../src/kernel/log.c) — legacy serial-only logger (reference)
- [`todo/02-kernel-core/TODO-01-kernel-init-sequencing.md`](./TODO-01-kernel-init-sequencing.md)
- → XREF: `TODO-20-kernel-libraries.md` §6 — cJSON DOM parser required by §6 (structured JSON events)
- → XREF: `TODO-06-networking` — UDP send path required by §7 (remote syslog); `src/kernel/net/udp.c` already exists but syslog send API is not yet wired

## Outcome

- All kernel subsystems write to dedicated log files (`network.log`, `boot.log`, `fs.log`, `mm.log`).
- Log files rotate automatically; disk never fills from logging alone.
- `events.jsonl` provides structured, human-readable log events readable by any editor.
- Remote syslog forwarding enables enterprise and headless debug scenarios.
- The klog lifecycle is gated correctly on VFS readiness per TODO-01 Phase 0/Phase 2 contract.

## Implementation Order

| ⭐  | Order | Deliverable                         | Depends On | Status |
| --- | :---: | ----------------------------------- | ---------- | :----: |
| 💎  |   1   | Boot-phase aware klog init          | TODO-01 §1 |  [ ]   |
| 💎  |   2   | Per-subsystem log splitting         | 1          |  [ ]   |
| 💎  |   3   | Per-subsystem verbosity control     | 2          |  [ ]   |
| 💎  |   4   | Log rotation                        | 2          |  [ ]   |
| 💎  |   5   | Rate limiting                       | 2          |  [ ]   |
| ⭐  |   6   | Structured JSON log events          | 4, TODO-20 §6 |  [ ]   |
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

The current `klog_disk.c` assumes VFS is available when it initialises. After TODO-01,
the kernel has explicit Phase 0 (no VFS) and Phase 2 (VFS ready) gates.
`klog` must split into a Phase 0 ring-buffer-only mode and a Phase 2 disk-enable step.

- [ ] Add `klog_early_init()` — Phase 0 safe; initialises ring buffer and serial output only; no VFS
- [ ] Add `klog_disk_enable()` — Phase 2 safe; opens log files on VFS and starts disk flushing; BOOT_REQUIRE(SUBSYS_VFS)
- [ ] Remove any VFS calls from the path triggered during Phase 0 (before `SUBSYS_VFS` is ready)
- [ ] Register `SUBSYS_KLOG` as ready after `klog_early_init()` and `SUBSYS_KLOG_DISK` after `klog_disk_enable()`
- [ ] Update `main.c`/`boot_storage.c` call sites to use the split init
- [ ] Commit: `"kernel: split klog early-init from disk-enable"`

## 2. Per-Subsystem Log Splitting `[Sonnet]`

Route log entries to dedicated per-subsystem log files based on the subsystem tag.
Entries with no matching tag continue to go to `kernel.log`.

- [ ] Define dispatch table in `klog_disk.c`:
  - `"net"` → `C:\Impossible\System\Logs\network.log`
  - `"boot"` → `C:\Impossible\System\Logs\boot.log`
  - `"fs"` → `C:\Impossible\System\Logs\fs.log`
  - `"mm"` → `C:\Impossible\System\Logs\mm.log`
  - `"drv"` → `C:\Impossible\System\Logs\drivers.log`
  - `"sec"` → `C:\Impossible\System\Logs\security.log`
  - unmatched → `kernel.log`
- [ ] Open each file once at `klog_disk_enable()` time; cache VFS handles
- [ ] Route entries in the flush loop by matching the subsystem tag against the dispatch table
- [ ] Fall back to `kernel.log` for unknown tags — do not drop entries
- [ ] Also write boot-session numbered logs (`BOOT_NNN.LOG`) for all subsystems combined
- [ ] Commit: `"kernel: per-subsystem log files"`

## 3. Per-Subsystem Verbosity Control `[Sonnet]`

Allow silencing verbose subsystems in release builds without recompiling.

- [ ] Add `klog_set_level(const char *subsystem, log_level_t min_level)` to `klog.h`/`klog.c`
- [ ] Store per-subsystem min levels in a small hash map keyed by subsystem tag string
- [ ] In `klog()`: look up the subsystem's min level before writing to ring buffer; drop entries below threshold
- [ ] Read initial per-subsystem levels from Registry at `klog_disk_enable()` time:
  `HKLM\SYSTEM\Logs\Levels\<subsystem>` → level string (`"DEBUG"`, `"INFO"`, etc.)
- [ ] Default: all subsystems at `LOG_DEBUG` in debug builds, `LOG_INFO` in release
- [ ] Commit: `"kernel: per-subsystem log verbosity control"`

## 4. Log Rotation `[Sonnet]`

Prevent log files growing unbounded on long-running or repeatedly booted systems.

- [ ] Before each `klog_disk_flush()`: check current file size via `vfs_stat()`
- [ ] If size exceeds threshold: rotate — rename `kernel.log` → `kernel.log.1`; open new `kernel.log`
- [ ] Keep at most N rotated files; delete oldest when N is exceeded
- [ ] Apply rotation to all per-subsystem log files (§2 dispatch table)
- [ ] Read `MaxSize` (default: 4 MB) from `HKLM\SYSTEM\Logs\MaxSize` — Registry key, integer bytes
- [ ] Read `MaxRotated` (default: 3) from `HKLM\SYSTEM\Logs\MaxRotated`
- [ ] Rotation check is O(1) — size is tracked in the open-file state, not re-stat'd every call
- [ ] Commit: `"kernel: log rotation"`

## 5. Rate Limiting `[Sonnet]`

Prevent a misbehaving subsystem from flooding the log and starving disk I/O.

- [ ] Track per-subsystem message count within a sliding window (default: 100 msgs / 1 second)
- [ ] When a subsystem exceeds the rate: drop entries and emit one summary: `"[net] rate limit: N entries dropped"`
- [ ] Reset the counter at the start of each window tick (driven by the timer subsystem)
- [ ] Rate limit thresholds are configurable per subsystem via Registry: `HKLM\SYSTEM\Logs\RateLimit\<subsystem>`
- [ ] Dropped entry counts are included in the `events.jsonl` structured record (§6)
- [ ] Commit: `"kernel: log rate limiting"`

## 6. Structured JSON Log Events `[Sonnet]`

Emit machine-parseable events alongside plain-text logs.
Requires cJSON from `TODO-20-kernel-libraries.md` §6.

- [ ] Define JSON event format: `{"ts":<ms>,"lvl":"WARN","sub":"net","msg":"DHCP timeout","dropped":0}`
- [ ] Add `klog_json_flush()` — serialise ring buffer entries to `C:\Impossible\System\Logs\events.jsonl`
- [ ] JSON Lines format — one JSON object per line; append-only
- [ ] Open `events.jsonl` once at `klog_disk_enable()` time; keep handle cached
- [ ] Apply §4 log rotation to `events.jsonl`
- [ ] Include `"dropped"` field from §5 rate limiter in each affected event
- [ ] Task Manager log viewer panel reads `events.jsonl` for colour-coded filtering by level and subsystem
- [ ] Commit: `"kernel: structured JSON log events"`

## 7. Remote Syslog Forwarding (RFC 5424) `[Sonnet]`

Forward log entries to a remote syslog server for enterprise and headless debug use.
`src/kernel/net/udp.c` already exists — this section wires syslog packet sending on top of it.

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
- [ ] `commit: "kernel: complete logging stack — splitting, rotation, JSON events, syslog"`
