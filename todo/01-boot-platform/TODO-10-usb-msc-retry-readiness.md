# TODO-10 — USB MSC Retry & Device Readiness *(SUPERSEDED by TODO-14)*

> [!IMPORTANT]
> **This TODO is fully superseded by `TODO-14-usb-boot-hardening.md`** which consolidates all USB boot fragility fixes into a single fail-safe pipeline. TODO-14 §1–§5 implements REQUEST SENSE, TEST UNIT READY, MSC BOT retry, sleep hack removal, and XUSB2PR polling — the complete scope of this TODO plus EHCI fallback, klog flush fixes, and media speed detection.

> **Original goal:** Replace the 2-second sleep hack after `xhci_init()` with proper SCSI retry logic in the USB MSC read/write path.

## Inputs

- [`src/kernel/drivers/usb_msc.c`](../../src/kernel/drivers/usb_msc.c) — BOT transport, SCSI commands, read/write sectors
- [`include/kernel/drivers/usb_msc.h`](../../include/kernel/drivers/usb_msc.h) — CBW/CSW structures, SCSI opcodes
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) — USB enumeration, MSC identify
- [`src/kernel/main/boot_storage.c`](../../src/kernel/main/boot_storage.c) — boot_phase2() with current sleep hack
- → XREF: `01-boot-platform/TODO-07-xhci-usb-boot.md §3` — USB MSC BOT driver
- → XREF: `01-boot-platform/TODO-09-usb-zero-delay-handover.md` — zero-delay USB boot (depends on this fix)
- → XREF: `01-boot-platform/TODO-11-klog-ixfs-bare-metal-perf.md` — klog flush fix (shares USB I/O perf root cause)

## Outcome

- `usb_msc_read_sectors()` retries on SCSI CHECK CONDITION / NOT READY with exponential backoff
- `usb_msc_write_sectors()` retries on transient SCSI errors
- `msc_bot_command()` parses SCSI sense data to distinguish retriable vs fatal errors
- TEST UNIT READY poll loop after BOT init ensures device is ready before first block I/O
- 2-second sleep hack removed from `boot_storage.c`
- USB boot works reliably on Haswell EHCI→xHCI routed systems without artificial delays
- No regression on QEMU, VirtualBox, or i5-11600K bare metal

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | REQUEST SENSE command and sense data parsing   | —          |  [/]   |
| 💎  |   2   | Retry logic in msc_bot_command()               | §1         |  [/]   |
| 💎  |   3   | TEST UNIT READY poll loop after BOT init       | §1         |  [/]   |
| 💎  |   4   | XUSB2PR port-ready polling (replace 500ms)     | —          |  [/]   |
| 💎  |   5   | Remove sleep hacks, verify on bare metal       | §2, §3, §4 |  [/]   |
| ⭐  |   6   | Boot-time device readiness reporting           | §5         |  [/]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. REQUEST SENSE Command and Sense Data Parsing
Add SCSI REQUEST SENSE (opcode 0x03) to retrieve sense data after a failed command, and parse the sense key / ASC / ASCQ triple.

**Files:** `src/kernel/drivers/usb_msc.c`, `include/kernel/drivers/usb_msc.h`

- [x] Add `SCSI_REQUEST_SENSE 0x03` opcode to `usb_msc.h`
- [x] Add SCSI sense key constants: `SENSE_NOT_READY (0x02)`, `SENSE_UNIT_ATTENTION (0x06)`, `SENSE_MEDIUM_ERROR (0x03)`
- [x] Implement `msc_request_sense(hc, dev, &sense_key, &asc, &ascq)` — sends 6-byte CDB, parses 18-byte fixed-format response
- [x] Add `msc_is_retriable(sense_key, asc, ascq)` — returns 1 for NOT READY (becoming ready, 0x04/0x01), UNIT ATTENTION (reset, 0x29/0x00), and medium not present (0x3A)
- [ ] Commit: `"drivers: USB MSC REQUEST SENSE + sense data parsing"`

**Test checkpoint:** After a failed SCSI command, REQUEST SENSE returns valid sense data. Serial log shows `usb-msc: sense key=0x06 ASC=0x29 ASCQ=0x00 (UNIT ATTENTION)`. POST code `POST16(0xDA00)` on entry, `POST16(0xDA01)` on exit. Test on: QEMU `run-usb`, bare metal i5-4210U.

**Regression risk:** LOW — new function, not called from existing paths yet. No behavior change until §2 wires it in.

## 2. Retry Logic in msc_bot_command()
Wrap the BOT command execution in a retry loop with exponential backoff. On failure, issue REQUEST SENSE and retry if the error is transient.

**Files:** `src/kernel/drivers/usb_msc.c`

- [x] After CSW status != PASS, call `msc_request_sense()` to get sense data
- [x] If `msc_is_retriable()` returns 1, retry up to 10 times with fixed 500ms intervals (5s total max — the I/O path is the last line of defense if TUR readiness missed a transient)
- [x] If sense key is MEDIUM ERROR or HARDWARE ERROR, fail immediately (no retry)
- [x] If all retries exhausted, log final sense data and return error
- [ ] Handle CSW phase error (status=2): issue Bulk-Only Mass Storage Reset (class request 0xFF), clear HALT on both endpoints, then retry
- [x] Log each retry: `usb-msc: retry N/10 (sense=0xKK ASC=0xAA ASCQ=0xQQ, 500ms)`
- [ ] Commit: `"drivers: USB MSC retry logic with sense data — replaces sleep hack"`

**Test checkpoint:** On first boot after flash, USB drive may return UNIT ATTENTION on first READ(10). Serial log shows retry succeeded: `usb-msc: retry 1/10 (sense=0x06 ASC=0x29 ASCQ=0x00, 200ms)` followed by successful read. POST code `POST16(0xDA02)` on entry, `POST16(0xDA03)` on exit. Test on: bare metal i5-4210U, QEMU `run-usb`. If crash, check last POST code — 0xDA02 = retry loop entered, 0xDA03 = retry loop completed.

**Regression risk:** MEDIUM — modifies the core BOT command path used by all USB reads/writes. If retry logic has a bug, USB I/O breaks entirely. Mitigation: retry count capped at 10, total delay bounded at 5s. Fixed 500ms interval matches Windows SCSI class driver. Rollback: remove retry loop, restore direct return.

## 3. TEST UNIT READY Poll Loop After BOT Init
Strengthen the existing TEST UNIT READY loop in `usb_msc_init()` to poll with proper sense data checks, ensuring the device is fully ready before declaring init complete.

**Files:** `src/kernel/drivers/usb_msc.c`

- [x] Replace the existing 3-retry TUR loop with a proper readiness poll:
  - Loop up to 20 times with 500ms intervals (10s total max)
  - On failure, issue REQUEST SENSE to check sense key
  - If NOT READY: retry (device is settling after EHCI→xHCI routing)
  - If UNIT ATTENTION: retry (device acknowledges reset)
  - If any other sense key: break (device has a real problem)
- [x] Log: `usb-msc: device becoming ready (attempt N/20)` on each retry
- [x] Log: `usb-msc: device ready after N attempts (Xms)` on success
- [ ] Commit: `"drivers: USB MSC TEST UNIT READY with sense-driven readiness poll"`

**Test checkpoint:** USB drive that needs spin-up time shows `device becoming ready` messages in serial. Device eventually reports ready. POST code `POST16(0xDA04)` on entry, `POST16(0xDA05)` on exit. Test on: bare metal i5-4210U with various USB sticks, QEMU `run-usb`.

**Regression risk:** LOW — replaces an existing retry loop with a smarter one. Worst case: takes up to 10s to declare device ready (vs 300ms currently). But this only triggers on devices that actually need it — fast devices pass TUR on attempt 1. Rollback: restore original 3-retry loop.

## 4. XUSB2PR Port-Ready Polling (Replace 500ms Delay)
Replace the fixed 500ms `xhci_delay_us(500000)` after Intel XUSB2PR/USB3_PSSEN routing with a PORTSC CCS polling loop. After routing USB 2.0 ports from EHCI to xHCI, devices need time to re-appear — but the actual time varies by hardware. Polling for CCS (Current Connect Status) is what the xHCI spec intends.

**Files:** `src/kernel/drivers/xhci.c`

- [x] After writing XUSB2PR and USB3_PSSEN registers, replace `xhci_delay_us` with a CCS poll loop
- [x] Poll all PORTSC registers for CCS bit (bit 0) every 50ms, up to 5000ms timeout
- [x] Exit early as soon as at least one port shows CCS — devices have re-appeared
- [x] If timeout with zero CCS ports: log warning and continue
- [x] Log: `xhci: XUSB2PR routing: CCS on port N after Xms` on success
- [x] Log: `xhci: XUSB2PR routing: no CCS after 5000ms — continuing` on timeout
- [ ] Commit: `"drivers: xHCI XUSB2PR port-ready polling — replace fixed 500ms delay"`

**Test checkpoint:** On i5-4210U Haswell, CCS appears within 50-200ms (not 500ms). Serial shows actual settle time. On i5-11600K, XUSB2PR is skipped entirely (no EHCI). POST code `POST16(0xDA06)` on entry, `POST16(0xDA07)` on exit. Test on: bare metal Haswell, bare metal Skylake+, QEMU. If crash at 0xDA06, CCS polling loop is failing.

**Regression risk:** MEDIUM — changes the EHCI→xHCI handoff timing on Intel 7/8/9-series. If CCS polling exits too early (device not fully stable), enumeration could fail. Mitigation: 50ms poll interval is conservative; 5000ms timeout is 10× the current 500ms. Rollback: restore `xhci_delay_us(500000)`.

## 5. Remove Sleep Hacks, Verify on Bare Metal
Remove both raw delays: the 2-second `sleep_ms(2000)` after `xhci_init()` in `boot_storage.c` and the 500ms XUSB2PR delay in `xhci.c` (now replaced by CCS polling). Verify that retry logic + CCS polling handle all readiness windows.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/xhci.c`

- [x] Remove the `if (xhci_msc_device_count() > 0) sleep_ms(5000);` block from `boot_storage.c`
- [x] Confirm XUSB2PR delay is replaced by CCS poll (§4)
- [ ] Verify on bare metal i5-4210U: C:\ mounts on first boot without either delay
- [ ] Verify on QEMU `make run`: no regression (USB boot still works)
- [ ] Verify on QEMU `make run-usb`: USB MSC enumeration + mount works
- [ ] Verify on i5-11600K bare metal: no regression
- [ ] Commit: `"boot: remove all USB settle sleeps — retry + CCS poll handle readiness"`

**Test checkpoint:** Boot completes without raw delays. Serial shows CCS poll time (§4) and SCSI retry count (§2). C:\ mounts reliably across 5 consecutive cold boots on the Haswell laptop. Total boot time reduced by ~2.5s. Test on: bare metal i5-4210U (5 cold boots), bare metal i5-11600K, QEMU WHPX, QEMU TCG, VirtualBox.

**Regression risk:** HIGH — removes both safety nets. Must be verified with 5+ consecutive cold boots on Haswell. Rollback: re-add both delays.

## 6. Boot-Time Device Readiness Reporting
Report USB device readiness status on the splash diagnostic line, showing retry count and total settle time.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/usb_msc.c`

- [x] Add `usb_msc_get_retry_count()` API — returns total retries across all MSC devices during this boot
- [x] Add `usb_msc_get_settle_ms()` API — returns total milliseconds spent in retry delays
- [x] Show on diagnostic subtitle: `USB: MSC ready (2 retries, 1500ms)` or `USB: MSC ready (0 retries)`
- [x] If retries > 3, log at WARN level: `DIAG-MSC: USB: MSC ready (N retries, Xms)`
- [ ] Commit: `"boot: USB MSC readiness reporting on splash diagnostic"`

**Test checkpoint:** Diagnostic subtitle shows retry stats. Serial log includes settle time. Test on: bare metal i5-4210U (debug=1), QEMU `run-usb`.

**Regression risk:** LOW — read-only reporting, no behavior change.

---

## OS Comparison

| ⭐ | Feature              | Win11                    | Linux                   | Impossible OS                |
|----|----------------------|--------------------------|-------------------------|------------------------------|
| 💎 | SCSI retry on error  | ✅ USBSTOR.SYS          | ✅ usb-storage          | ⬜ §2 — retry + backoff      |
| 💎 | REQUEST SENSE        | ✅ Full sense parse      | ✅ Full sense parse     | ⬜ §1 — key/ASC/ASCQ         |
| 💎 | Device readiness     | ✅ PnP IRP signaling     | ✅ TUR + sense poll     | ⬜ §3 — sense-driven TUR     |
| 💎 | Port-ready polling   | ✅ Hub debounce          | ✅ usb_port_debounce    | ⬜ §4 — CCS poll             |
| 💎 | No raw delay         | ✅ Event-driven          | ✅ Retry-based          | ⬜ §5 — remove sleep hacks   |
| ⭐ | Readiness reporting  | ❌ Not shown             | ❌ dmesg post-boot      | ⬜ §6 — splash + serial      |
| ⭐ | Per-device settle ms | ❌ Not tracked           | ❌ Not tracked          | ⬜ §6 — boot perf metric     |

> After §1-§5, Impossible OS matches Windows and Linux USB robustness.
> §6 is exclusive: visible readiness reporting + per-device settle metrics at boot.

## Verification

- [ ] QEMU WHPX `make run-usb`: USB MSC boot works, no retries needed
- [ ] QEMU TCG `make run-usb`: same as WHPX
- [ ] QEMU `make run`: no USB devices — graceful skip, no delay
- [ ] VirtualBox: boot completes — graceful skip (no xHCI or USB MSC)
- [ ] Bare metal i5-4210U: 5 consecutive cold boots, C:\ mounts every time without raw delays
- [ ] Bare metal i5-4210U: serial shows XUSB2PR CCS poll time <200ms (not fixed 500ms)
- [ ] Bare metal i5-11600K: no regression — USB boot still works (if USB stick present)
- [ ] Serial log shows retry attempts when device needs settle time
- [ ] Serial log shows zero retries when device is immediately ready
- [ ] Diagnostic subtitle shows readiness stats (debug=1)
- [ ] Total boot time reduced by ~2s compared to sleep hack baseline
- [ ] POST code sequence: 0xDA00→0xDA01 (§1), 0xDA02→0xDA03 (§2), 0xDA04→0xDA05 (§3), 0xDA06→0xDA07 (§4)
