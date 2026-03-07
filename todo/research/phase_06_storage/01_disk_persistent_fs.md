# 23 — Disk Drivers & Persistent Filesystem

## Overview

Currently IXFS is RAM-only — files disappear on reboot. For persistence we need disk drivers and a real filesystem.

---

## Disk Drivers

| Controller | Type | QEMU flag | Status |
|-----------|------|-----------|--------|
| **virtio-blk** | Paravirtual | `-drive file=disk.img,if=none,id=disk0 -device virtio-blk-pci,drive=disk0` | ✅ Implemented |
| **AHCI (SATA)** | HDD/SSD | `-drive file=sata.img,if=none,id=disk1 -device ahci,id=ahci0 -device ide-hd,drive=disk1,bus=ahci0.0` | ✅ Implemented |
| **NVMe** | Modern SSD | `-drive file=disk.img,if=none -device nvme` | Future |
| **IDE** | Legacy | `-drive file=disk.img,if=ide` | Legacy (ATA PIO exists) |

> **VirtIO-blk** is implemented using the modern VirtIO 1.0 MMIO transport (not legacy PIO).
> Uses the shared `virtio.c` infrastructure for PCI capability walking and split virtqueues.
> Tested on QEMU 8.2.2 and 10.2.1, verified on both UEFI and BIOS boot paths.

## Filesystem Options

| FS | License | Complexity | Read/Write |
|----|---------|-----------|-----------|
| **FAT32** | Open standard | ~1000 lines | R/W easy |
| **ext2** | GPL (but format is open) | ~1500 lines | R/W medium |
| **Custom (IXFS-on-disk)** | N/A | ~800 lines | R/W easy |

> **Recommended**: FAT32 first (Windows compatible, well-documented, USB drives use it), then custom persistent IXFS.

## Partition Tables: GPT (modern) or MBR (legacy)
## OS installer: Format disk, copy system files, install bootloader

## Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/virtio_blk.c` | VirtIO block driver (modern 1.0 transport) |
| `src/kernel/drivers/virtio.c` | Shared VirtIO modern PCI transport |
| `src/kernel/drivers/ata.c` | ATA/IDE PIO disk driver |
| `src/kernel/fs/fat32.c` | FAT32 filesystem driver |
| `src/kernel/fs/ixfs.c` | IXFS custom filesystem |

## Implementation: 3-6 weeks (VirtIO-blk done, FAT32/IXFS-on-disk remaining)
