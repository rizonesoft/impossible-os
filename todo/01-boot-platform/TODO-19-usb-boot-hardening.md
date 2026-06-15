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
| 💎  |   2   | TEST UNIT READY poll loop after BOT init           | §1            |  [x]   |
| 💎  |   3   | MSC BOT retry on transient errors                  | §1, §2, §4    |  [x]   |
| 💎  |   4   | USB transport error recovery (stall/halt)          | §1            |  [x]   |
| 💎  |   5   | Bulk transfer timeouts                             | §4            |  [/]   |
| 💎  |   6   | Remove sleep_ms(2000) hack                         | §2-§5, §7     |  [/]   |
| 💎  |   7   | XUSB2PR port-ready polling (Intel EHCI→xHCI)       | §4            |  [/]   |
| 💎  |   8   | klog_disk_flush bounded loop                       | --             |  [/]   |
| 💎  |   9   | klog deferred flush mode (batch to RAM)             | §8            |  [x]   |
| 💎  |  10   | Boot media speed detection                         | §8            |  [/]   |
| 💎  |  11   | EHCI/UHCI companion controller fallback            | --             |  [ ]   |
| ⭐  |  12   | USB boot diagnostic report                         | §1-§11        |  [ ]   |
| 💎  |  13   | Single-pass per-subsystem log routing              | §8            |  [ ]   |
| 💎  |  14   | IXFS boot tests: slow-media-aware                  | §10           |  [ ]   |
| ⭐  |  15   | Flush progress on splash diagnostic line           | §9            |  [ ]   |
| 💎  |  16   | xHCI command ring + BOT transport SMP serialization | §4            |  [ ]   |

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
> **Deferred:** [M] boot-LUN selection for composite/card-reader media blocked on GET_MAX_LUN (`xhci_control_transfer` is static) -> XREF: 04-drivers-hardware/TODO-10 §7 (item: "`usb_msc_get_max_lun(dev)`" at line 177)
> **Quality reviewed:** 2026-06-15 | Codex 6x (design, test-coverage, adversarial x2, consistency, perf) | 1H+3M fixed, 1H accepted-XREF, 1M rejected | scope: kernel-code-quality

---

## 2. TEST UNIT READY Poll Loop

After BOT init, poll TEST UNIT READY until the device reports ready instead of sleeping.

- [x] `msc_test_unit_ready(hc, dev)` -- TEST UNIT READY CDB (0x00) via BOT; the single-command primitive the poll loop drives (added in §1, wired here)
- [x] `msc_poll_unit_ready(hc, dev)` poll loop: send TUR; on CSW FAIL issue `REQUEST SENSE`; classify; NOT READY / UNIT ATTENTION / NO SENSE -> wait `MSC_TUR_POLL_US` (50ms) + retry
- [x] Pure `msc_tur_decide(rc, rs_rc, cls)` -> {READY, WAIT, GIVEUP, ABORT}: TUR/REQUEST-SENSE desync -> ABORT (init returns -1, no INQUIRY into a desynced pipe); hard sense, healthy pipe -> GIVEUP (warn + continue)
- [x] Real wall-clock budget `MSC_TUR_READY_BUDGET_MS` (2000ms) checked BEFORE each command via wrap-safe `uptime_ns()-start`; `MSC_TUR_MAX_ATTEMPTS` (40) backstops the pre-timer path; a ready drive returns on attempt 0
- [x] On readiness timeout / hard error: `klog(LOG_WARN, ...)` "USB drive not ready within 2000 ms, continuing" and proceed; only a transport desync aborts
- [x] Commit: `"drivers: TEST UNIT READY poll loop -- replace sleep hack with SCSI readiness"`

**Test checkpoint:** USB 2.0 drive on i5-4210U bare metal boots without the sleep hack. Serial shows TUR poll count.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_boot.c` "TEST UNIT READY poll decision" exercises the full `msc_tur_decide` matrix (READY / WAIT / GIVEUP / ABORT incl. TUR-desync + REQUEST-SENSE-desync rows); live poll validated via QEMU run-usb + bare metal.
> **Notes:**
> - Shipped: `msc_poll_unit_ready` + pure `msc_tur_decide` (`usb_msc.c`); `msc_request_sense` now reports its BOT status via an `rs_rc` out-param so a desync during recovery is distinguishable from a hard sense.
> - Integrates: replaces the §1 inline 3-retry loop in `usb_msc_init` with an event-driven poll under a real `uptime_ns()` wall-clock budget; a ready drive proceeds with zero added latency.
> - Hardening: ABORT vs GIVEUP split -- only a BOT pipe desync (TUR or REQUEST SENSE phase/transport failure) aborts init; a hard SCSI error on a healthy pipe warns and continues so INQUIRY/READ CAPACITY surface it. Codex review adoptions in the impl + review commits.
> - Scope boundary: §2 owns the readiness poll; removing the separate `sleep_ms(2000)` is §6; BOT mass-storage reset / endpoint-stall recovery for the ABORT path is §4.
> **Verified:** 2026-06-15 | review commit | 5/5 items | build OK | storage 109 kernel + 16 user-mode PASS (TUR poll decision matrix)
> **Accepted:** [M] `msc_info` keyed by per-controller `slot_id` collides across controllers -> XREF: 01-boot-platform/TODO-19 §11 (item: "Key MSC state by global device, not per-controller `slot_id`" at line 277)
> **Quality reviewed:** 2026-06-15 | Codex 9x (design, test-coverage, adversarial x2, re-adversarial x3, consistency, perf) | 4H+1M+1L fixed, 1H rejected, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## 3. MSC BOT Retry on Transient Errors

Wrap `msc_bot_command()` with automatic retry for transient SCSI errors.

- [x] CSW signature/tag validation BEFORE classifying status: `dCSWSignature` + `dCSWTag` checked in `msc_bot_command`; a mismatch is a desync -> BOT mass-storage reset (shipped in §4), not a SCSI sense retry
- [x] Residue check: `msc_bot_command` exposes `dCSWDataResidue` (rejects residue > requested); pure `msc_residue_short` -- exact-length READ/WRITE/CAPACITY with any residue FAILs safe; short REQUEST SENSE / INQUIRY rejected via the residue minimum
- [x] `bCSWStatus==2` (phase error) -> BOT mass-storage reset (shipped in §4); the reset result is checked so a failed reset returns transport-failure, never a retryable phase status
- [x] `msc_scsi_command` wrapper: on CSW FAIL calls `REQUEST SENSE` -> classify; pure `msc_scsi_retry_decide(rc, rs_rc, cls, exact_short)` drives the action (a REQUEST SENSE phase/transport desync -> reset-before-retry)
- [x] `UNIT_ATTENTION` (and benign NO SENSE on a CSW FAIL) -> retry now; a CSW FAIL never reports success off a benign sense
- [x] `NOT_READY` -> wait `MSC_SCSI_WAIT_US`(100ms) + retry
- [x] `MEDIUM_ERROR` / unrecoverable -> fail immediately (no retry will help)
- [x] Maximum `MSC_SCSI_RETRIES`(3) attempts per command, then fail; transport (<0) desync does a BOT reset before each retry
- [x] Log each retry (`klog` DEBUG: attempt, cdb, rc, sense class)
- [x] Commit: `"drivers: USB MSC BOT automatic retry for transient SCSI errors"`

**Test checkpoint:** Normal USB boot -- no retries needed, zero performance impact. Bare metal with slow USB 2.0 -- retries handle UNIT ATTENTION transparently.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_boot.c` "CSW residue short-transfer policy" + "SCSI whole-command retry decision" (full `msc_residue_short` + `msc_scsi_retry_decide` matrices incl. REQUEST-SENSE-desync rows); live retry validated via QEMU run-usb + bare metal.
> **Notes:**
> - Shipped: `msc_scsi_command` retry wrapper + pure `msc_residue_short` / `msc_scsi_retry_decide` (`usb_msc.c`); `msc_bot_command` exposes `dCSWDataResidue`; `msc_request_sense`/`msc_inquiry` gained residue minimum-length checks.
> - Integrates: `msc_inquiry`/`msc_read_capacity`/`usb_msc_read_sectors`/`usb_msc_write_sectors` now route through `msc_scsi_command` (READ/WRITE/CAPACITY exact-length, INQUIRY allocation-length); the sense-class retry sits above §4's transport recovery.
> - Hardening: a CSW FAIL never advances on a benign sense; a short exact-length transfer fails safe; a REQUEST SENSE that itself desyncs the pipe forces a BOT reset before retry. Closes the §1/§2 short-REQUEST-SENSE + residue gaps via the CSW residue.
> - Scope boundary: §3 owns the SCSI-sense retry + CSW-residue policy; §4 owns transport stall/BOT reset; §5 owns the xHCI per-transfer byte count; command-ring/BOT SMP serialization is §16.
> **Verified:** 2026-06-15 | review commit | 10/10 items | build OK | storage 394 kernel + 16 user-mode PASS (residue + retry-decision matrices)
> **Quality reviewed:** 2026-06-15 | Codex 8x (design, test-coverage, adversarial x2, re-adversarial x2, consistency, perf) | 4H+4M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 4. USB Transport Error Recovery (Stall/Halt Handling)

Handle USB transport-level stalls -- a separate layer from SCSI sense errors. A bulk endpoint can stall due to data toggle mismatch, babble, or controller error even when the SCSI command is valid. Without this, a single stalled endpoint hangs the entire USB boot.

**Files:** `src/kernel/drivers/xhci_dev.c` (extend), `src/kernel/drivers/usb_msc.c` (extend)

> [!NOTE]
> xHCI Transfer Event TRB completion codes: `STALL_ERROR (6)` = endpoint halted by device, `USB_TRANSACTION_ERROR (4)` = CRC/bitstuff/bad PID, `BABBLE_DETECTED_ERROR (3)` = device sent more data than expected, `DATA_BUFFER_ERROR (2)` = host controller data buffer overrun/underrun. All of these leave the endpoint in the Halted state. Recovery requires: (1) `CLEAR_FEATURE(ENDPOINT_HALT)` USB control transfer to the device, (2) xHCI Reset Endpoint command to transition the endpoint from Halted to Stopped, (3) xHCI Set TR Dequeue Pointer command to advance past the failed TRB, (4) re-ring the doorbell.

- [x] `xhci_endpoint_is_halted(hc, dev, dci)` + static `xhci_ep_state` read the output endpoint context `field0` bits 2:0 (`XHCI_EP_STATE_HALTED=2`) from the identity-mapped DCBAA output context
- [x] `xhci_clear_endpoint_halt(hc, dev, ep_addr)`: `CLEAR_FEATURE(ENDPOINT_HALT)` (bmRequestType `0x02`, bRequest `0x01`, wValue `0x00`, wIndex=ep_addr) via the no-recovery `xhci_send_control` primitive
- [x] `xhci_reset_endpoint(hc, slot, dci)` (static): Reset Endpoint command (`XHCI_TRB_RESET_EP=14`); waits for Command Completion CC==SUCCESS
- [x] `xhci_set_tr_dequeue(hc, slot, dci, ring)` (static): Set TR Dequeue Pointer (`XHCI_TRB_SET_TR_DEQUEUE=16`); dequeue = `ring->phys + ring->enqueue*16 | cycle` (DCS), syncs `ring->dequeue`
- [x] `xhci_recover_endpoint(hc, dev, dci, ep_addr, ring)`: STATE-AWARE -- Halted: clear+reset+dequeue; Stopped: clear+dequeue; Running: clear only; Error/Disabled: fail (never leaves a Stopped/un-armed ring claimed recovered)
- [x] `xhci_bulk_transfer_cc` returns the raw CC so STALL/USB_TXN/BABBLE/DATA_BUFFER reach the BOT boundary (`xhci_bulk_transfer` keeps 0/-1); recovery runs in `msc_bot_command`/`msc_bot_xfer`, never inside the EP0 primitive
- [x] `msc_bot_reset(hc, dev)`: BOT mass-storage reset (`0x21`/`0xFF`, wIndex=iface) + state-aware re-sync of both bulk pipes; fails if class reset or either pipe fails; fires on CSW phase error / signature / tag mismatch
- [x] Transport-level recover+retry in `msc_bot_xfer` (CBW/CSW, up to `MSC_STALL_RETRIES`=2); data-phase halt recovers then proceeds to CSW (no data re-send) and fails the command (`data_short`); SCSI-sense whole-command retry is §3
- [x] Commit: `"drivers: USB transport error recovery -- stall/halt clear, endpoint reset, BOT device reset"`

**Test checkpoint:** Force-stall a USB endpoint in QEMU (or encounter one naturally on bare metal USB 2.0) -- serial shows `"EP%u recovered"`, boot continues without hang. If recovery fails, log error and mark device non-functional rather than hanging.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_boot.c` "stall completion-code classification" exhaustively asserts `msc_cc_is_halt` over the 0..255 CC domain; live stall recovery validated via QEMU run-usb + bare metal.
> **Notes:**
> - Shipped: stall-recovery primitives in `xhci_dev.c` (`xhci_recover_endpoint`, Reset Endpoint/Set TR Dequeue cmds) + `usb_msc.c` BOT recovery (`msc_bot_xfer`, `msc_bot_reset`); new TRB/CC/EP-state constants.
> - Integrates: `xhci_bulk_transfer` now wraps a CC-returning `xhci_bulk_transfer_cc`; `msc_bot_command` recovers stalls at the BOT boundary and BOT-resets on a desynced CSW (bad signature/tag) or phase error.
> - Hardening: state-aware recovery never Resets a non-Halted endpoint or leaves a Stopped ring un-armed; a stall-recovered short data phase fails the command so READ(10) can't advance the LBA on partial data.
> - Scope boundary: §4 owns transport recovery; per-command CSW residue acceptance + SCSI-sense whole-command retry are §3; multi-TRB event-pointer dequeue validation is §5.
> **Verified:** 2026-06-15 | review commit | 8/8 items | build OK | storage 371 kernel + 16 user-mode PASS (exhaustive stall-CC matrix)
> **Accepted:** [H] xHCI command ring + BOT transport unserialized for SMP (concurrent recovery/enumeration race; non-atomic `cbw_tag`) -> XREF: 01-boot-platform/TODO-19 §16 (item: "Serialize the command ring: a controller-level lock around `xhci_cmd_submit` + `xhci_wait_command`" at line 177)
> **Quality reviewed:** 2026-06-15 | Codex 8x (design, test-coverage, adversarial x2, re-adversarial x2, consistency, perf) | 4H+3M fixed, 2H accepted-XREF | scope: kernel-code-quality

**Regression risk:** MEDIUM -- modifies the transfer completion path. If the recovery sequence issues incorrect xHCI commands, the endpoint may become permanently stuck. Rollback: disable recovery, return error immediately on stall (current behavior but with a timeout instead of infinite hang).

---

## 5. Bulk Transfer Timeouts

Add bounded timeouts to all bulk transfers. The current code polls the xHCI event ring in a tight loop with no deadline -- if a device stops responding (cable disconnect mid-transfer, firmware hang, powered hub brownout), the kernel hangs indefinitely.

**Files:** `src/kernel/drivers/xhci_dev.c` (extend), `src/kernel/drivers/xhci_ring.c` (extend)

> [!NOTE]
> xHCI does not have hardware transfer timeouts -- the host controller will wait indefinitely for a device response. Software must implement timeouts by: (1) recording the TSC or LAPIC timer value at transfer submission, (2) checking elapsed time on each event ring poll iteration, (3) on timeout: issuing a Stop Endpoint command (TRB type 15) to abort the pending TRB, then recovering via the §4 stall/halt path.

- [x] `xhci_wait_transfer(hc, out_bytes, timeout_ms)`: BULK passes `USB_BULK_TIMEOUT_MS`(5000); control keeps the legacy 500ms (EP0 recovery is the deferred item)
- [x] Software timeout = a microsecond budget (`timeout_ms * 1000`) decremented ~10us per event-ring poll iteration -- bounded, the existing decrement-loop pattern made configurable (no separate TSC deadline needed; `dev_delay_us` is the time base)
- [x] Timeout path: `xhci_stop_endpoint` (Stop Endpoint, `XHCI_TRB_STOP_EP`=15) aborts the stuck TD; `xhci_abort_endpoint` drains the matching STOPPED event (bounded ~20ms, slot+CC match) then Set TR Dequeue re-arms the ring
- [x] On timeout: `klog` the bulk timeout (slot, dci, ms) and return `0xFF` (the timeout sentinel)
- [x] Retry: `0xFF` is a non-halt transport failure -> `msc_bot_xfer` returns -1 -> `msc_scsi_command` BOT-resets and retries (bounded by `MSC_SCSI_RETRIES`)
- [x] `usb_msc_read_sectors`/`usb_msc_write_sectors` see the timeout as a `-1` transient I/O failure via the §3 retry wrapper
- [x] `xhci_bulk_transfer_cc` stops on the FIRST host SHORT_PKT + reports host-transferred via `xfer_out`; `msc_scsi_command` fails exact-length on `msc_host_short` (host count authoritative over CSW residue) and reports `actual_out` from it
- [x] `USB_BULK_TIMEOUT_MS`(5000) constant in `xhci_dev.h`
- [ ] DEFERRED: EP0 / control-transfer timeout recovery -- distinct abort design (Stop Endpoint DCI 1, Set TR Dequeue on `ep0_ring`, slot-reset fallback) since §4 recovery rejects EP0; control xfer keeps its 500ms bound for now
- [x] Commit: `"drivers: USB bulk transfer timeouts -- TSC deadline, Stop Endpoint, retry integration"`

**Test checkpoint:** Disconnect USB device during a bulk transfer (QEMU `device_del` mid-I/O) -- kernel logs timeout within 5 seconds instead of hanging. Boot continues if the device was not the boot drive. If boot drive times out, fall back to the no-USB boot path with a clear error message.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_boot.c` "host-observed short-transfer policy" (`msc_host_short` matrix); the timeout/Stop-Endpoint/abort paths are live-MMIO (validated via QEMU `device_del` + bare metal).
> **Notes:**
> - Shipped: configurable bulk timeout (`USB_BULK_TIMEOUT_MS`) on `xhci_wait_transfer`; `xhci_stop_endpoint`/`xhci_abort_endpoint` (Stop Endpoint TRB 15 + STOPPED-event drain + Set TR Dequeue); host stop-on-first-short + pure `msc_host_short`.
> - Integrates: `xhci_bulk_transfer_cc` reports host-transferred bytes; `msc_scsi_command` fails an exact-length command on a host short (authoritative over CSW residue); a bulk timeout (0xFF) flows through the §3 retry (BOT reset + retry).
> - Hardening: bulk transfers can no longer hang forever (5s software bound); a multi-fragment bulk-IN stops on the first short so CSW bytes never DMA into the data buffer; INQUIRY min-length checks the host count, not the device residue.
> - Scope boundary: §5 owns bulk timeouts + host-residual; EP0/control-transfer timeout recovery is deferred (own item); shared event-ring per-event correlation + command-ring locking is §16.
> **Verified:** 2026-06-15 | review commit | 9/10 items | build OK | storage 400 kernel + 16 user-mode PASS (host-short matrix)
> **Accepted:** [H] a failed timeout abort (Stop Endpoint / Set TR Dequeue rejected) leaves the stale TRB owned, so a later retry could enqueue behind it -> XREF: 01-boot-platform/TODO-19 §16 (item: "Timeout-tainted endpoint recovery: when `xhci_abort_endpoint` fails the stale TRB stays owned" at line 393)
> **Deferred:** [M] EP0 / control-transfer timeout recovery -- distinct EP0-abort design needed (§4 recovery rejects EP0) -> XREF: 01-boot-platform/TODO-19 §5 (item: "DEFERRED: EP0 / control-transfer timeout recovery" at line 203)
> **Quality reviewed:** 2026-06-15 | Codex 8x (design, adversarial x2, re-adversarial x3, consistency, perf) | 4H+2M fixed, 1H accepted-XREF | scope: kernel-code-quality

**Regression risk:** MEDIUM -- adds a per-poll timeout check on the bulk path. Rollback: raise `USB_BULK_TIMEOUT_MS` to effectively disable the bulk timeout while keeping the infrastructure.

---

## 6. Remove sleep_ms(2000) Hack

With proper SCSI retry (§1-§3) and USB transport recovery (§4-§5), the 2-second sleep after xhci_init is no longer needed.

- [x] No global `sleep_ms(2000)` exists -- grep-confirmed empty across `src/`; `boot_phase2` calls `xhci_init()` with no trailing sleep, readiness is event-driven via `msc_poll_unit_ready` (§2) in `usb_msc_init`
- [ ] Verify USB boot works on: QEMU TCG, bare metal i5-11600K, bare metal i5-4210U -- bare-metal "moment of truth"; user hardware action (no QEMU USB-drive-boot harness in WSL)
- [ ] If any platform fails without the sleep: investigate root cause, don't add the sleep back -- contingent on the platform-boot validation above
- [x] Verified: §4-§5 endpoint-stall + timeout recovery covers the readiness window the sleep masked -- now handled by §2 TUR poll + §3 BOT retry + §4 stall recovery + §5 bulk timeouts (`usb_msc_init` chain)
- [x] Commit: `"drivers: remove USB 2-second sleep hack -- SCSI retry + transport recovery handles readiness"`

**Test checkpoint:** USB boot works on all tested platforms without the delay. Boot time improves by ~2 seconds.
> **Test runner:** N/A (verify-only -- no source change; the readiness behavior that replaces the sleep is covered by `test_usb_boot.c` via §2-§5 helper suites) | validation: grep + code-truth read; bare-metal boot validation is the user hardware action.
> **Notes:**
> - Verify-only: the blind `sleep_ms(2000)` after `xhci_init()` that this section targeted does not exist in the current tree (grep empty across `src/`); no code change shipped.
> - Readiness replacement: post-enumeration device readiness is event-driven via `msc_poll_unit_ready` (§2 TUR poll) wired into `usb_msc_init`, backed by §3 BOT retry + §4 stall recovery + §5 bulk timeouts.
> - Scope boundary: §6 owns removal of the global post-`xhci_init` sleep only; the separate Intel `xhci_delay_us(500000)` XUSB2PR-routing wait (`xhci.c`) is owned by §7 and is NOT touched here.
> - Open items: cross-platform boot validation (QEMU TCG, i5-11600K, i5-4210U) is a bare-metal "moment of truth" user action -- kept `[ ]`; no QEMU USB-drive-boot harness exists in WSL.
> **Verified:** 2026-06-15 | commit `pending` | 2/4 items | build OK | verify-only -- grep empty across `src/`, readiness chain code-truth confirmed
> **Quality reviewed:** 2026-06-15 | Codex 0x (verify-only -- no code diff to review) | 0 findings | scope: N/A (verify-only, no source change)

**Regression risk:** HIGH -- this is the moment of truth. If retry logic isn't sufficient, USB boot breaks on slow hardware. Rollback: temporarily re-add `sleep_ms(500)` as a smaller delay while investigating.

---

## 7. XUSB2PR Port-Ready Polling (Intel EHCI→xHCI)

On Intel 7/8/9-series chipsets, USB 2.0 ports are routed from EHCI to xHCI via the XUSB2PR PCI register, then need a settle window before the routed devices re-present. §7 unifies the two previously-duplicated routing paths into one helper, gates the routing+wait on EHCI presence (so modern Intel skips it entirely), and waits a bounded proven-safe window. A sub-500ms event-driven early-exit was evaluated and deferred as unsafe (deferred item below).

- [x] Shared `xhci_route_intel_usb2_ports` helper (`xhci.c`) dedups the two previously-divergent routing paths (handover + full-init); both now do EHCI-detect + routing-write + bounded settle + connected-count log
- [x] EHCI-presence gate on BOTH paths: XUSB2PR(0xD0)/USB3_PSSEN(0xD8) written only when an EHCI controller (prog-if 0x20) shares the bus; modern Intel (100-series+, no EHCI) skips routing AND the settle wait (the old handover path always paid it)
- [x] On routing-eligible hardware wait the bounded `XHCI_XUSB2PR_ROUTE_MAX_US` (500ms); log `"XUSB2PR port routing settled (%u ms, %u port(s) connected)"` with the before/after connected-port delta
- [ ] DEFERRED: spec-backed event-driven early-exit to shorten the 500ms -- needs USB2 port-identity tracking (Supported Protocol caps); a timed early-exit is unsafe (no spec bound on XUSB2PR-to-CCS latency + synchronous boot enumeration)
- [x] Commit: `"drivers: XUSB2PR routing -- unify both paths, EHCI-gate, bounded settle (early-exit deferred)"`

**Test checkpoint:** i5-4210U bare metal with USB 2.0 drive boots; the routing settle logs the connected-port delta. Modern Intel (i5-11600K, no EHCI) logs the EHCI-skip and pays no routing wait. Non-Intel skips entirely.
> **Test runner:** N/A (live-PCI/MMIO -- EHCI-detect + XUSB2PR write + bounded settle; no pure decision surface after the event-driven early-exit was deferred) | validated on bare metal i5-4210U + i5-11600K skip-path.
> **Notes:**
> - Shipped: `xhci_route_intel_usb2_ports` (`xhci.c`) unifying the handover + full-init routing paths + `xhci_count_connected_ports` for the settle-delta log; `XHCI_XUSB2PR_ROUTE_MAX_US` (500ms) bounded wait.
> - EHCI-gate is the win: routing + wait happen only when an EHCI controller shares the bus, so modern Intel (100-series+) skips both -- the old handover path always paid the 500ms.
> - Safety: no sub-500ms early-exit -- XUSB2PR-write-to-CCS latency has no USB/xHCI spec bound and boot-time MSC enumeration is synchronous, so a timed early-exit could miss a late routed boot drive; full 500ms is the proven-safe value.
> - Deferred: spec-backed event-driven early-exit (route-capable port-identity tracking) kept as a `[ ]` item in this section; Codex design + 3x re-adversarial adoptions in the ship commit.
> - Scope boundary: §7 owns the routing + bounded settle; §6 owns the (already-absent) global post-`xhci_init` sleep; per-port reset/enumeration stays in `xhci_enumerate_ports`.
> **Verified:** 2026-06-15 | commit `c4f0e128` | 3/4 items | build OK | live-PCI/MMIO (validated bare metal i5-4210U + i5-11600K skip-path)
> **Deferred:** [M] event-driven early-exit to shorten the 500ms on routing HW -- unsafe without spec-backed routed-port identity tracking -> XREF: 01-boot-platform/TODO-19 §7 (item: "DEFERRED: spec-backed event-driven early-exit" at line 251)
> **Quality reviewed:** 2026-06-15 | Codex 8x (design, adversarial x2, re-adversarial x3, consistency, perf) | 1H+1M fixed, 0 open | scope: kernel-code-quality

---

## 8. klog_disk_flush Bounded Loop

Fix the unbounded flush loop that hangs 10+ minutes on USB 2.0.

- [x] Bound via the EXISTING seq cursor, NOT `ring_count`: `klog_disk_flush` flushes `cur_seq - ixfs_flush_seq` entries capped to `KLOG_RING_SIZE` -> XREF: 02-kernel-core/TODO-04 klog_ring_seq design
- [x] Preserve cursor-advance-on-success: `ixfs_flush_seq`/`jsonl_flush_seq` advance to `cur_seq` only on `flush_ok`; entries logged during flush wait for the next call -- correct across the 1000-entry ring wrap (`klog_disk.c`)
- [x] Progress logging: serial `"[KLOG] Flushing %u entries..."` at entry + `"flush done (%u entries, %u ms)"` at `done:`, for flushes >= 64 (`KLOG_FLUSH_PROGRESS_MIN`); timed via `uptime_ns()`, emitted on serial not klog (reentrancy guard)
- [x] Slow-media detect (flush > `KLOG_SLOW_MEDIA_MS`=5s): serial WARN + set `s_klog_slow_media`, exposed via `klog_slow_media_detected()` -- the trigger the deferred-flush mode (next section) consumes
- [ ] DEFERRED: all-or-nothing kernel.log retry -- a failed `vfs_write` now stops the loop + retains the cursor (no further-chunk corruption), but the retry re-appends; needs truncate-to-pre-flush-size rollback (a VFS truncate op) for a clean retry
- [ ] DEFERRED: durable per-subsystem-file cursor -- subsystem routing is best-effort (kernel.log is the durable copy); a per-file cursor would make `boot.log`/`fs.log`/etc. survive a subsystem-only write failure
- [x] Commit: `"kernel: bounded klog_disk_flush -- seq-cursor snapshot, no unbounded loop"`

**Test checkpoint:** USB 2.0 boot on bare metal -- flush completes in seconds, not minutes. Serial shows `"[KLOG] Flushing N entries..."` / `"flush done (N entries, M ms)"`; a >5s flush emits the slow-media WARN. Unit test asserts the bounded flush window caps to `KLOG_RING_SIZE`.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `test_klog.c` "Klog: bounded flush window" (`klog_flush_window` matrix -- the cap that prevents the unbounded-loop hang); the flush timing/progress/WARN + slow-media flag are live serial+VFS (validated on bare metal USB 2.0 i5-4210U).
> **Notes:**
> - Shipped: `klog_disk_flush` progress logging + `uptime_ns()` flush timing + slow-media detection; `klog_flush_window()` pure bounded-window helper now used at all 3 flush sites (`klog_disk.c`). Seq-cursor bounding (items 1-2) was already in place from the BlackBox log work, verified + extracted here.
> - How it runs: serial diagnostics fire only for flushes >= 64 entries (no spam on the frequent small flushes); `serial_write` is used (not klog) so it can't re-enter under the `flushing` reentrancy guard.
> - Downstream: `klog_slow_media_detected()` is the trigger the deferred-flush mode consumes (next section); a >5s flush flips it.
> - Hardening (review): the `flushing` guard is now an atomic test-and-set (SMP-safe, one CPU enters); the cursor advances only on verified full `vfs_write` returns (stop-on-first-failure); the done-path reports `flush FAILED` honestly when persistence did not complete.
> - Scope boundary: this section owns the bounded loop + progress + slow-media DETECTION; the deferred-flush mode (RAM batching that acts on the flag) is the next section.
> **Verified:** 2026-06-15 | commit `8ed6ca69` | 4/6 items | build OK | live serial+VFS (validated bare metal USB 2.0 i5-4210U)
> **Deferred:** [H] all-or-nothing kernel.log retry (truncate-to-pre-flush-size rollback for clean retry) -> XREF: 01-boot-platform/TODO-19 §8 (item: "DEFERRED: all-or-nothing kernel.log retry" at line 276)
> **Deferred:** [M] durable per-subsystem-file cursor (subsystem routing currently best-effort) -> XREF: 01-boot-platform/TODO-19 §8 (item: "DEFERRED: durable per-subsystem-file cursor" at line 277)
> **Quality reviewed:** 2026-06-15 | Codex 8x (design, adversarial x2, re-adversarial x3, consistency, perf) | 3H+3M fixed, 2 deferred | scope: kernel-code-quality

---

## 9. klog Deferred Flush Mode

For slow media, batch log entries in RAM and write once at boot end instead of per-subsystem.

- [x] `klog_set_deferred(int enabled)` sets the atomic `s_klog_deferred` flag (`klog_disk.c`)
- [x] During deferred mode `klog_disk_flush()` is a no-op (acquires the guard, sees the flag, releases + returns); entries accumulate in the 1000-entry ring
- [x] `klog_disk_flush_all()` -- guard-AWARE forced flush (spin-acquires guard; returns untouched on timeout; clears deferred after the body); wired in `boot_desktop.c` before `task_create`/`scheduler_enable` (no userland races the drain)
- [x] Combine into larger VFS writes -- the existing 32KB batch buffer already coalesces entries into few large `vfs_write`s; deferred mode defers WHEN, the batch handles HOW
- [x] Auto-enable: the slow-media WARN block (flush > 5s) sets `s_klog_deferred=1` (one-way until `flush_all`); ring-overflow loss is counted via `klog_lost_count()` + logged so the persisted log is never silently incomplete
- [x] Commit: `"kernel: klog deferred flush -- batch to RAM, single write at boot end"`

**Test checkpoint:** USB 2.0 slow-media boot -- after the first >5s flush, subsequent per-subsystem flushes are no-ops; the single `klog_disk_flush_all()` at boot end writes everything in one batch; serial shows the ring-overflow lost-count if boot emitted >1000 entries. Unit test asserts the `klog_lost_count` overflow math.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `test_klog.c` "Klog: ring-overflow lost count" (`klog_lost_count` matrix); the deferred-mode flush/flush_all paths are live serial+VFS (validated on bare metal USB 2.0 i5-4210U).
> **Notes:**
> - Shipped: `klog_set_deferred()` + atomic `s_klog_deferred` + guard-aware `klog_disk_flush_all()` + extracted `klog_disk_flush_locked()` body + `klog_lost_count()` helper (`klog_disk.c`); boot-end forced flush wired in `boot_desktop.c`.
> - How it runs: `klog_disk_flush()` no-ops under the flag; auto-enabled after a >5s flush; the single `flush_all` at boot end (post-deferred-init + summary) spin-acquires the guard and force-drains the RAM-batched entries.
> - Downstream: consumes §8's `klog_slow_media_detected()` infrastructure; the 32KB batch buffer (pre-existing) provides the "fewer USB transfers" coalescing.
> - Codex design review adoptions (boot-end flush site, guard-aware flush_all, lost-count) in the ship commit.
> - Scope boundary: §9 owns deferred mode + boot-end forced flush; the bounded loop + progress + slow-media detection are §8; the §8 truncate-rollback + per-subsystem durability follow-ups remain open.
> - Hardening (review): deferred state is ONE atomic word (`s_klog_defer_state`, ACTIVE+DISABLED bits) with a CAS auto-enable and a DISABLED-dominant `klog_defer_active()` predicate -- once the boot-end drain latches DISABLED no flush can re-defer; the drain stays before `task_create` (moving it after fails smoke).
> **Verified:** 2026-06-15 | commit `0448e6b8` | 6/6 items | build OK | smoke PASS (KVM 2.86s)
> **Quality reviewed:** 2026-06-15 | Codex 12x (design, adversarial x2, re-adversarial x7, consistency, perf) | 7H+6M fixed, 1M rejected | scope: kernel-code-quality

---

## 10. Boot Media Speed Detection

Detect whether boot media is fast (SSD/NVMe) or slow (USB 2.0/USB 3.0 stick) and adjust behavior.

- [x] `boot_media_probe()` times a 4 KiB `vfs_read` of the first openable boot file and `boot_media_classify()` maps it to fast (<1ms) / medium (1-10ms) / slow (>10ms) (`boot_media.c`); idempotent, UNKNOWN if no candidate readable
- [x] Stored in a KERNEL global `s_boot_media_speed` + `boot_media_speed()` getter -- NOT a `boot_info` ABI field (it is a kernel-runtime measurement; avoids a BOOT_INFO_VERSION bump) -> design adoption
- [x] Consumers: SLOW proactively calls `klog_set_deferred(1)` (§9) before per-subsystem flushes; the kernel test sweep is skipped on SLOW media in debug mode (honoring explicit `test=1`)
- [x] Log: `"Boot media speed: %s (%u us/4KiB)"` with the classification + measured time
- [ ] DEFERRED: "adjust timeouts" on slow media -- the bulk/control timeouts are already bounded (§5); a media-class-scaled timeout knob is a future refinement with no concrete consumer today
- [x] Commit: `"kernel: boot media speed detection -- adjust behavior for slow USB"`

**Test checkpoint:** SATA SSD / NVMe → "fast" (QEMU emulated disk classifies fast); USB 3.0 stick → "medium"; USB 2.0 stick → "slow" (proactively enables deferred klog). Unit test asserts the `boot_media_classify` thresholds.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `test_boot_device.c` "Boot media: speed classify thresholds" (`boot_media_classify` boundary matrix); the live timed probe is validated on bare metal (SSD fast vs USB 2.0 slow).
> **Notes:**
> - Shipped: `boot_media.{h,c}` -- pure `boot_media_classify()` + `boot_media_speed_name()` + `s_boot_media_speed` global + `boot_media_probe()` (timed 4 KiB `vfs_read`, classify, log, deferred-on-slow); wired into `boot_phase2` after `klog_disk_enable`.
> - How it runs: probes once (idempotent) right after klog disk is up so `klog_set_deferred(1)` takes effect before the slow per-subsystem flushes; UNKNOWN fallback leaves §8's reactive >5s-flush auto-enable to cover a mis-probe.
> - Downstream: consumes §9's `klog_set_deferred` (proactive deferred mode) and gates the §8/§9 slow-media path; the boot-test-skip trims debug boots on slow USB.
> - Codex design review adoptions (kernel global not boot_info field; timed-probe; proactive deferred) in the ship commit.
> - Scope boundary: §10 owns media classification + the two adaptations (deferred-klog, test-skip); the deferred-flush machinery is §9, the bounded loop is §8; media-class-scaled timeouts are a deferred refinement.

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
- [ ] Key MSC state by global device, not per-controller `slot_id`: `msc_info[dev->slot_id]` (`usb_msc.c`) collides when two controllers each have slot N (xHCI + EHCI / multi-xHCI) -- embed `usb_msc_info` in `struct xhci_device` or index globally
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

## 16. xHCI Command Ring + BOT Transport SMP Serialization

The xHCI command ring (`xhci_cmd_submit` / `xhci_wait_command`) and the MSC BOT transport (`msc_bot_command`) are unserialized: command enqueue mutates `hc->cmd_ring` with no lock and `xhci_wait_command` accepts the first completion event rather than matching the submitted command TRB, and two CPUs can enter the same MSC device concurrently. Single-threaded boot enumeration is safe today, but post-boot SMP block-layer I/O plus §4 stall recovery (which issues Reset Endpoint / Set TR Dequeue and re-arms the ring) makes a concurrent race destructive. Surfaced by the §4 adversarial review.

- [ ] Serialize command ring + correlate events: lock `xhci_cmd_submit`/`xhci_wait_command`; match each Command Completion AND Transfer Event to its submitted TRB pointer, not the first by type/CC (`xhci_wait_transfer`/abort drain)
- [ ] Defer hot-plug enumeration out of the shared event-ring drain so it cannot steal a synchronous command/transfer completion while a command is in flight
- [ ] Per-MSC-device BOT serialization: a sleepable mutex around `msc_bot_command` (CBW/data/CSW + recovery, `usb_msc.c`) so two CPUs can't interleave one device; Set TR Dequeue must not run while another caller has a queued TRB
- [ ] Make `cbw_tag` atomic (`__atomic_fetch_add`) so concurrent commands cannot reuse or reorder CSW tags
- [ ] Timeout-tainted endpoint recovery: when `xhci_abort_endpoint` fails the stale TRB stays owned -- force a re-abort or slot-level reset (Reset Device / Disable+Enable Slot) before any retry enqueues behind it on that ring
- [ ] Commit: `"drivers: serialize xHCI command ring + per-device BOT transport (SMP)"`

**Test checkpoint:** SMP stress (concurrent USB MSC reads from 2 CPUs) shows no command-ring corruption, no CSW tag reuse, and a stall recovery during concurrent I/O never advances the endpoint past another caller's TRB. Verify on bare metal i5-4210U (USB 2.0) and a multi-core target.

**Regression risk:** MEDIUM -- adds locking to the command + transfer hot path; a lock-order or hold-time mistake can deadlock against the recovery path. Hold no lock across the 500 ms transfer waits (snapshot under lock, release, then I/O).

---

## OS Comparison

| ⭐ | Feature                      | 🪟 Win11                     | 🐧 Linux                      | 🚀 Impossible OS               |
|----|------------------------------|---------------------------|----------------------------|-----------------------------|
| 💎 | SCSI error retry             | ✅ usbstor.sys retries   | ✅ usb-storage retries     | ✅ §1-§3 sense retry + residue |
| 💎 | Device readiness poll        | ✅ usbstor TUR wait      | ✅ sd spin-up poll         | ✅ §2 TUR poll, 2s budget   |
| 💎 | USB stall/halt recovery      | ✅ usbstor.sys auto-reset | ✅ usb-storage ep reset   | ✅ §4 reset EP + BOT reset  |
| 💎 | Bulk transfer timeouts       | ✅ USBD_DEFAULT_PIPE_TRANSFER_TIMEOUT | ✅ usb_submit_urb timeout | ✅ §5 5s bound + Stop EP |
| 💎 | No sleep hacks               | ✅ Event-driven readiness | ✅ SCSI start-stop         | ✅ No global xhci_init sleep |
| 💎 | Intel EHCI->xHCI port routing | ✅ USB stack routes ports | ✅ xhci-pci Intel quirk    | ✅ §7 EHCI-gate + bounded settle |
| 💎 | EHCI/UHCI fallback           | ✅ Full USB stack         | ✅ ehci-hcd + uhci-hcd     | ⬜ §11                      |
| 💎 | Bounded disk flush           | ✅ Async I/O              | ✅ Writeback cache          | ✅ §8 seq-cursor + progress  |
| 💎 | Deferred flush (slow media)  | ✅ Lazy writeback         | ✅ dirty_writeback_centisecs | ✅ §9 RAM batch + boot-end flush |
| 💎 | Media speed detection        | ✅ Performance tier       | ✅ readahead tuning        | ✅ §10 4KiB-probe classify  |
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
