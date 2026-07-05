---
schema_version: 1
id: storage-controller-device-drivers
domain: 04-drivers-hardware
status: active
title: "TODO-13 -- Storage Controller & Removable Media Drivers"
---

# TODO-13 -- Storage Controller & Removable Media Drivers

> **Goal:** Complete post-boot storage-controller coverage beyond the boot-critical NVMe/USB path: AHCI/SATA parity, ATA/ATAPI cleanup, VirtIO-blk production polish, SD/eMMC/SDHCI, USB card readers, optical/media devices, multipath identity, health reporting, and safe surprise removal. Filesystem semantics stay in `05-storage-filesystems`; this TODO owns the hardware block-device drivers.
> **Current state:** The tree has AHCI, ATA, NVMe, USB MSC, and a large VirtIO-blk implementation. Boot-critical NVMe and USB ownership moved to `01-boot-platform`. There is no single driver-domain owner for non-boot storage controller parity, removable card readers, optical devices, storage health, or block-device driver diagnostics.

## Inputs

- [`src/kernel/drivers/ahci/`](../../src/kernel/drivers/ahci/)
- [`src/kernel/drivers/ata.c`](../../src/kernel/drivers/ata.c)
- [`src/kernel/drivers/virtio/`](../../src/kernel/drivers/virtio/)
- [`src/kernel/drivers/blkdev.c`](../../src/kernel/drivers/blkdev.c)
- -> XREF: `../01-boot-platform/TODO-16-nvme-storage.md` -- boot-critical NVMe baseline
- -> XREF: `../01-boot-platform/TODO-17-xhci-usb-boot.md` -- boot-critical USB MSC baseline
- -> XREF: `TODO-01-pci-pcie-pnp-resource-manager.md` -- device enumeration, hot-plug, and resources
- -> XREF: `05-storage-filesystems` -- partitions and filesystems consume registered block devices

## Outcome

- Every storage controller registers through a common block-device driver contract.
- AHCI, ATA/ATAPI, VirtIO-blk, SD/eMMC, USB readers, and optical devices have explicit support boundaries.
- Storage health and removal state are visible to Device Manager, Registry, and logs.

## Implementation Order

| Priority  | Order | Deliverable                                         | Depends On      | Status |
| --------- | :---: | --------------------------------------------------- | --------------- | :----: |
| Parity    |   1   | Storage driver capability matrix                    | existing blkdev |  [ ]   |
| Parity    |   2   | AHCI/SATA parity completion                         | §1              |  [ ]   |
| Parity    |   3   | ATA/ATAPI and optical media path                    | §1              |  [ ]   |
| Parity    |   4   | VirtIO-blk production integration                   | §1, TODO-01     |  [ ]   |
| Parity    |   5   | SDHCI/eMMC/SD card driver                           | TODO-01         |  [ ]   |
| Parity    |   6   | USB card-reader and multi-LUN policy                | TODO-10         |  [ ]   |
| Parity    |   7   | Storage identity, health, and SMART/NVMe log bridge | §2-§6           |  [ ]   |
| Parity    |   8   | Surprise removal and media-change events            | §2-§6, TODO-01  |  [ ]   |
| Exclusive |   9   | Unified storage driver diagnostics report           | §7              |  [ ]   |
| Parity    |  10   | VM and bare-metal storage matrix                    | §1-§9           |  [ ]   |

## 1. Storage Driver Capability Matrix

- [ ] Document which driver owns AHCI, ATA, ATAPI, NVMe advanced, VirtIO-blk, SDHCI/eMMC, USB MSC, and card readers.
- [ ] Define common `blkdev_driver_caps_t` flags.
- [ ] Commit: `"drivers/storage: capability matrix"`

## 2. AHCI/SATA Parity Completion

- [ ] Audit NCQ, hot-plug, port reset, error recovery, FIS receive, and MSI mode against existing AHCI files.
- [ ] Add power-management callbacks for link state and suspend/resume.
- [ ] Commit: `"drivers/ahci: complete SATA parity path"`

## 3. ATA/ATAPI and Optical Media

- [ ] Support ATAPI inquiry/read for CD/DVD ISO media.
- [ ] Surface media-change events for optical drives.
- [ ] Commit: `"drivers: ATA and ATAPI media support"`

## 4. VirtIO-blk Production Integration

- [ ] Register VirtIO-blk through the central PnP driver model.
- [ ] Expose feature negotiation, queue count, flush/discard/zoned support, and health state.
- [ ] Gate VirtIO-blk registry exposure + tuning reads (`blk_init.c` Reg* calls) behind a post-`registry_init()` `SUBSYS_REGISTRY` hook -- they run in Phase 2 before the registry exists, losing HKLM exposure (consumer: `02-kernel-core/TODO-01 §4`).
- [ ] Commit: `"drivers/virtio-blk: PnP integration"`

## 5. SDHCI/eMMC/SD Card Driver

- [ ] Detect PCI SDHCI controllers and ACPI-enumerated eMMC devices.
- [ ] Implement command, data, ADMA2, voltage, card detect, and write-protect handling.
- [ ] Commit: `"drivers: SDHCI and eMMC block driver"`

## 6. USB Card Readers and Multi-LUN

- [ ] Ensure USB multi-LUN devices expose each card slot as a separate block device.
- [ ] Add media-change polling or interrupt-driven update where available.
- [ ] Commit: `"drivers: USB card reader multi-LUN policy"`

## 7. Storage Identity and Health

- [ ] Normalize serial/model/firmware/capacity across AHCI, NVMe, VirtIO, USB, SD, and ATAPI.
- [ ] Add SMART/NVMe health log bridge where hardware supports it.
- [ ] Commit: `"drivers/storage: identity and health API"`

## 8. Surprise Removal and Media Change

- [ ] Mark devices removed before dispatching new I/O.
- [ ] Unblock pending I/O with deterministic errors.
- [ ] Notify VFS and Device Manager.
- [ ] Commit: `"drivers/storage: surprise removal handling"`

## 9. Diagnostics Report

- [ ] Add `storage-devices` shell output and BlackBox storage inventory.
- [ ] Include queue mode, interrupt mode, health, media, and last error.
- [ ] Commit: `"drivers/storage: diagnostics report"`

## 10. Tests

- [ ] QEMU matrix: AHCI disk, IDE CD-ROM, VirtIO-blk, USB card reader fixture, SDHCI where supported.
- [ ] Bare-metal: SATA SSD, NVMe SSD, SD reader, USB card reader.
- [ ] Commit: `"test: storage controller driver matrix"`

## OS Comparison

| Priority  | Feature               | Windows              | Linux          | Impossible OS |
| --------- | --------------------- | -------------------- | -------------- | ------------- |
| Parity    | AHCI/SATA             | storahci             | libata/ahci    | TODO-13 §2    |
| Parity    | SD/eMMC               | sdstor               | sdhci/mmc      | TODO-13 §5    |
| Parity    | Storage health        | Storage Spaces/SMART | smartctl/sysfs | TODO-13 §7    |
| Exclusive | Unified driver report | scattered tools      | sysfs/dmesg    | TODO-13 §9    |

