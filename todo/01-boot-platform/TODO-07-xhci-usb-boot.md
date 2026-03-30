# TODO-07 — xHCI, USB Storage & USB HID (Boot-Critical)

> **Goal:** Enable USB storage and USB input after ExitBootServices so bare metal can: (1) mount C:\ from a USB boot drive, and (2) use USB keyboards/mice on systems without PS/2. Without this, bare metal boots from USB show desktop but can't access any filesystem or accept input on USB-only systems.

> [!IMPORTANT]
> This TODO extracts the **boot-critical** xHCI, USB MSC, and USB HID sections from `04-drivers-hardware/TODO-09-usb-stack.md`. Advanced USB features (hub driver, hot-plug, EHCI fallback, Bluetooth, CDC, full HID report parsing) remain in TODO-09. After this TODO, USB drives are mountable and USB keyboards/mice work in boot-protocol mode.

> [!NOTE]
> Partial xHCI implementation exists: `xhci.c` (controller init, DCBAA, TRB rings, port scan), `xhci_dev.c` (slot enable, Address Device, GET_DESCRIPTOR, SET_CONFIGURATION), `xhci_ring.c` (TRB ring management). **Do NOT rewrite — complete it.**

> [!NOTE]
> **Architecture: built-in now, bootloader-loaded later.** Windows loads `usbxhci.sys` and `USBSTOR.SYS` as boot-start drivers from the EFI partition via `winload.efi` — they're not part of `ntoskrnl.exe`. Linux uses initramfs. For now, xHCI/MSC/HID are built into the kernel binary to get bare metal working. When the kernel module loader exists (`04-drivers-hardware/TODO-01`), refactor these into separate `.sys` driver files loaded by `bootx64.efi` from `\EFI\ImpossibleOS\drivers\` before kernel entry.

## Inputs

- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) — xHCI controller driver (partial)
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) — device enumeration (partial)
- [`src/kernel/drivers/xhci_ring.c`](../../src/kernel/drivers/xhci_ring.c) — TRB ring management
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c) — PS/2 keyboard (injection target for USB HID)
- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) — PS/2 mouse (injection target for USB HID)
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md` — advanced USB features (hub, hot-plug, EHCI, BT, CDC)
- → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §5` — MSI/MSI-X (xHCI uses MSI)

## Outcome

- xHCI controller discovered, initialized, and operational on bare metal
- USB mass storage devices enumerated and registered as block devices
- USB keyboards and mice work in boot-protocol mode
- Boot from USB drive: filesystem accessible, input working
- PS/2 and USB input coexist — both active if both present

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI controller bring-up and port scan         | —          |  [x]   |
| 💎  |   2   | USB device enumeration and configuration       | §1         |  [x]   |
| 💎  |   3   | USB MSC BOT (Bulk-Only Transport) driver       | §2         |  [x]   |
| 💎  |   4   | Block device registration and VFS integration  | §3         |  [x]   |
| ⭐  |   5   | Interrupt-driven port detection + Intel routing | §1         |  [ ]   |
| 💎  |   6   | xHCI interrupt endpoint setup for HID          | §2         |  [ ]   |
| 💎  |   7   | USB HID boot-protocol keyboard driver          | §6         |  [ ]   |
| 💎  |   8   | USB HID boot-protocol mouse driver             | §6         |  [ ]   |
| 💎  |   9   | Input source priority and coexistence          | §7, §8     |  [ ]   |
| ⭐  |  10   | Hardware compatibility (90%+ of systems)       | §1, §5     |  [ ]   |

---

## 1. xHCI Controller Bring-Up and Port Scan
Verify and fix the existing xHCI controller initialization. Currently logs "No xHCI controllers found" on some platforms.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/pci.c`

- [x] Verify PCI discovery finds xHCI (class 0x0C, subclass 0x03, prog-if 0x30)
- [x] Controller halt, reset, DCBAA allocation, command ring, event ring — audit existing code
- [x] Port scan: detect attached USB devices, log port status
- [x] Map xHCI BAR0 via `vmm_map_mmio_uc()` (MMIO registers need UC mapping)
- [x] Commit: `"drivers: xHCI controller bring-up verified on QEMU + bare metal"`

**Test checkpoint:** Serial shows `xhci: N ports, M devices attached`. POST code 0xD700. Test on: QEMU `run-usb`, bare metal.

## 2. USB Device Enumeration and Configuration
Complete the device enumeration path: slot enable → Address Device → GET_DESCRIPTOR → SET_CONFIGURATION.

**Files:** `src/kernel/drivers/xhci_dev.c`

- [x] Audit existing `xhci_dev_enumerate()` — full 9-step enumeration implemented and verified
- [x] Parse device descriptor: class, subclass, protocol, VID:PID
- [x] Parse configuration descriptor: find MSC interface (class 0x08) — HID (class 0x03) deferred to HID driver section
- [x] Configure Endpoint for bulk-IN/OUT (MSC) — transfer rings allocated, Configure Endpoint command issued
- [x] Log: `usb: Device 46f4:0001 enumerated on port 1 (slot 1) [MSC]`
- [x] Commit: `"drivers: USB device enumeration — MSC interfaces detected"`

**Test checkpoint:** Serial shows detected USB devices with class info. POST code 0xD701. Test on: QEMU `run-usb`, bare metal.

## 3. USB MSC BOT (Bulk-Only Transport) Driver
Implement the SCSI-over-USB transport layer: CBW/CSW framing, INQUIRY, READ CAPACITY, READ(10), WRITE(10).

**Files:** `src/kernel/drivers/usb_msc.c` (new), `include/kernel/drivers/usb_msc.h` (new)

- [x] CBW (Command Block Wrapper) and CSW (Command Status Wrapper) structures
- [x] `usb_msc_inquiry()` — identify device type and name
- [x] `usb_msc_read_capacity()` — get sector count and sector size
- [x] `usb_msc_read_sectors(lba, count, buf)` — READ(10) via bulk-OUT CBW + bulk-IN data + bulk-IN CSW
- [x] `usb_msc_write_sectors(lba, count, buf)` — WRITE(10) via bulk-OUT CBW + bulk-OUT data + bulk-IN CSW
- [x] Error handling: CSW status check, tag validation, TEST UNIT READY with retries
- [x] Commit: `"drivers: USB MSC BOT — SCSI READ/WRITE over bulk endpoints"`

**Test checkpoint:** `usb_msc_read_capacity()` returns correct sector count. Read sector 0 matches expected MBR/GPT. POST code 0xD702. Test on: QEMU `run-usb`.

## 4. Block Device Registration and VFS Integration
Register USB MSC devices as block devices so VFS can mount filesystems from USB drives.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [x] USB MSC adapter in `blkdev_adapters.c` — register each MSC device as "usb0", "usb1", etc.
- [x] `xhci_get_device()`, `xhci_msc_device_count()`, `xhci_msc_device_index()` accessors
- [x] Adapter callbacks: `blkdev_usb_msc_read/write` route to `usb_msc_read/write_sectors`
- [x] Automatic: partition scan + filesystem mount via existing `partition_scan_all()`
- [x] Commit: `"drivers: USB MSC block device registration — USB drives mountable"`

**Test checkpoint:** `bash scripts/build.sh run-usb` — USB drive visible, partition scanned, filesystem mounted. Bare metal: boot from USB, C:\ accessible. POST code 0xD703.

## 5. Interrupt-Driven Port Detection and Intel Port Routing
Replace the fixed 500ms delay after Intel EHCI-to-xHCI port routing with proper interrupt-driven port status change detection. Currently works but wastes 500ms on every boot.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/xhci_ring.c`

- [ ] Register xHCI MSI/MSI-X interrupt handler during controller init
- [ ] After Intel port routing (XUSB2PR + USB3_PSSEN), enable Port Status Change Events
- [ ] ISR reads Event Ring for TRB type 34 (Port Status Change Event)
- [ ] On PSC event: identify port, read PORTSC, start enumeration if CCS=1
- [ ] Remove the fixed 500ms `xhci_delay_us()` — interrupt fires within ~50ms
- [ ] Fallback: if no interrupt after 1s, fall back to PORTSC polling (non-Intel or broken MSI)
- [ ] Non-Intel xHCI controllers: skip port routing, rely on normal CCS detection
- [ ] Hot-plug support: same ISR handles devices connected after boot
- [ ] Commit: `"drivers: xHCI interrupt-driven port detection — replace 500ms delay"`

**Test checkpoint:** Boot from USB — device detected via interrupt within 50ms (vs 500ms fixed delay). Serial log shows `xhci: PSC event on port N`. Hot-plug: plug USB drive after boot, device appears. QEMU `run-usb` still works.

## 6. xHCI Interrupt Endpoint Setup for HID
Configure interrupt-IN endpoints for HID devices so the xHCI controller polls them periodically.

**Files:** `src/kernel/drivers/xhci_dev.c`

- [ ] Detect HID interface (class 0x03) during USB enumeration (§2)
- [ ] `SET_PROTOCOL(0)` — switch to boot protocol (simpler reports)
- [ ] `SET_IDLE(0)` — report only on change
- [ ] Configure interrupt-IN endpoint via Configure Endpoint command
- [ ] Set up Transfer Ring with TRBs for periodic interrupt-IN transfers
- [ ] Event ring callback: when interrupt-IN completes, deliver report to HID driver
- [ ] Commit: `"drivers: xHCI interrupt endpoint setup for USB HID devices"`

**Test checkpoint:** USB keyboard/mouse detected, interrupt endpoint configured. POST code 0xD704. Test on: QEMU `run-usb`, bare metal.

## 7. USB HID Boot-Protocol Keyboard Driver
Parse 8-byte boot-protocol keyboard reports and inject key events into the input subsystem.

**Files:** `src/kernel/drivers/usb_hid_kbd.c` (new)

- [ ] Parse boot keyboard report: byte 0 = modifiers, bytes 2-7 = keycodes
- [ ] Convert USB HID usage codes to PS/2 scancodes (lookup table, ~104 entries)
- [ ] Inject into `keyboard_inject_scancode()` — same path as PS/2
- [ ] Handle key-up: compare current vs previous report, detect released keys
- [ ] Commit: `"drivers: USB HID boot-protocol keyboard — key events via interrupt-IN"`

**Test checkpoint:** USB keyboard: type characters, see them in terminal. POST code 0xD705. Test on: QEMU `run-usb`, bare metal.

## 8. USB HID Boot-Protocol Mouse Driver
Parse 3-byte boot-protocol mouse reports and inject mouse events.

**Files:** `src/kernel/drivers/usb_hid_mouse.c` (new)

- [ ] Parse boot mouse report: byte 0 = buttons, byte 1 = X delta, byte 2 = Y delta
- [ ] Inject into mouse subsystem via `mouse_inject_state(x, y, buttons)`
- [ ] Commit: `"drivers: USB HID boot-protocol mouse — movement + buttons"`

**Test checkpoint:** USB mouse: cursor moves on screen. POST code 0xD706. Test on: QEMU `run-usb`, bare metal.

## 9. Input Source Priority and Coexistence
Ensure PS/2 and USB input sources coexist without conflict.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`

- [ ] Both PS/2 and USB keyboard active simultaneously (both inject scancodes)
- [ ] Both PS/2 and USB mouse active simultaneously
- [ ] If PS/2 not detected (`acpi_has_8042()` = 0): USB is sole input source
- [ ] Log active input sources at boot
- [ ] Commit: `"drivers: input source coexistence — PS/2 + USB active simultaneously"`

**Test checkpoint:** System with both PS/2 and USB — both work. USB-only system — works. POST code 0xD707. Test on: QEMU `run-usb`, bare metal.

## 10. Hardware Compatibility — 90%+ of Systems
Make USB boot work on the vast majority of real hardware: Intel, AMD, third-party xHCI controllers, and systems with only EHCI (no xHCI). Currently only Intel with specific port routing is tested.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/ehci.c` (new)

### BIOS/OS Handoff (xHCI spec §4.22.1)
- [ ] Read `USBLEGSUP` capability register (Extended Capability ID = 1)
- [ ] Set HC OS Owned Semaphore bit, wait for BIOS Owned Semaphore to clear
- [ ] Timeout after 1s — if BIOS doesn't release, force-clear and proceed
- [ ] Must happen BEFORE controller halt/reset (currently missing)
- [ ] Without this, BIOS may still own the controller and interfere with our driver

### AMD xHCI Support
- [ ] AMD chipsets (vendor 0x1022) route all ports to xHCI by default — no XUSB2PR needed
- [ ] Verify BIOS handoff works on AMD (same USBLEGSUP mechanism)
- [ ] Test on AMD system if available

### Third-Party xHCI Controllers (ASMedia, Renesas, VIA)
- [ ] ASMedia (vendor 0x1B21): no port routing, just BIOS handoff + standard init
- [ ] Renesas (vendor 0x1912): may need firmware upload — detect and skip if unsupported
- [ ] VIA (vendor 0x1106): standard xHCI, BIOS handoff only
- [ ] Generic path: if vendor != Intel, skip port routing, rely on BIOS handoff + CCS

### EHCI Fallback Driver
- [ ] For systems with NO xHCI controller (only EHCI, prog-if 0x20)
- [ ] EHCI controller init: halt, reset, PERIODICLISTBASE, ASYNCLISTADDR
- [ ] EHCI BIOS/OS handoff via `USBLEGSUP` (PCI capability, same concept as xHCI)
- [ ] Async schedule: QH + qTD for control and bulk transfers
- [ ] Port reset + device enumeration (same USB protocol, different transport)
- [ ] Register as block device via same `usb_msc` layer
- [ ] This is a significant driver (~1000-2000 lines) — only implement if xHCI is absent

### UHCI/OHCI Legacy Controllers (pre-2008 hardware)
- [ ] UHCI (prog-if 0x00): Intel/VIA USB 1.x — Frame List + Transfer Descriptors
- [ ] OHCI (prog-if 0x10): AMD/NEC/others USB 1.x — HCCA + Endpoint Descriptors
- [ ] Both are USB 1.1 (12 Mbps max) — sufficient for keyboards, mice, and slow storage
- [ ] Detect at PCI scan: if no xHCI and no EHCI, try UHCI/OHCI
- [ ] Shared USB device layer: same `usb_msc` + `usb_hid` code on top, different transport
- [ ] Priority: LOW — only needed for hardware older than ~2008. Log warning if only UHCI/OHCI found.
- [ ] If not implemented: log `"USB: only UHCI/OHCI found — USB not supported on this hardware"`

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
- [ ] Commit: `"drivers: xHCI hardware compatibility — BIOS handoff, AMD, EHCI fallback"`

**Test checkpoint:** Test matrix (diagnostic splash shows `USB:XX` prog-if codes):
- `USB:30` Intel (i5-11600K): ✅ verified — port routing + 500ms settle
- `USB:30` AMD: BIOS handoff only, no port routing needed
- `USB:30,20` Intel (xHCI+EHCI): port routing moves EHCI ports to xHCI
- `USB:20` EHCI only: EHCI fallback driver handles enumeration
- `USB:00` UHCI only: log warning, graceful skip (low priority)
- `USB:10` OHCI only: log warning, graceful skip (low priority)
- USB hub: device behind hub enumerated
- QEMU `run-usb`: still works (regression check)

---

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                     |
|----|-------------------------|-----------------------------|------------------------------|-----------------------------------|
| 💎 | xHCI controller         | ✅ usbxhci.sys             | ✅ xhci-hcd                  | ✅ §1-§4 done, §5 planned        |
| 💎 | USB MSC                 | ✅ USBSTOR.SYS             | ✅ usb-storage               | ✅ §3 — BOT transport done       |
| 💎 | USB boot drive access   | ✅ Automatic               | ✅ initramfs + usb-storage   | ✅ §4 — verified on bare metal   |
| 💎 | USB HID keyboard        | ✅ hidusb.sys + kbdhid.sys | ✅ usbhid + hid-generic      | ⬜ §7 — boot protocol            |
| 💎 | USB HID mouse           | ✅ hidusb.sys + mouhid.sys | ✅ usbhid + hid-generic      | ⬜ §8 — boot protocol            |
| 💎 | PS/2 + USB coexist      | ✅ Automatic               | ✅ Automatic                 | ⬜ §9 — independent, both active |
| ⭐ | BIOS/OS handoff         | ✅ Automatic               | ✅ xhci-pci.c                | ⬜ §10 — USBLEGSUP              |
| ⭐ | EHCI fallback           | ✅ usbehci.sys             | ✅ ehci-hcd                  | ⬜ §10 — for legacy hardware     |
| ⭐ | USB hub support         | ✅ usbhub.sys              | ✅ hub.c                     | ⬜ §10 — recursive enumeration   |

## Verification

- [ ] `bash scripts/build.sh run-usb` — USB drive mounted, files readable
- [ ] Bare metal USB boot: C:\ accessible, klog writes to disk
- [ ] USB keyboard: type in terminal on bare metal
- [ ] USB mouse: cursor moves on bare metal
- [ ] PS/2-only system: still works (no regression)
