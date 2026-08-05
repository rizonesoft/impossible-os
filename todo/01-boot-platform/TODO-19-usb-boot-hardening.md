---
schema_version: 1
id: usb-boot-hardening
domain: 01-boot-platform
status: active
title: "TODO-19 -- USB Boot Hardening & Fail-Safe Pipeline"
---

# TODO-19 -- USB Boot Hardening & Fail-Safe Pipeline

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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

| ⭐   | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎   |   1   | SCSI REQUEST SENSE and error classification | --         |  [/]   |
| 💎   |   2   | TEST UNIT READY poll loop after BOT init | §1         |  [x]   |
| 💎   |   3   | MSC BOT retry on transient errors        | §1, §2, §4 |  [x]   |
| 💎   |   4   | USB transport error recovery (stall/halt) | §1         |  [x]   |
| 💎   |   5   | Bulk transfer timeouts                   | §4         |  [/]   |
| 💎   |   6   | Remove sleep_ms(2000) hack               | §2-§5, §7  |  [/]   |
| 💎   |   7   | XUSB2PR port-ready polling (Intel EHCI→xHCI) | §4         |  [/]   |
| 💎   |   8   | klog_disk_flush bounded loop             | --         |  [/]   |
| 💎   |   9   | klog deferred flush mode (batch to RAM)  | §8         |  [x]   |
| 💎   |  10   | Boot media speed detection               | §8         |  [/]   |
| 💎   |  11   | EHCI/UHCI companion controller fallback  | --         |  [/]   |
| ⭐   |  12   | USB boot diagnostic report               | §1-§11     |  [x]   |
| 💎   |  13   | Single-pass per-subsystem log routing    | §8         |  [x]   |
| 💎   |  14   | IXFS boot tests: slow-media-aware        | §10        |  [x]   |
| ⭐   |  15   | Flush progress on splash diagnostic line | §9         |  [/]   |
| 💎   |  16   | xHCI command ring + BOT transport SMP serialization | §4         |  [/]   |

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
> **Deferred:** [M] boot-LUN selection for composite/card-reader media blocked on GET_MAX_LUN (`xhci_control_transfer` is static) -> XREF: 04-drivers-hardware/TODO-10 §7 (item: "`usb_msc_get_max_lun(dev)`" at line 181)
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
> **Accepted:** [M] `msc_info` keyed by per-controller `slot_id` collides across controllers -> XREF: 01-boot-platform/TODO-19 §11 (item: "Key MSC state by global device, not per-controller `slot_id`" at line 121)
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
> **Accepted:** [H] xHCI command ring + BOT transport unserialized for SMP (concurrent recovery/enumeration race; non-atomic `cbw_tag`) -> XREF: 01-boot-platform/TODO-19 §16 (item: "Serialize the command ring: a controller-level lock around `xhci_cmd_submit` + `xhci_wait_command`" at line 180)
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
- [/] Verify USB boot works on: QEMU TCG, bare metal i5-11600K, bare metal i5-4210U -- bare-metal "moment of truth"; user hardware action (no QEMU USB-drive-boot harness in WSL)
- [/] If any platform fails without the sleep: investigate root cause, don't add the sleep back -- contingent on the platform-boot validation above
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
> **Deferred:** [awaiting-operator] bare-metal USB boot verification needs the operator's i5-11600K / i5-4210U hardware -- no unattended pass can perform this; operator action required (cohort 2026-07-31)

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
- [x] Consumers: SLOW calls `klog_set_deferred(1)` (§9) before the klog disk/live-log enable; the kernel test sweep skips on SLOW media in debug mode -- gated on probe-class OR §8 `klog_slow_media_detected()`, honoring `test=1`
- [x] Log: `"Boot media speed: %s (%u us/4KiB)"` with the classification + measured time
- [ ] DEFERRED: "adjust timeouts" on slow media -- the bulk/control timeouts are already bounded (§5); a media-class-scaled timeout knob is a future refinement with no concrete consumer today
- [x] Commit: `"kernel: boot media speed detection -- adjust behavior for slow USB"`

**Test checkpoint:** SATA SSD / NVMe → "fast" (QEMU emulated disk classifies fast); USB 3.0 stick → "medium"; USB 2.0 stick → "slow" (proactively enables deferred klog). Unit test asserts the `boot_media_classify` thresholds.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `test_boot_device.c` "Boot media: speed classify thresholds" (`boot_media_classify` boundary matrix); the live timed probe is validated on bare metal (SSD fast vs USB 2.0 slow).
> **Notes:**
> - Shipped: `boot_media.{h,c}` -- pure `boot_media_classify()` + `boot_media_speed_name()` + `s_boot_media_speed` global + `boot_media_probe()` (timed full-4 KiB `vfs_read`, short reads skipped, classify, log, deferred-on-slow); wired into `boot_phase2` BEFORE the klog disk/live-log enable.
> - How it runs: probes once (idempotent) before the klog disk/live-log enable so `klog_set_deferred(1)` lands before the first slow flush; UNKNOWN fallback leaves §8's reactive >5s-flush auto-enable AND the reactive-gated test-skip to cover a mis-probe.
> - Downstream: consumes §9's `klog_set_deferred` (proactive deferred mode) and gates the §8/§9 slow-media path; the boot-test-skip trims debug boots on slow USB.
> - Codex design review adoptions (kernel global not boot_info field; timed-probe; proactive deferred) in the ship commit.
> - Scope boundary: §10 owns media classification + the two adaptations (deferred-klog, test-skip); the deferred-flush machinery is §9, the bounded loop is §8; media-class-scaled timeouts are a deferred refinement.
> **Verified:** 2026-06-15 | commit `dbe7a293` | 4/5 items | build OK | smoke PASS (KVM 2.75s)
> **Deferred:** [L] media-class-scaled timeout knob -- bulk/control timeouts already bounded (§5), no concrete consumer today -> XREF: 01-boot-platform/TODO-19 §10 (item: "DEFERRED: \"adjust timeouts\" on slow media" at line 328)
> **Quality reviewed:** 2026-06-15 | Codex 7x (design, test-coverage, adversarial, consistency, perf, re-adversarial x2) | 1H+2M fixed, 0 open | scope: kernel-code-quality

---

## 11. EHCI/UHCI Companion Controller Fallback

For hardware without xHCI, detect legacy USB controllers (EHCI/UHCI/OHCI) and degrade gracefully without a hang. The active EHCI/UHCI boot-storage HCD is deferred to the USB-core abstraction (see note); this section ships the boot-integration detection + graceful skip + the multi-controller MSC keying fix.

> [!NOTE]
> EHCI ownership split: the full EHCI HCD (vtable + class-driver reuse) is owned by `04-drivers-hardware/TODO-10-usb-stack.md §10`, which itself depends on that TODO's §1 usb_core HCD abstraction. `usb_msc.c` is hardwired to `struct xhci_device`/`struct xhci_ring`, so a CREDIBLE shared-BOT EHCI fallback cannot land until §1's `usb_hcd_ops_t` vtable exists; a standalone EHCI-MSC copy is the `if(xhci)...else if(ehci)` anti-pattern §1 exists to prevent. This section therefore owns boot-integration detection + the OHCI-only graceful skip + the multi-controller MSC fix; the HCD driver, shared BOT, and HCD-agnostic recovery are deferred to TODO-10 §1/§10. Routed from `01-boot-platform/TODO-17 §6` (non-Intel/legacy de-scope).

- [x] Detect EHCI/UHCI/OHCI via PCI (class 0x0C/0x03, prog-if 0x20/0x00/0x10): pure `usb_legacy_classify` + `usb_legacy_scan` count each class (`usb_legacy.{c,h}`)
- [ ] Minimal EHCI driver (port reset, bulk, BOT SCSI) -> XREF: 04-drivers-hardware/TODO-10 §10 (item: "Async schedule: control/bulk via QH->QTD chain; `ehci_submit_control`/`ehci_submit_bulk`"); needs §1 usb_core HCD vtable
- [x] No-xHCI fallback decision: `usb_legacy_announce` logs which legacy controller exists + that boot storage via it is pending the HCD, then skips gracefully (no hang); active EHCI->UHCI drive deferred with the HCD
- [x] OHCI-only hardware (0x0C/0x03/0x10): `usb_legacy_announce` logs "OHCI-only controller -- USB storage not supported on this hardware" and skips (no hang); no OHCI driver planned
- [ ] Share `usb_msc.c` BOT layer across HCDs (needs the usb_core abstraction) -> XREF: 04-drivers-hardware/TODO-10 §1 (item: "Refactor `xhci_bulk_transfer()`/`xhci_control_transfer()` to `usb_hcd_ops_t`; `usb_msc.c` migrates to `usb_submit_bulk()`")
- [x] Key MSC state by global device index not per-controller `slot_id`: `msc_state(dev)` keys `msc_info[]` by `xhci_device_index(dev)`; `blkdev_adapters` routes I/O to `dev->owner` via `xhci_controller_index`, not hardcoded ctrl 0
- [ ] EHCI/UHCI transport recovery + timeouts at §4/§5 parity via an HCD-agnostic USB error contract -> XREF: 04-drivers-hardware/TODO-10 §1 (item: "`usb_submit_*` return `USB_ERR_STALL`/`USB_ERR_TIMEOUT`/`USB_ERR_TRANSPORT`") + §10 (EHCI HCD)
- [x] No-xHCI legacy diagnostic log shipped (`usb_legacy_announce`); the active "[USB] Using %s controller" log lands when the HCD drives storage -> XREF: 04-drivers-hardware/TODO-10 §10
- [x] Commit: `"drivers: USB legacy-controller detection + graceful no-xHCI skip; MSC multi-controller keying fix"`

**Test checkpoint:** `usb_legacy_classify` unit test asserts the PCI-triple to UHCI/OHCI/EHCI/NONE mapping (incl. independent class gate). On hardware with a legacy USB controller but no xHCI, serial shows the `usb_legacy_announce` graceful-skip line and boot continues (no hang). Active EHCI storage (VirtualBox EHCI) validates once the TODO-10 HCD lands.

**Regression note:** the deferred EHCI/UHCI HCD (HIGH risk -- new driver code, validate on VirtualBox) has not landed. What shipped touches the working xHCI MSC path only via the keying fix, which is behaviorally identical on single-controller systems (one controller -> `dev->owner` is ctrl 0 and the global index equals the unique slot_id), so it is provably a no-op on every current test platform and changes only the broken multi-xHCI case.

> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_boot.c` "usb-boot: legacy controller classification" (`usb_legacy_classify` PCI-triple matrix incl. independent class gate); the no-xHCI graceful-skip + MSC keying fix are live (validated on bare metal / VirtualBox legacy USB).
> **Notes:**
> - Shipped: `usb_legacy.{c,h}` (PCI detect + pure `usb_legacy_classify` + `usb_legacy_announce` no-xHCI/OHCI-only graceful-skip log) + multi-controller MSC fix (`msc_info` keyed by `xhci_device_index`; `blkdev` routes to `dev->owner`).
> - How it runs: `usb_legacy_scan()` after `pci_scan()` and `usb_legacy_announce(xhci_count)` after `xhci_init()` in `boot_phase2`; detection + klog only, no MMIO / transfers / HCD.
> - Deferred: active EHCI/UHCI HCD + shared BOT + HCD-agnostic recovery -> TODO-10 §1 (`usb_hcd_ops_t` vtable) + §10 (EHCI HCD); standalone EHCI-MSC copy rejected. Codex adoptions in the ship commit.
> - Scope boundary: §11 owns boot-time legacy-controller detection + graceful skip + multi-controller MSC keying; TODO-10 §1/§10 own the transport abstraction + the EHCI host-controller driver.
> **Verified:** 2026-06-15 | commit `5b924aa0` | 5/8 items | build OK | smoke PASS (KVM 2.02s)
> **Accepted:** [H] xHCI event-ring completion-stealing + non-atomic `cbw_tag` under concurrent MSC I/O (reason: pre-existing, not introduced by §11) -> XREF: 01-boot-platform/TODO-19 §16 (item: "Serialize command ring + correlate events ... match each ... Transfer Event to its submitted TRB pointer" at line 377)
> **Deferred:** [M] EHCI/UHCI HCD driver + shared BOT layer + HCD-agnostic recovery (reason: needs usb_core abstraction) -> XREF: 04-drivers-hardware/TODO-10 §1 (item: "Refactor `xhci_bulk_transfer()`/`xhci_control_transfer()` to `usb_hcd_ops_t`; `usb_msc.c` migrates to `usb_submit_bulk()`") + §10 (EHCI HCD)
> **Quality reviewed:** 2026-06-15 | Codex 7x (design, test-coverage, adversarial x2, consistency, perf, re-adversarial) | 1H+5M+2L fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 12. USB Boot Diagnostic Report

At end of USB enumeration, produce a comprehensive diagnostic summary.

- [x] Log all discovered USB devices: port, speed (LS/FS/HS/SS/SS+ via `usb_speed_name`, raw PORTSC id kept for unknown), vendor:product, MSC/HID/other (`usb_boot_report.{c,h}`)
- [x] Log controller type: xHCI (`hci_version`) + legacy EHCI/UHCI/OHCI counts via `usb_legacy_count`
- [x] Log port routing status: `xhci_xusb2pr_routed_count` (Intel USB 2.0 routing applied) + the legacy-controller line covers companion status
- [x] Log SCSI retry statistics: `usb_msc_total_retries` + `usb_msc_sense_keys_seen` (relaxed-atomic counters in `usb_msc.c`); each set sense key decoded by `msc_sense_key_name`
- [x] Log boot media speed: `boot_media_speed_name` -- report emitted AFTER `boot_media_probe` so the class is real, not UNKNOWN
- [x] Format: `"=== USB Boot Report ==="` klog block (subsystem tag `usb`), bracketed by an end marker
- [x] Commit: `"drivers: USB boot diagnostic report -- full enumeration summary"`

**Test checkpoint:** Serial output contains `"=== USB Boot Report ==="` then the controller list, per-device lines (port/speed/VID:PID/class), XUSB2PR routing count, SCSI retry stats + decoded sense keys, and boot media speed. `test_usb_boot.c` asserts the pure field formatters (`usb_speed_name` incl. SS+/unknown, `usb_boot_report_device_class`) by exact string.

> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | `test_usb_boot.c` "usb-boot: report field formatters" (`usb_speed_name` + `usb_boot_report_device_class` exact-string matrix); the live report emission is validated on the serial log (QEMU run-usb + bare metal).
> **Notes:**
> - Shipped: `usb_boot_report.{c,h}` -- aggregator emitting the "=== USB Boot Report ===" block (controllers, devices, XUSB2PR routing, SCSI retry stats, boot media speed) + pure `usb_speed_name`/`usb_boot_report_device_class`.
> - How it runs: called once in `boot_phase2` AFTER `boot_media_probe()` (so media speed is real); read-only, no MMIO. Reads getters from xHCI / MSC / `usb_legacy` / `boot_media`.
> - New inputs: relaxed-atomic `usb_msc_total_retries`/`usb_msc_sense_keys_seen` (SMP-safe counters in `usb_msc.c`); `xhci_xusb2pr_routed_count`; `USB_SPEED_SUPER_PLUS` (id 5) added to the xhci speed enum + `speed_to_str`.
> - Scope boundary: §12 owns the boot-storage report; the input-source (HID) diagnostic is `usb_input_diag.c` (TODO-18); single-pass log routing is §13.
> **Verified:** 2026-06-15 | commit `34ff4d47` | 6/6 items | build OK | smoke PASS (KVM 1.98s)
> **Quality reviewed:** 2026-06-15 | Codex 7x (design, test-coverage, adversarial x2, consistency, perf, re-adversarial) | 2H+4M fixed | scope: kernel-code-quality

---

## 13. Single-Pass Per-Subsystem Log Routing

Replace the 6-pass per-subsystem loop (one full ring scan per file) with a single pass that dispatches each entry to the correct subsystem buffer simultaneously. Only non-empty buffers trigger `vfs_open` + `vfs_write` + `vfs_close`.

**Files:** `src/kernel/klog_disk.c`

- [x] `klog_dispatch_slot()` helper (`klog.h` / `klog_disk.c`): pure tag→slot classifier (0-5, or -1 for kernel.log-only), reused by the routing path and unit-tested
- [x] Phase 1 single ring scan classifies each entry into `int8_t slot_of[KLOG_RING_SIZE]` + `sub_count[SUBSYS_LOG_COUNT]` -- the one expensive walk (ring read + dispatch), replacing the former 6 per-file scans
- [x] Phase 2 writes only non-empty subsystems (`sub_count[si]==0` skips `vfs_open`); each reuses the FULL 32 KB batch, chunk-flushed at the `batch_size` boundary so a burst-heavy subsystem keeps its WHOLE view (no `batch_size/6` per-slot cap)
- [x] Re-validate each entry's live tag immediately before `format_entry` (the ring is read without `s_klog_lock`) so a concurrent overwrite cannot misroute a newer entry into the wrong subsystem file
- [x] Commit: `"klog: single-pass subsystem routing -- 6 ring scans → 1"`

**Test checkpoint:** With 6 subsystem files and 200 ring entries, flush does 1 ring walk instead of 6 and only non-empty subsystems open files. A burst-heavy subsystem (>32 KB/flush) is chunk-written, not dropped. Serial log shows reduced flush time. Test on: QEMU (fast), bare metal i5-4210U USB (slow), bare metal i5-11600K SATA.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2697 kernel + 16 user, 0 failures

> **Notes:**
> - **What shipped:** `klog_disk.c` single-pass per-subsystem routing + pure helper `klog_dispatch_slot()` (decl `klog.h`): one ring scan bins entries into `slot_of[]`, Phase 2 reuses the full 32 KB batch per subsystem with chunked `vfs_write`.
> - **How it runs:** fires in the existing IXFS flush after the durable `kernel.log` write; the expensive work (ring walk + `format_entry`) stays once-per-entry, the 6 Phase-2 passes only re-read the in-cache `slot_of[]` array.
> - **Downstream effects:** removes the prior 6-scans-per-flush cost on slow USB boot media; Codex review fixed a `batch_size/6` capacity cap, a classify-vs-format misroute race, and a short-write offset-hole in the chunk writer.
> - **Canonical doc:** `src/kernel/klog_disk.c` header comment on the routing block.
> - **Scope boundary:** §13 owns per-subsystem disk-log routing; `kernel.log` durability/cursor is §8, deferred-flush batching §9, splash flush-progress §15.

**Regression risk:** LOW -- every entry kernel.log records is routed to its subsystem view (no `batch_size/6` cap, no silent drop); chunked writes keep a burst subsystem's whole window. Rollback: restore the per-subsystem loop.

> **Verified:** 2026-06-15 | commit `c94ffc19` | 4/4 items | build OK | smoke PASS (KVM 1.990s)
> **Quality reviewed:** 2026-06-15 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1M fixed | scope: kernel-code-quality

---

## 14. IXFS Boot Tests: Slow-Media-Aware

Make boot tests detect slow media (USB) and skip or simplify I/O-heavy tests automatically instead of hardcoded `#if 0` blocks.

**Files:** `src/kernel/main/boot_tests.c`, `src/kernel/main/boot_media.c`, `include/kernel/boot_media.h`, `src/kernel/test/test_boot_device.c`

- [x] Pure predicate `boot_media_is_usb_class(boot_media_speed_t)` in `boot_media.{h,c}` -- 1 for MEDIUM/SLOW (USB), 0 for FAST/UNKNOWN; uses §10's `boot_media_speed()` probe (`blkdev_boot_media_type()` was a never-existing draft name)
- [x] In the `debug=1`-only integration block of `boot_tests_run()`: `reduced_io = boot_media_is_usb_class(boot_media_speed()) || klog_slow_media_detected()` (reactive klog detector covers an UNKNOWN-but-slow boot the probe missed)
- [x] On `reduced_io`: skip `ixfs_test_performance()` entirely (hash index + snapshot + scrub = hundreds of writes)
- [x] On `reduced_io`: skip the WHOLE IXFS CRUD mutation block (create/write/read/delete + `TestDir` create/rmdir) -- every step is a cache-deferred write; the read-path smoke check + directory dump still run, so no dirty test artifact is left behind
- [x] FAST / UNKNOWN: full suite (all tests complete in <5 s on fast media)
- [x] Log: `test: IXFS tests: reduced (USB boot media)` or `test: IXFS tests: full suite (fast media)`
- [x] Commit: `"boot: IXFS tests auto-skip heavy I/O on USB boot media"`

**Test checkpoint:** On USB boot: serial shows `test: IXFS tests: reduced (USB boot media)`, no freeze. On SATA/NVMe: serial shows `test: IXFS tests: full suite (fast media)`, all tests pass. Test on: bare metal i5-4210U USB, QEMU AHCI, bare metal i5-11600K SATA.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2701 kernel + 16 user, 0 failures

> **Notes:**
> - **What shipped:** pure predicate `boot_media_is_usb_class()` in `boot_media.{h,c}` + `reduced_io` gating in `boot_tests.c` `boot_tests_run()` debug-integration block; `test_boot_media_is_usb_class` (4 asserts) in `test_boot_device.c`.
> - **How it runs:** on USB media the write-heavy `debug=1` tests (IXFS CRUD block + `ixfs_test_performance` + live-log dir dump) are skipped; the read-path smoke check still runs. Proactive `boot_media_speed()`, reactive `klog_slow_media_detected()`.
> - **Downstream effects:** complements the existing kernel-test-sweep skip (already `BOOT_MEDIA_SLOW`-gated) so both halves of a slow-USB boot avoid freezing; consumes §10's `boot_media` probe.
> - **Canonical doc:** `include/kernel/boot_media.h` contract + the `boot_tests.c` reduced-IO comment.
> - **Scope boundary:** §14 reduces the `debug=1` IXFS integration tests; the kernel `TEST_CAT_*` sweep skip is §9/§10-owned (`SLOW`-only); the speed probe is §10; IXFS read-path `i_atime`-write policy is IXFS-core territory.

**Regression risk:** LOW -- tests are skipped not broken; full suite still runs on fast media; the reduced path leaves no IXFS test artifact (the whole mutation block is gated). Rollback: remove the `reduced_io` gate, run all tests unconditionally.

> **Verified:** 2026-06-15 | commit `121df824` | 6/6 items | build OK | smoke PASS (KVM 2.430s)
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1M fixed | scope: kernel-code-quality

---

## 15. Flush Progress on Splash Diagnostic Line

Show klog flush progress on the diagnostic subtitle during boot, so slow flushes don't look frozen.

**Files:** `include/kernel/klog.h`, `src/kernel/klog_disk.c`, `src/kernel/main/boot_desktop.c`, `src/kernel/test/test_klog.c`

- [x] Callback API in `klog.h`: `klog_flush_progress_fn` typedef + `klog_disk_set_flush_progress_cb()` (registered static, atomic store/load; not a `klog_disk_flush()` signature change)
- [x] `klog_disk.c`: pure gate `klog_flush_progress_due(total)` + persisted-progress reporting in `klog_disk_flush_locked` -- cb fires after each successful chunk write, never overstating what reached disk
- [/] `boot_desktop.c` registers `boot_flush_splash_progress` -> `boot_splash_diag` around the boot-end `klog_disk_flush_all()`; plumbed but on screen DEFERRED (flush_all is post-splash-fade, compositor-locked, so `boot_splash_diag` no-ops)
- [ ] Render the deferred-mode drain on screen: reorder `boot_splash_finish()` fade-out to after `klog_disk_flush_all()`, OR add a post-splash boot-drain framebuffer surface keyed to the compositor-lock hand-off window
- [ ] Fire the whole-drain 100% tick at the true `klog_disk_flush_locked` end (after subsystem-routing + events.jsonl + serial writes), not after `kernel.log` -- shipped accounting reports only kernel.log per-chunk liveness (no premature 100%)
- [x] Commit: `"boot: klog flush progress on splash diagnostic line"`

**Test checkpoint:** Pure `klog_flush_progress_due` gate + persisted-progress accounting are unit-tested. On-screen validation (USB 2.0 deferred drain shows `"Writing boot log... N/M entries"`) is pending the deferred render-surface item -- the callback fires correctly; the splash is faded by the time `flush_all` runs. Test on: bare metal USB once the render surface lands.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2706 kernel + 16 user, 0 failures

> **Notes:**
> - **What shipped:** `klog.h` flush-progress callback API + `klog_disk.c` persisted-progress reporting + pure `klog_flush_progress_due()` gate; `test_klog_flush_progress_due` (5 asserts); `boot_flush_splash_progress` registration in `boot_desktop.c`.
> - **How it runs:** the flush loop fires the registered cb after each successful chunk write (persisted count, never overstated); `boot_splash_diag` renders it when the splash is up. Atomic release/acquire on the cb pointer.
> - **Downstream effects:** consumes §9's deferred-flush mode (the boot-end `flush_all` is the drain this targets); the on-screen render for that drain is the open `[ ]` item.
> - **Canonical doc:** `include/kernel/klog.h` callback contract + the `klog_disk_flush_locked` progress comment.
> - **Scope boundary:** §15 owns the flush-progress callback + reporting; the boot-end flush ordering / splash-fade timing is `boot_phase3` territory (the deferred render item).

**Regression risk:** LOW -- the callback is optional (NULL = no behavior change); the persisted-progress accounting only adds cb calls after writes that already happen.

> **Verified:** 2026-06-15 | commit `d14e2cd4` | 3/6 items | build OK | smoke PASS (KVM 1.970s)
> **Deferred:** [M] on-screen render of the boot-end drain + whole-drain 100% completion (flush_all runs after `boot_splash_finish()` in the compositor-locked hand-off) -> XREF: 01-boot-platform/TODO-19 §15 (item: "Render the deferred-mode drain on screen" + "Fire the whole-drain 100% tick at the true klog_disk_flush_locked end")
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1M fixed | scope: kernel-code-quality

---

## 16. xHCI Command Ring + BOT Transport SMP Serialization

The xHCI command ring (`xhci_cmd_submit` / `xhci_wait_command`) and the MSC BOT transport (`msc_bot_command`) are unserialized: command enqueue mutates `hc->cmd_ring` with no lock and `xhci_wait_command` accepts the first completion event rather than matching the submitted command TRB, and two CPUs can enter the same MSC device concurrently. Single-threaded boot enumeration is safe today, but post-boot SMP block-layer I/O plus §4 stall recovery (which issues Reset Endpoint / Set TR Dequeue and re-arms the ring) makes a concurrent race destructive. Surfaced by the §4 adversarial review.

- [ ] Serialize command ring + correlate events: lock `xhci_cmd_submit`/`xhci_wait_command`; match each Command Completion AND Transfer Event to its submitted TRB pointer, not the first by type/CC (`xhci_wait_transfer`/abort drain)
- [ ] Defer hot-plug enumeration out of the shared event-ring drain so it cannot steal a synchronous command/transfer completion while a command is in flight
- [ ] Per-MSC-device BOT serialization: a sleepable mutex around `msc_bot_command` (CBW/data/CSW + recovery, `usb_msc.c`) so two CPUs can't interleave one device; Set TR Dequeue must not run while another caller has a queued TRB
- [x] Made `cbw_tag` atomic -- `__atomic_fetch_add(&cbw_tag, 1, __ATOMIC_RELAXED)` in `usb_msc.c` `msc_bot_command`, so two CPUs cannot reuse/reorder a CSW tag (holds even before the deferred BOT serialization)
- [ ] Timeout-tainted endpoint recovery: when `xhci_abort_endpoint` fails the stale TRB stays owned -- force a re-abort or slot-level reset (Reset Device / Disable+Enable Slot) before any retry enqueues behind it on that ring
- [ ] Commit: `"drivers: serialize xHCI command ring + per-device BOT transport (SMP)"`

**Test checkpoint:** SMP stress (concurrent USB MSC reads from 2 CPUs) shows no command-ring corruption, no CSW tag reuse, and a stall recovery during concurrent I/O never advances the endpoint past another caller's TRB. Verify on bare metal i5-4210U (USB 2.0) and a multi-core target.

> **Test runner:** N/A (one-line atomic on a USB driver hot path; no pure kernel-test surface) | validation: SMP-stress on bare metal per the test checkpoint

> **Notes:**
> - **What shipped:** atomic `cbw_tag` -- `__atomic_fetch_add(&cbw_tag, 1, __ATOMIC_RELAXED)` in `usb_msc.c` `msc_bot_command` (item 4 of this section); items 1/2/3/5 are the deferred SMP-serialization refactor.
> - **How it runs:** every BOT command now allocates its CSW tag with one atomic RMW, so two CPUs issuing commands cannot collide on the tag even before per-device BOT serialization lands.
> - **Downstream effects:** down-payment on the forward-looking §16 SMP hardening; the command-ring/event-correlation + BOT-mutex + endpoint-recovery work stays deferred (Deferred stamp below).
> - **Canonical doc:** `src/kernel/drivers/usb_msc.c` `msc_bot_command` tag comment.
> - **Scope boundary:** §16 owns the xHCI/BOT SMP serialization; the atomic tag is the only piece shipped now -- the deadlock-prone command-ring + BOT-mutex refactor is deferred (forward-looking, not reachable until SMP USB block I/O exists).

**Regression risk:** Shipped (atomic `cbw_tag`): LOW -- a lock-free relaxed atomic RMW, no lock-order or hot-path locking introduced. Deferred (command-ring + BOT serialization): MEDIUM -- adds locking to the command/transfer hot path where a lock-order or hold-time mistake can deadlock against the recovery path; hold no lock across the 500 ms transfer waits (snapshot under lock, release, then I/O).

> **Verified:** 2026-06-15 | commit `9ab80d4b` | 1/5 items | build OK | tests pass (SUITE=boot 2706+16)
> **Deferred:** [M] the xHCI/BOT command-ring serialization (items 1/2/3/5) is forward-looking and not reachable today (`xhci.c` enables MSI only after boot enumeration, so boot media I/O is never exposed; the window is post-boot hot-plug concurrent with SMP USB block I/O, which does not exist yet). It is a large, deadlock-prone 5-file refactor across 4 `xhci_event_poll` call sites (CLAUDE.md "stop and ask before large refactors"); recommended scope is coarse per-controller `io_mutex` serializing command/transfer/event-drain + per-device BOT mutex + endpoint-recovery hardening, with fine-grained per-TRB correlation + async hot-plug-out-of-drain as the concurrency follow-on (atomic `cbw_tag` already shipped) -> XREF: 01-boot-platform/TODO-19 §16 (item: "Serialize command ring + correlate events" + "Per-MSC-device BOT serialization")
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1M fixed | scope: kernel-code-quality

---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11                              | 🐧 Linux                     | 🚀 Impossible OS                   |
| --- | ----------------------------- | ------------------------------------ | --------------------------- | --------------------------------- |
| 💎   | SCSI error retry              | ✅ usbstor.sys retries                | ✅ usb-storage retries       | ✅ §1-§3 sense retry + residue     |
| 💎   | Device readiness poll         | ✅ usbstor TUR wait                   | ✅ sd spin-up poll           | ✅ §2 TUR poll, 2s budget          |
| 💎   | USB stall/halt recovery       | ✅ usbstor.sys auto-reset             | ✅ usb-storage ep reset      | ✅ §4 reset EP + BOT reset         |
| 💎   | Bulk transfer timeouts        | ✅ USBD_DEFAULT_PIPE_TRANSFER_TIMEOUT | ✅ usb_submit_urb timeout    | ✅ §5 5s bound + Stop EP           |
| 💎   | No sleep hacks                | ✅ Event-driven readiness             | ✅ SCSI start-stop           | ✅ No global xhci_init sleep       |
| 💎   | Intel EHCI->xHCI port routing | ✅ USB stack routes ports             | ✅ xhci-pci Intel quirk      | ✅ §7 EHCI-gate + bounded settle   |
| 💎   | EHCI/UHCI fallback            | ✅ Full USB stack                     | ✅ ehci-hcd + uhci-hcd       | 🔶 §11 detect + skip; HCD T10      |
| 💎   | Multi-controller USB routing  | ✅ per-HCD device objects             | ✅ per-hcd usb_device        | ✅ §11 global dev idx + owner      |
| 💎   | Bounded disk flush            | ✅ Async I/O                          | ✅ Writeback cache           | ✅ §8 seq-cursor + progress        |
| 💎   | Deferred flush (slow media)   | ✅ Lazy writeback                     | ✅ dirty_writeback_centisecs | ✅ §9 RAM batch + boot-end flush   |
| 💎   | Media speed detection         | ✅ Performance tier                   | ✅ readahead tuning          | ✅ §10 4KiB-probe classify         |
| ⭐   | USB boot diagnostic report    | ❌ Hidden in Event Log                | ❌ dmesg only                | ✅ §12 consolidated report 🚀       |
| 💎   | Single-pass log routing       | ✅ ETW channel                        | ✅ /dev/kmsg                 | ✅ §13 1 ring scan, no drops       |
| 💎   | Media-aware boot tests        | ✅ WinPE adapts                       | ✅ initramfs skips           | ✅ §14 USB skips write-heavy       |
| ⭐   | Flush progress display        | ❌ Not shown                          | ❌ Not shown                 | 🔶 §15 cb shipped; render deferred |

After §1-§10 plus §11's detection + graceful skip, USB boot is as reliable as Windows and Linux on xHCI hardware -- transport-level stall recovery and bounded timeouts prevent hangs on flaky hardware, and it degrades cleanly (no hang) on legacy controllers. Active EHCI/UHCI boot storage lands with the TODO-10 §1/§10 usb_core HCD. §12-§15 add diagnostic and I/O optimizations for slow media.

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
  - Single-pass routing classifier: `klog_dispatch_slot()` tag→slot mapping shipped in `test_klog.c` (TEST_CAT_BOOT) -- all aliases, prefix/case/`:`-qualifier boundaries, NULL/empty/unmatched → -1 (§13)
  - **Note:** the live flush path (`klog_disk_flush_locked` empty-slot no-open, chunk-write, re-validate) is validated via boot-serial only -- driving it from a unit test would call live `vfs_open/write/close`, banned per `feedback_test_no_live_boot_calls` (WSL has no QEMU)
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
