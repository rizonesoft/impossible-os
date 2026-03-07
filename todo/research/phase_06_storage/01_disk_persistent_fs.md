# 23 — Disk Drivers & Persistent Filesystem

## Overview

Currently IXFS is RAM-only — files disappear on reboot. For persistence we need disk drivers and a real filesystem.

---

## Disk Drivers

| Controller | Type | QEMU flag | Priority |
|-----------|------|-----------|----------|
| **AHCI (SATA)** | HDD/SSD | `-drive file=disk.img,if=none -device ahci -device ide-hd` | P0 |
| **NVMe** | Modern SSD | `-drive file=disk.img,if=none -device nvme` | P1 |
| **IDE** | Legacy | `-drive file=disk.img,if=ide` | Legacy |
| **virtio-blk** | Paravirtual | `-drive file=disk.img,if=virtio` | Easiest |

> **Recommended**: Start with **virtio-blk** (simplest driver, ~200 lines), then AHCI (~500 lines).

## Filesystem Options

| FS | License | Complexity | Read/Write |
|----|---------|-----------|-----------|
| **FAT32** | Open standard | ~1000 lines | R/W easy |
| **ext2** | GPL (but format is open) | ~1500 lines | R/W medium |
| **Custom (IXFS-on-disk)** | N/A | ~800 lines | R/W easy |

> **Recommended**: FAT32 first (Windows compatible, well-documented, USB drives use it), then custom persistent IXFS.

## Partition Tables: GPT (modern) or MBR (legacy)
## OS installer: Format disk, copy system files, install bootloader

## Files: `src/kernel/drivers/virtio_blk.c`, `src/kernel/fs/fat32.c`
## Implementation: 3-6 weeks
