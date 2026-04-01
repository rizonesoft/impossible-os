# TODO-14 — USB Boot Hardening & Fail-Safe Pipeline

> **Goal:** Make USB boot bulletproof. The current USB boot path has a 2-second sleep hack masking a timing race, REQUEST SENSE retry logic that's partially wired, klog_disk_flush that hangs 10+ minutes on USB 2.0, and no EHCI/UHCI fallback for legacy hardware. This TODO consolidates and completes all USB boot fragility fixes from TODO-10, TODO-11, and TODO-07 §10 into a single fail-safe pipeline that works on USB 2.0 (EHCI), 3.0 (xHCI), 3.1, and 3.2 with proper SCSI retry logic, no sleep hacks, and no hangs.

> [!IMPORTANT]
> **Current state:** USB boot works on modern xHCI hardware (i5-11600K) but has known issues: (1) 2-second `sleep_ms(2000)` after xhci_init masks a SCSI UNIT ATTENTION race on EHCI→xHCI routed ports, (2) klog_disk_flush hangs 10+ min on USB 2.0 due to unbounded loop, (3) no EHCI/UHCI/OHCI fallback for systems without xHCI, (4) no retry on transient USB MSC errors, (5) XUSB2PR port-ready polling not implemented. These are all identified in TODO-10 and TODO-11 but spread across multiple files.

---

## Inputs

- `src/kernel/drivers/xhci.c` — xHCI controller init, DCBAA, port scan
- `src/kernel/drivers/xhci_dev.c` — USB device enumeration, MSC identification
- `src/kernel/drivers/usb_msc.c` — BOT SCSI transport (CBW/CSW/data)
- `src/kernel/klog_disk.c` — disk flush (hangs on USB 2.0)
- `src/kernel/main/boot_storage.c` — `sleep_ms(2000)` hack location
- → XREF: `TODO-10-usb-msc-retry-readiness.md §1–§6` — SCSI retry logic (partially done)
- → XREF: `TODO-11-klog-ixfs-bare-metal-perf.md §1–§7` — klog flush hang fix
- → XREF: `TODO-07-xhci-usb-boot.md §10` — EHCI/UHCI/OHCI fallback
- → XREF: `TODO-09-usb-zero-delay-handover.md §7` — corrupt state fallback

---

## Outcome

- USB boot works reliably on USB 2.0 (EHCI), 3.0, 3.1, 3.2 (xHCI) with zero sleep hacks.
- SCSI UNIT ATTENTION and NOT READY are handled via proper REQUEST SENSE + TEST UNIT READY retry.
- klog_disk_flush never hangs — bounded loop with progress reporting.
- EHCI/UHCI fallback for systems without xHCI (pre-2012 hardware).
- XUSB2PR port-ready polling replaces fixed delays on Intel EHCI→xHCI routing.
- Boot from USB is as reliable as boot from SATA.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On    | Status |
| --- | :---: | ------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | SCSI REQUEST SENSE and error classification        | —             |  [ ]   |
| 💎  |   2   | TEST UNIT READY poll loop after BOT init           | §1            |  [ ]   |
| 💎  |   3   | MSC BOT retry on transient errors                  | §1, §2        |  [ ]   |
| 💎  |   4   | Remove sleep_ms(2000) hack                         | §2, §3        |  [ ]   |
| 💎  |   5   | XUSB2PR port-ready polling (Intel EHCI→xHCI)       | §4            |  [ ]   |
| 💎  |   6   | klog_disk_flush bounded loop                       | —             |  [ ]   |
| 💎  |   7   | klog deferred flush mode (batch to RAM)             | §6            |  [ ]   |
| 💎  |   8   | Boot media speed detection                         | §6            |  [ ]   |
| 💎  |   9   | EHCI/UHCI companion controller fallback            | —             |  [ ]   |
| ⭐  |  10   | USB boot diagnostic report                         | §1–§9         |  [ ]   |

> 💎 = parity — Windows usbstor.sys and Linux usb-storage both handle SCSI retry and EHCI fallback.
> ⭐ = exclusive — comprehensive USB boot diagnostic report at end of enumeration.

---

## 1. SCSI REQUEST SENSE and Error Classification

Implement the SCSI REQUEST SENSE command to decode why a USB MSC command failed.

- [ ] Add `msc_request_sense(dev, sense_data)` to `usb_msc.c` — sends REQUEST SENSE CDB (0x03)
- [ ] Parse sense data: sense key (byte 2), ASC (byte 12), ASCQ (byte 13)
- [ ] Classify errors: `UNIT_ATTENTION` (key=6) → retry after reset, `NOT_READY` (key=2) → wait + retry, `MEDIUM_ERROR` (key=3) → unrecoverable
- [ ] Log: `"[USB] Sense: key=%u ASC=%u ASCQ=%u (%s)"` with human-readable description
- [ ] Commit: `"drivers: SCSI REQUEST SENSE command with error classification"`

**Test checkpoint:** Plug USB drive, boot. If drive returns UNIT ATTENTION on first command, serial shows sense data. Verify on bare metal i5-4210U.

---

## 2. TEST UNIT READY Poll Loop

After BOT init, poll TEST UNIT READY until the device reports ready instead of sleeping.

- [ ] Add `msc_test_unit_ready(dev)` — sends TEST UNIT READY CDB (0x00)
- [ ] Poll loop: send TUR → if error, `REQUEST SENSE` → if NOT_READY or UNIT_ATTENTION, wait 50ms + retry
- [ ] Maximum 40 attempts (2 seconds total) — same window as the current sleep hack but event-driven
- [ ] If all attempts fail: log `"[WARN] USB drive not ready after 2s"` but continue (device may respond later)
- [ ] Commit: `"drivers: TEST UNIT READY poll loop — replace sleep hack with SCSI readiness"`

**Test checkpoint:** USB 2.0 drive on i5-4210U bare metal boots without the sleep hack. Serial shows TUR poll count.

---

## 3. MSC BOT Retry on Transient Errors

Wrap `msc_bot_command()` with automatic retry for transient SCSI errors.

- [ ] After CSW returns error: call `REQUEST SENSE` → classify error
- [ ] `UNIT_ATTENTION`: retry command immediately (device reset itself, normal after plug)
- [ ] `NOT_READY`: wait 100ms + retry (drive spinning up)
- [ ] `MEDIUM_ERROR`: fail immediately (bad sector, no retry will help)
- [ ] Maximum 3 retries per command
- [ ] Log: `"[USB] Retry %u/3: sense key=%u"` on each retry
- [ ] Commit: `"drivers: USB MSC BOT automatic retry for transient SCSI errors"`

**Test checkpoint:** Normal USB boot — no retries needed, zero performance impact. Bare metal with slow USB 2.0 — retries handle UNIT ATTENTION transparently.

---

## 4. Remove sleep_ms(2000) Hack

With proper SCSI retry (§1–§3), the 2-second sleep after xhci_init is no longer needed.

- [ ] Remove `sleep_ms(2000)` from `boot_storage.c` (or wherever it currently lives)
- [ ] Verify USB boot works on: QEMU TCG, bare metal i5-11600K, bare metal i5-4210U
- [ ] If any platform fails without the sleep: investigate root cause, don't add the sleep back
- [ ] Commit: `"drivers: remove USB 2-second sleep hack — SCSI retry handles readiness"`

**Test checkpoint:** USB boot works on all tested platforms without the delay. Boot time improves by ~2 seconds.

**Regression risk:** HIGH — this is the moment of truth. If retry logic isn't sufficient, USB boot breaks on slow hardware. Rollback: temporarily re-add `sleep_ms(500)` as a smaller delay while investigating.

---

## 5. XUSB2PR Port-Ready Polling (Intel EHCI→xHCI)

On Intel chipsets, USB 2.0 ports are routed from EHCI to xHCI via the XUSB2PR PCI register. The port switch takes time — currently masked by the 2-second sleep.

- [ ] After writing XUSB2PR: poll port status registers until ports report connected/enabled
- [ ] Maximum wait: 500ms (real hardware typically takes <100ms)
- [ ] Log: `"[USB] XUSB2PR port routing complete (%u ms)"` with actual time
- [ ] If timeout: `"[WARN] XUSB2PR port routing timeout — some USB 2.0 ports may not work"`
- [ ] Only applies to Intel chipsets with XUSB2PR capability (detect via PCI vendor/device)
- [ ] Commit: `"drivers: XUSB2PR port-ready polling — replace fixed delay with event-driven wait"`

**Test checkpoint:** i5-4210U bare metal with USB 2.0 drive — port routing completes with logged time. Non-Intel systems skip this entirely.

---

## 6. klog_disk_flush Bounded Loop

Fix the unbounded flush loop that hangs 10+ minutes on USB 2.0.

- [ ] At `klog_disk_flush()` entry: snapshot `ring_count` — flush exactly that many entries, no more
- [ ] If new entries arrive during flush (from flush-triggered logging): ignore them until next flush call
- [ ] Add progress: `"[KLOG] Flushing %u entries to disk..."` at start, `"done (%u ms)"` at end
- [ ] If flush takes > 5 seconds: `"[WARN] Slow media detected — switching to deferred flush"`
- [ ] Commit: `"kernel: bounded klog_disk_flush — snapshot ring count, no unbounded loop"`

**Test checkpoint:** USB 2.0 boot on bare metal — flush completes in seconds, not minutes. Serial shows flush count and duration.

---

## 7. klog Deferred Flush Mode

For slow media, batch log entries in RAM and write once at boot end instead of per-subsystem.

- [ ] Add `klog_set_deferred(int enabled)` — switches to RAM-only buffering
- [ ] During deferred mode: entries accumulate in ring buffer, `klog_disk_flush()` is a no-op
- [ ] At boot complete: single `klog_disk_flush_all()` writes everything at once
- [ ] Combine multiple entries into larger VFS writes (fewer USB transfers)
- [ ] Automatically enable deferred mode if §6 detects slow media (>5s for first flush)
- [ ] Commit: `"kernel: klog deferred flush — batch to RAM, single write at boot end"`

**Test checkpoint:** USB 2.0 boot — all log entries appear in disk log file, written in one batch. Boot time comparable to no-disk-log scenario.

---

## 8. Boot Media Speed Detection

Detect whether boot media is fast (SSD/NVMe) or slow (USB 2.0/USB 3.0 stick) and adjust behavior.

- [ ] At first disk I/O: time a 4 KiB read — classify as fast (<1ms), medium (1–10ms), slow (>10ms)
- [ ] Store in `boot_info.boot_media_speed` (0=unknown, 1=fast, 2=medium, 3=slow)
- [ ] Kernel uses this to: enable deferred klog (slow), skip non-critical boot tests (slow), adjust timeouts
- [ ] Log: `"[BOOT] Boot media speed: %s (%u µs/4KiB)"` with classification
- [ ] Commit: `"kernel: boot media speed detection — adjust behavior for slow USB"`

**Test checkpoint:** SATA SSD → "fast", USB 3.0 stick → "medium", USB 2.0 stick → "slow".

---

## 9. EHCI/UHCI Companion Controller Fallback

For pre-2012 hardware without xHCI, provide basic USB storage via EHCI.

- [ ] Detect EHCI controller: PCI class 0x0C/0x03/0x20
- [ ] Minimal EHCI driver: port reset, bulk transfer, BOT SCSI — enough for MSC storage
- [ ] If xHCI not found: try EHCI; if EHCI not found: try UHCI (PCI class 0x0C/0x03/0x00)
- [ ] Share the `usb_msc.c` BOT layer — only the host controller interface differs
- [ ] Log: `"[USB] Using %s controller (xHCI not available)"` with EHCI/UHCI
- [ ] Commit: `"drivers: EHCI fallback for USB storage on pre-xHCI hardware"`

**Test checkpoint:** VirtualBox (which emulates EHCI by default) — USB storage works via EHCI fallback. Serial shows `"Using EHCI controller"`.

**Regression risk:** HIGH — new driver code. If EHCI driver corrupts USB state, system hangs. Test on VirtualBox first.

---

## 10. USB Boot Diagnostic Report

At end of USB enumeration, produce a comprehensive diagnostic summary.

- [ ] Log all discovered USB devices with: port, speed (HS/SS/SS+), vendor/product, MSC/HID/other
- [ ] Log controller type: xHCI (version), EHCI, UHCI
- [ ] Log port routing status: XUSB2PR active, companion controller status
- [ ] Log SCSI retry statistics: total retries, sense keys encountered
- [ ] Log boot media speed classification
- [ ] Format: `"[USB] === USB Boot Report ==="` section in serial log
- [ ] Commit: `"drivers: USB boot diagnostic report — full enumeration summary"`

**Test checkpoint:** Serial output contains `"=== USB Boot Report ==="` with device list and statistics.

---

## OS Comparison

| ⭐ | Feature                      | Win11                     | Linux                      | Impossible OS               |
|----|------------------------------|---------------------------|----------------------------|-----------------------------|
| 💎 | SCSI error retry             | ✅ usbstor.sys retries   | ✅ usb-storage retries     | ⬜ §1–§3                    |
| 💎 | No sleep hacks               | ✅ Event-driven readiness | ✅ SCSI start-stop         | ⬜ §4                       |
| 💎 | EHCI/UHCI fallback           | ✅ Full USB stack         | ✅ ehci-hcd + uhci-hcd     | ⬜ §9                       |
| 💎 | Bounded disk flush           | ✅ Async I/O              | ✅ Writeback cache          | ⬜ §6–§7                    |
| 💎 | Media speed detection        | ✅ Performance tier       | ✅ readahead tuning        | ⬜ §8                       |
| ⭐ | USB boot diagnostic report   | ❌ Hidden in Event Log    | ❌ dmesg only              | ⬜ §10 🚀                   |

After §1–§9, USB boot is as reliable as Windows and Linux across all USB generations and controller types. §10 provides visibility into the USB stack that neither Windows nor Linux surfaces to users.

---

## Verification

- [ ] **USB 2.0 bare metal**: boot from USB 2.0 on i5-4210U — no sleep hack, SCSI retries handle readiness, klog flush completes in seconds.
- [ ] **USB 3.0 QEMU**: boot from USB 3.0 via xHCI on TCG — works as before, no regressions.
- [ ] **EHCI fallback**: boot from USB on VirtualBox (EHCI) — MSC storage works via EHCI driver.
- [ ] **Normal SATA boot regression**: SATA boot on all 4 platforms unaffected.
- [ ] **klog flush**: all log entries appear in disk log file, flush completes in bounded time.
- [ ] Commit: `"boot: USB boot hardening complete — fail-safe pipeline across all USB generations"`
