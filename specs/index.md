# Specs

External reference specifications for hardware, firmware, storage, and hypervisor formats.

## Storage

### Controllers

| Document                                                          | Topics                                          |
| ----------------------------------------------------------------- | ----------------------------------------------- |
| [AHCI 1.3.1](storage/controllers/ahci-1.3.1.md)                  | AHCI HBA registers, command list, FIS, PRD      |
| [ATAPI/SCSI/MMC](storage/controllers/atapi-scsi-mmc.md)          | ATAPI commands, SCSI CDB, MMC optical media     |
| [NVMe 2.0](storage/controllers/nvme-2.0.md)                      | NVMe queues, registers, PRP/SGL, namespaces     |
| [NVMe 2.1](storage/controllers/nvme-2.1.md)                      | NVMe queues, doorbells, PRP, MSI-X, SMART       |
| [VirtIO 1.2](storage/controllers/virtio-1.2.md)                  | VirtIO queues, device negotiation, MMIO/PCI     |

### Partitioning

| Document                                              | Topics                                          |
| ----------------------------------------------------- | ----------------------------------------------- |
| [GPT](storage/partitioning/gpt.md)                    | GPT header, partition entries, protective MBR   |
| [MBR](storage/partitioning/mbr.md)                    | MBR boot record, partition table, CHS/LBA       |

### Filesystems

| Document                                                  | Topics                                             |
| --------------------------------------------------------- | -------------------------------------------------- |
| [APFS](storage/filesystems/apfs.md)                       | APFS container, volume, B-tree, CoW, encryption    |
| [Btrfs](storage/filesystems/btrfs.md)                     | Btrfs on-disk format, CoW, B-tree, checksums       |
| [exFAT 1.0](storage/filesystems/exfat-1.0.md)            | exFAT BPB, allocation bitmap, directory entries    |
| [ext4](storage/filesystems/ext4.md)                       | ext4 superblock, extents, journal, groups          |
| [FAT32](storage/filesystems/fat32.md)                     | FAT32 BPB, cluster chains, LFN, FSInfo            |
| [HFS+](storage/filesystems/hfsplus.md)                    | HFS+ volume header, B-trees, catalog, journal      |
| [NTFS 3.1](storage/filesystems/ntfs-3.1.md)              | NTFS MFT, attributes, $INDEX, journal             |

## Hardware

### CPU

| Document                                                          | Topics                                     |
| ----------------------------------------------------------------- | ------------------------------------------ |
| [AMD64 Zen](hardware/cpu/amd64-zen-systems-arch.md)              | AMD64 ISA, Zen microarchitecture           |
| [Intel SDM x86-64](hardware/cpu/intel-sdm-x86-64.md)             | Intel SDM, x86-64 Long Mode, paging, GDT   |

### Bus

| Document                                              | Topics                                |
| ----------------------------------------------------- | ------------------------------------- |
| [PCI 3.0](hardware/bus/pci-3.0.md)                    | PCI config space, BAR, MSI, bus enum  |

### Firmware

| Document                                                  | Topics                                          |
| --------------------------------------------------------- | ----------------------------------------------- |
| [ACPI 6.5](hardware/firmware/acpi-6.5.md)                | ACPI tables, DSDT, SSDT, AML, power management |
| [UEFI 2.10](hardware/firmware/uefi-2.10.md)              | UEFI boot services, GOP, memory map, protocols  |

### Interrupts

| Document                                                              | Topics                                  |
| --------------------------------------------------------------------- | --------------------------------------- |
| [APIC Architecture](hardware/interrupts/apic-architecture.md)        | Local APIC, I/O APIC, interrupt routing |

## Hypervisors

### Hyper-V

| Document                                                                          | Topics                                    |
| --------------------------------------------------------------------------------- | ----------------------------------------- |
| [Advanced Enlightenments](hypervisors/hyper-v/advanced-enlightenments.md)         | CPUID leaves, MSRs, synthetic MSRs        |
| [Guest Additions](hypervisors/hyper-v/guest-additions-integration.md)             | KVP, VSS, heartbeat, time sync            |
| [HID Synthetic Input](hypervisors/hyper-v/hid-synthetic-input.md)                 | Synthetic keyboard/mouse, VMBus HID       |
| [Page Table MMIO](hypervisors/hyper-v/page-table-mmio-safety.md)                  | Hypercall page, MMIO safety, EPT          |
| [Power Management](hypervisors/hyper-v/power-management.md)                       | Synthetic shutdown, sleep, hibernate      |
| [StorVSC](hypervisors/hyper-v/storvsc-synthetic-scsi.md)                          | Synthetic SCSI, StorVSC protocol          |
| [Synthetic Network](hypervisors/hyper-v/synthetic-network-driver.md)              | NetVSC, RNDIS, VMBus network              |
| [Synthetic Timer](hypervisors/hyper-v/synthetic-timer.md)                         | Reference TSC, synthetic timers           |
| [Synthetic Video](hypervisors/hyper-v/synthetic-video-driver.md)                  | Synthetic framebuffer, EDID              |
| [VMBus Core](hypervisors/hyper-v/vmbus-core-protocol.md)                          | VMBus protocol, channel offer, ring buffer |
