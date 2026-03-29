# TODO-07 — xHCI & USB Mass Storage (Boot-Critical)

> **Goal:** Enable USB storage access after ExitBootServices so the OS can mount C:\ from a USB boot drive on bare metal. Without this, bare metal boots from USB show desktop but cannot access any filesystem.

> [!IMPORTANT]
> This TODO extracts the **boot-critical** xHCI and USB MSC sections from `04-drivers-hardware/TODO-09-usb-stack.md`. Advanced USB features (hub driver, hot-plug, EHCI fallback, Bluetooth, CDC) remain in TODO-09. After this TODO, USB boot drives are accessible as block devices.

> [!NOTE]
> Partial xHCI implementation exists: `xhci.c` (controller init, DCBAA, TRB rings, port scan), `xhci_dev.c` (slot enable, Address Device, GET_DESCRIPTOR, SET_CONFIGURATION), `xhci_ring.c` (TRB ring management). **Do NOT rewrite — complete it.**

## Inputs

- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) — xHCI controller driver (partial)
- [`src/kernel/drivers/xhci_dev.c`](../../src/kernel/drivers/xhci_dev.c) — device enumeration (partial)
- [`src/kernel/drivers/xhci_ring.c`](../../src/kernel/drivers/xhci_ring.c) — TRB ring management
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md §3` — USB MSC BOT completion (this TODO implements the boot-critical subset)
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md §4` — hot-plug (deferred, not boot-critical)
- → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §5` — MSI/MSI-X (xHCI uses MSI)

## Outcome

- xHCI controller discovered on PCI, initialized, and operational
- USB mass storage devices enumerated and registered as block devices via `blkdev_register()`
- Boot from USB drive: kernel loads via UEFI, then OS accesses filesystem on same USB drive via xHCI+MSC
- `scripts/build.sh run-usb` test passes end-to-end

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | xHCI controller bring-up and port scan         | —          |  [ ]   |
| 💎  |   2   | USB device enumeration and configuration       | §1         |  [ ]   |
| 💎  |   3   | USB MSC BOT (Bulk-Only Transport) driver       | §2         |  [ ]   |
| 💎  |   4   | Block device registration and VFS integration  | §3         |  [ ]   |

---

## 1. xHCI Controller Bring-Up and Port Scan
Verify and fix the existing xHCI controller initialization. Currently logs "No xHCI controllers found" on some platforms.

**Files:** `src/kernel/drivers/xhci.c`, `src/kernel/drivers/pci.c`

- [ ] Verify PCI discovery finds xHCI (class 0x0C, subclass 0x03, prog-if 0x30)
- [ ] Controller halt, reset, DCBAA allocation, command ring, event ring — audit existing code
- [ ] Port scan: detect attached USB devices, log port status
- [ ] Map xHCI BAR0 via `vmm_map_mmio_uc()` (MMIO registers need UC mapping)
- [ ] Commit: `"drivers: xHCI controller bring-up verified on QEMU + bare metal"`

**Test checkpoint:** Serial shows `xhci: N ports, M devices attached`. POST code 0xD700. Test on: QEMU `run-usb`, bare metal.

## 2. USB Device Enumeration and Configuration
Complete the device enumeration path: slot enable → Address Device → GET_DESCRIPTOR → SET_CONFIGURATION.

**Files:** `src/kernel/drivers/xhci_dev.c`

- [ ] Audit existing `xhci_dev_enumerate()` — fix any incomplete paths
- [ ] Parse device descriptor: class, subclass, protocol, VID:PID
- [ ] Parse configuration descriptor: find MSC interface (class 0x08, subclass 0x06, protocol 0x50 = BOT)
- [ ] Configure Endpoint for bulk-IN and bulk-OUT endpoints
- [ ] Log: `xhci: USB device VID:PID class=08 subclass=06 (Mass Storage BOT)`
- [ ] Commit: `"drivers: USB device enumeration — MSC BOT interface detected"`

**Test checkpoint:** Serial shows MSC device detected with bulk endpoints. POST code 0xD701. Test on: QEMU `run-usb`.

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

**Test checkpoint:** `bash scripts/build.sh run-usb` — USB drive visible as block device, partition scanned, filesystem mounted. Bare metal: boot from USB, C:\ accessible. POST code 0xD703.

---

## OS Comparison

| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                    |
|----|-------------------------|-----------------------------|------------------------------|----------------------------------|
| 💎 | xHCI controller         | ✅ usbxhci.sys              | ✅ xhci-hcd                  | ⬜ §1 — partial, needs fix       |
| 💎 | USB MSC                 | ✅ USBSTOR.SYS              | ✅ usb-storage                | ⬜ §3 — BOT transport            |
| 💎 | USB boot drive access   | ✅ Automatic                 | ✅ initramfs + usb-storage    | ⬜ §4 — boot-critical path       |

## Verification

- [ ] `bash scripts/build.sh run-usb` — USB drive mounted, files readable
- [ ] Bare metal USB boot: C:\ accessible, klog writes to disk
- [ ] QEMU WHPX + TCG: USB test passes
