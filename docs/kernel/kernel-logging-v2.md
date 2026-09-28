<!-- docs: covers=todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md sources=src/kernel/klog.c,include/kernel/klog.h,src/kernel/klog_disk.c,include/kernel/smp.h reviewed=2026-09-28 order=32 -->
# Kernel Logging v2 (Lockless, Priority Lanes)

## What is it?

Kernel logging v2 is a design for the next kernel logging substrate: per-CPU lockless ring buffers instead of one global lock, priority lanes so a debug flood cannot crowd out a fatal line, structured fields instead of pre-formatted text, and an emergency path that gets a fatal message to serial even when the ring is full or serial is stuck. None of it has shipped: all ten rows of the roadmap's Implementation Order table are open and no `klog_v2` symbol exists in the tree. The logging system that runs today is v1, documented in [System Logging (klog)](system-logging.md).

## How does it work?

As designed, each CPU gets a ring field in `struct per_cpu_data` ([`smp.h`](../../include/kernel/smp.h)) holding a fixed ring of cache-line-aligned entries, with the producer and consumer indices on separate cache lines so only the owning CPU writes into it. A lockless `klog_v2()` enqueues without touching a global lock. Five priority lanes (debug, info, warn, error, fatal) share the ring with per-level budgets so a lower level cannot evict a higher one. Fatal messages bypass the ring and write straight to serial with a bounded retry, falling back to a static per-CPU emergency buffer. A background drain thread reads every CPU's ring in turn and batches serial writes, so the call site no longer pays serial latency. One binary wire format would feed the JSON Lines disk writer, ETW, and a host-side decoder.

The plan ships v2 beside v1 behind a build switch until benchmarks show it faster, and only then retires v1's [`klog.c`](../../src/kernel/klog.c) and [`klog.h`](../../include/kernel/klog.h).

## What are its interfaces?

None of the v2 interfaces exist. The roadmap names `klog_v2()`, the `KLOG_DEBUG` to `KLOG_FATAL` macros, ring enqueue and dequeue helpers, per-subsystem rate limits, statistics, a fatal path, survival-snapshot capture and recovery, and a system-information class for reading the statistics. Kernel code calls the v1 `klog()` API today, with the disk writer in [`klog_disk.c`](../../src/kernel/klog_disk.c); both are covered in [System Logging (klog)](system-logging.md).

## How do I use it?

There is nothing to use yet: no v2 build switch and no v2 symbol exist. Code that logs today calls `klog()`.

## What is not implemented yet?

All ten sections:

- **Per-CPU rings and atomic primitives** ([Per-CPU SPSC Ring Buffer + Atomic Primitives](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#1-per-cpu-spsc-ring-buffer--atomic-primitives)).
- **The lockless enqueue and format helpers** ([Lockless `klog_v2()` Enqueue + Format Helpers](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#2-lockless-klog_v2-enqueue--format-helpers)).
- **Priority lanes and per-level budgets** ([Priority Lanes + Per-Level Sub-Budgets](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#3-priority-lanes--per-level-sub-budgets)).
- **The fail-proof fatal path** ([Fail-Proof FATAL Emergency Path](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#4-fail-proof-fatal-emergency-path)).
- **Per-subsystem rate limits** ([Configurable Per-Subsystem Rate Limits + Diagnostic Exemption](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#5-configurable-per-subsystem-rate-limits--diagnostic-exemption)).
- **The background drain worker** ([Background Drain Worker](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#6-background-drain-worker)).
- **Backpressure-aware serial drain** ([Backpressure-Aware Serial Drain + Pressure Counters](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#7-backpressure-aware-serial-drain--pressure-counters)).
- **The structured wire format and its ETW and JSON Lines feeds** ([Native Structured Field Wire Format + ETW + JSON Lines Feed](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#8-native-structured-field-wire-format--etw--json-lines-feed)).
- **Boot-survival snapshot and recovery** ([Boot-Survival Ring Snapshot + Recovery](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#9-boot-survival-ring-snapshot--recovery)).
- **The v1 to v2 switch, benchmarks and v1 retirement** ([v1 -> v2 Migration Switch + Benchmarks + Retire v1](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md#10-v1---v2-migration-switch--benchmarks--retire-v1)).

## How does it compare with Windows 11 and Linux?

The roadmap's comparison table sets the target: since 5.10 Linux printk writes lock-free into one shared ring (per-CPU buffers belong to the separate tracing ring buffer), Windows ETW buffers per CPU, but neither guarantees a fatal line under every failure mode, neither has per-level lanes, and neither renders one structured record as both plain text and a binary event. Until the sections above ship, Impossible OS has none of these properties; v1 is a single ring behind a global lock with text entries, as [System Logging (klog)](system-logging.md) describes.

## See also

- [Kernel Logging v2 roadmap](../../todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md)
- [System Logging (klog)](system-logging.md)
- [Serial Log Cleanliness](serial-log-cleanliness.md)
