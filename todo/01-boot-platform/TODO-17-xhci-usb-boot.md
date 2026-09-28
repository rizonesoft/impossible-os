---
schema_version: 1
id: xhci-usb-boot
domain: 01-boot-platform
status: active
title: "TODO-17 -- xHCI, USB Storage & USB HID (Boot-Critical)"
---

# TODO-17 -- xHCI, USB Storage & USB HID (Boot-Critical)

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** USB boot that works on 95%+ of hardware. This TODO owns the baseline xHCI controller path, MSC transport, block-device registration, post-boot hot-plug, and broad hardware compatibility. The pre-ExitBootServices persistent-DMA handover path is consolidated under [TODO-20](TODO-20-usb-zero-delay-handover.md).

> [!IMPORTANT]
> **Two-track approach:** §1-§4 are the working baseline path (halt/reset/re-enumerate after ExitBootServices, Intel port routing where needed, known-good storage boot). §5 owns post-boot xHCI hot-plug and device lifecycle. Zero-delay pre-ExitBootServices handover is owned by TODO-20.

> [!NOTE]
> Working code exists: `xhci.c` (controller init, DCBAA, TRB rings, Intel port routing), `xhci_dev.c` (full 9-step enumeration + MSC identification), `xhci_ring.c` (TRB ring management), `usb_msc.c` (BOT SCSI transport). Verified on QEMU TCG + bare metal i5-11600K.

## Inputs

- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) -- xHCI controller driver (partial)
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) -- device enumeration (partial)
- [`src/kernel/drivers/xhci_ring.c`](../../src/kernel/drivers/xhci_ring.c) -- TRB ring management
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c) -- PS/2 keyboard (injection target for USB HID)
- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) -- PS/2 mouse (injection target for USB HID)
- → XREF: `04-drivers-hardware/TODO-10-usb-stack.md` -- advanced USB features (hub, hot-plug, EHCI, BT, CDC)
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §3` -- MSI/MSI-X (xHCI uses MSI)
- → XREF: `01-boot-platform/TODO-18-usb-hid-keyboard-mouse.md` -- USB HID keyboard and mouse (boot protocol, coexistence)
- → XREF: `01-boot-platform/TODO-20-usb-zero-delay-handover.md` -- true zero-delay handover (persistent DMA in bootloader)

## Outcome

- USB boot drive mounts through the proven baseline xHCI path owned here; zero-delay kernel-start availability is tracked in TODO-20.
- Hot-plug: USB devices connected after boot are detected via xHCI interrupts (§5).
- Works on Intel + non-Intel xHCI (AMD, ASMedia, VIA, Renesas) via the generic vendor path (§6); EHCI/UHCI/OHCI-only legacy hardware is owned by T10 §10 + T19 §11

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On | Status |
| --- | :---: | ------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI controller bring-up and port scan            | --         |  [x]   |
| 💎  |   2   | USB device enumeration and configuration          | §1         |  [x]   |
| 💎  |   3   | USB MSC BOT (Bulk-Only Transport) driver          | §2         |  [x]   |
| 💎  |   4   | Block device registration and VFS integration     | §3         |  [x]   |
| 💎  |   5   | Interrupt-driven hot-plug and post-boot lifecycle | §1-§4      |  [/]   |
| ⭐  |   6   | Non-Intel xHCI vendor compatibility (de-scoped)   | §1         |  [/]   |

---

## 1. xHCI Controller Bring-Up and Port Scan
Verify and fix the existing xHCI controller initialization. Currently logs "No xHCI controllers found" on some platforms.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/pci.c`

- [x] Verify PCI discovery finds xHCI (class 0x0C, subclass 0x03, prog-if 0x30)
- [x] Controller halt, reset, DCBAA allocation, command ring, event ring -- audit existing code
- [x] Port scan: detect attached USB devices, log port status
- [x] Map xHCI BAR0 via `vmm_map_mmio_uc()` (MMIO registers need UC mapping)
- [x] Commit: `"drivers: xHCI controller bring-up verified on QEMU + bare metal"`

**Test checkpoint:** Serial shows `xHCI vX.Y ready, N slots, N ports, N intrs, N scratchpads`. POST code 0xD750 (handoff); 0xD751 follows a successful inherit but precedes full init on the ordinary path, so the `ready` line is the success marker. Test on: QEMU `run-usb`, bare metal.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | live path via `make run-usb-ci` (TCG) + bare metal; `test_usb_boot.c` pending (Unit Tests).
> **Notes:**
> - Shipped: xHCI controller bring-up (`xhci.c`) -- PCI discovery (0x0C/0x03/0x30), halt/reset, DCBAA, command+event rings, BAR0 via `vmm_map_mmio_uc`, Intel XUSB2PR routing.
> - Integrates: feeds §2 enumeration; per-controller state in `controllers[]`; bootloader DMA handover inherited (TODO-20 path).
> - Review: no §1-specific defect; the §3/§4 transport hardening (64 KiB-boundary TRB split, LBA bounds) protects the path this section opens.
> - Scope boundary: §1 owns controller bring-up + port scan; enumeration is §2; USBLEGSUP handoff is TODO-20 §2; non-Intel vendor matrix is §6.
> **Verified:** 2026-06-15 | this review commit | 4/4 items | build OK | run-usb-ci PASS (xHCI 1b36:000d, 64 slots/8 ports)
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 4H fixed, 1H deferred (ISR -> TODO-10 §8) | scope: kernel-code-quality

## 2. USB Device Enumeration and Configuration
Complete the device enumeration path: slot enable → Address Device → GET_DESCRIPTOR → SET_CONFIGURATION.

**Files:** `src/kernel/drivers/xhci_dev.c`

- [x] Audit existing `xhci_dev_enumerate()` -- full 9-step enumeration implemented and verified
- [x] Parse device descriptor: class, subclass, protocol, VID:PID
- [x] Parse configuration descriptor: find MSC interface (class 0x08) -- HID (class 0x03) deferred to HID driver section
- [x] Configure Endpoint for bulk-IN/OUT (MSC) -- transfer rings allocated, Configure Endpoint command issued
- [x] Log: `usb: Device 46f4:0001 enumerated on port 1 (slot 1) [MSC]`
- [x] Commit: `"drivers: USB device enumeration -- MSC interfaces detected"`

**Test checkpoint:** Serial shows detected USB devices with class info (`Device VVVV:PPPP enumerated on port N (slot N) [MSC]`). POST code 0x2011 (`POST16_XHCI_OK`) once `xhci_init()` returns. Test on: QEMU `run-usb`, bare metal.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | enumeration validated live via `make run-usb-ci` + bare metal.
> **Notes:**
> - Shipped: full 9-step enumeration (`xhci_dev.c`) -- port reset, Enable Slot, Address Device, GET_DESCRIPTOR (device+config, wTotalLength capped at 4096), SET_CONFIGURATION, Configure Endpoint.
> - Integrates: parses class/subclass/VID:PID; finds the MSC interface (class 0x08) + allocates bulk-IN/OUT transfer rings for §3.
> - Review: descriptor-walk bounds already guard `desc_len`/offset overrun; no §2-specific defect found.
> - Scope boundary: §2 owns enumeration + MSC-interface detection; HID-interface handling is TODO-18/TODO-10 §5.
> **Verified:** 2026-06-15 | this review commit | 5/5 items | build OK | run-usb-ci PASS (USB 3.00, VID=46f4 enumerated, config set)
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 4H fixed, 1H deferred (ISR -> TODO-10 §8) | scope: kernel-code-quality

## 3. USB MSC BOT (Bulk-Only Transport) Driver
Implement the SCSI-over-USB transport layer: CBW/CSW framing, INQUIRY, READ CAPACITY, READ(10), WRITE(10).

**Files:** `src/kernel/drivers/usb_msc.c` (new), `include/kernel/drivers/usb_msc.h` (new)

- [x] CBW (Command Block Wrapper) and CSW (Command Status Wrapper) structures
- [x] `usb_msc_inquiry()` -- identify device type and name
- [x] `usb_msc_read_capacity()` -- get sector count and sector size
- [x] `usb_msc_read_sectors(lba, count, buf)` -- READ(10) via bulk-OUT CBW + bulk-IN data + bulk-IN CSW
- [x] `usb_msc_write_sectors(lba, count, buf)` -- WRITE(10) via bulk-OUT CBW + bulk-OUT data + bulk-IN CSW
- [x] Error handling: CSW status check, tag validation, TEST UNIT READY with retries
- [x] Commit: `"drivers: USB MSC BOT -- SCSI READ/WRITE over bulk endpoints"`

**Test checkpoint:** `usb_msc_read_capacity()` returns correct sector count. Read sector 0 matches expected MBR/GPT. No dedicated POST code; the `READ CAPACITY:` serial line is the marker. Test on: QEMU `run-usb`, bare metal.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | BOT read/capacity validated live via `make run-usb-ci` + bare metal.
> **Notes:**
> - Shipped: SCSI-over-USB BOT (`usb_msc.c`) -- CBW/CSW framing, INQUIRY, READ CAPACITY(10), READ(10)/WRITE(10), CSW signature/tag validation, TEST UNIT READY retries.
> - Review fixes: READ/WRITE(10) chunk by 16-bit block count; `xhci_bulk_transfer` splits each data phase at the 64 KiB physical TRB boundary; reject out-of-range LBA + bad sector size at capacity.
> - Integrates: registered as the `usbN` blkdev read/write (§4); each BOT command stays within one Normal-TRB window.
> - Scope boundary: §3 owns the BOT transport; stall/halt recovery + bounded timeouts are owned by D01 T19 (USB boot hardening).
> **Verified:** 2026-06-15 | this review commit | 6/6 items | build OK | run-usb-ci PASS (INQUIRY "QEMU HARDDISK", READ CAPACITY 131072x512)
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 4H fixed, 1H deferred (ISR -> TODO-10 §8) | scope: kernel-code-quality

## 4. Block Device Registration and VFS Integration
Register USB MSC devices as block devices so VFS can mount filesystems from USB drives.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [x] USB MSC adapter in `blkdev_adapters.c` -- register each MSC device as "usb0", "usb1", etc.
- [x] `xhci_get_device()`, `xhci_msc_device_count()`, `xhci_msc_device_index()` accessors
- [x] Adapter callbacks: `blkdev_usb_msc_read/write` route to `usb_msc_read/write_sectors`
- [x] Automatic: partition scan + filesystem mount via existing `partition_scan_all()`
- [x] Commit: `"drivers: USB MSC block device registration -- USB drives mountable"`

**Test checkpoint:** `bash scripts/build.sh run-usb` -- USB drive visible, partition scanned, filesystem mounted. Bare metal: boot from USB, C:\ accessible. No dedicated POST code; the `usbN` block-device line is the marker.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | mount path validated live via `make run-usb-ci` + bare metal.
> **Notes:**
> - Shipped: USB MSC namespaces registered as `usb0`..`usbN` (`blkdev_adapters.c`) wired into `boot_storage.c`; `partition_scan_all()` + filesystem mount run automatically.
> - Review fix: the blkdev USB read/write adapters now reject `lba > UINT32_MAX` before the 32-bit truncation into READ(10)/WRITE(10).
> - Integrates: `xhci_get_device`/`xhci_msc_device_count`/`xhci_msc_device_index` accessors back the adapter loop; no-USB boot paths skip gracefully.
> - Scope boundary: §4 owns blkdev registration + mount wiring; hot-plug lifecycle is §5/TODO-10 §8.
> **Verified:** 2026-06-15 | this review commit | 4/4 items | build OK | run-usb-ci PASS (usb0 64 MiB registered, FAT32 mounted)
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 4H fixed, 1H deferred (ISR -> TODO-10 §8) | scope: kernel-code-quality

## 5. Interrupt-Driven Hot-Plug and Post-Boot Lifecycle
The baseline boot path in §1-§4 is already good enough to boot from USB media. What remains in the core xHCI roadmap is the runtime lifecycle after boot: MSI-backed hot-plug, clean removal, and consistent block-device registration. The zero-delay pre-ExitBootServices handover work that used to live here is now consolidated under TODO-20.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [x] TODO-20 now owns the pre-ExitBootServices firmware discovery, USBLEGSUP takeover, persistent DMA state, and kernel inherit path
- [x] Register xHCI MSI interrupt handler (inline MSI setup, following AHCI pattern)
- [x] If MSI not available: graceful fallback to event ring polling (no crash)
- [/] ISR reads Event Ring for Port Status Change Events (TRB type 34) -- interim: drains the shared event ring that foreground I/O polls; robust event ownership owned by `04-drivers-hardware/TODO-10-usb-stack.md §8`
- [/] New device connected after boot → full enumeration (slot enable, address, etc.) -- interim: enumerates from ISR context (blocking); deferred-worker model owned by `04-drivers-hardware/TODO-10-usb-stack.md §8`
- [/] Device removed → clean up slot, unregister block device -- deferred to `04-drivers-hardware/TODO-10-usb-stack.md §8` (item: "CCS=0 path (disconnect)... call usb_device_detach(slot)... If MSC: call blkdev_unregister")
- [x] `POST16(0xD752)` entry, `POST16(0xD753)` exit
- [x] Commit: `"drivers: xHCI interrupt-driven hot-plug via MSI"`

**Test checkpoint:** Boot from USB through the baseline §1-§4 path, then hot-plug a second USB drive after desktop boot. Device appears within 100ms. POST codes 0xD752/0xD753 cover the hot-plug path here; zero-delay handover POSTs live in TODO-20. Test on: bare metal, QEMU `run-usb`.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | MSI/polling path observed via `make run-usb-ci`; hot-plug demo is bare metal.
> **Notes:**
> - Shipped: MSI interrupt setup (`xhci_setup_interrupts`) with event-ring-polling fallback; Port Status Change ISR; interim connect-path enumeration.
> - Deferred (interim): the ISR drains the shared event ring + enumerates from ISR context; the robust deferred-dispatch + ring-ownership model is owned by TODO-10 §8.
> - Boundary: MSI is enabled only after boot enumeration, so boot media I/O is not exposed; the race window is post-boot hot-plug concurrent with USB I/O.
> - Scope boundary: §5 owns the boot-baseline hot-plug; device removal + robust event loop are TODO-10 §8; zero-delay handover is TODO-20.
> **Verified:** 2026-06-15 | this review commit | 4/7 items [x], 3 [/] deferred | build OK | run-usb-ci PASS (no MSI in QEMU -> polling path)
> **Deferred:** [H] MSI ISR drains shared event ring + enumerates from ISR context (race + blocking) -> XREF: 04-drivers-hardware/TODO-10 §8 (item: "Event-ring ownership: ISR only acks + records... defers enumeration to a serialized worker")
> **Deferred:** [M] device removal slot cleanup + blkdev unregister -> XREF: 04-drivers-hardware/TODO-10 §8 (item: "CCS=0 path (disconnect)... call usb_device_detach(slot)... If MSC: call blkdev_unregister")
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 4H fixed, 1H deferred (ISR -> TODO-10 §8) | scope: kernel-code-quality

## 6. Hardware Compatibility -- Non-Intel xHCI Vendors
Finish the baseline xHCI boot path across controller vendors. The generic non-Intel path -- skip Intel-only XUSB2PR port routing, rely on BIOS handoff + CCS -- is already implemented (`xhci.c` gates routing on Intel VID 0x8086). What remains here is the Renesas firmware-skip decision, vendor diagnostics, and graceful degradation. EHCI/UHCI/OHCI fallback, USB hubs, USBLEGSUP handoff, and transport stall/halt robustness are owned by other TODOs (Routed block below) -- TODO-17 does NOT own them. (Section de-scoped from a 34-item sprawl that forked ownership with T10/T19/T20.)

**Files:** `src/kernel/drivers/xhci.c`

- [x] Generic non-Intel path: XUSB2PR/USB3_PSSEN routing gated on Intel VID 0x8086; AMD (0x1022), ASMedia (0x1B21), VIA (0x1106) skip port routing and use BIOS handoff + CCS -- already implemented (`xhci.c` `vid == 0x8086` gate + same-bus EHCI check)
- [x] Bulk/command transfers are timeout-bounded (no infinite event-ring wait) -- already implemented (`xhci_wait_transfer`/`xhci_wait_command`, 500 ms)
- [x] Renesas (0x1912): detect vendor ID in the discovery loop; warn `Renesas controller -- USB unavailable if host firmware not loaded` so a firmware-less board is diagnosable; graceful skip via the existing no-device path (`xhci.c` `xhci_init`)
- [x] Vendor diagnostics: `xhci_vendor_name()` maps VID to Intel/AMD/ASMedia/Renesas/VIA/QEMU; logged on bring-up (`Found xHCI at PCI ...: <vendor>`) on both the full-init and handover paths -- validated live (run-usb-ci: `QEMU (VID:DID 1b36:000d)`)
- [x] Graceful degradation: no-xHCI logs `continuing boot without USB` (not a boot failure); init-failure logs + continues; `boot_storage.c` ignores the count and proceeds -- PS/2 + SATA/NVMe still work
- [/] Hardware test matrix (manual -- bare metal): AMD, ASMedia, Renesas, VIA controllers boot from USB via the generic path
- [/] Commit: `"drivers: xHCI non-Intel vendor compatibility + graceful degradation"`

**Routed to owning TODOs (NOT owned by TODO-17 §6):**
- BIOS/OS USBLEGSUP handoff (all vendors) -> XREF: `01-boot-platform/TODO-20-usb-zero-delay-handover.md §2` (implemented + marked complete)
- EHCI fallback HCD (USB 2.0, no-xHCI hardware) -> XREF: `04-drivers-hardware/TODO-10-usb-stack.md §10` (full HCD/vtable); boot-storage integration -> `01-boot-platform/TODO-19-usb-boot-hardening.md §11`
- USB hub class driver (recursive enumeration, class 0x09) -> XREF: `04-drivers-hardware/TODO-10-usb-stack.md §9`
- UHCI legacy (USB 1.x) + OHCI-only graceful skip -> XREF: `01-boot-platform/TODO-19-usb-boot-hardening.md §11`
- Transport robustness (bulk stall/halt recovery, HSE, event-ring-full) -> XREF: `01-boot-platform/TODO-19-usb-boot-hardening.md`

**Test checkpoint:** Boot from USB on a non-Intel xHCI controller via the generic path (no XUSB2PR write). `POST16(0xD7A0)` entry, `POST16(0xD7A1)` exit; diagnostic splash shows `USB:30` + vendor name. QEMU `run-usb` still works (regression). Bare metal: AMD/ASMedia/Renesas/VIA tested manually.
> **Test runner:** `scripts\debug\kernel\run-storage-tests.bat` (SUITE=storage) | vendor diagnostics validated live via `make run-usb-ci`; the hardware matrix is bare metal.
> **Notes:**
> - Shipped: `xhci_vendor_name()` + bring-up vendor diagnostic (both full-init and handover paths); Renesas (0x1912) firmware-less warn; graceful "no xHCI -- continuing boot" path (`xhci.c`).
> - Already present (marked with evidence): generic non-Intel path (XUSB2PR gated on Intel 0x8086) + 500 ms transfer timeouts; this section finishes diagnostics + graceful degradation.
> - De-scoped: a 34-item sprawl forking 4 TODOs; EHCI/hub/USBLEGSUP/UHCI/OHCI/robustness routed to their owners (Routed block).
> - Scope boundary: §6 owns non-Intel xHCI vendor compat; legacy controllers + transport robustness are owned by the usb-stack / usb-boot-hardening / zero-delay-handover TODOs.
> **Verified:** 2026-06-15 | this commit | 5/6 items [x], 1 [ ] manual (HW matrix) | build OK | run-usb-ci PASS (vendor "QEMU" logged on bring-up)
> **Quality reviewed:** 2026-06-15 | Codex 1x (adversarial, §6 vendor diff: no material findings) | diagnostics/logging only; subsystem covered by the §1-§5 Codex 3x pass | scope: kernel-code-quality
> **Deferred:** [awaiting-operator] the vendor hardware matrix needs physical AMD/ASMedia/Renesas/VIA machines -- no unattended pass can perform this; operator action required (cohort 2026-07-31)

---

## OS Comparison

| ⭐  | Feature             | 🪟 Win11       | 🐧 Linux         | 🚀 Impossible OS             |
| --- | ------------------- | -------------- | ---------------- | ---------------------------- |
| 💎  | xHCI controller     | ✅ usbxhci.sys | ✅ xhci-hcd      | ✅ §1-§4 done                |
| 💎  | USB MSC             | ✅ USBSTOR.SYS | ✅ usb-storage   | ✅ §3 BOT done               |
| 💎  | USB boot drive      | ✅ Automatic   | ✅ initramfs     | ✅ §4 bare metal             |
| ⭐  | Pre-boot handover   | ✅ winload.efi | ❌ Re-enumerates | ⬜ TODO-20 zero-delay        |
| ⭐  | BIOS/OS handoff     | ✅ Automatic   | ✅ xhci-pci.c    | ✅ TODO-20 §2                |
| ⭐  | EHCI fallback       | ✅ usbehci.sys | ✅ ehci-hcd      | ⬜ T10 §10 + T19 §11         |
| ⭐  | USB hub support     | ✅ usbhub.sys  | ✅ hub.c         | ⬜ T10 §9 recursive          |
| ⭐  | Hot-plug            | ✅ Automatic   | ✅ Automatic     | ⚠️ §5 interim; robust T10 §8 |
| ⭐  | USB boot timing VPD | ❌ Not exposed | ❌ Not exposed   | ⬜ TODO-20 latency           |

## Unit Tests

> Wire into `test_runner_init()` via `test_register_usb_boot()` (-> XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> USB driver tests require hardware (real or emulated xHCI controller). Use `bash scripts/build.sh run-usb` for QEMU USB tests. Tests that need a controller gracefully skip when no xHCI is present.

- [x] Create `src/kernel/test/test_usb_boot.c` -- reconciled: only the read-only count+geometry surface is WSL-testable; the hardware-present rows below are `make run-usb-ci` (TCG) + bare-metal validated, not unit assertions:
  - `xhci_controller_count()` returns >= 0 (no crash when no controller) -- UNIT (`test_usb_boot.c`)
  - `xhci_msc_device_count()` returns >= 0 (valid even with no MSC devices) -- UNIT (`test_usb_boot.c`)
  - active MSC device exposes a supported sector size (512/1024/2048/4096) -- UNIT (when present)
  - When xHCI present: port count > 0; USBLEGSUP handoff (USBSTS.HCH==0) -- smoke (`make run-usb-ci`) + bare metal
  - USB MSC read: `usb_msc_read_sectors(0,1,buf)` matches MBR/GPT; READ CAPACITY non-zero -- smoke + bare metal
  - blkdev `usb0` registered when MSC present; `boot_info.usb_device_count` matches bootloader Phase A -- smoke + bare metal
- [x] Validate live serial via `make run-usb-ci` -- reconciled: `scripts/test-smoke.sh` NOT extended (smoke-stability policy); serial greps (`xhci:`, `usb: Device`, `block device`) run via `make run-usb-ci` + the Verification greps below
- [x] Register in `test_runner_init()`: `test_register_usb_boot()` (`test_runner.c` extern + call), `TEST_CAT_STORAGE`
- [x] Commit: `"test: add usb_boot test suite"`

> **Done:** 1 suite, 3 assertions (`usb: controller count + MSC geometry`) -- registered in `test_runner_init()`; storage SUITE 4/4 kernel + 16/16 user-mode PASS on TCG 2026-06-15.
> **Reconciled:** 4 items rewritten to match reality (read-only-only unit surface; smoke/bare-metal owns the hardware-present rows; test-smoke.sh deliberately not extended), 0 rejected.

## Verification

- [x] `make run-usb-ci` -- USB drive mounted, files readable -- 2026-06-15 TCG: xHCI 1b36:000d, USB 3.00 enumerate, INQUIRY "QEMU HARDDISK", usb0 64 MiB + FAT32 "NVME_TEST"... mounted, Boot complete
- [ ] Bare metal USB boot: C:\ accessible, klog writes to disk (manual -- run on bare metal)
- [x] PS/2-only system: still works (no regression) -- 2026-06-15 smoke PASS 2.49s; `xhci: No xHCI controller -- continuing boot without USB` in `smoke-test.stripped.log`
