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
| 💎  |   1   | xHCI controller bring-up and port scan         | —          |  [ ]   |
| 💎  |   2   | USB device enumeration and configuration       | §1         |  [ ]   |
| 💎  |   3   | USB MSC BOT (Bulk-Only Transport) driver       | §2         |  [ ]   |
| 💎  |   4   | Block device registration and VFS integration  | §3         |  [ ]   |
| 💎  |   5   | xHCI interrupt endpoint setup for HID          | §2         |  [ ]   |
| 💎  |   6   | USB HID boot-protocol keyboard driver          | §5         |  [ ]   |
| 💎  |   7   | USB HID boot-protocol mouse driver             | §5         |  [ ]   |
| 💎  |   8   | Input source priority and coexistence          | §6, §7     |  [ ]   |

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

- [ ] Audit existing `xhci_dev_enumerate()` — fix any incomplete paths
- [ ] Parse device descriptor: class, subclass, protocol, VID:PID
- [ ] Parse configuration descriptor: find MSC interface (class 0x08) and HID interface (class 0x03)
- [ ] Configure Endpoint for bulk-IN/OUT (MSC) and interrupt-IN (HID)
- [ ] Log: `xhci: USB device VID:PID class=08 (Mass Storage)` or `class=03 (HID)`
- [ ] Commit: `"drivers: USB device enumeration — MSC and HID interfaces detected"`

**Test checkpoint:** Serial shows detected USB devices with class info. POST code 0xD701. Test on: QEMU `run-usb`, bare metal.

## 3. USB MSC BOT (Bulk-Only Transport) Driver
Implement the SCSI-over-USB transport layer: CBW/CSW framing, INQUIRY, READ CAPACITY, READ(10), WRITE(10).

**Files:** `src/kernel/drivers/usb_msc.c` (new), `include/kernel/drivers/usb_msc.h` (new)

- [ ] CBW (Command Block Wrapper) and CSW (Command Status Wrapper) structures
- [ ] `usb_msc_inquiry()` — identify device type and name
- [ ] `usb_msc_read_capacity()` — get sector count and sector size
- [ ] `usb_msc_read_sectors(lba, count, buf)` — READ(10) via bulk-OUT CBW + bulk-IN data + bulk-IN CSW
- [ ] `usb_msc_write_sectors(lba, count, buf)` — WRITE(10) via bulk-OUT CBW + bulk-OUT data + bulk-IN CSW
- [ ] Error handling: CSW status check, bulk reset recovery on stall
- [ ] Commit: `"drivers: USB MSC BOT — SCSI READ/WRITE over bulk endpoints"`

**Test checkpoint:** `usb_msc_read_capacity()` returns correct sector count. Read sector 0 matches expected MBR/GPT. POST code 0xD702. Test on: QEMU `run-usb`.

## 4. Block Device Registration and VFS Integration
Register USB MSC devices as block devices so VFS can mount filesystems from USB drives.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [ ] `usb_msc_register_blkdev()` — register each MSC LUN as a block device via `blkdev_register()`
- [ ] Wire into `boot_phase2()`: after xHCI init, enumerate USB devices, register MSC block devices
- [ ] Partition scan + filesystem mount works on USB drives (GPT/MBR + FAT32/IXFS)
- [ ] Commit: `"drivers: USB MSC block device registration — USB drives mountable as C:\"`

**Test checkpoint:** `bash scripts/build.sh run-usb` — USB drive visible, partition scanned, filesystem mounted. Bare metal: boot from USB, C:\ accessible. POST code 0xD703.

## 5. xHCI Interrupt Endpoint Setup for HID
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

## 6. USB HID Boot-Protocol Keyboard Driver
Parse 8-byte boot-protocol keyboard reports and inject key events into the input subsystem.

**Files:** `src/kernel/drivers/usb_hid_kbd.c` (new)

- [ ] Parse boot keyboard report: byte 0 = modifiers, bytes 2-7 = keycodes
- [ ] Convert USB HID usage codes to PS/2 scancodes (lookup table, ~104 entries)
- [ ] Inject into `keyboard_inject_scancode()` — same path as PS/2
- [ ] Handle key-up: compare current vs previous report, detect released keys
- [ ] Commit: `"drivers: USB HID boot-protocol keyboard — key events via interrupt-IN"`

**Test checkpoint:** USB keyboard: type characters, see them in terminal. POST code 0xD705. Test on: QEMU `run-usb`, bare metal.

## 7. USB HID Boot-Protocol Mouse Driver
Parse 3-byte boot-protocol mouse reports and inject mouse events.

**Files:** `src/kernel/drivers/usb_hid_mouse.c` (new)

- [ ] Parse boot mouse report: byte 0 = buttons, byte 1 = X delta, byte 2 = Y delta
- [ ] Inject into mouse subsystem via `mouse_inject_state(x, y, buttons)`
- [ ] Commit: `"drivers: USB HID boot-protocol mouse — movement + buttons"`

**Test checkpoint:** USB mouse: cursor moves on screen. POST code 0xD706. Test on: QEMU `run-usb`, bare metal.

## 8. Input Source Priority and Coexistence
Ensure PS/2 and USB input sources coexist without conflict.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`

- [ ] Both PS/2 and USB keyboard active simultaneously (both inject scancodes)
- [ ] Both PS/2 and USB mouse active simultaneously
- [ ] If PS/2 not detected (`acpi_has_8042()` = 0): USB is sole input source
- [ ] Log active input sources at boot
- [ ] Commit: `"drivers: input source coexistence — PS/2 + USB active simultaneously"`

**Test checkpoint:** System with both PS/2 and USB — both work. USB-only system — works. POST code 0xD707. Test on: QEMU `run-usb`, bare metal.

---

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                     |
|----|-------------------------|-----------------------------|------------------------------|-----------------------------------|
| 💎 | xHCI controller         | ✅ usbxhci.sys             | ✅ xhci-hcd                  | ⬜ §1 — partial, needs fix       |
| 💎 | USB MSC                 | ✅ USBSTOR.SYS             | ✅ usb-storage               | ⬜ §3 — BOT transport            |
| 💎 | USB boot drive access   | ✅ Automatic               | ✅ initramfs + usb-storage   | ⬜ §4 — boot-critical path       |
| 💎 | USB HID keyboard        | ✅ hidusb.sys + kbdhid.sys | ✅ usbhid + hid-generic      | ⬜ §6 — boot protocol            |
| 💎 | USB HID mouse           | ✅ hidusb.sys + mouhid.sys | ✅ usbhid + hid-generic      | ⬜ §7 — boot protocol            |
| 💎 | PS/2 + USB coexist      | ✅ Automatic               | ✅ Automatic                 | ⬜ §8 — independent, both active |

## Verification

- [ ] `bash scripts/build.sh run-usb` — USB drive mounted, files readable
- [ ] Bare metal USB boot: C:\ accessible, klog writes to disk
- [ ] USB keyboard: type in terminal on bare metal
- [ ] USB mouse: cursor moves on bare metal
- [ ] PS/2-only system: still works (no regression)
