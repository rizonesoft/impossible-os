# Architecture

> OS internals — kernel subsystems, drivers, filesystem, boot chain, desktop, and networking.

## Documents

*No documents yet — architecture docs will be generated as kernel/driver/filesystem TODOs are completed.*

<!-- Template for future entries:
| Document                              | Owned Topics                                                    |
| ------------------------------------- | --------------------------------------------------------------- |
| [AHCI Driver](drivers/ahci.md)       | AHCI HBA, command lists, FIS, port multiplier, NCQ              |
| [FAT32](filesystem/fat32.md)         | FAT32 BPB, cluster chains, LFN, directory entries, FSInfo       |
| [UEFI Bootloader](boot/uefi.md)     | UEFI boot, GOP, EFI stub, PE/COFF, ExitBootServices            |
-->

## Subdirectories

| Directory      | Scope                                    |
| -------------- | ---------------------------------------- |
| `boot/`        | Boot chain, UEFI bootloader              |
| `drivers/`     | Hardware drivers (AHCI, VirtIO, NIC)     |
| `desktop/`     | Window manager, compositor, shell        |
| `filesystem/`  | VFS, FAT32, IXFS, ext4                   |
| `kernel/`      | PMM, VMM, scheduler, heap, syscalls      |
| `net/`         | Ethernet, ARP, IPv4, ICMP, UDP, DHCP     |
