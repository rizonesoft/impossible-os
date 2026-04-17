# TODO-17 -- xHCI, USB Storage & USB HID (Boot-Critical)

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
- Works on Intel, AMD, ASMedia, and EHCI-only hardware (§6)

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI controller bring-up and port scan         | --          |  [x]   |
| 💎  |   2   | USB device enumeration and configuration       | §1         |  [x]   |
| 💎  |   3   | USB MSC BOT (Bulk-Only Transport) driver       | §2         |  [x]   |
| 💎  |   4   | Block device registration and VFS integration  | §3         |  [x]   |
| 💎  |   5   | Interrupt-driven hot-plug and post-boot lifecycle | §1-§4   |  [/]   |
| ⭐  |   6   | Hardware compatibility (95%+ of systems)       | §1, §5     |  [ ]   |

---

## 1. xHCI Controller Bring-Up and Port Scan
Verify and fix the existing xHCI controller initialization. Currently logs "No xHCI controllers found" on some platforms.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/pci.c`

- [x] Verify PCI discovery finds xHCI (class 0x0C, subclass 0x03, prog-if 0x30)
- [x] Controller halt, reset, DCBAA allocation, command ring, event ring -- audit existing code
- [x] Port scan: detect attached USB devices, log port status
- [x] Map xHCI BAR0 via `vmm_map_mmio_uc()` (MMIO registers need UC mapping)
- [x] Commit: `"drivers: xHCI controller bring-up verified on QEMU + bare metal"`

**Test checkpoint:** Serial shows `xhci: N ports, M devices attached`. POST code 0xD700. Test on: QEMU `run-usb`, bare metal.

## 2. USB Device Enumeration and Configuration
Complete the device enumeration path: slot enable → Address Device → GET_DESCRIPTOR → SET_CONFIGURATION.

**Files:** `src/kernel/drivers/xhci_dev.c`

- [x] Audit existing `xhci_dev_enumerate()` -- full 9-step enumeration implemented and verified
- [x] Parse device descriptor: class, subclass, protocol, VID:PID
- [x] Parse configuration descriptor: find MSC interface (class 0x08) -- HID (class 0x03) deferred to HID driver section
- [x] Configure Endpoint for bulk-IN/OUT (MSC) -- transfer rings allocated, Configure Endpoint command issued
- [x] Log: `usb: Device 46f4:0001 enumerated on port 1 (slot 1) [MSC]`
- [x] Commit: `"drivers: USB device enumeration -- MSC interfaces detected"`

**Test checkpoint:** Serial shows detected USB devices with class info. POST code 0xD701. Test on: QEMU `run-usb`, bare metal.

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

**Test checkpoint:** `usb_msc_read_capacity()` returns correct sector count. Read sector 0 matches expected MBR/GPT. POST code 0xD702. Test on: QEMU `run-usb`, bare metal.

## 4. Block Device Registration and VFS Integration
Register USB MSC devices as block devices so VFS can mount filesystems from USB drives.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [x] USB MSC adapter in `blkdev_adapters.c` -- register each MSC device as "usb0", "usb1", etc.
- [x] `xhci_get_device()`, `xhci_msc_device_count()`, `xhci_msc_device_index()` accessors
- [x] Adapter callbacks: `blkdev_usb_msc_read/write` route to `usb_msc_read/write_sectors`
- [x] Automatic: partition scan + filesystem mount via existing `partition_scan_all()`
- [x] Commit: `"drivers: USB MSC block device registration -- USB drives mountable"`

**Test checkpoint:** `bash scripts/build.sh run-usb` -- USB drive visible, partition scanned, filesystem mounted. Bare metal: boot from USB, C:\ accessible. POST code 0xD703.

## 5. Interrupt-Driven Hot-Plug and Post-Boot Lifecycle
The baseline boot path in §1-§4 is already good enough to boot from USB media. What remains in the core xHCI roadmap is the runtime lifecycle after boot: MSI-backed hot-plug, clean removal, and consistent block-device registration. The zero-delay pre-ExitBootServices handover work that used to live here is now consolidated under TODO-20.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [x] TODO-20 now owns the pre-ExitBootServices firmware discovery, USBLEGSUP takeover, persistent DMA state, and kernel inherit path
- [x] Register xHCI MSI interrupt handler (inline MSI setup, following AHCI pattern)
- [x] If MSI not available: graceful fallback to event ring polling (no crash)
- [x] ISR reads Event Ring for Port Status Change Events (TRB type 34)
- [x] New device connected after boot → full enumeration (slot enable, address, etc.)
- [ ] Device removed → clean up slot, unregister block device -- deferred to `04-drivers-hardware/TODO-10-usb-stack.md §8` (hot-plug lifecycle)
- [x] `POST16(0xD752)` entry, `POST16(0xD753)` exit
- [x] Commit: `"drivers: xHCI interrupt-driven hot-plug via MSI"`

**Test checkpoint:** Boot from USB through the baseline §1-§4 path, then hot-plug a second USB drive after desktop boot. Device appears within 100ms. POST codes 0xD752/0xD753 cover the hot-plug path here; zero-delay handover POSTs live in TODO-20. Test on: bare metal, QEMU `run-usb`.

## 6. Hardware Compatibility -- 95%+ of Systems
Make USB boot work on 95%+ of real hardware: Intel, AMD, third-party xHCI controllers, EHCI fallback, USB hubs, and BIOS/OS handoff. Currently only Intel with specific port routing is tested.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/ehci.c` (new)

### BIOS/OS Handoff
- [ ] Core USBLEGSUP handoff is owned by `TODO-20 §2` -- reuse that implementation for all controller types
- [ ] Verify USBLEGSUP works identically on AMD, ASMedia, Renesas, VIA (same xHCI spec §4.22.1)
- [ ] EHCI variant: USBLEGSUP is a PCI capability (not xHCI extended cap) -- same semaphore concept, different register offset

### AMD xHCI Support
- [ ] AMD chipsets (vendor 0x1022) route all ports to xHCI by default -- no XUSB2PR needed
- [ ] Verify BIOS handoff works on AMD (same USBLEGSUP mechanism)
- [ ] Test on AMD system if available

### Third-Party xHCI Controllers (ASMedia, Renesas, VIA)
- [ ] ASMedia (vendor 0x1B21): no port routing, just BIOS handoff + standard init
- [ ] Renesas (vendor 0x1912): may need firmware upload -- detect and skip if unsupported
- [ ] VIA (vendor 0x1106): standard xHCI, BIOS handoff only
- [ ] Generic path: if vendor != Intel, skip port routing, rely on BIOS handoff + CCS

### EHCI Fallback Driver
- [ ] For systems with NO xHCI controller (only EHCI, prog-if 0x20)
- [ ] EHCI controller init: halt, reset, PERIODICLISTBASE, ASYNCLISTADDR
- [ ] EHCI BIOS/OS handoff via `USBLEGSUP` (PCI capability, same concept as xHCI)
- [ ] Async schedule: QH + qTD for control and bulk transfers
- [ ] Port reset + device enumeration (same USB protocol, different transport)
- [ ] Register as block device via same `usb_msc` layer
- [ ] This is a significant driver (~1000-2000 lines) -- only implement if xHCI is absent

### UHCI/OHCI Legacy Controllers (pre-2008 hardware)
- [ ] UHCI (prog-if 0x00): Intel/VIA USB 1.x -- Frame List + Transfer Descriptors
- [ ] OHCI (prog-if 0x10): AMD/NEC/others USB 1.x -- HCCA + Endpoint Descriptors
- [ ] Both are USB 1.1 (12 Mbps max) -- sufficient for keyboards, mice, and slow storage
- [ ] Detect at PCI scan: if no xHCI and no EHCI, try UHCI/OHCI
- [ ] Shared USB device layer: same `usb_msc` + `usb_hid` code on top, different transport
- [ ] Priority: LOW -- only needed for hardware older than ~2008. Log warning if only UHCI/OHCI found.
- [ ] If not implemented: log `"USB: only UHCI/OHCI found -- USB not supported on this hardware"`

### USB Hub Support
- [ ] Detect hub devices (class 0x09) during enumeration
- [ ] Hub descriptor: number of ports, power characteristics
- [ ] Set port power, poll port status, reset ports with connected devices
- [ ] Enumerate devices behind hubs (recursive, max 5 levels per USB spec)
- [ ] Without this, devices plugged into USB hubs are invisible

### Robustness
- [ ] Timeout all bulk transfers (currently infinite wait on event ring)
- [ ] Bulk reset recovery on stall (BOT spec §5.3.4: clear HALT + reset endpoint)
- [ ] Handle controller errors: Host System Error (HSE), event ring full
- [ ] Graceful degradation: if xHCI init fails, log and continue (don't hang boot)
- [ ] Commit: `"drivers: xHCI hardware compatibility -- BIOS handoff, AMD, EHCI fallback"`

**Test checkpoint:** POST codes: `POST16(0xD7A0)` entry, `POST16(0xD7A1)` exit. If crash at 0xD7A0: BIOS handoff or vendor-specific init failed -- check serial for vendor ID. Test matrix (diagnostic splash shows `USB:XX` prog-if codes):
- `USB:30` Intel (i5-11600K): ✅ verified -- `TODO-20` handover replaces the old port-routing delay path
- `USB:30` AMD: BIOS handoff only, no port routing needed
- `USB:30,20` Intel (xHCI+EHCI): port routing moves EHCI ports to xHCI
- `USB:20` EHCI only: EHCI fallback driver handles enumeration
- `USB:00` UHCI only: log warning, graceful skip (low priority)
- `USB:10` OHCI only: log warning, graceful skip (low priority)
- USB hub: device behind hub enumerated
- QEMU `run-usb`: still works (regression check)

---

## OS Comparison

| ⭐ | Feature              | 🪟 Win11              | 🐧 Linux              | 🚀 Impossible OS          |
|----|----------------------|--------------------|--------------------|------------------------|
| 💎 | xHCI controller      | ✅ usbxhci.sys    | ✅ xhci-hcd        | ✅ §1-§4 done          |
| 💎 | USB MSC              | ✅ USBSTOR.SYS    | ✅ usb-storage      | ✅ §3 BOT done         |
| 💎 | USB boot drive       | ✅ Automatic       | ✅ initramfs        | ✅ §4 bare metal       |
| ⭐ | Pre-boot handover    | ✅ winload.efi     | ❌ Re-enumerates    | ⬜ TODO-20 zero-delay  |
| ⭐ | BIOS/OS handoff      | ✅ Automatic       | ✅ xhci-pci.c       | ✅ TODO-20 §2          |
| ⭐ | EHCI fallback        | ✅ usbehci.sys     | ✅ ehci-hcd         | ⬜ §6 legacy HW        |
| ⭐ | USB hub support      | ✅ usbhub.sys      | ✅ hub.c            | ⬜ §6 recursive        |
| ⭐ | Hot-plug             | ✅ Automatic       | ✅ Automatic        | ✅ §5C MSI interrupt   |
| ⭐ | USB boot timing VPD  | ❌ Not exposed     | ❌ Not exposed      | ⬜ TODO-20 latency     |

## Unit Tests

> Wire into `test_runner_init()` via `test_register_usb_boot()` (-> XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> USB driver tests require hardware (real or emulated xHCI controller). Use `bash scripts/build.sh run-usb` for QEMU USB tests. Tests that need a controller gracefully skip when no xHCI is present.

- [ ] Create `src/kernel/test/test_usb_boot.c` with:
  - `xhci_controller_count()` returns >= 0 (no crash when no controller present)
  - When xHCI present: `xhci_get_port_count()` returns > 0
  - When xHCI present: USBLEGSUP handoff completed (controller OS-owned, `USBSTS.HCH == 0` when running)
  - `xhci_msc_device_count()` returns >= 0 (valid count even when no MSC devices attached)
  - USB MSC read: `usb_msc_read_sectors(0, 1, buf)` on first MSC device returns valid MBR/GPT header (when device present)
  - `usb_msc_read_capacity()` returns non-zero sector count and valid sector size (512 or 4096) for attached MSC device
  - Block device registration: `blkdev_find("usb0")` returns non-NULL when USB MSC device is present
  - `boot_info.usb_device_count` matches number of devices discovered by bootloader Phase A
- [ ] Add to `scripts/test-smoke.sh` (with `run-usb` target):
  - Grep serial for `xhci:` (controller discovered) or `xhci: no controller` (graceful skip)
  - Grep serial for `usb: Device` (device enumerated) when USB drive attached
  - Grep serial for `block device registered` when USB MSC present
- [ ] Register in `test_runner_init()`: `test_register_usb_boot()`
- [ ] Commit: `"test: add usb_boot test suite"`

## Verification

- [ ] `bash scripts/build.sh run-usb` -- USB drive mounted, files readable
- [ ] Bare metal USB boot: C:\ accessible, klog writes to disk
- [ ] PS/2-only system: still works (no regression)
