---
schema_version: 1
id: usb-boot-hardening
domain: 01-boot-platform
status: active
title: "TODO-19 -- USB Boot Hardening & Fail-Safe Pipeline"
---

# TODO-19 -- USB Boot Hardening & Fail-Safe Pipeline

> **Goal:** Make USB boot bulletproof. The current USB boot path has a 2-second sleep hack masking a timing race, REQUEST SENSE retry logic that's partially wired, klog_disk_flush that hangs 10+ minutes on USB 2.0, no USB transport error recovery (stall/halt handling), no bulk transfer timeouts, and no EHCI/UHCI fallback for legacy hardware. This TODO is the single fail-safe pipeline for all USB boot fragility: proper SCSI retry, USB transport stall recovery, bounded bulk transfer timeouts, bounded klog flush with single-pass routing and deferred mode, boot media speed detection, slow-media-aware IXFS tests, EHCI fallback, and flush progress display. Works on USB 2.0 (EHCI), 3.0 (xHCI), 3.1, and 3.2 with zero sleep hacks and zero hangs.

> [!IMPORTANT]
> **Current state:** USB boot works on modern xHCI hardware (i5-11600K) but has known issues: (1) 2-second `sleep_ms(2000)` after xhci_init masks a SCSI UNIT ATTENTION race on EHCI→xHCI routed ports, (2) klog_disk_flush hangs 10+ min on USB 2.0 due to unbounded loop, (3) no EHCI/UHCI/OHCI fallback for systems without xHCI, (4) no retry on transient USB MSC errors, (5) XUSB2PR port-ready polling not implemented, (6) no USB transport-level stall/halt recovery -- a stalled endpoint hangs the entire USB boot, (7) no bulk transfer timeouts -- a non-responsive device blocks the kernel indefinitely.

---

## Inputs

- `src/kernel/drivers/xhci.c` -- xHCI controller init, DCBAA, port scan
- `src/kernel/drivers/xhci_dev.c` -- USB device enumeration, MSC identification
- `src/kernel/drivers/usb_msc.c` -- BOT SCSI transport (CBW/CSW/data)
- `src/kernel/klog_disk.c` -- disk flush (hangs on USB 2.0)
- `src/kernel/main/boot_storage.c` -- `sleep_ms(2000)` hack location
- [`src/kernel/klog.c`](../../src/kernel/klog.c) -- `klog_disk_enable()` calls `klog_disk_init()` + `klog_disk_flush()`
- [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c) -- IXFS CRUD, VFS read, performance tests
- [`src/kernel/fs/ixfs/ixfs_test.c`](../../src/kernel/fs/ixfs/ixfs_test.c) -- `ixfs_test_performance()`: hash index, extents, journal, snapshots
- → XREF: `TODO-17-xhci-usb-boot.md §7` -- EHCI/UHCI/OHCI fallback
- → XREF: `TODO-20-usb-zero-delay-handover.md §7` -- corrupt state fallback

---

## Outcome

- USB boot works reliably on USB 2.0 (EHCI), 3.0, 3.1, 3.2 (xHCI) with zero sleep hacks.
- SCSI UNIT ATTENTION and NOT READY are handled via proper REQUEST SENSE + TEST UNIT READY retry.
- USB transport stalls recovered automatically: CLEAR_FEATURE(ENDPOINT_HALT), xHCI Reset Endpoint, Set TR Dequeue Pointer, BOT device reset.
- Bulk transfers have bounded timeouts -- a non-responsive device never hangs the kernel.
- klog_disk_flush never hangs -- bounded loop with progress reporting.
- EHCI/UHCI fallback for systems without xHCI (pre-2012 hardware).
- XUSB2PR port-ready polling replaces fixed delays on Intel EHCI→xHCI routing.
- Boot from USB is as reliable as boot from SATA.
- Per-subsystem log routing uses single-pass dispatch (1 ring scan, not 6).
- IXFS boot tests detect slow media and skip heavy I/O tests automatically.
- Splash diagnostic line shows flush progress during slow writes.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On    | Status |
| --- | :---: | ------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | SCSI REQUEST SENSE and error classification        | --             |  [/]   |
| 💎  |   2   | TEST UNIT READY poll loop after BOT init           | §1            |  [ ]   |
| 💎  |   3   | MSC BOT retry on transient errors                  | §1, §2        |  [ ]   |
| 💎  |   4   | USB transport error recovery (stall/halt)          | §1            |  [ ]   |
| 💎  |   5   | Bulk transfer timeouts                             | §4            |  [ ]   |
| 💎  |   6   | Remove sleep_ms(2000) hack                         | §2-§5, §7     |  [ ]   |
| 💎  |   7   | XUSB2PR port-ready polling (Intel EHCI→xHCI)       | §4            |  [ ]   |
| 💎  |   8   | klog_disk_flush bounded loop                       | --             |  [ ]   |
| 💎  |   9   | klog deferred flush mode (batch to RAM)             | §8            |  [ ]   |
| 💎  |  10   | Boot media speed detection                         | §8            |  [ ]   |
| 💎  |  11   | EHCI/UHCI companion controller fallback            | --             |  [ ]   |
| ⭐  |  12   | USB boot diagnostic report                         | §1-§11        |  [ ]   |
| 💎  |  13   | Single-pass per-subsystem log routing              | §8            |  [ ]   |
| 💎  |  14   | IXFS boot tests: slow-media-aware                  | §10           |  [ ]   |
| ⭐  |  15   | Flush progress on splash diagnostic line           | §9            |  [ ]   |

> 💎 = parity -- Windows usbstor.sys and Linux usb-storage both handle SCSI retry, stall recovery, transfer timeouts, and EHCI fallback.
> ⭐ = exclusive -- comprehensive USB boot diagnostic report and splash flush progress.

---

## 1. SCSI REQUEST SENSE and Error Classification

Implement the SCSI REQUEST SENSE command to decode why a USB MSC command failed.

- [x] `msc_request_sense(hc, dev, sense)` in `usb_msc.c` -- REQUEST SENSE CDB (0x03, 18-byte fixed format) via the shared BOT transport; non-recursive (REQUEST SENSE itself raises no CHECK CONDITION per SPC-4)
- [x] Parse sense: key (`sense[2]&0x0F`), ASC (`sense[12]`), ASCQ (`sense[13]`); SCSI sense keys moved to a shared `include/kernel/drivers/scsi.h` (was duplicated in `ahci.h`)
- [x] `msc_sense_classify()` (pure) -> `msc_err_class_t` {OK, RETRY_NOW (UNIT ATTENTION), WAIT_RETRY (NOT READY), UNRECOVERABLE}; wired into `usb_msc_init`'s TUR loop so the sense class drives the retry
- [x] Log `Sense: key=%u ASC=0x%02x ASCQ=0x%02x (%s)` with `msc_sense_key_name()` (all 10 named keys)
- [ ] Boot-LUN selection for composite media (card readers; BOT now reads `info->current_lun`, default 0): probe GET_MAX_LUN + per-LUN TUR -- BLOCKED on `04-drivers-hardware/TODO-10 §7` (item: "`usb_msc_get_max_lun(dev)`" at line 177)
- [ ] Commit: `"drivers: SCSI REQUEST SENSE command with error classification"`

**Test checkpoint:** Plug USB drive, boot. If drive returns UNIT ATTENTION on first command, serial shows sense data. Verify on bare metal i5-4210U.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_boot.c` SCSI sense classification + key-name (every key class + retry policy + high-bit mask + NULL/unknown); live REQUEST SENSE validated via QEMU run-usb + bare metal.
> **Notes:**
> - Shipped: `msc_request_sense` + pure `msc_sense_classify` / `msc_sense_key_name` (`usb_msc.c`) + shared `scsi.h` sense keys; the BOT CBW now addresses `info->current_lun` (foundation for boot-LUN).
> - Integrates: `usb_msc_init`'s TUR retry now decodes the failure and retries per the sense class (UNIT ATTENTION immediate, NOT READY waits, else stops) instead of a blind delay; `ahci.h` now includes the shared `scsi.h`.
> - Hardening: REQUEST SENSE is non-recursive (transport failure reported, not re-sensed); `msc_sense_classify` defaults unknown/named-non-retry keys to UNRECOVERABLE so a future change can't silently retry write-protected/illegal/aborted failures.
> - Scope boundary: §1 owns sense decode + classification; the retry LOOP is §3; boot-LUN probe is blocked on the GET_MAX_LUN owner (TODO-10 §7).
> **Verified:** 2026-06-15 | review commit | 4/5 items | build OK | storage 98 kernel + 16 user-mode PASS (SCSI sense classify + key-name)
> **Accepted:** [H] short REQUEST SENSE reply (`xhci_bulk_transfer` reports SHORT_PKT as success with no byte count) can leave key/ASC/ASCQ zero-filled; the `sense[0]` format gate is a partial guard -> XREF: 01-boot-platform/TODO-19 §5 (item: "Report ACTUAL transferred length from `xhci_bulk_transfer`" at line 171)
> **Deferred:** [M] boot-LUN selection for composite/card-reader media blocked on GET_MAX_LUN (`xhci_control_transfer` is static) -> XREF: 04-drivers-hardware/TODO-10 §7 (item: "`usb_msc_get_max_lun(dev)`" at line 177)
> **Quality reviewed:** 2026-06-15 | Codex 6x (design, test-coverage, adversarial x2, consistency, perf) | 1H+3M fixed, 1H accepted-XREF, 1M rejected | scope: kernel-code-quality

---

## 2. TEST UNIT READY Poll Loop

After BOT init, poll TEST UNIT READY until the device reports ready instead of sleeping.

- [ ] Add `msc_test_unit_ready(dev)` -- sends TEST UNIT READY CDB (0x00)
- [ ] Poll loop: send TUR → if error, `REQUEST SENSE` → if NOT_READY or UNIT_ATTENTION, wait 50ms + retry
- [ ] Maximum 40 attempts (2 seconds total) -- same window as the current sleep hack but event-driven
- [ ] If all attempts fail: log `"[WARN] USB drive not ready after 2s"` but continue (device may respond later)
- [ ] Commit: `"drivers: TEST UNIT READY poll loop -- replace sleep hack with SCSI readiness"`

**Test checkpoint:** USB 2.0 drive on i5-4210U bare metal boots without the sleep hack. Serial shows TUR poll count.

---

## 3. MSC BOT Retry on Transient Errors

Wrap `msc_bot_command()` with automatic retry for transient SCSI errors.

- [ ] CSW validation BEFORE classifying status (BOT correctness, not just SCSI): verify `dCSWSignature==0x53425355` and `dCSWTag` matches the CBW tag; on signature/tag mismatch -> BOT mass-storage reset (§4), not a SCSI sense retry
- [ ] Residue check: compare `dCSWDataResidue` to the requested length even when `bCSWStatus==0` (pass) -- a short data phase on a boot READ must FAIL SAFE (no silent partial read / corrupt filesystem input), not count as success
- [ ] `bCSWStatus==2` (phase error) -> BOT mass-storage reset + Clear-Feature on both bulk endpoints (§4), then re-issue; never SCSI-sense-retry a phase error
- [ ] After CSW returns error (`bCSWStatus==1`): call `REQUEST SENSE` → classify error
- [ ] `UNIT_ATTENTION`: retry command immediately (device reset itself, normal after plug)
- [ ] `NOT_READY`: wait 100ms + retry (drive spinning up)
- [ ] `MEDIUM_ERROR`: fail immediately (bad sector, no retry will help)
- [ ] Maximum 3 retries per command
- [ ] Log: `"[USB] Retry %u/3: sense key=%u"` on each retry
- [ ] Commit: `"drivers: USB MSC BOT automatic retry for transient SCSI errors"`

**Test checkpoint:** Normal USB boot -- no retries needed, zero performance impact. Bare metal with slow USB 2.0 -- retries handle UNIT ATTENTION transparently.

---

## 4. USB Transport Error Recovery (Stall/Halt Handling)

Handle USB transport-level stalls -- a separate layer from SCSI sense errors. A bulk endpoint can stall due to data toggle mismatch, babble, or controller error even when the SCSI command is valid. Without this, a single stalled endpoint hangs the entire USB boot.

**Files:** `src/kernel/drivers/xhci_dev.c` (extend), `src/kernel/drivers/usb_msc.c` (extend)

> [!NOTE]
> xHCI Transfer Event TRB completion codes: `STALL_ERROR (6)` = endpoint halted by device, `USB_TRANSACTION_ERROR (4)` = CRC/bitstuff/bad PID, `BABBLE_DETECTED_ERROR (3)` = device sent more data than expected, `DATA_BUFFER_ERROR (2)` = host controller data buffer overrun/underrun. All of these leave the endpoint in the Halted state. Recovery requires: (1) `CLEAR_FEATURE(ENDPOINT_HALT)` USB control transfer to the device, (2) xHCI Reset Endpoint command to transition the endpoint from Halted to Stopped, (3) xHCI Set TR Dequeue Pointer command to advance past the failed TRB, (4) re-ring the doorbell.

- [ ] `xhci_endpoint_is_halted(dev, ep_id)`: check endpoint context state field -- state `2` = Halted
- [ ] `xhci_clear_endpoint_halt(dev, ep_id)`: send `CLEAR_FEATURE(ENDPOINT_HALT)` control transfer to the device (bmRequestType=`0x02`, bRequest=`0x01` CLEAR_FEATURE, wValue=`0x00` ENDPOINT_HALT, wIndex=endpoint address)
- [ ] `xhci_reset_endpoint(hc, slot, ep_id)`: issue Reset Endpoint command (TRB type 14); wait for Command Completion Event with CC == SUCCESS
- [ ] `xhci_set_tr_dequeue(hc, slot, ep_id, dequeue_ptr)`: issue Set TR Dequeue Pointer command (TRB type 10) pointing past the failed TRB; advance the transfer ring's dequeue pointer
- [ ] `xhci_recover_endpoint(dev, ep_id)`: combined recovery sequence -- clear halt, reset endpoint, set dequeue, log `"[USB] EP%u recovered from stall (slot=%u)"`
- [ ] In `xhci_bulk_transfer()` / `xhci_control_transfer()`: on completion code `STALL_ERROR`, `USB_TRANSACTION_ERROR`, `BABBLE_DETECTED_ERROR`, or `DATA_BUFFER_ERROR`, call `xhci_recover_endpoint()` and return a retriable error code
- [ ] `msc_bot_reset(dev)`: send BOT mass-storage reset class request (bmRequestType=`0x21`, bRequest=`0xFF`, wValue=0, wIndex=interface, wLength=0); then `CLEAR_FEATURE(ENDPOINT_HALT)` on both bulk-IN and bulk-OUT endpoints; used after CSW phase error (`bCSWStatus == 2`)
- [ ] Integrate with §3 retry: after `xhci_recover_endpoint()`, retry the failed BOT command (up to 3 attempts)
- [ ] Commit: `"drivers: USB transport error recovery -- stall/halt clear, endpoint reset, BOT device reset"`

**Test checkpoint:** Force-stall a USB endpoint in QEMU (or encounter one naturally on bare metal USB 2.0) -- serial shows `"EP recovered from stall"`, boot continues without hang. If recovery fails after 3 retries, log error and mark device non-functional rather than hanging.

**Regression risk:** MEDIUM -- modifies the transfer completion path. If the recovery sequence issues incorrect xHCI commands, the endpoint may become permanently stuck. Rollback: disable recovery, return error immediately on stall (current behavior but with a timeout instead of infinite hang).

---

## 5. Bulk Transfer Timeouts

Add bounded timeouts to all bulk transfers. The current code polls the xHCI event ring in a tight loop with no deadline -- if a device stops responding (cable disconnect mid-transfer, firmware hang, powered hub brownout), the kernel hangs indefinitely.

**Files:** `src/kernel/drivers/xhci_dev.c` (extend), `src/kernel/drivers/xhci_ring.c` (extend)

> [!NOTE]
> xHCI does not have hardware transfer timeouts -- the host controller will wait indefinitely for a device response. Software must implement timeouts by: (1) recording the TSC or LAPIC timer value at transfer submission, (2) checking elapsed time on each event ring poll iteration, (3) on timeout: issuing a Stop Endpoint command (TRB type 15) to abort the pending TRB, then recovering via the §4 stall/halt path.

- [ ] Add `timeout_ms` parameter to `xhci_bulk_transfer()` and `xhci_control_transfer()` -- default 5000ms for bulk, 2000ms for control
- [ ] At transfer submission: record `start_tsc = rdtsc()`; compute `deadline_tsc = start_tsc + ms_to_tsc(timeout_ms)` using calibrated TSC frequency
- [ ] In event ring poll loop: after each poll iteration, check `rdtsc() > deadline_tsc`; if true, trigger timeout path
- [ ] Timeout path: issue Stop Endpoint command (TRB type 15, slot, EP DCI); wait for Command Completion; the stopped TRB appears as a Transfer Event with CC=`STOPPED` or `STOPPED_LENGTH_INVALID`; advance dequeue past it
- [ ] After timeout: log `"[USB] Bulk transfer timeout on slot=%u EP%u (%u ms)"` and return `USB_ERR_TIMEOUT`
- [ ] Integrate with §3/§4 retry: `USB_ERR_TIMEOUT` is retriable -- call `xhci_recover_endpoint()`, then retry (up to 3 attempts)
- [ ] MSC-specific: `usb_msc_read_sectors()` / `usb_msc_write_sectors()` propagate timeout error; caller sees it as a transient I/O failure
- [ ] Report ACTUAL transferred length from `xhci_bulk_transfer` (SHORT_PKT counts as success today); `msc_request_sense` (§1) must reject a fixed-format reply shorter than the key/ASC/ASCQ bytes, not classify a zero-filled tail as NO SENSE
- [ ] Configurable timeout: `USB_BULK_TIMEOUT_MS` (default 5000), `USB_CONTROL_TIMEOUT_MS` (default 2000) -- constants in header, tunable per-device if needed for slow media
- [ ] Commit: `"drivers: USB bulk transfer timeouts -- TSC deadline, Stop Endpoint, retry integration"`

**Test checkpoint:** Disconnect USB device during a bulk transfer (QEMU `device_del` mid-I/O) -- kernel logs timeout within 5 seconds instead of hanging. Boot continues if the device was not the boot drive. If boot drive times out, fall back to the no-USB boot path with a clear error message.

**Regression risk:** MEDIUM -- adds overhead (TSC read per poll iteration, ~5ns). If TSC frequency calibration is wrong, timeouts fire too early or too late. Rollback: set `USB_BULK_TIMEOUT_MS` to `UINT32_MAX` to effectively disable timeouts while keeping the infrastructure.

---

## 6. Remove sleep_ms(2000) Hack

With proper SCSI retry (§1-§3) and USB transport recovery (§4-§5), the 2-second sleep after xhci_init is no longer needed.

- [ ] Remove `sleep_ms(2000)` from `boot_storage.c` (or wherever it currently lives)
- [ ] Verify USB boot works on: QEMU TCG, bare metal i5-11600K, bare metal i5-4210U
- [ ] If any platform fails without the sleep: investigate root cause, don't add the sleep back
- [ ] Verify: endpoint stalls and transfer timeouts (§4-§5) handle cases the sleep was masking
- [ ] Commit: `"drivers: remove USB 2-second sleep hack -- SCSI retry + transport recovery handles readiness"`

**Test checkpoint:** USB boot works on all tested platforms without the delay. Boot time improves by ~2 seconds.

**Regression risk:** HIGH -- this is the moment of truth. If retry logic isn't sufficient, USB boot breaks on slow hardware. Rollback: temporarily re-add `sleep_ms(500)` as a smaller delay while investigating.

---

## 7. XUSB2PR Port-Ready Polling (Intel EHCI→xHCI)

On Intel chipsets, USB 2.0 ports are routed from EHCI to xHCI via the XUSB2PR PCI register. The port switch takes time -- currently masked by the 2-second sleep.

- [ ] After writing XUSB2PR: poll port status registers until ports report connected/enabled
- [ ] Maximum wait: 500ms (real hardware typically takes <100ms)
- [ ] Log: `"[USB] XUSB2PR port routing complete (%u ms)"` with actual time
- [ ] If timeout: `"[WARN] XUSB2PR port routing timeout -- some USB 2.0 ports may not work"`
- [ ] Only applies to Intel chipsets with XUSB2PR capability (detect via PCI vendor/device)
- [ ] Commit: `"drivers: XUSB2PR port-ready polling -- replace fixed delay with event-driven wait"`

**Test checkpoint:** i5-4210U bare metal with USB 2.0 drive -- port routing completes with logged time. Non-Intel systems skip this entirely.

---

## 8. klog_disk_flush Bounded Loop

Fix the unbounded flush loop that hangs 10+ minutes on USB 2.0.

- [ ] Bound via the EXISTING seq cursor, NOT `ring_count`: snapshot `klog_get_seq()` at entry, flush only entries with seq < snapshot (ring_count bounding caused a prior saturation/replay bug -> XREF: 02-kernel-core/TODO-04 klog_ring_seq design)
- [ ] Preserve cursor-advance-on-success: advance the persisted flush cursor only for entries actually written; entries logged DURING flush (seq >= snapshot) wait for the next call -- correct across the 1000-entry ring wrap
- [ ] Add progress: `"[KLOG] Flushing %u entries to disk..."` at start, `"done (%u ms)"` at end
- [ ] If flush takes > 5 seconds: `"[WARN] Slow media detected -- switching to deferred flush"` (triggers §9)
- [ ] Commit: `"kernel: bounded klog_disk_flush -- seq-cursor snapshot, no unbounded loop"`

**Test checkpoint:** USB 2.0 boot on bare metal -- flush completes in seconds, not minutes. Serial shows flush count and duration.

---

## 9. klog Deferred Flush Mode

For slow media, batch log entries in RAM and write once at boot end instead of per-subsystem.

- [ ] Add `klog_set_deferred(int enabled)` -- switches to RAM-only buffering
- [ ] During deferred mode: entries accumulate in ring buffer, `klog_disk_flush()` is a no-op
- [ ] At boot complete: single `klog_disk_flush_all()` writes everything at once
- [ ] Combine multiple entries into larger VFS writes (fewer USB transfers)
- [ ] Automatically enable deferred mode if §8 detects slow media (>5s for first flush)
- [ ] Commit: `"kernel: klog deferred flush -- batch to RAM, single write at boot end"`

**Test checkpoint:** USB 2.0 boot -- all log entries appear in disk log file, written in one batch. Boot time comparable to no-disk-log scenario.

---

## 10. Boot Media Speed Detection

Detect whether boot media is fast (SSD/NVMe) or slow (USB 2.0/USB 3.0 stick) and adjust behavior.

- [ ] At first disk I/O: time a 4 KiB read -- classify as fast (<1ms), medium (1-10ms), slow (>10ms)
- [ ] Store in `boot_info.boot_media_speed` (0=unknown, 1=fast, 2=medium, 3=slow)
- [ ] Kernel uses this to: enable deferred klog (slow), skip non-critical boot tests (slow), adjust timeouts
- [ ] Log: `"[BOOT] Boot media speed: %s (%u µs/4KiB)"` with classification
- [ ] Commit: `"kernel: boot media speed detection -- adjust behavior for slow USB"`

**Test checkpoint:** SATA SSD → "fast", USB 3.0 stick → "medium", USB 2.0 stick → "slow".

---

## 11. EHCI/UHCI Companion Controller Fallback

For pre-2012 hardware without xHCI, provide basic USB storage via EHCI.

> [!NOTE]
> EHCI ownership split: the full EHCI HCD (vtable + class-driver reuse) is owned by `04-drivers-hardware/TODO-10-usb-stack.md §10`. This section owns the minimal boot-storage EHCI/UHCI fallback + the OHCI-only graceful skip; if T10 §10 lands first, reuse that driver and own only boot integration + tests here. Routed from `01-boot-platform/TODO-17 §6` (non-Intel/legacy de-scope).

- [ ] Detect EHCI controller: PCI class 0x0C/0x03/0x20
- [ ] Minimal EHCI driver: port reset, bulk transfer, BOT SCSI -- enough for MSC storage
- [ ] If xHCI not found: try EHCI; if EHCI not found: try UHCI (PCI class 0x0C/0x03/0x00)
- [ ] OHCI-only hardware (PCI class 0x0C/0x03/0x10): no OHCI driver -- log `"USB: OHCI-only controller -- USB storage not supported on this hardware"` and skip gracefully (no hang)
- [ ] Share the `usb_msc.c` BOT layer -- only the host controller interface differs
- [ ] EHCI/UHCI transport recovery + timeouts at §4/§5 parity (halted-qTD, ClearFeature, async cleanup, bounded timeouts) via an HCD-agnostic USB error contract -- the fallback must not hang where §4/§5 harden xHCI
- [ ] Log: `"[USB] Using %s controller (xHCI not available)"` with EHCI/UHCI
- [ ] Commit: `"drivers: EHCI fallback for USB storage on pre-xHCI hardware"`

**Test checkpoint:** VirtualBox (which emulates EHCI by default) -- USB storage works via EHCI fallback. Serial shows `"Using EHCI controller"`.

**Regression risk:** HIGH -- new driver code. If EHCI driver corrupts USB state, system hangs. Test on VirtualBox first.

---

## 12. USB Boot Diagnostic Report

At end of USB enumeration, produce a comprehensive diagnostic summary.

- [ ] Log all discovered USB devices with: port, speed (HS/SS/SS+), vendor/product, MSC/HID/other
- [ ] Log controller type: xHCI (version), EHCI, UHCI
- [ ] Log port routing status: XUSB2PR active, companion controller status
- [ ] Log SCSI retry statistics: total retries, sense keys encountered
- [ ] Log boot media speed classification
- [ ] Format: `"[USB] === USB Boot Report ==="` section in serial log
- [ ] Commit: `"drivers: USB boot diagnostic report -- full enumeration summary"`

**Test checkpoint:** Serial output contains `"=== USB Boot Report ==="` with device list and statistics.

---

## 13. Single-Pass Per-Subsystem Log Routing

Replace the 6-pass per-subsystem loop (one full ring scan per file) with a single pass that dispatches each entry to the correct subsystem buffer simultaneously. Only non-empty buffers trigger `vfs_open` + `vfs_write` + `vfs_close`.

**Files:** `src/kernel/klog_disk.c`

- [ ] Allocate the existing 32 KB batch buffer, divided into 6 slots (`batch_size / SUBSYS_LOG_COUNT` each)
- [ ] Single loop through the ring: for each entry, match `dispatch_filename()` to the subsystem index, append to that slot's buffer region
- [ ] After the loop, write only non-empty slots -- skip `vfs_open/write/close` for subsystems with zero entries
- [ ] Track per-slot position in a `uint32_t sub_pos[SUBSYS_LOG_COUNT]` array
- [ ] Commit: `"klog: single-pass subsystem routing -- 6 ring scans → 1"`

**Test checkpoint:** With 6 subsystem files and 200 ring entries, flush does 1 ring scan instead of 6. Files with no matching entries are never opened. Serial log shows reduced flush time. Test on: QEMU (fast), bare metal i5-4210U USB (slow), bare metal i5-11600K SATA.

**Regression risk:** LOW -- same data written, same files, fewer I/O operations. Rollback: restore the per-subsystem loop.

---

## 14. IXFS Boot Tests: Slow-Media-Aware

Make boot tests detect slow media (USB) and skip or simplify I/O-heavy tests automatically instead of hardcoded `#if 0` blocks.

**Files:** `src/kernel/main/boot_tests.c`, `src/kernel/fs/ixfs/ixfs_test.c`

- [ ] At the start of `boot_tests_run()`, query `blkdev_boot_media_type()`
- [ ] If `MEDIA_USB`: skip `ixfs_test_performance()` entirely (hash index + snapshot + scrub = hundreds of writes)
- [ ] If `MEDIA_USB`: simplify IXFS CRUD test -- create + write + read only, skip delete test (delete triggers bitmap + inode writes)
- [ ] If `MEDIA_SATA` or `MEDIA_NVME`: run full test suite (all tests complete in <5 seconds on fast media)
- [ ] Log: `test: IXFS tests: full suite (SATA)` or `test: IXFS tests: reduced (USB boot media)`
- [ ] Commit: `"boot: IXFS tests auto-skip heavy I/O on USB boot media"`

**Test checkpoint:** On USB boot: serial shows `test: IXFS tests: reduced (USB boot media)`, no freeze. On SATA/NVMe: serial shows `test: IXFS tests: full suite (SATA)`, all tests pass. Test on: bare metal i5-4210U USB, QEMU AHCI, bare metal i5-11600K SATA.

**Regression risk:** LOW -- tests are skipped not broken. Full suite still runs on fast media. Rollback: remove the media check, run all tests unconditionally.

---

## 15. Flush Progress on Splash Diagnostic Line

Show klog flush progress on the diagnostic subtitle during boot, so slow flushes don't look frozen.

**Files:** `src/kernel/klog_disk.c`, `src/kernel/main/boot_storage.c`

- [ ] Add optional progress callback to `klog_disk_flush()` -- called periodically with entries written / total entries
- [ ] In boot_storage.c, pass a callback that updates `boot_splash_diagnostic()` with: `"Writing boot log... 150/400 entries"`
- [ ] Update after every 50 entries or every file write, whichever comes first
- [ ] Commit: `"boot: klog flush progress on splash diagnostic line"`

**Test checkpoint:** On USB 2.0 with deferred mode, single flush shows progress: `"Writing boot log... 50/400 entries"` → `"200/400"` → `"400/400"`. Diagnostic line updates smoothly. Test on: bare metal USB.

**Regression risk:** LOW -- optional callback, no behavior change without it.

---

## OS Comparison

| ⭐ | Feature                      | 🪟 Win11                     | 🐧 Linux                      | 🚀 Impossible OS               |
|----|------------------------------|---------------------------|----------------------------|-----------------------------|
| 💎 | SCSI error retry             | ✅ usbstor.sys retries   | ✅ usb-storage retries     | ⬜ §1-§3                    |
| 💎 | USB stall/halt recovery      | ✅ usbstor.sys auto-reset | ✅ usb-storage ep reset   | ⬜ §4                       |
| 💎 | Bulk transfer timeouts       | ✅ USBD_DEFAULT_PIPE_TRANSFER_TIMEOUT | ✅ usb_submit_urb timeout | ⬜ §5                 |
| 💎 | No sleep hacks               | ✅ Event-driven readiness | ✅ SCSI start-stop         | ⬜ §6                       |
| 💎 | EHCI/UHCI fallback           | ✅ Full USB stack         | ✅ ehci-hcd + uhci-hcd     | ⬜ §11                      |
| 💎 | Bounded disk flush           | ✅ Async I/O              | ✅ Writeback cache          | ⬜ §8-§9                    |
| 💎 | Media speed detection        | ✅ Performance tier       | ✅ readahead tuning        | ⬜ §10                      |
| ⭐ | USB boot diagnostic report   | ❌ Hidden in Event Log    | ❌ dmesg only              | ⬜ §12 🚀                   |
| 💎 | Single-pass log routing      | ✅ ETW channel            | ✅ /dev/kmsg               | ⬜ §13                      |
| 💎 | Media-aware boot tests       | ✅ WinPE adapts           | ✅ initramfs skips          | ⬜ §14                      |
| ⭐ | Flush progress display       | ❌ Not shown              | ❌ Not shown               | ⬜ §15 🚀                   |

After §1-§11, USB boot is as reliable as Windows and Linux across all USB generations and controller types -- including transport-level stall recovery and bounded timeouts that prevent hangs on flaky hardware. §12-§15 add diagnostic and I/O optimizations for slow media.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_usb_boot()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Hardware-dependent tests use `scripts/test-smoke.sh` serial pattern matching on QEMU `run-usb`.

- [ ] Create `src/kernel/test/test_usb_boot.c` with:
  - SCSI sense classification: `UNIT_ATTENTION` (key=6) classified as retriable
  - SCSI sense classification: `NOT_READY` (key=2) classified as retriable
  - SCSI sense classification: `MEDIUM_ERROR` (key=3) classified as unrecoverable
  - Retry cap honored: mock a command that always fails NOT_READY, verify exactly 3 retries (not infinite)
  - Stall recovery: `xhci_endpoint_is_halted()` returns true for state==2, false otherwise
  - Bulk timeout: transfer with `timeout_ms=0` returns `USB_ERR_TIMEOUT` immediately (boundary test)
  - `boot_info.boot_media_speed` is a valid value (0-3)
  - klog bounded flush: ring snapshot count does not grow during flush
  - Single-pass routing dispatches to correct subsystem slot (fill 3 entries across 2 subsystems, verify each slot has correct entries)
  - Empty subsystem slots produce zero VFS writes (mock VFS, assert no open/write/close for empty slots)
  - `blkdev_boot_media_type()` returns valid enum value (not out of range)
- [ ] Register in `test_runner_init()`: `test_register_usb_boot()`
- [ ] Add smoke test patterns to `scripts/test-smoke.sh` for QEMU `run-usb`:
  - Serial line `"[USB] === USB Boot Report ==="` present (§12 -- diagnostic report)
  - Absence of `"WATCHDOG"` or `"KERNEL PANIC"` during USB boot
- [ ] Commit: `"test: add USB boot hardening test suite"`

## Verification

- [ ] **USB 2.0 bare metal**: boot from USB 2.0 on i5-4210U -- no sleep hack, SCSI retries handle readiness, klog flush completes in seconds.
- [ ] **USB 3.0 QEMU**: boot from USB 3.0 via xHCI on TCG -- works as before, no regressions.
- [ ] **Stall recovery**: force a stall condition -- serial shows `"EP recovered from stall"`, boot continues.
- [ ] **Transfer timeout**: disconnect USB during I/O -- kernel logs timeout within 5s, does not hang.
- [ ] **EHCI fallback**: boot from USB on VirtualBox (EHCI) -- MSC storage works via EHCI driver.
- [ ] **Normal SATA boot regression**: SATA boot on all 4 platforms unaffected.
- [ ] **klog flush**: all log entries appear in disk log file, flush completes in bounded time.
- [ ] **IXFS slow media**: on USB boot, serial shows `test: IXFS tests: reduced (USB boot media)`. On SATA/NVMe, full suite runs.
- [ ] **Flush progress**: on USB 2.0 with deferred mode, splash diagnostic shows `"Writing boot log... N/M entries"`.
- [ ] Commit: `"boot: USB boot hardening complete -- fail-safe pipeline across all USB generations"`
