# TODO-07 — xHCI, USB Storage & USB HID (Boot-Critical)

> **Goal:** USB boot that works on 95%+ of hardware with zero delay. The bootloader discovers USB devices via UEFI firmware BEFORE ExitBootServices (like Windows), passes device state to the kernel, and the kernel inherits it instantly. USB keyboards and mice work in boot-protocol mode. Hot-plug via xHCI interrupts after boot.

> [!IMPORTANT]
> **Two-phase approach:** §1-§4 are the working prototype (xHCI re-enumeration after ExitBootServices, Intel port routing, 500ms delay). §5 replaces this with the proper Windows-style handover: bootloader uses UEFI protocols to discover devices, kernel inherits state with zero re-enumeration. §10 adds EHCI/UHCI/OHCI fallback for legacy hardware.

> [!NOTE]
> Working code exists: `xhci.c` (controller init, DCBAA, TRB rings, Intel port routing), `xhci_dev.c` (full 9-step enumeration + MSC identification), `xhci_ring.c` (TRB ring management), `usb_msc.c` (BOT SCSI transport). Verified on QEMU TCG + bare metal i5-11600K.

## Inputs

- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) — xHCI controller driver (partial)
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) — device enumeration (partial)
- [`src/kernel/drivers/xhci_ring.c`](../../src/kernel/drivers/xhci_ring.c) — TRB ring management
- [`src/kernel/drivers/keyboard.c`](../../src/kernel/drivers/keyboard.c) — PS/2 keyboard (injection target for USB HID)
- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) — PS/2 mouse (injection target for USB HID)
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md` — advanced USB features (hub, hot-plug, EHCI, BT, CDC)
- → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §5` — MSI/MSI-X (xHCI uses MSI)
- → XREF: `01-boot-platform/TODO-09-usb-zero-delay-handover.md` — true zero-delay handover (persistent DMA in bootloader)

## Outcome

- USB boot drive mounted as C:\ instantly on kernel start (zero delay via §5 handover)
- USB keyboards and mice work in boot-protocol mode (§7, §8)
- Hot-plug: USB devices connected after boot detected via xHCI interrupts (§5 Phase C)
- Works on Intel, AMD, ASMedia, and EHCI-only hardware (§10)
- PS/2 and USB input coexist — both active if both present (§9)

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI controller bring-up and port scan         | —          |  [x]   |
| 💎  |   2   | USB device enumeration and configuration       | §1         |  [x]   |
| 💎  |   3   | USB MSC BOT (Bulk-Only Transport) driver       | §2         |  [x]   |
| 💎  |   4   | Block device registration and VFS integration  | §3         |  [x]   |
| ⭐  |   5   | Pre-ExitBootServices USB handover (Windows-style) | §1, §4   |  [x]   |
| 💎  |   6   | xHCI interrupt endpoint setup for HID          | §2         |  [ ]   |
| 💎  |   7   | USB HID boot-protocol keyboard driver          | §6         |  [ ]   |
| 💎  |   8   | USB HID boot-protocol mouse driver             | §6         |  [ ]   |
| 💎  |   9   | Input source priority and coexistence          | §7, §8     |  [ ]   |
| ⭐  |  10   | Hardware compatibility (95%+ of systems)       | §1, §5     |  [ ]   |

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

**Test checkpoint:** `usb_msc_read_capacity()` returns correct sector count. Read sector 0 matches expected MBR/GPT. POST code 0xD702. Test on: QEMU `run-usb`, bare metal.

## 4. Block Device Registration and VFS Integration
Register USB MSC devices as block devices so VFS can mount filesystems from USB drives.

**Files:** `src/kernel/main/boot_storage.c`, `src/kernel/drivers/blkdev.c`

- [x] USB MSC adapter in `blkdev_adapters.c` — register each MSC device as "usb0", "usb1", etc.
- [x] `xhci_get_device()`, `xhci_msc_device_count()`, `xhci_msc_device_index()` accessors
- [x] Adapter callbacks: `blkdev_usb_msc_read/write` route to `usb_msc_read/write_sectors`
- [x] Automatic: partition scan + filesystem mount via existing `partition_scan_all()`
- [x] Commit: `"drivers: USB MSC block device registration — USB drives mountable"`

**Test checkpoint:** `bash scripts/build.sh run-usb` — USB drive visible, partition scanned, filesystem mounted. Bare metal: boot from USB, C:\ accessible. POST code 0xD703.

## 5. Pre-ExitBootServices USB Handover (Windows-Style)
Discover USB devices using UEFI firmware's own USB stack BEFORE ExitBootServices, then seamlessly hand over to our kernel xHCI driver. This eliminates the 500ms port routing delay, Intel-specific hacks, and the entire re-enumeration-from-scratch problem.

This is how Windows does it: `winload.efi` loads `usbxhci.sys` + `USBSTOR.SYS` while firmware USB is still active. After ExitBootServices, the drivers take over with full knowledge of what's connected.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`, `src/kernel/drivers/xhci.c`

### Phase A: Bootloader discovers USB devices via UEFI protocols (before ExitBootServices)
- [x] Use `EFI_USB_IO_PROTOCOL` to enumerate all USB devices while firmware is active
- [x] For each USB device: read device descriptor (VID, PID, class, endpoints)
- [x] Identify MSC devices (class 0x08, subclass 0x06, protocol 0x50)
- [x] For MSC devices: read the disk geometry (sector count, sector size) via `EFI_BLOCK_IO_PROTOCOL`
- [x] Record xHCI controller PCI location — not needed here (kernel PCI scan); deferred to `TODO-09 §1`
- [x] Record each USB device: port, speed, slot, endpoints, MSC geometry
- [x] Store all info in `boot_info.usb_devices[]` array passed to kernel
- [x] `POST16(0xB080)` entry, `POST16(0xB081)` exit (bootloader range)

### Phase B: Kernel takes over xHCI controller with BIOS handoff
- [x] Read `boot_info.usb_devices[]` — log pre-enumerated device inventory
- [x] Map xHCI BAR0 via `vmm_map_mmio_uc()` (same as now)
- [x] BIOS/OS handoff via USBLEGSUP (xHCI spec §4.22.1) — happens BEFORE halt/reset:
  - [x] Walk extended capabilities list (HCCPARAMS1 bits 31:16) for cap ID 1
  - [x] Set HC OS Owned Semaphore bit, wait for BIOS Owned Semaphore to clear
  - [x] Timeout after 1s — if BIOS doesn't release, force-clear and proceed
  - [x] Clear USBLEGCTLSTS (legacy SMI enables) after handoff
- [x] Skip Intel 500ms delay when XUSB2PR didn't change (ports already routed — true on 100-series+ without EHCI)
- [x] Register MSC from boot_info geometry — deferred to `TODO-09 §6` (saves ~5ms, not worth intercepting usb_msc_init here)
- [x] `POST16(0xD750)` entry, `POST16(0xD751)` exit
- [x] Result: ~500ms saved on modern Intel (no EHCI = skip XUSB2PR + delay) — verified on i5-11600K

> [!IMPORTANT]
> **Rollback:** If USBLEGSUP handoff fails or controller state is corrupt after takeover, fall back to the current halt/reset/re-enumerate path (§1-§4). The §1-§4 path is proven on bare metal — never remove it until §5 is verified on all platforms.

### Phase C: Interrupt-driven hot-plug (post-boot)
- [x] Register xHCI MSI interrupt handler (inline MSI setup, following AHCI pattern)
- [x] If MSI not available: graceful fallback to event ring polling (no crash)
- [x] ISR reads Event Ring for Port Status Change Events (TRB type 34)
- [x] New device connected after boot → full enumeration (slot enable, address, etc.)
- [ ] Device removed → clean up slot, unregister block device (Disable Slot command deferred)
- [x] `POST16(0xD752)` entry, `POST16(0xD753)` exit
- [ ] Commit: `"drivers: xHCI interrupt-driven hot-plug via MSI"`

**Test checkpoint:** Boot from USB — C:\ mounted within 10ms of kernel start (no 500ms delay). Hot-plug: plug USB drive after boot, device appears within 100ms. POST codes: 0xB080/0xB081 (bootloader), 0xD750/0xD751 (kernel takeover), 0xD752/0xD753 (hot-plug). If crash, check last POST — 0xD750 = USBLEGSUP handoff failed, fall back to §1-§4 path. Test on: bare metal, QEMU `run-usb`. No Intel-specific port routing code needed (firmware already routed ports correctly).

## 6. xHCI Interrupt Endpoint Setup for HID
Configure interrupt-IN endpoints for HID devices so the xHCI controller polls them periodically.

> [!NOTE]
> **Scope overlap with `04-drivers-hardware/TODO-09-usb-stack.md`:** TODO-09 §1-§3 still contain HID interrupt endpoint, HID class driver, and PS/2↔USB fallback sections that duplicate TODO-07 §6-§9. TODO-09 should be updated to XREF these sections instead of re-implementing them.

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

## 10. Hardware Compatibility — 95%+ of Systems
Make USB boot work on 95%+ of real hardware: Intel, AMD, third-party xHCI controllers, EHCI fallback, USB hubs, and BIOS/OS handoff. Currently only Intel with specific port routing is tested.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/ehci.c` (new)

### BIOS/OS Handoff
- [ ] Core USBLEGSUP handoff implemented in §5 Phase B — reuse for all controller types
- [ ] Verify USBLEGSUP works identically on AMD, ASMedia, Renesas, VIA (same xHCI spec §4.22.1)
- [ ] EHCI variant: USBLEGSUP is a PCI capability (not xHCI extended cap) — same semaphore concept, different register offset

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

**Test checkpoint:** POST codes: `POST16(0xD7A0)` entry, `POST16(0xD7A1)` exit. If crash at 0xD7A0: BIOS handoff or vendor-specific init failed — check serial for vendor ID. Test matrix (diagnostic splash shows `USB:XX` prog-if codes):
- `USB:30` Intel (i5-11600K): ✅ verified — §5 handover replaces port routing hack
- `USB:30` AMD: BIOS handoff only, no port routing needed
- `USB:30,20` Intel (xHCI+EHCI): port routing moves EHCI ports to xHCI
- `USB:20` EHCI only: EHCI fallback driver handles enumeration
- `USB:00` UHCI only: log warning, graceful skip (low priority)
- `USB:10` OHCI only: log warning, graceful skip (low priority)
- USB hub: device behind hub enumerated
- QEMU `run-usb`: still works (regression check)

---

## OS Comparison

| ⭐ | Feature              | Win11              | Linux              | Impossible OS          |
|----|----------------------|--------------------|--------------------|------------------------|
| 💎 | xHCI controller      | ✅ usbxhci.sys    | ✅ xhci-hcd        | ✅ §1-§4 done          |
| 💎 | USB MSC              | ✅ USBSTOR.SYS    | ✅ usb-storage      | ✅ §3 BOT done         |
| 💎 | USB boot drive       | ✅ Automatic       | ✅ initramfs        | ✅ §4 bare metal       |
| 💎 | USB HID keyboard     | ✅ kbdhid.sys      | ✅ usbhid           | ⬜ §7 boot protocol    |
| 💎 | USB HID mouse        | ✅ mouhid.sys      | ✅ usbhid           | ⬜ §8 boot protocol    |
| 💎 | PS/2 + USB coexist   | ✅ Automatic       | ✅ Automatic        | ⬜ §9 both active      |
| ⭐ | Pre-boot handover    | ✅ winload.efi     | ❌ Re-enumerates    | ⬜ §5 zero-delay       |
| ⭐ | BIOS/OS handoff      | ✅ Automatic       | ✅ xhci-pci.c       | ✅ §5B USBLEGSUP       |
| ⭐ | EHCI fallback        | ✅ usbehci.sys     | ✅ ehci-hcd         | ⬜ §10 legacy HW       |
| ⭐ | USB hub support      | ✅ usbhub.sys      | ✅ hub.c            | ⬜ §10 recursive       |
| ⭐ | Hot-plug             | ✅ Automatic       | ✅ Automatic        | ✅ §5C MSI interrupt   |
| ⭐ | USB boot timing VPD  | ❌ Not exposed     | ❌ Not exposed      | ⬜ §5 handover latency |

## Verification

- [ ] `bash scripts/build.sh run-usb` — USB drive mounted, files readable
- [ ] Bare metal USB boot: C:\ accessible, klog writes to disk
- [ ] USB keyboard: type in terminal on bare metal
- [ ] USB mouse: cursor moves on bare metal
- [ ] PS/2-only system: still works (no regression)
