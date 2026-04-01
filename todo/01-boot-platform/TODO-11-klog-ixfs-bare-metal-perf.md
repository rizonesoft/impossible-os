# TODO-11 — klog Disk Flush & IXFS Boot Tests on Slow Media *(PARTIALLY SUPERSEDED by TODO-14)*

> [!NOTE]
> **§1 (bounded flush), §3 (deferred flush), §4 (speed detection), §6 (restore flush calls)** are superseded by `TODO-14-usb-boot-hardening.md §6–§8`. **§2 (single-pass routing), §5 (IXFS test adaptation), §7 (flush progress)** remain in this TODO as they are not covered elsewhere.

> **Goal:** Fix `klog_disk_flush()` hanging for 10+ minutes on USB 2.0 bare metal, and make IXFS boot tests runnable on slow media (USB sticks, SD cards) without freezing. Currently both are bypassed with comments on bare metal — they must work correctly on all boot media.

> [!IMPORTANT]
> Discovered during bare-metal testing on an i5-4210U Haswell laptop booting from USB 2.0. Each `vfs_write()` goes through the USB MSC SCSI path (CBW→data→CSW = 3 bulk transfers per write). With hundreds of log entries and multiple output files (kernel.log + 6 subsystem logs + events.jsonl + serial log), the flush takes 10+ minutes and appears frozen. The IXFS CRUD test creates/writes/deletes files — each file operation does 3-5 block writes through USB, freezing for minutes.

## Inputs

- [`src/kernel/klog_disk.c`](../../src/kernel/klog_disk.c) — disk logging: `klog_disk_flush()`, per-subsystem routing, jsonl, serial log
- [`src/kernel/klog.c`](../../src/kernel/klog.c) — `klog_disk_enable()` calls `klog_disk_init()` + `klog_disk_flush()`
- [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c) — IXFS CRUD, VFS read, performance tests
- [`src/kernel/fs/ixfs/ixfs_test.c`](../../src/kernel/fs/ixfs/ixfs_test.c) — `ixfs_test_performance()`: hash index (70 files), extents, journal, snapshots, sparse, inline, checksums, scrub
- [`src/kernel/main/boot_storage.c`](../../src/kernel/main/boot_storage.c) — `klog_disk_enable()` and `klog_disk_flush()` call sites (currently bypassed)
- → XREF: `01-boot-platform/TODO-10-usb-msc-retry-readiness.md` — USB MSC retry logic (I/O reliability)
- → XREF: `01-boot-platform/TODO-07-xhci-usb-boot.md §3` — USB MSC BOT driver

## Outcome

- `klog_disk_flush()` completes in <5 seconds on USB 2.0, <1 second on SATA/NVMe
- Ring count is snapshot once at entry — no unbounded growth from USB error logs during flush
- Per-subsystem log routing uses single-pass dispatch (1 ring scan, not 6)
- IXFS boot tests detect slow media and skip heavy I/O tests automatically
- IXFS CRUD test works on USB (create + write + read + delete in <2 seconds)
- All bypassed `klog_disk_flush()` calls restored
- No regression on QEMU or fast bare metal (SATA/NVMe)

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | klog_disk_flush ring snapshot (stop unbounded loop) | —       |  [ ]   |
| 💎  |   2   | Single-pass per-subsystem log routing          | §1         |  [ ]   |
| 💎  |   3   | Deferred flush mode (batch to RAM, write once)  | §1        |  [ ]   |
| 💎  |   4   | Boot media speed detection                     | —          |  [ ]   |
| 💎  |   5   | IXFS boot tests: slow-media-aware              | §4         |  [ ]   |
| 💎  |   6   | Restore all bypassed klog_disk_flush calls     | §1, §2, §3 |  [ ]   |
| ⭐  |   7   | Flush progress on splash diagnostic line       | §3, §6     |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. klog_disk_flush Ring Snapshot (Stop Unbounded Loop)
Snapshot `ring_count` once at function entry. All flush sections (kernel.log, per-subsystem, jsonl, serial) use this snapshot. Entries added during flush (e.g. USB error logs from `vfs_write` failures) are deferred to the next flush call.

**Files:** `src/kernel/klog_disk.c`

- [ ] At the top of `klog_disk_flush()`, after the reentrancy guard, call `klog_get_ring(&ring_count, &ring_head)` once
- [ ] Early-return if `ring` is NULL or `ring_count` is 0
- [ ] Remove all subsequent `ring = klog_get_ring()` calls inside the function (currently at 4 locations: kernel.log section, per-subsystem section, jsonl section, serial log section)
- [ ] Each section uses the snapshot values — no re-reads
- [ ] Commit: `"klog: snapshot ring_count once in disk_flush — fix unbounded USB loop"`

**Test checkpoint:** On USB 2.0, `klog_disk_flush()` processes exactly the entries that existed at call time. USB error logs generated during flush don't extend the loop. Serial log shows flush completing in bounded time. POST code `POST16(0xDB00)` on entry, `POST16(0xDB01)` on exit. Test on: bare metal i5-4210U, QEMU, VirtualBox.

**Regression risk:** LOW — reduces I/O, doesn't change what gets written. New entries are flushed on the next call. Rollback: restore the 4 `klog_get_ring()` calls.

## 2. Single-Pass Per-Subsystem Log Routing
Replace the 6-pass per-subsystem loop (one full ring scan per file) with a single pass that dispatches each entry to the correct subsystem buffer simultaneously. Only non-empty buffers trigger `vfs_open` + `vfs_write` + `vfs_close`.

**Files:** `src/kernel/klog_disk.c`

- [ ] Allocate the existing 32 KB batch buffer, divided into 6 slots (`batch_size / SUBSYS_LOG_COUNT` each)
- [ ] Single loop through the ring: for each entry, match `dispatch_filename()` to the subsystem index, append to that slot's buffer region
- [ ] After the loop, write only non-empty slots — skip `vfs_open/write/close` for subsystems with zero entries
- [ ] Track per-slot position in a `uint32_t sub_pos[SUBSYS_LOG_COUNT]` array
- [ ] Commit: `"klog: single-pass subsystem routing — 6 ring scans → 1"`

**Test checkpoint:** With 6 subsystem files and 200 ring entries, flush does 1 ring scan instead of 6. Files with no matching entries are never opened. Serial log shows reduced flush time. Test on: QEMU (fast), bare metal i5-4210U USB (slow), bare metal i5-11600K SATA.

**Regression risk:** LOW — same data written, same files, fewer I/O operations. Rollback: restore the per-subsystem loop.

## 3. Deferred Flush Mode (Batch to RAM, Write Once)
Instead of flushing to disk at every `klog_disk_flush()` call during boot, accumulate entries in the existing 256 KB FAT32 buffer and write everything in a single large `vfs_write()` at a designated flush point (e.g. after registry init, before desktop). This turns N small writes into 1 large write.

**Files:** `src/kernel/klog_disk.c`, `src/kernel/klog.c`

- [ ] Add `klog_disk_set_deferred(int on)` — when on, `klog_disk_flush()` only appends to the RAM buffer without writing to disk
- [ ] Add `klog_disk_commit()` — performs the actual disk write of the accumulated buffer
- [ ] During boot: enable deferred mode before `klog_disk_enable()`, disable and commit after registry init
- [ ] The FAT32 buffer (256 KB) is already allocated — use it as the accumulation buffer
- [ ] Single `vfs_write()` of the full buffer is 1 USB transfer vs hundreds of small ones
- [ ] Commit: `"klog: deferred flush mode — batch boot log to RAM, write once"`

**Test checkpoint:** Boot log accumulated in RAM, written to disk in one operation. On USB 2.0, the single write takes <3 seconds vs 10+ minutes for per-entry writes. Verify log file contents match serial output. POST code `POST16(0xDB02)` on deferred commit entry, `POST16(0xDB03)` on exit. Test on: bare metal i5-4210U USB, QEMU AHCI, bare metal i5-11600K.

**Regression risk:** MEDIUM — if kernel panics before commit, deferred entries are lost. Mitigation: serial log always has everything. Critical errors still go to serial immediately. Rollback: disable deferred mode (all flushes go to disk immediately).

## 4. Boot Media Speed Detection
Detect whether the boot drive is USB, SATA, or NVMe and expose this as a kernel API. Used by boot tests and klog to adapt behavior for slow media.

**Files:** `src/kernel/drivers/blkdev.c`, `include/kernel/drivers/blkdev.h`

- [ ] Add `blkdev_media_type_t` enum: `MEDIA_USB`, `MEDIA_SATA`, `MEDIA_NVME`, `MEDIA_VIRTIO`, `MEDIA_UNKNOWN`
- [ ] Add `media_type` field to `struct blkdev` — set during registration in `blkdev_register_all()`
- [ ] Add `blkdev_boot_media_type()` — returns the media type of the C:\ boot drive
- [ ] USB MSC devices → `MEDIA_USB`, AHCI → `MEDIA_SATA`, NVMe → `MEDIA_NVME`
- [ ] Commit: `"drivers: boot media type detection — USB/SATA/NVMe"`

**Test checkpoint:** `blkdev_boot_media_type()` returns `MEDIA_USB` on bare metal USB boot, `MEDIA_SATA` on QEMU AHCI, `MEDIA_NVME` on QEMU `run-nvme`. Serial log shows `boot: media type=USB`. Test on: QEMU AHCI, QEMU `run-nvme`, bare metal i5-4210U, bare metal i5-11600K.

**Regression risk:** LOW — adds a field and a query function. No behavior change until §5 uses it.

## 5. IXFS Boot Tests: Slow-Media-Aware
Make boot tests detect slow media (USB) and skip or simplify I/O-heavy tests automatically instead of hardcoded `#if 0` blocks.

**Files:** `src/kernel/main/boot_tests.c`, `src/kernel/fs/ixfs/ixfs_test.c`

- [ ] At the start of `boot_tests_run()`, query `blkdev_boot_media_type()`
- [ ] If `MEDIA_USB`: skip `ixfs_test_performance()` entirely (hash index + snapshot + scrub = hundreds of writes)
- [ ] If `MEDIA_USB`: simplify IXFS CRUD test — create + write + read only, skip delete test (delete triggers bitmap + inode writes)
- [ ] If `MEDIA_SATA` or `MEDIA_NVME`: run full test suite (all tests complete in <5 seconds on fast media)
- [ ] Log: `test: IXFS tests: full suite (SATA)` or `test: IXFS tests: reduced (USB boot media)`
- [ ] Commit: `"boot: IXFS tests auto-skip heavy I/O on USB boot media"`

**Test checkpoint:** On USB boot: serial shows `test: IXFS tests: reduced (USB boot media)`, no freeze. On SATA/NVMe: serial shows `test: IXFS tests: full suite (SATA)`, all tests pass. Test on: bare metal i5-4210U USB, QEMU AHCI, bare metal i5-11600K SATA.

**Regression risk:** LOW — tests are skipped not broken. Full suite still runs on fast media. Rollback: remove the media check, run all tests unconditionally.

## 6. Restore All Bypassed klog_disk_flush Calls
Remove the `/* BYPASSED */` comments and restore all `klog_disk_flush()` calls now that the flush is bounded and deferred.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/main/boot_tests.c`, `src/kernel/main/boot_desktop.c`, `src/kernel/hw_dump.c`

- [ ] Restore `klog_disk_enable()` in boot_storage.c (after partition mount)
- [ ] Restore `klog_disk_flush()` before registry init in boot_storage.c
- [ ] Restore `klog_disk_flush()` at start of boot_tests_run() in boot_tests.c
- [ ] Restore `klog_disk_flush()` before scheduler tests in boot_tests.c
- [ ] Restore `klog_disk_flush()` in boot_desktop.c (before fonts/desktop init)
- [ ] Restore `klog_disk_flush()` in hw_dump.c (after hardware dump)
- [ ] Verify all 6 call sites work on USB 2.0 without freezing
- [ ] Commit: `"boot: restore all klog_disk_flush calls — deferred mode handles USB"`

**Test checkpoint:** All 6 bypassed calls restored. Boot completes on USB 2.0 bare metal without freezing. Log files written correctly on C:\. POST code `POST16(0xDB04)` before first restored flush, `POST16(0xDB05)` after last. Test on: bare metal i5-4210U, QEMU, VirtualBox, bare metal i5-11600K.

**Regression risk:** HIGH — the bypasses exist because these calls freeze on USB. Must only restore after §1-§3 are verified working. Rollback: re-bypass any call that freezes.

## 7. Flush Progress on Splash Diagnostic Line
Show klog flush progress on the diagnostic subtitle during boot, so slow flushes don't look frozen.

**Files:** `src/kernel/klog_disk.c`, `src/kernel/main/boot_storage.c`

- [ ] Add optional progress callback to `klog_disk_flush()` — called periodically with entries written / total entries
- [ ] In boot_storage.c, pass a callback that updates `boot_splash_diagnostic()` with: `"Writing boot log... 150/400 entries"`
- [ ] Update after every 50 entries or every file write, whichever comes first
- [ ] Commit: `"boot: klog flush progress on splash diagnostic line"`

**Test checkpoint:** On USB 2.0 with deferred mode, single flush shows progress: `"Writing boot log... 50/400 entries"` → `"200/400"` → `"400/400"`. Diagnostic line updates smoothly. Test on: bare metal USB.

**Regression risk:** LOW — optional callback, no behavior change without it.

---

## OS Comparison

| ⭐ | Feature              | Win11                  | Linux                  | Impossible OS              |
|----|----------------------|------------------------|------------------------|----------------------------|
| 💎 | Bounded log flush    | ✅ ETW batched         | ✅ printk ring         | ⬜ §1 — snapshot ring      |
| 💎 | Single-pass routing  | ✅ ETW channel         | ✅ /dev/kmsg           | ⬜ §2 — 1 scan vs 6       |
| 💎 | Deferred boot log    | ✅ ETW buffers         | ✅ dmesg late persist  | ⬜ §3 — RAM batch          |
| 💎 | Media-aware tests    | ✅ WinPE adapts        | ✅ initramfs skips     | ⬜ §5 — auto-skip USB      |
| ⭐ | Boot media detection | ❌ Hidden in PnP       | ❌ Hidden in udev      | ⬜ §4 — API + serial log   |
| ⭐ | Flush progress       | ❌ Not shown           | ❌ Not shown           | ⬜ §7 — splash diagnostic  |

> After §1-§6, Impossible OS handles slow media as well as Windows and Linux.
> §4 and §7 are exclusive: media type visibility and flush progress at boot.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_klog_flush()` (XREF: `docs/infrastructure/kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_klog_flush.c` with:
  - Ring snapshot captures count at call time: fill ring with 10 entries, snapshot returns 10
  - Entries added during flush are not included in current snapshot
  - Single-pass routing dispatches to correct subsystem slot (fill 3 entries across 2 subsystems, verify each slot has correct entries)
  - Empty subsystem slots produce zero VFS writes (mock VFS, assert no open/write/close for empty slots)
  - `klog_disk_set_deferred(1)` suppresses disk writes: call `klog_disk_flush()`, assert zero VFS writes
  - `klog_disk_commit()` writes accumulated buffer in one batch
  - `blkdev_boot_media_type()` returns valid enum value (not out of range)
- [ ] Register in `test_runner_init()`: `test_register_klog_flush()`
- [ ] Commit: `"test: add klog disk flush and media detection test suite"`

## Verification

- [ ] QEMU `make run`: klog_disk_flush <1s, full IXFS tests pass
- [ ] QEMU `make run-nvme`: media type reported as NVMe
- [ ] VirtualBox: boot completes, klog flush <1s
- [ ] Bare metal i5-4210U (USB 2.0): klog_disk_flush <5s, reduced IXFS tests <10s
- [ ] Bare metal i5-11600K (SATA): full IXFS test suite, klog flush <1s
- [ ] All 6 `/* BYPASSED */` comments removed from boot_storage.c, boot_tests.c, boot_desktop.c, hw_dump.c
- [ ] Log files on C:\ contain complete boot log (matches serial output)
- [ ] No unbounded loops in klog_disk_flush on any media type
- [ ] Diagnostic subtitle shows flush progress during deferred commit (debug=1)
- [ ] POST code sequence: 0xDB00→0xDB01 (§1), 0xDB02→0xDB03 (§3), 0xDB04→0xDB05 (§6)
