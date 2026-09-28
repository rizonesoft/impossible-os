<!-- docs: covers=todo/02-kernel-core/TODO-04-system-logging.md sources=src/kernel/klog.c,src/kernel/klog_disk.c,include/kernel/klog.h,src/kernel/etw.c,include/kernel/etw.h,src/kernel/test/test_klog.c reviewed=2026-09-28 order=4 -->
# System Logging (klog)

## What is it?

klog is the kernel log every subsystem writes through: a level (`LOG_DEBUG` to `LOG_FATAL`), a subsystem tag (`"net"`, `"fs"`, `"mm"` and so on) and a formatted message. Each entry goes to an in-memory ring buffer, serial output and the boot framebuffer and, once a filesystem is mounted, to log files under `X:\Logs\` (or `C:\Impossible\System\Logs\` when the BlackBox partition is not mounted). A related surface, ETW (Event Tracing for Windows), gives providers a structured tracing channel. klog is the primary tool for diagnosing boot failures and panics.

## How does it work?

klog starts in two stages that match the boot phases. `klog_early_init()` runs in Phase 0 before the VFS exists and starts only the 1000-entry ring buffer (`KLOG_RING_SIZE` in [`klog.h`](../../include/kernel/klog.h)) and serial output. `klog_disk_enable()` runs in Phase 2 once a filesystem is mounted: it opens the log files, replays crash-recovered entries and starts disk flushing.

Every `klog()` call goes through one path. It checks a per-subsystem verbosity override (a 32-entry table read under a seqlock), applies a per-subsystem rate limit (32 slots, a 1000 ms window, 100 messages by default), and stamps a surviving entry with the CPU, PID and TID before writing it to the ring, serial and framebuffer.

On disk, a flush writes each entry to `kernel.log` and, through an 18-entry tag-to-file table in [`klog_disk.c`](../../src/kernel/klog_disk.c), to one of six subsystem files (`network.log`, `boot.log`, `fs.log`, `mm.log`, `drivers.log`, `security.log`); an unmatched tag goes to `kernel.log` only. The same flush writes each entry as one JSON object per line to `X:\Logs\events.jsonl`. These files rotate once they pass a size threshold (4 MB by default), keeping 3 generations by default (at most 9). Rotated generations are LZ4-compressed to `.N.lz4` by default, with a CRC32 and a staged rename, so a failed compression falls back to a plain `.N`.

A separate path survives a panic. `klog_crash_persist()` copies the newest ring entries, with no heap or VFS use, into a 128 KiB region reserved with `pmm_alloc_contiguous()` (`KLOG_CRASH_PAGES`) whose address is kept in a UEFI NVRAM variable. On the next boot, `klog_crash_recover()` validates the region's magic and CRC32 early in Phase 0 and replays it to serial with a `[CRASH-PREV]` prefix; once the VFS is up, `klog_crash_write_to_disk()` writes the entries to `X:\Crash\crash_recovery.log`.

ETW starts in Phase 3: `etw_init()` and `etw_register_ssdt()` register seven `NtTrace*` system calls at service numbers 0x01D0 to 0x01D6 ([`etw.h`](../../include/kernel/etw.h)). Up to 8 sessions (`ETW_MAX_SESSIONS`), each with a 4 KB circular buffer, share one spinlock, and every handler copies user data in before taking it.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `klog(level, subsystem, fmt, ...)` | Log an entry, subject to level and rate-limit filtering ([`klog.h`](../../include/kernel/klog.h)) |
| `klog_unrated()`, `klog_receipted()` | Bypass the rate limiter, or also get a delivery receipt for the serial write |
| `klog_early_init()`, `klog_disk_enable()` | Phase 0 and Phase 2 initialization ([`klog.c`](../../src/kernel/klog.c)) |
| `klog_set_level()`, `klog_get_level()`, `klog_remove_override()` | Per-subsystem verbosity |
| `klog_get_ring()`, `klog_get_ring_snapshot()`, `klog_get_seq()` | Ring buffer access for the debug console and tests |
| `klog_crash_persist()`, `klog_crash_recover()`, `klog_crash_write_to_disk()` | Panic capture, next-boot recovery and disk replay |
| `klog_decompress_rotated()`, `klog_compress_buffer()` | LZ4 encode and decode for rotated archives |
| `NtTraceEvent`, `NtTraceControl`, `NtCreateTrace`, `NtQueryTrace`, `NtUpdateTrace`, `NtStopTrace`, `NtFlushTrace` | ETW system calls ([`etw.c`](../../src/kernel/etw.c)) |
| `SYS_LOG` | User-mode `sys_log(level, msg, len)` entry point |
| `HKLM\SYSTEM\Logs\Levels\<tag>`, `RateLimit\<tag>`, `MaxSize`, `MaxRotated`, `Compress` | Registry values read at boot for verbosity, rate limits, rotation size and count, and compression |

## How do I use it?

klog runs on every boot; there is nothing to enable.

```bash
bash scripts/test.sh SUITE=boot    # klog and LZ4 tests in test_klog.c
bash scripts/test.sh SUITE=abi     # the ETW tests, also in test_klog.c
```

After boot, `X:\Logs\kernel.log` holds every entry, the six subsystem files hold their tagged subset, and `X:\Logs\events.jsonl` holds the same stream as JSON Lines, readable with `jq`. Quiet a noisy subsystem with `klog_set_level("net", LOG_ERROR)` or persist that through `HKLM\SYSTEM\Logs\Levels\net`. After a panic, the next boot shows `[CRASH-PREV]` lines on serial and writes `X:\Crash\crash_recovery.log`. The tests are in [`test_klog.c`](../../src/kernel/test/test_klog.c).

## What is not implemented yet?

- Tamper-evident, HMAC-chained `events.jsonl` entries are designed but not built; where the verifier key is anchored (TPM NVRAM or protected UEFI NVRAM) is an operator decision that blocks the work ([Log Integrity Verification](../../todo/02-kernel-core/TODO-04-system-logging.md#10-log-integrity-verification)).
- ETW provider registration and per-session keyword and level filtering are not built, so every `NtTraceEvent` goes to every running session ([ETW Provider Registration and Session Filtering](../../todo/02-kernel-core/TODO-04-system-logging.md#11-etw-provider-registration-and-session-filtering)).
- ETW stack-walk capture, a boot-persistent autologger and self-describing schemas wait on the provider model ([ETW Advanced Capture](../../todo/02-kernel-core/TODO-04-system-logging.md#12-etw-advanced-capture----stack-walk-autologger-self-describing-schema)).
- Section 2 of the roadmap still has open items, and several hardening items in sections 14 to 17 are parked on the kernel image-size ceiling ([System Logging roadmap](../../todo/02-kernel-core/TODO-04-system-logging.md)).
- The rate limiter fails open when its 32-slot table is full; a per-CPU lockless redesign is planned in [Kernel Logging v2](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md).
- Remote syslog forwarding is owned by [Syslog Forwarding](../../todo/07-networking/TODO-11-syslog-forwarding.md).

## How does it compare with Windows 11 and Linux?

klog matches both on the core surface: a unified kernel log, five severity levels, per-subsystem files and verbosity, size-based rotation, rate limiting, CPU, PID and TID context per entry, and crash-persistent capture in reserved memory (comparable to Windows minidumps or Linux pstore). Its `events.jsonl` is readable as text, unlike the Windows Event Log's binary format, and parseable by tools, unlike the binary systemd journal; compressed rotation has no Windows equivalent.

It is behind Windows on ETW, where provider registration and session filtering are standard, and behind both on remote forwarding (Windows Event Forwarding, rsyslog). Tamper-evident logging would go beyond both, since the Windows Event Log has no built-in sealing and journald's Forward Secure Sealing is optional, but it is not built yet.

## See also

- [System Logging roadmap](../../todo/02-kernel-core/TODO-04-system-logging.md)
- [BlackBox Service Partition](../boot/blackbox-service-partition.md)
- [Kernel Embedded Libraries](kernel-libraries.md)
