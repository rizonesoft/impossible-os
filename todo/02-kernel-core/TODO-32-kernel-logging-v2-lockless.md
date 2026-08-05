---
schema_version: 1
id: kernel-logging-v2-lockless
domain: 02-kernel-core
status: active
title: "TODO-32 -- Kernel Logging v2: Lockless, Priority-Lanes, Fail-Proof"
---

# TODO-32 -- Kernel Logging v2: Lockless, Priority-Lanes, Fail-Proof

> **Goal:** Rebuild the kernel logging substrate from a single-lock global ring (TODO-04 v1) into a per-CPU lockless architecture with priority lanes, structured native fields, fail-proof FATAL guarantee, and a backpressure-aware drain worker. The v1 design works for normal-rate boot output but fails predictably under three real-world conditions: (1) a single subsystem bursts faster than 100 msgs/window (test runs, panic-time diagnostics, future telemetry), (2) multiple CPUs contend for the global lock during interrupt-storm scenarios (NIC RX completion, AHCI command storm), and (3) a recursive `klog()` from the rate-limit summary path can land in pathological lock states. v2 eliminates the global lock entirely (per-CPU SPSC rings), introduces priority lanes so a debug flood can never drop a panic line, makes diagnostic subsystems (`TEST`, etc.) exempt from rate limits without losing limiting for real noise, and ships native structured fields (timestamp/cpu/pid/tid/level/tag/file/line/msg as separate fields, not concatenated text) so log consumers do not parse strings. Reference designs: Linux 5.10 printk lockless (Senozhatsky/Mladek 2020), Windows ETW per-CPU buffers, Apple OSLog structured + privacy-aware fields.

> [!IMPORTANT]
> **Current state:** TODO-04 v1 klog is fully working: `klog.c` ~580 lines with a global ring, `s_klog_lock` spinlock-irqsave, single rate-limit table, recursive `klog()` from `rate_check`. It boots fine on all four platforms and has correctly served the project to date. v2 is a structured replacement, not a bug-fix patch -- it ships alongside v1 behind a build-time switch (`KLOG_V2=1`) until benchmarks prove parity, then v1 is retired in §10. This TODO supersedes TODO-04 §5 (rate limit) and TODO-04 §9 (per-entry context fields are reorganised); other TODO-04 work (disk flush, JSON Lines, ETW, HMAC integrity §10) is preserved and consumed by v2 unchanged.

> [!TIP]
> **Competitive position:** No existing OS provides ALL of {per-CPU lockless, native structured fields, fail-proof fatal-never-drops, plain-text + structured simultaneous render, HMAC-chained tamper-evident on-disk format, NMI-safe enqueue, boot-survival snapshot}. Linux has lockless + plain-text but binary structured journal is separate. ETW has structured + per-CPU but no fail-proof and no plain-text simultaneous. macOS OSLog has structured + privacy but proprietary daemon dependency. v2 unifies all of these in a single in-kernel layer with no external dependencies.

## Inputs

- [`src/kernel/klog.c`](../../src/kernel/klog.c) -- v1 implementation, replaced by v2
- [`include/kernel/klog.h`](../../include/kernel/klog.h) -- public API; v2 adds new entry points, keeps v1 ABI
- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c) -- disk flush, consumed unchanged
- [`src/kernel/etw.c`](../../src/kernel/etw.c) -- ETW tracing, fed by v2 structured fields
- [`include/kernel/smp.h`](../../include/kernel/smp.h) -- per-CPU data area; v2 adds ring fields
- [`src/kernel/sched/spinlock.h`](../../include/kernel/sched/spinlock.h) -- SPSC ring uses atomic primitives, not spinlocks
- [`src/kernel/timer.c`](../../src/kernel/timer.c) -- `uptime_ns()` for timestamps
- [`src/kernel/drivers/serial.c`](../../src/kernel/drivers/serial.c) -- output sink; v2 adds backpressure-aware path
- → XREF: [`TODO-04-system-logging.md`](./TODO-04-system-logging.md) -- v1 foundation; v2 supersedes §5 (rate limit) and reorganises §9 (per-entry context); §2-§6, §8, §1, §7, §10 are preserved
- → XREF: [`TODO-31-kernel-bulletproofing.md`](./TODO-31-kernel-bulletproofing.md) §1 -- per_cpu_data assembly offsets must be updated to add v2 ring fields; 5-layer defense applies
- → XREF: [`TODO-28-bsod-ux-enhancements.md`](./TODO-28-bsod-ux-enhancements.md) §3 -- BSOD reads recent klog entries via `klog_get_recent`; v2 ring layout change requires BSOD-side adapter
- → XREF: [`TODO-27-crash-dump-generation.md`](./TODO-27-crash-dump-generation.md) §2,§7 -- crash dump persists v2 ring snapshot; v2 §9 boot-survival snapshot uses TODO-27 reserved memory region
- → XREF: [`TODO-07-irql-model-dpcs.md`](./TODO-07-irql-model-dpcs.md) §4 -- v2 drain worker uses DPC infrastructure; consumes per-CPU DPC queue
- → XREF: [`TODO-12-native-api-ssdt.md`](./TODO-12-native-api-ssdt.md) §10 -- `NtQuerySystemInformation` SystemEventLog class consumes v2 structured fields
- → XREF: [`TODO-08-time-filetime-management.md`](./TODO-08-time-filetime-management.md) §9 -- `uptime_ns()` is the v2 timestamp source; TSC-derived monotonic time is mandatory for cross-CPU timestamp ordering

## Outcome

- v2 architecture: per-CPU SPSC ring (no global lock), atomic enqueue, lockless reader for drain worker.
- Priority lanes: each level (DEBUG/INFO/WARN/ERROR/FATAL) has its own per-CPU sub-budget; FATAL bypasses all rate limits and uses a dedicated emergency path that bypasses the ring entirely on overflow.
- Native structured fields: `klog_entry_t` carries timestamp_ns, cpu_id, pid, tid, level, tag, file, line, msg as separate fields; renderers (serial text, JSON Lines disk, ETW binary) consume the struct, not a pre-formatted string.
- Configurable rate limits: `klog_set_rate_limit(tag, msgs_per_sec)` companion to existing `klog_set_level`; `TEST` tag exempt by default.
- Background drain worker: a dedicated kernel thread reads per-CPU rings and writes serial in batches, eliminating serial latency from the call site. Hot path is enqueue-only (~50 cycles).
- Backpressure: when serial cannot keep up, the worker batches and the rings expose a "pressure" counter without dropping non-debug messages.
- NMI-safe enqueue: the ring atomic primitives are reentrant; a klog from an NMI handler does not deadlock.
- Boot-survival: ring snapshot is captured to a reserved physical region on panic and recovered into `X:\Logs\crash_recovery_v2.log` on next boot.
- Wire format: binary header + payload, machine-parseable, version-tagged. A host-side `dmplog v2` decoder reads the binary format directly without text parsing.
- Eliminates the recursive `klog()` from the rate-limit summary path (the v1 fragility documented in TODO-04 §5).

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On         | Status |
| --- | ----- | ---------------------------------------- | ------------------ | ------ |
| ⭐   | 1     | Per-CPU SPSC ring buffer + atomic primitives | T31 §1             | [ ]    |
| ⭐   | 2     | Lockless `klog_v2()` enqueue + format helpers | §1                 | [ ]    |
| ⭐   | 3     | Priority lanes + per-level sub-budgets   | §1, §2             | [ ]    |
| ⭐   | 4     | Fail-proof FATAL emergency path (bypasses ring on overflow) | §3                 | [ ]    |
| 💎   | 5     | Configurable per-subsystem rate limits + TEST exemption | §3                 | [ ]    |
| ⭐   | 6     | Background drain worker (per-CPU read, serial batch write) | §1, §2, T07 §4     | [ ]    |
| 💎   | 7     | Backpressure-aware serial drain + pressure counters | §6                 | [ ]    |
| ⭐   | 8     | Native structured field wire format + ETW + JSON Lines feed | §2, T04 §6, T04 §7 | [ ]    |
| ⭐   | 9     | Boot-survival ring snapshot + recovery   | §1, T27 §2         | [ ]    |
| 💎   | 10    | v1 -> v2 migration switch (`KLOG_V2=1`) + benchmarks + retire v1 | §2..§9, T04        | [ ]    |

> ⭐ = exclusive: no other production OS combines lockless per-CPU + fail-proof + structured + plain-text + HMAC-chained.
> 💎 = parity: feature exists in Linux/Windows but at lower quality (binary-only, daemon-dependent, or non-fail-proof).

## 1. Per-CPU SPSC Ring Buffer + Atomic Primitives

Each CPU owns a single-producer single-consumer ring of `klog_entry_v2_t`. Producer is the enqueueing CPU (interrupt-disabled or preemption-disabled). Consumer is the drain worker thread (§6). Atomic head/tail indices use `__atomic_load_n` / `__atomic_store_n` with `__ATOMIC_RELAXED` for indices and `__ATOMIC_RELEASE` / `__ATOMIC_ACQUIRE` for the data barrier between writing the entry and publishing the new head.

- [ ] Define `klog_entry_v2_t` in `include/kernel/klog_v2.h`: `{uint64_t timestamp_ns; uint32_t cpu_id; uint32_t pid; uint32_t tid; uint8_t level; uint8_t lane; uint16_t tag_id; uint16_t file_id; uint16_t line; uint16_t msg_len; char msg[224];}` -- 256 bytes total, 64-byte cache aligned
- [ ] Define `klog_ring_v2_t`: `{atomic_uint32_t head; uint32_t _pad1[15]; atomic_uint32_t tail; uint32_t _pad2[15]; klog_entry_v2_t entries[KLOG_RING_V2_DEPTH];}` -- head/tail in separate cache lines (128-byte separation) to prevent producer/consumer false sharing
- [ ] `KLOG_RING_V2_DEPTH = 256` per CPU (256 * 256B = 64 KiB per CPU; 64 CPUs = 4 MiB max). Power-of-two for `& (DEPTH-1)` index masking
- [ ] Add `klog_ring_v2_t klog_ring;` field to `struct per_cpu_data` in `include/kernel/smp.h` -- update T31 §1 5-layer defense (assembly offsets, runtime verification, unit test)
- [ ] Implement `klog_ring_v2_init(klog_ring_v2_t *r)`: zero head/tail, do NOT zero entries (entries are overwritten on enqueue; zeroing 64 KiB per CPU at boot wastes time)
- [ ] Implement `int klog_ring_v2_try_enqueue(klog_ring_v2_t *r, const klog_entry_v2_t *e)`: atomic load head, check `(head + 1) & MASK != tail` (full check), copy entry to `entries[head & MASK]` with `__builtin_memcpy`, atomic store-release new head; return 0 on success, -1 if ring full
- [ ] Implement `int klog_ring_v2_try_dequeue(klog_ring_v2_t *r, klog_entry_v2_t *out)`: atomic load tail, check `tail != head` (empty check), copy `entries[tail & MASK]` to `out`, atomic store-release new tail; return 0 on success, -1 if empty
- [ ] Add KLOG_V2_DROP_COUNTER: atomic counter incremented on `try_enqueue` ring-full failure; per-CPU, no global contention
- [ ] Commit: `"kernel: klog v2 -- per-CPU SPSC ring buffer infrastructure"`

**Test checkpoint:** Boot with `KLOG_V2=1`. Per-CPU ring init is logged via v1 fallback: `klog v2: ring init OK on cpu 0..N`. Unit test fills a ring to capacity, asserts `try_enqueue` returns -1 on full, drains via `try_dequeue`, asserts `try_enqueue` succeeds again. Verify on QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal. SMP correctness: spawn 4 kthreads on different CPUs each calling `klog_ring_v2_try_enqueue` 10000 times to their own per-CPU ring; assert no entries lost (drop counter == 0) and no torn writes (each entry's checksum matches).

## 2. Lockless `klog_v2()` Enqueue + Format Helpers

The hot-path entry point. Builds a `klog_entry_v2_t` on the caller's stack, copies into the per-CPU ring via §1 atomic enqueue, returns. Never touches a global lock. Format helpers handle the printf-style varargs into the bounded `msg[224]` field with explicit truncation marker.

- [ ] Implement `void klog_v2(uint8_t level, const char *tag, const char *file, int line, const char *fmt, ...)` in `src/kernel/klog_v2.c`: builds `klog_entry_v2_t` on stack, calls `kvsnprintf(e.msg, sizeof(e.msg), fmt, ap)`, fills timestamp via `uptime_ns()`, fills cpu_id via `smp_this_cpu()->cpu_id`, fills pid/tid from current task if scheduler ready, calls `klog_ring_v2_try_enqueue(&smp_this_cpu()->klog_ring, &e)`
- [ ] Implement `static int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)`: bounded printf with explicit `[trunc]` suffix when buffer is full, returns number of chars actually written (not C99-style would-have-written)
- [ ] Define convenience macros: `KLOG_DEBUG(tag, fmt, ...)`, `KLOG_INFO(...)`, `KLOG_WARN(...)`, `KLOG_ERROR(...)`, `KLOG_FATAL(...)` -- expand to `klog_v2(LEVEL, tag, __FILE__, __LINE__, fmt, ...)`
- [ ] Tag interning: `tag_id` is a 16-bit index into a static `s_tag_table[256]` populated on first use; `klog_v2` calls `tag_intern(tag)` which does a linear scan + atomic-CAS append on first sighting. Avoids storing arbitrary string pointers in the ring (entry size stays bounded; rendering looks up the tag string from the table)
- [ ] File interning: same scheme for `file_id` -- 16-bit index into `s_file_table[512]`
- [ ] Add `KLOG_V2_NESTED_GUARD` per-CPU recursion-depth counter; if a klog_v2 call originates from inside another klog_v2 (e.g. via panic from format helper), bypass enqueue and write directly to serial via §4 emergency path
- [ ] Disable preemption + interrupts during `klog_v2` body via `local_irq_save`/`restore` -- the SPSC ring requires the producer to be uninterrupted on its own CPU. Hold time: under 200 cycles (no I/O, no allocation)
- [ ] Commit: `"kernel: klog v2 -- lockless enqueue + format helpers"`

**Test checkpoint:** Hot-path benchmark: tight loop calling `KLOG_INFO("bench", "msg %u", i)` for 1 million iterations on a single CPU; report cycles/call. Target under 200 cycles. Compare to v1 (`klog(LOG_INFO, ...)`) -- target at least 5x faster. Concurrent multi-CPU enqueue: 4 kthreads spamming logs to their own per-CPU rings; assert zero false sharing via PMU counters (cache-line invalidations between CPUs ~ 0). Verify all 4 platforms.

## 3. Priority Lanes + Per-Level Sub-Budgets

Each priority level has its own slice of the per-CPU ring depth. DEBUG fills a small lane that gets recycled fast; FATAL has a reserved lane that DEBUG cannot consume. A debug flood from one subsystem cannot push out an error from another subsystem.

- [ ] Lane-aware ring layout: split `klog_ring_v2_t::entries[256]` into 5 fixed-size lanes within the same array: `LANE_DEBUG=0..63 (64 slots)`, `LANE_INFO=64..159 (96 slots)`, `LANE_WARN=160..207 (48 slots)`, `LANE_ERROR=208..239 (32 slots)`, `LANE_FATAL=240..255 (16 slots)`. Each lane has its own head/tail pair in a `klog_lane_t` substructure
- [ ] Update `klog_ring_v2_try_enqueue` to take `lane_id` parameter, write to `entries[lane.base + (lane.head & lane.mask)]`
- [ ] Update `klog_v2` to pick lane from `level`: `lane_for_level(level)` returns 0..4
- [ ] Drain worker (§6) reads lanes in priority order: FATAL first, then ERROR, WARN, INFO, DEBUG. Within a single drain pass, FATAL is fully drained before WARN gets a chance
- [ ] Per-lane drop counter: `lane_drops[5]` per CPU; the rendered serial line on first drop per lane reads `[STUB] kernel/klog: lane <name> dropped <n> entries since last drain`
- [ ] Define lane budget macros in `include/kernel/klog_v2.h`: `KLOG_LANE_DEBUG_DEPTH 64` etc; static_assert sum equals `KLOG_RING_V2_DEPTH`
- [ ] Lane runtime override: `klog_set_lane_depth(level, depth)` reallocates lanes; protected by per-CPU init-only path (called once at boot)
- [ ] Exception-telemetry retention lane: `exception_dispatch` events get a bounded reservation lane so a storm cannot evict other diagnostics from the ring (TODO-23 §16 v1 only rate-caps) → XREF: `02-kernel-core/TODO-23 §16`
- [ ] Commit: `"kernel: klog v2 -- priority lanes + per-level sub-budgets"`

**Test checkpoint:** Spam 1000 DEBUG messages followed by 1 ERROR; drain; assert ERROR is rendered first and DEBUG entries occupy the LANE_DEBUG slot range (verifiable via `klog_v2_dump_state()` introspection). Spam to fill DEBUG lane to overflow; assert DEBUG drop counter increments but ERROR / WARN lanes still accept new entries. Verify all 4 platforms.

## 4. Fail-Proof FATAL Emergency Path

FATAL must NEVER drop. Even if the per-CPU ring is full. Even if serial is hung. Even if we are in NMI context. The emergency path bypasses the ring and the worker entirely, writing directly to serial via a dedicated bounded-retry path that is preemption-safe and reentrant. If serial is unavailable, the message is captured to a static emergency buffer that boot-survival (§9) will recover.

- [ ] Define `klog_emergency_buf[16][256]` static circular buffer (4 KiB total); per-CPU pointer + atomic head; written under `__atomic_compare_exchange_n` on head increment
- [ ] Implement `void klog_v2_fatal(const char *tag, const char *file, int line, const char *fmt, ...)`: bypasses the ring entirely, formats directly to a stack buffer, writes to serial via `serial_write_emergency()` (a dedicated entry point that takes a bounded-retry spinlock, max 1000 spin-retries, then gives up to avoid hang), simultaneously writes to `klog_emergency_buf` for boot-survival recovery
- [ ] `serial_write_emergency()` in `src/kernel/serial.c`: never blocks indefinitely; if hardware UART is wedged, gives up after 1000 polls of LSR THRE bit and increments `s_serial_emergency_drops` counter
- [ ] FATAL also enqueues into its lane in §3 for normal drain (so non-emergency consumers see the message); the emergency path is additive, not exclusive
- [ ] Update `KLOG_FATAL` macro to call `klog_v2_fatal` instead of `klog_v2(LEVEL_FATAL, ...)`
- [ ] Add `klog_v2_emergency_replay(void)`: dumps `klog_emergency_buf` contents to serial; called by panic handler after unwinding to ensure all FATAL messages are visible before halt
- [ ] NMI-safe verification: a FATAL from within an NMI handler must not recurse into format buffers shared with thread context; emergency buf uses per-CPU stripes
- [ ] Commit: `"kernel: klog v2 -- fail-proof FATAL emergency path"`

**Test checkpoint:** Synthetic test calling `KLOG_FATAL` 32 times in a tight loop while drain worker is paused; assert all 32 messages appear in serial (no drops). Stress test: pause serial drain, fill ring to capacity, then issue `KLOG_FATAL` -- assert FATAL bypasses ring and reaches serial via emergency path. NMI-context test: trigger an NMI handler that calls `KLOG_FATAL`; assert no deadlock and message is rendered. Verify all 4 platforms.

## 5. Configurable Per-Subsystem Rate Limits + Diagnostic Exemption

`klog_set_rate_limit(tag, msgs_per_sec)` lets each subsystem opt into a custom rate limit. Diagnostic tags (`TEST`, `bench`, `trace`) are exempt by default -- legitimate to flood. Real noise (`net`, `usb`) keeps tight default. Rate limit is per-CPU (no global table) so SMP scaling holds.

- [ ] `klog_rate_v2_t` per CPU: `{tag_id, count, window_start_ns, max_rate}`; static array `s_rate[KLOG_RATE_V2_SLOTS=64]` per CPU
- [ ] `klog_set_rate_limit(const char *tag, uint32_t msgs_per_sec)` API: looks up `tag_id` (creates if missing), sets `max_rate`; called at subsystem init
- [ ] Default rate-limit table at `klog_v2_init`: `{"TEST", 0}` (exempt, 0 = unlimited), `{"bench", 0}`, `{"trace", 0}`, `{"usb", 50}`, `{"net", 100}`, all others use compile-time default `KLOG_RATE_V2_DEFAULT=200`
- [ ] Rate check happens BEFORE enqueue (in §2 hot path); rate-limit slot lookup is O(N) over 64 slots, ~ 50 cycles
- [ ] Drop summary: when rate limit drops a message, increment `tag_drops[tag_id]`; the drain worker (§6) emits a single `[STUB]` summary per second per dropped tag (`"klog/<tag> rate-limit dropped N msgs/sec"`)
- [ ] NO recursive `klog()` from rate-limit summary -- the worker emits the summary as a regular ring entry, not a re-call into `klog_v2`. This eliminates the v1 fragility documented in TODO-04 §5
- [ ] FATAL bypasses rate limit entirely (per §4 design; rate check is skipped for `level == LEVEL_FATAL`)
- [ ] Commit: `"kernel: klog v2 -- configurable per-tag rate limits + diagnostic exemption"`

**Test checkpoint:** Spam 5000 messages tagged `"net"` at default rate 100/sec; assert exactly 100 are enqueued in the first second and the rest are dropped with a single summary line. Spam 5000 messages tagged `"TEST"`; assert all 5000 are enqueued (exempt). Call `klog_set_rate_limit("net", 1000)`; spam 5000 again; assert all 5000 are enqueued. Verify all 4 platforms.

## 6. Background Drain Worker

A dedicated kernel thread reads per-CPU rings round-robin, batches entries, and writes to serial. Eliminates serial latency from the call site (the v1 design has each `klog()` write to serial inside the lock, multiplying jitter). The worker uses DPC-friendly patterns from TODO-17 §4.

- [ ] Spawn `klog_drain_worker_thread` via `kthread_create` at end of boot Phase 3 (after timer + smp ready). Thread runs at IRQL PASSIVE_LEVEL with priority slightly above user threads
- [ ] Worker loop: for each CPU, dequeue up to `KLOG_DRAIN_BATCH=32` entries by lane priority (FATAL > ERROR > WARN > INFO > DEBUG), build batched serial buffer (~8 KiB), call `serial_write_batched()` once per batch
- [ ] If all per-CPU rings are empty, the worker calls `event_wait_timeout(&klog_v2_pending_event, 10ms)` to sleep -- the event is signalled by `klog_v2` enqueue when a ring goes from empty to non-empty (avoids busy-poll while keeping latency under 10ms)
- [ ] On rate-limit drop summary: worker emits one `[STUB] klog/<tag> rate-limit dropped N` line per second per affected tag (replaces the v1 recursive klog from rate_check)
- [ ] On lane drop summary: worker emits one `[STUB] klog/<lane> lane dropped N` line per drain pass when drop counter advanced
- [ ] Worker exposes runtime stats via `klog_v2_get_stats()`: total enqueued, total dequeued, total drops by lane and by tag, drain batch size avg/max, serial latency avg/max
- [ ] Worker can be suspended via `klog_v2_drain_pause()` for debugging; while paused, FATAL messages still go via emergency path (§4)
- [ ] `klog_v2()` enqueue must never synchronously enter the live-disk flush: v1 `klog_disk_flush` calls `vfs_write`, so a PASSIVE caller holding a VFS lock deadlocks. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §2` (charge API)
- [ ] Commit: `"kernel: klog v2 -- background drain worker"`

**Test checkpoint:** Boot with worker enabled; assert serial output appears within 10ms of `klog_v2` calls under normal load. Pause worker via `klog_v2_drain_pause()`; spam 100 INFO messages; resume; assert all 100 appear in batch. Stats query returns sensible values. Verify all 4 platforms.

## 7. Backpressure-Aware Serial Drain + Pressure Counters

When serial cannot keep up, the worker batches and the ring exposes a pressure counter. Non-debug messages are NEVER dropped due to backpressure -- only DEBUG lane is sacrificed. Pressure counters surface to user-mode via a new `NtQuerySystemInformation` class so monitoring tools can graph log throughput vs serial saturation.

- [ ] `serial_write_batched(const char *buf, size_t len)`: writes to a 64 KiB serial output ring; the actual UART transmission is driven by serial's own background pump that polls THRE bit. Returns immediately on enqueue; falls back to synchronous write only if output ring is full
- [ ] When serial output ring is full, worker SUSPENDS draining LANE_DEBUG only; LANE_INFO and above continue to drain. DEBUG entries accumulate until serial catches up
- [ ] If serial ring is full AND DEBUG lane is full: increment `klog_pressure_critical` counter and rate-drop NEW DEBUG entries at enqueue time (not at drain time). All non-DEBUG continues to enqueue normally
- [ ] Pressure counters in per-CPU stats: `pressure_low_drops_debug`, `pressure_critical_active`
- [ ] `NtQuerySystemInformation(SystemKlogV2Info, ...)` returns pressure summary across all CPUs (sum drops, max critical-active CPU). Wired through TODO-12 §10
- [ ] Boot config: `klog.v2.serial.ring_size_kb` defaults to 64; can grow via boot.conf for environments with slow serial (cross-VM emulated UART, slow USB-serial dongles)
- [ ] Commit: `"kernel: klog v2 -- backpressure-aware serial drain"`

**Test checkpoint:** Throttle serial to 9600 baud (slow UART); spam 10000 mixed-level messages; assert FATAL/ERROR/WARN/INFO all reach serial (just slowly), DEBUG entries are partially dropped with summary. Pressure counter is non-zero during throttle. Verify all 4 platforms.

## 8. Native Structured Field Wire Format + ETW + JSON Lines Feed

The on-the-wire format for disk + remote consumers is a binary record with a version byte, fixed-size header, and variable-length fields. Plain-text serial render is computed from the same struct -- one source, two outputs. ETW provider (TODO-04 §1) consumes the binary form directly. JSON Lines disk (TODO-04 §8) is rendered from the struct fields, not parsed from a pre-formatted text string.

- [ ] Define wire format `klog_wire_v2_t`: `{uint8_t version=2; uint8_t level; uint16_t tag_id; uint16_t file_id; uint16_t line; uint32_t cpu_id; uint64_t timestamp_ns; uint32_t pid; uint32_t tid; uint16_t msg_len; uint8_t msg[]}` -- variable length, network byte order for portability
- [ ] Add `s_tag_table[256]` and `s_file_table[512]` snapshot at start of every disk flush so consumer can decode `tag_id`/`file_id` without ambiguity (table grows monotonically; snapshot delta written when new entries added)
- [ ] Update `klog_disk.c` JSON Lines flush to read `klog_entry_v2_t` directly: emit `{"ts":<ns>,"cpu":N,"pid":P,"tid":T,"level":"WARN","tag":"net","file":"net.c","line":42,"msg":"..."}` -- structured fields native, no string parsing
- [ ] Native `exception_dispatch` event: emit the TODO-23 §16 telemetry as a top-level structured record (type/code/addr/handler/disposition/frames_unwound as fields), not a `msg`-embedded JSON string → XREF: `02-kernel-core/TODO-23 §16`
- [ ] Update `etw.c` to read `klog_entry_v2_t` and emit ETW binary record without re-formatting
- [ ] Plain-text serial render: `klog_render_text(const klog_entry_v2_t *e, char *buf, size_t cap)` -- existing v1 format `[ts] [cpu] [LEVEL] tag: msg`
- [ ] Add host-side `dmplog2` decoder (added to 14-host-tools): reads binary wire format, prints human-readable, supports filtering by tag/level/cpu
- [ ] Backward compat: `dmplog2 --convert-from-v1 <path>` reads v1 ring snapshot, converts to v2 wire format
- [ ] Commit: `"kernel: klog v2 -- structured wire format + ETW/JSON feed"`

**Test checkpoint:** After 100 mixed log calls, JSON Lines flush emits valid `events.jsonl` with structured fields. `dmplog2 X:\Logs\events.bin` decodes correctly. `dmplog2 --filter level=ERROR` shows only ERROR entries. ETW provider emits binary records readable by host-side ETW consumer. Verify all 4 platforms.

## 9. Boot-Survival Ring Snapshot + Recovery

On panic, all per-CPU rings are captured to a reserved physical region (16 MiB carved out of high RAM at boot, surviving warm reset). On next boot, the recovery path reads the region, validates the magic + checksum, writes the captured entries to `X:\Logs\crash_recovery_v2.log` in JSON Lines format, then zeroes the region for the next crash.

- [ ] Reserve 16 MiB physical region at boot via `pmm_reserve_range(KLOG_V2_SURVIVAL_BASE, 16*1024*1024)`. Address comes from a fixed offset above kernel image; documented in `boot_init.h` (POST16 range `0xDF50..0xDF5F` for survival ops)
- [ ] Define `klog_survival_v2_t` layout in region: `{uint64_t magic="KLOGV2RX"; uint32_t version=2; uint32_t total_bytes; uint32_t cpu_count; per_cpu_snapshot[cpu_count];}` -- each per-CPU snapshot has tag/file table + ring entries
- [ ] On panic: `klog_v2_capture_to_survival()` walks all per-CPU rings, writes snapshot to reserved region, computes Blake2b checksum of all data, writes checksum to header. Called from panic handler before halt
- [ ] On boot Phase 3 (after VFS mount): `klog_v2_recover_survival()` checks magic at reserved region; if valid, reads all per-CPU snapshots, renders to `X:\Logs\crash_recovery_v2.log` (JSON Lines), zeroes magic to prevent double-recovery
- [ ] Reserved region must survive UEFI boot-services exit; bootloader marks it as `EfiReservedMemoryType` (TODO-01 boot ABI integration)
- [ ] Test mode: `klog_v2_force_survival_capture()` writes a known marker entry, simulates a panic, reboot, verifies `crash_recovery_v2.log` contains the marker
- [ ] Commit: `"kernel: klog v2 -- boot-survival ring snapshot + recovery"`

**Test checkpoint:** Boot, write 100 marker INFO messages, trigger `panic("test")`, reboot. Assert `X:\Logs\crash_recovery_v2.log` contains all 100 markers in JSON Lines format with original timestamps and CPU IDs. Verify on QEMU WHPX (most reliable for warm reset), VirtualBox, bare metal. TCG resets clear RAM so survival not testable there.

## 10. v1 -> v2 Migration Switch + Benchmarks + Retire v1

Ship v2 alongside v1 behind a build-time flag (`KLOG_V2=1`) until benchmarks prove parity on every platform, then retire v1. Migration must be observable: a single boot config flag flips, all subsystems use the new entry points via macro indirection. No subsystem-by-subsystem migration -- big-bang or nothing.

- [ ] Build flag `KLOG_V2`: when set, `include/kernel/klog.h` redirects `klog(...)` to `klog_v2(...)` via macro `#define klog(level, tag, fmt, ...) klog_v2((level), (tag), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)`
- [ ] When `KLOG_V2=0`: existing v1 paths unchanged. Allows side-by-side benchmarks
- [ ] Benchmark suite `src/kernel/test/test_klog_v2_perf.c`: hot-path cycles/call (target under 200), serial latency end-to-end (target under 10ms under normal load), ring throughput (target 1M msgs/sec aggregate across 64 CPUs), drop rate at 10x normal load (target 0% for non-DEBUG)
- [ ] Run benchmarks on all 4 platforms; record results in `docs/perf/klog-v2-bench.md`
- [ ] Migration acceptance criteria: v2 must beat v1 on cycles/call by at least 5x, equal or better on drop rate, equal or better on FATAL latency. If any criterion fails, fix v2 first
- [ ] Once accepted: remove v1 (`klog.c` becomes a thin shim over `klog_v2`). Keep `klog_disk.c` unchanged (already consumes structured format via §8)
- [ ] Update CLAUDE.md "Toolchain" / "Repository Layout" if file paths change. Update TODO-04 to mark §5 (rate limit), §9 (per-entry context) as superseded by TODO-32 with `[x]` and `superseded by TODO-32 §3,§5` annotation
- [ ] Commit: `"kernel: klog -- retire v1, v2 is now the primary implementation"`

**Test checkpoint:** Run full test suite with `KLOG_V2=1` -- all existing tests pass without modification (since `klog()` macro redirects). Benchmark report shows v2 hot-path under 200 cycles, 5x+ faster than v1. Drop rate under 10x load: 0% for FATAL/ERROR/WARN/INFO, bounded for DEBUG. Verify all 4 platforms.

## OS Comparison

| ⭐   | Feature                           | 🪟 Win11                     | 🐧 Linux                    | 🚀 Impossible OS               |
| --- | --------------------------------- | --------------------------- | -------------------------- | ----------------------------- |
| ⭐   | Lockless per-CPU enqueue          | ⚠️ ETW per-CPU, ring-lock   | ✅ printk 5.10+ lockless    | ⬜ §1, §2 (SPSC)               |
| ⭐   | Fail-proof FATAL guarantee        | ⚠️ ETW can drop hi-rate     | ⚠️ printk_emergency only   | ⬜ §4 (dedicated emergency)    |
| ⭐   | Priority lanes (per-level budget) | ❌ shared budget             | ❌ single ring              | ⬜ §3                          |
| 💎   | Native structured fields          | ✅ ETW binary                | ⚠️ journald binary only    | ⬜ §8 (binary + plain-text)    |
| 💎   | Plain-text + structured at once   | ❌ separate                  | ⚠️ printk vs journald      | ⬜ §8 (one source, two render) |
| ⭐   | NMI-safe enqueue                  | ⚠️ NMI uses dedicated buf   | ✅ NMI ring (since 5.10)    | ⬜ §1 (atomic SPSC)            |
| 💎   | Configurable per-tag rate limit   | ❌ global only               | ⚠️ rate_limit_burst        | ⬜ §5                          |
| ⭐   | Diagnostic-tag exemption (TEST)   | ❌                           | ❌                          | ⬜ §5                          |
| 💎   | Backpressure-aware serial drain   | ❌                           | ⚠️ printk synchronous      | ⬜ §6, §7                      |
| ⭐   | Boot-survival snapshot            | ⚠️ Event Log only post-boot | ⚠️ pstore / persistent ram | ⬜ §9 (reserved physmem)       |
| 💎   | HMAC-chained on-disk integrity    | ❌                           | ⚠️ journald FSS optional   | ✅ T04 §10 (consumed by v2)    |
| 💎   | Wire format versioned             | ✅ ETW manifests             | ⚠️ implicit                | ⬜ §8 (explicit version byte)  |

After §10 retires v1: Impossible OS is the only kernel combining lockless per-CPU + fail-proof FATAL + native structured + plain-text + HMAC-chained + boot-survival in a single in-kernel layer with zero external daemon dependency.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_klog_v2()` -- register in `src/kernel/test/test_runner.c`. New test category: `TEST_CAT_KLOG_V2` (add to `include/kernel/test/test.h` enum + `cat_names[]`/`cat_labels[]` in `test_runner.c` + `make test-klog-v2` target in `Makefile` + `bootx64.c` test_suite parser + `scripts/debug/kernel/run-klog-v2-tests.bat`).

- [ ] Create `src/kernel/test/test_klog_v2.c` with:
  - `test_klog_v2_ring_init` -- `klog_ring_v2_init` zeroes head/tail; `try_dequeue` returns -1 (empty)
  - `test_klog_v2_ring_fill_and_drain` -- enqueue to capacity, assert -1 on next try; drain all, assert -1 on next try
  - `test_klog_v2_ring_no_torn_writes` -- 4 kthreads on 4 CPUs each enqueue 10000 entries with checksum; drain; assert all checksums match (no torn writes)
  - `test_klog_v2_lanes_priority_drain` -- enqueue 100 DEBUG + 1 ERROR; drain 1; assert ERROR is what came out
  - `test_klog_v2_lanes_isolation` -- fill DEBUG lane to overflow; assert ERROR/WARN lanes still accept enqueues
  - `test_klog_v2_fatal_bypasses_ring` -- pause drain worker; fill ring; call `KLOG_FATAL`; assert FATAL appears in serial via emergency path
  - `test_klog_v2_fatal_nmi_safe` -- trigger NMI handler that calls KLOG_FATAL; assert no deadlock, message rendered
  - `test_klog_v2_rate_limit_default` -- spam 5000 "net" tagged msgs in 1 sec; assert exactly 100 enqueued
  - `test_klog_v2_rate_limit_test_exempt` -- spam 5000 "TEST" tagged msgs; assert all 5000 enqueued
  - `test_klog_v2_rate_limit_set` -- `klog_set_rate_limit("net", 1000)`; spam 5000; assert all 5000 enqueued
  - `test_klog_v2_drain_worker_running` -- after boot Phase 3, `klog_v2_get_stats().drain_worker_running == 1`
  - `test_klog_v2_drain_batch_size` -- enqueue 1000 entries; assert max drain batch size <= `KLOG_DRAIN_BATCH=32`
  - `test_klog_v2_backpressure_debug_drop` -- throttle serial; spam DEBUG; assert pressure counter non-zero, INFO+ still drained
  - `test_klog_v2_wire_format_decodes` -- enqueue entry; flush to disk; read binary; assert struct fields match
  - `test_klog_v2_json_lines_native` -- after 10 calls, `events.jsonl` lines parse as JSON with all expected fields (via `cJSON_Parse`)
- [ ] Note: `test_klog_v2_survival_recovery` requires reboot; covered by manual verification on QEMU WHPX, not by `make test-klog-v2`. Mark as `**Note:** survival recovery test runs during boot recovery path; verify via `X:\Logs\crash_recovery_v2.log` after panic+reboot.`
- [ ] Register in `test_runner_init()`: `test_register_klog_v2()`
- [ ] Commit: `"test: add klog v2 test suite (TEST_CAT_KLOG_V2)"`

## Verification

- [ ] `bash scripts/build.sh clean` with `KLOG_V2=0` -- `=== BUILD OK ===` (v1 unchanged)
- [ ] `bash scripts/build.sh clean` with `KLOG_V2=1` -- `=== BUILD OK ===` (v2 active)
- [ ] All v1 tests pass under `KLOG_V2=1` (the `klog()` macro redirect must be transparent)
- [ ] Boot under `KLOG_V2=1` shows `klog v2: ring init OK on cpu 0..N` for every CPU
- [ ] End-of-boot `klog_v2_get_stats()` reports zero drops on FATAL/ERROR/WARN, bounded drops on DEBUG (<5% under heavy test runs)
- [ ] `make test-klog-v2 SUITE=klog_v2 QUIET=0` runs all unit tests; `=== N tests passed, 0 failed ===`
- [ ] `scripts/debug/kernel/run-klog-v2-tests.bat` works on Windows QEMU
- [ ] Hot-path benchmark: under 200 cycles per `KLOG_INFO` call (measure via `rdtsc` delta in `test_klog_v2_perf.c`)
- [ ] Concurrent SMP enqueue: 4 kthreads on 4 CPUs each enqueue 10K entries; `perf` PMU reports zero false-sharing cache-line invalidations
- [ ] Backpressure: throttle serial to 9600 baud; spam 10000 messages; FATAL latency stays under 50ms (via emergency path), DEBUG drops gracefully
- [ ] Boot-survival: panic + reboot on QEMU WHPX; `X:\Logs\crash_recovery_v2.log` contains pre-panic markers
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal -- the per-CPU SPSC ring + atomic primitives behave identically across all platforms (no ordering surprises on real x86-64)

**Test runner:** `scripts\debug\kernel\run-klog-v2-tests.bat` (SUITE=klog_v2)

## History

| Date       | Action   | Summary |
|------------|----------|---------|
| 2026-04-14 | create   | TODO-32 created -- klog v2 architecture spec (10 sections: per-CPU SPSC rings, priority lanes, fail-proof FATAL, configurable rate limits, drain worker, backpressure, structured wire format, boot-survival, v1 retire). Reciprocal XREFs added in TODO-04 §5 (superseded note) and TODO-31 §1 (per_cpu_data ring field). New TEST_CAT_KLOG_V2 category proposed. Bat file `run-klog-v2-tests.bat` created. |
| 2026-04-14 | validate | Inputs anchors verified (`src/kernel/drivers/serial.c` corrected from initial `src/kernel/serial.c`). All 10 sections have flat `## N.` numbering, Commit line, Test checkpoint with all 4 platforms. OS Comparison populated with 12 rows; competitive position summary present. Unit Tests section wired with 15 concrete assertions + reboot-required survival test noted. Verification block has Test runner line. No N.M subnumbering, no model tags. Cross-TODO references use compact `T31 §1` / `T07 §4` notation. |
