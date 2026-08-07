---
schema_version: 1
id: disk-benchmark-diagnostics
domain: 05-storage-filesystems
status: active
title: "TODO-14 -- Disk Benchmark & I/O Diagnostics"
---

# TODO-14 -- Disk Benchmark & I/O Diagnostics

> **Goal:** Build the benchmark tool and diagnostic APIs that give Impossible OS native hardware-visibility: `diskbench` CLI (sequential/random/mixed with TSC-delta latency percentiles and queue-depth sweep), a real-time Disk Benchmark GUI, SMART extended diagnostics with health scoring, a generic `blkdev_stats_t` I/O metrics layer (generalizing VirtIO-only telemetry to all drivers), a per-device 64-bucket latency histogram, a VFS hot-path profiler, and a background storage health monitor daemon. Neither Windows nor Linux ships a built-in disk benchmark in their default install -- this is a first-class hardware-visibility exclusive.

> [!IMPORTANT]
> VirtIO already has a latency histogram engine in `src/kernel/drivers/virtio/blk_telemetry.c` (`latency_record()`, `latency_percentile()`, `virtio_blk_get_latency_stats()`). TSC frequency calibration exists in `src/kernel/boot_timing.c` (`s_tsc_freq`). `rdtsc_read()` is defined in `include/kernel/drivers/virtio/blk_internal.h`. §4 and §5 **generalize** these VirtIO-local primitives into a kernel-wide `blkdev_stats_t` and `latency_hist_t` API reused by AHCI, NVMe, and all other drivers. §1–§3 are the user-visible tools. §6–§7 are always-on background telemetry layers. Wire §4's `blkdev_stats` into `blkdev.h` before implementing §1.

## Inputs

- `src/kernel/drivers/virtio/blk_telemetry.c` + `include/kernel/drivers/virtio/blk_internal.h` -- `latency_record()`, `latency_percentile()`, `rdtsc_read()`, `tsc_per_us`; §4–§5 promote these to a generic API and the VirtIO driver becomes a first consumer
- `src/kernel/boot_timing.c` + `include/kernel/boot_timing.h` -- `s_tsc_freq` calibrated TSC frequency; expose as `tsc_get_freq_hz()` for use in §1 and §4
- `src/kernel/drivers/ahci/ahci_atapi.c` + `include/kernel/drivers/ahci.h` -- `atapi_dma_command()` used in §3 for ATA SMART READ DATA (`0xB0 0xD0`); AHCI NCQ tag occupancy from `src/kernel/drivers/ahci/ahci_ncq.c` consumed in §4
- `src/kernel/drivers/blkdev.c` + `include/kernel/drivers/blkdev.h` -- §4 adds `blkdev_stats_t` to `blkdev_t`; all read/write completion paths call `blkdev_stats_record()`
- → XREF: `05-storage-filesystems/TODO-01-block-storage-hardening.md §6` -- per-device I/O metrics `blkdev_t.stats` field defined there; §4 of this TODO extends that with histogram and queue-depth data
- Related (no stable XREF target): `02-kernel-core/TODO-xx-process-scheduler` -- SCHED_IDLE thread priority used by §7 health monitor daemon
- Related (no stable XREF target): `09-desktop-shell/TODO-xx-taskmanager` -- Task Manager "Storage" tab sparkline consumes §5 latency histogram data via `NtQuerySystemInformation(SystemDiskPerformanceInformation)`

## Outcome

- `diskbench` CLI: sequential + random + mixed throughput (MB/s), IOPS, and p50/p95/p99/max latency at QD=1/4/16/32.
- Disk Benchmark GUI: real-time bar chart, latency histogram, run history, export.
- Full SMART attribute table with color-coded health score (0–100%); registry export to `HKLM\HARDWARE\Disk\<id>\SMART\*`.
- Generic `blkdev_stats_t` + `blkdev_latency_hist_t` in `blkdev.h`; all drivers (VirtIO, AHCI, NVMe) populate it.
- Per-device I/O scheduler metrics: pending queue depth, AHCI NCQ tag occupancy.
- VFS hot-path profiler: per-filesystem per-op TSC timing averages; `/sys/fsperfstats` file.
- Storage health monitor: SCHED_IDLE daemon polls SMART every 60 s; desktop notification on health score < 80%.

## Implementation Order

| ⭐  | Order | Deliverable                                                                              | Depends On                                                              | Status |
| --- | :---: | ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §4 Generic `blkdev_stats_t` + histogram API -- promote VirtIO telemetry to all drivers   | Existing `blk_telemetry.c`; `blkdev.h` struct extension                 |  [ ]   |
| ⭐  |   2   | §5 Per-device latency histogram -- 64 buckets, `NtQuerySystemInformation` parity         | §4 (`blkdev_stats_t` must exist before histogram is exposed)            |  [ ]   |
| ⭐  |   3   | §1 `diskbench` CLI -- seq/rand/mixed throughput, IOPS, p50–max, QD sweep                 | §4 (raw blkdev I/O + TSC timing); `tsc_get_freq_hz()` from boot_timing  |  [ ]   |
| ⭐  |   4   | §3 SMART extended diagnostics -- ATA SMART READ DATA, health score, registry export      | Existing `atapi_dma_command()`; AHCI ATA passthrough for SMART          |  [ ]   |
| ⭐  |   5   | §6 VFS hot-path profiler -- `vfs_op_timer_t`, per-fs per-op averages, `/sys/fsperfstats` | §4 (`rdtsc_read()` generic); VFS `read`/`write`/`readdir` call sites    |  [ ]   |
| ⭐  |   6   | §7 Storage health monitor daemon -- SCHED_IDLE, SMART poll 60s, notification             | §3 (SMART attribute read); SCHED_IDLE thread; desktop notification API  |  [ ]   |
| ⭐  |   7   | §2 Disk Benchmark GUI -- bar chart, latency histogram, history, export                   | §3 CLI (benchmark engine reused); §4 (histogram data); GUI widget layer |  [ ]   |

> All sections are `⭐` exclusive -- Windows 11 has no built-in disk benchmark (requires `CrystalDiskMark`), no VFS hot-path profiler, no per-filesystem op-timing dashboard, and SMART is only accessible via WMI or `smartmontools`. Linux has `/proc/diskstats` and `hdparm` but no GUI benchmark or in-kernel VFS profiler in the default install. Impossible OS makes all of this first-class and built-in.

---

## 1. `diskbench` CLI `[Opus]`

Sequential read/write, random 4 KiB read/write, mixed 70/30, queue-depth sweep (QD=1/4/16/32). TSC-delta timing for latency percentiles. Bypass VFS -- raw `blkdev_read`/`blkdev_write` on a scratch LBA range.

**Files:** `src/shell/cmd_diskbench.c` (new), `include/shell/diskbench.h` (new)

> [!NOTE]
> This is `[Opus]` because the benchmark engine requires novel design: (1) correct TSC-delta timing with cache warm-up rounds to avoid cold-miss inflation; (2) latency array allocation and percentile computation without heap thrashing (pre-allocate 10 000-element array on the stack is too large -- use a 64-bucket histogram matching §4's `blkdev_latency_hist_t`); (3) queue-depth sweep requires submitting QD concurrent `blkdev_write` requests simultaneously (AHCI/NVMe multi-tag) and measuring completion time; (4) the scratch region must be validated to avoid overwriting live data -- require the user to specify a raw device index and LBA range explicitly; never auto-select. Warm-up: always run 8 MiB warm-up before recording. Timing: `rdtsc_read()` before submission; on completion callback: `rdtsc_read()` delta; convert via `tsc_get_freq_hz()`.

- [ ] `diskbench_run(dev, start_lba, lba_count, cfg, &result)`: takes `diskbench_cfg_t { test_flags, block_size_seq, block_size_rand, total_bytes_seq, total_ops_rand, queue_depth, warmup_bytes }` and populates `diskbench_result_t { seq_read_mbs, seq_write_mbs, rand_read_iops, rand_write_iops, mixed_iops, lat_avg_us, lat_p50_us, lat_p95_us, lat_p99_us, lat_max_us }`
- [ ] Sequential test: allocate 1 MiB aligned buffer; warm-up 8 MiB; issue 128 × 1 MiB reads sequentially, record total TSC delta; compute `bytes / time = MB/s`; same for writes
- [ ] Random test: pre-generate 10 000 random LBAs (within `[start_lba, start_lba+lba_count)`) using `rdrand`; issue 4 KiB reads at each; record per-op TSC delta into histogram; same for writes
- [ ] Mixed test: 70% reads + 30% writes interleaved; 10 000 ops; same histogram recording
- [ ] QD sweep: for QD = 1/4/16/32: submit `QD` concurrent requests; wait for all completions; record aggregate throughput and per-op latency; print QD column in result table
- [ ] Percentile extraction: `blkdev_latency_hist_percentile(&hist, 50)`, `(&hist, 95)`, `(&hist, 99)` from §4's histogram API
- [ ] `tsc_get_freq_hz()`: expose `s_tsc_freq` from `boot_timing.c` via a public API call; use to convert TSC ticks to microseconds
- [ ] CLI: `diskbench <device_idx> <start_lba> <lba_count> [/seq] [/rand] [/mixed] [/qd=N] [/warmup=<mb>]`; require `YES` confirmation before writes; output aligned result table; exit if `lba_count < 131072` (64 MiB minimum)
- [ ] Commit: `"shell: diskbench CLI -- seq/rand/mixed MB/s + IOPS, TSC-delta latency percentiles, QD sweep"`

## 2. Disk Benchmark GUI `[Sonnet]`

Drive selector, queue depth picker, Start button. Real-time bar chart updating after each sub-test. Latency histogram panel. Run history table. Export to timestamped text file.

**Files:** `src/desktop/dlg_diskbench.c` (new), `include/desktop/dlg_diskbench.h` (new)

> [!NOTE]
> GUI architecture: the benchmark runs on a background kernel thread; it sends `WM_BENCH_PROGRESS` messages to the GUI window after each sub-test completes. The main window processes messages and updates the bar chart. Bar chart: 8 bars (seq-read, seq-write, rand-read, rand-write, mixed at QD=1/4/16/32); filled left-to-right as each sub-test finishes; bar width = proportional to result vs. theoretical device max (clamped at 100%). Latency histogram: 64 log-scale buckets plotted as a vertical bar chart; x-axis = bucket midpoint (µs); y-axis = count. History: previous run results stored in `HKLM\SOFTWARE\ImpossibleOS\DiskBench\History\{drive}\{timestamp}`; comparison row below current run shows delta (green = better, red = worse).

- [ ] `dlg_diskbench_open(dev_idx)`: window with drive selector dropdown (populated from `blkdev_enumerate()`); QD picker (1/4/16/32 checkboxes); `Start` / `Stop` buttons; `Export` button
- [ ] Background benchmark thread: calls `diskbench_run()` for each enabled sub-test; posts `WM_BENCH_PROGRESS { test_id, result_so_far }` messages; main window handles by updating chart
- [ ] Bar chart widget: 8 bars; each bar drawn as a rounded rectangle filling proportionally; label = test name + MB/s or IOPS; gray = pending, blue = running (animated fill), green = complete
- [ ] Latency histogram panel: 64-bucket bar chart; x-axis buckets labeled `1µs 10µs 100µs 1ms 10ms 100ms 1s`; y-axis = log-scale count
- [ ] Run history: bottom panel; table columns = test, result, vs-prev (delta); read/write from registry on open; write on completion
- [ ] Export: `diskbench_export_txt(result, path)` writes a human-readable table to `C:\Temp\diskbench_YYYYMMDD_HHMMSS.txt`
- [ ] Commit: `"desktop: Disk Benchmark GUI -- real-time bar chart, latency histogram, run history, export"`

## 3. SMART Extended Diagnostics `[Sonnet]`

ATA SMART READ DATA and READ THRESHOLDS commands. Full 30-attribute table with color-coded failure detection. Disk health score (0–100%). Registry export to `HKLM\HARDWARE\Disk\<id>\SMART\*`.

**Files:** `src/kernel/drivers/ata_smart.c` (new), `include/kernel/drivers/ata_smart.h` (new)

> [!NOTE]
> ATA SMART commands via AHCI ATA passthrough (not ATAPI): issue an ATA command (not a SCSI CDB) using the AHCI H2D Register FIS. ATA SMART READ DATA: `Command=0xB0`, `Features=0xD0`, `LBA_Mid=0x4F`, `LBA_High=0xC2` → returns 512 bytes: 2-byte version, then 30 × `smart_attr { id(1), flags(2), value(1), worst(1), raw[6] }`. ATA SMART READ THRESHOLDS: `Features=0xD1` → returns 512 bytes with same layout but `threshold` field instead of flags. Failing attribute: `value ≤ threshold && id != 0`. Well-known attributes: `01` Reallocated Sector Count, `05` Reallocated Sectors, `07` Seek Error Rate, `0A` Spin Retry Count, `0C` Power Cycle Count, `C2` Temperature, `C5` Current Pending Sector, `C6` Uncorrectable Sector Count, `F1` Total LBAs Written, `F2` Total LBAs Read. Health score: start at 100; for each failing attribute: subtract 10; for `01 > 0` (reallocated sectors): subtract 20; for `C5 > 0` (pending sectors): subtract 15; clamp to 0. AHCI ATA passthrough: use AHCI port's H2D Register FIS (not a PACKET FIS) -- different from ATAPI.

- [ ] `ata_smart_read_data(ahci_port, &raw512)`: build H2D Register FIS for SMART READ DATA; issue via `ahci_issue_ata_cmd()`; verify return status
- [ ] `ata_smart_read_thresholds(ahci_port, &raw512)`: same for `Features=0xD1`
- [ ] `ata_smart_parse(data_buf, thresh_buf, &attrs[30])`: decode 30 `smart_attr_t { id, flags, value, worst, threshold, raw_u64, name[32], failing }` entries; `failing = (value <= threshold && id != 0)`
- [ ] `ata_smart_health_score(attrs, count)` → 0–100: apply per-attribute penalties; clamp to 0
- [ ] `ata_smart_to_registry(disk_idx, attrs, count)`: write `HKLM\HARDWARE\Disk\{disk_idx}\SMART\{attr_id_hex}` → `{value, worst, threshold, raw, failing}` per attribute; write `Health` → score
- [ ] Well-known attribute name table: `smart_attr_name(id)` → string (e.g., `"Reallocated Sector Count"`)
- [ ] `smart_get_temperature(attrs, count)` → Celsius from attribute `0xC2` raw bytes (byte 0 for most vendors; some use byte 2)
- [ ] CLI: `smart <drive>` → prints full attribute table with color codes; failing attrs shown in red; health score bar; exports to registry
- [ ] Commit: `"drivers: ATA SMART -- READ DATA + THRESHOLDS, attribute parse, health score, registry export"`

## 4. Generic `blkdev_stats_t` + Histogram API `[Opus]`

Generalize VirtIO's `blk_telemetry.c` primitives into a kernel-wide `blkdev_stats_t` and `blkdev_latency_hist_t`. Add `blkdev_stats_record()` to every driver's completion path. Expose AHCI NCQ tag occupancy.

**Files:** `src/kernel/drivers/blkdev.c` (extend), `include/kernel/drivers/blkdev.h` (extend), `src/kernel/drivers/blkdev_stats.c` (new), `include/kernel/drivers/blkdev_stats.h` (new)

> [!NOTE]
> This is `[Opus]` because generalizing per-driver telemetry requires careful concurrency design: each `blkdev_t` has its own `blkdev_stats_t` updated from interrupt/DPC context (completion callbacks); reads happen from task context for display. The histogram counters are 64-bit and must be updated atomically (use `atomic_fetch_add` or per-CPU accumulators with a merge step). Bucket boundaries must match the VirtIO `blk_telemetry.c` existing buckets (1 µs, 10 µs, 100 µs, 1 ms, 10 ms, 100 ms thresholds) so VirtIO can migrate without a behavioral change. AHCI NCQ: the count of active NCQ tags (`ahci_ncq_active_count(port)`) is the queue depth at submission time; record it into `stats.queue_depth_hist[tag_count]` (0–32 buckets).

- [ ] `blkdev_latency_hist_t { uint64_t buckets[64]; uint64_t total_ns; uint64_t count; uint64_t min_ns; uint64_t max_ns; }` -- 64 log-scale buckets; bucket `n` covers `[10^(n/10) µs, 10^((n+1)/10) µs)`; matches VirtIO existing boundary points
- [ ] `blkdev_stats_t { blkdev_latency_hist_t read_lat; blkdev_latency_hist_t write_lat; uint64_t bytes_read; uint64_t bytes_written; uint64_t read_ops; uint64_t write_ops; uint64_t errors; uint32_t queue_depth_hist[33]; uint32_t current_queue_depth; }` -- embedded in `blkdev_t`
- [ ] `blkdev_stats_record_read(dev, submit_tsc, bytes)`: called on every read completion; `delta_tsc = rdtsc_read() - submit_tsc`; convert to ns; find bucket; `atomic_fetch_add(&hist.buckets[b], 1)`; update `bytes_read`, `read_ops`
- [ ] `blkdev_stats_record_write(dev, submit_tsc, bytes)`: same for write path
- [ ] `blkdev_latency_hist_percentile(hist, percentile_x10)` → ns: walk buckets accumulating counts until reaching `percentile_x10 / 1000 * total`; return bucket midpoint -- reuse + generalize `latency_percentile()` from VirtIO
- [ ] Wire into completion paths: AHCI `ahci_rw.c` → `blkdev_stats_record_read/write()`; VirtIO `blk_io.c` → replace `latency_record()` call with `blkdev_stats_record_*()`; NVMe completion handler (when implemented)
- [ ] `ahci_ncq_active_count(port_idx)` → count of set bits in NCQ command issue register; called at submission time → record into `queue_depth_hist`
- [ ] `blkdev_stats_reset(dev)`: zero all stats fields
- [ ] `tsc_get_freq_hz()`: expose `s_tsc_freq` from `boot_timing.c` as a public function; update `blkdev_stats.h` to use it
- [ ] Commit: `"drivers: generic blkdev_stats_t -- 64-bucket histogram, atomic counters, AHCI NCQ occupancy, VirtIO migration"`

## 5. Per-Device Latency Histogram + `/sys/ioqueue` `[Sonnet]`

Expose per-device `blkdev_stats_t` via a `/sys/ioqueue` VFS virtual file. Implement `NtQuerySystemInformation(SystemDiskPerformanceInformation)` parity. Provide a `latency` shell command.

**Files:** `src/kernel/fs/sysfs.c` (new or extend), `src/shell/cmd_latency.c` (new)

> [!NOTE]
> `/sys/ioqueue`: a synthetic VFS directory (similar to Linux's `/proc/diskstats`); each file is named after the device (e.g., `/sys/ioqueue/sda`, `/sys/ioqueue/vda`); reading the file returns a fixed-width text table: `bytes_read bytes_written read_ops write_ops errors cur_qdepth read_lat_avg_us read_lat_p99_us write_lat_avg_us write_lat_p99_us`. VFS synthetic file: `sysfs_vfs_read(path, buf, offset, len)` enumerates `blkdev_t` list and formats stats on each read. `NtQuerySystemInformation(SystemDiskPerformanceInformation)`: returns a `DISK_PERFORMANCE` struct per logical drive: `BytesRead`, `BytesWritten`, `ReadTime`, `WriteTime`, `IdleTime`, `ReadCount`, `WriteCount`, `QueueDepth` -- map from `blkdev_stats_t`.

- [ ] `/sys/` VFS mount: `sysfs_init()` registers a synthetic filesystem at the VFS path `/sys/`; `sysfs_add_file(path, read_fn)` registers a read callback
- [ ] `/sys/ioqueue/{dev}` files: for each `blkdev_t` registered: call `sysfs_add_file("ioqueue/{dev_name}", sysfs_ioqueue_read_cb)`; callback formats stats as fixed-width text
- [ ] `NtQuerySystemInformation(SystemDiskPerformanceInformation)`: allocate array of `DISK_PERFORMANCE` (one per volume letter); populate from `blkdev_stats_t` of the underlying device; return via system call
- [ ] `latency <device>` CLI command: reads `/sys/ioqueue/{device}`; formats as a latency histogram ASCII bar chart (64 buckets, each row = one bucket); also prints summary stats (avg, p50, p95, p99, max)
- [ ] Task Manager sparkline hook: export `blkdev_stats_get_recent_throughput(dev, &read_mbs, &write_mbs)` computing a 1-second sliding average from the running byte counters; Task Manager "Storage" tab calls this every 1 s
- [ ] Commit: `"kernel: /sys/ioqueue + NtQuerySystemInformation disk perf + latency CLI command"`

## 6. VFS Hot-Path Profiler `[Sonnet]`

Wrap `vfs_read`, `vfs_write`, and `vfs_readdir` with TSC-delta measurement. Per-filesystem per-op running average. Expose via `/sys/fsperfstats`. Highlight slowest driver.

**Files:** `src/kernel/fs/vfs.c` (extend), `src/kernel/fs/vfs_perf.c` (new), `include/kernel/fs/vfs_perf.h` (new)

> [!NOTE]
> Profiling overhead must be minimal. Strategy: only measure when `vfs_perf_enabled` global is 1 (default off; enabled via `sysctl vfs_perf 1` or registry key `HKLM\SYSTEM\VfsPerfEnabled`). Measurement: `uint64_t t0 = rdtsc_read()` before the VFS dispatch; `uint64_t t1 = rdtsc_read()` after; accumulate into per-filesystem, per-op exponential moving average (EMA): `ema = (ema * 15 + delta) / 16` -- no allocation, no lock (accept ~1/16 chance of a torn update on a 64-bit counter, harmless for diagnostics). Per-filesystem tracking: up to 16 registered filesystems (FAT32, NTFS, IXFS, ext4, exFAT, Btrfs, etc.); each has a `vfs_fs_perf_t { uint64_t read_ema_ns; uint64_t write_ema_ns; uint64_t readdir_ema_ns; uint64_t read_count; uint64_t write_count; }`.

- [ ] `vfs_fs_perf_t` struct + `vfs_perf_table[16]` keyed by `vfs_fs_driver*` pointer
- [ ] `vfs_perf_record_op(driver, op_type, delta_ns)`: update the appropriate EMA field; increment count; mark `slowest_driver` if `delta_ns > current_max_ema_across_all_drivers`
- [ ] Wrap `vfs_read()`: if `vfs_perf_enabled`: `t0 = rdtsc_read()`; call driver; `vfs_perf_record_op(driver, VFS_OP_READ, rdtsc_read()-t0)`; same for `vfs_write()` and `vfs_readdir()`
- [ ] `/sys/fsperfstats` file: `sysfs_add_file("fsperfstats", sysfs_fsperfstats_read_cb)`; callback prints a table: `fs_name read_avg_us write_avg_us readdir_avg_us read_count write_count slowest_op`; highlight slowest row with `***`
- [ ] `vfs_perf_reset()`: zero all EMA fields and counts; callable from shell: `sysctl vfs_perf reset`
- [ ] `sysctl vfs_perf [0|1|reset]` shell command: read/write `vfs_perf_enabled`; print current stats; reset
- [ ] Commit: `"kernel: VFS hot-path profiler -- per-fs per-op EMA, /sys/fsperfstats, sysctl vfs_perf"`

## 7. Storage Health Monitor Daemon `[Sonnet]`

SCHED_IDLE background thread. Polls SMART every 60 seconds. Compares attribute deltas. Logs `[WARN] Disk health degraded` to serial + event log. Posts desktop notification when health score drops below 80%.

**Files:** `src/kernel/drivers/storage_health.c` (new), `include/kernel/drivers/storage_health.h` (new)

> [!NOTE]
> The daemon is a single SCHED_IDLE kernel thread created at boot by `storage_health_init()`. It wakes every 60 seconds via `ksleep(60000)`. On each wake: for each AHCI disk present: call `ata_smart_read_data()` + `ata_smart_parse()` + `ata_smart_health_score()`; compare to previous snapshot stored in `health_state[disk_idx]`; if any attribute's `raw_u64` increased (degrading): log to serial and event log; if health score dropped below 80%: post a desktop notification. The thread must never block on I/O for more than 5 seconds -- use a timeout on the SMART command. Temperature logging: if temperature crosses 55°C → log `[WARN]`; crosses 65°C → log `[CRIT]`.

- [ ] `health_state_t { uint8_t prev_health_score; smart_attr_t prev_attrs[30]; uint8_t prev_temp_c; uint8_t notified_below_80; }` -- one per disk
- [ ] `storage_health_daemon()` thread function: `while (1) { ksleep(60000); for each disk: storage_health_check(disk_idx); }`
- [ ] `storage_health_check(disk_idx)`:
  - `ata_smart_read_data()` with 5 s timeout; if timeout: log `[WARN] SMART timeout on disk %d`; skip
  - `ata_smart_parse()` → attrs; `ata_smart_health_score()` → score
  - For each attr: if `raw_u64 > prev.raw_u64 && id in {01, 05, 0A, C5, C6}`: log `[WARN] Disk %d: SMART attr %02X increased (was %llu, now %llu)`
  - If score < 80 and `!notified_below_80`: post desktop notification `"Disk health warning: %s (%d%%)"`; `notified_below_80 = 1`; log to event log
  - If score >= 80: clear `notified_below_80`
  - Temperature: compare `prev_temp_c`; log on threshold crossings
  - Update `health_state[disk_idx]`
- [ ] `storage_health_init()`: create SCHED_IDLE thread; initialize `health_state[]` with first SMART read; register shutdown handler
- [ ] `storage_health_get_score(disk_idx)` → current cached health score (0–100); used by §3 GUI and Task Manager
- [ ] Event log: write to `HKLM\SYSTEM\EventLog\Storage\*` registry hive with timestamp, severity, message
- [ ] Commit: `"kernel: storage health monitor -- SCHED_IDLE daemon, SMART poll 60s, attr delta, health notification"`

---

## OS Comparison


| ⭐  | Feature                                                                              | 🪟 Win11                                                                                   | 🐧 Linux                                                 | 🚀 Impossible OS                                                    |
| --- | ------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------ | -------------------------------------------------------- | ------------------------------------------------------------------- |
| ⭐  | Built-in disk benchmark                                                              | ❌ No built-in benchmark; requires `CrystalDiskMark`                                       | ❌ `hdparm -tT` (read only, no                           | ⬜ §1 -- TSC-delta latency percentiles; QD=1/4/16/32 sweep          |
| ⭐  | Disk Benchmark GUI                                                                   | ❌ No built-in GUI benchmark                                                               | ❌ No built-in GUI; `gnome-disks` has                    | ⬜ §2 -- animated fill bars; 64-bucket histogram                    |
| 💎  | SMART full attribute table                                                           | ✅ `smartctl` via WMI; `CrystalDiskInfo` (third-party);                                    | ✅ `smartmontools` (`smartctl`, `smartd`); no built-in   | ⬜ §3 -- in-kernel ATA SMART READ DATA                              |
| ⭐  | Generic `blkdev_stats_t`                                                             | ✅ `StorPort` miniport performance counters; per-driver,                                   | ✅ `/proc/diskstats`; per-device counters; no per-driver | ⬜ §4 -- kernel-wide unified API; AHCI NCQ                          |
| ⭐  | `/sys/ioqueue` + `NtQuerySystemInformation(SystemDiskPerformanceInformation)` parity | ✅ `NtQuerySystemInformation(SystemDiskPerformanceInformation)` returns `DISK_PERFORMANCE` | ✅ `/proc/diskstats`; no latency percentiles             | ⬜ §5 -- `/sys/ioqueue` + `latency` CLI; `NtQuerySystemInformation` |
| ⭐  | VFS hot-path profiler                                                                | ❌ No VFS profiler; ETW traces                                                             | ❌ `blktrace` + `perf` are available                     | ⬜ §6 -- zero-overhead when disabled; per-fs EMA                    |
| ⭐  | Storage health monitor                                                               | ✅ `smartd` equivalent via `Windows Health                                                 | ✅ `smartd` (third-party daemon); no built-in            | ⬜ §7 -- in-kernel SCHED_IDLE thread; attr-delta detection          |

> **After §1–§7:** Impossible OS becomes the only OS with a built-in disk benchmark GUI, in-kernel per-filesystem VFS profiler, and a unified `blkdev_stats_t` API covering all storage drivers from one place. The `diskbench` CLI with TSC-delta latency percentiles and queue-depth sweep exceeds what `CrystalDiskMark` delivers on Windows -- and it ships in the default OS install without any third-party tools.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `blkdev_stats_record_read(dev, t0, 4096)` called from AHCI completion path; `/sys/ioqueue/sda` shows non-zero `read_ops` and `read_lat_avg_us` after a file read
- [ ] `diskbench 0 2048 131072 /seq /rand /qd=4`: sequential read > 100 MB/s on VirtIO blk (QEMU); random read IOPS > 1000; p99 latency printed; no data overwritten outside the specified LBA range
- [ ] QD sweep: `diskbench 0 2048 131072 /qd=1 /qd=16 /qd=32` shows increasing throughput with higher queue depth on AHCI QEMU disk (NCQ enabled)
- [ ] Disk Benchmark GUI: opens; Start button triggers background thread; bar chart fills in real-time; latency histogram shows distribution; history panel populated after second run
- [ ] SMART: `smart C:` on QEMU VirtIO disk → prints attribute table; all values > threshold → green; health score = 100%; `HKLM\HARDWARE\Disk\0\SMART\01` registry key populated
- [ ] SMART failing: inject QEMU SMART attribute `0x01` value ≤ threshold → `smart C:` shows that row in red; health score reduced; `HKLM\HARDWARE\Disk\0\SMART\Health` = reduced score
- [ ] `/sys/ioqueue/vda`: `ReadFile("/sys/ioqueue/vda", ...)` returns formatted stats line with non-zero bytes_read after filesystem operations
- [ ] `latency vda`: prints ASCII bar chart of 64 histogram buckets; most ops in 1–100 µs range for VirtIO; p99 < 1 ms under light load
- [ ] `NtQuerySystemInformation(SystemDiskPerformanceInformation)`: returns `DISK_PERFORMANCE` struct with `BytesRead` matching `blkdev_stats.bytes_read`
- [ ] VFS profiler: `sysctl vfs_perf 1`; read 1000 files; `cat /sys/fsperfstats` → IXFS or FAT32 row shows non-zero `read_avg_us` and `read_count`; `sysctl vfs_perf reset` → counts zero
- [ ] Health monitor: start daemon; inject SMART attr `0xC5` (pending sectors) raw=1 in test hook; 60 s later: serial log shows `[WARN] Disk 0: SMART attr C5 increased`
- [ ] Health notification: inject health score drop to 75% in test hook; daemon posts desktop notification "Disk health warning: sda (75%)"
- [ ] Commit: `"storage: complete disk benchmark + diagnostics suite -- diskbench, SMART, blkdev_stats, latency histogram, VFS profiler, health daemon"`
