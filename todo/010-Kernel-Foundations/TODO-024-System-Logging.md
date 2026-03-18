# P0106 — System Logging

> **Goal:** Unified klog system with ring buffer, disk persistence, and per-subsystem log splitting.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Unified Logging System 🔄

**Prompt:** Verify the unified logging system implementation. The codebase has `klog.h`/`klog.c` with 5 levels (DEBUG → FATAL), subsystem tags, 1000-entry ring buffer, serial+framebuffer output with colored prefixes, and FATAL auto-halt. `klog_flush.c` writes to `C:\Impossible\System\Logs\kernel.log`. `SYS_LOG` syscall (#17) lets user-mode apps log. Per-subsystem log file splitting (network.log, boot.log) is not yet implemented — all entries go to a single kernel.log. After completing remaining items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: per-subsystem log files"`. Add notes, gotchas, and design decisions directly in this TODO section covering the klog system, log levels, subsystem tags, ring buffer, disk persistence, and per-subsystem splitting.

**Implementation files:** `include/kernel/klog.h`, `src/kernel/klog.c`, `src/kernel/klog_flush.c`, `include/kernel/log.h` (legacy, serial-only)

- [x] Add `LOG_DEBUG` and `LOG_FATAL` levels (currently: INFO, WARN, ERROR) — `klog.h` lines 20-26
- [x] Add source/subsystem tag to all log calls (e.g., `"net"`, `"fs"`, `"mm"`) — used throughout `main.c`
- [x] Implement in-memory ring buffer (last 1000 log entries) — `klog.c` `klog_ring[KLOG_RING_SIZE]`
- [/] Implement periodic flush to disk:
  - [x] `C:\Impossible\System\Logs\kernel.log` — `klog_flush.c`, called from `main.c`
  - [ ] `C:\Impossible\System\Logs\network.log` — not yet (all logs go to kernel.log)
  - [ ] `C:\Impossible\System\Logs\boot.log` — not yet (all logs go to kernel.log)
- [x] Add `sys_log()` syscall for user-mode apps to log — `SYS_LOG=17` in `syscall.h`/`syscall.c`
- [x] Commit: `"kernel: unified logging with disk persistence"`

---

## 2. Per-Subsystem Log Splitting

**Prompt:** Extend `klog_flush.c` to route log entries by subsystem tag to separate log files. Each entry's subsystem tag (e.g., `"net"`, `"fs"`, `"mm"`, `"boot"`) is matched against a dispatch table mapping tags to file paths. Entries with no matching tag go to `kernel.log`. Files are opened once at boot and kept open (VFS handles are cached). Add `klog_set_level(subsystem, level)` so per-subsystem verbosity can be controlled (e.g., silence `"mm"` DEBUG in release). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: per-subsystem log files"`. Add notes directly in this TODO section.


- [ ] Define dispatch table: `"net"` → `network.log`, `"boot"` → `boot.log`, `"fs"` → `fs.log`, `"mm"` → `mm.log`
- [ ] Route entries by subsystem tag in `klog_flush.c`
- [ ] Cache open VFS handles (don't re-open on each flush)
- [ ] Implement `klog_set_level(subsystem, level)` — per-subsystem verbosity control
- [ ] Commit: `"kernel: per-subsystem log files"`

---

## 3. Log Rotation

**Prompt:** Prevent log files from growing unbounded. When `kernel.log` exceeds a configurable size threshold (default: 4 MB, stored in Registry `HKLM\SYSTEM\Logs\MaxSize`), rename it to `kernel.log.1` and start a new `kernel.log`. Keep at most N rotated files (default: 3). Log rotation should happen at flush time (checked before each write). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: log rotation"`. Add notes directly in this TODO section.


- [ ] Check file size before each `klog_flush()` call
- [ ] If size > threshold: rotate — rename `kernel.log` → `kernel.log.1`, create new `kernel.log`
- [ ] Delete oldest rotated file to keep max N backups (default: 3)
- [ ] Read threshold from Registry `HKLM\SYSTEM\Logs\MaxSize` (default: 4 MB)
- [ ] Apply rotation to per-subsystem logs too
- [ ] Commit: `"kernel: log rotation"`

---

## 4. Structured Logging (JSON Events)

**Prompt:** Plain-text logs are hard to parse programmatically. Windows Event Log stores structured XML; Linux `journald` stores binary structured records. Impossible OS can emit structured JSON log events alongside the plain-text log — each entry is a JSON object `{"ts": 1234, "lvl": "WARN", "sub": "net", "msg": "DHCP timeout"}`. Written to `C:\Impossible\System\Logs\events.jsonl` (JSON Lines format — one JSON object per line). The Task Manager's log viewer reads this for color-coded filtering by level and subsystem. Requires cJSON from TODO-027 §4. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: structured JSON log events"`. Add notes directly in this TODO section.

> **Prerequisite:** cJSON (TODO-027 §4) and per-subsystem splitting (§2) must exist first.

> **Beats:** Linux journald requires special tools to read. Impossible OS events.jsonl is readable by any text editor or tool.


- [ ] Define JSON log entry format: `{"ts":<ms>, "lvl":"<LEVEL>", "sub":"<tag>", "msg":"<text>"}`
- [ ] Add `klog_json_flush()` — serialize ring buffer entries to `events.jsonl` in JSON Lines format
- [ ] Append-only: open `events.jsonl` once at boot, write one line per event
- [ ] Apply log rotation to `events.jsonl` (reuse §3 mechanism)
- [ ] Task Manager reads `events.jsonl` for structured log viewer panel
- [ ] Commit: `"kernel: structured JSON log events"`

---

## 5. Remote Log Forwarding (Syslog UDP)

**Prompt:** For enterprise use and remote debugging, forward kernel log entries to a remote syslog server (RFC 5424 / UDP port 514). The syslog server address is read from Registry `HKLM\SYSTEM\Logs\SyslogServer` at boot. If not set, forwarding is disabled (zero overhead). Severity levels map: `LOG_DEBUG→7`, `LOG_INFO→6`, `LOG_WARN→4`, `LOG_ERROR→3`, `LOG_FATAL→2`. Facility: `LOG_KERN(0)`. This is standard on Linux servers; Windows uses Windows Event Forwarding (WEF). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: syslog UDP forwarding"`. Add notes directly in this TODO section.

> **Prerequisite:** Network stack (UDP send) must be working — see TODO-070 Networking.


- [ ] Read syslog server IP from Registry `HKLM\SYSTEM\Logs\SyslogServer` at boot
- [ ] Map klog levels to RFC 5424 severity numbers
- [ ] In `klog_flush()`: if syslog configured, also send formatted syslog packet via UDP
- [ ] Format: `<PRI>VERSION TIMESTAMP HOSTNAME APPNAME MSGID STRUCTURED-DATA MSG`
- [ ] Graceful skip if network not yet up — queue events until network is available
- [ ] Commit: `"kernel: syslog UDP forwarding"`

---

## Priority Order

| Priority       | Section                      | Reason                                                |
|----------------|------------------------------|-------------------------------------------------------|
| 🔄 In Progress | 1. Unified Logging           | Core done; per-subsystem splitting pending            |
| 🟠 P1          | 2. Per-Subsystem Splitting   | Reduces noise in kernel.log; easier debugging         |
| 🟠 P1          | 3. Log Rotation              | Prevents disk fill on long-running sessions           |
| 🟡 P2          | 4. Structured JSON Logging   | Task Manager log viewer; human-readable by any editor |
| 🟡 P2          | 5. Remote Syslog Forwarding  | Enterprise/debug use; needs network stack             |

---

## OS Comparison

| Feature                       | 🪟 Windows Event Log | 🐧 Linux journald / syslog   | 🚀 Impossible OS                      |
| ----------------------------- | ------------------- | --------------------------- | ------------------------------------ |
| Unified kernel log            | ✅ Event Log         | ✅ `journald` + syslog       | ✅ §1 Done — klog with ring buffer    |
| Log levels (5+)               | ✅ 5 levels          | ✅ 8 POSIX levels            | ✅ Done — DEBUG/INFO/WARN/ERROR/FATAL |
| Per-subsystem splitting       | ✅ Event channels    | ✅ `syslog` facilities       | ⬜ §2 P1                              |
| Log rotation                  | ✅ Auto-rotation     | ✅ `logrotate`               | ⬜ §3 P1                              |
| Structured logging            | ✅ XML records       | ✅ Binary journal            | ⬜ §4 P2 — **JSON Lines, readable**   |
| Remote forwarding             | ✅ WEF / WinRM       | ✅ `rsyslog` UDP/TCP         | ⬜ §5 P2                              |
| Serial timestamp prefix       | ❌                   | ❌                           | ✅ Done — **Impossible OS only**      |
| **Human-readable structured** | ❌ XML is verbose    | ❌ Binary (needs journalctl) | ⬜ **§4 JSON Lines — beats both**     |

> **After §4:** `events.jsonl` is readable by any editor — beats Windows XML and Linux binary journal.
