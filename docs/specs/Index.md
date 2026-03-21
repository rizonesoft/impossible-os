# Specs

External reference specifications for hardware, firmware, and filesystem formats used by Impossible OS.

## Bus

| Document                   | Topics                                |
| -------------------------- | ------------------------------------- |
| [PCI 3.0](bus/pci-3.0.md) | PCI config space, BAR, MSI, bus enum  |

## CPU

| Document                                     | Topics                                     |
| -------------------------------------------- | ------------------------------------------ |
| [AMD64 Zen](cpu/amd64-zen-systems-arch.md)   | AMD64 ISA, Zen microarchitecture           |
| [Intel SDM x86-64](cpu/intel-sdm-x86-64.md)  | Intel SDM, x86-64 Long Mode, paging, GDT  |

## Drivers

| Document                                          | Topics                                |
| ------------------------------------------------- | ------------------------------------- |
| [APIC Architecture](drivers/apic-architecture.md) | Local APIC, I/O APIC, interrupt routing |

## Filesystem

| Document                               | Topics                                             |
| -------------------------------------- | -------------------------------------------------- |
| [Btrfs](filesystem/btrfs.md)           | Btrfs on-disk format, CoW, B-tree, checksums       |
| [exFAT 1.0](filesystem/exfat-1.0.md)  | exFAT BPB, allocation bitmap, directory entries    |
| [ext4](filesystem/ext4.md)             | ext4 superblock, extents, journal, groups          |
| [FAT32](filesystem/fat32.md)           | FAT32 BPB, cluster chains, LFN, FSInfo            |
| [NTFS 3.1](filesystem/ntfs-3.1.md)    | NTFS MFT, attributes, $INDEX, journal             |

## Firmware

| Document                              | Topics                                          |
| ------------------------------------- | ----------------------------------------------- |
| [ACPI 6.5](firmware/acpi-6.5.md)     | ACPI tables, DSDT, SSDT, AML, power management |
| [UEFI 2.10](firmware/uefi-2.10.md)   | UEFI boot services, GOP, memory map, protocols  |

## Hyper-V

| Document                                                      | Topics                                    |
| ------------------------------------------------------------- | ----------------------------------------- |
| [Advanced Enlightenments](hyper-v/advanced-enlightenments.md) | CPUID leaves, MSRs, synthetic MSRs        |
| [Guest Additions](hyper-v/guest-additions-integration.md)     | KVP, VSS, heartbeat, time sync            |
| [HID Synthetic Input](hyper-v/hid-synthetic-input.md)         | Synthetic keyboard/mouse, VMBus HID       |
| [Page Table MMIO](hyper-v/page-table-mmio-safety.md)          | Hypercall page, MMIO safety, EPT          |
| [Power Management](hyper-v/power-management.md)               | Synthetic shutdown, sleep, hibernate      |
| [StorVSC](hyper-v/storvsc-synthetic-scsi.md)                  | Synthetic SCSI, StorVSC protocol          |
| [Synthetic Network](hyper-v/synthetic-network-driver.md)      | NetVSC, RNDIS, VMBus network              |
| [Synthetic Timer](hyper-v/synthetic-timer.md)                 | Reference TSC, synthetic timers           |
| [Synthetic Video](hyper-v/synthetic-video-driver.md)          | Synthetic framebuffer, EDID              |
| [VMBus Core](hyper-v/vmbus-core-protocol.md)                  | VMBus protocol, channel offer, ring buffer |

## Storage

| Document                                    | Topics                                          |
| ------------------------------------------- | ----------------------------------------------- |
| [AHCI 1.3.1](storage/ahci-1.3.1.md)       | AHCI HBA registers, command list, FIS, PRD      |
| [ATAPI/SCSI/MMC](storage/atapi-scsi-mmc.md) | ATAPI commands, SCSI CDB, MMC optical media    |
| [GPT](storage/gpt.md)                      | GPT header, partition entries, protective MBR    |
| [MBR](storage/mbr.md)                      | MBR boot record, partition table, CHS/LBA       |
| [VirtIO 1.2](storage/virtio-1.2.md)        | VirtIO queues, device negotiation, MMIO/PCI     |
