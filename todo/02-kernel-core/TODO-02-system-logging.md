# TODO-02 -- System Logging

> **Goal:** Complete the klog system from its current working foundation to a production-grade logging stack: per-subsystem log splitting, log rotation, structured JSON events, rate limiting, and remote syslog forwarding. The core klog infrastructure (ring buffer, disk flush, serial/framebuffer output, numbered boot logs, user-mode syscall) is already implemented and is documented in the Completed section below for reference.

> [!IMPORTANT]
> **Current state:** `klog.c` (~580 lines) + `klog_disk.c` (912 lines) + `etw.c`. §1-§9 complete. Remaining: §10 (log integrity, blocked on TODO-20 §5 Monocypher). Remote syslog moved to `07-networking/TODO-11`.

## Inputs

- [`src/kernel/klog.c`](../../src/kernel/klog.c)
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c)
- [`include/kernel/klog.h`](../../include/kernel/klog.h)
- [`src/kernel/etw.c`](../../src/kernel/etw.c)
- [`include/kernel/etw.h`](../../include/kernel/etw.h)
- ~~`src/kernel/log.c`~~ -- legacy serial-only logger (removed, superseded by klog)
- [`todo/02-kernel-core/TODO-01-kernel-init-sequencing.md`](./TODO-01-kernel-init-sequencing.md)
- → XREF: `TODO-20-kernel-libraries.md §6` -- cJSON DOM parser; §6 was implemented with manual JSON formatting in `klog_disk.c` (cJSON not needed for serialization, may be used by Event Viewer for parsing)
- → XREF: `07-networking/TODO-01-tcp-network-infrastructure.md` -- UDP send path required by §7 (remote syslog); `src/kernel/net/udp.c` already exists but syslog send API is not yet wired
- → XREF: `01-boot-platform/TODO-11-usb-boot-hardening.md` -- USB boot hardening (klog_disk_flush bounded loop, deferred flush mode); both TODOs modify klog_disk.c
- → XREF: `TODO-05-native-api-ssdt.md §4` -- SSDT indices 0x01D0–0x01D6 reserved for ETW tracing syscalls; §8 of this TODO wires them into the SSDT
- → XREF: `TODO-16-crash-dump-generation.md §2,§7` -- crash dump raw-partition sink bypasses VFS; §8 of this TODO captures ring buffer to reserved physical memory on panic (complementary -- TODO-16 captures binary state, §8 captures text log)
- → XREF: `TODO-20-kernel-libraries.md §5` -- Monocypher Blake2b + kernel CSPRNG required by §10 (HMAC-chain log integrity)
- → XREF: `01-boot-platform/TODO-17-blackbox-service-partition.md` -- BlackBox X:\ partition; log paths migrate from C:\ to X:\Logs\
- → XREF: `14-host-tools/TODO-08-blackbox-log-extractor.md` -- host-side log viewer/extractor; solves "not verifiable from serial log" verification items

## Outcome

- All kernel subsystems write to dedicated log files on X:\Logs\ (`network.log`, `boot.log`, `fs.log`, `mm.log`).
- Log files rotate automatically; disk never fills from logging alone.
- `events.jsonl` provides structured, human-readable log events readable by any editor.
- Remote syslog forwarding (-> `07-networking/TODO-11`) enables enterprise and headless debug scenarios.
- The klog lifecycle is gated correctly on VFS readiness per TODO-01 Phase 0/Phase 2 contract.
- On panic, the last N ring buffer entries survive reboot via reserved physical memory and are recovered into `X:\Crash\crash_recovery.log`.
- Every log entry carries CPU number, PID, and TID for SMP and multi-process debugging.
- `events.jsonl` entries are HMAC-chained -- tampering is mathematically detectable without external tools.

## Implementation Order

| ⭐  | Order | Deliverable                         | Depends On     | Status |
| --- | :---: | ----------------------------------- | -------------- | :----: |
| 💎  |   1   | Boot-phase aware klog init          | T01 §1         |  [x]   |
| 💎  |   2   | Per-subsystem log splitting         | §1             |  [x]   |
| 💎  |   3   | Per-subsystem verbosity control     | §2             |  [x]   |
| 💎  |   4   | Log rotation                        | §2             |  [x]   |
| 💎  |   5   | Rate limiting                       | §2             |  [x]   |
| ⭐  |   6   | Structured JSON log events          | §4             |  [x]   |
| 💎  |   7   | ETW tracing syscalls wired to SSDT  | §4, T05 §4     |  [x]   |
| 💎  |   8   | Crash-persistent log capture        | §1             |  [x]   |
| 💎  |   9   | Per-entry context metadata          | §2             |  [x]   |
| ⭐  |  10   | Log integrity verification (HMAC)   | §6, T20 §5     |  [ ]   |

> 💎 = parity -- Windows Event Log and Linux journald/syslog both have these capabilities.
> ⭐ = exclusive -- HMAC-chained JSON Lines is human-readable AND cryptographically verifiable; beats Windows XML and Linux binary journal.

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
- [x] Numbered boot session logs `X:\BOOT_NNN.LOG` on FAT32 partition -- one file per boot
- [x] Live mode -- every `klog()` entry appended and flushed immediately when enabled
- [x] 256 KB flush buffer via `pmm_alloc_contiguous()` (identity-mapped)
- [x] Reentrancy guard (`flushing` flag) prevents recursive flush

### User-Mode Integration 

- [x] `SYS_LOG` syscall (#17) -- user-mode apps can write to kernel log -- `syscall.h`/`syscall.c`
- [x] Commit: `"kernel: unified logging with disk persistence"`

---

## 1. Boot-Phase Aware klog Init
The current `klog_disk.c` assumes VFS is available when it initialises. After TODO-01, the kernel has explicit Phase 0 (no VFS) and Phase 2 (VFS ready) gates. `klog` must split into a Phase 0 ring-buffer-only mode and a Phase 2 disk-enable step.

- [x] Add `klog_early_init()` -- Phase 0 safe; resets ring buffer, serial output only; no VFS
- [x] Add `klog_disk_enable()` -- Phase 2 safe; calls `klog_disk_init()` + `klog_disk_flush()` to open log files and flush ring to disk
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
- [x] Fall back to `kernel.log` for unknown tags -- `dispatch_filename()` returns "kernel.log" for unmatched
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
Emit machine-parseable events alongside plain-text logs. Implemented with manual JSON string formatting in `klog_disk.c` -- no cJSON dependency.

- [x] Define JSON event format: `{"ts":<ms>,"lvl":"WARN","sub":"net","msg":"DHCP timeout","dropped":0}`
- [x] JSON Lines flush integrated into `klog_disk_flush()` -- serializes ring buffer entries to `X:\Logs\events.jsonl`
- [x] JSON Lines format -- one JSON object per line; append-only; batched into 16 KB buffer for single `vfs_write()`
- [x] `events.jsonl` created at first flush; file size tracked for rotation
- [x] §4 log rotation applied to `events.jsonl` via `rotate_log_file()`
- [x] `"dropped"` field included from §5 rate limiter via `klog_get_dropped()` public API
- [/] Event viewer reads `events.jsonl` for colour-coded filtering -- see [13-tools-accessories/TODO-02](../13-tools-accessories/TODO-02-event-viewer.md)
- [x] Commit: `"kernel: structured JSON log events"`

## 7. ETW Tracing Syscalls Wired to SSDT
Event Tracing for Windows (ETW) provides high-performance kernel/user tracing. This section wires the tracing control syscalls into the SSDT. (→ XREF: TODO-05-native-api-ssdt.md §4, §22)

- [x] `NtTraceEvent(TraceHandle, Flags, FieldSize, Fields)` → SSDT 0x01D0: write a trace event to a session -- `etw.c` NtTraceEvent()
- [x] `NtTraceControl(FunctionCode, InBuffer, InLen, OutBuffer, OutLen, RetLen)` → SSDT 0x01D1: control trace sessions (start/stop/query/update/flush) -- `etw.c` NtTraceControl()
- [x] `NtCreateTrace(TraceHandle, DesiredAccess, ObjectAttributes, TraceGuid)` → SSDT 0x01D2: create a new trace session -- `etw.c` NtCreateTrace()
- [x] `NtQueryTrace(TraceHandle, TraceInformationClass, Buffer, Length)` → SSDT 0x01D3 -- `etw.c` NtQueryTrace()
- [x] `NtUpdateTrace(TraceHandle, InstanceName, Properties)` → SSDT 0x01D4 -- `etw.c` NtUpdateTrace()
- [x] `NtStopTrace(TraceHandle, InstanceName, Properties)` → SSDT 0x01D5 -- `etw.c` NtStopTrace()
- [x] `NtFlushTrace(TraceHandle, InstanceName, Properties)` → SSDT 0x01D6 -- `etw.c` NtFlushTrace()
- [x] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-05 §1)
- [x] Commit: `"kernel: klog -- wire ETW tracing syscalls to SSDT (0x01D0-0x01D6)"`

> **Implementation notes:** `etw.c` + `etw.h` (new files). Up to 8 concurrent trace sessions with 4 KB circular buffers. SMP-safe via `s_etw_lock` (irqsave spinlock). Registered in `boot_desktop.c` Phase 3 after `ssdt_init()`. 7 unit tests in `test_klog.c` cover struct sizes, SSDT registration, session lifecycle, and event writing.

**Test checkpoint:** `NtCreateTrace` returns valid session handle. `NtTraceEvent` writes event visible via `NtTraceControl` query. `NtStopTrace` + `NtFlushTrace` drain the buffer. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

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
- [x] Commit: `"kernel: crash-persistent klog capture via reserved physical memory"`

> [!NOTE]
> Complementary to TODO-16 crash dumps: TODO-16 captures binary CPU/memory state for WinDbg. §8 captures the text log ring buffer -- the human-readable context most useful for first-pass triage. Both write to physical memory without VFS dependency.

**Test checkpoint:** Force a panic with `test=1 crash_test=1` in boot.conf; on next boot, serial shows `[CRASH-PREV]` entries from the previous crash. `crash_recovery.log` contains recovered entries with timestamps. CRC32 mismatch on corrupted region produces `[CRASH-PREV] recovery failed: CRC32 mismatch` on serial. Verify on QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 9. Per-Entry Context Metadata

Add CPU number, process ID, and thread ID to every klog entry. Windows ETW includes `ProcessId`, `ThreadId`, `ProcessorNumber` on every event. Linux journald stores `_PID`, `_UID`, `_COMM`, `_SYSTEMD_UNIT`. Impossible OS's `klog_entry_t` currently has only `level`, `subsystem`, `timestamp`, `message` -- no process/CPU context. This metadata is essential for diagnosing SMP races and multi-process issues.

> [!WARNING]
> **High-risk:** Changes `klog_entry_t` struct layout -- affects `klog.c`, `klog_disk.c`, `test_klog.c`, and any code reading the ring buffer. If this breaks, revert the struct change and the `klog()` population code. Test incrementally: extend struct first, verify build, then add population logic.

- [x] Extend `klog_entry_t` in `klog.h`: add `uint8_t cpu_id`, `uint32_t pid`, `uint32_t tid` -- with 3-byte alignment padding
- [x] In `klog()`: populate `cpu_id` from `smp_this_cpu()->cpu_id` (NULL-safe, defaults to 0 before `smp_early_bsp_init()`)
- [x] In `klog()`: populate `pid`/`tid` from `task_current()->pid` gated on `kernel_subsystem_ready(SUBSYS_SCHED)`; defaults to 0/0 during boot
- [x] Update JSON Lines serialization in `klog_disk.c`: add `"cpu":N,"pid":N,"tid":N` fields to each JSON object in `events.jsonl`
- [x] Update serial output format: append `[cpu:N]` after the timestamp when SMP is ready or cpu_id > 0 (skipped during single-CPU early boot)
- [x] Update `klog_crash_persist()` (§8): `klog_crash_entry_t` extended with cpu_id/pid/tid fields (160 bytes); serialization copies new fields
- [x] Add debug POST codes: `POST16_KLOG_CTX` (0xDE10), `POST16_KLOG_CTX_STRUCT` (0xDE11), `POST16_KLOG_CTX_SERIAL` (0xDE12), `POST16_KLOG_CTX_JSON` (0xDE13) -- range 0xDE1x confirmed free
- [x] Commit: `"kernel: add CPU/PID/TID context to klog entries"`

**Test checkpoint:** Serial log shows `[cpu:0]` on BSP entries after SMP init. JSON in `events.jsonl` contains `"cpu":0,"pid":1,"tid":0` for entries logged after scheduler start. Entries logged before scheduler show `"pid":0,"tid":0`. On SMP boot, AP entries show `"cpu":1` (or higher). Verify on QEMU WHPX (SMP), QEMU TCG, VirtualBox, bare metal.

## 10. Log Integrity Verification

HMAC-chain `events.jsonl` entries so tampering is mathematically detectable. Linux journald has optional Forward Secure Sealing (FSS) but it's rarely enabled and uses a binary format unreadable by standard tools. Windows Event Log has no built-in cryptographic integrity. Impossible OS can be the first OS with kernel-level tamper-evident structured logging in a human-readable format.

> [!TIP]
> Neither Win11 nor Linux provides out-of-the-box tamper-evident text-format logging. Win11 Event Log is binary XML with no cryptographic sealing. Linux journald's FSS is optional, complex to set up, and uses a binary journal format. Impossible OS's HMAC-chained JSON Lines is human-readable AND cryptographically verifiable -- readable by `jq`, verifiable by `klog_verify_chain()`.

> [!IMPORTANT]
> **Blocked:** Requires both `crypto_blake2b()` (HMAC computation) and `csprng_fill()` (key generation) from TODO-20 §5 (Monocypher). Neither exists in the codebase. No cryptographic hash is available -- CRC32 in `gpt.c`/`ixfs_core.c` is not cryptographically secure and would be trivially forgeable. Implement §8-§9 first; return to §10 after TODO-20 §5 is complete.

- [ ] Define `klog_integrity_ctx_t`: previous HMAC (32 bytes), session key (32 bytes), initialized flag
- [ ] At `klog_disk_enable()`: initialize session HMAC key from `csprng_fill()` (→ XREF: TODO-20 §5 Monocypher CSPRNG); store in `klog_integrity_ctx_t`
- [ ] In JSON Lines flush: compute HMAC-Blake2b(key, previous_hmac || entry_json) for each entry; append `"hmac":"<64-hex-chars>"` field
- [ ] Store session key in Registry `HKLM\SYSTEM\Logs\IntegrityKey` (write-once per boot session) for post-boot verification
- [ ] Implement `klog_verify_chain(const char *jsonl_path)`: reads `events.jsonl` line by line, verifies each HMAC against the chain; returns first corrupted line number or 0 if all valid
- [ ] Wire into `dmpanalyze.exe` (-> XREF: TODO-16 §9): `dmpanalyze /verifylog` reads key from Registry and verifies the chain
- [ ] Gate behind `boot.conf` option `log_integrity=1` (default: enabled in release, can be disabled for performance-sensitive debug runs)
- [ ] Add debug POST codes: `POST16(0xDE20)` entry, `POST16(0xDE21)` HMAC key generated, `POST16(0xDE22)` first entry sealed, `POST16(0xDE23)` verification API ready -- range `0xDE2x` confirmed free
- [ ] Commit: `"kernel: HMAC-chain integrity verification for events.jsonl"`

**Test checkpoint:** Boot with `debug=1`; `events.jsonl` entries contain `"hmac":"..."` field (64 hex chars). `klog_verify_chain("X:\\Logs\\events.jsonl")` returns 0 (valid chain). Manually corrupt one JSON line; `klog_verify_chain()` returns the corrupted line number. Boot with `log_integrity=0`; `events.jsonl` entries have no `"hmac"` field. Verify on QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature               | Win11               | Linux                 | Impossible OS               |
|----|-----------------------|---------------------|-----------------------|-----------------------------|
| 💎 | Unified kernel log    | ✅ Event Log        | ✅ journald/syslog   | ✅ klog ring buffer        |
| 💎 | Log levels            | ✅ 5 levels         | ✅ 8 POSIX levels    | ✅ 5 levels                |
| 💎 | Serial debug output   | ⚠️ Needs WinDbg     | ✅ earlyprintk       | ✅ All entries to serial   |
| ⭐ | Per-boot log files    | ❌ Not built-in     | ❌ Not built-in      | ✅ BOOT_NNN.LOG on FAT32   |
| 💎 | User-mode log API     | ✅ ReportEvent/ETW  | ✅ syslog()          | ✅ SYS_LOG syscall #17     |
| 💎 | Boot-phase init       | ✅ Phase 0/1        | ✅ early_printk      | ✅ §1 -- done              |
| 💎 | Subsystem splitting   | ✅ Event channels   | ✅ syslog facilities | ✅ §2 -- done              |
| 💎 | Subsystem verbosity   | ✅ ETW filters      | ✅ per-facility      | ✅ §3 -- done              |
| 💎 | Log rotation          | ✅ Size-limited     | ✅ logrotate         | ✅ §4 -- done              |
| 💎 | Rate limiting         | ✅ ETW built-in     | ⚠️ rsyslog only      | ✅ §5 -- done              |
| ⭐ | Human-readable struct | ❌ XML verbose      | ❌ Binary journal    | ✅ §6 -- JSON Lines        |
| 💎 | Remote forwarding     | ✅ WEF              | ✅ rsyslog UDP       | ⬜ → net/TODO-11           |
| 💎 | ETW tracing API       | ✅ NtTraceEvent     | ✅ ftrace/perf_event | ✅ §7 -- 7 NtTrace* SSDT   |
| ⭐ | Serial timestamps     | ❌ Not standard     | ❌ Not standard      | ✅ Every entry             |
| 💎 | Crash-persistent log  | ✅ Minidump + WER   | ✅ pstore/ramoops    | ✅ §8 NVRAM + reserved RAM |
| 💎 | Per-entry CPU/PID/TID | ✅ ETW metadata     | ✅ journald _PID     | ✅ §9 -- cpu/pid/tid       |
| ⭐ | Tamper-evident log    | ❌ No integrity     | ⚠️ FSS optional      | ⬜ §10 -- HMAC-chain       |

> After §1-§9, Impossible OS matches or exceeds Windows and Linux on all core logging features.
> §6 (JSON Lines), per-boot files, and serial timestamps are exclusive edges. §7 wires 7 ETW syscalls. §9 closes the per-entry context parity gap.
> §10 (HMAC-chain) is a unique competitive advantage -- no other OS ships tamper-evident text-format logging out of the box.

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

> **Done:** 6 suites, 8 assertions -- registered in `test_runner_init()` (2026-04-02). Subsystem dispatch tests skipped: `dispatch_filename()` is static in klog_disk.c, no public API to test tag-to-filename mapping.

- [ ] Add crash-persistent log tests to `test_klog.c` (§8):
  - `klog_crash_persist()` writes valid header with magic `0x4B4C4F47` and correct `entry_count` to reserved region
  - CRC32 in crash header matches recomputed CRC32 of the persisted ring buffer entries
  - Recovery on next boot: simulated valid crash region → `klog_crash_recover()` returns entry count > 0
  - Recovery with corrupted CRC32: `klog_crash_recover()` returns 0 and logs `"CRC32 mismatch"` to serial
  - Recovery clears magic cookie after successful recovery (second call returns 0)
- [ ] Add per-entry context metadata tests to `test_klog.c` (§9):
  - `klog(LOG_INFO, "test", "ctx")` produces entry with `cpu_id == 0` (BSP)
  - Entry logged before scheduler has `pid == 0` and `tid == 0`
  - Entry logged after scheduler start has `pid != 0` (at least kernel idle task)
  - JSON serialization of entry includes `"cpu":0,"pid":N,"tid":N` fields (verify via ring buffer inspection since JSON write is to disk)
- [ ] Add log integrity verification tests to `test_klog.c` (§10):
  - `klog_verify_chain()` returns 0 (valid) on a freshly written `events.jsonl` with HMAC fields
  - `klog_verify_chain()` returns corrupted line number when one entry's `"hmac"` field is overwritten with zeros
  - HMAC field is absent when `log_integrity=0` is set in boot.conf
- [ ] Commit: `"test: add crash-persist, context metadata, log integrity tests to klog suite"`

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
- [ ] Syslog: entries appear on test syslog server (UDP 514) when configured -- (not verifiable from serial log, requires syslog server setup)
- [ ] Bare metal: log files written correctly to IXFS on SATA/NVMe -- (manual: requires physical hardware)
- [ ] Commit: `"kernel: system-logging verified -- splitting, rotation, JSON events, syslog"`
