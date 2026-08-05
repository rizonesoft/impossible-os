---
schema_version: 1
id: block-storage-hardening
domain: 05-storage-filesystems
status: active
title: "TODO-01 -- Block Storage Hardening"
---

# TODO-01 -- Block Storage Hardening

> **Goal:** Harden the working VirtIO-blk and AHCI drivers to production quality: add flush/write-back, error recovery, NCQ, SMART, per-device I/O metrics, and a block-level LRU sector cache -- providing the reliable, measurable foundation that all filesystem drivers depend on.

> [!IMPORTANT]
> §1–2 extend `src/kernel/drivers/virtio/blk.c`; §3–5 extend `src/kernel/drivers/ahci/ahci.c` (or the AHCI driver directory). §6–7 extend `src/kernel/main/blkdev.c` (the `blkdev_t` layer all drivers register with). All error paths must propagate `EIO` through `blkdev_t` to callers; no silent data loss is acceptable. §7 (disk cache) must call `cache_invalidate(dev)` at unmount time -- verify the VFS unmount path calls this before merge.

## Inputs

- `src/kernel/drivers/virtio/blk.c` -- VirtIO-blk driver; §1 (flush) and §2 (error recovery) extend it directly
- `src/kernel/drivers/ahci/` -- AHCI driver; §3 (NCQ), §4 (error recovery), §5 (SMART) extend it
- `src/kernel/main/blkdev.c` + `include/kernel/main/blkdev.h` -- `blkdev_t` abstraction; §6 (metrics) and §7 (cache) live here
- `src/kernel/main/blkdev_adapters.c` -- adapters that register physical drivers with `blkdev`; cross-check `blkdev_register()` signature before §6
- → XREF: `05-storage-filesystems` VFS TODO (future) -- the disk cache (§7) is the write-back layer VFS drivers call; verify `cache_flush()` is called at shutdown before those TODOs begin
- → XREF: `10-apps` Task Manager TODO -- §6 I/O metrics hook the Task Manager performance tab and `iostat` shell command

## Outcome

- `VIRTIO_BLK_T_FLUSH` issued correctly; `fsync()` syscall wakes VFS callers after flush confirm.
- VirtIO-blk errors logged, retried ×3, surfaced as `EIO` to VFS on persistent failure.
- AHCI NCQ active on NCQ-capable drives: up to 32 in-flight commands via `SATA_ACTIVE` bitmask.
- AHCI error recovery: `PxIS.TFES` handled, COMRESET on persistent failure, `EIO` propagated.
- `blkdev_smart_query(dev)` returns temperature, reallocated sector count, remaining life.
- `blkdev_stats(dev)` returns per-device counters; `iostat` shell command shows live read/write rates.
- LRU sector cache (2–8 MiB configurable): write-back with 5 s flush; `cache_invalidate()` at unmount.

## Implementation Order

| ⭐  | Order | Deliverable                                                                         | Depends On                                        | Status |
| --- | :---: | ----------------------------------------------------------------------------------- | ------------------------------------------------- | :----: |
| 💎  |   1   | §1 VirtIO-blk flush & write-back -- `VIRTIO_BLK_T_FLUSH`, write-back queue, `fsync` | `virtio/blk.c` working I/O path                  |  [ ]   |
| 💎  |   2   | §2 VirtIO-blk error recovery -- status parse, 3× retry, `EIO` surface               | §1 (flush before retry)                           |  [ ]   |
| 💎  |   3   | §3 AHCI NCQ -- `CAP.SNCQ` detect, `READ/WRITE_FPDMA_QUEUED`, 32-deep `SATA_ACTIVE` | AHCI DMA R/W + IRQ working (from old §1–4)        |  [ ]   |
| 💎  |   4   | §4 AHCI error recovery -- `PxIS.TFES`, COMRESET, retry, `EIO` surface               | §3 (NCQ in-flight tracking)                       |  [ ]   |
| 💎  |   5   | §5 AHCI SMART read -- `0xB0/0xD0`, attribute table, `blkdev_smart_query()`           | §4 (reliable ATA command path)                    |  [ ]   |
| ⭐  |   6   | §6 Block device I/O metrics -- per-device counters, `blkdev_stats()`, `iostat`       | `blkdev.c` dispatch path (all drivers registered) |  [ ]   |
| ⭐  |   7   | §7 Block-level disk cache -- LRU sector cache, write-back 5 s, `cache_invalidate()`  | §1 + §2 (flush/error path used by cache eviction)  |  [ ]   |
| ⭐  |   8   | §8 Block-I/O QoS -- per-owner IOPS/bandwidth caps, period refill, backpressure       | §6 (dispatch counters), D02 T25 §7 (rate record)  |  [ ]   |

> §6 (I/O metrics) and §7 (block-level LRU cache) are `⭐` exclusive: Windows exposes I/O counters only through PDH/ETW; Linux exposes them only through procfs. Impossible OS embeds live counters directly in `blkdev_t` and exposes them through both a kernel API and the Task Manager, eliminating the indirection of a separate monitoring daemon. The in-kernel LRU sector cache is a single write-back layer shared by all filesystem drivers -- neither Windows nor Linux unifies this at the `blkdev` level without a more complex page cache or request queue abstraction.

---

## 1. VirtIO-blk Flush & Write-back `[Sonnet]`

Add `VIRTIO_BLK_T_FLUSH` support and a write-back queue so dirty sectors are periodically committed and `fsync()` can block until the drive confirms completion.

**Files:** `src/kernel/drivers/virtio/blk.c` (extend), `include/kernel/drivers/virtio/blk.h` (extend)

> [!NOTE]
> `VIRTIO_BLK_T_FLUSH = 4`. The flush request has no sector field; only the type and status descriptor are needed. The status byte in the completion descriptor must be `VIRTIO_BLK_S_OK (0)`. An `fsync()` syscall must block the calling thread until the flush completion callback fires; use the existing semaphore/wait-queue primitive in the kernel.

- [ ] `virtio_blk_flush(dev)`: build a flush virtqueue descriptor (type=`VIRTIO_BLK_T_FLUSH`, sector=0, no data buffer); enqueue to virtqueue; ring doorbell; wait for completion; check `status == VIRTIO_BLK_S_OK`; return 0 or `-EIO`
- [ ] Write-back queue: add `dirty_queue[]` ring buffer (512 sector entries) to `virtio_blk_dev_t`; `virtio_blk_write()` enqueues sector on `dirty_queue` after dispatch; timer callback fires every 5 s → calls `virtio_blk_flush(dev)` → clears dirty_queue
- [ ] `fsync()` syscall handler: look up `blkdev` from fd's filesystem; call `blkdev->flush(dev)`; which calls `virtio_blk_flush()`; blocks until completion; return 0 to caller
- [ ] Log: `[VirtIO-BLK] Flush OK / Flush failed EIO`
- [ ] Commit: `"drivers: virtio-blk flush -- VIRTIO_BLK_T_FLUSH, write-back queue, fsync()"`

## 2. VirtIO-blk Error Recovery `[Sonnet]`

Parse the `status` byte in the completion descriptor. Retry transient errors up to 3 times before surfacing `EIO` to the VFS caller.

**Files:** `src/kernel/drivers/virtio/blk.c` (extend)

> [!NOTE]
> VirtIO-blk status values: `VIRTIO_BLK_S_OK (0)` = success; `VIRTIO_BLK_S_IOERR (1)` = device error; `VIRTIO_BLK_S_UNSUPP (2)` = unsupported request. `IOERR` is transient-retryable; `UNSUPP` is permanent. Track retry count in the in-flight request struct.

- [ ] Add `uint8_t retry_count` to `virtio_blk_req_t`; on completion: if `status == VIRTIO_BLK_S_IOERR && retry_count < 3`: increment, re-enqueue to virtqueue; else if `status != 0` after retries: log `[VirtIO-BLK] I/O error LBA %llu after %u retries`; call `blkdev_io_error(dev)` and wake caller with `-EIO`
- [ ] `VIRTIO_BLK_S_UNSUPP`: log `[VirtIO-BLK] Unsupported request type 0x%x`; immediately return `-ENOTSUP` without retry
- [ ] Error counters: increment `dev->stats.errors` in `blkdev_t` on any non-OK completion (feeds §6)
- [ ] Commit: `"drivers: virtio-blk error recovery -- status parse, 3× retry, EIO surface, error counters"`

## 3. AHCI Native Command Queuing (NCQ) `[Opus]`

Detect NCQ capability via `CAP.SNCQ`. Issue `READ_FPDMA_QUEUED (0x60)` / `WRITE_FPDMA_QUEUED (0x61)` commands. Track up to 32 in-flight commands per port via `SATA_ACTIVE` bitmask. Handle `SDB` FIS completion.

**Files:** `src/kernel/drivers/ahci/ahci.c` (extend), `include/kernel/drivers/ahci/ahci.h` (extend)

> [!NOTE]
> NCQ command tag allocation: `PxSACT` bitmask -- bit N set means tag N in-flight. Issue: write `PxSACT |= (1 << tag)` before writing `PxCI |= (1 << tag)`. FIS type `0x27 (Register H2D)`: `command=0x60/0x61`, `sector_count_low=1`, `feature_low=tag<<3`, `sector_count_high=0`. NCQ completion: SDB FIS (type `0xA1`) arrives on interrupt; `PxSACT` bits cleared by HBA. Read `PxSACT` to determine which tags completed.

- [ ] Detect NCQ: at port init, check `HBA_CAP.SNCQ (bit 30)` and drive's `IDENTIFY` word 76 bit 8 (`NCQ supported`); set `port->ncq_depth` = min(HBA queue depth, drive queue depth, 32)
- [ ] Tag allocator: `uint32_t port->ncq_active` bitmask; `ahci_alloc_tag(port)` → `__builtin_ctz(~port->ncq_active)` (first free bit); `ahci_free_tag(port, tag)` → `port->ncq_active &= ~(1 << tag)`
- [ ] `ahci_ncq_read(port, lba, count, buf)` / `ahci_ncq_write(port, lba, count, buf)`: allocate tag; build command FIS with `0x60`/`0x61`; set `PxSACT |= (1 << tag)`; set `PxCI |= (1 << tag)`; store callback in `port->pending[tag]`
- [ ] NCQ completion: IRQ handler -- if `PxIS.SDBS` set: read `PxSACT`; for each bit 0 in `~PxSACT & port->ncq_active`: call `port->pending[tag].callback`; `ahci_free_tag(port, tag)`
- [ ] Non-NCQ fallback: if `ncq_depth == 0`, fall through to existing DMA R/W path unchanged
- [ ] Guard NCQ tag state vs AHCI ISR: `ncq_sync_rw` timeout path in `ahci_ncq.c` races the `ahci_irq.c` SDB completion path on `tags_pending`/`tag_status` -- IRQ-save per-port lock or atomic bitops, reconcile HW completion before tag free
- [ ] Log: `[AHCI] Port %u NCQ enabled, depth=%u`
- [ ] Commit: `"drivers: AHCI NCQ -- CAP.SNCQ detect, READ/WRITE_FPDMA_QUEUED, 32-deep tag allocator, SDB FIS"`

## 4. AHCI Error Recovery `[Opus]`

Handle `PxIS.TFES` (Task File Error Status). Issue COMRESET on persistent failure. Retry the current command. Surface `EIO` to the VFS layer on unrecoverable errors.

**Files:** `src/kernel/drivers/ahci/ahci.c` (extend)

> [!NOTE]
> `PxIS.TFES (bit 30)` fires on a device error FIS (`D2H Register FIS` with `ERR` bit set) or command list overflow. Recovery sequence: (1) clear `PxCMD.ST`; (2) wait `PxCMD.CR == 0`; (3) clear `PxSERR`; (4) set `PxCMD.ST` to restart. If `PxTFD.STS.ERR` remains after 3 attempts, issue COMRESET: clear `PxCMD.ST`, set `PxSCTL.DET=1` for 1 ms, clear `PxSCTL.DET=0`, wait for `PxSSTS.DET==3`.

- [ ] IRQ handler: if `PxIS & (1 << 30)` (TFES): read `PxTFD.STS` (error register); increment `port->error_count`; if `error_count < 3`: clear `PxCMD.ST`, wait `CR==0`, clear `PxSERR`, set `PxCMD.ST`, re-issue command; else COMRESET sequence
- [ ] COMRESET: `ahci_port_comreset(port)`: `PxCMD.ST=0`; spin until `CR==0`; `PxSCTL.DET=1`; `timer_spin_us(1000)`; `PxSCTL.DET=0`; spin until `PxSSTS.DET==3`; set `PxCMD.ST=1`; reset `error_count`
- [ ] Persistent failure: after COMRESET, if device still returns error: call `blkdev_io_error(dev)`, wake pending callers with `-EIO`, log `[AHCI] Port %u unrecoverable error; drive offline`
- [ ] NCQ-aware recovery: before COMRESET, abort all in-flight NCQ tags: for each set bit in `port->ncq_active`: call pending callback with `-EIO`; clear bitmask
- [ ] Commit: `"drivers: AHCI error recovery -- PxIS.TFES, 3× retry, COMRESET, EIO surface, NCQ abort"`

## 5. AHCI SMART Read `[Sonnet]`

Issue ATA SMART READ DATA (`0xB0/0xD0`) via an AHCI non-data command. Parse the 512-byte SMART attribute table. Expose `blkdev_smart_query()` returning temperature, reallocated sector count, and remaining life.

**Files:** `src/kernel/drivers/ahci/ahci.c` (extend), `include/kernel/main/blkdev.h` (extend)

> [!NOTE]
> ATA SMART READ DATA: command `0xB0`, feature `0xD0`, LBA mid `0x4F`, LBA high `0xC2`. Response is a 512-byte DMA transfer. Layout: 12-byte header + 30 × 12-byte attribute entries (attr_id, flags[2], value, worst, raw[6], reserved). Key attribute IDs: `0x01` Raw Read Error Rate, `0x05` Reallocated Sectors Count (`raw[0]` = count), `0xC2` Temperature (`raw[0]` = °C), `0xBB` Uncorrectable Errors.

- [ ] `ahci_smart_read(port, buf[512])`: issue ATA non-data PIO command with `0xB0/0xD0`; wait completion; if error return `-EIO`; copy 512 bytes to `buf`
- [ ] `blkdev_smart_t { uint8_t temperature_c; uint32_t reallocated_sectors; uint8_t remaining_life_pct; }` in `blkdev.h`
- [ ] `blkdev_smart_query(dev, blkdev_smart_t *out)`: calls `dev->smart_read(dev, buf)`; scan attribute entries for IDs `0xC2` (temp), `0x05` (realloc), `0xBB` (life proxy); fill `out`; return 0 or `-EIO` or `-ENOTSUP`
- [ ] Log at boot: `[AHCI] Port %u SMART: temp=%u°C realloc=%u life=%u%%`
- [ ] Commit: `"drivers: AHCI SMART -- ATA 0xB0/0xD0, attribute parse, blkdev_smart_query API"`

## 6. Block Device I/O Metrics `[Sonnet]`

Add per-device atomic counters to `blkdev_t`. Expose `blkdev_stats()`. Add `iostat` shell command. Hook the Task Manager performance tab.

**Files:** `src/kernel/main/blkdev.c` + `include/kernel/main/blkdev.h` (extend), `src/shell/cmd_iostat.c` (new)

> [!NOTE]
> Counters must be updated in the `blkdev` dispatch path (not inside each driver) so all registered drivers (AHCI, VirtIO, future NVMe) get metrics for free. Use `__atomic_fetch_add(..., __ATOMIC_RELAXED)` -- relaxed ordering is sufficient for statistics; correctness does not depend on ordering relative to other memory operations.

- [ ] Add to `blkdev_t`: `uint64_t bytes_read, bytes_written; uint32_t read_ops, write_ops, errors; uint64_t last_read_ns, last_write_ns;` -- initialise to 0 in `blkdev_register()`
- [ ] In `blkdev_read()` dispatch: `__atomic_fetch_add(&dev->bytes_read, len, __ATOMIC_RELAXED)`, `__atomic_fetch_add(&dev->read_ops, 1, ...)`, record `last_read_ns = timer_ns()`
- [ ] In `blkdev_write()` dispatch: same for `bytes_written` / `write_ops`
- [ ] `blkdev_stats(dev, blkdev_stats_t *out)`: snapshot all counters into `out`; also call `blkdev_smart_query()` if `dev->smart_read != NULL`
- [ ] `blkdev_stats_t { uint64_t bytes_read, bytes_written; uint32_t read_ops, write_ops, errors; uint8_t temperature_c; uint32_t reallocated_sectors; }`
- [ ] `iostat` shell command: iterate registered `blkdev` list; call `blkdev_stats()`; print: `  Device   Reads/s  Writes/s  Read MB/s  Write MB/s  Errors  Temp`; compute rates from delta between two 1 s samples using `timer_ns()`
- [ ] Task Manager hook: `blkdev_stats()` called by the Task Manager performance tab's disk section (coordinate with `08-desktop-shell` Task Manager TODO)
- [ ] Commit: `"kernel: blkdev I/O metrics -- atomic counters, blkdev_stats API, iostat shell command"`

## 7. Block-Level Disk Cache `[Opus]`

Implement an LRU sector cache (2–8 MiB configurable) in `blkdev.c` as a write-back layer shared by all filesystem drivers. Flush every 5 s. `cache_flush()` on shutdown. `cache_invalidate(dev)` on unmount.

**Files:** `src/kernel/main/blkdev.c` (extend), `include/kernel/main/blkdev.h` (extend)

> [!NOTE]
> Cache entry granularity: one entry per 512-byte sector. LRU implemented as a doubly linked list + hash table (sector → `cache_entry_t*`). Write-back: dirty entries are flushed to the block device without evicting from cache. Eviction: when cache is full, evict the LRU clean entry; if no clean entries, flush the LRU dirty entry first, then evict. Cache size: default 4096 entries (2 MiB); configurable via `blkdev_cache_set_size(n_sectors)` at init. Write-through mode: when `BLKDEV_FLAG_WRITE_THROUGH` is set on `blkdev_t`, skip the dirty queue and write directly, but still populate the read cache for future reads.

- [ ] `cache_entry_t { uint64_t lba; blkdev_t *dev; uint8_t data[512]; bool dirty; cache_entry_t *lru_prev, *lru_next; }` -- allocated from a fixed pool via `pmm_alloc_contiguous()` at init
- [ ] Hash table: `cache_entry_t *cache_ht[CACHE_HT_SIZE]` keyed by `(dev_id << 32 | lba) % CACHE_HT_SIZE`; resolve collisions with chaining via `cache_entry_t.hash_next`
- [ ] `blkdev_cache_read(dev, lba, buf)`: lookup hash table; hit → copy 512 bytes from `entry->data` to `buf`; promote to MRU; return 0. Miss → `blkdev_raw_read(dev, lba, buf)`; insert new entry; if full, evict LRU
- [ ] `blkdev_cache_write(dev, lba, buf)`: lookup or insert entry; copy `buf` to `entry->data`; set `dirty=true`; promote to MRU; if write-through: also `blkdev_raw_write(dev, lba, entry->data)` immediately
- [ ] Write-back flush timer: fires every 5 s; iterates dirty list; for each dirty entry in LRU order: `blkdev_raw_write(dev, lba, data)`; if OK: `dirty=false`; if error: log, increment `dev->stats.errors`
- [ ] `cache_flush(dev)`: synchronously flush all dirty entries for `dev`; called at shutdown and by `fsync()` after driver-level flush
- [ ] `cache_invalidate(dev)`: remove all entries for `dev` from hash table and LRU list; called at unmount to prevent stale reads after device removal
- [ ] Replace `blkdev_read()` / `blkdev_write()` wrappers with `blkdev_cache_read()` / `blkdev_cache_write()`; `blkdev_raw_read/write` bypass the cache (used internally and for SMART, NCQ flush)
- [ ] Boot log: `[BLK-CACHE] %u sectors (%u MiB) LRU cache initialised`
- [ ] Commit: `"kernel: blkdev LRU sector cache -- write-back, 5s flush, cache_flush/invalidate, write-through mode"`

---

## 8. Block-I/O QoS (IOPS / Bandwidth Caps) `[Opus]`

Enforce the I/O rate-limit records published by the quota subsystem at the `blkdev` dispatch layer: per-owner IOPS and bytes-per-second caps with a per-period refill, applied to read, write, and control traffic independently. §6 MEASURES per-device traffic; this section THROTTLES it per owner.

**Files:** `src/kernel/main/blkdev.c` + `include/kernel/main/blkdev.h` (extend)

> [!NOTE]
> Enforcement belongs in the `blkdev` dispatch path for the same reason the §6 counters do: every registered driver inherits it without per-driver code. The policy record is defined and stored by the quota subsystem; nothing here invents its own limit shape.

- [ ] Consume `quota_rate_limit_get()` for the IO_READ / IO_WRITE / IO_CONTROL classes. -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7` (item: "Define a rate-limit record")
- [ ] Honor both `QUOTA_RATE_UNIT_OPS` (IOPS) and `QUOTA_RATE_UNIT_BYTES` (bandwidth) caps over `period_ns`, refilled at period rollover.
- [ ] Throttle by delaying submission (queue the request), never by failing the I/O: a rate cap is backpressure, not an error.
- [ ] Charge the ISSUING owner, not the device, so one process cannot spend another's budget.
- [ ] Leave `reservation` unenforced until an admission path exists, and say so in the section notes rather than silently ignoring the field.
- [ ] Wrap the submission delay and any completion wait in `quota_stall_task_stalled(QUOTA_STALL_IO)` / `_unstalled`, the seam the PSI io metric needs. -> XREF: `02-kernel-core/TODO-25 §12`
- [ ] Commit: `"blkdev: per-owner I/O QoS -- IOPS and bandwidth caps with period refill"`

**Test checkpoint:** a caller capped at N IOPS completes no more than N operations per period across two consecutive periods; a bytes-per-second cap limits throughput without returning an I/O error; read, write, and control caps apply independently; an uncapped caller is unaffected by another caller's cap.

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | VirtIO-blk flush (`VIRTIO_BLK_T_FLUSH`) + `fsync()` | ✅ `storport.sys` flush via `SCSI_SYNCHRONIZE_CACHE`; `FlushFileBuffers` | ✅ `virtio_blk.c` `REQ_OP_FLUSH`; `fsync()` via `submit_bio()` | ⬜ §1 -- `VIRTIO_BLK_T_FLUSH`, write-back queue, blocking `fsync()` |
| 💎  | VirtIO-blk error recovery                | ✅ `storport.sys` request retry + error  | ✅ `virtio_blk.c` `VIRTIO_BLK_S_IOERR` → `BLK_STS_IOERR`; retry | ⬜ §2 -- status byte parse, 3× retry,    |
| 💎  | AHCI NCQ                                 | ✅ `StorAHCI.sys`; NCQ tag allocation; SDB | ✅ `libahci.c`; `ata_eh_recover_host_bus()`; NCQ via `qc_issue()` | ⬜ §3 -- `CAP.SNCQ` detect, bitmask tag allocator, |
| 💎  | AHCI error recovery -- `PxIS.TFES`, COMRESET, retry | ✅ `StorAHCI.sys`; automatic port reset + | ✅ `ahci.c` `ahci_handle_port_interrupt()`; EH framework; COMRESET | ⬜ §4 -- `PxIS.TFES` IRQ, 3× retry, COMRESET |
| 💎  | AHCI SMART read                          | ✅ `StorAHCI.sys`; SMART via `IOCTL_STORAGE_QUERY_PROPERTY` | ✅ `libata-smart.c`; `hdparm -i`; `smartctl` (user | ⬜ §5 -- `0xB0/0xD0` DMA, attribute parser, `blkdev_smart_query()` |
| ⭐  | Per-device I/O counters in `blkdev_t` + `iostat` shell command | ⚠️ PDH/ETW counters; no unified per-device | ⚠️ `/proc/diskstats` per-device; no unified abstraction | ⬜ §6 -- atomic counters in `blkdev_t` (shared |
| ⭐  | In-kernel LRU sector cache               | ⚠️ Page cache (NTFS metadata/data) --     | ⚠️ Page cache + block device              | ⬜ §7 -- single LRU cache at `blkdev_t`  |

> **After §1–7:** Impossible OS has the most transparent and measurable block layer of the three platforms. The `⭐` sections -- §6 (in-kernel `blkdev_stats()`) and §7 (unified LRU sector cache) -- are architectural choices with no direct equivalent: Windows exposes I/O metrics only through PDH/ETW (external monitoring layers); Linux exposes them through procfs without a unified cache at the `blkdev` level. Impossible OS unifies both in `blkdev_t`, so every driver and every filesystem layer automatically inherits stats and caching without additional plumbing.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] VirtIO-blk flush: write files, call `fsync()` → serial log shows `[VirtIO-BLK] Flush OK`; write-back timer fires every 5 s
- [ ] VirtIO-blk error: inject `VIRTIO_BLK_S_IOERR` (QEMU `virtio-blk-pci,drive=no-such` test); verify 3 retries in serial log, then `EIO` returned to caller
- [ ] AHCI NCQ: QEMU with `ahci` device; `dmesg`-equivalent shows `[AHCI] Port 0 NCQ enabled, depth=32`; sequential read test shows multiple tags in-flight
- [ ] AHCI SMART: `blkdev_smart_query()` on virtual drive returns temperature (should be 0°C for QEMU); reallocated = 0; boot log shows `[AHCI] Port 0 SMART: temp=0°C realloc=0`
- [ ] I/O metrics: `iostat` shell command shows non-zero `Read MB/s` during file read; counters increment correctly
- [ ] Disk cache: cache hit log shown on repeated reads of same sectors; `cache_invalidate()` called and confirmed at unmount; flush fires every 5 s in serial log
- [ ] `blkdev_stats()`: call before/after large file write; `bytes_written` delta matches file size
- [ ] Commit: `"drivers: block storage hardening -- VirtIO flush+retry, AHCI NCQ+SMART+recovery, metrics, LRU cache"`
