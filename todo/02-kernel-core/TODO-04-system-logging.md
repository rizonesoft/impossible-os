---
schema_version: 1
id: system-logging
domain: 02-kernel-core
status: active
title: "TODO-04 -- System Logging"
---

# TODO-04 -- System Logging

> **Validated:** 2026-06-21 | validate-todo-file clean (structure / IO table / XREF / test wiring; inter-section `---` separators added)
> **Gap-audited:** 2026-06-21 | codex-gap-audit + parity-research-analyst (9 Win11/Linux gaps) in commit e5c56d54 -- §10 dep-drift fix + §11 ETW provider/session filtering, §12 ETW stack-walk/autologger/schema, §13 rotated-log LZ4 compression added (Codex-endorsed TODO-04 ownership of etw.c)

> **Goal:** Complete the klog system from its current working foundation to a production-grade logging stack: per-subsystem log splitting, log rotation, structured JSON events, rate limiting, and remote syslog forwarding. The core klog infrastructure (ring buffer, disk flush, serial/framebuffer output, numbered boot logs, user-mode syscall) is already implemented and is documented in the Completed section below for reference.

> [!IMPORTANT]
> **Current state:** `klog.c` (~580 lines) + `klog_disk.c` (912 lines) + `etw.c`. §1-§9 complete (old-system; pending dual-stamp review). Remaining: §10 (log integrity) -- UNBLOCKED, TODO-03 §5 Monocypher + kernel CSPRNG shipped (`crypto_blake2b` + `csprng_fill`); §11-§13 (ETW provider/session filtering, ETW stack-walk/autologger, rotated-log compression) added by the 2026-06-21 gap-audit -- §13 LZ4 compression now also unblocked by TODO-03 §3. Remote syslog moved to `07-networking/TODO-11`.

## Inputs

- [`src/kernel/klog.c`](../../src/kernel/klog.c)
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c)
- [`include/kernel/klog.h`](../../include/kernel/klog.h)
- [`src/kernel/etw.c`](../../src/kernel/etw.c)
- [`include/kernel/etw.h`](../../include/kernel/etw.h)
- ~~`src/kernel/log.c`~~ -- legacy serial-only logger (removed, superseded by klog)
- [`todo/02-kernel-core/TODO-01-kernel-init-sequencing.md`](./TODO-01-kernel-init-sequencing.md)
- → XREF: `TODO-03-kernel-libraries.md §4` -- cJSON DOM parser; §4 was implemented with manual JSON formatting in `klog_disk.c` (cJSON not needed for serialization, may be used by Event Viewer for parsing)
- → XREF: `07-networking/TODO-11-syslog-forwarding.md` -- remote syslog forwarding (moved from this TODO); depends on UDP send path in `07-networking/TODO-01`
- → XREF: `01-boot-platform/TODO-19-usb-boot-hardening.md` -- USB boot hardening (klog_disk_flush bounded loop, deferred flush mode); both TODOs modify klog_disk.c
- → XREF: `TODO-12-native-api-ssdt.md §5` -- SSDT indices 0x01D0–0x01D6 reserved for ETW tracing syscalls; §7 of this TODO wires them into the SSDT
- → XREF: `TODO-27-crash-dump-generation.md §2,§7` -- crash dump raw-partition sink bypasses VFS; §8 of this TODO captures ring buffer to reserved physical memory on panic (complementary -- TODO-27 captures binary state, §8 captures text log)
- → XREF: `TODO-28-bsod-ux-enhancements.md` §3: BSOD last N klog lines at panic (`klog_get_recent`); §8 persists ring for next boot replay. Panic reads stay memcpy only when `klog_entry_t` grows (T04 §9).
- → XREF: `TODO-03-kernel-libraries.md §5` -- Monocypher Blake2b + kernel CSPRNG required by §10 (HMAC-chain log integrity)
- → XREF: `01-boot-platform/TODO-24-blackbox-service-partition.md` -- BlackBox X:\ partition; log paths migrate from C:\ to X:\Logs\
- → XREF: `14-host-tools/TODO-08-blackbox-log-extractor.md` -- host-side log viewer/extractor; solves "not verifiable from serial log" verification items
- → XREF: [`TODO-32-kernel-logging-v2-lockless.md`](./TODO-32-kernel-logging-v2-lockless.md) -- v2 architecture: per-CPU lockless rings, priority lanes, fail-proof FATAL, native structured fields. SUPERSEDES this TODO's §5 (rate limit) and reorganises §9 (per-entry context); retains §1-§4, §6, §7, §8, §10 unchanged.
- → XREF: [`TODO-02-kernel-configuration-policy.md §9`](./TODO-02-kernel-configuration-policy.md) -- policy tamper/security audit events persist as durable structured events via §7 ETW
- → XREF: [`TODO-23-exception-dispatch-seh.md`](./TODO-23-exception-dispatch-seh.md) -- `RtlCaptureStackBackTrace` required by §12 ETW stack-walk
- → XREF: [`TODO-18-kernel-image-module-registry.md §4`](./TODO-18-kernel-image-module-registry.md) -- symbol provider required by §12 to symbolize ETW stack frames
- → XREF: [`TODO-03-kernel-libraries.md §3`](./TODO-03-kernel-libraries.md) -- LZ4 block compression required by §13 rotated-log compression
- → XREF: `TODO-15-security-reference-monitor.md` + `10-platform-services` (SACL audit generation deferred there) + `TODO-12 §24` (syscall audit hook) -- own security-event CONTENT (threat-intel provider, object-access audit); §7/§11 ETW is the transport, not the owner

## Outcome

- All kernel subsystems write to dedicated log files on X:\Logs\ (`network.log`, `boot.log`, `fs.log`, `mm.log`).
- Log files rotate automatically; disk never fills from logging alone.
- `events.jsonl` provides structured, human-readable log events readable by any editor.
- Remote syslog forwarding (-> `07-networking/TODO-11`) enables enterprise and headless debug scenarios.
- The klog lifecycle is gated correctly on VFS readiness per TODO-01 Phase 0/Phase 2 contract.
- On panic, the last N ring buffer entries survive reboot via reserved physical memory and are recovered into `X:\Crash\crash_recovery.log`.
- Every log entry carries CPU number, PID, and TID for SMP and multi-process debugging.
- `events.jsonl` entries are HMAC-chained -- tampering is mathematically detectable without external tools.
- ETW providers register stable GUIDs and sessions filter by keyword + level, so events route only to interested sessions.
- ETW captures per-event call stacks, runs a boot-persistent autologger, and self-describes event schemas.
- Rotated log files are LZ4-compressed, keeping more history in the same disk budget.

## Implementation Order

| ⭐  | Order | Deliverable                                                         | Depends On       | Status |
| --- | :---: | ------------------------------------------------------------------- | ---------------- | :----: |
| 💎  |   1   | Boot-phase aware klog init                                          | T01 §1           |  [x]   |
| 💎  |   2   | Per-subsystem log splitting                                         | §1               |  [/]   |
| 💎  |   3   | Per-subsystem verbosity control                                     | §2               |  [x]   |
| 💎  |   4   | Log rotation                                                        | §2               |  [x]   |
| 💎  |   5   | Rate limiting                                                       | §2               |  [x]   |
| ⭐  |   6   | Structured JSON log events                                          | §4               |  [x]   |
| 💎  |   7   | ETW tracing syscalls wired to SSDT                                  | §4, T12 §4       |  [x]   |
| 💎  |   8   | Crash-persistent log capture                                        | §1               |  [x]   |
| 💎  |   9   | Per-entry context metadata                                          | §2               |  [x]   |
| ⭐  |  10   | Log integrity verification (HMAC)                                   | §6, T03 §5       |  [/]   |
| 💎  |  11   | ETW provider registration + filtering                               | §7, T12 §5       |  [/]   |
| 💎  |  12   | ETW advanced capture (stack/autologger/schema)                      | §11, T23, T18 §4 |  [/]   |
| 💎  |  13   | Rotated-log compression (LZ4)                                       | §4, T03 §3       |  [x]   |
| ⭐  |  14   | Serial timestamp render bound                                       | §1               |  [ ]   |
| ⭐  |  15   | Post-ship follow-up backfill (2026-07-31 cohort)                    | --               |  [ ]   |
| 💎  |  16   | Klog assertions and scans that depend on nothing else having logged | §1               |  [ ]   |
| ⭐  |  17   | Bounded wait until the sinks have caught up to a given sequence     | §1, §2           |  [ ]   |

> 💎 = parity -- Windows Event Log and Linux journald/syslog both have these capabilities.
> ⭐ = exclusive -- HMAC-chained JSON Lines is human-readable AND cryptographically verifiable; beats Windows XML and Linux binary journal.

---

## 1. Boot-Phase Aware klog Init
The current `klog_disk.c` assumes VFS is available when it initialises. After TODO-01, the kernel has explicit Phase 0 (no VFS) and Phase 2 (VFS ready) gates. `klog` must split into a Phase 0 ring-buffer-only mode and a Phase 2 disk-enable step.

- [x] Add `klog_early_init()` -- Phase 0 safe; ring + serial only, no VFS. A no-op contract marker that PRESERVES Phase 0 ring entries (the §1 review removed a ring reset that wiped 30 boot_phase0 klog lines)
- [x] Add `klog_disk_enable()` -- Phase 2 safe; `klog_disk_init()` (now idempotent, no C:\DEBUG 256 KiB double-init leak) + crash-write + flush; returns `int` (1 iff disk logging is live = buffer + mounted target)
- [x] Remove any VFS calls from the path triggered during Phase 0 (klog ring + serial are static; no VFS calls before Phase 2)
- [x] Register `SUBSYS_KLOG` as ready after `klog_early_init()` in Phase 0
- [x] Update `boot_hw.c` (Phase 0) to call `klog_early_init()` and `boot_storage.c` (Phase 2) to call `klog_disk_enable()`, POST16-bracketed and gated on the live-status return
- [x] Commit: `"kernel: split klog early-init from disk-enable"`

**Test checkpoint:** boot reaches `C:\>` (smoke); Phase 0 klog lines (boot_info / UEFI / TPM) survive into the on-disk log (not wiped by early_init); on real media `POST16_KLOG_DISK_OK` (0x0053) appears only when disk logging is live; C:\DEBUG boot does not double-allocate the FAT32 buffer. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Notes:**
> - Shipped the Phase 0 / Phase 2 klog split: `klog_early_init()` (ring + serial, no VFS) + `klog_disk_enable()` (Phase 2 VFS-backed), wired at `boot_hw.c` (Phase 0) + `boot_storage.c` (Phase 2).
> - §1 review fixed 3 real bugs (detail in commit message): klog_early_init ring-reset wiped Phase 0 history, klog_disk_init double-init leaked 256 KiB on C:\DEBUG, boot POST16 over-reported disk-log success.
> - Canonical doc: `include/kernel/klog.h` (split-init contract banner).
> - Scope boundary: §1 owns the boot-phase init split + its POST16 telemetry; §8 owns crash-persistent capture (and the verified-first-flush-persistence follow-up); per-subsystem splitting/verbosity are §2/§3.
> **Verified:** 2026-06-21 | ship `c4a453d8` + review fixes | 6/6 items | build OK | smoke PASS (TCG 2.5s); 6800 kernel + 16 user PASS
> **Deferred:** [M] boot POST16_KLOG_DISK_OK still reports success on mounted-but-unwritable media (flush open/write failure) -- needs a verified-first-flush-persistence flag (reason: in-scope, owned by the disk-flush path) -> XREF: 02-kernel-core/TODO-04-system-logging.md §8 (item: "Verified-persistence flag in `klog_disk_flush()`")
> **Quality reviewed:** 2026-06-21 | Codex 6x (adversarial, consistency, perf, re-adversarial) | 1H+3M fixed, 1M deferred | scope: kernel-code-quality

---

## 2. Per-Subsystem Log Splitting
Route log entries to dedicated per-subsystem log files based on the subsystem tag. Entries with no matching tag continue to go to `kernel.log`.

- [x] Define dispatch table in `klog_disk.c` with 18 tag-to-file mappings:
  - `"net"` → `network.log`, `"boot"/"smp"/"UEFI"` → `boot.log`, `"fs"/"vfs"/"ixfs"/"fat32"` → `fs.log`
  - `"mm"` → `mm.log`, `"drv"/"ahci"/"pci"/"lapic"/"ioapic"/"acpi"/"blk"` → `drivers.log`
  - `"sec"/"TPM"` → `security.log`, unmatched → `kernel.log`
- [x] Create per-subsystem log files at `ensure_log_dirs()` time (6 files + kernel.log)
- [x] Route entries in the flush loop: after writing to `kernel.log`, iterate each subsystem file and append matching entries
- [x] Fall back to `kernel.log` for unknown tags -- `dispatch_filename()` returns "kernel.log" for unmatched
- [x] Boot-session numbered logs (`YYMMDDN.LOG`) on X: continue to contain all subsystems combined
- [x] Per-subsystem files now created on first write (`vfs_open ... VFS_O_CREATE`) so the X:\Logs BlackBox path (dirs-only skeleton) gets the split logs, not just `kernel.log` (§2 review fixed silent-skip)
- [ ] Live-mode (C:\DEBUG) runs per-subsystem routing on every per-line `klog_disk_flush()` -- batch it to periodic/final drains (needs `klog_disk_flush_all` to clear `live_enabled` so the drain still routes) (§2 review perf)
- [ ] Left-align flag (`%-Ns`) in `klog`: the parser takes zero-pad + width but no `-`, so `%-18s` is emitted LITERALLY and its argument never consumed, shifting every later field. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §10`
- [x] Commit: `"kernel: per-subsystem log files"`

**Test checkpoint:** after a flush, `X:\Logs\network.log` / `boot.log` / `fs.log` / `mm.log` / `drivers.log` / `security.log` exist and contain their tagged entries (not just `kernel.log`); an unknown tag routes to `kernel.log`; the numbered boot log keeps all subsystems combined. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Notes:**
> - Shipped per-subsystem log splitting in `klog_disk.c`: `s_dispatch[]` tag-to-file table + `dispatch_filename`/`klog_dispatch_slot` (kernel.log fallback), single-pass `slot_of[]` routing in the flush.
> - §2 review fixed a [H] false-completeness bug: split files were never created on the primary X:\Logs BlackBox path (dirs-only skeleton + `VFS_O_WRITE` open), leaving only kernel.log; now opened `VFS_O_WRITE | VFS_O_CREATE`.
> - Routing is lock-free (ring read without s_klog_lock; no ring lock across VFS I/O), serialized by the `flushing` guard.
> - Canonical doc: `src/kernel/klog_disk.c` dispatch/flush banner.
> - Scope boundary: §2 owns routing + file creation; rotation of split files is §4, the raw-subsystem-pointer lifetime is §9, rate limiting is §5.
> **Verified:** 2026-06-21 | ship `c055339f` + review fixes | 6/7 items | build OK | smoke PASS (TCG 2.75s); 6800 kernel + 16 user PASS
> **Accepted:** [M] per-subsystem split logs bypass rotation (grow past MaxSize) -> XREF: 02-kernel-core/TODO-04-system-logging.md §4 (item: "Rotate the per-subsystem split logs")
> **Accepted:** [M] ring stores the raw caller `subsystem` pointer (delayed-routing lifetime hazard) -> XREF: 02-kernel-core/TODO-04-system-logging.md §9 (item: "Store the subsystem tag as a bounded `char[16]` copy")
> **Deferred:** [M] live-mode (C:\DEBUG) runs per-subsystem routing on every per-line flush (VFS churn) (reason: in-scope perf, needs live_enabled-clear semantics) -> XREF: 02-kernel-core/TODO-04-system-logging.md §2 (item: "Live-mode (C:\DEBUG) runs per-subsystem routing")
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf) + auditor | 1H fixed, 3M deferred | scope: kernel-code-quality

---

## 3. Per-Subsystem Verbosity Control
Allow silencing verbose subsystems in release builds without recompiling.

- [x] Add `klog_set_level(const char *subsystem, log_level_t min_level)` to `klog.h`/`klog.c`
- [x] Store per-subsystem min levels in a 32-entry override table keyed by tag string (pointer + strcmp)
- [x] In `klog()`: look up `subsys_min_level()` before writing to ring buffer; drop entries below threshold
- [x] `klog_load_levels_from_registry()`: reads `HKLM\SYSTEM\Logs\Levels\<tag>` (REG_SZ level) + `HKLM\SYSTEM\Logs\RateLimit\<tag>` (REG_DWORD, validated `val_size == 4`) for 18 known subsystems (added `ioapic`, `blk`) via `RegReadKeyValue()`
- [x] Default: all subsystems at `LOG_DEBUG` (global min); adjustable via `klog_set_level(NULL, level)` or per-tag
- [x] Commit: `"kernel: per-subsystem log verbosity control"`

**Test checkpoint:** `klog_set_level("net", LOG_ERROR)` then a `klog(LOG_INFO, "net", ...)` is dropped (not in ring); `klog_get_level("net")` returns `LOG_ERROR`; `klog_remove_override("net")` restores the global default. SMP: the override table is a seqlock -- a concurrent reader never observes a torn `{tag, min_level}` pair during add/edit/remove.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_klog suite, 0 failures
> **Notes:**
> - `klog_set_level` / `klog_get_level` / `klog_has_override` / `klog_remove_override` + 18-tag registry loader in `src/kernel/klog.c`; 32-entry override table keyed by tag pointer + `str_eq` fallback.
> - Override table is the repo canonical seqlock (`include/kernel/sched/seqlock.h`): writers serialize on its spinlock; hot per-`klog()` reader + cold query APIs use `seqlock_read_begin/retry` -- no torn read during swap-with-last removal.
> - Review fixes: replaced a lock-free count-publish (then a hand-rolled gen) with `seqlock_t`; added `ioapic`/`blk` registry tags; validated REG_DWORD `val_size`.
> - Sub-threshold drops never enter the ring buffer (filtered before store), bounding disk-log volume; `s_global_min` is a separate atomic for the `klog_set_level(NULL, ...)` default.
> **Verified:** 2026-06-21 | ship `3bc86ce1` + review fixes | 6/6 items | build OK | smoke PASS (TCG 2.69s); 3212 kernel + 16 user PASS
> **Accepted:** [M] override table stores raw `const char*` tag pointers (no copy) -- a caller passing a non-static tag could dangle (reason: all current callers pass string literals) -> XREF: 02-kernel-core/TODO-04-system-logging.md §9 (item: "Copy the verbosity override-table tag into a bounded `char[16]`" at line 159)
> **Quality reviewed:** 2026-06-21 | Codex 6x (adversarial, consistency, perf, re-adversarial x2) | 1H+3M fixed, 1M accepted | scope: kernel-code-quality

---

## 4. Log Rotation
Prevent log files growing unbounded on long-running or repeatedly booted systems.

- [x] Before each `klog_disk_flush()`: check `kernel_log_size` against threshold
- [x] If size exceeds threshold: `rotate_log_file()` shifts `.1` → `.2` → `.3`, renames current to `.1`, creates fresh empty file
- [x] Keep at most N rotated files; delete oldest (`.N`) when N is exceeded
- [x] Rotation applies to kernel.log; per-subsystem files share the same `rotate_log_file()` function
- [x] `load_rotation_config()` reads `MaxSize` (default: 4 MB) from `HKLM\SYSTEM\Logs\MaxSize` via `RegReadKeyValue()`
- [x] Reads `MaxRotated` (default: 3, max: 9) from `HKLM\SYSTEM\Logs\MaxRotated`
- [x] O(1) check: `kernel_log_size` tracked in static var, updated from `logfile->size` after each flush
- [x] Rotate the per-subsystem split logs: the subsystem loop reads each file's on-disk size at open and calls `rotate_log_file()` when it exceeds MaxSize, so a high-volume subsystem cannot grow unbounded (§2 review)
- [/] No-RTC serial-log recency rotation: a CMOS-less boot pins `pick_log_number()` to the `00/01/01` sentinel, so `Serial_*` seq cannot encode recency (cap deletes by seq). Needs a cross-boot counter; blocked (none at `klog_disk_enable`).
- [x] Commit: `"kernel: log rotation"`

**Test checkpoint:** with `MaxSize` small, a flush past the threshold renames `kernel.log` -> `.1` (shifting `.1`->`.2`...), drops the oldest, and starts a fresh empty `kernel.log`; the same applies to each per-subsystem split log and `events.jsonl`. A failed rename keeps the live log intact (never truncated) and does not churn the rotated generations on retry.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_klog suite, 0 failures
> **Notes:**
> - `rotate_log_file()` (two-phase staged) + `klog_build_log_path()` bounded path builder in `src/kernel/klog_disk.c`; applied to kernel.log, the 6 per-subsystem split logs, and events.jsonl.
> - Staging-first order: rename current -> `.tmp` BEFORE any destructive delete/shift, then shift `.N-1`->`.N`, archive `.tmp`->`.1`, fresh current. Per-subsystem files read their on-disk size at open (no cached state to go stale across reboots).
> - A failed stage rename returns the real size and leaves the live log AND every rotated generation untouched -- no truncate, no per-retry generation churn. `MaxSize`/`MaxRotated` from registry with `val_size`-validated REG_DWORD reads.
> - Rotation unit test deferred (kernel image is at its BSS page budget); covered by 3-round adversarial review + the boot smoke test for now.
> **Verified:** 2026-06-21 | ship `4879f6a3` + review fixes | 9/10 items | build OK | smoke PASS (TCG 2.58s); 3212 kernel + 16 user PASS
> **Deferred:** [L] no dedicated rotation unit test (path builder is static + at BSS budget; rotate_log_file does real VFS I/O) -> XREF: 02-kernel-core/TODO-04-system-logging.md Unit Tests (item: "Rotation tests (§4): assert `klog_build_log_path` gen 0/.N/.tmp + cap-overflow" at line 409)
> **Deferred:** [M] no-RTC serial-log rotation deletes by seq, not recency (filed 2026-06-27 from TODO-08 §5 review; cap bounds growth so not a leak) -> XREF: 02-kernel-core/TODO-04-system-logging.md §4 (item: "No-RTC serial-log recency rotation" at line 170)
> **Quality reviewed:** 2026-06-21 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) + auditor | 1H+3M+1L fixed | scope: kernel-code-quality

---

## 5. Rate Limiting
Prevent a misbehaving subsystem from flooding the log and starving disk I/O.

> [!NOTE]
> **Superseded by TODO-32 §5** (configurable per-subsystem rate limits + diagnostic-tag exemption). The v1 design here treats every subsystem identically with a single global table and a recursive `klog()` from the rate-limit summary path -- both are real freeze risks documented in TODO-32's Goal. v1 is preserved unchanged until TODO-32 §10 retires it; this section's `[x]` items remain a correct record of the v1 implementation.

- [x] Track per-subsystem message count within a 100-tick sliding window (100 Hz = 1 second); 32-slot table
- [x] When a subsystem exceeds the rate: drop entries; the dropped count surfaces via the `dropped` field in `events.jsonl` (§6), not a recursive `klog()` line (that emit was removed as a freeze risk -- TODO-32 §5)
- [x] Reset the counter at window expiry (1000 ms monotonic window from `uptime_ns()/1e6`, checked on each `klog()` call so a wall-clock change cannot shrink/extend the window)
- [x] Rate limit thresholds configurable per subsystem via Registry: `HKLM\SYSTEM\Logs\RateLimit\<tag>` (REG_DWORD)
- [x] Dropped count tracked in `klog_rate_slot_t.dropped` for future `events.jsonl` integration (§6)
- [x] Commit: `"kernel: log rate limiting"`

**Test checkpoint:** a subsystem exceeding its per-second budget has further entries dropped (not stored in the ring); `klog_get_dropped(tag)` returns the count and it appears as the `dropped` field in `events.jsonl`. `klog_get_dropped` for an unknown tag returns 0. Registry config (`HKLM\SYSTEM\Logs\Levels` + `RateLimit`) is applied at boot.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_klog rate-limit-api suite, 0 failures
> **Notes:**
> - v1 per-subsystem rate limiter in `src/kernel/klog.c`: 32-slot table, 1000 ms monotonic window (`uptime_ns()`), default 100 msgs/window; `rate_check()` runs under `s_klog_lock` on the `klog()` path.
> - `klog_load_levels_from_registry()` is now actually called at boot (`boot_storage.c`, after `registry_populate_defaults()`) -- it was dead before, so the per-subsystem verbosity (§3) AND rate-limit registry config never loaded.
> - `dropped` count surfaces via the `events.jsonl` field (§6); `klog_get_dropped` is lock-free (rate_slot release-publishes the slot count, reader acquire-loads) so it adds no lock on the per-entry JSON flush path.
> - v1 is intentionally superseded by TODO-32 §5 (per-CPU lockless redesign): the global-table fail-open on exhaustion and the per-window drop-summary are owned there, preserved unchanged here.
> **Verified:** 2026-06-21 | ship `fcd203c2` + review fixes | 6/6 items | build OK | smoke PASS (TCG 2.57s); 3212 kernel + 16 user PASS
> **Accepted:** [H] 32-slot rate table fails open on exhaustion (caller-controlled tags can disable limiting) -> XREF: 02-kernel-core/TODO-32-kernel-logging-v2-lockless.md §5 (item: "`klog_rate_v2_t` per CPU ... `s_rate[KLOG_RATE_V2_SLOTS=64]` per CPU" at line 133)
> **Accepted:** [M] window reset clears `dropped` without a per-window summary, so cross-window drop evidence is lost -> XREF: 02-kernel-core/TODO-32-kernel-logging-v2-lockless.md §5 (item: "Drop summary: when rate limit drops a message, increment `tag_drops[tag_id]`" at line 138)
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 2H+2M fixed, 1H+1M accepted | scope: kernel-code-quality

---

## 6. Structured JSON Log Events
Emit machine-parseable events alongside plain-text logs. Implemented with manual JSON string formatting in `klog_disk.c` -- no cJSON dependency.

- [x] Define JSON event format: `{"ts":<ms>,"lvl":"WARN","sub":"net","msg":"DHCP timeout","dropped":0}`
- [x] JSON Lines flush integrated into `klog_disk_flush()` -- serializes ring buffer entries to `X:\Logs\events.jsonl`
- [x] JSON Lines format -- one JSON object per line; append-only; batched into a 16 KB buffer that chunk-flushes at the boundary, and `jsonl_flush_seq` advances only after every line in the window is written (kernel.log durability)
- [x] `events.jsonl` created at first flush; file size tracked for rotation
- [x] §4 log rotation applied to `events.jsonl` via `rotate_log_file()`
- [x] `"dropped"` field included from §5 rate limiter via `klog_get_dropped()` public API
- [/] Event viewer reads `events.jsonl` for colour-coded filtering -- owned by the eventview.exe tool -- → XREF: [13-tools-accessories/TODO-02](../13-tools-accessories/TODO-02-event-viewer.md)
- [/] When TODO-27 §7 first writes DUMP_PARTITION_HEADER: add JSON keys dump_encryption_state and dump_present_on_raw (blocked on the dump sink existing) -- → XREF: D02 TODO-27-crash-dump-generation §7
- [x] Commit: `"kernel: structured JSON log events"`

**Test checkpoint:** each ring entry serializes to one JSON object per line in `events.jsonl` (`ts`/`lvl`/`sub`/`cpu`/`pid`/`tid`/`msg`/`dropped`); `"` `\` and control chars are escaped so the output is valid JSONL. A window larger than the 16 KB batch chunk-flushes without dropping entries, and `jsonl_flush_seq` advances only after every line is durably written (no drop, duplicate, or merged line across a write/truncate failure).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_klog suite, 0 failures
> **Notes:**
> - Structured JSON Lines event log in `src/kernel/klog_disk.c`: 8-field objects, manual formatting + dual JSON-escaping (subsystem + message), serialized from the ring in `klog_disk_flush_locked`.
> - Chunk-flushed into a 16 KB batch and rotated via `rotate_log_file` (§4); the `dropped` field comes from the §5 rate limiter via lock-free `klog_get_dropped`; timestamp is 64-bit ms (`uptime ticks x 10`, no 5-day wrap).
> - Durability matches kernel.log: a committed-seq cursor + partial-commit record salvage + a resync-newline on degraded media guarantee no dropped, duplicated, or merged JSONL record across `vfs_write`/`vfs_truncate` failures.
> - Scope boundary: the `eventview.exe` viewer and the crash-dump JSON keys are owned by other TODOs (see Accepted XREFs).
> **Verified:** 2026-06-21 | ship `8956eb92` + review fixes | 6/8 items | build OK | smoke PASS (TCG 2.63s); 3212 kernel + 16 user PASS
> **Accepted:** [L] no in-OS viewer for `events.jsonl` (raw file / serial only) -> XREF: 13-tools-accessories/TODO-02-event-viewer.md (item: "Open and parse `X:\Logs\events.jsonl` line by line" at line 57)
> **Accepted:** [L] dump-partition JSON keys (`dump_encryption_state`, `dump_present_on_raw`) not emitted -- blocked on the dump sink existing -> XREF: 02-kernel-core/TODO-27-crash-dump-generation.md §7 (item: "`DUMP_PARTITION_HEADER` at physical sector 0 of the dump partition" at line 245)
> **Quality reviewed:** 2026-06-21 | Codex 9x (adversarial, consistency, perf, re-adversarial x6) + auditor | 5H+3M+3L fixed | scope: kernel-code-quality

---

## 7. ETW Tracing Syscalls Wired to SSDT
Event Tracing for Windows (ETW) provides high-performance kernel/user tracing. This section wires the tracing control syscalls into the SSDT. (→ XREF: TODO-12-native-api-ssdt.md §5, §22)

- [x] `NtTraceEvent(TraceHandle, Flags, FieldSize, Fields)` → SSDT 0x01D0: write a trace event to a session -- `etw.c` NtTraceEvent()
- [x] `NtTraceControl(FunctionCode, InBuffer, InLen, OutBuffer, OutLen, RetLen)` → SSDT 0x01D1: control trace sessions (start/stop/query/update/flush) -- `etw.c` NtTraceControl()
- [x] `NtCreateTrace(TraceHandle, DesiredAccess, ObjectAttributes, TraceGuid)` → SSDT 0x01D2: create a new trace session -- `etw.c` NtCreateTrace()
- [x] `NtQueryTrace(TraceHandle, TraceInformationClass, Buffer, Length)` → SSDT 0x01D3 -- `etw.c` NtQueryTrace()
- [x] `NtUpdateTrace(TraceHandle, InstanceName, Properties)` → SSDT 0x01D4 -- `etw.c` NtUpdateTrace()
- [x] `NtStopTrace(TraceHandle, InstanceName, Properties)` → SSDT 0x01D5 -- `etw.c` NtStopTrace()
- [x] `NtFlushTrace(TraceHandle, InstanceName, Properties)` → SSDT 0x01D6 -- `etw.c` NtFlushTrace()
- [x] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-12 §1)
- [x] Commit: `"kernel: klog -- wire ETW tracing syscalls to SSDT (0x01D0-0x01D6)"`

**Test checkpoint:** `NtCreateTrace` returns a valid session handle; `NtTraceEvent` writes an event visible via `NtTraceControl`/`NtQueryTrace`; `NtStopTrace` + `NtFlushTrace` drain/release the buffer. User-reachable handlers validate every user pointer and never deref one under `s_etw_lock`. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_klog ETW suite, 0 failures
> **Notes:**
> - 7 ETW trace-control syscalls wired to SSDT 0x01D0-0x01D6 in `src/kernel/etw.c`; 8 sessions, 4 KB circular buffers, SMP-safe via `s_etw_lock`, registered in `boot_desktop.c` Phase 3.
> - User-reachable handlers validate every user pointer via `etw_copy_in`/`etw_copy_out` (probe + copy_from/to_user): inputs copied before the lock, outputs built in locals + copied after -- no user deref under the spinlock.
> - Review hardened 3 Critical + 1 High raw-user-pointer derefs (kernel read/write primitives) into probe+copy; `NtCreateTrace` tears the session down + propagates the probe status on a copy-out fault.
> - Kernel-internal `etw_emit_kernel_event` unaffected (probes no-op for kernel mode); the test build reclaimed 16 KB BSS (audit-line fixtures 16->12 KiB) to stay under the user-base page budget.
> **Verified:** 2026-06-21 | ship `de5fd1f9` + review fixes | 8/8 items | build OK | smoke PASS (TCG 3.12s); 6800 kernel + 16 user PASS
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 3 Critical+1H+1M fixed | scope: kernel-code-quality

---

## 8. Crash-Persistent Log Capture

Reserve a physical memory region at boot so the ring buffer survives a kernel panic. On next boot, recover entries and write them to `X:\Crash\crash_recovery.log`. Linux has pstore/ramoops; Windows writes minidumps with log context. Without this, any crash before `klog_disk_flush()` loses the most recent ring buffer entries -- the entries most likely to contain the crash cause.

> [!WARNING]
> **High-risk:** Modifies `panic_screen()` (boot-critical path) and `klog_early_init()` (Phase 0). If this breaks, revert the `klog_crash_persist()` call in `panic_screen()` -- the BSOD still renders without crash log persistence. Verify `panic_screen()` still works after adding the call.

- [x] Reserve 128 KiB physical region via `pmm_alloc_contiguous(32)` in `klog_crash_recover()` for crash log persistence; physical address stored in UEFI NVRAM `ImpossibleCrashLog` variable so next boot can find it
- [x] Define `klog_crash_header_t`: magic `0x4B4C4F47` ("KLOG"), entry_count, crc32, ring_head, boot_timestamp
- [x] `klog_crash_persist()`: serializes ring entries (no pointers -- subsystem copied as char[16]) + CRC32 to reserved region; called from `panic_screen()` after crash dump write
- [x] No `kmalloc` or VFS in crash persist path -- direct physical memory write only
- [x] `klog_crash_recover()`: reads region address from NVRAM, validates magic + CRC32, replays to serial with `[CRASH-PREV]` prefix; called in Phase 0 after UEFI runtime init
- [x] `klog_crash_write_to_disk()`: writes recovered entries to `X:\Crash\crash_recovery.log`; called from `klog_disk_enable()` in Phase 2
- [x] Clears magic cookie after successful recovery; zeros fresh region for new boot
- [x] POST codes: `POST16(0xDE00)` entry, `POST16(0xDE01)` region allocated, `POST16(0xDE02)` recovery check, `POST16(0xDE03)` done
- [x] 3 unit tests: KLOG_CRASH_MAGIC value, header size bounds, POST code uniqueness
- [/] Verified-persistence flag gating `POST16_KLOG_DISK_OK` on a real `kernel.log` write (not just buffer+mount) -- deferred: needs a verification-flush flow in `klog_disk_enable()`, a separable boot diagnostic (§1-review gap)
- [x] Commit: `"kernel: crash-persistent klog capture via reserved physical memory"`

> [!NOTE]
> Complementary to TODO-27 crash dumps: TODO-27 captures binary CPU/memory state for WinDbg. §8 captures the text log ring buffer -- the human-readable context most useful for first-pass triage. Both write to physical memory without VFS dependency.

**Test checkpoint:** Force a panic with `test=1 crash_test=1` in boot.conf; on next boot, serial shows `[CRASH-PREV]` entries from the previous crash. `crash_recovery.log` contains recovered entries with timestamps. CRC32 mismatch on corrupted region produces `[CRASH-PREV] recovery failed: CRC32 mismatch` on serial. Verify on QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_klog crash suite, 0 failures
> **Notes:**
> - Crash-persistent ring capture in `src/kernel/klog.c`: `klog_crash_persist` (panic path, no kmalloc/VFS, phys write + CRC32), `klog_crash_recover` (Phase 0 NVRAM+magic+CRC32 validate + replay), `klog_crash_write_to_disk` (Phase 2 -> `X:\Crash`).
> - 128 KiB region from `pmm_alloc_contiguous`, addr in UEFI NVRAM `ImpossibleCrashLog`; 164-byte fixed entries (`_Static_assert`-pinned); recover bounds entry_count by region capacity + checks the phys addr in `[USER_ELF_END, 4 GiB)` before deref.
> - Complementary to TODO-27 binary crash dumps; this captures the human-readable text ring for first-pass triage.
> - Scope: `POST16_KLOG_DISK_OK` real-persistence gating deferred ([/] above) -- needs a verification-flush flow, not crash-persist correctness.
> **Verified:** 2026-06-21 | ship `62f0a171` + review fixes | 9/10 items | build OK | smoke PASS (TCG 2.69s); 6800 kernel + 16 user PASS
> **Deferred:** [M] `POST16_KLOG_DISK_OK` is gated on buffer+mount, not a verified `kernel.log` write -> XREF: 02-kernel-core/TODO-04-system-logging.md §8 (item: "Verified-persistence flag gating `POST16_KLOG_DISK_OK`" at line 284)
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 2H+2M fixed, 1M deferred | scope: kernel-code-quality

---

## 9. Per-Entry Context Metadata

Add CPU number, process ID, and thread ID to every klog entry. Windows ETW includes `ProcessId`, `ThreadId`, `ProcessorNumber` on every event. Linux journald stores `_PID`, `_UID`, `_COMM`, `_SYSTEMD_UNIT`. Impossible OS's `klog_entry_t` currently has only `level`, `subsystem`, `timestamp`, `message` -- no process/CPU context. This metadata is essential for diagnosing SMP races and multi-process issues.

> [!WARNING]
> **High-risk:** Changes `klog_entry_t` struct layout -- affects `klog.c`, `klog_disk.c`, `test_klog.c`, and any code reading the ring buffer. If this breaks, revert the struct change and the `klog()` population code. Test incrementally: extend struct first, verify build, then add population logic.

- [x] Extend `klog_entry_t` in `klog.h`: add `uint8_t cpu_id`, `uint32_t pid`, `uint32_t tid` -- with 3-byte alignment padding
- [x] In `klog()`: populate `cpu_id` from `smp_this_cpu()->cpu_id` (NULL-safe, defaults to 0 before `smp_early_bsp_init()`)
- [x] In `klog()`: populate `pid` from `task_current()->pid` and `tid` from `thread_current()->id`, gated on `kernel_subsystem_ready(SUBSYS_SCHED)`; defaults to 0/0 during boot (global-cursor best-effort -- per-CPU accuracy in TODO-32 §2)
- [x] Update JSON Lines serialization in `klog_disk.c`: add `"cpu":N,"pid":N,"tid":N` fields to each JSON object in `events.jsonl`
- [x] Update serial output format: append `[cpu:N]` after the timestamp when SMP is ready or cpu_id > 0 (skipped during single-CPU early boot)
- [x] Update `klog_crash_persist()` (§8): `klog_crash_entry_t` extended with cpu_id/pid/tid fields (164 bytes, `_Static_assert`-pinned); serialization copies new fields
- [x] Add debug POST codes: `POST16_KLOG_CTX` (0xDE10), `POST16_KLOG_CTX_STRUCT` (0xDE11), `POST16_KLOG_CTX_SERIAL` (0xDE12), `POST16_KLOG_CTX_JSON` (0xDE13) -- range 0xDE1x confirmed free
- [/] Bound-copy the `klog_entry_t` subsystem tag instead of the raw caller pointer (`klog.c` `e->subsystem`) -- superseded by TODO-32 §2 tag interning; no live bug (all v1 callers pass literals) (§2 review)
- [/] Bound-copy the verbosity override-table tag instead of the raw `const char*` (`klog.c` `s_overrides[].tag`) -- superseded by TODO-32 §2 tag interning; v1 callers pass literals (§3 review)
- [x] Commit: `"kernel: add CPU/PID/TID context to klog entries"`

**Test checkpoint:** Serial log shows `[cpu:0]` on BSP entries after SMP init. JSON in `events.jsonl` contains `"cpu":0,"pid":1,"tid":N` for entries logged after scheduler start. Entries logged before scheduler show `"pid":0,"tid":0`. On SMP boot, AP entries show `"cpu":1` (or higher). Verify on QEMU WHPX (SMP), QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_klog context suite, 0 failures
> **Notes:**
> - `klog()` stamps each ring entry with `cpu_id` (`smp_this_cpu()`), `pid` (`task_current()`), `tid` (`thread_current()->id`), gated on `SUBSYS_SCHED`; serial shows `[cpu:N]`, `events.jsonl` + the crash entry carry cpu/pid/tid.
> - All captures are best-effort reads of the scheduler's GLOBAL `(current_task, current_thread)` cursor under `s_klog_lock`: bounded (never OOB), per-CPU-accurate only on the scheduling context.
> - Review fix: `tid` was hardcoded 0; now the real `thread_current()->id`.
> - Scope: per-CPU-accurate attribution + raw-tag-pointer storage are both reorganized by TODO-32 v2 (Accepted XREFs), not v1 struct changes.
> **Verified:** 2026-06-21 | ship `98460514` + review fixes | 7/9 items | build OK | smoke PASS (TCG 2.77s); 6800 kernel + 16 user PASS
> **Accepted:** [M] pid/tid reflect the global scheduler cursor, not per-CPU-accurate attribution -> XREF: 02-kernel-core/TODO-32-kernel-logging-v2-lockless.md §2 (item: "Implement `void klog_v2(...)` ... fills pid/tid from current task" at line 88)
> **Accepted:** [M] `klog_entry_t`/override-table tags store raw caller pointers (no copy) -> XREF: 02-kernel-core/TODO-32-kernel-logging-v2-lockless.md §2 (item: "Tag interning: `tag_id` is a 16-bit index into a static `s_tag_table[256]`" at line 91)
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 1M+1L fixed, 2M accepted-XREF | scope: kernel-code-quality

---

## 10. Log Integrity Verification

HMAC-chain `events.jsonl` entries so tampering is mathematically detectable. Linux journald has optional Forward Secure Sealing (FSS) but it's rarely enabled and uses a binary format unreadable by standard tools. Windows Event Log has no built-in cryptographic integrity. Impossible OS can be the first OS with kernel-level tamper-evident structured logging in a human-readable format.

> [!TIP]
> Neither Win11 nor Linux provides out-of-the-box tamper-evident text-format logging. Win11 Event Log is binary XML with no cryptographic sealing. Linux journald's FSS is optional, complex to set up, and uses a binary journal format. Impossible OS's HMAC-chained JSON Lines is human-readable AND cryptographically verifiable -- readable by `jq`, verifiable by `klog_verify_chain()`.

> [!IMPORTANT]
> **Unblocked (2026-06-20):** Requires `crypto_blake2b_keyed()` (keyed MAC, like `seed_file.c`) and `csprng_fill()` + `csprng_crypto_ok()` (key generation) from TODO-03 §5 (Monocypher + kernel CSPRNG), now shipped (`src/libs/monocypher/` + `src/kernel/csprng.c`). CRC32 in `gpt.c`/`ixfs_core.c` is NOT cryptographically secure and must not be used for integrity sealing -- use the keyed `crypto_blake2b_keyed` MAC.

> [!WARNING]
> **Design review 2026-06-21 -- operator-reserved before implementing.** The naive plan (store the HMAC key in `HKLM\SYSTEM\Logs\IntegrityKey`) is a FALSE integrity guarantee: an attacker who can rewrite `events.jsonl` can also rewrite that registry value and recompute every `hmac`. The verifier key MUST be anchored OUTSIDE the mutable log trust domain (TPM NVRAM sealed to PCRs, or a protected UEFI NVRAM variable) -- this threat-model/key-anchoring choice is an OPERATOR-RESERVED security-architecture decision. Three more design constraints the implementer must honor: (1) `klog_disk_enable()` runs at `boot_storage.c:691` BEFORE `registry_init()` at line 800, so split key MINT (Phase 2, CSPRNG) from key PUBLISH (after registry/TPM is up) and hard-WARN/disable on publish failure; (2) the HMAC chain tail MUST follow the EXACT three-way state machine of the §6 `events.jsonl` flush -- per-line tails, a committed tail advanced only when `jcommitted` finalizes, AND a salvage tail for complete records that landed when `vfs_truncate` failed -- or a chunk-rollback replay forks the chain and breaks verification; (3) rotation (`events.jsonl`->`.1`) and per-boot key epochs are undefined -- define an authenticated per-file epoch header (boot id, generation, key id, previous-file tail) so a reset point cannot mask truncation/splicing.

- [/] Define `klog_integrity_ctx_t`: previous HMAC (32B), session key (32B), key-id/epoch, init flag (deferred -- see Design-review WARNING)
- [/] At `klog_disk_enable()`: MINT the session key via `csprng_fill()` gated on `csprng_crypto_ok()` (skip+WARN on degraded entropy); split MINT from PUBLISH (→ XREF: TODO-03 §5 CSPRNG)
- [/] In JSON Lines flush: append `"hmac":"<64hex>"` = `crypto_blake2b_keyed(key, prev_hmac || json_body)`; chain tail MUST mirror the §6 commit/rollback/salvage state machine; bump `line[512]` to >=640
- [/] Anchor the verifier key OUTSIDE the mutable log domain (TPM/protected UEFI NVRAM, NOT `HKLM`); publish after the registry/TPM subsystem is up; WARN+disable on failure (operator-reserved)
- [/] Implement `klog_verify_chain(const char *jsonl_path)`: recompute each HMAC against the chain (constant-time `crypto_verify32`); per-file epoch header so a rotation reset cannot mask truncation
- [ ] Wire into `dmpanalyze.exe` (-> XREF: TODO-27 §9): `dmpanalyze /verifylog` verifies the chain -- BLOCKED: `dmpanalyze.exe` (TODO-27 §9) not implemented
- [/] Gate behind `boot.conf` `log_integrity=1` (typed `boot_arg_desc_t` row in `config.c`; default enabled, disablable for perf debug)
- [/] Add Phase-2 key-mint POST codes only (`POST16(0xDE20)` entry, `POST16(0xDE21)` key minted; range `0xDE2x` free); runtime seal/verify are post-Phase-3 (no POST16)
- [ ] Consumer: Code Integrity allow/deny/audit decisions emit INTO this HMAC chain for tamper-evident CI forensics. -> XREF: D02 T19 §10 (CI audit events).
- [ ] Commit: `"kernel: HMAC-chain integrity verification for events.jsonl"`

**Test checkpoint:** Boot with `debug=1`; `events.jsonl` entries contain `"hmac":"..."` field (64 hex chars). `klog_verify_chain("X:\\Logs\\events.jsonl")` returns 0 (valid chain). Manually corrupt one JSON line; `klog_verify_chain()` returns the corrupted line number. Boot with `log_integrity=0`; `events.jsonl` entries have no `"hmac"` field. Verify on QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [Critical] HMAC verifier-key anchoring is an operator-reserved security-architecture decision -- the Codex design review (2026-06-21) found the planned `HKLM` key storage gives a FALSE integrity guarantee (attacker rewrites log + key + all hmacs); the whole section is blocked on the threat-model/key-anchor choice (TPM vs UEFI NVRAM) plus the §6-mirroring chain state machine and rotation epochs -> XREF: 02-kernel-core/TODO-04-system-logging.md §10 (item: "Anchor the verifier key OUTSIDE the mutable log domain" at line 353)
> **Deferred:** [M] `dmpanalyze /verifylog` wiring blocked on the analyzer existing -> XREF: 02-kernel-core/TODO-27-crash-dump-generation.md §9 (item: "`src/apps/dmpanalyze/dmpanalyze.c` -- standalone command-line app" at line 294)

---

## 11. ETW Provider Registration and Session Filtering

§7 wires the 7 `NtTrace*` session syscalls but ETW is not yet usable by real providers: there is no provider registry (GUID to name to channel), and no per-session filtering, so every `NtTraceEvent` writes to every running session. Windows registers providers via `EtwRegister` + manifest and applies a 64-bit keyword bitmask + level kernel-side before the buffer write. This section adds the missing provider/consumer bridge. (→ XREF: TODO-12 §5 SSDT ETW range; §7 of this TODO.)

> [!WARNING]
> **Design review 2026-06-21 -- corrected design before implementing.** The original plan would silently misroute events. Four constraints the implementer MUST honor: (1) NtTraceEvent only carries `(trace_handle, flags, field_size, fields)` and the 16-byte `etw_event_header_t` has no provider GUID or keyword -- §11 needs a VERSIONED event-metadata ABI (provider id/GUID + uint64 keyword + event_id + level), copied via the §7 `etw_copy_in` probe pattern, before any filter can work. (2) The §7 write-to-one-session-by-handle model conflicts with ETW provider-to-all-enabled-sessions semantics -- adopt the BROADCAST model (provider identity is the routing key; iterate sessions, write only matching ones), keeping any legacy explicit-session write as a separate compat path. (3) Scalar `match_any/match_all/max_level` on `etw_session_t` CANNOT represent two providers enabled into one session with different policies -- use a bounded per-session provider-enable TABLE keyed by provider id `{provider, match_any, match_all, max_level, enabled}`. (4) Registration MUST be two-phase: copy/validate user input + update the in-memory provider table under `s_etw_lock`, DROP the lock, THEN persist to registry (never call registry/VFS/heap under the irqsave `s_etw_lock`). This is a security-sensitive ETW ABI redesign extending the syscall surface §7 just hardened -- it warrants a focused implementation pass.

- [/] Define `etw_provider_t` in `etw.h` (GUID, name[64], channel id, registering pid) + a global provider registry table guarded by `s_etw_lock` (deferred -- see Design-review WARNING)
- [/] Define a VERSIONED event-metadata wire ABI (provider id/GUID + uint64 keyword + event_id + level) copied via `etw_copy_in`; extend the stored event record prefix to carry provider+keyword (NtTraceEvent cannot filter without it)
- [/] Provider registration path: register GUID->name->channel; resolve GUID->name; enumerate via `NtQueryTrace`; two-phase (in-memory under `s_etw_lock`, then persist OUTSIDE the lock)
- [/] Per-session provider-enable TABLE keyed by provider id `{provider, match_any, match_all (64-bit), max_level, enabled}` -- scalar per-session masks cannot hold two providers with different policies
- [/] BROADCAST filter: a provider event routes to ALL sessions that enabled it; deliver to a session only when keyword match_any/match_all both satisfied AND `event_level <= max_level` (keep legacy write-to-handle as a compat path)
- [/] Persist provider manifest under Registry `HKLM\SYSTEM\Logs\ETW\Providers\<guid>` AFTER dropping `s_etw_lock`; never call registry/VFS/heap under the irqsave lock
- [/] `klog(LOG_INFO, "etw", ...)` trace on provider register/unregister (observable; ETW is post-boot, no POST16)
- [/] Unit tests in `test_klog.c`: provider register + enumerate roundtrip; two sessions enabling one provider; one session enabling two providers with different masks; non-matching keyword dropped; bad/non-NUL-terminated name rejected
- [ ] Commit: `"kernel: ETW provider registration + per-session keyword/level filtering"`

**Test checkpoint:** Register a provider GUID, enumerate it by name via `NtQueryTrace`. Enable it into a session with a keyword mask; `NtTraceEvent` with a non-matching keyword does not appear in that session's buffer; a matching event does. Verify on QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [H] §11 needs an ETW ABI redesign before code -- the Codex design review (2026-06-21) found the plan misroutes events (no event-metadata wire ABI for provider/keyword, single-session-vs-broadcast conflict, scalar masks cannot hold multiple providers, registry-under-irqsave-lock); the corrected design (versioned metadata + broadcast routing + per-session provider table + two-phase registration) extends the §7-hardened syscall surface and warrants a focused pass -> XREF: 02-kernel-core/TODO-04-system-logging.md §11 (item: "Define a VERSIONED event-metadata wire ABI" at line 375)

---

## 12. ETW Advanced Capture -- Stack-Walk, Autologger, Self-Describing Schema

Deepen ETW to match Win11's diagnostic surface: per-event call-stack capture (used by profilers and security root-cause), a boot-persistent autologger session that captures init events before user-mode, and self-describing (TraceLogging-style) event schemas so consumers decode payloads without out-of-band manifests.

- [/] Stack-walk: per-provider stack-trace flag; capture frames via `RtlCaptureStackBackTrace` (→ XREF: TODO-23), symbolize via the symbol provider (→ XREF: TODO-18 §4), append to the event record (blocked on §11 provider model)
- [/] Autologger: registry-configured boot-persistent session (`HKLM\...\WMI\Autologger`) started Phase 2 before first user-mode, capturing init/driver-load events (gate behind config; boot must survive absent/corrupt config)
- [/] Self-describing schema: provider declares a field name/type array at registration; events carry a schema id so consumers decode payload fields without an external manifest (built on §11 registration)
- [/] `klog(LOG_INFO, "etw", ...)` trace on autologger start + stack-capture enable (observable)
- [/] Unit tests: stack-walk produces >= 1 resolvable frame; autologger config parse + session start; schema register + field decode roundtrip
- [ ] Commit: `"kernel: ETW stack-walk, autologger, and self-describing event schema"`

**Test checkpoint:** Enable stack capture on a provider; a logged event carries a non-empty frame array with at least one image+offset resolved. A registry-declared autologger session is active at first user-mode entry. A self-describing event decodes its field names without an external manifest. Verify on QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Deferred:** [H] blocked on the §11 ETW provider model (stack-walk flag, autologger session, and self-describing schema all build on provider registration + per-session enablement) -> XREF: 02-kernel-core/TODO-04-system-logging.md §11 (item: "Define `etw_provider_t`" at line 374)

---

## 13. Rotated-Log Compression

The OS Comparison row "Rotated log compress" is listed as planned (LZ4) but no section owns the wiring. Linux logrotate gzip-compresses rotated logs and journald LZ4-compresses its journal; Win11 does not compress rotated logs. Impossible OS has none yet. Compress rotated `.N` files so long-running systems keep more history in the same disk budget. (→ XREF: TODO-03 §3 LZ4 block compression primitive.)

- [x] `klog_compress_archive()` (klog_disk.c): LZ4-compresses the staged log behind a 20-byte `klog_lz4_hdr_t`, atomic `.1.lz4.tmp`->`.1.lz4`; any failure falls back to plain `.1`. Pure half factored as `klog_compress_buffer()` (→ XREF: TODO-03 §3)
- [x] LZ4 in/out buffers via `pmm_alloc_contiguous()` (TODO-03 memory rule, sized from `lz4_compress_bound`); staged size validated `>0 && <= LZ4_BLOCK_INPUT_MAX` before the uint32 narrow (no silent truncation)
- [x] Gate via Registry `HKLM\SYSTEM\Logs\Compress` REG_DWORD (default 1; Registry-driven like MaxSize/MaxRotated). `rotate_log_file()` shifts BOTH families (.N and .N.lz4) so a Compress toggle never strands a generation
- [x] `klog_decompress_rotated()` for the in-OS viewer + host extractor: validates magic/version/bounds/CRC32 and bounds the decode by the recorded `uncompressed_size` so a hostile block cannot over-expand (→ XREF: 14-host-tools/TODO-08)
- [x] Crash-safe recovery: `rotate_log_file()` Phase 0 recovers an orphaned `.tmp` -- idempotent shift + `klog_orphan_already_archived()` byte-compare drops a cross-reset duplicate; never loses or duplicates a generation
- [x] `klog(LOG_WARN, "klog", ...)` traces on rotation/recovery failure paths (observable)
- [x] Unit test `test_klog_lz4_roundtrip` (TEST_CAT_BOOT): roundtrip + negatives (bad magic, CRC mismatch, truncation, undersized dst, over-expanding valid-CRC block leaves caller bytes unclobbered)
- [x] Commit: `"kernel: LZ4 compression for rotated log files"`

> **Test checkpoint:** boot suite runs `test_klog_lz4_roundtrip`; on a Compress=1 system, rotated logs appear as `X:\Logs\*.1.lz4` and `klog_decompress_rotated()` restores them byte-identical. A Compress toggle between boots keeps both `.N` and `.N.lz4` generations in the chain.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1 suite added, 0 failures
> **Notes:**
> - LZ4 rotated-log compression: `klog_compress_archive()` writes a self-describing `[klog_lz4_hdr_t | LZ4 block]` `.N.lz4` via atomic `.tmp`->rename; `klog_decompress_rotated()` validates magic/version/CRC32 and caps the decode at the recorded size.
> - Registry `Compress` gates it (default 1, like MaxSize/MaxRotated); `rotate_log_file()` shifts both `.N` and `.N.lz4` families so a Compress toggle never strands a generation.
> - No-data-loss rotation: stage current->`.tmp`, shift only when slot 1 is occupied; Phase 0 + `klog_orphan_already_archived()` recover an orphaned `.tmp` across all interruption windows without loss or duplication.
> - Codex: design + adversarial + 3 re-adversarial rounds (1-2 fixed High over-expansion/truncation/double-shift; round 3 rejected a byte-identity medium as lossless). Detail in the commit message.
> - Buffers: `pmm_alloc_contiguous()` per the TODO-03 LZ4 rule; staged size validated `<= LZ4_BLOCK_INPUT_MAX` before narrowing. Host extractor consumes the same format (→ 14-host-tools/TODO-08).
> **Verified:** 2026-06-21 | ship `d9df4bd2` + review fixes | 7/7 items | build OK | smoke PASS (TCG 2.64s); 3224 kernel + 16 user PASS
> **Accepted:** [M] `.N.lz4` header is native-endian (correct on x86-64; a big-endian host extractor would misread fields) -> XREF: 14-host-tools/TODO-08 §4 (item: "Decompress `.N.lz4` rotated logs: parse `klog_lz4_hdr_t` by explicit little-endian offsets")
> **Quality reviewed:** 2026-06-21 | Codex 4x (adversarial, consistency, perf, re-adversarial) + auditor | 1H+2M+1L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 14. Serial Timestamp Render Bound

Discovered 2026-07-29 during a `00-infrastructure/TODO-04-usermode-test-framework.md` §29 re-adversarial review (a wire-cost derivation elsewhere needed to bound the timestamp field's worst-case byte count, which surfaced this). The serial-line renderer's timestamp digit loop (`src/kernel/klog.c` around the function documented at `klog.c:1240-1341`) writes decimal digits of `uint32_t sec` into a fixed local buffer with no bound on digit count: `char tmp[8]; ... while (v > 0) { tmp[n++] = ...; v /= 10; }`. `sec` is a `uint32_t` (max value 4,294,967,295, 10 decimal digits), but `tmp` holds only 8 bytes -- a boot whose uptime reaches 100,000,000 seconds (~3.17 years of continuous uptime) writes past the end of `tmp` on the stack. No test, panic path, or bare-metal gate currently catches this because reaching it requires uptime far beyond any real boot-test cycle or realistic continuous-uptime deployment; it is a latent stack buffer overflow, not a live one.

- [ ] Bound the timestamp digit loop in klog.c's serial renderer so it cannot write past its fixed stack buffer regardless of uptime seconds
      Size `tmp` to the true `uint32_t` worst case (10 digits) or cap the loop at 8 iterations and saturate/wrap once `sec` exceeds what the buffer holds, with a `_Static_assert` pinning the bound against `sizeof(tmp)`. Add a unit test driving the renderer at 7/8/9/10-digit `sec` values confirming no write past the declared size (canary byte after `tmp`, or refactor the digit-count logic into a testable pure function). Consumer: `00-infrastructure/TODO-04-usermode-test-framework.md` §29's `UTEST_RECORD_WIRE_MAX`, which must widen `KLOG_WIRE_TIMESTAMP_MAX` again if the fix changes the max digit count the renderer can safely emit.

---

## 15. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 9:
- [ ] Rate-stable `klog_entry_t.timestamp`: capture `uptime_ns()/1e7` (10ms units) at `klog()` emit, not raw PIT ticks, so disk-log ISO reconstruction survives `NtSetTimerResolution` rate changes (today it assumes 100 Hz). (TODO-08 §13 review.)

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## 16. Klog Assertions and Scans That Depend on Nothing Else Having Logged

Two related defects in how this subsystem's own tests read the ring. First, `test_klog.c` decides whether an entry landed by comparing two ring HEAD positions -- `head_after != head_before` for a delivery, `head_after == head_before` for a drop. Head is a shared cursor that every accepted entry from any CPU advances, so a positive assertion passes on somebody else's line and a NEGATIVE assertion ("this was filtered, so head did not move") FAILS the moment an AP logs anything inside the window. Second, and underneath it, `klog_get_ring`/`klog_get_ring_snapshot` lock only the head/count/seq metadata and hand back the LIVE array: `src/kernel/klog.c:1065-1069` states plainly that entry contents race concurrent logging, and records the copying API as deliberately deferred because no production caller needed it. Every window scan in the test tree is a caller that does.

> [!NOTE]
> Filed 2026-08-02 from `00-infrastructure/TODO-04-usermode-test-framework.md` §55, which converted the usermode launcher's window scans to a monotonic-sequence bound with content matching and could only buy a MARGIN against the live-array race: it refuses a window of exactly `KLOG_RING_SIZE` because at that width the oldest entry sits on `head` and one concurrent append destroys it, but a window of N-1 still dies after two. That margin is documented as a margin at its definition. This section owns the cure. The "Ring flush reads live entries without lock" row already Accepted in this file's Codex review table is the same underlying issue seen from the flush side.

- [ ] Give klog a snapshot API that copies a selected window under the lock
      - The shape §55 needs is "copy the entries between two monotonic sequence values into caller storage while holding `s_klog_lock`", so a consumer scans an immutable copy rather than the live array. Bound the copy so a caller cannot ask for more than the ring holds.
      - Convert the existing live scans to it: `u_test_scan_window` / `u_test_first_match` in `src/kernel/test/test_usermode_launcher.c`, the marker scan in `test_klog_ring_write`, and `test_klog_receipt_null_ack_is_the_ordinary_path` in the same file (added by `00-infrastructure/TODO-04` §57, which copied the reference shape and inherited the same exposure -- its own review round re-derived this defect independently). Once they consume a stable copy, §55's exact-capacity refusal can be relaxed back to the true arithmetic bound.
      - The window can also be invalidated DURING a scan, not only between the snapshot and the scan: a copy under the lock closes both, whereas a width check at snapshot time closes neither.
      - The exposure is WIDTH-DEPENDENT, which is why a single refusal width is not a fix. A window of width W has its oldest entry at `head - W`, and appends land at `head` outward, so that entry survives exactly `KLOG_RING_SIZE - W + 1` appends: one at `W = N`, two at `W = N-1`, and on the order of a full ring only for the small windows the tests actually open. Section 55 refuses `W = N` for that reason and leaves every wider window still exposed.
      -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §55 (item: "Prove the fix at the boundary the current tests cannot reach: a saturated ring")
- [ ] Give the test suite a sink-suppressed way to saturate the ring
      - Saturating the ring is the only way to make the count-vacuity claim against the REAL counter, and doing it through `klog()` is unaffordable: measured from `00-infrastructure/TODO-04` §55, a full run reached its test 367 entries short, so the filler ran every time -- 33,397 serial bytes and 1.09 s of guest time in the harness, about 8.8 s on a real 38400-baud UART, and roughly 24 s at a full 1000-entry shortfall against the 60 s harness deadline.
      - It is also unsafe on the acceptance platform: on SLOW boot media `klog_set_deferred(1)` (`src/kernel/main/boot_media.c`) batches to RAM until `klog_disk_flush_all()`, so driving the ring to saturation evicts entries the deferred flusher has not written and trades real `kernel.log` content for filler.
      - Shape: a `KERNEL_TESTS`-only seam that exercises ring append/head/count/sequence bookkeeping WITHOUT the serial, framebuffer and disk sinks, then restores sink state. With it, §55's saturated-ring test stops skipping and becomes deterministic.
      -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §55 (item: "Prove the fix at the boundary the current tests cannot reach: a saturated ring")
- [ ] Replace head-position equality with a content-matched sequence window in the klog suite
      - `test_klog_level_drop`, `test_klog_level_pass` and `test_klog_global_level` (`src/kernel/test/test_klog.c`) all decide delivery from `head_before` vs `head_after`. The claim each wants is "an entry matching X did / did not land in the window this test opened", which a sequence bound plus a content match expresses exactly and a head comparison only approximates. `test_klog_ring_write` in the same file is the reference shape after §55.
      - The negative assertions are the load-bearing ones: an unrelated line from another CPU turns "suppressed by the global LOG_ERROR override" into a failure, so the drop tests flake first under `-smp 2`.
      - `test_klog_ctx_tid_populated` and `test_klog_ctx_subsystem_populated` read a fixed `head - 1` offset, which is the same defect in its sharpest form -- they assert against whatever entry happens to sit there.
- [ ] Prove the negative case cannot be satisfied by an unrelated line
      - A fixture that logs an unrelated entry inside the measured window must leave every drop verdict unchanged; under the current head comparison it would flip them.
- [ ] Commit: `"kernel: bound klog suite assertions by content in a locked window snapshot"`

**Test checkpoint:** every klog delivery and suppression verdict is unchanged when an unrelated entry lands inside the measured window, the converted scans read an immutable copy rather than the live ring, and the drop tests stay green on a 2-CPU boot. Test on: QEMU TCG, QEMU KVM (2 CPUs).

---

## 17. Bounded Wait Until the Sinks Have Caught Up to a Given Sequence

A caller can ask klog what the current sequence is (`klog_get_seq`), how much of it is unflushed (`klog_flush_window`), and how much the ring already lost (`klog_lost_count`), but it cannot ask to WAIT until a particular record is out. `klog_disk_flush_all()` is the closest thing and is not the same shape: it forces one flush and clears deferred mode, takes no target sequence and no timeout, and covers only the disk sink. So a subsystem that must not proceed until its last record is externally visible -- a shutdown path, a pre-reset diagnostic, a test that has to read back what it just emitted -- has no primitive to say so and instead sleeps a guessed interval or proceeds blind.

> [!NOTE]
> Filed 2026-08-02 from the `00-infrastructure/TODO-04-usermode-test-framework.md` section 57 review, where the parity pass identified it against Linux `pr_flush(seq, timeout_ms, reset_on_progress)` -- which blocks until every registered console's own cursor reaches a target sequence, with a timeout and an optional reset-on-progress so a slow-but-advancing console is not cut off. Section 57 shipped a per-record delivery RECEIPT, which is the complementary producer-side mechanism and deliberately NOT this one: the receipt tells one caller about one record at one sink, whereas this is a caller waiting on the whole pipeline reaching a point. Neither substitutes for the other, and the receipt's own header says so. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` section 57 (item: "Publish a delivery acknowledgement from klog at the point a record is actually on the wire")

- [ ] Add a bounded wait for a target sequence across the sinks that have a cursor
      - Shape: `int klog_flush_until(uint64_t target_seq, uint32_t timeout_ms)`, returning which condition ended the wait (reached, timed out, or no progress) rather than a bare success flag -- a caller that cannot tell a timeout from a completion will treat a stalled sink as a flushed one.
      - Per-sink, not one global verdict: the disk sink has a real cursor and can genuinely lag, serial is synchronous inside `klog_emit`, and the framebuffer has no durability meaning at all. Say which sinks the answer covers instead of implying all of them.
      - Reset-on-progress, as Linux does: a fixed deadline cuts off a slow-but-advancing drain, which on USB boot media is the normal case rather than the pathological one (see the USB boot hardening XREF at the top of this file for the bounded-loop work on that same path).
- [ ] Do not let the wait itself lose records
      - `klog_set_deferred(1)` batches to RAM until `klog_disk_flush_all()` (`src/kernel/main/boot_media.c` sets it for slow media). A wait that forces a drain has to leave deferred mode exactly as it found it, or it silently changes the logging policy of every subsystem that runs after it.
      - The wait must not hold `s_klog_lock` while it yields, and must not be callable from a panic or fault funnel -- both are the existing constraints on this path, not new ones.
- [ ] Regression: a target sequence that never arrives ends the wait and says so
      - Pin all three outcomes: a target already reached returns immediately, a target reached mid-wait returns reached, and a sink pinned with no progress returns the no-progress verdict within the timeout rather than spinning.
- [ ] Commit: `"klog: bounded wait until the sinks reach a target sequence"`

**Test checkpoint:** a caller that emits a record and then waits for its sequence observes the record in `kernel.log` when the wait returns reached; a wait against a sequence no one will emit returns its timeout verdict within the stated bound; and deferred-flush mode is unchanged across both. Test on: QEMU TCG, QEMU KVM.

## OS Comparison

| ⭐  | Feature                | 🪟 Win11           | 🐧 Linux             | 🚀 Impossible OS           |
| --- | ---------------------- | ------------------ | -------------------- | -------------------------- |
| 💎  | Unified kernel log     | ✅ Event Log       | ✅ journald/syslog   | ✅ klog ring buffer        |
| 💎  | Log levels             | ✅ 5 levels        | ✅ 8 POSIX levels    | ✅ 5 levels                |
| 💎  | Serial debug output    | ⚠️ Needs WinDbg    | ✅ earlyprintk       | ✅ All entries to serial   |
| ⭐  | Per-boot log files     | ❌ Not built-in    | ❌ Not built-in      | ✅ BOOT_NNN.LOG on FAT32   |
| 💎  | User-mode log API      | ✅ ReportEvent/ETW | ✅ syslog()          | ✅ SYS_LOG syscall #17     |
| 💎  | Boot-phase init        | ✅ Phase 0/1       | ✅ early_printk      | ✅ §1 -- done              |
| 💎  | Subsystem splitting    | ✅ Event channels  | ✅ syslog facilities | ✅ §2 -- done              |
| 💎  | Subsystem verbosity    | ✅ ETW filters     | ✅ per-facility      | ✅ §3 -- done              |
| 💎  | Log rotation           | ✅ Size-limited    | ✅ logrotate         | ✅ §4 -- done              |
| 💎  | Rate limiting          | ✅ ETW built-in    | ⚠️ rsyslog only      | ✅ §5 -- done              |
| ⭐  | Human-readable struct  | ❌ XML verbose     | ❌ Binary journal    | ✅ §6 -- JSON Lines        |
| 💎  | Remote forwarding      | ✅ WEF             | ✅ rsyslog UDP       | ⬜ → net/TODO-11           |
| 💎  | ETW tracing API        | ✅ NtTraceEvent    | ✅ ftrace/perf_event | ✅ §7 -- 7 NtTrace* SSDT   |
| 💎  | ETW provider registry  | ✅ EtwRegister     | ⚠️ tracefs           | ⬜ §11 -- GUID registry    |
| 💎  | ETW session filtering  | ✅ keyword/level   | ⚠️ filter exprs      | ⬜ §11 -- keyword+level    |
| 💎  | ETW stack-walk         | ✅ stack trace     | ✅ perf/eBPF         | ⬜ §12 -- RtlCapture stack |
| 💎  | ETW autologger         | ✅ boot session    | ⚠️ early ftrace      | ⬜ §12 -- boot-persistent  |
| ⭐  | Self-describing events | ✅ TraceLogging    | ❌ none              | ⬜ §12 -- schema id        |
| 💎  | Log channel tiers      | ✅ Admin/Op/etc    | ⚠️ facilities        | ⬜ deferred refinement     |
| ⭐  | Serial timestamps      | ❌ Not standard    | ❌ Not standard      | ✅ Every entry             |
| 💎  | Crash-persistent log   | ✅ Minidump + WER  | ✅ pstore/ramoops    | ✅ §8 NVRAM + reserved RAM |
| 💎  | Per-entry CPU/PID/TID  | ✅ ETW metadata    | ✅ journald _PID     | ✅ §9 -- cpu/pid/tid       |
| ⭐  | Tamper-evident log     | ❌ No integrity    | ⚠️ FSS optional      | ⬜ §10 -- HMAC-chain       |
| 💎  | Rotated log compress   | ❌ Not built-in    | ✅ logrotate gzip    | ✅ §13 -- LZ4, atomic+CRC  |

> After §1-§9, Impossible OS matches or exceeds Windows and Linux on all core logging features.
> §6 (JSON Lines), per-boot files, and serial timestamps are exclusive edges. §7 wires 7 ETW syscalls. §9 closes the per-entry context parity gap.
> §10 (HMAC-chain) is a unique competitive advantage -- no other OS ships tamper-evident text-format logging out of the box.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_klog()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
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

> **Done:** 6 suites, 8 assertions -- registered in `test_runner_init()` (2026-04-02). Subsystem dispatch tests skipped: `dispatch_filename()` is static in klog_disk.c, no public API to test tag-to-filename mapping.

- [x] Crash-persistent log tests in `test_klog.c` (§8): crash entry layout, header field roundtrip, region allocation sanity, capacity bounds
- [x] Per-entry context metadata tests in `test_klog.c` (§9): cpu_id == 0 (BSP), pid/tid populated, subsystem matches "TEST", message matches logged text, timestamp advances
- [ ] Rotation tests (§4): assert `klog_build_log_path` gen 0/.N/.tmp + cap-overflow, plus a VFS-fault test that fails the stage rename and asserts generations stay untouched (blocked: BSS page budget forbids exposing the static helper)
- [ ] Log integrity verification tests (§10): TODO-03 §5 Monocypher now shipped -- implement alongside §10; `klog_verify_chain()` not yet implemented
- [ ] ETW provider/filtering tests (§11): provider register+enumerate roundtrip; non-matching keyword dropped; matching keyword+level delivered
- [ ] ETW advanced-capture tests (§12): stack-walk >=1 resolvable frame; autologger config parse+start; self-describing schema field decode
- [x] Rotated-log compression tests (§13): `test_klog_lz4_roundtrip` -- roundtrip + negatives (bad magic, CRC mismatch, truncation, undersized dst, over-expansion). Rotation-recovery windows validated via boot (no pure-test VFS surface)
- [x] Commit: `"test: add crash-persist, context metadata tests to klog suite (TODO-04 §7-§9)"`

---

## Completed (Reference)

These items are implemented and verified. Kept here for future correctness checks.

### Core klog Infrastructure 

- [x] 5 log levels: `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL` -- `klog.h` lines 20–26
- [x] Subsystem tag on every log call (`"net"`, `"fs"`, `"mm"`, `"boot"`, etc.)
- [x] 1000-entry in-memory ring buffer -- `klog.c` `klog_ring[KLOG_RING_SIZE]`
- [x] Serial output with colored level prefixes
- [x] Framebuffer output with colored level prefixes
- [x] `LOG_FATAL` auto-halt -- fatal log entries stop the kernel

### Disk Logging 

- [x] Batch flush to `X:\Logs\kernel.log` (IXFS, appendable) -- `klog_disk.c`
- [x] Numbered boot session logs under `X:\Boot\` (per-boot `YYMMDDNN.LOG` + `.json`) and serial sessions at `X:\Logs\Serial\Serial_YYMMDDNN.log` on the BlackBox FAT32 partition -- one set per boot
- [x] Live mode -- every `klog()` entry appended and flushed immediately when enabled
- [x] 256 KB flush buffer via `pmm_alloc_contiguous()` (identity-mapped)
- [x] Reentrancy guard (`flushing` flag) prevents recursive flush

### User-Mode Integration 

- [x] `SYS_LOG` syscall (#17) -- user-mode apps can write to kernel log -- `syscall.h`/`syscall.c`
- [x] Commit: `"kernel: unified logging with disk persistence"`

## Codex Adversarial Review

> Reviewed 2026-04-05 by Codex (o3). Scope: §1-§9 implemented code.
> **Round 1:** 6 findings (1 critical, 3 high, 2 medium). All fixed.
> **Round 2:** 2 new findings. 1 fixed (cursor-advance-on-failure), 1 accepted (ring snapshot).
> **Final verdict: resolved.** Build clean.

| #   | Severity | Finding                                    | Status                                                                                         |
| --- | -------- | ------------------------------------------ | ---------------------------------------------------------------------------------------------- |
| 1   | critical | `klog()` ring buffer not SMP-safe          | **Fixed** -- added `s_klog_lock` irqsave spinlock                                              |
| 2   | high     | Flush stops after ring saturates at 1000   | **Fixed** -- monotonic `klog_ring_seq` replaces capped `ring_count`                            |
| 3   | high     | Per-subsystem logs re-append entire ring   | **Fixed** -- subsystem routing uses same seq cursor as kernel.log                              |
| 4   | high     | JSON output not safely escaped             | **Fixed** -- inline escape for `\ " \n \r \t` + control chars                                  |
| 5   | medium   | Crash recovery fails when only X: mounted  | **Fixed** -- gate on `X: \|\| C:`                                                              |
| 6   | medium   | ETW sessions never released after stop     | **Fixed** -- `NtStopTrace` clears magic + kfree(buffer)                                        |
| 7   | high     | Flush cursor advances on write failure     | **Fixed** -- cursor gated on successful VFS write                                              |
| 8   | medium   | Ring flush reads live entries without lock | **Accepted** -- inherent ring buffer behavior; locking during disk I/O would block all logging |

## Verification

- [x] `bash scripts/build.sh clean` → `=== BUILD OK ===` -- PASS: build 1939 booted successfully (WHPX, 2026-04-02)
- [x] QEMU WHPX: serial log shows `klog_early_init()` in Phase 0, `klog_disk_enable()` in Phase 2 -- PASS: `[PHASE0] KLOG (0x0051)` at 0.000s, `klog: writing to X:\Logs\...` at 4.560s after VFS mount (WHPX, 2026-04-02)
- [ ] QEMU TCG: same as WHPX -- (not tested in this log, WHPX only)
- [ ] VirtualBox: boot completes, log files created -- (manual: requires VirtualBox)
- [ ] `X:\Logs\network.log` contains only `"net"` tagged entries -- (not verifiable from serial log, requires filesystem inspection)
- [ ] `X:\Logs\boot.log` contains only `"boot"` tagged entries -- (not verifiable from serial log, requires filesystem inspection)
- [x] `klog_set_level("mm", LOG_WARN)` silences mm DEBUG entries in serial -- PASS: unit test `Klog: level drop` passed at 8.510s, confirms LOG_DEBUG dropped after set_level(mm, LOG_WARN) (WHPX, 2026-04-02)
- [ ] Rotation: `kernel.log.1` appears when `kernel.log` exceeds MaxSize -- (not verifiable from serial log, requires filesystem inspection after multiple boots)
- [ ] `events.jsonl` parseable by `jq` -- one JSON object per line -- (not verifiable from serial log, requires filesystem inspection)
- _Syslog verification moved to `07-networking/TODO-11-syslog-forwarding.md`_
- [ ] Bare metal: log files written correctly to IXFS on SATA/NVMe -- (manual: requires physical hardware)
- [ ] Commit: `"kernel: system-logging verified -- splitting, rotation, JSON events, syslog"`

---
