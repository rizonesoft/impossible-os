# P0106 — System Logging

> **Goal:** Unified klog system with ring buffer, disk persistence, and per-subsystem log splitting.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Unified Logging System 🔄

**Prompt:** Verify the unified logging system implementation. The codebase has `klog.h`/`klog.c` with 5 levels (DEBUG → FATAL), subsystem tags, 1000-entry ring buffer, serial+framebuffer output with colored prefixes, and FATAL auto-halt. `klog_flush.c` writes to `C:\Impossible\System\Logs\kernel.log`. `SYS_LOG` syscall (#17) lets user-mode apps log. Per-subsystem log file splitting (network.log, boot.log) is not yet implemented — all entries go to a single kernel.log. After completing remaining items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: per-subsystem log files"`. Update `README.md` if it contains stale or incorrect references to logging. Add notes, gotchas, and design decisions directly in this TODO section covering the klog system, log levels, subsystem tags, ring buffer, disk persistence, and per-subsystem splitting.

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

**Prompt:** Extend `klog_flush.c` to route log entries by subsystem tag to separate log files. Each entry's subsystem tag (e.g., `"net"`, `"fs"`, `"mm"`, `"boot"`) is matched against a dispatch table mapping tags to file paths. Entries with no matching tag go to `kernel.log`. Files are opened once at boot and kept open (VFS handles are cached). Add `klog_set_level(subsystem, level)` so per-subsystem verbosity can be controlled (e.g., silence `"mm"` DEBUG in release). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: per-subsystem log files"`.


- [ ] Define dispatch table: `"net"` → `network.log`, `"boot"` → `boot.log`, `"fs"` → `fs.log`, `"mm"` → `mm.log`
- [ ] Route entries by subsystem tag in `klog_flush.c`
- [ ] Cache open VFS handles (don't re-open on each flush)
- [ ] Implement `klog_set_level(subsystem, level)` — per-subsystem verbosity control
- [ ] Commit: `"kernel: per-subsystem log files"`

---

## 3. Log Rotation

**Prompt:** Prevent log files from growing unbounded. When `kernel.log` exceeds a configurable size threshold (default: 4 MB, stored in Registry `HKLM\SYSTEM\Logs\MaxSize`), rename it to `kernel.log.1` and start a new `kernel.log`. Keep at most N rotated files (default: 3). Log rotation should happen at flush time (checked before each write). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: log rotation"`.


- [ ] Check file size before each `klog_flush()` call
- [ ] If size > threshold: rotate — rename `kernel.log` → `kernel.log.1`, create new `kernel.log`
- [ ] Delete oldest rotated file to keep max N backups (default: 3)
- [ ] Read threshold from Registry `HKLM\SYSTEM\Logs\MaxSize` (default: 4 MB)
- [ ] Apply rotation to per-subsystem logs too
- [ ] Commit: `"kernel: log rotation"`

---

## Priority Order

| Priority | Section                     | Reason                                          |
|----------|-----------------------------|-------------------------------------------------|
| 🔄 In Progress | 1. Unified Logging   | Core done; per-subsystem splitting pending      |
| 🟠 P1     | 2. Per-Subsystem Splitting  | Reduces noise in kernel.log; easier debugging   |
| 🟡 P2     | 3. Log Rotation             | Prevents disk fill on long-running sessions     |
